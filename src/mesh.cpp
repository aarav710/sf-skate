#include "mesh.h"

void MeshBuilder::hexa(const Vector3 p[8], Color side, Color top, float ao, bool bottom) {
    Color sb = scaleC(side, ao);
    // sides (bottom corners have bit 1 cleared)
    static const int faces[4][4] = {{0, 4, 6, 2}, {1, 5, 7, 3}, {0, 1, 3, 2}, {4, 5, 7, 6}};
    for (auto& f : faces) {
        Color k[4];
        for (int i = 0; i < 4; i++) k[i] = (f[i] & 2) ? side : sb;
        quadC(p[f[0]], p[f[1]], p[f[2]], p[f[3]], k[0], k[1], k[2], k[3]);
    }
    quad(p[2], p[3], p[7], p[6], top);
    if (bottom) quad(p[0], p[1], p[5], p[4], sb);
}

void MeshBuilder::box(Vector3 mn, Vector3 mx, Color side, Color top, float ao, bool bottom) {
    Vector3 p[8];
    for (int i = 0; i < 8; i++)
        p[i] = {(i & 1) ? mx.x : mn.x, (i & 2) ? mx.y : mn.y, (i & 4) ? mx.z : mn.z};
    hexa(p, side, top, ao, bottom);
}

void MeshBuilder::obox(Vector3 c, Vector3 h, float yaw, float pitch, Color k) {
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    Vector3 f = {cp * cy, sp, cp * sy};
    Vector3 r = {-sy, 0, cy};
    Vector3 u = Vector3CrossProduct(r, f);
    Vector3 p[8];
    for (int i = 0; i < 8; i++) {
        float a = (i & 1) ? h.x : -h.x, b = (i & 2) ? h.y : -h.y, d = (i & 4) ? h.z : -h.z;
        p[i] = {c.x + f.x * a + u.x * b + r.x * d, c.y + f.y * a + u.y * b + r.y * d, c.z + f.z * a + u.z * b + r.z * d};
    }
    hexa(p, k, k, 1.f, true);
}

void MeshBuilder::beam(Vector3 a, Vector3 b, float w, float h, Color k) {
    Vector3 d = Vector3Subtract(b, a);
    float L = Vector3Length(d);
    if (L < 1e-4f) return;
    Vector3 f = Vector3Scale(d, 1.f / L);
    Vector3 ref = fabsf(f.y) > 0.95f ? V3(1, 0, 0) : V3(0, 1, 0);
    Vector3 r = Vector3Normalize(Vector3CrossProduct(f, ref));
    Vector3 u = Vector3CrossProduct(r, f);
    Vector3 p[8];
    for (int i = 0; i < 8; i++) {
        float s = (i & 1) ? L : 0.f, uu = (i & 2) ? h * 0.5f : -h * 0.5f, rr = (i & 4) ? w * 0.5f : -w * 0.5f;
        p[i] = {a.x + f.x * s + u.x * uu + r.x * rr, a.y + f.y * s + u.y * uu + r.y * rr, a.z + f.z * s + u.z * uu + r.z * rr};
    }
    hexa(p, k, k, 1.f, true);
}

void MeshBuilder::prism(Vector3 b, int n, float r0, float r1, float height, Color side, Color top,
                        float rot, bool cap, float ao) {
    Color sb = scaleC(side, ao);
    for (int i = 0; i < n; i++) {
        float a0 = rot + TAU_F * i / n, a1 = rot + TAU_F * (i + 1) / n;
        Vector3 p0 = {b.x + cosf(a0) * r0, b.y, b.z + sinf(a0) * r0};
        Vector3 p1 = {b.x + cosf(a1) * r0, b.y, b.z + sinf(a1) * r0};
        Vector3 q0 = {b.x + cosf(a0) * r1, b.y + height, b.z + sinf(a0) * r1};
        Vector3 q1 = {b.x + cosf(a1) * r1, b.y + height, b.z + sinf(a1) * r1};
        if (r1 > 0.001f) quadC(p0, p1, q1, q0, sb, sb, side, side);
        else { vert(p0, sb); vert(p1, sb); vert(q0, side); }
        if (cap && r1 > 0.001f) tri(q0, q1, {b.x, b.y + height, b.z}, top);
    }
}

