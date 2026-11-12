#include "patch_collision.hpp"
#include "patch_shared.hpp"
#include "patch_respawn.hpp"
#include "lib.hpp"
#include "program/loggers.hpp"

namespace mod::patches {

namespace {

constexpr std::uintptr_t kPatch0007MarioAttackSensor = 0x5F7A0;

// Compile-time switch for collision sensor suppression during custom invulnerability/ghost state
// Set to false for vanilla damage testing so all enemy attacks reach players natively
constexpr bool kSensorSuppressionEnabled = kBubbleRespawnEnabled;

HOOK_DEFINE_TRAMPOLINE(Patch0007MarioAttackSensor) {
    static void Callback(std::uintptr_t actor, std::uintptr_t sender, std::uintptr_t receiver) {
        void* p1 = GetFirstPlayer();
        void* p2 = GetSecondPlayer();

        if (p1 != nullptr && p2 != nullptr && IsGuestPointer(sender) && IsGuestPointer(receiver)) {
            void* hostSender = *reinterpret_cast<void**>(sender + 0x30);
            void* hostReceiver = *reinterpret_cast<void**>(receiver + 0x30);

            if ((hostSender == p1 && hostReceiver == p2) ||
                (hostSender == p2 && hostReceiver == p1)) {
                return;
            }

            // Suppress sensor collision for dead or invulnerable players when enabled.
            if (kSensorSuppressionEnabled) {
                if ((hostSender == p1 && (IsP1Dead() || IsCoopTravelHidden(p1) || IsPlayerInvulnerable(p1))) ||
                    (hostReceiver == p1 && (IsP1Dead() || IsCoopTravelHidden(p1) || IsPlayerInvulnerable(p1))) ||
                    (hostSender == p2 && (IsP2Dead() || IsCoopTravelHidden(p2) || IsPlayerInvulnerable(p2))) ||
                    (hostReceiver == p2 && (IsP2Dead() || IsCoopTravelHidden(p2) || IsPlayerInvulnerable(p2)))) {
                    return;
                }
            }
        }

        Orig(actor, sender, receiver);
    }
};

}  // namespace

void InstallCollision(const galaxy::target::Identity& identity) {
    if (identity.game == galaxy::target::Game::Smg1) {
        Patch0007MarioAttackSensor::InstallAtOffset(kPatch0007MarioAttackSensor);
        Logging.Log("[GalaxyCoop:%s] PATCH-0007 collision filter installed sensor=0x%lx kill=1",
                    galaxy::target::GameName(identity.game),
                    static_cast<unsigned long>(kPatch0007MarioAttackSensor));
    }
}

}  // namespace mod::patches
