#include "patch_splitscreen.hpp"
#include "patch_shared.hpp"
#include "patch_actions.hpp"
#include "patch_camera.hpp"
#include "patch_respawn.hpp"
#include "patch_input.hpp"
#include "planet_camera_math.hpp"
#include "split_camera_math.hpp"
#include "split_timing.hpp"
#include "push_constant_filter.hpp"
#include "uniform_upload_timing.hpp"
#include "backend_timing.hpp"
#include "frame_timeline.hpp"
#include "fast_vertex_convert.hpp"
#include "producer_cost.hpp"
#include "frame_overlap.hpp"
#include "f264_fast_transform.hpp"
#include "idle_poll.hpp"
#include "draw_cost.hpp"
#include "sd_log.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace mod::patches {
namespace {
// DIAG-0173 / EVD-0208, SMG1 main SHA-256
// 76f1b4d83962b29c88d9f6d68ac70e233bb66f4c852faab681d0bc6f63805938. NSO offsets.
// 2016C main 3D sequence: opening stp/str/mov/ldrb/cbz; exlaunch relocates the
//   5-instruction window including cbz. Called once from scene root 1E2B0.
// 204F0 GXSetViewport leaf: adrp/add/ldr of state slot 8855C0, stores s0..s5 at
//   +4F4..+508, tail-calls XF writer 4C14F0. Replaced exactly, never called.
// 20300 camera projection load: 3D50 -> 3D48(root,0x17) -> +74 -> GXSetProjection
//   20330(mtx,0). Replaced exactly, never called.
// 20490 full-screen viewport/scissor from render mode 915FF8 ->0 ->8 ->+4.
constexpr std::uintptr_t kScene3dDraw = 0x2016C;
// DIAG-0195 / EVD-0337, same recorded SMG1 main hash. 20900 true void
// entry after UDF padding, opening stp/str/mov/mov relocatable (0x20 frame).
// Native scene-object25 sampler calls wait994F0 and emits token99680. Time
// the whole wrapper, preserving Orig; it is not a pure GPU-wait timer.
constexpr std::uintptr_t kLensSampling = 0x20900;
constexpr std::uintptr_t kGxSetViewport = 0x204F0;
constexpr std::uintptr_t kGxStateSlot = 0x8855C0;
constexpr std::uintptr_t kGxWriteXfViewport = 0x4C14F0;
constexpr std::uintptr_t kCameraLoadProjection = 0x20300;
constexpr std::uintptr_t kNameObjRoot = 0x3D50;
constexpr std::uintptr_t kNameObjByIndex = 0x3D48;
constexpr int kCameraContextIndex = 0x17;
constexpr std::uintptr_t kCameraProjection = 0x74;
constexpr std::uintptr_t kGxSetProjection = 0x20330;
constexpr std::uintptr_t kFullViewport = 0x20490;
constexpr std::uintptr_t kRenderModeSlot = 0x915FF8;
constexpr float kDefaultFrameWidth = 640;
// ATT-0156 / EVD-0311,0312,0315,0316, same recorded SMG1 main hash.
// 19E9F0 is a true 176-byte entry; opening stp/stp/stp/mov is relocatable.
// Only config+2C/30/34 and+38/3C/40 are read (u32,u32,u8 per pool).
// Backend VT889C68 is confirmed live, slot130 ->10C400. Feed a local
// configuration to the native allocation producer, never mutate a live pool
// or change dimensions. Deferred peeks still wait for incomplete old copies.
constexpr std::uintptr_t kReadbackConfigInit = 0x19E9F0;
constexpr std::uintptr_t kReadbackBackendVtable = 0x889C68;

struct ReadbackPoolConfig {
    std::uint8_t unused[0x2C];
    std::uint32_t colourWidth;
    std::uint32_t colourHeight;
    std::uint8_t colourAsync;
    std::uint8_t pad[3];
    std::uint32_t depthWidth;
    std::uint32_t depthHeight;
    std::uint8_t depthAsync;
};
static_assert(offsetof(ReadbackPoolConfig, colourWidth) == 0x2C);
static_assert(offsetof(ReadbackPoolConfig, colourHeight) == 0x30);
static_assert(offsetof(ReadbackPoolConfig, colourAsync) == 0x34);
static_assert(offsetof(ReadbackPoolConfig, depthWidth) == 0x38);
static_assert(offsetof(ReadbackPoolConfig, depthHeight) == 0x3C);
static_assert(offsetof(ReadbackPoolConfig, depthAsync) == 0x40);

HOOK_DEFINE_TRAMPOLINE(SplitReadbackConfig) {
    static std::uint64_t Callback(void* backend, const ReadbackPoolConfig* config) {
        const auto address = reinterpret_cast<std::uintptr_t>(backend);
        const auto input = reinterpret_cast<std::uintptr_t>(config);
        if (!kSplitAsyncColourReadbackEnabled || !IsGuestPointer(address) || !IsGuestPointer(input))
            return Orig(backend, config);
        const auto target = exl::util::modules::GetTargetStart();
        if (*reinterpret_cast<const std::uintptr_t*>(address) != target + kReadbackBackendVtable)
            return Orig(backend, config);
        // Restrict to the exact configuration observed in EVD-0312. A new
        // renderer setup passes through unchanged for a fresh investigation.
        if (config->colourWidth != 64 || config->colourHeight != 64 || config->colourAsync != 0 ||
            config->depthWidth != 1 || config->depthHeight != 1 || config->depthAsync != 1)
            return Orig(backend, config);
        ReadbackPoolConfig local{};
        local.colourWidth = config->colourWidth;
        local.colourHeight = config->colourHeight;
        local.colourAsync = 1;
        local.depthWidth = config->depthWidth;
        local.depthHeight = config->depthHeight;
        local.depthAsync = config->depthAsync;
        const auto result = Orig(backend, &local);
        Logging.Log("[GalaxyCoop:smg1] ATT-0156 readback init colour=64x64 async=1 depth=1x1 async=1 result=%lu kill=1",
            static_cast<unsigned long>(result));
        return result;
    }
};
// ATT-0150 / EVD-0280, same main hash. 20520 GXSetScissor is a true entry
// whose fourth instruction is ADRP, so only exact replacement is safe. It
// reads *( *(916E68) ) as GX state, updates BP words +F0/+F4 and sends BP61
// through the FIFO begin/u8/u32/end helpers. GDB confirmed the live chain.
constexpr std::uintptr_t kGxSetScissor = 0x20520;
constexpr std::uintptr_t kGxScissorStateRoot = 0x916E68;
constexpr std::uintptr_t kFifoBegin = 0x4C1C40;
constexpr std::uintptr_t kFifoWriteU8 = 0x4C1C70;
constexpr std::uintptr_t kFifoWriteBpU32 = 0x4C1CB0;
constexpr std::uintptr_t kFifoEnd = 0x4C1D10;

// ATT-0137 / EVD-0256, same SMG1 main hash. NSO offsets.
// 610 GXLoadPosMtxImm(mtx, id): clean entry after udf padding, opening
//   stp/stp/mov/mov has no PC-relative instruction; 80 BL callers. Trampoline.
// AA45AEB0 global view matrix V1 (.bss, 3x4 row-major, translation 3/7/11),
//   copied from camera context 0x17 +0x14 by 20390.
// GX state *(8855C0) +4D8: projection type int (20330 str w1): 0 perspective,
//   1 ortho. MarioActor +494 camera point (PATCH-0164, EVD-0181).
constexpr std::uintptr_t kGxLoadPosMtxImm = 0x610;
constexpr std::uintptr_t kViewMatrix = 0xAA45AEB0;
constexpr std::uintptr_t kGxProjectionType = 0x4D8;
constexpr std::uintptr_t kActorCameraPoint = 0x494;
constexpr float kMaxViewDelta = 1.0e6f;
// ATT-0205: minimum sine of the reference eye above its owner's ground (~17.5 deg).
constexpr float kReferenceMinElevation = 0.3f;
// ATT-0138 / EVD-0256 / EVD-0257, same SMG1 main hash. NSO offset.
// 9F5F0 GXLoadNrmMtxImm(mtx, id): clean entry after udf padding, opening
//   stp/stp/mov/add/mov has no PC-relative instruction; 11 BL callers.
constexpr std::uintptr_t kGxLoadNrmMtxImm = 0x9F5F0;
// ATT-0139 / EVD-0258, same SMG1 main hash. NSO offsets.
// 4068 ClippingJudge frustum test (judge x0, pos x1, radius s0, level w2) ->
//   clipped. Leaf, no prologue, opens with cbz (PC-relative), so it is replaced
//   exactly and never called. Level 0 tests the 6 planes (nx,ny,nz,d) at judge
//   +0x14, level n>0 those at +0x74+n*0x60; outside when n.p-d < -radius.
//   Callers 3FAC (actor holder), tail calls 7AF40, 8D788, 187650. Judge movement
//   4D9F0 builds the planes from camera context 0x17's view (210F0), i.e. V1.
constexpr std::uintptr_t kClippingJudgeTest = 0x4068;
constexpr std::uintptr_t kJudgeLevel0Planes = 0x14;
constexpr std::uintptr_t kJudgeLevelPlanes = 0x74;
constexpr std::uintptr_t kJudgeLevelStride = 0x60;
// ATT-0144 / EVD-0263..0267, same SMG1 main hash. NSO offsets.
// BB0 GXSetArray(attr, ptr, stride): opening stp/stp/stp/mov, 4 relocated. The
//   J3D shape draws 800 and 79794 pass attr 15 (pos mtx, stride 30) then 16
//   (nrm mtx, stride 24) with the model's current-view draw/nrm buffer and
//   store the same pointers at j3dSys +128/+130 just before. Trampoline.
// 7A380 J3D pos mtx load (m[12] -> XF id<<2), 7A310 nrm load from a 3x4
//   (XF 400+id*3), 4A55F0 nrm load from a 3x3: straight-line leaves built on
//   XF header 4A55B0(addr, count) and value writer 21510(u32 bits). Replaced
//   exactly, never called.
// j3dSys AA45AEB0: +40 current model. J3DModel vtables 882AA0 (base), 835670
//   (J3DModelX, players), 808148 (layer clone); +8 modelData (+78 u16 draw
//   matrix count), +10 flags (&3 mode; 1 stores world matrices), +98
//   J3DMtxBuffer (vtable 882AF0; +38 draw arr1, +60 view count, +64 view no).
//   ConcatView models point arr1 at the static array 9527E0.
// 210F0 MR::getCameraViewMtx (camera context 0x17 +14).
// 2A4270 fallback allocator (size, align) over the nn::mem arena in .bss; the
//   GPU range GXSetArray accepts is [*(B4D955E8), *(90AF38)] (EVD-0266/0267).
constexpr std::uintptr_t kGxSetArray = 0xBB0;
constexpr std::uintptr_t kJ3dLoadPosMtx = 0x7A380;
constexpr std::uintptr_t kJ3dLoadNrmMtx = 0x7A310;
constexpr std::uintptr_t kJ3dLoadNrmMtx33 = 0x4A55F0;
constexpr std::uintptr_t kXfLoadHeader = 0x4A55B0;
constexpr std::uintptr_t kFifoWriteU32 = 0x21510;
constexpr std::uintptr_t kCameraViewMtx = 0x210F0;
// ATT-0154 / EVD-0291, same main hash. E6B54 true 104-byte leaf copies x1's
// 3x4 to context+14, then tails to inverse writer with context+44 and x1.
// First four scalar LDR/STR have no PC-relative operands or stack/LR changes.
constexpr std::uintptr_t kCameraSetView = 0xE6B54;
constexpr std::uintptr_t kCameraContextVtable = 0x81AC18;
constexpr std::uintptr_t kFallbackAlloc = 0x2A4270;
constexpr std::uintptr_t kGpuRangeStart = 0xB4D955E8;
constexpr std::uintptr_t kGpuRangeEnd = 0x90AF38;
constexpr std::uintptr_t kJ3dSysModel = 0x40;
constexpr std::uintptr_t kJ3dSysPosArray = 0x128;
constexpr std::uintptr_t kJ3dSysNrmArray = 0x130;
constexpr std::uintptr_t kJ3dModelVtable = 0x882AA0;
constexpr std::uintptr_t kJ3dModelXVtable = 0x835670;
constexpr std::uintptr_t kJ3dModelLayerVtable = 0x808148;
constexpr std::uintptr_t kJ3dMtxBufferVtable = 0x882AF0;
constexpr std::uintptr_t kConcatViewDrawArray = 0x9527E0;
constexpr int kAttrPosMtx = 0x15, kAttrNrmMtx = 0x16;
constexpr std::size_t kTwinHalfBytes = 0x200000;  // per frame; two halves alternate like J3D's arr0/arr1
constexpr unsigned kTwinCacheSlots = 2048;
constexpr unsigned kMaxDrawMatrices = 1024;

using planet::Vec;
using planet::Dot;
using planet::Cross;
using planet::Length;

// ATT-0140 policy: extra = eye push-back a shared midpoint frame needs
// (PATCH-0170 fit). Split beyond 2500, merge again below 1800, so the shared
// view never zooms far out. Splitting reacts in 8 ticks; merging waits 45.
constexpr float kSplitExtra = 2500.0f, kMergeExtra = 1800.0f;
constexpr unsigned kSplitTicks = 8, kMergeTicks = 45, kDividerTicks = 20;
constexpr unsigned kForcedMergeTicks = 5; // ATT-0141 divider close for bindings, pipes, deaths
// ATT-0142: with diverging gravity the merged view is the native leader view,
// whose fit already keeps 20% screen margin plus body width. Split once it
// would need 400 more eye distance to hold both; merge when it needs none.
constexpr float kNativeSplitExtra = 400.0f, kNativeMergeExtra = 1.0f;
// ATT-0143: merge needs separation below 80% of the separation at the split.
constexpr float kReunionFactor = 0.8f;
// ATT-0147: a merged camera should only replace the two views when it can
// start close to both players. Earlier flat-ground merging at 2507 was jarring.
constexpr float kConvergentMergeSeparation = 400.0f;
constexpr float kVisibleReunionUpDot = 0.642788f; // ATT-0152: cos(50 degrees)
// ATT-0146: 90 ticks include the ordinary close and a merged-view hold.
constexpr unsigned kRevealReopenHoldTicks = 90;
// ATT-0148: the first stage of opening shifts P1's camera before any second
// viewport is drawn. Closing matches the right camera before moving the seam.
constexpr unsigned kCameraPreOpenTicks = 12, kCameraMatchTicks = 16;
constexpr unsigned kEasedDividerTicks = 40;
float sSplitSeparation = 0;

int sPass = -1;
float sFrameWidth = kDefaultFrameWidth;
float sFrameHeight = 480.0f;
float sRevealAmount = 0;
float sPassX[2]{0, kDefaultFrameWidth * 0.5f}, sPassW[2]{kDefaultFrameWidth * 0.5f, kDefaultFrameWidth * 0.5f};
float sSplitFraction = 0;  // s: 0 merged, 1 fully split
float sRigBlend = 0;       // ATT-0148: 0 is P1's view, 1 is P2's gravity rig
unsigned sCameraPreOpenTicks = 0;
bool sSplitWanted = false;
unsigned sFitTicks = 0, sMissTicks = 0, sPolicyLogs = 0;
unsigned sRevealReopenHoldTicks = 0;
int sLastForced = 0;
unsigned sLastCameraSerial = 0, sStaleCameraDraws = 0;
bool sLastActive = false;
unsigned sFrames = 0, sLogs = 0, sActiveLogs = 0;
// Pass-1 view correction D = V2 * inverse(V1) as rotation sRd and translation sTd.
bool sViewOffsetValid = false, sRigRotates = false;
// ATT-0204: last drawn pass-1 view, kept past the scene for P2's next movement.
float sP2InputView[12]{};
bool sP2InputViewValid = false;
float sRd[9]{1, 0, 0, 0, 1, 0, 0, 0, 1};
float sTd[3]{};
float sViewDelta[3]{};
Vec sSmoothUp[2]{};
bool sSmoothUpValid = false;
float sRigAngle = 0;
unsigned sViewLogs = 0;
// ATT-0149: frozen split-entry camera frame for P2. Only value data persists;
// actor and camera pointers are fetched afresh every draw.
bool sAnchorValid = false;
float sAnchorView[12]{};
Vec sAnchorPoint{}, sAnchorUp{};
// ATT-0213 follow camera: P2's camera point last drawn frame. Steps longer than
// kFollowMaxStepLength are teleports (pipes, respawns) and are ignored.
Vec sLastP2Point{};
bool sLastP2PointValid = false;
constexpr float kFollowMaxStepLength = 200.0f;
// ATT-0215: native distance assumed when no framing sample exists at reopen
// (the console's Gateway sample, EVD-0521).
constexpr float kFreshReferenceNativeDistance = 1600.0f;
// ATT-0216: Luigi's camera point when his death began.
Vec sP2Held{};
bool sP2HeldValid = false;
// Units per drawn frame; radians. ~90 deg/s at full speed, 25-frame easing,
// no swing while running at the camera (beyond 150 degrees).
constexpr splitmath::FollowTuning kFollowTuning{2.0f, 12.0f, 0.04f, 0.026f, 2.62f};
float sAnchorNativeDistance = 0;
unsigned sLeaderViewSerial = 0, sLeaderViewLogs = 0;
int sLeaderViewBin = -1;
// Last drawn rig, used by the next movement's clipping: G(w) = Qt (w - P2) + P1.
bool sClipRigValid = false;
float sClipQt[9]{};
Vec sClipP1{}, sClipP2{};
unsigned sClipRescues = 0, sClipLogs = 0;
unsigned sWipeLogs = 0;
int sLastWipeBin = -1;
// DIAG-0191: per-frame GXLoadPosMtxImm loads by pass, and pass-1 loads corrected.
unsigned sPosLoads[2]{}, sPosCorrected = 0, sProbeLogs = 0;
bool sProbeOnOpen = false;
// ATT-0144: permanent twin arena (two halves by frame parity), a per-frame
// source -> twin cache, and counters logged every 600 split frames.
// A slot is live only when its generation is the current frame's, so a new
// frame needs no clearing.
struct TwinSlot { std::uintptr_t source, twin; unsigned generation; };
std::uintptr_t sTwinArena = 0;
bool sTwinArenaFailed = false;
std::size_t sTwinUsed = 0, sTwinPeak = 0;
unsigned sTwinHalf = 0, sTwinGeneration = 0;
TwinSlot sTwinCache[kTwinCacheSlots]{};
const float* sTwinCameraView = nullptr;  // looked up once per pass 1
bool sTwinNrmFollows = false;
unsigned sTwinNrmCount = 0;
struct TwinStats { unsigned pos, nrm, cached, mismatch, gateSkip, world, overflow, immPos, immNrm; } sTwinStats{};
unsigned sTwinLogs = 0;
// DIAG-0212: per-pass census of J3D shape draws outside that pass's half view.
constexpr float kCensusMargins[3] = {500.0f, 2000.0f, 8000.0f};
struct PassCensus { unsigned shapes, unclassified, out[3], runs, runOut[3], playerIn, playerOut; };
PassCensus sCensus[2]{};
float sPassProj[2][4]{};  // p0, p2, p5, p6 of the pass's perspective projection
bool sPassProjValid[2]{};
std::uintptr_t sCensusLast[2]{};
unsigned sCensusLastMask[2]{};
unsigned sCensusFrames = 0, sCensusLogs = 0;
std::uintptr_t Target() { return exl::util::modules::GetTargetStart(); }
std::uintptr_t Load(std::uintptr_t p) { return IsGuestPointer(p) ? *reinterpret_cast<const std::uintptr_t*>(p) : 0; }

// ATT-0145: J3DMtxBuffer's u64 pointer table is allocated at four-byte
// alignment (EVD-0270). The normal Load rejects that address before reading it.
// GXSetArray's proven GPU range bounds the whole entry; memcpy avoids an
// unaligned C++ pointer dereference.
std::uintptr_t LoadDrawArrayEntry(std::uintptr_t table, unsigned view) {
    const auto slot = table + view * sizeof(std::uintptr_t);
    const auto start = Load(Target() + kGpuRangeStart);
    const auto end = Load(Target() + kGpuRangeEnd);
    if ((slot & 3u) || slot < start || slot > end || end - slot < sizeof(std::uintptr_t)) return 0;
    std::uintptr_t value = 0;
    std::memcpy(&value, reinterpret_cast<const void*>(slot), sizeof(value));
    return value;
}

float FrameWidth() {
    const auto renderMode = Load(Load(Load(Target() + kRenderModeSlot)) + 8);
    if (!IsGuestPointer(renderMode)) return kDefaultFrameWidth;
    const float width = *reinterpret_cast<const std::uint16_t*>(renderMode + 4);
    return width >= 320 && width <= 1280 ? width : kDefaultFrameWidth;
}

float FrameHeight() {
    const auto renderMode = Load(Load(Load(Target() + kRenderModeSlot)) + 8);
    if (!IsGuestPointer(renderMode)) return 480.0f;
    const float height = *reinterpret_cast<const std::uint16_t*>(renderMode + 6);
    return height >= 200 && height <= 720 ? height : 480.0f;
}

bool ProjectionIsPerspective() {
    const auto state = Load(Target() + kGxStateSlot);
    return state && *reinterpret_cast<const int*>(state + kGxProjectionType) == 0;
}

Vec ReadVec(std::uintptr_t p) { const auto* v = reinterpret_cast<const float*>(p); return {v[0], v[1], v[2]}; }

// ATT-0144: the camera view V1 (camera context 0x17 +14), which 20390 copies
// into j3dSys for every main draw list.
const float* CameraView() {
    const auto view = reinterpret_cast<std::uintptr_t (*)()>(Target() + kCameraViewMtx)();
    // The view sits at context +14; validate the 8-aligned context itself.
    return IsGuestPointer(view - 0x14) ? reinterpret_cast<const float*>(view) : nullptr;
}

// True while j3dSys holds the camera view, i.e. a main draw list rather than a
// renderer with its own camera (layout 3D, Mario's light-view shadow).
bool MainViewActive() {
    // The camera context lives for the scene; pass 1 reuses the pointer taken
    // when it began, and the compare below reads its current contents.
    const float* camera = sPass == 1 && sTwinCameraView ? sTwinCameraView : CameraView();
    return camera && std::memcmp(camera, reinterpret_cast<const void*>(Target() + kViewMatrix), sizeof(float) * 12) == 0;
}

// Eases a smoothed up toward the live one: proportional, never slower than
// 0.02 rad per frame, so a gravity flip settles in well under a second.
Vec FollowUp(Vec current, Vec target) {
    const float angle = std::acos(std::clamp(Dot(current, target), -1.0f, 1.0f));
    return planet::StepDirection(current, target, std::max(0.02f, angle * 0.12f));
}

using splitmath::MinimalRotation;

// ATT-0206: per-player rig rotations carried frame by frame since split entry.
splitmath::RigTransport sRigTransport[2]{};

// ATT-0155: change only reference translation. Scaling all three target-view
// coordinates preserves perspective screen placement, while rotation and
// player-local pitch remain the accepted entry frame. The native distance is
// sampled before shared fit (EVD-0060 pose/EVD-0302); no extra native calc.
// ATT-0208 / EVD-0509: a half shows about half the native horizontal field,
// so ease to a farther distance than native.
float HalfTarget(float native) {
    const float scale = kSplitHalfDistanceEnabled ? kSplitHalfDistanceScale : 1.0f;
    return kSplitHalfDistanceCapEnabled ? splitmath::HalfDistance(native, scale, kSplitHalfDistanceMaxExtra) : native * scale;
}

void BuildFramedReference(float amount, float out[12]) {
    std::memcpy(out,sAnchorView,sizeof(sAnchorView));
    if (!kSplitNativeDistanceFramingEnabled) return;
    const float target=HalfTarget(sAnchorNativeDistance);
    splitmath::FrameDistance(sAnchorView,sAnchorPoint,target,amount,out,
                             kSplitPullOutEnabled ? kSplitPullOutMaxRatio : 1.0f);
}

// Pass 1 views the world from P2 through the rig G(w) = inverse(Q)(w - P2) + P1,
// V2 = V1 * G. With V1(x) = R1 x + T1, D = V1 * G * inverse(V1) has rotation
// Rd = R1 Qt R1t and translation td = T1 + R1 P1 - R1 Qt P2 - Rd T1. Q = I (rig
// off or equal gravity) reduces td to ATT-0137's -R1 (P2 - P1).
bool BuildPassView() {
    sClipRigValid = false;
    void* actors[2] = {GetFirstPlayer(), GetSecondPlayer()};
    if (!IsValidMarioActor(actors[0]) || !IsValidMarioActor(actors[1])) { sSmoothUpValid = false; return false; }
    const Vec p1 = ReadVec(reinterpret_cast<std::uintptr_t>(actors[0]) + kActorCameraPoint);
    const Vec liveP2 = ReadVec(reinterpret_cast<std::uintptr_t>(actors[1]) + kActorCameraPoint);
    // ATT-0216: while Luigi's death plays (before his bubble), his half holds
    // the point and up it had when he died instead of following him down.
    const DeathPhase p2Phase = GetP2DeathPhase();
    const bool p2Held = kSplitPermanentEnabled && kSplitDeathKeepsSplitEnabled &&
                        (p2Phase == DeathPhase::Animating || p2Phase == DeathPhase::Ghost);
    if (!p2Held) sP2HeldValid = false;
    else if (!sP2HeldValid && planet::Finite(liveP2)) { sP2Held = liveP2; sP2HeldValid = true; }
    const Vec p2 = p2Held && sP2HeldValid ? sP2Held : liveP2;
    // ATT-0144 derives D from the camera view that J3D's buffers were built
    // with; the last lists of pass 0 may leave another view in j3dSys.
    const auto* view = kSplitJ3dTwinEnabled ? CameraView() : reinterpret_cast<const float*>(Target() + kViewMatrix);
    if (!view) { sSmoothUpValid = false; return false; }
    const Vec rows[3] = {{view[0], view[1], view[2]}, {view[4], view[5], view[6]}, {view[8], view[9], view[10]}};
    const Vec translation{view[3], view[7], view[11]};
    const Vec delta = p2 - p1;
    if (!planet::Finite(p1) || !planet::Finite(p2) || !planet::Finite(translation) ||
        Length(delta) >= kMaxViewDelta) { sSmoothUpValid = false; return false; }
    sViewDelta[0] = delta.x; sViewDelta[1] = delta.y; sViewDelta[2] = delta.z;
    const float rigBlend = kSplitCameraMatchEnabled ? std::clamp(sRigBlend, 0.0f, 1.0f) : 1.0f;
    float qt[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    sRigAngle = 0;
    if (kSplitGravityRigEnabled) {
        float up[2][3];
        if (GetCoopPlayerUp(actors[0], up[0]) && GetCoopPlayerUp(actors[1], up[1])) {
            const Vec live[2] = {{up[0][0], up[0][1], up[0][2]}, {up[1][0], up[1][1], up[1][2]}};
            for (int i = 0; i < 2; ++i) {
                if (i == 1 && p2Held && sSmoothUpValid) continue;
                sSmoothUp[i] = sSmoothUpValid ? FollowUp(sSmoothUp[i], live[i]) : live[i];
            }
            sSmoothUpValid = true;
        }
    }
    // ATT-0215: a permanent split (re)opens Luigi's half from a fresh level
    // view at Luigi, at the half distance, along the native heading only.
    if (kSplitCameraAnchorEnabled && !sAnchorValid && sSmoothUpValid &&
        kSplitPermanentEnabled && kSplitFreshReferenceEnabled) {
        const auto sample = GetCoopFramingSample();
        const float native = sample.valid && sample.nativeDistance >= 100 ? sample.nativeDistance : kFreshReferenceNativeDistance;
        float fresh[12];
        if (splitmath::SynthesizeFollowView(view, p2, sSmoothUp[1], HalfTarget(native), kSplitFreshReferenceElevation, fresh)) {
            std::memcpy(sAnchorView, fresh, sizeof(sAnchorView));
            sAnchorPoint = p2;
            sAnchorUp = sSmoothUp[1];
            sAnchorNativeDistance = native;
            sAnchorValid = true;
            const Vec entryRight{sAnchorView[0], sAnchorView[1], sAnchorView[2]};
            for (int i = 0; i < 2; ++i) splitmath::ResetTransport(sRigTransport[i], sAnchorUp, sSmoothUp[i], entryRight);
            static unsigned sFreshLogs = 0;
            if (sFreshLogs++ < 64) {
                const Vec look = splitmath::ViewForward(view);
                const Vec nativePlace = splitmath::ViewPoint(view, p2);
                Logging.Log("[GalaxyCoop:smg1] ATT-0215 fresh P2 reference distance=%.1f native=%.1f sampled=%d elevation=%.2f native_look_up=%.2f native_p2_distance=%.1f point=(%.1f,%.1f,%.1f) kill=1",
                    static_cast<double>(HalfTarget(native)), static_cast<double>(native), sample.valid ? 1 : 0,
                    static_cast<double>(kSplitFreshReferenceElevation),
                    static_cast<double>(Dot(planet::Unit(look), planet::Unit(sAnchorUp))),
                    static_cast<double>(Length(nativePlace)),
                    static_cast<double>(p2.x), static_cast<double>(p2.y), static_cast<double>(p2.z));
            }
        }
    }
    if (kSplitCameraAnchorEnabled && !sAnchorValid && sSmoothUpValid) {
        // ATT-0153 / EVD-0287: TickSplitPolicy opens at draw time, before
        // the next calc forces P1 as leader. This view may still belong to
        // P2. Carry the actual successful calc owner; policy is not evidence
        // of which player produced the current view.
        const int owner = kSplitCameraOwnerReferenceEnabled ? GetCoopNativeViewOwnerIndex() : 0;
        if (owner < 0 || owner > 1) return false;
        std::memcpy(sAnchorView, view, sizeof(sAnchorView));
        sAnchorPoint = owner == 0 ? p1 : p2;
        sAnchorUp = sSmoothUp[owner];
        if (kSplitReferenceUprightEnabled) {
            const auto upright = splitmath::UprightReference(sAnchorView, sAnchorPoint, sAnchorUp, kReferenceMinElevation);
            if (upright.changed && sPolicyLogs < 64)
                Logging.Log("[GalaxyCoop:smg1] ATT-0205 split reference upright owner=p%d elevation=%.2f->%.2f roll=%.1f kill=1",
                    owner + 1, static_cast<double>(upright.elevationBefore),
                    static_cast<double>(upright.elevationAfter), static_cast<double>(upright.rollDegrees));
        }
        // ATT-0207: after ATT-0205's orbit, which keeps the point's placement.
        if (kSplitReferenceCentredEnabled) {
            const float pan = splitmath::CentreReference(sAnchorView, sAnchorPoint);
            if (std::abs(pan) >= 1.0f && sPolicyLogs < 64)
                Logging.Log("[GalaxyCoop:smg1] ATT-0207 split reference centred owner=p%d pan=%.1f kill=1",
                    owner + 1, static_cast<double>(pan));
        }
        const auto sample=GetCoopFramingSample();
        sAnchorNativeDistance=sample.valid ? sample.nativeDistance : 0;
        sAnchorValid = true;
        const Vec entryRight{sAnchorView[0], sAnchorView[1], sAnchorView[2]};
        for (int i = 0; i < 2; ++i) splitmath::ResetTransport(sRigTransport[i], sAnchorUp, sSmoothUp[i], entryRight);
        if (sPolicyLogs++ < 64)
            Logging.Log("[GalaxyCoop:smg1] ATT-0149 P2 camera reference captured ATT-0153 owner=p%d point=(%.1f,%.1f,%.1f) kill=%d",
                owner + 1, static_cast<double>(sAnchorPoint.x), static_cast<double>(sAnchorPoint.y),
                static_cast<double>(sAnchorPoint.z), kSplitCameraOwnerReferenceEnabled ? 1 : 0);
        if (kSplitNativeDistanceFramingEnabled && sPolicyLogs<64)
            Logging.Log("[GalaxyCoop:smg1] ATT-0155 split distance entry=%.1f native=%.1f ATT-0208 target=%.1f reveal_ease=1 kill=1",
                static_cast<double>(Length(splitmath::ViewPoint(sAnchorView,sAnchorPoint))),
                static_cast<double>(sAnchorNativeDistance),
                static_cast<double>(HalfTarget(sAnchorNativeDistance)));
    }
    const bool anchored = kSplitCameraAnchorEnabled && sAnchorValid;
    // ATT-0213: in a permanent split the frozen reference would never turn,
    // so it orbits its point behind P2's movement. P2's heading is its camera
    // point's step since the last drawn frame, carried into reference space by
    // the transpose of P2's rig rotation (desired = reference * q^T).
    if (kSplitPermanentEnabled && kSplitP2FollowEnabled && anchored && sRigTransport[1].valid && sLastP2PointValid) {
        const Vec step = p2 - sLastP2Point;
        if (planet::Finite(step) && Length(step) < kFollowMaxStepLength) {
            const float* q = sRigTransport[1].q;
            const Vec heading{q[0] * step.x + q[3] * step.y + q[6] * step.z,
                              q[1] * step.x + q[4] * step.y + q[7] * step.z,
                              q[2] * step.x + q[5] * step.y + q[8] * step.z};
            const float yaw = splitmath::FollowYaw(splitmath::ViewForward(sAnchorView), heading, sAnchorUp, kFollowTuning);
            if (yaw != 0.0f) splitmath::OrbitViewAbout(sAnchorView, sAnchorPoint, planet::Unit(sAnchorUp), yaw);
        }
    }
    sLastP2Point = p2;
    sLastP2PointValid = anchored;
    float framedReference[12];
    if (anchored) BuildFramedReference(sRevealAmount,framedReference);
    const float* reference = anchored ? framedReference : view;
    const Vec referenceRows[3] = {{reference[0], reference[1], reference[2]},
                                  {reference[4], reference[5], reference[6]},
                                  {reference[8], reference[9], reference[10]}};
    const Vec referenceP1 = anchored ? sAnchorPoint : p1;
    const Vec matchedP2 = referenceP1 + (p2 - referenceP1) * rigBlend;
    // ATT-0206: once per drawn frame (pass 1), after sSmoothUp moved.
    const bool transported = kSplitIncrementalRigEnabled && anchored && rigBlend >= 1.0f &&
                             sSmoothUpValid && sRigTransport[0].valid && sRigTransport[1].valid;
    if (transported)
        for (int i = 0; i < 2; ++i) splitmath::AdvanceTransport(sRigTransport[i], sSmoothUp[i], referenceRows[0]);
    if (kSplitGravityRigEnabled && sSmoothUpValid) {
        const Vec referenceUp = anchored ? sAnchorUp : sSmoothUp[0];
        float q[9];
        if (transported) std::memcpy(q, sRigTransport[1].q, sizeof(q));
        else MinimalRotation(referenceUp, sSmoothUp[1], referenceRows[0], rigBlend, q);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) qt[r * 3 + c] = q[c * 3 + r];
        sRigAngle = std::acos(std::clamp(Dot(referenceUp, sSmoothUp[1]), -1.0f, 1.0f)) * rigBlend;
    }
    // Desired P2 view = frozen split-entry Vref * Gref. D maps the current
    // P1 view to that desired view, so P1 movement no longer moves P2's half.
    float desired[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            desired[r * 3 + c] = referenceRows[r].x * qt[c] + referenceRows[r].y * qt[3 + c] + referenceRows[r].z * qt[6 + c];
    float rd[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            rd[r * 3 + c] = desired[r * 3] * rows[c].x + desired[r * 3 + 1] * rows[c].y + desired[r * 3 + 2] * rows[c].z;
    const float t1[3] = {translation.x, translation.y, translation.z};
    float desiredT[3];
    for (int r = 0; r < 3; ++r) {
        desiredT[r] = reference[r * 4 + 3] + Dot(referenceRows[r], referenceP1) -
                      (desired[r * 3] * matchedP2.x + desired[r * 3 + 1] * matchedP2.y + desired[r * 3 + 2] * matchedP2.z);
        const float moved = desiredT[r] - (rd[r * 3] * t1[0] + rd[r * 3 + 1] * t1[1] + rd[r * 3 + 2] * t1[2]);
        if (!std::isfinite(moved)) return false;
        sTd[r] = moved;
    }
    for (float value : rd) if (!std::isfinite(value)) return false;
    std::memcpy(sRd, rd, sizeof(sRd));
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) sP2InputView[r * 4 + c] = desired[r * 3 + c];
        sP2InputView[r * 4 + 3] = desiredT[r];
    }
    const float trace = std::clamp((rd[0] + rd[4] + rd[8] - 1.0f) * 0.5f, -1.0f, 1.0f);
    sRigRotates = std::acos(trace) > 1.0e-3f;
    // H = inverse(V1) * V2 maps a world point into the current P1 clipping
    // volume exactly as the independent P2 view sees it.
    const float currentR[9] = {view[0], view[1], view[2], view[4], view[5], view[6], view[8], view[9], view[10]};
    float clipTranslation[3]{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c)
            sClipQt[r * 3 + c] = currentR[r] * desired[c] + currentR[3 + r] * desired[3 + c] + currentR[6 + r] * desired[6 + c];
        clipTranslation[r] = currentR[r] * (desiredT[0] - t1[0]) + currentR[3 + r] * (desiredT[1] - t1[1]) + currentR[6 + r] * (desiredT[2] - t1[2]);
    }
    sClipP1 = {clipTranslation[0], clipTranslation[1], clipTranslation[2]};
    sClipP2 = {0, 0, 0};
    sClipRigValid = true;
    return true;
}

