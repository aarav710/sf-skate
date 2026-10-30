// Collects colored triangles for the static city, later uploaded as GPU meshes.
#pragma once
#include "common.h"

// Normals: each vertex carries 4 signed bytes. (0,0,0,w) asks World::upload() to derive a normal from the
// triangles around it, smoothing across edges sharper than ~35 degrees (w = 0, boxy things stay crisp) or ~75
// degrees (w = SMOOTH_ORGANIC: trunks, domes, fronds). w = NRM_EXPLICIT means the normal was given (terrain,
// foliage that shades as a soft volume) and the shader uses it as is instead of flipping it toward the eye.
enum : signed char { SMOOTH_CRISP = 0, SMOOTH_ORGANIC = 1, NRM_EXPLICIT = 127 };

struct MeshBuilder {
    std::vector<float> v;
    std::vector<unsigned char> c;
    std::vector<signed char> n;
    signed char smooth = SMOOTH_CRISP;   // smoothing for the vertices that follow
    const Vector3* volN = nullptr;       // leaf(): explicit normal for a leaf in a canopy
    // leafCloud() puts its leaves in detailB (drawn up close) and a low-poly blob in farB (drawn further out)
    MeshBuilder* detailB = nullptr;
    MeshBuilder* farB = nullptr;

    size_t count() const { return v.size() / 3; }
    inline void vert(Vector3 p, Color k) {
        v.push_back(p.x); v.push_back(p.y); v.push_back(p.z);
        c.push_back(k.r); c.push_back(k.g); c.push_back(k.b); c.push_back(k.a);
        n.push_back(0); n.push_back(0); n.push_back(0); n.push_back(smooth);
    }
    inline void vertN(Vector3 p, Color k, Vector3 nn) {
        v.push_back(p.x); v.push_back(p.y); v.push_back(p.z);
        c.push_back(k.r); c.push_back(k.g); c.push_back(k.b); c.push_back(k.a);
        float l = sqrtf(nn.x * nn.x + nn.y * nn.y + nn.z * nn.z);
        if (l < 1e-6f) { nn = {0, 1, 0}; l = 1; }
        n.push_back((signed char)(nn.x / l * 126)); n.push_back((signed char)(nn.y / l * 126)); n.push_back((signed char)(nn.z / l * 126));
        n.push_back(NRM_EXPLICIT);
    }
    void release() { std::vector<float>().swap(v); std::vector<unsigned char>().swap(c); std::vector<signed char>().swap(n); }
    void tri(Vector3 a, Vector3 b, Vector3 d, Color k) { vert(a, k); vert(b, k); vert(d, k); }
    void quad(Vector3 a, Vector3 b, Vector3 d, Vector3 e, Color k) { tri(a, b, d, k); tri(a, d, e, k); }
    void quadC(Vector3 a, Vector3 b, Vector3 d, Vector3 e, Color ka, Color kb, Color kd, Color ke) {
        vert(a, ka); vert(b, kb); vert(d, kd);
        vert(a, ka); vert(d, kd); vert(e, ke);
    }

    // 8 corners indexed by bits (x=1, y=2, z=4). ao darkens the bottom corners.
    void hexa(const Vector3 p[8], Color side, Color top, float ao = 1.f, bool bottom = false);
    // Axis aligned box. side/top colors may differ (e.g. facade vs roof).
    void box(Vector3 mn, Vector3 mx, Color side, Color top, float ao = 1.f, bool bottom = false);
    void box(Vector3 mn, Vector3 mx, Color k, float ao = 1.f) { box(mn, mx, k, k, ao); }
    // Box rotated by yaw (and pitch) around its center. half = half extents (x fwd, y up, z right).
    void obox(Vector3 center, Vector3 half, float yaw, float pitch, Color k);
    // Rectangular beam between two points.
    void beam(Vector3 a, Vector3 b, float w, float h, Color k);
    // n-sided prism / cone / tapered column standing on base.
    void prism(Vector3 base, int n, float r0, float r1, float height, Color side, Color top,
               float rot = 0.f, bool cap = true, float ao = 1.f);
    void sphere(Vector3 ctr, float r, int rings, int segs, Color k, bool hemi = false);
    // Slab with sloped top (h0 -> h1 along axis 0=x or 1=z) down to a flat bottom.
    void slab(float x0, float x1, float z0, float z1, float h0, float h1, int axis, float bottom,
              Color top, Color side);
    // Hip roof: four sloped faces meeting at a ridge along the longer side.
    void hip(float x0, float x1, float z0, float z1, float y, float rise, Color roof);
    // Foliage. One leaf: a diamond split along its midrib of length len at p, facing along unit vector n, rolled by roll.
    void leaf(Vector3 p, Vector3 n, float len, float width, float roll, Color k);
    // A mass of individual leaves scattered through an ellipsoid (radii r), over a dark core so gaps read as depth
    // instead of sky. Leaves are lighter toward the top and sun side, darker inside and underneath, and a few turn.
    void leafCloud(Vector3 c, Vector3 r, int count, float len, float width, Color base, RNG& rng, bool withCore = true, float autumn = 0.f);
    // Tapered trunk or limb between two points (n-sided).
    void limb(Vector3 a, Vector3 b, float r0, float r1, Color k, int n = 6);
    // A palm frond (arched midrib with paired leaflets) rooted at p heading along dir (horizontal unit vector).
    void frond(Vector3 p, Vector3 dir, float len, float droop, Color k, RNG& rng);
    // A small flower: stem, leaves and a ring of petals around a center.
    void flower(Vector3 p, float h, Color petal, Color center, RNG& rng);
    // Gable roof over a rectangle, ridge running along axis (0 = x, 1 = z).
    void gable(float x0, float x1, float z0, float z1, float y, float rise, int axis, Color roof, Color wall);
};
