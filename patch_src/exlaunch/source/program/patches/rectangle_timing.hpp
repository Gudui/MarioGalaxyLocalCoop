#pragma once
#include <cstdint>
namespace mod::patches {
#if defined(GALAXY_RECTANGLE_TIMING_DIAG) && GALAXY_RECTANGLE_TIMING_DIAG
struct RectangleMetric { std::uint64_t ticks,maximum,calls; };
struct RectangleRow {
    std::uint64_t firstFrame,lastFrame,start,end,draws,bypasses,same,flags;
    RectangleMetric metric[3]; // viewport, scissor, clock pair
    RectangleMetric repeated[2]; // same arguments, NOT safe-to-skip calls
    std::uint64_t unsupported[2];
};
struct RectangleBlock {
    std::uint64_t magic=0x31544352434F4F50ULL,version=1,size=sizeof(RectangleBlock);
    std::uint64_t request=0,state=0,mode=0,count=0,rejected=0;
    std::uint64_t frequency=0,backend=0,thread=0,sequence=0;
    RectangleRow rows[240]{};
};
static_assert(sizeof(RectangleRow)==200);
static_assert(sizeof(RectangleBlock)==48096);
extern "C" RectangleBlock gGalaxyRectangleTiming;
void RectangleBoundary(std::uintptr_t backend);
void InstallRectangleTiming();
#else
inline void RectangleBoundary(std::uintptr_t) {}
inline void InstallRectangleTiming() {}
#endif
}
