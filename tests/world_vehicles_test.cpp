//Tests for ff8_demaster/world_vehicles.cpp against the real world map data. Portable, any C++20 compiler:
//	g++ -std=c++20 -O2 -Iff8_demaster tests/world_vehicles_test.cpp ff8_demaster/world_vehicles.cpp -o world_vehicles_test
//	./world_vehicles_test <folder with wmsetus.obj and chara.one>
//The two files come from world.fs (tools/world_vehicle_dump.py --extract writes them out).
//
//It plays the game's part: copies textures into a 1024x512 VRAM the way the world map loader does, converts pages to
//BGRA with a chosen palette (or colour animation frame) the way the renderer hands them to glTexImage2D, and checks that
//DEMASTER finds each texture, the right palette or frame, and draws the HD file in the right place.

#include "world_vehicles.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace worldvehicles;

static int failures = 0;
static int checks = 0;
#define CHECK(condition) do { checks++; if (!(condition)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); failures++; } } while (0)

static std::vector<uint8_t> ReadFile(const std::string& path)
{
	std::ifstream f(path, std::ios::binary);
	return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static uint32_t U32(const std::vector<uint8_t>& d, const size_t o) { uint32_t v; std::memcpy(&v, &d[o], 4); return v; }

//wmset section 42: table of contents of TIM offsets at the start of the section
static std::vector<Tim> Section42(const std::vector<uint8_t>& wmset)
{
	std::vector<Tim> tims;
	const uint32_t start = U32(wmset, 41 * 4);
	uint32_t end = static_cast<uint32_t>(wmset.size());
	for (int s = 0; s < 48; s++)
		if (const uint32_t p = U32(wmset, s * 4); p > start && p < end)
			end = p;
	for (uint32_t toc = start; toc + 4 <= end; toc += 4)
	{
		const uint32_t rel = U32(wmset, toc);
		if (rel == 0 || start + rel >= end)
			break;
		Tim tim;
		if (!ParseTim(&wmset[start + rel], end - start - rel, tim))
			break;
		tims.push_back(tim);
	}
	return tims;
}

struct Vram
{
	std::vector<uint16_t> words = std::vector<uint16_t>(1024 * 512, 0);
	uint16_t& At(const int x, const int y) { return words[static_cast<size_t>(y) * 1024 + x]; }

	void Upload(const Tim& tim, const int x, const int y)
	{
		for (int row = 0; row < tim.height; row++)
			for (int u = 0; u < tim.widthUnits; u++)
				At(x + u, y + row) = static_cast<uint16_t>(tim.image[(row * tim.widthUnits + u) * 2] | (tim.image[(row * tim.widthUnits + u) * 2 + 1] << 8));
	}

	//page as the renderer hands it to glTexImage2D: 256x256 BGRA, colour 0 transparent
	std::vector<uint8_t> Page(const int tPage, const int bpp, const std::vector<uint16_t>& palette)
	{
		const int px = tPage % 16 * 64, py = tPage / 16 * 256;
		std::vector<uint8_t> bgra(256 * 256 * 4);
		for (int y = 0; y < 256; y++)
			for (int x = 0; x < 256; x++)
			{
				int index;
				if (bpp == 0)
					index = (At(px + x / 4, py + y) >> ((x % 4) * 4)) & 15;
				else
					index = (At(px + x / 2, py + y) >> ((x % 2) * 8)) & 255;
				const uint16_t c = palette[index];
				const int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
				uint8_t* o = &bgra[(y * 256 + x) * 4];
				o[0] = static_cast<uint8_t>((b << 3) | (b >> 2));
				o[1] = static_cast<uint8_t>((g << 3) | (g >> 2));
				o[2] = static_cast<uint8_t>((r << 3) | (r >> 2));
				o[3] = c == 0 ? 0 : 255;
			}
		return bgra;
	}
};

static std::vector<uint16_t> PaletteOf(const Tim& tim, const int palette)
{
	std::vector<uint16_t> colours(tim.ColorsPerPalette());
	for (int i = 0; i < tim.ColorsPerPalette(); i++)
		colours[i] = tim.Color(palette, i);
	return colours;
}

static std::string S42(const int n) { return "world\\dat\\wmset\\section42\\texture" + std::to_string(n); }
static std::string S38(const int n) { return "world\\dat\\wmset\\section38\\texture" + std::to_string(n); }
static std::string C(const int model, const int texture) { return "world\\esk\\chara_one\\model" + std::to_string(model) + "-" + std::to_string(texture); }

static RgbaImage Solid(const int w, const int h, const uint8_t r, const uint8_t g, const uint8_t b)
{
	RgbaImage image;
	image.width = w;
	image.height = h;
	image.pixels.resize(static_cast<size_t>(w) * h * 4);
	for (int i = 0; i < w * h; i++)
	{
		image.pixels[i * 4] = r;
		image.pixels[i * 4 + 1] = g;
		image.pixels[i * 4 + 2] = b;
		image.pixels[i * 4 + 3] = 255;
	}
	return image;
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		std::printf("usage: world_vehicles_test <folder with wmsetus.obj and chara.one>\n");
		return 2;
	}
	const std::string folder = argv[1];
	const std::vector<uint8_t> wmset = ReadFile(folder + "/wmsetus.obj");
	const std::vector<uint8_t> charaOne = ReadFile(folder + "/chara.one");
	if (wmset.empty() || charaOne.empty())
	{
		std::printf("missing wmsetus.obj or chara.one in %s\n", folder.c_str());
		return 2;
	}

	//--- parsing
	const std::vector<NamedTexture> wm = ParseWmsetTextures(wmset.data(), wmset.size());
	std::map<std::string, std::shared_ptr<const NamedTexture>> named;
	for (const NamedTexture& t : wm)
		named[t.name] = std::make_shared<const NamedTexture>(t);
	CHECK(wm.size() == 36 + 13 + 1 + 33);
	auto W = [&](const std::string& name) -> const std::shared_ptr<const NamedTexture>& {
		static const std::shared_ptr<const NamedTexture> none;
		const auto it = named.find(name);
		return it == named.end() ? none : it->second;
	};
	auto T = [&](const std::string& name) -> const Tim& { return W(name)->tim; };
	const std::vector<Tim> s42 = Section42(wmset);
	CHECK(s42.size() == 33);
	CHECK(T(S42(0)).bpp == 1 && T(S42(0)).x == 832 && T(S42(0)).y == 0 && T(S42(0)).Width() == 256 && T(S42(0)).height == 64); //Balamb Garden
	CHECK(T(S42(10)).bpp == 0 && T(S42(10)).x == 992 && T(S42(10)).PaletteCount() == 2); //Galbadian army car
	CHECK(T(S42(15)).x == 960 && T(S42(15)).y == 128 && T(S42(15)).PaletteCount() == 4); //Galbadia Garden
	for (int n = 0; n < 33; n++)
		CHECK(T(S42(n)).image == s42[n].image);
	CHECK(T(S38(11)).x == 768 && T(S38(11)).y == 0 && T(S38(11)).Width() == 256 && T(S38(11)).height == 192
		&& T(S38(11)).PaletteCount() == 2); //the select map, colour and grey
	CHECK(T(S38(8)).PaletteCount() == 16); //sea
	CHECK(W("world\\dat\\wmset\\section40\\texture0") && T("world\\dat\\wmset\\section40\\texture0").x == 64);
	CHECK(W("world\\dat\\wmset\\section39\\texture12") && T("world\\dat\\wmset\\section39\\texture12").Width() == 64);
	//colour animation (section 41) belongs to the four 8-bit water textures whose colour table it replaces
	for (const NamedTexture& t : wm)
	{
		const bool animated = t.name == S38(16) || t.name == S38(17) || t.name == S38(18) || t.name == S38(19);
		CHECK(animated == !t.frames.empty());
		if (animated)
		{
			CHECK(t.frames.size() == 6 && t.frames[0].size() == 256);
			//texture17's own colour table is frame 0; texture16's and 19's are replaced by the frames when the game runs
			if (t.name == S38(17))
				CHECK(std::equal(t.frames[0].begin(), t.frames[0].end(), t.tim.clut.begin()));
			CHECK(t.frames[1] != t.frames[0]);
		}
	}
	//page atlases: effects at their VRAM spot in the page they are meant for
	auto atlasIs = [&](const std::string& name, const char* atlas, const int x, const int y) {
		const auto& t = W(name);
		return t && t->atlas == std::string("world\\dat\\wmset\\atlas\\") + atlas && t->atlasX == x && t->atlasY == y;
	};
	CHECK(atlasIs(S38(11), "page12", 0, 0)); //select map
	CHECK(atlasIs(S38(9), "page13", 0, 64)); //moon
	CHECK(atlasIs(S38(14), "page13", 192, 128));
	CHECK(atlasIs(S38(10), "page14", 0, 64)); //clouds
	CHECK(atlasIs(S38(8), "page28", 0, 0)); //sea
	CHECK(atlasIs(S38(17), "page28_8bit", 0, 192)); //the 8 water tiles share one 8-bit page
	CHECK(atlasIs(S38(19), "page28_8bit", 128, 192));
	CHECK(atlasIs(S38(22), "page28_8bit", 192, 128));
	CHECK(atlasIs("world\\dat\\wmset\\section39\\texture12", "page29", 128, 0));
	CHECK(W(S38(2))->atlas.empty() && W("world\\dat\\wmset\\section40\\texture0")->atlas.empty() && W(S42(0))->atlas.empty());
	//no two textures of one atlas overlap
	for (const NamedTexture& a : wm)
		for (const NamedTexture& b : wm)
			if (&a < &b && !a.atlas.empty() && a.atlas == b.atlas)
				CHECK(a.atlasX + a.tim.Width() <= b.atlasX || b.atlasX + b.tim.Width() <= a.atlasX
					|| a.atlasY + a.tim.height <= b.atlasY || b.atlasY + b.tim.height <= a.atlasY);
	CHECK(TwinName(S42(1)) == S42(0) && TwinName(S42(15)) == S42(16) && TwinName(S42(3)).empty());
	CHECK(TwinName("world\\esk\\chara_one\\model3") == "world\\esk\\chara_one\\model2");
	CHECK(TwinName("world\\esk\\chara_one\\model4").empty()); //student Squall's body differs
	for (const int n : { 1, 16, 32 })
		CHECK(T(S42(n)).image == T(TwinName(S42(n))).image);

	const std::vector<NamedTexture> charaList = ParseCharaOne(charaOne.data(), charaOne.size());
	CHECK(charaList.size() == 16);
	for (const NamedTexture& t : charaList)
		named[t.name] = std::make_shared<const NamedTexture>(t);
	//Ragnarok: one strip, 12 + 84 + 92 + 68 texels
	CHECK(W(C(1, 0))->strip == "world\\esk\\chara_one\\model1" && W(C(1, 0))->sliceX == 0 && W(C(1, 1))->sliceX == 12
		&& W(C(1, 2))->sliceX == 96 && W(C(1, 3))->sliceX == 188 && W(C(1, 3))->stripWidth == 256 && W(C(1, 3))->stripHeight == 64);
	CHECK(W(C(0, 1))->sliceX == 20 && W(C(0, 1))->stripWidth == 64 && W(C(2, 1))->stripWidth == 128);
	//the chocobo twin: the two models really are the same
	CHECK(T(C(3, 0)).image == T(C(2, 0)).image && T(C(3, 1)).image == T(C(2, 1)).image && T(C(3, 1)).clut == T(C(2, 1)).clut);
	CHECK(W(C(0, 0))->imageOffset > 0 && W(C(0, 0))->imageOffset < charaOne.size());

	//--- the game loads a disc 1/2 set: Balamb Garden, a Galbadian car, a train, the White SeeD ship spot holds Galbadia Garden
	Vram vram;
	VramTracker tracker;
	auto upload = [&](const std::string& name, const int x, const int y) {
		vram.Upload(T(name), x, y);
		tracker.Record(W(name), x, y);
	};
	for (const int n : { 0, 10, 2, 3, 15, 14 })
		upload(S42(n), T(S42(n)).x, T(S42(n)).y);
	//Ragnarok: chara.one textures are placed by the game, not where the file says
	upload(C(1, 1), 960, 192);
	CHECK(tracker.Snapshot().size() == 7);
	auto snapshotName = [&](const size_t i) { return tracker.Snapshot()[i].Name(); };

	//page 15 converted with the army car's second palette: every texture on page 15 is found (each region of the page is
	//only drawn with its own palette, so every palette texture of the page gets the same vehicles), and the car is
	//recognised as the one this palette belongs to
	auto names = [&](const MatchReport& r) {
		std::vector<std::string> out;
		for (const PageMatch& m : r.matches)
			out.push_back(snapshotName(m.textureIndex));
		std::sort(out.begin(), out.end());
		return out;
	};
	std::vector<std::string> expected15 = { S42(10), S42(14), S42(15), S42(2), S42(3), C(1, 1) };
	std::sort(expected15.begin(), expected15.end());
	{
		const auto page = vram.Page(15, 0, PaletteOf(T(S42(10)), 1));
		const MatchReport r = MatchPage(page.data(), 256, 256, 960, 0, tracker.Snapshot());
		CHECK(names(r) == expected15);
		for (const PageMatch& m : r.matches)
			if (snapshotName(m.textureIndex) == S42(10))
				CHECK(m.palette == 1 && m.frame == -1 && m.texelX == 128 && m.texelY == 0 && m.texelWidth == 64 && m.texelHeight == 64
					&& m.agreement == 1.0);
		//oldest first: the order they are drawn in
		for (size_t k = 1; k < r.matches.size(); k++)
			CHECK(r.matches[k - 1].textureIndex < r.matches[k].textureIndex);
	}
	//the same page made with Galbadia Garden's, the train's and Ragnarok's palettes: the same textures every time
	for (const std::vector<uint16_t>& palette : { PaletteOf(T(S42(15)), 2), PaletteOf(T(S42(2)), 0), PaletteOf(T(C(1, 1)), 0) })
	{
		const auto page = vram.Page(15, 0, palette);
		CHECK(names(MatchPage(page.data(), 256, 256, 960, 0, tracker.Snapshot())) == expected15);
	}
	{
		const auto page = vram.Page(15, 0, PaletteOf(T(S42(15)), 2));
		for (const PageMatch& m : MatchPage(page.data(), 256, 256, 960, 0, tracker.Snapshot()).matches)
			if (snapshotName(m.textureIndex) == S42(15))
				CHECK(m.palette == 2 && m.texelY == 128);
	}
	//Ragnarok: found where the game put it
	{
		const auto page = vram.Page(15, 0, PaletteOf(T(C(1, 1)), 0));
		for (const PageMatch& m : MatchPage(page.data(), 256, 256, 960, 0, tracker.Snapshot()).matches)
			if (snapshotName(m.textureIndex) == C(1, 1))
				CHECK(m.texelY == 192 && m.texelX == 0 && m.palette == 0);
	}
	//a palette where every colour is the same proves nothing: nothing is reported
	{
		const auto page = vram.Page(15, 0, std::vector<uint16_t>(16, 0x1234));
		CHECK(MatchPage(page.data(), 256, 256, 960, 0, tracker.Snapshot()).matches.empty());
	}
	//Balamb Garden is an 8-bit texture: 8-bit page 13 covers x 832..959
	{
		const auto page = vram.Page(13, 1, PaletteOf(T(S42(0)), 0));
		const MatchReport r = MatchPage(page.data(), 256, 256, 832, 0, tracker.Snapshot());
		CHECK(r.matches.size() == 1 && snapshotName(r.matches[0].textureIndex) == S42(0) && r.matches[0].texelWidth == 256
			&& r.matches[0].palette == 0);
	}
	//the same page as a 4-bit page (moon, effects) must not report the Garden
	{
		const auto page = vram.Page(13, 0, PaletteOf(T(S42(12)), 0));
		for (const PageMatch& m : MatchPage(page.data(), 256, 256, 832, 0, tracker.Snapshot()).matches)
			CHECK(snapshotName(m.textureIndex) != S42(0));
	}
	//a page uploaded at 2x (512) is matched on every second pixel
	{
		const auto small = vram.Page(15, 0, PaletteOf(T(S42(10)), 0));
		std::vector<uint8_t> big(512 * 512 * 4);
		for (int y = 0; y < 512; y++)
			for (int x = 0; x < 512; x++)
				std::memcpy(&big[(y * 512 + x) * 4], &small[((y / 2) * 256 + x / 2) * 4], 4);
		const MatchReport r = MatchPage(big.data(), 512, 512, 960, 0, tracker.Snapshot());
		CHECK(names(r) == expected15);
		for (const PageMatch& m : r.matches)
			if (snapshotName(m.textureIndex) == S42(10))
				CHECK(m.palette == 0);
	}
	//only the textures touching a VRAM rectangle
	{
		uint64_t generation = 0;
		CHECK(tracker.Snapshot(960, 0, 128, 256, &generation).size() == 7 - 1); //all but the Garden (832..959)
		CHECK(tracker.Snapshot(0, 256, 128, 256, nullptr).empty());
		CHECK(generation > 0);
	}

	//--- story moves on: another rental car is loaded into the same spot. The old entry goes, the new one matches.
	upload(S42(19), 992, 0);
	{
		bool hasOld = false;
		for (const VramTexture& t : tracker.Snapshot())
			hasOld |= t.Name() == S42(10);
		CHECK(!hasOld);
		const auto page = vram.Page(15, 0, PaletteOf(T(S42(19)), 1));
		bool found19 = false;
		for (const PageMatch& m : MatchPage(page.data(), 256, 256, 960, 0, tracker.Snapshot()).matches)
		{
			CHECK(snapshotName(m.textureIndex) != S42(10));
			if (snapshotName(m.textureIndex) == S42(19))
				found19 = m.palette == 1;
		}
		CHECK(found19);
	}
	//something the tracker never heard of overwrote the train: stale entry must not match any more
	{
		for (int y = 64; y < 128; y++)
			for (int x = 960; x < 992; x++)
				vram.At(x, y) = static_cast<uint16_t>(x * 31 + y * 7);
		const auto page = vram.Page(15, 0, PaletteOf(T(S42(2)), 0));
		for (const PageMatch& m : MatchPage(page.data(), 256, 256, 960, 0, tracker.Snapshot()).matches)
			CHECK(snapshotName(m.textureIndex) != S42(2));
	}
	//the vessel's spot cleared to zero: one flat colour, must not match under any palette
	{
		for (int y = 64; y < 128; y++)
			for (int x = 992; x < 1024; x++)
				vram.At(x, y) = 0;
		for (const std::vector<uint16_t>& palette : { PaletteOf(T(S42(14)), 0), PaletteOf(T(S42(19)), 1) })
		{
			const auto page = vram.Page(15, 0, palette);
			for (const PageMatch& m : MatchPage(page.data(), 256, 256, 960, 0, tracker.Snapshot()).matches)
				CHECK(snapshotName(m.textureIndex) != S42(14));
		}
	}

	//--- water: the sea (4-bit, 256x256) is loaded first, then the 8-bit water textures cover its lower half. The sea
	//stays (partly covered), is matched on what is left of it, and the 8-bit ones are matched on the 8-bit page.
	{
		Vram water;
		VramTracker waterTracker;
		auto put = [&](const std::string& name) {
			water.Upload(T(name), T(name).x, T(name).y);
			waterTracker.Record(W(name), T(name).x, T(name).y);
		};
		put(S38(8));
		for (int n = 16; n <= 23; n++)
			put(S38(n));
		CHECK(waterTracker.Snapshot().size() == 9);
		//4-bit page 28 with sea palette 5
		{
			const auto page = water.Page(28, 0, PaletteOf(T(S38(8)), 5));
			const MatchReport r = MatchPage(page.data(), 256, 256, 768, 256, waterTracker.Snapshot());
			bool sea = false;
			for (const PageMatch& m : r.matches)
				if (waterTracker.Snapshot()[m.textureIndex].Name() == S38(8))
					sea = m.palette == 5 && m.texelWidth == 256 && m.texelHeight == 256;
			CHECK(sea);
		}
		//8-bit page 28, with texture17's colour table at animation frame 3
		{
			const NamedTexture& t17 = *W(S38(17));
			const auto page = water.Page(28, 1, t17.frames[3]);
			const MatchReport r = MatchPage(page.data(), 256, 256, 768, 256, waterTracker.Snapshot());
			bool found = false;
			for (const PageMatch& m : r.matches)
				if (waterTracker.Snapshot()[m.textureIndex].Name() == S38(17))
					found = m.frame == 3 && m.texelX == 0 && m.texelY == 192 && m.texelWidth == 64;
			CHECK(found);
			for (const PageMatch& m : r.matches)
				CHECK(waterTracker.Snapshot()[m.textureIndex].Name() != S38(8)); //4-bit sea not on the 8-bit page
		}
		//frame 0 is reported as frame 0 (not as the texture's own palette)
		{
			const auto page = water.Page(28, 1, W(S38(17))->frames[0]);
			for (const PageMatch& m : MatchPage(page.data(), 256, 256, 768, 256, waterTracker.Snapshot()).matches)
				if (waterTracker.Snapshot()[m.textureIndex].Name() == S38(17))
					CHECK(m.frame == 0 && m.palette == -1);
		}
		//the select map: palette 1 (grey)
		{
			water.Upload(T(S38(11)), 768, 0);
			waterTracker.Record(W(S38(11)), 768, 0);
			const auto page = water.Page(12, 0, PaletteOf(T(S38(11)), 1));
			const MatchReport r = MatchPage(page.data(), 256, 256, 768, 0, waterTracker.Snapshot());
			CHECK(r.matches.size() == 1 && waterTracker.Snapshot()[r.matches[0].textureIndex].Name() == S38(11)
				&& r.matches[0].palette == 1 && r.matches[0].texelHeight == 192);
		}
		//a texture completely covered by a newer one is dropped
		{
			VramTracker t;
			t.Record(W(S38(12)), 880, 112); //16x16 effect
			t.Record(W(S38(14)), 880, 112); //64x16 over it
			CHECK(t.Snapshot().size() == 1 && t.Snapshot()[0].Name() == S38(14));
		}
	}

	//--- compose (CPU reference of what the GPU does): vanilla page upscaled x4, HD car drawn at its rectangle
	{
		const auto page = vram.Page(15, 0, PaletteOf(T(S42(19)), 1));
		const MatchReport r = MatchPage(page.data(), 256, 256, 960, 0, tracker.Snapshot());
		const RgbaImage hd = Solid(256, 256, 10, 200, 30);
		std::vector<Placement> placements;
		for (const PageMatch& m : r.matches)
			if (snapshotName(m.textureIndex) == S42(19))
				placements.push_back({ &hd, m.texelX, m.texelY, m.texelWidth, m.texelHeight });
		CHECK(placements.size() == 1);
		const RgbaImage out = ComposePage(page.data(), 256, 256, 4, nullptr, placements);
		CHECK(out.width == 1024 && out.height == 1024);
		const uint8_t* inside = out.At(128 * 4 + 5, 5);
		CHECK(inside[0] == 10 && inside[1] == 200 && inside[2] == 30 && inside[3] == 255);
		const uint8_t* corner = out.At((128 + 64) * 4 - 1, 64 * 4 - 1);
		CHECK(corner[1] == 200);
		//outside: the vanilla texel, nearest-neighbour upscaled (BGRA -> RGBA)
		const uint8_t* outside = out.At(4 * 4 + 1, 70 * 4 + 2);
		const uint8_t* vanilla = &page[(70 * 256 + 4) * 4];
		CHECK(outside[0] == vanilla[2] && outside[1] == vanilla[1] && outside[2] == vanilla[0] && outside[3] == vanilla[3]);
		//a base page image (Maki's wmset_XXX) replaces the vanilla pixels under the vehicles
		const RgbaImage base = Solid(512, 512, 1, 2, 3);
		const RgbaImage withBase = ComposePage(page.data(), 256, 256, 4, &base, placements);
		const uint8_t* b = withBase.At(4 * 4 + 1, 70 * 4 + 2);
		CHECK(b[0] == 1 && b[1] == 2 && b[2] == 3);
		CHECK(withBase.At(128 * 4 + 5, 5)[1] == 200);
	}
	//--- a strip slice: Ragnarok's second piece out of a 4x strip
	{
		RgbaImage strip = Solid(1024, 256, 0, 0, 0);
		for (int y = 0; y < 256; y++)
			for (int x = 48; x < 48 + 336; x++)
				strip.At(x, y)[1] = 255;
		const RgbaImage piece = Crop(strip, 48, 0, 336, 256);
		CHECK(piece.width == 336 && piece.height == 256 && piece.At(0, 0)[1] == 255 && piece.At(335, 255)[1] == 255);
		CHECK(!Crop(strip, 2000, 0, 10, 10).IsValid());
	}

	//--- resample keeps transparency: a half-transparent HD file stays transparent where it is transparent
	{
		RgbaImage hd = Solid(8, 8, 255, 0, 0);
		for (int y = 0; y < 8; y++)
			for (int x = 4; x < 8; x++)
				hd.At(x, y)[3] = 0;
		RgbaImage target = Solid(8, 8, 0, 0, 255);
		ResampleInto(hd, target, 0, 0, 8, 8);
		CHECK(target.At(1, 1)[0] == 255 && target.At(1, 1)[3] == 255);
		CHECK(target.At(6, 1)[3] == 0);
	}

	std::printf("%d checks, %d failure(s)\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