// Use precisely the same frozen frame and minimal gravity rotation as P2.
bool BuildLeaderView(const float native[12], Vec point, Vec up, float amount, float out[12]) {
    float q[9];
    float reference[12];BuildFramedReference(amount,reference);
    const Vec right{reference[0],reference[1],reference[2]};
    // ATT-0206: P1's transport is carried in BuildPassView alongside P2's.
    if (kSplitIncrementalRigEnabled && sRigTransport[0].valid) std::memcpy(q,sRigTransport[0].q,sizeof(q));
    else MinimalRotation(sAnchorUp,up,right,1.0f,q);
    const Vec placement=splitmath::ViewPoint(reference,sAnchorPoint);
    const float target[3]={placement.x,placement.y,placement.z};
    float desired[12];
    for (int r=0;r<3;++r) {
        for (int c=0;c<3;++c)
            desired[r*4+c]=reference[r*4]*q[c*3]+reference[r*4+1]*q[c*3+1]+reference[r*4+2]*q[c*3+2];
        desired[r*4+3]=target[r]-(desired[r*4]*point.x+desired[r*4+1]*point.y+desired[r*4+2]*point.z);
    }
    return splitmath::BlendPlayerView(native,desired,point,amount,out);
}

HOOK_DEFINE_TRAMPOLINE(SplitLeaderView) {
    static void Callback(std::uintptr_t context, const float* native) {
        const unsigned serial=GetCoopOrdinaryCameraSerial();
        const auto sample=GetCoopFramingSample();
        // A fresh ordinary calc must own this setter call. Other cameras,
        // repeated calls, bindings/death and invalid frames retain native data.
        void* p1=GetFirstPlayer(); void* p2=GetSecondPlayer();
        if (!kSplitP1AnchoredViewEnabled || !sAnchorValid || sRevealAmount<=0 ||
            !sample.valid || IsCoopEventCamera() || serial==sLeaderViewSerial ||
            GetCoopNativeViewOwnerIndex()!=0 || IsP2Parked() || !HasActivePlayerPair() ||
            !IsValidMarioActor(p1) || !IsValidMarioActor(p2) ||
            GetP1DeathPhase()!=DeathPhase::Alive || GetP2DeathPhase()!=DeathPhase::Alive ||
            GetCoopPipeLeader() || HasActiveBinding(reinterpret_cast<std::uintptr_t>(p1)) ||
            HasActiveBinding(reinterpret_cast<std::uintptr_t>(p2)) ||
            !IsGuestPointer(context) || Load(context)!=Target()+kCameraContextVtable) {
            Orig(context,native); return;
        }
        const auto* mainView=CameraView();
        if (!mainView || reinterpret_cast<std::uintptr_t>(mainView)-0x14!=context) {
            Orig(context,native); return;
        }
        sLeaderViewSerial=serial;
        float local[12], liveUp[3];
        if (!GetCoopPlayerUp(p1,liveUp)) { Orig(context,native); return; }
        const Vec up=sSmoothUpValid ? sSmoothUp[0] : Vec{liveUp[0],liveUp[1],liveUp[2]};
        const Vec point=ReadVec(reinterpret_cast<std::uintptr_t>(p1)+kActorCameraPoint);
        // ATT-0213: a permanent split keeps P1 on its own native camera (which
        // follows and avoids walls), only eased out to the half distance.
        const bool built = kSplitPermanentEnabled ?
            splitmath::FrameDistance(native,point,HalfTarget(sample.nativeDistance),
                sRevealAmount,local,kSplitPullOutEnabled ? kSplitPullOutMaxRatio : 1.0f) :
            BuildLeaderView(native,point,up,sRevealAmount,local);
        if (!built) { Orig(context,native); return; }
        // Orig copies the local view and computes its matching inverse, before
        // native clipping and J3D construction. No per-pass GPU arena changes.
        Orig(context,local);
        const int bin=static_cast<int>(sRevealAmount*4);
        if (bin!=sLeaderViewBin && sLeaderViewLogs++<32) {
            sLeaderViewBin=bin;
            Logging.Log("[GalaxyCoop:smg1] ATT-0154 P1 anchored view amount=%.3f kill=1",static_cast<double>(sRevealAmount));
        }
    }
};

