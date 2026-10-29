// Terrain shape (coastlines + hills), zones, collision queries, ground mesh and minimap.
#include "world_internal.h"
#include <thread>

const Color C_ASPHALT = col(58, 58, 63, M_GROUND);
const Color C_SIDEWALK = col(158, 156, 150, M_GROUND);
const Color C_PLAZA = col(172, 165, 154, M_GROUND);
const Color C_GRASS = col(92, 138, 68, M_GROUND);
const Color C_SAND = col(206, 190, 150, M_GROUND);
const Color C_YARD = col(104, 136, 80, M_GROUND);
const Color C_LOT = col(150, 147, 140, M_GROUND);

// ---------------------------------------------------------------- coastlines
struct Poly { int id; float shoreW; std::vector<Vector2> p; float minx, maxx, minz, maxz; };
static std::vector<Poly> gPolys;
enum { P_SF, P_MARIN, P_OAK, P_YBI, P_TI, P_ALCATRAZ, P_ANGEL, P_SFO, P_PEN };

static void addPoly(int id, float shore, std::vector<Vector2> pts) {
    Poly P{id, shore, pts, 1e9f, -1e9f, 1e9f, -1e9f};
    for (auto& v : pts) {
        P.minx = std::min(P.minx, v.x); P.maxx = std::max(P.maxx, v.x);
        P.minz = std::min(P.minz, v.y); P.maxz = std::max(P.maxz, v.y);
    }
    gPolys.push_back(P);
}
static void addEllipse(int id, float shore, float cx, float cz, float rx, float rz, int n, float wob) {
    std::vector<Vector2> pts;
    for (int i = 0; i < n; i++) {
        float a = TAU_F * i / n;
        float w = 1.0f + wob * sinf(a * 3 + cx * 0.01f) + wob * 0.5f * cosf(a * 5 + cz * 0.01f);
        pts.push_back({cx + cosf(a) * rx * w, cz + sinf(a) * rz * w});
    }
    addPoly(id, shore, pts);
}

static float polySD(const Poly& P, float x, float z) {
    float d2 = 1e18f;
    bool inside = false;
    size_t n = P.p.size();
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        Vector2 a = P.p[j], b = P.p[i];
        if (((a.y > z) != (b.y > z)) && (x < (b.x - a.x) * (z - a.y) / (b.y - a.y) + a.x)) inside = !inside;
        float d = distToSeg2D(x, z, a.x, a.y, b.x, b.y);
        d2 = std::min(d2, d * d);
    }
    float d = sqrtf(d2);
    return inside ? d : -d;
}

struct Hill { float x, z, h, r; };
static const Hill SF_H[] = {
    {-300, 880, 55, 420},  {-340, 850, 62, 150}, {-250, 945, 58, 150},   // Twin Peaks
    {-520, 690, 45, 170},                                               // Mt Sutro
    {580, -400, 55, 190},                                               // Nob Hill
    {330, -760, 62, 170},                                               // Russian Hill
    {880, -880, 50, 110},                                               // Telegraph Hill
    {-150, -520, 45, 260},                                              // Pacific Heights
    {-1150, -880, 35, 230},                                             // Presidio
    {-560, -80, 30, 140},                                               // Lone Mountain
    {-430, 520, 40, 130},                                               // Buena Vista
    {-330, 170, 16, 90},                                                // Alamo Square
    {900, 1250, 35, 220},                                               // Potrero Hill
    {350, 1550, 55, 180},                                               // Bernal Heights
    {-500, 1300, 70, 220},                                              // Mt Davidson
    {-150, 1250, 45, 200},                                              // Diamond Heights
    {200, 2350, 230, 380},                                              // San Bruno Mtn (south backdrop)
    {-900, 2200, 120, 400},
};
static const Hill MARIN_H[] = {
    {-1500, -2850, 230, 420}, {-800, -2900, 150, 300}, {-1250, -2520, 70, 140},
    {-450, -2750, 90, 200},   {-2300, -2700, 180, 400}, {-1100, -3400, 260, 500},
    {-2600, -5300, 780, 1400},                                                    // Mount Tamalpais
    {-5200, -7200, 420, 1600}, {-800, -6500, 300, 1100},
};
// The Peninsula south of the city: the mountain spine you see from SFO
static const Hill PEN_H[] = {
    {200, 2350, 230, 380},   {980, 2440, 110, 260},                                // San Bruno Mtn, Sign Hill
    {-450, 3000, 300, 450},  {-200, 3650, 330, 520},                               // Sweeney Ridge
    {-900, 4300, 520, 700},  {-300, 5200, 420, 700},                               // Montara Mountain
    {600, 4200, 120, 500},   {900, 6100, 160, 650},                                // foothills
    {-300, 6800, 520, 950},  {-2600, 9200, 620, 1500}, {-2800, 11200, 680, 1700},  // Santa Cruz Mountains, far to the west
    {200, 9700, 110, 700},   {500, 11300, 120, 700},                               // Woodside / Portola Valley foothills
};
struct Flat { float x, z, r0, r1, h; };
static const Flat FLATS[] = {
    {-1100, -2410, 55, 125, 30.0f}, // Vista Point at the north end of the Golden Gate
    {1225, 775, 80, 130, 3.0f},     // ballpark
};

static float hillSum(const Hill* H, int n, float x, float z) {
    float s = 0;
    for (int i = 0; i < n; i++) {
        float d2 = sq(x - H[i].x) + sq(z - H[i].z);
        float r2 = H[i].r * H[i].r;
        if (d2 < 9 * r2) s += H[i].h * expf(-d2 / r2);
    }
    return s;
}
static float rolling(float x, float z) {
    return 2.0f * sinf(x * 0.0061f + 1.3f) * cosf(z * 0.0047f + 0.4f) + 1.2f * sinf(x * 0.013f + z * 0.009f);
}

static float vn(float x, float z, float cell, int seed) {
    float fx = x / cell, fz = z / cell;
    int i = (int)floorf(fx), j = (int)floorf(fz);
    float tx = fx - i, tz = fz - j;
    tx = tx * tx * (3 - 2 * tx); tz = tz * tz * (3 - 2 * tz);
    return lerpf(lerpf(hash01(i, j, seed), hash01(i + 1, j, seed), tx), lerpf(hash01(i, j + 1, seed), hash01(i + 1, j + 1, seed), tx), tz);
}
// Ridged noise: turns smooth domes into hills with ridges and gullies.
static float ridges(float x, float z) {
    float s = 0, amp = 0.5f, cell = 900;
    for (int o = 0; o < 4; o++) {
        float n = vn(x, z, cell, 11 + o);
        s += amp * (1 - fabsf(2 * n - 1));
        amp *= 0.5f; cell *= 0.5f;
    }
    return s / 0.9375f; // 0..1
}
static float roughen(float h, float x, float z) { return 3 + (h - 3) * (0.55f + 0.7f * ridges(x, z)); }

