#include "draw_cost.hpp"
#if defined(GALAXY_DRAW_COST_DIAG) && GALAXY_DRAW_COST_DIAG
#if !(defined(GALAXY_FRAME_TIMELINE_DIAG) && GALAXY_FRAME_TIMELINE_DIAG)
#error "DIAG-0213 needs the DIAG-0208 frame timeline (GALAXY_FRAME_TIMELINE_DIAG)"
#endif
#if !(defined(GALAXY_PRODUCER_STALL_DIAG) && GALAXY_PRODUCER_STALL_DIAG)
#error "DIAG-0213 needs DIAG-0211 producer busy time (GALAXY_PRODUCER_STALL_DIAG)"
#endif
#if defined(GALAXY_PRODUCER_COST_DIAG) && GALAXY_PRODUCER_COST_DIAG
#error "DIAG-0213 and DIAG-0210 both hook 883C and 49FC0; build only one"
#endif
#include "frame_timeline.hpp"
#include "frame_overlap.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <cstdint>
#include <cstdio>
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// DIAG-0213 / ATT-0203 (EVD-0491): which per-draw producer steps make the
// jump/hit dips. Ordinary draws (7260, also D724/D7C0/529DB0) and the F264
// effect command (883C -> F264) share the same steps (ghidra_draw_cost_entries
// and _entries2_20261018, bl: lists):
//   B5C0 pipeline info build, 9944/A004/8F04/8900 pipeline hashes,
//   10420 uniform build (12 arguments), 91FC packet write (10 arguments),
//   EB0C post-packet, 9584 attribute setup, 2A3320 / 544B04 backend
//   allocators (vtable 889C68 +0x98/+0xA0).
// Their first 16 bytes are stack setup, register moves or plain loads, and no
// code references bytes 4..15 of any of them (ghidra_draw_cost_refs_20261018).
// Because 10420/91FC take stack arguments, every step is timed by an assembly
// wrapper that calls the original without moving sp and keeps its state in a
// per-step global block; only the producer thread is timed, others and nested
// calls pass straight through. 883C and 49FC0 keep DIAG-0210's C++ hooks
// (<= 8 register arguments, ghidra_producer_entries_20261013); 883C marks its
// sub-steps as effect work. Producer busy per frame comes from DIAG-0211.
namespace mod::patches {
namespace {
enum Slot:unsigned{SPipe,SHash0,SHash1,SHash2,SHash3,SUniform,SPacket,SPost,SAttr,SAllocA,SAllocB,SlotCount};
constexpr std::uintptr_t kSlotOffset[SlotCount]={0xB5C0,0x9944,0xA004,0x8F04,0x8900,0x10420,0x91FC,0xEB0C,0x9584,
                                                0x2A3320,0x544B04};
constexpr std::uintptr_t kCustom=0x883C,kBatch=0x49FC0;
constexpr unsigned kFrames=300,kTop=3,kMaxLogs=40;
}
}
extern "C" {
// One 64-byte block per step; the assembly below hard-codes this layout.
struct alignas(64) GalaxyDrawCostRaw{
    std::uint64_t lr,start,ticks[2],calls[2],depth,orig;
};
static_assert(sizeof(GalaxyDrawCostRaw)==64);
GalaxyDrawCostRaw gDrawCost[mod::patches::SlotCount]{};
std::uint64_t gDrawCostThread=0;  // producer tpidrro_el0
std::uint64_t gDrawCostCtx=0;     // 1 while F264 runs
void GalaxyDrawCost0();void GalaxyDrawCost1();void GalaxyDrawCost2();void GalaxyDrawCost3();
void GalaxyDrawCost4();void GalaxyDrawCost5();void GalaxyDrawCost6();void GalaxyDrawCost7();
void GalaxyDrawCost8();void GalaxyDrawCost9();void GalaxyDrawCost10();
}
asm(R"(
.macro GALAXY_DRAW_COST name, idx
.pushsection .text
.balign 4
.global \name
.type \name, %function
\name:
    mrs x16, tpidrro_el0
    adrp x17, gDrawCostThread
    ldr x17, [x17, :lo12:gDrawCostThread]
    cmp x16, x17
    b.ne 2f
    adrp x16, gDrawCost
    add x16, x16, :lo12:gDrawCost
    add x16, x16, #(\idx * 64)
    ldr x17, [x16, #48]
    cbnz x17, 2f
    mov x17, #1
    str x17, [x16, #48]
    str x30, [x16]
    mrs x17, cntpct_el0
    str x17, [x16, #8]
    ldr x17, [x16, #56]
    blr x17
    adrp x16, gDrawCost
    add x16, x16, :lo12:gDrawCost
    add x16, x16, #(\idx * 64)
    mrs x17, cntpct_el0
    ldr x9, [x16, #8]
    sub x17, x17, x9
    adrp x9, gDrawCostCtx
    ldr x9, [x9, :lo12:gDrawCostCtx]
    add x9, x16, x9, lsl #3
    ldr x10, [x9, #16]
    add x10, x10, x17
    str x10, [x9, #16]
    ldr x10, [x9, #32]
    add x10, x10, #1
    str x10, [x9, #32]
    str xzr, [x16, #48]
    ldr x30, [x16]
    ret
2:
    adrp x17, gDrawCost
    add x17, x17, :lo12:gDrawCost
    ldr x17, [x17, #(\idx * 64 + 56)]
    br x17
.size \name, .-\name
.popsection
.endm
GALAXY_DRAW_COST GalaxyDrawCost0, 0
GALAXY_DRAW_COST GalaxyDrawCost1, 1
GALAXY_DRAW_COST GalaxyDrawCost2, 2
GALAXY_DRAW_COST GalaxyDrawCost3, 3
GALAXY_DRAW_COST GalaxyDrawCost4, 4
GALAXY_DRAW_COST GalaxyDrawCost5, 5
GALAXY_DRAW_COST GalaxyDrawCost6, 6
GALAXY_DRAW_COST GalaxyDrawCost7, 7
GALAXY_DRAW_COST GalaxyDrawCost8, 8
GALAXY_DRAW_COST GalaxyDrawCost9, 9
GALAXY_DRAW_COST GalaxyDrawCost10, 10
)");
namespace mod::patches {
namespace {
void (*const kWrapper[SlotCount])()={GalaxyDrawCost0,GalaxyDrawCost1,GalaxyDrawCost2,GalaxyDrawCost3,GalaxyDrawCost4,
    GalaxyDrawCost5,GalaxyDrawCost6,GalaxyDrawCost7,GalaxyDrawCost8,GalaxyDrawCost9,GalaxyDrawCost10};
// Per-frame step totals: [ctx][slot], plus F264 and conversion batch.
struct Frame{
    std::uint64_t ticks[2][SlotCount],busy,customTicks,batchTicks;
    std::uint32_t calls[2][SlotCount],customCalls,batchCalls;
};
struct Window{
    Frame sum;
    Frame top[kTop];
    std::uint64_t busyMax;
    std::uint32_t frames;
};
// Producer thread only.
Window sWindow{};
Frame sCurrent{};
std::uint64_t sSnapTicks[2][SlotCount]{},sSnapCalls[2][SlotCount]{},sSnapBusy=0;
std::uint32_t sFrame=~0u,sLogs=0;
bool sFrameSplit=false;
std::uint64_t sFrequency=19200000;
std::uintptr_t Thread(){std::uintptr_t v;asm volatile("mrs %0,tpidrro_el0":"=r"(v));return v;}
std::uint64_t Clock(){std::uint64_t v;asm volatile("mrs %0,cntpct_el0":"=r"(v)::"memory");return v;}
double Ms(std::uint64_t ticks,std::uint32_t frames=1){
    return frames?static_cast<double>(ticks)*1000.0/static_cast<double>(sFrequency)/frames:0.0;
}
std::uint64_t HashTicks(const Frame& f,int c){return f.ticks[c][SHash0]+f.ticks[c][SHash1]+f.ticks[c][SHash2]+f.ticks[c][SHash3];}
std::uint64_t AllocTicks(const Frame& f,int c){return f.ticks[c][SAllocA]+f.ticks[c][SAllocB];}
// "pipe=ms/n hash=ms uni=ms/n pkt=ms/n post=ms attr=ms alloc=ms/n" for one context.
void Parts(char* out,std::size_t size,const Frame& f,int c,std::uint32_t frames){
    const double n=frames?static_cast<double>(frames):1.0;
    snprintf(out,size,"pipe=%.2f/%.0f hash=%.2f uni=%.2f/%.0f pkt=%.2f/%.0f post=%.2f attr=%.2f alloc=%.2f/%.0f",
        Ms(f.ticks[c][SPipe],frames),f.calls[c][SPipe]/n,Ms(HashTicks(f,c),frames),
        Ms(f.ticks[c][SUniform],frames),f.calls[c][SUniform]/n,Ms(f.ticks[c][SPacket],frames),f.calls[c][SPacket]/n,
        Ms(f.ticks[c][SPost],frames),Ms(f.ticks[c][SAttr],frames),
        Ms(AllocTicks(f,c),frames),(f.calls[c][SAllocA]+f.calls[c][SAllocB])/n);
}
void LogWindow(){
    if(sLogs>=kMaxLogs)return;
    ++sLogs;
    const auto& w=sWindow;
    char reg[192],eff[192];
    Parts(reg,sizeof reg,w.sum,0,w.frames);Parts(eff,sizeof eff,w.sum,1,w.frames);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0213 draw cost frames=%u busy=%.2f max=%.2f f264=(%.2f,%.1f) batch=(%.2f,%.1f) "
        "draws=(%s) effects=(%s) kill=1",
        w.frames,Ms(w.sum.busy,w.frames),Ms(w.busyMax),Ms(w.sum.customTicks,w.frames),
        static_cast<double>(w.sum.customCalls)/w.frames,Ms(w.sum.batchTicks,w.frames),
        static_cast<double>(w.sum.batchCalls)/w.frames,reg,eff);
    for(unsigned k=0;k<kTop;++k){
        const auto& f=w.top[k];
        if(!f.busy)continue;
        Parts(reg,sizeof reg,f,0,1);Parts(eff,sizeof eff,f,1,1);
        Logging.Log("[GalaxyCoop:smg1] DIAG-0213 heavy frame rank=%u busy=%.2f f264=(%.2f,%u) batch=(%.2f,%u) "
            "draws=(%s) effects=(%s) kill=1",
            k+1,Ms(f.busy),Ms(f.customTicks),f.customCalls,Ms(f.batchTicks),f.batchCalls,reg,eff);
    }
}
// Close the producer's current frame when main's frame id changes; only split
// frames count, kFrames of them make one log.
void Roll(){
    const auto producer=FrameTimelineProducerThread();
    if(producer&&!gDrawCostThread)gDrawCostThread=producer;
    const auto frame=FrameTimelineFrameId();
    if(frame==sFrame)return;
    sFrame=frame;
    const auto busy=ProducerStallBusyTicks();
    for(int c=0;c<2;++c)for(unsigned s=0;s<SlotCount;++s){
        sCurrent.ticks[c][s]=gDrawCost[s].ticks[c]-sSnapTicks[c][s];
        sCurrent.calls[c][s]=static_cast<std::uint32_t>(gDrawCost[s].calls[c]-sSnapCalls[c][s]);
        sSnapTicks[c][s]=gDrawCost[s].ticks[c];sSnapCalls[c][s]=gDrawCost[s].calls[c];
    }
    sCurrent.busy=busy-sSnapBusy;sSnapBusy=busy;
    if(sFrameSplit){
        auto& w=sWindow;
        auto& sum=w.sum;
        for(int c=0;c<2;++c)for(unsigned s=0;s<SlotCount;++s){sum.ticks[c][s]+=sCurrent.ticks[c][s];sum.calls[c][s]+=sCurrent.calls[c][s];}
        sum.busy+=sCurrent.busy;sum.customTicks+=sCurrent.customTicks;sum.customCalls+=sCurrent.customCalls;
        sum.batchTicks+=sCurrent.batchTicks;sum.batchCalls+=sCurrent.batchCalls;
        if(sCurrent.busy>w.busyMax)w.busyMax=sCurrent.busy;
        for(unsigned k=0;k<kTop;++k)if(sCurrent.busy>w.top[k].busy){
            for(unsigned j=kTop-1;j>k;--j)w.top[j]=w.top[j-1];
            w.top[k]=sCurrent;
            break;
        }
        if(++w.frames>=kFrames){LogWindow();w=Window{};}
    }
    sCurrent=Frame{};
    sFrameSplit=FrameTimelineSplit();
}
bool OnProducer(){
    const auto producer=FrameTimelineProducerThread();
    return producer&&producer==Thread();
}
using U=std::uintptr_t;
HOOK_DEFINE_TRAMPOLINE(DrawCostCustom){
    static U Callback(U a,U b,U c,U d,U e,U f,U g,U h){
        if(!OnProducer())return Orig(a,b,c,d,e,f,g,h);
        Roll();
        gDrawCostCtx=1;
        const auto start=Clock();
        const auto r=Orig(a,b,c,d,e,f,g,h);
        sCurrent.customTicks+=Clock()-start;++sCurrent.customCalls;
        gDrawCostCtx=0;
        return r;
    }
};
HOOK_DEFINE_TRAMPOLINE(DrawCostBatch){
    static U Callback(U a,U b,U c,U d,U e,U f,U g,U h){
        if(!OnProducer())return Orig(a,b,c,d,e,f,g,h);
        Roll();
        const auto start=Clock();
        const auto r=Orig(a,b,c,d,e,f,g,h);
        sCurrent.batchTicks+=Clock()-start;++sCurrent.batchCalls;
        return r;
    }
};
}
void InstallDrawCost(){
    asm volatile("mrs %0,cntfrq_el0":"=r"(sFrequency));
    if(!sFrequency)sFrequency=19200000;
    const auto base=exl::util::modules::GetTargetStart();
    for(unsigned s=0;s<SlotCount;++s)
        gDrawCost[s].orig=exl::hook::nx64::Hook(base+kSlotOffset[s],reinterpret_cast<std::uintptr_t>(kWrapper[s]),true);
    DrawCostCustom::InstallAtOffset(kCustom);
    DrawCostBatch::InstallAtOffset(kBatch);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0213 ATT-0203 draw cost installed steps=B5C0,9944,A004,8F04,8900,10420,91FC,EB0C,"
        "9584,2A3320,544B04 custom=0x883C batch=0x49FC0 frames=%u top=%u kill=1",kFrames,kTop);
}
}
#endif
