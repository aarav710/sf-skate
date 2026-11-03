// Seaplanes: a rare few moored at particular spots on the water. Climb in with E, taxi out, take off, fly
// over the city and the bay, and land back on the water. A couple more circle overhead with people aboard.
// On the water the plane is upright and steered by yaw alone. In the air its attitude is a quaternion, so it can pitch
// and roll through any angle: loops, barrel rolls, inverted flight. Stunts score as combos like the skater's.
#pragma once
#include "common.h"
#include "world.h"
#include "player.h"

struct Seaplanes {
    struct Moor { Vector3 pos; float yaw; Color body; };
    std::vector<Moor> moored;

    bool riding = false;
    int cur = -1;
    Vector3 pos{};
    // yaw/pitch/roll are the nose heading, nose elevation and bank (positive: right wing down), kept up to date from the
    // attitude for the camera, HUD and landing rules. On the water they are the pose; in the air `q` is. Assigning
    // them while airborne (a test or hook setting up a pose) re-seeds q and the flight path from them on the next update.
    float yaw = 0, pitch = 0, roll = 0, speed = 0, throttle = 0, prop = 0;
    bool onWater = true;
    Quaternion q{0, 0, 0, 1};        // attitude, body (x forward, y up, z right) to world; the source of truth in the air
    Vector3 dir{1, 0, 0};            // direction of flight: follows the nose with some lag
    float sagY = 0;                  // vertical sink, when the wings can't carry the weight (slow, or upside down)
    float stickIdle = 0;             // seconds since the last pitch/roll input: the auto-level waits for it
    float eY = 0, eP = 0, eR = 0;    // yaw/pitch/roll as last derived from q, to spot an outside assignment
    // stunts: progress and what's been done this flight (the counters are for tests and stats)
    float pitchAcc = 0, rollAcc = 0, pitchIdle = 0, rollIdle = 0, invT = 0, sinceStunt = 0;
    bool seenInvP = false, seenInvR = false, invScored = false;
    int loops = 0, rolls = 0, inverts = 0;
    bool evBanked = false;           // a combo was banked this update (so main doesn't talk over it)
    Vector3 boardedAt{};             // where the rider climbed in from, for after a crash
    // events for sound effects, read and cleared by main
    bool evCrash = false, evSplash = false, evMount = false;

    struct Spray { Vector3 pos, vel; float life; };
    std::vector<Spray> spray;

    void place(const World& w);                       // three moorings, in particular places
    int nearest(const Vector3& p, float maxD) const;
    void mount(int i, Player& p);
    bool dismount(Player& p, World& w);               // only on the water, slow, close to a shore inside the play area
    // why the last dismount() said no, and what to tell the rider
    enum Refusal { REF_NONE, REF_FLYING, REF_TOO_FAST, REF_NO_SHORE, REF_OUT_OF_PLAY };
    Refusal refusal = REF_NONE;
    const char* refusalMsg() const;
    static Quaternion poseQuat(float yaw, float pitch, float roll);   // the attitude yaw/pitch/roll describe: yaw about Y, then pitch about z, then roll about x
    static Vector3 toWorld(Vector3 pos, Quaternion q, Vector3 local); // a point of the plane (drawPlane's frame) in the world
    void setPose(float y, float p, float r);                           // point the plane (and its flight path) along yaw/pitch/roll
    void stunts(float dt, float pin, float rin, float dPitch, float dRoll, Player& p, const World& w);
    void update(float dt, const Input& in, bool climb, bool dive, Player& p, World& w);
    float altitude(const World& w) const;
    float upY() const { return Vector3RotateByQuaternion({0, 1, 0}, q).y; }   // 1 upright, -1 upside down
    Vector3 up() const { return Vector3RotateByQuaternion({0, 1, 0}, q); }
    Vector3 nose() const { return Vector3RotateByQuaternion({1, 0, 0}, q); }
    void draw(float now, const Vector3& cam) const;   // moored planes, the one being flown and the ones circling overhead
    void drawFx() const;
};
