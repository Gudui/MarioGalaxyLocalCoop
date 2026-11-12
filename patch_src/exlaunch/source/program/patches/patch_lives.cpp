#include "patch_lives.hpp"
#include "patch_shared.hpp"
#include "patch_diagnostics.hpp"
#include "patch_respawn.hpp"
#include "patch_input.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <algorithm>
#include <cstdint>

namespace mod::patches {

namespace {

// Static NSO offsets verified via Ghidra headless analysis on smg1 v1.0.0
constexpr std::uintptr_t kPatch0066AddPlayerLeft = 0x237560;
constexpr std::uintptr_t kPatch0066KinokoOneUpExeGot = 0x269C08;
constexpr std::uintptr_t kPatch0066GetPlayerLeftFunc = 0x07E330;

constexpr std::uint16_t kDefaultLives = 4;
constexpr std::uint16_t kMaxLives = 99;

// Compile-time switches for death isolation and life delta suppression
// Set to true to re-enable co-op isolated death and stage reload suppression;
// set to false for clean vanilla damage baseline logging.
constexpr bool kPatch0068Enabled = false;
constexpr bool kPatch0066SuppressDeathDelta = false;

// DIAG-0172 test only: the two players start with these life counts. Applied
// at every stage spawn until the first death or life gain, so a native reset
// on save load cannot undo it. Rev 2 (EVD-0215) gives them different counts so
// the P2-out-while-P1-alive case can be reached: P2 goes out after one death
// while P1 still holds lives. Set false for release builds.
// EVD-0215 / ATT-0121 closed this test; disabled so the power-up pass starts
// with native life counts for both players. Set true to replay the death test.
constexpr bool kDiag0172TestStartLivesEnabled = false;
constexpr std::uint16_t kDiag0172TestStartLivesP1 = 3;
constexpr std::uint16_t kDiag0172TestStartLivesP2 = 1;
bool sDiag0172Armed = kDiag0172TestStartLivesEnabled;

// DIAG-0173 test only: on the P2 controller, holding Minus and pressing D-pad
// Up acts as a 1-Up Mushroom picked up by P1, D-pad Down as one picked up by
// P2. Uses the Kinoko::exeGot recipient rule and the addPlayerLeft funnel.
// Raw nn::hid NpadButton bits from 0x4CEEB4 (EVD-0209). Set false for release builds.
constexpr bool kDiag0173OneUpTriggerEnabled = true;
constexpr std::uint64_t kNpadButtonMinus = 1ull << 11;
constexpr std::uint64_t kNpadButtonUp = 1ull << 13;
constexpr std::uint64_t kNpadButtonDown = 1ull << 15;
std::uint64_t sDiag0173PreviousButtons = 0;

// Dedicated Co-Op Life Storage
std::uint16_t sP2Lives = kDefaultLives;
bool sP2Initialized = false;

// PATCH-0174: the native game over -> continue restores P1's life count without
// passing through addPlayerLeft (0x237560). Every life change we route keeps
// this baseline current, so an unexplained increase at the next stage spawn is
// exactly that native restore. P2 then gets the same fresh start and an out P2
// is cleared, instead of staying invisible forever on the reloaded save.
constexpr bool kPatch0174GameOverResetEnabled = true;
std::uint16_t sP1LivesBaseline = 0;
struct P1LivesBaselineSync {
    ~P1LivesBaselineSync();
};

// 1-Up Recipient router state: 0 = unassigned (milestone), 1 = P1 (Mario), 2 = P2 (Luigi)
int sPending1UpRecipient = 0;

// Mushroom recipient for a collector index (1 = P1, 2 = P2).
int ChooseOneUpRecipient(int collector) {
    if (kCoopLifeRulesEnabled && HasActivePlayerPair()) {
        // PATCH-0171: a partner with 0 lives receives the mushroom.
        const int partner = collector == 1 ? 2 : 1;
        const std::uint16_t partnerLives = partner == 1 ? GetP1Lives() : sP2Lives;
        if (partnerLives == 0) return partner;
    }
    return collector;
}

// 0x237560: void MR::addPlayerLeft(int delta)
// Universal funnel for all life additions (delta = +1) and death decrements (delta = -1)
HOOK_DEFINE_TRAMPOLINE(Patch0066AddPlayerLeft) {
    static void Give(int index, int delta) {
        sDiag0172Armed = false;
        if (index == 0) Orig(delta);
        else sP2Lives = std::min<std::uint16_t>(sP2Lives + delta, kMaxLives);
        NotifyCoopLifeGained(index);
    }
    static void Callback(int delta) {
        const P1LivesBaselineSync sync;
        if (delta > 0 && kCoopLifeRulesEnabled) {
            // PATCH-0171: mushroom recipient chosen at pickup; milestone gives both.
            if (sPending1UpRecipient == 1 || sPending1UpRecipient == 2) {
                Give(sPending1UpRecipient - 1, delta);
                Logging.Log("[GalaxyCoop:smg1] PATCH-0171 1-Up Mushroom -> p%d lives=(%u,%u) kill=1",
                            sPending1UpRecipient, GetP1Lives(), sP2Lives);
            } else {
                Give(0, delta);
                Give(1, delta);
                Logging.Log("[GalaxyCoop:smg1] PATCH-0171 milestone 1-up -> both lives=(%u,%u) kill=1",
                            GetP1Lives(), sP2Lives);
            }
            return;
        }
        if (delta > 0) {
            // 1-Up Mushroom collected by Luigi (P2)
            if (sPending1UpRecipient == 2) {
                sP2Lives = std::min<std::uint16_t>(sP2Lives + delta, kMaxLives);
                Logging.Log("[GalaxyCoop:smg1] PATCH-0066 [LIVES] 1-Up Mushroom awarded to P2 (Luigi)! P1: %u, P2: %u",
                            GetP1Lives(), sP2Lives);
                return;
            }

            // 1-Up Mushroom collected by Mario (P1)
            if (sPending1UpRecipient == 1) {
                Orig(delta);
                Logging.Log("[GalaxyCoop:smg1] PATCH-0066 [LIVES] 1-Up Mushroom awarded to P1 (Mario)! P1: %u, P2: %u",
                            GetP1Lives(), sP2Lives);
                return;
            }

            // Milestone 1-Up (100 coins / 50 star bits): award to whichever player has fewer lives for catch-up parity
            const std::uint16_t p1Lives = GetP1Lives();
            if (sP2Lives < p1Lives) {
                sP2Lives = std::min<std::uint16_t>(sP2Lives + delta, kMaxLives);
                Logging.Log("[GalaxyCoop:smg1] PATCH-0066 [LIVES] Milestone 1-Up awarded to P2 (Luigi, catch-up)! P1: %u, P2: %u",
                            p1Lives, sP2Lives);
                return;
            } else {
                Orig(delta);
                Logging.Log("[GalaxyCoop:smg1] PATCH-0066 [LIVES] Milestone 1-Up awarded to P1 (Mario)! P1: %u, P2: %u",
                            GetP1Lives(), sP2Lives);
                return;
            }
        }

        if (delta < 0) {
            if (SuppressChargedNativeDeath()) return;
            // In co-op mode, death decrements are managed explicitly and exclusively by TryHandleCoopFatal.
            // When kPatch0066SuppressDeathDelta is true, native calls to addPlayerLeft(-1) are ignored.
            // When false (vanilla testing), native life decrements run unmodified.
            if (kPatch0066SuppressDeathDelta && HasActivePlayerPair()) {
                Logging.Log("[GalaxyCoop:smg1] PATCH-0066 [LIVES] Suppressed native death decrement (-1) to prevent double decrement");
                return;
            }

            Orig(delta);
            return;
        }

        Orig(delta);
    }
};

// 0x269C08: void Kinoko::exeGot(void)
// Triggered on mushroom pickup animation; native sensor collision stores the colliding MarioActor* at kinokoActor + 0x190
HOOK_DEFINE_TRAMPOLINE(Patch0066KinokoOneUpExeGot) {
    static void Callback(void* kinokoActor) {
        int collectorIndex = 1;
        if (kinokoActor != nullptr && IsGuestPointer(reinterpret_cast<std::uintptr_t>(kinokoActor))) {
            void* collector = *reinterpret_cast<void**>(reinterpret_cast<std::uintptr_t>(kinokoActor) + 0x190);
            void* p2 = GetSecondPlayer();
            if (IsValidMarioActor(p2) && collector == p2) collectorIndex = 2;
        }
        sPending1UpRecipient = ChooseOneUpRecipient(collectorIndex);

        Orig(kinokoActor);

        sPending1UpRecipient = 0;
    }
};

constexpr std::uintptr_t kPatch0068MarioExeGameOver = 0x1DB92C;
constexpr std::uintptr_t kPatch0068MarioExeGameOverAbyss = 0x1F49D0;
constexpr std::uintptr_t kPatch0068GlobalDeathSequence = 0x1F4AF0;
constexpr std::uintptr_t kPatch0068AscensionCamera = 0x233E30;

bool HandleCoopFatalExecution(void* actor, const char* context) {
    void* p1 = GetFirstPlayer();
    void* p2 = GetSecondPlayer();

    if (!IsValidMarioActor(p1) || !IsValidMarioActor(p2)) {
        return false;
    }

    const bool isP1 = (actor == p1);
    const bool isP2 = (actor == p2);

    if (!isP1 && !isP2) {
        return false;
    }

    LogDiag0144Flags("fatal");

    if (IsPlayerInvulnerable(actor)) {
        SetSuppressGlobalDeathSequence(true);
        return true;
    }

    void* partner = isP1 ? p2 : p1;
    const int partnerHealth = *reinterpret_cast<const int*>(
        reinterpret_cast<std::uintptr_t>(partner) + 0x528);
    const DeathPhase partnerPhase = isP1 ? GetP2DeathPhase() : GetP1DeathPhase();
    const bool partnerAlive = (partnerHealth > 0) && (partnerPhase == DeathPhase::Alive);

    const DeathPhase thisPhase = isP1 ? GetP1DeathPhase() : GetP2DeathPhase();

    // If partner is alive, isolate death: play stumble/collapse animation and suppress stage reload!
    if (partnerAlive || (partnerHealth > 0 && thisPhase != DeathPhase::Alive)) {
        constexpr std::uint32_t kDeathAnimDurationFrames = 65; // ~1.1s: exact collapse duration before ground rest
        if (thisPhase == DeathPhase::Alive) {
            if (isP1) {
                SetP1DeathPhase(DeathPhase::Animating);
                SetP1AnimTimer(kDeathAnimDurationFrames);
                DecrementP1LivesOnce();
                Logging.Log("[GalaxyCoop:smg1] PATCH-0068 [LIVES] Isolated P1 (Mario) death in %s. Decremented P1 lives to %u (P2: %u)",
                            context, GetP1Lives(), GetP2Lives());
            } else {
                SetP2DeathPhase(DeathPhase::Animating);
                SetP2AnimTimer(kDeathAnimDurationFrames);
                DecrementP2LivesOnce();
                Logging.Log("[GalaxyCoop:smg1] PATCH-0068 [LIVES] Isolated P2 (Luigi) death in %s. Decremented P2 lives to %u (P1: %u)",
                            context, GetP2Lives(), GetP1Lives());
            }
        }

        SetSuppressGlobalDeathSequence(true);
        return true;
    }

    // Both players dead! Native stage reload
    Logging.Log("[GalaxyCoop:smg1] PATCH-0068 Both players dead in %s! Triggering native stage reload.", context);
    if (isP1 && thisPhase == DeathPhase::Alive) {
        DecrementP1LivesOnce();
    } else if (isP2 && thisPhase == DeathPhase::Alive) {
        DecrementP2LivesOnce();
    }

    SetP1DeathPhase(DeathPhase::Alive);
    SetP2DeathPhase(DeathPhase::Alive);
    SetP1AnimTimer(0);
    SetP2AnimTimer(0);
    SetP1RespawnTimer(0);
    SetP2RespawnTimer(0);

    SetSuppressGlobalDeathSequence(false);
    return false;
}

// 0x1DB92C: MarioActor::exeGameOver(MarioActor* this)
// Universal normal death executor (0 wedges, damage, hazards)
HOOK_DEFINE_TRAMPOLINE(Patch0068MarioExeGameOver) {
    static void Callback(void* actor) {
        const bool isolated = HandleCoopFatalExecution(actor, "exeGameOver");
        Orig(actor);
        if (isolated) {
            SetSuppressGlobalDeathSequence(false);
        }
    }
};

// 0x1F49D0: MarioActor::exeGameOverAbyss(MarioActor* this)
// Universal void fall / abyss death executor
HOOK_DEFINE_TRAMPOLINE(Patch0068MarioExeGameOverAbyss) {
    static void Callback(void* actor) {
        const bool isolated = HandleCoopFatalExecution(actor, "exeGameOverAbyss");
        Orig(actor);
        if (isolated) {
            SetSuppressGlobalDeathSequence(false);
        }
    }
};

// 0x1F4AF0: GameSequenceDirector::notifyPlayerDown
// Global stage wipe & reload sequence trigger
HOOK_DEFINE_TRAMPOLINE(Patch0068GlobalDeathSequence) {
    static void Callback(void* eventName) {
        if (GetSuppressGlobalDeathSequence()) {
            return; // Suppressed: surviving partner continues playing!
        }
        Orig(eventName);
    }
};

// 0x233E30: CameraDirector::startAscensionCamera
// Global death camera takeover
HOOK_DEFINE_TRAMPOLINE(Patch0068AscensionCamera) {
    static void Callback(void* cameraName, std::uint32_t mode) {
        if (GetSuppressGlobalDeathSequence()) {
            return; // Suppressed: camera stays focused on alive partner!
        }
        Orig(cameraName, mode);
    }
};

P1LivesBaselineSync::~P1LivesBaselineSync() { sP1LivesBaseline = GetP1Lives(); }

}  // namespace

void DecrementP1LivesOnce() {
    const P1LivesBaselineSync sync;
    sDiag0172Armed = false;
    Patch0066AddPlayerLeft::Orig(-1);
}

void DecrementP2LivesOnce() {
    sDiag0172Armed = false;
    sP2Lives = (sP2Lives > 0) ? sP2Lives - 1 : 0;
}

// Native direct leaf call (0x07E330) - zero hooks, zero overhead, 100% native execution
std::uint16_t GetP1Lives() {
    auto fn = reinterpret_cast<std::uint16_t(*)()>(exl::util::modules::GetTargetStart() + kPatch0066GetPlayerLeftFunc);
    return fn();
}

std::uint16_t GetP2Lives() {
    return sP2Lives;
}

void TickDiag0173OneUpTrigger() {
    if (!kDiag0173OneUpTriggerEnabled) return;
    const std::uint64_t buttons = ReadNpadRawButtons(1);
    const std::uint64_t pressed = buttons & ~sDiag0173PreviousButtons;
    sDiag0173PreviousButtons = buttons;
    if ((buttons & kNpadButtonMinus) == 0 || !HasActivePlayerPair()) return;
    const int collector = (pressed & kNpadButtonUp) != 0 ? 1 : (pressed & kNpadButtonDown) != 0 ? 2 : 0;
    if (collector == 0) return;
    // Same path as a real pickup: Kinoko recipient rule, then the addPlayerLeft funnel.
    const int recipient = ChooseOneUpRecipient(collector);
    sPending1UpRecipient = recipient;
    Patch0066AddPlayerLeft::Callback(1);
    sPending1UpRecipient = 0;
    Logging.Log("[GalaxyCoop:smg1] DIAG-0173 debug 1-Up Mushroom pickup p%d -> p%d lives=(%u,%u) kill=1",
                collector, recipient, GetP1Lives(), sP2Lives);
}

void SetP2Lives(std::uint16_t lives) {
    sP2Lives = std::min<std::uint16_t>(lives, kMaxLives);
}

void NotifyStageSpawnLives() {
    ResetBubbleRespawn();
    SetP1DeathPhase(DeathPhase::Alive);
    SetP2DeathPhase(DeathPhase::Alive);
    SetP1AnimTimer(0);
    SetP2AnimTimer(0);
    SetP1RespawnTimer(0);
    SetP2RespawnTimer(0);
    SetSuppressGlobalDeathSequence(false);

    if (sDiag0172Armed) {
        const std::uint16_t before = GetP1Lives();
        if (before != kDiag0172TestStartLivesP1)
            Patch0066AddPlayerLeft::Orig(static_cast<int>(kDiag0172TestStartLivesP1) - static_cast<int>(before));
        sP2Lives = kDiag0172TestStartLivesP2;
        sP2Initialized = true;
        // This change is ours, so it is not the native game over restore that
        // PATCH-0174 looks for. Without this the restore would fire below and
        // overwrite P2's separate count with P1's.
        sP1LivesBaseline = GetP1Lives();
        Logging.Log("[GalaxyCoop:smg1] DIAG-0172 test start lives applied p1=%u->%u p2=%u kill=1",
                    before, GetP1Lives(), sP2Lives);
    }
    const std::uint16_t p1Lives = GetP1Lives();
    if (kPatch0174GameOverResetEnabled && sP2Initialized && p1Lives > sP1LivesBaseline) {
        // Native game over -> continue, or a save load that restocked lives.
        const std::uint16_t previous = sP2Lives;
        sP2Lives = p1Lives;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0174 native life restore p1=%u->%u p2=%u->%u out_cleared=%d kill=1",
                    sP1LivesBaseline, p1Lives, previous, sP2Lives, previous == 0 ? 1 : 0);
    }
    if (!sP2Initialized) {
        sP2Lives = (p1Lives > 0) ? p1Lives : kDefaultLives;
        sP2Initialized = true;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0066 [LIVES] Initialized stage lives: P1=%u, P2=%u",
                    p1Lives, sP2Lives);
    } else {
        Logging.Log("[GalaxyCoop:smg1] PATCH-0066 [LIVES] Preserved stage lives: P1=%u, P2=%u",
                    p1Lives, sP2Lives);
    }
    if (kCoopLifeRulesEnabled && kBubbleRespawnEnabled && sP2Lives == 0) {
        // PATCH-0171: an out P2 stays out across checkpoint reloads and stages.
        SetP2DeathPhase(DeathPhase::Ghost);
        Logging.Log("[GalaxyCoop:smg1] PATCH-0171 p2 spawned out (0 lives) kill=1");
    }

    sP1LivesBaseline = GetP1Lives();
    LogDiag0144Flags("stage-spawn");
}

