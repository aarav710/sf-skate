# Grind balance controls are reversed compared with the HUD meter

## What happens
While grinding, the balance meter draws the marker at `sw/2 + balance * 115`. The help text and the code comment say "tap the opposite direction to stay on", but the input actually pushes balance the other way. Following the meter makes you fall.

| Strategy (40 seeded grinds on the Embarcadero bar) | Bailed "Lost balance" | Avg time on rail |
|---|---|---|
| Press away from the marker (what the HUD suggests) | 40/40 | 0.75 s |
| No input | 39/40 | 1.6 s |
| Press toward the marker | 0/40 | 2.65 s |

## Where
- `src/player.cpp:344`: `balanceVel += (... + balance * 2.4f - in.steer * 4.2f) * dt;`
- `src/main.cpp:1311`: `int mx = (int)(sw / 2 + P.balance * 115);`

Pressing D (`steer = +1`) moves balance negative, which moves the marker left, so it doesn't push it back from the right.

## Suggested fix
Flip one side: draw the marker at `sw/2 - balance * 115`, or use `+ in.steer * 4.2f`. Check by playing that "press toward the side you're falling to" (or away, whichever is intended) matches the HUD text.
