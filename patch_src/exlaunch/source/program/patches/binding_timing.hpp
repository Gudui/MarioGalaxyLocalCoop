#pragma once
#include <cstdint>
namespace mod::patches {
#if defined(GALAXY_BINDING_TIMING_DIAG) && GALAXY_BINDING_TIMING_DIAG
struct BindingTimingMetric { std::uint64_t ticks=0, maximum=0, calls=0; };
struct BindingTimingRow {
    std::uint64_t firstFrame=0,lastFrame=0,start=0,end=0,draws=0,bypasses=0,same=0,flags=0;
    BindingTimingMetric metric[7]{}; // indexed, other, pipeline, index, vertex, native draw count, clock pair
};
struct BindingTimingBlock {
    std::uint64_t magic=0x31444E42434F4F50ULL,version=1,size=sizeof(BindingTimingBlock);
    std::uint64_t request=0,state=0,mode=0,count=0,rejected=0;
    std::uint64_t frequency=0,backend=0,thread=0,sequence=0;
    BindingTimingRow rows[240]{};
};
static_assert(sizeof(BindingTimingRow)==232 && sizeof(BindingTimingBlock)==55776);
extern "C" BindingTimingBlock gGalaxyBindingTiming;
void BindingBoundary(std::uintptr_t backend);
void InstallBindingTiming();
#else
inline void BindingBoundary(std::uintptr_t) {}
inline void InstallBindingTiming() {}
#endif
}
