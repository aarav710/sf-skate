# Falling into any pond or fountain says "Splash! Into the bay"

## What happens
All 198 ponds use the same bail message as the bay: the Marina pond, the Embarcadero Plaza fountain, even Menlo Park backyard pools at y ≈ 17 m all say **"Splash! Into the bay"**.

## Where
- `World::isWater` (`src/world_terrain.cpp:677`) counts ponds as water.
- `src/player.cpp:172` and `:282` hard-code `bail("Splash! Into the bay")`.

## Suggested fix
Have `isWater` report what kind of water it was, or add a `w.inPond(x, z)` check. Use a matching message, e.g. "Splash! Into the fountain" / "…the pool" / "…the pond".
