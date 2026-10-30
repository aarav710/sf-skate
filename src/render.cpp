#include "render.h"
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include <string>

const char* Renderer::QUALITY_NAMES[3] = {"Low", "Medium", "High"};

// Static chunks (chunkOrg.w = 1) bring their position packed: whole 1/1024 m steps from chunkOrg, x and z in the
// low 21 bits of each word, y split over the top 11 bits of both (see bakeChunk). Instanced movers (instOn = 1, see
// instance.cpp) bring a unit mesh plus the rows of a per-instance world matrix and a color. Everything else: vertexPosition.
#define PACKED_POS R"(
in vec3 vertexPosition;
layout(location = 10) in uvec2 vertexPacked;
layout(location = 11) in vec4 instRow0;
layout(location = 12) in vec4 instRow1;
layout(location = 13) in vec4 instRow2;
uniform vec4 chunkOrg = vec4(0.0);
uniform int instOn = 0;
vec3 chunkPos() {
    if (instOn == 1) {
        vec4 p = vec4(vertexPosition, 1.0);
        return vec3(dot(instRow0, p), dot(instRow1, p), dot(instRow2, p));
    }
    if (chunkOrg.w < 0.5) return vertexPosition;
    uvec3 q = uvec3(vertexPacked.x & 0x1fffffu, (vertexPacked.x >> 21) | ((vertexPacked.y >> 21) << 11), vertexPacked.y & 0x1fffffu);
    return chunkOrg.xyz + vec3(q) * (1.0 / 1024.0);
}
)"

// ---------------------------------------------------------------- city shader
// Static meshes bring a normal (w = 1: given explicitly, use as is), the height above the bare ground under the
// vertex (in vertexTexCoord.x) and the material code in the color alpha. Immediate-mode and instanced models only bring
// position and color, so they shade with flat face normals from screen-space derivatives (useVN = 0).
// Built twice: FAR 0 for the near pass, FAR 1 for everything past 250 m, which is most of the vertices on screen
// and needs neither normals, shadows nor fine surface detail (fewer vertex outputs is what matters on a tiled GPU).
static const char* CITY_VS = PACKED_POS R"(
in vec4 vertexColor;
layout(location = 14) in vec4 instCol;
uniform mat4 mvp;
out vec3 wPos;
out vec4 vCol;
#if !FAR
in vec2 vertexTexCoord;
in vec4 vertexNormal;
out vec4 vNH;   // normal (length 2 when given explicitly), height above the bare ground
#endif
void main() {
    wPos = chunkPos();
    vCol = instOn == 1 ? instCol : vertexColor;
#if !FAR
    vNH = vec4(vertexNormal.xyz * (vertexNormal.w > 0.5 ? 2.0 : 1.0), vertexTexCoord.x);
#endif
    gl_Position = mvp * vec4(wPos, 1.0);
})";

static const char* CITY_FS = R"(
in vec3 wPos;
in vec4 vCol;
#if !FAR
in vec4 vNH;
#endif
uniform vec3 camPos;
uniform vec3 sunDir;
uniform vec3 sunCol;
uniform vec3 ambCol;
uniform vec3 fogCol;
uniform vec3 skyCol;
uniform float fogDen;
uniform float night;
uniform float time;
uniform float wet;
uniform int useVN;
uniform sampler2DShadow shadowMap0;
uniform sampler2DShadow shadowMap1;
uniform mat4 lightVP0;
uniform mat4 lightVP1;
uniform vec4 shadowInfo;   // cascades, world texel of cascade 0 and 1, strength
uniform vec3 shadowMisc;   // uv texel, fog height scale, unused
out vec4 finalColor;