bool OutsideVolume(std::uintptr_t judge, const float* pos, float radius, int level) {
    const auto* plane = reinterpret_cast<const float*>(
        judge + (level == 0 ? kJudgeLevel0Planes : kJudgeLevelPlanes + static_cast<std::intptr_t>(level) * kJudgeLevelStride));
    const float limit = -radius;
    for (int i = 0; i < 6; ++i, plane += 4)
        if (plane[1] * pos[1] + plane[0] * pos[0] + plane[2] * pos[2] - plane[3] < limit) return true;
    return false;
}

bool PassViewApplies() { return sPass == 1 && sViewOffsetValid && ProjectionIsPerspective(); }

bool UsesCornerBackground() {
    return kSplitBottomCornerWipeEnabled &&
        (!kSplitCornerTransitionOnlyEnabled || sRevealAmount < 1.0f);
}

HOOK_DEFINE_REPLACE(SplitScissor) {
    static void Callback(int left, int top, int width, int height) {
        // ATT-0150: keep the right viewport at its final scale. Only its
        // scissor grows from the bottom center toward the top right.
        if (kSplitBottomCornerWipeEnabled && sPass == 1 && sRevealAmount < 1.0f) {
            const int frameW = static_cast<int>(sFrameWidth + 0.5f);
            const int frameH = static_cast<int>(sFrameHeight + 0.5f);
            const int revealLeft = frameW / 2;
            const int revealRight = std::min(frameW, revealLeft +
                std::max(1, static_cast<int>(std::ceil((frameW - revealLeft) * sRevealAmount))));
            const int revealTop = std::max(0, frameH -
                std::max(1, static_cast<int>(std::ceil(frameH * sRevealAmount))));
            const int x0 = std::max(left, revealLeft);
            const int y0 = std::max(top, revealTop);
            const int x1 = std::min(left + width, revealRight);
            const int y1 = std::min(top + height, frameH);
            if (x1 > x0 && y1 > y0) {
                left = x0; top = y0; width = x1 - x0; height = y1 - y0;
            } else {
                // A disjoint inset still needs a valid GX rectangle. One
                // corner pixel is safer than an underflowing width/height.
                left = revealLeft; top = revealTop; width = height = 1;
            }
            const int bin = static_cast<int>(sRevealAmount * 4.0f);
            if (bin != sLastWipeBin && sWipeLogs++ < 40) {
                sLastWipeBin = bin;
                Logging.Log("[GalaxyCoop:smg1] ATT-0150 corner wipe progress=%.2f rect=(%d,%d,%d,%d) kill=1",
                    static_cast<double>(sRevealAmount), left, top, width, height);
            }
        }
        // Reproduce main+0x20520 exactly. Its opening ADRP cannot be moved
        // into a trampoline, and there is no safe Orig call for a replacement.
        const auto slot = Load(Target() + kGxScissorStateRoot);
        const auto state = Load(slot);
        if (!IsGuestPointer(state)) return;
        auto* words = reinterpret_cast<std::uint32_t*>(state + 0xF0);
        const std::uint32_t x0 = static_cast<std::uint32_t>(left + 0x156) & 0x7FFu;
        const std::uint32_t y0 = static_cast<std::uint32_t>(top + 0x156) & 0x7FFu;
        const std::uint32_t x1 = static_cast<std::uint32_t>(left + width + 0x155) & 0x7FFu;
        const std::uint32_t y1 = static_cast<std::uint32_t>(top + height + 0x155) & 0x7FFu;
        words[0] = (words[0] & 0xFF800800u) | (x0 << 12) | y0;
        words[1] = (words[1] & 0xFF800800u) | (x1 << 12) | y1;
        reinterpret_cast<void (*)()>(Target() + kFifoBegin)();
        reinterpret_cast<void (*)(std::uint8_t)>(Target() + kFifoWriteU8)(0x61);
        reinterpret_cast<void (*)()>(Target() + kFifoBegin)();
        reinterpret_cast<void (*)(std::uint32_t)>(Target() + kFifoWriteBpU32)(words[0]);
        reinterpret_cast<void (*)()>(Target() + kFifoBegin)();
        reinterpret_cast<void (*)(std::uint8_t)>(Target() + kFifoWriteU8)(0x61);
        reinterpret_cast<void (*)()>(Target() + kFifoBegin)();
        reinterpret_cast<void (*)(std::uint32_t)>(Target() + kFifoWriteBpU32)(words[1]);
        reinterpret_cast<void (*)()>(Target() + kFifoEnd)();
    }
};