static float landHeight(int id, float x, float z) {
    switch (id) {
    case P_SF: {
        float hs = hillSum(SF_H, sizeof(SF_H) / sizeof(Hill), x, z);
        float flat = 1 - 0.92f * smooth01(780, 1000, x) * smooth01(-780, -640, z);
        flat *= 1 - 0.85f * smooth01(380, 560, x) * smooth01(-40, 120, z) * (1 - smooth01(1050, 1200, z));
        float roll = rolling(x, z) * (x < 650 ? 0.6f : 0.2f);
        return 3.0f + (hs + roll) * flat;
    }
    case P_MARIN: return roughen(4.0f + hillSum(MARIN_H, sizeof(MARIN_H) / sizeof(Hill), x, z) + std::max(rolling(x, z) * 1.5f, -1.5f), x, z);
    case P_OAK: {
        // the East Bay shore bends away east south of Oakland, so the hills sit far across the water from SFO
        float shore = z < 1300 ? 4750.f : z < 2300 ? lerpf(4750, 5200, (z - 1300) / 1000) : z < 3800 ? lerpf(5200, 6300, (z - 2300) / 1500)
                    : z < 6000 ? lerpf(6300, 7000, (z - 3800) / 2200) : 7000 + (z - 6000) * 0.2f;
        float h = 3.0f + 330 * smooth01(shore, shore + 1500, x) + rolling(x, z) * smooth01(4500, 5200, x) * 4;
        h += 140 * sinf(x * 0.0011f) * cosf(z * 0.0009f + 1.0f) * smooth01(6200, 8200, x);
        h += 900 * expf(-(sq(x - 17000) + sq(z + 1500)) / sq(2200));                   // Mount Diablo, far on the horizon
        h += 250 * expf(-(sq(x - 9500) + sq(z - 8000)) / sq(2200));
        return z > 1300 ? roughen(h, x, z) : lerpf(h, roughen(h, x, z), smooth01(5000, 6000, x));
    }
    case P_YBI: return 3.0f + 48 * expf(-(sq(x - 2620) + sq(z - 170)) / sq(110));
    case P_TI: return 2.8f;
    case P_ALCATRAZ: return 3.0f + 22 * expf(-(sq(x - 300) + sq(z - 1900 * -1)) / sq(70));
    case P_SFO: return AIRPORT_Y;
    case P_PEN: {
        float h = 3.0f + hillSum(PEN_H, sizeof(PEN_H) / sizeof(Hill), x, z) + std::max(rolling(x, z) * 3.0f, -1.5f) +
                  std::max(0.f, 40 * sinf(x * 0.0021f + 0.7f) * cosf(z * 0.0017f)) * smooth01(2600, 3600, z);
        h = std::max(h, 2.5f);
        if (sq(x - 980) + sq(z - 2440) < sq(330)) return h; // keep Sign Hill smooth for its letters
        return lerpf(h, roughen(h, x, z), smooth01(2100, 2600, z));
    }
    case P_ANGEL: return 3.0f + 240 * expf(-(sq(x - 330) + sq(z + 3050)) / sq(330));
    }
    return 0;
}

static void initRail();

void initGeography() {
    if (!gPolys.empty()) return;
    addPoly(P_SF, 25, {{-1480, 2600}, {-1480, -480}, {-1445, -640}, {-1370, -790}, {-1260, -910},
                       {-1160, -1015}, {-1085, -1078}, {-950, -1092}, {-700, -1098}, {-450, -1106},
                       {-150, -1110}, {100, -1116}, {250, -1136}, {450, -1150}, {650, -1152},
                       {850, -1142}, {1000, -1112}, {1120, -1042}, {1230, -922}, {1300, -762},
                       {1325, -500}, {1330, -200}, {1340, 100}, {1345, 400}, {1338, 700},
                       {1322, 1000}, {1352, 1300}, {1380, 1700}, {1400, 2000}, {1360, 2600}});
    addPoly(P_MARIN, 70, {{-2900, -2330}, {-2000, -2310}, {-1500, -2300}, {-1250, -2292}, {-1000, -2300},
                          {-800, -2335}, {-560, -2420}, {-330, -2560}, {-200, -2760}, {-160, -3000},
                          {-260, -3900}, {500, -5200}, {900, -8000}, {700, -14500}, {-12000, -14500},
                          {-12000, -7000}, {-5000, -3300}});
    addPoly(P_OAK, 30, {{3985, -2600}, {3965, -1500}, {3992, -800}, {4010, 0}, {4002, 600},
                        {3990, 1300}, {4500, 2300}, {5600, 3800}, {6300, 6000}, {6900, 9000}, {7400, 14500},
                        {18000, 14500}, {18000, -14500}, {2600, -14500}, {2900, -8000}, {3700, -5000}});
    addPoly(P_PEN, 40, {{-1700, 1900}, {1340, 1900}, {1360, 2620}, {1700, 2700}, {2150, 3300}, {2600, 5000},
                        {3400, 8000}, {4200, 11000}, {4800, 14500}, {-3000, 14500}, {-2200, 8000},
                        {-1900, 5000}, {-1600, 3200}});
    addEllipse(P_YBI, 30, 2620, 190, 180, 150, 24, 0.06f);
    addPoly(P_TI, 10, {{2430, -720}, {2860, -720}, {2880, -80}, {2700, 30}, {2560, 30}, {2430, -80}});
    addEllipse(P_ALCATRAZ, 12, 300, -1900, 130, 70, 16, 0.08f);
    addEllipse(P_ANGEL, 80, 330, -3050, 520, 420, 24, 0.1f);
    // SFO: flat reclaimed land on the bay shore south of the city
    // (rail profile is computed at the end of this function)
    addPoly(P_SFO, 12, {{1340, 1580}, {2660, 1580}, {2740, 1680}, {2740, 2530}, {2620, 2580}, {1340, 2600}});
    initRail();
}

static float rawHeight(float x, float z) {
    float best = -14.f;
    for (auto& P : gPolys) {
        if (x < P.minx - 150 || x > P.maxx + 150 || z < P.minz - 150 || z > P.maxz + 150) continue;
        float sd = polySD(P, x, z);
        if (sd < -150) continue;
        float h;
        if (sd >= 0) {
            float shoreW = P.shoreW;
            if (P.id == P_SF) shoreW = lerpf(25, 140, smooth01(-1300, -1420, x)); // Ocean Beach is wide and flat
            float land = landHeight(P.id, x, z);
            h = 0.6f + (land - 0.6f) * smooth01(0, shoreW, sd);
        } else {
            h = 0.6f - 14.6f * smooth01(0, 60, -sd);
        }
        best = std::max(best, h);
    }
    // SFO: a raised plateau that blends into the land around it, with a bank down to the bay
    {
        float dx = std::max(std::max(1360.f - x, 0.f), x - 2720.f), dz = std::max(std::max(1590.f - z, 0.f), z - 2560.f);
        float d = sqrtf(dx * dx + dz * dz);
        if (d == 0 && best > -2) best = AIRPORT_Y;
        else if (d < 140 && best > 0.6f) best = lerpf(best, AIRPORT_Y, 1 - smooth01(0, 140, d));
    }
    for (auto& F : FLATS) {
        float d = sqrtf(sq(x - F.x) + sq(z - F.z));
        if (d < F.r1 && best > 0) best = lerpf(best, F.h, 1 - smooth01(F.r0, F.r1, d));
    }
    // Menlo Park: gentle bayside flats rising slowly toward the wooded hills in the west
    {
        float dx = std::max(std::max(1100.f - x, 0.f), x - 3900.f), dz = std::max(std::max(8400.f - z, 0.f), z - 10800.f);
        float d = sqrtf(dx * dx + dz * dz);
        if (d < 400 && best > 0.6f) {
            float target = 9 + clampf((2300 - x) * 0.012f, 0, 12) + 1.2f * sinf(x * 0.004f) * cosf(z * 0.005f);
            target = lerpf(target, 2.2f, smooth01(3300, 3780, x)); // low marshland at the bay
            best = lerpf(best, target, 1 - smooth01(0, 400, d));
        }
    }
    return best;
}

// ---------------------------------------------------------------- Caltrain corridor
// 4th & King -> along the bayside of the Peninsula -> Menlo Park -> Palo Alto
static std::vector<Vector2> gRoute = {{1210, 880},  {1210, 1760}, {1300, 1900}, {1300, 2700}, {1500, 3600},
                                      {1900, 5000}, {2200, 6500}, {2400, 8000}, {2450, 10650}};
