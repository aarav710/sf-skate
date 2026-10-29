// Bridges, landmarks, waterfront, skate spots, teleports, tokens and the generation order.
#include "world_internal.h"
#include "models.h"

void Path::finish() {
    cum.assign(p.size(), 0.f);
    for (size_t i = 1; i < p.size(); i++) cum[i] = cum[i - 1] + Vector2Distance(p[i - 1], p[i]);
    len = cum.empty() ? 0 : cum.back();
}

Vector2 Path::at(float s, Vector2* dir) const {
    s = clampf(s, 0, len);
    size_t k = 1;
    while (k + 1 < p.size() && cum[k] < s) k++;
    float seg = cum[k] - cum[k - 1];
    float t = seg > 0 ? (s - cum[k - 1]) / seg : 0;
    if (dir) *dir = Vector2Normalize(Vector2Subtract(p[k], p[k - 1]));
    return Vector2Lerp(p[k - 1], p[k], t);
}

float World::deckHeightAt(const std::vector<Vector2>& prof, float s) const {
    for (size_t k = 0; k + 1 < prof.size(); k++) {
        float a = prof[k].x, b = prof[k + 1].x;
        if ((s - a) * (s - b) <= 0) return lerpf(prof[k].y, prof[k + 1].y, (s - a) / (b - a));
    }
    return fabsf(s - prof.front().x) < fabsf(s - prof.back().x) ? prof.front().y : prof.back().y;
}

// Build a bridge deck from a height profile (s, h) along an axis.
void World::deckSegments(int axis, float center, float halfW, const std::vector<Vector2>& prof, Color sideCol,
                         Color railCol, const char* railName, int key) {
    Color road = C_ASPHALT, walk = col(150, 148, 144, M_GROUND), stripe = col(230, 230, 225, M_GROUND);
    MeshBuilder& m = mbKey(key);
    auto P = [&](float s, float lat, float y) { return axis == 0 ? V3(s, y, center + lat) : V3(center + lat, y, s); };
    for (size_t k = 0; k + 1 < prof.size(); k++) {
        float s0 = prof[k].x, s1 = prof[k + 1].x, h0 = prof[k].y, h1 = prof[k + 1].y;
        float lo = std::min(s0, s1), hi = std::max(s0, s1);
        float hlo = s0 < s1 ? h0 : h1, hhi = s0 < s1 ? h1 : h0;
        Platform deck, r1, r2;
        if (axis == 0) {
            deck = {lo, hi, center - halfW, center + halfW, hlo, hhi, std::min(h0, h1) - 7, 0, PK_DECK};
            r1 = {lo, hi, center - halfW - 0.4f, center - halfW + 0.05f, hlo + 1.15f, hhi + 1.15f, std::min(h0, h1) - 2, 0, PK_RAILING};
            r2 = {lo, hi, center + halfW - 0.05f, center + halfW + 0.4f, hlo + 1.15f, hhi + 1.15f, std::min(h0, h1) - 2, 0, PK_RAILING};
        } else {
            deck = {center - halfW, center + halfW, lo, hi, hlo, hhi, std::min(h0, h1) - 7, 1, PK_DECK};
            r1 = {center - halfW - 0.4f, center - halfW + 0.05f, lo, hi, hlo + 1.15f, hhi + 1.15f, std::min(h0, h1) - 2, 1, PK_RAILING};
            r2 = {center + halfW - 0.05f, center + halfW + 0.4f, lo, hi, hlo + 1.15f, hhi + 1.15f, std::min(h0, h1) - 2, 1, PK_RAILING};
        }
        addPlat(deck);
        addPlat(r1);
        addPlat(r2);
        float rl = halfW - 0.18f;
        rails.push_back({P(s0, -rl, h0 + 1.15f), P(s1, -rl, h1 + 1.15f), railName});
        rails.push_back({P(s0, rl, h0 + 1.15f), P(s1, rl, h1 + 1.15f), railName});
        // visual sub-segments
        int n = std::max(1, (int)(fabsf(s1 - s0) / 20.f));
        for (int i = 0; i < n; i++) {
            float a = lerpf(s0, s1, (float)i / n), b = lerpf(s0, s1, (float)(i + 1) / n);
            float ya = lerpf(h0, h1, (float)i / n), yb = lerpf(h0, h1, (float)(i + 1) / n);
            m.quad(P(a, -halfW + 3, ya), P(b, -halfW + 3, yb), P(b, halfW - 3, yb), P(a, halfW - 3, ya), road);
            m.quad(P(a, -halfW, ya + 0.12f), P(b, -halfW, yb + 0.12f), P(b, -halfW + 3, yb + 0.12f), P(a, -halfW + 3, ya + 0.12f), walk);
            m.quad(P(a, halfW - 3, ya + 0.12f), P(b, halfW - 3, yb + 0.12f), P(b, halfW, yb + 0.12f), P(a, halfW, ya + 0.12f), walk);
            // sides (truss / girder) and underside
            for (int sd = -1; sd <= 1; sd += 2)
                m.quad(P(a, sd * halfW, ya + 0.12f), P(b, sd * halfW, yb + 0.12f), P(b, sd * halfW, yb - 6), P(a, sd * halfW, ya - 6), sideCol);
            m.quad(P(a, -halfW, ya - 6), P(b, -halfW, yb - 6), P(b, halfW, yb - 6), P(a, halfW, ya - 6), scaleC(sideCol, 0.7f));
            // lane dashes
            for (float lat = -(halfW - 3) + (halfW - 3) / 3; lat < halfW - 3.01f; lat += (halfW - 3) / 3) {
                if (fabsf(lat) < 0.5f) continue;
                float c = (a + b) / 2, yc = (ya + yb) / 2;
                m.quad(P(c - 2, lat - 0.08f, yc + 0.02f), P(c + 2, lat - 0.08f, yc + 0.02f), P(c + 2, lat + 0.08f, yc + 0.02f),
                       P(c - 2, lat + 0.08f, yc + 0.02f), stripe);
            }
            // railings
            for (int sd = -1; sd <= 1; sd += 2) {
                m.beam(P(a, sd * rl, ya + 1.15f), P(b, sd * rl, yb + 1.15f), 0.12f, 0.12f, railCol);
                m.beam(P(a, sd * rl, ya + 0.6f), P(b, sd * rl, yb + 0.6f), 0.06f, 0.06f, railCol);
                for (int q = 0; q < 4; q++) {
                    float s = lerpf(a, b, q / 4.f), y = lerpf(ya, yb, q / 4.f);
                    m.beam(P(s, sd * rl, y + 0.1f), P(s, sd * rl, y + 1.15f), 0.08f, 0.08f, railCol);
                }
            }
            // lamps
            if (i % 3 == 0)
                for (int sd = -1; sd <= 1; sd += 2) {
                    Vector3 base = P(a, sd * (halfW - 0.6f), ya + 0.1f);
                    m.beam(base, {base.x, base.y + 5.5f, base.z}, 0.15f, 0.15f, railCol);
                    m.box({base.x - 0.3f, base.y + 5.3f, base.z - 0.3f}, {base.x + 0.3f, base.y + 5.7f, base.z + 0.3f},
                          col(255, 215, 150, M_EMISSIVE));
                }
        }
    }
}

// parabolic cable between two points with a sag
static void cable(MeshBuilder& m, Vector3 a, Vector3 b, float sag, float thick, Color c, MeshBuilder* susp,
                  float (*deck)(float), int axis, float lat, Color sc, float step = 15.f) {
    float L = axis == 0 ? fabsf(b.x - a.x) : fabsf(b.z - a.z);
    int n = std::max(2, (int)(L / step));
    Vector3 prev = a;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / n;
        Vector3 p = Vector3Lerp(a, b, t);
        p.y -= 4 * sag * t * (1 - t);
        m.beam(prev, p, thick, thick, c);
        if (susp && i < n) {
            float s = axis == 0 ? p.x : p.z;
            float dy = deck(s);
            if (p.y - dy > 1.5f) {
                Vector3 bot = axis == 0 ? V3(p.x, dy + 0.1f, lat) : V3(lat, dy + 0.1f, p.z);
                susp->beam(bot, p, 0.14f, 0.14f, sc);
            }
        }
        prev = p;
    }
}

