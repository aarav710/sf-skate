#include "player.h"
#include <cstdio>

static const Color C_GOLD = {255, 205, 60, 255};
static const Color C_RED = {255, 80, 70, 255};

// how fast the ground under a board at p rolling at speed along f rises (+) or falls (-), m/s
static float groundRate(World& w, Vector3 p, Vector3 f, float speed) {
    float hF = w.ground(p.x + f.x * 0.5f, p.z + f.z * 0.5f, p.y + 0.2f);
    float hB = w.ground(p.x - f.x * 0.5f, p.z - f.z * 0.5f, p.y + 0.2f);
    return clampf(speed * (hF - hB), -30, 30);
}

void Player::spawn(Vector3 p, float y) {
    pos = p; yaw = y; speed = 0; vel = {0, 0, 0}; vy = 0; rideVy = 0;
    state = RIDE; flipT = shoveT = -1; flipAngle = shoveAngle = 0; combo.clear(); comboPts = 0;
    rail = -1; splashed = false; airClip = false; bailT = 0; pitch = 0;
    safePos = p; safeYaw = y;
}

void Player::addTrick(const std::string& n, float pts) {
    combo.push_back(n);
    comboPts += pts;
}

void Player::bankCombo() {
    if (combo.empty()) return;
    int mult = (int)combo.size();
    long long total = (long long)(comboPts * mult);
    score += total;
    bestCombo = std::max(bestCombo, (int)total);
    std::string s;
    for (size_t i = 0; i < combo.size(); i++) {
        if (i) s += " + ";
        s += combo[i];
        if (s.size() > 70) { s += " ..."; break; }
    }
    char buf[64];
    if (mult > 1) snprintf(buf, sizeof buf, "+%lld  (x%d)", total, mult);
    else snprintf(buf, sizeof buf, "+%lld", total);
    if (!landNote.empty()) s += "   " + landNote;
    say(buf, C_GOLD, s);
    combo.clear();
    comboPts = 0;
}

void Player::bail(const char* why) {
    if (state == BAIL) return;
    airClip = false;
    Vector3 f = state == AIR ? vel : Vector3Scale(fwd(), speed);
    if (state == AIR) {
        float hv = sqrtf(vel.x * vel.x + vel.z * vel.z);
        if (hv > 0.5f) yaw = atan2f(vel.z, vel.x);
        speed = hv * 0.6f;
    }
    if (state == GRIND) vy = 0;
    state = BAIL;
    bailT = 0;
    boardPos = pos;
    boardVel = Vector3Add(Vector3Scale(f, 0.8f), Vector3Scale(fwd(), 1.5f)); // kick along the heading, not world +x
    boardVel.y = 3.0f;
    if (Vector3Length(boardVel) < 0.1f) boardVel = Vector3Scale(fwd(), 2);
    boardYaw = yaw;
    say("BAILED!", C_RED, why);
    combo.clear();
    comboPts = 0;
    rail = -1;
    evBail = true;
}

void Player::ollie(float strength) {
    vel = Vector3Scale(fwd(), speed);
    vy = strength;
    state = AIR;
    airTime = 0;
    spinAccum = 0;
    grabbed = false;
    grabT = 0;
    addTrick("Ollie", 100);
    evPop = true;
}

void Player::update(float dt, const Input& in, World& w) {
    popupT -= dt;
    evPop = evLand = evBail = evSplash = evGrindStart = false;
    Vector3 before = pos;
    if (state != AIR) bouncing = ollieQueued = false;
    switch (state) {
    case RIDE: ride(dt, in, w); break;
    case AIR: air(dt, in, w); break;
    case GRIND: grind(dt, in, w); break;
    case BAIL: bailUpdate(dt, w); break;
    }
    // keep inside the playable world
    if (!w.inPlay(pos.x, pos.z) && w.inPlay(before.x, before.z)) {
        pos.x = before.x; pos.z = before.z;
        if (state == RIDE) speed *= 0.5f;
        if (state == AIR) vel = Vector3Scale(vel, 0.3f);
    }
    // remember the last safe spot for respawns after a splash
    safeT += dt;
    if (state == RIDE && safeT > 0.5f && w.terrain(pos.x, pos.z) > 1.0f && fabsf(speed) < 20) {
        safeT = 0;
        safePos = pos;
        safeYaw = yaw;
    }
    if (state == RIDE && w.onBridge(pos.x, pos.z) && safeT > 0.5f) {
        safeT = 0;
        safePos = pos;
        safeYaw = yaw;
    }
}

