#include "patch_hud.hpp"
#include "patch_lives.hpp"
#include "patch_actions.hpp"
#include "patch_shared.hpp"
#include "patch_spawning.hpp"
#include "patch_warp.hpp"
#include "patch_respawn.hpp"
#include "patch_input.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"

namespace mod::patches {

namespace {

constexpr std::uintptr_t kPatch0065SelectorInit = 0x221EE8;
constexpr std::uintptr_t kPatch0065SelectorRequestDamage = 0x2F7E78;
constexpr std::uintptr_t kPatch0065Allocator = 0xF1CB0;
constexpr std::uintptr_t kPatch0065MeterConstructor = 0x221F58;
constexpr std::uintptr_t kPatch0065InitWithoutIter = 0xD9384;
constexpr std::uintptr_t kPatch0065SetNerve = 0x1188C0;
constexpr std::uintptr_t kPatch0065SetTextBoxNumber = 0x6AB40;

// Stage 2: Dual-Widget Visible Lives HUD (PlayerLeft)
constexpr std::uintptr_t kPatch0067CounterLayoutInit = 0x218968;
constexpr std::uintptr_t kPatch0067PlayerLeftConstructor = 0x218A40;
constexpr std::uintptr_t kPatch0067StartPaneAnim = 0x131490;
constexpr std::uintptr_t kPatch0067PlayerLeftControl = 0x284A50;
constexpr std::uintptr_t kPatch0067PlayerLeftAppear = 0x2849EC;
constexpr std::uintptr_t kPatch0067CounterLayoutDisappear = 0x157390;
constexpr std::uintptr_t kPatch0067PlayerLeftDisappear = 0x284A40;
// PATCH-0163 / EVD-0194, SMG1 main SHA-256
// 76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// Wait compares its cached count against the global life getter twice. Scope
// that getter to P2 only while P2's native wait executor runs.
constexpr bool kPatch0163LifeAnimationOwnerEnabled = true;
constexpr std::uintptr_t kPatch0163PlayerLeftWait = 0x140728; // true actor entry, not wrapper140720
constexpr std::uintptr_t kPatch0163GetPlayerLeft = 0x07E340; // true entry, not thunk7E330
constexpr std::uintptr_t kPatch0163PlayerLeftVtable = 0x8331A0;
bool sPatch0163P2LifeScope = false;
unsigned sPatch0163LifeSelectionLogs = 0;
constexpr std::uintptr_t kPatch0065SelectorNormalMeterField = 0x18;
constexpr std::uintptr_t kPatch0065SelectorSuddenDeathField = 0x20;
constexpr std::uintptr_t kPatch0065LayoutManagerField = 0x18;
constexpr std::uintptr_t kPatch0065LayoutResourceField = 0x08;
constexpr std::uintptr_t kPatch0065RootPaneField = 0x20;
constexpr std::uintptr_t kPatch0065PaneTranslateXField = 0x58;
constexpr std::uintptr_t kPatch0065PaneTranslateYField = 0x5C;
constexpr std::uintptr_t kPatch0065PaneScaleXField = 0x70;
constexpr std::uintptr_t kPatch0065PaneScaleYField = 0x74;
constexpr std::uintptr_t kPatch0065PaneVisibleField = 0xE3;
constexpr std::uintptr_t kPatch0065MeterCountField = 0x68;
constexpr std::uintptr_t kPatch0065MeterAlphaField = 0x74;
constexpr std::uintptr_t kPatch0065CurrentHealthField = 0x528;
constexpr std::size_t kPatch0065MeterSize = 0x78;
constexpr float kPatch0065MeterScale = 0.75f;
constexpr float kPatch0065MoveMeterBaseX = 280.0f;
constexpr float kPatch0065MoveMeterBaseY = 159.0f;
constexpr float kPatch0065P1OffsetY = 25.0f;
constexpr float kPatch0065P2OffsetY = -45.0f;
constexpr std::uint32_t kPatch0065LogLimit = 24;

// Colors (Little-Endian RGBA8 uint32 for TxtTitle +0x108 Top / +0x10C Bottom)
constexpr uint32_t kPatch0065MarioTopColor = 0xFF2020FF;
constexpr uint32_t kPatch0065MarioBottomColor = 0xFF1010CC;
constexpr uint32_t kPatch0065LuigiTopColor = 0xFF20FF20;
constexpr uint32_t kPatch0065LuigiBottomColor = 0xFF10CC10;

// Disc Tint RGB (PicBase only)
constexpr uint8_t kPatch0065MarioRedR = 255;
constexpr uint8_t kPatch0065MarioRedG = 60;
constexpr uint8_t kPatch0065MarioRedB = 60;

constexpr uint8_t kPatch0065LuigiGreenR = 60;
constexpr uint8_t kPatch0065LuigiGreenG = 255;
constexpr uint8_t kPatch0065LuigiGreenB = 60;

using Patch0065AllocatorFn = void* (*)(std::size_t size);
using Patch0065MeterConstructorFn = void (*)(
    void* meter, const char* objectName, const char* archiveName);
using Patch0065InitWithoutIterFn = void (*)(void* object);
using Patch0065SetNerveFn = void (*)(void* object, const void* nerve);
using Patch0065SetTextBoxNumberFn = void (*)(
    void* layoutActor, const char* paneName, int num);
using Patch0065FindPaneFn = std::uintptr_t (*)(
    std::uintptr_t pane, const char* name, bool recursive);
using Patch0065SetColorFn = void (*)(
    std::uintptr_t pane, uint32_t targetIdx, uint8_t value);
using Patch0065SetStringFn = void (*)(
    std::uintptr_t textBox, const wchar_t* str, uint32_t dstIdx);

std::uint32_t sPatch0065LogCount = 0;


std::uintptr_t Patch0065GetRootPane(std::uintptr_t meter) {
    const std::uintptr_t manager = IsGuestPointer(meter)
        ? *reinterpret_cast<std::uintptr_t*>(
              meter + kPatch0065LayoutManagerField)
        : 0;
    const std::uintptr_t resource = IsGuestPointer(manager)
        ? *reinterpret_cast<std::uintptr_t*>(
              manager + kPatch0065LayoutResourceField)
        : 0;
    const std::uintptr_t rootPane = IsGuestPointer(resource)
        ? *reinterpret_cast<std::uintptr_t*>(
              resource + kPatch0065RootPaneField)
        : 0;
    return IsGuestPointer(rootPane) ? rootPane : 0;
}

void Patch0065TintPane(std::uintptr_t pane, uint8_t r, uint8_t g, uint8_t b) {
    if (!IsGuestPointer(pane)) {
        return;
    }
    const std::uintptr_t vtable = *reinterpret_cast<std::uintptr_t*>(pane);
    if (!IsGuestPointer(vtable)) {
        return;
    }
    const auto setColor = reinterpret_cast<Patch0065SetColorFn>(
        *reinterpret_cast<std::uintptr_t*>(vtable + 0x58));
    if (!setColor) {
        return;
    }
    for (uint32_t vtx = 0; vtx < 4; ++vtx) {
        setColor(pane, vtx * 4 + 0, r);
        setColor(pane, vtx * 4 + 1, g);
        setColor(pane, vtx * 4 + 2, b);
        setColor(pane, vtx * 4 + 3, 255);
    }
}

// Dedicated Native Widget Instances
void* sP1Meter = nullptr;
void* sP2Meter = nullptr;
void* sP1PlayerLeft = nullptr;
void* sP2PlayerLeft = nullptr;

void Patch0065ApplyStyling(std::uintptr_t meter, float offsetY, const wchar_t* label, uint32_t topColor, uint32_t bottomColor, uint8_t r, uint8_t g, uint8_t b) {
    const std::uintptr_t rootPane = Patch0065GetRootPane(meter);
    if (!rootPane) {
        return;
    }

    *reinterpret_cast<uint8_t*>(
        rootPane + kPatch0065PaneVisibleField) |= 1;

    const std::uintptr_t vtable = *reinterpret_cast<std::uintptr_t*>(rootPane);
    if (IsGuestPointer(vtable)) {
        const auto findPane = reinterpret_cast<Patch0065FindPaneFn>(
            *reinterpret_cast<std::uintptr_t*>(vtable + 0x70));
        if (findPane) {
            // 1. MoveMeter positioned at top-right screen anchor + vertical offset
            const std::uintptr_t moveMeter = findPane(rootPane, "MoveMeter", true);
            if (IsGuestPointer(moveMeter)) {
                *reinterpret_cast<float*>(moveMeter + kPatch0065PaneTranslateXField) = kPatch0065MoveMeterBaseX;
                *reinterpret_cast<float*>(moveMeter + kPatch0065PaneTranslateYField) = kPatch0065MoveMeterBaseY + offsetY;
            }

            // 2. Uniform 0.75x scaling on HPMeter
            const std::uintptr_t hpMeter = findPane(rootPane, "HPMeter", true);
            if (IsGuestPointer(hpMeter)) {
                *reinterpret_cast<float*>(hpMeter + kPatch0065PaneScaleXField) = kPatch0065MeterScale;
                *reinterpret_cast<float*>(hpMeter + kPatch0065PaneScaleYField) = kPatch0065MeterScale;
            }

            // 3. Align HitPoint (4:3 disc) to X = 0.0f, HitPointMeter to X = -37.0f
            const std::uintptr_t hitPoint = findPane(rootPane, "HitPoint", true);
            if (IsGuestPointer(hitPoint)) {
                *reinterpret_cast<float*>(hitPoint + kPatch0065PaneTranslateXField) = 0.0f;
                *reinterpret_cast<float*>(hitPoint + kPatch0065PaneTranslateYField) = 0.0f;
                *reinterpret_cast<uint8_t*>(hitPoint + kPatch0065PaneVisibleField) = 1;
            }
            const std::uintptr_t hitPointMeter = findPane(rootPane, "HitPointMeter", true);
            if (IsGuestPointer(hitPointMeter)) {
                *reinterpret_cast<float*>(hitPointMeter + kPatch0065PaneTranslateXField) = -37.0f;
                *reinterpret_cast<float*>(hitPointMeter + kPatch0065PaneTranslateYField) = 0.0f;
            }

            // 4. Hide duplicate widescreen MoveMeterW / HitPointW subtree
            const std::uintptr_t moveMeterW = findPane(rootPane, "MoveMeterW", true);
            if (IsGuestPointer(moveMeterW)) {
                *reinterpret_cast<uint8_t*>(moveMeterW + kPatch0065PaneVisibleField) = 0;
            }
            const std::uintptr_t hitPointW = findPane(rootPane, "HitPointW", true);
            if (IsGuestPointer(hitPointW)) {
                *reinterpret_cast<uint8_t*>(hitPointW + kPatch0065PaneVisibleField) = 0;
            }
            const std::uintptr_t picBaseW = findPane(rootPane, "PicBaseW", true);
            if (IsGuestPointer(picBaseW)) {
                *reinterpret_cast<uint8_t*>(picBaseW + kPatch0065PaneVisibleField) = 0;
            }

            // 5. Tint ONLY PicBase (outer background circle)
            const std::uintptr_t picBase = findPane(rootPane, "PicBase", true);
            Patch0065TintPane(picBase, r, g, b);

            // 6. Set title label and text colors
            const std::uintptr_t txtTitle = findPane(rootPane, "TxtTitle", true);
            if (IsGuestPointer(txtTitle)) {
                *reinterpret_cast<uint32_t*>(txtTitle + 0x108) = topColor;
                *reinterpret_cast<uint32_t*>(txtTitle + 0x10C) = bottomColor;

                const std::uintptr_t txtVtable = *reinterpret_cast<std::uintptr_t*>(txtTitle);
                if (IsGuestPointer(txtVtable)) {
                    const auto setString = reinterpret_cast<Patch0065SetStringFn>(
                        *reinterpret_cast<std::uintptr_t*>(txtVtable + 0xD8));
                    if (setString && label != nullptr) {
                        setString(txtTitle, label, 0);
                    }
                }
            }
        }
    }
}

void NativeUpdateMeter(void* meter, int newHealth) {
    if (!IsGuestPointer(reinterpret_cast<std::uintptr_t>(meter))) {
        return;
    }

    const std::uintptr_t meterAddr = reinterpret_cast<std::uintptr_t>(meter);
    int* const healthField = reinterpret_cast<int*>(meterAddr + kPatch0065MeterCountField);
    const int oldHealth = *healthField;

    if (oldHealth == newHealth) {
        return;
    }

    *healthField = newHealth;
    *reinterpret_cast<uint8_t*>(meterAddr + 0x6C) = 0;

    const std::uintptr_t target = exl::util::modules::GetTargetStart();

    // 1. Immediate native BRLAN animation frame update for wedges (Frame = float(health))
    using SetAnimFrameFn = void (*)(float, void*, const char*, uint32_t);
    const auto setAnimFrame = reinterpret_cast<SetAnimFrameFn>(target + 0x067E00);
    if (setAnimFrame) {
        setAnimFrame(static_cast<float>(newHealth), meter, "HitPointMeter", 0);
    }

    // 2. Immediate numeric text update ("HitPointNumber")
    const auto setNum = reinterpret_cast<Patch0065SetTextBoxNumberFn>(target + kPatch0065SetTextBoxNumber);
    if (setNum) {
        setNum(meter, "HitPointNumber", newHealth);
    }

    // 3. Ensure alpha is visible (1.0f) during health transitions
    *reinterpret_cast<float*>(meterAddr + kPatch0065MeterAlphaField) = 1.0f;

    // 4. Native nerve transition (Damage vs Recovery)
    using SetNerveFn = void (*)(void*, const void*);
    const auto setNerve = reinterpret_cast<SetNerveFn>(target + kPatch0065SetNerve);

    if (newHealth < oldHealth) {
        // Damage transition: NrvHitPointMeter::Zero vs NrvHitPointMeter::Damage
        const void* nrv = nullptr;
        if (newHealth == 0) {
            nrv = *reinterpret_cast<const void**>(target + 0x915428); // Zero
        } else {
            nrv = *reinterpret_cast<const void**>(target + 0x915430); // Damage
        }
        if (nrv != nullptr && setNerve) {
            setNerve(meter, nrv);
        }
    } else {
        // Recovery transition: NrvHitPointMeter::Recovery vs NrvHitPointMeter::RecoveryCount
        const void* nrv = nullptr;
        if (newHealth < 4) {
            nrv = *reinterpret_cast<const void**>(target + 0x915440); // Recovery
        } else {
            nrv = *reinterpret_cast<const void**>(target + 0x915448); // RecoveryCount
        }
        if (nrv != nullptr && setNerve) {
            setNerve(meter, nrv);
        }
    }

    if (sPatch0065LogCount < kPatch0065LogLimit) {
        ++sPatch0065LogCount;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0065 NativeUpdateMeter meter=%p old=%d new=%d sample=%u/%u",
                    meter, oldHealth, newHealth,
                    static_cast<unsigned>(sPatch0065LogCount),
                    static_cast<unsigned>(kPatch0065LogLimit));
    }
}

HOOK_DEFINE_TRAMPOLINE(Patch0020SelectorInit) {
    static void Callback(std::uintptr_t selector) {
        const std::uintptr_t target = exl::util::modules::GetTargetStart();
        const auto allocate = reinterpret_cast<Patch0065AllocatorFn>(
            target + kPatch0065Allocator);
        const auto constructMeter = reinterpret_cast<Patch0065MeterConstructorFn>(
            target + kPatch0065MeterConstructor);
        const auto initWithoutIter = reinterpret_cast<Patch0065InitWithoutIterFn>(
            target + kPatch0065InitWithoutIter);

        // 1. Mario HitPointMeter (0x78 bytes) at selector + 0x18
        // Guarded: a null/foreign allocator return must never reach the
        // constructor, init, field store, or styling (EVD-0156 boot crash).
        void* p1Storage = allocate(kPatch0065MeterSize);
        if (p1Storage != nullptr && IsGuestPointer(reinterpret_cast<std::uintptr_t>(p1Storage))) {
            constructMeter(p1Storage, "MarioHitPointMeter", "HitPointMeter");
            *reinterpret_cast<void**>(selector + kPatch0065SelectorNormalMeterField) = p1Storage;
            initWithoutIter(p1Storage);
            sP1Meter = p1Storage;

            *reinterpret_cast<uint8_t*>(reinterpret_cast<std::uintptr_t>(p1Storage) + 0x38) = 0;
            *reinterpret_cast<uint8_t*>(reinterpret_cast<std::uintptr_t>(p1Storage) + 0x3b) = 0;
            Patch0065ApplyStyling(reinterpret_cast<std::uintptr_t>(p1Storage), kPatch0065P1OffsetY, L"1P", kPatch0065MarioTopColor, kPatch0065MarioBottomColor, kPatch0065MarioRedR, kPatch0065MarioRedG, kPatch0065MarioRedB);
        } else {
            sP1Meter = nullptr;
            Logging.Log("[GalaxyCoop:smg1] PATCH-0065 skipped P1 meter: allocator returned %p kill=1", p1Storage);
        }

        // 2. Luigi HitPointMeter (0x78 bytes) at selector + 0x20
        void* p2Storage = allocate(kPatch0065MeterSize);
        if (p2Storage != nullptr && IsGuestPointer(reinterpret_cast<std::uintptr_t>(p2Storage))) {
            constructMeter(p2Storage, "LuigiHitPointMeter", "HitPointMeter");
            *reinterpret_cast<void**>(selector + kPatch0065SelectorSuddenDeathField) = p2Storage;
            initWithoutIter(p2Storage);
            sP2Meter = p2Storage;

            *reinterpret_cast<uint8_t*>(reinterpret_cast<std::uintptr_t>(p2Storage) + 0x38) = 0;
            *reinterpret_cast<uint8_t*>(reinterpret_cast<std::uintptr_t>(p2Storage) + 0x3b) = 0;
            Patch0065ApplyStyling(reinterpret_cast<std::uintptr_t>(p2Storage), kPatch0065P2OffsetY, L"2P", kPatch0065LuigiTopColor, kPatch0065LuigiBottomColor, kPatch0065LuigiGreenR, kPatch0065LuigiGreenG, kPatch0065LuigiGreenB);
        } else {
            sP2Meter = nullptr;
            Logging.Log("[GalaxyCoop:smg1] PATCH-0065 skipped P2 meter: allocator returned %p kill=1", p2Storage);
        }

        Logging.Log("[GalaxyCoop:smg1] PATCH-0065 selector initialized sP1Meter=%p sP2Meter=%p",
                    sP1Meter, sP2Meter);
    }
};

// PATCH-0179 (TASK-0023, EVD-0235): per-player bee flight gauge.
// updateHud 0x66A50 runs for each actor and, for power-up 4, passes
// fuel(+0x3C0->+0x412)/max to 0x6867C -> 0x32CD3C -> 0x67AD0 on the one
// FlyMeter at MarioSubMeter(scene layout holder 0x66B30 +0x38)+0x18, so the
// last actor (P2) owns the value. The meter's control 0x7DE24 places it at
// MarioHolder(0x4340)+0x18 = P1's screen position and decides show/hide from
// that actor. PATCH-0179 builds a second FlyMeter in MarioSubMeter::init
// 0x20E2E8, routes P2's ratio to it by swapping submeter+0x18 during P2's
// updateHud, runs its control with MarioHolder+0x18 = P2, and mirrors the
// native activation (submeter+0x28 == FlyMeter) with appear/kill.
constexpr bool kPatch0179P2FlyMeterEnabled = true;
constexpr std::uintptr_t kPatch0179SubMeterInit = 0x20E2E8;
constexpr std::uintptr_t kPatch0179SubMeterLayoutCtor = 0x20E364;
constexpr std::uintptr_t kPatch0179SubMeterControl = 0x7DE24;
constexpr std::uintptr_t kPatch0179LayoutAppear = 0x273958;
constexpr std::uintptr_t kPatch0179LayoutKill = 0x1A8AA0;
constexpr std::uintptr_t kPatch0179SceneLayoutHolder = 0x66B30;
constexpr std::uintptr_t kPatch0179MarioHolderGet = 0x4340;
constexpr std::uintptr_t kPatch0179HolderSubMeterField = 0x38;
constexpr std::uintptr_t kPatch0179SubMeterFlyField = 0x18;
constexpr std::uintptr_t kPatch0179SubMeterActiveField = 0x28;
constexpr std::uintptr_t kPatch0179MarioHolderPlayerField = 0x18;
constexpr std::size_t kPatch0179SubMeterLayoutSize = 0x48;

std::uintptr_t sPatch0179SubMeter = 0;
std::uintptr_t sPatch0179P2FlyMeter = 0;
bool sPatch0179P2FlyActive = false;
std::uint32_t sPatch0179Logs = 0;

// Current MarioSubMeter only if it is the one that owns our P2 meter.
std::uintptr_t Patch0179CurrentSubMeter() {
    if (!sPatch0179SubMeter || !sPatch0179P2FlyMeter) return 0;
    const std::uintptr_t target = exl::util::modules::GetTargetStart();
    const auto holder = reinterpret_cast<std::uintptr_t (*)()>(target + kPatch0179SceneLayoutHolder)();
    if (!IsGuestPointer(holder)) return 0;
    const auto subMeter = *reinterpret_cast<const std::uintptr_t*>(holder + kPatch0179HolderSubMeterField);
    return subMeter == sPatch0179SubMeter ? subMeter : 0;
}

void Patch0179SyncActivation(std::uintptr_t subMeter) {
    const std::uintptr_t target = exl::util::modules::GetTargetStart();
    const auto fly = *reinterpret_cast<const std::uintptr_t*>(subMeter + kPatch0179SubMeterFlyField);
    const auto active = *reinterpret_cast<const std::uintptr_t*>(subMeter + kPatch0179SubMeterActiveField);
    const bool want = fly != 0 && active == fly && IsValidMarioActor(GetSecondPlayer());
    if (want == sPatch0179P2FlyActive) return;
    sPatch0179P2FlyActive = want;
    const auto fn = reinterpret_cast<void (*)(std::uintptr_t)>(
        target + (want ? kPatch0179LayoutAppear : kPatch0179LayoutKill));
    fn(sPatch0179P2FlyMeter);
    if (sPatch0179Logs < 16) {
        ++sPatch0179Logs;
        Logging.Log("[GalaxyCoop:smg1] PATCH-0179 p2 fly meter %s meter=0x%lx kill=1",
                    want ? "appear" : "kill", static_cast<unsigned long>(sPatch0179P2FlyMeter));
    }
}

HOOK_DEFINE_TRAMPOLINE(Patch0179SubMeterInit) {
    static void Callback(std::uintptr_t subMeter) {
        Orig(subMeter);
        sPatch0179SubMeter = 0;
        sPatch0179P2FlyMeter = 0;
        sPatch0179P2FlyActive = false;
        const std::uintptr_t target = exl::util::modules::GetTargetStart();
        void* storage = reinterpret_cast<Patch0065AllocatorFn>(target + kPatch0065Allocator)(
            kPatch0179SubMeterLayoutSize);
        if (storage == nullptr || !IsGuestPointer(reinterpret_cast<std::uintptr_t>(storage))) {
            Logging.Log("[GalaxyCoop:smg1] PATCH-0179 skipped p2 fly meter: allocator returned %p kill=1", storage);
            return;
        }
        reinterpret_cast<void (*)(void*, const char*, const char*)>(target + kPatch0179SubMeterLayoutCtor)(
            storage, "LuigiFlyMeter", "FlyMeter");
        reinterpret_cast<Patch0065InitWithoutIterFn>(target + kPatch0065InitWithoutIter)(storage);
        sPatch0179SubMeter = subMeter;
        sPatch0179P2FlyMeter = reinterpret_cast<std::uintptr_t>(storage);
        Logging.Log("[GalaxyCoop:smg1] PATCH-0179 p2 fly meter built submeter=0x%lx p1=0x%lx p2=0x%lx kill=1",
                    static_cast<unsigned long>(subMeter),
                    static_cast<unsigned long>(*reinterpret_cast<const std::uintptr_t*>(subMeter + kPatch0179SubMeterFlyField)),
                    static_cast<unsigned long>(sPatch0179P2FlyMeter));
    }
};

HOOK_DEFINE_TRAMPOLINE(Patch0179SubMeterControl) {
    static void Callback(std::uintptr_t layout) {
        void* p2 = GetSecondPlayer();
        if (layout != sPatch0179P2FlyMeter || layout == 0 || !IsValidMarioActor(p2)) {
            Orig(layout);
            return;
        }
        const std::uintptr_t target = exl::util::modules::GetTargetStart();
        const auto holder = reinterpret_cast<std::uintptr_t (*)()>(target + kPatch0179MarioHolderGet)();
        if (!IsGuestPointer(holder)) {
            Orig(layout);
            return;
        }
        auto* slot = reinterpret_cast<std::uintptr_t*>(holder + kPatch0179MarioHolderPlayerField);
        const std::uintptr_t previous = *slot;
        *slot = reinterpret_cast<std::uintptr_t>(p2);
        Orig(layout);
        *slot = previous;
    }
};

// 0x066A50: MarioActor::updateHud(MarioActor* this)
constexpr std::uintptr_t kPatchMarioActorUpdateHud = 0x066A50;

HOOK_DEFINE_TRAMPOLINE(PatchMarioActorUpdateHud) {
    static void Callback(std::uintptr_t actor) {
        void* p1 = GetFirstPlayer();
        void* p2 = GetSecondPlayer();

        void* targetMeter = nullptr;
        if (reinterpret_cast<void*>(actor) == p1) {
            targetMeter = sP1Meter;
        } else if (reinterpret_cast<void*>(actor) == p2) {
            targetMeter = sP2Meter;

            // FIELD-SMG1-MARIO-ENABLE-NORMAL-DAMAGE (+0x8F1): Mirror P1's normal-damage enable state
            // to P2's inner Mario every frame. Native setEnableNormalDamage (0x2B5750) only sets P1 via singleton.
            const std::uintptr_t p2Inner = *reinterpret_cast<const std::uintptr_t*>(actor + 0x3C0);
            if (IsGuestPointer(p2Inner)) {
                if (IsValidMarioActor(p1)) {
                    const std::uintptr_t p1Inner = *reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(p1) + 0x3C0);
                    if (IsGuestPointer(p1Inner)) {
                        *reinterpret_cast<uint8_t*>(p2Inner + 0x8F1) = *reinterpret_cast<const uint8_t*>(p1Inner + 0x8F1);
                    } else {
                        *reinterpret_cast<uint8_t*>(p2Inner + 0x8F1) = 1;
                    }
                } else {
                    *reinterpret_cast<uint8_t*>(p2Inner + 0x8F1) = 1;
                }
            }
        }

        if (targetMeter != nullptr) {
            const int health = *reinterpret_cast<const int*>(actor + kPatch0065CurrentHealthField);
            NativeUpdateMeter(targetMeter, health);
        }

        const std::uintptr_t subMeter =
            (kPatch0179P2FlyMeterEnabled && reinterpret_cast<void*>(actor) == p2) ? Patch0179CurrentSubMeter() : 0;
        if (subMeter != 0) {
            Patch0179SyncActivation(subMeter);
            auto* flySlot = reinterpret_cast<std::uintptr_t*>(subMeter + kPatch0179SubMeterFlyField);
            const std::uintptr_t p1Fly = *flySlot;
            *flySlot = sPatch0179P2FlyMeter;
            Orig(actor);
            *flySlot = p1Fly;
            return;
        }
        Orig(actor);
    }
};


