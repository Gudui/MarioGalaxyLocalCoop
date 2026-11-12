#pragma once
#include <cstdint>
#include <cstddef>
namespace mod::patches {
#if defined(GALAXY_DRAW_ASSOCIATION_DIAG) && GALAXY_DRAW_ASSOCIATION_DIAG
// A bounded observational record, never a replay command or equality oracle.
struct DrawAssociationRow {
    std::uint64_t kind, bank, command, target, pipeline, pipelineIndex;
    std::uint64_t vertexId, fragmentId, topology, keyFingerprint;
    std::uint64_t descriptorCursor, descriptor;
    std::uint32_t arguments[6]; // indices, instances, firstIndex, vertexOffset bits, firstInstance, scale
    std::uint32_t viewport[6], scissor[3], reserved;
    std::uint64_t offsets[9], fingerprints[9];
    std::uint8_t header[24], attributes[0x2A8];
};
struct DrawAssociationBlock {
    std::uint64_t magic=0x31415344434F4F50ULL, version=1, size=sizeof(DrawAssociationBlock);
    std::uint64_t request=0, state=0, count=0, flags=0, backend=0, thread=0;
    std::uint64_t firstFrame=0, lastFrame=0, parentCalls=0, nativeCalls=0, sequence=0;
    std::uint64_t capacity=2048, rowSize=sizeof(DrawAssociationRow), completedDraws=0;
    DrawAssociationRow rows[2048]{};
};
static_assert(sizeof(DrawAssociationRow)==1008);
extern "C" DrawAssociationBlock gGalaxyDrawAssociation;
void DrawAssociationBoundary(std::uintptr_t backend);
void InstallDrawAssociation();
#else
inline void DrawAssociationBoundary(std::uintptr_t) {}
inline void InstallDrawAssociation() {}
#endif
}
