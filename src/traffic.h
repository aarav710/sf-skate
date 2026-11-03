// Cars, buses, cable cars and pedestrians.
#pragma once
#include "common.h"
#include "world.h"
#include "player.h"
#include <unordered_map>

struct Car {
    int type = 0;
    Color body{};
    int mode = 0;               // 0 = street grid, 1 = path (bridges, Market St), 2 = cable car, 3 = Muni Metro
    const Grid* g = nullptr;
    int i = 0, j = 0, d = 0;
    int pd = -1, nd = -1;       // grid: direction of the previous / next block, so corners can be rounded
    float t = 0;
    float uturn = -1;           // path cars: distance into a U-turn at the end of the road (-1 = not turning)
    float xover = 1e9f;         // rail cars: distance since reversing (crossing over to the other track)
    int path = 0;
    float s = 0;
    int pdir = 1;
    float lane = 2.4f;
    float speed = 0, maxSpeed = 12, stopT = 0, honkCD = 0;
    Vector3 pos{};
    float yaw = 0, pitch = 0;
    bool braking = false, placed = false;
    float pedal = 0;            // crank angle (pedal buses)
    float acrossT = -1;         // grid cars: when a car on a main road last stopped for this one (Traffic::clock)
    int box = -1, wantBox = -1; // grid cars: the intersection (node j * ni + i) this one is cleared through / waiting at
    float boxSince = 0;         // (Traffic::clock) waiting: since when (the longest wait goes first; 1e9 while it's only
                                // waiting for room in the road out); cleared: when it last got anywhere
    bool queued = false;        // grid cars: held up behind the car ahead (or for room past a main road), so not about
                                // to pull out across a main road whatever its traffic does
    float still = 0;            // how long it's been stood still (s)
    bool passing = false;       // main-road cars: given up waiting for grid cars stood in the way, easing past them
    // where it's drawn (and its roof is): between where it was at the start of its latest step (prev) and where it is
    // now, by how far the frame has got toward the next step (see Traffic::advance). The simulation never reads these.
    Vector3 prevPos{}, vpos{};
    float prevYaw = 0, prevPitch = 0, vyaw = 0, vpitch = 0;
    bool prevPlaced = false, vplaced = false;
    Vector3 lastPos{};          // where it was drawn last frame, i.e. the roof the skater stood on (see carryRider)
    float lastYaw = 0, lastPitch = 0;
    bool lastPlaced = false;
    uint16_t hops = 0;          // grid cars: turns picked so far (each pick is hashed from the car, the node and this)
};

struct Ped {
    int mode = 0;               // 0 = sidewalks, 1 = path, 2 = standing around, 3 = sitting (bench or lawn)
    const Grid* g = nullptr;
    int i = 0, j = 0, qx = 1, qz = 1;
    Vector2 from{}, to{};
    float t = 0, len = 1;
    int path = 0;
    float s = 0;
    int pdir = 1;
    float lane = 0;
    Vector3 pos{};
    float yaw = 0, phase = 0, speed = 1.3f, scale = 1;
    Color shirt{}, pants{}, skin{}, hair{};
    float knock = 0, idleTurn = 0;
    Vector2 dodge{};            // how far they've stepped off their route to let the skater by
    // getting out of the skater's way: a brisk sidestep, backing off facing the skater, a hop aside, or a dive
    uint8_t evade = 0;          // EV_NONE .. EV_DIVE
    float evT = 0, evDur = 0;   // time into the move, and how long the whole move (landing, getting up) takes
    float evMove = 0;           // the part of it spent travelling
    Vector2 evFrom{}, evTo{};   // dodge offset at the start and the end of the move
    float evY = 0, evTilt = 0;  // off the ground (hop, dive), and how far over toward lying flat (0..1)
    float gait = 0;             // how fast they're actually moving (m/s, smoothed): drives the legs and the facing
    uint8_t pet = 0;            // 0 none, 1 a dog on a leash
    Color petC{}, petC2{};      // coat and the darker ears / patches
    float petS = 1;             // dog size
    Vector3 dog{};              // where the dog stands: on the ground there and clear of walls (see dogSpot in traffic.cpp)
    Vector2 dogOff{};           // its offset from the walker, eased so it trots over rather than jumping when the spot moves
    float dogSide = 1;          // which side of the walker it keeps to (+1 / -1); it only switches when that side is blocked
    // looks: hair style, what they carry, what they do while standing, build, and the bag / hat color
    uint8_t hairStyle = 0, carry = 0, idle = 0;
    float build = 1;
    Color extra{};
    float wait = 0;             // waiting at a corner to cross
    bool grass = false;         // sitting on a blanket on the lawn rather than on a bench
    float lag = 0;              // far away: time not placed yet (they're placed a few at a time, see Traffic::update)
    uint32_t rolls = 0;         // random picks made so far (each one is hashed from the person and this)
};
enum { EV_NONE, EV_STEP, EV_BACK, EV_HOP, EV_DIVE };
enum { HAIR_SHORT, HAIR_LONG, HAIR_BUN, HAIR_BALD, HAIR_CAP, HAIR_CURLY };
enum { CARRY_NONE, CARRY_BACKPACK, CARRY_SHOULDER, CARRY_SHOPPING, CARRY_PHONE, CARRY_LAPTOP };
enum { IDLE_WAIT, IDLE_CHAT, IDLE_CALL, IDLE_ARMS };

