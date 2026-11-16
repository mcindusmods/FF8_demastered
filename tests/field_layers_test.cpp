//Tests for ff8_demaster/field_layers.cpp.
//  g++ -std=c++20 -I ../ff8_demaster field_layers_test.cpp ../ff8_demaster/field_layers.cpp -o field_layers_test
//  ./field_layers_test [path to the OpenVIII oracle, or -] [folder of real .map files] [PuPu oracle command]
//The OpenVIII oracle (optional) is a small program built from OpenVIII's own Map / PupuID / UniquifyPupu - the code
//Field-Map-Editor uses - that prints the canvas and every tile's position and PupuID; random maps are checked against it.
//The PuPu oracle (optional) is tests/pupu_oracle (PuPuSharp's own code, run as "dotnet .../PupuOracle.dll"): every tile's
//PuPu name and every picture's frame, checked on random maps and on the real ones. A bdifrit1.map in the folder is also
//checked against the files PuPu made for it.
#include "field_layers.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

using namespace fieldlayers;

static int checks = 0, failures = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::vector<uint8_t> Bytes(const std::vector<Tile>& tiles, bool terminator = true)
{
	std::vector<uint8_t> out;
	for (const Tile& t : tiles)
	{
		uint8_t b[16];
		auto put = [&](int o, uint16_t v) { b[o] = v & 0xFF; b[o + 1] = v >> 8; };
		put(0, static_cast<uint16_t>(t.x));
		put(2, static_cast<uint16_t>(t.y));
		put(4, t.z);
		put(6, t.texId);
		put(8, t.palId);
		b[10] = t.srcX; b[11] = t.srcY; b[12] = t.layer; b[13] = t.blend; b[14] = t.animId; b[15] = t.animState;
		out.insert(out.end(), b, b + 16);
	}
	if (terminator)
	{
		out.push_back(0xFF);
		out.push_back(0x7F);
	}
	return out;
}

static Tile MakeTile(int x, int y, int page, int depth, int palette, int sx, int sy, int layer = 0, int blend = 4, int anim = 0xFF, int state = 0)
{
	Tile t;
	t.x = static_cast<int16_t>(x);
	t.y = static_cast<int16_t>(y);
	t.z = 100;
	t.texId = static_cast<uint16_t>(page | 1 << 4 | depth << 7);
	t.palId = static_cast<uint16_t>(30 | palette << 6);
	t.srcX = static_cast<uint8_t>(sx);
	t.srcY = static_cast<uint8_t>(sy);
	t.layer = static_cast<uint8_t>(layer << 1);
	t.blend = static_cast<uint8_t>(blend);
	t.animId = static_cast<uint8_t>(anim);
	t.animState = static_cast<uint8_t>(state);
	return t;
}

static std::vector<Tile> RandomMap(std::mt19937& rng)
{
	std::uniform_int_distribution<int> count(1, 900), coin(0, 99), cell(-25, 40), small(0, 15), layer(0, 7), blend(0, 4),
		anim(0, 12), state(0, 3), page(0, 12), depth(0, 1), palette(0, 15), odd(1, 15);
	const int n = count(rng);
	const bool positive = coin(rng) < 30; //no shift at all
	const bool onlyY = coin(rng) < 15; //only y negative
	std::vector<Tile> tiles;
	for (int i = 0; i < n; i++)
	{
		int x = cell(rng) * 16, y = cell(rng) * 16;
		if (positive) { x = std::abs(x) + 8; y = std::abs(y); }
		if (onlyY) x = std::abs(x);
		if (coin(rng) < 8) x += odd(rng);
		if (coin(rng) < 8) y += odd(rng);
		if (coin(rng) < 10 && !tiles.empty()) { x = tiles[coin(rng) % tiles.size()].x; y = tiles[coin(rng) % tiles.size()].y; }
		const bool animated = coin(rng) < 15;
		tiles.push_back(MakeTile(x, y, page(rng), depth(rng), palette(rng), small(rng) * 16, small(rng) * 16, layer(rng),
			coin(rng) < 80 ? 4 : blend(rng), animated ? anim(rng) : 0xFF, animated ? state(rng) : 0));
	}
	return tiles;
}

static bool RunOracle(const std::string& oracle, const std::vector<uint8_t>& bytes, int& cw, int& ch, std::vector<int>& xs,
	std::vector<int>& ys, std::vector<uint32_t>& pupus)
{
	const std::string mapPath = "/tmp/field_layers_test.map";
	{
		std::ofstream f(mapPath, std::ios::binary);
		f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}
	const std::string command = "'" + oracle + "' " + mapPath;
	FILE* p = popen(command.c_str(), "r");
	if (!p)
		return false;
	char line[256];
	int cx = 0, cy = 0;
	bool header = false;
	while (fgets(line, sizeof(line), p))
	{
		if (!header)
		{
			header = sscanf(line, "canvas %d %d %d %d", &cx, &cy, &cw, &ch) == 4;
			continue;
		}
		int x = 0, y = 0;
		unsigned pupu = 0;
		if (sscanf(line, "%d %d %x", &x, &y, &pupu) == 3)
		{
			xs.push_back(x);
			ys.push_back(y);
			pupus.push_back(pupu);
		}
	}
	pclose(p);
	return header;
}

