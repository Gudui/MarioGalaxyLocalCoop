#include "rectangle_timing.hpp"
#if defined(GALAXY_RECTANGLE_TIMING_DIAG) && GALAXY_RECTANGLE_TIMING_DIAG
#if !defined(GALAXY_PUSH_CONSTANT_FILTER) || !GALAXY_PUSH_CONSTANT_FILTER
#error Rectangle diagnostic requires qualified production frame owner
#endif
#if defined(GALAXY_RETIREMENT_TIMING_DIAG) && GALAXY_RETIREMENT_TIMING_DIAG
#error Fulfilled retirement diagnostic must be omitted
#endif
#include "push_constant_filter.hpp"
#include "rectangle_argument_history.hpp"
#include "patch_shared.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <atomic>
#include <algorithm>
namespace mod::patches {
extern "C" { RectangleBlock gGalaxyRectangleTiming{}; }
// EVD0385: untouched main GOT, preserve x16 for native PLT lazy binding.
// Original API x0 command,x1 first,x2 count,x3 payload; extra x4 is ignored.
extern "C" void GalaxyRectangleNativeImport(std::uintptr_t,std::uint32_t,std::uint32_t,const void*,std::uintptr_t);
asm(".pushsection .text\n.balign 4\n.global GalaxyRectangleNativeImport\n.type GalaxyRectangleNativeImport, %function\n"
    "GalaxyRectangleNativeImport:\nmov x16,x4\nldr x17,[x16]\nbr x17\n"
    ".size GalaxyRectangleNativeImport, .-GalaxyRectangleNativeImport\n.popsection\n");
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// EVD0385 aligned16B ADRP/LDR/ADD/BR imports: overwrite, never trampoline.
// EVD0386 native payload strides24/16, first0/count1 observational subset.
constexpr std::uintptr_t kViewport=0x5F9EE0,kViewportGot=0x919AB0;
constexpr std::uintptr_t kScissor=0x5F9EF0,kScissorGot=0x919AB8,kBackendVT=0x889C68;
RectangleArgumentHistory<24> sViewport{};
RectangleArgumentHistory<16> sScissor{};
RectangleRow sRow{};
bool sActive=false;
unsigned sDepth=0;
std::atomic<std::uint64_t> sForeign{0};
std::uint64_t sFirstForeign=0;
std::uint64_t Clock() { std::uint64_t v;asm volatile("mrs %0, cntpct_el0":"=r"(v)::"memory");return v; }
std::uintptr_t Thread() { std::uintptr_t v;asm volatile("mrs %0, tpidrro_el0":"=r"(v));return v; }
template<class T> T Read(std::uintptr_t p,std::uintptr_t off) { return *reinterpret_cast<const T*>(p+off); }
bool Owned() { return gGalaxyPushFilter.thread && Thread()==gGalaxyPushFilter.thread; }
bool Backend(std::uintptr_t p) { return IsGuestPointer(p)&&Read<std::uintptr_t>(p,0)==exl::util::modules::GetTargetStart()+kBackendVT; }
void Add(RectangleMetric& m,std::uint64_t ticks) { m.ticks+=ticks;m.maximum=std::max(m.maximum,ticks);++m.calls; }
template<unsigned Kind,unsigned Bytes> void ObserveAndForward(RectangleArgumentHistory<Bytes>& history,
    std::uintptr_t command,std::uint32_t first,std::uint32_t count,const void* data,std::uintptr_t got) {
    const auto native=[&] { GalaxyRectangleNativeImport(command,first,count,data,exl::util::modules::GetTargetStart()+got); };
    if(!Owned()) { sForeign.fetch_add(1,std::memory_order_relaxed);return native(); }
    if(!sActive) return native();
    if(sDepth++) sRow.flags|=0x200;
    const bool supported=command && first==0 && count==1 && data;
    const bool repeated=history.Observe(command,first,count,data);
    sRow.unsupported[Kind]+=!supported;
    const bool timed=gGalaxyRectangleTiming.mode==1;
    const auto start=timed?Clock():0;
    native(); // Every call forwarded, every payload byte and effect preserved.
    const auto ticks=timed?Clock()-start:0;
    Add(sRow.metric[Kind],ticks);
    if(repeated) Add(sRow.repeated[Kind],ticks);
    --sDepth;
}
HOOK_DEFINE_REPLACE(RectangleViewportTiming) {
    static void Callback(std::uintptr_t command,std::uint32_t first,std::uint32_t count,const void* data) {
        ObserveAndForward<0>(sViewport,command,first,count,data,kViewportGot);
    }
};
HOOK_DEFINE_REPLACE(RectangleScissorTiming) {
    static void Callback(std::uintptr_t command,std::uint32_t first,std::uint32_t count,const void* data) {
        ObserveAndForward<1>(sScissor,command,first,count,data,kScissorGot);
    }
};
}
void RectangleBoundary(std::uintptr_t backend) {
    if(!Owned()||!Backend(backend)) return;
    auto& b=gGalaxyRectangleTiming;++b.sequence;
    if(!sActive&&b.state!=1&&b.request!=1&&b.request!=2) return;
    const auto now=Clock(),frame=Read<std::uint64_t>(backend,0x60C8);
    const auto bank=Read<std::uint32_t>(backend,0x60C0);
    if(sActive) {
        sActive=false;sRow.lastFrame=frame;sRow.end=now;
        if(sForeign.load(std::memory_order_relaxed)!=sFirstForeign) sRow.flags|=0x100;
        bool valid=b.backend==backend&&b.thread==Thread()&&bank<2&&!sDepth&&frame==sRow.firstFrame+1&&
            now>sRow.start&&b.frequency&&now-sRow.start<=b.frequency/5&&sRow.flags==b.mode;
        if(valid) {
            const auto stats=backend+0x6020+(bank^1)*0x40;
            sRow.draws=Read<std::uint32_t>(stats,0x30);sRow.bypasses=Read<std::uint32_t>(stats,0x34);sRow.same=Read<std::uint32_t>(stats,0x38);
            valid=sRow.bypasses<=sRow.same&&sRow.same<=sRow.draws;
        }
        if(valid) b.rows[b.count++]=sRow;else ++b.rejected;
        if(b.count==240||b.rejected==240) b.state=2;
    }
    if(b.request==1||b.request==2) {
        b.mode=b.request;b.request=0;b.state=1;b.count=b.rejected=0;
        asm volatile("mrs %0, cntfrq_el0":"=r"(b.frequency));
    }
    if(b.state!=1||bank>1||b.count>=240) return;
    b.backend=backend;b.thread=Thread();sRow={};sViewport.Reset();sScissor.Reset();
    sFirstForeign=sForeign.load(std::memory_order_relaxed);
    sRow.firstFrame=frame;sRow.start=now;sRow.flags=b.mode;sActive=true;
    if(b.mode==1) { const auto before=Clock();Add(sRow.metric[2],Clock()-before); }
}
void InstallRectangleTiming() {
    RectangleViewportTiming::InstallAtOffset(kViewport);
    RectangleScissorTiming::InstallAtOffset(kScissor);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0198 ATT-0174 rectangle shadow costs installed default-idle window=240 forward-all kill=1");
}
}
#endif
