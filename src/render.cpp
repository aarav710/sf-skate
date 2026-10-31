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
uniform sampler2DShadow shadowMap2;
uniform mat4 lightVP2;
uniform vec4 farInfo;      // the wide far cascade: on, world texel, -, -
#if !FAR
uniform vec4 lamps[16];    // street lamps near the camera: position, strength
uniform int lampN;
#endif
out vec4 finalColor;

float h21(vec2 p) { p = fract(p * vec2(123.34, 456.21)); p += dot(p, p + 45.32); return fract(p.x * p.y); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = h21(i), b = h21(i + vec2(1, 0)), c = h21(i + vec2(0, 1)), d = h21(i + vec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// ---- the far cascade (High): 2.9 km wide, one hardware-filtered tap. The far pass has only face normals from
// derivatives, so it leans on a big normal offset rather than a depth bias.
float farShadow(vec3 p, vec3 Ng) {
    if (farInfo.x < 0.5) return 1.0;
    float t = farInfo.y;
    vec4 l = lightVP2 * vec4(p + Ng * t * 1.6 + sunDir * t * 1.2, 1.0);
    float e = max(abs(l.x), abs(l.y));
    if (e >= 0.97) return 1.0;
    float s = texture(shadowMap2, l.xyz * 0.5 + 0.5 - vec3(0.0, 0.0, 0.00004));
    return mix(s, 1.0, smoothstep(0.8, 0.97, e));
}

// ---- sun shadows: two cascades, 4 hardware-filtered taps each
float taps(sampler2DShadow sm, vec3 c) {
    float t = shadowMisc.x * 0.75;
    return 0.25 * (texture(sm, c + vec3(-t, -t, 0.0)) + texture(sm, c + vec3(t, -t, 0.0)) +
                   texture(sm, c + vec3(-t, t, 0.0)) + texture(sm, c + vec3(t, t, 0.0)));
}
float sunShadow(vec3 p, vec3 Ng, float dist) {
    if (shadowInfo.x < 0.5) return 1.0;
    if (dist > 270.0) return mix(1.0, farShadow(p, Ng), shadowInfo.w);
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
    // past the near cascades: the far one, if there is one
    float tail = fade > 0.0 ? farShadow(p, Ng) : 1.0;
    return mix(1.0, mix(s, tail, fade), shadowInfo.w);
}

#if FAR
// aerial perspective: far off, colors pale and drift toward the haze, whatever the fog is doing
vec3 aerial(vec3 col, float dist, vec3 haze) {
    float k = smoothstep(800.0, 4000.0, dist) * 0.4;
    float l = dot(col, vec3(0.3, 0.59, 0.11));
    return mix(col, mix(vec3(l), haze, 0.6), k);
}
#else
// street lamps: a warm pool of light on the ground and the walls around each one
vec3 lampLight(vec3 p, vec3 N) {
    vec3 s = vec3(0.0);
    for (int i = 0; i < lampN; i++) {
        vec3 L = lamps[i].xyz - p;
        float d2 = dot(L, L);
        float reach = clamp(1.0 - d2 / 400.0, 0.0, 1.0);   // 20 m
        float ndl = max(dot(N, L) * inversesqrt(d2 + 1e-4), 0.0);
        s += vec3(lamps[i].w * reach * reach * ndl / (1.0 + d2 * 0.04));
    }
    return s * vec3(1.0, 0.7, 0.4);
}
#endif

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
    float specK = 0.0, specP = 32.0, bloom = 0.0, emisBloom = 0.8;
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
                // lit at night: each building (a 23 m cell) lights 20-40% of its windows, in its own mix of warm
                // lamps, neutral and cool office light, kept dim enough not to burn out into white
                vec2 bld = floor(wPos.xz / 23.0);
                if (h21(cell + bld * 7.13) < 0.2 + 0.2 * h21(bld * 3.17 + 0.5)) {
                    float tone = h21(bld * 5.3 + vec2(cell.y * 0.37, 0.0));
                    vec3 tint = tone < 0.55 ? vec3(1.0, 0.76, 0.48) : (tone < 0.8 ? vec3(0.95, 0.87, 0.72) : vec3(0.7, 0.84, 1.0));
                    emis = tint * (0.3 + 0.4 * h21(cell * 1.7)) * night * (m == 6 ? 0.85 : 1.0);
                    emisBloom = 0.45;
                }
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
        float sh = mix(1.0, farShadow(wPos, vec3(0.0, 1.0, 0.0)), shadowInfo.w);
#else
        float sh = sunShadow(wPos, vec3(0.0, 1.0, 0.0), dist);
#endif
        // sun (or moon) on the water: a tight glint up close that spreads into a glitter path further out
        float sp = mix(900.0, 120.0, smoothstep(30.0, 1500.0, dist)) * (1.0 + slick);
        vec3 spec = sunCol * (pow(max(dot(R, sunDir), 0.0), sp) * 3.0 + pow(max(dot(R, sunDir), 0.0), 12.0) * 0.05) * sh * (1.0 - wet * 0.7);
        col += spec;
        // whitecaps: the bay is windy, so the odd crest breaks white
        float cap = smoothstep(1.0, 1.5, crest + gust * 0.4) * smoothstep(0.55, 0.85, vnoise(p * 0.3 + vec2(t * 0.5, t * 0.2))) * (1.0 - slick);
        cap *= clamp(1.0 - dist / 900.0, 0.0, 1.0) * (1.0 - wet * 0.3);
        col = mix(col, vec3(0.9, 0.93, 0.94) * (ambCol * 1.1 + sunCol * 0.75 * sh), cap * 0.8);
#if FAR
        col = aerial(col, dist, fogColor(V));
#endif
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
    float sh = diff > 0.0 ? mix(1.0, farShadow(wPos, Ng), shadowInfo.w) : 1.0;
#else
    float sh = diff > 0.0 ? sunShadow(wPos, Ng, dist) : 1.0;
#endif
    // sky light from above; immediate-mode models (flat normals, lots of small downward faces) get a gentler falloff
    float hemi = useVN == 1 ? 0.62 + 0.38 * N.y : 0.72 + 0.28 * N.y;
    // shadows: long ones (a low sun behind Twin Peaks) thin out with distance into the haze, and what the sun
    // misses still gets sky light from the sun's side of the sky, cool blue, most of all with a low sun
    float lowSun = (1.0 - smoothstep(0.2, 0.5, sunDir.y)) * (1.0 - smoothstep(0.6, 0.9, night));
    sh = mix(1.0, sh, (1.0 - 0.7 * smoothstep(120.0, 2200.0, dist)) * (1.0 - 0.3 * lowSun * smoothstep(100.0, 1400.0, dist)));
    vec3 fill = vec3(0.72, 0.88, 1.2) * dot(ambCol, vec3(0.333)) * (0.15 + 0.45 * lowSun);
    vec3 light = ambCol * hemi * ao * 1.06 + (sunCol * sh + fill * (1.0 - sh)) * diff * (0.6 + 0.4 * ao);
    if (m == 10) {                       // rooms: warm lamp light and soft window light, warmer still at night
        vec3 day = (ambCol * 1.2 + sunCol * 0.2 + vec3(0.28, 0.22, 0.12)) * (0.88 + 0.12 * hemi);
        light = mix(day, vec3(1.0, 0.88, 0.7) * (0.9 + 0.1 * hemi), night);
    }
#if !FAR
    else if (lampN > 0 && m != 4) light += lampLight(wPos, N) * (0.6 + 0.4 * ao);
#endif
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
#if FAR
    col = aerial(col, dist, fogColor(V));
#endif
    col += emis;
    bloom += clamp(dot(emis, vec3(0.33)) * emisBloom, 0.0, emisBloom);
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

// ---------------------------------------------------------------- sky
// Drawn first, over the whole frame: a gradient from the horizon up, haze along the horizon warmed toward the sun
// (matching the fog on the city), the sun or moon with a halo, and a drifting layer of flat-shaded cumulus puffs,
// lit from the sun's side. Alpha is the bloom mask (1 - bloom), so the sun and moon glow.
// The cloud noise uses the same integer hash as hashu() so cloudAt() on the CPU can hide stars behind clouds.
#define SKY_NOISE R"(
float hash2(vec2 p) {
    uvec2 q = uvec2(ivec2(p));
    uint h = q.x * 374761393u + q.y * 668265263u;
    h = (h ^ (h >> 13u)) * 1274126177u;
    h ^= h >> 16u;
    return float(h & 0xffffffu) / 16777216.0;
}
float vnoise2(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash2(i), hash2(i + vec2(1, 0)), f.x), mix(hash2(i + vec2(0, 1)), hash2(i + vec2(1, 1)), f.x), f.y);
}
// cloud density: a broad, slow "weather" noise gathers the clouds into groups with clear sky between them; the
// coarse part (two octaves) shapes the clouds and lights them, and round puffs (below) lump up their edges
float cloudCoarse(vec2 uv) {
    return vnoise2(uv) * 0.55 + vnoise2(uv * 2.13 + vec2(17.0, 5.0)) * 0.27 + (vnoise2(uv * 0.43 + vec2(41.0, 13.0)) - 0.5) * 0.32;
}
// round puffs: 1 - distance to the nearest of one random point per cell, so the outline bulges out in rounded
// lobes with a notch between each, like a cumulus
float cloudPuffs(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    float d = 1.5;
    for (int y = -1; y <= 1; y++)
        for (int x = -1; x <= 1; x++) {
            vec2 c = vec2(x, y);
            vec2 o = vec2(hash2(i + c), hash2(i + c + vec2(57.0, 113.0)));
            d = min(d, length(c + o - f));
        }
    return 1.0 - d;
}
float cloudField(vec2 uv, float puffK) { return cloudCoarse(uv) + (cloudPuffs(uv * 3.1 + vec2(9.0, 31.0)) - 0.5) * puffK; }
)"
static const char* SKY_FS = "#version 330\n" SKY_NOISE R"(
uniform vec2 res;
uniform vec3 camF, camR, camU;
uniform vec2 lens;         // tan(fovy / 2) * aspect, tan(fovy / 2)
uniform vec3 sunDir, sunCol, top, horizon, fogCol;
uniform vec4 misc;         // night, time, rain, fog
uniform vec3 cloudK;       // coverage threshold, opacity, drift time
out vec4 finalColor;
void main() {
    vec2 ndc = gl_FragCoord.xy / res * 2.0 - 1.0;
    vec3 dir = normalize(camF + camR * ndc.x * lens.x + camU * ndc.y * lens.y);
    float night = misc.x, rain = misc.z, fogK = misc.w;
    float e = dir.y;
    float golden = (1.0 - smoothstep(0.2, 0.5, sunDir.y)) * (1.0 - smoothstep(0.6, 0.9, night));
    float sd = dot(dir, sunDir);

    // gradient, then haze at the horizon tinted toward the sun like the city's fog, a faint city glow at night
    vec3 col = mix(horizon, top, pow(clamp(e / 0.38, 0.0, 1.0), 0.75));
    float sunAmt = pow(max(sd, 0.0), 6.0);
    vec3 haze = mix(fogCol, sunCol * 1.05, sunAmt * (0.25 + 0.4 * golden) * (1.0 - night * 0.8));
    haze += vec3(0.07, 0.045, 0.02) * night;
    col = mix(col, haze, exp(-max(e, 0.0) * 22.0) * 0.4);
    col = mix(col, haze, smoothstep(0.0, -0.04, e));

    // sun by day, moon by night; overcast and fog hide them
    float moonK = smoothstep(0.55, 0.65, night);
    float vis = (1.0 - smoothstep(0.35, 0.6, rain)) * (1.0 - 0.75 * fogK);
    float rad = mix(0.04, 0.028, moonK);
    float disc = smoothstep(cos(rad * 1.12), cos(rad * 0.92), sd) * vis;
    vec3 discCol = mix(mix(vec3(1.0, 0.98, 0.92), vec3(1.0, 0.86, 0.65), golden * 0.6) * 3.0, vec3(0.92, 0.92, 0.86) * 1.15, moonK);
    float sdp = max(sd, 0.0);
    vec3 halo = mix(sunCol, vec3(0.3, 0.33, 0.42), moonK) * (pow(sdp, 170.0) * 0.4 + pow(sdp, 24.0) * 0.18 * golden) * vis;

    // clouds: a layer overhead, curved like a dome so clouds stay round toward the horizon instead of streaking.
    // Flat-shaded like the city: a crisp edge, a bright band along the side facing the sun, a body tone and a
    // darker underside (the side toward the horizon), in a cool tone so a low sun doesn't turn them all orange.
    float cloud = 0.0;
    vec3 cloudCol = vec3(0.0);
    if (e > 0.03 && cloudK.y > 0.0) {
        vec2 drift = cloudK.z * vec2(0.006, 0.0025);
        vec2 uv0 = dir.xz / (e + 0.25) * 2.3;
        vec2 uv = uv0 + drift;
        float n = cloudField(uv, 0.24 * (1.0 - 0.75 * rain));   // rain: smoother, one grey deck
        float thr = cloudK.x;
        float fw = fwidth(n) * 1.2 + 0.0005 + 0.05 * rain;   // a pixel or two: crisp, just not aliased
        cloud = smoothstep(thr - fw, thr + fw, n) * smoothstep(0.03, 0.09, e);
        if (cloud > 0.0) {
            vec2 toSun = normalize(sunDir.xz + vec2(1e-4)) * 0.13;
            vec2 down = normalize(uv0 + vec2(1e-4)) * 0.24;   // outward in the dome: toward the horizon
            float litEdge = 1.0 - smoothstep(thr - 0.03, thr + 0.05, cloudCoarse(uv + toSun));
            float under = 1.0 - smoothstep(thr - 0.06, thr + 0.06, cloudCoarse(uv + down));
            float core = smoothstep(thr + 0.02, thr + 0.16, cloudCoarse(uv));   // thick middles are a shade darker
            // day: white, soft grey below; golden hour: warm lit edges, dusty rose body, lavender-grey underside
            vec3 litC = mix(vec3(1.0, 1.0, 0.98), vec3(1.0, 0.8, 0.58) * 1.08, golden);
            vec3 bodyC = mix(vec3(0.91, 0.93, 0.97), vec3(0.76, 0.62, 0.64), golden);
            vec3 underC = mix(vec3(0.7, 0.75, 0.84), vec3(0.47, 0.44, 0.58), golden);
            litC += sunCol * pow(sdp, 24.0) * 0.35 * (1.0 - rain);   // brighter still right by the sun
            vec3 c = mix(bodyC, underC, core * 0.35);
            c = mix(c, underC, under * 0.85);
            c = mix(c, litC, litEdge * (1.0 - 0.5 * under));
            // rain: one low grey overcast
            vec3 grey = mix(horizon, top, 0.35) * vec3(0.92, 0.94, 0.98);
            c = mix(c, mix(grey * 1.12, grey * 0.8, under * 0.6 + core * 0.3), rain * 0.85);
            // low down they sink into the horizon haze
            c = mix(c, haze, (1.0 - smoothstep(0.03, 0.3, e)) * 0.5);
            // at night: dark blue shapes, just lighter than the sky, warmed a little by the city from below
            vec3 nightC = mix(vec3(0.035, 0.045, 0.08), vec3(0.075, 0.09, 0.15), litEdge) + vec3(0.05, 0.03, 0.015) * (1.0 - smoothstep(0.05, 0.4, e));
            cloudCol = mix(c, nightC, smoothstep(0.5, 0.9, night));
            cloud *= cloudK.y;
        }
    }
    // the sun and its halo stay in front: a cloud drifting over must not hide it
    col = mix(col, cloudCol, cloud) + halo;
    col = mix(col, discCol, disc);
    float bloom = disc * mix(0.8, 0.45, moonK) * (1.0 - fogK);
    finalColor = vec4(col, 1.0 - bloom);
})";