static std::vector<float> gCum, gGrade; // grade sampled every 10 m
constexpr float GRADE_STEP = 10.f;

static Vector2 routeAt(float s, Vector2* dir) {
    s = clampf(s, 0, gCum.back());
    size_t k = 1;
    while (k + 1 < gRoute.size() && gCum[k] < s) k++;
    float t = (s - gCum[k - 1]) / (gCum[k] - gCum[k - 1]);
    if (dir) *dir = Vector2Normalize(Vector2Subtract(gRoute[k], gRoute[k - 1]));
    return Vector2Lerp(gRoute[k - 1], gRoute[k], t);
}
static float routeDist(float x, float z, float* sOut) {
    float best = 1e9f, bs = 0;
    for (size_t k = 1; k < gRoute.size(); k++) {
        float t;
        float d = distToSeg2D(x, z, gRoute[k - 1].x, gRoute[k - 1].y, gRoute[k].x, gRoute[k].y, &t);
        if (d < best) { best = d; bs = gCum[k - 1] + t * (gCum[k] - gCum[k - 1]); }
    }
    if (sOut) *sOut = bs;
    return best;
}
static float gradeAt(float s) {
    float f = clampf(s / GRADE_STEP, 0, (float)gGrade.size() - 1.001f);
    int i = (int)f;
    return lerpf(gGrade[i], gGrade[i + 1], f - i);
}
static void initRail() {
    gCum.assign(gRoute.size(), 0);
    for (size_t k = 1; k < gRoute.size(); k++) gCum[k] = gCum[k - 1] + Vector2Distance(gRoute[k - 1], gRoute[k]);
    int n = (int)(gCum.back() / GRADE_STEP) + 2;
    std::vector<float> raw(n), zs(n);
    for (int i = 0; i < n; i++) {
        Vector2 p = routeAt(i * GRADE_STEP, nullptr);
        raw[i] = std::max(rawHeight(p.x, p.y), 3.0f);
        zs[i] = p.y;
    }
    // smooth the grade on the Peninsula (trains don't like hills); follow the streets in the city
    gGrade.assign(n, 0);
    const int W = 30;
    for (int i = 0; i < n; i++) {
        float sum = 0;
        int c = 0;
        for (int k = std::max(0, i - W); k <= std::min(n - 1, i + W); k++) { sum += raw[k]; c++; }
        gGrade[i] = lerpf(raw[i] + 0.15f, sum / c, smooth01(1700, 1900, zs[i]));
    }
    // limit the Peninsula grade to 2.5%: the line cuts through hills instead of climbing them
    const float maxStep = 0.025f * GRADE_STEP;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 1; i < n; i++)
            if (zs[i] > 1850) gGrade[i] = clampf(gGrade[i], gGrade[i - 1] - maxStep, gGrade[i - 1] + maxStep);
        for (int i = n - 2; i >= 0; i--)
            if (zs[i] > 1850) gGrade[i] = clampf(gGrade[i], gGrade[i + 1] - maxStep, gGrade[i + 1] + maxStep);
    }
}

float analyticHeight(float x, float z) {
    float h = rawHeight(x, z);
    if (z > 1650 && x > 900 && x < 2900 && !gGrade.empty()) {
        float s;
        float d = routeDist(x, z, &s);
        if (d < 60) h = lerpf(h, gradeAt(s) - 0.4f, (1 - smooth01(12, 60, d)) * smooth01(1700, 1900, z));
    }
    return h;
}

float World::railLen() const { return gCum.back(); }
float World::railGrade(float s) const { return gradeAt(s); }
float World::railDistance(float x, float z, float* s) const { return routeDist(x, z, s); }
Vector3 World::trackPoint(float s, float lateral, Vector2* dir) const {
    Vector2 d;
    Vector2 p = routeAt(s, &d);
    if (dir) *dir = d;
    Vector2 r = {-d.y, d.x};
    return {p.x + r.x * lateral, gradeAt(s), p.y + r.y * lateral};
}

// ---------------------------------------------------------------- zones
struct Zone { float x0, z0, x1, z1, cx, cz, r; bool circle; int flags; };
static std::vector<Zone> gZones;
static void zRect(float x0, float z0, float x1, float z1, int f) { gZones.push_back({x0, z0, x1, z1, 0, 0, 0, false, f}); }
static void zCirc(float cx, float cz, float r, int f) { gZones.push_back({cx - r, cz - r, cx + r, cz + r, cx, cz, r, true, f}); }

static void initZones() {
    if (!gZones.empty()) return;
    zRect(-1600, -1300, gx(4), gz(4), Z_NOBUILD | Z_NOROAD | Z_GRASS | Z_TREES);          // Presidio
    zRect(-1600, gz(12), gx(8), gz(14), Z_NOBUILD | Z_NOROAD | Z_GRASS | Z_TREES);        // Golden Gate Park
    zRect(gx(9), gz(11), gx(10), gz(12), Z_NOBUILD | Z_GRASS);                            // Alamo Square
    zRect(gx(10), gz(11), gx(11), gz(12), Z_NOBUILD);                                     // Painted Ladies block
    zRect(gx(12), gz(17), gx(13), gz(18), Z_NOBUILD | Z_GRASS | Z_TREES);                 // Dolores Park
    zCirc(-295, 895, 175, Z_NOBUILD | Z_NOROAD | Z_SCRUB);                                // Twin Peaks
    zCirc(-520, 690, 120, Z_NOBUILD | Z_NOROAD | Z_GRASS | Z_TREES);                      // Mt Sutro
    zCirc(880, -880, 80, Z_NOBUILD | Z_GRASS | Z_TREES);                                  // Pioneer Park / Coit
    zRect(gx(19), gz(6), gx(20), gz(7), Z_NOBUILD | Z_PLAZA);                             // Union Square
    zRect(gx(23), gz(7), gx(24), gz(8), Z_NOBUILD | Z_PLAZA);                             // Embarcadero Plaza
    zRect(gx(14), gz(12), gx(15), gz(13), Z_NOBUILD | Z_PLAZA);                           // Civic Center Plaza
    zRect(gx(14), gz(13), gx(15), gz(14), Z_NOBUILD | Z_PLAZA);                           // City Hall
    zRect(gx(6), -1300, gx(11), gz(1), Z_NOBUILD | Z_NOROAD | Z_GRASS);                   // Marina Green
    zRect(gx(4), -1300, gx(6), -880, Z_NOBUILD | Z_NOROAD | Z_GRASS | Z_TREES);           // Palace of Fine Arts
    zRect(gx(23), gz(16), 1400, gz(18), Z_NOBUILD | Z_NOROAD | Z_PLAZA);                  // ballpark
    zRect(gx(11), gz(5), gx(12), gz(6), Z_NOBUILD | Z_GRASS | Z_TREES);                   // Lafayette Park
    zCirc(-430, 520, 85, Z_NOBUILD | Z_GRASS | Z_TREES);                                  // Buena Vista
    zCirc(350, 1550, 95, Z_NOBUILD | Z_NOROAD | Z_SCRUB);                                 // Bernal Heights
    zRect(gx(21), gz(5), gx(22), gz(6), Z_NOBUILD | Z_PLAZA);                             // Transamerica
    zRect(gx(22), gz(11), gx(23), gz(12), Z_NOBUILD | Z_PLAZA);                           // Salesforce
    zRect(1180, gz(18), gx(24), gz(19), Z_NOBUILD | Z_PLAZA);                             // Caltrain 4th & King
    zCirc(-500, 1300, 110, Z_NOBUILD | Z_NOROAD | Z_GRASS | Z_TREES);                     // Mt Davidson
    zRect(gx(20), gz(2), gx(21), gz(3), Z_NOBUILD | Z_GRASS | Z_TREES);                   // Washington Square
}