// Cats on front steps and sidewalks: sitting, loafing or grooming, and bolting if you skate at them.
struct Cat {
    Vector3 pos{};
    float yaw = 0, t = 0, flee = 0;
    int pose = 0;               // 0 sitting, 1 loafing, 2 running
    Color c{}, patch{};
};

struct Train {
    float s; int dir; float v; float dwell;
    float prevS = 0, vs = 0;    // drawn: as Car::vpos (prevS: at the start of the latest step)
};
// Where a grid street's lane crosses the Caltrain tracks at grade, in m along the block from its node: the line a car's
// front stops at short of them, and where the lane is clear of them on the far side; s: along the line, at the crossing
struct RailCrossing { float stop = 1e9f, clear = 0, s = 0; };   // stop 1e9: no crossing

// Someone the traffic reacts to: cars stop for them, people get out of their way, cats bolt. The local skater, or
// in a session every player, so all machines run the same traffic.
struct TrafficActor {
    Vector3 pos{};
    Vector2 vel{};              // over the ground (m/s)
    float hspeed = 0;           // as Player::hspeed
    int state = 0;              // Player::State
    static TrafficActor of(const Player& p);
};

struct Traffic {
    std::vector<Car> cars;
    std::vector<Ped> peds;
    std::vector<Cat> cats;
    std::vector<Train> trains; // Caltrain
    bool evHonk = false, evCrash = false;
    float clock = 0;            // drives the traffic signals: simTime, as a float (never accumulated on its own, so it
                                // can't drift from simTime however long a session runs)
    double simTime = 0;         // time simulated (fixed steps of STEP, see advance); in a session, the session clock
    double acc = 0;             // frame time not simulated yet
    static constexpr float STEP = 1.f / 60;
    static constexpr int MAX_STEPS = 8;   // a frame runs at most this many steps; any backlog past that is dropped
    Vector3 view = {1e6f, 0, 1e6f};   // the camera, as of last frame: people near it get the full update too
    int farTurn = 0;            // which slice of the far-away people gets its catch-up update this frame
    std::unordered_map<int, std::vector<int>> boxUsers;   // grid intersection -> cars cleared through or waiting there
    std::vector<uint8_t> nearMain[3];   // per node of the SF, Oakland and Menlo Park grids: 1 close to a main road
    std::vector<float> exitTail[3];     // per node and heading on those grids: the back of the last car stopped (or all
                                        // but) in the road leaving it, in m from the node; rebuilt every frame
    std::vector<Vector2> mainBand[3];   // per node and heading on those grids: where the lane leaving it crosses a main
                                        // road (from, to: m from the node, wide enough for both cars; from 1e9: none)
    std::vector<RailCrossing> railX[3]; // per node and heading on those grids: the lane leaving it over the Caltrain tracks
    std::vector<std::vector<Vector2>> pathCross;   // per car path and way along it (path * 2 + (pdir > 0)): where its
                                                   // lanes cross a grid street (from, to: s), in order
    std::vector<uint64_t> carCells;   // (40 m cell << 32 | car index), sorted: who's near whom, for every car each frame

