# Landing bounce: Space is ignored, and steering or flipping during it bails a clean landing

## What happens
Every landing harder than 4.5 m/s (that includes a normal flat-ground ollie) puts the skater back into the `AIR` state for a small bounce, with all air controls live:

- Pressing Space 1–16 frames (0.02–0.27 s) after landing does **nothing**. The ollie only registers from frame ~19.
- Holding A/D during the bounce spins the board. After a "Stomped!" ollie on a flat street the skater bails with "Landed sideways" ~0.27 s later.
- Pressing J/K/L during the bounce starts a flip that can't finish, and you bail with "Didn't finish the flip".

## How it was found
A headless harness drove `Player` with input timed relative to the landing frame.

## Where
`src/player.cpp:322`, in `land()`:
```cpp
if (impact > 4.5f && state == RIDE) { ... state = AIR; ... }
```
`ride()` only reads `in.ollie` in `RIDE`, and `air()` applies spin and starts flips.

## Suggested fix
- Treat the bounce as part of riding: add a short "settle" timer in `RIDE` that only moves the visual board/body, or
- buffer the ollie press and fire it when the bounce lands, and ignore spin input and flips during the bounce (or make them harmless).
