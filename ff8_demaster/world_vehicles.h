#pragma once
//World map textures by name (WORLD_VEHICLES): vehicles, characters, water, effects, the select map, ...
//
//The world map does not keep its textures in fixed places. It copies what the current story point needs into VRAM
//(wmset sections 38-42, chara.one), several of them share a spot, and it animates water by swapping colour tables.
//A whole-page wmset_XXX file is right for one moment only, and hashing pages fails because every page mixes textures.
//So DEMASTER is told by the game, every time it copies a texture to VRAM, which texture it is and where it goes
//(texturepatch_v2_worldVehicles.cpp hooks the game's LoadImage). When a page is later uploaded as a texture, every
//named texture still on that page is drawn into it from its HD file (on the GPU), over the page's wmset_XXX file if
//there is one. Still one texture per page, built only when the game uploads it.
//
//File names (DEMASTER_EXP\textures\..., .dds or .png):
//	world\dat\wmset\atlas\page13           effects (wmset sections 38 and 39): one file per VRAM page, every texture at
//	                                          its VRAM spot in its own colours (page12 select map, page13 moon and effects,
//	                                          page14 clouds, page28 sea, page28_8bit water and shores, page29 roads/rails)
//	world\dat\wmset\atlas\page28_8bit_f2    optional: colour animation frame 2 / palette 3 (..._p3) of the textures on it
//	world\dat\wmset\section38\texture11       one file per texture, used for every palette of it (wins over the atlas)
//	world\dat\wmset\section38\texture8_p3     optional: only for palette 3 (textures whose palettes recolour the same texels,
//	                                          such as the sea or the select map's grey version)
//	world\dat\wmset\section38\texture17_f2    optional: frame 2 of the texture's colour animation (wmset section 41)
//	world\dat\wmset\section42\texture0        vehicles: one file with every palette region in it (tools/world_vehicle_dump.py)
//	world\esk\chara_one\model1                one strip per chara.one model, its textures left to right (Ragnarok:
//	                                          12+84+92+68 = 256 texels wide); the game may place the pieces apart, DEMASTER
//	                                          cuts the strip by their widths
//
//This file has no Windows, OpenGL or game dependencies so it can be tested on its own (tests/world_vehicles_test.cpp).