void MeshBuilder::sphere(Vector3 c, float r, int rings, int segs, Color k, bool hemi) {
    int r0 = hemi ? rings / 2 : 0;
    for (int i = r0; i < rings; i++) {
        float t0 = PI * i / rings - PI / 2, t1 = PI * (i + 1) / rings - PI / 2;
        for (int j = 0; j < segs; j++) {
            float p0 = TAU_F * j / segs, p1 = TAU_F * (j + 1) / segs;
            auto P = [&](float t, float p) {
                return V3(c.x + r * cosf(t) * cosf(p), c.y + r * sinf(t), c.z + r * cosf(t) * sinf(p));
            };
            quad(P(t0, p0), P(t0, p1), P(t1, p1), P(t1, p0), k);
        }
    }
}

void MeshBuilder::slab(float x0, float x1, float z0, float z1, float h0, float h1, int axis, float bottom,
                       Color top, Color side) {
    // heights at the 4 top corners
    float a = h0, b = axis == 0 ? h1 : h0, cc = h1, d = axis == 0 ? h0 : h1;
    Vector3 t00 = {x0, a, z0}, t10 = {x1, b, z0}, t11 = {x1, cc, z1}, t01 = {x0, d, z1};
    quad(t00, t10, t11, t01, top);
    Vector3 b00 = {x0, bottom, z0}, b10 = {x1, bottom, z0}, b11 = {x1, bottom, z1}, b01 = {x0, bottom, z1};
    quad(b00, b10, t10, t00, side);
    quad(b10, b11, t11, t10, side);
    quad(b11, b01, t01, t11, side);
    quad(b01, b00, t00, t01, side);
}

void MeshBuilder::hip(float x0, float x1, float z0, float z1, float y, float rise, Color k) {
    float dx = x1 - x0, dz = z1 - z0;
    if (dx >= dz) {
        float zm = (z0 + z1) / 2, a = x0 + dz / 2, b = x1 - dz / 2;
        quad({x0, y, z0}, {x1, y, z0}, {b, y + rise, zm}, {a, y + rise, zm}, k);
        quad({x1, y, z1}, {x0, y, z1}, {a, y + rise, zm}, {b, y + rise, zm}, scaleC(k, 0.92f));
        tri({x0, y, z1}, {x0, y, z0}, {a, y + rise, zm}, scaleC(k, 0.96f));
        tri({x1, y, z0}, {x1, y, z1}, {b, y + rise, zm}, scaleC(k, 0.96f));
    } else {
        float xm = (x0 + x1) / 2, a = z0 + dx / 2, b = z1 - dx / 2;
        quad({x0, y, z1}, {x0, y, z0}, {xm, y + rise, a}, {xm, y + rise, b}, k);
        quad({x1, y, z0}, {x1, y, z1}, {xm, y + rise, b}, {xm, y + rise, a}, scaleC(k, 0.92f));
        tri({x0, y, z0}, {x1, y, z0}, {xm, y + rise, a}, scaleC(k, 0.96f));
        tri({x1, y, z1}, {x0, y, z1}, {xm, y + rise, b}, scaleC(k, 0.96f));
    }
}

void MeshBuilder::gable(float x0, float x1, float z0, float z1, float y, float rise, int axis, Color roof, Color wall) {
    if (axis == 0) {
        float zm = (z0 + z1) * 0.5f;
        quad({x0, y, z0}, {x1, y, z0}, {x1, y + rise, zm}, {x0, y + rise, zm}, roof);
        quad({x0, y, z1}, {x1, y, z1}, {x1, y + rise, zm}, {x0, y + rise, zm}, roof);
        tri({x0, y, z0}, {x0, y, z1}, {x0, y + rise, zm}, wall);
        tri({x1, y, z0}, {x1, y, z1}, {x1, y + rise, zm}, wall);
    } else {
        float xm = (x0 + x1) * 0.5f;
        quad({x0, y, z0}, {x0, y, z1}, {xm, y + rise, z1}, {xm, y + rise, z0}, roof);
        quad({x1, y, z0}, {x1, y, z1}, {xm, y + rise, z1}, {xm, y + rise, z0}, roof);
        tri({x0, y, z0}, {x1, y, z0}, {xm, y + rise, z0}, wall);
        tri({x0, y, z1}, {x1, y, z1}, {xm, y + rise, z1}, wall);
    }
}

