// Cars, buses, cable cars and pedestrians.
#pragma once
#include "common.h"
#include "world.h"
#include "player.h"

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
};

struct Ped {
    int mode = 0;               // 0 = sidewalks, 1 = path, 2 = standing around
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
    Vector2 dodge{};
    uint8_t pet = 0;            // 0 none, 1 a dog on a leash
    Color petC{}, petC2{};      // coat and the darker ears / patches
    float petS = 1;             // dog size
    // looks: hair style, what they carry, what they do while standing, build, and the bag / hat color
    uint8_t hairStyle = 0, carry = 0, idle = 0;
    float build = 1;
    Color extra{};
    float wait = 0;             // waiting at a corner to cross
};
enum { HAIR_SHORT, HAIR_LONG, HAIR_BUN, HAIR_BALD, HAIR_CAP, HAIR_CURLY };
enum { CARRY_NONE, CARRY_BACKPACK, CARRY_SHOULDER, CARRY_SHOPPING, CARRY_PHONE };
enum { IDLE_WAIT, IDLE_CHAT, IDLE_CALL, IDLE_ARMS };

// Cats on front steps and sidewalks: sitting, loafing or grooming, and bolting if you skate at them.
struct Cat {
    Vector3 pos{};
    float yaw = 0, t = 0, flee = 0;
    int pose = 0;               // 0 sitting, 1 loafing, 2 running
    Color c{}, patch{};
};

struct Train { float s; int dir; float v; float dwell; };

struct Traffic {
    std::vector<Car> cars;
    std::vector<Ped> peds;
    std::vector<Cat> cats;
    std::vector<Train> trains; // Caltrain
    bool evHonk = false, evCrash = false;
    float clock = 0;            // drives the traffic signals

    void init(World& w);
    void update(float dt, World& w, const Player& p);
    void interact(Player& p);
    // nearPass: everything within reach of the near clip range; otherwise only cars far enough to need the far pass
    void draw(Vector3 cam, Vector3 camFwd, bool nearPass) const;
    // sun shadow casters: cars within rCars of c, people (and their dogs) within rPeds
    void drawCasters(Vector3 c, float rCars, float rPeds) const;
    void drawShadows(Vector3 cam) const;
    void drawTrains(const World& w, Vector3 cam) const;
    void drawSignals(const World& w, Vector3 cam) const;   // the lamps of the traffic lights, cycling
    static void drawTrain(const World& w, float s, int dir, Vector3 cam);

private:
    void placeGridPed(Ped& p, RNG& r);
    void nextPedMove(Ped& p, RNG& r);
};
