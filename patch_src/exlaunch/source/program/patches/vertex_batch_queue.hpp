#pragma once
#include <cstdint>

namespace mod::patches {
// Only the compact first=0, count=1 sequence proven at SMG1 main26038.
// No native pointer reads here. Failed appends leave the previous run intact.
struct VertexBatchQueue {
    static constexpr unsigned Capacity=27;
    std::uintptr_t command=0;
    unsigned count=0;
    std::uintptr_t buffers[Capacity]{};
    std::uint64_t offsets[Capacity]{};
    bool Append(std::uintptr_t cmd,unsigned first,unsigned incoming,
                std::uintptr_t buffer,std::uint64_t offset) {
        if(!cmd||incoming!=1||first!=count||count>=Capacity||
           (count&&cmd!=command)) return false;
        command=cmd;buffers[count]=buffer;offsets[count]=offset;++count;
        return true;
    }
    void Clear() { command=0;count=0; }
};
}
