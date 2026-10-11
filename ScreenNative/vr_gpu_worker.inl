// GPU-only live relay. Included after the existing NR / measured Fast path.
// The D3D11 producer retains its slot until both the output fence and completed
// sequence retire. No captured colour passes through a CPU buffer.
namespace {
template<class T> void VrRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }
struct VrRelay {
    ID3D12Resource *input = nullptr, *output = nullptr;
    ID3D12Fence *inputFence = nullptr, *outputFence = nullptr;
    ~VrRelay() { close(); }
    void close() { VrRelease(input); VrRelease(output); VrRelease(inputFence); VrRelease(outputFence); }
};
static bool VrAlive(vrgpu::Shared *s, HANDLE driver) {
    return !vrgpu::state(&s->stop) && WaitForSingleObject(driver, 0) == WAIT_TIMEOUT;
}
static bool VrFence(ID3D12Fence *f, UINT64 value, vrgpu::Shared *s, HANDLE driver, DWORD timeout=30000) {
    const UINT64 initial=f->GetCompletedValue();
    if(initial==UINT64_MAX)return false;
    if(initial>=value)return true;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!event)return false;
    if(FAILED(f->SetEventOnCompletion(value,event))){CloseHandle(event);return false;}
    const ULONGLONG until=GetTickCount64()+timeout;
    bool completed=false;
    do {
        const UINT64 done=f->GetCompletedValue();
        if(done==UINT64_MAX || !VrAlive(s,driver))break;
        if(done>=value){completed=true;break;}
        HANDLE waits[]{event,driver};
        const DWORD result=WaitForMultipleObjects(2,waits,FALSE,10);
        if(result==WAIT_OBJECT_0){const auto finished=f->GetCompletedValue();completed=finished!=UINT64_MAX && finished>=value;break;}
        if(result!=WAIT_TIMEOUT)break;
    } while(GetTickCount64()<until);
    CloseHandle(event);
    if(!completed)Log("[vr-gpu] fence interrupted or timeout at %llu", value);
    return completed;
}
static bool VrValid(const vrgpu::Shared &p) {
    return p.magic==vrgpu::Magic && p.version==vrgpu::Version && p.bytes==sizeof(p) && p.driverPid &&
        p.eye<2 && p.passes>=1 && p.passes<=6 && p.format==DXGI_FORMAT_R8G8B8A8_UNORM && p.width>=64 && p.height>=64 &&
        p.width<=7680 && p.height<=4320 && p.workWidth>=64 && p.workHeight>=64 &&
        p.workWidth<=p.width && p.workHeight<=p.height && p.inputHandle && p.outputHandle &&
        p.inputFenceHandle && p.outputFenceHandle && p.fast<=1 && p.reset<=1 && p.target>=1 && p.target<=6 && p.style<=2 &&
        std::isfinite(p.stability) && p.stability>=0 && p.stability<=.9f &&
        std::isfinite(p.intensity) && p.intensity>=0 && p.intensity<=2 &&
        std::isfinite(p.localTone) && p.localTone>=0 && p.localTone<=2 &&
        std::isfinite(p.localStructure) && p.localStructure>=0 && p.localStructure<=2 &&
        std::isfinite(p.skinStructure) && p.skinStructure>=-1 && p.skinStructure<=2;
}
template<class T> static bool VrOpen(HANDLE owner, uint64_t foreign, T **out) {
    HANDLE local=nullptr;
    if(!DuplicateHandle(owner,reinterpret_cast<HANDLE>(static_cast<uintptr_t>(foreign)),GetCurrentProcess(),
                        &local,0,FALSE,DUPLICATE_SAME_ACCESS)) {
        Log("[vr-gpu] DuplicateHandle failed %lu",GetLastError()); return false;
    }
    const HRESULT hr=h.dev->OpenSharedHandle(local,__uuidof(T),reinterpret_cast<void**>(out));
    CloseHandle(local);
    if(FAILED(hr)) Log("[vr-gpu] OpenSharedHandle failed 0x%08X",hr);
    return SUCCEEDED(hr);
}
static bool VrOpenRelay(HANDLE owner,const vrgpu::Shared &p,VrRelay &r) {
    r.close();
    if(!VrOpen(owner,p.inputHandle,&r.input)||!VrOpen(owner,p.outputHandle,&r.output)||
       !VrOpen(owner,p.inputFenceHandle,&r.inputFence)||!VrOpen(owner,p.outputFenceHandle,&r.outputFence)) return false;
    for(auto texture:{r.input,r.output}) {
        const auto d=texture->GetDesc();
        if(d.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width!=p.width || d.Height!=p.height ||
           d.Format!=DXGI_FORMAT_R8G8B8A8_UNORM || d.DepthOrArraySize!=1 || d.MipLevels!=1 || d.SampleDesc.Count!=1) {
            Log("[vr-gpu] relay descriptor mismatch"); return false;
        }
    }
    return true;
}
static void VrControls(const vrgpu::Shared &p) {
    g_vr_requested_passes=p.passes;
    g_video_options.intensity=p.intensity; g_video_options.local_tone=p.localTone;
    g_video_options.local_structure=p.localStructure; g_video_options.skin_structure=p.skinStructure;
    g_video_options.style=p.style; g_video_options.auto_mask=1; g_video_options.ui_correction=0;
    if(g_fast_settings.enabled!=(p.fast!=0)||g_fast_settings.target!=p.target||g_fast_settings.stability!=p.stability)
        ++g_fast_settings_revision;
    g_fast_settings.enabled=p.fast!=0; g_fast_settings.target=p.target; g_fast_settings.stability=p.stability;
}
static bool VrGray(VideoState& v){
    const UINT width=v.upscale?v.full_w:v.w,height=v.upscale?v.full_h:v.hgt;
    if(!EnsureGrayPipeline() || !EnsureOfaGray(width,height))return false;
    const UINT stride=h.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu=g_ofa_gray_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
    h.dev->CreateShaderResourceView(v.color.tex,&srv,cpu);
    auto uavCpu=cpu;uavCpu.ptr+=stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R8_UNORM;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
    h.dev->CreateUnorderedAccessView(g_ofa_gray,nullptr,&uav,uavCpu);
    ID3D12DescriptorHeap* heaps[]{g_ofa_gray_heap};h.list->SetDescriptorHeaps(1,heaps);
    h.list->SetComputeRootSignature(g_gray_rs);h.list->SetPipelineState(g_gray_pso);
    const UINT sizes[]{width,height,g_ofa_w,g_ofa_h};h.list->SetComputeRoot32BitConstants(0,4,sizes,0);
    auto gpu=g_ofa_gray_heap->GetGPUDescriptorHandleForHeapStart();auto ugpu=gpu;ugpu.ptr+=stride;
    h.list->SetComputeRootDescriptorTable(1,gpu);h.list->SetComputeRootDescriptorTable(2,ugpu);
    h.list->Dispatch((g_ofa_w+7)/8,(g_ofa_h+7)/8,1);return true;
}
static bool VrCreate(VideoState &v,const vrgpu::Shared &p,vrgpu::Shared *s,HANDLE owner) {
    CloseNvofa();CloseOfaGray();
    ReleaseMultiPass();
    if(h.feature) { SafeReleaseFeature(h.feature); h.feature=nullptr; }
    ReleaseVideoTextures(v); v=VideoState{};
    VrControls(p);
    if(!CreateVideoResources(v,p.workWidth,p.workHeight,p.width,p.height,1)) return false;
    // Never silently fall back to an unexpectedly expensive full-size network.
    if((p.workWidth!=p.width||p.workHeight!=p.height)&&!v.nr_small) return false;
    NVSDK_NGX_Result result=NVSDK_NGX_Result_Fail;
    if(!CreateFeature(v.nr_small?v.nr_w:v.w,v.nr_small?v.nr_h:v.hgt,0,&result)) {
        s->result=g_create_result; return false;
    }
    std::vector<BYTE> neutral(static_cast<size_t>(p.width)*p.height*4,128);
    for(size_t i=3;i<neutral.size();i+=4) neutral[i]=255;
    std::vector<BYTE> motion(static_cast<size_t>(v.w)*v.hgt*4,0);
    if(!UploadVideoFrame(v,neutral.data(),motion.data(),false)) return false;
    if(!BeginCommands() || !VrGray(v) || !EndCommands())return false;
    // Warm up actual NR and Fast before advertising readiness; then explicitly
    // reset the first user's frame so neutral history cannot contaminate it.
    // Prepare measured Fast and its temporal shader even when initially OFF:
    // a later live ON must never compile a shader in the frame budget.
    g_fast_settings.enabled=true;
    for(int i=0;i<3;++i) {
        if(!VrAlive(s,owner)) return false;
        UINT64 submitted=0;
        if(!EnsureFastMultiPass(v.nr_small?v.nr_w:v.w,v.nr_small?v.nr_h:v.hgt)) return false;
        if(!RunNvofa(v,i==0,&submitted)||!VrFence(h.fence,submitted,s,owner))return false;
        if(!EvaluateVideo(v,i==0?1:0,&submitted)||!VrFence(h.fence,submitted,s,owner)) {
            s->result=g_last_eval_result; return false;
        }
    }
    VrControls(p);
    // Warm real sequential features before advertising Ready.
    if(!p.fast && p.passes>1)for(int i=0;i<3;++i){
        UINT64 submitted=0;if(!VrAlive(s,owner) || !EvaluateVideo(v,i==0?1:0,&submitted) || !VrFence(h.fence,submitted,s,owner))return false;
    }
    s->appliedPasses=g_pass_count;
    Log("[vr-gpu] eye %u warmup complete: source %ux%u NR %ux%u Fast %u target %u",
        p.eye,p.width,p.height,v.nr_small?v.nr_w:v.w,v.nr_small?v.nr_h:v.hgt,p.fast,p.target);
    return true;
}
static bool VrStage(VideoState &v,VrRelay &r,UINT64 n,vrgpu::Shared *s,HANDLE owner) {
    // Observe producer completion first, then enqueue the explicit GPU wait.
    // This prevents an abandoned producer from leaving a permanently blocked queue.
    if(!VrFence(r.inputFence,n,s,owner,2000)||FAILED(h.queue->Wait(r.inputFence,n))||!BeginCommands()) return false;
    D3D12_RESOURCE_BARRIER before[]={Transition(r.input,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(v.color.tex,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST)};
    h.list->ResourceBarrier(2,before); h.list->CopyResource(v.color.tex,r.input);
    D3D12_RESOURCE_BARRIER after[]={Transition(r.input,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON),
        Transition(v.color.tex,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    h.list->ResourceBarrier(2,after);
    if(!VrGray(v)){AbortCommands();return false;}
    return EndCommands()!=0;
}
static bool VrExport(VideoState &v,VrRelay &r,UINT64 n,vrgpu::Shared *s,HANDLE owner) {
    if(!BeginCommands()) return false;
    D3D12_RESOURCE_BARRIER before[]={Transition(v.output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(r.output,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST)};
    h.list->ResourceBarrier(2,before); h.list->CopyResource(r.output,v.output);
    D3D12_RESOURCE_BARRIER after[]={Transition(v.output,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(r.output,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON)};
    h.list->ResourceBarrier(2,after);
    if(!EndCommands()||FAILED(h.queue->Signal(r.outputFence,n))) return false;
    return VrFence(r.outputFence,n,s,owner);
}
}
static int RunVrGpu(const char *mappingName) {
    HANDLE mapping=OpenFileMappingA(FILE_MAP_ALL_ACCESS,FALSE,mappingName);
    if(!mapping) return 20;
    auto s=static_cast<vrgpu::Shared*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(vrgpu::Shared)));
    if(!s) { CloseHandle(mapping); return 21; }
    vrgpu::Shared p; memcpy(&p,s,sizeof(p));
    HANDLE owner=nullptr,requestEvent=nullptr,completedEvent=nullptr; int exitCode=22; VideoState v; VrRelay relay;
    auto finish=[&](bool failed) {
        if(failed && !s->result) s->result=0x80004005u;
        InterlockedExchange(&s->status,failed?vrgpu::Failed:vrgpu::Stopped);
        // Process exit retires abandoned GPU slots. Never publish completion on error.
        const bool drained=h.fence && h.fence->GetCompletedValue()!=UINT64_MAX &&
            (h.fence->GetCompletedValue()>=h.fence_value || WaitFenceValue(h.fence,h.fence_value,1000));
        if(drained) {
            CloseNvofa();CloseOfaGray();
            CleanupVideoNgx();
            ReleaseVideoTextures(v);
        }
        if(drained)relay.close();
        else{ // Keep outstanding GPU references alive until this process exits.
            relay.input=nullptr;relay.output=nullptr;relay.inputFence=nullptr;relay.outputFence=nullptr;
        }
        if(owner) CloseHandle(owner);if(requestEvent)CloseHandle(requestEvent);if(completedEvent)CloseHandle(completedEvent);
        UnmapViewOfFile(s); CloseHandle(mapping); return failed?exitCode:0;
    };
    if(!VrValid(p)) return finish(true);
    const std::string requestName=std::string(mappingName)+"-request";
    requestEvent=OpenEventA(SYNCHRONIZE,FALSE,requestName.c_str());
    if(!requestEvent){Log("[vr-gpu] request event missing");return finish(true);}
    // Optional for old protocol-v2 diagnostic clients. New drivers wait on this
    // event after the output fence AND completion metadata have been published.
    completedEvent=OpenEventA(EVENT_MODIFY_STATE,FALSE,(std::string(mappingName)+"-complete").c_str());
    g_vr_gpu_mode=true; g_video_mode=true; g_vr_adapter_luid={p.adapterLow,p.adapterHigh};
    char dir[MAX_PATH]={}; GetModuleFileNameA(nullptr,dir,MAX_PATH); if(char *slash=strrchr(dir,'\\')) *(slash+1)=0;
    sprintf_s(g_log_path,"%svr-gpu-%u-%lu.log",dir,p.eye,GetCurrentProcessId());
    Log("[vr-gpu] mapping %s LUID %08X:%08X",mappingName,p.adapterHigh,p.adapterLow);
    owner=OpenProcess(PROCESS_DUP_HANDLE|SYNCHRONIZE,FALSE,p.driverPid);
    if(!owner) return finish(true);
    wchar_t runtime[MAX_PATH]={}; GetModuleFileNameW(nullptr,runtime,MAX_PATH);
    if(wchar_t *slash=wcsrchr(runtime,L'\\')) *(slash+1)=0;
    wcscat_s(runtime,L"libraries\\nvngx_dlssnr.dll");
    if(GetFileAttributesW(runtime)==INVALID_FILE_ATTRIBUTES) { Log("[vr-gpu] bundled NR missing: %ls",runtime); return finish(true); }
    SetEnvironmentVariableW(L"NS_NR_DLL",runtime);
    SetEnvironmentVariableA("NS_NGX_VIA_CORE",nullptr); SetEnvironmentVariableA("NS_FORWARDER",nullptr);
    g_nr_direct=false; g_halo=0; g_sharpness=0; g_edge_smooth=0; g_colors_read=true; g_pass_count=1; g_pw_exposure=1;
    if(!InitDisguise()||!InitNgx()||!VrOpenRelay(owner,p,relay)||!VrCreate(v,p,s,owner)) return finish(true);
    if(!VrAlive(s,owner)) return finish(false);
    InterlockedExchange(&s->status,vrgpu::Ready);
    uint64_t generation=p.generation; LONG64 last=vrgpu::sequence(&s->completed); bool first=true;
    for(;;) {
        if(!VrAlive(s,owner)) return finish(false);
        const LONG64 n=vrgpu::sequence(&s->request);
        if(n==last) {WaitForSingleObject(requestEvent,100);continue;}
        if(n<=last||n<=0) { Log("[vr-gpu] nonmonotonic sequence"); return finish(true); }
        const auto start=std::chrono::steady_clock::now();
        memcpy(&p,s,sizeof(p)); // stable until driver retires this single slot
        if(!VrValid(p)||p.driverPid==0||p.adapterHigh!=g_vr_adapter_luid.HighPart||p.adapterLow!=g_vr_adapter_luid.LowPart)
            return finish(true);
        if(p.generation!=generation||p.workWidth!=v.w||p.workHeight!=v.hgt||p.width!=(v.upscale?v.full_w:v.w)||p.height!=(v.upscale?v.full_h:v.hgt)) {
            InterlockedExchange(&s->status,vrgpu::Starting);
            if(!VrFence(h.fence,h.fence_value,s,owner)||!VrOpenRelay(owner,p,relay)||!VrCreate(v,p,s,owner)) return finish(true);
            generation=p.generation; first=true; InterlockedExchange(&s->status,vrgpu::Ready);
        }
        VrControls(p);
        if(p.fast&&!EnsureFastMultiPass(v.nr_small?v.nr_w:v.w,v.nr_small?v.nr_h:v.hgt)) return finish(true);
        UINT64 eval=0;
        if(!VrStage(v,relay,n,s,owner))return finish(true);
        const auto staged=std::chrono::steady_clock::now();
        if(!RunNvofa(v,first||p.reset,&eval))return finish(true);
        const auto motionDone=std::chrono::steady_clock::now();
        if(!EvaluateVideo(v,(first||p.reset)?1:0,&eval)) {
            s->result=g_last_eval_result; return finish(true);
        }
        const auto evaluated=std::chrono::steady_clock::now();
        if(!VrExport(v,relay,n,s,owner)) return finish(!(!VrAlive(s,owner)));
        first=false; last=n; s->result=0;s->appliedPasses=g_pass_count;
        s->elapsedUs=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
        // Published only after the externally visible output fence really completed.
        InterlockedExchange64(&s->completed,n);
        if(completedEvent)SetEvent(completedEvent);
        if(n<=12||n%120==0) Log("[vr-gpu] completed %lld generation %llu %llu us Fast %u target %u passes=%u stage=%lld flow=%lld eval=%lld export=%lld",n,generation,s->elapsedUs,p.fast,p.target,g_pass_count,
            std::chrono::duration_cast<std::chrono::microseconds>(staged-start).count(),
            std::chrono::duration_cast<std::chrono::microseconds>(motionDone-staged).count(),
            std::chrono::duration_cast<std::chrono::microseconds>(evaluated-motionDone).count(),
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-evaluated).count());
    }
}
