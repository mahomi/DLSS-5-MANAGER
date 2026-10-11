#include <windows.h>
#include <cstdio>
#include <cstring>
#pragma warning(push)
#pragma warning(disable:4100)
#include "sdk/openvr_driver.h"
#pragma warning(pop)
using AddedFn=bool(__fastcall*)(vr::IVRServerDriverHost*,const char*,vr::ETrackedDeviceClass,vr::ITrackedDeviceServerDriver*);
#define BAD_IF(condition) do { if(condition) { ++failures; printf("failed_at_line=%d\n",__LINE__); } } while(0)
struct FakeDirect final:vr::IVRDriverDirectModeComponent{
    unsigned create=0,destroy=0,destroyAll=0,next=0,submit=0,present=0,post=0,timing=0;
    uint32_t creatorPid=0;
    vr::SharedTextureHandle_t sync=0;
    const SubmitLayerPerEye_t* submitted=nullptr;
    const Throttling_t* throttle=nullptr;
    void CreateSwapTextureSet(uint32_t pid,const SwapTextureSetDesc_t*,SwapTextureSet_t* out) override{
        ++create;creatorPid=pid;
        out->rSharedTextureHandles[0]=11;out->rSharedTextureHandles[1]=12;out->rSharedTextureHandles[2]=13;out->unTextureFlags=7;
    }
    void DestroySwapTextureSet(vr::SharedTextureHandle_t) override{++destroy;}
    void DestroyAllSwapTextureSets(uint32_t) override{++destroyAll;}
    void GetNextSwapTextureSetIndex(vr::SharedTextureHandle_t[2],uint32_t(*indices)[2]) override{
        ++next;(*indices)[0]=1;(*indices)[1]=2;
    }
    void SubmitLayer(const SubmitLayerPerEye_t(&eyes)[2]) override{++submit;submitted=eyes;}
    void Present(vr::SharedTextureHandle_t handle) override{++present;sync=handle;}
    void PostPresent(const Throttling_t* p) override{++post;throttle=p;}
    void GetFrameTiming(vr::DriverDirectMode_FrameTiming* p) override{++timing;p->m_nReprojectionFlags=123;}
};
struct FakeDevice final:vr::ITrackedDeviceServerDriver{
    FakeDirect direct;
    unsigned activates=0,deactivates=0,standby=0,debug=0,poses=0;
    uint32_t activatedIndex=0,debugLength=0;
    const char* debugRequest=nullptr;
    int otherComponent=99;
    vr::EVRInitError Activate(uint32_t index) override{++activates;activatedIndex=index;return vr::VRInitError_None;}
    void Deactivate() override{++deactivates;}
    void EnterStandby() override{++standby;}
    void* GetComponent(const char* name) override{
        if(!name)return nullptr;
        return strcmp(name,vr::IVRDriverDirectModeComponent_Version)==0?static_cast<void*>(&direct):static_cast<void*>(&otherComponent);
    }
    void DebugRequest(const char* request,char* response,uint32_t length) override{
        ++debug;debugRequest=request;debugLength=length;
        if(length>=3){response[0]='O';response[1]='K';response[2]=0;}
    }
    vr::DriverPose_t GetPose() override{
        ++poses;vr::DriverPose_t pose{};pose.deviceIsConnected=true;pose.poseIsValid=true;pose.vecPosition[2]=42.25;return pose;
    }
};
struct FakeHost final:vr::IVRServerDriverHost{
    vr::ITrackedDeviceServerDriver* received=nullptr;
    const char* serial=nullptr;
    vr::ETrackedDeviceClass kind=vr::TrackedDeviceClass_Invalid;
    unsigned adds=0;
    bool TrackedDeviceAdded(const char* s,vr::ETrackedDeviceClass k,vr::ITrackedDeviceServerDriver* d) override{
        ++adds;serial=s;kind=k;received=d;return true;
    }
    void TrackedDevicePoseUpdated(uint32_t,const vr::DriverPose_t&,uint32_t) override{}
    void VsyncEvent(double) override{}
    void VendorSpecificEvent(uint32_t,vr::EVREventType,const vr::VREvent_Data_t&,double) override{}
    bool IsExiting() override{return false;}
    bool PollNextEvent(vr::VREvent_t*,uint32_t) override{return false;}
    void GetRawTrackedDevicePoses(float,vr::TrackedDevicePose_t*,uint32_t) override{}
    void RequestRestart(const char*,const char*,const char*,const char*) override{}
    uint32_t GetFrameTimings(vr::Compositor_FrameTiming*,uint32_t) override{return 0;}
    void SetDisplayEyeToHead(uint32_t,const vr::HmdMatrix34_t&,const vr::HmdMatrix34_t&) override{}
    void SetDisplayProjectionRaw(uint32_t,const vr::HmdRect2_t&,const vr::HmdRect2_t&) override{}
    void SetRecommendedRenderTargetSize(uint32_t,uint32_t,uint32_t) override{}
};
struct MockLog final:vr::IVRDriverLog{void Log(const char*) override{}};
struct FakeContext final:vr::IVRDriverContext{
    FakeHost* host;
    MockLog log;
    explicit FakeContext(FakeHost* p):host(p){}
    void* GetGenericInterface(const char* name,vr::EVRInitError* error) override{
        if(error)*error=vr::VRInitError_None;
        if(strcmp(name,vr::IVRServerDriverHost_Version)==0)return host;
        if(strcmp(name,vr::IVRDriverLog_Version)==0)return &log;
        if(error)*error=vr::VRInitError_Init_InterfaceNotFound;return nullptr;
    }
    vr::DriverHandle_t GetDriverHandle() override{return 123;}
};
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    setvbuf(stdout,nullptr,_IONBF,0);
    FakeHost host;FakeDevice device;int failures=0;
    const auto module=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!module){printf("load_error=%lu\n",GetLastError());return 3;}
    auto factory=reinterpret_cast<void*(*)(const char*,int*)>(GetProcAddress(module,"HmdDriverFactory"));
    if(!factory)return 4;
    int error=-1;
    auto provider=static_cast<vr::IServerTrackedDeviceProvider*>(factory(vr::IServerTrackedDeviceProvider_Version,&error));
    if(!provider || error)return 5;
    BAD_IF(provider->Init(nullptr)==vr::VRInitError_None);
    FakeContext context(&host);
    void** table=*reinterpret_cast<void***>(static_cast<vr::IVRServerDriverHost*>(&host));
    auto slot=reinterpret_cast<void* volatile*>(&table[0]);
    void* saved=*slot;
    if(provider->Init(&context)!=vr::VRInitError_None){puts("mock_hook_init_failed");return 1;}
    provider->RunFrame();
    BAD_IF(provider->ShouldBlockStandbyMode());provider->EnterStandby();provider->LeaveStandby();
    // Read the runtime dispatch slot explicitly to prevent compiler devirtualization.
    auto added=reinterpret_cast<AddedFn>(*slot);
    const char* serial="mock-hmd";
    BAD_IF(!added(&host,serial,vr::TrackedDeviceClass_HMD,&device));
    BAD_IF(host.received==&device || !host.received || host.serial!=serial || host.kind!=vr::TrackedDeviceClass_HMD);
    if(!host.received){provider->Cleanup();return 1;}
    auto wrapped=host.received;
    BAD_IF(wrapped->Activate(7)!=vr::VRInitError_None);
    wrapped->EnterStandby();
    char response[8]{};const char* request="test";
    wrapped->DebugRequest(request,response,sizeof(response));
    const auto pose=wrapped->GetPose();
    BAD_IF(!pose.deviceIsConnected || !pose.poseIsValid || pose.vecPosition[2]!=42.25);
    BAD_IF(strcmp(response,"OK")!=0 || device.debugRequest!=request || device.debugLength!=sizeof(response));
    BAD_IF(wrapped->GetComponent("other")!=&device.otherComponent);
    BAD_IF(wrapped->GetComponent(nullptr)!=nullptr);
    auto direct=static_cast<vr::IVRDriverDirectModeComponent*>(wrapped->GetComponent(vr::IVRDriverDirectModeComponent_Version));
    BAD_IF(!direct || direct==&device.direct);
    BAD_IF(wrapped->GetComponent(vr::IVRDriverDirectModeComponent_Version)!=direct);
    if(!direct){provider->Cleanup();return 1;}
    vr::IVRDriverDirectModeComponent::SwapTextureSetDesc_t desc{100,200,28,1};
    vr::IVRDriverDirectModeComponent::SwapTextureSet_t out{};
    direct->CreateSwapTextureSet(55,&desc,&out);
    BAD_IF(out.rSharedTextureHandles[0]!=11 || out.rSharedTextureHandles[2]!=13 || out.unTextureFlags!=7 || device.direct.creatorPid!=55);
    vr::SharedTextureHandle_t handles[]{11,12};uint32_t indices[2]{};
    direct->GetNextSwapTextureSetIndex(handles,&indices);
    BAD_IF(indices[0]!=1 || indices[1]!=2);
    vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t eyes[2]{};
    eyes[0].hTexture=11;eyes[1].hTexture=12;eyes[0].hDepthTexture=9;
    eyes[0].bounds={0.1f,0.2f,0.9f,0.8f};eyes[1].bounds={0,0,1,1};
    eyes[0].mHmdPose.m[0][3]=1.25f;
    const auto beforeLeft=eyes[0],beforeRight=eyes[1];
    direct->SubmitLayer(eyes);direct->Present(987);
    BAD_IF(device.direct.submitted!=eyes || device.direct.sync!=987);
    BAD_IF(memcmp(&eyes[0],&beforeLeft,sizeof(beforeLeft))!=0 || memcmp(&eyes[1],&beforeRight,sizeof(beforeRight))!=0);
    vr::IVRDriverDirectModeComponent::Throttling_t throttle{2,3};direct->PostPresent(&throttle);
    BAD_IF(device.direct.throttle!=&throttle);
    vr::DriverDirectMode_FrameTiming timing{};direct->GetFrameTiming(&timing);
    BAD_IF(timing.m_nReprojectionFlags!=123);
    direct->DestroySwapTextureSet(11);direct->DestroyAllSwapTextureSets(55);
    wrapped->Deactivate();
    BAD_IF(device.activates!=1 || device.activatedIndex!=7 || device.standby!=1 || device.debug!=1 || device.poses!=1 || device.deactivates!=1);
    BAD_IF(device.direct.create!=1 || device.direct.destroy!=1 || device.direct.destroyAll!=1 || device.direct.next!=1 ||
        device.direct.submit!=1 || device.direct.present!=1 || device.direct.post!=1 || device.direct.timing!=1);
    BAD_IF(!added(&host,"controller",vr::TrackedDeviceClass_Controller,&device) || host.received!=&device);
    provider->Cleanup();BAD_IF(*slot!=saved);
    // Previously returned wrappers remain valid and pass through after observation is disabled.
    direct->SubmitLayer(eyes);BAD_IF(device.direct.submit!=2);
    BAD_IF(wrapped->GetComponent(vr::IVRDriverDirectModeComponent_Version)!=&device.direct);
    BAD_IF(factory("unsupported",&error)!=nullptr || error!=vr::VRInitError_Init_InterfaceNotFound);
    printf("mock_dispatch_and_passthrough_failures=%d host_add_calls=%u\n",failures,host.adds);
    puts("actual_DLL_loaded=true slot_restoration_checked=true original_frame_arguments_unchanged=true SteamVR_initialized=false");
    FreeLibrary(module);
    return failures?1:0;
}
