// Headless checks for shared traffic (traffic_sync): a host and a client Traffic joined by a simulated network link
// (latency, jitter, loss), each with every player as an actor and the other's skater seen late, as over a real one.
#include "../src/traffic_sync.h"
#include <chrono>
#include <cstdio>
#include <future>
#include <algorithm>

using CheckFn = void (*)(bool, const char*, const char*);

namespace {

// A one-way link: what's sent at t arrives at t + lat + jitter, unless it's lost. In order or not, as UDP.
struct Link {
    double lat, jit, loss;
    RNG r;
    std::multimap<double, netp::MsgCarSnapshot> q;
    double spikeEvery = 0;   // (> 0: every this many s, what's sent in the next 0.1 s takes 350 ms: a wifi hiccup)
    Link(double l, double j, double lo, uint32_t seed) : lat(l), jit(j), loss(lo), r(seed) {}
    void send(double t, const netp::MsgCarSnapshot& m) {
        if (r.f() < loss) return;
        bool spike = spikeEvery > 0 && fmod(t, spikeEvery) < 0.1;
        q.insert({t + (spike ? 0.35 : lat + jit * r.f()), m});
    }
    template <class F> void deliver(double t, F f) {
        while (!q.empty() && q.begin()->first <= t) { f(q.begin()->second); q.erase(q.begin()); }
    }
};

struct CarRec { Vector3 pos; int mode, i, j, d; float s; int pdir; };
struct PedRec { Vector3 pos; int mode, i, j, qx, qz; float t, s; uint32_t rolls; };
std::vector<CarRec> carsOf(const Traffic& T) {
    std::vector<CarRec> v;
    for (const Car& c : T.cars) v.push_back({c.pos, c.mode, c.i, c.j, c.d, c.s, c.pdir});
    return v;
}
std::vector<PedRec> pedsOf(const Traffic& T) {
    std::vector<PedRec> v;
    for (const Ped& p : T.peds) v.push_back({p.pos, p.mode, p.i, p.j, p.qx, p.qz, p.t, p.s, p.rolls});
    return v;
}
bool sameRoute(const CarRec& a, const Car& c) {
    return c.mode == 0 ? a.i == c.i && a.j == c.j && a.d == c.d : a.pdir == c.pdir && fabsf(a.s - c.s) < 0.5f;
}

// cars within `r` of `at`: the worst position error against the host's, and how many are on another route
struct Cmp { int n = 0, routeBad = 0; float worst = 0; double sum = 0; };
void compareNear(const std::vector<CarRec>& host, const Traffic& C, Vector3 at, float r, Cmp& out) {
    for (size_t k = 0; k < C.cars.size(); k++) {
        const Car& c = C.cars[k];
        if (sq(host[k].pos.x - at.x) + sq(host[k].pos.z - at.z) > r * r && sq(c.pos.x - at.x) + sq(c.pos.z - at.z) > r * r) continue;
        float e = sqrtf(sq(c.pos.x - host[k].pos.x) + sq(c.pos.z - host[k].pos.z));
        out.n++;
        out.sum += e;
        out.worst = std::max(out.worst, e);
        out.routeBad += !sameRoute(host[k], c);
    }
}

// people against the host's: on the same leg of the same route, and how far apart (median, 90th percentile, worst)
struct PedCmp { int n = 0, same = 0; float med = 0, p90 = 0, worst = 0; };
PedCmp comparePeds(const std::vector<PedRec>& host, const Traffic& C) {
    PedCmp o;
    std::vector<float> d;
    for (size_t k = 0; k < C.peds.size(); k++) {
        const Ped& p = C.peds[k];
        const PedRec& h = host[k];
        if (p.mode == 3) continue;
        o.n++;
        o.same += p.mode == h.mode && p.i == h.i && p.j == h.j && p.qx == h.qx && p.qz == h.qz && p.rolls == h.rolls;
        d.push_back(sqrtf(sq(p.pos.x - h.pos.x) + sq(p.pos.z - h.pos.z)));
    }
    std::sort(d.begin(), d.end());
    if (!d.empty()) { o.med = d[d.size() / 2]; o.p90 = d[d.size() * 9 / 10]; o.worst = d.back(); }
    return o;
}

}  // namespace

