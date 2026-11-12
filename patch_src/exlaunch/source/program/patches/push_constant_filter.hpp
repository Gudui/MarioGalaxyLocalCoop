#pragma once
#include <cstdint>
namespace mod::patches {
#if defined(GALAXY_PUSH_CONSTANT_FILTER) && GALAXY_PUSH_CONSTANT_FILTER
// ATT0166: production control/identity only; no duration rows or hot clocks.
struct PushFilterControl {
    std::uint64_t magic=0x31524650434F4F50ULL, version=1, size=sizeof(PushFilterControl);
    std::uint64_t mode=2, backend=0, thread=0, sequence=0, invalidations=0;
};
static_assert(sizeof(PushFilterControl)==64);
extern "C" PushFilterControl gGalaxyPushFilter;
void InstallPushConstantFilter();
#else
inline void InstallPushConstantFilter() {}
#endif
}
