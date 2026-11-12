#include "patch_effects.hpp"
#include "patch_shared.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <atomic>
#include <cstdint>

namespace mod::patches {
namespace {

// ATT-0214 / EVD-0520, SMG1 main SHA-256
// 76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// 0x1D3EB8 (stp/stp/mov/mov ... ret, 16 instructions) pops a 0x558-byte
// emitter from the free list at .bss 0xAA456558 (0x10F948: head +0, free
// count +8, capacity +0xC, peak +0x10; returns null when empty) and builds it
// with 0x1D3EF8 without a null check. Two players spinning emptied the pool on
// a real Switch and the build faulted at null+0x64 (crash report
// 01792961666). Its only caller, 0x1D3D88, already skips the effect when this
// returns null, so an empty pool now drops one effect instead of crashing.
constexpr bool kAtt0214EmitterGuardEnabled = true;
constexpr std::uintptr_t kEmitterCreate = 0x1D3EB8;
constexpr std::uintptr_t kEmitterPool = 0xAA456558;
constexpr std::uintptr_t kPoolPop = 0x10F948;
constexpr std::uintptr_t kEmitterConstruct = 0x1D3EF8;
constexpr std::uint32_t kEmitterSize = 0x558;
std::uint32_t sDrops = 0;

struct EmitterPool {
    void* head;
    std::int32_t free;
    std::uint32_t capacity;
    std::uint32_t peak;
};

HOOK_DEFINE_REPLACE(GuardedEmitterCreate) {
    static void* Callback(std::uintptr_t self) {
        const auto target = exl::util::modules::GetTargetStart();
        auto* pool = reinterpret_cast<EmitterPool*>(target + kEmitterPool);
        void* emitter = reinterpret_cast<void* (*)(EmitterPool*, std::uint32_t)>(target + kPoolPop)(pool, kEmitterSize);
        if (emitter == nullptr) {
            ++sDrops;
            if (sDrops <= 8 || (sDrops & (sDrops - 1)) == 0)
                Logging.Log("[GalaxyCoop:smg1] ATT-0214 emitter pool empty; effect dropped drops=%u capacity=%u peak=%u kill=1",
                            static_cast<unsigned>(sDrops), static_cast<unsigned>(pool->capacity),
                            static_cast<unsigned>(pool->peak));
            return nullptr;
        }
        reinterpret_cast<void (*)(void*, std::uintptr_t, std::uintptr_t)>(target + kEmitterConstruct)(
            emitter, self, *reinterpret_cast<const std::uintptr_t*>(self + 0x20));
        return emitter;
    }
};

// ATT-0220 / EVD-0529: after the Gateway Grand Star a map object's first wait
// step (0x788D8) starts the animation named after it: 0x7F720 -> 0x7F730,
// which looks the name up with 0x71CB0 (holder actor+0x80; returns 0 when no
// resource matches) and hands the result unchecked to 0x7F758 -> 0x7F7D4,
// whose first load faults on null (crash report 01793306001, PC 0x7F7E4).
// 0x7F758 (stp/str/mov/mov, then bl; callers 0x3282C, 0x7F744, 0x13156C,
// 0x2F9AF4, void result) now returns at once on null, so a missing animation
// is not played instead of crashing. 0x71CB0 (sub/stp/str/stp, 24 callers)
// logs each name that comes back null for 0x7F730 (return 0x7F740) or 0x3270C
// (return 0x3282C), the two callers that pass it on to 0x7F758. Through
// 0x7F730 the caller frame is 0x7F730's own (stp x29,x30,[sp,#-0x20]!;
// str x19,[sp,#0x10]; mov x29,sp): its saved x30 names the code that started
// the animation, and for the 0x788D8 wait step (return 0x78920) its saved x19
// is that step's actor (EVD-0529 stack).
constexpr bool kAtt0220MissingAnimGuardEnabled = true;
constexpr std::uintptr_t kAnimLookup = 0x71CB0;
constexpr std::uintptr_t kAnimPlay = 0x7F758;
constexpr std::uintptr_t kLookupFrom7F730 = 0x7F740, kLookupFrom3270C = 0x3282C;
constexpr std::uintptr_t kWaitStepReturn = 0x78920;
std::uint32_t sMissingSkips = 0;

// ATT-0221 / EVD-0531: of the 24 callers of 0x71CB0 only 0xB899C tests the
// result (cbz at return 0xB89A0); the other 23 use a missed lookup unchecked,
// so each miss is a possible Switch crash that Eden's unmapped reads let pass.
// Every miss from those 23 is logged the first time its (caller, name) pair
// is seen and again at each power-of-two repeat.
constexpr bool kAtt0221AllMissesLogEnabled = true;
constexpr std::uintptr_t kLookupFromChecked = 0xB89A0;
constexpr std::uint32_t kMissSlots = 64;
struct MissSlot {
    std::uintptr_t from;
    std::uint32_t hash;
    std::uint32_t count;
};
MissSlot sMissSlots[kMissSlots];
std::uint32_t sMissSlotsUsed = 0, sMissOverflow = 0;
std::atomic_flag sMissLock = ATOMIC_FLAG_INIT;

std::uint32_t NameHash(const char* name) {
    std::uint32_t hash = 2166136261u;
    for (std::uint32_t i = 0; name != nullptr && name[i] != '\0' && i < 64; ++i)
        hash = (hash ^ static_cast<std::uint8_t>(name[i])) * 16777619u;
    return hash;
}

// Count of this (caller, name) miss including this one; 0 when the table is
// busy on another thread or full (full misses go to sMissOverflow).
std::uint32_t NoteMiss(std::uintptr_t from, const char* name) {
    if (sMissLock.test_and_set(std::memory_order_acquire)) return 0;
    const std::uint32_t hash = NameHash(name);
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < sMissSlotsUsed && count == 0; ++i)
        if (sMissSlots[i].from == from && sMissSlots[i].hash == hash) count = ++sMissSlots[i].count;
    if (count == 0 && sMissSlotsUsed < kMissSlots) {
        sMissSlots[sMissSlotsUsed++] = {from, hash, 1};
        count = 1;
    } else if (count == 0) {
        ++sMissOverflow;
    }
    sMissLock.clear(std::memory_order_release);
    return count;
}

// noipa: the return address and frame must be this callback's own; GCC's
// function splitting otherwise moves the cold part into a clone called by bl.
HOOK_DEFINE_TRAMPOLINE(Att0220AnimLookup) {
    [[gnu::noipa]] static std::uintptr_t Callback(std::uintptr_t holder, const char* name) {
        // The hook stub branches without a call, so the return address is the
        // lookup's caller and this frame record links to the caller's x29.
        const auto returnAddress = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        const auto* frame = *reinterpret_cast<const std::uintptr_t* const*>(__builtin_frame_address(0));
        const std::uintptr_t found = Orig(holder, name);
        if (found != 0) return found;
        const auto target = exl::util::modules::GetTargetStart();
        const auto from = returnAddress - target;
        if (from == kLookupFromChecked) return found;
        const bool played = from == kLookupFrom7F730 || from == kLookupFrom3270C;
        if (!played && !kAtt0221AllMissesLogEnabled) return found;
        const std::uint32_t count = NoteMiss(from, name);
        if (count == 0) {
            const std::uint32_t overflow = sMissOverflow;
            if (overflow != 0 && (overflow & (overflow - 1)) == 0)
                Logging.Log("[GalaxyCoop:smg1] ATT-0221 missed lookup table full overflow=%u kill=1",
                            static_cast<unsigned>(overflow));
            return found;
        }
        if ((count & (count - 1)) != 0) return found;
        if (!played) {
            Logging.Log("[GalaxyCoop:smg1] ATT-0221 missed lookup name=%.64s holder=%p via=0x%lx count=%u kill=1",
                        name ? name : "(null)", reinterpret_cast<void*>(holder), static_cast<unsigned long>(from),
                        static_cast<unsigned>(count));
            return found;
        }
        std::uintptr_t starter = 0, actorVtable = 0;
        if (from == kLookupFrom7F730) {
            // frame is 0x7F730's: [0] x29, [1] x30, [2] saved x19.
            starter = frame[1] - target;
            const std::uintptr_t actor = frame[2];
            if (starter == kWaitStepReturn && IsGuestPointer(actor))
                actorVtable = *reinterpret_cast<const std::uintptr_t*>(actor) - target;
        }
        Logging.Log("[GalaxyCoop:smg1] ATT-0220 missing animation name=%.64s holder=%p via=0x%lx starter=0x%lx actor_vtable=0x%lx count=%u kill=1",
                    name ? name : "(null)", reinterpret_cast<void*>(holder), static_cast<unsigned long>(from),
                    static_cast<unsigned long>(starter), static_cast<unsigned long>(actorVtable),
                    static_cast<unsigned>(count));
        return found;
    }
};

HOOK_DEFINE_TRAMPOLINE(Att0220AnimPlay) {
    static void Callback(std::uintptr_t resource) {
        if (resource == 0) {
            ++sMissingSkips;
            if (sMissingSkips <= 8 || (sMissingSkips & (sMissingSkips - 1)) == 0)
                Logging.Log("[GalaxyCoop:smg1] ATT-0220 skipped missing animation skips=%u kill=1",
                            static_cast<unsigned>(sMissingSkips));
            return;
        }
        Orig(resource);
    }
};

}  // namespace

