#include "weather.h"

void Weather::init() {
    Image img = GenImageGradientRadial(64, 64, 0.f, {255, 255, 255, 255}, {255, 255, 255, 0});
    puff = LoadTextureFromImage(img);
    UnloadImage(img);
    SetTextureFilter(puff, TEXTURE_FILTER_BILINEAR);
}

void Weather::update(float dt) {
    rain += ((raining ? 1.f : 0.f) - rain) * (1 - expf(-dt * 0.5f));
    if (rain > wet) wet = std::min(rain, wet + dt * rain / 15.f);   // soaks in
    else wet = std::max(rain, wet - dt / 70.f);                     // dries out
}

// Streaks fixed in the world on a 44 m repeating lattice around the camera, so walking through the rain looks right;
// each falls on its own clock and wraps from 18 m below the eye back to 18 m above it.
void Weather::drawRain(const Camera3D& cam, const World& w, float now, float night, int quality) const {
    if (rain < 0.02f) return;
    if (w.ceiling(cam.position.x, cam.position.z, cam.position.y - 1.7f) < 1e8f) return;   // indoors or under a deck
    const float BOX = 44.f, H = 36.f, SPEED = 10.5f, LEN = 0.55f;
    int n = (int)((quality == 2 ? 3200 : quality == 1 ? 2200 : 1300) * rain);
    Vector3 wind = {0.22f, 0, 0.1f};   // a slant from the ocean breeze
    Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    unsigned char a = (unsigned char)(85 * rain);
    Color c = night > 0.6f ? Color{150, 160, 185, a} : Color{205, 212, 225, a};
    rlBegin(RL_LINES);
    for (int i = 0; i < n; i++) {
        uint32_t h = hashu(i, 7, 31);
        float hx = (h & 1023) / 1024.f * BOX, hz = ((h >> 10) & 1023) / 1024.f * BOX, hy = ((h >> 20) & 1023) / 1024.f;
        float x = hx + BOX * roundf((cam.position.x - hx) / BOX), z = hz + BOX * roundf((cam.position.z - hz) / BOX);
        float fall = fmodf(now * SPEED * (0.9f + 0.2f * hy) + hy * H, H);
        float y = cam.position.y + H * 0.5f - fall;
        Vector3 p = {x + wind.x * (H - fall), y, z + wind.z * (H - fall)};
        Vector3 d = Vector3Subtract(p, cam.position);
        if (Vector3DotProduct(d, fwd) < 0.5f) continue;   // behind the camera
        float dd = Vector3LengthSqr(d);
        if (dd > 22.f * 22.f) continue;
        float len = LEN * (dd < 36.f ? 1.6f : 1.f);
        rlColor4ub(c.r, c.g, c.b, c.a);
        rlVertex3f(p.x, p.y, p.z);
        rlColor4ub(c.r, c.g, c.b, 0);
        rlVertex3f(p.x - wind.x * len, p.y + len, p.z - wind.z * len);
    }
    rlEnd();
}

// Puffs of steam from some manholes: each rises, swells and fades on its own cycle, drifting with the breeze.
void Weather::drawSteam(const Camera3D& cam, const World& w, float now, float night) const {
    for (size_t v = 0; v < w.steamVents.size(); v++) {
        Vector3 b = w.steamVents[v];
        float d2 = sq(b.x - cam.position.x) + sq(b.z - cam.position.z);
        if (d2 > 160.f * 160.f) continue;
        float fade = 1.f - smooth01(120.f, 160.f, sqrtf(d2));
        for (int k = 0; k < 8; k++) {
            float ph = fmodf(now * 0.22f + k / 8.f + v * 0.37f, 1.f);
            float rise = ph * 4.5f;
            Vector3 p = {b.x + 1.4f * ph * ph + sinf(now * 0.7f + k) * 0.25f * ph, b.y + 0.2f + rise, b.z + 0.6f * ph * ph};
            float size = 0.8f + ph * 3.4f;
            float al = sinf(ph * PI) * (0.55f - 0.3f * ph) * fade * (1.f + rain * 0.6f);
            Color tint = night > 0.6f ? Color{170, 165, 160, 255} : Color{240, 240, 242, 255};
            tint.a = (unsigned char)clampf(al * 150.f, 0, 255);
            DrawBillboard(cam, puff, p, size, tint);
        }
    }
}
