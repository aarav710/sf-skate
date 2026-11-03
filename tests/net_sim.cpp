// Headless checks for shared traffic (traffic_sync): a host and a client Traffic joined by a simulated network link
// (latency, jitter, loss), each with every player as an actor and the other's skater seen late, as over a real one.
#include "../src/traffic_sync.h"
#include "../src/gondola.h"
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

    // ---------------------------------------------------------------- Stage 3b: events, the session clock elsewhere
    // A reliable message (an EVENT): it always gets there, LAT + up to JIT later. to: 0 the host, 1 the client.
    struct EvLink {
        RNG r{31};
        std::multimap<double, std::pair<int, netp::MsgEvent>> q;
        void send(double t, int to, const netp::MsgEvent& m) { q.insert({t + 0.05 + 0.03 * r.f(), {to, m}}); }
    };
    // people near either skater at the same step on both machines: how far apart (median, 90th percentile, worst), and
    // how many of them are mid-dodge on one machine and not the other
    struct PedNear { int n = 0, evadeOdd = 0; std::vector<float> d; };
    auto pedsNear = [&](const std::vector<PedRec>& h, const std::vector<uint8_t>& hEv, const Traffic& C, Vector3 a, Vector3 c2, PedNear& o) {
        for (size_t k = 0; k < C.peds.size(); k++) {
            const Ped& p = C.peds[k];
            if (std::min(sq(h[k].pos.x - a.x) + sq(h[k].pos.z - a.z), sq(h[k].pos.x - c2.x) + sq(h[k].pos.z - c2.z)) > 25 * 25) continue;
            o.n++;
            o.d.push_back(sqrtf(sq(p.pos.x - h[k].pos.x) + sq(p.pos.z - h[k].pos.z)));
            o.evadeOdd += (p.evade != EV_NONE) != (hEv[k] != EV_NONE);
        }
    };
    auto pct = [](std::vector<float> v, float q) { if (v.empty()) return 0.f; std::sort(v.begin(), v.end()); return v[std::min(v.size() - 1, (size_t)(v.size() * q))]; };

    // Host H and client C as in "client matches host". events: each machine decides the dodges its own skater causes
    // and sends them (applied RemotePlayer::DELAY = 0.1 s after they arrive, when the sender is drawn there); without,
    // each guesses the dodges for the other's skater too (as before). With events, the client also runs into a car at
    // HIT_AT: stopped on the client at once and held against the snapshots, and a CAR_HIT to the host.
    struct EvRun {
        PedNear peds;
        int sentH = 0, sentC = 0, appliedPed = 0;
        bool hitDone = false;
        int hitCar = -1;
        double hostAge = -1, hostDelay = -1, heldFor = -1;   // the host's stop: how much later in traffic time, after how long; the hold
        // the hit car: fastest on the client in its stop; client vs host while it settles (a stopped car eases the last
        // metre onto its route point, the host's starting that much later) and from then on
        float clientSpeed = 0, worstSettling = 0, worstAfter = 0, hostSpeedAt = 0;
        int afterN = 0, applied = 0;
    };
    const double HIT_AT = 20;
    auto runEvents = [&](bool events) {
        EvRun out;
        Traffic H, C;
        H.init(W); C.init(W);
        H.logEvents = C.logEvents = events;
        TrafficSync ts, hts;
        ts.synced = true; ts.wantFull = false;
        Link link(LAT, JIT, LOSS, 23);
        EvLink ev;
        std::map<long long, std::vector<PedRec>> hostPeds;
        std::map<long long, std::vector<uint8_t>> hostEv;
        std::map<long long, Vector3> hostCar;
        std::multimap<double, PedEvent> pedDue[2];
        long long hitStep = -1, hostAppliedStep = -1;
        double nextSend = 0;
        // (both skate the sidewalks of the street, where the people are: on the host's side, and the other)
        auto walker = [&](double t, int who) {
            TrafficActor a = skater(t, who);
            a.pos.z = n0.y + (who == 1 ? 1 : -1) * g.walkOff;
            a.pos.y = W.terrain(a.pos.x, a.pos.z) + 0.05f;
            return a;
        };
        for (int f = 0; f <= 60 * 60; f++) {
            double t = f / 60.0;
            std::vector<TrafficActor> ha{walker(t, 1), walker(t - SEEN_LATE, 2)}, ca{walker(t - SEEN_LATE, 1), walker(t, 2)};
            if (events) { ha[1].local = false; ca[0].local = false; }
            // events in: car hits to the host, people a little later
            while (!ev.q.empty() && ev.q.begin()->first <= t) {
                auto& e = ev.q.begin()->second;
                PedEvent pe;
                if (e.second.kind == netp::EVT_CAR_HIT) {
                    if (TrafficSync::applyCarHit(H, e.second)) {
                        hostAppliedStep = stepOf(H.simTime);
                        out.hostAge = (hostAppliedStep - stepOf(e.second.simTime)) * (double)Traffic::STEP;
                        out.hostDelay = t - HIT_AT;
                    }
                } else if (pedEventOf(e.second, &pe)) pedDue[e.first].insert({ev.q.begin()->first + 0.1, pe});
                ev.q.erase(ev.q.begin());
            }
            for (int k = 0; k < 2; k++)
                while (!pedDue[k].empty() && pedDue[k].begin()->first <= t) {
                    out.appliedPed += (k ? C : H).applyPedEvent(pedDue[k].begin()->second);
                    pedDue[k].erase(pedDue[k].begin());
                }
            float a;
            hts.hostStep(H, W, ha, t, &a);
            long long hs = stepOf(H.simTime);
            if (hs % 10 == 0) {
                hostPeds[hs] = pedsOf(H);
                std::vector<uint8_t> e;
                for (auto& p : H.peds) e.push_back(p.evade);
                hostEv[hs] = e;
            }
            if (out.hitCar >= 0) hostCar[hs] = H.cars[out.hitCar].pos;
            if (t >= nextSend - 1e-9) {
                nextSend = std::max(nextSend + TrafficSync::INTERVAL, t);
                for (auto& m : TrafficSync::snapshot(H, {skater(t - LAT - 0.015, 2).pos}, RTT)) link.send(t, m);
            }
            link.deliver(t, [&](const netp::MsgCarSnapshot& m) { ts.onCarSnapshot(m, t + 777, RTT, C); });
            ts.step(C, W, ca, t + 777, &a);
            long long cs = stepOf(C.simTime);
            for (auto& e : H.pedEvents) { ev.send(t, 1, pedEventMsg(e)); out.sentH++; }
            for (auto& e : C.pedEvents) { ev.send(t, 0, pedEventMsg(e)); out.sentC++; }
            H.pedEvents.clear(); C.pedEvents.clear();
            // the client's skater runs into the nearest car coming along the street (what interact does to it), and tells the host
            if (events && !out.hitDone && t >= HIT_AT) {
                Vector3 me = skater(t, 2).pos;
                float best = 1e9f;
                for (size_t k = 0; k < C.cars.size(); k++) {
                    const Car& c = C.cars[k];
                    float d2 = sq(c.pos.x - me.x) + sq(c.pos.z - me.z);
                    if (c.placed && c.speed > 4 && d2 < 80 * 80 && d2 < best) { best = d2; out.hitCar = (int)k; }
                }
                if (out.hitCar >= 0) {
                    out.hitDone = true;
                    Car& c = C.cars[out.hitCar];
                    c.stopT = 3; c.speed = 0;
                    ev.send(t, 0, ts.carHit(C, out.hitCar));
                    hitStep = cs;
                }
            }
            if (hitStep >= 0) {
                const Car& c = C.cars[out.hitCar];
                double since = (cs - hitStep) * (double)Traffic::STEP;
                if (since < 2.5) out.clientSpeed = std::max(out.clientSpeed, c.speed);
                if (out.heldFor < 0 && ts.held() == 0) out.heldFor = since;
                auto it = hostCar.find(cs);
                if (it != hostCar.end() && hostAppliedStep >= 0 && cs > hostAppliedStep && since < 8) {
                    float d = sqrtf(sq(c.pos.x - it->second.x) + sq(c.pos.z - it->second.z));
                    if (since < 0.8) out.worstSettling = std::max(out.worstSettling, d);
                    else { out.worstAfter = std::max(out.worstAfter, d); out.afterN++; }
                    if (since > 4.5 && since < 4.6) out.hostSpeedAt = H.cars[out.hitCar].speed;
                }
            }
            auto ip = hostPeds.find(cs);
            if (ip != hostPeds.end() && t > 5) {
                pedsNear(ip->second, hostEv[cs], C, walker(t, 1).pos, walker(t, 2).pos, out.peds);
                hostPeds.erase(hostPeds.begin(), ip);
                hostEv.erase(hostEv.begin(), hostEv.find(cs));
            }
        }
        out.applied = ts.applied;
        return out;
    };

    // Host handover: H hosts A and B (A's skater stands off to the side, B's skates the street). At MIG H leaves; A hosts
    // from its own session clock on (TrafficSync::becomeHost), B carries on toward A's. 0.2 s before, B ran into a car:
    // the CAR_HIT went to the room, but H had already stopped answering (it left by timing out); A, who hosts next,
    // applies it.
    struct MigRun {
        double worstStepA = 0, worstStepB = 0, worstAB = 0, worstHB = 0;   // the session clocks: frame-to-frame steps off dt; A vs B; H vs B
        int appliedBefore = 0, appliedAfter = 0;
        double firstAfter = -1;
        Cmp nearB;
        bool hitStopped = false;
        float hitErr = -1;
        size_t heldAtEnd = 0;
    };
    const double MIG = 30;
    auto runMigrate = [&] {
        MigRun out;
        Traffic H, A, B;
        H.init(W); A.init(W); B.init(W);
        TrafficSync hts, ats, bts;
        ats.synced = bts.synced = true; ats.wantFull = bts.wantFull = false;
        Link toA(LAT, JIT, LOSS, 41), toB(LAT, JIT, LOSS, 43), aToB(LAT, JIT, LOSS, 47);
        EvLink ev;
        Vector3 aSpot = {n0.x + 3 * g.sp, 0, n0.y + 40};
        aSpot.y = W.terrain(aSpot.x, aSpot.z);
        auto stand = [&](bool local) { TrafficActor x; x.pos = aSpot; x.local = local; return x; };
        // (ids: H 1, A 2, B 3; each sees the others late)
        auto actors = [&](double t, int me) {
            std::vector<TrafficActor> v;
            if (t < MIG) { TrafficActor h = skater(me == 1 ? t : t - SEEN_LATE, 1); h.local = me == 1; v.push_back(h); }
            v.push_back(stand(me == 2));
            TrafficActor b = skater(me == 3 ? t : t - SEEN_LATE, 2); b.local = me == 3; v.push_back(b);
            return v;
        };
        std::map<long long, std::vector<CarRec>> aAt;
        double prevA = -1, prevB = -1, prevT = -1, nextSend = 0;
        int hitCar = -1;
        bool hit = false, bSwitched = false;
        for (int f = 0; f <= 60 * (int)(MIG + 15); f++) {
            double t = f / 60.0;
            bool hGone = t >= MIG, bKnows = t >= MIG + 0.03;   // (HOST_CHANGED reaches B a little after A)
            float a;
            if (!hGone) {
                hts.hostStep(H, W, actors(t, 1), t, &a);
                if (t >= nextSend - 1e-9) {
                    nextSend = std::max(nextSend + TrafficSync::INTERVAL, t);
                    for (auto& m : TrafficSync::snapshot(H, {aSpot, skater(t - LAT - 0.015, 2).pos}, RTT)) { toA.send(t, m); toB.send(t, m); }
                }
            } else if (f == (int)(MIG * 60)) {
                toA.q.clear(); toB.q.clear();   // (the relay drops the old host's: it's gone)
                ats.becomeHost(A, t + 500);
                nextSend = t;
            }
            // events: B's hit goes to the room (H and A); H never gets it
            while (!ev.q.empty() && ev.q.begin()->first <= t) {
                const netp::MsgEvent& e = ev.q.begin()->second.second;
                ats.noteHit(e, A);   // (H: not answering any more)
                if (hGone) TrafficSync::applyCarHit(A, e);
                ev.q.erase(ev.q.begin());
            }
            if (!hGone) {
                toA.deliver(t, [&](const netp::MsgCarSnapshot& m) { ats.onCarSnapshot(m, t + 500, RTT, A); });
                ats.step(A, W, actors(t, 2), t + 500, &a);
            } else {
                ats.hostStep(A, W, actors(t, 2), t + 500, &a);
                if (t >= nextSend - 1e-9) {
                    nextSend = std::max(nextSend + TrafficSync::INTERVAL, t);
                    for (auto& m : TrafficSync::snapshot(A, {skater(t - LAT - 0.015, 2).pos}, RTT)) aToB.send(t, m);
                }
                long long as = stepOf(A.simTime);
                if (as % 20 == 0) aAt[as] = carsOf(A);
            }
            if (bKnows && !bSwitched) { bSwitched = true; bts.hostChanged(); toB.q.clear(); out.appliedBefore = bts.applied; }
            toB.deliver(t, [&](const netp::MsgCarSnapshot& m) { if (!bKnows) bts.onCarSnapshot(m, t + 777, RTT, B); });
            aToB.deliver(t, [&](const netp::MsgCarSnapshot& m) { if (bKnows) bts.onCarSnapshot(m, t + 777, RTT, B); });
            bts.step(B, W, actors(t, 3), t + 777, &a);
            // B runs into a car 0.2 s before H leaves
            if (!hit && t >= MIG - 0.2) {
                hit = true;
                Vector3 me = skater(t, 2).pos;
                float best = 1e9f;
                for (size_t k = 0; k < B.cars.size(); k++) {
                    const Car& c = B.cars[k];
                    float d2 = sq(c.pos.x - me.x) + sq(c.pos.z - me.z);
                    if (c.placed && c.speed > 4 && d2 < 80 * 80 && d2 < best) { best = d2; hitCar = (int)k; }
                }
                if (hitCar >= 0) {
                    B.cars[hitCar].stopT = 3; B.cars[hitCar].speed = 0;
                    ev.send(t, 0, bts.carHit(B, hitCar));
                }
            }
            // the session clocks, every frame (H's: its traffic time, running on)
            double sa, sb, sh;
            bool okA = ats.sessionNow(hGone, t + 500, &sa), okB = bts.sessionNow(false, t + 777, &sb), okH = !hGone && hts.sessionNow(true, t, &sh);
            if (t > 10 && okA && okB) {
                if (prevA >= 0) {
                    out.worstStepA = std::max(out.worstStepA, fabs((sa - prevA) - (t - prevT)));
                    out.worstStepB = std::max(out.worstStepB, fabs((sb - prevB) - (t - prevT)));
                }
                if (t > MIG + 5) out.worstAB = std::max(out.worstAB, fabs(sa - sb));
                if (okH) out.worstHB = std::max(out.worstHB, fabs(sh - sb));
                prevA = sa; prevB = sb; prevT = t;
            }
            if (bKnows && out.firstAfter < 0 && bts.applied > out.appliedBefore) out.firstAfter = t - MIG;
            long long bs = stepOf(B.simTime);
            auto it = aAt.find(bs);
            if (it != aAt.end()) {
                if (t > MIG + 4) compareNear(it->second, B, skater(t, 2).pos, 100, out.nearB);
                if (hitCar >= 0 && t > MIG + 0.5 && t < MIG + 2.5) {   // (still in its 3 s stop)
                    out.hitStopped = (out.hitErr < 0 || out.hitStopped) && A.cars[hitCar].speed == 0 && B.cars[hitCar].speed == 0;
                    out.hitErr = std::max(out.hitErr, sqrtf(sq(it->second[hitCar].pos.x - B.cars[hitCar].pos.x) + sq(it->second[hitCar].pos.z - B.cars[hitCar].pos.z)));
                }
                aAt.erase(aAt.begin(), it);
            }
        }
        out.appliedAfter = bts.applied - out.appliedBefore;
        out.heldAtEnd = bts.held();
        return out;
    };

    auto e1 = std::async(std::launch::async, runEvents, true), e2 = std::async(std::launch::async, runEvents, false);
    auto m1 = std::async(std::launch::async, runMigrate);
    {   // gondolas on the session clock: where they are at a time doesn't depend on how they got there (a pause, a join),
        // and is where running them all along would have them
        Gondolas G1, G2, G3;
        G1.init(W); G2.init(W); G3.init(W);
        for (int f = 0; f < 60 * 300; f++) {
            if (f < 60 * 100 || f > 60 * 140) G1.update(1 / 60.f, W);   // (paused 40 s)
            G3.update(1 / 60.f, W);
        }
        G1.setTime(300, W); G2.setTime(300, W);
        float same = 0, along = 0;
        for (size_t k = 0; k < G1.cabins.size(); k++) {
            float L2 = 2 * W.gondolas[G1.cabins[k].line].len, d = fabsf(G2.cabins[k].u - G3.cabins[k].u);
            same = std::max(same, fabsf(G1.cabins[k].u - G2.cabins[k].u));
            along = std::max(along, std::min(d, L2 - d));
        }
        snprintf(b, sizeof b, "%zu cabins at 300 s: paused for 40 s or not, worst %.4f m apart; against running all along, worst %.2f m", G1.cabins.size(), same, along);
        check(G1.cabins.size() > 10 && same == 0 && along < 0.5f, "gondolas on the session clock", b);
    }
    {   // a knock-down and a dodge as events: through the wire format and onto the same person; anything out of range refused
        Traffic T1;
        T1.init(W);
        int walker = -1;
        for (size_t k = 0; k < T1.peds.size() && walker < 0; k++) if (T1.peds[k].mode == 0) walker = (int)k;
        PedEvent kn{walker, PedEvent::KNOCK, {0, 0}, 0}, dv{walker + 1, EV_DIVE, {0, 1}, 3.f}, got;
        auto through = [&](const PedEvent& e) {
            netp::MsgEvent m;
            std::vector<uint8_t> bytes = netp::pack(pedEventMsg(e));
            return netp::unpack(bytes.data(), bytes.size(), m) && pedEventOf(m, &got) && T1.applyPedEvent(got);
        };
        bool ok = walker >= 0 && through(kn) && T1.peds[walker].knock == 4.0f && T1.peds[walker].evade == EV_NONE;
        bool dive = T1.peds[walker + 1].mode == 3 || (through(dv) && T1.peds[walker + 1].evade == EV_DIVE && T1.peds[walker + 1].evTo.y == T1.peds[walker + 1].evFrom.y + 3.f);
        PedEvent out{(int)T1.peds.size(), PedEvent::KNOCK, {0, 0}, 0}, neg{-1, EV_STEP, {1, 0}, 1};
        bool refused = !T1.applyPedEvent(out) && !T1.applyPedEvent(neg);
        snprintf(b, sizeof b, "person %d knocked down %d, the next one dives the same way %d; person %zu and -1 refused %d", walker, (int)ok, (int)dive,
                 T1.peds.size(), (int)refused);
        check(ok && dive && refused, "people events applied", b);
    }
    {   // honks: in a session the cars honk at every player; only those near you are heard
        Traffic T1;
        T1.init(W);
        int honkSteps = 0, nearHeard = 0, farHeard = 0;
        Vector3 far = {n0.x + 1200, 0, n0.y + 900};
        TrafficActor sk;   // (standing in a lane, mid-block: cars come up on them and honk)
        sk.pos = {n0.x + 2.5f * g.sp, 0, n0.y + 2.4f};
        sk.pos.y = W.terrain(sk.pos.x, sk.pos.z) + 0.05f;
        for (int f = 0; f < 60 * 60; f++) {
            T1.beginFrame();
            T1.update(Traffic::STEP, W, {sk});
            honkSteps += T1.evHonk;
            nearHeard += T1.evHonk && honkNear(T1, sk.pos);
            farHeard += honkNear(T1, far);
        }
        snprintf(b, sizeof b, "60 s standing in a lane: %d steps with a honk; heard by the skater %d, by someone 1.5 km away %d", honkSteps, nearHeard, farHeard);
        check(honkSteps > 0 && nearHeard == honkSteps && farHeard == 0, "honks heard only nearby", b);
    }
    EvRun ev = e1.get(), noEv = e2.get();
    MigRun mg = m1.get();
    snprintf(b, sizeof b, "car %d hit by the client at %.0f s: stopped on the host %.0f ms later (%.0f ms on in traffic time), held on the client %.2f s, "
             "fastest there in its stop %.2f m/s; settling in 0.8 s, up to %.3f m apart; then %d steps compared (stopped, and driving on), worst %.3f m; "
             "host speed 4.5 s on %.1f; snapshot parts applied %d",
             ev.hitCar, HIT_AT, ev.hostDelay * 1000, ev.hostAge * 1000, ev.heldFor, ev.clientSpeed, ev.worstSettling, ev.afterN, ev.worstAfter, ev.hostSpeedAt,
             ev.applied);
    check(ev.hitCar >= 0 && ev.hostDelay >= 0 && ev.hostDelay < 0.15 && ev.hostAge < 0.3 && ev.heldFor > 0 && ev.heldFor < 1.0 && ev.clientSpeed == 0 &&
          ev.worstSettling < 1.f && ev.afterN > 200 && ev.worstAfter < 0.05f && ev.hostSpeedAt > 0.5f,
          "car hit: the host stops the car where the client did", b);
    PedNear& pe = ev.peds, &pn = noEv.peds;
    snprintf(b, sizeof b, "people within 25 m of a skater, %d samples: with events (%d sent by the host, %d by the client, %d applied): %.2f/%.2f/%.2f m apart "
             "(median/90%%/worst), %.2f%% mid-dodge on one side only; each guessing the other's (%d samples): %.2f/%.2f/%.2f m, %.2f%%",
             pe.n, ev.sentH, ev.sentC, ev.appliedPed, pct(pe.d, 0.5f), pct(pe.d, 0.9f), pct(pe.d, 1.f), pe.n ? 100.0 * pe.evadeOdd / pe.n : 0,
             pn.n, pct(pn.d, 0.5f), pct(pn.d, 0.9f), pct(pn.d, 1.f), pn.n ? 100.0 * pn.evadeOdd / pn.n : 0);
    check(pe.n > 1000 && ev.sentH > 5 && ev.sentC > 5 && ev.appliedPed == ev.sentH + ev.sentC && pct(pe.d, 0.9f) < 0.5f &&
          pct(pe.d, 1.f) < pct(pn.d, 1.f), "people dodge the same for everyone", b);
    snprintf(b, sizeof b, "session clock steps off the frame time by at most %.2f ms (new host) / %.2f ms (client); old host vs client %.1f ms, new host vs client "
             "%.1f ms; client's snapshot parts: %d before, %d after (first %.2f s after); then near the client %d samples, worst %.3f m, %d off route; "
             "the car hit just before: stopped on both %d, %.3f m apart; cars held at the end %zu",
             mg.worstStepA * 1000, mg.worstStepB * 1000, mg.worstHB * 1000, mg.worstAB * 1000, mg.appliedBefore, mg.appliedAfter, mg.firstAfter, mg.nearB.n,
             mg.nearB.worst, mg.nearB.routeBad, (int)mg.hitStopped, mg.hitErr, mg.heldAtEnd);
    check(mg.worstStepA < 0.005 && mg.worstStepB < 0.005 && mg.worstHB < 0.05 && mg.worstAB < 0.05 && mg.appliedAfter > 300 && mg.firstAfter >= 0 && mg.firstAfter < 2 &&
          mg.nearB.n > 100 && mg.nearB.worst < 0.5f && mg.nearB.routeBad == 0 && mg.hitStopped && mg.hitErr >= 0 && mg.hitErr < 0.05f,
          "host handover: the clock carries on", b);
}
