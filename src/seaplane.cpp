#include "seaplane.h"
#include <algorithm>

namespace {
constexpr float TAKEOFF_SPEED = 24.f, STALL_SPEED = 19.f, CEILING = 700.f;
// controls (full authority above ~50 mph; less when slow), lag of the flight path behind the nose, and the stunts
constexpr float PITCH_RATE = 1.0f, ROLL_RATE = 2.2f, ALIGN = 3.0f, LEVEL_DELAY = 1.0f, LEVEL_RATE = 0.8f;
constexpr float INVERT_SECS = 2.0f, STUNT_IDLE = 1.0f, BANK_AFTER = 5.0f;

bool openWater(const World& w, float x, float z) {
    return x > WX0 + 100 && x < WX1 - 100 && z > WZ0 + 100 && z < WZ1 - 100 && w.terrain(x, z) < 0.05f && !w.solidAt({x, 0.8f, z});
}

Vector3 fwd3(float yaw, float pitch) { return {cosf(pitch) * cosf(yaw), sinf(pitch), cosf(pitch) * sinf(yaw)}; }

// Two planes on sightseeing loops over the bay: center, radius, height, angular speed (negative = clockwise).
struct Loop { float cx, cz, r, h, w; Color body; };
const Loop LOOPS[] = {{700, -1700, 650, 190, 0.065f, {200, 40, 44, 255}}, {1700, 600, 850, 260, -0.05f, {240, 240, 236, 255}}};
}

Quaternion Seaplanes::poseQuat(float yaw, float pitch, float roll) {
    Quaternion qy = QuaternionFromAxisAngle({0, 1, 0}, -yaw), qz = QuaternionFromAxisAngle({0, 0, 1}, pitch), qx = QuaternionFromAxisAngle({1, 0, 0}, roll);
    return QuaternionMultiply(qy, QuaternionMultiply(qz, qx));
}

Vector3 Seaplanes::toWorld(Vector3 pos, Quaternion q, Vector3 l) { return Vector3Add(pos, Vector3RotateByQuaternion(l, q)); }

void Seaplanes::setPose(float y, float p, float r) {
    yaw = eY = y; pitch = eP = p; roll = eR = r;
    q = poseQuat(y, p, r);
    dir = Vector3RotateByQuaternion({1, 0, 0}, q);
    sagY = 0;
}

void Seaplanes::place(const World& w) {
    // the few places you'll find one: off the north waterfront by the Wharf, on the Marin shore across the Gate,
    // and on the bay by SFO. Each takes the nearest bit of shore with plenty of open water around it.
    struct Anchor { float x, z; Color body; };
    const Anchor anchors[] = {{350, -1200, {200, 40, 44, 255}}, {-650, -2550, {240, 196, 40, 255}}, {1900, 1560, {40, 110, 180, 255}}};
    for (auto& an : anchors) {
        float bd = 1e9f;
        Moor best{};
        for (float x = an.x - 700; x <= an.x + 700; x += 20)
            for (float z = an.z - 700; z <= an.z + 700; z += 20) {
                float h = w.terrain(x, z);
                if (h < 0.4f || h > 6.f || !w.inPlay(x, z)) continue;   // a shore you're allowed to climb out onto
                float d0 = sq(x - an.x) + sq(z - an.z);
                if (d0 >= bd) continue;
                for (int k = 0; k < 8; k++) {
                    float a = k * TAU_F / 8, ca = cosf(a), sa = sinf(a);
                    float d = 2;
                    while (d < 24 && !openWater(w, x + ca * d, z + sa * d)) d += 1;
                    if (d >= 24) continue;
                    float px = x + ca * (d + 11), pz = z + sa * (d + 11);
                    if (!w.inPlay(px, pz)) continue;
                    bool ok = true;   // room for the wings, and a clear run straight out for the takeoff
                    for (float r2 = 0; r2 <= 120 && ok; r2 += 20) ok = openWater(w, px + ca * r2, pz + sa * r2);
                    for (int q = 0; q < 8 && ok; q++) ok = openWater(w, px + cosf(q * TAU_F / 8) * 6, pz + sinf(q * TAU_F / 8) * 6);
                    if (!ok) continue;
                    // nose along the shore, so the plane sits side-on to whoever walks up
                    best = {{px, 0, pz}, a + PI / 2, an.body};
                    bd = d0;
                    break;
                }
            }
        if (bd < 1e8f) moored.push_back(best);
    }
}