// Compile-time switches for HUD damage/health and death machine
// SMG1 main 76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938.
// EVD-0188: native selector always writes +0x18 (P1's meter), including
// updates originating from P2. Actor-discriminated updates above own both
// meters; block the competing selector writes while the pair is available.
constexpr bool kPatch0065DamageHooksEnabled = true;

// kDeathLoopEnabled: when false (vanilla testing), the custom Animating/Ghost/Respawn loop is skipped
constexpr bool kDeathLoopEnabled = false;

// kPatch0067DisappearHookEnabled: setting true reproduces the blinking lives bug (hook on 0x284A50 PlayerLeft::control)
constexpr bool kPatch0067DisappearHookEnabled = false;

// 0x066B48: HitPointMeterSelector::setHealth(HitPointMeterSelector* this, int health)
constexpr std::uintptr_t kPatch0065SelectorSetHealth = 0x066B48;

bool OwnsHealthSelector(std::uintptr_t selector) {
    return IsGuestPointer(selector) &&
        IsGuestPointer(reinterpret_cast<std::uintptr_t>(sP1Meter)) &&
        IsGuestPointer(reinterpret_cast<std::uintptr_t>(sP2Meter)) &&
        *reinterpret_cast<void* const*>(selector + kPatch0065SelectorNormalMeterField) == sP1Meter;
}

