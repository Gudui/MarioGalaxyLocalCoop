#include "patch_warp.hpp"
#include "patch_shared.hpp"
#include "patch_respawn.hpp"
#include "patch_actions.hpp"
#include "patch_input.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <algorithm>
#include <cmath>

namespace mod::patches {

namespace {

constexpr std::uintptr_t kPatch0016RecoveryProducer = 0x2649B0;
constexpr std::uintptr_t kPatch0016WarpBasisQuery = 0x2655A0;
constexpr std::uintptr_t kPatch0017MarioWarpUpdate = 0x264EB0;
constexpr std::uintptr_t kPatch0017MarioWarpClose = 0x2651B0;

// PATCH-0161 / EVD-0189, SMG1 main SHA-256
// 76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// All locations are NSO offsets, independently established in Ghidra:
// EarthenPipe vtable +0x20 = LiveActor::movement, +0xE0 = receiveOtherMsg.
// receiveOtherMsg stores sender sensor+0x30 at pipe+0x140 on AutoRushBegin.
// Its entry check uses global player position (0x4300); Ready/In/Out nerves
// use global animation helpers resolving MarioHolder+0x18 (0x4340/0x432C).
// Scope that selector to the actual participant for native callbacks only.
// Both hook openings contain only stack saves and register moves (first16B).
constexpr bool kPatch0161PipeRoutingEnabled = true;
constexpr std::uintptr_t kPipeVtable = 0x84DE40;
constexpr std::uintptr_t kPipeMovement = 0x031E58;
constexpr std::uintptr_t kPipeReceiveOtherMsg = 0x2900C4;
constexpr std::uintptr_t kPipeBoundActorField = 0x140;
constexpr std::uintptr_t kPipeSensorHostField = 0x30;
constexpr std::uintptr_t kPipeMarioHolderGet = 0x4340;
constexpr std::uintptr_t kPipeHolderActorField = 0x18;
constexpr std::uint32_t kPipeAutoRushBegin = 0x92;
constexpr std::uint32_t kPipeUpdateBaseMtx = 0xA1;
constexpr std::uint32_t kPipeLogLimit = 16;
std::uint32_t sPipeBeginLogs = 0;
std::uint32_t sPipeExitLogs = 0;

bool IsEarthenPipe(std::uintptr_t actor) {
    return IsGuestPointer(actor) &&
        *reinterpret_cast<const std::uintptr_t*>(actor) ==
            exl::util::modules::GetTargetStart() + kPipeVtable;
}

// PATCH-0186 / EVD-0245, same SMG1 main hash, NSO offsets (Ghidra):
// factory "SuperSpinDriver"/"...Green"/"...Pink" creators 0x294640/680/6C0
// all construct via 0x2942C8 -> vtable address point 0x85F160 (Launch Star).
// +0x20 is the shared LiveActor::movement 0x31E58, +0xE0 receiveOtherMsg
// 0x1CBAF4 (0x92 binds sender host at +0xD0, 0xA1 sets the +0xD0 actor's
// base matrix from star state, 0x93/0x94 clear +0xD0). Nerve animation
// uses thunk 0x1A8FB0 -> 0x1A8FC0; both releases (launch 0x1B9C50, capture
// timeout 0x190090) reach the global endRush 0x19016C. So the proven pipe
// pairing applies with the bound field at +0xD0 instead of +0x140.
constexpr bool kPatch0186LaunchStarPairEnabled = true;
constexpr std::uintptr_t kLaunchStarVtable = 0x85F160;
constexpr std::uintptr_t kLaunchStarReceiveOtherMsg = 0x1CBAF4;
constexpr std::uintptr_t kLaunchStarBoundActorField = 0xD0;
unsigned sLaunchStarLogs = 0;
// ATT-0215 / EVD-0522, same SMG1 main hash, NSO offsets (Ghidra): the Sling
// Star (SpinDriver; creator 0x26A3A0, ctor 0x26A3D4, vtable address point
// 0x8493A0) shares movement 0x31E58 and has receiveOtherMsg 0x24F9C0 with the
// Launch Star protocol: 0x92 binds sender+0x30 at +0xD0, 0x93/0x94 clear it,
// 0xA1 sets the +0xD0 actor's base matrix (0x37040). Its launch 0x1E6BC4 calls
// 0x1B9C50 -> endRush 0x19016C inside the star's movement, which
// Patch0167PipeRelease already duplicates for the follower. Opening
// sub/stp/str/add: no PC-relative form.
constexpr bool kAtt0215SlingStarPairEnabled = true;
constexpr std::uintptr_t kSlingStarVtable = 0x8493A0;
constexpr std::uintptr_t kSlingStarReceiveOtherMsg = 0x24F9C0;
// ATT-0215 / EVD-0522, EVD-0523: bunnies and NPCs ask for "the player" through
// MarioHolder+0x18 (0x4340/0x432C; position 0x4300 = actor+0x14). During such
// an actor's movement (shared 0x31E58) that slot points at whichever player is
// nearer to it, so bunnies flee from Luigi too and Luigi can start a talk.
// Vtables: RunawayRabbitCollect 0x83CA60 (Gateway bunnies, string
// "RunawayRabbit" in its init 0x354698), DemoRabbit 0x83F3A8, TrickRabbit
// 0x83BDA8 (GOT 0x913B98), TrickRabbitFreeRun 0x83C2D8, TrickRabbitFreeRunCollect
// 0x83C4D0, Rosetta 0x83FF20 (Rosalina; its control 0x141150 is used by no
// other vtable).
// ATT-0222 / EVD-0533: FlipPanel and FlipPanelReverse (creator 0x247650, ctor
// 0x247684, vtable 0x855B70; Gateway's inner-planet "floor buttons") ask
// whether the player stands on them (0x12B0EC), is grounded (0x7E074) and
// where (0x70E7C) only through 0x4340/0x432C.
constexpr bool kAtt0215NearerPlayerEnabled = true;
constexpr std::uintptr_t kNearerPlayerVtables[] = {0x83CA60, 0x83F3A8, 0x83BDA8, 0x83C2D8, 0x83C4D0, 0x83FF20,
                                                   0x855B70};
unsigned sNearerLogs = 0;
// ATT-0223 / EVD-0534: 136-248 of the 478 named classes read the player
// through MarioHolder+0x18 and 230 of the 248 move through 0x31E58, so the
// swap applies to every actor there, not only the list. Rush hosts (pipes,
// Launch and Sling Stars) take their own branch before it. Excluded:
// MarioActor itself, PowerStar/GrandStar 0x841570 (creator 0x257C20; DIAG-0188
// rush host) and the dome star 0x8572C8 (PATCH-0189). An actor changes player
// only when the other one is 15% nearer, remembered per actor in a
// direct-mapped table cleared at each stage spawn.
constexpr bool kAtt0223GlobalNearerEnabled = true;
constexpr std::uintptr_t kNearerExcludedVtables[] = {kMarioActorVtableSmg1, 0x841570, 0x8572C8};
constexpr float kNearerTakeOver2 = 0.85f * 0.85f;
constexpr unsigned kNearerClassLogLimit = 128;
struct NearerChoice { std::uintptr_t actor; bool p2; };
NearerChoice sNearerChoices[1024]{};
// ATT-0219 (EVD-0527): a line each time Luigi becomes or stops being a listed
// actor's player, per actor, with why it stopped. A state that changes back
// within kNearerQuietCalls movement calls of the last line is only counted.
constexpr bool kAtt0219NearerSwapLogEnabled = true;
constexpr unsigned kNearerQuietCalls = 30, kNearerSwapLogLimit = 1500;
struct NearerTrack { std::uintptr_t actor, vtable; bool on, logged; unsigned calls, flips; };
NearerTrack sNearerTrack[32]{};
unsigned sNearerTrackNext = 0, sNearerSwapLogs = 0;
// ATT-0216 / EVD-0524: the native player-demo lock is applied through the
// singleton: demo start 0x11F804 sets +0x568, +0x15D0, +0x12A8; lock 0x25A704
// sets +0x568; demo end 0x1C4DB8 clears them. A scene that began inside a
// bunny/Rosalina movement while the singleton was Luigi locked him for good.
// Lock calls always target Mario; no nearer-player swap while either player
// is locked; a demo end also ends a stale Luigi demo (+0x15D0) natively.
// Openings: 0x11F804 sub/stp/stp/add, 0x25A704 stp/mov, 0x1C4DB8 stp/str/mov.
constexpr bool kAtt0216DemoLockOnMarioEnabled = true;
constexpr std::uintptr_t kPlayerDemoStart = 0x11F804;
constexpr std::uintptr_t kPlayerInputLock = 0x25A704;
constexpr std::uintptr_t kPlayerDemoEnd = 0x1C4DB8;
constexpr std::uintptr_t kPlayerLockFlag = 0x568;
constexpr std::uintptr_t kPlayerDemoMode = 0x15D0;
unsigned sDemoLockLogs = 0;
// DIAG-0187 (test only): trace every Launch Star message so a star that never
// captures can be told apart from one that captures but never shoots.
constexpr bool kDiag0187LaunchStarTraceEnabled = true;
constexpr unsigned kDiag0187Limit = 24;
unsigned sDiag0187Logs = 0;
// EVD-0248: 0x2B/0x87 arrive every frame a player touches the star and burned
// the whole budget on one encounter. Only the binding protocol is traced.
constexpr bool Diag0187Interesting(std::uint32_t msg) {
    return msg==0x92 || msg==0x93 || msg==0x94 || msg==0x98 || msg==0x99;
}
// DIAG-0188 (test only): beginRush is the single funnel every rush binding
// passes through (it copies the candidate sensor at +0xCE0 into +0xF20; its
// only callers are 0x57E54 star-to-star transfer, 0x11B394 and 0x137914
// walk-in). Logging the host class here names whatever launches a player out
// of an observatory dome, which no per-class receiveOtherMsg hook can see.
constexpr bool kDiag0188BeginRushTraceEnabled = true;
constexpr unsigned kDiag0188Limit = 24;
unsigned sDiag0188Logs = 0;
// PATCH-0189 / EVD-0251, same SMG1 main hash, NSO offsets (Ghidra):
// DIAG-0188 named the observatory departure host. NameObjFactory entry at
// 0x80BD70 is {"AstroDomeBlueStar", creator 0x251380, archive "GCaptureTarget"};
// the creator allocates 0x158 bytes and runs ctor 0x2513B4, whose only vtable
// store is address point 0x8572C8 (slot +0xE0 receiveOtherMsg = 0x25CDD8). It
// is the blue star inside a dome that departs for the selected galaxy, and it
// binds only the player who touched it (EVD-0250). Its bound-player field is
// not proven yet, so it is deliberately NOT routed through the PATCH-0167
// pairing; the partner rides the proven co-op bubble for the departure demo
// instead, which is also what the user asked to see during the dialogue.
constexpr bool kPatch0189DomeStarCompanionEnabled = true;
constexpr std::uintptr_t kAstroDomeBlueStarVtable = 0x8572C8;
unsigned sDomeCompanionLogs = 0;

// DIAG-0190 (test only) / EVD-0252: the departure stalled with P1 held roughly
// 500 units above P2 and no beginRush at all, so the star never bound anyone -
// the stall is upstream of the funnel DIAG-0188 watches. 0x25CDD8 opens
// stp/str/stp/mov with no PC-relative form, so it takes a trampoline. Its ABI
// matches the Launch Star handler (this, msg, sender sensor, receiver sensor)
// and the accept branch reads the owner actor at sender+0x30. Trace what the
// star answers: nothing at all means the approach never reaches it, a refused
// 0x92 names a gate inside the star.
constexpr bool kDiag0190DomeStarTraceEnabled = true;
constexpr std::uintptr_t kAstroDomeBlueStarReceiveOtherMsg = 0x25CDD8;
constexpr unsigned kDiag0190Limit = 40;
unsigned sDiag0190Logs = 0;
std::uint32_t sDiag0190SeenMsg[16] = {};
unsigned sDiag0190SeenCount = 0;
// Every message the star ever answers is worth one line; the binding protocol
// is worth every occurrence. Per-frame touch traffic must not eat the budget.
bool Diag0190FirstOfKind(std::uint32_t msg) {
    for (unsigned i = 0; i < sDiag0190SeenCount; ++i)
        if (sDiag0190SeenMsg[i] == msg) return false;
    if (sDiag0190SeenCount < 16) sDiag0190SeenMsg[sDiag0190SeenCount++] = msg;
    return true;
}

bool IsLaunchStar(std::uintptr_t actor) {
    return kPatch0186LaunchStarPairEnabled && IsGuestPointer(actor) &&
        *reinterpret_cast<const std::uintptr_t*>(actor) ==
            exl::util::modules::GetTargetStart() + kLaunchStarVtable;
}
bool IsSlingStar(std::uintptr_t actor) {
    return kPatch0186LaunchStarPairEnabled && kAtt0215SlingStarPairEnabled && IsGuestPointer(actor) &&
        *reinterpret_cast<const std::uintptr_t*>(actor) ==
            exl::util::modules::GetTargetStart() + kSlingStarVtable;
}
// Launch Star or Sling Star: same rush protocol, bound actor at +0xD0.
bool IsStarHost(std::uintptr_t actor) { return IsLaunchStar(actor) || IsSlingStar(actor); }
bool IsAstroDomeBlueStar(std::uintptr_t actor) {
    return kPatch0189DomeStarCompanionEnabled && IsGuestPointer(actor) &&
        *reinterpret_cast<const std::uintptr_t*>(actor) ==
            exl::util::modules::GetTargetStart() + kAstroDomeBlueStarVtable;
}
bool IsCoopRushHost(std::uintptr_t actor) {
    return IsEarthenPipe(actor) || IsStarHost(actor);
}
std::uintptr_t* RushHostBoundSlot(std::uintptr_t host) {
    return reinterpret_cast<std::uintptr_t*>(
        host + (IsStarHost(host) ? kLaunchStarBoundActorField : kPipeBoundActorField));
}
const char* RushHostLabel(std::uintptr_t host) {
    return IsSlingStar(host) ? "ATT-0215 sling star" : IsLaunchStar(host) ? "PATCH-0186 launch star" : "PATCH-0167 pipe";
}

bool IsCapturedPipePlayer(std::uintptr_t actor) {
    return actor != 0 &&
        (actor == reinterpret_cast<std::uintptr_t>(GetFirstPlayer()) ||
         actor == reinterpret_cast<std::uintptr_t>(GetSecondPlayer())) &&
        IsValidMarioActor(reinterpret_cast<void*>(actor));
}

class PipePlayerScope {
    std::uintptr_t* mSlot = nullptr;
    std::uintptr_t mPrevious = 0;
public:
    explicit PipePlayerScope(std::uintptr_t actor) {
        if (!IsCapturedPipePlayer(actor)) return;
        const auto getHolder = reinterpret_cast<std::uintptr_t (*)()>(
            exl::util::modules::GetTargetStart() + kPipeMarioHolderGet);
        const std::uintptr_t holder = getHolder();
        if (!IsGuestPointer(holder)) return;
        auto* slot = reinterpret_cast<std::uintptr_t*>(holder + kPipeHolderActorField);
        if (!IsCapturedPipePlayer(*slot)) return;
        mSlot = slot;
        mPrevious = *slot;
        *mSlot = actor;
    }
    ~PipePlayerScope() {
        // Stack lifetime only, including nested callbacks; never cache scene
        // pointers across frames or leave the singleton assigned to P2.
        if (mSlot != nullptr) *mSlot = mPrevious;
    }
    PipePlayerScope(const PipePlayerScope&) = delete;
    PipePlayerScope& operator=(const PipePlayerScope&) = delete;
};

// PATCH-0167 / EVD-0198, same SMG1 main hash as above, NSO offsets.
// Native beginRush consumes actor+CE0 and writes F20/F38; native endRush
// clears that binding and restores playable motion. One pipe nerve remains
// authoritative. Duplicate only its actor animation/visibility/release calls.
constexpr std::uintptr_t kPipeBeginRush = 0x11BEB4;
constexpr std::uintptr_t kPipeSetBaseMatrix = 0x037040;
constexpr std::uintptr_t kPipeStartAnimation = 0x1A8FC0; // real framed entry, NOT branch thunk1A8FB0
constexpr std::uintptr_t kPipeJumpAnimation = 0x1D4E08;
constexpr std::uintptr_t kPipeVisibility = 0x20D1D0;
constexpr std::uintptr_t kPipeEndRush = 0x19016C;
constexpr std::uintptr_t kPipeEndRushActor = 0x1901C4;
constexpr std::uintptr_t kPipeEndDescriptor = 0x190194;
constexpr std::uintptr_t kPlayerRushSensor = 0xF20;
constexpr std::uintptr_t kPlayerRushCandidate = 0xCE0;
// Strict synchronous scope; restore before returning from native movement.
std::uintptr_t sActivePipe = 0;
unsigned sPairLogs=0;
template<typename T> T PipeFn(std::uintptr_t offset) {
    return reinterpret_cast<T>(exl::util::modules::GetTargetStart()+offset);
}
bool BoundToPipe(std::uintptr_t actor, std::uintptr_t pipe) {
    if (!IsCapturedPipePlayer(actor) || !IsCoopRushHost(pipe)) return false;
    const auto sensor=*reinterpret_cast<const std::uintptr_t*>(actor+kPlayerRushSensor);
    return IsGuestPointer(sensor) &&
        *reinterpret_cast<const std::uintptr_t*>(sensor+kPipeSensorHostField)==pipe;
}
// A recorded co-op travel leader that holds no rush sensor is stale: its
// release path was missed, and every later 0x92 from the other player would be
// refused forever. Drop it before gating a fresh request.
unsigned sStaleLeaderLogs=0;
void DropStaleRushLeader() {
    void* leader=GetCoopPipeLeader();
    if (!leader || !IsValidMarioActor(leader)) return;
    if (*reinterpret_cast<const std::uintptr_t*>(
            reinterpret_cast<std::uintptr_t>(leader)+kPlayerRushSensor)!=0) return;
    FinishCoopPipeTravel(leader);
    if (sStaleLeaderLogs<8) {
        ++sStaleLeaderLogs;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0186 dropped stale rush leader p%d kill=1",
            leader==GetSecondPlayer()?2:1);
    }
}
std::uintptr_t PairedFollower(std::uintptr_t pipe) {
    if (!kPipeCompanionEnabled || !HasActivePlayerPair() || !IsCoopRushHost(pipe)) return 0;
    const auto leader=*RushHostBoundSlot(pipe);
    if (!IsCapturedPipePlayer(leader) || reinterpret_cast<void*>(leader)!=GetCoopPipeLeader()) return 0;
    const auto other=reinterpret_cast<std::uintptr_t>(
        reinterpret_cast<void*>(leader)==GetFirstPlayer() ? GetSecondPlayer() : GetFirstPlayer());
    return BoundToPipe(other,pipe) ? other : 0;
}
unsigned sJoinDeclineLogs=0;
void JoinDecline(std::uintptr_t host, const char* reason) {
    if (!kDiag0187LaunchStarTraceEnabled || !IsStarHost(host) || sJoinDeclineLogs>=12) return;
    ++sJoinDeclineLogs;
    Logging.Log("[GalaxyCoop:smg1] DIAG-0187 star join declined star=%p reason=%s kill=0",
        reinterpret_cast<void*>(host), reason);
}
void JoinNativePipe(std::uintptr_t pipe, std::uintptr_t leader) {
    if (!kPipeCompanionEnabled || IsP2Parked()) return;
    if (reinterpret_cast<void*>(leader)!=GetCoopPipeLeader()) {
        if (IsCapturedPipePlayer(leader)) JoinDecline(pipe,"not_coop_leader");
        return;
    }
    if (!BoundToPipe(leader,pipe)) { JoinDecline(pipe,"leader_not_bound"); return; }
    if (PairedFollower(pipe)) return;
    const bool leaderP1=reinterpret_cast<void*>(leader)==GetFirstPlayer();
    const auto other=reinterpret_cast<std::uintptr_t>(leaderP1 ? GetSecondPlayer() : GetFirstPlayer());
    if (!IsCapturedPipePlayer(other) || (leaderP1 ? GetP2DeathPhase() : GetP1DeathPhase())!=DeathPhase::Alive ||
        *reinterpret_cast<const int*>(other+0x528)<=0 ||
        HasActiveBinding(other)) {  // PATCH-0180: ignore stale enemy entries
        JoinDecline(pipe,"follower_unavailable");
        return;
    }
    const auto core=*reinterpret_cast<const std::uintptr_t*>(other+0x3C0);
    if (!IsGuestPointer(core) || *reinterpret_cast<const std::uintptr_t*>(core+8)!=other) {
        JoinDecline(pipe,"follower_core");
        return;
    }
    const auto sensor=*reinterpret_cast<const std::uintptr_t*>(leader+kPlayerRushSensor);
    auto* candidate=reinterpret_cast<std::uintptr_t*>(other+kPlayerRushCandidate);
    const auto previous=*candidate;
    *candidate=sensor;
    {
        PipePlayerScope scope(other);
        PipeFn<void (*)(std::uintptr_t)>(kPipeBeginRush)(other);
    }
    *candidate=previous; // temporary producer input only; native owns F20/F38
    if (sPairLogs<16) {
        ++sPairLogs;
        Logging.Log("[GalaxyCoop:smg1] %s paired leader=p%d follower=p%d bound=%d hp=(%d,%d) kill=1",
            RushHostLabel(pipe), leaderP1?1:2,leaderP1?2:1,BoundToPipe(other,pipe)?1:0,
            *reinterpret_cast<const int*>(reinterpret_cast<std::uintptr_t>(GetFirstPlayer())+0x528),
            *reinterpret_cast<const int*>(reinterpret_cast<std::uintptr_t>(GetSecondPlayer())+0x528));
    }
}
void CancelRushFollower(std::uintptr_t pipe, std::uintptr_t follower) {
    if (!follower || !BoundToPipe(follower,pipe)) return;
    alignas(8) unsigned char descriptor[0x30]{};
    const float zero[3]={};
    PipeFn<void (*)(void*,std::uintptr_t,int,const float*,bool,int)>(kPipeEndDescriptor)(descriptor,pipe,4,zero,false,0);
    PipePlayerScope scope(follower);
    PipeFn<void (*)(std::uintptr_t,const void*)>(kPipeEndRushActor)(follower,descriptor);
    PipeFn<void (*)(bool)>(kPipeVisibility)(false);
}
void CancelPipeFollower(std::uintptr_t pipe) { CancelRushFollower(pipe, PairedFollower(pipe)); }
struct ActivePipeScope {
    std::uintptr_t previous;
    explicit ActivePipeScope(std::uintptr_t pipe):previous(sActivePipe) {sActivePipe=pipe;}
    ~ActivePipeScope() {sActivePipe=previous;}
};
HOOK_DEFINE_TRAMPOLINE(Patch0167PipeAnimation) {
    static void Callback(const char* name,const char* upper) {
        Orig(name,upper);
        if (const auto follower=PairedFollower(sActivePipe)) {
            PipePlayerScope scope(follower); Orig(name,upper);
        }
    }
};
HOOK_DEFINE_TRAMPOLINE(Patch0167PipeJumpAnimation) {
    static void Callback(const char* name,int frame) {
        Orig(name,frame);
        if (const auto follower=PairedFollower(sActivePipe)) {
            PipePlayerScope scope(follower); Orig(name,frame);
        }
    }
};
HOOK_DEFINE_TRAMPOLINE(Patch0167PipeVisibility) {
    static void Callback(bool hidden) {
        Orig(hidden);
        if (const auto follower=PairedFollower(sActivePipe)) {
            PipePlayerScope scope(follower); Orig(hidden);
        }
    }
};
HOOK_DEFINE_TRAMPOLINE(Patch0167PipeRelease) {
    static void Callback(const void* descriptor) {
        const auto follower=PairedFollower(sActivePipe);
        Orig(descriptor);
        if (follower) {
            PipePlayerScope scope(follower);
            Orig(descriptor); // exact same native exit velocity and flags
            if (sPairLogs<16)
                Logging.Log("[GalaxyCoop:smg1] PATCH-0167 paired native release follower=p%d binding_cleared=%d kill=1",
                    reinterpret_cast<void*>(follower)==GetSecondPlayer()?2:1,
                    *reinterpret_cast<const std::uintptr_t*>(follower+kPlayerRushSensor)==0?1:0);
        }
    }
};

HOOK_DEFINE_TRAMPOLINE(Patch0161PipeReceive) {
    static bool Callback(std::uintptr_t pipe, std::uint32_t msg,
                         std::uintptr_t sender, std::uintptr_t receiver) {
        if (!IsEarthenPipe(pipe)) return Orig(pipe, msg, sender, receiver);
        std::uintptr_t actor = *reinterpret_cast<const std::uintptr_t*>(
            pipe + kPipeBoundActorField);
        if (msg == kPipeAutoRushBegin && IsGuestPointer(sender)) {
            actor = *reinterpret_cast<const std::uintptr_t*>(sender + kPipeSensorHostField);
            // Let the short native bubble finish before beginning another
            // transport. Ghost countdowns may continue through pipe travel.
            if (kPipeCompanionEnabled && IsCapturedPipePlayer(actor) &&
                (GetP1DeathPhase()==DeathPhase::Recovering || GetP2DeathPhase()==DeathPhase::Recovering)) return false;
            DropStaleRushLeader();
            void* leader=GetCoopPipeLeader();
            if (IsCoopTravelHidden(reinterpret_cast<void*>(actor)) ||
                (leader && leader!=reinterpret_cast<void*>(actor))) return false;
        }
        // Each actor requests the same pipe matrix through its own sensor.
        // Native pipe+140 stays assigned to the leader throughout the trip.
        const auto follower=PairedFollower(pipe);
        const auto senderActor=IsGuestPointer(sender) ?
            *reinterpret_cast<const std::uintptr_t*>(sender+kPipeSensorHostField) : 0;
        if (follower && senderActor==follower) {
            if (msg==kPipeUpdateBaseMtx) {
                PipeFn<void (*)(std::uintptr_t,std::uintptr_t)>(kPipeSetBaseMatrix)(follower,pipe+0x148);
                return true;
            }
            if (msg==0x93 || msg==kPipeAutoRushBegin) return false;
        }
        if (msg==0x93 && IsCapturedPipePlayer(actor)) CancelPipeFollower(pipe);
        // Native UpdateBaseMtx unconditionally dereferences the bound actor.
        // A late message after cancellation/release has no valid recipient.
        if (msg == kPipeUpdateBaseMtx &&
            !IsValidMarioActor(reinterpret_cast<void*>(actor))) return false;
        bool accepted;
        {
            PipePlayerScope scope(actor);
            accepted = Orig(pipe, msg, sender, receiver);
        }
        if (accepted && msg==kPipeAutoRushBegin && IsCapturedPipePlayer(actor))
            BeginCoopPipeTravel(reinterpret_cast<void*>(actor));
        if (msg==0x93 && IsCapturedPipePlayer(actor) &&
            *reinterpret_cast<const std::uintptr_t*>(pipe+kPipeBoundActorField)==0)
            FinishCoopPipeTravel(reinterpret_cast<void*>(actor));
        if (msg == kPipeAutoRushBegin && IsCapturedPipePlayer(actor) &&
            sPipeBeginLogs < kPipeLogLimit) {
            ++sPipeBeginLogs;
            Logging.Log("[GalaxyCoop:smg1] PATCH-0161 pipe begin p%d accepted=%d pipe=%p bound=%p kill=1",
                actor == reinterpret_cast<std::uintptr_t>(GetSecondPlayer()) ? 2 : 1,
                accepted ? 1 : 0, reinterpret_cast<void*>(pipe),
                reinterpret_cast<void*>(*reinterpret_cast<const std::uintptr_t*>(pipe + kPipeBoundActorField)));
        }
        return accepted;
    }
};

// PATCH-0186: Launch Star counterpart of Patch0161PipeReceive. The follower's
// UpdateBaseMtx reuses the native star matrix by pointing +0xD0 at the
// follower for that one synchronous call; every other follower message is
// refused so it can never begin, cancel or end the leader's launch.
int LaunchStarPlayerId(std::uintptr_t actor) {
    if (actor == reinterpret_cast<std::uintptr_t>(GetFirstPlayer())) return 1;
    if (actor == reinterpret_cast<std::uintptr_t>(GetSecondPlayer())) return 2;
    return 0;
}
void Diag0187(std::uintptr_t star, std::uint32_t msg, std::uintptr_t senderActor,
              std::uintptr_t boundBefore, std::uintptr_t boundAfter,
              int accepted, const char* reason) {
    if (!kDiag0187LaunchStarTraceEnabled || sDiag0187Logs >= kDiag0187Limit) return;
    if (!Diag0187Interesting(msg)) return;
    ++sDiag0187Logs;
    void* leader = GetCoopPipeLeader();
    Logging.Log("[GalaxyCoop:smg1] DIAG-0187 star=%p msg=0x%x from=p%d bound=p%d->p%d accepted=%d leader=p%d reason=%s kill=0",
        reinterpret_cast<void*>(star), msg, LaunchStarPlayerId(senderActor),
        LaunchStarPlayerId(boundBefore), LaunchStarPlayerId(boundAfter), accepted,
        leader ? LaunchStarPlayerId(reinterpret_cast<std::uintptr_t>(leader)) : 0, reason);
}
HOOK_DEFINE_TRAMPOLINE(Patch0189BeginRush) {
    static void Callback(std::uintptr_t player) {
        const auto sensor = *reinterpret_cast<const std::uintptr_t*>(player + kPlayerRushCandidate);
        std::uintptr_t host = 0;
        if (IsGuestPointer(sensor)) {
            host = *reinterpret_cast<const std::uintptr_t*>(sensor + kPipeSensorHostField);
            if (!IsGuestPointer(host)) host = 0;
        }
        if (kDiag0188BeginRushTraceEnabled && sDiag0188Logs < kDiag0188Limit) {
            const auto base = exl::util::modules::GetTargetStart();
            const auto vtable = host ? *reinterpret_cast<const std::uintptr_t*>(host) : 0;
            ++sDiag0188Logs;
            Logging.Log("[GalaxyCoop:smg1] DIAG-0188 begin rush p%d host=%p vtable=0x%lx launch_star=%d sling_star=%d pipe=%d dome_star=%d kill=0",
                LaunchStarPlayerId(player), reinterpret_cast<void*>(host),
                vtable > base ? static_cast<unsigned long>(vtable - base) : 0UL,
                IsLaunchStar(host) ? 1 : 0, IsSlingStar(host) ? 1 : 0, IsEarthenPipe(host) ? 1 : 0,
                IsAstroDomeBlueStar(host) ? 1 : 0);
        }
        Orig(player);
        // Only the dome departure. A pipe or a Launch Star carries the partner
        // natively (PATCH-0167/PATCH-0186) and an enemy grab is not travel.
        if (!kPatch0189DomeStarCompanionEnabled || !IsAstroDomeBlueStar(host)) return;
        if (!HasActivePlayerPair() || !IsCapturedPipePlayer(player)) return;
        auto* other = reinterpret_cast<void*>(player) == GetFirstPlayer()
            ? GetSecondPlayer() : GetFirstPlayer();
        if (!other) return;
        const bool bubbled = RequestCoopTravelBubble(other);
        if (!bubbled && sDomeCompanionLogs < 8) {
            ++sDomeCompanionLogs;
            Logging.Log("[GalaxyCoop:smg1] PATCH-0189 dome departure p%d partner not bubbled kill=1",
                        LaunchStarPlayerId(player));
        }
    }
};
HOOK_DEFINE_TRAMPOLINE(Diag0190DomeStarReceive) {
    static bool Callback(std::uintptr_t star, std::uint32_t msg,
                         std::uintptr_t sender, std::uintptr_t receiver) {
        const bool accepted = Orig(star, msg, sender, receiver);
        if (!kDiag0190DomeStarTraceEnabled || sDiag0190Logs >= kDiag0190Limit) return accepted;
        const bool binding = Diag0187Interesting(msg);
        if (!binding && !Diag0190FirstOfKind(msg)) return accepted;
        std::uintptr_t owner = 0;
        if (IsGuestPointer(sender)) {
            owner = *reinterpret_cast<const std::uintptr_t*>(sender + kPipeSensorHostField);
            if (!IsGuestPointer(owner)) owner = 0;
        }
        auto* p1 = GetFirstPlayer();
        auto* p2 = GetSecondPlayer();
        ++sDiag0190Logs;
        Logging.Log("[GalaxyCoop:smg1] DIAG-0190 dome star=%p msg=0x%x from=p%d accepted=%d bound=(%d,%d) kill=0",
            reinterpret_cast<void*>(star), msg, LaunchStarPlayerId(owner), accepted ? 1 : 0,
            p1 && HasActiveBinding(reinterpret_cast<std::uintptr_t>(p1)) ? 1 : 0,
            p2 && HasActiveBinding(reinterpret_cast<std::uintptr_t>(p2)) ? 1 : 0);
        return accepted;
    }
};
// PATCH-0186 / ATT-0215: one body for both star receive hooks (Launch Star
// 0x1CBAF4, Sling Star 0x24F9C0); Hook supplies that hook's own Orig.
template<typename Hook>
bool StarReceive(std::uintptr_t star, std::uint32_t msg, std::uintptr_t sender, std::uintptr_t receiver) {
    if (!IsStarHost(star)) return Hook::Orig(star, msg, sender, receiver);
    auto* slot = RushHostBoundSlot(star);
    const auto senderActor = IsGuestPointer(sender) ?
        *reinterpret_cast<const std::uintptr_t*>(sender + kPipeSensorHostField) : 0;
    const auto follower = PairedFollower(star);
    if (follower && senderActor == follower) {
        if (msg != kPipeUpdateBaseMtx) {
            Diag0187(star, msg, senderActor, *slot, *slot, 0, "follower_refused");
            return false;
        }
        const auto leader = *slot;
        *slot = follower;
        bool accepted;
        {
            PipePlayerScope scope(follower);
            accepted = Hook::Orig(star, msg, sender, receiver);
        }
        *slot = leader;
        return accepted;
    }
    if (msg == kPipeAutoRushBegin && IsCapturedPipePlayer(senderActor)) {
        if (GetP1DeathPhase()==DeathPhase::Recovering || GetP2DeathPhase()==DeathPhase::Recovering) {
            Diag0187(star, msg, senderActor, *slot, *slot, 0, "recovering");
            return false;
        }
        DropStaleRushLeader();
        void* leader = GetCoopPipeLeader();
        if (leader && leader != reinterpret_cast<void*>(senderActor)) {
            Diag0187(star, msg, senderActor, *slot, *slot, 0, "other_leader");
            return false;
        }
    }
    const auto boundBefore = *slot;
    const auto scoped = IsCapturedPipePlayer(senderActor) ? senderActor : boundBefore;
    bool accepted;
    {
        PipePlayerScope scope(scoped);
        accepted = Hook::Orig(star, msg, sender, receiver);
    }
    Diag0187(star, msg, senderActor, boundBefore, *slot, accepted ? 1 : 0, "native");
    if (accepted && msg == kPipeAutoRushBegin && IsCapturedPipePlayer(senderActor)) {
        BeginCoopPipeTravel(reinterpret_cast<void*>(senderActor));
        if (sLaunchStarLogs < kPipeLogLimit) {
            ++sLaunchStarLogs;
            Logging.Log("[GalaxyCoop:smg1] %s begin p%d star=%p kill=1", RushHostLabel(star),
                senderActor == reinterpret_cast<std::uintptr_t>(GetSecondPlayer()) ? 2 : 1,
                reinterpret_cast<void*>(star));
        }
    }
    // A leader cancel (0x93/0x94) clears +0xD0 outside any nerve, so the
    // movement hook never sees the release: end the follower and travel here.
    // Inside the star's own movement, Patch0167PipeRelease duplicates the
    // native launch release instead; never pre-empt it with a dead stop.
    if (sActivePipe != star && IsCapturedPipePlayer(boundBefore) && *slot == 0) {
        if (follower) CancelRushFollower(star, follower);
        FinishCoopPipeTravel(reinterpret_cast<void*>(boundBefore));
        if (sLaunchStarLogs < kPipeLogLimit) {
            ++sLaunchStarLogs;
            Logging.Log("[GalaxyCoop:smg1] %s cancel msg=0x%x leader=p%d follower=%d kill=1",
                RushHostLabel(star), msg, boundBefore == reinterpret_cast<std::uintptr_t>(GetSecondPlayer()) ? 2 : 1,
                follower ? 1 : 0);
        }
    }
    return accepted;
}
HOOK_DEFINE_TRAMPOLINE(Patch0186LaunchStarReceive) {
    static bool Callback(std::uintptr_t star, std::uint32_t msg,
                         std::uintptr_t sender, std::uintptr_t receiver) {
        return StarReceive<Patch0186LaunchStarReceive>(star, msg, sender, receiver);
    }
};
HOOK_DEFINE_TRAMPOLINE(Att0215SlingStarReceive) {
    static bool Callback(std::uintptr_t star, std::uint32_t msg,
                         std::uintptr_t sender, std::uintptr_t receiver) {
        return StarReceive<Att0215SlingStarReceive>(star, msg, sender, receiver);
    }
};

// ATT-0216: either native player lock byte (input lock, demo mode).
bool InPlayerLock(void* player) {
    if (!IsValidMarioActor(player)) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(player);
    return *reinterpret_cast<const std::uint8_t*>(base + kPlayerLockFlag) != 0 ||
           *reinterpret_cast<const std::uint8_t*>(base + kPlayerDemoMode) != 0;
}

// ATT-0219: the line for one listed actor's choice; why is as NearerPlayerChoice
// sets it, d1/d2 squared distances (-1 when not measured).
void NoteNearerSwap(std::uintptr_t actor, std::uintptr_t vtable, int why, float d1, float d2) {
    NearerTrack* track = nullptr;
    for (auto& entry : sNearerTrack) if (entry.actor == actor && entry.vtable == vtable) track = &entry;
    if (!track) {
        track = &sNearerTrack[sNearerTrackNext++ % 32];
        *track = {actor, vtable, false, false, kNearerQuietCalls, 0};
    }
    const bool on = why == 0;
    if (on != track->on) ++track->flips;
    track->on = on;
    track->calls = std::min(track->calls + 1, 100000u);
    if (on == track->logged || track->calls < kNearerQuietCalls) return;
    track->logged = on;
    if (sNearerSwapLogs < kNearerSwapLogLimit) {
        ++sNearerSwapLogs;
        const auto root = [](float d) { return d < 0 ? -1.0 : static_cast<double>(std::sqrt(d)); };
        Logging.Log("[GalaxyCoop:smg1] ATT-0219 nearer %s actor=%p vtable=0x%lx why=%d (0 p2 nearer, 1 no pair/parked, 2 scene, 3 p2 down/hidden, 4 p1 nearer) distance=(%.0f,%.0f) flips=%u calls=%u kill=1",
            on ? "p2 start" : "p2 stop", reinterpret_cast<void*>(actor), static_cast<unsigned long>(vtable),
            why, root(d1), root(d2), track->flips, track->calls);
    }
    track->calls = 0;
    track->flips = 0;
}

// ATT-0215: the player an actor should see during its movement: P2 when it
// is in play and nearer, else 0 (keep the native singleton, P1). ATT-0223:
// the player it had last time keeps it until the other is 15% nearer.
// why: 0 P2 nearer, 1 no pair or P2 parked, 2 scene or talk, 3 P2 down or
// hidden, 4 P1 nearer.
std::uintptr_t NearerPlayerChoice(std::uintptr_t actor, std::uintptr_t vtable, bool listed, bool hadP2,
                                  int& why, float& d1, float& d2) {
    why = 1;
    if (!HasActivePlayerPair() || IsP2Parked()) return 0;
    // ATT-0216: a scene or talk in progress belongs to Mario alone.
    why = 2;
    if (kAtt0216DemoLockOnMarioEnabled && (InPlayerLock(GetFirstPlayer()) || InPlayerLock(GetSecondPlayer()))) return 0;
    why = 3;
    void* p1 = GetFirstPlayer(); void* p2 = GetSecondPlayer();
    if (!IsValidMarioActor(p1) || !IsValidMarioActor(p2) || GetP2DeathPhase() != DeathPhase::Alive ||
        IsCoopTravelHidden(p2)) return 0;
    const auto* at = reinterpret_cast<const float*>(actor + 0x14);
    const auto distance2 = [at](void* player) {
        const auto* p = reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(player) + 0x14);
        const float dx = p[0] - at[0], dy = p[1] - at[1], dz = p[2] - at[2];
        return dx * dx + dy * dy + dz * dz;
    };
    d1 = distance2(p1); d2 = distance2(p2);
    why = 4;
    if (hadP2 ? d1 < d2 * kNearerTakeOver2 : !(d2 < d1 * kNearerTakeOver2)) return 0;
    why = 0;
    // One line per actor class the first time Luigi becomes its player.
    static std::uintptr_t sLogged[kNearerClassLogLimit]{};
    bool seen = false;
    for (unsigned i = 0; i < sNearerLogs; ++i) seen = seen || sLogged[i] == vtable;
    if (!seen && sNearerLogs < kNearerClassLogLimit) {
        sLogged[sNearerLogs++] = vtable;
        Logging.Log("[GalaxyCoop:smg1] %s nearer player p2 actor=%p vtable=0x%lx distance=(%.0f,%.0f) classes=%u kill=1",
            listed ? "ATT-0215" : "ATT-0223", reinterpret_cast<void*>(actor), static_cast<unsigned long>(vtable),
            static_cast<double>(std::sqrt(d1)), static_cast<double>(std::sqrt(d2)), sNearerLogs);
    }
    return reinterpret_cast<std::uintptr_t>(p2);
}
std::uintptr_t NearerPlayerFor(std::uintptr_t actor) {
    if (!kAtt0215NearerPlayerEnabled || !IsGuestPointer(actor)) return 0;
    const auto vtable = *reinterpret_cast<const std::uintptr_t*>(actor) - exl::util::modules::GetTargetStart();
    bool listed = false, excluded = false;
    for (const auto offset : kNearerPlayerVtables) listed = listed || vtable == offset;
    for (const auto offset : kNearerExcludedVtables) excluded = excluded || vtable == offset;
    if (!listed && (!kAtt0223GlobalNearerEnabled || excluded)) return 0;
    NearerChoice& choice = sNearerChoices[(actor >> 4) % std::size(sNearerChoices)];
    const bool hadP2 = choice.actor == actor && choice.p2;
    int why;
    float d1 = -1, d2 = -1;
    const auto nearer = NearerPlayerChoice(actor, vtable, listed, hadP2, why, d1, d2);
    choice = {actor, nearer != 0};
    // Per-actor start/stop lines stay on the listed classes; the rest log once per class.
    if (listed && kAtt0219NearerSwapLogEnabled) NoteNearerSwap(actor, vtable, why, d1, d2);
    return nearer;
}

