# Clipping a pole mid-air deflects you and causes a misleading "Landed sideways" bail

## What happens
Ollie at ~10 m/s (22 mph) and brush a lamp post while airborne. The air collision slides the velocity along the pole but leaves the board's `yaw` unchanged. The skater then lands with the board crosswise to the motion and bails with **"Landed sideways"**, even with no steering input.

## How to reproduce
Headless bot at Embarcadero Plaza: ollie heading `yaw=0.77` at 10 m/s next to the lamp post at x 1184.85–1185.15, z -222.85…-222.55.
- At frame ~108 the horizontal speed drops from 10.0 to 5.8 and the velocity turns sideways.
- `yaw` stays at 0.77.
- On landing: `bail("Landed sideways")`.

## Expected
Either treat the hit as a crash ("Flew into a wall" / "Hit a pole"), or turn the board with the deflection, as `ride()` already does with `yaw = lerpAngle(yaw, ny, 0.5f)`.

## Where
`src/player.cpp:228` (`Player::air`):
```cpp
if (w.collide(pos, 0.35f, pos.y, 1.7f, &n)) {
    float into = -(vel.x * n.x + vel.z * n.y);
    if (into > 9.f) { bail("Flew into a wall"); return; }
    if (into > 0) { vel.x += n.x * into; vel.z += n.y * into; }   // velocity changes, yaw doesn't
}
```
