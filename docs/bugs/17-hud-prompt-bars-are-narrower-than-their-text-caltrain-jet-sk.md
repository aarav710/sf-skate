# HUD prompt bars are narrower than their text (Caltrain, jet ski, seaplane)

## What happens
Three prompt boxes at the bottom of the screen have fixed widths that are smaller than their text, so the text spills past the box. Measured with raylib's `MeasureText`:

| Prompt | Text width | Box width | Line |
|---|---|---|---|
| "Press E to ride Caltrain to San Francisco (4th & King)" | 612 px | 520 px | `src/main.cpp:1407` |
| Jet ski controls ("W go   S brake   A D steer   Shift boost   E hop off at the shore") | 680 px | 540 px | `src/main.cpp:1387` |
| Seaplane controls line | 814 px | 660 px | `src/main.cpp:1395` |

## Suggested fix
Compute the box width from `MeasureText(text, size) + padding`. A small helper like `promptBar(text, size, color)` would fix all three.