// ATT-0216: native lock setters run with the singleton on Mario.
HOOK_DEFINE_TRAMPOLINE(Att0216DemoStart) {
    static void Callback() {
        PipePlayerScope scope(reinterpret_cast<std::uintptr_t>(GetFirstPlayer()));
        Orig();
    }
};
HOOK_DEFINE_TRAMPOLINE(Att0216InputLock) {
    static void Callback() {
        PipePlayerScope scope(reinterpret_cast<std::uintptr_t>(GetFirstPlayer()));
        Orig();
    }
};
// Demo end for Mario, then natively for a Luigi left in demo mode.
HOOK_DEFINE_TRAMPOLINE(Att0216DemoEnd) {
    static void Callback() {
        {
            PipePlayerScope scope(reinterpret_cast<std::uintptr_t>(GetFirstPlayer()));
            Orig();
        }
        void* p2 = GetSecondPlayer();
        if (!HasActivePlayerPair() || !IsValidMarioActor(p2)) return;
        const auto base = reinterpret_cast<std::uintptr_t>(p2);
        if (*reinterpret_cast<const std::uint8_t*>(base + kPlayerDemoMode) == 0) return;
        {
            PipePlayerScope scope(base);
            Orig();
        }
        if (sDemoLockLogs < 16) {
            ++sDemoLockLogs;
            Logging.Log("[GalaxyCoop:smg1] ATT-0216 ended stale Luigi demo lock demo_mode=%u lock=%u kill=1",
                *reinterpret_cast<const std::uint8_t*>(base + kPlayerDemoMode),
                *reinterpret_cast<const std::uint8_t*>(base + kPlayerLockFlag));
        }
    }
};

