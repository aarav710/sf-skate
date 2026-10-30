#include "instance.h"
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include <functional>

namespace inst {

// One queued shape: the rows of its 3x4 world matrix (unit mesh -> world) and its color (alpha = material code).
struct Inst { float r[12]; unsigned char c[4]; };

// A unit mesh and the instances queued for it this pass. The key says which raylib call it stands for.
struct Kind {
    // 0 cube, 1 sphere (a = rings, b = slices), 2 cylinder (a = sides, k = end / start radius),
    // 3 cylinder along local z at height y with radius k (a = sides), see zcylinder
    int type = 0;
    int a = 0, b = 0;
    float k = 0, y = 0;
    unsigned vao = 0, vbo = 0, ibo = 0;
    int verts = 0;
    size_t cap = 0;          // instance buffer size
    std::vector<Inst> q;
};
static std::vector<Kind> kinds;

// the transform stack: row-major 3x4 (rotation / scale in columns 0-2, translation in column 3)
struct Xf { float m[12]; };
static Xf stack[32] = {{{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0}}};
static int top = 0;

static rlRenderBatch* frameBatch = nullptr;   // the batch the game draws with (nullptr = raylib's default)
void setFrameBatch(rlRenderBatch* b) { frameBatch = b; }

// Run a raylib draw call into a private render batch and read back the triangles it made, in its own local space.
static std::vector<float> capture(const std::function<void()>& draw) {
    rlDrawRenderBatchActive();
    rlRenderBatch cap = rlLoadRenderBatch(1, 4096);
    rlSetRenderBatchActive(&cap);
    rlMatrixMode(RL_MODELVIEW);
    rlPushMatrix();
    rlLoadIdentity();
    draw();
    rlPopMatrix();
    std::vector<float> out;
    const float* v = cap.vertexBuffer[0].vertices;
    int base = 0;
    for (int d = 0; d < cap.drawCounter; d++) {
        rlDrawCall& dc = cap.draws[d];
        auto put = [&](int i) { out.insert(out.end(), v + (base + i) * 3, v + (base + i) * 3 + 3); };
        if (dc.mode == RL_TRIANGLES)
            for (int i = 0; i < dc.vertexCount; i++) put(i);
        else if (dc.mode == RL_QUADS)   // drawn as two triangles per quad, the way rlgl indexes them
            for (int i = 0; i + 3 < dc.vertexCount; i += 4)
                for (int j : {0, 1, 2, 0, 2, 3}) put(i + j);
        base += dc.vertexCount + dc.vertexAlignment;
        dc.vertexCount = 0;   // nothing to draw when the batch is flushed below
    }
    rlSetRenderBatchActive(frameBatch);   // meshes can be built on first use mid-frame: go back to the game's batch
    rlUnloadRenderBatch(cap);
    return out;
}

// cylinders are told apart by how much they taper (end radius / start radius), to 4 decimals
static float taper(float r0, float r1) { return roundf(r1 / r0 * 1e4f) * 1e-4f; }

static Kind& kind(int type, int a, int b, float k, float y = 0) {
    for (auto& K : kinds)
        if (K.type == type && K.a == a && K.b == b && K.k == k && K.y == y) return K;
    std::vector<float> v = capture([&] {
        if (type == 0) DrawCube({0, 0, 0}, 1, 1, 1, WHITE);
        else if (type == 1) DrawSphereEx({0, 0, 0}, 1, a, b, WHITE);
        else if (type == 2) DrawCylinderEx({0, 0, 0}, {0, 0, 1}, 1, k, a, WHITE);
        else DrawCylinderEx({0, y, -1}, {0, y, 1}, k, k, a, WHITE);
    });
    Kind K;
    K.type = type; K.a = a; K.b = b; K.k = k; K.y = y;
    K.verts = (int)v.size() / 3;
    glGenVertexArrays(1, &K.vao);
    glBindVertexArray(K.vao);
    glGenBuffers(1, &K.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, K.vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);   // vertexPosition
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glGenBuffers(1, &K.ibo);
    glBindBuffer(GL_ARRAY_BUFFER, K.ibo);
    for (int i = 0; i < 3; i++) {   // instRow0..2 at locations 11..13, instCol at 14 (see PACKED_POS in render.cpp)
        glEnableVertexAttribArray(11 + i);
        glVertexAttribPointer(11 + i, 4, GL_FLOAT, GL_FALSE, sizeof(Inst), (void*)(size_t)(i * 16));
        glVertexAttribDivisor(11 + i, 1);
    }
    glEnableVertexAttribArray(14);
    glVertexAttribPointer(14, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(Inst), (void*)offsetof(Inst, c));
    glVertexAttribDivisor(14, 1);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    kinds.push_back(std::move(K));
    return kinds.back();
}

void init() {
    kinds.reserve(32);
    // every shape the movers use, built up front (anything else is built on first use)
    kind(0, 0, 0, 0);
    for (auto rs : {std::pair{5, 8}, {4, 8}, {4, 6}, {5, 6}}) kind(1, rs.first, rs.second, 0);
    kind(3, 6, 0, 0.08f, 1.43f);                // shoulders
    kind(2, 5, 0, taper(1, 0.85f));             // pedal bus riders' limbs
    kind(2, 4, 0, taper(0.035f, 0.02f));        // dog's tail
    kind(2, 4, 0, taper(0.022f, 0.016f));       // cat's tail
}

void push() { stack[top + 1] = stack[top]; top++; }
void pop() { top--; }

void translate(float x, float y, float z) {
    float* m = stack[top].m;
    for (int i = 0; i < 3; i++) m[i * 4 + 3] += m[i * 4] * x + m[i * 4 + 1] * y + m[i * 4 + 2] * z;
}

// same rotation as rlRotatef: angle in degrees about the axis (x, y, z), applied in the current local frame
void rotate(float deg, float x, float y, float z) {
    float l2 = x * x + y * y + z * z;
    if (l2 != 1.f && l2 != 0.f) { float il = 1.f / sqrtf(l2); x *= il; y *= il; z *= il; }
    float s = sinf(DEG2RAD * deg), c = cosf(DEG2RAD * deg), t = 1.f - c;
    const float R[9] = {x * x * t + c, x * y * t - z * s, x * z * t + y * s,
                        y * x * t + z * s, y * y * t + c, y * z * t - x * s,
                        z * x * t - y * s, z * y * t + x * s, z * z * t + c};
    float* m = stack[top].m;
    for (int i = 0; i < 3; i++) {
        float a = m[i * 4], b = m[i * 4 + 1], d = m[i * 4 + 2];
        for (int j = 0; j < 3; j++) m[i * 4 + j] = a * R[j] + b * R[3 + j] + d * R[6 + j];
    }
}

void scale(float x, float y, float z) {
    float* m = stack[top].m;
    for (int i = 0; i < 3; i++) { m[i * 4] *= x; m[i * 4 + 1] *= y; m[i * 4 + 2] *= z; }
}

// queue the unit mesh of K mapped by (current transform) * (L | o): L's columns are the mesh's x, y and z axes
static void put(Kind& K, const float L[9], Vector3 o, Color k) {
    const float* m = stack[top].m;
    Inst in;
    for (int i = 0; i < 3; i++) {
        const float* row = m + i * 4;
        for (int j = 0; j < 3; j++) in.r[i * 4 + j] = row[0] * L[j] + row[1] * L[3 + j] + row[2] * L[6 + j];
        in.r[i * 4 + 3] = row[0] * o.x + row[1] * o.y + row[2] * o.z + row[3];
    }
    in.c[0] = k.r; in.c[1] = k.g; in.c[2] = k.b; in.c[3] = k.a;
    K.q.push_back(in);
}

void cube(Vector3 c, float w, float h, float l, Color k) {
    const float L[9] = {w, 0, 0, 0, h, 0, 0, 0, l};
    put(kinds[0], L, c, k);
}

void sphere(Vector3 c, float r, int rings, int slices, Color k) {
    const float L[9] = {r, 0, 0, 0, r, 0, 0, 0, r};
    put(kind(1, rings, slices, 0), L, c, k);
}

// DrawCylinderEx builds its rings on a basis b1, b2 around the axis; the unit mesh was made along +z with radius 1
// (and r1 / r0 at the far end), so map its basis onto this call's basis, scaled by r0, and +z onto the axis.
void cylinder(Vector3 a, Vector3 b, float r0, float r1, int sides, Color k) {
    Vector3 d = Vector3Subtract(b, a);
    if (d.x == 0 && d.y == 0 && d.z == 0) return;
    Kind& K = kind(2, sides, 0, taper(r0, r1));
    Vector3 ez = {0, 0, 1}, p = Vector3Normalize(Vector3Perpendicular(d));
    const Vector3 src[3] = {Vector3Normalize(Vector3Perpendicular(ez)), Vector3Normalize(Vector3CrossProduct(Vector3Normalize(Vector3Perpendicular(ez)), ez)), ez};
    const Vector3 dst[3] = {Vector3Scale(p, r0), Vector3Scale(Vector3Normalize(Vector3CrossProduct(p, d)), r0), d};
    // L = [dst] * [src]^T, as the unit basis is orthonormal
    auto at = [](Vector3 v, int i) { return i == 0 ? v.x : i == 1 ? v.y : v.z; };
    float L[9];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) {
            float s = 0;
            for (int n = 0; n < 3; n++) s += at(dst[n], i) * at(src[n], j);
            L[i * 3 + j] = s;
        }
    put(K, L, a, k);
}

