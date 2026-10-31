#include "jetski.h"
#include <algorithm>

static const Color HULL[5] = {{240, 196, 40, 255}, {220, 60, 50, 255}, {50, 130, 210, 255}, {60, 190, 150, 255}, {236, 236, 240, 255}};

// open water inside the playable area: moorings and rides both stay where the skater is allowed to be
static bool openWater(const World& w, float x, float z) {
    return w.inPlay(x, z) && x > WX0 + 80 && x < WX1 - 80 && z > WZ0 + 80 && z < WZ1 - 80 && w.terrain(x, z) < 0.05f && !w.solidAt({x, 0.7f, z}, false, true);
}

void JetSkis::place(const World& w) {
    // candidate spots: open water a few meters off a shoreline, found by scanning for land beside water
    struct Cand { Vector3 p; float yaw; };
    std::vector<Cand> cands;
    for (float x = WX0 + 100; x < WX1 - 100; x += 24)
        for (float z = WZ0 + 100; z < WZ1 - 100; z += 24) {
            float h = w.terrain(x, z);
            if (h < 0.4f || h > 5.f) continue;
            for (int k = 0; k < 8; k++) {
                float a = k * TAU_F / 8, ca = cosf(a), sa = sinf(a);
                // walk out from the land to the first open water, and moor just beyond the edge so it's a short step away
                float d = 2;
                while (d < 24 && !openWater(w, x + ca * d, z + sa * d)) d += 1;
                if (d >= 24) continue;
                float wx = x + ca * (d + 2.2f), wz = z + sa * (d + 2.2f);
                if (!openWater(w, wx, wz) || !openWater(w, wx + 3.5f, wz) || !openWater(w, wx - 3.5f, wz) || !openWater(w, wx, wz + 3.5f) || !openWater(w, wx, wz - 3.5f)) continue;
                cands.push_back({{wx, 0, wz}, a});
                break;
            }
        }
    RNG r(1977);
    for (size_t i = cands.size(); i > 1; i--) std::swap(cands[i - 1], cands[r.next() % i]);
    // a handful, spread well apart so you have to go looking
    for (auto& c : cands) {
        if (skis.size() >= 9) break;
        bool ok = true;
        for (auto& s : skis) if (Vector2Distance({s.pos.x, s.pos.z}, {c.p.x, c.p.z}) < 650) { ok = false; break; }
        if (ok) skis.push_back({c.p, c.yaw + PI + r.range(-0.5f, 0.5f), (int)(skis.size() % 5)});
    }
}

int JetSkis::nearest(const Vector3& p, float maxD) const {
    int b = -1;
    float bd = maxD;
    for (size_t i = 0; i < skis.size(); i++) {
        if (riding && (int)i == cur) continue;
        float d = Vector2Distance({skis[i].pos.x, skis[i].pos.z}, {p.x, p.z});
        if (d < bd && fabsf(p.y - skis[i].pos.y) < 5) { bd = d; b = (int)i; }
    }
    return b;
}

void JetSkis::mount(int i, Player& p) {
    cur = i; riding = true;
    yaw = skis[i].yaw; speed = 0; vel = {}; lean = pitch = throttle = 0;
    spray.clear(); trail.clear();
    p.state = Player::RIDE;
    p.pos = skis[i].pos; p.yaw = yaw; p.speed = 0;
    p.combo.clear(); p.comboPts = 0;
    evMount = true;
}

bool JetSkis::dismount(Player& p, World& w) {
    // nearest bit of solid shore within a short hop
    Vector3 best{};
    float bd = 1e9f;
    for (float d = 3; d <= 16; d += 1.5f)
        for (int k = 0; k < 16; k++) {
            float a = k * TAU_F / 16, x = p.pos.x + cosf(a) * d, z = p.pos.z + sinf(a) * d;
            float h = w.terrain(x, z);
            if (h < 0.5f || !w.inPlay(x, z) || w.isWater(x, z, h) || w.solidAt({x, h + 1.2f, z})) continue;
            if (d < bd) { bd = d; best = {x, h, z}; }
        }
    if (bd > 1e8f) return false;
    skis[cur] = {{p.pos.x, 0, p.pos.z}, yaw, skis[cur].color};
    riding = false; cur = -1; speed = 0;
    p.spawn({best.x, w.ground(best.x, best.z, best.y + 2), best.z}, atan2f(best.z - p.pos.z, best.x - p.pos.x));
    return true;
}

