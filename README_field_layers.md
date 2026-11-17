# DEMASTER field layers (v13)

Field backgrounds load straight from Field-Map-Editor's deswizzled layers. No swizzled pages to make.

**New in v13: PBR lighting for these layers** (`FIELD_PBR`, and the `fieldprep.exe` tool that prepares it). See `README_field_pbr.md`. Fields without PBR files load exactly as in v12.

- **Pages built in game.** When the game builds a field page, DEMASTER fills it from your layer images, using the field's own `.map`.
- **Everything the game does per tile stays.** Each tile keeps its own depth, layer, blend mode and animation state. Only where its pixels come from changes.
- **Shared tiles get split.** In the original data, tiles that look alike share one 16×16 cell of a texture page. Each tile that has its own image now gets a free cell (the `.map` is changed in game memory only), so every tile can look different.
- **PNG or DDS.** Both work. When a layer has both, the newer file is used: edit the PNG, then make the DDS when you're happy with it.
- **Live editing** (`FIELD_LAYERS_LIVE`). Save a layer image while you're standing in the field and it shows up within a second.
- **BC7 goes straight to the GPU.** BC7 `.dds` layers at 4x or 8x are copied block by block into the game's page: nothing is decoded or re-encoded. PNG is put together on the CPU (a little slower to load, same result).
- **Smooth edges, no seams** (`FIELD_SMOOTH`). The page is filtered smoothly, but each tile only ever reads its own cell, so nothing bleeds in from the tiles around it. This needs the `main.frag` in this zip.
- **Sharper character cut-outs** (`FIELD_BG_RESOLUTION`). Remastered draws field backgrounds into their own buffer, and characters are cut by the background at that buffer's resolution (the blocky edge when they walk behind something). 4 = 1280×960 (DEMASTER's), 6 = 1920×1440, 8 = 2560×1920.

## PuPu's images (v15.8)

DEMASTER and fieldprep read layer images from PuPu as well as from FME, in the size each tool makes them.

- **Names.** The two tools name some layers differently. In bdifrit1, FME's `00CFF000`, `00C00000`, `00901000` and `01902000` are PuPu's `004FF000`, `00400000`, `00101000` and `01102000` (`02103000` is the same in both).
  - PuPu puts the raw blend number in the name.
  - FME adds flags for tiles that sit off the 16-pixel grid.
  - Where PuPu finds two tiles on the same spot, it numbers the second one +1 (`004FF001`, `004FF002`).
- **Sizes.**
  - FME makes every image the size of the whole field's render area: 400×248 for bdifrit1 (1600×992 at 4x).
  - PuPu makes each layer only as big as the box around its tiles and the field's centre (0,0). In bdifrit1, the main and lava layers are 336×240 (1344×960 at 4x); `01102000` and `02103000` are 368×144 (1472×576).
- **Which file a tile uses.** For each tile, DEMASTER looks for the PuPu-named file at PuPu's size, then the FME-named file at FME's size. The image's size has to be one of those, times one whole-number scale. A file whose size fits neither gets a log line and isn't used (its tiles keep the game's pixels).
- **Mixing works.** One folder can have some layers from PuPu and some from FME. If both tools' files exist for a layer, PuPu's is used.
- **The canvas.** The field's own maps (the whole-field painting, `_N`, `_DEPTH`, `_ORME`, the lights and the camera) are on one canvas:
  - FME's render area when all the images are FME's;
  - otherwise, the box around the opaque layers' pictures (bdifrit1 from PuPu: 336×240, the main layer's size).
  - fieldprep writes it into `_pbr.txt` (`canvas = left top width height`) and DEMASTER reads it from there.
  - See `README_field_pbr.md`.

## Smooth scroll and in-between pictures (v15.11, not yet run in the game)

Two things that only fields drawn from layer images can have, because the layer images' cells tell DEMASTER which layer every tile the game draws belongs to. Both are on by default in `demaster.conf` and each has its own switch.