// ATT-0224 / EVD-0535, same SMG1 main hash, NSO offsets (Ghidra): 0x11C818
// (mario, status) unlinks status from the inner Mario's status stack (head
// +0xBE0, next +0x10) and calls its vtable +0x20 close; with a status that is
// not on the stack its walk reaches null and stores to +0x10 (console crash
// 01793522751, PC 0x11C888). Talk end 0x1B15A4 closes "the player"'s own talk
// status (MarioActor+0x3C0 -> +0x928) through MarioHolder+0x18, so a talk that
// ATT-0223 opened on Luigi (during a Tico's movement) was closed on Mario.
// Opening stp/str/mov/ldr/mov, nothing branches into 0x11C81C..0x11C82C.
// ATT-0225 / EVD-0536: any status can be opened under one player and closed
// under the other, so the match no longer depends on the talk field. The opener
// 0x16C170 identifies a status by its type id (+0x18, int): a close for a
// status not on that stack closes the status of the same type on the other
// player, and is dropped when neither stack has one. The native game never
// closes a status that is not open (it would fault), so only co-op gets here.
constexpr bool kAtt0224StatusCloseGuardEnabled = true;
constexpr std::uintptr_t kStatusClose = 0x11C818;
constexpr std::uintptr_t kInnerMario = 0x3C0, kStatusHead = 0xBE0, kStatusNext = 0x10, kStatusType = 0x18;
unsigned sStatusCloseLogs = 0;