void JetSkis::update(float dt, const Input& in, bool boost, Player& p, const World& w) {
    bob += dt;
    hitCD -= dt;
    float thr = in.push ? 1.f : 0.f;
    throttle = lerpf(throttle, thr, 1 - expf(-dt * 4));
    float fwdX = cosf(yaw), fwdZ = sinf(yaw);
    float vf = vel.x * fwdX + vel.z * fwdZ, vl = -vel.x * fwdZ + vel.z * fwdX;   // along the ski, and sideways
    float acc = in.push ? (boost ? 18.f : 12.f) : 0.f;
    if (in.brake) acc = vf > 0 ? -16.f : -3.f;
    vf += (acc - 0.45f * vf - (in.push ? 0.f : 0.6f) * (vf > 0 ? 1.f : -1.f)) * dt;
    if (vf < -4) vf = -4;
    vl *= expf(-dt * 2.4f);                                                      // the hull bites: little sideways slide
    // steering needs thrust: a jet ski turns by pushing water, so it turns tighter with some speed
    float turn = in.steer * 1.7f * clampf((fabsf(vf) + 2.5f) / 11.f, 0.2f, 1.f) / (1.f + fabsf(vf) / 38.f);
    yaw = wrapPi(yaw + turn * dt * (vf < 0 ? -1.f : 1.f));
    fwdX = cosf(yaw); fwdZ = sinf(yaw);
    vel = {fwdX * vf - fwdZ * vl, 0, fwdZ * vf + fwdX * vl};
    // move, staying on open water (shore, piers, piers' pilings and bridge towers all stop you)
    Vector3 np = {p.pos.x + vel.x * dt, 0, p.pos.z + vel.z * dt};
    if (!openWater(w, np.x, np.z)) {
        bool xOK = openWater(w, np.x, p.pos.z), zOK = openWater(w, p.pos.x, np.z);
        float hit;   // speed into whatever stopped us, so scraping along a shore or the edge of the map stays quiet
        if (xOK) { hit = fabsf(vel.z); np.z = p.pos.z; vel.z *= -0.3f; }
        else if (zOK) { hit = fabsf(vel.x); np.x = p.pos.x; vel.x *= -0.3f; }
        else { hit = sqrtf(vel.x * vel.x + vel.z * vel.z); np.x = p.pos.x; np.z = p.pos.z; vel.x *= -0.3f; vel.z *= -0.3f; }
        if (hit > 6 && hitCD <= 0) { evCrash = true; hitCD = 0.6f; }
    }
    // the visible speed along the ski
    speed = vel.x * fwdX + vel.z * fwdZ;
    p.pos = {np.x, 0, np.z};
    skis[cur].pos = p.pos; skis[cur].yaw = yaw;
    p.yaw = yaw; p.speed = speed; p.vy = 0; p.vel = {};
    float tl = -in.steer * clampf(fabsf(speed) / 18.f, 0.15f, 1.f) * 0.4f;
    lean = lerpf(lean, tl, 1 - expf(-dt * 5));
    pitch = lerpf(pitch, clampf(speed / 22.f, 0, 1) * 0.16f - (in.brake ? 0.05f : 0), 1 - expf(-dt * 3));
    p.safePos = {np.x, 0, np.z}; p.safeT = 0;
    // wake and spray
    static RNG sr(31);
    if (speed > 4) {
        Vector3 back = {np.x - fwdX * 1.3f, 0.05f, np.z - fwdZ * 1.3f};
        for (int k = 0; k < 3; k++) {
            float side = (k & 1) ? 1.f : -1.f;
            Vector3 v = {-fwdX * speed * 0.15f - fwdZ * side * sr.range(1, 3), sr.range(1.5f, 3.5f) + speed * 0.06f, -fwdZ * speed * 0.15f + fwdX * side * sr.range(1, 3)};
            spray.push_back({{back.x + fwdZ * side * 0.3f, 0.2f, back.z - fwdX * side * 0.3f}, v, sr.range(0.5f, 0.9f)});
        }
        if (trail.empty() || Vector3Distance(trail.back().pos, back) > 1.4f) trail.push_back({back, 0});
    }
    for (auto& s : spray) { s.life -= dt; s.vel.y -= 9.f * dt; s.pos = Vector3Add(s.pos, Vector3Scale(s.vel, dt)); }
    spray.erase(std::remove_if(spray.begin(), spray.end(), [](const Spray& s) { return s.life <= 0 || s.pos.y < 0; }), spray.end());
    for (auto& f : trail) f.age += dt;
    trail.erase(std::remove_if(trail.begin(), trail.end(), [](const Foam& f) { return f.age > 5.f; }), trail.end());
}

