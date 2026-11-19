//Tests for fieldprep model (fieldprep/models.cpp): the field models' textures made into BC7 DDS files with mip levels.
//
//Build (from tests/):
//  g++ -std=c++20 -O2 -pthread fieldprep_model_test.cpp ../fieldprep/models.cpp ../fieldprep/image_io.cpp
//      ../ff8_demaster/field_pbr.cpp ../ff8_demaster/field_layers.cpp ../fieldprep/third_party/lodepng.cpp
//      ../fieldprep/third_party/bc7enc.cpp ../fieldprep/third_party/bc7decomp.cpp -o fieldprep_model_test
//Run: fieldprep_model_test <empty scratch folder>   (QUALITY=0-4 for another BC7 effort, VERBOSE=1 for each level's error)

#include "../fieldprep/image_io.h"
#include "../fieldprep/models.h"
#include "../ff8_demaster/field_pbr.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <thread>

using namespace fieldprep;
namespace fs = std::filesystem;

static int checks = 0, failures = 0;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); } } while (0)

static Image Make(const int w, const int h, const std::function<void(int, int, uint8_t*)>& f)
{
	Image im;
	im.width = w;
	im.height = h;
	im.rgba.resize(static_cast<size_t>(w) * h * 4);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++)
			f(x, y, im.rgba.data() + (static_cast<size_t>(y) * w + x) * 4);
	return im;
}

static uint8_t Hash(int x, int y, int salt)
{
	uint32_t v = static_cast<uint32_t>(x * 73856093) ^ static_cast<uint32_t>(y * 19349663) ^ static_cast<uint32_t>(salt * 83492791);
	v ^= v >> 13;
	v *= 0x5bd1e995;
	v ^= v >> 15;
	return static_cast<uint8_t>(v);
}

//a texture like a character's: smooth colour areas, fine detail, hair strands cut out (alpha 0 between them)
static Image Albedo(const int w, const int h, const bool hair)
{
	return Make(w, h, [&](int x, int y, uint8_t* p)
	{
		const double u = static_cast<double>(x) / w, v = static_cast<double>(y) / h;
		p[0] = static_cast<uint8_t>(std::clamp(120 + 80 * std::sin(u * 9) + (Hash(x, y, 1) & 15), 0.0, 255.0));
		p[1] = static_cast<uint8_t>(std::clamp(90 + 60 * std::cos(v * 7) + (Hash(x, y, 2) & 15), 0.0, 255.0));
		p[2] = static_cast<uint8_t>(std::clamp(70 + 50 * std::sin((u + v) * 5), 0.0, 255.0));
		p[3] = 255;
		if (hair && v < 0.4 && (x / 3) % 2 == 1) //strands 3 px wide, 3 px apart
			p[3] = 0;
	});
}

//a normal map of round bumps, green up
static Image Normals(const int w, const int h)
{
	return Make(w, h, [&](int x, int y, uint8_t* p)
	{
		const double cx = (x % 32) - 15.5, cy = (y % 32) - 15.5;
		const double r2 = (cx * cx + cy * cy) / (12.0 * 12.0);
		double nx = 0, ny = 0, nz = 1;
		if (r2 < 1)
		{
			nx = cx / 12.0 * 0.8;
			ny = -cy / 12.0 * 0.8; //image y down, normal y up
			nz = std::sqrt(std::max(0.0, 1 - nx * nx - ny * ny));
		}
		p[0] = static_cast<uint8_t>(std::lround((nx * 0.5 + 0.5) * 255));
		p[1] = static_cast<uint8_t>(std::lround((ny * 0.5 + 0.5) * 255));
		p[2] = static_cast<uint8_t>(std::lround((nz * 0.5 + 0.5) * 255));
		p[3] = 255;
	});
}

static Image Grey(const int w, const int h, const std::function<int(int, int)>& f)
{
	return Make(w, h, [&](int x, int y, uint8_t* p)
	{
		const uint8_t g = static_cast<uint8_t>(std::clamp(f(x, y), 0, 255));
		p[0] = p[1] = p[2] = g;
		p[3] = 255;
	});
}

