#include "split_timing.hpp"

#if defined(GALAXY_SPLIT_TIMING_DIAG) && GALAXY_SPLIT_TIMING_DIAG
#include <algorithm>

namespace mod::patches {
extern "C" { SplitTimingBlock gGalaxySplitTiming{}; }
namespace {
SplitTimingRow sRow{};
bool sActive = false;
unsigned sMode = 0;

std::uint64_t Clock() {
    std::uint64_t ticks;
    // Compiler ordering only: do not introduce a GPU/CPU memory barrier.
    // Eden0.2.1 multicore uses a wall clock for CNTPCT; validate frequency
    // against host elapsed time before converting this test's ticks to ms.
    asm volatile("mrs %0, cntpct_el0" : "=r"(ticks) :: "memory");
    return ticks;
}
std::uint64_t Frequency() {
    std::uint64_t ticks;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(ticks));
    return ticks;
}
bool IsTwin(SplitTimingMetric m) {
    return m == SplitTimingMetric::TwinSelector || m == SplitTimingMetric::TwinProducer;
}
}

void SplitTimingBeginFrame(bool eligible, unsigned mode) {
    auto& b = gGalaxySplitTiming;
    sActive = false;
    ++b.sequence;
    if (b.request == 1 || b.request == 2) {
        b.mode = b.request;
        b.request = 0;
        b.state = 1;
        b.count = b.rejected = 0;
        b.frequency = Frequency();
    }
    if (b.state != 1 || !eligible || b.target != mode || b.count >= kSplitTimingFrames) return;
    sRow = {};
    sRow.sequence = b.sequence;
    // bit0: timed window, bit1: twin scopes sampled (every eighth row).
    sRow.flags = (b.mode == 1 ? 1u : 0u) | ((b.count % 8) == 0 ? 2u : 0u);
    sRow.start = Clock();
    sMode = mode;
    sActive = true;
    if (b.mode == 1) {
        SplitTimingScope calibration(SplitTimingMetric::ClockPair);
    }
}

void SplitTimingFinishFrame(bool eligible, unsigned mode) {
    if (!sActive) return;
    sRow.end = Clock();
    sActive = false;
    auto& b = gGalaxySplitTiming;
    // A debugger pause during a call must not be counted as its render cost.
    // No pauses are permitted during a measurement window; this only rejects
    // gross contamination, reversed clocks and mode transitions.
    if (!eligible || mode != sMode || !b.frequency || sRow.end < sRow.start ||
        sRow.end - sRow.start > b.frequency / 5) {
        ++b.rejected;
        return;
    }
    b.rows[b.count++] = sRow;
    if (b.count == kSplitTimingFrames) b.state = 2;
}

SplitTimingScope::SplitTimingScope(SplitTimingMetric m) :
    metric(m), active(sActive && (!IsTwin(m) || (sRow.flags & 2))),
    timed(active && (sRow.flags & 1)), start(timed ? Clock() : 0) {}

SplitTimingScope::~SplitTimingScope() {
    if (!active) return;
    const auto elapsed = timed ? Clock() - start : 0;
    auto& a = sRow.metric[static_cast<unsigned>(metric)];
    a.ticks += elapsed;
    a.maximum = std::max(a.maximum, elapsed);
    ++a.calls;
}
} // namespace mod::patches
#endif