HOOK_DEFINE_TRAMPOLINE(Patch0065SelectorSetHealth) {
    static void Callback(std::uintptr_t selector, int health) {
        if (!OwnsHealthSelector(selector)) {
            Orig(selector, health);
        }
    }
};

HOOK_DEFINE_TRAMPOLINE(Patch0065SelectorRequestDamage) {
    static void Callback(std::uintptr_t selector) {
        if (!OwnsHealthSelector(selector)) {
            Orig(selector);
        }
    }
};

// Stage 2: Dual-Widget Visible Lives HUD (PlayerLeft)

HOOK_DEFINE_TRAMPOLINE(Patch0067CounterLayoutInit) {
    static void Callback(std::uintptr_t controller) {
        Orig(controller);

        if (controller == 0 || !IsGuestPointer(controller)) {
            Logging.Log("[GalaxyCoop:smg1] PATCH-0067 skipped counter init: controller=%p kill=1",
                        reinterpret_cast<void*>(controller));
            return;
        }

        const std::uintptr_t target = exl::util::modules::GetTargetStart();
        const auto allocate = reinterpret_cast<Patch0065AllocatorFn>(target + kPatch0065Allocator);
        const auto constructPlayerLeft = reinterpret_cast<void (*)(void*, const char*)>(
            target + kPatch0067PlayerLeftConstructor);
        const auto initWithoutIter = reinterpret_cast<Patch0065InitWithoutIterFn>(
            target + kPatch0065InitWithoutIter);
        const auto startPaneAnim = reinterpret_cast<void (*)(void*, const char*, int)>(
            target + kPatch0067StartPaneAnim);

        // 1. Capture primary PlayerLeft (Mario / P1)
        void* p1PlayerLeft = *reinterpret_cast<void**>(controller + 0x60);
        sP1PlayerLeft = p1PlayerLeft;

        // Position P1 widget shifted up by 55.0f so Luigi (P2) fits neatly below it at native Y
        const std::uintptr_t rootPaneP1 = Patch0065GetRootPane(reinterpret_cast<std::uintptr_t>(sP1PlayerLeft));
        if (rootPaneP1) {
            *reinterpret_cast<float*>(rootPaneP1 + kPatch0065PaneTranslateYField) = 55.0f;
        }

        // 2. Instantiate Luigi PlayerLeft (0x58 bytes)
        void* p2PlayerLeft = allocate(0x58);
        if (p2PlayerLeft != nullptr) {
            constructPlayerLeft(p2PlayerLeft, "LuigiPlayerLeft");
            initWithoutIter(p2PlayerLeft);
            sP2PlayerLeft = p2PlayerLeft;

            // 3. Switch to native Luigi icon/animation
            if (startPaneAnim) {
                startPaneAnim(p2PlayerLeft, "Luigi", 2);
            }

            // 4. Position P2 widget at native Y (0.0f offset), right below Mario (P1)
            const std::uintptr_t rootPaneP2 = Patch0065GetRootPane(reinterpret_cast<std::uintptr_t>(p2PlayerLeft));
            if (rootPaneP2) {
                *reinterpret_cast<float*>(rootPaneP2 + kPatch0065PaneTranslateYField) = 0.0f;
            }

            // 5. Keep unclipped
            *reinterpret_cast<uint8_t*>(reinterpret_cast<std::uintptr_t>(p2PlayerLeft) + 0x38) = 0;
            *reinterpret_cast<uint8_t*>(reinterpret_cast<std::uintptr_t>(p2PlayerLeft) + 0x3b) = 0;
        }
        // Keep sP1PlayerLeft unclipped
        if (sP1PlayerLeft != nullptr && IsGuestPointer(reinterpret_cast<std::uintptr_t>(sP1PlayerLeft))) {
            *reinterpret_cast<uint8_t*>(reinterpret_cast<std::uintptr_t>(sP1PlayerLeft) + 0x38) = 0;
            *reinterpret_cast<uint8_t*>(reinterpret_cast<std::uintptr_t>(sP1PlayerLeft) + 0x3b) = 0;
        }

        Logging.Log("[GalaxyCoop:smg1] PATCH-0067 PlayerLeft dual widgets initialized: sP1PlayerLeft=%p sP2PlayerLeft=%p",
                    sP1PlayerLeft, sP2PlayerLeft);
    }
};

