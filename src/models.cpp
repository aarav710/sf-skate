#include "models.h"
#include "instance.h"

static const Color GLASS = col(38, 46, 58);
static const Color TIRE = col(22, 22, 24);
static const Color HEAD = col(255, 248, 225, M_EMISSIVE);
static const Color TAIL = col(200, 25, 20, M_EMISSIVE);
static const Color BUMP = col(70, 72, 76);

static void addWheels(std::vector<LBox>& b, float hx, float hz, float r) {
    for (int i = 0; i < 4; i++) {
        float x = (i & 1) ? hx : -hx, z = (i & 2) ? hz : -hz;
        b.push_back({{x, r, z}, {r * 2, r * 2, 0.28f}, TIRE, 0});
    }
}

static std::vector<CarModel> buildModels() {
    std::vector<CarModel> m(CT_COUNT);
    {   // sedan
        auto& b = m[CT_SEDAN].boxes;
        b.push_back({{0, 0.62f, 0}, {4.4f, 0.62f, 1.8f}, col(255, 255, 255), 1});
        b.push_back({{-0.25f, 1.17f, 0}, {2.3f, 0.5f, 1.62f}, GLASS, 0});
        b.push_back({{-0.25f, 1.44f, 0}, {2.1f, 0.07f, 1.55f}, col(255, 255, 255), 1});
        b.push_back({{2.21f, 0.5f, 0}, {0.1f, 0.22f, 1.8f}, BUMP, 0});
        b.push_back({{-2.21f, 0.5f, 0}, {0.1f, 0.22f, 1.8f}, BUMP, 0});
        b.push_back({{2.22f, 0.72f, 0.6f}, {0.06f, 0.14f, 0.38f}, HEAD, 0});
        b.push_back({{2.22f, 0.72f, -0.6f}, {0.06f, 0.14f, 0.38f}, HEAD, 0});
        b.push_back({{-2.22f, 0.74f, 0.65f}, {0.06f, 0.14f, 0.32f}, TAIL, 0});
        b.push_back({{-2.22f, 0.74f, -0.65f}, {0.06f, 0.14f, 0.32f}, TAIL, 0});
        addWheels(b, 1.35f, 0.8f, 0.34f);
        m[CT_SEDAN].halfLen = 2.25f; m[CT_SEDAN].halfWid = 0.92f; m[CT_SEDAN].height = 1.5f;
    }
    {   // SUV
        auto& b = m[CT_SUV].boxes;
        b.push_back({{0, 0.78f, 0}, {4.7f, 0.85f, 1.92f}, col(255, 255, 255), 1});
        b.push_back({{-0.3f, 1.48f, 0}, {3.0f, 0.58f, 1.8f}, GLASS, 0});
        b.push_back({{-0.3f, 1.8f, 0}, {2.9f, 0.08f, 1.75f}, col(255, 255, 255), 1});
        b.push_back({{2.36f, 0.82f, 0.62f}, {0.06f, 0.16f, 0.4f}, HEAD, 0});
        b.push_back({{2.36f, 0.82f, -0.62f}, {0.06f, 0.16f, 0.4f}, HEAD, 0});
        b.push_back({{-2.36f, 0.9f, 0.7f}, {0.06f, 0.2f, 0.3f}, TAIL, 0});
        b.push_back({{-2.36f, 0.9f, -0.7f}, {0.06f, 0.2f, 0.3f}, TAIL, 0});
        addWheels(b, 1.5f, 0.85f, 0.4f);
        m[CT_SUV].halfLen = 2.4f; m[CT_SUV].halfWid = 0.98f; m[CT_SUV].height = 1.85f;
    }
    {   // delivery van
        auto& b = m[CT_VAN].boxes;
        b.push_back({{-0.3f, 1.25f, 0}, {4.4f, 2.0f, 2.0f}, col(255, 255, 255), 1});
        b.push_back({{2.3f, 0.85f, 0}, {1.0f, 1.1f, 1.95f}, col(255, 255, 255), 1});
        b.push_back({{2.1f, 1.55f, 0}, {0.6f, 0.55f, 1.9f}, GLASS, 0});
        b.push_back({{2.82f, 0.8f, 0.65f}, {0.06f, 0.16f, 0.36f}, HEAD, 0});
        b.push_back({{2.82f, 0.8f, -0.65f}, {0.06f, 0.16f, 0.36f}, HEAD, 0});
        b.push_back({{-2.52f, 0.9f, 0.8f}, {0.06f, 0.24f, 0.25f}, TAIL, 0});
        b.push_back({{-2.52f, 0.9f, -0.8f}, {0.06f, 0.24f, 0.25f}, TAIL, 0});
        addWheels(b, 1.7f, 0.88f, 0.38f);
        m[CT_VAN].halfLen = 2.85f; m[CT_VAN].halfWid = 1.02f; m[CT_VAN].height = 2.3f;
    }
    {   // taxi = sedan with a roof sign
        m[CT_TAXI] = m[CT_SEDAN];
        m[CT_TAXI].boxes.push_back({{-0.25f, 1.6f, 0}, {0.7f, 0.25f, 0.3f}, col(255, 240, 200, M_EMISSIVE), 0});
    }
    {   // Muni bus (white with red stripe)
        auto& b = m[CT_BUS].boxes;
        b.push_back({{0, 1.75f, 0}, {12.0f, 2.7f, 2.55f}, col(236, 236, 232), 0});
        b.push_back({{0, 2.2f, 0}, {11.6f, 0.9f, 2.6f}, GLASS, 0});
        b.push_back({{0, 0.75f, 0}, {12.02f, 0.35f, 2.57f}, col(190, 30, 40), 0});
        b.push_back({{0, 3.15f, 0}, {10.0f, 0.12f, 2.4f}, col(200, 200, 200), 0});
        b.push_back({{6.02f, 1.0f, 0.9f}, {0.06f, 0.2f, 0.4f}, HEAD, 0});
        b.push_back({{6.02f, 1.0f, -0.9f}, {0.06f, 0.2f, 0.4f}, HEAD, 0});
        b.push_back({{6.02f, 3.0f, 0}, {0.06f, 0.25f, 1.6f}, col(255, 160, 40, M_EMISSIVE), 0});
        b.push_back({{-6.02f, 1.0f, 1.0f}, {0.06f, 0.3f, 0.3f}, TAIL, 0});
        b.push_back({{-6.02f, 1.0f, -1.0f}, {0.06f, 0.3f, 0.3f}, TAIL, 0});
        addWheels(b, 4.0f, 1.05f, 0.5f);
        m[CT_BUS].halfLen = 6.0f; m[CT_BUS].halfWid = 1.3f; m[CT_BUS].height = 3.2f;
    }
    {   // cable car: maroon/cream with open ends
        auto& b = m[CT_CABLECAR].boxes;
        b.push_back({{0, 0.75f, 0}, {8.4f, 0.5f, 2.4f}, col(120, 30, 35), 0});
        b.push_back({{0, 1.0f, 0}, {8.6f, 0.08f, 2.5f}, col(60, 40, 30), 0});
        // closed cabin in the middle, with open windows so the riders inside show
        Color cream = col(230, 215, 170), wood = col(105, 70, 45);
        b.push_back({{0, 1.35f, 0}, {4.2f, 0.6f, 2.3f}, cream, 0});
        b.push_back({{0, 2.69f, 0}, {4.2f, 0.28f, 2.3f}, cream, 0});
        b.push_back({{0, 1.65f, 0}, {4.26f, 0.3f, 2.33f}, col(120, 30, 35), 0});
        for (int e = -1; e <= 1; e += 2) b.push_back({{e * 2.08f, 2.12f, 0}, {0.06f, 0.95f, 2.3f}, GLASS, 0});
        for (int k = -3; k <= 3; k++)
            for (int s = -1; s <= 1; s += 2) b.push_back({{k * 0.69f, 2.12f, s * 1.13f}, {0.08f, 0.95f, 0.06f}, cream, 0});
        // open ends: back-to-back benches facing out, plus running boards along both sides
        for (int e = -1; e <= 1; e += 2) {
            b.push_back({{e * 3.5f, 1.27f, 0}, {1.2f, 0.45f, 0.5f}, wood, 0});
            b.push_back({{e * 3.5f, 1.5f, 0}, {1.3f, 0.07f, 1.0f}, wood, 0});
            b.push_back({{e * 3.5f, 1.82f, 0}, {1.3f, 0.6f, 0.07f}, wood, 0});
        }
        for (int s = -1; s <= 1; s += 2) b.push_back({{0, 0.62f, s * 1.42f}, {8.0f, 0.07f, 0.4f}, wood, 0});
        b.push_back({{0, 2.95f, 0}, {8.8f, 0.25f, 2.6f}, col(110, 30, 32), 0});
        for (int i = -1; i <= 1; i += 2)
            for (int s = -1; s <= 1; s += 2)
                for (float x : {2.6f, 3.4f}) b.push_back({{i * x, 1.95f, s * 1.1f}, {0.1f, 1.9f, 0.1f}, col(200, 170, 80), 0});
        b.push_back({{4.25f, 1.4f, 0}, {0.06f, 0.25f, 0.25f}, HEAD, 0});
        b.push_back({{-4.25f, 1.4f, 0}, {0.06f, 0.25f, 0.25f}, HEAD, 0});
        addWheels(b, 2.6f, 0.8f, 0.4f);
        m[CT_CABLECAR].halfLen = 4.3f; m[CT_CABLECAR].halfWid = 1.25f; m[CT_CABLECAR].height = 3.1f;
    }
    {   // F-line historic streetcar (body color varies)
        auto& b = m[CT_STREETCAR].boxes;
        b.push_back({{0, 1.6f, 0}, {14.0f, 2.4f, 2.6f}, col(255, 255, 255), 1});
        b.push_back({{0, 2.1f, 0}, {13.6f, 0.85f, 2.65f}, col(250, 240, 210), 0});
        b.push_back({{0, 2.12f, 0}, {13.0f, 0.65f, 2.67f}, GLASS, 0});
        b.push_back({{0, 3.0f, 0}, {13.0f, 0.35f, 2.3f}, col(255, 255, 255), 2});
        b.push_back({{0, 3.9f, 0}, {0.1f, 1.6f, 0.1f}, col(40, 40, 40), 0}); // trolley pole
        b.push_back({{7.02f, 1.2f, 0}, {0.06f, 0.3f, 0.3f}, HEAD, 0});
        b.push_back({{-7.02f, 1.2f, 0}, {0.06f, 0.3f, 0.3f}, HEAD, 0});
        addWheels(b, 4.5f, 0.9f, 0.45f);
        m[CT_STREETCAR].halfLen = 7.0f; m[CT_STREETCAR].halfWid = 1.3f; m[CT_STREETCAR].height = 3.4f;
    }
    {   // Muni Metro light rail vehicle: silver with a red stripe and a pantograph
        auto& b = m[CT_LRV].boxes;
        Color silver = col(205, 207, 212), red = col(196, 30, 40);
        b.push_back({{0, 1.95f, 0}, {22.0f, 2.8f, 2.65f}, silver, 0});
        b.push_back({{0, 2.4f, 0}, {21.4f, 1.0f, 2.7f}, GLASS, 0});
        b.push_back({{0, 1.05f, 0}, {22.04f, 0.42f, 2.68f}, red, 0});
        b.push_back({{0, 1.95f, 0}, {0.7f, 2.7f, 2.66f}, col(40, 40, 44), 0}); // articulation
        b.push_back({{0, 3.45f, 0}, {9.0f, 0.3f, 1.8f}, col(120, 120, 125), 0});
        b.push_back({{0, 3.95f, 0}, {0.12f, 0.9f, 0.12f}, col(40, 40, 40), 0});
        b.push_back({{0, 4.4f, 0}, {1.4f, 0.08f, 1.8f}, col(40, 40, 40), 0});
        for (int e = -1; e <= 1; e += 2) {
            b.push_back({{e * 11.03f, 1.3f, 0.8f}, {0.06f, 0.2f, 0.4f}, HEAD, 0});
            b.push_back({{e * 11.03f, 1.3f, -0.8f}, {0.06f, 0.2f, 0.4f}, HEAD, 0});
            b.push_back({{e * 11.03f, 3.1f, 0}, {0.06f, 0.3f, 1.4f}, col(255, 170, 40, M_EMISSIVE), 0});
        }
        addWheels(b, 8.0f, 0.95f, 0.42f);
        b.push_back({{0, 0.42f, 0.95f}, {0.84f, 0.84f, 0.28f}, TIRE, 0});
        b.push_back({{0, 0.42f, -0.95f}, {0.84f, 0.84f, 0.28f}, TIRE, 0});
        m[CT_LRV].halfLen = 11.0f; m[CT_LRV].halfWid = 1.35f; m[CT_LRV].height = 3.6f;
    }
    {   // airliner (local x = nose direction); body color is the livery
        auto& b = m[CT_PLANE].boxes;
        Color w = col(240, 240, 238), g = col(185, 188, 194), dark = col(70, 72, 78);
        b.push_back({{0, 3.5f, 0}, {36.0f, 4.0f, 4.0f}, w, 0});
        b.push_back({{18.6f, 3.3f, 0}, {1.6f, 3.0f, 3.2f}, w, 0});
        b.push_back({{19.6f, 3.1f, 0}, {0.8f, 2.0f, 2.2f}, w, 0});
        b.push_back({{18.7f, 4.35f, 0}, {1.0f, 0.55f, 3.0f}, GLASS, 0});
        b.push_back({{-0.5f, 4.35f, 0}, {32.0f, 0.4f, 4.04f}, GLASS, 0});
        b.push_back({{0, 2.3f, 0}, {35.0f, 0.7f, 4.04f}, w, 1});
        b.push_back({{-19.0f, 3.9f, 0}, {3.0f, 2.6f, 2.6f}, w, 0});
        b.push_back({{0, 2.5f, 0}, {7.0f, 0.5f, 34.0f}, g, 0});
        for (int s = -1; s <= 1; s += 2) {
            b.push_back({{1.5f, 1.55f, s * 7.0f}, {4.2f, 1.9f, 1.9f}, g, 0});
            b.push_back({{3.65f, 1.55f, s * 7.0f}, {0.1f, 1.5f, 1.5f}, dark, 0});
            b.push_back({{-1.5f, 2.62f, s * 16.6f}, {1.5f, 0.12f, 0.3f}, s > 0 ? col(40, 220, 60, M_EMISSIVE) : col(230, 30, 30, M_EMISSIVE), 0});
        }
        b.push_back({{-17.0f, 8.0f, 0}, {5.0f, 7.5f, 0.45f}, w, 1});
        b.push_back({{-17.5f, 4.4f, 0}, {3.4f, 0.3f, 12.0f}, g, 0});
        b.push_back({{14.0f, 0.75f, 0}, {0.4f, 1.5f, 0.4f}, dark, 0});
        b.push_back({{-1.0f, 0.75f, 2.5f}, {0.5f, 1.5f, 0.5f}, dark, 0});
        b.push_back({{-1.0f, 0.75f, -2.5f}, {0.5f, 1.5f, 0.5f}, dark, 0});
        m[CT_PLANE].halfLen = 19.0f; m[CT_PLANE].halfWid = 2.0f; m[CT_PLANE].height = 5.5f;
    }
    {   // SFO AirTrain: three white cars with a blue window band
        auto& b = m[CT_AIRTRAIN].boxes;
        for (int k = -1; k <= 1; k++) {
            b.push_back({{k * 12.0f, 1.7f, 0}, {11.4f, 3.0f, 2.8f}, col(240, 240, 240), 0});
            b.push_back({{k * 12.0f, 2.1f, 0}, {11.0f, 1.2f, 2.85f}, GLASS, 0});
            b.push_back({{k * 12.0f, 1.0f, 0}, {11.42f, 0.3f, 2.82f}, col(30, 80, 170), 0});
        }
        b.push_back({{18.0f, 2.0f, 0}, {0.1f, 0.3f, 1.6f}, HEAD, 0});
        b.push_back({{-18.0f, 2.0f, 0}, {0.1f, 0.3f, 1.6f}, HEAD, 0});
        m[CT_AIRTRAIN].halfLen = 18; m[CT_AIRTRAIN].halfWid = 1.4f; m[CT_AIRTRAIN].height = 3.2f;
    }
    {   // Caltrain electric bilevel car (white/grey with a red stripe); the cab car adds a driver's end at +x
        auto& b = m[CT_CALTRAIN].boxes;
        Color body = col(226, 227, 229), red = col(200, 30, 40), dark = col(60, 62, 66);
        b.push_back({{0, 2.85f, 0}, {25.4f, 4.3f, 3.1f}, body, 0});
        b.push_back({{0, 2.3f, 0}, {24.4f, 0.95f, 3.14f}, GLASS, 0});
        b.push_back({{0, 3.85f, 0}, {24.4f, 0.95f, 3.14f}, GLASS, 0});
        b.push_back({{0, 1.2f, 0}, {25.42f, 0.45f, 3.12f}, red, 0});
        b.push_back({{0, 5.05f, 0}, {24.0f, 0.2f, 2.8f}, col(150, 150, 155), 0});
        b.push_back({{0, 0.85f, 0}, {25.0f, 0.5f, 2.9f}, dark, 0});
        for (int e = -1; e <= 1; e += 2) {
            b.push_back({{e * 9.0f, 0.5f, 0}, {3.2f, 0.8f, 2.6f}, dark, 0});
            b.push_back({{e * 12.75f, 2.8f, 0}, {0.1f, 2.4f, 1.0f}, col(90, 92, 96), 0}); // end doors
        }
        m[CT_CALTRAIN_CAB] = m[CT_CALTRAIN];
        auto& c = m[CT_CALTRAIN_CAB].boxes;
        c.push_back({{12.95f, 2.6f, 0}, {0.6f, 3.6f, 2.9f}, red, 0});
        c.push_back({{13.28f, 3.7f, 0}, {0.1f, 1.2f, 2.4f}, GLASS, 0});
        c.push_back({{13.3f, 1.5f, 1.0f}, {0.06f, 0.3f, 0.5f}, HEAD, 0});
        c.push_back({{13.3f, 1.5f, -1.0f}, {0.06f, 0.3f, 0.5f}, HEAD, 0});
        c.push_back({{6.0f, 5.45f, 0}, {2.4f, 0.1f, 1.6f}, dark, 0}); // pantograph
        c.push_back({{6.0f, 5.2f, 0}, {0.12f, 0.5f, 0.12f}, dark, 0});
        for (int k : {CT_CALTRAIN, CT_CALTRAIN_CAB}) { m[k].halfLen = 12.9f; m[k].halfWid = 1.55f; m[k].height = 5.0f; }
    }
    {   // pedal bus ("pedal pub"): an open frame under a canopy, stools round a central bar, a driver up front.
        // The riders and their pedalling legs are drawn by Traffic, since they move.
        auto& b = m[CT_PEDALBUS].boxes;
        Color frame = col(42, 42, 46), wood = col(150, 98, 54), seat = col(30, 30, 32);
        b.push_back({{0, 0.55f, 0}, {6.2f, 0.16f, 2.0f}, frame, 0});                       // chassis
        b.push_back({{-0.2f, 0.88f, 0}, {4.0f, 0.62f, 0.42f}, col(255, 255, 255), 1});    // bar base
        b.push_back({{-0.2f, 1.22f, 0}, {4.3f, 0.1f, 0.78f}, wood, 0});                    // bar top
        for (int k = 0; k < 5; k++)
            for (int sd = -1; sd <= 1; sd += 2) {
                float x = -2.0f + k * 0.9f;
                b.push_back({{x, 0.82f, sd * 0.98f}, {0.06f, 0.4f, 0.06f}, frame, 0});      // seat post
                b.push_back({{x, 1.04f, sd * 0.98f}, {0.32f, 0.08f, 0.3f}, seat, 0});      // saddle
                b.push_back({{x, 0.6f, sd * 0.62f}, {0.08f, 0.08f, 0.08f}, frame, 0});     // crank hub
            }
        for (int i = 0; i < 4; i++) {
            float x = (i & 1) ? 2.95f : -2.95f, z = (i & 2) ? 1.02f : -1.02f;
            b.push_back({{x, 1.62f, z}, {0.08f, 2.0f, 0.08f}, frame, 0});                  // canopy posts
        }
        b.push_back({{0, 2.66f, 0}, {6.5f, 0.1f, 2.36f}, col(255, 255, 255), 1});          // canopy
        b.push_back({{0, 2.5f, 1.17f}, {6.5f, 0.22f, 0.04f}, col(255, 255, 255), 2});      // valance
        b.push_back({{0, 2.5f, -1.17f}, {6.5f, 0.22f, 0.04f}, col(255, 255, 255), 2});
        b.push_back({{3.12f, 0.92f, 0}, {0.14f, 0.6f, 1.9f}, col(255, 255, 255), 2});      // front panel
        b.push_back({{2.55f, 1.0f, 0}, {0.4f, 0.1f, 0.4f}, seat, 0});                      // driver's seat
        b.push_back({{2.95f, 1.3f, 0}, {0.06f, 0.06f, 0.7f}, frame, 0});                   // handlebar
        b.push_back({{2.95f, 1.0f, 0}, {0.06f, 0.6f, 0.06f}, frame, 0});
        b.push_back({{-2.85f, 0.95f, 0}, {0.55f, 0.14f, 1.8f}, wood, 0});                  // rear bench
        b.push_back({{-3.1f, 1.3f, 0}, {0.08f, 0.6f, 1.8f}, wood, 0});
        b.push_back({{3.2f, 0.95f, 0.7f}, {0.06f, 0.14f, 0.3f}, HEAD, 0});
        b.push_back({{3.2f, 0.95f, -0.7f}, {0.06f, 0.14f, 0.3f}, HEAD, 0});
        b.push_back({{-3.18f, 0.7f, 0.85f}, {0.06f, 0.14f, 0.2f}, TAIL, 0});
        b.push_back({{-3.18f, 0.7f, -0.85f}, {0.06f, 0.14f, 0.2f}, TAIL, 0});
        addWheels(b, 2.4f, 1.0f, 0.42f);
        m[CT_PEDALBUS].halfLen = 3.25f; m[CT_PEDALBUS].halfWid = 1.18f; m[CT_PEDALBUS].height = 2.7f;
    }
    return m;
}

