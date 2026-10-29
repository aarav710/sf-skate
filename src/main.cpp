// SF Skate - skate through a stylized San Francisco.
#include "common.h"
#include "world.h"
#include "player.h"
#include "traffic.h"
#include "audio.h"
#include "models.h"
#include "chase.h"
#include "jetski.h"
#include "gondola.h"
#include "seaplane.h"
#include <cstdio>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>

// ---------------------------------------------------------------- shader
static const char* VS = R"(#version 330
in vec3 vertexPosition;
in vec4 vertexColor;
uniform mat4 mvp;
out vec3 wPos;
out vec4 vCol;
void main() {
    wPos = vertexPosition;
    vCol = vertexColor;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
})";

static const char* FS = R"(#version 330
in vec3 wPos;
in vec4 vCol;
uniform vec3 camPos;
uniform vec3 sunDir;
uniform vec3 sunCol;
uniform vec3 ambCol;
uniform vec3 fogCol;
uniform vec3 skyCol;
uniform float fogDen;
uniform float night;
uniform float time;
out vec4 finalColor;

float h21(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = h21(i), b = h21(i + vec2(1, 0)), c = h21(i + vec2(0, 1)), d = h21(i + vec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    vec3 N = normalize(cross(dFdx(wPos), dFdy(wPos)));
    vec3 toCam = camPos - wPos;
    float dist = length(toCam);
    vec3 V = toCam / max(dist, 1e-3);
    if (dot(N, V) < 0.0) N = -N;
    int m = int(floor((1.0 - vCol.a) * 17.0 + 0.5));
    vec3 base = vCol.rgb;
    vec3 emis = vec3(0.0);
    float specK = 0.0, specP = 32.0;
    float near = clamp(1.0 - dist / 260.0, 0.0, 1.0);

    if (m == 5) {                       // ground: subtle noise
        float n = vnoise(wPos.xz * 0.8) * 0.5 + vnoise(wPos.xz * 0.11) * 0.5;
        base *= mix(1.0, 0.84 + 0.32 * n, 0.35 + 0.65 * near);
    } else if (m == 1 || m == 2 || m == 6 || (m >= 7 && m <= 9)) {   // facades with windows
        if (abs(N.y) < 0.45) {
            bool house = (m == 2 || m >= 7);            // 7..9 are house materials with shifted floor lines
            float u = abs(N.x) > abs(N.z) ? wPos.z : wPos.x;
            float y = wPos.y - (m >= 7 ? 0.825 * float(m - 6) : 0.0);
            float fh = house ? 3.3 : 3.8;
            float fw = house ? 2.6 : (m == 6 ? 1.6 : 3.0);
            vec2 cell = vec2(floor(u / fw), floor(y / fh));
            vec2 f = vec2(fract(u / fw), fract(y / fh));
            // how big one window cell is on screen: fade the pattern out before it turns into speckle
            float px = max(fwidth(u / fw), fwidth(y / fh));
            float detail = house ? clamp(1.6 - px * 7.0, 0.0, 1.0) : 1.0;
            vec3 plain = base;
            bool win;
            bool trimmed = false;
            if (house) {                                // tall sash windows, white surround, hood, sill, meeting rail
                win = f.x > 0.31 && f.x < 0.69 && f.y > 0.27 && f.y < 0.83;
                bool rail = win && abs(f.y - 0.55) < 0.014;
                trimmed = rail || (f.x > 0.25 && f.x < 0.75 && f.y > 0.2 && f.y < 0.89 && !win) || f.y < 0.035;
                if (rail) win = false;
            } else if (m == 1) win = f.x > 0.1 && f.x < 0.9 && f.y > 0.3 && f.y < 0.92;
            else win = f.x > 0.05 && f.y > 0.07;
            if (win) {
                float fres = pow(1.0 - abs(dot(N, V)), 2.0);
                vec3 glass = house ? vec3(0.17, 0.2, 0.25) : mix(vec3(0.14, 0.2, 0.27), skyCol, 0.25 + 0.55 * fres);
                if (m == 6) glass = mix(base * 0.6, skyCol, 0.3 + 0.5 * fres);
                base = mix(base, glass, 0.92);
                specK = 0.7; specP = 70.0;
                float lit = h21(cell + floor(wPos.xz / 23.0) * 7.13);
                if (lit > 0.42) emis = vec3(1.0, 0.82, 0.55) * (0.5 + 0.5 * h21(cell * 1.7)) * night * (m == 6 ? 0.8 : 1.0);
            } else if (trimmed) {
                base = mix(base, vec3(0.94, 0.92, 0.86), 0.88);
            } else if (m == 6) {
                base *= 0.9;
            }
            if (house) {
                // average facade tone seen from afar: wall, a share of dark glass and cream trim
                vec3 avg = plain * 0.62 + vec3(0.17, 0.2, 0.25) * 0.21 + vec3(0.94, 0.92, 0.86) * 0.17;
                base = mix(avg, base, detail);
                specK *= detail;
            }
        }
    } else if (m == 3) {                 // water
        vec2 p = wPos.xz;
        float t = time;
        float fade = clamp(1.0 - dist / 1800.0, 0.15, 1.0);
        float a = p.x * 0.11 + t * 1.1, b = p.x * 0.23 + p.y * 0.17 - t * 1.6, c = p.y * 0.13 + t * 0.9, d = p.x * 0.6 - p.y * 0.5 + t * 2.5;
        float dx = 0.35 * 0.11 * cos(a) + 0.18 * 0.23 * cos(b) + 0.08 * 0.6 * cos(d);
        float dz = 0.18 * 0.17 * cos(b) + 0.25 * 0.13 * cos(c) - 0.08 * 0.5 * cos(d);
        N = normalize(vec3(-dx * fade * 3.0, 1.0, -dz * fade * 3.0));
        float fres = 0.04 + 0.96 * pow(1.0 - max(dot(N, V), 0.0), 5.0);
        vec3 deep = base * (ambCol * 1.2 + sunCol * 0.35);
        vec3 refl = mix(fogCol, skyCol, 0.4);
        vec3 col = mix(deep, refl, clamp(fres * 0.9, 0.0, 0.85));
        vec3 H = normalize(sunDir + V);
        col += sunCol * pow(max(dot(N, H), 0.0), 220.0) * 2.2;
        float f = 1.0 - exp(-dist * fogDen);
        finalColor = vec4(mix(col, fogCol, f), 1.0);
        return;
    } else if (m == 4) {                 // lights
        emis = base * night * 1.3;
    }

    float diff = max(dot(N, sunDir), 0.0);
    float hemi = 0.62 + 0.38 * N.y;
    vec3 light = ambCol * hemi + sunCol * diff;
    if (m == 10) {                       // rooms: warm lamp light and soft window light, warmer still at night
        vec3 day = (ambCol * 1.2 + sunCol * 0.2 + vec3(0.28, 0.22, 0.12)) * (0.88 + 0.12 * hemi);
        light = mix(day, vec3(1.0, 0.88, 0.7) * (0.9 + 0.1 * hemi), night);
    }
    vec3 col = base * light;
    if (specK > 0.0) {
        vec3 H = normalize(sunDir + V);
        col += sunCol * specK * pow(max(dot(N, H), 0.0), specP);
    }
    col += emis;
    // aerial perspective: distant hills fade into a blue-grey haze
    float f = 1.0 - exp(-dist * fogDen);
    float sunAmt = pow(max(dot(-V, sunDir), 0.0), 8.0);
    vec3 fc = mix(fogCol, sunCol, sunAmt * 0.3 * (1.0 - night));
    if (length(emis) > 0.05) f *= 0.75;   // lights shine through the fog a bit
    col = mix(col, fc, f);
    finalColor = vec4(col, 1.0);
})";

// ---------------------------------------------------------------- lighting presets
struct Sky {
    Vector3 sunDir, sunCol, amb, top, horizon, fog;
    float night;
};
static Sky PRESETS[3] = {
    // day
    {Vector3Normalize({-0.35f, 0.8f, 0.45f}), {0.84f, 0.8f, 0.72f}, {0.38f, 0.42f, 0.5f},
     {0.32f, 0.55f, 0.88f}, {0.76f, 0.85f, 0.94f}, {0.76f, 0.84f, 0.92f}, 0.0f},
    // golden hour: sun setting over the Pacific
    {Vector3Normalize({-0.9f, 0.16f, 0.25f}), {1.1f, 0.72f, 0.45f}, {0.36f, 0.32f, 0.42f},
     {0.22f, 0.3f, 0.56f}, {0.98f, 0.66f, 0.44f}, {0.88f, 0.66f, 0.55f}, 0.35f},
    // night
    {Vector3Normalize({0.3f, 0.6f, -0.4f}), {0.22f, 0.26f, 0.4f}, {0.1f, 0.11f, 0.17f},
     {0.01f, 0.02f, 0.06f}, {0.07f, 0.09f, 0.16f}, {0.07f, 0.09f, 0.15f}, 1.0f},
};
static const char* PRESET_NAMES[3] = {"Day", "Golden Hour", "Night"};

static Sky lerpSky(const Sky& a, const Sky& b, float t) {
    return {Vector3Normalize(Vector3Lerp(a.sunDir, b.sunDir, t)), Vector3Lerp(a.sunCol, b.sunCol, t), Vector3Lerp(a.amb, b.amb, t),
            Vector3Lerp(a.top, b.top, t), Vector3Lerp(a.horizon, b.horizon, t), Vector3Lerp(a.fog, b.fog, t), lerpf(a.night, b.night, t)};
}
static Color toColor(Vector3 v) { return {(unsigned char)clampf(v.x * 255, 0, 255), (unsigned char)clampf(v.y * 255, 0, 255), (unsigned char)clampf(v.z * 255, 0, 255), 255}; }

// ---------------------------------------------------------------- globals
static World W;
static Player P;
static Traffic T;
static Sfx S;
static Chase CH;
static JetSkis JS;
static Seaplanes SP;
static Gondolas GO;

static const Color C_COP = {255, 70, 60, 255};