// The rings of a cylinder along z sit at the same x and y whatever its length, so with the mesh made for this y and
// radius, stretching it along z gives exactly the vertices raylib would (the general mapping above can be off by
// a rounding step, enough to move the shadow speckle on a surface that faces away from the sun).
void zcylinder(float y, float h, float r, int sides, Color k) {
    const float L[9] = {1, 0, 0, 0, 1, 0, 0, 0, h};
    put(kind(3, sides, 0, r, y), L, {0, 0, 0}, k);
}

void flush(const Shader& s) {
    bool any = false;
    for (auto& K : kinds) any |= !K.q.empty();
    if (!any) return;
    rlDrawRenderBatchActive();   // whatever immediate-mode drawing came first goes first
    static unsigned lastId = 0;
    static int locOn = -1;
    if (s.id != lastId) { lastId = s.id; locOn = glGetUniformLocation(s.id, "instOn"); }
    Matrix mvp = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    glUseProgram(s.id);
    glUniformMatrix4fv(s.locs[SHADER_LOC_MATRIX_MVP], 1, GL_FALSE, MatrixToFloatV(mvp).v);
    glUniform1i(locOn, 1);
    for (auto& K : kinds) {
        if (K.q.empty()) continue;
        size_t bytes = K.q.size() * sizeof(Inst);
        glBindBuffer(GL_ARRAY_BUFFER, K.ibo);
        if (bytes > K.cap) K.cap = bytes * 2;
        glBufferData(GL_ARRAY_BUFFER, K.cap, nullptr, GL_STREAM_DRAW);   // orphan: the last pass may still be reading it
        glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, K.q.data());
        glBindVertexArray(K.vao);
        glDrawArraysInstanced(GL_TRIANGLES, 0, K.verts, (int)K.q.size());
        K.q.clear();
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glUniform1i(locOn, 0);
}

}   // namespace inst
