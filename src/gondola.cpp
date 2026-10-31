// Gondola lines: stations on summits, lattice towers, sagging cables, and cabins that slow into each station.
#include "gondola.h"
#include "world_internal.h"
#include <algorithm>

namespace {
constexpr float CABLE_GAP = 1.5f;     // each cable sits this far to the side of the centerline
constexpr float CLEARANCE = 15.f;     // cable height above the ground at the least
constexpr float CABLE_SPEED = 6.f;
constexpr float TERMINAL_H = 5.2f;    // terminal walls above its (lowest) ground
constexpr float ROOF_SLAB = 0.38f;    // the roof slab's top above the walls (centre +0.2, half-height 0.18)
constexpr float ROOF_R = 7.2f * 1.4143f + 1.6f;   // roof slab half-diagonal plus a cabin's half-width and its swing
constexpr float CABIN_CLEAR = 0.8f;   // gap left between a cabin's floor and the roof below it

struct Node { float x, z, y; bool station; float top; float gy = 0; };   // y: ground; top: cable height above ground; gy: terminal ground

// the terminal stands on the lowest ground under its 12 m footprint
float terminalGround(const World& w, float x, float z) {
    float gy = w.terrain(x, z);
    for (float dx = -6; dx <= 6; dx += 6) for (float dz = -6; dz <= 6; dz += 6) gy = std::min(gy, w.terrain(x + dx, z + dz));
    return gy;
}

Vector2 peakNear(const World& w, float cx, float cz, float r) {
    Vector2 best = {cx, cz};
    float hb = -1e9f;
    for (float x = cx - r; x <= cx + r; x += 5)
        for (float z = cz - r; z <= cz + r; z += 5) {
            if (sq(x - cx) + sq(z - cz) > r * r) continue;
            float h = w.terrain(x, z);
            if (h > hb) { hb = h; best = {x, z}; }
        }
    return best;
}

float sagAt(float t, float span) { return 4.f * t * (1.f - t) * span * 0.012f; }
}

