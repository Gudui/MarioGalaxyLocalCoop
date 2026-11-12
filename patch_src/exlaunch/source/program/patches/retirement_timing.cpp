#include "retirement_timing.hpp"
#if defined(GALAXY_RETIREMENT_TIMING_DIAG) && GALAXY_RETIREMENT_TIMING_DIAG
#if !defined(GALAXY_PUSH_CONSTANT_FILTER) || !GALAXY_PUSH_CONSTANT_FILTER
#error Retirement diagnostic requires the production frame owner
#endif
#include "push_constant_filter.hpp"
#include "patch_shared.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <algorithm>
namespace mod::patches {
extern "C" { RetirementBlock gGalaxyRetirementTiming{}; }
namespace {
// Recorded SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// EVD0375:75E20 true entry SUB/STP/STR/STP;76564 true entry LDR/MOV/MOV/MOV.
// Neither opening contains a PC-relative instruction.76564 seven args tail-
// calls untouched native query import; retain its x0 result, including errors.
constexpr std::uintptr_t kRetire=0x75E20,kQuery=0x76564,kBackendVtable=0x889C68;
RetirementRow sRow{};
bool sActive=false;
unsigned sDepth=0;
std::uint64_t Clock() {
    std::uint64_t value; asm volatile("mrs %0, cntpct_el0":"=r"(value)::"memory"); return value;
}
std::uintptr_t Thread() {
    std::uintptr_t value; asm volatile("mrs %0, tpidrro_el0":"=r"(value)); return value;
}
template<class T> T Read(std::uintptr_t p,std::uintptr_t off) {
    return *reinterpret_cast<const T*>(p+off);
}
bool Owned() { return gGalaxyPushFilter.thread && Thread()==gGalaxyPushFilter.thread; }
bool Backend(std::uintptr_t p) {
    return IsGuestPointer(p)&&Read<std::uintptr_t>(p,0)==
        exl::util::modules::GetTargetStart()+kBackendVtable;
}
struct Scope {
    unsigned metric; bool active,timed; std::uint64_t start;
    Scope(unsigned m,bool eligible):metric(m),active(eligible&&Owned()&&sActive),
        timed(active&&gGalaxyRetirementTiming.mode==1),start(timed?Clock():0) {}
    ~Scope() {
        if(!active) return;
        const auto elapsed=timed?Clock()-start:0;
        auto& value=sRow.metric[metric];
        value.ticks+=elapsed;value.maximum=std::max(value.maximum,elapsed);++value.calls;
    }
};
HOOK_DEFINE_TRAMPOLINE(RetirementTiming) {
    static void Callback(std::uintptr_t backend,std::uint32_t index) {
        const bool active=Owned()&&sActive;
        const bool eligible=active&&index<12&&Backend(backend)&&
            backend==gGalaxyRetirementTiming.backend;
        if(active&&!eligible) sRow.flags|=0x100;
        if(eligible&&sDepth) sRow.flags|=0x200;
        if(eligible) ++sDepth;
        { Scope scope(0,eligible);Orig(backend,index); }
        if(eligible) --sDepth;
    }
};
HOOK_DEFINE_TRAMPOLINE(RetirementQueryTiming) {
    static std::int32_t Callback(std::uintptr_t pool,std::uint32_t first,std::uint32_t count,
                                std::uint64_t size,void* data,std::uint64_t stride,std::uint32_t flags) {
        Scope scope(1,sDepth!=0);
        return Orig(pool,first,count,size,data,stride,flags);
    }
};
}
void RetirementBoundary(std::uintptr_t backend) {
    if(!Owned()||!Backend(backend)) return;
    auto& b=gGalaxyRetirementTiming;
    ++b.sequence;
    // Default-idle production path adds no clock reads or per-call log output.
    if(!sActive&&b.state!=1&&b.request!=1&&b.request!=2) return;
    const auto now=Clock(),frame=Read<std::uint64_t>(backend,0x60C8);
    const auto bank=Read<std::uint32_t>(backend,0x60C0);
    if(sActive) {
        sActive=false;sRow.lastFrame=frame;sRow.end=now;
        bool valid=b.backend==backend&&bank<2&&!sDepth&&frame==sRow.firstFrame+1&&
            now>sRow.start&&b.frequency&&now-sRow.start<=b.frequency/5&&sRow.flags==b.mode;
        if(valid) {
            const auto stats=backend+0x6020+(bank^1)*0x40;
            sRow.draws=Read<std::uint32_t>(stats,0x30);
            sRow.bypasses=Read<std::uint32_t>(stats,0x34);
            sRow.same=Read<std::uint32_t>(stats,0x38);
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
    b.backend=backend;b.thread=Thread();sRow={};
    sRow.firstFrame=frame;sRow.start=now;sRow.flags=b.mode;sActive=true;
    if(b.mode==1) { Scope calibration(2,true); }
}
void InstallRetirementTiming() {
    RetirementTiming::InstallAtOffset(kRetire);
    RetirementQueryTiming::InstallAtOffset(kQuery);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0197 ATT-0171 retirement/query durations installed default-idle window=240 kill=1");
}
}
#endif
