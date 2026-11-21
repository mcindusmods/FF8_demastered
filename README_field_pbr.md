# DEMASTER PBR fields (v15.3): backgrounds, characters and models

Field backgrounds lit in the shader from your Marigold/CHORD maps, and (new in v15) the field's characters and 3D models lit by the same room, with real shadows, reflections in the background and per-pixel depth. Two separate parts:

- **The PBR modules in DEMASTER** (`FIELD_PBR` and the `FIELD_PBR_*` switches) draw a layer as **albedo × lightmap**, let lights flicker or follow the game's light groups, and add light on top using the normals and roughness/metallic. With every light at rest you get your painting back, pixel for pixel. Background pages are lit inside the draws the game already makes. Characters are collected and drawn by DEMASTER itself, in the game's order, right before the background.
- **fieldprep.exe** is the preparation tool and never runs in the game. It reads your maps, works out the lightmap, finds the lights, bakes the AO, measures where things are in depth, bakes the room's light into a 3D grid for the characters, and packs everything into the files DEMASTER loads. It shares DEMASTER's field code (`field_layers`, `field_pbr`), so both sides agree on names, cells and depth.

Only opaque layers (blend 4) are PBR. The glow and shadow layers (blend 0-3, like bvtr_1's `001FF000`) are drawn exactly as before, on top. Fields without PBR files are drawn exactly as before, characters included.

## Setup

1. Build with `build_demaster.bat`. It makes `ff8_demaster.dll` and `fieldprep.exe` (64-bit).
2. Copy **every file** in `DEMASTER_EXP\shaders\` into the game's `DEMASTER_EXP\shaders\`: `main.frag`, `main_char.frag` and `main_notex.frag` (v15.15: the game's own, with the colour grade and, for the world map's sky, the night), `demaster_field.vert/.frag`, `demaster_chara.vert/.frag`, `demaster_shadow.frag`, `demaster_fighter.vert` (battles). Copy `DEMASTER_EXP\lut\` too (the colour grade's LUTs, see "The colour grade"), and `field_time.txt` (Deling City's time by the story, see "Day and night"; DEMASTER writes it if it's missing). Without them, PBR layers fall back to their paintings and characters are drawn by the game as before; the log says which file is missing.
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
| `<field>_E.png` | the glows | **a picture of them, in colour** (v15.12): what lamps, lit windows and signs look like once it's dark. Painted on black, or with an alpha of its own. Packed into `_EC` (see "The glows as a picture"). A grey one without alpha is the old mask (white: the painting's own colour glows there), and without any `_E` fieldprep makes that mask from `_RESM`. `_H` (height) isn't used |
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
| `<field>_ORME` | AO, roughness, metallic | emissive mask (an older `_ORMH` still works: its alpha, the height, is left out). With an `_EC`: plain white, not read |
| `<field>_EC` | the glows' colour, already times its coverage | coverage: how much of the painting the glow takes the place of |

Plus:
- `<field>_PROBES.dds`: the room's light in 3D, for the characters (a small 3D texture, about 0.5 MB). For points in front of the camera it stores how much light arrives from each direction (your painting's light, from its depth), and how much of it flickers.
- `<field>_O.png` (baked AO, made only once so your edits survive), `<field>_pbr.txt` (canvas, camera, depth model, lightmap scales, depth measurements; rewritten every run, your three keys kept), `<field>_lights.txt` (written once, then yours) and `<field>_fieldprep_check.png`.

- **fieldprep stops when layer images are missing (v15.14).** If an opaque layer image the whole field needs (`<field>.png` is put together from the opaque layers' first animation states) isn't there, fieldprep writes nothing and lists the missing ones by both names (PuPu's and FME's). The `<field>.png` fieldprep puts together carries a note in the file saying so, and the next run puts it together again instead of taking it as yours; a `<field>.png` without the note (yours, or from before v15.14) is used as it is (`--force` with `compose` makes it again).
- **The albedo warning (v15.14).** When the lightmap fieldprep works out is much brighter than the albedo (the light more than 2× the albedo on most of the field, or up to 6× or more somewhere), the albedo is probably from another version of the painting or another set of layers: fieldprep says so. Such a lightmap turns grey or too dark at night and when the look relights the field. "albedo raised in N pixels" now counts only pixels raised by more than 2 steps of 255.
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
intensity = 0.200               ; light added on top (0 keeps the painting exactly); it rolls off towards white, so 1-3 is usable
range = 75.0                    ; native px at the light's depth
night_radius = 120.0            ; new in v15.14, a lamp on at night: how far around it the painting keeps its light
                                ; at night (native px; left out: its range)
flicker = fire                  ; none | fire | candle | pulse | strobe | fluorescent
amount = 0.600
speed = 1.000
specular = 1.000
shadow = 0.5                    ; how much a character's shadow takes away from this light (0-1)
time = day                      ; day | night | both: when it is on (day and night lights cross-fade with DAY_NIGHT's clock)
source = found                  ; found (fieldprep's), mark (your _LIGHTS.png), yours (you wrote it here: kept on --relight)
follows = 01                    ; new: follow the game's light group 01 (the animation id in the pupu: 004 01 000)
on_states = 1, 2                ; new, with follows: on only while the group shows one of these frames

[emissive]                      ; the field's glows (your picture of them, _EC; else the old mask), for the whole field
hue = 0.0                       ; degrees their colour turns around the colour wheel (-180 to 180)
tint = 1.000 1.000 1.000        ; times this (linear)
strength = 1.000                ; how bright (1: as painted)
day = 0.000                     ; new in v15.12: how much of them shows in daylight (0 none, 1 all, as at night)

[night]                         ; the field at night (v15.8), for the whole field
level = 0.180                   ; how much of the painted light is left away from the night's lamps (0-1)
tint = 0.600 0.700 1.000        ; what's left times this (linear): the moonlight's colour
lamps = 0.900                   ; how much of the painted light stays around the lamps on at night, within their range (0-1)
```

- **How fieldprep finds the lights: inverse lighting** (as in Marschner & Greenberg, "Inverse Lighting for Photography", 1997). It doesn't look for bright spots. It asks which few point lights, shining on the room as your normal and depth maps shape it (n·l / r², out of view too), best explain the diffuse light in the painting (`_BL` if you have it, else the lightmap), on top of a smooth ambient (a constant, the surfaces' facing and a slope across the screen). It adds the light that explains the most of what's left, fits all of them again (none can be negative), moves each around to where it fits best, and stops when a new light would explain less than 2% (`--light-share`). Then it keeps only the large ones (see below) and fits them again. A light that ends up explaining almost nothing is dropped. Colour comes from the same fit per channel, range from where its light falls to a tenth of the ambient. The report and each light's comment say how much of the light it explains.
- **Two large lights per time of day, small ones yours.** fieldprep keeps at most 2 lights by day and 2 by night (`--max-lights`): the ones that light the most of the field (where each adds a tenth or more to the light already there), which are the room's own light, ceiling lamps and windows. A sconce or a lit sign lights only its spot, however bright: fieldprep leaves it out and fits the large ones again without those spots, so they don't lean towards them. Those spots are for the emissive map and for your own lights. The ones fieldprep keeps start on: they add `--light-intensity` (0.2) on top of the painting and cast character shadows (`shadow` left out: 0.5, as for every painted light). Each light's comment says how much of the light it explains and how much of the field it lights.
- **Your own lights: up to 6 per time of day**, for the small lamps that matter (a desk lamp a character walks past, a candle). Write them into `_lights.txt` yourself (`source = yours`, or leave `source` out) or mark them in `_LIGHTS.png` (below); marks count towards the 6. Lights you wrote yourself are kept when fieldprep finds the lights again (`--relight`, or a newer `_LIGHTS.png`); fieldprep's and your marks' are found again. A file from before v15.7 has no `source` lines, so all of it is taken as fieldprep's: copy anything you added from `<field>_lights_old.txt`. **On at a time (v15.14): 2 by day, 6 by night.** By day the sun takes one of the two places in a field that has it (`sky = 1`, or `[sun] on = 1`), so the day is the sun and your one light; without the sun, two lights. By night six (the moon has the sun's place). Within a time of day yours come first, in file order, then fieldprep's. 1 + 1 + 6 is the 8 the shader draws, so nothing drops out while day and night cross-fade. More are left out, and both fieldprep and DEMASTER's log say so. For a field with a lot of lamps, use the night's six with the glows (`_EC`).
- **Day and night lights.** fieldprep finds the day's lamps in the painting's light (as above) and the night's in `_RESM` (what the painting's glows give off, close to the field at night), the same way. A lamp found in both, at the same place, is one lamp on day and night. Each light says when it is on: `time = day`, `night` or `both` (left out, as in older files: both). With the day/night clock (below) the day's lights fade out and the night's fade in. No `_RESM`: no night lights. A file holds up to 32 lights, day and night together; at most 8 are on at a time.
- **Your marks: `<field>_LIGHTS.png`**, painted on **black**, at the field's size (any other size works too). Each separate spot becomes one lamp at its middle, kept even if the fit gives it nothing, always on: **pure green** (0, 255, 0) a day lamp, **pure blue** (0, 0, 255) a night lamp, **cyan** (0, 255, 255) a lamp on day and night. **Pure red** (255, 0, 0) over anything that isn't a lamp's light (a floor reflection, a glowing sign, a spot fieldprep got wrong): no lamp, day or night, goes there, and that part of the painting isn't explained by lamps at all. Any other colour is ignored. Use a hard brush (no soft edges or anti-aliasing) and save as PNG. When `_LIGHTS.png` is newer than `_lights.txt`, fieldprep finds the lights again on its own (the old file is kept as `<field>_lights_old.txt`).
- **`[emissive]`: the glows' colour**, for the whole field: `hue` turns the painted colour of every glow around the colour wheel (in degrees, as an image editor's hue slider does), `tint` multiplies them (what colours a white glow, which has no hue to turn), `strength` makes them brighter or dimmer. fieldprep writes the section with `hue = 0`, `tint = 1 1 1`, `strength = 1` and keeps yours when it finds the lights again. It shows as the room goes dark at night (below); `FIELD_PBR_VIEW=16` shows it any time.
- **The glows as a picture (v15.12): `<field>_E.png` in colour.** Paint what glows the way it should look at night: a window that is blue by day can be yellow. `fieldprep orme` packs it into `<field>_EC`, and at night it is laid over the dimmed painting: painting × (1 − coverage) + glow.
  - **On black, no alpha:** the bright parts take the painting's place, the dim ones (a halo around a lamp) add their light to it. `--emissive-cover <%>` sets the brightness from which a pixel replaces the painting entirely (default 50; below a fifth of that it only adds).
  - **With an alpha of its own:** a layer over the night painting, exactly as your paint program shows it with that layer on top.
  - `[emissive] day` shows that much of it in daylight too, for a room that needs its lights on by day. `hue`, `tint` and `strength` still apply. One layer's own: `<field>_<pupu>_E.png`.
  - A **grey** `_E.png` without alpha is still the old mask (the painting's own colour glows there), and nothing changes for fields that have one.
- **Pictures of an animation state share their state's maps (v15.12).** A field whose whole scene has two states (bccent_1: `0040100x` and `0040101x`) needs `_ND` and `_ORME` per state, since the field's own show only one of them. Give them to the state's first picture (the name ending in 0, e.g. `bccent_1_00401010_N.png`): the other pictures of that state (`…1011` to `…1015`) take them when they have none of their own, before the field's. The same goes for `_EC`. **No `_A` needed for the other frames (v15.14):** a picture of an animation state without an `_A` of its own, whose state's first picture has one, is drawn from its painting and still lit like the rest (DEMASTER takes the albedo out of the painting with the lightmap, painting ÷ light, as fieldprep splits it). fieldprep still writes an `_A` for every opaque picture; delete a frame's `_A` to have it drawn from its painting, keep it (or make your own) to override.
- None flicker at first. Set `flicker` on the ones that should, then **run fieldprep again**: the mask in `_LF`'s alpha is worked out from the baked lights that flicker or follow a group (the painted light above the field's ambient level, faded out by range and by surfaces facing away). Since v15.14 that is the day's lamps against the bounced light (`_BL`, else the lightmap) and the night's lamps (`time = night`) against `_RESM`, each pixel taking the stronger, so a lamp that only burns at night flickers too. A pixel flickers with the nearest such lamp that is on and reaches it, so a night lamp's part stays still by day. After that, the flicker's type, amount and speed change live.
- **`shadow`**: a character standing in a lamp's light takes away this much of the light the lamp adds, and of the lamp's painted light where its shadow falls (there's no way to tell how much of the painting came from that lamp, so you say). Default: 1 for a new light, 0.5 for a painted one. 0: casts no character shadows.
- **`follows`**: the game keeps a colour and a frame number for every background light group, and field scripts drive them (BGANIME switches the frames: a lamp switching on, an animated fire; BGSHADE changes the colour: an alarm pulsing, a fade). A light that follows a group is coloured by it (128 = as painted) and, with `on_states`, is on only while the group shows those frames. A painted light that follows a group goes dark with it too (its part of the painting is under the `_LF` mask: run fieldprep again after adding `follows`). Only in the English/European game; the Japanese game's table isn't known yet, so there lights ignore `follows`.
- **The canvas line in `_pbr.txt`** (`canvas = -168 -120 336 240` for bdifrit1 from PuPu): the part of the field the whole-field maps cover, in `.map` coordinates (left, top, width, height, native pixels). DEMASTER puts `_LF`, `_ND`, `_ORME`, the lights and the camera where that says. A `_pbr.txt` without it (from v15.7 or older) means FME's render area. Leave it to fieldprep: it's rewritten every run.
- DEMASTER reads `_lights.txt`, `_pbr.txt`, `_A`, `_LF`, `_ND`, `_ORME` and `_PROBES.dds` again within a second of you saving them (`FIELD_LAYERS_LIVE=1`).

## Day and night (v15.8)

One clock for the whole game (`DAY_NIGHT=1`): 8 minutes of day and 4 of night, with 40-second fades, in fields, on the world map and in menus. Battles run the same clock 3 times slower (24 + 12 minutes, 120-second fades), so when a battle ends the field carries on from wherever the battle left it. It runs while the game draws frames. Each save slot has its own time of day (below); with `DAY_NIGHT_SAVES=0` there is one clock for the game, kept in `DEMASTER_EXP\daynight.txt` between sessions (written every 30 seconds and after every key). The lengths are in `demaster.conf` (`DAY_NIGHT_DAY`, `_NIGHT`, `_FADE`, `_BATTLE`).

**What changes at night**, on PBR fields:
- **The lights.** Lights with `time = day` fade out, `time = night` fade in, `both` stay. Two are on by day and six by night (see "On at a time" above), so both sets fit during a fade. **The evening (v15.14, `DAY_NIGHT_EVENING=0.9`):** the day's own lights start going, and the glows start coming on, at 90% of the day, before the dusk's fade begins; both are done when the fade ends. Dawn is the fade itself. `1` puts them back on the fade.
- **The painting dims.** Away from the lamps that are on at night, the painted light goes down to `[night] level` times `tint` (moonlight). Around those lamps (`time = night` or `both`, `baked = 1`), the painting keeps `[night] lamps` of its own light, most at the lamp and fading out towards the edge of its `night_radius` (v15.14; left out: its `range`), so a lamp's pool at night can be bigger or smaller than the light it adds. Change `[night]` in `_lights.txt` while you stand in the field at night: it's read again within a second.
- **The glows stay lit.** With a picture of the glows (`_EC`): see "The glows as a picture" above. With the old mask: where the emissive map says a pixel glows, the glow (`[emissive]`) takes the dimmed painting's place as the room goes dark: a fully glowing pixel looks as painted (with `strength = 1`, `tint = 1 1 1`, `hue = 0`), a half glowing one halfway between that and the dimmed painting. Up to v15.9 the glow was added on top of the dimmed painting, which counted the glow twice and burnt lamps out.
- **Night pictures (v15.14): `<field>_<pupu>_nt_A.png`.** A layer that looks different at night (the sky, water, a window view) can have a night picture: how it should look at night, painted as it is (not an albedo). At night the layer is that picture (the clock cross-fades to it; its alpha says how much of it, so a picture with holes keeps the usual night there); the glows and the lamps' added light still go on top. `nt` so it isn't taken for `_N`. A `<field>_nt_A.png` at the canvas's size does the same for every layer. Each animation frame's night picture is its own (a frame without one has none). fieldprep makes their DDS with the layer images.
- **Characters lit by the room** dim the same way (their light grid times the same level, tint and lamps). Characters the game lights itself (`FIELD_PBR_CHARACTERS=0`, or fields without PBR) don't change.
- **Not changed:** fields without PBR files and layers without PBR maps (layers with no `_A`, and the game's glow and shadow layers, blend 0-3). Battles and the world map have their own night (`SCENE_LIGHT`, see "Battles and the world map").

**Per save slot (v15.15, `DAY_NIGHT_SAVES=1`):** every save slot has its own time of day, and nothing is shared between saves. When the game writes a save, DEMASTER notes which file it was, the game's own play time and the time of day in `DEMASTER_EXP\daynight.dat`. Saving over a slot replaces that slot's time. A save brings its play time back when it is loaded, and DEMASTER puts the clock back to that save's time of day (the log: `DAY_NIGHT: a save was loaded (slot1_save01.ff8, play time …): back to the time of day it was saved at`). A new game starts in the morning. A save with no time of its own (made before this, or on another computer) gets the time its play time gives: the new game's morning plus its play time, around the cycle, so it is the same every time you load it, until you save it again. DEMASTER sees a save being written by watching the save folder (`Documents\My Games\FINAL FANTASY VIII Remastered`, or `DAY_NIGHT_SAVE_FOLDER`); it doesn't touch the save files.
- `daynight.dat` is not a text file to edit: it is packed, scrambled and sealed with a checksum, and if it has been changed outside the game DEMASTER uses none of it (the log says so) and writes a new one at the next save. It isn't encryption, only enough that the time of day can't be changed in Notepad.
- v15.14's `daynight_saves.txt` is read into `daynight.dat` once and then deleted. `daynight.txt` is not used while each save has its own time.
- The testing keys below still change the time (and the next save keeps it): turn them off (`DAY_NIGHT_KEYS=0`) for players.

**A field's own time:** `time = day` or `time = night` in its `_pbr.txt` fixes it, and the clock waits while you are there (see "Your keys" below). A battle started there is fought by day (v15.14).

**The story's time (v15.15, `DEMASTER_EXP\field_time.txt`):** rules that say a field's time for part of the story, over its `_pbr.txt`'s. Each is a `[rule <name>]` with `field =` (names, `*` for any letters, `!` before one it isn't for), `moment =` (the story's counter, the savemap's "game moment", e.g. `359-419`; left out: the whole game) and `time = cycle | day | night`. The last rule that holds decides; they're applied when a field is entered and every frame in it, so a scene that moves the counter on changes the time on the spot. The file is read again within two seconds when you save it, and written with the defaults when it's missing. The defaults are for Deling City, which the game paints at night all the time: it follows the clock (also where a `_pbr.txt` says night), and is night for Laguna's visit to Julia (counter 221-234) and for the sorceress parade, from the city square after Caraway's mansion to the fight with Edea on the float (359-419). For it to have a day, give its fields day paintings and their night paintings as `_nt_A` night pictures. The log says which rule holds, with the counter, whenever that changes (`DAY_NIGHT: glyagu1: [rule the sorceress parade] in field_time.txt holds (the story's counter is 392): always night here`).

**Testing** (`DAY_NIGHT_KEYS=1`):
- **Ctrl+F7** swaps day and night: to the start of the full night when it's mostly day now, to the start of the full day when it's mostly night.
- **Ctrl+F8** pauses the clock (again: runs it).
- **Ctrl+F9** runs it at 1x, 10x (a fade takes 4 s), 60x (the whole day in 12 s).
- **Ctrl+F6** jumps to 5 seconds before the next fade, to watch one at normal speed.
- While the clock is paused or sped up, and for 4 seconds after a key, a thin bar across the top of the screen shows the cycle, starting at dawn on the left: amber day, blue night. The mark on it is where the clock is: white at 1x, yellow at 10x, orange at 60x, red paused, grey in a field that holds the clock. `DAY_NIGHT_BAR=0` turns it off.
- The log says what each key did and the time: `DAY_NIGHT: swapped: night, 0:00 in (dawn starts in 3:20)`. With `DAY_NIGHT_SAVES=0` you can also set the time in `daynight.txt` while the game is closed (`time = 500` is the start of the night).

## The colour grade (v15.15, `LUT=1`)

The time of day also changes the picture's colours: a golden dusk, a rose dawn, a cool night, the day as painted. It's done with LUTs (colour look-up tables: for every colour, the colour it becomes), one for each time of day, cross-fading with the clock, everywhere the clock is: fields, battles and the world map. The shaders do it on the scene itself (backgrounds, characters, models, battle stages, fighters, the world map), not on the menus, the text, or effects that add light (spells, glows, lamp halos), so those keep their colours.

**When.** The dusk comes with the evening (`DAY_NIGHT_EVENING`, while the day's lamps go out) and is fullest in the middle of the dusk's fade, when the sun is down; over the next minute (one and a half fades) it turns into the night: the blue hour. The dawn comes over the minute before the middle of the dawn's fade and goes into the day over the minute after it. In battle that's three times slower, like the clock. A field with `time = day` or `night` in its `_pbr.txt` has that time's LUT; a battle has its time as `SCENE_LIGHT` gives it (`scene_light.txt`, and where the battle started).

**The files** are in `DEMASTER_EXP\lut\`: `day.png`, `dusk.png`, `night.png`, `dawn.png`, and the Lunar Cry's `lunar_day.png`, `lunar_dusk.png`, `lunar_night.png`, `lunar_dawn.png`. Each is a strip of 32 squares of 32 x 32 (1024 x 32), the layout ReShade and Unreal use: in a square red goes right and green down, and blue steps from square to square. Other sizes (16, 64: n x n squares side by side, or one under the other) and `.cube` files (Resolve, Photoshop and most LUT tools export them) work too. A missing one leaves that time's colours as they are. Change one while you play: it's read again within two seconds.

**A place of its own.** `lut\field\<field>\`, `lut\battle\a0stgNNN\` and `lut\world\` can hold any of the same files; each one there replaces the whole game's for that time, in that place only, and the times it doesn't have stay the whole game's. The log says which files a place uses (`LUT: field bghall_1: day lut\day.png, dusk lut\field\bghall_1\dusk.png, ...`) the first time, and for every place that has its own.

**Making your own.** Grade a screenshot in any editor that can export a LUT (or a `.cube`), or start from the ones here: `tools\make_luts.py` makes them from a few numbers per time of day (white balance, the tint of the shade and of the light, how much the brightest parts keep their colour, saturation, contrast, the night's air) and writes a preview sheet of a screenshot through all eight (`python tools\make_luts.py --preview day.png night.png sheet.png`).

**What the ones here do.** Day: nothing (it's there as a neutral one to start from). Dusk: amber light, violet shade, a little more colour and depth. Dawn: rose and peach light, lavender shade, a soft haze, a little quieter. Night: the night itself (PBR fields dimming, `SCENE_LIGHT` in battles) is already there before the grade, so the night LUT doesn't darken: it cools the shade, takes colour out of the dark parts the way eyes do at night, and leaves the lamps warm. The Lunar Cry's: the same four times under a red moon, greens and blues losing their colour.

**The switches.** `LUT_STRENGTH` (0-1) is how much of the grade. `LUT_INSIDE` (0.5) is the share a place gets that isn't open to the sky (fields without `sky = 1` in their `_pbr.txt`, fields without PBR files, and battles started there): a room is coloured by the dusk outside only a little. `LUT_LUNAR_FROM` / `LUT_LUNAR_TO`: the story's counter between which the Lunar Cry's set is used; 0 (the default) never, because the counter's value for the Lunar Cry isn't known yet. `LUT_DEBUG=1` logs the counter whenever it changes, which is how to find it: play (or load a save) from just before the Lunar Cry and note where it goes. **Ctrl+F5** (with `DAY_NIGHT_KEYS=1`) shows the grade as the story has it, then the Lunar Cry's set, then no grade (to compare), in turn.

**Needs** this release's `main.frag`, `main_char.frag`, `demaster_field.frag` and `demaster_chara.frag`; with an older one the draws it does just aren't graded. The cost: two to four texture reads per pixel of the scene during a cross-fade, two otherwise, none by day with the day LUT here (DEMASTER sees it changes nothing and doesn't use it).

## Battles and the world map (`SCENE_LIGHT`)

Battles and the world map are drawn by the game's `main.frag` (DEMASTER's version in `DEMASTER_EXP\shaders`). `DEMASTER_EXP\scene_light.txt` (written with the defaults, explained, when it's missing; read again within a second of saving) says what the time of day does there: the night's tint and level, the sun and the moon crossing the place by the clock (the camera is read from the game every frame), and rules for some stages or some part of the story.

**A page's maps**, next to its picture, DDS or PNG:
- a battle stage page (`textures\battle.fs\hd_new\a0stgNNN_P`): `_N` (normals, green up), `_ORMH` (AO, roughness, metalness, and the height in the alpha: 1 the surface, 0 the deepest; or `_ORM` without the height), and its glows at night: `_E` (a coloured emissive: their colour on black, added) or `_EC` (a picture of its glows with their coverage);
- a world map page (v15.15: `textures\world\dat\texl\texl_NNN_P`, `wmset\wmset_NNN_P`, the ground, the far land, the sea, the moon): `_N`, `_ORMH` (or `_ORM`), and its glows at night: `_E` (a coloured emissive: their colour on black, added) or `_EC`. A palette's own (`texl_003_2_N`), else the first palette's (`texl_003_0_N`).

The normal map's slopes, the AO, a highlight by roughness and metalness, and the glows from the evening on. `parallax` (0-64, default 4) is how deep an `_ORMH` map's height goes in the place's units: lower parts shift with the view. The log says which maps each page has.

An `_E`'s alpha isn't read: its colour is only added. A picture of the glows that should cover the darkened picture where it's lit is an `_EC`, its alpha the coverage.

**Making them with fieldprep (v15.15):** `fieldprep battle` packs and converts the battle textures, for one image or the whole folder:

```
fieldprep battle "<game>\DEMASTER_EXP\textures\battle.fs\hd_new\a0stg012_16.png"   (one image; any of its maps works too)
fieldprep battle "<game>\DEMASTER_EXP\textures\battle.fs\hd_new"                    (every image there)
fieldprep battle "<game>\DEMASTER_EXP\textures\battle.fs\hd_new" --match a0stg*     (only the stages; "c0m*,mag*": enemies and GFs)
```

Every image in the folder: the stages' pages (`a0stg012_16`, an animated page's frames `a0stg012_16_0` ...) and the fighters', enemies', bosses' and GFs' textures (`d0c000_0`, `c0m001_0`, `mag001_0` ...), each with whatever maps it has. BC7 DDS files with every mip level, made as `fieldprep model` makes them (the albedo in linear light, cut-outs keeping their coverage, normals unit length, padding around the islands):

| You put next to `a0stg012_16.png` | fieldprep writes |
|---|---|
| nothing, or `a0stg012_16_A.png` (or `_AM`) | `a0stg012_16.dds`: the albedo, BC7 sRGB |
| `a0stg012_16_N.png`: normal, green up | `a0stg012_16_N.dds` |
| `a0stg012_16_O.png`, `_R.png`, `_M.png`, `_H.png` (grey; `_RC`/`_RM`, `_MC`/`_MM` work too), or one packed `a0stg012_16_ORMH.png` | `a0stg012_16_ORMH.dds`: AO, roughness, metallic, height. Missing ones are plain (AO 1, roughness 0.7, not metal, height 1: the surface, nothing moves) |
| `a0stg012_16_E.png`: the glows' colour on black | `a0stg012_16_E.dds` (added at night). An `_E.png` with an alpha of its own is a picture of the glows: it becomes `_EC.dds` |
| `a0stg012_16_EC.png` | `a0stg012_16_EC.dds`, its alpha the coverage |

- **An animated page's maps** can be one set for all its frames, named after the page (`a0stg012_17_N.png` next to `a0stg012_17_0.png`, `a0stg012_17_1.png` ...): they're made at their own size, and DEMASTER uses them for every frame without a map of its own.
- **DEMASTER reads the DDS whenever there is one** (before the PNG), so after changing a PNG run fieldprep again: only what changed is made again (`--force`: all). The log, `fieldprep_battle.txt` in the folder, says what each image got.
- `--quality`, `--flip-green`, `--coverage` and `--no-pad` as for `fieldprep model`.

**The world map's ground** is drawn with smooth normals (v15.15): DEMASTER keeps the faces around each corner from frame to frame by its place on the map, so the ground's many small draws meet without seams and each corner gets its neighbourhood's normal; a cliff's edge (faces more than 60° apart) stays sharp. `smooth = 0` in `[world]` puts the game's flat triangles back.

**The world map's lights** (v15.15): `light = x y z  r g b  range [night|day]` in `[world]` or in a rule with `where = world`, 64 at most; the eight whose reach comes nearest the camera are lit. Their places are the world map's own numbers, as the game keeps the party's (x 0-262143 and z 0-196607 across the map, which wraps round; y the height, the game's way). With `DAY_NIGHT_KEYS=1`, **Ctrl+F3** on the world map adds one 300 above where the party stands, as its own rule at the end of `scene_light.txt` (a warm light at night, reaching 2500: change it there, the game picks it up at once). With `SCENE_DEBUG=1` the log says every 10 seconds where the party is, in those numbers and on the screen: if the screen place isn't where the party is drawn, the lights won't be where you put them either (send that log).

## The look (v15.10): the maps change the painting itself

Until v15.10 a PBR field at rest was its painting, exactly: the normal, roughness and AO maps only shaped light that was *added* (lights with `intensity`, flicker, shadows, reflections, the night). That is still what you get with `FIELD_PBR_LOOK=0`, the default. The look is for when you want the maps to show.

| In `demaster.conf` | Default | What |
|---|---|---|
| `FIELD_PBR_LOOK` | 0 | The master dial. 0: off. 1: the three dials below as set. Up to 2: twice that. |
| `FIELD_PBR_RELIGHT` | 0.5 | The painted light follows the normal map: what is turned towards the lamps that are on gets brighter, what is turned away gets darker (at 1: up to about half again as bright, down to about a fifth). The field as a whole keeps its brightness. |
| `FIELD_PBR_HIGHLIGHTS` | 0.5 | The painted lamps make highlights on glossy and metal surfaces (GGX, from roughness and metallic), without their diffuse light a second time. |
| `FIELD_PBR_AO` | 0.5 | The AO map darkens the painting too (at 1: the painting times the AO map). |

Each dial's strength is its own value × `FIELD_PBR_LOOK` × the field's `look` in `_pbr.txt`.

- **Per field, in `_pbr.txt`** (fieldprep keeps them): `look = 1` is this field's share of the master dial (0: this field stays as painted; 2: twice). `look_relight`, `look_highlights`, `look_ao` are `auto` (demaster.conf's value) or a number of this field's own. DEMASTER reads `_pbr.txt` again within a second of saving it, so set `FIELD_PBR_LOOK=1` once and tune a field while you stand in it. The log says what a field ended up with: `FIELD_PBR: bdifrit1: the look: relight 0.50, highlights 0.50, AO 0.50`.
- **Which lamps count.** Relight and highlights use the lamps that are *in the painting* (`baked = 1` in `_lights.txt`: fieldprep's found lamps, your marks), as much as each is on (day and night lamps fade with the clock; a lamp its light group turned off counts for nothing), and the sun (below). A lamp you added with `baked = 0` is new light: it has its own `intensity` and `specular`. A field with no baked lamps and no sun only gets the AO dial.
- **Where it doesn't apply.** Layers without a normal map (relight), sky, glowing pixels (a bulb is a light, not a lit surface), and anything out of every lamp's `range`: so `range` is also how far a lamp's relight reaches.
- **It needs a decent albedo.** Relight multiplies the painted light, so it works on any split. Highlights take their strength from the albedo and metallic maps: on a field whose albedo is nearly the painting itself (the lightmap doing all the work the other way round, as bdifrit1's is now) they will look chalky.
- **Cost:** a few multiplications per pixel and lamp, no extra texture reads. The same at every `FIELD_PBR_QUALITY`.

In graphics terms: relight is a shading-ratio relight (the painting's own light times computed shading over a reference), highlights are specular-only lights, the AO dial is a multiply. None of it is new as a technique; what's specific here is that the reference is worked out per field and per frame so the average brightness holds (below, "How it's drawn").

## Moving lights (v15.10)

Lights that aren't at a fixed place in `_lights.txt`. Their own module: `FIELD_MOVING_LIGHTS=1` (0: none of this). To the shaders each is one more light among the field's 8, so when a field already has 8 on, the faintest of its own steps aside.

### The sun (`FIELD_SUN`, default 1.0: its strength)

On fields open to the sky a sun crosses the sky with the day/night clock: rising at the middle of dawn's fade, highest at the middle of the day, setting at the middle of dusk's fade; orange near the horizon, near white high up. At night a faint moon takes the same path.

- **Which fields:** those with `sky = 1` in `_pbr.txt`, or `[sun] on = 1` in `_lights.txt` (an open street with no sky in the picture). `on = 0` turns it off for a field.
- **What it does:** adds its light to what faces it (`intensity`), the characters cast shadows from it (long at dawn and dusk), and with the relight dial the painted light follows it from morning to evening (`shade`). With `FIELD_PBR_LOOK=0` only its added light and the shadows show.
- **A field with `time = day`** has it standing still at `fixed` (0.3: mid-morning). `time = night`: the moon at its highest. With `DAY_NIGHT=0` every field is a day field.
- **Walls keep it out** at medium and high quality (the same wall checks added lights get, looking as far as the field is wide).

```
[sun]
on = auto          ; auto: only with sky = 1 in _pbr.txt; 1: on; 0: off
azimuth = 0.0      ; degrees: where it rises. 0: on the right of the picture, setting on the left; 180: the other way;
                   ; 90: it rises behind the scene and sets behind you
height = 60.0      ; degrees above the horizon at noon (it leans towards the camera, so what faces you is lit)
intensity = 0.350  ; the light it adds on top of the painting
shade = 1.000      ; how much the painted light follows it (the relight dial)
moon = 0.250       ; the moon's share of both at night (0: no moon)
fixed = 0.300      ; where on its arc it stands with time = day (0 rising, 0.5 noon, 1 setting)
```

### The party light (`FIELD_PARTY_LIGHT`: 0 off, 1 at night, 2 always; default 0 since v15.14)

Off by default: it takes one of the 8 places, so a field that turns it on (`[party] on = 1`) has five night lights instead of six. Lights from effects (save points, draw points, spells) are planned to take over what it was for.


A warm light the party leader carries, a little above the middle of the body and towards the camera. At 1 it comes on as the room goes dark and goes out at dawn. It lights the room (normals, roughness and wall checks as for any added light) and the characters near it. `FIELD_PARTY_LIGHT_STRENGTH` scales it everywhere.

```
[party]
on = auto          ; auto: as demaster.conf says; off; night; always
color = 1.000 0.740 0.450
intensity = 1.000
range = 72.0       ; native pixels
height = 0.70      ; 0: the leader's middle, 1: the top of the head
follow = auto      ; auto: DEMASTER picks the leader; a number: that script entity
```

- **Who the leader is.** DEMASTER has no address for "the party leader", so it works it out: each script entity has a byte the script's SETPC command sets (found in FFNx's description of the entity structure). The characters whose byte differs from what most of the field's entities have are the playable ones; among those drawn, the one that moved last carries the light, and keeps it until another moves. The log says who it took: `MOVING_LIGHTS: bdifrit1: the party light's carrier is script entity 3 (...)`. **This is the least certain part of v15.10: it has not run in the game yet.** If it picks wrong on a field, `follow = <entity>` fixes that field, and the log line tells me what to correct.
- Fields without a measured camera (`camera = estimated` in `_pbr.txt`) have no script entities to go by: the character that moved last carries it.
- It fades in and out over a third of a second when its carrier appears or is hidden.

Both sections are written into new `_lights.txt` files by fieldprep and kept when it finds the lights again. A file without them (every file so far) uses the values above.

## Your keys in `_pbr.txt`

fieldprep rewrites `_pbr.txt` every run but keeps these:

| Key | What |
|---|---|
| `time = cycle/day/night` | (v15.8) `cycle` (default): the field follows the day/night clock. `day`: always as painted. `night`: always night (a story scene, a boss). In a `day` or `night` field the clock waits until you leave it (battles started from it too), then goes on. |
| `sky = 0/1` | 1: pixels at the farthest depth of your depth map are sky (no added light, no shadows, nothing reflected, characters never hidden by them). For a sky painted into the same layer as the foreground. Layers that scroll on their own (parallax, script scrolls) are found automatically and treated as sky either way. |
| `character_shade = replace/keep` | The game darkens characters by one value per field (for example −0.094 in bvtr_1) and scripts change it (flashes, silhouettes, fades). `replace` (default): DEMASTER notes each character's value once it has settled after the field loads; the room's light replaces that value, and later changes from scripts are added on top the way the game adds them. `keep`: the game's darkening on top of the room's light (for a field that sets its look after the fade-in). |
| `look = <number>` | (v15.10) This field's share of `FIELD_PBR_LOOK`: 1 all of it (default), 0 the field stays as painted, 2 twice. |
| `look_relight`, `look_highlights`, `look_ao` = `auto` / `<number>` | (v15.10) `auto` (default): demaster.conf's dial. A number: this field's own value for that dial (still × `FIELD_PBR_LOOK` × `look`). |
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

**Checking the look and the moving lights.** `FIELD_PBR_VIEW=9` shows only what is added (the highlights dial, the sun's and the party light's own light); `FIELD_PBR_VIEW=12` the shadows, the sun's among them. For relight and AO compare with `look = 0` in the field's `_pbr.txt` (saved while you stand there: the picture switches within a second). The day/night test keys move the sun: Ctrl+F9 for 10× or 60× sweeps it across the sky.

`FIELD_PBR_VIEW` shows one thing at a time: on PBR layers 1 albedo, 2 lightmap, 3 normals, 4 depth, 5 AO, 6 roughness, 7 metallic, 8 flicker mask, 9 only the added light, 10 with every light at rest, **12 shadows** (blue: sky), **13 reflections** (×4), **14 how far each pixel is behind its tile's nearest part** (black: at the tile's own depth, nothing gets through; white: twice as far or more; orange: a tile without a measured nearest part; blue: sky); **16 the glows as they'll show at night** (the emissive map × the painting's colour there, with `[emissive]`'s hue, tint and strength; black: no glow), **17 the emissive map**; on characters **11 the room's light on grey** and **15 their normals**. 0 is normal.

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

**BC7 straight to the GPU (v15.14, `FIELD_MODEL_BC7=1`).** When every image of a model's atlas has a BC7 `.dds` of its size, and each one's place in the atlas lines up with BC7's 4 × 4 blocks, the atlas itself is BC7 on the GPU: the files' blocks are copied in as they are (nothing decoded, a quarter of the video memory). Its levels go as far as every image's line up and the images have them. The `_N` and `_ORME` map atlases do the same when every image has that map as a BC7 `.dds`. One PNG (or another format) in the atlas and it is decoded and RGBA, as before; the log says which atlases are BC7 (`FIELD_DEBUG=1`). Everything else already went to the GPU as BC7: field layers (block by block into the pages), battle stage pages, battle characters and monsters, and the world map's terrain. The world map's vehicles and its other named textures (`WORLD_VEHICLES`) are still decoded: they are fitted onto the game's pages by resizing on the GPU, which BC7 can't be.

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
- **Island padding (v15.15):** a model's texture is islands (the parts its triangles use) on a background nothing uses. The small levels, and the graphics card's filtering at a cut-out's edge, mix texels across an island's edge, so a black background creeps onto the model as dark seams and dark fringes on hair, more the smaller the model is on screen. Now everything outside the islands takes its island's edge (colour, normal, ORME), spread outwards until it's all filled, in every level. Where the islands are comes from the albedo's alpha (the `_A`, else `d001_0.png`): texels with alpha 0. A texture with no see-through texels anywhere can't say where its islands are, so it isn't padded (the log says so). The see-through texels stay see-through: only what's under them changes. `--no-pad` turns it off. DDS files made before this are "up to date" until their PNGs change: run once with `--force` to pad them.
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
2. **Painted light.** albedo × lightmap × (1 + mask × (flicker − 1)), where the flicker comes from the nearest light that flickers (or follows a group), is on and reaches the pixel (v15.14). Characters' shadows take away the share of a lamp's painted light you set with `shadow`; contact darkening around their bodies.
3. **Added light.** Each light's intensity: Lambert diffuse plus GGX specular (Smith-Schlick geometry, Schlick Fresnel, metallic workflow), from the normal, roughness and metallic, with a smooth range falloff, blocked by characters (shadow maps) and by the room (stepping across the depth map). AO only dims this added light, never the painted one. What's added (and the reflections) **rolls off towards white** instead of being cut off at it: with P the painted pixel and u the light added, the pixel is P + (1 − P)(1 − e^(−u / (1 − P))) per colour channel. A little added light counts in full; the more there is, the less each bit adds, so a light turned up high keeps its shape and its colour instead of burning a flat white hole (in graphics terms: a soft shoulder, tone mapping only the added part). With nothing added it's exactly P. Characters' added lights and their models' own glow roll off the same way over the room's light.
4. **Reflections.** The reflected ray meets the characters' cards (their pictures stood up where they are), blurred by roughness.
5. **Depth.** The tile's depth goes to the depth buffer exactly as the game sends it (the GPU can skip hidden pixels before lighting them). With `FIELD_PBR_DEPTH`, a pixel with a character between the tile and what it shows isn't drawn, so the character shows.
6. **Glows.** A cell with a picture of its glows (`_EC`, flag 16 in the cell table): painted × (1 − coverage × showing) + the picture's colour × tint × strength × showing, where showing is how night it is, or `[emissive] day` if that is more. Otherwise the old mask: the emissive map (`_ORME`'s alpha) says which pixels are the painting's own glow: a bulb, a screen, a sign. By day nothing changes (the painting has them already). As the room goes dark at night, they stay lit: the painting's own colour there, turned and tinted by `[emissive]` in `_lights.txt`, takes the dimmed painting's place by as much as the emissive map says (mix(dimmed painting, glow, emissive × night)). The game's additive glow layers are drawn as painted either way.
7. **The look** (`FIELD_PBR_LOOK`). In the same pass over the lights as steps 2 and 3:
   - *Relight:* for every lamp that reaches the pixel, its weight w = (how much of it is on) × the range falloff, and how the pixel's normal faces it, s = clamp((n·l + 0.5) / 1.5, 0, 1): 0 turned away, 2/3 half turned, 1 facing it. With W = Σw and S = Σw·s the painted light is multiplied by 1 + relight × ((0.25 + scale × S) / (0.25 + W) − 1). The 0.25 is the share of the painted light no lamp moves, so out of every lamp's range nothing changes. `scale` is worked out by DEMASTER every frame from a 32 × 24 grid of the room's normals and depths (`fieldpbr::RelightScale`) so that the average over the field is exactly 1: the field keeps its brightness whatever the lamps and the sun are doing. A moving light's share is also kept out by the room's walls.
   - *Highlights:* step 3's GGX term alone, with the painted lamp's own colour instead of colour × intensity.
   - *AO:* the painted light × (1 − ao dial × (1 − AO)).
   - Glowing pixels are left out of all three by as much as they glow.
