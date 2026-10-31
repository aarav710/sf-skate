# House interiors: TV, sofa back, chairs, plant and floor lamp have no collision; skater rides through them

## What happens
Inside the enterable houses, several pieces of furniture are drawn but marked non-solid, so the skater rides straight through them:

| Item | Height | Solid? |
|---|---|---|
| TV cabinet + TV | up to 1.1 m | no |
| Sofa backrest | 0.45–0.95 m | no |
| Dining chairs | 0.45 m | no |
| Potted plant | ~0.9 m | no |
| Floor lamp | 1.6 m | no |

The sofa seat (0.45 m) and coffee table (0.42 m) are solid but below the 0.5 m `STEP_UP`. You can roll up onto the sofa seat and then pass through its backrest.

## How to reproduce
- Headless: in house 100 (x 109–116.4, z -1015…-996), push from (115.0, -1000) toward -z. The skater slides along the partition straight through the TV cabinet's space (x 114.4–115.6, z -1003.43…-1003.93) and stops at (112.45, -1007.0).
- Visual: `SFSKATE_AUTOTEST=1 SFSKATE_VIEW="115.0,-1003.6,-1.5708,6.5" ./sfskate`. `shot_00.png` shows the skater standing inside the TV cabinet with legs and board through it.

## Where
`src/world_city.cpp`, `World::addHouseInterior` (starts at line 858). The `aabb(..., solid)` calls pass `false` for:
- the sofa back (~953)
- the floor lamp stem (957)
- the chairs (~1000)
- the TV cabinet and TV (~1004–1010)
- the pot and plant (1025)

## Suggested fix
Pass `solid = true` for the TV cabinet/TV, sofa back, plant pot and floor lamp. Give the chairs a small solid footprint. Consider raising the sofa seat and coffee table above `STEP_UP`, or treating them as full-height obstacles.