bool StatusStackHas(std::uintptr_t mario, std::uintptr_t status) {
    auto node = *reinterpret_cast<const std::uintptr_t*>(mario + kStatusHead);
    for (unsigned guard = 0; node != 0 && guard < 64; ++guard) {
        if (node == status) return true;
        node = *reinterpret_cast<const std::uintptr_t*>(node + kStatusNext);
    }
    return false;
}
std::uintptr_t StatusOfType(std::uintptr_t mario, std::int32_t type) {
    auto node = *reinterpret_cast<const std::uintptr_t*>(mario + kStatusHead);
    for (unsigned guard = 0; node != 0 && guard < 64; ++guard) {
        if (*reinterpret_cast<const std::int32_t*>(node + kStatusType) == type) return node;
        node = *reinterpret_cast<const std::uintptr_t*>(node + kStatusNext);
    }
    return 0;
}
std::uintptr_t InnerMario(void* player) {
    if (!IsValidMarioActor(player)) return 0;
    return *reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(player) + kInnerMario);
}

HOOK_DEFINE_TRAMPOLINE(Att0224StatusClose) {
    [[gnu::noipa]] static void Callback(std::uintptr_t mario, std::uintptr_t status) {
        const auto returnAddress = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
        if (status == 0 || !IsGuestPointer(mario) || StatusStackHas(mario, status)) {
            Orig(mario, status);
            return;
        }
        const auto m1 = InnerMario(GetFirstPlayer()), m2 = InnerMario(GetSecondPlayer());
        const auto other = mario == m1 ? m2 : mario == m2 ? m1 : 0;
        const bool typed = IsGuestPointer(status);
        const std::int32_t type = typed ? *reinterpret_cast<const std::int32_t*>(status + kStatusType) : -1;
        bool redirected = false;
        if (typed && IsGuestPointer(other)) {
            if (const auto match = StatusOfType(other, type)) {
                Orig(other, match);
                redirected = true;
            }
        }
        if (sStatusCloseLogs < 32) {
            ++sStatusCloseLogs;
            Logging.Log("[GalaxyCoop:smg1] ATT-0225 status close not on stack player=p%d type=%d redirected=%d from=0x%lx kill=1",
                mario == m1 ? 1 : mario == m2 ? 2 : 0, static_cast<int>(type), redirected ? 1 : 0,
                static_cast<unsigned long>(returnAddress - exl::util::modules::GetTargetStart()));
        }
    }
};

