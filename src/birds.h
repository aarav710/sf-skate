// Seagulls wheeling over the waterfront and piers, and pigeons pecking around the plazas that burst into the air
// when a skater rolls through, circle, and settle back down.
#pragma once
#include "common.h"
#include "world.h"

struct Bird {
    Vector3 pos{}, vel{};
    Vector3 home{};          // gull: center of its circle; pigeon: its patch of plaza
    float yaw = 0, phase = 0, t = 0, radius = 20, speed = 1, alt = 20;
    int state = 0;           // pigeons: 0 on the ground, 1 flying off, 2 coming back down
    bool gull = false;
    Vector3 target{};
};

struct Birds {
    std::vector<Bird> birds;
    bool evScatter = false;  // a flock just took off (for the flutter sound)
    void init(const World& w);
    void update(float dt, const World& w, Vector3 skater, float skaterSpeed);
    void draw(Vector3 cam, Vector3 camFwd) const;
};
