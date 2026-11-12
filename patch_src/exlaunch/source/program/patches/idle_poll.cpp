#include "idle_poll.hpp"
#if defined(GALAXY_IDLE_POLL) && GALAXY_IDLE_POLL
#include "lib.hpp"
#include "program/loggers.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
namespace mod::patches {
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// PATCH-0196 / ATT-0202 (EVD-0489): when a step finds nothing to do, the GX
// producer (5048, bl at 517C) and consumer (4694, bl at 46DC) call the wrapper
// 5328, a tail branch to the nn::os::SleepThread import stub 5F9090, with a
// TimeSpan of 100 ns. On a Switch that is a microsecond-scale pause; on Eden the
// sleep rounds up to its timer granularity, so every poll costs ~0.1 ms
// (DIAG-0211: 81-112 us per idle step) on the split's critical path.
// Install times a few svcSleepThread(100 ns): only if they take >= kCoarseUs is
// the stub patched, so a console keeps the native code. Then, in split frames
// only, those two call sites yield (svcSleepThread(0), no timer) for up to
// kBudgetUs of one idle run and use the native sleep beyond it, so
// lower-priority threads on the two cores still run. Every other call goes on
// exactly as the PLT entry would: x16 = GOT slot 919388, x17 = its contents,
// br x17. Main's imports bind lazily, so the slot may still hold the resolver
// during install; neither it nor an rtld symbol lookup may be called directly
// (both crashed at boot, EVD-0490). The stub is one 16-byte PLT entry entered
// only at its start; its words are checked (ghidra_idle_sleep_plt_20261018).
constexpr bool kIdlePollEnabled=true;
constexpr std::uintptr_t kSleepPlt=0x5F9090,kSleepGot=0x919388;
constexpr std::uintptr_t kProducerReturn=0x5180,kConsumerReturn=0x46E0;
constexpr std::uint32_t kPltWords[4]={0x90001910,0xF941C611,0x910E2210,0xD61F0220};
constexpr std::int64_t kPollNs=100,kYield=-1;
// Eden's boot calibration has measured medians of 44 and 286 us; a console wakes
// from a 100 ns sleep within a few microseconds (EVD-0490/0492).
constexpr unsigned kCoarseUs=25,kBudgetUs=200,kRunGapUs=300,kCalibrations=16,kLogEvery=600,kMaxLogs=40;
bool sCoarse=false;
std::uintptr_t sBase=0;
std::uint64_t sFrequency=19200000,sBudgetTicks=0,sRunGapTicks=0;
std::atomic<bool> sSplit{false};
// Each site is polled by one thread only; counters are read by main for logs.
// A run continues while calls arrive within kRunGapUs of the previous one
// (a native Eden sleep returns after ~0.1 ms, a busy step usually takes longer).
struct Site{
    std::uint64_t runStart,lastEntry;
    bool exhausted;
    std::atomic<std::uint32_t> yields,sleeps,runs,exhaustedRuns;
};
Site sSites[2]{};
std::uint32_t sFrames=0,sLogs=0;
std::uint64_t Clock(){std::uint64_t v;asm volatile("mrs %0,cntpct_el0":"=r"(v)::"memory");return v;}
std::uint64_t Us(std::uint64_t us){return us*sFrequency/1000000;}
std::int64_t Decide(Site& s){
    const auto now=Clock();
    if(now-s.lastEntry>sRunGapTicks){
        s.runStart=now;s.exhausted=false;
        s.runs.fetch_add(1,std::memory_order_relaxed);
    }
    s.lastEntry=now;
    if(!s.exhausted&&now-s.runStart<sBudgetTicks){
        s.yields.fetch_add(1,std::memory_order_relaxed);
        return kYield;
    }
    if(!s.exhausted){s.exhausted=true;s.exhaustedRuns.fetch_add(1,std::memory_order_relaxed);}
    s.sleeps.fetch_add(1,std::memory_order_relaxed);
    return kPollNs;
}
void LogStats(){
    std::uint32_t v[2][4];
    for(int i=0;i<2;++i){
        auto& s=sSites[i];
        v[i][0]=s.yields.exchange(0,std::memory_order_relaxed);v[i][1]=s.sleeps.exchange(0,std::memory_order_relaxed);
        v[i][2]=s.runs.exchange(0,std::memory_order_relaxed);v[i][3]=s.exhaustedRuns.exchange(0,std::memory_order_relaxed);
    }
    Logging.Log("[GalaxyCoop:smg1] PATCH-0196 idle polls frames=%u producer=(yields=%u sleeps=%u runs=%u exhausted=%u) "
        "consumer=(yields=%u sleeps=%u runs=%u exhausted=%u) kill=1",
        sFrames,v[0][0],v[0][1],v[0][2],v[0][3],v[1][0],v[1][1],v[1][2],v[1][3]);
}
}
extern "C" {
std::uintptr_t gGalaxySleepGot=0;
void GalaxySleepStub();
// Called from the stub with the caller's return address: kYield, or the
// TimeSpan to forward to nn::os::SleepThread.
std::int64_t GalaxySleepPolicy(std::uintptr_t ret,std::int64_t ns){
    if(ns!=kPollNs||!sSplit.load(std::memory_order_relaxed))return ns;
    const auto offset=ret-sBase;
    if(offset==kProducerReturn)return Decide(sSites[0]);
    if(offset==kConsumerReturn)return Decide(sSites[1]);
    return ns;
}
}
// Entered by the absolute jump written over the PLT entry, with the PLT's
// register contract: x0 = TimeSpan, x30 = the caller's return, x16/x17 free.
asm(".pushsection .text\n.balign 4\n.global GalaxySleepStub\n.type GalaxySleepStub, %function\n"
    "GalaxySleepStub:\n"
    "stp x29,x30,[sp,#-16]!\nmov x29,sp\n"
    "mov x1,x0\nmov x0,x30\n"
    "bl GalaxySleepPolicy\n"
    "ldp x29,x30,[sp],#16\n"
    "cmn x0,#1\nb.ne 1f\n"
    "mov x0,#0\nsvc #0xb\nret\n"  // yield without a timer, back to the caller
    "1:\n"
    "adrp x16,gGalaxySleepGot\nldr x16,[x16,:lo12:gGalaxySleepGot]\n"
    "ldr x17,[x16]\nbr x17\n"  // what the PLT entry does
    ".size GalaxySleepStub, .-GalaxySleepStub\n.popsection\n");
