// Motion-compensated NR/Fast color history, anchored to the source picture.
static const char kFastTemporalHlsl[] = R"hlsl(
Texture2D<float4> gRaw : register(t0);
Texture2D<float4> gSource : register(t1);
Texture2D<float2> gMv : register(t2);
Texture2D<float4> gHistory : register(t3);
Texture2D<float4> gSourceHistory : register(t4);
RWTexture2D<float4> gResult : register(u0);
RWTexture2D<float4> gNext : register(u1);
RWTexture2D<float4> gNextSource : register(u2);
cbuffer PC : register(b0) {
    uint gW; uint gH; uint gMvW; uint gMvH;
    uint gValid; float gWeight; float gLimit; uint gReserved;
};
float4 HistoryAt(float2 p) {
    int2 q = int2(floor(p)), hi = int2(gW - 1, gH - 1);
    float2 f = frac(p);
    return lerp(lerp(gHistory[clamp(q, int2(0,0), hi)],
                     gHistory[clamp(q+int2(1,0), int2(0,0), hi)], f.x),
                lerp(gHistory[clamp(q+int2(0,1), int2(0,0), hi)],
                     gHistory[clamp(q+int2(1,1), int2(0,0), hi)], f.x), f.y);
}
float3 SourceAt(float2 p) {
    int2 q=int2(floor(p)), hi=int2(gW-1,gH-1);float2 f=frac(p);
    return lerp(lerp(gSourceHistory[clamp(q,int2(0,0),hi)].rgb,
        gSourceHistory[clamp(q+int2(1,0),int2(0,0),hi)].rgb,f.x),
        lerp(gSourceHistory[clamp(q+int2(0,1),int2(0,0),hi)].rgb,
        gSourceHistory[clamp(q+int2(1,1),int2(0,0),hi)].rgb,f.x),f.y);
}
[numthreads(8,8,1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gW || id.y >= gH) return;
    int2 p = int2(id.xy);
    float4 raw = gRaw[p], current = gResult[p];
    float3 source=gSource[p].rgb;
    float3 anchor=source;
    float3 delta = current.rgb - anchor;
    float luma = dot(source, float3(.2126f,.7152f,.0722f));
    float2 mv = 0;
    bool finiteMotion = true;
    if (gMvW > 0 && gMvH > 0) {
        int2 q = clamp(int2((float2(p)+.5f)*float2(gMvW,gMvH)/float2(gW,gH)),
                       int2(0,0), int2(gMvW-1,gMvH-1));
        mv = gMv[q]; finiteMotion = all(isfinite(mv));
    }
    float2 previous = float2(p) + mv*float2(gW,gH)/float2(max(1u,gMvW),max(1u,gMvH));
    if (gValid != 0 && gWeight > 0 && finiteMotion &&
        all(previous >= 0) && all(previous <= float2(gW-1,gH-1))) {
        {
            float3 miss=abs(SourceAt(previous)-source);
            float best=max(miss.r,max(miss.g,miss.b));
            // Refine coarse/subpixel flow only in a tiny neighborhood. Optical
            // flow grid errors otherwise reject useful history on textured floors.
            if (best > .025f) {
                float2 candidateBase=previous,bestAt=previous;
                [unroll] for(int y=-1;y<=1;++y) [unroll] for(int x=-1;x<=1;++x) {
                    float2 candidate=candidateBase+float2(x,y)*1.5f;
                    if(all(candidate>=0) && all(candidate<=float2(gW-1,gH-1))) {
                        float3 error=abs(SourceAt(candidate)-source);
                        float score=max(error.r,max(error.g,error.b));
                        if(score < best) { best=score;bestAt=candidate; }
                    }
                }
                previous=bestAt;
            }
        }
        float4 held = HistoryAt(previous);
        float3 miss=abs(SourceAt(previous)-source);
        // Color validation also rejects equal-luminance scene replacements.
        float trust=1-smoothstep(.025f,.12f,max(miss.r,max(miss.g,miss.b)));
        float radius=.20f;
        float3 filtered=lerp(delta,clamp(held.rgb,delta-radius,delta+radius),gWeight*trust);
        float3 added=source+filtered-raw.rgb;
        added=clamp(added,-gLimit,gLimit);
        current.rgb=raw.rgb+clamp(added,-.75f*raw.rgb,.75f*(1-raw.rgb));
        delta=current.rgb-anchor;
    }
    gResult[p] = float4(current.rgb,raw.a);
    gNext[p] = float4(delta,luma);
    gNextSource[p]=float4(source,1);
}
)hlsl";

static ID3D12RootSignature *g_fast_temporal_rs = nullptr;
static ID3D12PipelineState *g_fast_temporal_pso = nullptr;
static ID3D12DescriptorHeap *g_fast_temporal_heap = nullptr;
static ID3D12Resource *g_fast_history[2] = {};
static ID3D12Resource *g_fast_source_history[2] = {};
static bool g_fast_history_valid = false, g_fast_temporal_failed = false;
static unsigned g_fast_history_ping = 0;
static UINT64 g_fast_history_revision = 0;
static bool FastTemporalAvailable() { return g_fast_history[0] && g_fast_source_history[0] && g_fast_temporal_pso; }

