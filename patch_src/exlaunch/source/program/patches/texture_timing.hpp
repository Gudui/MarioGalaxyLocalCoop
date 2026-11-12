#pragma once
#include <cstdint>
namespace mod::patches {
#if defined(GALAXY_TEXTURE_TIMING_DIAG) && GALAXY_TEXTURE_TIMING_DIAG
struct TextureTimingMetric { std::uint64_t ticks=0, maximum=0, calls=0; };
struct TextureTimingRow {
    std::uint64_t firstFrame=0,lastFrame=0,start=0,end=0,draws=0,bypasses=0,same=0,flags=0;
    TextureTimingMetric metric[4]{}; // indexed, other, nested texture producer, clock pair
#if defined(GALAXY_TEXTURE_CLEAN_FAST) && GALAXY_TEXTURE_CLEAN_FAST
    std::uint64_t clean=0,shadow=0,fast=0,mismatch=0,route=0;
#endif
};
struct TextureTimingBlock {
    std::uint64_t magic=0x31585453434F4F50ULL,
#if defined(GALAXY_TEXTURE_CLEAN_FAST) && GALAXY_TEXTURE_CLEAN_FAST
        version=2,
#else
        version=1,
#endif
        size=sizeof(TextureTimingBlock);
    std::uint64_t request=0,state=0,mode=0,count=0,rejected=0;
    std::uint64_t frequency=0,backend=0,thread=0,sequence=0;
    TextureTimingRow rows[240]{};
};
#if defined(GALAXY_TEXTURE_CLEAN_FAST) && GALAXY_TEXTURE_CLEAN_FAST
static_assert(sizeof(TextureTimingRow)==200 && sizeof(TextureTimingBlock)==48096);
struct TextureFastControl {
    std::uint64_t magic=0x31544653434F4F50ULL,version=1,size=sizeof(TextureFastControl);
    std::uint64_t request=0,mode=0,failures=0,shadowPassed=0,refOccurrences=0;
};
static_assert(sizeof(TextureFastControl)==64);
extern "C" TextureFastControl gGalaxyTextureFast;
#else
static_assert(sizeof(TextureTimingRow)==160 && sizeof(TextureTimingBlock)==38496);
#endif
extern "C" TextureTimingBlock gGalaxyTextureTiming;
void TextureBoundary(std::uintptr_t backend);
void InstallTextureTiming();
#else
inline void TextureBoundary(std::uintptr_t) {}
inline void InstallTextureTiming() {}
#endif
}
