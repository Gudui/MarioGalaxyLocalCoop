#pragma once
#include <cstdint>
namespace mod::patches {
#if defined(GALAXY_DRAW_COMMAND_TIMING_DIAG) && GALAXY_DRAW_COMMAND_TIMING_DIAG
struct DrawCommandTimingMetric { std::uint64_t ticks=0, maximum=0, calls=0; };
struct DrawCommandTimingRow {
    std::uint64_t firstFrame=0,lastFrame=0,start=0,end=0,draws=0,bypasses=0,same=0,flags=0;
    DrawCommandTimingMetric metric[4]{}; // indexed, other, nested draw command producer, clock pair
};
struct DrawCommandTimingBlock {
    std::uint64_t magic=0x31574453434F4F50ULL,version=1,size=sizeof(DrawCommandTimingBlock);
    std::uint64_t request=0,state=0,mode=0,count=0,rejected=0;
    std::uint64_t frequency=0,backend=0,thread=0,sequence=0;
    DrawCommandTimingRow rows[240]{};
};
static_assert(sizeof(DrawCommandTimingRow)==160 && sizeof(DrawCommandTimingBlock)==38496);
extern "C" DrawCommandTimingBlock gGalaxyDrawCommandTiming;
void DrawCommandBoundary(std::uintptr_t backend);
void InstallDrawCommandTiming();
#else
inline void DrawCommandBoundary(std::uintptr_t) {}
inline void InstallDrawCommandTiming() {}
#endif
}