const CarModel& carModel(int type) {
    static std::vector<CarModel> models = buildModels();
    return models[type];
}

Color randomCarColor(RNG& r) {
    static const Color pal[] = {col(235, 235, 235), col(25, 25, 28), col(150, 152, 158), col(120, 20, 25),
                                col(30, 60, 120), col(200, 200, 205), col(60, 70, 60), col(90, 30, 40),
                                col(210, 120, 30), col(40, 110, 140), col(240, 240, 240), col(70, 70, 75)};
    return pal[r.irange(0, 11)];
}

void addCarToMesh(MeshBuilder& mb, int type, Color body, Vector3 pos, float yaw, float pitch) {
    const CarModel& m = carModel(type);
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch);
    Vector3 f = {cp * cy, sp, cp * sy}, r = {-sy, 0, cy};
    Vector3 u = Vector3CrossProduct(r, f);
    for (auto& b : m.boxes) {
        Vector3 c = {pos.x + f.x * b.c.x + u.x * b.c.y + r.x * b.c.z, pos.y + f.y * b.c.x + u.y * b.c.y + r.y * b.c.z,
                     pos.z + f.z * b.c.x + u.z * b.c.y + r.z * b.c.z};
        Color k = boxColor(b, body);
        if (k.a == M_EMISSIVE) k.a = M_PLAIN; // parked cars have their lights off
        mb.obox(c, Vector3Scale(b.s, 0.5f), yaw, pitch, k);
    }
}

// queued as instances (see instance.h), drawn by inst::flush at the end of the pass
void drawCarModel(int type, Color body, Vector3 pos, float yaw, float pitch, bool brake) {
    const CarModel& m = carModel(type);
    inst::push();
    inst::translate(pos.x, pos.y, pos.z);
    inst::rotate(-yaw * RAD2DEG, 0, 1, 0);
    inst::rotate(pitch * RAD2DEG, 0, 0, 1);
    for (auto& b : m.boxes) {
        Color k = boxColor(b, body);
        if (brake && b.k.r == 200 && b.k.g == 25) k = col(255, 40, 30, M_EMISSIVE);
        inst::cube(b.c, b.s.x, b.s.y, b.s.z, k);
    }
    inst::pop();
}
