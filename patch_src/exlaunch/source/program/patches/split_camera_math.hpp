#pragma once
#include "planet_camera_math.hpp"
#include <cstring>

namespace mod::patches::splitmath {
using planet::Vec;
struct Quaternion { float w, x, y, z; };

inline Quaternion RotationQuaternion(const float v[12]) {
    const float trace = v[0] + v[5] + v[10];
    if (trace > 0) {
        const float s = std::sqrt(trace + 1) * 2;
        return {s * .25f, (v[9] - v[6]) / s, (v[2] - v[8]) / s, (v[4] - v[1]) / s};
    }
    if (v[0] > v[5] && v[0] > v[10]) {
        const float s = std::sqrt(1 + v[0] - v[5] - v[10]) * 2;
        return {(v[9] - v[6]) / s, s * .25f, (v[1] + v[4]) / s, (v[2] + v[8]) / s};
    }
    if (v[5] > v[10]) {
        const float s = std::sqrt(1 + v[5] - v[0] - v[10]) * 2;
        return {(v[2] - v[8]) / s, (v[1] + v[4]) / s, s * .25f, (v[6] + v[9]) / s};
    }
    const float s = std::sqrt(1 + v[10] - v[0] - v[5]) * 2;
    return {(v[4] - v[1]) / s, (v[2] + v[8]) / s, (v[6] + v[9]) / s, s * .25f};
}

inline Vec ViewPoint(const float v[12], Vec p) {
    return {v[0]*p.x + v[1]*p.y + v[2]*p.z + v[3],
            v[4]*p.x + v[5]*p.y + v[6]*p.z + v[7],
            v[8]*p.x + v[9]*p.y + v[10]*p.z + v[11]};
}

// Interpolate a rigid rotation and the player's view-space placement. This
// keeps the player framed through a large orbit/roll change; linear matrix
// blending would shear the world or become singular at an opposing rotation.
inline bool BlendPlayerView(const float native[12], const float desired[12], Vec player,
                            float amount, float out[12]) {
    for (int i=0;i<12;++i)
        if (!std::isfinite(native[i]) || !std::isfinite(desired[i])) return false;
    if (!planet::Finite(player) || !std::isfinite(amount)) return false;
    if (amount <= 0) { std::memcpy(out,native,sizeof(float)*12); return true; }
    if (amount >= 1) { std::memcpy(out,desired,sizeof(float)*12); return true; }
    Quaternion a = RotationQuaternion(native), b = RotationQuaternion(desired);
    if (a.w*b.w + a.x*b.x + a.y*b.y + a.z*b.z < 0) b = {-b.w,-b.x,-b.y,-b.z};
    Quaternion q = {a.w+(b.w-a.w)*amount,a.x+(b.x-a.x)*amount,
                    a.y+(b.y-a.y)*amount,a.z+(b.z-a.z)*amount};
    const float norm = std::sqrt(q.w*q.w+q.x*q.x+q.y*q.y+q.z*q.z);
    if (!std::isfinite(norm) || norm < .001f) return false;
    q.w/=norm; q.x/=norm; q.y/=norm; q.z/=norm;
    out[0]=1-2*(q.y*q.y+q.z*q.z); out[1]=2*(q.x*q.y-q.w*q.z); out[2]=2*(q.x*q.z+q.w*q.y);
    out[4]=2*(q.x*q.y+q.w*q.z); out[5]=1-2*(q.x*q.x+q.z*q.z); out[6]=2*(q.y*q.z-q.w*q.x);
    out[8]=2*(q.x*q.z-q.w*q.y); out[9]=2*(q.y*q.z+q.w*q.x); out[10]=1-2*(q.x*q.x+q.y*q.y);
    const Vec start=ViewPoint(native,player), finish=ViewPoint(desired,player);
    const Vec placement=start+(finish-start)*amount;
    out[3]=placement.x-(out[0]*player.x+out[1]*player.y+out[2]*player.z);
    out[7]=placement.y-(out[4]*player.x+out[5]*player.y+out[6]*player.z);
    out[11]=placement.z-(out[8]*player.x+out[9]*player.y+out[10]*player.z);
    for (int i=0;i<12;++i) if (!std::isfinite(out[i])) return false;
    return true;
}

// Row-major rotation taking unit a onto unit b about the shortest arc. The
// antipodal case turns about the camera right vector projected off a.
inline void MinimalRotation(Vec a, Vec b, Vec fallbackAxis, float amount, float q[9]) {
    const float fullCos = std::clamp(planet::Dot(a, b), -1.0f, 1.0f);
    if (amount <= 0.0f || fullCos > 0.999999f) {
        const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        std::memcpy(q, identity, sizeof(identity));
        return;
    }
    Vec axis = planet::Cross(a, b);
    const float axisLength = planet::Length(axis);
    if (axisLength < 1.0e-4f) {
        axis = planet::Unit(fallbackAxis - a * planet::Dot(fallbackAxis, a), planet::Perpendicular(a));
    } else {
        axis = axis * (1.0f / axisLength);
    }
    const float angle = std::acos(fullCos) * std::clamp(amount, 0.0f, 1.0f);
    const float c = std::cos(angle), s = std::sin(angle);
    const float k = 1 - c;
    q[0] = c + axis.x * axis.x * k;          q[1] = axis.x * axis.y * k - axis.z * s; q[2] = axis.x * axis.z * k + axis.y * s;
    q[3] = axis.y * axis.x * k + axis.z * s; q[4] = c + axis.y * axis.y * k;          q[5] = axis.y * axis.z * k - axis.x * s;
    q[6] = axis.z * axis.x * k - axis.y * s; q[7] = axis.z * axis.y * k + axis.x * s; q[8] = c + axis.z * axis.z * k;
}

// ATT-0206 / EVD-0503: the rig's rotation from the fixed split-entry up to a
// player's up is ill-conditioned near the antipode of that entry up, which a
// player walking forward on a small planet reaches in seconds; its yaw then
// whips. Carry the rotation instead: each frame compose the small minimal
// rotation from last frame's up to this frame's up (parallel transport along
// the player's path), which stays well conditioned all the way round.
struct RigTransport { float q[9]; Vec up, referenceUp; bool valid; };

inline void ResetTransport(RigTransport& t, Vec referenceUp, Vec up, Vec fallbackAxis) {
    MinimalRotation(referenceUp, up, fallbackAxis, 1.0f, t.q);
    t.up = up;
    t.referenceUp = referenceUp;
    t.valid = true;
}

inline void ComposeRotation(const float a[9], float q[9]) {  // q = a * q
    float next[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            next[r * 3 + c] = a[r * 3] * q[c] + a[r * 3 + 1] * q[3 + c] + a[r * 3 + 2] * q[6 + c];
    std::memcpy(q, next, sizeof(next));
}

inline void AdvanceTransport(RigTransport& t, Vec up, Vec fallbackAxis) {
    float step[9], next[9];
    MinimalRotation(t.up, up, fallbackAxis, 1.0f, step);
    std::memcpy(next, t.q, sizeof(next));
    ComposeRotation(step, next);
    // Float acos and MinimalRotation's 0.08 deg identity cut-off lose a little
    // of each small step; turn the carried entry up back onto the live up.
    // The residual is tiny, so this correction is well conditioned.
    const Vec ref = t.referenceUp;
    const Vec carried{next[0]*ref.x + next[1]*ref.y + next[2]*ref.z, next[3]*ref.x + next[4]*ref.y + next[5]*ref.z,
                      next[6]*ref.x + next[7]*ref.y + next[8]*ref.z};
    float correction[9];
    MinimalRotation(planet::Unit(carried), up, fallbackAxis, 1.0f, correction);
    ComposeRotation(correction, next);
    // Re-orthonormalise rows so float drift cannot shear the view.
    Vec rows[3] = {{next[0], next[1], next[2]}, {next[3], next[4], next[5]}, {next[6], next[7], next[8]}};
    rows[0] = planet::Unit(rows[0], Vec{1, 0, 0});
    rows[1] = planet::Unit(rows[1] - rows[0] * planet::Dot(rows[1], rows[0]), planet::Perpendicular(rows[0]));
    rows[2] = planet::Cross(rows[0], rows[1]);
    for (int r = 0; r < 3; ++r) { t.q[r * 3] = rows[r].x; t.q[r * 3 + 1] = rows[r].y; t.q[r * 3 + 2] = rows[r].z; }
    t.up = up;
}

// Row-major rotation by angle about a unit axis (Rodrigues).
inline void AxisRotation(Vec axis, float angle, float q[9]) {
    const float c = std::cos(angle), s = std::sin(angle), k = 1 - c;
    q[0] = c + axis.x * axis.x * k;          q[1] = axis.x * axis.y * k - axis.z * s; q[2] = axis.x * axis.z * k + axis.y * s;
    q[3] = axis.y * axis.x * k + axis.z * s; q[4] = c + axis.y * axis.y * k;          q[5] = axis.y * axis.z * k - axis.x * s;
    q[6] = axis.z * axis.x * k - axis.y * s; q[7] = axis.z * axis.y * k + axis.x * s; q[8] = c + axis.z * axis.z * k;
}

inline Vec Rotate(const float q[9], Vec v) {
    return {q[0]*v.x + q[1]*v.y + q[2]*v.z, q[3]*v.x + q[4]*v.y + q[5]*v.z, q[6]*v.x + q[7]*v.y + q[8]*v.z};
}

// Eye of a rigid view: -(R^T t).
inline Vec ViewEye(const float v[12]) {
    return {-(v[0]*v[3] + v[4]*v[7] + v[8]*v[11]), -(v[1]*v[3] + v[5]*v[7] + v[9]*v[11]),
            -(v[2]*v[3] + v[6]*v[7] + v[10]*v[11])};
}

// Turns the camera rigidly about a world point: rows and eye rotate by q, the
// point keeps its view-space position.
inline void RotateViewAbout(float v[12], Vec point, const float q[9]) {
    const Vec eye = point + Rotate(q, ViewEye(v) - point);
    for (int r = 0; r < 3; ++r) {
        const Vec row = Rotate(q, Vec{v[r*4], v[r*4+1], v[r*4+2]});
        v[r*4] = row.x; v[r*4+1] = row.y; v[r*4+2] = row.z;
        v[r*4+3] = -planet::Dot(row, eye);
    }
}

struct UprightResult { bool changed; float elevationBefore, elevationAfter, rollDegrees; };

// ATT-0205 / EVD-0501: a split reference frozen at entry can see its owner from
// below the owner's ground, or upside down, and the gravity rig keeps that for
// the whole split. Raise the eye about the owner's point to at least
// minElevation (sine above the owner's ground plane), then roll about the
// eye-to-point line until the owner's up is screen-up. Both turns keep the
// point's view-space position, so placement and distance framing are unchanged.
inline UprightResult UprightReference(float v[12], Vec point, Vec up, float minElevation) {
    UprightResult result{false, 0, 0, 0};
    for (int i = 0; i < 12; ++i) if (!std::isfinite(v[i])) return result;
    up = planet::Unit(up, Vec{0, 0, 0});
    const Vec offset = ViewEye(v) - point;
    const float distance = planet::Length(offset);
    if (planet::Length(up) < 0.5f || !planet::Finite(point) || !std::isfinite(distance) || distance < 1) return result;
    float work[12];
    std::memcpy(work, v, sizeof(work));
    result.elevationBefore = planet::Dot(offset, up) / distance;
    if (result.elevationBefore < minElevation) {
        const Vec direction = offset * (1 / distance);
        const Vec right{work[0], work[1], work[2]};
        const Vec axis = planet::Unit(planet::Cross(direction, up),
                                      planet::Unit(right - up * planet::Dot(right, up), planet::Perpendicular(up)));
        const float angle = std::asin(std::clamp(minElevation, -1.0f, 1.0f)) -
                            std::asin(std::clamp(result.elevationBefore, -1.0f, 1.0f));
        float q[9];
        AxisRotation(axis, angle, q);
        RotateViewAbout(work, point, q);
    }
    const Vec line = planet::Unit(point - ViewEye(work), Vec{0, 0, 0});
    const Vec screenUp{work[4], work[5], work[6]};
    const Vec r = screenUp - line * planet::Dot(screenUp, line);
    const Vec w = up - line * planet::Dot(up, line);
    if (planet::Length(line) > 0.5f && planet::Length(w) > 0.05f && planet::Dot(r, w) < 0) {
        const float roll = std::atan2(planet::Dot(line, planet::Cross(r, w)), planet::Dot(r, w));
        float q[9];
        AxisRotation(line, roll, q);
        RotateViewAbout(work, point, q);
        result.rollDegrees = roll * 57.29578f;
    }
    for (int i = 0; i < 12; ++i) if (!std::isfinite(work[i])) return result;
    result.elevationAfter = planet::Dot(ViewEye(work) - point, up) / distance;
    result.changed = std::memcmp(work, v, sizeof(work)) != 0;
    std::memcpy(v, work, sizeof(work));
    return result;
}

// ATT-0207 / EVD-0507: the shared view at split entry frames both players, so
// its owner can sit far off centre (30 deg in the hub), which a half-width
// view crops. Pan the camera in place about its own screen-up axis until the
// point is on the vertical centre line. Eye, screen-up row, roll, elevation and
// the point's vertical angle and distance are unchanged. Returns the pan in
// degrees (0 when the point is behind the eye or the view is not finite).
inline float CentreReference(float v[12], Vec point) {
    for (int i = 0; i < 12; ++i) if (!std::isfinite(v[i])) return 0;
    if (!planet::Finite(point)) return 0;
    const Vec p = ViewPoint(v, point);
    if (p.z >= -1) return 0;
    const float angle = std::atan2(p.x, -p.z);
    const float c = std::cos(angle), s = std::sin(angle);
    float work[12];
    for (int k = 0; k < 4; ++k) {
        work[k] = c * v[k] + s * v[8 + k];  // x' = c x + s z
        work[4 + k] = v[4 + k];
        work[8 + k] = c * v[8 + k] - s * v[k];  // z' = c z - s x
    }
    for (int i = 0; i < 12; ++i) if (!std::isfinite(work[i])) return 0;
    std::memcpy(v, work, sizeof(work));
    return angle * 57.29578f;
}

// ATT-0155: move only the translation, scaling the point's view-space position
// toward targetDistance by amount (0..1). Rotation and screen placement are
// unchanged; never pulls out beyond the entry distance. False leaves out as v.
// maxRatio 1 only pulls in (ATT-0155/0208); above 1 it also pulls out to the
// target, at most maxRatio times the entry distance (ATT-0212).
inline bool FrameDistance(const float v[12], Vec point, float targetDistance, float amount, float out[12],
                          float maxRatio = 1.0f) {
    std::memcpy(out, v, sizeof(float) * 12);
    const Vec placement = ViewPoint(v, point);
    const float distance = planet::Length(placement);
    if (!planet::Finite(placement) || !std::isfinite(distance) || distance < 1 ||
        !std::isfinite(targetDistance) || targetDistance < 100 || targetDistance > 20000 || placement.z >= -1)
        return false;
    const float ratio = std::min(std::max(maxRatio, 1.0f), targetDistance / distance);
    const float scale = 1.0f + (ratio - 1.0f) * std::clamp(amount, 0.0f, 1.0f);
    const Vec change = placement * (scale - 1.0f);
    out[3] += change.x; out[7] += change.y; out[11] += change.z;
    return true;
}

// ATT-0217: the half distance, scale times native but at most maxExtra past
// native, so a camera zone that is already wide is not widened by half again.
inline float HalfDistance(float native, float scale, float maxExtra) {
    return native + std::min(native * (scale - 1.0f), maxExtra);
}

// ATT-0213: follow camera. The yaw (radians, right-handed about up) that turns
// a camera's horizontal forward toward the player's horizontal heading this
// frame: none below minSpeed or when the player runs at the camera (error
// beyond maxError), otherwise gain * error, at most maxStep at fullSpeed.
struct FollowTuning { float minSpeed, fullSpeed, gain, maxStep, maxError; };
inline float FollowYaw(Vec forward, Vec heading, Vec up, const FollowTuning& t) {
    const Vec u = planet::Unit(up);
    const Vec f = forward - u * planet::Dot(forward, u);
    const Vec h = heading - u * planet::Dot(heading, u);
    const float speed = planet::Length(h);
    if (!planet::Finite(f) || !planet::Finite(h) || planet::Length(f) < 1.0e-3f || !(speed >= t.minSpeed))
        return 0.0f;
    const Vec fu = planet::Unit(f), hu = h * (1.0f / speed);
    const float error = std::atan2(planet::Dot(u, planet::Cross(fu, hu)), planet::Dot(fu, hu));
    if (!std::isfinite(error) || std::abs(error) > t.maxError) return 0.0f;
    const float limit = t.maxStep * std::min(1.0f, speed / t.fullSpeed);
    return std::clamp(error * t.gain, -limit, limit);
}

// Camera forward of a view (it looks down -z): minus the third row.
inline Vec ViewForward(const float v[12]) { return {-v[8], -v[9], -v[10]}; }

// ATT-0213: orbit a view about a world point by angle around axis (unit,
// right-handed). Rows are rotated and re-squared (right, up, back = right x
// up); the translation is then rebuilt so the point keeps exactly its old
// view-space placement, so per-frame use cannot drift distance or centring.
inline void OrbitViewAbout(float v[12], Vec point, Vec axis, float angle) {
    const Vec placement = ViewPoint(v, point);
    float q[9];
    AxisRotation(axis, angle, q);
    const Vec right = planet::Unit(Rotate(q, Vec{v[0], v[1], v[2]}), Vec{1, 0, 0});
    const Vec upRow = Rotate(q, Vec{v[4], v[5], v[6]});
    const Vec up = planet::Unit(upRow - right * planet::Dot(upRow, right), planet::Perpendicular(right));
    const Vec rows[3] = {right, up, planet::Cross(right, up)};
    const float place[3] = {placement.x, placement.y, placement.z};
    for (int r = 0; r < 3; ++r) {
        v[r*4] = rows[r].x; v[r*4+1] = rows[r].y; v[r*4+2] = rows[r].z;
        v[r*4+3] = place[r] - planet::Dot(rows[r], point);
    }
}

// ATT-0215 / EVD-0521: a fresh, level follow view for a split (re)opening,
// instead of copying the native view, which after a pipe or a death can sit
// below the ground, rolled over, or a few units away. It looks at point from
// distance, raised by elevation (radians) above the plane normal to up, along
// the native camera's heading flattened onto that plane (the native screen-up
// when the native camera looks along up). Point centred, screen-up toward up.
inline bool SynthesizeFollowView(const float native[12], Vec point, Vec up, float distance,
                                 float elevation, float out[12]) {
    for (int i = 0; i < 12; ++i) if (!std::isfinite(native[i])) return false;
    const Vec u = planet::Unit(up, Vec{0, 0, 0});
    if (planet::Length(u) < 0.5f || !planet::Finite(point) || !std::isfinite(distance) ||
        distance < 1 || !std::isfinite(elevation)) return false;
    const Vec look = ViewForward(native);
    Vec heading = look - u * planet::Dot(look, u);
    if (planet::Length(heading) < 0.2f) {
        // Looking down, the top of the screen is ahead; looking up, behind.
        const Vec screenUp = Vec{native[4], native[5], native[6]} * (planet::Dot(look, u) < 0 ? 1.0f : -1.0f);
        heading = screenUp - u * planet::Dot(screenUp, u);
    }
    heading = planet::Unit(heading, planet::Perpendicular(u));
    const Vec eye = point - heading * (std::cos(elevation) * distance) + u * (std::sin(elevation) * distance);
    const Vec forward = planet::Unit(point - eye, heading);
    const Vec right = planet::Unit(planet::Cross(forward, u), planet::Perpendicular(u));
    const Vec rows[3] = {right, planet::Cross(right, forward), forward * -1.0f};
    for (int r = 0; r < 3; ++r) {
        out[r*4] = rows[r].x; out[r*4+1] = rows[r].y; out[r*4+2] = rows[r].z;
        out[r*4+3] = -planet::Dot(rows[r], eye);
    }
    for (int i = 0; i < 12; ++i) if (!std::isfinite(out[i])) return false;
    return true;
}
} // namespace mod::patches::splitmath
