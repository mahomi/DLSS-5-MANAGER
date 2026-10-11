// SDK call-chain observation inside vrserver. No new devices or frame texture access.
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <atomic>
#pragma warning(push)
#pragma warning(disable:4100) // Valve SDK default stub methods intentionally ignore arguments.
#include "sdk/openvr_driver.h"
#pragma warning(pop)
static HMODULE ownModule=nullptr;
static std::mutex logMutex;
static const char* const* const versions=vr::k_InterfaceVersions;
static void record(const char* role,const char* format,...){
    char message[2048]{};
    va_list args;va_start(args,format);vsnprintf_s(message,sizeof(message),_TRUNCATE,format,args);va_end(args);
    wchar_t modulePath[32768]{},processPath[32768]{};
    if(!GetModuleFileNameW(ownModule,modulePath,32768) || !GetModuleFileNameW(nullptr,processPath,32768))return;
    try{
#ifdef OBSERVER_SELF_TEST
        const auto root=std::filesystem::path(modulePath).parent_path();
#else
        const auto root=std::filesystem::path(modulePath).parent_path().parent_path().parent_path();
#endif
        const auto folder=root/L"logs";
        std::error_code error;std::filesystem::create_directory(folder,error);
        const auto name=std::filesystem::path(processPath).stem().wstring()+L"-"+std::to_wstring(GetCurrentProcessId())+L".txt";
        SYSTEMTIME utc{};GetSystemTime(&utc);
        char line[2300]{};
        snprintf(line,sizeof(line),"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ role=%s %s\n",
            utc.wYear,utc.wMonth,utc.wDay,utc.wHour,utc.wMinute,utc.wSecond,utc.wMilliseconds,role,message);
        std::lock_guard<std::mutex> lock(logMutex);
        std::ofstream file(folder/name,std::ios::binary|std::ios::app);file<<line;
    }catch(...){/* Diagnostics must not interrupt the runtime. */}
}
static void modules(const char* role){
    const wchar_t* names[]{L"driver_VirtualDesktop.dll",L"driver_oculus.dll",L"vrclient_x64.dll"};
    const char* labels[]{"driver_VirtualDesktop.dll","driver_oculus.dll","vrclient_x64.dll"};
    for(int i=0;i<3;++i){
        // Inspect only already-loaded modules. Never load or initialize the other drivers.
        const auto module=GetModuleHandleW(names[i]);
        record(role,"loaded_module=%s present=%d factory_export_present=%d",
            labels[i],int(module!=nullptr),int(module && GetProcAddress(module,"HmdDriverFactory")!=nullptr));
    }
}
static void inspect(vr::IVRDriverContext* context,const char* role){
    const char* names[]{vr::IVRDriverLog_Version,vr::IVRServerDriverHost_Version,vr::IVRDriverManager_Version};
    for(auto name:names){
        vr::EVRInitError error=vr::VRInitError_None;
        auto ptr=context->GetGenericInterface(name,&error);
        record(role,"runtime_interface=%s available=%d error=%d",name,int(ptr!=nullptr && !error),int(error));
        if(ptr && !error && strcmp(name,vr::IVRDriverLog_Version)==0)
            static_cast<vr::IVRDriverLog*>(ptr)->Log("codex_live_flow: current-frame stereo NR GPU relay; live controls default off");
    }
    record(role,"context_driver_handle=%llu",(unsigned long long)context->GetDriverHandle());
    modules(role);
}