void World::buildGondolas() {
    struct Def { std::vector<Vector3> pts; Color accent; };   // pts: x, z, search radius (0 = exact)
    const Def defs[] = {
        {{{-520, 690, 60}, {-300, 900, 90}, {-500, 1300, 70}}, col(214, 60, 52)},          // Mt Sutro - Twin Peaks - Mt Davidson
        {{{200, 2350, 70}, {720, 2430, 0}}, col(40, 150, 170)},                            // San Bruno Mountain - Sign Hill
        {{{-1110, -2440, 0}, {-1500, -2850, 90}}, col(232, 150, 40)},                      // Vista Point - Marin Headlands
    };
    Color steel = col(150, 156, 162), dark = col(52, 56, 62), conc = col(188, 184, 176), roofC = col(226, 228, 230);
    for (auto& d : defs) {
        // stations at the ends and wherever a line has a middle stop; towers between them every ~130 m
        std::vector<Node> nodes;
        std::vector<Vector2> keys;
        for (auto& p : d.pts) keys.push_back(p.z > 0 ? peakNear(*this, p.x, p.y, p.z) : Vector2{p.x, p.y});
        // (the def stores z in .y and the search radius in .z)
        for (size_t k = 0; k < keys.size(); k++) {
            if (k) {
                Vector2 a = keys[k - 1], b = keys[k];
                float L = Vector2Distance(a, b);
                int n = (int)ceilf(L / 130.f);
                for (int i = 1; i < n; i++) {
                    float t = (float)i / n;
                    float x = lerpf(a.x, b.x, t), z = lerpf(a.y, b.y, t);
                    nodes.push_back({x, z, terrain(x, z), false, 24.f});
                }
            }
            Node st{keys[k].x, keys[k].y, terrain(keys[k].x, keys[k].y), true, 0};
            st.gy = terminalGround(*this, st.x, st.z);
            // the cable meets the station high enough that a hanging cabin clears the terminal roof
            st.top = st.gy + TERMINAL_H + ROOF_SLAB + GONDOLA_CABIN_DROP + CABIN_CLEAR - st.y;
            nodes.push_back(st);
        }
        // raise towers until every span clears the ground below it
        for (int it = 0; it < 60; it++) {
            bool changed = false;
            for (size_t i = 0; i + 1 < nodes.size(); i++) {
                Node &a = nodes[i], &b = nodes[i + 1];
                float span = hypotf(b.x - a.x, b.z - a.z);
                float worst = 0;
                for (int s = 1; s < 24; s++) {
                    float t = s / 24.f;
                    float y = lerpf(a.y + a.top, b.y + b.top, t) - sagAt(t, span);
                    worst = std::max(worst, terrain(lerpf(a.x, b.x, t), lerpf(a.z, b.z, t)) + CLEARANCE - y);
                }
                if (worst > 0.2f) {
                    if (!a.station) a.top = std::min(a.top + worst, 80.f);
                    if (!b.station) b.top = std::min(b.top + worst, 80.f);
                    changed = true;
                }
            }
            if (!changed) break;
        }
        // the cable can still dip toward a lower neighbour (and sags) while it is over a terminal's roof:
        // raise the station until every point of it within reach of the roof keeps the cabin clear
        for (size_t i = 0; i < nodes.size(); i++) {
            Node& st = nodes[i];
            if (!st.station) continue;
            float need = st.gy + TERMINAL_H + ROOF_SLAB + GONDOLA_CABIN_DROP + CABIN_CLEAR, raise = 0;
            for (int nb = -1; nb <= 1; nb += 2) {
                if ((nb < 0 && i == 0) || (nb > 0 && i + 1 == nodes.size())) continue;
                const Node& o = nodes[i + nb];
                float span = hypotf(o.x - st.x, o.z - st.z);
                for (float dd = 0; dd <= std::min(ROOF_R, span); dd += 0.5f) {
                    float t = dd / span;   // from the station toward the neighbour (sag is symmetric in t)
                    float y = lerpf(st.y + st.top, o.y + o.top, t) - sagAt(t, span);
                    if (y < need) raise = std::max(raise, (need - y) / std::max(1.f - t, 0.5f));
                }
            }
            st.top += raise;
        }
        // centerline with sag, every ~10 m
        GondolaLine gl;
        gl.accent = d.accent;
        for (size_t i = 0; i < nodes.size(); i++) {
            if (nodes[i].station) gl.stationS.push_back(0); // filled below
            if (i + 1 == nodes.size()) break;
            Node &a = nodes[i], &b = nodes[i + 1];
            float span = hypotf(b.x - a.x, b.z - a.z);
            int n = std::max(2, (int)(span / 10.f));
            for (int s = 0; s < n; s++) {
                float t = (float)s / n;
                gl.path.push_back({lerpf(a.x, b.x, t), lerpf(a.y + a.top, b.y + b.top, t) - sagAt(t, span), lerpf(a.z, b.z, t)});
            }
        }
        gl.path.push_back({nodes.back().x, nodes.back().y + nodes.back().top, nodes.back().z});
        gl.cum.assign(gl.path.size(), 0);
        for (size_t i = 1; i < gl.path.size(); i++) gl.cum[i] = gl.cum[i - 1] + Vector3Distance(gl.path[i], gl.path[i - 1]);
        gl.len = gl.cum.back();
        {   // where the stations fall along the path
            size_t si = 0;
            float acc = 0;
            for (size_t i = 0; i < nodes.size(); i++) {
                if (nodes[i].station) {
                    // path index nearest this node
                    size_t bi = 0; float bd = 1e9f;
                    for (size_t k = 0; k < gl.path.size(); k++) {
                        float dd = sq(gl.path[k].x - nodes[i].x) + sq(gl.path[k].z - nodes[i].z);
                        if (dd < bd) { bd = dd; bi = k; }
                    }
                    gl.stationS[si++] = gl.cum[bi];
                }
            }
            (void)acc;
        }
        // cables: one each side of the centerline
        for (size_t i = 0; i + 1 < gl.path.size(); i++) {
            Vector3 a = gl.path[i], b = gl.path[i + 1];
            Vector3 dir = Vector3Normalize(Vector3Subtract(b, a));
            Vector3 side = {-dir.z, 0, dir.x};
            float sl = hypotf(side.x, side.z);
            if (sl > 1e-4f) side = Vector3Scale(side, 1.f / sl);
            for (int sd = -1; sd <= 1; sd += 2) {
                Vector3 o = Vector3Scale(side, CABLE_GAP * sd);
                mb((a.x + b.x) / 2, (a.z + b.z) / 2).beam(Vector3Add(a, o), Vector3Add(b, o), 0.07f, 0.07f, dark);
            }
        }
        // stations and towers
        for (size_t i = 0; i < nodes.size(); i++) {
            Node& n = nodes[i];
            Vector2 toward = i + 1 < nodes.size() ? Vector2{nodes[i + 1].x - n.x, nodes[i + 1].z - n.z} : Vector2{n.x - nodes[i - 1].x, n.z - nodes[i - 1].z};
            float yaw = atan2f(toward.y, toward.x);
            MeshBuilder& m = mb(n.x, n.z);
            if (n.station) {
                // a low concrete terminal with a glass front, a flat roof, and a frame carrying the bullwheel
                float gy = n.gy;
                float base = gy - 1.2f, top = gy + TERMINAL_H;
                addBox(n.x - 6, n.z - 6, n.x + 6, n.z + 6, base, top, PK_PROP, conc, roofC, 0.85f);
                m.obox({n.x, top + 0.2f, n.z}, {7.2f, 0.18f, 7.2f}, yaw, 0, roofC);
                m.obox({n.x + cosf(yaw) * 3.3f, gy + 2.6f, n.z + sinf(yaw) * 3.3f}, {2.9f, 1.1f, 5.4f}, yaw, 0, col(120, 170, 200, M_GLASS)); // window band toward the line
                m.obox({n.x, gy + 0.6f, n.z}, {6.2f, 0.4f, 6.2f}, 0, 0, d.accent);                                                         // base stripe
                for (int sd = -1; sd <= 1; sd += 2) {
                    Vector3 f = {n.x + cosf(yaw + PI / 2) * 2.4f * sd, top + 0.3f, n.z + sinf(yaw + PI / 2) * 2.4f * sd};
                    m.limb(f, {f.x, n.y + n.top, f.z}, 0.3f, 0.2f, steel, 6);
                }
                m.obox({n.x, n.y + n.top, n.z}, {1.2f, 0.25f, 3.0f}, yaw, 0, steel);                                                      // crossarm
            } else {
                float h = n.top, hw = 2.0f + h * 0.02f;
                static const float cx[4] = {-1, 1, 1, -1}, cz[4] = {-1, -1, 1, 1};
                float ca = cosf(yaw), sa = sinf(yaw);
                auto foot = [&](int k, float u) { // corner k at fraction u up the tower (narrows toward the top)
                    float w2 = lerpf(hw, 0.5f, u);
                    float lx = cx[k] * w2, lz = cz[k] * w2;
                    return V3(n.x + lx * ca - lz * sa, n.y - 1.0f + (h + 1.0f) * u, n.z + lx * sa + lz * ca);
                };
                for (int k = 0; k < 4; k++) m.limb(foot(k, 0), foot(k, 1), 0.28f, 0.18f, steel, 6);
                for (int lv = 1; lv < 5; lv++) {   // horizontal ties and diagonals
                    float u0 = lv / 5.f, u1 = (lv + 1) / 5.f;
                    for (int k = 0; k < 4; k++) {
                        m.beam(foot(k, u0), foot((k + 1) % 4, u0), 0.12f, 0.12f, steel);
                        if (lv < 4) m.beam(foot(k, u0), foot((k + 1) % 4, u1), 0.1f, 0.1f, scaleC(steel, 0.85f));
                    }
                }
                m.obox({n.x, n.y + h, n.z}, {1.0f, 0.3f, 3.2f}, yaw, 0, steel);                // crossarm carrying the sheaves
                m.obox({n.x, n.y + h - 0.7f, n.z}, {0.5f, 0.25f, 3.0f}, yaw, 0, d.accent);
                // red and white aviation bands near the top
                for (int k = 0; k < 4; k++) m.beam(foot(k, 0.82f), foot(k, 0.96f), 0.34f, 0.34f, col(220, 60, 50));
                // collision on each leg only, so you can ride through between them (the lowest ties are ~5 m up);
                // each box covers the leg from its foot to the collision top, where it has leaned in a little
                float uTop = 4.f / (h + 1.f);
                for (int k = 0; k < 4; k++) {
                    Vector3 p0 = foot(k, 0), p1 = foot(k, uTop);
                    float r = 0.35f;
                    addPlat({std::min(p0.x, p1.x) - r, std::max(p0.x, p1.x) + r, std::min(p0.z, p1.z) - r, std::max(p0.z, p1.z) + r,
                             n.y + 3.f, n.y + 3.f, n.y - 1.f, 0, PK_POLE});
                }
            }
        }
        gondolas.push_back(gl);
    }
}

