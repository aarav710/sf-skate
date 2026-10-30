#include "audio.h"
#include <atomic>

static std::atomic<float> aRoll{0}, aGrind{0}, aWind{0};
// ambience levels set by the game each frame, read by the audio thread
enum { A_TRAFFIC, A_WAVES, A_SEAL, A_WINDHI, A_MUSIC, A_RAIN, A_CHAT, A_VOICES, A_LEAVES, A_BIRDS, A_CRICKETS, A_COUNT };
static std::atomic<float> aLv[A_COUNT];
constexpr int SR = 44100;

// Background ambience, one sample at a time. Everything is filtered noise or simple oscillators.
// One person talking: a buzzing voice source through two vowel resonances (formants), in syllables and
// phrases with pauses. Several at once, muffled a little, read as a murmur of conversation with no words.
struct Voice {
    float f0 = 120, base = 120, ph = 0, age = 1, len = 0.2f, wait = 0;
    float F1 = 500, F2 = 1500, tF1 = 500, tF2 = 1500;
    float l1 = 0, b1 = 0, l2 = 0, b2 = 0, src = 0;
    int left = 0;          // syllables left in this phrase
    bool female = false;
};
// A songbird: phrases of short whistled notes, each a quick sweep or trill, then a rest.
struct Songbird {
    float wait = 1, ph = 0, age = 1, len = 0, gap = 0, f = 3000, sweep = 0, trill = 0, base = 3000;
    int notes = 0;
};

