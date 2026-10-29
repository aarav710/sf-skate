// Jet skis moored along the shore. Find one, press E to hop on, and cruise the bay and the ocean.
#pragma once
#include "common.h"
#include "world.h"
#include "player.h"

struct Ski { Vector3 pos{}; float yaw = 0; int color = 0; };

struct JetSkis {
    std::vector<Ski> skis;
    bool riding = false;
    int cur = -1;                    // the ski being ridden
    float speed = 0, yaw = 0, lean = 0, pitch = 0, bob = 0, throttle = 0;
    Vector3 vel{};
    float hitCD = 0;
    // events for sound effects, read and cleared by main
    bool evCrash = false, evMount = false;

    struct Spray { Vector3 pos, vel; float life; };
    struct Foam { Vector3 pos; float age; };
    std::vector<Spray> spray;
    std::vector<Foam> trail;

    void place(const World& w);                       // moor a few skis at scattered spots along the shore
    int nearest(const Vector3& p, float maxD) const;  // index of a moored ski within reach, or -1
    void mount(int i, Player& p);
    bool dismount(Player& p, World& w);               // false if no shore is close enough
    void update(float dt, const Input& in, bool boost, Player& p, const World& w);
    void draw(float now, const Vector3& cam) const;   // moored skis and the one being ridden, in the lit pass
    void drawFx(const Vector3& cam) const;            // spray and wake foam, blended
};
