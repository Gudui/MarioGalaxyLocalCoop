#include "binding_timing.hpp"
#if defined(GALAXY_BINDING_TIMING_DIAG) && GALAXY_BINDING_TIMING_DIAG
#if !defined(GALAXY_PUSH_CONSTANT_FILTER) || !GALAXY_PUSH_CONSTANT_FILTER
#error Binding diagnostic requires qualified production frame owner
#endif
#if (defined(GALAXY_RECTANGLE_TIMING_DIAG) && GALAXY_RECTANGLE_TIMING_DIAG) || \
    (defined(GALAXY_RETIREMENT_TIMING_DIAG) && GALAXY_RETIREMENT_TIMING_DIAG) || \
    (defined(GALAXY_TEXTURE_TIMING_DIAG) && GALAXY_TEXTURE_TIMING_DIAG)
#error Fulfilled diagnostics must be omitted
#endif
#if defined(GALAXY_DRAW_COMMAND_TIMING_DIAG) && GALAXY_DRAW_COMMAND_TIMING_DIAG
#error Fulfilled draw-command diagnostic must be omitted
#endif
#include "push_constant_filter.hpp"
#include "patch_shared.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <atomic>
#include <algorithm>
#include <type_traits>
namespace mod::patches {
extern "C" { BindingTimingBlock gGalaxyBindingTiming{}; }
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// EVD0408 Ghidra true aligned16B import entries and native ABI/callers.
// Pipeline x0/w1/x2; index x0/x1/x2/w3; vertex x0/w1/w2/x3/x4.
// EVD0409 fresh paused ownerVT889C68/current ring/magic/dispatch qualify.
// ALL native arguments/state/commands/resources forwarded, no cached output.
// Existing pipeline cache EVD0353 is not elapsed or complete suppression proof.
constexpr std::uintptr_t kBinding=0x5F9F10,kPipeline=0x5F9EC0,kIndex=0x5F9ED0,kVertex=0x5F9F00;
constexpr std::uintptr_t kIndexed=0x639C0,kOther=0x243A0,kBackendVT=0x889C68;
BindingTimingRow sRow{};
bool sActive=false;
unsigned sDrawDepth=0,sBindingDepth=0;
std::atomic<std::uint64_t> sForeign{0};
std::uint64_t sFirstForeign=0;
std::uint64_t Clock() { std::uint64_t v;asm volatile("mrs %0, cntpct_el0":"=r"(v)::"memory");return v; }
std::uintptr_t Thread() { std::uintptr_t v;asm volatile("mrs %0, tpidrro_el0":"=r"(v));return v; }
template<class T> T Read(std::uintptr_t p,std::uintptr_t off) { return *reinterpret_cast<const T*>(p+off); }
bool Owned() { return gGalaxyPushFilter.thread && Thread()==gGalaxyPushFilter.thread; }
bool Backend(std::uintptr_t p) { return IsGuestPointer(p)&&Read<std::uintptr_t>(p,0)==exl::util::modules::GetTargetStart()+kBackendVT; }
void Add(BindingTimingMetric& m,std::uint64_t ticks) { m.ticks+=ticks;m.maximum=std::max(m.maximum,ticks);++m.calls; }
struct Scope {
    unsigned kind;
    bool active,timed;
    std::uint64_t start;
    Scope(unsigned k,bool valid):kind(k),active(Owned()&&sActive&&(k<2||sDrawDepth==1)),timed(active&&gGalaxyBindingTiming.mode==1&&k!=5),start(0) {
        if(!Owned()) sForeign.fetch_add(1,std::memory_order_relaxed);
        if(!active) return;
        if(!valid) { sRow.flags|=0x400;active=timed=false;return; }
        if(kind<2) { if(sDrawDepth++) sRow.flags|=0x200; }
        else if(kind>=2) { if(sDrawDepth!=1 || sBindingDepth) sRow.flags|=0x200; ++sBindingDepth; }
        start=timed?Clock():0;
    }
    ~Scope() {
        if(!active) return;
        Add(sRow.metric[kind],timed?Clock()-start:0);
        if(kind<2) --sDrawDepth;
        else if(kind>=2) --sBindingDepth;
    }
};
bool ValidCommand(std::uintptr_t command) {
    // Fresh owner each frame; idle never follows a native ring pointer.
    const auto backend=gGalaxyPushFilter.backend;
    if(!Owned()||!sActive||sDrawDepth!=1||!Backend(backend)) return false;
    const auto bank=Read<std::uint32_t>(backend,0x1061F8);
    return bank<12&&command==Read<std::uintptr_t>(backend,0x106190+8*bank);
}
HOOK_DEFINE_TRAMPOLINE(BindingPipelineTiming) {
    static void Callback(std::uintptr_t command,std::uint32_t bindpoint,std::uintptr_t pipeline) {
        Scope scope(2,ValidCommand(command));
        Orig(command,bindpoint,pipeline);
    }
};
HOOK_DEFINE_TRAMPOLINE(BindingIndexTiming) {
    static void Callback(std::uintptr_t command,std::uintptr_t buffer,std::uint64_t offset,std::uint32_t type) {
        Scope scope(3,ValidCommand(command));
        Orig(command,buffer,offset,type);
    }
};
HOOK_DEFINE_TRAMPOLINE(BindingVertexTiming) {
    static void Callback(std::uintptr_t command,std::uint32_t first,std::uint32_t count,const void* buffers,const void* offsets) {
        Scope scope(4,ValidCommand(command));
        Orig(command,first,count,buffers,offsets);
    }
};
HOOK_DEFINE_TRAMPOLINE(BindingDrawTiming) {
    static void Callback(std::uintptr_t command,std::uint32_t indices,std::uint32_t instances,
                         std::uint32_t firstIndex,std::int32_t vertexOffset,std::uint32_t firstInstance) {
        Scope scope(5,ValidCommand(command));
        Orig(command,indices,instances,firstIndex,vertexOffset,firstInstance);
    }
};
HOOK_DEFINE_TRAMPOLINE(BindingIndexedTiming) {
    static void Callback(std::uintptr_t backend,const void* packet,const void* indices) {
        Scope scope(0,packet&&backend==gGalaxyBindingTiming.backend);
        Orig(backend,packet,indices);
    }
};
HOOK_DEFINE_TRAMPOLINE(BindingOtherTiming) {
    static void Callback(std::uintptr_t backend,const void* packet) {
        if(!packet) return Orig(backend,packet);
        Scope scope(1,backend==gGalaxyBindingTiming.backend);
        Orig(backend,packet);
    }
};
}
void BindingBoundary(std::uintptr_t backend) {
    if(!Owned()||!Backend(backend)) return;
    auto& b=gGalaxyBindingTiming;++b.sequence;
    if(!sActive&&b.state!=1&&b.request!=1&&b.request!=2) return;
    const auto now=Clock(),frame=Read<std::uint64_t>(backend,0x60C8);
    const auto bank=Read<std::uint32_t>(backend,0x60C0);
    if(sActive) {
        sActive=false;sRow.lastFrame=frame;sRow.end=now;
        if(sForeign.load(std::memory_order_relaxed)!=sFirstForeign) sRow.flags|=0x100;
        bool valid=b.backend==backend&&b.thread==Thread()&&bank<2&&!sDrawDepth&&!sBindingDepth&&
            frame==sRow.firstFrame+1&&now>sRow.start&&b.frequency&&now-sRow.start<=b.frequency/5&&sRow.flags==b.mode;
        if(valid) {
            const auto stats=backend+0x6020+(bank^1)*0x40;
            sRow.draws=Read<std::uint32_t>(stats,0x30);sRow.bypasses=Read<std::uint32_t>(stats,0x34);sRow.same=Read<std::uint32_t>(stats,0x38);
            valid=sRow.bypasses<=sRow.same&&sRow.same<=sRow.draws&&
                sRow.metric[0].calls+sRow.metric[1].calls==sRow.draws&&sRow.metric[2].calls==sRow.draws&&sRow.metric[3].calls==sRow.draws&&sRow.metric[5].calls==sRow.draws;
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
    if(b.mode==1) { const auto before=Clock();Add(sRow.metric[6],Clock()-before); }
}
void InstallBindingTiming() {
    BindingIndexedTiming::InstallAtOffset(kIndexed);
    BindingOtherTiming::InstallAtOffset(kOther);
    BindingPipelineTiming::InstallAtOffset(kPipeline);
    BindingIndexTiming::InstallAtOffset(kIndex);
    BindingVertexTiming::InstallAtOffset(kVertex);
    BindingDrawTiming::InstallAtOffset(kBinding);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0201 ATT-0179 pipeline/index/vertex costs installed default-idle window=240 forward-all kill=1");
}
}
#endif
