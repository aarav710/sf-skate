// Streets, city blocks, street furniture, trees and parks.
#include "world_internal.h"
#include "models.h"

int w_stat[4];
// ---------------------------------------------------------------- helpers
void World::addBox(float x0, float z0, float x1, float z1, float base, float top, uint8_t kind,
                   Color side, Color topC, float ao) {
    mb((x0 + x1) / 2, (z0 + z1) / 2).box({x0, base, z0}, {x1, top, z1}, side, topC, ao);
    Platform p{x0, x1, z0, z1, top, top, base, 0, kind};
    addPlat(p);
}

void World::addRailObj(Vector3 a, Vector3 b, const char* name) {
    rails.push_back({a, b, name});
    MeshBuilder& m = mb((a.x + b.x) / 2, (a.z + b.z) / 2);
    Color steel = col(150, 155, 162);
    m.beam(a, b, 0.07f, 0.07f, steel);
    Vector3 d = Vector3Subtract(b, a);
    float L = sqrtf(d.x * d.x + d.z * d.z);
    int n = std::max(2, (int)(L / 2.5f) + 1);
    for (int i = 0; i < n; i++) {
        float t = (float)i / (n - 1);
        Vector3 p = Vector3Lerp(a, b, t);
        float gy = terrain(p.x, p.z);
        if (p.y - gy < 3.0f) m.beam({p.x, gy, p.z}, p, 0.06f, 0.06f, steel);
    }
}

// Lays the convex polygon q (x, z, counter-clockwise like the ground mesh's triangles) onto the ground mesh `off`
// above it: cut along every ground triangle it covers, so each piece lies in that triangle's plane. A strip drawn as
// flat quads between samples cuts the chords of a curved hill (the 10 m ground triangles fold along their edges and
// diagonals), and on steep ground like Telegraph Hill the grass broke up through the asphalt.
static void drapePoly(World& w, MeshBuilder& m, const Vector2* q, int nq, float off, Color c) {
    float ox, oz, S;
    Vector2 ctr = {0, 0};
    for (int k = 0; k < nq; k++) ctr = Vector2Add(ctr, q[k]);
    ctr = Vector2Scale(ctr, 1.f / nq);
    if (!groundMeshGrid(ctr.x, ctr.y, &ox, &oz, &S)) {   // no ground mesh here: follow terrain() at the corners
        for (int k = 1; k + 1 < nq; k++)
            m.tri({q[0].x, w.terrain(q[0].x, q[0].y) + off, q[0].y}, {q[k].x, w.terrain(q[k].x, q[k].y) + off, q[k].y},
                  {q[k + 1].x, w.terrain(q[k + 1].x, q[k + 1].y) + off, q[k + 1].y}, c);
        return;
    }
    float x0 = q[0].x, x1 = q[0].x, z0 = q[0].y, z1 = q[0].y;
    for (int k = 1; k < nq; k++) {
        x0 = std::min(x0, q[k].x); x1 = std::max(x1, q[k].x);
        z0 = std::min(z0, q[k].y); z1 = std::max(z1, q[k].y);
    }
    int i0 = (int)floorf((x0 - ox) / S), i1 = (int)floorf((x1 - ox) / S);
    int j0 = (int)floorf((z0 - oz) / S), j1 = (int)floorf((z1 - oz) / S);
    // Clipping and plane lookup run in coordinates local to each square: in world coordinates (x up to ~6000, z up to
    // ~10000) the products in the edge tests and the area lose metres squared to float rounding and real pieces of
    // road far from the origin came out with a negative area and were dropped
    Vector2 buf[2][16];
    for (int j = j0; j <= j1; j++)
        for (int i = i0; i <= i1; i++) {
            float cx = ox + i * S, cz = oz + j * S;
            float h00 = w.terrainRaw(cx, cz), h10 = w.terrainRaw(cx + S, cz);
            float h01 = w.terrainRaw(cx, cz + S), h11 = w.terrainRaw(cx + S, cz + S);
            // the two triangles of the square, as the ground mesh splits it (see terrain())
            static const Vector2 TRI[2][3] = {{{0, 0}, {1, 0}, {1, 1}}, {{0, 0}, {1, 1}, {0, 1}}};
            for (int t = 0; t < 2; t++) {
                int n = nq, cur = 0;
                for (int k = 0; k < nq; k++) buf[0][k] = {q[k].x - cx, q[k].y - cz};
                for (int e = 0; e < 3 && n >= 3; e++) {   // Sutherland-Hodgman against each triangle edge
                    Vector2 a = Vector2Scale(TRI[t][e], S), b = Vector2Scale(TRI[t][(e + 1) % 3], S);
                    auto side = [&](Vector2 p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); };
                    const Vector2* in = buf[cur];
                    Vector2* out = buf[cur ^ 1];
                    int no = 0;
                    for (int k = 0; k < n && no < 14; k++) {
                        Vector2 p = in[k], r = in[(k + 1) % n];
                        float sp = side(p), sr = side(r);
                        if (sp >= 0) out[no++] = p;
                        if ((sp >= 0) != (sr >= 0)) out[no++] = Vector2Lerp(p, r, sp / (sp - sr));
                    }
                    n = no;
                    cur ^= 1;
                }
                if (n < 3) continue;
                const Vector2* p = buf[cur];
                float area = 0;
                for (int k = 1; k + 1 < n; k++)
                    area += (p[k].x - p[0].x) * (p[k + 1].y - p[0].y) - (p[k + 1].x - p[0].x) * (p[k].y - p[0].y);
                if (area < 1e-6f) continue;
                // height on this triangle's plane (the same plane terrain() reads there)
                auto at = [&](Vector2 v) -> Vector3 {
                    float tx = v.x / S, tz = v.y / S;
                    float y = t == 0 ? h00 + (h10 - h00) * tx + (h11 - h10) * tz : h00 + (h11 - h01) * tx + (h01 - h00) * tz;
                    return {cx + v.x, y + off, cz + v.y};
                };
                Vector3 v0 = at(p[0]);
                for (int k = 1; k + 1 < n; k++) m.tri(v0, at(p[k]), at(p[k + 1]), c);
            }
        }
}

void roadStrip(World& w, Vector2 a, Vector2 b, float halfL, float halfR, float ext, float off, Color c, float step) {
    Vector2 d = Vector2Subtract(b, a);
    float L = Vector2Length(d);
    if (L < 0.01f) return;
    Vector2 dir = Vector2Scale(d, 1.f / L), right = {-dir.y, dir.x};
    Vector2 s = Vector2Subtract(a, Vector2Scale(dir, ext));
    float total = L + 2 * ext;
    // drapePoly cuts each piece to the ground triangles, so `step` no longer shapes the strip: pieces only need to be
    // short enough to keep the strip spread over the mesh chunks it crosses (at least 30 m, longer if step asks)
    int n = std::max(1, (int)ceilf(total / std::max(step, 30.f)));
    for (int k = 0; k < n; k++) {
        float s0 = total * k / n, s1 = total * (k + 1) / n;
        Vector2 p0 = Vector2Add(s, Vector2Scale(dir, s0)), p1 = Vector2Add(s, Vector2Scale(dir, s1));
        Vector2 q[4] = {Vector2Subtract(p0, Vector2Scale(right, halfL)), Vector2Subtract(p1, Vector2Scale(right, halfL)),
                        Vector2Add(p1, Vector2Scale(right, halfR)), Vector2Add(p0, Vector2Scale(right, halfR))};
        Vector2 mid = Vector2Add(p0, Vector2Scale(right, (halfR - halfL) * 0.5f));
        drapePoly(w, w.mb(mid.x, mid.y), q, 4, off, c);
    }
}

void groundPad(World& w, float x0, float z0, float x1, float z1, float off, Color c) {
    int nx = std::max(1, (int)ceilf((x1 - x0) / 5)), nz = std::max(1, (int)ceilf((z1 - z0) / 5));
    for (int j = 0; j < nz; j++)
        for (int i = 0; i < nx; i++) {
            float a = lerpf(x0, x1, (float)i / nx), b = lerpf(x0, x1, (float)(i + 1) / nx);
            float e = lerpf(z0, z1, (float)j / nz), f = lerpf(z0, z1, (float)(j + 1) / nz);
            w.mb((a + b) / 2, (e + f) / 2).quad({a, w.terrain(a, e) + off, e}, {b, w.terrain(b, e) + off, e},
                                                {b, w.terrain(b, f) + off, f}, {a, w.terrain(a, f) + off, f}, c);
        }
}

// ---------------------------------------------------------------- roads
void World::buildRoads(const Grid& g, int style) {
    Color yellow = col(225, 190, 60, M_GROUND), white = col(230, 230, 225, M_GROUND);
    for (int j = 0; j < g.nj; j++)
        for (int i = 0; i < g.ni; i++) {
            if (!g.nodeOk(i, j)) continue;
            for (int d = 0; d < 4; d++) {
                if (!g.edge(i, j, d)) continue;
                Vector2 a = g.node(i, j), b = g.node(i + DI[d], j + DJ[d]);
                Vector2 dir = {(float)DI[d], (float)DJ[d]}, right = {-dir.y, dir.x};
                if (d == 0 || d == 2) roadStrip(*this, a, b, g.roadHalf, g.roadHalf, g.roadHalf, 0.08f, C_ASPHALT);
                if (style == 1) continue; // suburban streets: no paint
                if (d == 0 || d == 2) {
                    // double yellow center line
                    Vector2 a2 = Vector2Add(a, Vector2Scale(dir, 13)), b2 = Vector2Subtract(b, Vector2Scale(dir, 13));
                    roadStrip(*this, Vector2Add(a2, Vector2Scale(right, 0.15f)), Vector2Add(b2, Vector2Scale(right, 0.15f)),
                              0.06f, 0.06f, 0, 0.1f, yellow, 8.f);
                    roadStrip(*this, Vector2Subtract(a2, Vector2Scale(right, 0.15f)), Vector2Subtract(b2, Vector2Scale(right, 0.15f)),
                              0.06f, 0.06f, 0, 0.1f, yellow, 8.f);
                }
                // crosswalk where this street leaves the intersection
                for (float l = -5.6f; l <= 5.61f; l += 1.4f) {
                    Vector2 c0 = Vector2Add(a, Vector2Add(Vector2Scale(dir, 8.5f), Vector2Scale(right, l)));
                    Vector2 c1 = Vector2Add(c0, Vector2Scale(dir, 2.6f));
                    roadStrip(*this, c0, c1, 0.3f, 0.3f, 0, 0.11f, white, 3.f);
                }
            }
        }
}

void World::buildMarket() {
    Vector2 a = {MK_AX, MK_AZ}, b = {MK_BX, MK_BZ};
    roadStrip(*this, a, b, MK_HALF, MK_HALF, 0, 0.085f, C_ASPHALT);
    Vector2 d = Vector2Normalize(Vector2Subtract(b, a)), r = {-d.y, d.x};
    Color rail = col(110, 110, 115, M_GROUND), yellow = col(225, 190, 60, M_GROUND);
    for (float l : {-3.75f, -2.25f, 2.25f, 3.75f})
        roadStrip(*this, Vector2Add(a, Vector2Scale(r, l)), Vector2Add(b, Vector2Scale(r, l)), 0.06f, 0.06f, 0, 0.1f, rail, 8.f);
    roadStrip(*this, Vector2Add(a, Vector2Scale(r, 0.2f)), Vector2Add(b, Vector2Scale(r, 0.2f)), 0.07f, 0.07f, 0, 0.1f, yellow, 8.f);
    roadStrip(*this, Vector2Subtract(a, Vector2Scale(r, 0.2f)), Vector2Subtract(b, Vector2Scale(r, 0.2f)), 0.07f, 0.07f, 0, 0.1f, yellow, 8.f);
    // lamps along both sides
    float L = Vector2Distance(a, b);
    for (float s = 15; s < L - 10; s += 30)
        for (int side = -1; side <= 1; side += 2) {
            Vector2 p = Vector2Add(Vector2Add(a, Vector2Scale(d, s)), Vector2Scale(r, side * (MK_HALF + 0.8f)));
            addLamp(p.x, p.y, atan2f(-r.y * side, -r.x * side));
        }
    // cars U-turn at the ends of their path in a loop as wide as their lane: start it 32 m up Market so the loop
    // (7 m outer lane, plus the car) stays in Market's own roadway and clear of the Embarcadero, where turning
    // across the cross traffic had them driving through each other
    Path P;
    P.p = {Vector2Add(a, Vector2Scale(d, 32)), b};
    P.finish();
    carPaths.push_back(P);
    carPathLanes.push_back(2);
    carPathKind.push_back(1);
}

void World::buildLombard() {
    // "The crookedest street": 8 switchbacks down the east face of Russian Hill.
    float x0 = gx(16) + 6, x1 = gx(17) - 6, zc = gz(3);
    Color brick = col(150, 72, 60, M_GROUND), hedge = col(70, 120, 55), flower = col(220, 80, 130); (void)hedge; (void)flower;
    std::vector<Vector2> pts;
    int legs = 8;
    pts.push_back({gx(16) - 4, zc});
    for (int k = 0; k <= legs; k++) pts.push_back({lerpf(x0, x1, (float)k / legs), zc + ((k % 2) ? 6.5f : -6.5f)});
    pts.push_back({gx(17) + 4, zc});
    for (size_t k = 0; k + 1 < pts.size(); k++) roadStrip(*this, pts[k], pts[k + 1], 2.4f, 2.4f, 2.0f, 0.1f, brick, 3.f);
    RNG r(99);
    for (int k = 1; k <= legs; k++) {
        Vector2 a = pts[k], b = pts[k + 1];
        Vector2 d = Vector2Normalize(Vector2Subtract(b, a)), n = {-d.y, d.x};
        float L = Vector2Distance(a, b);
        for (int side = -1; side <= 1; side += 2)
            for (float s = 2.5f; s < L - 2.0f; s += 1.6f) {
                Vector2 p = Vector2Add(Vector2Add(a, Vector2Scale(d, s)), Vector2Scale(n, side * 3.4f));
                if (fabsf(p.y - zc) > 9.5f) continue;
                float gy = terrain(p.x, p.y);
                // collision fills the shrub drawn below: its main clump (0.79 m radius, crown ~1.1 m up), not just the soil bed
                addPlat({p.x - 0.75f, p.x + 0.75f, p.y - 0.75f, p.y + 0.75f, gy + 1.1f, gy + 1.1f, gy - 0.3f, 0, PK_PROP});
                mb(p.x, p.y).box({p.x - 0.7f, gy - 0.3f, p.y - 0.7f}, {p.x + 0.7f, gy + 0.12f, p.y + 0.7f}, col(112, 84, 62), 0.85f); // soil bed
                addShrub(p.x, p.y, 1.05f, r, r.chance(0.6f));
                addFlowerBed(p.x - 0.55f, p.y - 0.55f, p.x + 0.55f, p.y + 0.55f, 4, r);
            }
    }
}

// ---------------------------------------------------------------- buildings
// Muted San Francisco house colors: cream, sage, slate blue, dusty rose, butter, terra cotta...
const Color PASTEL[12] = {col(228, 214, 182), col(176, 192, 160), col(132, 152, 176), col(214, 172, 168),
                          col(230, 208, 150), col(172, 196, 210), col(194, 134, 110), col(202, 188, 168),
                          col(168, 198, 184), col(182, 164, 178), col(206, 176, 108), col(238, 234, 224)};
static const Color MISSION_C[] = {col(222, 128, 98), col(228, 188, 96), col(100, 152, 180), col(170, 104, 136),
                                  col(132, 170, 122), col(224, 154, 86), col(106, 130, 176), col(226, 220, 204)};
static const Color STONE[] = {col(202, 192, 176), col(172, 162, 150), col(152, 92, 72), col(222, 216, 206),
                              col(122, 126, 136), col(92, 102, 116), col(190, 178, 150), col(140, 120, 100)};
static const Color GLASSC[] = {col(72, 98, 124), col(98, 122, 142), col(62, 82, 92), col(142, 162, 178), col(112, 132, 120)};

// Trim, door and roof colors for the Victorian/Edwardian fronts.
static const Color TRIMS[] = {col(242, 236, 220), col(248, 246, 240), col(236, 224, 190), col(226, 226, 218)};
static const Color DARK_TRIMS[] = {col(66, 78, 92), col(58, 84, 70), col(108, 56, 56), col(74, 62, 84), col(60, 60, 64)};
static const Color DOORS[] = {col(104, 36, 38), col(38, 66, 54), col(34, 48, 78), col(40, 36, 34), col(120, 76, 40), col(86, 40, 62)};
static const unsigned char HOUSE_MATS[4] = {M_HOUSE, M_HOUSE1, M_HOUSE2, M_HOUSE3};
constexpr float STOREY = 3.3f;    // matches the window rows in the M_HOUSE shader
constexpr float CELL_W = 2.6f;    // ... and its window column spacing

static float luma(Color c) { return 0.3f * c.r + 0.59f * c.g + 0.11f * c.b; }

