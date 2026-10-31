# Grind that slows to a stop shifts the skater along x only (off the Golden Gate, or into railings)

## What happens
When a grind slows below 1 m/s, the skater is dropped off the rail by `pos.x += -dir.y * 0.6f`. That offset is only applied to x, always to the same side:

- On the Golden Gate's east railing (x = -1086.2, a north-south rail) the skater is moved **0.6 m outside the deck**, falls from y ≈ 33 and bails "Splash! Into the bay".
- On east-west rails (Vista Point, the Bay Bridge railings; 42 rails) the offset is **0.00 m**, so the skater ends up standing inside the 1 m railing.

## How it was found
Headless grinds that were allowed to slow to a stop on each rail.

## Where
`src/player.cpp:350` (`Player::grind`):
```cpp
if (speed < 1.0f) {
    state = AIR; vel = {...}; ...
    pos.x += -dir.y * 0.6f;   // no z component, no choice of side
```

## Suggested fix
Offset along the full perpendicular `(-dir.y, dir.x) * 0.6f * side`. Pick `side` by testing both sides with `w.ground`/`w.collide`, and prefer the side that has ground and no wall.