static void CompareWithOracle(const std::string& oracle, const std::vector<uint8_t>& bytes, const char* what)
{
	const std::vector<Tile> tiles = ParseMap(bytes.data(), bytes.size());
	const Layout layout = ComputeLayout(tiles);
	int cw = 0, ch = 0;
	std::vector<int> xs, ys;
	std::vector<uint32_t> pupus;
	CHECK(RunOracle(oracle, bytes, cw, ch, xs, ys, pupus));
	CHECK(xs.size() == tiles.size());
	CHECK(cw == layout.width && ch == layout.height);
	size_t bad = 0;
	for (size_t i = 0; i < tiles.size() && i < xs.size(); i++)
		if (xs[i] != layout.x[i] || ys[i] != layout.y[i] || pupus[i] != layout.pupu[i])
		{
			if (bad++ < 3)
				printf("  %s tile %zu: oracle %d,%d %08X ours %d,%d %08X\n", what, i, xs[i], ys[i], pupus[i], layout.x[i], layout.y[i], layout.pupu[i]);
		}
	CHECK(bad == 0);
}

static void TestParseAndStore()
{
	std::vector<Tile> tiles = { MakeTile(-160, -112, 3, 1, 5, 32, 48, 2, 1, 7, 2), MakeTile(0, 0, 0, 0, 0, 0, 0) };
	std::vector<uint8_t> bytes = Bytes(tiles);
	bytes.insert(bytes.end(), 32, 0xAB); //junk after the end marker
	const std::vector<Tile> parsed = ParseMap(bytes.data(), bytes.size());
	CHECK(parsed.size() == 2);
	CHECK(parsed[0].x == -160 && parsed[0].y == -112 && parsed[0].TexturePage() == 3 && parsed[0].Depth() == 1);
	CHECK(parsed[0].Palette() == 5 && parsed[0].srcX == 32 && parsed[0].srcY == 48 && parsed[0].LayerId() == 2);
	CHECK(parsed[0].blend == 1 && parsed[0].animId == 7 && parsed[0].animState == 2);
	StoreSource(bytes.data(), 9, 12, 240, 16);
	const std::vector<Tile> stored = ParseMap(bytes.data(), bytes.size());
	CHECK(stored[0].TexturePage() == 9 && stored[0].Palette() == 12 && stored[0].srcX == 240 && stored[0].srcY == 16);
	CHECK(stored[0].Depth() == 1 && (stored[0].texId & 0x10) && (stored[0].palId & 0x3F) == 30); //other bits kept
	CHECK(stored[0].x == -160 && stored[0].z == 100 && stored[0].animId == 7);
	CHECK(ParseMap(nullptr, 0).empty());
	CHECK(ParseMap(bytes.data(), 15).empty());
}

static void TestKnownLayout()
{
	//two tiles at the same spot, same pupu: the second gets +1; an off-grid tile gets bit 31
	const std::vector<Tile> tiles = { MakeTile(-16, -8, 0, 0, 0, 0, 0), MakeTile(-16, -8, 0, 0, 0, 16, 0), MakeTile(8, 0, 0, 0, 0, 32, 0, 3, 2) };
	const Layout l = ComputeLayout(tiles);
	CHECK(l.shiftX == 16 && l.shiftY == 8);
	CHECK(l.x[0] == 0 && l.y[0] == 0 && l.x[2] == 24 && l.y[2] == 8);
	CHECK(l.pupu[0] == 0x004FF000u && l.pupu[1] == 0x004FF001u);
	CHECK(l.pupu[2] == (0x80000000u | 3u << 24 | 0x00800000u | 2u << 20 | 0xFFu << 12));
	CHECK(PupuHex(l.pupu[0]) == "004FF000");
	CHECK(l.width == 24 + 0 + 16 && l.height == 8 + 0 + 16);
	//all positive: no shift
	const Layout p = ComputeLayout({ MakeTile(32, 16, 0, 0, 0, 0, 0), MakeTile(64, 48, 0, 0, 0, 0, 0) });
	CHECK(p.shiftX == 0 && p.shiftY == 0 && p.width == 64 + 32 + 16 && p.height == 48 + 16 + 16);
}

static void TestUnshare()
{
	std::mt19937 rng(7);
	for (int round = 0; round < 200; round++)
	{
		std::vector<Tile> tiles;
		std::uniform_int_distribution<int> cellD(0, 3), coin(0, 99), pageD(0, 2), palD(0, 1);
		const int n = 20 + round % 200;
		for (int i = 0; i < n; i++)
			tiles.push_back(MakeTile(i * 16, 0, pageD(rng), round % 3 == 0 ? 1 : 0, palD(rng), cellD(rng) * 16, cellD(rng) * 16));
		std::vector<bool> has(tiles.size());
		for (size_t i = 0; i < has.size(); i++)
			has[i] = coin(rng) < 70;
		const std::vector<Tile> before = tiles;
		const UnshareResult r = Unshare(tiles, has);
		std::vector<Tile> after = tiles;
		ApplyRelocations(after, r.moved);
		std::set<size_t> movedTiles;
		for (const Relocation& m : r.moved)
		{
			movedTiles.insert(m.tile);
			CHECK(has[m.tile]);
			CHECK(m.srcX % 16 == 0 && m.srcY % 16 == 0);
		}
		//a moved tile's new cell was used by no tile before
		std::set<std::tuple<int, int, int, int, int>> original;
		for (const Tile& t : before)
			original.insert({ t.TexturePage(), t.Depth(), t.Palette(), t.srcX, t.srcY });
		for (const Relocation& m : r.moved)
			CHECK(!original.count({ m.texturePage, before[m.tile].Depth(), m.palette, m.srcX, m.srcY }));
		//tiles with an image never share a cell with another tile with an image, unless unresolved
		std::map<std::tuple<int, int, int, int, int>, int> withImage;
		for (size_t i = 0; i < after.size(); i++)
			if (has[i])
				withImage[{ after[i].TexturePage(), after[i].Depth(), after[i].Palette(), after[i].srcX, after[i].srcY }]++;
		size_t extra = 0;
		for (const auto& [cell, count] : withImage)
			extra += count - 1;
		CHECK(extra <= r.unresolved);
		//nothing else changed; palettes are ones already used on the target page
		std::set<std::tuple<int, int, int>> pagePalettes;
		for (const Tile& t : before)
			pagePalettes.insert({ t.TexturePage(), t.Depth(), t.Palette() });
		for (size_t i = 0; i < after.size(); i++)
		{
			CHECK(after[i].x == before[i].x && after[i].Depth() == before[i].Depth() && after[i].layer == before[i].layer);
			if (!movedTiles.count(i))
				CHECK(after[i].srcX == before[i].srcX && after[i].Palette() == before[i].Palette());
			CHECK(pagePalettes.count({ after[i].TexturePage(), after[i].Depth(), after[i].Palette() }));
		}
	}
	//full page: nowhere to go
	std::vector<Tile> full;
	for (int c = 0; c < 256; c++)
		full.push_back(MakeTile(c * 16, 0, 0, 0, 0, (c % 16) * 16, (c / 16) * 16));
	full.push_back(MakeTile(0, 16, 0, 0, 0, 0, 0));
	const UnshareResult r = Unshare(full, std::vector<bool>(full.size(), true));
	CHECK(r.moved.empty() && r.unresolved == 1);
}

