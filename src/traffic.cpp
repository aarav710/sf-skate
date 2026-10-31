#include "traffic.h"
#include "instance.h"
#include "models.h"

static RNG gR(20240);
static const World* gTrainWorld = nullptr;

static void colorPed(Ped& p, RNG& r) {
    static const Color SKIN[] = {{240, 205, 175, 255}, {214, 170, 134, 255}, {170, 120, 85, 255}, {120, 80, 55, 255}, {90, 60, 40, 255}};
    static const Color HAIR[] = {{30, 25, 20, 255}, {80, 50, 30, 255}, {200, 170, 100, 255}, {120, 120, 120, 255}, {20, 20, 20, 255}};
    static const Color SHIRT[] = {{200, 50, 50, 255}, {40, 90, 170, 255}, {240, 240, 235, 255}, {30, 30, 35, 255}, {240, 200, 60, 255},
                                  {60, 140, 90, 255}, {150, 80, 160, 255}, {230, 120, 40, 255}, {120, 130, 140, 255}, {190, 60, 110, 255}};
    static const Color PANTS[] = {{40, 50, 80, 255}, {30, 30, 32, 255}, {120, 100, 80, 255}, {90, 90, 95, 255}, {60, 70, 110, 255}};
    p.skin = SKIN[r.irange(0, 4)];
    p.hair = HAIR[r.irange(0, 4)];
    p.shirt = SHIRT[r.irange(0, 9)];
    p.pants = PANTS[r.irange(0, 4)];
    p.scale = r.range(0.9f, 1.08f);
    p.speed = r.range(1.0f, 1.6f);
    p.phase = r.range(0, 6);
    static const Color EXTRA[] = {{40, 40, 44, 255}, {150, 40, 40, 255}, {50, 80, 140, 255}, {200, 170, 120, 255}, {90, 110, 60, 255},
                                  {230, 230, 225, 255}, {120, 70, 40, 255}, {220, 120, 40, 255}};
    static const uint8_t HAIRS[] = {HAIR_SHORT, HAIR_SHORT, HAIR_LONG, HAIR_LONG, HAIR_BUN, HAIR_BALD, HAIR_CAP, HAIR_CURLY};
    uint32_t h = r.next();
    p.hairStyle = HAIRS[h % 8];
    int c = (h >> 3) % 20;   // about half carry something; phones are everywhere, and the odd laptop open on the go
    p.carry = c < 8 ? CARRY_NONE : c < 9 ? CARRY_LAPTOP : c < 12 ? CARRY_BACKPACK : c < 14 ? CARRY_SHOULDER : c < 16 ? CARRY_SHOPPING : CARRY_PHONE;
    p.idle = (h >> 8) % 4;
    p.build = 0.9f + ((h >> 12) % 100) * 0.0022f;
    p.extra = EXTRA[(h >> 20) % 8];
}

static int pickCarType(RNG& r, bool sf) {
    float x = r.f();
    if (x < 0.5f) return CT_SEDAN;
    if (x < 0.72f) return CT_SUV;
    if (x < 0.8f) return CT_VAN;
    if (x < 0.9f) return CT_TAXI;
    return sf ? CT_BUS : CT_SEDAN;
}

// A rounded corner on a lane `off` metres right of a route that turns at `v` from direction din to dout.
// u runs 0..1 from R before the corner to R after it. Returns the position and writes the travel direction.
static Vector2 cornerPos(Vector2 v, Vector2 din, Vector2 dout, float off, float u, float R, Vector2* tan) {
    Vector2 rin = {-din.y, din.x}, rout = {-dout.y, dout.x};
    Vector2 p0 = Vector2Add(v, Vector2Add(Vector2Scale(rin, off), Vector2Scale(din, -R)));
    Vector2 p3 = Vector2Add(v, Vector2Add(Vector2Scale(rout, off), Vector2Scale(dout, R)));
    float dp = Vector2DotProduct(din, dout), cr = din.x * dout.y - din.y * dout.x;
    float a, b;
    if (dp < -0.95f) a = b = (R + fabsf(off)) / 0.75f;     // U-turn: a loop reaching `off` past the corner
    else if (fabsf(cr) < 1e-4f) a = b = R * 0.55f;          // straight on
    else {                                                  // ease toward where the two lane lines meet
        Vector2 w = Vector2Subtract(p3, p0);
        a = std::max(0.3f, (w.x * dout.y - w.y * dout.x) / cr) * 0.55f;
        b = std::max(0.3f, (din.x * w.y - din.y * w.x) / cr) * 0.55f;
    }
    Vector2 p1 = Vector2Add(p0, Vector2Scale(din, a)), p2 = Vector2Subtract(p3, Vector2Scale(dout, b));
    u = clampf(u, 0, 1);
    float m = 1 - u;
    Vector2 pos = Vector2Add(Vector2Add(Vector2Scale(p0, m * m * m), Vector2Scale(p1, 3 * m * m * u)),
                             Vector2Add(Vector2Scale(p2, 3 * m * u * u), Vector2Scale(p3, u * u * u)));
    Vector2 d = Vector2Add(Vector2Add(Vector2Scale(Vector2Subtract(p1, p0), 3 * m * m), Vector2Scale(Vector2Subtract(p2, p1), 6 * m * u)),
                           Vector2Scale(Vector2Subtract(p3, p2), 3 * u * u));
    if (tan) *tan = Vector2Length(d) > 1e-5f ? Vector2Normalize(d) : (u < 0.5f ? din : dout);
    return pos;
}

// A point on a lane `off` metres right of a path (facing +s), with its bends rounded off.
static Vector2 pathLane(const Path& P, float s, float off, Vector2* tan) {
    s = clampf(s, 0, P.len);
    for (size_t k = 1; k + 1 < P.p.size(); k++) {
        if (fabsf(s - P.cum[k]) > 40) continue;
        Vector2 din = Vector2Normalize(Vector2Subtract(P.p[k], P.p[k - 1]));
        Vector2 dout = Vector2Normalize(Vector2Subtract(P.p[k + 1], P.p[k]));
        float c = clampf(Vector2DotProduct(din, dout), -1, 1);
        if (c > 0.9995f) continue;
        float th = std::min(tanf(acosf(c) * 0.5f), 4.f);
        float R = std::min({0.45f * (P.cum[k] - P.cum[k - 1]), 0.45f * (P.cum[k + 1] - P.cum[k]), std::max(8.f, fabsf(off) * th + 4)});
        if (R > 0.5f && fabsf(s - P.cum[k]) < R) return cornerPos(P.p[k], din, dout, off, (s - P.cum[k] + R) / (2 * R), R, tan);
    }
    Vector2 d;
    Vector2 p = P.at(s, &d);
    if (tan) *tan = d;
    return Vector2Add(p, Vector2Scale({-d.y, d.x}, off));
}

// Pick which way a car leaves grid node (i, j), having arrived heading d. Prefers going straight; U-turns only at dead ends.
static int pickGridDir(const Grid& g, int i, int j, int d, RNG& r) {
    int rev = d ^ 1;
    int opts[4], wts[4], n = 0, tot = 0;
    for (int k = 0; k < 4; k++) {
        if (k == rev || !g.edge(i, j, k)) continue;
        opts[n] = k; wts[n] = k == d ? 4 : 1; tot += wts[n]; n++;
    }
    if (n == 0) return rev;
    int x = r.irange(0, tot - 1);
    for (int k = 0; k < n; k++) { if (x < wts[k]) return opts[k]; x -= wts[k]; }
    return opts[0];
}

static Color carBody(int type, RNG& r) {
    if (type == CT_TAXI) return col(242, 196, 30);
    return randomCarColor(r);
}

