#include "chase.h"

// difficulty knobs: cruiser/officer speed, seconds per star, and how hard a grabbing officer holds on
static const float SPEED_K[3] = {0.86f, 1.f, 1.12f}, STAR_S[3] = {26.f, 18.f, 13.f}, HOLD_K[3] = {0.7f, 1.f, 1.3f};

static float dist2d(Vector3 a, Vector3 b) { return sqrtf(sq(a.x - b.x) + sq(a.z - b.z)); }
static float dist2d(Vector3 a, Vector2 b) { return sqrtf(sq(a.x - b.x) + sq(a.z - b.y)); }

void Chase::reset(float calm) {
    active = false;
    scuffle = false;
    scCop = -1;
    cops.clear();
    parked.clear();
    stars = 0;
    chaseT = hideT = bustT = heatAcc = grace = strike = 0;
    nearest = 1e9f;
    calmT = calm;
}

// Puts a cruiser on open ground a street or two away, behind the skater (or, for a roadblock, in front).
void Chase::spawnCop(const Player& p, const World& w, float minD, float maxD, bool ahead) {
    float back = ahead ? p.yaw : p.yaw + PI;
    for (int tries = 0; tries < 40; tries++) {
        float a = tries < 20 ? back + rng.range(-1.2f, 1.2f) : rng.range(0, TAU_F);
        float d = rng.range(minD, maxD);
        float x = p.pos.x + cosf(a) * d, z = p.pos.z + sinf(a) * d;
        if (!w.inPlay(x, z)) continue;
        float y = w.ground(x, z, p.pos.y + 2);
        if (fabsf(y - p.pos.y) > 8 || w.isWater(x, z, y) || w.solidAt({x, y + 1.0f, z})) continue;
        Cop c;
        c.pos = {x, y, z};
        c.yaw = atan2f(p.pos.z - z, p.pos.x - x);
        c.side = cops.size() % 2 ? -1.f : 1.f;
        cops.push_back(c);
        return;
    }
}

// Where a cruiser or officer is aiming: where the skater will be, not where they are.
static Vector2 aimPoint(const Cop& c, const Player& p) {
    Vector2 pv = p.state == Player::AIR ? Vector2{p.vel.x, p.vel.z} : Vector2{cosf(p.yaw) * p.speed, sinf(p.yaw) * p.speed};
    float lead = clampf(sqrtf(sq(c.pos.x - p.pos.x) + sq(c.pos.z - p.pos.z)) / 25.f, 0.f, 1.3f);
    return {p.pos.x + pv.x * lead, p.pos.z + pv.y * lead};
}

void Chase::moveCar(Cop& c, float dt, const Player& p, const World& w) {
    Vector2 aim = aimPoint(c, p);
    float want = atan2f(aim.y - c.pos.z, aim.x - c.pos.x);
    if (c.stuck > 0.4f) want += c.side * 1.1f; // shoulder around whatever is in the way
    float diff = wrapPi(want - c.yaw);
    c.yaw += clampf(diff, -3.6f * dt, 3.6f * dt);
    float vmax = (12.f + 1.8f * stars) * SPEED_K[difficulty] * (0.45f + 0.55f * cosf(clampf(fabsf(diff), 0.f, 1.2f)));
    c.speed += clampf(vmax - c.speed, -14 * dt, 10 * dt);
    Vector3 np = c.pos;
    np.x += cosf(c.yaw) * c.speed * dt;
    np.z += sinf(c.yaw) * c.speed * dt;
    Vector3 pre = np;
    w.collide(np, 1.8f, c.pos.y, 1.5f, nullptr);
    float y = w.ground(np.x, np.z, c.pos.y);
    if (w.isWater(np.x, np.z, y)) { c.stuck += dt; c.speed *= 0.5f; return; }
    float moved = dist2d(np, c.pos), asked = c.speed * dt;
    c.stuck = (asked > 0.01f && moved < asked * 0.4f) ? c.stuck + dt : std::max(0.f, c.stuck - dt * 2);
    if (dist2d(np, pre) > 0.5f) c.speed *= 0.8f;
    c.pos = {np.x, y, np.z};
}

