#pragma once

#include "program/target_identity.hpp"

namespace mod::patches {

// DIAG-0173 / TASK-0018 / EVD-0208: fixed 50/50 vertical split test. Runs the
// main 3D draw sequence twice per frame (P1 view left, then right half) with
// the same camera. Default off: patch_src is shared with the HANDOFF.md
// workstream, so enable only for a coordinated split-screen test deploy.
inline constexpr bool kFixedSplitDiagEnabled = true;

// ATT-0137 / EVD-0256: per-player views. Parked P2 (menus, file select, fly-in)
// draws one full-screen pass; while split the camera holds native leader P1 and
// pass 1 re-views every GXLoadPosMtxImm load at P2. Requires the DIAG-0173
// pass loop; off restores DIAG-0173's same-camera halves.
inline constexpr bool kSplitPerPlayerViewEnabled = true;

// ATT-0138: gravity rig. Pass 1 transports P1's view to P2 through the minimal
// rotation Q from P1's smoothed gravity up to P2's, so P2 stands upright in its
// half; normals follow via GXLoadNrmMtxImm. Off (or ATT-0137 off) keeps the
// ATT-0137 pure translation.
inline constexpr bool kSplitGravityRigEnabled = true;

// ATT-0139 / EVD-0258: culling. The ClippingJudge frustum test 0x4068 is
// replaced exactly; while split an actor clipped by P1's frustum stays live when
// its point transported into P1's frame (the pass-1 rig) is inside it, i.e.
// when P2's half sees it. Requires ATT-0137.
inline constexpr bool kSplitCullingEnabled = true;

// ATT-0140: dynamic policy. One shared full-screen view while the PATCH-0170
// gravity gate agrees and a midpoint frame is neither too far out nor blocked;
// split otherwise, with hysteresis and a 20-tick animated divider. While the
// split is available visibility regroups stop and distance regroups wait for
// ~6000 units. Off keeps the ATT-0137 permanent split. Requires ATT-0137.
inline constexpr bool kSplitDynamicPolicyEnabled = true;

// ATT-0141: forced merge. One view while either player is in a native binding
// (pipe, Launch Star), co-op pipe travel, a death/ghost/recovering phase, or a
// non-ordinary camera (event/cutscene; snaps at once). Parking already merges.
// Requires ATT-0137; works with or without ATT-0140.
inline constexpr bool kSplitForcedMergeEnabled = true;

// ATT-0142 / EVD-0259: policy fit. Diverging gravity no longer splits by
// itself: the merged view is then the native leader view, so it stays single
// while both players fit that view and are visible from its eye. Terrain only
// splits after 3 blocked samples in a row (~18 ticks), not one. Off restores
// the ATT-0140 test. Requires ATT-0140.
inline constexpr bool kSplitPolicyNativeFitEnabled = true;

// ATT-0143 / EVD-0260: reunion rule and legacy instant cut. A split merges
// only once the players are closer than 80% of their split separation.
// ATT-0146 overrides the ordinary instant cut when enabled. Off restores
// ATT-0142 merging. Requires ATT-0140.
inline constexpr bool kSplitInstantReunionEnabled = true;

// ATT-0146 / EVD-0261, EVD-0272: reveal P2's already independent view by
// opening/closing the divider over 20 drawn frames. The existing camera
// blend-out eases P1 off shared framing; P2's local view stays stable. Hold a
// merged view briefly before permitting another ordinary split. Forced event,
// binding, pipe and death merges retain their immediate native safety path.
// Off restores ATT-0143's exact instant ordinary transition.
inline constexpr bool kSplitGradualRevealEnabled = true;

// ATT-0147: keep independent views until the players are truly reunited.
// Ordinary closing needs the camera's stable shared-gravity gate and nearby
// camera points. Off restores ATT-0146's native-fit-only reunion decision.
inline constexpr bool kSplitConvergentMergeEnabled = true;

// ATT-0152 / EVD-0284: moderate planet curvature may fail the strict shared
// gravity gate even when the native view already fits both visible players.
// Allow reunion up to 50 degrees in that case. Off restores ATT-0147.
inline constexpr bool kSplitVisibleReunionEnabled = true;

// ATT-0148: stage the ordinary transition. Give P1's native camera a short
// head start before revealing P2; on reunion, bring P2's view into P1's view
// while the divider is still open, then close it. Off restores ATT-0147.
inline constexpr bool kSplitCameraMatchEnabled = false;

// ATT-0149: hold the right view's split-entry P1 camera reference while P2
// moves. P1's later camera motion cannot steer P2's half. Off restores the
// ATT-0147 view transport.
inline constexpr bool kSplitCameraAnchorEnabled = true;

// ATT-0153 / EVD-0287: the first split draw can still carry a P2-led native
// view. Freeze the actual producer's point/up with it, rather than labelling
// every view as P1. Off restores ATT-0149's unconditional P1 reference.
inline constexpr bool kSplitCameraOwnerReferenceEnabled = true;

// ATT-0154: frame P1 through the same independent reference/gravity rig as
// P2. The final main-view producer blends with the reveal at both endpoints.
inline constexpr bool kSplitP1AnchoredViewEnabled = true;

// ATT-0205 / EVD-0501: the entry reference can be the shared view at the
// moment it sees its owner from below the owner's ground (or upside down), and
// the rig keeps that for the whole split in both halves, mirroring up/down.
// Raise its eye about the owner's point to >= ~17 deg above the owner's ground
// and roll the owner upright before freezing it. Off restores ATT-0153.
inline constexpr bool kSplitReferenceUprightEnabled = true;

// ATT-0206 / EVD-0503: the rig rotation from the fixed entry up whips near
// that up's antipode, which holding forward on a small planet reaches in
// seconds, so the camera spins and the player turns round. Carry each
// player's rotation frame by frame instead (parallel transport along the
// path). Off restores the per-frame rotation from the entry up.
inline constexpr bool kSplitIncrementalRigEnabled = true;

// ATT-0207 / EVD-0507: the shared view at split entry frames both players, so
// its owner can sit far off centre (30 deg in the hub) and every half-width
// view keeps that placement, cropping both players. Pan the frozen reference
// in place so its owner is horizontally centred. Off restores ATT-0206.
inline constexpr bool kSplitReferenceCentredEnabled = true;

// ATT-0208 / EVD-0509: ATT-0155 eases each half to the native single-screen
// distance, but a half is half as wide, so the split reads as zoomed in.
// Ease to 1.5x native (operator choice), still never past the entry distance.
// Off restores ATT-0155's native distance.
inline constexpr bool kSplitHalfDistanceEnabled = true;
inline constexpr float kSplitHalfDistanceScale = 1.5f;

// ATT-0217 (operator decision): a wide camera zone (Gateway tree stump, native
// ~3800) became ~5700. The half distance adds at most this much to native;
// ordinary views (native <= 1600) are unchanged. Off restores ATT-0208's 1.5x.
inline constexpr bool kSplitHalfDistanceCapEnabled = true;
inline constexpr float kSplitHalfDistanceMaxExtra = 800.0f;

// ATT-0212 / EVD-0518: "never past the entry distance" kept planet splits at
// their entry (~1840 against a 2400 target) and a split reopened right after a
// cutscene at the cutscene's close-up (454). Also pull out to the target, at
// most this many times the entry distance. Off restores ATT-0208.
inline constexpr bool kSplitPullOutEnabled = true;
inline constexpr float kSplitPullOutMaxRatio = 6.0f;

// ATT-0213 / EVD-0519 (operator decision): permanent split for now. Both
// halves stay split whenever both players are in play; pipes, Launch Stars
// and other bindings keep the split (PATCH-0167/0186 still carry both
// players); deaths merge as before; an event camera merges only once it has
// lasted kSplitCutsceneMergeTicks drawn frames, so hit pauses never close the
// split. Mario's half is the game's own camera eased out to the half distance;
// Luigi's half turns behind him as he moves (kSplitP2FollowEnabled). Off
// restores the dynamic split (ATT-0140..0212) unchanged.
inline constexpr bool kSplitPermanentEnabled = true;
inline constexpr bool kSplitP2FollowEnabled = true;
inline constexpr unsigned kSplitCutsceneMergeTicks = 30;

// ATT-0215 / EVD-0521: the native camera stopped (the only event signal) for
// pipes, Launch Stars and the crater as well as cutscenes, and each reopen
// copied whatever the native camera showed. In a permanent split:
// - kSplitTravelKeepsSplitEnabled: a stopped camera while either player is in
//   a rush binding or co-op pipe travel never merges.
// - kSplitCutsceneNeedsLockEnabled: otherwise it merges only while Mario is
//   locked (his movement gate closed, or his movement not running).
// - kSplitCutsceneHoldP2Enabled: while such a cutscene merge lasts, Luigi's
//   stick and routed buttons read neutral.
// - kSplitFreshReferenceEnabled: Luigi's half (re)opens from a fresh level view
//   at kSplitFreshReferenceElevation radians and the half distance.
// Each off restores ATT-0213's behaviour for that part.
inline constexpr bool kSplitTravelKeepsSplitEnabled = true;
inline constexpr bool kSplitCutsceneNeedsLockEnabled = true;
inline constexpr bool kSplitCutsceneHoldP2Enabled = true;
inline constexpr bool kSplitFreshReferenceEnabled = true;
inline constexpr float kSplitFreshReferenceElevation = 0.35f;

// ATT-0216 / EVD-0524: Mario's movement gate also closed in the planet tunnel.
// - kSplitCutsceneDemoFlagEnabled: Mario counts as locked only through the
//   native player lock (+0x568 or demo mode +0x15D0) or a suspended movement;
//   off restores ATT-0215's movement-gate test.
// - kSplitDeathKeepsSplitEnabled: a death or bubble no longer merges a
//   permanent split; a dead Luigi's half holds still until his bubble starts.
inline constexpr bool kSplitCutsceneDemoFlagEnabled = true;
inline constexpr bool kSplitDeathKeepsSplitEnabled = true;

// ATT-0155 / EVD-0302: split entry can inherit thousands of units of shared
// push-back. Ease both independent views toward the stage's pre-fit native
// distance, preserving entry angle and screen placement. Off restores ATT-0154.
inline constexpr bool kSplitNativeDistanceFramingEnabled = true;

// ATT-0156 / EVD-0311,0312,0315: native colour pixel peeks otherwise flush
// GPU work synchronously inside both scene passes. Configure the existing
// completed-prior-frame path at pool initialization. Off restores the original
// producer; depth mode, pool allocation and camera math stay native.
inline constexpr bool kSplitAsyncColourReadbackEnabled = true;

// ATT-0149: make the divider's motion visible with a 40-frame smoothstep
// reveal. Off restores the 20-frame linear ATT-0146 divider.
inline constexpr bool kSplitEasedRevealEnabled = true;

// ATT-0150: reveal the fixed-size right view through a scissor rectangle that
// grows from the bottom center. P1 draws behind it at full width. Off retains
// ATT-0149's horizontal slider and does not install the scissor replacement.
inline constexpr bool kSplitBottomCornerWipeEnabled = true;

// ATT-0151 / EVD-0282: the full-width P1 background is needed only while
// revealing or closing. A fully open split uses ATT-0149's two half viewports.
// Off restores ATT-0150's permanent full-width background.
inline constexpr bool kSplitCornerTransitionOnlyEnabled = true;

// DIAG-0191: pass-1 view probe. On each split opening (and every 600 split
// frames) logs the view V1 read for pass 1, the correction td, and how many
// GXLoadPosMtxImm loads each pass made and pass 1 corrected. Read-only.
inline constexpr bool kDiag0191PassViewProbeEnabled = false;

// DIAG-0212 / ATT-0200: per-pass visibility census. Each split pass draws the
// whole scene, so it also draws models only the other half sees. In a fully
// open split, counts each pass's J3D shape draws (GXSetArray pos mtx) whose
// model's joint origins lie outside that pass's half frustum by 500/2000/8000
// units; player models are the answer key. Read-only, log every 300 frames.
#if defined(GALAXY_PASS_VISIBILITY_DIAG) && GALAXY_PASS_VISIBILITY_DIAG
inline constexpr bool kDiag0212PassVisibilityEnabled = true;
#else
inline constexpr bool kDiag0212PassVisibilityEnabled = false;
#endif

// ATT-0144 / EVD-0263..0267 / HYP-0004: J3D models draw from view-space
// draw-matrix buffers the GPU reads after both passes, so pass 1 swaps each
// model's buffer at GXSetArray for a copy re-viewed at P2 (D*M, normals
// R(D)*N) and re-views J3D's immediate loads the same way. Off restores
// ATT-0137/0138, where only non-J3D loads follow P2. Requires ATT-0137.
inline constexpr bool kSplitJ3dTwinEnabled = true;

// ATT-0145 / EVD-0270: J3DMtxBuffer's draw-array pointer table may be only
// four-byte aligned. Read its eight-byte entry with memcpy after range checks.
// Off restores ATT-0144's original strict selector.
inline constexpr bool kSplitJ3dPackedTableEnabled = true;

void InstallSplitScreen(const galaxy::target::Identity& identity);
// True while the draw is split into per-player halves (divider open or
// animating); the camera then keeps P1's own native view and suspends shared
// framing and visibility regroups.
bool IsCoopSplitActive();
// ATT-0148: P1 camera starts moving toward native before the first split pass.
bool IsCoopSplitPreparing();
// ATT-0140: true in two-player gameplay where the dynamic split may open, so
// separation is handled by splitting rather than by regroup bubbles.
bool IsCoopSplitAvailable();
// ATT-0204: the view P2's half was last drawn with (pass 1, V2 = D * V1), while
// that half is at least half revealed. False when merged, revealing below half,
// or the rig failed; callers then keep the native main view.
bool GetCoopP2InputView(float out[12]);
// ATT-0215: true while a permanent split is merged for a cutscene (Mario
// locked under a stopped camera); P2's input then reads neutral.
bool IsCoopCutsceneHold();

}  // namespace mod::patches