int Seaplanes::nearest(const Vector3& p, float maxD) const {
    int b = -1;
    float bd = maxD;
    for (size_t i = 0; i < moored.size(); i++) {
        if (riding && (int)i == cur) continue;
        float d = Vector2Distance({moored[i].pos.x, moored[i].pos.z}, {p.x, p.z});
        if (d < bd && fabsf(p.y - moored[i].pos.y) < 6) { bd = d; b = (int)i; }
    }
    return b;
}

void Seaplanes::mount(int i, Player& p) {
    cur = i; riding = true;
    boardedAt = p.pos;
    pos = moored[i].pos;
    setPose(moored[i].yaw, 0, 0);
    speed = throttle = 0;
    onWater = true;
    stickIdle = pitchAcc = rollAcc = pitchIdle = rollIdle = invT = sinceStunt = 0;
    seenInvP = seenInvR = invScored = false;
    loops = rolls = inverts = 0;
    spray.clear();
    p.state = Player::RIDE; p.pos = pos; p.yaw = yaw; p.speed = 0;
    p.combo.clear(); p.comboPts = 0;
    evMount = true;
}

bool Seaplanes::dismount(Player& p, World& w) {
    refusal = REF_NONE;
    if (!onWater) { refusal = REF_FLYING; return false; }
    // nearest bit of solid shore within a short hop; note too whether there's shore here at all, just outside the
    // play area (East Bay, Peninsula, Marin), so the rider is told why they can't climb out rather than to slow down
    Vector3 best{};
    float bd = 1e9f;
    bool outShore = false;
    for (float d = 4; d <= 24; d += 1.5f)
        for (int k = 0; k < 16; k++) {
            float a = k * TAU_F / 16, x = pos.x + cosf(a) * d, z = pos.z + sinf(a) * d;
            float h = w.terrain(x, z);
            if (h < 0.5f || w.isWater(x, z, h) || w.solidAt({x, h + 1.2f, z})) continue;
            if (!w.inPlay(x, z)) { outShore = true; continue; }
            if (d < bd) { bd = d; best = {x, h, z}; }
        }
    if (bd > 1e8f) { refusal = outShore ? REF_OUT_OF_PLAY : REF_NO_SHORE; return false; }
    if (speed > 6) { refusal = REF_TOO_FAST; return false; }
    moored[cur].pos = {pos.x, 0, pos.z};
    moored[cur].yaw = yaw;
    riding = false; cur = -1;
    p.spawnAshore(w, {best.x, w.ground(best.x, best.z, best.y + 2), best.z}, atan2f(best.z - pos.z, best.x - pos.x));
    return true;
}

const char* Seaplanes::refusalMsg() const {
    switch (refusal) {
    case REF_FLYING: return "Land on the water first";
    case REF_TOO_FAST: return "Slow down to climb out";
    case REF_NO_SHORE: return "Get closer to the shore to climb out";
    case REF_OUT_OF_PLAY: return "Can't go ashore here - head back toward the city";
    default: return "";
    }
}