//--- PuPu's names and frames, and pictures in either tool's frame

static void TestPupuNames()
{
	//no off-grid flags and the blend byte as it is; at exactly the same spot the second tile gets +1, the third +2
	const std::vector<Tile> tiles = { MakeTile(-16, -8, 0, 0, 0, 0, 0), MakeTile(-16, -8, 0, 0, 0, 16, 0), MakeTile(-16, -8, 0, 0, 0, 32, 0),
		MakeTile(8, 0, 0, 0, 0, 48, 0, 3, 2), MakeTile(0, -8, 0, 0, 0, 64, 0) };
	const PupuNames p = ComputePupuNames(tiles);
	CHECK(p.name[0] == 0x004FF000u && p.name[1] == 0x004FF001u && p.name[2] == 0x004FF002u && p.name[4] == 0x004FF000u);
	CHECK(p.name[3] == (3u << 24 | 2u << 20 | 0xFFu << 12)); //off the grid both ways: no flags (FME has two)
	CHECK(ComputeLayout(tiles).pupu[3] == (0x80000000u | 3u << 24 | 0x00800000u | 2u << 20 | 0xFFu << 12));
	//one frame per layer number: its tiles' box and 0, 0
	CHECK(p.FrameOf(0x004FF001u) == (Frame{ -16, -8, 32, 24 }));
	CHECK(p.FrameOf(p.name[3]) == (Frame{ 0, 0, 24, 16 })); //0, 0 is in the box though no tile is
	//a tile off the frame's own grid moves the frame by its own amount
	const PupuNames far = ComputePupuNames({ MakeTile(40, 24, 0, 0, 0, 0, 0) });
	CHECK(far.FrameOf(far.name[0]) == (Frame{ 0, 0, 56, 40 }));
	//frames
	CHECK((Frame{ 0, 0, 336, 240 }).ScaleOf(1344, 960) == 4 && (Frame{ 0, 0, 336, 240 }).ScaleOf(1344, 961) == 0);
	CHECK((Frame{ 0, 0, 336, 240 }).ScaleOf(1344, 720) == 0 && (Frame{ 0, 0, 336, 240 }).ScaleOf(336 * 17, 240 * 17) == 0);
	CHECK((Frame{ -8, 0, 16, 16 }).Union(Frame{ 0, -16, 32, 16 }) == (Frame{ -8, -16, 40, 32 }));
}

//a field laid out like bdifrit1: layer 0 (the room, lava frames, a glow) 8 pixels off FME's grid, layers 1 and 2 (fog)
//reaching further left, right and up
static std::vector<Tile> FritLike()
{
	std::vector<Tile> tiles;
	int cell = 0;
	const auto add = [&](int x, int y, int layer, int blend, int anim, int state)
	{
		tiles.push_back(MakeTile(x, y, cell / 256 % 12, 1, 0, cell % 16 * 16, cell / 16 % 16 * 16, layer, blend, anim, state));
		cell++;
	};
	for (int y = -120; y <= 104; y += 16)
		for (int x = -168; x <= 152; x += 16)
		{
			add(x, y, 0, 4, 0xFF, 0);
			if ((x + y) % 64 == 0)
				add(x, y, 0, 4, 0xFF, 0); //an overlapping one: 004FF001
			if (y > 40 && x < 0)
				for (int s = 0; s < 3; s++)
					add(x, y, 0, 4, 0, s); //lava frames
			if (y < 0)
				add(x, y, 0, 1, 1, 0); //a glow
		}
	for (int y = -120; y <= 8; y += 16)
		for (int x = -200; x <= 152; x += 16)
			add(x, y, 1, 1, 2, 0);
	for (int y = -128; y <= -16; y += 16)
		for (int x = -168; x <= 184; x += 16)
			add(x, y, 2, 1, 3, 0);
	return tiles;
}

