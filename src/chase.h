// Police chase: cruisers hunt you through the streets, officers get out and run you down (through doors too),
// and a caught skater can fight their way free. Lose them, or get busted.
#pragma once
#include "common.h"
#include "world.h"
#include "player.h"

struct Cop {
    Vector3 pos{};
    float yaw = 0, speed = 0, stuck = 0, side = 1;
    bool onFoot = false;             // out of the cruiser and running
    float stun = 0;                  // knocked down: lying there for this many more seconds
    float phase = 0;                 // run cycle
    float lostT = 0;                 // seconds spent far behind
    int stage = 0, house = -1;       // door routing: 0 direct, 1 heading to the near side of a door, 2 through it
    bool leaving = false;            // routing out of a house rather than into one
};

struct ParkedCruiser { Vector3 pos; float yaw; };

struct Chase {
    bool on = true;
    int difficulty = 1;              // 0 easy, 1 normal, 2 hard: cop speed, how fast heat builds, how hard they hold on
    static constexpr const char* DIFF_NAMES[3] = {"Easy", "Normal", "Hard"};
    bool active = false;             // cops are after you
    float calmT = 8;                 // seconds until the next patrol spots you
    int stars = 0;
    float chaseT = 0, hideT = 0, bustT = 0, heatAcc = 0;
    float nearest = 1e9f;            // meters to the closest cop
    std::vector<Cop> cops;
    std::vector<ParkedCruiser> parked; // cruisers the officers got out of
    int escapes = 0, busts = 0, bestStars = 0, shoves = 0;
    long long scoreAtStart = 0;
    Vector3 lastPos{};
    bool hasLast = false;
    RNG rng{911};

    // a cop has you: tug of war between their grip and your struggling
    bool scuffle = false;
    int scCop = -1;
    float meter = 0;                 // 0 = busted, 1 = break free
    float grace = 0;                 // seconds after a scuffle when nobody can grab you again
    float strike = 0;                // struggle input this frame, filled in by main (mash keys, board swings)
    float hitCD = 0;

    void reset(float calm);
    void update(float dt, Player& p, World& w);

    static constexpr float ESCAPE_DIST = 150, ESCAPE_TIME = 5, BUST_TIME = 1.4f;

    // events for sound effects, read and cleared by main
    bool evStart = false, evEscape = false, evBusted = false, evGrab = false, evShove = false, evBowled = false;

private:
    void spawnCop(const Player& p, const World& w, float minD, float maxD, bool ahead);
    void moveCar(Cop& c, float dt, const Player& p, const World& w);
    void moveFoot(Cop& c, float dt, const Player& p, const World& w);
    void arrest(Player& p);
    void breakFree(Player& p);
};