void Traffic::init(World& w) {
    gTrainWorld = &w;
    RNG& r = gR;
    auto spawnGrid = [&](const Grid* g, int n, bool sf) {
        for (int k = 0; k < n * 20 && n > 0; k++) {
            int i = r.irange(0, g->ni - 1), j = r.irange(0, g->nj - 1), d = r.irange(0, 3);
            if (!g->edge(i, j, d)) continue;
            Car c;
            c.mode = 0; c.g = g; c.i = i; c.j = j; c.d = d; c.t = r.f();
            c.type = pickCarType(r, sf);
            c.body = carBody(c.type, r);
            c.maxSpeed = c.type == CT_BUS ? r.range(7, 9) : r.range(9, 13);
            c.speed = c.maxSpeed;
            c.lane = 2.4f;
            cars.push_back(c);
            n--;
        }
    };
    spawnGrid(&w.sf, 1300, true);
    spawnGrid(&w.oak, 170, false);
    spawnGrid(&w.mp, 190, false);
    for (int pi = 0; pi < (int)w.carPaths.size(); pi++) {
        const Path& P = w.carPaths[pi];
        int lanes = w.carPathLanes[pi];
        bool market = w.carPathKind[pi] == 1;
        int n = (int)(P.len / (market ? 24.f : 18.f));
        for (int k = 0; k < n; k++) {
            Car c;
            c.mode = 1; c.path = pi; c.s = r.range(std::min(60.f, P.len * 0.2f), std::max(P.len - 60, P.len * 0.8f)); c.pdir = r.chance(0.5f) ? 1 : -1;
            int ln = r.irange(0, lanes - 1);
            if (market) {
                c.lane = ln == 0 ? 3.0f : 7.0f;
                c.type = ln == 0 && r.chance(0.3f) ? CT_STREETCAR : pickCarType(r, true);
            } else {
                c.lane = (P.p[0].x < -1000 ? 2.0f : 2.5f) + ln * 3.5f;
                if (w.carPathKind[pi] == 2) c.lane = 2.2f + ln * 3.6f;
                c.type = pickCarType(r, false);
                if (c.type == CT_BUS && r.chance(0.6f)) c.type = CT_SUV;
            }
            if (c.type == CT_STREETCAR) {
                static const Color FL[] = {col(230, 120, 40), col(40, 110, 80), col(200, 40, 50), col(240, 200, 60), col(80, 120, 170)};
                c.body = FL[r.irange(0, 4)];
                c.lane = 3.0f;
            } else c.body = carBody(c.type, r);
            c.maxSpeed = market ? r.range(8, 11) : r.range(17, 23);
            if (c.type == CT_STREETCAR) c.maxSpeed = 7;
            c.speed = c.maxSpeed;
            cars.push_back(c);
        }
    }
    for (int pi = 0; pi < (int)w.cablePaths.size(); pi++)
        for (int k = 0; k < 3; k++) {
            Car c;
            c.mode = 2; c.path = pi; c.s = w.cablePaths[pi].len * (k + 0.5f) / 3; c.pdir = k % 2 ? 1 : -1;
            c.type = CT_CABLECAR; c.body = col(120, 30, 35); c.lane = 0; c.maxSpeed = 4.5f; c.speed = 4.5f;
            cars.push_back(c);
        }
    for (int pi = 0; pi < (int)w.tramPaths.size(); pi++) {
        int n = std::max(2, (int)(w.tramPaths[pi].len / 450.f));
        for (int k = 0; k < n; k++) {
            Car c;
            c.mode = 3; c.path = pi; c.s = w.tramPaths[pi].len * (k + 0.5f) / n; c.pdir = k % 2 ? 1 : -1;
            c.type = CT_LRV; c.body = col(205, 207, 212); c.lane = 2.4f; c.maxSpeed = r.range(9, 11); c.speed = c.maxSpeed;
            c.honkCD = 0;
            cars.push_back(c);
        }
    }

    // pedal buses: slow party bikes on the flatter streets of the north side (the Wharf, North Beach, the Marina)
    {
        static const Color PARTY[] = {col(220, 50, 60), col(30, 160, 160), col(130, 60, 190), col(240, 140, 30), col(120, 190, 50), col(230, 80, 160)};
        int n = 34;
        for (int k = 0; k < 4000 && n > 0; k++) {
            const Grid* g = &w.sf;
            int i = r.irange(0, g->ni - 1), j = r.irange(0, g->nj - 1), d = r.irange(0, 3);
            if (!g->edge(i, j, d)) continue;
            Vector2 a = g->node(i, j), b = g->node(i + DI[d], j + DJ[d]);
            if (a.y > gz(9) || fabsf(w.terrain(a.x, a.y) - w.terrain(b.x, b.y)) > 5.f) continue;
            Car c;
            c.mode = 0; c.g = g; c.i = i; c.j = j; c.d = d; c.t = r.f();
            c.type = CT_PEDALBUS;
            c.body = PARTY[r.irange(0, 5)];
            c.maxSpeed = r.range(3.0f, 4.2f);
            c.speed = c.maxSpeed;
            c.lane = 2.4f;
            c.pedal = r.range(0, TAU_F);
            cars.push_back(c);
            n--;
        }
    }

    // pedestrians
    auto spawnPeds = [&](const Grid* g, int n) {
        for (int k = 0; k < n * 20 && n > 0; k++) {
            int i = r.irange(0, g->ni - 1), j = r.irange(0, g->nj - 1);
            if (!g->nodeOk(i, j)) continue;
            bool any = false;
            for (int d = 0; d < 4; d++) any |= g->edge(i, j, d);
            if (!any) continue;
            Ped p;
            p.mode = 0; p.g = g; p.i = i; p.j = j;
            p.qx = r.chance(0.5f) ? 1 : -1; p.qz = r.chance(0.5f) ? 1 : -1;
            colorPed(p, r);
            placeGridPed(p, r);
            peds.push_back(p);
            n--;
        }
    };
    spawnPeds(&w.sf, 4000);
    spawnPeds(&w.oak, 420);
    spawnPeds(&w.mp, 520);
    // Caltrain: three trains working the line
    if (!w.stations.empty()) {
        trains.push_back({w.stations[0].s, 1, 0, 8});
        trains.push_back({w.stations.back().s, -1, 0, 20});
        trains.push_back({w.stations[2].s, 1, 0, 2});
    }
    for (int pi = 0; pi < (int)w.pedPaths.size(); pi++) {
        const Path& P = w.pedPaths[pi];
        int n = (int)(P.len / 11.f);
        for (int k = 0; k < n; k++) {
            Ped p;
            p.mode = 1; p.path = pi; p.s = r.range(0, P.len); p.pdir = r.chance(0.5f) ? 1 : -1; p.lane = r.range(-1.0f, 1.0f);
            colorPed(p, r);
            peds.push_back(p);
        }
    }
    for (auto& t : w.tourists) {
        Ped p;
        p.mode = 2; p.pos = t; p.from = {t.x, t.z}; p.yaw = r.range(-PI, PI);
        colorPed(p, r);
        peds.push_back(p);
    }
    // people sitting on benches and park lawns, more than half of them working on a laptop
    {
        RNG sr(777);   // its own stream, so nothing else in the city moves
        for (auto& st : w.seats) {
            if (sr.f() >= (st.grass ? 1.f : 0.45f)) continue;
            Ped p;
            p.mode = 3; p.grass = st.grass;
            p.pos = st.pos; p.from = {st.pos.x, st.pos.z}; p.yaw = st.yaw;
            colorPed(p, sr);
            float c = sr.f();
            p.carry = c < 0.6f ? CARRY_LAPTOP : c < 0.85f ? CARRY_PHONE : (p.carry == CARRY_PHONE ? CARRY_NONE : p.carry);
            peds.push_back(p);
        }
    }

    // dogs: about one walker in ten has one on a leash (seeded per person, so nothing else in the city moves)
    static const Color COAT[] = {{214, 170, 90, 255}, {120, 80, 50, 255}, {30, 30, 32, 255}, {236, 232, 222, 255}, {150, 150, 155, 255},
                                 {190, 120, 60, 255}, {90, 60, 40, 255}, {230, 200, 150, 255}};
    for (size_t i = 0; i < peds.size(); i++) {
        Ped& p = peds[i];
        uint32_t h = hashu((int)i, 13, 3);
        float chance = p.mode == 0 ? 0.11f : p.mode == 1 ? 0.07f : p.mode == 2 ? 0.05f : 0.f;
        if ((h % 1000) / 1000.f >= chance) continue;
        p.pet = 1;
        p.petC = COAT[(h >> 10) % 8];
        p.petC2 = (h >> 14) & 1 ? scaleC(p.petC, 0.6f) : Color{40, 34, 30, 255};
        p.petS = 0.6f + 0.6f * (((h >> 16) % 100) / 100.f);
        p.speed = std::min(p.speed, 1.3f);   // strolling with the dog
    }
    // cats: on the front steps of a few houses
    static const Color FUR[] = {{30, 30, 32, 255}, {214, 130, 50, 255}, {130, 130, 135, 255}, {235, 232, 225, 255}, {120, 90, 60, 255}, {235, 232, 225, 255}};
    for (size_t i = 0; i < w.houses.size(); i++) {
        uint32_t h = hashu((int)i, 77, 9);
        if (h % 100 >= 3) continue;
        const House& hs = w.houses[i];
        float x = hs.doorOut.x + ((int)((h >> 8) % 100) - 50) * 0.01f, z = hs.doorOut.y + ((int)((h >> 15) % 100) - 50) * 0.01f;
        Cat c;
        c.pos = {x, w.ground(x, z, w.terrain(x, z) + 3.f), z};
        c.yaw = ((h >> 20) % 628) * 0.01f;
        c.pose = (h >> 4) & 1;
        int k = (h >> 24) % 6;
        c.c = FUR[k];
        c.patch = k == 5 ? Color{214, 130, 50, 255} : (k == 4 ? Color{70, 50, 34, 255} : c.c);   // calico and tabby markings
        c.t = (h % 97) * 0.1f;
        cats.push_back(c);
    }
}

void Traffic::placeGridPed(Ped& p, RNG& r) {
    Vector2 n = p.g->node(p.i, p.j);
    p.from = p.to = {n.x + p.qx * p.g->walkOff, n.y + p.qz * p.g->walkOff};
    p.t = 1;
    nextPedMove(p, r);
}

void Traffic::nextPedMove(Ped& p, RNG& r) {
    const Grid& g = *p.g;
    struct Opt { int ni, nj, qx, qz; float w; };
    Opt opts[4];
    int n = 0;
    int dx = p.qx > 0 ? 0 : 1, dz = p.qz > 0 ? 2 : 3;
    if (g.edge(p.i, p.j, dx)) opts[n++] = {p.i + p.qx, p.j, -p.qx, p.qz, 3};
    if (g.edge(p.i, p.j, dz)) opts[n++] = {p.i, p.j + p.qz, p.qx, -p.qz, 3};
    if (g.edge(p.i, p.j, 2) || g.edge(p.i, p.j, 3)) opts[n++] = {p.i, p.j, -p.qx, p.qz, 0.8f};
    if (g.edge(p.i, p.j, 0) || g.edge(p.i, p.j, 1)) opts[n++] = {p.i, p.j, p.qx, -p.qz, 0.8f};
    if (n == 0) { p.mode = 2; return; }
    float tot = 0;
    for (int k = 0; k < n; k++) tot += opts[k].w;
    float x = r.f() * tot;
    int pick = 0;
    for (int k = 0; k < n; k++) { if (x < opts[k].w) { pick = k; break; } x -= opts[k].w; }
    Opt o = opts[pick];
    p.from = p.to;
    p.i = o.ni; p.j = o.nj; p.qx = o.qx; p.qz = o.qz;
    Vector2 nd = g.node(p.i, p.j);
    p.to = {nd.x + p.qx * g.walkOff, nd.y + p.qz * g.walkOff};
    p.len = std::max(0.5f, Vector2Distance(p.from, p.to));
    p.t = 0;
}

void Traffic::startEvade(Ped& p, int kind, Vector2 dir, float dist) {
    p.evade = (uint8_t)kind;
    p.evT = 0;
    p.evFrom = p.dodge;
    p.evTo = Vector2Add(p.dodge, Vector2Scale(dir, dist));
    p.wait = 0;
    if (kind == EV_STEP)      { p.evMove = std::max(0.5f, dist / 2.6f); p.evDur = p.evMove + 0.35f; }   // brisk walk, then settle
    else if (kind == EV_BACK) { p.evMove = std::max(0.6f, dist / 2.0f); p.evDur = p.evMove + 0.5f; }
    else if (kind == EV_HOP)  { p.evMove = 0.5f; p.evDur = 0.95f; }                                     // hop, land in a crouch
    else                      { p.evMove = 0.65f; p.evDur = 0.65f + 1.4f + 0.95f; }                     // leap, lie there, get up
}

// The cheap update for someone far from the skater and the camera: walk the route with no dodging and no
// wall checks (the full update skips those past 250 m anyway), and one ground lookup for the height, so
// they're on their route at the right height whenever they come back into view.
void Traffic::farPedUpdate(Ped& p, float dt, World& w, RNG& r) {
    if (p.knock > 0) { p.knock -= dt; return; }
    if (p.mode == 3) return;
    float prevGround = p.pos.y - p.evY;
    if (p.evade != EV_NONE) { p.dodge = p.evTo; p.evade = EV_NONE; }   // nobody's dodging out here: finish the move
    p.evY = 0; p.evTilt = 0;
    float L = Vector2Length(p.dodge);   // and they drift back onto their route at a walk
    if (L > 0.01f) p.dodge = Vector2Scale(p.dodge, (L - std::min(L, 1.0f * dt)) / L);
    Vector2 base, routeDir = {0, 0};
    bool walking = false;
    if (p.mode == 0) {
        if (p.wait > 0) p.wait -= dt;
        else { p.t += p.speed * dt / p.len; walking = true; }
        if (p.t >= 1) { p.t = 1; nextPedMove(p, r); if (r.chance(0.22f)) p.wait = r.range(1.5f, 5.f); }
        base = Vector2Lerp(p.from, p.to, clampf(p.t, 0, 1));
        if (Vector2Length(Vector2Subtract(p.to, p.from)) > 0.1f) routeDir = Vector2Normalize(Vector2Subtract(p.to, p.from));
    } else if (p.mode == 1) {
        const Path& P = w.pedPaths[p.path];
        p.s += p.pdir * p.speed * dt;
        if (p.s > P.len) { p.s = P.len; p.pdir = -1; }
        if (p.s < 0) { p.s = 0; p.pdir = 1; }
        Vector2 dir;
        base = P.at(p.s, &dir);
        dir = Vector2Scale(dir, (float)p.pdir);
        base = Vector2Add(base, Vector2Scale({-dir.y, dir.x}, p.lane));
        routeDir = dir;
        walking = true;
    } else {
        base = p.from;
        p.idleTurn -= dt;
        if (p.idleTurn < 0) { p.idleTurn = r.range(2, 7); p.yaw += r.range(-1.2f, 1.2f); }
    }
    float x = base.x + p.dodge.x, z = base.y + p.dodge.y;
    float y = w.ground(x, z, std::max(w.terrain(x, z), prevGround));   // same step-climbing reference as the full update
    p.pos = {x, y, z};
    p.gait = walking && p.mode != 2 ? p.speed : 0;
    if (walking && (routeDir.x != 0 || routeDir.y != 0)) p.yaw = atan2f(routeDir.y, routeDir.x);
    p.phase += dt * p.gait * 5.2f;
}

