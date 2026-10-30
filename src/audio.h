// Procedurally generated sound: wheel roll, grind, wind, and one-shot effects.
#pragma once
#include "common.h"

struct Sfx {
    bool ok = false;
    Sound pop{}, land{}, bail{}, splash{}, honk{}, token{}, clack{}, siren{}, gull{}, flutter{}, bell{}, coo{};
    AudioStream loop{};
    void init();
    void shutdown();
    // continuous sound levels, 0..1
    void setLevels(float roll, float grind, float wind);
    // the city around you, 0..1 each
    struct Ambience {
        float traffic = 0, waves = 0, sealions = 0, windHigh = 0, music = 0, rain = 0;
        float chatter = 0;   // people talking nearby (sidewalks, pedal buses, cable cars and streetcars)
        float voices = 0;    // how many are talking at once, 0..1
        float leaves = 0;    // trees rustling
        float birds = 0;     // songbirds in the trees (daytime)
        float crickets = 0;  // crickets in the grass (night)
    };
    void setAmbience(const Ambience& a);
    void play(Sound& s, float vol = 1.f, float pitch = 1.f);
};
