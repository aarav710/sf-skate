# Some jet skis are moored outside the play area; you can skate out of bounds and the HUD says "SFO Airport"

## What happens
4 of the 9 jet skis are moored outside `World::inPlay()`, e.g. at (4921, 2879) on the East Bay shore. Hopping off there leaves you skating freely outside the playable area. Everything with x > 1350 and z > 1550 is labelled "SFO Airport" by `district()`, so an empty East Bay beach shows "SFO Airport" in the HUD.

## How it was found
A headless harness checked `inPlay()` at each mooring and after `dismount()`. A staged screenshot shows an empty beach with the HUD reading "SFO Airport".

## Where / likely cause
- `src/jetski.cpp:7` and `:14–15`: moorings are searched over the whole terrain area (`WX0..WX1`, `WZ0..WZ1`), not the playable box.
- `src/player.cpp:85`: the out-of-bounds clamp only triggers when you cross from inside to outside (`!inPlay(pos) && inPlay(before)`). If you start outside, you're never pushed back.
- `src/world_terrain.cpp:433`: `if (x > AX0 + 50 && z > AZ0) return "SFO Airport";` catches far too much.

## Suggested fix
Only moor skis where `inPlay()` is true. Make `dismount` refuse shores outside the play area. Tighten the SFO district box.