float h21(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = h21(i), b = h21(i + vec2(1, 0)), c = h21(i + vec2(0, 1)), d = h21(i + vec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// ---- sun shadows: two cascades, 4 hardware-filtered taps each
float taps(sampler2DShadow sm, vec3 c) {
    float t = shadowMisc.x * 0.75;
    return 0.25 * (texture(sm, c + vec3(-t, -t, 0.0)) + texture(sm, c + vec3(t, -t, 0.0)) +
                   texture(sm, c + vec3(-t, t, 0.0)) + texture(sm, c + vec3(t, t, 0.0)));
}
float sunShadow(vec3 p, vec3 Ng, float dist) {
    if (shadowInfo.x < 0.5 || dist > 270.0) return 1.0;
    vec3 off0 = Ng * shadowInfo.y * 2.2 + sunDir * shadowInfo.y * 0.8;
    vec4 l0 = lightVP0 * vec4(p + off0, 1.0);
    float e0 = max(abs(l0.x), abs(l0.y));
    float s = 1.0, fade = smoothstep(220.0, 265.0, dist);
    if (e0 < 0.97) {
        s = taps(shadowMap0, l0.xyz * 0.5 + 0.5 - vec3(0.0, 0.0, 0.00002));
        if (e0 > 0.85 && shadowInfo.x > 1.5) {   // blend into the wide cascade near the edge of the sharp one
            vec4 l1 = lightVP1 * vec4(p + Ng * shadowInfo.z * 2.2 + sunDir * shadowInfo.z * 0.8, 1.0);
            s = mix(s, taps(shadowMap1, l1.xyz * 0.5 + 0.5 - vec3(0.0, 0.0, 0.00004)), smoothstep(0.85, 0.97, e0));
        }
    } else if (shadowInfo.x > 1.5) {
        vec4 l1 = lightVP1 * vec4(p + Ng * shadowInfo.z * 2.2 + sunDir * shadowInfo.z * 0.8, 1.0);
        float e1 = max(abs(l1.x), abs(l1.y));
        if (e1 >= 0.98) return 1.0;
        s = taps(shadowMap1, l1.xyz * 0.5 + 0.5 - vec3(0.0, 0.0, 0.00004));
        fade = max(fade, smoothstep(0.88, 0.98, e1));
    } else {
        fade = max(fade, smoothstep(0.9, 0.97, e0));
    }
    return mix(1.0, mix(s, 1.0, fade), shadowInfo.w);
}

// ---- haze: thicker low down, thinner over the hilltops; warm toward a low sun, a faint city glow at night
float fogAmount(float dist) {
    float H = shadowMisc.y;
    float dy = wPos.y - camPos.y;
    float k = abs(dy) > 0.5 ? (1.0 - exp(-dy / H)) / (dy / H) : 1.0;
    float dens = fogDen * 1.3 * exp(-max(camPos.y, 0.0) / H);
    return 1.0 - exp(-dist * dens * k);
}
vec3 fogColor(vec3 V) {
    float golden = (1.0 - smoothstep(0.2, 0.5, sunDir.y)) * (1.0 - smoothstep(0.6, 0.9, night));
    float sunAmt = pow(max(dot(-V, sunDir), 0.0), 6.0);
    vec3 fc = mix(fogCol, sunCol * 1.05, sunAmt * (0.25 + 0.4 * golden) * (1.0 - night * 0.8));
    fc += vec3(0.07, 0.045, 0.02) * night * exp(-max(wPos.y, 0.0) / 120.0) * (1.0 - wet * 0.3);
    return fc;
}

// ---- ground: asphalt cracks and patches, paving joints, grass variation (picked by the ground color)
vec3 groundDetail(vec3 base, vec2 p, float near) {
    float lum = dot(base, vec3(0.3, 0.59, 0.11));
    float sat = max(base.r, max(base.g, base.b)) - min(base.r, min(base.g, base.b));
    float n = vnoise(p * 0.8) * 0.5 + vnoise(p * 0.11) * 0.5;
    if (lum < 0.3 && sat < 0.06) {                       // asphalt
        float fw = fwidth(p.x) + fwidth(p.y);
        base *= 0.9 + 0.14 * vnoise(p * 0.3) + 0.06 * (vnoise(p * 2.7) - 0.5) + 0.08 * (vnoise(p * 11.0) - 0.5) * clamp(1.0 - fw * 6.0, 0.0, 1.0);
        vec2 pc = floor(p / vec2(2.7, 4.3));
        if (h21(pc) > 0.94) base *= 0.82;                // patched squares of newer, darker blacktop
        float cr = abs(vnoise(p * 0.55 + vec2(17.0, 3.0)) - 0.5) + 0.5 * abs(vnoise(p * 2.2) - 0.5) * 0.25;
        float line = 1.0 - smoothstep(0.003, 0.012 + fw * 0.3, cr);
        float mask = smoothstep(0.45, 0.75, vnoise(p * 0.07 + 9.0));
        base *= 1.0 - 0.35 * line * mask * clamp(1.0 - fw * 2.5, 0.0, 1.0);
        base *= 1.0 - 0.12 * smoothstep(0.6, 0.85, vnoise(p * 0.45 + 31.0)) * smoothstep(0.5, 0.8, vnoise(p * 0.05)); // oil stains
        return base;
    }
    if (lum > 0.45 && lum < 0.8 && sat < 0.09) {         // sidewalks and plazas: slabs with joints
        vec2 q = p / 1.5;
        vec2 f = abs(fract(q) - 0.5);
        float fw = max(fwidth(q.x), fwidth(q.y));
        float joint = smoothstep(0.488 - fw, 0.496, max(f.x, f.y)) * clamp(1.0 - fw * 5.0, 0.0, 1.0);
        base *= (0.95 + 0.08 * h21(floor(q))) * (1.0 - 0.16 * joint);
        base *= mix(1.0, 0.86 + 0.28 * n, 0.3 + 0.4 * near);
        return base;
    }
    if (base.g > base.r * 1.1 && base.g > base.b * 1.15) {   // grass: dry and lush patches, tufts up close
        float g1 = vnoise(p * 0.12), g2 = vnoise(p * 0.9), g3 = vnoise(p * 5.0);
        base = mix(base * vec3(0.92, 0.98, 0.9), base * vec3(1.18, 1.08, 0.78), smoothstep(0.4, 0.85, g1) * 0.55);
        base *= 0.84 + 0.2 * g2 + 0.14 * (g3 - 0.5) * near;
        return base;
    }
    return base * mix(1.0, 0.84 + 0.32 * n, 0.35 + 0.65 * near);
}

void main() {
    vec3 Ng = normalize(cross(dFdx(wPos), dFdy(wPos)));
    vec3 toCam = camPos - wPos;
    float dist = length(toCam);
    vec3 V = toCam / max(dist, 1e-3);
    if (dot(Ng, V) < 0.0) Ng = -Ng;
    int m = int(floor((1.0 - vCol.a) * 17.0 + 0.5));
    vec3 N = Ng;
    bool given = false;
    float ao = 1.0;
#if !FAR
    if (useVN == 1) {
        vec3 vn = vNH.xyz;
        float L = length(vn);
        if (L > 0.1) {
            given = L > 1.5;
            vn /= L;
            if (!given && dot(vn, Ng) < 0.0) vn = -vn;
            N = vn;
        }
        // contact shadow where walls, trunks, poles and parked cars meet the ground
        if (!given && m != 10)
            ao = mix(1.0, 0.52 + 0.48 * smoothstep(0.0, 1.5, vNH.w), (1.0 - abs(Ng.y)) * clamp(1.6 - dist / 120.0, 0.0, 1.0));
    }
#endif
    vec3 base = vCol.rgb;
    vec3 emis = vec3(0.0);
    float specK = 0.0, specP = 32.0, bloom = 0.0;
    float near = clamp(1.0 - dist / 260.0, 0.0, 1.0);

    if (m == 5) {                       // ground
#if FAR
        float n = vnoise(wPos.xz * 0.8) * 0.5 + vnoise(wPos.xz * 0.11) * 0.5;
        base *= mix(1.0, 0.84 + 0.32 * n, 0.35);
#else
        base = groundDetail(base, wPos.xz, near);
#endif
    } else if (m == 1 || m == 2 || m == 6 || (m >= 7 && m <= 9)) {   // facades with windows
        if (abs(Ng.y) < 0.45) {
            bool house = (m == 2 || m >= 7);            // 7..9 are house materials with shifted floor lines
            float u = abs(Ng.x) > abs(Ng.z) ? wPos.z : wPos.x;
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
                float fres = pow(1.0 - abs(dot(Ng, V)), 2.0);
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
#if !FAR
            else if (house) {                         // painted wooden siding: a shadow line under each board
                float sy = y / 0.21, sw = fwidth(sy);
                base *= 1.0 - 0.1 * (1.0 - smoothstep(0.0, 0.3, fract(sy))) * clamp(1.0 - sw * 2.0, 0.0, 1.0);
            } else if (base.r > base.g * 1.25 && base.r > base.b * 1.3) {   // brick: running bond with pale mortar
                vec2 bq = vec2(u / 0.24, y / 0.085);
                bq.x += 0.5 * mod(floor(bq.y), 2.0);
                vec2 bf = fract(bq);
                float fwb = max(fwidth(bq.x), fwidth(bq.y));
                float mortar = max(1.0 - smoothstep(0.0, 0.1 + fwb, bf.y), 1.0 - smoothstep(0.0, 0.05 + fwb * 0.5, bf.x));
                float k = clamp(1.0 - fwb * 2.0, 0.0, 1.0);
                vec3 brick = mix(base * (0.86 + 0.26 * h21(floor(bq))), vec3(0.6, 0.57, 0.52), mortar * 0.7);
                base = mix(base, brick, k);
            } else {                                    // concrete / stone: a dark seam at each floor line and some streaking
                float k = clamp(1.0 - fwidth(y / fh) * 6.0, 0.0, 1.0);
                base *= 1.0 - 0.12 * (1.0 - smoothstep(0.0, 0.04, f.y)) * k;
                base *= 0.95 + 0.08 * vnoise(vec2(u * 0.7, y * 0.12));
            }
#endif
            if (house) {
                // average facade tone seen from afar: wall, a share of dark glass and cream trim
                vec3 avg = plain * 0.62 + vec3(0.17, 0.2, 0.25) * 0.21 + vec3(0.94, 0.92, 0.86) * 0.17;
                base = mix(avg, base, detail);
                specK *= detail;
            }
        }
    } else if (m == 3) {                 // water: see-through in the shallows, a mirror for the sky at a low angle
        vec2 p = wPos.xz;
        float t = time;
        float fw = length(fwidth(p));   // metres per pixel: drop the waves too small to show, instead of letting them sparkle
        // swell rolling in through the Golden Gate, wind chop across it, and small ripples on top
        const vec4 W[6] = vec4[6](vec4(0.94, 0.34, 0.045, 0.9), vec4(0.70, -0.71, 0.09, 0.55), vec4(0.98, -0.2, 0.16, 0.38),
                                  vec4(-0.45, 0.89, 0.23, 0.26), vec4(0.31, 0.95, 0.41, 0.17), vec4(-0.87, -0.5, 0.67, 0.11));
        vec2 g = vec2(0.0);
        float crest = 0.0;
        for (int i = 0; i < 6; i++) {
            vec2 d = W[i].xy;
            float k = W[i].z, ph = dot(d, p) * k + t * sqrt(9.8 * k) * 0.55 + float(i) * 1.7;
            float vis = clamp(1.6 - fw * k * 2.5, 0.0, 1.0);
            g += d * (W[i].w * k * cos(ph)) * vis;
            crest += W[i].w * sin(ph) * vis;
        }
        float near = clamp(1.0 - fw * 3.0, 0.0, 1.0);
        vec2 q = p * 1.6 + vec2(t * 0.9, t * 0.35);
        g += vec2(vnoise(q + vec2(0.05, 0.0)) - vnoise(q - vec2(0.05, 0.0)), vnoise(q + vec2(0.0, 0.05)) - vnoise(q - vec2(0.0, 0.05))) * 1.6 * near;
        if (wet > 0.0) {                 // rain rings on the surface
            g.x += wet * 0.05 * (vnoise(p * 3.0 + t * 7.0) - 0.5);
            g.y += wet * 0.05 * (vnoise(p * 3.0 - t * 7.0 + 5.0) - 0.5);
        }
        // glassy slicks where the wind drops, and gustier patches that are rougher
        float slick = smoothstep(0.55, 0.78, vnoise(p * 0.0035 + vec2(t * 0.004, 0.0)));
        float gust = smoothstep(0.4, 0.8, vnoise(p * 0.0021 + vec2(-t * 0.006, 3.0)));
        g *= (1.0 - 0.65 * slick) * (0.85 + 0.35 * gust);
        N = normalize(vec3(-g.x, 1.0, -g.y));
        float fres = 0.02 + 0.98 * pow(1.0 - max(dot(N, V), 0.0), 5.0);
        vec3 R = reflect(-V, N);
        R.y = abs(R.y);
        vec3 env = mix(fogColor(V), skyCol, smoothstep(0.02, 0.45, R.y));   // the sky, hazy at the horizon
        vec3 deep = base * 0.62 * (ambCol * 1.2 + sunCol * 0.35);
        vec3 col = mix(deep, env, clamp(fres, 0.0, 0.9));
#if FAR
        float sh = 1.0;
#else
        float sh = dist < 260.0 ? sunShadow(wPos, vec3(0.0, 1.0, 0.0), dist) : 1.0;
#endif
        // sun (or moon) on the water: a tight glint up close that spreads into a glitter path further out
        float sp = mix(900.0, 120.0, smoothstep(30.0, 1500.0, dist)) * (1.0 + slick);
        vec3 spec = sunCol * (pow(max(dot(R, sunDir), 0.0), sp) * 3.0 + pow(max(dot(R, sunDir), 0.0), 12.0) * 0.05) * sh * (1.0 - wet * 0.7);
        col += spec;
        // whitecaps: the bay is windy, so the odd crest breaks white
        float cap = smoothstep(1.0, 1.5, crest + gust * 0.4) * smoothstep(0.55, 0.85, vnoise(p * 0.3 + vec2(t * 0.5, t * 0.2))) * (1.0 - slick);
        cap *= clamp(1.0 - dist / 900.0, 0.0, 1.0) * (1.0 - wet * 0.3);
        col = mix(col, vec3(0.9, 0.93, 0.94) * (ambCol * 1.1 + sunCol * 0.75 * sh), cap * 0.8);
        float f = fogAmount(dist);
        // how much you see through: most looking straight down in the shallows, a mirror at a low angle, opaque far off
        float a = mix(0.42 + 0.58 * fres, 1.0, smoothstep(220.0, 650.0, dist));
        a = max(a, cap * 0.9);
        finalColor = vec4(mix(col, fogColor(V), f), a);
        return;
    } else if (m == 4) {                 // lights
        emis = base * night * 1.3;
        bloom = night * 0.95;
    }

    // wet streets: darker, glossy, with puddles that mirror the sky
    float wetS = 0.0, reflK = 0.0;
    if (FAR == 0 && wet > 0.0 && Ng.y > 0.55 && m != 10 && m != 4) {
        float puddle = smoothstep(0.58, 0.72, vnoise(wPos.xz * 0.23) * 0.75 + vnoise(wPos.xz * 1.9) * 0.25);
        wetS = wet * (m == 5 ? 0.75 + 0.25 * puddle : 0.5);
        reflK = wet * (m == 5 ? 0.22 + 0.6 * puddle : 0.25);   // standing water mirrors, damp ground just darkens
        base *= 1.0 - 0.42 * wetS;
    }

    float nl = dot(N, sunDir);
    float diff = given ? max(nl * 0.8 + 0.2, 0.0) : max(nl, 0.0);   // leaves let a little light through
#if FAR
    float sh = 1.0;
#else
    float sh = diff > 0.0 ? sunShadow(wPos, Ng, dist) : 1.0;
#endif
    // sky light from above; immediate-mode models (flat normals, lots of small downward faces) get a gentler falloff
    float hemi = useVN == 1 ? 0.62 + 0.38 * N.y : 0.72 + 0.28 * N.y;
    vec3 light = ambCol * hemi * ao * 1.06 + sunCol * diff * sh * (0.6 + 0.4 * ao);
    if (m == 10) {                       // rooms: warm lamp light and soft window light, warmer still at night
        vec3 day = (ambCol * 1.2 + sunCol * 0.2 + vec3(0.28, 0.22, 0.12)) * (0.88 + 0.12 * hemi);
        light = mix(day, vec3(1.0, 0.88, 0.7) * (0.9 + 0.1 * hemi), night);
    }
    vec3 col = base * light;
    if (specK > 0.0) {
        vec3 H = normalize(sunDir + V);
        vec3 sp = sunCol * specK * pow(max(dot(N, H), 0.0), specP) * sh;
        col += sp;
        bloom += clamp(dot(sp, vec3(0.33)) - 0.25, 0.0, 1.0);
    }
    if (wetS > 0.0) {
        // rain ripples break up the reflection a little
        vec2 rp = wPos.xz * 2.5;
        vec3 Nw = normalize(vec3((vnoise(rp + time * 6.0) - 0.5) * 0.12, 1.0, (vnoise(rp - time * 6.0 + 7.0) - 0.5) * 0.12));
        float fres = 0.05 + 0.95 * pow(1.0 - max(dot(Nw, V), 0.0), 5.0);
        vec3 R = reflect(-V, Nw);
        vec3 env = mix(fogCol, skyCol, clamp(R.y * 3.0, 0.0, 1.0));
        col = mix(col, env * (0.55 + 0.25 * sh), clamp(fres * reflK, 0.0, 0.75));
        vec3 H = normalize(sunDir + V);
        vec3 sp = sunCol * pow(max(dot(Nw, H), 0.0), 90.0) * 1.6 * wetS * sh;
        col += sp;
        // city lights smeared across wet asphalt at night
        col += vec3(1.0, 0.72, 0.42) * night * wetS * fres * 0.5 * (0.6 + 0.4 * vnoise(wPos.xz * 0.15));
        bloom += clamp(dot(sp, vec3(0.33)) - 0.2, 0.0, 0.6);
    }
    col += emis;
    bloom += clamp(dot(emis, vec3(0.33)) * 0.8, 0.0, 0.8);
    // the bay floor, seen through the water: sunlight rippling over the shallows, foam washing up and down the
    // beach, and the water's own green-grey closing in with depth
    if (m == 5 && wPos.y < 0.35) {
        float depth = max(-wPos.y, 0.0);
        vec2 p = wPos.xz;
        float fade = clamp(1.0 - dist / 700.0, 0.0, 1.0);
        float c1 = vnoise(p * 0.7 + vec2(time * 0.35, time * 0.2)), c2 = vnoise(p * 0.9 - vec2(time * 0.25, -time * 0.3) + 7.0);
        float caustic = pow(1.0 - abs(c1 - c2), 10.0);
        col += sunCol * caustic * 0.45 * smoothstep(0.05, 0.4, depth) * exp(-depth * 0.55) * sh * fade;
        float surge = -0.12 + 0.2 * sin(time * 0.8 + vnoise(p * 0.04) * 9.0);   // the edge of the water running up and back
        float lace = smoothstep(0.35, 0.65, vnoise(p * 1.3 + vec2(time * 0.3, 0.0)));
        float foam = (1.0 - smoothstep(0.0, 0.08 + 0.12 * lace, abs(wPos.y - surge))) * (0.55 + 0.45 * lace) * clamp(1.0 - dist / 900.0, 0.0, 1.0);
        if (wPos.y > surge && wPos.y < surge + 0.35) col *= 0.82;             // wet sand the wash just left
        col = mix(col, vec3(0.92, 0.95, 0.95) * light, foam * 0.8);
        vec3 deepLit = vec3(0.075, 0.2, 0.21) * (ambCol * 1.2 + sunCol * 0.35);
        col = mix(col, deepLit, 1.0 - exp(-depth * 0.42));
    }
    float f = fogAmount(dist);
    if (length(emis) > 0.05) f *= 0.75;   // lights shine through the fog a bit
    col = mix(col, fogColor(V), f);
    finalColor = vec4(col, 1.0 - clamp(bloom * (1.0 - f * 0.8), 0.0, 1.0));
})";

// shadow casters only need depth
static const char* DEPTH_VS = "#version 330\n" PACKED_POS R"(
uniform mat4 mvp;
void main() { gl_Position = mvp * vec4(chunkPos(), 1.0); })";
static const char* DEPTH_FS = R"(#version 330
out vec4 finalColor;
void main() { finalColor = vec4(1.0); })";

