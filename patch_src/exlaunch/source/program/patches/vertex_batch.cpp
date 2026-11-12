#include "vertex_batch.hpp"
#if defined(GALAXY_VERTEX_BATCH_DIAG) && GALAXY_VERTEX_BATCH_DIAG
#if !defined(GALAXY_PUSH_CONSTANT_FILTER) || !GALAXY_PUSH_CONSTANT_FILTER
#error Vertex batching requires the qualified production owner
#endif
#if (defined(GALAXY_BINDING_TIMING_DIAG) && GALAXY_BINDING_TIMING_DIAG) || \
    (defined(GALAXY_DRAW_COMMAND_TIMING_DIAG) && GALAXY_DRAW_COMMAND_TIMING_DIAG) || \
    (defined(GALAXY_TEXTURE_TIMING_DIAG) && GALAXY_TEXTURE_TIMING_DIAG) || \
    (defined(GALAXY_RECTANGLE_TIMING_DIAG) && GALAXY_RECTANGLE_TIMING_DIAG) || \
    (defined(GALAXY_RETIREMENT_TIMING_DIAG) && GALAXY_RETIREMENT_TIMING_DIAG)
#error Fulfilled diagnostics must be omitted
#endif
#include "vertex_batch_queue.hpp"
#include "push_constant_filter.hpp"
#include "patch_shared.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <algorithm>
#include <atomic>

