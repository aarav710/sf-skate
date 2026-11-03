// Glue between the game and the net client: things that need the game's types (World, Player), which net.cpp
// keeps out so the net tests and the relay build without raylib.
#pragma once
#include "net.h"
#include "player.h"

// A cheap hash of the generated world (counts and the hand-placed spots), so players with different builds
// are told so instead of skating in two different cities.
uint32_t worldFingerprint(const World& w);

// This player's state for PLAYER_STATE. pos is where they are (on a train, the train); vehicle is a PlayerState::Vehicle.
netp::PlayerState captureState(const Player& p, Vector3 pos, uint8_t vehicle);
