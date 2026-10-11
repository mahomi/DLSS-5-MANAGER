#include "live_test_hmd.h"
#include <thread>
#include <chrono>
#include "live_control.h"
#include <chrono>
#include <tlhelp32.h>
static std::string ReadStatus(){std::ifstream f("status.json");return std::string((std::istreambuf_iterator<char>(f)),{});}
static DWORD Number(const std::string& text,const char* key){const std::string pattern=std::string("\"")+key+"\":";auto pos=text.find(pattern);return pos==std::string::npos?0:DWORD(strtoul(text.c_str()+pos+pattern.size(),nullptr,10));}
static HANDLE OwnLeftWorker(){
    const auto text=ReadStatus();if(Number(text,"driver_pid")!=GetCurrentProcessId())return nullptr;
    HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|PROCESS_TERMINATE,FALSE,Number(text,"left_pid"));if(!p)return nullptr;
    wchar_t path[32768]{};DWORD size=32768;if(!QueryFullProcessImageNameW(p,0,path,&size)){CloseHandle(p);return nullptr;}
    const auto expected=std::filesystem::current_path()/L"engine"/L"left"/L"nvngx.dll";
    if(_wcsicmp(path,expected.c_str())){CloseHandle(p);return nullptr;}return p;
}
static HANDLE MainThread(DWORD pid){
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);if(snapshot==INVALID_HANDLE_VALUE)return nullptr;
    THREADENTRY32 e{};e.dwSize=sizeof(e);HANDLE selected=nullptr;uint64_t earliest=UINT64_MAX;
    if(Thread32First(snapshot,&e))do{if(e.th32OwnerProcessID==pid){HANDLE t=OpenThread(THREAD_SUSPEND_RESUME|THREAD_QUERY_LIMITED_INFORMATION,FALSE,e.th32ThreadID);FILETIME c{},x{},k{},u{};
        if(t && GetThreadTimes(t,&c,&x,&k,&u)){const uint64_t time=(uint64_t(c.dwHighDateTime)<<32)|c.dwLowDateTime;if(time<earliest){if(selected)CloseHandle(selected);selected=t;earliest=time;t=nullptr;}}
        if(t)CloseHandle(t);}}while(Thread32Next(snapshot,&e));CloseHandle(snapshot);return selected;
}
static std::string Control(bool nr,bool fast,int target,float scale,int passes=1,bool autoBudget=false){
    std::ostringstream s;s<<"{\"version\":1,\"active\":true,\"nr\":"<<(nr?"true":"false")<<",\"fast\":"<<(fast?"true":"false")<<",\"target\":"<<target<<",\"stability\":0.9,\"work_scale\":"<<scale<<",\"intensity\":1,\"local_tone\":0.5,\"local_structure\":1,\"skin_structure\":-1,\"style\":1,\"passes\":"<<passes<<",\"auto_budget\":"<<(autoBudget?"true":"false")<<",\"pair_budget_ms\":25,\"lease_until_unix_ms\":"<<(vrlive::unixMs()+3000)<<"}";return s.str();
}
static void Gradient(GpuDirect& d,uint64_t handle,unsigned eye){
    ComPtr<ID3D11Texture2D> t;d.device->OpenSharedResource((HANDLE)(uintptr_t)handle,IID_PPV_ARGS(&t));
    D3D11_TEXTURE2D_DESC desc{};t->GetDesc(&desc);std::vector<unsigned char> p(size_t(desc.Width)*desc.Height*4);
    for(UINT y=0;y<desc.Height;++y)for(UINT x=0;x<desc.Width;++x){const size_t o=(size_t(y)*desc.Width+x)*4;
        p[o]=BYTE(30+(x*170/desc.Width)+(x*y)%13);p[o+1]=BYTE(30+y*140/desc.Height);p[o+2]=BYTE(25+((x/24+y/18)%2)*80+eye*35);p[o+3]=255;}
    d.context->UpdateSubresource(t.Get(),0,nullptr,p.data(),desc.Width*4,0);d.context->Flush();
}
int wmain(int argc,wchar_t** argv){
    if(argc<2 || argc>3)return 2;const bool fullSize=argc==3 && wcscmp(argv[2],L"--full-resolution")==0;setvbuf(stdout,nullptr,_IONBF,0);int failures=0;
    FakeHost host;GpuHmd hmd;FakeContext context(&host);
    BAD_IF(FAILED(framecapture::createDevice(hmd.direct.device,hmd.direct.context)));if(!hmd.direct.device)return 3;
    const auto module=LoadLibraryExW(argv[1],nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);if(!module)return 4;
    auto factory=reinterpret_cast<void*(*)(const char*,int*)>(GetProcAddress(module,"HmdDriverFactory"));if(!factory)return 5;
    int error=0;auto provider=static_cast<vr::IServerTrackedDeviceProvider*>(factory(vr::IServerTrackedDeviceProvider_Version,&error));if(!provider||error)return 6;
    BAD_IF(provider->Init(&context)!=vr::VRInitError_None);
    auto table=*reinterpret_cast<void***>(static_cast<vr::IVRServerDriverHost*>(&host));auto slot=reinterpret_cast<void* volatile*>(&table[0]);
    BAD_IF(!reinterpret_cast<AddedFn>(*slot)(&host,"nr-route-test",vr::TrackedDeviceClass_HMD,&hmd));
    auto direct=static_cast<vr::IVRDriverDirectModeComponent*>(host.received->GetComponent(vr::IVRDriverDirectModeComponent_Version));if(!direct)return 7;
    const auto left=hmd.direct.make(fullSize?2496:640,fullSize?2688:512,29,D3D11_RESOURCE_MISC_SHARED,0xff772244);
    const auto right=hmd.direct.make(fullSize?2496:640,fullSize?2688:512,29,D3D11_RESOURCE_MISC_SHARED,0xff228844);
    printf("stereo_source_size=%ux%u deadline_default_ms=25\n",fullSize?2496:640,fullSize?2688:512);
    const auto sync=hmd.direct.make(8,8,29,D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX,0);hmd.direct.inputSyncForChecking=sync;
    BAD_IF(!left||!right||!sync);Gradient(hmd.direct,left,0);Gradient(hmd.direct,right,1);
    vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t eyes[2]{};eyes[0].hTexture=left;eyes[1].hTexture=right;
    eyes[0].bounds={.1f,.2f,.9f,.8f};eyes[1].bounds={0,0,1,1};eyes[0].mHmdPose.m[0][3]=12.5f;eyes[1].hDepthTexture=123;
    const auto original=framecapture::capture(hmd.direct.device.Get(),hmd.direct.context.Get(),sync,{left,right});BAD_IF(!original.success);
    auto frame=[&](){direct->SubmitLayer(eyes);direct->Present(sync);};
    auto waitStatus=[&](const char* state){const auto deadline=GetTickCount64()+3000;const auto token=std::string("\"state\":\"")+state+"\"";
        do{vrlive::atomicWrite(L"control.json",Control(true,true,6,.25f));if(ReadStatus().find(token)!=std::string::npos)return true;Sleep(50);}while(GetTickCount64()<deadline);
        printf("status_wait_failed expected=%s actual=%s\n",state,ReadStatus().c_str());return false;};
    vrlive::atomicWrite(L"control.json",Control(false,false,2,.25f));Sleep(150);frame();BAD_IF(hmd.direct.received.back()[0].hTexture!=left);
    auto processed=[&](bool fast,int target,int passes=1,float scale=.25f,bool autoBudget=false){
        // Publish before submitting: the broker reads controls asynchronously.
        // Otherwise the first frame can legitimately be from the previous mode.
        vrlive::atomicWrite(L"control.json",Control(true,fast,target,scale,passes,autoBudget));Sleep(200);
        const auto deadline=GetTickCount64()+12000;bool success=false;unsigned retries=0;
        do{
            vrlive::atomicWrite(L"control.json",Control(true,fast,target,scale,passes,autoBudget));frame();
            success=hmd.direct.received.back()[0].hTexture!=eyes[0].hTexture;
            if(success)break;
            if(ReadStatus().find("budget_exceeded")!=std::string::npos && retries++<3){
                printf("test retry after real scheduling budget miss; guard remained raw\n");
                vrlive::atomicWrite(L"control.json",Control(false,false,target,scale,passes,autoBudget));Sleep(200);
            }
            Sleep(30);
        }while(GetTickCount64()<deadline);
        if(!success){std::ifstream status("status.json");printf("status=%s\n",std::string((std::istreambuf_iterator<char>(status)),{}).c_str());}
        return success;
    };
    BAD_IF(!processed(false,2));
    auto enhanced=hmd.direct.received.back();
    if(enhanced[0].hTexture!=left){
        BAD_IF(enhanced[1].hTexture==right || hmd.direct.lastSync==sync || !hmd.direct.splitHealthy);
        auto expected0=eyes[0],expected1=eyes[1];expected0.hTexture=enhanced[0].hTexture;expected1.hTexture=enhanced[1].hTexture;
        BAD_IF(memcmp(&enhanced[0],&expected0,sizeof(expected0)) || memcmp(&enhanced[1],&expected1,sizeof(expected1)));
        const auto event=hmd.direct.events.size();BAD_IF(event<3 || hmd.direct.events[event-1]!='N' || hmd.direct.events[event-2]!='P');
    }
    const auto nr=framecapture::capture(hmd.direct.device.Get(),hmd.direct.context.Get(),sync,{enhanced[0].hTexture,enhanced[1].hTexture});BAD_IF(!nr.success);
    Sleep(150);BAD_IF(!processed(true,6));Sleep(150);BAD_IF(!processed(true,6));
    enhanced=hmd.direct.received.back();const auto fast=framecapture::capture(hmd.direct.device.Get(),hmd.direct.context.Get(),sync,{enhanced[0].hTexture,enhanced[1].hTexture});BAD_IF(!fast.success);
    size_t nrChanges=0,fastChanges=0;if(nr.success&&fast.success&&original.success)for(size_t eye=0;eye<2;++eye)for(size_t i=0;i<nr.images[eye].rgba.size();++i){nrChanges+=nr.images[eye].rgba[i]!=original.images[eye].rgba[i];fastChanges+=nr.images[eye].rgba[i]!=fast.images[eye].rgba[i];}
    BAD_IF(nrChanges<100 || fastChanges<100);printf("actual_stereo_NR_changed_bytes=%zu actual_stereo_Fast_changed_bytes=%zu\n",nrChanges,fastChanges);
    const auto unchanged=framecapture::capture(hmd.direct.device.Get(),hmd.direct.context.Get(),sync,{left,right});
    BAD_IF(!unchanged.success);if(unchanged.success&&original.success)for(size_t eye=0;eye<2;++eye)BAD_IF(unchanged.images[eye].rgba!=original.images[eye].rgba);
    // Reproduce the uncounted zero-timeout ownership failure using the
    // test's own producer sync. It must pause once, never flash back to NR.
    BAD_IF(!processed(true,6));
    ComPtr<ID3D11Texture2D> heldSync;ComPtr<IDXGIKeyedMutex> heldKey;
    BAD_IF(FAILED(hmd.direct.device->OpenSharedResource((HANDLE)(uintptr_t)sync,IID_PPV_ARGS(&heldSync))) || FAILED(heldSync.As(&heldKey)));
    if(heldKey){
        BAD_IF(heldKey->AcquireSync(0,0)!=S_OK);HRESULT producerRelease=E_FAIL;
        std::thread producer([&]{Sleep(50);producerRelease=heldKey->ReleaseSync(0);});
        frame();producer.join();BAD_IF(producerRelease!=S_OK);
        BAD_IF(hmd.direct.received.back()[0].hTexture==left || hmd.direct.received.back()[1].hTexture==right);
        printf("50ms_producer_delay_waited_and_current_NR_delivered=true old5ms_limit_not_used=true\n");
        BAD_IF(heldKey->AcquireSync(0,0)!=S_OK);
        std::thread cancel([&]{Sleep(100);vrlive::atomicWrite(L"control.json",Control(false,false,2,.25f));Sleep(120);producerRelease=heldKey->ReleaseSync(0);});
        auto cancelStart=std::chrono::steady_clock::now();frame();const auto cancelMs=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-cancelStart).count();cancel.join();
        BAD_IF(cancelMs>=1000 || producerRelease!=S_OK || hmd.direct.received.back()[0].hTexture!=left || hmd.direct.received.back()[1].hTexture!=right);
        BAD_IF(!processed(true,6));
        printf("OFF_while_waiting_for_input_cancels_within1sec=true elapsed_ms=%lld fresh_NR_recovers=true\n",cancelMs);
        BAD_IF(heldKey->AcquireSync(0,0)!=S_OK);frame();
        BAD_IF(hmd.direct.received.back()[0].hTexture!=left || hmd.direct.received.back()[1].hTexture!=right);
        BAD_IF(heldKey->ReleaseSync(0)!=S_OK);
        // Ownership is available again, but automatic NR/raw alternation is forbidden.
        frame();BAD_IF(hmd.direct.received.back()[0].hTexture!=left || hmd.direct.received.back()[1].hTexture!=right);
        BAD_IF(!waitStatus("error"));
        BAD_IF(ReadStatus().find("input_watchdog_or_sync_failed")==std::string::npos);
        vrlive::atomicWrite(L"control.json",Control(false,false,2,.25f));Sleep(200);BAD_IF(!processed(true,6));
        printf("busy_input_sync_counted=true single_failure_paused=true released_sync_does_not_flash_back=true explicit_off_on_recovers=true\n");}
    Sleep(600);HANDLE worker=OwnLeftWorker();BAD_IF(!worker);
    if(worker){
        HANDLE thread=MainThread(GetProcessId(worker));BAD_IF(!thread);
        if(thread){const auto suspended=SuspendThread(thread);BAD_IF(suspended==DWORD(-1));
            if(suspended!=DWORD(-1)){
                DWORD resumed=DWORD(-1);
                std::thread resume([&]{Sleep(300);resumed=ResumeThread(thread);});
                vrlive::atomicWrite(L"control.json",Control(true,true,6,.25f));frame();resume.join();
                BAD_IF(resumed==DWORD(-1) || hmd.direct.received.back()[0].hTexture==left || hmd.direct.received.back()[1].hTexture==right);
                printf("300ms_delayed_eye_current_stereo_output_delivered=true old25_250ms_limits_not_used=true\n");
                BAD_IF(SuspendThread(thread)==DWORD(-1));
                for(int i=0;i<3;++i){vrlive::atomicWrite(L"control.json",Control(true,true,6,.25f));frame();BAD_IF(hmd.direct.received.back()[0].hTexture!=left || hmd.direct.received.back()[1].hTexture!=right || hmd.direct.lastSync!=sync);}
                BAD_IF(!waitStatus("error"));
                BAD_IF(ResumeThread(thread)==DWORD(-1));Sleep(150);frame();BAD_IF(hmd.direct.received.back()[0].hTexture!=left || hmd.direct.received.back()[1].hTexture!=right);
                vrlive::atomicWrite(L"control.json",Control(false,false,2,.25f));Sleep(150);BAD_IF(!processed(true,2));
                printf("pending_eye_deadline_raw_both=true single_visible_NR_miss_latched=true delayed_old_output_not_reused=true off_on_recovered=true\n");
            }
            CloseHandle(thread);
        }
        CloseHandle(worker);Sleep(600);worker=OwnLeftWorker();BAD_IF(!worker);
        if(worker){BAD_IF(!TerminateProcess(worker,88));CloseHandle(worker);}Sleep(200);frame();BAD_IF(hmd.direct.received.back()[0].hTexture!=left || hmd.direct.received.back()[1].hTexture!=right);
        BAD_IF(!waitStatus("error"));
        vrlive::atomicWrite(L"control.json",Control(false,false,2,.25f));Sleep(150);BAD_IF(!processed(true,2));
        printf("own_left_worker_exit_raw_both=true fresh_workers_recovered=true\n");
    }
    vrlive::atomicWrite(L"control.json",Control(false,false,2,.25f));Sleep(150);frame();
    if(hmd.direct.received.back()[0].hTexture!=left || hmd.direct.lastSync!=sync)printf("OFF not yet observed status=%s\n",ReadStatus().c_str());
    BAD_IF(hmd.direct.received.back()[0].hTexture!=left || hmd.direct.lastSync!=sync);
    BAD_IF(!processed(true,2));Sleep(3250);frame();
    if(hmd.direct.received.back()[0].hTexture!=left || hmd.direct.lastSync!=sync)printf("expired lease still applied status=%s control=%s\n",ReadStatus().c_str(),Control(true,true,2,.25f).c_str());
    BAD_IF(hmd.direct.received.back()[0].hTexture!=left || hmd.direct.lastSync!=sync);
    vrlive::atomicWrite(L"control.json","{broken");Sleep(150);frame();BAD_IF(hmd.direct.received.back()[0].hTexture!=left);
    BAD_IF(!processed(false,2,1,.5f,true));
    for(int i=0;i<8;++i){vrlive::atomicWrite(L"control.json",Control(true,false,2,.5f,1,true));frame();BAD_IF(hmd.direct.received.back()[0].hTexture==left || hmd.direct.received.back()[1].hTexture==right);}
    BAD_IF(!processed(false,2,2,.2f,true));Sleep(600);BAD_IF(ReadStatus().find("\"applied_passes\":2")==std::string::npos);
    BAD_IF(!processed(false,2,6,.2f,true));Sleep(600);BAD_IF(ReadStatus().find("\"applied_passes\":6")==std::string::npos);
    printf("original_stereo_source_50percent_sustained_NR_and20percent_real_2_6_passes_checked=true\n");
    direct->DestroySwapTextureSet(left);direct->DestroySwapTextureSet(right);
    const auto resizedLeft=hmd.direct.make(800,600,29,D3D11_RESOURCE_MISC_SHARED,0xff772244);
    const auto resizedRight=hmd.direct.make(800,600,29,D3D11_RESOURCE_MISC_SHARED,0xff228844);
    Gradient(hmd.direct,resizedLeft,0);Gradient(hmd.direct,resizedRight,1);eyes[0].hTexture=resizedLeft;eyes[1].hTexture=resizedRight;
    BAD_IF(!processed(true,2));const auto resized=hmd.direct.received.back();BAD_IF(resized[0].hTexture==resizedLeft || resized[1].hTexture==resizedRight);
    ComPtr<ID3D11Texture2D> resizedTexture;BAD_IF(FAILED(hmd.direct.device->OpenSharedResource((HANDLE)(uintptr_t)resized[0].hTexture,IID_PPV_ARGS(&resizedTexture))));
    if(resizedTexture){D3D11_TEXTURE2D_DESC d{};resizedTexture->GetDesc(&d);BAD_IF(d.Width!=800 || d.Height!=600);}
    printf("source_swapsets_destroyed_and_stereo_resize_rebuilt=true new_output_size=800x600\n");
    BAD_IF(!processed(false,2,1,.5f,true));auto regularFrame=hmd.direct.received.back();
    auto one=framecapture::capture(hmd.direct.device.Get(),hmd.direct.context.Get(),sync,{regularFrame[0].hTexture,regularFrame[1].hTexture});
    BAD_IF(!processed(false,2,2,.5f,true));regularFrame=hmd.direct.received.back();
    auto two=framecapture::capture(hmd.direct.device.Get(),hmd.direct.context.Get(),sync,{regularFrame[0].hTexture,regularFrame[1].hTexture});
    Sleep(600);BAD_IF(ReadStatus().find("\"applied_passes\":2")==std::string::npos);
    BAD_IF(!processed(false,2,6,.5f,true));regularFrame=hmd.direct.received.back();
    auto six=framecapture::capture(hmd.direct.device.Get(),hmd.direct.context.Get(),sync,{regularFrame[0].hTexture,regularFrame[1].hTexture});
    Sleep(600);BAD_IF(ReadStatus().find("\"applied_passes\":6")==std::string::npos);
    size_t diff2=0,diff6=0;BAD_IF(!one.success || !two.success || !six.success);
    if(one.success && two.success && six.success)for(size_t eye=0;eye<2;++eye)for(size_t i=0;i<one.images[eye].rgba.size();++i){diff2+=one.images[eye].rgba[i]!=two.images[eye].rgba[i];diff6+=two.images[eye].rgba[i]!=six.images[eye].rgba[i];}
    BAD_IF(diff2<100 || diff6<100);
    BAD_IF(!processed(true,2,6,.5f,true));Sleep(600);BAD_IF(ReadStatus().find("\"applied_passes\":1")==std::string::npos);
    BAD_IF(!processed(false,2,6,.5f,true));Sleep(600);BAD_IF(ReadStatus().find("\"applied_passes\":6")==std::string::npos);
    printf("real_stereo_normal_1_2_6_passes_checked=true difference_1_to_2=%zu difference_2_to_6=%zu Fast_overrides_to1_OFF_restores6=true auto_deadline_scale50=true\n",diff2,diff6);
    BAD_IF(!processed(true,2));
    // Keep the lease alive independently of render submissions while testing
    // source absence. Broker publication can lag its observation by 500 ms.
    const auto waitUntil=GetTickCount64()+3000;bool waiting=false;
    do{vrlive::atomicWrite(L"control.json",Control(true,true,2,.25f));Sleep(100);const auto s=ReadStatus();
        waiting=s.find("\"state\":\"waiting_source\"")!=std::string::npos && s.find("\"nr_applied\":false")!=std::string::npos;
    }while(!waiting && GetTickCount64()<waitUntil);
    if(!waiting)printf("source_absence_status=%s\n",ReadStatus().c_str());BAD_IF(!waiting);
    printf("source_frames_stopped_applied_status_cleared=true\n");
    hmd.direct.lifetimeEnded=true;provider->Cleanup();DeleteFileW(L"control.json");BAD_IF(hmd.direct.postLifetimeCalls!=0);
    BAD_IF(!hmd.direct.splitHealthy || hmd.direct.splitChecks<3);
    printf("actual_DLL_stereo_route_failures=%d NR_and_Fast_real=true source_unchanged=true held_input_and_private_output_sync=true current_eye_pair=true next_index_after_present=true off_and_lease_and_invalid_control_raw_checked=true SteamVR_initialized=false\n",failures);
    return failures?1:0;
}
