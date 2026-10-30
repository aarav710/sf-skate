// Rain (streaks around the camera, wet streets), and steam rising from manholes.
#pragma once
#include "common.h"
#include "world.h"

struct Weather {
    bool raining = false;
    float rain = 0;     // how hard it is raining right now, 0..1 (eases in and out)
    float wet = 0;      // how wet the streets are: soaks in over ~15 s, dries over a minute or so
    void init();
    void update(float dt);
    // translucent: call with depth writes off, after the opaque passes
    void drawRain(const Camera3D& cam, const World& w, float now, float night, int quality) const;
    void drawSteam(const Camera3D& cam, const World& w, float now, float night) const;
private:
    Texture2D puff{};   // a soft round blob, made in code
};