// Boxes on the street-facing side of a lot. u = along the facade, o = outward distance.
static void facadeBox(MeshBuilder& m, float x0, float z0, float x1, float z1, int facing, float u0, float u1,
                      float y0, float y1, float o0, float o1, Color c) {
    Vector3 mn, mx;
    switch (facing) {
    case 0: mn = {u0, y0, z0 - o1}; mx = {u1, y1, z0 - o0}; break;
    case 1: mn = {u0, y0, z1 + o0}; mx = {u1, y1, z1 + o1}; break;
    case 2: mn = {x0 - o1, y0, u0}; mx = {x0 - o0, y1, u1}; break;
    default: mn = {x1 + o0, y0, u0}; mx = {x1 + o1, y1, u1}; break;
    }
    m.box(mn, mx, c, c, 1.f);
}

// The street side of a lot as a coordinate frame: (u along the facade, y, o outward) -> world.
struct Facade {
    MeshBuilder& m;
    float x0, z0, x1, z1;
    int facing;
    Vector3 P(float u, float y, float o) const {
        switch (facing) {
        case 0: return {u, y, z0 - o};
        case 1: return {u, y, z1 + o};
        case 2: return {x0 - o, y, u};
        default: return {x1 + o, y, u};
        }
    }
    void box(float u0, float u1, float y0, float y1, float o0, float o1, Color c) const {
        facadeBox(m, x0, z0, x1, z1, facing, u0, u1, y0, y1, o0, o1, c);
    }
    // flat panel just in front of the wall (2 triangles)
    void panel(float u0, float u1, float y0, float y1, float o, Color c) const {
        m.quad(P(u0, y0, o), P(u1, y0, o), P(u1, y1, o), P(u0, y1, o), c);
    }
    void tri(float ua, float ya, float ub, float yb, float uc, float yc, float o, Color c) const {
        m.tri(P(ua, ya, o), P(ub, yb, o), P(uc, yc, o), c);
    }
};

// Three-faced angled bay window (2 triangles per face) with a small hipped cap.
// c = center, hw = half width at the wall, fhw = half width of the front panel, D = projection.
// yLow..y0 is a solid base (skipped when hovering), y0..y1 the windowed part.
static void bayWindow(const Facade& F, float c, float hw, float fhw, float D, float yLow, float y0, float y1,
                      Color wall, Color base, Color trim, Color roof, bool hover) {
    const float pu[4] = {c - hw, c - fhw, c + fhw, c + hw}, po[4] = {0.f, D, D, 0.f};
    auto face = [&](int i, float ya, float yb, Color k) {
        F.m.quad(F.P(pu[i], ya, po[i]), F.P(pu[i + 1], ya, po[i + 1]), F.P(pu[i + 1], yb, po[i + 1]), F.P(pu[i], yb, po[i]), k);
    };
    if (hover) F.m.quad(F.P(pu[0], y0, 0), F.P(pu[1], y0, D), F.P(pu[2], y0, D), F.P(pu[3], y0, 0), base);
    else for (int i = 0; i < 3; i++) face(i, yLow, y0, base);
    // the front panel carries the windows; the angled cheeks are plain and a shade darker
    Color cheek = withMat(scaleC(wall, 0.9f), M_PLAIN);
    face(0, y0, y1, cheek);
    face(1, y0, y1, wall);
    face(2, y0, y1, cheek);
    // cornice ring and hipped cap, slightly wider than the bay
    const float cu[4] = {pu[0] - 0.08f, pu[1] - 0.1f, pu[2] + 0.1f, pu[3] + 0.08f}, co[4] = {0.f, D + 0.12f, D + 0.12f, 0.f};
    for (int i = 0; i < 3; i++)
        F.m.quad(F.P(cu[i], y1, co[i]), F.P(cu[i + 1], y1, co[i + 1]), F.P(cu[i + 1], y1 + 0.25f, co[i + 1]),
                 F.P(cu[i], y1 + 0.25f, co[i]), trim);
    Vector3 apex = F.P(c, y1 + 0.9f, D * 0.45f);
    for (int i = 0; i < 3; i++) F.m.tri(F.P(cu[i], y1 + 0.25f, co[i]), F.P(cu[i + 1], y1 + 0.25f, co[i + 1]), apex, roof);
}

constexpr float WALL_T = 0.35f;     // wall thickness of an enterable house
constexpr float CEIL_H = 3.6f;      // ground-floor ceiling above the entry level (clears an ollie)
constexpr float DOOR_HW = 0.6f;     // half width of the doorway
constexpr float OPEN_H = 2.65f;     // height of the doorway

// What the street front decided: whether the door is open to a real room, and where.
struct FrontInfo {
    bool enter = false;
    float cDoor = 0, gd = 0, H = 0;
    Color trim{}, doorC{};
    int ns = 0;
    float tread = 0.55f;
    // a front stair that runs along the facade (houses well above a steep street): ramp and landing rectangles
    bool side = false;
    float uA = 0, uL = 0, yStart = 0, lu0 = 0, lu1 = 0;
    float doorY = 0;   // floor level at the front door (below the entry floor for a garden-level door)
};

// A house body with a doorway cut through its street wall (the other three sides and the roof as a plain box).
static void shellMesh(MeshBuilder& m, float x0, float z0, float x1, float z1, float base, float top, int facing,
                      float cDoor, float floorY, Color side, Color roof, float ao, Color trim, Color floorC) {
    Vector3 p[8];
    for (int i = 0; i < 8; i++) p[i] = {(i & 1) ? x1 : x0, (i & 2) ? top : base, (i & 4) ? z1 : z0};
    Color sb = scaleC(side, ao);
    static const int faces[4][4] = {{0, 4, 6, 2}, {1, 5, 7, 3}, {0, 1, 3, 2}, {4, 5, 7, 6}};
    static const int frontFace[4] = {2, 3, 0, 1};
    for (int f = 0; f < 4; f++) {
        if (f == frontFace[facing]) continue;
        Color k[4];
        for (int i = 0; i < 4; i++) k[i] = (faces[f][i] & 2) ? side : sb;
        m.quadC(p[faces[f][0]], p[faces[f][1]], p[faces[f][2]], p[faces[f][3]], k[0], k[1], k[2], k[3]);
    }
    m.quad(p[2], p[3], p[7], p[6], roof);
    Facade F{m, x0, z0, x1, z1, facing};
    float fu0 = facing < 2 ? x0 : z0, fu1 = facing < 2 ? x1 : z1;
    float d0 = cDoor - DOOR_HW, d1 = cDoor + DOOR_HW, dTop = floorY + OPEN_H;
    m.quadC(F.P(fu0, base, 0), F.P(d0, base, 0), F.P(d0, top, 0), F.P(fu0, top, 0), sb, sb, side, side);
    m.quadC(F.P(d1, base, 0), F.P(fu1, base, 0), F.P(fu1, top, 0), F.P(d1, top, 0), sb, sb, side, side);
    m.quad(F.P(d0, dTop, 0), F.P(d1, dTop, 0), F.P(d1, top, 0), F.P(d0, top, 0), side);
    // reveals, header underside and sill
    m.quad(F.P(d0, floorY, 0), F.P(d0, floorY, -WALL_T), F.P(d0, dTop, -WALL_T), F.P(d0, dTop, 0), trim);
    m.quad(F.P(d1, floorY, 0), F.P(d1, floorY, -WALL_T), F.P(d1, dTop, -WALL_T), F.P(d1, dTop, 0), trim);
    m.quad(F.P(d0, dTop, 0), F.P(d1, dTop, 0), F.P(d1, dTop, -WALL_T), F.P(d0, dTop, -WALL_T), trim);
    m.quad(F.P(d0, floorY + 0.01f, 0), F.P(d1, floorY + 0.01f, 0), F.P(d1, floorY + 0.01f, -WALL_T), F.P(d0, floorY + 0.01f, -WALL_T), floorC);
}

// Victorian / Edwardian / Queen Anne street front: raised plinth, stoop and door, angled bay, bracketed cornice,
// front gable or pediment. Window rows come from the shader; bay, door and brackets are placed on its 2.6 m
// window columns so they line up with it. L0 is the entry floor line, n the number of storeys above it.
static void victorianFront(const World& w, MeshBuilder& m, float x0, float z0, float x1, float z1, int facing, int style,
                           float L0, int n, float top, Color side, Color roof, bool gable, RNG& hr, bool canEnter, FrontInfo& info) {
    Facade F{m, x0, z0, x1, z1, facing};
    float fu0 = facing < 2 ? x0 : z0, fu1 = facing < 2 ? x1 : z1, fw = fu1 - fu0;
    float depth = facing < 2 ? z1 - z0 : x1 - x0;
    auto ground = [&](float u) { Vector3 p = F.P(u, 0, 0); return w.terrain(p.x, p.z); };
    float gLow = std::min(ground(fu0), ground(fu1));
    bool sunset = style == BS_SUNSET, lady = style == BS_LADY;

    // paint scheme
    Color trim = TRIMS[hr.irange(0, 3)];
    if (hr.chance(lady ? 0.4f : 0.3f)) trim = DARK_TRIMS[hr.irange(0, 4)];
    if (fabsf(luma(trim) - luma(side)) < 40.f) trim = luma(side) > 150.f ? DARK_TRIMS[hr.irange(0, 4)] : TRIMS[1];
    Color doorC = DOORS[hr.irange(0, 5)];
    Color accent = lady ? withMat(mixC(doorC, trim, 0.15f), M_PLAIN) : trim;      // brackets, bay cap
    Color plinth = withMat(scaleC(mixC(side, col(118, 112, 104), 0.6f), 0.9f), M_PLAIN);
    Color shingle = withMat(jitterC(mixC(side, trim, 0.55f), hr, 0.05f), M_PLAIN);
    Color roofC = scaleC(roof, hr.range(0.85f, 1.2f));
    Color glassC = col(50, 66, 84);

    // layout: window columns (centers of the shader's cells) available on this facade
    float cs[8];
    int nc = 0;
    for (float c = (ceilf((fu0 + 0.75f) / CELL_W - 0.5f) + 0.5f) * CELL_W; c <= fu1 - 0.75f && nc < 8; c += CELL_W) cs[nc++] = c;
    bool doorFirst = hr.chance(0.5f);
    float cDoor = nc ? cs[doorFirst ? 0 : nc - 1] : (fu0 + fu1) * 0.5f;
    float cBay = nc ? cs[doorFirst ? nc - 1 : 0] : cDoor;
    if (nc >= 3 && hr.chance(0.35f)) cBay = cs[1];

    float top2 = L0 - 0.12f;                       // top of the raised plinth
    // plinth (raised basement) with a garage on Sunset houses
    if (!sunset) F.panel(fu0, fu1, gLow - 0.4f, top2, 0.05f, plinth);

    // bay window or turret
    float hw = std::min(1.6f, std::min(cBay - fu0 - 0.05f, fu1 - 0.05f - cBay));
    float fhw = std::min(0.78f, hw - 0.12f), D = 1.15f * (hw - fhw);
    int bayFloors = sunset ? 1 : std::min(2, n - 1);
    float by0 = sunset ? L0 + STOREY - 0.3f : top2;
    float by1 = sunset ? top - 0.35f : L0 + bayFloors * STOREY;
    bool turret = lady && hr.chance(0.35f) && fw > 9.f;
    if (turret) {
        // octagonal corner tower with a conical roof
        float tu = doorFirst ? fu1 - 1.45f : fu0 + 1.45f, tr = 1.3f;
        Vector3 b = F.P(tu, gLow - 0.4f, -0.35f);
        m.prism(b, 8, tr, tr, top + 1.2f - b.y, side, side, 0.39f, false, 1.f);
        m.prism({b.x, top + 1.2f, b.z}, 8, tr + 0.12f, tr + 0.12f, 0.25f, trim, trim, 0.39f, false, 1.f);
        m.prism({b.x, top + 1.45f, b.z}, 8, tr + 0.2f, 0.f, 3.4f, roofC, roofC, 0.39f, true, 1.f);
        cBay = tu;
    } else if (hw > 0.85f && by1 > by0 + 2.f) {
        bayWindow(F, cBay, hw, fhw, D, gLow - 0.4f, by0, by1, side, plinth, accent, roofC, sunset);
    }

    // entry: stoop, door, surround
    {   // a door too close to the lot edge has no room for its side walls: move it one window column inward
        float mid = (fu0 + fu1) * 0.5f;
        if (canEnter && nc >= 3 && (cDoor - fu0 < DOOR_HW + WALL_T + 0.15f || fu1 - cDoor < DOOR_HW + WALL_T + 0.15f)) {
            float alt = cDoor + (cDoor < mid ? CELL_W : -CELL_W);
            if (fabsf(alt - cBay) > 2.4f) cDoor = alt;
        }
    }
    if (canEnter && (cDoor - fu0 < DOOR_HW + WALL_T + 0.15f || fu1 - cDoor < DOOR_HW + WALL_T + 0.15f) && fw >= 6.5f) {
        // a narrow lot with no window column to move to: centre the door, if that clears the bay window
        float mid = (fu0 + fu1) * 0.5f;
        if (fabsf(mid - cBay) > 2.6f || turret) cDoor = mid;
    }
    float gd = ground(cDoor), H = L0 - gd;
    info.cDoor = cDoor; info.gd = gd; info.H = H; info.trim = trim; info.doorC = doorC;
    bool inMargin = cDoor - DOOR_HW - 0.12f > fu0 + WALL_T && cDoor + DOOR_HW + 0.12f < fu1 - WALL_T;
    if (canEnter && !inMargin) w_stat[0]++;
    else if (canEnter && H > 6.5f) w_stat[1]++;
    else if (canEnter && H > 2.3f && fw < 7.4f) w_stat[2]++;
    else if (canEnter && H > 2.3f) w_stat[3]++;
    info.enter = canEnter && H <= 2.3f && inMargin;
    // a flight of stairs running along the front of the house up to a landing at the door, for doors well above
    // the street; it runs toward the far end of the facade and must not be steeper than about 36 degrees
    auto trySide = [&]() {
        bool doorLow = cDoor < (fu0 + fu1) * 0.5f;
        float uA = doorLow ? fu1 - 0.5f : fu0 + 0.5f;
        float uL = doorLow ? cDoor + 1.15f : cDoor - 1.15f;
        Vector3 sP = F.P(uA, 0, 0.7f), sQ = F.P(uA, 0, 2.2f);
        float yS = std::min(L0 - 0.1f, std::min(w.terrain(sP.x, sP.z), w.terrain(sQ.x, sQ.z)) + 0.3f);
        float run = fabsf(uA - uL);
        if (run < 3.5f || (L0 - yS) / run > 0.72f) return false;
        info.side = true;
        info.uA = uA; info.uL = uL; info.yStart = yS;
        info.lu0 = std::max(fu0 + 0.3f, cDoor - 1.15f);
        info.lu1 = std::min(fu1 - 0.3f, cDoor + 1.15f);
        return true;
    };
    if (!info.enter && canEnter && inMargin && H > 2.3f && H <= 6.5f && fw >= 7.4f && trySide()) info.enter = true;
    // houses you can't go into still get believable steps: a stoop when the door is low, a flight along the
    // front when it is high, and when neither fits, a garden-level door in the raised basement instead
    float doorY = L0;
    const float STOOP_MAX = 2.6f;
    if (!info.enter && H > STOOP_MAX && (sunset || turret || !trySide())) {
        doorY = std::max(ground(cDoor - 0.7f), ground(cDoor + 0.7f)) + 0.17f;
        H = doorY - gd;
    }
    info.doorY = doorY;
    Color conc = col(188, 184, 176), concSide = col(158, 154, 146), rail = col(40, 44, 48);
    if (info.side) {
        // individual steps (~19 cm risers) whose front edges sit on the collision ramp's line; the last riser
        // steps up onto the landing
        float rise = L0 - info.yStart, run = info.uL - info.uA;
        int ns = std::clamp((int)ceilf(rise / 0.19f), 2, 40);
        for (int i = 0; i < ns; i++) {
            float ua = info.uA + run * i / ns, ub = info.uA + run * (i + 1) / ns;
            float yTop = info.yStart + rise * i / ns;
            Vector3 a = F.P(std::min(ua, ub), gLow - 1.5f, 0), b = F.P(std::max(ua, ub), yTop, 1.4f);
            m.box({std::min(a.x, b.x), a.y, std::min(a.z, b.z)}, {std::max(a.x, b.x), b.y, std::max(a.z, b.z)}, concSide, conc, 1.f);
        }
        F.box(info.lu0, info.lu1, gLow - 1.5f, L0, 0, 1.4f, concSide);
        F.panel(info.lu0, info.lu1, L0 - 0.001f, L0 + 0.001f, 0.7f, conc);
        // handrail along the open side of the flight and the landing, on posts
        m.beam(F.P(info.uA, info.yStart + 0.95f, 1.3f), F.P(info.uL, L0 + 0.95f, 1.3f), 0.06f, 0.06f, rail);
        m.beam(F.P(info.uL, L0 + 0.95f, 1.3f), F.P(info.uL > cDoor ? info.lu1 : info.lu0, L0 + 0.95f, 1.3f), 0.06f, 0.06f, rail);
        for (int k = 0; k <= 5; k++) {
            float u = lerpf(info.uA, info.uL, k / 5.f), yy = lerpf(info.yStart, L0, k / 5.f);
            m.beam(F.P(u, yy, 1.3f), F.P(u, yy + 0.95f, 1.3f), 0.06f, 0.06f, rail);
        }
    } else if (H > 0.1f) {
        // a stoop straight out to the sidewalk: risers of at most ~22 cm, no deeper than 3 m
        info.ns = std::max(1, (int)ceilf(H / 0.22f));
        info.tread = std::min(0.32f, 3.0f / info.ns);
        for (int i = 1; i <= info.ns; i++)
            F.box(cDoor - 0.9f, cDoor + 0.9f, gd - 0.4f, gd + H * i / info.ns, 0, info.tread * (info.ns - i + 1), col(176, 172, 164));
    }
    if (info.enter) {
        // an open doorway into a real room: a framed opening, the door swung open inside
        F.panel(cDoor - DOOR_HW - 0.18f, cDoor - DOOR_HW, L0, L0 + OPEN_H + 0.35f, 0.07f, trim);
        F.panel(cDoor + DOOR_HW, cDoor + DOOR_HW + 0.18f, L0, L0 + OPEN_H + 0.35f, 0.07f, trim);
        F.panel(cDoor - DOOR_HW - 0.18f, cDoor + DOOR_HW + 0.18f, L0 + OPEN_H, L0 + OPEN_H + 0.35f, 0.07f, trim);
        F.panel(cDoor - DOOR_HW - 0.1f, cDoor + DOOR_HW + 0.1f, L0 + OPEN_H + 0.05f, L0 + OPEN_H + 0.3f, 0.09f, glassC); // transom
    } else {
        // a garden-level door is a little shorter when the main floor sits just above it
        float dh = doorY < L0 ? std::clamp(L0 - 0.6f - doorY, 2.1f, 2.5f) : 2.5f;
        F.panel(cDoor - 0.62f, cDoor + 0.62f, doorY, doorY + dh + 0.45f, 0.07f, trim);
        F.panel(cDoor - 0.46f, cDoor + 0.46f, doorY, doorY + dh, 0.1f, doorC);
        F.panel(cDoor - 0.46f, cDoor + 0.46f, doorY + dh + 0.08f, doorY + dh + 0.35f, 0.1f, glassC);
    }

    if (sunset) {
        // Edwardian garage under the second-floor bay
        float gc = nc ? (cDoor == cs[0] ? cs[nc - 1] : cs[0]) : fu0 + fw * 0.3f;
        float gw = std::min(1.25f, std::min(gc - fu0 - 0.05f, fu1 - 0.05f - gc));
        float gg = std::min(ground(gc - gw), ground(gc + gw)) - 0.3f;
        Color gdoor = hr.chance(0.5f) ? col(226, 222, 212) : col(150, 118, 86);
        F.panel(gc - gw, gc + gw, gg, L0 + 2.4f, 0.08f, gdoor);
        F.panel(gc - gw, gc + gw, L0 + 2.4f, L0 + 3.05f, 0.08f, withMat(scaleC(side, 1.0f), M_PLAIN));
        for (int k = 1; k <= 2; k++) {
            float yy = gg + (L0 + 2.4f - gg) * k / 3.f;
            F.panel(gc - gw, gc + gw, yy - 0.03f, yy + 0.03f, 0.1f, scaleC(gdoor, 0.6f));
        }
    }

    // corner boards between neighbours
    F.panel(fu0, fu0 + 0.2f, gLow - 0.4f, top, 0.05f, trim);
    F.panel(fu1 - 0.2f, fu1, gLow - 0.4f, top, 0.05f, trim);

    // cornice with paired brackets between the windows
    if (sunset) {
        F.box(fu0, fu1, top - 0.35f, top + 0.25f, 0, 0.3f, trim);
    } else {
        F.box(fu0, fu1, top - 0.4f, top + 0.2f, 0, 0.55f, trim);
        for (int i = 0; i < nc; i++)
            for (int s = -1; s <= 1; s += 2) {
                float bu = cs[i] + s * 0.85f;
                if (bu < fu0 + 0.3f || bu > fu1 - 0.3f) continue;
                F.m.quad(F.P(bu - 0.13f, top - 0.4f, 0.1f), F.P(bu + 0.13f, top - 0.4f, 0.1f),
                         F.P(bu + 0.05f, top - 1.0f, 0.1f), F.P(bu - 0.05f, top - 1.0f, 0.1f), trim);
            }
    }

    // roofline: front gable (Queen Anne / Stick) with bargeboards and attic window, or a pediment
    float um = (fu0 + fu1) * 0.5f;
    if (gable) {
        float rise = fw * (lady ? 0.4f : 0.3f);
        Color wall = lady ? shingle : withMat(side, M_PLAIN);
        m.gable(x0, x1, z0, z1, top, rise, facing < 2 ? 1 : 0, roofC, wall);
        for (int s = -1; s <= 1; s += 2) {
            float ue = s < 0 ? fu0 : fu1;
            F.m.quad(F.P(ue, top + 0.1f, 0.04f), F.P(um, top + rise, 0.04f), F.P(um, top + rise - 0.45f, 0.04f),
                     F.P(ue, top - 0.35f, 0.04f), trim);
        }
        float ay0 = top + rise * 0.2f, ay1 = top + rise * 0.62f;
        F.panel(um - 0.5f, um + 0.5f, ay0 - 0.1f, ay1 + 0.1f, 0.04f, trim);
        F.panel(um - 0.38f, um + 0.38f, ay0, ay1, 0.06f, glassC);
    } else if (!sunset && hr.chance(0.5f)) {
        F.tri(um - 1.4f, top + 0.2f, um + 1.4f, top + 0.2f, um, top + 1.15f, 0.06f, trim);
        F.tri(um - 0.85f, top + 0.3f, um + 0.85f, top + 0.3f, um, top + 0.85f, 0.08f, scaleC(side, 0.9f));
    } else if (hr.chance(0.35f) && depth > 8.f) {
        // brick chimney
        float cu = fu0 + fw * hr.range(0.25f, 0.75f);
        F.box(cu - 0.45f, cu + 0.45f, top - 0.2f, top + 1.9f, -depth * 0.45f - 0.45f, -depth * 0.45f + 0.45f, col(146, 84, 68));
    }
}