struct DdsFile
{
	int width = 0, height = 0, mips = 0;
	uint32_t dxgi = 0;
	std::vector<Image> levels;
	bool ok = false;
};

static uint32_t U32(const uint8_t* p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | static_cast<uint32_t>(p[3]) << 24;
}

//reads a BC7 DDS with all its levels, the way a DDS reader walks it (each level's blocks after the last)
static DdsFile ReadDds(const fs::path& path)
{
	DdsFile d;
	std::ifstream f(path, std::ios::binary);
	std::vector<uint8_t> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
	if (b.size() < 148 || memcmp(b.data(), "DDS ", 4) != 0)
		return d;
	d.height = static_cast<int>(U32(b.data() + 12));
	d.width = static_cast<int>(U32(b.data() + 16));
	d.mips = static_cast<int>(U32(b.data() + 28));
	d.dxgi = U32(b.data() + 128);
	size_t offset = 148;
	int w = d.width, h = d.height;
	for (int level = 0; level < d.mips; level++)
	{
		const size_t bytes = static_cast<size_t>((w + 3) / 4) * ((h + 3) / 4) * 16;
		if (offset + bytes > b.size())
			return d;
		d.levels.push_back(DecodeBc7(std::vector<uint8_t>(b.begin() + offset, b.begin() + offset + bytes), w, h));
		offset += bytes;
		w = std::max(1, w / 2);
		h = std::max(1, h / 2);
	}
	d.ok = offset == b.size();
	return d;
}

static double MeanError(const Image& a, const Image& b, const bool alphaToo = true)
{
	if (a.width != b.width || a.height != b.height)
		return 1e9;
	double sum = 0;
	size_t n = 0;
	for (size_t i = 0; i < a.rgba.size(); i++)
	{
		if (!alphaToo && i % 4 == 3)
			continue;
		sum += std::abs(a.rgba[i] - b.rgba[i]);
		n++;
	}
	return n ? sum / n : 0;
}

