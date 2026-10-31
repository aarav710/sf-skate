// Seaplanes: a rare few moored at particular spots on the water. Climb in with E, taxi out, take off, fly
// over the city and the bay, and land back on the water. A couple more circle overhead with people aboard.
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
    float yaw = 0, pitch = 0, roll = 0, speed = 0, throttle = 0, prop = 0;
    bool onWater = true;
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
    void update(float dt, const Input& in, bool climb, bool dive, Player& p, World& w);
    float altitude(const World& w) const;
    void draw(float now, const Vector3& cam) const;   // moored planes, the one being flown and the ones circling overhead
    void drawFx() const;
};
