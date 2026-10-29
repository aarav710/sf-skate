// Caltrain down the Peninsula and Menlo Park at the end of the line.
#include "world_internal.h"
#include "models.h"

// ---------------------------------------------------------------- Caltrain
void World::buildCaltrain() {
    const float L = railLen();
    Color ballast = col(120, 112, 102, M_GROUND), steel = col(150, 150, 156), tie = col(96, 84, 72);
    Color pole = col(160, 162, 166), wire = col(40, 40, 44);
    const float tracks[2] = {-2.4f, 2.4f};
    for (float s = 0; s < L; s += 5) {
        float s1 = std::min(s + 5, L);
        Vector2 d;
        Vector3 a = trackPoint(s, 0, &d), b = trackPoint(s1, 0, nullptr);
        Vector2 r = {-d.y, d.x};
        auto P = [&](Vector3 c, float lat, float dy) { return V3(c.x + r.x * lat, c.y + dy, c.z + r.y * lat); };
        MeshBuilder& m = mb(a.x, a.z);
        m.quad(P(a, -5.2f, 0), P(b, -5.2f, 0), P(b, 5.2f, 0), P(a, 5.2f, 0), ballast);
        m.quad(P(a, -7.5f, -0.8f), P(b, -7.5f, -0.8f), P(b, -5.2f, 0), P(a, -5.2f, 0), scaleC(ballast, 0.9f));
        m.quad(P(a, 5.2f, 0), P(b, 5.2f, 0), P(b, 7.5f, -0.8f), P(a, 7.5f, -0.8f), scaleC(ballast, 0.9f));
        for (float tr : tracks) {
            for (float q = 0; q < 5; q += 2.5f) {
                Vector3 c = Vector3Lerp(a, b, q / 5);
                m.beam(P(c, tr - 1.3f, 0.08f), P(c, tr + 1.3f, 0.08f), 0.3f, 0.16f, tie);
            }
            for (float o : {-0.72f, 0.72f}) m.beam(P(a, tr + o, 0.25f), P(b, tr + o, 0.25f), 0.09f, 0.16f, steel);
        }
    }
    // electrified: portal poles and contact wires
    for (float s = 20; s < L; s += 55) {
        Vector2 d;
        Vector3 c = trackPoint(s, 0, &d);
        Vector2 r = {-d.y, d.x};
        MeshBuilder& m = mb(c.x, c.z);
        for (int side = -1; side <= 1; side += 2) {
            Vector3 base = {c.x + r.x * side * 6.4f, c.y - 0.6f, c.z + r.y * side * 6.4f};
            m.beam(base, {base.x, c.y + 8.2f, base.z}, 0.35f, 0.35f, pole);
            if (inPlay(base.x, base.z))
                addPlat({base.x - 0.2f, base.x + 0.2f, base.z - 0.2f, base.z + 0.2f, c.y + 8, c.y + 8, c.y - 1, 0, PK_POLE});
        }
        m.beam({c.x - r.x * 6.4f, c.y + 7.8f, c.z - r.y * 6.4f}, {c.x + r.x * 6.4f, c.y + 7.8f, c.z + r.y * 6.4f}, 0.3f, 0.3f, pole);
    }
    for (float s = 0; s < L; s += 25) {
        float s1 = std::min(s + 25, L);
        for (float tr : tracks) {
            Vector3 a = trackPoint(s, tr, nullptr), b = trackPoint(s1, tr, nullptr);
            mb(a.x, a.z).beam({a.x, a.y + 6.2f, a.z}, {b.x, b.y + 6.2f, b.z}, 0.04f, 0.04f, wire);
        }
    }
    // stations
    struct St { const char* name; Vector2 at; Color bldg; };
    St list[] = {{"4th & King", {1210, 950}, col(214, 200, 170)},  {"Millbrae", {1300, 2640}, col(205, 205, 210)},
                 {"San Mateo", {1900, 5000}, col(226, 206, 160)},  {"Redwood City", {2330, 7470}, col(205, 192, 176)},
                 {"Menlo Park", {2428, 9400}, col(238, 204, 92)}};
    Color plat = col(176, 174, 168, M_GROUND), edge = col(236, 200, 50, M_GROUND), canopy = col(200, 202, 206);
    for (auto& st : list) {
        float s;
        railDistance(st.at.x, st.at.y, &s);
        stations.push_back({st.name, s});
        Vector2 d;
        Vector3 c = trackPoint(s, 0, &d);
        float yaw = atan2f(d.y, d.x);
        bool playable = inPlay(c.x, c.z);
        for (int side = -1; side <= 1; side += 2) {
            Vector3 a = trackPoint(s - 70, side * 6.2f, nullptr), b = trackPoint(s + 70, side * 6.2f, nullptr);
            float top = c.y + 0.47f;
            MeshBuilder& m = mb(c.x, c.z);
            m.beam({a.x, top - 0.6f, a.z}, {b.x, top - 0.6f, b.z}, 4.0f, 1.2f, plat);
            Vector3 ea = trackPoint(s - 70, side * 4.4f, nullptr), eb = trackPoint(s + 70, side * 4.4f, nullptr);
            m.beam({ea.x, top + 0.01f, ea.z}, {eb.x, top + 0.01f, eb.z}, 0.5f, 0.02f, edge);
            if (playable)
                addPlat({std::min(a.x, b.x) - 2, std::max(a.x, b.x) + 2, std::min(a.z, b.z) - 2, std::max(a.z, b.z) + 2, top, top, c.y - 1, 0, PK_PROP});
            // canopy over the middle of each platform
            for (float k = -40; k <= 40; k += 20) {
                Vector3 p = trackPoint(s + k, side * 6.8f, nullptr);
                m.beam({p.x, top, p.z}, {p.x, top + 4.2f, p.z}, 0.25f, 0.25f, canopy);
            }
            Vector3 ca = trackPoint(s - 45, side * 6.4f, nullptr), cb = trackPoint(s + 45, side * 6.4f, nullptr);
            m.beam({ca.x, top + 4.3f, ca.z}, {cb.x, top + 4.3f, cb.z}, 3.6f, 0.25f, canopy);
        }
        if (strcmp(st.name, "Menlo Park") == 0) continue; // its historic depot is built with the town
        Vector3 bp = trackPoint(s, 16, nullptr);
        mb(bp.x, bp.z).obox({bp.x, c.y + 3, bp.z}, {14, 3.6f, 4.5f}, yaw, 0, withMat(st.bldg, M_HOUSE));
        if (playable) addPlat({bp.x - 14, bp.x + 14, bp.z - 14, bp.z + 14, c.y + 6.6f, c.y + 6.6f, c.y - 1, 0, PK_BUILDING});
    }
}

