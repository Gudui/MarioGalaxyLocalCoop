#pragma once
#include <cstdint>
namespace mod::patches {
#if defined(GALAXY_DESCRIPTOR_VALUE_EXPERIMENT) && GALAXY_DESCRIPTOR_VALUE_EXPERIMENT
struct DescriptorValueMetric { std::uint64_t ticks=0,maximum=0,calls=0; };
struct DescriptorValueRow {
    std::uint64_t firstFrame=0,lastFrame=0,start=0,end=0,draws=0,flags=0;
    DescriptorValueMetric metric[6]{}; // indexed,other,update,bind inclusive,compare nested,clock pair
    std::uint64_t vertexBinds=0,candidates=0,selected=0,bytes=0,failures=0,drawCalls=0,submissions=0,oracleChecks=0;
};
struct DescriptorValueBlock {
    std::uint64_t magic=0x31564C44434F4F50ULL,version=1,size=sizeof(DescriptorValueBlock);
    std::uint64_t request=0,state=0,mode=1,count=0,rejected=0;
    std::uint64_t frequency=0,backend=0,thread=0,sequence=0,shadowPassed=0;
    DescriptorValueRow rows[240]{};
};
static_assert(sizeof(DescriptorValueRow)==256&&sizeof(DescriptorValueBlock)==61544);
extern "C" DescriptorValueBlock gGalaxyDescriptorValue;
void DescriptorValueBoundary(std::uintptr_t backend);
void DescriptorValueInvalidate();
void InstallDescriptorValue();
#else
inline void DescriptorValueBoundary(std::uintptr_t) {}
inline void DescriptorValueInvalidate() {}
inline void InstallDescriptorValue() {}
#endif
}
