#include "seaplane.h"
#include <algorithm>

namespace {
constexpr float TAKEOFF_SPEED = 24.f, STALL_SPEED = 19.f, CEILING = 700.f;

bool openWater(const World& w, float x, float z) {
    return x > WX0 + 100 && x < WX1 - 100 && z > WZ0 + 100 && z < WZ1 - 100 && w.terrain(x, z) < 0.05f && !w.solidAt({x, 0.8f, z});
}

Vector3 fwd3(float yaw, float pitch) { return {cosf(pitch) * cosf(yaw), sinf(pitch), cosf(pitch) * sinf(yaw)}; }

// Two planes on sightseeing loops over the bay: center, radius, height, angular speed (negative = clockwise).
struct Loop { float cx, cz, r, h, w; Color body; };
const Loop LOOPS[] = {{700, -1700, 650, 190, 0.065f, {200, 40, 44, 255}}, {1700, 600, 850, 260, -0.05f, {240, 240, 236, 255}}};
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
    pos = moored[i].pos; yaw = moored[i].yaw;
    pitch = roll = speed = throttle = 0;
    onWater = true;
    spray.clear();
    p.state = Player::RIDE; p.pos = pos; p.yaw = yaw; p.speed = 0;
    p.combo.clear(); p.comboPts = 0;
    evMount = true;
}

bool Seaplanes::dismount(Player& p, World& w) {
    if (!onWater || speed > 6) return false;
    Vector3 best{};
    float bd = 1e9f;
    for (float d = 4; d <= 24; d += 1.5f)
        for (int k = 0; k < 16; k++) {
            float a = k * TAU_F / 16, x = pos.x + cosf(a) * d, z = pos.z + sinf(a) * d;
            float h = w.terrain(x, z);
            if (h < 0.5f || !w.inPlay(x, z) || w.isWater(x, z, h) || w.solidAt({x, h + 1.2f, z})) continue;
            if (d < bd) { bd = d; best = {x, h, z}; }
        }
    if (bd > 1e8f) return false;
    moored[cur].pos = {pos.x, 0, pos.z};
    moored[cur].yaw = yaw;
    riding = false; cur = -1;
    p.spawn({best.x, w.ground(best.x, best.z, best.y + 2), best.z}, atan2f(best.z - pos.z, best.x - pos.x));
    return true;
}

float Seaplanes::altitude(const World& w) const { return pos.y - std::max(w.terrain(pos.x, pos.z), 0.f); }

