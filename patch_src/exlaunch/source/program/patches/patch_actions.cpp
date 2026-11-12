#include "patch_actions.hpp"
#include "patch_shared.hpp"
#include "patch_camera.hpp"
#include "patch_input.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <cmath>

namespace mod::patches {

namespace {

constexpr std::uintptr_t kPatch0019RootSlot = 0x90CEA8;
constexpr std::uintptr_t kPatch0019SceneEquals = 0x1C2320;
constexpr std::uintptr_t kPatch0019StageEquals = 0x09BF90;
// SMG1 main SHA-256 76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// NSO 0x123654: native actor-local stick output, EVD-0026 / EVD-0183.
// Returns zero while its native input checks reject movement. ATT-0056 /
// EVD-0110 proved camera activity alone is insufficient for parking release.
constexpr std::uintptr_t kPatch0019GetStickValue = 0x123654;
constexpr bool kPatch0019InputReleaseEnabled = true; // ATT-0107 kill switch
// ATT-0131 kill switch: also release when P1's native movement gate is open
// and either controller gives stick or A/B/X/Y/ZL input, so P2 (or P1's
// buttons) can start play; false keeps the P1-stick-only release.
constexpr bool kPatch0019AnyPlayerReleaseEnabled = true;
// ATT-0136 (EVD-0253): the galaxy fly-in binds P1 as a native rush (host
// 0x81AA60, +0xF38 set; EVD-0238) and 0x123654's actor gates still pass
// during it, so a jump press released P2 in empty space. Hold parking,
// which carries P2 beside P1, until P1's rush binding ends.
constexpr bool kPatch0019HoldWhileP1BoundEnabled = true;
// ATT-0210 kill switch (EVD-0514, EVD-0515): HeavensDoorGalaxy and
// PeachCastleGardenGalaxy kept P2 parked for the whole stage. Their scripted
// entries are now held by the arrival gate below like any other stage, and
// their Mario-only spin lockout by PATCH-0013; true restores whole-stage parking.
constexpr bool kPatch0019ParkPrologueEnabled = false;
// PATCH-0013 kill switch (EVD-0052, EVD-0053): P2 copies P1's spin-enabled
// byte MarioActor+0x161B. Its only explicit writer, setter 0x27A700, resolves
// the singleton (P1); the other +0x161B sites 0x670DC, 0x11B45C, 0x14CCD8,
// 0x16BD3C and 0x1FA658 only read it (Ghidra 0x161B scan, 2026-10-25).
constexpr bool kPatch0013SpinMirrorEnabled = true;
constexpr std::uintptr_t kMarioActorSpinEnabled = 0x161B;

bool sP2Parked = true;
bool sCameraManGameSeen = false;
std::uint32_t sInputWaitTicks = 0;
std::uint32_t sParkedSampleCount = 0;
std::uint32_t sBoundDeferLogs = 0;
std::uint32_t sSpinMirrorLogs = 0;
bool sPrologueLogged = false;
constexpr std::uint32_t kParkedSampleLimit = 10;

bool HasNativeP1Movement(void* p1Actor, float* x, float* y) {
    *x = 0.0f;
    *y = 0.0f;
    if (!IsValidMarioActor(p1Actor)) return false;
    const auto p1 = reinterpret_cast<std::uintptr_t>(p1Actor);
    // Native getStickValue follows actor+0x3C0; validate that hop first.
    const auto mario = *reinterpret_cast<const std::uintptr_t*>(p1 + 0x3C0);
    if (!IsGuestPointer(mario)) return false;
    const auto target = exl::util::modules::GetTargetStart();
    using GetStickFn = void (*)(void*, float*, float*);
    reinterpret_cast<GetStickFn>(target + kPatch0019GetStickValue)(p1Actor, x, y);
    // Match the established 0.1 movement threshold; reject NaN/out-of-range.
    return *x >= -1.0f && *x <= 1.0f && *y >= -1.0f && *y <= 1.0f &&
           (*x * *x + *y * *y > 0.01f);
}

bool IsProtectedScene() {
    const std::uintptr_t target = exl::util::modules::GetTargetStart();
    const uintptr_t rootSlot = target + kPatch0019RootSlot;
    const uintptr_t* rootPtrPtr = reinterpret_cast<const uintptr_t*>(rootSlot);
    if (rootPtrPtr == nullptr) return true;
    const uintptr_t root = *rootPtrPtr;
    if (!IsGuestPointer(root)) return true;

    const uintptr_t obj = *reinterpret_cast<const uintptr_t*>(root);
    if (!IsGuestPointer(obj)) return true;

    const uintptr_t sceneInfo = *reinterpret_cast<const uintptr_t*>(obj + 0x48);
    if (!IsGuestPointer(sceneInfo)) return true;

    using NameEqualFn = bool (*)(const char*);
    const auto sceneEquals = reinterpret_cast<NameEqualFn>(target + kPatch0019SceneEquals);
    const auto stageEquals = reinterpret_cast<NameEqualFn>(target + kPatch0019StageEquals);

    if (sceneEquals("FileSelect")) return true;
    const char* prologue = stageEquals("HeavensDoorGalaxy")       ? "HeavensDoorGalaxy"
                           : stageEquals("PeachCastleGardenGalaxy") ? "PeachCastleGardenGalaxy"
                                                                    : nullptr;
    if (prologue == nullptr) return false;
    if (!sPrologueLogged) {
        sPrologueLogged = true;
        Logging.Log("[GalaxyCoop:smg1] ATT-0210 prologue stage=%s parked=%d kill=1", prologue,
                    kPatch0019ParkPrologueEnabled ? 1 : 0);
    }
    return kPatch0019ParkPrologueEnabled;
}

// ATT-0211 kill switch (EVD-0516): park P2 150 units to P1's side along P1's
// ground (world +X with its gravity component removed) instead of world +X,
// which on a round planet can sit inside the surface or over a pit. Gravity is
// Mario core (+0x3C0) +0x1E8, as RespawnPlayer and PATCH-0184 read it.
constexpr bool kAtt0211ParkAlongGroundEnabled = true;
constexpr float kParkDistance = 150.0f;

void ParkOffset(std::uintptr_t p1, float offset[3]) {
    offset[0] = kParkDistance;
    offset[1] = 0.0f;
    offset[2] = 0.0f;
    if (!kAtt0211ParkAlongGroundEnabled) return;
    const auto core = *reinterpret_cast<const std::uintptr_t*>(p1 + 0x3C0);
    if (!IsGuestPointer(core)) return;
    const float* g = reinterpret_cast<const float*>(core + 0x1E8);
    const float gLength = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (!std::isfinite(gLength) || gLength < 1.0e-3f) return;
    const float down[3] = {g[0] / gLength, g[1] / gLength, g[2] / gLength};
    // World +X, or +Z when gravity is nearly along X, minus its gravity part.
    float side[3] = {1.0f, 0.0f, 0.0f};
    if (std::fabs(down[0]) > 0.9f) side[0] = 0.0f, side[2] = 1.0f;
    const float along = side[0] * down[0] + side[1] * down[1] + side[2] * down[2];
    for (int axis = 0; axis < 3; ++axis) side[axis] -= along * down[axis];
    const float sideLength = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
    if (!std::isfinite(sideLength) || sideLength < 1.0e-3f) return;
    for (int axis = 0; axis < 3; ++axis) offset[axis] = kParkDistance * side[axis] / sideLength;
}

void ApplyP2ParkingCarrier(void* p1Actor, void* p2Actor) {
    if (!IsValidMarioActor(p1Actor) || !IsValidMarioActor(p2Actor)) return;

    const uintptr_t p1 = reinterpret_cast<uintptr_t>(p1Actor);
    const uintptr_t p2 = reinterpret_cast<uintptr_t>(p2Actor);

    const float* p1Pos = reinterpret_cast<const float*>(p1 + 0x14);
    float* p2Pos = reinterpret_cast<float*>(p2 + 0x14);
    float* p2Vel = reinterpret_cast<float*>(p2 + 0x38);

    // Keep P2 safely parked beside P1
    float offset[3];
    ParkOffset(p1, offset);
    p2Pos[0] = p1Pos[0] + offset[0];
    p2Pos[1] = p1Pos[1] + offset[1];
    p2Pos[2] = p1Pos[2] + offset[2];

    // Zero actor velocity XYZ
    p2Vel[0] = 0.0f;
    p2Vel[1] = 0.0f;
    p2Vel[2] = 0.0f;

    // Mario core position at MarioActor + 0x3c0 -> Mario + 0x140
    uintptr_t p2Mario = *reinterpret_cast<uintptr_t*>(p2 + 0x3C0);
    if (IsGuestPointer(p2Mario)) {
        float* p2CorePos = reinterpret_cast<float*>(p2Mario + 0x140);
        p2CorePos[0] = p2Pos[0];
        p2CorePos[1] = p2Pos[1];
        p2CorePos[2] = p2Pos[2];

        float* p2CoreVel = reinterpret_cast<float*>(p2Mario + 0x14C);
        p2CoreVel[0] = 0.0f;
        p2CoreVel[1] = 0.0f;
        p2CoreVel[2] = 0.0f;
    }

    if (sParkedSampleCount < kParkedSampleLimit) {
        ++sParkedSampleCount;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0019 parked P2=%p next to P1=%p pos=(%.1f,%.1f,%.1f) sample=%u/%u kill=1",
                    p2Actor, p1Actor,
                    static_cast<double>(p2Pos[0]),
                    static_cast<double>(p2Pos[1]),
                    static_cast<double>(p2Pos[2]),
                    static_cast<unsigned>(sParkedSampleCount),
                    static_cast<unsigned>(kParkedSampleLimit));
    }
}

// PATCH-0070: Actor-Local Damage Reaction (0x3429C8)
// Eliminates P1 singleton gatekeeping (MR::isPlayerDamage) so both players have independent damage handling.
constexpr std::uintptr_t kPatch0070MarioTakeDamage = 0x3429C8;
constexpr std::uintptr_t kPatch0070IsStatusActive = 0x060624;
constexpr std::uintptr_t kPatch0070MarioActorDamage = 0x25E90C;
constexpr std::uintptr_t kPatch0070MarioChangeState = 0x1DB460;
constexpr std::uintptr_t kPatch0070MarioStartAnim = 0x172928;
constexpr std::uintptr_t kPatch0070DamageAnimString = 0x64DADE; // "ダメージ"
constexpr bool kPatch0070Enabled = true;

HOOK_DEFINE_TRAMPOLINE(Patch0070MarioTakeDamage) {
    static void Callback(void* innerMario, std::uint32_t param2) {
        if (!kPatch0070Enabled || innerMario == nullptr ||
            !IsGuestPointer(reinterpret_cast<std::uintptr_t>(innerMario))) {
            Orig(innerMario, param2);
            return;
        }

        const std::uintptr_t target = exl::util::modules::GetTargetStart();
        using IsStatusActiveFn = std::uint8_t (*)(void*, int);
        const auto isStatusActive = reinterpret_cast<IsStatusActiveFn>(target + kPatch0070IsStatusActive);

        // Check actor-local damage status (0x13) instead of global P1 singleton!
        if (isStatusActive(innerMario, 0x13) != 0) {
            return;
        }

        const std::uintptr_t innerAddr = reinterpret_cast<std::uintptr_t>(innerMario);
        const std::uint8_t invuln = *reinterpret_cast<const std::uint8_t*>(innerAddr + 0x8F0);

        if ((param2 & 1) != 0 && invuln == 0) {
            void* actor = *reinterpret_cast<void**>(innerAddr + 0x8);
            if (IsValidMarioActor(actor)) {
                using DamageFn = void (*)(void*, std::uint16_t);
                using ChangeStateFn = void (*)(void*, int);
                using StartAnimFn = void (*)(void*, const char*, int);

                const auto damageActor = reinterpret_cast<DamageFn>(target + kPatch0070MarioActorDamage);
                const auto changeState = reinterpret_cast<ChangeStateFn>(target + kPatch0070MarioChangeState);
                const auto startAnim = reinterpret_cast<StartAnimFn>(target + kPatch0070MarioStartAnim);

                damageActor(actor, 0);
                changeState(innerMario, 2);
                const char* animName = reinterpret_cast<const char*>(target + kPatch0070DamageAnimString);
                startAnim(innerMario, animName, -1);
            }
        }

        *reinterpret_cast<std::uint8_t*>(innerAddr + 0x8F0) = 1;
    }
};

}  // namespace