**`FIELD_SMOOTH_SCROLL`.** The game scrolls a field in whole PlayStation pixels (8 screen pixels at 4x on a 1440p screen), and works out each layer's place from the scroll with whole-number arithmetic. So a slow scroll steps, and layers that move at another speed than the main one (parallax: a sky, a far wall) step at other moments than it does, which reads as shimmer between them. DEMASTER reads each layer's place off the tiles as the game draws them, turns the staircase of places back into the line it was rounded from, and draws the layer there. The characters get the main layer's difference, so they stay on the floor. Nothing is ever drawn more than 3/4 of a PlayStation pixel from where the game has it, and when the screen stands still the picture is exactly the game's.

**`FIELD_INTERPOLATION`.** The game makes 30 pictures a second in fields. With this on, DEMASTER shows a picture of its own before each of them: every layer halfway along its scroll, every character halfway between its last pose and this one. The game keeps running at its own speed (nothing in its logic is sped up, unlike a 60 FPS patch), and what you see is 60 pictures a second. The price: everything in a field is drawn twice (about double the graphics card's work there, and more video memory), and the picture is half a frame later than without. `FIELD_INTERPOLATION=0` turns it off; DEMASTER also turns it off by itself for a field where the game runs slower with it than without, and says so in the log.

What to look at first in the game, and what the log (`FIELD_DEBUG` not needed) says:

- `FIELD_INTERPOLATION: in-between pictures here (the game shows 30.0 pictures a second, with vertical sync)` once per field: it is running. If it says another rate instead ("the game shows 60.0 pictures a second here, not about 30"), Remastered shows fields differently from what this was written for, and nothing is interpolated.
- `...the game slowed down here...`: the half-frame wait is not coming out of the game's own idle time on this machine. Send the log.
- Flicker at 30 times a second anywhere in the picture: something the game draws is missing from the in-between picture. A frame log (`FIELD_FRAME_LOG=1`, Ctrl+F11) of that field shows what.
- Known not to be in-between: the round shadows under characters (and FIELD_PBR's shadows and reflections), and effects that are neither background tiles nor characters. They are the frame's own in both pictures, so against a moving character they are half a step ahead every other picture.

**`ANALOG_MOVEMENT`** is not about layers but arrived with these: in any field, the left stick of an XInput pad walks at any angle (`ANALOG_DEADZONE` sets how far it moves before it counts). The d-pad and keyboard are untouched. Not on the world map yet.

## What changed in v12

- **The game says which palette went into each slot.** In bvtr_1, two of page 0's slots (the animated scenery's palettes 1 and 5) matched neither the `.mim` nor live VRAM: the game builds those textures from palette colours it read earlier, and the field has changed them since. DEMASTER now watches the game hand each palette to the driver (`read_vram_palette` and `write_palette_to_driver`, the same two functions FFNx uses on the PC version, found in both the EFIGS and JP DLLs) and records which CLUT went into which slot of which texture. When the pixels match nothing, that record decides. When the pixels match, the log says whether the game agrees.
- **No bit-depth hint.** The texture header field v11 read says 8-bit for every page, so it's gone.
- **The shader warning says which shader it is:** our `main.frag`, an older `main.frag` without the change, or another shader altogether.

## What changed in v11

- **Palettes are matched against the game's live VRAM.** v10 compared the game's pages with the `.mim` as it ships. In bvtr_1, three of page 0's palette slots only matched 62-84%: by then the field had changed those colours (or pixels) in VRAM, so v10 left those pages as the game made them. And because tiles on those pages had been moved to cells of their own, they showed whatever was in their new cells: the scrambled look. DEMASTER now reads the game's PlayStation VRAM (found in both the EFIGS and JP DLLs) at the moment the page is built, so it compares against exactly what the game converted.
- **The game's bit depth is used.** The game numbers palette slots separately for 4-bit and 8-bit pages (page 3 has a 4-bit slot 0 and an 8-bit slot 0), and its texture header says which one it's building (the log shows it next to each slot). v10 mixed the two up.
- **No more guessing from a slot's previous palette.** v10 fell back to "whatever this slot was before" when nothing matched, which picks wrong when the game reuses a slot. Now: a clean match, else a clearly best partial match, else the page is left as the game makes it.
- **Semi-transparent tiles** (blend 0-3, like the shadow overlay) take the game's per-texel alpha, so they blend where the original blends.

## What changed in v10

- **Palette numbering.** The game doesn't number a page's palettes the way the `.map` does. It hands out palette slots on the fly, in the order it first needs them, so DEMASTER matches each page the game uploads to the right `.map` palette by comparing pixels.
- **`FIELD_BG_RESOLUTION`** is 4, 6 or 8. The screen-height mode is gone.
- **PNG/DDS newest-wins rule and live reload** (above).

## Setup

1. **Build.** Unzip into a new, empty folder and run `build_demaster.bat` (see `HOW_TO_BUILD.txt`).
2. **Shader.** Copy `DEMASTER_EXP\shaders\main.frag` into the game's `DEMASTER_EXP\shaders\`. If you've edited your own `main.frag`, merge it: the change is the `demasterTileClamp` uniform, the `demasterTileTexture` function, and the first line of `main()`.
3. **`demaster.conf`** (the one in this zip has these set):
   ```
   BATTLE_HOOK_MONSTER_FIELDS=1   ; needed: the page upload hook is shared with world and battle
   FIELD_LAYERS=1
   FIELD_LAYERS_LIVE=1
   FIELD_LAYERS_UNSHARE=1
   FIELD_SMOOTH=1
   FIELD_BG_RESOLUTION=4          ; 4, 6 or 8
   FIELD_DEBUG=1                  ; 2 = also save every built page as a PNG
   ```
4. **Layers.**
   - In Field-Map-Editor, use Batch → Deswizzle. You get one image per PupuID.
   - Upscale those images at one scale for the whole field.
   - Or export them from PuPu (keep its names and sizes; see "PuPu's images" above).
   - Put them in `DEMASTER_EXP\textures\field_bg\<first two letters>\<field>\`, named the way FME (or PuPu) names them:
     ```
     DEMASTER_EXP\textures\field_bg\bv\bvtr_1\bvtr_1_00400000.png
     DEMASTER_EXP\textures\field_bg\bv\bvtr_1\bvtr_1_004FF000.png
     ...
     ```
     For a field with a `.map` per language: `<field>_<language>_<pupu>` (`bghall_1_fr_004FF000.png`). The plain name is used when there is no language file.
   - Your bvtr_1 set matches: all 22 names and the 1344×960 size (the field's 336×240 canvas at 4x) check out against `bvtr_1.map`.
5. **BC7, when you're done editing.** For example, with DirectXTex's texconv:
   ```
   texconv -f BC7_UNORM -m 1 -y -o <folder> <folder>\*.png
   ```
   - The DDS files come out newer than the PNGs, so they're the ones used. Touch a PNG again and it takes over.
   - Use 4x or 8x for the GPU path. At 6x a tile at an odd pixel position can't be copied as BC7 blocks; that page is put together on the CPU instead (it still works).

## Live editing

- Edit and save a layer image while the game is in that field. Within a second, the pages that use it are rebuilt.
- Works for PNG and DDS. A file that's still being written is tried again a second later.
- A layer image that didn't exist when you entered the field is picked up the next time you enter it (the tiles are assigned to images when the field loads).
- `FIELD_LAYERS_LIVE=0` turns the checking off.

## Fields you've only partly converted

- **Your old pages still work.** A field that has layer images no longer loads its swizzled pages as a whole. Each tile without an image takes its cell from the swizzled page (`<field>_<page>_<palette>`, as before, with the game's palette numbers) if one exists, else from the game's own page. So you can move a field over a few layers at a time.
- **Fields with no layers** load exactly as before.

## Checking it

With `FIELD_DEBUG=1`, `demasterlog.txt` gets a summary for every field with a folder, then lines for each page the game asks for. For bvtr_1 the first line will read as below; the palette numbers in the other two are only an example:
```
FIELD_LAYERS: bvtr_1: 1343 tiles, canvas 336x240 (FME's), 22 images (0 named and framed the PuPu way, 22 FME's), 1343 tiles drawn from them, 436 given their own cell, 0 sharing, 0 without a free cell, 15 pages
FIELD_LAYERS: bvtr_1 page 0 palette slot 8 is .map palette 3 (4-bit): 100.0% of 60416 texels match in VRAM, next best 71.5%; the game loaded CLUT 0,243 into it, same palette
FIELD_LAYERS: bvtr_1 page 0 palette slot 8 (.map palette 3): 135 cells from images, 1024x1024 at 4x, put together on the CPU (not all images are BC7)
```
Lines worth looking for if something's wrong:
- **"canvas WxH (from _pbr.txt)"**: the canvas fieldprep worked on. "(FME's)" means there's no `_pbr.txt` with a canvas (or PBR is off), so FME's render area is used.
- **"<file> is WxH, not ... times a whole number: not used"**: the image is neither PuPu's size nor FME's for that layer (at any whole scale). Its tiles keep the game's pixels.
- **"layer image(s) not found (FME's name/PuPu's ...)"**: each missing layer is listed by both names (one name when the two tools agree).
- **"no palette the .map uses on that page matches"**: that page is left as the game makes it. Send me the log line (it has the best scores, from VRAM and from the `.mim`).
- **"...match from the game's palette write"**: the pixels match no palette (the field changed its colours); the game's own record of what it loaded decided.
- **"THE GAME LOADED ... INTO IT"**: the pixels and the game's record disagree. The pixels win, but send me that line.
- **"...partly, among palettes no other slot has"**: no exact match and no record from the game; one palette was clearly closest.
- **"semi-transparent tiles, the game's texels there: ... blended"**: how many texels of the semi-transparent tiles the game blends. When there are any, your images get the same alpha there.
- **"neither the game's VRAM nor the field's .mim can be read"**: palettes can't be matched at all; the field is left as the game makes it.
- **"...drawn with shader program N, which has no demasterTileClamp (...)"**: smoothing is off. The part in brackets says whether that's an old `main.frag` (copy the one from this zip into `DEMASTER_EXP\shaders\`) or another shader.
- **`FIELD_DEBUG=2`** saves every built page to `DEMASTER_EXP\field_dump\` (`<field>_<page>_<.map palette>.png`).

## Not covered yet

- **Palette colour changes.** Scripts that change a field's colours at runtime (flicker, fades through the palette) aren't reflected in images from layers: each animation state shows its image as you made it.
- **FME "Full Filename" images** (several PupuIDs combined through `deswizzle.toml`) aren't read, only one image per PupuID.
- **Old-format fields** (the few with a 401408-byte `.mim`) are left as the game makes them.

## In this zip (v12 parts)

- The whole DEMASTER project, ready to build.
- New since v10: live VRAM matching and the game's palette writes (`ReadLiveVram`, `ReadVramPaletteDetour`, `WritePaletteDetour`, `IdentifyPalette` in `texturepatch_v2_fieldLayers.cpp`; `PSX_VRAM`, `READ_VRAM_PALETTE`, `WRITE_PALETTE` for EFIGS and JP in `mymemory.h`), `InheritSemiTransparency` in `field_layers.cpp`.
- `tests\field_layers_test.cpp`: PupuIDs and canvas against OpenVIII's own code (300 random maps), splitting shared cells, building pages, palette matching (also run on every page and palette of `bvtr_1.map`) and semi-transparency.
- `tools\verify_addresses.py`: 93 checks, 0 mismatches on both game DLLs (it also checks where the VRAM is and the two palette functions).