HOOK_DEFINE_REPLACE(SplitViewport) {
    static void Callback(float left, float top, float wd, float ht, float nearZ, float farZ) {
        // Every viewport inside a pass follows that pass's physical width.
        // ATT-0151 keeps pass zero full-screen only under a partial mask.
        if (sPass >= 0) {
            const float scale = sPassW[sPass] / sFrameWidth;
            left = sPassX[sPass] + left * scale;
            wd *= scale;
        }
        const auto state = *reinterpret_cast<const std::uintptr_t*>(Target() + kGxStateSlot);
        auto* fields = reinterpret_cast<float*>(state + 0x4F4);
        fields[0] = left; fields[1] = top; fields[2] = wd; fields[3] = ht; fields[4] = nearZ; fields[5] = farZ;
        reinterpret_cast<void (*)()>(Target() + kGxWriteXfViewport)();
    }
};

// ATT-0226 / EVD-0537, SMG1 main SHA-256 76f1b4d8..., NSO offsets (Ghidra):
// bloom (preDraw 0x122948, postDraw 0xBE56C) and 0x13DA90 draw scratch tiles at
// frame coordinates, which SplitViewport maps into the pass's half, then copy
// them back through 0xBE908(tex, x, y, w, h, fmt, mip, clear, dstW, dstH, mode)
// with full-frame rectangles, so each half read the other half's contents (the
// garden glow bands and flicker, EVD-0518). The copy rectangle now follows the
// viewport's mapping; dst sizes of 0 follow it and 0xBF09C re-inits the texture,
// the path dynamic resolution already uses. Even x and width, as the native
// multiple-of-16 frame widths give. Opening sub/stp, no references into
// 0xBE90C..0xBE91C.
constexpr bool kAtt0226SplitCopyEnabled = true;
constexpr std::uintptr_t kEfbCopy = 0xBE908;
unsigned sCopyLogs = 0;

HOOK_DEFINE_TRAMPOLINE(SplitEfbCopy) {
    static void Callback(std::uintptr_t texture, int x, int y, std::uint32_t w, std::uint32_t h, int format,
                         std::uint32_t mip, int clear, std::uint32_t dstW, std::uint32_t dstH, int mode) {
        if (sPass >= 0) {
            const float scale = sPassW[sPass] / sFrameWidth;
            const int mappedX = static_cast<int>(sPassX[sPass] + static_cast<float>(x) * scale) & ~1;
            const int mappedW = std::max(2, static_cast<int>(static_cast<float>(w) * scale) & ~1);
            if (sCopyLogs < 24) {
                ++sCopyLogs;
                Logging.Log("[GalaxyCoop:smg1] ATT-0226 split copy pass=%d x=%d->%d y=%d w=%u->%d h=%u mip=%u dst=(%u,%u) kill=1",
                    sPass, x, mappedX, y, w, mappedW, h, mip, dstW, dstH);
            }
            x = mappedX;
            w = static_cast<std::uint32_t>(mappedW);
        }
        Orig(texture, x, y, w, h, format, mip, clear, dstW, dstH, mode);
    }
};

HOOK_DEFINE_REPLACE(SplitProjection) {
    static void Callback() {
        using Root = std::uintptr_t (*)();
        using ByIndex = std::uintptr_t (*)(std::uintptr_t, int);
        using SetProjection = void (*)(const float*, int);
        const auto base = Target();
        const auto context = reinterpret_cast<ByIndex>(base + kNameObjByIndex)(
            reinterpret_cast<Root>(base + kNameObjRoot)(), kCameraContextIndex);
        const auto* matrix = reinterpret_cast<const float*>(context + kCameraProjection);
        const auto set = reinterpret_cast<SetProjection>(base + kGxSetProjection);
        if (sPass < 0 || !IsGuestPointer(context)) { set(matrix, 0); return; }
        // Perspective GXSetProjection reads indices 0,2,5,6,10,11. Narrowing the
        // viewport keeps the vertical FOV only if x scale grows by W/width.
        float local[16]{};
        std::memcpy(local, matrix, sizeof(float) * 12);
        local[0] *= sFrameWidth / sPassW[sPass];
        if (UsesCornerBackground() && sPass == 0)
            local[2] += sRevealAmount * 0.5f;
        if (kDiag0212PassVisibilityEnabled) {
            const float values[4]{local[0], local[2], local[5], local[6]};
            std::memcpy(sPassProj[sPass], values, sizeof(values));
            sPassProjValid[sPass] = true;
        }
        set(local, 0);
    }
};

// ATT-0137/0138: every pass-1 modelview becomes D * M. Without rotation D only
// adds td to the translation column. Ortho loads (2D insets, screen effects)
// are not world views and stay untouched.
HOOK_DEFINE_TRAMPOLINE(SplitPosMtx) {
    static void Callback(const float* mtx, std::uint32_t id) {
        if (kDiag0191PassViewProbeEnabled && sPass >= 0) ++sPosLoads[sPass];
        if (!PassViewApplies()) { Orig(mtx, id); return; }
        if (kDiag0191PassViewProbeEnabled) ++sPosCorrected;
        float local[12];
        if (!sRigRotates) {
            std::memcpy(local, mtx, sizeof(local));
            for (int row = 0; row < 3; ++row) local[row * 4 + 3] += sTd[row];
        } else {
            for (int row = 0; row < 3; ++row)
                for (int column = 0; column < 4; ++column)
                    local[row * 4 + column] = sRd[row * 3] * mtx[column] + sRd[row * 3 + 1] * mtx[4 + column] +
                                              sRd[row * 3 + 2] * mtx[8 + column] + (column == 3 ? sTd[row] : 0.0f);
        }
        Orig(local, id);
    }
};

// ATT-0138: normal matrices take only the rotation of D (orthonormal, so it is
// its own inverse transpose). Translation-only passes leave normals as loaded.
HOOK_DEFINE_TRAMPOLINE(SplitNrmMtx) {
    static void Callback(const float* mtx, std::uint32_t id) {
        if (!sRigRotates || !PassViewApplies()) { Orig(mtx, id); return; }
        float local[12];
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column)
                local[row * 4 + column] = sRd[row * 3] * mtx[column] + sRd[row * 3 + 1] * mtx[4 + column] +
                                          sRd[row * 3 + 2] * mtx[8 + column];
            local[row * 4 + 3] = mtx[row * 4 + 3];
        }
        Orig(local, id);
    }
};

// ATT-0144: D applied to one 3x4 modelview, and its rotation to one normal
// matrix whose rows are rowStride floats apart (4 for a 3x4, 3 for a 3x3).
void ViewAtP2(const float* m, float* out) {
    if (!sRigRotates) {
        std::memcpy(out, m, sizeof(float) * 12);
        for (int row = 0; row < 3; ++row) out[row * 4 + 3] += sTd[row];
        return;
    }
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 4; ++column)
            out[row * 4 + column] = sRd[row * 3] * m[column] + sRd[row * 3 + 1] * m[4 + column] +
                                    sRd[row * 3 + 2] * m[8 + column] + (column == 3 ? sTd[row] : 0.0f);
}

void RotateNormal(const float* n, int rowStride, float* out) {
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column)
            out[row * 3 + column] = sRd[row * 3] * n[column] + sRd[row * 3 + 1] * n[rowStride + column] +
                                    sRd[row * 3 + 2] * n[rowStride * 2 + column];
}

