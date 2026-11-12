#pragma once
#include <cstdint>
#include <cstddef>
#include <initializer_list>

namespace mod::patches::texture_clean {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// EVD0396: 26CA0 selector/copy and 261B0/64790 caller release loops.
struct Binding {
    std::uint32_t type, padding;
    std::uintptr_t ref1, ref2, handle;
    std::uint32_t scale, tailPadding;
};
struct Result { std::uint32_t mask, padding; Binding slots[8]; };
static_assert(sizeof(Binding)==40 && sizeof(Result)==328);
static_assert(offsetof(Binding,ref1)==8 && offsetof(Binding,scale)==32);

template<class T> inline T Read(std::uintptr_t p,std::uintptr_t off) {
    return *reinterpret_cast<const T*>(p+off);
}
// No heap pointer/result cache. Strict DC0==current draw narrows the native ABI.
// Validate every reference before any acquisition, so failure is side-effect free.
template<class Guard> bool Eligible(std::uintptr_t manager,std::uintptr_t holder,
                                    std::uintptr_t draw,std::uint32_t& used,Guard guard) {
    const auto word=Read<std::uint32_t>(draw,0);
    if(((word>>16)&7) || Read<std::uintptr_t>(holder,8)!=draw) return false;
    const auto slots=manager+0x30;
    for(unsigned i=0;i<8;++i) {
        const auto slot=slots+i*40;
        if(Read<std::uint32_t>(slot,0)>3) return false;
        for(const auto off:{8u,16u}) {
            const auto ref=Read<std::uintptr_t>(slot,off);
            if(ref&&!guard(ref)) return false;
        }
    }
    used=0;
    const auto dirty=Read<std::uint32_t>(holder,0x10);
    for(unsigned stage=0;stage<=((word>>10)&15);++stage) {
        const auto shift=12*(stage&1);
        const auto packed=Read<std::uint32_t>(draw,0xA0+4*(stage>>1));
        if(!(packed&(0x40u<<shift))) continue;
        const auto unit=(packed>>shift)&7;
        const auto bit=1u<<unit;
        if((dirty&bit) || (Read<std::uint32_t>(draw,0x410+(unit&~3u))&
           (0x01111111u<<(unit&3))) || !Read<std::uint32_t>(slots+unit*40,0)) return false;
        used|=bit;
    }
    return true;
}
inline void CopyDefined(Binding& dest,const Binding& src) {
    dest.type=src.type;dest.ref1=src.ref1;dest.ref2=src.ref2;
    dest.handle=src.handle;dest.scale=src.scale;
}
inline bool EqualDefined(const Binding& a,const Binding& b) {
    return a.type==b.type&&a.ref1==b.ref1&&a.ref2==b.ref2&&a.handle==b.handle&&a.scale==b.scale;
}
inline void Construct(Result& result,std::uintptr_t manager,std::uint32_t used) {
    result.mask=used;
    const auto* slots=reinterpret_cast<const Binding*>(manager+0x30);
    for(unsigned i=0;i<8;++i) {
        // Preserve native order and multiplicity, including aliased refs.
        CopyDefined(result.slots[i],slots[i]);
        if(slots[i].ref1) ++*reinterpret_cast<std::uint64_t*>(slots[i].ref1);
        if(slots[i].ref2) ++*reinterpret_cast<std::uint64_t*>(slots[i].ref2);
    }
}
}