void SyncSpinEnabled(const char* phase) {
    if (!kPatch0013SpinMirrorEnabled) return;
    void* p1 = GetFirstPlayer();
    void* p2 = GetSecondPlayer();
    if (!IsValidMarioActor(p1) || !IsValidMarioActor(p2)) return;
    const std::uint8_t enabled =
        *reinterpret_cast<const std::uint8_t*>(reinterpret_cast<std::uintptr_t>(p1) + kMarioActorSpinEnabled);
    auto* p2Enabled = reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uintptr_t>(p2) + kMarioActorSpinEnabled);
    if (*p2Enabled == enabled) return;
    if (sSpinMirrorLogs < 16) {
        ++sSpinMirrorLogs;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0013 spin mirror phase=%s p1=%u p2_old=%u sample=%u/16 kill=1",
                    phase, static_cast<unsigned>(enabled), static_cast<unsigned>(*p2Enabled),
                    static_cast<unsigned>(sSpinMirrorLogs));
    }
    *p2Enabled = enabled;
}

void NotifyStageSpawn() {
    ResetSharedCamera();
    sP2Parked = true;
    sCameraManGameSeen = false;
    sParkedSampleCount = 0;
    sInputWaitTicks = 0;
    sBoundDeferLogs = 0;
    sPrologueLogged = false;
}