8. **Night.** The painted light (step 2) times `[night] level` × `tint` away from the night's lamps, kept around them (`lamps`, within their range); how much of that applies follows the clock (0 by day, 1 at night, smooth through the fades).

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
- **v15.10:**
  - The look and the moving lights (`tests/field_pbr_test.cpp`, 173 more checks): the dials times the master dial and the field's own; `_pbr.txt` and `_lights.txt` round trips with the new keys and sections, old files reading as the defaults; the relight's scale makes the samples' average exactly 1; the sun's path (rises on the right at azimuth 0, never below the horizon, the right height at noon, also with a tilted camera), its colour, its falloff flat to within 4% across a field, off below the horizon, fixed in `time = day` fields, the moon; the leader choice (the playable one over a walking NPC, the last one to move of two, `follow`, no entity data); the party light's place, modes and strength.
  - `tests/pbr_shader_test.cpp` on a field with an AO and emissive map: every dial at 0 with a painted lamp in the field: 0 values differ from the painting. Relight at 1 and at 0.5 against the same sums worked out on the CPU from the field's `_ND`: mean 0.24 and 0.21 levels off, the field's brightness × 1.000. AO at 1: the painting × the AO map within 1 level. Highlights: 109,234 pixels brighter, none darker, none outside the lamp's range. The sun with relight: morning and evening both × 1.000 of the painting's brightness, and the picture changes between them; its own light only adds; wall checks never brighten. The party light: +3.5 levels around its carrier, nothing far from it, nothing darker.
  - Timing (`BENCH=10`, software rendering, 1344×960, so only the ratios mean anything, and run to run they move by about 8%): the field program with 3 lights and no wall checks takes 79 to 84 ms against v15.9's 75 to 79, which is within that noise (v15.9's three per-light loops are one now, and the roll-off costs a little where light is added). The look on top: no difference that shows above the noise. Sun and party light on top of that: about +30% at low quality and +50% at medium and high (each is one more light with its wall checks, and the sun reaches every pixel).
  - `tools/syntax_check.sh` (new): MinGW's compiler reads 22 of the DLL's 35 source files, the ones that only build on Windows among them (the PBR, character, moving lights, day/night and config code), and finds no errors. Not Microsoft's compiler, and the files with inline assembly can't be read this way, but the new DLL-side code has been through a compiler's type checking now.
  - **Not run anywhere:** the DLL itself. The look's default strengths and the sun's are first guesses that have not been seen on a real field; the party leader choice is untested against the game (above).
  - `tests/pbr_shader_test.cpp`: the roll-off: from the picture at rest and the picture with a test light, the picture with that light 4 times as strong is what the curve gives (417,105 values, mean 0.56 levels off, max 3), nothing gets darker, and of the 98,960 values a cut-off would have drawn white 11,007 are; the glows at night: on a field with an emissive map, mix(painting × level × tint, glow, emissive) within 1 level, as painted and with a tint and half strength, and nothing brighter than painted. Both checks fail on the v15.9 shader.
  - bx and bimg from `ff8_demaster\vendor\src` (the Release build's): compiled for 32-bit Windows with MinGW and linked against a program making DEMASTER's calls; built natively they read fieldprep's BC7 DDS files (with and without mip levels) and PNGs. The Release configuration itself has not been through Microsoft's compiler yet.
  - Not run here: the clock, the keys, the bar and the file in the game.
- **The shared code:** `tests/field_pbr_test.cpp` (17,692 checks: names, settings (with the canvas), lights, the `[emissive]` settings, the emissive map from a residual (a bulb glows, a lit wall and a dark corner don't), groups, flicker, camera, depth fit, the light grid file, packing, BC7/DDS, PNG; inverse lighting on a box room lit by two large lamps and a sconce: the two large ones found within a tenth of their distance with their colours, the sconce left out; your green mark added, nothing on your red one; the night's two lamps found in a _RESM-like picture; your own lights kept on relight; the 2 + 6 per time of day) and `tests/field_chars_test.cpp` (back to view space, smooth normals, round shadows, telling characters apart, facing).
- **Not run here: the DLL in the game.** Characters drawn by DEMASTER, the back faces, shadows, reflections, light groups and the model DDS swap all need a test in the game. Use `FIELD_DEBUG=1`, a frame log (Ctrl+F11: the log now notes where DEMASTER drew the characters and which round shadows it left out) and send `demasterlog.txt`.

## Not covered yet

- **Moving lights that the game moves.** The sun and the party light are the first two. Spells, summons, a train's headlights, lightning: each needs its own source of position (a script entity, a particle), which the module's interface takes (`fieldpbr::MovingLight`), but nothing feeds it yet.
- **The sun on characters' light grid.** Characters get the sun's added light and cast its shadows, but the room light they stand in (the light grid fieldprep bakes) doesn't swing with the relight dial.

- **Night on the rest of the game.** Non-PBR fields and layers without PBR maps stay as painted at night (the clock still runs through them).

- Particles (fire, sparks, save point glow) as light sources, and status effects: with the battle magic module.
- Language-specific maps (`<field>_<lang>_AM.png`): handled per `.map` like the layers, but not tested on a real multi-language field yet. Fields that only have per-language `.map` files (bvtr_3) work with images named without a language: fieldprep uses the English `.map` for them (the report says so), and DEMASTER finds the plain-named files for every language.
- Light groups in the Japanese game.