// Points over the whole plane, in the frame drawPlane draws it in (x forward, y up, z to the right, origin on the
// water line): wing every 0.9 m, fuselage and tail, tailplane and fin, floats, the prop disc. They are 0.4 m balls, so
// with that spacing nothing 0.14 m thick (a suspender) slips between two of them.
static const std::vector<Vector3>& planePoints() {
    static std::vector<Vector3> P;
    if (P.empty()) {
        for (float z = -5.75f; z <= 5.76f; z += 0.9f) P.push_back({0.7f, 2.88f, z});
        for (float z : {-5.75f, 5.75f}) { P.push_back({-0.15f, 2.88f, z}); P.push_back({1.55f, 2.88f, z}); }
        for (float x = -4.1f; x <= 3.3f; x += 0.9f) P.push_back({x, 2.1f, 0});
        for (float z : {-1.5f, 0.f, 1.5f}) P.push_back({-3.95f, 2.4f, z});
        P.push_back({-3.95f, 3.5f, 0});
        for (float z : {-1.4f, 1.4f}) for (float x : {-2.6f, -1.3f, 0.f, 1.3f, 2.6f, 3.2f}) P.push_back({x, 0.4f, z});
        for (float y : {1.0f, 3.1f}) P.push_back({3.3f, y, 0});
    }
    return P;
}
static constexpr float PLANE_BALL = 0.4f;
enum { HIT_AERIAL = 1, HIT_LAND = 2, HIT_SOLID = 4, HIT_WATER = 8 };

// What did the plane touch moving from one pose to another? Every sample point is swept, so a fast plane can't skip
// through a 0.9 m cable between frames, and a fast rotation is cut into short steps (about 0.2 rad each) so a wingtip
// swinging round can't tunnel into the ground or a wall. Cables, masts and towers (aerials) are tested along each
// point's path; terrain, the sea and buildings at each step's end. Water counts as touched when a point is within
// 0.7 m of the surface (the floats of a level plane at 0.3 m up), land when it's within a ball's radius of the ground.
static int sweepPlane(const World& w, Vector3 p0, Quaternion q0, Vector3 p1, Quaternion q1) {
    float d = q0.x * q1.x + q0.y * q1.y + q0.z * q1.z + q0.w * q1.w;
    if (d < 0) q1 = QuaternionScale(q1, -1.f);
    float ang = 2 * acosf(std::min(1.f, fabsf(d)));
    int n = std::clamp((int)ceilf(std::max(ang / 0.2f, Vector3Distance(p0, p1) / 3.f)), 1, 24);
    int hit = 0;
    Vector3 pa = p0;
    Quaternion qa = q0;
    for (int i = 1; i <= n; i++) {
        float t = (float)i / n;
        Vector3 pb = Vector3Lerp(p0, p1, t);
        Quaternion qb = i == n ? q1 : QuaternionNlerp(q0, q1, t);
        for (const Vector3& l : planePoints()) {
            Vector3 A = Seaplanes::toWorld(pa, qa, l), B = Seaplanes::toWorld(pb, qb, l);
            if (w.aerialHit(A, B, PLANE_BALL)) hit |= HIT_AERIAL;
            float gp = w.terrain(B.x, B.z);
            if (gp < 0.15f) { if (B.y <= 0.7f) hit |= HIT_WATER; }
            else if (B.y < gp + PLANE_BALL) hit |= HIT_LAND;
            if (w.solidAt(B)) hit |= HIT_SOLID;
        }
        pa = pb; qa = qb;
    }
    return hit;
}

// Did any part of the plane touch an aerial obstacle between two poses?
static bool planeHitsAerial(const World& w, Vector3 p0, Quaternion q0, Vector3 p1, Quaternion q1) {
    for (const Vector3& l : planePoints())
        if (w.aerialHit(Seaplanes::toWorld(p0, q0, l), Seaplanes::toWorld(p1, q1, l), PLANE_BALL)) return true;
    return false;
}

float Seaplanes::altitude(const World& w) const { return pos.y - std::max(w.terrain(pos.x, pos.z), 0.f); }

