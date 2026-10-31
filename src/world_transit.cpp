// Muni Metro streetcar lines and SFO airport.
#include "world_internal.h"
#include "models.h"

// ---------------------------------------------------------------- streetcar lines
static void drawTramTrack(World& w, Vector2 a, Vector2 b) {
    Vector2 d = Vector2Normalize(Vector2Subtract(b, a)), r = {-d.y, d.x};
    float L = Vector2Distance(a, b);
    Color steel = col(128, 130, 136, M_GROUND), pole = col(52, 60, 56), wire = col(30, 30, 32);
    for (float lane : {-2.4f, 2.4f})
        for (float off : {-0.72f, 0.72f}) {
            Vector2 o = Vector2Scale(r, lane + off);
            roadStrip(w, Vector2Add(a, o), Vector2Add(b, o), 0.06f, 0.06f, 0, 0.105f, steel, 6.f);
        }
    // overhead wire poles
    for (float s = 9; s < L - 5; s += 35)
        for (int side = -1; side <= 1; side += 2) {
            Vector2 p = Vector2Add(Vector2Add(a, Vector2Scale(d, s)), Vector2Scale(r, side * 6.9f));
            float y = w.terrain(p.x, p.y);
            Vector2 tip = Vector2Add(Vector2Add(a, Vector2Scale(d, s)), Vector2Scale(r, side * 1.2f));
            MeshBuilder& m = w.mb(p.x, p.y);
            m.beam({p.x, y - 0.2f, p.y}, {p.x, y + 6.7f, p.y}, 0.2f, 0.2f, pole);
            m.beam({p.x, y + 6.4f, p.y}, {tip.x, w.terrain(tip.x, tip.y) + 6.4f, tip.y}, 0.08f, 0.08f, pole);
        }
    // contact wires above each track
    for (float lane : {-2.4f, 2.4f}) {
        int n = std::max(1, (int)(L / 10));
        for (int k = 0; k < n; k++) {
            Vector2 p0 = Vector2Add(Vector2Add(a, Vector2Scale(d, L * k / n)), Vector2Scale(r, lane));
            Vector2 p1 = Vector2Add(Vector2Add(a, Vector2Scale(d, L * (k + 1) / n)), Vector2Scale(r, lane));
            w.mb(p0.x, p0.y).beam({p0.x, w.terrain(p0.x, p0.y) + 5.9f, p0.y}, {p1.x, w.terrain(p1.x, p1.y) + 5.9f, p1.y}, 0.05f, 0.05f, wire);
        }
    }
}

void World::buildTramLines() {
    struct Line { const char* name; int i0, j0, i1, j1; };
    static const Line lines[] = {
        {"N Judah", 15, 15, 0, 15},     {"L Taraval", 13, 20, 0, 20}, {"J Church", 13, 16, 13, 24},
        {"K Ingleside", 6, 14, 6, 25},  {"T Third", 23, 10, 23, 26},  {"E Embarcadero", 24, 1, 24, 15},
    };
    for (auto& ln : lines) {
        int di = (ln.i1 > ln.i0) - (ln.i1 < ln.i0), dj = (ln.j1 > ln.j0) - (ln.j1 < ln.j0);
        int d = di > 0 ? 0 : di < 0 ? 1 : dj > 0 ? 2 : 3;
        std::vector<Vector2> run;
        auto flush = [&]() {
            if (run.size() >= 3) {
                Path P;
                P.p = {run.front(), run.back()};
                P.finish();
                tramPaths.push_back(P);
                tramNames.push_back(ln.name);
                drawTramTrack(*this, run.front(), run.back());
            }
            run.clear();
        };
        int i = ln.i0, j = ln.j0;
        while (i != ln.i1 || j != ln.j1) {
            if (sf.edge(i, j, d)) {
                if (run.empty()) run.push_back(sf.node(i, j));
                run.push_back(sf.node(i + DI[d], j + DJ[d]));
            } else flush();
            i += DI[d];
            j += DJ[d];
        }
        flush();
    }
    // overhead poles are solid
    for (auto& P : tramPaths) {
        Vector2 a = P.p[0], b = P.p[1];
        Vector2 d = Vector2Normalize(Vector2Subtract(b, a)), r = {-d.y, d.x};
        for (float s = 9; s < P.len - 5; s += 35)
            for (int side = -1; side <= 1; side += 2) {
                Vector2 p = Vector2Add(Vector2Add(a, Vector2Scale(d, s)), Vector2Scale(r, side * 6.9f));
                float y = terrain(p.x, p.y);
                if (doorBlocked(p.x, p.y, 1.3f)) continue;
                addPlat({p.x - 0.12f, p.x + 0.12f, p.y - 0.12f, p.y + 0.12f, y + 6.7f, y + 6.7f, y - 1, 0, PK_POLE});
            }
    }
}