static std::vector<Vector2> gGG, gBB;
static float ggDeck(float s) {
    for (size_t k = 0; k + 1 < gGG.size(); k++)
        if ((s - gGG[k].x) * (s - gGG[k + 1].x) <= 0) return lerpf(gGG[k].y, gGG[k + 1].y, (s - gGG[k].x) / (gGG[k + 1].x - gGG[k].x));
    return gGG.back().y;
}
static float bbDeck(float s) {
    for (size_t k = 0; k + 1 < gBB.size(); k++)
        if ((s - gBB[k].x) * (s - gBB[k + 1].x) <= 0) return lerpf(gBB[k].y, gBB[k + 1].y, (s - gBB[k].x) / (gBB[k + 1].x - gBB[k].x));
    return gBB.back().y;
}

void World::buildGoldenGate() {
    const float X = -1100, HW = 14;
    Color orange = col(192, 58, 40), conc = col(160, 158, 152);
    // approach road through the Presidio
    {
        Vector2 a = {X, gz(4)}, b = {X, -960};
        float L = Vector2Distance(a, b);
        for (float s = 0; s < L; s += 5) {
            float z0 = a.y - s, z1 = std::max(a.y - s - 5, b.y);
            MeshBuilder& m = mb(X, z0);
            for (int q = 0; q < 2; q++) {
                float l0 = q ? 0 : -9, l1 = q ? 9 : 0;
                m.quad({X + l0, terrain(X + l0, z0) + 0.08f, z0}, {X + l1, terrain(X + l1, z0) + 0.08f, z0},
                       {X + l1, terrain(X + l1, z1) + 0.08f, z1}, {X + l0, terrain(X + l0, z1) + 0.08f, z1}, C_ASPHALT);
            }
        }
    }
    gGG = {{-960, terrain(X, -960) + 0.1f}, {-1100, 30}, {-1700, 36}, {-2300, 30}, {-2410, terrain(X, -2410) + 0.1f}};
    deckSegments(1, X, HW, gGG, orange, orange, "Golden Gate Railing", 900001);
    MeshBuilder& m = mbKey(900002);
    // towers
    for (float tz : {-1250.f, -2130.f}) {
        m.box({X - 30, -10, tz - 14}, {X + 30, 5, tz + 14}, conc, conc, 0.8f);
        addPlat({X - 30, X + 30, tz - 14, tz - 9, 5, 5, -10, 0, PK_BUILDING});
        addPlat({X - 30, X + 30, tz + 9, tz + 14, 5, 5, -10, 0, PK_BUILDING});
        for (int sd = -1; sd <= 1; sd += 2) {
            float lx = X + sd * 19.5f;
            m.box({lx - 4.5f, 5, tz - 5.5f}, {lx + 4.5f, 85, tz + 5.5f}, orange, orange, 0.85f);
            m.box({lx - 3.8f, 85, tz - 4.6f}, {lx + 3.8f, 152, tz + 4.6f}, orange, orange, 1.f);
            m.box({lx - 4.6f, 30, tz - 5.6f}, {lx + 4.6f, 31, tz + 5.6f}, scaleC(orange, 0.85f)); // banding
            addPlat({lx - 4.5f, lx + 4.5f, tz - 5.5f, tz + 5.5f, 152, 152, -10, 0, PK_BUILDING});
            m.box({lx - 0.8f, 152, tz - 0.8f}, {lx + 0.8f, 154, tz + 0.8f}, col(255, 30, 20, M_EMISSIVE));
        }
        for (float py : {22.f, 78.f, 108.f, 134.f, 148.f}) {
            float hgt = py > 140 ? 6 : 4.5f;
            m.box({X - 15.5f, py, tz - 4.0f}, {X + 15.5f, py + hgt, tz + 4.0f}, orange, orange, 0.9f);
        }
    }
    // main cables and suspenders
    MeshBuilder& cm = mbKey(900003);
    for (int sd = -1; sd <= 1; sd += 2) {
        float cx = X + sd * 15.0f;
        Vector3 sAnchor = {cx, ggDeck(-1045) + 4, -1045}, t1 = {cx, 150, -1250}, t2 = {cx, 150, -2130};
        Vector3 nAnchor = {cx, ggDeck(-2330) + 4, -2330};
        cable(cm, sAnchor, t1, 10, 0.9f, orange, &cm, ggDeck, 1, cx, orange);
        cable(cm, t1, t2, 110, 0.9f, orange, &cm, ggDeck, 1, cx, orange);
        cable(cm, t2, nAnchor, 10, 0.9f, orange, &cm, ggDeck, 1, cx, orange);
    }
    // anchorages
    for (float az : {-1045.f, -2330.f}) {
        float y = ggDeck(az);
        m.box({X - 22, terrain(X, az) - 5, az - 12}, {X - 14.5f, y + 4, az + 12}, conc, conc, 0.8f);
        m.box({X + 14.5f, terrain(X, az) - 5, az - 12}, {X + 22, y + 4, az + 12}, conc, conc, 0.8f);
    }
    // piers under the approach spans
    for (float z = -1060; z > -1240; z -= 45) {
        float y = ggDeck(z);
        for (int sd = -1; sd <= 1; sd += 2)
            m.box({X + sd * 10 - 2, terrain(X, z) - 2, z - 2}, {X + sd * 10 + 2, y - 6, z + 2}, orange, orange, 0.8f);
    }
    for (float z = -2150; z > -2320; z -= 45) {
        float y = ggDeck(z);
        for (int sd = -1; sd <= 1; sd += 2)
            m.box({X + sd * 10 - 2, std::min(terrain(X, z), 0.f) - 2, z - 2}, {X + sd * 10 + 2, y - 6, z + 2}, orange, orange, 0.8f);
    }
    // Vista Point plaza on the Marin side
    {
        float y = terrain(X, -2410);
        MeshBuilder& v = mb(X, -2410);
        groundPad(*this, X - 45, -2445, X + 45, -2375, 0.06f, C_PLAZA);
        RNG r(31);
        for (int k = 0; k < 8; k++) {
            float px = X + 22 + (k % 4) * 5, pz = -2430 + (k / 4) * 9;
            Color c = randomCarColor(r);
            addCarToMesh(v, CT_SEDAN, c, {px, terrain(px, pz) + 0.06f, pz}, PI / 2, 0);
            addPlat({px - 0.92f, px + 0.92f, pz - 2.25f, pz + 2.25f, terrain(px, pz) + 1.4f, terrain(px, pz) + 1.4f, y - 1, 0, PK_CAR});
        }
        addRailObj({X - 40, y + 1.0f, -2377}, {X + 10, y + 1.0f, -2377}, "Vista Point Rail");
    }
    Path P;
    P.p = {{X, gz(4)}, {X, -2410}};
    P.onDeck = true;
    P.finish();
    carPaths.push_back(P);
    carPathLanes.push_back(3);
    carPathKind.push_back(0);
    for (int sd = -1; sd <= 1; sd += 2) {
        Path W;
        W.p = {{X + sd * 12.5f, -990}, {X + sd * 12.5f, -2390}};
        W.onDeck = true;
        W.finish();
        pedPaths.push_back(W);
    }
}