HOOK_DEFINE_TRAMPOLINE(Patch0067PlayerLeftControl) {
    static void Callback(void* actor) {
        if (actor == sP1PlayerLeft) {
            *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(actor) + 0x3c) = GetP1Lives();
        } else if (actor == sP2PlayerLeft) {
            *reinterpret_cast<int*>(reinterpret_cast<std::uintptr_t>(actor) + 0x3c) = GetP2Lives();
        }
        Orig(actor);
    }
};

HOOK_DEFINE_TRAMPOLINE(Patch0163GetPlayerLeft) {
    static std::uint16_t Callback() {
        if (sPatch0163P2LifeScope) return GetP2Lives();
        return Orig();
    }
};

HOOK_DEFINE_TRAMPOLINE(Patch0163PlayerLeftWait) {
    static void Callback(void* actor) {
        const auto a = reinterpret_cast<std::uintptr_t>(actor);
        const auto target = exl::util::modules::GetTargetStart();
        const bool owned = actor == sP2PlayerLeft && IsGuestPointer(a) &&
            *reinterpret_cast<const std::uintptr_t*>(a) == target + kPatch0163PlayerLeftVtable;
        if (owned && sPatch0163LifeSelectionLogs < 4 && GetP1Lives() != GetP2Lives()) {
            ++sPatch0163LifeSelectionLogs;
            Logging.Log("[GalaxyCoop:smg1] PATCH-0163 P2 native life animation selected own=%u p1=%u kill=1", GetP2Lives(), GetP1Lives());
        }
        struct Scope {
            bool previous;
            explicit Scope(bool active) : previous(sPatch0163P2LifeScope) { sPatch0163P2LifeScope = active; }
            ~Scope() { sPatch0163P2LifeScope = previous; }
        } scope(owned);
        Orig(actor); // preserve native flash, number animation and auto-hide
    }
};

