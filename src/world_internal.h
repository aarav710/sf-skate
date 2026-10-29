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

void groundPad(World& w, float x0, float z0, float x1, float z1, float off, Color c);
void roadStrip(World& w, Vector2 a, Vector2 b, float halfL, float halfR, float ext, float off, Color c, float step = 5.f);
extern const Color PASTEL[12];