void World::buildBayBridge() {
    const float Z = 335, HW = 15;
    Color steel = col(172, 178, 184), conc = col(150, 150, 146), white = col(236, 236, 232);
    Color lights = col(214, 220, 232, M_EMISSIVE);
    gBB = {{1265, terrain(1265, Z) + 0.1f}, {1460, 35}, {1925, 40}, {2450, 36}, {2800, 36}, {3050, 38}, {3600, 22},
           {4080, terrain(4080, Z) + 0.1f}};
    deckSegments(0, Z, HW, gBB, scaleC(steel, 0.8f), conc, "Bay Bridge Railing", 900011);
    MeshBuilder& m = mbKey(900012);
    MeshBuilder& cm = mbKey(900013);
    // western suspension spans
    float towers[] = {1580, 1820, 2030, 2270};
    for (float tx : towers) {
        m.box({tx - 10, -10, Z - 28}, {tx + 10, 4, Z + 28}, conc, conc, 0.8f);
        for (int sd = -1; sd <= 1; sd += 2) {
            float lz = Z + sd * 19.5f;
            m.box({tx - 3.5f, 4, lz - 3.5f}, {tx + 3.5f, 142, lz + 3.5f}, steel, steel, 0.85f);
            addPlat({tx - 3.5f, tx + 3.5f, lz - 3.5f, lz + 3.5f, 142, 142, -10, 0, PK_BUILDING});
            m.box({tx - 0.6f, 142, lz - 0.6f}, {tx + 0.6f, 144, lz + 0.6f}, col(255, 30, 20, M_EMISSIVE));
        }
        for (float y : {20.f, 60.f, 95.f, 125.f, 138.f})
            m.box({tx - 2.5f, y, Z - 16}, {tx + 2.5f, y + 3.5f, Z + 16}, steel, steel, 0.9f);
        for (float y0 : {60.f, 95.f}) { // X bracing
            m.beam({tx, y0 + 3.5f, Z - 16}, {tx, y0 + 33, Z + 16}, 1.2f, 1.2f, steel);
            m.beam({tx, y0 + 3.5f, Z + 16}, {tx, y0 + 33, Z - 16}, 1.2f, 1.2f, steel);
        }
    }
    // center anchorage + end anchorages
    for (float ax : {1460.f, 1925.f, 2450.f}) {
        float y = bbDeck(ax);
        float top = ax == 1925 ? y + 22 : y + 5;
        m.box({ax - 20, -10, Z - 25}, {ax + 20, y - 6, Z + 25}, conc, conc, 0.8f);
        for (int sd = -1; sd <= 1; sd += 2)
            m.box({ax - 20, y - 6, sd > 0 ? Z + 15.5f : Z - 25}, {ax + 20, top, sd > 0 ? Z + 25 : Z - 15.5f}, conc, conc, 0.85f);
    }
    for (int sd = -1; sd <= 1; sd += 2) {
        float cz = Z + sd * 16.0f;
        auto C = [&](float x, float y) { return V3(x, y, cz); };
        cable(cm, C(1460, bbDeck(1460) + 5), C(1580, 140), 6, 0.8f, steel, &cm, bbDeck, 0, cz, lights);
        cable(cm, C(1580, 140), C(1820, 140), 95, 0.8f, steel, &cm, bbDeck, 0, cz, lights);
        cable(cm, C(1820, 140), C(1925, bbDeck(1925) + 22), 6, 0.8f, steel, &cm, bbDeck, 0, cz, lights);
        cable(cm, C(1925, bbDeck(1925) + 22), C(2030, 140), 6, 0.8f, steel, &cm, bbDeck, 0, cz, lights);
        cable(cm, C(2030, 140), C(2270, 140), 95, 0.8f, steel, &cm, bbDeck, 0, cz, lights);
        cable(cm, C(2270, 140), C(2450, bbDeck(2450) + 5), 8, 0.8f, steel, &cm, bbDeck, 0, cz, lights);
    }
    // eastern self-anchored span: one white tower
    {
        float tx = 3050;
        m.box({tx - 12, -10, Z - 30}, {tx + 12, 4, Z + 30}, conc, conc, 0.8f);
        for (int sd = -1; sd <= 1; sd += 2) {
            float lz = Z + sd * 19.0f;
            m.box({tx - 3.0f, 4, lz - 3.0f}, {tx + 3.0f, 160, lz + 3.0f}, white, white, 0.9f);
            addPlat({tx - 3, tx + 3, lz - 3, lz + 3, 160, 160, -10, 0, PK_BUILDING});
            for (float y : {50.f, 100.f, 150.f})
                m.box({tx - 2, y, Z - 19}, {tx + 2, y + 2.5f, Z + 19}, white, white, 0.9f);
            float cz = Z + sd * 16.0f;
            cable(cm, V3(2860, bbDeck(2860) + 1, cz), V3(tx, 158, cz), -25, 0.9f, white, &cm, bbDeck, 0, cz, white);
            cable(cm, V3(tx, 158, cz), V3(3330, bbDeck(3330) + 1, cz), -25, 0.9f, white, &cm, bbDeck, 0, cz, white);
        }
    }
    // piers
    for (float x = 1320; x < 4040; x += 60) {
        if (x > 2440 && x < 2810) continue; // over Yerba Buena Island
        bool near = false;
        for (float tx : towers) near |= fabsf(x - tx) < 25;
        if (fabsf(x - 1925) < 30 || fabsf(x - 1460) < 30 || fabsf(x - 3050) < 30) near = true;
        if (near) continue;
        float y = bbDeck(x);
        float g = std::min(terrain(x, Z), 0.f) - 2;
        for (int sd = -1; sd <= 1; sd += 2)
            m.box({x - 2, g, Z + sd * 9 - 2}, {x + 2, y - 6, Z + sd * 9 + 2}, conc, conc, 0.8f);
    }
    Path P;
    P.p = {{1265, Z}, {4080, Z}};
    P.onDeck = true;
    P.finish();
    carPaths.push_back(P);
    carPathLanes.push_back(3);
    carPathKind.push_back(0);
}

// ---------------------------------------------------------------- landmarks
static const Color LADIES[] = {col(110, 160, 200), col(236, 160, 170), col(244, 220, 120), col(140, 190, 150),
                               col(190, 150, 200), col(240, 230, 210), col(150, 200, 225)};

