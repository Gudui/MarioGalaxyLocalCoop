#include "patch_spawning.hpp"
#include "patch_shared.hpp"
#include "patch_actions.hpp"
#include "patch_camera.hpp"
#include "patch_lives.hpp"
#include "patch_powerup.hpp"
#include "patch_warp.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"

namespace mod::patches {

namespace {

constexpr std::uintptr_t kPatch0004StageSpawner = 0xD5944;
constexpr std::uintptr_t kPatch0004MarioSpawn = 0xDB248;
constexpr std::uintptr_t kPatch0004MarioFactory = 0x255DB0;
constexpr std::uintptr_t kPatch0004MarioHolderGet = 0x4340;
constexpr std::uintptr_t kPatch0004MarioHolderActorField = 0x18;
constexpr std::uintptr_t kPatch0004MarioPositionX = 0x14;

constexpr std::uintptr_t kPatch0008MarioVisualInit = 0x1BDC40;
constexpr std::uintptr_t kPatch0008GlobalCharacterBytePtr = 0x913E40;

// Temporary diagnostic for the AstroDome memory-burst bisect (EVD-0154):
// true skips the second-Mario factory spawn while keeping every other hook,
// read, and log identical, isolating our reads versus P2 presence.
// REVERTED to false after the solo-hub verdict (EVD-0155); co-op restored.
// Kill switch for the test.
constexpr bool kDiagNoP2Spawn = false;

// Burst fix candidate (EVD-0153/0155): never call the Mario factory
// reentrantly from inside the 0xD5944 spawn coordinator. Arm here, spawn on
// the first owned per-frame tick after native setup completes, with the
// holder generation re-validated before firing.
// REVERTED to false by EVD-0156: firing from a menu-phase per-frame tick
// crashes before the main menu (holder churn under a half-built scene).
// Immediate in-callback spawn restored; reentrancy stays open.
constexpr bool kDeferP2Spawn = false;
std::uintptr_t sP2PendingScene = 0;
void* sP2PendingP1 = nullptr;

bool sPatch0004Armed = false;

using Patch0004MarioSpawnFn = void (*)(std::uintptr_t scene);
using Patch0004MarioHolderGetFn = std::uintptr_t (*)();

std::uintptr_t* HolderActorField() {
    const std::uintptr_t target = exl::util::modules::GetTargetStart();
    const auto holderGet = reinterpret_cast<Patch0004MarioHolderGetFn>(
        target + kPatch0004MarioHolderGet);
    const std::uintptr_t holder = holderGet();
    if (!IsGuestPointer(holder)) return nullptr;
    return reinterpret_cast<std::uintptr_t*>(holder + kPatch0004MarioHolderActorField);
}

HOOK_DEFINE_TRAMPOLINE(Patch0004FactoryCapture) {
    static void* Callback(std::uintptr_t param) {
        void* actor = Orig(param);
        if (sPatch0004Armed && GetSecondPlayer() == nullptr) {
            SetCapturedPlayers(GetFirstPlayer(), actor);
            Logging.Log("[GalaxyCoop:smg1] PATCH-0004 second MarioActor factory returned %p", actor);
        }
        return actor;
    }
};

HOOK_DEFINE_TRAMPOLINE(Patch0008MarioVisualInit) {
    static void Callback(std::uintptr_t actor) {
        void* p2 = GetSecondPlayer();
        const std::uintptr_t target = exl::util::modules::GetTargetStart();
        uint8_t** charBytePtrPtr = reinterpret_cast<uint8_t**>(target + kPatch0008GlobalCharacterBytePtr);
        uint8_t* charBytePtr = (charBytePtrPtr != nullptr) ? *charBytePtrPtr : nullptr;
        uint8_t oldVal = 0;

        const bool isP2 = (actor == reinterpret_cast<std::uintptr_t>(p2));
        if (isP2 && charBytePtr != nullptr) {
            oldVal = *charBytePtr;
            *charBytePtr = 1;
            Logging.Log("[GalaxyCoop:smg1] PATCH-0008 set Luigi mode for P2 actor=%p", reinterpret_cast<void*>(actor));
        }

        SetP2VisualInitActive(isP2);
        Orig(actor);
        SetP2VisualInitActive(false);

        if (isP2 && charBytePtr != nullptr) {
            *charBytePtr = oldVal;
        }
    }
};

HOOK_DEFINE_TRAMPOLINE(Patch0004StageSpawn) {
    static void Callback(std::uintptr_t scene) {
        Orig(scene);

        std::uintptr_t* const actorField = HolderActorField();
        if (actorField == nullptr) {
            Logging.Log("[GalaxyCoop:smg1] PATCH-0004 skipped: MarioHolder null/invalid");
            return;
        }

        const std::uintptr_t p1 = *actorField;
        if (!IsValidMarioActor(reinterpret_cast<void*>(p1))) {
            Logging.Log("[GalaxyCoop:smg1] PATCH-0004 skipped: P1=%p invalid", reinterpret_cast<void*>(p1));
            return;
        }

        SetCapturedPlayers(reinterpret_cast<void*>(p1), nullptr);

        if (kDiagNoP2Spawn) {
            NotifyStageSpawn();
            NotifyStageSpawnLives();
            NotifyStageSpawnWarp(scene);
            Logging.Log("[GalaxyCoop:smg1] DIAG-0154 P2 spawn skipped (burst bisect) P1=%p kill=1",
                        reinterpret_cast<void*>(p1));
            return;
        }

        if (kDeferP2Spawn) {
            if (!IsGuestPointer(scene)) {
                Logging.Log("[GalaxyCoop:smg1] PATCH-0004 deferred arm skipped: scene invalid");
                return;
            }
            sP2PendingScene = scene;
            sP2PendingP1 = reinterpret_cast<void*>(p1);
            NotifyStageSpawn();
            NotifyStageSpawnLives();
            NotifyStageSpawnWarp(scene);
            Logging.Log("[GalaxyCoop:smg1] PATCH-0004 P2 deferred armed P1=%p scene=%p kill=1",
                        reinterpret_cast<void*>(p1),
                        reinterpret_cast<void*>(scene));
            return;
        }

        sPatch0004Armed = true;
        const std::uintptr_t target = exl::util::modules::GetTargetStart();
        const auto spawn = reinterpret_cast<Patch0004MarioSpawnFn>(
            target + kPatch0004MarioSpawn);
        spawn(scene);
        sPatch0004Armed = false;

        const std::uintptr_t p2 = reinterpret_cast<std::uintptr_t>(GetSecondPlayer());
        const std::uintptr_t overwritten = *actorField;
        *actorField = p1;

        // PATCH-0164 never installs a recovery camera target at spawn.
        // Shared framing is scoped to ordinary calc after the arrival gate.

        if (!IsValidMarioActor(reinterpret_cast<void*>(p2))) {
            Logging.Log("[GalaxyCoop:smg1] PATCH-0004 failed: P2=%p overwritten=%p singleton_restored=%p",
                        reinterpret_cast<void*>(p2),
                        reinterpret_cast<void*>(overwritten),
                        reinterpret_cast<void*>(*actorField));
            return;
        }

        *reinterpret_cast<float*>(p2 + kPatch0004MarioPositionX) += 150.0f;

        // FIELD-SMG1-MARIO-ENABLE-NORMAL-DAMAGE (+0x8F1): Enable normal damage on P2 inner Mario.
        // Native 0x2B5750 only initializes P1 via singleton. Without this, P2 bypasses 0x3429C8 and fatal collapses.
        const std::uintptr_t p2Inner = *reinterpret_cast<const std::uintptr_t*>(p2 + 0x3C0);
        if (IsGuestPointer(p2Inner)) {
            *reinterpret_cast<uint8_t*>(p2Inner + 0x8F1) = 1;
        }

        NotifyStageSpawn();
        NotifyStageSpawnLives();
        NotifyStageSpawnWarp(scene);

        Logging.Log("[GalaxyCoop:smg1] PATCH-0004 spawned P1=%p P2=%p overwritten=%p restored=%p observable=two_visible_marios",
                    reinterpret_cast<void*>(p1),
                    reinterpret_cast<void*>(p2),
                    reinterpret_cast<void*>(overwritten),
                    reinterpret_cast<void*>(*actorField));
    }
};

}  // namespace

void PumpDeferredP2Spawn() {
    if (!kDeferP2Spawn || kDiagNoP2Spawn) return;
    if (sP2PendingScene == 0) return;
    if (!IsGuestPointer(sP2PendingScene)) {
        sP2PendingScene = 0;
        sP2PendingP1 = nullptr;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0004 deferred dropped: stale scene kill=1");
        return;
    }

    void* p1 = GetFirstPlayer();
    if (!IsValidMarioActor(p1) || p1 != sP2PendingP1) {
        sP2PendingScene = 0;
        sP2PendingP1 = nullptr;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0004 deferred dropped: P1 generation changed kill=1");
        return;
    }

    std::uintptr_t* const actorField = HolderActorField();
    if (actorField == nullptr || *actorField != reinterpret_cast<std::uintptr_t>(p1)) {
        sP2PendingScene = 0;
        sP2PendingP1 = nullptr;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0004 deferred dropped: holder moved on kill=1");
        return;
    }

    const std::uintptr_t target = exl::util::modules::GetTargetStart();
    sPatch0004Armed = true;
    const auto spawn = reinterpret_cast<Patch0004MarioSpawnFn>(
        target + kPatch0004MarioSpawn);
    spawn(sP2PendingScene);
    sPatch0004Armed = false;

    const std::uintptr_t scene = sP2PendingScene;
    sP2PendingScene = 0;
    sP2PendingP1 = nullptr;

    const std::uintptr_t p2 = reinterpret_cast<std::uintptr_t>(GetSecondPlayer());
    const std::uintptr_t overwritten = *actorField;
    *actorField = reinterpret_cast<std::uintptr_t>(p1);

    if (!IsValidMarioActor(reinterpret_cast<void*>(p2))) {
        Logging.Log("[GalaxyCoop:smg1] PATCH-0004 deferred failed: P2=%p restored=%p",
                    reinterpret_cast<void*>(p2),
                    reinterpret_cast<void*>(*actorField));
        return;
    }

    *reinterpret_cast<float*>(p2 + kPatch0004MarioPositionX) += 150.0f;

    const std::uintptr_t p2Inner = *reinterpret_cast<const std::uintptr_t*>(p2 + 0x3C0);
    if (IsGuestPointer(p2Inner)) {
        *reinterpret_cast<uint8_t*>(p2Inner + 0x8F1) = 1;
    }
    Logging.Log("[GalaxyCoop:smg1] PATCH-0004 deferred spawned P1=%p P2=%p overwritten=%p restored=%p scene=%p observable=two_visible_marios",
                p1,
                reinterpret_cast<void*>(p2),
                reinterpret_cast<void*>(overwritten),
                reinterpret_cast<void*>(*actorField),
                reinterpret_cast<void*>(scene));
}

void InstallSpawning(const galaxy::target::Identity& identity) {
    if (identity.game == galaxy::target::Game::Smg1) {
        Patch0004FactoryCapture::InstallAtOffset(kPatch0004MarioFactory);
        Patch0008MarioVisualInit::InstallAtOffset(kPatch0008MarioVisualInit);
        Patch0004StageSpawn::InstallAtOffset(kPatch0004StageSpawner);
        Logging.Log("[GalaxyCoop:%s] PATCH-0004/0008 spawning installed spawner=0x%lx factory=0x%lx visual=0x%lx kill=1",
                    galaxy::target::GameName(identity.game),
                    static_cast<unsigned long>(kPatch0004StageSpawner),
                    static_cast<unsigned long>(kPatch0004MarioFactory),
                    static_cast<unsigned long>(kPatch0008MarioVisualInit));
    }
}

}  // namespace mod::patches
