#include "traffic_sync.h"
#include <algorithm>

TrafficActor actorOf(const netp::PlayerState& s) {
    TrafficActor a;
    a.pos = {s.x, s.y, s.z};
    a.vel = {s.vx, s.vz};
    a.hspeed = sqrtf(s.vx * s.vx + s.vz * s.vz);
    a.state = s.state;
    a.local = false;   // (their own machine decides which people dodge them)
    return a;
}

void SlewedOffset::aim(double t) {
    target = t;
    if (!valid) { cur = t; valid = true; last = -1e18; }
}

double SlewedOffset::at(double local) {
    if (!valid) return local;
    if (last > -1e17) {
        double dt = std::max(0.0, local - last);
        double d = target - cur;
        if (d > SNAP || d < -BACK_SNAP) cur = target;
        else cur += std::clamp(d, -SLEW * dt, SLEW * dt);
    }
    last = std::max(last, local);
    return local + cur;
}

void SessionClock::sample(double simTime, double recvAt, double hostRttMs, double myRttMs) {
    if (hostRttMs >= 0 && myRttMs >= 0) {   // (each leg's half of its round trip)
        double ow = (hostRttMs + myRttMs) / 2000;
        oneWay = oneWay < 0 ? ow : oneWay + 0.1 * (ow - oneWay);
    }
    seen.push_back(simTime - recvAt);
    if (seen.size() > (size_t)WINDOW) seen.pop_front();
    std::vector<double> v(seen.begin(), seen.end());
    std::sort(v.begin(), v.end());
    // the median arrival offset: a late packet or two moves it less than the mean, and it doesn't hang on the one
    // quickest packet the way the earliest does. The spread below it is how late snapshots get here.
    double med = v[v.size() / 2];
    // (the 10th percentile, not the earliest: one packet held up by a wifi hiccup mustn't pull the traffic back)
    margin = std::clamp(med - v[v.size() / 10] + 0.02, MARGIN_MIN, MARGIN_MAX);
    traffic.aim(med - margin);
    session.aim(med + std::max(oneWay, 0.0));
}

std::vector<int> TrafficSync::pickCars(const Traffic& T, const std::vector<Vector3>& others) {
    std::vector<std::pair<float, int>> v;
    if (others.empty()) return {};
    for (int k = 0; k < (int)T.cars.size(); k++) {
        const Car& c = T.cars[k];
        float d2 = 1e30f;
        for (const Vector3& o : others) d2 = std::min(d2, sq(c.pos.x - o.x) + sq(c.pos.z - o.z));
        if (d2 < R_SYNC * R_SYNC) v.push_back({d2, k});
    }
    if (v.size() > (size_t)MAX_CARS) {
        std::nth_element(v.begin(), v.begin() + MAX_CARS, v.end());
        v.resize(MAX_CARS);
    }
    std::sort(v.begin(), v.end());
    std::vector<int> idx;
    for (auto& e : v) idx.push_back(e.second);
    return idx;
}

std::vector<netp::MsgCarSnapshot> TrafficSync::snapshot(const Traffic& T, const std::vector<Vector3>& others, double rttMs) {
    std::vector<int> idx = pickCars(T, others);
    int parts = std::max(1, (int)(idx.size() + CARS_PER_PART - 1) / CARS_PER_PART);
    std::vector<netp::MsgCarSnapshot> out(parts);
    for (int p = 0; p < parts; p++) {
        netp::MsgCarSnapshot& m = out[p];
        m.simTime = T.simTime;
        m.hostRttMs = rttMs < 0 ? 0xFFFF : (uint16_t)std::min(rttMs, 65534.0);
        m.part = (uint8_t)p; m.parts = (uint8_t)parts;
        T.serializeTrains(m.trains);
        std::vector<int> some(idx.begin() + std::min(idx.size(), (size_t)p * CARS_PER_PART),
                              idx.begin() + std::min(idx.size(), (size_t)(p + 1) * CARS_PER_PART));
        T.serializeCars(some, m.cars);
    }
    return out;
}

void TrafficSync::onCarSnapshot(const netp::MsgCarSnapshot& m, double recvAt, double myRttMs, const Traffic& T) {
    clock.sample(m.simTime, recvAt, m.hostRttMs == 0xFFFF ? -1 : m.hostRttMs, myRttMs);
    if (!synced) return;   // (our traffic isn't the host's yet: its cars mean nothing here)
    long long k = stepOf(m.simTime), cur = stepOf(T.simTime);
    // Our traffic runs a little behind the snapshots coming in, so this one waits for the step it was taken at and is
    // applied there: the cars it has are then exactly where the host had them at that step. One that comes in after
    // our traffic has passed that step is dropped rather than applied a few steps out (it would set the cars back
    // along their routes, and they'd lag the host until the next one); the clock's margin grows to fit it next time.
    if (k < lastApplied_) { stale++; return; }
    if (k < cur) { late++; return; }
    pending_[k].push_back(m);
    while (pending_.size() > 64) { stale += (int)pending_.begin()->second.size(); pending_.erase(pending_.begin()); }
}

