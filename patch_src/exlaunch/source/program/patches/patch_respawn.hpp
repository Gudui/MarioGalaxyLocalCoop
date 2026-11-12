#pragma once
#include "program/target_identity.hpp"

namespace mod::patches {
// PATCH-0162: separate experiment; the retired PATCH-0068 stays disabled.
inline constexpr bool kBubbleRespawnEnabled = true;
inline constexpr bool kPipeCompanionEnabled = true; // PATCH-0167, paired native pipe binding
inline constexpr bool kDistanceCatchupEnabled = true; // PATCH-0166, no life/HP cost
void InstallRespawn(const galaxy::target::Identity& identity);
void TickBubbleRespawn();
void ResetBubbleRespawn();
bool SuppressChargedNativeDeath();
// PATCH-0171: a life was added to player index; revive an out player soon.
void NotifyCoopLifeGained(int index);
void BeginCoopPipeTravel(void* leader);
// PATCH-0189: the partner rides a co-op bubble through a departure that binds
// only one player (observatory dome blue star).
bool RequestCoopTravelBubble(void* stranded);
void FinishCoopPipeTravel(void* leader);
bool IsCoopTravelHidden(void* actor);
void* GetCoopPipeLeader();
// PATCH-0168: values only, exchanged with ordinary-camera callbacks.
int GetCoopCameraLeaderIndex();
// ATT-0140: the split camera belongs to P1; pin the PATCH-0169 leader to it.
void HoldCoopCameraLeader(int index);
bool IsCoopRegrouping();
void ReportCoopCameraVisibility(bool valid, bool blocked);
}
