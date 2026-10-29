// Aerial gondolas over the hills: cabins circulate on the cables built by World::buildGondolas.
#pragma once
#include "common.h"
#include "world.h"

struct Gondolas {
    struct Cabin { int line; float u; int tint; };   // u: distance around the loop (0..2*len): out on one cable, back on the other
    std::vector<Cabin> cabins;

    void init(const World& w);
    void update(float dt, const World& w);
    void draw(const World& w, const Vector3& cam) const;
};