static void TestPictures()
{
	const std::vector<Tile> tiles = FritLike();
	const Layout fme = ComputeLayout(tiles);
	const PupuNames pupu = ComputePupuNames(tiles);
	//FME: one 400x248 canvas, and every layer 0 tile off its grid (the shift is 128 down)
	CHECK(fme.width == 400 && fme.height == 248 && fme.shiftX == 200 && fme.shiftY == 128);
	CHECK(fme.fme == (Frame{ -200, -128, 400, 248 }) && fme.canvas == fme.fme);
	CHECK(fme.pupu[0] == 0x00CFF000u && pupu.name[0] == 0x004FF000u);
	const Frame room{ -168, -120, 336, 240 }, fog1{ -200, -120, 368, 144 }, fog2{ -168, -128, 368, 144 };
	CHECK(pupu.FrameOf(0x004FF000u) == room && pupu.FrameOf(0x00101000u) == room && pupu.FrameOf(0x00400020u) == room);
	CHECK(pupu.FrameOf(0x01102000u) == fog1 && pupu.FrameOf(0x02103000u) == fog2);
	std::set<uint32_t> names(pupu.name.begin(), pupu.name.end());
	CHECK(names == (std::set<uint32_t>{ 0x00101000u, 0x00400000u, 0x00400010u, 0x00400020u, 0x004FF000u, 0x004FF001u, 0x01102000u, 0x02103000u }));

	//PuPu's files at 4x
	const auto pupuFiles = [&](const uint32_t name, int& w, int& h)
	{
		if (!names.count(name))
			return false;
		const Frame f = pupu.FrameOf(name);
		w = f.width * 4;
		h = f.height * 4;
		return true;
	};
	PictureSet set = FindPictures(tiles, fme, pupu, pupuFiles);
	size_t pictured = 0;
	for (const int64_t t : set.tile)
		pictured += t >= 0;
	CHECK(pictured == tiles.size() && set.pictures.size() == names.size() && set.unfit.empty());
	CHECK(set.pictures[0x02103000u].pupu && set.pictures[0x02103000u].frame == fog2 && set.pictures[0x02103000u].scale == 4);
	const Frame canvas = FieldFrame(tiles, set, fme.fme);
	CHECK(canvas == room); //the opaque tiles are all in layer 0
	const Layout framed = FramedLayout(tiles, fme, set, canvas);
	CHECK(framed.width == 336 && framed.height == 240 && framed.shiftX == 168 && framed.shiftY == 120);
	CHECK(framed.x[0] == 0 && framed.y[0] == 0 && framed.pupu[0] == 0x004FF000u && framed.canvas == room && framed.fme == fme.fme);
	//every file by the frame its size matches: the painting of a fog layer, a map at the field's size, an old FME-sized one
	Frame f;
	CHECK(FitPicture(framed, 0x02103000u, 1472, 576, f) == 4 && f == fog2);
	CHECK(FitPicture(framed, 0x02103000u, 1344, 960, f) == 4 && f == room);
	CHECK(FitPicture(framed, 0x004FF000u, 1600, 992, f) == 4 && f == fme.fme);
	CHECK(FitPicture(framed, 0x004FF000u, 2688, 1920, f) == 8 && f == room);
	CHECK(FitPicture(framed, 0x004FF000u, 1400, 1000, f) == 0);

	//a page from PuPu's pictures holds what a page from FME's holds: every texel the colour of its .map position
	const auto paint = [](const Frame& frame, const int scale)
	{
		Image image;
		image.width = frame.width * scale;
		image.height = frame.height * scale;
		image.rgba.resize(static_cast<size_t>(image.width) * image.height * 4);
		for (int y = 0; y < image.height; y++)
			for (int x = 0; x < image.width; x++)
			{
				const int mx = frame.left * scale + x, my = frame.top * scale + y;
				uint8_t* p = &image.rgba[(static_cast<size_t>(y) * image.width + x) * 4];
				p[0] = static_cast<uint8_t>(mx & 0xFF);
				p[1] = static_cast<uint8_t>(my & 0xFF);
				p[2] = static_cast<uint8_t>((mx >> 8 & 0xF) | (my >> 8 & 0xF) << 4);
				p[3] = 255;
			}
		return image;
	};
	std::vector<bool> has(tiles.size(), true);
	std::vector<Tile> moved = tiles;
	ApplyRelocations(moved, Unshare(moved, has).moved);
	const std::map<uint32_t, PagePlan> pupuPages = PlanPages(moved, framed, has), fmePages = PlanPages(moved, fme, has);
	CHECK(pupuPages.size() == fmePages.size() && !pupuPages.empty());
	std::map<uint32_t, Image> pupuImages, fmeImages;
	for (const auto& [name, picture] : set.pictures)
		pupuImages[name] = paint(picture.frame, 2);
	const Image fmeImage = paint(fme.fme, 2);
	size_t compared = 0, same = 0;
	for (const auto& [key, plan] : pupuPages)
	{
		std::vector<StampSource> a, b;
		for (const CellStamp& s : plan.stamps)
		{
			Frame frame;
			const int scale = FitPicture(framed, s.pupu, pupuImages[s.pupu].width, pupuImages[s.pupu].height, frame);
			a.push_back({ &pupuImages[s.pupu], scale, s.mapX - frame.left, s.mapY - frame.top, s.cellX, s.cellY });
		}
		for (const CellStamp& s : fmePages.at(key).stamps)
			b.push_back({ &fmeImage, 2, s.layerX, s.layerY, s.cellX, s.cellY });
		const Image pa = ComposePage(256, 256, 2, nullptr, 0, 0, false, a), pb = ComposePage(256, 256, 2, nullptr, 0, 0, false, b);
		compared++;
		same += pa.rgba == pb.rgba;
	}
	CHECK(compared > 0 && same == compared);

	//FME's files only: the same layout as ever
	const auto fmeFiles = [&](const uint32_t name, int& w, int& h)
	{
		if (std::find(fme.pupu.begin(), fme.pupu.end(), name) == fme.pupu.end())
			return false;
		w = 1600;
		h = 992;
		return true;
	};
	const PictureSet fmeSet = FindPictures(tiles, fme, pupu, fmeFiles);
	const Frame fmeCanvas = FieldFrame(tiles, fmeSet, fme.fme);
	const Layout same2 = FramedLayout(tiles, fme, fmeSet, fmeCanvas);
	CHECK(fmeCanvas == fme.fme && same2.x == fme.x && same2.y == fme.y && same2.pupu == fme.pupu && same2.width == fme.width
		&& same2.height == fme.height && same2.shiftX == fme.shiftX && same2.shiftY == fme.shiftY);
	CHECK(!fmeSet.pictures.begin()->second.pupu);

	//PuPu's room with FME's fog; with both tools' files for the room, PuPu's are used
	const auto mixed = [&](const uint32_t name, int& w, int& h)
	{
		if (name >> 24 == 0 && names.count(name))
			return pupuFiles(name, w, h);
		if (name == 0x00CFF000u || (name >> 24 != 0 && std::find(fme.pupu.begin(), fme.pupu.end(), name) != fme.pupu.end()))
		{
			w = 1600;
			h = 992;
			return true;
		}
		return false;
	};
	const PictureSet mix = FindPictures(tiles, fme, pupu, mixed);
	CHECK(mix.tile[0] == 0x004FF000 && mix.pictures.count(0x00CFF000u) == 0);
	CHECK(mix.pictures.count(0x01902000u) == 1 && !mix.pictures.at(0x01902000u).pupu && mix.pictures.at(0x01902000u).frame == fme.fme);
	CHECK(mix.pictures.count(0x02103000u) == 1 && mix.pictures.at(0x02103000u).frame == fme.fme); //a name both tools give: FME's size
	CHECK(FieldFrame(tiles, mix, fme.fme) == room);
	//a file that fits neither frame is left out and said why
	const auto wrong = [&](const uint32_t name, int& w, int& h)
	{
		if (name != 0x004FF000u)
			return pupuFiles(name, w, h);
		w = 1500;
		h = 900;
		return true;
	};
	const PictureSet bad = FindPictures(tiles, fme, pupu, wrong);
	CHECK(bad.unfit.count(0x004FF000u) == 1 && bad.tile[0] == -1 && bad.unfit.at(0x004FF000u).find("1500x900") == 0);
}

