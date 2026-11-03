// Shared traffic in a multiplayer session (plan section 2): the host's traffic time is the session clock. The host
// sends the cars near the other players 10 times a second (and a full snapshot to anyone who joins); everyone else
// runs the same fixed-step traffic on the session clock, with every player as an actor, and takes the host's cars
// at the very step they were taken at. No networking here: main feeds it from Net, the tests from a simulated link.
#pragma once
#include "traffic.h"
#include "net_protocol.h"
#include <deque>
#include <map>

// An offset from our clock to another one that changes smoothly: toward its target at most SLEW s per s (so a clock
// read through it runs 5% fast or slow, never backwards), or in one jump when it's more than SNAP off.
struct SlewedOffset {
    static constexpr double SLEW = 0.05, SNAP = 0.25;
    double cur = 0, target = 0, last = 0;
    bool valid = false;
    void aim(double t);              // a new target (the first one is taken at once)
    double at(double local);         // local + the offset, slewed on to `local` (call with local never decreasing)
};

// The host's traffic time as seen from here, from the snapshots it stamps with it.
struct SessionClock {
    static constexpr int WINDOW = 31;             // the snapshots the estimate goes by (~3 s of them)
    static constexpr double MARGIN_MIN = 0.03, MARGIN_MAX = 0.5;
    std::deque<double> seen;                      // simTime - recvAt of the latest snapshots: their arrival offsets
    double oneWay = -1;                           // host -> relay -> here (s), smoothed from the round trips
    double margin = 0.05;                         // how far the traffic runs behind the snapshots arriving now
    SlewedOffset session, traffic;
    void reset() { *this = SessionClock(); }
    // a snapshot the host took at simTime arrived at our recvAt; round trips to the relay in ms (< 0: not known yet)
    void sample(double simTime, double recvAt, double hostRttMs, double myRttMs);
    bool valid() const { return traffic.valid; }
    // the host's traffic time right now: what it was when the latest snapshots left, plus their trip here
    double sessionTime(double local) { return session.at(local); }
    // where this machine's traffic runs: just behind the snapshots arriving now (by `margin`, the spread of their
    // arrival times), so each one gets here before the traffic reaches the step it was taken at
    double trafficTime(double local) { return traffic.at(local); }
};

class TrafficSync {
public:
    static constexpr float R_SYNC = 350;          // the host sends the cars this close to another player
    static constexpr int MAX_CARS = 160, CARS_PER_PART = 40;   // (40 cars: ~1.1 KB, one packet)
    static constexpr double INTERVAL = 0.1;       // 10 a second
    static constexpr double RESYNC = 5.0;         // further off the session clock than this: ask for a full snapshot
    static constexpr double ASK_EVERY = 10.0;     // asking again while one doesn't come
    static constexpr long long CATCHUP_MAX = 20 * 60 * 60;   // people catch up on at most 20 min of steps (~0.5 s)

    // --- host
    // the cars within R_SYNC of any of `others`, nearest one first, at most MAX_CARS
    static std::vector<int> pickCars(const Traffic& T, const std::vector<Vector3>& others);
    // the snapshot for now, in parts (always at least one: the trains and the clock)
    static std::vector<netp::MsgCarSnapshot> snapshot(const Traffic& T, const std::vector<Vector3>& others, double rttMs);

    // --- everyone else
    SessionClock clock;
    bool synced = false;        // runs the host's traffic (a full snapshot applied since joining)
    bool wantFull = true;       // needs one: just joined, or fell out of step
    double askedAt = -1e18;
    int applied = 0, late = 0, stale = 0, bad = 0;   // snapshot parts: taken, too late, older than one taken, turned down

    void onCarSnapshot(const netp::MsgCarSnapshot& m, double recvAt, double myRttMs, const Traffic& T);
    // a full snapshot: traffic started over from the host's (people caught up as well as they can be). False if bad.
    bool onFull(Traffic& T, World& w, const uint8_t* data, size_t n);
    // whether to ask the host for a full snapshot now (then asks again every ASK_EVERY until one comes)
    bool shouldAsk(double now);
    // A frame on a synced machine: T's steps up to the traffic time for our clock `now` (at most MAX_STEPS), each
    // snapshot applied right at the step it was taken at. *alpha: how far the frame is into the next step, for
    // Traffic::endFrame. Returns the steps run; -1 if there's no clock yet (the caller runs T on its own frames).
    int step(Traffic& T, World& w, const std::vector<TrafficActor>& actors, double now, float* alpha);
    void hostChanged();         // the old host's snapshots go, and the clock starts over from the new one's
    void reset() { *this = TrafficSync(); }
    size_t pending() const { return pending_.size(); }

private:
    std::map<long long, std::vector<netp::MsgCarSnapshot>> pending_;   // by the step they were taken at
    long long lastApplied_ = -1;
    void applyDue(Traffic& T);
};

// another player as the traffic sees them, from the state they sent (as TrafficActor::of for the local skater)
TrafficActor actorOf(const netp::PlayerState& s);

// the step a traffic time falls on (simTime is a whole number of steps, added up in doubles)
inline long long stepOf(double simTime) { return llround(simTime / (double)Traffic::STEP); }