// An officer on foot: running, arms out to grab, or flat on the ground after being knocked down.
static void drawCop(const Cop& c, float now, bool grabbing) {
    rlPushMatrix();
    rlTranslatef(c.pos.x, c.pos.y, c.pos.z);
    rlRotatef(-c.yaw * RAD2DEG, 0, 1, 0);
    bool down = c.stun > 0;
    if (down) {
        rlTranslatef(0, 0.15f, 0);
        rlRotatef(-85.f * clampf((3.2f - (c.stun > 3.2f ? 3.2f : c.stun)) / 3.2f * 6.f + 0.f, 0.f, 1.f), 0, 0, 1);
    }
    if (grabbing) rlTranslatef(0, 0, sinf(now * 40.f) * 0.03f);
    Color shirt = {28, 44, 96, M_PLAIN}, pants = {22, 26, 44, M_PLAIN}, skin = {224, 180, 142, M_PLAIN};
    float sw = (down || grabbing) ? 0.f : sinf(c.phase) * 0.7f;
    for (int side = -1; side <= 1; side += 2) {
        rlPushMatrix();
        rlTranslatef(0, 0.9f, side * 0.11f);
        rlRotatef(sw * side * RAD2DEG, 0, 0, 1);
        DrawCube({0, -0.44f, 0}, 0.16f, 0.88f, 0.16f, pants);
        DrawCube({0.05f, -0.86f, 0}, 0.26f, 0.08f, 0.14f, {20, 20, 20, M_PLAIN});
        rlPopMatrix();
        rlPushMatrix();
        rlTranslatef(0, 1.43f, side * 0.27f);
        rlRotatef((grabbing ? -75.f : -sw * side * 0.9f * RAD2DEG), 0, 0, 1);
        DrawCube({0, -0.28f, 0}, 0.12f, 0.58f, 0.12f, shirt);
        DrawCube({0, -0.6f, 0}, 0.11f, 0.11f, 0.11f, skin);
        rlPopMatrix();
    }
    DrawCube({0, 1.18f, 0}, 0.26f, 0.6f, 0.42f, shirt);
    DrawCube({0, 0.92f, 0}, 0.28f, 0.08f, 0.44f, {30, 30, 34, M_PLAIN});        // belt
    DrawCube({0.14f, 1.35f, -0.12f}, 0.04f, 0.12f, 0.1f, {240, 200, 60, M_PLAIN}); // badge
    DrawCube({0, 1.6f, 0}, 0.21f, 0.25f, 0.21f, skin);
    DrawCube({0, 1.76f, 0}, 0.25f, 0.1f, 0.25f, {20, 28, 60, M_PLAIN});          // cap
    DrawCube({0.14f, 1.71f, 0}, 0.1f, 0.03f, 0.24f, {16, 20, 40, M_PLAIN});     // brim
    rlPopMatrix();
}

static void drawCruiser(Vector3 pos, float yaw, float now) {
    bool flip = (int)(now * 8) % 2;
    drawCarModel(CT_SEDAN, {235, 235, 240, 255}, pos, yaw, 0, false);
    DrawCube(Vector3Add(pos, rotY({-0.1f, 1.62f, 0}, yaw)), 0.35f, 0.18f, 1.2f, {25, 25, 30, M_PLAIN});
    Color red = flip ? Color{255, 40, 40, M_EMISSIVE} : Color{110, 10, 10, M_EMISSIVE};
    Color blue = flip ? Color{20, 40, 120, M_EMISSIVE} : Color{60, 110, 255, M_EMISSIVE};
    DrawCube(Vector3Add(pos, rotY({-0.1f, 1.78f, -0.38f}, yaw)), 0.3f, 0.16f, 0.42f, red);
    DrawCube(Vector3Add(pos, rotY({-0.1f, 1.78f, 0.38f}, yaw)), 0.3f, 0.16f, 0.42f, blue);
}

static void drawStar(float cx, float cy, float r, Color c) {
    Vector2 v[10];
    for (int i = 0; i < 10; i++) {
        float a = -PI / 2 + i * PI / 5, rr = i % 2 ? r * 0.42f : r;
        v[i] = {cx + cosf(a) * rr, cy + sinf(a) * rr};
    }
    for (int i = 0; i < 10; i++) {
        DrawTriangle({cx, cy}, v[i], v[(i + 1) % 10], c);
        DrawTriangle({cx, cy}, v[(i + 1) % 10], v[i], c);
    }
}
// two maps: the Bay Area around San Francisco, and the Peninsula around Menlo Park
struct MapView { Texture2D tex; float x0, z0, res; };
static MapView gMaps[2];
static Vector3 gMapFocus; // where the maps are centred (the skater, or the train during a ride)
static const MapView& curMap() { return gMaps[gMapFocus.z > 3400 ? 1 : 0]; }

static void loadingScreen(const char* msg, float frac) {
    BeginDrawing();
    ClearBackground({18, 22, 30, 255});
    int w = GetScreenWidth(), h = GetScreenHeight();
    const char* title = "SF SKATE";
    DrawText(title, w / 2 - MeasureText(title, 80) / 2, h / 2 - 120, 80, {240, 90, 60, 255});
    DrawText(msg, w / 2 - MeasureText(msg, 24) / 2, h / 2 + 10, 24, {220, 220, 230, 255});
    DrawRectangle(w / 2 - 250, h / 2 + 60, 500, 10, {50, 55, 70, 255});
    DrawRectangle(w / 2 - 250, h / 2 + 60, (int)(500 * frac), 10, {240, 90, 60, 255});
    EndDrawing();
}

static void textShadow(const char* t, int x, int y, int size, Color c) {
    DrawText(t, x + 2, y + 2, size, {0, 0, 0, (unsigned char)(c.a * 0.6f)});
    DrawText(t, x, y, size, c);
}
static void textCenter(const char* t, int cx, int y, int size, Color c) { textShadow(t, cx - MeasureText(t, size) / 2, y, size, c); }


static void drawPlayerArrow(Vector2 c, float yaw, float s, Color col) {
    Vector2 f = {cosf(yaw), sinf(yaw)}, r = {-f.y, f.x};
    Vector2 a = {c.x + f.x * s, c.y + f.y * s};
    Vector2 b = {c.x - f.x * s * 0.6f + r.x * s * 0.6f, c.y - f.y * s * 0.6f + r.y * s * 0.6f};
    Vector2 d = {c.x - f.x * s * 0.6f - r.x * s * 0.6f, c.y - f.y * s * 0.6f - r.y * s * 0.6f};
    DrawTriangle(a, d, b, col);
    DrawTriangle(a, b, d, col);
}

static void drawMinimap(int x, int y, int size) {
    float span = 900.f; // meters across
    const MapView& mv = curMap();
    Vector3 c = gMapFocus;
    float srcW = span / mv.res;
    float px = (c.x - mv.x0) / mv.res, pz = (c.z - mv.z0) / mv.res;
    DrawRectangle(x - 4, y - 4, size + 8, size + 8, {0, 0, 0, 140});
    BeginScissorMode(x, y, size, size);
    DrawTexturePro(mv.tex, {px - srcW / 2, pz - srcW / 2, srcW, srcW}, {(float)x, (float)y, (float)size, (float)size}, {0, 0}, 0, WHITE);
    float k = size / span;
    for (auto& t : W.tokens) {
        if (t.got) continue;
        float sx = x + size / 2 + (t.pos.x - c.x) * k, sy = y + size / 2 + (t.pos.z - c.z) * k;
        DrawCircle((int)sx, (int)sy, 5, {255, 200, 40, 255});
        DrawCircleLines((int)sx, (int)sy, 5, {120, 80, 0, 255});
    }
    for (auto& car : T.cars) {
        float sx = x + size / 2 + (car.pos.x - c.x) * k, sy = y + size / 2 + (car.pos.z - c.z) * k;
        if (sx > x && sx < x + size && sy > y && sy < y + size) DrawRectangle((int)sx - 1, (int)sy - 1, 3, 3, {60, 60, 70, 255});
    }
    if (CH.on)
        for (auto& cp : CH.cops) {
            float sx = x + size / 2 + (cp.pos.x - c.x) * k, sy = y + size / 2 + (cp.pos.z - c.z) * k;
            sx = clampf(sx, x + 6, x + size - 6);
            sy = clampf(sy, y + 6, y + size - 6);
            DrawCircle((int)sx, (int)sy, 6, ((int)(GetTime() * 6) % 2) ? C_COP : Color{70, 110, 255, 255});
            DrawCircleLines((int)sx, (int)sy, 6, WHITE);
        }
    EndScissorMode();
    // tokens outside the view: arrows on the border
    drawPlayerArrow({(float)x + size / 2, (float)y + size / 2}, P.state == Player::AIR ? atan2f(P.vel.z, P.vel.x) : P.yaw, 9, {230, 40, 40, 255});
    DrawRectangleLines(x - 1, y - 1, size + 2, size + 2, {255, 255, 255, 120});
    textShadow("N", x + size / 2 - 5, y + 4, 18, WHITE);
}

static void drawBigMap() {
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    DrawRectangle(0, 0, sw, sh, {0, 0, 0, 170});
    const MapView& mv = curMap();
    float scale = std::min((sw - 80.f) / mv.tex.width, (sh - 120.f) / mv.tex.height);
    float w = mv.tex.width * scale, h = mv.tex.height * scale;
    float x0 = (sw - w) / 2, y0 = (sh - h) / 2 + 20;
    DrawTexturePro(mv.tex, {0, 0, (float)mv.tex.width, (float)mv.tex.height}, {x0, y0, w, h}, {0, 0}, 0, WHITE);
    BeginScissorMode((int)x0, (int)y0, (int)w, (int)h);
    auto S2 = [&](float wx, float wz) { return Vector2{x0 + (wx - mv.x0) / mv.res * scale, y0 + (wz - mv.z0) / mv.res * scale}; };
    for (auto& l : W.labels) {
        Vector2 p = S2(l.x, l.z);
        int fs = l.size == 2 ? 22 : (l.size == 1 ? 18 : 13);
        Color c = l.size == 2 ? Color{220, 235, 255, 230} : Color{40, 40, 50, 255};
        if (l.size >= 1) c = {255, 255, 255, 235};
        int tw = MeasureText(l.name, fs);
        if (l.size == 0) DrawText(l.name, (int)p.x - tw / 2 + 1, (int)p.y + 1, fs, {255, 255, 255, 140});
        DrawText(l.name, (int)p.x - tw / 2, (int)p.y, fs, c);
    }
    for (size_t i = 0; i < W.spots.size(); i++) {
        Vector2 p = S2(W.spots[i].pos.x, W.spots[i].pos.z);
        DrawCircleV(p, 11, {30, 30, 40, 230});
        char b[4];
        snprintf(b, sizeof b, "%d", (int)((i + 1) % 10));
        DrawText(b, (int)p.x - MeasureText(b, 16) / 2, (int)p.y - 8, 16, WHITE);
    }
    for (auto& t : W.tokens)
        if (!t.got) DrawCircleV(S2(t.pos.x, t.pos.z), 5, {255, 200, 40, 255});
    if (CH.on)
        for (auto& cp : CH.cops) DrawCircleV(S2(cp.pos.x, cp.pos.z), 8, C_COP);
    drawPlayerArrow(S2(P.pos.x, P.pos.z), P.yaw, 11, {230, 40, 40, 255});
    EndScissorMode();
    textCenter("MAP  -  press a number key to travel there,  M to close", sw / 2, 20, 22, WHITE);
}