void InstallLives(const galaxy::target::Identity& identity) {
    if (identity.game == galaxy::target::Game::Smg1) {
        Patch0066AddPlayerLeft::InstallAtOffset(kPatch0066AddPlayerLeft);
        Patch0066KinokoOneUpExeGot::InstallAtOffset(kPatch0066KinokoOneUpExeGot);

        if (kPatch0068Enabled) {
            Patch0068MarioExeGameOver::InstallAtOffset(kPatch0068MarioExeGameOver);
            Patch0068MarioExeGameOverAbyss::InstallAtOffset(kPatch0068MarioExeGameOverAbyss);
            Patch0068GlobalDeathSequence::InstallAtOffset(kPatch0068GlobalDeathSequence);
            Patch0068AscensionCamera::InstallAtOffset(kPatch0068AscensionCamera);
        }

        Logging.Log("[GalaxyCoop:%s] PATCH-0066/0068 Death isolation, animation & life router installed (kPatch0068Enabled=%d)",
                    galaxy::target::GameName(identity.game), kPatch0068Enabled ? 1 : 0);
        Logging.Log("[GalaxyCoop:%s] PATCH-0171 coop life rules installed p2_out=1 mushroom_to_out=1 milestone_both=1 revive=%u p1_out=0 kill=%d",
                    galaxy::target::GameName(identity.game), 60u, kCoopLifeRulesEnabled ? 1 : 0);
        Logging.Log("[GalaxyCoop:%s] PATCH-0174 game over life restore installed both_reset=1 kill=%d",
                    galaxy::target::GameName(identity.game), kPatch0174GameOverResetEnabled ? 1 : 0);
    }
}

}  // namespace mod::patches