// ---------------------------------------------------------------- post
// bright parts: the scene's alpha is 1 - bloom, so lamps, neon, lit windows and glints glow and nothing else does
static const char* EXTRACT_FS = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform vec2 texel;
out vec4 finalColor;
void main() {
    vec3 s = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        vec2 o = vec2((i & 1) == 0 ? -0.5 : 0.5, i < 2 ? -0.5 : 0.5) * texel;
        vec4 c = texture(texture0, fragTexCoord + o);
        s += c.rgb * (1.0 - c.a);
    }
    finalColor = vec4(s * 0.25, 1.0);
})";
static const char* BLUR_FS = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform vec2 dir;
out vec4 finalColor;
void main() {
    // 9 taps folded into 5 bilinear fetches
    vec3 s = texture(texture0, fragTexCoord).rgb * 0.227;
    s += (texture(texture0, fragTexCoord + dir * 1.385).rgb + texture(texture0, fragTexCoord - dir * 1.385).rgb) * 0.316;
    s += (texture(texture0, fragTexCoord + dir * 3.231).rgb + texture(texture0, fragTexCoord - dir * 3.231).rgb) * 0.070;
    finalColor = vec4(s, 1.0);
})";
static const char* POST_FS = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform sampler2D bloomTex;
uniform sampler2D bloomTex2;
uniform vec3 gain;
uniform vec3 lift;
uniform vec4 params;   // saturation, contrast, bloom, vignette
uniform float blur;    // radial speed blur
out vec4 finalColor;
void main() {
    vec2 uv = fragTexCoord;
    vec3 c = texture(texture0, uv).rgb;
    if (blur > 0.0005) {   // speed: streak the edges of the frame outward from the middle
        vec2 d = uv - vec2(0.5);
        float k = blur * smoothstep(0.1, 0.6, length(d));
        for (int i = 1; i < 7; i++) c += texture(texture0, uv - d * k * float(i)).rgb;
        c /= 7.0;
    }
    c += (texture(bloomTex, uv).rgb * 0.6 + texture(bloomTex2, uv).rgb * 0.8) * params.z;
    c = c * gain + lift * (1.0 - c);
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(l), c, params.x);
    c = (c - 0.5) * params.y + 0.5;
    vec2 v = uv - 0.5;
    c *= 1.0 - params.w * dot(v, v) * 1.6;
    // a touch of dither so the sky gradient doesn't band
    c += (fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5) / 255.0;
    finalColor = vec4(clamp(c, 0.0, 1.0), 1.0);
})";