// An officer on foot: quicker to turn than a cruiser, can climb steps and, unlike a car, goes in through doors.
void Chase::moveFoot(Cop& c, float dt, const Player& p, const World& w) {
    Vector2 aim = aimPoint(c, p);
    float eyeY = p.pos.y;
    int hp = w.houseAt(p.pos.x, p.pos.z, eyeY), hc = w.houseAt(c.pos.x, c.pos.z, c.pos.y);
    // route through the front door if the skater is in a house the officer is not, or the other way round
    if (hp >= 0 && hc != hp) {
        if (c.house != hp || c.leaving) { c.house = hp; c.stage = 1; c.leaving = false; }
    } else if (hp != hc && hc >= 0) {
        if (c.house != hc || !c.leaving) { c.house = hc; c.stage = 1; c.leaving = true; }
    } else {
        c.stage = 0;
        c.house = -1;
    }
    if (c.stage > 0 && c.house >= 0) {
        // follow the house's route from the street to just inside the door (or back out again), waypoint by waypoint
        const House& h = w.houses[c.house];
        int n = h.nRoute;
        int k = c.stage - 1;
        aim = c.leaving ? h.route[n - 1 - k] : h.route[k];
        if (dist2d(c.pos, aim) < 0.9f) c.stage++;
        if (c.stage > n) c.stage = 0;
    }
    float want = atan2f(aim.y - c.pos.z, aim.x - c.pos.x);
    if (c.stuck > 0.5f && c.stage == 0) want += c.side * 1.2f;
    float diff = wrapPi(want - c.yaw);
    c.yaw += clampf(diff, -8.f * dt, 8.f * dt);
    float vmax = (6.6f + 0.9f * stars) * SPEED_K[difficulty] * (0.35f + 0.65f * cosf(clampf(fabsf(diff), 0.f, 1.4f)));
    c.speed += clampf(vmax - c.speed, -20 * dt, 14 * dt);
    Vector3 np = c.pos;
    np.x += cosf(c.yaw) * c.speed * dt;
    np.z += sinf(c.yaw) * c.speed * dt;
    w.collide(np, 0.45f, c.pos.y, 1.8f, nullptr);
    float y = w.ground(np.x, np.z, c.pos.y);
    if (w.isWater(np.x, np.z, y)) { c.stuck += dt; c.speed = 0; return; }
    float moved = dist2d(np, c.pos), asked = c.speed * dt;
    c.stuck = (asked > 0.01f && moved < asked * 0.35f) ? c.stuck + dt : std::max(0.f, c.stuck - dt * 2);
    c.pos = {np.x, y, np.z};
    c.phase += dt * c.speed * 1.3f;
}

void Chase::arrest(Player& p) {
    long long fine = std::min(p.score, (long long)(400 * stars));
    p.score -= fine;
    busts++;
    evBusted = true;
    p.spawn(p.safePos, p.safeYaw);
    lastPos = p.pos;
    p.say("BUSTED!", {255, 70, 60, 255}, TextFormat("Fined %lld", fine));
    reset(9);
}

void Chase::breakFree(Player& p) {
    Cop& c = cops[scCop];
    float away = atan2f(p.pos.z - c.pos.z, p.pos.x - c.pos.x);
    c.stun = 3.2f;
    c.speed = 0;
    c.pos.x -= cosf(away) * 1.6f;
    c.pos.z -= sinf(away) * 1.6f;
    c.yaw = away + PI;
    p.yaw = away;
    p.speed = 9.f;
    p.state = Player::RIDE;
    p.bailT = 0;
    scuffle = false;
    scCop = -1;
    grace = 2.5f;
    bustT = 0;
    shoves++;
    p.score += 400;
    evShove = true;
    p.say("SHOVED OFF!  +400", {120, 230, 120, 255}, "Go go go!");
}