void World::buildLandmarks() {
    RNG r(8080);
    // --- Transamerica Pyramid
    {
        float cx = (gx(21) + gx(22)) / 2, cz = (gz(5) + gz(6)) / 2, y = terrain(cx, cz) - 1;
        MeshBuilder& m = mb(cx, cz);
        Color wc = col(236, 233, 226, M_OFFICE);
        float b = 18, H = 170;
        Vector3 apex = {cx, y + H, cz};
        Vector3 c[4] = {{cx - b, y, cz - b}, {cx + b, y, cz - b}, {cx + b, y, cz + b}, {cx - b, y, cz + b}};
        for (int i = 0; i < 4; i++) m.tri(c[i], c[(i + 1) % 4], apex, wc);
        for (int sd = -1; sd <= 1; sd += 2) { // elevator "wings"
            float wy0 = y + 95, wy1 = y + 178;
            float off = b * (1 - 95 / H);
            m.box({cx + sd * off - (sd > 0 ? 0 : 5), wy0, cz - 4}, {cx + sd * off + (sd > 0 ? 5 : 0), wy1, cz + 4}, wc, col(220, 218, 210), 1.f);
        }
        m.beam({cx, y + H - 5, cz}, {cx, y + H + 35, cz}, 0.8f, 0.8f, col(236, 233, 226));
        m.box({cx - 0.5f, y + H + 35, cz - 0.5f}, {cx + 0.5f, y + H + 37, cz + 0.5f}, col(255, 40, 30, M_EMISSIVE));
        addPlat({cx - b + 3, cx + b - 3, cz - b + 3, cz + b - 3, y + 120, y + 120, y, 0, PK_BUILDING});
        for (int k = 0; k < 6; k++) addTree(cx - 40 + k * 4, cz + 30, 0, 0.9f, r, true);
    }
    // --- Salesforce Tower (rounded glass obelisk with a glowing crown)
    {
        float cx = (gx(22) + gx(23)) / 2, cz = (gz(11) + gz(12)) / 2, y = terrain(cx, cz) - 1;
        MeshBuilder& m = mb(cx, cz);
        Color g = col(178, 190, 198, M_GLASS);
        m.prism({cx, y, cz}, 16, 23, 18, 200, g, g, 0.2f, false);
        m.prism({cx, y + 200, cz}, 16, 18, 15, 20, col(220, 225, 235, M_EMISSIVE), col(90, 90, 95), 0.2f, true);
        addPlat({cx - 18, cx + 18, cz - 18, cz + 18, y + 220, y + 220, y, 0, PK_BUILDING});
        m.box({cx - 40, y + 0.9f, cz + 25}, {cx + 40, y + 1.4f, cz + 40}, col(90, 140, 70)); // rooftop-park nod
    }
    // --- Coit Tower on Telegraph Hill
    {
        float cx = 880, cz = -880, y = terrain(cx, cz);
        MeshBuilder& m = mb(cx, cz);
        Color w = col(226, 220, 204, M_HOUSE);
        m.box({cx - 14, y - 2, cz - 14}, {cx + 14, y + 5, cz + 14}, col(212, 204, 186), col(150, 140, 120), 0.8f);
        m.prism({cx, y + 5, cz}, 16, 6, 5.4f, 52, w, w, 0, false);
        m.prism({cx, y + 57, cz}, 16, 6.2f, 6.2f, 3, col(220, 214, 196), col(200, 195, 180), 0, true);
        addPlat({cx - 14, cx + 14, cz - 14, cz + 14, y + 5, y + 5, y - 2, 0, PK_BUILDING});
        addPlat({cx - 5, cx + 5, cz - 5, cz + 5, y + 60, y + 60, y, 0, PK_BUILDING});
        groundPad(*this, cx - 30, cz + 14, cx + 30, cz + 40, 0.07f, C_PLAZA);
    }
    // --- City Hall with its golden dome
    {
        float cx = (gx(14) + gx(15)) / 2, cz = (gz(13) + gz(14)) / 2, y = terrain(cx, cz) - 1;
        MeshBuilder& m = mb(cx, cz);
        Color w = col(216, 212, 202, M_HOUSE), roof = col(170, 168, 160);
        addBox(cx - 45, cz - 32, cx + 45, cz + 32, y - 1, y + 24, PK_BUILDING, w, roof);
        m.box({cx - 20, y + 0.5f, cz - 38}, {cx + 20, y + 20, cz - 32}, col(225, 220, 210), roof, 0.9f); // portico
        for (int k = 0; k < 8; k++) m.prism({cx - 17.5f + k * 5, y + 0.5f, cz - 39}, 8, 0.8f, 0.8f, 19, col(235, 232, 225), w);
        m.prism({cx, y + 24, cz}, 16, 15, 15, 14, w, w);
        Color gold = col(212, 172, 72);
        m.sphere({cx, y + 38, cz}, 15, 12, 18, gold, true);
        m.prism({cx, y + 52, cz}, 8, 2.5f, 2.5f, 6, gold, gold);
        m.prism({cx, y + 58, cz}, 8, 2.5f, 0.1f, 5, gold, gold);
    }
    // --- Palace of Fine Arts
    {
        float cx = -830, cz = -1010, y = terrain(cx, cz);
        MeshBuilder& m = mb(cx, cz);
        Color peach = col(222, 182, 150), dome = col(196, 130, 100);
        for (int k = 0; k < 8; k++) {
            float a = TAU_F * k / 8;
            float px = cx + cosf(a) * 15, pz = cz + sinf(a) * 15;
            m.prism({px, y - 0.5f, pz}, 8, 1.6f, 1.4f, 20, peach, peach);
            addPlat({px - 1.5f, px + 1.5f, pz - 1.5f, pz + 1.5f, y + 20, y + 20, y - 1, 0, PK_POLE});
        }
        m.prism({cx, y + 20, cz}, 16, 17, 16, 4, peach, peach);
        m.sphere({cx, y + 24, cz}, 15.5f, 12, 18, dome, true);
        for (int k = 0; k < 24; k++) { // colonnade arc
            float a = 2.4f + k * 0.075f;
            float px = cx + cosf(a) * 55, pz = cz + sinf(a) * 55 + 10;
            float py = terrain(px, pz);
            m.prism({px, py - 0.5f, pz}, 6, 1.0f, 1.0f, 12, peach, peach);
            m.box({px - 1.5f, py + 12, pz - 1.5f}, {px + 1.5f, py + 13.5f, pz + 1.5f}, peach);
            addPlat({px - 1, px + 1, pz - 1, pz + 1, py + 12, py + 12, py - 1, 0, PK_POLE});
        }
        ponds.push_back({cx + 25, cz - 30, cx + 90, cz + 25});
        float py = terrain(cx + 55, cz) - 0.3f;
        m.quad({cx + 25, py, cz - 30}, {cx + 90, py, cz - 30}, {cx + 90, py, cz + 25}, {cx + 25, py, cz + 25}, col(50, 100, 110, M_WATER));
    }
    // --- Sutro Tower
    {
        float cx = -470, cz = 770, y = terrain(cx, cz);
        MeshBuilder& m = mb(cx, cz);
        Color red = col(196, 52, 42), wht = col(232, 230, 226);
        Vector3 top = {cx, y + 140, cz};
        for (int k = 0; k < 3; k++) {
            float a = TAU_F * k / 3 + 0.4f;
            Vector3 b = {cx + cosf(a) * 20, y - 1, cz + sinf(a) * 20};
            Vector3 t = {cx + cosf(a) * 4, y + 140, cz + sinf(a) * 4};
            for (int s = 0; s < 7; s++)
                m.beam(Vector3Lerp(b, t, s / 7.f), Vector3Lerp(b, t, (s + 1) / 7.f), 1.6f, 1.6f, (s % 2) ? wht : red);
            Vector3 mt = {cx + cosf(a) * 4, y + 140, cz + sinf(a) * 4};
            for (int s = 0; s < 3; s++)
                m.beam({mt.x, y + 140 + s * 20, mt.z}, {mt.x, y + 160 + s * 20, mt.z}, 1.0f, 1.0f, (s % 2) ? red : wht);
            addPlat({b.x - 1.5f, b.x + 1.5f, b.z - 1.5f, b.z + 1.5f, y + 50, y + 50, y - 2, 0, PK_POLE});
            for (float hy : {100.f, 140.f, 165.f}) {
                float a2 = a + TAU_F / 3;
                m.beam({cx + cosf(a) * (hy > 120 ? 14 : 8), y + hy, cz + sinf(a) * (hy > 120 ? 14 : 8)},
                       {cx + cosf(a2) * (hy > 120 ? 14 : 8), y + hy, cz + sinf(a2) * (hy > 120 ? 14 : 8)}, 0.8f, 0.8f, red);
            }
        }
        m.box({top.x - 1, y + 200, top.z - 1}, {top.x + 1, y + 202, top.z + 1}, col(255, 30, 20, M_EMISSIVE));
    }
    // --- Painted Ladies on Steiner St, facing Alamo Square
    {
        float x0 = gx(10) + BLOCK_INSET, x1 = x0 + 17;
        for (int k = 0; k < 7; k++) {
            float z0 = gz(11) + BLOCK_INSET + 2 + k * 12.4f, z1 = z0 + 12.2f;
            Color c = withMat(LADIES[k], M_HOUSE);
            addBuilding(x0, z0, x1, z1, 10.5f, c, col(80, 70, 70), BS_LADY, 2, r);
        }
        // the rest of the block: regular rowhouses on the other sides
        float bz0 = gz(11) + BLOCK_INSET, bz1 = gz(12) - BLOCK_INSET, bx1 = gx(11) - BLOCK_INSET;
        for (float z = bz0; z < bz1 - 8; z += 9.5f) addBuilding(bx1 - 20, z, bx1, z + 9.3f, r.range(9, 13), withMat(PASTEL[r.irange(0, 11)], M_HOUSE), col(92, 88, 86), BS_ROWHOUSE, 3, r);
        for (float x = x1 + 1; x < bx1 - 21; x += 9.5f) {
            addBuilding(x, bz0, x + 9.3f, bz0 + 20, r.range(9, 13), withMat(PASTEL[r.irange(0, 11)], M_HOUSE), col(92, 88, 86), BS_ROWHOUSE, 0, r);
            addBuilding(x, bz1 - 20, x + 9.3f, bz1, r.range(9, 13), withMat(PASTEL[r.irange(0, 11)], M_HOUSE), col(92, 88, 86), BS_ROWHOUSE, 1, r);
        }
    }
    // --- Golden Gate Park: Dutch Windmill, Conservatory of Flowers, de Young tower
    {
        float cx = -1440, cz = 260, y = terrain(cx, cz);
        MeshBuilder& m = mb(cx, cz);
        Color w = col(236, 230, 214);
        m.prism({cx, y - 1, cz}, 8, 6, 4, 22, w, w, 0, false);
        m.prism({cx, y + 21, cz}, 8, 4.5f, 0.5f, 5, col(90, 80, 70), col(90, 80, 70));
        Vector3 hub = {cx + 5, y + 21, cz};
        for (int k = 0; k < 4; k++) {
            float a = PI / 4 + k * PI / 2;
            m.beam(hub, {hub.x + 0.3f, hub.y + sinf(a) * 14, hub.z + cosf(a) * 14}, 2.2f, 0.25f, col(130, 110, 90));
        }
        addPlat({cx - 5, cx + 5, cz - 5, cz + 5, y + 22, y + 22, y - 1, 0, PK_BUILDING});
        // conservatory
        float kx = -560, kz = 255;
        y = terrain(kx, kz);
        Color wh = col(245, 245, 240, M_GLASS);
        addBox(kx - 30, kz - 6, kx + 30, kz + 6, y - 1, y + 7, PK_BUILDING, wh, wh);
        mb(kx, kz).sphere({kx, y + 7, kz}, 9, 10, 14, wh, true);
        // de Young copper tower
        float dx = -760, dz = 300;
        y = terrain(dx, dz);
        Color cu = col(150, 100, 66, M_OFFICE);
        addBox(dx - 50, dz - 20, dx + 20, dz + 20, y - 1, y + 12, PK_BUILDING, cu, col(120, 85, 60));
        mb(dx, dz).obox({dx - 30, y + 24, dz}, {9, 24, 9}, 0.25f, 0, cu);
    }
    // --- Oracle Park
    {
        Color brick = col(150, 72, 56), green = col(60, 130, 60, M_GROUND), dirt = col(170, 120, 80, M_GROUND);
        float y = 3.0f;
        MeshBuilder& m = mb(1225, 775);
        m.quad({1175, y + 0.05f, 700}, {1300, y + 0.05f, 700}, {1300, y + 0.05f, 852}, {1175, y + 0.05f, 852}, green);
        Vector3 hp = {1190, y + 0.08f, 838};
        m.quad(hp, {hp.x + 19, hp.y, hp.z - 6}, {hp.x + 25, hp.y, hp.z - 25}, {hp.x + 6, hp.y, hp.z - 19}, dirt);
        addBox(1158, 684, 1175, 868, y - 1, y + 22, PK_BUILDING, brick, col(60, 90, 70));
        addBox(1175, 852, 1300, 868, y - 1, y + 22, PK_BUILDING, brick, col(60, 90, 70));
        addBox(1175, 684, 1225, 698, y - 1, y + 18, PK_BUILDING, brick, col(60, 90, 70));
        addBox(1245, 684, 1300, 698, y - 1, y + 18, PK_BUILDING, brick, col(60, 90, 70));
        addBox(1300, 690, 1312, 868, y - 1, y + 3, PK_PROP, col(40, 80, 60), col(40, 80, 60));
        m.slab(1175, 1195, 698, 852, y + 18, y + 2, 0, y, col(40, 90, 70), brick);
        m.box({1280, y, 702}, {1298, y + 26, 708}, col(30, 30, 34), col(30, 30, 34));       // scoreboard
        m.prism({1290, y + 3, 715}, 10, 2.5f, 1.2f, 18, col(190, 30, 30), col(190, 30, 30));  // the giant bottle
        m.sphere({1290, y + 12, 735}, 6, 8, 10, col(140, 90, 50));                           // the giant glove
        for (Vector2 t : {Vector2{1165, 690}, Vector2{1165, 862}, Vector2{1295, 860}})
            m.box({t.x - 0.7f, y, t.y - 0.7f}, {t.x + 0.7f, y + 40, t.y + 0.7f}, col(80, 80, 85)),
            m.box({t.x - 3, y + 38, t.y - 3}, {t.x + 3, y + 42, t.y + 3}, col(255, 250, 220, M_EMISSIVE));
    }
    // --- Alcatraz
    {
        float cx = 300, cz = -1905, y = terrain(cx, cz);
        MeshBuilder& m = mb(cx, cz);
        m.box({cx - 40, y - 2, cz - 12}, {cx + 40, y + 13, cz + 12}, col(200, 195, 184, M_HOUSE), col(150, 145, 138), 0.8f);
        m.prism({cx + 50, y - 1, cz + 20}, 8, 2.6f, 2.2f, 26, col(240, 238, 230), col(240, 238, 230));
        m.box({cx + 48.5f, y + 25, cz + 18.5f}, {cx + 51.5f, y + 28, cz + 21.5f}, col(255, 240, 200, M_EMISSIVE));
        m.prism({cx - 55, y + 12, cz - 10}, 10, 5, 5, 8, col(150, 140, 120), col(150, 140, 120));
        for (int k = 0; k < 4; k++) m.beam({cx - 55 + (k & 1 ? 4.f : -4.f), y - 1, cz - 10 + (k & 2 ? 4.f : -4.f)}, {cx - 55, y + 12, cz - 10}, 0.5f, 0.5f, col(120, 110, 100));
        m.box({cx - 70, terrain(cx - 60, cz + 50) - 1, cz + 40}, {cx - 30, terrain(cx - 60, cz + 50) + 8, cz + 58}, col(210, 200, 180, M_HOUSE), col(120, 110, 100));
    }
    // --- Treasure Island hangars
    for (int k = 0; k < 3; k++) {
        float cx = 2550 + k * 100, cz = -480, y = terrain(cx, cz);
        mb(cx, cz).box({cx - 40, y - 1, cz - 30}, {cx + 40, y + 24, cz + 30}, col(170, 172, 168), col(140, 140, 138), 0.8f);
    }
    {
        float cx = 2620, cz = -40, y = terrain(cx, cz);
        mb(cx, cz).box({cx - 50, y - 1, cz - 12}, {cx + 50, y + 14, cz + 12}, col(232, 222, 196, M_HOUSE), col(150, 140, 120), 0.8f);
    }
    // --- Port of Oakland cranes and containers
    {
        Color cw = col(230, 230, 226), cr = col(200, 50, 40), cb = col(60, 90, 160);
        static const Color CONT[] = {col(180, 50, 40), col(40, 90, 150), col(50, 120, 70), col(200, 140, 40), col(130, 130, 135), col(150, 70, 110)};
        for (float z = 500; z <= 1300; z += 130) {
            float y = terrain(4005, z);
            MeshBuilder& m = mb(4005, z);
            Color c = (int)z % 260 == 0 ? cb : cw;
            for (int k = 0; k < 4; k++) {
                float lx = 4005 + (k & 1 ? 12.f : -12.f), lz = z + (k & 2 ? 9.f : -9.f);
                m.beam({lx, y - 1, lz}, {lx, y + 42, lz}, 1.4f, 1.4f, c);
                addPlat({lx - 0.7f, lx + 0.7f, lz - 0.7f, lz + 0.7f, y + 42, y + 42, y - 1, 0, PK_POLE});
            }
            m.box({3995 - 60, y + 42, z - 3}, {4005 + 30, y + 46, z + 3}, c, c, 1.f);
            m.beam({4005, y + 46, z}, {4005, y + 66, z}, 1.2f, 1.2f, c);
            m.beam({4005, y + 66, z}, {3940, y + 46, z}, 0.4f, 0.4f, c);
            m.beam({4005, y + 66, z}, {4035, y + 46, z}, 0.4f, 0.4f, c);
            m.box({4000, y + 46, z - 5}, {4012, y + 52, z + 5}, cr, cr, 1.f);
        }
        RNG cr2(77);
        for (float z = 480; z < 1320; z += 14) {
            if (cr2.chance(0.25f)) continue;
            for (float x = 4030; x < 4068; x += 3.0f) {
                if (cr2.chance(0.2f)) continue;
                int stack = cr2.irange(1, 4);
                float y = terrain(x, z);
                for (int s = 0; s < stack; s++)
                    mb(x, z).box({x, y + s * 2.6f, z}, {x + 2.5f, y + (s + 1) * 2.6f - 0.05f, z + 12}, CONT[cr2.irange(0, 5)], CONT[cr2.irange(0, 5)], 0.85f);
                addPlat({x, x + 2.5f, z, z + 12, y + stack * 2.6f, y + stack * 2.6f, y - 1, 0, PK_BUILDING});
            }
        }
    }
}