void Traffic::update(float dt, World& w, const Player& pl) {
    RNG& r = gR;
    evHonk = false;
    clock += dt;
    for (auto& c : cats) {
        float dx = c.pos.x - pl.pos.x, dz = c.pos.z - pl.pos.z, d2 = dx * dx + dz * dz;
        if (d2 > 80 * 80) continue;
        c.t += dt;
        if (c.flee > 0) {   // bolting away, around anything in the way
            c.flee -= dt;
            float nx = c.pos.x + cosf(c.yaw) * 6.5f * dt, nz = c.pos.z + sinf(c.yaw) * 6.5f * dt;
            float ny = w.ground(nx, nz, c.pos.y + 0.6f);
            if (w.solidAt({nx, ny + 0.25f, nz}) || fabsf(ny - c.pos.y) > 0.6f || w.isWater(nx, nz, ny)) c.yaw += 1.6f;
            else c.pos = {nx, ny, nz};
            if (c.flee <= 0) { c.pose = 0; c.yaw += PI; }   // safe: sit down and look back
        } else if (d2 < 25 && pl.hspeed() > 3.f && fabsf(c.pos.y - pl.pos.y) < 2) {
            c.flee = 1.4f; c.pose = 2;
            c.yaw = atan2f(dz, dx) + ((int)(c.t * 10) % 2 ? 0.5f : -0.5f);
        }
    }
    Vector3 pp = pl.pos;
    for (auto& c : cars) {
        float distP = sqrtf(sq(c.pos.x - pp.x) + sq(c.pos.z - pp.z));
        // target speed: stop for the skater and for cars ahead
        float target = c.maxSpeed;
        if (c.stopT > 0) { c.stopT -= dt; target = 0; }
        Vector2 f = {cosf(c.yaw), sinf(c.yaw)};
        if (c.placed && distP < 40) {
            float rx = pp.x - c.pos.x, rz = pp.z - c.pos.z;
            float along = rx * f.x + rz * f.y, lat = fabsf(-rx * f.y + rz * f.x);
            float look = 6 + c.speed * 0.9f + carModel(c.type).halfLen;
            if (along > 0 && along < look && lat < 2.3f && fabsf(pp.y - c.pos.y) < 3) {
                target = 0;
                if (c.honkCD <= 0 && c.speed > 3 && along < 14) { evHonk = true; c.honkCD = 4; }
            }
        }
        c.honkCD -= dt;
        if (c.placed && distP < 350) {
            for (auto& o : cars) {
                if (&o == &c) continue;
                float rx = o.pos.x - c.pos.x, rz = o.pos.z - c.pos.z;
                if (fabsf(rx) > 20 || fabsf(rz) > 20) continue;
                float along = rx * f.x + rz * f.y, lat = fabsf(-rx * f.y + rz * f.x);
                float gap = carModel(c.type).halfLen + carModel(o.type).halfLen + 2.5f;
                if (along > 0 && along < gap + c.speed * 0.6f && lat < 1.6f && fabsf(o.pos.y - c.pos.y) < 3) {
                    float of = cosf(o.yaw) * f.x + sinf(o.yaw) * f.y;
                    if (of > 0.3f) target = std::min(target, along < gap ? 0.f : o.speed);
                }
            }
        }
        // slow down for corners and U-turns, braking gently on the way in
        {
            float cap = 1e9f, dist = 0;
            if (c.mode == 0 && c.nd >= 0) {
                float u = c.t * c.g->sp;
                if (c.pd >= 0 && c.pd != c.d && u < 10) cap = c.pd == (c.d ^ 1) ? 3.5f : 6.f;      // still finishing a turn
                if (c.nd != c.d) { float k = c.nd == (c.d ^ 1) ? 3.5f : 6.f; dist = std::max(0.f, c.g->sp - 10 - u); cap = std::min(cap, sqrtf(k * k + 8 * dist)); }
            } else if (c.mode == 1 && c.type != CT_STREETCAR) {
                if (c.uturn >= 0) cap = 4;
                else { const Path& P = w.carPaths[c.path]; dist = c.pdir > 0 ? P.len - c.s : c.s; cap = sqrtf(16 + 8 * dist); }
            }
            target = std::min(target, cap);
        }
        // traffic lights: stop at the line on red, and on yellow unless too close to stop
        if (c.mode == 0 && c.g == &w.sf && !w.signals.empty()) {
            int ni = c.i + DI[c.d], nj = c.j + DJ[c.d];
            if (c.g->in(ni, nj) && w.signals[(size_t)nj * c.g->ni + ni]) {
                int st = signalState(nj * c.g->ni + ni, c.d < 2 ? 0 : 1, clock);
                float dist = c.g->sp - 11.5f - c.t * c.g->sp - carModel(c.type).halfLen;
                if (st != 0 && dist > -0.3f && !(st == 1 && dist < c.speed * 1.1f))
                    target = std::min(target, dist > 0.4f ? sqrtf(2 * 4.5f * dist) - 0.6f : 0.f);
            }
        }
        float dv = target - c.speed;
        c.speed += clampf(dv, -14 * dt, 3.5f * dt);
        c.speed = std::max(c.speed, 0.f);
        if (c.type == CT_PEDALBUS) c.pedal += c.speed * dt * 1.7f;
        c.braking = dv < -0.5f;

        Vector2 tp, tan = {cosf(c.yaw), sinf(c.yaw)};
        bool deck = false;
        auto dirOf = [](int d) { return Vector2{(float)DI[d], (float)DJ[d]}; };
        if (c.mode == 0) {
            const Grid& g = *c.g;
            if (c.pd < 0) c.pd = c.d;
            if (c.nd < 0) c.nd = pickGridDir(g, c.i + DI[c.d], c.j + DJ[c.d], c.d, r);
            c.t += c.speed * dt / g.sp;
            int guard = 0;
            while (c.t >= 1 && guard++ < 4) {
                c.t -= 1;
                c.i += DI[c.d]; c.j += DJ[c.d];
                c.pd = c.d; c.d = c.nd;
                c.nd = pickGridDir(g, c.i + DI[c.d], c.j + DJ[c.d], c.d, r);
            }
            // the turn is decided a block ahead, so each intersection is driven as one smooth curve
            Vector2 a = g.node(c.i, c.j), b = g.node(c.i + DI[c.d], c.j + DJ[c.d]);
            float u = c.t * g.sp, R = std::min(10.f, g.sp * 0.3f);
            if (u > g.sp - R) tp = cornerPos(b, dirOf(c.d), dirOf(c.nd), c.lane, (u - g.sp + R) / (2 * R), R, &tan);
            else if (u < R) tp = cornerPos(a, dirOf(c.pd), dirOf(c.d), c.lane, (u + R) / (2 * R), R, &tan);
            else {
                Vector2 right = {(float)-DJ[c.d], (float)DI[c.d]};
                tp = Vector2Add(Vector2Lerp(a, b, c.t), Vector2Scale(right, c.lane));
                tan = dirOf(c.d);
            }
        } else {
            const Path& P = c.mode == 1 ? w.carPaths[c.path] : (c.mode == 3 ? w.tramPaths[c.path] : w.cablePaths[c.path]);
            deck = P.onDeck;
            // trams and cable cars have a cab at each end, so they just change ends; road vehicles U-turn
            bool rail = c.mode >= 2 || c.type == CT_STREETCAR;
            float step = c.speed * dt, Lu = PI * std::max(c.lane, 1.f);
            if (c.uturn >= 0) {
                c.uturn += step;
                if (c.uturn >= Lu) { c.uturn = -1; c.pdir = -c.pdir; }
            } else {
                float before = c.s;
                c.s += c.pdir * step;
                c.xover += step;
                if (c.s > P.len || c.s < 0) {
                    c.s = clampf(c.s, 0, P.len);
                    if (rail) { c.pdir = -c.pdir; c.yaw = wrapPi(c.yaw + PI); c.xover = 0; }
                    else c.uturn = 0;
                }
                // streetcars and cable cars pause at stops along the line
                if (c.mode >= 2 && floorf(before / 220.f) != floorf(c.s / 220.f) && c.stopT <= 0) c.stopT = 6;
                if (c.mode >= 2 && (c.s == 0 || c.s == P.len) && c.stopT <= 0 && c.speed > 2) c.stopT = 8;
            }
            if (c.uturn >= 0) {
                bool atEnd = c.s > P.len * 0.5f;
                size_t n = P.p.size();
                Vector2 e = atEnd ? P.p[n - 1] : P.p[0];
                Vector2 din = atEnd ? Vector2Normalize(Vector2Subtract(P.p[n - 1], P.p[n - 2])) : Vector2Normalize(Vector2Subtract(P.p[0], P.p[1]));
                tp = cornerPos(e, din, Vector2Negate(din), c.lane, c.uturn / Lu, 0, &tan);
            } else {
                // after reversing, a tram eases across to the other track over its first 45 m
                auto lanePt = [&](float s, float xo) {
                    float f = rail ? -1 + 2 * smooth01(0, 45, xo) : 1.f;
                    return pathLane(P, s, c.pdir * c.lane * f, nullptr);
                };
                tp = lanePt(c.s, c.xover);
                float s2 = c.s + c.pdir * 0.5f;
                Vector2 dd = s2 >= 0 && s2 <= P.len ? Vector2Subtract(lanePt(s2, c.xover + 0.5f), tp)
                                                    : Vector2Subtract(tp, lanePt(c.s - c.pdir * 0.5f, c.xover - 0.5f));
                if (Vector2Length(dd) > 1e-4f) tan = Vector2Normalize(dd);
            }
        }
        Vector2 cur = {c.pos.x, c.pos.z};
        if (!c.placed) cur = tp;
        Vector2 np = Vector2Lerp(cur, tp, c.placed ? 1 - expf(-dt * 7) : 1.f);
        // face along the route itself, never toward wherever the target point happened to jump
        c.yaw = wrapPi(lerpAngle(c.yaw, atan2f(tan.y, tan.x), c.placed ? 1 - expf(-dt * 10) : 1.f));
        auto H = [&](float x, float z) { return deck ? w.ground(x, z, 1e5f) : w.terrain(x, z) + 0.08f; };
        float y = H(np.x, np.y);
        float hl = carModel(c.type).halfLen * 0.8f;
        float yf = H(np.x + cosf(c.yaw) * hl, np.y + sinf(c.yaw) * hl), yb = H(np.x - cosf(c.yaw) * hl, np.y - sinf(c.yaw) * hl);
        c.pitch = atan2f(yf - yb, 2 * hl);
        c.pos = {np.x, (yf + yb) * 0.5f * 0.5f + y * 0.5f, np.y};
        c.placed = true;
    }

    // the skater's velocity on the ground, for the people deciding whether to get out of the way
    Vector2 skV = pl.state == Player::AIR ? Vector2{pl.vel.x, pl.vel.z} : Vector2{cosf(pl.yaw) * pl.speed, sinf(pl.yaw) * pl.speed};
    float skSp = Vector2Length(skV);
    Vector2 skDir = skSp > 0.1f ? Vector2Scale(skV, 1.f / skSp) : Vector2{1, 0};
    // Only people within PED_NEAR of the skater or the camera get the full update below (dodging, walls,
    // climbing steps). Nobody is drawn past 230 m, so the rest just walk their routes: each frame one slice
    // of them catches up on the time they've missed, and anyone coming into range catches up first.
    const float PED_NEAR = 300.f;
    const int FAR_SLICES = 16;
    farTurn = (farTurn + 1) % FAR_SLICES;
    for (size_t pi = 0; pi < peds.size(); pi++) {
        Ped& p = peds[pi];
        float dP2 = sq(p.pos.x - pp.x) + sq(p.pos.z - pp.z);
        if (std::min(dP2, sq(p.pos.x - view.x) + sq(p.pos.z - view.z)) > PED_NEAR * PED_NEAR) {
            p.lag += dt;
            if ((int)(pi % FAR_SLICES) == farTurn) { farPedUpdate(p, p.lag, w, r); p.lag = 0; }
            continue;
        }
        if (p.lag > 0) { farPedUpdate(p, p.lag, w, r); p.lag = 0; }
        if (p.knock > 0) { p.knock -= dt; continue; }
        if (p.mode == 3) continue;   // sitting: stays put, on the seat
        Vector3 prev = p.pos;
        float prevGround = p.pos.y - p.evY;

        // ---- getting out of the skater's way
        // how soon they notice: someone heads-down in a phone or laptop spots the skater late, others at varying distances
        uint32_t ph = hashu((int)(p.scale * 1e4f), (int)(p.build * 1e4f), 9);
        float u01 = (ph % 1000) / 1000.f;
        bool distracted = p.carry == CARRY_PHONE || p.carry == CARRY_LAPTOP;
        float notice = distracted ? lerpf(3.5f, 7.f, u01) : lerpf(6.f, 14.f, u01);
        if (p.evade == EV_NONE && dP2 < notice * notice && skSp > 3.f && pl.state != Player::BAIL) {
            Vector2 rel = {p.pos.x - pp.x, p.pos.z - pp.z};
            Vector2 perp = {-skDir.y, skDir.x};
            float along = Vector2DotProduct(rel, skDir), lat = Vector2DotProduct(rel, perp);
            float tti = along / skSp;   // seconds until the skater is level with them
            if (along > -0.3f && fabsf(lat) < 1.5f && tti < 1.5f && fabsf(pl.pos.y - p.pos.y) < 2.f) {
                uint32_t h = hashu((int)(p.phase * 1000.f), (int)(along * 10.f), 41);
                // away from the skater's line, to the side they're already on (a coin toss when dead ahead)
                float side = fabsf(lat) > 0.2f ? (lat > 0 ? 1.f : -1.f) : ((h & 1) ? 1.f : -1.f);
                float clear = 1.5f - fabsf(lat) + 0.4f;   // how far to go to be clear of the board and the body
                auto blocked = [&](Vector2 off) {
                    Vector2 to = Vector2Add(Vector2Add({p.pos.x, p.pos.z}, Vector2Subtract(off, p.dodge)), Vector2Scale(Vector2Normalize(off), 0.4f));
                    float g = w.terrain(to.x, to.y);
                    return w.solidAt({to.x, std::max(prevGround, g) + 0.6f, to.y}) || w.isWater(to.x, to.y, g);
                };
                // pick the move: time to spare means a brisk walk out of the way; the closer and faster the
                // skater, the more desperate it gets: a hop aside, or a dive and a sprawl on the ground
                float r01 = ((h >> 4) % 1000) / 1000.f;
                int kind;
                if (tti > 0.85f || skSp < 6.f) {
                    float faceSk = cosf(p.yaw) * -skDir.x + sinf(p.yaw) * -skDir.y;   // already looking at the skater?
                    kind = faceSk > 0.4f ? EV_BACK : EV_STEP;
                } else if (tti < 0.55f && skSp > 9.f && r01 < 0.55f) kind = EV_DIVE;
                else kind = EV_HOP;
                float dist = kind == EV_DIVE ? clear + 1.1f : kind == EV_HOP ? clear + 0.35f : clear;
                Vector2 dir = Vector2Scale(perp, side);
                if (kind == EV_BACK) dir = Vector2Normalize(Vector2Add(Vector2Scale(perp, side), Vector2Scale(skDir, 0.45f)));
                Vector2 off = Vector2Add(p.dodge, Vector2Scale(dir, dist));
                if (blocked(off)) {   // a wall or the water that way: go the other way instead
                    side = -side;
                    dist += 2.f * fabsf(lat);
                    dir = kind == EV_BACK ? Vector2Normalize(Vector2Add(Vector2Scale(perp, side), Vector2Scale(skDir, 0.45f))) : Vector2Scale(perp, side);
                    off = Vector2Add(p.dodge, Vector2Scale(dir, dist));
                    if (kind == EV_DIVE && blocked(off)) kind = EV_HOP;   // nowhere to land a dive
                }
                startEvade(p, kind, dir, Vector2Length(Vector2Subtract(off, p.dodge)));
            }
        }
        bool evading = p.evade != EV_NONE;
        p.evY = 0;
        if (evading) {
            p.evT += dt;
            float k = clampf(p.evT / p.evMove, 0, 1);
            float e = p.evade == EV_STEP || p.evade == EV_BACK ? k * k * (3 - 2 * k)       // ease in and out
                                                               : k * (1.5f - 0.5f * k);    // launch quick, ease into the landing
            p.dodge = Vector2Lerp(p.evFrom, p.evTo, e);
            if (p.evade == EV_HOP) p.evY = 4.f * 0.38f * k * (1 - k);
            if (p.evade == EV_DIVE) {
                p.evY = 4.f * 0.45f * k * (1 - k);
                float up = p.evDur - 0.95f;   // when they start getting up
                p.evTilt = p.evT < p.evMove ? smooth01(0.f, 1.f, k) : 1.f - smooth01(up, p.evDur - 0.1f, p.evT);
            } else p.evTilt = 0;
            if (p.evT >= p.evDur) { p.evade = EV_NONE; p.evTilt = 0; }
        } else if (Vector2Length(p.dodge) > 0.01f) {
            // back to their route at a walk once the skater has gone by (they turn and walk it, see the facing below)
            // (a skater stopped right beside them is still in the way: they keep their step round until past)
            bool clear = dP2 > 6 * 6 || (skSp < 1.f && dP2 > 2.5f * 2.5f);
            if (clear) {
                float L = Vector2Length(p.dodge), step = std::min(L, 1.0f * dt);
                p.dodge = Vector2Subtract(p.dodge, Vector2Scale(p.dodge, step / L));
            }
        }

        Vector2 base, routeDir = {0, 0};
        bool hold = evading;   // mid-dodge they stop following their route
        if (p.mode == 0) {
            if (p.wait > 0) p.wait -= dt;   // waiting at the corner to cross
            else if (!hold) p.t += p.speed * dt / p.len;
            if (p.t >= 1) { p.t = 1; nextPedMove(p, r); if (r.chance(0.22f)) p.wait = r.range(1.5f, 5.f); }
            base = Vector2Lerp(p.from, p.to, clampf(p.t, 0, 1));
            if (Vector2Length(Vector2Subtract(p.to, p.from)) > 0.1f) routeDir = Vector2Normalize(Vector2Subtract(p.to, p.from));
        } else if (p.mode == 1) {
            const Path& P = w.pedPaths[p.path];
            if (!hold) p.s += p.pdir * p.speed * dt;
            if (p.s > P.len) { p.s = P.len; p.pdir = -1; }
            if (p.s < 0) { p.s = 0; p.pdir = 1; }
            Vector2 dir;
            base = P.at(p.s, &dir);
            dir = Vector2Scale(dir, (float)p.pdir);
            base = Vector2Add(base, Vector2Scale({-dir.y, dir.x}, p.lane));
            routeDir = dir;
        } else {
            base = p.from;
            p.idleTurn -= dt;
            if (p.idleTurn < 0 && p.gait < 0.2f && !evading) { p.idleTurn = r.range(2, 7); p.yaw += r.range(-1.2f, 1.2f); }
        }
        // a skater standing or rolling slowly (too slow to set off the dodges above) is walked round like
        // anyone else on the sidewalk: drift sideways off the route while they're in the way ahead, to the
        // side already favoured, or the other side if a wall or the water is there
        bool slowSk = pl.hspeed() < 2.f && fabsf(pl.pos.y - p.pos.y) < 1.4f;
        if (slowSk && !evading && dP2 < 4.f * 4.f && (routeDir.x != 0 || routeDir.y != 0)) {
            const float R = 0.8f;   // how wide a berth they give the skater
            Vector2 at = {base.x + p.dodge.x - pp.x, base.y + p.dodge.y - pp.z};   // skater to where they're headed
            Vector2 side = {-routeDir.y, routeDir.x};
            float ahead = -Vector2DotProduct(at, routeDir), lat = Vector2DotProduct(at, side);
            if (ahead > -0.3f && ahead < 2.5f && fabsf(lat) < R) {
                float sg = fabsf(lat) > 0.05f ? (lat > 0 ? 1.f : -1.f) : ((hashu((int)(p.phase * 1000.f), (int)pi, 43) & 1) ? 1.f : -1.f);
                Vector2 to = {pp.x + side.x * sg * (R + 0.3f), pp.z + side.y * sg * (R + 0.3f)};
                float g = w.terrain(to.x, to.y);
                if (w.solidAt({to.x, std::max(prevGround, g) + 0.6f, to.y}) || w.isWater(to.x, to.y, g)) sg = -sg;
                float need = sg * lat > 0 ? R - fabsf(lat) : R + fabsf(lat);
                p.dodge = Vector2Add(p.dodge, Vector2Scale(side, sg * std::min(need, 1.4f * dt)));
            }
        }
        float x = base.x + p.dodge.x, z = base.y + p.dodge.y;
        // and never in the same spot as a slow skater (anyone faster knocks them down, in interact):
        // a soft push out, kept in the sidestep so it lasts and the walls below still win
        if (slowSk && p.evade != EV_DIVE) {
            const float BODY = 0.6f;
            float ox = x - pp.x, oz = z - pp.z, d = sqrtf(ox * ox + oz * oz);
            if (d < BODY) {
                Vector2 n = d > 0.01f ? Vector2{ox / d, oz / d} : (routeDir.x != 0 || routeDir.y != 0) ? Vector2{-routeDir.y, routeDir.x} : Vector2{1, 0};
                Vector2 push = Vector2Scale(n, BODY - d);
                p.dodge = Vector2Add(p.dodge, push);
                x += push.x; z += push.y;
            }
        }
        // climb steps one at a time from where they stood last frame, like the skater does,
        // instead of reading the ground from street level and walking through a stoop
        float yRef = std::max(w.terrain(x, z), prevGround);
        float y = w.ground(x, z, yRef);
        if (dP2 < 250 * 250) {
            // keep the body out of walls (a sidestep or a route clipping a corner would sink them into it);
            // the push also comes off the sidestep, so it can't keep shoving them back in
            Vector3 c = {x, y, z};
            if (w.collide(c, 0.32f, y, 1.7f, nullptr)) {
                // remember only the sideways part of the push, so they walk round a pole or a news box; a push back
                // along the route would leave them stuck behind it while the route ran on without them
                Vector2 push = {c.x - x, c.z - z}, keep = push;
                if (routeDir.x != 0 || routeDir.y != 0) {
                    float back = Vector2DotProduct(push, routeDir);
                    keep = Vector2Subtract(push, Vector2Scale(routeDir, back));
                    if (back < 0 && Vector2Length(keep) < -back * 0.5f) {   // walked square into it: pick a side to go round
                        Vector2 side = {-routeDir.y, routeDir.x};
                        float sg = Vector2DotProduct(keep, side) < 0 ? -1.f : 1.f;
                        keep = Vector2Add(keep, Vector2Scale(side, sg * -back * 0.5f));
                    }
                }
                p.dodge = Vector2Add(p.dodge, keep);
                float L = Vector2Length(p.dodge);
                if (L > 4.f) p.dodge = Vector2Scale(p.dodge, 4.f / L);
                if (evading) { p.evTo = Vector2Add(p.evTo, keep); p.evFrom = Vector2Add(p.evFrom, keep); }
                x = c.x; z = c.z;
                y = w.ground(x, z, std::max(w.terrain(x, z), yRef));
            }
        }
        // never faster than a person moves: a correction (rounding an obstacle the route runs through, say)
        // plays out as a quick step instead of a pop. A huge gap is a fresh placement (start of the game), taken as is.
        {
            float mx = x - prev.x, mz = z - prev.z, d = sqrtf(mx * mx + mz * mz);
            float vmax = (evading ? 9.f : 4.f) * dt;
            if (d > vmax && d < 30.f) {
                x = prev.x + mx * vmax / d; z = prev.z + mz * vmax / d;
                y = w.ground(x, z, std::max(w.terrain(x, z), yRef));
                Vector3 c = {x, y, z};   // a step this short from a clear spot only ever needs a nudge to stay clear
                if (dP2 < 250 * 250 && w.collide(c, 0.32f, y, 1.7f, nullptr)) { x = c.x; z = c.z; y = w.ground(x, z, std::max(w.terrain(x, z), yRef)); }
            }
        }
        p.pos = {x, y + p.evY, z};

        // legs and facing follow how they actually move: walking the route, stepping aside, walking back
        Vector2 v = {(p.pos.x - prev.x) / std::max(dt, 1e-4f), (p.pos.z - prev.z) / std::max(dt, 1e-4f)};
        float vs = Vector2Length(v);
        if (vs > 8.f) vs = 0, v = {0, 0};   // a jump to a new route corner, not a step
        p.gait = lerpf(p.gait, vs, 1 - expf(-dt * 10));
        if (p.evade == EV_BACK) {
            // backing off: keep facing the skater, the legs stepping backwards
            p.yaw = lerpAngle(p.yaw, atan2f(pp.z - p.pos.z, pp.x - p.pos.x), 1 - expf(-dt * 8));
            p.phase -= dt * vs * 5.2f;
        } else {
            if (vs > 0.3f && p.evTilt < 0.5f && !(p.evade == EV_DIVE && p.evT > p.evMove))
                p.yaw = lerpAngle(p.yaw, atan2f(v.y, v.x), 1 - expf(-dt * (evading ? 12.f : 6.f)));
            p.phase += dt * vs * 5.2f;
        }
    }
    // Caltrain: accelerate, brake for the next station, dwell, reverse at the ends
    float lineEnd = w.railLen() - 25, lineStart = w.stations.empty() ? 0 : w.stations[0].s;
    for (auto& t : trains) {
        if (t.dwell > 0) { t.dwell -= dt; continue; }
        float stop = t.dir > 0 ? lineEnd : lineStart;
        for (auto& st : w.stations)
            if ((st.s - t.s) * t.dir > 2 && fabsf(st.s - t.s) < fabsf(stop - t.s)) stop = st.s;
        float dist = (stop - t.s) * t.dir;
        float vt = std::min(38.f, sqrtf(2 * 1.1f * std::max(dist, 0.f)));
        t.v += clampf(vt - t.v, -1.6f * dt, 0.9f * dt);
        t.v = std::max(t.v, dist > 1 ? 1.5f : 0.f);
        t.s += t.dir * t.v * dt;
        if (dist < 0.6f) {
            t.s = stop; t.v = 0; t.dwell = 16;
            if (stop == lineEnd || stop == lineStart) { t.dir = -t.dir; t.dwell = 30; }
        }
    }
}