static void TestPieces()
{
	//names
	std::string suffix;
	CHECK(ModelStem("d001_0.png", &suffix) == "d001_0" && suffix.empty());
	CHECK(ModelStem("d001_0_ORME.png", &suffix) == "d001_0" && suffix == "_ORME");
	CHECK(ModelStem("d001_0_n.png", &suffix) == "d001_0" && suffix == "_N");
	CHECK(ModelStem("o002_0_RM.png", &suffix) == "o002_0" && suffix == "_RM");
	CHECK(ModelStem("o002_0_AM.png", &suffix) == "o002_0" && suffix == "_AM");
	CHECK(ModelStem("wmset_vehicle_0.png", &suffix) == "wmset_vehicle_0" && suffix.empty());
	CHECK(ModelStem("d001_0_E", &suffix) == "d001_0" && suffix == "_E");

	//level sizes: halves down to 1x1, odd sizes rounded down
	{
		const std::vector<Image> l = DataLevels(Grey(300, 200, [](int x, int) { return x % 256; }));
		CHECK(l.size() == 9);
		CHECK(l[1].width == 150 && l[1].height == 100);
		CHECK(l[3].width == 37 && l[3].height == 25);
		CHECK(l[7].width == 2 && l[7].height == 1);
		CHECK(l[8].width == 1 && l[8].height == 1);
		CHECK(DataLevels(Grey(1024, 512, [](int, int) { return 0; })).size() == 11);
	}
	//data: plain averages per channel, each channel on its own (alpha too)
	{
		const Image im = Make(2, 2, [](int x, int y, uint8_t* p) { p[0] = x ? 255 : 0; p[1] = 100; p[2] = y ? 200 : 0; p[3] = (x + y) == 1 ? 0 : 255; });
		const std::vector<Image> l = DataLevels(im);
		CHECK(l.size() == 2);
		CHECK(std::abs(l[1].rgba[0] - 128) <= 1 && l[1].rgba[1] == 100 && l[1].rgba[2] == 100 && std::abs(l[1].rgba[3] - 128) <= 1);
	}
	//albedo: averages in linear light (black and white make 188, not 128); see-through pixels don't bleed
	{
		const Image checker = Make(2, 2, [](int x, int y, uint8_t* p) { const uint8_t v = (x + y) % 2 ? 255 : 0; p[0] = p[1] = p[2] = v; p[3] = 255; });
		const std::vector<Image> l = AlbedoLevels(checker, 0.44f);
		CHECK(std::abs(l[1].rgba[0] - 188) <= 1);
		const Image edge = Make(2, 2, [](int x, int, uint8_t* p) { if (x == 0) { p[0] = 200; p[1] = 20; p[2] = 20; p[3] = 255; } else { p[0] = 0; p[1] = 255; p[2] = 0; p[3] = 0; } });
		const std::vector<Image> e = AlbedoLevels(edge, 0.0f);
		CHECK(e[1].rgba[0] == 200 && e[1].rgba[1] == 20); //the hidden green doesn't show
		CHECK(std::abs(e[1].rgba[3] - 128) <= 1);
	}
	//cut-outs keep their coverage in the small levels; plain averaging thins them out
	{
		const Image hair = Albedo(256, 256, true);
		const double c0 = Coverage(hair, 0.44f);
		const std::vector<Image> kept = AlbedoLevels(hair, 0.44f), plain = AlbedoLevels(hair, 0.0f);
		double worstKept = 0, worstPlain = 0;
		for (size_t i = 1; i + 2 < kept.size(); i++) //down to 4x4
		{
			worstKept = std::max(worstKept, std::abs(Coverage(kept[i], 0.44f) - c0));
			worstPlain = std::max(worstPlain, std::abs(Coverage(plain[i], 0.44f) - c0));
		}
		printf("coverage: level 0 %.3f; small levels off by at most %.3f kept, %.3f averaged plainly\n", c0, worstKept, worstPlain);
		CHECK(worstKept < 0.07);
		CHECK(worstPlain > worstKept);
	}
	//normals: every level unit length, the bumps' tilt averaging out towards flat
	{
		const std::vector<Image> l = NormalLevels(Normals(128, 128));
		double worst = 0;
		for (size_t i = 1; i < l.size(); i++)
			for (size_t p = 0; p < l[i].rgba.size(); p += 4)
			{
				double len = 0;
				for (int c = 0; c < 3; c++)
				{
					const double v = l[i].rgba[p + c] / 255.0 * 2 - 1;
					len += v * v;
				}
				worst = std::max(worst, std::abs(std::sqrt(len) - 1));
				CHECK(l[i].rgba[p + 3] == 255);
			}
		CHECK(worst < 0.012);
		const Image& last = l.back();
		CHECK(last.width == 1 && std::abs(last.rgba[0] - 128) <= 2 && std::abs(last.rgba[1] - 128) <= 2 && last.rgba[2] >= 253);
	}
	//ORME: the four channels, plain where a map is missing; a coloured glow counts
	{
		const Image ao = Grey(4, 4, [](int, int) { return 200; }), e = Make(4, 4, [](int, int, uint8_t* p) { p[0] = 0; p[1] = 0; p[2] = 230; p[3] = 255; });
		const Image orme = PackOrme(4, 4, &ao, nullptr, nullptr, &e);
		CHECK(orme.rgba[0] == 200 && orme.rgba[1] == 179 && orme.rgba[2] == 0 && orme.rgba[3] == 230);
		const Image plain = PackOrme(1, 1, nullptr, nullptr, nullptr, nullptr);
		CHECK(plain.rgba[0] == 255 && plain.rgba[1] == 179 && plain.rgba[2] == 0 && plain.rgba[3] == 0);
	}
	//island padding: an island of colour on a see-through black background; what's outside takes the island's edge, in every
	//level, so the small levels' edge texels keep the island's colour (unpadded, black creeps in)
	{
		const auto inIsland = [](const int x, const int y) { return x >= 10 && x < 30 && y >= 10 && y < 30; };
		const Image island = Make(64, 64, [&](int x, int y, uint8_t* p)
		{
			const bool in = inIsland(x, y);
			p[0] = in ? 200 : 0; p[1] = in ? 60 : 0; p[2] = in ? 40 : 0; p[3] = in ? 255 : 0;
		});
		const std::vector<Image> padded = AlbedoLevels(island, 0.0f, true), plain = AlbedoLevels(island, 0.0f, false);
		CHECK(padded[0].rgba[(5 * 64 + 5) * 4] == 200 && padded[0].rgba[(5 * 64 + 5) * 4 + 3] == 0); //far outside: the colour, still see-through
		CHECK(plain[0].rgba[(5 * 64 + 5) * 4] == 0);
		CHECK(padded[0].rgba[(15 * 64 + 15) * 4 + 1] == 60); //inside: as it was
		//level 3 (8 x 8): texel 1 is made of 8..15, half outside the island
		const uint8_t* edge = padded[3].rgba.data() + (1 * 8 + 1) * 4;
		CHECK(std::abs(edge[0] - 200) <= 1 && std::abs(edge[1] - 60) <= 1 && edge[3] > 0);
		const std::vector<uint8_t> covered = IslandCoverage(island, island);
		CHECK(covered.size() == 64 * 64 && covered[15 * 64 + 15] == 1 && covered[5 * 64 + 5] == 0);
		//a map without see-through texels says nothing about islands
		const Image opaque = Grey(64, 64, [](int, int) { return 100; });
		CHECK(IslandCoverage(opaque, opaque).empty());
		CHECK(IslandCoverage(opaque, island).size() == covered.size()); //the image the game reads, when the albedo is opaque
		//ORME (data) and normals: padded from the albedo's islands
		const Image data = Make(64, 64, [&](int x, int y, uint8_t* p) { const bool in = inIsland(x, y); p[0] = p[1] = p[2] = p[3] = in ? 100 : 0; });
		const std::vector<Image> dPadded = DataLevels(data, covered), dPlain = DataLevels(data);
		const size_t at = (2 * 16 + 2) * 4; //level 2 (16 x 16): texel 2 is made of 8..11, half outside
		CHECK(dPadded[2].rgba[at] == 100 && dPadded[2].rgba[at + 3] == 100 && dPlain[2].rgba[at] < 80);
		const Image tilted = Make(64, 64, [&](int x, int y, uint8_t* p)
		{
			const bool in = inIsland(x, y);
			p[0] = in ? 192 : 0; p[1] = in ? 128 : 0; p[2] = in ? 238 : 0; p[3] = 255; //(0.5, 0, 0.87); outside: garbage
		});
		const std::vector<Image> nPadded = NormalLevels(tilted, covered), nPlain = NormalLevels(tilted);
		CHECK(std::abs(nPadded[2].rgba[at] - 192) <= 2 && std::abs(nPadded[2].rgba[at + 2] - 238) <= 2);
		CHECK(std::abs(nPlain[2].rgba[at] - 192) > 2 || std::abs(nPlain[2].rgba[at + 2] - 238) > 2);
	}
	//resizing: smaller by area, larger bilinear
	{
		const Image big = Grey(8, 8, [](int x, int) { return x < 4 ? 0 : 200; });
		const Image half = ResizeImage(big, 4, 4);
		CHECK(half.rgba[0] == 0 && half.rgba[3 * 4] == 200);
		const Image up = ResizeImage(half, 8, 8);
		CHECK(up.rgba[0] == 0 && up.rgba[7 * 4] == 200 && up.rgba[3 * 4] > 0 && up.rgba[3 * 4] < 200);
	}
}