// ---------------------------------------------------------------- waterfront
void World::buildWaterfront() {
    RNG r(3939);
    Color wood = col(140, 112, 82, M_GROUND), pale = col(222, 216, 200, M_HOUSE), roof = col(120, 120, 118);
    // Ferry Building with its clock tower
    {
        float y = terrain(1290, -200);
        Color w = col(226, 216, 196, M_HOUSE);
        addBox(1282, -292, 1312, -108, y - 3, y + 15, PK_BUILDING, w, roof);
        MeshBuilder& m = mb(1296, -200);
        m.box({1289, y + 15, -207}, {1303, y + 46, -193}, w, w, 1.f);
        m.box({1290.5f, y + 46, -205.5f}, {1301.5f, y + 62, -194.5f}, w, w, 1.f);
        m.box({1292, y + 62, -204}, {1300, y + 72, -196}, w, w, 1.f);
        Color face = col(250, 248, 235, M_EMISSIVE);
        m.box({1288.8f, y + 50, -203}, {1303.2f, y + 57, -197}, face, face, 1.f);
        m.box({1292.5f, y + 50, -205.7f}, {1299.5f, y + 57, -194.3f}, face, face, 1.f);
        m.prism({1296, y + 72, -200}, 4, 6, 0.3f, 10, w, w, PI / 4);
        addPlat({1289, 1303, -207, -193, y + 72, y + 72, y, 0, PK_BUILDING});
    }
    // Piers along the Embarcadero
    for (float zc : {-800.f, -660.f, -520.f, -380.f, 50.f, 180.f, 480.f}) {
        float y = terrain(1290, zc) + 0.02f;
        float x0 = 1290, x1 = 1480;
        MeshBuilder& m = mb(1380, zc);
        m.box({x0, -3, zc - 15}, {x1, y, zc + 15}, col(110, 100, 90), wood, 0.7f);
        addPlat({x0, x1, zc - 15, zc + 15, y, y, -4, 0, PK_PIER});
        for (float x = x0 + 10; x < x1; x += 15)
            for (int sd = -1; sd <= 1; sd += 2) m.beam({x, -4, zc + sd * 14}, {x, y - 0.5f, zc + sd * 14}, 0.6f, 0.6f, col(80, 70, 60));
        addBox(x0 + 12, zc - 11, x1 - 8, zc + 11, y - 0.5f, y + 11, PK_BUILDING, pale, roof);
        addBox(x0 + 4, zc - 13, x0 + 12, zc + 13, y - 0.5f, y + 14, PK_BUILDING, col(236, 230, 214, M_HOUSE), col(200, 195, 185));
        m.gable(x0 + 12, x1 - 8, zc - 11, zc + 11, y + 11, 3.5f, 0, roof, pale);
    }
    // Pier 39
    {
        float x0 = 990, x1 = 1060, z0 = -1300, z1 = -1105;
        float y = std::max(terrain(1025, -1112), 2.0f) + 0.02f;
        MeshBuilder& m = mb(1025, -1200);
        m.box({x0, -3, z0}, {x1, y, z1}, col(110, 100, 90), wood, 0.7f);
        addPlat({x0, x1, z0, z1, y, y, -4, 0, PK_PIER});
        for (float z = z0 + 8; z < z1 - 20; z += 18)
            for (int sd = -1; sd <= 1; sd += 2) {
                float sx0 = sd < 0 ? x0 + 2 : x1 - 16, sx1 = sx0 + 14;
                Color c = withMat(jitterC(col(150, 110, 80), r, 0.15f), M_HOUSE);
                addBox(sx0, z, sx1, z + 15, y, y + r.range(6, 9), PK_BUILDING, c, col(100, 70, 50));
            }
        m.prism({1025, y, z0 + 15}, 12, 7, 7, 4, col(240, 220, 200), col(200, 60, 60));
        m.prism({1025, y + 4, z0 + 15}, 12, 8, 0.3f, 4, col(200, 60, 60), col(200, 60, 60));
        addPlat({1018, 1032, z0 + 8, z0 + 22, y + 8, y + 8, y, 0, PK_BUILDING});
        for (int k = 0; k < 6; k++) { // sea lion docks
            float dx = x0 - 30 + (k % 3) * 9, dz = z0 + 30 + (k / 3) * 12;
            m.box({dx, -0.2f, dz}, {dx + 7, 0.4f, dz + 9}, col(120, 105, 85));
            for (int s = 0; s < 4; s++) {
                float sx = dx + r.range(1, 6), sz = dz + r.range(1, 8);
                m.obox({sx, 0.6f, sz}, {0.9f, 0.25f, 0.35f}, r.range(0, 6), 0, col(70, 55, 45));
            }
        }
    }
    // Hyde Street Pier with the Balclutha tall ship
    {
        float y = std::max(terrain(470, -1130), 2.0f);
        addBox(462, -1270, 476, -1125, -3, y, PK_PIER, col(110, 100, 90), wood);
        MeshBuilder& m = mb(495, -1200);
        m.obox({495, 1.5f, -1200}, {35, 3.5f, 5.5f}, -PI / 2, 0, col(28, 28, 30));
        m.obox({495, 5.1f, -1200}, {34, 0.2f, 5.2f}, -PI / 2, 0, col(150, 120, 90));
        for (int k = -1; k <= 1; k++) {
            float mz = -1200 + k * 18;
            m.beam({495, 5, mz}, {495, 40, mz}, 0.6f, 0.6f, col(110, 85, 60));
            for (float yy : {16.f, 25.f, 33.f}) m.beam({488, yy, mz}, {502, yy, mz}, 0.35f, 0.35f, col(110, 85, 60));
        }
    }
    // Ghirardelli Square sign building
    {
        float x = 300, z = -1060, y = terrain(x, z);
        addBox(x - 20, z - 12, x + 20, z + 12, y - 1, y + 14, PK_BUILDING, col(170, 80, 60, M_HOUSE), col(110, 60, 50));
        mb(x, z).box({x - 14, y + 14, z - 1}, {x + 14, y + 18, z + 1}, col(255, 240, 200, M_EMISSIVE));
    }
}

