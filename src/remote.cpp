#include "remote.h"
#include "net_game.h"

// ---------------------------------------------------------------- looks
// Hand-picked shirt / cap pairs that read apart at a distance. None has the local skater's blue shirt and red cap,
// and no tag is red (the minimap's own arrow).
static const struct { Color shirt, cap, tag; } PALETTE[REMOTE_PALETTE] = {
    {{235, 130, 40, 255}, {30, 40, 80, 255}, {255, 165, 70, 255}},     // orange
    {{60, 170, 90, 255}, {235, 235, 235, 255}, {110, 235, 140, 255}},  // green
    {{140, 70, 190, 255}, {240, 200, 40, 255}, {200, 145, 255, 255}},  // purple
    {{235, 200, 50, 255}, {30, 30, 34, 255}, {255, 230, 80, 255}},     // yellow
    {{230, 90, 160, 255}, {40, 170, 170, 255}, {255, 140, 205, 255}},  // pink
    {{40, 180, 190, 255}, {240, 120, 40, 255}, {90, 230, 245, 255}},   // teal
    {{232, 232, 232, 255}, {40, 140, 70, 255}, {245, 245, 245, 255}},  // white
    {{160, 210, 60, 255}, {100, 50, 150, 255}, {195, 245, 95, 255}},   // lime
};
static const Color SKINS[4] = {{214, 170, 134, 255}, {241, 200, 165, 255}, {170, 120, 85, 255}, {110, 75, 55, 255}};
static const Color DECKS[4] = {{210, 120, 40, 255}, {60, 140, 210, 255}, {200, 60, 70, 255}, {230, 210, 90, 255}};

static uint32_t nameHash(const std::string& name) {
    uint32_t h = 0;
    for (unsigned char c : name) h = hashu((int)h, c, 7);
    return h;
}

int preferredSlot(const std::string& name) { return (int)(nameHash(name) % REMOTE_PALETTE); }

RemoteLook remoteLook(int slot, const std::string& name) {
    uint32_t h = nameHash(name);
    RemoteLook l;
    slot = ((slot % REMOTE_PALETTE) + REMOTE_PALETTE) % REMOTE_PALETTE;
    l.skater.shirt = PALETTE[slot].shirt;
    l.skater.cap = PALETTE[slot].cap;
    l.skater.skin = SKINS[(h >> 8) % 4];
    l.skater.deckBottom = DECKS[(h >> 12) % 4];
    l.skater.wavePhase = (h >> 16) % 628 / 100.f;
    l.tag = PALETTE[slot].tag;
    return l;
}

std::vector<int> assignSlots(const std::vector<std::pair<uint8_t, std::string>>& roster) {
    std::vector<size_t> order(roster.size());
    for (size_t i = 0; i < order.size(); i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return roster[a].first < roster[b].first; });
    std::vector<int> slot(roster.size(), 0);
    bool used[REMOTE_PALETTE] = {};
    int nUsed = 0;
    for (size_t i : order) {
        int s = preferredSlot(roster[i].second);
        if (nUsed < REMOTE_PALETTE)
            while (used[s]) s = (s + 1) % REMOTE_PALETTE;
        if (!used[s]) { used[s] = true; nUsed++; }
        slot[i] = s;
    }
    return slot;
}

// ---------------------------------------------------------------- smoothing
void RemotePlayer::addSample(float sentAt, double recvAt, const netp::PlayerState& s) {
    double o = recvAt - sentAt;
    if (!synced || fabs(o - offset) > 1.0) {
        // first state, or the sender's clock jumped (or a stall of over a second): start over from this one
        if (synced) samples.clear();
        offset = o;
        synced = true;
    } else if (o < offset) {
        offset = o;   // a quicker packet: the latency floor is lower than we thought
    } else {
        offset += (o - offset) * 0.01;   // creep up, in case the route got slower for good (or the clocks drift)
    }
    lastSeen = recvAt;
    if (!samples.empty() && sentAt <= samples.back().t) return;   // out of order or repeated
    samples.push_back({(double)sentAt, s});
    if ((int)samples.size() > MAX_SAMPLES) samples.erase(samples.begin());
}