// ---------------------------------------------------------------- SFO
// 5x7 block letters for the hillside sign
static const char* glyph(char c) {
    switch (c) {
    case 'S': return "01111" "10000" "10000" "01110" "00001" "00001" "11110";
    case 'O': return "01110" "10001" "10001" "10001" "10001" "10001" "01110";
    case 'U': return "10001" "10001" "10001" "10001" "10001" "10001" "01110";
    case 'T': return "11111" "00100" "00100" "00100" "00100" "00100" "00100";
    case 'H': return "10001" "10001" "10001" "11111" "10001" "10001" "10001";
    case 'A': return "01110" "10001" "10001" "11111" "10001" "10001" "10001";
    case 'N': return "10001" "11001" "10101" "10011" "10001" "10001" "10001";
    case 'F': return "11111" "10000" "10000" "11110" "10000" "10000" "10000";
    case 'R': return "11110" "10001" "10001" "11110" "10100" "10010" "10001";
    case 'C': return "01111" "10000" "10000" "10000" "10000" "10000" "01111";
    case 'I': return "11111" "00100" "00100" "00100" "00100" "00100" "11111";
    case 'E': return "11111" "10000" "10000" "11110" "10000" "10000" "11111";
    case 'D': return "11110" "10001" "10001" "10001" "10001" "10001" "11110";
    case 'L': return "10000" "10000" "10000" "10000" "10000" "10000" "11111";
    case 'Y': return "10001" "10001" "01010" "00100" "00100" "00100" "00100";
    default: return nullptr;
    }
}

