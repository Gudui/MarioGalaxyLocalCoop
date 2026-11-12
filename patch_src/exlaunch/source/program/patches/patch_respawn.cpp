#include "patch_respawn.hpp"
#include "patch_shared.hpp"
#include "patch_lives.hpp"
#include "patch_actions.hpp"
#include "patch_camera.hpp"
#include "patch_splitscreen.hpp"
#include "leader_camera_policy.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <cmath>
#include <limits>

namespace mod::patches {
namespace {
// PATCH-0162 / EVD-0192, SMG1 main SHA-256
// 76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// NSO offsets only. Death entries: EVD-0054/current recheck. Bubble producer,
// live module carrier, completion: EVD-0067/0073/0079/0081/current recheck.
// Isolated deaths never call the full game-over executor: its pointer/demo
// mutations extend beyond the event/camera calls suppressed by old PATCH-0068.
constexpr std::uintptr_t kNormalDeath = 0x1DB92C;
constexpr std::uintptr_t kAbyssDeath = 0x1F49D0;
// ATT-0211 kill switch (EVD-0516, EVD-0517): MarioActor's other death nerves
// ran the native full miss, so one player's fall reloaded the stage for both.
// Nerve table 0x835940..; relocatable openings; Petari names.
constexpr bool kAtt0211ExtraDeathsEnabled = true;
constexpr std::uintptr_t kBlackHoleDeath = 0x3443BC; // exeGameOverBlackHole2
constexpr std::uintptr_t kNonStopDeath = 0x3384C8;   // exeGameOverNonStop
constexpr std::uintptr_t kSinkDeath = 0x338330;      // exeGameOverSink
// ATT-0218 kill switch (EVD-0526): a black hole that took one player stays in
// its swallow nerve (exe 0x3942B8 only loops its sound) until the native stage
// reload, which a per-player death skips, so its attackSensor 0x3940A0 (wait
// nerve only) never takes the other player. Re-arm it when the bubble pops.
constexpr bool kAtt0218BlackHoleRearmEnabled = true;
constexpr std::uintptr_t kBlackHoleHost = 0x1690;          // MarioActor; set by the 0x73 message
constexpr std::uintptr_t kBlackHoleVtable = 0x84E768;
constexpr std::uintptr_t kBlackHoleWaitNerve = 0x94B288;
constexpr std::uintptr_t kBlackHoleSwallowNerve = 0x94B290;
constexpr std::uintptr_t kActorSpine = 0x60;               // spine+8 = current nerve
constexpr std::uintptr_t kDeathCleanup = 0x1DBA30;
constexpr std::uintptr_t kDeathAnimation = 0x1DBC94;
constexpr std::uintptr_t kAnimationStopped = 0x0B8980;
constexpr std::uintptr_t kActorMovement = 0x0DD974;
constexpr std::uintptr_t kActorView = 0x06B748;
constexpr std::uintptr_t kClearStates = 0x11C818;
constexpr std::uintptr_t kSetNerve = 0x0A1BE8; // call-only thunk
constexpr std::uintptr_t kInitialNerveSlot = 0x913E48; // init1BB068 ->939B20
constexpr std::uintptr_t kRecoveryBasis = 0x11A2E4; // receiver is Mario, not actor
constexpr std::uintptr_t kRecoveryStart = 0x33A814;
constexpr std::uintptr_t kWarpUpdate = 0x33AF44;
constexpr std::uintptr_t kWarpClose = 0x33B5F0;
constexpr std::uintptr_t kWarpVtable = 0x836740;
constexpr std::uintptr_t kAddModuleTranslation = 0x1B523C;
constexpr std::uintptr_t kCameraTargetCommit = 0x1B3E48;
constexpr unsigned kDelayTicks = 600; // 10 seconds of 60 Hz gameplay after animation
constexpr unsigned kAnimationMaxTicks = 120;
constexpr unsigned kRetryTicks = 60;
// EVD-0194: restore exeGameOver's per-frame actor velocity stop, which
// PATCH-0162 initially omitted when bypassing the global death executor.
constexpr bool kDeathPoseStopEnabled = true;
constexpr float kBubbleStep = 25.0f;
constexpr float kBubblePopDistance = 60.0f;

struct RespawnState {
    unsigned animationTicks = 0;
    unsigned delayTicks = 0;
    unsigned bubbleTicks = 0;
    unsigned retryLogs = 0;
    bool charged = false;
    float deathOrigin[3] = {};
    bool distanceRecovery = false;
    bool outLogged = false;
    bool outHidden = false; // PATCH-0171: not drawn until the revive bubble starts
    bool blackHole = false; // ATT-0218: routed black-hole death
};
constexpr unsigned kReviveDelayTicks = 60; // PATCH-0171
// PATCH-0171 / EVD-0212: MarioActor vtable835830 slot5 draw. Its native hidden
// flags return before drawMarioModel. Slot 6 (kActorView) is calcAnim, not draw.
constexpr std::uintptr_t kActorDraw = 0x078B94;
RespawnState sState[2];
// PATCH-0166/0167: values and indices only; reset on scene spawn.
int sPipeLeader = -1;
bool sPipeInProgress = false;
unsigned sMovementTick = 0, sMovementStart[2] = {}, sNeutralTicks[2] = {};
bool sMoving[2] = {};
unsigned sDistanceOverTicks = 0, sDistanceCooldown = 0;
unsigned sVisibilityTick=0, sVisibilityBlockedTicks=0;
bool sVisibilityValid=false, sVisibilityBlocked=false;
int sCameraLeader=0, sCameraLeaderCandidate=0;
unsigned sCameraLeaderTicks=0;
LeaderCameraPolicy sLeaderPolicy;
unsigned sLeaderChangeLogs=0;
constexpr float kCatchupDistance = 3000.0f;
constexpr bool kAtt0212LeaderHoldEnabled = true; // ATT-0212 kill switch
// PATCH-0184: distance/visibility bubbles only between players on one surface:
// aligned gravity (core+0x1E8) and a small height offset along shared up,
// or (v2) both gravity lines meeting one planet centre at a similar radius.
constexpr bool kPatch0184SameSurfaceGateEnabled = true;
// v4 (EVD-0248): 0.97 rejected an ordinary tilted walkway (up_dot 0.968) and
// stranded P2; 0.90 is ~26 degrees, still far from a different planet.
constexpr float kSameSurfaceUpDot = 0.90f;
constexpr float kSameSurfaceHeight = 500.0f;
constexpr float kSamePlanetMinUpDiff = 0.01f;   // |up0-up1|^2; below this lines are parallel
constexpr float kSamePlanetMaxRadius = 20000.0f;
constexpr float kSamePlanetResidual = 600.0f;   // radius/altitude mismatch tolerance
constexpr float kSamePlanetResidualRatio = 0.30f; // v4: tolerance scales with radius
// PATCH-0184 v3 (EVD-0246): the camera-visibility regroup used a flat 250-unit
// floor, so on a small planet the horizon hides P2 after a few steps and the
// bubble fires at ~430 units. Scale the floor with the fitted planet radius and
// drop the visibility path entirely on planets small enough to walk around.
constexpr float kVisibilityMinSeparation = 700.0f;   // flat ground floor
constexpr float kVisibilityMaxSeparation = 1800.0f;  // never exceed the distance rule
constexpr float kVisibilityPlanetArc = 1.3f;         // ~75 degrees of arc
constexpr float kSmallPlanetNoVisibilityBubble = 900.0f;
// PATCH-0184 v4 (EVD-0248): a failed same-surface test no longer vetoes the
// catch-up outright. It only disables the short-range visibility path and
// raises the distance threshold, so P1 walking into a room with its own
// gravity still pulls P2 along instead of leaving it behind forever.
constexpr float kCrossSurfaceCatchupDistance = 4000.0f;
constexpr float kSplitCatchupDistance = 6000.0f; // ATT-0140
int sSameSurfaceLogged = -1;
unsigned sSameSurfaceLogs = 0;
constexpr std::uintptr_t kActorStick = 0x123654; // EVD0185, native actor gates
bool sNativeDeathCharged = false;
bool sReloadIssued = false;
unsigned sBubbleCallbackDepth = 0;

template<typename T> T Fn(std::uintptr_t offset) {
    return reinterpret_cast<T>(exl::util::modules::GetTargetStart() + offset);
}
template<typename T> T& Field(std::uintptr_t object, std::uintptr_t offset) {
    return *reinterpret_cast<T*>(object + offset);
}
int PlayerIndex(void* actor) {
    if (!IsValidMarioActor(actor)) return -1;
    if (actor == GetFirstPlayer()) return 0;
    if (actor == GetSecondPlayer()) return 1;
    return -1;
}
void* Player(int index) { return index == 0 ? GetFirstPlayer() : GetSecondPlayer(); }
DeathPhase Phase(int index) { return index == 0 ? GetP1DeathPhase() : GetP2DeathPhase(); }
void SetPhase(int index, DeathPhase phase) {
    if (index == 0) SetP1DeathPhase(phase); else SetP2DeathPhase(phase);
}
std::uintptr_t Core(void* actor) {
    if (!IsValidMarioActor(actor)) return 0;
    const auto core = Field<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(actor), 0x3C0);
    return IsGuestPointer(core) && Field<void*>(core, 8) == actor ? core : 0;
}
// PATCH-0191 (EVD-0470): kActorStick 123654 -> 66D18 loads [actor+0x3C8]+0x18
// and 57224 then reads +0x88 from it. After a native death reload the new P2
// (PATCH-0004) runs its first ticks with +0x3C8 still null; skip it until set.
constexpr bool kStickReadyGuard = true;
bool StickReady(void* actor) {
    if (!kStickReadyGuard) return true;
    const auto pad = Field<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(actor), 0x3C8);
    return IsGuestPointer(pad) && IsGuestPointer(Field<std::uintptr_t>(pad, 0x18));
}
bool Finite3(const float* v) {
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(v[i]) || std::abs(v[i]) > 1.0e7f) return false;
    return true;
}
bool Living(int index) {
    auto* actor = Player(index);
    const auto phase = Phase(index);
    return IsValidMarioActor(actor) &&
        (phase == DeathPhase::Alive || phase == DeathPhase::Recovering) &&
        Field<int>(reinterpret_cast<std::uintptr_t>(actor), 0x528) > 0;
}
void ChargeOnce(int index) {
    if (sState[index].charged) return;
    sState[index].charged = true;
    sNativeDeathCharged = true;
    if (index == 0) {
        if (GetP1Lives() > 0) DecrementP1LivesOnce();
    } else DecrementP2LivesOnce();
    Logging.Log("[GalaxyCoop:smg1] PATCH-0162 death charged p%d lives=(%u,%u) kill=1",
                index + 1, GetP1Lives(), GetP2Lives());
}