void Player::ride(float dt, const Input& in, World& w) {
    float spd = fabsf(speed);
    float turnRate = spd < 0.6f ? 2.4f : lerpf(2.6f, 1.1f, clampf(spd / 34.f, 0, 1));
    yaw = wrapPi(yaw + in.steer * turnRate * dt);
    lean = lerpf(lean, in.steer * clampf(spd / 6.f, 0, 1), 1 - expf(-dt * 8));

    Vector3 f = fwd();
    float hF = w.ground(pos.x + f.x * 0.7f, pos.z + f.z * 0.7f, pos.y + 0.2f);
    float hB = w.ground(pos.x - f.x * 0.7f, pos.z - f.z * 0.7f, pos.y + 0.2f);
    float slope = clampf((hF - hB) / 1.4f, -1.5f, 1.5f);
    pitch = lerpf(pitch, atanf(slope), 1 - expf(-dt * 14));
    float acc = -9.8f * slope / sqrtf(1 + slope * slope);
    bool pushing = in.push && speed < 17.f && speed > -1.f;
    if (pushing) acc += 8.0f;
    pushAmt = lerpf(pushAmt, pushing ? 1.f : 0.f, 1 - expf(-dt * 6));
    if (pushAmt > 0.05f) pushPhase += dt * 5.5f;
    if (in.brake) {
        if (fabsf(speed) < 0.5f) speed = 0;
        else acc += speed > 0 ? -8.f : 8.f;
    }
    float drag = (in.tuck ? 0.0010f : 0.0026f) * speed * speed + 0.15f;
    speed += acc * dt;
    if (fabsf(speed) > 0.01f) speed -= copysignf(std::min(drag * dt, fabsf(speed)), speed);
    speed = clampf(speed, -9, 40);
    crouch = lerpf(crouch, in.tuck ? 0.85f : (in.ollieHeld ? 0.6f : 0.12f), 1 - expf(-dt * 10));

    Vector3 np = Vector3Add(pos, Vector3Scale(f, speed * dt));
    Vector2 n;
    if (w.collide(np, 0.35f, pos.y, 1.7f, &n)) {
        Vector3 v = Vector3Scale(f, speed);
        float into = -(v.x * n.x + v.z * n.y);
        if (into > 8.5f) { pos = np; bail("Slammed into a wall"); return; }
        if (into > 0) {
            v.x += n.x * into; v.z += n.y * into;
            float s = sqrtf(v.x * v.x + v.z * v.z);
            if (s > 0.5f) {
                float ny = atan2f(v.z, v.x);
                if (speed < 0) ny = wrapPi(ny + PI);
                yaw = lerpAngle(yaw, ny, 1 - perFrame60(0.5f, dt));
            }
            speed = copysignf(s * perFrame60(0.85f, dt), speed);   // scraping along a wall
        }
    }
    float g = w.ground(np.x, np.z, pos.y);
    float predicted = pos.y + rideVy * dt - 0.5f * GRAVITY * dt * dt;
    if (g < predicted - 0.06f && fabsf(speed) > 1.5f) {
        // the ground dropped away: we're flying (ramp lip, stairs, crest)
        pos = np;
        pos.y = predicted;
        vel = Vector3Scale(f, speed);
        vy = rideVy;
        state = AIR;
        airTime = 0;
        spinAccum = 0;
        grabbed = false;
        if (speed < 0) { yaw = wrapPi(yaw + PI); }
        return;
    }
    Vector3 f2 = fwd();
    float hF2 = w.ground(np.x + f2.x * 0.5f, np.z + f2.z * 0.5f, g + 0.2f);
    float hB2 = w.ground(np.x - f2.x * 0.5f, np.z - f2.z * 0.5f, g + 0.2f);
    rideVy = clampf(speed * (hF2 - hB2), -30, 30);
    pos = np;
    pos.y = g;

    if (w.isWater(pos.x, pos.z, pos.y)) {
        splashed = true;
        evSplash = true;
        bail(("Splash! Into the " + std::string(w.waterKind(pos.x, pos.z, pos.y))).c_str());
        return;
    }
    if (in.ollie && fabsf(speed) < 40) {
        if (speed < 0) { yaw = wrapPi(yaw + PI); speed = -speed; } // pop out of fakie rolling
        ollie(7.4f + clampf(fabsf(speed) * 0.03f, 0, 0.6f));
        // going uphill, pop off the slope rather than off flat ground: keep the climb the board already had,
        // or a fast ollie up a steep hill leaves the skater inside the rising hillside
        vy += std::max(rideVy, 0.f);
    }
}