// ---------------------------------------------------------------- SFO air traffic
// One jet takes off west on runway 28R and one lands on 28L, on a 75 second loop.
struct Jet { Vector3 pos; float yaw, pitch; bool visible; Color livery; };
static void airTraffic(float t, Jet out[2]) {
    const float Y = AIRPORT_Y;
    float a = fmodf(t, 75.f);
    // departure
    {
        Jet& j = out[0];
        j.livery = {30, 60, 140, 255};
        j.yaw = PI;
        float x, alt = 0, pitch = 0;
        if (a < 28) {
            x = 2600 - 0.5f * 2.2f * a * a;
            if (a > 22) { alt = sq(a - 22) * 0.45f; pitch = 0.1f * (a - 22) / 6; }
        } else {
            x = 2600 - 0.5f * 2.2f * 28 * 28 - 62 * (a - 28);
            alt = sq(6) * 0.45f + 13 * (a - 28);
            pitch = 0.2f;
        }
        j.pos = {x, Y + alt, 2262};
        j.pitch = pitch;
        j.visible = true;
    }
    // arrival
    {
        Jet& j = out[1];
        j.livery = {190, 30, 40, 255};
        j.yaw = PI;
        float b = fmodf(t + 30, 75.f);
        float x, alt;
        if (b < 30) { x = 2560 + 70 * (30 - b); alt = (x - 2560) * 0.052f; j.pitch = 0.04f; }
        else { float tau = std::min(b - 30, 27.f); x = 2560 - (70 * tau - 0.5f * 2.6f * tau * tau); alt = 0; j.pitch = 0; }
        j.pos = {x, Y + alt, 2182};
        j.visible = b < 62;
    }
}

