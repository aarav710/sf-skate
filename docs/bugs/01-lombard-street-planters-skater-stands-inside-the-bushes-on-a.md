# Lombard Street planters: skater stands inside the bushes on an invisible collision box

## What happens
The flower planters along the Lombard Street switchbacks have a collision box that doesn't match what's drawn:

- The visible soil bed is only ~0.12 m tall, but the collision box is **0.75 m tall**. The skater can ollie onto it and then stands on air, with the board buried in the shrub and leaves through the legs.
- The collision box is 1.2 × 1.2 m, but the shrub drawn on it is wider. Rolling into a planter stops the skater *inside* the foliage.

## How to reproduce
- Teleport to Lombard Street (key 4) and let the skater roll down into the hedges, or
- Stage it: `SFSKATE_AUTOTEST=1 SFSKATE_VIEW="397.38,-769.68,2.3,58.5" ./sfskate`. `shot_00.png` shows the board sunk into the bush.

## Expected
Collision matches the visuals.

## Where / likely cause
`src/world_city.cpp:139` (`World::buildLombard`):
```cpp
addPlat({p.x - 0.6f, p.x + 0.6f, p.y - 0.6f, p.y + 0.6f, gy + 0.75f, gy + 0.75f, gy - 0.3f, 0, PK_PROP});
mb(p.x, p.y).box({..., gy - 0.3f, ...}, {..., gy + 0.12f, ...}, ...); // soil bed
addShrub(p.x, p.y, 1.05f, r, ...);
```
The collision top (0.75) and the drawn bed top (0.12) disagree, and the shrub is bigger than the box.

## Suggested fix
Size the box to the drawn shrub (radius and height), or make the planter a full-height obstacle so the skater can't stand on it.