HOOK_DEFINE_TRAMPOLINE(Patch0067PlayerLeftAppear) {
    static void Callback(void* actor) {
        Orig(actor);
        if (actor == sP1PlayerLeft && sP2PlayerLeft != nullptr &&
            IsGuestPointer(reinterpret_cast<std::uintptr_t>(sP2PlayerLeft))) {
            Orig(sP2PlayerLeft);
        }
    }
};

HOOK_DEFINE_TRAMPOLINE(Patch0067CounterLayoutDisappear) {
    static void Callback(std::uintptr_t controller) {
        Orig(controller);
        if (sP2PlayerLeft != nullptr && IsGuestPointer(reinterpret_cast<std::uintptr_t>(sP2PlayerLeft))) {
            const std::uintptr_t target = exl::util::modules::GetTargetStart();
            const auto disappearPlayerLeft = reinterpret_cast<void (*)(void*)>(
                target + kPatch0067PlayerLeftDisappear);
            if (disappearPlayerLeft) {
                disappearPlayerLeft(sP2PlayerLeft);
            }
        }
    }
};

}  // namespace

void UpdateHud(std::uintptr_t target,
               std::uintptr_t p1,
               std::uintptr_t p2) {
    // Pure Native 2-Widget Architecture: No per-frame polling required.
}