void Player::air(float dt, const Input& in, World& w) {
    airTime += dt;
    // bounced off an edge into a real drop: controls back. Hills are fine: the ground is expected to keep its slope under the hop
    if (bouncing && w.ground(pos.x, pos.z, pos.y + 0.2f) < bounceY + bounceRate * airTime - 1.0f) bouncing = ollieQueued = false;
    pitch = lerpf(pitch, 0.f, 1 - expf(-dt * 3));
    float prevY = pos.y;
    vy -= GRAVITY * dt;
    pos.x += vel.x * dt;
    pos.z += vel.z * dt;
    pos.y += vy * dt;
    // bump your head on ceilings (inside houses, under bridge decks and overhangs)
    float cl = w.ceiling(pos.x, pos.z, prevY);
    if (pos.y + 1.75f > cl) {
        pos.y = cl - 1.75f;
        if (vy > 0) vy = -vy * 0.25f;
    }
    float spin = bouncing ? 0.f : in.steer * 6.0f * dt;
    yaw = wrapPi(yaw + spin);
    spinAccum += spin;
    crouch = lerpf(crouch, in.grab ? 1.0f : 0.55f, 1 - expf(-dt * 10));

    // flip tricks
    if (flipT < 0 && shoveT < 0 && !bouncing) {
        if (in.kickflip) { flipT = 0; flipDir = 1; addTrick("Kickflip", 500); }
        else if (in.heelflip) { flipT = 0; flipDir = -1; addTrick("Heelflip", 500); }
        else if (in.shove) { shoveT = 0; addTrick("Pop Shove-it", 400); }
    }
    if (flipT >= 0) {
        flipT += dt;
        flipAngle = flipDir * 360.f * clampf(flipT / flipDur, 0, 1);
        if (flipT >= flipDur) { flipT = -1; flipAngle = 0; }
    }
    if (shoveT >= 0) {
        shoveT += dt;
        shoveAngle = 180.f * clampf(shoveT / shoveDur, 0, 1);
        if (shoveT >= shoveDur) { shoveT = -1; shoveAngle = 0; }
    }
    if (in.grab && flipT < 0 && shoveT < 0 && !bouncing) {
        if (!grabbed) { grabbed = true; addTrick("Indy Grab", 300); }
        grabT += dt;
        comboPts += 500 * dt;
    }
    if (airTime > 1.1f) comboPts += 350 * dt;

    // walls
    Vector2 n;
    float hitSize;
    if (w.collide(pos, 0.35f, pos.y, 1.7f, &n, &hitSize)) {
        bool pole = hitSize < 1.2f;
        float into = -(vel.x * n.x + vel.z * n.y);
        if (into > 9.f) { bail(pole ? "Hit a pole" : "Flew into a wall"); return; }
        if (into > 0) {
            float before = atan2f(vel.z, vel.x);
            vel.x += n.x * into; vel.z += n.y * into;
            float turn = wrapPi(atan2f(vel.z, vel.x) - before);
            // slamming a wall at a steep angle is a crash; brushing a pole or glancing off a wall just knocks you aside
            if (!pole && into > 2.f && fabsf(turn) > 0.7f) { bail("Flew into a wall"); return; }
            // turn the board with the deflection (keeping any spin/fakie offset), or the landing reads it as sideways
            if (vel.x * vel.x + vel.z * vel.z > 1.f) yaw = wrapPi(yaw + turn);
            airClip = true;
        }
    }

    // grind snapping: falling onto a rail or ledge edge
    if (vy <= 0) {
        float hv = sqrtf(vel.x * vel.x + vel.z * vel.z);
        for (int i = 0; i < (int)w.rails.size() && hv > 1.5f; i++) {
            const Rail& r = w.rails[i];
            if (fabsf(pos.x - r.a.x) > 400 && fabsf(pos.x - r.b.x) > 400) continue;
            float t;
            float d = distToSeg2D(pos.x, pos.z, r.a.x, r.a.z, r.b.x, r.b.z, &t);
            if (d > 0.6f || t <= 0.001f || t >= 0.999f) continue;
            float ry = lerpf(r.a.y, r.b.y, t);
            if (prevY < ry - 0.12f || pos.y > ry + 0.2f) continue;
            Vector2 rd = Vector2Normalize({r.b.x - r.a.x, r.b.z - r.a.z});
            float al = (vel.x * rd.x + vel.z * rd.y) / hv;
            if (fabsf(al) < 0.35f) continue;
            if (flipT >= 0 || shoveT >= 0) continue; // finish the flip first
            rail = i;
            railT = t;
            railDir = al > 0 ? 1 : -1;
            speed = std::max(3.5f, hv * fabsf(al));
            Vector2 bf = {cosf(yaw), sinf(yaw)};
            boardslide = fabsf(bf.x * rd.x + bf.y * rd.y) < 0.6f;
            balance = 0;
            balanceVel = (GetRandomValue(0, 1) ? 1 : -1) * 0.3f;
            state = GRIND;
            airClip = false;
            std::string nm = boardslide ? "Boardslide" : "50-50";
            if (r.name) nm += std::string(" (") + r.name + ")";
            addTrick(nm, 250);
            if (airTime > 0.6f && spinAccum != 0) {
                int halves = (int)roundf(fabsf(spinAccum) / PI);
                if (halves > 0) addTrick(std::string(spinAccum > 0 ? "FS " : "BS ") + std::to_string(halves * 180), 300.f * halves);
            }
            pos = {lerpf(r.a.x, r.b.x, t), ry, lerpf(r.a.z, r.b.z, t)};
            evGrindStart = true;
            return;
        }
    }

    // landing
    float g = w.ground(pos.x, pos.z, std::max(prevY, pos.y));
    if (pos.y <= g && vy <= 0) {
        bool pop = bouncing && (ollieQueued || in.ollie); // Space during a landing bounce pops as it touches down
        bouncing = ollieQueued = false;
        land(w, g);
        if (pop && (state == RIDE || bouncing)) { // (on a steep downhill the touchdown may start another bounce: pop anyway)
            bouncing = false;
            float up = groundRate(w, pos, fwd(), speed);
            ollie(7.4f + clampf(fabsf(speed) * 0.03f, 0, 0.6f));
            vy += std::max(up, 0.f); // uphill: keep the climb, as ride() does
        }
    }
    else if (pos.y < g) pos.y = g;   // still rising into a steeper hillside: skim along it rather than through it
    if (bouncing && in.ollie) ollieQueued = true;
}

