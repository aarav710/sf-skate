#include "birds.h"

void Birds::init(const World& w) {
    RNG r(8080);
    // gulls: a few over each stretch of waterfront (Pier 39 and its sea lions, the Wharf, the Ferry Building, the
    // Embarcadero piers, Crissy Field, Ocean Beach, under the Golden Gate)
    const Vector3 GULLS[] = {{1000, 0, -1210}, {970, 0, -1265}, {700, 0, -1175}, {490, 0, -1210}, {1300, 0, -200}, {1330, 0, -600},
                             {1320, 0, 250}, {-500, 0, -1120}, {-1460, 0, -60}, {-1460, 0, 520}, {-1090, 0, -1500}, {1240, 0, 700}};
    for (auto& g : GULLS)
        for (int k = 0; k < 6; k++) {
            Bird b;
            b.gull = true;
            b.home = {g.x + r.range(-40, 40), 0, g.z + r.range(-40, 40)};
            b.radius = r.range(14, 45);
            b.alt = r.range(12, 38);
            b.speed = r.range(0.18f, 0.32f) * (r.chance(0.5f) ? 1.f : -1.f);   // turn rate, either way round
            b.phase = r.range(0, TAU_F);
            b.t = r.range(0, 100);
            birds.push_back(b);
        }
    // pigeons: flocks on the open paving of Union Square, Embarcadero Plaza and Civic Center
    struct Flock { float x, z, spread; int n; };
    const Flock FLOCKS[] = {{(gx(19) + gx(20)) / 2 - 12, (gz(6) + gz(7)) / 2 + 6, 9, 22}, {(gx(19) + gx(20)) / 2 + 14, (gz(6) + gz(7)) / 2 - 8, 6, 12},
                            {1188, -255, 10, 24}, {1215, -230, 7, 12},
                            {(gx(14) + gx(15)) / 2, (gz(12) + gz(13)) / 2, 6, 18}, {(gx(14) + gx(15)) / 2 - 2, (gz(12) + gz(13)) / 2 + 40, 5, 10}};
    for (auto& f : FLOCKS)
        for (int k = 0; k < f.n; k++) {
            Bird b;
            float x = f.x + r.range(-f.spread, f.spread), z = f.z + r.range(-f.spread, f.spread);
            b.home = {x, w.ground(x, z, w.terrain(x, z) + 1.f), z};
            b.pos = b.home;
            b.yaw = r.range(0, TAU_F);
            b.phase = r.range(0, TAU_F);
            b.t = r.range(0, 5);
            birds.push_back(b);
        }
    // gulls wheeling round Alcatraz, which is famous for them (their own stream, so the flocks above stay put)
    RNG ar(1859);
    for (Vector3 g : {Vector3{300, 0, -1900}, Vector3{420, 0, -1890}, Vector3{220, 0, -1910}})
        for (int k = 0; k < 7; k++) {
            Bird b;
            b.gull = true;
            b.home = {g.x + ar.range(-40, 40), 0, g.z + ar.range(-30, 30)};
            b.radius = ar.range(14, 45);
            b.alt = ar.range(40, 70);   // high enough to clear the cellhouse and the water tower
            b.speed = ar.range(0.18f, 0.32f) * (ar.chance(0.5f) ? 1.f : -1.f);
            b.phase = ar.range(0, TAU_F);
            b.t = ar.range(0, 100);
            birds.push_back(b);
        }
}

