#pragma once
#include <cstdint>
namespace mod::patches {
#if defined(GALAXY_VERTEX_BATCH_DIAG) && GALAXY_VERTEX_BATCH_DIAG
struct VertexBatchMetric { std::uint64_t ticks=0,maximum=0,calls=0; };
struct VertexBatchRow {
    std::uint64_t firstFrame=0,lastFrame=0,start=0,end=0,draws=0,bypasses=0,same=0,flags=0;
    VertexBatchMetric metric[5]{}; // indexed, Other, input callbacks, native flush, clock pair
    std::uint64_t eligibleCalls=0,eligibleDraws=0,nativeCalls=0,queuedSlots=0,failures=0,drawCommands=0;
};
struct VertexBatchBlock {
    std::uint64_t magic=0x31564253434F4F50ULL,version=1,size=sizeof(VertexBatchBlock);
    std::uint64_t request=0,state=0,mode=0,count=0,rejected=0;
    std::uint64_t frequency=0,backend=0,thread=0,sequence=0,shadowPassed=0,shadowRemoved=0;
    VertexBatchRow rows[240]{};
};
static_assert(sizeof(VertexBatchRow)==232 && sizeof(VertexBatchBlock)==55792);
extern "C" VertexBatchBlock gGalaxyVertexBatch;
void VertexBatchBoundary(std::uintptr_t backend);
void InstallVertexBatch();
#else
inline void VertexBatchBoundary(std::uintptr_t) {}
inline void InstallVertexBatch() {}
#endif
}
