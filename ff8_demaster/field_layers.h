#pragma once
//Field backgrounds from Field-Map-Editor's deswizzled layers (FIELD_LAYERS). This part has no game or Windows code, so
//tests/field_layers_test.cpp builds it on its own.
//
//A field background is a list of 16x16 tiles (the .map): where each tile goes on screen (x, y), its depth (z), its
//layer, blend mode and animation, and which 16x16 cell of which texture page and palette its pixels come from. The
//pages are "swizzled": cells are packed wherever there was room, and tiles that look the same share one cell.
//
//Field-Map-Editor (FME) "deswizzles" a field into one full-size image per PupuID (layer, blend, animation group and
//state, off-grid flags and an overlap counter). PuPu (Omzy's PuPu 2.1, rewritten as PuPuSharp) names them a little
//differently and gives every layer number its own, smaller frame. Here both tools' tile-to-image rules are computed from
//the .map, so the game can be fed either tool's images directly (each file is placed by the frame its size matches):
//	1. tiles that share a cell but have their own image get a free cell of their own (the .map in game memory is
//	   changed), so every tile can look different
//	2. when the game makes a page texture, every cell is filled with its tile's part of its image
//The game still draws tile by tile with each tile's own depth, blend and animation state; only where the pixels come
//from changes.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace fieldlayers
{
	constexpr int TILE_SIZE = 16;
	constexpr size_t TILE_BYTES = 16;

	//One tile of a field .map (the 16-byte format of every retail field; OpenVIII Tile1, deling BackgroundFile)
	struct Tile
	{
		int16_t x = 0, y = 0;
		uint16_t z = 0;
		uint16_t texId = 0; //bits 0-3 texture page, 4 draw, 5-6 blend, 7-8 depth (0: 4-bit, 1: 8-bit, 2: 16-bit)
		uint16_t palId = 0; //bits 6-9 palette
		uint8_t srcX = 0, srcY = 0; //cell, in texels of the texture page
		uint8_t layer = 0; //layer id << 1
		uint8_t blend = 4; //0 half-add, 1 add, 2 subtract, 3 quarter-add, 4 none
		uint8_t animId = 0xFF, animState = 0; //0xFF: not animated

		int TexturePage() const { return texId & 0xF; }
		int Depth() const { return (texId >> 7) & 3; }
		int Palette() const { return (palId >> 6) & 0xF; }
		int LayerId() const { return layer >> 1; }
	};

	//Tiles up to the end marker (x == 0x7FFF) or the end of the data; tile i is at byte i * 16
	std::vector<Tile> ParseMap(const uint8_t* data, size_t size);
	//Writes a tile's texture page, palette and cell into its 16 bytes (everything else is kept)
	void StoreSource(uint8_t* tileBytes, int texturePage, int palette, int srcX, int srcY);

	//The part of the field a picture covers: a rectangle in .map coordinates (native pixels; 0, 0 is the camera's axis)
	struct Frame
	{
		int left = 0, top = 0, width = 0, height = 0;
		bool operator==(const Frame& o) const { return left == o.left && top == o.top && width == o.width && height == o.height; }
		bool operator!=(const Frame& o) const { return !(*this == o); }
		bool Empty() const { return width <= 0 || height <= 0; }
		//the scale of a file this big (the frame times a whole number both ways, 1-16); 0 when it isn't
		int ScaleOf(int fileWidth, int fileHeight) const;
		Frame Union(const Frame& o) const;
	};

	//Where every tile is: on the canvas (the frame the field's own maps, lights and camera use) and in its picture.
	//ComputeLayout: FME's deswizzled images, same rules as OpenVIII (Map::shift_to_origin, Map::canvas, PupuID,
	//UniquifyPupu): one canvas, every picture covers all of it. FramedLayout: whichever pictures were found (FindPictures).
	struct Layout
	{
		int shiftX = 0, shiftY = 0; //added to every tile's x, y: its place on the canvas (= -canvas.left, -canvas.top)
		int width = 0, height = 0; //the canvas in native pixels
		std::vector<uint32_t> pupu; //per tile: the name of its picture (FME's PupuID, or PuPu's)
		std::vector<int> x, y; //per tile: position on the canvas, native pixels
		Frame canvas; //the canvas as a frame
		Frame fme; //FME's canvas (the same as canvas in ComputeLayout)
		std::unordered_map<uint32_t, Frame> frames; //per picture name, when it isn't the canvas: what its file covers
	};
	Layout ComputeLayout(const std::vector<Tile>& tiles);
	std::string PupuHex(uint32_t pupu); //8 upper-case hex digits, as in FME's and PuPu's file names
	uint32_t BasePupu(const Tile& tile, int x, int y); //PupuID without the overlap counter (x, y: shifted)
	//A file of width x height used for the tiles of picture `name` (the painting, or a map made for it): the frame it
	//covers and its scale. Tried in order: the picture's own frame, the canvas, FME's canvas; the first its size matches.
	//0 when it matches none. (A frame the same size as another one it lies in is the same rectangle.)
	int FitPicture(const Layout& layout, uint32_t name, int width, int height, Frame& frame);

	//--- PuPu's names and frames (Omzy's PuPu 2.1 and PuPuSharp, read from their code)
	//
	//A tile's name: layer << 24 | blend << 20 | animation id << 12 | animation state << 4, with the .map's blend byte as it
	//is and no off-grid flags; a later tile at exactly the same x, y with the same name (all but the last 4 bits) gets the
	//earlier one's + 1. Every picture of one layer number (the name's first two hex digits) has that layer's frame: the box
	//around all of the layer's tiles and the camera's axis (0, 0), right/bottom edge + 16.
	struct PupuNames
	{
		std::vector<uint32_t> name; //per tile
		std::map<int, Frame> frame; //per layer number (name >> 24)
		Frame FrameOf(uint32_t name) const;
	};
	PupuNames ComputePupuNames(const std::vector<Tile>& tiles);

	//--- Which file each tile's pixels come from

	struct Picture
	{
		uint32_t name = 0; //the hex in the file name
		Frame frame; //what the file covers
		int scale = 0; //file pixels per native pixel
		bool pupu = false; //named and framed the PuPu way (else FME's)
		size_t tiles = 0; //tiles drawn from it
	};
	//size(name, width, height): the size of the field's file with that name; false when there is none
	using PictureSize = std::function<bool(uint32_t name, int& width, int& height)>;
	struct PictureSet
	{
		std::map<uint32_t, Picture> pictures; //by name, the ones some tile is drawn from
		std::vector<int64_t> tile; //per tile: the name of its picture, -1 none
		//files there are but no tile could use: name -> "WxH, not ..." (for the report)
		std::map<uint32_t, std::string> unfit;
	};
	//Per tile: its PuPu-named file in PuPu's frame for it, else its FME-named file on FME's canvas (a name both tools give
	//is tried in both frames: the file's size decides). One scale per field is not checked here (Picture::scale).
	PictureSet FindPictures(const std::vector<Tile>& tiles, const Layout& fme, const PupuNames& pupu, const PictureSize& size);
	//The canvas the field's own maps, lights and camera use: the box around the pictures of its opaque tiles (blend 4).
	//All FME's: FME's canvas. No opaque tile with a picture: FME's canvas.
	Frame FieldFrame(const std::vector<Tile>& tiles, const PictureSet& set, const Frame& fme);
	//The layout on that canvas: pupu per tile its picture's name (FME's PupuID for a tile without one), frames per picture
	Layout FramedLayout(const std::vector<Tile>& tiles, const Layout& fme, const PictureSet& set, const Frame& canvas);

	//A tile given a cell of its own
	struct Relocation
	{
		size_t tile = 0;
		int texturePage = 0, palette = 0, srcX = 0, srcY = 0;
	};
	struct UnshareResult
	{
		std::vector<Relocation> moved;
		size_t shared = 0; //tiles left sharing a cell (no image of their own: they show the first tile's)
		size_t unresolved = 0; //tiles with an image that found no free cell (they show the first tile's)
	};
	//Tiles are taken in .map order; the first user of a cell keeps it. Free cells are looked for in the same page and
	//palette, then the other palettes this field uses on that page, then the other pages it uses with the same depth.
	UnshareResult Unshare(const std::vector<Tile>& tiles, const std::vector<bool>& hasImage);
	void ApplyRelocations(std::vector<Tile>& tiles, const std::vector<Relocation>& moved);

	//Per game page texture (texture page + palette): which cells come from which image
	struct CellStamp
	{
		size_t tile = 0;
		uint32_t pupu = 0;
		int layerX = 0, layerY = 0; //tile position on the canvas, native pixels
		int mapX = 0, mapY = 0; //tile position in the .map: in a file covering frame f it is at mapX - f.left, mapY - f.top
		int cellX = 0, cellY = 0; //cell on the page, texels
		bool semi = false; //drawn semi-transparently (blend 0-3)
		int originX = -1, originY = -1; //the cell the .map gave it, when that is on this page with this palette (else -1)
		int z = 0; //the tile's .map Z (draw depth)
		int layerId = 0; //the tile's layer (the background moves per layer: parallax, scrolls)
	};
	struct PagePlan
	{
		int texturePage = 0, palette = 0, depth = 0;
		bool mixedDepth = false;
		std::vector<CellStamp> stamps; //first tile per cell
		bool complete = true; //every cell a tile uses on this page has a stamp (nothing of the game's page is needed)
		bool aligned = true; //every cell starts on the 16-texel grid
	};
	uint32_t PageKey(int texturePage, int palette);
	//tiles: after Unshare; shipped (optional): the same tiles before, for CellStamp::origin
	std::map<uint32_t, PagePlan> PlanPages(const std::vector<Tile>& tiles, const Layout& layout, const std::vector<bool>& hasImage,
		const std::vector<Tile>* shipped = nullptr);

	//CPU composition (used when the GPU copy cannot be used, and by the tests)
	struct Image
	{
		int width = 0, height = 0;
		std::vector<uint8_t> rgba;
		bool IsValid() const { return width > 0 && height > 0 && rgba.size() >= static_cast<size_t>(width) * height * 4; }
	};
	struct StampSource
	{
		const Image* image = nullptr;
		int scale = 1; //image pixels per native pixel
		int layerX = 0, layerY = 0, cellX = 0, cellY = 0; //layerX/Y: the tile's place in this image, native pixels
		bool coverage = true; //alpha is coverage (resampling weights colour by it); false for data in alpha (PBR maps)
	};
	//The page at (logicalWidth x logicalHeight texels) * scale. base: what is under the stamps (the game's page in BGRA,
	//or a swizzled HD page in RGBA; any size, scaled to fit), or null for transparent. Every stamp replaces its cell with
	//its tile's part of its image.
	Image ComposePage(int logicalWidth, int logicalHeight, int scale, const uint8_t* base, int baseWidth, int baseHeight,
		bool baseIsBgra, const std::vector<StampSource>& stamps);
	//Tiles drawn semi-transparently blend only where the game's page says so: the PlayStation blends the texels whose colour
	//has the semi-transparency bit, and the game's page carries that in its alpha. Every texel a semi-transparent stamp put
	//on the page takes the alpha of the game's texel it stands for (its original cell, nearest), when that alpha is neither
	//0 nor 255. gameAlpha: the game's page alpha, gameWidth x gameHeight (256 x 256 texels times a whole scale).
	//Returns how many texels changed.
	size_t InheritSemiTransparency(Image& page, int scale, const std::vector<CellStamp>& stamps, const uint8_t* gameAlpha,
		int gameWidth, int gameHeight);

	//"bghall_1_fr" -> "bghall_1" + "fr" (FME's language suffix); anything else is returned as the field name
	void SplitLanguage(const std::string& stem, std::string& field, std::string& language);

	//--- Which .map palette a page texture of the game was made with
	//
	//The game does not number a page's palettes the way the .map does. It gives each palette a slot on the page in the
	//order it first needs it (tex_header palette_index = 2 x slot), so "page 0 palette 2" in the game can be any of the
	//page's .map palettes. The page the game uploads is the field's VRAM (the .mim) converted with that palette, so the
	//palette is found by decoding the .mim with every palette the field uses on that page and comparing.
	//
	//.mim: 4096 unused bytes, 16 palettes of 256 colours (16-bit, from 0x1000), then 256 rows of 1664 bytes (13 texture
	//pages of 128 bytes, from 0x3000). Colours: red bits 0-4, green 5-9, blue 10-14, bit 15 semi-transparency; 0 is
	//transparent.
	constexpr size_t MIM_SIZE = 438272;
	constexpr size_t MIM_PALETTES = 0x1000;
	constexpr size_t MIM_TEXTURE = 0x3000;
	constexpr size_t MIM_ROW = 1664;
	//16-bit colour of texel (x, y) of a texture page at a depth (0: 4-bit, 1: 8-bit, 2: 16-bit), with a palette; -1 outside
	int MimTexel(const uint8_t* mim, size_t size, int texturePage, int depth, int palette, int x, int y);

	struct PaletteMatch
	{
		int palette = 0, depth = 0;
		size_t compared = 0, matching = 0;
		size_t opaque = 0; //compared texels this palette does not make transparent (nothing to tell palettes apart by otherwise)
		double Score() const { return compared == 0 ? 0.0 : static_cast<double>(matching) / static_cast<double>(compared); }
	};
	//Every (palette, depth) the tiles use on this texture page, compared with the game's page (B, G, R, A bytes,
	//width x height = 256 x 256 texels times a whole scale) over every cell those tiles use at that depth. Best first.
	//tiles: the .map as the game shipped it (cells moved by Unshare hold nothing of the .mim).
	std::vector<PaletteMatch> MatchPalette(const uint8_t* mim, size_t mimSize, const std::vector<Tile>& tiles, int texturePage,
		const uint8_t* bgra, int width, int height);
}
