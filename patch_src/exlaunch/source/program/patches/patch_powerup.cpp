#include "patch_powerup.hpp"
#include "patch_shared.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <cstddef>
#include <cstdint>

namespace mod::patches {
namespace {
// PATCH-0172 / ATT-0123 / ATT-0124 / ATT-0125 / EVD-0219..EVD-0221, SMG1 main SHA-256
// 76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// NSO offsets only.
//
// EVD-0219: MorphItemNeo::receiveOtherMsg (0x396648) hands the item's morph
// type (actor+0xec) to the grant switch at 0x26AB54. Every case there resolves
// the player the same way - MarioHolder (0x4340), then holder+0x18 (0x432C) -
// and calls MarioActor::changePowerUp(actor, id, effects) at 0x11CAC8. That
// holder field is the P1 actor PATCH-0004 captures, so the item never looks at
// who touched it and P1 always transforms. Morph type maps to power-up id
// 0/7/8->1 invincible, 1->5 spring, 2->4 bee, 3->6 boo, 4->3 ice, 5->2 fire,
// 6->7 red star.
//
// The hook wraps 0x11CAC8 rather than 0x26AB54: 0x11CAC8 opens with four
// position-independent frame instructions (stp/stp/stp/mov x29,sp) while
// 0x26AB54's fourth instruction is a PC-relative b.hi into its jump table.
// Wrapping the setter also covers grant paths outside MorphItemNeo, since all
// twenty of its callers pass an explicit MarioActor.
constexpr std::uintptr_t kMarioChangePowerUp = 0x11CAC8;
// MarioActor fields the setter reads or dereferences for its own actor.
constexpr std::uintptr_t kPowerUpIdOffset = 0x57C;   // current power-up id
constexpr std::uintptr_t kInnerMarioOffset = 0x3C0;
constexpr std::uintptr_t kParamTableOffset = 0x3D8;
constexpr std::uintptr_t kBooModelOffset = 0x0FE0;
constexpr std::uintptr_t kInvincibleModelOffset = 0x1018;
constexpr std::uintptr_t kBeeWingOffset = 0x1040;
constexpr std::uintptr_t kBeeBodyOffset = 0x1048;
constexpr std::uintptr_t kHandLeftOffset = 0x10F0;
constexpr std::uintptr_t kHandRightOffset = 0x10F8;
// EVD-0220: the held power-up item. MarioActor's touch handler 0x61160 stores
// the item HitSensor here only when that actor itself touches a type-0x50
// sensor. After a grant the setter sets +0x1619, 0x66434 then arms the change
// flourish (+0x580 = 0x40), and the flourish in 0xDE850 reads *(sensor+0x30)
// through vtable+0x68 for the effect origin - a null hop on an actor granted a
// power-up it never touched (the P1 crash at LR 0x136CF0). On id 0 the setter
// calls the item host's slot 10 (0x290BF0, ItemRevival) and clears the field,
// so whoever still holds the sensor respawns the item.
constexpr std::uintptr_t kHeldItemSensorOffset = 0x690;
constexpr std::uintptr_t kFlourishPendingOffset = 0x1619;
// EVD-0221: for a type-0x50 item, 0x61160 first sends message 0x87 to the item
// (0xBC1C0), which runs the MorphItemNeo grant - and so this setter -
// synchronously; only after that returns does it revive the previous item and
// store the new sensor at +0x690. At grant time no actor holds the new sensor
// yet, not even the toucher, so rev 2's hand-off never fired and its guard
// cancelled the recipient's flourish (P1 kept the ability but not the model).
constexpr std::uintptr_t kMarioItemTouch = 0x61160;
constexpr std::int32_t kItemTouchMsg = 0x50;
// EVD-0221: each power-up model builder in the visual init 0x1BDC40 (bee:
// 0x1BEDC8) picks the Mario or Luigi archive by the character byte and builds
// nothing unless 0xDC3A0 reports that archive resident. Stages preload only the
// Mario variants, so P2, initialized in Luigi mode (PATCH-0008), never got its
// objects (+0x1040 null in the ATT-0124 run). The holder creation behind
// 0xF3E00 (0xF4050) mounts a missing archive synchronously, the same route
// that already loads P2's Luigi.arc.
constexpr std::uintptr_t kArchiveResident = 0xDC3A0;

// PATCH-0172 kill switch: false restores the native single-player grant, so the
// power-up lands only on whoever MarioHolder+0x18 points at.
constexpr bool kPatch0172Enabled = kSharedPowerUpEnabled;
// false mirrors with the effects flag cleared, which skips the partner's
// transformation flourish; true replays the native argument for both players.
constexpr bool kPatch0172MirrorEffects = true;
// ATT-0124 kill switch: false drops the held-item hand-off and flourish guard,
// which brings back the rev 1 null hop whenever the non-toucher is granted.
constexpr bool kPatch0172ShareHeldItem = true;
// ATT-0125 kill switch: false hands the item over at grant time (rev 2), when
// no actor holds it yet; true waits until the touch handler has stored it.
constexpr bool kPatch0172DeferHeldItem = true;
// ATT-0125 kill switch: false leaves P2 without power-up models on stages that
// preload only the Mario variants; true reports a Luigi power-up archive as
// resident during P2's visual init whenever its Mario counterpart is.
constexpr bool kPatch0172P2PowerUpModels = true;

// DIAG-0180 (EVD-0224) kill switch, test-only: logs which native path ends a
// power-up. ATT-0126 showed P1 losing a shared Bee right after the grant; the
// setter has eight id-0 callers (water cancel 0xDEC9C via 0xDE8D0, duration
// 0x287744, enemy sensor 0x601BC, area 0x58BFC/0x58C20, 0x11B874, 0x137A44,
// 0x2263E0/0x22651C, 0x25E908/0x25E9F0). The return address names the path.
// Answered by EVD-0226 (P1 kept the Bee; the one clear, lr 0x25e83c, came
// with a health drop 3->2, i.e. native damage). Disarmed.
constexpr bool kDiag0180PowerUpClearProbeEnabled = false;

struct PowerUpArchivePair {
    const char* luigi;
    const char* mario;
};
// The names the visual-init builders pass to 0xDC3A0 (EVD-0221).
constexpr PowerUpArchivePair kPowerUpArchives[] = {
    {"BeeLuigi", "BeeMario"},
    {"IceLuigi", "IceMario"},
    {"InvincibleLuigi", "InvincibleMario"},
    {"HopperLuigi", "HopperMario"},
    {"TeresaLuigi", "TeresaMario"},
};

constexpr std::uint32_t kMirrorLogLimit = 12;
constexpr std::uint32_t kItemLogLimit = 8;

bool sMirroring = false;
std::uint32_t sMirrorLogs = 0;
std::uint32_t sSkipLogs = 0;
std::uint32_t sItemLogs = 0;
// The sensor both players held after the last shared grant, with the pair it
// was recorded for. Compared only, never dereferenced, and ignored once the
// player pair changes, so a scene reload cannot make it act on freed memory.
std::uintptr_t sSharedItem = 0;
void* sSharedP1 = nullptr;
void* sSharedP2 = nullptr;

// The actor whose item touch is running, and the recipients granted inside it
// that take the toucher's sensor once the handler has stored it.
void* sTouchActor = nullptr;
void* sTouchRecipients[2] = {nullptr, nullptr};
std::uint32_t sTouchRecipientCount = 0;
std::int16_t sTouchGrantId = 0;
bool sTouchMirrored = false;

bool sP2VisualInit = false;
std::uint32_t sArchiveLogs = 0;

std::uintptr_t& Field(std::uintptr_t actor, std::uintptr_t offset) {
    return *reinterpret_cast<std::uintptr_t*>(actor + offset);
}

std::int16_t PowerUpOf(std::uintptr_t actor) {
    return *reinterpret_cast<const std::int16_t*>(actor + kPowerUpIdOffset);
}

const char* NameOf(std::uintptr_t actor) {
    return (reinterpret_cast<void*>(actor) == GetFirstPlayer()) ? "p1" : "p2";
}

bool SharedItemCurrent() {
    return sSharedItem != 0 && sSharedP1 == GetFirstPlayer() && sSharedP2 == GetSecondPlayer();
}

void RecordSharedItem(std::uintptr_t sensor) {
    sSharedItem = sensor;
    sSharedP1 = GetFirstPlayer();
    sSharedP2 = GetSecondPlayer();
}

// The other spawned player regardless of death phase; the held-item hand-off
// must happen even when that player is not a mirror target.
void* OtherPlayerOf(void* actor) {
    void* const p1 = GetFirstPlayer();
    void* const p2 = GetSecondPlayer();
    if (actor == p1) return p2;
    if (actor == p2) return p1;
    return nullptr;
}

// Returns 0 when every sub-object the setter dereferences for this transition
// is present, otherwise the first missing MarioActor offset. The setter tears
// down the current id's objects and builds the new id's (EVD-0219, EVD-0220),
// so only those two ids matter.
std::uintptr_t MissingPowerUpField(std::uintptr_t actor, std::int16_t id) {
    static constexpr std::uintptr_t kCommon[] = {kInnerMarioOffset, kParamTableOffset};
    static constexpr std::uintptr_t kInvincible[] = {kInvincibleModelOffset, kHandLeftOffset, kHandRightOffset};
    static constexpr std::uintptr_t kBee[] = {kBeeWingOffset, kBeeBodyOffset};
    static constexpr std::uintptr_t kBoo[] = {kBooModelOffset};
    const auto check = [actor](const std::uintptr_t* fields, std::size_t count) -> std::uintptr_t {
        for (std::size_t i = 0; i < count; ++i) {
            if (!IsGuestPointer(Field(actor, fields[i]))) return fields[i];
        }
        return 0;
    };
    const auto checkId = [&check](std::int16_t powerUp) -> std::uintptr_t {
        switch (powerUp) {
        case 1: return check(kInvincible, 3);
        case 4: return check(kBee, 2);
        case 6: return check(kBoo, 1);
        default: return 0;
        }
    };
    if (const std::uintptr_t missing = check(kCommon, 2)) return missing;
    if (const std::uintptr_t missing = checkId(PowerUpOf(actor))) return missing;
    return checkId(id);
}

// Before a grant: make sure the granted actor holds the item sensor. The
// native grant always lands on MarioHolder+0x18 (P1), so when P2 touched the
// item only P2's +0x690 carries it. A value equal to the last shared item is
// stale - the other player just touched a new item and the native touch
// handler already revived the old one - so the fresh sensor wins.
void AdoptHeldItem(std::uintptr_t actor, void* other) {
    if (!kPatch0172ShareHeldItem || !IsValidMarioActor(other)) return;
    const std::uintptr_t mine = Field(actor, kHeldItemSensorOffset);
    const std::uintptr_t theirs = Field(reinterpret_cast<std::uintptr_t>(other), kHeldItemSensorOffset);
    if (!IsGuestPointer(theirs) || theirs == mine) return;
    const bool stale = mine != 0 && SharedItemCurrent() && mine == sSharedItem;
    if (mine != 0 && !stale) return;
    Field(actor, kHeldItemSensorOffset) = theirs;
    if (sItemLogs++ < kItemLogLimit) {
        Logging.Log("[GalaxyCoop:smg1] PATCH-0172 held item adopted actor=%s sensor=0x%lx from=%s replaced=0x%lx kill=1",
                    NameOf(actor), static_cast<unsigned long>(theirs),
                    NameOf(reinterpret_cast<std::uintptr_t>(other)), static_cast<unsigned long>(mine));
    }
}

// After a grant: an actor that still holds no item would take the null hop in
// the flourish, so its flourish is cancelled instead. The power-up itself
// stays; only the change effect is lost.
void GuardFlourish(std::uintptr_t actor) {
    if (!kPatch0172ShareHeldItem || Field(actor, kHeldItemSensorOffset) != 0) return;
    auto* const pending = reinterpret_cast<std::uint8_t*>(actor + kFlourishPendingOffset);
    if (*pending == 0) return;
    *pending = 0;
    if (sItemLogs++ < kItemLogLimit) {
        Logging.Log("[GalaxyCoop:smg1] PATCH-0172 flourish cancelled actor=%s id=%d no_held_item kill=1",
                    NameOf(actor), static_cast<int>(PowerUpOf(actor)));
    }
}

// Before a loss (id 0): when the other player still holds the same item and
// still has a power-up, this actor lets go without reviving it, so the item
// respawns once, when the last of the two loses the form.
void ReleaseSharedItem(std::uintptr_t actor, void* other) {
    if (!kPatch0172ShareHeldItem || !IsValidMarioActor(other)) return;
    const std::uintptr_t mine = Field(actor, kHeldItemSensorOffset);
    const std::uintptr_t otherAddr = reinterpret_cast<std::uintptr_t>(other);
    if (mine == 0 || Field(otherAddr, kHeldItemSensorOffset) != mine || PowerUpOf(otherAddr) == 0) return;
    Field(actor, kHeldItemSensorOffset) = 0;
    if (sItemLogs++ < kItemLogLimit) {
        Logging.Log("[GalaxyCoop:smg1] PATCH-0172 held item released actor=%s sensor=0x%lx partner_keeps_revival=1 kill=1",
                    NameOf(actor), static_cast<unsigned long>(mine));
    }
}

// NSO offset of a code address, or the raw value when it lies outside main.
std::uintptr_t CodeOffset(std::uintptr_t address) {
    const std::uintptr_t base = exl::util::modules::GetTargetStart();
    return (address >= base && address - base < 0x600000) ? address - base : address;
}

std::uint32_t sClearLogs = 0;

// lr is the setter's return address (a tail call names the caller's caller);
// lr2 is one frame further up, read only when the saved frame pointer sits a
// short, aligned distance above this frame on the same stack.
__attribute__((noinline)) void ProbePowerUpClear(std::uintptr_t actor, std::uintptr_t lr) {
    if (!kDiag0180PowerUpClearProbeEnabled || PowerUpOf(actor) == 0 || sClearLogs >= 12) return;
    const auto* const fp = static_cast<const std::uintptr_t*>(__builtin_frame_address(0));
    std::uintptr_t lr2 = 0;
    const std::uintptr_t here = reinterpret_cast<std::uintptr_t>(fp);
    std::uintptr_t up = fp[0];
    // Our frame -> Callback's frame -> the setter caller's frame.
    for (int hop = 0; hop < 1 && up > here && up - here < 0x10000 && (up & 7) == 0; ++hop) {
        up = reinterpret_cast<const std::uintptr_t*>(up)[0];
    }
    if (up > here && up - here < 0x10000 && (up & 7) == 0) {
        lr2 = reinterpret_cast<const std::uintptr_t*>(up)[1];
    }
    ++sClearLogs;
    Logging.Log("[GalaxyCoop:smg1] DIAG-0180 power-up cleared actor=%s prev=%d lr=0x%lx lr2=0x%lx flourish=%u timer=%u held=0x%lx p1_phase=%d p2_phase=%d sample=%u/12 kill=1",
                NameOf(actor), static_cast<int>(PowerUpOf(actor)),
                static_cast<unsigned long>(CodeOffset(lr)), static_cast<unsigned long>(CodeOffset(lr2)),
                static_cast<unsigned>(*reinterpret_cast<const std::uint16_t*>(actor + 0x580)),
                static_cast<unsigned>(*reinterpret_cast<const std::uint16_t*>(actor + 0x584)),
                static_cast<unsigned long>(Field(actor, kHeldItemSensorOffset)),
                static_cast<int>(GetP1DeathPhase()), static_cast<int>(GetP2DeathPhase()), sClearLogs);
}

bool SameName(const char* a, const char* b) {
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

bool InTouch() {
    return kPatch0172ShareHeldItem && kPatch0172DeferHeldItem && sTouchActor != nullptr;
}

// Inside a touch, a recipient other than the toucher is handed the sensor when
// the handler returns; the toucher stores its own natively.
void QueueTouchRecipient(void* actor) {
    if (actor == sTouchActor) return;
    for (std::uint32_t i = 0; i < sTouchRecipientCount; ++i) {
        if (sTouchRecipients[i] == actor) return;
    }
    if (sTouchRecipientCount < 2) sTouchRecipients[sTouchRecipientCount++] = actor;
}

// After the touch handler: each queued recipient adopts the toucher's fresh
// sensor, and one that still has none loses only its flourish. When the
// toucher did not transform itself (its mirror was skipped), the revival moves
// to the recipient so the item cannot be revived twice.
void FinishTouch(void* toucher) {
    const std::uintptr_t toucherAddr = reinterpret_cast<std::uintptr_t>(toucher);
    const std::uintptr_t sensor = Field(toucherAddr, kHeldItemSensorOffset);
    bool handedOver = false;
    for (std::uint32_t i = 0; i < sTouchRecipientCount; ++i) {
        void* const recipient = sTouchRecipients[i];
        sTouchRecipients[i] = nullptr;
        if (!IsValidMarioActor(recipient)) continue;
        const std::uintptr_t recipientAddr = reinterpret_cast<std::uintptr_t>(recipient);
        AdoptHeldItem(recipientAddr, toucher);
        GuardFlourish(recipientAddr);
        if (sensor != 0 && Field(recipientAddr, kHeldItemSensorOffset) == sensor) handedOver = true;
    }
    sTouchRecipientCount = 0;
    if (!IsGuestPointer(sensor)) return;
    if (handedOver && PowerUpOf(toucherAddr) != sTouchGrantId) {
        Field(toucherAddr, kHeldItemSensorOffset) = 0;
        if (sItemLogs++ < kItemLogLimit) {
            Logging.Log("[GalaxyCoop:smg1] PATCH-0172 held item moved off untransformed toucher=%s sensor=0x%lx kill=1",
                        NameOf(toucherAddr), static_cast<unsigned long>(sensor));
        }
        return;
    }
    if (sTouchMirrored) RecordSharedItem(sensor);
}

// The partner is only a mirror target while it is playable; a dead, bubbled or
// recovering player keeps whatever PATCH-0162 gave it.
void* AlivePartnerOf(void* actor) {
    void* const p1 = GetFirstPlayer();
    void* const p2 = GetSecondPlayer();
    if (actor == p1) return (GetP2DeathPhase() == DeathPhase::Alive) ? p2 : nullptr;
    if (actor == p2) return (GetP1DeathPhase() == DeathPhase::Alive) ? p1 : nullptr;
    return nullptr;
}

HOOK_DEFINE_TRAMPOLINE(Patch0172ChangePowerUp) {
    static void Callback(void* actor, std::int16_t powerUp, std::uint64_t effects) {
        if (!kPatch0172Enabled || sMirroring || !IsValidMarioActor(actor)) {
            Orig(actor, powerUp, effects);
            return;
        }
        const std::uintptr_t actorAddr = reinterpret_cast<std::uintptr_t>(actor);
        void* const other = OtherPlayerOf(actor);

        // Ending a power-up stays per player: each actor runs its own duration
        // at +0x584, and death clears it through this same setter.
        if (powerUp == 0) {
            ProbePowerUpClear(actorAddr, reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)));
            ReleaseSharedItem(actorAddr, other);
            Orig(actor, powerUp, effects);
            return;
        }

        const bool inTouch = InTouch();
        if (inTouch) {
            // The recipient keeps its flourish flag; the sensor that flourish
            // reads is handed over before it runs (FinishTouch).
            sTouchGrantId = powerUp;
            Orig(actor, powerUp, effects);
            QueueTouchRecipient(actor);
        } else {
            AdoptHeldItem(actorAddr, other);
            Orig(actor, powerUp, effects);
            GuardFlourish(actorAddr);
        }

        void* const partner = AlivePartnerOf(actor);
        if (!IsValidMarioActor(partner)) return;

        const std::uintptr_t partnerAddr = reinterpret_cast<std::uintptr_t>(partner);
        if (PowerUpOf(partnerAddr) == powerUp) return;
        if (const std::uintptr_t missing = MissingPowerUpField(partnerAddr, powerUp)) {
            if (sSkipLogs++ < 4) {
                Logging.Log("[GalaxyCoop:smg1] PATCH-0172 mirror skipped id=%d partner=%p current=%d missing=+0x%lx kill=1",
                            static_cast<int>(powerUp), partner, static_cast<int>(PowerUpOf(partnerAddr)),
                            static_cast<unsigned long>(missing));
            }
            // The partner will not transform, so the granted actor alone owns
            // the item's revival; a copy left on the partner would revive it a
            // second time the next time the partner touches an item.
            if (kPatch0172ShareHeldItem && !inTouch &&
                Field(partnerAddr, kHeldItemSensorOffset) == Field(actorAddr, kHeldItemSensorOffset)) {
                Field(partnerAddr, kHeldItemSensorOffset) = 0;
            }
            return;
        }

        if (!inTouch) AdoptHeldItem(partnerAddr, actor);
        sMirroring = true;
        Orig(partner, powerUp, kPatch0172MirrorEffects ? effects : 0ull);
        sMirroring = false;
        if (inTouch) {
            QueueTouchRecipient(partner);
            sTouchMirrored = true;
        } else {
            GuardFlourish(partnerAddr);
            if (kPatch0172ShareHeldItem) RecordSharedItem(Field(actorAddr, kHeldItemSensorOffset));
        }

        if (sMirrorLogs++ < kMirrorLogLimit) {
            Logging.Log("[GalaxyCoop:smg1] PATCH-0172 power-up id=%d shared taker=%s from=%p to=%p effects=%d held=0x%lx kill=1",
                        static_cast<int>(powerUp), NameOf(actorAddr), actor, partner,
                        kPatch0172MirrorEffects ? 1 : 0,
                        static_cast<unsigned long>(Field(actorAddr, kHeldItemSensorOffset)));
        }
    }
};

HOOK_DEFINE_TRAMPOLINE(Patch0172ItemTouch) {
    static std::uint64_t Callback(void* actor, const std::int32_t* msg) {
        if (!kPatch0172ShareHeldItem || !kPatch0172DeferHeldItem || sTouchActor != nullptr ||
            msg == nullptr || *msg != kItemTouchMsg || !IsValidMarioActor(actor)) {
            return Orig(actor, msg);
        }
        sTouchActor = actor;
        sTouchRecipientCount = 0;
        sTouchGrantId = 0;
        sTouchMirrored = false;
        const std::uint64_t result = Orig(actor, msg);
        sTouchActor = nullptr;
        FinishTouch(actor);
        return result;
    }
};

HOOK_DEFINE_TRAMPOLINE(Patch0172ArchiveResident) {
    static std::uint64_t Callback(const char* name) {
        const std::uint64_t resident = Orig(name);
        if ((resident & 1) != 0 || !sP2VisualInit || name == nullptr) return resident;
        for (const PowerUpArchivePair& pair : kPowerUpArchives) {
            if (!SameName(name, pair.luigi)) continue;
            const std::uint64_t marioResident = Orig(pair.mario);
            if (sArchiveLogs++ < 8) {
                Logging.Log("[GalaxyCoop:smg1] PATCH-0172 p2 power-up archive %s resident=%d via %s kill=1",
                            name, static_cast<int>(marioResident & 1), pair.mario);
            }
            return marioResident;
        }
        return resident;
    }
};
}  // namespace

