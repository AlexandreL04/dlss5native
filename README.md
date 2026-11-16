# dlss5native

Runs NVIDIA's **DLSS 5** (DLSS Neural Rendering) in any DirectX 12 game that uses DLSS, right after the
game's own DLSS upscale. Two files dropped next to the game's executable; nothing of the game is replaced.

> **Single-player only.** dlss5native is a DLL loaded into the game's process and it hooks NVIDIA's NGX
> functions. Anti-cheat systems treat that as tampering. **Do not use it in online or competitive games,
> or in any game protected by an anti-cheat: you risk a ban.**

> **No NVIDIA file is included.** This repository is source code only. You supply NVIDIA's DLSS 5 model
> file from software you own (see [Files you provide](#files-you-provide)).

## What it does

Every DLSS game ends up calling NVIDIA's NGX core (`_nvngx.dll`), whether it goes through Streamline or
calls NGX itself (as the NVIDIA plugins for Unreal Engine do). That call is the one place common to all
of them, and it is where dlss5native works:

1. **in** — the core's `CreateFeature` / `EvaluateFeature` / `ReleaseFeature` are hooked. A DLSS Super
   Resolution or Ray Reconstruction evaluate of the game gives the frame, the depth, the motion vectors
   and the jitter, straight from its parameters;
2. **out** — right after that evaluate, on the same command list, the DLSS 5 model (NGX feature 18) is
   evaluated with the parameters NVIDIA's own plugin gives it.

The same two files behave according to the game, with nothing to configure:

| the game… | what happens |
| --- | --- |
| has no DLSS 5 | DLSS 5 is added after its DLSS, with the settings panel |
| has DLSS 5 of its own | its DLSS 5 is left exactly as it is; on a GPU older than RTX 50, where NVIDIA's core refuses the model, the model file you supplied is handed to it instead |

### The colour stage

NVIDIA's model expects a display-referred, sRGB-encoded image in [0, 1]. A native title is authored to
hand it one. A game that was not prepared for DLSS 5 hands its DLSS output as it is: scene-linear HDR in
arbitrary units, PQ, or SDR code values. Feeding that straight to the model gives wrong colours and
washed-out blacks.

The colour stage converts the frame for the model and carries the model's *edit* back onto the
untouched frame, so highlights, blacks and HDR range stay the game's own:

| step | what happens |
| --- | --- |
| classify | SDR / relative linear HDR / absolute linear HDR / PQ, from the texture format and the game's DLSS flags |
| scale | a divisor measured on the frame (it only reduces an over-range frame), or the game's own exposure when it supplies one |
| encode | shadow toe, hue-preserving shoulder, sRGB — at the model's working resolution |
| model | NVIDIA's DLSS 5 model |
| stabilize | the edit is filtered over time: reprojected with the game's motion vectors, clipped, rejected on change |
| transport | below full resolution, the edit is brought up along the frame's edges (joint bilateral) |
| resolve | the edit applied as a bounded gain on the original frame |

An unchanged model output gives the frame back as the game made it — bit for bit on float HDR frames,
within rounding on PQ and SDR ones (`SkipModel=1` shows it in game, the off-line tests check it). When
the game's texture allows it, everything is read and written **in place**: the frame is never copied.

The algorithms of this stage follow the RenoDX DLSS Neural Rendering add-on (MIT) — see
[CREDITS.md](CREDITS.md). `ColorStage=0` switches the stage off and calls the model strictly in place.

## Requirements

- Windows 10/11, DirectX 12, a game with **DLSS** or DLSS Ray Reconstruction, enabled in its options.
- An NVIDIA RTX GPU and a driver that supports DLSS 5.
- NVIDIA's DLSS 5 model file (below).

## Files you provide

One file, from a game that ships DLSS 5 — never from this repository:

| file | purpose |
| --- | --- |
| `nvngx_dlssnr.dll` | NVIDIA's DLSS 5 model |

NVIDIA's signed model contains kernels for RTX 50 only. On RTX 20 / 30 / 40 the file must be a build that
carries kernels for your GPU. This repository does not provide one.

## Install

1. Build `hid.dll` (below).
2. Copy `hid.dll` and `nvngx_dlssnr.dll` next to the game's executable (for an Unreal Engine game:
   `…\<Game>\Binaries\Win64`).
3. Start the game with DLSS enabled in its options. `dlss5native.log` and `dlss5native.ini` appear next
   to the executable; the log says what was found and what was done.

To remove it, delete those two files (and the `.ini` / `.log`). No file of the game was changed.

`hid.dll` is a proxy for the Windows library of the same name; if the game does not load `hid.dll`,
load the DLL with the injector or proxy of your choice.

## Using it

| key | action |
| --- | --- |
| F6 | DLSS 5 on / off |
| F7 | reload `dlss5native.ini` |
| F8 | settings panel (arrows to move and change) |

The panel is drawn on the finished frame when it is presented, after the game's exposure and tone
mapping, so it looks the same in every scene. Every change is saved to `dlss5native.ini`; the file alone
is enough if you prefer not to use the panel. In a game that has DLSS 5 of its own there is no panel:
that game's own settings apply.

The panel's rows carry the names and ranges of the same settings in the RenoDX add-on. 1 is the default
of every slider.

| panel row | ini key | meaning |
| --- | --- | --- |
| `Intensity` | `Intensity` | strength of the effect, asked of the model (0–2) |
| `Style` | `Style` | the model's look: Default, Natural, Cinematic |
| `Local tone` | `LocalToneStrength` | how strongly the model reshapes brightness and contrast area by area (0–2) |
| `Structure` | `LocalStructureStrength` | how strongly the model reworks fine structure and surfaces (0–2) |
| `Character mask` | `UseAutoMask` | lets the model find people |
| `Character / skin` | `SkinStructureStrength` | how much the model reworks their skin (0–1); the model reads it only with the mask |
| `Edit strength` | `LookStrength` | scales what the model changed, after the fact: 0 gives the game's frame back (0–2) |
| `Brightening`, `Darkening` | `LookBrightening`, `LookDarkening` | the same, only where the model brightens / darkens |
| `Colour` | `LookColour` | the model's colour change toward or away from grey |
| `Resolution scale` | `ResolutionScale` | the model's working resolution, 33–100 % of the DLSS output: lower is faster |

Only in the ini:

| ini key | meaning |
| --- | --- |
| `Passes` | run the model again on its own result (each pass costs one more evaluate) |
| `FollowRenderResolution` | run the model at the game's render resolution |
| `DetailStability` | temporal filter on the edit: auto / off / on |
| `LookHueShift`, `LookShadows`, `LookMidtones`, `LookHighlights` | the model's hue change; its edit in the dark / middle / bright parts of the picture |
| `ColorStrength` | 0 = brightness only, 1 = the model's colour |

Everything else (frame classification, codecs, exposure) is automatic and documented in the ini. The log
lines `model receives` and `shaders receive` show the values that actually arrived after each change.

## Building

Requirements: Visual Studio 2022 (MSVC x64) and two source checkouts.

```
git clone --recursive <this repository>
rem or, in an existing clone:
git submodule update --init
```

`deps/Streamline` (only the NGX SDK headers it contains are used) and `deps/Detours` are git submodules.
Then, from an *x64 Native Tools Command Prompt*:

```
src\build.bat
```

The result is `src\hid.dll`.

### Tests

```
tests\build.bat
```

builds and runs the off-line tests: no game and no NVIDIA file are needed. The colour stage, the panel
and the settings run on real D3D12 devices - your GPU, then Microsoft's software device - with the debug
layer and GPU-based validation switched on, several frames in flight, working-resolution changes in
flight and a device change; the test program is built with AddressSanitizer. They check that an
unchanged model output gives the frame back, that nothing outside the DLSS rectangle is touched, that an
arbitrary model output stays inside the clamps, that the settings file survives bad input, and that the
two parameter blocks are right: what is read from a game's DLSS call (sub-rectangles, dynamic resolution,
missing inputs) and what the model is handed. `tools\build.bat` compiles every shader on its own.