#include <memory>
#include <vector>
#include "marker_session.h"
static std::atomic_bool observing{false};
using AddedFn=bool(__fastcall*)(vr::IVRServerDriverHost*,const char*,vr::ETrackedDeviceClass,vr::ITrackedDeviceServerDriver*);
static AddedFn originalAdded=nullptr;
static void** addedSlot=nullptr;
static std::mutex devicesMutex;
struct DirectObserver final:vr::IVRDriverDirectModeComponent{
    vr::IVRDriverDirectModeComponent* original;
    MarkerSession marker;
    std::atomic_uint64_t submits{0},presents{0},lastLog{0};
    std::atomic_uint64_t indexCalls{0};
    explicit DirectObserver(vr::IVRDriverDirectModeComponent* p):original(p),marker(p){}
    void CreateSwapTextureSet(uint32_t pid,const SwapTextureSetDesc_t* desc,SwapTextureSet_t* out) override{
        original->CreateSwapTextureSet(pid,desc,out);
        if(observing && desc && out)record("frame","create_set creator_pid=%u size=%ux%u format=%u samples=%u handles=%llu,%llu,%llu flags=%u",
            pid,desc->nWidth,desc->nHeight,desc->nFormat,desc->nSampleCount,
            (unsigned long long)out->rSharedTextureHandles[0],(unsigned long long)out->rSharedTextureHandles[1],
            (unsigned long long)out->rSharedTextureHandles[2],out->unTextureFlags);
    }
    void DestroySwapTextureSet(vr::SharedTextureHandle_t handle) override{
        marker.invalidateGpuCache();
        if(observing)record("frame","destroy_set handle=%llu",(unsigned long long)handle);
        original->DestroySwapTextureSet(handle);
    }
    void DestroyAllSwapTextureSets(uint32_t pid) override{
        marker.invalidateGpuCache();
        if(observing)record("frame","destroy_all creator_pid=%u",pid);
        original->DestroyAllSwapTextureSets(pid);
    }
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t handles[2],uint32_t(*indices)[2]) override{
        const bool profiling=markerRequested.load();const auto start=profiling?perfNowUs():0;
        original->GetNextSwapTextureSetIndex(handles,indices);
        if(profiling){
            const auto elapsed=perfNowUs()-start,count=indexCalls.fetch_add(1)+1;
            if(elapsed>=1000 || count<=3 || count%60==0)
                record("perf_client_index","count=%llu elapsed_us=%llu handles=%llu,%llu indices=%u,%u",
                    (unsigned long long)count,(unsigned long long)elapsed,(unsigned long long)handles[0],
                    (unsigned long long)handles[1],(*indices)[0],(*indices)[1]);
        }
    }
    void SubmitLayer(const SubmitLayerPerEye_t(&eyes)[2]) override{
        const auto count=submits.fetch_add(1)+1,now=GetTickCount64();
        auto prior=lastLog.load();
        if(observing && (count<=6 || (now-prior>=2000 && lastLog.compare_exchange_strong(prior,now))))
            record("frame","submit count=%llu left=%llu right=%llu depth_left=%llu depth_right=%llu bounds_left=%f,%f,%f,%f bounds_right=%f,%f,%f,%f",
                (unsigned long long)count,(unsigned long long)eyes[0].hTexture,(unsigned long long)eyes[1].hTexture,
                (unsigned long long)eyes[0].hDepthTexture,(unsigned long long)eyes[1].hDepthTexture,
                eyes[0].bounds.uMin,eyes[0].bounds.vMin,eyes[0].bounds.uMax,eyes[0].bounds.vMax,
                eyes[1].bounds.uMin,eyes[1].bounds.vMin,eyes[1].bounds.uMax,eyes[1].bounds.vMax);
        if(!observing || !marker.queue(eyes))original->SubmitLayer(eyes);
    }
    void Present(vr::SharedTextureHandle_t sync) override{
        const auto count=presents.fetch_add(1)+1;
        if(observing && count<=3)record("frame","present count=%llu sync=%llu",(unsigned long long)count,(unsigned long long)sync);
        if(!observing || !marker.present(sync))original->Present(sync);
    }
    void PostPresent(const Throttling_t* throttle) override{original->PostPresent(throttle);}
    void GetFrameTiming(vr::DriverDirectMode_FrameTiming* timing) override{original->GetFrameTiming(timing);}
};
struct DeviceObserver final:vr::ITrackedDeviceServerDriver{
    vr::ITrackedDeviceServerDriver* original;
    std::mutex componentMutex;
    // Keep wrappers alive until process exit; runtime may retain component pointers.
    std::vector<std::unique_ptr<DirectObserver>> components;
    std::atomic_uint componentRequests{0};
    explicit DeviceObserver(vr::ITrackedDeviceServerDriver* p):original(p){}
    vr::EVRInitError Activate(uint32_t index) override{
        if(observing)record("device","activate index=%u",index);return original->Activate(index);
    }
    void Deactivate() override{if(observing)record("device","deactivate");original->Deactivate();}
    void EnterStandby() override{original->EnterStandby();}
    void* GetComponent(const char* name) override{
        void* ptr=original->GetComponent(name);
        if(observing && componentRequests.fetch_add(1)<32)record("device","component=%s original_available=%d",name?name:"(null)",int(ptr!=nullptr));
        if(!observing || !ptr || !name || strcmp(name,vr::IVRDriverDirectModeComponent_Version)!=0)return ptr;
        try{
            std::lock_guard<std::mutex> lock(componentMutex);
            for(auto& c:components)if(c->original==ptr)return c.get();
            if(components.size()>=8){record("device","component_limit passthrough_original=true");return ptr;}
            components.push_back(std::make_unique<DirectObserver>(static_cast<vr::IVRDriverDirectModeComponent*>(ptr)));
            record("device","direct_mode_observer_created version=%s",vr::IVRDriverDirectModeComponent_Version);
            return components.back().get();
        }catch(...){return ptr;}
    }
    void DebugRequest(const char* request,char* response,uint32_t length) override{original->DebugRequest(request,response,length);}
    vr::DriverPose_t GetPose() override{return original->GetPose();}
};
static std::vector<std::unique_ptr<DeviceObserver>> devices;
static bool __fastcall deviceAdded(vr::IVRServerDriverHost* host,const char* serial,vr::ETrackedDeviceClass kind,vr::ITrackedDeviceServerDriver* device){
    auto forwarded=device;
    if(observing && kind==vr::TrackedDeviceClass_HMD && device){
        try{
            std::lock_guard<std::mutex> lock(devicesMutex);
            bool found=false;
            for(auto& d:devices)if(d->original==device){forwarded=d.get();found=true;break;}
            if(!found && devices.size()<8){devices.push_back(std::make_unique<DeviceObserver>(device));forwarded=devices.back().get();}
        }catch(...){forwarded=device;}
        record("host","hmd_added wrapped=%d",int(forwarded!=device));
    }
    // Same caller host, serial, device class and result. No new device registration.
    return originalAdded(host,serial,kind,forwarded);
}
static bool install(vr::IVRServerDriverHost* host,bool pin){
    if(!host || addedSlot)return false;
    void** table=*reinterpret_cast<void***>(host);
    if(!table || !table[0])return false;
    if(pin){
        HMODULE pinned=nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(&deviceAdded),&pinned)){record("hook","pin_failed error=%lu",GetLastError());return false;}
    }
    auto slot=&table[0];DWORD protection=0;
    if(!VirtualProtect(slot,sizeof(void*),PAGE_READWRITE,&protection)){record("hook","protect_failed error=%lu",GetLastError());return false;}
    originalAdded=reinterpret_cast<AddedFn>(*slot);
    observing=true;
    void* replaced=InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(slot),
        reinterpret_cast<void*>(&deviceAdded),reinterpret_cast<void*>(originalAdded));
    DWORD ignored=0;const bool protectedAgain=VirtualProtect(slot,sizeof(void*),protection,&ignored)!=FALSE;
    if(replaced!=reinterpret_cast<void*>(originalAdded)){observing=false;record("hook","slot_changed_aborted");return false;}
    if(!protectedAgain){
        observing=false;
        InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(slot),
            reinterpret_cast<void*>(originalAdded),reinterpret_cast<void*>(&deviceAdded));
        record("hook","page_protection_restore_failed observer_aborted=true");return false;
    }
    addedSlot=slot;
    record("hook","host_006_slot0_installed=true protection_restored=%d module_pinned=%d",int(protectedAgain),int(pin));
    return true;
}
static void restore(){
    observing=false;
    if(!addedSlot)return;
    DWORD protection=0;
    if(VirtualProtect(addedSlot,sizeof(void*),PAGE_READWRITE,&protection)){
        void* prior=InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(addedSlot),
            reinterpret_cast<void*>(originalAdded),reinterpret_cast<void*>(&deviceAdded));
        DWORD ignored=0;VirtualProtect(addedSlot,sizeof(void*),protection,&ignored);
        record("hook","cleanup_slot_restored=%d another_owner_preserved=%d",
            int(prior==reinterpret_cast<void*>(&deviceAdded)),int(prior!=reinterpret_cast<void*>(&deviceAdded)));
    }else record("hook","cleanup_protect_failed=%lu module_remains_pinned=true",GetLastError());
    // Do not clear originalAdded or delete wrappers while the runtime may still call them.
    addedSlot=nullptr;
}
class ObserverServer final:public vr::IServerTrackedDeviceProvider{
    bool started=false;
public:
    vr::EVRInitError Init(vr::IVRDriverContext* context) override{
        if(!context)return vr::VRInitError_Init_InvalidInterface;
        record("server","init version=live-flow-v1 pid=%lu processing_default_off=true",GetCurrentProcessId());
        vr::EVRInitError error=vr::VRInitError_None;
        auto host=static_cast<vr::IVRServerDriverHost*>(context->GetGenericInterface(vr::IVRServerDriverHost_Version,&error));
        const bool enabled=!error && host && install(host,true);
        record("server","observer_enabled=%d interface_error=%d",int(enabled),int(error));
        inspect(context,"server");started=false;
        return vr::VRInitError_None;
    }
    void Cleanup() override{
        record("server","cleanup");restore();
        std::lock_guard<std::mutex> lock(devicesMutex);
        for(auto& d:devices){
            std::lock_guard<std::mutex> componentLock(d->componentMutex);
            for(auto& c:d->components){
                c->marker.shutdown();
                record("summary","submits=%llu presents=%llu",(unsigned long long)c->submits.load(),(unsigned long long)c->presents.load());
            }
        }
    }
    const char* const* GetInterfaceVersions() override{return versions;}
    void RunFrame() override{
        if(!started){record("server","run_frame_started");started=true;}
    }
    bool ShouldBlockStandbyMode() override{return false;}
    void EnterStandby() override{}
    void LeaveStandby() override{}
};
static ObserverServer observerServer;
extern "C" __declspec(dllexport) void* HmdDriverFactory(const char* name,int* error){
    if(error)*error=vr::VRInitError_None;
    if(name && strcmp(name,vr::IServerTrackedDeviceProvider_Version)==0)return &observerServer;
    if(error)*error=vr::VRInitError_Init_InterfaceNotFound;return nullptr;
}
BOOL APIENTRY DllMain(HMODULE module,DWORD reason,LPVOID){
    if(reason==DLL_PROCESS_ATTACH)ownModule=module;
    return TRUE;
}