void Traffic::interact(Player& pl) {
    evCrash = false;
    if (pl.state == Player::BAIL) return;
    for (auto& t : trains) {
        for (int k = 0; k < 5; k++) {
            float sc = t.s - t.dir * (k * 25.8f + 12.9f);
            Vector2 d;
            Vector3 c = gTrainWorld->trackPoint(sc, 2.4f * t.dir, &d);
            float dx = pl.pos.x - c.x, dz = pl.pos.z - c.z;
            if (dx * dx + dz * dz > 400) continue;
            float lx = dx * d.x + dz * d.y, lz = -dx * d.y + dz * d.x;
            if (fabsf(lx) > 13.3f || fabsf(lz) > 1.95f || pl.pos.y > c.y + 5.2f) continue;
            if (t.v > 1) { pl.bail("Hit by Caltrain!"); evHonk = true; evCrash = true; return; }
            float push = 1.95f - fabsf(lz), sgn = lz > 0 ? 1.f : -1.f;
            pl.pos.x += -d.y * push * sgn; pl.pos.z += d.x * push * sgn;
            pl.speed *= 0.5f;
        }
    }
    for (auto& c : cars) {
        float dx = pl.pos.x - c.pos.x, dz = pl.pos.z - c.pos.z;
        if (dx * dx + dz * dz > 100) continue;
        const CarModel& m = carModel(c.type);
        float f0 = cosf(c.yaw), f1 = sinf(c.yaw);
        float lx = dx * f0 + dz * f1, lz = -dx * f1 + dz * f0;
        float px = m.halfLen + 0.35f - fabsf(lx), pz = m.halfWid + 0.35f - fabsf(lz);
        if (px <= 0 || pz <= 0) continue;
        if (pl.pos.y > c.pos.y + m.height - 0.2f || pl.pos.y < c.pos.y - 1.5f) continue;
        Vector2 pv = pl.state == Player::AIR ? Vector2{pl.vel.x, pl.vel.z} : Vector2{cosf(pl.yaw) * pl.speed, sinf(pl.yaw) * pl.speed};
        Vector2 cv = {f0 * c.speed, f1 * c.speed};
        float rel = Vector2Length(Vector2Subtract(pv, cv));
        if (rel > 4.5f || c.speed > 4) {
            pl.bail(c.type == CT_CABLECAR ? "Hit by a cable car!" : c.type == CT_BUS ? "Hit by a Muni bus!" : c.type == CT_PEDALBUS ? "Hit by a pedal bus!" : c.type == CT_LRV ? "Hit by a Muni streetcar!" : "Hit by a car!");
            c.stopT = 3;
            c.speed = 0;
            evHonk = true;
            evCrash = true;
            return;
        }
        // slow bump: push out
        if (px < pz) { float s = lx > 0 ? 1.f : -1.f; pl.pos.x += f0 * px * s; pl.pos.z += f1 * px * s; }
        else { float s = lz > 0 ? 1.f : -1.f; pl.pos.x += -f1 * pz * s; pl.pos.z += f0 * pz * s; }
        pl.speed *= 0.5f;
    }
    for (auto& p : peds) {
        if (p.knock > 0 || p.evade == EV_DIVE || p.evY > 0.25f) continue;   // mid-leap or sprawled: already out of the way
        float dx = pl.pos.x - p.pos.x, dz = pl.pos.z - p.pos.z;
        if (dx * dx + dz * dz > 0.55f * 0.55f || fabsf(pl.pos.y - p.pos.y) > 1.4f) continue;
        if (pl.hspeed() < 2.0f) continue;
        p.knock = 4.0f;
        if (pl.hspeed() > 7.5f) pl.bail("Ran into a pedestrian");
        else { pl.speed *= 0.4f; pl.vel = Vector3Scale(pl.vel, 0.4f); pl.say("Sorry!", {255, 160, 120, 255}, "Watch where you're rolling"); }
        return;
    }
}