void TrafficSync::applyDue(Traffic& T) {
    long long cur = stepOf(T.simTime);
    std::vector<int> hold, released;
    for (size_t k = 0; k < held_.size();) {
        if (held_[k].second < cur) held_.erase(held_.begin() + k);   // (the host never stopped it: its snapshots win)
        else hold.push_back(held_[k++].first);
    }
    while (!pending_.empty() && pending_.begin()->first <= cur) {
        auto& e = *pending_.begin();
        if (e.first < cur) late += (int)e.second.size();   // (the clock jumped past it)
        else
            for (const netp::MsgCarSnapshot& m : e.second) {
                bool ok = T.applyTrains(m.trains.data(), m.trains.size());
                ok &= T.applyCars(m.cars.data(), m.cars.size(), false, true, hold.empty() ? nullptr : &hold, &released);
                ok ? applied++ : bad++;
            }
        lastApplied_ = e.first;
        pending_.erase(pending_.begin());
    }
    for (int k : released)
        held_.erase(std::remove_if(held_.begin(), held_.end(), [&](const auto& h) { return h.first == k; }), held_.end());
}

bool TrafficSync::onFull(Traffic& T, World& w, const uint8_t* data, size_t n) {
    uint32_t sd;
    double st;
    if (!Traffic::peekFull(data, n, &sd, &st) || st < 0 || st > netp::MAX_HOST_TIME) { bad++; return false; }
    // from the start again, so the people (not in the snapshot) can be walked on from where the host's started
    T.init(w, sd);
    if (!T.applyFull(data, n)) { bad++; return false; }
    // the people catch up over the next frames (CATCHUP_WORK a frame), rather than in one long freeze
    pedSteps_ = std::min(stepOf(st), CATCHUP_MAX);
    pedNext_ = 0;
    pedEnd_ = T.peds.size();
    synced = true;
    wantFull = false;
    pending_.clear();
    lastApplied_ = -1;
    return true;
}

bool TrafficSync::shouldAsk(double now) {
    if (!wantFull || now - askedAt < ASK_EVERY) return false;
    askedAt = now;
    return true;
}

void TrafficSync::catchUp(Traffic& T, World& w) {
    if (pedNext_ >= pedEnd_ || pedSteps_ <= 0) return;
    size_t n = (size_t)std::max(1LL, CATCHUP_WORK / pedSteps_);
    size_t to = std::min(pedEnd_, pedNext_ + n);
    T.catchUpPeds(pedSteps_, w, pedNext_, to);
    pedNext_ = to;
}

int TrafficSync::step(Traffic& T, World& w, const std::vector<TrafficActor>& actors, double now, float* alpha) {
    if (!synced || !clock.valid()) return -1;
    catchUp(T, w);
    double target = clock.trafficTime(now);
    long long cur = stepOf(T.simTime), want = (long long)floor(target / (double)Traffic::STEP);
    // far out of step (a long hitch, the host's clock somewhere else): rather than run hundreds of steps, ask for
    // the host's traffic again, and catch up at MAX_STEPS a frame meanwhile
    if (fabs((double)(want - cur) * Traffic::STEP) > RESYNC && !wantFull) { wantFull = true; askedAt = -1e18; }
    return runTo(T, w, actors, target, alpha);
}

int TrafficSync::hostStep(Traffic& T, World& w, const std::vector<TrafficActor>& actors, double now, float* alpha) {
    if (!anchored_ || (now + anchor_ - T.simTime) > RESYNC) { anchor_ = T.simTime - now; anchored_ = true; }
    return runTo(T, w, actors, now + anchor_, alpha);
}

int TrafficSync::runTo(Traffic& T, World& w, const std::vector<TrafficActor>& actors, double target, float* alpha) {
    long long cur = stepOf(T.simTime), want = (long long)floor(target / (double)Traffic::STEP + 1e-6);
    int n = (int)std::clamp(want - cur, 0LL, (long long)Traffic::MAX_STEPS);
    bool honk = false;
    for (int k = 0; k < n; k++) {
        applyDue(T);
        T.update(Traffic::STEP, w, actors);
        honk |= T.evHonk;
    }
    applyDue(T);
    T.evHonk = honk;
    T.acc = 0;
    *alpha = want - cur > n ? 1.f : (float)std::clamp((target - T.simTime) / Traffic::STEP, 0.0, 1.0);
    return n;
}

