// The San Francisco world: terrain, streets, buildings, landmarks, bridges and collision.
#pragma once
#include "common.h"
#include "mesh.h"
#include <unordered_map>
#include <functional>

// Terrain extent (includes background hills) and the area the skater can reach.
constexpr float WX0 = -2900, WX1 = 6500, WZ0 = -3900, WZ1 = 3400;
constexpr float PX0 = -1530, PX1 = 4690, PZ0 = -3150, PZ1 = 1840;
// SFO airport sits south-east of the city, outside the main box
constexpr float AIRPORT_Y = 6.0f; // SFO sits on a raised plateau above the bay
constexpr float AX0 = 1300, AX1 = 2720, AZ0 = 1550, AZ1 = 2540;
// Menlo Park, at the south end of the Caltrain line down the Peninsula
constexpr float MX0 = 1350, MX1 = 3700, MZ0 = 8750, MZ1 = 10450;
constexpr float STEP_UP = 0.5f;   // tallest ledge you can roll onto without jumping
constexpr float ROAD_HALF = 7.0f;
constexpr float BLOCK_INSET = 11.0f; // road half width + sidewalk

// North is -Z, east is +X.
// SF street grid lines: x = -1375 + 110*i, z = -1095 + 110*j
inline float gx(int i) { return -1375.f + 110.f * i; }
inline float gz(int j) { return -1095.f + 110.f * j; }

enum PlatKind : uint8_t { PK_BUILDING, PK_DECK, PK_RAILING, PK_PROP, PK_POLE, PK_CAR, PK_PIER };

// An axis-aligned solid whose top may slope along one axis. Used for buildings,
// bridge decks, ramps, ledges, poles... Low ones are ground, tall ones are walls.
struct Platform {
    float x0, x1, z0, z1;
    float h0, h1;   // top height at the low/high end of `axis`
    float base;     // bottom
    uint8_t axis;   // 0 = slope along x, 1 = along z
    uint8_t kind;
    float top(float x, float z) const {
        float t = axis == 0 ? (x - x0) / (x1 - x0) : (z - z0) / (z1 - z0);
        return lerpf(h0, h1, clampf(t, 0.f, 1.f));
    }
    bool contains(float x, float z, float pad = 0) const {
        return x >= x0 - pad && x <= x1 + pad && z >= z0 - pad && z <= z1 + pad;
    }
};

struct Rail { Vector3 a, b; const char* name; };

// An aerial gondola: stations on summits joined by cables on lattice towers. path is the sagging centerline
// (stations at stationS along it), and the cabins ride a loop up one cable and back down the other.
struct GondolaLine {
    std::vector<Vector3> path;
    std::vector<float> cum;          // distance along path at each point
    std::vector<float> stationS;     // distance along path of each station
    float len = 0;
    Color accent{};
};

struct Grid {
    float x0 = 0, z0 = 0, sp = 110;
    int ni = 0, nj = 0;
    float roadHalf = 7.0f, walkOff = 9.0f; // street half-width, sidewalk offset
    std::vector<uint8_t> ok, ex, ez; // node valid, edge to +x, edge to +z
    Vector2 node(int i, int j) const { return {x0 + i * sp, z0 + j * sp}; }
    bool in(int i, int j) const { return i >= 0 && j >= 0 && i < ni && j < nj; }
    bool nodeOk(int i, int j) const { return in(i, j) && ok[j * ni + i]; }
    // d: 0 = +x, 1 = -x, 2 = +z, 3 = -z
    bool edge(int i, int j, int d) const;
    void setEdge(int i, int j, int d, bool v);
};
static const int DI[4] = {1, -1, 0, 0};
static const int DJ[4] = {0, 0, 1, -1};

struct Path {
    std::vector<Vector2> p;
    std::vector<float> cum;
    float len = 0;
    bool onDeck = false;
    void finish();
    Vector2 at(float s, Vector2* dir = nullptr) const;
};

