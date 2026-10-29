#include "audio.h"
#include <atomic>

static std::atomic<float> aRoll{0}, aGrind{0}, aWind{0};
constexpr int SR = 44100;

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
        float v = clampf((roll + grind + wind) * 0.6f, -1, 1);
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

void Sfx::play(Sound& s, float vol, float pitch) {
    if (!ok) return;
    SetSoundVolume(s, vol);
    SetSoundPitch(s, pitch);
    PlaySound(s);
}

void Sfx::shutdown() {
    if (!ok) return;
    UnloadAudioStream(loop);
    for (Sound* s : {&pop, &land, &bail, &splash, &honk, &token, &clack, &siren}) UnloadSound(*s);
    CloseAudioDevice();
}
