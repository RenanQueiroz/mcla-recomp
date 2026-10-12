# Car reflection fix (white car bodies with a glow halo)

Found and fixed 2026-10-11 on Direct3D 12; carried over to Vulkan (PC and PS5) and confirmed on a PS5 2026-10-12. The change to the SDK is `patches/rexglue-minmax-blend-factor.patch`.

## The issue

The player's car, and every car drawn with the full paint shader, came out far too bright. At night the whole body was flat white with a bloom halo around it, and the mirror image of the street that the paint should show was buried. By day the car was only somewhat too bright. The stock ReXGlue `xenos` plugin and Xenia Canary both show it ("the car reflection bug").

## The cause

It is not in the car shaders. It is in what they reflect.

1. Every frame the game renders the surroundings of the player's car into a dual-paraboloid environment map (512 x 256, both halves side by side, and a blurred 64 x 32 copy for the paint's soft specular). `PS_CarPaint` samples both (`dpFrontSampler`, `dpSpecFrontSampler`).
2. Into that map the game also draws the **light glows** (street lights, car lamps, traffic signals, the sun): `VS_LightGlow` + `PS_LightGlow`, one quad per light. The pixel shader writes the light's colour, constant over the whole quad, and puts the round falloff into **alpha** only: `alpha = (1 - |uv|)^2 * colour.a`.
3. Those quads are blended with `RB_BLENDCONTROL0 = 0x01660166`: colour = **MAX**(source x SrcAlpha, destination x One), and the same for alpha.
4. On the Xbox 360's GPU the source factor still applies in the MIN and MAX blend equations, so the result is `max(colour x alpha, destination)`: a soft round glow. Direct3D 12 and Vulkan ignore the blend factors for MIN and MAX. The plugin passes the state through as it is, so the host computed `max(colour, destination)`: **a solid square in the light's full colour**.
5. At night there are dozens of lights around the car, so the environment map held a band of big white and red squares along the horizon. The blurred copy averaged 0.4 to 0.66 along that band where it should be a few hundredths, and the paint's specular term mirrored it: a white body, and enough brightness to trip the bloom pass. By day only a few glows are on (sun, signals).

Evidence that the console applies the factor: the game's own shader. With factors ignored, the alpha it computes would have no effect at all and every glow would be a square on the console too.

How it was found, in short: a recorder in the GPU plugin for every draw (the cars are drawn from recorded D3D command buffers and never pass the D3D draw calls), per-shader constant overrides to switch the paint's terms off one at a time (lights, reflection, specular), and a readback of the environment map from guest memory (`--readback_resolve=full`).

## The fix

In the GPU plugin, for the host render target path (the default on both backends):

- when a render target's colour (or alpha) blend equation is MIN or MAX with the source factor SrcAlpha and the destination factor One, the command processor sets a system flag for that render target;
- the translated pixel shader then multiplies its colour (or its alpha) by its own alpha, saturated, before the value is written. The host's `max(source, destination)` then computes what the console does.

143 added lines, nothing removed:

| File (in the SDK) | Change |
|---|---|
| `include/rex/graphics/pipeline/shader/dxbc_translator.h` | D3D12: eight new system flags, `kSysFlag_MinMaxColor{0..3}TimesSrcAlpha`, `kSysFlag_MinMaxAlpha{0..3}TimesSrcAlpha` (29 of 32 used) |
| `src/graphics/pipeline/shader/dxbc_translator_om.cpp` | D3D12: `CompletePixelShader_WriteToRTVs`, the multiply by alpha, ahead of the exponent bias |
| `src/graphics/d3d12/command_processor.cpp` | D3D12: `UpdateSystemConstantValues` sets the flags from `RB_BLENDCONTROL0..3` |
| `include/rex/graphics/pipeline/shader/spirv_translator.h` | Vulkan: the same eight flags. The SPIR-V flag word has only two bits free, so they share the bits of the seven fragment-shader-interlock flags plus one free bit. The two sets are never in use together: the interlock flags are set and read on the interlock path only, these on the host render target path only |
| `src/graphics/pipeline/shader/spirv_translator_rb.cpp` | Vulkan: `CompleteFragmentShaderInMain`, the multiply by alpha, ahead of the exponent bias (without branches: the factor is 1 where a flag is not set) |
| `src/graphics/vulkan/command_processor.cpp` | Vulkan: `UpdateSystemConstantValues` sets the flags from `RB_BLENDCONTROL0..3` |

## How far it has been checked

| Renderer | State |
|---|---|
| Windows, Direct3D 12 | Confirmed in game: same spot at 23:30 and at noon, before and after. The car shows the street again instead of a white body with a halo. |
| Windows, Vulkan | Builds; the game starts and draws the title-screen city at dusk with round light glows and no shader or pipeline errors. Not compared before and after in a race. |
| PS5 (Vulkan) | The PS5 build uses the same translator and command processor files as Windows/Vulkan, with no PS5-specific code in the changed places, and `ps5/make_ps5.sh` applies the patch. Confirmed in game on a console, 2026-10-12, with a build made from scratch by that script: the reflection problem is gone. |

## What it does not cover

- The pixel-shader-interlock ("ROV" / "FSI") render target path: not changed. It does its blending in the shader and also treats MIN and MAX as factor-less.
- Other factors than SrcAlpha, or a destination factor other than One, in a MIN or MAX blend: not handled (the game does not use them in the frames looked at; a destination factor cannot be done by the host's blender at all).
- The dither pattern on shadows and foliage that LARecomp's notes list: untouched.

## Getting it

A new build has it: both build scripts apply the patch. For an existing build, update and run the same build command again; the scripts add the patch to an SDK that was patched before it existed and rebuild what changed.

```powershell
git pull
.\scripts\make_pc.ps1
```

```bash
git pull
bash ps5/make_ps5.sh --console <console address>
```

By hand, in the SDK checkout: `git apply <repository>/patches/rexglue-minmax-blend-factor.patch`, then build the SDK and the game again. On Windows the game folder needs the new `rexgpu-xenos.dll` next to `mcla.exe` (`make_pc.ps1` copies it). To take the fix out again: the same command with `git apply -R`, and build again.