int World::zoneFlags(float x, float z, bool strict) const {
    float s = strict ? 1.5f : 0.f;
    int f = 0;
    for (auto& Z : gZones) {
        if (x < Z.x0 + s || x > Z.x1 - s || z < Z.z0 + s || z > Z.z1 - s) continue;
        if (Z.circle && sq(x - Z.cx) + sq(z - Z.cz) > sq(Z.r - s)) continue;
        f |= Z.flags;
    }
    return f;
}

bool marketDist(float x, float z, float* d) {
    float t;
    *d = distToSeg2D(x, z, MK_AX, MK_AZ, MK_BX, MK_BZ, &t);
    return t > 0 && t < 1;
}

int buildStyleAt(float x, float z) {
    if (x > 3900) {
        if (sq(x - 4380) + sq(z + 60) < sq(260)) return BS_OAK_DOWNTOWN;
        return BS_OAK;
    }
    if (x > 825 && z > -660 && z < 120) return BS_TOWERS;
    if (x > 1000 && z >= 120 && z < 700) return BS_TOWERS; // Rincon Hill / Transbay
    if (z < -985 && x > 300) return BS_WHARF;
    if (x > 385 && z >= 120 && z < 1150) return BS_MIDRISE;   // SoMa
    if (x > 600 && z > -660 && z < 120) return BS_MIDRISE;    // Union Square / Chinatown
    if (x > 385 && x <= 600 && z > -660 && z < 0) return BS_MIDRISE; // Nob Hill
    if (x > 50 && x < 385 && z > -200 && z < 600) return BS_MIDRISE;  // Civic Center / Hayes
    if (x < -600) return BS_SUNSET;
    if (x > -100 && z > 650) return BS_MISSION;
    return BS_ROWHOUSE;
}

const char* World::district(float x, float z) const {
    if (x > MX0 - 50 && x < MX1 + 50 && z > MZ0 - 50 && z < MZ1 + 50) {
        float s;
        float rd = railDistance(x, z, &s);
        if (!stations.empty() && rd < 30 && fabsf(s - stations.back().s) < 90) return "Menlo Park Caltrain";
        if (fabsf(x - mp.node(6, 0).x) < 14) return "El Camino Real";
        if (x > mp.node(3, 0).x && x < mp.node(7, 0).x && fabsf(z - mp.node(0, 4).y) < 150) return "Downtown Menlo Park";
        if (x > 3250) return "Menlo Park Bayfront";
        return x > 2430 ? "Linfield Oaks, Menlo Park" : "Central Menlo Park";
    }
    if (z > 880 && z < 1010 && x > 1180 && x < 1265) return "Caltrain 4th & King";
    if (x > -1120 && x < -1080 && z < -960 && z > -2340) return "Golden Gate Bridge";
    if (x > gx(23) && x < gx(24) - 4 && z > gz(7) && z < gz(8)) return "Embarcadero Plaza";
    if (x > gx(19) && x < gx(20) && z > gz(6) && z < gz(7)) return "Union Square";
    if (x > gx(14) && x < gx(15) && z > gz(12) && z < gz(14)) return "Civic Center";
    if (x > gx(16) && x < gx(17) && fabsf(z - gz(3)) < 12) return "Lombard Street";
    if (x > gx(9) - 8 && x < gx(11) && z > gz(11) - 8 && z < gz(12) + 8) return "Alamo Square";
    if (z > 315 && z < 355 && x > 1300 && x < 4000) return x > 2420 && x < 2820 ? "Yerba Buena Island" : "Bay Bridge";
    float h = terrain(x, z);
    if (h < 0.3f) {
        if (z < -1150 && x < -300) return "Golden Gate Strait";
        if (x < -1400) return "Pacific Ocean";
        return "San Francisco Bay";
    }
    if (x > AX0 + 50 && z > AZ0) return "SFO Airport";
    if (z < -2200) return "Marin Headlands";
    if (x > 3900) return "Oakland";
    if (x > 2400) return z < 50 ? "Treasure Island" : "Yerba Buena Island";
    if (x > 200 && x < 420 && z < -1800) return "Alcatraz";
    if (sq(x + 295) + sq(z - 895) < sq(200)) return "Twin Peaks";
    if (x > gx(23) && z > gz(16) && z < gz(18)) return "Oracle Park";
    if (x > 1255) return "The Embarcadero";
    if (x > 825 && z > -660 && z < 120) return "Financial District";
    if (sq(x - 880) + sq(z + 880) < sq(150)) return "Telegraph Hill";
    if (z < -985 && x > 300) return "Fisherman's Wharf";
    if (x > 600 && z < -660) return "North Beach";
    if (x > 600 && x <= 825 && z > -660 && z < -300) return "Chinatown";
    if (x > 600 && x <= 825 && z >= -300 && z < 120) return "Union Square";
    if (x > 385 && x <= 600 && z > -660 && z < 0) return "Nob Hill";
    if (x > 200 && x <= 600 && z <= -660) return "Russian Hill";
    if (x > 385 && z >= 120 && z < 1150) return "SoMa";
    if (x > 700 && z >= 1150) return "Potrero Hill";
    if (x < -935 && z < -655) return "The Presidio";
    if (x > -935 && x < -165 && z < -900) return "The Marina";
    if (x < -495 && z > gz(12) && z < gz(14)) return "Golden Gate Park";
    if (x < -500 && z < gz(12)) return "Richmond District";
    if (x < -500) return "Sunset District";
    if (z < -300) return "Pacific Heights";
    if (x > 50 && x < 385 && z > 100 && z < 600) return "Civic Center";
    if (x > -500 && x < -100 && z > 380 && z < 700) return "Haight-Ashbury";
    if (z < 380 && x < 50) return "Western Addition";
    if (x > -100 && z > 650 && z < 1150) return "The Mission";
    if (x <= -100 && z > 650 && z < 1100) return "The Castro";
    if (z >= 1100) return x > 0 ? "Bernal Heights" : "Noe Valley";
    return "Hayes Valley";
}

// ---------------------------------------------------------------- heightfield
constexpr float HF_RES = 4.0f;

void World::buildHeightfield() {
    initGeography();
    initZones();
    hfW = (int)((WX1 - WX0) / HF_RES) + 1;
    hfH = (int)((WZ1 - WZ0) / HF_RES) + 1;
    hf.assign((size_t)hfW * hfH, 0.f);
    unsigned nt = std::max(2u, std::thread::hardware_concurrency());
    std::vector<std::thread> th;
    for (unsigned t = 0; t < nt; t++) {
        th.emplace_back([this, t, nt]() {
            for (int j = (int)t; j < hfH; j += (int)nt)
                for (int i = 0; i < hfW; i++)
                    hf[(size_t)j * hfW + i] = analyticHeight(WX0 + i * HF_RES, WZ0 + j * HF_RES);
        });
    }
    for (auto& t : th) t.join();
    // Menlo Park patch
    hf2W = (int)((MX1 - MX0 + 800) / HF_RES) + 1;
    hf2H = (int)((MZ1 - MZ0 + 800) / HF_RES) + 1;
    hf2.assign((size_t)hf2W * hf2H, 0.f);
    th.clear();
    for (unsigned t = 0; t < nt; t++) {
        th.emplace_back([this, t, nt]() {
            for (int j = (int)t; j < hf2H; j += (int)nt)
                for (int i = 0; i < hf2W; i++)
                    hf2[(size_t)j * hf2W + i] = analyticHeight(MX0 - 400 + i * HF_RES, MZ0 - 400 + j * HF_RES);
        });
    }
    for (auto& t : th) t.join();
}

