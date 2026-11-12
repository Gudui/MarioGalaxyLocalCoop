#pragma once
namespace mod::patches {
// PATCH-0195: faster, bit-identical position instancing loop inside the GX
// producer's custom pipeline command (F264).
#if defined(GALAXY_F264_FAST_TRANSFORM) && GALAXY_F264_FAST_TRANSFORM
void InstallF264FastTransform();
#else
inline void InstallF264FastTransform() {}
#endif
}