// ---------------------------------------------------------------- signage text
// A 5x7 block font drawn as flat quads (runs of lit pixels merged), so signs carry real lettering.
static const char GLYPH_CHARS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789&-'";
static const uint8_t GLYPHS[][7] = {
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E},
    {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10},
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E},
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F},
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}, {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11},
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E},
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}, {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11},
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F},
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F},
    {0x1E, 0x01, 0x01, 0x0E, 0x01, 0x01, 0x1E}, {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E},
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E},
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, {0x0C, 0x12, 0x14, 0x08, 0x15, 0x12, 0x0D}, {0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00},
    {0x04, 0x04, 0x08, 0x00, 0x00, 0x00, 0x00},
};

static const uint8_t* glyphOf(char c) {
    for (int i = 0; GLYPH_CHARS[i]; i++) if (GLYPH_CHARS[i] == c) return GLYPHS[i];
    return nullptr;
}

// Lettering as quads. pt(s, t) maps (distance along the reading direction, height above the baseline) to the world.
template <class PT>
static void textQuads(MeshBuilder& m, const char* str, float px, Color c, PT pt) {
    int n = (int)strlen(str), cols = n * 6 - 1;
    for (int row = 0; row < 7; row++) {
        float t0 = (6 - row) * px, t1 = t0 + px;
        int col0 = 0;
        while (col0 < cols) {
            auto lit = [&](int cc) {
                if (cc >= cols || cc % 6 == 5) return false;
                const uint8_t* g = glyphOf(str[cc / 6]);
                return g && ((g[row] >> (4 - cc % 6)) & 1);
            };
            if (!lit(col0)) { col0++; continue; }
            int e = col0;
            while (lit(e + 1)) e++;
            m.quad(pt(col0 * px, t0), pt((e + 1) * px, t0), pt((e + 1) * px, t1), pt(col0 * px, t1), c);
            col0 = e + 1;
        }
    }
}

// Lettering on a facade: centered at u, baseline at y, o out from the wall; shrinks to fit maxW.
static float facadeText(const Facade& F, const char* str, float u, float y, float o, float height, float maxW, Color c) {
    int n = (int)strlen(str);
    if (n == 0) return 0;
    float px = height / 7.f, wid = (n * 6 - 1) * px;
    if (wid > maxW) { px = maxW / (n * 6 - 1); wid = maxW; }
    float dir = (F.facing == 1 || F.facing == 2) ? 1.f : -1.f;   // so it reads left to right from the street
    float start = u - dir * wid * 0.5f;
    textQuads(F.m, str, px, c, [&](float s, float t) { return F.P(start + dir * s, y + t, o); });
    return px * 7.f;
}

// Ground-floor shops along one street side of a building: bays of lit display windows with mullions, doors, signs
// with lettering, striped awnings and blade signs, each bay a different business.
static void shopfront(World& w, MeshBuilder& m, float x0, float z0, float x1, float z1, int facing) {
    Facade F{m, x0, z0, x1, z1, facing};
    float u0 = facing < 2 ? x0 : z0, u1 = facing < 2 ? x1 : z1, len = u1 - u0;
    if (len < 8.f) return;
    RNG sr(hashu((int)(x0 * 8), (int)(z0 * 8), 77 + facing));
    static const Color ACC[] = {col(160, 40, 44), col(36, 92, 64), col(30, 56, 110), col(206, 150, 44), col(96, 34, 70), col(24, 24, 28), col(210, 90, 40), col(40, 120, 130)};
    static const Color GLOW[] = {col(240, 206, 150, M_EMISSIVE), col(236, 226, 190, M_EMISSIVE), col(250, 196, 140, M_EMISSIVE), col(210, 226, 236, M_EMISSIVE)};
    static const Color LETTER[] = {col(246, 240, 224), col(236, 200, 80), col(250, 250, 250), col(210, 230, 210)};
    Color stone = col(176, 168, 156), dark = col(36, 38, 44);
    int nb = std::max(1, (int)(len / sr.range(5.5f, 8.5f)));
    // sidewalk height along the front, lowest and highest under a stretch of it
    auto groundSpan = [&](float a, float b, float& lo, float& hi) {
        lo = 1e9f; hi = -1e9f;
        int n = std::max(2, (int)((b - a) / 0.75f));
        for (int q = 0; q <= n; q++) {
            Vector3 p = F.P(lerpf(a, b, q / (float)n), 0, 0.6f);
            float g = w.terrain(p.x, p.z);
            lo = std::min(lo, g); hi = std::max(hi, g);
        }
    };
    // on a steep street, narrower shops, so no bay climbs more than about a metre from end to end
    while (len / (nb + 1) >= 3.4f) {
        float worst = 0;
        for (int i = 0; i < nb; i++) { float lo, hi; groundSpan(u0 + i * len / nb, u0 + (i + 1) * len / nb, lo, hi); worst = std::max(worst, hi - lo); }
        if (worst <= 1.0f) break;
        nb++;
    }
    float bw = len / nb;
    for (int i = 0; i < nb; i++) {
        float a = u0 + i * bw, b = a + bw;
        // each bay is level with the sidewalk at its uphill end, so a shop row steps down a hill like real ones do:
        // the sidewalk never climbs over the windows, and the base and piers reach down to the low end
        float gLo, gHi;
        groundSpan(a, b, gLo, gHi);
        float y0 = gHi - 0.05f, yLow = gLo - 1.6f;
        Vector3 pa = F.P(a, 0, 0.6f), pb = F.P(b, 0, 0.6f);
        float ga = w.terrain(pa.x, pa.z), gb = w.terrain(pb.x, pb.z);
        Color acc = ACC[sr.irange(0, 7)], glow = GLOW[sr.irange(0, 3)], let = LETTER[sr.irange(0, 3)];
        bool bank = acc.r == 24 && acc.g == 24;   // the dark ones are banks and bars: tinted glass, no awning
        F.box(a, a + 0.45f, yLow, y0 + 4.5f, 0, 0.2f, stone);                       // pier between shops
        F.box(a + 0.45f, b, yLow, y0 + 0.65f, 0, 0.14f, scaleC(acc, 0.7f));          // bulkhead under the window
        // door at one end of the bay, the display window across the rest
        bool doorLeft = sr.chance(0.5f);
        if (fabsf(ga - gb) > 0.25f) doorLeft = ga > gb;   // on a slope the door is at the top, where it meets the sidewalk
        float dw = 1.5f, du0 = doorLeft ? a + 0.7f : b - 0.7f - dw, du1 = du0 + dw;
        float wa = doorLeft ? du1 + 0.2f : a + 0.65f, wb = doorLeft ? b - 0.1f : du0 - 0.2f;
        F.box(wa, wb, y0 + 0.65f, y0 + 3.0f, 0, 0.06f, glow);                      // lit interior seen through the glass
        for (float u = wa + 1.3f; u < wb - 0.5f; u += 1.4f) F.box(u, u + 0.07f, y0 + 0.65f, y0 + 3.0f, 0, 0.12f, dark); // mullions
        F.box(wa, wb, y0 + 2.95f, y0 + 3.05f, 0, 0.12f, dark);
        // goods on show
        for (int k = 0; k < 3 && wb - wa > 2.4f; k++) {
            float gu = lerpf(wa + 0.4f, wb - 0.8f, sr.f());
            Color gc = ACC[sr.irange(0, 7)];
            F.box(gu, gu + 0.45f, y0 + 0.65f, y0 + 0.65f + sr.range(0.5f, 1.2f), 0.07f, 0.4f, gc);
        }
        F.box(du0, du1, y0, y0 + 2.4f, 0, 0.16f, dark);                            // door frame
        if (gHi - gLo > 0.2f) F.box(du0 - 0.1f, du1 + 0.1f, yLow, y0, 0, 0.35f, stone);   // stone sill under it, down to the falling sidewalk
        F.box(du0 + 0.15f, du1 - 0.15f, y0 + 0.3f, y0 + 2.3f, 0.16f, 0.2f, glow);  // glazed door
        // sign board with lettering
        F.box(a + 0.45f, b, y0 + 3.2f, y0 + 4.15f, 0, 0.3f, acc);
        {   // the shop's name, lettered on the sign board
            static const char* NOUN[] = {"CAFE", "BAKERY", "DELI", "PIZZA", "BOOKS", "BARBER", "FLOWERS", "PHARMACY", "SUSHI", "TACOS", "WINE BAR", "GALLERY",
                                         "RECORDS", "TAILOR", "NOODLES", "GRILL", "BOUTIQUE", "COFFEE", "GELATO", "HARDWARE", "OPTICAL", "TEA HOUSE", "DINER", "MARKET"};
            static const char* PRE[] = {"GOLDEN", "MISSION", "FOG CITY", "BAY", "SUNSET", "LUCKY", "CABLE CAR", "NOB HILL", "PACIFIC", "COIT", "HAIGHT",
                                        "NORTH BEACH", "MARINA", "DOLORES", "CASTRO", "REDWOOD", "ALAMO", "SOMA"};
            static const char* BANKS[] = {"FIRST BAY BANK", "PACIFIC SAVINGS", "THE TIPSY OWL", "IRON HORSE BAR", "GOLD COAST BANK", "THE DRAFT HOUSE"};
            char name[40];
            if (bank) snprintf(name, sizeof name, "%s", BANKS[sr.irange(0, 5)]);
            else if (sr.chance(0.55f)) snprintf(name, sizeof name, "%s %s", PRE[sr.irange(0, 17)], NOUN[sr.irange(0, 23)]);
            else snprintf(name, sizeof name, "%s", NOUN[sr.irange(0, 23)]);
            if (strlen(name) > 18) { char* sp = strrchr(name, ' '); if (sp) snprintf(name, sizeof name, "%s", sp + 1); }
            Color lc = let; lc.a = M_EMISSIVE;
            facadeText(F, name, (a + b) * 0.5f + 0.2f, y0 + 3.4f, 0.34f, 0.6f, b - a - 1.2f, lc);
            // little neon OPEN sign in the window
            if (sr.chance(0.45f) && wb - wa > 1.8f) {
                Color oc = sr.chance(0.5f) ? col(255, 70, 60, M_EMISSIVE) : col(80, 220, 255, M_EMISSIVE);
                facadeText(F, "OPEN", (wa + wb) * 0.5f, y0 + 2.25f, 0.1f, 0.26f, 1.4f, oc);
            }
        }
        // awning
        if (!bank && sr.chance(0.6f)) {
            Color c1 = acc, c2 = sr.chance(0.5f) ? col(240, 236, 226) : scaleC(acc, 1.5f);
            float yT = y0 + 3.15f, yB = y0 + 2.55f, oT = 0.1f, oB = 1.7f;
            int ns = std::max(2, (int)((b - a - 0.5f) / 0.55f));
            float sw = (b - a - 0.5f) / ns;
            for (int k = 0; k < ns; k++) {
                float ua = a + 0.25f + k * sw, ub = ua + sw;
                m.quad(F.P(ua, yT, oT), F.P(ub, yT, oT), F.P(ub, yB, oB), F.P(ua, yB, oB), (k & 1) ? c2 : c1);
                F.box(ua, ub, yB - 0.22f, yB, oB - 0.02f, oB, (k & 1) ? c2 : c1); // valance
            }
        }
        // blade sign on the pier
        if (sr.chance(0.35f)) {
            F.box(a + 0.1f, a + 0.16f, y0 + 3.5f, y0 + 4.3f, 0.2f, 0.9f, dark);
            F.box(a + 0.08f, a + 0.18f, y0 + 2.8f, y0 + 3.5f, 0.55f, 1.15f, acc);
        }
    }
}