HOOK_DEFINE_TRAMPOLINE(Patch0161PipeMovement) {
    static void Callback(std::uintptr_t actor) {
        // Shared LiveActor entry: no pipe-layout read until exact vtable match.
        if (!IsCoopRushHost(actor)) {
            if (const auto nearer = NearerPlayerFor(actor)) {
                PipePlayerScope scope(nearer);
                Orig(actor);
                return;
            }
            Orig(actor);
            return;
        }
        const std::uintptr_t bound = *RushHostBoundSlot(actor);
        JoinNativePipe(actor,bound);
        {
            ActivePipeScope pipeScope(actor);
            PipePlayerScope scope(bound);
            Orig(actor);
        }
        if (IsCapturedPipePlayer(bound) && *RushHostBoundSlot(actor) == 0) {
            FinishCoopPipeTravel(reinterpret_cast<void*>(bound));
            if (sPipeExitLogs < kPipeLogLimit) {
                ++sPipeExitLogs;
                Logging.Log("[GalaxyCoop:smg1] %s released p%d host=%p kill=1",
                    IsEarthenPipe(actor) ? "PATCH-0161 pipe" : RushHostLabel(actor),
                    bound == reinterpret_cast<std::uintptr_t>(GetSecondPlayer()) ? 2 : 1,
                    reinterpret_cast<void*>(actor));
            }
        }
    }
};

// DIAG-0150 / PATCH-0150 dev boot warp provenance (SMG1 main
// SHA-256 76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938,
// image base 0x7100000000, Ghidra program "main"):
// - EggStarGalaxy string at NSO 0x61373F (GalaxyStringSurface) with 8 ADRP+ADD
//   code sites (GalaxyFindAdrpRefs), including wrapper FUN at 0x199080:
//   `FUN_7100116200("EggStarGalaxy",1)` (GalaxyDecompileContaining).
// - 0x116200 is a proven typed entry (stp x29,x30,[sp,#-0x20]! at 0x116200,
//   GalaxyListingAround) that self-derives its manager live from root slot
//   0x90CEA8 (+0x0 -> +0x18 -> +0x10 -> 0x8BD5C) then tail-branches (B, never
//   hook) to 0x116244 (GalaxyListingAround). 7 BL/B callers incl. 0x19908C,
//   0x22FE1C (GalaxyFindBlCallers). Signature (const char* stage, int scenario).
// - Stage/Scene selectors 0x09BF90 / 0x1C2320 consume root+0x48 scene-info+0x20
//   and must be gated on both pointers (EVD-0100); same guard as PATCH-0019.
// Never inline-hook the tail branch at 0x116240. This experiment adds zero new
// hook sites: it only calls 0x116200 once from an already-owned per-frame hook.
// Rev 2: trigger covers AstroDome hub as well as HeavensDoorGalaxy because a
// loaded save lands in the Observatory hub, not the Gateway tutorial. The
// selector stays the proven 0x09BF90 with identical root+0x48+0x20 guards.
// Rev 13: producer path exhausted (rc=0 every context, EVD-0164). New route:
// native fill 0x21D33C plus execute 0x21D320 with disc 6 (direct-load,
// EVD-0165): fill our own 32-byte request at hub spawn, execute per-frame
// under a live-sequence gate. Zero new hook sites; one latched attempt.
constexpr std::uintptr_t kDevWarpTransFill = 0x21D33C;
constexpr std::uintptr_t kDevWarpTransExecute = 0x21D320;
// Disc 0 mirrors the native generic loader 0x1B6040 (fill disc 0 plus caller
// stage, scenario, extra, then execute). Disc 6 was the 0x2B18E0 template
// guess and is retired untested by this stronger evidence (EVD-0167).
constexpr std::uint32_t kDevWarpTransDisc = 0;
// Route 2 static buffer removed (sTransReq, sTransFilled) — exhausted.
// Rev 8: heartbeat proved no silent flip; the hub budget was simply spent in
// FileSelect. Deeper finding: spawn-callback chain reads are in-flow valid
// (hub=1 at spawn) while steady-state per-frame reads are out-of-flow
// garbage, so the trigger ARMS at spawn and per-frame only acts on the flag
// plus the 0x116200 manager-chain gate (root/*+0/+0x18/+0x10).
// Rev 3: stop guessing hub names. DIAG-0151 dumps the raw stage-name string
// from scene-info+0x20 (bounded, same guards) so one hub run reveals the true
// stage the save lands in; the warp trigger itself is unchanged this round.
constexpr std::uintptr_t kDevWarpRootSlot = 0x90CEA8;
constexpr std::uintptr_t kDevWarpSceneEquals = 0x1C2320;
constexpr std::uintptr_t kDevWarpStageEquals = 0x09BF90;
constexpr std::uintptr_t kDevWarpStageRequest = 0x116200;
// EggStar literal 0x61373F and scenario 1 now live in the destination table below.

// Rev 21 (power-up testing): the single EggStar destination becomes a table.
// EggStarGalaxy places no power-up in any scenario, so the legacy target is
// useless for the power-up pass and entry 0 is kept only as the proven control.
//
// Stage-name provenance, SMG1 main SHA-256
// 76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938, image base
// 0x7100000000: GalaxyStringSurface located every name below as a standalone
// NUL-terminated ASCII literal. Entries 1..6 come from the contiguous
// stage-name block 0x652239..0x65242E; entry 0 keeps the literal at 0x61373F
// already proven by EVD-0150/EVD-0160. Each pointer is content-checked at use
// (DevWarpAsciiEqualFold), so a wrong offset disables that destination instead
// of feeding a bad pointer into the transition request.
//
// Scenario numbers are romfs-local, not guessed: StageData/<Galaxy>/
// <Galaxy>Scenario.arc scenariodata.bcsv gives the per-zone layer mask per
// ScenarioNo, joined with jmp/placement/<layer>/objinfo in each zone arc. The
// scenario chosen for each row is one whose active layers place the named
// power-up with SW_APPEAR = -1 (no switch required).
struct DevWarpDestination {
    const char* expected;       // required .rodata content
    std::uintptr_t nameOffset;  // NSO offset of that literal in main
    std::uint32_t scenario;     // ScenarioNo to request
    const char* powerUp;        // power-up reachable in that scenario
    std::int32_t startNo;       // PATCH-0185: start MarioNo, -1 keeps native
    std::int32_t startZone;     // PATCH-0185: zone id of that start
};

