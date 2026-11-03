// Aerial gondolas over the hills: cabins circulate on the cables built by World::buildGondolas.
#pragma once
#include "common.h"
#include "world.h"

// how far a cabin's floor hangs below its cable (the body in Gondolas::draw), plus a little for its sway
constexpr float GONDOLA_CABIN_DROP = 5.05f + 0.05f;

struct Gondolas {
    struct Cabin { int line; float u; int tint; double t0 = 0; };   // u: distance around the loop (0..2*len): out on one cable,
                                                                    // back on the other; t0: when it was at u = 0 (see setTime)
    std::vector<Cabin> cabins;

    void init(const World& w);
    void update(float dt, const World& w);
    // Every cabin where it is `t` s after init, as update would have it (to within cm) had it run all along: in a
    // session, from the session clock, so they're in the same place for everyone and don't stop for a pause.
    void setTime(double t, const World& w);
    void draw(const World& w, const Vector3& cam) const;

private:
    static constexpr float TABLE_STEP = 1.f;   // m of loop between the entries of loopT
    std::vector<std::vector<double>> loopT;    // per line: how long a cabin takes from u = 0 to each TABLE_STEP of u
    double timeAt(int line, float u) const;
};