struct Ambience {
    uint32_t seed = 991;
    double t = 0;
    float lvT = 0, lvW = 0, lvS = 0, lvH = 0, lvM = 0, lvR = 0;   // smoothed levels
    float lvC = 0, lvV = 0, lvL = 0, lvB = 0, lvK = 0;
    // talking
    Voice voice[6];
    float clp = 0, clp2 = 0;
    // sea: a slow deep wash under the breaking waves
    float slp = 0, slp2 = 0;
    // leaves: a soft hiss and a crackle of small grains, both riding the gusts
    float llp = 0, llp2 = 0, grain = 0;
    // birdsong and crickets
    Songbird song[3];
    float crPh = 0, crT = 0, crT2 = 0.37f;
    float rnd01() { return (white() + 1.f) * 0.5f; }
    float brown = 0, tlp = 0, wlp = 0, wlp2 = 0, hlp = 0, hlp2 = 0, rlp = 0, drop = 0;
    // sea lions: bursts of barks
    float barkT = 2.f, barkOn = 0, barkLen = 0.2f, barkF = 260, barkPh = 0, bp1 = 0, bp2 = 0;
    int barksLeft = 0;
    // music: a little four-chord party loop
    float mPh[4] = {0, 0, 0, 0}, kickPh = 0, mlp = 0;
    float white() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (seed & 0xFFFF) / 32768.f - 1.f; }

    float sample(const float* lv) {
        const float dt = 1.f / SR;
        t += dt;
        lvT += (lv[A_TRAFFIC] - lvT) * 0.0002f; lvW += (lv[A_WAVES] - lvW) * 0.0002f; lvS += (lv[A_SEAL] - lvS) * 0.0002f;
        lvH += (lv[A_WINDHI] - lvH) * 0.0002f; lvM += (lv[A_MUSIC] - lvM) * 0.0003f; lvR += (lv[A_RAIN] - lvR) * 0.0002f;
        lvC += (lv[A_CHAT] - lvC) * 0.0002f; lvV += (lv[A_VOICES] - lvV) * 0.0001f; lvL += (lv[A_LEAVES] - lvL) * 0.0001f;
        lvB += (lv[A_BIRDS] - lvB) * 0.0001f; lvK += (lv[A_CRICKETS] - lvK) * 0.0001f;
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
            // and under it the deep, steady wash of open water
            slp += (w - slp) * 0.004f;
            slp2 += (slp - slp2) * 0.01f;
            out += (slp - slp2) * 5.f * (0.7f + 0.3f * sinf(tf * 0.21f)) * lvW;
        }
        if (lvC > 0.002f) out += talking(dt) * lvC;
        if (lvL > 0.002f) {   // leaves: high, airy noise that swells with each gust, with a crackle of leaves knocking together
            float gust = 0.35f + 0.65f * powf(fabsf(sinf(tf * 0.23f + 1.3f) * sinf(tf * 0.57f)), 0.7f);
            llp += (w - llp) * 0.3f;
            float hp = w - llp;               // brighter than the rest of the city
            llp2 += (hp - llp2) * 0.5f;
            if (rnd01() < 0.001f + 0.008f * gust * gust) grain = std::max(grain, 0.3f + 0.7f * rnd01());
            grain *= 0.992f;
            out += llp2 * (0.1f * gust * gust + 0.7f * grain) * lvL;
        }
        if (lvB > 0.002f) for (auto& b : song) out += birdsong(b, dt) * lvB;
        if (lvK > 0.002f) {   // two crickets, each a 4.6 kHz chirp of three quick pulses, a little out of step
            crPh += 4600.f * dt;
            crPh -= floorf(crPh);
            crT += dt; crT2 += dt;
            if (crT > 0.62f) crT -= 0.62f;
            if (crT2 > 0.71f) crT2 -= 0.71f;
            auto pulses = [](float u) {   // three 25 ms pulses, 35 ms apart
                float q = fmodf(u, 0.035f);
                return u < 0.105f && q < 0.025f ? sinf(q / 0.025f * PI) : 0.f;
            };
            out += sinf(TAU_F * crPh) * (pulses(crT) * 0.1f + pulses(crT2) * 0.07f) * lvK;
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

    float talking(float dt) {
        // vowel formants (F1, F2): ah, eh, ee, oh, oo, a (cat), uh
        static const float VOW[7][2] = {{730, 1090}, {530, 1840}, {300, 2200}, {570, 840}, {320, 900}, {660, 1720}, {520, 1190}};
        int n = 1 + (int)(lvV * 5.f + 0.5f);   // how many people are talking
        float sum = 0;
        for (int k = 0; k < 6; k++) {
            Voice& v = voice[k];
            v.age += dt;
            if (v.age >= v.len) {   // next syllable, or a pause between phrases
                if (v.wait > 0) { v.wait -= dt; if (k >= n) continue; }
                else if (v.left <= 0) {
                    v.wait = 0.3f + rnd01() * 2.2f;
                    v.left = 2 + (int)(rnd01() * 9);
                    v.female = rnd01() < 0.5f;
                    v.base = v.female ? 185.f + rnd01() * 50.f : 100.f + rnd01() * 35.f;
                    continue;
                } else {
                    if (k >= n) continue;
                    v.left--;
                    v.age = 0;
                    v.len = 0.11f + rnd01() * 0.17f;
                    const float* vw = VOW[(int)(rnd01() * 6.99f)];
                    float s = v.female ? 1.15f : 1.f;
                    v.tF1 = vw[0] * s; v.tF2 = vw[1] * s;
                    // the voice rises and falls through a phrase and drops at its end
                    v.f0 = v.base * (0.9f + 0.25f * rnd01()) * (v.left == 0 ? 0.85f : 1.f);
                }
            }
            if (v.age >= v.len) continue;
            float env = std::min(1.f, v.age * 50.f) * std::min(1.f, (v.len - v.age) * 22.f);
            v.F1 += (v.tF1 - v.F1) * 0.004f;
            v.F2 += (v.tF2 - v.F2) * 0.004f;
            v.ph += v.f0 * (1.f + 0.02f * sinf(v.age * 30.f)) * dt;
            v.ph -= floorf(v.ph);
            float buzz = 1.f - 2.f * v.ph;                        // glottal buzz
            float hiss = v.age < 0.035f ? white() * 0.8f : 0.f;   // a consonant at the start of each syllable
            v.src += (buzz + hiss - v.src) * 0.18f;   // a soft, breathy buzz rather than a harsh one
            // two resonant filters at the vowel's formants
            float f1 = TAU_F * v.F1 / SR, f2 = TAU_F * v.F2 / SR;   // 2 sin(pi f / SR), close enough below 3 kHz
            v.l1 += f1 * v.b1; float h1 = v.src - v.l1 - 0.25f * v.b1; v.b1 += f1 * h1;
            v.l2 += f2 * v.b2; float h2 = v.src - v.l2 - 0.3f * v.b2; v.b2 += f2 * h2;
            sum += (v.b1 + 0.6f * v.b2) * env * (k < 2 ? 1.f : 0.7f);
        }
        clp += (sum - clp) * 0.3f;   // across the street, not in your ear: the highs fall away
        clp2 += (clp - clp2) * 0.3f;
        return clp2 * 0.24f;
    }

    float birdsong(Songbird& b, float dt) {
        if (b.notes <= 0) {
            b.wait -= dt;
            if (b.wait > 0) return 0;
            b.notes = 3 + (int)(rnd01() * 7);
            b.base = 2400.f + rnd01() * 3000.f;
            b.age = b.len = 0;
            b.gap = 0;
        }
        b.age += dt;
        if (b.age >= b.len + b.gap) {   // next note of the phrase
            if (--b.notes <= 0) { b.wait = 0.4f + rnd01() * 2.6f; return 0; }
            b.age = 0;
            b.len = 0.04f + rnd01() * 0.09f;
            b.gap = 0.03f + rnd01() * 0.06f;
            b.f = b.base * (0.85f + 0.3f * rnd01());
            b.sweep = (rnd01() - 0.45f) * 2.f;   // up or down
            b.trill = rnd01() < 0.3f ? 60.f + rnd01() * 50.f : 0.f;
        }
        if (b.age >= b.len) return 0;
        float u = b.age / b.len;
        float f = b.f * (1.f + 0.35f * b.sweep * u) * (1.f + (b.trill > 0 ? 0.08f * sinf(TAU_F * b.trill * b.age) : 0.f));
        b.ph += f * dt;
        b.ph -= floorf(b.ph);
        float env = sinf(PI * u);
        return sinf(TAU_F * b.ph) * env * env * 0.15f;
    }
};
static Ambience gAmb;