//PuPu's own code (tests/pupu_oracle): every tile's name and every picture's frame
static bool RunPupuOracle(const std::string& oracle, const std::vector<uint8_t>& bytes, std::vector<uint32_t>& names,
	std::map<uint32_t, Frame>& frames)
{
	const std::string mapPath = "/tmp/field_layers_test_pupu.map";
	{
		std::ofstream f(mapPath, std::ios::binary);
		f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}
	FILE* p = popen((oracle + " " + mapPath).c_str(), "r");
	if (!p)
		return false;
	char line[256];
	while (fgets(line, sizeof(line), p))
	{
		unsigned name = 0;
		int index = 0;
		Frame f;
		if (sscanf(line, "tile %d %x", &index, &name) == 2)
			names.push_back(name);
		else if (sscanf(line, "image %x %d %d %d %d", &name, &f.left, &f.top, &f.width, &f.height) == 5)
			frames[name] = f;
	}
	pclose(p);
	return !names.empty();
}

static void CompareWithPupu(const std::string& oracle, const std::vector<uint8_t>& bytes, const char* what)
{
	//PuPu reads (size / 16 - 1) tiles: the last 16 bytes are the end marker (a whole tile, as in the game's files)
	std::vector<uint8_t> terminated = bytes;
	if (terminated.size() % 16 != 0 || terminated.size() < 16 || !(terminated[terminated.size() - 16] == 0xFF && terminated[terminated.size() - 15] == 0x7F))
	{
		terminated = Bytes(ParseMap(bytes.data(), bytes.size()), false);
		terminated.push_back(0xFF);
		terminated.push_back(0x7F);
		terminated.resize(terminated.size() + 14, 0);
	}
	const std::vector<Tile> tiles = ParseMap(terminated.data(), terminated.size());
	const PupuNames mine = ComputePupuNames(tiles);
	std::vector<uint32_t> names;
	std::map<uint32_t, Frame> frames;
	CHECK(RunPupuOracle(oracle, terminated, names, frames));
	CHECK(names.size() == tiles.size());
	size_t bad = 0;
	for (size_t i = 0; i < names.size() && i < mine.name.size(); i++)
		bad += names[i] != mine.name[i];
	for (const auto& [name, frame] : frames)
		bad += mine.FrameOf(name) != frame;
	if (bad)
		printf("FAIL %s: %zu differences from PuPu\n", what, bad);
	CHECK(bad == 0);
}

