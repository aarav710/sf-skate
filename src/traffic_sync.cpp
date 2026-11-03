#include "traffic_sync.h"
#include <algorithm>

TrafficActor actorOf(const netp::PlayerState& s) {
    TrafficActor a;
    a.pos = {s.x, s.y, s.z};
    a.vel = {s.vx, s.vz};
    a.hspeed = sqrtf(s.vx * s.vx + s.vz * s.vz);
    a.state = s.state;
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
        if (fabs(d) > SNAP) cur = target;
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
    // quickest packet the way the earliest does. The spread below it is how late a snapshot gets here.
    double med = v[v.size() / 2];
    margin = std::clamp(med - v[0] + 0.02, MARGIN_MIN, MARGIN_MAX);
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
    while (!pending_.empty() && pending_.begin()->first <= cur) {
        auto& e = *pending_.begin();
        if (e.first < cur) late += (int)e.second.size();   // (the clock jumped past it)
        else
            for (const netp::MsgCarSnapshot& m : e.second) {
                bool ok = T.applyTrains(m.trains.data(), m.trains.size());
                ok &= T.applyCars(m.cars.data(), m.cars.size(), false, true);
                ok ? applied++ : bad++;
            }
        lastApplied_ = e.first;
        pending_.erase(pending_.begin());
    }
}

bool TrafficSync::onFull(Traffic& T, World& w, const uint8_t* data, size_t n) {
    uint32_t sd;
    double st;
    if (!Traffic::peekFull(data, n, &sd, &st) || st < 0 || st > netp::MAX_HOST_TIME) { bad++; return false; }
    // from the start again, so the people (not in the snapshot) can be walked on from where the host's started
    T.init(w, sd);
    if (!T.applyFull(data, n)) { bad++; return false; }
    T.catchUpPeds(std::min(stepOf(st), CATCHUP_MAX), w);
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

int TrafficSync::step(Traffic& T, World& w, const std::vector<TrafficActor>& actors, double now, float* alpha) {
    if (!synced || !clock.valid()) return -1;
    double target = clock.trafficTime(now);
    long long cur = stepOf(T.simTime), want = (long long)floor(target / (double)Traffic::STEP);
    // far out of step (a long hitch, the host's clock somewhere else): rather than run hundreds of steps, ask for
    // the host's traffic again, and catch up at MAX_STEPS a frame meanwhile
    if (fabs((double)(want - cur) * Traffic::STEP) > RESYNC && !wantFull) { wantFull = true; askedAt = -1e18; }
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
    clock.reset();
    if (!synced) askedAt = -1e18;   // ask the new host straight away
}