void Player::land(World& w, float g) {
    pos.y = g;
    if (w.isWater(pos.x, pos.z, g)) {
        splashed = true;
        evSplash = true;
        bail(("Splash! Into the " + std::string(w.waterKind(pos.x, pos.z, g))).c_str());
        return;
    }
    if (flipT >= 0 && flipT < flipDur * 0.8f) { bail("Didn't finish the flip"); return; }
    if (shoveT >= 0 && shoveT < shoveDur * 0.75f) { bail("Didn't finish the shove-it"); return; }
    if (vy < -21.f) { bail("Way too much drop"); return; }
    float hv = sqrtf(vel.x * vel.x + vel.z * vel.z);
    float diff = hv > 1.f ? wrapPi(yaw - atan2f(vel.z, vel.x)) : 0.f;
    bool fakie = false;
    if (fabsf(diff) > 0.65f) {
        if (fabsf(fabsf(diff) - PI) < 0.65f) { fakie = true; yaw = wrapPi(yaw + PI); diff = wrapPi(yaw - atan2f(vel.z, vel.x)); }
        else { bail("Landed sideways"); return; }
    }
    flipT = shoveT = -1;
    flipAngle = shoveAngle = 0;
    // how clean was it? lined up and gentle is a stomp, crooked or from a height is sketchy
    float impact = -vy;
    float quality = 1.f - clampf(fabsf(fabsf(diff) > PI / 2 ? fabsf(diff) - PI : diff) / 0.5f, 0.f, 1.f) * 0.7f - clampf((impact - 9.f) / 12.f, 0.f, 1.f) * 0.5f;
    bool stomp = quality > 0.85f && impact > 3.f && !airClip, sketchy = quality < 0.45f;
    airClip = false;
    // spins and Big Air are part of the combo too, so add them before the landing scales it
    int halves = (int)roundf(fabsf(spinAccum) / PI);
    if (halves > 0) addTrick(std::string(spinAccum > 0 ? "FS " : "BS ") + std::to_string(halves * 180), 300.f * halves);
    if (airTime > 1.6f) addTrick("Big Air", 400);
    landNote.clear();
    if (stomp) { comboPts *= 1.15f; landNote = "STOMPED +15%"; }
    else if (sketchy) { comboPts *= 0.8f; landNote = "sketchy landing -20%"; }
    landShake = clampf(impact / 18.f, 0.f, 1.f);
    if (fakie && !combo.empty()) combo.back() += " to Fakie";
    speed = hv * cosf(diff) * (sketchy ? 0.78f : 0.97f);
    rideVy = 0;
    state = RIDE;
    crouch = clampf(0.75f + impact / 40.f, 0.75f, 1.f);
    evLand = true;
    bool onlyOllie = combo.size() == 1 && combo[0] == "Ollie";
    if (onlyOllie) {
        score += stomp ? 150 : 100;
        if (stomp) say("Stomped!", C_GOLD);
        combo.clear(); comboPts = 0;
    } else bankCombo();
    landNote.clear();
    // the trucks and knees take the hit, then spring back: a real landing bounces
    if (impact > 4.5f && state == RIDE) {
        float bv = clampf(impact * (stomp ? 0.3f : 0.22f), 1.2f, 3.4f);
        vel = Vector3Scale(fwd(), speed);
        vy = bv;
        state = AIR;
        airTime = 0;
        spinAccum = 0;
        grabbed = false;
        bouncing = true;
        bounceY = g;
        bounceRate = groundRate(w, pos, fwd(), speed);
    }
}

