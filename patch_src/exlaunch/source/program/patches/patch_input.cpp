#include "patch_input.hpp"
#include "patch_shared.hpp"
#include "patch_actions.hpp"
#include "patch_splitscreen.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <cmath>
#include <cstddef>

namespace mod::patches {
namespace {
// SMG1 main SHA-256 76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// NSO offsets: EVD-0026/0028/0029 and EVD-0185. Preserve native actor input
// checks by calling Orig and routing only the two downstream stick selectors.
constexpr std::uintptr_t kGetStickValue = 0x123654;
constexpr std::uintptr_t kGetStickX = 0x27A5B0;
constexpr std::uintptr_t kGetStickY = 0x27A5E0;
constexpr std::uintptr_t kNpadReaderUpdate = 0x4CEEB4;
constexpr std::uintptr_t kGetNpadStyle = 0x5F96B0;
// EVD-0012: leaf 2-slot player-holder accessor, idx<2, else null.
constexpr std::uintptr_t kPlayerHolderGet = 0x403A0;
constexpr bool kPatch0006Enabled = true; // ATT-0108 kill switch
// PATCH-0175 kill switch: P1's movement stick comes from explicit Npad 0
// instead of the aggregated holder[0] pad, so the P2 controller stops steering
// Mario as well (EVD-0013 recorded both controllers steering P1).
constexpr bool kPatch0175P1ExplicitNpadEnabled = true;
// DIAG-0176 kill switch: read-only probe of both native holder pads and both
// explicit Npad readers, to find where each controller's input actually lands.
// Answered by EVD-0215: holder[0] carries both controllers, holder[1] is never
// fed. Disarmed; re-enable only to re-measure that binding.
constexpr bool kDiag0176InputSourceProbeEnabled = false;

// Owned scratch, never a cached scene/heap pointer. Complete reader layout.
struct alignas(8) NpadReader {
    std::uint64_t unused;
    std::uint32_t id, style, orientation, previousOrientation;
    std::uint32_t buttons, previousButtons, sample, previousSample;
    float leftX, leftY, rightX, rightY;
    std::uint64_t rawButtons;
    std::uint32_t attributes, mode;
};
static_assert(sizeof(NpadReader) == 0x48);
static_assert(offsetof(NpadReader, id) == 0x08);
static_assert(offsetof(NpadReader, leftX) == 0x28);
static_assert(offsetof(NpadReader, mode) == 0x44);
struct StickRoute {
    bool active = false;
    float x = 0.0f, y = 0.0f;
};
StickRoute sRoute;
// PATCH-0019 release probe: while set, the actor stick query sees a full
// synthetic deflection so its native actor-local gates alone decide output.
bool sGateProbe = false;
std::uint32_t sMovementLogs = 0, sStyleLogs = 0;
std::uint32_t sLastStyle[2] = {~0u, ~0u};

float ClampAxis(float value) {
    if (!(value >= -2.0f && value <= 2.0f)) return 0.0f;
    if (value < -1.0f) return -1.0f;
    if (value > 1.0f) return 1.0f;
    return value;
}
// active=false means that Npad is disconnected or of an unsupported style; the
// caller decides whether that means neutral or native fallback.
StickRoute ReadNpadStick(std::uint32_t npadId) {
    StickRoute route{};
    NpadReader reader{};
    reader.id = npadId;
    const auto target = exl::util::modules::GetTargetStart();
    using StyleFn = std::uint32_t (*)(const std::uint32_t*);
    using ReaderFn = void (*)(NpadReader*);
    reader.style = reinterpret_cast<StyleFn>(target + kGetNpadStyle)(&reader.id);
    const std::uint32_t slot = npadId < 2 ? npadId : 0;
    if (reader.style != sLastStyle[slot] && sStyleLogs < 16) {
        sLastStyle[slot] = reader.style;
        ++sStyleLogs;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0006 controller npad=%u style=0x%x connected=%d",
                    npadId, reader.style, (reader.style & 0x1f) != 0 ? 1 : 0);
    }
    if ((reader.style & 0x1f) == 0) return route;
    reinterpret_cast<ReaderFn>(target + kNpadReaderUpdate)(&reader);
    route.active = true;
    route.x = ClampAxis(reader.leftX);
    route.y = ClampAxis(reader.leftY);
    return route;
}
StickRoute ReadP2Stick() {
    const StickRoute route = ReadNpadStick(1);
    // Disconnected/unsupported P2 is neutral, never fallback to P1 input.
    return route.active ? route : StickRoute{true, 0.0f, 0.0f};
}
HOOK_DEFINE_TRAMPOLINE(Patch0006StickX) {
    static float Callback() { return sRoute.active ? sRoute.x : Orig(); }
};
HOOK_DEFINE_TRAMPOLINE(Patch0006StickY) {
    static float Callback() { return sRoute.active ? sRoute.y : Orig(); }
};
HOOK_DEFINE_TRAMPOLINE(Patch0006ActorStick) {
    static void Callback(void* actor, float* x, float* y) {
        // Save/restore synchronous query context for nested calls; no native
        // singleton is mutated. P1 and unrelated actors keep native routing.
        const StickRoute previous = sRoute;
        sRoute = {};
        const bool valid = IsValidMarioActor(actor);
        const bool p2 = valid && actor == GetSecondPlayer();
        const bool p1 = valid && !p2 && actor == GetFirstPlayer();
        if (sGateProbe) sRoute = StickRoute{true, 1.0f, 0.0f};
        else if (p2) sRoute = IsP2Parked() || IsCoopCutsceneHold() ? StickRoute{true, 0.0f, 0.0f} : ReadP2Stick();
        // PATCH-0175: keep P1 on its own controller. An absent Npad 0 (for
        // example handheld mode) falls back to the native aggregated pad.
        else if (p1 && kPatch0175P1ExplicitNpadEnabled && HasActivePlayerPair())
            sRoute = ReadNpadStick(0);
        Orig(actor, x, y);
        sRoute = previous;
        if (p2 && x != nullptr && y != nullptr && sMovementLogs < 8 &&
            (*x * *x + *y * *y > 0.01f)) {
            ++sMovementLogs;
            Logging.Log("[GalaxyCoop:smg1] PATCH-0006 P2 routed npad=1 output=(%.3f,%.3f) sample=%u/8",
                        static_cast<double>(*x), static_cast<double>(*y), sMovementLogs);
        }
    }
};
// PATCH-0176 / EVD-0223: every MarioActor button query (selector 0x11B57C
// cases 3-10 via 0x67080, 0x1489C0, 0x66CF0 ...) reads holder[0] (0x403A0(0))
// button object +0x10: held word +0x8, rising-edge word +0xC. That object is
// fed from one emulated remote bound to the most recently active Npad
// (0x4C994C -> 0x4CAC2C), so both controllers act on both players. Inside
// each MarioActor movement (vtable 0x835830 slot 4, 0xDD974, relocatable
// stack-store opening) the routed bits are replaced by that player's own
// Npad and restored afterwards. Only A is routed: DIAG-0176 measured Npad
// bit 0 -> word 0x800 (EVD-0215 log); other buttons stay native until
// DIAG-0178 maps them.
constexpr std::uintptr_t kMarioActorMovement = 0xDD974;
constexpr bool kPatch0176ActorButtonsEnabled = true;
// DIAG-0178 kill switch: logs Npad raw buttons against holder[0]'s held word
// on each change, to map the remaining Switch buttons to native bits.
constexpr bool kDiag0178ButtonMapProbeEnabled = true;
struct RoutedButton {
    std::uint64_t npadBit;
    std::uint32_t nativeBit;
};
// DIAG-0178 (EVD-0226): Npad bit 0 (A) -> 0x800, bit 1 (B) -> 0x400; both jump.
// DIAG-0178 (EVD-0230): Npad bit 8 (ZL) -> 0x2000 (crouch, ground pound).
constexpr RoutedButton kPatch0176Buttons[] = {
    {1ull << 0, 0x800},  // A: jump, bee hover
    {1ull << 1, 0x400},  // B: jump, bee hover
    {1ull << 8, 0x2000}, // ZL: crouch, ground pound, long jump
};
constexpr std::uint32_t kPatch0176Mask = 0x2C00;
constexpr std::uint32_t kDiag0178LogLimit = 256;
// PATCH-0177 kill switch (EVD-0227, EVD-0228): spin is not in the button word.
// The per-actor swing function 0x66ebc (control 0x66434, inside movement via
// 0x31e58) raises the spin edge actor+0x1630 from 0x67010(0) -> 0x67050 ->
// 0x4cb1e8, i.e. the emulated remote's shake-state int +0xb4 in 1..14, and
// only while actor+0x164c is 0. Selector case 4 / 0x66fd0 read the shake byte
// +0x77, whose hit sets actor+0x164c=10 and so blocks the spin edge. Both live
// on one shared remote (.bss NSO 0xab190a40). +0xb4 counts up 1,2,3.. while
// anyone holds shake (EVD-0229), so a second presser only saw values past 14.
// Inside each movement +0xb4 is replaced by a per-player counter (1 on the
// player's own Y/X press, +1 per movement while held, 0 when released) and a
// player not holding Y/X sees +0x77 as 0.
constexpr bool kPatch0177ActorShakeEnabled = true;
constexpr std::uintptr_t kEmulatedRemote = 0xAB190A40;
constexpr std::uintptr_t kRemoteShakeByte = 0x77;
constexpr std::uintptr_t kRemoteShakeState = 0xB4;
constexpr std::uint64_t kShakeHoldNpad = (1ull << 3) | (1ull << 2); // Y, X
constexpr std::int32_t kShakeCountCap = 60;
std::int32_t sShakeCount[2] = {0, 0};
std::uint32_t sShakeLogs = 0;
std::uint32_t sOwnHeld[2] = {0, 0};
std::uint32_t sButtonLogs = 0;
std::uint64_t sDiag0178Last[2] = {~0ull, ~0ull};
std::uint32_t sDiag0178Logs = 0;

// Buttons only for Handheld/JoyDual/FullKey styles (EVD-0209); false means
// the Npad cannot supply them and the caller keeps native routing.
bool ReadNpadButtons(std::uint32_t npadId, std::uint64_t* raw) {
    NpadReader reader{};
    reader.id = npadId;
    const auto target = exl::util::modules::GetTargetStart();
    using StyleFn = std::uint32_t (*)(const std::uint32_t*);
    using ReaderFn = void (*)(NpadReader*);
    reader.style = reinterpret_cast<StyleFn>(target + kGetNpadStyle)(&reader.id);
    if ((reader.style & 0x7) == 0) return false;
    reinterpret_cast<ReaderFn>(target + kNpadReaderUpdate)(&reader);
    *raw = reader.rawButtons;
    return true;
}
std::uintptr_t HolderZeroButtons() {
    const auto target = exl::util::modules::GetTargetStart();
    using HolderFn = std::uintptr_t (*)(std::uint32_t);
    const std::uintptr_t holder = reinterpret_cast<HolderFn>(target + kPlayerHolderGet)(0);
    if (!IsGuestPointer(holder)) return 0;
    const auto buttons = *reinterpret_cast<const std::uintptr_t*>(holder + 0x10);
    return IsGuestPointer(buttons) ? buttons : 0;
}
void ProbeButtonMap(std::uintptr_t buttons) {
    if (!kDiag0178ButtonMapProbeEnabled || sDiag0178Logs >= kDiag0178LogLimit) return;
    std::uint64_t raw[2] = {0, 0};
    ReadNpadButtons(0, &raw[0]);
    ReadNpadButtons(1, &raw[1]);
    // Low 16 bits are buttons and D-pad; stick-as-button bits are known.
    raw[0] &= 0xFFFF;
    raw[1] &= 0xFFFF;
    if (raw[0] == sDiag0178Last[0] && raw[1] == sDiag0178Last[1]) return;
    sDiag0178Last[0] = raw[0];
    sDiag0178Last[1] = raw[1];
    ++sDiag0178Logs;
    Logging.Log("[GalaxyCoop:smg1] DIAG-0178 raw0=0x%x raw1=0x%x held=0x%x trig=0x%x sample=%u/%u kill=1",
                static_cast<unsigned>(raw[0]), static_cast<unsigned>(raw[1]),
                *reinterpret_cast<const std::uint32_t*>(buttons + 0x8),
                *reinterpret_cast<const std::uint32_t*>(buttons + 0xC), sDiag0178Logs,
                kDiag0178LogLimit);
}
unsigned sMovementSerial[2] = {0, 0};
HOOK_DEFINE_TRAMPOLINE(Patch0176MarioMovement) {
    static void Callback(void* actor) {
        if (actor == GetFirstPlayer()) ++sMovementSerial[0];
        else if (actor == GetSecondPlayer()) ++sMovementSerial[1];
        if (!kPatch0176ActorButtonsEnabled || !HasActivePlayerPair() || !IsValidMarioActor(actor)) {
            Orig(actor);
            return;
        }
        const bool p2 = actor == GetSecondPlayer();
        if (!p2 && actor != GetFirstPlayer()) {
            Orig(actor);
            return;
        }
        const std::uintptr_t buttons = HolderZeroButtons();
        if (buttons == 0) {
            Orig(actor);
            return;
        }
        if (!p2) ProbeButtonMap(buttons);
        const std::uint32_t index = p2 ? 1 : 0;
        std::uint64_t raw = 0;
        // Same fallback as PATCH-0175: P1 without a button-capable Npad 0
        // keeps native input; P2 without Npad 1 is neutral, never P1's.
        if (!ReadNpadButtons(index, &raw)) {
            if (!p2) {
                Orig(actor);
                return;
            }
            raw = 0;
        }
        if (p2 && (IsP2Parked() || IsCoopCutsceneHold())) raw = 0;
        std::uint32_t own = 0;
        for (const auto& button : kPatch0176Buttons)
            if ((raw & button.npadBit) != 0) own |= button.nativeBit;
        const std::uint32_t rising = own & ~sOwnHeld[index];
        sOwnHeld[index] = own;

        const bool shakeHeld = (raw & kShakeHoldNpad) != 0;
        std::int32_t& count = sShakeCount[index];
        count = !shakeHeld ? 0 : count < kShakeCountCap ? count + 1 : kShakeCountCap;
        const bool shakePulse = count == 1;

        auto* held = reinterpret_cast<std::uint32_t*>(buttons + 0x8);
        auto* trig = reinterpret_cast<std::uint32_t*>(buttons + 0xC);
        const auto remote = exl::util::modules::GetTargetStart() + kEmulatedRemote;
        auto* shake = reinterpret_cast<std::uint8_t*>(remote + kRemoteShakeByte);
        auto* shakeState = reinterpret_cast<std::int32_t*>(remote + kRemoteShakeState);
        const std::uint32_t savedHeld = *held, savedTrig = *trig;
        const std::uint8_t savedShake = *shake;
        const std::int32_t savedState = *shakeState;
        *held = (savedHeld & ~kPatch0176Mask) | own;
        *trig = (savedTrig & ~kPatch0176Mask) | rising;
        if (kPatch0177ActorShakeEnabled) {
            *shake = shakeHeld ? savedShake : 0;
            *shakeState = count;
        }
        Orig(actor);
        *held = savedHeld;
        *trig = savedTrig;
        if (kPatch0177ActorShakeEnabled) {
            *shake = savedShake;
            *shakeState = savedState;
        }
        if (rising != 0 && sButtonLogs < 16) {
            ++sButtonLogs;
            Logging.Log("[GalaxyCoop:smg1] PATCH-0176 p%u own button edge=0x%x native_held=0x%x sample=%u/16 kill=1",
                        index + 1, rising, savedHeld, sButtonLogs);
        }
        if (kPatch0177ActorShakeEnabled && shakePulse && sShakeLogs < 24) {
            ++sShakeLogs;
            Logging.Log("[GalaxyCoop:smg1] PATCH-0177 p%u shake own_start=%d native_state=%d byte=%u own_held=%d sample=%u/24 kill=1",
                        index + 1, shakePulse ? 1 : 0, savedState, savedShake, shakeHeld ? 1 : 0, sShakeLogs);
        }
    }
};
// DIAG-0176 state: last logged snapshot of both native pads and both Npads.
struct InputProbe {
    std::uint32_t signature = 0;
    std::uint32_t logs = 0;
    bool holderLogged = false;
};
InputProbe sProbe;
constexpr std::uint32_t kDiag0176LogLimit = 48;

// ATT-0204 / EVD-0497, EVD-0498, EVD-0499: DE438 (called from movement DD974
// and 1F5428, x0 = MarioActor) caches the camera eye +16C0, right +16CC, up
// +16D8 and forward +16E4 (previous forward to +16F0) from the main camera's
// inverse view (context 0x17 via 1B9F8: columns 0/1/-2 and translation), and
// flags a cut +16BC when forward turns >= 30 deg (108E10) or the eye moves
// over 500, then calls E160C(mario, 0), 56214(actor, 1) and clears u16 +15C8.
// Stick conversion 1237F4 reads these axes (EVD-0298), so in a split P2 steered
// by P1's half. While P2's half is at least half revealed the same writes are
// replayed for P2 from the view that half was drawn with; the basis switch is
// itself a native cut, so a held direction carries over. Entry: relocatable
// 16-byte opening, no branches into bytes 4..15, callers DD974/1F5428
// (ghidra_p2_split_input_hook_branches_20261023).
constexpr std::uintptr_t kMarioActorCameraAxes = 0xDE438;
constexpr std::uintptr_t kVectorAngle = 0x108E10;
constexpr std::uintptr_t kMarioCameraCut = 0xE160C;
constexpr std::uintptr_t kActorCameraCut = 0x56214;
constexpr bool kAtt0204P2SplitInputBasisEnabled = true;
constexpr float kCameraCutAngle = 0.5235988f;
constexpr float kCameraCutDistance = 500.0f;
bool sAtt0204Split = false;
std::uint32_t sAtt0204Logs = 0;

bool UnitRow(const float* view, int row, float sign, float* out) {
    const float x = view[row * 4], y = view[row * 4 + 1], z = view[row * 4 + 2];
    const float length = std::sqrt(x * x + y * y + z * z);
    if (!std::isfinite(length) || length < 1.0e-4f) return false;
    out[0] = sign * x / length; out[1] = sign * y / length; out[2] = sign * z / length;
    return true;
}
HOOK_DEFINE_TRAMPOLINE(Att0204P2CameraAxes) {
    static void Callback(void* actor) {
        float view[12];
        const bool split = kAtt0204P2SplitInputBasisEnabled && HasActivePlayerPair() &&
                           IsValidMarioActor(actor) && actor == GetSecondPlayer() &&
                           !IsP2Parked() && GetCoopP2InputView(view);
        const auto base = reinterpret_cast<std::uintptr_t>(actor);
        const auto mario = split ? *reinterpret_cast<const std::uintptr_t*>(base + 0x3C0) : 0;
        float right[3], up[3], forward[3];
        if (!split || !IsGuestPointer(mario) || !UnitRow(view, 0, 1.0f, right) ||
            !UnitRow(view, 1, 1.0f, up) || !UnitRow(view, 2, -1.0f, forward)) {
            if (sAtt0204Split && actor == GetSecondPlayer()) {
                sAtt0204Split = false;
                if (sAtt0204Logs++ < 64) Logging.Log("[GalaxyCoop:smg1] ATT-0204 P2 input basis=main kill=1");
            }
            Orig(actor);
            return;
        }
        // Eye of a rigid view: -(R^T t).
        float eye[3];
        for (int c = 0; c < 3; ++c)
            eye[c] = -(view[c] * view[3] + view[4 + c] * view[7] + view[8 + c] * view[11]);
        auto* cached = reinterpret_cast<float*>(base + 0x16C0);  // eye, right, up, forward, old forward
        using AngleFn = float (*)(const float*, const float*);
        const float turn = reinterpret_cast<AngleFn>(exl::util::modules::GetTargetStart() + kVectorAngle)(cached + 9, forward);
        const float dx = cached[0] - eye[0], dy = cached[1] - eye[1], dz = cached[2] - eye[2];
        const bool cut = turn >= kCameraCutAngle || std::sqrt(dx * dx + dy * dy + dz * dz) > kCameraCutDistance;
        *reinterpret_cast<std::uint8_t*>(base + 0x16BC) = cut ? 1 : 0;
        if (cut) {
            const auto target = exl::util::modules::GetTargetStart();
            reinterpret_cast<void (*)(std::uintptr_t, int)>(target + kMarioCameraCut)(mario, 0);
            reinterpret_cast<void (*)(void*, int)>(target + kActorCameraCut)(actor, 1);
            *reinterpret_cast<std::uint16_t*>(base + 0x15C8) = 0;
        }
        for (int i = 0; i < 3; ++i) {
            cached[i] = eye[i];
            cached[3 + i] = right[i];
            cached[6 + i] = up[i];
            cached[12 + i] = cached[9 + i];
            cached[9 + i] = forward[i];
        }
        if (!sAtt0204Split) {
            sAtt0204Split = true;
            if (sAtt0204Logs++ < 64)
                Logging.Log("[GalaxyCoop:smg1] ATT-0204 P2 input basis=split cut=%d turn=%.1f kill=1",
                            cut ? 1 : 0, static_cast<double>(turn * 57.29578f));
        }
    }
};
} // namespace
unsigned GetMarioMovementSerial(int index) { return index == 0 || index == 1 ? sMovementSerial[index] : 0; }
std::uint64_t ReadNpadRawButtons(std::uint32_t npadId) {
    // 0x4CEEB4 copies the nn::hid NpadButton set to +0x38 for Handheld,
    // JoyDual and FullKey styles (EVD-0209). Single Joy-Con styles remap it,
    // so they report no buttons.
    NpadReader reader{};
    reader.id = npadId;
    const auto target = exl::util::modules::GetTargetStart();
    using StyleFn = std::uint32_t (*)(const std::uint32_t*);
    using ReaderFn = void (*)(NpadReader*);
    reader.style = reinterpret_cast<StyleFn>(target + kGetNpadStyle)(&reader.id);
    if ((reader.style & 0x7) == 0) return 0;
    reinterpret_cast<ReaderFn>(target + kNpadReaderUpdate)(&reader);
    return reader.rawButtons;
}
bool IsNativeMovementGateOpen(void* actor) {
    // Without the PATCH-0006 actor stick hook the synthetic route never
    // applies; report closed so the caller keeps its real-P1-stick release.
    if (!kPatch0006Enabled || !IsValidMarioActor(actor)) return false;
    const auto mario = *reinterpret_cast<const std::uintptr_t*>(
        reinterpret_cast<std::uintptr_t>(actor) + 0x3C0);
    if (!IsGuestPointer(mario)) return false;
    float x = 0.0f, y = 0.0f;
    sGateProbe = true;
    using GetStickFn = void (*)(void*, float*, float*);
    reinterpret_cast<GetStickFn>(exl::util::modules::GetTargetStart() + kGetStickValue)(actor, &x, &y);
    sGateProbe = false;
    return x * x + y * y > 0.01f;
}
std::uint32_t ActivePlayerInputMask() {
    // A, B, X, Y, ZL; the stick uses the established 0.1 movement threshold.
    constexpr std::uint64_t kPlayButtons = 0x10F;
    std::uint32_t mask = 0;
    for (std::uint32_t npad = 0; npad < 2; ++npad) {
        const StickRoute stick = ReadNpadStick(npad);
        if ((stick.active && stick.x * stick.x + stick.y * stick.y > 0.01f) ||
            (ReadNpadRawButtons(npad) & kPlayButtons) != 0)
            mask |= 1u << npad;
    }
    return mask;
}
// DIAG-0176: one falsifiable question - which native player-holder pad carries
// which physical controller? Read-only: holder[i] (0x403A0, EVD-0012), its
// button word at [holder+0x10]+0x8 (0x1152E0 family opening) and its stick at
// [holder+0x40]+0x8/+0xc (EVD-0026), against explicit Npad 0 and 1.
void TickDiag0176InputSources() {
    if (!kDiag0176InputSourceProbeEnabled || sProbe.logs >= kDiag0176LogLimit) return;
    const auto target = exl::util::modules::GetTargetStart();
    using HolderFn = std::uintptr_t (*)(std::uint32_t);
    const auto holderGet = reinterpret_cast<HolderFn>(target + kPlayerHolderGet);

    std::uintptr_t holder[2] = {0, 0};
    std::uint32_t padButtons[2] = {0, 0};
    float padX[2] = {0.0f, 0.0f}, padY[2] = {0.0f, 0.0f};
    int padValid[2] = {0, 0};
    for (std::uint32_t i = 0; i < 2; ++i) {
        holder[i] = holderGet(i);
        if (!IsGuestPointer(holder[i])) continue;
        const auto buttonState = *reinterpret_cast<std::uintptr_t*>(holder[i] + 0x10);
        const auto stickState = *reinterpret_cast<std::uintptr_t*>(holder[i] + 0x40);
        if (!IsGuestPointer(buttonState) || !IsGuestPointer(stickState)) continue;
        padButtons[i] = *reinterpret_cast<std::uint32_t*>(buttonState + 0x08);
        padX[i] = *reinterpret_cast<float*>(stickState + 0x08);
        padY[i] = *reinterpret_cast<float*>(stickState + 0x0C);
        padValid[i] = 1;
    }
    if (!sProbe.holderLogged) {
        sProbe.holderLogged = true;
        Logging.Log("[GalaxyCoop:smg1] DIAG-0176 holders h0=%p valid=%d h1=%p valid=%d kill=1",
                    reinterpret_cast<void*>(holder[0]), padValid[0],
                    reinterpret_cast<void*>(holder[1]), padValid[1]);
    }

    const StickRoute npad0 = ReadNpadStick(0);
    const StickRoute npad1 = ReadNpadStick(1);
    const auto raw0 = static_cast<std::uint32_t>(ReadNpadRawButtons(0));
    const auto raw1 = static_cast<std::uint32_t>(ReadNpadRawButtons(1));

    auto bucket = [](float v) -> std::uint32_t {
        const int q = static_cast<int>(v * 4.0f);
        return static_cast<std::uint32_t>((q < -4 ? -4 : q > 4 ? 4 : q) + 4);
    };
    const std::uint32_t signature =
        (padButtons[0] * 2654435761u) ^ (padButtons[1] * 40503u) ^ (raw0 * 2246822519u) ^
        (raw1 * 374761393u) ^ (bucket(padX[0]) << 1) ^ (bucket(padY[0]) << 5) ^
        (bucket(padX[1]) << 9) ^ (bucket(npad0.x) << 13) ^ (bucket(npad1.x) << 17);
    if (signature == sProbe.signature) return;
    sProbe.signature = signature;
    ++sProbe.logs;
    Logging.Log("[GalaxyCoop:smg1] DIAG-0176 pad0=(%.2f,%.2f) btn0=0x%x valid0=%d "
                "pad1=(%.2f,%.2f) btn1=0x%x valid1=%d npad0=(%.2f,%.2f) raw0=0x%x "
                "npad1=(%.2f,%.2f) raw1=0x%x sample=%u/%u kill=1",
                static_cast<double>(padX[0]), static_cast<double>(padY[0]), padButtons[0], padValid[0],
                static_cast<double>(padX[1]), static_cast<double>(padY[1]), padButtons[1], padValid[1],
                static_cast<double>(npad0.x), static_cast<double>(npad0.y), raw0,
                static_cast<double>(npad1.x), static_cast<double>(npad1.y), raw1,
                sProbe.logs, kDiag0176LogLimit);
}
void InstallInput(const galaxy::target::Identity& identity) {
    if (identity.game != galaxy::target::Game::Smg1 || !kPatch0006Enabled) return;
    // Hook proven entries, never the selector+0xC BL. The nx64 relocator
    // handles the contained BL in each selector opening (EVD-0185).
    Patch0006StickX::InstallAtOffset(kGetStickX);
    Patch0006StickY::InstallAtOffset(kGetStickY);
    Patch0006ActorStick::InstallAtOffset(kGetStickValue);
    if (kPatch0176ActorButtonsEnabled || kDiag0178ButtonMapProbeEnabled)
        Patch0176MarioMovement::InstallAtOffset(kMarioActorMovement);
    if (kAtt0204P2SplitInputBasisEnabled) Att0204P2CameraAxes::InstallAtOffset(kMarioActorCameraAxes);
    Logging.Log("[GalaxyCoop:smg1] ATT-0204 P2 split input basis installed axes=0x%lx reveal=0.5 enabled=%d kill=1",
                static_cast<unsigned long>(kMarioActorCameraAxes), kAtt0204P2SplitInputBasisEnabled ? 1 : 0);
    Logging.Log("[GalaxyCoop:smg1] PATCH-0176 actor-local buttons installed movement=0x%lx routed=A,B,ZL enabled=%d shake0177=%d diag0178=%d kill=1",
                static_cast<unsigned long>(kMarioActorMovement),
                kPatch0176ActorButtonsEnabled ? 1 : 0, kPatch0177ActorShakeEnabled ? 1 : 0,
                kDiag0178ButtonMapProbeEnabled ? 1 : 0);
    Logging.Log("[GalaxyCoop:smg1] PATCH-0006 independent analog installed P1=%s P2=npad1 native_gates=1 parking=1 kill=1",
                kPatch0175P1ExplicitNpadEnabled ? "npad0" : "native");
    Logging.Log("[GalaxyCoop:smg1] DIAG-0176 input source probe armed holder=0x%lx kill=%d",
                static_cast<unsigned long>(kPlayerHolderGet), kDiag0176InputSourceProbeEnabled ? 1 : 0);
}
} // namespace mod::patches
