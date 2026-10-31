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

// a bridge pier: drawn always, and solid where it stands in the water so boats can't pass through it (#63);
// the ones on land stay mesh-only, as before, so nothing that drives or walks there changes
void World::addBridgePier(MeshBuilder& m, float x0, float z0, float x1, float z1, float lo, float hi, Color c) {
    m.box({x0, lo, z0}, {x1, hi, z1}, c, c, 0.8f);
    if (terrain((x0 + x1) / 2, (z0 + z1) / 2) < 0.05f) addPlat({x0, x1, z0, z1, hi, hi, lo, 0, PK_BUILDING});
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
        // the whole concrete base is solid (not just its two ends), so boats can't run through between the legs (#63)
        addPlat({X - 30, X + 30, tz - 14, tz + 14, 5, 5, -10, 0, PK_BUILDING});
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
            addBridgePier(m, X + sd * 10 - 2, z - 2, X + sd * 10 + 2, z + 2, terrain(X, z) - 2, y - 6, orange);
    }
    for (float z = -2150; z > -2320; z -= 45) {
        float y = ggDeck(z);
        for (int sd = -1; sd <= 1; sd += 2)
            addBridgePier(m, X + sd * 10 - 2, z - 2, X + sd * 10 + 2, z + 2, std::min(terrain(X, z), 0.f) - 2, y - 6, orange);
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
        // the overlook rail along the plaza's south edge, in two pieces either side of the bridge road and its
        // sidewalks (X +- 14), so cars, walkers and skaters coming off the bridge pass between them
        addRailObj({X - 42, y + 1.0f, -2377}, {X - 17, y + 1.0f, -2377}, "Vista Point Rail");
        addRailObj({X + 17, y + 1.0f, -2377}, {X + 42, y + 1.0f, -2377}, "Vista Point Rail");
    }
    // cars U-turn at the city end 23 m short of the street the approach meets (gz(4)), the loop (9 m outer lane) clear
    // of its cross traffic rather than out across it, where the two waited on each other for minutes (#47)
    Path P;
    P.p = {{X, gz(4) - 23}, {X, -2410}};
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
        addPlat({tx - 10, tx + 10, Z - 28, Z + 28, 4, 4, -10, 0, PK_BUILDING});   // the base in the water (#63)
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
        addPlat({ax - 20, ax + 20, Z - 25, Z + 25, y - 6, y - 6, -10, 0, PK_BUILDING});   // solid down to the water (#63)
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
        addPlat({tx - 12, tx + 12, Z - 30, Z + 30, 4, 4, -10, 0, PK_BUILDING});
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
            addBridgePier(m, x - 2, Z + sd * 9 - 2, x + 2, Z + sd * 9 + 2, g, y - 6, conc);
    }
    Path P;
    // cars U-turn at the city end up on the approach ramp, the loop (9.5 m outer lane) clear of the Embarcadero
    // roadway (x < 1272), rather than out across its cross traffic; the same at the Oakland end, short of the street
    // the bridge comes down onto (x > 4073), where they held each other up for minutes (#47)
    P.p = {{1288, Z}, {4056, Z}};
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
        // collision steps up the taper: each box is as wide as the glass at its own top, so it sits just inside the
        // walls all the way down (the old single +-15 box let the skater ride 2.4 m into the base, #65)
        for (float t : {4.f, 20.f, 40.f, 60.f, 80.f, 100.f, 120.f, 140.f, 160.f}) {
            float hw = b * (1 - t / H);
            addPlat({cx - hw, cx + hw, cz - hw, cz + hw, y + t, y + t, y, 0, PK_BUILDING});
        }
        for (int sd = -1; sd <= 1; sd += 2) { // the wings stick out past the taper above 95 m
            float off = b * (1 - 95 / H);
            addPlat({cx + (sd > 0 ? off : -off - 5), cx + (sd > 0 ? off + 5 : -off), cz - 4, cz + 4, y + 178, y + 178, y + 95, 0, PK_BUILDING});
        }
        for (int k = 0; k < 6; k++) addTree(cx - 40 + k * 4, cz + 30, 0, 0.9f, r, true);
    }
    // --- Salesforce Tower (rounded glass obelisk with a glowing crown)
    {
        float cx = (gx(22) + gx(23)) / 2, cz = (gz(11) + gz(12)) / 2, y = terrain(cx, cz) - 1;
        MeshBuilder& m = mb(cx, cz);
        Color g = col(178, 190, 198, M_GLASS);
        m.prism({cx, y, cz}, 16, 23, 18, 200, g, g, 0.2f, false);
        m.prism({cx, y + 200, cz}, 16, 18, 15, 20, col(220, 225, 235, M_EMISSIVE), col(90, 90, 95), 0.2f, true);
        // collision for the round, tapering shaft: at each step a stack of centred boxes whose corners sit on the
        // glass radius at that step's top. At the deepest (45 deg) notches the skater's centre stops about 0.6 m
        // inside the glass's flat face, against 4.6 m inside the 23 m base with the old +-18 square (#65)
        auto disc = [&](float top, float rad, int k) {
            for (int i = 0; i < k; i++) {
                float a = (i + 0.5f) * (PI / 2) / k, hx = rad * cosf(a), hz = rad * sinf(a);
                addPlat({cx - hx, cx + hx, cz - hz, cz + hz, y + top, y + top, y, 0, PK_BUILDING});
            }
        };
        disc(4, 23 - 5 * 4 / 200.f, 12);   // street level, where the skater rides: finest fit
        for (float t : {50.f, 100.f, 150.f, 200.f}) disc(t, 23 - 5 * t / 200, 8);
        disc(220, 15, 8);                  // the crown's roof
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
        addPlat({cx - 20, cx + 20, cz - 38, cz - 32, y + 20, y + 20, y - 1, 0, PK_BUILDING});
        for (int k = 0; k < 8; k++) {   // the front columns, solid like the Palace of Fine Arts' ones
            float px = cx - 17.5f + k * 5, pz = cz - 39;
            m.prism({px, y + 0.5f, pz}, 8, 0.8f, 0.8f, 19, col(235, 232, 225), w);
            addPlat({px - 0.8f, px + 0.8f, pz - 0.8f, pz + 0.8f, y + 19.5f, y + 19.5f, y - 1, 0, PK_POLE});
        }
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
        // the lagoon sits in a basin dug into the terrain (rawHeight). The sheet reaches one ground cell past the
        // splash rectangle, over the bank, so the shoreline is drawn where the bank rises out of the water
        ponds.push_back({LAGOON_X0, LAGOON_Z0, LAGOON_X1, LAGOON_Z1, "lagoon"});
        float wx0 = LAGOON_X0 - 10, wx1 = LAGOON_X1 + 10, wz0 = LAGOON_Z0 - 10, wz1 = LAGOON_Z1 + 10, py = LAGOON_Y;
        m.quad({wx0, py, wz0}, {wx1, py, wz0}, {wx1, py, wz1}, {wx0, py, wz1}, col(50, 100, 110, M_WATER));
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
        // grandstand: a seating bank rising from a 2 m front wall at x 1195 up to the back wall at x 1175
        m.slab(1175, 1195, 698, 852, y + 18, y + 2, 0, y, col(40, 90, 70), brick);
        addPlat({1175, 1195, 698, 852, y + 18, y + 2, y - 1, 0, PK_PROP});
        m.box({1280, y, 702}, {1298, y + 26, 708}, col(30, 30, 34), col(30, 30, 34));       // scoreboard
        addPlat({1280, 1298, 702, 708, y + 26, y + 26, y - 1, 0, PK_PROP});
        // the giant bottle stands on a plinth (it used to float 3 m over the grass); solid down to the field
        Color red = col(190, 30, 30);
        m.box({1287.5f, y, 712.5f}, {1292.5f, y + 3, 717.5f}, brick, brick);
        m.prism({1290, y + 3, 715}, 10, 2.5f, 1.2f, 18, red, red);
        addPlat({1287.5f, 1292.5f, 712.5f, 717.5f, y + 3, y + 3, y - 1, 0, PK_PROP});      // plinth
        addPlat({1288.2f, 1291.8f, 713.2f, 716.8f, y + 21, y + 21, y - 1, 0, PK_PROP});    // bottle (its tapering 10-gon, roughly)
        // the giant glove sits on a pedestal (it used to float 6 m over the grass); the glove itself is approximated by
        // a box around the core of its r 6 sphere
        Color leather = col(140, 90, 50);
        m.box({1287.5f, y, 732.5f}, {1292.5f, y + 7, 737.5f}, brick, brick);
        m.sphere({1290, y + 12, 735}, 6, 8, 10, leather);
        addPlat({1287.5f, 1292.5f, 732.5f, 737.5f, y + 7, y + 7, y - 1, 0, PK_PROP});      // pedestal
        addPlat({1285.8f, 1294.2f, 730.8f, 739.2f, y + 16, y + 16, y + 8, 0, PK_PROP});    // glove
        for (Vector2 t : {Vector2{1165, 690}, Vector2{1165, 862}, Vector2{1295, 860}}) {     // light towers
            m.box({t.x - 0.7f, y, t.y - 0.7f}, {t.x + 0.7f, y + 40, t.y + 0.7f}, col(80, 80, 85));
            m.box({t.x - 3, y + 38, t.y - 3}, {t.x + 3, y + 42, t.y + 3}, col(255, 250, 220, M_EMISSIVE));
            addPlat({t.x - 0.7f, t.x + 0.7f, t.y - 0.7f, t.y + 0.7f, y + 40, y + 40, y - 1, 0, PK_POLE});
        }
    }
    buildAlcatraz();
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
        float root = 1290, x1 = 1480;   // the shed and the piles stand from the root out
        auto lowest = [&](float x) {
            float h = 1e9f;
            for (float z = zc - 15; z <= zc + 15; z += 2.5f) h = std::min(h, terrain(x, z));
            return h;
        };
        // level with the lowest point of the promenade across its root, so nowhere along it is there a lip a
        // rolling board can't ride up (the promenade falls across the width)
        float x0 = root, y = lowest(root) + 0.02f;
        if (y < 2.4f) {
            // The north end of the promenade runs out past the seawall: at z -800 the shore has already dropped into
            // the bay at the root. Run the deck back west over the shore to where the promenade is still up at its
            // height across the whole width, and build it at that height, so you roll straight off the promenade
            // onto it (never under the bay, and no step up out of the water at its end).
            while (x0 > root - 40 && lowest(x0) < 2.4f) x0 -= 1;
            y = std::max(lowest(x0), 2.0f) + 0.02f;
        }
        MeshBuilder& m = mb(1380, zc);
        m.box({x0, -3, zc - 15}, {x1, y, zc + 15}, col(110, 100, 90), wood, 0.7f);
        addPlat({x0, x1, zc - 15, zc + 15, y, y, -4, 0, PK_PIER});
        for (float x = root + 10; x < x1; x += 15)
            for (int sd = -1; sd <= 1; sd += 2) m.beam({x, -4, zc + sd * 14}, {x, y - 0.5f, zc + sd * 14}, 0.6f, 0.6f, col(80, 70, 60));
        addBox(root + 12, zc - 11, x1 - 8, zc + 11, y - 0.5f, y + 11, PK_BUILDING, pale, roof);
        addBox(root + 4, zc - 13, root + 12, zc + 13, y - 0.5f, y + 14, PK_BUILDING, col(236, 230, 214, M_HOUSE), col(200, 195, 185));
        m.gable(root + 12, x1 - 8, zc - 11, zc + 11, y + 11, 3.5f, 0, roof, pale);
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
        // the hull is solid (it ran 489.5..500.5 x -1235..-1165, keel underwater, deck at 5.3): boats bump it (#63)
        addPlat({489.5f, 500.5f, -1235, -1165, 5.3f, 5.3f, -2, 0, PK_BUILDING});
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
void World::addRamp(float x0, float z0, float x1, float z1, float rise, int axis, bool up, bool coping) {
    float y = terrain((x0 + x1) / 2, (z0 + z1) / 2);
    float lo = y, hi = y + rise;
    Platform p{x0, x1, z0, z1, up ? lo : hi, up ? hi : lo, y - 0.4f, (uint8_t)axis, PK_PROP};
    addPlat(p);
    mb((x0 + x1) / 2, (z0 + z1) / 2).slab(x0, x1, z0, z1, p.h0, p.h1, axis, y - 0.4f, col(184, 152, 112, M_GROUND), col(120, 120, 124));
    // coping rail on the high edge (not where the ramp butts against a deck or box: there it's a seam in a flat top)
    if (!coping) return;
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
        addRamp(1205, -310, 1216, -288, 1.6f, 0, false, false); // bank down off the deck (no coping: it adjoins the deck)
        addLedge(1172, -268, 1190, -267.2f, 0.55f, gran);
        addLedge(1172, -258, 1190, -257.2f, 0.45f, gran);
        addLedge(1172, -248, 1192, -247.0f, 0.7f, gran);
        addRailObj({1196, y + 0.45f, -238}, {1214, y + 0.45f, -238}, "Flat Bar");
        addRamp(1222, -262, 1226, -250, 1.0f, 0, true, false);   // funbox: the ramps' high edges meet the box top
        addBox(1226, -262, 1232, -250, y - 0.3f, y + 1.0f, PK_PROP, conc, C_PLAZA);
        rails.push_back({{1226, y + 1.0f, -261.9f}, {1232, y + 1.0f, -261.9f}, "Funbox"});
        rails.push_back({{1226, y + 1.0f, -250.1f}, {1232, y + 1.0f, -250.1f}, "Funbox"});
        addRamp(1232, -262, 1236, -250, 1.0f, 0, false, false);
        addRamp(1240, -244, 1243, -238, 0.9f, 0, true);     // kicker
        addRamp(1222, -300, 1225, -294, 0.9f, 0, true);     // kicker
        // Vaillancourt Fountain
        ponds.push_back({1236, -312, 1253, -296, "fountain"});
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
    // The square sits on a hill that falls ~2 m every 10 m to the east, so nothing here can share one height: each
    // prop is fitted to the ground under its own footprint.
    {
        float cx = (gx(19) + gx(20)) / 2, cz = (gz(6) + gz(7)) / 2, y = terrain(cx, cz);
        // lowest / highest ground under a footprint (the terrain mesh is 10 m triangles; 1 m samples catch its kinks)
        auto lowHigh = [&](float x0, float z0, float x1, float z1, float& lo, float& hi) {
            lo = 1e9f; hi = -1e9f;
            int nx = std::max(1, (int)ceilf(x1 - x0)), nz = std::max(1, (int)ceilf(z1 - z0));
            for (int j = 0; j <= nz; j++)
                for (int i = 0; i <= nx; i++) {
                    float t = terrain(lerpf(x0, x1, (float)i / nx), lerpf(z0, z1, (float)j / nz));
                    lo = std::min(lo, t); hi = std::max(hi, t);
                }
        };
        float mLo, mHi;
        lowHigh(cx - 3, cz - 3, cx + 3, cz + 3, mLo, mHi);
        MeshBuilder& m = mb(cx, cz);
        m.box({cx - 3, mLo - 0.3f, cz - 3}, {cx + 3, y + 2, cz + 3}, col(200, 196, 190));
        m.prism({cx, y + 2, cz}, 12, 1.4f, 1.2f, 26, col(225, 222, 215), col(225, 222, 215));
        m.prism({cx, y + 28, cz}, 8, 0.8f, 0.2f, 4, col(200, 160, 60), col(200, 160, 60));
        addPlat({cx - 3, cx + 3, cz - 3, cz + 3, y + 28, y + 28, mLo - 1, 0, PK_BUILDING});
        RNG r(44);
        for (int k = 0; k < 4; k++) addTree(cx + (k & 1 ? 32.f : -32.f), cz + (k & 2 ? 32.f : -32.f), 2, 1.1f, r, true);
        // granite ledges that run with the slope, standing h above the ground along their whole length
        auto ledge = [&](float x0, float z0, float x1, float z1, float h) {
            int axis = x1 - x0 > z1 - z0 ? 0 : 1;
            float lo, hi, e0, e1, dummy;
            lowHigh(x0, z0, x1, z1, lo, hi);
            if (axis == 0) { lowHigh(x0, z0, x0, z1, dummy, e0); lowHigh(x1, z0, x1, z1, dummy, e1); }
            else           { lowHigh(x0, z0, x1, z0, dummy, e0); lowHigh(x0, z1, x1, z1, dummy, e1); }
            float h0 = e0 + h, h1 = e1 + h;
            Platform p{x0, x1, z0, z1, h0, h1, lo - 0.3f, (uint8_t)axis, PK_PROP};
            addPlat(p);
            mb((x0 + x1) / 2, (z0 + z1) / 2).slab(x0, x1, z0, z1, h0, h1, axis, lo - 0.3f, scaleC(gran, 1.05f), gran);
            if (axis == 0) {
                rails.push_back({{x0, h0, z0 + 0.04f}, {x1, h1, z0 + 0.04f}, "Ledge"});
                rails.push_back({{x0, h0, z1 - 0.04f}, {x1, h1, z1 - 0.04f}, "Ledge"});
            } else {
                rails.push_back({{x0 + 0.04f, h0, z0}, {x0 + 0.04f, h1, z1}, "Ledge"});
                rails.push_back({{x1 - 0.04f, h0, z0}, {x1 - 0.04f, h1, z1}, "Ledge"});
            }
        };
        ledge(cx - 25, cz - 15, cx - 10, cz - 14.2f, 0.55f);
        ledge(cx + 10, cz + 14.2f, cx + 25, cz + 15, 0.55f);
        ledge(cx - 15, cz + 20, cx - 14.2f, cz + 34, 0.55f);   // (like every ledge, over ROLL_UP: it takes an ollie to get on)
        // a terrace dug into the hill: level with the ground on its uphill (west) side, with stairs and handrails
        // running down its downhill (east) face to the plaza below. The ground keeps falling under the stairs, so the
        // bottom step is set just above the highest ground under it (its face then stands only ~0.3 m over the plaza
        // where you ride into it) and the rest share the climb from there to the deck evenly.
        float dx0 = cx + 12, dx1 = cx + 20, sx1 = cx + 30, dz0 = cz - 27, dz1 = cz - 15;
        float dLo, dHi;
        lowHigh(dx0, dz0, dx1, dz1, dLo, dHi);
        float deck = dHi;
        addBox(dx0, dz0, dx1, dz1, dLo - 0.3f, deck, PK_PROP, conc, C_PLAZA);
        Color stone = col(186, 180, 170);
        const int steps = 10;
        float bLo, bHi;
        lowHigh(lerpf(dx1, sx1, (float)(steps - 1) / steps), dz0, sx1, dz1, bLo, bHi);
        float bottom = bHi + 0.05f;
        for (int k = 0; k < steps; k++) {
            float a0 = lerpf(dx1, sx1, (float)k / steps), a1 = lerpf(dx1, sx1, (float)(k + 1) / steps);
            float sLo, sHi;
            lowHigh(a0, dz0, a1, dz1, sLo, sHi);
            float top = std::max(lerpf(deck, bottom, (float)(k + 1) / steps), sHi + 0.05f);
            addBox(a0, dz0, a1, dz1, sLo - 0.3f, top, PK_PROP, stone, stone, 0.9f);
        }
        float hr = 0.95f;
        for (float z : {dz0 + 0.4f, dz1 - 0.4f})
            addRailObj({dx1, deck + hr, z}, {sx1 + 1.5f, terrain(sx1 + 1.5f, z) + hr * 0.6f, z}, "Handrail");
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
    // --- Marina Green mega kicker: kicker, a table a real drop below the lip, then a long landing slope back down to
    // the grass, like a real kicker-to-landing setup. The table butts against the kicker's high end, so its face
    // toward the kicker is hidden under the lip and nothing faces the kicker with a wall: rolling off the lip or an
    // ollie lands on the table or the landing slope, and anything faster lands on open grass past it (#94).
    {
        float y = terrain(-474, -1046), lip = y + 3.4f, table = lip - 1.3f;   // the kicker's base (addRamp samples its centre)
        addRamp(-480, -1050, -468, -1042, 3.4f, 0, true);
        addBox(-468, -1050, -458, -1042, y - 0.4f, table, PK_PROP, col(120, 120, 124), col(184, 152, 112, M_GROUND));
        float yl = terrain(-438, -1046), base = fminf(y, yl) - 0.4f;   // the landing meets the grass at its foot
        addPlat({-458, -438, -1050, -1042, table, yl, base, 0, PK_PROP});
        mb(-448, -1046).slab(-458, -438, -1050, -1042, table, yl, 0, base, col(184, 152, 112, M_GROUND), col(120, 120, 124));
    }
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
        float y = terrain(1282.75f, z + 0.4f) + 0.5f;
        if (y < 1.3f) continue;   // the north end of the row runs out past the seawall, under the bay
        bool north = ((int)(z / 45.f)) & 1;   // alternate benches face up and down the promenade
        for (float x : {1282.f, 1283.6f}) seats.push_back({{x, y, north ? z + 0.32f : z + 0.48f}, north ? -PI / 2 : PI / 2, false});
    }
}

