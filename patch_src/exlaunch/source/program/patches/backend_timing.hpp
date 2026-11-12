#pragma once
#include <cstdint>
namespace mod::patches {
#if defined(GALAXY_BACKEND_TIMING_DIAG) && GALAXY_BACKEND_TIMING_DIAG
// DIAG-0196: mod-owned single-worker storage; arm only while GDB-paused.
// Request1 timed,2 counts-only control. Native render state is never written.
struct BackendTimingMetric { std::uint64_t ticks, maximum, calls; };
struct BackendTimingRow {
    std::uint64_t firstFrame, lastFrame, start, end, draws, bypasses, same, flags;
    BackendTimingMetric metric[4]; // indexed, other, nested lookup, clock pair
};
struct BackendTimingBlock {
    std::uint64_t magic = 0x31544253434F4F50ULL, version = 1;
    std::uint64_t size = sizeof(BackendTimingBlock);
    std::uint64_t request = 0, state = 0, mode = 0, count = 0, rejected = 0;
    std::uint64_t frequency = 0, backend = 0, thread = 0, sequence = 0;
    BackendTimingRow rows[240]{};
};
static_assert(sizeof(BackendTimingRow) == 160);
static_assert(sizeof(BackendTimingBlock) == 38496);
extern "C" BackendTimingBlock gGalaxyBackendTiming;
#if defined(GALAXY_DESCRIPTOR_TIMING_DIAG) && GALAXY_DESCRIPTOR_TIMING_DIAG
// ATT0163: nested native descriptor API durations, same qualified frame join.
// Default idle, no retained native handles or native state writes.
struct DescriptorProbeRow {
    std::uint64_t frame;
    BackendTimingMetric metric[2]; // update, bind
};
struct DescriptorProbeBlock {
    std::uint64_t magic=0x31534453434F4F50ULL, version=1;
    std::uint64_t size=sizeof(DescriptorProbeBlock), count=0;
    DescriptorProbeRow rows[240]{};
};
static_assert(sizeof(DescriptorProbeRow)==56);
static_assert(sizeof(DescriptorProbeBlock)==13472);
extern "C" DescriptorProbeBlock gGalaxyDescriptorProbe;
#endif
#if defined(GALAXY_PUSH_CONSTANT_EXPERIMENT) && GALAXY_PUSH_CONSTANT_EXPERIMENT
// ATT0159: request mode only while paused. 0 passthrough,1 shadow,2 filter.
// Rows join the exact qualified DIAG0196 frame rows. No native field writes.
struct PushProbeRow {
    std::uint64_t frame, calls, duplicates, skipped, nativeTicks,
        duplicateTicks, cacheTicks, invalidations;
};
struct PushProbeBlock {
    std::uint64_t magic=0x3153485355434F50ULL, version=1;
    std::uint64_t size=sizeof(PushProbeBlock), mode=0, count=0;
    PushProbeRow rows[240]{};
};
static_assert(sizeof(PushProbeBlock)==15400);
extern "C" PushProbeBlock gGalaxyPushProbe;
#endif
void InstallBackendTiming();
#else
inline void InstallBackendTiming() {}
#endif
}