float World::terrain(float x, float z) const {
    float fx = (x - WX0) / HF_RES, fz = (z - WZ0) / HF_RES;
    if (fx < 0 || fz < 0 || fx >= hfW - 1 || fz >= hfH - 1) {
        float gx2 = (x - (MX0 - 400)) / HF_RES, gz2 = (z - (MZ0 - 400)) / HF_RES;
        if (gx2 >= 0 && gz2 >= 0 && gx2 < hf2W - 1 && gz2 < hf2H - 1 && !hf2.empty()) {
            int i = (int)gx2, j = (int)gz2;
            const float* r0 = &hf2[(size_t)j * hf2W + i];
            const float* r1 = r0 + hf2W;
            return lerpf(lerpf(r0[0], r0[1], gx2 - i), lerpf(r1[0], r1[1], gx2 - i), gz2 - j);
        }
        return analyticHeight(x, z);
    }
    int i = (int)fx, j = (int)fz;
    float tx = fx - i, tz = fz - j;
    const float* r0 = &hf[(size_t)j * hfW + i];
    const float* r1 = r0 + hfW;
    return lerpf(lerpf(r0[0], r0[1], tx), lerpf(r1[0], r1[1], tx), tz);
}

// ---------------------------------------------------------------- platforms / collision
constexpr float PG_CELL = 24.f;
constexpr float PG_X0 = PX0 - 100, PG_Z0 = PZ0 - 100;

int World::addPlat(const Platform& p) {
    if (pgrid.empty()) {
        pgW = (int)((PX1 + 100 - PG_X0) / PG_CELL) + 1;
        pgH = (int)((MZ1 + 100 - PG_Z0) / PG_CELL) + 1;
        pgrid.resize((size_t)pgW * pgH);
    }
    int idx = (int)plats.size();
    plats.push_back(p);
    int i0 = (int)floorf((p.x0 - PG_X0) / PG_CELL), i1 = (int)floorf((p.x1 - PG_X0) / PG_CELL);
    int j0 = (int)floorf((p.z0 - PG_Z0) / PG_CELL), j1 = (int)floorf((p.z1 - PG_Z0) / PG_CELL);
    i0 = std::max(i0, 0); j0 = std::max(j0, 0); i1 = std::min(i1, pgW - 1); j1 = std::min(j1, pgH - 1);
    for (int j = j0; j <= j1; j++)
        for (int i = i0; i <= i1; i++) pgrid[(size_t)j * pgW + i].push_back(idx);
    return idx;
}

float World::ground(float x, float z, float yRef) const {
    float h = terrain(x, z);
    int i = (int)floorf((x - PG_X0) / PG_CELL), j = (int)floorf((z - PG_Z0) / PG_CELL);
    if (i < 0 || j < 0 || i >= pgW || j >= pgH) return h;
    for (int idx : pgrid[(size_t)j * pgW + i]) {
        const Platform& p = plats[idx];
        if (!p.contains(x, z)) continue;
        float t = p.top(x, z);
        if (t <= yRef + STEP_UP && t > h) h = t;
    }
    for (auto& pd : ponds)
        if (x > pd.x0 && x < pd.x1 && z > pd.z0 && z < pd.z1) h = std::min(h, terrain(x, z) - 0.6f);
    return h;
}

bool World::collide(Vector3& p, float r, float feetY, float height, Vector2* nrm) const {
    bool hit = false;
    for (int iter = 0; iter < 3; iter++) {
        bool any = false;
        int i0 = (int)floorf((p.x - r - PG_X0) / PG_CELL), i1 = (int)floorf((p.x + r - PG_X0) / PG_CELL);
        int j0 = (int)floorf((p.z - r - PG_Z0) / PG_CELL), j1 = (int)floorf((p.z + r - PG_Z0) / PG_CELL);
        for (int j = std::max(j0, 0); j <= std::min(j1, pgH - 1); j++)
            for (int i = std::max(i0, 0); i <= std::min(i1, pgW - 1); i++)
                for (int idx : pgrid[(size_t)j * pgW + i]) {
                    const Platform& pl = plats[idx];
                    if (!pl.contains(p.x, p.z, r)) continue;
                    float cx = clampf(p.x, pl.x0, pl.x1), cz = clampf(p.z, pl.z0, pl.z1);
                    float t = pl.top(cx, cz);
                    if (t <= feetY + STEP_UP || pl.base >= feetY + height) continue;
                    float dx = p.x - cx, dz = p.z - cz;
                    float d2 = dx * dx + dz * dz;
                    Vector2 n;
                    float push;
                    if (d2 > 1e-8f) {
                        float d = sqrtf(d2);
                        if (d >= r) continue;
                        n = {dx / d, dz / d};
                        push = r - d;
                    } else {
                        // center inside the box: push out along the shallowest side
                        float l = p.x - pl.x0, rr = pl.x1 - p.x, f = p.z - pl.z0, b = pl.z1 - p.z;
                        float m = std::min(std::min(l, rr), std::min(f, b));
                        if (m == l) n = {-1, 0}; else if (m == rr) n = {1, 0}; else if (m == f) n = {0, -1}; else n = {0, 1};
                        push = m + r;
                    }
                    p.x += n.x * push;
                    p.z += n.y * push;
                    if (nrm) *nrm = n;
                    hit = any = true;
                }
        if (!any) break;
    }
    return hit;
}

bool World::solidAt(Vector3 p) const {
    if (terrain(p.x, p.z) > p.y - 0.25f) return true;
    int i = (int)floorf((p.x - PG_X0) / PG_CELL), j = (int)floorf((p.z - PG_Z0) / PG_CELL);
    if (i < 0 || j < 0 || i >= pgW || j >= pgH) return false;
    for (int idx : pgrid[(size_t)j * pgW + i]) {
        const Platform& pl = plats[idx];
        if (pl.kind != PK_BUILDING && pl.kind != PK_DECK) continue;
        if (pl.contains(p.x, p.z, 0.3f) && p.y < pl.top(p.x, p.z) + 0.3f && p.y > pl.base) return true;
    }
    return false;
}

float World::ceiling(float x, float z, float feetY) const {
    int i = (int)floorf((x - PG_X0) / PG_CELL), j = (int)floorf((z - PG_Z0) / PG_CELL);
    float best = 1e9f;
    if (i < 0 || j < 0 || i >= pgW || j >= pgH) return best;
    for (int idx : pgrid[(size_t)j * pgW + i]) {
        const Platform& pl = plats[idx];
        if (pl.kind != PK_BUILDING && pl.kind != PK_DECK) continue;
        if (!pl.contains(x, z) || pl.base < feetY + 1.0f) continue;
        best = std::min(best, pl.base);
    }
    return best;
}

static int doorCell(float x, float z) { return (int)floorf(x / 16.f) * 4096 + (int)floorf(z / 16.f); }

bool World::doorBlocked(float x, float z, float pad) const {
    if (doorWays.empty()) return false;
    for (int dx = -1; dx <= 1; dx++)
        for (int dz = -1; dz <= 1; dz++) {
            auto r = doorWays.equal_range(doorCell(x + dx * 16.f, z + dz * 16.f));
            for (auto it = r.first; it != r.second; ++it)
                if (distToSeg2D(x, z, it->second.x, it->second.y, it->second.z, it->second.w) < pad) return true;
        }
    return false;
}

int World::houseAt(float x, float z, float y) const {
    for (int i = 0; i < (int)houses.size(); i++) {
        const House& h = houses[i];
        if (x > h.x0 && x < h.x1 && z > h.z0 && z < h.z1 && y > h.floorY - 1.f && y < h.floorY + 3.f) return i;
    }
    return -1;
}