//bdifrit1: the files PuPu made for it (names and sizes at 4x, as you sent them) and FME's canvas
static void TestBdifrit1(const std::vector<uint8_t>& bytes)
{
	const std::vector<Tile> tiles = ParseMap(bytes.data(), bytes.size());
	const PupuNames pupu = ComputePupuNames(tiles);
	const Layout fme = ComputeLayout(tiles);
	CHECK(tiles.size() == 1861 && fme.width == 400 && fme.height == 248);
	const std::map<uint32_t, std::pair<int, int>> files = {
		{ 0x004FF000u, { 1344, 960 } }, { 0x004FF001u, { 1344, 960 } }, { 0x004FF002u, { 1344, 960 } },
		{ 0x00400000u, { 1344, 960 } }, { 0x00400010u, { 1344, 960 } }, { 0x00400020u, { 1344, 960 } }, { 0x00400030u, { 1344, 960 } },
		{ 0x00400040u, { 1344, 960 } }, { 0x00400050u, { 1344, 960 } }, { 0x00400060u, { 1344, 960 } }, { 0x00400070u, { 1344, 960 } },
		{ 0x00101000u, { 1344, 960 } }, { 0x01102000u, { 1472, 576 } }, { 0x02103000u, { 1472, 576 } } };
	CHECK(std::set<uint32_t>(pupu.name.begin(), pupu.name.end()).size() == files.size());
	for (const auto& [name, size] : files)
		CHECK(pupu.FrameOf(name).width * 4 == size.first && pupu.FrameOf(name).height * 4 == size.second);
	const PictureSet set = FindPictures(tiles, fme, pupu, [&](const uint32_t name, int& w, int& h)
	{
		const auto it = files.find(name);
		if (it == files.end())
			return false;
		w = it->second.first;
		h = it->second.second;
		return true;
	});
	size_t pictured = 0;
	for (const int64_t t : set.tile)
		pictured += t >= 0;
	CHECK(pictured == tiles.size() && set.pictures.size() == files.size());
	CHECK(FieldFrame(tiles, set, fme.fme) == (Frame{ -168, -120, 336, 240 }));
	//layer 2 stops a row above 0, 0: PuPu's frame still takes in that row (the empty band at the bottom of 02103000)
	int lowest = -1000;
	for (size_t i = 0; i < tiles.size(); i++)
		if (pupu.name[i] == 0x02103000u)
			lowest = std::max<int>(lowest, tiles[i].y);
	CHECK(lowest == -16 && pupu.FrameOf(0x02103000u).top + pupu.FrameOf(0x02103000u).height == 16);
}

static void TestPlanAndCompose()
{
	//tile 0 and 1 share a cell, tile 2 has no image
	std::vector<Tile> tiles = { MakeTile(0, 0, 0, 0, 0, 0, 0), MakeTile(16, 0, 0, 0, 0, 0, 0), MakeTile(32, 0, 0, 0, 0, 16, 0) };
	std::vector<bool> has = { true, true, false };
	const UnshareResult r = Unshare(tiles, has);
	CHECK(r.moved.size() == 1 && r.moved[0].tile == 1 && r.moved[0].srcX == 32 && r.moved[0].srcY == 0);
	ApplyRelocations(tiles, r.moved);
	const Layout layout = ComputeLayout(tiles);
	const std::map<uint32_t, PagePlan> pages = PlanPages(tiles, layout, has);
	CHECK(pages.size() == 1);
	const PagePlan& page = pages.begin()->second;
	CHECK(page.stamps.size() == 2 && !page.complete && page.aligned);

	//image: 48x16 native at scale 2, each tile area a different colour
	Image image;
	image.width = 96;
	image.height = 32;
	image.rgba.assign(96 * 32 * 4, 0);
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 96; x++)
		{
			uint8_t* p = &image.rgba[(y * 96 + x) * 4];
			p[0] = static_cast<uint8_t>(x / 32 * 100);
			p[1] = static_cast<uint8_t>(y);
			p[2] = 7;
			p[3] = 255;
		}
	std::vector<uint8_t> base(256 * 256 * 4);
	for (size_t i = 0; i < base.size(); i += 4)
	{
		base[i] = 1; base[i + 1] = 2; base[i + 2] = 3; base[i + 3] = 4;
	}
	std::vector<StampSource> sources;
	for (const CellStamp& s : page.stamps)
		sources.push_back({ &image, 2, s.layerX, s.layerY, s.cellX, s.cellY });
	const Image out = ComposePage(256, 256, 2, base.data(), 256, 256, true, sources);
	CHECK(out.width == 512 && out.height == 512);
	auto at = [&](int x, int y) { return &out.rgba[(static_cast<size_t>(y) * out.width + x) * 4]; };
	CHECK(at(0, 0)[0] == 0 && at(31, 31)[1] == 31); //tile 0 at cell 0
	CHECK(at(64, 5)[0] == 100 && at(64, 5)[1] == 5); //tile 1 moved to cell x=32 (64 at scale 2)
	CHECK(at(32, 0)[0] == 3 && at(32, 0)[2] == 1 && at(32, 0)[3] == 4); //tile 2: the game's page (BGRA -> RGBA)
	//other scale: resampled inside the block only
	const Image half = ComposePage(256, 256, 1, nullptr, 0, 0, true, sources);
	CHECK(half.width == 256 && half.rgba[(0 * 256 + 32) * 4] == 100 && half.rgba[(0 * 256 + 32) * 4 + 3] == 255);
	CHECK(half.rgba[(0 * 256 + 16) * 4 + 3] == 0); //not stamped, no base
	//an RGBA base (swizzled HD page) at another size
	std::vector<uint8_t> hd(1024 * 1024 * 4, 0);
	for (size_t i = 0; i < hd.size(); i += 4)
	{
		hd[i] = 10; hd[i + 1] = 20; hd[i + 2] = 30; hd[i + 3] = 255;
	}
	const Image swizzled = ComposePage(256, 256, 2, hd.data(), 1024, 1024, false, sources);
	CHECK(swizzled.rgba[(0 * 512 + 32) * 4] == 10 && swizzled.rgba[(0 * 512 + 32) * 4 + 2] == 30); //tile 2 from the HD page
	CHECK(swizzled.rgba[(5 * 512 + 64) * 4] == 100); //stamps on top
}

