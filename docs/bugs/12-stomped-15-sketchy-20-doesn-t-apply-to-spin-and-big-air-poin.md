# STOMPED +15% / sketchy -20% doesn't apply to spin and Big Air points

## What happens
The landing quality modifier is applied to `comboPts` **before** the spin (FS/BS 180, 360…) and "Big Air" tricks are added. Those points are never scaled.

Example from a headless run: Ollie + FS 180 to Fakie, stomped, paid **+830** instead of **+920**. A kickflip combo is scaled correctly (+1380), because the flip is added before landing.

## Where
`src/player.cpp:302–306`:
```cpp
if (stomp) { comboPts *= 1.15f; landNote = "STOMPED +15%"; }
else if (sketchy) { comboPts *= 0.8f; ... }
...
if (halves > 0) addTrick(... 300.f * halves);
if (airTime > 1.6f) addTrick("Big Air", 400);
```

## Suggested fix
Add the spin and Big Air tricks first, then apply the stomp/sketchy multiplier.
