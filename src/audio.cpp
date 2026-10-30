#include "audio.h"
#include <atomic>

static std::atomic<float> aRoll{0}, aGrind{0}, aWind{0};
static std::atomic<float> aTraffic{0}, aWaves{0}, aSeal{0}, aWindHi{0}, aMusic{0}, aRain{0};
constexpr int SR = 44100;

// Background ambience, one sample at a time. Everything is filtered noise or simple oscillators.
struct Ambience {
    uint32_t seed = 991;
    double t = 0;
    float lvT = 0, lvW = 0, lvS = 0, lvH = 0, lvM = 0, lvR = 0;   // smoothed levels
    float brown = 0, tlp = 0, wlp = 0, wlp2 = 0, hlp = 0, hlp2 = 0, rlp = 0, drop = 0;
    // sea lions: bursts of barks
    float barkT = 2.f, barkOn = 0, barkLen = 0.2f, barkF = 260, barkPh = 0, bp1 = 0, bp2 = 0;
    int barksLeft = 0;
    // music: a little four-chord party loop
    float mPh[4] = {0, 0, 0, 0}, kickPh = 0, mlp = 0;
    float white() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (seed & 0xFFFF) / 32768.f - 1.f; }

    float sample(float tT, float tW, float tS, float tH, float tM, float tR) {
        const float dt = 1.f / SR;
        t += dt;
        lvT += (tT - lvT) * 0.0002f; lvW += (tW - lvW) * 0.0002f; lvS += (tS - lvS) * 0.0002f;
        lvH += (tH - lvH) * 0.0002f; lvM += (tM - lvM) * 0.0003f; lvR += (tR - lvR) * 0.0002f;
        float w = white(), out = 0;
        float tf = (float)fmod(t, 1000.0);
        if (lvT > 0.002f) {   // traffic: a deep rumble that swells as cars pass, and a faint engine drone
            brown = brown * 0.995f + w * 0.02f;
            tlp += (brown - tlp) * 0.05f;
            float swell = 0.65f + 0.35f * sinf(tf * 0.9f) * sinf(tf * 0.31f + 1.f);
            out += (tlp * 5.5f + sinf(TAU_F * 58.f * tf) * 0.03f * swell) * swell * lvT;
        }
        if (lvW > 0.002f) {   // surf: noise that rises as each wave breaks and hisses as it runs out
            float a = 0.5f + 0.5f * sinf(tf * TAU_F / 6.3f + 0.8f * sinf(tf * 0.37f));
            float env = 0.2f + 0.8f * a * a * a;
            wlp += (w - wlp) * (0.025f + 0.12f * env);
            wlp2 += (wlp - wlp2) * 0.4f;
            out += wlp2 * 2.2f * env * lvW;
        }
        if (lvS > 0.01f) {    // sea lions: a gruff, buzzing "ork ork ork", then a pause
            barkT -= dt;
            if (barkT <= 0) {
                if (barksLeft <= 0) { barksLeft = 2 + (seed % 4); barkF = 200 + (seed >> 8) % 140; }
                barkOn = barkLen = 0.17f + ((seed >> 4) % 10) * 0.008f;
                barksLeft--;
                barkT = barksLeft > 0 ? barkOn + 0.12f : 1.5f + ((seed >> 12) % 100) * 0.05f;
            }
            if (barkOn > 0) {
                barkOn -= dt;
                barkPh += (barkF * (1.f + 0.06f * sinf(tf * 70.f))) * dt;
                float saw = 2.f * (barkPh - floorf(barkPh)) - 1.f;
                bp1 += 0.25f * (saw + w * 0.3f - bp1 - 0.6f * bp2);   // a crude resonant formant
                bp2 += 0.25f * bp1;
                float env = std::min(1.f, barkOn * 30.f) * std::min(1.f, (barkLen - barkOn) * 25.f);
                out += bp1 * 0.5f * env * lvS;
            }
        }
        if (lvH > 0.002f) {   // wind up high: a hollow, gusting whistle of band-limited noise
            float gust = 0.45f + 0.55f * fabsf(sinf(tf * 0.33f) * sinf(tf * 0.71f + 2.f));
            hlp += (w - hlp) * 0.08f;
            hlp2 += (hlp - hlp2) * 0.02f;
            out += (hlp - hlp2) * 1.9f * gust * lvH;
        }
        if (lvR > 0.002f) {   // rain: steady hiss plus the odd fat drop
            rlp += (w - rlp) * 0.35f;
            out += (w - rlp) * 0.22f * lvR;
            if (((seed >> 3) & 1023) == 0) drop = 1.f;
            drop *= 0.996f;
            out += drop * w * 0.25f * lvR;
        }
        if (lvM > 0.002f) {   // pedal bus music: I-V-vi-IV at 112 bpm with bass, chord stabs, kick and hi-hat
            const float beat = 60.f / 112.f;
            float bt = (float)fmod(t, beat * 16.0);
            int bar = (int)(bt / (beat * 4)), step = (int)(bt / (beat * 0.5f));
            float inBeat = fmodf(bt, beat), in8 = fmodf(bt, beat * 0.5f);
            static const float ROOT[4] = {130.81f, 98.0f, 110.0f, 87.31f};
            static const int MINOR[4] = {0, 0, 1, 0};
            float root = ROOT[bar];
            float third = root * (MINOR[bar] ? 1.1892f : 1.2599f), fifth = root * 1.4983f;
            float freqs[4] = {root * 0.5f, root * 2, third * 2, fifth * 2};
            float m = 0;
            for (int k = 0; k < 4; k++) {
                mPh[k] += freqs[k] * dt;
                mPh[k] -= floorf(mPh[k]);
            }
            float bass = (mPh[0] < 0.5f ? 1.f : -1.f) * 0.35f * expf(-in8 * 5.f);
            float stab = 0;
            if (step % 2 == 1) for (int k = 1; k < 4; k++) stab += (2 * mPh[k] - 1) * 0.12f * expf(-in8 * 9.f);
            kickPh += (50.f + 90.f * expf(-inBeat * 30.f)) * dt;
            float kick = sinf(TAU_F * kickPh) * expf(-inBeat * 9.f) * 0.7f;
            float hat = (step % 2 == 1) ? w * 0.12f * expf(-in8 * 40.f) : 0.f;
            m = bass + stab + kick + hat;
            mlp += (m - mlp) * 0.3f;   // a little muffled, as from a speaker on a moving bike
            out += mlp * 0.75f * lvM;
        }
        return out;
    }
};
static Ambience gAmb;