#include <cstdint>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace worldvehicles
{
	//8-bit RGBA, R,G,B,A byte order, rows top to bottom
	struct RgbaImage
	{
		int width = 0;
		int height = 0;
		std::vector<uint8_t> pixels;

		bool IsValid() const { return width > 0 && height > 0 && pixels.size() == static_cast<size_t>(width) * height * 4; }
		const uint8_t* At(int x, int y) const { return pixels.data() + (static_cast<size_t>(y) * width + x) * 4; }
		uint8_t* At(int x, int y) { return pixels.data() + (static_cast<size_t>(y) * width + x) * 4; }
	};

	//PlayStation TIM: indexed image + colour tables (CLUTs)
	struct Tim
	{
		int bpp = 0; //0 = 4-bit, 1 = 8-bit (16-bit images are not used here)
		uint16_t x = 0, y = 0; //VRAM position written in the file (chara.one textures are moved by the game)
		uint16_t widthUnits = 0, height = 0; //width in 16-bit VRAM units
		std::vector<uint8_t> image; //raw bytes as stored (2 or 1 texels per byte)
		uint16_t clutX = 0, clutY = 0, clutWidth = 0, clutHeight = 0;
		std::vector<uint16_t> clut;

		int TexelsPerUnit() const { return bpp == 0 ? 4 : 2; }
		int Width() const { return widthUnits * TexelsPerUnit(); }
		int ColorsPerPalette() const { return bpp == 0 ? 16 : 256; }
		int PaletteCount() const;
		uint8_t IndexAt(int texelX, int texelY) const;
		uint16_t Color(int palette, int index) const;
	};

	//Parses a TIM at data[0..size). imageOffset (optional) receives the offset of the first texel byte.
	bool ParseTim(const uint8_t* data, size_t size, Tim& out, size_t* imageOffset = nullptr);

	//A texture in one of the game's files, with its name and where its texels are in that file
	struct NamedTexture
	{
		std::string name; //"world\\dat\\wmset\\section38\\texture11", "world\\esk\\chara_one\\model1-2"
		Tim tim;
		size_t imageOffset = 0; //offset of the first texel byte in the file
		std::vector<std::vector<uint16_t>> frames; //colour animation (wmset section 41): one colour table per frame
		//chara.one: the model's strip file and where this texture is in it (texels)
		std::string strip; //"world\\esk\\chara_one\\model1", empty for other textures
		int sliceX = 0, stripWidth = 0, stripHeight = 0;
		//wmset effects (section 38 from texture 8 on, section 39): the page atlas, laid out like the VRAM page the texture
		//is meant for (256x256 texels), and where the texture is in it
		std::string atlas; //"world\\dat\\wmset\\atlas\\page13", "...\\page28_8bit", empty for other textures
		int atlasX = 0, atlasY = 0;
	};

	//The page atlas a texture belongs to, from the VRAM spot in its header: 4-bit textures go to the page they start in,
	//8-bit ones to the 8-bit page (128 units wide) on a 128-unit boundary that holds them (else the one starting at their
	//page). false if it does not fit one page.
	bool AtlasOf(const Tim& tim, std::string& name, int& texelX, int& texelY);

	//wmset (world\dat\wmsetXX.obj): the TIMs of sections 38, 39, 40 and 42, with section 41's colour animations
	//attached to the textures whose colour table they replace
	std::vector<NamedTexture> ParseWmsetTextures(const uint8_t* data, size_t size);

	//chara.one (world\esk): the table of contents is at the end of the file, read backwards (same as FFNx)
	struct CharaOneTexture
	{
		int model = 0;
		int texture = 0;
		uint32_t timOffset = 0; //offset of the TIM in the file
	};
	std::vector<CharaOneTexture> ParseCharaOneTextures(const uint8_t* data, size_t size);
	std::vector<NamedTexture> ParseCharaOne(const uint8_t* data, size_t size);

	//A named texture the game copied into VRAM, and where it went
	struct VramTexture
	{
		std::shared_ptr<const NamedTexture> texture;
		int x = 0, y = 0; //VRAM position in 16-bit units, where the game actually put it
		uint64_t serial = 0;

		const std::string& Name() const { return texture->name; }
		const Tim& GetTim() const { return texture->tim; }
		int WidthUnits() const { return texture->tim.widthUnits; }
		int Height() const { return texture->tim.height; }
	};

	//Remembers what the game copied where, oldest first. A copy removes older ones it covers completely; partly covered
	//ones stay (MatchPage only looks at their uncovered texels).
	class VramTracker
	{
	public:
		void Record(std::shared_ptr<const NamedTexture> texture, int x, int y);
		std::vector<VramTexture> Snapshot() const;
		//only the textures touching the VRAM rectangle (units), and the tracker's generation (changes with every Record)
		std::vector<VramTexture> Snapshot(int x, int y, int widthUnits, int height, uint64_t* generation) const;
		void Clear();

	private:
		mutable std::mutex mutex;
		std::vector<VramTexture> textures;
		uint64_t nextSerial = 1;
	};

	//A named texture found in a page upload
	struct PageMatch
	{
		size_t textureIndex = 0; //into the vector given to MatchPage
		int texelX = 0, texelY = 0, texelWidth = 0, texelHeight = 0; //in page texels (1x)
		double agreement = 0.0; //fraction of its uncovered texels that agree with the texture's own index pattern
		int palette = -1; //which of the texture's palettes this page texture was made with, -1 = none of them
		int frame = -1; //which colour animation frame, -1 = none
	};

	struct MatchReport
	{
		std::vector<PageMatch> matches; //oldest first: draw in this order
		std::vector<PageMatch> nearMisses; //agreed on at least half the texels but not enough (for logs)
	};

	//page: BGRA bytes exactly as the game hands them to glTexImage2D, width x height pixels.
	//pageX/pageY: VRAM origin of the page (tPage % 16 * 64, tPage / 16 * 256). A page is 256 texels wide in both
	//4-bit (64 units) and 8-bit (128 units) mode; pages bigger than 256 are treated as an integer upscale.
	//textures: oldest first (as the tracker returns them); a newer one hides what it covers of older ones.
	//The game makes one texture of the page per palette, so the colours depend on which palette this one was made with.
	//What does not depend on it is the index pattern: texels that share a colour index must share a colour. A texture
	//is present when at least minAgreement of its uncovered texels follow its own pattern and it is not one flat colour.
	MatchReport MatchPage(const uint8_t* pageBgra, int width, int height, int pageX, int pageY,
		const std::vector<VramTexture>& textures, double minAgreement = 0.97);

	//Area-weighted resample of source into target[x..x+w, y..y+h) (alpha-weighted, clipped to the target)
	void ResampleInto(const RgbaImage& source, RgbaImage& target, int x, int y, int w, int h);

	//source[x..x+w, y..y+h) as its own image (clipped)
	RgbaImage Crop(const RgbaImage& source, int x, int y, int w, int h);

	//CPU version of what the game glue does on the GPU (tests, reference): base (RGBA) or, when base is not valid, the
	//game's page (BGRA) upscaled with nearest neighbour to (256 * scale, height * scale); every image drawn at its texel
	//rectangle times scale, in order.
	struct Placement
	{
		const RgbaImage* image = nullptr;
		int texelX = 0, texelY = 0, texelWidth = 0, texelHeight = 0;
	};
	RgbaImage ComposePage(const uint8_t* pageBgra, int width, int height, int scale, const RgbaImage* base,
		const std::vector<Placement>& placements);

	//Textures (or chara.one model strips) that are exact copies, so one file covers both:
	//	section 42: 1 = 0, 16 = 15, 32 = 29 (same VRAM spot, drawn by two models at once: a Garden and its ring)
	//	chara.one:  model3 = model2 (the two chocobo models)
	//Returns the other name of the pair, or an empty string.
	std::string TwinName(const std::string& name);
}