    // seed: the cars' types, colors and places and the people's looks; the same seed gives the same traffic
    void init(World& w, uint32_t seed = 20240);
    // one step of the simulation, reacting to everyone in `actors`. Nothing in it depends on the order things are
    // updated in, the camera or the frame rate (given fixed steps), so machines with the same actors stay identical
    void update(float dt, World& w, const std::vector<TrafficActor>& actors);
    // one step for just the local skater, which also carries them along on a car roof and publishes the roofs
    void update(float dt, World& w, Player& p);
    // a frame of the game: as many fixed steps as frameDt adds up to (0 or more), then the skater's roof ride and the
    // roofs. Returns the steps run.
    int advance(float frameDt, World& w, Player& p);
    // the same for everyone in `actors` (the skater among them; in a session, every player)
    int advance(float frameDt, World& w, Player& p, const std::vector<TrafficActor>& actors);
    // the end of a frame of steps: drawn alpha of the way into the next step (see interpolate), the skater's roof ride
    // over frameDt, and the roofs published around them
    void endFrame(float alpha, float frameDt, World& w, Player& p);
    int takeSteps(float frameDt);      // the accumulator: steps owed for a frame of frameDt
    void beginFrame();                 // remember where the roofs are (as published), for carryRider after the steps
    // place everything where it's drawn: alpha of the way from the start of the latest step to its end (1: at the end)
    void interpolate(float alpha);
    int rideCar = -1;                  // the car (index) the skater was last carried on, -1 if none
    void interact(Player& p);
    // put the roofs of the vehicles near `at` into w.vehicleTops, for the skater to land on (update does this each frame)
    void publishRoofs(World& w, Vector3 at) const;
    void carryRider(Player& pl, float dt);
    // nearPass: everything within reach of the near clip range; otherwise only cars far enough to need the far pass
    void draw(Vector3 cam, Vector3 camFwd, bool nearPass) const;
    // sun shadow casters: cars within rCars of c, people (and their dogs) within rPeds
    void drawCasters(Vector3 c, float rCars, float rPeds) const;
    void drawShadows(Vector3 cam) const;
    void drawTrains(const World& w, Vector3 cam) const;
    void drawSignals(const World& w, Vector3 cam) const;   // the lamps of the traffic lights, cycling
    static void drawTrain(const World& w, float s, int dir, Vector3 cam);
    static void startEvade(Ped& p, int kind, Vector2 dir, float dist);   // set a person off on a dodge: dir unit, dist metres

    // snapshots, for a host to correct other machines' traffic. serializeCars: the cars listed, compactly (position,
    // heading, speed, route); applyCars snaps the cars to them (snapPos false: only the route, and the car eases over
    // to it on its own as it drives, see update). serializeFull: everything that drives the cars and trains from
    // here on, so init + applyFull on another machine carries on exactly as this one does. People aren't included.
    void serializeCars(const std::vector<int>& idx, std::vector<uint8_t>& out) const;
    // keepClose: a car already on the same route within a few cm and at the speed sent keeps its own state, so a
    // machine whose prediction was right stays bit-identical to the host instead of taking the rounding
    bool applyCars(const uint8_t* data, size_t n, bool snapPos = true, bool keepClose = false);
    void serializeFull(std::vector<uint8_t>& out) const;
    bool applyFull(const uint8_t* data, size_t n);
    // the seed and the time a full snapshot was taken at, without applying it (false if it isn't one)
    static bool peekFull(const uint8_t* data, size_t n, uint32_t* seed, double* simTime);
    uint32_t seedOf() const { return seed; }
    // the Caltrain trains alone (13 B each), for every car snapshot; applyTrains turns down a bad one whole
    void serializeTrains(std::vector<uint8_t>& out) const;
    bool applyTrains(const uint8_t* data, size_t n);
    // People walk their routes `steps` steps on, as the fixed steps would have taken them with nobody about (each
    // one's route is their own, see pedRoute), and are placed there. For a machine that joins a session: right after
    // init, with the steps the session has run, its people are where the host's are (bar any the skaters disturbed).
    // (people from..to-1 only: the catching up can be spread over frames, as each person's route is their own)
    void catchUpPeds(long long steps, World& w, size_t from = 0, size_t to = (size_t)-1);

private:
    RNG rng{20240};                    // init's draws only: nothing in update takes from a shared stream
    uint32_t seed = 20240, seedMix = 0;   // (seedMix: mixed into each car's and person's own picks)
    const Grid* grids[3] = {};         // (sf, oak, mp) as in init
    void placeGridPed(Ped& p, RNG& r);
    void nextPedMove(Ped& p, RNG& r);
    void pedRoute(Ped& p, int idx, float dt, bool hold);
    void pedPlaceFar(Ped& p, float dt, World& w);
    int gridOf(const Car& c) const { return c.g == grids[0] ? 0 : c.g == grids[1] ? 1 : 2; }
    void unbox(int idx);               // take a car out of the intersections it's cleared through or waiting at
    void rebuildBoxUsers();
};