void World::addBuilding(float x0, float z0, float x1, float z1, float height, Color side, Color roof, int style,
                        int facing, RNG& r) {
    float h00 = terrain(x0, z0), h10 = terrain(x1, z0), h01 = terrain(x0, z1), h11 = terrain(x1, z1);
    float hmin = std::min(std::min(h00, h10), std::min(h01, h11));
    float hmax = std::max(std::max(h00, h10), std::max(h01, h11));
    float base = hmin - 1.5f, top = hmax + height;
    bool rowhouse = style == BS_ROWHOUSE || style == BS_MISSION || style == BS_OAK || style == BS_LADY || style == BS_SUNSET;
    float L0 = 0;
    int nFloors = 0;
    if (rowhouse && facing >= 0) {
        // Snap the roofline to whole storeys above an entry floor a little over street level, and pick the
        // window-row phase so the shader's floor lines coincide with it.
        float gMid;
        switch (facing) {
        case 0: gMid = (h00 + h10) * 0.5f; break;
        case 1: gMid = (h01 + h11) * 0.5f; break;
        case 2: gMid = (h00 + h01) * 0.5f; break;
        default: gMid = (h10 + h11) * 0.5f; break;
        }
        float minE = style == BS_SUNSET ? 0.1f : style == BS_OAK ? 0.6f : 1.0f;
        int phase = 0;
        L0 = 1e9f;
        for (int p = 0; p < 4; p++) {
            float o = 0.825f * p, y = o + STOREY * ceilf((gMid + minE - o) / STOREY);
            if (y < L0) { L0 = y; phase = p; }
        }
        {   // hillside houses sit higher on their lots, like real ones with a tall flight of steps: lift the floor line
            // (in window-row quarter steps) until it clears the ground under the whole footprint
            float depthH = facing < 2 ? z1 - z0 : x1 - x0, widthH = facing < 2 ? x1 - x0 : z1 - z0;
            if (depthH >= 9.f && widthH >= 6.5f) {
                float tmax = -1e9f;
                for (int a = 0; a <= 4; a++)
                    for (int b = 0; b <= 4; b++)
                        tmax = std::max(tmax, terrain(lerpf(x0 + WALL_T, x1 - WALL_T, a / 4.f), lerpf(z0 + WALL_T, z1 - WALL_T, b / 4.f)));
                for (int guard = 0; tmax > L0 - 0.1f && guard < 8; guard++) { L0 += 0.825f; phase = (phase + 1) & 3; }
            }
        }
        nFloors = std::clamp((int)roundf((hmax + height - L0) / STOREY), 2, style == BS_SUNSET ? 3 : 4);
        if (L0 + nFloors * STOREY < hmax + 4.f) nFloors = (int)ceilf((hmax + 4.f - L0) / STOREY);
        top = L0 + nFloors * STOREY;
        if (side.a == M_HOUSE) side.a = HOUSE_MATS[phase];
    }
    MeshBuilder& m = mb((x0 + x1) / 2, (z0 + z1) / 2);
    FrontInfo fi;
    bool victorian = rowhouse && facing >= 0;
    bool gable = false;
    RNG hr(hashu((int)(x0 * 8), (int)(z0 * 8), 31));
    if (victorian) {
        // the global RNG draws stay the same as before so the rest of the city keeps its layout;
        // per-house variety comes from a generator seeded by the lot position
        gable = style == BS_LADY || (style != BS_SUNSET && r.chance(0.3f));
        // a house is enterable if its floor sits above the ground under the whole footprint
        float depthH = facing < 2 ? z1 - z0 : x1 - x0, widthH = facing < 2 ? x1 - x0 : z1 - z0;
        bool canEnter = depthH >= 9.f && widthH >= 6.5f;
        for (int a = 0; a <= 4 && canEnter; a++)
            for (int b = 0; b <= 4 && canEnter; b++)
                if (terrain(lerpf(x0 + WALL_T, x1 - WALL_T, a / 4.f), lerpf(z0 + WALL_T, z1 - WALL_T, b / 4.f)) > L0 - 0.1f) canEnter = false;
        if (!canEnter) houseTooSteep++;
        victorianFront(*this, m, x0, z0, x1, z1, facing, style, L0, nFloors, top, side, roof, gable, hr, canEnter, fi);
        rowhouseCount++;
        if (canEnter && !fi.enter) houseTooHigh++;
    }
    // collision for the front steps and a clear way to them (no trees, lamps or parked cars in front)
    auto frontStairs = [&]() {
        Facade F{m, x0, z0, x1, z1, facing};
        Vector3 po = F.P(fi.cDoor, 0, fi.side ? 0.8f : fi.ns * fi.tread + 0.7f), pi = F.P(fi.cDoor, 0, -1.6f), pf = F.P(fi.uA, 0, 0.7f);
        Vector2 doorOut = {po.x, po.z}, doorIn = {pi.x, pi.z}, foot = {pf.x, pf.z};
        // remember each step box, so sidewalk walkers can go round the front of it instead of through it
        Vector2 toStreet = facing == 0 ? Vector2{0, -1} : facing == 1 ? Vector2{0, 1} : facing == 2 ? Vector2{-1, 0} : Vector2{1, 0};
        // (in every cell its reach touches, the box grown by STOOP_REACH, so a lookup needs only the walker's cell)
        auto addStoop = [&](float sx0, float sx1, float sz0, float sz1) {
            for (int ci = (int)floorf((sx0 - STOOP_REACH) / 16.f); ci <= (int)floorf((sx1 + STOOP_REACH) / 16.f); ci++)
                for (int cj = (int)floorf((sz0 - STOOP_REACH) / 16.f); cj <= (int)floorf((sz1 + STOOP_REACH) / 16.f); cj++)
                    stoops.emplace(ci * 4096 + cj, Stoop{sx0, sx1, sz0, sz1, toStreet});
        };
        if (fi.side) {   // collision for the flight along the front and its landing
            float ulo = std::min(fi.uA, fi.uL), uhi = std::max(fi.uA, fi.uL);
            Vector3 a = F.P(ulo, 0, 0), b = F.P(uhi, 0, 1.4f);
            float h0 = fi.uA < fi.uL ? fi.yStart : fi.doorY, h1 = fi.uA < fi.uL ? fi.doorY : fi.yStart;
            addPlat({std::min(a.x, b.x), std::max(a.x, b.x), std::min(a.z, b.z), std::max(a.z, b.z), h0, h1, fi.yStart - 2.f, (uint8_t)(facing < 2 ? 0 : 1), PK_PROP});
            addStoop(std::min(a.x, b.x), std::max(a.x, b.x), std::min(a.z, b.z), std::max(a.z, b.z));
            Vector3 la = F.P(fi.lu0, 0, 0), lb = F.P(fi.lu1, 0, 1.4f);
            addPlat({std::min(la.x, lb.x), std::max(la.x, lb.x), std::min(la.z, lb.z), std::max(la.z, lb.z), fi.doorY, fi.doorY, fi.gd - 2.f, 0, PK_PROP});
            addStoop(std::min(la.x, lb.x), std::max(la.x, lb.x), std::min(la.z, lb.z), std::max(la.z, lb.z));
        } else {   // collision for the front steps: a flat top step, then a smooth ramp down to the street so you roll up
            // instead of bouncing off each tread (it follows the front edges of the drawn steps)
            float W2 = fi.ns * fi.tread;
            // the far end of the ramp meets the street wherever the street actually is there, a short step up from it
            Vector3 farP = F.P(fi.cDoor, 0, W2 + 0.4f), farQ = F.P(fi.cDoor, 0, W2);
            float yFar = std::min(fi.doorY - 0.05f, std::min(terrain(farP.x, farP.z), terrain(farQ.x, farQ.z)) + 0.36f);
            auto stepPlat = [&](float dep0, float dep1, float yNear, float yFar2, bool slope) {
                Vector3 a = F.P(fi.cDoor - 0.9f, 0, dep0), b = F.P(fi.cDoor + 0.9f, 0, dep1);
                float x0s = std::min(a.x, b.x), x1s = std::max(a.x, b.x), z0s = std::min(a.z, b.z), z1s = std::max(a.z, b.z);
                float base2 = fi.gd - 0.4f;
                addStoop(x0s, x1s, z0s, z1s);
                switch (facing) { // wall side is the near end
                case 0: addPlat({x0s, x1s, z0s, z1s, yFar2, yNear, base2, 1, PK_PROP}); break;
                case 1: addPlat({x0s, x1s, z0s, z1s, yNear, yFar2, base2, 1, PK_PROP}); break;
                case 2: addPlat({x0s, x1s, z0s, z1s, yFar2, yNear, base2, 0, PK_PROP}); break;
                default: addPlat({x0s, x1s, z0s, z1s, yNear, yFar2, base2, 0, PK_PROP}); break;
                }
                (void)slope;
            };
            if (fi.ns >= 1) stepPlat(0, std::min(fi.tread, W2), fi.doorY, fi.doorY, false);
            if (fi.ns >= 2) stepPlat(fi.tread, W2, fi.doorY, yFar, true);
        }
        if (fi.ns >= 1 || fi.side) {   // keep trees, lamps and parked cars off the way to the door
            Vector2 out = Vector2Normalize({doorOut.x - doorIn.x, doorOut.y - doorIn.y});
            auto addSeg = [&](Vector2 p0, Vector2 p1) {
                Vector4 seg = {p0.x, p0.y, p1.x, p1.y};
                int n = std::max(2, (int)(Vector2Distance(p0, p1) / 4.f) + 1);
                for (int k = 0; k <= n; k++) {
                    float px = lerpf(seg.x, seg.z, (float)k / n), pz = lerpf(seg.y, seg.w, (float)k / n);
                    doorWays.emplace((int)floorf(px / 16.f) * 4096 + (int)floorf(pz / 16.f), seg);
                }
            };
            Vector2 from = fi.side ? foot : doorOut;
            addSeg({from.x - out.x * 3.6f, from.y - out.y * 3.6f}, {from.x + out.x * 8.f, from.y + out.y * 8.f}); // steps and the way to them
            if (fi.side) addSeg(foot, doorOut); // along the flight to the landing
        }
    };
    if (fi.enter) {
        shellMesh(m, x0, z0, x1, z1, base, top, facing, fi.cDoor, L0, side, roof, 0.72f, fi.trim, col(150, 108, 70, M_INTERIOR));
        House h{x0, z0, x1, z1, L0, facing, {}, {}};
        Facade F{m, x0, z0, x1, z1, facing};
        Vector3 po = F.P(fi.cDoor, 0, fi.side ? 0.8f : fi.ns * fi.tread + 0.7f), pi = F.P(fi.cDoor, 0, -1.6f);
        h.doorOut = {po.x, po.z};
        h.doorIn = {pi.x, pi.z};
        h.sideStair = fi.side;
        if (fi.side) {
            Vector3 pre = F.P(fi.uA, 0, 3.4f), foot = F.P(fi.uA, 0, 0.7f);
            h.route[0] = {pre.x, pre.z};
            h.route[1] = {foot.x, foot.z};
            h.route[2] = h.doorOut;
            h.route[3] = h.doorIn;
            h.nRoute = 4;
        } else {
            h.route[0] = h.doorOut;
            h.route[1] = h.doorIn;
            h.nRoute = 2;
        }
        interiorSrc.push_back({h, base, top, fi.cDoor, hr});   // the room's mesh is built later, near the camera
        addHouseInterior(h, base, top, fi.cDoor, hr, nullptr);
        frontStairs();
        // open door leaf swung back against the wall inside
        Vector3 la = F.P(fi.cDoor - DOOR_HW + 0.03f, L0, -WALL_T - 0.02f), lb = F.P(fi.cDoor - DOOR_HW + 0.03f, L0 + 2.5f, -WALL_T - 1.3f);
        m.quad(la, F.P(fi.cDoor - DOOR_HW + 0.03f, L0, -WALL_T - 1.3f), lb, F.P(fi.cDoor - DOOR_HW + 0.03f, L0 + 2.5f, -WALL_T - 0.02f),
               withMat(fi.doorC, M_INTERIOR));
        houses.push_back(h);
    } else {
        m.box({x0, base, z0}, {x1, top, z1}, side, roof, 0.72f);
        addPlat({x0, x1, z0, z1, top, top, base, 0, PK_BUILDING});
        if (victorian) frontStairs();
    }
    if (facing < 0) return;

    // facade coordinates
    float fu0 = (facing < 2) ? x0 : z0, fu1 = (facing < 2) ? x1 : z1;
    float streetY;
    switch (facing) {
    case 0: streetY = std::min(h00, h10); break;
    case 1: streetY = std::min(h01, h11); break;
    case 2: streetY = std::min(h00, h01); break;
    default: streetY = std::min(h10, h11); break;
    }
    if (rowhouse) {
        // (the street front was built above, before the body)
    } else if (style == BS_MIDRISE || style == BS_WHARF || style == BS_OAK_DOWNTOWN) {
        static const Color AWN[] = {col(160, 40, 40), col(40, 90, 60), col(30, 50, 100), col(200, 150, 40), col(90, 30, 60)};
        shopfront(*this, m, x0, z0, x1, z1, facing);
        (void)AWN; (void)streetY;
        {   // billboards on the roof and tall neon signs up the front, from a lot-seeded generator so the city layout is untouched
            Facade F{m, x0, z0, x1, z1, facing};
            RNG br(hashu((int)(x0 * 8), (int)(z0 * 8), 91 + facing));
            float len = fu1 - fu0;
            struct Ad { const char *brand, *tag; Color bg, fg; };
            static const Ad ADS[] = {
                {"ZOOMCO", "FASTER EVERY DAY", col(230, 60, 50), col(255, 255, 255)}, {"FOGBREW", "COFFEE FOR THE FOG", col(40, 90, 70), col(250, 240, 210)},
                {"BAYWAVE", "STREAM THE BAY", col(30, 80, 170), col(255, 255, 255)}, {"SKYBANK", "BANK ON THE SKY", col(20, 40, 90), col(255, 215, 90)},
                {"GOLDEN FIZZ", "TASTE THE SUN", col(240, 170, 30), col(120, 30, 20)}, {"HAPPY TACO", "TACOS TACOS TACOS", col(210, 60, 120), col(255, 245, 200)},
                {"SUNSET MOTORS", "DRIVE THE COAST", col(30, 30, 36), col(250, 120, 60)}, {"TECHNOVA", "BUILD WHATS NEXT", col(90, 50, 190), col(210, 240, 255)},
                {"ORCA AIR", "FLY THE PACIFIC", col(20, 130, 150), col(255, 255, 255)}, {"CABLE FIT", "MOVE YOUR CITY", col(235, 235, 230), col(200, 40, 40)}};
            if (len >= 15.f && br.chance(style == BS_MIDRISE ? 0.4f : 0.2f)) {
                const Ad& ad = ADS[br.irange(0, 9)];
                float w2 = std::min(14.f, len - 5.f), h2 = 5.5f, yb = top + 2.4f, u = fu0 + len * br.range(0.35f, 0.65f);
                float ua = u - w2 * 0.5f, ub = u + w2 * 0.5f;
                Color metal = col(70, 74, 80);
                F.box(ua + 1.0f, ua + 1.4f, top, yb, -1.5f, -1.1f, metal);          // legs
                F.box(ub - 1.4f, ub - 1.0f, top, yb, -1.5f, -1.1f, metal);
                F.box(ua, ub, yb, yb + h2, -1.3f, -0.95f, metal);                   // backing
                Color bg = ad.bg; bg.a = M_EMISSIVE;
                F.panel(ua + 0.2f, ub - 0.2f, yb + 0.2f, yb + h2 - 0.2f, -0.9f, bg);
                Color fg = ad.fg; fg.a = M_EMISSIVE;
                float tw = w2 - 1.6f;
                facadeText(F, ad.brand, u, yb + h2 * 0.5f, -0.88f, 1.7f, tw, fg);
                facadeText(F, ad.tag, u, yb + 0.7f, -0.88f, 0.6f, tw, fg);
                Color bar = scaleC(ad.fg, 0.9f); bar.a = M_EMISSIVE;
                F.panel(ua + 0.5f, ub - 0.5f, yb + h2 - 0.75f, yb + h2 - 0.55f, -0.88f, bar);
            }
            // tall neon blade signs on the front of the building
            static const char* BLADE[] = {"HOTEL", "BAR", "PIZZA", "CAFE", "INN", "DELI", "BOOKS", "SUSHI"};
            for (float u = fu0 + 7.f; u < fu1 - 5.f; u += 17.f) {
                if (!br.chance(0.4f)) continue;
                const char* word = BLADE[br.irange(0, 7)];
                int n = (int)strlen(word);
                Vector3 g = F.P(u, 0, 0.6f);
                float gy = terrain(g.x, g.z), px = 0.13f, adv = 8 * px, h3 = n * adv + 0.4f, yTop = gy + 6.2f + h3;
                if (yTop + 1.f > top) continue;
                F.box(u - 0.55f, u + 0.55f, yTop - h3, yTop, 0, 0.45f, col(34, 36, 42));
                Color nc = br.chance(0.5f) ? col(255, 80, 80, M_EMISSIVE) : col(90, 230, 255, M_EMISSIVE);
                for (int i = 0; i < n; i++) {
                    char ch[2] = {word[i], 0};
                    facadeText(F, ch, u, yTop - 0.2f - (i + 1) * adv + px, 0.48f, 7 * px, 0.9f, nc);
                }
            }
        }
        facadeBox(m, x0, z0, x1, z1, facing, fu0, fu1, top - 0.8f, top, 0, 0.5f, scaleC(side, 0.9f));
    }
    // rooftop clutter: water tanks and AC units
    if ((style == BS_MIDRISE || style == BS_OAK_DOWNTOWN) && r.chance(0.5f)) {
        float cx = lerpf(x0, x1, r.range(0.3f, 0.7f)), cz = lerpf(z0, z1, r.range(0.3f, 0.7f));
        if (r.chance(0.5f)) {
            m.prism({cx, top, cz}, 8, 1.8f, 1.8f, 3.2f, col(120, 90, 60), col(90, 70, 50));
            m.prism({cx, top + 3.2f, cz}, 8, 1.9f, 0.2f, 1.0f, col(90, 70, 50), col(90, 70, 50));
        } else
            m.box({cx - 2, top, cz - 1.5f}, {cx + 2, top + 1.8f, cz + 1.5f}, col(150, 150, 150));
    }
}

