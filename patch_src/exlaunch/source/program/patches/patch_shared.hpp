#pragma once

#include "lib.hpp"
#include <cstdint>
#include <cstddef>

namespace mod::patches {

// Canonical guest pointer ranges
constexpr std::uintptr_t kMinGuestPointer = 0x08000000;
constexpr std::uintptr_t kMaxGuestPointer = 0x8000000000ull;
constexpr std::uintptr_t kMarioActorVtableSmg1 = 0x835830;

bool IsGuestPointer(std::uintptr_t ptr);
bool IsValidMarioActor(void* actor);

// PATCH-0180 kill switch (ATT-0134): enemies (Kuribo, Kabokuri, Karikari)
// write MarioActor+0xF20 outside producer 0x11BEB4 and leave it set with the
// rush-active byte +0xF38 at 0. Real pipe/Launch Star bindings go through the
// producer, which sets +0xF38=1 until release 0x1901C4 clears both. Co-op
// camera, bubble and pipe pairing treat only +0xF20 with +0xF38 set as bound.
constexpr bool kPatch0180ActiveBindingOnlyEnabled = true;
inline bool HasActiveBinding(std::uintptr_t actor) {
    if (*reinterpret_cast<const std::uintptr_t*>(actor + 0xF20) == 0) return false;
    return !kPatch0180ActiveBindingOnlyEnabled || *reinterpret_cast<const std::uint8_t*>(actor + 0xF38) != 0;
}

// Canonical player pointer state
void SetCapturedPlayers(void* p1, void* p2);
void ClearCapturedPlayers();
void* GetFirstPlayer();
void* GetSecondPlayer();
bool HasActivePlayerPair();

// Co-Op 3-Phase Death & Respawn State Machine
enum class DeathPhase : std::uint8_t {
    Alive = 0,
    Animating = 1,
    Ghost = 2,
    Recovering = 3
};

DeathPhase GetP1DeathPhase();
DeathPhase GetP2DeathPhase();
void SetP1DeathPhase(DeathPhase phase);
void SetP2DeathPhase(DeathPhase phase);

std::uint32_t GetP1AnimTimer();
std::uint32_t GetP2AnimTimer();
void SetP1AnimTimer(std::uint32_t frames);
void SetP2AnimTimer(std::uint32_t frames);

std::uint32_t GetP1RespawnTimer();
std::uint32_t GetP2RespawnTimer();
void SetP1RespawnTimer(std::uint32_t frames);
void SetP2RespawnTimer(std::uint32_t frames);

std::uint32_t GetP1InvulnerableTimer();
std::uint32_t GetP2InvulnerableTimer();
void DecrementInvulnerableTimers();

bool IsP1Dead();
bool IsP2Dead();
bool IsPlayerGhost(void* actor);
bool IsPlayerInvulnerable(void* actor);

bool GetSuppressGlobalDeathSequence();
void SetSuppressGlobalDeathSequence(bool suppress);

void CaptureAliveNerves();

void DeactivateDeadActor(void* actor);
void KeepDeadPlayerParked(void* deadActor, void* partnerActor);
void RespawnPlayer(void* deadActor, void* partnerActor);

}  // namespace mod::patches
