#define NOMINMAX
#define wmain passthrough_test_entry
#include "observer_self_test.cpp"
#undef wmain
#include "capture_gpu.h"
using framecapture::ComPtr;
struct GpuDirect final:vr::IVRDriverDirectModeComponent{
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::vector<ComPtr<ID3D11Texture2D>> textures;
    std::vector<std::array<SubmitLayerPerEye_t,2>> received;
    std::vector<char> events;
    unsigned creates=0,next=0,presents=0;
    uint32_t ownerPid=0;
    uint64_t lastSync=0;
    uint64_t inputSyncForChecking=0;
    unsigned splitChecks=0;
    bool splitHealthy=true;
    bool lifetimeEnded=false;unsigned postLifetimeCalls=0,destroys=0;
    void DestroySwapTextureSet(vr::SharedTextureHandle_t) override{++destroys;if(lifetimeEnded)++postLifetimeCalls;}
    uint64_t make(UINT width,UINT height,UINT format,UINT misc,unsigned color){
        D3D11_TEXTURE2D_DESC desc{};desc.Width=width;desc.Height=height;desc.Format=static_cast<DXGI_FORMAT>(format);
        desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;
        desc.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;desc.MiscFlags=misc;
        std::vector<unsigned> pixels(size_t(width)*height,color);D3D11_SUBRESOURCE_DATA initial{pixels.data(),width*4,0};
        ComPtr<ID3D11Texture2D> texture;HRESULT hr=device->CreateTexture2D(&desc,&initial,&texture);if(FAILED(hr))return 0;
        ComPtr<IDXGIResource> resource;HANDLE handle=nullptr;
        if(FAILED(texture.As(&resource)) || FAILED(resource->GetSharedHandle(&handle)))return 0;
        textures.push_back(texture);return reinterpret_cast<uintptr_t>(handle);
    }
    void CreateSwapTextureSet(uint32_t pid,const SwapTextureSetDesc_t* desc,SwapTextureSet_t* out) override{
        ++creates;ownerPid=pid;*out={};
        for(auto& handle:out->rSharedTextureHandles)handle=make(desc->nWidth,desc->nHeight,desc->nFormat,D3D11_RESOURCE_MISC_SHARED,0);
        context->Flush();
    }
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t[2],uint32_t(*indices)[2]) override{
        events.push_back('N');
        (*indices)[0]=(*indices)[1]=next++%3;
    }
    void SubmitLayer(const SubmitLayerPerEye_t(&eyes)[2]) override{
        events.push_back('S');
        received.push_back({eyes[0],eyes[1]});
    }
    void Present(vr::SharedTextureHandle_t sync) override{
        events.push_back('P');++presents;lastSync=sync;
        if(inputSyncForChecking && sync!=inputSyncForChecking){
            ++splitChecks;
            ComPtr<ID3D11Texture2D> input,output;ComPtr<IDXGIKeyedMutex> inputMutex,outputMutex;
            auto hr=device->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(inputSyncForChecking)),
                __uuidof(ID3D11Texture2D),reinterpret_cast<void**>(input.GetAddressOf()));
            if(FAILED(hr) || FAILED(input.As(&inputMutex))){splitHealthy=false;return;}
            const auto inputStatus=inputMutex->AcquireSync(0,0);
            if(inputStatus!=WAIT_TIMEOUT){splitHealthy=false;if(inputStatus==S_OK)inputMutex->ReleaseSync(0);}
            hr=device->OpenSharedResource(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(sync)),
                __uuidof(ID3D11Texture2D),reinterpret_cast<void**>(output.GetAddressOf()));
            if(FAILED(hr) || FAILED(output.As(&outputMutex))){splitHealthy=false;return;}
            const auto outputStatus=outputMutex->AcquireSync(0,0);
            if(outputStatus!=S_OK){splitHealthy=false;return;}
            if(outputMutex->ReleaseSync(0)!=S_OK)splitHealthy=false;
        }
    }
};
struct GpuHmd final:vr::ITrackedDeviceServerDriver{
    GpuDirect direct;
    vr::EVRInitError Activate(uint32_t) override{return vr::VRInitError_None;}
    void Deactivate() override{}
    void EnterStandby() override{}
    void* GetComponent(const char* name) override{
        return name && strcmp(name,vr::IVRDriverDirectModeComponent_Version)==0?&direct:nullptr;
    }
    void DebugRequest(const char*,char*,uint32_t) override{}
    vr::DriverPose_t GetPose() override{return {};}
};
