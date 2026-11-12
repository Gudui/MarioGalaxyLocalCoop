#pragma once
#include <cstdint>
#include <cstring>
namespace mod::patches {
// Observational equality only: deliberately NOT a safe command-elision cache.
// Native pipeline/context changes may require identical arguments again.
template<unsigned Bytes> struct RectangleArgumentHistory {
    std::uintptr_t command=0;
    bool valid=false;
    unsigned char payload[Bytes]{};
    void Reset() { valid=false; }
    bool Observe(std::uintptr_t cmd,std::uint32_t first,std::uint32_t count,const void* data) {
        if(!cmd || first || count!=1 || !data) { Reset();return false; }
        const bool repeated=valid && command==cmd && std::memcmp(payload,data,Bytes)==0;
        std::memcpy(payload,data,Bytes); command=cmd; valid=true;
        return repeated;
    }
};
}
