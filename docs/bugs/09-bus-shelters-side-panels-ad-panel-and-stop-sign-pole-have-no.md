# Bus shelters: side panels, ad panel and stop-sign pole have no collision; roof isn't a ceiling

## What happens
Muni bus shelters only have collision for the back glass panel and the two front posts. The skater can ride straight through:

- both glass side panels
- the lit ad panel at one end (1.9 m tall)
- the stop-sign pole

The roof is not a ceiling either (only `PK_BUILDING`/`PK_DECK` count in `World::ceiling`), so an ollie under the shelter likely puts the skater's head through the roof. That last part wasn't tested.

## How it was found
A headless harness found 20 shelters (PK_POLE boxes 3.8 m long and 3.3 m tall). For each, it rode a skater lengthwise from 4.5 m outside one end through the middle of the shelter. **20 of 20**: the skater rode straight through both ends unblocked (e.g. the shelter at 642.5, -647.0 in Chinatown).

## Where
`src/world_city.cpp:1392` (`addClutter`, case 5). The side panels and ad panel are drawn with `m.obox(at(±L, ...))`, but only the back panel (`addPlat` at line ~1406) and `pole(a * L, -0.6f, ...)` (line 1407) get collision.

## Suggested fix
Add thin collision boxes for the two end panels and the stop-sign pole. Optionally add the roof as a `PK_DECK` box so `World::ceiling` sees it.