constexpr DevWarpDestination kDevWarpDestinations[] = {
    {"EggStarGalaxy",         0x61373F, 1, "none-control", -1, 0},  // 0 legacy target
    {"HoneyBeeKingdomGalaxy", 0x652239, 1, "Bee",          -1, 0},           // 1 Minus + Y
    {"PhantomGalaxy",         0x65232F, 1, "Boo",          141, 6},           // 2 Minus + X; zone 6 = ZoneList row of TeresaHouseZone (EVD-0243)
    {"IceVolcanoGalaxy",      0x65239F, 1, "Ice+Fire",     -1, 0},      // 3 Minus + B
    {"FactoryGalaxy",         0x65242E, 1, "Spring",       -1, 0},        // 4 Minus + A
    {"SandClockGalaxy",       0x6523C1, 2, "Rainbow",      -1, 0},       // 5 Minus + Left
    {"HoneyBeeExGalaxy",      0x6523B0, 1, "Bee-quick",    -1, 0},     // 6 Minus + Right
};
constexpr bool kPatch0185StartIdEnabled = true;
constexpr std::uint32_t kDevWarpDestinationCount =
    static_cast<std::uint32_t>(sizeof(kDevWarpDestinations) / sizeof(kDevWarpDestinations[0]));
// Destination used by the zero-interaction boot arm when no combo was pressed.
constexpr std::uint32_t kDevWarpDefaultDestination = 2; // Boo (Bee verified: EVD-0231, EVD-0236)
std::uint32_t sDevWarpDestination = kDevWarpDefaultDestination;

// The proven dome-entry gate shape carried scenario -1, which is the native
// "keep the star the dome selection chose". P1 cannot aim the Star Pointer
// (TASK-0022), so no star can be chosen and the explicit ScenarioNo from the
// table is requested instead. Set true to fall back to the proven -1 shape.
constexpr bool kDevWarpUseNativeScenario = false;
constexpr std::uint32_t kDevWarpNativeScenario = 0xFFFFFFFFu;

// Disabled by operator request (2026-10-24): preserve normal stage progression.
constexpr bool kDevWarpEnabled = false;
// Fire switch: false parks the warp trigger (gate-fix-first decision) while
// DIAG-0150/0151/0152 logging stays armed for passive chain data.
constexpr bool kDevWarpFireEnabled = false;

constexpr std::uint32_t kDevWarpLogLimit = 8;
constexpr std::uint32_t kWarpHeartbeatLimit = 6;
std::uint32_t sWarpHeartbeat = 0;
std::uint32_t sTopSnapCount = 0;

constexpr float kTriggerDistance = 1500.0f;
constexpr std::uint32_t kHoldFrames = 45;

enum class WarpState { Inactive, Triggered, Flying, Completed };
WarpState sWarpState = WarpState::Inactive;
std::uint32_t sHoldCounter = 0;
float sCarrierPosition[3] = {};

}  // namespace

const float* GetCameraPositionForActor(std::uintptr_t actor, const float* nativePosition) {
    void* p2 = GetSecondPlayer();
    if (actor == reinterpret_cast<std::uintptr_t>(p2) && sWarpState == WarpState::Flying) {
        return sCarrierPosition;
    }
    return nativePosition;
}

bool ApplyRecoveryCameraMidpoint(float* watch, float* eye, const float* player1, const float* player2) {
    if (sWarpState == WarpState::Flying) {
        watch[0] = (player1[0] + sCarrierPosition[0]) * 0.5f;
        watch[1] = (player1[1] + sCarrierPosition[1]) * 0.5f;
        watch[2] = (player1[2] + sCarrierPosition[2]) * 0.5f;
        return true;
    }
    return false;
}

namespace {

HOOK_DEFINE_TRAMPOLINE(Patch0016RecoveryProducer) {
    static void Callback(std::uintptr_t actor) {
        Orig(actor);

        void* p1 = GetFirstPlayer();
        void* p2 = GetSecondPlayer();
        if (!IsValidMarioActor(p1) || !IsValidMarioActor(p2)) {
            return;
        }

        const float* p1Pos = reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(p1) + 0x14);
        const float* p2Pos = reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(p2) + 0x14);

        const float dx = p1Pos[0] - p2Pos[0];
        const float dy = p1Pos[1] - p2Pos[1];
        const float dz = p1Pos[2] - p2Pos[2];
        const float distSq = dx * dx + dy * dy + dz * dz;

        if (distSq > kTriggerDistance * kTriggerDistance) {
            if (sWarpState == WarpState::Inactive) {
                ++sHoldCounter;
                if (sHoldCounter >= kHoldFrames) {
                    sWarpState = WarpState::Triggered;
                    Logging.Log("[GalaxyCoop:smg1] PATCH-0016 distance recovery triggered dist=%.1f", static_cast<double>(distSq));
                }
            }
        } else {
            sHoldCounter = 0;
        }
    }
};

}  // namespace

namespace {

// Rev 4: the rev3 log join showed every root-chain read as <bad-scene> while
// the hook itself is present and parked P2, so the warp never fires. This round
// only diagnoses: dump root/scene/stage pointer hex plus the stage name read
// both via the root chain and via the spawn scene param, on separate throttles.
// The warp trigger is unchanged.
// Rev 5: DIAG-0152 proved the old chain wrong (root stable guest, but
// *(root+0x48) is a small int 0x42/0x62/0xc82/0x1182, never a pointer). The raw
// listing at 0x9BF90 shows the truth the decompiler hid: slot -> *slot ->
// *+0x0 -> +0x48 -> name INLINE at +0x20 (add x0,x8,#0x20, not a load), then a
// tail branch to FUN_7100032B20 which is plain strcasecmp. The missing *+0x0
// hop (0x9bf9c ldr x8,[x8]) is restored below, and names are read inline.
bool DevWarpReadSceneBase(std::uintptr_t* outSceneBase) {
    const std::uintptr_t target = exl::util::modules::GetTargetStart();
    const std::uintptr_t root = *reinterpret_cast<const std::uintptr_t*>(target + kDevWarpRootSlot);
    if (!IsGuestPointer(root)) return false;
    const std::uintptr_t obj = *reinterpret_cast<const std::uintptr_t*>(root);
    if (!IsGuestPointer(obj)) return false;
    const std::uintptr_t sceneBase = *reinterpret_cast<const std::uintptr_t*>(obj + 0x48);
    if (!IsGuestPointer(sceneBase)) return false;
    if (outSceneBase != nullptr) *outSceneBase = sceneBase;
    return true;
}

bool DevWarpCopyInlineName(const char* raw, char* out) {
    for (int i = 0; i < 64; ++i) out[i] = '\0';
    if (raw == nullptr) return false;
    for (int i = 0; i < 63; ++i) {
        const char c = raw[i];
        if (c == '\0') return out[0] != '\0';
        if (c < 0x20 || c > 0x7E) return false;
        out[i] = c;
    }
    return false;
}

bool DevWarpAsciiEqualFold(const char* a, const char* b) {
    for (int i = 0; i < 64; ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
        if (ca != cb) return false;
        if (ca == '\0') return true;
    }
    return false;
}
bool sDevWarpFired = false;
bool sDevWarpArrivalLogged = false;
bool sWarpArmed = false;

const DevWarpDestination& DevWarpDestinationEntry() {
    const std::uint32_t index =
        (sDevWarpDestination < kDevWarpDestinationCount) ? sDevWarpDestination : 0;
    return kDevWarpDestinations[index];
}

// Resolve the selected stage name in main .rodata, content-checked. Returns
// nullptr when the literal does not read back as its recorded text, which
// holds the warp instead of writing an unproven pointer into the request.
const char* DevWarpDestinationString() {
    const DevWarpDestination& dest = DevWarpDestinationEntry();
    const char* text = reinterpret_cast<const char*>(
        exl::util::modules::GetTargetStart() + dest.nameOffset);
    return DevWarpAsciiEqualFold(text, dest.expected) ? text : nullptr;
}

std::uint32_t DevWarpDestinationScenario() {
    return kDevWarpUseNativeScenario ? kDevWarpNativeScenario
                                     : DevWarpDestinationEntry().scenario;
}

// DIAG-0177 test only: destination select on the P2 controller. Minus is the
// modifier DIAG-0173 already proved harmless on that pad; DIAG-0173 owns D-pad
// Up/Down, so this takes the four face buttons plus D-pad Left/Right. A press
// latches the destination, clears the one-shot fire latch and re-arms, so the
// next native transition (a dome entry, or pause -> return to the Star Ship)
// lands there. Set false for release builds.
constexpr bool kDevWarpSelectEnabled = true;
constexpr std::uint64_t kDevWarpNpadA = 1ull << 0;
constexpr std::uint64_t kDevWarpNpadB = 1ull << 1;
constexpr std::uint64_t kDevWarpNpadX = 1ull << 2;
constexpr std::uint64_t kDevWarpNpadY = 1ull << 3;
constexpr std::uint64_t kDevWarpNpadMinus = 1ull << 11;
constexpr std::uint64_t kDevWarpNpadLeft = 1ull << 12;
constexpr std::uint64_t kDevWarpNpadRight = 1ull << 14;
std::uint64_t sDevWarpSelectPrevious = 0;

void TickDevWarpSelect() {
    if (!kDevWarpEnabled || !kDevWarpSelectEnabled) return;
    const std::uint64_t buttons = ReadNpadRawButtons(1);
    const std::uint64_t pressed = buttons & ~sDevWarpSelectPrevious;
    sDevWarpSelectPrevious = buttons;
    if ((buttons & kDevWarpNpadMinus) == 0) return;
    std::uint32_t choice = kDevWarpDestinationCount;
    if ((pressed & kDevWarpNpadY) != 0) choice = 1;
    else if ((pressed & kDevWarpNpadX) != 0) choice = 2;
    else if ((pressed & kDevWarpNpadB) != 0) choice = 3;
    else if ((pressed & kDevWarpNpadA) != 0) choice = 4;
    else if ((pressed & kDevWarpNpadLeft) != 0) choice = 5;
    else if ((pressed & kDevWarpNpadRight) != 0) choice = 6;
    if (choice >= kDevWarpDestinationCount) return;
    sDevWarpDestination = choice;
    const bool resolved = DevWarpDestinationString() != nullptr;
    sDevWarpFired = false;
    sDevWarpArrivalLogged = false;
    sWarpArmed = resolved;
    Logging.Log("[GalaxyCoop:smg1] DIAG-0177 warp select idx=%u stage=%s scenario=%u powerup=%s resolved=%d armed=%d kill=1",
                static_cast<unsigned>(choice),
                DevWarpDestinationEntry().expected,
                static_cast<unsigned>(DevWarpDestinationScenario()),
                DevWarpDestinationEntry().powerUp,
                resolved ? 1 : 0,
                sWarpArmed ? 1 : 0);
}
std::uint32_t sDevWarpLogCount = 0;
std::uint32_t sDiag0151HubCount = 0;
std::uint32_t sDiag0151SpawnCount = 0;
std::uintptr_t sLastSpawnScene = 0;

using TransFillFn = void (*)(void*, std::uint32_t, const char*, int, void*);
using TransExecuteFn = void (*)(void*);

const char* DevWarpReadStageName() {
    static char sStageName[64];
    const std::uintptr_t target = exl::util::modules::GetTargetStart();
    const std::uintptr_t root = *reinterpret_cast<const std::uintptr_t*>(target + kDevWarpRootSlot);
    if (!IsGuestPointer(root)) return "<bad-root>";
    const std::uintptr_t obj = *reinterpret_cast<const std::uintptr_t*>(root);
    if (!IsGuestPointer(obj)) return "<bad-obj>";
    const std::uintptr_t sceneBase = *reinterpret_cast<const std::uintptr_t*>(obj + 0x48);
    if (!IsGuestPointer(sceneBase)) return "<bad-scene>";
    if (!DevWarpCopyInlineName(reinterpret_cast<const char*>(sceneBase + 0x20), sStageName)) return "<bad-name>";
    return sStageName;
}

const char* DevWarpReadStageNameFromScene(std::uintptr_t sceneBase) {
    static char sSpawnStageName[64];
    if (!IsGuestPointer(sceneBase)) return "<bad-spawn-scene>";
    if (!DevWarpCopyInlineName(reinterpret_cast<const char*>(sceneBase + 0x20), sSpawnStageName)) {
        return "<bad-spawn-name>";
    }
    return sSpawnStageName;
}

void DevWarpDumpChain(const char* tag) {
    const std::uintptr_t target = exl::util::modules::GetTargetStart();
    const std::uintptr_t root = *reinterpret_cast<const std::uintptr_t*>(target + kDevWarpRootSlot);
    const std::uintptr_t obj = IsGuestPointer(root)
        ? *reinterpret_cast<const std::uintptr_t*>(root) : 0;
    const std::uintptr_t sceneBase = IsGuestPointer(obj)
        ? *reinterpret_cast<const std::uintptr_t*>(obj + 0x48) : 0;
    Logging.Log("[GalaxyCoop:smg1] DIAG-0152 %s root=0x%lx obj=0x%lx scene=0x%lx spawnscene=0x%lx chain=<%s> spawn=<%s> kill=1",
                tag,
                static_cast<unsigned long>(root),
                static_cast<unsigned long>(obj),
                static_cast<unsigned long>(sceneBase),
                static_cast<unsigned long>(sLastSpawnScene),
                DevWarpReadStageName(),
                DevWarpReadStageNameFromScene(sLastSpawnScene));
}

// DevWarpSequenceReady() removed — only used by exhausted route 2.

bool DevWarpStageEquals(const char* name) {
    std::uintptr_t sceneBase = 0;
    if (!DevWarpReadSceneBase(&sceneBase)) return false;
    char current[64];
    if (!DevWarpCopyInlineName(reinterpret_cast<const char*>(sceneBase + 0x20), current)) return false;
    return DevWarpAsciiEqualFold(current, name);
}

}  // namespace

