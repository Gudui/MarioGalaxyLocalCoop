#pragma once
namespace mod::patches {
// PATCH-0196: the GX producer and consumer poll for work with
// nn::os::SleepThread(100 ns). Where such a sleep lasts far longer than asked
// (Eden: ~0.1 ms), split frames yield briefly instead; native elsewhere.
#if defined(GALAXY_IDLE_POLL) && GALAXY_IDLE_POLL
void InstallIdlePoll();
void IdlePollFrame(bool split);
#else
inline void InstallIdlePoll() {}
inline void IdlePollFrame(bool) {}
#endif
}