void Chase::update(float dt, Player& p, World& w) {
    if (!on) return;
    // teleporting, resetting or riding a train far away starts over
    if (hasLast && dist2d(p.pos, lastPos) > 150) reset(8);
    lastPos = p.pos;
    hasLast = true;
    grace = std::max(0.f, grace - dt);
    hitCD = std::max(0.f, hitCD - dt);

    if (!active) {
        calmT -= dt;
        if (calmT > 0) return;
        for (int i = 0; i < 3; i++) spawnCop(p, w, 100, 140, false);
        if (cops.empty()) { calmT = 1; return; } // nowhere sensible to spawn yet
        active = true;
        stars = 1;
        chaseT = hideT = bustT = heatAcc = 0;
        scoreAtStart = p.score;
        evStart = true;
        p.say("COPS!", {255, 70, 60, 255}, "Lose them, or get busted");
        return;
    }

    chaseT += dt;
    int want = std::min(5, 1 + (int)(chaseT / STAR_S[difficulty]));
    if (want > stars) {
        stars = want;
        bestStars = std::max(bestStars, stars);
        spawnCop(p, w, 110, 150, false);
        if (stars >= 2) spawnCop(p, w, 90, 130, true); // roadblock up ahead
        p.say("MORE HEAT", {255, 150, 60, 255}, stars >= 2 ? TextFormat("%d stars, they're coming from the front too", stars) : "2 stars");
    }
    bestStars = std::max(bestStars, stars);
    // surviving the chase is worth points
    heatAcc += dt * 8 * stars;
    if (heatAcc >= 1) { p.score += (long long)heatAcc; heatAcc -= floorf(heatAcc); }

    // ---- the cops
    int hp = w.houseAt(p.pos.x, p.pos.z, p.pos.y);
    nearest = 1e9f;
    for (size_t i = 0; i < cops.size(); i++) {
        Cop& c = cops[i];
        float d = dist2d(c.pos, p.pos);
        if (c.stun > 0) {
            c.stun -= dt;
            c.speed = 0;
        } else if (scuffle && (int)i == scCop) {
            c.speed = 0;
            c.yaw = atan2f(p.pos.z - c.pos.z, p.pos.x - c.pos.x);
        } else if (!c.onFoot) {
            // get out when the skater slows down close by, or ducks into a house nearby
            if ((d < 18 && p.hspeed() < 7.f) || (hp >= 0 && d < 70)) {
                c.onFoot = true;
                parked.push_back({c.pos, c.yaw});
                c.pos.x += -sinf(c.yaw) * 2.2f * c.side;
                c.pos.z += cosf(c.yaw) * 2.2f * c.side;
                c.pos.y = w.ground(c.pos.x, c.pos.z, c.pos.y);
                c.speed = 0;
            } else moveCar(c, dt, p, w);
        } else moveFoot(c, dt, p, w);
        nearest = std::min(nearest, d);
        // an officer who has fallen well behind calls in a fresh cruiser instead
        c.lostT = (c.onFoot && d > 85) ? c.lostT + dt : 0;
    }
    for (size_t i = 0; i < cops.size();)
        if (cops[i].lostT > 5.f && !(scuffle && (int)i == scCop)) {
            if (scCop > (int)i) scCop--;
            cops.erase(cops.begin() + i);
            spawnCop(p, w, 170, 210, false); // reinforcements come from outside the escape radius, and take a while to arrive
        } else i++;
    for (size_t i = 0; i < cops.size(); i++) // keep them from stacking up
        for (size_t j = i + 1; j < cops.size(); j++) {
            float dx = cops[j].pos.x - cops[i].pos.x, dz = cops[j].pos.z - cops[i].pos.z, d = sqrtf(dx * dx + dz * dz);
            float minD = (cops[i].onFoot && cops[j].onFoot) ? 1.0f : 4.f;
            if (d < minD && d > 0.01f) {
                float k = (minD - d) * 0.5f / d;
                cops[i].pos.x -= dx * k; cops[i].pos.z -= dz * k;
                cops[j].pos.x += dx * k; cops[j].pos.z += dz * k;
            }
        }

    // ---- contact
    for (size_t i = 0; i < cops.size(); i++) {
        Cop& c = cops[i];
        float d = dist2d(c.pos, p.pos);
        if (fabsf(c.pos.y - p.pos.y) > 2.5f) continue;
        if (!c.onFoot) { // a cruiser that clips you at speed knocks you off
            if (d < 3.0f && p.state != Player::BAIL && c.speed > 5.f) p.bail("Clipped by a police car");
            continue;
        }
        if (c.stun > 0 || scuffle || d > 1.9f) continue;
        if (p.hspeed() > 8.5f && p.state == Player::RIDE) {
            // skating into an officer at speed flattens them
            float away = atan2f(c.pos.z - p.pos.z, c.pos.x - p.pos.x);
            c.stun = 3.5f;
            c.speed = 0;
            c.pos.x += cosf(away) * 1.5f;
            c.pos.z += sinf(away) * 1.5f;
            p.speed *= 0.72f;
            p.score += 250;
            evBowled = true;
            p.say("BOWLED OVER A COP!  +250", {255, 190, 60, 255});
        } else if (grace <= 0 && (p.state == Player::RIDE || p.state == Player::BAIL)) {
            // a skater who has just fallen over is easier to pin: a worse start, but still a fight
            scuffle = true;
            scCop = (int)i;
            meter = (p.state == Player::BAIL ? 0.3f : 0.45f) - 0.04f * stars;
            strike = 0;
            evGrab = true;
            p.say("GRABBED!", {255, 120, 60, 255}, "Mash SPACE / A D, or swing the board: J K L");
        }
    }

    // ---- the fight
    if (scuffle) {
        if (scCop < 0 || scCop >= (int)cops.size() || p.state == Player::AIR || p.state == Player::GRIND) {
            scuffle = false;
            scCop = -1;
        } else {
            Cop& c = cops[scCop];
            p.speed *= expf(-dt * 14.f);
            if (p.state == Player::BAIL) p.bailT = std::min(p.bailT, 0.5f); // hold them down
            p.yaw = lerpAngle(p.yaw, atan2f(c.pos.z - p.pos.z, c.pos.x - p.pos.x), 1 - expf(-dt * 10.f));
            int helpers = 0;
            for (size_t i = 0; i < cops.size(); i++)
                if ((int)i != scCop && cops[i].onFoot && cops[i].stun <= 0 && dist2d(cops[i].pos, p.pos) < 4.5f) helpers++;
            meter -= (0.20f + 0.035f * stars + 0.1f * helpers) * HOLD_K[difficulty] * dt;
            meter += strike;
            strike = 0;
            meter = std::min(meter, 1.f);
            if (meter >= 1.f) breakFree(p);
            else if (meter <= 0.f) { arrest(p); return; }
        }
    }

    // a bailed skater lying near an officer is as good as caught
    // (a fallen skater with a cruiser right on top of them, and no officer out yet, is also caught)
    bool pinned = p.state == Player::BAIL && !scuffle && nearest < 3.5f;
    if (pinned) bustT += dt;
    else bustT = std::max(0.f, bustT - dt);
    if (bustT > BUST_TIME) { arrest(p); return; }

    // ---- losing them: get well clear and stay there
    if (nearest > ESCAPE_DIST) hideT += dt;
    else hideT = std::max(0.f, hideT - dt * 3);
    if (hideT > ESCAPE_TIME) {
        long long tricks = std::max(0LL, p.score - scoreAtStart);
        long long pts = 700LL * stars + (long long)(chaseT * 12) + tricks / 4;
        p.score += pts;
        escapes++;
        evEscape = true;
        p.say(TextFormat("LOST THEM!  +%lld", pts), {255, 210, 60, 255}, TextFormat("%d star chase, %d s", stars, (int)chaseT));
        reset(9);
    }
}
