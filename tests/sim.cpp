// Headless gameplay checks: drives the skater with scripted input and checks the outcome.
#include "../src/world.h"
#include "../src/world_internal.h"
#include "../src/player.h"
#include "../src/chase.h"
#include "../src/traffic.h"
#include "../src/models.h"
#include "../src/seaplane.h"
#include "../src/jetski.h"
#include "../src/gondola.h"
#include "../src/jetski.h"
#include <cstdio>
#include <cstring>
#include <string>
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
    {   // a short tap of I: the grab pose lets go with the key, but the Indy Grab still scores once
        Sim s; s.spawn(1045 + 2.4f, -80, PI / 2, 8);
        Input in; in.ollie = true;
        s.run(1, in);
        Input g; g.grab = true;
        int held = 0, stuck = 0, f = 0;
        s.run(6, g, [&](Player& p) { held += p.grabbing; return p.state != Player::AIR; });
        int indy = 0;
        for (auto& t : s.p.combo) indy += t == "Indy Grab";
        s.run(120, Input{}, [&](Player& p) { if (p.state == Player::AIR && p.grabbing) stuck++; f++; return p.state != Player::AIR; });
        snprintf(b, sizeof b, "pose on %d/6 held frames, %d of %d frames after release; Indy Grab x%d", held, stuck, f, indy);
        check(held == 6 && stuck == 0 && f > 10 && indy == 1, "grab pose follows I", b);
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
    {   // the bounce is part of riding: Space pops, steering and flip keys can't bail a clean landing (issue #11)
        // spot: where to ride from; ks: frames after landing to give the input
        auto bounceInput = [&](float x, float z, float yaw, float spd, int warm, std::initializer_list<int> ks, int& popped, int& bails, int& tries) {
            for (int mode = 0; mode < 3; mode++)
                for (int k : ks) {
                    Sim s; s.spawn(x, z, yaw, spd);
                    s.run(warm, Input{});
                    Input in; in.ollie = true; s.run(1, in);
                    s.run(200, Input{}, [](Player& p) { return p.evLand; });
                    s.run(k - 1, Input{});
                    Input b2;
                    if (mode == 0) b2.ollie = true; else if (mode == 1) b2.steer = 1; else b2.kickflip = true;
                    float peak = 0;
                    bool bailed = s.run(60, b2, [&](Player& p) {
                        peak = std::max(peak, p.pos.y - W.ground(p.pos.x, p.pos.z, p.pos.y + 0.2f));
                        return p.state == Player::BAIL;
                    });
                    tries++;
                    if (bailed) bails++;
                    if (mode == 0 && peak > 1.0f) popped++;
                }
        };
        int popped = 0, bails = 0, tries = 0;
        bounceInput(1045 + 2.4f, -80, PI / 2, 8, 0, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, popped, bails, tries);
        snprintf(b, sizeof b, "flat: Space popped %d of %d, %d bails in %d tries", popped, tries / 3, bails, tries);
        check(popped == tries / 3 && bails == 0, "input during landing bounce", b);
        // downhill (12-30% straight roads): the ground falls away under the hop, so it lasts longer
        int spots = 0; popped = bails = tries = 0;
        for (float x = -3000; x < 3000 && spots < 12; x += 37) for (float z = -3000; z < 3000 && spots < 12; z += 41) {
            if (!W.inPlay(x, z)) continue;
            float g = W.ground(x, z, 500); if (g < 2) continue;
            float gx = (W.ground(x + 1, z, g + 0.5f) - W.ground(x - 1, z, g + 0.5f)) / 2, gz = (W.ground(x, z + 1, g + 0.5f) - W.ground(x, z - 1, g + 0.5f)) / 2;
            float sl = sqrtf(gx * gx + gz * gz); if (sl < 0.12f || sl > 0.3f) continue;
            float yaw = atan2f(-gz, -gx);
            bool ok = true;
            for (int i = 1; i <= 25 && ok; i++) {
                float px = x + cosf(yaw) * i, pz = z + sinf(yaw) * i, gg = W.ground(px, pz, g + 1);
                Vector3 q{px, gg, pz}; Vector2 nn;
                if (fabsf((g - gg) / i - sl) > 0.05f || W.collide(q, 0.5f, gg, 1.7f, &nn)) ok = false;
            }
            if (!ok) continue;
            spots++;
            bounceInput(x, z, yaw, 10, 20, {1, 5, 10, 16}, popped, bails, tries);
        }
        snprintf(b, sizeof b, "%d hills: Space popped %d of %d, %d bails in %d tries", spots, popped, tries / 3, bails, tries);
        check(spots >= 8 && popped == tries / 3 && bails == 0, "input during downhill landing bounce", b);
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
    {   // the board flies off the way the skater travels: along the heading, not always east (issue #20),
        // and backwards when rolling backwards (issue #72)
        float worst = 0, worstPos = 0, worstRider = 0;
        for (float spd : {0.f, 8.f, -1.f, -9.f})
            for (int d = 0; d < 4; d++) {
                float yaw = d * PI / 2; // E, S, W, N
                float travel = spd < 0 ? yaw + PI : yaw;
                Sim s; s.spawn(1045 + 2.4f, -80, yaw, spd);
                s.p.bail("test");
                Vector3 bv = s.p.boardVel;
                worst = std::max(worst, fabsf(wrapPi(atan2f(bv.z, bv.x) - travel)));
                Vector3 b0 = s.p.boardPos, r0 = s.p.pos;
                s.run(15, Input{});
                Vector3 db = Vector3Subtract(s.p.boardPos, b0), dr = Vector3Subtract(s.p.pos, r0);
                worstPos = std::max(worstPos, fabsf(wrapPi(atan2f(db.z, db.x) - travel)));
                if (spd != 0) worstRider = std::max(worstRider, fabsf(wrapPi(atan2f(dr.z, dr.x) - travel)));
            }
        {   // boardslide: the heading is across the rail, but board and rider fall along the rail travel
            Sim s; s.spawn(1045 + 2.4f, -80, PI / 2, 6);
            s.p.state = Player::GRIND; s.p.boardslide = true; s.p.railTravel = {1, 0};
            s.p.bail("test");
            Vector3 bv = s.p.boardVel;
            worst = std::max(worst, fabsf(wrapPi(atan2f(bv.z, bv.x))));
        }
        snprintf(b, sizeof b, "worst travel error: board velocity %.1f deg, board path %.1f deg, rider path %.1f deg",
                 worst * RAD2DEG, worstPos * RAD2DEG, worstRider * RAD2DEG);
        check(worst < 0.02f && worstPos < 0.02f && worstRider < 0.02f, "bail throws board along travel", b);
    }
    {   // slamming a building wall: the tumbling board bounces off it instead of flying inside (issue #41)
        int slams = 0, inside = 0; float deepest = 0; Vector3 worstAt = {};
        for (float x = 120; x <= 480 && slams < 24; x += 45)
            for (float z = 300; z <= 660 && slams < 24; z += 45)
                for (int d = 0; d < 4; d++) {
                    float yaw = d * PI / 2;
                    Sim s; s.spawn(x, z, yaw, 12);
                    s.run(240, Input{}, [](Player& p) { return p.state != Player::RIDE; });
                    if (s.p.state != Player::BAIL || s.p.subPopup != "Slammed into a wall") continue;
                    slams++;
                    bool in = false;
                    s.run(100, Input{}, [&](Player& p) {
                        Vector3 c = p.boardPos; Vector2 n;
                        if (W.collide(c, 0.01f, p.boardPos.y, 0.3f, &n)) {
                            in = true;
                            float dd = Vector2Length({c.x - p.boardPos.x, c.z - p.boardPos.z});
                            if (dd > deepest) { deepest = dd; worstAt = p.boardPos; }
                        }
                        return p.state != Player::BAIL;
                    });
                    inside += in;
                }
        snprintf(b, sizeof b, "%d of %d slams left the board in a wall, deepest %.2f m at (%.1f, %.1f, %.1f)",
                 inside, slams, deepest, worstAt.x, worstAt.y, worstAt.z);
        check(slams >= 8 && inside == 0, "bail board stays out of walls", b);
    }
    {   // 360 spin
        Sim s; s.spawn(1045 + 2.4f, -80, PI / 2, 8);
        Input in; in.ollie = true; s.run(1, in);
        Input sp; sp.steer = 1; s.run(63, sp, [](Player& p) { return fabsf(p.spinAccum) > 2 * PI; });
        s.run(120, Input{}, [](Player& p) { return p.state != Player::AIR; });
        snprintf(b, sizeof b, "state=%d score=%lld", s.p.state, s.p.score);
        check(s.p.state == Player::RIDE && s.p.score > 100, "FS 360 lands", b);
    }
    {   // brushing the Embarcadero Plaza lamp post mid-ollie turns the board with the deflection (issue #3)
        int ok = 0, deflected = 0; float worst = 0; bool spinTrick = false;
        for (float side : {-0.3f, 0.3f}) {
            float yaw = 0.77f, x = 1185 - cosf(yaw) * 4 - sinf(yaw) * side, z = -222.7f - sinf(yaw) * 4 + cosf(yaw) * side;
            Sim s; s.spawn(x, z, yaw, 10);
            Input in; in.ollie = true; s.run(1, in);
            bool hit = false; float off = 0;
            s.run(120, Input{}, [&](Player& p) {
                if (p.state == Player::AIR && p.hspeed() < 9) hit = true;
                if (p.state == Player::AIR) off = fabsf(wrapPi(p.yaw - atan2f(p.vel.z, p.vel.x)));
                return p.state != Player::AIR;
            });
            for (auto& t : s.p.combo) if (t.rfind("FS ", 0) == 0 || t.rfind("BS ", 0) == 0) spinTrick = true;
            worst = std::max(worst, off);
            deflected += hit;
            ok += hit && s.p.state == Player::RIDE && s.p.score == 100; // lands, but a clipped landing is no stomp
        }
        snprintf(b, sizeof b, "deflected %d of 2, landed clean %d, board off line %.0f deg, spin trick=%d", deflected, ok, worst * RAD2DEG, spinTrick);
        check(deflected == 2 && ok == 2 && worst < 0.05f && !spinTrick, "mid-air pole clip lands", b);
    }
    {   // rolling fakie off the Embarcadero Plaza deck, or ollieing fakie, keeps the board facing the same way (issue #46)
        auto fakie = [&](float x, float z, float yaw, bool pop, const char* name, long long okLo, long long okHi, float spd = -4) {
            Sim s; s.spawn(x, z, yaw, spd);
            float jump = 0, prev = s.p.yaw;
            bool air = false;
            if (pop) { Input in; in.ollie = true; s.run(1, in); air = s.p.state == Player::AIR; jump = fabsf(wrapPi(s.p.yaw - prev)); prev = s.p.yaw; }
            s.run(150, Input{}, [&](Player& p) {   // takeoff, landing and bounce, then rolling on
                air |= p.state == Player::AIR;
                jump = std::max(jump, fabsf(wrapPi(p.yaw - prev)));
                prev = p.yaw;
                return false;
            });
            snprintf(b, sizeof b, "air=%d max yaw step %.2f rad, state=%d speed=%.1f score=%lld %s", air, jump, s.p.state, s.p.speed, s.p.score, s.p.subPopup.c_str());
            check(air && jump < 0.05f && s.p.state == Player::RIDE && s.p.speed < 0 && s.p.score >= okLo && s.p.score <= okHi, name, b);
        };
        fakie(1173, -300, 0, false, "roll off a ledge fakie: no snap", 0, 0);
        fakie(1045 + 2.4f, -80, PI / 2, true, "fakie ollie: no snap, no spin", 100, 150);
        fakie(1045 + 2.4f, -80, PI / 2, true, "slow fakie ollie stays fakie", 100, 150, -0.8f);
    }
    {   // ...a 180 from fakie lands forwards, and it's the 180 that scores, not "to Fakie"
        Sim s; s.spawn(1045 + 2.4f, -80, PI / 2, -8);
        Input in; in.ollie = true; s.run(1, in);
        Input sp; sp.steer = 1; s.run(63, sp, [](Player& p) { return fabsf(p.spinAccum) >= PI; });
        s.run(120, Input{}, [](Player& p) { return p.state == Player::RIDE; });
        snprintf(b, sizeof b, "state=%d speed=%.1f banked \"%s\"", s.p.state, s.p.speed, s.p.subPopup.c_str());
        check(s.p.state == Player::RIDE && s.p.speed > 0 && s.p.subPopup.find("180") != std::string::npos &&
              s.p.subPopup.find("Fakie") == std::string::npos, "fakie 180 lands forwards", b);
    }
    {   // ...and a forwards 180 lands fakie: keeps the yaw it landed with and rolls on backwards, no snap round (issue #92)
        Sim s; s.spawn(1045 + 2.4f, -80, PI / 2, 8);
        Input in; in.ollie = true; s.run(1, in);
        Input sp; sp.steer = 1; s.run(63, sp, [](Player& p) { return fabsf(p.spinAccum) >= PI; });
        float jump = 0, prev = s.p.yaw;
        bool landed = false;
        s.run(150, Input{}, [&](Player& p) {   // touchdown, landing bounce, then rolling on
            jump = std::max(jump, fabsf(wrapPi(p.yaw - prev)));
            prev = p.yaw;
            landed |= p.evLand;
            return false;
        });
        snprintf(b, sizeof b, "landed=%d max yaw step %.2f rad, state=%d speed=%.1f banked \"%s\"", landed, jump, s.p.state, s.p.speed, s.p.subPopup.c_str());
        check(landed && jump < 0.05f && s.p.state == Player::RIDE && s.p.speed < -4.f &&
              s.p.subPopup.find("FS 180 to Fakie") != std::string::npos, "forwards 180 rolls away fakie", b);
    }
    {   // ...and ollieing fakie onto the Embarcadero Plaza Flat Bar grinds fakie, then rolls away fakie
        int ok = 0, tries = 6; float worst = 0; char last[200] = "";
        for (int t = 0; t < tries; t++) {
            SetRandomSeed(4600 + t);
            Sim s; s.spawn(1186, -238, PI, -7);
            s.run(60, Input{}, [](Player& p) { return p.pos.x > 1193.5f; });
            float prev = s.p.yaw, jump = 0;
            bool grind = false, settled = false;
            std::string tricks;
            auto track = [&](Player& p) { jump = std::max(jump, fabsf(wrapPi(p.yaw - prev))); prev = p.yaw; };
            Input in; in.ollie = true; s.run(1, in, [&](Player& p) { track(p); return false; });
            for (int f = 0; f < 600 && !settled && s.p.state != Player::BAIL; f++) {
                Input bal; // follow the HUD meter, as in the grind balance check
                if (s.p.state == Player::GRIND) bal.steer = s.p.balance > 0.1f ? -1.f : s.p.balance < -0.1f ? 1.f : 0.f;
                s.run(1, bal, [&](Player& p) { track(p); return false; });
                if (s.p.state == Player::GRIND) { grind = true; tricks = s.p.combo.back(); }
                settled = grind && s.p.state == Player::RIDE && !s.p.bouncing;
            }
            s.run(10, Input{}, [&](Player& p) { track(p); return false; });
            worst = std::max(worst, jump);
            bool good = grind && jump < 0.05f && s.p.state == Player::RIDE && s.p.speed < 0 && tricks.rfind("Fakie 50-50", 0) == 0;
            ok += good;
            if (!good || !last[0]) snprintf(last, sizeof last, " (seed %d: grind=%d \"%s\" yaw step %.2f, state=%d speed=%.1f)", t, grind, tricks.c_str(), jump, s.p.state, s.p.speed);
        }
        snprintf(b, sizeof b, "%d of %d fakie grinds rolled away fakie, worst yaw step %.2f rad%s", ok, tries, worst, last);
        check(ok == tries, "fakie grind: no snap", b);
    }
    {   // ...and losing balance on a fakie 50-50 throws the board and rider on along the rail (+x), not back up it
        SetRandomSeed(4700);
        Sim s; s.spawn(1186, -238, PI, -7);
        s.run(60, Input{}, [](Player& p) { return p.pos.x > 1193.5f; });
        Input in; in.ollie = true; s.run(1, in);
        bool grind = s.run(90, Input{}, [](Player& p) { return p.state == Player::GRIND; });
        Input lean; lean.steer = 1;   // push the balance off the meter
        s.run(240, lean, [](Player& p) { return p.state != Player::GRIND; });
        bool bailed = s.p.state == Player::BAIL;
        Vector3 bv = s.p.boardVel; float x0 = s.p.pos.x;
        s.run(30, Input{});
        snprintf(b, sizeof b, "grind=%d bailed=%d board vel %.1f,%.1f, rider moved %.2f m in x", grind, bailed, bv.x, bv.z, s.p.pos.x - x0);
        check(grind && bailed && bv.x > 1.f && s.p.pos.x - x0 > 0.2f, "fakie grind bail goes along travel", b);
    }
    {   // ...but flying into a building wall at a steep angle is still a crash
        int bailed = 0, tot = 0;
        for (float sp : {8.f, 10.f})
            for (float off : {15.f, 30.f, 45.f}) {
                float yaw = off * DEG2RAD, x = 396 - cosf(yaw) * 2.35f, z = -572 - sinf(yaw) * 2;
                Sim s; s.spawn(x, z, yaw, sp);
                Input in; in.ollie = true; s.run(1, in);
                s.run(120, Input{}, [](Player& p) { return p.state != Player::AIR; });
                tot++;
                bailed += s.p.state == Player::BAIL && s.p.subPopup == "Flew into a wall";
            }
        snprintf(b, sizeof b, "%d of %d steep wall hits at 8-10 m/s bailed", bailed, tot);
        check(bailed == tot, "steep mid-air wall hit bails", b);
    }
    {   // a stomped landing scales the spin and Big Air too, not just the tricks done before it
        auto stomped = [](float spin, float air) {
            Sim s; s.spawn(1045 + 2.4f, -80, PI / 2);
            Player& p = s.p;
            p.state = Player::AIR; p.pos.y += 0.05f; p.vel = Vector3Scale(p.fwd(), 8); p.vy = -6;
            p.combo = {"Ollie"}; p.comboPts = 100; p.spinAccum = spin; p.airTime = air;
            s.run(30, Input{}, [](Player& q) { return q.state != Player::AIR; });
            return p.score;
        };
        long long spun = stomped(PI, 0.5f), big = stomped(0, 1.7f);
        snprintf(b, sizeof b, "Ollie+FS 180 +%lld (want 920), Ollie+Big Air +%lld (want ~1160)", spun, big);
        check(spun >= 915 && spun <= 925 && big >= 1140 && big <= 1180, "stomp bonus covers spin & Big Air", b);
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
    {   // grind balance follows the HUD meter: pressing away from the marker's side keeps you on the rail
        int lost = 0, tries = 12;
        for (int t = 0; t < tries; t++) {
            SetRandomSeed(1300 + t);
            Sim s; s.spawn(1186, -238, 0, 7);
            s.run(60, Input{}, [](Player& p) { return p.pos.x > 1193.5f; });
            Input in; in.ollie = true; s.run(1, in);
            if (!s.run(90, Input{}, [](Player& p) { return p.state == Player::GRIND; })) { lost++; continue; }
            for (int f = 0; f < 600 && s.p.state == Player::GRIND; f++) {
                Input bal; // marker sits at sw/2 + balance * 115; tap the opposite direction
                bal.steer = s.p.balance > 0.1f ? -1.f : s.p.balance < -0.1f ? 1.f : 0.f;
                s.run(1, bal);
            }
            if (s.p.state == Player::BAIL) lost++;
        }
        snprintf(b, sizeof b, "lost balance %d/%d following the meter", lost, tries);
        check(lost == 0, "grind balance matches HUD meter", b);
    }
    {   // coping only on real lips: never a seam where a ramp meets a deck/box at its own height (issue #38)
        int coping = 0, flat = 0; char where[96] = "";
        for (const Rail& r : W.rails) {
            if (strcmp(r.name, "Coping") != 0) continue;
            coping++;
            bool alongZ = fabsf(r.b.z - r.a.z) > fabsf(r.b.x - r.a.x);
            for (float t : {0.25f, 0.5f, 0.75f}) {
                float x = lerpf(r.a.x, r.b.x, t), z = lerpf(r.a.z, r.b.z, t), y = lerpf(r.a.y, r.b.y, t);
                float g0 = alongZ ? W.ground(x - 0.3f, z, y + 0.2f) : W.ground(x, z - 0.3f, y + 0.2f);
                float g1 = alongZ ? W.ground(x + 0.3f, z, y + 0.2f) : W.ground(x, z + 0.3f, y + 0.2f);
                if (std::min(g0, g1) > y - 0.3f) {   // ground at rail height on both sides: mid-surface
                    if (!flat++) snprintf(where, sizeof where, " first at %.1f,%.2f,%.1f (ground %.2f/%.2f)", x, y, z, g0, g1);
                    break;
                }
            }
        }
        snprintf(b, sizeof b, "%d of %d Coping rails lie mid-surface%s", flat, coping, where);
        check(coping > 0 && flat == 0, "coping only along real lips", b);
        // ride along each Embarcadero seam (deck/bank, funbox top/ramps) and ollie: no grind on flat ground
        int snaps = 0; char got[96] = "";
        for (Vector2 sp : {Vector2{1205, -309}, Vector2{1226, -261.5f}, Vector2{1232, -261.5f}}) {
            Sim s; s.spawn(sp.x, sp.y, PI / 2, 6);
            s.run(8, Input{});
            Input in; in.ollie = true; s.run(1, in);
            if (s.run(90, Input{}, [](Player& p) { return p.state == Player::GRIND && strcmp(W.rails[p.rail].name, "Coping") == 0; })) {
                if (!snaps++) snprintf(got, sizeof got, " first at x=%.1f (%.1f,%.2f,%.1f)", sp.x, s.p.pos.x, s.p.pos.y, s.p.pos.z);
            }
        }
        snprintf(b, sizeof b, "%d of 3 Embarcadero seams gave a Coping grind%s", snaps, got);
        check(snaps == 0, "no grind on Embarcadero flat tops", b);
    }
    {   // kicker ramp launches you
        Sim s; s.spawn(1233, -241, 0, 9);
        bool air = s.run(90, Input{}, [](Player& p) { return p.state == Player::AIR; });
        snprintf(b, sizeof b, "air=%d x=%.1f vy=%.1f", air, s.p.pos.x, s.p.vy);
        check(air && s.p.pos.x > 1241, "kicker launches", b);
    }
    {   // rolling off the Marina Green mega kicker (3.4 m, lip at x -468) without an ollie flies up and out,
        // not slammed down by a slope sample read past the lip (#93)
        int ok = 0; char got[160] = "";
        for (float spd : {12.f, 15.f, 20.f}) {
            Sim s; s.spawn(-495, -1046, 0, spd);
            float vy0 = -99, x0 = 0; bool air = false, landed = false;
            s.run(360, Input{}, [&](Player& q) {
                if (!air && q.state == Player::AIR) { air = true; vy0 = q.vy; x0 = q.pos.x; }
                landed |= air && (q.evLand || q.state == Player::BAIL);
                return landed;
            });
            bool good = air && landed && vy0 > 0.5f && x0 > -469 && s.p.pos.x > -464 &&
                        !(s.p.state == Player::BAIL && s.p.subPopup == "Way too much drop");
            ok += good;
            if (!good && !got[0]) snprintf(got, sizeof got, " (%.0f m/s: air=%d vy0 %.1f at x %.1f, ended x %.1f, state %d '%s')",
                                           spd, air, vy0, x0, s.p.pos.x, s.p.state, s.p.subPopup.c_str());
        }
        snprintf(b, sizeof b, "%d of 3 roll-offs launched up and forward%s", ok, got);
        check(ok == 3, "roll off a big kicker launches upward", b);
    }
    {   // at rest half a metre from the Embarcadero deck's drop edge (x 1170), facing it or backed up to it: stays put
        int held = 0; char got[96] = "";
        for (float yaw : {PI, 0.f}) {
            Sim s; s.spawn(1170.5f, -300, yaw, 0);
            float y0 = s.p.pos.y;
            s.run(120, Input{});
            if (s.p.state == Player::RIDE && fabsf(s.p.pos.y - y0) < 0.01f && fabsf(s.p.pos.x - 1170.5f) < 0.05f) held++;
            else if (!got[0]) snprintf(got, sizeof got, " (yaw %.1f: x %.2f, dy %.2f, speed %.2f)", yaw, s.p.pos.x, s.p.pos.y - y0, s.p.speed);
        }
        snprintf(b, sizeof b, "%d of 2 rested at the deck edge%s", held, got);
        check(held == 2, "no pull over a drop edge", b);
    }
    {   // rolling slowly off that edge still drops through the air and lands, not a one-frame teleport down
        Sim s; s.spawn(1171.5f, -300, PI, 1.0f);
        float y0 = s.p.pos.y; bool air = false, landed = false;
        s.run(240, Input{}, [&](Player& q) {
            air |= q.state == Player::AIR;
            landed |= air && q.evLand;
            return landed;
        });
        snprintf(b, sizeof b, "air=%d landed=%d dropped %.2f m", air, landed, y0 - s.p.pos.y);
        check(air && landed && y0 - s.p.pos.y > 1.4f, "slow roll off a drop goes airborne", b);
    }
    {   // ...and rolling slowly off it fakie keeps going away from the edge after landing, not back into the deck (#46)
        int ok = 0; char got[160] = "";
        for (float spd : {-0.6f, -1.0f}) {
            Sim s; s.spawn(1170.4f, -300, 0, spd);
            float y0 = s.p.pos.y; bool air = false;
            s.run(180, Input{}, [&](Player& q) { air |= q.state == Player::AIR; return false; });
            bool good = air && y0 - s.p.pos.y > 1.4f && s.p.state == Player::RIDE && s.p.speed < 0 && s.p.pos.x < 1169.5f;
            ok += good;
            if (!good && !got[0]) snprintf(got, sizeof got, " (%.1f m/s: air=%d dropped %.2f, x %.2f, speed %.2f, state %d)", spd, air, y0 - s.p.pos.y, s.p.pos.x, s.p.speed, s.p.state);
        }
        snprintf(b, sizeof b, "%d of 2 slow fakie roll-offs kept rolling away from the edge%s", ok, got);
        check(ok == 2, "slow fakie roll off a drop", b);
    }
    {   // braking at rest on a steep hill holds without creeping
        Sim s; s.spawn(-282, 878, -0.6f, 0);
        Vector3 p0 = s.p.pos;
        Input in; in.brake = true;
        s.run(180, in);
        float moved = Vector3Distance(p0, s.p.pos);
        snprintf(b, sizeof b, "moved %.3f m in 3 s, speed %.3f", moved, s.p.speed);
        check(moved < 0.01f && s.p.state == Player::RIDE, "brake holds on a hill", b);
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
    {   // ride at the Salesforce and Transamerica towers from 8 directions: the skater must stop at the drawn glass,
        // not metres inside it (#65). Salesforce: 16-gon of radius 23 (apothem 22.6); Transamerica: square base +-18
        // tapering to the apex 170 m up. Measures the closest the skater gets (radius / largest axis offset).
        struct T { const char* name; float cx, cz, lo, hi; bool square; };
        const T towers[] = {{"Salesforce walls at the glass", (gx(22) + gx(23)) / 2, (gz(11) + gz(12)) / 2, 21.5f, 24.0f, false},
                            {"Transamerica walls at the glass", (gx(21) + gx(22)) / 2, (gz(5) + gz(6)) / 2, 17.3f, 18.8f, true}};
        for (const T& t : towers) {
            float worstIn = 1e9f, worstOut = 0;
            for (int k = 0; k < 8; k++) {
                float a = k * PI / 4, d0 = t.lo + 7;
                Sim s; s.spawn(t.cx + cosf(a) * d0, t.cz + sinf(a) * d0, a + PI, 4);
                float closest = 1e9f;
                s.run(300, Input{}, [&](Player& q) {
                    float dx = q.pos.x - t.cx, dz = q.pos.z - t.cz;
                    closest = std::min(closest, t.square ? std::max(fabsf(dx), fabsf(dz)) : sqrtf(dx * dx + dz * dz));
                    return false;
                });
                worstIn = std::min(worstIn, closest);
                worstOut = std::max(worstOut, closest);
            }
            snprintf(b, sizeof b, "closest %.2f..%.2f m from centre (want %.1f..%.1f)", worstIn, worstOut, t.lo, t.hi);
            check(worstIn >= t.lo && worstOut <= t.hi, t.name, b);
        }
    }
    {   // fast riding into thin solids (poles, house side walls) stops on the near side instead of tunnelling through (issue #83)
        int tried = 0, through = 0;
        std::string first;
        for (const Platform& pl : W.plats) {
            if (pl.kind != PK_POLE && pl.kind != PK_BUILDING) continue;
            bool alongX = pl.x1 - pl.x0 < 0.5f;   // thin across x: ride along +x into it, else thin across z: ride along +z
            if (!alongX && pl.z1 - pl.z0 >= 0.5f) continue;
            float cx = (pl.x0 + pl.x1) / 2, cz = (pl.z0 + pl.z1) / 2;
            float sx = alongX ? pl.x0 - 1.5f : cx, sz = alongX ? cz : pl.z0 - 1.5f;
            float ex = alongX ? pl.x1 + 1.5f : cx, ez = alongX ? cz : pl.z1 + 1.5f;
            float g0 = W.ground(sx, sz, 500), g1 = W.ground(ex, ez, 500);
            if (fabsf(g1 - g0) > 0.1f || fabsf(W.terrain(sx, sz) - g0) > 0.1f) continue;   // flat street, nothing to ride up or onto
            float top = pl.top(cx, cz);
            if (top < g0 + 2.5f || pl.base > g0 + 0.5f) continue;   // a solid the skater can't hop or pass under
            Vector3 q = {sx, g0, sz};
            Vector2 n;
            if (W.collide(q, 0.6f, g0, 1.7f, &n)) continue;   // start spot clear
            for (float dt : {1.f / 60, 1.f / 30}) {
                tried++;
                Player p;
                p.spawn({sx, g0, sz}, alongX ? 0 : PI / 2);
                p.speed = 36;
                Input in; in.tuck = true;
                for (int f = 0; f < (int)(0.3f / dt) && p.state != Player::BAIL; f++) {
                    p.update(dt, in, W);
                    if ((alongX ? p.pos.x > pl.x1 : p.pos.z > pl.z1) && p.pos.y < top) {
                        through++;
                        if (first.empty()) { char w[96]; snprintf(w, sizeof w, " (first: %s at %.1f,%.1f, dt 1/%.0f)", pl.kind == PK_POLE ? "pole" : "wall", cx, cz, 1 / dt); first = w; }
                        break;
                    }
                }
            }
            if (tried >= 800) break;
        }
        snprintf(b, sizeof b, "%d of %d runs at 36 m/s passed through%s", through, tried, first.c_str());
        check(tried > 100 && through == 0, "fast rides don't tunnel thin walls", b);
    }
    {   // Oracle Park's grandstand, scoreboard, Coke bottle and glove are solid (issue #67): push across the field into each
        struct Run { const char* name; float x, z, yaw; bool alongX; float wall; int dir; };
        const Run runs[] = {
            {"grandstand", 1215, 775, PI, true, 1195, -1},          // westward into the stand's 2 m front wall
            {"scoreboard", 1284, 725, -PI / 2, false, 708, -1},     // north into the scoreboard
            {"bottle", 1268, 715, 0, true, 1287.5f, 1},             // east into the bottle's plinth
            {"glove", 1268, 735, 0, true, 1287.5f, 1},              // east into the glove's pedestal
        };
        int ok = 0;
        std::string why;
        for (const Run& r : runs) {
            Sim s; s.spawn(r.x, r.z, r.yaw, 2);
            Input in; in.push = true;
            float deepest = -1e9f;   // furthest past the face, in the direction of travel
            s.run(400, in, [&](Player& q) {
                float c = r.alongX ? q.pos.x : q.pos.z;
                deepest = std::max(deepest, (c - r.wall) * r.dir);
                return false;
            });
            if (deepest < 0) ok++;
            else if (why.empty()) { char w[96]; snprintf(w, sizeof w, " (first miss: %s, %.1f m past its face)", r.name, deepest); why = w; }
        }
        snprintf(b, sizeof b, "%d of 4 ballpark structures stopped the skater%s", ok, why.c_str());
        check(ok == 4, "Oracle Park structures are solid", b);
    }
    {   // ride into the Pacific
        Sim s; s.spawn(-1440, 0, PI, 6);
        Input in; in.push = true;
        bool splash = s.run(900, in, [](Player& p) { return p.splashed; });
        s.run(200, Input{});
        snprintf(b, sizeof b, "splash=%d respawned at x=%.1f y=%.1f state=%d", splash, s.p.pos.x, s.p.pos.y, s.p.state);
        check(splash && s.p.pos.y > 0.5f && s.p.state == Player::RIDE, "splash + respawn", b);
    }
    {   // piers name the shore they belong to, not the water under them; Alcatraz's dock end isn't the Wharf (#61)
        struct { float x, z; const char* want; } spots[] = {
            {1025, -1280, "Fisherman's Wharf"},   // Pier 39, where its token is
            {1380, -800, "The Embarcadero"}, {1380, -660, "The Embarcadero"}, {1380, -520, "The Embarcadero"},
            {1380, -380, "The Embarcadero"}, {1380, 50, "The Embarcadero"}, {1380, 180, "The Embarcadero"},
            {1380, 480, "The Embarcadero"},
            {469, -1200, "Fisherman's Wharf"},    // Hyde Street Pier
            {440, -1886, "Alcatraz"},             // the Alcatraz dock
            {423, -1916, "Alcatraz"},             // Building 64
            {1600, -730, "San Francisco Bay"}};   // open water past the pier ends is still the bay
        int bad = 0; b[0] = 0;
        for (auto& sp : spots)
            if (std::string(W.district(sp.x, sp.z)) != sp.want && !bad++)
                snprintf(b, sizeof b, "(%.0f,%.0f) says %s, want %s", sp.x, sp.z, W.district(sp.x, sp.z), sp.want);
        check(bad == 0, "piers and Alcatraz dock have the right district", b);
    }
    {   // after a splash off a gentle shore, the respawn stays put: it doesn't roll back into the water (#90)
        struct { float x, z; } starts[] = {{4007.8f, -2575.6f}, {2825.3f, -713.9f}, {-751.0f, -1073.1f}};
        int bad = 0, tried = 0;
        std::string worst;
        for (auto& st : starts) {
            float best = 1e9f, yaw = 0;   // head for the nearest water
            for (int k = 0; k < 64; k++) {
                float a = k * (2 * PI / 64);
                for (float r = 1; r < 40 && r < best; r += 1)
                    if (W.isWater(st.x + cosf(a) * r, st.z + sinf(a) * r, 0)) { best = r; yaw = a; }
            }
            if (best > 39) continue;
            // from the start itself, and from 4 m short of the water: too close for any spot on the way to be
            // saved, so the respawn is the (unchecked) start spot, on the shore slope facing the water
            for (float back : {best, 4.f}) {
                float sx = st.x + cosf(yaw) * (best - back), sz = st.z + sinf(yaw) * (best - back);
                Sim s; s.spawn(sx, sz, yaw, 3);
                Input in; in.push = true;
                if (!s.run(60 * 20, in, [](Player& p) { return p.splashed; })) continue;
                tried++;
                int again = 0;
                for (int f = 0; f < 60 * 15; f++) {
                    s.p.update(1.f / 60, Input{}, W);
                    if (s.p.evSplash) again++;
                }
                if (again || s.p.state != Player::RIDE) {
                    bad++;
                    char t[96];
                    snprintf(t, sizeof t, " (%.0f,%.0f): %d more splashes", sx, sz, again);
                    worst += t;
                }
            }
        }
        snprintf(b, sizeof b, "%d/%d shores re-splashed%s", bad, tried, worst.c_str());
        check(tried > 0 && bad == 0, "splash respawn stays out of water", b);
    }
    {   // R while riding a jet ski puts you back on land, not on the water where the ski is (issue #69)
        JetSkis js; js.place(W);
        bool found = false;
        Vector3 land{};
        if (!js.skis.empty()) {
            const Vector3 sk = js.skis[0].pos;
            for (float d = 2; d <= 40 && !found; d += 1)
                for (int k = 0; k < 16 && !found; k++) {
                    float a = k * TAU_F / 16, x = sk.x + cosf(a) * d, z = sk.z + sinf(a) * d, h = W.terrain(x, z);
                    if (h < 1.0f || h > 4.f || !W.inPlay(x, z) || W.isWater(x, z, h) || W.solidAt({x, h + 1.2f, z})) continue;
                    bool dry = true;   // clear of water for 6 m around, like a spot the game itself would save
                    for (int j = 0; j < 12 && dry; j++)
                        for (float r = 2; r <= 6 && dry; r += 2) {
                            float qx = x + cosf(j * PI / 6) * r, qz = z + sinf(j * PI / 6) * r;
                            if (W.isWater(qx, qz, W.ground(qx, qz, h + 1.f))) dry = false;
                        }
                    if (dry) { land = {x, W.ground(x, z, h + 2), z}; found = true; }
                }
        }
        Sim s; s.spawn(land.x, land.z, 0);              // on the shore by a moored ski (spawn records it as the safe spot)
        int ski = found ? js.nearest(s.p.pos, 50.f) : -1;
        bool onWater = false;
        if (ski >= 0) {
            js.mount(ski, s.p);
            Input in; in.push = true; in.steer = 0.3f;
            for (int f = 0; f < 60 * 4; f++) js.update(1.f / 60, in, false, s.p, W);
            onWater = js.riding && W.terrain(s.p.pos.x, s.p.pos.z) < 0.05f;
            // what main does on R: leave the ski where it is, then back to the safe spot
            js.skis[js.cur].pos = s.p.pos; js.riding = false; js.cur = -1;
            s.p.respawnSafe(W);
        }
        Vector3 r = s.p.pos;
        float h = W.terrain(r.x, r.z);
        Input hold; hold.brake = true;                   // stand still: a sloped shore mustn't roll you in
        bool splash = s.run(120, hold, [](Player& p) { return p.splashed; });
        snprintf(b, sizeof b, "ski=%d onWater=%d reset to %.0f,%.1f,%.0f terrain=%.1f splash=%d", ski, onWater, r.x, r.y, r.z, h, splash);
        check(ski >= 0 && onWater && h > 0.5f && !W.isWater(r.x, r.z, r.y) && !splash && s.p.state == Player::RIDE, "R on a jet ski resets to land", b);
    }
    {   // the splash message names the water: ocean, bay, fountain, lake, lagoon, pool, salt pond (rolling in and dropping in)
        float sea = -1440;   // the Pacific, west of the city
        while (!W.isWater(sea, 0, 0) && sea > -3000) sea -= 5;
        float bay = 1500;    // the bay, out past the ends of the Embarcadero piers
        while (!W.isWater(bay, -730, 0) && bay < 3000) bay += 5;
        auto pondOf = [](const char* kind) -> const Pond* {
            for (auto& pd : W.ponds) if (std::string(pd.kind) == kind) return &pd;
            return nullptr;
        };
        const Pond* pool = pondOf("pool");   // a peninsula backyard pool, up on the hills
        const Pond* lagoon = pondOf("lagoon");   // by the Palace of Fine Arts
        const Pond* salt = pondOf("salt pond");   // past the Menlo Park bayfront
        auto mx = [](const Pond* pd) { return pd ? (pd->x0 + pd->x1) / 2 : 0.f; };
        auto mz = [](const Pond* pd) { return pd ? (pd->z0 + pd->z1) / 2 : 0.f; };
        check(std::string(W.district(sea - 20, 0)) == "Pacific Ocean" && std::string(W.district(bay + 20, -730)) == "San Francisco Bay",
              "splash test spots are in the ocean and the bay", "");
        struct { float x, z; const char* msg; } spots[] = {
            {sea - 20, 0, "Splash! Into the ocean"}, {bay + 20, -730, "Splash! Into the bay"},
            {1244.5f, -304, "Splash! Into the fountain"}, {-800, 387, "Splash! Into the lake"},
            {mx(lagoon), mz(lagoon), "Splash! Into the lagoon"}, {mx(pool), mz(pool), "Splash! Into the pool"},
            {mx(salt), mz(salt), "Splash! Into the salt pond"}};
        for (auto& sp : spots)
            for (int air = 0; air < 2; air++) {
                Sim s; s.spawn(sp.x, sp.z, 0);
                if (air) { s.p.state = Player::AIR; s.p.pos.y += 2; }
                bool splash = s.run(120, Input{}, [](Player& p) { return p.splashed; });
                snprintf(b, sizeof b, "%s: splash=%d \"%s\"", air ? "drop" : "roll", splash, s.p.subPopup.c_str());
                check(splash && s.p.subPopup == sp.msg, "splash message names the water", b);
            }
    }
    {   // the Golden Gate railing keeps you on the deck
        Sim s; s.spawn(-1100, -1500, 0, 8); // heading east toward the railing
        Input in; in.push = true;
        s.run(300, in);
        snprintf(b, sizeof b, "x=%.2f y=%.1f state=%d", s.p.pos.x, s.p.pos.y, s.p.state);
        check(s.p.pos.x < -1086 && s.p.pos.y > 25, "bridge railing holds", b);
    }
    {   // a jet ski at full throttle stops at piers, bridge bases and the Balclutha instead of driving through (#63)
        struct { const char* name; float x0, z0, x1, z1, sx, sz, yaw; } hits[] = {
            {"Pier 39", 990, -1300, 1060, -1105, 930, -1200, 0},
            {"Golden Gate south tower base", -1130, -1264, -1070, -1236, -1200, -1250, 0},
            {"Embarcadero pier", 1290, 165, 1480, 195, 1570, 180, PI},
            {"Balclutha", 489.5f, -1235, 500.5f, -1165, 570, -1200, PI},
            {"Bay Bridge tower base", 1570, 307, 1590, 363, 1580, 240, PI / 2},
        };
        int ok = 0, tot = 0;
        char det[600] = "";
        for (auto& h : hits) {
            tot++;
            JetSkis js;
            js.skis.push_back({{h.sx, 0, h.sz}, h.yaw, 0});
            Player p;
            js.mount(0, p);
            Input in; in.push = true;
            int inside = 0;
            float closest = 1e9f;
            for (int f = 0; f < 60 * 15; f++) {
                js.update(1.f / 60, in, false, p, W);
                float x = p.pos.x, z = p.pos.z;
                if (x > h.x0 + 0.5f && x < h.x1 - 0.5f && z > h.z0 + 0.5f && z < h.z1 - 0.5f) inside++;
                float dx = std::max(std::max(h.x0 - x, x - h.x1), 0.f), dz = std::max(std::max(h.z0 - z, z - h.z1), 0.f);
                closest = std::min(closest, sqrtf(dx * dx + dz * dz));
            }
            // it has to have actually reached the structure (not been stuck at the start) and never got inside
            if (inside == 0 && closest < 3.f) ok++;
            else snprintf(det + strlen(det), sizeof det - strlen(det), "%s: %d frames inside, closest %.1f m; ", h.name, inside, closest);
        }
        snprintf(b, sizeof b, "%d/%d stopped. %.200s", ok, tot, det);
        check(ok == tot, "jet ski can't drive through piers / bridge bases / ships", b);
    }
    {   // a grind that slows to a stop steps off onto the deck: not over the Golden Gate railing, not into a rail
        struct { float x, z; bool ns; } spots[] = {{-1086.2f, -1500, true}, {-1130, -2377, false}};
        int ok = 0, tot = 0;
        char det[400] = "";
        for (auto& sp : spots) {
            int ri = -1;
            for (int i = 0; i < (int)W.rails.size() && ri < 0; i++) {
                const Rail& r = W.rails[i];
                float lo = sp.ns ? std::min(r.a.z, r.b.z) : std::min(r.a.x, r.b.x), hi = sp.ns ? std::max(r.a.z, r.b.z) : std::max(r.a.x, r.b.x);
                float c = sp.ns ? sp.z : sp.x;
                if ((sp.ns ? fabsf(r.a.x - sp.x) + fabsf(r.b.x - sp.x) : fabsf(r.a.z - sp.z) + fabsf(r.b.z - sp.z)) < 0.3f && c > lo && c < hi) ri = i;
            }
            if (ri < 0) continue;
            for (int dirn = -1; dirn <= 1; dirn += 2) {
                const Rail& r = W.rails[ri];
                Sim s; s.spawn(sp.x, sp.z, 0, 0);
                float t = sp.ns ? (sp.z - r.a.z) / (r.b.z - r.a.z) : (sp.x - r.a.x) / (r.b.x - r.a.x);
                s.p.state = Player::GRIND; s.p.rail = ri; s.p.railT = t; s.p.railDir = dirn;
                s.p.pos = Vector3Lerp(r.a, r.b, t); s.p.speed = 0.5f; s.p.yaw = atan2f((r.b.z - r.a.z) * dirn, (r.b.x - r.a.x) * dirn); s.p.balance = s.p.balanceVel = 0;
                float railY = s.p.pos.y;
                s.run(1, Input{});
                float side = sp.ns ? s.p.pos.x - sp.x : s.p.pos.z - sp.z;
                s.run(180, Input{}, [](Player& p) { return p.state == Player::RIDE || p.state == Player::BAIL; });
                float deck = W.ground(sp.ns ? sp.x - 1.5f : sp.x, sp.ns ? sp.z : sp.z - 1.5f, railY);
                bool good = s.p.state == Player::RIDE && !s.p.splashed && fabsf(side) > 0.4f && fabsf(s.p.pos.y - deck) < 0.5f;
                tot++; ok += good;
                char one[120]; snprintf(one, sizeof one, "%s%s off=%.2f y=%.1f st=%d %s; ", r.name, dirn > 0 ? "+" : "-", side, s.p.pos.y, s.p.state, s.p.subPopup.c_str());
                strncat(det, one, sizeof det - strlen(det) - 1);
            }
        }
        check(tot == 4 && ok == tot, "grind stop steps off onto the deck", det);
    }
    {   // a boardslide that slows to a stop on the Embarcadero Flat Bar rolls out like a 50-50, not "Landed sideways"
        int ri = -1;
        for (int i = 0; i < (int)W.rails.size(); i++) if (strcmp(W.rails[i].name, "Flat Bar") == 0) ri = i;
        int ok = 0, tot = 0;
        char det[400] = "";
        for (int bs = 0; bs <= 1 && ri >= 0; bs++)
            for (int dirn = -1; dirn <= 1; dirn += 2) {
                const Rail& r = W.rails[ri];
                SetRandomSeed(8400 + tot);
                Sim s; s.spawn(1205, -238, 0, 0);
                float t = 0.5f, along = atan2f((r.b.z - r.a.z) * dirn, (r.b.x - r.a.x) * dirn);
                s.p.state = Player::GRIND; s.p.rail = ri; s.p.railT = t; s.p.railDir = dirn; s.p.railFakie = false;
                s.p.boardslide = bs; s.p.yaw = wrapPi(along + (bs ? PI / 2 : 0.f));
                s.p.pos = Vector3Lerp(r.a, r.b, t); s.p.speed = 0.5f; s.p.balance = s.p.balanceVel = 0;
                s.p.combo = {bs ? "Boardslide (Flat Bar)" : "50-50 (Flat Bar)"}; s.p.comboPts = 500;
                s.run(180, Input{}, [](Player& p) { return p.state == Player::RIDE || p.state == Player::BAIL; });
                bool good = s.p.state == Player::RIDE && s.p.score > 0;
                tot++; ok += good;
                char one[120]; snprintf(one, sizeof one, "%s%s st=%d score=%lld %s; ", bs ? "board" : "50-50", dirn > 0 ? "+" : "-", s.p.state, s.p.score, s.p.subPopup.c_str());
                strncat(det, one, sizeof det - strlen(det) - 1);
            }
        check(tot == 4 && ok == tot, "boardslide stop rolls out", det);
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
    {   // teleporting is not a free token: idling at a spawn point (rolling down its slope, no input) never
        // brings the skater into a token's pickup radius (the game's test: feet + 1 m within 3 m)
        int bad = 0;
        for (auto& sp : W.spots) {
            Sim s; s.spawn(sp.pos.x, sp.pos.z, sp.yaw);
            const char* hit = nullptr;
            s.run(60 * 3, Input{}, [&](Player& p) {
                for (auto& t : W.tokens)
                    if (Vector3Distance(t.pos, {p.pos.x, p.pos.y + 1.0f, p.pos.z}) < 3.0f) { hit = t.name; return true; }
                return false;
            });
            if (hit) { bad++; printf("      spot %s collects token %s with no input\n", sp.name, hit); }
        }
        snprintf(b, sizeof b, "%d of %d", bad, (int)W.spots.size());
        check(bad == 0, "no token is collected just by teleporting", b);
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
    {   // street lamps, trees and curb clutter are never built inside a building or landmark base (issue #88:
        // Coit Tower's base swallowed the hilltop's lamps and trees, a lamp head poking out of its roof)
        std::unordered_map<long long, std::vector<const Platform*>> blds;   // building boxes bucketed by 32 m cell
        auto cell = [](float x, float z) { return (long long)floorf(x / 32.f) * 1000003LL + (long long)floorf(z / 32.f); };
        for (auto& pl : W.plats) {
            if (pl.kind != PK_BUILDING || pl.x1 - pl.x0 > 600 || pl.z1 - pl.z0 > 600) continue;
            for (float x = floorf(pl.x0 / 32.f) * 32.f; x <= pl.x1; x += 32.f)
                for (float z = floorf(pl.z0 / 32.f) * 32.f; z <= pl.z1; z += 32.f) blds[cell(x + 1, z + 1)].push_back(&pl);
        }
        auto inside = [&](const Platform& p) {   // pole centre inside a building box that overlaps its height
            float x = (p.x0 + p.x1) / 2, z = (p.z0 + p.z1) / 2, top = std::max(p.h0, p.h1);
            auto it = blds.find(cell(x, z));
            if (it == blds.end()) return false;
            for (const Platform* bl : it->second)
                if (bl->contains(x, z) && bl->base < top - 0.5f && std::max(bl->h0, bl->h1) > p.base + 1.2f) return true;
            return false;
        };
        int coit = 0, coitBad = 0, city = 0, cityBad = 0;
        for (auto& p : W.plats) {
            if (p.kind != PK_POLE || p.x1 - p.x0 > 1.8f || p.z1 - p.z0 > 1.8f) continue;
            float x = (p.x0 + p.x1) / 2, z = (p.z0 + p.z1) / 2, tall = std::max(p.h0, p.h1) - p.base;
            if (x > 840 && x < 920 && z > -920 && z < -840) {   // Telegraph Hill around Coit Tower: everything small
                coit++;
                if (inside(p)) { coitBad++; printf("      pole at %.1f, %.1f inside Coit Tower's base\n", x, z); }
            }
            // city-wide: street lamps / signal poles (7.3-7.5 m boxes) and trees (6 m boxes) on the SF grid
            if (x > -1460 && x < 1340 && z > -1080 && z < 1820 && tall > 5.9f && tall < 7.6f) {
                city++;
                if (inside(p)) { cityBad++; if (cityBad <= 5) printf("      lamp/tree at %.1f, %.1f inside a building\n", x, z); }
            }
        }
        snprintf(b, sizeof b, "Coit hill: %d of %d inside; city lamps/trees: %d of %d inside", coitBad, coit, cityBad, city);
        check(coitBad == 0 && city > 1000 && cityBad == 0, "no street furniture inside buildings", b);
    }
    {   // tourists stand on the ground at their landmark: on the terrain (or a kerb a step up from it), or on a deck
        // or pier (SFO's departures level, out over the water) -- never up on a roof or a monument, never inside
        // one, and never out in the traffic on a car path or a city street
        auto road = [](float x, float z, bool deck) {
            for (size_t k = 0; k < W.carPaths.size(); k++) {
                const Path& P = W.carPaths[k];
                if (P.onDeck != deck) continue;
                for (size_t s = 0; s + 1 < P.p.size(); s++)
                    if (distToSeg2D(x, z, P.p[s].x, P.p[s].y, P.p[s + 1].x, P.p[s + 1].y) < W.carPathLanes[k] * 4.f) return true;
            }
            if (deck) return false;
            for (const Grid* g : {&W.sf, &W.oak, &W.mp})
                for (int j = 0; j < g->nj; j++)
                    for (int i = 0; i < g->ni; i++)
                        for (int d : {0, 2}) {
                            if (!g->edge(i, j, d)) continue;
                            Vector2 m = g->node(i, j), e = g->node(i + DI[d], j + DJ[d]);
                            if (distToSeg2D(x, z, m.x, m.y, e.x, e.y) < g->roadHalf) return true;
                        }
            return false;
        };
        int bad = 0, roof = 0, inside = 0, floating = 0, inRoad = 0, decks = 0;
        for (const Vector3& t : W.tourists) {
            float terr = W.terrain(t.x, t.z), g = W.ground(t.x, t.z, t.y);
            bool deck = false;
            for (const Platform& pl : W.plats)
                deck |= (pl.kind == PK_PIER || pl.kind == PK_DECK) && pl.contains(t.x, t.z) && std::min(pl.x1 - pl.x0, pl.z1 - pl.z0) > 4.f &&
                        fabsf(pl.top(t.x, t.z) - t.y) < 0.05f && t.y <= std::max(terr, 0.f) + 10.f;
            bool over = !deck && (terr < 0.15f || t.y - terr > STEP_UP + 0.1f);
            Vector3 q = t;
            Vector2 n;
            bool in = W.collide(q, 0.3f, t.y, 1.7f, &n) || W.solidAt({t.x, t.y + 0.6f, t.z}, true);
            bool fl = fabsf(g - t.y) > 0.05f || t.y < terr - 0.05f || W.isWater(t.x, t.z, t.y);
            bool rd = road(t.x, t.z, deck);
            if (over || in || fl || rd) {
                if (bad++ < 6) printf("      tourist at %.0f,%.0f (%s) y %.2f, ground %.2f, terrain %.2f%s%s%s%s\n", t.x, t.z, W.district(t.x, t.z),
                                      t.y, g, terr, over ? " up high" : "", in ? " inside a solid" : "", fl ? " not on a surface" : "",
                                      rd ? " in a roadway" : "");
            }
            roof += over; inside += in; floating += fl; inRoad += rd; decks += deck;
        }
        snprintf(b, sizeof b, "%d of %d tourists misplaced (%d up high, %d inside a solid, %d off a surface, %d in a roadway; %d on decks)",
                 bad, (int)W.tourists.size(), roof, inside, floating, inRoad, decks);
        check(W.tourists.size() > 100 && bad == 0, "Tourists stand on the ground", b);
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
    {   // stepping off the train (at 4th & King or Menlo Park, the two ends you can ride to): on the platform,
        // clear of the station building, and you can push off down the platform without hitting anything
        int bad = 0;
        for (int i : {0, (int)W.stations.size() - 1}) {
            float yaw;
            Vector3 p = W.caltrainDropoff(i, &yaw);
            Vector3 q = p;
            Vector2 n;
            bool inWall = W.collide(q, 0.4f, p.y, 1.5f, &n);
            bool onGround = fabsf(W.ground(p.x, p.z, p.y + 0.5f) - p.y) < 0.05f && !W.isWater(p.x, p.z, p.y);
            Sim s; s.p.spawn(p, yaw);
            Input in; in.push = true;
            bool bailed = s.run(60 * 8, in, [](Player& pl) { return pl.state == Player::BAIL; });   // push off down the platform
            float moved = Vector2Distance({s.p.pos.x, s.p.pos.z}, {p.x, p.z});
            if (inWall || !onGround || bailed || s.p.state != Player::RIDE || moved < 40) {
                bad++;
                printf("      %s: (%.1f, %.1f) wall %d ground %d state %d moved %.1f\n", W.stations[i].name, p.x, p.z, inWall, onGround,
                       s.p.state, moved);
            }
        }
        snprintf(b, sizeof b, "%d bad of 2", bad);
        check(bad == 0, "Caltrain arrival spots are clear", b);
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
    {   // furniture indoors stops you (sofa back, floor lamp, potted plant, TV), and the TV leaves the way to the
        // back room open. Positions follow the room layout in World::addHouseInterior (u along the facade, d inward).
        int tried = 0, bad[5] = {0}, runs[5] = {0};
        for (int k = 0; k < (int)W.houses.size() && tried < 16; k += 37) {
            const House& h = W.houses[k];
            const float t = 0.35f;
            float fu0 = h.facing < 2 ? h.x0 : h.z0, fu1 = h.facing < 2 ? h.x1 : h.z1;
            float D = h.facing < 2 ? h.z1 - h.z0 : h.x1 - h.x0, iu0 = fu0 + t, iu1 = fu1 - t;
            if (iu1 - iu0 < 6.3f) continue;
            tried++;
            auto toW = [&](float u, float d) -> Vector2 {
                switch (h.facing) { case 0: return {u, h.z0 + d}; case 1: return {u, h.z1 - d}; case 2: return {h.x0 + d, u}; default: return {h.x1 - d, u}; }
            };
            auto toU = [&](Vector3 p) { return h.facing < 2 ? p.x : p.z; };
            auto toD = [&](Vector3 p) { switch (h.facing) { case 0: return p.z - h.z0; case 1: return h.z1 - p.z; case 2: return p.x - h.x0; default: return h.x1 - p.x; } };
            // ride from (u, d) along (du, dd) for 3 s; returns how far it got along that direction
            auto ride = [&](float u, float d, float du, float dd) {
                Vector2 s0 = toW(u, d), s1 = toW(u + du, d + dd);
                Sim s; s.p.spawn({s0.x, h.floorY, s0.y}, atan2f(s1.y - s0.y, s1.x - s0.x)); s.p.speed = 3;
                float best = 0;
                for (int f = 0; f < 180; f++) {
                    Input in; in.push = s.p.speed < 3;
                    s.p.update(1.f / 60, in, W);
                    best = std::max(best, (toU(s.p.pos) - u) * du + (toD(s.p.pos) - d) * dd);
                }
                return best;
            };
            float doorU = h.facing < 2 ? h.doorIn.x : h.doorIn.y;
            bool doorLeft = doorU < (fu0 + fu1) * 0.5f;
            float dp = std::clamp(D * 0.42f, 5.5f, 9.f), sofaD = t + 3.2f, gapC = doorLeft ? iu0 + 1.5f : iu1 - 1.5f;
            float farU0 = doorLeft ? iu1 - 2.4f : iu0, farU1 = doorLeft ? iu1 : iu0 + 2.4f, sgn = doorLeft ? 1.f : -1.f;
            float plantU = farU0 + 0.45f;   // plant: ride at it from 2 m out, toward the side wall behind it
            runs[0]++; if (ride(plantU - 2.f * sgn, sofaD - 0.75f, sgn, 0) > 1.5f) bad[0]++;
            float span0 = doorLeft ? gapC + 0.7f : farU1, span1 = doorLeft ? farU0 : gapC - 0.7f;
            if (span1 - span0 >= 1.7f) {   // TV: ride at it toward the partition; it stops you in front of the screen
                runs[1]++; if (ride((span0 + span1) * 0.5f, dp - 2.2f, 0, 1) > 1.79f) bad[1]++;
            }
            runs[2]++; if (ride(gapC, dp - 2.2f, 0, 1) < 2.8f) bad[2]++;   // the doorway in the partition stays open
            if (doorLeft) {
                runs[3]++; if (ride(iu1 - 0.35f, sofaD - 1.5f, 0, 1) > 1.3f) bad[3]++;   // along the wall into the sofa back
                float ld = sofaD + 2.625f;
                if (fabsf(ld - (dp + 0.1f)) > 0.9f) { runs[4]++; if (ride(iu1 - 2.235f, ld, 1, 0) > 1.6f) bad[4]++; } // lamp
            }
        }
        snprintf(b, sizeof b, "%d rooms; rode through plant %d/%d, TV %d/%d, sofa back %d/%d, lamp %d/%d; doorway blocked %d/%d",
                 tried, bad[0], runs[0], bad[1], runs[1], bad[3], runs[3], bad[4], runs[4], bad[2], runs[2]);
        check(tried >= 8 && runs[1] > 0 && runs[3] > 0 && runs[4] > 0 && !bad[0] && !bad[1] && !bad[2] && !bad[3] && !bad[4],
              "Furniture indoors is solid", b);
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
    {   // gondola towers stand on four legs: you can ride straight through between them
        // (each tower adds its four leg boxes in a row: PK_POLE, 4 m tall, about a metre across)
        auto isLeg = [](const Platform& q) {
            return q.kind == PK_POLE && fabsf(q.h0 - q.base - 4.f) < 0.01f && q.x1 - q.x0 > 0.6f && q.x1 - q.x0 < 1.6f;
        };
        int towers = 0, through = 0;
        for (size_t i = 0; i + 3 < W.plats.size(); i++) {
            const Platform* l[4] = {&W.plats[i], &W.plats[i + 1], &W.plats[i + 2], &W.plats[i + 3]};
            bool ok = true;
            Vector2 c[4], mid = {0, 0};
            for (int k = 0; k < 4; k++) {
                ok &= isLeg(*l[k]);
                c[k] = {(l[k]->x0 + l[k]->x1) * 0.5f, (l[k]->z0 + l[k]->z1) * 0.5f};
                mid = Vector2Add(mid, Vector2Scale(c[k], 0.25f));
            }
            for (int k = 0; k < 4 && ok; k++) ok &= Vector2Distance(c[k], mid) < 6.f;
            if (!ok) continue;
            towers++;
            i += 3;
            Vector2 d = Vector2Normalize(Vector2Subtract(c[1], c[0]));   // between legs 0 and 1 and out between 2 and 3
            // ride in from the downhill side so the skater keeps speed
            float hA = W.terrain(mid.x - d.x * 12, mid.y - d.y * 12), hB = W.terrain(mid.x + d.x * 12, mid.y + d.y * 12);
            if (hA < hB) d = Vector2Scale(d, -1);
            // start on the hillside itself, not on top of whatever might stand there
            Sim s;
            float sx = mid.x - d.x * 6, sz = mid.y - d.y * 6;   // close in: some towers sit on steep slopes
            s.p.spawn({sx, W.ground(sx, sz, W.terrain(sx, sz) + 0.5f), sz}, atan2f(d.y, d.x));
            s.p.speed = 6;
            Input in; in.push = true;
            bool bailed = false;
            bool passed = s.run(360, in, [&](Player& p) {
                bailed |= p.state == Player::BAIL;
                return Vector2DotProduct({p.pos.x - mid.x, p.pos.z - mid.y}, d) > 5.f;
            });
            if (passed && !bailed) through++;
        }
        snprintf(b, sizeof b, "%d of %d towers ridden through", through, towers);
        check(towers > 0 && through == towers, "Ride between gondola tower legs", b);
    }
    {   // gondola cabins clear the station roofs: wherever the cable passes over a terminal, the cabin's floor
        // stays above the roof slab (terminal: a 12 m PK_PROP box under the station; slab top 0.38 m above it)
        int stations = 0, clear = 0;
        float worst = 1e9f;
        for (const GondolaLine& g : W.gondolas)
            for (float ss : g.stationS) {
                size_t k = std::lower_bound(g.cum.begin(), g.cum.end(), ss) - g.cum.begin();
                Vector3 c = g.path[std::min(k, g.path.size() - 1)];
                const Platform* term = nullptr;
                for (const Platform& q : W.plats)
                    if (q.kind == PK_PROP && fabsf(q.x1 - q.x0 - 12.f) < 0.01f && fabsf(q.z1 - q.z0 - 12.f) < 0.01f &&
                        fabsf((q.x0 + q.x1) * 0.5f - c.x) < 1.f && fabsf((q.z0 + q.z1) * 0.5f - c.z) < 1.f) term = &q;
                if (!term) continue;
                stations++;
                float roof = term->h0 + 0.38f, gap = 1e9f;
                for (float s = std::max(0.f, ss - 30.f); s <= std::min(g.len, ss + 30.f); s += 0.5f) {
                    size_t i = std::min(std::max<size_t>(std::upper_bound(g.cum.begin(), g.cum.end(), s) - g.cum.begin(), 1), g.path.size() - 1);
                    float t = clampf((s - g.cum[i - 1]) / std::max(g.cum[i] - g.cum[i - 1], 1e-4f), 0, 1);
                    Vector3 p = Vector3Lerp(g.path[i - 1], g.path[i], t);
                    if (sq(p.x - c.x) + sq(p.z - c.z) > sq(7.2f * 1.4143f + 1.2f)) continue;   // not over the roof
                    gap = std::min(gap, p.y - GONDOLA_CABIN_DROP - roof);
                }
                worst = std::min(worst, gap);
                if (gap > 0) clear++;
            }
        snprintf(b, sizeof b, "%d of %d stations clear, tightest %.2f m", clear, stations, worst);
        check(stations >= 7 && clear == stations, "Gondola cabins clear station roofs", b);
    }
    {   // Lombard planters: the collision box is the drawn shrub (same layout as World::buildLombard)
        float x0 = gx(16) + 6, x1 = gx(17) - 6, zc = gz(3);
        std::vector<Vector2> pts;
        pts.push_back({gx(16) - 4, zc});
        for (int k = 0; k <= 8; k++) pts.push_back({lerpf(x0, x1, k / 8.f), zc + ((k % 2) ? 6.5f : -6.5f)});
        int tot = 0, tallOk = 0, wideOk = 0;
        float worst = 99;
        for (int k = 1; k <= 8; k++) {
            Vector2 a = pts[k], b2 = pts[k + 1];
            Vector2 d = Vector2Normalize(Vector2Subtract(b2, a)), n = {-d.y, d.x};
            float L = Vector2Distance(a, b2);
            for (int side = -1; side <= 1; side += 2)
                for (float s0 = 2.5f; s0 < L - 2.0f; s0 += 1.6f) {
                    Vector2 p = Vector2Add(Vector2Add(a, Vector2Scale(d, s0)), Vector2Scale(n, side * 3.4f));
                    if (fabsf(p.y - zc) > 9.5f) continue;
                    float gy = W.terrain(p.x, p.y);
                    tot++;
                    if (W.ground(p.x, p.y, gy + 2) > gy + 1.0f) tallOk++;   // standing on it puts the board at the shrub's crown
                    // roll from the road straight into it: the skater stops outside the foliage
                    Vector2 q = Vector2Subtract(p, Vector2Scale(n, side * 1.8f));
                    Sim s;
                    s.p.spawn({q.x, W.ground(q.x, q.y, W.terrain(q.x, q.y) + 0.5f), q.y}, atan2f(p.y - q.y, p.x - q.x));
                    s.p.speed = 4;
                    float minD = 99;
                    s.run(90, Input{}, [&](Player& pl) { minD = std::min(minD, sqrtf(sq(pl.pos.x - p.x) + sq(pl.pos.z - p.y))); return false; });
                    worst = std::min(worst, minD);
                    if (minD > 1.05f) wideOk++;
                }
        }
        snprintf(b, sizeof b, "%d planters, %d tall enough, %d stop outside (closest %.2f m)", tot, tallOk, wideOk, worst);
        check(tot > 20 && tallOk == tot && wideOk == tot, "Lombard planters match the shrubs", b);
    }
    {   // Muni bus shelters: the end panels and the stop-sign pole stop you, the front stays open, the roof is a ceiling
        // (the roof is a thin PK_DECK box about 4.1 m by 1.5 m, 2.44 m above the sidewalk; the curb-side back panel
        // is a PK_POLE box 3.8 m long and 3.3 m tall, the sign pole a 0.12 m PK_POLE 3.9 m tall at the glass end)
        auto near = [](const Platform& q, Vector2 m, float r) { return fabsf((q.x0 + q.x1) * 0.5f - m.x) < r && fabsf((q.z0 + q.z1) * 0.5f - m.y) < r; };
        int shelters = 0, blocked = 0, poles = 0, open = 0, capped = 0;
        float worstGap = 0;
        for (const Platform& r : W.plats) {
            float sx = r.x1 - r.x0, sz = r.z1 - r.z0;
            if (r.kind != PK_DECK || fabsf(r.h0 - r.base - 0.12f) > 0.01f || fabsf(std::max(sx, sz) - 4.1f) > 0.05f || fabsf(std::min(sx, sz) - 1.5f) > 0.05f) continue;
            if (shelters >= 20) break;
            Vector2 mid = {(r.x0 + r.x1) * 0.5f, (r.z0 + r.z1) * 0.5f}, d = sx > sz ? Vector2{1, 0} : Vector2{0, 1}, o = {-d.y, d.x};
            const Platform *back = nullptr, *sign = nullptr;
            for (const Platform& q : W.plats) {
                if (q.kind != PK_POLE || !near(q, mid, 3.f)) continue;
                float w = q.x1 - q.x0, h = q.z1 - q.z0, t = q.h0 - q.base;
                if (fabsf(std::max(w, h) - 3.8f) < 0.05f && std::min(w, h) < 0.2f && fabsf(t - 3.3f) < 0.01f) back = &q;
                if (w < 0.13f && h < 0.13f && fabsf(t - 3.9f) < 0.01f) sign = &q;
            }
            if (!back || !sign) continue;
            shelters++;
            // shelter space: +o toward the back panel (curb), -o the open front the bench faces; +d toward the sign pole
            if (Vector2DotProduct({(back->x0 + back->x1) * 0.5f - mid.x, (back->z0 + back->z1) * 0.5f - mid.y}, o) < 0) o = Vector2Scale(o, -1);
            Vector2 sp = {(sign->x0 + sign->x1) * 0.5f, (sign->z0 + sign->z1) * 0.5f};
            if (Vector2DotProduct(Vector2Subtract(sp, mid), d) < 0) d = Vector2Scale(d, -1);
            float g = W.ground(mid.x, mid.y, r.base - 1.f);
            // ride from 4.5 m out toward a target `at` along d (offset `out` along o); the skater must stop short of it,
            // within 0.5 m (the gap is measured from the skater's center to the target's center plane)
            auto rideInto = [&](float at, float out, float sgn) {
                Vector2 dir = Vector2Scale(d, -sgn), c = Vector2Add(mid, Vector2Scale(o, out));
                float x = c.x + d.x * sgn * (fabsf(at) + 4.5f), z = c.y + d.y * sgn * (fabsf(at) + 4.5f);
                Sim s;
                s.p.spawn({x, W.ground(x, z, g + 0.5f), z}, atan2f(dir.y, dir.x));
                s.p.speed = 6;
                Input in; in.push = true;
                float gap = 99;
                s.run(240, in, [&](Player& p) { gap = std::min(gap, Vector2DotProduct({p.pos.x - c.x, p.pos.z - c.y}, Vector2Scale(dir, -1.f)) - fabsf(at)); return false; });
                worstGap = std::max(worstGap, gap);
                return gap > 0 && gap <= 0.5f;
            };
            if (rideInto(1.9f, -0.02f, -1) && rideInto(1.9f, -0.02f, 1)) blocked++;   // ad panel end, glass end
            float pAt = Vector2DotProduct(Vector2Subtract(sp, mid), d), pOut = Vector2DotProduct(Vector2Subtract(sp, mid), o);
            if (rideInto(pAt, pOut, 1)) poles++;
            // the open front: standing room in the middle, nothing in the gap between the front posts
            Vector3 c = {mid.x, g, mid.y}, f = {mid.x - o.x * 0.8f, g, mid.y - o.y * 0.8f};
            if (!W.collide(c, 0.3f, g, 1.7f, nullptr) && !W.collide(f, 0.3f, g, 1.7f, nullptr)) open++;
            // an ollie underneath bumps the roof instead of poking through it
            Sim s;
            s.p.spawn({mid.x, g, mid.y}, atan2f(d.y, d.x));
            Input in; in.ollie = true;
            float top = 0;
            s.run(90, in, [&](Player& p) { top = std::max(top, p.pos.y + 1.75f); return false; });
            if (W.ceiling(mid.x, mid.y, g) <= r.base + 0.01f && top <= r.base + 0.02f) capped++;
        }
        snprintf(b, sizeof b, "%d shelters: %d ends blocked, %d sign poles, %d open fronts, %d roofs overhead (widest gap %.2f m)", shelters, blocked, poles, open, capped, worstGap);
        check(shelters > 0 && blocked == shelters && poles == shelters && open == shelters && capped == shelters, "Bus shelters are solid", b);
    }
    {   // traffic doesn't drive through itself: the queue behind a skater standing on the Golden Gate, and Market St and
        // the Bay Bridge's Embarcadero end against the street grid; and nobody gets gridlocked at those crossings
        auto overlap = [](const Car& a, const Car& b) {   // oriented boxes, a little inside the drawn bodies
            const CarModel &ma = carModel(a.type), &mb = carModel(b.type);
            if (fabsf(a.pos.y - b.pos.y) > 3) return false;
            Vector2 ax[4] = {{cosf(a.yaw), sinf(a.yaw)}, {-sinf(a.yaw), cosf(a.yaw)}, {cosf(b.yaw), sinf(b.yaw)}, {-sinf(b.yaw), cosf(b.yaw)}};
            Vector2 d = {b.pos.x - a.pos.x, b.pos.z - a.pos.z};
            for (auto& n : ax) {
                float ra = (ma.halfLen - 0.15f) * fabsf(Vector2DotProduct(ax[0], n)) + (ma.halfWid - 0.15f) * fabsf(Vector2DotProduct(ax[1], n));
                float rb = (mb.halfLen - 0.15f) * fabsf(Vector2DotProduct(ax[2], n)) + (mb.halfWid - 0.15f) * fabsf(Vector2DotProduct(ax[3], n));
                if (fabsf(Vector2DotProduct(d, n)) > ra + rb) return false;
            }
            return true;
        };
        struct { const char* n; float x, z; } spots[] = {{"Golden Gate", -1100, -2000}, {"Bay Bridge end", 1265, 335}, {"Market St", 1100, -80},
                                                          // the path ends on the Embarcadero line (x = 1265): Market St and the airport highway
                                                          {"Market St end", 1265, -200}, {"Airport hwy end", 1265, 1765}};
        float sameOv = 0, crossOv = 0, longestStop = 0;
        // embOv: crossing overlap at the Embarcadero line (roadway x 1258..1272), which the path ends' U-turns keep clear of.
        // slowest: the lowest average speed of the path cars (Market St, bridges, highway) at a spot (are they moving?)
        float embOv = 0, slowest = 1e9f;
        for (auto& sp : spots) {
            Traffic T; T.init(W);
            Player pl; pl.spawn({sp.x, W.ground(sp.x, sp.z, 1000), sp.z}, 0);
            bool onBridge = &sp == &spots[0];
            if (!onBridge) pl.pos.y += 200;   // the skater out of everyone's way
            std::vector<float> still(T.cars.size(), 0);
            float mainV = 0; int mainN = 0;
            for (int f = 0; f < 60 * 70; f++) {
                T.update(1.f / 60, W, pl);
                for (size_t i = 0; i < T.cars.size(); i++) {
                    const Car& a = T.cars[i];
                    if (a.mode > 1 || sq(a.pos.x - sp.x) + sq(a.pos.z - sp.z) > 300 * 300) continue;
                    still[i] = a.speed < 0.3f ? still[i] + 1.f / 60 : 0;
                    if (!onBridge) longestStop = std::max(longestStop, still[i]);
                    if (!onBridge && a.mode == 1) { mainV += a.speed; mainN++; }
                    if (f < 60 * 10 || sq(a.pos.x - sp.x) + sq(a.pos.z - sp.z) > 150 * 150) continue;
                    for (size_t j = i + 1; j < T.cars.size(); j++) {
                        const Car& b = T.cars[j];
                        if (b.mode > 1 || (a.mode == 0 && b.mode == 0) || fabsf(a.pos.x - b.pos.x) > 25 || fabsf(a.pos.z - b.pos.z) > 25) continue;
                        if (a.mode != b.mode && std::max(a.pos.x, b.pos.x) > 1235 && overlap(a, b)) embOv += 1.f / 60;
                        // not checked here: a bus swinging wide round the inner U-turn loop, or a streetcar crossing
                        // over to the other track at the end of its line, against the same road's other lane
                        bool big = a.type == CT_BUS || b.type == CT_BUS || a.type == CT_STREETCAR || b.type == CT_STREETCAR;
                        if (big && a.mode == b.mode) continue;
                        if (overlap(a, b)) (a.mode == b.mode ? sameOv : crossOv) += 1.f / 60;
                    }
                }
            }
            if (!onBridge) slowest = std::min(slowest, mainV / std::max(mainN, 1));
        }
        snprintf(b, sizeof b, "overlapping: same road %.1f s, crossing %.1f s (Embarcadero %.1f s); longest stop %.0f s; main roads avg %.1f m/s",
                 sameOv, crossOv, embOv, longestStop, slowest);
        check(sameOv == 0 && crossOv < 20 && embOv == 0 && longestStop < 60 && slowest > 5, "Traffic keeps out of itself", b);
        W.vehicleTops.clear();   // T.update published the roofs of the cars near pl
    }
    {   // street-grid cars take turns at intersections instead of driving through each other, and nobody gets
        // gridlocked waiting on everyone else: the whole city, a few minutes of traffic. Crossings of a main road
        // (Market St, the bridges and highways) go by its rules, checked above, so this looks at the rest of the grid.
        // (Over nine traffic seeds, 240 s: overlapping 0-5.3 s, where the streets without turn-taking run to ~600 s;
        // longest stop under 45 s; nobody stood in an intersection more than 7 s.)
        auto overlap = [](const Car& a, const Car& b) {   // oriented boxes, a little inside the drawn bodies
            const CarModel &ma = carModel(a.type), &mb = carModel(b.type);
            if (fabsf(a.pos.y - b.pos.y) > 3) return false;
            Vector2 ax[4] = {{cosf(a.yaw), sinf(a.yaw)}, {-sinf(a.yaw), cosf(a.yaw)}, {cosf(b.yaw), sinf(b.yaw)}, {-sinf(b.yaw), cosf(b.yaw)}};
            Vector2 d = {b.pos.x - a.pos.x, b.pos.z - a.pos.z};
            for (auto& n : ax) {
                float ra = (ma.halfLen - 0.15f) * fabsf(Vector2DotProduct(ax[0], n)) + (ma.halfWid - 0.15f) * fabsf(Vector2DotProduct(ax[1], n));
                float rb = (mb.halfLen - 0.15f) * fabsf(Vector2DotProduct(ax[2], n)) + (mb.halfWid - 0.15f) * fabsf(Vector2DotProduct(ax[3], n));
                if (fabsf(Vector2DotProduct(d, n)) > ra + rb) return false;
            }
            return true;
        };
        // 20 m cells within 120 m of a main road
        const float MC = 20, MR = 120;
        std::vector<int64_t> nearMain;
        auto cellOf = [&](float x, float z) { return (int64_t)floorf(x / MC) << 32 ^ (int64_t)(uint32_t)(int)floorf(z / MC); };
        for (auto& P : W.carPaths)
            for (float s = 0; s <= P.len; s += 4) {
                Vector2 q = P.at(s);
                for (float dz = -MR; dz <= MR; dz += MC)
                    for (float dx = -MR; dx <= MR; dx += MC)
                        if (dx * dx + dz * dz <= MR * MR) nearMain.push_back(cellOf(q.x + dx, q.y + dz));
            }
        std::sort(nearMain.begin(), nearMain.end());
        nearMain.erase(std::unique(nearMain.begin(), nearMain.end()), nearMain.end());
        auto away = [&](const Car& c) { return !std::binary_search(nearMain.begin(), nearMain.end(), cellOf(c.pos.x, c.pos.z)); };
        Traffic T; T.init(W);
        Player pl; pl.spawn({0, W.ground(0, 0, 1000) + 200, 0}, 0);   // the skater out of everyone's way
        std::vector<int> grid;
        for (size_t i = 0; i < T.cars.size(); i++) if (T.cars[i].mode == 0) grid.push_back((int)i);
        std::vector<float> still(T.cars.size(), 0), stillIn(T.cars.size(), 0);
        float gridOv = 0, longest = 0, longestIn = 0;
        double dist = 0;   // (summed over ~370k car-seconds: a float drifts several percent)
        const int SECS = 240;
        for (int f = 0; f < 60 * SECS; f++) {
            T.update(1.f / 60, W, pl);
            std::sort(grid.begin(), grid.end(), [&](int a, int b) { return T.cars[a].pos.x < T.cars[b].pos.x; });
            bool counting = f >= 60 * 10;   // after everyone's settled in from where they were dropped
            for (size_t k = 0; k < grid.size(); k++) {
                const Car& a = T.cars[grid[k]];
                if (!counting) continue;
                dist += a.speed / 60;
                if (!away(a)) { still[grid[k]] = 0; continue; }
                // (only counting queues for intersections off the main roads too: one backed up from a main-road
                // crossing can reach a long way back, and that crossing goes by the main road's rules)
                Vector2 nx = a.g->node(a.i + DI[a.d], a.j + DJ[a.d]);
                bool nextAway = !std::binary_search(nearMain.begin(), nearMain.end(), cellOf(nx.x, nx.y));
                still[grid[k]] = a.speed < 0.3f && nextAway ? still[grid[k]] + 1.f / 60 : 0;
                longest = std::max(longest, still[grid[k]]);
                // standing in the middle of an intersection (not waiting at its line): blocking everyone across
                const Grid& g = *a.g;
                float ux = (a.pos.x - g.x0) / g.sp, uz = (a.pos.z - g.z0) / g.sp;
                bool inBox = fabsf(ux - roundf(ux)) * g.sp < g.roadHalf && fabsf(uz - roundf(uz)) * g.sp < g.roadHalf;
                stillIn[grid[k]] = a.speed < 0.3f && inBox ? stillIn[grid[k]] + 1.f / 60 : 0;
                longestIn = std::max(longestIn, stillIn[grid[k]]);
                for (size_t m = k + 1; m < grid.size() && T.cars[grid[m]].pos.x - a.pos.x < 25; m++) {
                    const Car& b = T.cars[grid[m]];
                    if (fabsf(a.pos.z - b.pos.z) < 25 && overlap(a, b)) gridOv += 1.f / 60;
                }
            }
        }
        float avg = (float)(dist / ((SECS - 10) * (double)grid.size()));
        snprintf(b, sizeof b, "%d grid cars, off the main roads: overlapping %.1f s, longest stop %.0f s (%.0f s in an intersection); average speed %.1f m/s",
                 (int)grid.size(), gridOv, longest, longestIn, avg);
        check(gridOv < 15 && longest < 120 && longestIn < 30 && avg > 6.5f, "Grid traffic takes turns", b);
        W.vehicleTops.clear();   // T.update published the roofs of the cars near pl
    }
    {   // and where the main roads cross the grid, and at their ends, nobody waits on anybody for minutes (#47): the
        // whole city for ten minutes (those lockups mostly started after three), every car on the main roads and the
        // grid, near the main roads or not. (Over nine traffic seeds, 600 s: one car in all stood just over a minute,
        // 62 s; longest stop on the main roads 42 s. Before, 25-209 cars a seed stood over a minute, the longest
        // 4.5-9.5 minutes, main-road cars included.)
        Traffic T; T.init(W);
        Player pl; pl.spawn({0, W.ground(0, 0, 1000) + 200, 0}, 0);   // the skater out of everyone's way
        std::vector<float> still(T.cars.size(), 0), most(T.cars.size(), 0);
        float longGrid = 0, longMain = 0;
        for (int f = 0; f < 60 * 600; f++) {
            T.update(1.f / 60, W, pl);
            if (f < 60 * 10) continue;
            for (size_t i = 0; i < T.cars.size(); i++) {
                const Car& a = T.cars[i];
                if (a.mode > 1) continue;
                still[i] = a.speed < 0.3f ? still[i] + 1.f / 60 : 0;
                most[i] = std::max(most[i], still[i]);
                (a.mode == 0 ? longGrid : longMain) = std::max(a.mode == 0 ? longGrid : longMain, still[i]);
            }
        }
        int stuck = 0;
        for (float m : most) stuck += m > 60;
        snprintf(b, sizeof b, "%d cars stood over a minute; longest stop %.0f s on the grid, %.0f s on the main roads",
                 stuck, longGrid, longMain);
        check(std::max(longGrid, longMain) < 90 && longMain < 60, "Main-road crossings don't lock up", b);
        W.vehicleTops.clear();   // T.update published the roofs of the cars near pl
    }
    {   // sidewalk walkers go round the front of a house's steps, not through them (#86): the walking line runs ~2 m
        // out from the house fronts and a stoop reaches up to 3 m out, so following it put them inside the step boxes,
        // feet at street level and legs sunk into the stoop (about 2% of them at any moment). Checked near the
        // skater (the full update) and far from them (the cheap one), once a second for half a minute.
        Traffic T; T.init(W);
        Player pl; pl.spawn({0, W.ground(0, 0, 1000) + 200, 0}, 0);   // the skater out of everyone's way
        int sunk = 0, samples = 0, walkers = 0;
        float worst = 0;
        Vector3 at{};
        for (auto& p : T.peds) walkers += p.mode == 0;
        for (int f = 1; f <= 60 * 30; f++) {
            T.update(1.f / 60, W, pl);
            if (f < 60 * 5 || f % 60) continue;
            for (auto& p : T.peds) {
                if (p.mode != 0 || p.knock > 0) continue;
                samples++;
                bool in = false;
                for (int dx = -1; dx <= 1 && !in; dx++)
                    for (int dz = -1; dz <= 1 && !in; dz++) {
                        int cell = (int)floorf(p.pos.x / 16.f + dx) * 4096 + (int)floorf(p.pos.z / 16.f + dz);
                        auto r = W.stoops.equal_range(cell);
                        for (auto it = r.first; it != r.second && !in; ++it) {
                            const World::Stoop& st = it->second;
                            if (p.pos.x <= st.x0 + 0.05f || p.pos.x >= st.x1 - 0.05f || p.pos.z <= st.z0 + 0.05f || p.pos.z >= st.z1 - 0.05f) continue;
                            float top = W.ground(p.pos.x, p.pos.z, p.pos.y + 5.f), feet = p.pos.y - p.evY;
                            if (top > feet + 0.3f) {
                                in = true;
                                if (top - feet > worst) { worst = top - feet; at = p.pos; }
                            }
                        }
                    }
                sunk += in;
            }
        }
        snprintf(b, sizeof b, "%d of %d walker samples (%d walkers) sunk into front steps; deepest %.2f m at (%.1f, %.1f)",
                 sunk, samples, walkers, worst, at.x, at.z);
        check(W.stoops.size() > 100 && sunk == 0, "Walkers go round front steps", b);
    }
    {   // the Golden Gate walkways have people on them, on the deck: not on the seabed 45 m below or walking inside
        // the approach deck (#87). Skater standing mid-span, 30 s, every walker on a deck path checked twice a second,
        // near (full update) and far (route only) alike
        Traffic T; T.init(W);
        Player pl; pl.spawn({-1100, W.ground(-1100, -1500, 1e5f), -1500}, -PI / 2);
        int tot = 0, bad = 0, near = 0;
        float worst = 0;
        for (int f = 0; f <= 60 * 30; f++) {
            if (f > 0) T.update(1.f / 60, W, pl);
            if (f % 30) continue;
            for (const Ped& p : T.peds) {
                if (p.mode != 1 || !W.pedPaths[p.path].onDeck) continue;
                float g = W.ground(p.pos.x, p.pos.z, 1e5f), d = fabsf(p.pos.y - p.evY - g);
                tot++;
                near += sq(p.pos.x - pl.pos.x) + sq(p.pos.z - pl.pos.z) < 120 * 120;
                if (d > 0.3f) { bad++; worst = std::max(worst, d); }
            }
        }
        snprintf(b, sizeof b, "%d of %d walkway samples (%d within 120 m) off the deck, worst %.1f m", bad, tot, near, worst);
        check(tot > 61 * 200 && near > 61 * 10 && bad == 0, "Golden Gate walkers stay on the deck", b);
        W.vehicleTops.clear();   // T.update published the roofs of the cars near pl
    }
    {   // Caltrain grade crossings (#60): street cars wait for the trains instead of driving straight through them.
        // Eight minutes of traffic, every grid car near a crossing against every car of every train (oriented boxes, a
        // little inside the drawn bodies); and the cars that waited for one got going again after it.
        Traffic T; T.init(W);
        Player pl; pl.spawn({0, W.ground(0, 0, 1000) + 200, 0}, 0);   // the skater out of everyone's way
        int crossings = 0, menlo = 0;
        for (int gi = 0; gi < 3; gi++)
            for (size_t k = 0; k < T.railX[gi].size(); k++)
                if (T.railX[gi][k].stop < 1e8f) { crossings++; menlo += gi == 2; }
        const CarModel& tm = carModel(CT_CALTRAIN);
        auto hit = [&](const Car& a, Vector3 c, Vector2 d) {
            const CarModel& m = carModel(a.type);
            Vector2 ax[4] = {{cosf(a.yaw), sinf(a.yaw)}, {-sinf(a.yaw), cosf(a.yaw)}, d, {-d.y, d.x}};
            Vector2 r = {c.x - a.pos.x, c.z - a.pos.z};
            for (auto& n : ax) {
                float ra = (m.halfLen - 0.15f) * fabsf(Vector2DotProduct(ax[0], n)) + (m.halfWid - 0.15f) * fabsf(Vector2DotProduct(ax[1], n));
                float rb = (tm.halfLen - 0.15f) * fabsf(Vector2DotProduct(ax[2], n)) + (tm.halfWid - 0.15f) * fabsf(Vector2DotProduct(ax[3], n));
                if (fabsf(Vector2DotProduct(r, n)) > ra + rb) return false;
            }
            return true;
        };
        std::vector<float> still(T.cars.size(), 0);
        std::vector<uint8_t> waited(T.cars.size(), 0);
        float inside = 0, longest = 0;
        int samples = 0, waits = 0;
        for (int f = 0; f < 60 * 480; f++) {
            T.update(1.f / 60, W, pl);
            for (size_t i = 0; i < T.cars.size(); i++) {
                const Car& a = T.cars[i];
                float rs;
                if (a.mode != 0 || W.railDistance(a.pos.x, a.pos.z, &rs) > 30) { still[i] = 0; continue; }
                still[i] = a.speed < 0.3f ? still[i] + 1.f / 60 : 0;
                longest = std::max(longest, still[i]);
                bool in = false, near = false;
                for (auto& t : T.trains)
                    for (int k = 0; k < 5; k++) {
                        Vector2 d;
                        Vector3 c = W.trackPoint(t.s - t.dir * (k * 25.8f + 12.9f), 2.4f * t.dir, &d);
                        float d2 = sq(c.x - a.pos.x) + sq(c.z - a.pos.z);
                        near |= d2 < 40 * 40;
                        if (d2 < 30 * 30 && hit(a, c, d)) in = true;
                    }
                if (in) { inside += 1.f / 60; samples++; }
                if (near && a.speed < 0.3f && !waited[i]) { waited[i] = 1; waits++; }
            }
        }
        snprintf(b, sizeof b, "%d grade crossings (%d in Menlo Park); car inside a train %.1f s (%d samples); %d cars waited for one, longest stop %.0f s",
                 crossings, menlo, inside, samples, waits, longest);
        check(menlo >= 2 && samples == 0 && waits > 0 && longest < 90, "Cars stop for Caltrain", b);
    }
    {   // a stopped vehicle is solid all the way to its ends: roll at 3 m/s into each end of each type, on the axis and
        // near a corner, on the runway (flat and open), and the skater stops at the body (or bails), not inside it
        int bad = 0, tot = 0;
        std::string det;
        struct { int type; const char* n; } kinds[] = {{CT_SEDAN, "car"}, {CT_BUS, "bus"}, {CT_CABLECAR, "cable car"},
                                                       {CT_STREETCAR, "streetcar"}, {CT_LRV, "LRV"}, {CT_PEDALBUS, "pedal bus"}};
        for (auto& k : kinds) {
            int type = k.type;
            const CarModel& m = carModel(type);
            float worst = 1e9f;
            for (int end = -1; end <= 1; end += 2)
                for (float lat : {0.f, m.halfWid - 0.2f}) {
                    Traffic T;
                    Car c; c.type = type; c.speed = 0; c.stopT = 1e9f; c.placed = true;
                    c.pos = {1950, W.ground(1950, 2262, 500), 2262};
                    T.cars.push_back(c);
                    Sim s; s.spawn(1950 + end * (m.halfLen + 2.5f), 2262 + lat, end > 0 ? PI : 0, 3);
                    float reach = 1e9f;
                    for (int f = 0; f < 60 * 3 && s.p.state != Player::BAIL; f++) {
                        s.run(1, Input{});
                        T.interact(s.p);
                        reach = std::min(reach, fabsf(s.p.pos.x - 1950));
                    }
                    tot++;
                    if (s.p.state != Player::BAIL && reach < m.halfLen + 0.2f) bad++;
                    if (s.p.state != Player::BAIL) worst = std::min(worst, reach - m.halfLen);
                }
            char one[64];
            snprintf(one, sizeof one, "%s%s %+.2f", det.empty() ? "" : ", ", k.n, worst);
            det += one;
        }
        snprintf(b, sizeof b, "%d of %d rides got inside; closest stop, m outside the body: %s", bad, tot, det.c_str());
        check(bad == 0, "Vehicles are solid to their ends", b);
    }
    {   // the roof of a vehicle stopped in traffic is ground: drop onto it from 1 m up at 3 and 5 m/s (along it, from
        // just inside its back end) and the skater lands on top and rides, rather than sinking into it or bailing
        int bad = 0, tot = 0;
        std::string det;
        struct { int type; const char* n; } kinds[] = {{CT_SEDAN, "car"}, {CT_BUS, "bus"}, {CT_CABLECAR, "cable car"},
                                                       {CT_STREETCAR, "streetcar"}, {CT_LRV, "LRV"}, {CT_PEDALBUS, "pedal bus"}};
        for (auto& k : kinds)
            for (float spd : {3.f, 5.f}) {
                const CarModel& m = carModel(k.type);
                Traffic T;
                Car c; c.type = k.type; c.speed = 0; c.stopT = 1e9f; c.placed = true;
                c.pos = {1950, W.ground(1950, 2262, 500), 2262};
                T.cars.push_back(c);
                float roof = c.pos.y + m.height - 0.1f;
                Sim s; s.spawn(1950 - m.halfLen + 0.3f, 2262, 0, spd);
                s.p.pos.y = roof + 1; s.p.state = Player::AIR; s.p.vel = {spd, 0, 0}; s.p.vy = 0;
                bool rode = false;
                float low = 1e9f;
                for (int f = 0; f < 60 && s.p.state != Player::BAIL; f++) {
                    T.publishRoofs(W, s.p.pos);
                    s.run(1, Input{});
                    T.interact(s.p);
                    if (fabsf(s.p.pos.x - 1950) < m.halfLen - 0.3f) low = std::min(low, s.p.pos.y);
                    if (s.p.state == Player::RIDE && fabsf(s.p.pos.y - roof) < 0.05f) rode = true;
                }
                tot++;
                bool ok = rode && s.p.state != Player::BAIL && low > roof - 0.05f;
                if (!ok) {
                    char one[96];
                    snprintf(one, sizeof one, "%s%s at %.0f m/s: %s, lowest %+.2f", det.empty() ? "" : "; ", k.n, spd,
                             s.p.state == Player::BAIL ? s.p.popup.c_str() : rode ? "rode" : "never rode on top", low - roof);
                    det += one;
                    bad++;
                }
            }
        W.vehicleTops.clear();
        snprintf(b, sizeof b, "%d of %d drops onto a stopped vehicle missed the roof%s%s", bad, tot, det.empty() ? "" : ": ", det.c_str());
        check(bad == 0, "Stopped vehicles have a roof to land on", b);
    }
    {   // the "SFO Airport" label covers the airport fill, and not the East Bay shore and hills beyond it
        int wrong = 0, land = 0, missed = 0, fill = 0;
        for (float x = AX0 + 10; x < WX1; x += 40)
            for (float z = AZ0 + 10; z < WZ1; z += 40) {
                if (W.terrain(x, z) < 0.3f) continue;
                bool sfo = !strcmp(W.district(x, z), "SFO Airport");
                if (x < AX1 + 25 && z < AZ1 + 65) { fill++; if (!sfo) missed++; }
                else { land++; if (sfo) wrong++; }
            }
        snprintf(b, sizeof b, "%d of %d airport spots not labelled SFO; %d of %d land spots beyond it labelled SFO", missed, fill, wrong, land);
        check(missed == 0 && wrong == 0, "SFO label only at the airport", b);
    }
    {   // a skater who somehow ends up outside the play area is put back, not left skating out there
        Sim s; s.spawn(1045 + 2.4f, -80, PI / 2);
        s.p.pos = {-2700, W.ground(-2700, -2327, 500), -2327}; s.p.yaw = 0;   // on the Marin hills, heading east along them
        Input in; in.push = true;
        s.run(60 * 3, in);
        snprintf(b, sizeof b, "after 3 s at x=%.0f z=%.0f (%s)", s.p.pos.x, s.p.pos.z, W.district(s.p.pos.x, s.p.pos.z));
        check(W.inPlay(s.p.pos.x, s.p.pos.z), "out of bounds puts you back", b);
    }
    {   // Union Square's skate props sit on its sloping plaza: nothing floats over it or sinks into it
        // (assumes the square fills the block gx(19)..gx(20) x gz(6)..gz(7) and every PK_PROP platform in it is one of
        // its props; the largest of them is the deck, and props under 1 m wide are the ledges)
        const float X0 = gx(19), X1 = gx(20), Z0 = gz(6), Z1 = gz(7);
        int props = 0, floating = 0, buried = 0, badLedge = 0;
        float worstGap = 0, worstSink = 0;
        const Platform* deck = nullptr;
        for (const Platform& p : W.plats) {
            if (p.kind != PK_PROP || p.x0 < X0 || p.x1 > X1 || p.z0 < Z0 || p.z1 > Z1) continue;
            props++;
            if (!deck || (p.x1 - p.x0) * (p.z1 - p.z0) > (deck->x1 - deck->x0) * (deck->z1 - deck->z0)) deck = &p;
            bool thin = std::min(p.x1 - p.x0, p.z1 - p.z0) < 1.0f, fl = false, bu = false, bl = false;
            for (float x = p.x0; x <= p.x1 + 0.01f; x += std::max(0.25f, (p.x1 - p.x0) / 40))
                for (float z = p.z0; z <= p.z1 + 0.01f; z += std::max(0.25f, (p.z1 - p.z0) / 40)) {
                    float t = W.terrain(x, z), top = p.top(x, z);
                    worstGap = std::max(worstGap, p.base - t);
                    worstSink = std::max(worstSink, t - top);
                    fl |= p.base > t + 0.05f;
                    bu |= top < t - 0.02f;
                    bl |= thin && (top - t < 0.3f || top - t > 0.9f);
                }
            floating += fl; buried += bu; badLedge += bl;
        }
        snprintf(b, sizeof b, "%d props: %d floating (worst gap %.2f m), %d buried (worst %.2f m), %d ledges not ledge-height",
                 props, floating, worstGap, buried, worstSink, badLedge);
        check(props >= 8 && floating == 0 && buried == 0 && badLedge == 0, "Union Square props on the ground", b);

        int rails = 0, badRail = 0;
        float worst = 0;
        for (const Rail& r : W.rails) {
            if (r.a.x < X0 || r.a.x > X1 || r.a.z < Z0 || r.a.z > Z1) continue;
            rails++;
            bool ledge = strcmp(r.name, "Ledge") == 0, bad = false;
            for (int k = 0; k <= 20; k++) {
                Vector3 q = Vector3Lerp(r.a, r.b, k / 20.f);
                float up = q.y - W.terrain(q.x, q.z);
                if (up < 0.3f || (ledge && up > 0.9f)) { bad = true; worst = up < 0.3f ? std::min(worst, up) : std::max(worst, up); }
            }
            badRail += bad;
        }
        snprintf(b, sizeof b, "%d rails, %d under the ground or off their ledge (worst %.2f m above the ground)", rails, badRail, worst);
        check(rails >= 8 && badRail == 0, "Union Square rails above the ground", b);

        // walk from the plaza below up the stairs and onto the deck; its uphill (west) edge meets the ground
        bool onDeck = false;
        float maxDrop = 0, westLip = 99;
        if (deck) {
            float z = (deck->z0 + deck->z1) / 2, h = W.terrain(deck->x1 + 15, z);
            for (float x = deck->x1 + 15; x >= deck->x0 - 3; x -= 0.1f) {
                float nh = W.ground(x, z, h);
                maxDrop = std::max(maxDrop, h - nh);
                h = nh;
                onDeck |= deck->contains(x, z) && fabsf(h - deck->top(x, z)) < 0.01f;
            }
            westLip = 0;
            for (float zz = deck->z0; zz <= deck->z1; zz += 0.5f) westLip = std::max(westLip, deck->top(deck->x0, zz) - W.terrain(deck->x0 - 0.05f, zz));
        }
        snprintf(b, sizeof b, "reached deck=%d, biggest drop %.2f m, west lip %.2f m above the ground", onDeck, maxDrop, westLip);
        check(onDeck && maxDrop < STEP_UP && westLip < 0.3f, "Union Square stairs reach the deck", b);

        // ride up the stairs from the plaza below, pushing, in the middle and along both edges
        int rides = 0, upOk = 0;
        std::string why;
        for (float v : {4.f, 6.f, 9.f, 12.f})
            for (int lane = 0; deck && lane < 3; lane++) {
                float z = lane == 0 ? deck->z0 + 1 : lane == 1 ? (deck->z0 + deck->z1) / 2 : deck->z1 - 1;
                Sim s; s.spawn(deck->x1 + 16, z, PI, v);
                Input in; in.push = true;
                bool bailed = false, on = false;
                s.run(60 * 8, in, [&](Player& q) {
                    bailed |= q.state == Player::BAIL;
                    on |= deck->contains(q.pos.x, q.pos.z) && fabsf(q.pos.y - deck->top(q.pos.x, q.pos.z)) < 0.05f;
                    return bailed || on;
                });
                rides++;
                if (on && !bailed) upOk++;
                else if (why.empty()) { char w[96]; snprintf(w, sizeof w, " (first miss: %.0f m/s at z %.0f, bailed=%d, stopped at x %.1f)", v, z, bailed, s.p.pos.x); why = w; }
            }
        snprintf(b, sizeof b, "%d of %d pushes up the stairs reached the deck%s", upOk, rides, why.c_str());
        check(rides == 12 && upOk == rides, "Union Square stairs ride up", b);

        // ollie onto each ledge while rolling along it
        int ledges = 0, grinds = 0;
        for (const Platform& p : W.plats) {
            if (p.kind != PK_PROP || p.x0 < X0 || p.x1 > X1 || p.z0 < Z0 || p.z1 > Z1 || std::min(p.x1 - p.x0, p.z1 - p.z0) >= 1.0f) continue;   // (ledges: under 1 m wide)
            ledges++;
            bool alongX = p.x1 - p.x0 > p.z1 - p.z0;
            float sx = alongX ? p.x0 - 6 : p.x0 + 0.04f, sz = alongX ? p.z0 + 0.04f : p.z0 - 6;
            Sim s; s.spawn(sx, sz, alongX ? 0 : PI / 2, 7);
            s.run(60, Input{}, [&](Player& q) { return alongX ? q.pos.x > p.x0 - 2.5f : q.pos.z > p.z0 - 2.5f; });
            Input in; in.ollie = true; s.run(1, in);
            grinds += s.run(90, Input{}, [](Player& q) { return q.state == Player::GRIND; });
        }
        snprintf(b, sizeof b, "%d of %d ledges grindable", grinds, ledges);
        check(ledges == 3 && grinds == ledges, "Union Square ledges grindable", b);
    }
    {   // seaplane: climbing out says why it can't (#68) -- next to a shore outside the play area it must not say "slow down"
        Seaplanes SP;
        SP.place(W);
        check(SP.moored.size() == 3, "seaplanes moored", "");
        auto tryAt = [&](float x, float z, float spd, bool water, Player& q) {
            Seaplanes t = SP;
            t.mount(0, q);
            t.pos = {x, 0, z}; t.speed = spd; t.onWater = water;
            if (!water) t.pos.y = 80;
            bool ok = t.dismount(q, W);
            return std::make_pair(ok, t);
        };
        // the moored plane's own spot: slow climbs out onto the play area, fast is told to slow down, airborne to land
        Player q;
        Vector3 m = SP.moored.empty() ? Vector3{} : SP.moored[0].pos;
        auto fast = tryAt(m.x, m.z, 12, true, q);
        auto air = tryAt(m.x, m.z, 30, false, q);
        auto slow = tryAt(m.x, m.z, 2, true, q);
        snprintf(b, sizeof b, "fast=%d/%d air=%d/%d slow=%d at %.0f,%.0f in play=%d", fast.first, fast.second.refusal, air.first,
                 air.second.refusal, slow.first, q.pos.x, q.pos.z, W.inPlay(q.pos.x, q.pos.z));
        check(!fast.first && fast.second.refusal == Seaplanes::REF_TOO_FAST && !air.first && air.second.refusal == Seaplanes::REF_FLYING &&
              slow.first && slow.second.refusal == Seaplanes::REF_NONE && W.inPlay(q.pos.x, q.pos.z), "seaplane climb-out at moorings", b);
        // all over the bay: slow, on the water, by a shore that's only outside the play area -> "can't go ashore here"
        int oob = 0, oobFast = 0, slowDown = 0, wrong = 0;
        for (float x = WX0 + 120; x < WX1 - 120; x += 40)
            for (float z = WZ0 + 120; z < WZ1 - 120; z += 40) {
                if (W.terrain(x, z) >= 0.05f || W.solidAt({x, 0.8f, z})) continue;
                Player r;
                auto res = tryAt(x, z, 2, true, r);
                if (res.first) { if (!W.inPlay(r.pos.x, r.pos.z)) wrong++; continue; }
                const char* msg = res.second.refusalMsg();
                if (strstr(msg, "Slow down")) slowDown++;   // slow already: telling them to slow down is the bug
                if (res.second.refusal != Seaplanes::REF_OUT_OF_PLAY) continue;
                oob++;
                if (!strstr(msg, "Can't go ashore")) wrong++;
                if (tryAt(x, z, 12, true, r).second.refusal == Seaplanes::REF_OUT_OF_PLAY) oobFast++;   // slowing down wouldn't help
            }
        snprintf(b, sizeof b, "%d out-of-play shores (%d fast), %d slow-down, %d wrong", oob, oobFast, slowDown, wrong);
        check(oob > 20 && oobFast == oob && slowDown == 0 && wrong == 0, "seaplane: no shore leave outside play", b);
    }
    {   // Telegraph Hill: the asphalt lies on the ground mesh all over (no grass breaking up through it on the steep
        // climb), and no street runs into Coit Tower's base
        int tris = 0, under = 0;
        float worst = 1e9f;
        for (auto& kv : W.builders) {
            const MeshBuilder& m = kv.second;
            size_t r = 0;
            for (size_t t = 0; t + 2 < m.count(); t += 3) {
                while (r < m.c.size() && m.c[r].end <= t) r++;
                if (r == m.c.size() || memcmp(m.c[r].d, &C_ASPHALT, 4)) continue;
                Vector3 P[3];
                for (int k = 0; k < 3; k++) P[k] = {m.v[3 * (t + k)], m.v[3 * (t + k) + 1], m.v[3 * (t + k) + 2]};
                float cx = (P[0].x + P[1].x + P[2].x) / 3, cz = (P[0].z + P[1].z + P[2].z) / 3;
                if (cx < 770 || cx > 990 || cz < -990 || cz > -760) continue;
                tris++;
                for (int a = 0; a <= 4; a++)
                    for (int c = 0; a + c <= 4; c++) {   // points spread over the triangle
                        float u = a / 4.f, v = c / 4.f, w0 = 1 - u - v;
                        Vector3 q = {P[0].x * w0 + P[1].x * u + P[2].x * v, P[0].y * w0 + P[1].y * u + P[2].y * v,
                                     P[0].z * w0 + P[1].z * u + P[2].z * v};
                        float d = q.y - W.terrain(q.x, q.z);
                        worst = std::min(worst, d);
                        if (d < 0.03f) under++;
                    }
            }
        }
        int intoBase = 0;   // street centerline samples within a half street width of the base block (880, -880) +-14
        for (int j = 0; j < W.sf.nj; j++)
            for (int i = 0; i < W.sf.ni; i++)
                for (int d = 0; d < 4; d += 2) {
                    if (!W.sf.nodeOk(i, j) || !W.sf.edge(i, j, d)) continue;
                    Vector2 a = W.sf.node(i, j), e = W.sf.node(i + DI[d], j + DJ[d]);
                    for (int k = 0; k <= 20; k++) {
                        float x = a.x + (e.x - a.x) * k / 20, z = a.y + (e.y - a.y) * k / 20;
                        if (fabsf(x - 880) < 14 + W.sf.roadHalf && fabsf(z + 880) < 14 + W.sf.roadHalf) intoBase++;
                    }
                }
        snprintf(b, sizeof b, "%d tris, %d samples under, lowest %.3f m; %d into Coit base", tris, under, worst, intoBase);
        check(tris > 100 && under == 0 && intoBase == 0, "Telegraph Hill road on the ground", b);
    }
    {   // City Hall: riding south from Civic Center, the front columns and the portico behind them stop the skater
        // (they were mesh only, so the skater rode through the column line at z 351 into the portico at z 352..358)
        float cx = (gx(14) + gx(15)) / 2, cz = (gz(13) + gz(14)) / 2;
        float worst = -1e9f, worstCol = -1e9f;
        for (int lane = 0; lane < 2; lane++) {   // between the middle two columns, then straight at one
            float x = lane == 0 ? cx : cx - 2.5f;
            Sim s; s.spawn(x, cz - 48, PI / 2, 6);
            float maxZ = -1e9f;
            for (int f = 0; f < 60 * 5; f++) {
                Input in; in.push = s.p.speed < 5.f && s.p.state == Player::RIDE;
                s.p.update(1.f / 60, in, W);
                maxZ = std::max(maxZ, s.p.pos.z);
            }
            if (lane == 0) worst = maxZ; else worstCol = maxZ;
        }
        snprintf(b, sizeof b, "between columns reached z %.2f (portico face %.1f), at a column z %.2f (column face %.1f)", worst, cz - 38,
                 worstCol, cz - 39.8f);
        check(worst < cz - 38 && worstCol < cz - 39.8f, "City Hall portico and columns solid", b);
    }
    {   // every Embarcadero pier deck stands out of the bay, and you can skate onto it from the promenade (issue #64:
        // the northernmost one, at z -800, was built a metre under the bay where the shore has run out past the seawall)
        int piers = 0, dry = 0, rides = 0, rode = 0;
        float worstX = 1e9f;
        for (const Platform& p : W.plats) {
            if (p.kind != PK_PIER || p.x1 != 1480 || p.z1 - p.z0 != 30) continue;
            piers++;
            float zc = (p.z0 + p.z1) * 0.5f;
            if (p.h0 > 2.f && !W.isWater(1300, zc + 14.2f, p.h0) && W.ground(1300, zc + 14.2f, p.h0 + 1) > p.h0 - 0.1f) dry++;
            for (float z : {zc - 14.2f, zc + 14.2f}) {   // the open strips of deck either side of the shed
                rides++;
                Sim s; s.spawn(1260, z, 0, 3);   // on the promenade (the Embarcadero roadway, south of its north end)
                bool bad = false;
                for (int f = 0; f < 60 * 15 && s.p.pos.x < 1305; f++) {
                    Input in; in.push = s.p.speed < 5.f && s.p.state == Player::RIDE;
                    s.p.update(1.f / 60, in, W);
                    bad |= s.p.state == Player::BAIL || s.p.splashed;
                }
                worstX = std::min(worstX, s.p.pos.x);
                if (!bad && s.p.pos.x >= 1305 && s.p.pos.y > p.h0 - 0.1f) rode++;
            }
        }
        snprintf(b, sizeof b, "%d piers, %d decks above the water, %d of %d rides from the promenade onto the deck (least x %.1f)", piers, dry,
                 rode, rides, worstX);
        check(piers == 7 && dry == piers && rode == rides, "Embarcadero piers skateable from shore", b);
    }
    {   // roads far from the origin are draped whole: strips laid in Menlo Park (El Camino Real), Oakland and out east,
        // straight and diagonal, wide and paint-thin, cover their full area on the ground (float rounding in world
        // coordinates once dropped pieces there). Runs last: it adds geometry to the world.
        struct Strip { Vector2 a, b; float half; };
        const float ecr = W.mp.node(6, 0).x;
        const Strip strips[] = {{{ecr, 8800}, {ecr, 10400}, 11}, {{ecr - 300, 9000}, {ecr + 500, 9700}, 11},
                                {{4093, 0}, {4093, 900}, 0.06f}, {{4080, 500}, {4600, 820}, 7}, {{6000, 3000}, {6300, 3000}, 0.06f},
                                {{4088.5f, 500}, {4091.1f, 500}, 0.3f}};
        int bad = 0;
        char det[96] = "";
        for (int k = 0; k < (int)(sizeof strips / sizeof strips[0]); k++) {
            const Strip& st = strips[k];
            Color mark = col(1, 2, 3 + k, M_GROUND);   // a color nothing else uses
            roadStrip(W, st.a, st.b, st.half, st.half, 0, 0.08f, mark);
            double area = 0;
            int under = 0;
            for (auto& kv : W.builders) {
                const MeshBuilder& m = kv.second;
                size_t r = 0;
                for (size_t t = 0; t + 2 < m.count(); t += 3) {
                    while (r < m.c.size() && m.c[r].end <= t) r++;
                    if (r == m.c.size() || memcmp(m.c[r].d, &mark, 4)) continue;
                    double ax = m.v[3 * t], az = m.v[3 * t + 2];
                    double ux = m.v[3 * (t + 1)] - ax, uz = m.v[3 * (t + 1) + 2] - az, vx = m.v[3 * (t + 2)] - ax, vz = m.v[3 * (t + 2) + 2] - az;
                    area += (ux * vz - vx * uz) / 2;
                    for (int q = 0; q < 3; q++)
                        if (m.v[3 * (t + q) + 1] - W.terrain(m.v[3 * (t + q)], m.v[3 * (t + q) + 2]) < 0.05f) under++;
                }
            }
            double want = Vector2Distance(st.a, st.b) * 2 * st.half;
            if (fabs(area - want) > want * 0.002 + 0.01 || under) {
                bad++;
                snprintf(det, sizeof det, " (strip %d: %.2f of %.2f m2, %d under)", k, area, want, under);
            }
        }
        snprintf(b, sizeof b, "%d of %d strips short%s", bad, (int)(sizeof strips / sizeof strips[0]), det);
        check(bad == 0, "far-off roads draped whole", b);
    }
    printf("\n%s (%d failed)\n", fails ? "SOME CHECKS FAILED" : "ALL CHECKS PASSED", fails);
    return fails ? 1 : 0;
}

