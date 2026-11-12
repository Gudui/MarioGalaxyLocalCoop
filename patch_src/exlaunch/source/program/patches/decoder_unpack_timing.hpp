#pragma once
#include <cstdint>
namespace mod::patches {
#if defined(GALAXY_DECODER_UNPACK_TIMING_DIAG) && GALAXY_DECODER_UNPACK_TIMING_DIAG
struct DecoderUnpackRow {
    std::uint64_t firstFrame=0,lastFrame=0,start=0,end=0,mode=0;
    std::uint64_t draws=0,decoderDraws=0,backendDraws=0,preTicks=0,preMaximum=0;
    std::uint64_t opcode2=0,opcode3=0,clockTicks=0,clockPairs=0,flags=0,reserved=0;
};
struct DecoderUnpackBlock {
    std::uint64_t magic=0x314B5044434F4F50ULL,version=1,size=sizeof(DecoderUnpackBlock);
    std::uint64_t request=0,state=0,mode=1,count=0,rejected=0;
    std::uint64_t frequency=0,backend=0,thread=0,sequence=0;
    DecoderUnpackRow rows[240]{};
};
static_assert(sizeof(DecoderUnpackRow)==128&&sizeof(DecoderUnpackBlock)==30816);
extern "C" DecoderUnpackBlock gGalaxyDecoderUnpack;
void DecoderUnpackBoundary(std::uintptr_t);
void InstallDecoderUnpackTiming();
#else
inline void DecoderUnpackBoundary(std::uintptr_t) {}
inline void InstallDecoderUnpackTiming() {}
#endif
}