static void loopCallback(void* buffer, unsigned int frames) {
    static uint32_t seed = 12345;
    static float lp = 0, lp2 = 0, lpw = 0, ph = 0, ph2 = 0, rollS = 0, grindS = 0, windS = 0;
    short* out = (short*)buffer;
    float tr = aRoll.load(), tg = aGrind.load(), tw = aWind.load();
    float lv[A_COUNT];
    for (int k = 0; k < A_COUNT; k++) lv[k] = aLv[k].load(std::memory_order_relaxed);
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
        float amb = gAmb.sample(lv);
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
    bell = makeSound(1.3f, [](float t, float&, uint32_t&) {   // the cable car gripman's bell: "clang-clang ... clang-clang"
        static const float hits[4] = {0.f, 0.17f, 0.5f, 0.67f};
        float o = 0;
        for (float h : hits) {
            float u = t - h;
            if (u < 0) continue;
            float e = expf(-u * 7.f) * std::min(1.f, u * 900.f);
            o += (sinf(TAU_F * 1040.f * u) * 0.35f + sinf(TAU_F * 2650.f * u) * 0.18f * expf(-u * 6.f) +
                  sinf(TAU_F * 4180.f * u) * 0.1f * expf(-u * 14.f)) * e;
        }
        return o * 0.8f;
    });
    coo = makeSound(1.2f, [](float t, float& lp, uint32_t& s) {   // a pigeon: a soft, throaty "hoo-oo-oo"
        static const float st[3] = {0.f, 0.32f, 0.62f}, ln[3] = {0.26f, 0.24f, 0.5f}, f[3] = {440.f, 400.f, 360.f};
        float o = 0;
        for (int k = 0; k < 3; k++) {
            float u = t - st[k];
            if (u < 0 || u > ln[k]) continue;
            float e = sinf(PI * u / ln[k]);
            float fr = f[k] * (1.f - 0.12f * u / ln[k]);
            o += (sinf(TAU_F * fr * u) + 0.3f * sinf(TAU_F * 2 * fr * u)) * e * (0.8f + 0.2f * sinf(u * 160.f));
        }
        lp += (o + rnd(s) * 0.05f - lp) * 0.2f;
        return lp * 0.5f;
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

void Sfx::setAmbience(const Ambience& a) {
    const float v[A_COUNT] = {a.traffic, a.waves, a.sealions, a.windHigh, a.music, a.rain, a.chatter, a.voices, a.leaves, a.birds, a.crickets};
    for (int k = 0; k < A_COUNT; k++) aLv[k].store(clampf(v[k], 0, 1), std::memory_order_relaxed);
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
    for (Sound* s : {&pop, &land, &bail, &splash, &honk, &token, &clack, &siren, &gull, &flutter, &bell, &coo}) UnloadSound(*s);
    CloseAudioDevice();
}