static void TestFolder(const fs::path& dir, double& seconds1024)
{
	fs::remove_all(dir);
	fs::create_directories(dir);
	std::string error;
	const auto save = [&](const std::string& name, const Image& im) { CHECK(SavePng((dir / name).string(), im, error)); };
	//d001_0: everything, separate O R M E, its own albedo with hair
	save("d001_0.png", Albedo(256, 256, true));
	save("d001_0_A.png", Albedo(256, 256, true));
	save("d001_0_N.png", Normals(256, 256));
	save("d001_0_O.png", Grey(256, 256, [](int x, int y) { return 255 - ((x / 16 + y / 16) % 3) * 40; }));
	save("d001_0_R.png", Grey(256, 256, [](int x, int) { return x; }));
	save("d001_0_M.png", Grey(256, 256, [](int x, int y) { return (x < 128 && y < 128) ? 255 : 0; }));
	save("d001_0_E.png", Grey(256, 256, [](int x, int y) { return (x - 200) * (x - 200) + (y - 50) * (y - 50) < 100 ? 255 : 0; }));
	//o002_0: odd size, a normal map at twice its size (resized), a packed ORME
	save("o002_0.png", Albedo(300, 200, false));
	save("o002_0_N.png", Normals(600, 400));
	save("o002_0_ORME.png", Make(300, 200, [](int x, int y, uint8_t* p) { p[0] = 255; p[1] = static_cast<uint8_t>(x % 256); p[2] = 0; p[3] = static_cast<uint8_t>(y); }));
	//p003_0: no maps (left alone); x004_0: maps without the image (skipped)
	save("p003_0.png", Albedo(64, 64, false));
	save("x004_0_N.png", Normals(64, 64));

	ModelOptions o;
	o.threads = std::max(1u, std::thread::hardware_concurrency());
	if (const char* q = getenv("QUALITY"))
		o.quality = atoi(q);
	CHECK(Models(dir.string(), o) == 0);
	CHECK(fs::exists(dir / "d001_0.dds") && fs::exists(dir / "d001_0_N.dds") && fs::exists(dir / "d001_0_ORME.dds"));
	CHECK(fs::exists(dir / "o002_0.dds") && fs::exists(dir / "o002_0_N.dds") && fs::exists(dir / "o002_0_ORME.dds"));
	CHECK(!fs::exists(dir / "p003_0.dds") && !fs::exists(dir / "x004_0_N.dds"));
	CHECK(fs::exists(dir / "fieldprep_model.txt"));

	//the files: every level there, in the format said, close to what they're made from
	{
		Image a;
		CHECK(LoadRgba((dir / "d001_0_A.png").string(), a, error));
		const DdsFile d = ReadDds(dir / "d001_0.dds");
		CHECK(d.ok && d.mips == 9 && d.dxgi == 99 && d.width == 256 && d.height == 256);
		const std::vector<Image> want = AlbedoLevels(a, 0.44f);
		//BC7's error: about 2 levels in the big levels; the tiny ones (a whole 4x4 level in one block) can't hold this much detail
		double worst = 0;
		for (size_t i = 0; i < d.levels.size() && i < want.size(); i++)
		{
			if (std::min(want[i].width, want[i].height) >= 64)
				worst = std::max(worst, MeanError(d.levels[i], want[i]));
			if (getenv("VERBOSE"))
			{
				double rgb = 0, alpha = 0, hidden = 0;
				size_t nv = 0, nh = 0;
				for (size_t p = 0; p < want[i].rgba.size(); p += 4)
				{
					const bool seen = want[i].rgba[p + 3] > 0;
					for (int c = 0; c < 3; c++)
						(seen ? rgb : hidden) += std::abs(d.levels[i].rgba[p + c] - want[i].rgba[p + c]) / 3.0;
					alpha += std::abs(d.levels[i].rgba[p + 3] - want[i].rgba[p + 3]);
					(seen ? nv : nh)++;
				}
				printf("  level %zu %dx%d: rgb (seen) %.2f, rgb (hidden) %.2f, alpha %.2f\n", i, want[i].width, want[i].height, nv ? rgb / nv : 0,
					nh ? hidden / nh : 0, alpha / (nv + nh));
			}
		}
		printf("d001_0.dds: %d levels, BC7 sRGB, mean error in the levels of 64 px and more at most %.2f\n", d.mips, worst);
		CHECK(worst < 3.0);
	}
	{
		Image n;
		CHECK(LoadRgba((dir / "d001_0_N.png").string(), n, error));
		const DdsFile d = ReadDds(dir / "d001_0_N.dds");
		CHECK(d.ok && d.mips == 9 && d.dxgi == 98);
		const std::vector<Image> want = NormalLevels(n);
		CHECK(MeanError(d.levels[0], want[0]) < 2.0);
		CHECK(d.levels[0].rgba[3] == 255);
	}
	{
		const DdsFile d = ReadDds(dir / "d001_0_ORME.dds");
		CHECK(d.ok && d.mips == 9 && d.dxgi == 98);
		const Image& l0 = d.levels[0];
		//metallic 255 in the top left quarter, 0 elsewhere; roughness the x ramp; the glow's spot
		const auto at = [&](int x, int y, int c) { return static_cast<int>(l0.rgba[(static_cast<size_t>(y) * 256 + x) * 4 + c]); };
		CHECK(at(20, 20, 2) > 245 && at(200, 200, 2) < 10);
		CHECK(std::abs(at(100, 150, 1) - 100) <= 6);
		CHECK(at(200, 50, 3) > 240 && at(30, 200, 3) < 10);
		CHECK(at(5, 5, 0) > 245);
	}
	{
		const DdsFile d = ReadDds(dir / "o002_0_N.dds");
		CHECK(d.ok && d.width == 300 && d.height == 200 && d.mips == 9); //resized to the image
		const DdsFile orme = ReadDds(dir / "o002_0_ORME.dds");
		CHECK(orme.ok && orme.width == 300 && orme.mips == 9);
		//the packed file's alpha (emissive) is kept apart from its colour
		CHECK(std::abs(orme.levels[0].rgba[(static_cast<size_t>(150) * 300 + 10) * 4 + 3] - 150) <= 4);
		const DdsFile a = ReadDds(dir / "o002_0.dds");
		CHECK(a.ok && a.dxgi == 99); //from the image itself
	}

	//again: everything up to date; a changed map: only its DDS; --force: all
	{
		const auto timeOf = [&](const char* name) { return fs::last_write_time(dir / name); };
		const auto before = timeOf("d001_0_ORME.dds"), beforeN = timeOf("d001_0_N.dds"), beforeA = timeOf("d001_0.dds");
		std::this_thread::sleep_for(std::chrono::milliseconds(1100));
		CHECK(Models(dir.string(), o) == 0);
		CHECK(timeOf("d001_0_ORME.dds") == before && timeOf("d001_0_N.dds") == beforeN);
		save("d001_0_R.png", Grey(256, 256, [](int, int y) { return y; }));
		std::this_thread::sleep_for(std::chrono::milliseconds(1100));
		CHECK(Models((dir / "d001_0_R.png").string(), o) == 0); //one image, named by one of its maps
		CHECK(timeOf("d001_0_ORME.dds") != before && timeOf("d001_0_N.dds") == beforeN && timeOf("d001_0.dds") == beforeA);
		ModelOptions force = o;
		force.force = true;
		CHECK(Models((dir / "d001_0").string(), force) == 0); //no extension: d001_0.png
		CHECK(timeOf("d001_0_N.dds") != beforeN && timeOf("d001_0.dds") != beforeA);
	}
	//green down: flipped
	{
		Image n = Normals(64, 64);
		for (size_t i = 0; i < n.rgba.size(); i += 4)
			n.rgba[i + 1] = static_cast<uint8_t>(255 - n.rgba[i + 1]);
		save("q005_0.png", Albedo(64, 64, false));
		save("q005_0_N.png", n);
		ModelOptions flip = o;
		flip.flipGreen = true;
		CHECK(Models((dir / "q005_0.png").string(), flip) == 0);
		const DdsFile d = ReadDds(dir / "q005_0_N.dds");
		CHECK(d.ok && MeanError(d.levels[0], Normals(64, 64)) < 2.0);
	}
	//how long a 1024x1024 image with all its maps takes (one image: every thread on it)
	{
		const fs::path big = dir / "big";
		fs::create_directories(big);
		CHECK(SavePng((big / "c010_0.png").string(), Albedo(1024, 1024, true), error));
		CHECK(SavePng((big / "c010_0_N.png").string(), Normals(1024, 1024), error));
		CHECK(SavePng((big / "c010_0_R.png").string(), Grey(1024, 1024, [](int x, int y) { return (x ^ y) & 255; }), error));
		const auto start = std::chrono::steady_clock::now();
		CHECK(Models((big / "c010_0.png").string(), o) == 0);
		seconds1024 = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		printf("usage: fieldprep_model_test <scratch folder>\n");
		return 2;
	}
	TestPieces();
	double seconds = 0;
	TestFolder(fs::path(argv[1]), seconds);
	printf("one 1024x1024 image (albedo, normal, ORME): %.1f s with %u threads\n", seconds, std::max(1u, std::thread::hardware_concurrency()));
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