bool StartBubble(int index, bool distance = false);
// PATCH-0188 (EVD-0248): while the partner sits inside a native binding the
// scene is mid-travel - a Launch Star, a green pipe, or the scripted departure
// out of an observatory dome. A player left far behind on the old geometry
// then falls into the void through no fault of its own. Charge nothing for
// that; regroup it onto the partner instead.
constexpr bool kPatch0188PartnerTravelRescueEnabled = true;
constexpr float kPartnerTravelRescueSeparation = 3000.0f;
unsigned sPartnerTravelRescueLogs = 0;
unsigned sTravelBubbleLogs = 0;
// PATCH-0188 v2 (EVD-0250): v1 asked whether the partner was bound at the exact
// frame of the fatal callback. In the AstroDome the blue star had already let
// P1 go - and PATCH-0166 had put P1 in a bubble, so its phase was no longer
// Alive - by the time P2 hit the void, so nothing was rescued and a life was
// spent. Latch the binding for a whole departure instead of sampling it once.
constexpr unsigned kPartnerTravelRescueTicks = 900; // 15 s of departure demo
constexpr unsigned kTravelSettleTicks = 180;        // 3 s after a rush ends
unsigned sTravelTick = 0;
unsigned sLastBoundTick[2] = {0,0};
void SampleNativeBindings() {
    ++sTravelTick;
    for (int i = 0; i < 2; ++i) {
        auto* actor = Player(i);
        if (actor && IsValidMarioActor(actor) &&
            HasActiveBinding(reinterpret_cast<std::uintptr_t>(actor)))
            sLastBoundTick[i] = sTravelTick;
    }
}
bool TravellingWithin(int index, unsigned ticks) {
    return sLastBoundTick[index] && sTravelTick - sLastBoundTick[index] <= ticks;
}
bool RescueDuringPartnerTravel(int index) {
    if (!kPatch0188PartnerTravelRescueEnabled || !kBubbleRespawnEnabled) return false;
    void* partner = Player(1-index);
    const auto partnerPhase = Phase(1-index);
    // A partner riding its own bubble is still alive and still travelling.
    if (!partner || (partnerPhase != DeathPhase::Alive && partnerPhase != DeathPhase::Recovering))
        return false;
    if (!TravellingWithin(1-index, kPartnerTravelRescueTicks) &&
        !HasActiveBinding(reinterpret_cast<std::uintptr_t>(partner))) return false;
    const auto self = Core(Player(index));
    const auto other = Core(partner);
    if (!self || !other) return false;
    float squared = 0;
    for (int axis = 0; axis < 3; ++axis) {
        const float d = Field<float>(self,0x140+axis*4) - Field<float>(other,0x140+axis*4);
        squared += d*d;
    }
    if (!std::isfinite(squared) ||
        squared <= kPartnerTravelRescueSeparation*kPartnerTravelRescueSeparation) return false;
    if (!StartBubble(index,true)) return false;
    if (sPartnerTravelRescueLogs < 8) {
        ++sPartnerTravelRescueLogs;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0188 p%d rescued mid-travel separation=%.1f lives=(%u,%u) kill=1",
                    index+1, static_cast<double>(std::sqrt(squared)), GetP1Lives(), GetP2Lives());
    }
    return true;
}

