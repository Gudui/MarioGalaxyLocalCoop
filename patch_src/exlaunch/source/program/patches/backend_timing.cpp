#include "backend_timing.hpp"
#if defined(GALAXY_BACKEND_TIMING_DIAG) && GALAXY_BACKEND_TIMING_DIAG
#include "patch_shared.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <atomic>
#include <algorithm>
#if defined(GALAXY_PUSH_CONSTANT_EXPERIMENT) && GALAXY_PUSH_CONSTANT_EXPERIMENT
#include "push_constant_cache.hpp"
#endif
namespace mod::patches {
extern "C" { BackendTimingBlock gGalaxyBackendTiming{}; }
#if defined(GALAXY_DESCRIPTOR_TIMING_DIAG) && GALAXY_DESCRIPTOR_TIMING_DIAG
extern "C" { DescriptorProbeBlock gGalaxyDescriptorProbe{}; }
// EVD0365 exact native imports; preserve PLT0 x16 even before lazy resolution.
extern "C" void GalaxyFiveArgNativeImport(std::uintptr_t,std::uint32_t,const void*,
    std::uint32_t,const void*,std::uintptr_t);
extern "C" void GalaxyEightArgNativeImport(std::uintptr_t,std::uint32_t,std::uintptr_t,
    std::uint32_t,std::uint32_t,const void*,std::uint32_t,const void*,std::uintptr_t);
asm(".pushsection .text\n"
    ".balign 4\n.global GalaxyFiveArgNativeImport\n.type GalaxyFiveArgNativeImport, %function\n"
    "GalaxyFiveArgNativeImport:\nmov x16,x5\nldr x17,[x16]\nbr x17\n"
    ".size GalaxyFiveArgNativeImport, .-GalaxyFiveArgNativeImport\n"
    ".balign 4\n.global GalaxyEightArgNativeImport\n.type GalaxyEightArgNativeImport, %function\n"
    "GalaxyEightArgNativeImport:\nldr x16,[sp]\nldr x17,[x16]\nbr x17\n"
    ".size GalaxyEightArgNativeImport, .-GalaxyEightArgNativeImport\n.popsection\n");
#endif
#if defined(GALAXY_PUSH_CONSTANT_EXPERIMENT) && GALAXY_PUSH_CONSTANT_EXPERIMENT
extern "C" { PushProbeBlock gGalaxyPushProbe{}; }
// EVD0355: a direct C++ call through an unresolved GOT slot omits the
// import-slot x16 consumed by main's PLT0 (5F8960 saves x16/LR). Preserve
// the exact lazy-binding ABI before tail-calling the untouched slot. The
// extra helper argument supplies its address; native Vulkan ignores it.
extern "C" void GalaxyPushNativeImport(std::uintptr_t,std::uintptr_t,
    std::uint32_t,std::uint32_t,std::uint32_t,const void*,std::uintptr_t);
extern "C" std::int32_t GalaxyTwoArgNativeImport(std::uintptr_t,std::uintptr_t,std::uintptr_t);
asm(".pushsection .text\n"
    ".balign 4\n.global GalaxyPushNativeImport\n.type GalaxyPushNativeImport, %function\n"
    "GalaxyPushNativeImport:\nmov x16,x6\nldr x17,[x16]\nbr x17\n"
    ".size GalaxyPushNativeImport, .-GalaxyPushNativeImport\n"
    ".balign 4\n.global GalaxyTwoArgNativeImport\n.type GalaxyTwoArgNativeImport, %function\n"
    "GalaxyTwoArgNativeImport:\nmov x16,x2\nldr x17,[x16]\nbr x17\n"
    ".size GalaxyTwoArgNativeImport, .-GalaxyTwoArgNativeImport\n.popsection\n");
#endif
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// EVD0343..0345: true entries, relocatable stack/register openings.
// 243A0 stack80+A00 balances at26218..2623C despite body decompile warning.
constexpr std::uintptr_t kDrawIndexed=0x639C0, kDrawOther=0x243A0;
constexpr std::uintptr_t kLookup=0x264A0, kRotate=0x113B68, kBackendVtable=0x889C68;
std::atomic<std::uintptr_t> sOwnerThread{0};
BackendTimingRow sRow{};
bool sActive=false;
unsigned sDrawDepth=0;
std::uint64_t Clock() {
    std::uint64_t v;
    asm volatile("mrs %0, cntpct_el0" : "=r"(v) :: "memory");
    return v;
}
std::uintptr_t Thread() {
    std::uintptr_t v;
    asm volatile("mrs %0, tpidrro_el0" : "=r"(v));
    return v;
}
template<class T> T Read(std::uintptr_t p, std::uintptr_t off) {
    return *reinterpret_cast<const T*>(p+off);
}
bool Owned() {
    const auto owner=sOwnerThread.load(std::memory_order_relaxed);
    return owner && Thread()==owner;
}
#if defined(GALAXY_DESCRIPTOR_TIMING_DIAG) && GALAXY_DESCRIPTOR_TIMING_DIAG
// SMG1 main SHA above, EVD0365. Exact aligned16B thunk replacements:
// ADRP/LDR/ADD/BR openings are overwritten, never relocated; no BL hook.
constexpr std::uintptr_t kDescriptorUpdateThunk=0x5F9F30, kDescriptorUpdateGot=0x919AD8;
constexpr std::uintptr_t kDescriptorBindThunk=0x5F9F40, kDescriptorBindGot=0x919AE0;
DescriptorProbeRow sDescriptorRow{};
struct DescriptorScope {
    unsigned metric;
    bool active,timed;
    std::uint64_t start;
    explicit DescriptorScope(unsigned m):metric(m),active(Owned()&&sActive&&sDrawDepth!=0),
        timed(active&&gGalaxyBackendTiming.mode==1),start(timed?Clock():0) {}
    ~DescriptorScope() {
        if (!active) return;
        const auto elapsed=timed?Clock()-start:0;
        auto& value=sDescriptorRow.metric[metric];
        value.ticks+=elapsed; value.maximum=std::max(value.maximum,elapsed); ++value.calls;
    }
};
HOOK_DEFINE_REPLACE(DescriptorUpdateTiming) {
    static void Callback(std::uintptr_t device,std::uint32_t writes,const void* writeInfo,
                         std::uint32_t copies,const void* copyInfo) {
        DescriptorScope scope(0);
        GalaxyFiveArgNativeImport(device,writes,writeInfo,copies,copyInfo,
            exl::util::modules::GetTargetStart()+kDescriptorUpdateGot);
    }
};
HOOK_DEFINE_REPLACE(DescriptorBindTiming) {
    static void Callback(std::uintptr_t command,std::uint32_t point,std::uintptr_t layout,
                         std::uint32_t first,std::uint32_t sets,const void* setInfo,
                         std::uint32_t dynamics,const void* dynamicInfo) {
        DescriptorScope scope(1);
        // The ninth helper argument lives at entry SP. Preserve x0..x7 and
        // tail-call the original slot; native ABI ignores the extra stack word.
        GalaxyEightArgNativeImport(command,point,layout,first,sets,setInfo,dynamics,dynamicInfo,
            exl::util::modules::GetTargetStart()+kDescriptorBindGot);
    }
};
#endif
#if defined(GALAXY_PUSH_CONSTANT_EXPERIMENT) && GALAXY_PUSH_CONSTANT_EXPERIMENT
// SMG1 main only, EVD0352..0354. Exact import-thunk replacements; ADRP
// openings are never relocated and no PC-relative BL is hooked. Original
// resolved GOT pointers remain untouched. ABI: Vulkan1.0 core functions.
constexpr std::uintptr_t kPushThunk=0x5F9F50, kPushGot=0x919AE8;
constexpr std::uintptr_t kBeginThunk=0x5FA280, kBeginGot=0x919C80;
constexpr std::uintptr_t kResetThunk=0x5FA270, kResetGot=0x919C78;
PushConstantCache sPushCache{};
PushProbeRow sPushRow{};
std::atomic<std::uint64_t> sPushEpoch{1};
std::uint64_t sSeenEpoch=0, sSeenMode=0;
std::uintptr_t ImportSlot(std::uintptr_t offset) {
    return exl::util::modules::GetTargetStart()+offset;
}
HOOK_DEFINE_REPLACE(PushConstantExperiment) {
    static void Callback(std::uintptr_t command,std::uintptr_t layout,std::uint32_t stages,
                         std::uint32_t offset,std::uint32_t size,const void* data) {
        const auto native=[&]() {
            GalaxyPushNativeImport(command,layout,stages,offset,size,data,ImportSlot(kPushGot));
        };
        if (!Owned()) {
            sPushEpoch.fetch_add(1,std::memory_order_relaxed);
            return native();
        }
        const bool record=sActive&&sDrawDepth!=0;
        const auto mode=gGalaxyPushProbe.mode;
        if (record) ++sPushRow.calls;
        if (mode!=1 && mode!=2) {
            sSeenMode=0; sSeenEpoch=0;
            return native();
        }
        const auto epoch=sPushEpoch.load(std::memory_order_relaxed);
        if (epoch!=sSeenEpoch || mode!=sSeenMode) {
            sPushCache.Reset(); sSeenEpoch=epoch; sSeenMode=mode;
            if (record) ++sPushRow.invalidations;
        }
        const auto beforeCache=record?Clock():0;
        // Payload is valid under the original Vulkan ABI. Unsupported shapes
        // pass through; no retained pointer is read and bytes are copied now.
        const bool duplicate=sPushCache.Duplicate(command,layout,stages,offset,size,data);
        if (record) {
            sPushRow.cacheTicks+=Clock()-beforeCache;
            sPushRow.duplicates+=duplicate;
        }
        if (mode==2 && duplicate) {
            if (record) ++sPushRow.skipped;
            return;
        }
        const auto beforeNative=record?Clock():0;
        native();
        if (record) {
            const auto elapsed=Clock()-beforeNative;
            sPushRow.nativeTicks+=elapsed;
            if (duplicate) sPushRow.duplicateTicks+=elapsed;
        }
    }
};
HOOK_DEFINE_REPLACE(PushBeginExperiment) {
    static std::int32_t Callback(std::uintptr_t command,const void* info) {
        sPushEpoch.fetch_add(1,std::memory_order_relaxed);
        return GalaxyTwoArgNativeImport(command,reinterpret_cast<std::uintptr_t>(info),ImportSlot(kBeginGot));
    }
};
HOOK_DEFINE_REPLACE(PushResetExperiment) {
    static std::int32_t Callback(std::uintptr_t command,std::uint32_t flags) {
        sPushEpoch.fetch_add(1,std::memory_order_relaxed);
        return GalaxyTwoArgNativeImport(command,flags,ImportSlot(kResetGot));
    }
};
#endif
bool Backend(std::uintptr_t p) {
    // Fresh native argument; never dereference a retained heap pointer.
    return IsGuestPointer(p) && Read<std::uintptr_t>(p,0)==
        exl::util::modules::GetTargetStart()+kBackendVtable;
}
struct Scope {
    unsigned metric;
    bool active,timed;
    std::uint64_t start;
    Scope(unsigned m,bool eligible=true):metric(m),active(eligible&&Owned()&&sActive),
        timed(active&&gGalaxyBackendTiming.mode==1),start(timed?Clock():0) {}
    ~Scope() {
        if (!active) return;
        const auto elapsed=timed?Clock()-start:0;
        auto& a=sRow.metric[metric];
        a.ticks+=elapsed; a.maximum=std::max(a.maximum,elapsed); ++a.calls;
    }
};
void Boundary(std::uintptr_t backend) {
    auto& b=gGalaxyBackendTiming;
    const auto frame=Read<std::uint64_t>(backend,0x60C8),now=Clock();
    const auto bank=Read<std::uint32_t>(backend,0x60C0);
    if (sActive) {
        sActive=false; sRow.lastFrame=frame; sRow.end=now;
        bool valid=b.backend==backend&&bank<2&&!sDrawDepth&&frame==sRow.firstFrame+1&&
            now>=sRow.start&&b.frequency&&now-sRow.start<=b.frequency/5;
        if (valid) {
#if defined(GALAXY_DESCRIPTOR_TIMING_DIAG) && GALAXY_DESCRIPTOR_TIMING_DIAG
            sDescriptorRow.frame=frame;
            gGalaxyDescriptorProbe.rows[b.count]=sDescriptorRow;
            gGalaxyDescriptorProbe.count=b.count+1;
#endif
            const auto stats=backend+0x6020+(bank^1)*0x40;
            sRow.draws=Read<std::uint32_t>(stats,0x30);
            sRow.bypasses=Read<std::uint32_t>(stats,0x34);
            sRow.same=Read<std::uint32_t>(stats,0x38);
            valid=sRow.bypasses<=sRow.same&&sRow.same<=sRow.draws;
        }
        if (valid) {
#if defined(GALAXY_PUSH_CONSTANT_EXPERIMENT) && GALAXY_PUSH_CONSTANT_EXPERIMENT
            sPushRow.frame=frame;
            gGalaxyPushProbe.rows[b.count]=sPushRow;
            gGalaxyPushProbe.count=b.count+1;
#endif
            b.rows[b.count++]=sRow;
        } else ++b.rejected;
        if (b.count==240||b.rejected==240) b.state=2;
    }
    ++b.sequence;
    if (b.request==1||b.request==2) {
        b.mode=b.request; b.request=0; b.state=1; b.count=b.rejected=0;
#if defined(GALAXY_DESCRIPTOR_TIMING_DIAG) && GALAXY_DESCRIPTOR_TIMING_DIAG
        gGalaxyDescriptorProbe.count=0;
#endif
#if defined(GALAXY_PUSH_CONSTANT_EXPERIMENT) && GALAXY_PUSH_CONSTANT_EXPERIMENT
        gGalaxyPushProbe.count=0;
#endif
        asm volatile("mrs %0, cntfrq_el0" : "=r"(b.frequency));
    }
#if defined(GALAXY_PUSH_CONSTANT_EXPERIMENT) && GALAXY_PUSH_CONSTANT_EXPERIMENT
    sPushCache.Reset(); // Conservative cross-frame/opaque handle reuse barrier.
    sSeenEpoch=0;
#endif
    if (b.state!=1||bank>1||b.count>=240) return;
    b.backend=backend; b.thread=Thread(); sRow={};
#if defined(GALAXY_DESCRIPTOR_TIMING_DIAG) && GALAXY_DESCRIPTOR_TIMING_DIAG
    sDescriptorRow={};
#endif
#if defined(GALAXY_PUSH_CONSTANT_EXPERIMENT) && GALAXY_PUSH_CONSTANT_EXPERIMENT
    sPushRow={};
#endif
    sRow.firstFrame=frame; sRow.start=now; sRow.flags=b.mode; sActive=true;
    if (b.mode==1) { Scope calibration(3); }
}
HOOK_DEFINE_TRAMPOLINE(BackendIndexedTiming) {
    static void Callback(std::uintptr_t backend,const void* packet,const void* indices) {
        const bool owner=Owned(); if(owner) ++sDrawDepth;
        { Scope scope(0); Orig(backend,packet,indices); }
        if(owner) --sDrawDepth;
    }
};
HOOK_DEFINE_TRAMPOLINE(BackendOtherTiming) {
    static void Callback(std::uintptr_t backend,const void* packet) {
        const bool owner=Owned(); if(owner) ++sDrawDepth;
        { Scope scope(1,packet!=nullptr); Orig(backend,packet); }
        if(owner) --sDrawDepth;
    }
};
HOOK_DEFINE_TRAMPOLINE(BackendLookupTiming) {
    static std::uint64_t Callback(void* cache,const void* key,std::uint32_t mode) {
        Scope scope(2,Owned()&&sDrawDepth!=0); return Orig(cache,key,mode);
    }
};
HOOK_DEFINE_TRAMPOLINE(BackendRotateTiming) {
    static bool Callback(std::uintptr_t backend) {
        const bool result=Orig(backend);
        if(result&&Backend(backend)) {
            std::uintptr_t empty=0;
            sOwnerThread.compare_exchange_strong(empty,Thread(),std::memory_order_relaxed);
            if(Owned()) Boundary(backend);
        }
        return result;
    }
};
}
void InstallBackendTiming() {
    BackendIndexedTiming::InstallAtOffset(kDrawIndexed);
    BackendOtherTiming::InstallAtOffset(kDrawOther);
    BackendLookupTiming::InstallAtOffset(kLookup);
    BackendRotateTiming::InstallAtOffset(kRotate);
#if defined(GALAXY_DESCRIPTOR_TIMING_DIAG) && GALAXY_DESCRIPTOR_TIMING_DIAG
    DescriptorUpdateTiming::InstallAtOffset(kDescriptorUpdateThunk);
    DescriptorBindTiming::InstallAtOffset(kDescriptorBindThunk);
    Logging.Log("[GalaxyCoop:smg1] ATT-0163 descriptor durations installed default-idle window=240 kill=1");
#endif
#if defined(GALAXY_PUSH_CONSTANT_EXPERIMENT) && GALAXY_PUSH_CONSTANT_EXPERIMENT
    PushConstantExperiment::InstallAtOffset(kPushThunk);
    PushBeginExperiment::InstallAtOffset(kBeginThunk);
    PushResetExperiment::InstallAtOffset(kResetThunk);
    Logging.Log("[GalaxyCoop:smg1] ATT-0159 push-constant experiment installed default-off mode=0 kill=1");
#endif
    Logging.Log("[GalaxyCoop:smg1] DIAG-0196 backend durations installed default-idle window=240 kill=1");
}
}
#endif