bool IsP2Parked() {
    return sP2Parked;
}

void UpdateP2Parking(void* p1, void* p2) {
    if (p1 == nullptr || p2 == nullptr) return;

    const bool protectedScene = IsProtectedScene();
    if (protectedScene) {
        if (!sP2Parked) {
            sP2Parked = true;
            sCameraManGameSeen = false;
            Logging.Log("[GalaxyCoop:smg1] PATCH-0019 re-parked P2 in protected scene");
        }
        ApplyP2ParkingCarrier(p1, p2);
        return;
    }

    // Ordinary camera calculation also runs during scripted arrival. Retain
    // parking until P1's native movement gate passes and a player gives input.
    if (sCameraManGameSeen && sP2Parked) {
        float stickX = 0.0f;
        float stickY = 0.0f;
        std::uint32_t inputMask = 0;
        const bool p1Bound = kPatch0019HoldWhileP1BoundEnabled && IsValidMarioActor(p1) &&
                             HasActiveBinding(reinterpret_cast<std::uintptr_t>(p1));
        bool inputReady = !kPatch0019InputReleaseEnabled ||
                          HasNativeP1Movement(p1, &stickX, &stickY);
        if (!inputReady && kPatch0019AnyPlayerReleaseEnabled) {
            inputMask = ActivePlayerInputMask();
            inputReady = inputMask != 0 && IsNativeMovementGateOpen(p1);
        }
        if (inputReady && p1Bound) {
            inputReady = false;
            if (sBoundDeferLogs < 8) {
                ++sBoundDeferLogs;
                Logging.Log("[GalaxyCoop:smg1] PATCH-0019 release deferred p1_bound=1 players=0x%x stick=(%.3f,%.3f) waited=%u kill=1",
                            static_cast<unsigned>(inputMask), static_cast<double>(stickX),
                            static_cast<double>(stickY), static_cast<unsigned>(sInputWaitTicks));
            }
        }
        if (inputReady) {
            // Commit the existing carrier once before independent simulation.
            ApplyP2ParkingCarrier(p1, p2);
            sP2Parked = false;
            Logging.Log("[GalaxyCoop:smg1] PATCH-0019 released mode=ordinary reason=%s stick=(%.3f,%.3f) players=0x%x waited=%u P1=%p P2=%p kill=1",
                        inputMask != 0 ? "camera_native_gate_any_player" : "camera_and_native_p1_input",
                        static_cast<double>(stickX), static_cast<double>(stickY),
                        static_cast<unsigned>(inputMask),
                        static_cast<unsigned>(sInputWaitTicks), p1, p2);
        } else if (sInputWaitTicks < 900) {
            ++sInputWaitTicks;
            if (sInputWaitTicks == 1 || sInputWaitTicks == 60 ||
                sInputWaitTicks == 300 || sInputWaitTicks == 600 || sInputWaitTicks == 900) {
                Logging.Log("[GalaxyCoop:smg1] PATCH-0019 arrival held camera_seen=1 native_stick=(%.3f,%.3f) waited=%u kill=1",
                            static_cast<double>(stickX), static_cast<double>(stickY),
                            static_cast<unsigned>(sInputWaitTicks));
            }
        }
    }

    if (sP2Parked) {
        ApplyP2ParkingCarrier(p1, p2);
    }
}