void Player::grind(float dt, const Input& in, World& w) {
    const Rail& r = w.rails[rail];
    Vector3 d = Vector3Subtract(r.b, r.a);
    float Lxz = sqrtf(d.x * d.x + d.z * d.z);
    Vector2 dir = {d.x / Lxz, d.z / Lxz};
    float slope = d.y / Lxz * railDir;
    pitch = lerpf(pitch, atanf(slope), 1 - expf(-dt * 14));
    speed += (-9.8f * slope * 0.8f - 0.7f) * dt;
    comboPts += 300 * dt;
    crouch = lerpf(crouch, 0.5f, 1 - expf(-dt * 8));
    // balance: tap the opposite direction to stay on (D pushes balance, and the HUD marker, right)
    balanceVel += ((GetRandomValue(-100, 100) / 100.f) * 2.4f + balance * 2.4f + in.steer * 4.2f) * dt;
    balance += balanceVel * dt;
    lean = balance * 0.8f;
    if (fabsf(balance) > 1.f) { bail("Lost balance on the rail"); return; }
    if (speed < 1.0f) {
        state = AIR; vel = {dir.x * railDir * 1.5f, 0, dir.y * railDir * 1.5f}; vy = 0; airTime = 0; spinAccum = 0;
        pos.x += -dir.y * 0.6f;
        return;
    }
    if (!boardslide) yaw = atan2f(dir.y * railDir, dir.x * railDir);
    railT += railDir * speed * dt / Lxz;
    if (railT >= 1.f || railT <= 0.f) {
        railT = clampf(railT, 0, 1);
        pos = Vector3Lerp(r.a, r.b, railT);
        state = AIR;
        vel = {dir.x * railDir * speed, 0, dir.y * railDir * speed};
        vy = slope * speed + 1.0f;
        airTime = 0; spinAccum = 0; grabbed = false;
        if (boardslide) yaw = atan2f(vel.z, vel.x) + 0.0f;
        return;
    }
    pos = Vector3Lerp(r.a, r.b, railT);
    if (in.ollie) {
        state = AIR;
        Vector2 right = {-dir.y, dir.x};
        float side = in.steer != 0 ? in.steer : (balance > 0 ? 1.f : -1.f);
        vel = {dir.x * railDir * speed + right.x * side * 3.0f, 0, dir.y * railDir * speed + right.y * side * 3.0f};
        vy = 6.5f;
        if (boardslide) yaw = atan2f(vel.z, vel.x);
        airTime = 0; spinAccum = 0; grabbed = false;
        pos.y += 0.05f;
        evPop = true;
    }
}

