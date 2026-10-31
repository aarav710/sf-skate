# Controls help panel text is cut off at the right edge (and bottom in small windows)

## What happens
The CONTROLS panel (H) is a fixed 314 px wide. `"B  free roam / cop chase   N  difficulty"` runs off the right edge of the screen and reads "N difficu…". In smaller windows (e.g. 960×540) the panel also runs off the bottom, because it starts at a fixed `hy = 270` and is 482 px tall.

## How to reproduce
Start at 1600×900, drop in, and look at the help panel.

## Where
`src/main.cpp:1371`:
```cpp
int hx = sw - 330, hy = 270;
DrawRectangle(hx, hy, 314, 482, {0, 0, 0, 140});
```

## Suggested fix
Size the panel from `MeasureText` of the widest line, place it at `sw - width - 16`, and clamp `hy` so it fits vertically.