constexpr std::uintptr_t kPatch0065PerFrameUpdate = 0x10D358;

// Temporary burst-bisect reverser (EVD-0153/0158/0159): when true, a valid
// player pair only gets a read-only integrity snapshot here; every pair-gated
// write below (parking, nerves, timers, death progression, meter sync, warp)
// is skipped. Isolates our per-frame writes versus native dome code reacting
// to P2. REVERTED to false by user path decision (resume warp work); the dome
// burst is parked as a known issue under TASK-0016.
constexpr bool kDiagPairWritesOff = false;
std::uint32_t sIntegrityCount = 0;
constexpr std::uint32_t kIntegrityLimit = 4;

void LogActorIntegrity(const char* tag, void* actor) {
    const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(actor);
    const std::uintptr_t target = exl::util::modules::GetTargetStart();
    const std::uintptr_t vtable = *reinterpret_cast<const std::uintptr_t*>(a);
    const int health = *reinterpret_cast<const int*>(a + 0x528);
    const std::uintptr_t spine = *reinterpret_cast<const std::uintptr_t*>(a + 0x60);
    const void* nerve = IsGuestPointer(spine)
        ? *reinterpret_cast<void* const*>(spine + 0x8) : nullptr;
    const std::uintptr_t mario = *reinterpret_cast<const std::uintptr_t*>(a + 0x3C0);
    Logging.Log("[GalaxyCoop:smg1] DIAG-0159 %s vok=%d hp=%d spine=0x%lx nerve=%p mario=0x%lx kill=1",
                tag,
                (vtable == target + 0x835830) ? 1 : 0,
                health,
                static_cast<unsigned long>(IsGuestPointer(spine) ? spine : 0),
                nerve,
                static_cast<unsigned long>(IsGuestPointer(mario) ? mario : 0));
}

