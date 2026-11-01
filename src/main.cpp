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
#include "render.h"
#include "weather.h"
#include "birds.h"
#include "instance.h"
#include <cstdio>
#include <chrono>
#include <thread>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>

// ---------------------------------------------------------------- frame pacing
// "Max" (the default): vsync alone paces frames. (macOS's OpenGL vsync lets up to two frames through per display
// refresh, so this can run faster than the display.) raylib's SetTargetFPS limiter stays off: its sleep runs long,
// so its "60 fps" frames took ~17.1 ms against the display's 16.7 ms and every few dozen frames one was skipped.
// "60" (Y key): our own wait to an absolute deadline. (A vsync swap interval of 2 would be neater, but macOS
// accepts it and ignores it.)
static bool gCap60 = true;    // the Mac's display is 60 Hz: drawing faster only adds heat (Y switches to Max)
static double gFramePeriod = 1 / 60.0;
static void updateFramePeriod() {   // a whole number of refreshes (1 at 60 Hz, 2 at 120 Hz), so frames land on refreshes
    int hz = GetMonitorRefreshRate(GetCurrentMonitor());
    if (hz <= 0) hz = 60;
    gFramePeriod = std::max(1L, lround(hz / 60.0)) / (double)hz;
}
static void waitForFrameSlot() {   // call once a frame, after EndDrawing
    static double next = 0;
    static int frames = 0;
    if (++frames % 120 == 0) updateFramePeriod();   // the window may have moved to another display
    double now = GetTime();
    next += gFramePeriod;
    // off, or this frame ran late: no wait, and the schedule restarts from now (so a stall isn't "caught up" with a
    // burst of fast frames, and a frame vsync held back resyncs us to the display)
    if (!gCap60 || now >= next) { next = now; return; }
    // sleep most of the way, then spin the last 2 ms, since the OS can oversleep a little
    if (next - now > 0.002) std::this_thread::sleep_for(std::chrono::duration<double>(next - now - 0.002));
    while (GetTime() < next) {}
}

// ---------------------------------------------------------------- lighting presets
struct Sky {
    Vector3 sunDir, sunCol, amb, top, horizon, fog;
    float night;
    Vector3 gain;             // color grade: tint, saturation, contrast, how much the lights bloom
    float sat, contrast, bloom;
};
static Sky PRESETS[3] = {
    // day: crisp and a little cool in the shade
    {Vector3Normalize({-0.35f, 0.8f, 0.45f}), {0.84f, 0.8f, 0.72f}, {0.38f, 0.42f, 0.5f},
     {0.32f, 0.55f, 0.88f}, {0.76f, 0.85f, 0.94f}, {0.76f, 0.84f, 0.92f}, 0.0f,
     {1.0f, 1.0f, 1.0f}, 1.04f, 1.05f, 0.35f},
    // golden hour: sun setting over the Pacific, warm and saturated
    {Vector3Normalize({-0.9f, 0.16f, 0.25f}), {1.32f, 0.84f, 0.5f}, {0.31f, 0.28f, 0.38f},
     {0.22f, 0.3f, 0.56f}, {0.98f, 0.66f, 0.44f}, {0.88f, 0.66f, 0.55f}, 0.35f,
     {1.05f, 0.99f, 0.92f}, 1.12f, 1.07f, 0.7f},
    // night: moonlit blue, lights bloom
    {Vector3Normalize({0.3f, 0.6f, -0.4f}), {0.22f, 0.26f, 0.4f}, {0.1f, 0.11f, 0.17f},
     {0.01f, 0.02f, 0.06f}, {0.07f, 0.09f, 0.16f}, {0.07f, 0.09f, 0.15f}, 1.0f,
     {0.96f, 0.99f, 1.06f}, 0.96f, 1.06f, 1.1f},
};
static const char* PRESET_NAMES[3] = {"Day", "Golden Hour", "Night"};