bool J3dTwinApplies() { return kSplitJ3dTwinEnabled && PassViewApplies() && MainViewActive(); }

// One permanent arena from the game's fallback allocator: its nn::mem arena
// lies inside the range GXSetArray accepts, and no scene heap owns it.
bool EnsureTwinArena() {
    if (sTwinArena) return true;
    if (sTwinArenaFailed) return false;
    const auto alloc = reinterpret_cast<std::uintptr_t (*)(std::size_t, int)>(Target() + kFallbackAlloc);
    const std::uintptr_t arena = alloc(kTwinHalfBytes * 2, 0x40);
    const auto start = *reinterpret_cast<const std::uintptr_t*>(Target() + kGpuRangeStart);
    const auto end = *reinterpret_cast<const std::uintptr_t*>(Target() + kGpuRangeEnd);
    const bool ok = arena && arena >= start && arena + kTwinHalfBytes * 2 <= end;
    Logging.Log("[GalaxyCoop:smg1] ATT-0144 twin arena=0x%lx bytes=0x%lx gpu=[0x%lx,0x%lx] ok=%d kill=1",
        static_cast<unsigned long>(arena), static_cast<unsigned long>(kTwinHalfBytes * 2),
        static_cast<unsigned long>(start), static_cast<unsigned long>(end), ok ? 1 : 0);
    if (!ok) { sTwinArenaFailed = true; return false; }
    sTwinArena = arena;
    return true;
}

// Called as pass 1 starts: the other half keeps last frame's twins alive for
// the GPU, the same two-frame reuse J3D's arr0/arr1 swap relies on.
void BeginTwinFrame() {
    sTwinHalf ^= 1u;
    sTwinUsed = 0;
    sTwinNrmFollows = false;
    sTwinCameraView = CameraView();
    // Generation 0 marks never-used slots, so skip it on wrap-around.
    if (++sTwinGeneration == 0) sTwinGeneration = 1;
}

// The pointer 800/79794 pass for attr 15 must be the current model's view-space
// draw buffer: j3dSys +40 model, mode other than world matrices, arr1[view].
bool IsTwinSource(std::uintptr_t array, unsigned& count, bool countWorld = true) {
    SplitTimingScope timing(SplitTimingMetric::TwinSelector);
    const auto base = Target();
    const auto model = Load(base + kViewMatrix + kJ3dSysModel);
    if (!model) return false;
    const auto vtable = Load(model) - base;
    if (vtable != kJ3dModelVtable && vtable != kJ3dModelXVtable && vtable != kJ3dModelLayerVtable)
        return false;
    if ((*reinterpret_cast<const std::uint32_t*>(model + 0x10) & 3) == 1) {
        if (countWorld) ++sTwinStats.world;
        return false;
    }
    const auto data = Load(model + 8);
    if (!IsGuestPointer(data)) return false;
    count = *reinterpret_cast<const std::uint16_t*>(data + 0x78);
    if (count == 0 || count > kMaxDrawMatrices) return false;
    const auto buffer = Load(model + 0x98);
    if (!buffer || Load(buffer) - base != kJ3dMtxBufferVtable) return false;
    const auto views = *reinterpret_cast<const std::uint32_t*>(buffer + 0x60);
    const auto view = *reinterpret_cast<const std::uint32_t*>(buffer + 0x64);
    if (views == 0 || views > 4 || view >= views) return false;
    const auto drawArrays = Load(buffer + 0x38);
    if (!drawArrays || drawArrays == base + kConcatViewDrawArray) return false;
    const auto expected = kSplitJ3dPackedTableEnabled
        ? LoadDrawArrayEntry(drawArrays, view) : Load(drawArrays + view * 8);
    return expected == array;
}

// Pass-1 copy of a draw (position) or normal buffer, cached per source for the
// frame since every shape of a model shares its buffers. 0 on overflow.
std::uintptr_t TwinOf(std::uintptr_t source, unsigned count, bool position) {
    SplitTimingScope timing(SplitTimingMetric::TwinProducer);
    unsigned slot = static_cast<unsigned>((source >> 5) * 2654435761u) % kTwinCacheSlots;
    for (unsigned probe = 0;; ++probe, slot = (slot + 1) % kTwinCacheSlots) {
        if (probe == 32) { ++sTwinStats.overflow; return 0; }
        if (sTwinCache[slot].generation != sTwinGeneration) break;
        if (sTwinCache[slot].source == source) { ++sTwinStats.cached; return sTwinCache[slot].twin; }
    }
    const std::size_t stride = position ? 0x30 : 0x24;
    const std::size_t bytes = (count * stride + 0x3F) & ~static_cast<std::size_t>(0x3F);
    if (sTwinUsed + bytes > kTwinHalfBytes) { ++sTwinStats.overflow; return 0; }
    const std::uintptr_t twin = sTwinArena + sTwinHalf * kTwinHalfBytes + sTwinUsed;
    sTwinUsed += bytes;
    sTwinPeak = std::max(sTwinPeak, sTwinUsed);
    const auto* in = reinterpret_cast<const float*>(source);
    auto* out = reinterpret_cast<float*>(twin);
    if (position) {
        for (unsigned i = 0; i < count; ++i) ViewAtP2(in + i * 12, out + i * 12);
        ++sTwinStats.pos;
    } else {
        for (unsigned i = 0; i < count; ++i) RotateNormal(in + i * 9, 3, out + i * 9);
        ++sTwinStats.nrm;
    }
    sTwinCache[slot] = {source, twin, sTwinGeneration};
    return twin;
}

// DIAG-0212: bit k set when the box around the joint origins of `count` 3x4
// view matrices (moved by D in pass 1, as the twins are) lies outside one plane
// of the pass's perspective frustum (or behind the eye) by kCensusMargins[k].
// GX perspective: x' = p0*x + p2*z, y' = p5*y + p6*z, w = -z; inside n.v <= 0.
unsigned CensusOutside(std::uintptr_t array, unsigned count, int pass) {
    float lo[3]{1e30f, 1e30f, 1e30f}, hi[3]{-1e30f, -1e30f, -1e30f};
    const auto* m = reinterpret_cast<const float*>(array);
    for (unsigned i = 0; i < count; ++i, m += 12) {
        float t[3]{m[3], m[7], m[11]};
        if (pass == 1) {
            float u[3];
            for (int row = 0; row < 3; ++row)
                u[row] = (sRigRotates ? sRd[row * 3] * t[0] + sRd[row * 3 + 1] * t[1] + sRd[row * 3 + 2] * t[2] : t[row]) +
                         sTd[row];
            std::memcpy(t, u, sizeof(t));
        }
        for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], t[a]); hi[a] = std::max(hi[a], t[a]); }
    }
    const float* p = sPassProj[pass];
    const float planes[5][3] = {{p[0], 0, p[1] + 1}, {-p[0], 0, 1 - p[1]}, {0, p[2], p[3] + 1}, {0, -p[2], 1 - p[3]}, {0, 0, 1}};
    unsigned mask = 0;
    for (const auto& n : planes) {
        float d = 0;
        for (int a = 0; a < 3; ++a) d += n[a] >= 0 ? n[a] * lo[a] : n[a] * hi[a];
        const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (unsigned k = 0; k < 3; ++k)
            if (d > kCensusMargins[k] * length) mask |= 1u << k;
    }
    return mask;
}

// DIAG-0212: one attr-15 GXSetArray in a fully open split pass. A model's
// shapes usually arrive back to back, so its verdict is reused while the source
// array repeats; a new source is one "run".
void CensusShape(std::uintptr_t array) {
    if (sPass < 0 || sRevealAmount < 1.0f || !sPassProjValid[sPass] || (sPass == 1 && !sViewOffsetValid)) return;
    auto& census = sCensus[sPass];
    ++census.shapes;
    unsigned count = 0;
    if (!MainViewActive() || !IsTwinSource(array, count, false)) { ++census.unclassified; return; }
    if (array != sCensusLast[sPass]) {
        sCensusLast[sPass] = array;
        const unsigned mask = CensusOutside(array, count, sPass);
        sCensusLastMask[sPass] = mask;
        ++census.runs;
        for (unsigned k = 0; k < 3; ++k)
            if (mask & (1u << k)) ++census.runOut[k];
        const auto model = Load(Target() + kViewMatrix + kJ3dSysModel);
        if (Load(model) - Target() == kJ3dModelXVtable) ++((mask & 1u) ? census.playerOut : census.playerIn);
    }
    for (unsigned k = 0; k < 3; ++k)
        if (sCensusLastMask[sPass] & (1u << k)) ++census.out[k];
}

// ATT-0144: in pass 1 the pos/nrm matrix arrays of J3D shape draws point at the
// twins, and so do j3dSys +128/+130, which texgen reads right after.
HOOK_DEFINE_TRAMPOLINE(SplitSetArray) {
    static void Callback(int attr, std::uintptr_t array, std::uint8_t stride) {
        if (kDiag0212PassVisibilityEnabled && attr == kAttrPosMtx && array) CensusShape(array);
        const bool matrixArray = attr == kAttrPosMtx || attr == kAttrNrmMtx;
        if (!matrixArray || !array || !kSplitJ3dTwinEnabled || !PassViewApplies()) {
            if (attr == kAttrPosMtx) sTwinNrmFollows = false;
            Orig(attr, array, stride);
            return;
        }
        if (!MainViewActive() || !EnsureTwinArena()) {
            if (attr == kAttrPosMtx) { sTwinNrmFollows = false; ++sTwinStats.gateSkip; }
            Orig(attr, array, stride);
            return;
        }
        auto* j3dSys = reinterpret_cast<std::uintptr_t*>(Target() + kViewMatrix);
        if (attr == kAttrPosMtx) {
            sTwinNrmFollows = false;
            unsigned count = 0;
            if (!IsTwinSource(array, count)) { ++sTwinStats.mismatch; Orig(attr, array, stride); return; }
            const auto twin = TwinOf(array, count, true);
            if (!twin) { Orig(attr, array, stride); return; }
            j3dSys[kJ3dSysPosArray / 8] = twin;
            // Normals change only when D rotates; the next attr 16 is this shape's.
            sTwinNrmFollows = sRigRotates;
            sTwinNrmCount = count;
            Orig(attr, twin, stride);
            return;
        }
        if (!sTwinNrmFollows) { Orig(attr, array, stride); return; }
        sTwinNrmFollows = false;
        const auto twin = TwinOf(array, sTwinNrmCount, false);
        if (!twin) { Orig(attr, array, stride); return; }
        j3dSys[kJ3dSysNrmArray / 8] = twin;
        Orig(attr, twin, stride);
    }
};

// Exact re-implementations of J3D's immediate matrix loads (XF header, then
// the values as u32 bits), re-viewed at P2 in pass 1.
void WriteXf(std::uint32_t address, const float* values, std::uint32_t count) {
    reinterpret_cast<void (*)(std::uint32_t, std::uint32_t)>(Target() + kXfLoadHeader)(address, count);
    const auto write = reinterpret_cast<void (*)(std::uint32_t)>(Target() + kFifoWriteU32);
    for (std::uint32_t i = 0; i < count; ++i) {
        std::uint32_t bits;
        std::memcpy(&bits, &values[i], sizeof(bits));
        write(bits);
    }
}

HOOK_DEFINE_REPLACE(SplitJ3dPosMtx) {
    static void Callback(const float* mtx, int id) {
        float local[12];
        const float* source = mtx;
        if (J3dTwinApplies()) { ViewAtP2(mtx, local); source = local; ++sTwinStats.immPos; }
        WriteXf(static_cast<std::uint32_t>(id) << 2, source, 12);
    }
};

HOOK_DEFINE_REPLACE(SplitJ3dNrmMtx) {
    static void Callback(const float* mtx, int id) {
        float out[9];
        if (sRigRotates && J3dTwinApplies()) {
            RotateNormal(mtx, 4, out);
            ++sTwinStats.immNrm;
        } else {
            for (int row = 0; row < 3; ++row)
                for (int column = 0; column < 3; ++column) out[row * 3 + column] = mtx[row * 4 + column];
        }
        WriteXf(static_cast<std::uint32_t>(id * 3 + 0x400), out, 9);
    }
};

HOOK_DEFINE_REPLACE(SplitJ3dNrmMtx33) {
    static void Callback(const float* mtx, int id) {
        float out[9];
        if (sRigRotates && J3dTwinApplies()) {
            RotateNormal(mtx, 3, out);
            ++sTwinStats.immNrm;
        } else {
            std::memcpy(out, mtx, sizeof(out));
        }
        WriteXf(static_cast<std::uint32_t>(id * 3 + 0x400), out, 9);
    }
};

// ATT-0139: an actor is live if either half sees it. P2's half views w exactly
// where P1's frustum sees G(w), so the second test reuses P1's planes. The rig
// is the one drawn last frame; clipping runs in movement before the next draw.
HOOK_DEFINE_REPLACE(SplitClipJudge) {
    static bool Callback(std::uintptr_t judge, const float* pos, float radius, int level) {
        if (!OutsideVolume(judge, pos, radius, level)) return false;
        if (!sClipRigValid) return true;
        const Vec offset = Vec{pos[0], pos[1], pos[2]} - sClipP2;
        const float moved[3] = {
            sClipQt[0] * offset.x + sClipQt[1] * offset.y + sClipQt[2] * offset.z + sClipP1.x,
            sClipQt[3] * offset.x + sClipQt[4] * offset.y + sClipQt[5] * offset.z + sClipP1.y,
            sClipQt[6] * offset.x + sClipQt[7] * offset.y + sClipQt[8] * offset.z + sClipP1.z};
        if (OutsideVolume(judge, moved, radius, level)) return true;
        ++sClipRescues;
        return false;
    }
};