void InstallEffects(const galaxy::target::Identity& identity) {
    if (identity.game != galaxy::target::Game::Smg1) return;
    if (kAtt0214EmitterGuardEnabled) {
        GuardedEmitterCreate::InstallAtOffset(kEmitterCreate);
        Logging.Log("[GalaxyCoop:smg1] ATT-0214 emitter pool guard installed create=0x%lx pool=0x%lx kill=1",
                    static_cast<unsigned long>(kEmitterCreate), static_cast<unsigned long>(kEmitterPool));
    }
    if (kAtt0220MissingAnimGuardEnabled) {
        Att0220AnimLookup::InstallAtOffset(kAnimLookup);
        Att0220AnimPlay::InstallAtOffset(kAnimPlay);
        Logging.Log("[GalaxyCoop:smg1] ATT-0220 missing animation guard installed lookup=0x%lx play=0x%lx kill=1",
                    static_cast<unsigned long>(kAnimLookup), static_cast<unsigned long>(kAnimPlay));
        Logging.Log("[GalaxyCoop:smg1] ATT-0221 all missed lookups logged enabled=%d skip_checked=0x%lx slots=%u kill=1",
                    kAtt0221AllMissesLogEnabled ? 1 : 0, static_cast<unsigned long>(kLookupFromChecked),
                    static_cast<unsigned>(kMissSlots));
    }
}

}  // namespace mod::patches
