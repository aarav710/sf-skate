# Chase camera goes inside gondola terminals (and other large PK_PROP boxes)

## What happens
With the skater standing beside the Vista Point gondola terminal, the camera ends up inside the terminal. The screen is all walls and the skater can't be seen.

## How it was found
A headless check of the camera target against the world showed the camera inside the terminal's box. A staged screenshot at the terminal shows the camera inside it.

## Where / likely cause
- Gondola terminals are added as `PK_PROP` (`src/gondola.cpp:133`).
- The camera's collision test `World::solidAt` (`src/world_terrain.cpp:627`) only considers `PK_BUILDING` and `PK_DECK`, so it ignores them. There are 9 such large prop boxes, including the Oracle Park wall and a box in the Marina.

## Suggested fix
Make `solidAt` also treat tall `PK_PROP` boxes (e.g. taller than ~2 m) as solid for the camera, or add the terminals as `PK_BUILDING`.
