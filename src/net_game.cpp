#include "net_game.h"

// Only counts and hand-placed x/z go in: generated coordinates can differ in the last bits between CPUs (Intel
// vs Apple Silicon, fused multiply-add), and hashing those would keep friends on the same build apart.
uint32_t worldFingerprint(const World& w) {
    uint32_t h = 2166136261u;   // FNV-1a
    auto mix = [&](uint32_t v) {
        for (int i = 0; i < 4; i++, v >>= 8) { h ^= v & 0xFF; h *= 16777619u; }
    };
    auto n = [&](size_t v) { mix((uint32_t)v); };
    auto at = [&](float x, float z) { mix((uint32_t)(int32_t)lrintf(x)); mix((uint32_t)(int32_t)lrintf(z)); };
    mix(netp::GAME_BUILD);
    n(w.plats.size()); n(w.rails.size()); n(w.houses.size()); n(w.chunks.size()); n(w.ponds.size()); n(w.lamps.size());
    n(w.signals.size()); n(w.signalHeads.size()); n(w.seats.size()); n(w.carPaths.size()); n(w.tramPaths.size());
    n(w.cablePaths.size()); n(w.pedPaths.size()); n(w.gondolas.size()); n(w.stations.size());
    n(w.hfW); n(w.hfH); n(w.hf2W); n(w.hf2H);
    for (const Grid* g : {&w.sf, &w.oak, &w.mp}) { n(g->ni); n(g->nj); }
    n(w.spots.size());
    for (auto& s : w.spots) at(s.pos.x, s.pos.z);
    n(w.tokens.size());
    for (auto& t : w.tokens) at(t.pos.x, t.pos.z);
    return h;
}

netp::PlayerState captureState(const Player& p, Vector3 pos, uint8_t vehicle) {
    netp::PlayerState s;
    s.x = pos.x; s.y = pos.y; s.z = pos.z;
    s.yaw = p.yaw;
    // world-space velocity, whatever the state keeps it in
    if (p.state == Player::AIR || p.state == Player::BAIL) { s.vx = p.vel.x; s.vy = p.vy; s.vz = p.vel.z; }
    else if (p.state == Player::GRIND) { float v = fabsf(p.speed); s.vx = p.railTravel.x * v; s.vz = p.railTravel.y * v; }
    else { Vector3 f = p.fwd(); s.vx = f.x * p.speed; s.vy = p.rideVy; s.vz = f.z * p.speed; }
    s.state = (uint8_t)p.state;
    s.vehicle = vehicle;
    s.flags = (p.grabbing ? netp::PlayerState::F_GRABBING : 0) | (p.bouncing ? netp::PlayerState::F_BOUNCING : 0) |
              (p.splashed ? netp::PlayerState::F_SPLASHED : 0) | (p.boardslide ? netp::PlayerState::F_BOARDSLIDE : 0) |
              (p.railFakie ? netp::PlayerState::F_FAKIE : 0);
    s.rail = p.rail; s.railT = p.railT;
    s.crouch = p.crouch; s.lean = p.lean; s.pushPhase = p.pushPhase; s.pushAmt = p.pushAmt;
    s.pitch = p.pitch; s.balance = p.balance;
    s.flipAngle = p.flipAngle; s.shoveAngle = p.shoveAngle;
    s.flipProg = p.flipT >= 0 ? p.flipT / p.flipDur : -1;
    s.shoveProg = p.shoveT >= 0 ? p.shoveT / p.shoveDur : -1;
    s.bailT = p.bailT;
    s.boardDx = p.boardPos.x - p.pos.x; s.boardDy = p.boardPos.y - p.pos.y; s.boardDz = p.boardPos.z - p.pos.z;
    s.boardYaw = p.boardYaw;
    s.score = p.score <= 0 ? 0u : p.score >= 0xFFFFFFFFll ? 0xFFFFFFFFu : (uint32_t)p.score;
    return s;
}

SkaterPose poseFromState(const netp::PlayerState& s) {
    SkaterPose p;
    p.state = s.state;
    p.pos = {s.x, s.y, s.z};
    p.yaw = s.yaw;
    p.crouch = s.crouch; p.lean = s.lean; p.pushPhase = s.pushPhase; p.pushAmt = s.pushAmt; p.pitch = s.pitch;
    p.flipProg = s.flipProg; p.shoveProg = s.shoveProg;
    p.flipAngle = s.flipAngle; p.shoveAngle = s.shoveAngle;
    p.grabbing = s.flags & netp::PlayerState::F_GRABBING;
    p.bailT = s.bailT;
    p.splashed = s.flags & netp::PlayerState::F_SPLASHED;
    p.boardPos = {s.x + s.boardDx, s.y + s.boardDy, s.z + s.boardDz};
    p.boardYaw = s.boardYaw;
    return p;
}