bool SplitBase() {
    return kFixedSplitDiagEnabled && kSplitPerPlayerViewEnabled && HasActivePlayerPair() && !IsP2Parked();
}

// ATT-0141: why the view must be single now; 0 when it may split. An event
// camera (1) snaps; the others close the divider over kForcedMergeTicks.
int ForcedMergeReason() {
    if (!kSplitForcedMergeEnabled) return 0;
    // Event cameras either replace CameraManGame in its calc (vtable test) or
    // run another manager so the ordinary calc stops; two stale draws in a row
    // mean the latter (pause menus included).
    const unsigned serial = GetCoopOrdinaryCameraSerial();
    sStaleCameraDraws = serial == sLastCameraSerial ? std::min(sStaleCameraDraws + 1, 1000u) : 0;
    sLastCameraSerial = serial;
    if (IsCoopEventCamera() || sStaleCameraDraws >= 2) return 1;
    void* actors[2] = {GetFirstPlayer(), GetSecondPlayer()};
    for (void* actor : actors)
        if (IsValidMarioActor(actor) && HasActiveBinding(reinterpret_cast<std::uintptr_t>(actor))) return 2;
    if (GetCoopPipeLeader()) return 3;
    if (GetP1DeathPhase() != DeathPhase::Alive || GetP2DeathPhase() != DeathPhase::Alive) return 4;
    return 0;
}

// ATT-0215: either player in a native rush binding (pipe, Launch Star, Sling
// Star) or co-op pipe travel.
bool AnyTravel() {
    void* actors[2] = {GetFirstPlayer(), GetSecondPlayer()};
    for (void* actor : actors)
        if (IsValidMarioActor(actor) && HasActiveBinding(reinterpret_cast<std::uintptr_t>(actor))) return true;
    return GetCoopPipeLeader() != nullptr;
}

// ATT-0215: Mario is locked when the native stick query would refuse his
// movement (control byte +0x1652, state 0x66D18, Mario +0x20; EVD-0523) or his
// movement did not run for two drawn frames (a demo suspending him). Called
// once per drawn frame.
unsigned sP1MovementSerial = 0, sP1StillDraws = 0;
// ATT-0216 / EVD-0523, EVD-0524: the parts of Mario's movement gate 0x123654,
// as bits: 1 control byte +0x1652 off, 2 lock +0x568, 4 demo mode +0x15D0,
// 8 Mario +0x14 bit 2, 0x10 status 0x12 (0x60624), 0x20 gate state 0x66D18,
// 0x40 Mario +0x20 negative, 0x80 movement not run for two draws.
constexpr std::uintptr_t kMarioStatus = 0x60624;
constexpr std::uintptr_t kMarioGateState = 0x66D18;
unsigned P1LockBits(void* p1, bool still) {
    const auto a = reinterpret_cast<std::uintptr_t>(p1);
    const auto mario = Load(a + 0x3C0);
    unsigned bits = still ? 0x80u : 0u;
    if (!*reinterpret_cast<const std::uint8_t*>(a + 0x1652)) bits |= 1;
    if (*reinterpret_cast<const std::uint8_t*>(a + 0x568)) bits |= 2;
    if (*reinterpret_cast<const std::uint8_t*>(a + 0x15D0)) bits |= 4;
    if (!IsGuestPointer(mario)) return bits;
    if ((*reinterpret_cast<const std::uint8_t*>(mario + 0x14) >> 2) & 1) bits |= 8;
    if (reinterpret_cast<bool (*)(std::uintptr_t, int)>(Target() + kMarioStatus)(mario, 0x12)) bits |= 0x10;
    if (reinterpret_cast<bool (*)(std::uintptr_t)>(Target() + kMarioGateState)(mario)) bits |= 0x20;
    if (*reinterpret_cast<const std::int8_t*>(mario + 0x20) < 0) bits |= 0x40;
    return bits;
}
bool P1Locked(bool& gateOpen, bool& still, unsigned& bits) {
    void* p1 = GetFirstPlayer();
    gateOpen = true; still = false; bits = 0;
    if (!IsValidMarioActor(p1)) return false;
    const unsigned serial = GetMarioMovementSerial(0);
    sP1StillDraws = serial == sP1MovementSerial ? std::min(sP1StillDraws + 1, 1000u) : 0;
    sP1MovementSerial = serial;
    still = sP1StillDraws >= 2;
    gateOpen = IsNativeMovementGateOpen(p1);
    bits = P1LockBits(p1, still);
    if (kSplitCutsceneDemoFlagEnabled) return (bits & (2 | 4 | 0x80)) != 0;
    return still || !gateOpen;
}

// ATT-0213: which forced merges a permanent split keeps. Called after
// ForcedMergeReason, which advances the stale-camera count. Each stopped-camera
// episode is logged with how much of it was travel or had Mario locked, so the
// cutscene rule (ATT-0215) can be checked.
unsigned sEventTicks = 0, sEventLogs = 0, sEpisodeTicks = 0, sEpisodeTravel = 0, sEpisodeLocked = 0;
bool sEventSawVtable = false, sEpisodeMerged = false, sCutsceneHold = false;
unsigned sEpisodeBits = ~0u, sLockBitLogs = 0;
int PermanentForcedReason(int reason) {
    sCutsceneHold = false;
    if (!kSplitPermanentEnabled || !kSplitForcedMergeEnabled) return reason;
    const bool vtable = IsCoopEventCamera();
    const bool stale = vtable || sStaleCameraDraws >= 2;
    const bool travel = kSplitTravelKeepsSplitEnabled && AnyTravel();
    bool gateOpen, still;
    unsigned bits;
    const bool locked = P1Locked(gateOpen, still, bits);
    // ATT-0216: a death or bubble is never a cutscene in a permanent split.
    const bool down = GetP1DeathPhase() != DeathPhase::Alive || GetP2DeathPhase() != DeathPhase::Alive;
    const bool event = stale && !travel && !(kSplitDeathKeepsSplitEnabled && down) &&
                       (!kSplitCutsceneNeedsLockEnabled || locked);
    sEventTicks = event ? std::min(sEventTicks + 1, 100000u) : 0;
    sCutsceneHold = event && sEventTicks >= kSplitCutsceneMergeTicks;
    if (stale) {
        sEpisodeTicks = std::min(sEpisodeTicks + 1, 100000u);
        if (travel) ++sEpisodeTravel;
        if (locked) ++sEpisodeLocked;
        sEventSawVtable = sEventSawVtable || vtable;
        sEpisodeMerged = sEpisodeMerged || sCutsceneHold;
        // DIAG part of ATT-0215: what Mario's lock looks like once the camera
        // has been stopped long enough to count as a cutscene.
        if (sEpisodeTicks == kSplitCutsceneMergeTicks && sEventLogs < 128) {
            ++sEventLogs;
            void* p1 = GetFirstPlayer(); void* p2 = GetSecondPlayer();
            const int control = IsValidMarioActor(p1) ?
                *reinterpret_cast<const std::uint8_t*>(reinterpret_cast<std::uintptr_t>(p1) + 0x1652) : -1;
            Logging.Log("[GalaxyCoop:smg1] ATT-0215 stopped camera at %u ticks travel=%d locked=%d p1_gate=%d p1_still=%d p1_control=%d p2_gate=%d merging=%d ATT-0216 lock_bits=0x%x down=%d kill=1",
                sEpisodeTicks, travel ? 1 : 0, locked ? 1 : 0, gateOpen ? 1 : 0, still ? 1 : 0, control,
                IsValidMarioActor(p2) && IsNativeMovementGateOpen(p2) ? 1 : 0, sCutsceneHold ? 1 : 0, bits, down ? 1 : 0);
        }
        // ATT-0216: timeline of Mario's lock parts through a stopped camera.
        // Hits stop the camera for 1-2 ticks; only longer episodes are traced.
        if (sEpisodeTicks >= 3 && bits != sEpisodeBits) {
            if (sLockBitLogs < 96) {
                ++sLockBitLogs;
                Logging.Log("[GalaxyCoop:smg1] ATT-0216 stopped camera tick=%u lock_bits=0x%x->0x%x travel=%d down=%d kill=1",
                    sEpisodeTicks, sEpisodeBits, bits, travel ? 1 : 0, down ? 1 : 0);
            }
            sEpisodeBits = bits;
        }
    } else if (sEpisodeTicks) {
        if (sEventLogs++ < 128)
            Logging.Log("[GalaxyCoop:smg1] ATT-0213 event camera episode ticks=%u event_vtable=%d ATT-0215 travel=%u locked=%u merged=%d kill=1",
                sEpisodeTicks, sEventSawVtable ? 1 : 0, sEpisodeTravel, sEpisodeLocked, sEpisodeMerged ? 1 : 0);
        sEpisodeTicks = sEpisodeTravel = sEpisodeLocked = 0;
        sEpisodeBits = ~0u;
        sEventSawVtable = sEpisodeMerged = false;
    }
    if (sCutsceneHold) return 1;
    if (down && !kSplitDeathKeepsSplitEnabled) return 4;
    return 0;
}

// ATT-0140: one decision per drawn frame from the camera's framing sample. A
// frame with no sample (native binding, pipe, death, event camera) holds the
// counters; parking or leaving two-player play merges at once.
void TickSplitPolicy() {
    if (!SplitBase()) {
        sSplitFraction = 0; sSplitWanted = false; sFitTicks = 0; sMissTicks = 0;
        sRigBlend = 0; sCameraPreOpenTicks = 0;
        sAnchorValid = false;
        sRevealReopenHoldTicks = 0; sLastForced = 0;
        sCutsceneHold = false;
        return;
    }
    const int forced = PermanentForcedReason(ForcedMergeReason());
    if (forced != sLastForced) {
        if (sPolicyLogs++ < 64)
            Logging.Log("[GalaxyCoop:smg1] ATT-0141 forced merge reason=%d (1 event camera, 2 binding, 3 pipe, 4 death) was=%d s=%.2f kill=1",
                forced, sLastForced, static_cast<double>(sSplitFraction));
        sLastForced = forced;
    }
    if (forced) {
        // A fresh evaluation starts once the forced state ends.
        sSplitWanted = false; sFitTicks = 0; sMissTicks = 0;
        sRigBlend = 0; sCameraPreOpenTicks = 0;
        sRevealReopenHoldTicks = 0;
        sSplitFraction = forced == 1 || kSplitInstantReunionEnabled ? 0.0f : std::max(0.0f, sSplitFraction - 1.0f / kForcedMergeTicks);
        return;
    }
    if (!kSplitDynamicPolicyEnabled) {
        sSplitFraction = 1; sSplitWanted = true; sRigBlend = 1; sCameraPreOpenTicks = 0;
        return;
    }
    if (kSplitPermanentEnabled) {
        // ATT-0213: no distance/reunion decision; reopen after a forced merge.
        static unsigned sOpenLogs = 0;
        if (!sSplitWanted && sOpenLogs++ < 64)
            Logging.Log("[GalaxyCoop:smg1] ATT-0213 permanent split open sample=%u kill=1", sOpenLogs);
        sSplitWanted = true; sFitTicks = 0; sMissTicks = 0; sRevealReopenHoldTicks = 0;
    } else {
    if (sRevealReopenHoldTicks) --sRevealReopenHoldTicks;
    const CoopFramingSample sample = GetCoopFramingSample();
    bool visibleReunion = false;
    float reunionUpDot = -1.0f;
    if (sample.valid) {
        bool fits;
        if (!kSplitPolicyNativeFitEnabled)
            fits = sample.aligned && !sample.blocked &&
                sample.extra < (sSplitWanted ? kMergeExtra : kSplitExtra);
        else if (sample.aligned)
            fits = !sample.sharedBlocked && sample.extra < (sSplitWanted ? kMergeExtra : kSplitExtra);
        else
            fits = !sample.nativeBlocked &&
                sample.nativeExtra < (sSplitWanted ? kNativeMergeExtra : kNativeSplitExtra);
        // ATT-0143: a split view merges only on reunion, once the players are
        // clearly closer than where it split, not when a blocked line clears.
        if (kSplitInstantReunionEnabled && sSplitWanted && sSplitSeparation > 0 &&
            sample.separation >= sSplitSeparation * kReunionFactor)
            fits = false;
        // ATT-0152: the held 307-unit reunion had clear native/shared sight,
        // zero native fit cost and 43-degree gravity difference (EVD-0284).
        // Reuse the proven unit-up getter; never remove the opposite-side gate.
        bool reunionReady = sample.aligned;
        if (kSplitVisibleReunionEnabled && kSplitConvergentMergeEnabled && sSplitWanted &&
            !reunionReady && !sample.nativeBlocked && !sample.sharedBlocked &&
            sample.nativeExtra < kNativeMergeExtra) {
            float up[2][3]{};
            if (GetCoopPlayerUp(GetFirstPlayer(), up[0]) && GetCoopPlayerUp(GetSecondPlayer(), up[1])) {
                reunionUpDot = std::clamp(up[0][0] * up[1][0] + up[0][1] * up[1][1] +
                    up[0][2] * up[1][2], -1.0f, 1.0f);
                visibleReunion = reunionUpDot >= kVisibleReunionUpDot;
                reunionReady = visibleReunion;
            }
        }
        // ATT-0147: keep physical reunion and the existing 45-tick fit hold.
        if (kSplitConvergentMergeEnabled && sSplitWanted &&
            (!reunionReady || sample.separation > kConvergentMergeSeparation))
            fits = false;
        if (fits) { sFitTicks = std::min(sFitTicks + 1, 1000u); sMissTicks = 0; }
        else { sMissTicks = std::min(sMissTicks + 1, 1000u); sFitTicks = 0; }
    }
    if (kSplitGradualRevealEnabled && sRevealReopenHoldTicks) sMissTicks = 0;
    const bool next = sSplitWanted ? sFitTicks < kMergeTicks : sMissTicks >= kSplitTicks;
    if (next != sSplitWanted) {
        sSplitWanted = next;
        if (next) {
            sSplitSeparation = sample.valid ? sample.separation : 0.0f;
            sCameraPreOpenTicks = kSplitCameraMatchEnabled ? kCameraPreOpenTicks : 0;
        } else {
            sCameraPreOpenTicks = 0;
            if (kSplitGradualRevealEnabled) sRevealReopenHoldTicks = kRevealReopenHoldTicks;
        }
        if (sPolicyLogs++ < 64)
            Logging.Log("[GalaxyCoop:smg1] ATT-0140 %s aligned=%d extra=%.1f blocked=%d ATT-0142 fit=%d native=%.1f native_blocked=%d shared_blocked=%d ATT-0143 separation=%.1f split_at=%.1f ATT-0152 visible_reunion=%d up_dot=%.3f kill=1",
                next ? "split" : "merge", sample.aligned ? 1 : 0, static_cast<double>(sample.extra), sample.blocked ? 1 : 0,
                kSplitPolicyNativeFitEnabled ? 1 : 0, static_cast<double>(sample.nativeExtra),
                sample.nativeBlocked ? 1 : 0, sample.sharedBlocked ? 1 : 0,
                static_cast<double>(sample.separation), static_cast<double>(sSplitSeparation),
                visibleReunion ? 1 : 0, static_cast<double>(reunionUpDot));
    }
    }  // !kSplitPermanentEnabled
    const float oldFraction = sSplitFraction;
    const float step = kSplitInstantReunionEnabled && !kSplitGradualRevealEnabled ? 1.0f :
        1.0f / (kSplitEasedRevealEnabled ? kEasedDividerTicks : kDividerTicks);
    if (kSplitCameraMatchEnabled && kSplitGradualRevealEnabled) {
        if (sSplitWanted) {
            if (sSplitFraction == 0.0f && sCameraPreOpenTicks) {
                --sCameraPreOpenTicks;
                sRigBlend = 0.0f;
                if (sCameraPreOpenTicks == 0 && sPolicyLogs++ < 64)
                    Logging.Log("[GalaxyCoop:smg1] ATT-0148 camera prepared divider=%.2f kill=1",
                        static_cast<double>(sSplitFraction));
            } else {
                // The rig is ready before the first pixel of P2 is revealed.
                sRigBlend = 1.0f;
                sSplitFraction = std::min(1.0f, sSplitFraction + step);
            }
        } else if (sRigBlend > 0.0f && sSplitFraction > 0.0f) {
            // Match P2's view to P1 before the divider starts moving.
            sRigBlend = std::max(0.0f, sRigBlend - 1.0f / kCameraMatchTicks);
            if (sRigBlend == 0.0f && sPolicyLogs++ < 64)
                Logging.Log("[GalaxyCoop:smg1] ATT-0148 camera matched divider=%.2f kill=1",
                    static_cast<double>(sSplitFraction));
        } else {
            sSplitFraction = std::max(0.0f, sSplitFraction - step);
        }
    } else {
        sSplitFraction = sSplitWanted ? std::min(1.0f, sSplitFraction + step) : std::max(0.0f, sSplitFraction - step);
        sRigBlend = sSplitFraction > 0.0f ? 1.0f : 0.0f;
    }
    if (kSplitGradualRevealEnabled && oldFraction != sSplitFraction &&
        (sSplitFraction == 0.0f || sSplitFraction == 1.0f) && sPolicyLogs++ < 64)
        Logging.Log("[GalaxyCoop:smg1] ATT-0146 reveal %s fraction=%.2f reopen_hold=%u kill=1",
            sSplitFraction == 1.0f ? "open" : "closed", static_cast<double>(sSplitFraction), sRevealReopenHoldTicks);
}