// ---------------------------------------------------------------- foliage
static Vector3 unitV(Vector3 v) { float l = Vector3Length(v); return l < 1e-5f ? V3(0, 1, 0) : Vector3Scale(v, 1.f / l); }

void MeshBuilder::leaf(Vector3 p, Vector3 n, float len, float width, float roll, Color k) {
    n = unitV(n);
    Vector3 ref = fabsf(n.y) > 0.9f ? V3(1, 0, 0) : V3(0, 1, 0);
    Vector3 a = unitV(Vector3CrossProduct(n, ref));       // across the leaf face
    Vector3 b = Vector3CrossProduct(n, a);                // along the midrib (before roll)
    float cr = cosf(roll), sr = sinf(roll);
    Vector3 rib = {b.x * cr + a.x * sr, b.y * cr + a.y * sr, b.z * cr + a.z * sr};
    Vector3 side = {a.x * cr - b.x * sr, a.y * cr - b.y * sr, a.z * cr - b.z * sr};
    Vector3 tip = Vector3Add(p, Vector3Scale(rib, len));
    Vector3 l = Vector3Add(Vector3Add(p, Vector3Scale(rib, len * 0.38f)), Vector3Scale(side, width * 0.5f));
    Vector3 rr = Vector3Add(Vector3Add(p, Vector3Scale(rib, len * 0.38f)), Vector3Scale(side, -width * 0.5f));
    Color dk = scaleC(k, 0.86f);
    tri(p, l, tip, k); tri(p, tip, rr, dk);        // two halves either side of the midrib catch the light differently
}

void MeshBuilder::leafCloud(Vector3 c, Vector3 rad, int count, float len, float width, Color base, RNG& rng, bool withCore, float autumn) {
    // dark core: a low-poly ellipsoid inside so the canopy has depth
    Color core = scaleC(base, 0.62f);
    int rings = 3, segs = 6;
    for (int i = 0; i < (withCore ? rings : 0); i++) {
        float t0 = PI * i / rings - PI / 2, t1 = PI * (i + 1) / rings - PI / 2;
        for (int j = 0; j < segs; j++) {
            float p0 = TAU_F * j / segs, p1 = TAU_F * (j + 1) / segs;
            auto P = [&](float t, float p) { return V3(c.x + rad.x * 0.62f * cosf(t) * cosf(p), c.y + rad.y * 0.62f * sinf(t), c.z + rad.z * 0.62f * cosf(t) * sinf(p)); };
            quad(P(t0, p0), P(t0, p1), P(t1, p1), P(t1, p0), core);
        }
    }
    for (int i = 0; i < count; i++) {
        // direction biased to the surface: most leaves sit on the outer shell, fewer deep inside
        Vector3 d;
        do d = {rng.range(-1, 1), rng.range(-1, 1), rng.range(-1, 1)}; while (Vector3LengthSqr(d) > 1.f || Vector3LengthSqr(d) < 0.01f);
        d = unitV(d);
        float depth = 1.f - 0.55f * rng.f() * rng.f();     // 0.45 .. 1 of the radius
        Vector3 off = {d.x * rad.x * depth, d.y * rad.y * depth, d.z * rad.z * depth};
        Vector3 pos = Vector3Add(c, off);
        // lighting by where the leaf sits: top and the sun side bright, underside and interior dim
        float up = d.y * 0.5f + 0.5f, sun = d.x * 0.35f - d.z * 0.2f + 0.5f;
        float shade = 0.85f + 0.4f * up + 0.12f * sun;
        shade *= 0.72f + 0.28f * depth;
        shade *= rng.range(0.88f, 1.1f);
        Color k = scaleC(base, shade);
        k = mixC(k, Color{(unsigned char)clampf(base.r * 1.35f, 0, 255), (unsigned char)clampf(base.g * 1.12f, 0, 255), (unsigned char)(base.b * 0.7f), base.a}, rng.f() < 0.12f ? 0.5f : 0.f); // sun-yellowed
        if (autumn > 0 && rng.f() < autumn) k = mixC(k, Color{200, 120, 40, base.a}, 0.7f);
        // leaves face roughly outward, tilted by a random amount; the tip droops under gravity
        Vector3 nrm = unitV(V3(d.x + rng.range(-0.7f, 0.7f), d.y + rng.range(-0.7f, 0.7f) + 0.2f, d.z + rng.range(-0.7f, 0.7f)));
        float s = rng.range(0.8f, 1.25f);
        leaf(pos, nrm, len * s, width * s, rng.range(0, TAU_F), k);
    }
}

