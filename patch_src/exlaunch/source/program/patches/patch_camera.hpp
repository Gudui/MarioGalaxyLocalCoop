#pragma once

#include "program/target_identity.hpp"

namespace mod::patches {

// PATCH-0164: ordinary-camera-only framing, after the verified arrival gate.
// This switch never installs a recovery camera target at scene spawn.
inline constexpr bool kSharedCameraEnabled = false;
inline constexpr bool kPlanetCameraEnabled = false; // PATCH-0168 rejected, EVD-0202
inline constexpr bool kNativeLeaderCameraEnabled = true; // PATCH-0169
// PATCH-0170 / EVD-0204: shared midpoint+zoom while gravity agrees, native
// leader view otherwise. Requires kNativeLeaderCameraEnabled.
inline constexpr bool kHybridSharedCameraEnabled = true;

void InstallCamera(const galaxy::target::Identity& identity);
void ResetSharedCamera();
bool IsCoopSharedFraming();
// Unit gravity up (-core+0x1E8) of a valid player; false when unavailable.
bool GetCoopPlayerUp(void* actor, float up[3]);

// ATT-0140: would one shared view frame both players this frame? Filled by the
// ordinary two-player camera calc only; valid=false in every other case.
struct CoopFramingSample {
    bool valid;
    bool aligned;   // PATCH-0170 gravity gate (enter <=30 deg for 20 ticks, exit >40 deg)
    float extra;    // eye push-back a midpoint frame needs beyond the native distance
    bool blocked;   // map geometry hides a player from that shared eye
    // ATT-0142: the same questions for the native leader view (the merged view
    // while gravity diverges), and blocking held for 3 samples in a row.
    float nativeExtra;    // eye push-back the native view needs to hold both players
    bool nativeBlocked;   // a player is steadily hidden from the native eye
    bool sharedBlocked;   // a player is steadily hidden from the shared eye
    float separation;     // ATT-0143: distance between the players' camera points
    float nativeDistance; // ATT-0155: eye-to-watch distance before shared fit pushes eye back
};
CoopFramingSample GetCoopFramingSample();
// ATT-0141: the last camera calc ran a non-ordinary camera (vtable != 0x81E4C0,
// e.g. an event or cutscene camera).
bool IsCoopEventCamera();
// ATT-0141: counts ordinary CameraManGame calcs (vtable 0x81E4C0). A draw that
// sees no new count means another camera manager owns the view this frame.
unsigned GetCoopOrdinaryCameraSerial();
// ATT-0153: player used by the latest successful ordinary native calc, not
// the leader policy selected afterward. -1 means the view has no proven owner.
int GetCoopNativeViewOwnerIndex();

}  // namespace mod::patches