// ---------------------------------------------------------------- ambient occlusion (High)
// At half size from the near pass's depth: 10 taps on a spiral in a disc of about a metre, each counting how far
// the surface there rises above this pixel's plane (Alchemy-style). Taps much further than the radius (a person in
// front of a wall, the sky) are ignored, so silhouettes get no dark halo. Out: r = AO, g = view depth.
static const char* SSAO_FS = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;   // resolved depth, full size
uniform vec4 proj;            // tan(fovy / 2) * aspect, tan(fovy / 2), near, far
uniform vec2 texel;           // one full-size texel
out vec4 finalColor;
float linz(float d) { float z = d * 2.0 - 1.0; return 2.0 * proj.z * proj.w / (proj.w + proj.z - z * (proj.w - proj.z)); }
// snapped to a texel's centre, so the depth read and the position rebuilt from it belong to the same texel
vec3 viewPos(vec2 uv) {
    uv = (floor(uv / texel) + 0.5) * texel;
    return vec3((uv * 2.0 - 1.0) * proj.xy, -1.0) * linz(texture(texture0, uv).r);
}
void main() {
    vec2 uv = (floor(fragTexCoord / texel) + 0.5) * texel;
    float d = texture(texture0, uv).r;
    if (d > 0.99999) { finalColor = vec4(1.0, 1e4, 0.0, 1.0); return; }   // sky, or the far pass
    vec3 P = viewPos(uv);
    float z = -P.z;
    // face normal from the neighbors, taking the closer one each way so edges don't smear
    vec2 ex = vec2(texel.x * 2.0, 0.0), ey = vec2(0.0, texel.y * 2.0);
    vec3 a = viewPos(uv + ex) - P, b = P - viewPos(uv - ex);
    vec3 c = viewPos(uv + ey) - P, f = P - viewPos(uv - ey);
    vec3 N = normalize(cross(abs(a.z) < abs(b.z) ? a : b, abs(c.z) < abs(f.z) ? c : f));
    if (dot(N, P) > 0.0) N = -N;
    const float R = 1.0;
    vec2 rad = min(R / (2.0 * proj.xy * z), vec2(0.12));
    float ang = 6.2832 * fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
    float sum = 0.0;
    for (int i = 0; i < 10; i++) {
        float t = (float(i) + 0.6) / 10.0;
        float an = ang + float(i) * 2.39996;
        vec3 v = viewPos(uv + vec2(cos(an), sin(an)) * rad * t) - P;
        float vv = dot(v, v);
        sum += max(dot(v, N) - 0.02 - 0.002 * z, 0.0) / (vv + 0.03) * (1.0 - smoothstep(R * R, 4.0 * R * R, vv));
    }
    float ao = clamp(1.0 - 0.2 * sum, 0.0, 1.0);
    ao = mix(ao, 1.0, smoothstep(70.0, 170.0, z));   // only up close: further out a metre is a pixel or two
    finalColor = vec4(ao, z, 0.0, 1.0);
})";
// separable blur that stops at depth edges
static const char* AOBLUR_FS = R"(#version 330
in vec2 fragTexCoord;
uniform sampler2D texture0;
uniform vec2 dir;
out vec4 finalColor;
void main() {
    vec2 c = texture(texture0, fragTexCoord).rg;
    float s = 0.0, ws = 0.0;
    for (int i = -3; i <= 3; i++) {
        vec2 v = texture(texture0, fragTexCoord + dir * float(i)).rg;
        float w = exp(-float(i * i) / 8.0) * exp(-abs(v.g - c.g) / (0.03 * c.g + 0.05));
        s += v.r * w;
        ws += w;
    }
    finalColor = vec4(s / ws, c.g, 0.0, 1.0);
})";

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
        s += min(c.rgb, vec3(1.6)) * (1.0 - c.a);   // HDR: a little headroom, but the sun mustn't flood the frame
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
uniform sampler2D aoTex;      // half size: AO, view depth
uniform sampler2D depthTex;   // full size depth of the near pass
uniform vec4 aoInfo;          // on, near, far, -
uniform vec2 aoTexel;
uniform float hdrOn;
out vec4 finalColor;
float linz(float d) { float z = d * 2.0 - 1.0; return 2.0 * aoInfo.y * aoInfo.z / (aoInfo.z + aoInfo.y - z * (aoInfo.z - aoInfo.y)); }
// AO up to full size: the four nearest half-size values, weighted toward those at this pixel's depth
float aoAt(vec2 uv) {
    float d = texture(depthTex, uv).r;
    if (d > 0.99999) return 1.0;
    float z = linz(d);
    vec2 b = uv / aoTexel - 0.5, f = fract(b);
    vec2 b0 = (floor(b) + 0.5) * aoTexel;
    float s = 0.0, ws = 0.0;
    for (int i = 0; i < 4; i++) {
        vec2 o = vec2(i & 1, i >> 1);
        vec2 a = texture(aoTex, b0 + o * aoTexel).rg;
        float w = (o.x > 0.5 ? f.x : 1.0 - f.x) * (o.y > 0.5 ? f.y : 1.0 - f.y) * exp(-abs(a.g - z) / (0.02 * z + 0.05)) + 1e-4;
        s += a.r * w;
        ws += w;
    }
    return s / ws;
}
// HDR (High): colors below 0.8 pass as they are; above that the brightest channel rolls off smoothly (keeping the
// hue, so warm lights stay warm instead of clipping to white), and only very bright light burns toward white
vec3 shoulder(vec3 c) {
    float m = max(c.r, max(c.g, c.b));
    if (m <= 0.8) return c;
    float t = 0.8 + (m - 0.8) / (1.0 + (m - 0.8) / 0.3);
    return mix(c * (t / m), vec3(t), smoothstep(1.5, 6.0, m) * 0.7);
}
void main() {
    vec2 uv = fragTexCoord;
    vec4 c0 = texture(texture0, uv);
    vec3 c = c0.rgb;
    if (blur > 0.0005) {   // speed: streak the edges of the frame outward from the middle
        vec2 d = uv - vec2(0.5);
        float k = blur * smoothstep(0.1, 0.6, length(d));
        for (int i = 1; i < 7; i++) c += texture(texture0, uv - d * k * float(i)).rgb;
        c /= 7.0;
    }
    if (aoInfo.x > 0.5) c *= mix(1.0, aoAt(uv), c0.a);   // lit surfaces only, not lamps and windows
    c += (texture(bloomTex, uv).rgb * 0.6 + texture(bloomTex2, uv).rgb * 0.8) * params.z;
    c = c * gain + lift * (1.0 - c);
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = mix(vec3(l), c, params.x);
    c = (c - 0.5) * params.y + 0.5;
    vec2 v = uv - 0.5;
    c *= 1.0 - params.w * dot(v, v) * 1.6;
    if (hdrOn > 0.5) c = shoulder(max(c, vec3(0.0)));
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
    sky = LoadShaderFromMemory(nullptr, SKY_FS);
    ssao = LoadShaderFromMemory(nullptr, SSAO_FS);
    aoBlur = LoadShaderFromMemory(nullptr, AOBLUR_FS);
    locUseVN = GetShaderLocation(city, "useVN");
    locShadow[0] = GetShaderLocation(city, "shadowMap0");
    locShadow[1] = GetShaderLocation(city, "shadowMap1");
    locShadow[2] = GetShaderLocation(city, "shadowMap2");
    locLVP[0] = GetShaderLocation(city, "lightVP0");
    locLVP[1] = GetShaderLocation(city, "lightVP1");
    locLVP[2] = GetShaderLocation(city, "lightVP2");
    locShInfo = GetShaderLocation(city, "shadowInfo");
    // the shadow maps live on texture units 10 to 12, out of the way of raylib's own (the far pass only reads 12)
    for (int k = 0; k < MAXC; k++) {
        int u = 10 + k;
        SetShaderValue(city, locShadow[k], &u, SHADER_UNIFORM_INT);
    }
    int u2 = 12;
    SetShaderValue(cityFar, GetShaderLocation(cityFar, "shadowMap2"), &u2, SHADER_UNIFORM_INT);
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
    bool wantHdr = q == 2;
    if (samples != msSamples || wantHdr != hdr) freeTargets();
    msSamples = samples;
    hdr = wantHdr;
    ensureTargets();
}