void Seaplanes::update(float dt, const Input& in, bool climb, bool dive, Player& p, World& w) {
    evBanked = false;
    if (!onWater && (yaw != eY || pitch != eP || roll != eR)) setPose(yaw, pitch, roll);   // someone set the pose by hand
    Vector3 pos0 = pos; Quaternion q0 = onWater ? poseQuat(yaw, pitch, roll) : q;   // last frame's pose, for the sweep
    // throttle stays where you leave it: W opens it up, S closes it
    if (in.push) throttle = std::min(1.f, throttle + 0.7f * dt);
    if (in.brake) throttle = std::max(0.f, throttle - 0.9f * dt);
    prop += (8.f + 60.f * throttle) * dt;
    float thrust = throttle * 15.f;
    float drag = 0.0036f * speed * speed + (onWater ? 0.22f * speed + 0.6f : 0.f);
    if (!onWater) {   // pulling the nose away from the flight path costs speed
        float aoa = acosf(clampf(Vector3DotProduct(dir, nose()), -1.f, 1.f));
        drag += 1.5f * aoa * aoa * speed;
    }
    speed += (thrust - drag - 9.8f * (onWater ? sinf(pitch) : dir.y)) * dt;
    speed = std::max(speed, 0.f);

    if (onWater) {
        // taxiing: the rudder and water rudders turn it, slowly at a crawl; it rides up onto the step as it speeds up
        yaw = wrapPi(yaw + in.steer * 0.8f * clampf((speed + 2) / 10.f, 0.25f, 1.f) * dt);
        roll = lerpf(roll, 0, 1 - expf(-dt * 4));
        float planing = 0.05f * clampf(speed / TAKEOFF_SPEED, 0, 1);
        if (climb && speed > TAKEOFF_SPEED) pitch += 0.45f * dt;    // rotate and lift off
        else pitch = lerpf(pitch, planing, 1 - expf(-dt * 3));
        Vector3 f = fwd3(yaw, pitch);
        Vector3 np = Vector3Add(pos, Vector3Scale(f, speed * dt));
        np.y = std::max(np.y, 0.f);
        if (np.y < 0.4f && pitch < 0.08f) np.y = 0;   // still on the floats until it's rotated for takeoff
        if ((!openWater(w, np.x, np.z) && np.y < 0.4f) ||
            planeHitsAerial(w, pos0, q0, np, poseQuat(yaw, pitch, roll))) {      // ran into the shore, a pier or a piling
            if (speed > 8) evCrash = true;
            speed = 0;
            np = pos;
        }
        pos = np;
        if (pos.y >= 0.4f) { onWater = false; setPose(yaw, pitch, roll); stickIdle = 0; sinceStunt = 0; }   // airborne: from here the attitude is q
        // spray off the floats
        static RNG sr(52);
        if (speed > 5)
            for (int k = 0; k < 3; k++) {
                float side = (k & 1) ? 1.4f : -1.4f;
                Vector3 b = {pos.x - cosf(yaw) * 1.5f - sinf(yaw) * side, 0.2f, pos.z - sinf(yaw) * 1.5f + cosf(yaw) * side};
                spray.push_back({b, {-cosf(yaw) * speed * 0.2f + sr.range(-1, 1), sr.range(1.5f, 3.f) + speed * 0.05f, -sinf(yaw) * speed * 0.2f + sr.range(-1, 1)}, sr.range(0.5f, 0.9f)});
            }
    } else {
        // flying, in any attitude. Space pitches the nose up and Shift down, A D roll, all about the plane's own axes, so
        // holding one keeps going round: a loop, a barrel roll. Banking turns it (the lift tilts with the wings), and
        // it only levels itself off gently, after the stick has been let go for a second.
        const Vector3 UP = {0, 1, 0};
        float pin = (climb ? 1.f : 0.f) - (dive ? 1.f : 0.f), rin = fabsf(in.steer) > 0.05f ? in.steer : 0.f;
        stickIdle = (pin != 0 || rin != 0) ? 0.f : stickIdle + dt;
        float auth = clampf(speed / 22.f, 0.3f, 1.f);                       // slow: the controls go soft
        float dPitch = pin * PITCH_RATE * auth * dt, dRoll = rin * ROLL_RATE * auth * dt;
        q = QuaternionMultiply(q, QuaternionMultiply(QuaternionFromAxisAngle({1, 0, 0}, dRoll), QuaternionFromAxisAngle({0, 0, 1}, dPitch)));
        auto turnWorld = [&](Quaternion r) { q = QuaternionMultiply(r, q); dir = Vector3RotateByQuaternion(dir, r); };
        // banking: the lift's sideways part swings the heading round (upside down, "up" pulls toward the ground)
        Vector3 f = nose(), u = up();
        float hl = sqrtf(f.x * f.x + f.z * f.z), hdg = hl > 0.25f ? atan2f(f.z, f.x) : yaw;
        float side = clampf(u.x * -sinf(hdg) + u.z * cosf(hdg), -1.f, 1.f);
        turnWorld(QuaternionFromAxisAngle(UP, -side * 9.8f / std::max(speed, 15.f) * 1.3f * dt));
        // the gentle return to wings-level and upright: proportional to how far the plane's up is from the sky's, so it
        // fades toward upside down (no tug-of-war with a held inverted flight) and only starts once the stick is idle
        if (stickIdle > LEVEL_DELAY) {
            Vector3 ax = Vector3CrossProduct(up(), UP);
            float m = Vector3Length(ax);
            if (m > 1e-4f) q = QuaternionMultiply(QuaternionFromAxisAngle(Vector3Scale(ax, 1.f / m), m * LEVEL_RATE * smooth01(LEVEL_DELAY, LEVEL_DELAY + 0.8f, stickIdle) * dt), q);
        }
        // nose falls toward the ground: a stall at any attitude, and the ceiling
        auto noseDown = [&](float rate) {
            Vector3 fw = nose(), ax = Vector3CrossProduct(fw, {0, -1, 0});
            float m = Vector3Length(ax);
            if (m > 0.05f) q = QuaternionMultiply(QuaternionFromAxisAngle(Vector3Scale(ax, 1.f / m), rate * std::max(m, 0.35f) * dt), q);
            else if (fw.y > 0) q = QuaternionMultiply(q, QuaternionFromAxisAngle({0, 0, 1}, -rate * 0.35f * dt));   // straight up: tip over
        };
        if (speed < STALL_SPEED) noseDown((STALL_SPEED - speed) * 0.09f);
        if (pos.y > CEILING) noseDown(1.0f);
        q = QuaternionNormalize(q);
        // the flight path follows the nose with some lag
        f = nose();
        dir = Vector3Normalize(Vector3Add(dir, Vector3Scale(Vector3Subtract(f, dir), 1 - expf(-dt * ALIGN * clampf(speed / STALL_SPEED, 0.4f, 1.f)))));
        if (pos.y > CEILING && dir.y > 0) dir = Vector3Normalize({dir.x, 0, dir.z});
        // the wings carry the weight: fully upright above stall speed, 70% upside down, little when slow
        float sup = std::min(1.f, sq(speed / STALL_SPEED)) * (0.85f + 0.15f * upY());
        sagY += (-(1.f - sup) * 9.8f - sagY * 0.8f) * dt;
        pos = Vector3Add(pos, Vector3Scale(Vector3Add(Vector3Scale(dir, speed), {0, sagY, 0}), dt));
        // the edge of the map turns you round
        if (pos.x < WX0 + 150 || pos.x > WX1 - 150 || pos.z < WZ0 + 150 || pos.z > WZ1 - 150) {
            float toC = atan2f((WZ0 + WZ1) * 0.5f - pos.z, (WX0 + WX1) * 0.5f - pos.x);
            Vector3 fw = nose();
            if (fw.x * fw.x + fw.z * fw.z > 0.06f)
                turnWorld(QuaternionFromAxisAngle(UP, -wrapPi(toC - atan2f(fw.z, fw.x)) * (1 - expf(-dt * 0.8f))));
            pos.x = clampf(pos.x, WX0 + 100, WX1 - 100);
            pos.z = clampf(pos.z, WZ0 + 100, WZ1 - 100);
        }
        q = QuaternionNormalize(q);
        // heading, elevation and bank, for the camera, HUD and landing rules
        {
            Vector3 fw = nose(), rt = Vector3RotateByQuaternion({0, 0, 1}, q), uu = up();
            if (fw.x * fw.x + fw.z * fw.z > 4e-4f) yaw = atan2f(fw.z, fw.x);
            pitch = asinf(clampf(fw.y, -1.f, 1.f));
            roll = atan2f(-rt.y, uu.y);
            eY = yaw; eP = pitch; eR = roll;
        }
        float g = w.terrain(pos.x, pos.z);
        bool water = g < 0.15f;
        int hit = sweepPlane(w, pos0, q0, pos, q);
        if (hit & (HIT_AERIAL | HIT_LAND | HIT_SOLID)) evCrash = true;                     // cables, towers, the ground, buildings: any part of the plane
        else if ((!water && pos.y < g + 0.8) || w.solidAt({pos.x, pos.y + 1.6f, pos.z})) evCrash = true;
        else if (hit & HIT_WATER) {
            float vy = dir.y * speed + sagY;
            if (water && pitch > -0.3f && fabsf(roll) < 0.5f && speed < 48.f && vy > -14.f) {   // a landing: upright, level, gentle
                onWater = true; pos.y = 0; pitch = 0; roll = 0; speed *= 0.75f; evSplash = true;
                if (!p.combo.empty()) { p.landNote = ""; p.bankCombo(); evBanked = true; }
            } else evCrash = true;     // upside down, steeply banked or diving, or a wing in the sea
        }
        if (!onWater && !evCrash) stunts(dt, pin, rin, dPitch, dRoll, p, w);
    }
    for (auto& s : spray) { s.life -= dt; s.vel.y -= 9.f * dt; s.pos = Vector3Add(s.pos, Vector3Scale(s.vel, dt)); }
    spray.erase(std::remove_if(spray.begin(), spray.end(), [](const Spray& s) { return s.life <= 0 || s.pos.y < 0; }), spray.end());

    if (evCrash && !onWater) {
        // wreck: the plane is towed back to its mooring and you're back where you climbed in
        Moor& m = moored[cur];
        (void)m;
        riding = false; cur = -1;
        p.spawn(boardedAt, yaw);
        p.say("CRASHED!", {255, 90, 60, 255}, "The plane's been towed back to its mooring");
        return;
    }
    p.pos = pos; p.yaw = yaw; p.speed = speed; p.vy = 0; p.vel = {};
    p.state = Player::RIDE;
    p.safeT = 0;
}

