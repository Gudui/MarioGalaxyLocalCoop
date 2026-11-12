#pragma once

namespace mod::patches {
// PATCH-0169: ownership is independent of movement-start ordering. Both
// moving or both idle retain the leader; only sustained sole movement
// transfers it. The initial claim is faster than subsequent handoffs.
struct LeaderCameraPolicy {
    int leader = 0;
    unsigned soleTicks = 0;
    int candidate = -1;
    bool claimed = false;

    void Pause() { soleTicks = 0; candidate = -1; }
    void Follow(int index) { leader = index; claimed = true; Pause(); }
    void Tick(bool p1Moving, bool p2Moving) {
        if (p1Moving == p2Moving) { Pause(); return; }
        const int moving = p1Moving ? 0 : 1;
        if (candidate != moving) { candidate = moving; soleTicks = 0; }
        if (++soleTicks >= (claimed ? 60u : 12u)) Follow(moving);
    }
    int Follower(bool p1Moving, bool p2Moving) const {
        // An idle player always follows the mover, even during a pending
        // camera handoff. Otherwise preserve the settled camera owner.
        if (p1Moving != p2Moving) return p1Moving ? 1 : 0;
        return 1 - leader;
    }
};
}
