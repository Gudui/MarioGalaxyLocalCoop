#pragma once
#include <cstdint>
namespace mod::patches {
// PATCH-0192: in split frames the frame-end GXDrawDone releases main once the GX
// producer has parsed the draw-done command instead of after the consumer
// executed the whole frame, so the consumer tail overlaps the next frame.
#if defined(GALAXY_FRAME_OVERLAP) && GALAXY_FRAME_OVERLAP
void InstallFrameOverlap();
#if defined(GALAXY_PRODUCER_STALL_DIAG) && GALAXY_PRODUCER_STALL_DIAG
// DIAG-0211: producer busy ticks so far, counted in split frames on the producer thread.
std::uint64_t ProducerStallBusyTicks();
#endif
#else
inline void InstallFrameOverlap() {}
#endif
}
