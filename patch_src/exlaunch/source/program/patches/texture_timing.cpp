#include "texture_timing.hpp"
#if defined(GALAXY_TEXTURE_TIMING_DIAG) && GALAXY_TEXTURE_TIMING_DIAG
#if !defined(GALAXY_PUSH_CONSTANT_FILTER) || !GALAXY_PUSH_CONSTANT_FILTER
#error Texture diagnostic requires qualified production frame owner
#endif
#if (defined(GALAXY_RECTANGLE_TIMING_DIAG) && GALAXY_RECTANGLE_TIMING_DIAG) || \
    (defined(GALAXY_RETIREMENT_TIMING_DIAG) && GALAXY_RETIREMENT_TIMING_DIAG)
#error Fulfilled rectangle and retirement diagnostics must be omitted
#endif
#include "push_constant_filter.hpp"
#include "patch_shared.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <atomic>
#include <algorithm>
#include <type_traits>
#if defined(GALAXY_TEXTURE_CLEAN_FAST) && GALAXY_TEXTURE_CLEAN_FAST
#include "texture_clean.hpp"
#endif
namespace mod::patches {
extern "C" { TextureTimingBlock gGalaxyTextureTiming{}; }
#if defined(GALAXY_TEXTURE_CLEAN_FAST) && GALAXY_TEXTURE_CLEAN_FAST
extern "C" { TextureFastControl gGalaxyTextureFast{}; }
#endif
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// EVD0390 Ghidra:26CA0 true entry, relocatable stack/register stores.
// Native x0=manager,x1=state holder,x2=draw state,x8=indirect328B result.
// EVD0343/345:639C0/243A0 true entries, balanced frames/relocatable openings.
constexpr std::uintptr_t kTexture=0x26CA0,kIndexed=0x639C0,kOther=0x243A0,kBackendVT=0x889C68;
// Trivial aggregate return deliberately uses AAPCS64 x8. No C++ ownership or
// destructor is imposed on native reference-counted pointers in these bytes.
#if defined(GALAXY_TEXTURE_CLEAN_FAST) && GALAXY_TEXTURE_CLEAN_FAST
using TextureBindings=texture_clean::Result;
struct RefCheck { std::uintptr_t ptr;std::uint64_t before,occurrences; };
#else
struct TextureBindings { std::uint64_t representation[41]; };
#endif
static_assert(sizeof(TextureBindings)==328 && std::is_trivially_copyable_v<TextureBindings>);
TextureTimingRow sRow{};
bool sActive=false;
unsigned sDrawDepth=0,sTextureDepth=0;
std::atomic<std::uint64_t> sForeign{0};
std::uint64_t sFirstForeign=0;
std::uint64_t Clock() { std::uint64_t v;asm volatile("mrs %0, cntpct_el0":"=r"(v)::"memory");return v; }
std::uintptr_t Thread() { std::uintptr_t v;asm volatile("mrs %0, tpidrro_el0":"=r"(v));return v; }
template<class T> T Read(std::uintptr_t p,std::uintptr_t off) { return *reinterpret_cast<const T*>(p+off); }
bool Owned() { return gGalaxyPushFilter.thread && Thread()==gGalaxyPushFilter.thread; }
bool Backend(std::uintptr_t p) { return IsGuestPointer(p)&&Read<std::uintptr_t>(p,0)==exl::util::modules::GetTargetStart()+kBackendVT; }
void Add(TextureTimingMetric& m,std::uint64_t ticks) { m.ticks+=ticks;m.maximum=std::max(m.maximum,ticks);++m.calls; }
struct Scope {
    unsigned kind;
    bool active,timed,tracked;
    std::uint64_t start;
    Scope(unsigned k,bool valid):kind(k),active(Owned()&&sActive),timed(active&&gGalaxyTextureTiming.mode==1),tracked(active),start(0) {
#if defined(GALAXY_TEXTURE_CLEAN_FAST) && GALAXY_TEXTURE_CLEAN_FAST
        tracked=Owned()&&(sActive||gGalaxyTextureFast.mode==2);
#endif
        if(!Owned()) sForeign.fetch_add(1,std::memory_order_relaxed);
        if(!tracked) return;
        if(!valid) { sRow.flags|=0x400;active=timed=tracked=false;return; }
        if(kind<2) { if(sDrawDepth++) sRow.flags|=0x200; }
        else if(kind==2) { if(sDrawDepth!=1 || sTextureDepth) sRow.flags|=0x200; ++sTextureDepth; }
        start=timed?Clock():0;
    }
    ~Scope() {
        if(!tracked) return;
        if(active) Add(sRow.metric[kind],timed?Clock()-start:0);
        if(kind<2) --sDrawDepth;
        else if(kind==2) --sTextureDepth;
    }
};
HOOK_DEFINE_TRAMPOLINE(TextureProducerTiming) {
#if defined(GALAXY_TEXTURE_CLEAN_FAST) && GALAXY_TEXTURE_CLEAN_FAST
    // Separate helpers keep shadow-only stack arrays out of native/fast calls.
    // Single named result permits NRVO; Callback returns each helper prvalue
    // directly, preserving caller x8 without a328B output copy.
    __attribute__((noinline)) static TextureBindings Fast(std::uintptr_t manager,std::uint32_t used) {
        TextureBindings result;
        texture_clean::Construct(result,manager,used);
        return result;
    }
    __attribute__((noinline)) static TextureBindings Shadow(std::uintptr_t manager,std::uintptr_t holder,const void* draw,std::uint32_t used) {
        // Bounded native shadow: snapshot all defined fields and distinct ref
        // counts, then check native output and per-occurrence increments.
        TextureBindings expected;expected.mask=used;
        RefCheck refs[16]{};unsigned count=0;
        auto& control=gGalaxyTextureFast;
        const auto* slots=reinterpret_cast<const texture_clean::Binding*>(manager+0x30);
        for(unsigned i=0;i<8;++i) {
            texture_clean::CopyDefined(expected.slots[i],slots[i]);
            for(auto ref:{slots[i].ref1,slots[i].ref2}) if(ref) {
                unsigned index=0;while(index<count&&refs[index].ptr!=ref) ++index;
                if(index==count) { refs[count++]={ref,Read<std::uint64_t>(ref,0),0}; }
                ++refs[index].occurrences;
            }
        }
        auto result=Orig(manager,holder,draw);
        bool equal=result.mask==expected.mask;
        for(unsigned i=0;i<8;++i) equal=equal&&
            texture_clean::EqualDefined(result.slots[i],expected.slots[i])&&
            texture_clean::EqualDefined(slots[i],expected.slots[i]);
        for(unsigned i=0;i<count;++i) {
            equal=equal&&refs[i].before>0&&refs[i].before<(1ULL<<48)&&
                Read<std::uint64_t>(refs[i].ptr,0)==refs[i].before+refs[i].occurrences;
            control.refOccurrences+=refs[i].occurrences;
        }
        if(equal) { ++sRow.shadow;++control.shadowPassed; }
        else { ++sRow.mismatch;++control.failures;control.mode=0; }
        return result;
    }
#endif
    static TextureBindings Callback(std::uintptr_t manager,std::uintptr_t holder,const void* draw) {
        const auto backend=gGalaxyPushFilter.backend;
        const bool valid=manager==backend+0x188508 && holder==backend+0xDB8 &&
            reinterpret_cast<std::uintptr_t>(draw)==backend+0x7E8;
        Scope scope(2,valid);
#if defined(GALAXY_TEXTURE_CLEAN_FAST) && GALAXY_TEXTURE_CLEAN_FAST
        auto& control=gGalaxyTextureFast;
        const auto mode=control.mode;
        std::uint32_t used=0;
        if(mode && !control.failures && valid && Owned() && Backend(backend) &&
           sDrawDepth==1 && sTextureDepth==1 &&
           texture_clean::Eligible(manager,holder,reinterpret_cast<std::uintptr_t>(draw),used,IsGuestPointer)) {
            if(sActive) ++sRow.clean;
            if(mode==2) {
                if(sActive) ++sRow.fast;
                return Fast(manager,used);
            }
            return Shadow(manager,holder,draw,used);
        }
#endif
        // Guaranteed C++17 return elision: original writes the caller's x8 result.
        // Owned ELF review must confirm this and all forwarded x0..x2 paths.
        return Orig(manager,holder,draw);
    }
};
HOOK_DEFINE_TRAMPOLINE(TextureIndexedTiming) {
    static void Callback(std::uintptr_t backend,const void* packet,const void* indices) {
        Scope scope(0,packet&&backend==gGalaxyPushFilter.backend);
        Orig(backend,packet,indices);
    }
};
HOOK_DEFINE_TRAMPOLINE(TextureOtherTiming) {
    static void Callback(std::uintptr_t backend,const void* packet) {
        if(!packet) return Orig(backend,packet);
        Scope scope(1,backend==gGalaxyPushFilter.backend);
        Orig(backend,packet);
    }
};
}
void TextureBoundary(std::uintptr_t backend) {
    if(!Owned()||!Backend(backend)) return;
    auto& b=gGalaxyTextureTiming;++b.sequence;
    if(!sActive&&b.state!=1&&b.request!=1&&b.request!=2
#if defined(GALAXY_TEXTURE_CLEAN_FAST) && GALAXY_TEXTURE_CLEAN_FAST
       && !gGalaxyTextureFast.request
#endif
       ) return;
    const auto now=Clock(),frame=Read<std::uint64_t>(backend,0x60C8);
    const auto bank=Read<std::uint32_t>(backend,0x60C0);
    if(sActive) {
        sActive=false;sRow.lastFrame=frame;sRow.end=now;
        if(sForeign.load(std::memory_order_relaxed)!=sFirstForeign) sRow.flags|=0x100;
        bool valid=b.backend==backend&&b.thread==Thread()&&bank<2&&!sDrawDepth&&!sTextureDepth&&
            frame==sRow.firstFrame+1&&now>sRow.start&&b.frequency&&now-sRow.start<=b.frequency/5&&sRow.flags==b.mode;
#if defined(GALAXY_TEXTURE_CLEAN_FAST) && GALAXY_TEXTURE_CLEAN_FAST
        valid=valid&&!gGalaxyTextureFast.failures && sRow.route==gGalaxyTextureFast.mode;
#endif
        if(valid) {
            const auto stats=backend+0x6020+(bank^1)*0x40;
            sRow.draws=Read<std::uint32_t>(stats,0x30);sRow.bypasses=Read<std::uint32_t>(stats,0x34);sRow.same=Read<std::uint32_t>(stats,0x38);
            valid=sRow.bypasses<=sRow.same&&sRow.same<=sRow.draws&&
                sRow.metric[0].calls+sRow.metric[1].calls==sRow.draws&&sRow.metric[2].calls==sRow.draws;
        }
        if(valid) b.rows[b.count++]=sRow;else ++b.rejected;
        if(b.count==240||b.rejected==240) b.state=2;
    }
    if(b.request==1||b.request==2) {
        b.mode=b.request;b.request=0;b.state=1;b.count=b.rejected=0;
        asm volatile("mrs %0, cntfrq_el0":"=r"(b.frequency));
    }
#if defined(GALAXY_TEXTURE_CLEAN_FAST) && GALAXY_TEXTURE_CLEAN_FAST
    auto& fast=gGalaxyTextureFast;
    if(fast.request) {
        const auto requested=fast.request;fast.request=0;
        if(requested==1) fast.mode=0;
        else if(requested==2&&!fast.failures) fast.mode=1;
        else if(requested==3&&!fast.failures&&fast.shadowPassed>=1024) fast.mode=2;
    }
    // Shadow exists only within a bounded armed window; fast/native modes persist
    // until an owned request changes them. A failed shadow permanently returns native.
    if(b.state!=1&&fast.mode==1) fast.mode=0;
#endif
    if(b.state!=1||bank>1||b.count>=240) return;
    b.backend=backend;b.thread=Thread();sRow={};sFirstForeign=sForeign.load(std::memory_order_relaxed);
    sRow.firstFrame=frame;sRow.start=now;sRow.flags=b.mode;sActive=true;
#if defined(GALAXY_TEXTURE_CLEAN_FAST) && GALAXY_TEXTURE_CLEAN_FAST
    sRow.route=gGalaxyTextureFast.mode;
#endif
    if(b.mode==1) { const auto before=Clock();Add(sRow.metric[3],Clock()-before); }
}
void InstallTextureTiming() {
    TextureIndexedTiming::InstallAtOffset(kIndexed);
    TextureOtherTiming::InstallAtOffset(kOther);
    TextureProducerTiming::InstallAtOffset(kTexture);
#if defined(GALAXY_TEXTURE_CLEAN_FAST) && GALAXY_TEXTURE_CLEAN_FAST
    Logging.Log("[GalaxyCoop:smg1] DIAG-0200 ATT-0176 clean texture installed native=0 shadow=1 fast=2 default-native window=240 kill=1");
#else
    Logging.Log("[GalaxyCoop:smg1] DIAG-0199 ATT-0175 texture producer costs installed default-idle window=240 forward-all kill=1");
#endif
}
}
#endif