void LogWarpTopSnapshot(const char* tag) {
    if (!kDevWarpEnabled) return;
    if (sTopSnapCount >= 4) return;
    ++sTopSnapCount;
    Logging.Log("[GalaxyCoop:smg1] DIAG-0153 topsnap %s fired=%d pair=%d hubcount=%u self=%p kill=1",
                tag,
                sDevWarpFired ? 1 : 0,
                HasActivePlayerPair() ? 1 : 0,
                static_cast<unsigned>(sDiag0151HubCount),
                reinterpret_cast<const void*>(&sDevWarpFired));
}

void TryDevBootWarp() {
    if (!kDevWarpEnabled) return;
    TickDevWarpSelect();
    LogWarpTopSnapshot("warp");
    if (sWarpHeartbeat < kWarpHeartbeatLimit) {
        ++sWarpHeartbeat;
        Logging.Log("[GalaxyCoop:smg1] DIAG-0153 warp heartbeat n=%u fired=%d pair=%d hubcount=%u kill=1",
                    static_cast<unsigned>(sWarpHeartbeat),
                    sDevWarpFired ? 1 : 0,
                    HasActivePlayerPair() ? 1 : 0,
                    static_cast<unsigned>(sDiag0151HubCount));
    }
    if (sDevWarpFired) {
        if (!sDevWarpArrivalLogged && DevWarpStageEquals(DevWarpDestinationEntry().expected)) {
            sDevWarpArrivalLogged = true;
            Logging.Log("[GalaxyCoop:smg1] PATCH-0150 arrival stage=%s powerup=%s observable=dev_warp_landed kill=1",
                        DevWarpDestinationEntry().expected,
                        DevWarpDestinationEntry().powerUp);
        }
        return;
    }
    if (!HasActivePlayerPair()) return;
    if (sDiag0151HubCount < kDevWarpLogLimit) {
        ++sDiag0151HubCount;
        Logging.Log("[GalaxyCoop:smg1] DIAG-0151 hub stage=<%s> fired=%d armed=%d kill=1",
                    DevWarpReadStageName(), sDevWarpFired ? 1 : 0, sWarpArmed ? 1 : 0);
        DevWarpDumpChain("hub");
    }
    // Route 2 per-frame execute removed (exhausted, seq=0 every frame).
    // Fill hijack v2 fires from the observer callback, not per-frame.
}

void NotifyStageSpawnWarp(std::uintptr_t scene) {
    // Per-scene budgets: one crowded encounter must not blind the next stage.
    sDiag0187Logs=0;sJoinDeclineLogs=0;sDiag0188Logs=0;sDiag0190Logs=0;sDiag0190SeenCount=0;sStaleLeaderLogs=0;sDomeCompanionLogs=0;
    sLaunchStarLogs=0;sPairLogs=0;sPipeBeginLogs=0;sPipeExitLogs=0;
    // ATT-0223: actors of the old stage are gone; start every new one on Mario.
    std::fill(std::begin(sNearerChoices), std::end(sNearerChoices), NearerChoice{});
    if (!kDevWarpEnabled) return;
    if (IsGuestPointer(scene)) sLastSpawnScene = scene;
    if (sDiag0151SpawnCount < kDevWarpLogLimit) {
        ++sDiag0151SpawnCount;
        Logging.Log("[GalaxyCoop:smg1] DIAG-0151 spawn stage=<%s> fired=%d kill=1",
                    DevWarpReadStageName(), sDevWarpFired ? 1 : 0);
        DevWarpDumpChain("spawn");
    }
    const bool inEgg = DevWarpStageEquals("EggStarGalaxy");
    const bool inDoor = DevWarpStageEquals("HeavensDoorGalaxy");
    const bool inDome = DevWarpStageEquals("AstroDome");
    const bool inHub = DevWarpStageEquals("AstroGalaxy");
    const bool inFileSelect = DevWarpStageEquals("FileSelect");
    if (sDevWarpLogCount < kDevWarpLogLimit) {
        ++sDevWarpLogCount;
        Logging.Log("[GalaxyCoop:smg1] DIAG-0150 stage spawn fired=%d egg=%d door=%d dome=%d hub=%d fsel=%d sample=%u/%u kill=1",
                    sDevWarpFired ? 1 : 0, inEgg ? 1 : 0, inDoor ? 1 : 0, inDome ? 1 : 0, inHub ? 1 : 0, inFileSelect ? 1 : 0,
                    static_cast<unsigned>(sDevWarpLogCount),
                    static_cast<unsigned>(kDevWarpLogLimit));
    }
    if (sDevWarpFired && !sDevWarpArrivalLogged && DevWarpStageEquals(DevWarpDestinationEntry().expected)) {
        sDevWarpArrivalLogged = true;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0150 arrival stage=%s powerup=%s observable=dev_warp_landed kill=1",
                    DevWarpDestinationEntry().expected,
                    DevWarpDestinationEntry().powerUp);
    }
    // Arm from FileSelect (zero-interaction warp: catches disc=7 hub trigger),
    // or from hub/dome/door (fallback: catches disc=0 dome entry).
    if (kDevWarpEnabled && !sDevWarpFired && !sWarpArmed && (inDoor || inDome || inHub || inFileSelect)) {
        sWarpArmed = true;
        Logging.Log("[GalaxyCoop:smg1] DIAG-0150 warp armed from %s kill=1",
                    inFileSelect ? "FileSelect" : (inHub ? "AstroGalaxy" : (inDome ? "AstroDome" : "HeavensDoorGalaxy")));
    }
    // Route 2 (self-fill+execute) removed: exhausted (EVD-0165-0168, seq=0
    // every frame). Self-fill also triggered our observer as a spurious n=3
    // that would consume the one-shot warp before the real transition.
}

// Generic-loader override (EVD-0174): 0x1B6040 fills disc 0 plus caller
// stage, scenario, extra then executes, with a fully relocatable opening
// (sub/stp/add plus register movs only, zero PC-relative words) and callers
// on transition paths. Unlike the dispatcher hook it only ever runs when
// native code is already loading somewhere, and arming gates it to post-hub
// gameplay, so boot behavior is untouched.
constexpr std::uintptr_t kLoaderOverrideEntry = 0x1B6040;
// Re-enabled by the pool fix (EVD-0176): the boot kills were the 22nd-hook
// allocator abort, not the entries. Dispatcher hook stays off.
constexpr bool kLoaderOverrideEnabled = true;

HOOK_DEFINE_TRAMPOLINE(Patch0150LoaderOverride) {
    static void Callback(std::uintptr_t stage, std::uint32_t scenario, std::uintptr_t extra) {
        if (kDevWarpEnabled && kDevWarpFireEnabled && sWarpArmed && !sDevWarpFired) {
            const char* destination = DevWarpDestinationString();
            if (destination != nullptr) {
                sDevWarpFired = true;
                Logging.Log("[GalaxyCoop:smg1] PATCH-0150 loader override stage=%p scenario=%u -> %s(%u) powerup=%s kill=1",
                            reinterpret_cast<const void*>(stage),
                            static_cast<unsigned>(scenario),
                            DevWarpDestinationEntry().expected,
                            static_cast<unsigned>(DevWarpDestinationScenario()),
                            DevWarpDestinationEntry().powerUp);
                Orig(reinterpret_cast<std::uintptr_t>(destination),
                     DevWarpDestinationScenario(), extra);
                return;
            }
        }
        Orig(stage, scenario, extra);
    }
};
constexpr std::uintptr_t kStructHijackDispatcher = 0x22F920;
constexpr std::uint32_t kStructHijackDisc = 1;
// REVERTED by boot crash (EVD-0170): the trampoline at the dispatcher entry
// kills launch outright, so the install stays off until the entry mismatch
// is root-caused. All hijack behavior below is dormant while this is false.
constexpr bool kStructHijackEnabled = false;

HOOK_DEFINE_TRAMPOLINE(Patch0150StructHijack) {
    static void Callback(std::uintptr_t seq, std::uintptr_t req, std::uintptr_t flag) {
        if (kDevWarpEnabled && kDevWarpFireEnabled && sWarpArmed && !sDevWarpFired &&
            IsGuestPointer(req)) {
            const std::uint32_t inDisc = *reinterpret_cast<const std::uint32_t*>(req);
            if (inDisc != 8) {
                const char* destination = DevWarpDestinationString();
                if (destination != nullptr) {
                    *reinterpret_cast<std::uint32_t*>(req) = kStructHijackDisc;
                    *reinterpret_cast<const char**>(req + 8) = destination;
                    *reinterpret_cast<std::uint32_t*>(req + 0x10) = DevWarpDestinationScenario();
                    sDevWarpFired = true;
                    Logging.Log("[GalaxyCoop:smg1] PATCH-0150 struct hijack disc=%u->1 %s(%u) powerup=%s kill=1",
                                static_cast<unsigned>(inDisc),
                                DevWarpDestinationEntry().expected,
                                static_cast<unsigned>(DevWarpDestinationScenario()),
                                DevWarpDestinationEntry().powerUp);
                }
            }
        }
        Orig(seq, req, flag);
    }
};

