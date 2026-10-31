# Cars drive through each other (bridges, Market St, Embarcadero)

## What happens
Traffic cars regularly overlap each other's bodies by 1–2 m, sometimes for many seconds:

- **Same lane, same direction:** on the Golden Gate Bridge, two cars at ~18 m/s were found 0.6 m apart, sitting inside each other. Queues on the bridges pile up with stopped cars overlapping.
- **Crossing traffic:** path cars on Market Street / the Embarcadero (`mode == 1`) drive straight through grid cars (`mode == 0`) at intersections.

## How it was found
A headless harness ran `Traffic::update` for 60 s at each landmark and checked every pair of cars within 150 m of the skater with an oriented-box overlap test. It found 100+ distinct overlapping pairs, some on the Bay Bridge overlapping for a combined 30–50 s.

## Likely cause
`src/traffic.cpp:411`:
1. The car-ahead check only runs within 350 m of the skater (`if (c.placed && distP < 350)`). Cars further away never avoid each other, so they merge on long roads like the bridges and are still merged when you arrive.
2. Once two cars overlap, `along > 0 && along < gap` can't separate them: one car always sees the other behind it (`along < 0`).
3. Path cars never check traffic lights (that block is `mode == 0` only) or crossing traffic.

## Suggested fix
- Run a cheap same-path/lane following check for all cars.
- Push apart, or hold back, cars that overlap.
- Make path cars obey signals at grid intersections, or yield to crossing cars.