// ---------------------------------------------------------------- skate spots
void World::addRamp(float x0, float z0, float x1, float z1, float rise, int axis, bool up) {
    float y = terrain((x0 + x1) / 2, (z0 + z1) / 2);
    float lo = y, hi = y + rise;
    Platform p{x0, x1, z0, z1, up ? lo : hi, up ? hi : lo, y - 0.4f, (uint8_t)axis, PK_PROP};
    addPlat(p);
    mb((x0 + x1) / 2, (z0 + z1) / 2).slab(x0, x1, z0, z1, p.h0, p.h1, axis, y - 0.4f, col(184, 152, 112, M_GROUND), col(120, 120, 124));
    // coping rail on the high edge
    if (axis == 0) {
        float ex = up ? x1 : x0;
        rails.push_back({{ex, hi, z0 + 0.1f}, {ex, hi, z1 - 0.1f}, "Coping"});
    } else {
        float ez = up ? z1 : z0;
        rails.push_back({{x0 + 0.1f, hi, ez}, {x1 - 0.1f, hi, ez}, "Coping"});
    }
}

void World::addLedge(float x0, float z0, float x1, float z1, float h, Color c) {
    float y = terrain((x0 + x1) / 2, (z0 + z1) / 2);
    addBox(x0, z0, x1, z1, y - 0.3f, y + h, PK_PROP, c, scaleC(c, 1.05f), 0.85f);
    if (x1 - x0 > z1 - z0) {
        rails.push_back({{x0, y + h, z0 + 0.04f}, {x1, y + h, z0 + 0.04f}, "Ledge"});
        rails.push_back({{x0, y + h, z1 - 0.04f}, {x1, y + h, z1 - 0.04f}, "Ledge"});
    } else {
        rails.push_back({{x0 + 0.04f, y + h, z0}, {x0 + 0.04f, y + h, z1}, "Ledge"});
        rails.push_back({{x1 - 0.04f, y + h, z0}, {x1 - 0.04f, y + h, z1}, "Ledge"});
    }
}

// Stairs going DOWN in direction dir (0 +x, 1 -x, 2 +z, 3 -z) from a raised level `drop` above ground.
void World::addStairs(float x0, float z0, float x1, float z1, float drop, int steps, int dir, bool handrails) {
    float y = terrain((x0 + x1) / 2, (z0 + z1) / 2);
    Color stone = col(186, 180, 170);
    for (int k = 0; k < steps; k++) {
        float t0 = (float)k / steps, t1 = (float)(k + 1) / steps;
        float top = y + drop * (steps - k) / (steps + 1);
        float a0, a1, b0, b1;
        if (dir < 2) {
            float s0 = dir == 0 ? lerpf(x0, x1, t0) : lerpf(x1, x0, t1), s1 = dir == 0 ? lerpf(x0, x1, t1) : lerpf(x1, x0, t0);
            a0 = s0; a1 = s1; b0 = z0; b1 = z1;
        } else {
            float s0 = dir == 2 ? lerpf(z0, z1, t0) : lerpf(z1, z0, t1), s1 = dir == 2 ? lerpf(z0, z1, t1) : lerpf(z1, z0, t0);
            a0 = x0; a1 = x1; b0 = s0; b1 = s1;
        }
        addBox(a0, b0, a1, b1, y - 0.3f, top, PK_PROP, stone, stone, 0.9f);
    }
    if (handrails) {
        float hr = 0.95f;
        if (dir >= 2) {
            float zt = dir == 2 ? z0 : z1, zb = dir == 2 ? z1 : z0;
            for (float x : {x0 + 0.4f, x1 - 0.4f})
                addRailObj({x, y + drop + hr, zt}, {x, y + hr * 0.6f, zb + (dir == 2 ? 1.5f : -1.5f)}, "Handrail");
        } else {
            float xt = dir == 0 ? x0 : x1, xb = dir == 0 ? x1 : x0;
            for (float z : {z0 + 0.4f, z1 - 0.4f})
                addRailObj({xt, y + drop + hr, z}, {xb + (dir == 0 ? 1.5f : -1.5f), y + hr * 0.6f, z}, "Handrail");
        }
    }
}

