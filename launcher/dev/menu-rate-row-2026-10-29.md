# The pause menu's MENU UPDATES row (2026-10-29)

**Superseded before v0.3.1:** the row (and HUD UPDATES) was replaced by one SMOOTH UI row (`ui-interpolation-2026-10-29.md`, round 3); `menu_rate_row.py` became `smooth_ui_row.py`. Kept as the record of what was measured for it.

`mcla_ui_menu_rate` (the rate the menus, pop-ups and map panels advance at, `KNOWN-ISSUES.md`, `menu-lag-2026-10-29.md`) is also a row of the pause menu's DISPLAY page, under HUD UPDATES, with the HUD's four values: 30, 60, 120 PER SECOND and EVERY FRAME (`src/settings_menu.cpp`, `kDisplayItems`; the PS5 build has no 120, as for HUD UPDATES). It is built, saved and applied like HUD UPDATES: stepping a row writes the option at once (the movie reads it at every advance), the option is noted as changed, and the page's save writes only the options changed through the menus.

## What changed with it

- DISPLAY has 16 rows (15 before). Pads that count them: `menu_input.py` (`ui_count == 16`, PEAK BRIGHTNESS is row 11, was 10) and `scripts/menu-pages.pad` (the bottom of DISPLAY is 15 presses of down, was 14).
- The page's width should not change: the longest label is GRAPHICS API (RESTART) (263 glyph pixels) and the longest value 120 PER SECOND (169), the movie sizes a list to those two; MENU UPDATES is about 160 and its values are HUD UPDATES'. Text level 2 shortens " PER SECOND" as it does for HUD UPDATES. No new short form was needed (computed, see Measured).
- Not added to the launcher. It would take one `GameSetting` in `launcher/src/GameSettings.cs` next to `HudRate` (key `mcla_ui_menu_rate`, label "Menu updates", the same four choices, default 30), its place in `GraphicsFor`, a sentence in the Info text, and the rows in `docs/settings.md` and `launcher/README.md`; no old-settings migration (the launcher never forced this one).

## The check (`menu_rate_row.py`, a chain of one build and four game runs)

- Pictures: `scripts/menu-pages.pad` at 3x and 1x (DISPLAY, PERFORMANCE, CONTROLS; Xbox and PlayStation glyphs; the layout log on), and `menu_rate_row.py pad --kind values` at 3x: the row selected, one picture at each value.
- Function: `menu_rate_row.py pad --kind function` at native resolution, a 120 cap, `--mcla_ui_advance_log=true`: the row is stepped right through its values and round, then left; `get mcla_ui_menu_rate` after each step; 3 s with the page open at 30, 60, 120 and EVERY FRAME, counting the log's `ui adv: PAUSEMOVIE` lines; the page is left at 60 and the run's `mcla.toml` is compared with the file it started with (only `mcla_ui_menu_rate = 60` may be new).

## Measured

The chain `menurow` (2026-10-29): RTX 5080, Direct3D 12, the build of `feat/menu-rate-row` a1eaf74 (with `mcla_ui_advance_on_input` still off by default, which the row does not depend on). **4 of 4 checks passed.**

- Function (native resolution, a 120 FPS cap): DISPLAY opened with 16 rows. `get mcla_ui_menu_rate` after each step was 30, 60, 120, 0, 30, 0, 120, 60, 60 (right goes round from EVERY FRAME to 30, left from 30 to EVERY FRAME), and the menu's own log lines read 60, 120, 0, 30, 0, 120, 60. With the page open for 3 s at each value, `PAUSEMOVIE` advanced 30.0, 60.0, 120.0 and 120.0 times a second at 30, 60, 120 and EVERY FRAME (the game ran 120.0 updates a second). The page was left at 60: the run's `mcla.toml` got one new line, `mcla_ui_menu_rate = 60`, and lost none.
- Width (the movie's own `menu.width`, cap 750), at 1x and at 3x: DISPLAY 653 with 16 rows, the same as the code without the row (widest label GRAPHICS API (RESTART) 263 px, widest value 120 PER SECOND 169 px: the row's texts are not the longest); PERFORMANCE 719 with 19 rows. No page fell back to text level 2. CONTROLS gave no width line (the page was open under 0.6 s).
- Pictures: `scripts/menu-pages.pad` at 3x and at 1x (18 pictures each; Xbox and PlayStation glyphs) and the row at its four values at 3x. Looked at by hand: the four values, and the bottom of DISPLAY at 3x with Xbox glyphs: the hint row whole, the scroll bar visible, MENU UPDATES with its label and value aligned with the rows around it, the last row clear of the frame. The other pictures were judged by their widths only (the contact sheets are in the work folder, `C:\mcv\menurow\sheets`).
- Not measured: Vulkan; the launcher (no row there); Linux and the PS5 (not built or run, as above).
