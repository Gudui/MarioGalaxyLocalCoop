#include "patch_shared.hpp"
#include "patch_lives.hpp"

namespace mod::patches {

namespace {
void* sFirstPlayer = nullptr;
void* sSecondPlayer = nullptr;

DeathPhase sP1DeathPhase = DeathPhase::Alive;
DeathPhase sP2DeathPhase = DeathPhase::Alive;
std::uint32_t sP1AnimTimer = 0;
std::uint32_t sP2AnimTimer = 0;
std::uint32_t sP1RespawnTimer = 0;
std::uint32_t sP2RespawnTimer = 0;
std::uint32_t sP1InvulnerableTimer = 0;
std::uint32_t sP2InvulnerableTimer = 0;
bool sSuppressGlobalDeathSequence = false;

void* sP1LastAliveNerve = nullptr;
void* sP2LastAliveNerve = nullptr;

constexpr std::uint32_t kInvulnerableDurationFrames = 120; // 2 seconds post-respawn grace

// Verified NSO offsets for MarioActor and inner Mario in SMG1 v1.3.1
constexpr std::uintptr_t kMarioActorPositionOffset = 0x14;          // actor+0x14 (pos XYZ: 0x14, 0x18, 0x1c)
constexpr std::uintptr_t kMarioActorScaleOffset = 0x2C;             // actor+0x2c (scale XYZ: 0x2c, 0x30, 0x34)
constexpr std::uintptr_t kMarioActorVelocityOffset = 0x38;          // actor+0x38 (vel XYZ: 0x38, 0x3c, 0x40)
constexpr std::uintptr_t kMarioInnerOffset = 0x3C0;
constexpr std::uintptr_t kMarioInnerPositionOffset = 0x140;         // innerMario+0x140 (pos XYZ: 0x140, 0x144, 0x148)
constexpr std::uintptr_t kMarioInnerVelocityOffset = 0x14C;         // innerMario+0x14c (vel XYZ: 0x14c, 0x150, 0x154)
constexpr std::uintptr_t kMarioInnerGravityOffset = 0x1E8;          // innerMario+0x1e8 (gravity XYZ: 0x1e8, 0x1ec, 0x1f0, via FUN_71000a0d48)
constexpr std::uintptr_t kMarioInnerSafeGroundOffset1 = 0x950;      // innerMario+0x950 (safe ground XYZ: 0x950, 0x954, 0x958)
constexpr std::uintptr_t kMarioInnerSafeGroundOffset2 = 0x9B0;      // innerMario+0x9b0 (safe ground XYZ: 0x9b0, 0x9b4, 0x9b8)
constexpr std::uintptr_t kMarioStateClearFunc = 0x11C818;          // FUN_710011c818(innerMario, nullptr)
constexpr std::uintptr_t kMarioActorSetNerveThunk = 0x0A1BE8;       // MR::setNerve(actor, nerve)
constexpr std::uintptr_t kMarioActorDefaultNerve = 0x939B20;        // default playable nerve (exeWait, from 0x913e48)
constexpr std::uintptr_t kMarioInnerInvulnerableOffset = 0x8F0;     // innerMario+0x8f0 (invulnerable byte)
constexpr std::uintptr_t kMarioInnerEnableDamageOffset = 0x8F1;     // innerMario+0x8f1 (enable normal damage byte)
constexpr std::uintptr_t kMarioActorHealthOffset = 0x528;           // actor+0x528 (health wedges)
constexpr std::uintptr_t kMarioActorFatalReasonOffset = 0x545;      // actor+0x545 (fatal reason byte)
constexpr std::uintptr_t kMarioActorDamagePendingOffset = 0x530;    // actor+0x530 (damage pending word)
constexpr std::uintptr_t kMarioActorDamageStateOffset = 0x588;      // actor+0x588 (damage state short)
constexpr std::uintptr_t kMarioActorStatusFlagsOffset = 0x111E;     // actor+0x111e (status flag byte)
}  // namespace

bool IsGuestPointer(std::uintptr_t ptr) {
    return ptr >= kMinGuestPointer &&
           ptr < kMaxGuestPointer &&
           (ptr & (alignof(std::uintptr_t) - 1u)) == 0;
}

bool IsValidMarioActor(void* actor) {
    if (!IsGuestPointer(reinterpret_cast<std::uintptr_t>(actor))) {
        return false;
    }
    const std::uintptr_t target = exl::util::modules::GetTargetStart();
    const std::uintptr_t expectedVtable = target + kMarioActorVtableSmg1;
    return *reinterpret_cast<std::uintptr_t*>(actor) == expectedVtable;
}

void SetCapturedPlayers(void* p1, void* p2) {
    sFirstPlayer = p1;
    sSecondPlayer = p2;
}

void ClearCapturedPlayers() {
    sFirstPlayer = nullptr;
    sSecondPlayer = nullptr;
    sP1DeathPhase = DeathPhase::Alive;
    sP2DeathPhase = DeathPhase::Alive;
    sP1AnimTimer = 0;
    sP2AnimTimer = 0;
    sP1RespawnTimer = 0;
    sP2RespawnTimer = 0;
    sP1InvulnerableTimer = 0;
    sP2InvulnerableTimer = 0;
    sSuppressGlobalDeathSequence = false;
    sP1LastAliveNerve = nullptr;
    sP2LastAliveNerve = nullptr;
}

DeathPhase GetP1DeathPhase() { return sP1DeathPhase; }
DeathPhase GetP2DeathPhase() { return sP2DeathPhase; }
void SetP1DeathPhase(DeathPhase phase) { sP1DeathPhase = phase; }
void SetP2DeathPhase(DeathPhase phase) { sP2DeathPhase = phase; }

std::uint32_t GetP1AnimTimer() { return sP1AnimTimer; }
std::uint32_t GetP2AnimTimer() { return sP2AnimTimer; }
void SetP1AnimTimer(std::uint32_t frames) { sP1AnimTimer = frames; }
void SetP2AnimTimer(std::uint32_t frames) { sP2AnimTimer = frames; }

std::uint32_t GetP1RespawnTimer() { return sP1RespawnTimer; }
std::uint32_t GetP2RespawnTimer() { return sP2RespawnTimer; }
void SetP1RespawnTimer(std::uint32_t frames) { sP1RespawnTimer = frames; }
void SetP2RespawnTimer(std::uint32_t frames) { sP2RespawnTimer = frames; }

// Compile-time switch: when true, mod manually manages innerMario+0x8F0 post-respawn grace.
// When false (vanilla testing), native engine manages +0x8F0 invulnerability entirely.
constexpr bool kModInvulnTimerEnabled = false;

std::uint32_t GetP1InvulnerableTimer() { return sP1InvulnerableTimer; }
std::uint32_t GetP2InvulnerableTimer() { return sP2InvulnerableTimer; }
void DecrementInvulnerableTimers() {
    if (sP1InvulnerableTimer > 0) {
        --sP1InvulnerableTimer;
        if (kModInvulnTimerEnabled && sP1InvulnerableTimer == 0 && IsValidMarioActor(sFirstPlayer)) {
            const std::uintptr_t core = *reinterpret_cast<const std::uintptr_t*>(
                reinterpret_cast<std::uintptr_t>(sFirstPlayer) + kMarioInnerOffset);
            if (IsGuestPointer(core)) {
                *reinterpret_cast<std::uint8_t*>(core + kMarioInnerInvulnerableOffset) = 0;
            }
        }
    }
    if (sP2InvulnerableTimer > 0) {
        --sP2InvulnerableTimer;
        if (kModInvulnTimerEnabled && sP2InvulnerableTimer == 0 && IsValidMarioActor(sSecondPlayer)) {
            const std::uintptr_t core = *reinterpret_cast<const std::uintptr_t*>(
                reinterpret_cast<std::uintptr_t>(sSecondPlayer) + kMarioInnerOffset);
            if (IsGuestPointer(core)) {
                *reinterpret_cast<std::uint8_t*>(core + kMarioInnerInvulnerableOffset) = 0;
            }
        }
    }
}

bool IsP1Dead() { return sP1DeathPhase != DeathPhase::Alive; }
bool IsP2Dead() { return sP2DeathPhase != DeathPhase::Alive; }

bool IsPlayerGhost(void* actor) {
    if (actor == sFirstPlayer) {
        return sP1DeathPhase == DeathPhase::Ghost;
    }
    if (actor == sSecondPlayer) {
        return sP2DeathPhase == DeathPhase::Ghost;
    }
    return false;
}

bool IsPlayerInvulnerable(void* actor) {
    if (actor == sFirstPlayer) {
        return sP1InvulnerableTimer > 0;
    }
    if (actor == sSecondPlayer) {
        return sP2InvulnerableTimer > 0;
    }
    return false;
}

bool GetSuppressGlobalDeathSequence() { return sSuppressGlobalDeathSequence; }
void SetSuppressGlobalDeathSequence(bool suppress) { sSuppressGlobalDeathSequence = suppress; }

void CaptureAliveNerves() {
    if (sP1DeathPhase == DeathPhase::Alive && IsValidMarioActor(sFirstPlayer)) {
        const std::uintptr_t p1Addr = reinterpret_cast<std::uintptr_t>(sFirstPlayer);
        const int p1Health = *reinterpret_cast<const int*>(p1Addr + 0x528);
        if (p1Health > 0) {
            const std::uintptr_t p1Spine = *reinterpret_cast<const std::uintptr_t*>(p1Addr + 0x60);
            if (IsGuestPointer(p1Spine)) {
                void* curNerve = *reinterpret_cast<void**>(p1Spine + 0x8);
                if (curNerve != nullptr) sP1LastAliveNerve = curNerve;
            }
        }
    }
    if (sP2DeathPhase == DeathPhase::Alive && IsValidMarioActor(sSecondPlayer)) {
        const std::uintptr_t p2Addr = reinterpret_cast<std::uintptr_t>(sSecondPlayer);
        const int p2Health = *reinterpret_cast<const int*>(p2Addr + 0x528);
        if (p2Health > 0) {
            const std::uintptr_t p2Spine = *reinterpret_cast<const std::uintptr_t*>(p2Addr + 0x60);
            if (IsGuestPointer(p2Spine)) {
                void* curNerve = *reinterpret_cast<void**>(p2Spine + 0x8);
                if (curNerve != nullptr) sP2LastAliveNerve = curNerve;
            }
        }
    }
}

void* GetFirstPlayer() {
    return sFirstPlayer;
}

void* GetSecondPlayer() {
    return sSecondPlayer;
}

bool HasActivePlayerPair() {
    return IsValidMarioActor(sFirstPlayer) && IsValidMarioActor(sSecondPlayer);
}

void DeactivateDeadActor(void* actor) {
    if (!IsValidMarioActor(actor)) return;

    const std::uintptr_t actorAddr = reinterpret_cast<std::uintptr_t>(actor);

    // 1. Zero velocity XYZ (+0x38, +0x3C, +0x40)
    float* vel = reinterpret_cast<float*>(actorAddr + kMarioActorVelocityOffset);
    vel[0] = 0.0f;
    vel[1] = 0.0f;
    vel[2] = 0.0f;

    // 2. Zero core velocity XYZ
    const std::uintptr_t core = *reinterpret_cast<const std::uintptr_t*>(actorAddr + kMarioInnerOffset);
    if (IsGuestPointer(core)) {
        float* coreVel = reinterpret_cast<float*>(core + kMarioInnerVelocityOffset);
        coreVel[0] = 0.0f;
        coreVel[1] = 0.0f;
        coreVel[2] = 0.0f;
    }

    // 3. Set health to 0
    *reinterpret_cast<int*>(actorAddr + kMarioActorHealthOffset) = 0;

    // 4. Hide model by scaling to 0.0f
    float* scale = reinterpret_cast<float*>(actorAddr + kMarioActorScaleOffset);
    scale[0] = 0.0f;
    scale[1] = 0.0f;
    scale[2] = 0.0f;
}

void KeepDeadPlayerParked(void* deadActor, void* partnerActor) {
    if (!IsValidMarioActor(deadActor) || !IsValidMarioActor(partnerActor)) return;

    const std::uintptr_t deadAddr = reinterpret_cast<std::uintptr_t>(deadActor);
    const std::uintptr_t partnerAddr = reinterpret_cast<std::uintptr_t>(partnerActor);

    // Pin position to partner at +0x14
    const float* partnerPos = reinterpret_cast<const float*>(partnerAddr + kMarioActorPositionOffset);
    float* deadPos = reinterpret_cast<float*>(deadAddr + kMarioActorPositionOffset);
    deadPos[0] = partnerPos[0];
    deadPos[1] = partnerPos[1];
    deadPos[2] = partnerPos[2];

    // Zero velocity
    float* deadVel = reinterpret_cast<float*>(deadAddr + kMarioActorVelocityOffset);
    deadVel[0] = 0.0f;
    deadVel[1] = 0.0f;
    deadVel[2] = 0.0f;

    // Ensure scale remains 0.0f during ghost phase
    float* scale = reinterpret_cast<float*>(deadAddr + kMarioActorScaleOffset);
    scale[0] = 0.0f;
    scale[1] = 0.0f;
    scale[2] = 0.0f;

    const std::uintptr_t deadCore = *reinterpret_cast<const std::uintptr_t*>(deadAddr + kMarioInnerOffset);
    if (IsGuestPointer(deadCore)) {
        float* corePos = reinterpret_cast<float*>(deadCore + kMarioInnerPositionOffset);
        corePos[0] = deadPos[0];
        corePos[1] = deadPos[1];
        corePos[2] = deadPos[2];

        float* coreVel = reinterpret_cast<float*>(deadCore + kMarioInnerVelocityOffset);
        coreVel[0] = 0.0f;
        coreVel[1] = 0.0f;
        coreVel[2] = 0.0f;

        // Synchronize safe ground recovery basis to partner position
        float* ground1 = reinterpret_cast<float*>(deadCore + kMarioInnerSafeGroundOffset1);
        ground1[0] = partnerPos[0];
        ground1[1] = partnerPos[1];
        ground1[2] = partnerPos[2];
        float* ground2 = reinterpret_cast<float*>(deadCore + kMarioInnerSafeGroundOffset2);
        ground2[0] = partnerPos[0];
        ground2[1] = partnerPos[1];
        ground2[2] = partnerPos[2];
    }

    // Keep health 0
    *reinterpret_cast<int*>(deadAddr + kMarioActorHealthOffset) = 0;
}

void RespawnPlayer(void* deadActor, void* partnerActor) {
    if (!IsValidMarioActor(deadActor) || !IsValidMarioActor(partnerActor)) return;

    const std::uintptr_t deadAddr = reinterpret_cast<std::uintptr_t>(deadActor);
    const std::uintptr_t partnerAddr = reinterpret_cast<std::uintptr_t>(partnerActor);

    // 1. Calculate true spherical UP vector from partner's local planetary gravity
    // Partner's inner Mario core at +0x3C0 -> +0x1E8 stores the local gravity vector (gx, gy, gz)
    float upX = 0.0f;
    float upY = 1.0f;
    float upZ = 0.0f;
    const std::uintptr_t partnerCore = *reinterpret_cast<const std::uintptr_t*>(partnerAddr + kMarioInnerOffset);
    if (IsGuestPointer(partnerCore)) {
        const float* gravity = reinterpret_cast<const float*>(partnerCore + kMarioInnerGravityOffset);
        const float gSq = gravity[0] * gravity[0] + gravity[1] * gravity[1] + gravity[2] * gravity[2];
        if (gSq > 0.0001f) {
            const float invLen = 1.0f / __builtin_sqrtf(gSq);
            upX = -gravity[0] * invLen;
            upY = -gravity[1] * invLen;
            upZ = -gravity[2] * invLen;
        }
    }

    // 2. Safe surface placement: place player 40 units above partner along true planetary normal
    // Gentle touchdown directly beside partner; prevents terrain clipping or falling into planet core
    const float* partnerPos = reinterpret_cast<const float*>(partnerAddr + kMarioActorPositionOffset);
    float* deadPos = reinterpret_cast<float*>(deadAddr + kMarioActorPositionOffset);
    deadPos[0] = partnerPos[0] + 40.0f * upX;
    deadPos[1] = partnerPos[1] + 40.0f * upY;
    deadPos[2] = partnerPos[2] + 40.0f * upZ;

    // 3. Restore scale to 1.0f (re-appear)
    float* scale = reinterpret_cast<float*>(deadAddr + kMarioActorScaleOffset);
    scale[0] = 1.0f;
    scale[1] = 1.0f;
    scale[2] = 1.0f;

    // 4. Zero initial velocity (clean touchdown, zero momentum into floor)
    float* deadVel = reinterpret_cast<float*>(deadAddr + kMarioActorVelocityOffset);
    deadVel[0] = 0.0f;
    deadVel[1] = 0.0f;
    deadVel[2] = 0.0f;

    const std::uintptr_t deadCore = *reinterpret_cast<const std::uintptr_t*>(deadAddr + kMarioInnerOffset);
    if (IsGuestPointer(deadCore)) {
        float* corePos = reinterpret_cast<float*>(deadCore + kMarioInnerPositionOffset);
        corePos[0] = deadPos[0];
        corePos[1] = deadPos[1];
        corePos[2] = deadPos[2];

        float* coreVel = reinterpret_cast<float*>(deadCore + kMarioInnerVelocityOffset);
        coreVel[0] = 0.0f;
        coreVel[1] = 0.0f;
        coreVel[2] = 0.0f;

        // Synchronize safe ground recovery basis to partner position
        // Guarantees that if the engine ever queries recovery basis, it returns partner's ground
        float* ground1 = reinterpret_cast<float*>(deadCore + kMarioInnerSafeGroundOffset1);
        ground1[0] = partnerPos[0];
        ground1[1] = partnerPos[1];
        ground1[2] = partnerPos[2];
        float* ground2 = reinterpret_cast<float*>(deadCore + kMarioInnerSafeGroundOffset2);
        ground2[0] = partnerPos[0];
        ground2[1] = partnerPos[1];
        ground2[2] = partnerPos[2];

        // 5. Pop/clear all states from inner MarioState stack (+0xBE0)
        const std::uintptr_t target = exl::util::modules::GetTargetStart();
        using MarioStateClearFn = void (*)(void*, void*);
        const auto clearState = reinterpret_cast<MarioStateClearFn>(target + kMarioStateClearFunc);
        clearState(reinterpret_cast<void*>(deadCore), nullptr);

        // 6. Set invulnerable flag (+0x8F0) during post-respawn grace
        *reinterpret_cast<std::uint8_t*>(deadCore + kMarioInnerInvulnerableOffset) = 1;

        // 7. Ensure normal damage is enabled (+0x8F1)
        *reinterpret_cast<std::uint8_t*>(deadCore + kMarioInnerEnableDamageOffset) = 1;
    }

    // 8. Restore health to full 3 wedges
    *reinterpret_cast<int*>(deadAddr + kMarioActorHealthOffset) = 3;

    // 9. Clear fatal reason and pending damage flags on MarioActor
    *reinterpret_cast<std::uint8_t*>(deadAddr + kMarioActorFatalReasonOffset) = 0;
    *reinterpret_cast<std::uint32_t*>(deadAddr + kMarioActorDamagePendingOffset) = 0;
    *reinterpret_cast<std::uint16_t*>(deadAddr + kMarioActorDamageStateOffset) = 0;
    *reinterpret_cast<std::uint8_t*>(deadAddr + kMarioActorStatusFlagsOffset) = 0;

    // 10. Transition Spine nerve back to native default playable nerve (0x939B20)
    const std::uintptr_t target = exl::util::modules::GetTargetStart();
    const auto setNerve = reinterpret_cast<void (*)(void*, const void*)>(target + kMarioActorSetNerveThunk);
    const void* defaultNerve = reinterpret_cast<const void*>(target + kMarioActorDefaultNerve);
    setNerve(deadActor, defaultNerve);

    if (deadActor == sFirstPlayer) {
        sP1DeathPhase = DeathPhase::Alive;
        sP1RespawnTimer = 0;
        sP1AnimTimer = 0;
        sP1InvulnerableTimer = kInvulnerableDurationFrames;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0068 Respawned Mario (P1) cleanly above Luigi! Restored health=3, cleared state stack & flags");
    } else {
        sP2DeathPhase = DeathPhase::Alive;
        sP2RespawnTimer = 0;
        sP2AnimTimer = 0;
        sP2InvulnerableTimer = kInvulnerableDurationFrames;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0068 Respawned Luigi (P2) cleanly above Mario! Restored health=3, cleared state stack & flags");
    }
}

}  // namespace mod::patches