bool World::onBridge(float x, float z) const {
    return (x > -1115 && x < -1085 && z < -1050 && z > -2340) || (z > 318 && z < 352 && x > 1320 && x < 3990);
}

bool World::isWater(float x, float z, float y) const {
    for (auto& pd : ponds)
        if (x > pd.x0 && x < pd.x1 && z > pd.z0 && z < pd.z1 && y < terrain(x, z) + 0.2f) return true;
    return terrain(x, z) < 0.15f && y < 0.4f;
}

bool Grid::edge(int i, int j, int d) const {
    switch (d) {
    case 0: return in(i + 1, j) && ex[j * ni + i];
    case 1: return in(i - 1, j) && ex[j * ni + i - 1];
    case 2: return in(i, j + 1) && ez[j * ni + i];
    default: return in(i, j - 1) && ez[(j - 1) * ni + i];
    }
}
void Grid::setEdge(int i, int j, int d, bool v) {
    switch (d) {
    case 0: if (in(i + 1, j)) ex[j * ni + i] = v; break;
    case 1: if (in(i - 1, j)) ex[j * ni + i - 1] = v; break;
    case 2: if (in(i, j + 1)) ez[j * ni + i] = v; break;
    default: if (in(i, j - 1)) ez[(j - 1) * ni + i] = v; break;
    }
}

void World::buildGrid(Grid& g) {
    g.ok.assign(g.ni * g.nj, 0);
    g.ex.assign(g.ni * g.nj, 0);
    g.ez.assign(g.ni * g.nj, 0);
    for (int j = 0; j < g.nj; j++)
        for (int i = 0; i < g.ni; i++) {
            Vector2 p = g.node(i, j);
            bool ok = terrain(p.x, p.y) >= 0.9f && !(zoneFlags(p.x, p.y, true) & Z_NOROAD) &&
                      inPlay(p.x, p.y);
            g.ok[j * g.ni + i] = ok;
        }
    for (int j = 0; j < g.nj; j++)
        for (int i = 0; i < g.ni; i++) {
            if (!g.ok[j * g.ni + i]) continue;
            for (int d = 0; d < 4; d += 2) {
                int ni = i + DI[d], nj = j + DJ[d];
                if (!g.nodeOk(ni, nj)) continue;
                Vector2 a = g.node(i, j), b = g.node(ni, nj);
                bool ok = !(zoneFlags((a.x + b.x) / 2, (a.y + b.y) / 2, true) & Z_NOROAD);
                for (int k = 1; k < 6 && ok; k++) {
                    float t = k / 6.f;
                    if (terrain(lerpf(a.x, b.x, t), lerpf(a.y, b.y, t)) < 0.8f) ok = false;
                }
                g.setEdge(i, j, d, ok);
            }
        }
}

// ---------------------------------------------------------------- ground mesh
static float vnoise2(float x, float z, float cell, int seed) {
    float fx = x / cell, fz = z / cell;
    int i = (int)floorf(fx), j = (int)floorf(fz);
    float tx = fx - i, tz = fz - j;
    tx = tx * tx * (3 - 2 * tx); tz = tz * tz * (3 - 2 * tz);
    return lerpf(lerpf(hash01(i, j, seed), hash01(i + 1, j, seed), tx), lerpf(hash01(i, j + 1, seed), hash01(i + 1, j + 1, seed), tx), tz);
}

static Color groundColor(const World& w, float x, float z, float h, float slope) {
    if (h < 0.5f) return col(196, 182, 146, M_GROUND);
    if (x > MX0 - 400 && x < MX1 + 400 && z > MZ0 - 400 && z < MZ1 + 400) {
        float n2 = vnoise2(x, z, 60.f, 9);
        if (x > 3450) return mixC(col(150, 140, 96, M_GROUND), col(110, 120, 80, M_GROUND), n2); // bayside marsh
        return mixC(col(78, 132, 60, M_GROUND), col(104, 140, 68, M_GROUND), n2);                 // irrigated lawns
    }
    if (x > 1365 && x < 2715 && z > 1595 && z < 2555) // airfield grass, patchy
        return mixC(col(124, 146, 82, M_GROUND), col(168, 164, 104, M_GROUND), vnoise2(x, z, 90.f, 7));
    int zf = w.zoneFlags(x, z, false);
    float n = vnoise2(x, z, 140.f, 3) * 0.65f + vnoise2(x, z, 45.f, 5) * 0.35f;
    if (zf & Z_PLAZA) return C_PLAZA;
    if (zf & Z_SCRUB) return mixC(col(150, 140, 90, M_GROUND), col(105, 125, 70, M_GROUND), n);
    if (zf & Z_GRASS) return mixC(C_GRASS, col(80, 125, 62, M_GROUND), n);
    bool sfUrban = x > -1385 && x < 1272 && z > -1120 && z < 1830 && x < 1500;
    bool oakUrban = x > 4035 && x < 4700 && z > -1610 && z < 1800;
    if (x < 1500 && z > -1200 && z < 1900 && x <= -1385) return C_SAND; // Ocean Beach
    if (x >= 1272 && x < 1500 && z > -1000 && z < 1300) return C_PLAZA; // Embarcadero promenade
    if (x > 2400 && x < 2900 && z < 40 && z > -800) return C_LOT;        // Treasure Island
    if (sfUrban || oakUrban) {
        float x0 = sfUrban ? -1375.f : 4080.f, z0 = sfUrban ? -1095.f : -1565.f, sp = sfUrban ? 110.f : 95.f;
        float fx = (x - x0) / sp, fz = (z - z0) / sp;
        float dx = fabsf(x - x0 - roundf(fx) * sp), dz = fabsf(z - z0 - roundf(fz) * sp);
        float dl = std::min(dx, dz);
        float md;
        if (sfUrban && marketDist(x, z, &md) && md < MK_HALF + 4) return C_SIDEWALK;
        if (dl <= BLOCK_INSET + 0.5f) return C_SIDEWALK;
        int st = buildStyleAt(x, z);
        if ((st == BS_ROWHOUSE || st == BS_SUNSET || st == BS_MISSION || st == BS_OAK) && dl > 31) return C_YARD;
        return C_LOT;
    }
    // natural hills: golden grass with green patches, rock on steep slopes
    Color g = mixC(col(182, 160, 98, M_GROUND), col(96, 126, 66, M_GROUND), smooth01(0.3f, 0.7f, n));
    if (z < -2200 || x > 4700) g = mixC(g, col(110, 130, 70, M_GROUND), 0.3f);
    if (h > 180 && (z > 3300 || z < -4500)) g = mixC(g, col(62, 92, 58, M_GROUND), smooth01(0.35f, 0.75f, n)); // forested ridges
    if (slope > 0.75f) g = mixC(g, col(128, 116, 102, M_GROUND), smooth01(0.75f, 1.2f, slope));
    return g;
}