HOOK_DEFINE_TRAMPOLINE(Patch0065PerFrameUpdate) {
    static void Callback(std::uintptr_t arena) {
        Orig(arena);

        void* p1 = GetFirstPlayer();
        void* p2 = GetSecondPlayer();
        PumpDeferredP2Spawn();
        p1 = GetFirstPlayer();
        p2 = GetSecondPlayer();
        if (IsValidMarioActor(p1) && IsValidMarioActor(p2)) {
            LogWarpTopSnapshot("frame");
        }
        if (kDiagPairWritesOff && IsValidMarioActor(p1) && IsValidMarioActor(p2)) {
            if (sIntegrityCount < kIntegrityLimit) {
                ++sIntegrityCount;
                LogActorIntegrity("p1", p1);
                LogActorIntegrity("p2", p2);
            }
            return;
        }
        if (IsValidMarioActor(p1) && IsValidMarioActor(p2)) {
            TryDevBootWarp();
            CaptureAliveNerves();
            DecrementInvulnerableTimers();
            TickBubbleRespawn();
            TickDiag0173OneUpTrigger();
            TickDiag0176InputSources();
            SyncSpinEnabled("frame");

            if (kDeathLoopEnabled) {
                // P2 Lifecycle Management
                const DeathPhase p2Phase = GetP2DeathPhase();
                if (p2Phase == DeathPhase::Alive) {
                    UpdateP2Parking(p1, p2);
                } else if (p2Phase == DeathPhase::Animating) {
                    const std::uint32_t animTimer = GetP2AnimTimer();
                    if (animTimer > 1) {
                        SetP2AnimTimer(animTimer - 1);
                    } else if (animTimer == 1) {
                        SetP2AnimTimer(0);
                        SetP2DeathPhase(DeathPhase::Ghost);
                        SetP2RespawnTimer(300);
                        DeactivateDeadActor(p2);
                        Logging.Log("[GalaxyCoop:smg1] PATCH-0068 P2 collapse animation finished; entered Ghost state (5s countdown).");
                    }
                } else if (p2Phase == DeathPhase::Ghost) {
                    KeepDeadPlayerParked(p2, p1);
                    const std::uint32_t timer = GetP2RespawnTimer();
                    if (timer > 1) {
                        SetP2RespawnTimer(timer - 1);
                    } else if (timer == 1) {
                        SetP2RespawnTimer(0);
                        RespawnPlayer(p2, p1);
                        NativeUpdateMeter(sP2Meter, 3);
                    }
                }

                // P1 Lifecycle Management
                const DeathPhase p1Phase = GetP1DeathPhase();
                if (p1Phase == DeathPhase::Animating) {
                    const std::uint32_t animTimer = GetP1AnimTimer();
                    if (animTimer > 1) {
                        SetP1AnimTimer(animTimer - 1);
                    } else if (animTimer == 1) {
                        SetP1AnimTimer(0);
                        SetP1DeathPhase(DeathPhase::Ghost);
                        SetP1RespawnTimer(300);
                        DeactivateDeadActor(p1);
                        Logging.Log("[GalaxyCoop:smg1] PATCH-0068 P1 collapse animation finished; entered Ghost state (5s countdown).");
                    }
                } else if (p1Phase == DeathPhase::Ghost) {
                    KeepDeadPlayerParked(p1, p2);
                    const std::uint32_t timer = GetP1RespawnTimer();
                    if (timer > 1) {
                        SetP1RespawnTimer(timer - 1);
                    } else if (timer == 1) {
                        SetP1RespawnTimer(0);
                        RespawnPlayer(p1, p2);
                        NativeUpdateMeter(sP1Meter, 3);
                    }
                }
            } else {
                if (!IsP2Dead()) UpdateP2Parking(p1, p2);
            }

            // Keep both health meters alive and unclipped in the scene layout render graph
            if (sP1Meter != nullptr && sP2Meter != nullptr &&
                IsGuestPointer(reinterpret_cast<std::uintptr_t>(sP1Meter)) &&
                IsGuestPointer(reinterpret_cast<std::uintptr_t>(sP2Meter))) {

                *reinterpret_cast<uint8_t*>(reinterpret_cast<std::uintptr_t>(sP1Meter) + 0x38) = 0;
                *reinterpret_cast<uint8_t*>(reinterpret_cast<std::uintptr_t>(sP1Meter) + 0x3b) = 0;
                *reinterpret_cast<uint8_t*>(reinterpret_cast<std::uintptr_t>(sP2Meter) + 0x38) = 0;
                *reinterpret_cast<uint8_t*>(reinterpret_cast<std::uintptr_t>(sP2Meter) + 0x3b) = 0;

                const float p1Alpha = *reinterpret_cast<const float*>(
                    reinterpret_cast<std::uintptr_t>(sP1Meter) + kPatch0065MeterAlphaField);
                const int p2Health = *reinterpret_cast<const int*>(
                    reinterpret_cast<std::uintptr_t>(sP2Meter) + kPatch0065MeterCountField);

                // When Luigi is at full health and alive, sync P2 alpha to P1 alpha for unified walking auto-hide
                if (p2Health >= 3 && !IsP2Dead()) {
                    *reinterpret_cast<float*>(
                        reinterpret_cast<std::uintptr_t>(sP2Meter) + kPatch0065MeterAlphaField) = p1Alpha;
                }
            }
        }
    }
};

