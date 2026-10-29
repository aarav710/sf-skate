// Headless gameplay checks: drives the skater with scripted input and checks the outcome.
#include "../src/world.h"
#include "../src/player.h"
#include "../src/chase.h"
#include <cstdio>
#include <algorithm>

static World W;
static int fails = 0;
static void check(bool ok, const char* name, const char* detail) {
    printf("%s  %-34s %s\n", ok ? "PASS" : "FAIL", name, detail);
    if (!ok) fails++;
}
static void progress(const char*, float) {}

struct Sim {
    Player p;
    void spawn(float x, float z, float yaw, float speed = 0) {
        p.spawn({x, W.ground(x, z, 500), z}, yaw);
        p.speed = speed;
    }
    // run n frames with input; returns true if `stop` became true
    template <class F> bool run(int n, Input in, F stop) {
        for (int i = 0; i < n; i++) {
            p.update(1.f / 60, in, W);
            in.ollie = in.kickflip = in.heelflip = in.shove = false; // presses last one frame
            if (stop(p)) return true;
        }
        return false;
    }
    bool run(int n, Input in) { return run(n, in, [](Player&) { return false; }); }
};

// Rides a skater along a house's route (street, stair foot, landing, door) like a person steering toward each point.
static bool rideRoute(Sim& s, const House& hs, bool fromStreet) {
    int n = hs.nRoute, k = 0;
    for (int f = 0; f < 60 * 25; f++) {
        Vector2 tgt = hs.route[k];
        float want = atan2f(tgt.y - s.p.pos.z, tgt.x - s.p.pos.x);
        Input in;
        in.steer = clampf(wrapPi(want - s.p.yaw) * 3.f, -1.f, 1.f);
        in.push = s.p.speed < (k < n - 2 ? 4.0f : 3.2f);
        s.p.update(1.f / 60, in, W);
        if (sqrtf(sq(tgt.x - s.p.pos.x) + sq(tgt.y - s.p.pos.z)) < 0.9f && k < n - 1) k++;
        if (W.houseAt(s.p.pos.x, s.p.pos.z, s.p.pos.y) >= 0) return true;
        if (s.p.state == Player::BAIL) return false;
    }
    (void)fromStreet;
    return false;
}