// ---------------------------------------------------------------- Menlo Park homes
static const Color WALLS[] = {col(242, 240, 232), col(236, 226, 202), col(176, 190, 200), col(186, 196, 176),
                              col(210, 194, 168), col(170, 104, 84),  col(225, 222, 214), col(150, 160, 150)};
static const Color ROOFS[] = {col(72, 72, 78), col(92, 78, 68), col(112, 82, 62), col(84, 92, 96), col(128, 64, 50)};

void World::addEstate(float x0, float z0, float x1, float z1, int facing, RNG& r) {
    auto ok = [&](float x, float z) {
        float s;
        return terrain(x, z) > 1.2f && inPlay(x, z) && railDistance(x, z, &s) > 20;
    };
    if (!ok(x0, z0) || !ok(x1, z1) || !ok(x0, z1) || !ok(x1, z0)) return;
    float hw = r.range(18, 24), hd = r.range(12, 15), setback = r.range(13, 18);
    float cx = lerpf(x0, x1, r.range(0.42f, 0.58f));
    float hz0 = facing == 0 ? z0 + setback : z1 - setback - hd, hz1 = hz0 + hd;
    float hx0 = cx - hw / 2, hx1 = cx + hw / 2;
    float h00 = terrain(hx0, hz0), h11 = terrain(hx1, hz1), h01 = terrain(hx0, hz1), h10 = terrain(hx1, hz0);
    float base = std::min(std::min(h00, h11), std::min(h01, h10)) - 0.6f, floor = std::max(std::max(h00, h11), std::max(h01, h10));
    Color wall = withMat(WALLS[r.irange(0, 7)], M_HOUSE), roof = ROOFS[r.irange(0, 4)];
    float top = floor + r.range(6.2f, 7.4f);
    MeshBuilder& m = mb(cx, (hz0 + hz1) / 2);
    addBox(hx0, hz0, hx1, hz1, base, top, PK_BUILDING, wall, roof, 0.8f);
    if (r.chance(0.65f)) m.hip(hx0 - 0.7f, hx1 + 0.7f, hz0 - 0.7f, hz1 + 0.7f, top, r.range(3.0f, 4.2f), roof);
    else m.gable(hx0 - 0.5f, hx1 + 0.5f, hz0 - 0.6f, hz1 + 0.6f, top, r.range(3.5f, 4.5f), 0, roof, wall);
    float fz = facing == 0 ? hz0 : hz1, out = facing == 0 ? -1.f : 1.f;   // front facade line, outward direction
    float streetZ = facing == 0 ? z0 - 4 : z1 + 4;
    // front door with a small portico
    m.box({cx - 1.0f, floor - 0.3f, std::min(fz, fz + out * 0.1f)}, {cx + 1.0f, floor + 2.4f, std::max(fz, fz + out * 0.1f)}, col(60, 44, 36));
    m.box({cx - 2.2f, floor + 2.8f, std::min(fz, fz + out * 2.2f)}, {cx + 2.2f, floor + 3.1f, std::max(fz, fz + out * 2.2f)}, col(240, 240, 236));
    for (float px : {cx - 2.0f, cx + 2.0f})
        m.box({px - 0.15f, floor - 0.3f, std::min(fz + out * 2.0f, fz + out * 2.2f)}, {px + 0.15f, floor + 2.8f, std::max(fz + out * 2.0f, fz + out * 2.2f)}, col(240, 240, 236));
    groundPad(*this, cx - 0.8f, std::min(streetZ, fz), cx + 0.8f, std::max(streetZ, fz), 0.07f, col(196, 190, 180, M_GROUND));
    // attached garage + driveway (+ a car in it sometimes)
    bool right = r.chance(0.5f);
    float gx0 = right ? hx1 : hx0 - 7.5f, gx1 = gx0 + 7.5f;
    float gz0 = facing == 0 ? hz0 : hz1 - 7.5f, gz1 = gz0 + 7.5f;
    addBox(gx0, gz0, gx1, gz1, base, floor + 3.4f, PK_BUILDING, wall, roof, 0.8f);
    m.hip(gx0 - 0.4f, gx1 + 0.4f, gz0 - 0.4f, gz1 + 0.4f, floor + 3.4f, 1.6f, roof);
    m.box({gx0 + 1, floor - 0.3f, std::min(fz, fz + out * 0.08f)}, {gx1 - 1, floor + 2.5f, std::max(fz, fz + out * 0.08f)}, col(232, 230, 224));
    groundPad(*this, gx0 + 0.5f, std::min(streetZ, fz), gx1 - 0.5f, std::max(streetZ, fz), 0.075f, col(188, 184, 176, M_GROUND));
    if (r.chance(0.45f)) {
        float cz = (fz + streetZ) / 2 + out * 1.5f, cxx = (gx0 + gx1) / 2;
        int type = r.chance(0.5f) ? CT_SUV : CT_SEDAN;
        const CarModel& cm = carModel(type);
        float y = terrain(cxx, cz) + 0.08f;
        addCarToMesh(m, type, randomCarColor(r), {cxx, y, cz}, facing == 0 ? -PI / 2 : PI / 2, 0);
        addPlat({cxx - cm.halfWid, cxx + cm.halfWid, cz - cm.halfLen, cz + cm.halfLen, y + cm.height - 0.1f, y + cm.height - 0.1f, y - 0.5f, 0, PK_CAR});
    }
    // hedge or white picket fence along the front, with gaps for the walk and driveway
    float lineZ = facing == 0 ? z0 + 0.6f : z1 - 0.6f;
    bool hedge = r.chance(0.6f);
    for (float x = x0 + 1; x < x1 - 1; x += 2) {
        if (fabsf(x + 1 - cx) < 2.0f || (x + 1 > gx0 - 0.5f && x + 1 < gx1 + 0.5f)) continue;
        float y = terrain(x + 1, lineZ);
        if (hedge) {
            addPlat({x, x + 2, lineZ - 0.6f, lineZ + 0.6f, y + 1.2f, y + 1.2f, y - 0.3f, 0, PK_PROP});
            for (int k = 0; k < 2; k++) addShrub(x + 0.5f + k, lineZ, 1.5f, r, false);   // clipped hedge of clumped leaves
        }
        else {
            mb(x, lineZ).box({x, y + 0.5f, lineZ - 0.05f}, {x + 2, y + 0.65f, lineZ + 0.05f}, col(246, 246, 242));
            for (float px = x; px < x + 2; px += 0.4f) mb(x, lineZ).box({px, y - 0.1f, lineZ - 0.04f}, {px + 0.12f, y + 1.0f, lineZ + 0.04f}, col(246, 246, 242));
            addPlat({x, x + 2, lineZ - 0.1f, lineZ + 0.1f, y + 1.0f, y + 1.0f, y - 0.5f, 0, PK_PROP});
        }
    }
    // backyard pool
    if (r.chance(0.45f)) {
        float pz0 = facing == 0 ? hz1 + 5 : hz0 - 10, pz1 = pz0 + 5;
        float px0 = cx - 5, px1 = cx + 5;
        groundPad(*this, px0 - 0.8f, pz0 - 0.8f, px1 + 0.8f, pz1 + 0.8f, 0.06f, col(232, 228, 216, M_GROUND));
        float y = terrain(cx, (pz0 + pz1) / 2);
        mb(cx, pz0).quad({px0, y + 0.08f, pz0}, {px1, y + 0.08f, pz0}, {px1, y + 0.08f, pz1}, {px0, y + 0.08f, pz1}, col(60, 160, 200, M_WATER));
        ponds.push_back({px0, pz0, px1, pz1});
    }
    // oaks and the odd stand of redwoods
    int n = r.irange(2, 4);
    for (int k = 0; k < n * 4 && n > 0; k++) {
        float tx = r.range(x0 + 4, x1 - 4), tz = r.range(z0 + 4, z1 - 4);
        if (tx > hx0 - 7 && tx < hx1 + 7 && tz > hz0 - 7 && tz < hz1 + 7) continue;
        if (tx > gx0 - 7 && tx < gx1 + 7) continue;
        addTree(tx, tz, 4, r.range(0.85f, 1.25f), r, true);
        n--;
    }
    if (r.chance(0.25f)) {
        float tz = facing == 0 ? z1 - 6 : z0 + 6;
        for (int k = 0; k < 3; k++) addTree(x0 + 6 + k * 4.5f, tz + r.range(-2, 2), 5, r.range(0.8f, 1.1f), r, true);
    }
}