// The ground floor of an enterable house: wall linings, floor, ceiling, a hall partition, furniture and the
// collision for all of it. The door is at facade coordinate doorU; "depth" runs from the street wall inward.
// With geo null only the collision is kept (the mesh goes to a scratch builder); otherwise only the mesh, into geo.
void World::addHouseInterior(const House& h, float base, float top, float doorU, RNG& hr, MeshBuilder* geo) {
    static MeshBuilder scratch;
    scratch.v.clear(); scratch.c.clear(); scratch.n.clear();
    MeshBuilder& m = geo ? *geo : scratch;
    auto plat = [&](const Platform& p) { if (!geo) this->addPlat(p); };
    Facade F{m, h.x0, h.z0, h.x1, h.z1, h.facing};
    const float t = WALL_T, Y = h.floorY;
    float fu0 = h.facing < 2 ? h.x0 : h.z0, fu1 = h.facing < 2 ? h.x1 : h.z1;
    float D = h.facing < 2 ? h.z1 - h.z0 : h.x1 - h.x0;
    float iu0 = fu0 + t, iu1 = fu1 - t, d0 = t, d1 = D - t;
    auto I = [&](float u, float y, float d) { return F.P(u, y, -d); };
    auto aabb = [&](float u0, float u1, float dd0, float dd1, float y0, float y1, Color c, bool solid, uint8_t kind = PK_PROP) {
        Vector3 a = I(u0, y0, dd0), b = I(u1, y1, dd1);
        Vector3 mn = {std::min(a.x, b.x), y0, std::min(a.z, b.z)}, mx = {std::max(a.x, b.x), y1, std::max(a.z, b.z)};
        m.box(mn, mx, c.a == M_EMISSIVE ? c : withMat(c, M_INTERIOR), 1.f);
        if (solid) plat({mn.x, mx.x, mn.z, mx.z, y1, y1, y0, 0, kind});
    };

    // palette
    static const Color PLASTER[] = {col(236, 224, 200), col(224, 214, 190), col(214, 226, 214), col(226, 214, 218), col(232, 222, 176)};
    static const Color WOOD[] = {col(150, 108, 70), col(126, 90, 60), col(168, 130, 88)};
    Color wall = PLASTER[hr.irange(0, 4)], floorWood = WOOD[hr.irange(0, 2)], ceil = col(244, 240, 230);
    Color wood = scaleC(col(112, 74, 48), hr.range(0.8f, 1.15f)); // furniture: walnut, darker than the floor
    Color W_ = withMat(wall, M_INTERIOR), baseb = withMat(scaleC(wall, 0.6f), M_INTERIOR);
    float Yc = Y + CEIL_H;

    // shell collision: back and side walls full height, front wall in two pieces with the door gap between,
    // a header over the doorway, and the upper storeys sitting on a ceiling above the room
    float du0 = doorU - DOOR_HW, du1 = doorU + DOOR_HW;
    auto wallPlat = [&](float u0, float u1, float dd0, float dd1) {
        Vector3 a = I(u0, 0, dd0), b = I(u1, 0, dd1);
        plat({std::min(a.x, b.x), std::max(a.x, b.x), std::min(a.z, b.z), std::max(a.z, b.z), top, top, base, 0, PK_BUILDING});
    };
    wallPlat(fu0, fu1, D - t, D);            // back
    wallPlat(fu0, fu0 + t, 0, D);            // sides
    wallPlat(fu1 - t, fu1, 0, D);
    wallPlat(fu0, du0, 0, t);                // front, either side of the door
    wallPlat(du1, fu1, 0, t);
    {   // header above the doorway, then everything above the ground floor
        Vector3 a = I(du0, 0, 0), b = I(du1, 0, t);
        plat({std::min(a.x, b.x), std::max(a.x, b.x), std::min(a.z, b.z), std::max(a.z, b.z), top, top, Y + OPEN_H, 0, PK_BUILDING});
        a = I(fu0, 0, 0); b = I(fu1, 0, D);
        plat({std::min(a.x, b.x), std::max(a.x, b.x), std::min(a.z, b.z), std::max(a.z, b.z), top, top, Yc, 0, PK_BUILDING});
        plat({std::min(a.x, b.x), std::max(a.x, b.x), std::min(a.z, b.z), std::max(a.z, b.z), Y, Y, Y - 0.4f, 0, PK_DECK}); // floor
    }

    // surfaces: floor, ceiling, wall linings with a skirting board
    Color floorC = withMat(floorWood, M_INTERIOR);
    m.quad(I(iu0, Y + 0.01f, d0), I(iu1, Y + 0.01f, d0), I(iu1, Y + 0.01f, d1), I(iu0, Y + 0.01f, d1), floorC);
    for (float u = iu0 + 0.9f; u < iu1 - 0.3f; u += 0.9f) // floorboards
        m.quad(I(u - 0.015f, Y + 0.012f, d0), I(u + 0.015f, Y + 0.012f, d0), I(u + 0.015f, Y + 0.012f, d1), I(u - 0.015f, Y + 0.012f, d1), scaleC(floorC, 0.8f));
    m.quad(I(iu0, Yc - 0.01f, d0), I(iu1, Yc - 0.01f, d0), I(iu1, Yc - 0.01f, d1), I(iu0, Yc - 0.01f, d1), withMat(ceil, M_INTERIOR));
    auto lining = [&](Vector3 a, Vector3 b) { // a, b = floor-level endpoints of a wall run
        m.quad(a, b, {b.x, Yc, b.z}, {a.x, Yc, a.z}, W_);
        m.quad({a.x, Y + 0.0f, a.z}, {b.x, Y + 0.0f, b.z}, {b.x, Y + 0.14f, b.z}, {a.x, Y + 0.14f, a.z}, baseb);
    };
    lining(I(iu0, Y, d1), I(iu1, Y, d1));                       // back
    lining(I(iu0, Y, d0), I(iu0, Y, d1));                       // sides
    lining(I(iu1, Y, d0), I(iu1, Y, d1));
    lining(I(iu0, Y, d0), I(du0, Y, d0));                       // front, around the door
    lining(I(du1, Y, d0), I(iu1, Y, d0));
    m.quad(I(du0, Y + OPEN_H, d0), I(du1, Y + OPEN_H, d0), I(du1, Yc, d0), I(du0, Yc, d0), W_);

    // windows seen from inside, on the window columns of the front and back walls
    Color pane = withMat(col(176, 208, 232), M_EMISSIVE), frame = withMat(col(246, 242, 232), M_INTERIOR);
    for (float c = (ceilf((fu0 + 0.75f) / CELL_W - 0.5f) + 0.5f) * CELL_W; c <= fu1 - 0.75f; c += CELL_W)
        for (int wallK = 0; wallK < 2; wallK++) {
            if (wallK == 0 && fabsf(c - doorU) < 1.6f) continue;
            float dd = wallK == 0 ? d0 + 0.015f : d1 - 0.015f;
            m.quad(I(c - 0.5f, Y + 0.75f, dd), I(c + 0.5f, Y + 0.75f, dd), I(c + 0.5f, Y + 2.65f, dd), I(c - 0.5f, Y + 2.65f, dd), frame);
            m.quad(I(c - 0.42f, Y + 0.83f, dd * 0.995f), I(c + 0.42f, Y + 0.83f, dd * 0.995f), I(c + 0.42f, Y + 2.57f, dd * 0.995f), I(c - 0.42f, Y + 2.57f, dd * 0.995f), pane);
        }

    // hall partition between the front room and the back room, with a doorway on the street-door side
    bool doorLeft = doorU < (fu0 + fu1) * 0.5f;
    float dp = std::clamp(D * 0.42f, 5.5f, 9.f);
    float gapC = doorLeft ? iu0 + 1.5f : iu1 - 1.5f;
    float g0 = gapC - 0.7f, g1 = gapC + 0.7f;
    if (iu1 - iu0 > 5.f) {
        auto part = [&](float u0, float u1) {
            if (u1 - u0 < 0.1f) return;
            aabb(u0, u1, dp, dp + 0.2f, Y, Yc, wall, true, PK_BUILDING);
        };
        part(iu0, g0);
        part(g1, iu1);
        aabb(g0, g1, dp, dp + 0.2f, Y + 2.35f, Yc, wall, true, PK_BUILDING); // header over the doorway
    }

    // furniture, kept clear of the entry hall and of the walkway to the back room
    float farU0 = doorLeft ? iu1 - 2.4f : iu0, farU1 = doorLeft ? iu1 : iu0 + 2.4f;     // side away from the door
    float nearU0 = doorLeft ? iu0 : iu1 - 2.4f;
    static const Color FAB[] = {col(120, 62, 58), col(58, 86, 110), col(96, 110, 78), col(150, 120, 80)};
    Color sofaC = FAB[hr.irange(0, 3)];
    float sofaD = d0 + 3.2f;
    aabb(farU0, farU1, sofaD, sofaD + 2.2f, Y, Y + 0.45f, sofaC, true);                              // sofa seat
    aabb(doorLeft ? farU1 - 0.45f : farU0, doorLeft ? farU1 : farU0 + 0.45f, sofaD, sofaD + 2.2f, Y + 0.45f, Y + 0.95f, scaleC(sofaC, 0.9f), true);  // back
    aabb((farU0 + farU1) * 0.5f - 0.5f, (farU0 + farU1) * 0.5f + 0.5f, sofaD + 3.0f, sofaD + 3.0f + 0.9f, Y, Y + 0.04f, col(176, 70, 60), false); // rug
    aabb((farU0 + farU1) * 0.5f - 0.45f, (farU0 + farU1) * 0.5f + 0.45f, sofaD + 0.2f, sofaD + 1.6f, Y, Y + 0.42f, wood, true);          // coffee table
    aabb(nearU0, nearU0 + 0.4f, dp - 2.4f, dp - 0.4f, Y, Y + 2.1f, scaleC(wood, 0.8f), true);                                         // bookshelf by the partition
    if (doorLeft) aabb(iu1 - 0.35f, iu1 - 0.12f, sofaD + 2.5f, sofaD + 2.75f, Y, Y + 1.6f, col(60, 60, 64), true);                     // floor lamp stem
    aabb((farU0 + farU1) * 0.5f - 0.2f, (farU0 + farU1) * 0.5f + 0.2f, d0 + 1.0f, d0 + 1.4f, Y + CEIL_H - 0.1f, Y + CEIL_H - 0.02f, withMat(col(255, 236, 190), M_EMISSIVE), false); // ceiling light
    // back room: counter along the back wall, table and chairs
    aabb(iu0, iu0 + std::min(3.2f, iu1 - iu0 - 1.f), d1 - 0.65f, d1, Y, Y + 0.92f, col(190, 184, 170), true);
    aabb(iu0, iu0 + std::min(3.2f, iu1 - iu0 - 1.f), d1 - 0.65f, d1, Y + 0.92f, Y + 0.97f, col(70, 70, 76), false);
    float tu = (iu0 + iu1) * 0.5f + (doorLeft ? 0.6f : -0.6f), td = (dp + 0.2f + d1) * 0.5f;
    aabb(tu - 0.7f, tu + 0.7f, td - 0.45f, td + 0.45f, Y, Y + 0.76f, wood, true);
    for (int k = 0; k < 4; k++) {
        float cu = tu + (k % 2 ? 0.95f : -0.95f), cd = td + (k < 2 ? 0.0f : 0.0f);
        if (k >= 2) { cu = tu + (k == 2 ? -0.35f : 0.35f); cd = td + 0.85f; }
        aabb(cu - 0.2f, cu + 0.2f, cd - 0.2f, cd + 0.2f, Y, Y + 0.45f, scaleC(wood, 0.85f), true);
    }
    aabb(tu - 0.2f, tu + 0.2f, td - 0.2f, td + 0.2f, Y + 0.76f, Y + 0.95f, withMat(col(255, 226, 150), M_EMISSIVE), false); // lamp on the table
    aabb(tu - 0.25f, tu + 0.25f, td - 0.25f, td + 0.25f, Y + CEIL_H - 0.1f, Y + CEIL_H - 0.02f, withMat(col(255, 236, 190), M_EMISSIVE), false);
    // ---- the details that make it a lived-in room
    Color trimW = withMat(col(246, 242, 232), M_INTERIOR);
    auto stripe = [&](Vector3 a, Vector3 b, float y0, float y1, Color k) { m.quad({a.x, y0, a.z}, {b.x, y0, b.z}, {b.x, y1, b.z}, {a.x, y1, a.z}, k); };
    // wainscot panels with a chair rail, and crown moulding, along every wall
    Color panel = withMat(scaleC(wall, 0.84f), M_INTERIOR);
    auto wallDress = [&](Vector3 a, Vector3 b) {
        stripe(a, b, Y + 0.14f, Y + 1.0f, panel);
        stripe(a, b, Y + 1.0f, Y + 1.08f, trimW);
        stripe(a, b, Yc - 0.22f, Yc, trimW);
    };
    wallDress(I(iu0 + 0.01f, Y, d1 - 0.01f), I(iu1 - 0.01f, Y, d1 - 0.01f));
    wallDress(I(iu0 + 0.01f, Y, d0 + 0.01f), I(iu0 + 0.01f, Y, d1 - 0.01f));
    wallDress(I(iu1 - 0.01f, Y, d0 + 0.01f), I(iu1 - 0.01f, Y, d1 - 0.01f));
    wallDress(I(iu0 + 0.01f, Y, d0 + 0.01f), I(du0, Y, d0 + 0.01f));
    wallDress(I(du1, Y, d0 + 0.01f), I(iu1 - 0.01f, Y, d0 + 0.01f));
    // curtains at the windows
    Color curt = withMat(FAB[hr.irange(0, 3)], M_INTERIOR);
    for (float cc = (ceilf((fu0 + 0.75f) / CELL_W - 0.5f) + 0.5f) * CELL_W; cc <= fu1 - 0.75f; cc += CELL_W)
        for (int wallK = 0; wallK < 2; wallK++) {
            if (wallK == 0 && fabsf(cc - doorU) < 1.6f) continue;
            float dd = wallK == 0 ? d0 + 0.04f : d1 - 0.04f;
            for (int sd = -1; sd <= 1; sd += 2)
                m.quad(I(cc + sd * 0.62f - 0.13f, Y + 0.6f, dd), I(cc + sd * 0.62f + 0.13f, Y + 0.6f, dd), I(cc + sd * 0.62f + 0.13f, Y + 2.9f, dd), I(cc + sd * 0.62f - 0.13f, Y + 2.9f, dd), curt);
        }
    // fireplace on the side wall of the front room, with a mantle and a glow
    {
        float fu0b = doorLeft ? iu1 - 0.55f : iu0, fu1b = doorLeft ? iu1 : iu0 + 0.55f;
        float fd = d0 + 6.4f;
        aabb(fu0b, fu1b, fd, fd + 1.7f, Y, Y + 1.2f, col(150, 144, 138), true);            // stone surround
        aabb(fu0b - (doorLeft ? 0.08f : -0.08f), doorLeft ? fu1b : fu1b + 0.08f, fd - 0.1f, fd + 1.8f, Y + 1.2f, Y + 1.28f, wood, false); // mantle
        Vector3 g0 = I(doorLeft ? iu1 - 0.56f : iu0 + 0.56f, Y + 0.12f, fd + 0.45f), g1 = I(doorLeft ? iu1 - 0.56f : iu0 + 0.56f, Y + 0.12f, fd + 1.25f);
        m.quad(g0, g1, {g1.x, Y + 0.75f, g1.z}, {g0.x, Y + 0.75f, g0.z}, withMat(col(255, 150, 60), M_EMISSIVE)); // firelight
    }
    // television on a low cabinet against the partition, between its doorway and the sofa so it never stands
    // in the way to the back room; left out where that stretch of wall is too short for it
    {
        float span0 = doorLeft ? g1 : farU1, span1 = doorLeft ? farU0 : g0;
        float tc = (span0 + span1) * 0.5f;
        if (iu1 - iu0 > 6.f && span1 - span0 >= 1.7f) {
            aabb(tc - 0.8f, tc + 0.8f, dp - 0.55f, dp - 0.05f, Y, Y + 0.5f, wood, true);
            aabb(tc - 0.6f, tc + 0.6f, dp - 0.12f, dp - 0.05f, Y + 0.55f, Y + 1.1f, col(20, 22, 26), true);
            aabb(tc - 0.54f, tc + 0.54f, dp - 0.14f, dp - 0.12f, Y + 0.6f, Y + 1.05f, withMat(col(110, 150, 220), M_EMISSIVE), false); // screen
        }
    }
    // books on the shelf by the partition
    for (int k = 0; k < 7; k++) {
        float bd = dp - 2.35f + k * 0.26f, bh = 0.35f + 0.1f * (k % 3);
        for (int row = 0; row < 4; row++)
            aabb(nearU0 + 0.4f, nearU0 + 0.46f, bd, bd + 0.2f, Y + 0.3f + row * 0.48f, Y + 0.3f + row * 0.48f + bh, FAB[(k + row) % 4], false);
    }
    // kitchen: fridge and stove beside the counter, and a potted plant by the sofa
    {
        float ku = iu0 + std::min(3.2f, iu1 - iu0 - 1.f);
        aabb(ku + 0.1f, ku + 0.85f, d1 - 0.8f, d1, Y, Y + 1.85f, col(214, 218, 222), true);                  // fridge
        aabb(ku + 0.85f, ku + 1.55f, d1 - 0.7f, d1, Y, Y + 0.92f, col(60, 62, 68), true);                    // stove
        aabb(ku + 0.9f, ku + 1.5f, d1 - 0.65f, d1 - 0.1f, Y + 0.92f, Y + 0.96f, withMat(col(255, 120, 50), M_EMISSIVE), false); // burners
        aabb(farU0 + 0.2f, farU0 + 0.7f, sofaD - 1.0f, sofaD - 0.5f, Y, Y + 0.4f, col(150, 84, 60), false);   // pot
        m.prism(I(farU0 + 0.45f, Y + 0.4f, sofaD - 0.75f), 6, 0.32f, 0.12f, 0.9f, withMat(col(64, 128, 70), M_INTERIOR), withMat(col(64, 128, 70), M_INTERIOR), 0.f, true, 1.f);
        Vector3 pa = I(farU0 + 0.2f, 0, sofaD - 1.0f), pb = I(farU0 + 0.7f, 0, sofaD - 0.5f); // pot and plant: one column
        plat({std::min(pa.x, pb.x), std::max(pa.x, pb.x), std::min(pa.z, pb.z), std::max(pa.z, pb.z), Y + 1.3f, Y + 1.3f, Y, 0, PK_PROP});
    }
    // pictures on the side walls
    for (int k = 0; k < 3; k++) {
        Color pc = withMat(FAB[(k + 1) % 4], M_INTERIOR);
        float dd = d0 + 2.0f + k * 3.1f;
        m.quad(I(iu0 + 0.02f, Y + 1.5f, dd), I(iu0 + 0.02f, Y + 1.5f, dd + 1.0f), I(iu0 + 0.02f, Y + 2.3f, dd + 1.0f), I(iu0 + 0.02f, Y + 2.3f, dd), pc);
    }
}