void World::buildTerrainMesh() {
    const float S = 10.f;
    int nx = (int)((WX1 - WX0) / S), nz = (int)((WZ1 - WZ0) / S);
    std::vector<float> H((size_t)(nx + 1) * (nz + 1));
    std::vector<Color> C((size_t)(nx + 1) * (nz + 1));
    for (int j = 0; j <= nz; j++)
        for (int i = 0; i <= nx; i++) H[(size_t)j * (nx + 1) + i] = terrain(WX0 + i * S, WZ0 + j * S);
    for (int j = 0; j <= nz; j++)
        for (int i = 0; i <= nx; i++) {
            float x = WX0 + i * S, z = WZ0 + j * S;
            float h = H[(size_t)j * (nx + 1) + i];
            float hx = H[(size_t)j * (nx + 1) + std::min(i + 1, nx)] - H[(size_t)j * (nx + 1) + std::max(i - 1, 0)];
            float hz = H[(size_t)std::min(j + 1, nz) * (nx + 1) + i] - H[(size_t)std::max(j - 1, 0) * (nx + 1) + i];
            float slope = sqrtf(hx * hx + hz * hz) / (2 * S);
            C[(size_t)j * (nx + 1) + i] = groundColor(*this, x, z, h, slope);
        }
    for (int j = 0; j < nz; j++)
        for (int i = 0; i < nx; i++) {
            size_t a = (size_t)j * (nx + 1) + i, b = a + 1, c = a + nx + 1, d = c + 1;
            float mx = std::max(std::max(H[a], H[b]), std::max(H[c], H[d]));
            if (mx < -1.0f) continue;
            float x0 = WX0 + i * S, z0 = WZ0 + j * S;
            MeshBuilder& m = mb(x0 + S / 2, z0 + S / 2);
            Vector3 pa = {x0, H[a], z0}, pb = {x0 + S, H[b], z0}, pc = {x0, H[c], z0 + S}, pd = {x0 + S, H[d], z0 + S};
            m.vert(pa, C[a]); m.vert(pb, C[b]); m.vert(pd, C[d]);
            m.vert(pa, C[a]); m.vert(pd, C[d]); m.vert(pc, C[c]);
        }
    // detailed ground around Menlo Park
    {
        const float S2 = 10.f, X0 = MX0 - 390, Z0 = MZ0 - 390;
        int mx = (int)((MX1 - MX0 + 780) / S2), mz = (int)((MZ1 - MZ0 + 780) / S2);
        std::vector<float> H2((size_t)(mx + 1) * (mz + 1));
        std::vector<Color> C2(H2.size());
        for (int j = 0; j <= mz; j++)
            for (int i = 0; i <= mx; i++) H2[(size_t)j * (mx + 1) + i] = terrain(X0 + i * S2, Z0 + j * S2);
        for (int j = 0; j <= mz; j++)
            for (int i = 0; i <= mx; i++) {
                size_t k = (size_t)j * (mx + 1) + i;
                float hx = H2[(size_t)j * (mx + 1) + std::min(i + 1, mx)] - H2[(size_t)j * (mx + 1) + std::max(i - 1, 0)];
                float hz = H2[(size_t)std::min(j + 1, mz) * (mx + 1) + i] - H2[(size_t)std::max(j - 1, 0) * (mx + 1) + i];
                C2[k] = groundColor(*this, X0 + i * S2, Z0 + j * S2, H2[k], sqrtf(hx * hx + hz * hz) / (2 * S2));
            }
        for (int j = 0; j < mz; j++)
            for (int i = 0; i < mx; i++) {
                size_t a = (size_t)j * (mx + 1) + i, b = a + 1, c = a + mx + 1, d = c + 1;
                float x0 = X0 + i * S2, z0 = Z0 + j * S2;
                MeshBuilder& m = mb(x0 + S2 / 2, z0 + S2 / 2);
                Vector3 pa = {x0, H2[a], z0}, pb = {x0 + S2, H2[b], z0}, pc = {x0, H2[c], z0 + S2}, pd = {x0 + S2, H2[d], z0 + S2};
                m.vert(pa, C2[a]); m.vert(pb, C2[b]); m.vert(pd, C2[d]);
                m.vert(pa, C2[a]); m.vert(pd, C2[d]); m.vert(pc, C2[c]);
            }
    }
    // the sea: one large quad, the shader animates it
    Color wc = col(30, 78, 98, M_WATER);
    waterB.quad({-40000, 0, -40000}, {40000, 0, -40000}, {40000, 0, 40000}, {-40000, 0, 40000}, wc);
}

// ---------------------------------------------------------------- distant mountains
// A coarse mesh straight from the terrain function, covering everything outside the
// detailed heightfield so the Peninsula, East Bay and Marin mountains frame the horizon.
void World::buildBackdrop() {
    const float S = 80.f, X0 = -16000, X1 = 20000, Z0 = -16000, Z1 = 16000;
    int nx = (int)((X1 - X0) / S), nz = (int)((Z1 - Z0) / S);
    std::vector<float> H((size_t)(nx + 1) * (nz + 1));
    std::vector<Color> C(H.size());
    for (int j = 0; j <= nz; j++)
        for (int i = 0; i <= nx; i++) {
            float x = X0 + i * S, z = Z0 + j * S;
            bool inside = x > WX0 + S && x < WX1 - S && z > WZ0 + S && z < WZ1 - S;
            H[(size_t)j * (nx + 1) + i] = inside ? terrain(x, z) : analyticHeight(x, z);
        }
    for (int j = 0; j <= nz; j++)
        for (int i = 0; i <= nx; i++) {
            size_t k = (size_t)j * (nx + 1) + i;
            float hx = H[(size_t)j * (nx + 1) + std::min(i + 1, nx)] - H[(size_t)j * (nx + 1) + std::max(i - 1, 0)];
            float hz = H[(size_t)std::min(j + 1, nz) * (nx + 1) + i] - H[(size_t)std::max(j - 1, 0) * (nx + 1) + i];
            C[k] = groundColor(*this, X0 + i * S, Z0 + j * S, H[k], sqrtf(hx * hx + hz * hz) / (2 * S));
        }
    for (int j = 0; j < nz; j++)
        for (int i = 0; i < nx; i++) {
            float x0 = X0 + i * S, z0 = Z0 + j * S;
            // skip cells fully covered by the detailed terrain
            if (x0 >= WX0 && x0 + S <= WX1 && z0 >= WZ0 && z0 + S <= WZ1) continue;
            if (x0 >= MX0 - 390 && x0 + S <= MX1 + 390 && z0 >= MZ0 - 390 && z0 + S <= MZ1 + 390) continue;
            size_t a = (size_t)j * (nx + 1) + i, b = a + 1, c = a + nx + 1, d = c + 1;
            if (std::max(std::max(H[a], H[b]), std::max(H[c], H[d])) < -1.0f) continue;
            MeshBuilder& m = mbKey(800000 + (j / 25) * 1000 + i / 25);
            Vector3 pa = {x0, H[a], z0}, pb = {x0 + S, H[b], z0}, pc = {x0, H[c], z0 + S}, pd = {x0 + S, H[d], z0 + S};
            m.vert(pa, C[a]); m.vert(pb, C[b]); m.vert(pd, C[d]);
            m.vert(pa, C[a]); m.vert(pd, C[d]); m.vert(pc, C[c]);
        }
}

// ---------------------------------------------------------------- minimap image
static void fillRect(Color* px, int W, int H, int x0, int y0, int x1, int y1, Color c) {
    x0 = std::max(x0, 0); y0 = std::max(y0, 0); x1 = std::min(x1, W - 1); y1 = std::min(y1, H - 1);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) px[y * W + x] = c;
}

constexpr float MAP_X0 = -1700, MAP_Z0 = -3300, MAP_RES = 4.f;

void World::buildMap() {
    buildMapImage(mapImg, MAP_X0, MAP_Z0, 4800, 2600, MAP_RES);
    buildMapImage(mapImg2, 1000, 8500, 3900, 11000, 4.f);
}

