#pragma once
#include <cstdint>
#include <cstring>

namespace mod::patches {
// ATT0161: direct lookup replaces the measured linear hit scan. Handles are
// opaque identifiers; no saved native pointer is dereferenced. Begin/reset,
// frame boundaries and out-of-owner calls invalidate through the adapter.
class PushConstantCache {
    struct Entry {
        std::uint32_t stages=0, size=0;
        bool valid=false;
        std::uint8_t bytes[64]{};
    };
    std::uintptr_t command=0, layout=0;
    // All accepted starts are four-byte aligned in a bounded256-byte range.
    Entry entries[64]{};
    template<unsigned N> static bool EqualWords(const std::uint8_t* a, const std::uint8_t* b) {
        // memcpy gives defined unaligned loads. Read exactly N bytes, including
        // the final word; equality is bitwise (including float NaNs/signed zero).
        if constexpr (N >= 8) {
            std::uint64_t x, y;
            std::memcpy(&x,a,8); std::memcpy(&y,b,8);
            if constexpr (N == 8) return x == y;
            else return x == y && EqualWords<N-8>(a+8,b+8);
        } else {
            static_assert(N == 4);
            std::uint32_t x, y;
            std::memcpy(&x,a,4); std::memcpy(&y,b,4);
            return x == y;
        }
    }
    static bool InlineEqual(const std::uint8_t* a, const void* data, unsigned size) {
        const auto* b=static_cast<const std::uint8_t*>(data);
        // EVD0381: current seven native shapes plus auxiliary stage17,size20.
        // Other accepted sizes use the original general comparison.
        switch (size) {
            case 4: return EqualWords<4>(a,b);
            case 8: return EqualWords<8>(a,b);
            case 12: return EqualWords<12>(a,b);
            case 20: return EqualWords<20>(a,b);
            case 32: return EqualWords<32>(a,b);
            case 64: return EqualWords<64>(a,b);
            default: return std::memcmp(a,data,size)==0;
        }
    }
public:
    void Reset() {
        for (auto& entry : entries) entry.valid=false;
        command=layout=0;
    }
    bool Duplicate(std::uintptr_t cb, std::uintptr_t pl, std::uint32_t stages,
                   std::uint32_t offset, std::uint32_t size, const void* data,
                   bool inline_equal=false) {
        // Unsupported writes still reach Vulkan and invalidate prior claims.
        if (!cb || !pl || !data || !stages || (stages & ~0x3FU) ||
            !size || size>64 || offset>256 || size>256-offset ||
            (offset & 3) || (size & 3)) {
            Reset(); return false;
        }
        if (command!=cb || layout!=pl) { Reset(); command=cb; layout=pl; }
        auto& entry=entries[offset/4];
        if (entry.valid && entry.stages==stages && entry.size==size &&
            (inline_equal ? InlineEqual(entry.bytes,data,size) :
                            std::memcmp(entry.bytes,data,size)==0)) return true;
        // With a maximum64-byte entry, only aligned starts in
        // [offset-60,offset+size-4] can overlap. Disjoint stage masks remain
        // independent. Incompatible keys at the same start evict safely.
        const auto first=offset>60?(offset-60)/4:0;
        const auto last=(offset+size-4)/4;
        for (auto i=first;i<=last;++i) {
            auto& prior=entries[i];
            if (prior.valid && (prior.stages & stages) && i*4+prior.size>offset)
                prior.valid=false;
        }
        entry.stages=stages; entry.size=size;
        std::memcpy(entry.bytes,data,size); entry.valid=true;
        return false;
    }
};
}