// Fill observer (EVD-0177): 0x21D33C entry is hook-clean (stp/str/mov
// frame, zero PC-relative words in the opening) and every native transition
// request passes through it, so logging its args maps the exact live shapes
// (disc, stage, scenario, extra) with zero behavior change: pure observe,
// always Orig with untouched args.
constexpr std::uintptr_t kTransObserveFill = 0x21D33C;
constexpr std::uint32_t kTransObserveLimit = 12;
std::uint32_t sTransObserveCount = 0;

HOOK_DEFINE_TRAMPOLINE(Patch0150TransObserve) {
    static void Callback(std::uintptr_t out, std::uint32_t disc, std::uintptr_t stage,
                         std::uint32_t scenario, std::uintptr_t extra) {
        if (kDevWarpEnabled && sTransObserveCount < kTransObserveLimit) {
            ++sTransObserveCount;
            char stageName[64];
            const char* stageText = "<null>";
            if (stage != 0 && IsGuestPointer(stage) &&
                DevWarpCopyInlineName(reinterpret_cast<const char*>(stage), stageName)) {
                stageText = stageName;
            } else if (stage != 0) {
                stageText = "<unreadable>";
            }
            Logging.Log("[GalaxyCoop:smg1] DIAG-0160 trans observe n=%u disc=%u stage=%p <%s> scenario=%u extra=%p kill=1",
                        static_cast<unsigned>(sTransObserveCount),
                        static_cast<unsigned>(disc),
                        reinterpret_cast<const void*>(stage),
                        stageText,
                        static_cast<unsigned>(scenario),
                        reinterpret_cast<const void*>(extra));
        }
        Orig(out, disc, stage, scenario, extra);
        // Fill hijack v2 (EVD-0180): fire on the first non-menu fill after
        // arming. The old name-check approach failed because the stage field
        // in the fill buffer is NOT a const char* — all observed pointers
        // (0x8071a73f, 0x80748e55, 0x12ed666f6) were unreadable as ASCII.
        // Disc 8 is the menu path (FileSelect); everything else is a game
        // transition. One hijack per boot; disc 8 never touched.
        if (kDevWarpEnabled && kDevWarpFireEnabled && sWarpArmed && !sDevWarpFired &&
            IsGuestPointer(out)) {
            const std::uint32_t filledDisc = *reinterpret_cast<const std::uint32_t*>(out);
            if (filledDisc != 8) {
                const char* destination = DevWarpDestinationString();
                if (destination != nullptr) {
                    Logging.Log("[GalaxyCoop:smg1] PATCH-0150 fill hijack disc=%u->2 %s(%u) powerup=%s kill=1",
                                static_cast<unsigned>(filledDisc),
                                DevWarpDestinationEntry().expected,
                                static_cast<unsigned>(DevWarpDestinationScenario()),
                                DevWarpDestinationEntry().powerUp);
                    *reinterpret_cast<std::uint32_t*>(out) = 2;
                    *reinterpret_cast<const char**>(out + 8) = destination;
                    *reinterpret_cast<std::uint32_t*>(out + 0x10) = DevWarpDestinationScenario();
                    // PATCH-0185 / EVD-0240: request +0x18 carries the 8-byte
                    // start id the stage restart fills (default {0,0}).
                    // Boo: TeresaHouseZone start MarioNo 141; zone 1 assumes
                    // StageObjInfo order after the main zone (unverified).
                    const auto& entry = DevWarpDestinationEntry();
                    if (kPatch0185StartIdEnabled && entry.startNo >= 0) {
                        auto* id = reinterpret_cast<std::int32_t*>(out + 0x18);
                        Logging.Log("[GalaxyCoop:smg1] PATCH-0185 start id (%d,%d) -> (%d,%d) kill=1",
                                    id[0], id[1], entry.startNo, entry.startZone);
                        id[0] = entry.startNo;
                        id[1] = entry.startZone;
                    }
                    sDevWarpFired = true;
                }
            }
        }
    }
};

void InstallWarp(const galaxy::target::Identity& identity) {
    if (identity.game == galaxy::target::Game::Smg1 && kPatch0161PipeRoutingEnabled) {
        Patch0161PipeReceive::InstallAtOffset(kPipeReceiveOtherMsg);
        Patch0161PipeMovement::InstallAtOffset(kPipeMovement);
        if (kAtt0216DemoLockOnMarioEnabled) {
            Att0216DemoStart::InstallAtOffset(kPlayerDemoStart);
            Att0216InputLock::InstallAtOffset(kPlayerInputLock);
            Att0216DemoEnd::InstallAtOffset(kPlayerDemoEnd);
            Logging.Log("[GalaxyCoop:smg1] ATT-0216 player locks on Mario installed start=0x%lx lock=0x%lx end=0x%lx kill=1",
                static_cast<unsigned long>(kPlayerDemoStart), static_cast<unsigned long>(kPlayerInputLock),
                static_cast<unsigned long>(kPlayerDemoEnd));
        }
        if (kAtt0224StatusCloseGuardEnabled) {
            Att0224StatusClose::InstallAtOffset(kStatusClose);
            Logging.Log("[GalaxyCoop:smg1] ATT-0225 status close guard installed close=0x%lx head=+0x%lx type=+0x%lx kill=1",
                static_cast<unsigned long>(kStatusClose), static_cast<unsigned long>(kStatusHead),
                static_cast<unsigned long>(kStatusType));
        }
        if (kPipeCompanionEnabled) {
            // EVD0198: all four entries begin with16B of stack/register saves.
            Patch0167PipeAnimation::InstallAtOffset(kPipeStartAnimation);
            Patch0167PipeJumpAnimation::InstallAtOffset(kPipeJumpAnimation);
            Patch0167PipeVisibility::InstallAtOffset(kPipeVisibility);
            Patch0167PipeRelease::InstallAtOffset(kPipeEndRush);
            Logging.Log("[GalaxyCoop:smg1] PATCH-0167 paired native pipe installed exit_bubble=0 kill=1");
            if (kDiag0188BeginRushTraceEnabled || kPatch0189DomeStarCompanionEnabled) {
                // EVD-0248: 0x11BEB4 opens stp/stp/mov/ldr, no PC-relative form.
                Patch0189BeginRush::InstallAtOffset(kPipeBeginRush);
                Logging.Log("[GalaxyCoop:smg1] PATCH-0189 dome star companion + DIAG-0188 begin rush trace armed dome_vtable=0x%lx kill=1",
                    static_cast<unsigned long>(kAstroDomeBlueStarVtable));
            }
            if (kDiag0190DomeStarTraceEnabled) {
                Diag0190DomeStarReceive::InstallAtOffset(kAstroDomeBlueStarReceiveOtherMsg);
                Logging.Log("[GalaxyCoop:smg1] DIAG-0190 dome star message trace armed recv=0x%lx kill=0",
                    static_cast<unsigned long>(kAstroDomeBlueStarReceiveOtherMsg));
            }
            if (kPatch0186LaunchStarPairEnabled) {
                // EVD-0245: 0x1CBAF4 opens with stp/str/mov/sub (no PC-relative).
                Patch0186LaunchStarReceive::InstallAtOffset(kLaunchStarReceiveOtherMsg);
                if (kAtt0215SlingStarPairEnabled) {
                    Att0215SlingStarReceive::InstallAtOffset(kSlingStarReceiveOtherMsg);
                    Logging.Log("[GalaxyCoop:smg1] ATT-0215 paired sling star installed vtable=0x%lx receive=0x%lx kill=1",
                        static_cast<unsigned long>(kSlingStarVtable), static_cast<unsigned long>(kSlingStarReceiveOtherMsg));
                }
                Logging.Log("[GalaxyCoop:smg1] PATCH-0186 paired launch star installed kill=1");
            }
        }
        Logging.Log("[GalaxyCoop:smg1] PATCH-0161 green pipe participant routing installed kill=1");
        Logging.Log("[GalaxyCoop:smg1] ATT-0223 nearer player for all actors enabled=%d listed=%u excluded=%u take_over=0.85 kill=1",
                    kAtt0223GlobalNearerEnabled ? 1 : 0, static_cast<unsigned>(std::size(kNearerPlayerVtables)),
                    static_cast<unsigned>(std::size(kNearerExcludedVtables)));
    }
    Logging.Log("[GalaxyCoop:%s] Warp recovery inactive (mid-function hook 0x2649B0 retired)",
                galaxy::target::GameName(identity.game));
    if (identity.game == galaxy::target::Game::Smg1 && kStructHijackEnabled) {
        Patch0150StructHijack::InstallAtOffset(kStructHijackDispatcher);
        Logging.Log("[GalaxyCoop:%s] PATCH-0150 struct hijack installed dispatcher=0x%lx kill=1",
                    galaxy::target::GameName(identity.game),
                    static_cast<unsigned long>(kStructHijackDispatcher));
    }
    if (identity.game == galaxy::target::Game::Smg1 && kLoaderOverrideEnabled) {
        Patch0150LoaderOverride::InstallAtOffset(kLoaderOverrideEntry);
        Logging.Log("[GalaxyCoop:%s] PATCH-0150 loader override installed entry=0x%lx kill=1",
                    galaxy::target::GameName(identity.game),
                    static_cast<unsigned long>(kLoaderOverrideEntry));
    }
    if (identity.game == galaxy::target::Game::Smg1 && kDevWarpEnabled) {
        Patch0150TransObserve::InstallAtOffset(kTransObserveFill);
        Logging.Log("[GalaxyCoop:%s] DIAG-0160 trans observe installed fill=0x%lx kill=1",
                    galaxy::target::GameName(identity.game),
                    static_cast<unsigned long>(kTransObserveFill));
        Logging.Log("[GalaxyCoop:%s] PATCH-0150 dev warp rev21 fire=%d dests=%u default=%s(%u) request=0x%lx kill=1",
                    galaxy::target::GameName(identity.game),
                    kDevWarpFireEnabled ? 1 : 0,
                    static_cast<unsigned>(kDevWarpDestinationCount),
                    DevWarpDestinationEntry().expected,
                    static_cast<unsigned>(DevWarpDestinationScenario()),
                    static_cast<unsigned long>(kDevWarpStageRequest));
        Logging.Log("[GalaxyCoop:%s] DIAG-0177 warp select armed p2 Minus+Y=Bee X=Boo B=IceFire A=Spring Left=Rainbow Right=BeeQuick kill=1",
                    galaxy::target::GameName(identity.game));
        Logging.Log("[GalaxyCoop:%s] DIAG-0151 raw stage-name dump armed kill=1",
                    galaxy::target::GameName(identity.game));
    }
}

}  // namespace mod::patches