// ---------------------------------------------------------------- cabins
void Gondolas::init(const World& w) {
    cabins.clear();
    for (size_t l = 0; l < w.gondolas.size(); l++) {
        float L = w.gondolas[l].len;
        int n = std::max(4, (int)(L / 170.f));
        for (int i = 0; i < n; i++) cabins.push_back({(int)l, 2 * L * i / n, i % 4});
    }
}

static float stationFactor(const GondolaLine& g, float s) {
    float dmin = 1e9f;
    for (float ss : g.stationS) dmin = std::min(dmin, fabsf(s - ss));
    return 0.18f + 0.82f * clampf(dmin / 40.f, 0, 1);
}

void Gondolas::update(float dt, const World& w) {
    for (auto& c : cabins) {
        const GondolaLine& g = w.gondolas[c.line];
        float s = c.u < g.len ? c.u : 2 * g.len - c.u;
        c.u += CABLE_SPEED * stationFactor(g, s) * dt;
        if (c.u >= 2 * g.len) c.u -= 2 * g.len;
    }
}

static Vector3 pathAt(const GondolaLine& g, float s, Vector3* tan) {
    s = clampf(s, 0, g.len - 0.01f);
    size_t i = std::upper_bound(g.cum.begin(), g.cum.end(), s) - g.cum.begin();
    i = std::min(std::max<size_t>(i, 1), g.path.size() - 1);
    float t = (s - g.cum[i - 1]) / std::max(g.cum[i] - g.cum[i - 1], 1e-4f);
    if (tan) *tan = Vector3Normalize(Vector3Subtract(g.path[i], g.path[i - 1]));
    return Vector3Lerp(g.path[i - 1], g.path[i], t);
}