// Returns true only for the one native all-dead execution. No resetting death
// latches until the next scene; repeated fatal callbacks cannot spend lives.
bool HandleDeath(void* actor, const char* reason) {
    const int index = PlayerIndex(actor);
    if (index < 0 || !HasActivePlayerPair() || !Core(actor)) return true;
    if (sReloadIssued) return false;
    if (kDeathPoseStopEnabled &&
        (Phase(index) == DeathPhase::Alive || Phase(index) == DeathPhase::Animating)) {
        const auto a = reinterpret_cast<std::uintptr_t>(actor);
        // Keep native model animation running, but do not integrate the last
        // enemy hit's velocity for the entire sitting/collapse animation.
        // Do not freeze movement(), zero scale, or move the corpse to P1.
        for (int axis = 0; axis < 3; ++axis) Field<float>(a, 0x38 + axis*4) = 0.0f;
    }
    if (Phase(index) != DeathPhase::Alive) return false;
    if (RescueDuringPartnerTravel(index)) return false;
    for (int axis = 0; axis < 3; ++axis)
        sState[index].deathOrigin[axis] = Field<float>(reinterpret_cast<std::uintptr_t>(actor), 0x14 + axis*4);
    ChargeOnce(index);
    SetPhase(index, DeathPhase::Animating);
    Field<int>(reinterpret_cast<std::uintptr_t>(actor), 0x528) = 0;
    // PATCH-0171: both players out of lives uses the native all-down path.
    const bool allOut = kCoopLifeRulesEnabled && GetP1Lives() == 0 && GetP2Lives() == 0;
    if (!Living(1 - index) || allOut) {
        if (allOut) Logging.Log("[GalaxyCoop:smg1] PATCH-0171 both players at 0 lives; native all-down kill=1");
        // Same-frame zero HP may precede the other actor's death callback.
        ChargeOnce(1 - index);
        sReloadIssued = true;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0162 both down; native checkpoint reload once kill=1");
        return true;
    }
    sState[index].animationTicks = 0;
    Fn<void (*)(void*)>(kDeathCleanup)(actor);
    Fn<void (*)(void*)>(kDeathAnimation)(actor);
    Logging.Log("[GalaxyCoop:smg1] PATCH-0162 p%d death animation reason=%s kill=1", index + 1, reason);
    return false;
}

HOOK_DEFINE_TRAMPOLINE(RespawnNormalDeath) {
    static void Callback(void* actor) { if (HandleDeath(actor, "normal")) Orig(actor); }
};
HOOK_DEFINE_TRAMPOLINE(RespawnAbyssDeath) {
    static void Callback(void* actor) { if (HandleDeath(actor, "abyss")) Orig(actor); }
};
HOOK_DEFINE_TRAMPOLINE(RespawnBlackHoleDeath) {
    static void Callback(void* actor) {
        if (HandleDeath(actor, "blackhole")) { Orig(actor); return; }
        const int index = PlayerIndex(actor);
        if (index >= 0 && Phase(index) != DeathPhase::Alive) sState[index].blackHole = true;
    }
};

