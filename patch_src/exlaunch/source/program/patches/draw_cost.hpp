#pragma once
namespace mod::patches {
// DIAG-0213: GX producer cost of each per-draw step (pipeline build and hashes,
// uniform build, packet write, allocators) for ordinary draws and for the F264
// effect command, per split frame, with the heaviest frames of each window.
// Log-only; needs DIAG-0208 (frames, producer thread) and DIAG-0211 (busy).
#if defined(GALAXY_DRAW_COST_DIAG) && GALAXY_DRAW_COST_DIAG
void InstallDrawCost();
#else
inline void InstallDrawCost() {}
#endif
}