void World::buildAirport() {
    const float y = AIRPORT_Y;
    RNG r(4115);
    Color runway = col(50, 50, 54, M_GROUND), apron = col(168, 166, 160, M_GROUND), white = col(236, 236, 230, M_GROUND);
    Color yellow = col(226, 190, 60, M_GROUND), light = col(255, 236, 190, M_EMISSIVE), blue = col(70, 110, 255, M_EMISSIVE);
    Color conc = col(196, 196, 192), steel = col(150, 154, 160);

    // ---- apron and taxiway
    groundPad(*this, 1500, 1955, 2620, 2125, 0.06f, apron);
    groundPad(*this, 1500, 2099.8f, 2620, 2100.2f, 0.08f, yellow);
    for (float x = 1500; x <= 2620; x += 30) {
        mb(x, 2125).box({x - 0.2f, y, 2125.5f}, {x + 0.2f, y + 0.35f, 2125.9f}, blue);
        if ((int)x % 60 == 0) groundPad(*this, x - 0.1f, 1960, x + 0.1f, 2095, 0.075f, col(150, 148, 142, M_GROUND)); // slab joints
    }

    // ---- parallel runways 28L / 28R
    for (float zc : {2182.f, 2262.f}) {
        groundPad(*this, 1480, zc - 23, 2640, zc + 23, 0.07f, runway);
        for (float x = 1600; x < 2530; x += 60) groundPad(*this, x, zc - 0.45f, x + 30, zc + 0.45f, 0.09f, white);
        for (float side : {-1.f, 1.f}) groundPad(*this, 1480, zc + side * 21.5f - 0.3f, 2640, zc + side * 21.5f + 0.3f, 0.09f, white);
        for (int end = 0; end < 2; end++) {
            float x0 = end ? 2590.f : 1490.f, dir = end ? -1.f : 1.f;
            for (int k = 0; k < 8; k++) { // threshold "piano keys"
                float zz = zc - 18.5f + k * 4.6f + (k >= 4 ? 1.5f : 0.f);
                groundPad(*this, x0, zz, x0 + 40, zz + 1.8f, 0.09f, white);
            }
            float ax = end ? 2640 - 330.f : 1480 + 300.f; // aiming point blocks
            for (float side : {-1.f, 1.f}) groundPad(*this, ax, zc + side * 11 - 4, ax + 45, zc + side * 11 + 4, 0.09f, white);
            for (int k = 1; k <= 3; k++) { // touchdown zone bars
                float tx = (end ? 2640.f : 1480.f) + dir * (150 + k * 150) - (end ? 22.f : 0.f);
                for (float side : {-1.f, 1.f})
                    for (int bar = 0; bar < 4 - k + 1; bar++)
                        groundPad(*this, tx, zc + side * (5 + bar * 2.2f) - 0.8f, tx + 22, zc + side * (5 + bar * 2.2f) + 0.8f, 0.09f, white);
            }
            float bx = end ? 2640.f : 1440.f; // blast pad chevrons
            groundPad(*this, bx, zc - 23, bx + 40, zc + 23, 0.065f, col(70, 70, 72, M_GROUND));
            for (int k = 0; k < 3; k++) {
                float cx = bx + 8 + k * 12;
                groundPad(*this, cx, zc - 18, cx + 1.5f, zc + 18, 0.085f, yellow);
            }
        }
        for (float x = 1480; x <= 2640; x += 30) {
            for (float side : {-1.f, 1.f}) {
                Color c = x < 1500 ? col(60, 230, 90, M_EMISSIVE) : x > 2620 ? col(240, 40, 40, M_EMISSIVE) : light;
                mb(x, zc).box({x - 0.25f, y, zc + side * 23.5f - 0.25f}, {x + 0.25f, y + 0.45f, zc + side * 23.5f + 0.25f}, c);
            }
            mb(x, zc).box({x - 0.3f, y + 0.06f, zc - 0.3f}, {x + 0.3f, y + 0.12f, zc + 0.3f}, light); // centerline lights
        }
        // approach lights marching out into the bay on pylons
        for (float x = 2770; x <= 3240; x += 30) {
            float top = y + 1.0f + (x - 2770) * 0.012f;
            MeshBuilder& m = mb(x, zc);
            m.beam({x, -4, zc}, {x, top, zc}, 0.9f, 0.9f, steel);
            m.beam({x, top, zc - 6}, {x, top, zc + 6}, 0.3f, 0.3f, steel);
            for (int k = -2; k <= 2; k++) m.box({x - 0.4f, top + 0.15f, zc + k * 2.6f - 0.4f}, {x + 0.4f, top + 0.7f, zc + k * 2.6f + 0.4f}, light);
            if (x > 2770) m.beam({x - 30, y + 1.0f + (x - 2800) * 0.012f - 0.4f, zc + 1.2f}, {x, top - 0.4f, zc + 1.2f}, 0.8f, 0.2f, steel);
        }
    }
    // ---- crossing runway 1R/19L
    groundPad(*this, 2470, 1620, 2516, 2500, 0.075f, runway);
    for (float z = 1660; z < 2470; z += 60) groundPad(*this, 2492.5f, z, 2493.5f, z + 30, 0.095f, white);
    for (float z = 1620; z <= 2500; z += 30)
        for (float side : {-1.f, 1.f}) mb(2493, z).box({2493 + side * 24.5f - 0.25f, y, z - 0.25f}, {2493 + side * 24.5f + 0.25f, y + 0.45f, z + 0.25f}, light);

    // ---- rock seawall around the raised airfield
    for (float t = 0; t < 1; t += 0.0015f) {
        // walk the plateau perimeter
        float per = 2 * (2720 - 1360) + 2 * (2560 - 1590), d = t * per, x, z;
        if (d < 1360) { x = 1360 + d; z = 1590; }
        else if (d < 1360 + 970) { x = 2720; z = 1590 + d - 1360; }
        else if (d < 2 * 1360 + 970) { x = 2720 - (d - 1360 - 970); z = 2560; }
        else { x = 1360; z = 2560 - (d - 2 * 1360 - 970); }
        for (int k = 0; k < 3; k++) {
            float ox = x + r.range(-40, 40), oz = z + r.range(-40, 40);
            float h = terrain(ox, oz);
            if (h < -1.5f || h > y - 0.8f) continue;
            float s = r.range(1.0f, 2.4f);
            mb(ox, oz).obox({ox, h + s * 0.3f, oz}, {s, s * 0.6f, s * 0.8f}, r.range(0, 6), r.range(-0.3f, 0.3f), jitterC(col(128, 124, 116), r, 0.15f));
        }
    }

    // ---- highway from the city, with lamps
    std::vector<Vector2> hw = {{1265, 1765}, {1500, 1885}, {2010, 1885}};
    for (size_t k = 0; k + 1 < hw.size(); k++) {
        roadStrip(*this, hw[k], hw[k + 1], 8.5f, 8.5f, 4, 0.08f, C_ASPHALT);
        Vector2 d = Vector2Normalize(Vector2Subtract(hw[k + 1], hw[k])), rr = {-d.y, d.x};
        roadStrip(*this, Vector2Add(hw[k], Vector2Scale(rr, 0.15f)), Vector2Add(hw[k + 1], Vector2Scale(rr, 0.15f)), 0.06f, 0.06f, 0, 0.1f, yellow, 8.f);
        roadStrip(*this, Vector2Subtract(hw[k], Vector2Scale(rr, 0.15f)), Vector2Subtract(hw[k + 1], Vector2Scale(rr, 0.15f)), 0.06f, 0.06f, 0, 0.1f, yellow, 8.f);
        float L = Vector2Distance(hw[k], hw[k + 1]);
        for (float s = 20; s < L; s += 40)
            for (int side = -1; side <= 1; side += 2) {
                Vector2 p = Vector2Add(Vector2Add(hw[k], Vector2Scale(d, s)), Vector2Scale(rr, side * 9.5f));
                if (k == 1 && p.x > 1630 && side > 0) continue; // terminal side is covered by the departures deck
                addLamp(p.x, p.y, atan2f(-rr.y * side, -rr.x * side));
            }
    }
    // cars U-turn at the city end 24 m along the highway, the loop (5.8 m outer lane) clear of the street they
    // meet (x < 1272), rather than out across its cross traffic
    Path P;
    P.p = hw;
    P.p[0] = Vector2Add(hw[0], Vector2Scale(Vector2Normalize(Vector2Subtract(hw[1], hw[0])), 24));
    P.finish();
    carPaths.push_back(P);
    carPathLanes.push_back(2);
    carPathKind.push_back(2);
    groundPad(*this, 1620, 1894, 2000, 1898, 0.12f, C_SIDEWALK);

    // ---- elevated departures roadway along the terminal front
    {
        MeshBuilder& m = mb(1815, 1885);
        float dy = y + 7.5f;
        m.box({1625, dy - 1.2f, 1876}, {1995, dy, 1894}, conc, C_ASPHALT, 1.f);
        m.box({1625, dy, 1876}, {1995, dy + 1.0f, 1876.5f}, conc);
        for (float x = 1640; x < 1990; x += 25)
            for (float z : {1877.5f, 1892.5f}) {
                m.box({x - 0.6f, y - 0.5f, z - 0.6f}, {x + 0.6f, dy - 1.2f, z + 0.6f}, conc);
                addPlat({x - 0.6f, x + 0.6f, z - 0.6f, z + 0.6f, dy, dy, y - 1, 0, PK_POLE});
            }
        addPlat({1625, 1995, 1876, 1894, dy, dy, dy - 1.2f, 0, PK_DECK});
    }

    // ---- terminal with a curved wing roof, parking garage, concourses and gates
    addBox(1680, 1795, 1920, 1874, y - 1, y + 14, PK_BUILDING, col(176, 174, 166, M_OFFICE), col(120, 120, 118));
    addBox(1640, 1898, 1990, 1955, y - 1, y + 17, PK_BUILDING, col(140, 172, 196, M_GLASS), col(205, 205, 210));
    {
        MeshBuilder& m = mb(1815, 1925);
        Color roof = col(226, 228, 232);
        int nseg = 12;
        for (float bx = 1630; bx < 1995; bx += 73) {
            float bx1 = bx + 73;
            for (int k = 0; k < nseg; k++) {
                float z0 = lerpf(1888, 1962, (float)k / nseg), z1 = lerpf(1888, 1962, (float)(k + 1) / nseg);
                float h0 = y + 17 + 7 * sinf(PI * k / nseg), h1 = y + 17 + 7 * sinf(PI * (k + 1) / nseg);
                m.quad({bx, h0, z0}, {bx1, h0, z0}, {bx1, h1, z1}, {bx, h1, z1}, roof);
            }
            m.beam({bx, y, 1897}, {bx, y + 17, 1897}, 0.8f, 0.8f, col(240, 240, 240)); // facade fins
        }
        for (float x = 1645; x < 1990; x += 8) m.box({x - 0.15f, y, 1897.6f}, {x + 0.15f, y + 17, 1898.1f}, col(235, 235, 235));
    }
    static const Color LIVERY[] = {col(30, 60, 140), col(190, 30, 40), col(20, 120, 130), col(230, 120, 30), col(40, 40, 70), col(90, 30, 110)};
    for (float cx : {1700.f, 1815.f, 1930.f}) {
        addBox(cx - 12, 1955, cx + 12, 2060, y - 1, y + 11, PK_BUILDING, col(190, 196, 202, M_GLASS), col(160, 160, 165));
        for (int s = -1; s <= 1; s += 2)
            for (float pz : {2005.f, 2048.f}) {
                float px = cx + s * 34;
                float yaw = s > 0 ? PI : 0;
                addCarToMesh(mb(px, pz), CT_PLANE, LIVERY[r.irange(0, 5)], {px, y, pz}, yaw, 0);
                addPlat({px - 19, px + 19, pz - 2, pz + 2, y + 5.5f, y + 5.5f, y + 1.5f, 0, PK_BUILDING});
                addPlat({px - 4.5f, px + 4.5f, pz - 17, pz + 17, y + 2.8f, y + 2.8f, y + 2.3f, 0, PK_BUILDING});
                float x0 = std::min(cx + s * 12, px - s * 14.5f), x1 = std::max(cx + s * 12, px - s * 14.5f);
                addBox(x0, pz - 4.8f, x1, pz - 2.2f, y + 3.0f, y + 5.8f, PK_BUILDING, col(150, 152, 156), col(150, 152, 156));
                // ground crew: baggage tug + carts, sometimes a fuel truck
                float gx0 = px + s * 6, gz0 = pz + 6;
                MeshBuilder& m = mb(gx0, gz0);
                m.box({gx0 - 1.2f, y, gz0 - 0.8f}, {gx0 + 1.2f, y + 1.4f, gz0 + 0.8f}, col(240, 200, 40));
                for (int c = 1; c <= 3; c++)
                    m.box({gx0 + s * (c * 3.2f) - 1.3f, y + 0.3f, gz0 - 0.9f}, {gx0 + s * (c * 3.2f) + 1.3f, y + 1.6f, gz0 + 0.9f}, col(90, 95, 105));
                addPlat({std::min(gx0, gx0 + s * 11.f) - 1.4f, std::max(gx0, gx0 + s * 11.f) + 1.4f, gz0 - 1, gz0 + 1, y + 1.6f, y + 1.6f, y - 1, 0, PK_CAR});
                if (r.chance(0.4f)) {
                    float fz = pz - 9;
                    m.box({px - 4, y, fz - 1.2f}, {px - 1, y + 2.6f, fz + 1.2f}, col(230, 230, 230));
                    m.prism({px + 3, y + 1.4f, fz}, 10, 1.2f, 1.2f, 0.01f, col(230, 230, 230), col(230, 230, 230));
                    m.obox({px + 3, y + 1.6f, fz}, {4, 1.0f, 1.1f}, 0, 0, col(200, 200, 205));
                    addPlat({px - 4, px + 7, fz - 1.3f, fz + 1.3f, y + 2.6f, y + 2.6f, y - 1, 0, PK_CAR});
                }
            }
    }
    // ---- control tower: slim shaft that flares into a glass cab
    {
        float tx = 2080, tz = 1990;
        MeshBuilder& m = mb(tx, tz);
        Color c = col(214, 212, 206);
        m.prism({tx, y - 1, tz}, 12, 6, 3.6f, 57, c, c, 0, false);
        m.prism({tx, y + 56, tz}, 12, 3.6f, 8.5f, 6, c, c, 0, false);
        m.prism({tx, y + 62, tz}, 12, 8.5f, 9.6f, 7, col(70, 110, 130, M_GLASS), c);
        m.prism({tx, y + 69, tz}, 12, 9.8f, 5.0f, 2.2f, col(225, 225, 225), col(225, 225, 225));
        m.beam({tx, y + 71, tz}, {tx, y + 83, tz}, 0.4f, 0.4f, col(200, 200, 200));
        m.box({tx - 0.5f, y + 83, tz - 0.5f}, {tx + 0.5f, y + 84, tz + 0.5f}, col(255, 30, 20, M_EMISSIVE));
        addPlat({tx - 5, tx + 5, tz - 5, tz + 5, y + 72, y + 72, y - 1, 0, PK_BUILDING});
    }
    // ---- maintenance hangars
    for (int k = 0; k < 3; k++) {
        float x0 = 2200 + k * 130;
        addBox(x0, 1640, x0 + 110, 1740, y - 1, y + 24, PK_BUILDING, col(176, 180, 184), col(150, 152, 156));
        mb(x0 + 55, 1690).gable(x0, x0 + 110, 1640, 1740, y + 24, 6, 0, col(150, 152, 156), col(176, 180, 184));
        mb(x0 + 55, 1742).box({x0 + 10, y - 0.5f, 1740}, {x0 + 100, y + 20, 1740.4f}, col(120, 124, 130));
    }
    // ---- AirTrain guideway, stations, rental car center and the airport hotel
    {
        float gy = y + 10, gzz = 1785;
        MeshBuilder& m = mb(1800, gzz);
        m.box({1440, gy - 1.3f, gzz - 1.6f}, {2185, gy, gzz + 1.6f}, col(232, 232, 228), col(200, 200, 198), 1.f);
        for (float x = 1450; x < 2185; x += 30) {
            m.box({x - 0.8f, y - 0.5f, gzz - 0.8f}, {x + 0.8f, gy - 1.3f, gzz + 0.8f}, col(220, 220, 216));
            addPlat({x - 0.8f, x + 0.8f, gzz - 0.8f, gzz + 0.8f, gy, gy, y - 1, 0, PK_POLE});
        }
        for (float sx : {1640.f, 2000.f}) {
            m.box({sx - 20, gy - 1.5f, gzz - 6}, {sx + 20, gy - 1.3f, gzz + 6}, col(200, 200, 200));
            m.box({sx - 20, gy + 4.2f, gzz - 6}, {sx + 20, gy + 4.6f, gzz + 6}, col(235, 235, 235));
            m.box({sx - 20, gy - 1.3f, gzz + 5.6f}, {sx + 20, gy + 4.2f, gzz + 6}, col(120, 160, 190, M_GLASS));
        }
        addBox(1370, 1690, 1470, 1775, y - 1, y + 20, PK_BUILDING, col(196, 186, 170, M_OFFICE), col(130, 130, 128));
        addBox(2140, 1795, 2200, 1845, y - 1, y + 46, PK_BUILDING, col(150, 176, 190, M_GLASS), col(140, 140, 145));
        mb(2170, 1820).box({2155, y + 46, 1805}, {2185, y + 49, 1835}, col(130, 130, 132));
    }

    // ---- "SOUTH SAN FRANCISCO THE INDUSTRIAL CITY" laid out on Sign Hill, facing the bay
    {
        const char* rows[2] = {"SOUTH SAN FRANCISCO", "THE INDUSTRIAL CITY"};
        // letters lie on the steep east face of the hill; stretched downhill so they read from the airfield
        const float pw = 4.5f, ph = 8.0f;
        Color wc = col(250, 250, 246);
        for (int row = 0; row < 2; row++) {
            int n = (int)strlen(rows[row]);
            float zStart = 2440 + n * 6 * pw / 2;
            float xTop = 1100 + row * 9 * ph;
            for (int ci = 0; ci < n; ci++) {
                const char* g = glyph(rows[row][ci]);
                if (!g) continue;
                for (int gy2 = 0; gy2 < 7; gy2++)
                    for (int gx2 = 0; gx2 < 5; gx2++) {
                        if (g[gy2 * 5 + gx2] != '1') continue;
                        float x = xTop + gy2 * ph, z = zStart - (ci * 6 + gx2) * pw;
                        float h0 = terrain(x - ph * 0.45f, z), h1 = terrain(x + ph * 0.45f, z);
                        Vector3 a = {x - ph * 0.45f, h0 + 0.35f, z}, b = {x + ph * 0.45f, h1 + 0.35f, z};
                        mb(x, z).beam(a, b, pw * 0.9f, 0.6f, wc);
                    }
            }
        }
    }
    // ---- San Mateo Bridge across the South Bay (scenery)
    {
        float zc = 5200, x0 = 2640, x1 = 4360;
        MeshBuilder& m = mbKey(900021);
        auto deckY = [&](float x) { return 9 + 34 * expf(-sq((x - 2900) / 140)); };
        for (float x = x0; x < x1; x += 20) {
            float a = deckY(x), b = deckY(x + 20);
            m.quad({x, a, zc - 8}, {x + 20, b, zc - 8}, {x + 20, b, zc + 8}, {x, a, zc + 8}, col(120, 120, 124));
            m.quad({x, a, zc + 8}, {x + 20, b, zc + 8}, {x + 20, b - 2.5f, zc + 8}, {x, a - 2.5f, zc + 8}, col(170, 170, 168));
            m.quad({x, a, zc - 8}, {x + 20, b, zc - 8}, {x + 20, b - 2.5f, zc - 8}, {x, a - 2.5f, zc - 8}, col(170, 170, 168));
            if ((int)(x - x0) % 60 == 0)
                for (float side : {-5.f, 5.f}) m.box({x - 1, -3, zc + side - 1}, {x + 1, a - 2.5f, zc + side + 1}, col(160, 160, 156));
            if ((int)(x - x0) % 80 == 0) m.box({x - 0.3f, a, zc - 8.3f}, {x + 0.3f, a + 0.8f, zc - 7.7f}, col(255, 220, 160, M_EMISSIVE));
        }
    }
    // ---- Peninsula towns on the hills: South San Francisco, San Bruno, Millbrae, Burlingame
    for (int k = 0; k < 4200; k++) {
        float x = r.range(-1400, 2400), z = r.range(1900, 5200);
        float h = terrain(x, z);
        if (h < 4 || h > 170) continue;
        if (x > 1300 && z < 2640) continue;                         // the airfield
        if (sq(x - 1000) + sq(z - 2450) < sq(320)) continue;        // keep Sign Hill clear
        float rs;
        if (railDistance(x, z, &rs) < 24) continue;                  // Caltrain right of way
        if (x < 1310 && z < 2460 && x > -1400) {                    // Daly City block already has houses
            if (r.chance(0.6f)) continue;
        }
        float slope = fabsf(terrain(x + 6, z) - terrain(x - 6, z)) + fabsf(terrain(x, z + 6) - terrain(x, z - 6));
        if (slope > 8) { if (r.chance(0.5f)) addTree(x, z, r.chance(0.5f) ? 3 : 1, r.range(1.0f, 1.6f), r, false); continue; }
        bool flatland = h < 25 && x > 700;
        Color c = flatland && r.chance(0.5f) ? col(196, 190, 176) : PASTEL[r.irange(0, 11)];
        float w = flatland ? r.range(18, 40) : r.range(7, 11), d = flatland ? r.range(16, 30) : r.range(8, 13);
        float hh = flatland ? r.range(7, 14) : r.range(5, 8);
        mb(x, z).box({x, h - 2, z}, {x + w, h + hh, z + d}, withMat(c, M_PLAIN), flatland ? col(170, 170, 168) : jitterC(col(196, 170, 150), r, 0.1f), 0.8f);
        if (r.chance(0.15f)) addTree(x + w + 3, z + d / 2, 0, r.range(0.9f, 1.3f), r, false);
    }
    Path walk;
    walk.p = {{1650, 1890}, {1985, 1890}};
    walk.finish();
    pedPaths.push_back(walk);
}