void InstallHud(const galaxy::target::Identity& identity) {
    if (identity.game == galaxy::target::Game::Smg1) {
        Patch0020SelectorInit::InstallAtOffset(kPatch0065SelectorInit);
        PatchMarioActorUpdateHud::InstallAtOffset(kPatchMarioActorUpdateHud);
        if (kPatch0065DamageHooksEnabled) {
            Patch0065SelectorSetHealth::InstallAtOffset(kPatch0065SelectorSetHealth);
            Patch0065SelectorRequestDamage::InstallAtOffset(kPatch0065SelectorRequestDamage);
        }
        Patch0065PerFrameUpdate::InstallAtOffset(kPatch0065PerFrameUpdate);
        if (kPatch0179P2FlyMeterEnabled) {
            Patch0179SubMeterInit::InstallAtOffset(kPatch0179SubMeterInit);
            Patch0179SubMeterControl::InstallAtOffset(kPatch0179SubMeterControl);
            Logging.Log("[GalaxyCoop:smg1] PATCH-0179 p2 fly meter installed init=0x%lx control=0x%lx kill=1",
                        static_cast<unsigned long>(kPatch0179SubMeterInit),
                        static_cast<unsigned long>(kPatch0179SubMeterControl));
        }

        // Stage 2: Dual-Widget Visible Lives HUD
        Patch0067CounterLayoutInit::InstallAtOffset(kPatch0067CounterLayoutInit);
        Patch0067PlayerLeftControl::InstallAtOffset(kPatch0067PlayerLeftControl);
        Patch0067PlayerLeftAppear::InstallAtOffset(kPatch0067PlayerLeftAppear);
        if (kPatch0163LifeAnimationOwnerEnabled) {
            Patch0163GetPlayerLeft::InstallAtOffset(kPatch0163GetPlayerLeft);
            Patch0163PlayerLeftWait::InstallAtOffset(kPatch0163PlayerLeftWait);
            Logging.Log("[GalaxyCoop:smg1] PATCH-0163 native life animation owner routing installed kill=1");
        }
        if (kPatch0067DisappearHookEnabled) {
            Patch0067CounterLayoutDisappear::InstallAtOffset(kPatch0067CounterLayoutDisappear);
        }

        Logging.Log("[GalaxyCoop:%s] PATCH-0065, 0067 & 0068 Pure Native HUD & Model Visibility installed (kPatch0065DamageHooksEnabled=%d, kDeathLoopEnabled=%d)",
                    galaxy::target::GameName(identity.game),
                    kPatch0065DamageHooksEnabled ? 1 : 0,
                    kDeathLoopEnabled ? 1 : 0);
    }
}

}  // namespace mod::patches
