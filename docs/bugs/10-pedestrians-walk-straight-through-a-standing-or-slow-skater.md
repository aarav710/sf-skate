# Pedestrians walk straight through a standing or slow skater

## What happens
When the skater stands still or rolls slowly, people walking along the sidewalk pass straight through the skater's body.

## How it was found
A headless harness put the skater 3 m ahead of a walking pedestrian, on their route, at 18 locations, standing still for 12 s each. It counted pedestrians whose centre came within 0.3 m of the skater's centre. Result: **19 walk-throughs in 18 trials** (Embarcadero, Russian Hill, Alamo Square, Richmond, Financial District, Chinatown, Telegraph Hill…).

## Likely cause
`src/traffic.cpp:773` in `Traffic::interact`:
```cpp
if (pl.hspeed() < 2.0f) continue;
```
Below 2 m/s (~4.5 mph) there's no skater–pedestrian collision at all. Pedestrian dodging is also only triggered by a moving skater, so walkers keep to their route through a stationary one.

## Suggested fix
At low speed, have the pedestrian step around the skater (reuse the dodge offset), or at least push them apart. Keep the knock/bail behaviour for higher speeds.