int main() {
    SetTraceLogLevel(LOG_WARNING);
    W.generate(progress);
    char b[256];

    {   // pushing on a flat street
        Sim s; s.spawn(1045 + 2.4f, -80, PI / 2);
        Input in; in.push = true;
        s.run(180, in);
        snprintf(b, sizeof b, "speed after 3s = %.1f m/s", s.p.speed);
        check(s.p.speed > 13 && s.p.speed < 18.5f && s.p.state == Player::RIDE, "push accelerates", b);
    }
    {   // ollie and land
        Sim s; s.spawn(1045 + 2.4f, -80, PI / 2, 8);
        Input in; in.ollie = true;
        s.run(1, in);
        bool air = s.p.state == Player::AIR;
        float peak = 0, y0 = s.p.pos.y;
        bool landed = s.run(120, Input{}, [&](Player& p) { peak = std::max(peak, p.pos.y - y0); return p.state == Player::RIDE; });
        snprintf(b, sizeof b, "air=%d landed=%d peak=%.2fm score=%lld", air, landed, peak, s.p.score);
        check(air && landed && peak > 1.3f && peak < 2.2f && (s.p.score == 100 || s.p.score == 150), "ollie", b);
    }
    {   // a landing bounces: the board pops back up a little before rolling out
        Sim s; s.spawn(1045 + 2.4f, -80, PI / 2, 8);
        Input in; in.ollie = true; s.run(1, in);
        bool left = false, hopped = false;
        float hop = 0;
        s.run(240, Input{}, [&](Player& p) {
            if (p.state == Player::AIR && p.vy < 0 && p.pos.y > 0.2f) left = true;
            if (left && p.state == Player::AIR && p.vy > 0.5f) { hopped = true; hop = std::max(hop, p.vy); }
            return false;
        });
        snprintf(b, sizeof b, "hopped=%d launch=%.1f m/s, final state=%d", hopped, hop, s.p.state);
        check(hopped && hop < 3.5f && s.p.state == Player::RIDE, "landing bounce", b);
    }
    {   // kickflip landed cleanly
        Sim s; s.spawn(1045 + 2.4f, -80, PI / 2, 8);
        Input in; in.ollie = true; s.run(1, in);
        Input k; k.kickflip = true; s.run(1, k);
        s.run(120, Input{}, [](Player& p) { return p.state != Player::AIR; });
        snprintf(b, sizeof b, "state=%d score=%lld", s.p.state, s.p.score);
        check(s.p.state == Player::RIDE && s.p.score >= 1000 && s.p.score <= 1400, "kickflip scores ~1200", b);
    }
    {   // late kickflip should bail
        Sim s; s.spawn(1045 + 2.4f, -80, PI / 2, 8);
        Input in; in.ollie = true; s.run(1, in);
        s.run(50, Input{});
        Input k; k.kickflip = true; s.run(1, k);
        s.run(60, Input{}, [](Player& p) { return p.state != Player::AIR; });
        snprintf(b, sizeof b, "state=%d (3 = bail)", s.p.state);
        check(s.p.state == Player::BAIL, "late kickflip bails", b);
    }
    {   // 360 spin
        Sim s; s.spawn(1045 + 2.4f, -80, PI / 2, 8);
        Input in; in.ollie = true; s.run(1, in);
        Input sp; sp.steer = 1; s.run(63, sp, [](Player& p) { return fabsf(p.spinAccum) > 2 * PI; });
        s.run(120, Input{}, [](Player& p) { return p.state != Player::AIR; });
        snprintf(b, sizeof b, "state=%d score=%lld", s.p.state, s.p.score);
        check(s.p.state == Player::RIDE && s.p.score > 100, "FS 360 lands", b);
    }
    {   // grind the flat bar at Embarcadero Plaza
        Sim s; s.spawn(1186, -238, 0, 7);
        bool ground = s.run(60, Input{}, [](Player& p) { return p.pos.x > 1193.5f; });
        Input in; in.ollie = true; s.run(1, in);
        bool grind = s.run(90, Input{}, [](Player& p) { return p.state == Player::GRIND; });
        snprintf(b, sizeof b, "reached=%d grind=%d pos=%.1f,%.2f,%.1f", ground, grind, s.p.pos.x, s.p.pos.y, s.p.pos.z);
        check(grind, "ollie onto rail starts grind", b);
        Input bal; // try to keep balance with a simple controller
        s.run(200, bal, [](Player& p) { return p.state != Player::GRIND; });
        s.run(120, Input{}, [](Player& p) { return p.state == Player::RIDE || p.state == Player::BAIL; });
        snprintf(b, sizeof b, "state=%d score=%lld", s.p.state, s.p.score);
        check(s.p.score > 0 || s.p.state == Player::BAIL, "grind ends (banked or bailed)", b);
    }
    {   // kicker ramp launches you
        Sim s; s.spawn(1233, -241, 0, 9);
        bool air = s.run(90, Input{}, [](Player& p) { return p.state == Player::AIR; });
        snprintf(b, sizeof b, "air=%d x=%.1f vy=%.1f", air, s.p.pos.x, s.p.vy);
        check(air && s.p.pos.x > 1241, "kicker launches", b);
    }
    {   // rolling downhill from Twin Peaks gains speed without pushing
        Sim s; s.spawn(-282, 878, -0.6f, 0);
        s.run(240, Input{});
        snprintf(b, sizeof b, "speed after 4s = %.1f m/s (%s)", s.p.hspeed(), W.district(s.p.pos.x, s.p.pos.z));
        check(s.p.hspeed() > 6, "gravity on hills", b);
    }
    {   // find a real rowhouse facing a street and ride into it
        const Platform* tgt = nullptr;
        for (auto& pl : W.plats)
            if (pl.kind == PK_BUILDING && pl.x0 > -1300 && pl.x0 < 1000 && pl.z0 > -600 && pl.z0 < 1500 && pl.x1 - pl.x0 > 15 &&
                pl.z1 - pl.z0 > 6 && pl.h0 - W.terrain(pl.x0, (pl.z0 + pl.z1) / 2) > 6 &&
                W.zoneFlags(pl.x0 - 6, (pl.z0 + pl.z1) / 2, false) == 0 &&
                fabsf(W.terrain(pl.x0 - 6, (pl.z0 + pl.z1) / 2) - W.terrain(pl.x0 - 1, (pl.z0 + pl.z1) / 2)) < 0.3f) {
                Vector3 q = {pl.x0 - 6, W.terrain(pl.x0 - 6, (pl.z0 + pl.z1) / 2), (pl.z0 + pl.z1) / 2};
                Vector2 n;
                Vector3 q2 = q;
                if (!W.collide(q2, 1.0f, q.y, 1.7f, &n)) { tgt = &pl; break; }
            }
        check(tgt != nullptr, "found a test building", "");
        if (tgt) {
            float zc = (tgt->z0 + tgt->z1) / 2;
            Sim s; s.spawn(tgt->x0 - 6, zc, 0, 13);
            bool bailed = s.run(120, Input{}, [](Player& p) { return p.state == Player::BAIL; });
            snprintf(b, sizeof b, "bailed=%d x=%.1f wall at %.1f", bailed, s.p.pos.x, tgt->x0);
            check(bailed && s.p.pos.x < tgt->x0, "hitting a wall fast bails", b);
            Sim s2; s2.spawn(tgt->x0 - 6, zc, 0, 2);
            Input in; in.push = true;
            s2.run(400, in);
            snprintf(b, sizeof b, "x=%.1f state=%d wall at %.1f", s2.p.pos.x, s2.p.state, tgt->x0);
            check(s2.p.pos.x < tgt->x0, "buildings are solid", b);
        }
    }
    {   // ride into the Pacific
        Sim s; s.spawn(-1440, 0, PI, 6);
        Input in; in.push = true;
        bool splash = s.run(900, in, [](Player& p) { return p.splashed; });
        s.run(200, Input{});
        snprintf(b, sizeof b, "splash=%d respawned at x=%.1f y=%.1f state=%d", splash, s.p.pos.x, s.p.pos.y, s.p.state);
        check(splash && s.p.pos.y > 0.5f && s.p.state == Player::RIDE, "splash + respawn", b);
    }
    {   // the Golden Gate railing keeps you on the deck
        Sim s; s.spawn(-1100, -1500, 0, 8); // heading east toward the railing
        Input in; in.push = true;
        s.run(300, in);
        snprintf(b, sizeof b, "x=%.2f y=%.1f state=%d", s.p.pos.x, s.p.pos.y, s.p.state);
        check(s.p.pos.x < -1086 && s.p.pos.y > 25, "bridge railing holds", b);
    }
    {   // ride the length of the Bay Bridge deck
        Sim s; s.spawn(1500, 335 + 4, 0, 10);
        Input in; in.push = true; in.tuck = true;
        s.run(60 * 20, in);
        snprintf(b, sizeof b, "x=%.0f y=%.1f state=%d (%s)", s.p.pos.x, s.p.pos.y, s.p.state, W.district(s.p.pos.x, s.p.pos.z));
        check(s.p.pos.x > 1600 && s.p.pos.y > 20 && s.p.state == Player::RIDE, "Bay Bridge is rideable", b);
    }
    {   // spawn points are all on solid ground
        int bad = 0;
        for (auto& sp : W.spots) {
            Sim s; s.spawn(sp.pos.x, sp.pos.z, sp.yaw);
            s.run(30, Input{});
            if (s.p.state != Player::RIDE) { bad++; printf("      spot %s state %d\n", sp.name, s.p.state); }
        }
        snprintf(b, sizeof b, "%d bad of %d", bad, (int)W.spots.size());
        check(bad == 0, "teleport spots are safe", b);
    }
    {   // every token is reachable-ish: above ground, not inside a wall
        int bad = 0;
        for (auto& t : W.tokens) {
            Vector3 p = {t.pos.x, t.pos.y - 1.4f, t.pos.z};
            Vector3 q = p;
            Vector2 n;
            if (W.collide(q, 0.3f, p.y, 1.5f, &n) || W.isWater(p.x, p.z, p.y)) { bad++; printf("      token %s blocked\n", t.name); }
        }
        snprintf(b, sizeof b, "%d bad of %d", bad, (int)W.tokens.size());
        check(bad == 0, "tokens are not inside walls", b);
    }
    {   // streetcar lines exist and run through the city
        std::string names;
        float total = 0;
        for (size_t i = 0; i < W.tramPaths.size(); i++) { names += W.tramNames[i]; names += " "; total += W.tramPaths[i].len; }
        snprintf(b, sizeof b, "%d lines, %.1f km: %s", (int)W.tramPaths.size(), total / 1000, names.c_str());
        check(W.tramPaths.size() >= 6 && total > 6000, "Muni Metro lines built", b);
    }
    {   // ride the highway from the south end of the city to SFO
        Sim s; s.spawn(1268, 1770, atan2f(120, 235), 10);
        Input in; in.push = true;
        s.run(60 * 25, in, [](Player& p) { return p.pos.x > 1495; });
        snprintf(b, sizeof b, "x=%.0f z=%.0f state=%d (%s)", s.p.pos.x, s.p.pos.z, s.p.state, W.district(s.p.pos.x, s.p.pos.z));
        check(s.p.pos.x > 1495 && s.p.state == Player::RIDE, "highway reaches the airport", b);
        Sim r; r.spawn(2010, 2262, PI, 12); // runway is long, flat and open
        Input go; go.push = true; go.tuck = true;
        r.run(60 * 20, go);
        snprintf(b, sizeof b, "x=%.0f speed=%.1f state=%d", r.p.pos.x, r.p.speed, r.p.state);
        check(r.p.pos.x < 1800 && r.p.state == Player::RIDE, "runway is skateable", b);
    }
    {   // Caltrain: gentle grade, clear right of way, five stations
        float maxGrade = 0, L = W.railLen();
        for (float t = 1800; t < L - 50; t += 20) maxGrade = std::max(maxGrade, fabsf(W.railGrade(t + 20) - W.railGrade(t)) / 20);
        int blocked = 0;
        for (auto& pl : W.plats) {
            if (pl.kind != PK_BUILDING && pl.kind != PK_CAR) continue;
            float s;
            float cx = (pl.x0 + pl.x1) / 2, cz = (pl.z0 + pl.z1) / 2;
            float half = std::max(pl.x1 - pl.x0, pl.z1 - pl.z0) / 2;
            if (cz > 800 && W.railDistance(cx, cz, &s) < 5.5f + half * 0.5f && W.railDistance(cx, cz, &s) < 6) blocked++;
        }
        snprintf(b, sizeof b, "length %.1f km, %d stations, steepest grade %.1f%%, %d obstacles on the tracks",
                 L / 1000, (int)W.stations.size(), maxGrade * 100, blocked);
        check(W.stations.size() == 5 && maxGrade < 0.04f && blocked == 0 && L > 9000, "Caltrain line", b);
    }
    {   // Menlo Park: big homes, oak-lined streets you can skate down
        int homes = 0;
        for (auto& pl : W.plats)
            if (pl.kind == PK_BUILDING && pl.x0 > MX0 && pl.x1 < MX1 && pl.z0 > MZ0 && pl.z1 < MZ1 && (pl.x1 - pl.x0) > 15) homes++;
        int oaks = 0;
        for (auto& pl : W.plats)
            if (pl.kind == PK_POLE && pl.x0 > MX0 && pl.x1 < MX1 && pl.z0 > MZ0 && pl.z1 < MZ1 && (pl.x1 - pl.x0) > 0.6f) oaks++;
        Sim s; s.spawn(W.mp.node(9, 0).x + 2.4f, W.mp.node(0, 2).y, PI / 2, 8); // ride south down a residential street
        Input in; in.push = true;
        s.run(60 * 6, in);
        snprintf(b, sizeof b, "%d houses, %d big trees, skated to z=%.0f state=%d (%s)", homes, oaks, s.p.pos.z, s.p.state,
                 W.district(s.p.pos.x, s.p.pos.z));
        check(homes > 150 && oaks > 300 && s.p.state == Player::RIDE && s.p.pos.z > W.mp.node(0, 2).y + 60, "Menlo Park", b);
    }
    {   // houses you can walk into
        int n = (int)W.houses.size(), enteredOk = 0, tried = 0, blocked = 0;
        int sideCount = 0;
        for (auto& hh : W.houses) sideCount += hh.sideStair;
        for (int k = 0; k < n && tried < 12; k += std::max(1, n / 12)) {
            const House& h = W.houses[k];
            if (h.sideStair) continue;
            tried++;
            // roll from the sidewalk toward the door
            Vector2 d = Vector2Normalize({h.doorIn.x - h.doorOut.x, h.doorIn.y - h.doorOut.y});
            Sim s;
            Vector2 start = {h.doorOut.x - d.x * 4.f, h.doorOut.y - d.y * 4.f};
            s.spawn(start.x, start.y, atan2f(d.y, d.x), 5.5f);
            bool inside = false;
            for (int f = 0; f < 60 * 8 && !inside; f++) { // a person pushes only when slow: full speed into a jamb would bail
                Input in; in.push = s.p.speed < 5.5f;
                s.p.update(1.f / 60, in, W);
                if (W.houseAt(s.p.pos.x, s.p.pos.z, s.p.pos.y) >= 0) inside = true;
            }
            if (inside && s.p.state != Player::BAIL) enteredOk++;
            else { blocked++; if (false) printf("   blocked house %d facing %d: end %.1f,%.1f y=%.2f (floor %.2f) state=%d door %.1f,%.1f [%s]\n", k, h.facing, s.p.pos.x, s.p.pos.z, s.p.pos.y, h.floorY, s.p.state, h.doorOut.x, h.doorOut.y, s.p.subPopup.c_str()); }
        }
        int blockedDoors = 0;
        for (auto& h : W.houses) { // is the way from the sidewalk to the door free of trees, lamps and parked cars?
            Vector2 dd = Vector2Normalize({h.doorIn.x - h.doorOut.x, h.doorIn.y - h.doorOut.y});
            bool bad = false;
            for (float t = -3.f; t <= 2.5f && !bad; t += 0.4f) {
                Vector3 p = {h.doorOut.x + dd.x * t, 0, h.doorOut.y + dd.y * t};
                p.y = W.ground(p.x, p.z, h.floorY + (t > 0 ? 0.f : -0.5f));
                Vector3 q = p;
                if (W.collide(q, 0.4f, p.y, 1.7f, nullptr) && Vector3Distance(p, q) > 0.05f) {
                    for (auto& pl : W.plats) if (pl.contains(p.x, p.z, 0.5f) && (pl.kind == PK_POLE || pl.kind == PK_CAR) && pl.top(p.x, p.z) > p.y + 0.5f) { bad = true; break; }
                }
            }
            if (bad) blockedDoors++;
        }
        for (int k : {100, 2000, 4000}) { const House& q = W.houses[k]; printf("   house %d: x %.1f..%.1f z %.1f..%.1f floor %.2f facing %d door %.1f,%.1f\n", k, q.x0, q.x1, q.z0, q.z1, q.floorY, q.facing, q.doorOut.x, q.doorOut.y); }
        {   // what is in the way, by platform kind (diagnostic)
            int byKind[8] = {0};
            for (auto& h : W.houses) {
                Vector2 dd = Vector2Normalize({h.doorIn.x - h.doorOut.x, h.doorIn.y - h.doorOut.y});
                for (float t = -3.f; t <= 2.5f; t += 0.4f) {
                    Vector3 p = {h.doorOut.x + dd.x * t, 0, h.doorOut.y + dd.y * t};
                    p.y = W.ground(p.x, p.z, h.floorY + (t > 0 ? 0.f : -0.5f));
                    for (auto& pl : W.plats)
                        if (pl.contains(p.x, p.z, 0.4f) && pl.top(p.x, p.z) > p.y + 0.5f && pl.base < p.y + 1.7f && (pl.kind == PK_POLE || pl.kind == PK_CAR)) { byKind[pl.kind]++; break; }
                }
            }
            printf("   door blockers by kind: deck %d railing %d prop %d pole %d car %d pier %d\n", byKind[PK_DECK], byKind[PK_RAILING], byKind[PK_PROP], byKind[PK_POLE], byKind[PK_CAR], byKind[PK_PIER]);
        }
        printf("   %d street fronts: %d enterable, %d on ground too steep, %d with a step too high\n", W.rowhouseCount, n, W.houseTooSteep, W.houseTooHigh);
        { extern int w_stat[4]; printf("   closed fronts that could hold a room: no room beside the door %d, rise over 6.5 m %d, narrow facade with a high door %d, stair too steep %d\n", w_stat[0], w_stat[1], w_stat[2], w_stat[3]); }
        printf("   %d of %d front doors have something in the way\n", blockedDoors, n);
        {   // the tallest front stairs must still be climbable from the street with a bit of push
            std::vector<std::pair<float, int>> byRise;
            for (int k = 0; k < n; k++) {
                const House& hs = W.houses[k];
                if (hs.sideStair) continue;
                byRise.push_back({hs.floorY - W.terrain(hs.doorOut.x, hs.doorOut.y), k});
            }
            std::sort(byRise.rbegin(), byRise.rend());
            int ok = 0, tot = 0;
            for (int i = 0; i < 400 && tot < 10; i += 40) {
                const House& hs = W.houses[byRise[i].second];
                Vector2 dd = Vector2Normalize({hs.doorIn.x - hs.doorOut.x, hs.doorIn.y - hs.doorOut.y});
                Sim s;
                s.spawn(hs.doorOut.x - dd.x * 6.f, hs.doorOut.y - dd.y * 6.f, atan2f(dd.y, dd.x), 6.f);
                bool inside = false;
                for (int f = 0; f < 60 * 8 && !inside; f++) { // push only when slow, as a person would
                    Input in; in.push = s.p.speed < 6.5f;
                    s.p.update(1.f / 60, in, W);
                    if (W.houseAt(s.p.pos.x, s.p.pos.z, s.p.pos.y) >= 0) inside = true;
                }
                tot++;
                if (inside) ok++;
            }
            snprintf(b, sizeof b, "tallest stoops: rode %d of %d up to the door (rise of the top ones %.1f m)", ok, tot, byRise[0].first);
            check(ok >= tot - 2, "tall stoops climbable", b);
        }
        {   // houses high above steep streets: the flight of stairs along the front must be ridable up to the door
            int ok = 0, tot = 0;
            std::vector<int> idx;
            for (int k = 0; k < n; k++) if (W.houses[k].sideStair) idx.push_back(k);
            for (int q = 0; q < (int)idx.size() && tot < 12; q += std::max(1, (int)idx.size() / 12)) {
                const House& hs = W.houses[idx[q]];
                Sim s;
                s.spawn(hs.route[0].x, hs.route[0].y, atan2f(hs.route[1].y - hs.route[0].y, hs.route[1].x - hs.route[0].x), 3.f);
                tot++;
                if (rideRoute(s, hs, true)) ok++;
            }
            snprintf(b, sizeof b, "%d houses with a front stair along the facade; rode up %d of %d", (int)idx.size(), ok, tot);
            check(idx.size() > 500 && ok >= tot - 2, "side stairs climbable", b);
        }
        // jumping indoors must not pop you through the ceiling or the walls
        const House& h = W.houses[0];
        Sim s; s.p.spawn({(h.x0 + h.x1) * 0.5f, h.floorY, (h.z0 + h.z1) * 0.5f}, 0);
        float y0 = s.p.pos.y, maxY = y0;
        Input j; j.ollie = true; s.run(1, j);
        s.run(90, Input{}, [&](Player& p) { maxY = std::max(maxY, p.pos.y); return false; });
        bool stayed = W.houseAt(s.p.pos.x, s.p.pos.z, s.p.pos.y) >= 0 && maxY - y0 < 2.0f;
        snprintf(b, sizeof b, "%d enterable houses, rolled in %d of %d, ollie inside peak %.2f m stayed=%d", n, enteredOk, tried, maxY - y0, stayed);
        check(n > 300 && enteredOk >= tried - 1 && stayed, "walk into houses", b);
    }
    {   // police chase: cops show up, stand still and you get busted, outrun them and you escape
        Chase c; Player p;
        Vector3 start = W.spots[0].pos;
        p.spawn(start, 0);
        c.calmT = 0;
        c.update(1.f / 60, p, W);
        bool started = c.active && c.stars == 1 && !c.cops.empty();
        bool busted = false;
        for (int i = 0; i < 60 * 40 && !busted; i++) { // skater stands still on the spot
            c.update(1.f / 60, p, W);
            busted = c.evBusted;
        }
        snprintf(b, sizeof b, "started=%d busted=%d busts=%d", started, busted, c.busts);
        check(started && busted && !c.active, "Cops catch a stationary skater", b);

        // an officer grabs you; mashing breaks you free and the chase goes on
        Chase g; Player r;
        r.spawn(start, 0);
        g.calmT = 0;
        g.update(1.f / 60, r, W);
        g.cops.clear();
        Cop k; k.onFoot = true; k.pos = {r.pos.x + 1.4f, r.pos.y, r.pos.z}; k.yaw = PI;
        g.cops.push_back(k);
        g.update(1.f / 60, r, W);
        bool grabbed = g.scuffle;
        for (int i = 0; i < 240 && g.scuffle; i++) {
            if (i % 6 == 0) g.strike += 0.1f;
            g.update(1.f / 60, r, W);
        }
        bool freed = !g.scuffle && g.shoves == 1 && g.active && g.cops[0].stun > 0 && r.speed > 5.f && g.grace > 0 && r.state == Player::RIDE;
        snprintf(b, sizeof b, "grabbed=%d freed=%d shoves=%d speed=%.1f", grabbed, freed, g.shoves, r.speed);
        check(grabbed && freed, "Fighting an officer off", b);

        // skating into an officer flattens them
        Chase h; Player t;
        t.spawn(start, 0);
        h.calmT = 0;
        h.update(1.f / 60, t, W);
        h.cops.clear();
        Cop k2; k2.onFoot = true; k2.pos = {t.pos.x + 7.f, t.pos.y, t.pos.z}; k2.yaw = 0;
        h.cops.push_back(k2);
        t.speed = 13.f;
        long long sc0 = t.score;
        bool bowled = false;
        for (int i = 0; i < 90 && !bowled; i++) {
            h.cops[0].pos = {h.cops[0].pos.x, t.pos.y, h.cops[0].pos.z};
            Input in; in.push = false;
            t.update(1.f / 60, in, W);
            h.update(1.f / 60, t, W);
            bowled = h.evBowled;
        }
        snprintf(b, sizeof b, "bowled=%d stun=%.1f +%lld pts speed=%.1f", bowled, h.cops[0].stun, t.score - sc0, t.speed);
        check(bowled && h.cops[0].stun > 3.f && !h.scuffle, "Skating into a cop bowls them over", b);

        // an officer follows you through the front door
        {
            int pick = -1;
            for (int i = 0; i < (int)W.houses.size() && pick < 0; i += 7) {
                const House& hs = W.houses[i];
                Vector2 dd = Vector2Normalize({hs.doorIn.x - hs.doorOut.x, hs.doorIn.y - hs.doorOut.y});
                bool clear = true;
                for (float u = -16.f; u <= 3.f && clear; u += 0.5f) {
                    Vector3 p = {hs.doorOut.x + dd.x * u, 0, hs.doorOut.y + dd.y * u};
                    p.y = W.ground(p.x, p.z, hs.floorY - 0.4f);
                    if (u < 0 && fabsf(p.y - hs.floorY) > 4.f) clear = false;
                    Vector3 q = p;
                    if (W.collide(q, 0.45f, p.y, 1.8f, nullptr) && Vector3Distance(p, q) > 0.02f) clear = false;
                }
                if (clear) pick = i;
            }
            bool followed = false;
            if (pick >= 0) {
                const House& hs = W.houses[pick];
                Vector2 dd = Vector2Normalize({hs.doorOut.x - hs.doorIn.x, hs.doorOut.y - hs.doorIn.y});
                Chase d; Player u;
                u.spawn({(hs.x0 + hs.x1) * 0.5f, hs.floorY, (hs.z0 + hs.z1) * 0.5f}, 0);
                d.calmT = 0;
                d.update(1.f / 60, u, W);
                d.cops.clear();
                Cop k3; k3.onFoot = true; k3.pos = {hs.doorOut.x + dd.x * 14.f, 0, hs.doorOut.y + dd.y * 14.f};
                k3.pos.y = W.ground(k3.pos.x, k3.pos.z, hs.floorY);
                d.cops.push_back(k3);
                for (int i = 0; i < 60 * 25 && !followed; i++) {
                    u.pos = u.pos; // stands still inside
                    d.update(1.f / 60, u, W);
                    if (!d.cops.empty() && W.houseAt(d.cops[0].pos.x, d.cops[0].pos.z, d.cops[0].pos.y) >= 0) followed = true;
                    if (!d.active) break;
                }
            }
            snprintf(b, sizeof b, "house %d, officer came inside=%d", pick, followed);
            check(pick >= 0 && followed, "Officers follow you into a house", b);
        }

        Chase e; Player q;
        q.spawn(start, 0);
        e.calmT = 0;
        e.update(1.f / 60, q, W);
        for (auto& cp : e.cops) cp.pos = {q.pos.x + 600, q.pos.y, q.pos.z}; // far away, in the bay
        long long before = q.score;
        for (int i = 0; i < 60 * 8 && !e.evEscape; i++) {
            for (auto& cp : e.cops) { cp.pos = {q.pos.x + 600, q.pos.y, q.pos.z}; cp.speed = 0; }
            e.update(1.f / 60, q, W);
        }
        snprintf(b, sizeof b, "escapes=%d, +%lld pts", e.escapes, q.score - before);
        check(e.evEscape && e.escapes == 1 && q.score >= before + 500, "Losing the cops pays off", b);
    }
    {   // robustness: random skating with the police on you, from several spots, never produces a broken state
        RNG fz(4242);
        int bad = 0, steps = 0, scuffles = 0, inHouse = 0;
        const Vector3 starts[] = {W.spots[0].pos, W.spots[3].pos, W.spots[4].pos, W.spots[7].pos, W.houses[300].doorOut.x ? Vector3{W.houses[300].doorOut.x, W.houses[300].floorY, W.houses[300].doorOut.y} : W.spots[1].pos};
        for (const Vector3& st : starts) {
            Chase c; Player p;
            p.spawn(st, fz.range(0, TAU_F));
            c.calmT = 0;
            Input in;
            for (int f = 0; f < 60 * 45; f++) {
                if (f % 20 == 0) { in = Input{}; in.steer = fz.range(-1, 1); in.push = fz.chance(0.8f); in.tuck = fz.chance(0.2f); in.brake = fz.chance(0.1f); }
                in.ollie = fz.chance(0.01f); in.kickflip = fz.chance(0.01f); in.shove = fz.chance(0.005f);
                if (c.scuffle) { in = Input{}; if (f % 7 == 0) c.strike += 0.1f; }
                p.update(1.f / 60, in, W);
                c.update(1.f / 60, p, W);
                steps++;
                if (c.scuffle) scuffles++;
                if (W.houseAt(p.pos.x, p.pos.z, p.pos.y) >= 0) inHouse++;
                bool ok = std::isfinite(p.pos.x + p.pos.y + p.pos.z) && p.pos.y > -30 && p.pos.y < 3000;
                for (auto& k : c.cops) ok = ok && std::isfinite(k.pos.x + k.pos.y + k.pos.z) && k.pos.y > -30 && k.pos.y < 3000;
                if (!ok) { bad++; break; }
            }
        }
        snprintf(b, sizeof b, "%d frames, %d bad states, %d frames in a fight, %d frames indoors", steps, bad, scuffles, inHouse);
        check(bad == 0, "Chase fuzz", b);
    }
    {   // balance check: a bot that flees the cops and fights when grabbed (strong, steady mashing)
        RNG br(99);
        int escapes = 0, busts = 0, runs = 0, grabs = 0, shoves = 0, maxStars = 0;
        float chaseSecs = 0;
        for (int run = 0; run < 24; run++) {
            const Spot& sp = W.spots[run % 9];
            Chase c; Player p;
            p.spawn(sp.pos, br.range(0, TAU_F));
            c.calmT = 0;
            runs++;
            bool grabbedBefore = false;
            for (int f = 0; f < 60 * 150; f++) {
                Input in;
                if (c.scuffle) { if (f % 6 == 0) c.strike += 0.1f; in = Input{}; }
                else if (c.active && !c.cops.empty()) {
                    // run directly away from the nearest cop, tucking, and slide along walls by turning toward open ground
                    const Cop* nc = &c.cops[0];
                    for (auto& k : c.cops) if (Vector3Distance(k.pos, p.pos) < Vector3Distance(nc->pos, p.pos)) nc = &k;
                    float away = atan2f(p.pos.z - nc->pos.z, p.pos.x - nc->pos.x);
                    // look ahead for open ground: take the free heading nearest to "away"
                    float best = away;
                    for (float off : {0.f, 0.4f, -0.4f, 0.8f, -0.8f, 1.2f, -1.2f, 1.6f, -1.6f, 2.4f}) {
                        float hd = away + off;
                        bool free = true;
                        for (float dist : {3.f, 6.f, 10.f, 14.f}) {
                            Vector3 q = {p.pos.x + cosf(hd) * dist, p.pos.y, p.pos.z + sinf(hd) * dist};
                            Vector3 q2 = q;
                            q.y = W.ground(q.x, q.z, p.pos.y);
                            if (!W.inPlay(q.x, q.z) || W.isWater(q.x, q.z, q.y) || fabsf(q.y - p.pos.y) > 1.5f) { free = false; break; }
                            q2 = q;
                            if (W.collide(q2, 0.7f, q.y, 1.7f, nullptr)) { free = false; break; }
                        }
                        if (free) { best = hd; break; }
                    }
                    float d = wrapPi(best - p.yaw);
                    in.steer = clampf(d * 2.5f, -1.f, 1.f);
                    in.push = p.speed < 16.f;
                    in.tuck = fabsf(d) < 0.25f;
                }
                else { in.push = true; in.steer = sinf(f * 0.01f); }
                p.update(1.f / 60, in, W);
                c.update(1.f / 60, p, W);
                maxStars = std::max(maxStars, c.stars);
                if (c.evGrab) { grabs++; c.evGrab = false; }
                if (c.evShove) { shoves++; c.evShove = false; }
                if (c.evEscape) { escapes++; c.evEscape = false; chaseSecs += c.chaseT; break; }
                if (c.evBusted) { busts++; c.evBusted = false; break; }
                (void)grabbedBefore;
            }
        }
        snprintf(b, sizeof b, "%d runs: %d escaped, %d busted, %d unresolved; %d grabs, %d shoved free; max stars %d", runs, escapes, busts, runs - escapes - busts, grabs, shoves, maxStars);
        check(escapes > 0 && busts > 0 && escapes + busts >= runs / 2, "Chase is winnable and losable (bot)", b);
    }
    {   // a person mashing at a human pace (about 5 presses a second) should win a fight against a 3 star officer, and hard
        // difficulty should hold on tighter than easy
        int wins[3] = {0, 0, 0};
        float secs[3] = {0, 0, 0};
        for (int d = 0; d < 3; d++) {
            Chase g; Player r;
            g.difficulty = d;
            r.spawn(W.spots[0].pos, 0);
            g.calmT = 0;
            g.update(1.f / 60, r, W);
            g.cops.clear();
            g.stars = 3;
            Cop k; k.onFoot = true; k.pos = {r.pos.x + 1.4f, r.pos.y, r.pos.z}; k.yaw = PI;
            g.cops.push_back(k);
            int f = 0;
            for (; f < 60 * 20; f++) {
                if (f % 12 == 0) g.strike += 0.085f;
                g.update(1.f / 60, r, W);
                if (!g.scuffle && f > 2) break;
            }
            wins[d] = g.shoves;
            secs[d] = f / 60.f;
        }
        snprintf(b, sizeof b, "3 stars, 5 presses/s: easy %s %.1fs, normal %s %.1fs, hard %s %.1fs", wins[0] ? "won" : "busted", secs[0], wins[1] ? "won" : "busted", secs[1], wins[2] ? "won" : "busted", secs[2]);
        check(wins[0] && wins[1] && secs[0] <= secs[1] && secs[1] <= secs[2] + 0.01f, "Fight is winnable by mashing, harder on hard", b);
    }
    printf("\n%s (%d failed)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
    return fails ? 1 : 0;
}
