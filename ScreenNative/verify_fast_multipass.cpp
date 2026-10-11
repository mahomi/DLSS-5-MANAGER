// Standalone GPU checks of the actual production post-process, no NGX needed.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <stdexcept>
#include <string>
using Microsoft::WRL::ComPtr;
struct Host {
    static const int kFrames = 3;
    ID3D12Device *dev = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    int frame_slot = 0;
};
static Host h;
// This standalone probe exercises desktop settings; the production worker
// supplies this flag for its separate VR control path.
static bool g_vr_gpu_mode = false;
static void Log(const char *fmt, ...) {
    va_list args; va_start(args, fmt); vprintf(fmt, args); va_end(args); puts("");
}
static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource *r, D3D12_RESOURCE_STATES a,
                                        D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER t = {}; t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    t.Transition.pResource = r; t.Transition.StateBefore = a; t.Transition.StateAfter = b;
    t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; return t;
}
#include "fast_multipass.inl"
static void Check(bool ok, const char *text) { if (!ok) throw std::runtime_error(text); }
static void Hr(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D12 failure " + std::to_string(hr)); }
static ComPtr<ID3D12CommandQueue> queue;
static ComPtr<ID3D12CommandAllocator> allocator;
static ComPtr<ID3D12Fence> fence;
static UINT64 token = 0;
static HANDLE eventHandle;

static ComPtr<ID3D12Resource> Buffer(UINT64 bytes, D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES heap = {}; heap.Type = type;
    D3D12_RESOURCE_DESC desc = {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    Hr(h.dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&r))); return r;
}
static ComPtr<ID3D12Resource> Texture(UINT w, UINT height, DXGI_FORMAT format, bool uav) {
    D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc = {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w; desc.Height = height; desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.Format = format; desc.SampleDesc.Count = 1;
    if (uav) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    Hr(h.dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r))); return r;
}
static ComPtr<ID3D12Resource> Upload(ID3D12Resource *r, const std::vector<uint8_t> &pixels) {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT64 bytes;
    const auto desc = r->GetDesc(); h.dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
    auto upload = Buffer(bytes, D3D12_HEAP_TYPE_UPLOAD); uint8_t *p;
    Hr(upload->Map(0, nullptr, reinterpret_cast<void **>(&p)));
    for (UINT y = 0; y < desc.Height; ++y)
        memcpy(p + y * fp.Footprint.RowPitch, pixels.data() + y * desc.Width * 4, static_cast<size_t>(desc.Width) * 4);
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = r; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = upload.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint = fp;
    h.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr); return upload;
}
static std::vector<uint8_t> Run(UINT w, UINT height, const std::vector<uint8_t> &x,
                                const std::vector<uint8_t> &y1, bool fastMotion = false, bool reset = true,
                                uint16_t motionHalf = 0x5000, UINT motionW = 3, UINT motionH = 2, unsigned style = 2) {
    const bool enabled = EnsureFastMultiPass(w, height);
    Check(enabled || !FastMultiPassRequested() || FastMultiPassStrength() == 1.0f, "pipeline initialization failed");
    Hr(allocator->Reset()); Hr(h.list->Reset(allocator.Get(), nullptr));
    auto original = Texture(w, height, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    auto result = Texture(w, height, DXGI_FORMAT_R8G8B8A8_UNORM, true);
    auto ux = Upload(original.Get(), x), uy = Upload(result.Get(), y1);
    auto a = Transition(original.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    auto b = Transition(result.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    h.list->ResourceBarrier(1, &a); h.list->ResourceBarrier(1, &b);
    ComPtr<ID3D12Resource> mv, um;
    if (fastMotion) {
        mv = Texture(motionW, motionH, DXGI_FORMAT_R16G16_FLOAT, false);
        std::vector<uint8_t> motion(motionW * motionH * 4, 0);
        for (size_t i = 0; i < motion.size(); i += 4) {
            motion[i] = static_cast<uint8_t>(motionHalf);
            motion[i+1] = static_cast<uint8_t>(motionHalf >> 8);
        }
        um = Upload(mv.Get(), motion);
        auto t = Transition(mv.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        h.list->ResourceBarrier(1, &t);
    }
    if (enabled) RecordFastMultiPass(original.Get(), result.Get(), w, height, mv.Get(), motionW, motionH, reset, style);
    auto r = Transition(result.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto o = Transition(original.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    h.list->ResourceBarrier(1, &r); h.list->ResourceBarrier(1, &o);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {}; UINT64 bytes;
    auto desc = result->GetDesc(); h.dev->GetCopyableFootprints(&desc, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
    auto rb = Buffer(bytes, D3D12_HEAP_TYPE_READBACK), origRb = Buffer(bytes, D3D12_HEAP_TYPE_READBACK);
    auto copy = [&](ID3D12Resource *srcResource, ID3D12Resource *dstResource) {
        D3D12_TEXTURE_COPY_LOCATION src = {}; src.pResource = srcResource;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst = {}; dst.pResource = dstResource;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        h.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    };
    copy(result.Get(), rb.Get()); copy(original.Get(), origRb.Get());
    Hr(h.list->Close()); ID3D12CommandList *lists[] = { h.list }; queue->ExecuteCommandLists(1, lists);
    Hr(queue->Signal(fence.Get(), ++token)); Hr(fence->SetEventOnCompletion(token, eventHandle));
    Check(WaitForSingleObject(eventHandle, 30000) == WAIT_OBJECT_0, "GPU timed out");
    std::vector<uint8_t> out(w * height * 4); uint8_t *p, *q;
    Hr(rb->Map(0, nullptr, reinterpret_cast<void **>(&p)));
    Hr(origRb->Map(0, nullptr, reinterpret_cast<void **>(&q)));
    for (UINT y = 0; y < height; ++y) {
        memcpy(out.data() + y * w * 4, p + y * fp.Footprint.RowPitch, w * 4);
        Check(memcmp(x.data() + y * w * 4, q + y * fp.Footprint.RowPitch, w * 4) == 0, "original frame was modified");
    }
    rb->Unmap(0, nullptr); origRb->Unmap(0, nullptr);
    for (size_t i = 3; i < out.size(); i += 4) Check(out[i] == y1[i], "alpha was modified");
    h.frame_slot = (h.frame_slot + 1) % Host::kFrames; return out;
}
static std::vector<uint8_t> Flat(UINT w, UINT height, uint8_t value) {
    std::vector<uint8_t> p(w * height * 4, value);
    for (size_t i = 3; i < p.size(); i += 4) p[i] = static_cast<uint8_t>(i % 256);
    return p;
}
int wmain(int argc, wchar_t **argv) {
    try {
        if (argc == 3 && wcscmp(argv[1], L"--settings-check") == 0) {
            const wchar_t *path = argv[2];
            auto write = [&](const char *text) {
                FILE *f = nullptr; _wfopen_s(&f, path, L"wb"); Check(f != nullptr, "test settings opened");
                fwrite(text, 1, strlen(text), f); fclose(f); g_fast_next_poll = 0;
                UpdateFastMultiPassSettings(path);
            };
            write("{\"enabled\":true,\"target\":3,\"stability\":0.45}");
            Check(FastMultiPassRequested() && FastMultiPassStrength()==3.f && g_fast_settings.stability==.45f,"new schema loaded");
            const UINT64 revision=g_fast_settings_revision;
            Sleep(10); write("{\"enabled\":true,\"target\":3,\"stability\":0.45,\"strength\":6,\"texture\":true,\"tone\":0.6,\"reference\":false,\"temporal\":false,\"full_stability\":false,\"final_output\":true}");
            Check(g_fast_settings_revision==revision,"retired options changed effective settings/history revision");
            Sleep(10); write("{\"enabled\":false,\"target\":6,\"stability\":0}");
            Check(!FastMultiPassRequested() && FastMultiPassStrength()==6.f && g_fast_settings.stability==0.f,"OFF/target/amount hot loaded");
            Sleep(10); write("{broken");
            Check(!FastMultiPassRequested() && FastMultiPassStrength()==6.f,"invalid update retained valid configuration");
            Check(DeleteFileW(path)!=0,"test settings removed"); g_fast_next_poll=0; UpdateFastMultiPassSettings(path);
            Check(!FastMultiPassRequested() && FastMultiPassStrength()==2.f && g_fast_settings.stability==.9f,"missing settings defaults");
            FastMultiPassSettings value;
            for(const char *bad : {"{}","{\"enabled\":1}","{\"enabled\":true,\"target\":2.5}",
                "{\"enabled\":true,\"stability\":NaN}","{\"enabled\":true,\"strength\":1e999}",
                "{\"enabled\":true,\"strength\":+2}","{\"enabled\":true,\"strength\":02}",
                "{\"enabled\":true,}","{\"enabled\":true}junk","{\"enabled\":true,\"enabled\":false}",
                "{\"enabled\":true,\"reference\":1}","{\"enabled\":true,\"final_output\":1}",
                "{\"enabled\":true,\"texture\":true,\"texture\":false}"})
                Check(!ParseFastMultiPassSettings(bad,value),"invalid JSON accepted");
            Check(ParseFastMultiPassSettings("{\"enabled\":true}",value) && value.target==2 && value.stability==.9f,"optional defaults");
            Check(ParseFastMultiPassSettings("{\"target\":99,\"stability\":99,\"enabled\":true}",value) && value.target==6 && value.stability==.9f,"upper bounds");
            Check(ParseFastMultiPassSettings("{\"enabled\":true,\"target\":-1,\"stability\":-1}",value) && value.target==1 && value.stability==0.f,"lower bounds");
            puts("PASS all live settings checks"); return 0;
        }
        Check(argc == 2, "pass an isolated UI settings JSON path");
        UpdateFastMultiPassSettings(argv[1]);
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
        ComPtr<ID3D12Device> device; Hr(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        h.dev = device.Get();
        ComPtr<IDXGIFactory4> factory; Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        Hr(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter)));
        DXGI_ADAPTER_DESC1 adapterDesc = {}; Hr(adapter->GetDesc1(&adapterDesc));
        Log("GPU: %ls", adapterDesc.Description);
        Check((adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0, "hardware GPU required");
        D3D12_COMMAND_QUEUE_DESC qd = {}; Hr(h.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue)));
        Hr(h.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;
        Hr(h.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        h.list = list.Get(); Hr(list->Close()); Hr(h.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr); Check(eventHandle != nullptr, "event creation failed");
        auto liveSetting = [&](const char *text) {
            Sleep(10); FILE *f=nullptr; _wfopen_s(&f,argv[1],L"wb"); Check(f!=nullptr,"test JSON unavailable");
            fwrite(text,1,strlen(text),f); fclose(f); g_fast_next_poll=0; UpdateFastMultiPassSettings(argv[1]);
        };
        auto x=Flat(17,13,100),y=Flat(17,13,120);
        for(size_t i=0;i<x.size();i+=4) for(int c=0;c<3;++c) {x[i+c]+=uint8_t((i/4*7+c*13)%40);y[i+c]+=uint8_t((i/4*11+c*5)%30);}
        for(unsigned style=0;style<3;++style) for(unsigned target=1;target<=6;++target) {
            char cfg[100];sprintf_s(cfg,"{\"enabled\":true,\"target\":%u,\"stability\":0}",target); liveSetting(cfg);
            auto out=Run(17,13,x,y,false,true,0x5000,3,2,style);
            if(target==1) Check(out==y,"target1 changed current NR");
            else Check(out!=y,"measured coefficients not applied");
            for(size_t i=0;i<out.size();++i) if(i%4!=3)
                Check(abs(int(out[i])-int(y[i]))<=int(std::ceil(.04f*target*255)),"gain exceeded bound");
            Check(Run(17,13,x,y,true,true,0x7e00,3,2,style)==y,"invalid motion amplified");
            for(int frame=0;frame<4;++frame) Check(Run(17,13,x,y,false,true,0x5000,3,2,style)==out,"descriptor slots differed");
        }
        puts("PASS all 3 profiles/6 targets, bounded RGB, alpha, source immutability and NaN motion");
        liveSetting("{\"enabled\":true,\"target\":6,\"stability\":0.9}");
        auto matched=Run(17,13,x,y);
        liveSetting("{\"enabled\":true,\"target\":6,\"stability\":0.9,\"reference\":false,\"strength\":1,\"texture\":true,\"tone\":0,\"temporal\":false,\"full_stability\":false}");
        Check(Run(17,13,x,y)==matched,"retired options altered measured output");
        Check(Run(1,1,Flat(1,1,100),Flat(1,1,120)).size()==4,"1x1 border dispatch failed");
        Run(19,11,Flat(19,11,100),Flat(19,11,120));
        Check(Run(17,13,x,y)==matched,"resize/recreation changed output");
        auto flat=Flat(17,13,100),low=Flat(17,13,120),high=Flat(17,13,124);
        for(float amount : {0.f,.3f,.6f,.9f}) {
            char cfg[100];sprintf_s(cfg,"{\"enabled\":true,\"target\":1,\"stability\":%.2f}",amount); liveSetting(cfg);
            Check(Run(17,13,flat,low)==low,"first history frame changed target1");
            auto filtered=Run(17,13,flat,high,false,false);
            if(amount==0) Check(filtered==high,"0% retained history");
            else Check(filtered[0]<high[0],"nonzero stabilization failed to smooth NR");
            Check(Run(17,13,flat,high,false,true)==high,"reset reused history");
            Log("PASS stabilization %.2f: response=%u",amount,unsigned(filtered[0]));
        }
        auto colored=Flat(17,13,100),coloredNr=colored;
        for(size_t i=0;i<colored.size();i+=4) {colored[i]=190;colored[i+1]=74;colored[i+2]=92;
            for(int c=0;c<3;++c) coloredNr[i+c]=uint8_t(colored[i+c]+10);}
        auto fresh=Run(17,13,colored,coloredNr);
        Run(17,13,flat,low);
        Check(Run(17,13,colored,coloredNr,false,false)==fresh,"disocclusion reused previous hue");
        liveSetting("{\"enabled\":false,\"target\":6}");
        Check(Run(17,13,x,y)==y && !g_fast_history_valid,"OFF retained effect/history");
        ReleaseFastMultiPass();liveSetting("{\"enabled\":true,\"target\":6}");
        Check(Run(17,13,x,y)==matched,"OFF/ON recreation failed");
        puts("PASS retired settings, resize, live 0/30/60/90%, scene reset, RGB rejection and OFF/ON");
        ComPtr<ID3D12InfoQueue> info;
        if (SUCCEEDED(device.As(&info))) {
            for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
                SIZE_T bytes = 0; info->GetMessage(i, nullptr, &bytes); std::vector<uint8_t> storage(bytes);
                auto msg = reinterpret_cast<D3D12_MESSAGE *>(storage.data()); Hr(info->GetMessage(i, msg, &bytes));
                if (msg->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) puts(msg->pDescription);
                Check(msg->Severity > D3D12_MESSAGE_SEVERITY_ERROR, "D3D12 debug validation error");
            }
            puts("PASS D3D12 debug layer: no errors");
        } else puts("INFO D3D12 debug layer unavailable");
        ReleaseFastMultiPass(); CloseHandle(eventHandle); puts("PASS all GPU checks"); return 0;
    } catch (const std::exception &e) { fprintf(stderr, "FAIL %s\n", e.what()); return 1; }
}
