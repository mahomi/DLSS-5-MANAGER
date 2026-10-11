// Real D3D11 -> external D3D12 NR worker -> D3D11 GPU fence/pixel test.
// Readback exists only in this diagnostic client, never in the live worker.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdio>
#include <vector>
#include <string>
#include <chrono>
#include "vr_gpu_protocol.h"
using Microsoft::WRL::ComPtr;
static unsigned W=640,H=360;
static bool Check(HRESULT hr,const char *what) { if(FAILED(hr)) printf("FAIL %s HRESULT=%08X\n",what,(unsigned)hr); return SUCCEEDED(hr); }
static uint64_t Hash(const std::vector<BYTE> &p) { uint64_t h=1469598103934665603ull; for(auto v:p) { h^=v; h*=1099511628211ull; } return h; }
int main(int argc,char **argv) {
    const bool benchmark=argc>1 && std::string(argv[1])=="--benchmark";
    if(benchmark){W=2496;H=2688;}
    ComPtr<IDXGIFactory1> factory; ComPtr<IDXGIAdapter1> adapter;
    if(!Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory")) return 1;
    DXGI_ADAPTER_DESC1 ad={};
    for(UINT i=0;;++i) {
        ComPtr<IDXGIAdapter1> a;
        if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND) break;
        a->GetDesc1(&ad); if(ad.VendorId==0x10de && !(ad.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) { adapter=a; break; }
    }
    if(!adapter) { printf("FAIL no NVIDIA adapter\n"); return 2; }
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    if(!Check(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&context),"D3D11")) return 3;
    ComPtr<ID3D11Device5> dev5; ComPtr<ID3D11DeviceContext4> ctx4;
    if(!Check(device.As(&dev5),"Device5")||!Check(context.As(&ctx4),"Context4")) return 4;
    ComPtr<ID3D11Texture2D> input,output,readback;
    D3D11_TEXTURE2D_DESC td={}; td.Width=W;td.Height=H;td.MipLevels=td.ArraySize=1;
    td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;
    td.BindFlags=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET;
    td.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if(!Check(device->CreateTexture2D(&td,nullptr,&input),"input texture")||!Check(device->CreateTexture2D(&td,nullptr,&output),"output texture")) return 5;
    td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;td.BindFlags=td.MiscFlags=0;
    if(!Check(device->CreateTexture2D(&td,nullptr,&readback),"readback")) return 6;
    ComPtr<ID3D11Fence> inputFence,outputFence;
    if(!Check(dev5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&inputFence)),"input fence")||
       !Check(dev5->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&outputFence)),"output fence")) return 7;
    HANDLE hi=nullptr,ho=nullptr,hfi=nullptr,hfo=nullptr;
    ComPtr<IDXGIResource1> ri,ro; input.As(&ri); output.As(&ro);
    if(!Check(ri->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&hi),"input handle")||
       !Check(ro->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&ho),"output handle")||
       !Check(inputFence->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&hfi),"input fence handle")||
       !Check(outputFence->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&hfo),"output fence handle")) return 8;
    char name[128]; sprintf_s(name,"Local\\DLSS5VRGpuSelfTest-%lu",GetCurrentProcessId());
    HANDLE mapping=CreateFileMappingA(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(vrgpu::Shared),name);
    HANDLE requestEvent=CreateEventA(nullptr,FALSE,FALSE,(std::string(name)+"-request").c_str());
    auto s=static_cast<vrgpu::Shared*>(MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(vrgpu::Shared)));
    if(!s) return 9; new(s) vrgpu::Shared{};
    s->driverPid=GetCurrentProcessId();s->width=W;s->height=H;s->workWidth=benchmark?(W/4)&~1u:320;s->workHeight=benchmark?(H/4)&~1u:180;
    s->adapterHigh=ad.AdapterLuid.HighPart;s->adapterLow=ad.AdapterLuid.LowPart;
    s->inputHandle=reinterpret_cast<uintptr_t>(hi);s->outputHandle=reinterpret_cast<uintptr_t>(ho);
    s->inputFenceHandle=reinterpret_cast<uintptr_t>(hfi);s->outputFenceHandle=reinterpret_cast<uintptr_t>(hfo);
    // Optional negative cases must reject without publishing readiness/completion.
    std::string negative=argc>1&&!benchmark?argv[1]:"";
    if(negative=="--invalid-version") s->version=99;
    if(negative=="--legacy-version") {s->version=1;s->bytes=160;}
    if(negative=="--invalid-luid") s->adapterLow^=0x12345678;
    if(negative=="--invalid-handle") s->inputHandle=0x123456789;
    char exe[MAX_PATH];GetModuleFileNameA(nullptr,exe,MAX_PATH);if(auto slash=strrchr(exe,'\\')) *(slash+1)=0;
    strcat_s(exe,"nvngx.dll");
    char command[1024];sprintf_s(command,"\"%s\" --vr-gpu %s",exe,name);
    STARTUPINFOA si={};si.cb=sizeof(si);PROCESS_INFORMATION pi={};
    if(!CreateProcessA(exe,command,nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)) {printf("FAIL spawn %lu\n",GetLastError());return 10;}
    auto cleanup=[&] {
        InterlockedExchange(&s->stop,1);
        SetEvent(requestEvent);
        if(WaitForSingleObject(pi.hProcess,5000)!=WAIT_OBJECT_0) { printf("FAIL worker stop timed out\n");TerminateProcess(pi.hProcess,99);WaitForSingleObject(pi.hProcess,5000); }
        DWORD code=99;GetExitCodeProcess(pi.hProcess,&code);printf("worker_exit=%lu\n",code);
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);UnmapViewOfFile(s);CloseHandle(mapping);CloseHandle(requestEvent);
        for(HANDLE h:{hi,ho,hfi,hfo}) CloseHandle(h);
        return code;
    };
    ULONGLONG until=GetTickCount64()+60000;
    while(vrgpu::state(&s->status)==vrgpu::Starting&&GetTickCount64()<until&&WaitForSingleObject(pi.hProcess,0)==WAIT_TIMEOUT) Sleep(5);
    if(!negative.empty()) {
        const bool rejected=vrgpu::state(&s->status)==vrgpu::Failed&&vrgpu::sequence(&s->completed)==0;
        printf("%s negative %s status=%ld result=%08X completed=%lld\n",rejected?"PASS":"FAIL",negative.c_str(),s->status,s->result,vrgpu::sequence(&s->completed));
        cleanup();return rejected?0:11;
    }
    if(vrgpu::state(&s->status)!=vrgpu::Ready) {printf("FAIL not Ready status=%ld result=%08X\n",s->status,s->result);cleanup();return 12;}
    printf("Ready actual NR warmup, LUID=%08X:%08X\n",s->adapterHigh,s->adapterLow);
    std::vector<BYTE> pixels(W*H*4);
    for(unsigned y=0;y<H;++y) for(unsigned x=0;x<W;++x) {
        const unsigned o=(y*W+x)*4;unsigned noise=(x*13+y*7+(x*y)%31)%21;
        pixels[o]=BYTE(30+(x*170/W)+noise);pixels[o+1]=BYTE(40+y*140/H+noise);pixels[o+2]=BYTE(30+((x/40+y/30)%2)*90+noise);pixels[o+3]=255;
    }
    auto frame=[&](LONG64 n,unsigned fast,unsigned target,bool generation,std::vector<BYTE> &got) {
        if(vrgpu::sequence(&s->request)!=vrgpu::sequence(&s->completed)) return false;
        s->fast=fast;s->target=target;s->reset=1;
        if(generation) {++s->generation;s->workWidth=benchmark?(W/5)&~1u:256;s->workHeight=benchmark?(H/5)&~1u:144;}
        context->UpdateSubresource(input.Get(),0,nullptr,pixels.data(),W*4,0);
        if(!Check(ctx4->Signal(inputFence.Get(),n),"input Signal")) return false;
        context->Flush();InterlockedExchange64(&s->request,n);SetEvent(requestEvent);
        const ULONGLONG deadline=GetTickCount64()+30000;
        while(vrgpu::sequence(&s->completed)!=n&&GetTickCount64()<deadline&&WaitForSingleObject(pi.hProcess,0)==WAIT_TIMEOUT&&vrgpu::state(&s->status)!=vrgpu::Failed) Sleep(1);
        const UINT64 f=outputFence->GetCompletedValue();
        if(vrgpu::sequence(&s->completed)!=n||f==UINT64_MAX||f<UINT64(n)||s->result) { printf("FAIL sequence=%lld completed=%lld fence=%llu status=%ld result=%08X\n",n,vrgpu::sequence(&s->completed),f,s->status,s->result);return false; }
        if(!Check(ctx4->Wait(outputFence.Get(),n),"output Wait")) return false;
        context->CopyResource(readback.Get(),output.Get());D3D11_MAPPED_SUBRESOURCE mapped={};
        if(!Check(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"readback Map")) return false;
        got.resize(W*H*4);for(unsigned y=0;y<H;++y) memcpy(got.data()+y*W*4,static_cast<BYTE*>(mapped.pData)+y*mapped.RowPitch,W*4);
        context->Unmap(readback.Get(),0);
        printf("completed=%lld fence=%llu generation=%llu Fast=%u target=%u elapsed_us=%llu hash=%016llX\n",n,f,s->generation,fast,target,s->elapsedUs,Hash(got));
        return true;
    };
    std::vector<BYTE> nr,fast2,fast6,off,resized;
    bool ok=frame(1,0,2,false,nr)&&frame(2,1,2,false,fast2)&&frame(3,1,6,false,fast6)&&frame(4,0,2,false,off)&&frame(5,1,2,true,resized);
    if(ok) {
        size_t changed=0,fastChanged=0,targetChanged=0;for(size_t i=0;i<pixels.size();++i) {changed+=nr[i]!=pixels[i];fastChanged+=fast2[i]!=nr[i];targetChanged+=fast2[i]!=fast6[i];}
        printf("changed_NR_bytes=%zu changed_Fast_bytes=%zu changed_target_bytes=%zu off_restores_NR=%s resize_output_bytes=%zu\n",changed,fastChanged,targetChanged,nr==off?"yes":"no",resized.size());
        ok=changed>100&&fastChanged>100&&targetChanged>100&&nr==off&&resized.size()==pixels.size();
    }
    const DWORD stopped=cleanup();ok=ok&&stopped==0;
    printf("%s actual NR, hot Fast 0/2/6/off, generation resize, output fences, clean stop\n",ok?"PASS":"FAIL");return ok?0:13;
}