static void loopCallback(void* buffer, unsigned int frames) {
    static uint32_t seed = 12345;
    static float lp = 0, lp2 = 0, lpw = 0, ph = 0, ph2 = 0, rollS = 0, grindS = 0, windS = 0;
    short* out = (short*)buffer;
    float tr = aRoll.load(), tg = aGrind.load(), tw = aWind.load();
    for (unsigned i = 0; i < frames; i++) {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        float white = (seed & 0xFFFF) / 32768.f - 1.f;
        rollS += (tr - rollS) * 0.0005f;
        grindS += (tg - grindS) * 0.002f;
        windS += (tw - windS) * 0.0005f;
        // rolling: low rumble with a slow wobble
        lp += (white - lp) * (0.02f + 0.06f * rollS);
        lp2 += (lp - lp2) * 0.3f;
        ph += 2 * PI * (28 + 40 * rollS) / SR;
        float roll = (lp2 * 3.0f) * (0.75f + 0.25f * sinf(ph)) * rollS;
        // grind: bright metallic hiss with a ringing tone
        ph2 += 2 * PI * 1450 / SR;
        float grind = (white * 0.35f + sinf(ph2) * 0.12f + sinf(ph2 * 1.51f) * 0.08f) * grindS;
        // wind
        lpw += (white - lpw) * 0.01f;
        float wind = lpw * 4.0f * windS;
        float amb = gAmb.sample(aTraffic.load(std::memory_order_relaxed), aWaves.load(std::memory_order_relaxed), aSeal.load(std::memory_order_relaxed),
                                aWindHi.load(std::memory_order_relaxed), aMusic.load(std::memory_order_relaxed), aRain.load(std::memory_order_relaxed));
        float v = clampf((roll + grind + wind + amb) * 0.6f, -1, 1);
        out[i] = (short)(v * 30000);
    }
    if (ph > 2 * PI) ph -= 2 * PI;
    if (ph2 > 2 * PI * 100) ph2 = fmodf(ph2, 2 * PI);
}

static Sound makeSound(float dur, float (*gen)(float t, float& state, uint32_t& seed)) {
    int n = (int)(dur * SR);
    short* data = (short*)MemAlloc(n * sizeof(short));
    float state = 0;
    uint32_t seed = 777;
    for (int i = 0; i < n; i++) data[i] = (short)(clampf(gen((float)i / SR, state, seed), -1, 1) * 28000);
    Wave w{(unsigned)n, SR, 16, 1, data};
    Sound s = LoadSoundFromWave(w);
    UnloadWave(w);
    return s;
}

static float rnd(uint32_t& s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s & 0xFFFF) / 32768.f - 1.f; }