// ATT-0219 (EVD-0527): a line every kHeartbeatDraws drawn frames, so a log
// that stops tells a freeze (no more lines) from a scene that never ends
// (lines keep coming with the state it is stuck in); and the SD log's timed
// flush, so the last second reaches the card. Called once per drawn frame.
constexpr bool kAtt0219HeartbeatEnabled = true;
constexpr unsigned kHeartbeatDraws = 180;
unsigned sHeartbeatDraws = 0;
void Heartbeat() {
    mod::sdlog::Poll();
    if (!kAtt0219HeartbeatEnabled || ++sHeartbeatDraws % kHeartbeatDraws != 0) return;
    void* players[2] = {GetFirstPlayer(), GetSecondPlayer()};
    unsigned bits[2] = {~0u, ~0u};
    float at[2][3] = {};
    for (int i = 0; i < 2; ++i) {
        if (!IsValidMarioActor(players[i])) continue;
        bits[i] = P1LockBits(players[i], i == 0 && sP1StillDraws >= 2);
        std::memcpy(at[i], reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(players[i]) + 0x14), sizeof(at[i]));
    }
    Logging.Log("[GalaxyCoop:smg1] ATT-0219 heartbeat draw=%u split=%d s=%.2f forced=%d hold=%d stale=%u episode=%u event_cam=%d travel=%d bits=(0x%x,0x%x) death=(%d,%d) parked=%d p1=(%.0f,%.0f,%.0f) p2=(%.0f,%.0f,%.0f) kill=1",
        sHeartbeatDraws, IsCoopSplitActive() ? 1 : 0, static_cast<double>(sSplitFraction), sLastForced,
        sCutsceneHold ? 1 : 0, sStaleCameraDraws, sEpisodeTicks, IsCoopEventCamera() ? 1 : 0, AnyTravel() ? 1 : 0,
        bits[0], bits[1], static_cast<int>(GetP1DeathPhase()), static_cast<int>(GetP2DeathPhase()), IsP2Parked() ? 1 : 0,
        static_cast<double>(at[0][0]), static_cast<double>(at[0][1]), static_cast<double>(at[0][2]),
        static_cast<double>(at[1][0]), static_cast<double>(at[1][1]), static_cast<double>(at[1][2]));
}

HOOK_DEFINE_TRAMPOLINE(SplitLensTiming) {
    static void Callback() {
        const auto metric = sPass == 0 ? SplitTimingMetric::Sampling0 :
            sPass == 1 ? SplitTimingMetric::Sampling1 : SplitTimingMetric::SamplingShared;
        SplitTimingScope timing(metric);
        Orig();
    }
};

HOOK_DEFINE_TRAMPOLINE(SplitScene3d) {
    static void Callback(std::uintptr_t scene) {
        UniformUploadSceneFrame();
        if (kSplitPerPlayerViewEnabled) TickSplitPolicy();
        Heartbeat();
        const bool active = kSplitPerPlayerViewEnabled ? IsCoopSplitActive() : HasActivePlayerPair();
        IdlePollFrame(active);
        if (active != sLastActive) {
            sLastActive = active;
            sProbeOnOpen = active;
            if (sActiveLogs++ < 96) Logging.Log("[GalaxyCoop:smg1] DIAG-0173 split active=%d width=%.1f parked=%d per_player=%d dynamic=%d kill=1",
                active ? 1 : 0, static_cast<double>(FrameWidth()), IsP2Parked() ? 1 : 0,
                kSplitPerPlayerViewEnabled ? 1 : 0, kSplitDynamicPolicyEnabled ? 1 : 0);
        }
        if (!active) {
            FrameTimelineSceneState(false, 0.0f, IsCoopSplitAvailable());
            sSmoothUpValid = false; sClipRigValid = false; sAnchorValid = false; sRevealAmount = 0;
            sP2InputViewValid = false;
            sRigTransport[0].valid = sRigTransport[1].valid = false;
            if (kSplitTimingDiagEnabled) SplitTimingBeginFrame(IsCoopSplitAvailable(), 0);
            { SplitTimingScope timing(SplitTimingMetric::Scene); Orig(scene); }
            if (kSplitTimingDiagEnabled) SplitTimingFinishFrame(IsCoopSplitAvailable() && !IsCoopSplitActive(), 0);
            return;
        }
        // Pass widths W*(1-s/2) and W*s/2; s=1 is the DIAG-0173 50/50 split.
        const float progress = kSplitPerPlayerViewEnabled ? sSplitFraction : 1.0f;
        const float s = kSplitEasedRevealEnabled ? progress * progress * (3.0f - 2.0f * progress) : progress;
        sFrameWidth = FrameWidth();
        sFrameHeight = FrameHeight();
        sRevealAmount = s;
        FrameTimelineSceneState(true, s, IsCoopSplitAvailable());
        // ATT-0151: full-screen background is needed only under a partial
        // mask. Once open, use the same half viewport/projection as ATT-0149.
        sPassW[0] = UsesCornerBackground() ? sFrameWidth : sFrameWidth * (1.0f - s * 0.5f);
        sPassW[1] = kSplitBottomCornerWipeEnabled ? sFrameWidth * 0.5f : sFrameWidth * s * 0.5f;
        sPassX[0] = 0;
        sPassX[1] = kSplitBottomCornerWipeEnabled ? sFrameWidth * 0.5f : sPassW[0];
        sPosLoads[0] = sPosLoads[1] = 0;
        sPosCorrected = 0;
        if (kSplitTimingDiagEnabled) SplitTimingBeginFrame(IsCoopSplitAvailable() && sRevealAmount == 1.0f, 1);
        {
        SplitTimingScope sceneTiming(SplitTimingMetric::Scene);
        for (int pass = 0; pass < 2; ++pass) {
            sPass = pass;
            sPassProjValid[pass] = false;
            sCensusLast[pass] = 0;
            // Pass 0 has published this frame's V1 to 0xAA45AEB0 by now.
            if (pass == 1) {
                SplitTimingScope rigTiming(SplitTimingMetric::Rig);
                sViewOffsetValid = kSplitPerPlayerViewEnabled && BuildPassView();
                if (kSplitPerPlayerViewEnabled && kSplitJ3dTwinEnabled) BeginTwinFrame();
            }
            SplitTimingScope passTiming(pass == 0 ? SplitTimingMetric::Pass0 : SplitTimingMetric::Pass1);
            Orig(scene);
        }
        }
        sPass = -1;
        if (kSplitTimingDiagEnabled) SplitTimingFinishFrame(IsCoopSplitAvailable() && sRevealAmount == 1.0f, 1);
        if (kDiag0212PassVisibilityEnabled && sRevealAmount == 1.0f && ++sCensusFrames == 300) {
            if (sCensusLogs++ < 64) {
                const auto& a = sCensus[0];
                const auto& b = sCensus[1];
                Logging.Log("[GalaxyCoop:smg1] DIAG-0212 pass visibility frames=300 margins=500/2000/8000 "
                    "p0=(shapes=%u uncl=%u out=%u/%u/%u runs=%u rout=%u/%u/%u player=%u/%u proj=%.3f,%.3f,%.3f,%.3f) "
                    "p1=(shapes=%u uncl=%u out=%u/%u/%u runs=%u rout=%u/%u/%u player=%u/%u proj=%.3f,%.3f,%.3f,%.3f) rescues=%u kill=1",
                    a.shapes, a.unclassified, a.out[0], a.out[1], a.out[2], a.runs, a.runOut[0], a.runOut[1], a.runOut[2],
                    a.playerIn, a.playerOut, static_cast<double>(sPassProj[0][0]), static_cast<double>(sPassProj[0][1]),
                    static_cast<double>(sPassProj[0][2]), static_cast<double>(sPassProj[0][3]),
                    b.shapes, b.unclassified, b.out[0], b.out[1], b.out[2], b.runs, b.runOut[0], b.runOut[1], b.runOut[2],
                    b.playerIn, b.playerOut, static_cast<double>(sPassProj[1][0]), static_cast<double>(sPassProj[1][1]),
                    static_cast<double>(sPassProj[1][2]), static_cast<double>(sPassProj[1][3]), sClipRescues);
            }
            sCensus[0] = sCensus[1] = PassCensus{};
            sCensusFrames = 0;
        }
        // DIAG-0191: the opening frame's divider is barely open, so sample the
        // 10th split frame of each opening, then every 600th.
        if (kDiag0191PassViewProbeEnabled && sProbeLogs < 32 &&
            ((sProbeOnOpen && sSplitFraction >= 0.5f) || (sFrames % 600) == 300)) {
            sProbeOnOpen = false;
            ++sProbeLogs;
            const auto* v = reinterpret_cast<const float*>(Target() + kViewMatrix);
            Logging.Log("[GalaxyCoop:smg1] DIAG-0191 pass view valid=%d s=%.2f loads=(%u,%u) corrected=%u persp=%d "
                "V1r0=(%.3f,%.3f,%.3f) V1r1=(%.3f,%.3f,%.3f) V1r2=(%.3f,%.3f,%.3f) T1=(%.1f,%.1f,%.1f) td=(%.1f,%.1f,%.1f) rot=%d delta=(%.1f,%.1f,%.1f) kill=1",
                sViewOffsetValid ? 1 : 0, static_cast<double>(sSplitFraction), sPosLoads[0], sPosLoads[1], sPosCorrected,
                ProjectionIsPerspective() ? 1 : 0,
                static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]),
                static_cast<double>(v[4]), static_cast<double>(v[5]), static_cast<double>(v[6]),
                static_cast<double>(v[8]), static_cast<double>(v[9]), static_cast<double>(v[10]),
                static_cast<double>(v[3]), static_cast<double>(v[7]), static_cast<double>(v[11]),
                static_cast<double>(sTd[0]), static_cast<double>(sTd[1]), static_cast<double>(sTd[2]), sRigRotates ? 1 : 0,
                static_cast<double>(sViewDelta[0]), static_cast<double>(sViewDelta[1]), static_cast<double>(sViewDelta[2]));
        }
        sP2InputViewValid = sViewOffsetValid;
        sViewOffsetValid = false;
        // Layouts drawn after the 3D sequence expect the full-screen viewport.
        reinterpret_cast<void (*)()>(Target() + kFullViewport)();
        if ((sFrames++ % 600) == 0 && sLogs < 40) {
            ++sLogs;
            Logging.Log("[GalaxyCoop:smg1] DIAG-0173 split frame=%u passes=2 widths=(%.1f,%.1f) kill=1",
                sFrames, static_cast<double>(sPassW[0]), static_cast<double>(sPassW[1]));
        }
        if (kSplitPerPlayerViewEnabled && (sFrames % 600) == 1 && sViewLogs < 40) {
            ++sViewLogs;
            Logging.Log("[GalaxyCoop:smg1] ATT-0137 pass1 view delta=(%.1f,%.1f,%.1f) ATT-0138 rig=%d angle=%.1f kill=1",
                static_cast<double>(sViewDelta[0]), static_cast<double>(sViewDelta[1]),
                static_cast<double>(sViewDelta[2]), kSplitGravityRigEnabled ? 1 : 0,
                static_cast<double>(sRigAngle * 57.2957795f));
        }
        if (kSplitCullingEnabled && (sFrames % 600) == 1 && sClipLogs < 40) {
            ++sClipLogs;
            Logging.Log("[GalaxyCoop:smg1] ATT-0139 p2 view kept %u clip tests live over 600 frames rig=%d kill=1",
                sClipRescues, sClipRigValid ? 1 : 0);
            sClipRescues = 0;
        }
        if (kSplitPerPlayerViewEnabled && kSplitJ3dTwinEnabled && (sFrames % 600) == 1 && sTwinLogs < 40) {
            ++sTwinLogs;
            Logging.Log("[GalaxyCoop:smg1] ATT-0144 j3d twins over 600 split frames: pos=%u nrm=%u cached=%u mismatch=%u gate_skip=%u world=%u overflow=%u imm_pos=%u imm_nrm=%u peak=0x%lx arena=0x%lx rot=%d kill=1",
                sTwinStats.pos, sTwinStats.nrm, sTwinStats.cached, sTwinStats.mismatch, sTwinStats.gateSkip,
                sTwinStats.world, sTwinStats.overflow, sTwinStats.immPos, sTwinStats.immNrm,
                static_cast<unsigned long>(sTwinPeak), static_cast<unsigned long>(sTwinArena), sRigRotates ? 1 : 0);
            sTwinStats = {};
            sTwinPeak = 0;
        }
    }
};
}  // namespace