void World::buildMapImage(Image& img, float X0, float Z0, float X1, float Z1, float RES) {
    int W = (int)((X1 - X0) / RES), H = (int)((Z1 - Z0) / RES);
    Color* px = (Color*)MemAlloc(W * H * sizeof(Color));
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float wx = X0 + x * RES, wz = Z0 + y * RES;
            float h = terrain(wx, wz);
            Color c;
            if (h < 0.15f) c = mixC(Color{70, 130, 170, 255}, Color{40, 90, 135, 255}, clampf(-h / 10.f, 0, 1));
            else {
                Color g = groundColor(*this, wx, wz, h, 0);
                g = scaleC(g, 1.15f);
                g = scaleC(g, 0.85f + 0.15f * clampf(h / 120.f, 0, 1) + 0.0f);
                c = Color{g.r, g.g, g.b, 255};
                if (h > 40) c = scaleC(c, 1.0f + 0.002f * ((int)h % 20 < 2 ? -30 : 0)); // faint contour lines
            }
            px[y * W + x] = c;
        }
    auto toPx = [&](float wx, float wz, int& x, int& y) { x = (int)((wx - X0) / RES); y = (int)((wz - Z0) / RES); };
    Color road = {245, 245, 240, 255};
    for (const Grid* g : {&sf, &oak, &mp})
        for (int j = 0; j < g->nj; j++)
            for (int i = 0; i < g->ni; i++)
                for (int d = 0; d < 4; d += 2) {
                    if (!g->edge(i, j, d)) continue;
                    Vector2 a = g->node(i, j), b = g->node(i + DI[d], j + DJ[d]);
                    int x0, y0, x1, y1;
                    toPx(std::min(a.x, b.x) - g->roadHalf, std::min(a.y, b.y) - g->roadHalf, x0, y0);
                    toPx(std::max(a.x, b.x) + g->roadHalf, std::max(a.y, b.y) + g->roadHalf, x1, y1);
                    fillRect(px, W, H, x0, y0, x1, y1, road);
                }
    for (int k = 0; k <= 400; k++) { // Market Street
        float t = k / 400.f;
        int x, y;
        toPx(lerpf(MK_AX, MK_BX, t), lerpf(MK_AZ, MK_BZ, t), x, y);
        fillRect(px, W, H, x - 2, y - 2, x + 2, y + 2, road);
    }
    for (auto& p : plats) {
        Color c;
        if (p.kind == PK_BUILDING) c = {150, 145, 140, 255};
        else if (p.kind == PK_DECK) c = (p.x0 < -1000) ? Color{205, 70, 50, 255} : Color{170, 175, 180, 255};
        else if (p.kind == PK_PIER) c = {175, 160, 135, 255};
        else continue;
        int x0, y0, x1, y1;
        toPx(p.x0, p.z0, x0, y0);
        toPx(p.x1, p.z1, x1, y1);
        fillRect(px, W, H, x0, y0, x1, y1, c);
    }
    for (auto& P : tramPaths) // Muni Metro lines in red
        for (size_t k = 0; k + 1 < P.p.size(); k++)
            for (int s = 0; s <= 200; s++) {
                int x, y;
                toPx(lerpf(P.p[k].x, P.p[k + 1].x, s / 200.f), lerpf(P.p[k].y, P.p[k + 1].y, s / 200.f), x, y);
                fillRect(px, W, H, x - 1, y - 1, x, y, {205, 40, 50, 255});
            }
    for (auto& P : cablePaths)
        for (int s = 0; s <= 200; s++) {
            int x, y;
            toPx(lerpf(P.p[0].x, P.p[1].x, s / 200.f), lerpf(P.p[0].y, P.p[1].y, s / 200.f), x, y);
            fillRect(px, W, H, x - 1, y - 1, x, y, {130, 70, 40, 255});
        }
    for (float s = 0; s < railLen(); s += 3) { // Caltrain
        Vector3 p = trackPoint(s, 0, nullptr);
        int x, y;
        toPx(p.x, p.z, x, y);
        fillRect(px, W, H, x - 1, y - 1, x + 1, y + 1, ((int)(s / 30) % 2) ? Color{60, 60, 66, 255} : Color{220, 220, 220, 255});
    }
    for (auto& st : stations) {
        Vector3 p = trackPoint(st.s, 0, nullptr);
        int x, y;
        toPx(p.x, p.z, x, y);
        fillRect(px, W, H, x - 4, y - 18, x + 4, y + 18, {200, 40, 50, 255});
    }
    img.data = px;
    img.width = W;
    img.height = H;
    img.mipmaps = 1;
    img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
}

// ---------------------------------------------------------------- mesh chunks
constexpr float CHUNK = 400.f;

MeshBuilder& World::mb(float x, float z) {
    int cx = (int)floorf((x - WX0) / CHUNK), cz = (int)floorf((z - WZ0) / CHUNK);
    return builders[cz * 1000 + cx];
}

void World::upload() {
    auto make = [](MeshBuilder& b) {
        Chunk ch{};
        size_t n = b.count();
        ch.mesh.vertexCount = (int)n;
        ch.mesh.triangleCount = (int)(n / 3);
        ch.mesh.vertices = (float*)MemAlloc((unsigned)(n * 3 * sizeof(float)));
        ch.mesh.colors = (unsigned char*)MemAlloc((unsigned)(n * 4));
        memcpy(ch.mesh.vertices, b.v.data(), n * 3 * sizeof(float));
        memcpy(ch.mesh.colors, b.c.data(), n * 4);
        ch.mn = {1e9f, 1e9f, 1e9f};
        ch.mx = {-1e9f, -1e9f, -1e9f};
        for (size_t i = 0; i < n; i++) {
            Vector3 p = {b.v[i * 3], b.v[i * 3 + 1], b.v[i * 3 + 2]};
            ch.mn = Vector3Min(ch.mn, p);
            ch.mx = Vector3Max(ch.mx, p);
        }
        UploadMesh(&ch.mesh, false);
        return ch;
    };
    for (auto& kv : builders) {
        if (kv.second.count() < 3) continue;
        chunks.push_back(make(kv.second));
        MeshBuilder().v.swap(kv.second.v);
        MeshBuilder().c.swap(kv.second.c);
    }
    builders.clear();
    waterMesh = make(waterB).mesh;
}

void World::drawPass(const Camera3D& cam, float nearD, float farD, const Material& mat) {
    Vector3 fwd = Vector3Normalize(Vector3Subtract(cam.target, cam.position));
    for (auto& ch : chunks) {
        Vector3 c = Vector3Scale(Vector3Add(ch.mn, ch.mx), 0.5f);
        float rad = Vector3Distance(ch.mn, ch.mx) * 0.5f;
        Vector3 d = Vector3Subtract(c, cam.position);
        if (Vector3DotProduct(d, fwd) < -rad) continue;
        // nearest and farthest distance from camera to the chunk box
        Vector3 q = {std::max(std::max(ch.mn.x - cam.position.x, 0.f), cam.position.x - ch.mx.x),
                     std::max(std::max(ch.mn.y - cam.position.y, 0.f), cam.position.y - ch.mx.y),
                     std::max(std::max(ch.mn.z - cam.position.z, 0.f), cam.position.z - ch.mx.z)};
        float nearest = Vector3Length(q);
        if (nearest > farD) continue;
        Vector3 f = {std::max(fabsf(ch.mn.x - cam.position.x), fabsf(ch.mx.x - cam.position.x)),
                     std::max(fabsf(ch.mn.y - cam.position.y), fabsf(ch.mx.y - cam.position.y)),
                     std::max(fabsf(ch.mn.z - cam.position.z), fabsf(ch.mx.z - cam.position.z))};
        if (Vector3Length(f) < nearD) continue;
        DrawMesh(ch.mesh, mat, MatrixIdentity());
    }
}

void World::drawWater(const Material& mat) { DrawMesh(waterMesh, mat, MatrixIdentity()); }