void IdlePollFrame(bool split){
    if(!sCoarse)return;
    sSplit.store(split,std::memory_order_relaxed);
    if(!split)return;
    if(++sFrames>=kLogEvery){if(sLogs<kMaxLogs){++sLogs;LogStats();}sFrames=0;}
}
void InstallIdlePoll(){
    if(!kIdlePollEnabled)return;
    sBase=exl::util::modules::GetTargetStart();
    const auto* plt=reinterpret_cast<const std::uint32_t*>(sBase+kSleepPlt);
    for(unsigned i=0;i<4;++i)if(plt[i]!=kPltWords[i]){
        Logging.Log("[GalaxyCoop:smg1] PATCH-0196 idle polls refused word%u=0x%08x kill=1",i,plt[i]);
        return;
    }
    asm volatile("mrs %0,cntfrq_el0":"=r"(sFrequency));
    if(!sFrequency)sFrequency=19200000;
    sBudgetTicks=Us(kBudgetUs);sRunGapTicks=Us(kRunGapUs);
    // Calibrate with the kernel call itself: median of a few 100 ns sleeps.
    std::uint64_t t[kCalibrations];
    for(unsigned i=0;i<kCalibrations;++i){const auto a=Clock();svcSleepThread(kPollNs);t[i]=Clock()-a;}
    std::sort(t,t+kCalibrations);
    const auto us=[](std::uint64_t ticks){return static_cast<double>(ticks)*1e6/static_cast<double>(sFrequency);};
    sCoarse=us(t[kCalibrations/2])>=kCoarseUs;
    if(sCoarse){
        gGalaxySleepGot=sBase+kSleepGot;
        exl::patch::CodePatcher p(kSleepPlt);
        p.Write<std::uint32_t>(0x58000051);  // ldr x17, #8
        p.Write<std::uint32_t>(0xD61F0220);  // br x17
        p.Write<std::uint64_t>(reinterpret_cast<std::uint64_t>(&GalaxySleepStub));
    }
    Logging.Log("[GalaxyCoop:smg1] PATCH-0196 idle polls %s sleep100ns_median_us=%.1f min_us=%.1f max_us=%.1f "
        "coarse=%d budget_us=%u run_gap_us=%u sites=0x517C/0x46DC kill=1",
        sCoarse?"installed":"native (fine-grained sleep)",us(t[kCalibrations/2]),us(t[0]),us(t[kCalibrations-1]),
        sCoarse?1:0,kBudgetUs,kRunGapUs);
}
}
#endif
