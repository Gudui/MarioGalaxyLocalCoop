#pragma once
#include <cstdint>
namespace mod::patches {
// DIAG-0210: inclusive GX producer time per frame in the command processor's
// children, plus converter time against a plain-memory destination. Log-only;
// needs the DIAG-0208 frame timeline. Normal builds omit the instrument.
#if defined(GALAXY_PRODUCER_COST_DIAG) && GALAXY_PRODUCER_COST_DIAG
void InstallProducerCost();
// True on the GX producer thread once DIAG-0208 has seen it publish.
bool ProducerCostActive();
std::uint64_t ProducerCostClock();
// One converter call: kind 0..2, ticks spent writing the real destination and,
// when sampled, the same conversion into a plain-memory scratch buffer.
void ProducerCostConvert(unsigned kind,std::uint64_t ticks,std::uint32_t verts,bool sampled,std::uint64_t scratchTicks);
#else
inline void InstallProducerCost() {}
#endif
}