// Progress of the stunts while airborne, scored like the skater's tricks (chained into the combo, banked on touchdown or
// once nothing new has happened for a while; a crash loses them). A loop is the nose pitched through 360 degrees with the
// plane going upside down on the way, a barrel roll the same about the roll axis, inverted flight is held upside down
// for INVERT_SECS (then it keeps earning). Low passes (under 30 m) score half again.
void Seaplanes::stunts(float dt, float pin, float rin, float dPitch, float dRoll, Player& p, const World& w) {
    float uy = upY();
    bool low = altitude(w) < 30.f;
    auto track = [&](float& acc, float& idle, bool& seen, float d, float input, const char* name, float pts, int& count) {
        if (input != 0) {
            idle = 0;
            if (acc * d < 0) { acc = 0; seen = false; }     // reversed: start over
            acc += d;
        } else if ((idle += dt) > STUNT_IDLE) { acc = 0; seen = false; }
        if (uy < -0.3f) seen = true;
        if (fabsf(acc) >= TAU_F && seen) {
            acc -= copysignf(TAU_F, acc);
            seen = false;
            count++;
            sinceStunt = 0;
            std::string n = low ? std::string("Low ") + name : name;
            p.addTrick(n, low ? pts * 1.5f : pts);
            p.say(n + "!", {255, 205, 60, 255}, "x" + std::to_string(p.combo.size()) + " combo - land it to bank");
        }
    };
    track(pitchAcc, pitchIdle, seenInvP, dPitch, pin, "Loop", 600, loops);
    track(rollAcc, rollIdle, seenInvR, dRoll, rin, "Barrel Roll", 500, rolls);
    if (uy < -0.4f) {
        invT += dt;
        if (invT >= INVERT_SECS) {
            if (!invScored) {
                invScored = true; inverts++; sinceStunt = 0;
                p.addTrick(low ? "Low Inverted" : "Inverted", low ? 450.f : 300.f);
                p.say("Inverted!", {255, 205, 60, 255}, "x" + std::to_string(p.combo.size()) + " combo - keep it up");
            } else p.comboPts += (low ? 180.f : 120.f) * dt;
        }
    } else if (uy > 0.2f) { invT = 0; invScored = false; }
    bool busy = fabsf(pitchAcc) > 0.5f || fabsf(rollAcc) > 0.5f || invT > 0;
    if (p.combo.empty() || busy) sinceStunt = 0;
    else if ((sinceStunt += dt) > BANK_AFTER) { p.landNote = "stunts banked"; p.bankCombo(); evBanked = true; sinceStunt = 0; }
}