// ---------------------------------------------------------------- Menlo Park
void World::buildMenlo() {
    RNG r(650);
    mp.x0 = 1380; mp.z0 = 8850; mp.sp = 140; mp.ni = 14; mp.nj = 11;
    mp.roadHalf = 5.0f; mp.walkOff = 6.8f;
    buildGrid(mp);
    for (int j = 0; j < mp.nj; j++)
        if (j != 1 && j != 4 && j != 8) mp.setEdge(7, j, 0, false); // only a few grade crossings over the tracks
    buildRoads(mp, 1);
    const float ECR = mp.node(6, 0).x, SCA = mp.node(0, 4).y; // El Camino Real, Santa Cruz Avenue
    Color sidewalk = C_SIDEWALK, white = col(232, 232, 226, M_GROUND);

    // El Camino Real: wide boulevard with a planted median
    roadStrip(*this, {ECR, MZ0 + 5}, {ECR, MZ1 - 5}, 11, 11, 0, 0.09f, C_ASPHALT);
    groundPad(*this, ECR - 1.5f, MZ0 + 5, ECR + 1.5f, MZ1 - 5, 0.2f, col(96, 140, 70, M_GROUND));
    for (float z = MZ0 + 20; z < MZ1 - 10; z += 12)
        for (float lx : {-5.5f, 5.5f}) groundPad(*this, ECR + lx - 0.08f, z, ECR + lx + 0.08f, z + 5, 0.11f, white);
    for (float z = MZ0 + 25; z < MZ1 - 20; z += 28) {
        if (fabsf(z - SCA) < 15) continue;
        addTree(ECR, z, 0, 1.25f, r, true);
    }
    for (float z = MZ0 + 30; z < MZ1 - 20; z += 45)
        for (int side = -1; side <= 1; side += 2) addLamp(ECR + side * 12.2f, z, side > 0 ? PI : 0);
    {
        Path P;
        P.p = {{ECR, MZ0 + 5}, {ECR, MZ1 - 5}};
        P.finish();
        carPaths.push_back(P);
        carPathLanes.push_back(2);
        carPathKind.push_back(2);
    }

    // downtown Santa Cruz Avenue: sidewalks, street trees, shops
    float dx0 = mp.node(3, 0).x, dx1 = mp.node(7, 0).x - 8;
    for (int side = -1; side <= 1; side += 2) {
        groundPad(*this, dx0, side < 0 ? SCA - 9.5f : SCA + 5, dx1, side < 0 ? SCA - 5 : SCA + 9.5f, 0.12f, sidewalk);
        for (float x = dx0 + 9; x < dx1 - 5; x += 16) {
            if (fabsf(x - ECR) < 14) continue;
            addTree(x, SCA + side * 7.6f, 0, 0.85f, r, true);
        }
        for (float x = dx0 + 17; x < dx1 - 5; x += 32) {
            if (fabsf(x - ECR) < 14) continue;
            addLamp(x, SCA + side * 5.6f, side > 0 ? -PI / 2 : PI / 2);
        }
    }
    static const Color SHOP[] = {col(232, 214, 180), col(214, 180, 150), col(240, 232, 214), col(196, 160, 130),
                                 col(220, 200, 170), col(180, 190, 170), col(206, 140, 110), col(160, 170, 190)};

    for (int j = 0; j + 1 < mp.nj; j++)
        for (int i = 0; i + 1 < mp.ni; i++) {
            Vector2 a = mp.node(i, j), b = mp.node(i + 1, j + 1);
            float bx0 = a.x + (i == 6 ? 13.f : 9.f), bx1 = b.x - (i + 1 == 6 ? 13.f : 9.f);
            float bz0 = a.y + 9, bz1 = b.y - 9;
            if (i == 7) continue;                                   // the railroad block
            bool downtown = (i >= 3 && i <= 5) && (j == 3 || j == 4);
            if (downtown) {
                // shops along Santa Cruz Ave, a parking lot behind
                int facing = j == 3 ? 1 : 0;
                float sz0 = facing == 1 ? bz1 - 18 : bz0, sz1 = sz0 + 18;
                float x = bx0;
                while (x < bx1 - 6) {
                    float w = std::min(r.range(9, 17), bx1 - x);
                    addBuilding(x + 0.1f, sz0, x + w - 0.1f, sz1, r.range(4.5f, 8.5f), withMat(SHOP[r.irange(0, 7)], M_HOUSE),
                                col(150, 120, 100), BS_WHARF, facing, r);
                    x += w;
                }
                float pz0 = facing == 1 ? bz0 + 4 : sz1 + 6, pz1 = facing == 1 ? sz0 - 6 : bz1 - 4;
                groundPad(*this, bx0 + 4, pz0, bx1 - 4, pz1, 0.08f, C_ASPHALT);
                for (float px = bx0 + 8; px < bx1 - 8; px += 3.2f) {
                    if (!r.chance(0.45f)) continue;
                    float cz = (pz0 + pz1) / 2 + (r.chance(0.5f) ? -6.f : 6.f);
                    float y = terrain(px, cz) + 0.08f;
                    addCarToMesh(mb(px, cz), r.chance(0.3f) ? CT_SUV : CT_SEDAN, randomCarColor(r), {px, y, cz}, PI / 2, 0);
                    addPlat({px - 0.95f, px + 0.95f, cz - 2.3f, cz + 2.3f, y + 1.4f, y + 1.4f, y - 0.5f, 0, PK_CAR});
                }
                continue;
            }
            if (i == 6) {
                // low commercial strip facing El Camino
                float z = bz0;
                while (z < bz1 - 10) {
                    float w = std::min(r.range(24, 40), bz1 - z);
                    float s;
                    if (railDistance(bx0 + 25, z + w / 2, &s) > 22)
                        addBuilding(bx0, z + 1, bx0 + 26, z + w - 1, r.range(5, 10), withMat(SHOP[r.irange(0, 7)], M_OFFICE),
                                    col(140, 140, 138), BS_WHARF, 2, r);
                    z += w;
                }
                groundPad(*this, bx0 + 30, bz0 + 2, bx1 - 4, bz1 - 2, 0.08f, C_ASPHALT);
                continue;
            }
            // residential: four big lots per block
            float mx = (bx0 + bx1) / 2, mz = (bz0 + bz1) / 2;
            addEstate(bx0, bz0, mx - 1, mz - 1, 0, r);
            addEstate(mx + 1, bz0, bx1, mz - 1, 0, r);
            addEstate(bx0, mz + 1, mx - 1, bz1, 1, r);
            addEstate(mx + 1, mz + 1, bx1, bz1, 1, r);
        }
    // street trees on the verges of residential streets
    for (int j = 0; j < mp.nj; j++)
        for (int i = 0; i < mp.ni; i++)
            for (int d = 0; d < 4; d += 2) {
                if (!mp.edge(i, j, d)) continue;
                Vector2 a = mp.node(i, j), b = mp.node(i + DI[d], j + DJ[d]);
                Vector2 dir = Vector2Normalize(Vector2Subtract(b, a)), rr = {-dir.y, dir.x};
                bool dt = d == 0 && j == 4 && i >= 3 && i < 7;
                if (dt || (d == 2 && i == 6)) continue;
                for (float s = 14; s < mp.sp - 12; s += 20)
                    for (int side = -1; side <= 1; side += 2) {
                        if (!r.chance(0.55f)) continue;
                        Vector2 p = Vector2Add(Vector2Add(a, Vector2Scale(dir, s + r.range(-3, 3))), Vector2Scale(rr, side * 7.0f));
                        float ss;
                        if (railDistance(p.x, p.y, &ss) < 14) continue;
                        addTree(p.x, p.y, r.chance(0.7f) ? 4 : (r.chance(0.5f) ? 0 : 5), r.range(0.8f, 1.15f), r, true);
                    }
            }

    // the historic Menlo Park depot (yellow Victorian, 1867) and its plaza
    {
        float s = stations.back().s;
        Vector3 c = trackPoint(s, 0, nullptr);
        float y = c.y;
        float x0 = c.x - 25, x1 = c.x - 14;
        groundPad(*this, mp.node(7, 0).x + 5, c.z - 45, c.x - 8.5f, c.z + 45, 0.12f, C_PLAZA);
        Color yel = col(238, 204, 92, M_HOUSE), trim = col(250, 248, 240), brown = col(120, 72, 52);
        addBox(x0, c.z - 16, x1, c.z + 16, y - 1, y + 5.2f, PK_BUILDING, yel, yel);
        MeshBuilder& m = mb(c.x, c.z);
        m.gable(x0 - 0.8f, x1 + 0.8f, c.z - 17, c.z + 17, y + 5.2f, 3.4f, 1, brown, yel);
        m.box({x0 - 0.2f, y + 4.9f, c.z - 16.2f}, {x1 + 0.2f, y + 5.25f, c.z + 16.2f}, trim);
        m.box({x1, y + 3.1f, c.z - 16}, {x1 + 2.6f, y + 3.3f, c.z + 16}, brown); // trackside awning
        for (float z = c.z - 15; z <= c.z + 15; z += 5) m.box({x1 + 2.3f, y, z - 0.12f}, {x1 + 2.5f, y + 3.1f, z + 0.12f}, trim);
        for (int k = 0; k < 4; k++) addTree(c.x - 34, c.z - 36 + k * 24, 4, 1.15f, r, true);
        for (float z = c.z - 30; z < c.z + 30; z += 12) addLedge(c.x - 31, z, c.x - 29.5f, z + 5, 0.48f, col(140, 110, 80)); // benches
    }

    // bayfront tech campus with a rooftop park, and the salt ponds beyond
    {
        float y = terrain(3440, 9410);
        roadStrip(*this, {mp.node(13, 4).x, SCA}, {3300, SCA}, 5, 5, 0, 0.08f, C_ASPHALT);
        groundPad(*this, 3270, 9230, 3620, 9590, 0.07f, col(150, 150, 150, M_GROUND));
        addBox(3300, 9280, 3590, 9500, y - 1, y + 13, PK_BUILDING, col(150, 172, 186, M_GLASS), col(96, 142, 70));
        MeshBuilder& m = mb(3440, 9390);
        for (int k = 0; k < 40; k++) {
            float tx = r.range(3310, 3580), tz = r.range(9290, 9490), ty = y + 13;
            m.beam({tx, ty, tz}, {tx, ty + 2.4f, tz}, 0.3f, 0.3f, col(96, 72, 52));
            m.prism({tx, ty + 2.0f, tz}, 6, 0.4f, 2.2f, 1.4f, col(72, 124, 58), col(72, 124, 58), r.f(), false);
            m.prism({tx, ty + 3.4f, tz}, 6, 2.2f, 0.3f, 1.8f, col(80, 132, 62), col(72, 124, 58), r.f(), true);
        }
        for (float x = 3320; x < 3580; x += 10) m.box({x, y + 13, 9278}, {x + 0.2f, y + 14.2f, 9502}, col(200, 200, 200)); // roof railings
        static const Color POND[] = {col(170, 96, 84, M_WATER), col(118, 140, 96, M_WATER), col(196, 160, 120, M_WATER), col(92, 132, 150, M_WATER)};
        int k = 0;
        for (float z = 8780; z < 10400; z += 260)
            for (float x = 3630; x < 3900; x += 140, k++) {
                float h = terrain(x + 60, z + 120);
                if (h < 0.4f) continue;
                mb(x, z).quad({x, h + 0.15f, z}, {x + 130, h + 0.15f, z}, {x + 130, h + 0.15f, z + 250}, {x, h + 0.15f, z + 250}, POND[k % 4]);
                ponds.push_back({x, z, x + 130, z + 250});
            }
    }

    // Stanford: Hoover Tower and the red-roofed Main Quad, south of town (scenery)
    {
        float tx = 2150, tz = 10950, y = terrain(tx, tz);
        MeshBuilder& m = mbKey(900031);
        Color sand = col(216, 192, 152, M_HOUSE), tile = col(186, 82, 60);
        m.box({tx - 7, y - 1, tz - 7}, {tx + 7, y + 66, tz + 7}, sand, sand, 0.85f);
        m.box({tx - 6.4f, y + 66, tz - 6.4f}, {tx + 6.4f, y + 76, tz + 6.4f}, col(200, 178, 140), col(200, 178, 140), 1.f);
        m.box({tx - 6.5f, y + 68, tz - 3}, {tx + 6.5f, y + 74, tz + 3}, col(50, 44, 40));
        m.box({tx - 3, y + 68, tz - 6.5f}, {tx + 3, y + 74, tz + 6.5f}, col(50, 44, 40));
        m.sphere({tx, y + 76, tz}, 6.5f, 10, 14, tile, true);
        m.prism({tx, y + 82, tz}, 8, 1.4f, 0.2f, 4, tile, tile);
        for (int k = 0; k < 6; k++) {
            float qx = tx + 60 + k * 45, qz = tz + (k % 2 ? 40.f : -10.f), qy = terrain(qx, qz);
            m.box({qx - 20, qy - 1, qz - 12}, {qx + 20, qy + 9, qz + 12}, sand, sand, 0.85f);
            m.gable(qx - 21, qx + 21, qz - 13, qz + 13, qy + 9, 4, 0, tile, sand);
        }
    }

    // San Mateo, Belmont, San Carlos, Redwood City and Atherton along the line (scenery seen from the train)
    for (int k = 0; k < 5200; k++) {
        float z = r.range(5000, 8700), x = r.range(800, 3500);
        float h = terrain(x, z);
        if (h < 2 || h > 120) continue;
        float s;
        float rd = railDistance(x, z, &s);
        if (rd < 22) continue;
        if (r.chance(0.35f)) { addTree(x, z, r.chance(0.75f) ? 4 : 5, r.range(0.9f, 1.3f), r, false); continue; }
        bool commercial = rd < 160 && r.chance(0.5f);
        Color c = commercial ? col(206, 196, 180) : WALLS[r.irange(0, 7)];
        float w = commercial ? r.range(20, 45) : r.range(12, 18), d = commercial ? r.range(16, 30) : r.range(10, 14);
        float hh = commercial ? r.range(6, 16) : r.range(5, 7);
        MeshBuilder& m = mb(x, z);
        m.box({x, h - 2, z}, {x + w, h + hh, z + d}, withMat(c, commercial ? M_OFFICE : M_HOUSE), col(140, 140, 138), 0.85f);
        if (!commercial) m.hip(x - 0.5f, x + w + 0.5f, z - 0.5f, z + d + 0.5f, h + hh, 2.6f, ROOFS[r.irange(0, 4)]);
    }
    // oak savanna and big houses up in the Woodside hills (scenery)
    for (int k = 0; k < 2600; k++) {
        float x = r.range(-600, 1340), z = r.range(6500, 11800);
        float h = terrain(x, z);
        if (h < 12 || h > 420) continue;
        float s;
        if (railDistance(x, z, &s) < 40) continue;
        if (r.chance(0.12f) && h < 220) {
            Color c = WALLS[r.irange(0, 7)];
            float w = r.range(16, 26), d = r.range(12, 18);
            MeshBuilder& m = mb(x, z);
            m.box({x, h - 2, z}, {x + w, h + 6.5f, z + d}, c, c, 0.85f);
            m.hip(x - 0.6f, x + w + 0.6f, z - 0.6f, z + d + 0.6f, h + 6.5f, 3.2f, ROOFS[r.irange(0, 4)]);
        } else addTree(x, z, r.chance(0.8f) ? 4 : 5, r.range(1.0f, 1.5f), r, false);
    }
}
