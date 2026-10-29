// Box-based vehicle models shared by parked cars (static mesh) and moving traffic.
#pragma once
#include "common.h"
#include "mesh.h"

// slot: 0 = fixed color, 1 = body color, 2 = darker body color
struct LBox { Vector3 c, s; Color k; uint8_t slot; };

enum CarType { CT_SEDAN, CT_SUV, CT_VAN, CT_TAXI, CT_BUS, CT_CABLECAR, CT_STREETCAR, CT_LRV, CT_PLANE, CT_AIRTRAIN, CT_CALTRAIN, CT_CALTRAIN_CAB, CT_PEDALBUS, CT_COUNT };

struct CarModel {
    std::vector<LBox> boxes;
    float halfLen, halfWid, height;
};

const CarModel& carModel(int type);
Color randomCarColor(RNG& r);

inline Color boxColor(const LBox& b, Color body) {
    if (b.slot == 1) return withMat(body, b.k.a);
    if (b.slot == 2) return withMat(scaleC(body, 0.75f), b.k.a);
    return b.k;
}

// Add a model to the static mesh (parked cars).
void addCarToMesh(MeshBuilder& mb, int type, Color body, Vector3 pos, float yaw, float pitch);
// Draw a model with immediate-mode cubes (moving cars). Must be inside BeginMode3D.
void drawCarModel(int type, Color body, Vector3 pos, float yaw, float pitch, bool brake);
