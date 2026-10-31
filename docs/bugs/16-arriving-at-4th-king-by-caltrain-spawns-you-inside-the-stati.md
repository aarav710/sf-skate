# Arriving at 4th & King by Caltrain spawns you inside the station building

## What happens
When you ride Caltrain to San Francisco, the arrival point (1203.8, 950) is inside the 4th & King depot box (x 1180–1208, z 936–964, top 9.76). On the first frame, collision shoves the skater east onto the tracks, standing in the train's lane and facing the building wall. Pushing gives speed 0 until you turn around.

## How it was found
A headless harness used the arrival code path. A staged screenshot at the arrival point shows the skater between the rails, facing the red wall.

## Where
`src/main.cpp:647`:
```cpp
Vector3 p = W.trackPoint(ride.target, 6.2f, nullptr);
```
It uses a fixed lateral offset and a fixed yaw of `PI`, with no clearance check.

## Suggested fix
Use a per-station drop-off point on the platform, facing away from the building. Or search outward from the track point for a spot where `collide` is clear and the ground is level, and face along the platform.
