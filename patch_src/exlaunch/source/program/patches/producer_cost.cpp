#include "producer_cost.hpp"
#if defined(GALAXY_PRODUCER_COST_DIAG) && GALAXY_PRODUCER_COST_DIAG
#if !(defined(GALAXY_FRAME_TIMELINE_DIAG) && GALAXY_FRAME_TIMELINE_DIAG)
#error "DIAG-0210 needs the DIAG-0208 frame timeline (GALAXY_FRAME_TIMELINE_DIAG)"
#endif
#include "frame_timeline.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <cstdint>
namespace mod::patches {
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// DIAG-0210 / ATT-0194: where the GPU_Decoding producer spends a frame, timed
// in the guest with the debugger off (paused sampling overweights memory
// loads, EVD-0478). Inclusive per-frame time and calls of:
//   6DF0  primitive flush on a state change; called only from 5690
//   49FC0 attribute conversion batch, the body behind the 49FB8 stub (A840)
//   883C  custom 0xC0 subtype 3 command -> F264 pipeline info; called from 6640
//   7194  display-list call check; 5690 (5C40) plus 26CA0/28D10
// Prologues are stack stores and register moves only, and no branch targets
// bytes 4..15 of any of them (ghidra_producer_entries/_entry_refs_20261013).
// Other threads pass straight through.
constexpr std::uintptr_t kFlush=0x6DF0,kBatch=0x49FC0,kCustom=0x883C,kDl=0x7194;
enum Bucket:unsigned{BFlush,BBatch,BCustom,BDl,BConv0,BConv1,BConv2,BucketCount};
constexpr const char* kCategory[2]={"other","split"};
constexpr unsigned kFrames=300,kMaxLogs=80;
struct Acc{
    std::uint64_t ticks[BucketCount],calls[BucketCount],verts[3];
    std::uint64_t abPairs,abVerts,abReal,abScratch;
    std::uint32_t frames;
};
// Producer thread only, so plain fields.
Acc sAcc[2]{};
std::uint32_t sFrame=~0u;
unsigned sCat=0,sLogs=0;
std::uint64_t sFrequency=0;
std::uintptr_t Thread(){std::uintptr_t v;asm volatile("mrs %0,tpidrro_el0":"=r"(v));return v;}
double Ms(std::uint64_t ticks){return static_cast<double>(ticks)*1000.0/static_cast<double>(sFrequency)/kFrames;}
double Per(std::uint64_t n){return static_cast<double>(n)/kFrames;}
double Ns(std::uint64_t ticks,std::uint64_t verts){
    return verts?static_cast<double>(ticks)*1e9/static_cast<double>(sFrequency)/static_cast<double>(verts):0.0;
}
void Flush(unsigned cat,const Acc& a){
    if(sLogs>=kMaxLogs)return;
    ++sLogs;
    const std::uint64_t cv=a.ticks[BConv0]+a.ticks[BConv1]+a.ticks[BConv2],vv=a.verts[0]+a.verts[1]+a.verts[2];
    Logging.Log("[GalaxyCoop:smg1] DIAG-0210 producer cost state=%s frames=%u flush=(%.2f,%.0f) batch=(%.2f,%.0f) "
        "custom=(%.2f,%.0f) dl=(%.2f,%.0f) conv=(%.3f,%.3f,%.3f) conv_calls=(%.0f,%.0f,%.0f) verts=(%.0f,%.0f,%.0f) "
        "conv_ns_per_vert=%.1f ab=(%lu,%lu,%.1f,%.1f) kill=1",
        kCategory[cat],kFrames,Ms(a.ticks[BFlush]),Per(a.calls[BFlush]),Ms(a.ticks[BBatch]),Per(a.calls[BBatch]),
        Ms(a.ticks[BCustom]),Per(a.calls[BCustom]),Ms(a.ticks[BDl]),Per(a.calls[BDl]),
        Ms(a.ticks[BConv0]),Ms(a.ticks[BConv1]),Ms(a.ticks[BConv2]),
        Per(a.calls[BConv0]),Per(a.calls[BConv1]),Per(a.calls[BConv2]),
        Per(a.verts[0]),Per(a.verts[1]),Per(a.verts[2]),Ns(cv,vv),
        a.abPairs,a.abVerts,Ns(a.abReal,a.abVerts),Ns(a.abScratch,a.abVerts));
}
// A new main-loop frame id starts the next frame for the category it belongs
// to; kFrames frames of one category make a line.
void Roll(){
    const auto frame=FrameTimelineFrameId();
    if(frame==sFrame)return;
    sFrame=frame;
    sCat=FrameTimelineSplit()?1u:0u;
    auto& a=sAcc[sCat];
    if(a.frames>=kFrames){Flush(sCat,a);a=Acc{};}
    ++a.frames;
}
template<unsigned B,class F>std::uintptr_t Timed(F orig){
    if(!ProducerCostActive())return orig();
    Roll();
    const auto start=ProducerCostClock();
    const auto r=orig();
    auto& a=sAcc[sCat];
    a.ticks[B]+=ProducerCostClock()-start;
    ++a.calls[B];
    return r;
}
using U=std::uintptr_t;
HOOK_DEFINE_TRAMPOLINE(CostFlush){
    static U Callback(U a,U b,U c,U d,U e,U f,U g,U h){return Timed<BFlush>([&]{return Orig(a,b,c,d,e,f,g,h);});}
};
HOOK_DEFINE_TRAMPOLINE(CostBatch){
    static U Callback(U a,U b,U c,U d,U e,U f,U g,U h){return Timed<BBatch>([&]{return Orig(a,b,c,d,e,f,g,h);});}
};
HOOK_DEFINE_TRAMPOLINE(CostCustom){
    static U Callback(U a,U b,U c,U d,U e,U f,U g,U h){return Timed<BCustom>([&]{return Orig(a,b,c,d,e,f,g,h);});}
};
HOOK_DEFINE_TRAMPOLINE(CostDl){
    static U Callback(U a,U b,U c,U d,U e,U f,U g,U h){return Timed<BDl>([&]{return Orig(a,b,c,d,e,f,g,h);});}
};
}
bool ProducerCostActive(){
    const auto producer=FrameTimelineProducerThread();
    return producer&&sFrequency&&producer==Thread();
}
std::uint64_t ProducerCostClock(){std::uint64_t v;asm volatile("mrs %0,cntpct_el0":"=r"(v)::"memory");return v;}
void ProducerCostConvert(unsigned kind,std::uint64_t ticks,std::uint32_t verts,bool sampled,std::uint64_t scratchTicks){
    auto& a=sAcc[sCat];
    a.ticks[BConv0+kind]+=ticks;++a.calls[BConv0+kind];a.verts[kind]+=verts;
    if(sampled){++a.abPairs;a.abVerts+=verts;a.abReal+=ticks;a.abScratch+=scratchTicks;}
}
void InstallProducerCost(){
    asm volatile("mrs %0,cntfrq_el0":"=r"(sFrequency));
    CostFlush::InstallAtOffset(kFlush);
    CostBatch::InstallAtOffset(kBatch);
    CostCustom::InstallAtOffset(kCustom);
    CostDl::InstallAtOffset(kDl);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0210 ATT-0194 producer cost installed flush=0x6DF0 batch=0x49FC0 custom=0x883C dl=0x7194 "
        "frames=%u freq=%lu kill=1",kFrames,sFrequency);
}
}
#endif