// ---------------------------------------------------------------- setup
static Shader cityShader(bool far) {
    std::string head = std::string("#version 330\n#define FAR ") + (far ? "1" : "0") + "\n";
    return LoadShaderFromMemory((head + CITY_VS).c_str(), (head + CITY_FS).c_str());
}

void Renderer::init() {
    city = cityShader(false);
    cityFar = cityShader(true);
    depth = LoadShaderFromMemory(DEPTH_VS, DEPTH_FS);
    extract = LoadShaderFromMemory(nullptr, EXTRACT_FS);
    blur = LoadShaderFromMemory(nullptr, BLUR_FS);
    post = LoadShaderFromMemory(nullptr, POST_FS);
    locUseVN = GetShaderLocation(city, "useVN");
    locShadow[0] = GetShaderLocation(city, "shadowMap0");
    locShadow[1] = GetShaderLocation(city, "shadowMap1");
    locLVP[0] = GetShaderLocation(city, "lightVP0");
    locLVP[1] = GetShaderLocation(city, "lightVP1");
    locShInfo = GetShaderLocation(city, "shadowInfo");
    // the shadow maps live on texture units 10 and 11, out of the way of raylib's own
    int u0 = 10, u1 = 11;
    SetShaderValue(city, locShadow[0], &u0, SHADER_UNIFORM_INT);
    SetShaderValue(city, locShadow[1], &u1, SHADER_UNIFORM_INT);
    for (int k = 0; k < MAXC; k++) {
        glGenTextures(1, &shTex[k]);
        glBindTexture(GL_TEXTURE_2D, shTex[k]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, shSize, shSize, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
        glGenFramebuffers(1, &shFbo[k]);
        glBindFramebuffer(GL_FRAMEBUFFER, shFbo[k]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, shTex[k], 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) TraceLog(LOG_WARNING, "SFSKATE: shadow map %d incomplete", k);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    for (int k = 0; k < MAXC; k++) {
        glActiveTexture(GL_TEXTURE10 + k);
        glBindTexture(GL_TEXTURE_2D, shTex[k]);
    }
    glActiveTexture(GL_TEXTURE0);
    setQuality(quality);
}

void Renderer::setQuality(int q) {
    quality = q;
    int samples = 4;   // the resolve is cheap, and 2x measured no faster here
    if (samples != msSamples) freeTargets();
    msSamples = samples;
    ensureTargets();
}

void Renderer::freeTargets() {
    if (!msFbo) return;
    glDeleteFramebuffers(1, &msFbo);
    glDeleteRenderbuffers(1, &msColor);
    glDeleteRenderbuffers(1, &msDepth);
    msFbo = msColor = msDepth = 0;
    UnloadRenderTexture(scene); UnloadRenderTexture(bloomA); UnloadRenderTexture(bloomB); UnloadRenderTexture(bloomC);
    w = h = 0;
}

void Renderer::ensureTargets() {
    int W = GetRenderWidth(), H = GetRenderHeight();
    if (W == w && H == h && msFbo) return;
    freeTargets();
    w = W; h = H;
    glGenRenderbuffers(1, &msColor);
    glBindRenderbuffer(GL_RENDERBUFFER, msColor);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, msSamples, GL_RGBA8, w, h);
    glGenRenderbuffers(1, &msDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, msDepth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, msSamples, GL_DEPTH_COMPONENT24, w, h);
    glGenFramebuffers(1, &msFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, msFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msColor);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, msDepth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) TraceLog(LOG_WARNING, "SFSKATE: scene target incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    scene = LoadRenderTexture(w, h);
    bloomA = LoadRenderTexture(std::max(1, w / 2), std::max(1, h / 2));
    bloomB = LoadRenderTexture(std::max(1, w / 4), std::max(1, h / 4));
    bloomC = LoadRenderTexture(std::max(1, w / 4), std::max(1, h / 4));
    for (auto* t : {&scene, &bloomA, &bloomB, &bloomC}) {
        SetTextureFilter(t->texture, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(t->texture, TEXTURE_WRAP_CLAMP);
    }
}

void Renderer::setLook(const Look& L) {
    float misc[3] = {1.f / shSize, L.fogH, 0};
    for (Shader* s : {&city, &cityFar}) {
        auto set = [&](const char* n, const void* v, int t) { SetShaderValue(*s, GetShaderLocation(*s, n), v, t); };
        set("camPos", &L.camPos, SHADER_UNIFORM_VEC3);
        set("sunDir", &L.sunDir, SHADER_UNIFORM_VEC3);
        set("sunCol", &L.sunCol, SHADER_UNIFORM_VEC3);
        set("ambCol", &L.amb, SHADER_UNIFORM_VEC3);
        set("fogCol", &L.fog, SHADER_UNIFORM_VEC3);
        set("skyCol", &L.sky, SHADER_UNIFORM_VEC3);
        set("fogDen", &L.fogDen, SHADER_UNIFORM_FLOAT);
        set("night", &L.night, SHADER_UNIFORM_FLOAT);
        set("time", &L.time, SHADER_UNIFORM_FLOAT);
        set("wet", &L.wet, SHADER_UNIFORM_FLOAT);
        set("shadowMisc", misc, SHADER_UNIFORM_VEC3);
    }
}

void Renderer::staticNormals(bool on) {
    rlDrawRenderBatchActive();
    int v = on ? 1 : 0;
    SetShaderValue(city, locUseVN, &v, SHADER_UNIFORM_INT);
}

// ---------------------------------------------------------------- shadows
// Cascade 0 is a sharp 100 m square just ahead of the camera, cascade 1 a wide one out to the end of the near
// pass. Each is a bounding square (so turning the camera doesn't change it) snapped to whole texels (so moving
// doesn't make the edges crawl).
int Renderer::planShadows(Vector3 camPos, Vector3 camFwd, Vector3 sunDir, float strength) {
    nCasc = quality == 2 ? 2 : (quality == 1 ? 1 : 0);
    shStrength = strength;
    if (strength <= 0.01f || sunDir.y < 0.03f) nCasc = 0;
    const float DIST = 1300.f;
    Vector3 up = fabsf(sunDir.y) > 0.98f ? V3(1, 0, 0) : V3(0, 1, 0);
    // light basis (matches MatrixLookAt)
    Vector3 zA = Vector3Normalize(sunDir), xA = Vector3Normalize(Vector3CrossProduct(up, zA)), yA = Vector3CrossProduct(zA, xA);
    Vector3 flat = Vector3Normalize({camFwd.x, 0.f, camFwd.z});
    for (int k = 0; k < nCasc; k++) {
        float hf = k == 0 ? (nCasc == 1 ? 90.f : 50.f) : 185.f;
        float ahead = k == 0 ? (nCasc == 1 ? 70.f : 38.f) : 150.f;
        half[k] = hf;
        Vector3 c = Vector3Add(camPos, Vector3Scale(flat, ahead));
        c.y = camPos.y - 2.f;
        float tex = 2 * hf / shSize;
        float a = Vector3DotProduct(c, xA), b = Vector3DotProduct(c, yA);
        c = Vector3Add(c, Vector3Add(Vector3Scale(xA, floorf(a / tex) * tex - a), Vector3Scale(yA, floorf(b / tex) * tex - b)));
        lightView[k] = MatrixLookAt(Vector3Add(c, Vector3Scale(zA, DIST)), c, up);
        Matrix proj = MatrixOrtho(-hf, hf, -hf, hf, 1.0, 2 * DIST);
        lightVP[k] = MatrixMultiply(lightView[k], proj);
    }
    float info[4] = {(float)nCasc, 2 * half[0] / shSize, 2 * half[1] / shSize, shStrength};
    SetShaderValue(city, locShInfo, info, SHADER_UNIFORM_VEC4);
    for (int k = 0; k < MAXC; k++) SetShaderValueMatrix(city, locLVP[k], lightVP[k]);
    return nCasc;
}

void Renderer::beginCascade(int k) {
    rlDrawRenderBatchActive();
    savedP = rlGetMatrixProjection();
    savedMV = rlGetMatrixModelview();
    glGetIntegerv(GL_VIEWPORT, vp);
    glBindFramebuffer(GL_FRAMEBUFFER, shFbo[k]);
    glViewport(0, 0, shSize, shSize);
    rlEnableDepthTest();   // off outside BeginMode3D, and without it nothing is written
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(2.2f, 4.0f);
    Matrix proj = MatrixOrtho(-half[k], half[k], -half[k], half[k], 1.0, 2 * 1300.0);
    rlSetMatrixProjection(proj);
    rlSetMatrixModelview(lightView[k]);
}

void Renderer::endCascade() {
    rlDrawRenderBatchActive();
    glDisable(GL_POLYGON_OFFSET_FILL);
    rlDisableDepthTest();
    rlSetMatrixProjection(savedP);
    rlSetMatrixModelview(savedMV);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
}

// ---------------------------------------------------------------- scene and post
void Renderer::beginScene(Color clear) {
    ensureTargets();
    rlDrawRenderBatchActive();
    glBindFramebuffer(GL_FRAMEBUFFER, msFbo);
    glViewport(0, 0, w, h);
    ClearBackground(clear);
    // 2D sky drawing must not disturb the bloom mask in alpha
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
}

void Renderer::opaque(bool on) {
    rlDrawRenderBatchActive();
    if (on) glDisable(GL_BLEND);
    else {
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    }
}

static void blit(RenderTexture2D& dst, Texture2D src, Shader& sh) {
    BeginTextureMode(dst);
    BeginShaderMode(sh);
    DrawTexturePro(src, {0, 0, (float)src.width, -(float)src.height}, {0, 0, (float)dst.texture.width, (float)dst.texture.height}, {0, 0}, 0, WHITE);
    EndShaderMode();
    EndTextureMode();
}

void Renderer::endScene() {
    rlDrawRenderBatchActive();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, scene.id);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (quality == 0) return;
    // bloom: bright mask at half size, then two blur passes at quarter size, kept at two widths
    glDisable(GL_BLEND);
    float tx[2] = {1.f / w, 1.f / h};
    SetShaderValue(extract, GetShaderLocation(extract, "texel"), tx, SHADER_UNIFORM_VEC2);
    blit(bloomA, scene.texture, extract);
    int ld = GetShaderLocation(blur, "dir");
    float dx[2] = {1.f / bloomB.texture.width, 0}, dy[2] = {0, 1.f / bloomB.texture.height};
    SetShaderValue(blur, ld, dx, SHADER_UNIFORM_VEC2);
    blit(bloomB, bloomA.texture, blur);
    SetShaderValue(blur, ld, dy, SHADER_UNIFORM_VEC2);
    blit(bloomC, bloomB.texture, blur);
    // a second, wider blur of the first one; A (free again) keeps it
    float dx2[2] = {2.5f / bloomB.texture.width, 0}, dy2[2] = {0, 2.5f / bloomB.texture.height};
    SetShaderValue(blur, ld, dx2, SHADER_UNIFORM_VEC2);
    blit(bloomB, bloomC.texture, blur);
    SetShaderValue(blur, ld, dy2, SHADER_UNIFORM_VEC2);
    blit(bloomA, bloomB.texture, blur);   // A now holds the wide glow (stretched to half size)
    glEnable(GL_BLEND);
}

void Renderer::present(const Grade& g) {
    rlDrawRenderBatchActive();
    glDisable(GL_BLEND);
    BeginShaderMode(post);
    SetShaderValue(post, GetShaderLocation(post, "gain"), &g.gain, SHADER_UNIFORM_VEC3);
    SetShaderValue(post, GetShaderLocation(post, "lift"), &g.lift, SHADER_UNIFORM_VEC3);
    float params[4] = {g.sat, g.contrast, quality == 0 ? 0.f : g.bloom, g.vignette};
    SetShaderValue(post, GetShaderLocation(post, "params"), params, SHADER_UNIFORM_VEC4);
    SetShaderValue(post, GetShaderLocation(post, "blur"), &g.blur, SHADER_UNIFORM_FLOAT);
    SetShaderValueTexture(post, GetShaderLocation(post, "bloomTex"), bloomC.texture);
    SetShaderValueTexture(post, GetShaderLocation(post, "bloomTex2"), bloomA.texture);
    DrawTexturePro(scene.texture, {0, 0, (float)w, -(float)h}, {0, 0, (float)GetScreenWidth(), (float)GetScreenHeight()}, {0, 0}, 0, WHITE);
    EndShaderMode();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