struct Token { Vector3 pos; const char* name; bool got = false; };
// A traffic signal head over (or beside) the lane it controls: lamps face `face` (toward oncoming traffic).
struct SignalHead { Vector3 pos; Vector2 face; int node; uint8_t axis; };
// Every signalized intersection runs the same 26 s cycle (offset per intersection): traffic along z gets green,
// then yellow, then a moment of all-red, then traffic along x. 0 green, 1 yellow, 2 red.
inline int signalState(int node, int axis, float t) {
    float c = fmodf(t + hash01(node, 3, 11) * 26.f, 26.f);
    if (axis == 1) return c < 10.f ? 0 : c < 13.f ? 1 : 2;
    return c < 13.5f ? 2 : c < 23.5f ? 0 : c < 26.f - 0.5f ? 1 : 2;
}
struct Spot { const char* name; Vector3 pos; float yaw; };
struct Seat { Vector3 pos; float yaw; bool grass; };   // where someone can sit: the seat surface (or lawn) and the way they face
struct MapLabel { const char* name; float x, z; int size; };
struct Pond { float x0, z0, x1, z1; const char* kind = "pond"; };   // kind names it in the splash message

// Static geometry is split into layers per 400 m cell: the main layer is always drawn; foliage detail (every leaf)
// only up close, swapped for low-poly canopy blobs further out; house interiors only near the camera.
enum : int { LAYER_MAIN, LAYER_DETAIL, LAYER_FAR, LAYER_INTERIOR };
constexpr int LAYER_STRIDE = 10000000;
constexpr float DETAIL_DIST = 650.f, INTERIOR_DIST = 110.f;
// house rooms are meshed one house at a time once the camera comes within INTERIOR_LOAD, and freed past INTERIOR_DROP
constexpr float INTERIOR_LOAD = 200.f, INTERIOR_DROP = 225.f;
// count is the index count; packed chunks store 1/1024 m steps from org (w = 1), see the city shader
constexpr int PACKED_LOC = 10;
struct Chunk {
    unsigned vao = 0, vaoFar = 0, vboA = 0, vboB = 0, ebo = 0;
    int count = 0;
    bool packed = false, idx32 = false;
    Vector4 org{0, 0, 0, 0};
    Vector3 mn{}, mx{};
    int layer = 0;
    Vector2 c0{}, c1{};
};

// A house you can walk into: its ground floor is a real room behind an open door.
struct House {
    float x0, z0, x1, z1;      // outer footprint
    float floorY;              // interior floor height (the entry level)
    int facing;                // which side the street is on: 0 = -z, 1 = +z, 2 = -x, 3 = +x
    Vector2 doorOut, doorIn;   // points just outside the front steps and just inside the door
    // the way in from the street, for anyone on foot: waypoints ending just inside the door. A front stair that
    // runs along the facade (steep streets) adds waypoints at its foot and along it.
    bool sideStair = false;
    Vector2 route[4]{};
    int nRoute = 0;
};

// What it takes to build a house's room again: the house, its shell heights, the door and its generator then.
struct InteriorSrc { House h; float base, top, doorU; RNG hr; };
struct InteriorStats { double ms = 0; int built = 0, cached = 0, missingNear = 0; };   // missingNear: within 30 m, not yet built

struct World {
    // ---- generation ----
    void generate(void (*progress)(const char*, float));
    void upload(const std::function<void()>& meanwhile = {});   // runs meanwhile() on this thread during the bake
    // slim: position and color only (the far pass)
    void drawPass(const Camera3D& cam, float nearD, float farD, const Shader& sh, bool slim);
    void drawWater(const Shader& sh, bool slim);
    // shadow maps: draw every chunk inside the light's box (lightView space: |x|,|y| < half, -zFar < z < 0).
    // far: for the wide far cascade, with the low-poly canopies in place of every leaf
    int drawCasters(Matrix lightView, float half, float zFar, const Shader& sh, bool far = false);
    // build the rooms near the camera (nearest first, for about budget seconds) and free the far ones
    InteriorStats updateInteriors(Vector3 cam, double budget);
    void freeInteriors();

