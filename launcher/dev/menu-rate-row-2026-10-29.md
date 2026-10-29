# The pause menu's MENU UPDATES row (2026-10-29)

`mcla_ui_menu_rate` (the rate the menus, pop-ups and map panels advance at, `KNOWN-ISSUES.md`, `menu-lag-2026-10-29.md`) is also a row of the pause menu's DISPLAY page, under HUD UPDATES, with the HUD's four values: 30, 60, 120 PER SECOND and EVERY FRAME (`src/settings_menu.cpp`, `kDisplayItems`; the PS5 build has no 120, as for HUD UPDATES). It is built, saved and applied like HUD UPDATES: stepping a row writes the option at once (the movie reads it at every advance), the option is noted as changed, and the page's save writes only the options changed through the menus.

## What changed with it

- DISPLAY has 16 rows (15 before). Pads that count them: `menu_input.py` (`ui_count == 16`, PEAK BRIGHTNESS is row 11, was 10) and `scripts/menu-pages.pad` (the bottom of DISPLAY is 15 presses of down, was 14).
- The page's width should not change: the longest label is GRAPHICS API (RESTART) (263 glyph pixels) and the longest value 120 PER SECOND (169), the movie sizes a list to those two; MENU UPDATES is about 160 and its values are HUD UPDATES'. Text level 2 shortens " PER SECOND" as it does for HUD UPDATES. No new short form was needed (computed, see Measured).
- Not added to the launcher. It would take one `GameSetting` in `launcher/src/GameSettings.cs` next to `HudRate` (key `mcla_ui_menu_rate`, label "Menu updates", the same four choices, default 30), its place in `GraphicsFor`, a sentence in the Info text, and the rows in `docs/settings.md` and `launcher/README.md`; no old-settings migration (the launcher never forced this one).

## The check (`menu_rate_row.py`, a chain of one build and four game runs)

- Pictures: `scripts/menu-pages.pad` at 3x and 1x (DISPLAY, PERFORMANCE, CONTROLS; Xbox and PlayStation glyphs; the layout log on), and `menu_rate_row.py pad --kind values` at 3x: the row selected, one picture at each value.
- Function: `menu_rate_row.py pad --kind function` at native resolution, a 120 cap, `--mcla_ui_advance_log=true`: the row is stepped right through its values and round, then left; `get mcla_ui_menu_rate` after each step; 3 s with the page open at 30, 60, 120 and EVERY FRAME, counting the log's `ui adv: PAUSEMOVIE` lines; the page is left at 60 and the run's `mcla.toml` is compared with the file it started with (only `mcla_ui_menu_rate = 60` may be new).

## Measured

Nothing yet: the chain has not run (2026-10-29). Not built or run: Linux and the PS5 (`src/settings_menu.cpp` is shared with both).
