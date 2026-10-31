# Number-key teleport while on a jet ski or seaplane doesn't get you off it (jet ski ends up buried in Twin Peaks)

## What happens
Pressing a number key (1–0) while riding a jet ski, a seaplane or Caltrain calls `P.spawn(...)` but never ends the ride:

- **Jet ski:** teleport to Twin Peaks while riding. You stay "riding" at **y = 0, 158 m inside the hill**. The jet ski moves there too and is left buried inside Twin Peaks after you hop off.
- **Seaplane:** the next `SP.update` puts the player back at the plane's position, so the teleport is silently undone. The game still shows "Teleported".
- **Caltrain:** probably the same (not tested). The ride code keeps setting the position while `ride.on` is true.

## How to reproduce
Headless, using the real `JetSkis`/`Seaplanes` code: mount ski 0, then `P.spawn(W.spots[2])`, then `JS.update`. Result: `riding=1 pos=-282.0,0.0,878.0 terrain=157.7 ski0 at -282.0,0.0,878.0`. With the seaplane, one frame after the teleport: `riding=1 pos=339.8,0.0,-1152.4`, i.e. back at the plane.

In game: ride a jet ski, press 3.

## Where / likely cause
`src/main.cpp:512` onward (number-key teleport loop) only does `P.spawn` and the camera reset. In contrast:
- `startAt()` (Tab / start screen) clears `ride.on`, `JS.riding/cur` and `SP.riding/cur`.
- R reset handles the jet ski and seaplane.

## Suggested fix
Reuse the dismount logic from `startAt()` in the number-key teleport: leave the jet ski moored where it was, put the seaplane back at its mooring, and end the Caltrain ride. Alternatively, ignore teleport keys while riding.