static bool lotOK(const World& w, float x0, float z0, float x1, float z1) {
    for (int k = 0; k < 9; k++) {
        float x = lerpf(x0, x1, (k % 3) * 0.5f), z = lerpf(z0, z1, (k / 3) * 0.5f);
        float rs;
        if (z > 800 && w.railDistance(x, z, &rs) < 16) return false; // Caltrain right of way
        if (w.terrain(x, z) < 1.2f) return false;
        if (w.zoneFlags(x, z, true) & Z_NOBUILD) return false;
        if (distToSeg2D(x, z, MK_AX, MK_AZ, MK_BX, MK_BZ) < MK_HALF + 3.f) return false;
    }
    return w.inPlay(x0, z0) && w.inPlay(x1, z1);
}

void World::buildBlocks(const Grid& g, bool oakland) {
    RNG r(oakland ? 777 : 1234);
    float inset = oakland ? 10.5f : BLOCK_INSET;
    for (int j = 0; j + 1 < g.nj; j++)
        for (int i = 0; i + 1 < g.ni; i++) {
            Vector2 a = g.node(i, j), b = g.node(i + 1, j + 1);
            // need at least one street touching this block
            bool road = g.edge(i, j, 0) || g.edge(i, j, 2) || g.edge(i + 1, j, 2) || g.edge(i, j + 1, 0);
            if (!road) continue;
            float bx0 = a.x + inset, bz0 = a.y + inset, bx1 = b.x - inset, bz1 = b.y - inset;
            float cx = (bx0 + bx1) / 2, cz = (bz0 + bz1) / 2;
            int style = buildStyleAt(cx, cz);
            if (oakland && style != BS_OAK_DOWNTOWN) style = BS_OAK;

            if (style == BS_TOWERS || style == BS_OAK_DOWNTOWN) {
                int nx = r.irange(1, 3), nz = r.irange(1, 3);
                float lw = (bx1 - bx0) / nx, lz = (bz1 - bz0) / nz;
                for (int u = 0; u < nx; u++)
                    for (int v = 0; v < nz; v++) {
                        float x0 = bx0 + u * lw + 1.5f, x1 = bx0 + (u + 1) * lw - 1.5f;
                        float z0 = bz0 + v * lz + 1.5f, z1 = bz0 + (v + 1) * lz - 1.5f;
                        if (!lotOK(*this, x0, z0, x1, z1)) continue;
                        float maxH;
                        if (style == BS_OAK_DOWNTOWN) maxH = 30 + 70 * expf(-(sq(cx - 4380) + sq(cz + 60)) / sq(200));
                        else maxH = 45 + 140 * expf(-(sq(cx - 1050) + sq(cz + 200)) / sq(420));
                        float H = maxH * r.range(0.35f, 1.0f) * (nx * nz > 4 ? 0.7f : 1.0f);
                        addTower(x0, z0, x1, z1, H, r);
                        MeshBuilder& tm = mb((x0 + x1) / 2, (z0 + z1) / 2);
                        if (v == 0) shopfront(*this, tm, x0, z0, x1, z1, 0);
                        if (v == nz - 1) shopfront(*this, tm, x0, z0, x1, z1, 1);
                        if (u == 0) shopfront(*this, tm, x0, z0, x1, z1, 2);
                        if (u == nx - 1) shopfront(*this, tm, x0, z0, x1, z1, 3);
                    }
                continue;
            }

            // perimeter lots around the block
            float wMin = 7, wMax = 10.5f, dMin = 18, dMax = 22, hMin = 9, hMax = 14;
            const Color* pal = PASTEL;
            int npal = 12;
            switch (style) {
            case BS_MIDRISE: wMin = 14; wMax = 26; dMin = 26; dMax = 36; hMin = 14; hMax = 40; pal = STONE; npal = 8; break;
            case BS_WHARF: wMin = 14; wMax = 28; dMin = 24; dMax = 30; hMin = 6; hMax = 11; pal = STONE; npal = 8; break;
            case BS_SUNSET: wMin = 7.5f; wMax = 10; dMin = 17; dMax = 19; hMin = 6.5f; hMax = 9; break;
            case BS_MISSION: pal = MISSION_C; npal = 8; hMin = 8; hMax = 13; break;
            case BS_OAK: wMin = 9; wMax = 16; dMin = 16; dMax = 22; hMin = 6; hMax = 11; break;
            default: break;
            }
            float depth = r.range(dMin, dMax);
            for (int side = 0; side < 4; side++) {
                float s0, s1;
                if (side < 2) { s0 = bx0; s1 = bx1; }
                else { s0 = bz0 + depth; s1 = bz1 - depth; }
                float s = s0;
                while (s < s1 - 3) {
                    float w = r.range(wMin, wMax);
                    if (s1 - (s + w) < wMin * 0.7f) w = s1 - s;
                    float x0, x1, z0, z1;
                    switch (side) {
                    case 0: x0 = s; x1 = s + w; z0 = bz0; z1 = bz0 + depth; break;
                    case 1: x0 = s; x1 = s + w; z0 = bz1 - depth; z1 = bz1; break;
                    case 2: x0 = bx0; x1 = bx0 + depth; z0 = s; z1 = s + w; break;
                    default: x0 = bx1 - depth; x1 = bx1; z0 = s; z1 = s + w; break;
                    }
                    s += w;
                    if (!lotOK(*this, x0, z0, x1, z1)) continue;
                    Color c = pal[r.irange(0, npal - 1)];
                    c = jitterC(c, r, 0.05f);
                    c.a = (style == BS_MIDRISE && r.chance(0.5f)) ? M_OFFICE : M_HOUSE;
                    Color roof = (style == BS_MIDRISE || style == BS_WHARF) ? col(120, 118, 115) : col(92, 88, 86);
                    float h = r.range(hMin, hMax);
                    addBuilding(x0 + 0.05f, z0 + 0.05f, x1 - 0.05f, z1 - 0.05f, h, c, roof, style, side, r);
                }
            }
            // backyard trees
            if (style != BS_MIDRISE && style != BS_WHARF) {
                int nt = r.irange(0, 3);
                for (int t = 0; t < nt; t++) {
                    float x = r.range(bx0 + depth + 4, bx1 - depth - 4), z = r.range(bz0 + depth + 4, bz1 - depth - 4);
                    if (lotOK(*this, x - 1, z - 1, x + 1, z + 1)) addTree(x, z, r.chance(0.3f) ? 1 : 0, r.range(0.8f, 1.2f), r, false);
                }
                for (int t = 0; t < 2; t++) { // garden: bushes and a flower bed
                    float x = r.range(bx0 + depth + 3, bx1 - depth - 3), z = r.range(bz0 + depth + 3, bz1 - depth - 3);
                    if (!lotOK(*this, x - 1.5f, z - 1.5f, x + 1.5f, z + 1.5f)) continue;
                    if (r.chance(0.5f)) addShrub(x, z, r.range(0.9f, 1.4f), r, r.chance(0.5f));
                    else addFlowerBed(x - 1.2f, z - 0.8f, x + 1.2f, z + 0.8f, 14, r);
                }
            }
        }
}

// ---------------------------------------------------------------- trees, lamps
float World::treesNear(float x, float z) const {
    int cx = (int)floorf(x / 25.f), cz = (int)floorf(z / 25.f);
    float n = 0;
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            auto it = treeCells.find((cz + dz) * 100000 + cx + dx);
            if (it != treeCells.end()) n += it->second * (dx == 0 && dz == 0 ? 1.f : 0.5f);
        }
    return n;
}

void World::addTree(float x, float z, int type, float s, RNG& r, bool solid) {
    if (solid && doorBlocked(x, z, 2.6f)) return; // never plant a trunk in front of a front door
    float y = terrain(x, z);
    if (y < 0.8f) return;
    if (buildingAt(x, z, 0.5f * s, y - 0.2f, y + 5.f)) return;   // not inside a building or landmark base
    treeCells[(int)floorf(z / 25.f) * 100000 + (int)floorf(x / 25.f)]++;
    MeshBuilder& m = mb(x, z);
    Color bark = jitterC(col(96, 72, 52), r, 0.15f);
    Color leaf = jitterC(type == 1 ? col(56, 104, 62) : type == 3 ? col(112, 134, 96) : col(88, 150, 62), r, 0.1f);
    float rot = r.range(0, TAU_F);
    switch (type) {
    case 0: { // broadleaf street tree: a trunk forking into limbs, each carrying a clump of leaves
        float th = 2.6f * s;
        m.limb({x, y - 0.2f, z}, {x, y + th, z}, 0.2f * s, 0.11f * s, bark);
        for (int k = 0; k < 4; k++) {
            float a = rot + k * TAU_F / 4 + r.range(-0.4f, 0.4f), reach = (k == 3 ? 0.f : 1.5f) * s;
            Vector3 from = {x, y + th * r.range(0.75f, 1.f), z};
            Vector3 c = {x + cosf(a) * reach, y + (k == 3 ? 4.7f : r.range(3.5f, 4.2f)) * s, z + sinf(a) * reach};
            m.limb(from, c, 0.09f * s, 0.04f * s, bark, 5);
            m.leafCloud(c, {1.5f * s, 1.15f * s, 1.5f * s}, 24, 0.85f * s, 0.5f * s, leaf, r, k == 3);
        }
        break;
    }
    case 1: { // cypress / pine: dark, layered tiers of drooping branchlets
        float h = 8.0f * s;
        m.limb({x, y - 0.2f, z}, {x + r.range(-0.1f, 0.1f), y + h, z}, 0.25f * s, 0.04f * s, bark);
        for (int k = 0; k < 6; k++) {
            float t = (k + 0.5f) / 6, rr = (1.9f - 1.5f * t) * s;
            m.leafCloud({x, y + (1.2f + 6.2f * t) * s, z}, {rr, 0.75f * s, rr}, 22, 0.75f * s, 0.2f * s,
                        scaleC(leaf, 0.95f + 0.2f * t), r, k == 2);
        }
        break;
    }
    case 2: { // palm: ringed trunk leaning slightly, a crown of arching fronds and a cluster of dates
        float h = 8.5f * s;
        Color tr = jitterC(col(134, 108, 80), r, 0.08f);
        Vector3 prev = {x, y - 0.2f, z};
        for (int k = 1; k <= 5; k++) {   // trunk in sections, thick at the foot, with a dark ring at each joint
            float t = k / 5.f;
            Vector3 q = {x + 0.3f * t * t * s, y + h * t, z};
            m.limb(prev, q, (0.26f - 0.08f * (t - 0.2f)) * s, (0.26f - 0.08f * t) * s, (k & 1) ? tr : scaleC(tr, 0.86f), 7);
            prev = q;
        }
        Vector3 t = prev;
        for (int k = 0; k < 11; k++) {
            float a = TAU_F * k / 11 + rot;
            m.frond({t.x, t.y - 0.1f, t.z}, {cosf(a), 0, sinf(a)}, r.range(3.4f, 4.4f) * s, r.range(0.6f, 1.3f), jitterC(col(66, 124, 48), r, 0.12f), r);
        }
        for (int k = 0; k < 6; k++) m.sphere({t.x + r.range(-0.3f, 0.3f) * s, t.y - 0.5f - 0.05f * k, t.z + r.range(-0.3f, 0.3f) * s}, 0.16f * s, 3, 4, col(176, 110, 40));
        break;
    }
    case 4: { // valley oak: short thick trunk, broad spreading canopy of several clumps on forked limbs
        Color wood = jitterC(col(84, 70, 58), r, 0.1f);
        Color oak = jitterC(col(80, 124, 54), r, 0.1f);
        m.limb({x, y - 0.2f, z}, {x + 0.3f, y + 3.0f * s, z}, 0.55f * s, 0.3f * s, wood, 7);
        for (int k = 0; k < 5; k++) {
            float a = rot + k * TAU_F / 5 + r.range(-0.3f, 0.3f), rr = r.range(2.8f, 4.4f) * s;
            Vector3 elbow = {x + cosf(a) * rr * 0.45f, y + (3.6f + r.range(0, 0.8f)) * s, z + sinf(a) * rr * 0.45f};
            Vector3 c = {x + cosf(a) * rr, y + (4.9f + r.range(0, 1.2f)) * s, z + sinf(a) * rr};
            m.limb({x + 0.3f, y + 2.8f * s, z}, elbow, 0.2f * s, 0.12f * s, wood, 5);
            m.limb(elbow, c, 0.12f * s, 0.05f * s, wood, 5);
            m.leafCloud(c, {2.5f * s, 1.6f * s, 2.5f * s}, 28, 0.95f * s, 0.55f * s, scaleC(oak, r.range(0.9f, 1.1f)), r, k == 0);
        }
        m.leafCloud({x, y + 6.2f * s, z}, {2.6f * s, 1.4f * s, 2.6f * s}, 24, 0.95f * s, 0.55f * s, oak, r, false);
        if (solid) addPlat({x - 0.45f * s, x + 0.45f * s, z - 0.45f * s, z + 0.45f * s, y + 5, y + 5, y - 1, 0, PK_POLE});
        return;
    }
    case 5: { // coast redwood: very tall, fluted trunk and tiers of short drooping boughs
        Color red = jitterC(col(122, 64, 46), r, 0.1f);
        Color nd = jitterC(col(38, 76, 48), r, 0.1f);
        m.limb({x, y - 0.2f, z}, {x, y + 24 * s, z}, 0.6f * s, 0.06f * s, red, 8);
        for (int k = 0; k < 8; k++) {
            float t = (k + 0.5f) / 8, rr = (3.0f - 2.4f * t) * s;
            m.leafCloud({x, y + (5.5f + 17.f * t) * s, z}, {rr, 1.1f * s, rr}, 22, 0.9f * s, 0.2f * s, scaleC(nd, 0.9f + 0.25f * t), r, k == 3);
        }
        if (solid) addPlat({x - 0.5f * s, x + 0.5f * s, z - 0.5f * s, z + 0.5f * s, y + 5, y + 5, y - 1, 0, PK_POLE});
        return;
    }
    default: { // monterey cypress / eucalyptus: tall, leaning trunk and a wide, open, lumpy crown of narrow leaves
        float h = 6.0f * s;
        Vector3 top = {x + r.range(-0.4f, 0.4f) * s, y + h, z + r.range(-0.4f, 0.4f) * s};
        m.limb({x, y - 0.2f, z}, top, 0.3f * s, 0.14f * s, bark);
        for (int k = 0; k < 5; k++) {
            float a = rot + k * TAU_F / 5, rr = (k == 4 ? 0.f : 2.4f) * s;
            Vector3 c = {top.x + cosf(a) * rr, top.y + (k == 4 ? 2.2f : r.range(0.4f, 1.4f)) * s, top.z + sinf(a) * rr};
            m.limb({top.x, top.y - 0.8f * s, top.z}, c, 0.1f * s, 0.04f * s, bark, 5);
            m.leafCloud(c, {2.2f * s, 1.4f * s, 2.2f * s}, 24, 1.1f * s, 0.26f * s, leaf, r, k == 4);
        }
        break;
    }
    }
    if (solid) addPlat({x - 0.3f * s, x + 0.3f * s, z - 0.3f * s, z + 0.3f * s, y + 5, y + 5, y - 1, 0, PK_POLE});
}

