#pragma once
#include "live_gpu.h"
static std::atomic_bool markerRequested{false};
static uint64_t perfNowUs(){return vrlive::monotonicUs();}
class MarkerSession {
    using Direct=vr::IVRDriverDirectModeComponent;
    using Eyes=std::array<Direct::SubmitLayerPerEye_t,2>;
    Direct* original;std::filesystem::path root;
    std::mutex mutex;std::vector<Eyes> layers;
    std::shared_ptr<vrlive::Control> control=std::make_shared<vrlive::Control>();
    std::shared_ptr<vrlive::Generation> gpu;
    std::array<uint64_t,2> desiredSources{};uint64_t desiredGeneration=0,builtGeneration=0;
    std::array<Direct::SwapTextureSet_t,2> sets{};std::array<uint32_t,2> nextIndices{};
    uint64_t outputGeneration=0,outputSyncHandle=0;bool resetNext=true;
    framecapture::ComPtr<ID3D11Texture2D> outputSyncTexture;
    vrlive::PairDecision budget;
    std::atomic_bool stopping{false},applied{false},fastApplied{false},latched{false};std::thread broker;
    std::atomic_uint64_t processed{0},skipped{0},leftUs{0},rightUs{0};
    std::atomic_uint64_t pairUs{0},pairBudgetUs{vrlive::WatchdogUs},attempts{0};
    std::atomic_uint64_t syncMisses{0},routeMisses{0};
    int previousRoute=-1;uint64_t routeTransitions=0,noLayerPresents=0;
    std::string pausedReason="NR paused. Turn DLSS 5 off/on to retry.";
    std::atomic_uint32_t workWidth{0},workHeight{0};
    std::atomic_uint32_t actualPasses{0};
    std::atomic_uint32_t leftPid{0},rightPid{0};
    std::atomic_uint64_t lastSource{0};
    std::mutex stateMutex;std::string state="off",message="VR processing off";
    void status(const char* value,const std::string& why){std::lock_guard<std::mutex> lock(stateMutex);state=value;message=why;}
    static void waitProgress(vrlive::Generation& g){
        HANDLE events[]{g.eyes[0].completedEvent,g.eyes[1].completedEvent,g.eyes[0].process,g.eyes[1].process};
        WaitForMultipleObjects(4,events,FALSE,10);
    }
    void clearOutputs(bool callVendor=true){
        // All vendor component methods are serialized with the original render callback.
        for(auto& set:sets){if(callVendor && set.rSharedTextureHandles[0])original->DestroySwapTextureSet(set.rSharedTextureHandles[0]);set={};}
        outputSyncTexture.Reset();outputSyncHandle=outputGeneration=0;nextIndices={};
    }
    bool allocateOutputs(vrlive::Generation& g){
        if(outputGeneration==g.id)return true;clearOutputs();
        for(unsigned i=0;i<2;++i){const auto& d=g.desc[i];Direct::SwapTextureSetDesc_t vd{d.Width,d.Height,UINT(d.Format),1};original->CreateSwapTextureSet(GetCurrentProcessId(),&vd,&sets[i]);
            if(sets[i].unTextureFlags || !sets[i].rSharedTextureHandles[0] || !sets[i].rSharedTextureHandles[1] || !sets[i].rSharedTextureHandles[2]){clearOutputs();return false;}}
        D3D11_TEXTURE2D_DESC d{};d.Width=d.Height=8;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R8G8B8A8_UNORM;d.Usage=D3D11_USAGE_DEFAULT;d.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;d.MiscFlags=D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        framecapture::ComPtr<IDXGIResource> resource;HANDLE handle=nullptr;
        if(FAILED(g.device->CreateTexture2D(&d,nullptr,&outputSyncTexture)) || FAILED(outputSyncTexture.As(&resource)) || FAILED(resource->GetSharedHandle(&handle))){clearOutputs();return false;}
        outputSyncHandle=(uintptr_t)handle;outputGeneration=g.id;return true;
    }
    void publishStatus(){
        std::string s,m;{std::lock_guard<std::mutex> lock(stateMutex);s=state;m=message;}
        // Messages are internal fixed strings/HRESULT text. Escape to keep the JSON valid.
        std::string escaped;for(char c:m){if(c=='"' || c=='\\')escaped+='\\';if((unsigned char)c>=32)escaped+=c;}
        std::ostringstream json;json<<"{\"version\":1,\"updated_unix_ms\":"<<vrlive::unixMs()<<",\"driver_pid\":"<<GetCurrentProcessId()<<",\"state\":\""<<s<<"\",\"message\":\""<<escaped<<"\",\"nr_applied\":"<<(applied.load()?"true":"false")<<",\"fast_applied\":"<<(fastApplied.load()?"true":"false")<<",\"processed\":"<<processed.load()<<",\"skipped\":"<<skipped.load()<<",\"left_us\":"<<leftUs.load()<<",\"right_us\":"<<rightUs.load()<<",\"work_width\":"<<workWidth.load()<<",\"work_height\":"<<workHeight.load()<<",\"left_pid\":"<<leftPid.load()<<",\"right_pid\":"<<rightPid.load()<<"}\n";
        auto output=json.str();output.pop_back();output.pop_back();
        output+=",\"pair_us\":"+std::to_string(pairUs.load())+",\"pair_budget_us\":"+std::to_string(pairBudgetUs.load())+",\"sync_misses\":"+std::to_string(syncMisses.load())+",\"route_misses\":"+std::to_string(routeMisses.load())+"}\n";
        output.pop_back();output.pop_back();output+=",\"applied_passes\":"+std::to_string(applied?actualPasses.load():0)+"}\n";
        vrlive::atomicWrite(root/L"status.json",output);
    }
    void runBroker(){
        std::vector<std::shared_ptr<vrlive::Generation>> retired;uint64_t lastWrite=0;bool priorNR=false;float priorScale=.25f;unsigned priorPasses=1;
        while(!stopping){
            const auto now=vrlive::unixMs();auto cfg=std::make_shared<vrlive::Control>(vrlive::readControl(root/L"control.json",now));
            std::atomic_store(&control,cfg);markerRequested=cfg->enabled(now);
            pairBudgetUs=vrlive::WatchdogUs;
            std::array<uint64_t,2> sources{};uint64_t wanted=0;std::shared_ptr<vrlive::Generation> current;
            {std::unique_lock<std::mutex> lock(mutex,std::try_to_lock);
                if(!lock.owns_lock()){
                    // Keep heartbeat/OFF publication alive while the render
                    // callback waits for a slow producer or NR completion.
                    if(!cfg->enabled(now)){applied=false;fastApplied=false;status("off","VR processing off or control lease expired");}
                    if(now-lastWrite>=500){publishStatus();lastWrite=now;}
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));continue;
                }
                if(!cfg->nr){budget.observe(false);latched=false;resetNext=true;}
                // Reactivation gets freshly warmed eye sessions; retaining idle
                // sessions repeatedly exceeded the budget in recovery tests.
                if(cfg->scale!=priorScale || cfg->effectivePasses()!=priorPasses || (!priorNR && cfg->nr)){++desiredGeneration;budget.observe(false);latched=false;resetNext=true;}
                priorNR=cfg->nr;priorScale=cfg->scale;priorPasses=cfg->effectivePasses();sources=desiredSources;wanted=desiredGeneration;current=gpu;
                // OFF / app Stop / expired heartbeat releases our model resources.
                // Vendor output sets are retired later on its render callback.
                if(!cfg->enabled(now) && gpu){gpu->stop();retired.push_back(gpu);gpu.reset();current.reset();}
            }
            if(!cfg->enabled(now)){applied=false;fastApplied=false;status(cfg->valid?"off":"error",cfg->valid?"VR processing off or control lease expired":"Missing or invalid control.json; original passthrough");}
            else if(current && wanted==builtGeneration && (!current->error.empty() || current->eyes[0].exited() || current->eyes[1].exited() || (current->eyes[0].shared && vrgpu::state(&current->eyes[0].shared->status)==vrgpu::Failed) || (current->eyes[1].shared && vrgpu::state(&current->eyes[1].shared->status)==vrgpu::Failed))){applied=false;fastApplied=false;current->stop();status("error",current->error.empty()?"An eye worker exited or NR failed; both eyes use original frames":current->error);}
            else if(latched){applied=false;fastApplied=false;std::string why;{std::lock_guard<std::mutex> lock(mutex);why=pausedReason;}status("error",why);}
            else if(!sources[0] || !sources[1] || (lastSource && GetTickCount64()-lastSource.load()>1500)){applied=false;fastApplied=false;status("waiting_source","Waiting for live SteamVR source frames");}
            else if(wanted!=builtGeneration){
                applied=false;fastApplied=false;status("starting","Creating GPU relays and warming both eye workers");
                auto prepared=std::make_shared<vrlive::Generation>();const bool ok=prepared->build(root,sources,cfg->scale,wanted,unsigned(cfg->passes),cfg->fast);
                leftPid=prepared->eyes[0].process?GetProcessId(prepared->eyes[0].process):0;rightPid=prepared->eyes[1].process?GetProcessId(prepared->eyes[1].process):0;
                {std::lock_guard<std::mutex> lock(mutex);builtGeneration=wanted;if(gpu){gpu->stop();retired.push_back(gpu);}gpu=prepared;}
                if(!ok)status("error",prepared->error);
            }else if(current){
                if(!current->error.empty())status("error",current->error);
                else if(current->eyes[0].exited() || current->eyes[1].exited() || vrgpu::state(&current->eyes[0].shared->status)==vrgpu::Failed || vrgpu::state(&current->eyes[1].shared->status)==vrgpu::Failed){
                    applied=false;fastApplied=false;current->stop();status("error","An eye worker exited or NR failed; both eyes use original frames");
                }else if(!current->ready())status("starting","Both NR workers must finish warmup before processing");
                else if(!applied)status("ready","Both eye workers ready; waiting for matching current-frame completion");
            }
            for(auto it=retired.begin();it!=retired.end();){if(it->use_count()==1 && (*it)->pollStopped()){(*it)->join();it=retired.erase(it);}else ++it;}
            if(now-lastWrite>=500){publishStatus();lastWrite=now;}
            for(unsigned i=0;i<10 && !stopping;++i)std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        {std::lock_guard<std::mutex> lock(mutex);if(gpu){gpu->stop();retired.push_back(gpu);gpu.reset();}}
        for(auto& g:retired)g->join();retired.clear();applied=false;fastApplied=false;status("off","Driver cleanup completed");publishStatus();
    }
    bool sourceMatches(vrlive::Generation& g,const std::array<framecapture::ComPtr<ID3D11Texture2D>,2>& source){
        for(unsigned i=0;i<2;++i){D3D11_TEXTURE2D_DESC d{};source[i]->GetDesc(&d);const auto& old=g.desc[i];if(d.Width!=old.Width || d.Height!=old.Height || d.Format!=old.Format || d.MipLevels!=1 || d.ArraySize!=1 || d.SampleDesc.Count!=1)return false;}return true;
    }
