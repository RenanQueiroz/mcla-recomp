# The pause menu's settings pages: why they were cut off (2026-10-29)

What this is: how the cause was found and what is known about the pause movie's layout, for whoever changes those pages again. The player-facing summary is in KNOWN-ISSUES.md ("The pause menu's settings pages were cut off on the right").

## The report

Screenshots from the fork's owner at 3x (Direct3D 12, a DualSense): DISPLAY and PERFORMANCE had the hint row cut off at a straight edge, the frame around the menu going on past it, the last visible row's label low, and an empty bar under the hint row. The Settings list itself (the game's own tab) was fine, and so is an older 1x picture of the Settings tab; an older 1x picture of PERFORMANCE has the same cut.

## How it was read

The pause menu is a Scaleform movie (`PAUSEMOVIE`, resources/ui/pause in `xarchive_cache.rpf`). Its ActionScript 2 code was read out of the archive with throwaway scripts (extract the guest image from `default.xex`, find the archive's table-of-contents key in the image, read the entry, undo its LZX stream, disassemble the byte code into text). Those scripts are not in the repository: they handle the game's own files and keys. What follows is what the code says, in the movie's own variable names.

## What the code does

- The list's width is `menu.width = max(widthpanel_min, list_intercol_all)`: `widthpanel_min` is 480 for the pause movie's list templates, and `list_intercol_all` is the widest label, plus the widest value where rows have choices (their children's names), plus fixed margins (55 a side), taken over every row of the page (`menu.count`), not only the six in view (`List_max` is 6 for the pause movie). A scroll bar adds 10. The text is measured by the game for "Dev Gothic" at size 30 (an FSCommand, MeasureString).
- The hint row (`allprompts`) is placed against the menu's right edge (`allprompts._x = menu.width`) and built leftwards from there; the hint bar (`prompt_bg`) lies under the list across the whole width.
- The frame and the glass are 3D meshes scaled by `cur_width` (it follows `menu.width` by a third a frame); the movie itself is drawn into a picture of its own, which is mapped onto a quad. Whatever the movie draws beyond the picture's width is cut off, at a straight edge, while the meshes go on.
- With `tabs_count` 0 (what the game's own sub-menus and these pages set) the title and its icon are shown instead of the tab strip, and the list starts 60 units down.
- The movie is seen at an angle: rows and the bar rise to the right. A wider panel rises more over its width, so a label at the left of a row sits lower than its value at the right (5, 10, 15 and 17 px for the 3rd to 6th rows of DISPLAY at the screenshot's size; it grows down the page, which fits a perspective effect; not checked beyond that).

## What was measured (2026-10-29)

The first guesses (a picture about 1,000 to 1,200 units wide, 1.9 units to a glyph pixel, 175 units of margins) were fitted to two screenshots. A run of the pad at 1x, 2x and 3x with Xbox and PlayStation glyphs (RTX, Direct3D 12) replaced them with the movie's own `menu.width` (`--mcla_menu_log_layout=true`):

| Page | Old wording (level 0) | Level 1, "(R)" | Level 2, "(R)" |
|---|---|---|---|
| PERFORMANCE (19 rows) | 795 | 635 | 594 |
| DISPLAY (15 rows) | 751 | 598 | 555 |

- In the pictures the hint row, the scroll bar and the last row were whole at level 1 on every page, at 1x, 2x and 3x, with both glyph sets (level 2 was run at 3x, whole too); at the old wording PERFORMANCE (795) was cut and DISPLAY (751) was at the edge. The picture holds about 750 to 760 units, read from where the hint row was cut (it sits against the width's right edge), so `mcla_menu_width_cap` is 750.
- The log's estimate had been 1.9 units a glyph pixel and was off by about 60 percent; refitted to these six pages it is 245 plus 0.9 per glyph pixel (within about 20 units).
- The legend line under the title ("(R) NEXT START") did not show in any picture. It was removed, and restart-only rows say "(RESTART)" at level 1 ("(R)" at level 2, unexplained, as the fallback). "(RESTART)" is about 80 font pixels longer than "(R)", so about 85 units: estimated 720 for PERFORMANCE and 680 for DISPLAY; **measured** on 2026-10-29 by the chain `menus` on `integrate/menus` (1x and 3x, Xbox and PlayStation glyphs): PERFORMANCE 719, DISPLAY 653, no page went to level 2 by itself (the same widths with the MENU UPDATES row, `menu-rate-row-2026-10-29.md`).
- Pictures and sheets of the first run: not in the repository.

## How to see it

`launcher/dev/scripts/menu-pages.pad` (needs `{SHOT}`, a folder, and `{SCALE}`, the picture size, filled in) opens the pause menu, goes to Settings and takes pictures of the game's own list and OPTIONS page and of DISPLAY, PERFORMANCE and CONTROLS (top, middle, bottom), with Xbox and then PlayStation glyphs. Run it through `devrun.py run --res 1|2|3`, once with `--extra --mcla_menu_text_level=0` for the old wording.
