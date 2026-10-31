# Bailing always throws the board east, whatever direction you're facing

## What happens
When you bail, the board tumbles away with a fixed +1.5 m/s added along the **world x axis**, not the rider's heading:

- Bailing at a standstill always sends the board ~0.62 m **east**, whichever way you face.
- Bailing while heading north at 8 m/s skews the board ~13° east.

## How it was found
A headless harness triggered bails at standstill and while moving in each direction, and recorded where the board ended up.

## Where
`src/player.cpp:52` (`Player::bail`):
```cpp
boardVel = {f.x * 0.8f + 1.5f, 3.0f, f.z * 0.8f};
```

## Suggested fix
Add the kick along the heading or to the rider's side, e.g. `boardVel = f*0.8 + fwd()*1.5` (or the right vector `(-sin(yaw), cos(yaw)) * 1.5`), so it's consistent in every direction.
