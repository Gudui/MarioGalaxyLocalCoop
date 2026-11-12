#pragma once
#include <cstdint>
namespace mod::patches {
#if defined(GALAXY_DYNAMIC_UNIFORM_EXPERIMENT) && GALAXY_DYNAMIC_UNIFORM_EXPERIMENT
struct DynamicUniformMetric { std::uint64_t ticks=0,maximum=0,calls=0; };
struct DynamicUniformRow {
    std::uint64_t firstFrame=0,lastFrame=0,start=0,end=0,draws=0,flags=0;
    DynamicUniformMetric metric[5]{}; // indexed,other,update adapter,bind adapter,clock pair
    std::uint64_t vertexUpdates=0,vertexBinds=0,writesRemoved=0,copiesRemoved=0;
    std::uint64_t initializations=0,repairs=0,failures=0,addressChecks=0;
};
struct DynamicUniformBlock {
    std::uint64_t magic=0x314E5559434F4F50ULL,version=1,size=sizeof(DynamicUniformBlock);
    std::uint64_t request=0,state=0,mode=1,count=0,rejected=0;
    std::uint64_t frequency=0,backend=0,thread=0,sequence=0,shadowPassed=0;
    std::uint64_t layoutsConverted=0,layoutsRefused=0,poolsExtended=0,creationFailures=0;
    DynamicUniformRow rows[240]{};
};
static_assert(sizeof(DynamicUniformRow)==232&&sizeof(DynamicUniformBlock)==55816);
extern "C" DynamicUniformBlock gGalaxyDynamicUniform;
void DynamicUniformBoundary(std::uintptr_t backend);
void InstallDynamicUniform();
#else
inline void DynamicUniformBoundary(std::uintptr_t) {}
inline void InstallDynamicUniform() {}
#endif
}
