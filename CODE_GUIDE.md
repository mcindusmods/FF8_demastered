# Code guide

Where things are in this repository and how they fit together, for working on it later. It covers what this continuation added on top of Maki's DEMASTER (the `mcindus` branch); Maki's own modules are listed but not explained. What each feature does for a player or a modder is in the READMEs; what changed when is in `CHANGELOG.txt`.

Names of functions are given instead of line numbers, which go stale. Search for the name.

## The layout

| Folder or file | What it is |
|---|---|
| `ff8_demaster/` | The DLL the game loads (32-bit). Everything that runs in the game. |
| `fieldprep/` | `fieldprep.exe` (64-bit, command line): makes a field's PBR files. Nothing in it runs in the game. |
| `DEMASTER_EXP/shaders/` | The GLSL shaders DEMASTER uses in place of the game's on PBR fields. Copied into the game's `DEMASTER_EXP\shaders\`. |
| `tests/` | Tests that build and run on Linux, and the scripts that build and run them. |
| `tools/` | Python helpers (porting addresses to another game build, world map textures) and `syntax_check.sh`. |
| `demaster.conf` | Every setting with its explanation. The DLL reads the copy in the game folder at start. |
| `README_field_layers.md`, `README_field_pbr.md` | The field features, for the person making the images. |
| `COMPILING.md`, `HOW_TO_BUILD.txt`, `build_demaster.bat` | Building on Windows. |
| `ff8_demaster/vendor/` | Other people's code: headers and Debug libraries (`include`, `lib`), and bx/bimg as source for the Release build (`src`, see its `README.txt`). |

## The pattern every feature follows: rules, then the game side

Each feature is split in two, and knowing which half you are in tells you how it can be tested.

- **The rules**: a `.h`/`.cpp` pair with no Windows, OpenGL or game-memory code in it. File formats, names, arithmetic, decisions. These build on Linux, the tests run them, and `fieldprep` shares the ones about PBR files, so the DLL and fieldprep can't disagree.
- **The game side**: a `texturepatch_v2_*.cpp` file. Hooks, the game's memory, OpenGL calls. It only builds on Windows and only runs in the game, so keep it thin: read things, call the rules, send the result to the driver.

| Feature | Rules (tested) | Game side | Test |
|---|---|---|---|
| World map textures by name | `world_vehicles.h/.cpp` | `texturepatch_v2_worldVehicles.cpp` | `world_vehicles_test` (needs game files) |
| Field layers from PuPu/FME images | `field_layers.h/.cpp` | `texturepatch_v2_fieldLayers.cpp` | `field_layers_test` |
| PBR backgrounds, the lights file, the settings file, day/night arithmetic, the look | `field_pbr.h/.cpp` | `texturepatch_v2_fieldPbr.cpp` | `field_pbr_test`, `pbr_shader_test` |
| Moving lights (sun, party light) | `field_moving_lights.h/.cpp` | `texturepatch_v2_fieldMovingLights.cpp` | `field_pbr_test`, `pbr_shader_test` |
| Characters and models on PBR fields | `field_chars.h/.cpp` | `texturepatch_v2_fieldChars.cpp` | `field_chars_test`, `chara_shader_test`, `field_depth_test` |
| Field model textures (DDS, mips, maps) | (in fieldprep: `models.h/.cpp`) | `texturepatch_v2_fieldModelMaps.cpp` | `fieldprep_model_test` |
| The day/night clock | (in `field_pbr.cpp`: `NightAmount`, `AdvanceClock`, ...) | `day_night.h/.cpp` | `field_pbr_test` |
| OpenGL state without asking the driver | | `gl_state.h/.cpp` | |
| Frame log (Ctrl+F11) | | `frame_log.h/.cpp` | |

`field_pbr_runtime.h` is the one header the DLL's field modules share with each other (not with fieldprep): what the layer module hands the PBR module, what the PBR module tells the character module about the field (`FieldView`, `LightUniforms`), what the character module hands back (`CharFrame`), the quality levels, and the moving lights module's five functions.

Maki's modules (not changed in structure): `dllmain.cpp` (start-up, patches), `mymemory.h/.cpp` (addresses per game build), `opengl.cpp` (the OpenGL hooks everything hangs off), `texture.cpp`, `iopatch.cpp`, `texturepatch_v2_battle*.cpp`, `texturepatch_v2_fieldBackground.cpp`, `texturepatch_v2_fieldChara.cpp`, `texturepatch_v2_world.cpp`, `uvpatch.cpp`, `config.h/.cpp`, `debug.cpp` (the log).

## A field frame, in order

1. **The field loads** (game thread). `texturepatch_v2_fieldLayers.cpp`'s read hook sees the field's `.map` (`OnMapRead`): it finds the layer images, and `fieldrt::PbrFindFiles` finds the PBR files. The field's time of day goes to the clock (`DayNightFieldEntered`).
2. **The game uploads a background page** (renderer thread). `FieldLayersBuildPage` puts the layers' images into the page instead (`fieldrt::BuildPage`); for a page with PBR layers, `fieldrt::PbrPageBuilt` makes the matching lightmap, normal+depth, ORME and cell-table textures.
3. **The game draws the characters** first in every frame. `FieldCharsDraw` (called from the `glDrawElements` hook in `opengl.cpp`) keeps those draws. Right before the first thing that isn't a character it draws them itself (`Flush` in `texturepatch_v2_fieldChars.cpp`): works out who is who (`Group` in `field_chars.cpp`), tells the moving lights module who was drawn (`MovingLightsCharacters`), draws shadow maps, reflection pictures, the characters lit by the room, and hands the result to the PBR module (`PbrSetCharacters`).
4. **The game draws the background**, page by page. `FieldLayersBeforeDraw` → `fieldrt::PbrBeforeDraw`: binds the page's maps, swaps in DEMASTER's field program, sends the uniforms (`SetUniforms`). The draw itself is the game's. `FieldLayersAfterDraw` → `PbrAfterDraw` puts everything back.
5. **SwapBuffers** (`HookSwapBuffers` in `opengl.cpp`): `TweenEndOfDraws`, `FieldCharsSwap` (which calls `PbrEndFrame`, which calls `MovingLightsEndFrame`), `FrameLogSwap`, `DayNightSwap` (the clock moves on).

Everything from step 2 on is one thread, the renderer's. The game thread only does step 1; what crosses over is made once and read-only afterwards (`PbrField`) or atomic (the field's time).

## The lights: one set of arrays for everything

Every light a shader sees, whatever it is, is an entry in `fieldrt::LightUniforms` (`field_pbr_runtime.h`), filled by `Lights()` in `texturepatch_v2_fieldPbr.cpp`:

| Array (4 floats per light) | Shader uniform | What |
|---|---|---|
| `pos` | `demasterLightPos` | view x, y, z; range |
| `color` | `demasterLightColor` | light added now (colour × intensity × flicker × group × day/night weight); specular strength |
| `flicker` | `demasterLightFlicker` | painted light's change now; owns the flicker mask; shadow strength; is a night lamp |
| `paint` | `demasterLightPaint` | *(the look)* the lamp's own colour for the highlights dial; its weight in the relight dial (negative: a moving light) |

`Lights()` first asks the moving lights module how many places it needs (`MovingLightsWanted`), gives the rest to the field's own lights (`fieldpbr::ActiveLights`, which also does the day/night cross-fade), then appends the moving ones (`MovingLightsNow`). The character module reads the same struct (`PbrLights()`), so a light added here lights the characters and can cast their shadows (`castsShadow`) without touching the character code.

View space everywhere: x right, y down, z away from the camera, in units of world/4096. `fieldpbr::Space` converts between canvas pixels + depth map value and view positions.

## The look (FIELD_PBR_LOOK)

| What | Where |
|---|---|
| The four settings | `config.h/.cpp`: `FIELD_PBR_LOOK`, `FIELD_PBR_RELIGHT`, `FIELD_PBR_HIGHLIGHTS`, `FIELD_PBR_AO` |
| A field's own (`look`, `look_relight`, ...) | `fieldpbr::Settings` in `field_pbr.h`; read and written in `ParseSettings` / `WriteSettings` |
| Dials × master × field → what the shader gets | `fieldpbr::FieldLook` |
| The relight's scale (keeps the field's brightness) | `fieldpbr::RelightScale`, from samples made by `fieldpbr::ShadeSamples`; the samples are taken when the room's `_ND` loads (`LoadRoom` in `texturepatch_v2_fieldPbr.cpp`) |
| Sent to the shader | `SetUniforms`: `demasterLook` (relight, highlights, AO, scale), `demasterLightPaint` |
| The arithmetic | `demaster_field.frag`: the one loop over the lights in `demasterPbrShade` (search "the look") |
| Checked | `TestLook` in `field_pbr_test.cpp`; "the look" block in `pbr_shader_test.cpp`, which repeats the shader's sums on the CPU |

If you change the relight formula, change it in three places that must agree: the shader, `RelightScale`, and the test's `relightFactor`.

## Moving lights (FIELD_MOVING_LIGHTS)

| What | Where |
|---|---|
| Settings | `config.h/.cpp`: `FIELD_MOVING_LIGHTS`, `FIELD_SUN`, `FIELD_PARTY_LIGHT`, `FIELD_PARTY_LIGHT_STRENGTH` |
| Per field: `[sun]`, `[party]` in `_lights.txt` | `fieldpbr::Sun`, `fieldpbr::PartyLight` in `field_pbr.h`; `ParseLights` / `WriteLights` |
| Where the sun is, its colour, the sun as a light | `SkyTimeOf`, `SunDirection`, `SunColor`, `SunLight` in `field_moving_lights.cpp` |
| Who carries the party light, the party light as a light | `ChooseLeader`, `PartyLightFor` |
| The clock's position on the sun's arc | `DayNightSky` in `day_night.cpp` |
| The DLL's side: keeps the leader, hands the lights over | `texturepatch_v2_fieldMovingLights.cpp` |
| The characters drawn this frame, and the entity byte the leader choice uses | `Flush` and `ReadEntities` in `texturepatch_v2_fieldChars.cpp` (the byte is at 0x255 in the 612-byte entity; FFNx's `ff8_field_state_other` calls it `setpc`) |
| Checked | `TestMovingLights` in `field_pbr_test.cpp`; "the moving lights" block in `pbr_shader_test.cpp` |

**To add another moving light** (a spell, a vehicle's headlight): write a function in `field_moving_lights.cpp` that returns a `fieldpbr::MovingLight` from whatever positions it, test it there, raise `MAX_MOVING_LIGHTS` in `field_pbr_runtime.h` if more than two can be on at once, and add it to `MovingLightsWanted` and `MovingLightsNow`. No shader change is needed: the sun itself is only a point light four field-widths away with a very long range.

## Smooth scroll and in-between pictures (FIELD_SMOOTH_SCROLL, FIELD_INTERPOLATION)

| File | What is in it |
|---|---|
| `field_tween.h/.cpp` | The rules, no game: `Axis` (the filter that turns a layer's whole-pixel places into a line), `LayerTrack` / `Advance` / `ShiftNow` / `ShiftBetween`, `PlaceOfDraw` (which layer a draw's tiles are, and where the game put it), `KeepCovering`, `TweenCharacters`, `Pacer`. Tested by `tests/field_tween_test.cpp`. |
| `field_mirror.h/.cpp` | The second picture's framebuffers. Knows nothing about fields: it is told what the game has bound (`Host`) and is called after each draw, clear, blit and copy of the game's, and does the same to its copies. Tested by `tests/mirror_test.cpp` on a frame built like the game's. |
| `texturepatch_v2_fieldTween.cpp` | The game side: the hooks on `glClear`, `glBlitFramebuffer`, `glCopyTex(Sub)Image2D`; what a draw is (`Classify`); the draw itself, moved and mirrored (`DrawNow`); draws kept back for the characters (`Hold`, `ReplayHeld`); the end of the frame (`TweenShowBetween`, `TweenShowFrame`, `TweenFrameEnd`). |
| `texturepatch_v2_fieldChars.cpp` | `MoveOfFrame`: the kept characters' shift and their in-between places; `drawModels(true)` / `drawAll(true)` draw them a second time into the copy. |

How a draw goes through `HookGlDrawElements` now: `TweenBeforeDraw` (what is it; hold it?) → `FieldCharsDraw` (keep it, or draw what was kept) → `TweenDraw` (`FieldLayersBeforeDraw`, the draw with the layer's shift in the projection matrix, the same draw into the copies with the in-between shift, `FieldLayersAfterDraw`). With both settings at 0 the hook takes its old three steps.

Things that are easy to get wrong:

- **Layers are known only on pages built from layer images.** `TweenPageBuilt` (called from `PbrPageBuilt`, for every page, PBR or not) keeps each cell's layer and canvas place. A draw whose tiles are of two layers, or don't agree on where the canvas is, is left alone.
- **The main layer's shift is fixed once per frame** (`FixMain`), the first time anything needs it: the characters, the round shadows and the main layer's own draws must all use the same one. If the characters have to be drawn before the main layer was seen and the draw in between can't be kept back, it is fixed at zero for that frame and the characters aren't tweened either.
- **A new framebuffer copy starts as what the game's holds, and that has to be read before the game draws into it** this frame (`fieldmirror::BeforeDraw`, `TweenFramebufferCopy` before the character pass). Otherwise the copy has this frame's draw in it twice, at two places.
- **The character module's depth copy gets its in-between twin by itself**: `CopyCharacterDepth` calls `glCopyTexSubImage2D`, the hook repeats it from the copy of the scene into a copy of that texture, and the mirrored background draws read the twin (unit 8 is inside `fieldmirror::UNITS`). So the mirrored character pass has to stay before it.
- **DEMASTER's own binds inside the mirror go past its texture hook** (`HostBindTexture`) and are put back before anything looks; framebuffer binds go through the tracker and are put back the same way.
- **What only a log from the game can tell:** the rate the game shows fields at (the log says it once per field), whether the wait inside SwapBuffers is taken out of the game's own frame limiter's idle time (if not, the "slowed down" line appears), and whether any framebuffer of the game is written by a call that isn't hooked (the in-between picture would flicker).

## Walking at any angle (ANALOG_MOVEMENT)

`analog_move.h/.cpp` is the arithmetic (stick to the game's 0-255 per axis), `texturepatch_v2_analogMove.cpp` the three call sites it replaces in the field's movement function and the XInput reading (loaded at run time, no library). The addresses are `ANALOG_GET_VALUE`, `ANALOG_FIELD_TEST`, `ANALOG_FIELD_X`, `ANALOG_FIELD_Y` in `mymemory.h`, for both game DLLs. The game's own function is always called first: it also puts the game's emulated stack back.

- **The swing (`MOVE_TURN_SPEED`).** `analogmove::FromDpad` turns bits 12-15 of the game's buttons word into a stick pushed all the way; `analogmove::Swing` turns the direction given towards the one held. The buttons word is the movement function's own argument, read from the guest stack in `FieldStickTest` (0x64 above the emulated ESP at that call, the same in both DLLs). Answering the stick test with anything but -1 sends the game down its analogue path, which reads the stick and the same walk/run button.
- **The world map (`ANALOG_WORLD_MAP`).** `WorldStick` answers the eight calls in the world map's input function (found by scanning 0x240 bytes from `ANALOG_WORLD_FIRST` for calls to the getter: there must be exactly eight). It takes the d-pad bits out of `WM_KEYS[WM_KEY_INDEX]` while the left stick is pushed.
- **Who the player moves.** `FIELD_PLAYER_ENTITY` (a guest address, s16): the index of the script entity the movement code gives the pad to. The party light reads it (`texturepatch_v2_fieldChars.cpp`, passed to `fieldpbr::ChooseLeader` as `controlled`).

## Field pages: on the list, parked, taken back (FIELD_LAYERS)

`fieldPages` in `texturepatch_v2_fieldLayers.cpp` maps a game texture to the page DEMASTER built into it; a draw with such a texture bound gets the tile filter and, with PBR maps, DEMASTER's field program. A page leaves the list when the game uploads to its texture (`FieldLayersForgetTexture`, from the glTexImage2D hook), when its texture has another size (`DriverWidth`, asked only when the driver has that texture bound), or when a .map is read (`ParkAll`: the pages go to `parkedPages`). A parked page whose texture the game draws without a new upload, in the same field, is put back by `TakeBack`. `LogDrop` writes each case to the log with `FIELD_DEBUG`.

## The glows (FIELD_PBR)

Two kinds, per cell (flags in the cell table): the old mask in `_ORME`'s alpha (flag 8: the painting's own colour glows) and a picture of the glows, `_EC` (flag 16: `fieldpbr::EMISSIVE`, texture unit 9, sampler `demasterEC`). `fieldpbr::PackEmissive` makes `_EC` from your `_E.png` (fieldprep's `GlowPicture`); `EmissiveIsPicture` tells a picture from the old grey mask. The shader's formula is under "Glows" in `README_field_pbr.md`. A layer's `_ND`, `_ORME` and `_EC` are looked for in this order: its own, its animation state's first picture's (`pupu & ~0xF`), the field's (`PbrFindFiles`).

## How to...

**Add a setting to demaster.conf.** Declare it in `config.h` (`inline int` or `inline double`), read it in `ReadConfigFile` in `config.cpp` with its default, document it in `demaster.conf`. It is read once, at start.

**Add a per-field setting.** `_pbr.txt`: a member of `fieldpbr::Settings`, a branch in `ParseSettings`, a line in `WriteSettings` under "yours", and in `fieldprep/main.cpp` copy it from the old file where the others are copied (search `settings.time = previous.time`), or fieldprep will reset it on its next run. `_lights.txt` works the same through `ParseLights` / `WriteLights` and the block in `fieldprep/main.cpp` that keeps `[emissive]` and `[night]`. Both files are read again by the DLL within a second of being saved (`PbrPoll`), which is what makes tuning in the game possible. Add a round-trip check to `field_pbr_test.cpp`.

**Add a uniform to the field shader.** Declare it in `demaster_field.frag`; add a location to `FieldProgram` and look it up in `MakeFieldProgram`; send it in `SetUniforms`. `SetUniforms` has two halves, and this matters for speed: the first three calls are sent before *every* background draw (they differ per page); everything after the `p.sent == uniformSerial` check is sent once per frame. Put a new uniform in the second half unless it truly differs between pages, and if something other than a new frame can change it, raise `uniformSerial` where that happens (it is already raised at the end of every frame, when the characters' data arrives, when a file is read again and when the field changes). Set the same uniform in `pbr_shader_test.cpp`'s `draw`.

**Add a debug view.** `FIELD_PBR_VIEW` numbers are handled at the end of `demasterPbrShade` (backgrounds) and `main` in `demaster_chara.frag` (characters); document the number in `demaster.conf` and `README_field_pbr.md`.

**Add a game address.** `mymemory.h` (`ADDRESS_NAME`) and the tables in `mymemory.cpp`; `tools/port_addresses.py` finds the same place in another build of the game. Guest (emulated) memory is read with `fieldrt::GuestRead`, never dereferenced directly.

## Speed: what costs what

The target is a GTX 1060-class card at `FIELD_PBR_QUALITY=medium`, and integrated graphics at `low`. The levels are one table: `CurrentQuality()` at the bottom of `texturepatch_v2_fieldPbr.cpp`.

- **Texture reads per pixel are what cost.** A PBR pixel reads albedo and lightmap (4 each with `FIELD_SMOOTH`), normal+depth, ORME and two cell-table texels. A wall check towards a light (`demasterRoomClear`) reads the room's depth once per step: 6 steps at medium, 12 at high, none at low, for each light that reaches the pixel. Shadow maps: 1 read (low) or 4 per map.
- **Arithmetic per light is cheap** by comparison. The look adds only arithmetic. A moving light costs what any light costs; the sun reaches every pixel, so on a sunlit field at medium it is one more wall check per pixel.
- **The shader goes over the lights once.** Painted light (flicker, shadows, night), the look and added light share one loop in `demasterPbrShade`. Keep it that way: a second loop repeats the distance arithmetic for every light.
- **On the CPU**, calls into the driver are what cost, and asking the driver anything costs most (see `gl_state.h`). Per background draw DEMASTER sends 3 uniforms and binds the page's maps; the rest goes once per frame.
- **Debug or Release.** The Debug DLL is unoptimised apart from the files marked `MaxSpeed` in `ff8_demaster.vcxproj`, and its C++ runtime checks every container access. The Release configuration is the same code optimised; the files with assembly hooks stay unoptimised in it on purpose (the project file says why).

`BENCH=10 FIELD=<a prepared field> tests/run_tests.sh` times the field shader per setup. It is software rendering, so only compare the rows with each other.

## Building and checking

| | Command | Notes |
|---|---|---|
| The DLL and fieldprep, Windows | `build_demaster.bat` | Debug into `.build\Debug`, then Release into `.build\Release`. `COMPILING.md`. |
| fieldprep and every test, Linux | `tests/build_tests.sh` | Rebuilds everything every time, on purpose: an object left from before a header changed links fine and reads the wrong fields. |
| Run the tests | `tests/run_tests.sh` | The shader tests run when `FIELD=` and `GAME_SHADERS=` say where a prepared field and the game's shaders are. |
| Type-check the Windows-only files, Linux | `tools/syntax_check.sh` | MinGW reads them without compiling. Not Microsoft's compiler; skips the files with inline assembly. |

What none of this can check: the DLL running in the game. Anything in a `texturepatch_v2_*.cpp` file is proven only by a log from the game, so those files log what they decide (`fieldrt::Log`, `LogOnce`; `FIELD_DEBUG=1` for more), and `FIELD_FRAME_LOG` (Ctrl+F11) writes down a whole frame.

## Conventions

- Comments say what the code is for and why, in plain words, next to the code. A file's first comment says what the file is and where its other half is.
- A `.h` that the tests build must not include Windows or OpenGL headers.
- Per-frame code in the DLL goes in files marked `MaxSpeed` in the project, or stays trivial.
- Every version gets a `CHANGELOG.txt` entry; a new setting gets its lines in `demaster.conf`.
- The game's own draws are never reordered and nothing the game would hide is shown; a feature that is switched off leaves the frame exactly as the game draws it. The tests that compare against the painting or against `main_char.frag` exist to keep that true.