void RearmBlackHole(int index) {
    if (!kAtt0218BlackHoleRearmEnabled) return;
    const auto target = exl::util::modules::GetTargetStart();
    void* actor = Player(index);
    if (!IsValidMarioActor(actor)) return;
    const auto host = Field<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(actor), kBlackHoleHost);
    if (!IsGuestPointer(host) || Field<std::uintptr_t>(host, 0) != target + kBlackHoleVtable) return;
    const auto spine = Field<std::uintptr_t>(host, kActorSpine);
    if (!IsGuestPointer(spine) || Field<std::uintptr_t>(spine, 8) != target + kBlackHoleSwallowNerve) return;
    Fn<void (*)(std::uintptr_t, const void*)>(kSetNerve)(host, reinterpret_cast<const void*>(target + kBlackHoleWaitNerve));
    Logging.Log("[GalaxyCoop:smg1] ATT-0218 black hole re-armed after p%d host=0x%lx kill=1",
                index + 1, static_cast<unsigned long>(host - target));
}
HOOK_DEFINE_TRAMPOLINE(RespawnNonStopDeath) {
    static void Callback(void* actor) { if (HandleDeath(actor, "nonstop")) Orig(actor); }
};
HOOK_DEFINE_TRAMPOLINE(RespawnSinkDeath) {
    static void Callback(void* actor) { if (HandleDeath(actor, "sink")) Orig(actor); }
};
HOOK_DEFINE_TRAMPOLINE(RespawnActorMovement) {
    static void Callback(void* actor) {
        const int index = PlayerIndex(actor);
        // Keep the hidden player out of native physics/fatal execution during
        // the delay. Scale remains native: no singular zero-scale matrices.
        if (index >= 0 && Phase(index) == DeathPhase::Ghost && !sReloadIssued) return;
        Orig(actor);
    }
};
HOOK_DEFINE_TRAMPOLINE(RespawnActorView) {
    static void Callback(void* actor) {
        const int index = PlayerIndex(actor);
        if (index >= 0 && Phase(index) == DeathPhase::Ghost && !sReloadIssued) return;
        Orig(actor);
    }
};
HOOK_DEFINE_TRAMPOLINE(RespawnActorDraw) {
    static void Callback(void* actor) {
        // PATCH-0171: a player out of lives is not drawn, like native hidden
        // Mario. Ordinary death delays keep the collapsed body visible.
        const int index = PlayerIndex(actor);
        if (kCoopLifeRulesEnabled && index == 1 && Phase(index) == DeathPhase::Ghost &&
            (GetP2Lives() == 0 || sState[index].outHidden)) return;
        Orig(actor);
    }
};

bool QueryDestination(int index, float* destination, float* up, float* axis1, float* axis2) {
    if (sPipeInProgress) return false; // never recover into an in-flight pipe
    if (!Living(1 - index)) return false;
    const auto core = Core(Player(1 - index));
    if (!core) return false;
    const auto* base = Fn<const float* (*)(std::uintptr_t, float*, float*, float*)>(kRecoveryBasis)(
        core, up, axis1, axis2);
    if (!IsGuestPointer(reinterpret_cast<std::uintptr_t>(base)) ||
        !Finite3(base) || !Finite3(up) || !Finite3(axis1) || !Finite3(axis2)) return false;
    const float length = std::sqrt(up[0]*up[0] + up[1]*up[1] + up[2]*up[2]);
    if (length < 0.01f || length > 100.0f) return false;
    for (int i = 0; i < 3; ++i) {
        up[i] /= length;
        destination[i] = base[i] + up[i] * 160.0f;
    }
    return Finite3(destination);
}
std::uintptr_t WarpFor(void* actor) {
    const auto core = Core(actor);
    if (!core) return 0;
    const auto warp = Field<std::uintptr_t>(core, 0x8F8);
    if (!IsGuestPointer(warp) ||
        Field<std::uintptr_t>(warp, 0) != exl::util::modules::GetTargetStart() + kWarpVtable ||
        Field<void*>(warp, 8) != actor) return 0;
    return warp;
}
int RecoveringOwner(std::uintptr_t warp) {
    if (!IsGuestPointer(warp)) return -1;
    for (int i = 0; i < 2; ++i)
        if (Phase(i) == DeathPhase::Recovering && WarpFor(Player(i)) == warp) return i;
    return -1;
}
struct BubbleCallbackScope {
    BubbleCallbackScope() { ++sBubbleCallbackDepth; }
    ~BubbleCallbackScope() { --sBubbleCallbackDepth; }
};
HOOK_DEFINE_TRAMPOLINE(RespawnCameraTargetCommit) {
    static void Callback(const void* descriptor) {
        // Covers both start's 1DC2F0 wrapper and the producer's far-distance
        // direct commit. Scope exists only inside this player's recovery calls.
        if (sBubbleCallbackDepth != 0) return;
        Orig(descriptor);
    }
};

bool StartBubble(int index, bool distance) {
    void* actor = Player(index);
    const auto core = Core(actor);
    const auto warp = WarpFor(actor);
    float destination[3], up[3], axis1[3], axis2[3];
    if (!core || !warp || !QueryDestination(index, destination, up, axis1, axis2)) return false;
    const auto a = reinterpret_cast<std::uintptr_t>(actor);
    const int restoredHealth=distance ? Field<int>(a, 0x528) : 3;
    if (restoredHealth<=0) return false;
    const auto spine = Field<std::uintptr_t>(a, 0x60);
    if (!IsGuestPointer(spine)) return false;
    const auto target = exl::util::modules::GetTargetStart();
    const auto initialNerve = *reinterpret_cast<void* const*>(target + kInitialNerveSlot);
    if (!IsGuestPointer(reinterpret_cast<std::uintptr_t>(initialNerve))) return false;
    float previousActor[3], previousCore[3];
    for (int i=0;i<3;++i) {
        previousActor[i]=Field<float>(a,0x14+i*4);
        previousCore[i]=Field<float>(core,0x140+i*4);
    }
    if (!distance) {
        Fn<void (*)(std::uintptr_t, void*)>(kClearStates)(core, nullptr);
        // Only the death route clears terminal state and stages above the
        // partner. Distance recovery starts at the player's current position.
        Field<std::uint8_t>(a, 0x545) = 0;
        Field<std::uint16_t>(a, 0x12A8) = 0;
        Field<std::uint8_t>(a, 0x111E) = 0;
        Field<std::uint16_t>(a, 0x1708) = 0;
        Field<std::uintptr_t>(a, 0x1700) = 0;
    }
    Field<int>(a, 0x528) = restoredHealth;
    if (!distance) for (int i = 0; i < 3; ++i) {
        const float start = destination[i] + up[i] * 400.0f;
        Field<float>(a, 0x14 + i*4) = start;
        Field<float>(core, 0x140 + i*4) = start;
        Field<float>(a, 0x38 + i*4) = 0.0f;
        Field<float>(core, 0x14C + i*4) = 0.0f;
    }
    if (!distance) Fn<void (*)(void*, const void*)>(kSetNerve)(actor, initialNerve);
    SetPhase(index, DeathPhase::Recovering); // start invokes virtual update synchronously
    sState[index].distanceRecovery=distance;
    sState[index].bubbleTicks = 0;
    bool started;
    {
        BubbleCallbackScope scope;
        started = Fn<bool (*)(std::uintptr_t, const float*, const float*, const float*, const float*)>(kRecoveryStart)(
            core, destination, up, axis1, axis2);
    }
    if (!started) {
        SetPhase(index, distance ? DeathPhase::Alive : DeathPhase::Ghost);
        Field<int>(a, 0x528) = distance ? restoredHealth : 0;
        sState[index].distanceRecovery=false;
        if (distance) for(int i=0;i<3;++i) {
            Field<float>(a,0x14+i*4)=previousActor[i];
            Field<float>(core,0x140+i*4)=previousCore[i];
        }
        return false;
    }
    Field<int>(warp, 0x78) = 3; // bypass WipeCam states1/2; EVD-0081
    if (distance) {
        Logging.Log("[GalaxyCoop:smg1] PATCH-0166 p%d distance bubble started hp=%d lives=(%u,%u) kill=1",
                    index+1,restoredHealth,GetP1Lives(),GetP2Lives());
    } else Logging.Log("[GalaxyCoop:smg1] PATCH-0162 p%d bubble started after600 delay ticks hp=3 kill=1", index + 1);
    return true;
}

