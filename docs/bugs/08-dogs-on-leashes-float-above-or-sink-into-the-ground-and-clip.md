# Dogs on leashes float above or sink into the ground and clip into walls

## What happens
A walker's dog is drawn at a fixed offset (0.95 m ahead, 0.5 m to the side) at the **walker's** height, with no ground or wall check. On SF's hills and along building fronts:

- Dogs near the skater were **floating or sunk by more than 15 cm about 23% of the time** (11.8% floating, 11.5% sunk). Some were sunk by 0.4–0.6 m, enough to bury the whole dog in the sidewalk.
- **About 1.7%** of the time the dog was inside a wall or pole.

## How it was found
A headless harness ran `Traffic::update` at 10 landmarks for 30 s each. It sampled dogs within 150 m of the skater (1,259 samples) and compared the dog position with `World::ground` and `World::collide`. Example: dog of ped 603 at (-198.1, 5.3, 105.5) was in a wall and 0.61 m below the ground (Western Addition).

## Where
`src/traffic.cpp:834` (`drawDog`), line 838:
```cpp
Vector3 at = sit ? W(0.15f, 0, 0.65f) : W(0.95f, 0, 0.5f);   // y = walker's y
```

## Suggested fix
Snap the dog to `w.ground(at.x, at.z, p.pos.y + 0.5f)`, or store the dog position in `Ped` during the update. Keep the dog on the street side of the walker (flip the side offset if `collide` reports a wall), or pull it in toward the walker when blocked.