static void InvalidateFastTemporal() {
    g_fast_history_valid = false;
    g_fast_temporal_failed = false; // retry after an explicit OFF/ON transition
}
static void ReleaseFastTemporal() {
    for (auto &r : g_fast_history) { if (r) r->Release(); r = nullptr; }
    for (auto &r : g_fast_source_history) { if (r) r->Release(); r = nullptr; }
    if (g_fast_temporal_heap) g_fast_temporal_heap->Release();
    if (g_fast_temporal_pso) g_fast_temporal_pso->Release();
    if (g_fast_temporal_rs) g_fast_temporal_rs->Release();
    g_fast_temporal_heap = nullptr; g_fast_temporal_pso = nullptr; g_fast_temporal_rs = nullptr;
    g_fast_history_valid = false; g_fast_temporal_failed = false; g_fast_history_ping = 0;
}
static bool EnsureFastTemporal(UINT w, UINT height) {
    if (g_fast_history[0]) return true;
    if (g_fast_temporal_failed) return false;
    ID3DBlob *code = nullptr, *errors = nullptr, *root = nullptr;
    HRESULT hr = D3DCompile(kFastTemporalHlsl, sizeof(kFastTemporalHlsl)-1,
        "fast-temporal.hlsl", nullptr, nullptr, "CSMain", "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) Log("[fast-multipass] temporal shader failed: %s",
                       errors ? static_cast<const char *>(errors->GetBufferPointer()) : "no diagnostic");
    if (errors) { errors->Release(); errors = nullptr; }
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; ranges[0].NumDescriptors = 5;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV; ranges[1].NumDescriptors = 3;
    ranges[1].OffsetInDescriptorsFromTableStart = 5;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 8;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2; params[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC desc = {}; desc.NumParameters = 2; desc.pParameters = params;
    if (SUCCEEDED(hr)) hr = D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&root,&errors);
    if (errors) errors->Release();
    if (SUCCEEDED(hr)) hr = h.dev->CreateRootSignature(0,root->GetBufferPointer(),root->GetBufferSize(),IID_PPV_ARGS(&g_fast_temporal_rs));
    if (root) root->Release();
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {}; pd.pRootSignature = g_fast_temporal_rs;
        pd.CS = {code->GetBufferPointer(),code->GetBufferSize()};
        hr = h.dev->CreateComputePipelineState(&pd,IID_PPV_ARGS(&g_fast_temporal_pso));
    }
    if (code) code->Release();
    if (SUCCEEDED(hr)) {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 8 * Host::kFrames; hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = h.dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&g_fast_temporal_heap));
    }
    D3D12_HEAP_PROPERTIES hp = {}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = height; td.DepthOrArraySize = td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    for (auto &r : g_fast_history) if (SUCCEEDED(hr))
        hr = h.dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&r));
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    for (auto &r : g_fast_source_history) if (SUCCEEDED(hr))
        hr=h.dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&r));
    if (FAILED(hr)) {
        ReleaseFastTemporal(); g_fast_temporal_failed = true;
        Log("[fast-multipass] stabilization unavailable 0x%08X; using selected spatial Fast",hr);
        return false;
    }
    Log("[fast-multipass] optional residual history ready %ux%u",w,height);
    return true;
}

static void RecordFastTemporal(ID3D12Resource *original, ID3D12Resource *result,
                               UINT w, UINT height, ID3D12Resource *motion, UINT mw, UINT mh, bool reset) {
    if (!g_fast_history[0] || !g_fast_temporal_pso) return;
    const bool valid = g_fast_history_valid && !reset && g_fast_history_revision == g_fast_settings_revision;
    const unsigned previous = g_fast_history_ping, next = 1-previous;
    auto barrier = Transition(g_fast_history[next],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    h.list->ResourceBarrier(1,&barrier);
    barrier=Transition(g_fast_source_history[next],D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    h.list->ResourceBarrier(1,&barrier);
    const UINT stride = h.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = g_fast_temporal_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(h.frame_slot)*8*stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {}; sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    h.dev->CreateShaderResourceView(g_fast_copy,&sd,cpu); cpu.ptr += stride;
    h.dev->CreateShaderResourceView(original,&sd,cpu); cpu.ptr += stride;
    sd.Format = DXGI_FORMAT_R16G16_FLOAT;
    h.dev->CreateShaderResourceView(motion,&sd,cpu); cpu.ptr += stride;
    sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    h.dev->CreateShaderResourceView(g_fast_history[previous],&sd,cpu); cpu.ptr += stride;
    sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    h.dev->CreateShaderResourceView(g_fast_source_history[previous],&sd,cpu);cpu.ptr+=stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {}; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    h.dev->CreateUnorderedAccessView(result,nullptr,&ud,cpu); cpu.ptr += stride;
    ud.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    h.dev->CreateUnorderedAccessView(g_fast_history[next],nullptr,&ud,cpu);
    cpu.ptr+=stride;ud.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    h.dev->CreateUnorderedAccessView(g_fast_source_history[next],nullptr,&ud,cpu);
    UINT32 constants[8] = {w,height,motion ? mw : 0u,motion ? mh : 0u,valid ? 1u : 0u,0,0,0};
    const float limit = .04f*FastMultiPassStrength();
    memcpy(&constants[5],&g_fast_settings.stability,sizeof(float)); memcpy(&constants[6],&limit,sizeof(float));
    h.list->SetDescriptorHeaps(1,&g_fast_temporal_heap); h.list->SetComputeRootSignature(g_fast_temporal_rs);
    h.list->SetPipelineState(g_fast_temporal_pso); h.list->SetComputeRoot32BitConstants(0,8,constants,0);
    auto gpu = g_fast_temporal_heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(h.frame_slot)*8*stride;
    h.list->SetComputeRootDescriptorTable(1,gpu); h.list->Dispatch((w+7)/8,(height+7)/8,1);
    D3D12_RESOURCE_BARRIER done = {}; done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; done.UAV.pResource = result;
    h.list->ResourceBarrier(1,&done);
    barrier = Transition(g_fast_history[next],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    h.list->ResourceBarrier(1,&barrier);
    barrier=Transition(g_fast_source_history[next],D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    h.list->ResourceBarrier(1,&barrier);
    g_fast_history_ping = next; g_fast_history_valid = true; g_fast_history_revision = g_fast_settings_revision;
}