void Sfx::init() {
    InitAudioDevice();
    ok = IsAudioDeviceReady();
    if (!ok) return;
    pop = makeSound(0.18f, [](float t, float& st, uint32_t& s) {
        st += (rnd(s) - st) * 0.5f;
        return (st * 0.8f * expf(-t * 60) + sinf(2 * PI * 140 * t) * expf(-t * 30) * 0.9f);
    });
    land = makeSound(0.3f, [](float t, float& st, uint32_t& s) {
        st += (rnd(s) - st) * 0.25f;
        return (st * 0.9f * expf(-t * 25) + sinf(2 * PI * 70 * t) * expf(-t * 18));
    });
    clack = makeSound(0.06f, [](float t, float& st, uint32_t& s) {
        st += (rnd(s) - st) * 0.4f;
        return st * 0.5f * expf(-t * 90);
    });
    bail = makeSound(0.8f, [](float t, float& st, uint32_t& s) {
        st += (rnd(s) - st) * 0.12f;
        return (st * 1.5f * expf(-t * 4) + sinf(2 * PI * 55 * t) * expf(-t * 10) * 0.8f);
    });
    splash = makeSound(1.2f, [](float t, float& st, uint32_t& s) {
        st += (rnd(s) - st) * (0.08f + 0.3f * expf(-t * 6));
        return st * 2.0f * expf(-t * 2.5f) * (1 - expf(-t * 80));
    });
    honk = makeSound(0.45f, [](float t, float&, uint32_t&) {
        float a = fmodf(t * 415, 1.f) < 0.5f ? 1.f : -1.f, b = fmodf(t * 523, 1.f) < 0.5f ? 1.f : -1.f;
        return (a + b) * 0.22f * (t < 0.4f ? 1.f : (0.45f - t) * 20);
    });
    token = makeSound(0.7f, [](float t, float&, uint32_t&) {
        float f = t < 0.12f ? 988 : 1319;
        return sinf(2 * PI * f * t) * 0.5f * expf(-t * 5) + sinf(2 * PI * f * 2 * t) * 0.15f * expf(-t * 8);
    });
    siren = makeSound(1.4f, [](float t, float& ph, uint32_t&) {
        float f = 780 + 260 * sinf(TAU_F * t / 0.7f);
        ph += TAU_F * f / SR;
        float env = std::min(1.f, t * 20) * std::min(1.f, (1.4f - t) * 20);
        return (sinf(ph) * 0.4f + sinf(ph * 2) * 0.12f) * env;
    });
    gull = makeSound(0.9f, [](float t, float& ph, uint32_t& s) {   // "kee-ow, kyow": a falling cry, twice
        float syl = t < 0.42f ? t : t - 0.45f;
        if (syl < 0) return 0.f;
        float f = (t < 0.42f ? 1650.f : 1500.f) - 700.f * syl / 0.4f + 40.f * sinf(t * 180.f);
        ph += TAU_F * f / SR;
        float env = std::min(1.f, syl * 40.f) * std::max(0.f, 1.f - syl / 0.4f);
        return (sinf(ph) * 0.3f + sinf(ph * 2) * 0.18f + sinf(ph * 3) * 0.08f + rnd(s) * 0.04f) * env;
    });
    flutter = makeSound(0.9f, [](float t, float& st, uint32_t& s) {   // a flock of wings clattering up
        st += (rnd(s) - st) * 0.5f;
        float beat = 0.5f + 0.5f * sinf(t * TAU_F * 17.f);
        return st * beat * beat * 1.3f * expf(-t * 2.8f) * std::min(1.f, t * 60.f);
    });
    SetAudioStreamBufferSizeDefault(2048);
    loop = LoadAudioStream(SR, 16, 1);
    SetAudioStreamCallback(loop, loopCallback);
    PlayAudioStream(loop);
}

void Sfx::setLevels(float roll, float grind, float wind) {
    aRoll = clampf(roll, 0, 1);
    aGrind = clampf(grind, 0, 1);
    aWind = clampf(wind, 0, 1);
}

void Sfx::setAmbience(float traffic, float waves, float sealions, float windHigh, float music, float rain) {
    aTraffic = clampf(traffic, 0, 1);
    aWaves = clampf(waves, 0, 1);
    aSeal = clampf(sealions, 0, 1);
    aWindHi = clampf(windHigh, 0, 1);
    aMusic = clampf(music, 0, 1);
    aRain = clampf(rain, 0, 1);
}

void Sfx::play(Sound& s, float vol, float pitch) {
    if (!ok) return;
    SetSoundVolume(s, vol);
    SetSoundPitch(s, pitch);
    PlaySound(s);
}

void Sfx::shutdown() {
    if (!ok) return;
    UnloadAudioStream(loop);
    for (Sound* s : {&pop, &land, &bail, &splash, &honk, &token, &clack, &siren, &gull, &flutter}) UnloadSound(*s);
    CloseAudioDevice();
}
