#include "fast_multipass_shader.h"
#include "fast_multipass_settings.h"
#include "fast_reference_style0.h"
#include "fast_reference_style1.h"
#include "fast_reference_style2.h"
#include <shlobj.h>

static FastMultiPassSettings g_fast_settings;
static ULONGLONG g_fast_next_poll = 0;
static FILETIME g_fast_settings_stamp = {};
static UINT64 g_fast_settings_revision = 0;

// A small Manager-owned settings file is shared directly with the worker.
// No controller protocol changes, process restart or environment variables.
static void UpdateFastMultiPassSettings(const wchar_t *testPath = nullptr)
{
    const ULONGLONG now = GetTickCount64();
    if (now < g_fast_next_poll) return;
    g_fast_next_poll = now + 100;
    static const std::wstring settingsPath = [] {
        PWSTR local = nullptr;
        std::wstring path;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
            path = local;
            path += L"\\DLSS5Manager\\Screen\\fast-multipass.json";
            CoTaskMemFree(local);
        }
        return path;
    }();
    const wchar_t *path = testPath ? testPath : settingsPath.c_str();
    WIN32_FILE_ATTRIBUTE_DATA attr = {};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attr)) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) {
            if (g_fast_settings.enabled || g_fast_settings.target != 2 ||
                g_fast_settings.stability != 0.9f) ++g_fast_settings_revision;
            g_fast_settings = {}; g_fast_settings_stamp = {};
        }
        return;
    }
    if (CompareFileTime(&attr.ftLastWriteTime, &g_fast_settings_stamp) == 0) return;
    if (attr.nFileSizeHigh || attr.nFileSizeLow == 0 || attr.nFileSizeLow > 2048) return;
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char text[2049] = {}; DWORD bytes = 0;
    const bool read = ReadFile(file, text, attr.nFileSizeLow, &bytes, nullptr) && bytes == attr.nFileSizeLow;
    CloseHandle(file);
    if (!read) return;
    g_fast_settings_stamp = attr.ftLastWriteTime;
    FastMultiPassSettings next;
    if (!ParseFastMultiPassSettings(text, next)) {
        Log("[fast-multipass] invalid UI settings; retaining last valid values"); return;
    }
    if (next.enabled != g_fast_settings.enabled || next.target != g_fast_settings.target ||
        next.stability != g_fast_settings.stability) {
        ++g_fast_settings_revision;
        Log("[fast-multipass] measured matching: %s, target %u passes, NR/Fast stabilization %.2f",
            next.enabled ? "ON" : "OFF", next.target, next.stability);
    }
    g_fast_settings = next;
}

static bool FastMultiPassRequested() { return g_fast_settings.enabled; }
static float FastMultiPassStrength() { return float(g_fast_settings.target); }

static bool FastMultiPassSettingsChanged()
{
    UpdateFastMultiPassSettings();
    static UINT64 last = 0;
    const bool changed = last != g_fast_settings_revision;
    last = g_fast_settings_revision;
    return changed;
}

static ID3D12RootSignature *g_fast_rs = nullptr;
static ID3D12PipelineState *g_fast_pso = nullptr;
static ID3D12DescriptorHeap *g_fast_heap = nullptr;
static ID3D12Resource *g_fast_copy = nullptr; // immutable Y1 during dispatch
static bool g_fast_failed = false;
static void ReleaseFastTemporal();
static bool EnsureFastTemporal(UINT w, UINT height);
static bool FastTemporalAvailable();
static void InvalidateFastTemporal();
static void RecordFastTemporal(ID3D12Resource *original, ID3D12Resource *result,
                               UINT w, UINT height, ID3D12Resource *motion, UINT mw, UINT mh, bool reset);

// Called only after the worker has drained pending GPU work, also on resize.
static void ReleaseFastMultiPass()
{
    ReleaseFastTemporal();
    if (g_fast_copy) { g_fast_copy->Release(); g_fast_copy = nullptr; }
    if (g_fast_heap) { g_fast_heap->Release(); g_fast_heap = nullptr; }
    if (g_fast_pso) { g_fast_pso->Release(); g_fast_pso = nullptr; }
    if (g_fast_rs) { g_fast_rs->Release(); g_fast_rs = nullptr; }
    g_fast_failed = false;
}