// The people, pets and riders below draw through inst:: (instance.h): every box, sphere and cylinder is queued as an
// instance and the whole pass is drawn at once by inst::flush (in main), instead of vertex by vertex.

// The riders of a pedal bus: ten on stools facing the bar, pedalling, and a driver steering up front.
static void drawPedalRiders(const Car& c, int seed) {
    static const Color SKIN[] = {{240, 205, 175, 255}, {214, 170, 134, 255}, {170, 120, 85, 255}, {120, 80, 55, 255}};
    static const Color SHIRT[] = {{200, 50, 50, 255}, {40, 90, 170, 255}, {240, 240, 235, 255}, {240, 200, 60, 255}, {60, 140, 90, 255},
                                  {150, 80, 160, 255}, {230, 120, 40, 255}, {30, 30, 35, 255}};
    Color pants = {50, 56, 80, 255}, shoe = {35, 35, 38, 255};
    inst::push();
    inst::translate(c.pos.x, c.pos.y, c.pos.z);
    inst::rotate(-c.yaw * RAD2DEG, 0, 1, 0);
    inst::rotate(c.pitch * RAD2DEG, 0, 0, 1);
    auto limb = [](Vector3 a, Vector3 b, float r, Color k) { inst::cylinder(a, b, r, r * 0.85f, 5, k); };
    for (int k = 0; k < 10; k++) {
        uint32_t h = hashu(seed, k, 5);
        if ((h & 7) == 0) continue;                       // the odd empty stool
        float sx = -2.0f + (k / 2) * 0.9f, sd = (k & 1) ? 1.f : -1.f;   // sd: which side of the bar
        Color skin = SKIN[(h >> 3) % 4], shirt = SHIRT[(h >> 6) % 8];
        float bob = 0.03f * sinf(c.pedal * 2 + k);
        Vector3 hip = {sx, 1.1f + bob, sd * 0.98f};
        Vector3 sh = {sx, 1.62f + bob, sd * 0.88f};      // leaning in toward the bar
        limb(hip, sh, 0.17f, shirt);
        inst::sphere({sx, 1.84f + bob, sd * 0.84f}, 0.12f, 5, 6, skin);
        for (int a = -1; a <= 1; a += 2) {                // forearms on the bar
            Vector3 s = {sx + a * 0.18f, 1.55f + bob, sd * 0.86f}, hand = {sx + a * 0.16f, 1.3f, sd * 0.45f};
            limb(s, hand, 0.045f, shirt);
        }
        // legs on the cranks under the bar, the two pedals half a turn apart
        float ph = c.pedal + (h % 628) * 0.01f;
        for (int f = 0; f < 2; f++) {
            float ang = ph + f * PI;
            Vector3 foot = {sx + (f ? 0.11f : -0.11f), 0.6f + 0.16f * sinf(ang), sd * (0.62f - 0.16f * cosf(ang))};
            Vector3 hp = {hip.x + (f ? 0.1f : -0.1f), hip.y, hip.z};
            Vector3 knee = {(hp.x + foot.x) * 0.5f, (hp.y + foot.y) * 0.5f + 0.22f, (hp.z + foot.z) * 0.5f - sd * 0.12f};
            limb(hp, knee, 0.075f, pants);
            limb(knee, foot, 0.06f, pants);
            inst::cube(foot, 0.12f, 0.07f, 0.2f, shoe);
        }
    }
    // the driver, steering
    Color ds = {40, 40, 44, 255};
    limb({2.55f, 1.1f, 0}, {2.62f, 1.7f, 0}, 0.17f, ds);
    inst::sphere({2.66f, 1.92f, 0}, 0.12f, 5, 6, SKIN[seed % 4]);
    inst::cube({2.66f, 2.03f, 0}, 0.26f, 0.07f, 0.26f, {200, 40, 40, 255});
    for (int a = -1; a <= 1; a += 2) {
        limb({2.62f, 1.62f, a * 0.2f}, {2.95f, 1.32f, a * 0.3f}, 0.045f, ds);
        limb({2.55f, 1.1f, a * 0.12f}, {2.95f, 0.75f, a * 0.15f}, 0.07f, pants);
    }
    inst::pop();
}

