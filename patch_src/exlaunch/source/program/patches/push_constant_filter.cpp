#include "push_constant_filter.hpp"
#if defined(GALAXY_PUSH_CONSTANT_FILTER) && GALAXY_PUSH_CONSTANT_FILTER
#if (defined(GALAXY_BACKEND_TIMING_DIAG) && GALAXY_BACKEND_TIMING_DIAG) || \
    (defined(GALAXY_PUSH_CONSTANT_EXPERIMENT) && GALAXY_PUSH_CONSTANT_EXPERIMENT) || \
    (defined(GALAXY_DESCRIPTOR_TIMING_DIAG) && GALAXY_DESCRIPTOR_TIMING_DIAG)
#error Production push filter must not overlap diagnostic import/frame hooks
#endif
#include "patch_shared.hpp"
#include "push_constant_cache.hpp"
#include "retirement_timing.hpp"
#include "rectangle_timing.hpp"
#include "texture_timing.hpp"
#include "draw_command_timing.hpp"
#include "binding_timing.hpp"
#include "vertex_batch.hpp"
#include "dynamic_uniform.hpp"
#include "draw_association.hpp"
#include "descriptor_value.hpp"
#include "uniform_upload_timing.hpp"
#include "decoder_unpack_timing.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <atomic>

namespace mod::patches {
extern "C" { PushFilterControl gGalaxyPushFilter{}; }
// EVD0352/355/356: untouched GOT and x16 preserve PLT0 lazy binding.
extern "C" void GalaxyFilterPushNativeImport(std::uintptr_t,std::uintptr_t,
    std::uint32_t,std::uint32_t,std::uint32_t,const void*,std::uintptr_t);
extern "C" std::int32_t GalaxyFilterTwoArgNativeImport(std::uintptr_t,std::uintptr_t,std::uintptr_t);
asm(".pushsection .text\n"
    ".balign 4\n.global GalaxyFilterPushNativeImport\n.type GalaxyFilterPushNativeImport, %function\n"
    "GalaxyFilterPushNativeImport:\nmov x16,x6\nldr x17,[x16]\nbr x17\n"
    ".size GalaxyFilterPushNativeImport, .-GalaxyFilterPushNativeImport\n"
    ".balign 4\n.global GalaxyFilterTwoArgNativeImport\n.type GalaxyFilterTwoArgNativeImport, %function\n"
    "GalaxyFilterTwoArgNativeImport:\nmov x16,x2\nldr x17,[x16]\nbr x17\n"
    ".size GalaxyFilterTwoArgNativeImport, .-GalaxyFilterTwoArgNativeImport\n.popsection\n");
namespace {
// SMG1 main SHA76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// EVD0352: aligned16B imports overwritten completely, no ADRP relocation/BL hook.
// EVD0343/345:113B68 true entry/relocatable opening and live backend VT/slot.
constexpr std::uintptr_t kPush=0x5F9F50, kPushGot=0x919AE8;
constexpr std::uintptr_t kBegin=0x5FA280, kBeginGot=0x919C80;
constexpr std::uintptr_t kReset=0x5FA270, kResetGot=0x919C78;
constexpr std::uintptr_t kRotate=0x113B68, kBackendVtable=0x889C68;
PushConstantCache sCache{};
std::atomic<std::uintptr_t> sOwner{0};
std::atomic<std::uint64_t> sEpoch{1};
std::uint64_t sSeenEpoch=0, sSeenMode=0;
std::uintptr_t Thread() {
    std::uintptr_t value;
    asm volatile("mrs %0, tpidrro_el0" : "=r"(value));
    return value;
}
std::uintptr_t Slot(std::uintptr_t offset) { return exl::util::modules::GetTargetStart()+offset; }
HOOK_DEFINE_REPLACE(ProductionPushFilter) {
    static void Callback(std::uintptr_t command,std::uintptr_t layout,std::uint32_t stages,
                         std::uint32_t offset,std::uint32_t size,const void* data) {
        const auto native=[&]() {
            GalaxyFilterPushNativeImport(command,layout,stages,offset,size,data,Slot(kPushGot));
        };
        const auto owner=sOwner.load(std::memory_order_relaxed);
        if (!owner || Thread()!=owner) {
            sEpoch.fetch_add(1,std::memory_order_relaxed);
            return native();
        }
        const auto mode=gGalaxyPushFilter.mode;
        bool inlineEqual=false;
#if defined(GALAXY_PUSH_CACHE_INLINE_EQUAL) && GALAXY_PUSH_CACHE_INLINE_EQUAL
        inlineEqual=mode==3;
#endif
        if (mode!=2 && !inlineEqual) { sSeenMode=0; sSeenEpoch=0; return native(); }
        const auto epoch=sEpoch.load(std::memory_order_relaxed);
        if (epoch!=sSeenEpoch || mode!=sSeenMode) {
            sCache.Reset(); sSeenEpoch=epoch; sSeenMode=mode;
        }
        // ATT0172 changes equality only in mode3. Keys, lifetime barriers and
        // overlap fallback retain the ATT0161 byte/stage oracle. No native flags,
        // saved native pointer dereferences, timestamps or per-call logs.
        if (!sCache.Duplicate(command,layout,stages,offset,size,data,inlineEqual)) native();
    }
};
HOOK_DEFINE_REPLACE(ProductionPushBegin) {
    static std::int32_t Callback(std::uintptr_t command,const void* info) {
        sEpoch.fetch_add(1,std::memory_order_relaxed);
        DescriptorValueInvalidate();
        return GalaxyFilterTwoArgNativeImport(command,reinterpret_cast<std::uintptr_t>(info),Slot(kBeginGot));
    }
};
HOOK_DEFINE_REPLACE(ProductionPushReset) {
    static std::int32_t Callback(std::uintptr_t command,std::uint32_t flags) {
        sEpoch.fetch_add(1,std::memory_order_relaxed);
        DescriptorValueInvalidate();
        return GalaxyFilterTwoArgNativeImport(command,flags,Slot(kResetGot));
    }
};
HOOK_DEFINE_TRAMPOLINE(ProductionPushFrame) {
    static bool Callback(std::uintptr_t backend) {
        const bool result=Orig(backend);
        if (!result || !IsGuestPointer(backend) ||
            *reinterpret_cast<const std::uintptr_t*>(backend)!=Slot(kBackendVtable)) return result;
        std::uintptr_t empty=0;
        const auto thread=Thread();
        sOwner.compare_exchange_strong(empty,thread,std::memory_order_relaxed);
        if (sOwner.load(std::memory_order_relaxed)!=thread) return result;
        sCache.Reset(); sSeenEpoch=0; // Conservative cross-frame/handle-reuse barrier.
        auto& control=gGalaxyPushFilter;
        control.backend=backend; control.thread=thread;
        ++control.sequence; ++control.invalidations;
        RetirementBoundary(backend);
        RectangleBoundary(backend);
        TextureBoundary(backend);
        DrawCommandBoundary(backend);
        BindingBoundary(backend);
        VertexBatchBoundary(backend);
        DynamicUniformBoundary(backend);
        DrawAssociationBoundary(backend);
        DescriptorValueBoundary(backend);
        DecoderUnpackBoundary(backend);
        return result;
    }
};
}
void InstallPushConstantFilter() {
    ProductionPushFrame::InstallAtOffset(kRotate);
    ProductionPushFilter::InstallAtOffset(kPush);
    ProductionPushBegin::InstallAtOffset(kBegin);
    ProductionPushReset::InstallAtOffset(kReset);
    InstallRetirementTiming();
    InstallRectangleTiming();
    InstallTextureTiming();
    InstallDrawCommandTiming();
    InstallBindingTiming();
    InstallVertexBatch();
    InstallDynamicUniform();
    InstallDrawAssociation();
    InstallDescriptorValue();
    InstallUniformUploadTiming();
    InstallDecoderUnpackTiming();
#if defined(GALAXY_PUSH_CACHE_INLINE_EQUAL) && GALAXY_PUSH_CACHE_INLINE_EQUAL
    Logging.Log("[GalaxyCoop:smg1] ATT-0172 exact push filter installed mode=2 inline=3 timing=0 kill=1");
#else
    Logging.Log("[GalaxyCoop:smg1] ATT-0166 production exact push filter installed mode=2 timing=0 kill=1");
#endif
}
}
#endif
