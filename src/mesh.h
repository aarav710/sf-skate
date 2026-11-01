// Collects colored triangles for the static city, later uploaded as GPU meshes.
#pragma once
#include "common.h"
#include <sys/mman.h>

// Allocator for the big generation and bake buffers: anything from 64 KB up comes straight from the OS (mmap) and
// goes straight back when freed. malloc keeps freed blocks around for reuse, and with a city's worth of growing and
// freed builder arrays that left several hundred MB of dead memory resident at the startup peak.
template <class T> struct OsAlloc {
    using value_type = T;
    static constexpr size_t BIG = 64 * 1024;
    OsAlloc() = default;
    template <class U> OsAlloc(const OsAlloc<U>&) {}
    T* allocate(size_t n) {
        size_t bytes = n * sizeof(T);
        if (bytes < BIG) {
            if (void* p = malloc(bytes)) return (T*)p;
            throw std::bad_alloc();
        }
        void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) throw std::bad_alloc();
        return (T*)p;
    }
    void deallocate(T* p, size_t n) {
        if (n * sizeof(T) < BIG) free(p);
        else munmap(p, n * sizeof(T));
    }
    template <class U> bool operator==(const OsAlloc<U>&) const { return true; }
    template <class U> bool operator!=(const OsAlloc<U>&) const { return false; }
};
template <class T> using BigVec = std::vector<T, OsAlloc<T>>;

// Normals: each vertex carries 4 signed bytes. (0,0,0,w) asks World::upload() to derive a normal from the
// triangles around it, smoothing across edges sharper than ~35 degrees (w = 0, boxy things stay crisp) or ~75
// degrees (w = SMOOTH_ORGANIC: trunks, domes, fronds). w = NRM_EXPLICIT means the normal was given (terrain,
// foliage that shades as a soft volume) and the shader uses it as is instead of flipping it toward the eye.
enum : signed char { SMOOTH_CRISP = 0, SMOOTH_ORGANIC = 1, NRM_EXPLICIT = 127 };

// Colors and normals are stored as runs (4 bytes plus the index just past the run's last vertex): neighbouring
// vertices mostly share them (a box face, a leaf, a whole building's "derive my normal"), so the whole city takes
// about 840 MB instead of 1.1 GB while it is generated. Lossless: World::upload() expands it to the same bytes.
struct MeshRun { unsigned char d[4]; uint32_t end; };

// Everything one leafCloud() call needs to make its leaves again, exactly: the arguments and the generator's state
// on the way in. The leaves of the whole city are kept like this and meshed only near the camera (World::updateFoliage).
struct LeafSrc {
    Vector3 c, rad;
    float len, width, autumn;
    Color base;
    uint32_t rng;
    uint16_t count;
    bool withCore;
    size_t verts() const { return (withCore ? 108 : 0) + (size_t)count * 6; }   // vertices leafCloud() emits
};

struct MeshBuilder {
    BigVec<float> v;                     // xyz per vertex
    BigVec<MeshRun> c, n;                // colors (rgba), normals (xyz + smoothing flag), run-length coded
    signed char smooth = SMOOTH_CRISP;   // smoothing for the vertices that follow
    const Vector3* volN = nullptr;       // leaf(): explicit normal for a leaf in a canopy
    // leafCloud() puts a low-poly blob in farB (drawn further out) and its leaves, drawn up close, in leafSrc as a
    // source to mesh them from later (and in detailB too, if set: the headless checks compare the two)
    std::vector<LeafSrc>* leafSrc = nullptr;
    MeshBuilder* detailB = nullptr;
    MeshBuilder* farB = nullptr;

    size_t count() const { return v.size() / 3; }
    static inline void addRun(BigVec<MeshRun>& r, const void* d) {   // one more vertex with these 4 bytes
        if (!r.empty() && !memcmp(r.back().d, d, 4)) { r.back().end++; return; }
        MeshRun q;
        memcpy(q.d, d, 4);
        q.end = r.empty() ? 1 : r.back().end + 1;
        r.push_back(q);
    }
    inline void vert(Vector3 p, Color k) {
        v.push_back(p.x); v.push_back(p.y); v.push_back(p.z);
        addRun(c, &k);
        signed char nn[4] = {0, 0, 0, smooth};
        addRun(n, nn);
    }
    inline void vertN(Vector3 p, Color k, Vector3 nn) {
        v.push_back(p.x); v.push_back(p.y); v.push_back(p.z);
        addRun(c, &k);
        float l = sqrtf(nn.x * nn.x + nn.y * nn.y + nn.z * nn.z);
        if (l < 1e-6f) { nn = {0, 1, 0}; l = 1; }
        signed char q[4] = {(signed char)(nn.x / l * 126), (signed char)(nn.y / l * 126), (signed char)(nn.z / l * 126), NRM_EXPLICIT};
        addRun(n, q);
    }
    void release() { BigVec<float>().swap(v); BigVec<MeshRun>().swap(c); BigVec<MeshRun>().swap(n); }
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
    void leafCloud(const LeafSrc& s) { RNG r(1); r.s = s.rng; leafCloud(s.c, s.rad, s.count, s.len, s.width, s.base, r, s.withCore, s.autumn); }
    // the generator draws of leafCloud(), without making anything: rng ends up exactly where leafCloud() leaves it
    static void skipLeafCloud(int count, float autumn, RNG& rng);
    // Tapered trunk or limb between two points (n-sided).
    void limb(Vector3 a, Vector3 b, float r0, float r1, Color k, int n = 6);
    // A palm frond (arched midrib with paired leaflets) rooted at p heading along dir (horizontal unit vector).
    void frond(Vector3 p, Vector3 dir, float len, float droop, Color k, RNG& rng);
    // A small flower: stem, leaves and a ring of petals around a center.
    void flower(Vector3 p, float h, Color petal, Color center, RNG& rng);
    // Gable roof over a rectangle, ridge running along axis (0 = x, 1 = z).
    void gable(float x0, float x1, float z0, float z1, float y, float rise, int axis, Color roof, Color wall);
};