void Birds::update(float dt, const World& w, Vector3 sk, float skSpeed) {
    evScatter = false;
    for (auto& b : birds) {
        b.t += dt;
        if (b.gull) {   // wheel round in a lazy circle that wanders, rising and dipping on the breeze
            b.phase += b.speed * dt;
            float r = b.radius * (1 + 0.25f * sinf(b.t * 0.13f));
            Vector3 c = {b.home.x + sinf(b.t * 0.05f) * 25, 0, b.home.z + cosf(b.t * 0.043f) * 25};
            Vector3 np = {c.x + cosf(b.phase) * r, b.alt + sinf(b.t * 0.4f) * 3, c.z + sinf(b.phase) * r};
            if (Vector3LengthSqr(b.pos) > 0) b.vel = Vector3Scale(Vector3Subtract(np, b.pos), 1.f / std::max(dt, 1e-3f));
            b.pos = np;
            b.yaw = atan2f(b.vel.z, b.vel.x);
            continue;
        }
        float dx = b.pos.x - sk.x, dz = b.pos.z - sk.z, d2 = dx * dx + dz * dz;
        if (b.state == 0) {
            // pecking about: little steps, turning now and then
            if (fmodf(b.t + b.phase, 2.3f) < dt * 1.5f) b.yaw += sinf(b.phase * 7 + b.t) * 1.4f;
            if (fmodf(b.t * 1.7f + b.phase, 3.f) < 0.6f) {
                float nx = b.pos.x + cosf(b.yaw) * 0.35f * dt, nz = b.pos.z + sinf(b.yaw) * 0.35f * dt;
                // follow the paving up and down the slope, but turn back from a kerb or ledge too tall to hop
                float ny = w.ground(nx, nz, b.pos.y);
                Vector3 np = {nx, ny, nz};
                if (sq(nx - b.home.x) + sq(nz - b.home.z) < 64 && fabsf(ny - b.pos.y) < 0.3f && !w.collide(np, 0.1f, ny, 0.3f, nullptr))
                    b.pos = {nx, ny, nz};
                else b.yaw += PI * 0.7f;
            }
            // something on wheels comes through: everyone nearby goes up at once
            if ((d2 < 6.5f * 6.5f && skSpeed > 2.f) || d2 < 2.2f * 2.2f) {
                if (fabsf(sk.y - b.pos.y) < 3.f) {
                    for (auto& o : birds)
                        if (!o.gull && o.state == 0 && Vector3Distance(o.home, b.home) < 22) {
                            o.state = 1;
                            o.t = 0;
                            Vector3 away = Vector3Normalize({o.pos.x - sk.x + 0.01f, 0, o.pos.z - sk.z});
                            float a = atan2f(away.z, away.x) + sinf(o.phase * 13) * 0.7f;
                            o.vel = {cosf(a) * 6.f, 5.5f + 1.5f * sinf(o.phase * 5), sinf(a) * 6.f};
                            float far = 35 + 20 * fabsf(sinf(o.phase * 3));
                            o.target = {o.home.x + cosf(a) * far, o.home.y + 10 + 4 * sinf(o.phase), o.home.z + sinf(a) * far};
                        }
                    evScatter = true;
                }
            }
        } else if (b.state == 1) {
            // climb away, swing round in a loop over the plaza, then head back
            float back = b.t > 9.f + 3.f * fabsf(sinf(b.phase)) ? 1.f : 0.f;
            Vector3 goal = back > 0 ? Vector3Add(b.home, {0, 2.5f, 0}) : b.target;
            Vector3 want = Vector3Subtract(goal, b.pos);
            float L = Vector3Length(want);
            Vector3 dv = L > 0.1f ? Vector3Scale(want, (back > 0 ? 7.f : 9.f) / L) : Vector3Zero();
            if (back == 0 && L < 8) { dv = {-want.z, 0, want.x}; dv = Vector3Scale(Vector3Normalize(dv), 8.f); } // circle the target
            b.vel = Vector3Lerp(b.vel, dv, 1 - expf(-dt * 1.6f));
            b.pos = Vector3Add(b.pos, Vector3Scale(b.vel, dt));
            b.yaw = atan2f(b.vel.z, b.vel.x);
            if (back > 0 && L < 3.f) { b.state = 2; b.t = 0; }
        } else {
            // flutter down onto the paving
            Vector3 want = Vector3Subtract(b.home, b.pos);
            b.vel = Vector3Lerp(b.vel, Vector3Scale(want, 1.6f), 1 - expf(-dt * 3.f));
            b.pos = Vector3Add(b.pos, Vector3Scale(b.vel, dt));
            if (b.pos.y - b.home.y < 0.05f || b.t > 3.f) { b.pos = b.home; b.state = 0; b.t = 0; }
        }
    }
}

