#pragma once
namespace mod::patches {
// PATCH-0190 / EVD-0474: byte-identical replacements for the producer's
// hottest vertex attribute converters. Kill switch GALAXY_FAST_VERTEX_CONVERT.
#if defined(GALAXY_FAST_VERTEX_CONVERT) && GALAXY_FAST_VERTEX_CONVERT
void InstallFastVertexConvert();
#else
inline void InstallFastVertexConvert() {}
#endif
}
