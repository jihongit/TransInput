// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include "test_support.h"

namespace transinput::testing {
struct ResourceSample {
    uint64_t private_bytes{},cpu_ms{};
    DWORD handles{},gdi{},user{};
    static ResourceSample Read(HANDLE process){
        PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof memory;
        Require(K32GetProcessMemoryInfo(process,reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),sizeof memory)!=FALSE,"owned process memory counters");
        ResourceSample value;value.private_bytes=memory.PrivateUsage;
        Require(GetProcessHandleCount(process,&value.handles)!=FALSE,"owned process handle count");
        value.gdi=GetGuiResources(process,GR_GDIOBJECTS);value.user=GetGuiResources(process,GR_USEROBJECTS);
        FILETIME created{},exited{},kernel{},cpu{};
        Require(GetProcessTimes(process,&created,&exited,&kernel,&cpu)!=FALSE,"owned process CPU counters");
        const auto ticks=[](const FILETIME& time){return (static_cast<uint64_t>(time.dwHighDateTime)<<32)|time.dwLowDateTime;};
        value.cpu_ms=(ticks(kernel)+ticks(cpu))/10000;return value;
    }
    void Print(uint64_t elapsed,const char* name)const{
        std::printf("METRIC,%llu,%s,%llu,%lu,%lu,%lu,%llu\n",static_cast<unsigned long long>(elapsed),name,
            static_cast<unsigned long long>(private_bytes),handles,gdi,user,static_cast<unsigned long long>(cpu_ms));
        std::fflush(stdout);
    }
    void CheckGrowth(const ResourceSample& baseline)const{
        // Broad warm-state ceilings detect runaway allocation/handle leaks.
        // They are test bounds, not a product memory/latency guarantee.
        Require(private_bytes<=baseline.private_bytes+32ULL*1024*1024,"owned process private memory exceeded warm baseline by 32 MiB");
        Require(handles<=baseline.handles+32,"owned process handles exceeded warm baseline by 32");
        Require(gdi<=baseline.gdi+8&&user<=baseline.user+8,"owned process GUI resources exceeded warm baseline by 8");
    }
};
}