// A dog on a leash, trotting a little ahead of its walker (sitting beside them when they stand still).
static void drawDog(const Ped& p, float now) {
    float s = p.petS, cy = cosf(p.yaw), sy = sinf(p.yaw);
    bool sit = p.mode == 2 || p.knock > 0;
    auto W = [&](float x, float y, float z) { return Vector3{p.pos.x + x * cy - z * sy, p.pos.y + y, p.pos.z + x * sy + z * cy}; };
    Vector3 at = sit ? W(0.15f, 0, 0.65f) : W(0.95f, 0, 0.5f);
    inst::push();
    inst::translate(at.x, at.y, at.z);
    inst::rotate(-p.yaw * RAD2DEG, 0, 1, 0);
    float sw = sit ? 0.f : sinf(p.phase * 1.6f) * 0.55f;
    for (int i = 0; i < 4; i++) {   // legs: diagonal pairs swing together
        float lx = (i & 1) ? 0.24f * s : -0.24f * s, lz = (i & 2) ? 0.09f * s : -0.09f * s;
        float a = ((i == 0 || i == 3) ? sw : -sw) * RAD2DEG;
        inst::push();
        inst::translate(lx, 0.3f * s, lz);
        inst::rotate(sit && lx < 0 ? 70.f : a, 0, 0, 1);
        inst::cube({0, -0.15f * s, 0}, 0.08f * s, 0.3f * s, 0.08f * s, p.petC);
        inst::pop();
    }
    inst::push();
    if (sit) { inst::translate(-0.2f * s, 0, 0); inst::rotate(28, 0, 0, 1); inst::translate(0.2f * s, -0.06f * s, 0); }
    inst::cube({0, 0.42f * s, 0}, 0.66f * s, 0.26f * s, 0.25f * s, p.petC);                 // body
    inst::cube({0.4f * s, 0.6f * s, 0}, 0.24f * s, 0.22f * s, 0.2f * s, p.petC);            // head
    inst::cube({0.56f * s, 0.55f * s, 0}, 0.14f * s, 0.1f * s, 0.12f * s, p.petC2);         // muzzle
    inst::cube({0.63f * s, 0.58f * s, 0}, 0.03f * s, 0.05f * s, 0.06f * s, {20, 20, 20, 255});
    for (int e = -1; e <= 1; e += 2) inst::cube({0.36f * s, 0.73f * s, e * 0.08f * s}, 0.06f * s, 0.12f * s, 0.06f * s, p.petC2);
    float wag = sinf(now * 13.f + p.phase) * 0.12f * s;
    inst::cylinder({-0.32f * s, 0.5f * s, 0}, {-0.48f * s, 0.72f * s, wag}, 0.035f * s, 0.02f * s, 4, p.petC);   // tail
    inst::pop();
    inst::pop();
    // the leash, from the walker's hand to the collar, sagging a little
    float ps = p.scale;
    Vector3 hand = W(0.12f * ps, 0.86f * ps, 0.27f * ps);
    Vector3 collar = sit ? W(0.15f + 0.3f * s, 0.62f * s, 0.65f) : W(0.95f + 0.3f * s, 0.6f * s, 0.5f);
    Vector3 mid = Vector3Lerp(hand, collar, 0.5f);
    mid.y -= 0.12f;
    DrawLine3D(hand, mid, {150, 30, 40, 255});
    DrawLine3D(mid, collar, {150, 30, 40, 255});
}

static void drawCat(const Cat& c) {
    inst::push();
    inst::translate(c.pos.x, c.pos.y, c.pos.z);
    inst::rotate(-c.yaw * RAD2DEG, 0, 1, 0);
    Color pink = {220, 150, 150, 255};
    float swish = sinf(c.t * 2.3f) * 0.08f;
    if (c.pose == 2) {   // running flat out, tail up
        float sw = sinf(c.t * 22.f) * 0.7f;
        for (int i = 0; i < 4; i++) {
            float lx = (i & 1) ? 0.14f : -0.14f, lz = (i & 2) ? 0.05f : -0.05f;
            inst::push();
            inst::translate(lx, 0.17f, lz);
            inst::rotate((lx > 0 ? sw : -sw) * RAD2DEG, 0, 0, 1);
            inst::cube({0, -0.08f, 0}, 0.05f, 0.17f, 0.05f, c.c);
            inst::pop();
        }
        inst::cube({0, 0.2f, 0}, 0.42f, 0.13f, 0.13f, c.c);
        inst::cube({0.02f, 0.25f, 0}, 0.2f, 0.04f, 0.12f, c.patch);
        inst::cube({0.27f, 0.25f, 0}, 0.14f, 0.12f, 0.13f, c.c);
        inst::cylinder({-0.2f, 0.24f, 0}, {-0.34f, 0.44f, swish}, 0.022f, 0.016f, 4, c.c);
        for (int e = -1; e <= 1; e += 2) inst::cube({0.27f, 0.33f, e * 0.04f}, 0.03f, 0.05f, 0.03f, c.c);
    } else if (c.pose == 1) {   // loafing: a compact lump with the paws tucked in
        inst::cube({0, 0.08f, 0}, 0.34f, 0.15f, 0.2f, c.c);
        inst::cube({-0.03f, 0.15f, 0}, 0.2f, 0.04f, 0.18f, c.patch);
        inst::cube({0.18f, 0.16f, 0}, 0.13f, 0.12f, 0.13f, c.c);
        for (int e = -1; e <= 1; e += 2) inst::cube({0.18f, 0.24f, e * 0.04f}, 0.03f, 0.05f, 0.03f, c.c);
        inst::cylinder({-0.16f, 0.03f, 0.06f}, {0.05f, 0.03f, 0.14f + swish * 0.3f}, 0.02f, 0.016f, 4, c.c);
    } else {   // sitting up, now and then dipping its head to groom
        float groom = fmaxf(0.f, sinf(c.t * 0.7f)) > 0.8f ? sinf(c.t * 9.f) * 0.03f - 0.06f : 0.f;
        inst::push();
        inst::rotate(35, 0, 0, 1);
        inst::cube({0, 0.14f, 0}, 0.3f, 0.15f, 0.16f, c.c);
        inst::cube({0, 0.2f, 0}, 0.2f, 0.04f, 0.15f, c.patch);
        inst::pop();
        inst::cube({0.08f, 0.04f, 0}, 0.05f, 0.08f, 0.12f, c.c);                         // front paws
        inst::cube({0.1f, 0.33f + groom, 0}, 0.14f, 0.13f, 0.14f, c.c);                  // head
        for (int e = -1; e <= 1; e += 2) {
            inst::cube({0.1f, 0.42f + groom, e * 0.045f}, 0.03f, 0.06f, 0.03f, c.c);     // ears
            inst::cube({0.11f, 0.42f + groom, e * 0.045f}, 0.01f, 0.03f, 0.015f, pink);
        }
        inst::cylinder({-0.12f, 0.02f, 0}, {-0.22f, 0.02f, 0.14f + swish}, 0.022f, 0.016f, 4, c.c);   // tail round the feet
    }
    inst::pop();
}