void Player::bailUpdate(float dt, World& w) {
    bailT += dt;
    // board tumbles away
    boardVel.y -= GRAVITY * dt;
    boardPos = Vector3Add(boardPos, Vector3Scale(boardVel, dt));
    float bg = w.ground(boardPos.x, boardPos.z, boardPos.y + 0.3f);
    if (boardPos.y < bg) { boardPos.y = bg; boardVel = Vector3Scale(boardVel, 0.6f); boardVel.y = fabsf(boardVel.y) * 0.3f; }
    boardYaw += dt * 6 * clampf(Vector3Length(boardVel) / 5, 0, 1);
    // rider slides and falls
    speed *= expf(-dt * 3);
    if (!splashed) {
        Vector3 np = Vector3Add(pos, Vector3Scale(fwd(), speed * dt));
        Vector2 n;
        w.collide(np, 0.35f, pos.y, 1.7f, &n);
        float g = w.ground(np.x, np.z, pos.y);
        vy -= GRAVITY * dt;
        np.y = pos.y + vy * dt;
        if (np.y < g) { np.y = g; vy = 0; }
        pos = np;
        if (w.isWater(pos.x, pos.z, pos.y)) { splashed = true; evSplash = true; }
    } else {
        pos.y = std::max(pos.y - dt * 2, -1.5f);
    }
    if (bailT > (splashed ? 1.6f : 1.8f)) {
        if (splashed) {
            spawn(safePos, safeYaw);
        } else {
            state = RIDE;
            speed = 0;
            vy = 0;
            rideVy = 0;
            flipT = shoveT = -1;
            flipAngle = shoveAngle = 0;
        }
    }
}

// ---------------------------------------------------------------- drawing
static void limb(Vector3 a, Vector3 b, float r, Color c) { DrawCylinderEx(a, b, r, r * 0.9f, 6, c); }

