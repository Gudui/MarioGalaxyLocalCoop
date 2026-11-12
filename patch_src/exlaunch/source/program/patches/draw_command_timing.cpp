#include "draw_command_timing.hpp"
#if defined(GALAXY_DRAW_COMMAND_TIMING_DIAG) && GALAXY_DRAW_COMMAND_TIMING_DIAG
#if !defined(GALAXY_PUSH_CONSTANT_FILTER) || !GALAXY_PUSH_CONSTANT_FILTER
#error DrawCommand diagnostic requires qualified production frame owner
#endif
#if (defined(GALAXY_RECTANGLE_TIMING_DIAG) && GALAXY_RECTANGLE_TIMING_DIAG) || \
    (defined(GALAXY_RETIREMENT_TIMING_DIAG) && GALAXY_RETIREMENT_TIMING_DIAG) || \
    (defined(GALAXY_TEXTURE_TIMING_DIAG) && GALAXY_TEXTURE_TIMING_DIAG)
#error Fulfilled diagnostics must be omitted
#endif
#include "push_constant_filter.hpp"
#include "patch_shared.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <atomic>
#include <algorithm>
#include <type_traits>
namespace mod::patches {
extern "C" { DrawCommandTimingBlock gGalaxyDrawCommandTiming{}; }
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// EVD0401 main5F9F10 aligned16B ADRP/LDR/ADD/BR thunk, GOT919AC8.
// Two callers243A0/639C0; x0 command/w1..5 indexed draw parameters.
// EVD0402 live ring/dispatch and concrete native types; all fields retained.
// Native subsdk4 dispatch1C92490 ->1625B20 ->1639EE8 commits state before
// command append. This is guest API time, not physical GPU execution time.
// EVD0343/345:639C0/243A0 relocatable true-entry parent scopes.
constexpr std::uintptr_t kDrawCommand=0x5F9F10,kIndexed=0x639C0,kOther=0x243A0,kBackendVT=0x889C68;
DrawCommandTimingRow sRow{};
bool sActive=false;
unsigned sDrawDepth=0,sDrawCommandDepth=0;
std::atomic<std::uint64_t> sForeign{0};
std::uint64_t sFirstForeign=0;
std::uint64_t Clock() { std::uint64_t v;asm volatile("mrs %0, cntpct_el0":"=r"(v)::"memory");return v; }
std::uintptr_t Thread() { std::uintptr_t v;asm volatile("mrs %0, tpidrro_el0":"=r"(v));return v; }
template<class T> T Read(std::uintptr_t p,std::uintptr_t off) { return *reinterpret_cast<const T*>(p+off); }
bool Owned() { return gGalaxyPushFilter.thread && Thread()==gGalaxyPushFilter.thread; }
bool Backend(std::uintptr_t p) { return IsGuestPointer(p)&&Read<std::uintptr_t>(p,0)==exl::util::modules::GetTargetStart()+kBackendVT; }
void Add(DrawCommandTimingMetric& m,std::uint64_t ticks) { m.ticks+=ticks;m.maximum=std::max(m.maximum,ticks);++m.calls; }
struct Scope {
    unsigned kind;
    bool active,timed;
    std::uint64_t start;
    Scope(unsigned k,bool valid):kind(k),active(Owned()&&sActive),timed(active&&gGalaxyDrawCommandTiming.mode==1),start(0) {
        if(!Owned()) sForeign.fetch_add(1,std::memory_order_relaxed);
        if(!active) return;
        if(!valid) { sRow.flags|=0x400;active=timed=false;return; }
        if(kind<2) { if(sDrawDepth++) sRow.flags|=0x200; }
        else if(kind==2) { if(sDrawDepth!=1 || sDrawCommandDepth) sRow.flags|=0x200; ++sDrawCommandDepth; }
        start=timed?Clock():0;
    }
    ~Scope() {
        if(!active) return;
        Add(sRow.metric[kind],timed?Clock()-start:0);
        if(kind<2) --sDrawDepth;
        else if(kind==2) --sDrawCommandDepth;
    }
};
HOOK_DEFINE_TRAMPOLINE(DrawCommandProducerTiming) {
    static void Callback(std::uintptr_t command,std::uint32_t indices,std::uint32_t instances,
                         std::uint32_t firstIndex,std::int32_t vertexOffset,std::uint32_t firstInstance) {
        // Fresh frame owner, never a timing-window pointer after a scene load.
        // Idle diagnostics do not walk any native command-ring pointer.
        const auto backend=gGalaxyPushFilter.backend;
        const bool owner=Owned()&&sActive&&Backend(backend);
        const auto bank=owner?Read<std::uint32_t>(backend,0x1061F8):12;
        const bool valid=owner&&bank<12&&command==Read<std::uintptr_t>(backend,0x106190+8*bank);
        Scope scope(2,valid);
        // Native state commit, all draw parameters/barriers/counters/stream remain.
        Orig(command,indices,instances,firstIndex,vertexOffset,firstInstance);
    }
};
HOOK_DEFINE_TRAMPOLINE(DrawCommandIndexedTiming) {
    static void Callback(std::uintptr_t backend,const void* packet,const void* indices) {
        Scope scope(0,packet&&backend==gGalaxyDrawCommandTiming.backend);
        Orig(backend,packet,indices);
    }
};
HOOK_DEFINE_TRAMPOLINE(DrawCommandOtherTiming) {
    static void Callback(std::uintptr_t backend,const void* packet) {
        if(!packet) return Orig(backend,packet);
        Scope scope(1,backend==gGalaxyDrawCommandTiming.backend);
        Orig(backend,packet);
    }
};
}
void DrawCommandBoundary(std::uintptr_t backend) {
    if(!Owned()||!Backend(backend)) return;
    auto& b=gGalaxyDrawCommandTiming;++b.sequence;
    if(!sActive&&b.state!=1&&b.request!=1&&b.request!=2) return;
    const auto now=Clock(),frame=Read<std::uint64_t>(backend,0x60C8);
    const auto bank=Read<std::uint32_t>(backend,0x60C0);
    if(sActive) {
        sActive=false;sRow.lastFrame=frame;sRow.end=now;
        if(sForeign.load(std::memory_order_relaxed)!=sFirstForeign) sRow.flags|=0x100;
        bool valid=b.backend==backend&&b.thread==Thread()&&bank<2&&!sDrawDepth&&!sDrawCommandDepth&&
            frame==sRow.firstFrame+1&&now>sRow.start&&b.frequency&&now-sRow.start<=b.frequency/5&&sRow.flags==b.mode;
        if(valid) {
            const auto stats=backend+0x6020+(bank^1)*0x40;
            sRow.draws=Read<std::uint32_t>(stats,0x30);sRow.bypasses=Read<std::uint32_t>(stats,0x34);sRow.same=Read<std::uint32_t>(stats,0x38);
            valid=sRow.bypasses<=sRow.same&&sRow.same<=sRow.draws&&
                sRow.metric[0].calls+sRow.metric[1].calls==sRow.draws&&sRow.metric[2].calls==sRow.draws;
        }
        if(valid) b.rows[b.count++]=sRow;else ++b.rejected;
        if(b.count==240||b.rejected==240) b.state=2;
    }
    if(b.request==1||b.request==2) {
        b.mode=b.request;b.request=0;b.state=1;b.count=b.rejected=0;
        asm volatile("mrs %0, cntfrq_el0":"=r"(b.frequency));
    }
    if(b.state!=1||bank>1||b.count>=240) return;
    b.backend=backend;b.thread=Thread();sRow={};sFirstForeign=sForeign.load(std::memory_order_relaxed);
    sRow.firstFrame=frame;sRow.start=now;sRow.flags=b.mode;sActive=true;
    if(b.mode==1) { const auto before=Clock();Add(sRow.metric[3],Clock()-before); }
}
void InstallDrawCommandTiming() {
    DrawCommandIndexedTiming::InstallAtOffset(kIndexed);
    DrawCommandOtherTiming::InstallAtOffset(kOther);
    DrawCommandProducerTiming::InstallAtOffset(kDrawCommand);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0201 ATT-0177 draw-command costs installed default-idle window=240 forward-all kill=1");
}
}
#endif