void NotifyCameraSeen(std::uintptr_t cameraMan) {
    if (!sCameraManGameSeen) {
        sCameraManGameSeen = true;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0019 camera seen cameraMan=0x%lx",
                    static_cast<unsigned long>(cameraMan));
    }
}

void InstallActions(const galaxy::target::Identity& identity) {
    if (identity.game == galaxy::target::Game::Smg1) {
        Logging.Log("[GalaxyCoop:smg1] PATCH-0019 arrival input release gate enabled=%d any_player=%d hold_p1_bound=%d entry=0x%lx",
                    kPatch0019InputReleaseEnabled ? 1 : 0, kPatch0019AnyPlayerReleaseEnabled ? 1 : 0,
                    kPatch0019HoldWhileP1BoundEnabled ? 1 : 0,
                    static_cast<unsigned long>(kPatch0019GetStickValue));
        if (kPatch0070Enabled) {
            Patch0070MarioTakeDamage::InstallAtOffset(kPatch0070MarioTakeDamage);
        }
        Logging.Log("[GalaxyCoop:%s] PATCH-0019 & PATCH-0070 actions installed damage=0x%lx kill=1",
                    galaxy::target::GameName(identity.game),
                    static_cast<unsigned long>(kPatch0070MarioTakeDamage));
    }
}

}  // namespace mod::patches
