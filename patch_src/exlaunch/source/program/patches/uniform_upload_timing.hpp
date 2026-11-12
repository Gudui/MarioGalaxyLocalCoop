#pragma once
#include <cstdint>
namespace mod::patches {
#if defined(GALAXY_UNIFORM_UPLOAD_TIMING_DIAG) && GALAXY_UNIFORM_UPLOAD_TIMING_DIAG
struct UniformUploadRow {
    std::uint64_t firstFrame=0,lastFrame=0,start=0,end=0,completedSceneIntervals=0,mode=0;
    std::uint64_t ticks=0,maximum=0,calls=0,clockTicks=0,clockCalls=0;
    std::uint64_t callerCalls[3]{},returnMaskOr=0,invalidCalls=0;
};
struct UniformUploadBlock {
    std::uint64_t magic=0x31504C55434F4F50ULL,version=2,size=sizeof(UniformUploadBlock);
    std::uint64_t request=0,state=0,mode=1,count=0,rejected=0,frequency=0,backend=0,thread=0,sequence=0;
    UniformUploadRow rows[240]{};
};
static_assert(sizeof(UniformUploadRow)==128&&sizeof(UniformUploadBlock)==30816);
extern "C" UniformUploadBlock gGalaxyUniformUpload;
void UniformUploadSceneFrame();
void InstallUniformUploadTiming();
#else
inline void UniformUploadSceneFrame(){}
inline void InstallUniformUploadTiming(){}
#endif
}