void SetP2VisualInitActive(bool active) {
    sP2VisualInit = kPatch0172Enabled && kPatch0172P2PowerUpModels && active;
}

void InstallPowerUp(const galaxy::target::Identity& identity) {
    if (identity.game != galaxy::target::Game::Smg1 || !kPatch0172Enabled) return;
    Patch0172ChangePowerUp::InstallAtOffset(kMarioChangePowerUp);
    if (kPatch0172ShareHeldItem && kPatch0172DeferHeldItem) Patch0172ItemTouch::InstallAtOffset(kMarioItemTouch);
    if (kPatch0172P2PowerUpModels) Patch0172ArchiveResident::InstallAtOffset(kArchiveResident);
    Logging.Log("[GalaxyCoop:smg1] PATCH-0172 rev3 shared power-up installed setter=0x%lx effects=%d held_item=%d deferred=%d p2_models=%d kill=%d",
                static_cast<unsigned long>(kMarioChangePowerUp),
                kPatch0172MirrorEffects ? 1 : 0,
                kPatch0172ShareHeldItem ? 1 : 0,
                kPatch0172DeferHeldItem ? 1 : 0,
                kPatch0172P2PowerUpModels ? 1 : 0,
                kPatch0172Enabled ? 1 : 0);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0180 power-up clear probe armed kill=%d",
                kDiag0180PowerUpClearProbeEnabled ? 1 : 0);
}
}  // namespace mod::patches