// A floatplane: high wing on struts, two floats, a radial nose and a spinning prop. pos is the water line.
static void drawPlane(Vector3 pos, Quaternion att, float prop, Color body, bool people, int seed) {
    Color wht = {238, 238, 234, 255}, dark = {40, 42, 48, 255}, glass = {120, 160, 190, 255}, strut = {150, 152, 158, 255};
    rlPushMatrix();
    rlTranslatef(pos.x, pos.y, pos.z);
    Vector3 ax; float an;
    QuaternionToAxisAngle(att, &ax, &an);
    rlRotatef(an * RAD2DEG, ax.x, ax.y, ax.z);    // yaw about Y, then pitch about z, then roll about x (positive: right wing down), or any attitude
    for (int s = -1; s <= 1; s += 2) {
        DrawCube({0, 0.35f, s * 1.4f}, 5.4f, 0.5f, 0.55f, wht);                     // float
        rlPushMatrix();
        rlTranslatef(2.85f, 0.45f, s * 1.4f);
        rlRotatef(20, 0, 0, 1);
        DrawCube({0, 0, 0}, 0.7f, 0.32f, 0.5f, wht);                                // float bow, turned up
        rlPopMatrix();
        DrawCube({0, 0.62f, s * 1.4f}, 5.0f, 0.06f, 0.6f, body);                    // stripe
        DrawCylinderEx({1.2f, 0.6f, s * 1.4f}, {1.0f, 1.6f, s * 0.45f}, 0.05f, 0.05f, 4, strut);
        DrawCylinderEx({-1.0f, 0.6f, s * 1.4f}, {-0.8f, 1.6f, s * 0.45f}, 0.05f, 0.05f, 4, strut);
        DrawCylinderEx({0.6f, 1.7f, s * 0.5f}, {0.7f, 2.82f, s * 2.6f}, 0.04f, 0.04f, 4, strut);  // wing strut
        DrawCube({1.1f, 2.38f, s * 0.56f}, 1.5f, 0.5f, 0.02f, glass);              // side windows
        DrawCube({-0.4f, 2.38f, s * 0.56f}, 1.0f, 0.42f, 0.02f, glass);
    }
    DrawCube({0.3f, 2.1f, 0}, 5.0f, 1.15f, 1.1f, body);                              // fuselage
    DrawCube({-2.9f, 2.3f, 0}, 2.4f, 0.7f, 0.6f, body);                              // tail boom
    DrawCube({2.3f, 2.5f, 0}, 0.1f, 0.5f, 0.9f, glass);                              // windshield
    DrawCube({2.95f, 2.05f, 0}, 0.5f, 0.95f, 0.95f, dark);                           // cowling
    DrawCube({0.7f, 2.85f, 0}, 1.7f, 0.12f, 11.5f, wht);                             // wing
    DrawCube({0.7f, 2.92f, 0}, 1.72f, 0.04f, 11.52f, body);
    DrawCube({-3.95f, 3.0f, 0}, 1.0f, 1.4f, 0.1f, body);                             // fin
    DrawCube({-3.95f, 2.4f, 0}, 0.9f, 0.08f, 3.0f, wht);                             // tailplane
    // the prop: blades when it's turning slowly, a blur disc when it's going
    rlPushMatrix();
    rlTranslatef(3.28f, 2.05f, 0);
    DrawCube({0, 0, 0}, 0.2f, 0.22f, 0.22f, dark);
    rlRotatef(prop * RAD2DEG, 1, 0, 0);
    DrawCube({0.05f, 0, 0}, 0.05f, 2.3f, 0.16f, {60, 60, 64, 255});
    rlPopMatrix();
    if (people) {   // pilot and a couple of passengers at the windows
        static const Color SKIN[] = {{240, 205, 175, 255}, {214, 170, 134, 255}, {170, 120, 85, 255}, {120, 80, 55, 255}};
        DrawSphereEx({1.5f, 2.42f, 0.22f}, 0.17f, 5, 6, SKIN[seed % 4]);
        DrawCube({1.5f, 2.6f, 0.22f}, 0.32f, 0.08f, 0.32f, {200, 40, 40, 255});
        DrawSphereEx({0.4f, 2.42f, -0.22f}, 0.17f, 5, 6, SKIN[(seed + 1) % 4]);
        DrawSphereEx({-0.4f, 2.42f, 0.22f}, 0.17f, 5, 6, SKIN[(seed + 2) % 4]);
    }
    rlPopMatrix();
}