// Low bush: a few overlapping clumps of small leaves close to the ground, optionally dotted with flowers.
void World::addShrub(float x, float z, float s, RNG& r, bool flowering) {
    float y = terrain(x, z);
    if (y < 0.8f) return;
    MeshBuilder& m = mb(x, z);
    Color leaf = jitterC(col(78, 136, 56), r, 0.14f);
    int n = r.irange(2, 3);
    for (int k = 0; k < n; k++) {
        float a = r.range(0, TAU_F), rr = k == 0 ? 0 : 0.55f * s;
        m.leafCloud({x + cosf(a) * rr, y + 0.5f * s, z + sinf(a) * rr}, {0.75f * s, 0.55f * s, 0.75f * s}, 16, 0.42f * s, 0.26f * s, leaf, r, k == 0);
    }
    if (flowering) {
        static const Color P[] = {col(226, 70, 120), col(246, 206, 64), col(240, 240, 236), col(150, 90, 200), col(236, 110, 60)};
        Color pc = P[r.irange(0, 4)];
        for (int k = 0; k < 7; k++) {
            float a = r.range(0, TAU_F), rr = r.range(0.1f, 0.7f) * s;
            Vector3 p = {x + cosf(a) * rr, y + r.range(0.4f, 0.95f) * s, z + sinf(a) * rr};
            m.leaf(p, V3(cosf(a), 0.8f, sinf(a)), 0.2f, 0.18f, r.range(0, TAU_F), pc);
        }
    }
}

// A patch of individual flowers with stems and leaves, in a mixed color or two.
void World::addFlowerBed(float x0, float z0, float x1, float z1, int count, RNG& r) {
    static const Color P[] = {col(228, 64, 112), col(250, 210, 60), col(244, 244, 240), col(160, 96, 210), col(240, 120, 56), col(230, 60, 60)};
    Color a = P[r.irange(0, 5)], b = P[r.irange(0, 5)];
    for (int k = 0; k < count; k++) {
        float x = r.range(x0, x1), z = r.range(z0, z1), y = terrain(x, z);
        if (y < 0.8f) continue;
        mb(x, z).flower({x, y, z}, r.range(0.28f, 0.55f), r.chance(0.6f) ? a : b, col(250, 220, 90), r);
    }
}

void World::addLamp(float x, float z, float yaw) {
    if (doorBlocked(x, z, 2.1f)) return;
    float y = terrain(x, z);
    // keep the pole out of buildings and landmark bases, and the arm's lamp head out of their walls
    Vector3 head = {x + cosf(yaw) * 1.4f, y + 6.6f, z + sinf(yaw) * 1.4f};
    if (buildingAt(x, z, 0.2f, y - 0.2f, y + 6.6f) || buildingAt(head.x, head.z, 0.3f, head.y - 0.3f, head.y + 0.1f)) return;
    MeshBuilder& m = mb(x, z);
    Color pole = col(56, 66, 60);
    m.beam({x, y - 0.2f, z}, {x, y + 6.5f, z}, 0.18f, 0.18f, pole);
    Vector3 tip = head;
    m.beam({x, y + 6.4f, z}, tip, 0.1f, 0.1f, pole);
    m.box({tip.x - 0.3f, tip.y - 0.25f, tip.z - 0.3f}, {tip.x + 0.3f, tip.y + 0.05f, tip.z + 0.3f}, col(255, 222, 160, M_EMISSIVE));
    lamps.push_back({tip.x, tip.y - 0.4f, tip.z});
    addPlat({x - 0.15f, x + 0.15f, z - 0.15f, z + 0.15f, y + 6.5f, y + 6.5f, y - 1, 0, PK_POLE});
}

// A flat disk standing upright (wheels): n-sided fan around c in the plane of unit vectors a and b.
static void disk(MeshBuilder& m, Vector3 c, Vector3 a, Vector3 b, float r, int n, Color k) {
    for (int i = 0; i < n; i++) {
        float a0 = TAU_F * i / n, a1 = TAU_F * (i + 1) / n;
        auto P = [&](float t) { return Vector3Add(c, Vector3Add(Vector3Scale(a, cosf(t) * r), Vector3Scale(b, sinf(t) * r))); };
        m.tri(c, P(a0), P(a1), k);
    }
}

void World::addClutter(Vector2 p, Vector2 f, Vector2 o, int kind, uint32_t h) {
    float y = terrain(p.x, p.y);
    if (buildingAt(p.x, p.y, kind == 5 ? 2.2f : 1.0f, y - 0.2f, y + 2.6f)) return;   // not inside a building or landmark base
    MeshBuilder& m = mb(p.x, p.y);
    Vector3 F = {f.x, 0, f.y}, O = {o.x, 0, o.y}, U = {0, 1, 0};
    auto at = [&](float alongF, float up, float outO) { return V3(p.x + f.x * alongF + o.x * outO, y + up, p.y + f.y * alongF + o.y * outO); };
    auto pole = [&](float alongF, float outO, float hw, float top) {   // collision for something small and solid
        Vector3 q = at(alongF, 0, outO);
        addPlat({q.x - hw, q.x + hw, q.z - hw, q.z + hw, y + top, y + top, y - 1, 0, PK_POLE});
    };
    float yaw = atan2f(f.y, f.x);
    switch (kind) {
    case 0: {   // fire hydrant: squat white body, colored bonnet, side outlets
        static const Color CAP[] = {col(40, 90, 170), col(200, 40, 40), col(230, 190, 40), col(240, 240, 235)};
        Color body = col(232, 230, 222), cap = CAP[(h >> 17) % 4];
        m.prism(at(0, -0.05f, 0), 8, 0.2f, 0.2f, 0.12f, body, body);
        m.prism(at(0, 0.07f, 0), 8, 0.14f, 0.13f, 0.5f, body, body);
        m.prism(at(0, 0.57f, 0), 8, 0.16f, 0.05f, 0.16f, cap, cap);
        m.beam(at(-0.2f, 0.36f, 0), at(0.2f, 0.36f, 0), 0.1f, 0.1f, cap);
        m.beam(at(0, 0.36f, 0), at(0, 0.36f, 0.2f), 0.13f, 0.13f, cap);
        pole(0, 0, 0.18f, 0.75f);
        break;
    }
    case 1: {   // SF's green public trash can, lid and mouth facing the sidewalk
        Color g = col(42, 74, 54), dk = col(28, 44, 34);
        m.prism(at(0, -0.05f, 0), 10, 0.3f, 0.29f, 0.98f, g, dk);
        m.prism(at(0, 0.93f, 0), 10, 0.33f, 0.28f, 0.1f, dk, dk);
        m.obox(at(0, 0.78f, -0.27f), {0.04f, 0.09f, 0.18f}, yaw + PI / 2, 0, col(16, 18, 18));
        pole(0, 0, 0.3f, 1.0f);
        break;
    }
    case 2: {   // a row of newspaper boxes in different colors, coin slot and window on the sidewalk side
        static const Color NB[] = {col(30, 70, 160), col(200, 40, 40), col(230, 190, 40), col(40, 120, 70), col(230, 230, 230), col(120, 60, 140)};
        int n = 2 + (h >> 18) % 2;
        for (int k = 0; k < n; k++) {
            float a = (k - (n - 1) * 0.5f) * 0.56f;
            Color c = NB[(h >> (20 + k * 3)) % 6];
            m.obox(at(a, 0.62f, 0), {0.24f, 0.42f, 0.22f}, yaw, 0, c);
            m.obox(at(a, 0.75f, -0.225f), {0.18f, 0.14f, 0.01f}, yaw, 0, col(200, 205, 200));   // window
            for (int l = -1; l <= 1; l += 2) m.beam(at(a + l * 0.18f, -0.05f, 0), at(a + l * 0.18f, 0.22f, 0), 0.05f, 0.05f, col(60, 60, 64));
        }
        Vector3 q0 = at(-n * 0.28f, 0, -0.25f), q1 = at(n * 0.28f, 0, 0.25f);
        addPlat({std::min(q0.x, q1.x), std::max(q0.x, q1.x), std::min(q0.z, q1.z), std::max(q0.z, q1.z), y + 1.05f, y + 1.05f, y - 1, 0, PK_POLE});
        break;
    }
    case 3: {   // bike rack (two inverted U's) with a bike or two locked to it, standing across the curb
        static const Color BK[] = {col(200, 40, 40), col(30, 30, 34), col(40, 100, 170), col(230, 230, 225), col(60, 140, 90), col(230, 140, 40)};
        Color steel = col(140, 142, 148);
        for (int k = 0; k < 2; k++) {
            float a = k ? 0.45f : -0.45f;
            m.beam(at(a, -0.05f, -0.35f), at(a, 0.82f, -0.35f), 0.05f, 0.05f, steel);
            m.beam(at(a, -0.05f, 0.35f), at(a, 0.82f, 0.35f), 0.05f, 0.05f, steel);
            m.beam(at(a, 0.82f, -0.37f), at(a, 0.82f, 0.37f), 0.05f, 0.05f, steel);
        }
        int bikes = 1 + (h >> 18) % 2;
        for (int k = 0; k < bikes; k++) {
            float a = k ? 0.7f : -0.2f;
            Color fr = BK[(h >> (20 + k * 3)) % 6], tire = col(22, 22, 24);
            Vector3 rear = at(a, 0.34f, -0.52f), front = at(a, 0.34f, 0.52f);
            disk(m, rear, O, U, 0.34f, 8, tire);
            disk(m, front, O, U, 0.34f, 8, tire);
            disk(m, rear, O, U, 0.07f, 6, col(170, 170, 175));
            disk(m, front, O, U, 0.07f, 6, col(170, 170, 175));
            Vector3 bb = at(a, 0.3f, -0.05f), seat = at(a, 0.88f, -0.2f), head = at(a, 0.85f, 0.38f);
            m.beam(rear, bb, 0.035f, 0.035f, fr);
            m.beam(bb, seat, 0.04f, 0.04f, fr);
            m.beam(seat, head, 0.04f, 0.04f, fr);
            m.beam(bb, head, 0.045f, 0.045f, fr);
            m.beam(rear, seat, 0.03f, 0.03f, fr);
            m.beam(head, front, 0.035f, 0.035f, col(150, 150, 155));
            m.obox(Vector3Add(seat, V3(0, 0.05f, 0)), {0.12f, 0.03f, 0.06f}, yaw + PI / 2, 0, col(25, 25, 25));
            Vector3 hb = Vector3Add(head, V3(0, 0.12f, 0));
            m.beam(Vector3Add(hb, Vector3Scale(F, -0.25f)), Vector3Add(hb, Vector3Scale(F, 0.25f)), 0.03f, 0.03f, col(40, 40, 42));
        }
        pole(0.15f, 0, 0.6f, 0.9f);
        break;
    }
    case 4: {   // dockless scooters on their kickstands, leaning a little
        static const Color SC[] = {col(90, 200, 70), col(30, 30, 32), col(230, 70, 60), col(240, 210, 50)};
        int n = 1 + (h >> 18) % 3;
        for (int k = 0; k < n; k++) {
            float a = (k - (n - 1) * 0.5f) * 0.42f, lean = 0.08f;
            Color c = SC[(h >> (20 + k * 2)) % 4];
            Vector3 tilt = Vector3Scale(F, lean);
            Vector3 b0 = at(a, 0.1f, -0.38f), b1 = at(a, 0.1f, 0.38f);
            m.beam(b0, b1, 0.14f, 0.06f, c);                                              // deck
            disk(m, at(a, 0.1f, -0.42f), O, U, 0.1f, 6, col(22, 22, 24));
            disk(m, at(a, 0.1f, 0.42f), O, U, 0.1f, 6, col(22, 22, 24));
            Vector3 top = Vector3Add(at(a, 1.0f, 0.36f), tilt);
            m.beam(at(a, 0.1f, 0.4f), top, 0.045f, 0.045f, col(60, 60, 64));             // stem
            m.beam(Vector3Add(top, Vector3Scale(F, -0.24f)), Vector3Add(top, Vector3Scale(F, 0.24f)), 0.035f, 0.035f, col(40, 40, 42));
        }
        pole(0, 0, 0.35f, 1.0f);
        break;
    }
    case 5: {   // Muni bus shelter: back panel on the curb side, roof, bench, a lit ad panel at one end
        Color frame = col(70, 74, 80), glass = col(150, 175, 190), roof = col(90, 94, 100);
        float L = 1.9f;
        for (int a = -1; a <= 1; a += 2)
            for (int b = 0; b < 2; b++) m.beam(at(a * L, -0.05f, b ? -0.6f : 0.55f), at(a * L, 2.45f, b ? -0.6f : 0.55f), 0.08f, 0.08f, frame);
        m.obox(at(0, 2.5f, -0.02f), {L + 0.15f, 0.06f, 0.75f}, yaw, 0, roof);
        m.obox(at(0, 1.3f, 0.55f), {L - 0.05f, 1.0f, 0.02f}, yaw, 0, glass);
        m.obox(at(-L, 1.25f, 0), {0.03f, 0.9f, 0.55f}, yaw, 0, glass);
        m.obox(at(L, 1.25f, 0), {0.06f, 0.95f, 0.55f}, yaw, 0, col(250, 236, 200, M_EMISSIVE));   // backlit ad
        m.obox(at(0, 0.45f, 0.32f), {1.3f, 0.04f, 0.2f}, yaw, 0, col(150, 120, 90));               // bench
        for (int a = -1; a <= 1; a += 2) seats.push_back({at(a * 0.45f, 0.49f, 0.42f), atan2f(-o.y, -o.x), false});
        m.beam(at(-L - 0.4f, -0.05f, 0.75f), at(-L - 0.4f, 2.9f, 0.75f), 0.06f, 0.06f, frame);      // stop sign pole
        m.obox(at(-L - 0.4f, 2.75f, 0.75f), {0.25f, 0.18f, 0.02f}, yaw, 0, col(200, 40, 40));
        Vector3 q0 = at(-L, 0, 0.5f), q1 = at(L, 0, 0.62f);
        addPlat({std::min(q0.x, q1.x), std::max(q0.x, q1.x), std::min(q0.z, q1.z), std::max(q0.z, q1.z), y + 2.3f, y + 2.3f, y - 1, 0, PK_POLE});
        for (int a = -1; a <= 1; a += 2) pole(a * L, -0.6f, 0.08f, 2.4f);
        auto box = [&](float f0, float f1, float o0, float o1, float top, float base, uint8_t pk) {
            Vector3 c0 = at(f0, 0, o0), c1 = at(f1, 0, o1);
            addPlat({std::min(c0.x, c1.x), std::max(c0.x, c1.x), std::min(c0.z, c1.z), std::max(c0.z, c1.z), y + top, y + top, base, 0, pk});
        };
        box(-L - 0.05f, -L + 0.05f, -0.55f, 0.55f, 2.15f, y - 1, PK_POLE);   // glass end panel
        box(L - 0.06f, L + 0.06f, -0.55f, 0.55f, 2.2f, y - 1, PK_POLE);      // ad panel
        pole(-L - 0.4f, 0.75f, 0.06f, 2.9f);                                   // stop sign pole
        box(-L - 0.15f, L + 0.15f, -0.77f, 0.73f, 2.56f, y + 2.44f, PK_DECK);  // roof: a ceiling to bump your head on
        break;
    }
    }
}