namespace mod::patches {
extern "C" { VertexBatchBlock gGalaxyVertexBatch{}; }
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// EVD0415: true aligned API entries; Other caller26038 loops over27 attributes,
// compact slots starting0, then only ownership stores before Draw260D4.
// Native subs4 SHA9d7b958f58dcfbe013b3dbd0f126096a6b85452249f09fb7841247e348c2f46d:
// 16599E0 accepts arrays up to32 slots; each original buffer/offset entry reaches
// the same native loop. No pipeline/index/draw/barrier/ref/effect is suppressed.
// Direct ExLaunch B/BR preserves LR. Own ABI and current shadow verify gates.
constexpr std::uintptr_t kVertex=0x5F9F00,kDraw=0x5F9F10,kIndexed=0x639C0,kOther=0x243A0;
constexpr std::uintptr_t kVertexReturn=0x2603C,kDrawReturn=0x260D8,kBackendVT=0x889C68;
VertexBatchQueue sQueue{};
VertexBatchRow sRow{};
bool sActive=false;
unsigned sDrawDepth=0,sOtherDepth=0;
std::atomic<std::uint64_t> sForeign{0};
std::uint64_t sFirstForeign=0;
std::uint64_t Clock() { std::uint64_t v;asm volatile("mrs %0,cntpct_el0":"=r"(v)::"memory");return v; }
std::uintptr_t Thread() { std::uintptr_t v;asm volatile("mrs %0,tpidrro_el0":"=r"(v));return v; }
template<class T> T Read(std::uintptr_t p,std::uintptr_t off) { return *reinterpret_cast<const T*>(p+off); }
bool Owned() { return gGalaxyPushFilter.thread&&Thread()==gGalaxyPushFilter.thread; }
bool Backend(std::uintptr_t p) { return IsGuestPointer(p)&&Read<std::uintptr_t>(p,0)==exl::util::modules::GetTargetStart()+kBackendVT; }
bool Command(std::uintptr_t command) {
    const auto p=gGalaxyPushFilter.backend;
    if(!Owned()||!sActive||!Backend(p)) return false;
    const auto bank=Read<std::uint32_t>(p,0x1061F8);
    return bank<12&&command==Read<std::uintptr_t>(p,0x106190+8*bank);
}
void Add(VertexBatchMetric& m,std::uint64_t ticks) { m.ticks+=ticks;m.maximum=std::max(m.maximum,ticks);++m.calls; }
void Fail() { ++sRow.failures;sRow.flags|=0x400;gGalaxyVertexBatch.shadowPassed=0; }
struct Timing {
    unsigned kind;bool active,timed;std::uint64_t start;
    Timing(unsigned k):kind(k),active(Owned()&&sActive),timed(active&&gGalaxyVertexBatch.mode!=2),start(timed?Clock():0) {}
    ~Timing() { if(active) Add(sRow.metric[kind],timed?Clock()-start:0); }
};
struct Parent {
    bool active,other;Timing timing;
    Parent(std::uintptr_t backend,bool isOther):active(Owned()&&sActive),other(isOther),timing(isOther?1:0) {
        if(!Owned()) sForeign.fetch_add(1,std::memory_order_relaxed);
        if(!active) return;
        if(backend!=gGalaxyVertexBatch.backend||sDrawDepth) sRow.flags|=0x200;
        ++sDrawDepth;
        if(other) ++sOtherDepth;
    }
    ~Parent() { if(active) { if(other) --sOtherDepth;--sDrawDepth; } }
};
void Flush(std::uintptr_t command,std::uintptr_t caller);
HOOK_DEFINE_TRAMPOLINE(VertexArrayInput) {
    static void Callback(std::uintptr_t command,std::uint32_t first,std::uint32_t count,
                         const std::uintptr_t* buffers,const std::uint64_t* offsets) {
        const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const bool active=Owned()&&sActive&&sDrawDepth==1;
        if(!active) {
            if(!Owned()) sForeign.fetch_add(1,std::memory_order_relaxed);
            return Orig(command,first,count,buffers,offsets);
        }
        Timing timing(2);
        if(gGalaxyVertexBatch.mode==3&&!gGalaxyVertexBatch.shadowPassed) {
            Flush(command,0);++sRow.nativeCalls;
            return Orig(command,first,count,buffers,offsets);
        }
        const bool eligible=sOtherDepth==1&&caller==exl::util::modules::GetTargetStart()+kVertexReturn;
        if(!eligible) { Flush(command,0);++sRow.nativeCalls;return Orig(command,first,count,buffers,offsets); }
        ++sRow.eligibleCalls;
        if(!Command(command)||count!=1||!IsGuestPointer(reinterpret_cast<std::uintptr_t>(buffers))||
           !IsGuestPointer(reinterpret_cast<std::uintptr_t>(offsets))||
           !sQueue.Append(command,first,count,buffers[0],offsets[0])) {
            Fail();
            // A valid queued run is still forwarded before this unexpected call.
            Flush(command,0);
            ++sRow.nativeCalls;return Orig(command,first,count,buffers,offsets);
        }
        if(gGalaxyVertexBatch.mode==3) return;
        ++sRow.nativeCalls;Orig(command,first,count,buffers,offsets);
    }
};
void Flush(std::uintptr_t command,std::uintptr_t caller) {
    if(!sQueue.count) return;
    ++sRow.eligibleDraws;sRow.queuedSlots+=sQueue.count;
    if(caller!=exl::util::modules::GetTargetStart()+kDrawReturn) Fail();
    if(gGalaxyVertexBatch.mode==3) {
        if(command==sQueue.command&&Command(command)) {
            Timing timing(3);++sRow.nativeCalls;
            VertexArrayInput::Orig(command,0,sQueue.count,sQueue.buffers,sQueue.offsets);
        } else Fail(); // Refuse an invalid/stale command, never dereference it.
    }
    sQueue.Clear();
}
HOOK_DEFINE_TRAMPOLINE(VertexArrayDraw) {
    static void Callback(std::uintptr_t command,std::uint32_t indices,std::uint32_t instances,
                         std::uint32_t firstIndex,std::int32_t vertexOffset,std::uint32_t firstInstance) {
        const auto caller=reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        if(Owned()&&sActive&&sDrawDepth==1) {
            if(!Command(command)) Fail();
            Flush(command,caller);++sRow.drawCommands;
        } else if(!Owned()) sForeign.fetch_add(1,std::memory_order_relaxed);
        Orig(command,indices,instances,firstIndex,vertexOffset,firstInstance);
    }
};
HOOK_DEFINE_TRAMPOLINE(VertexArrayIndexed) {
    static void Callback(std::uintptr_t backend,const void* packet,const void* indices) {
        Parent parent(backend,false);Orig(backend,packet,indices);
    }
};
HOOK_DEFINE_TRAMPOLINE(VertexArrayOther) {
    static void Callback(std::uintptr_t backend,const void* packet) {
        if(!packet) return Orig(backend,packet);
        Parent parent(backend,true);Orig(backend,packet);
        if(parent.active&&sQueue.count) { Fail();sQueue.Clear(); }
    }
};
}
void VertexBatchBoundary(std::uintptr_t backend) {
    if(!Owned()||!Backend(backend)) return;
    auto& b=gGalaxyVertexBatch;++b.sequence;
    if(!sActive&&b.state!=1&&b.request!=1&&b.request!=2&&b.request!=3) return;
    const auto now=Clock(),frame=Read<std::uint64_t>(backend,0x60C8);
    const auto bank=Read<std::uint32_t>(backend,0x60C0);
    if(sActive) {
        sActive=false;sRow.lastFrame=frame;sRow.end=now;
        if(sForeign.load(std::memory_order_relaxed)!=sFirstForeign) sRow.flags|=0x100;
        bool valid=b.backend==backend&&b.thread==Thread()&&bank<2&&!sDrawDepth&&!sOtherDepth&&!sQueue.count&&
            frame==sRow.firstFrame+1&&now>sRow.start&&b.frequency&&now-sRow.start<=b.frequency/5&&sRow.flags==b.mode;
        if(valid) {
            const auto stats=backend+0x6020+(bank^1)*0x40;
            sRow.draws=Read<std::uint32_t>(stats,0x30);sRow.bypasses=Read<std::uint32_t>(stats,0x34);sRow.same=Read<std::uint32_t>(stats,0x38);
            valid=sRow.bypasses<=sRow.same&&sRow.same<=sRow.draws&&sRow.metric[0].calls+sRow.metric[1].calls==sRow.draws&&
                sRow.drawCommands==sRow.draws&&sRow.queuedSlots==sRow.eligibleCalls&&sRow.eligibleDraws<=sRow.metric[1].calls&&
                sRow.eligibleDraws<=sRow.eligibleCalls&&sRow.eligibleCalls<=sRow.metric[2].calls&&
                sRow.nativeCalls==sRow.metric[2].calls-(b.mode==3?sRow.eligibleCalls-sRow.eligibleDraws:0)&&!sRow.failures;
        }
        if(valid) b.rows[b.count++]=sRow;else { ++b.rejected;b.shadowPassed=0;if(b.mode==3) b.state=2; }
        if(b.count==240||b.rejected==240) {
            b.state=2;
            if(b.mode==2&&b.count==240&&!b.rejected) {
                std::uint64_t removed=0;
                for(const auto& r:b.rows) removed+=r.eligibleCalls-r.eligibleDraws;
                b.shadowRemoved=removed;b.shadowPassed=removed>=240000;
            }
        }
    }
    if(b.request>=1&&b.request<=3) {
        b.mode=b.request;b.request=0;b.state=1;b.count=b.rejected=0;
        if(b.mode==2) { b.shadowPassed=0;b.shadowRemoved=0; }
        asm volatile("mrs %0,cntfrq_el0":"=r"(b.frequency));
        if(b.mode==3&&!b.shadowPassed) { b.state=2;return; }
    }
    if(b.state!=1||bank>1||b.count>=240) return;
    if((b.backend&&b.backend!=backend)||(b.thread&&b.thread!=Thread())) {
        b.shadowPassed=0;if(b.mode==3) { b.state=2;return; }
    }
    b.backend=backend;b.thread=Thread();sRow={};sQueue.Clear();sFirstForeign=sForeign.load(std::memory_order_relaxed);
    sRow.firstFrame=frame;sRow.start=now;sRow.flags=b.mode;sActive=true;
    if(b.mode!=2) { const auto before=Clock();Add(sRow.metric[4],Clock()-before); }
}
void InstallVertexBatch() {
    VertexArrayIndexed::InstallAtOffset(kIndexed);VertexArrayOther::InstallAtOffset(kOther);
    VertexArrayInput::InstallAtOffset(kVertex);VertexArrayDraw::InstallAtOffset(kDraw);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0202 ATT-0181 native vertex arrays installed default-native bounded=240 shadow-required kill=1");
}
}
#endif