    // ---- queries ----
    float terrain(float x, float z) const;                    // bare ground height, exactly as the ground mesh draws it
    float terrainRaw(float x, float z) const;                 // the 4 m heightfield itself (what the mesh is built from)
    float ground(float x, float z, float yRef) const;         // highest walkable surface <= yRef+STEP_UP
    bool collide(Vector3& p, float r, float feetY, float height, Vector2* nrm, float* size = nullptr) const; // push out of walls; size = widest box hit
    // collide() along the move from `from` to p in short steps, so a fast mover can't skip through a thin wall or pole;
    // after a hit the rest of the move slides along the wall. p ends at the resolved position
    bool collideSwept(Vector3 from, Vector3& p, float r, float feetY, float height, Vector2* nrm, float* size = nullptr) const;
    bool solidAt(Vector3 p, bool bigProps = false, bool piers = false) const;   // buildings/decks (+ big prop boxes: the camera; + piers: boats)
    float ceiling(float x, float z, float feetY) const;        // lowest underside above the feet (1e9 if open sky)
    int houseAt(float x, float z, float y) const;              // index into houses if inside one, else -1
    bool doorBlocked(float x, float z, float pad) const;       // is (x,z) on the way from the sidewalk to a front door?
    // how far to step a sidewalk walker at (x,z), out toward the street, to keep them clear of the front steps
    // of a house (routeDir: the way their route runs, or zero); zero when no stoop is near
    Vector2 stoopDetour(float x, float z, Vector2 routeDir) const;
    bool buildingAt(float x, float z, float pad, float y0, float y1) const; // does a building box overlap x/z +-pad between heights y0..y1?
    bool isWater(float x, float z, float y) const;
    const char* waterKind(float x, float z, float y) const;   // what isWater found there: "bay", "ocean", or the pond's kind
    const Pond* pondAt(float x, float z, float y) const;      // the pond (lake, pool, ...) whose water is at this spot, if any
    bool onBridge(float x, float z) const;
    bool inPlay(float x, float z) const {
        return (x >= PX0 && x <= PX1 && z >= PZ0 && z <= PZ1) || (x >= AX0 && x <= AX1 && z >= AZ0 && z <= AZ1) ||
               (x >= MX0 && x <= MX1 && z >= MZ0 && z <= MZ1);
    }
    const char* district(float x, float z) const;
    bool overPier(float x, float z) const;                  // is (x,z) on a pier deck (Embarcadero, Pier 39, Hyde St, Alcatraz)?
    int zoneFlags(float x, float z, bool strict) const;

    // ---- data ----
    std::vector<float> hf; int hfW = 0, hfH = 0;
    std::vector<float> hf2; int hf2W = 0, hf2H = 0;  // detailed terrain around Menlo Park
    // trees counted per 25 m cell (for the rustle of leaves and the birdsong around you)
    std::unordered_map<int, int> treeCells;
    float treesNear(float x, float z) const;   // trees within about 35 m, weighted toward the closest
    // Caltrain: route polyline, stations (distance along the line), track geometry helpers
    struct Station { const char* name; float s; };
    std::vector<Station> stations;
    float railLen() const;
    float railGrade(float s) const;                                 // top of the ballast
    Vector3 trackPoint(float s, float lateral, Vector2* dir) const; // lateral > 0 = west side going south
    float railDistance(float x, float z, float* s) const;
    Vector3 caltrainDropoff(int station, float* yaw) const;        // a clear spot on the platform to step off the train
    Image mapImg2{};                 // Peninsula / Menlo Park map
    std::vector<House> houses;
    int rowhouseCount = 0, houseTooSteep = 0, houseTooHigh = 0;   // generation stats: street fronts built, and why some stay closed
    std::unordered_multimap<int, Vector4> doorWays; // coarse grid of door approach segments (ax, az, bx, bz)
    static constexpr float STOOP_REACH = 3.f;   // how far round a stoop stoopDetour can move a walker (clearance + ease)
    struct Stoop { float x0, x1, z0, z1; Vector2 out; };   // a house's front-step collision box, and the way to the street
    std::unordered_multimap<int, Stoop> stoops;     // same coarse grid, in every cell within STOOP_REACH of the box
    std::vector<Platform> plats;
    std::vector<std::vector<int>> pgrid; int pgW = 0, pgH = 0;
    std::vector<Rail> rails;
    std::vector<Pond> ponds;
    Grid sf, oak, mp;
    std::vector<Path> carPaths;      // bridges, Market St
    std::vector<int> carPathLanes;   // lanes per direction for each car path
    std::vector<int> carPathKind;    // 0 = bridge deck, 1 = Market St, 2 = highway
    std::vector<Path> tramPaths;     // Muni Metro streetcar lines
    std::vector<const char*> tramNames;
    std::vector<Path> cablePaths;    // cable car lines
    std::vector<Path> pedPaths;      // bridge walkways, promenades
    std::vector<Token> tokens;
    std::vector<GondolaLine> gondolas;
    std::vector<Spot> spots;
    std::vector<MapLabel> labels;
    std::vector<Vector3> tourists;   // idle people at viewpoints
    std::vector<Seat> seats;         // benches and park lawns where people sit (often with a laptop)
    std::vector<uint8_t> signals;    // per SF grid node (j * ni + i): 1 where the intersection has traffic lights
    std::vector<SignalHead> signalHeads;
    std::vector<Vector3> steamVents; // manholes letting off steam
    std::vector<Vector3> lamps;      // street lamp heads (they light the street at night)
    Image mapImg{};
    std::vector<Chunk> chunks;
    Chunk water{};
    std::vector<InteriorSrc> interiorSrc;
    std::unordered_map<int, Chunk> interiorMesh;   // index into interiorSrc -> its room