// Intersections: traffic signals downtown (a pole on the near-right corner of each approach with a mast arm over
// the lanes; the lamps themselves are drawn live by Traffic), four-way stop signs in the neighborhoods, manhole
// covers in the road, a few letting off steam.
void World::buildIntersections(const Grid& g) {
    signals.assign((size_t)g.ni * g.nj, 0);
    Color pole = col(70, 76, 72), stopRed = col(196, 30, 36), white = col(240, 240, 236), dark = col(36, 38, 36);
    for (int j = 0; j < g.nj; j++)
        for (int i = 0; i < g.ni; i++) {
            if (!g.nodeOk(i, j)) continue;
            bool ex = g.edge(i, j, 0) || g.edge(i, j, 1), ez = g.edge(i, j, 2) || g.edge(i, j, 3);
            Vector2 n = g.node(i, j);
            float md, rs;
            if (marketDist(n.x, n.y, &md) && md < 30) continue;
            if (n.y > 800 && railDistance(n.x, n.y, &rs) < 20) continue;
            if (i == 24 || terrain(n.x, n.y) < 1.f) continue;   // the Embarcadero has its own median
            int style = buildStyleAt(n.x, n.y);
            uint32_t hh = hashu(i, j, 404);
            // manhole covers out in the lanes
            for (int d = 0; d < 4; d++) {
                if (!g.edge(i, j, d) || ((hh >> d) & 1)) continue;
                Vector2 dir = {(float)DI[d], (float)DJ[d]}, right = {-dir.y, dir.x};
                Vector2 q = {n.x + dir.x * 16.f + right.x * 2.2f, n.y + dir.y * 16.f + right.y * 2.2f};
                float qy = terrain(q.x, q.y) + 0.085f;
                mb(q.x, q.y).prism({q.x, qy, q.y}, 10, 0.36f, 0.36f, 0.015f, col(46, 44, 42, M_GROUND), col(40, 38, 36, M_GROUND));
                bool steamy = style == BS_TOWERS || style == BS_MIDRISE || style == BS_MISSION || style == BS_WHARF;
                if (steamy && hash01(i * 4 + d, j, 77) < 0.16f) steamVents.push_back({q.x, qy, q.y});
            }
            if (!ex || !ez) continue;
            bool signal = style == BS_TOWERS || style == BS_MIDRISE || style == BS_MISSION || style == BS_WHARF;
            if (signal) signals[(size_t)j * g.ni + i] = 1;
            // each approach: arriving from direction d means heading -dir; its curb is on the driver's right
            for (int d = 0; d < 4; d++) {
                if (!g.edge(i, j, d)) continue;
                Vector2 head = {-(float)DI[d], -(float)DJ[d]}, right = {-head.y, head.x};
                // the near-right corner, 7.7 m out from both streets
                Vector2 c = {n.x - head.x * 7.7f + right.x * 7.7f, n.y - head.y * 7.7f + right.y * 7.7f};
                if (doorBlocked(c.x, c.y, 3.6f)) continue;
                float y = terrain(c.x, c.y);
                if (buildingAt(c.x, c.y, 0.3f, y - 0.2f, y + 6.5f)) continue;   // a corner inside a landmark's base
                MeshBuilder& m = mb(c.x, c.y);
                Vector3 face = {-head.x, 0, -head.y};   // toward the oncoming cars
                if (signal) {
                    m.prism({c.x, y - 0.2f, c.y}, 8, 0.13f, 0.1f, 6.4f, pole, pole);
                    Vector2 tip = {c.x - right.x * 5.2f, c.y - right.y * 5.2f};
                    m.beam({c.x, y + 6.1f, c.y}, {tip.x, y + 6.1f, tip.y}, 0.12f, 0.12f, pole);
                    float yaw = atan2f(face.z, face.x);
                    for (int k = 0; k < 2; k++) {   // one head hanging from the arm, one on the pole at eye level
                        Vector3 hp = k ? V3(c.x + face.x * 0.18f, y + 3.1f, c.y + face.z * 0.18f) : V3(tip.x, y + 5.35f, tip.y);
                        m.obox(hp, {0.15f, 0.55f, 0.2f}, yaw, 0, dark);
                        m.obox(Vector3Add(hp, Vector3Scale(face, -0.02f)), {0.17f, 0.6f, 0.24f}, yaw, 0, col(30, 30, 30));
                        signalHeads.push_back({Vector3Add(hp, Vector3Scale(face, 0.17f)), {face.x, face.z}, (int)(j * g.ni + i), (uint8_t)(d < 2 ? 0 : 1)});
                    }
                    addPlat({c.x - 0.15f, c.x + 0.15f, c.y - 0.15f, c.y + 0.15f, y + 6.3f, y + 6.3f, y - 1, 0, PK_POLE});
                } else {   // a red octagon on a post, white rim behind it
                    m.beam({c.x, y - 0.2f, c.y}, {c.x, y + 2.6f, c.y}, 0.07f, 0.07f, col(150, 150, 154));
                    Vector3 sc = {c.x + face.x * 0.05f, y + 2.45f, c.y + face.z * 0.05f};
                    Vector3 side = {-face.z, 0, face.x};
                    disk(m, sc, side, {0, 1, 0}, 0.4f, 8, white);
                    disk(m, Vector3Add(sc, Vector3Scale(face, 0.012f)), side, {0, 1, 0}, 0.36f, 8, stopRed);
                    m.obox(Vector3Add(sc, Vector3Scale(face, 0.02f)), {0.008f, 0.035f, 0.16f}, atan2f(face.z, face.x), 0, white);   // the lettering
                    addPlat({c.x - 0.08f, c.x + 0.08f, c.y - 0.08f, c.y + 0.08f, y + 2.6f, y + 2.6f, y - 1, 0, PK_POLE});
                }
            }
        }
}

void World::buildStreetFurniture(const Grid& g, bool oakland) {
    RNG r(oakland ? 4242 : 2424);
    for (int j = 0; j < g.nj; j++)
        for (int i = 0; i < g.ni; i++)
            for (int d = 0; d < 4; d += 2) {
                if (!g.edge(i, j, d)) continue;
                Vector2 a = g.node(i, j), b = g.node(i + DI[d], j + DJ[d]);
                Vector2 dir = {(float)DI[d], (float)DJ[d]}, right = {-dir.y, dir.x};
                float L = g.sp;
                bool embarcadero = !oakland && i == 24 && d == 2;
                int slot = 0;
                for (float s = 15; s < L - 13; s += 7.5f, slot++) {
                    Vector2 c = Vector2Add(a, Vector2Scale(dir, s));
                    float rs;
                    if (!oakland && c.y > 800 && railDistance(c.x, c.y, &rs) < 12) continue;
                    if (embarcadero) { // palm-lined median on the Embarcadero
                        if (slot % 3 == 0) addTree(c.x, c.y, 2, r.range(0.9f, 1.15f), r, true);
                        continue;
                    }
                    int style = buildStyleAt(c.x, c.y);
                    for (int side = -1; side <= 1; side += 2) {
                        Vector2 lat = Vector2Scale(right, (float)side);
                        // street lamp
                        if ((slot + (side > 0 ? 0 : 2)) % 4 == 0) {
                            Vector2 p = Vector2Add(c, Vector2Scale(lat, 7.7f));
                            if (terrain(p.x, p.y) > 1 && !doorBlocked(p.x, p.y, 2.1f)) addLamp(p.x, p.y, atan2f(-lat.y, -lat.x));
                            continue;
                        }
                        // parked car
                        float md;
                        bool nearMarket = !oakland && (marketDist(c.x, c.y, &md) ? md < 16 : false);
                        if (!nearMarket && r.chance(style == BS_TOWERS ? 0.08f : 0.16f)) {
                            Vector2 p = Vector2Add(c, Vector2Scale(lat, 5.6f));
                            float yaw = atan2f(dir.y * side, dir.x * side);
                            float h0 = terrain(p.x - dir.x * 2, p.y - dir.y * 2), h1 = terrain(p.x + dir.x * 2, p.y + dir.y * 2);
                            float pitch = atan2f((h1 - h0) * side, 4.f);
                            float y = terrain(p.x, p.y) + 0.08f;
                            int type = r.chance(0.2f) ? CT_SUV : (r.chance(0.08f) ? CT_VAN : CT_SEDAN);
                            const CarModel& cm = carModel(type);
                            Color carC = randomCarColor(r);
                            if (doorBlocked(p.x, p.y, 3.4f)) continue;
                            addCarToMesh(mb(p.x, p.y), type, carC, {p.x, y, p.y}, yaw, pitch);
                            float hl = cm.halfLen, hw = cm.halfWid;
                            float ex = fabsf(dir.x) > 0.5f ? hl : hw, ez = fabsf(dir.x) > 0.5f ? hw : hl;
                            addPlat({p.x - ex, p.x + ex, p.y - ez, p.y + ez, y + cm.height - 0.1f, y + cm.height - 0.1f, y - 0.5f, 0, PK_CAR});
                            continue;
                        }
                        // sidewalk tree
                        bool leafy = style == BS_ROWHOUSE || style == BS_MISSION || style == BS_SUNSET || style == BS_OAK || style == BS_MIDRISE;
                        if (leafy && r.chance(0.22f)) {
                            Vector2 p = Vector2Add(c, Vector2Scale(lat, 10.1f));
                            if (terrain(p.x, p.y) > 1.2f && !doorBlocked(p.x, p.y, 2.6f)) addTree(p.x, p.y, r.chance(0.15f) ? 2 : 0, r.range(0.8f, 1.1f), r, true);
                        }
                        // curbside clutter between the parked cars and the walking line (hashed, so the rest of the
                        // street keeps its layout). Shelters and news boxes cluster downtown, bikes and bins everywhere.
                        if (!nearMarket && slot % 2 == 1) {
                            uint32_t hh = hashu((int)floorf(c.x * 3.f) + side * 7, (int)floorf(c.y * 3.f), 99);
                            float u = (hh & 0xFFFF) / 65536.f;
                            bool busy = style == BS_TOWERS || style == BS_MIDRISE || style == BS_MISSION || style == BS_WHARF || style == BS_OAK_DOWNTOWN;
                            int kind = -1;
                            if (u < 0.07f) kind = 0;
                            else if (u < (busy ? 0.15f : 0.11f)) kind = 1;
                            else if (u < (busy ? 0.2f : 0.12f)) kind = 2;
                            else if (u < (busy ? 0.25f : 0.15f)) kind = 3;
                            else if (u < (busy ? 0.3f : 0.17f)) kind = 4;
                            else if (busy && u < 0.33f && s > 30 && s < L - 30) kind = 5;
                            Vector2 p = Vector2Add(c, Vector2Scale(lat, kind == 5 ? 8.55f : 7.75f));
                            if (kind >= 0 && terrain(p.x, p.y) > 1.f && !doorBlocked(p.x, p.y, kind == 5 ? 4.6f : 3.9f))
                                addClutter(p, dir, Vector2Scale(lat, -1.f), kind, hh);
                        }
                    }
                }
            }
}

// ---------------------------------------------------------------- parks and nature
void World::buildParks() {
    RNG r(5150);
    // Golden Gate Park: forest with meadows, JFK Drive through the middle, Stow Lake
    float pz0 = gz(12) + 8, pz1 = gz(14) - 8;
    roadStrip(*this, {gx(0) - 60, 335}, {gx(8), 335}, 5.0f, 5.0f, 0, 0.08f, C_ASPHALT);
    ponds.push_back({-840, 365, -760, 410, "lake"});
    {
        MeshBuilder& m = mb(-800, 387);
        float y = terrain(-800, 387) - 0.3f;
        m.quad({-840, y, 365}, {-760, y, 365}, {-760, y, 410}, {-840, y, 410}, col(40, 90, 100, M_WATER));
    }
    for (int k = 0; k < 2600; k++) {
        float x = r.range(-1460, gx(8) - 6), z = r.range(pz0, pz1);
        if (fabsf(z - 335) < 9) continue;
        if (x > -845 && x < -755 && z > 360 && z < 415) continue;
        if (hash01((int)floorf(x / 70), (int)floorf(z / 70), 9) < 0.3f) continue; // meadow
        addTree(x, z, r.chance(0.4f) ? 3 : (r.chance(0.5f) ? 1 : 0), r.range(0.9f, 1.4f), r, true);
    }
    // Presidio forest (keep the bridge approach clear)
    for (int k = 0; k < 2400; k++) {
        float x = r.range(-1440, gx(4) - 6), z = r.range(-1060, gz(4) - 6);
        if (fabsf(x + 1100) < 24) continue;
        if (terrain(x, z) < 3) continue;
        addTree(x, z, r.chance(0.55f) ? 3 : 1, r.range(0.9f, 1.5f), r, true);
    }
    // smaller parks: scatter trees over zones that have them
    struct PZ { float x0, z0, x1, z1; int n; };
    PZ small[] = {{gx(12), gz(17), gx(13), gz(18), 30}, {gx(11), gz(5), gx(12), gz(6), 30}, {gx(20), gz(2), gx(21), gz(3), 18},
                  {-605, 575, -435, 805, 120}, {-510, 435, -350, 605, 60}, {800, -960, 960, -800, 35},
                  {-610, 1190, -390, 1410, 120}, {gx(9), gz(11), gx(10), gz(12), 10}};
    for (auto& p : small)
        for (int k = 0; k < p.n; k++) {
            float x = r.range(p.x0 + 6, p.x1 - 6), z = r.range(p.z0 + 6, p.z1 - 6);
            int zf = zoneFlags(x, z, true);
            if (!(zf & (Z_TREES | Z_GRASS))) continue;
            if (sq(x - 880) + sq(z + 880) < sq(30)) continue; // Coit Tower base
            if (sq(x + 470) + sq(z - 770) < sq(30)) continue; // Sutro Tower base
            addTree(x, z, r.chance(0.3f) ? 2 : (r.chance(0.5f) ? 3 : 0), r.range(0.9f, 1.3f), r, true);
        }
    // Twin Peaks / Bernal scrub
    for (int k = 0; k < 400; k++) {
        float a = r.range(0, TAU_F), d = sqrtf(r.f()) * 170;
        float x = -295 + cosf(a) * d, z = 895 + sinf(a) * d;
        if (hash01((int)x, (int)z) < 0.5f) continue;
        float y = terrain(x, z);
        mb(x, z).prism({x, y - 0.3f, z}, 5, 1.2f, 0.5f, 1.2f, jitterC(col(110, 120, 70), r, 0.15f), col(100, 115, 65), r.f());
    }
    // Marin, Angel Island, Yerba Buena: decorative trees (out of reach, no collision)
    for (int k = 0; k < 2500; k++) {
        float x = r.range(-2800, -200), z = r.range(-3800, -2350);
        float h = terrain(x, z);
        if (h < 6 || h > 230) continue;
        if (hash01((int)floorf(x / 120), (int)floorf(z / 120), 4) < 0.45f) continue;
        bool nearVista = sq(x + 1100) + sq(z + 2410) < sq(140);
        if (nearVista && sq(x + 1100) + sq(z + 2410) < sq(80)) continue;
        addTree(x, z, r.chance(0.6f) ? 3 : 1, r.range(1.0f, 1.8f), r, nearVista);
    }
    for (int k = 0; k < 600; k++) {
        float a = r.range(0, TAU_F), d = sqrtf(r.f()) * 470;
        float x = 330 + cosf(a) * d, z = -3050 + sinf(a) * d * 0.8f;
        if (terrain(x, z) > 6) addTree(x, z, r.chance(0.5f) ? 3 : 1, r.range(1.2f, 1.9f), r, false);
    }
    for (int k = 0; k < 220; k++) {
        float x = r.range(2460, 2790), z = r.range(60, 300);
        if (terrain(x, z) > 5 && fabsf(z - 335) > 22) addTree(x, z, r.chance(0.5f) ? 3 : 1, r.range(1.0f, 1.5f), r, false);
    }
    // Sausalito & East Bay hillside houses (scenery)
    for (int k = 0; k < 500; k++) {
        float x = r.range(-700, -200), z = r.range(-3100, -2450);
        float h = terrain(x, z);
        if (h < 4 || h > 90) continue;
        Color c = PASTEL[r.irange(0, 11)];
        float w = r.range(6, 10), d = r.range(6, 10);
        mb(x, z).box({x, h - 2, z}, {x + w, h + r.range(5, 9), z + d}, withMat(c, M_PLAIN), col(110, 90, 80), 0.8f);
    }
    for (int k = 0; k < 2500; k++) {
        float x = r.range(4720, 6300), z = r.range(-2500, 2500);
        float h = terrain(x, z);
        if (h < 4 || h > 260) continue;
        if (h > 120 && r.chance(0.6f)) { addTree(x, z, 3, r.range(1.5f, 2.2f), r, false); continue; }
        Color c = r.chance(0.5f) ? PASTEL[r.irange(0, 11)] : STONE[r.irange(0, 7)];
        mb(x, z).box({x, h - 2, z}, {x + r.range(8, 14), h + r.range(5, 9), z + r.range(8, 14)}, withMat(c, M_PLAIN), col(110, 90, 80), 0.8f);
    }
    // south of the city (Daly City) – scenery houses beyond the street grid
    for (int k = 0; k < 900; k++) {
        float x = r.range(-1400, 1300), z = r.range(1860, 2450);
        float h = terrain(x, z);
        if (h < 2 || h > 150) continue;
        if (sq(x - 1000) + sq(z - 2450) < sq(320)) continue; // Sign Hill stays bare
        float rs;
        if (railDistance(x, z, &rs) < 22) continue;
        Color c = PASTEL[r.irange(0, 11)];
        mb(x, z).box({x, h - 2, z}, {x + r.range(8, 12), h + r.range(5, 8), z + r.range(10, 16)}, withMat(c, M_PLAIN), jitterC(col(196, 170, 150), r, 0.1f), 0.8f);
    }
}