// A pedestrian in three levels of detail. Up close: jointed arms and legs, a round head with a hair style, shoulders,
// whatever they carry, and when standing a little idle routine (waiting, chatting, on a call, arms folded).
// Further out the same figure in plain boxes, and far away just a handful of them.
static void drawPerson(const Ped& p, float dist2, float now) {
    inst::push();
    inst::translate(p.pos.x, p.pos.y, p.pos.z);
    inst::rotate(-p.yaw * RAD2DEG, 0, 1, 0);
    if (p.knock > 0) {
        inst::translate(0, 0.15f, 0);
        inst::rotate(-85.f * clampf((4.0f - p.knock) / 0.3f, 0, 1), 0, 0, 1);
    } else if (p.evTilt > 0) {   // diving out of the way: pitching forward to land flat, then pushing back up
        inst::translate(0, 0.15f * p.evTilt, 0);
        inst::rotate(-88.f * p.evTilt, 0, 0, 1);
    }
    float s = p.scale;
    bool seated = p.mode == 3 && p.knock <= 0;
    if (seated) {
        if (p.grass) {   // a picnic blanket on the lawn
            static const Color BLANKET[] = {{196, 58, 52, 255}, {52, 96, 168, 255}, {226, 186, 64, 255}, {70, 140, 96, 255}, {236, 232, 220, 255}};
            Color bc = BLANKET[(int)(p.phase * 10.f) % 5];
            inst::cube({0.3f, 0.015f, 0}, 1.7f, 0.03f, 1.25f, bc);
            if (dist2 < 35 * 35) inst::cube({0.3f, 0.032f, 0}, 1.7f, 0.004f, 0.18f, scaleC(bc, 0.75f));   // a stripe across it
        }
        inst::translate(0, (p.grass ? 0.12f : 0.06f) - 0.92f * s, 0);   // drop the hips (0.92 m up when standing) onto the seat
    }
    inst::scale(s, s, s);
    Color shoe = {40, 40, 40, 255};
    bool standing = p.knock > 0 || p.gait < 0.35f;   // walking whenever they're actually on the move
    float stride = clampf(p.gait / 1.3f, 0.7f, 1.6f);  // a brisk step out of the way takes longer strides
    bool hop = p.evade == EV_HOP, dive = p.evade == EV_DIVE, inAir = (hop || dive) && p.evT < p.evMove;
    bool laptop = p.carry == CARRY_LAPTOP && p.knock <= 0;
    float sh = 0.19f * p.build;   // shoulder half-width
    if (dist2 > 110 * 110) {      // far away: legs, body, head
        float sw = standing ? 0.f : sinf(p.phase) * 0.5f;
        for (int side = -1; side <= 1; side += 2) {
            if (seated) {   // thighs forward along the seat, shins down (bench) or out along the lawn
                inst::cube({0.22f, 0.92f, side * 0.1f}, 0.45f, 0.15f, 0.15f, p.pants);
                if (p.grass) inst::cube({0.66f, 0.9f, side * 0.1f}, 0.45f, 0.13f, 0.13f, p.pants);
                else inst::cube({0.44f, 0.7f, side * 0.1f}, 0.15f, 0.45f, 0.15f, p.pants);
                continue;
            }
            inst::push();
            inst::translate(0, 0.9f, side * 0.1f);
            inst::rotate(sw * side * RAD2DEG, 0, 0, 1);
            inst::cube({0, -0.45f, 0}, 0.15f, 0.9f, 0.15f, p.pants);
            inst::pop();
        }
        inst::cube({0, 1.2f, 0}, 0.24f, 0.56f, sh * 2 + 0.06f, p.shirt);
        inst::cube({0, 1.66f, 0}, 0.2f, 0.26f, 0.19f, p.hairStyle == HAIR_BALD ? p.skin : p.hair);
        inst::pop();
        return;
    }
    bool close = dist2 < 35 * 35;
    float t = now * 1.0f + p.phase * 3.1f;
    // pose: arm swing / out / elbow per side (-1 left, +1 right), head yaw and pitch, body sway
    float swing[2], out[2] = {0.06f, 0.06f}, elbow[2], headYaw = 0, headPitch = 0, sway = 0, bob = 0;
    bool phoneHand = false, phoneEar = false;
    if (!standing) {
        for (int k = 0; k < 2; k++) {
            float sd = k ? 1.f : -1.f;
            swing[k] = -sinf(p.phase) * 0.42f * sd * stride;
            elbow[k] = 0.25f + 0.2f * std::max(0.f, swing[k]);
        }
        bob = fabsf(sinf(p.phase)) * 0.035f;
        if (p.carry == CARRY_PHONE) { swing[1] = 0.45f; elbow[1] = 1.45f; out[1] = 0.12f; headPitch = 0.35f; phoneHand = true; }
        if (p.carry == CARRY_SHOPPING) { swing[1] *= 0.35f; elbow[1] = 0.1f; }
        if (laptop) {   // walking along with the laptop open in both hands, eyes on the screen
            swing[0] = swing[1] = 0.35f; elbow[0] = elbow[1] = 1.0f; out[0] = out[1] = -0.15f; headPitch = 0.4f;
        }
    } else {
        for (int k = 0; k < 2; k++) { swing[k] = 0.04f; elbow[k] = 0.12f; }
        int idle = p.knock > 0 ? -1 : (p.carry == CARRY_PHONE && p.idle == IDLE_WAIT ? IDLE_ARMS : p.idle);
        sway = sinf(t * 0.55f) * 0.025f;
        if (idle == IDLE_WAIT) {            // looking up the street now and then, shifting weight
            headYaw = sinf(t * 0.35f) * 0.6f * smooth01(0.2f, 0.7f, fabsf(sinf(t * 0.17f)));
        } else if (idle == IDLE_CHAT) {     // talking with their hands, nodding along
            swing[1] = 0.35f + 0.25f * sinf(t * 1.9f);
            elbow[1] = 1.1f + 0.45f * sinf(t * 2.7f);
            out[1] = 0.2f;
            swing[0] = 0.2f; elbow[0] = 1.0f; out[0] = 0.5f;   // other hand on the hip
            headPitch = sinf(t * 2.3f) * 0.08f;
            headYaw = sinf(t * 0.5f) * 0.25f;
        } else if (idle == IDLE_CALL) {     // phone to the ear, pacing a little
            swing[1] = 0.25f; out[1] = 0.35f; elbow[1] = 2.5f;
            headPitch = -0.05f; headYaw = sinf(t * 0.4f) * 0.3f;
            phoneEar = true;
        } else if (idle == IDLE_ARMS) {     // arms folded, or both hands on a phone
            swing[0] = swing[1] = 0.5f; elbow[0] = elbow[1] = 1.55f; out[0] = out[1] = -0.32f;
            if (p.carry == CARRY_PHONE) { out[0] = out[1] = -0.2f; headPitch = 0.4f; phoneHand = true; }
        }
        if (laptop) {   // typing away: forearms out over the keyboard, head down, glancing up now and then
            swing[0] = swing[1] = 0.35f; elbow[0] = elbow[1] = 1.0f; out[0] = out[1] = -0.15f;
            elbow[0] += 0.05f * sinf(t * 9.f); elbow[1] += 0.05f * sinf(t * 8.f + 1.f);
            headPitch = 0.4f - 0.35f * smooth01(0.85f, 0.95f, sinf(t * 0.3f));
            headYaw = 0; phoneHand = phoneEar = false;
        } else if (seated) {
            if (p.carry == CARRY_PHONE) { swing[0] = swing[1] = 0.5f; elbow[0] = elbow[1] = 1.55f; out[0] = out[1] = -0.2f; headPitch = 0.4f; phoneHand = true; phoneEar = false; }
            else { swing[0] = swing[1] = 0.45f; elbow[0] = elbow[1] = 0.5f; out[0] = out[1] = 0.f; headPitch = 0.05f; }   // hands on the knees, watching the world go by
        }
    }
    // hop and dive poses: arms flung up and legs tucked in the air, a crouch on landing; arms out ahead in a dive
    if (hop) {
        for (int k = 0; k < 2; k++) {
            if (inAir) { swing[k] = 0.7f; out[k] = 0.75f; elbow[k] = 0.5f; }
            else { swing[k] = 0.35f; out[k] = 0.35f; elbow[k] = 0.4f; }
        }
        headYaw = 0; headPitch = -0.1f; phoneHand = phoneEar = false;
    } else if (dive) {
        float upK = 1.f - p.evTilt;   // 0 flat on the ground, 1 back on their feet
        for (int k = 0; k < 2; k++) {
            swing[k] = lerpf(2.9f, 0.4f, upK); out[k] = 0.15f; elbow[k] = lerpf(0.1f, 0.9f, upK);
        }
        headYaw = 0; headPitch = -0.3f * p.evTilt; phoneHand = phoneEar = false;
    }
    inst::translate(0, bob, sway);
    // legs: thigh and shin, the knee bending as the leg comes through
    for (int k = 0; k < 2; k++) {
        float sd = k ? 1.f : -1.f;
        float lp = p.phase + (k ? 0.f : PI);
        float th = standing ? (k ? 0.03f : -0.02f) : sinf(lp) * 0.42f * stride;
        float knee = standing ? 0.04f : 0.12f + std::max(0.f, cosf(lp)) * 0.55f * stride;
        if (hop) {
            if (inAir) { th = k ? 0.65f : -0.3f; knee = k ? 1.1f : 0.8f; }   // knees tucked up mid-hop
            else { float c = 1.f - smooth01(p.evMove, p.evDur, p.evT); th = 0.55f * c; knee = 1.0f * c; }   // landing crouch
        } else if (dive) {
            float upK = 1.f - p.evTilt;
            th = lerpf(-0.08f, 0.9f, upK * (1 - upK) * 4.f * (p.evT > p.evMove ? 1.f : 0.f));   // a knee comes under as they push up
            knee = lerpf(0.1f, 1.3f, upK * (1 - upK) * 4.f * (p.evT > p.evMove ? 1.f : 0.f));
            if (k == 0) th *= 0.5f;
        }
        if (seated) { th = PI / 2; knee = p.grass ? 0.25f + 0.1f * k : PI / 2 - 0.15f * k; }
        inst::push();
        inst::translate(0, 0.92f, sd * 0.1f);
        inst::rotate(th * RAD2DEG, 0, 0, 1);
        inst::cube({0, -0.22f, 0}, 0.16f, 0.46f, 0.16f, p.pants);
        inst::translate(0, -0.45f, 0);
        inst::rotate(-knee * RAD2DEG, 0, 0, 1);
        inst::cube({0, -0.21f, 0}, 0.13f, 0.43f, 0.13f, p.pants);
        inst::cube({0.05f, -0.43f, 0}, 0.26f, 0.08f, 0.12f, shoe);
        inst::pop();
    }
    // hips, waist, chest and rounded shoulders
    inst::cube({0, 0.96f, 0}, 0.21f, 0.16f, 0.3f, p.pants);
    inst::cube({0, 1.1f, 0}, 0.2f, 0.2f, 0.32f + 0.02f * p.build, p.shirt);
    inst::cube({0, 1.32f, 0}, 0.24f, 0.28f, sh * 2, p.shirt);
    if (close) inst::zcylinder(1.43f, sh - 0.02f, 0.08f, 6, p.shirt);
    inst::cube({0, 1.5f, 0}, 0.09f, 0.1f, 0.09f, p.skin);   // neck
    // carried things on the body
    if (p.carry == CARRY_BACKPACK) {
        inst::cube({-0.2f, 1.24f, 0}, 0.15f, 0.4f, 0.3f, p.extra);
        if (close) inst::cube({-0.28f, 1.18f, 0}, 0.04f, 0.16f, 0.22f, scaleC(p.extra, 0.8f));
    } else if (p.carry == CARRY_SHOULDER) {
        inst::cube({0.0f, 0.98f, -(sh + 0.05f)}, 0.27f, 0.22f, 0.08f, p.extra);
        if (close) inst::cube({0.0f, 1.25f, -(sh - 0.04f)}, 0.035f, 0.34f, 0.03f, scaleC(p.extra, 0.7f));   // strap
    }
    // arms: upper arm, forearm, hand (and whatever is in it)
    for (int k = 0; k < 2; k++) {
        float sd = k ? 1.f : -1.f;
        inst::push();
        inst::translate(0, 1.43f, sd * (sh + 0.03f));
        inst::rotate(swing[k] * RAD2DEG, 0, 0, 1);
        inst::rotate(-sd * out[k] * RAD2DEG, 1, 0, 0);
        inst::cube({0, -0.15f, 0}, 0.1f, 0.3f, 0.1f, p.shirt);
        inst::translate(0, -0.29f, 0);
        inst::rotate(elbow[k] * RAD2DEG, 0, 0, 1);
        inst::cube({0, -0.13f, 0}, 0.085f, 0.26f, 0.085f, p.shirt);
        inst::cube({0, -0.29f, 0}, 0.075f, 0.09f, 0.06f, p.skin);
        if (k == 1 && (phoneHand || phoneEar)) {
            inst::cube({0.02f, -0.31f, 0}, 0.025f, 0.14f, 0.07f, {25, 25, 28, 255});
            if (close) inst::cube({0.035f, -0.31f, 0}, 0.006f, 0.12f, 0.06f, {150, 190, 255, M_EMISSIVE});   // the screen
        }
        if (k == 1 && p.carry == CARRY_SHOPPING) {
            inst::rotate(-(swing[k] + elbow[k]) * RAD2DEG, 0, 0, 1);   // the bag hangs straight down
            inst::cube({0, -0.5f, 0}, 0.3f, 0.32f, 0.12f, p.extra);
            if (close) inst::cube({0, -0.32f, 0}, 0.02f, 0.06f, 0.1f, scaleC(p.extra, 0.6f));
        }
        inst::pop();
    }
    if (laptop) {   // the laptop: open on the lap, or held out in front, the screen glowing toward its owner
        float by = seated ? 1.01f : 1.06f;
        Color shell = (int)(p.phase * 7.f) % 3 ? Color{176, 178, 184, 255} : Color{46, 48, 54, 255};
        inst::cube({0.37f, by, 0}, 0.24f, 0.02f, 0.33f, shell);
        inst::push();
        inst::translate(0.49f, by + 0.01f, 0);
        inst::rotate(-14.f, 0, 0, 1);   // the screen tipped back a little
        inst::cube({0, 0.11f, 0}, 0.014f, 0.22f, 0.33f, shell);
        if (close) inst::cube({-0.009f, 0.11f, 0}, 0.004f, 0.19f, 0.29f, {170, 200, 245, M_EMISSIVE});
        inst::pop();
    }
    // head and hair
    inst::push();
    inst::translate(0, 1.56f, 0);
    inst::rotate(headYaw * RAD2DEG, 0, 1, 0);
    inst::rotate(-headPitch * RAD2DEG, 0, 0, 1);
    Vector3 hc = {0.01f, 0.13f, 0};
    Color hair = p.hairStyle == HAIR_CAP ? p.extra : p.hair;
    if (close) {
        inst::push();
        inst::translate(hc.x, hc.y, hc.z);
        inst::scale(1.f, 1.16f, 0.95f);
        inst::sphere({0, 0, 0}, 0.11f, 5, 8, p.skin);
        inst::pop();
        Color eye = {30, 26, 24, 255};
        for (int e = -1; e <= 1; e += 2) inst::cube({hc.x + 0.1f, hc.y + 0.015f, e * 0.038f}, 0.012f, 0.018f, 0.022f, eye);
        inst::cube({hc.x + 0.112f, hc.y - 0.015f, 0}, 0.02f, 0.035f, 0.025f, scaleC(p.skin, 0.92f));   // nose
        if (p.hairStyle != HAIR_BALD) {   // a cap of hair over the crown and the back of the head
            inst::push();
            float big = p.hairStyle == HAIR_CURLY ? 1.22f : 1.f;
            inst::translate(hc.x - 0.014f, hc.y + 0.045f, 0);
            inst::scale(1.03f * big, (p.hairStyle == HAIR_CAP ? 0.55f : 0.66f) * big, 1.03f * big);
            inst::sphere({0, 0, 0}, 0.118f, 4, 8, hair);
            inst::pop();
        }
    } else {
        inst::cube({hc.x, hc.y, 0}, 0.2f, 0.25f, 0.19f, p.skin);
        if (p.hairStyle != HAIR_BALD) inst::cube({hc.x - 0.015f, hc.y + 0.11f, 0}, 0.23f, 0.06f, 0.22f, hair);
    }
    switch (p.hairStyle) {
    case HAIR_SHORT: if (!close) inst::cube({hc.x - 0.1f, hc.y + 0.03f, 0}, 0.04f, 0.15f, 0.2f, hair); break;
    case HAIR_LONG:
        inst::cube({hc.x - 0.09f, hc.y - 0.09f, 0}, 0.07f, 0.32f, 0.21f, hair);
        if (close) for (int e = -1; e <= 1; e += 2) inst::cube({hc.x - 0.02f, hc.y - 0.06f, e * 0.1f}, 0.12f, 0.24f, 0.03f, hair);
        break;
    case HAIR_BUN:
        if (close) inst::sphere({hc.x - 0.1f, hc.y + 0.08f, 0}, 0.055f, 4, 6, hair);
        else inst::cube({hc.x - 0.11f, hc.y + 0.07f, 0}, 0.08f, 0.08f, 0.08f, hair);
        break;
    case HAIR_CAP: inst::cube({hc.x + 0.12f, hc.y + 0.065f, 0}, 0.12f, 0.018f, 0.17f, scaleC(p.extra, 0.8f)); break;   // the brim
    case HAIR_CURLY: if (!close) inst::cube({hc.x - 0.02f, hc.y + 0.06f, 0}, 0.26f, 0.16f, 0.26f, hair); break;
    }
    inst::pop();
    inst::pop();
}