void Seaplanes::draw(float now, const Vector3& cam) const {
    for (size_t i = 0; i < moored.size(); i++) {
        bool me = riding && (int)i == cur;
        if (me) { drawPlane(pos, onWater ? poseQuat(yaw, pitch, roll) : q, prop, moored[i].body, true, (int)i); continue; }
        if (Vector3Distance(moored[i].pos, cam) > 900) continue;
        float bob = 0.05f * sinf(now * 1.7f + i);
        drawPlane({moored[i].pos.x, bob, moored[i].pos.z}, poseQuat(moored[i].yaw, 0, 0.015f * sinf(now * 1.3f + i)), 0.3f, moored[i].body, false, (int)i);
    }
    for (int k = 0; k < 2; k++) {   // sightseeing flights going round and round
        const Loop& L = LOOPS[k];
        float a = now * L.w + k * 2.f;
        Vector3 p = {L.cx + cosf(a) * L.r, L.h + 12 * sinf(now * 0.3f + k), L.cz + sinf(a) * L.r};
        float yaw = a + (L.w > 0 ? PI / 2 : -PI / 2);
        float bank = (L.w > 0 ? 1.f : -1.f) * 0.32f;
        drawPlane(p, poseQuat(yaw, 0.02f, bank), now * 50.f, L.body, true, 7 + k);
    }
}

void Seaplanes::drawFx() const {
    for (auto& s : spray) {
        float a = clampf(s.life / 0.7f, 0, 1);
        DrawSphereEx(s.pos, 0.07f + 0.06f * (1 - a), 4, 4, {240, 248, 255, (unsigned char)(170 * a)});
    }
}