// ---------------------------------------------------------------- Alcatraz
// The rock in the middle of the bay, worth the jet ski ride: a road winding up from the dock past Building 64 to
// the cellhouse on top, which you can skate straight through (in the front door, down Broadway between the cell
// blocks, out into the recreation yard), plus the lighthouse, the burned-out Warden's House, the water tower and
// the smokestack.
void World::buildAlcatraz() {
    const float Y = AZ_TOP;
    Color wall = col(206, 200, 186, M_HOUSE), plain = col(206, 200, 186), roofC = col(122, 120, 114);
    Color inside = col(188, 182, 168, M_INTERIOR), cellDark = col(40, 40, 44, M_INTERIOR), steel = col(78, 82, 86, M_INTERIOR);
    auto solid = [&](float x0, float z0, float x1, float z1, float base, float top, Color side, Color topC) {
        addBox(x0, z0, x1, z1, base, top, PK_BUILDING, side, topC, 0.85f);
    };
    auto lowest = [&](float x0, float z0, float x1, float z1) {
        return std::min(std::min(terrain(x0, z0), terrain(x1, z0)), std::min(terrain(x0, z1), terrain(x1, z1)));
    };
    auto pole = [&](float x, float z, float r, float top) { addPlat({x - r, x + r, z - r, z + r, top, top, terrain(x, z) - 1, 0, PK_POLE}); };

    // ---- the road up from the dock: asphalt on its shelf, lamps on the uphill side, handrails along the drops
    for (int k = 0; k + 1 < AZ_ROAD_N; k++) {
        Vector2 a = {AZ_ROAD[k].x, AZ_ROAD[k].z}, b = {AZ_ROAD[k + 1].x, AZ_ROAD[k + 1].z};
        roadStrip(*this, a, b, 3.5f, 3.5f, 2.f, 0.06f, col(70, 70, 74, M_GROUND), 3.f);
        Vector2 d = Vector2Normalize(Vector2Subtract(b, a)), n = {-d.y, d.x};
        Vector2 mid = Vector2Scale(Vector2Add(a, b), 0.5f);
        if (Vector2DotProduct(n, {mid.x - AZ_X, mid.y - AZ_Z}) < 0) n = Vector2Scale(n, -1);   // n points out, toward the water
        float L = Vector2Distance(a, b);
        if (k == 1 || k == 2 || k == 5 || k == 6) {
            Vector2 p0 = Vector2Add(Vector2Add(a, Vector2Scale(d, 3)), Vector2Scale(n, 4.4f)), p1 = Vector2Add(Vector2Add(b, Vector2Scale(d, -3)), Vector2Scale(n, 4.4f));
            float y0 = lerpf(AZ_ROAD[k].y, AZ_ROAD[k + 1].y, 3 / L), y1 = lerpf(AZ_ROAD[k].y, AZ_ROAD[k + 1].y, 1 - 3 / L);
            addRailObj({p0.x, y0 + 1.0f, p0.y}, {p1.x, y1 + 1.0f, p1.y}, "Alcatraz Rail");
        }
        for (float s = 12; s < L - 6; s += 38) {
            Vector2 q = Vector2Add(Vector2Add(a, Vector2Scale(d, s)), Vector2Scale(n, -5.6f));
            addLamp(q.x, q.y, atan2f(n.y, n.x));
        }
    }

    // ---- the cellhouse: 68 x 24 m and three storeys, doors at both ends lined up with Broadway
    const float CX0 = 280, CX1 = 348, CZ0 = -1913, CZ1 = -1889, CH = 14, T = 0.6f;
    const float DZ0 = -1903.5f, DZ1 = -1898.5f, DH = 4.f;
    MeshBuilder& m = mb((CX0 + CX1) / 2, (CZ0 + CZ1) / 2);
    solid(CX0, CZ0, CX1, CZ0 + T, Y - 1, Y + CH, wall, roofC);
    solid(CX0, CZ1 - T, CX1, CZ1, Y - 1, Y + CH, wall, roofC);
    for (float wx : {CX0, CX1 - T}) {
        solid(wx, CZ0 + T, wx + T, DZ0, Y - 1, Y + CH, wall, roofC);
        solid(wx, DZ1, wx + T, CZ1 - T, Y - 1, Y + CH, wall, roofC);
        solid(wx, DZ0, wx + T, DZ1, Y + DH, Y + CH, wall, roofC);
        m.box({wx, Y + DH - 0.02f, DZ0}, {wx + T, Y + DH, DZ1}, inside, inside, 1.f, true);   // underside of the lintel
    }
    solid(CX0, CZ0, CX1, CZ1, Y + CH - 0.5f, Y + CH, plain, roofC);   // roof
    m.box({CX0 + 4, Y + CH, DZ0 - 1}, {CX1 - 4, Y + CH + 2.2f, DZ1 + 1}, col(150, 160, 166), roofC, 0.9f);   // clerestory
    for (float x = CX0 + 4.5f; x < CX1 - 4.5f; x += 3)   // its window strip, both sides
        for (float z : {DZ0 - 1.02f, DZ1 + 1.0f}) m.box({x, Y + CH + 0.5f, z}, {x + 2.2f, Y + CH + 1.7f, z + 0.02f}, col(60, 70, 78));
    // inside: walls and ceiling lined so the sun-lit outside faces don't show through
    m.box({CX0 + T, Y + CH - 0.52f, CZ0 + T}, {CX1 - T, Y + CH - 0.5f, CZ1 - T}, inside, inside, 1.f, true);
    m.box({CX0 + T, Y, CZ0 + T}, {CX1 - T, Y + CH - 0.5f, CZ0 + T + 0.03f}, inside);
    m.box({CX0 + T, Y, CZ1 - T - 0.03f}, {CX1 - T, Y + CH - 0.5f, CZ1 - T}, inside);
    for (float wx : {CX0 + T, CX1 - T - 0.03f}) {
        m.box({wx, Y, CZ0 + T}, {wx + 0.03f, Y + CH - 0.5f, DZ0}, inside);
        m.box({wx, Y, DZ1}, {wx + 0.03f, Y + CH - 0.5f, CZ1 - T}, inside);
        m.box({wx, Y + DH, DZ0}, {wx + 0.03f, Y + CH - 0.5f, DZ1}, inside);
    }
    // B and C blocks: three tiers of barred cells on each face, galleries along the upper tiers, lamps down Broadway
    layer = LAYER_INTERIOR;
    for (int blk = 0; blk < 2; blk++) {
        float z0 = blk ? DZ1 : -1909.f, z1 = blk ? -1893.f : DZ0;
        addBox(290, z0, 338, z1, Y - 0.5f, Y + 8.2f, PK_BUILDING, inside, col(150, 146, 138, M_INTERIOR), 0.8f);
        MeshBuilder& c = mb(314, (z0 + z1) / 2);
        for (int face = 0; face < 2; face++) {
            float fz = face ? z1 : z0, o = face ? 1.f : -1.f;
            auto Z = [&](float a, float b) { return std::pair<float, float>{std::min(fz + o * a, fz + o * b), std::max(fz + o * a, fz + o * b)}; };
            for (int tier = 0; tier < 3; tier++) {
                float y0 = Y + tier * 2.65f;
                for (float x = 290.3f; x + 1.5f <= 337.9f; x += 1.6f) {
                    auto [ra, rb] = Z(0.f, 0.02f);
                    c.box({x + 0.1f, y0 + 0.1f, ra}, {x + 1.4f, y0 + 2.3f, rb}, cellDark);
                    auto [ba, bb] = Z(0.02f, 0.09f);
                    for (int b = 0; b < 3; b++) { float bx = x + 0.35f + b * 0.3f; c.box({bx - 0.025f, y0 + 0.1f, ba}, {bx + 0.025f, y0 + 2.3f, bb}, steel); }
                    c.box({x + 0.1f, y0 + 1.15f, ba}, {x + 1.4f, y0 + 1.22f, bb}, steel);
                }
                if (tier > 0) {   // the gallery outside the upper cells, with its railing
                    auto [ga, gb] = Z(0.f, 1.f);
                    c.box({290, y0 - 0.15f, ga}, {338, y0, gb}, steel, steel, 1.f, true);
                    float rz = fz + o * 0.97f;
                    c.box({290, y0 + 0.95f, rz - 0.03f}, {338, y0 + 1.0f, rz + 0.03f}, steel);
                    for (float px = 290; px <= 338; px += 2) c.box({px - 0.03f, y0, rz - 0.03f}, {px + 0.03f, y0 + 0.95f, rz + 0.03f}, steel);
                }
            }
        }
    }
    for (float x = 292; x < 338; x += 8) {   // lamps hanging over Broadway
        MeshBuilder& c = mb(x, -1901);
        c.beam({x, Y + 9.5f, -1901}, {x, Y + CH - 0.5f, -1901}, 0.04f, 0.04f, steel);
        c.box({x - 0.25f, Y + 9.2f, -1901.25f}, {x + 0.25f, Y + 9.5f, -1900.75f}, col(255, 236, 190, M_EMISSIVE));
    }
    layer = LAYER_MAIN;
    // the administration wing across the east end, with the way in tunneling through it, and a plaque over the door
    solid(CX1, -1910, CX1 + 9, DZ0, Y - 1, Y + 10, wall, roofC);
    solid(CX1, DZ1, CX1 + 9, -1892, Y - 1, Y + 10, wall, roofC);
    solid(CX1, DZ0, CX1 + 9, DZ1, Y + DH, Y + 10, wall, roofC);
    m.box({CX1, Y + DH - 0.02f, DZ0}, {CX1 + 9, Y + DH, DZ1}, inside, inside, 1.f, true);
    for (float z : {DZ0, DZ1 - 0.03f}) m.box({CX1, Y, z}, {CX1 + 9, Y + DH, z + 0.03f}, inside);
    m.box({CX1 + 9, Y + 4.6f, DZ0 - 1.5f}, {CX1 + 9.12f, Y + 6.f, DZ1 + 1.5f}, col(64, 60, 54));
    for (float z = DZ0 - 1.1f; z < DZ1 + 1.1f; z += 0.42f) m.box({CX1 + 9.12f, Y + 5.05f, z}, {CX1 + 9.15f, Y + 5.55f, z + 0.26f}, col(220, 214, 196));   // the lettering
    {   // flagpole on the plaza
        float fx = 362, fz = -1890, fy = terrain(fx, fz);
        m.beam({fx, fy, fz}, {fx, fy + 14, fz}, 0.12f, 0.12f, col(200, 200, 205));
        pole(fx, fz, 0.15f, fy + 14);
        for (int s = 0; s < 7; s++) m.box({fx + 0.1f, fy + 12.6f - s * 0.2f, fz - 0.02f}, {fx + 3.2f, fy + 12.8f - s * 0.2f, fz + 0.02f}, s & 1 ? col(240, 240, 240) : col(190, 40, 46));
        m.box({fx + 0.1f, fy + 12.0f, fz - 0.03f}, {fx + 1.4f, fy + 12.8f, fz + 0.03f}, col(40, 54, 120));
    }

    // ---- the recreation yard off the west end: high walls, and concrete bleachers stepping up the north wall
    solid(245, -1916, CX0, -1915, Y - 1, Y + 6, plain, roofC);
    solid(245, -1885, CX0, -1884, Y - 1, Y + 6, plain, roofC);
    solid(245, -1916, 246, -1884, Y - 1, Y + 6, plain, roofC);
    for (int s = 0; s < 4; s++) solid(251, -1915, 275, -1915 + (4 - s) * 0.9f, Y - 1, Y + (s + 1) * 0.45f, col(196, 190, 178), col(184, 178, 166));
    for (float x : {255.f, 268.f}) m.box({x - 0.1f, Y, -1886}, {x + 0.1f, Y + 3.2f, -1885.8f}, col(90, 92, 96)),   // a pair of backboards
                                    m.box({x - 0.9f, Y + 3.0f, -1886.1f}, {x + 0.9f, Y + 4.1f, -1885.95f}, col(236, 236, 230));
    {   // guard tower at the yard's corner
        MeshBuilder& g = mb(246, -1915);
        for (int k = 0; k < 4; k++) g.beam({245.f + (k & 1) * 2.4f, Y, -1917.f + (k >> 1) * 2.4f}, {245.f + (k & 1) * 2.4f, Y + 9, -1917.f + (k >> 1) * 2.4f}, 0.2f, 0.2f, col(90, 92, 96));
        g.box({244.6f, Y + 9, -1917.4f}, {247.8f, Y + 11.4f, -1914.2f}, col(180, 176, 166), roofC, 0.9f);
        g.box({244.5f, Y + 10.0f, -1917.5f}, {247.9f, Y + 11.0f, -1914.1f}, col(50, 60, 66));   // windows all round
        g.box({244.3f, Y + 11.4f, -1917.7f}, {248.1f, Y + 11.6f, -1913.9f}, roofC);
        pole(246.2f, -1915.8f, 1.3f, Y + 11.6f);
    }

    // ---- the lighthouse, and the burned shell of the Warden's House beside it
    {
        float lx = 366, lz = -1884, ly = terrain(lx, lz) - 0.5f, top = Y + 22;
        MeshBuilder& l = mb(lx, lz);
        l.prism({lx, ly, lz}, 8, 1.9f, 1.5f, top - ly, col(242, 240, 232), col(242, 240, 232), 0.39f);
        l.prism({lx, top, lz}, 8, 2.5f, 2.5f, 0.3f, col(60, 62, 64), col(60, 62, 64), 0.39f);   // gallery
        l.prism({lx, top + 0.3f, lz}, 8, 1.3f, 1.3f, 2.2f, col(255, 244, 205, M_EMISSIVE), col(255, 244, 205, M_EMISSIVE), 0.39f);
        l.prism({lx, top + 2.5f, lz}, 8, 1.6f, 0.2f, 1.3f, col(52, 54, 56), col(52, 54, 56), 0.39f);
        pole(lx, lz, 1.9f, top + 3.8f);
    }
    {
        RNG wr(1963);
        Color brick = col(158, 104, 84), burnt = col(70, 62, 58);
        const float X0 = 330, X1 = 350, Z0 = -1886.5f, Z1 = -1874;
        float base = lowest(X0, Z0, X1, Z1) - 1;
        auto wallRun = [&](float x0, float z0, float x1, float z1) {   // in 2.5 m bays, alternately a window gap and a jagged pier
            bool alongX = fabsf(x1 - x0) > fabsf(z1 - z0);
            float L = alongX ? x1 - x0 : z1 - z0;
            for (float u = 0; u < L - 0.01f; u += 2.5f) {
                float u1 = std::min(u + 2.5f, L), h = (int)(u / 2.5f) % 2 ? wr.range(1.2f, 3.f) : wr.range(4.5f, 8.5f);
                float ya = terrain(alongX ? x0 + u : x0, alongX ? z0 : z0 + u);
                float bx0 = alongX ? x0 + u : x0, bx1 = alongX ? x0 + u1 : x1, bz0 = alongX ? z0 : z0 + u, bz1 = alongX ? z1 : z0 + u1;
                solid(bx0, bz0, bx1, bz1, base, ya + h, brick, burnt);
            }
        };
        wallRun(X0, Z0, X1, Z0 + 0.5f);
        wallRun(X0, Z1 - 0.5f, X1, Z1);
        wallRun(X0, Z0 + 0.5f, X0 + 0.5f, Z1 - 0.5f);
        wallRun(X1 - 0.5f, Z0 + 0.5f, X1, Z1 - 0.5f);
        mb(340, -1880).prism({344, terrain(344, -1880) - 0.5f, -1880}, 4, 0.9f, 0.8f, 10.5f, brick, burnt, 0.79f);   // the chimney still standing
        pole(344, -1880, 0.9f, terrain(344, -1880) + 10);
    }

    // ---- the water tower on its stilts, and the smokestack
    {
        float wx = 380, wz = -1928, wy = terrain(wx, wz), ty = wy + 15;
        MeshBuilder& w = mb(wx, wz);
        Color legC = col(150, 146, 138), tank = col(214, 206, 190);
        for (int k = 0; k < 4; k++) {
            float lx = wx + (k & 1 ? 3.5f : -3.5f), lz = wz + (k & 2 ? 3.5f : -3.5f);
            w.beam({lx, terrain(lx, lz) - 0.5f, lz}, {lx, ty, lz}, 0.35f, 0.35f, legC);
            pole(lx, lz, 0.3f, ty);
        }
        for (float y : {wy + 5.f, wy + 10.f}) {   // cross braces
            w.beam({wx - 3.5f, y, wz - 3.5f}, {wx + 3.5f, y + 4, wz - 3.5f}, 0.12f, 0.12f, legC);
            w.beam({wx - 3.5f, y, wz + 3.5f}, {wx + 3.5f, y + 4, wz + 3.5f}, 0.12f, 0.12f, legC);
            w.beam({wx - 3.5f, y, wz - 3.5f}, {wx - 3.5f, y + 4, wz + 3.5f}, 0.12f, 0.12f, legC);
            w.beam({wx + 3.5f, y, wz - 3.5f}, {wx + 3.5f, y + 4, wz + 3.5f}, 0.12f, 0.12f, legC);
        }
        w.prism({wx, ty, wz}, 14, 4.4f, 4.4f, 6.5f, tank, tank, 0.f, true, 0.9f);
        w.prism({wx, ty + 2.2f, wz}, 14, 4.45f, 4.45f, 1.1f, col(176, 70, 56), col(176, 70, 56), 0.f, false);   // faded red lettering band
        w.prism({wx, ty + 6.5f, wz}, 14, 4.6f, 0.4f, 2.f, col(150, 140, 128), col(150, 140, 128), 0.f, true);
        addPlat({wx - 4.4f, wx + 4.4f, wz - 4.4f, wz + 4.4f, ty + 6.5f, ty + 6.5f, ty, 0, PK_BUILDING});
    }
    {
        float sx = 262, sz = -1925, sy = terrain(sx, sz) - 0.5f;
        mb(sx, sz).prism({sx, sy, sz}, 10, 1.7f, 1.2f, Y + 26 - sy, col(176, 164, 150), col(60, 56, 52), 0.f, true, 0.9f);
        pole(sx, sz, 1.6f, Y + 26);
    }

    // ---- down at the dock: Building 64 (the old barracks) with its sign, a pier, a guard tower and the ferry
    {
        const float X0 = 403, X1 = 424, Z0 = -1927, Z1 = -1905;
        float base = lowest(X0, Z0, X1, Z1), top = base + 14;
        solid(X0, Z0, X1, Z1, base - 1, top, col(214, 200, 166, M_HOUSE), roofC);
        MeshBuilder& b = mb(X1, (Z0 + Z1) / 2);
        b.box({X1 - 0.5f, top, Z0 + 3}, {X1, top + 2.4f, Z1 - 3}, col(236, 232, 220));   // the sign on the roof
        for (float z = Z0 + 3.6f; z < Z1 - 3.6f; z += 0.55f)
            if (hashu((int)(z * 10), 64, 1) % 5) b.box({X1 + 0.0f, top + 0.7f, z}, {X1 + 0.04f, top + 1.6f, z + 0.35f}, col(36, 36, 40));
        float px0 = 426, px1 = 455, pz0 = -1892, pz1 = -1880, py = 3.f;
        b.box({px0, py - 0.4f, pz0}, {px1, py, pz1}, col(122, 104, 84), col(134, 116, 94), 0.9f, true);
        addPlat({px0, px1, pz0, pz1, py, py, py - 0.4f, 0, PK_PIER});
        for (float x = px0 + 2; x < px1; x += 4)
            for (float z : {pz0 + 0.4f, pz1 - 0.4f}) b.beam({x, -4, z}, {x, py - 0.4f, z}, 0.4f, 0.4f, col(84, 70, 58));
        for (float x = px0 + 4; x < px1; x += 8) b.prism({x, py, pz1 - 0.5f}, 6, 0.25f, 0.22f, 0.6f, col(40, 40, 42), col(40, 40, 42));   // bollards
        MeshBuilder& g = mb(450, -1897);   // guard tower standing in the water off the pier
        for (int k = 0; k < 4; k++) g.beam({448.f + (k & 1) * 3, -3, -1899.f + (k >> 1) * 3}, {448.f + (k & 1) * 3, 9, -1899.f + (k >> 1) * 3}, 0.3f, 0.3f, col(110, 108, 104));
        g.box({447.6f, 9, -1899.4f}, {451.4f, 11.6f, -1895.6f}, col(196, 190, 176), roofC, 0.9f);
        g.box({447.5f, 10.2f, -1899.5f}, {451.5f, 11.2f, -1895.5f}, col(50, 60, 66));
        g.box({447.2f, 11.6f, -1899.8f}, {451.8f, 11.8f, -1895.2f}, roofC);
        // the ferry tied up along the pier's south side
        MeshBuilder& f = mb(440, -1874);
        f.box({429, -1.f, -1878.8f}, {452, 2.2f, -1870.4f}, col(232, 234, 236), col(160, 120, 90), 0.85f);
        f.box({429, 0.2f, -1878.85f}, {452, 0.6f, -1870.35f}, col(40, 70, 130));   // stripe at the waterline
        f.box({433, 2.2f, -1878.f}, {447, 4.8f, -1871.2f}, col(240, 240, 240), col(210, 210, 214), 0.9f);
        f.box({432.9f, 3.2f, -1878.1f}, {447.1f, 4.2f, -1871.1f}, col(50, 64, 76));   // cabin windows
        f.box({443, 4.8f, -1876.5f}, {446.5f, 6.6f, -1872.7f}, col(240, 240, 240), col(200, 200, 204), 0.9f);   // wheelhouse
        addPlat({429, 452, -1878.8f, -1870.4f, 2.2f, 2.2f, -1.f, 0, PK_BUILDING});
    }

    // ---- the gardens gone wild on the slopes: cypress, eucalyptus and flowering shrubs, kept off the road and buildings
    {
        RNG gr(1934);
        const Rectangle keep[] = {{240, -1920, 125, 40}, {326, -1890, 46, 20}, {370, -1938, 20, 20}, {398, -1932, 32, 32}, {255, -1932, 14, 14}};
        auto clear = [&](float x, float z) {
            float ry;
            if (alcatrazRoad(x, z, &ry) < 12.5f || terrain(x, z) < 4.f) return false;
            for (auto& k : keep) if (x > k.x && x < k.x + k.width && z > k.y && z < k.y + k.height) return false;
            return true;
        };
        for (int k = 0, tries = 0; k < 12 && tries < 400; tries++) {
            float x = gr.range(185, 420), z = gr.range(-1970, -1830), d = alcatrazD(x, z);
            if (d < 0.6f || d > 0.92f || !clear(x, z)) continue;
            addTree(x, z, gr.chance(0.5f) ? 1 : 3, gr.range(0.9f, 1.25f), gr, true);
            k++;
        }
        for (int k = 0, tries = 0; k < 45 && tries < 900; tries++) {
            float x = gr.range(185, 420), z = gr.range(-1970, -1830), d = alcatrazD(x, z);
            if (d < 0.55f || d > 0.95f || !clear(x, z)) continue;
            addShrub(x, z, gr.range(0.7f, 1.3f), gr, gr.chance(0.6f));
            k++;
        }
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
    tok("Coit Tower", 880, -904);   // behind the tower, well away from the teleport spot on the south plaza
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
    tok("Pier 39", 1025, -1265);   // on the deck just south of the carousel (+z) (1018..1032, -1292..-1278), not on its 8 m roof
    tok("Salesforce Tower", 1100, 210);
    tok("Embarcadero Plaza", 1230, -256);
    tok("SFO Runway 28R", 1560, 2262);
    tok("Alcatraz", 263, -1893);   // out in the yard: skate in the front door and through the cellhouse to get it
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
               {1277, -170, 5, 10},    {gx(19) + 55, gz(6) + 55, 25, 14}, {1815, 1880, 25, 16}, {2380, 9400, 14, 10}, {1900, 9418, 30, 10},
               {360, -1900, 4, 6},     {263, -1900, 10, 8},    {438, -1886, 8, 5}};   // Alcatraz: by the front door, in the yard, on the pier
    // tourists stand on the ground (the terrain, or a kerb or plaza a step up from it), not on whatever roof or
    // monument top is highest there. Where that ground is a roadway or the water, they stand on a deck or pier
    // above it instead (SFO's departures level, the piers), if there is one. A spot inside a building or a
    // monument, or out in the traffic, is skipped and another one tried, so each landmark still gets its crowd
    auto road = [&](float x, float z, bool deck) {   // on a car path (on a deck or not), or a city-grid street
        for (size_t k = 0; k < carPaths.size(); k++) {
            const Path& P = carPaths[k];
            if (P.onDeck != deck) continue;
            for (size_t s = 0; s + 1 < P.p.size(); s++)
                if (distToSeg2D(x, z, P.p[s].x, P.p[s].y, P.p[s + 1].x, P.p[s + 1].y) < carPathLanes[k] * 4.f) return true;
        }
        if (deck) return false;
        for (const Grid* g : {&sf, &oak, &mp})
            for (int j = 0; j < g->nj; j++)
                for (int i = 0; i < g->ni; i++)
                    for (int d : {0, 2}) {
                        if (!g->edge(i, j, d)) continue;
                        Vector2 m = g->node(i, j), e = g->node(i + DI[d], j + DJ[d]);
                        if (distToSeg2D(x, z, m.x, m.y, e.x, e.y) < g->roadHalf) return true;
                    }
        return false;
    };
    auto deckOver = [&](float x, float z, float y) {   // the lowest deck or pier over y (up to 10 m up), else 1e9
        float best = 1e9f, lim = std::max(terrain(x, z), 0.f) + 10.f;
        for (const Platform& pl : plats)
            if ((pl.kind == PK_DECK || pl.kind == PK_PIER) && pl.contains(x, z) && std::min(pl.x1 - pl.x0, pl.z1 - pl.z0) > 4.f) {
                float t = pl.top(x, z);
                if (t > y + STEP_UP && t <= lim) best = std::min(best, t);
            }
        return best;
    };
    for (auto& t : ts)
        for (int k = 0, tries = 0; k < t.n && tries < t.n * 10; tries++) {
            float a = r.range(0, TAU_F), d = r.range(2, t.rad);
            float x = t.x + cosf(a) * d, z = t.z + sinf(a) * d;
            if (onBridge(x, z) || fabsf(x + 1100) < 17) continue;
            Vector3 p = {x, ground(x, z, terrain(x, z)), z};
            if (isWater(x, z, p.y) || road(x, z, false)) {
                p.y = deckOver(x, z, p.y);
                if (p.y > 1e8f || ground(x, z, p.y) > p.y + 0.05f || road(x, z, true)) continue;
            }
            Vector3 tmp = p;
            Vector2 n;
            if (collide(tmp, 0.5f, p.y, 1.8f, &n)) continue;
            tourists.push_back(p);
            k++;
        }
    // people out on the lawns of the parks on a blanket, many with a laptop (this is San Francisco)
    {
        RNG lr(4242);   // its own stream, so nothing else in the city moves
        struct Lawn { float x0, z0, x1, z1; int n; };
        const Lawn lawns[] = {{gx(12) + 6, gz(17) + 6, gx(13) - 6, gz(18) - 6, 22},   // Dolores Park
                              {gx(9) + 8, gz(11) + 8, gx(10) - 8, gz(12) - 8, 7},     // Alamo Square
                              {gx(20) + 8, gz(2) + 8, gx(21) - 8, gz(3) - 8, 6},      // Washington Square
                              {gx(11) + 8, gz(5) + 8, gx(12) - 8, gz(6) - 8, 5}};     // Lafayette Park
        for (auto& l : lawns)
            for (int k = 0, tries = 0; k < l.n && tries < l.n * 12; tries++) {
                float x = lr.range(l.x0, l.x1), z = lr.range(l.z0, l.z1);
                Vector3 p = {x, G(x, z), z}, tmp = p;
                Vector2 n;
                if (collide(tmp, 0.9f, p.y, 1.2f, &n)) continue;
                if (fabsf(terrain(x + 1, z) - terrain(x - 1, z)) > 0.6f || fabsf(terrain(x, z + 1) - terrain(x, z - 1)) > 0.6f) continue;   // too steep to sit
                bool close = false;
                for (auto& s : seats) if (s.grass && sq(s.pos.x - x) + sq(s.pos.z - z) < 9) close = true;
                if (close) continue;
                seats.push_back({p, lr.range(-PI, PI), true});
                k++;
            }
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
    sf.setEdge(20, 2, 0, false); // Coit Tower's base (880, -880) stands on this block of the street up Telegraph Hill
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
    buildIntersections(sf);
    buildStreetFurniture(oak, true);
    buildParks();
    progress("Drawing the map", 0.9f);
    buildMeta();
    buildMap();
    progress("Uploading to the GPU", 0.96f);
}