void Renderer::freeTargets() {
    if (!msFbo) return;
    glDeleteFramebuffers(1, &msFbo);
    glDeleteRenderbuffers(1, &msColor);
    glDeleteRenderbuffers(1, &msDepth);
    msFbo = msColor = msDepth = 0;
    glDeleteFramebuffers(1, &dsFbo);
    glDeleteTextures(1, &dsTex);
    dsFbo = dsTex = 0;
    for (auto* t : {&scene, &bloomA, &bloomB, &bloomC, &aoA, &aoB}) UnloadRenderTexture(*t);
    w = h = 0;
}

// A color-only target: 8-bit, or half-float for HDR
static RenderTexture2D makeTarget(int w, int h, bool half, bool linear) {
    RenderTexture2D t{};
    glGenTextures(1, &t.texture.id);
    glBindTexture(GL_TEXTURE_2D, t.texture.id);
    glTexImage2D(GL_TEXTURE_2D, 0, half ? GL_RGBA16F : GL_RGBA8, w, h, 0, GL_RGBA, half ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    t.texture.width = w;
    t.texture.height = h;
    t.texture.mipmaps = 1;
    t.texture.format = half ? PIXELFORMAT_UNCOMPRESSED_R16G16B16A16 : PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    glGenFramebuffers(1, &t.id);
    glBindFramebuffer(GL_FRAMEBUFFER, t.id);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.texture.id, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) TraceLog(LOG_WARNING, "SFSKATE: render target incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return t;
}

void Renderer::ensureTargets() {
    int W = GetRenderWidth(), H = GetRenderHeight();
    if (W == w && H == h && msFbo) return;
    freeTargets();
    w = W; h = H;
    glGenRenderbuffers(1, &msColor);
    glBindRenderbuffer(GL_RENDERBUFFER, msColor);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, msSamples, hdr ? GL_RGBA16F : GL_RGBA8, w, h);
    glGenRenderbuffers(1, &msDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, msDepth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, msSamples, GL_DEPTH_COMPONENT24, w, h);
    glGenFramebuffers(1, &msFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, msFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, msColor);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, msDepth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) TraceLog(LOG_WARNING, "SFSKATE: scene target incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    scene = makeTarget(w, h, hdr, true);
    bloomA = makeTarget(std::max(1, w / 2), std::max(1, h / 2), hdr, true);
    bloomB = makeTarget(std::max(1, w / 4), std::max(1, h / 4), hdr, true);
    bloomC = makeTarget(std::max(1, w / 4), std::max(1, h / 4), hdr, true);
    // ambient occlusion: a plain depth texture to resolve the near pass's depth into, and two half-size buffers
    glGenTextures(1, &dsTex);
    glBindTexture(GL_TEXTURE_2D, dsTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, w, h, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenFramebuffers(1, &dsFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, dsFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, dsTex, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) TraceLog(LOG_WARNING, "SFSKATE: depth resolve target incomplete");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    aoA = makeTarget(std::max(1, w / 2), std::max(1, h / 2), true, false);
    aoB = makeTarget(std::max(1, w / 2), std::max(1, h / 2), true, false);
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

void Renderer::setLamps(const Vector4* l, int n) {
    n = std::min(n, MAXLAMPS);
    if (n > 0) SetShaderValueV(city, GetShaderLocation(city, "lamps"), l, SHADER_UNIFORM_VEC4, n);
    SetShaderValue(city, GetShaderLocation(city, "lampN"), &n, SHADER_UNIFORM_INT);
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
        shProj[k] = MatrixOrtho(-hf, hf, -hf, hf, 1.0, 2 * DIST);
        lightVP[k] = MatrixMultiply(lightView[k], shProj[k]);
    }
    // the far cascade (High): a 2.9 km square around the camera with only the static city in it, so it is drawn
    // again only once the camera has gone 300 m from where it was centred, or the sun has moved
    farCasc = quality == 2 && nCasc > 0;
    if (farCasc) {
        const float FH = 1450.f, FD = 3000.f;
        float tex = 2 * FH / shSize;
        bool moved = sq(camPos.x - farAt.x) + sq(camPos.z - farAt.z) > 300.f * 300.f;
        if (moved || Vector3DotProduct(farSun, sunDir) < 0.999999f || farAt.x > 1e8f) {
            farDirty = true;
            farAt = {camPos.x, 0.f, camPos.z};
            farSun = sunDir;
            Vector3 c = farAt;
            float a = Vector3DotProduct(c, xA), b = Vector3DotProduct(c, yA);
            c = Vector3Add(c, Vector3Add(Vector3Scale(xA, floorf(a / tex) * tex - a), Vector3Scale(yA, floorf(b / tex) * tex - b)));
            half[2] = FH;
            lightView[2] = MatrixLookAt(Vector3Add(c, Vector3Scale(zA, FD)), c, up);
            shProj[2] = MatrixOrtho(-FH, FH, -FH, FH, 1.0, 2 * FD);
            lightVP[2] = MatrixMultiply(lightView[2], shProj[2]);
        }
    }
    float info[4] = {(float)nCasc, 2 * half[0] / shSize, 2 * half[1] / shSize, shStrength};
    float far[4] = {farCasc ? 1.f : 0.f, 2 * half[2] / shSize, 0, 0};
    SetShaderValue(city, locShInfo, info, SHADER_UNIFORM_VEC4);
    for (int k = 0; k < MAXC; k++) SetShaderValueMatrix(city, locLVP[k], lightVP[k]);
    SetShaderValue(city, GetShaderLocation(city, "farInfo"), far, SHADER_UNIFORM_VEC4);
    SetShaderValue(cityFar, GetShaderLocation(cityFar, "shadowInfo"), info, SHADER_UNIFORM_VEC4);
    SetShaderValue(cityFar, GetShaderLocation(cityFar, "farInfo"), far, SHADER_UNIFORM_VEC4);
    SetShaderValueMatrix(cityFar, GetShaderLocation(cityFar, "lightVP2"), lightVP[2]);
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
    rlSetMatrixProjection(shProj[k]);
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

// the cloud field's threshold: scattered cumulus when it's dry, overcast in the rain
static float cloudCover(float rain) { return lerpf(0.51f, 0.255f, rain); }

void Renderer::drawSky(const SkyLook& k) {
    lastSky = k;
    rlDrawRenderBatchActive();
    glDisable(GL_BLEND);   // writes the bloom mask in alpha too
    BeginShaderMode(sky);
    auto set = [&](const char* n, const void* v, int t) { SetShaderValue(sky, GetShaderLocation(sky, n), v, t); };
    float res[2] = {(float)w, (float)h}, lens[2] = {k.tanHalf * k.aspect, k.tanHalf};
    float misc[4] = {k.night, k.time, k.rain, k.fogK};
    float cov = cloudCover(k.rain);   // overcast in the rain
    float ck[3] = {cov, (1.f - 0.85f * k.fogK) * lerpf(0.95f, 0.85f, k.night), k.time};
    set("res", res, SHADER_UNIFORM_VEC2);
    set("camF", &k.fwd, SHADER_UNIFORM_VEC3);
    set("camR", &k.right, SHADER_UNIFORM_VEC3);
    set("camU", &k.up, SHADER_UNIFORM_VEC3);
    set("lens", lens, SHADER_UNIFORM_VEC2);
    set("sunDir", &k.sunDir, SHADER_UNIFORM_VEC3);
    set("sunCol", &k.sunCol, SHADER_UNIFORM_VEC3);
    set("top", &k.top, SHADER_UNIFORM_VEC3);
    set("horizon", &k.horizon, SHADER_UNIFORM_VEC3);
    set("fogCol", &k.fog, SHADER_UNIFORM_VEC3);
    set("misc", misc, SHADER_UNIFORM_VEC4);
    set("cloudK", ck, SHADER_UNIFORM_VEC3);
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), WHITE);
    EndShaderMode();
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
}

