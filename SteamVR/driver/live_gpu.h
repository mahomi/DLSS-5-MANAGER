#pragma once
#include "marker_gpu.h"
#include "live_control.h"
#include "../../ScreenNative/vr_gpu_protocol.h"
#include <d3d11_4.h>
#include <thread>
#include <chrono>
#include <memory>
namespace vrlive {
using framecapture::ComPtr;
inline uint64_t monotonicUs(){return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
struct Eye {
    ComPtr<ID3D11Texture2D> input,output;ComPtr<ID3D11Fence> inputFence,outputFence;
    HANDLE inputHandle=nullptr,outputHandle=nullptr,inputFenceHandle=nullptr,outputFenceHandle=nullptr,mapping=nullptr,process=nullptr,requestEvent=nullptr,completedEvent=nullptr;
    vrgpu::Shared* shared=nullptr;std::wstring name;uint64_t pending=0;
    std::atomic_uint64_t stopTick{0};
    bool exited()const{return process && WaitForSingleObject(process,0)==WAIT_OBJECT_0;}
    bool retired()const{return !pending || exited() || (shared && vrgpu::sequence(&shared->completed)==LONG64(pending) && outputFence->GetCompletedValue()>=pending);}
    ~Eye(){if(shared)UnmapViewOfFile(shared);for(HANDLE h:{inputHandle,outputHandle,inputFenceHandle,outputFenceHandle,mapping,process,requestEvent,completedEvent})if(h)CloseHandle(h);}
    void stop(){uint64_t unset=0;stopTick.compare_exchange_strong(unset,GetTickCount64());if(shared)InterlockedExchange(&shared->stop,1);if(requestEvent)SetEvent(requestEvent);}
    bool pollStopped(){
        if(!process || exited())return true;
        // Keep control polling/status publication responsive during retirement.
        // This handle belongs to the worker created by this generation.
        if(stopTick && GetTickCount64()-stopTick.load()>=1500)TerminateProcess(process,71);
        return exited();
    }
    bool join(){if(!process)return true;if(WaitForSingleObject(process,1500)==WAIT_OBJECT_0)return true;TerminateProcess(process,71);return WaitForSingleObject(process,1500)==WAIT_OBJECT_0;}
};
struct Generation {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Device5> device5;ComPtr<ID3D11DeviceContext4> context4;
    ComPtr<ID3D11Fence> copyFence;uint64_t copySequence=0;
    std::array<Eye,2> eyes;std::array<D3D11_TEXTURE2D_DESC,2> desc{};
    framemarker::Cache cache;float scale=.25f;uint64_t id=0,sequence=0;std::string error;
    Control lastControl{};bool haveControl=false;
    unsigned configuredPasses=1;
    bool ready()const{return error.empty() && eyes[0].shared && eyes[1].shared && !eyes[0].exited() && !eyes[1].exited() && vrgpu::state(&eyes[0].shared->status)==vrgpu::Ready && vrgpu::state(&eyes[1].shared->status)==vrgpu::Ready;}
    bool available()const{return eyes[0].retired() && eyes[1].retired();}
    void stop(){for(auto& e:eyes)e.stop();}
    bool pollStopped(){const bool l=eyes[0].pollStopped(),r=eyes[1].pollStopped();return l && r;}
    bool join(){bool ok=true;for(auto& e:eyes)ok=e.join() && ok;return ok;}
    ~Generation(){stop();join();}
    bool build(const std::filesystem::path& root,const std::array<uint64_t,2>& handles,float requestedScale,uint64_t generation,unsigned passes=1,bool fast=false){
        scale=requestedScale;id=generation;configuredPasses=fast?1:passes;
        auto fail=[&](const char* what,HRESULT hr=E_FAIL){char b[160]{};snprintf(b,sizeof(b),"%s (0x%08lx)",what,(unsigned long)hr);error=b;return false;};
        HRESULT hr=framecapture::createDevice(device,context);if(FAILED(hr))return fail("NVIDIA D3D11 device unavailable",hr);
        if(FAILED(hr=device.As(&device5)) || FAILED(hr=context.As(&context4)))return fail("D3D11Device5/Context4 fences unsupported",hr);
        ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;DXGI_ADAPTER_DESC ad{};
        if(FAILED(hr=device.As(&dxgi)) || FAILED(hr=dxgi->GetAdapter(&adapter)) || FAILED(hr=adapter->GetDesc(&ad)))return fail("Adapter LUID unavailable",hr);
        if(FAILED(hr=device5->CreateFence(0,D3D11_FENCE_FLAG_NONE,IID_PPV_ARGS(&copyFence))))return fail("Copy completion fence creation failed",hr);
        for(unsigned i=0;i<2;++i){auto& e=eyes[i];ComPtr<ID3D11Texture2D> source;
            if(FAILED(hr=device->OpenSharedResource((HANDLE)(uintptr_t)handles[i],IID_PPV_ARGS(&source))))return fail("Source shared texture unavailable",hr);
            source->GetDesc(&desc[i]);const auto& d=desc[i];
            if(d.Width<64 || d.Height<64 || d.Width>7680 || d.Height>4320 || d.ArraySize!=1 || d.MipLevels!=1 || d.SampleDesc.Count!=1 || (d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM && d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB))return fail("Unsupported source format/dimensions; RGBA8 mono-sample <=7680x4320 required",E_NOTIMPL);
            D3D11_TEXTURE2D_DESC relay=d;relay.Format=DXGI_FORMAT_R8G8B8A8_UNORM;relay.Usage=D3D11_USAGE_DEFAULT;relay.CPUAccessFlags=0;relay.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;relay.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED;
            if(FAILED(hr=device->CreateTexture2D(&relay,nullptr,&e.input)) || FAILED(hr=device->CreateTexture2D(&relay,nullptr,&e.output)))return fail("NT relay texture creation failed",hr);
            ComPtr<IDXGIResource1> in,out;if(FAILED(hr=e.input.As(&in)) || FAILED(hr=e.output.As(&out)) || FAILED(hr=in->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&e.inputHandle)) || FAILED(hr=out->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&e.outputHandle)))return fail("NT texture handles unavailable",hr);
            if(FAILED(hr=device5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&e.inputFence))) || FAILED(hr=device5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&e.outputFence))) || FAILED(hr=e.inputFence->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&e.inputFenceHandle)) || FAILED(hr=e.outputFence->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&e.outputFenceHandle)))return fail("Shared fence creation failed",hr);
            e.name=L"Local\\CodexVR_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(generation)+L"_"+std::to_wstring(i)+L"_"+std::to_wstring(monotonicUs());
            e.mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(vrgpu::Shared),e.name.c_str());if(!e.mapping)return fail("Control mapping creation failed");
            e.requestEvent=CreateEventW(nullptr,FALSE,FALSE,(e.name+L"-request").c_str());if(!e.requestEvent)return fail("Request event creation failed");
            e.completedEvent=CreateEventW(nullptr,FALSE,FALSE,(e.name+L"-complete").c_str());if(!e.completedEvent)return fail("Completion event creation failed");
            e.shared=(vrgpu::Shared*)MapViewOfFile(e.mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(vrgpu::Shared));if(!e.shared)return fail("Control mapping view failed");
            new(e.shared) vrgpu::Shared();auto& s=*e.shared;s.driverPid=GetCurrentProcessId();s.width=d.Width;s.height=d.Height;s.workWidth=workDimension(d.Width,scale);s.workHeight=workDimension(d.Height,scale);s.adapterHigh=ad.AdapterLuid.HighPart;s.adapterLow=ad.AdapterLuid.LowPart;s.eye=i;s.generation=id;
            s.inputHandle=(uintptr_t)e.inputHandle;s.outputHandle=(uintptr_t)e.outputHandle;s.inputFenceHandle=(uintptr_t)e.inputFenceHandle;s.outputFenceHandle=(uintptr_t)e.outputFenceHandle;
            s.passes=passes;s.fast=fast?1:0;
            const auto executable=root/L"engine"/(i?L"right":L"left")/L"nvngx.dll";
            std::wstring cmd=L"\""+executable.wstring()+L"\" --vr-gpu \""+e.name+L"\"";STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
            if(!CreateProcessW(executable.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,executable.parent_path().c_str(),&si,&pi))return fail("VR engine process launch failed");
            e.process=pi.hProcess;CloseHandle(pi.hThread);
        }return true;
    }
};
}
