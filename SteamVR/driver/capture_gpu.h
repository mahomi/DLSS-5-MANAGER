#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <algorithm>
#include <vector>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
namespace framecapture {
using Microsoft::WRL::ComPtr;
struct Image {UINT width=0,height=0;DXGI_FORMAT format=DXGI_FORMAT_UNKNOWN;std::vector<unsigned char> rgba;};
struct Result {
    std::array<Image,2> images;
    HRESULT open=S_OK,acquire=E_FAIL,release=E_FAIL,gpu=S_FALSE,map=S_FALSE;
    unsigned long long elapsedMicroseconds=0;
    bool success=false;
};
inline HRESULT createDevice(ComPtr<ID3D11Device>& device,ComPtr<ID3D11DeviceContext>& context){
    ComPtr<IDXGIFactory1> factory;
    HRESULT hr=CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(factory.GetAddressOf()));
    if(FAILED(hr))return hr;
    for(UINT index=0;index<16;++index){
        ComPtr<IDXGIAdapter1> adapter;if(factory->EnumAdapters1(index,&adapter)==DXGI_ERROR_NOT_FOUND)break;
        if(!adapter)continue;
        DXGI_ADAPTER_DESC1 desc{};adapter->GetDesc1(&desc);
        if(desc.VendorId!=0x10de || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))continue;
        return D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,
            D3D11_SDK_VERSION,&device,nullptr,&context);
    }
    return DXGI_ERROR_NOT_FOUND;
}
inline Result capture(ID3D11Device* device,ID3D11DeviceContext* context,uint64_t sync,
                       const std::array<uint64_t,2>& handles){
    Result result;
    if(!device || !context || !sync || !handles[0] || !handles[1])return result;
    LARGE_INTEGER start{},frequency{};QueryPerformanceCounter(&start);QueryPerformanceFrequency(&frequency);
    ComPtr<ID3D11Texture2D> syncTexture;ComPtr<IDXGIKeyedMutex> syncMutex;
    result.open=device->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(sync)),
        __uuidof(ID3D11Texture2D),reinterpret_cast<void**>(syncTexture.GetAddressOf()));
    if(FAILED(result.open))return result;
    result.open=syncTexture.As(&syncMutex);if(FAILED(result.open))return result;
    std::array<ComPtr<ID3D11Texture2D>,2> sources,staging;
    std::array<D3D11_BOX,2> boxes{};
    for(size_t eye=0;eye<2;++eye){
        result.open=device->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handles[eye])),
            __uuidof(ID3D11Texture2D),reinterpret_cast<void**>(sources[eye].GetAddressOf()));
        if(FAILED(result.open))return result;
        D3D11_TEXTURE2D_DESC desc{};sources[eye]->GetDesc(&desc);
        if(desc.SampleDesc.Count!=1 || desc.ArraySize!=1 || !desc.Width || !desc.Height ||
            (desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
             && desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)){
            result.open=E_NOTIMPL;return result;
        }
        // Only a centered <=320px crop. Original shared textures are copy sources only.
        auto& image=result.images[eye];image.width=(std::min)(desc.Width,UINT(320));
        image.height=(std::min)(desc.Height,UINT(320));image.format=desc.Format;
        image.rgba.resize(size_t(image.width)*image.height*4);
        const UINT x=(desc.Width-image.width)/2,y=(desc.Height-image.height)/2;
        boxes[eye]={x,y,0,x+image.width,y+image.height,1};
        desc.Width=image.width;desc.Height=image.height;desc.MipLevels=1;
        desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        desc.BindFlags=desc.MiscFlags=0;
        result.open=device->CreateTexture2D(&desc,nullptr,&staging[eye]);if(FAILED(result.open))return result;
    }
    ComPtr<ID3D11Query> query;D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT,0};
    result.open=device->CreateQuery(&queryDesc,&query);if(FAILED(result.open))return result;
    // OpenVR's DirectMode Present contract uses key 0 on the sync texture.
    // WAIT_TIMEOUT and WAIT_ABANDONED are not S_OK, despite SUCCEEDED returning true.
    result.acquire=syncMutex->AcquireSync(0,0);
    if(result.acquire!=S_OK)return result;
    for(size_t eye=0;eye<2;++eye)
        context->CopySubresourceRegion(staging[eye].Get(),0,0,0,0,sources[eye].Get(),0,&boxes[eye]);
    context->End(query.Get());context->Flush();
    const auto deadline=GetTickCount64()+200;
    do{
        BOOL complete=FALSE;result.gpu=context->GetData(query.Get(),&complete,sizeof(complete),0);
        if(FAILED(result.gpu) || (result.gpu==S_OK && complete))break;
        result.gpu=S_FALSE;SwitchToThread();
    }while(GetTickCount64()<deadline);
    // Preserve key 0 for the existing driver's Present. No disk IO while holding it.
    result.release=syncMutex->ReleaseSync(0);
    if(result.gpu!=S_OK || result.release!=S_OK)return result;
    for(size_t eye=0;eye<2;++eye){
        D3D11_MAPPED_SUBRESOURCE mapped{};
        result.map=context->Map(staging[eye].Get(),0,D3D11_MAP_READ,D3D11_MAP_FLAG_DO_NOT_WAIT,&mapped);
        if(FAILED(result.map))return result;
        auto& image=result.images[eye];
        const bool bgra=image.format==DXGI_FORMAT_B8G8R8A8_UNORM || image.format==DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        for(UINT y=0;y<image.height;++y){
            const auto source=static_cast<const unsigned char*>(mapped.pData)+size_t(y)*mapped.RowPitch;
            auto dest=image.rgba.data()+size_t(y)*image.width*4;
            for(UINT x=0;x<image.width;++x){
                dest[x*4]=source[x*4+(bgra?2:0)];dest[x*4+1]=source[x*4+1];
                dest[x*4+2]=source[x*4+(bgra?0:2)];dest[x*4+3]=source[x*4+3];
            }
        }
        context->Unmap(staging[eye].Get(),0);
    }
    LARGE_INTEGER end{};QueryPerformanceCounter(&end);
    result.elapsedMicroseconds=static_cast<unsigned long long>((end.QuadPart-start.QuadPart)*1000000/frequency.QuadPart);
    result.success=true;return result;
}
inline bool saveBmp(const std::filesystem::path& path,const Image& image){
    if(image.rgba.size()!=size_t(image.width)*image.height*4 || !image.width || !image.height)return false;
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};
    file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);
    file.bfSize=file.bfOffBits+static_cast<DWORD>(image.rgba.size());
    info.biSize=sizeof(info);info.biWidth=static_cast<LONG>(image.width);info.biHeight=-static_cast<LONG>(image.height);
    info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
    std::ofstream stream(path,std::ios::binary);stream.write(reinterpret_cast<const char*>(&file),sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info),sizeof(info));
    for(size_t pixel=0;pixel<image.rgba.size();pixel+=4){
        const unsigned char bgra[]{image.rgba[pixel+2],image.rgba[pixel+1],image.rgba[pixel],255};
        stream.write(reinterpret_cast<const char*>(bgra),4);
    }
    return stream.good();
}
}