static void TestLanguage()
{
	std::string f, l;
	SplitLanguage("bghall_1_fr", f, l);
	CHECK(f == "bghall_1" && l == "fr");
	SplitLanguage("bghall_1", f, l);
	CHECK(f == "bghall_1" && l.empty());
	SplitLanguage("bdenter1", f, l);
	CHECK(f == "bdenter1" && l.empty());
	SplitLanguage("test_xx", f, l);
	CHECK(f == "test_xx" && l.empty());
}

//--- MatchPalette: which .map palette a page texture of the game was made with

static std::vector<uint8_t> RandomMim(std::mt19937& rng)
{
	std::vector<uint8_t> mim(MIM_SIZE, 0);
	for (size_t i = MIM_TEXTURE; i < MIM_SIZE; i++)
		mim[i] = static_cast<uint8_t>(rng());
	for (int p = 0; p < 16; p++)
		for (int c = 0; c < 256; c++)
		{
			const uint16_t colour = c == 0 ? 0 : static_cast<uint16_t>(rng() & 0xFFFF);
			mim[MIM_PALETTES + (p * 256 + c) * 2] = static_cast<uint8_t>(colour & 0xFF);
			mim[MIM_PALETTES + (p * 256 + c) * 2 + 1] = static_cast<uint8_t>(colour >> 8);
		}
	return mim;
}

//what the game uploads: the page decoded with one palette, B G R A (or R G B A), at a scale
static std::vector<uint8_t> GamePage(const std::vector<uint8_t>& mim, int page, int depth, int palette, int scale, bool rgba, bool plainShift)
{
	const int size = 256 * scale;
	std::vector<uint8_t> out(static_cast<size_t>(size) * size * 4, 0);
	for (int y = 0; y < size; y++)
		for (int x = 0; x < size; x++)
		{
			const int c = MimTexel(mim.data(), mim.size(), page, depth, palette, x / scale, y / scale);
			uint8_t* d = out.data() + (static_cast<size_t>(y) * size + x) * 4;
			if (c <= 0)
				continue;
			const int r = c & 31, g = c >> 5 & 31, b = c >> 10 & 31;
			const auto conv = [plainShift](int v) { return static_cast<uint8_t>(plainShift ? v << 3 : (v << 3 | v >> 2)); };
			d[rgba ? 0 : 2] = conv(r);
			d[1] = conv(g);
			d[rgba ? 2 : 0] = conv(b);
			d[3] = (c & 0x8000) ? 0x7F : 0xFF;
		}
	return out;
}

static void CheckIdentified(const std::vector<uint8_t>& mim, const std::vector<Tile>& tiles, int page, int depth, int palette,
	int scale = 1, bool rgba = false, bool plainShift = false)
{
	const std::vector<uint8_t> pixels = GamePage(mim, page, depth, palette, scale, rgba, plainShift);
	const std::vector<PaletteMatch> m = MatchPalette(mim.data(), mim.size(), tiles, page, pixels.data(), 256 * scale, 256 * scale);
	CHECK(!m.empty());
	if (m.empty())
		return;
	CHECK(m[0].palette == (depth == 2 ? 0 : palette) && m[0].depth == depth && m[0].Score() == 1.0 && m[0].opaque > 0);
	if (m.size() > 1)
		CHECK(m[1].Score() < 0.9);
}

static void TestMatchPalette()
{
	std::mt19937 rng(77);
	std::vector<uint8_t> mim = RandomMim(rng);
	//MimTexel against the layout: 4-bit low nibble first, 8-bit, 16-bit
	mim[MIM_TEXTURE + 5 * MIM_ROW + 2 * 128 + 3] = 0xA7;
	mim[MIM_PALETTES + (4 * 256 + 7) * 2] = 0x34;
	mim[MIM_PALETTES + (4 * 256 + 7) * 2 + 1] = 0x12;
	CHECK(MimTexel(mim.data(), mim.size(), 2, 0, 4, 6, 5) == 0x1234);
	CHECK(MimTexel(mim.data(), mim.size(), 2, 1, 4, 3, 5) == (mim[MIM_PALETTES + (4 * 256 + 0xA7) * 2] | mim[MIM_PALETTES + (4 * 256 + 0xA7) * 2 + 1] << 8));
	CHECK(MimTexel(mim.data(), mim.size(), 12, 2, 0, 63, 0) >= 0);
	CHECK(MimTexel(mim.data(), mim.size(), 12, 2, 0, 64, 0) == -1); //past the last page
	CHECK(MimTexel(mim.data(), mim.size(), 0, 0, 0, 0, 256) == -1);

	std::vector<Tile> tiles;
	for (int i = 0; i < 40; i++)
		tiles.push_back(MakeTile(i * 16, 0, 0, 0, i % 3, (i % 16) * 16, (i / 16) * 16));
	for (int i = 0; i < 12; i++)
		tiles.push_back(MakeTile(i * 16, 16, 0, 1, 3, (i % 8) * 16, 128 + (i / 8) * 16));
	for (int i = 0; i < 6; i++)
		tiles.push_back(MakeTile(i * 16, 32, 2, 2, 5, i * 16, 0));
	for (int palette = 0; palette < 3; palette++)
		CheckIdentified(mim, tiles, 0, 0, palette);
	CheckIdentified(mim, tiles, 0, 1, 3);
	CheckIdentified(mim, tiles, 2, 2, 5);
	CheckIdentified(mim, tiles, 0, 0, 1, 2); //uploaded at 512x512
	CheckIdentified(mim, tiles, 0, 0, 2, 1, true); //R G B A order
	CheckIdentified(mim, tiles, 0, 1, 3, 1, false, true); //c << 3 conversion

	//two palettes with the same colours tie (the game side keeps slots apart)
	std::copy(mim.begin() + MIM_PALETTES + 1 * 512, mim.begin() + MIM_PALETTES + 2 * 512, mim.begin() + MIM_PALETTES + 2 * 512);
	const std::vector<uint8_t> pixels = GamePage(mim, 0, 0, 2, 1, false, false);
	const std::vector<PaletteMatch> m = MatchPalette(mim.data(), mim.size(), tiles, 0, pixels.data(), 256, 256);
	CHECK(m.size() >= 2 && m[0].Score() == 1.0 && m[1].Score() == 1.0 && m[0].palette == 1 && m[1].palette == 2);
	//nothing to compare
	CHECK(MatchPalette(mim.data(), mim.size(), tiles, 7, pixels.data(), 256, 256).empty());
	CHECK(MatchPalette(nullptr, 0, tiles, 0, pixels.data(), 256, 256).empty());
}