static void drawSki(const Vector3& pos, float yaw, float lean, float pitch, float bobT, int color, bool rider, float steerLean) {
    Color body = HULL[color % 5], dark = {34, 36, 42, 255}, trim = {240, 240, 244, 255};
    float bobY = 0.05f + 0.04f * sinf(bobT * 2.1f + pos.x * 0.3f);
    rlPushMatrix();
    rlTranslatef(pos.x, pos.y + bobY, pos.z);
    rlRotatef(-yaw * RAD2DEG, 0, 1, 0);
    rlRotatef(-pitch * RAD2DEG, 0, 0, 1);
    rlRotatef(lean * RAD2DEG, 1, 0, 0);
    // hull: a dark underside, the colored deck, a sloped bow and a flat stern
    DrawCube({-0.05f, 0.2f, 0}, 2.5f, 0.3f, 0.78f, dark);
    DrawCube({-0.1f, 0.46f, 0}, 2.35f, 0.34f, 0.82f, body);
    rlPushMatrix();
    rlTranslatef(1.25f, 0.46f, 0);
    rlRotatef(-14, 0, 0, 1);
    DrawCube({0, 0, 0}, 0.9f, 0.3f, 0.74f, body);
    rlPopMatrix();
    DrawCube({1.62f, 0.3f, 0}, 0.4f, 0.2f, 0.5f, dark);
    for (int s = -1; s <= 1; s += 2) {
        DrawCube({-0.1f, 0.5f, s * 0.42f}, 2.2f, 0.07f, 0.05f, trim);       // side stripe
        DrawCube({-0.55f, 0.66f, s * 0.46f}, 1.0f, 0.06f, 0.22f, dark);    // foot rail
    }
    DrawCube({-0.55f, 0.69f, 0}, 1.1f, 0.16f, 0.32f, dark);                 // seat
    DrawCube({0.45f, 0.73f, 0}, 0.5f, 0.26f, 0.5f, body);                   // console
    DrawCylinderEx({0.45f, 0.84f, 0}, {0.5f, 1.12f, 0}, 0.04f, 0.04f, 6, dark); // steering column
    DrawCube({0.5f, 1.14f, 0}, 0.06f, 0.05f, 0.62f, dark);                  // handlebar
    DrawCube({0.72f, 0.95f, 0}, 0.04f, 0.3f, 0.34f, {150, 200, 230, 150});  // little windshield
    DrawCylinderEx({-1.25f, 0.3f, 0}, {-1.5f, 0.3f, 0}, 0.13f, 0.1f, 8, {120, 124, 132, 255}); // jet nozzle
    if (rider) {
        Color skin = {214, 170, 134, 255}, vest = {235, 120, 40, 255}, pants = {40, 60, 120, 255}, cap = {200, 40, 40, 255};
        Vector3 hip = {-0.5f, 0.82f, 0}, shoulder = {-0.05f, 1.48f, 0};
        shoulder.z += steerLean * 0.15f;
        for (int s = -1; s <= 1; s += 2) {
            DrawCylinderEx({hip.x, hip.y, s * 0.14f}, {0.05f, 0.82f, s * 0.38f}, 0.08f, 0.07f, 6, pants);   // thigh
            DrawCylinderEx({0.05f, 0.82f, s * 0.38f}, {-0.2f, 0.7f, s * 0.46f}, 0.065f, 0.06f, 6, pants);   // shin to the foot rail
            DrawCylinderEx({shoulder.x + 0.05f, shoulder.y - 0.05f, s * 0.2f}, {0.5f, 1.14f, s * 0.28f}, 0.05f, 0.045f, 6, skin); // arm to the bar
        }
        DrawCylinderEx(hip, shoulder, 0.17f, 0.18f, 8, vest);
        DrawSphereEx({shoulder.x + 0.12f, shoulder.y + 0.24f, shoulder.z}, 0.13f, 6, 8, skin);
        DrawCube({shoulder.x + 0.12f, shoulder.y + 0.35f, shoulder.z}, 0.26f, 0.08f, 0.26f, cap);
    }
    rlPopMatrix();
}

void JetSkis::draw(float now, const Vector3& cam) const {
    for (size_t i = 0; i < skis.size(); i++) {
        if (Vector3Distance(skis[i].pos, cam) > 450) continue;
        bool me = riding && (int)i == cur;
        drawSki(skis[i].pos, skis[i].yaw, me ? lean : 0, me ? pitch : 0, now, skis[i].color, me, me ? lean * 2.5f : 0);
    }
}

void JetSkis::drawFx(const Vector3& cam) const {
    for (auto& f : trail) {   // fading foam patches left behind
        float a = clampf(1.f - f.age / 5.f, 0, 1);
        float r = 0.6f + f.age * 0.45f;
        DrawCylinder({f.pos.x, 0.04f, f.pos.z}, r, r, 0.01f, 10, {235, 245, 250, (unsigned char)(60 * a)});
    }
    for (auto& s : spray) {
        float a = clampf(s.life / 0.7f, 0, 1);
        DrawSphereEx(s.pos, 0.05f + 0.05f * (1 - a), 4, 4, {240, 248, 255, (unsigned char)(170 * a)});
    }
}