void World::buildSkateSpots() {
    Color conc = col(182, 178, 170), gran = col(150, 146, 142);
    // --- Embarcadero Plaza ("EMB"), the classic SF skate spot
    {
        float y = terrain(1190, -300);
        addBox(1170, -312, 1205, -286, y - 0.3f, y + 1.6f, PK_PROP, conc, C_PLAZA);
        addStairs(1176, -286, 1199, -278, 1.6f, 4, 2, true);
        addRamp(1205, -310, 1216, -288, 1.6f, 0, false); // bank down off the deck
        addLedge(1172, -268, 1190, -267.2f, 0.55f, gran);
        addLedge(1172, -258, 1190, -257.2f, 0.45f, gran);
        addLedge(1172, -248, 1192, -247.0f, 0.7f, gran);
        addRailObj({1196, y + 0.45f, -238}, {1214, y + 0.45f, -238}, "Flat Bar");
        addRamp(1222, -262, 1226, -250, 1.0f, 0, true);
        addBox(1226, -262, 1232, -250, y - 0.3f, y + 1.0f, PK_PROP, conc, C_PLAZA);
        rails.push_back({{1226, y + 1.0f, -261.9f}, {1232, y + 1.0f, -261.9f}, "Funbox"});
        rails.push_back({{1226, y + 1.0f, -250.1f}, {1232, y + 1.0f, -250.1f}, "Funbox"});
        addRamp(1232, -262, 1236, -250, 1.0f, 0, false);
        addRamp(1240, -244, 1243, -238, 0.9f, 0, true);     // kicker
        addRamp(1222, -300, 1225, -294, 0.9f, 0, true);     // kicker
        // Vaillancourt Fountain
        ponds.push_back({1236, -312, 1253, -296});
        MeshBuilder& m = mb(1245, -304);
        m.quad({1236, y - 0.25f, -312}, {1253, y - 0.25f, -312}, {1253, y - 0.25f, -296}, {1236, y - 0.25f, -296}, col(60, 110, 120, M_WATER));
        RNG r(12);
        for (int k = 0; k < 14; k++) {
            Vector3 a = {r.range(1238, 1251), y - 0.5f, r.range(-310, -298)};
            Vector3 b = {a.x + r.range(-4, 4), y + r.range(3, 9), a.z + r.range(-4, 4)};
            m.beam(a, b, 2.2f, 2.2f, col(160, 154, 140));
            m.beam(b, {b.x + r.range(-5, 5), b.y + r.range(-1, 2), b.z + r.range(-3, 3)}, 2.0f, 2.0f, col(160, 154, 140));
        }
        addPlat({1239, 1250, -309, -299, y + 9, y + 9, y - 1, 0, PK_BUILDING});
    }
    // --- Union Square: Dewey Monument, palms, ledges
    {
        float cx = (gx(19) + gx(20)) / 2, cz = (gz(6) + gz(7)) / 2, y = terrain(cx, cz);
        MeshBuilder& m = mb(cx, cz);
        m.box({cx - 3, y - 0.3f, cz - 3}, {cx + 3, y + 2, cz + 3}, col(200, 196, 190));
        m.prism({cx, y + 2, cz}, 12, 1.4f, 1.2f, 26, col(225, 222, 215), col(225, 222, 215));
        m.prism({cx, y + 28, cz}, 8, 0.8f, 0.2f, 4, col(200, 160, 60), col(200, 160, 60));
        addPlat({cx - 3, cx + 3, cz - 3, cz + 3, y + 28, y + 28, y - 1, 0, PK_BUILDING});
        RNG r(44);
        for (int k = 0; k < 4; k++) addTree(cx + (k & 1 ? 32.f : -32.f), cz + (k & 2 ? 32.f : -32.f), 2, 1.1f, r, true);
        addLedge(cx - 25, cz - 15, cx - 10, cz - 14.2f, 0.55f, gran);
        addLedge(cx + 10, cz + 14.2f, cx + 25, cz + 15, 0.55f, gran);
        addLedge(cx - 15, cz + 20, cx - 14.2f, cz + 34, 0.5f, gran);
        addStairs(cx + 14, cz - 34, cx + 30, cz - 27, 1.4f, 4, 3, true);
        addBox(cx + 14, cz - 27, cx + 30, cz - 18, y - 0.3f, y + 1.4f, PK_PROP, conc, C_PLAZA);
    }
    // --- Civic Center Plaza: raised lawns with grindable edges
    {
        float cx = (gx(14) + gx(15)) / 2, cz = (gz(12) + gz(13)) / 2;
        Color lawn = col(90, 140, 70, M_GROUND);
        for (int k = 0; k < 4; k++) {
            float lx = cx + (k & 1 ? 8.f : -38.f), lz = cz + (k & 2 ? 8.f : -38.f);
            float y = terrain(lx + 15, lz + 15);
            addBox(lx, lz, lx + 30, lz + 30, y - 0.3f, y + 0.55f, PK_PROP, conc, lawn, 0.9f);
            rails.push_back({{lx, y + 0.55f, lz + 0.05f}, {lx + 30, y + 0.55f, lz + 0.05f}, "Lawn Ledge"});
            rails.push_back({{lx, y + 0.55f, lz + 29.95f}, {lx + 30, y + 0.55f, lz + 29.95f}, "Lawn Ledge"});
            rails.push_back({{lx + 0.05f, y + 0.55f, lz}, {lx + 0.05f, y + 0.55f, lz + 30}, "Lawn Ledge"});
            rails.push_back({{lx + 29.95f, y + 0.55f, lz}, {lx + 29.95f, y + 0.55f, lz + 30}, "Lawn Ledge"});
            RNG r(70 + k);
            for (int t = 0; t < 4; t++) {
                float tx = lx + 6 + (t & 1) * 18, tz = lz + 6 + (t >> 1) * 18;
                float ty = y + 0.55f;
                MeshBuilder& mm = mb(tx, tz);
                mm.beam({tx, ty, tz}, {tx, ty + 3.5f, tz}, 0.4f, 0.4f, col(96, 72, 52));
                mm.prism({tx, ty + 3.0f, tz}, 6, 0.5f, 2.6f, 1.6f, col(80, 125, 60), col(80, 125, 60));
                mm.prism({tx, ty + 4.6f, tz}, 6, 2.6f, 0.3f, 2.0f, col(86, 132, 64), col(80, 125, 60));
                addPlat({tx - 0.25f, tx + 0.25f, tz - 0.25f, tz + 0.25f, ty + 5, ty + 5, ty - 1, 0, PK_POLE});
            }
        }
        addRamp(cx - 4, cz - 3, cx - 1, cz + 3, 1.0f, 0, true);
    }
    // --- Marina Green mega kicker
    addRamp(-480, -1050, -468, -1042, 3.4f, 0, true);
    addRamp(-440, -1052, -430, -1040, 1.6f, 0, false);   // landing bump
    // --- Twin Peaks summit lookout with a kicker
    {
        float cx = -280, cz = 890, y = terrain(cx, cz);
        (void)y;
        groundPad(*this, cx - 25, cz - 15, cx + 25, cz + 15, 0.06f, C_PLAZA);
        addRailObj({cx - 20, terrain(cx - 20, cz - 16) + 1.0f, cz - 16}, {cx + 20, terrain(cx + 20, cz - 16) + 1.0f, cz - 16}, "Lookout Rail");
    }
    // --- Dolores Park: a kicker at the top of the hill
    addRamp(gx(12) + 30, gz(17) + 50, gx(12) + 33, gz(17) + 56, 0.9f, 0, true);
    // --- benches along the Embarcadero promenade
    for (float z = -900; z < 600; z += 45) {
        if (z > -300 && z < -100) continue;
        if (z > 300 && z < 370) continue;
        addLedge(1281, z, 1284.5f, z + 0.8f, 0.5f, col(120, 90, 60));
    }
}

