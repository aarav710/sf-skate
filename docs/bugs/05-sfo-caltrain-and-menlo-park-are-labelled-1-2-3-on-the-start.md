# SFO, Caltrain and Menlo Park are labelled 1/2/3 on the start screen and map, but those keys go elsewhere

## What happens
The landmark list numbers entries with `(i + 1) % 10`, so the 11th–13th landmarks show as "1 SFO Airport", "2 Caltrain 4th & King" and "3 Menlo Park". Pressing 1/2/3 actually goes to Embarcadero, Golden Gate and Twin Peaks. The map markers use the same formula, so the map shows duplicate 1/2/3 markers.

## Where
- `src/main.cpp:1253` (picker list): `DrawText(TextFormat("%d", (i + 1) % 10), ...)`
- `src/main.cpp:214` (map markers): `snprintf(b, sizeof b, "%d", (int)((i + 1) % 10));`

## Suggested fix
Only draw numbers for `i < 10`. Draw no number, or a plain dot, for the rest.