    // generation scratch
    std::unordered_map<int, MeshBuilder> builders;
    MeshBuilder waterB;
    int layer = LAYER_MAIN;          // which layer mb() builds into
    MeshBuilder& mb(float x, float z);
    MeshBuilder& mbKey(int key) { return builders[key]; }

private:
    void buildHeightfield();
    void buildGrid(Grid& g);
    void buildTerrainMesh();
    void buildBackdrop();
    void buildRoads(const Grid& g, int style); // 0 = city, 1 = suburban (no lines)
    void buildMarket();
    void buildLombard();
    void buildBlocks(const Grid& g, bool oakland);
    void buildStreetFurniture(const Grid& g, bool oakland);
    void buildGoldenGate();
    void buildBayBridge();
    void buildLandmarks();
    void buildWaterfront();
    void buildParks();
    void buildAlcatraz();
    void buildSkateSpots();
    void buildMap();
    void buildMapImage(Image& img, float X0, float Z0, float X1, float Z1, float RES);
    void buildCaltrain();
    void buildMenlo();
    void addEstate(float x0, float z0, float x1, float z1, int facing, RNG& r);
    void buildTramLines();
    void buildAirport();
    void buildGondolas();
    void buildMeta();

    int addPlat(const Platform& p);
    void addBox(float x0, float z0, float x1, float z1, float base, float top, uint8_t kind,
                Color side, Color topC, float ao = 0.8f);
    void addBridgePier(MeshBuilder& m, float x0, float z0, float x1, float z1, float lo, float hi, Color c);
    void addBuilding(float x0, float z0, float x1, float z1, float height, Color side, Color roof, int style, int facing, RNG& r);
    void addTower(float x0, float z0, float x1, float z1, float H, RNG& r);
    // geo null: collision only (the city build); otherwise the mesh only, into geo
    void addHouseInterior(const House& h, float base, float top, float doorU, RNG& hr, MeshBuilder* geo);
    void addTree(float x, float z, int type, float scale, RNG& r, bool solid = true);
    void addShrub(float x, float z, float scale, RNG& r, bool flowering = false);
    void addFlowerBed(float x0, float z0, float x1, float z1, int count, RNG& r);
    void addLamp(float x, float z, float yaw);
    // curbside things: 0 hydrant, 1 trash can, 2 newspaper boxes, 3 bike rack with bikes, 4 parked scooters, 5 bus shelter.
    // p is on the sidewalk, f runs along the street, o points out toward the road.
    void addClutter(Vector2 p, Vector2 f, Vector2 o, int kind, uint32_t h);
    void buildIntersections(const Grid& g);
    void addRamp(float x0, float z0, float x1, float z1, float rise, int axis, bool up, bool coping = true);
    void addLedge(float x0, float z0, float x1, float z1, float h, Color c);
    void addRailObj(Vector3 a, Vector3 b, const char* name = nullptr);
    void addStairs(float x0, float z0, float x1, float z1, float drop, int steps, int dir, bool handrails);
    void deckSegments(int axis, float center, float halfW, const std::vector<Vector2>& prof,
                      Color sideCol, Color railCol, const char* railName, int mbKeyBase);
    float deckHeightAt(const std::vector<Vector2>& prof, float s) const;
};

// Zone flags
enum : int { Z_NOBUILD = 1, Z_NOROAD = 2, Z_GRASS = 4, Z_PLAZA = 8, Z_TREES = 16, Z_SCRUB = 32 };

bool marketDist(float x, float z, float* d);
