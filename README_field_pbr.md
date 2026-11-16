# DEMASTER PBR fields (v15.3): backgrounds, characters and models

Field backgrounds lit in the shader from your Marigold/CHORD maps, and (new in v15) the field's characters and 3D models lit by the same room, with real shadows, reflections in the background and per-pixel depth. Two separate parts:

- **The PBR modules in DEMASTER** (`FIELD_PBR` and the `FIELD_PBR_*` switches) draw a layer as **albedo × lightmap**, let lights flicker or follow the game's light groups, and add light on top using the normals and roughness/metallic. With every light at rest you get your painting back, pixel for pixel. Background pages are lit inside the draws the game already makes. Characters are collected and drawn by DEMASTER itself, in the game's order, right before the background.
- **fieldprep.exe** is the preparation tool and never runs in the game. It reads your maps, works out the lightmap, finds the lights, bakes the AO, measures where things are in depth, bakes the room's light into a 3D grid for the characters, and packs everything into the files DEMASTER loads. It shares DEMASTER's field code (`field_layers`, `field_pbr`), so both sides agree on names, cells and depth.

Only opaque layers (blend 4) are PBR. The glow and shadow layers (blend 0-3, like bvtr_1's `001FF000`) are drawn exactly as before, on top. Fields without PBR files are drawn exactly as before, characters included.

## Setup

1. Build with `build_demaster.bat`. It makes `ff8_demaster.dll` and `fieldprep.exe` (64-bit).
2. Copy **every file** in `DEMASTER_EXP\shaders\` into the game's `DEMASTER_EXP\shaders\`: `main.frag`, `demaster_field.vert/.frag`, `demaster_chara.vert/.frag`, `demaster_shadow.frag`. Without them, PBR layers fall back to their paintings and characters are drawn by the game as before; the log says which file is missing.
3. `demaster.conf` needs `FIELD_LAYERS=1` and `FIELD_PBR=1`. The `FIELD_PBR_*` switches are all on by default except `FIELD_AVATARS` (see "The switches" below).
4. `fieldprep.exe` can go anywhere. It finds the game's data from the field folder you give it.
5. **Run fieldprep again on fields you made with v13/v14.** It adds what v15 needs (the depth measurements, the camera, the light grid). Without the re-run the field still works: per-tile depth, and characters get only the added lights plus the game's own darkening.

## Your maps (in the field's folder, all PNG)

| File | What | Notes |
|---|---|---|
| `<field>.png` | the whole field, opaque layers only | what you ran Marigold/CHORD on. If it's missing, fieldprep puts it together from the opaque layers (first animation state) and saves it, so `fieldprep compose` is a quick way to get the image to feed ComfyUI |
| `<field>_AM.png` | albedo (sRGB) | **required**; AM colour × AM2 detail as you make it. A field-level `_A.png` is used if there's no `_AM` |
| `<field>_N.png` | normals | green up or down: fieldprep works it out |
| `<field>_DEPTH.png` | depth, 16-bit is best | bright-is-near or far: fieldprep works it out |
| `<field>_BL.png` | bounced light (was `_AOCOLOR`) | linear or sRGB: fieldprep works it out |
| `<field>_RESM.png` | non-diffuse residual | finds the night's lamps, and makes the emissive map (where it's most of the painting: a bulb, a lit screen) |
| `<field>_R.png`, `_M.png` | roughness, metallic | `_RC`/`_RM` and `_MC`/`_MM` are read when `_R`/`_M` are missing |
| `<field>_E.png` | emissive (white: glows) | yours, if you'd rather paint it; else fieldprep makes it from `_RESM`. `_H` (height) isn't used any more: its place holds the emissive map |
| `<field>_O.png` | AO | fieldprep bakes it (see below); yours if you've edited it |
| `<field>_<pupu>_AM.png` (and `_N`, `_DEPTH`, `_O`, `_R`, `_M`, `_E`) | one layer's own maps | for layers that don't look like the whole field (animation states you ran through Marigold separately) |

**What size your maps are (v15.8).** The whole-field maps (`<field>.png`, `_AM`, `_N`, `_DEPTH`, `_BL`, `_RESM`, `_R`, `_M`, `_E`, `_O`, `_LIGHTS`) go on the field's *canvas*:
- **All layer images from FME:** FME's render area (bdifrit1: 400×248, 1600×992 at 4x), as before.
- **Images from PuPu** (see `README_field_layers.md`): the box around the opaque layers' pictures. For bdifrit1 that's the main layer's 336×240 (1344×960 at 4x), so maps you made from PuPu's images stay as they are.
- **A map made for the other one** (FME's 1600×992 when the canvas is 1344×960, or the other way round) is cropped or padded into place by its exact offset, not stretched. The report says "made for FME's canvas, moved into place".
- **A map at another scale** is resized.
- **A map whose shape fits nothing** is resized with a warning to check that it lines up.
- **One layer's own maps** (`<field>_<pupu>_AM` and so on) go in that layer's picture's frame, the same size as its layer image. A map made for the canvas or FME's canvas is moved into place.

The report's first lines say which it is: `14 layer images at 4x: 14 named and framed the PuPu way, 0 FME's` and `canvas (...): 336x240 (1344x960 at 4x), the box around the opaque layers' pictures`.

The field's `.map`, camera (`.ca`) and walkmesh (`.id`) come from `<field>.map/.ca/.id` in the folder if they're there, else from `DEMASTER_EXP\data\field.fs`, else straight from the game's `main.zzz`, so nothing needs unpacking.

## Running fieldprep

```
fieldprep "<game>\DEMASTER_EXP\textures\field_bg\bv\bvtr_1"        (stage 1)
fieldprep ormh "<game>\DEMASTER_EXP\textures\field_bg\bv\bvtr_1"   (stage 2: pack ORM-H once you're happy with _O)
fieldprep all "<game>\DEMASTER_EXP\textures\field_bg"              (every field, both stages)
```

| Command | Does |
|---|---|
| `prep` (default) | `_A` for every opaque layer, `_LF`, `_ND`, bakes `_O`, writes `_lights.txt` (once) and `_pbr.txt` |
| `orme` | packs `_O`, `_R`, `_M` and the emissive map into `_ORME` (`ormh`, its old name, still works) |
| `all` | `prep`, then `orme` |
| `compose` | only writes `<field>.png` (`--force` replaces yours) |

Options: `--no-dds`, `--relight` (find lights again; the old file is kept as `<field>_lights_old.txt`), `--rebake` (bake `_O` again), `--tint 0-1` (default 0.5), `--ao-radius <native px>` (10), `--ao-strength` (1), `--light-intensity` (what each found light adds on top: 0.2; 0 keeps the painting as it is), `--light-share <%>` (how much of the painting's light a light must explain to count: 2), `--max-lights` (the large lights fieldprep keeps per time of day: 2, at most 2), `--emissive-share <%>` (orme: how much of the painting `_RESM` has to be for a pixel to start glowing: 35; full glow 40 points above), `--data <folder or main.zzz>`, `--threads`.

It prints what it did and saves the same as `<field>_fieldprep.txt`. Send me that file if something looks off.

## What it writes (PNG, then BC7 DDS)

| File | RGB | A |
|---|---|---|
| `<field>_<pupu>_A` | albedo of that layer (sRGB), in its picture's frame (its layer image's size) | the layer's coverage |
| `<field>_LF` | lightmap: painting ÷ albedo, sRGB curve, up to the scale in `_pbr.txt` | steady mask: white = steady, darker = the part that follows the nearest flickering light |
| `<field>_<pupu>_LF` | the same for a layer that differs from the whole field | (same) |
| `<field>_ND` | normal, green up | depth |
| `<field>_ORME` | AO, roughness, metallic | emissive (an older `_ORMH` still works: its alpha, the height, is left out) |

Plus:
- `<field>_PROBES.dds`: the room's light in 3D, for the characters (a small 3D texture, about 0.5 MB). For points in front of the camera it stores how much light arrives from each direction (your painting's light, from its depth), and how much of it flickers.
- `<field>_O.png` (baked AO, made only once so your edits survive), `<field>_pbr.txt` (canvas, camera, depth model, lightmap scales, depth measurements; rewritten every run, your three keys kept), `<field>_lights.txt` (written once, then yours) and `<field>_fieldprep_check.png`.

- **Your layer images as DDS too (v15.9).** `prep` (and `all`) also writes every layer image as `<field>_<pupu>.dds` (BC7): the opaque layers, every animation state, and the glow and fog layers. A DDS newer than its PNG is left alone. Then the game's folder can hold DDS files only. DEMASTER still needs **every layer image** there, as a PNG or a DDS: it finds each layer, and where its tiles go, by its layer image, and only then looks for that layer's `_A`. A layer without its image is drawn with the game's own pixels and gets no PBR (the log says "layer image(s) not found"). fieldprep reads the PNG when both are there (the DDS is BC7, a little off), so keep the PNGs somewhere if you'll run fieldprep again; with only the DDS it works from that.
- **BC7 DDS without texconv.** fieldprep writes BC7_UNORM DDS itself (bc7enc), always after the PNG. DEMASTER takes the newer of the two, as with layers: the DDS normally, the PNG while you edit it.
- **The pair multiplies back to the painting.** The albedo is rounded to 8 bits first and the lightmap worked out from that. For the DDS pair, the lightmap is worked out from the albedo *after* BC7, so the albedo's compression error is cancelled. Measured on bvtr_1: PNG pair within 0.7 levels of the paintings, DDS pair within the same error as compressing the painting itself to BC7.
- **Which layers get their own `_LF`.** A layer whose visible pixels are all what the whole field shows uses the field's `_LF`. Animation states and parts hidden behind other layers get their own, using the field's light at that spot (light belongs to the place, not the layer). Their albedo becomes painting ÷ that light. In bvtr_1 that's all of them, because of the 16 states of the scenery outside.
- **The tint.** With `_BL` there, `--tint` moves the bounced light's colour (relative to the field's average) out of the albedo and into the lightmap. This is the "red light on a white wall" correction. Albedo × lightmap stays exactly the painting.

## The lights file

```
[light]
name = light 1
position = 100.5 59.5 0.0094    ; canvas x y (native px), depth map value there
color = 1.000 0.752 0.474       ; linear
baked = 1                       ; 1: in the painting already; 0: a new light
intensity = 0.200               ; light added on top (0 keeps the painting exactly)
range = 75.0                    ; native px at the light's depth
flicker = fire                  ; none | fire | candle | pulse | strobe | fluorescent
amount = 0.600
speed = 1.000
specular = 1.000
shadow = 0.5                    ; how much a character's shadow takes away from this light (0-1)
time = day                      ; day | night | both: when it is on (day and night lights cross-fade with DAY_NIGHT's clock)
source = found                  ; found (fieldprep's), mark (your _LIGHTS.png), yours (you wrote it here: kept on --relight)
follows = 01                    ; new: follow the game's light group 01 (the animation id in the pupu: 004 01 000)
on_states = 1, 2                ; new, with follows: on only while the group shows one of these frames

[emissive]                      ; the field's glows (the emissive map), for the whole field
hue = 0.0                       ; degrees their painted colour turns around the colour wheel (-180 to 180)
tint = 1.000 1.000 1.000        ; times this (linear): gives white glows a colour, which hue can't
strength = 1.000                ; how bright (1: as painted)

[night]                         ; the field at night (v15.8), for the whole field
level = 0.180                   ; how much of the painted light is left away from the night's lamps (0-1)
tint = 0.600 0.700 1.000        ; what's left times this (linear): the moonlight's colour
lamps = 0.900                   ; how much of the painted light stays around the lamps on at night, within their range (0-1)
```

- **How fieldprep finds the lights: inverse lighting** (as in Marschner & Greenberg, "Inverse Lighting for Photography", 1997). It doesn't look for bright spots. It asks which few point lights, shining on the room as your normal and depth maps shape it (n·l / r², out of view too), best explain the diffuse light in the painting (`_BL` if you have it, else the lightmap), on top of a smooth ambient (a constant, the surfaces' facing and a slope across the screen). It adds the light that explains the most of what's left, fits all of them again (none can be negative), moves each around to where it fits best, and stops when a new light would explain less than 2% (`--light-share`). Then it keeps only the large ones (see below) and fits them again. A light that ends up explaining almost nothing is dropped. Colour comes from the same fit per channel, range from where its light falls to a tenth of the ambient. The report and each light's comment say how much of the light it explains.
- **Two large lights per time of day, small ones yours.** fieldprep keeps at most 2 lights by day and 2 by night (`--max-lights`): the ones that light the most of the field (where each adds a tenth or more to the light already there), which are the room's own light, ceiling lamps and windows. A sconce or a lit sign lights only its spot, however bright: fieldprep leaves it out and fits the large ones again without those spots, so they don't lean towards them. Those spots are for the emissive map and for your own lights. The ones fieldprep keeps start on: they add `--light-intensity` (0.2) on top of the painting and cast character shadows (`shadow` left out: 0.5, as for every painted light). Each light's comment says how much of the light it explains and how much of the field it lights.
- **Your own lights: up to 6 per time of day**, for the small lamps that matter (a desk lamp a character walks past, a candle). Write them into `_lights.txt` yourself (`source = yours`, or leave `source` out) or mark them in `_LIGHTS.png` (below); marks count towards the 6. Lights you wrote yourself are kept when fieldprep finds the lights again (`--relight`, or a newer `_LIGHTS.png`); fieldprep's and your marks' are found again. A file from before v15.7 has no `source` lines, so all of it is taken as fieldprep's: copy anything you added from `<field>_lights_old.txt`. DEMASTER uses at most 2 of fieldprep's and 6 of yours at a time (8 in all): more are left out, and both fieldprep and DEMASTER's log say so.
- **Day and night lights.** fieldprep finds the day's lamps in the painting's light (as above) and the night's in `_RESM` (what the painting's glows give off, close to the field at night), the same way. A lamp found in both, at the same place, is one lamp on day and night. Each light says when it is on: `time = day`, `night` or `both` (left out, as in older files: both). With the day/night clock (below) the day's lights fade out and the night's fade in. No `_RESM`: no night lights. A file holds up to 32 lights, day and night together; at most 8 are on at a time.
- **Your marks: `<field>_LIGHTS.png`**, painted on **black**, at the field's size (any other size works too). Each separate spot becomes one lamp at its middle, kept even if the fit gives it nothing, always on: **pure green** (0, 255, 0) a day lamp, **pure blue** (0, 0, 255) a night lamp, **cyan** (0, 255, 255) a lamp on day and night. **Pure red** (255, 0, 0) over anything that isn't a lamp's light (a floor reflection, a glowing sign, a spot fieldprep got wrong): no lamp, day or night, goes there, and that part of the painting isn't explained by lamps at all. Any other colour is ignored. Use a hard brush (no soft edges or anti-aliasing) and save as PNG. When `_LIGHTS.png` is newer than `_lights.txt`, fieldprep finds the lights again on its own (the old file is kept as `<field>_lights_old.txt`).
- **`[emissive]`: the glows' colour**, for the whole field: `hue` turns the painted colour of every glow around the colour wheel (in degrees, as an image editor's hue slider does), `tint` multiplies them (what colours a white glow, which has no hue to turn), `strength` makes them brighter or dimmer. fieldprep writes the section with `hue = 0`, `tint = 1 1 1`, `strength = 1` and keeps yours when it finds the lights again. It shows as the room goes dark at night (below); `FIELD_PBR_VIEW=16` shows it any time.
- None flicker at first. Set `flicker` on the ones that should, then **run fieldprep again**: the mask in `_LF`'s alpha is worked out from the baked lights that flicker or follow a group (the painted light above the field's ambient level, faded out by range and by surfaces facing away). After that, the flicker's type, amount and speed change live.
- **`shadow`**: a character standing in a lamp's light takes away this much of the light the lamp adds, and of the lamp's painted light where its shadow falls (there's no way to tell how much of the painting came from that lamp, so you say). Default: 1 for a new light, 0.5 for a painted one. 0: casts no character shadows.
- **`follows`**: the game keeps a colour and a frame number for every background light group, and field scripts drive them (BGANIME switches the frames: a lamp switching on, an animated fire; BGSHADE changes the colour: an alarm pulsing, a fade). A light that follows a group is coloured by it (128 = as painted) and, with `on_states`, is on only while the group shows those frames. A painted light that follows a group goes dark with it too (its part of the painting is under the `_LF` mask: run fieldprep again after adding `follows`). Only in the English/European game; the Japanese game's table isn't known yet, so there lights ignore `follows`.
- **The canvas line in `_pbr.txt`** (`canvas = -168 -120 336 240` for bdifrit1 from PuPu): the part of the field the whole-field maps cover, in `.map` coordinates (left, top, width, height, native pixels). DEMASTER puts `_LF`, `_ND`, `_ORME`, the lights and the camera where that says. A `_pbr.txt` without it (from v15.7 or older) means FME's render area. Leave it to fieldprep: it's rewritten every run.
- DEMASTER reads `_lights.txt`, `_pbr.txt`, `_A`, `_LF`, `_ND`, `_ORME` and `_PROBES.dds` again within a second of you saving them (`FIELD_LAYERS_LIVE=1`).

## Day and night (v15.8)

One clock for the whole game (`DAY_NIGHT=1`): 8 minutes of day and 4 of night, with 40-second fades, in fields, on the world map and in menus. Battles run the same clock 3 times slower (24 + 12 minutes, 120-second fades), so when a battle ends the field carries on from wherever the battle left it. It runs while the game draws frames and is kept in `DEMASTER_EXP\daynight.txt` between sessions (written every 30 seconds and after every key). The lengths are in `demaster.conf` (`DAY_NIGHT_DAY`, `_NIGHT`, `_FADE`, `_BATTLE`).

**What changes at night**, on PBR fields:
- **The lights.** Lights with `time = day` fade out, `time = night` fade in, `both` stay. Each time of day has its own 2 of fieldprep's and 6 of yours. During a fade both sets are on; if that's more than 8, the faintest wait until the fade ends (the log says so once).
- **The painting dims.** Away from the lamps that are on at night, the painted light goes down to `[night] level` times `tint` (moonlight). Around those lamps (`time = night` or `both`, `baked = 1`), the painting keeps `[night] lamps` of its own light, most at the lamp and fading out towards the edge of its `range`. A lamp's `range` sets how big its pool of light stays at night. Change `[night]` in `_lights.txt` while you stand in the field at night: it's read again within a second.
- **The glows come on.** The emissive map's glows (`[emissive]`) are added as the room goes dark.
- **Characters lit by the room** dim the same way (their light grid times the same level, tint and lamps). Characters the game lights itself (`FIELD_PBR_CHARACTERS=0`, or fields without PBR) don't change.
- **Not changed:** fields without PBR files, layers without PBR maps (layers with no `_A`, and the game's glow and shadow layers, blend 0-3), and battles and the world map (no PBR there yet: they only move the clock).

**A field's own time:** `time = day` or `time = night` in its `_pbr.txt` fixes it, and the clock waits while you are there (see "Your keys" below).

**Testing** (`DAY_NIGHT_KEYS=1`):
- **Ctrl+F7** swaps day and night: to the start of the full night when it's mostly day now, to the start of the full day when it's mostly night.
- **Ctrl+F8** pauses the clock (again: runs it).
- **Ctrl+F9** runs it at 1x, 10x (a fade takes 4 s), 60x (the whole day in 12 s).
- **Ctrl+F6** jumps to 5 seconds before the next fade, to watch one at normal speed.
- While the clock is paused or sped up, and for 4 seconds after a key, a thin bar across the top of the screen shows the cycle, starting at dawn on the left: amber day, blue night. The mark on it is where the clock is: white at 1x, yellow at 10x, orange at 60x, red paused, grey in a field that holds the clock. `DAY_NIGHT_BAR=0` turns it off.
- The log says what each key did and the time: `DAY_NIGHT: swapped: night, 0:00 in (dawn starts in 3:20)`. You can also set the time in `daynight.txt` while the game is closed (`time = 500` is the start of the night).

## Your keys in `_pbr.txt`

fieldprep rewrites `_pbr.txt` every run but keeps these:

| Key | What |
|---|---|
| `time = cycle/day/night` | (v15.8) `cycle` (default): the field follows the day/night clock. `day`: always as painted. `night`: always night (a story scene, a boss). In a `day` or `night` field the clock waits until you leave it (battles started from it too), then goes on. |
| `sky = 0/1` | 1: pixels at the farthest depth of your depth map are sky (no added light, no shadows, nothing reflected, characters never hidden by them). For a sky painted into the same layer as the foreground. Layers that scroll on their own (parallax, script scrolls) are found automatically and treated as sky either way. |
| `character_shade = replace/keep` | The game darkens characters by one value per field (for example −0.094 in bvtr_1) and scripts change it (flashes, silhouettes, fades). `replace` (default): DEMASTER notes each character's value once it has settled after the field loads; the room's light replaces that value, and later changes from scripts are added on top the way the game adds them. `keep`: the game's darkening on top of the room's light (for a field that sets its look after the fade-in). |
| `character_light = auto / <number>` | How bright the room makes the characters. `auto` (default): as bright as the game draws them, on average; the room gives the direction and colour. A number: the room's real light times it (1 = as bright as the room is). |

## Depth, camera and walkmesh

- fieldprep projects the field's walkmesh through its camera (Deling's convention; 3 mirrored variants are tried too) and reads your depth map where the floor lands. It then fits the depth model to the real distances: linear (`a + b·value`) or inverse (`1 / (a + b·value)`, for disparity maps like Depth Anything). The fit is RANSAC, so floor hidden behind things doesn't spoil it. The result goes in `_pbr.txt` as `camera = measured`.
- **`<field>_fieldprep_check.png`**: the field darkened, the walkmesh in green as the camera sees it, the lights as crosses with their range in yellow. **The green grid should lie on the walkable floor.** If it doesn't, the camera reading is wrong for that field: send me the image and the `_fieldprep.txt`.
- No camera or no good fit: the depth is estimated (`camera = estimated`). Lighting still works; distances in depth are just guessed.
- **New in v15**, written into `_pbr.txt` for the characters and per-pixel depth:
  - `occlusion.<Z>`: for every tile Z nearer than the base picture, how near the nearest part of what those tiles show is in your depth map (that part stays at the tile's depth, what lies behind it is pushed back);
  - `game_depth`: a check of the game's own depth scale against the tiles (16384 per view unit; the report says if your depth map and the tiles disagree);
  - `camera_eye/right/down/forward` and `up`: the camera itself and the world's up, so DEMASTER can place the game's script entities;
  - `sky_value`: the farthest depth there is (where `sky = 1` starts);
  - `probe_depth`: the distances the light grid's slices run between.

## The switches (demaster.conf)

| Key | Default | What |
|---|---|---|
| `FIELD_PBR_DEPTH` | 1 | Every background tile keeps the game's own depth, so tiles never change places among themselves and nothing the original hides shows through. A pixel only lets a character through where the character stands between the tile and what that pixel really shows: the floor inside a pillar's tile, with someone in front of the floor and behind the pillar. "What it really shows" comes from `_ND`, measured from the tile's nearest part (the distance a tenth of its pixels are nearer than, so a sliver of door frame in a door's tile doesn't count), with a 15% margin. DEMASTER copies the characters' depth right after drawing them for this. 0: per tile only, exactly like the game. |
| `FIELD_PBR_QUALITY` | medium | How much of the graphics card PBR fields use. **low** (integrated graphics): 2 shadow maps of one sample each, no reflections, no checks for walls between a surface and an added light. **medium** (GTX 1060 class, the default): 4 soft shadow maps, reflections, 6-step wall checks for added lights and 3-step ones for reflections. **high**: 8 shadow maps, 12- and 6-step wall checks. |
| `FIELD_GL_TRACK` | 1 | DEMASTER remembers the OpenGL state as the game sets it (hooks on the calls that set it) instead of asking the driver around each of the hundreds of character and background draws in a frame. Asking makes the game wait for the driver, a lot with NVIDIA's "Threaded optimization". It compares what it remembers with the driver a few times early on and then every half minute; if they ever differ, the log says what differed and it goes back to asking. 0: always ask (as v15 did). |
| `FIELD_PBR_SHADOWS` | 1 | Character shadows from the field's lights (real outlines, from the whole model), soft contact darkening around each character (sized from the game's round shadow), room shadows for added lights (walls and pillars block them: found by stepping across your depth map). Replaces the game's round shadows. |
| `FIELD_PBR_REFLECTIONS` | 1 | Characters and 3D models reflected in glossy and metal background surfaces, blurred by roughness, stronger at shallow angles, blocked by walls. A wall behind a character reflects its back, the floor in front its front. |
| `FIELD_PBR_CHARACTERS` | 1 | Characters lit by the room (the light grid), the field's lights (with flicker and room shadows) and their own maps. 0: drawn exactly as the game draws them. |
| `FIELD_AVATARS` | 0 | A camera in front of each character's head draws its face at 512×512 with the room's light, ready for portraits later. Updated while the face is turned towards the game's camera. **Ctrl+F10** saves them to `DEMASTER_EXP\avatars\`. |

With shadows, reflections or face pictures on, DEMASTER turns off the game's own back-face removal on PBR fields (two 6-byte jumps in its model code, turned back on in every other field), so the whole model arrives: the back for reflections, a solid body for shadows. The GPU hides the back faces when the model is drawn on screen, so it looks the same.

## Checking it in the game

`FIELD_PBR_VIEW` shows one thing at a time: on PBR layers 1 albedo, 2 lightmap, 3 normals, 4 depth, 5 AO, 6 roughness, 7 metallic, 8 flicker mask, 9 only the added light, 10 with every light at rest, **12 shadows** (blue: sky), **13 reflections** (×4), **14 how far each pixel is behind its tile's nearest part** (black: at the tile's own depth, nothing gets through; white: twice as far or more; orange: a tile without a measured nearest part; blue: sky); **16 the glows as they'll show at night** (the emissive map × the painting's colour there, with `[emissive]`'s hue, tint and strength), **17 the emissive map**; on characters **11 the room's light on grey** and **15 their normals**. 0 is normal.

With `FIELD_DEBUG=1` the log has, per field and per page:
```
FIELD_PBR: bvtr_1: 21 PBR layer(s) (drawn from their _A, lit with its _pbr.txt), 1 light(s), camera measured
FIELD_PBR: bvtr_1 page 0 (.map palette 3): 120 PBR cells, _LF 1024x1024 at 4x, BC7 block copies on the GPU, _ND ..., _ORME ...
FIELD_PBR: PBR pages are drawn with DEMASTER's field program (DEMASTER_EXP\shaders\demaster_field.vert/.frag)
FIELD_PBR: field characters are drawn with DEMASTER's character program (...), lit by the room, shadows, reflections
FIELD_PBR: bvtr_1: the room's depth from ..._ND.dds (1344x960)
FIELD_PBR: bvtr_1: light grid 40x30x8 from ..._PROBES.dds, average light 0.412
```
Lines worth sending me:
- **"does not compile"** / **"is missing"**: a shader file from this release isn't in the game's `DEMASTER_EXP\shaders\`.
- **"layers moving on their own (drawn as sky)"**: which layers DEMASTER found scrolling separately.
- **"have an _A but no _LF"**
- **"cells off the 16 grid"**: that page's PBR layers are drawn unlit.
- **"unexpected code at ... (FIELD_CULL_TRI)"**: the back faces can't be turned on in this game version; characters keep only the halves the camera sees.

## Characters and field models

DEMASTER collects every model the game draws in a frame (the game sends them already projected on its screen, in runs of up to 32 polygons, about 70 draws per character), and right before the background:

1. **Puts every vertex back in the room.** The background tiles of the last frame say where the field's canvas is on the screen; the depth the game gives each vertex (4 × its camera distance) says how far it is. That's the same space your depth map and the lights live in.
2. **Smooth normals** over the whole model (faces sharing a vertex, within 65° of each other, so soles and collars stay hard).
3. **Tells the characters apart**: one texture per model; the game's round shadows (where each character's feet are, and how dark the game made the shadow) split two NPCs with the same model.
4. **Draws the pictures** reflections use (each character from the front, and from behind mirrored), the **shadow maps** (the characters as the strongest lights near them see them, up to 8), and the face pictures. The frame's models go to the GPU once (one vertex buffer), and every pass draws from there. With instancing (`GL_ARB_draw_instanced`, every GL 3.1 driver) one draw makes all of a character's shadow maps, and one both of its pictures: copy *i* goes through its own camera into its own tile, and what falls outside the tile is thrown away. Without it (the log says why), one draw each, as before.
5. **Draws the models** with `demaster_chara.vert/.frag`, in the game's order (opaque parts first, semi-transparent ones after), each with the game's state (texture, blending, cut-out test, darkening, script tints).

### Model textures: DDS with your mip levels

When the game copies a model image (`field.fs\field_hd_new\d001_0.png` and so on) into the model's atlas, DEMASTER puts `d001_0.dds` there instead if it exists and is the newer of the two (as for layers), **with all its mip levels**. Filtering stays nearest inside each level (no softening), and blends between your levels only when the model is drawn smaller than the texture. Keep the PNG next to the DDS: the game reads the PNG to place it.

### Model PBR maps

Next to the image, laid out exactly like it:

| File | RGB | A |
|---|---|---|
| `d001_0_N.dds` | normal: x along the image's right, **y up the image (OpenGL, green up)**, z out of it | free (kept for a later mask: subsurface, anisotropy) |
| `d001_0_ORME.dds` | AO, roughness, metallic | emissive (the albedo's own colour glows where it's white) |

- Opacity stays in the albedo's alpha (the game's cut-out test, under about 44%, drops hair strands and fringes).
- Mirrored and rotated UV islands need nothing: the shader works out per pixel which way the texture's x and y run across each triangle (a cotangent frame), so a mirrored island gets a mirrored frame.
- If your normal generator writes green down (DirectX), flip green when you export.
- PNG works too (its levels are made from it). Missing maps: flat (smooth normals only) and plain (AO 1, roughness 0.7, not metal, no glow).
- Debug view 15 colours the character normals: a bump should come out, not in, on both halves of a mirrored part.

### Making them with fieldprep (v15.9)

`fieldprep model` makes the DDS files from your PNGs, for one image or the whole folder:

```
fieldprep model "<game>\DEMASTER_EXP\textures\field.fs\field_hd_new\d001_0.png"    (one image; any of its maps works too)
fieldprep model "<game>\DEMASTER_EXP\textures\field.fs\field_hd_new"                (every image there that has maps)
```

| You put next to `d001_0.png` | fieldprep writes |
|---|---|
| `d001_0_A.png` (or `_AM`): the albedo | `d001_0.dds`: BC7 sRGB. Without `_A`, made from `d001_0.png` itself |
| `d001_0_N.png`: normal, green up | `d001_0_N.dds`: BC7, alpha left white so every bit goes to the normal |
| `d001_0_O.png`, `_R.png`, `_M.png`, `_E.png` (grey; `_RC`/`_RM`, `_MC`/`_MM` work too), or one packed `d001_0_ORME.png` | `d001_0_ORME.dds`: BC7, packed AO / roughness / metallic / emissive. Missing ones are plain (AO 1, roughness 0.7, not metal, no glow); a coloured emissive map counts its brightest channel |

- **Every mip level, made for what each map is:** the albedo in linear light, with see-through pixels kept out of their neighbours' colour, and cut-outs keeping how much of them passes the game's 44% test (hair and fringes don't thin out when the model is small; `--coverage 0` turns it off). The normal map's levels are made unit length again. The ORME map's four channels are averaged each on its own (emissive 0 doesn't wipe the others).
- **Sizes:** a map at another size than `d001_0.png` is resized to it (the game places the image by its PNG), and the log says so. Any size works, not only multiples of 4.
- **Only what changed:** a DDS newer than every PNG it's made from is left alone, so running the whole folder again only redoes what you've changed. `--force` makes them all again.
- **Keep `d001_0.png`**: the game reads it. DEMASTER takes the DDS because it's newer. If you save the PNG again after, run fieldprep again.
- **Options:** `--flip-green` (your normal maps are green down, DirectX), `--quality 0-4` (BC7 effort, default 2), `--threads`. A folder runs one image per thread. The log is also saved as `fieldprep_model.txt` in the folder.
- **Time:** about 4 s for a 1024×1024 image with all three maps on 2 threads here; `--quality 0` takes about a third of that and measured within 0.1 level of 2 and 4.

## Frame log (for reflections, shadows and character lighting)

`FIELD_FRAME_LOG=1` (on in this release's `demaster.conf`; it costs nothing until you use it). In a field, press **Ctrl+F11** once. DEMASTER writes down everything the game draws in the next frame into `DEMASTER_EXP\frame_log\<field>_<date>_<time>\`:

- `frame.txt`: every draw in order (`glDrawElements`, `glDrawArrays`, `glBegin`):
  - the buffer it draws into, the shader program and its uniform values (`vertextype`, `blend_mode`, `ambientColor`, matrices...);
  - blending, depth test and writes, culling;
  - the texture and what kind the game filled it as (57 = field character);
  - the vertex arrays, the range of every vertex coordinate, how many triangles face each way, and the first vertices in full.
- `programs.txt`: the source of every shader the game draws with.
- `framebuffer_<n>_colour.png`, `framebuffer_<n>_depth24.png` (depth, exact: 24 bits packed into red, green and blue), `framebuffer_<n>_depth_view.png` (the same, stretched so you can see it), and the same for `screen_`.
- `texture_<n>.png`: the textures used, by the numbers in `frame.txt`.

The top of `frame.txt` also says how long DEMASTER's steps took on the CPU (the average of the frames before the logged one: reading the characters, drawing them, shadow maps, the background set-up), and the end how long they took on the GPU in the logged frame (timestamps between the steps, and the PBR background draws added up).

**What to send:** one log from a field with the party on screen, standing where their round shadow shows (bvtr_1 is fine). Zip the folder and send it with `demasterlog.txt`.

## How it's drawn

For each pixel of a PBR layer, inside the draw the game already makes (with DEMASTER's field program for that draw only):

1. **Where it is.** A small table per page says where each 16×16 cell's tile is on the canvas, its Z and layer. With the depth from `_ND` and the camera from `_pbr.txt`, that gives the pixel's position in 3D (view space).
2. **Painted light.** albedo × lightmap × (1 + mask × (flicker − 1)), where the flicker comes from the nearest light that flickers (or follows a group). Characters' shadows take away the share of a lamp's painted light you set with `shadow`; contact darkening around their bodies.
3. **Added light.** Each light's intensity: Lambert diffuse plus GGX specular (Smith-Schlick geometry, Schlick Fresnel, metallic workflow), from the normal, roughness and metallic, with a smooth range falloff, blocked by characters (shadow maps) and by the room (stepping across the depth map). AO only dims this added light, never the painted one.
4. **Reflections.** The reflected ray meets the characters' cards (their pictures stood up where they are), blurred by roughness.
5. **Depth.** The tile's depth goes to the depth buffer exactly as the game sends it (the GPU can skip hidden pixels before lighting them). With `FIELD_PBR_DEPTH`, a pixel with a character between the tile and what it shows isn't drawn, so the character shows.
6. **Glows.** The emissive map (`_ORME`'s alpha) says which pixels are the painting's own glow: a bulb, a screen, a sign. By day nothing changes (the painting has them already). As the room goes dark at night, they stay lit: the painting's own colour there × the emissive map, turned and tinted by `[emissive]` in `_lights.txt`. The game's additive glow layers are drawn as painted either way.
7. **Night.** The painted light (step 2) times `[night] level` × `tint` away from the night's lamps, kept around them (`lamps`, within their range); how much of that applies follows the clock (0 by day, 1 at night, smooth through the fades).

Cost: three more textures per page (1 MB each in BC7 at 4x), the room's `_ND` once per field, two loops over at most 8 lights per pixel, and per frame a few thousand character triangles sent to the GPU once and drawn a handful of times (on bvtr_3: about 1.1 MB once, instead of about 7 times; 6 extra draws for 3 characters instead of 18).

## What was tested here

- **fieldprep end to end, on bvtr_1 with a synthetic set** (a room with a known camera, walkmesh, depth, normals and a warm lamp): camera depth model within 0.1%, the lamp within a pixel and its colour, depth bright-is-near and normals green-up; reading the field from `main.zzz` (LZSS and LZ4); the light grid and the depth measurements.
- **The shaders, run in Mesa's software OpenGL:**
  - `tests/pbr_shader_test.cpp`: `demaster_field.frag` with the pages bound as DEMASTER binds them gives back bvtr_1's paintings within 2 levels (mean 0.06) at rest, FIELD_SMOOTH on or off; flicker changes only the light's pool.
  - `tests/field_depth_test.cpp`: the tiles' nearest-part pass (from `texturepatch_v2_fieldPbr.cpp`) measures each tile, leaving out what the layer doesn't cover and a thin sliver of something nearer; a character behind a door stays hidden (also with a sliver of the door's frame in its tile); a character between a pillar and the floor in the pillar's tile shows where the floor is and is hidden where the pillar is; behind the floor, the floor hides it; a far tile drawn after a near one never covers it; the depth buffer holds the tiles' own depth.
  - `tests/chara_shader_test.cpp`: every DEMASTER program compiles and links; with the room's light off, `demaster_chara` draws exactly what the game's `main_char.frag` draws (random triangles, colours, darkening values, cut-outs); lit by a flat room it matches the game within 1 step; the reflection pictures come out the right way round from the front and from behind; instanced, four pictures (lit and normals, mirrored and not, two box and two perspective cameras) and four shadow maps of a model bigger than the cameras see, in one draw each, match four draws with a viewport each (1 pixel of 76,800 differs, on a triangle's very edge), and no copy spills into another's tile.
- **PuPu's images (v15.8):**
  - `tests/field_layers_test.cpp` (88,619 checks) compares DEMASTER's PuPu names and frames with PuPuSharp's own code (`tests/pupu_oracle`, built with .NET) on bdifrit1's `.map` and 300 random ones. The rule matches all 14 of your bdifrit1 file sizes.
  - Pages put together from PuPu's pictures are identical to pages from FME's.
  - fieldprep on a bdifrit1 set with PuPu's names and sizes: canvas 336×240, nothing resized, `_ORME` 1344×960. With FME's: unchanged from v15.7. With your maps made for FME's canvas: moved into place, and the results equal the FME run's, cropped.
  - `pbr_shader_test` draws the PuPu-framed field identically to the FME-framed one.
- **fieldprep model (v15.9):** `tests/fieldprep_model_test.cpp` (5,535 checks): names, level sizes (odd ones too), the albedo's levels in linear light without see-through colour bleeding, cut-out coverage kept (within 0.07 where plain averaging lost 0.20), unit-length normals at every level, ORME's channels and defaults, resizing, the DDS files read back level by level (BC7 within about 2 levels in the big levels), only what changed made again, `--force`, `--flip-green`. The DDS files were also read with bimg (the image library DEMASTER reads them with): every level found at its size, the pixels within 1 level of fieldprep's own decode (the two decoders round differently).
- **Day and night (v15.8):**
  - `tests/field_pbr_test.cpp`: the clock (day, night and the fades centred on the changes, smooth; battles 3 times slower; swap, the jump before a fade, wrapping, the file), which lights are on when (each time of day's 2 + 6, the cross-fade weights, the night's lamps, 16 in a fade: the 8 brightest), `[night]` and `time` read and written.
  - `tests/pbr_shader_test.cpp`: at night with no lamps the shader gives the painting × level × tint (within 1 level, mean 0.39); with a night lamp, at the lamp the painting keeps `lamps` of its light (within 1) while far from it it's level × tint; by day (night 0) the paintings come back as before.
  - `tests/chara_shader_test.cpp`: characters lit by a flat room at night get the room's light × level × tint (within 2); night 0 is exactly the day.
  - Not run here: the clock, the keys, the bar and the file in the game.
- **The shared code:** `tests/field_pbr_test.cpp` (17,692 checks: names, settings (with the canvas), lights, the `[emissive]` settings, the emissive map from a residual (a bulb glows, a lit wall and a dark corner don't), groups, flicker, camera, depth fit, the light grid file, packing, BC7/DDS, PNG; inverse lighting on a box room lit by two large lamps and a sconce: the two large ones found within a tenth of their distance with their colours, the sconce left out; your green mark added, nothing on your red one; the night's two lamps found in a _RESM-like picture; your own lights kept on relight; the 2 + 6 per time of day) and `tests/field_chars_test.cpp` (back to view space, smooth normals, round shadows, telling characters apart, facing).
- **Not run here: the DLL in the game.** Characters drawn by DEMASTER, the back faces, shadows, reflections, light groups and the model DDS swap all need a test in the game. Use `FIELD_DEBUG=1`, a frame log (Ctrl+F11: the log now notes where DEMASTER drew the characters and which round shadows it left out) and send `demasterlog.txt`.

## Not covered yet

- **The day/night clock and your game saves.** The time of day is DEMASTER's own (one `daynight.txt`), not saved with each game save: loading an older save keeps the time it is now. Tying it to the saves needs the game's play-time counter in memory, not found yet.
- **Night on the rest of the game.** Battles, the world map, non-PBR fields and layers without PBR maps stay as painted at night (the clock still runs through them).

- Particles (fire, sparks, save point glow) as light sources, and status effects: with the battle magic module.
- Language-specific maps (`<field>_<lang>_AM.png`): handled per `.map` like the layers, but not tested on a real multi-language field yet. Fields that only have per-language `.map` files (bvtr_3) work with images named without a language: fieldprep uses the English `.map` for them (the report says so), and DEMASTER finds the plain-named files for every language.
- Light groups in the Japanese game.