HOOK_DEFINE_TRAMPOLINE(RespawnWarpUpdate) {
    static bool Callback(std::uintptr_t warp) {
        const int index = RecoveringOwner(warp);
        if (index < 0) return Orig(warp);
        BubbleCallbackScope scope;
        const auto core = Core(Player(index));
        if (!core) return Orig(warp);
        float destination[3], up[3], axis1[3], axis2[3];
        const bool haveDestination = QueryDestination(index, destination, up, axis1, axis2);
        float before[3];
        for (int i = 0; i < 3; ++i) before[i] = Field<float>(core, 0x140 + i*4);
        if (!haveDestination || !Finite3(before)) {
            Field<int>(warp, 0x78) = 3;
            return Orig(warp);
        }
        float delta[3];
        float distanceSquared = 0.0f;
        for (int i = 0; i < 3; ++i) {
            delta[i] = destination[i] - before[i];
            distanceSquared += delta[i]*delta[i];
            Field<float>(warp, 0x20 + i*4) = destination[i];
        }
        const unsigned ticks = ++sState[index].bubbleTicks;
        if (ticks > 40 && distanceSquared < kBubblePopDistance*kBubblePopDistance) {
            Fn<void (*)(std::uintptr_t, const float*)>(kAddModuleTranslation)(warp, delta);
            return false; // native state machine calls close/pop exactly once
        }
        Field<int>(warp, 0x78) = 3;
        Orig(warp); // retain native effects, animation and recovery setup
        // Continuous live-partner carrier, correcting native eased motion.
        // Leave the initial40-tick materialization intact.
        if (ticks > 40) {
            const float distance = std::sqrt(distanceSquared);
            const float fraction = distance > kBubbleStep ? kBubbleStep/distance : 1.0f;
            for (int i = 0; i < 3; ++i)
                delta[i] = before[i] + delta[i]*fraction - Field<float>(core, 0x140 + i*4);
            Fn<void (*)(std::uintptr_t, const float*)>(kAddModuleTranslation)(warp, delta);
        }
        return true;
    }
};
HOOK_DEFINE_TRAMPOLINE(RespawnWarpClose) {
    static bool Callback(std::uintptr_t warp) {
        const int index = RecoveringOwner(warp);
        const bool result = Orig(warp);
        if (index >= 0) {
            const bool distance=sState[index].distanceRecovery;
            if (distance) sDistanceCooldown=120;
            SetPhase(index, DeathPhase::Alive);
            if (sState[index].blackHole) RearmBlackHole(index);
            sState[index] = {};
            // Native close supplies its own120-tick post-recovery damage grace.
            Logging.Log("[GalaxyCoop:smg1] %s p%d bubble popped; native control/grace restored lives=(%u,%u) kill=1",
                        distance ? "PATCH-0166" : "PATCH-0162", index + 1, GetP1Lives(), GetP2Lives());
        }
        return result;
    }
};
// Sample both routed native sticks once per gameplay tick. A start edge is
// remembered across continuous movement; brief neutral gaps are debounced.
// EVD-0199 policy correction: an idle player follows a moving partner.
// Otherwise use latest start; equal-frame starts choose P2 deterministically.
// No native heap is cached.
void TickDistanceCatchup() {
    if (!kDistanceCatchupEnabled) return;
    if (sDistanceCooldown) --sDistanceCooldown;
    if (IsP2Parked() || sPipeInProgress || Phase(0)!=DeathPhase::Alive ||
        Phase(1)!=DeathPhase::Alive || !Living(0) || !Living(1)) {
        sDistanceOverTicks=0;sVisibilityBlockedTicks=0;sVisibilityValid=false;
        sLeaderPolicy.Pause();
        return;
    }
    float pos[2][3];
    for (int i=0;i<2;++i) {
        const auto core=Core(Player(i));
        const auto actor=reinterpret_cast<std::uintptr_t>(Player(i));
        // F20 is the native active binding sensor (EVD0198). Never pull a
        // player out of a pipe, Launch Star or another native binding.
        // PATCH-0180: stale enemy entries (+0xF38 clear) do not count.
        // PATCH-0184 v5 (EVD-0250): a rush that has just ended still leaves the
        // traveller thousands of units away in the middle of a scripted
        // departure. Bubbling it there drags it back out of the demo, which is
        // what yanked P1 off the AstroDome blue star. Hold until it settles.
        if (!core || !StickReady(Player(i)) || HasActiveBinding(actor) || TravellingWithin(i,kTravelSettleTicks)) {sDistanceOverTicks=0;sVisibilityBlockedTicks=0;sVisibilityValid=false;sLeaderPolicy.Pause();return;}
        for(int axis=0;axis<3;++axis) pos[i][axis]=Field<float>(core,0x140+axis*4);
        if (!Finite3(pos[i])) {sDistanceOverTicks=0;sVisibilityBlockedTicks=0;sVisibilityValid=false;sLeaderPolicy.Pause();return;}
    }
    ++sMovementTick;
    for(int i=0;i<2;++i) {
        float x=0,y=0;
        Fn<void (*)(void*,float*,float*)>(kActorStick)(Player(i),&x,&y);
        const float magnitude=x*x+y*y;
        if (magnitude>0.04f && std::isfinite(magnitude)) {
            if (!sMoving[i]) sMovementStart[i]=sMovementTick;
            sMoving[i]=true;sNeutralTicks[i]=0;
        } else if (magnitude<0.01f && ++sNeutralTicks[i]>=6) sMoving[i]=false;
    }
    float squared=0;
    for(int axis=0;axis<3;++axis) {const float d=pos[0][axis]-pos[1][axis];squared+=d*d;}
    const bool oneMoving=sMoving[0]!=sMoving[1];
    int chosen=oneMoving ? (sMoving[0] ? 1 : 0) :
        (sMovementStart[0]>sMovementStart[1] ? 0 : 1);
    if (kNativeLeaderCameraEnabled) {
        const int previous=sLeaderPolicy.leader;
        // ATT-0212 (EVD-0516): no handoff while an event camera or the split
        // owns the view. In a cutscene only P2 moves, so the leader passed to
        // P2 and the split reopened from P2's view (roll -136.6, up_dot -1);
        // in a split the calc forces P1 back each frame, so handoffs repeated.
        if (kAtt0212LeaderHoldEnabled && (IsCoopEventCamera() || IsCoopSplitActive() || IsCoopSplitPreparing()))
            sLeaderPolicy.Pause();
        else sLeaderPolicy.Tick(sMoving[0],sMoving[1]);
        chosen=sLeaderPolicy.Follower(sMoving[0],sMoving[1]);
        if (previous!=sLeaderPolicy.leader) {
            sVisibilityBlockedTicks=0;sVisibilityValid=false;
            if (sLeaderChangeLogs++<24)
                Logging.Log("[GalaxyCoop:smg1] PATCH-0169 handoff p%d -> p%d sustained_sole_mover=1 kill=1",previous+1,sLeaderPolicy.leader+1);
        }
    } else {
        const int leader=1-chosen;
        if (leader!=sCameraLeaderCandidate) {sCameraLeaderCandidate=leader;sCameraLeaderTicks=0;}
        if (sCameraLeaderTicks<12) ++sCameraLeaderTicks;
        if (sCameraLeaderTicks>=12) sCameraLeader=leader;
    }
    // PATCH-0184 v3: raised below once the planet fit is known.
    float visibilityMinSeparation = kVisibilityMinSeparation;
    bool crossSurface = false;
    if (kPatch0184SameSurfaceGateEnabled) {
        float up[2][3];
        bool valid=true;
        for(int i=0;i<2&&valid;++i) {
            const auto core=Core(Player(i));
            float length=0;
            for(int axis=0;axis<3;++axis) {up[i][axis]=-Field<float>(core,0x1E8+axis*4);length+=up[i][axis]*up[i][axis];}
            valid=std::isfinite(length) && length>1e-6f;
            if (valid) {length=std::sqrt(length);for(int axis=0;axis<3;++axis) up[i][axis]/=length;}
        }
        float upDot=0,height=0,upDiff2=0,dDotU=0;
        if (valid) for(int axis=0;axis<3;++axis) {
            upDot+=up[0][axis]*up[1][axis];
            height+=(pos[1][axis]-pos[0][axis])*0.5f*(up[0][axis]+up[1][axis]);
            const float u=up[0][axis]-up[1][axis];
            upDiff2+=u*u;dDotU+=(pos[0][axis]-pos[1][axis])*u;
        }
        // Sphere: p_i = C + r*up_i gives p0-p1 = r*(up0-up1); residual measures the misfit.
        float radius=0,residual=-1;
        bool planet=false;
        if (valid && upDiff2>=kSamePlanetMinUpDiff) {
            radius=dDotU/upDiff2;
            float r2=0;
            for(int axis=0;axis<3;++axis) {const float e=(pos[0][axis]-pos[1][axis])-radius*(up[0][axis]-up[1][axis]);r2+=e*e;}
            residual=std::sqrt(r2);
            planet=radius>100.0f && radius<=kSamePlanetMaxRadius &&
                   residual<=std::fmax(kSamePlanetResidual,radius*kSamePlanetResidualRatio);
        }
        const bool flat=valid && upDot>=kSameSurfaceUpDot && std::fabs(height)<=kSameSurfaceHeight;
        const bool same=flat || planet;
        // A planet you can walk around in a couple of seconds never needs a
        // visibility regroup; the horizon hides the other player constantly.
        if (planet) visibilityMinSeparation = radius<kSmallPlanetNoVisibilityBubble ?
            std::numeric_limits<float>::infinity() :
            std::fmin(kVisibilityMaxSeparation,std::fmax(kVisibilityMinSeparation,radius*kVisibilityPlanetArc));
        if ((same?1:0)!=sSameSurfaceLogged && sSameSurfaceLogs<48) {
            sSameSurfaceLogged=same?1:0;++sSameSurfaceLogs;
            Logging.Log("[GalaxyCoop:smg1] PATCH-0184 same_surface=%d flat=%d planet=%d up_dot=%.3f height=%.1f radius=%.1f residual=%.1f separation=%.1f vis_min=%.1f kill=1",
                        same?1:0,flat?1:0,planet?1:0,static_cast<double>(upDot),static_cast<double>(height),
                        static_cast<double>(radius),static_cast<double>(residual),static_cast<double>(std::sqrt(squared)),
                        static_cast<double>(visibilityMinSeparation));
        }
        if (!same) {
            crossSurface = true;
            visibilityMinSeparation = std::numeric_limits<float>::infinity();
        }
    }
    const bool freshVisibility=sVisibilityValid && sMovementTick-sVisibilityTick<=2;
    if ((kPlanetCameraEnabled || kNativeLeaderCameraEnabled) && freshVisibility && sVisibilityBlocked &&
        squared>visibilityMinSeparation*visibilityMinSeparation && !sDistanceCooldown)
        ++sVisibilityBlockedTicks;
    else sVisibilityBlockedTicks=0;
    // ATT-0140: a blocked or diverging view splits the screen instead.
    const bool visibilityReturn=!IsCoopSplitAvailable() &&
        sVisibilityBlockedTicks>=(kNativeLeaderCameraEnabled ? 30u : 90u);
    // PATCH-0170: shared framing already widens, keep the pre-leader fallback.
    // ATT-0140: when the draw can split, separation is shown, not regrouped;
    // only a real drift apart (~6000) still bubbles the follower back.
    const float catchupDistance=IsCoopSplitAvailable() ? kSplitCatchupDistance :
        crossSurface ? kCrossSurfaceCatchupDistance :
        (kNativeLeaderCameraEnabled && !IsCoopSharedFraming() ? 1800.0f : kCatchupDistance);
    if (squared>catchupDistance*catchupDistance && !sDistanceCooldown) ++sDistanceOverTicks;
    else sDistanceOverTicks=0;
    if ((!visibilityReturn && sDistanceOverTicks<12) || (!sMovementStart[0]&&!sMovementStart[1])) return;
    sDistanceOverTicks=0;sDistanceCooldown=60;
    if (StartBubble(chosen,true)) {
        if (kNativeLeaderCameraEnabled) {
            sLeaderPolicy.Follow(1-chosen);
            Logging.Log("[GalaxyCoop:smg1] PATCH-0169 regroup p%d -> p%d visibility=%d blocked_ticks=%u separation=%.1f idle_priority=%d kill=1",
                chosen+1,2-chosen,visibilityReturn?1:0,sVisibilityBlockedTicks,static_cast<double>(std::sqrt(squared)),oneMoving?1:0);
        } else if (visibilityReturn)
            Logging.Log("[GalaxyCoop:smg1] PATCH-0168 visibility regroup p%d -> p%d blocked_ticks=%u separation=%.1f kill=1",
                        chosen+1,2-chosen,sVisibilityBlockedTicks,static_cast<double>(std::sqrt(squared)));
        Logging.Log("[GalaxyCoop:smg1] PATCH-0166 catchup p%d -> p%d separation=%.1f movement_starts=(%u,%u) moving=(%d,%d) idle_priority=%d threshold=%.0f kill=1",
                    chosen+1,2-chosen,static_cast<double>(std::sqrt(squared)),sMovementStart[0],sMovementStart[1],
                    sMoving[0]?1:0,sMoving[1]?1:0,oneMoving?1:0,static_cast<double>(catchupDistance));
    }
    sVisibilityBlockedTicks=0;
}
} // namespace

