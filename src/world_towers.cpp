// Downtown towers with the shapes San Francisco is known for: setback Art Deco shafts with copper crowns,
// faceted granite towers with bay windows, glass slabs with sloped crowns, mid-century slabs, slender
// glass needles and old brick high-rises with water tanks.
#include "world_internal.h"

namespace {
const Color DECO[] = {col(226, 212, 182), col(206, 184, 142), col(198, 146, 112), col(216, 212, 200), col(204, 164, 150)};
const Color GRANITE[] = {col(112, 64, 58), col(94, 74, 64), col(72, 74, 82), col(130, 84, 70)};
const Color BRICK[] = {col(150, 88, 70), col(172, 120, 96), col(132, 92, 80), col(186, 150, 118)};
const Color GLASS[] = {col(72, 98, 124), col(98, 122, 142), col(62, 84, 96), col(120, 144, 160), col(86, 116, 112)};
const Color COPPER = col(98, 154, 130), GILT = col(214, 176, 78), STEEL = col(150, 154, 160);

Color mat(Color c, unsigned char m) { return withMat(c, m); }
}

// kinds
enum { TK_DECO, TK_FACETED, TK_GLASS, TK_MIDCENTURY, TK_NEEDLE, TK_BRICK };

void World::addTower(float x0, float z0, float x1, float z1, float Hmax, RNG& r) {
    RNG tr(hashu((int)(x0 * 4), (int)(z0 * 4), 77));  // local variety; the global stream is left alone
    float hmax = std::max(std::max(terrain(x0, z0), terrain(x1, z0)), std::max(terrain(x0, z1), terrain(x1, z1)));
    float hmin = std::min(std::min(terrain(x0, z0), terrain(x1, z0)), std::min(terrain(x0, z1), terrain(x1, z1)));
    float W = x1 - x0, D = z1 - z0, cx = (x0 + x1) / 2, cz = (z0 + z1) / 2;
    MeshBuilder& m = mb(cx, cz);

    float H = Hmax;
    // pick a design by height and lot shape
    int kind;
    float roll = tr.f();
    if (H < 60) kind = roll < 0.45f ? TK_BRICK : (roll < 0.8f ? TK_MIDCENTURY : TK_DECO);
    else if (H > 100 && std::min(W, D) > 20 && roll < 0.2f) kind = TK_NEEDLE;
    else if (roll < 0.33f) kind = TK_DECO;
    else if (roll < 0.55f) kind = TK_FACETED;
    else if (roll < 0.8f) kind = TK_GLASS;
    else kind = TK_MIDCENTURY;

    float podH = tr.range(7.f, 13.f);
    float y0 = hmax + podH;
    Color podC = kind == TK_BRICK ? BRICK[tr.irange(0, 3)] : kind == TK_FACETED ? scaleC(GRANITE[tr.irange(0, 3)], 1.1f) : DECO[tr.irange(0, 4)];

    auto box = [&](float ax0, float az0, float ax1, float az1, float ya, float yb, Color side, Color roof, bool solid) {
        m.box({ax0, ya, az0}, {ax1, yb, az1}, side, roof, 1.f);
        if (solid) addPlat({ax0, ax1, az0, az1, yb, yb, ya - 1.f, 0, PK_BUILDING});
    };
    // vertical piers on every face, on the shader's window grid (3.0 m cells; 1.6 m for curtain wall)
    auto piers = [&](float ax0, float az0, float ax1, float az1, float ya, float yb, Color c, float sp, float w, float out) {
        for (float u = ceilf((ax0 + 0.8f) / sp) * sp; u < ax1 - 0.8f; u += sp) {
            m.box({u - w, ya, az0 - out}, {u + w, yb, az0}, c, 1.f);
            m.box({u - w, ya, az1}, {u + w, yb, az1 + out}, c, 1.f);
        }
        for (float u = ceilf((az0 + 0.8f) / sp) * sp; u < az1 - 0.8f; u += sp) {
            m.box({ax0 - out, ya, u - w}, {ax0, yb, u + w}, c, 1.f);
            m.box({ax1, ya, u - w}, {ax1 + out, yb, u + w}, c, 1.f);
        }
    };
    auto corners = [&](float ax0, float az0, float ax1, float az1, float ya, float yb, Color c, float s) {
        for (int i = 0; i < 4; i++) {
            float ux = (i & 1) ? ax1 : ax0, uz = (i & 2) ? az1 : az0;
            float dx = (i & 1) ? -1.f : 1.f, dz = (i & 2) ? -1.f : 1.f;
            float xa = std::min(ux - dx * 0.25f, ux + dx * s), xb = std::max(ux - dx * 0.25f, ux + dx * s);
            float za = std::min(uz - dz * 0.25f, uz + dz * s), zb = std::max(uz - dz * 0.25f, uz + dz * s);
            m.box({xa, ya, za}, {xb, yb, zb}, c, 1.f);
        }
    };
    auto belt = [&](float ax0, float az0, float ax1, float az1, float y, float h, float over, Color c) {
        m.box({ax0 - over, y, az0 - over}, {ax1 + over, y + h, az1 + over}, c, 1.f);
    };
    // lit trim that glows at night: a thin light strip just proud of a tier's edge
    auto glowBand = [&](float ax0, float az0, float ax1, float az1, float y, Color c) {
        belt(ax0, az0, ax1, az1, y, 0.22f, 0.42f, withMat(c, M_EMISSIVE));
    };
    auto roofClutter = [&](float ax0, float az0, float ax1, float az1, float y, bool pent) {
        float w = ax1 - ax0, d = az1 - az0;
        if (pent) {
            float px = w * 0.28f, pz = d * 0.28f, hh = tr.range(3.5f, 6.5f);
            box(cx - px, cz - pz, cx + px, cz + pz, y, y + hh, col(168, 170, 172), col(120, 122, 124), false);
            for (int k = 0; k < 4; k++) // louvers
                m.box({cx - px - 0.1f, y + 0.8f + k * 0.9f, cz - pz - 0.1f}, {cx + px + 0.1f, y + 1.0f + k * 0.9f, cz + pz + 0.1f}, col(134, 136, 140), 1.f);
            y += hh;
        }
        // aviation lights and an antenna
        m.box({cx - 0.25f, y, cz - 0.25f}, {cx + 0.25f, y + tr.range(8.f, 20.f), cz + 0.25f}, STEEL, 1.f);
        for (int i = 0; i < 4; i++) {
            float ux = (i & 1) ? ax1 - 0.6f : ax0 + 0.6f, uz = (i & 2) ? az1 - 0.6f : az0 + 0.6f;
            m.box({ux - 0.25f, y - 0.5f, uz - 0.25f}, {ux + 0.25f, y, uz + 0.25f}, col(230, 40, 40, M_EMISSIVE), 1.f);
        }
        // a window-washing rig
        float rx = ax0 + w * 0.2f, rz = az0 + d * 0.2f;
        m.box({rx, y - (pent ? 0.f : 0.f), rz}, {rx + 2.4f, y + 0.5f, rz + 1.2f}, col(90, 92, 98), 1.f);
        m.beam({rx + 1.2f, y + 0.5f, rz + 0.6f}, {rx + 3.2f, y + 2.4f, rz + 0.6f}, 0.2f, 0.2f, col(110, 112, 118));
    };

    // ------------------------------------------------------------ podium
    {
        Color pc = mat(podC, M_OFFICE);
        m.box({x0, hmin - 1.5f, z0}, {x1, y0, z1}, pc, col(112, 112, 114), 0.78f);
        addPlat({x0, x1, z0, z1, y0, y0, hmin - 1.5f, 0, PK_BUILDING});
        float baseY = hmax;
        // dark storefront band along the street, a heavy cornice and pilasters
        Color glassBand = col(44, 56, 66, M_GLASS);
        m.box({x0 + 0.5f, baseY - 0.2f, z0 - 0.12f}, {x1 - 0.5f, baseY + 4.2f, z0}, glassBand, 1.f);
        m.box({x0 + 0.5f, baseY - 0.2f, z1}, {x1 - 0.5f, baseY + 4.2f, z1 + 0.12f}, glassBand, 1.f);
        m.box({x0 - 0.12f, baseY - 0.2f, z0 + 0.5f}, {x0, baseY + 4.2f, z1 - 0.5f}, glassBand, 1.f);
        m.box({x1, baseY - 0.2f, z0 + 0.5f}, {x1 + 0.12f, baseY + 4.2f, z1 - 0.5f}, glassBand, 1.f);
        belt(x0, z0, x1, z1, baseY + 4.2f, 0.4f, 0.2f, scaleC(podC, 0.8f));
        glowBand(x0 + 0.5f, z0 + 0.5f, x1 - 0.5f, z1 - 0.5f, baseY + 4.0f, col(255, 226, 170)); // warm lobby glow along the street
        belt(x0, z0, x1, z1, y0 - 0.9f, 0.9f, 0.55f, scaleC(podC, 0.88f));
        piers(x0, z0, x1, z1, baseY + 4.6f, y0 - 0.9f, scaleC(podC, 1.07f), 3.f, 0.2f, 0.25f);
        // an entrance canopy on the long side
        bool alongX = W >= D;
        float ce = (alongX ? cx : cz) + tr.range(-3.f, 3.f);
        if (alongX) { m.box({ce - 3.5f, baseY + 4.6f, z0 - 3.2f}, {ce + 3.5f, baseY + 4.85f, z0}, col(70, 76, 84), 1.f); m.box({ce - 3.3f, baseY + 4.55f, z0 - 3.0f}, {ce + 3.3f, baseY + 4.6f, z0 - 0.2f}, col(255, 236, 190, M_EMISSIVE), 1.f); }
        else { m.box({x0 - 3.2f, baseY + 4.6f, ce - 3.5f}, {x0, baseY + 4.85f, ce + 3.5f}, col(70, 76, 84), 1.f); m.box({x0 - 3.0f, baseY + 4.55f, ce - 3.3f}, {x0 - 0.2f, baseY + 4.6f, ce + 3.3f}, col(255, 236, 190, M_EMISSIVE), 1.f); }
    }

    float ins = tr.range(1.5f, 3.2f);
    float tx0 = x0 + ins, tx1 = x1 - ins, tz0 = z0 + ins, tz1 = z1 - ins;
    if (tx1 - tx0 < 8 || tz1 - tz0 < 8 || H < podH + 10) {
        // too small for a tower: a plain roof with clutter
        roofClutter(x0, z0, x1, z1, y0, false);
        return;
    }
    float top = hmax + H;

    switch (kind) {
    case TK_DECO: {
        // setback shaft: three stepped tiers of piers and punched windows, a ziggurat crown and a copper roof
        Color stone = DECO[tr.irange(0, 4)];
        Color pierC = mixC(stone, col(244, 238, 222), 0.5f);
        float tiers[3] = {0.0f, 0.58f, 0.80f};
        float stepIn = std::min(std::min(W, D) * 0.14f, 4.5f);
        float ax0 = tx0, ax1 = tx1, az0 = tz0, az1 = tz1, ya = y0;
        for (int t = 0; t < 3; t++) {
            if (t > 0) { ax0 += stepIn; ax1 -= stepIn; az0 += stepIn; az1 -= stepIn; }
            if (ax1 - ax0 < 7.f || az1 - az0 < 7.f) break;
            float yb = t == 2 ? hmax + H * 0.93f : hmax + H * tiers[t + 1];
            box(ax0, az0, ax1, az1, ya, yb, mat(stone, M_OFFICE), scaleC(stone, 0.75f), true);
            piers(ax0, az0, ax1, az1, ya, yb, pierC, 3.f, 0.26f, 0.3f);
            corners(ax0, az0, ax1, az1, ya, yb, pierC, 1.3f);
            belt(ax0, az0, ax1, az1, yb - 0.8f, 0.8f, 0.5f, scaleC(pierC, 0.95f));
            glowBand(ax0, az0, ax1, az1, yb - 0.1f, col(255, 214, 150)); // each setback is floodlit along its edge
            ya = yb;
        }
        // crown: stepped ziggurat, green copper hip roof, gilded finial
        float cw = (ax1 - ax0) * 0.8f, cd = (az1 - az0) * 0.8f;
        box(cx - cw / 2, cz - cd / 2, cx + cw / 2, cz + cd / 2, ya, ya + 3.5f, mat(stone, M_OFFICE), scaleC(stone, 0.8f), true);
        float hw = cw * 0.72f, hd = cd * 0.72f;
        box(cx - hw / 2, cz - hd / 2, cx + hw / 2, cz + hd / 2, ya + 3.5f, ya + 6.5f, scaleC(pierC, 0.95f), COPPER, false);
        m.hip(cx - hw / 2 - 0.4f, cx + hw / 2 + 0.4f, cz - hd / 2 - 0.4f, cz + hd / 2 + 0.4f, ya + 6.5f, std::min(hw, hd) * 0.55f, withMat(COPPER, M_EMISSIVE)); // the copper crown glows green-white at night
        float fy = ya + 6.5f + std::min(hw, hd) * 0.55f;
        m.prism({cx, fy, cz}, 4, 0.55f, 0.08f, tr.range(7.f, 12.f), GILT, GILT, 0.78f, true, 1.f);
        break;
    }
    case TK_FACETED: {
        // dark granite with a sawtooth of glass bay windows, stepping in as it climbs
        Color gran = GRANITE[tr.irange(0, 3)];
        float ax0 = tx0, ax1 = tx1, az0 = tz0, az1 = tz1, ya = y0;
        float cuts[3] = {0.62f, 0.86f, 1.0f};
        for (int t = 0; t < 3; t++) {
            if (t > 0) { float s = std::min(std::min(W, D) * 0.16f, 5.f); ax0 += s; ax1 -= s; az0 += s; az1 -= s; }
            if (ax1 - ax0 < 8.f || az1 - az0 < 8.f) break;
            float yb = hmax + H * cuts[t];
            box(ax0, az0, ax1, az1, ya, yb, mat(gran, M_OFFICE), scaleC(gran, 0.7f), true);
            Color glassC = col(46, 58, 68, M_GLASS);
            const float BS = 4.5f, BW = 1.15f, BO = 1.0f;
            for (float u = ax0 + 2.f; u < ax1 - 2.f; u += BS) {
                m.box({u - BW, ya, az0 - BO}, {u + BW, yb, az0}, glassC, 1.f);
                m.box({u - BW, ya, az1}, {u + BW, yb, az1 + BO}, glassC, 1.f);
            }
            for (float u = az0 + 2.f; u < az1 - 2.f; u += BS) {
                m.box({ax0 - BO, ya, u - BW}, {ax0, yb, u + BW}, glassC, 1.f);
                m.box({ax1, ya, u - BW}, {ax1 + BO, yb, u + BW}, glassC, 1.f);
            }
            belt(ax0, az0, ax1, az1, yb - 0.5f, 0.5f, 0.35f, scaleC(gran, 0.85f));
            glowBand(ax0, az0, ax1, az1, yb + 0.05f, col(255, 200, 140));
            ya = yb;
            top = yb;
            if (t == 2 || ax1 - ax0 < 14.f) break;
        }
        m.hip(cx - (tx1 - tx0) * 0.12f, cx + (tx1 - tx0) * 0.12f, cz - (tz1 - tz0) * 0.12f, cz + (tz1 - tz0) * 0.12f, top, 4.5f, scaleC(gran, 1.2f));
        roofClutter(tx0 + 4, tz0 + 4, tx1 - 4, tz1 - 4, top + 4.5f, false);
        break;
    }
    case TK_GLASS: {
        // glass slab with fins at the corners and a sloped crown, plus a lower wing on one side
        Color g = GLASS[tr.irange(0, 4)];
        bool lowWing = tr.chance(0.6f) && W > 26;
        float wx0 = tx0, wx1 = tx1;
        if (lowWing) wx1 = tx0 + (tx1 - tx0) * 0.6f;
        box(wx0, tz0, wx1, tz1, y0, top, mat(g, M_GLASS), col(176, 180, 184), true);
        if (lowWing) box(wx1, tz0 + 2.f, tx1, tz1 - 2.f, y0, hmax + H * 0.55f, mat(scaleC(g, 1.08f), M_GLASS), col(150, 154, 158), true);
        corners(wx0, tz0, wx1, tz1, y0, top, col(184, 186, 190), 0.9f);
        piers(wx0, tz0, wx1, tz1, y0, top, col(178, 180, 186), 4.8f, 0.12f, 0.18f);
        // sloped crown
        float rise = tr.range(8.f, 15.f);
        bool alongZ = tr.chance(0.5f);
        m.slab(wx0, wx1, tz0, tz1, top, top + rise, alongZ ? 1 : 0, top, col(190, 194, 198), mat(g, M_GLASS));
        glowBand(wx0, tz0, wx1, tz1, top - 0.1f, col(150, 200, 255)); // blue LED line at the base of the crown
        addPlat({wx0, wx1, tz0, tz1, top + rise * 0.5f, top + rise * 0.5f, top - 1.f, 0, PK_BUILDING});
        roofClutter(wx0, tz0, wx1, tz1, top + rise, false);
        break;
    }
    case TK_MIDCENTURY: {
        // pale curtain-wall slab on a recessed glass lobby, with a penthouse and a helipad
        Color s = tr.chance(0.5f) ? col(218, 218, 212) : col(196, 200, 204);
        float sx0 = tx0, sx1 = tx1, sz0 = tz0, sz1 = tz1;
        box(sx0, sz0, sx1, sz1, y0, top, mat(s, M_OFFICE), col(120, 122, 124), true);
        belt(sx0, sz0, sx1, sz1, top - 1.2f, 1.2f, 0.3f, scaleC(s, 0.82f));
        glowBand(sx0, sz0, sx1, sz1, top - 1.35f, col(255, 255, 240));
        // vertical louver fins on the two short sides
        piers(sx0, sz0, sx1, sz1, y0 + 4.f, top - 1.2f, scaleC(s, 0.9f), 6.f, 0.14f, 0.35f);
        float pw = (sx1 - sx0) * 0.5f, pd = (sz1 - sz0) * 0.5f, ph = tr.range(4.f, 6.5f);
        box(cx - pw / 2, cz - pd / 2, cx + pw / 2, cz + pd / 2, top, top + ph, col(176, 178, 182), col(132, 134, 138), false);
        if (tr.chance(0.55f)) { // helipad on the flat part of the roof
            float hx = tx0 + 5.5f, hz = tz0 + 5.5f;
            m.prism({hx, top + 0.05f, hz}, 20, 5.f, 5.f, 0.2f, col(70, 72, 76), col(80, 82, 86), 0.f, true, 1.f);
            for (int k = 0; k < 12; k++) { float a = k * TAU_F / 12.f; m.box({hx + cosf(a) * 4.8f - 0.18f, top + 0.2f, hz + sinf(a) * 4.8f - 0.18f}, {hx + cosf(a) * 4.8f + 0.18f, top + 0.5f, hz + sinf(a) * 4.8f + 0.18f}, col(120, 255, 160, M_EMISSIVE), 1.f); }
            m.box({hx - 1.8f, top + 0.26f, hz - 0.3f}, {hx + 1.8f, top + 0.3f, hz + 0.3f}, col(240, 240, 235), 1.f);
            m.box({hx - 1.8f, top + 0.26f, hz - 2.0f}, {hx - 1.2f, top + 0.3f, hz + 2.0f}, col(240, 240, 235), 1.f);
            m.box({hx + 1.2f, top + 0.26f, hz - 2.0f}, {hx + 1.8f, top + 0.3f, hz + 2.0f}, col(240, 240, 235), 1.f);
        }
        roofClutter(sx0, sz0, sx1, sz1, top + ph, false);
        break;
    }
    case TK_NEEDLE: {
        // slender faceted glass tower rising from the podium to a tapering spire
        float rad = std::min(tx1 - tx0, tz1 - tz0) * 0.4f;
        Color g = GLASS[tr.irange(0, 4)];
        float shaftH = (top - y0) * 0.84f;
        m.prism({cx, y0, cz}, 8, rad, rad * 0.86f, shaftH, mat(g, M_GLASS), col(190, 194, 198), 0.39f, false, 1.f);
        m.prism({cx, y0 + shaftH, cz}, 8, rad * 0.86f, 0.f, top - y0 - shaftH + 6.f, mat(scaleC(g, 1.1f), M_GLASS), col(190, 194, 198), 0.39f, true, 1.f);
        for (int k = 1; k <= 3; k++) // ring ribs
            m.prism({cx, y0 + shaftH * k / 4.f, cz}, 8, rad * (1.f - 0.14f * k / 4.f) + 0.25f, rad * (1.f - 0.14f * k / 4.f) + 0.25f, 0.5f, col(190, 225, 255, M_EMISSIVE), col(190, 225, 255, M_EMISSIVE), 0.39f, true, 1.f);
        float a = rad * 0.72f;
        addPlat({cx - a, cx + a, cz - a, cz + a, top - (top - y0) * 0.2f, top - (top - y0) * 0.2f, y0 - 1.f, 0, PK_BUILDING});
        m.box({cx - 0.3f, top + 6.f, cz - 0.3f}, {cx + 0.3f, top + 22.f, cz + 0.3f}, STEEL, 1.f);
        m.box({cx - 0.35f, top + 22.f, cz - 0.35f}, {cx + 0.35f, top + 22.8f, cz + 0.35f}, col(255, 60, 50, M_EMISSIVE), 1.f);
        break;
    }
    case TK_BRICK: {
        // old brick high-rise: corner quoins, belt courses, a deep bracketed cornice, a rooftop water tank
        Color brick = BRICK[tr.irange(0, 3)];
        float hh = std::min(H, 62.f);
        float yt = hmax + hh;
        float bx0 = x0 + 0.6f, bx1 = x1 - 0.6f, bz0 = z0 + 0.6f, bz1 = z1 - 0.6f;
        box(bx0, bz0, bx1, bz1, y0, yt, mat(brick, M_OFFICE), scaleC(brick, 0.7f), true);
        Color trim = mixC(brick, col(232, 220, 196), 0.7f);
        corners(bx0, bz0, bx1, bz1, y0, yt, trim, 1.1f);
        for (float yy = y0 + (yt - y0) * 0.33f; yy < yt - 6.f; yy += (yt - y0) * 0.33f) belt(bx0, bz0, bx1, bz1, yy, 0.7f, 0.25f, trim);
        belt(bx0, bz0, bx1, bz1, yt - 2.4f, 1.0f, 0.5f, trim);
        belt(bx0, bz0, bx1, bz1, yt - 1.0f, 1.0f, 1.0f, scaleC(trim, 0.88f));
        for (float u = ceilf((bx0 + 1.f) / 3.f) * 3.f; u < bx1 - 1.f; u += 3.f) { // cornice brackets
            m.box({u - 0.18f, yt - 2.4f, bz0 - 0.9f}, {u + 0.18f, yt - 1.0f, bz0 - 0.1f}, trim, 1.f);
            m.box({u - 0.18f, yt - 2.4f, bz1 + 0.1f}, {u + 0.18f, yt - 1.0f, bz1 + 0.9f}, trim, 1.f);
        }
        // water tank on stilts
        float wx = cx + tr.range(-3.f, 3.f), wz = cz + tr.range(-3.f, 3.f);
        for (int i = 0; i < 4; i++) {
            float ux = wx + ((i & 1) ? 1.4f : -1.4f), uz = wz + ((i & 2) ? 1.4f : -1.4f);
            m.beam({ux, yt, uz}, {ux, yt + 3.0f, uz}, 0.18f, 0.18f, col(70, 66, 62));
        }
        m.prism({wx, yt + 3.0f, wz}, 10, 2.1f, 2.1f, 3.6f, col(122, 90, 62), col(96, 72, 50), 0.f, true, 1.f);
        m.prism({wx, yt + 6.6f, wz}, 10, 2.25f, 0.f, 1.3f, col(86, 66, 48), col(86, 66, 48), 0.f, true, 1.f);
        // a painted rooftop sign
        m.box({bx0 + 2.f, yt + 0.0f, cz - 0.15f}, {bx0 + 9.f, yt + 3.0f, cz + 0.15f}, col(255, 200, 120, M_EMISSIVE), 1.f);
        top = yt;
        roofClutter(bx0, bz0, bx1, bz1, yt, false);
        break;
    }
    }
}
