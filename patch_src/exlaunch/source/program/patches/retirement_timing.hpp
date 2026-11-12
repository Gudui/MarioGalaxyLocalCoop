#pragma once
#include <cstdint>
namespace mod::patches {
#if defined(GALAXY_RETIREMENT_TIMING_DIAG) && GALAXY_RETIREMENT_TIMING_DIAG
// ATT0171: owned bounded storage. Request1 timed,2 counts-only while paused.
struct RetirementMetric { std::uint64_t ticks, maximum, calls; };
struct RetirementRow {
    std::uint64_t firstFrame,lastFrame,start,end,draws,bypasses,same,flags;
    RetirementMetric metric[3]; // retirement inclusive, nested query, clock pair
};
struct RetirementBlock {
    std::uint64_t magic=0x31545252434F4F50ULL,version=1,size=sizeof(RetirementBlock);
    std::uint64_t request=0,state=0,mode=0,count=0,rejected=0;
    std::uint64_t frequency=0,backend=0,thread=0,sequence=0;
    RetirementRow rows[240]{};
};
static_assert(sizeof(RetirementRow)==136);
static_assert(sizeof(RetirementBlock)==32736);
extern "C" RetirementBlock gGalaxyRetirementTiming;
void RetirementBoundary(std::uintptr_t backend);
void InstallRetirementTiming();
#else
inline void RetirementBoundary(std::uintptr_t) {}
inline void InstallRetirementTiming() {}
#endif
}