void Player::draw() const {
    Color skin = {214, 170, 134, 255}, shirt = {40, 110, 190, 255}, pants = {50, 52, 60, 255};
    Color shoe = {235, 235, 235, 255}, cap = {200, 40, 40, 255};
    Color deck = {30, 30, 32, 255}, deckBottom = {210, 120, 40, 255}, wheel = {240, 238, 225, 255}, truck = {150, 150, 155, 255};

    auto drawBoard = [&]() {
        DrawCube({0, 0.11f, 0}, 0.82f, 0.03f, 0.21f, deck);
        DrawCube({0, 0.093f, 0}, 0.8f, 0.01f, 0.2f, deckBottom);
        DrawCube({0.37f, 0.125f, 0}, 0.1f, 0.03f, 0.2f, deck); // kick tails
        DrawCube({-0.37f, 0.125f, 0}, 0.1f, 0.03f, 0.2f, deck);
        for (int i = -1; i <= 1; i += 2) {
            DrawCube({i * 0.26f, 0.075f, 0}, 0.05f, 0.03f, 0.18f, truck);
            for (int s = -1; s <= 1; s += 2) DrawCube({i * 0.26f, 0.035f, s * 0.1f}, 0.06f, 0.06f, 0.04f, wheel);
        }
    };

    if (state == BAIL) {
        // board on its own
        rlPushMatrix();
        rlTranslatef(boardPos.x, boardPos.y + 0.03f, boardPos.z);
        rlRotatef(-boardYaw * RAD2DEG, 0, 1, 0);
        rlRotatef(bailT * 400.f * (bailT < 0.6f ? 1.f : 0.f), 1, 0, 0);
        drawBoard();
        rlPopMatrix();
        if (splashed && bailT > 0.3f) return;
        // rider tumbling to the ground
        float fall = clampf(bailT / 0.35f, 0, 1);
        rlPushMatrix();
        rlTranslatef(pos.x, pos.y + 0.05f, pos.z);
        rlRotatef(-yaw * RAD2DEG, 0, 1, 0);
        rlRotatef(-85.f * fall, 0, 0, 1);
        limb({0, 0, -0.12f}, {0, 0.9f, -0.1f}, 0.07f, pants);
        limb({0, 0, 0.12f}, {0.1f, 0.85f, 0.1f}, 0.07f, pants);
        limb({0, 0.9f, 0}, {0, 1.45f, 0}, 0.16f, shirt);
        limb({0, 1.35f, -0.2f}, {0.3f, 1.0f, -0.5f}, 0.05f, shirt);
        limb({0, 1.35f, 0.2f}, {-0.2f, 1.1f, 0.55f}, 0.05f, shirt);
        DrawSphereEx({0, 1.65f, 0}, 0.13f, 6, 8, skin);
        DrawCube({0, 1.76f, 0}, 0.26f, 0.08f, 0.26f, cap);
        rlPopMatrix();
        return;
    }

    rlPushMatrix();
    rlTranslatef(pos.x, pos.y + 0.04f, pos.z);
    rlRotatef(-yaw * RAD2DEG, 0, 1, 0);
    rlRotatef(lean * -10.f, 1, 0, 0);

    float lift = (flipT >= 0 || shoveT >= 0) ? 0.25f * sinf(PI * clampf((flipT >= 0 ? flipT / flipDur : shoveT / shoveDur), 0, 1)) : 0.f;
    // board (with flip / shove-it rotation)
    rlPushMatrix();
    rlRotatef(pitch * RAD2DEG, 0, 0, 1);   // the board lies along the slope; the rider stays upright over it
    rlTranslatef(0, 0.1f + lift * 0.4f, 0);
    rlRotatef(shoveAngle, 0, 1, 0);
    rlRotatef(flipAngle, 1, 0, 0);
    rlTranslatef(0, -0.1f, 0);
    drawBoard();
    rlPopMatrix();

    // rider: stands sideways, facing the board's right side (+z)
    float c = crouch;
    float hipY = 0.98f - 0.38f * c + lift;
    float footY = 0.13f + lift;
    Vector3 hipF = {0.1f, hipY, 0}, hipB = {-0.1f, hipY, 0};
    Vector3 footF = {0.24f, footY, 0.0f}, footB = {-0.22f, footY, 0.0f};
    if (pushAmt > 0.05f && state == RIDE) {
        // back foot pushes along the ground
        float ph = sinf(pushPhase);
        footB = {lerpf(-0.22f, -0.35f + ph * 0.35f, pushAmt), lerpf(footY, 0.02f + std::max(0.f, -ph) * 0.12f, pushAmt), lerpf(0.f, 0.18f, pushAmt)};
    }
    // on a slope the feet ride up or down with the board (and the pushing foot with the ground)
    float tp = tanf(pitch);
    footF.y += footF.x * tp;
    footB.y += footB.x * tp;
    // legs keep their length: a foot that would land out of reach (the pushing foot on a steep climb reached a
    // metre below the hip) stops at full stretch, and the knee bends forward by however much the leg is folded
    auto legIK = [](Vector3 hip, Vector3& foot, float side) {
        const float SEG = 0.43f;   // thigh and shin
        Vector3 d = Vector3Subtract(foot, hip);
        float dist = Vector3Length(d);
        if (dist > 2 * SEG - 0.005f) { d = Vector3Scale(d, (2 * SEG - 0.005f) / dist); dist = 2 * SEG - 0.005f; foot = Vector3Add(hip, d); }
        Vector3 axis = Vector3Scale(d, 1.f / std::max(dist, 1e-4f));
        Vector3 hint = {0.08f * side, 0.f, 1.f};   // knees point out the front of the rider
        Vector3 bend = Vector3Normalize(Vector3Subtract(hint, Vector3Scale(axis, Vector3DotProduct(hint, axis))));
        float h = sqrtf(std::max(0.f, SEG * SEG - dist * dist * 0.25f));
        return Vector3Add(Vector3Add(hip, Vector3Scale(d, 0.5f)), Vector3Scale(bend, h));
    };
    Vector3 kneeF = legIK(hipF, footF, 1.f);
    Vector3 kneeB = legIK(hipB, footB, -1.f);
    limb(footF, kneeF, 0.065f, pants); limb(kneeF, hipF, 0.075f, pants);
    limb(footB, kneeB, 0.065f, pants); limb(kneeB, hipB, 0.075f, pants);
    DrawCube({footF.x, footF.y + 0.02f, footF.z}, 0.12f, 0.06f, 0.26f, shoe);
    DrawCube({footB.x, footB.y + 0.02f, footB.z}, 0.12f, 0.06f, 0.26f, shoe);
    Vector3 hip = {0, hipY, 0};
    Vector3 chest = {0.03f, hipY + 0.55f - 0.08f * c, 0.06f + 0.16f * c};
    limb(hip, chest, 0.16f, shirt);
    DrawSphereEx({chest.x + 0.04f, chest.y + 0.2f, chest.z + 0.02f}, 0.12f, 6, 8, skin);
    DrawCube({chest.x + 0.04f, chest.y + 0.31f, chest.z + 0.02f}, 0.25f, 0.07f, 0.25f, cap);
    DrawCube({chest.x + 0.17f, chest.y + 0.29f, chest.z + 0.02f}, 0.12f, 0.02f, 0.2f, cap); // brim points forward
    // arms out for balance, or grabbing the board
    float armWave = sinf((float)GetTime() * 2.f) * 0.05f;
    Vector3 shF = {chest.x + 0.16f, chest.y - 0.02f, chest.z}, shB = {chest.x - 0.16f, chest.y - 0.02f, chest.z};
    Vector3 handF = {0.55f, chest.y - 0.3f + armWave + lean * 0.2f, 0.12f};
    Vector3 handB = {-0.55f, chest.y - 0.25f - armWave - lean * 0.2f, 0.05f};
    if (state == AIR && grabbed && flipT < 0 && shoveT < 0) handF = {0.08f, 0.16f + lift, 0.11f};
    if (pushAmt > 0.05f && state == RIDE) handB = {-0.35f, chest.y - 0.45f, 0.25f};
    limb(shF, handF, 0.05f, shirt);
    limb(shB, handB, 0.05f, shirt);
    rlPopMatrix();
}