bool IsCoopSplitActive() {
    return SplitBase() && sSplitFraction > 0;
}

bool IsCoopSplitPreparing() {
    return kSplitCameraMatchEnabled && SplitBase() && sSplitWanted && sSplitFraction == 0.0f;
}

bool IsCoopSplitAvailable() {
    return kSplitDynamicPolicyEnabled && SplitBase();
}

bool GetCoopP2InputView(float out[12]) {
    if (!sP2InputViewValid || sRevealAmount < 0.5f || !IsCoopSplitActive()) return false;
    std::memcpy(out, sP2InputView, sizeof(sP2InputView));
    return true;
}

bool IsCoopCutsceneHold() {
    return kSplitCutsceneHoldP2Enabled && sCutsceneHold && SplitBase();
}

void InstallSplitScreen(const galaxy::target::Identity& identity) {
    if (identity.game != galaxy::target::Game::Smg1 || !kFixedSplitDiagEnabled) return;
    InstallBackendTiming();
    InstallPushConstantFilter();
    InstallFrameTimeline();
    InstallFastVertexConvert();
    InstallProducerCost();
    InstallDrawCost();
    InstallFrameOverlap();
    InstallF264FastTransform();
    InstallIdlePoll();
    if (kSplitTimingDiagEnabled) {
        SplitLensTiming::InstallAtOffset(kLensSampling);
        Logging.Log("[GalaxyCoop:smg1] DIAG-0195 transient render timing armed via GDB only lens=0x20900 window=240 kill=1");
    }
    if (kSplitAsyncColourReadbackEnabled) {
        SplitReadbackConfig::InstallAtOffset(kReadbackConfigInit);
        Logging.Log("[GalaxyCoop:smg1] ATT-0156 native deferred colour readback installed init=0x%lx kill=1",
            static_cast<unsigned long>(kReadbackConfigInit));
    }
    SplitViewport::InstallAtOffset(kGxSetViewport);
    if (kAtt0226SplitCopyEnabled) {
        SplitEfbCopy::InstallAtOffset(kEfbCopy);
        Logging.Log("[GalaxyCoop:smg1] ATT-0226 split screen copies installed copy=0x%lx kill=1",
            static_cast<unsigned long>(kEfbCopy));
    }
    SplitProjection::InstallAtOffset(kCameraLoadProjection);
    if (kSplitPerPlayerViewEnabled && kSplitBottomCornerWipeEnabled)
        SplitScissor::InstallAtOffset(kGxSetScissor);
    SplitScene3d::InstallAtOffset(kScene3dDraw);
    if (kSplitPerPlayerViewEnabled && kSplitP1AnchoredViewEnabled)
        SplitLeaderView::InstallAtOffset(kCameraSetView);
    if (kSplitPerPlayerViewEnabled) SplitPosMtx::InstallAtOffset(kGxLoadPosMtxImm);
    if (kSplitPerPlayerViewEnabled && kSplitGravityRigEnabled) {
        SplitNrmMtx::InstallAtOffset(kGxLoadNrmMtxImm);
        Logging.Log("[GalaxyCoop:smg1] ATT-0138 gravity rig installed nrm_mtx=0x%lx smoothing=0.12/0.02rad antipodal=camera_right kill=1",
            static_cast<unsigned long>(kGxLoadNrmMtxImm));
    }
    if (kSplitPerPlayerViewEnabled && kSplitCullingEnabled) {
        SplitClipJudge::InstallAtOffset(kClippingJudgeTest);
        Logging.Log("[GalaxyCoop:smg1] ATT-0139 clipping judge 0x%lx replaced; P2 half keeps actors live kill=1",
            static_cast<unsigned long>(kClippingJudgeTest));
    }
    if (kSplitPerPlayerViewEnabled && kSplitJ3dTwinEnabled) {
        SplitSetArray::InstallAtOffset(kGxSetArray);
        SplitJ3dPosMtx::InstallAtOffset(kJ3dLoadPosMtx);
        SplitJ3dNrmMtx::InstallAtOffset(kJ3dLoadNrmMtx);
        SplitJ3dNrmMtx33::InstallAtOffset(kJ3dLoadNrmMtx33);
        Logging.Log("[GalaxyCoop:smg1] ATT-0144 j3d twins installed set_array=0x%lx pos=0x%lx nrm=0x%lx nrm33=0x%lx half=0x%lx kill=1",
            static_cast<unsigned long>(kGxSetArray), static_cast<unsigned long>(kJ3dLoadPosMtx),
            static_cast<unsigned long>(kJ3dLoadNrmMtx), static_cast<unsigned long>(kJ3dLoadNrmMtx33),
            static_cast<unsigned long>(kTwinHalfBytes));
    }
    Logging.Log("[GalaxyCoop:smg1] DIAG-0173 fixed split installed passes=2 same_camera=%d x_scale=W/width kill=1",
        kSplitPerPlayerViewEnabled ? 0 : 1);
    if (kSplitPerPlayerViewEnabled && kSplitDynamicPolicyEnabled)
        Logging.Log("[GalaxyCoop:smg1] ATT-0140 dynamic split installed split_extra=%.0f merge_extra=%.0f ticks=%u/%u divider=%u regroup=6000 kill=1",
            static_cast<double>(kSplitExtra), static_cast<double>(kMergeExtra), kSplitTicks, kMergeTicks, kDividerTicks);
    if (kSplitPerPlayerViewEnabled && kSplitDynamicPolicyEnabled && kSplitPolicyNativeFitEnabled)
        Logging.Log("[GalaxyCoop:smg1] ATT-0142 policy fit installed native_split=%.0f native_merge=%.0f blocked_samples=3 kill=1",
            static_cast<double>(kNativeSplitExtra), static_cast<double>(kNativeMergeExtra));
    if (kSplitPerPlayerViewEnabled && kSplitDynamicPolicyEnabled && kSplitInstantReunionEnabled)
        Logging.Log("[GalaxyCoop:smg1] ATT-0143 reunion merge installed reunion=%.2f instant_cut=%d kill=1",
            static_cast<double>(kReunionFactor), kSplitGradualRevealEnabled ? 0 : 1);
    if (kSplitPerPlayerViewEnabled && kSplitDynamicPolicyEnabled && kSplitGradualRevealEnabled)
        Logging.Log("[GalaxyCoop:smg1] ATT-0146 gradual reveal installed divider_ticks=%u reopen_hold=%u camera_blend=30 forced_snap=1 kill=1",
            kSplitEasedRevealEnabled ? kEasedDividerTicks : kDividerTicks, kRevealReopenHoldTicks);
    if (kSplitPerPlayerViewEnabled && kSplitDynamicPolicyEnabled && kSplitConvergentMergeEnabled)
        Logging.Log("[GalaxyCoop:smg1] ATT-0147 convergent merge installed aligned=1 max_separation=%.0f fit_ticks=%u kill=1",
            static_cast<double>(kConvergentMergeSeparation), kMergeTicks);
    if (kSplitPerPlayerViewEnabled && kSplitConvergentMergeEnabled && kSplitVisibleReunionEnabled)
        Logging.Log("[GalaxyCoop:smg1] ATT-0152 visible reunion installed max_angle=50 native_fit=1 clear_views=1 max_separation=%.0f fit_ticks=%u kill=1",
            static_cast<double>(kConvergentMergeSeparation), kMergeTicks);
    if (kSplitPerPlayerViewEnabled && kSplitDynamicPolicyEnabled && kSplitCameraMatchEnabled)
        Logging.Log("[GalaxyCoop:smg1] ATT-0148 staged camera match installed pre_open=%u match=%u divider=%u kill=1",
            kCameraPreOpenTicks, kCameraMatchTicks, kDividerTicks);
    if (kSplitPerPlayerViewEnabled && kSplitCameraAnchorEnabled && kSplitEasedRevealEnabled)
        Logging.Log("[GalaxyCoop:smg1] ATT-0149 P2 camera anchor + eased divider installed ticks=%u kill=1",
            kEasedDividerTicks);
    if (kSplitPerPlayerViewEnabled && kSplitCameraAnchorEnabled && kSplitCameraOwnerReferenceEnabled)
        Logging.Log("[GalaxyCoop:smg1] ATT-0153 camera reference follows actual native calc owner kill=1");
    if (kSplitPerPlayerViewEnabled && kSplitP1AnchoredViewEnabled)
        Logging.Log("[GalaxyCoop:smg1] ATT-0154 P1 anchored final-view producer installed setter=0x%lx reveal_blend=1 kill=1",static_cast<unsigned long>(kCameraSetView));
    if (kSplitPerPlayerViewEnabled && kSplitBottomCornerWipeEnabled)
        Logging.Log("[GalaxyCoop:smg1] ATT-0150 bottom-center corner wipe installed scissor=0x%lx fixed_right=1 p1_background=1 kill=1",
            static_cast<unsigned long>(kGxSetScissor));
    if (kSplitPerPlayerViewEnabled && kSplitBottomCornerWipeEnabled && kSplitCornerTransitionOnlyEnabled)
        Logging.Log("[GalaxyCoop:smg1] ATT-0151 corner background transition_only=1 full_split_half_viewports=1 kill=1");
    if (kSplitPerPlayerViewEnabled && kDiag0191PassViewProbeEnabled)
        Logging.Log("[GalaxyCoop:smg1] DIAG-0191 pass view probe armed kill=1");
    if (kSplitPerPlayerViewEnabled && kSplitJ3dTwinEnabled && kDiag0212PassVisibilityEnabled)
        Logging.Log("[GalaxyCoop:smg1] DIAG-0212 ATT-0200 pass visibility census armed set_array=0x%lx window=300 kill=1",
            static_cast<unsigned long>(kGxSetArray));
    if (kSplitPerPlayerViewEnabled && kSplitForcedMergeEnabled)
        Logging.Log("[GalaxyCoop:smg1] ATT-0141 forced merge installed binding=1 pipe=1 death=1 event_camera=snap parked=snap close_ticks=%u kill=1",
            kForcedMergeTicks);
    if (kSplitPerPlayerViewEnabled)
        Logging.Log("[GalaxyCoop:smg1] ATT-0137 per-player views installed pos_mtx=0x%lx parked_single_pass=1 camera_p1=1 kill=1",
            static_cast<unsigned long>(kGxLoadPosMtxImm));
}

}  // namespace mod::patches
