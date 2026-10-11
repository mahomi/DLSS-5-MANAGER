#pragma once
#include "capture_gpu.h"
namespace framemarker {
struct Cache {
    ID3D11Device* owner=nullptr;
    std::vector<std::pair<uint64_t,framecapture::ComPtr<ID3D11Texture2D>>> textures;
    framecapture::ComPtr<ID3D11Query> query;
    std::array<std::vector<unsigned>,2> patterns;
    std::array<UINT,2> sides{};
    uint64_t opens=0;
    Cache()=default;
    void clear(){textures.clear();query.Reset();patterns={};sides={};owner=nullptr;}
    framecapture::ComPtr<ID3D11Texture2D> get(ID3D11Device* device,uint64_t handle,HRESULT& hr){
        if(owner!=device){clear();owner=device;}
        for(const auto& item:textures)if(item.first==handle){hr=S_OK;return item.second;}
        framecapture::ComPtr<ID3D11Texture2D> texture;
        hr=device->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handle)),
            __uuidof(ID3D11Texture2D),reinterpret_cast<void**>(texture.GetAddressOf()));
        ++opens;
        if(SUCCEEDED(hr) && textures.size()<64){
            if(textures.empty())textures.reserve(64);
            textures.emplace_back(handle,texture);
        }
        return texture;
    }
};
struct InputLease {
    framecapture::ComPtr<IDXGIKeyedMutex> mutex;
    InputLease()=default;
    InputLease(const InputLease&)=delete;
    InputLease& operator=(const InputLease&)=delete;
    InputLease(InputLease&& other) noexcept:mutex(std::move(other.mutex)){}
    HRESULT release(){if(!mutex)return S_FALSE;const auto hr=mutex->ReleaseSync(0);mutex.Reset();return hr;}
    InputLease& operator=(InputLease&& other) noexcept{release();mutex=std::move(other.mutex);return *this;}
    ~InputLease(){release();}
};
struct Result {
    HRESULT open=S_OK,acquire=E_FAIL,release=E_FAIL,gpu=S_FALSE;bool success=false;
    uint64_t elapsedUs=0,resourceUs=0,waitUs=0,opens=0;
    InputLease lease;
};
inline Result process(ID3D11Device* device,ID3D11DeviceContext* context,uint64_t sync,
    const std::array<uint64_t,2>& inputs,const std::array<uint64_t,2>& outputs,Cache* persistent=nullptr,bool drawPattern=true,bool holdInput=false){
    Result result;
    if(!device || !context)return result;
    LARGE_INTEGER start{},frequency{};QueryPerformanceCounter(&start);QueryPerformanceFrequency(&frequency);
    Cache local;
    auto& cache=persistent?*persistent:local;
    const auto opensBefore=cache.opens;
    auto syncTexture=cache.get(device,sync,result.open);
    framecapture::ComPtr<IDXGIKeyedMutex> mutex;
    if(FAILED(result.open))return result;
    result.open=syncTexture.As(&mutex);if(FAILED(result.open))return result;
    std::array<framecapture::ComPtr<ID3D11Texture2D>,2> sources,targets;
    std::array<D3D11_BOX,2> boxes{};
    std::array<UINT,2> sides{};
    for(size_t eye=0;eye<2;++eye){
        if(inputs[eye]==outputs[eye]){result.open=E_INVALIDARG;return result;}
        sources[eye]=cache.get(device,inputs[eye],result.open);
        if(FAILED(result.open))return result;
        targets[eye]=cache.get(device,outputs[eye],result.open);
        if(FAILED(result.open))return result;
        D3D11_TEXTURE2D_DESC input{},output{};sources[eye]->GetDesc(&input);targets[eye]->GetDesc(&output);
        if(input.Width!=output.Width || input.Height!=output.Height || input.Format!=output.Format ||
            input.SampleDesc.Count!=1 || output.SampleDesc.Count!=1 || input.ArraySize!=1 || output.ArraySize!=1 ||
            (input.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && input.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
             input.Format!=DXGI_FORMAT_B8G8R8A8_UNORM && input.Format!=DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)){
            result.open=E_NOTIMPL;return result;
        }
        const UINT side=(std::min)(UINT(256),(std::min)(input.Width,input.Height));sides[eye]=side;
        if(drawPattern && cache.sides[eye]!=side){
        cache.sides[eye]=side;cache.patterns[eye].resize(size_t(side)*side);
        for(UINT y=0;y<side;++y)for(UINT x=0;x<side;++x){
            const bool border=x<8 || y<8 || x+8>=side || y+8>=side;
            const bool green=!border && ((x/32+y/32)%2==0);
            cache.patterns[eye][size_t(y)*side+x]=green?0xff00ff00u:0xff000000u;
        }
        }
        const UINT x=(input.Width-side)/2,y=(input.Height-side)/2;
        boxes[eye]={x,y,0,x+side,y+side,1};
    }
    if(!cache.query){D3D11_QUERY_DESC queryDesc{D3D11_QUERY_EVENT,0};
        result.open=device->CreateQuery(&queryDesc,&cache.query);if(FAILED(result.open))return result;}
    LARGE_INTEGER ready{};QueryPerformanceCounter(&ready);
    result.resourceUs=static_cast<uint64_t>((ready.QuadPart-start.QuadPart)*1000000/frequency.QuadPart);
    result.opens=cache.opens-opensBefore;
    result.acquire=mutex->AcquireSync(0,0);if(result.acquire!=S_OK)return result;
    for(size_t eye=0;eye<2;++eye){
        // Targets are distinct buffers allocated by the original driver for this diagnostic.
        context->CopyResource(targets[eye].Get(),sources[eye].Get());
        if(drawPattern)context->UpdateSubresource(targets[eye].Get(),0,&boxes[eye],cache.patterns[eye].data(),sides[eye]*4,0);
    }
    context->End(cache.query.Get());context->Flush();const auto deadline=GetTickCount64()+200;
    LARGE_INTEGER submitted{};QueryPerformanceCounter(&submitted);
    do{
        BOOL complete=FALSE;result.gpu=context->GetData(cache.query.Get(),&complete,sizeof(complete),0);
        if(FAILED(result.gpu) || (result.gpu==S_OK && complete))break;
        result.gpu=S_FALSE;SwitchToThread();
    }while(GetTickCount64()<deadline);
    LARGE_INTEGER completed{};QueryPerformanceCounter(&completed);
    result.waitUs=static_cast<uint64_t>((completed.QuadPart-submitted.QuadPart)*1000000/frequency.QuadPart);
    if(holdInput && result.gpu==S_OK){
        result.lease.mutex=mutex;result.release=S_FALSE;result.success=true;
    }else{
        result.release=mutex->ReleaseSync(0);result.success=result.gpu==S_OK && result.release==S_OK;
    }
    if(!result.success)cache.query.Reset(); // Do not reuse an outstanding or failed event.
    LARGE_INTEGER end{};QueryPerformanceCounter(&end);
    result.elapsedUs=static_cast<uint64_t>((end.QuadPart-start.QuadPart)*1000000/frequency.QuadPart);
    return result;
}
}