// Cable car riders, drawn in the person's own frame (x = the way they face, origin at the feet or the seat).
enum { R_HANG, R_BENCH, R_INSIDE, R_GRIP };
static void drawRider(const Ped& p, int pose, int side, float t) {
    float sc = pose == R_INSIDE ? std::min(p.scale, 1.0f) : p.scale;
    inst::scale(sc, sc, sc);
    Color shoe = {40, 40, 40, 255};
    if (pose == R_HANG || pose == R_GRIP) {
        if (pose == R_HANG) inst::rotate(side * (9 + 3 * sinf(t * 1.7f + p.phase)), 1, 0, 0); // lean out over the street
        for (int k = -1; k <= 1; k += 2) {
            inst::cube({0, 0.46f, k * 0.1f}, 0.15f, 0.88f, 0.15f, p.pants);
            inst::cube({0.05f, 0.04f, k * 0.1f}, 0.24f, 0.07f, 0.13f, shoe);
        }
    } else if (pose == R_BENCH) {
        for (int k = -1; k <= 1; k += 2) {
            inst::cube({0.22f, 0.02f, k * 0.1f}, 0.45f, 0.15f, 0.15f, p.pants);   // thighs
            inst::cube({0.44f, -0.23f, k * 0.1f}, 0.15f, 0.46f, 0.15f, p.pants);  // shins
            inst::cube({0.49f, -0.47f, k * 0.1f}, 0.24f, 0.07f, 0.13f, shoe);
        }
        inst::translate(0, -0.89f, 0);   // same body as standing, from the hips up
    } else inst::translate(0, -0.89f, 0);
    inst::cube({0, 1.18f, 0}, 0.24f, 0.58f, 0.4f, p.shirt);
    inst::cube({0, 1.6f, 0}, 0.2f, 0.24f, 0.2f, p.skin);
    inst::cube({-0.02f, 1.74f, 0}, 0.23f, 0.06f, 0.22f, p.hair);
    for (int k = -1; k <= 1; k += 2) {
        inst::push();
        inst::translate(0, 1.43f, k * 0.25f);
        if (pose == R_HANG && k == -side) inst::rotate(side * 150.f, 1, 0, 0);            // inner hand up on the pole
        else if (pose == R_HANG) inst::rotate(-side * (15 + 10 * sinf(t * 0.9f + p.phase)), 1, 0, 0);
        else if (pose == R_GRIP) inst::rotate(35.f, 0, 0, 1);                               // both hands on the grip lever
        else if (pose == R_BENCH) inst::rotate(25.f, 0, 0, 1);                              // hands in the lap
        inst::cube({0, -0.28f, 0}, 0.11f, 0.58f, 0.11f, p.shirt);
        inst::cube({0, -0.6f, 0}, 0.1f, 0.1f, 0.1f, p.skin);
        inst::pop();
    }
}

// The usual crowd on a cable car: people hanging off the running boards, sitting out on the end benches,
// passengers inside and the gripman at the front. Who is aboard is fixed per car.
static void drawCableRiders(const Car& c, int idx) {
    RNG r(9137 + idx * 7919);
    float t = (float)GetTime();
    inst::push();
    inst::translate(c.pos.x, c.pos.y, c.pos.z);
    inst::rotate(-c.yaw * RAD2DEG, 0, 1, 0);
    inst::rotate(c.pitch * RAD2DEG, 0, 0, 1);
    auto place = [&](float x, float y, float z, float face, int pose, int side) {
        Ped p; colorPed(p, r);
        inst::push();
        inst::translate(x, y, z);
        inst::rotate(face, 0, 1, 0);
        drawRider(p, pose, side, t);
        inst::pop();
    };
    for (int s = -1; s <= 1; s += 2)
        for (float x : {-3.5f, -2.7f, 2.7f, 3.5f})
            if (r.chance(0.6f)) place(x, 0.66f, s * 1.5f, x > 0 ? 0.f : 180.f, R_HANG, x > 0 ? s : -s);
    for (int s = -1; s <= 1; s += 2)
        for (float x : {-3.85f, -3.15f, 3.15f, 3.85f})
            if (r.chance(0.65f)) place(x, 1.54f, s * 0.3f, -90.f * s, R_BENCH, s);
    for (int s = -1; s <= 1; s += 2)
        for (float x : {-1.5f, -0.75f, 0.f, 0.75f, 1.5f})
            if (r.chance(0.5f)) place(x, 1.65f, s * 0.75f, 90.f * s, R_INSIDE, s);
    place(2.45f, 1.04f, 0, 0, R_GRIP, 0);
    inst::cube({2.85f, 1.45f, 0}, 0.06f, 0.9f, 0.06f, {60, 60, 64, 255});    // grip lever
    inst::pop();
}

void Traffic::drawShadows(Vector3 cam) const {
    for (auto& c : cars) {
        if (sq(c.pos.x - cam.x) + sq(c.pos.z - cam.z) > 140 * 140) continue;
        const CarModel& m = carModel(c.type);
        rlPushMatrix();
        rlTranslatef(c.pos.x, c.pos.y + 0.05f, c.pos.z);
        rlRotatef(-c.yaw * RAD2DEG, 0, 1, 0);
        rlRotatef(c.pitch * RAD2DEG, 0, 0, 1);
        DrawCube({0, 0, 0}, m.halfLen * 2 + 0.4f, 0.02f, m.halfWid * 2 + 0.4f, {0, 0, 0, 55});
        rlPopMatrix();
    }
    for (auto& p : peds) {
        if (sq(p.pos.x - cam.x) + sq(p.pos.z - cam.z) > 70 * 70 || p.knock > 0) continue;
        DrawCylinder({p.pos.x, p.pos.y + 0.12f, p.pos.z}, 0.38f, 0.38f, 0.01f, 10, {0, 0, 0, 40});
    }
}

void Traffic::draw(Vector3 cam, Vector3 camFwd, bool nearPass) const {
    for (auto& c : cars) {
        Vector3 d = Vector3Subtract(c.pos, cam);
        float dist2 = d.x * d.x + d.z * d.z;
        if (dist2 > 420 * 420) continue;
        if (!nearPass && dist2 < 235 * 235) continue;   // wholly inside the near pass: drawn there
        if (Vector3DotProduct(d, camFwd) < -15) continue;
        drawCarModel(c.type, c.body, c.pos, c.yaw, c.pitch, c.braking);
        if (c.type == CT_CABLECAR && dist2 < 170 * 170) drawCableRiders(c, (int)(&c - cars.data()));
        if (c.type == CT_PEDALBUS && dist2 < 140 * 140) drawPedalRiders(c, (int)(&c - cars.data()));
    }
    if (!nearPass) return;   // people and pets never reach the far pass (250 m+)
    float now = (float)GetTime();
    for (auto& p : peds) {
        Vector3 d = Vector3Subtract(p.pos, cam);
        float dist2 = d.x * d.x + d.z * d.z;
        if (dist2 > 230 * 230) continue;
        if (Vector3DotProduct(d, camFwd) < -5) continue;
        drawPerson(p, dist2, now);
        if (p.pet && dist2 < 150 * 150) drawDog(p, now);
    }
    for (auto& c : cats) {
        Vector3 d = Vector3Subtract(c.pos, cam);
        if (d.x * d.x + d.z * d.z > 90 * 90 || Vector3DotProduct(d, camFwd) < -3) continue;
        drawCat(c);
    }
}

void Traffic::drawCasters(Vector3 c, float rCars, float rPeds) const {
    for (auto& k : cars) {
        float dist2 = sq(k.pos.x - c.x) + sq(k.pos.z - c.z);
        if (dist2 > rCars * rCars) continue;
        drawCarModel(k.type, k.body, k.pos, k.yaw, k.pitch, false);
        if (k.type == CT_PEDALBUS && dist2 < 60 * 60) drawPedalRiders(k, (int)(&k - cars.data()));
    }
    if (rPeds <= 0) return;
    float now = (float)GetTime();
    for (auto& p : peds) {
        float dist2 = sq(p.pos.x - c.x) + sq(p.pos.z - c.z);
        if (dist2 > rPeds * rPeds) continue;
        drawPerson(p, 1e9f, now);   // the far (cheap) version is plenty for a shadow
        if (p.pet && dist2 < 40 * 40) drawDog(p, now);
    }
}

void Traffic::drawTrain(const World& w, float s, int dir, Vector3 cam) {
    for (int k = 0; k < 5; k++) {
        float sc = s - dir * (k * 25.8f + 12.9f);
        if (sc < 0 || sc > w.railLen()) continue;
        Vector2 d;
        Vector3 c = w.trackPoint(sc, 2.4f * dir, &d);
        if (sq(c.x - cam.x) + sq(c.z - cam.z) > 3000.f * 3000.f) continue;
        float yaw = atan2f(d.y * dir, d.x * dir);
        float pitch = atan2f((w.railGrade(sc + 10 * dir) - w.railGrade(sc - 10 * dir)), 20);
        bool cab = k == 0 || k == 4;
        if (k == 4) yaw += PI, pitch = -pitch;
        drawCarModel(cab ? CT_CALTRAIN_CAB : CT_CALTRAIN, WHITE, {c.x, c.y + 0.3f, c.z}, yaw, pitch, false);
    }
}

void Traffic::drawSignals(const World& w, Vector3 cam) const {
    static const Color ON[3] = {{60, 255, 120, M_EMISSIVE}, {255, 190, 30, M_EMISSIVE}, {255, 40, 30, M_EMISSIVE}};
    static const Color OFF[3] = {{24, 44, 32, 255}, {54, 44, 20, 255}, {54, 22, 20, 255}};
    for (auto& h : w.signalHeads) {
        if (sq(h.pos.x - cam.x) + sq(h.pos.z - cam.z) > 240.f * 240.f) continue;
        int st = signalState(h.node, h.axis, clock);
        for (int k = 0; k < 3; k++) {   // red on top, then yellow, green at the bottom
            int lamp = 2 - k;
            Vector3 p = {h.pos.x, h.pos.y + 0.36f - k * 0.36f, h.pos.z};
            bool x = fabsf(h.face.x) > 0.5f;
            DrawCube(p, x ? 0.04f : 0.22f, 0.22f, x ? 0.22f : 0.04f, st == lamp ? ON[lamp] : OFF[lamp]);
        }
    }
}

void Traffic::drawTrains(const World& w, Vector3 cam) const {
    for (auto& t : trains) drawTrain(w, t.s, t.dir, cam);
}
