// Procedurally generated sound: wheel roll, grind, wind, and one-shot effects.
#pragma once
#include "common.h"

struct Sfx {
    bool ok = false;
    Sound pop{}, land{}, bail{}, splash{}, honk{}, token{}, clack{}, siren{}, gull{}, flutter{};
    AudioStream loop{};
    void init();
    void shutdown();
    // continuous sound levels, 0..1
    void setLevels(float roll, float grind, float wind);
    // the city around you, 0..1 each: traffic hum, surf, sea lions barking, wind up high and on the bridges,
    // music from a passing pedal bus, rain
    void setAmbience(float traffic, float waves, float sealions, float windHigh, float music, float rain);
    void play(Sound& s, float vol = 1.f, float pitch = 1.f);
};