public:
    explicit MarkerSession(Direct* p):original(p){
        wchar_t path[32768]{};GetModuleFileNameW(ownModule,path,32768);
#ifdef OBSERVER_SELF_TEST
        root=std::filesystem::path(path).parent_path();
#else
        root=std::filesystem::path(path).parent_path().parent_path().parent_path();
#endif
        layers.reserve(32);broker=std::thread([this]{runBroker();});
    }
    void shutdown(){stopping=true;if(broker.joinable())broker.join();std::lock_guard<std::mutex> lock(mutex);layers.clear();
        // Vendor may already have destroyed the HMD during provider cleanup.
        // Its teardown/process exit owns the remaining vendor texture sets.
        clearOutputs(false);}
    void invalidateGpuCache(){std::lock_guard<std::mutex> lock(mutex);if(gpu)gpu->cache.clear();desiredSources={};resetNext=true;++desiredGeneration;budget.observe(false);latched=false;}
    bool queue(const Direct::SubmitLayerPerEye_t(&pair)[2]){
        std::lock_guard<std::mutex> lock(mutex);if(stopping)return false;auto cfg=std::atomic_load(&control);
        const auto sourceNow=GetTickCount64();if(lastSource && sourceNow-lastSource.load()>1500)resetNext=true;lastSource=sourceNow;
        if(layers.empty() && !cfg->enabled(vrlive::unixMs()))return false;
        if(layers.size()>=32){for(const auto& l:layers)original->SubmitLayer(*reinterpret_cast<const Direct::SubmitLayerPerEye_t(*)[2]>(l.data()));layers.clear();return false;}
        Eyes copy{pair[0],pair[1]};layers.push_back(copy);if(!desiredSources[0]){desiredSources={pair[0].hTexture,pair[1].hTexture};++desiredGeneration;}return true;
    }
    bool present(uint64_t sync){
        std::lock_guard<std::mutex> lock(mutex);auto cfg=std::atomic_load(&control);
        if(layers.empty()){if(cfg->enabled(vrlive::unixMs()) && ++noLayerPresents<=3)record("nr_present_without_layer","sync=%llu previous_route=%d",(unsigned long long)sync,previousRoute);if(!cfg->enabled(vrlive::unixMs()) && outputGeneration)clearOutputs();return false;}
        bool success=false;framemarker::InputLease lease;auto g=gpu;
        const uint64_t limit=vrlive::WatchdogUs,frameStart=vrlive::monotonicUs();
        auto stillRequested=[&](){auto current=std::atomic_load(&control);return !stopping && current->enabled(vrlive::unixMs()) && current->scale==cfg->scale && current->fast==cfg->fast && current->effectivePasses()==cfg->effectivePasses();};
        const bool configurationMatches=!g || (g->scale==cfg->scale && g->configuredPasses==cfg->effectivePasses());
        if(!configurationMatches){budget.observe(false);latched=false;resetNext=true;}
        const char* reject="workers_not_ready";HRESULT acquire=E_PENDING;
        if(!cfg->nr){budget.observe(false);latched=false;resetNext=true;}
        if(stillRequested() && !budget.latched && configurationMatches && g && g->ready() && g->id==desiredGeneration){
            while(!g->available() && g->ready() && stillRequested() && vrlive::monotonicUs()-frameStart<limit)waitProgress(*g);
        }
        if(cfg->enabled(vrlive::unixMs()) && !budget.latched && configurationMatches && g && g->ready() && g->id==desiredGeneration && g->available()){
            HRESULT hr=S_OK;std::array<framecapture::ComPtr<ID3D11Texture2D>,2> sources,targets;
            auto syncTexture=g->cache.get(g->device.Get(),sync,hr);framecapture::ComPtr<IDXGIKeyedMutex> key;
            bool opened=SUCCEEDED(hr) && SUCCEEDED(syncTexture.As(&key));
            reject="shared_texture_open_failed";
            for(unsigned i=0;i<2 && opened;++i){sources[i]=g->cache.get(g->device.Get(),layers.front()[i].hTexture,hr);opened=SUCCEEDED(hr);}
            if(opened && !sourceMatches(*g,sources)){desiredSources={layers.front()[0].hTexture,layers.front()[1].hTexture};++desiredGeneration;budget.observe(false);latched=false;opened=false;reject="source_changed";status("starting","Source dimensions or format changed; rebuilding both eyes");}
            if(opened && !allocateOutputs(*g)){opened=false;status("error","Vendor output texture allocation failed");}
            for(unsigned i=0;i<2 && opened;++i){if(nextIndices[i]>=3){opened=false;break;}targets[i]=g->cache.get(g->device.Get(),sets[i].rSharedTextureHandles[nextIndices[i]],hr);opened=SUCCEEDED(hr);}
            // Wait for real producer readiness; 5 ms was a performance cutoff,
            // not a correctness requirement. Only cancellation/device failure
            // or the long recovery watchdog aborts this current frame.
            const auto pairStart=frameStart;
            if(opened){
                while(stillRequested() && vrlive::monotonicUs()-pairStart<limit){acquire=g->device->GetDeviceRemovedReason();if(FAILED(acquire))break;acquire=key->AcquireSync(0,10);if(acquire!=WAIT_TIMEOUT)break;}
                reject=acquire==S_OK?"NR_watchdog_or_worker_failed":"input_watchdog_or_sync_failed";
            }
            if(opened && acquire==S_OK){
                lease.mutex=key;const uint64_t sequence=++g->sequence;
                const bool parameterReset=!g->haveControl || cfg->style!=g->lastControl.style || cfg->intensity!=g->lastControl.intensity || cfg->localTone!=g->lastControl.localTone || cfg->localStructure!=g->lastControl.localStructure || cfg->skinStructure!=g->lastControl.skinStructure;
                for(unsigned i=0;i<2;++i){auto& e=g->eyes[i];g->context->CopyResource(e.input.Get(),sources[i].Get());hr=g->context4->Signal(e.inputFence.Get(),sequence);if(FAILED(hr))break;}
                // Independent completion for the source snapshot, even if a shared Signal fails.
                const auto snapshotFence=++g->copySequence;
                const auto snapshotSignal=g->context4->Signal(g->copyFence.Get(),snapshotFence);
                if(FAILED(snapshotSignal))hr=snapshotSignal;
                g->context->Flush();const auto snapshot=pairStart;
                if(SUCCEEDED(hr))for(auto& e:g->eyes){auto& s=*e.shared;s.reset=(resetNext || parameterReset || !e.pending)?1:0;s.fast=cfg->fast;s.passes=cfg->passes;s.target=cfg->target;s.style=cfg->style;s.stability=cfg->stability;s.intensity=cfg->intensity;s.localTone=cfg->localTone;s.localStructure=cfg->localStructure;s.skinStructure=cfg->skinStructure;s.result=0;e.pending=sequence;InterlockedExchange64(&s.request,LONG64(sequence));SetEvent(e.requestEvent);}
                if(SUCCEEDED(hr)){g->lastControl=*cfg;g->haveControl=true;}
                bool complete=false;
                while(SUCCEEDED(hr) && stillRequested() && vrlive::monotonicUs()-snapshot<=limit){
                    auto& l=g->eyes[0];auto& r=g->eyes[1];
                    hr=g->device->GetDeviceRemovedReason();if(FAILED(hr)){reject="GPU_device_removed";break;}
                    complete=budget.accept(sequence,uint64_t(vrgpu::sequence(&l.shared->completed)),uint64_t(vrgpu::sequence(&r.shared->completed)),l.outputFence->GetCompletedValue(),r.outputFence->GetCompletedValue(),l.shared->result==0 && !l.exited() && vrgpu::state(&l.shared->status)==vrgpu::Ready,r.shared->result==0 && !r.exited() && vrgpu::state(&r.shared->status)==vrgpu::Ready,vrlive::monotonicUs()-snapshot);
                    if(complete && (l.shared->appliedPasses!=g->configuredPasses || r.shared->appliedPasses!=g->configuredPasses)){complete=false;hr=E_FAIL;reject="NR_pass_count_mismatch";}
                    if(complete || FAILED(hr) || l.exited() || r.exited() || vrgpu::state(&l.shared->status)==vrgpu::Failed || vrgpu::state(&r.shared->status)==vrgpu::Failed)break;waitProgress(*g);
                }
                pairUs=vrlive::monotonicUs()-snapshot;
                // Record attempted timing even when the result is rejected.
                leftUs=g->eyes[0].shared->elapsedUs;rightUs=g->eyes[1].shared->elapsedUs;
                workWidth=g->eyes[0].shared->workWidth;workHeight=g->eyes[0].shared->workHeight;
                const auto attempt=++attempts;
                if(attempt<=12 || attempt%120==0)record("nr_pair","attempt=%llu complete=%d elapsed_us=%llu limit_us=%llu left_completed=%lld right_completed=%lld requested=%llu",
                    (unsigned long long)attempt,int(complete),(unsigned long long)pairUs.load(),(unsigned long long)limit,
                    vrgpu::sequence(&g->eyes[0].shared->completed),vrgpu::sequence(&g->eyes[1].shared->completed),(unsigned long long)sequence);
                // Input copies must finish before releasing producer ownership, including timeout fallback.
                // This exceptional safety wait can outlast the NR budget under a GPU/device stall.
                while(SUCCEEDED(snapshotSignal) && g->copyFence->GetCompletedValue()!=UINT64_MAX && g->copyFence->GetCompletedValue()<snapshotFence){
                    const auto removed=g->device->GetDeviceRemovedReason();if(FAILED(removed)){hr=removed;break;}SwitchToThread();
                }
                if(g->copyFence->GetCompletedValue()==UINT64_MAX || FAILED(g->device->GetDeviceRemovedReason())){hr=DXGI_ERROR_DEVICE_REMOVED;complete=false;g->stop();status("error","GPU device removed; original passthrough");}
                if(complete && stillRequested()){
                    for(unsigned i=0;i<2;++i)g->context->CopyResource(targets[i].Get(),g->eyes[i].output.Get());
                    const auto copy=++g->copySequence;hr=g->context4->Signal(g->copyFence.Get(),copy);g->context->Flush();
                    while(SUCCEEDED(hr) && g->copyFence->GetCompletedValue()<copy){hr=g->device->GetDeviceRemovedReason();if(FAILED(hr))break;SwitchToThread();}
                    success=SUCCEEDED(hr);
                    if(success){for(unsigned i=0;i<2;++i)layers.front()[i].hTexture=sets[i].rSharedTextureHandles[nextIndices[i]];budget.hit();actualPasses=g->eyes[0].shared->appliedPasses;resetNext=false;++processed;leftUs=g->eyes[0].shared->elapsedUs;rightUs=g->eyes[1].shared->elapsedUs;workWidth=g->eyes[0].shared->workWidth;workHeight=g->eyes[0].shared->workHeight;applied=true;fastApplied=cfg->fast;status("applied","DLSS 5 processed both eyes for the current source frame");}
                }
                if(!success)lease.release();
            }
        }else if(g && g->ready() && !g->available())reject="previous_pair_still_pending";
        if(!success && stillRequested() && !budget.latched){
            ++skipped;
            // Warmup is expected. Every unexpected raw frame after successful
            // delivery, including ownership/open failures, pauses NR once.
            if(configurationMatches && ((g && g->ready() && g->id==desiredGeneration) || budget.delivered)){
                ++routeMisses;if(acquire!=S_OK && std::string(reject)=="input_watchdog_or_sync_failed")++syncMisses;
                record("nr_rejected","reason=%s acquire=0x%08lx generation=%llu layers=%zu had_nr=%d",reject,(unsigned long)acquire,(unsigned long long)desiredGeneration,layers.size(),int(budget.delivered));
                budget.miss();budget.latched=true;latched=true;
                pausedReason=std::string("NR stopped after a GPU/worker error or prolonged wait: ")+reject+". Both eyes use live original frames. Turn DLSS 5 off/on to retry.";status("error",pausedReason);
            }
        }
        if(!success){applied=false;fastApplied=false;resetNext=true;}
        const int route=success?1:0;
        if(previousRoute!=route){++routeTransitions;record("nr_route","route=%s transitions=%llu layers=%zu nr_requested=%d reason=%s",success?"nr":"original",(unsigned long long)routeTransitions,layers.size(),int(cfg->nr),success?"current_pair":reject);previousRoute=route;}
        if(success && processed<=3)for(size_t i=0;i<layers.size();++i)record("nr_layer","index=%zu processed=%d left=%llu right=%llu",i,int(i==0),(unsigned long long)layers[i][0].hTexture,(unsigned long long)layers[i][1].hTexture);
        for(const auto& layer:layers)original->SubmitLayer(*reinterpret_cast<const Direct::SubmitLayerPerEye_t(*)[2]>(layer.data()));
        original->Present(success?outputSyncHandle:sync);
        const auto released=lease.release();if(success && released!=S_OK){applied=false;fastApplied=false;status("error","Input ownership release failed");}
        if(success){uint64_t handles[2]{layers.front()[0].hTexture,layers.front()[1].hTexture};uint32_t indices[2]{};original->GetNextSwapTextureSetIndex(handles,&indices);nextIndices={indices[0],indices[1]};if(indices[0]>=3 || indices[1]>=3){budget.latched=true;latched=true;status("error","Vendor returned invalid output index");}}
        layers.clear();return true;
    }
};