//every (page, palette, depth) a real .map uses, against a random .mim decoded the way the game does
static void TestMatchPaletteOnMap(const std::vector<uint8_t>& bytes)
{
	std::mt19937 rng(static_cast<unsigned>(bytes.size()));
	const std::vector<uint8_t> mim = RandomMim(rng);
	const std::vector<Tile> tiles = ParseMap(bytes.data(), bytes.size());
	std::set<std::tuple<int, int, int>> used;
	for (const Tile& t : tiles)
		if (t.Depth() <= 2)
			used.insert({ t.TexturePage(), t.Depth(), t.Depth() == 2 ? 0 : t.Palette() });
	for (const auto& [page, depth, palette] : used)
		CheckIdentified(mim, tiles, page, depth, palette);
}

//--- semi-transparent tiles take the game's alpha
static void TestSemiTransparency()
{
	//two tiles on page 0 palette 0: the second shares the first's cell, is moved to (16, 0) and is semi-transparent
	std::vector<Tile> shipped = { MakeTile(0, 0, 0, 0, 0, 0, 0, 0, 4), MakeTile(16, 0, 0, 0, 0, 0, 0, 1, 1) };
	std::vector<bool> has(2, true);
	const Layout layout = ComputeLayout(shipped);
	std::vector<Tile> tiles = shipped;
	const UnshareResult r = Unshare(tiles, has);
	ApplyRelocations(tiles, r.moved);
	auto plans = PlanPages(tiles, layout, has, &shipped);
	const PagePlan& plan = plans.begin()->second;
	CHECK(plan.stamps.size() == 2);
	CHECK(!plan.stamps[0].semi && plan.stamps[0].originX == 0 && plan.stamps[0].originY == 0);
	CHECK(plan.stamps[1].semi && plan.stamps[1].cellX == 16 && plan.stamps[1].originX == 0 && plan.stamps[1].originY == 0);
	CHECK(PlanPages(tiles, layout, has)[PageKey(0, 0)].stamps[1].originX == -1); //no shipped tiles given

	//the game's page: texel (3, 2) of the original cell is blended (alpha 127), the rest opaque
	std::vector<uint8_t> alpha(256 * 256, 255);
	alpha[2 * 256 + 3] = 127;
	Image page;
	page.width = page.height = 512; //2x
	page.rgba.assign(512 * 512 * 4, 255);
	const size_t changed = InheritSemiTransparency(page, 2, plan.stamps, alpha.data(), 256, 256);
	CHECK(changed == 4); //one game texel = 2x2 texels at 2x, only in the semi-transparent tile's cell
	CHECK(page.rgba[((2 * 2) * 512 + (16 * 2 + 3 * 2)) * 4 + 3] == 127);
	CHECK(page.rgba[((2 * 2 + 1) * 512 + (16 * 2 + 3 * 2 + 1)) * 4 + 3] == 127);
	CHECK(page.rgba[((2 * 2) * 512 + (3 * 2)) * 4 + 3] == 255); //the opaque tile's cell is left alone
	//transparent HD texels stay transparent
	page.rgba[((2 * 2) * 512 + (16 * 2 + 3 * 2)) * 4 + 3] = 0;
	CHECK(InheritSemiTransparency(page, 2, plan.stamps, alpha.data(), 256, 256) == 3);
}

int main(int argc, char** argv)
{
	TestParseAndStore();
	TestKnownLayout();
	TestUnshare();
	TestPlanAndCompose();
	TestLanguage();
	TestMatchPalette();
	TestSemiTransparency();
	TestPupuNames();
	TestPictures();
	const bool openviii = argc > 1 && std::string(argv[1]) != "-";
	const std::string pupuOracle = argc > 3 ? argv[3] : "";
	std::mt19937 rng(1234);
	for (int i = 0; i < 300 && (openviii || !pupuOracle.empty()); i++)
	{
		const std::vector<uint8_t> bytes = Bytes(RandomMap(rng));
		if (openviii)
			CompareWithOracle(argv[1], bytes, "random");
		if (!pupuOracle.empty())
			CompareWithPupu(pupuOracle, bytes, "random");
	}
	if (argc > 2)
		for (const auto& entry : std::filesystem::recursive_directory_iterator(argv[2]))
			if (entry.path().extension() == ".map")
			{
				std::ifstream f(entry.path(), std::ios::binary);
				std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
				if (openviii)
					CompareWithOracle(argv[1], bytes, entry.path().filename().string().c_str());
				if (!pupuOracle.empty())
					CompareWithPupu(pupuOracle, bytes, entry.path().filename().string().c_str());
				TestMatchPaletteOnMap(bytes);
				if (entry.path().filename() == "bdifrit1.map")
					TestBdifrit1(bytes);
			}
	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