bool SuppressChargedNativeDeath() { return kBubbleRespawnEnabled && sNativeDeathCharged; }
void NotifyCoopLifeGained(int index) {
    if (!kCoopLifeRulesEnabled || index < 0 || index > 1 || sReloadIssued) return;
    if (Phase(index) != DeathPhase::Ghost || !sState[index].outLogged) return;
    sState[index].outLogged = false;
    if (sState[index].delayTicks > kReviveDelayTicks) sState[index].delayTicks = kReviveDelayTicks;
    Logging.Log("[GalaxyCoop:smg1] PATCH-0171 p%d revived by life gain; bubble in %u ticks lives=(%u,%u) kill=1",
                index + 1, sState[index].delayTicks, GetP1Lives(), GetP2Lives());
}
int GetCoopCameraLeaderIndex() {
    if (IsCoopRegrouping()) return Phase(0)==DeathPhase::Alive ? 0 : 1;
    return kNativeLeaderCameraEnabled ? sLeaderPolicy.leader : sCameraLeader;
}
void HoldCoopCameraLeader(int index) {
    if (kNativeLeaderCameraEnabled && (index==0 || index==1) && sLeaderPolicy.leader!=index) sLeaderPolicy.Follow(index);
}
bool IsCoopRegrouping() {
    // If the partner dies during a regroup, leave camera selection to the
    // existing death path instead of orbiting around a collapsing actor.
    return (Phase(0)==DeathPhase::Recovering && sState[0].distanceRecovery && Phase(1)==DeathPhase::Alive) ||
           (Phase(1)==DeathPhase::Recovering && sState[1].distanceRecovery && Phase(0)==DeathPhase::Alive);
}
void ReportCoopCameraVisibility(bool valid, bool blocked) {
    sVisibilityTick=sMovementTick;sVisibilityValid=valid;sVisibilityBlocked=blocked;
}
void ResetBubbleRespawn() {
    sState[0] = {}; sState[1] = {};
    sNativeDeathCharged = false;
    sReloadIssued = false;
    sBubbleCallbackDepth = 0;
    sPipeLeader=-1; sPipeInProgress=false;
    sMovementTick=0;sDistanceOverTicks=0;sDistanceCooldown=0;
    sVisibilityTick=0;sVisibilityBlockedTicks=0;sVisibilityValid=false;sVisibilityBlocked=false;
    sCameraLeader=0;sCameraLeaderCandidate=0;sCameraLeaderTicks=0;
    sLeaderPolicy={};sLeaderChangeLogs=0;sPartnerTravelRescueLogs=0;
    sTravelTick=0;sLastBoundTick[0]=0;sLastBoundTick[1]=0;sTravelBubbleLogs=0;
    for(int i=0;i<2;++i) {sMovementStart[i]=0;sNeutralTicks[i]=0;sMoving[i]=false;}
}
bool IsCoopTravelHidden(void*) { return false; } // PATCH0167 uses native pipe visibility
void* GetCoopPipeLeader() {
    if (!kPipeCompanionEnabled || sPipeLeader<0 || !sPipeInProgress) return nullptr;
    void* actor=Player(sPipeLeader);
    return IsValidMarioActor(actor) ? actor : nullptr;
}
void BeginCoopPipeTravel(void* leader) {
    if (!kPipeCompanionEnabled || !HasActivePlayerPair() || IsP2Parked()) return;
    const int index=PlayerIndex(leader);
    if (index<0 || Phase(index)!=DeathPhase::Alive || sPipeInProgress) return;
    sPipeLeader=index;sPipeInProgress=true;
    if (kNativeLeaderCameraEnabled) sLeaderPolicy.Follow(index);
}
void FinishCoopPipeTravel(void* leader) {
    if (PlayerIndex(leader)!=sPipeLeader) return;
    sPipeInProgress=false;sPipeLeader=-1;sDistanceCooldown=120;
}
// PATCH-0189 (EVD-0250/EVD-0251): the departure host binds only the player who
// touched it. Give the partner the proven co-op bubble for the demo so it rides
// along instead of standing on geometry the departure is about to tear down.
bool RequestCoopTravelBubble(void* stranded) {
    if (!kBubbleRespawnEnabled || !HasActivePlayerPair() || IsP2Parked()) return false;
    const int index = PlayerIndex(stranded);
    if (index < 0 || Phase(index) != DeathPhase::Alive) return false;
    if (HasActiveBinding(reinterpret_cast<std::uintptr_t>(stranded))) return false;
    if (!StartBubble(index, true)) return false;
    sDistanceCooldown = 120;
    if (sTravelBubbleLogs < 8) {
        ++sTravelBubbleLogs;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0189 p%d bubbled for partner departure lives=(%u,%u) kill=1",
                    index+1, GetP1Lives(), GetP2Lives());
    }
    return true;
}
void TickBubbleRespawn() {
    if (!kBubbleRespawnEnabled || !HasActivePlayerPair() || sReloadIssued) return;
    SampleNativeBindings();
    TickDistanceCatchup();
    for (int i = 0; i < 2; ++i) {
        if (Phase(i) == DeathPhase::Animating) {
            const unsigned ticks = ++sState[i].animationTicks;
            const bool stopped = Fn<bool (*)(void*)>(kAnimationStopped)(Player(i));
            if ((ticks >= 30 && stopped) || ticks >= kAnimationMaxTicks) {
                SetPhase(i, DeathPhase::Ghost);
                sState[i].delayTicks = kDelayTicks;
                float distanceSquared = 0.0f;
                for (int axis = 0; axis < 3; ++axis) {
                    const float delta = Field<float>(reinterpret_cast<std::uintptr_t>(Player(i)), 0x14 + axis*4) - sState[i].deathOrigin[axis];
                    distanceSquared += delta*delta;
                }
                Logging.Log("[GalaxyCoop:smg1] PATCH-0162 p%d animation ended ticks=%u stopped=%d; delay=600 death_drift=%.1f pose_stop=%d kill=1",
                            i + 1, ticks, stopped ? 1 : 0, static_cast<double>(std::sqrt(distanceSquared)), kDeathPoseStopEnabled ? 1 : 0);
            }
        } else if (Phase(i) == DeathPhase::Ghost) {
            // PATCH-0171: P2 without lives stays hidden until a life arrives.
            if (kCoopLifeRulesEnabled && i == 1 && GetP2Lives() == 0) {
                sState[i].outHidden = true;
                if (!sState[i].outLogged) {
                    sState[i].outLogged = true;
                    Logging.Log("[GalaxyCoop:smg1] PATCH-0171 p2 out of lives; held hidden lives=(%u,%u) kill=1",
                                GetP1Lives(), GetP2Lives());
                }
                continue;
            }
            if (sState[i].delayTicks > 0) --sState[i].delayTicks;
            if (sState[i].delayTicks == 0 && !StartBubble(i)) {
                sState[i].delayTicks = kRetryTicks;
                if (sState[i].retryLogs++ < 4)
                    Logging.Log("[GalaxyCoop:smg1] PATCH-0162 p%d bubble pending valid native recovery destination kill=1", i + 1);
            }
        }
    }
}
void InstallRespawn(const galaxy::target::Identity& identity) {
    if (identity.game != galaxy::target::Game::Smg1 || !kBubbleRespawnEnabled) return;
    RespawnNormalDeath::InstallAtOffset(kNormalDeath);
    RespawnAbyssDeath::InstallAtOffset(kAbyssDeath);
    if (kAtt0211ExtraDeathsEnabled) {
        RespawnBlackHoleDeath::InstallAtOffset(kBlackHoleDeath);
        RespawnNonStopDeath::InstallAtOffset(kNonStopDeath);
        RespawnSinkDeath::InstallAtOffset(kSinkDeath);
    }
    RespawnActorMovement::InstallAtOffset(kActorMovement);
    RespawnActorView::InstallAtOffset(kActorView);
    RespawnWarpUpdate::InstallAtOffset(kWarpUpdate);
    RespawnWarpClose::InstallAtOffset(kWarpClose);
    RespawnCameraTargetCommit::InstallAtOffset(kCameraTargetCommit);
    if (kCoopLifeRulesEnabled) RespawnActorDraw::InstallAtOffset(kActorDraw);
    Logging.Log("[GalaxyCoop:smg1] PATCH-0162 isolated death +10s native bubble respawn installed old0068=0 pose_stop=%d kill=1", kDeathPoseStopEnabled ? 1 : 0);
    Logging.Log("[GalaxyCoop:smg1] ATT-0211 extra deaths routed blackhole=0x%lx nonstop=0x%lx sink=0x%lx kill=%d",
                static_cast<unsigned long>(kBlackHoleDeath), static_cast<unsigned long>(kNonStopDeath),
                static_cast<unsigned long>(kSinkDeath), kAtt0211ExtraDeathsEnabled ? 1 : 0);
    Logging.Log("[GalaxyCoop:smg1] ATT-0218 black hole re-arm on bubble pop vtable=0x%lx kill=%d",
                static_cast<unsigned long>(kBlackHoleVtable), kAtt0218BlackHoleRearmEnabled ? 1 : 0);
    Logging.Log("[GalaxyCoop:smg1] PATCH-0171 out player draw hidden draw=0x%lx kill=%d",
                static_cast<unsigned long>(kActorDraw), kCoopLifeRulesEnabled ? 1 : 0);
}
} // namespace mod::patches
