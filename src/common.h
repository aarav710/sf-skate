// Shared helpers: math, colors, random numbers, material codes.
#pragma once
#include "raylib.h"
#include "raymath.h"
#include "rlgl.h"
#include <vector>
#include <string>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

constexpr float TAU_F = 6.28318530718f;
constexpr float GRAVITY = 16.0f; // a bit stronger than real life for snappy jumps

inline float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float sq(float x) { return x * x; }
inline float smooth01(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
inline float wrapPi(float a) {
    while (a > PI) a -= TAU_F;
    while (a < -PI) a += TAU_F;
    return a;
}
inline float lerpAngle(float a, float b, float t) { return a + wrapPi(b - a) * t; }
inline Vector3 V3(float x, float y, float z) { return Vector3{x, y, z}; }
inline Vector2 V2(float x, float y) { return Vector2{x, y}; }

// Rotate a local vector (x = forward, z = right) by yaw around Y.
// Matches rlRotatef(-yaw * RAD2DEG, 0, 1, 0).
inline Vector3 rotY(Vector3 l, float yaw) {
    float c = cosf(yaw), s = sinf(yaw);
    return {l.x * c - l.z * s, l.y, l.x * s + l.z * c};
}

inline float distToSeg2D(float px, float pz, float ax, float az, float bx, float bz, float* tOut = nullptr) {
    float ex = bx - ax, ez = bz - az;
    float len2 = ex * ex + ez * ez;
    float t = len2 > 0 ? clampf(((px - ax) * ex + (pz - az) * ez) / len2, 0.f, 1.f) : 0.f;
    if (tOut) *tOut = t;
    float dx = px - (ax + ex * t), dz = pz - (az + ez * t);
    return sqrtf(dx * dx + dz * dz);
}

struct RNG {
    uint32_t s;
    explicit RNG(uint32_t seed = 1) : s(seed ? seed : 1) {}
    uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float f() { return (next() & 0xFFFFFF) / 16777216.0f; }
    float range(float a, float b) { return a + (b - a) * f(); }
    int irange(int a, int b) { return a + (int)(next() % (uint32_t)(b - a + 1)); }
    bool chance(float p) { return f() < p; }
};

inline uint32_t hashu(int x, int z, int s = 0) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)z * 668265263u + (uint32_t)s * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
inline float hash01(int x, int z, int s = 0) { return (hashu(x, z, s) & 0xFFFFFF) / 16777216.0f; }

// Material codes are stored in the vertex alpha channel and decoded by the shader.
enum : unsigned char {
    M_PLAIN = 255,    // regular lit surface
    M_OFFICE = 240,   // office facade with window grid
    M_HOUSE = 225,    // residential facade with smaller windows
    M_WATER = 210,    // animated water
    M_EMISSIVE = 195, // glows at night (lamps, headlights, bridge lights)
    M_GROUND = 180,   // ground with noise detail
    M_GLASS = 165,    // glass curtain wall
    // M_HOUSE variants whose window rows are shifted up by 1-3 quarters of a storey, so each rowhouse can
    // line its floors up with its own entry level (see World::addBuilding).
    M_HOUSE1 = 150,
    M_HOUSE2 = 135,
    M_HOUSE3 = 120,
    M_INTERIOR = 105, // inside a house: lit by lamps and window light, not by the sun
};

inline Color col(int r, int g, int b, unsigned char m = M_PLAIN) {
    return Color{(unsigned char)r, (unsigned char)g, (unsigned char)b, m};
}
inline Color withMat(Color c, unsigned char m) { c.a = m; return c; }
inline Color scaleC(Color c, float k) {
    return Color{(unsigned char)clampf(c.r * k, 0, 255), (unsigned char)clampf(c.g * k, 0, 255),
                 (unsigned char)clampf(c.b * k, 0, 255), c.a};
}
inline Color mixC(Color a, Color b, float t) {
    return Color{(unsigned char)lerpf(a.r, b.r, t), (unsigned char)lerpf(a.g, b.g, t),
                 (unsigned char)lerpf(a.b, b.b, t), a.a};
}
inline Color jitterC(Color c, RNG& r, float amt) { return scaleC(c, 1.0f + r.range(-amt, amt)); }
