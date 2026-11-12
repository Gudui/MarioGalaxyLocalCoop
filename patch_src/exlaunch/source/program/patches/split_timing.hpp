#pragma once

#include <cstdint>

namespace mod::patches {

// DIAG-0195 / ATT-0157: transient main-thread durations, not GPU timings.
// Normal builds omit the instrument. The test build only arms via its own
// GDB control block; no native flags, sampling tokens or render calls change.
enum class SplitTimingMetric : unsigned {
    Scene, Pass0, Pass1, Rig, SamplingShared, Sampling0, Sampling1,
    TwinSelector, TwinProducer, ClockPair, Count
};

#if defined(GALAXY_SPLIT_TIMING_DIAG) && GALAXY_SPLIT_TIMING_DIAG
inline constexpr bool kSplitTimingDiagEnabled = true;
inline constexpr unsigned kSplitTimingFrames = 240;
struct SplitTimingAccumulator {
    std::uint64_t ticks, maximum, calls;
};
struct SplitTimingRow {
    std::uint64_t sequence, start, end, flags;
    SplitTimingAccumulator metric[static_cast<unsigned>(SplitTimingMetric::Count)];
};
// All storage is mod-owned. Single main-thread writer; read/write only while
// GDB has stopped the guest. Request1=timed,2=overhead control; target0=shared,
// target1=fully split. Request is consumed on the next Scene3d entry.
struct SplitTimingBlock {
    std::uint64_t magic = 0x31544753434F4F50ULL;
    std::uint64_t version = 1;
    std::uint64_t size = sizeof(SplitTimingBlock);
    std::uint64_t request = 0, target = 1, state = 0, mode = 0;
    std::uint64_t count = 0, rejected = 0, frequency = 0, sequence = 0;
    SplitTimingRow rows[kSplitTimingFrames]{};
};
static_assert(sizeof(SplitTimingRow) == 272);
static_assert(sizeof(SplitTimingBlock) == 65368);
extern "C" SplitTimingBlock gGalaxySplitTiming;
void SplitTimingBeginFrame(bool eligible, unsigned mode);
void SplitTimingFinishFrame(bool eligible, unsigned mode);
struct SplitTimingScope {
    SplitTimingMetric metric;
    bool active, timed;
    std::uint64_t start;
    explicit SplitTimingScope(SplitTimingMetric metric);
    ~SplitTimingScope();
};
#else
inline constexpr bool kSplitTimingDiagEnabled = false;
inline void SplitTimingBeginFrame(bool, unsigned) {}
inline void SplitTimingFinishFrame(bool, unsigned) {}
struct SplitTimingScope {
    explicit SplitTimingScope(SplitTimingMetric) {}
};
#endif

} // namespace mod::patches