void netSimChecks(World& W, CheckFn check) {
    char b[700];
    using clk = std::chrono::steady_clock;

    {   // The session clock: snapshots every 0.1 s over two legs of 25 ms + up to 15 ms jitter each (round trips 65 ms),
        // our clock 777 s off the host's. It ends up within a few ms of the host's, never goes backwards, and the
        // traffic time stays behind the snapshots coming in (by the margin) so none of them is late.
        SessionClock sc;
        RNG r(7);
        double worst = 0, sumErr = 0, prevS = -1e9, prevT = -1e9, worstMargin = 0;
        int n = 0, back = 0, lateN = 0, sent = 0;
        std::multimap<double, double> q;   // arrival -> host time it was sent at
        for (int f = 0; f < 60 * 60; f++) {
            double t = f / 60.0 + (r.f() - 0.5) * 0.004;   // (frames a little uneven)
            if (f % 6 == 0) { q.insert({t + 0.025 + 0.015 * r.f() + 0.025 + 0.015 * r.f(), t}); sent++; }
            while (!q.empty() && q.begin()->first <= t) {
                // (taken as it arrives: the game gets it at its next frame, half a frame later on average, which shows
                // up as that much more latency than the round trips say)
                double S = q.begin()->second, at = q.begin()->first;
                if (sc.valid() && sc.trafficTime(at + 777) > S && f > 600) lateN++;
                sc.sample(S, at + 777, 65, 65);
                q.erase(q.begin());
            }
            if (!sc.valid()) continue;
            double s = sc.sessionTime(t + 777), tt = sc.trafficTime(t + 777);
            back += s < prevS || tt < prevT;
            prevS = s; prevT = tt;
            if (f > 600) { worst = std::max(worst, fabs(s - t)); sumErr += fabs(s - t); n++; worstMargin = std::max(worstMargin, sc.margin); }
        }
        // a host clock that jumps: 1 s on, the clock jumps after it; 1 s back, it slews back (never backwards: a clock
        // that steps back freezes the traffic); 6 s back (a stall the host gave up on), it jumps back
        auto jump = [](double by) {
            SessionClock sj;
            for (int k = 0; k < 40; k++) { sj.sample(k * 0.1, k * 0.1 + 0.05, 50, 50); sj.sessionTime(k * 0.1 + 0.05); }
            double prev = sj.sessionTime(3.95), low = 1e9;
            for (int k = 40; k < 80; k++) {
                sj.sample(k * 0.1 + by, k * 0.1 + 0.05, 50, 50);
                double s = sj.sessionTime(k * 0.1 + 0.05);
                low = std::min(low, s - prev);
                prev = s;
            }
            return std::pair<double, double>{sj.sessionTime(8.0) - 8.0, low};   // where it ended up, and its smallest step
        };
        auto fwd = jump(1), back1 = jump(-1), back6 = jump(-6);
        snprintf(b, sizeof b, "off by %.2f ms on average, %.2f ms at worst; went backwards %d times; %d of %d snapshots late; margin up to %.0f ms; "
                 "host clock jumps +1 s: ends %+.2f s; -1 s: %+.2f s, smallest step %.3f s; -6 s: %+.2f s",
                 n ? sumErr / n * 1000 : 0, worst * 1000, back, lateN, sent, worstMargin * 1000, fwd.first, back1.first, back1.second, back6.first);
        check(n > 2900 && worst < 0.005 && back == 0 && lateN == 0 && fabs(fwd.first - 1) < 0.05 && back1.first > -0.2 && back1.first < -0.05 &&
              back1.second > 0.09 && fabs(back6.first + 6) < 0.05, "session clock follows the host", b);
    }
    {   // wifi hiccups: one snapshot in 50 takes 350 ms. Its own snapshot is dropped, but the traffic mustn't stall
        // (the clock stepping back) or let the others come in late.
        SessionClock sc;
        RNG r(9);
        std::multimap<double, double> q;
        int stalled = 0, lateN = 0, spikes = 0, sent = 0;
        double prevT = -1e9;
        for (int f = 0; f < 60 * 120; f++) {
            double t = f / 60.0;
            if (f % 6 == 0) {
                bool spike = fmod(t, 5) < 0.09;
                q.insert({t + (spike ? 0.35 : 0.05 + 0.03 * r.f()), spike ? -t : t});   // (negative: the spiked one)
                sent++; spikes += spike;
            }
            while (!q.empty() && q.begin()->first <= t) {
                double S = fabs(q.begin()->second);
                if (sc.valid() && f > 600 && q.begin()->second > 0 && sc.trafficTime(t) > S) lateN++;
                sc.sample(S, t, 60, 60);
                q.erase(q.begin());
            }
            if (!sc.valid()) continue;
            double tt = sc.trafficTime(t);
            if (f > 600 && tt - prevT < 0.5 / 60) stalled++;
            prevT = tt;
        }
        snprintf(b, sizeof b, "%d of %d snapshots held up 350 ms; traffic stood still %d frames of %d; %d of the others late", spikes, sent, stalled,
                 60 * 110, lateN);
        check(spikes > 20 && stalled == 0 && lateN < sent / 100, "session clock rides out latency spikes", b);
    }

    // a downtown street six blocks long, as in "traffic is deterministic": the host and the client skate back and forth
    // in its lanes (so the cars brake for both), the host east-bound, the client west-bound and half a lap behind
    const Grid& g = W.sf;
    int i0 = -1, j0 = -1;
    for (int j = g.nj / 2; j < g.nj && i0 < 0; j++)
        for (int i = g.ni / 2; i + 6 < g.ni && i0 < 0; i++) {
            bool ok = true;
            for (int k = 0; k < 6 && ok; k++) ok = g.edge(i + k, j, 0) && g.nodeOk(i + k, j);
            if (ok) { i0 = i; j0 = j; }
        }
    Vector2 n0 = g.node(std::max(i0, 0), std::max(j0, 0));
    float L = 6 * g.sp;
    auto skater = [&](double t, int who) {
        float sp = who == 1 ? 8.f : 7.f;
        double u = fmod(std::max(t, 0.0) * sp + (who == 1 ? 0 : L), 2 * L);
        float x = n0.x + (float)(u < L ? u : 2 * L - u), z = n0.y + (who == 1 ? 2.4f : -2.4f), dir = u < L ? 1.f : -1.f;
        TrafficActor a;
        a.pos = {x, W.terrain(x, z) + 0.05f, z};
        a.vel = {dir * sp, 0};
        a.hspeed = sp;
        a.state = Player::RIDE;
        return a;
    };
    // what each machine knows of the skaters: its own now, the other's as it was 100 ms (drawn behind) + 65 ms (the
    // trip) ago; in player id order (host 1, client 2), the same on both
    const double SEEN_LATE = 0.165, LAT = 0.05, JIT = 0.03, LOSS = 0.05, RTT = 65;
    auto hostActors = [&](double t, bool client) {
        std::vector<TrafficActor> v{skater(t, 1)};
        if (client) v.push_back(skater(t - SEEN_LATE, 2));
        return v;
    };
    auto clientActors = [&](double t) { return std::vector<TrafficActor>{skater(t - SEEN_LATE, 1), skater(t, 2)}; };

    // Host H and client C from the same seed for SECS s; the host sends snapshots (or not) over a 50-80 ms link that
    // loses 5%. Each second (on the host's steps), the cars within 100 m of the client's skater are compared at the same
    // step on both. Returns the comparisons over the run and the last one.
    // Host frames: hostFps a second (the host's traffic runs on its clock, TrafficSync::hostStep), with no frame at all
    // for 0.5 s at hitchAt; spikeEvery: wifi hiccups on the link (see Link).
    struct Run { Cmp all, last; float trainErr = 0; int applied = 0, late = 0, stale = 0, bad = 0, honks = 0, stalled = 0; double bytes = 0, bytes3 = 0, secs = 0; int maxCars = 0; };
    struct Cfg { bool snaps = true; double hostFps = 60, hitchAt = -1, spikeEvery = 0; int secs = 120; uint32_t seed = 11; };
    const int SECS = 120;
    auto runPair = [&](Cfg cfg) {
        Run out;
        Traffic H, C;
        H.init(W); C.init(W);
        TrafficSync ts, hts;
        ts.synced = true; ts.wantFull = false;   // (the same seed and both from the start: already in step)
        Link link(LAT, JIT, LOSS, cfg.seed);
        link.spikeEvery = cfg.spikeEvery;
        bool snaps = cfg.snaps;
        std::map<long long, std::vector<CarRec>> hostAt;
        std::map<long long, std::vector<float>> trainsAt;
        Vector3 far3 = {(float)g.node(g.ni / 4, g.nj / 4).x, 0, (float)g.node(g.ni / 4, g.nj / 4).y};   // (a third player, for the bandwidth)
        double nextFrame = 0, nextSend = 0;
        int zeroRun = 0;
        for (int f = 0; f <= 60 * cfg.secs; f++) {
            double t = f / 60.0;
            bool hostFrame = t >= nextFrame - 1e-9 && !(cfg.hitchAt >= 0 && t >= cfg.hitchAt && t < cfg.hitchAt + 0.5);
            if (hostFrame) {
                nextFrame = std::max(nextFrame + 1 / cfg.hostFps, t);
                long long before = stepOf(H.simTime);
                float a;
                hts.hostStep(H, W, hostActors(t, true), t, &a);
                out.honks += H.evHonk;
                long long hs = stepOf(H.simTime);
                if (hs / 30 != before / 30 || hs == 0) {   // (now and then: a frame can run several steps)
                    hostAt[hs] = carsOf(H);
                    for (auto& tr : H.trains) trainsAt[hs].push_back(tr.s);
                }
            }
            if (hostFrame && t >= nextSend - 1e-9) {
                nextSend = std::max(nextSend + TrafficSync::INTERVAL, t);
                Vector3 seen = skater(t - LAT - 0.015, 2).pos;   // (the latest state the host has of the client)
                auto parts = TrafficSync::snapshot(H, {seen}, RTT);
                for (auto& m : parts) { link.send(t, m); out.bytes += netp::pack(m).size() + 40; }   // (+ UDP/IP + ENet)
                for (auto& m : TrafficSync::snapshot(H, {seen, far3}, RTT)) out.bytes3 += netp::pack(m).size() + 40;
                out.maxCars = std::max(out.maxCars, (int)TrafficSync::pickCars(H, {seen}).size());
            }
            link.deliver(t, [&](const netp::MsgCarSnapshot& m) {
                if (snaps) ts.onCarSnapshot(m, t + 777, RTT, C);
                else ts.clock.sample(m.simTime, t + 777, RTT, RTT);
            });
            float alpha;
            int n = ts.step(C, W, clientActors(t), t + 777, &alpha);
            // stood still: three or more frames in a row with no step (one now and then is the clock slewing)
            zeroRun = n == 0 ? zeroRun + 1 : 0;
            if (t > 10) out.stalled += zeroRun == 3 ? 3 : zeroRun > 3;
            long long cs = stepOf(C.simTime);
            auto it = hostAt.find(cs);
            if (it != hostAt.end() && t > 10) {
                Cmp one;
                compareNear(it->second, C, skater(t, 2).pos, 100, one);
                out.all.n += one.n; out.all.routeBad += one.routeBad; out.all.sum += one.sum; out.all.worst = std::max(out.all.worst, one.worst);
                out.last = one;
                for (size_t k = 0; k < C.trains.size(); k++) out.trainErr = std::max(out.trainErr, fabsf(C.trains[k].s - trainsAt[cs][k]));
                hostAt.erase(hostAt.begin(), it);
            }
        }
        out.applied = ts.applied; out.late = ts.late; out.stale = ts.stale; out.bad = ts.bad;
        out.secs = cfg.secs;
        return out;
    };

    // Joining: the host runs JOIN_AT s on its own, then the client turns up: a full snapshot (taking 0.4 s to arrive),
    // car snapshots, and both run AFTER s more. Cars compared as above; people compared with the host's at the step the
    // snapshot was taken at, right after it's applied (caught up, and for comparison a plain init + applyFull), and at
    // the end.
    struct JoinRun { Cmp near, all; bool fullOk = false; double fullMs = 0; size_t fullBytes = 0; PedCmp atJoin, atJoinPlain, atEnd; int applied = 0, late = 0, catchFrames = 0; double catchMs = 0; };
    const double JOIN_AT = 300, AFTER = 60;
    auto runJoin = [&] {
        JoinRun out;
        Traffic H, C;
        H.init(W); C.init(W);
        for (int f = 1; f <= (int)(60 * JOIN_AT); f++) H.update(Traffic::STEP, W, hostActors(f / 60.0, false));
        std::vector<uint8_t> full;
        H.serializeFull(full);
        out.fullBytes = full.size();
        std::vector<PedRec> pedsAtFull = pedsOf(H);
        long long fullStep = stepOf(H.simTime);
        double fullArrives = JOIN_AT + 0.4;
        TrafficSync ts;
        Link link(LAT, JIT, LOSS, 13);
        std::map<long long, std::vector<CarRec>> hostAt;
        std::vector<PedRec> pedsEnd;
        long long endStep = stepOf(JOIN_AT + AFTER - 1);
        bool joined = false;
        std::map<long long, std::vector<PedRec>> pedsAt;
        for (int f = (int)(60 * JOIN_AT) + 1; f <= (int)(60 * (JOIN_AT + AFTER)); f++) {
            double t = f / 60.0;
            H.update(Traffic::STEP, W, hostActors(t, true));
            long long hs = stepOf(H.simTime);
            if (hs % 60 == 0) hostAt[hs] = carsOf(H);
            if (hs == endStep) pedsEnd = pedsOf(H);
            if (t >= fullArrives - 0.1 && t < fullArrives + 3) pedsAt[hs] = pedsOf(H);
            if (f % 6 == 0) for (auto& m : TrafficSync::snapshot(H, {skater(t - LAT - 0.015, 2).pos}, RTT)) link.send(t, m);
            link.deliver(t, [&](const netp::MsgCarSnapshot& m) { ts.onCarSnapshot(m, t + 777, RTT, C); });
            if (!joined && t >= fullArrives) {
                joined = true;
                auto t0 = clk::now();
                out.fullOk = ts.onFull(C, W, full.data(), full.size());
                out.fullMs = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
                Traffic X; X.init(W); X.applyFull(full.data(), full.size());
                out.atJoinPlain = comparePeds(pedsAtFull, X);
                (void)fullStep;
            }
            float alpha;
            bool catching = joined && ts.pedsOwed() > 0;
            auto t1 = clk::now();
            if (ts.step(C, W, clientActors(t), t + 777, &alpha) < 0) C.update(Traffic::STEP, W, clientActors(t));   // (its own until then)
            long long cs = stepOf(C.simTime);
            if (catching) {
                out.catchFrames++;
                out.catchMs = std::max(out.catchMs, std::chrono::duration<double, std::milli>(clk::now() - t1).count());
                if (ts.pedsOwed() == 0 && pedsAt.count(cs)) out.atJoin = comparePeds(pedsAt[cs], C);   // (all caught up)
            }
            if (cs == endStep && !pedsEnd.empty()) out.atEnd = comparePeds(pedsEnd, C);
            auto it = hostAt.find(cs);
            if (it != hostAt.end() && ts.synced && t > fullArrives + 5) {
                Cmp one, every;
                compareNear(it->second, C, skater(t, 2).pos, 100, one);
                compareNear(it->second, C, {0, 0, 0}, 1e7f, every);
                out.near.n += one.n; out.near.routeBad += one.routeBad; out.near.worst = std::max(out.near.worst, one.worst); out.near.sum += one.sum;
                out.all = every;
                hostAt.erase(hostAt.begin(), it);
            }
        }
        out.applied = ts.applied; out.late = ts.late;
        return out;
    };

    Cfg plain, off, slow, spiky;
    off.snaps = false; off.seed = 12;
    slow.hostFps = 20; slow.hitchAt = 30; slow.secs = 60; slow.seed = 14;
    spiky.spikeEvery = 5; spiky.secs = 60; spiky.seed = 15;
    auto f1 = std::async(std::launch::async, runPair, plain), f2 = std::async(std::launch::async, runPair, off);
    auto f3 = std::async(std::launch::async, runJoin);
    auto f4 = std::async(std::launch::async, runPair, slow), f5 = std::async(std::launch::async, runPair, spiky);
    Run with = f1.get(), without = f2.get(), slowR = f4.get(), spikeR = f5.get();
    JoinRun jr = f3.get();

    snprintf(b, sizeof b, "street (%d, %d), %d s, %d honks; near the client: %d car samples, worst %.3f m (mean %.4f), %d off route, last second: "
             "%d cars, worst %.3f m, %d off route; trains worst %.4f m; parts applied %d, late %d, stale %d, bad %d; host sends %.1f KB/s "
             "(3 players %.1f KB/s), up to %d cars",
             i0, j0, SECS, with.honks, with.all.n, with.all.worst, with.all.n ? with.all.sum / with.all.n : 0, with.all.routeBad, with.last.n,
             with.last.worst, with.last.routeBad, with.trainErr, with.applied, with.late, with.stale, with.bad, with.bytes / with.secs / 1024,
             with.bytes3 / with.secs / 1024, with.maxCars);
    check(i0 >= 0 && with.honks > 0 && with.last.n > 5 && with.last.worst < 0.5f && with.last.routeBad == 0 && with.all.routeBad == 0 &&
          with.all.worst < 0.5f && with.trainErr < 0.01f && with.applied > 1000 && with.bad == 0,
          "shared traffic: client matches host", b);
    for (int k = 0; k < 2; k++) {
        const Run& r = k ? spikeR : slowR;
        int parts = r.applied + r.late + r.stale;
        snprintf(b, sizeof b, "%s, %d s: parts applied %d, late %d (%.1f%%), stale %d; client traffic stood still %d frames; near the client %d samples, "
                 "worst %.3f m, %d off route", k ? "a 350 ms hiccup every 5 s" : "host at 20 fps with a 0.5 s hitch", (int)r.secs, r.applied, r.late,
                 parts ? 100.0 * r.late / parts : 0, r.stale, r.stalled, r.all.n, r.all.worst, r.all.routeBad);
        check(r.applied > 1000 && r.late < parts / 20 && r.stalled < 10 && r.all.n > 100 && r.all.worst < 0.5f && r.all.routeBad == 0,
              k ? "shared traffic: latency spikes" : "shared traffic: slow, hitching host", b);
    }
    snprintf(b, sizeof b, "the same with no car snapshots: %d car samples, worst %.2f m (mean %.3f), %d off route; last second %d cars, worst %.2f m",
             without.all.n, without.all.worst, without.all.n ? without.all.sum / without.all.n : 0, without.all.routeBad, without.last.n, without.last.worst);
    check(without.all.worst > 2 * std::max(with.all.worst, 0.05f), "shared traffic: diverges without snapshots", b);
    snprintf(b, sizeof b, "full snapshot %zu B applied %s in %.0f ms; then near the client %d samples, worst %.3f m, %d off route; all cars: %d, worst %.3f m, "
             "%d off route; parts applied %d late %d; people caught up over %d frames (at most %.0f ms a frame, with its steps): %d of %d on the host's leg, %.2f/%.2f/%.1f m (median/90%%/worst) "
             "(not caught up: %d, %.1f/%.1f m); after %.0f s: %d, %.2f/%.2f/%.1f m",
             jr.fullBytes, jr.fullOk ? "ok" : "FAILED", jr.fullMs, jr.near.n, jr.near.worst, jr.near.routeBad, jr.all.n, jr.all.worst, jr.all.routeBad,
             jr.applied, jr.late, jr.catchFrames, jr.catchMs, jr.atJoin.same, jr.atJoin.n, jr.atJoin.med, jr.atJoin.p90, jr.atJoin.worst, jr.atJoinPlain.same, jr.atJoinPlain.med,
             jr.atJoinPlain.p90, AFTER, jr.atEnd.same, jr.atEnd.med, jr.atEnd.p90, jr.atEnd.worst);
    check(jr.fullOk && jr.atJoin.n > 5000 && jr.catchFrames > 5 && jr.catchMs < 150 && jr.near.n > 100 && jr.near.worst < 0.5f && jr.near.routeBad == 0 && jr.all.routeBad == 0 &&
          jr.atJoin.same > jr.atJoin.n * 9 / 10 && jr.atJoin.p90 < 1.f && jr.atJoinPlain.same < jr.atJoin.n / 2 && jr.atEnd.same > jr.atEnd.n * 9 / 10,
          "shared traffic: joining mid-session", b);
}