static bool EnsureFastMultiPass(UINT w, UINT height)
{
    if (!FastMultiPassRequested() || g_fast_failed) {
        InvalidateFastTemporal(); return false;
    }
    if (g_fast_copy && g_fast_copy->GetDesc().Width == w && g_fast_copy->GetDesc().Height == height)
    {
        EnsureFastTemporal(w, height);
        return true;
    }
    ReleaseFastMultiPass();
    g_fast_failed = true; // don't retry every frame after a failure
    ID3DBlob *code = nullptr, *errors = nullptr, *rs = nullptr;
    HRESULT hr = D3DCompile(kFastMultiPassHlsl, sizeof(kFastMultiPassHlsl) - 1,
        "fast-multipass.hlsl", nullptr, nullptr, "CSMain", "cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr))
    {
        Log("[fast-multipass] shader failed 0x%08X: %s; using 1-pass NR", hr,
            errors ? static_cast<const char *>(errors->GetBufferPointer()) : "no diagnostic");
        if (errors) errors->Release();
        if (code) code->Release();
        return false;
    }
    if (errors) { errors->Release(); errors = nullptr; }
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 3;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 32;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 2; desc.pParameters = params;
    hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &errors);
    if (errors) errors->Release();
    if (SUCCEEDED(hr))
        hr = h.dev->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&g_fast_rs));
    if (rs) rs->Release();
    if (SUCCEEDED(hr))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = g_fast_rs;
        pd.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        hr = h.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_fast_pso));
    }
    code->Release();
    if (SUCCEEDED(hr))
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        // A descriptor set per allocator slot: never overwrite in-flight views.
        hd.NumDescriptors = 4 * Host::kFrames;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = h.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_fast_heap));
    }
    if (SUCCEEDED(hr))
    {
        D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w; td.Height = height; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
        hr = h.dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&g_fast_copy));
    }
    if (FAILED(hr))
    {
        ReleaseFastMultiPass(); g_fast_failed = true;
        Log("[fast-multipass] setup failed 0x%08X; using 1-pass NR", hr);
        return false;
    }
    g_fast_failed = false;
    EnsureFastTemporal(w, height);
    Log("[fast-multipass] measured matching at raw NR, target %.0f, %ux%u",
        FastMultiPassStrength(), w, height);
    return true;
}

// Original and motion enter/leave in NPSR; result enters/leaves in UAV.
// No read/write aliasing: all neighbouring Y1 samples come from a copy.
static void RecordFastMultiPass(ID3D12Resource *original, ID3D12Resource *result,
                                UINT w, UINT height, ID3D12Resource *motion, UINT mw, UINT mh, bool reset = false, unsigned style = 2)
{
    D3D12_RESOURCE_BARRIER pre[] = {
        Transition(result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Transition(g_fast_copy, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)
    };
    h.list->ResourceBarrier(2, pre);
    h.list->CopyResource(g_fast_copy, result);
    D3D12_RESOURCE_BARRIER mid[] = {
        Transition(result, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Transition(g_fast_copy, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    };
    h.list->ResourceBarrier(2, mid);
    const UINT stride = h.dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu = g_fast_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(h.frame_slot) * 4 * stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    h.dev->CreateShaderResourceView(original, &sd, cpu); cpu.ptr += stride;
    h.dev->CreateShaderResourceView(g_fast_copy, &sd, cpu); cpu.ptr += stride;
    sd.Format = DXGI_FORMAT_R16G16_FLOAT;
    h.dev->CreateShaderResourceView(motion, &sd, cpu); cpu.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM; ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    h.dev->CreateUnorderedAccessView(result, nullptr, &ud, cpu);
    h.list->SetDescriptorHeaps(1, &g_fast_heap);
    h.list->SetComputeRootSignature(g_fast_rs);
    h.list->SetPipelineState(g_fast_pso);
    const float strength = FastMultiPassStrength();
    const float limit = 0.04f * strength; // 2x: +/-0.08, 4x: +/-0.16, 6x: +/-0.24
    // Retain the constant-buffer layout; retired mode/strength/tone slots are padding.
    UINT32 constants[32] = { w, height, 0, 0, motion ? mw : 0u, motion ? mh : 0u, 0, 0 };
    memcpy(&constants[3], &limit, sizeof(float));
    // Coefficient padding carries motion confidence, including at stability 0%.
    const float stableMotion = FastTemporalAvailable() ? 1.f : 0.f;
    memcpy(&constants[11], &stableMotion, sizeof(float));
    const float (*profile)[3][6] = style == 0 ? kFastReference0 : (style == 1 ? kFastReference1 : kFastReference2);
    const unsigned target = std::clamp(g_fast_settings.target, 1u, 6u)-1;
    for (unsigned basis=0; basis<6; ++basis)
        for (unsigned channel=0; channel<3; ++channel)
            memcpy(&constants[8+basis*4+channel], &profile[target][channel][basis], sizeof(float));
    h.list->SetComputeRoot32BitConstants(0, 32, constants, 0);
    auto gpu = g_fast_heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(h.frame_slot) * 4 * stride;
    h.list->SetComputeRootDescriptorTable(1, gpu);
    h.list->Dispatch((w + 7) / 8, (height + 7) / 8, 1);
    // Downstream compute may read/write result without a state transition.
    D3D12_RESOURCE_BARRIER done = {};
    done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV; done.UAV.pResource = result;
    h.list->ResourceBarrier(1, &done);
    RecordFastTemporal(original, result, w, height, motion, mw, mh, reset);
}

#include "fast_multipass_temporal.inl"