void MeshBuilder::limb(Vector3 a, Vector3 b, float r0, float r1, Color k, int n) {
    Vector3 d = Vector3Subtract(b, a);
    float L = Vector3Length(d);
    if (L < 1e-4f) return;
    Vector3 f = Vector3Scale(d, 1.f / L);
    Vector3 ref = fabsf(f.y) > 0.95f ? V3(1, 0, 0) : V3(0, 1, 0);
    Vector3 u = unitV(Vector3CrossProduct(f, ref)), w = Vector3CrossProduct(f, u);
    Color kb = scaleC(k, 0.8f);
    for (int i = 0; i < n; i++) {
        float a0 = TAU_F * i / n, a1 = TAU_F * (i + 1) / n;
        auto ring = [&](Vector3 base, float r, float ang) {
            return Vector3Add(base, Vector3Add(Vector3Scale(u, cosf(ang) * r), Vector3Scale(w, sinf(ang) * r)));
        };
        // bark: alternate facets slightly darker for a ridged look
        Color s = (i & 1) ? scaleC(k, 0.88f) : k;
        quadC(ring(a, r0, a0), ring(a, r0, a1), ring(b, r1, a1), ring(b, r1, a0), scaleC(s, 0.85f), scaleC(s, 0.85f), s, s);
    }
    (void)kb;
}

void MeshBuilder::frond(Vector3 p, Vector3 dir, float len, float droop, Color k, RNG& rng) {
    const int segs = 6;
    Vector3 side = {-dir.z, 0, dir.x};
    Vector3 prev = p, cur;
    Color dk = scaleC(k, 0.8f);
    for (int s = 1; s <= segs; s++) {
        float t = (float)s / segs;
        // arches up then falls away
        cur = {p.x + dir.x * len * t, p.y + len * (0.55f * sinf(t * 2.2f) - droop * t * t), p.z + dir.z * len * t};
        float lw = len * 0.2f * sinf(PI * (t - 0.04f)) + 0.05f;   // leaflet length: widest mid-frond
        Vector3 pm = Vector3Scale(Vector3Add(prev, cur), 0.5f);
        for (int sd = -1; sd <= 1; sd += 2) {
            Vector3 hang = {side.x * sd * lw, -lw * (0.55f + droop * 0.4f), side.z * sd * lw};
            Vector3 tipL = Vector3Add(pm, hang);
            tri(prev, cur, tipL, sd < 0 ? k : dk);
        }
        beam(prev, cur, 0.07f, 0.05f, scaleC(k, 0.75f));
        prev = cur;
    }
    (void)rng;
}

void MeshBuilder::flower(Vector3 p, float h, Color petal, Color center, RNG& rng) {
    Color stem = col(58, 112, 46);
    Vector3 top = {p.x + rng.range(-0.05f, 0.05f) * h, p.y + h, p.z + rng.range(-0.05f, 0.05f) * h};
    beam(p, top, 0.02f, 0.02f, stem);
    // two leaves on the stem
    for (int i = 0; i < 2; i++) {
        float a = rng.range(0, TAU_F), y = p.y + h * (0.25f + 0.25f * i);
        leaf({p.x, y, p.z}, V3(0, 1, 0), h * 0.35f, h * 0.12f, a, stem);
    }
    int np = 5 + (rng.next() & 1);
    float pr = h * 0.2f;
    for (int i = 0; i < np; i++) {
        float a = TAU_F * i / np + rng.range(0, 0.3f);
        Vector3 e = {top.x + cosf(a) * pr, top.y + pr * 0.15f, top.z + sinf(a) * pr};
        float a2 = a + TAU_F / np * 0.5f;
        Vector3 s1 = {top.x + cosf(a - 0.35f) * pr * 0.6f, top.y, top.z + sinf(a - 0.35f) * pr * 0.6f};
        Vector3 s2 = {top.x + cosf(a + 0.35f) * pr * 0.6f, top.y, top.z + sinf(a + 0.35f) * pr * 0.6f};
        tri(top, s1, e, petal); tri(top, e, s2, scaleC(petal, 0.92f));
        (void)a2;
    }
    box({top.x - 0.02f, top.y, top.z - 0.02f}, {top.x + 0.02f, top.y + 0.03f, top.z + 0.02f}, center);
}