// ---------------------------------------------------------------- meta: teleports, tokens, labels, paths, people
void World::buildMeta() {
    auto G = [&](float x, float z) { return ground(x, z, 200); };
    auto spot = [&](const char* n, float x, float z, float yaw) { spots.push_back({n, {x, G(x, z), z}, yaw}); };
    spot("Embarcadero Plaza", 1176, -231, 0);
    spot("Golden Gate Bridge", -1092, -1010, -PI / 2);
    spot("Twin Peaks", -282, 878, -0.6f);
    spot("Lombard Street", gx(16) - 4, gz(3), 0);
    spot("Painted Ladies", -300, 170, 0);
    spot("Bay Bridge", 1500, 339, 0);
    spot("Ocean Beach", -1430, -100, PI / 2);
    spot("Union Square", gx(19) + 15, gz(6) + 15, 0);
    spot("Coit Tower", 880, -855, PI / 2);
    spot("Vista Point (Marin)", -1095, -2400, PI / 2);
    spot("SFO Airport", 2010, 2095, PI);
    {
        Vector3 p = trackPoint(stations[0].s - 30, 6.2f, nullptr);
        spots.push_back({"Caltrain 4th & King", {p.x, ground(p.x, p.z, 200), p.z}, PI / 2});
    }
    spot("Menlo Park", mp.node(5, 0).x - 20, mp.node(0, 4).y + 2.4f, PI);

    auto tok = [&](const char* n, float x, float z) { tokens.push_back({{x, G(x, z) + 1.4f, z}, n}); };
    tok("Coit Tower", 880, -852);
    tok("Golden Gate Bridge", -1100, -1690);
    tok("Twin Peaks", -300, 905);
    tok("Lombard Street", gx(17) + 3, gz(3));
    tok("Painted Ladies", -330, 170);
    tok("Ferry Building", 1276, -200);
    tok("Yerba Buena Island", 2620, 335);
    tok("Ocean Beach", -1440, 600);
    tok("Vista Point", -1060, -2420);
    tok("Palace of Fine Arts", -830, -1010);
    tok("Union Square", gx(19) + 30, gz(6) + 55);
    tok("Oracle Park", 1235, 690);
    tok("Dolores Park", gx(12) + 55, gz(17) + 30);
    tok("Sutro Tower", -470, 800);
    tok("Port of Oakland", 4040, 900);
    tok("Pier 39", 1025, -1280);
    tok("Salesforce Tower", 1100, 210);
    tok("Embarcadero Plaza", 1230, -256);
    tok("SFO Runway 28R", 1560, 2262);
    {
        Vector3 p = trackPoint(stations[0].s + 40, 6.2f, nullptr);
        tokens.push_back({{p.x, ground(p.x, p.z, 200) + 1.4f, p.z}, "4th & King"});
        p = trackPoint(stations.back().s + 30, 6.2f, nullptr);
        tokens.push_back({{p.x, ground(p.x, p.z, 200) + 1.4f, p.z}, "Menlo Park Station"});
    }
    tok("Santa Cruz Avenue", mp.node(4, 0).x + 30, mp.node(0, 4).y + 2.4f);
    tok("Bayfront Campus", 3285, mp.node(0, 4).y + 2.4f);
    tok("Oak-lined streets", mp.node(10, 0).x + 70, mp.node(0, 7).y + 2.4f);

    labels = {{"PACIFIC OCEAN", -1650, 400, 2}, {"SAN FRANCISCO BAY", 2000, -800, 2}, {"GOLDEN GATE", -1100, -1700, 1},
              {"MARIN HEADLANDS", -1500, -2700, 1}, {"ALCATRAZ", 300, -1960, 0}, {"ANGEL ISLAND", 330, -3000, 0},
              {"OAKLAND", 4380, -400, 1}, {"TREASURE ISLAND", 2650, -380, 0}, {"FINANCIAL DISTRICT", 1030, -320, 0},
              {"SOMA", 800, 500, 0}, {"MISSION", 250, 950, 0}, {"SUNSET", -950, 900, 0}, {"RICHMOND", -950, -250, 0},
              {"PRESIDIO", -1200, -850, 0}, {"MARINA", -500, -980, 0}, {"NOB HILL", 520, -380, 0},
              {"NORTH BEACH", 750, -780, 0}, {"GOLDEN GATE PARK", -950, 335, 0}, {"TWIN PEAKS", -290, 850, 0},
              {"HAIGHT", -300, 450, 0}, {"PACIFIC HEIGHTS", -150, -600, 0}, {"BAY BRIDGE", 2000, 300, 0}, {"SFO AIRPORT", 2000, 1980, 1}, {"MENLO PARK", 1700, 9100, 2},
              {"EL CAMINO REAL", 2190, 9900, 0}, {"SANTA CRUZ AVE", 1700, 9395, 0}, {"CALTRAIN", 2470, 9150, 0},
              {"LINFIELD OAKS", 2850, 9700, 0}, {"BAYFRONT", 3450, 9200, 1}, {"STANFORD", 2300, 10900, 1},
              {"WOODSIDE HILLS", 1150, 10200, 0}, {"SALT PONDS", 3760, 10000, 0}};

    // cable car lines (Powell-Hyde and California)
    auto cablePath = [&](Vector2 a, Vector2 b) {
        Path P;
        P.p = {a, b};
        P.finish();
        cablePaths.push_back(P);
        Vector2 d = Vector2Normalize(Vector2Subtract(b, a)), r = {-d.y, d.x};
        Color steel = col(120, 122, 128, M_GROUND), slot = col(30, 30, 32, M_GROUND);
        roadStrip(*this, Vector2Add(a, Vector2Scale(r, 0.75f)), Vector2Add(b, Vector2Scale(r, 0.75f)), 0.06f, 0.06f, 0, 0.11f, steel, 6.f);
        roadStrip(*this, Vector2Subtract(a, Vector2Scale(r, 0.75f)), Vector2Subtract(b, Vector2Scale(r, 0.75f)), 0.06f, 0.06f, 0, 0.11f, steel, 6.f);
        roadStrip(*this, a, b, 0.03f, 0.03f, 0, 0.11f, slot, 6.f);
    };
    cablePath({gx(19), gz(8)}, {gx(19), gz(1)});
    cablePath({gx(23), gz(6)}, {gx(14), gz(6)});

    auto pedPath = [&](std::vector<Vector2> pts) {
        Path P;
        P.p = pts;
        P.finish();
        pedPaths.push_back(P);
    };
    pedPath({{1296, -950}, {1296, -300}});
    pedPath({{1278, -100}, {1278, 600}});
    pedPath({{-1435, -420}, {-1435, 1700}});
    pedPath({{-900, -1075}, {-200, -1080}});
    pedPath({{1278, -1000}, {1100, -1090}, {700, -1120}});

    RNG r(606);
    struct TS { float x, z, rad; int n; };
    TS ts[] = {{-1150, -2415, 22, 14}, {-285, 885, 20, 12}, {-330, 175, 30, 14}, {1250, -224, 6, 8},
               {880, -862, 12, 8},     {-830, -1040, 25, 10}, {1025, -1180, 15, 14}, {700, -1080, 40, 14},
               {1290, -170, 10, 10},   {gx(19) + 55, gz(6) + 55, 25, 14}, {1815, 1880, 25, 16}, {2380, 9400, 14, 10}, {1900, 9418, 30, 10}};
    for (auto& t : ts)
        for (int k = 0; k < t.n; k++) {
            float a = r.range(0, TAU_F), d = r.range(2, t.rad);
            float x = t.x + cosf(a) * d, z = t.z + sinf(a) * d;
            if (onBridge(x, z) || fabsf(x + 1100) < 17) continue;
            Vector3 p = {x, G(x, z), z};
            Vector3 tmp = p;
            Vector2 n;
            if (collide(tmp, 0.5f, p.y, 1.8f, &n)) continue;
            tourists.push_back(p);
        }
}

// ---------------------------------------------------------------- generation order
void World::generate(void (*progress)(const char*, float)) {
    progress("Shaping the hills of San Francisco", 0.02f);
    buildHeightfield();
    sf.x0 = -1375; sf.z0 = -1095; sf.sp = 110; sf.ni = 25; sf.nj = 27;
    oak.x0 = 4080; oak.z0 = -1565; oak.sp = 95; oak.ni = 7; oak.nj = 36;
    buildGrid(sf);
    buildGrid(oak);
    sf.setEdge(16, 3, 0, false); // Lombard's crooked block replaces the straight street
    progress("Laying the ground", 0.25f);
    buildTerrainMesh();
    buildBackdrop();
    progress("Paving the streets", 0.4f);
    buildRoads(sf, 0);
    buildRoads(oak, 0);
    buildMarket();
    buildLombard();
    buildTramLines();
    progress("Building the Golden Gate Bridge", 0.5f);
    buildGoldenGate();
    buildBayBridge();
    progress("Raising landmarks", 0.58f);
    buildLandmarks();
    buildWaterfront();
    buildSkateSpots();
    buildAirport();
    progress("Laying Caltrain down the Peninsula", 0.62f);
    buildCaltrain();
    progress("Planting the oaks of Menlo Park", 0.64f);
    buildMenlo();
    buildGondolas();
    progress("Building neighborhoods", 0.66f);
    buildBlocks(sf, false);
    buildBlocks(oak, true);
    progress("Planting trees and parking cars", 0.8f);
    buildStreetFurniture(sf, false);
    buildStreetFurniture(oak, true);
    buildParks();
    progress("Drawing the map", 0.9f);
    buildMeta();
    buildMap();
    progress("Uploading to the GPU", 0.96f);
}