void Gondolas::draw(const World& w, const Vector3& cam) const {
    static const Color TINT[4] = {{236, 236, 238, 255}, {250, 214, 80, 255}, {120, 200, 230, 255}, {240, 130, 110, 255}};
    Color glass = {150, 196, 220, 255}, dark = {44, 46, 52, 255};
    float now = (float)GetTime();
    for (auto& c : cabins) {
        const GondolaLine& g = w.gondolas[c.line];
        bool out = c.u < g.len;
        float s = out ? c.u : 2 * g.len - c.u;
        Vector3 t, p = pathAt(g, s, &t);
        if (sq(p.x - cam.x) + sq(p.z - cam.z) > 2600.f * 2600.f) continue;
        // which cable: out on the right of the direction of travel, back on the other, swinging round at each end
        float lat = CABLE_GAP * (out ? 1.f : -1.f);
        float toEnd = std::min(s, g.len - s);
        float blend = clampf(toEnd / 14.f, 0, 1);
        // around the bullwheel the cabin swings to the other cable
        float side = lat * blend;
        Vector3 perp = {-t.z, 0, t.x};
        float pl = hypotf(perp.x, perp.z);
        if (pl > 1e-4f) perp = Vector3Scale(perp, 1.f / pl);
        p = Vector3Add(p, Vector3Scale(perp, side));
        float yaw = atan2f(t.z, t.x) + (out ? 0.f : PI);
        float sway = 0.04f * sinf(now * 1.3f + c.u * 0.05f);
        rlPushMatrix();
        rlTranslatef(p.x, p.y, p.z);
        rlRotatef(-yaw * RAD2DEG, 0, 1, 0);
        rlRotatef(sway * RAD2DEG, 1, 0, 0);
        DrawCube({0, -0.15f, 0}, 0.7f, 0.3f, 0.5f, dark);                        // grip on the cable
        DrawCube({0, -1.5f, 0}, 0.12f, 2.4f, 0.12f, dark);                       // hanger
        DrawCube({0, -3.3f, 0}, 2.5f, 0.14f, 2.3f, {236, 238, 240, 255});       // roof
        DrawCube({0, -4.2f, 0}, 2.3f, 1.7f, 2.1f, TINT[c.tint]);                 // body
        DrawCube({0, -3.95f, 0}, 2.36f, 0.8f, 2.0f, glass);                      // window band
        DrawCube({0, -4.95f, 0}, 2.2f, 0.14f, 2.0f, g.accent);                   // belt
        // (the body's floor, -4.2 - 0.85, is GONDOLA_CABIN_DROP below the cable)
        rlPopMatrix();
    }
}