// Between two states u of the way (span seconds apart): Hermite on position along the sent velocities, angles the
// short way round, and the discrete parts (state, rail, flags, which trick) from whichever state is nearer.
static netp::PlayerState blend(const netp::PlayerState& a, const netp::PlayerState& b, float u, float span) {
    using PS = netp::PlayerState;
    float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    if (dx * dx + dy * dy + dz * dz > RemotePlayer::TELEPORT * RemotePlayer::TELEPORT) return u < 0.5f ? a : b;
    PS o = u < 0.5f ? a : b;
    if (a.vehicle == PS::ON_FOOT && b.vehicle == PS::ON_FOOT) {
        float u2 = u * u, u3 = u2 * u;
        float h00 = 2 * u3 - 3 * u2 + 1, h10 = u3 - 2 * u2 + u, h01 = -2 * u3 + 3 * u2, h11 = u3 - u2;
        o.x = h00 * a.x + h10 * span * a.vx + h01 * b.x + h11 * span * b.vx;
        o.y = h00 * a.y + h10 * span * a.vy + h01 * b.y + h11 * span * b.vy;
        o.z = h00 * a.z + h10 * span * a.vz + h01 * b.z + h11 * span * b.vz;
    } else {   // riding something: the sent velocity is the skater's, not the vehicle's
        o.x = lerpf(a.x, b.x, u); o.y = lerpf(a.y, b.y, u); o.z = lerpf(a.z, b.z, u);
    }
    o.vx = lerpf(a.vx, b.vx, u); o.vy = lerpf(a.vy, b.vy, u); o.vz = lerpf(a.vz, b.vz, u);
    o.yaw = wrapPi(lerpAngle(a.yaw, b.yaw, u));
    o.boardYaw = wrapPi(lerpAngle(a.boardYaw, b.boardYaw, u));
    o.pushPhase = wrapPi(lerpAngle(a.pushPhase, b.pushPhase, u));
    o.crouch = lerpf(a.crouch, b.crouch, u);
    o.lean = lerpf(a.lean, b.lean, u);
    o.pushAmt = lerpf(a.pushAmt, b.pushAmt, u);
    o.pitch = lerpf(a.pitch, b.pitch, u);
    o.balance = lerpf(a.balance, b.balance, u);
    if (a.state == b.state && a.rail == b.rail && a.rail >= 0) o.railT = lerpf(a.railT, b.railT, u);
    if (a.state == b.state && a.state == 3 /* BAIL */ && b.bailT >= a.bailT) {
        o.bailT = lerpf(a.bailT, b.bailT, u);
        o.boardDx = lerpf(a.boardDx, b.boardDx, u); o.boardDy = lerpf(a.boardDy, b.boardDy, u); o.boardDz = lerpf(a.boardDz, b.boardDz, u);
    }
    // a flip or shove-it turns smoothly while the same one goes on (progress rising, same direction); across its
    // start or end the angle snaps, so 350 degrees -> 0 at the landing never spins the board back round
    if (a.flipProg >= 0 && b.flipProg >= a.flipProg && a.flipAngle * b.flipAngle >= 0) {
        o.flipProg = lerpf(a.flipProg, b.flipProg, u);
        o.flipAngle = lerpf(a.flipAngle, b.flipAngle, u);
    }
    if (a.shoveProg >= 0 && b.shoveProg >= a.shoveProg) {
        o.shoveProg = lerpf(a.shoveProg, b.shoveProg, u);
        o.shoveAngle = lerpf(a.shoveAngle, b.shoveAngle, u);
    }
    return o;
}

bool RemotePlayer::stateAt(double t, netp::PlayerState& out, bool* frozenOut) const {
    if (frozenOut) *frozenOut = false;
    if (samples.empty()) return false;
    if (t <= samples.front().t) { out = samples.front().s; return true; }
    const Sample& last = samples.back();
    if (t >= last.t) {
        double e = t - last.t;
        if (frozenOut) *frozenOut = e > EXTRAP;
        e = std::min(e, EXTRAP);
        out = last.s;
        float vx = last.s.vx, vy = last.s.vy, vz = last.s.vz, turn = 0;
        if (last.s.vehicle != netp::PlayerState::ON_FOOT) vx = vy = vz = 0;   // the sent velocity is the skater's, not the vehicle's
        if (samples.size() >= 2) {
            const Sample& p = samples[samples.size() - 2];
            float span = (float)(last.t - p.t);
            float jump2 = sq(last.s.x - p.s.x) + sq(last.s.y - p.s.y) + sq(last.s.z - p.s.z);
            if (span > 1e-3f && span < 0.3f && p.s.state == last.s.state && p.s.vehicle == last.s.vehicle && jump2 < TELEPORT * TELEPORT) {
                turn = wrapPi(last.s.yaw - p.s.yaw) / span;   // keep turning as they were (no turn rate is sent)
                if (last.s.vehicle != netp::PlayerState::ON_FOOT) {   // on a vehicle: how the sent position itself moves
                    vx = (last.s.x - p.s.x) / span; vy = (last.s.y - p.s.y) / span; vz = (last.s.z - p.s.z) / span;
                }
            }
        }
        out.x += vx * (float)e; out.y += vy * (float)e; out.z += vz * (float)e;
        out.yaw = wrapPi(out.yaw + turn * (float)e);
        return true;
    }
    size_t i = 1;
    while (samples[i].t < t) i++;   // samples[i-1].t < t <= samples[i].t
    const Sample &a = samples[i - 1], &b = samples[i];
    double span = b.t - a.t;
    out = blend(a.s, b.s, (float)((t - a.t) / span), (float)span);
    return true;
}

