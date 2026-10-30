// Instanced drawing for the small moving models (people, pets, cars, cops). Same calls as raylib's DrawCube,
// DrawSphereEx, DrawCylinderEx and rlPushMatrix / rlTranslatef / rlRotatef / rlScalef, but instead of emitting every
// vertex each frame, each shape is queued as one instance (a 3x4 world matrix and a color) of a mesh built once at
// startup from raylib's own output, so the shapes, colors and material codes come out the same. flush() then draws
// everything queued in one instanced call per mesh, with the shader of the pass (city, cityFar or depth).
#pragma once
#include "common.h"

namespace inst {
void init();                       // after the window is open: builds the meshes
// the render batch to make active again after building a mesh (nullptr = raylib's default); see main.cpp
void setFrameBatch(rlRenderBatch* b);

// a transform stack of its own, starting at identity (like rlPushMatrix ... rlPopMatrix outside any other push)
void push();
void pop();
void translate(float x, float y, float z);
void rotate(float deg, float x, float y, float z);
void scale(float x, float y, float z);

void cube(Vector3 c, float w, float h, float l, Color k);
void sphere(Vector3 c, float r, int rings, int slices, Color k);
void cylinder(Vector3 a, Vector3 b, float r0, float r1, int sides, Color k);
// DrawCylinderEx({0, y, -h}, {0, y, h}, r, r, sides, k), with one mesh per y and r (used for the shoulders)
void zcylinder(float y, float h, float r, int sides, Color k);

// draw all queued shapes with this shader (the one active for the pass) and the current view / projection
void flush(const Shader& s);
}