void Birds::draw(Vector3 cam, Vector3 camFwd) const {
    float now = (float)GetTime();
    for (auto& b : birds) {
        Vector3 d = Vector3Subtract(b.pos, cam);
        float d2 = Vector3LengthSqr(d);
        if (d2 > (b.gull ? 420.f * 420.f : 150.f * 150.f)) continue;
        if (Vector3DotProduct(d, camFwd) < -2) continue;
        rlPushMatrix();
        rlTranslatef(b.pos.x, b.pos.y, b.pos.z);
        rlRotatef(-b.yaw * RAD2DEG, 0, 1, 0);
        if (b.gull) {
            // mostly gliding on stiff wings with a slow flap now and then, banking into the turn
            float flap = fmodf(b.t + b.phase, 7.f) < 1.6f ? sinf(b.t * 9.f) * 0.55f : 0.08f * sinf(b.t * 1.3f);
            rlRotatef(-b.speed * 70.f, 1, 0, 0);
            Color white = {238, 238, 236, 255}, grey = {150, 156, 165, 255}, tip = {30, 30, 32, 255};
            DrawCube({0, 0, 0}, 0.55f, 0.13f, 0.14f, white);
            DrawCube({0.31f, 0.03f, 0}, 0.11f, 0.1f, 0.1f, white);                       // head
            DrawCube({0.4f, 0.02f, 0}, 0.08f, 0.025f, 0.03f, {240, 190, 40, 255});        // beak
            DrawCube({-0.32f, 0.01f, 0}, 0.14f, 0.03f, 0.13f, grey);                     // tail
            for (int s = -1; s <= 1; s += 2) {
                rlPushMatrix();
                rlTranslatef(0.03f, 0.04f, s * 0.07f);
                rlRotatef(s * flap * RAD2DEG, 1, 0, 0);
                DrawCube({0, 0, s * 0.33f}, 0.24f, 0.02f, 0.62f, grey);
                DrawCube({-0.03f, 0, s * 0.68f}, 0.16f, 0.018f, 0.12f, tip);
                rlPopMatrix();
            }
        } else {
            Color body = {120, 122, 134, 255}, neck = {88, 104, 110, 255}, wing = {150, 152, 162, 255};
            if (b.state == 0) {
                float peck = fmodf(b.t * 1.3f + b.phase, 2.f) < 0.5f ? -0.07f : 0.f;   // head down to the ground
                float bob = sinf(b.t * 8.f + b.phase) * 0.01f;
                DrawCube({0, 0.12f, 0}, 0.26f, 0.13f, 0.13f, body);
                DrawCube({-0.15f, 0.12f, 0}, 0.1f, 0.03f, 0.1f, {80, 82, 92, 255});
                DrawCube({0.12f + bob, 0.21f + peck, 0}, 0.08f, 0.09f, 0.08f, neck);
                DrawCube({0.17f + bob, 0.21f + peck, 0}, 0.03f, 0.02f, 0.02f, {60, 50, 50, 255});
                for (int s = -1; s <= 1; s += 2) DrawCube({0.02f, 0.03f, s * 0.035f}, 0.02f, 0.07f, 0.02f, {200, 100, 100, 255});
            } else {
                float flap = sinf(now * 22.f + b.phase) * 0.9f;
                DrawCube({0, 0, 0}, 0.28f, 0.12f, 0.12f, body);
                DrawCube({0.17f, 0.03f, 0}, 0.08f, 0.08f, 0.08f, neck);
                DrawCube({-0.18f, 0, 0}, 0.12f, 0.02f, 0.12f, {80, 82, 92, 255});
                for (int s = -1; s <= 1; s += 2) {
                    rlPushMatrix();
                    rlTranslatef(0, 0.03f, s * 0.05f);
                    rlRotatef(s * flap * RAD2DEG, 1, 0, 0);
                    DrawCube({0, 0, s * 0.17f}, 0.17f, 0.015f, 0.32f, wing);
                    rlPopMatrix();
                }
            }
        }
        rlPopMatrix();
    }
}
