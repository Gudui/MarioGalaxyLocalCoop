#pragma once
#include <cstdint>
namespace mod::patches {
// DIAG-0208 / ATT-0187: main-thread frame phase timeline (EVD-0466). Log-only,
// no GDB control block. Normal builds omit the instrument.
#if defined(GALAXY_FRAME_TIMELINE_DIAG) && GALAXY_FRAME_TIMELINE_DIAG
void FrameTimelineSceneState(bool active, float reveal, bool available);
void InstallFrameTimeline();
// Main-loop frame id (relaxed; any thread) and whether the last finished
// frame was fully split. Main waits for draw done, so producer work for a
// frame never straddles an id change.
std::uint32_t FrameTimelineFrameId();
bool FrameTimelineSplit();
// tpidrro_el0 of the GX producer thread (C86C publish), 0 until seen.
std::uintptr_t FrameTimelineProducerThread();
#else
inline void FrameTimelineSceneState(bool, float, bool) {}
inline void InstallFrameTimeline() {}
inline std::uint32_t FrameTimelineFrameId() { return 0; }
inline bool FrameTimelineSplit() { return false; }
inline std::uintptr_t FrameTimelineProducerThread() { return 0; }
#endif
}