// the sky shader's cloud cover, on the CPU (same hash and noise)
static float vnoise2(float x, float y) {
    float ix = floorf(x), iy = floorf(y), fx = x - ix, fy = y - iy;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    auto H = [](float a, float b) { return (hashu((int)a, (int)b) & 0xffffff) / 16777216.f; };
    return lerpf(lerpf(H(ix, iy), H(ix + 1, iy), fx), lerpf(H(ix, iy + 1), H(ix + 1, iy + 1), fx), fy);
}
static float cloudPuffs(float x, float y) {
    float ix = floorf(x), iy = floorf(y), fx = x - ix, fy = y - iy, d = 1.5f;
    for (int b = -1; b <= 1; b++)
        for (int a = -1; a <= 1; a++) {
            float ox = (hashu((int)(ix + a), (int)(iy + b)) & 0xffffff) / 16777216.f;
            float oy = (hashu((int)(ix + a + 57), (int)(iy + b + 113)) & 0xffffff) / 16777216.f;
            d = std::min(d, sqrtf(sq(a + ox - fx) + sq(b + oy - fy)));
        }
    return 1 - d;
}
float Renderer::cloudAt(Vector3 d) const {
    const SkyLook& k = lastSky;
    if (d.y <= 0.03f) return 0;
    float u = d.x / (d.y + 0.25f) * 2.3f + k.time * 0.006f, v = d.z / (d.y + 0.25f) * 2.3f + k.time * 0.0025f;
    float n = vnoise2(u, v) * 0.55f + vnoise2(u * 2.13f + 17.f, v * 2.13f + 5.f) * 0.27f + (vnoise2(u * 0.43f + 41.f, v * 0.43f + 13.f) - 0.5f) * 0.32f +
              (cloudPuffs(u * 3.1f + 9.f, v * 3.1f + 31.f) - 0.5f) * 0.24f * (1 - 0.75f * k.rain);
    float cov = cloudCover(k.rain);
    float w = 0.02f + 0.05f * k.rain;
    return smooth01(cov - w, cov + w, n) * smooth01(0.03f, 0.09f, d.y);
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
    if (quality == 2) ambientOcclusion();
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

void Renderer::ambientOcclusion() {
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, msFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dsFbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    float th = tanf(viewFovy * 0.5f * DEG2RAD);
    float proj[4] = {th * w / (float)h, th, viewNear, viewFar}, tx[2] = {1.f / w, 1.f / h};
    SetShaderValue(ssao, GetShaderLocation(ssao, "proj"), proj, SHADER_UNIFORM_VEC4);
    SetShaderValue(ssao, GetShaderLocation(ssao, "texel"), tx, SHADER_UNIFORM_VEC2);
    Texture2D depthT = {dsTex, w, h, 1, PIXELFORMAT_UNCOMPRESSED_R32};
    blit(aoA, depthT, ssao);
    int ld = GetShaderLocation(aoBlur, "dir");
    float dx[2] = {1.f / aoA.texture.width, 0}, dy[2] = {0, 1.f / aoA.texture.height};
    SetShaderValue(aoBlur, ld, dx, SHADER_UNIFORM_VEC2);
    blit(aoB, aoA.texture, aoBlur);
    SetShaderValue(aoBlur, ld, dy, SHADER_UNIFORM_VEC2);
    blit(aoA, aoB.texture, aoBlur);
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
    float aoInfo[4] = {quality == 2 ? 1.f : 0.f, viewNear, viewFar, 0}, aoTx[2] = {1.f / aoA.texture.width, 1.f / aoA.texture.height};
    float hdrOn = hdr ? 1.f : 0.f;
    SetShaderValue(post, GetShaderLocation(post, "aoInfo"), aoInfo, SHADER_UNIFORM_VEC4);
    SetShaderValue(post, GetShaderLocation(post, "aoTexel"), aoTx, SHADER_UNIFORM_VEC2);
    SetShaderValue(post, GetShaderLocation(post, "hdrOn"), &hdrOn, SHADER_UNIFORM_FLOAT);
    SetShaderValueTexture(post, GetShaderLocation(post, "aoTex"), aoA.texture);
    SetShaderValueTexture(post, GetShaderLocation(post, "depthTex"), Texture2D{dsTex, w, h, 1, PIXELFORMAT_UNCOMPRESSED_R32});
    DrawTexturePro(scene.texture, {0, 0, (float)w, -(float)h}, {0, 0, (float)GetScreenWidth(), (float)GetScreenHeight()}, {0, 0}, 0, WHITE);
    EndShaderMode();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