SkaterPose remotePose(const netp::PlayerState& s) {
    SkaterPose p = poseFromState(s);
    if (s.vehicle != netp::PlayerState::ON_FOOT) {   // not drawn riding yet: standing at the spot they sent
        SkaterPose st;
        st.pos = p.pos;
        st.yaw = p.yaw;
        st.boardPos = p.pos;
        return st;
    }
    return p;
}

// ---------------------------------------------------------------- roster
RemotePlayer* Remotes::find(uint8_t id) {
    for (auto& r : list) if (r.id == id) return &r;
    return nullptr;
}

RemotePlayer& Remotes::join(uint8_t id, const std::string& name) {
    RemotePlayer* r = find(id);
    if (!r) { list.emplace_back(); r = &list.back(); }
    else *r = RemotePlayer{};   // back again before the fade-out finished: start fresh
    r->id = id;
    r->name = name;
    assignLooks();
    return *r;
}

void Remotes::leave(uint8_t id) {
    if (RemotePlayer* r = find(id)) r->leaving = true;
}

void Remotes::addSample(uint8_t id, float sentAt, double recvAt, const netp::PlayerState& s) {
    RemotePlayer* r = find(id);
    if (r && !r->leaving) r->addSample(sentAt, recvAt, s);
}

void Remotes::assignLooks() {
    std::vector<std::pair<uint8_t, std::string>> roster;
    roster.push_back({myId, myName});
    for (auto& r : list) if (!r.leaving) roster.push_back({r.id, r.name});
    std::vector<int> slots = assignSlots(roster);
    size_t k = 1;
    for (auto& r : list) if (!r.leaving) { r.look = remoteLook(slots[k], r.name); k++; }
}

void RemotePlayer::advance(double now, float dt) {
    if (!synced) return;
    // a quicker packet or a slower route moves the offset in steps: ease the drawn clock over instead of skipping
    double step = 0.05 * dt;
    if (!drawSynced || fabs(offset - drawOffset) > 0.5) drawOffset = offset;
    else drawOffset += std::max(-step, std::min(step, offset - drawOffset));
    drawSynced = true;
    drawnAt = now - drawOffset - DELAY;
    if (stateAt(drawnAt, cur, &frozen)) {
        hasPose = true;
        pose = remotePose(cur);
    }
}

void Remotes::update(double now, float dt) {
    for (auto& r : list) {
        r.advance(now, dt);
        if (r.leaving) r.fade -= dt / 1.0f;
        else if (r.hasPose) r.fade = std::min(1.f, r.fade + dt / 0.5f);
    }
    list.erase(std::remove_if(list.begin(), list.end(), [](const RemotePlayer& r) { return r.leaving && r.fade <= 0; }), list.end());
}

// ---------------------------------------------------------------- drawing
// Grows in from (and shrinks back to) the feet while fading: the city shader draws everything opaque.
static void drawScaled(const RemotePlayer& r) {
    float k = smooth01(0.f, 1.f, r.fade);
    if (k < 0.02f) return;
    if (k > 0.999f) { drawSkater(r.pose, r.look.skater); return; }
    Vector3 p = r.pose.pos;
    rlPushMatrix();
    rlTranslatef(p.x, p.y, p.z);
    rlScalef(k, k, k);
    rlTranslatef(-p.x, -p.y, -p.z);
    drawSkater(r.pose, r.look.skater);
    rlPopMatrix();
}

void Remotes::draw(Vector3 camPos, Vector3 camFwd, float maxDist) const {
    for (auto& r : list) {
        if (!r.shown()) continue;
        Vector3 d = Vector3Subtract(r.pose.pos, camPos);
        if (Vector3LengthSqr(d) > maxDist * maxDist || Vector3DotProduct(d, camFwd) < -3.f) continue;
        drawScaled(r);
    }
}

void Remotes::drawCasters(Vector3 centre, float radius) const {
    for (auto& r : list)
        if (r.shown() && Vector3Distance(r.pose.pos, centre) < radius) drawScaled(r);
}