### How the hooks are placed

| where | what |
| --- | --- |
| the NGX core's `CreateFeature` / `EvaluateFeature` / `ReleaseFeature` | sees the game's DLSS features; runs DLSS 5 behind each DLSS evaluate; steps aside when the game creates DLSS 5 itself |
| the model's import of `GetModuleFileNameW` | the model only answers a caller named like the NGX core: the one question it asks about this DLL is answered with the core's name |
| DXGI's `Present` / `Present1` | draws the settings panel on the back buffer while it is open; does nothing while it is closed |
| the DXGI factory's swap chain creation functions | notes the command queue each swap chain is created with, which the panel is drawn through |
| `LoadLibraryExW` and the version-information functions (`src/gate.cpp`) | only for a game with DLSS 5 of its own on a GPU older than RTX 50: the model file you supplied is handed to NGX and reported as fit for the GPU |

## How it was verified

Three games, each using DLSS in a different way, with the same two files and nothing configured
(RTX 4090, 2560x1440):

| game | how it uses DLSS | what happened |
| --- | --- | --- |
| Marvel's Spider-Man 2 | through Streamline (its own 2.7.2 files, untouched), DLSS Ray Reconstruction | DLSS 5 added: model created at 2560x1440, every evaluate succeeds; panel drawn at present |
| Bodycam (Unreal Engine 5) | calls NGX itself through NVIDIA's DLSS plugin, Streamline only for frame generation | DLSS 5 added: model created at 2560x1440, every evaluate succeeds; panel drawn at present, over the game's camera filter |
| NBA 2K27 | has DLSS 5 of its own | option on: the game's own DLSS 5 runs on this GPU (2000 evaluates counted, none failed) and nothing is added; option off: the game does not create it, and DLSS 5 is added as in the other two |

Cost on a fixed scene of Marvel's Spider-Man 2, frame generation x2, against the RenoDX add-on in the same session
conditions (rendered frames per second, DLSS 5 on / off alternated four times):

| | DLSS 5 on | DLSS 5 off | cost of DLSS 5 |
| --- | --- | --- | --- |
| RenoDX add-on | 57.9 fps | 73.5 fps | 3.66 ms |
| dlss5native | 59.4 fps | 75.3 fps | 3.55 ms |

Not verified yet:

- any GPU other than the RTX 4090.

## Limits

- DirectX 12 and DLSS only. No DirectX 11, no Vulkan, no game without DLSS (FSR or XeSS alone give no
  depth and motion vectors to read).
- The parameters of the model are not in NVIDIA's public headers; they were recorded from NVIDIA's own
  plugin and may change with a future model.
- Anything not listed under "How it was verified" is untested.

## Licence

MIT — see [LICENSE](LICENSE) and [CREDITS.md](CREDITS.md). Not affiliated with or endorsed by NVIDIA.
