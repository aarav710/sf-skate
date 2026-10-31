// The skater: riding physics, ollies, flip tricks, grinds and bails.
#pragma once
#include "common.h"
#include "world.h"

struct Input {
    float steer = 0;       // -1 left .. +1 right
    bool push = false, brake = false, tuck = false;
    bool ollie = false;    // pressed this frame
    bool ollieHeld = false;
    bool kickflip = false, heelflip = false, shove = false; // pressed this frame
    bool grab = false;     // held
};

struct Sfx; // audio hooks

struct Player {
    enum State { RIDE, AIR, GRIND, BAIL };
    State state = RIDE;
    Vector3 pos{0, 0, 0};
    float yaw = 0;            // direction the board points
    float speed = 0;          // along the board while riding / grinding
    Vector3 vel{0, 0, 0};     // horizontal velocity while airborne
    float vy = 0;             // vertical velocity
    float rideVy = 0;         // vertical speed implied by the slope while riding

    // animation
    float crouch = 0, lean = 0, pushPhase = 0, pushAmt = 0;
    float pitch = 0;   // board tilt along the ground or rail (radians, + = nose up), so it sits on hills instead of in them
    float flipT = -1, flipDur = 0.42f, flipDir = 1; // board roll
    float shoveT = -1, shoveDur = 0.4f;           // board spin
    float flipAngle = 0, shoveAngle = 0;
    float grabT = 0;
    float bailT = 0;
    bool splashed = false;
    bool airClip = false;      // brushed a wall or pole this jump: the landing can't be a stomp
    Vector3 boardPos{}, boardVel{};
    float boardYaw = 0;

    // tricks & score
    float spinAccum = 0, airTime = 0;
    std::vector<std::string> combo;
    float comboPts = 0;
    long long score = 0;
    int bestCombo = 0;
    bool grabbed = false;     // Indy Grab already scored this jump
    bool grabbing = false;    // holding the board right now (the pose follows this)
    std::string popup; float popupT = 0; Color popupCol = WHITE;
    std::string subPopup;
    std::string landNote;     // how the last landing went, shown with the banked combo
    float landShake = 0;      // 0..1 camera kick from a hard landing, decays in the camera
    bool bouncing = false;    // AIR is just the hop after a landing: no spins, flips or grabs
    float bounceY = 0, bounceRate = 0; // ground height and its rise/fall rate along the hop at its start; ground well below that line = rolled off into real air
    bool ollieQueued = false; // Space pressed during the hop: pop as the wheels come back down

    // grind
    int rail = -1; float railT = 0; int railDir = 1; float balance = 0, balanceVel = 0;
    bool boardslide = false;

    // respawn
    Vector3 safePos{}; float safeYaw = 0; float safeT = 0;

    void spawn(Vector3 p, float yaw);
    void update(float dt, const Input& in, World& w);
    void bail(const char* why);
    Vector3 fwd() const { return {cosf(yaw), 0, sinf(yaw)}; }
    float hspeed() const { return state == AIR ? sqrtf(vel.x * vel.x + vel.z * vel.z) : fabsf(speed); }
    void draw() const;
    void say(const std::string& s, Color c, const std::string& sub = "") { popup = s; subPopup = sub; popupT = 2.2f; popupCol = c; }

    // events for sound effects, read and cleared by main each frame
    bool evPop = false, evLand = false, evBail = false, evSplash = false, evGrindStart = false;

private:
    void ride(float dt, const Input& in, World& w);
    void air(float dt, const Input& in, World& w);
    void grind(float dt, const Input& in, World& w);
    void bailUpdate(float dt, World& w);
    void ollie(float strength);
    void land(World& w, float g);
    void addTrick(const std::string& n, float pts);
    void bankCombo();
};
