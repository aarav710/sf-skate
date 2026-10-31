// Helpers shared between the world_*.cpp files.
#pragma once
#include "world.h"

float analyticHeight(float x, float z);
void initGeography();

// Market Street runs diagonally from the Ferry Building toward Twin Peaks.
constexpr float MK_AX = 1265, MK_AZ = -200, MK_BX = -120, MK_BZ = 840, MK_HALF = 11;

enum BuildStyle { BS_TOWERS, BS_MIDRISE, BS_ROWHOUSE, BS_SUNSET, BS_MISSION, BS_WHARF, BS_OAK_DOWNTOWN, BS_OAK, BS_LADY = 50 };
int buildStyleAt(float x, float z);

// Ground palette
extern const Color C_ASPHALT, C_SIDEWALK, C_PLAZA, C_GRASS, C_SAND, C_YARD, C_LOT;

// Alcatraz: a flat-topped rock with the prison on top, and a road that winds up from the dock on the east end,
// west along the south shore, round the west tip and back east along the north side to the top. Points are
// (x, road height, z). The road sits on a level shelf cut into the slope (alcatrazRoad gives the distance to its
// centerline and the road height there).
constexpr float AZ_X = 300, AZ_Z = -1900, AZ_TOP = 33;
inline const Vector3 AZ_ROAD[] = {{418, 3.4f, -1884}, {392, 4.6f, -1860}, {300, 9.f, -1852},   {230, 12.f, -1864}, {205, 15.f, -1900},
                                  {235, 18.5f, -1930}, {280, 22.5f, -1940}, {330, 27.5f, -1936}, {362, AZ_TOP, -1914}};
constexpr int AZ_ROAD_N = sizeof(AZ_ROAD) / sizeof(AZ_ROAD[0]);
float alcatrazRoad(float x, float z, float* roadY);
inline float alcatrazD(float x, float z) { return sqrtf(sq((x - AZ_X) / 118.f) + sq((z - AZ_Z) / 60.f)); }   // 0 center, ~1 foot of the cliffs

// The ground mesh (and so terrain()) at (x, z): squares of size S from (ox, oz), each split along its (0,0)-(1,1)
// diagonal. False where terrain() reads the raw heightfield instead.
bool groundMeshGrid(float x, float z, float* ox, float* oz, float* S);
void groundPad(World& w, float x0, float z0, float x1, float z1, float off, Color c);
void roadStrip(World& w, Vector2 a, Vector2 b, float halfL, float halfR, float ext, float off, Color c, float step = 5.f);
extern const Color PASTEL[12];