void Seaplanes::update(float dt, const Input& in, bool climb, bool dive, Player& p, World& w) {
    // throttle stays where you leave it: W opens it up, S closes it
    if (in.push) throttle = std::min(1.f, throttle + 0.7f * dt);
    if (in.brake) throttle = std::max(0.f, throttle - 0.9f * dt);
    prop += (8.f + 60.f * throttle) * dt;
    float thrust = throttle * 15.f;
    float drag = 0.0036f * speed * speed + (onWater ? 0.22f * speed + 0.6f : 0.f);
    speed += (thrust - drag - 9.8f * sinf(pitch)) * dt;
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
        if (!openWater(w, np.x, np.z) && np.y < 0.4f) {      // ran into the shore, a pier or a piling
            if (speed > 8) evCrash = true;
            speed = 0;
            np = pos;
        }
        pos = np;
        if (pos.y >= 0.4f) onWater = false;
        // spray off the floats
        static RNG sr(52);
        if (speed > 5)
            for (int k = 0; k < 3; k++) {
                float side = (k & 1) ? 1.4f : -1.4f;
                Vector3 b = {pos.x - cosf(yaw) * 1.5f - sinf(yaw) * side, 0.2f, pos.z - sinf(yaw) * 1.5f + cosf(yaw) * side};
                spray.push_back({b, {-cosf(yaw) * speed * 0.2f + sr.range(-1, 1), sr.range(1.5f, 3.f) + speed * 0.05f, -sinf(yaw) * speed * 0.2f + sr.range(-1, 1)}, sr.range(0.5f, 0.9f)});
            }
    } else {
        // flying: bank to turn, Space to climb, Shift to dive, and it levels itself off when you let go
        roll = lerpf(roll, in.steer * 0.75f, 1 - expf(-dt * 2.5f));
        if (climb) pitch += 0.6f * dt;
        else if (dive) pitch -= 0.6f * dt;
        else pitch = lerpf(pitch, 0.f, 1 - expf(-dt * 0.6f));
        if (speed < STALL_SPEED) pitch -= (STALL_SPEED - speed) * 0.08f * dt;   // too slow: the nose drops
        pitch = clampf(pitch, -0.7f, 0.55f);
        if (pos.y > CEILING) pitch = std::min(pitch, 0.f);
        yaw = wrapPi(yaw + sinf(roll) * 9.8f / std::max(speed, 15.f) * 1.3f * dt);
        Vector3 f = fwd3(yaw, pitch);
        pos = Vector3Add(pos, Vector3Scale(f, speed * dt));
        if (speed < STALL_SPEED) pos.y -= (STALL_SPEED - speed) * 0.4f * dt;
        // the edge of the map turns you round
        if (pos.x < WX0 + 150 || pos.x > WX1 - 150 || pos.z < WZ0 + 150 || pos.z > WZ1 - 150) {
            float toC = atan2f((WZ0 + WZ1) * 0.5f - pos.z, (WX0 + WX1) * 0.5f - pos.x);
            yaw = lerpAngle(yaw, toC, 1 - expf(-dt * 0.8f));
            pos.x = clampf(pos.x, WX0 + 100, WX1 - 100);
            pos.z = clampf(pos.z, WZ0 + 100, WZ1 - 100);
        }
        float g = w.terrain(pos.x, pos.z);
        bool water = g < 0.15f;
        if (water && pos.y <= 0.3f) {
            if (pitch > -0.3f && fabsf(roll) < 0.5f && speed < 48.f) {   // a landing
                onWater = true; pos.y = 0; pitch = 0; roll = 0; speed *= 0.75f; evSplash = true;
            } else evCrash = true;
        } else if ((!water && pos.y < g + 0.8f) || w.solidAt({pos.x, pos.y + 1.6f, pos.z})) evCrash = true;
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

// A floatplane: high wing on struts, two floats, a radial nose and a spinning prop. pos is the water line.
static void drawPlane(Vector3 pos, float yaw, float pitch, float roll, float prop, Color body, bool people, int seed) {
    Color wht = {238, 238, 234, 255}, dark = {40, 42, 48, 255}, glass = {120, 160, 190, 255}, strut = {150, 152, 158, 255};
    rlPushMatrix();
    rlTranslatef(pos.x, pos.y, pos.z);
    rlRotatef(-yaw * RAD2DEG, 0, 1, 0);
    rlRotatef(pitch * RAD2DEG, 0, 0, 1);
    rlRotatef(roll * RAD2DEG, 1, 0, 0);    // positive roll: right wing down, for a right turn
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
        if (me) { drawPlane(pos, yaw, pitch, roll, prop, moored[i].body, true, (int)i); continue; }
        if (Vector3Distance(moored[i].pos, cam) > 900) continue;
        float bob = 0.05f * sinf(now * 1.7f + i);
        drawPlane({moored[i].pos.x, bob, moored[i].pos.z}, moored[i].yaw, 0, 0.015f * sinf(now * 1.3f + i), 0.3f, moored[i].body, false, (int)i);
    }
    for (int k = 0; k < 2; k++) {   // sightseeing flights going round and round
        const Loop& L = LOOPS[k];
        float a = now * L.w + k * 2.f;
        Vector3 p = {L.cx + cosf(a) * L.r, L.h + 12 * sinf(now * 0.3f + k), L.cz + sinf(a) * L.r};
        float yaw = a + (L.w > 0 ? PI / 2 : -PI / 2);
        float bank = (L.w > 0 ? 1.f : -1.f) * 0.32f;
        drawPlane(p, yaw, 0.02f, bank, now * 50.f, L.body, true, 7 + k);
    }
}

void Seaplanes::drawFx() const {
    for (auto& s : spray) {
        float a = clampf(s.life / 0.7f, 0, 1);
        DrawSphereEx(s.pos, 0.07f + 0.06f * (1 - a), 4, 4, {240, 248, 255, (unsigned char)(170 * a)});
    }
}