// ---------------------------------------------------------------- main
int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(1600, 900, "SF Skate");
    SetTargetFPS(60);
    S.init();

    loadingScreen("Starting up", 0);
    double genT0 = GetTime();
    W.generate(loadingScreen);
    TraceLog(LOG_INFO, "SFSKATE: generated in %.2fs", GetTime() - genT0);
    W.upload();
    gMaps[0] = {LoadTextureFromImage(W.mapImg), -1700, -3300, 4};
    gMaps[1] = {LoadTextureFromImage(W.mapImg2), 1000, 8500, 4};
    for (auto& m : gMaps) SetTextureFilter(m.tex, TEXTURE_FILTER_BILINEAR);
    loadingScreen("Waking up the city", 0.98f);
    T.init(W);
    JS.place(W);
    SP.place(W);
    TraceLog(LOG_INFO, "SFSKATE: %d seaplanes moored", (int)SP.moored.size());
    if (getenv("SFSKATE_SKI")) for (auto& s : SP.moored) TraceLog(LOG_INFO, "SFSKATE: seaplane at %.0f,%.0f yaw %.2f", s.pos.x, s.pos.z, s.yaw); // test hook
    GO.init(W);
    TraceLog(LOG_INFO, "SFSKATE: %d gondola lines, %d cabins", (int)W.gondolas.size(), (int)GO.cabins.size());
    if (getenv("SFSKATE_SKI")) for (auto& g : W.gondolas) TraceLog(LOG_INFO, "SFSKATE: gondola from %.0f,%.0f len %.0f", g.path[0].x, g.path[0].z, g.len);
    TraceLog(LOG_INFO, "SFSKATE: %d jet skis moored along the shore", (int)JS.skis.size());
    if (getenv("SFSKATE_SKI")) for (auto& s : JS.skis) TraceLog(LOG_INFO, "SFSKATE: ski at %.0f,%.0f yaw %.2f", s.pos.x, s.pos.z, s.yaw); // test hook

    Shader sh = LoadShaderFromMemory(VS, FS);
    Material mat = LoadMaterialDefault();
    mat.shader = sh;
    int locCam = GetShaderLocation(sh, "camPos"), locSun = GetShaderLocation(sh, "sunDir"), locSunC = GetShaderLocation(sh, "sunCol");
    int locAmb = GetShaderLocation(sh, "ambCol"), locFog = GetShaderLocation(sh, "fogCol"), locSky = GetShaderLocation(sh, "skyCol");
    int locFogD = GetShaderLocation(sh, "fogDen"), locNight = GetShaderLocation(sh, "night"), locTime = GetShaderLocation(sh, "time");
    rlDisableBackfaceCulling();

    const Spot& start = W.spots[0];
    P.spawn(start.pos, start.yaw);

    Camera3D cam{};
    cam.up = {0, 1, 0};
    cam.fovy = 65;
    cam.projection = CAMERA_PERSPECTIVE;
    float camYaw = P.yaw, orbit = 0, orbitPitch = 0, orbitHold = 0;
    int camMode = 0;
    const float CAM_D[3] = {5.2f, 9.0f, 3.0f}, CAM_H[3] = {2.1f, 3.4f, 1.1f};
    Vector3 camPos = {P.pos.x - 6, P.pos.y + 3, P.pos.z};

    int preset = 0;
    Sky sky = PRESETS[0];
    bool heavyFog = false;
    float fogDen = 0.00011f;
    bool showHelp = true, showMap = false, title = true, paused = false;
    bool picker = true;   // landmark / time-of-day chooser (start screen and Tab mid-game)
    struct Ride { bool on = false; float s = 0, target = 0, v = 0; int dir = 1, dest = 0; } ride; // Caltrain trip
    const int menloStation = (int)W.stations.size() - 1;
    int pickIdx = 0;
    const char* lastDistrict = "";
    float districtT = 0;
    std::vector<Vector3> stars;
    {
        RNG r(5);
        for (int i = 0; i < 400; i++) {
            float a = r.range(0, TAU_F), e = asinf(r.range(0.05f, 1.0f));
            stars.push_back({cosf(a) * cosf(e), sinf(e), sinf(a) * cosf(e)});
        }
    }
    struct Puff { Vector3 dir; float size; };
    std::vector<Puff> clouds;
    {
        RNG r(77);
        for (int c = 0; c < 22; c++) {
            float az = r.range(0, TAU_F), el = r.range(0.04f, 0.2f);
            int n = r.irange(5, 10);
            for (int k = 0; k < n; k++) {
                float a = az + r.range(-0.07f, 0.07f), e = el + r.range(-0.012f, 0.02f);
                clouds.push_back({{cosf(a) * cosf(e), sinf(e), sinf(a) * cosf(e)}, r.range(250, 600)});
            }
        }
    }
    float titleT = 0, sirenT = 0, landKick = 0, swingT = 0, squeezeYaw = 0;
    if (getenv("SFSKATE_NIGHT")) preset = 2; // test hook: start at night
    if (getenv("SFSKATE_COPS")) CH.calmT = 0.5f; // test hook: cops show up right away
    int tokensGot = 0;
    // SFSKATE_AUTOTEST=1: visit each teleport spot, save a screenshot, log FPS, then quit (used for testing)
    bool autotest = getenv("SFSKATE_AUTOTEST") != nullptr;
    if (autotest && getenv("SFSKATE_VIEW")) { // optional: first test shot from a custom spot "x,z,yaw"
        float vx, vz, vy, yref = 500;
        if (sscanf(getenv("SFSKATE_VIEW"), "%f,%f,%f,%f", &vx, &vz, &vy, &yref) >= 3) W.spots[0] = {"View", {vx, W.ground(vx, vz, yref), vz}, vy};
    }
    int atFrame = 0, atSpot = -1;
    double atFpsSum = 0; int atFpsN = 0;
    TraceLog(LOG_INFO, "SFSKATE: world ready. chunks=%d platforms=%d rails=%d cars=%d peds=%d",
             (int)W.chunks.size(), (int)W.plats.size(), (int)W.rails.size(), (int)T.cars.size(), (int)T.peds.size());

    while (!WindowShouldClose()) {
        float dt = std::min(GetFrameTime(), 1.f / 30.f);
        float now = (float)GetTime();
        int sw = GetScreenWidth(), sh2 = GetScreenHeight();

        if (autotest && getenv("SFSKATE_RIDE")) { // test hook: ride the train south and take a few shots
            static int rf = 0;
            rf++;
            if (rf == 5) { title = false; picker = false; ride.on = true; ride.dest = menloStation; ride.s = W.stations[0].s;
                           ride.target = W.stations[menloStation].s; ride.dir = 1; ride.v = 0; }
            if (rf == 900) TakeScreenshot("ride_1.png");
            if (rf == 2400) TakeScreenshot("ride_2.png");
            if (rf == 4200) { TakeScreenshot("ride_3.png"); break; }
            if (rf > 4200) break;
        } else
        if (autotest) {
            atFrame++;
            if (atFrame == 60) TakeScreenshot("shot_title.png");
            if (atFrame == 61) { title = false; picker = false; }
            if (atFrame == 75 && getenv("SFSKATE_SCUFFLE")) { // test hook: an officer grabs the skater (plus a cruiser and a second officer)
                CH.reset(0);
                CH.active = true; CH.stars = 2;
                Cop k; k.onFoot = true; k.pos = {P.pos.x + cosf(P.yaw) * 1.4f, P.pos.y, P.pos.z + sinf(P.yaw) * 1.4f}; k.yaw = P.yaw + PI;
                CH.cops.push_back(k);
                Cop k2; k2.onFoot = true; k2.pos = {P.pos.x + cosf(P.yaw) * 5.f + 2.f, P.pos.y, P.pos.z + sinf(P.yaw) * 5.f}; k2.yaw = P.yaw + PI; k2.phase = 1.2f; k2.speed = 8;
                CH.cops.push_back(k2);
                Cop k3; k3.pos = {P.pos.x + cosf(P.yaw) * 14.f - 3.f, P.pos.y, P.pos.z + sinf(P.yaw) * 14.f}; k3.yaw = P.yaw + PI;
                CH.cops.push_back(k3);
                CH.parked.push_back({{P.pos.x + cosf(P.yaw) * 9.f - 4.f, P.pos.y, P.pos.z + sinf(P.yaw) * 9.f - 4.f}, P.yaw + 0.5f});
            }
            if (getenv("SFSKATE_FLY") && atFrame > 61) { // test hook: board seaplane 0, take off and climb, with shots along the way
                int fr = atFrame - 62;
                if (fr == 0) { P.spawn(SP.moored[0].pos, 0); SP.mount(0, P); SP.yaw -= PI / 2; camYaw = SP.yaw; }
                if (fr == 90 || fr == 300 || fr == 600) TakeScreenshot(TextFormat("fly_%d.png", fr));
                if (fr % 100 == 0) TraceLog(LOG_INFO, "SFSKATE: fly fr=%d riding=%d water=%d speed=%.1f alt=%.0f pitch=%.2f", fr, (int)SP.riding, (int)SP.onWater, SP.speed, SP.altitude(W), SP.pitch);
                if (fr == (getenv("SFSKATE_FLY")[0] == '2' ? 1500 : 620)) break;
            } else
            if (getenv("SFSKATE_SKIRIDE") && atFrame > 61 && atFrame < 400) { // test hook: hop on ski 0 and cruise, shots at 3 moments
                int fr = atFrame - 62;
                if (fr == 0) { P.spawn({JS.skis[0].pos.x, 0, JS.skis[0].pos.z}, 0); JS.mount(0, P); JS.yaw += PI; camYaw = JS.yaw; camPos = {P.pos.x - 8, 3, P.pos.z}; }
                if (fr == 60 || fr == 140 || fr == 220) TakeScreenshot(TextFormat("ski_%d.png", fr));
                if (fr == 140) TraceLog(LOG_INFO, "SFSKATE: ski speed %.1f at %.0f,%.0f riding=%d", JS.speed, P.pos.x, P.pos.z, (int)JS.riding);
                if (fr == 250) break;
            } else
            if (atFrame > 61 && (atFrame - 62) % 150 == 0) {
                atSpot++;
                if (atSpot >= (int)W.spots.size() + 2) break;
                if (atSpot < (int)W.spots.size()) {
                    P.spawn(W.spots[atSpot].pos, W.spots[atSpot].yaw);
                    camYaw = P.yaw;
                    camPos = {P.pos.x - cosf(P.yaw) * 6, P.pos.y + 3, P.pos.z - sinf(P.yaw) * 6};
                } else if (atSpot == (int)W.spots.size()) { preset = 2; P.spawn(W.spots[2].pos, W.spots[2].yaw); camYaw = P.yaw; }
                else { preset = 1; P.spawn(W.spots[1].pos, W.spots[1].yaw); camYaw = P.yaw; }
            }
            if (atFrame > 61 && (atFrame - 62) % 150 == 140) {
                TakeScreenshot(TextFormat("shot_%02d.png", atSpot));
                TraceLog(LOG_INFO, "SFSKATE: spot %d fps=%d pos=%.1f,%.1f,%.1f state=%d", atSpot, GetFPS(), P.pos.x, P.pos.y, P.pos.z, (int)P.state);
            }
            if (atFrame > 100) { atFpsSum += GetFPS(); atFpsN++; }
        }
        // ---------------- input
        bool justStarted = false;
        auto setCops = [&](bool on) { // game mode: free roam (no police) or cop chase
            if (CH.on == on) return;
            CH.on = on;
            CH.reset(8);
        };
        auto startAt = [&](int idx) {
            justStarted = true;
            ride.on = false;
            JS.riding = false; JS.cur = -1;
            if (SP.riding) { SP.riding = false; SP.cur = -1; }
            P.spawn(W.spots[idx].pos, W.spots[idx].yaw);
            camYaw = P.yaw;
            camPos = {P.pos.x - cosf(P.yaw) * 6, P.pos.y + 3, P.pos.z - sinf(P.yaw) * 6};
            P.say(W.spots[idx].name, {200, 230, 255, 255}, PRESET_NAMES[preset]);
            showMap = false;
            paused = false;
        };
        if (title) titleT += dt;
        if (picker && !autotest) {
            int n = (int)W.spots.size();
            if (IsKeyPressed(KEY_DOWN) || IsKeyPressed(KEY_S)) pickIdx = (pickIdx + 1) % n;
            if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_W)) pickIdx = (pickIdx + n - 1) % n;
            if (IsKeyPressed(KEY_RIGHT) || IsKeyPressed(KEY_D)) preset = (preset + 1) % 3;
            if (IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_A)) preset = (preset + 2) % 3;
            for (int k = 0; k < 10 && k < n; k++)
                if (IsKeyPressed(k == 9 ? KEY_ZERO : KEY_ONE + k)) pickIdx = k;
            if (IsKeyPressed(KEY_B)) setCops(!CH.on);
            // mouse: click a landmark row, or a time-of-day button
            Vector2 m = GetMousePosition();
            int px = sw / 2 - 330, py = sh2 / 2 - 230;
            for (int i = 0; i < n; i++) {
                Rectangle r = {(float)px + 20, (float)py + 90 + i * 34, 380, 30};
                if (CheckCollisionPointRec(m, r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                    if (pickIdx == i) { title = false; picker = false; startAt(i); }
                    pickIdx = i;
                }
            }
            for (int t = 0; t < 3; t++) {
                Rectangle r = {(float)px + 430, (float)py + 120 + t * 50, 210, 40};
                if (CheckCollisionPointRec(m, r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) preset = t;
            }
            for (int md = 0; md < 2; md++) { // game mode: 0 free roam, 1 cop chase
                Rectangle r = {(float)px + 430, (float)py + 316 + md * 44, 210, 40};
                if (CheckCollisionPointRec(m, r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) setCops(md == 1);
            }
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE)) { title = false; picker = false; startAt(pickIdx); }
            else if (!title && IsKeyPressed(KEY_TAB)) picker = false;
        } else if (!title) {
            if (IsKeyPressed(KEY_TAB)) { picker = true; showMap = false; }
            if (IsKeyPressed(KEY_H)) showHelp = !showHelp;
            if (IsKeyPressed(KEY_N)) {
                CH.difficulty = (CH.difficulty + 1) % 3;
                P.say(TextFormat("Police: %s", Chase::DIFF_NAMES[CH.difficulty]), C_COP, CH.difficulty == 0 ? "Slower cops, a weaker grip" : CH.difficulty == 2 ? "Faster cops, heat builds quickly" : "The default chase");
            }
            if (IsKeyPressed(KEY_B)) { setCops(!CH.on); P.say(CH.on ? "COP CHASE MODE" : "FREE ROAM MODE", CH.on ? C_COP : WHITE, CH.on ? "Police on patrol" : "No police, just skate"); }
            if (IsKeyPressed(KEY_M)) showMap = !showMap;
            if (IsKeyPressed(KEY_P)) paused = !paused;
        }
        if (IsKeyPressed(KEY_T)) {
            preset = (preset + 1) % 3;
            if (!title && !picker) P.say(PRESET_NAMES[preset], {255, 225, 160, 255}, "Time of day");
        }
        if (!title && !picker) {
            if (IsKeyPressed(KEY_F1)) { preset = 0; P.say(PRESET_NAMES[0], {255, 225, 160, 255}, "Time of day"); }
            if (IsKeyPressed(KEY_F2)) { preset = 1; P.say(PRESET_NAMES[1], {255, 225, 160, 255}, "Time of day"); }
            if (IsKeyPressed(KEY_F3)) { preset = 2; P.say(PRESET_NAMES[2], {255, 225, 160, 255}, "Time of day"); }
        }
        if (IsKeyPressed(KEY_F)) heavyFog = !heavyFog;
        if (IsKeyPressed(KEY_C)) camMode = (camMode + 1) % 3;
        for (int k = 0; k < 10 && !title && !picker; k++) {
            if (IsKeyPressed(KEY_ONE + k) || (k == 9 && IsKeyPressed(KEY_ZERO))) {
                int idx = k;
                if (k == 9 && !IsKeyPressed(KEY_ZERO)) continue;
                if (idx < (int)W.spots.size()) {
                    P.spawn(W.spots[idx].pos, W.spots[idx].yaw);
                    camYaw = P.yaw;
                    camPos = {P.pos.x - cosf(P.yaw) * 6, P.pos.y + 3, P.pos.z - sinf(P.yaw) * 6};
                    P.say(W.spots[idx].name, {200, 230, 255, 255}, "Teleported");
                    showMap = false;
                }
            }
        }
        if (!title && !picker && IsKeyPressed(KEY_R)) { if (JS.riding) { JS.skis[JS.cur].pos = P.pos; JS.riding = false; JS.cur = -1; } if (SP.riding) { SP.riding = false; SP.cur = -1; } P.spawn(P.safePos, P.safeYaw); P.say("Reset", WHITE); }

        Input in;
        in.steer = (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT) ? 1.f : 0.f) - (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT) ? 1.f : 0.f);
        in.push = IsKeyDown(KEY_W) || IsKeyDown(KEY_UP);
        in.brake = IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN);
        in.tuck = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (autotest && getenv("SFSKATE_SKIRIDE")) { in.push = true; in.steer = (atFrame % 200) > 120 ? 0.6f : 0.f; in.tuck = atFrame > 160; }
        in.ollie = IsKeyPressed(KEY_SPACE) && !justStarted;
        in.ollieHeld = IsKeyDown(KEY_SPACE);
        in.kickflip = IsKeyPressed(KEY_J);
        in.heelflip = IsKeyPressed(KEY_K);
        in.shove = IsKeyPressed(KEY_L);
        in.grab = IsKeyDown(KEY_I);
        if (CH.scuffle && !title && !paused) {
            // caught: no riding, just struggling. Mash for strength, or swing the board for a big shove.
            float s = 0;
            if (IsKeyPressed(KEY_SPACE)) s += 0.10f;
            if (IsKeyPressed(KEY_A) || IsKeyPressed(KEY_D) || IsKeyPressed(KEY_LEFT) || IsKeyPressed(KEY_RIGHT)) s += 0.07f;
            if ((IsKeyPressed(KEY_J) || IsKeyPressed(KEY_K) || IsKeyPressed(KEY_L)) && CH.hitCD <= 0) { s += 0.17f; CH.hitCD = 0.22f; swingT = 0.22f; S.play(S.pop, 0.9f, 0.7f); }
            CH.strike += s;
            in = Input{};
        }
        if (autotest && getenv("SFSKATE_LAND")) { // test hook: roll, ollie, land, with screenshots on the way down
            if (atFrame == 66) P.speed = 8.f;
            if (atFrame == 80) in.ollie = true;
            if (atFrame >= 66 && atFrame <= 160) P.speed = std::max(P.speed, 6.f);
            static const int shots[4] = {96, 118, 138, 143};
            for (int k = 0; k < 4; k++) if (atFrame == shots[k]) TakeScreenshot(TextFormat("land_%d.png", k));
            if (atFrame >= 80 && atFrame <= 160 && atFrame % 4 == 0) TraceLog(LOG_INFO, "LAND f=%d state=%d y=%.2f vy=%.2f", atFrame, (int)P.state, P.pos.y, P.vy);
        }
        if (autotest && CH.scuffle && getenv("SFSKATE_SCUFFLE")) CH.strike += 0.0075f; // test hook: hold the fight steady for the screenshot
        if (swingT > 0) { swingT -= dt; P.flipAngle = 360.f * clampf(1.f - swingT / 0.22f, 0.f, 1.f); }
        else if (P.state == Player::RIDE && P.flipT < 0) P.flipAngle = 0;

        // ---------------- update
        bool running = !title && !paused && !showMap && !picker && !ride.on && !JS.riding && !SP.riding;
        // Caltrain: board at a station platform with E
        int nearStation = -1;
        if (running && P.state != Player::BAIL) {
            float s;
            float d = W.railDistance(P.pos.x, P.pos.z, &s);
            for (int i = 0; i < (int)W.stations.size(); i++)
                if (d < 14 && fabsf(s - W.stations[i].s) < 80) nearStation = i;
        }
        if (nearStation >= 0 && IsKeyPressed(KEY_E)) {
            ride.on = true;
            ride.dest = nearStation == menloStation ? 0 : menloStation;
            ride.s = W.stations[nearStation].s;
            ride.target = W.stations[ride.dest].s;
            ride.dir = ride.target > ride.s ? 1 : -1;
            ride.v = 0;
            P.evPop = P.evLand = P.evBail = P.evSplash = P.evGrindStart = false;
            running = false;
        }
        // jet skis: hop on a moored one with E, hop off near the shore with E
        int nearSki = -1;
        bool justMounted = false;
        if (running && P.state != Player::BAIL) nearSki = JS.nearest(P.pos, 10.f);
        if (nearSki >= 0 && IsKeyPressed(KEY_E)) {
            justMounted = true; // the same E press must not also hop straight back off
            JS.mount(nearSki, P);
            CH.reset(8);
            P.evPop = P.evLand = P.evBail = P.evSplash = P.evGrindStart = false;
            camYaw = P.yaw;
            P.say("JET SKI", {120, 220, 255, 255}, "Hold W to go, Shift to boost, E near the shore to hop off");
            running = false;
        }
        if (JS.riding && !paused && !picker && !showMap && !title) {
            if (IsKeyPressed(KEY_E) && !justMounted) {
                if (JS.dismount(P, W)) { camYaw = P.yaw; P.say("Back on land", WHITE); }
                else P.say("Get closer to the shore to hop off", WHITE);
            }
            if (JS.riding) { JS.update(dt, in, in.tuck, P, W); T.update(dt, W, P); }
            if (JS.evMount) S.play(S.splash, 0.5f, 1.3f);
            if (JS.evCrash) S.play(S.splash, 0.9f, 0.7f);
            JS.evMount = JS.evCrash = false;
        }
        // seaplanes: climb into a moored one with E; taxi, fly, land on the water, and E near the shore to climb out
        int nearPlane = -1;
        if (running && P.state != Player::BAIL && nearSki < 0) nearPlane = SP.nearest(P.pos, 16.f);
        if (nearPlane >= 0 && IsKeyPressed(KEY_E) && !justMounted) {
            justMounted = true;
            SP.mount(nearPlane, P);
            CH.reset(8);
            P.evPop = P.evLand = P.evBail = P.evSplash = P.evGrindStart = false;
            camYaw = P.yaw;
            P.say("SEAPLANE", {255, 225, 140, 255}, "W throttle up, Space to lift off past 55 mph");
            running = false;
        }
        if (SP.riding && !paused && !picker && !showMap && !title) {
            if (IsKeyPressed(KEY_E) && !justMounted) {
                if (SP.dismount(P, W)) { camYaw = P.yaw; P.say("Back on land", WHITE); }
                else P.say(SP.onWater ? "Slow down near the shore to climb out" : "Land on the water first", WHITE);
            }
            if (SP.riding) {
                bool climb = IsKeyDown(KEY_SPACE), dive = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
                if (autotest && getenv("SFSKATE_FLY")) { // test hook: full throttle, lift off, climb to ~80 m, then bank
                    bool land = getenv("SFSKATE_FLY")[0] == '2' && atFrame > 420;   // "2": then throttle back and glide down onto the water
                    in.push = !land; in.brake = land;
                    climb = SP.speed > 27 && SP.altitude(W) < 80 && SP.pitch < 0.25f && !land;
                    if (land) { climb = SP.pitch < -0.12f; dive = SP.pitch > -0.08f && SP.altitude(W) > 6; }
                    in.steer = atFrame > 450 && !land ? 0.6f : 0.f;
                }
                SP.update(dt, in, climb, dive, P, W);
                T.update(dt, W, P);
                if (!SP.riding) camYaw = P.yaw;   // crashed and put back on land
            }
            if (SP.evMount) S.play(S.splash, 0.4f, 1.5f);
            if (SP.evSplash) { S.play(S.splash, 1.0f, 0.8f); P.say("Touchdown", {200, 235, 255, 255}); }
            if (SP.evCrash) S.play(S.bail, 1.0f, 0.6f);
            SP.evMount = SP.evSplash = SP.evCrash = false;
        }
        if (ride.on && !paused && !picker && !showMap) {
            float remain = (ride.target - ride.s) * ride.dir;
            float vt = std::min(120.f, sqrtf(2 * 5.f * std::max(remain, 0.f)));
            ride.v += clampf(vt - ride.v, -6 * dt, 4 * dt);
            ride.v = std::max(ride.v, remain > 1 ? 2.f : 0.f);
            ride.s += ride.dir * ride.v * dt;
            T.update(dt, W, P);
            if (remain < 0.5f || IsKeyPressed(KEY_SPACE)) {
                Vector3 p = W.trackPoint(ride.target, 6.2f, nullptr);
                P.spawn({p.x, W.ground(p.x, p.z, p.y + 3), p.z}, PI);
                camYaw = PI;
                camPos = {p.x + 6, p.y + 3, p.z};
                P.say(W.stations[ride.dest].name, {255, 225, 160, 255}, "Arrived by Caltrain");
                ride.on = false;
            }
        }
        if (running) {
            P.update(dt, in, W);
            T.update(dt, W, P);
            T.interact(P);
            {
                Jet jets[2];
                airTraffic(now, jets);
                for (auto& j : jets) {
                    if (!j.visible || j.pos.y > P.pos.y + 5) continue;
                    if (fabsf(P.pos.x - j.pos.x) < 19 && fabsf(P.pos.z - j.pos.z) < 17 && P.pos.y > j.pos.y - 1) {
                        P.bail("Hit by a plane!");
                        S.play(S.bail);
                    }
                }
            }
            CH.update(dt, P, W);
            if (CH.evStart) { S.play(S.siren, 0.8f); sirenT = 0; }
            if (CH.evEscape) S.play(S.token, 1.0f, 1.25f);
            if (CH.evBusted) S.play(S.bail, 0.8f, 0.8f);
            if (CH.evGrab) S.play(S.bail, 0.6f, 1.3f);
            if (CH.evShove) S.play(S.land, 1.0f, 1.2f);
            if (CH.evBowled) S.play(S.land, 1.0f, 0.7f);
            CH.evStart = CH.evEscape = CH.evBusted = CH.evGrab = CH.evShove = CH.evBowled = false;
            if (CH.active) {
                sirenT -= dt;
                if (sirenT <= 0) { S.play(S.siren, clampf(1.1f - CH.nearest / 250.f, 0.12f, 0.8f)); sirenT = 1.4f; }
            }
            for (auto& t : W.tokens) {
                if (t.got) continue;
                if (Vector3Distance(t.pos, {P.pos.x, P.pos.y + 1.0f, P.pos.z}) < 3.0f) {
                    t.got = true;
                    tokensGot++;
                    P.score += 1000;
                    P.say(TextFormat("TOKEN %d/%d", tokensGot, (int)W.tokens.size()), {255, 210, 60, 255}, t.name);
                    S.play(S.token);
                    if (tokensGot == (int)W.tokens.size()) P.say("ALL TOKENS!", {255, 210, 60, 255}, "You've skated all of San Francisco");
                }
            }
        } else if (title) {
            T.update(dt, W, P);
        }
        // sounds
        if (P.evPop) S.play(S.pop, 0.8f, 1.0f + GetRandomValue(-10, 10) / 100.f);
        if (P.evLand) S.play(S.land, 0.9f);
        if (P.evSplash) S.play(S.splash, 1.0f);
        else if (P.evBail) S.play(S.bail, 0.9f);
        if (P.evGrindStart) S.play(S.clack, 1.0f, 0.7f);
        if (T.evHonk) S.play(S.honk, 0.6f, 1.0f + GetRandomValue(-15, 15) / 100.f);
        float spd = P.hspeed();
        if (ride.on) S.setLevels(0.25f + ride.v / 160.f, 0, ride.v / 200.f);
        else if (SP.riding) S.setLevels(0.2f + 0.45f * SP.throttle, 0, SP.speed / 50.f);
        else if (JS.riding) S.setLevels(0.15f + 0.3f * JS.throttle, 0, fabsf(JS.speed) / 24.f);
        else S.setLevels(running && P.state == Player::RIDE ? spd / 14.f : 0.f, running && P.state == Player::GRIND ? 0.8f : 0.f,
                    running ? (spd - 6.f) / 22.f : 0.f);

        // sky & fog
        if (!paused) GO.update(dt, W);
        sky = lerpSky(sky, PRESETS[preset], 1 - expf(-dt * 2.0f));
        float fogTarget = heavyFog ? 0.0022f : (preset == 2 ? 0.00009f : 0.00011f);
        fogDen = lerpf(fogDen, fogTarget, 1 - expf(-dt * 1.5f));
        Vector3 fogC = sky.fog;
        if (heavyFog) fogC = Vector3Lerp(fogC, Vector3Scale({0.78f, 0.8f, 0.83f}, 1.0f - sky.night * 0.8f), 0.7f);
        Vector3 horizonC = heavyFog ? fogC : sky.horizon;

        // ---------------- camera
        if (title) {
            float a = titleT * 0.05f + 0.6f;
            cam.position = {250 + cosf(a) * 1700, 420, -300 + sinf(a) * 1700};
            cam.target = {250, 20, -350};
            cam.fovy = 55;
        } else if (ride.on) {
            // trackside chase camera, a little above and beside the leading car
            Vector2 d;
            Vector3 lead = W.trackPoint(ride.s, 2.4f * ride.dir, &d);
            Vector2 f = Vector2Scale(d, (float)ride.dir), rr = {-f.y, f.x};
            Vector3 want = {lead.x - f.x * 45 - rr.x * 24, lead.y + 11, lead.z - f.y * 45 - rr.y * 24};
            float gy = W.terrain(want.x, want.z) + 4;
            if (want.y < gy) want.y = gy;
            camPos = Vector3Lerp(camPos, want, 1 - expf(-dt * 3));
            cam.position = camPos;
            cam.target = {lead.x - f.x * 30, lead.y + 3, lead.z - f.y * 30};
            cam.fovy = lerpf(cam.fovy, 60, 1 - expf(-dt * 3));
        } else {
            float targetYaw = camYaw;
            if (P.state == Player::AIR) {
                float hv = sqrtf(P.vel.x * P.vel.x + P.vel.z * P.vel.z);
                if (hv > 1) targetYaw = atan2f(P.vel.z, P.vel.x);
            } else if (P.state == Player::GRIND) {
                const Rail& r = W.rails[P.rail];
                targetYaw = atan2f((r.b.z - r.a.z) * P.railDir, (r.b.x - r.a.x) * P.railDir);
            } else if (P.state == Player::RIDE) {
                targetYaw = P.speed < -0.5f ? wrapPi(P.yaw + PI) : P.yaw;
                if (fabsf(P.speed) < 0.3f) targetYaw = camYaw;
            }
            camYaw = lerpAngle(camYaw, targetYaw, 1 - expf(-dt * 3.5f));
            if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
                Vector2 md = GetMouseDelta();
                orbit += md.x * 0.006f;
                orbitPitch = clampf(orbitPitch + md.y * 0.004f, -0.3f, 1.1f);
                orbitHold = 1.5f;
            } else {
                orbitHold -= dt;
                if (orbitHold < 0) { orbit *= expf(-dt * 2); orbitPitch *= expf(-dt * 2); }
            }
            float wheel = GetMouseWheelMove();
            static float zoom = 1.f;
            zoom = clampf(zoom - wheel * 0.1f, 0.5f, 3.0f);
            float planeK = SP.riding ? 3.2f : 1.f;   // pull well back to fit the wings in
            float dist = CAM_D[camMode] * zoom * planeK, hgt = CAM_H[camMode] * zoom * (SP.riding ? 1.8f : 1.f) + orbitPitch * dist;
            Vector3 target = {P.pos.x, P.pos.y + 1.2f, P.pos.z};
            // squeezed against a wall (tight rooms): swing round to whichever side has the most room
            auto freeFrac = [&](float cyy) {
                Vector3 d = {-cosf(cyy) * dist, hgt, -sinf(cyy) * dist};
                for (int i = 1; i <= 14; i++)
                    if (W.solidAt(Vector3Add(target, Vector3Scale(d, i / 14.f)))) return (i - 1) / 14.f;
                return 1.f;
            };
            {
                float base = camYaw + orbit, wantSq = squeezeYaw;
                float cur = freeFrac(base + squeezeYaw);
                if (cur < 0.55f && !IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
                    float bestF = cur;
                    for (int k = -8; k <= 8; k++) {
                        float a = k * PI / 8.f, f = freeFrac(base + a) - fabsf(a) * 0.03f;
                        if (f > bestF + 0.05f) { bestF = f; wantSq = a; }
                    }
                } else if (freeFrac(base) > 0.9f) wantSq = 0;
                squeezeYaw += wrapPi(wantSq - squeezeYaw) * (1 - expf(-dt * 3.f));
            }
            float cy = camYaw + orbit + squeezeYaw;
            Vector3 desired = {P.pos.x - cosf(cy) * dist, P.pos.y + hgt, P.pos.z - sinf(cy) * dist};
            // pull the camera in front of walls
            Vector3 dir = Vector3Subtract(desired, target);
            float best = 1.f;
            for (int i = 1; i <= 14; i++) {
                float t = i / 14.f;
                if (W.solidAt(Vector3Add(target, Vector3Scale(dir, t)))) { best = std::max(0.12f, (i - 1) / 14.f); break; }
            }
            desired = Vector3Add(target, Vector3Scale(dir, best));
            if (best < 0.6f) {
                // squeezed in a small room: rise toward the ceiling and look down instead of sitting inside the skater
                float cl = W.ceiling(desired.x, desired.z, P.pos.y);
                float lift = std::min(1.7f, 0.9f + (0.6f - best) * 1.6f);
                desired.y = std::max(desired.y, std::min(P.pos.y + lift, cl - 0.25f));
            }
            float gy = W.terrain(desired.x, desired.z) + 0.4f;
            if (desired.y < gy) desired.y = gy;
            camPos = Vector3Lerp(camPos, desired, 1 - expf(-dt * 12));
            if (best < 0.99f) camPos = Vector3Lerp(camPos, desired, 0.5f);
            // a hard landing dips the camera and punches the field of view, then it settles
            if (P.landShake > 0) { landKick = std::max(landKick, P.landShake); P.landShake = 0; }
            landKick *= expf(-dt * 6.5f);
            cam.position = camPos;
            cam.position.y -= landKick * 0.45f;
            cam.target = target;
            cam.fovy = lerpf(cam.fovy, 62 + clampf(spd, 0, 30) * 0.6f, 1 - expf(-dt * 3)) + landKick * 5.f;
        }
        if (autotest && getenv("SFSKATE_CAM")) { // test hook: free camera "x,y,z,tx,ty,tz"
            float v[6];
            if (sscanf(getenv("SFSKATE_CAM"), "%f,%f,%f,%f,%f,%f", v, v + 1, v + 2, v + 3, v + 4, v + 5) == 6) { cam.position = {v[0], v[1], v[2]}; cam.target = {v[3], v[4], v[5]}; }
        }
        if (autotest && getenv("SFSKATE_PET")) { // test hook: look at the first dog walker in the city ("cat": the first cat)
            bool cat = getenv("SFSKATE_PET")[0] == 'c';
            Vector3 at{}; float yw = 0;
            if (cat && !T.cats.empty()) { at = T.cats[40 % T.cats.size()].pos; yw = 0; }
            else for (auto& p : T.peds) if (p.pet && p.mode == 0) { at = p.pos; yw = p.yaw; break; }
            float dd = cat ? 2.6f : 4.5f;
            cam.position = {at.x + cosf(yw + 1.1f) * dd, at.y + (cat ? 1.3f : 2.0f), at.z + sinf(yw + 1.1f) * dd};
            cam.target = {at.x + cosf(yw) * (cat ? 0 : 0.5f), at.y + (cat ? 0.2f : 0.6f), at.z + sinf(yw) * (cat ? 0 : 0.5f)};
        }
        if (autotest && getenv("SFSKATE_PEDAL")) // test hook: look at the first pedal bus from the side and a little ahead
            for (auto& c : T.cars)
                if (c.type == CT_PEDALBUS) {
                    float f = cosf(c.yaw), s = sinf(c.yaw);
                    cam.position = {c.pos.x + f * 5.f - s * 6.f, c.pos.y + 3.2f, c.pos.z + s * 5.f + f * 6.f};
                    cam.target = {c.pos.x, c.pos.y + 1.2f, c.pos.z};
                    break;
                }
        Vector3 camFwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
        gMapFocus = ride.on ? cam.target : P.pos;

        // ---------------- shader uniforms
        SetShaderValue(sh, locCam, &cam.position, SHADER_UNIFORM_VEC3);
        SetShaderValue(sh, locSun, &sky.sunDir, SHADER_UNIFORM_VEC3);
        SetShaderValue(sh, locSunC, &sky.sunCol, SHADER_UNIFORM_VEC3);
        SetShaderValue(sh, locAmb, &sky.amb, SHADER_UNIFORM_VEC3);
        SetShaderValue(sh, locFog, &fogC, SHADER_UNIFORM_VEC3);
        SetShaderValue(sh, locSky, &horizonC, SHADER_UNIFORM_VEC3);
        SetShaderValue(sh, locFogD, &fogDen, SHADER_UNIFORM_FLOAT);
        SetShaderValue(sh, locNight, &sky.night, SHADER_UNIFORM_FLOAT);
        SetShaderValue(sh, locTime, &now, SHADER_UNIFORM_FLOAT);

        // ---------------- draw
        BeginDrawing();
        ClearBackground(toColor(fogC));
        {
            // sky gradient down to the horizon line
            float pitch = asinf(clampf(camFwd.y, -1, 1));
            float horizonY = sh2 * 0.5f + tanf(pitch) * (sh2 * 0.5f) / tanf(cam.fovy * 0.5f * DEG2RAD);
            Color top = toColor(heavyFog ? Vector3Lerp(sky.top, fogC, 0.7f) : sky.top);
            int hy = (int)clampf(horizonY, 0, (float)sh2);
            if (hy > 0) DrawRectangleGradientV(0, 0, sw, hy, top, toColor(horizonC));
            {
                float pxPerRad = (sh2 * 0.5f) / tanf(cam.fovy * 0.5f * DEG2RAD);
                Vector3 cc = Vector3Lerp(Vector3Scale(sky.sunCol, 1.15f), {1, 1, 1}, 0.5f);
                if (sky.night > 0.6f) cc = {0.25f, 0.27f, 0.35f};
                unsigned char ca = heavyFog ? 25 : (sky.night > 0.6f ? 40 : 70);
                Color c = toColor(cc);
                c.a = ca;
                for (auto& p : clouds) {
                    if (Vector3DotProduct(p.dir, camFwd) < 0.3f) continue;
                    Vector2 sp = GetWorldToScreen(Vector3Add(cam.position, Vector3Scale(p.dir, 9000)), cam);
                    float rad = p.size / 9000.f * pxPerRad;
                    DrawCircleV(sp, rad, c);
                    DrawCircleV({sp.x, sp.y - rad * 0.25f}, rad * 0.7f, c);
                }
            }
            if (sky.night > 0.6f && !heavyFog) {
                for (auto& s : stars) {
                    if (Vector3DotProduct(s, camFwd) < 0.2f) continue;
                    Vector2 p = GetWorldToScreen(Vector3Add(cam.position, Vector3Scale(s, 1000)), cam);
                    if (p.y < hy) DrawPixelV(p, {255, 255, 240, (unsigned char)(150 + (int)(s.x * 1000) % 100)});
                }
                Vector2 mp = GetWorldToScreen(Vector3Add(cam.position, Vector3Scale(sky.sunDir, 1000)), cam);
                if (Vector3DotProduct(sky.sunDir, camFwd) > 0.2f) DrawCircleV(mp, 18, {235, 235, 220, 255});
            } else if (Vector3DotProduct(sky.sunDir, camFwd) > 0.2f) {
                Vector2 sp = GetWorldToScreen(Vector3Add(cam.position, Vector3Scale(sky.sunDir, 1000)), cam);
                Color sc = toColor(Vector3Scale(sky.sunCol, 1.1f));
                DrawCircleV(sp, 60, {sc.r, sc.g, sc.b, (unsigned char)(heavyFog ? 20 : 50)});
                DrawCircleV(sp, 26, {255, 250, 230, (unsigned char)(heavyFog ? 60 : 255)});
            }
        }

        // far pass
        rlSetClipPlanes(250.0, 40000.0);
        BeginMode3D(cam);
        W.drawPass(cam, 250, 40000, mat);
        W.drawWater(mat);
        BeginShaderMode(sh);
        T.draw(cam.position, camFwd);
        GO.draw(W, cam.position);
        SP.draw(now, cam.position);
        T.drawTrains(W, cam.position);
        if (ride.on) Traffic::drawTrain(W, ride.s, ride.dir, cam.position);
        {
            Jet jets[2];
            airTraffic(now, jets);
            for (auto& j : jets)
                if (j.visible) drawCarModel(CT_PLANE, j.livery, j.pos, j.yaw, j.pitch, false);
            // AirTrain shuttling between the rental car center and the hotel, pausing at each station
            for (int k = 0; k < 2; k++) {
                float ph = fmodf(now + k * 38.f, 76.f);
                const float stops[4] = {1460, 1640, 2000, 2165};
                float x;
                int leg = (int)(ph / 19.f);
                float u = clampf((fmodf(ph, 19.f) - 5.f) / 14.f, 0, 1);
                u = u * u * (3 - 2 * u);
                const int order[4][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
                x = lerpf(stops[order[leg][0]], stops[order[leg][1]], u);
                drawCarModel(CT_AIRTRAIN, WHITE, {x, AIRPORT_Y + 10, 1785}, 0, 0, false);
            }
        }
        EndShaderMode();
        EndMode3D();
        rlDrawRenderBatchActive();
        glClear(GL_DEPTH_BUFFER_BIT);
        // near pass
        rlSetClipPlanes(0.15, 270.0);
        BeginMode3D(cam);
        W.drawPass(cam, 0, 270, mat);
        W.drawWater(mat);
        BeginShaderMode(sh);
        T.draw(cam.position, camFwd);
        GO.draw(W, cam.position);
        SP.draw(now, cam.position);
        T.drawTrains(W, cam.position);
        if (ride.on) Traffic::drawTrain(W, ride.s, ride.dir, cam.position);
        {
            Jet jets[2];
            airTraffic(now, jets);
            for (auto& j : jets)
                if (j.visible) drawCarModel(CT_PLANE, j.livery, j.pos, j.yaw, j.pitch, false);
            // AirTrain shuttling between the rental car center and the hotel, pausing at each station
            for (int k = 0; k < 2; k++) {
                float ph = fmodf(now + k * 38.f, 76.f);
                const float stops[4] = {1460, 1640, 2000, 2165};
                float x;
                int leg = (int)(ph / 19.f);
                float u = clampf((fmodf(ph, 19.f) - 5.f) / 14.f, 0, 1);
                u = u * u * (3 - 2 * u);
                const int order[4][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
                x = lerpf(stops[order[leg][0]], stops[order[leg][1]], u);
                drawCarModel(CT_AIRTRAIN, WHITE, {x, AIRPORT_Y + 10, 1785}, 0, 0, false);
            }
        }
        if (!title && !ride.on && !JS.riding && !SP.riding) P.draw();
        JS.draw(now, cam.position);
        for (auto& t : W.tokens) {
            if (t.got) continue;
            if (Vector3Distance(t.pos, cam.position) > 260) continue;
            rlPushMatrix();
            rlTranslatef(t.pos.x, t.pos.y + sinf(now * 2) * 0.25f, t.pos.z);
            rlRotatef(now * 120, 0, 1, 0);
            rlRotatef(90, 1, 0, 0);
            DrawCylinder({0, -0.12f, 0}, 0.8f, 0.8f, 0.24f, 16, {255, 200, 40, M_EMISSIVE});
            DrawCylinder({0, -0.14f, 0}, 0.55f, 0.55f, 0.28f, 16, {230, 150, 20, M_EMISSIVE});
            rlPopMatrix();
            DrawCube({t.pos.x, t.pos.y - 1.5f + 30, t.pos.z}, 0.15f, 60, 0.15f, {255, 220, 80, M_EMISSIVE}); // beacon
        }
        if (CH.on && !title && !ride.on) { // cruisers with flashing light bars, and the officers on foot
            for (auto& pc : CH.parked)
                if (Vector3Distance(pc.pos, cam.position) < 700) drawCruiser(pc.pos, pc.yaw, now);
            for (size_t i = 0; i < CH.cops.size(); i++) {
                const Cop& cp = CH.cops[i];
                if (Vector3Distance(cp.pos, cam.position) > 700) continue;
                if (cp.onFoot) drawCop(cp, now, CH.scuffle && (int)i == CH.scCop);
                else drawCruiser(cp.pos, cp.yaw, now);
            }
        }
        EndShaderMode();
        // soft blob shadows, drawn with the default shader so they blend
        rlDrawRenderBatchActive();
        rlDisableDepthMask();
        JS.drawFx(cam.position);
        SP.drawFx();
        if (!title && !ride.on && !JS.riding && !SP.riding && !(P.state == Player::BAIL && P.splashed)) {
            Vector3 sp = P.state == Player::BAIL ? P.boardPos : P.pos;
            float gy = W.ground(sp.x, sp.z, P.pos.y + 0.1f);
            float hgt = P.pos.y - gy;
            if (hgt < 12) DrawCylinder({sp.x, gy + 0.12f, sp.z}, 0.6f, 0.6f, 0.01f, 14, {0, 0, 0, (unsigned char)(55 * clampf(1 - hgt / 8, 0.25f, 1))});
        }
        if (CH.on) {
            for (auto& cp : CH.cops) DrawCylinder({cp.pos.x, cp.pos.y + 0.12f, cp.pos.z}, cp.onFoot ? 0.5f : 2.0f, cp.onFoot ? 0.5f : 2.0f, 0.01f, 14, {0, 0, 0, 70});
            for (auto& pc : CH.parked) DrawCylinder({pc.pos.x, pc.pos.y + 0.12f, pc.pos.z}, 2.0f, 2.0f, 0.01f, 14, {0, 0, 0, 70});
        }
        T.drawShadows(cam.position);
        rlDrawRenderBatchActive();
        rlEnableDepthMask();
        EndMode3D();

        // ---------------- HUD
        if (title) textCenter("SF SKATE", sw / 2, 30, 80, {245, 95, 60, 255});
        if (picker) {
            int n = (int)W.spots.size();
            int px = sw / 2 - 330, py = sh2 / 2 - 230;
            DrawRectangle(px, py, 660, 120 + n * 34, {10, 14, 22, 215});
            DrawRectangleLines(px, py, 660, 120 + n * 34, {255, 255, 255, 60});
            textShadow(title ? "WHERE DO YOU WANT TO START?" : "CHANGE LOCATION / TIME", px + 20, py + 18, 28, WHITE);
            textShadow("LANDMARK", px + 20, py + 60, 18, {255, 210, 60, 255});
            textShadow("TIME OF DAY", px + 430, py + 60, 18, {255, 210, 60, 255});
            for (int i = 0; i < n; i++) {
                int y = py + 90 + i * 34;
                bool sel = i == pickIdx;
                if (sel) DrawRectangle(px + 16, y - 2, 388, 32, {245, 95, 60, 220});
                DrawText(TextFormat("%d", (i + 1) % 10), px + 28, y + 4, 20, sel ? WHITE : Color{150, 160, 180, 255});
                DrawText(W.spots[i].name, px + 60, y + 4, 22, WHITE);
            }
            const Color TOD[3] = {{120, 180, 240, 255}, {240, 150, 80, 255}, {60, 70, 130, 255}};
            for (int t = 0; t < 3; t++) {
                int y = py + 120 + t * 50;
                bool sel = t == preset;
                DrawRectangle(px + 430, y, 210, 40, sel ? TOD[t] : Color{40, 45, 60, 255});
                if (sel) DrawRectangleLines(px + 430, y, 210, 40, WHITE);
                DrawText(PRESET_NAMES[t], px + 445, y + 10, 22, WHITE);
            }
            textShadow("GAME MODE  (B)", px + 430, py + 290, 18, {255, 210, 60, 255});
            const char* MODES[2] = {"Free roam", "Cop chase"};
            const Color MODE_C[2] = {{70, 170, 110, 255}, C_COP};
            for (int md = 0; md < 2; md++) {
                int y = py + 316 + md * 44;
                bool sel = (md == 1) == CH.on;
                DrawRectangle(px + 430, y, 210, 40, sel ? MODE_C[md] : Color{40, 45, 60, 255});
                if (sel) DrawRectangleLines(px + 430, y, 210, 40, WHITE);
                DrawText(MODES[md], px + 445, y + 10, 22, WHITE);
            }
            const char* hints[] = {"Up / Down   pick a landmark", "Left / Right  time of day", "1-0   jump to a landmark",
                                   "Click twice or ENTER  to go", title ? "Esc  quit" : "Tab  back to skating"};
            for (int k = 0; k < 5; k++) DrawText(hints[k], px + 430, py + 416 + k * 24, 17, {200, 205, 215, 255});
            float a = 0.6f + 0.4f * sinf((float)GetTime() * 4);
            textCenter(title ? "Press ENTER to drop in" : "Press ENTER to go there", sw / 2, py + 130 + n * 34, 28,
                       {255, 230, 120, (unsigned char)(255 * a)});
        }
        if (!title) {
            if (!ride.on) { // regular HUD (hidden while riding Caltrain)
            // score panel
            DrawRectangle(16, 16, 300, 108, {0, 0, 0, 120});
            textShadow(TextFormat("SCORE  %lld", P.score), 28, 26, 30, WHITE);
            textShadow(TextFormat("Tokens  %d / %d", tokensGot, (int)W.tokens.size()), 28, 64, 22, {255, 210, 60, 255});
            textShadow(TextFormat("Best combo  %d", P.bestCombo), 28, 92, 20, {200, 210, 230, 255});
            // speed + district
            const char* dist = W.district(P.pos.x, P.pos.z);
            if (strcmp(dist, lastDistrict) != 0) { lastDistrict = dist; districtT = 3.0f; }
            districtT -= dt;
            DrawRectangle(16, sh2 - 92, 330, 76, {0, 0, 0, 120});
            textShadow(TextFormat("%d mph", (int)(spd * 2.237f)), 28, sh2 - 86, 40, WHITE);
            textShadow(dist, 28, sh2 - 42, 22, {190, 220, 255, 255});
            textShadow(TextFormat("%s (T)", PRESET_NAMES[preset]), 200, sh2 - 80, 18, {220, 220, 220, 200});
            if (districtT > 0) {
                unsigned char al = (unsigned char)(255 * clampf(districtT, 0, 1));
                textCenter(dist, sw / 2, 40, 40, {255, 255, 255, al});
            }
            // current combo
            if (!P.combo.empty() && (P.state == Player::AIR || P.state == Player::GRIND)) {
                std::string s;
                for (size_t i = 0; i < P.combo.size(); i++) { if (i) s += " + "; s += P.combo[i]; }
                textCenter(s.c_str(), sw / 2, sh2 - 150, 26, {255, 255, 255, 255});
                textCenter(TextFormat("%d  x%d", (int)P.comboPts, (int)P.combo.size()), sw / 2, sh2 - 118, 30, {255, 210, 60, 255});
            }
            if (P.state == Player::GRIND) {
                int bx = sw / 2 - 120, by = sh2 - 190;
                DrawRectangle(bx, by, 240, 12, {0, 0, 0, 150});
                DrawRectangle(sw / 2 - 2, by - 3, 4, 18, WHITE);
                int mx = (int)(sw / 2 + P.balance * 115);
                Color bc = fabsf(P.balance) > 0.7f ? Color{255, 70, 60, 255} : Color{120, 230, 120, 255};
                DrawRectangle(mx - 6, by, 12, 12, bc);
                textCenter("A / D to balance", sw / 2, by - 24, 18, {230, 230, 230, 220});
            }
            if (P.popupT > 0) {
                float a = clampf(P.popupT, 0, 1);
                Color c = P.popupCol;
                c.a = (unsigned char)(255 * a);
                textCenter(P.popup.c_str(), sw / 2, sh2 / 2 - 140, 48, c);
                if (!P.subPopup.empty()) textCenter(P.subPopup.c_str(), sw / 2, sh2 / 2 - 86, 22, {255, 255, 255, c.a});
            }
            if (CH.active) {
                bool flip = (int)(now * 4) % 2;
                float k = 0.55f + 0.45f * sinf(now * 8);
                DrawRectangleGradientH(0, 0, 90, sh2, flip ? Color{255, 40, 40, (unsigned char)(90 * k)} : Color{40, 80, 255, (unsigned char)(90 * k)}, {0, 0, 0, 0});
                DrawRectangleGradientH(sw - 90, 0, 90, sh2, {0, 0, 0, 0}, flip ? Color{40, 80, 255, (unsigned char)(90 * k)} : Color{255, 40, 40, (unsigned char)(90 * k)});
            }
            drawMinimap(sw - 250, 20, 230);
            if (CH.scuffle) {
                int bw = 380, bx = sw / 2 - bw / 2, by = sh2 - 124;
                DrawRectangle(bx - 12, by - 56, bw + 24, 130, {0, 0, 0, 170});
                float jit = sinf(now * 45.f) * 2.f;
                textCenter("STRUGGLE!", sw / 2 + (int)jit, by - 48, 40, {255, 140, 70, 255});
                DrawRectangle(bx, by, bw, 24, {170, 40, 40, 255});
                DrawRectangle(bx, by, (int)(bw * clampf(CH.meter, 0, 1)), 24, {110, 230, 120, 255});
                DrawRectangleLines(bx, by, bw, 24, WHITE);
                textCenter("Mash SPACE and A / D   -   swing the board with J K L", sw / 2, by + 36, 20, WHITE);
            }
            if (CH.on) {
                int bw = 440, bx = sw / 2 - bw / 2, by = 92;
                DrawRectangle(bx, by, bw, 78, {0, 0, 0, 140});
                DrawRectangle(bx, by, 6, 78, CH.active ? C_COP : Color{110, 230, 120, 255});
                if (CH.active) {
                    for (int i = 0; i < 5; i++) drawStar(bx + 30 + i * 28, by + 24, 12, i < CH.stars ? Color{255, 210, 60, 255} : Color{255, 255, 255, 60});
                    textShadow(TextFormat("Nearest cop  %d m", (int)std::min(CH.nearest, 9999.f)), bx + 20, by + 48, 20, WHITE);
                    if (CH.bustT > 0) {
                        DrawRectangle(bx + 250, by + 14, 170, 12, {0, 0, 0, 150});
                        DrawRectangle(bx + 250, by + 14, (int)(170 * clampf(CH.bustT / Chase::BUST_TIME, 0, 1)), 12, C_COP);
                        textShadow("BUSTED", bx + 250, by + 30, 16, {255, 150, 140, 255});
                    } else if (CH.hideT > 0) {
                        DrawRectangle(bx + 250, by + 14, 170, 12, {0, 0, 0, 150});
                        DrawRectangle(bx + 250, by + 14, (int)(170 * clampf(CH.hideT / Chase::ESCAPE_TIME, 0, 1)), 12, {110, 230, 120, 255});
                        textShadow("LOSING THEM", bx + 250, by + 30, 16, {170, 240, 170, 255});
                    } else {
                        textShadow(TextFormat("Get %d m away", (int)Chase::ESCAPE_DIST), bx + 250, by + 14, 18, {200, 210, 230, 255});
                    }
                    // arrow toward the closest cop, relative to the camera
                    const Cop* nc = &CH.cops[0];
                    for (auto& cp : CH.cops) if (Vector3Distance(cp.pos, P.pos) < Vector3Distance(nc->pos, P.pos)) nc = &cp;
                    Vector2 f = Vector2Normalize({cam.target.x - cam.position.x, cam.target.z - cam.position.z});
                    Vector2 d = Vector2Normalize({nc->pos.x - P.pos.x, nc->pos.z - P.pos.z});
                    float ang = atan2f(-(d.x * f.x + d.y * f.y), d.x * -f.y + d.y * f.x); // screen direction: right, up = ahead
                    drawPlayerArrow({(float)bx + bw - 22, (float)by + 56}, ang, 14, C_COP);
                } else {
                    textShadow("LAYING LOW", bx + 20, by + 8, 24, {170, 240, 170, 255});
                    textShadow(TextFormat("Patrol in %d s    escapes %d    (%s, N)", std::max(0, (int)ceilf(CH.calmT)), CH.escapes, Chase::DIFF_NAMES[CH.difficulty]), bx + 20, by + 44, 20, {200, 210, 230, 255});
                }
            }
            if (showHelp) {
                int hx = sw - 330, hy = 270;
                DrawRectangle(hx, hy, 314, 458, {0, 0, 0, 140});
                const char* lines[] = {"CONTROLS", "W / Up      push", "S / Down    brake", "A D         steer / spin in air",
                                       "Shift       tuck (go faster)", "Space       ollie / jump off rail", "J  K  L     kickflip, heelflip, shove-it",
                                       "I (hold)    grab", "Land on rails & ledges to grind", "B  free roam / cop chase   N  difficulty", "Right mouse  look around", "Wheel       zoom    C  camera",
                                       "Tab  choose landmark / time", "M  map  1-0  teleport  E  train/jetski", "T or F1-F3  time of day",
                                       "F  fog   R  reset   P  pause", "Caught: mash SPACE A D / J K L", "H  hide this help"};
                for (int i = 0; i < 18; i++) DrawText(lines[i], hx + 14, hy + 12 + i * 24, i == 0 ? 22 : 18, i == 0 ? Color{255, 210, 60, 255} : WHITE);
            }
            }
            if (nearSki >= 0) {
                DrawRectangle(sw / 2 - 200, sh2 - 230, 400, 44, {20, 110, 170, 220});
                textCenter("Press E to ride the jet ski", sw / 2, sh2 - 218, 22, WHITE);
            }
            if (JS.riding) {
                DrawRectangle(sw / 2 - 270, sh2 - 230, 540, 44, {0, 0, 0, 130});
                textCenter("W go   S brake   A D steer   Shift boost   E hop off at the shore", sw / 2, sh2 - 218, 20, {200, 235, 255, 255});
            }
            if (nearPlane >= 0) {
                DrawRectangle(sw / 2 - 210, sh2 - 230, 420, 44, {200, 140, 30, 230});
                textCenter("Press E to fly the seaplane", sw / 2, sh2 - 218, 22, WHITE);
            }
            if (SP.riding) {
                DrawRectangle(sw / 2 - 330, sh2 - 230, 660, 44, {0, 0, 0, 130});
                textCenter("W / S throttle   A D steer   Space climb   Shift dive   E climb out at the shore", sw / 2, sh2 - 218, 20, {255, 235, 200, 255});
                int bx = 16, by = sh2 - 200;
                DrawRectangle(bx, by, 300, 96, {0, 0, 0, 130});
                textShadow(TextFormat("ALT  %d ft", (int)(SP.altitude(W) * 3.281f)), bx + 14, by + 10, 24, WHITE);
                textShadow("THROTTLE", bx + 14, by + 50, 18, {220, 220, 220, 255});
                DrawRectangle(bx + 120, by + 52, 160, 14, {255, 255, 255, 40});
                DrawRectangle(bx + 120, by + 52, (int)(160 * SP.throttle), 14, {255, 200, 80, 255});
                if (!SP.onWater && SP.speed < 19.f) textCenter("STALL", sw / 2, 200, 40, {255, 80, 60, 255});
            }
            if (nearStation >= 0 && !ride.on) {
                const char* to = nearStation == menloStation ? "San Francisco (4th & King)" : "Menlo Park";
                DrawRectangle(sw / 2 - 260, sh2 - 230, 520, 44, {200, 30, 40, 220});
                textCenter(TextFormat("Press E to ride Caltrain to %s", to), sw / 2, sh2 - 218, 22, WHITE);
            }
            if (ride.on) {
                static const struct { float z; const char* n; } towns[] = {
                    {1850, "San Francisco"}, {2400, "South San Francisco"}, {2950, "San Bruno / Millbrae"}, {4000, "Burlingame"},
                    {5600, "San Mateo"}, {7000, "Belmont / San Carlos"}, {8400, "Redwood City"}, {8900, "Atherton"}, {1e9f, "Menlo Park"}};
                const char* town = "";
                for (auto& t : towns) if (cam.target.z < t.z) { town = t.n; break; }
                DrawRectangle(0, 0, sw, 96, {150, 20, 30, 225});
                textShadow("CALTRAIN", 24, 14, 36, WHITE);
                textShadow(TextFormat("%s to %s", ride.dir > 0 ? "Southbound" : "Northbound", W.stations[ride.dest].name), 24, 56, 22, {255, 225, 200, 255});
                textCenter(town, sw / 2, 20, 34, WHITE);
                textShadow(TextFormat("%.1f km to go", fabsf(ride.target - ride.s) / 1000.f), sw - 300, 18, 30, WHITE);
                textShadow("Space: skip to arrival", sw - 300, 58, 18, {255, 225, 200, 255});
                // progress along the line with station ticks
                int bx = sw / 2 - 300, by = 64;
                DrawRectangle(bx, by, 600, 6, {255, 255, 255, 90});
                float L = W.railLen();
                for (auto& st : W.stations) DrawCircle(bx + (int)(600 * st.s / L), by + 3, 6, WHITE);
                DrawCircle(bx + (int)(600 * ride.s / L), by + 3, 8, {255, 210, 60, 255});
            }
            if (paused) textCenter("PAUSED", sw / 2, sh2 / 2 - 30, 60, WHITE);
            if (showMap) drawBigMap();
            DrawFPS(sw - 100, sh2 - 30);
        }
        EndDrawing();
    }
    if (autotest) TraceLog(LOG_INFO, "SFSKATE: average fps %.1f", atFpsN ? atFpsSum / atFpsN : 0.0);
    S.shutdown();
    CloseWindow();
    return 0;
}
