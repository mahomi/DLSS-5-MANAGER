#pragma once
#include <windows.h>
#include <cstdint>
// One driver-owned GPU input/output pair per eye, never game-owned handles.
// Worker must not touch a slot until request is published after input Signal.
// Driver must not reuse slot until completed fence and matching sequence/generation.
namespace vrgpu {
constexpr uint32_t Magic=0x31524756, Version=2;
enum Status:LONG { Starting=0, Ready=1, Failed=2, Stopped=3 };
struct alignas(8) Shared {
    uint32_t magic=Magic,version=Version,bytes=sizeof(Shared),driverPid=0;
    uint32_t width=0,height=0,workWidth=0,workHeight=0,format=28;
    int32_t adapterHigh=0;uint32_t adapterLow=0,eye=0;
    uint64_t inputHandle=0,outputHandle=0,inputFenceHandle=0,outputFenceHandle=0;
    volatile LONG status=Starting,stop=0;
    volatile LONG64 request=0,completed=0;
    uint64_t generation=1;
    uint32_t reset=1,fast=0,target=2,style=1;
    float stability=.9f,intensity=1.f,localTone=.5f,localStructure=1.f,skinStructure=-1.f;
    uint32_t result=0;
    uint64_t elapsedUs=0;
    uint32_t passes=1,appliedPasses=0;
};
// Acquire/Release are real Windows interlocked operations, not volatile synchronization.
inline LONG64 sequence(volatile LONG64* p){return InterlockedCompareExchange64(p,0,0);}
inline LONG state(volatile LONG* p){return InterlockedCompareExchange(p,0,0);}
}