void TrafficSync::hostChanged() {
    pending_.clear();
    lastApplied_ = -1;
    clock.newHost();
    held_.clear();   // (the new host's snapshots may never have them stopped: the hit went to the old one)
    if (!synced) askedAt = -1e18;   // ask the new host straight away
}

void TrafficSync::becomeHost(Traffic& T, double now) {
    double s = T.simTime;
    if (synced && clock.valid()) s = std::max(s, clock.sessionTime(now));
    std::deque<netp::MsgEvent> hits = hits_;
    *this = TrafficSync();
    synced = true; wantFull = false;   // (ours is the session's traffic now)
    anchored_ = true;
    anchor_ = s - now;
    T.acc = 0;
    for (const netp::MsgEvent& e : hits) applyCarHit(T, e);
}

bool TrafficSync::sessionNow(bool host, double now, double* t) {
    if (host) {
        if (!anchored_) return false;
        *t = now + anchor_;
        return true;
    }
    if (!clock.valid()) return false;
    *t = clock.sessionTime(now);
    return true;
}

netp::MsgEvent TrafficSync::carHit(Traffic& T, int k) {
    netp::MsgEvent e;
    e.kind = netp::EVT_CAR_HIT;
    e.idx = (uint32_t)k;
    e.simTime = T.simTime;
    T.serializeCars({k}, e.car);
    held_.erase(std::remove_if(held_.begin(), held_.end(), [&](const auto& h) { return h.first == k; }), held_.end());
    held_.push_back({k, stepOf(T.simTime) + (long long)(HIT_HOLD / Traffic::STEP)});
    noteHit(e, T);
    return e;
}

bool TrafficSync::applyCarHit(Traffic& T, const netp::MsgEvent& e) {
    if (e.kind != netp::EVT_CAR_HIT || e.idx >= T.cars.size()) return false;
    double age = (double)(stepOf(T.simTime) - stepOf(e.simTime)) * Traffic::STEP;
    if (age > HIT_KEEP) return false;   // (long over, or about to be)
    age = std::max(age, 0.0);
    Car& c = T.cars[e.idx];
    // the car as the hitter had it, if it's this one and near ours (anything else: just stopped where it is)
    ByteReader r(e.car.data(), e.car.size());
    r.u8();
    int n = r.u16(), k = r.u16();
    float x = r.f32(), z = r.f32();
    if (!r.bad && n == 1 && k == (int)e.idx && std::isfinite(x) && std::isfinite(z) && sq(x - c.pos.x) + sq(z - c.pos.z) < HIT_SNAP * HIT_SNAP)
        T.applyCars(e.car.data(), e.car.size(), true, false);
    // stopped for the rest of the 3 s the hitter's machine gave it (Traffic::interact): the same as theirs from here on
    if (c.stopT < 1e8f) c.stopT = std::max(c.stopT, (float)(3.0 - age));
    c.speed = 0;
    return true;
}

void TrafficSync::noteHit(const netp::MsgEvent& e, const Traffic& T) {
    if (e.kind != netp::EVT_CAR_HIT) return;
    hits_.push_back(e);
    while (!hits_.empty() && (hits_.size() > 32 || T.simTime - hits_.front().simTime > HIT_KEEP)) hits_.pop_front();
}

netp::MsgEvent pedEventMsg(const PedEvent& e) {
    netp::MsgEvent m;
    m.idx = (uint32_t)e.ped;
    if (e.kind == PedEvent::KNOCK) m.kind = netp::EVT_PED_KNOCK;
    else { m.kind = netp::EVT_PED_EVADE; m.evade = e.kind; m.dirX = e.dir.x; m.dirZ = e.dir.y; m.dist = std::min(e.dist, netp::MsgEvent::DIST_MAX); }
    return m;
}

bool pedEventOf(const netp::MsgEvent& m, PedEvent* e) {
    if (m.idx > 0x7FFFFFFF) return false;
    e->ped = (int)m.idx;
    if (m.kind == netp::EVT_PED_KNOCK) { e->kind = PedEvent::KNOCK; e->dir = {0, 0}; e->dist = 0; return true; }
    if (m.kind != netp::EVT_PED_EVADE || m.evade < EV_STEP || m.evade > EV_DIVE) return false;
    e->kind = m.evade; e->dir = {m.dirX, m.dirZ}; e->dist = m.dist;
    return true;
}

bool honkNear(const Traffic& T, Vector3 ear, float r) {
    for (const Vector3& h : T.honks) if (sq(h.x - ear.x) + sq(h.z - ear.z) < r * r) return true;
    return false;
}
