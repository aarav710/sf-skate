// Procedurally generated sound: wheel roll, grind, wind, and one-shot effects.
#pragma once
#include "common.h"

struct Sfx {
    bool ok = false;
    Sound pop{}, land{}, bail{}, splash{}, honk{}, token{}, clack{}, siren{};
    AudioStream loop{};
    void init();
    void shutdown();
    // continuous sound levels, 0..1
    void setLevels(float roll, float grind, float wind);
    void play(Sound& s, float vol = 1.f, float pitch = 1.f);
};
