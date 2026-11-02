# Keyboard and mouse prompts

`mnk_mode=true` and `mcla_keyboard_prompts=true` replace the shared controller
prompt sprites with the primary binding from the SDK's live `keybind_*` cvars.
For example, A becomes `[SPC]` with `keybind_a="Space"`; rebinding A to R
changes it to `[R]`. `Backspace,C` displays the first binding, `[BKSP]`.
`ENT` means Enter, `DN` means the Down arrow. Camera-stick prompts say `MOUSE`
when `mnk_mouse=true`. Controller mode or `mcla_keyboard_prompts=false` restores
the original artwork. This follows the configured input mode, not last-device
auto-detection.

The descriptors, such as ACCEPT and BACK, retain their original localized text.
The native Flash player draws the replacement under its own transform, colour,
animation and clipping. Font advances set the width; four-direction hints use
two lines. The renderer selects an existing font strike from the bound movie's
font dictionary, avoiding the null vector fallback in bitmap-only fonts.

## Definitions and rendering

The source selector is the icon sprite's zero-based current frame, not a match
against English prose. The controller definition is identified by a sprite
instance named `icon` and its controller timeline size; other instances sharing
that definition are recognized too. Menu tab buttons have separate semantic
instances, `btn_lb` and `btn_rb`.

| Frames (Xbox) | Control |
| --- | --- |
| 0–3 | A, B, X, Y |
| 4–7 | LB, RB, LT, RT |
| 8–9 | Start, Back |
| 12–13 | Left stick horizontal / four directions |
| 14–17 | Right stick general / vertical / horizontal / four directions |
| 18–21 | D-pad general / vertical / horizontal / four directions |
| 22–23 | Paired triggers / shoulders |
| 27–28 | Right / left stick press |
| 29–32 | Left stick right / left / up / down |
| 33–36 | Right stick right / left / up / down |

PlayStation's shared timeline uses the same selectors plus 49. Frames 10–11
were blank in the original pause-movie gallery; 24–26 are decorative arrows.
Unrecognized/decorative frames and movies without a suitable loaded font keep
the original artwork. No game textures, assets, or translated strings are
patched on disk. Coverage is of these shared definitions and the named tabs;
it is not a claim that every tutorial and race-specific UI asset was visited.

Layouts were checked against this build's generated PPC code: the recursive
draw at `0x825FB348`, timeline update at `0x825FB0D8`, seek at `0x825F90C0`,
font lookup at `0x825ED5F8`, and native text draw at `0x82604548`. Strong wrappers
use the generated weak aliases; no manual generated-source edits are required.
Keep this implementation tied to the supported Complete Edition XEX/build.

## Repeatable offscreen checks

Build with the existing remembered toolchain:

```powershell
$env:CMAKE_BUILD_PARALLEL_LEVEL='4'
.\scripts\build.ps1 -NoCodegen
$env:MCLA_DEV_ROOT='D:\agent-work\mcla\2026-11-02-prompt-qa'
python launcher/dev/keyboard_prompts.py --out D:/agent-work/mcla/2026-11-02-prompt-qa/gallery --run
```

The generator waits for the live `pause` signal (rather than a stale UI list),
checks that all requested selectors were resolved, and retains a `.pad` script,
frame/control manifest, screenshots and labeled contact sheet. It requires a prepared starter-career snapshot under `MCLA_DEV_ROOT/pristine`
(a save that starts in the city). Use `devrun.py prepare` with your own save
before the first run; saves/configuration are protected by `devrun.py`.
Contact-sheet generation requires Pillow (`python -m pip install Pillow`).
Use `--mouse --frames 14,15,16,17,33,34,35,36` for camera hints, or
`--playstation --frames 0,1,6,8` for the alternate selector bank. Output paths
must have no whitespace because of dev_pad command tokenization. Keep large
captures and build output on D:, as in the example.

`devrun.py --offscreen` supplies the command-line-only `mcla_dev_offscreen`
mode. It starts a hidden, nonactivating window outside the monitor bounds,
forces mute/windowed mode, and omits SDL physical input drivers using the SDK's
supported tool mode. The outer dev-pad factory still adds ScriptPad. Consequently
`mnk_mouse=true` can be checked as a rendering setting without creating the mouse
capture backend. Window show/restore commands are rejected in this mode.
Screenshots read the game render target. This is not a private window station,
filesystem sandbox, or network sandbox. Normal Play does not enable this mode.

`mcla_key_prompt_probe=true` logs resolved IDs/bindings and the selected font.
`mcla_key_prompt_gallery_frame=N` seeks the original shared timeline during
Update, when its movie pools are bound. `-1` disables it. Never seek from Draw:
the pool allocation path is not safe there. The original frame gallery should
be used as the independent expectation when adding mappings.

## Provenance

[Urban Chaos Riot Response Burner Edition](https://github.com/linkssy2/UrbanChaosRiotResponseBurnerEdition)
demonstrates identifying controller artwork and deriving replacements from
bindings, with context/layout treated together. This MCLA implementation uses
that design principle with independently traced Flash definitions and native
font rendering. No Urban Chaos implementation or artwork was copied.

Retained failed probes established two constraints: per-English-descriptor
substitution overlaps adjacent prompts, and requesting a nonexistent font
strike in a bitmap-only font follows its null vector-glyph pointer. The final
path uses control selectors, measured widths and already-loaded strikes.

## Local verification, 2026-11-02

Runtime checks on Windows with an RTX 3090 and D3D12 covered 32 keyboard
selectors, eight mouse-camera selectors, four alternate-bank selectors,
title/pause/Modes screens, live R/X/F1/F2 rebinding, controller fallback, and
full 2560x1440 captures. The game and scratch save required for these checks
are user-supplied and are not included in the repository. Sampled BACK/ACCEPT descriptor regions were pixel
identical with the feature on/off. Test readbacks confirmed hidden/unfocused
windows and mute. No physical SDL input driver was initialized. Native runs
exited successfully and devrun restored the real settings/save data. The invalid
frame generator probe and historical missed-pause batch were rejected.