static Sky lerpSky(const Sky& a, const Sky& b, float t) {
    return {Vector3Normalize(Vector3Lerp(a.sunDir, b.sunDir, t)), Vector3Lerp(a.sunCol, b.sunCol, t), Vector3Lerp(a.amb, b.amb, t),
            Vector3Lerp(a.top, b.top, t), Vector3Lerp(a.horizon, b.horizon, t), Vector3Lerp(a.fog, b.fog, t), lerpf(a.night, b.night, t),
            Vector3Lerp(a.gain, b.gain, t), lerpf(a.sat, b.sat, t), lerpf(a.contrast, b.contrast, t), lerpf(a.bloom, b.bloom, t)};
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
static Renderer R;
static Weather WX;
static Birds BI;

static const Color C_COP = {255, 70, 60, 255};

// An officer on foot: running, arms out to grab, or flat on the ground after being knocked down.
static void drawCop(const Cop& c, float now, bool grabbing) {
    inst::push();
    inst::translate(c.pos.x, c.pos.y, c.pos.z);
    inst::rotate(-c.yaw * RAD2DEG, 0, 1, 0);
    bool down = c.stun > 0;
    if (down) {
        inst::translate(0, 0.15f, 0);
        inst::rotate(-85.f * clampf((3.2f - (c.stun > 3.2f ? 3.2f : c.stun)) / 3.2f * 6.f + 0.f, 0.f, 1.f), 0, 0, 1);
    }
    if (grabbing) inst::translate(0, 0, sinf(now * 40.f) * 0.03f);
    Color shirt = {28, 44, 96, M_PLAIN}, pants = {22, 26, 44, M_PLAIN}, skin = {224, 180, 142, M_PLAIN};
    float sw = (down || grabbing) ? 0.f : sinf(c.phase) * 0.7f;
    for (int side = -1; side <= 1; side += 2) {
        inst::push();
        inst::translate(0, 0.9f, side * 0.11f);
        inst::rotate(sw * side * RAD2DEG, 0, 0, 1);
        inst::cube({0, -0.44f, 0}, 0.16f, 0.88f, 0.16f, pants);
        inst::cube({0.05f, -0.86f, 0}, 0.26f, 0.08f, 0.14f, {20, 20, 20, M_PLAIN});
        inst::pop();
        inst::push();
        inst::translate(0, 1.43f, side * 0.27f);
        inst::rotate((grabbing ? -75.f : -sw * side * 0.9f * RAD2DEG), 0, 0, 1);
        inst::cube({0, -0.28f, 0}, 0.12f, 0.58f, 0.12f, shirt);
        inst::cube({0, -0.6f, 0}, 0.11f, 0.11f, 0.11f, skin);
        inst::pop();
    }
    inst::cube({0, 1.18f, 0}, 0.26f, 0.6f, 0.42f, shirt);
    inst::cube({0, 0.92f, 0}, 0.28f, 0.08f, 0.44f, {30, 30, 34, M_PLAIN});        // belt
    inst::cube({0.14f, 1.35f, -0.12f}, 0.04f, 0.12f, 0.1f, {240, 200, 60, M_PLAIN}); // badge
    inst::cube({0, 1.6f, 0}, 0.21f, 0.25f, 0.21f, skin);
    inst::cube({0, 1.76f, 0}, 0.25f, 0.1f, 0.25f, {20, 28, 60, M_PLAIN});          // cap
    inst::cube({0.14f, 1.71f, 0}, 0.1f, 0.03f, 0.24f, {16, 20, 40, M_PLAIN});     // brim
    inst::pop();
}

static void drawCruiser(Vector3 pos, float yaw, float now) {
    bool flip = (int)(now * 8) % 2;
    drawCarModel(CT_SEDAN, {235, 235, 240, 255}, pos, yaw, 0, false);
    inst::cube(Vector3Add(pos, rotY({-0.1f, 1.62f, 0}, yaw)), 0.35f, 0.18f, 1.2f, {25, 25, 30, M_PLAIN});
    Color red = flip ? Color{255, 40, 40, M_EMISSIVE} : Color{110, 10, 10, M_EMISSIVE};
    Color blue = flip ? Color{20, 40, 120, M_EMISSIVE} : Color{60, 110, 255, M_EMISSIVE};
    inst::cube(Vector3Add(pos, rotY({-0.1f, 1.78f, -0.38f}, yaw)), 0.3f, 0.16f, 0.42f, red);
    inst::cube(Vector3Add(pos, rotY({-0.1f, 1.78f, 0.38f}, yaw)), 0.3f, 0.16f, 0.42f, blue);
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

// test hook (SFSKATE_BAKEHASH=file): a fingerprint of the static world as it is on the GPU, read back chunk by
// chunk in draw order, plus the leaf sources, so a change to how the world is baked can be checked byte for byte
static void bakeHash(const char* path) {
    uint64_t h = 1469598103934665603ull, total = h;
    auto put = [&](const void* d, size_t n) { for (size_t i = 0; i < n; i++) { h ^= ((const unsigned char*)d)[i]; h *= 1099511628211ull; } };
    auto buf = [&](unsigned id) {
        GLint n = 0;
        glBindBuffer(GL_COPY_READ_BUFFER, id);
        glGetBufferParameteriv(GL_COPY_READ_BUFFER, GL_BUFFER_SIZE, &n);
        std::vector<unsigned char> d(n);
        if (n) glGetBufferSubData(GL_COPY_READ_BUFFER, 0, n, d.data());
        put(&n, sizeof(n));
        put(d.data(), d.size());
    };
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (size_t k = 0; k <= W.chunks.size(); k++) {
        const Chunk& ch = k < W.chunks.size() ? W.chunks[k] : W.water;
        h = 1469598103934665603ull;
        put(&ch.count, sizeof(int)); put(&ch.packed, 1); put(&ch.idx32, 1); put(&ch.org, sizeof(Vector4));
        put(&ch.mn, sizeof(Vector3)); put(&ch.mx, sizeof(Vector3)); put(&ch.layer, sizeof(int));
        put(&ch.c0, sizeof(Vector2)); put(&ch.c1, sizeof(Vector2)); put(&ch.fol, sizeof(int));
        buf(ch.vboA); buf(ch.vboB); buf(ch.ebo);
        fprintf(f, "%zu %016llx\n", k, (unsigned long long)h);
        total ^= h; total *= 1099511628211ull;
    }
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    h = 1469598103934665603ull;
    for (const LeafSrc& s : W.leafSrcs) {
        put(&s.c, sizeof(Vector3)); put(&s.rad, sizeof(Vector3)); put(&s.len, 4); put(&s.width, 4); put(&s.autumn, 4);
        put(&s.base, 4); put(&s.rng, 4); put(&s.count, 2); put(&s.withCore, 1);
    }
    for (const FoliagePiece& p : W.folPieces) { put(&p.cell, 4); put(&p.s0, 4); put(&p.s1, 4); put(&p.mn, 12); put(&p.mx, 12); put(&p.verts, 8); }
    for (const FoliageCell& c : W.folCells) { put(&c.key, 4); put(&c.c0, 8); put(&c.c1, 8); put(&c.p0, 4); put(&c.np, 4); }
    fprintf(f, "leaves %016llx\n", (unsigned long long)h);
    total ^= h; total *= 1099511628211ull;
    fprintf(f, "total %016llx (%zu chunks)\n", (unsigned long long)total, W.chunks.size());
    fclose(f);
    TraceLog(LOG_INFO, "SFSKATE: bake fingerprint %016llx (%zu chunks)", (unsigned long long)total, W.chunks.size());
}

static void textShadow(const char* t, int x, int y, int size, Color c) {
    DrawText(t, x + 2, y + 2, size, {0, 0, 0, (unsigned char)(c.a * 0.6f)});
    DrawText(t, x, y, size, c);
}
static void textCenter(const char* t, int cx, int y, int size, Color c) { textShadow(t, cx - MeasureText(t, size) / 2, y, size, c); }
// centred 44 px prompt bar sized from the measured text (20 px padding each side); in narrow windows the
// font shrinks until the bar fits with an 8 px margin, down to size 10
static void promptBar(const char* t, int sw, int y, int size, Color bg, Color fg) {
    while (size > 10 && MeasureText(t, size) + 40 > sw - 16) size--;
    int w = MeasureText(t, size) + 40;
    DrawRectangle(sw / 2 - w / 2, y, w, 44, bg);
    textCenter(t, sw / 2, y + 22 - size / 2, size, fg);
}


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
        if (i >= 10) { DrawCircleV(p, 3, WHITE); continue; } // only 1..9, 0 have a key; the rest get a plain dot
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
    // no MSAA on the window itself: the 3D scene is drawn multisampled into its own target (see render.cpp)
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(1600, 900, "SF Skate");
    SetWindowFocused();   // take the keyboard from the terminal that launched us (see the first-frame note below)
    SetTargetFPS(0);   // no raylib frame limiter: see "frame pacing" above
    updateFramePeriod();
    S.init();

    loadingScreen("Starting up", 0);
    double genT0 = GetTime();
    W.earlyBake = true;   // bake and upload the Peninsula while the city is generated (the GL context is up)
    W.generate(loadingScreen);
    TraceLog(LOG_INFO, "SFSKATE: generated in %.2fs", GetTime() - genT0);
    W.upload([] {   // shaders and render targets don't need the world: set them up while its meshes bake
        if (getenv("SFSKATE_QUALITY")) R.quality = clampf((float)atoi(getenv("SFSKATE_QUALITY")), 0, 2); // test hook
        double rT0 = GetTime();
        R.init();
        inst::init();
        TraceLog(LOG_INFO, "SFSKATE: renderer init in %.3fs", GetTime() - rT0);
        WX.init();
    });
    if (getenv("SFSKATE_BAKEHASH")) bakeHash(getenv("SFSKATE_BAKEHASH")); // test hook
    // raylib's default render batch has a single vertex buffer, so the HUD's first draw after the 3D scene waited
    // ~11 ms a frame for the GPU to finish reading it. With 8 buffers in rotation the CPU fills one while the GPU
    // still reads the others. (Made after inst::init, whose mesh capture switches batches; see inst::setFrameBatch.)
    rlRenderBatch frameBatch = rlLoadRenderBatch(8, 8192);   // 8192 quads per buffer, as in raylib's default
    rlSetRenderBatchActive(&frameBatch);
    inst::setFrameBatch(&frameBatch);
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

    BI.init(W);
    if (getenv("SFSKATE_RAIN")) WX.raining = true, WX.rain = WX.wet = 1; // test hook: start in the rain
    const Shader& sh = R.city;
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
    bool skySnap = true;
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
    float titleT = 0, sirenT = 0, landKick = 0, swingT = 0, squeezeYaw = 0;
    float airT = 0;   // clock for the SFO jets and the AirTrain: stands still while paused, on the map or in the picker
    if (getenv("SFSKATE_NIGHT")) preset = 2; // test hook: start at night
    if (getenv("SFSKATE_TOD")) preset = atoi(getenv("SFSKATE_TOD")) % 3; // test hook: 0 day, 1 golden hour, 2 night
    if (getenv("SFSKATE_COPS")) CH.calmT = 0.5f; // test hook: cops show up right away
    if (getenv("SFSKATE_FOG")) heavyFog = true, fogDen = 0.0022f; // test hook: start in Karl the Fog
    if (getenv("SFSKATE_NOHELP")) showHelp = false; // test hook: no controls panel in the screenshots
    const int atMaxSpots = getenv("SFSKATE_SHOTS") ? atoi(getenv("SFSKATE_SHOTS")) : 1000; // test hook: stop after this many spots
    double atMs = 0; int atMsN = 0;   // frame time while settled at a spot
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
                if (atSpot >= (int)W.spots.size() + 2 || atSpot >= atMaxSpots) break;
                if (atSpot < (int)W.spots.size()) {
                    P.spawn(W.spots[atSpot].pos, W.spots[atSpot].yaw);
                    camYaw = P.yaw;
                    camPos = {P.pos.x - cosf(P.yaw) * 6, P.pos.y + 3, P.pos.z - sinf(P.yaw) * 6};
                } else if (atSpot == (int)W.spots.size()) { preset = 2; P.spawn(W.spots[2].pos, W.spots[2].yaw); camYaw = P.yaw; }
                else { preset = 1; P.spawn(W.spots[1].pos, W.spots[1].yaw); camYaw = P.yaw; }
            }
            if (atFrame > 61 && (atFrame - 62) % 150 == 140) {
                TakeScreenshot(TextFormat("shot_%02d.png", atSpot));
                TraceLog(LOG_INFO, "SFSKATE: spot %d fps=%d frame=%.2f ms pos=%.1f,%.1f,%.1f state=%d", atSpot, GetFPS(), atMsN ? atMs / atMsN : 0.0, P.pos.x, P.pos.y, P.pos.z, (int)P.state);
                atMs = 0; atMsN = 0;
            }
            if (atFrame > 100) { atFpsSum += GetFPS(); atFpsN++; }
            if (atFrame > 61 && (atFrame - 62) % 150 >= 40) { atMs += GetFrameTime() * 1000.0; atMsN++; }
        }
        // ---------------- input
        bool justStarted = false;
        auto setCops = [&](bool on) { // game mode: free roam (no police) or cop chase
            if (CH.on == on) return;
            CH.on = on;
            CH.reset(8);
        };
        // get off whatever is being ridden before a teleport: the train ride ends, the jet ski stays moored
        // where it was (it follows the rider while ridden) and the seaplane is back at its mooring
        auto leaveVehicles = [&]() {
            ride.on = false;
            if (JS.riding) { JS.riding = false; JS.cur = -1; JS.speed = 0; }
            if (SP.riding) { SP.riding = false; SP.cur = -1; }
        };
        auto startAt = [&](int idx) {
            justStarted = true;
            leaveVehicles();
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
        if (IsKeyPressed(KEY_G) && !title) {
            WX.raining = !WX.raining;
            P.say(WX.raining ? "Rain" : "Clearing up", {190, 210, 235, 255}, WX.raining ? "Streets get wet and shiny (G)" : "The streets will dry out");
        }
        if (IsKeyPressed(KEY_V) && !title) {
            R.setQuality((R.quality + 2) % 3);
            P.say(TextFormat("Graphics: %s", Renderer::QUALITY_NAMES[R.quality]), WHITE,
                  R.quality == 2 ? "Sharp + distant shadows, soft occlusion, HDR, bloom" : R.quality == 1 ? "Softer shadows, bloom" : "No sun shadows or bloom (fastest)");
        }
        if (IsKeyPressed(KEY_Y) && !title) {
            gCap60 = !gCap60;
            updateFramePeriod();
            P.say(gCap60 ? "Frame rate: 60" : "Frame rate: Max", WHITE, gCap60 ? "Capped at 60 fps" : "Matches the display's refresh rate");
        }
        if (IsKeyPressed(KEY_C)) camMode = (camMode + 1) % 3;
        for (int k = 0; k < 10 && !title && !picker; k++) {
            if (IsKeyPressed(KEY_ONE + k) || (k == 9 && IsKeyPressed(KEY_ZERO))) {
                int idx = k;
                if (k == 9 && !IsKeyPressed(KEY_ZERO)) continue;
                if (idx < (int)W.spots.size()) {
                    leaveVehicles();
                    P.spawn(W.spots[idx].pos, W.spots[idx].yaw);
                    camYaw = P.yaw;
                    camPos = {P.pos.x - cosf(P.yaw) * 6, P.pos.y + 3, P.pos.z - sinf(P.yaw) * 6};
                    P.say(W.spots[idx].name, {200, 230, 255, 255}, "Teleported");
                    showMap = false;
                }
            }
        }
        if (!title && !picker && IsKeyPressed(KEY_R)) { if (JS.riding) { JS.skis[JS.cur].pos = P.pos; JS.riding = false; JS.cur = -1; } if (SP.riding) { SP.riding = false; SP.cur = -1; } P.respawnSafe(W); P.say("Reset", WHITE); }

        Input in;
        in.steer = (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT) ? 1.f : 0.f) - (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT) ? 1.f : 0.f);
        in.push = IsKeyDown(KEY_W) || IsKeyDown(KEY_UP);
        in.brake = IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN);
        in.tuck = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        if (autotest && getenv("SFSKATE_PUSH")) in.push = true;   // test hook: keep pushing (e.g. up a steep hill)
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
        // wrapped at 75 * 76 s, a whole number of jet (75 s) and AirTrain (76 s) loops, so nothing jumps at the wrap
        if (!paused && !showMap && (!picker || title)) airT = fmodf(airT + dt, 75.f * 76.f);
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
            running = false;
        }
        if (SP.riding && !paused && !picker && !showMap && !title) {
            if (IsKeyPressed(KEY_E) && !justMounted) {
                if (SP.dismount(P, W)) { camYaw = P.yaw; P.say("Back on land", WHITE); }
                else P.say(SP.refusalMsg(), WHITE);
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
                float yaw;
                Vector3 p = W.caltrainDropoff(ride.dest, &yaw);
                P.spawn(p, yaw);
                camYaw = yaw;
                camPos = {p.x - cosf(yaw) * 6, p.y + 3, p.z - sinf(yaw) * 6};
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
                airTraffic(airT, jets);
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
        // the sound of where you are: traffic downtown, surf and sea lions on the waterfront, wind on the bridges and
        // hilltops, music from a pedal bus rolling by, rain
        {
            Vector3 ear = ride.on ? cam.target : P.pos;
            static float waterK = 0, waterT = 0, treeK = 0;
            if ((waterT -= dt) <= 0) {   // how much open water and how many trees are around (sampled a few times a second)
                waterT = 0.25f;
                treeK = smooth01(0.5f, 8.f, W.treesNear(ear.x, ear.z));
                float wsum = 0;
                for (int k = 0; k < 12; k++) {
                    float a = k * TAU_F / 12, r = (k % 3 + 1) * 28.f;
                    if (W.isWater(ear.x + cosf(a) * r, ear.z + sinf(a) * r, 0.3f)) wsum += (k % 3 == 0 ? 1.6f : k % 3 == 1 ? 1.f : 0.6f);
                }
                waterK = clampf(wsum / 7.f, 0, 1);
            }
            float traffic = 0, music = 0, talk = 0, bellD = 1e9f;
            for (auto& c : T.cars) {
                float d2 = sq(c.pos.x - ear.x) + sq(c.pos.z - ear.z);
                if (d2 > 160 * 160) continue;
                float d = sqrtf(d2);
                if (c.type == CT_PEDALBUS) {   // a party on wheels: music and a crowd of riders talking over it
                    music = std::max(music, 1.f - smooth01(10.f, 75.f, d));
                    talk += 2.5f * (1.f - smooth01(5.f, 45.f, d));
                }
                if (c.type == CT_CABLECAR || c.type == CT_STREETCAR || c.type == CT_LRV) talk += 1.2f * (1.f - smooth01(4.f, 28.f, d));
                if (c.type == CT_CABLECAR) bellD = std::min(bellD, d);
                traffic += (0.3f + c.speed / 15.f) / (1.f + d2 / 900.f);
            }
            // people on the sidewalks: those standing around chatting or on the phone talk the most
            for (auto& p : T.peds) {
                float d2 = sq(p.pos.x - ear.x) + sq(p.pos.z - ear.z);
                if (d2 > 30 * 30 || fabsf(p.pos.y - ear.y) > 12) continue;
                float k = 1.f - sqrtf(d2) / 30.f;
                float chatty = p.carry == CARRY_LAPTOP ? 0.1f : p.mode == 3 ? 0.3f : p.mode != 2 ? 0.35f : p.idle == IDLE_CHAT ? 1.6f : p.idle == IDLE_CALL ? 1.f : 0.5f;
                talk += k * k * chatty;
            }
            if (ride.on) talk += 0.8f;   // other passengers on the train
            float above = ear.y - W.terrain(ear.x, ear.z);
            float high = std::max(W.onBridge(ear.x, ear.z) ? 0.75f : 0.f, smooth01(70.f, 200.f, ear.y));
            if (SP.riding) high = std::max(high, smooth01(5.f, 60.f, above) * 0.9f);
            float seal = 1.f - smooth01(40.f, 260.f, Vector2Distance({ear.x, ear.z}, {975, -1255}));
            bool inside = W.houseAt(ear.x, ear.z, ear.y) >= 0;
            float muffle = inside ? 0.35f : 1.f;
            float quiet = title ? 0.f : 1.f;
            float dark = smooth01(0.3f, 0.8f, sky.night), dry = 1.f - smooth01(0.1f, 0.6f, WX.rain);
            Sfx::Ambience am;
            am.traffic = clampf(traffic / 3.5f, 0, 0.8f) * muffle * quiet;
            am.waves = waterK * 0.9f * muffle * quiet;
            am.sealions = seal * quiet;
            am.windHigh = high * 0.8f * quiet;
            am.music = music * 0.8f * muffle * quiet;
            am.rain = WX.rain * (inside ? 0.4f : 0.85f) * quiet;
            am.chatter = clampf(talk / 2.5f, 0, 0.9f) * muffle * quiet;
            am.voices = clampf(talk / 5.f, 0, 1);
            am.leaves = treeK * (0.55f + 0.45f * std::max(high, WX.rain)) * muffle * quiet;
            am.birds = treeK * (1.f - dark) * dry * 0.8f * muffle * quiet;
            am.crickets = treeK * dark * dry * 0.7f * muffle * quiet;
            S.setAmbience(am);
            // now and then a gull cries somewhere overhead
            static float gullT = 3;
            if ((gullT -= dt) <= 0) {
                gullT = 2.5f + GetRandomValue(0, 60) / 10.f;
                for (auto& b : BI.birds)
                    if (b.gull && Vector3Distance(b.pos, ear) < 90) {
                        S.play(S.gull, clampf(1.1f - Vector3Distance(b.pos, ear) / 90.f, 0.15f, 0.7f) * quiet, 0.9f + GetRandomValue(0, 25) / 100.f);
                        break;
                    }
            }
            // pigeons pecking around a plaza coo now and then
            static float cooT = 2;
            if ((cooT -= dt) <= 0) {
                cooT = 1.5f + GetRandomValue(0, 40) / 10.f;
                for (auto& b : BI.birds)
                    if (!b.gull && b.state == 0 && Vector3Distance(b.pos, ear) < 25) {
                        S.play(S.coo, clampf(1.f - Vector3Distance(b.pos, ear) / 25.f, 0.1f, 0.6f) * muffle * quiet, 0.92f + GetRandomValue(0, 16) / 100.f);
                        break;
                    }
            }
            // a cable car coming up the hill rings its bell
            static float bellT = 3;
            if ((bellT -= dt) <= 0) {
                bellT = 4.f + GetRandomValue(0, 50) / 10.f;
                if (bellD < 80) S.play(S.bell, clampf(1.f - bellD / 80.f, 0.1f, 0.7f) * muffle * quiet, 0.97f + GetRandomValue(0, 6) / 100.f);
            }
        }

        // sky & fog
        if (!paused) GO.update(dt, W);
        sky = lerpSky(sky, PRESETS[preset], skySnap ? 1.f : 1 - expf(-dt * 2.0f));
        skySnap = false;
        if (!paused) {
            WX.update(dt);
            BI.update(dt, W, P.pos, running ? spd : 0.f);
            if (BI.evScatter) S.play(S.flutter, 0.8f, 1.f + GetRandomValue(-10, 10) / 100.f);
        }
        float fogTarget = heavyFog ? 0.0022f : (preset == 2 ? 0.00009f : 0.00011f);
        fogDen = lerpf(fogDen, fogTarget, 1 - expf(-dt * 1.5f));
        Vector3 fogC = sky.fog;
        if (heavyFog) fogC = Vector3Lerp(fogC, Vector3Scale({0.78f, 0.8f, 0.83f}, 1.0f - sky.night * 0.8f), 0.7f);
        Vector3 horizonC = heavyFog ? fogC : sky.horizon;
        // rain: a low grey overcast, weak sun, thicker haze, duller colors
        Sky lit = sky;
        float rainK = WX.rain;
        if (rainK > 0.001f) {
            Vector3 grey = Vector3Scale({0.56f, 0.6f, 0.65f}, 1.f - sky.night * 0.82f);
            fogC = Vector3Lerp(fogC, grey, 0.65f * rainK);
            horizonC = Vector3Lerp(horizonC, grey, 0.7f * rainK);
            lit.top = Vector3Lerp(sky.top, Vector3Scale(grey, 0.8f), 0.75f * rainK);
            lit.sunCol = Vector3Scale(sky.sunCol, 1.f - 0.7f * rainK);
            lit.amb = Vector3Scale(sky.amb, 1.f + 0.12f * rainK);
            lit.sat = sky.sat * (1.f - 0.18f * rainK);
            lit.gain = Vector3Lerp(sky.gain, {0.95f, 0.98f, 1.03f}, rainK);
        }
        float fogUse = fogDen * (1.f + 2.2f * rainK);

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
                    if (W.solidAt(Vector3Add(target, Vector3Scale(d, i / 14.f)), true)) return (i - 1) / 14.f;
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
                if (W.solidAt(Vector3Add(target, Vector3Scale(dir, t)), true)) { best = std::max(0.12f, (i - 1) / 14.f); break; }
            }
            desired = Vector3Add(target, Vector3Scale(dir, best));
            if (best < 0.6f) {
                // squeezed in a small room: rise toward the ceiling and look down instead of sitting inside the skater
                float cl = W.ceiling(desired.x, desired.z, P.pos.y);
                float lift = std::min(1.7f, 0.9f + (0.6f - best) * 1.6f);
                desired.y = std::max(desired.y, std::min(P.pos.y + lift, cl - 0.25f));
            }
            // keep the hill out of the shot: lift the camera until its sightline to the skater clears the ground
            // (climbing a slope that steepens, the hill otherwise rises between the two and swallows the skater)
            for (int i = 1; i <= 16; i++) {
                float t = i / 16.f;
                float gx = lerpf(target.x, desired.x, t), gz = lerpf(target.z, desired.z, t);
                float need = W.terrain(gx, gz) + 0.5f;
                float lineY = lerpf(target.y, desired.y, t);
                if (lineY < need) desired.y = target.y + (need - target.y) / t;
            }
            float gy = W.terrain(desired.x, desired.z) + 0.6f;
            if (desired.y < gy) desired.y = gy;
            camPos = Vector3Lerp(camPos, desired, 1 - expf(-dt * 12));
            if (best < 0.99f) camPos = Vector3Lerp(camPos, desired, 1 - perFrame60(0.5f, dt));   // a wall is close: catch up faster
            // the smoothing lags behind a rising hill: never let the camera itself end up inside it
            camPos.y = std::max(camPos.y, W.terrain(camPos.x, camPos.z) + 0.6f);
            // a hard landing dips the camera and punches the field of view, then it settles
            if (P.landShake > 0) { landKick = std::max(landKick, P.landShake); P.landShake = 0; }
            landKick *= expf(-dt * 6.5f);
            cam.position = camPos;
            cam.position.y = std::max(cam.position.y - landKick * 0.45f, W.terrain(camPos.x, camPos.z) + 0.4f);
            cam.target = target;
            cam.fovy = lerpf(cam.fovy, 62 + clampf(spd, 0, 30) * 0.6f, 1 - expf(-dt * 3)) + landKick * 5.f;
        }
        if (autotest && getenv("SFSKATE_CAM")) { // test hook: free camera "x,y,z,tx,ty,tz"
            float v[6];
            if (sscanf(getenv("SFSKATE_CAM"), "%f,%f,%f,%f,%f,%f", v, v + 1, v + 2, v + 3, v + 4, v + 5) == 6) { cam.position = {v[0], v[1], v[2]}; cam.target = {v[3], v[4], v[5]}; }
        }
        if (autotest && getenv("SFSKATE_SIDE")) { // test hook: watch the skater side-on, close up (e.g. how the board sits on a slope)
            cam.position = {P.pos.x - sinf(P.yaw) * 3.2f, P.pos.y + 0.9f, P.pos.z + cosf(P.yaw) * 3.2f};
            cam.target = {P.pos.x, P.pos.y + 0.7f, P.pos.z};
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
        if (autotest && getenv("SFSKATE_DODGE")) { // test hook: a walker near the skater hops ("h"), dives ("d") or backs off ("b"); shots along the way
            static int di = -1;
            static Vector3 camAt{};
            static Vector2 away{};
            char kc = getenv("SFSKATE_DODGE")[0];
            if (atFrame == 100) {
                float bd = 1e9f;
                for (int i = 0; i < (int)T.peds.size(); i++) {
                    const Ped& p = T.peds[i];
                    float d = Vector3Distance(p.pos, P.pos);
                    if (p.mode == 0 && p.knock <= 0 && d < bd) { bd = d; di = i; }
                }
                if (di >= 0) {
                    Ped& p = T.peds[di];
                    away = {-sinf(p.yaw), cosf(p.yaw)};   // off to their left
                    Traffic::startEvade(p, kc == 'd' ? EV_DIVE : kc == 'b' ? EV_BACK : EV_HOP, away, kc == 'd' ? 2.6f : 1.8f);
                    camAt = p.pos;
                }
            }
            if (di >= 0 && atFrame >= 100) {
                Vector2 fw = {cosf(atan2f(away.y, away.x) - PI / 2), sinf(atan2f(away.y, away.x) - PI / 2)};
                cam.position = {camAt.x + away.x * 1.3f + fw.x * 5.5f, camAt.y + 1.6f, camAt.z + away.y * 1.3f + fw.y * 5.5f};
                cam.target = {camAt.x + away.x * 1.3f, camAt.y + 0.7f, camAt.z + away.y * 1.3f};
                cam.fovy = 55;
                int f = atFrame - 100;
                if (f == 8 || f == 18 || f == 30 || f == 70 || f == 140 || f == 165) TakeScreenshot(TextFormat("dodge_%c_%03d.png", kc, f));
            }
        }
        if (autotest && getenv("SFSKATE_SIT")) { // test hook: someone on a laptop or sitting; k%3: 0 bench, 1 lawn, 2 walking with a laptop
            int k = atoi(getenv("SFSKATE_SIT")), seen = 0;
            for (auto& p : T.peds) {
                bool ok = k % 3 == 0 ? p.mode == 3 && !p.grass : k % 3 == 1 ? p.mode == 3 && p.grass : p.mode == 0 && p.carry == CARRY_LAPTOP;
                if (!ok || seen++ < k / 3) continue;
                cam.position = {p.pos.x + cosf(p.yaw + 0.6f) * 2.6f, p.pos.y + 1.6f, p.pos.z + sinf(p.yaw + 0.6f) * 2.6f};
                cam.target = {p.pos.x, p.pos.y + (p.mode == 3 ? 0.4f : 1.0f), p.pos.z};
                break;
            }
        }
        if (autotest && getenv("SFSKATE_PED")) { // test hook: look at a pedestrian up close, "k" picks which (odd: one standing still)
            int k = atoi(getenv("SFSKATE_PED")), seen = 0;
            for (auto& p : T.peds) {
                if (p.mode != (k % 2 ? 2 : 0) || p.pos.x < -1300 || p.pos.x > 1250 || p.pos.z < -1000 || p.pos.z > 1000) continue;
                if (seen++ < k / 2) continue;
                cam.position = {p.pos.x + cosf(p.yaw + 0.5f) * 3.2f, p.pos.y + 1.7f, p.pos.z + sinf(p.yaw + 0.5f) * 3.2f};
                cam.target = {p.pos.x, p.pos.y + 1.1f, p.pos.z};
                break;
            }
        }
        if (autotest && getenv("SFSKATE_STEAM") && !W.steamVents.empty()) { // test hook: look at the steaming manhole nearest the skater
            Vector3 v = W.steamVents[0];
            for (auto& q : W.steamVents) if (Vector3Distance(q, P.pos) < Vector3Distance(v, P.pos)) v = q;
            cam.position = {v.x + 7.f, v.y + 2.5f, v.z + 5.f};
            cam.target = {v.x, v.y + 1.5f, v.z};
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
        T.view = cam.position;   // people near the camera get the full update next frame, even far from the skater
        gMapFocus = ride.on ? cam.target : P.pos;

        // ---------------- shader uniforms
        Look look{cam.position, sky.sunDir, lit.sunCol, lit.amb, fogC, horizonC, fogUse, sky.night, now};
        look.fogH = heavyFog ? 70.f : 300.f - 120.f * rainK;
        look.wet = WX.wet;
        R.setLook(look);
        // street lamps: the 16 nearest the camera light the street at night. The furthest of them fade out, so one
        // swapping in or out of the set doesn't pop.
        {
            float lampK = smooth01(0.4f, 0.9f, sky.night);
            static std::vector<std::pair<float, int>> near;
            near.clear();
            if (lampK > 0.01f && !title)
                for (int i = 0; i < (int)W.lamps.size(); i++) {
                    const Vector3& l = W.lamps[i];
                    float d2 = sq(l.x - cam.position.x) + sq(l.z - cam.position.z);
                    if (d2 < 160.f * 160.f && fabsf(l.y - cam.position.y) < 60.f) near.push_back({d2, i});
                }
            int n = std::min((int)near.size(), Renderer::MAXLAMPS + 1);
            std::partial_sort(near.begin(), near.begin() + n, near.end());
            float edge = n > Renderer::MAXLAMPS ? sqrtf(near[Renderer::MAXLAMPS].first) : 160.f;
            Vector4 lamps[Renderer::MAXLAMPS];
            int m = std::min(n, Renderer::MAXLAMPS);
            for (int k = 0; k < m; k++) {
                const Vector3& l = W.lamps[near[k].second];
                lamps[k] = {l.x, l.y, l.z, 2.f * lampK * (1.f - smooth01(edge * 0.75f, edge, sqrtf(near[k].first)))};
            }
            R.setLamps(lamps, m);
        }

        // house rooms near the camera: a millisecond or two of meshing a frame, nearest first
        {
            InteriorStats is = W.updateInteriors(cam.position, 1.5e-3);
            static int built = 0, frames = 0, sinceTp = -1;
            static double sum = 0, mx = 0;
            if (is.built) { built += is.built; frames++; sum += is.ms; mx = std::max(mx, is.ms); }
            if (autotest) {
                if (atFrame > 61 && (atFrame - 62) % 150 == 0) sinceTp = 0;   // just teleported
                if (sinceTp >= 0 && is.missingNear == 0) {
                    TraceLog(LOG_INFO, "SFSKATE: rooms within 30 m built %d frames after the teleport", sinceTp);
                    sinceTp = -1;
                } else if (sinceTp >= 0) sinceTp++;
                if (atFrame > 61 && (atFrame - 62) % 150 == 140)
                    TraceLog(LOG_INFO, "SFSKATE: rooms cached %d, built %d in %d frames: %.2f ms a frame on average, %.2f max",
                             is.cached, built, frames, frames ? sum / frames : 0.0, mx);
            }
        }

        // tree leaves near the camera and the skater: meshed on worker threads, a millisecond or so a frame here
        {
            FoliageStats fs = W.updateFoliage(cam.position, P.pos, 1.5e-3);
            static int sinceTp = -1, frames = 0, uploaded = 0;
            static double worstMs = 0, worstBake = 0, sumMs = 0;
            if (fs.uploaded) { frames++; uploaded += fs.uploaded; sumMs += fs.ms; }
            worstMs = std::max(worstMs, fs.ms);
            worstBake = std::max(worstBake, fs.worstBake);
            static int fr = 0, bareFrames = 0;
            fr++;
            bareFrames += fs.cellsReady < fs.cellsNear;
            if (autotest && fr % 600 == 0)
                TraceLog(LOG_INFO, "SFSKATE: leaves after %d frames: %d pieces (%.0f MB) on the GPU, worst %.2f ms a frame here, worst "
                         "worker bake %.1f ms, %d frames with a cell within %.0f m still on canopy blobs (%d of %d now)", fr, fs.loaded,
                         fs.gpuMB, worstMs, worstBake, bareFrames, DETAIL_DIST, fs.cellsNear - fs.cellsReady, fs.cellsNear);
            if (autotest) {
                if (atFrame > 61 && (atFrame - 62) % 150 == 0) sinceTp = 0;   // just teleported
                if (sinceTp >= 0 && fs.cellsReady == fs.cellsNear) {
                    TraceLog(LOG_INFO, "SFSKATE: leaves within %.0f m all in %d frames after the teleport", DETAIL_DIST, sinceTp);
                    sinceTp = -1;
                } else if (sinceTp >= 0) sinceTp++;
                if (atFrame > 61 && (atFrame - 62) % 150 == 140)
                    TraceLog(LOG_INFO, "SFSKATE: leaves: %d pieces on the GPU (%.0f MB), %d uploaded in %d frames (%.2f ms a frame on average, "
                             "%.2f max here), worst worker bake %.1f ms", fs.loaded, fs.gpuMB, uploaded, frames, frames ? sumMs / frames : 0.0, worstMs, worstBake);
            }
        }

        // ---------------- draw
        BeginDrawing();
        // sun shadows: the city, traffic and people near the camera, into one or two cascades
        {
            float strength = title ? 0.f : (heavyFog ? 0.45f : 1.f) * (1.f - 0.75f * rainK);
            int nc = R.planShadows(cam.position, camFwd, sky.sunDir, strength);
            for (int k = 0; k < nc; k++) {
                R.beginCascade(k);
                W.drawCasters(R.lightView[k], R.half[k], 2600.f, R.depth);
                BeginShaderMode(R.depth);
                Vector3 c = Vector3Add(cam.position, Vector3Scale(Vector3Normalize({camFwd.x, 0, camFwd.z}), k == 0 ? 38.f : 150.f));
                T.drawCasters(c, k == 0 ? 75.f : 230.f, k == 0 ? 60.f : 0.f);
                T.drawTrains(W, cam.position);
                GO.draw(W, cam.position);
                if (k == 0) {
                    if (!ride.on && !JS.riding && !SP.riding) P.draw();
                    JS.draw(now, cam.position);
                    if (CH.on && !ride.on)
                        for (size_t i = 0; i < CH.cops.size(); i++) {
                            const Cop& cp = CH.cops[i];
                            if (Vector3Distance(cp.pos, cam.position) > 120) continue;
                            if (cp.onFoot) drawCop(cp, now, CH.scuffle && (int)i == CH.scCop);
                            else drawCruiser(cp.pos, cp.yaw, now);
                        }
                }
                if (CH.on && !ride.on)
                    for (auto& pc : CH.parked)
                        if (Vector3Distance(pc.pos, cam.position) < 230) drawCruiser(pc.pos, pc.yaw, now);
                inst::flush(R.depth);   // people, cars and cops are queued as instances: draw them now
                EndShaderMode();
                R.endCascade();
            }
            // the wide far cascade (High): only the static city, so only when it has moved
            if (R.farCasc && R.farDirty) {
                R.beginCascade(2);
                W.drawCasters(R.lightView[2], R.half[2], 6000.f, R.depth, true);
                R.endCascade();
                R.farDirty = false;
            }
        }
        R.beginScene(toColor(fogC));
        {
            // the sky: gradient, haze, sun or moon, clouds (see render.cpp)
            float fogK = clampf((fogDen - 0.00011f) / (0.0022f - 0.00011f), 0, 1);
            SkyLook sl;
            sl.fwd = camFwd;
            sl.right = Vector3Normalize(Vector3CrossProduct(camFwd, {0, 1, 0}));
            sl.up = Vector3CrossProduct(sl.right, camFwd);
            sl.tanHalf = tanf(cam.fovy * 0.5f * DEG2RAD);
            sl.aspect = (float)sw / sh2;
            sl.sunDir = sky.sunDir;
            sl.sunCol = sky.sunCol;
            sl.top = heavyFog ? Vector3Lerp(lit.top, fogC, 0.7f) : lit.top;
            sl.horizon = horizonC;
            sl.fog = fogC;
            sl.night = sky.night;
            sl.time = now;
            sl.rain = rainK;
            sl.fogK = fogK;
            R.drawSky(sl);
            // stars on clear nights, hidden behind the clouds
            if (rainK < 0.5f && sky.night > 0.6f && !heavyFog)
                for (auto& st : stars) {
                    if (Vector3DotProduct(st, camFwd) < 0.2f) continue;
                    float a = (150 + (int)(st.x * 1000) % 100) * (1.f - R.cloudAt(st));
                    if (a < 8) continue;
                    Vector2 p = GetWorldToScreen(Vector3Add(cam.position, Vector3Scale(st, 1000)), cam);
                    DrawPixelV(p, {255, 255, 240, (unsigned char)a});
                }
        }

        // everything opaque draws with the city shader, which writes the bloom mask into alpha
        R.opaque(true);
        auto drawMovers = [&](bool nearPass) {
            BeginShaderMode(nearPass ? sh : R.cityFar);
            T.draw(cam.position, camFwd, nearPass);
            if (nearPass) T.drawSignals(W, cam.position);
            BI.draw(cam.position, camFwd);
            GO.draw(W, cam.position);
            SP.draw(now, cam.position);
            T.drawTrains(W, cam.position);
            if (ride.on) Traffic::drawTrain(W, ride.s, ride.dir, cam.position);
            Jet jets[2];
            airTraffic(airT, jets);
            for (auto& j : jets)
                if (j.visible) drawCarModel(CT_PLANE, j.livery, j.pos, j.yaw, j.pitch, false);
            // AirTrain shuttling between the rental car center and the hotel, pausing at each station
            for (int k = 0; k < 2; k++) {
                float ph = fmodf(airT + k * 38.f, 76.f);
                const float stops[4] = {1460, 1640, 2000, 2165};
                int leg = (int)(ph / 19.f);
                float u = clampf((fmodf(ph, 19.f) - 5.f) / 14.f, 0, 1);
                u = u * u * (3 - 2 * u);
                const int order[4][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
                float x = lerpf(stops[order[leg][0]], stops[order[leg][1]], u);
                drawCarModel(CT_AIRTRAIN, WHITE, {x, AIRPORT_Y + 10, 1785}, 0, 0, false);
            }
        };
        // far pass
        rlSetClipPlanes(250.0, 40000.0);
        BeginMode3D(cam);
        W.drawPass(cam, 250, 40000, R.cityFar, true);
        R.opaque(false);   // the bay is see-through: blended over the floor beneath it
        W.drawWater(R.cityFar, true);
        R.opaque(true);
        drawMovers(false);
        inst::flush(R.cityFar);
        EndShaderMode();
        EndMode3D();
        rlDrawRenderBatchActive();
        glClear(GL_DEPTH_BUFFER_BIT);
        // near pass
        rlSetClipPlanes(0.15, 270.0);
        BeginMode3D(cam);
        R.staticNormals(true);
        W.drawPass(cam, 0, 270, sh, false);
        R.opaque(false);   // the bay is see-through: blended over the floor beneath it
        W.drawWater(sh, false);
        R.opaque(true);
        R.staticNormals(false);
        drawMovers(true);
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
        inst::flush(sh);
        EndShaderMode();
        // translucent bits, drawn with the default shader so they blend (and leave the bloom mask alone)
        R.opaque(false);
        rlDisableDepthMask();
        JS.drawFx(cam.position);
        SP.drawFx();
        WX.drawSteam(cam, W, now, sky.night);
        WX.drawRain(cam, W, now, sky.night, R.quality);
        // a soft contact blob under the skater helps judge landings (the sun's shadow can fall far away)
        if (!title && !ride.on && !JS.riding && !SP.riding && !(P.state == Player::BAIL && P.splashed)) {
            Vector3 sp = P.state == Player::BAIL ? P.boardPos : P.pos;
            float gy = W.skaterGround(sp.x, sp.z, P.pos.y + 0.1f);
            float hgt = P.pos.y - gy;
            float k = R.nCasc > 0 ? 0.6f : 1.f;
            if (hgt < 12) DrawCylinder({sp.x, gy + 0.12f, sp.z}, 0.6f, 0.6f, 0.01f, 14, {0, 0, 0, (unsigned char)(55 * k * clampf(1 - hgt / 8, 0.25f, 1))});
        }
        if (R.nCasc == 0) {   // without sun shadows, blobs keep everything grounded
            if (CH.on) {
                for (auto& cp : CH.cops) DrawCylinder({cp.pos.x, cp.pos.y + 0.12f, cp.pos.z}, cp.onFoot ? 0.5f : 2.0f, cp.onFoot ? 0.5f : 2.0f, 0.01f, 14, {0, 0, 0, 70});
                for (auto& pc : CH.parked) DrawCylinder({pc.pos.x, pc.pos.y + 0.12f, pc.pos.z}, 2.0f, 2.0f, 0.01f, 14, {0, 0, 0, 70});
            }
            T.drawShadows(cam.position);
        }
        rlDrawRenderBatchActive();
        rlEnableDepthMask();
        EndMode3D();
        R.setView(cam.fovy, 0.15f, 270.f);   // the near pass's projection, for the ambient occlusion
        R.endScene();

        // graded, bloomed, and blurred at speed
        {
            static Vector3 lastCam = cam.position;
            static float camSpd = 0;
            float moved = Vector3Distance(cam.position, lastCam);
            lastCam = cam.position;
            if (moved < 6.f && dt > 0) camSpd = lerpf(camSpd, moved / dt, 1 - expf(-dt * 5));
            Grade g;
            g.gain = lit.gain;
            g.sat = lit.sat;
            g.contrast = sky.contrast;
            g.bloom = sky.bloom * (heavyFog ? 1.2f : 1.f);
            g.vignette = 0.22f;
            g.blur = (title || paused) ? 0.f : smooth01(14.f, 45.f, camSpd) * 0.012f;
            R.present(g);
        }

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
                if (i < 10) DrawText(TextFormat("%d", (i + 1) % 10), px + 28, y + 4, 20, sel ? WHITE : Color{150, 160, 180, 255});
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
                const char* hint = "Mash SPACE and A / D   -   swing the board with J K L";
                int pw = std::max(bw, MeasureText(hint, 20)) + 24;   // backdrop wide enough for the hint line
                DrawRectangle(sw / 2 - pw / 2, by - 56, pw, 130, {0, 0, 0, 170});
                float jit = sinf(now * 45.f) * 2.f;
                textCenter("STRUGGLE!", sw / 2 + (int)jit, by - 48, 40, {255, 140, 70, 255});
                DrawRectangle(bx, by, bw, 24, {170, 40, 40, 255});
                DrawRectangle(bx, by, (int)(bw * clampf(CH.meter, 0, 1)), 24, {110, 230, 120, 255});
                DrawRectangleLines(bx, by, bw, 24, WHITE);
                textCenter(hint, sw / 2, by + 36, 20, WHITE);
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
                const char* skateLines[] = {"CONTROLS", "W / Up      push", "S / Down    brake", "A D         steer / spin in air",
                                       "Shift       tuck (go faster)", "Space       ollie / jump off rail", "J  K  L     kickflip, heelflip, shove-it",
                                       "I (hold)    grab", "Land on rails & ledges to grind", "B  free roam / cop chase   N  difficulty", "Right mouse  look around", "Wheel       zoom    C  camera",
                                       "Tab  choose landmark / time", "M  map  1-0  teleport  E  train/jetski", "T or F1-F3  time of day",
                                       "F fog  G rain  R reset  P pause", TextFormat("V  graphics: %s", Renderer::QUALITY_NAMES[R.quality]),
                                       TextFormat("Y  frame rate: %s", gCap60 ? "60" : "Max"), "Caught: mash SPACE A D / J K L", "H  hide this help"};
                // on a jet ski or seaplane the panel shows its controls instead, with its name as a small title
                // ("key|action": drawn as two aligned columns)
                const char* skiLines[] = {"JET SKI", "W|go", "S|brake", "A D|steer", "Shift|boost", "E|hop off at the shore",
                                          "Right mouse|look around", "H|hide controls"};
                const char* planeLines[] = {"SEAPLANE", "W / S|throttle", "A D|steer", "Space|climb (lift off past 55 mph)",
                                            "Shift|dive", "E|climb out at the shore", "Right mouse|look around", "H|hide controls"};
                const char** lines = JS.riding ? skiLines : SP.riding ? planeLines : skateLines;
                const int n = JS.riding ? (int)(sizeof(skiLines) / sizeof(*skiLines))
                            : SP.riding ? (int)(sizeof(planeLines) / sizeof(*planeLines)) : (int)(sizeof(skateLines) / sizeof(*skateLines));
                Color head = JS.riding ? Color{120, 220, 255, 255} : SP.riding ? Color{255, 225, 140, 255} : Color{255, 210, 60, 255};
                // size the panel from the measured text so no line is cut off; in small windows shrink the font
                // until it fits below the minimap (y 270) and clear of the FPS counter below it, then slide it up if still too tall
                auto split = [](const char* l, std::string& key) {   // "key|action": key out, action returned (null if no '|')
                    const char* bar = strchr(l, '|');
                    if (bar) key.assign(l, bar - l);
                    return bar ? bar + 1 : nullptr;
                };
                auto keyCol = [&](int fs) {   // width of the key column
                    int w = 0; std::string k;
                    for (int i = 1; i < n; i++) if (split(lines[i], k)) w = std::max(w, MeasureText(k.c_str(), fs) + 14);
                    return w;
                };
                auto panelW = [&](int fs) {
                    int w = 0, kc = keyCol(fs); std::string k;
                    for (int i = 0; i < n; i++) {
                        const char* act = i ? split(lines[i], k) : nullptr;
                        w = std::max(w, act ? kc + MeasureText(act, fs) : MeasureText(lines[i], i == 0 ? fs + 4 : fs));
                    }
                    return w + 28;
                };
                auto panelH = [&](int fs) { return 10 + n * (fs + 4) + 8; };   // 22 px rows at size 18
                int fs = 18;
                while (fs > 12 && (panelH(fs) > sh2 - 40 - 270 || panelW(fs) > sw - 32)) fs--;
                while (fs > 10 && panelH(fs) > sh2 - 32) fs--;   // tiny windows: last resort, smallest legible size
                int hw = panelW(fs), hh = panelH(fs);
                int hx = std::max(16, sw - hw - 16), hy = std::max(16, std::min(270, sh2 - 40 - hh));
                DrawRectangle(hx, hy, hw, hh, {0, 0, 0, 140});
                int kc = keyCol(fs); std::string k;
                for (int i = 0; i < n; i++) {
                    int y = hy + 10 + i * (fs + 4);
                    const char* act = i ? split(lines[i], k) : nullptr;
                    if (act) {
                        DrawText(k.c_str(), hx + 14, y, fs, {200, 210, 225, 255});
                        DrawText(act, hx + 14 + kc, y, fs, WHITE);
                    } else DrawText(lines[i], hx + 14, y, i == 0 ? fs + 4 : fs, i == 0 ? head : WHITE);
                }
            } else if (JS.riding || SP.riding) {
                textShadow("H  show controls", sw - MeasureText("H  show controls", 18) - 24, 270, 18, {220, 225, 235, 220});
            }
            }
            if (nearSki >= 0) {
                promptBar("Press E to ride the jet ski", sw, sh2 - 230, 22, {20, 110, 170, 220}, WHITE);
            }
            if (nearPlane >= 0) {
                promptBar("Press E to fly the seaplane", sw, sh2 - 230, 22, {200, 140, 30, 230}, WHITE);
            }
            if (SP.riding) {
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
                promptBar(TextFormat("Press E to ride Caltrain to %s", to), sw, sh2 - 230, 22, {200, 30, 40, 220}, WHITE);
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
        waitForFrameSlot();
        static bool firstFrame = true;
        if (firstFrame) {
            firstFrame = false;
            TraceLog(LOG_INFO, "SFSKATE: first frame done %.2fs after InitWindow", GetTime());
            // launched from a terminal, macOS leaves the keyboard with the terminal: the first arrow press
            // on the start menu went there instead of the game. Take the keyboard once the window is up.
            if (!autotest) SetWindowFocused();
        }
    }
    if (autotest) TraceLog(LOG_INFO, "SFSKATE: average fps %.1f", atFpsN ? atFpsSum / atFpsN : 0.0);
    S.shutdown();
    W.freeInteriors();
    W.freeFoliage();
    inst::setFrameBatch(nullptr);
    rlSetRenderBatchActive(nullptr);   // back to raylib's own batch, which CloseWindow frees
    rlUnloadRenderBatch(frameBatch);
    CloseWindow();
    return 0;
}
