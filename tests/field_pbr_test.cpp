//Tests for ff8_demaster/field_pbr.cpp (what DEMASTER and fieldprep agree on) and fieldprep's own parts (archives,
//lightmap split, depth fit, orientation, packing, BC7).
//  g++ -std=c++20 -O2 -pthread field_pbr_test.cpp ../ff8_demaster/field_pbr.cpp ../ff8_demaster/field_layers.cpp
//      ../fieldprep/prep.cpp ../fieldprep/image_io.cpp ../fieldprep/archive.cpp ../fieldprep/third_party/*.cpp -o field_pbr_test
//  ./field_pbr_test [folder with a real world.fl/fi/fs or field.fl/fi/fs, to read every entry]

#include "../ff8_demaster/field_layers.h"
#include "../ff8_demaster/field_pbr.h"
#include "../fieldprep/archive.h"
#include "../fieldprep/image_io.h"
#include "../fieldprep/prep.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <random>

using namespace fieldpbr;
using namespace fieldprep;

static int checks = 0, failures = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NEAR(a, b, tolerance) CHECK(std::fabs((a) - (b)) <= (tolerance))

static void TestColour()
{
	const float* table = SrgbTable();
	for (int i = 0; i < 256; i++)
		CHECK(LinearToSrgb8(table[i]) == i);
	for (int i = 1; i < 256; i++)
		CHECK(table[i] > table[i - 1]);
	for (const float scale : { 1.0f, 2.5f, 8.0f })
		for (float light = 0; light <= scale; light += scale / 97)
		{
			const float back = DecodeLightmap(EncodeLightmap(light, scale), scale);
			//one sRGB step at most
			CHECK(std::fabs(back - light) <= scale * 0.02f + 0.002f);
		}
	CHECK(EncodeLightmap(100.0f, 2.0f) == 255);
	CHECK(EncodeFlickerMask(0) == 255 && EncodeFlickerMask(1) == 0);
	NEAR(DecodeFlickerMask(EncodeFlickerMask(0.4f)), 0.4f, 0.003f);
}

static void TestSpace()
{
	for (const DepthModel model : { DepthModel::Linear, DepthModel::Inverse })
	{
		Space s;
		s.model = model;
		s.zoom = 412;
		s.originX = 168;
		s.originY = 120;
		s.a = model == DepthModel::Linear ? 9.0 : 0.1;
		s.b = model == DepthModel::Linear ? -7.0 : 0.37;
		for (double v = 0.05; v < 1; v += 0.1)
		{
			NEAR(s.Value(s.Distance(v)), v, 1e-9);
			const Vec3 p = s.ViewPosition(30.5, 200.25, v);
			NEAR(p.z, s.Distance(v), 1e-12);
			double x, y;
			CHECK(s.Project(p, x, y));
			NEAR(x, 30.5, 1e-9);
			NEAR(y, 200.25, 1e-9);
			NEAR(s.ViewLength(10, p.z), 10 * p.z / 412, 1e-12);
		}
	}
	const Space near = EstimatedSpace(10, 20, true), far = EstimatedSpace(10, 20, false);
	CHECK(near.Distance(1) < near.Distance(0));
	CHECK(far.Distance(0) < far.Distance(1));
	NEAR(near.Distance(1), 1.0, 1e-9);
	NEAR(near.Distance(0), 4.0, 1e-9);
	CHECK(near.originX == 10 && near.originY == 20 && !near.measured);
}

static void TestSettings()
{
	Settings s;
	s.space.measured = true;
	s.space.zoom = 512.5;
	s.space.originX = 160;
	s.space.originY = 112.5;
	s.space.model = DepthModel::Linear;
	s.space.a = 12.25;
	s.space.b = -9.5;
	s.space.nearest = 0.25;
	s.brightIsNear = false;
	s.normalYDown = true;
	s.lightmapScale = 2.75f;
	s.layerLightmapScale[0x00400010] = 1.5f;
	s.layerLightmapScale[0x004FF001] = 3.25f;
	s.hasCanvas = true;
	s.canvas[0] = -168; s.canvas[1] = -120; s.canvas[2] = 336; s.canvas[3] = 240;
	const std::string text = WriteSettings(s, "bvtr_1", "a note\nsecond line");
	Settings r;
	std::string warnings;
	CHECK(ParseSettings(text, r, warnings));
	CHECK(warnings.empty());
	CHECK(r.space.measured && r.space.model == DepthModel::Linear);
	NEAR(r.space.zoom, 512.5, 1e-9);
	NEAR(r.space.originY, 112.5, 1e-9);
	NEAR(r.space.a, 12.25, 1e-9);
	NEAR(r.space.b, -9.5, 1e-9);
	NEAR(r.space.nearest, 0.25, 1e-9);
	CHECK(!r.brightIsNear && r.normalYDown);
	NEAR(r.lightmapScale, 2.75f, 1e-4f);
	CHECK(r.layerLightmapScale.size() == 2);
	NEAR(r.LightmapScale(0x004FF001, true), 3.25f, 1e-4f);
	NEAR(r.LightmapScale(0x004FF001, false), 2.75f, 1e-4f);
	NEAR(r.LightmapScale(0x12345678, true), 2.75f, 1e-4f);
	//the canvas (the part of the .map the maps cover) comes back; a file without it has none
	CHECK(r.hasCanvas);
	CHECK(r.canvas[0] == -168 && r.canvas[1] == -120 && r.canvas[2] == 336 && r.canvas[3] == 240);
	{
		Settings old;
		std::string w;
		CHECK(ParseSettings("zoom = 300\norigin = 160 112\n", old, w));
		CHECK(!old.hasCanvas);
		Settings bad;
		w.clear();
		ParseSettings("canvas = 0 0 8 240\n", bad, w);
		CHECK(!bad.hasCanvas);
		CHECK(!w.empty());
	}
	//what the text doesn't say stays as the caller set it; nonsense is reported
	Settings d;
	d.space.originX = 99;
	warnings.clear();
	ParseSettings("zoom = 300\nwhat = 1\nzoom = banana\n", d, warnings);
	NEAR(d.space.zoom, 300, 1e-9);
	NEAR(d.space.originX, 99, 1e-9);
	CHECK(warnings.find("unknown setting what") != std::string::npos);
	CHECK(warnings.find("can't read zoom") != std::string::npos);
}

static void TestLights()
{
	Lights l;
	l.ambient[1] = 0.05f;
	for (int i = 0; i < 3; i++)
	{
		Light light;
		light.name = "lamp " + std::to_string(i);
		light.x = 10.5 + i;
		light.y = 20.25;
		light.depth = 0.375;
		light.color[0] = 1;
		light.color[1] = 0.5f;
		light.color[2] = 0.25f;
		light.intensity = 0.3f;
		light.range = 80;
		light.baked = i != 1;
		light.flicker = i == 2 ? Flicker::Fluorescent : Flicker::Fire;
		light.amount = 0.6f;
		light.speed = 1.5f;
		light.specular = 0.5f;
		light.note = "found";
		light.source = i == 0 ? LightSource::Found : i == 1 ? LightSource::Mark : LightSource::Yours;
		light.time = i == 0 ? LightTime::Day : LightTime::Both;
		l.lights.push_back(light);
	}
	l.emissive.hue = -40.5f;
	l.emissive.tint[2] = 0.25f;
	l.emissive.strength = 1.5f;
	const std::string text = WriteLights(l, "bvtr_1", "header");
	Lights r;
	std::string warnings;
	CHECK(ParseLights(text, r, warnings));
	CHECK(warnings.empty());
	CHECK(r.lights.size() == 3);
	NEAR(r.ambient[1], 0.05f, 1e-4f);
	//the glows' settings: written every time (so you see them), read back
	CHECK(r.hasEmissive && text.find("[emissive]") != std::string::npos);
	NEAR(r.emissive.hue, -40.5f, 0.05f);
	NEAR(r.emissive.tint[0], 1.0f, 1e-3f);
	NEAR(r.emissive.tint[2], 0.25f, 1e-3f);
	NEAR(r.emissive.strength, 1.5f, 1e-3f);
	{
		Lights e;
		std::string w;
		ParseLights("[emissive]\nhue = 90\nglow = 2\nstrength = -1\n[light]\nposition = 1 2 0.5\n", e, w);
		CHECK(e.hasEmissive && e.emissive.hue == 90.0f && e.emissive.strength == 1.0f && e.lights.size() == 1);
		CHECK(w.find("unknown setting glow in [emissive]") != std::string::npos && w.find("can't read strength") != std::string::npos);
		ParseLights("[light]\nposition = 1 2 0.5\n", e, w);
		CHECK(!e.hasEmissive && e.emissive.hue == 0.0f && e.emissive.strength == 1.0f);
	}
	for (size_t i = 0; i < r.lights.size() && i < 3; i++)
	{
		const Light& a = l.lights[i];
		const Light& b = r.lights[i];
		CHECK(a.name == b.name);
		NEAR(a.x, b.x, 0.05);
		NEAR(a.depth, b.depth, 1e-4);
		NEAR(a.color[1], b.color[1], 1e-3f);
		NEAR(a.intensity, b.intensity, 1e-3f);
		NEAR(a.range, b.range, 0.05f);
		CHECK(a.baked == b.baked && a.flicker == b.flicker);
		CHECK(a.source == b.source && a.time == b.time);
		NEAR(a.amount, b.amount, 1e-3f);
		NEAR(a.speed, b.speed, 1e-3f);
		NEAR(a.specular, b.specular, 1e-3f);
	}
	//more than DEMASTER uses at a time (20 of your own, all on by day: the file keeps them, the day uses 6), bad values, a
	//light without a name
	std::string many;
	for (int i = 0; i < 20; i++)
		many += "[light]\nposition = 1 2 0.5\nflicker = wobble\nrange = -3\n";
	warnings.clear();
	ParseLights(many, r, warnings);
	CHECK(r.lights.size() == 20);
	CHECK(r.lights[3].name == "light 4");
	CHECK(warnings.find("can't read flicker") != std::string::npos);
	CHECK(warnings.find("can't read range") != std::string::npos);
	CHECK(warnings.find("more than 6 of your own lights on by day") != std::string::npos);
	CHECK(warnings.find("of fieldprep's lights") == std::string::npos);
	{
		Lights day = r;
		KeepDayLights(day);
		CHECK(day.lights.size() == static_cast<size_t>(MAX_YOUR_LIGHTS));
	}
	//per time of day: 2 of fieldprep's and 6 of yours (marks count as yours); 6 + 6 of yours and 2 + 2 found, day and
	//night, is fine; a third found light by night warns; more than 32 aren't read
	{
		std::string halves;
		for (int i = 0; i < 12; i++)
			halves += std::string("[light]\nposition = 1 2 0.5\ntime = ") + (i < 6 ? "day" : "night") + (i % 3 == 0 ? "\nsource = mark" : "") + "\n";
		for (int i = 0; i < 4; i++)
			halves += std::string("[light]\nposition = 1 2 0.5\nsource = found\ntime = ") + (i < 2 ? "day" : "night") + "\n";
		Lights h;
		std::string w;
		ParseLights(halves, h, w);
		CHECK(h.lights.size() == 16 && w.empty());
		CHECK(h.lights[0].source == LightSource::Mark && h.lights[1].source == LightSource::Yours && h.lights[12].source == LightSource::Found);
		Lights day = h;
		KeepDayLights(day);
		CHECK(day.lights.size() == 8);
		ParseLights(halves + "[light]\nposition = 1 2 0.5\nsource = found\n", h, w);
		CHECK(w.find("more than 2 of fieldprep's lights on by night: only the first 2 are used") != std::string::npos);
		CHECK(w.find("more than 2 of fieldprep's lights on by day") != std::string::npos); //time left out: both
		day = h;
		KeepDayLights(day); //the third found one isn't used
		CHECK(day.lights.size() == 8 && std::count_if(day.lights.begin(), day.lights.end(), [](const Light& l) { return l.source == LightSource::Found; }) == 2);
		CHECK(!ParseLights("[light]\nsource = someone\n", h, w) || w.find("can't read source") != std::string::npos);
		std::string lots;
		for (int i = 0; i < 40; i++)
			lots += "[light]\nposition = 1 2 0.5\ntime = night\n";
		ParseLights(lots, h, w);
		CHECK(h.lights.size() == static_cast<size_t>(MAX_LIGHTS_IN_FILE) && w.find("only the first 32 are read") != std::string::npos);
		CHECK(!ParseLights("[light]\ntime = noon\n", h, w) || w.find("can't read time") != std::string::npos);
	}
	//flicker
	for (int f = 0; f <= 5; f++)
	{
		const Flicker flicker = static_cast<Flicker>(f);
		double low = 1e9, high = -1e9;
		for (double t = 0; t < 10; t += 0.013)
		{
			const double v = FlickerValue(flicker, 0.5f, 1.0f, 77, t);
			low = std::min(low, v);
			high = std::max(high, v);
			CHECK(v == FlickerValue(flicker, 0.5f, 1.0f, 77, t)); //the same every time
		}
		CHECK(low >= 0.5 - 1e-9 && high <= 1.5 + 1e-9);
		if (flicker == Flicker::None)
			CHECK(low == 1 && high == 1);
		else
		{
			CHECK(high - low > 0.2);
			if (high - low <= 0.2)
				printf("  flicker %s: %.3f to %.3f\n", FlickerName(flicker), low, high);
		}
	}
	CHECK(FlickerValue(Flicker::Fire, 0.5f, 1.0f, 1, 2.0) != FlickerValue(Flicker::Fire, 0.5f, 1.0f, 2, 2.0));
	CHECK(FlickerValue(Flicker::Fire, 0.0f, 1.0f, 1, 2.0) == 1.0);
	Light a, b;
	a.name = b.name = "x";
	CHECK(LightSeed(a, 0) != LightSeed(b, 1));
}

//a camera looking down at a floor, written the way the game stores it (axes x4096, position as Deling reads it)
static Camera MakeCamera(const Vec3& eye, const Vec3& look, const int zoom)
{
	const Vec3 f = Normalize(look);
	const Vec3 up0{ 0, 1, 0 };
	const Vec3 u = Normalize(up0 - f * Dot(f, up0));
	const Vec3 s = Cross(f, u);
	Camera c;
	const Vec3 axes[3] = { s, u * -1.0, f }; //axis Y is stored negated
	for (int i = 0; i < 3; i++)
	{
		c.axis[i][0] = static_cast<int16_t>(std::lround(axes[i].x * 4096));
		c.axis[i][1] = static_cast<int16_t>(std::lround(axes[i].y * 4096));
		c.axis[i][2] = static_cast<int16_t>(std::lround(axes[i].z * 4096));
	}
	//eye = -(p.x s + p.y u + p.z f), position = (p.x, -p.y, p.z) x 4096
	c.position[0] = static_cast<int32_t>(std::lround(-Dot(eye, s) * 4096));
	c.position[1] = static_cast<int32_t>(std::lround(Dot(eye, u) * 4096));
	c.position[2] = static_cast<int32_t>(std::lround(-Dot(eye, f) * 4096));
	c.zoom = static_cast<uint16_t>(zoom);
	return c;
}

static void TestCamera()
{
	const Vec3 eye{ 0.5, 1.5, -4 };
	const Camera c = MakeCamera(eye, { 0.1, -0.35, 1 }, 300);
	//bytes as in a .ca, and back
	uint8_t bytes[40] = {};
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			memcpy(bytes + (i * 3 + j) * 2, &c.axis[i][j], 2);
	memcpy(bytes + 18, &c.axis[2][2], 2);
	memcpy(bytes + 20, c.position, 12);
	memcpy(bytes + 36, &c.zoom, 2);
	memcpy(bytes + 38, &c.zoom, 2);
	const std::vector<Camera> parsed = ParseCameras(bytes, 40);
	CHECK(parsed.size() == 1);
	CHECK(parsed.size() == 1 && memcmp(parsed[0].axis, c.axis, sizeof(c.axis)) == 0 && parsed[0].zoom == 300 && parsed[0].position[2] == c.position[2]);
	CHECK(ParseCameras(bytes, 38).size() == 1);
	const CameraTransform t = MakeCameraTransform(c, 0);
	NEAR(Length(t.eye - eye), 0.0, 0.002);
	NEAR(Dot(t.right, t.down), 0.0, 1e-3);
	NEAR(Dot(t.right, t.forward), 0.0, 1e-3);
	//the eye looks along forward; a point below the eye and ahead is lower on screen (bigger y)
	const Vec3 ahead = t.ToView(t.eye + t.forward * 3.0);
	NEAR(ahead.x, 0.0, 1e-9);
	NEAR(ahead.y, 0.0, 1e-9);
	NEAR(ahead.z, 3.0, 1e-9);
	CHECK(t.ToView(t.eye + t.forward * 3.0 + Vec3{ 0, -0.5, 0 }).y > 0);
	//variants flip the screen axes
	const CameraTransform m = MakeCameraTransform(c, 1), u = MakeCameraTransform(c, 2);
	NEAR(Dot(m.right, t.right), -1.0, 1e-9);
	NEAR(Dot(u.down, t.down), -1.0, 1e-9);
	//walkmesh bytes
	std::vector<uint8_t> id(4 + 24 + 6);
	const uint32_t one = 1;
	memcpy(id.data(), &one, 4);
	const int16_t v[12] = { 4096, 0, 8192, 0, -4096, 0, 8192, 0, 0, 0, 12288, 0 };
	memcpy(id.data() + 4, v, 24);
	const Walkmesh w = ParseWalkmesh(id.data(), id.size());
	CHECK(w.triangles.size() == 1);
	if (!w.triangles.empty())
	{
		NEAR(w.triangles[0][0].x, 1.0, 1e-12);
		NEAR(w.triangles[0][0].z, 2.0, 1e-12);
		NEAR(w.triangles[0][2].z, 3.0, 1e-12);
	}
	CHECK(ParseWalkmesh(id.data(), 20).triangles.empty());
}

//a field whose depth map is made from the camera: fieldprep's fit must find the camera's distances
static void TestDepthFit()
{
	const Camera camera = MakeCamera({ 0, 1.5, -4 }, { 0, -0.35, 1 }, 300);
	const CameraTransform t = MakeCameraTransform(camera, 0);
	fieldlayers::Layout layout;
	layout.width = 160;
	layout.height = 120;
	layout.shiftX = 80;
	layout.shiftY = 60;
	const int scale = 2;
	Walkmesh mesh;
	for (int i = -3; i < 3; i++)
		for (int j = -1; j < 7; j++)
		{
			const Vec3 a{ double(i), 0, double(j) }, b{ i + 1.0, 0, double(j) }, c{ i + 1.0, 0, j + 1.0 }, d{ double(i), 0, j + 1.0 };
			mesh.triangles.push_back({ a, b, c });
			mesh.triangles.push_back({ a, c, d });
		}
	for (const DepthModel model : { DepthModel::Inverse, DepthModel::Linear })
	{
		Gray depth;
		depth.width = layout.width * scale;
		depth.height = layout.height * scale;
		depth.v.assign(static_cast<size_t>(depth.width) * depth.height, 0.0f);
		const double zn = 2.0, zf = 12.0;
		for (int y = 0; y < depth.height; y++)
			for (int x = 0; x < depth.width; x++)
			{
				//the floor seen through this pixel
				const double X = ((x + 0.5) / scale - layout.shiftX) / 300.0, Y = ((y + 0.5) / scale - layout.shiftY) / 300.0;
				const Vec3 dir = t.right * X + t.down * Y + t.forward;
				double z = zf;
				if (dir.y < -1e-6)
					z = std::clamp(-t.eye.y / dir.y, zn, zf);
				depth.v[static_cast<size_t>(y) * depth.width + x] = static_cast<float>(model == DepthModel::Inverse
					? (1 / z - 1 / zf) / (1 / zn - 1 / zf) : (zf - z) / (zf - zn));
			}
		const DepthFit fit = FitDepth(camera, mesh, depth, scale, layout);
		CHECK(fit.ok);
		CHECK(fit.variant == 0);
		CHECK(fit.space.model == model);
		CHECK(fit.space.measured && fit.space.zoom == 300 && fit.space.originX == 80);
		//distances it gives against the true ones
		for (const double z : { 3.0, 5.0, 8.0 })
		{
			const double v = model == DepthModel::Inverse ? (1 / z - 1 / zf) / (1 / zn - 1 / zf) : (zf - z) / (zf - zn);
			NEAR(fit.space.Distance(v), z, z * 0.01);
		}
		if (!fit.ok)
			printf("%s", fit.report.c_str());
	}
	fieldpbr::Walkmesh empty;
	CHECK(!FitDepth(camera, empty, Gray(), 1, layout).ok);
}

static void TestOrientation()
{
	//a floor (normal up) seen from above: nearer at the bottom of the picture; and a wall leaning right
	for (const bool brightNear : { true, false })
		for (const bool yDown : { false, true })
		{
			const int w = 128, h = 96;
			Gray depth;
			depth.width = w;
			depth.height = h;
			depth.v.resize(static_cast<size_t>(w) * h);
			Image normal;
			normal.width = w;
			normal.height = h;
			normal.rgba.resize(static_cast<size_t>(w) * h * 4);
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++)
				{
					const size_t i = static_cast<size_t>(y) * w + x;
					const bool floor = y > h / 2;
					//nearness: floor rises downwards; the left wall (normal pointing right) rises to the left
					const double nearness = floor ? 0.3 + 0.5 * y / h : 0.5 - 0.3 * x / w;
					depth.v[i] = static_cast<float>(brightNear ? nearness : 1 - nearness);
					double nx = floor ? 0 : 0.7, ny = floor ? 0.8 : 0, nz = 0.6; //x right, y up, z towards the camera
					if (yDown)
						ny = -ny;
					normal.rgba[i * 4] = static_cast<uint8_t>(std::lround((nx + 1) * 127.5));
					normal.rgba[i * 4 + 1] = static_cast<uint8_t>(std::lround((ny + 1) * 127.5));
					normal.rgba[i * 4 + 2] = static_cast<uint8_t>(std::lround((nz + 1) * 127.5));
					normal.rgba[i * 4 + 3] = 255;
				}
			const Orientation o = DetectOrientation(depth, normal, 1);
			CHECK(o.known);
			CHECK(o.brightIsNear == brightNear);
			CHECK(o.normalYDown == yDown);
			const Image up = NormalsYUp(normal, o.normalYDown);
			CHECK(up.rgba[(static_cast<size_t>(h - 2) * w) * 4 + 1] > 200); //floor's green points up now
		}
}

static void TestSplit()
{
	std::mt19937 random(5);
	const int w = 64, h = 48;
	Image painting;
	painting.width = w;
	painting.height = h;
	painting.rgba.resize(static_cast<size_t>(w) * h * 4);
	Rgb albedo;
	albedo.width = w;
	albedo.height = h;
	albedo.v.resize(static_cast<size_t>(w) * h * 3);
	for (size_t i = 0; i < static_cast<size_t>(w) * h; i++)
	{
		for (int c = 0; c < 3; c++)
		{
			//light between a third and three times the albedo; two pixels' albedo far too dark (they get raised)
			painting.rgba[i * 4 + c] = static_cast<uint8_t>(random() % 256);
			const float p = SrgbTable()[painting.rgba[i * 4 + c]];
			albedo.v[i * 3 + c] = i % 1500 == 7 ? p * 0.01f : std::min(1.0f, p / (0.33f + (random() % 1000) / 1000.0f * 2.67f));
		}
		painting.rgba[i * 4 + 3] = i % 17 == 0 ? 0 : 255;
	}
	const Split split = SplitLight(painting, albedo, 0);
	CHECK(split.scale >= 2.9f && split.scale <= 3.1f);
	CHECK(split.raised > 0);
	const float* table = SrgbTable();
	int worst = 0;
	for (size_t i = 0; i < static_cast<size_t>(w) * h; i++)
	{
		CHECK(split.albedo.rgba[i * 4 + 3] == painting.rgba[i * 4 + 3]);
		if (painting.rgba[i * 4 + 3] == 0)
			continue;
		for (int c = 0; c < 3; c++)
		{
			const float light = DecodeLightmap(EncodeLightmap(split.light.v[i * 3 + c], split.scale), split.scale);
			const int back = static_cast<int>(std::lround(LinearToSrgb(table[split.albedo.rgba[i * 4 + c]] * light) * 255));
			worst = std::max(worst, std::abs(back - painting.rgba[i * 4 + c]));
			CHECK(split.light.v[i * 3 + c] <= split.scale + 1e-5f);
		}
	}
	CHECK(worst <= 3); //8-bit albedo x 8-bit lightmap: the painting within a few levels (exact before the lightmap is stored)
	printf("  split: painting back within %d levels, scale %.2f, %zu pixels raised\n", worst, split.scale, split.raised);
	//the light again from a changed albedo still multiplies to the painting
	Image other = split.albedo;
	for (size_t i = 0; i < other.rgba.size(); i += 4)
		other.rgba[i] = static_cast<uint8_t>(std::min(255, other.rgba[i] + 3));
	const Rgb again = LightFrom(painting, other, split.scale);
	int worst2 = 0;
	for (size_t i = 0; i < static_cast<size_t>(w) * h; i++)
		if (painting.rgba[i * 4 + 3] && table[other.rgba[i * 4]] * split.scale >= table[painting.rgba[i * 4]])
		{
			const int back = static_cast<int>(std::lround(LinearToSrgb(table[other.rgba[i * 4]] * again.v[i * 3]) * 255));
			worst2 = std::max(worst2, std::abs(back - painting.rgba[i * 4]));
		}
	CHECK(worst2 <= 1);
	//mismatches
	Image composite = painting;
	composite.rgba[4 * 5] ^= 0x40;
	std::vector<uint8_t> mask;
	CHECK(Mismatches(painting, composite, &mask) == 1);
	CHECK(mask[5] == 1);
}

static void TestTint()
{
	Rgb bounced;
	bounced.width = 32;
	bounced.height = 32;
	bounced.v.resize(32 * 32 * 3);
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 32; x++)
		{
			float* p = bounced.At(x, y);
			const bool warm = x < 16;
			p[0] = warm ? 0.6f : 0.4f;
			p[1] = 0.5f;
			p[2] = warm ? 0.4f : 0.6f;
		}
	TintSettings settings;
	settings.strength = 1.0f;
	const Rgb tint = LightTint(bounced, 0, settings);
	CHECK(tint.At(2, 5)[0] > tint.At(2, 5)[2]); //warm half: red raised
	CHECK(tint.At(30, 5)[2] > tint.At(30, 5)[0]);
	NEAR(Luma(tint.At(2, 5)), 1.0f, 1e-3f);
	settings.strength = 0;
	const Rgb none = LightTint(bounced, 0, settings);
	NEAR(none.At(2, 5)[0], 1.0f, 1e-6f);
}

static void TestCompose()
{
	using fieldlayers::Tile;
	std::vector<Tile> tiles(3);
	for (Tile& t : tiles)
	{
		t.x = 0;
		t.y = 0;
		t.blend = 4;
		t.texId = 1 << 4;
	}
	tiles[0].z = 100;
	tiles[0].layer = 0;
	tiles[1].z = 50; //nearer: on top
	tiles[1].layer = 2;
	tiles[2].z = 10;
	tiles[2].layer = 4;
	tiles[2].blend = 1; //a glow: not in the composite
	tiles.push_back(tiles[0]);
	tiles[3].x = 16;
	tiles[3].animId = 3;
	tiles[3].animState = 1;
	tiles.push_back(tiles[3]);
	tiles[4].animState = 0;
	tiles[4].layer = 6;
	const fieldlayers::Layout layout = fieldlayers::ComputeLayout(tiles);
	std::map<uint32_t, Image> images;
	std::map<uint32_t, const Image*> pointers;
	for (size_t i = 0; i < tiles.size(); i++)
	{
		Image& image = images[layout.pupu[i]];
		image.width = layout.width;
		image.height = layout.height;
		image.rgba.assign(static_cast<size_t>(layout.width) * layout.height * 4, 0);
		for (int y = 0; y < 16; y++)
			for (int x = 0; x < 16; x++)
			{
				uint8_t* p = image.rgba.data() + (static_cast<size_t>(y) * layout.width + layout.x[i] + x) * 4;
				p[0] = static_cast<uint8_t>(10 + i);
				p[3] = 255;
			}
		pointers[layout.pupu[i]] = &image;
	}
	const Image out = ComposeOpaque(tiles, layout, pointers, 1);
	CHECK(out.rgba[0] == 11); //tile 1 (z 50) over tile 0 (z 100); the glow left out
	CHECK(out.rgba[16 * 4] == 14); //animation 3: its first state (tile 4), not state 1 (tile 3)
	const Gray glow = GlowLayers(tiles, layout, pointers, 1);
	CHECK(glow.v[0] > 0 && glow.v[16] == 0);
}

static void TestPacking()
{
	Rgb light;
	light.width = 4;
	light.height = 4;
	light.v.assign(48, 0.5f);
	Gray mask;
	mask.width = 4;
	mask.height = 4;
	mask.v.assign(16, 0.25f);
	const Image lf = PackLightmap(light, 2.0f, &mask);
	CHECK(lf.rgba[0] == EncodeLightmap(0.5f, 2.0f) && lf.rgba[3] == EncodeFlickerMask(0.25f));
	const Image steady = PackLightmap(light, 2.0f, nullptr);
	CHECK(steady.rgba[3] == 255);
	const Image orme = PackOrme(4, 4, nullptr, nullptr, nullptr, nullptr);
	CHECK(orme.rgba[0] == 255 && orme.rgba[1] == 204 && orme.rgba[2] == 0 && orme.rgba[3] == 0); //no emissive map: nothing glows
	//the emissive map from the residual: a bulb (the residual is nearly all of the painting), a wall the lamp lights (a
	//fifth of it), a dark corner where the residual is most of almost nothing
	{
		Rgb painting, residual;
		painting.width = residual.width = 3;
		painting.height = residual.height = 1;
		painting.v = { 0.9f, 0.8f, 0.6f, 0.5f, 0.4f, 0.3f, 0.004f, 0.004f, 0.004f };
		residual.v = { 0.85f, 0.75f, 0.5f, 0.1f, 0.08f, 0.06f, 0.004f, 0.004f, 0.004f };
		const Gray e = EmissiveFromResidual(painting, residual);
		CHECK(e.IsValid() && e.v[0] > 0.99f && e.v[1] == 0.0f && e.v[2] == 0.0f);
		//a lower bar: the wall glows a little
		const Gray low = EmissiveFromResidual(painting, residual, 0.1f);
		CHECK(low.v[1] > 0.1f && low.v[1] < 0.9f && low.v[2] == 0.0f);
		Rgb other = residual;
		other.width = 2;
		CHECK(!EmissiveFromResidual(painting, other).IsValid());
	}
	Image normal;
	normal.width = 4;
	normal.height = 4;
	normal.rgba.assign(64, 128);
	Gray depth = mask;
	const Image nd = PackNormalDepth(normal, depth);
	CHECK(nd.rgba[3] == 64 && nd.rgba[0] == 128);
	//padding: colour spreads under uncovered pixels, alpha set as asked
	Image a;
	a.width = 8;
	a.height = 1;
	a.rgba.assign(32, 0);
	a.rgba[0] = 200;
	a.rgba[3] = 255;
	Image coverage = a;
	PadUncovered(a, coverage, 3, 7);
	CHECK(a.rgba[4] == 200 && a.rgba[12] == 200 && a.rgba[16] == 0);
	CHECK(a.rgba[3] == 255 && a.rgba[7] == 7);
}

static void TestBc7AndFiles()
{
	const std::string folder = (std::filesystem::temp_directory_path() / "field_pbr_test").string();
	std::filesystem::create_directories(folder);
	Image image;
	image.width = 64;
	image.height = 32;
	image.rgba.resize(64 * 32 * 4);
	for (int y = 0; y < 32; y++)
		for (int x = 0; x < 64; x++)
		{
			uint8_t* p = image.rgba.data() + (static_cast<size_t>(y) * 64 + x) * 4;
			p[0] = static_cast<uint8_t>(x * 4);
			p[1] = static_cast<uint8_t>(y * 8);
			p[2] = 128;
			p[3] = x < 32 ? 255 : 0;
		}
	const std::vector<uint8_t> blocks = EncodeBc7(image, true, 2);
	CHECK(blocks.size() == 16 * 8 * 16);
	const Image back = DecodeBc7(blocks, 64, 32);
	int worst = 0;
	for (size_t i = 0; i < image.rgba.size(); i++)
		worst = std::max(worst, std::abs(image.rgba[i] - back.rgba[i]));
	CHECK(worst <= 8);
	std::string error;
	CHECK(SaveDds(folder + "/t.dds", 64, 32, blocks, error));
	Image read;
	CHECK(LoadRgba(folder + "/t.dds", read, error));
	CHECK(read.width == 64 && read.height == 32 && read.rgba == back.rgba);
	CHECK(SavePng(folder + "/t.png", image, error));
	CHECK(LoadRgba(folder + "/t.png", read, error) && read.rgba == image.rgba);
	//newest of png/dds wins
	std::filesystem::last_write_time(folder + "/t.png", std::filesystem::file_time_type::clock::now() - std::chrono::hours(1));
	CHECK(NewestImage(folder + "/t") == folder + "/t.dds");
	//16-bit grey keeps its precision
	Gray g;
	g.width = 3;
	g.height = 1;
	g.v = { 0.0f, 0.123456f, 1.0f };
	CHECK(SaveGrayPng(folder + "/g.png", g, true, error));
	Gray gr;
	CHECK(LoadGray(folder + "/g.png", gr, error) && gr.sixteen);
	NEAR(gr.v[1], 0.123456f, 1.0f / 65535);
	std::filesystem::remove_all(folder);
}

static void TestArchive(const char* real)
{
	//LZSS: flags byte, then literals / (offset, count) pairs over a 4 KB ring that starts at 4078
	//"ABCABCABCX": literals A B C (ring 4078-4080), then a copy of 6 from 4078, then X
	const std::vector<uint8_t> lzss = { 0x17, 'A', 'B', 'C', 0xEE, 0xF3, 'X' };
	const std::vector<uint8_t> text = Lzss(lzss.data(), lzss.size(), 10);
	CHECK(std::string(text.begin(), text.end()) == "ABCABCABCX");
	//LZ4 block: 3 literals, a match of 7 at offset 3, then 5 literals at the end
	const std::vector<uint8_t> lz4 = { 0x33, 'a', 'b', 'c', 3, 0, 0x50, 'v', 'w', 'x', 'y', 'z' };
	const std::vector<uint8_t> four = Lz4Block(lz4.data(), lz4.size(), 15);
	CHECK(std::string(four.begin(), four.end()) == "abcabcabcavwxyz");
	//an archive in memory with one entry of each kind
	std::vector<uint8_t> fs, fi;
	const auto put32 = [](std::vector<uint8_t>& v, const uint32_t x) { for (int i = 0; i < 4; i++) v.push_back(static_cast<uint8_t>(x >> (i * 8))); };
	const std::string plain = "plain data";
	put32(fi, static_cast<uint32_t>(plain.size()));
	put32(fi, static_cast<uint32_t>(fs.size()));
	put32(fi, 0);
	fs.insert(fs.end(), plain.begin(), plain.end());
	put32(fi, 10);
	put32(fi, static_cast<uint32_t>(fs.size()));
	put32(fi, 1);
	put32(fs, static_cast<uint32_t>(lzss.size()));
	fs.insert(fs.end(), lzss.begin(), lzss.end());
	put32(fi, 15);
	put32(fi, static_cast<uint32_t>(fs.size()));
	put32(fi, 2);
	put32(fs, static_cast<uint32_t>(lz4.size() + 8));
	fs.insert(fs.end(), { '4', 'Z', 'L', '_' });
	put32(fs, 15);
	fs.insert(fs.end(), lz4.begin(), lz4.end());
	const std::string list = "c:\\ff8\\data\\x\\field\\mapdata\\bv\\bvtr_1\\bvtr_1.MAP\r\nc:\\ff8\\a\\b.ca\r\nc:\\ff8\\a\\c.id\r\n";
	Archive archive;
	std::string error;
	CHECK(archive.OpenMemory(std::vector<uint8_t>(list.begin(), list.end()), fi, fs, error));
	CHECK(archive.Count() == 3);
	CHECK(archive.Find("bvtr_1.map") == 0 && archive.Find("B.CA") == 1 && archive.Find("d.id") == -1);
	std::vector<uint8_t> out;
	CHECK(archive.Extract(0, out, error) && std::string(out.begin(), out.end()) == plain);
	CHECK(archive.Extract(1, out, error) && std::string(out.begin(), out.end()) == "ABCABCABCX");
	CHECK(archive.Extract(2, out, error) && std::string(out.begin(), out.end()) == "abcabcabcavwxyz");
	CHECK(!archive.Extract(3, out, error));
	//a field with only per-language .map files (bvtr_3): images without a language take English's, the camera comes anyway
	{
		const auto pack = [&](const std::vector<std::pair<std::string, std::string>>& files, std::vector<uint8_t>& flOut,
			std::vector<uint8_t>& fiOut, std::vector<uint8_t>& fsOut)
		{
			flOut.clear();
			fiOut.clear();
			fsOut.clear();
			for (const auto& [name, data] : files)
			{
				const std::string line = "c:\\ff8\\data\\x\\field\\mapdata\\bv\\bvtr_3\\" + name + "\r\n";
				flOut.insert(flOut.end(), line.begin(), line.end());
				put32(fiOut, static_cast<uint32_t>(data.size()));
				put32(fiOut, static_cast<uint32_t>(fsOut.size()));
				put32(fiOut, 0);
				fsOut.insert(fsOut.end(), data.begin(), data.end());
			}
		};
		const auto write = [](const std::string& path, const std::vector<uint8_t>& data)
		{
			std::ofstream o(path, std::ios::binary);
			o.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
		};
		const auto make = [&](const std::string& folder, const std::vector<std::pair<std::string, std::string>>& innerFiles)
		{
			std::filesystem::create_directories(folder);
			std::vector<uint8_t> ifl, ifi, ifs, ofl, ofi, ofs;
			pack(innerFiles, ifl, ifi, ifs);
			pack({ { "bvtr_3.fl", std::string(ifl.begin(), ifl.end()) }, { "bvtr_3.fi", std::string(ifi.begin(), ifi.end()) },
				{ "bvtr_3.fs", std::string(ifs.begin(), ifs.end()) } }, ofl, ofi, ofs);
			write(folder + "/field.fl", ofl);
			write(folder + "/field.fi", ofi);
			write(folder + "/field.fs", ofs);
		};
		const std::string folder = (std::filesystem::temp_directory_path() / "fieldprep_langmap").string();
		make(folder, { { "bvtr_3_fr.map", "FRENCH" }, { "bvtr_3_en.map", "ENGLISH" }, { "bvtr_3.ca", "CAMERA" }, { "bvtr_3.id", "WALK" } });
		FieldFiles files;
		CHECK(LoadFromArchive(folder, "bvtr_3", "", files, error));
		CHECK(files.mapName == "bvtr_3_en.map" && std::string(files.map.begin(), files.map.end()) == "ENGLISH" && !files.note.empty());
		CHECK(std::string(files.ca.begin(), files.ca.end()) == "CAMERA" && std::string(files.id.begin(), files.id.end()) == "WALK");
		CHECK(LoadFromArchive(folder, "bvtr_3", "fr", files, error));
		CHECK(files.mapName == "bvtr_3_fr.map" && files.note.empty());
		CHECK(LoadFromArchive(folder, "bvtr_3", "de", files, error) && files.mapName == "bvtr_3_en.map" && !files.note.empty());
		//no .map at all: an error that lists what is there, and still the camera
		make(folder, { { "bvtr_3.ca", "CAMERA" }, { "bvtr_3.mim", "M" } });
		CHECK(!LoadFromArchive(folder, "bvtr_3", "", files, error));
		CHECK(error.find("bvtr_3.mim") != std::string::npos && std::string(files.ca.begin(), files.ca.end()) == "CAMERA");
		//a plain .map is still the first choice
		make(folder, { { "bvtr_3_en.map", "ENGLISH" }, { "bvtr_3.map", "PLAIN" } });
		CHECK(LoadFromArchive(folder, "bvtr_3", "", files, error) && files.mapName == "bvtr_3.map" && files.note.empty());
		std::filesystem::remove_all(folder);
	}
	//a real archive of the game: every entry comes out at the size the index says
	if (real != nullptr)
		for (const char* base : { "world", "field" })
		{
			const std::string stem = std::string(real) + "/" + base;
			if (!std::filesystem::exists(stem + ".fl"))
				continue;
			Archive game;
			CHECK(game.OpenFiles(stem + ".fl", stem + ".fi", stem + ".fs", error));
			size_t ok = 0, bad = 0;
			for (size_t i = 0; i < game.Count(); i++)
			{
				if (game.Extract(i, out, error))
					ok++;
				else
				{
					bad++;
					if (bad < 4)
						printf("  %s entry %zu (%s): %s\n", base, i, game.Name(i).c_str(), error.c_str());
				}
			}
			printf("  %s.fs: %zu of %zu entries read\n", base, ok, game.Count());
			CHECK(bad == 0 && ok > 0);
		}
}

//v15: the new settings and light keys, the light grid file
static void TestV15Files()
{
	Settings s;
	s.space.measured = true;
	s.gameDepth = 16384;
	s.occlusion[162] = 0.158;
	s.occlusion[131] = 0.128;
	s.cameraKnown = true;
	s.cameraEye = { 1, 2, 3 };
	s.cameraRight = { 1, 0, 0 };
	s.cameraDown = { 0, 0.8, -0.6 };
	s.cameraForward = { 0, 0.6, 0.8 };
	s.up = { 0, -0.8, 0.6 };
	s.skyValue = 0.02;
	s.probeNear = 0.05;
	s.probeFar = 0.9;
	s.sky = true;
	s.characterShadeKeep = true;
	s.characterLight = 1.5;
	const std::string text = WriteSettings(s, "bvtr_1", "test");
	Settings back;
	std::string warnings;
	CHECK(ParseSettings(text, back, warnings));
	CHECK(warnings.empty());
	NEAR(back.gameDepth, 16384.0, 1e-6);
	CHECK(back.occlusion.size() == 2);
	NEAR(back.occlusion[162], 0.158, 1e-9);
	CHECK(back.cameraKnown);
	NEAR(back.cameraDown.y, 0.8, 1e-9);
	NEAR(back.up.z, 0.6, 1e-6);
	NEAR(back.skyValue, 0.02, 1e-6);
	NEAR(back.probeFar, 0.9, 1e-9);
	CHECK(back.sky && back.characterShadeKeep);
	NEAR(back.characterLight, 1.5, 1e-6);
	CHECK(ParseSettings("character_light = auto\n", back, warnings) && back.characterLight == 0);
	//a file without them leaves the defaults
	Settings plain;
	CHECK(ParseSettings("zoom = 400\n", plain, warnings));
	CHECK(!plain.sky && !plain.characterShadeKeep && plain.occlusion.empty() && plain.gameDepth == GAME_DEPTH_PER_VIEW);
	Settings bad;
	warnings.clear();
	ParseSettings("character_shade = maybe\nocclusion.5000 = 1\n", bad, warnings);
	CHECK(warnings.find("character_shade") != std::string::npos && warnings.find("occlusion.5000") != std::string::npos);

	Lights lights;
	warnings.clear();
	CHECK(ParseLights("[light]\nname = lamp\nshadow = 0.7\nfollows = 0A\non_states = 1, 2,3\n[light]\nname = torch\nbaked = 1\n", lights, warnings));
	CHECK(warnings.empty());
	CHECK(lights.lights.size() == 2);
	NEAR(lights.lights[0].shadow, 0.7f, 1e-6);
	CHECK(lights.lights[0].follows == 10);
	CHECK(lights.lights[0].onStates.size() == 3 && lights.lights[0].onStates[2] == 3);
	NEAR(lights.lights[1].ShadowStrength(), 0.5f, 1e-6); //painted, left out
	lights.lights[1].baked = false;
	NEAR(lights.lights[1].ShadowStrength(), 1.0f, 1e-6);
	CHECK(lights.lights[0].OwnsMask()); //baked by default and follows a group
	Lights again;
	CHECK(ParseLights(WriteLights(lights, "bvtr_1", ""), again, warnings));
	CHECK(again.lights[0].follows == 10 && again.lights[0].onStates.size() == 3);
	NEAR(again.lights[0].shadow, 0.7f, 1e-3);
	CHECK(again.lights[1].shadow < 0 && again.lights[1].follows < 0);

	//half floats
	for (const float f : { 0.0f, 1.0f, -2.5f, 0.000123f, 65504.0f, 3.14159f, 1e-6f })
		NEAR(HalfToFloat(FloatToHalf(f)), f, std::fabs(f) * 0.001f + 1e-7f);
	//the grid through its file
	ProbeGrid g;
	g.nx = 5;
	g.ny = 3;
	g.nz = 4;
	g.data.resize(static_cast<size_t>(g.nx) * g.ny * g.nz * 16);
	for (size_t i = 0; i < g.data.size(); i++)
		g.data[i] = static_cast<float>(i % 97) / 13.0f - 2.0f;
	const std::vector<uint8_t> file = WriteProbes(g);
	CHECK(file.size() == 148 + g.data.size() * 2);
	ProbeGrid read;
	CHECK(ReadProbes(file.data(), file.size(), read));
	CHECK(read.nx == 5 && read.ny == 3 && read.nz == 4);
	NEAR(read.At(2, 4, 1, 3)[1], g.At(2, 4, 1, 3)[1], 0.01f);
	NEAR(read.At(0, 0, 0, 0)[3], g.At(0, 0, 0, 0)[3], 0.01f);
	CHECK(!ReadProbes(file.data(), 100, read));
}


//inverse lighting: a box room lit by two lamps and an ambient; fieldprep must find the two lamps where they are, few extras,
//and follow your marks (green adds, red keeps away)
static void TestFindLights()
{
	const int W = 224, H = 160, scale = 2;
	Space space;
	space.measured = true;
	space.model = DepthModel::Linear;
	space.zoom = 260;
	space.originX = W / 2.0;
	space.originY = H / 2.0;
	space.a = 0.5;
	space.b = -0.4;
	space.nearest = 0.01;
	struct Lamp
	{
		Vec3 at;
		double strength;
		float color[3];
	};
	//two large lamps (a ceiling light, a wall light) and a small one close to the back wall (an orange sconce)
	const Lamp lamps[3] = { { { 0.05, -0.09, 0.40 }, 0.004, { 1.0f, 0.9f, 0.8f } }, { { -0.11, 0.03, 0.36 }, 0.0025, { 0.6f, 0.8f, 1.0f } },
		{ { 0.06, 0.02, 0.442 }, 0.0006, { 1.0f, 0.5f, 0.3f } } };
	Gray depth;
	depth.width = W * scale;
	depth.height = H * scale;
	depth.v.assign(static_cast<size_t>(depth.width) * depth.height, 0.0f);
	Image normal;
	normal.width = depth.width;
	normal.height = depth.height;
	normal.rgba.assign(static_cast<size_t>(normal.width) * normal.height * 4, 255);
	Rgb light;
	light.width = depth.width;
	light.height = depth.height;
	light.v.assign(static_cast<size_t>(light.width) * light.height * 3, 0.0f);
	for (int y = 0; y < depth.height; y++)
		for (int x = 0; x < depth.width; x++)
		{
			const double cx = (x + 0.5) / scale, cy = (y + 0.5) / scale;
			const Vec3 dir{ (cx - space.originX) / space.zoom, (cy - space.originY) / space.zoom, 1.0 };
			//floor y = 0.12, ceiling y = -0.12, walls x = -/+0.15, back z = 0.45
			double best = 1e9;
			Vec3 n{ 0, 0, -1 };
			const auto plane = [&](const double t, const Vec3& normalHere)
			{
				if (t > 1e-6 && t < best)
				{
					best = t;
					n = normalHere;
				}
			};
			if (dir.y > 0) plane(0.12 / dir.y, { 0, -1, 0 });
			if (dir.y < 0) plane(-0.12 / dir.y, { 0, 1, 0 });
			if (dir.x < 0) plane(-0.15 / dir.x, { 1, 0, 0 });
			if (dir.x > 0) plane(0.15 / dir.x, { -1, 0, 0 });
			plane(0.45, { 0, 0, -1 });
			const Vec3 p = dir * best;
			const size_t i = static_cast<size_t>(y) * depth.width + x;
			depth.v[i] = static_cast<float>(space.Value(p.z));
			normal.rgba[i * 4 + 0] = static_cast<uint8_t>(std::lround((n.x + 1.0) * 127.5));
			normal.rgba[i * 4 + 1] = static_cast<uint8_t>(std::lround((1.0 - n.y) * 127.5));
			normal.rgba[i * 4 + 2] = static_cast<uint8_t>(std::lround((1.0 - n.z) * 127.5));
			for (int c = 0; c < 3; c++)
				light.v[i * 3 + c] = 0.35f;
			for (const Lamp& lamp : lamps)
			{
				const Vec3 d = lamp.at - p;
				const double r = Length(d), facing = Dot(n, d);
				if (facing > 0)
					for (int c = 0; c < 3; c++)
						light.v[i * 3 + c] += static_cast<float>(lamp.strength * lamp.color[c] * facing / (r * r * r));
			}
		}
	LightSearch search;
	search.threads = 4;
	std::string report;
	const auto start = std::chrono::steady_clock::now();
	const Lights found = FindLights(light, &normal, depth, space, scale, nullptr, search, report);
	printf("  inverse lighting: %.2f s\n%s", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(), report.c_str());
	CHECK(found.lights.size() == 2);
	//the found light nearest a lamp (prefix: "light" found, "your light" marked)
	const auto nearest = [&](const Lights& lights, const Vec3& at, const char* prefix, const Light** which)
	{
		double best = 1e9;
		for (const Light& l : lights.lights)
			if (l.name.rfind(prefix, 0) == 0)
			{
				const double d = Length(space.ViewPosition(l.x, l.y, l.depth) - at);
				if (d < best)
				{
					best = d;
					if (which != nullptr)
						*which = &l;
				}
			}
		return best;
	};
	//the two large lamps, within a tenth of their distance, with their colours (warm, cool), on, fieldprep's; the small
	//sconce is left to the emissive texture and your own lights
	for (int k = 0; k < 2; k++)
	{
		const Light* l = nullptr;
		CHECK(nearest(found, lamps[k].at, "light", &l) < 0.1 * lamps[k].at.z);
		if (l == nullptr)
			continue;
		CHECK(k == 1 ? l->color[2] > l->color[0] : l->color[0] > l->color[2]);
		CHECK(l->intensity == 0.2f && l->ShadowStrength() == 0.5f && l->source == LightSource::Found && l->time == LightTime::Day);
	}
	CHECK(nearest(found, lamps[2].at, "light", nullptr) > 0.1 * lamps[2].at.z);
	CHECK(report.find("kept the 2 largest") != std::string::npos);
	for (const Light& l : found.lights)
		CHECK(l.baked && l.range >= 16 && l.range <= 320);
	//--max-lights 1: the one reaching furthest (the strong warm lamp)
	{
		LightSearch one = search;
		one.maxLights = 1;
		std::string unused;
		const Lights single = FindLights(light, &normal, depth, space, scale, nullptr, one, unused);
		CHECK(single.lights.size() == 1 && nearest(single, lamps[0].at, "light", nullptr) < 0.1 * lamps[0].at.z);
	}
	//relit: your own lights from the old file stay, fieldprep's and your marks' are found again; a file from before sources
	//keeps nothing (all of it was fieldprep's then)
	{
		Lights old = found;
		Light mine;
		mine.name = "my candle";
		mine.source = LightSource::Yours;
		old.lights.push_back(mine);
		Light mark = mine;
		mark.source = LightSource::Mark;
		old.lights.push_back(mark);
		Lights again = found;
		CHECK(KeepYourLights(again, old, true) == 1 && again.lights.size() == found.lights.size() + 1 && again.lights.back().name == "my candle");
		again = found;
		CHECK(KeepYourLights(again, old, false) == 0 && again.lights.size() == found.lights.size());
	}
	//your marks: a lamp where nothing is (green), none at the second lamp (red)
	Image marks;
	marks.width = W;
	marks.height = H;
	marks.rgba.assign(static_cast<size_t>(W) * H * 4, 0);
	for (size_t i = 0; i < static_cast<size_t>(W) * H; i++)
		marks.rgba[i * 4 + 3] = 255;
	const auto paint = [&](const double cx, const double cy, const int radius, const uint8_t r, const uint8_t g)
	{
		for (int y = static_cast<int>(cy) - radius; y <= static_cast<int>(cy) + radius; y++)
			for (int x = static_cast<int>(cx) - radius; x <= static_cast<int>(cx) + radius; x++)
				if (x >= 0 && y >= 0 && x < W && y < H)
				{
					uint8_t* p = marks.rgba.data() + (static_cast<size_t>(y) * W + x) * 4;
					p[0] = r;
					p[1] = g;
					p[2] = 0;
				}
	};
	double lx = 0, ly = 0;
	space.Project(lamps[1].at, lx, ly);
	paint(lx, ly, 10, 255, 0);
	paint(190, 120, 1, 0, 255);
	const LightMarks read = ReadLightMarks(marks, W, H);
	CHECK(read.add.size() == 1);
	CHECK(read.removePixels > 0);
	report.clear();
	const Lights marked = FindLights(light, &normal, depth, space, scale, &read, search, report);
	printf("%s", report.c_str());
	CHECK(!marked.lights.empty() && marked.lights[0].name.rfind("your light", 0) == 0 && marked.lights[0].source == LightSource::Mark);
	CHECK(std::count_if(marked.lights.begin(), marked.lights.end(), [](const Light& l) { return l.source == LightSource::Found; }) <= 2);
	if (!marked.lights.empty())
	{
		NEAR(marked.lights[0].x, 190.5, 1.0);
		NEAR(marked.lights[0].y, 120.5, 1.0);
		CHECK(marked.lights[0].intensity == 0.2f && marked.lights[0].ShadowStrength() == 0.5f); //yours are on, however small
	}
	CHECK(nearest(marked, lamps[0].at, "light", nullptr) < 0.1 * lamps[0].at.z);
	//none on the red mark
	for (const Light& l : marked.lights)
		CHECK(std::hypot(l.x - lx, l.y - ly) > 10.0);
	//the colours on black: green a day lamp, blue a night lamp, cyan both, red none; others ignored; each spot one lamp
	{
		Image colours;
		colours.width = W;
		colours.height = H;
		colours.rgba.assign(static_cast<size_t>(W) * H * 4, 0);
		for (size_t i = 0; i < static_cast<size_t>(W) * H; i++)
			colours.rgba[i * 4 + 3] = 255;
		const auto dot = [&](const int cx, const int cy, const uint8_t r, const uint8_t g, const uint8_t b)
		{
			for (int y = cy; y < cy + 3; y++)
				for (int x = cx; x < cx + 3; x++)
				{
					uint8_t* p = colours.rgba.data() + (static_cast<size_t>(y) * W + x) * 4;
					p[0] = r;
					p[1] = g;
					p[2] = b;
				}
		};
		dot(10, 10, 0, 255, 0);
		dot(13, 10, 0, 0, 255); //touching the green one: a spot of its own colour
		dot(40, 20, 0, 255, 255);
		dot(70, 30, 255, 0, 0);
		dot(90, 40, 255, 255, 0); //yellow: not a mark
		dot(110, 50, 128, 128, 128); //grey: not a mark
		const LightMarks m = ReadLightMarks(colours, W, H);
		CHECK(m.add.size() == 1 && m.addNight.size() == 1 && m.addBoth.size() == 1 && m.removePixels == 9);
		if (m.add.size() == 1 && m.addNight.size() == 1 && m.addBoth.size() == 1)
		{
			NEAR(m.add[0].first, 11.5, 0.01);
			NEAR(m.addNight[0].first, 14.5, 0.01);
			NEAR(m.addBoth[0].second, 21.5, 0.01);
		}
		CHECK(m.Lamps(LightTime::Day).size() == 2 && m.Lamps(LightTime::Night).size() == 2);
	}

	//the night: _RESM-like light from the sconce and a lamp that only shines at night, nearly black around them. The night
	//search finds those two; merged with the day's (the two large lamps, the sconce left out by day), none is on both
	{
		const Lamp nightLamps[2] = { lamps[2], { { -0.06, -0.02, 0.43 }, 0.0006, { 0.7f, 0.8f, 1.0f } } };
		Rgb resm = light;
		for (int y = 0; y < depth.height; y++)
			for (int x = 0; x < depth.width; x++)
			{
				const size_t i = static_cast<size_t>(y) * depth.width + x;
				const Vec3 p = space.ViewPosition((x + 0.5) / scale, (y + 0.5) / scale, depth.v[i]);
				const uint8_t* nn = normal.rgba.data() + i * 4;
				const Vec3 n{ nn[0] / 127.5 - 1.0, 1.0 - nn[1] / 127.5, 1.0 - nn[2] / 127.5 };
				for (int c = 0; c < 3; c++)
					resm.v[i * 3 + c] = 0.01f;
				for (const Lamp& lamp : nightLamps)
				{
					const Vec3 d = lamp.at - p;
					const double r = Length(d), facing = Dot(n, d);
					if (facing > 0)
						for (int c = 0; c < 3; c++)
							resm.v[i * 3 + c] += static_cast<float>(lamp.strength * lamp.color[c] * facing / (r * r * r));
				}
			}
		std::string nightReport;
		const Lights night = FindLights(resm, &normal, depth, space, scale, nullptr, search, nightReport, LightTime::Night);
		printf("%s", nightReport.c_str());
		CHECK(night.lights.size() == 2);
		for (const Light& l : night.lights)
			CHECK(l.time == LightTime::Night && l.name.rfind("night light", 0) == 0);
		for (const Lamp& lamp : nightLamps)
			CHECK(nearest(night, lamp.at, "night light", nullptr) < 0.1 * lamp.at.z);
		Lights merged = found;
		const size_t both = MergeDayNight(merged, night, space);
		CHECK(both == 0 && merged.lights.size() == found.lights.size() + night.lights.size());
		for (const Lamp& lamp : nightLamps)
		{
			const Light* l = nullptr;
			nearest(merged, lamp.at, "night light", &l);
			CHECK(l != nullptr && l->time == LightTime::Night && l->source == LightSource::Found);
		}
		for (int k = 0; k < 2; k++)
		{
			const Light* l = nullptr;
			nearest(merged, lamps[k].at, "light", &l);
			CHECK(l != nullptr && l->time == LightTime::Day);
		}
		//the file keeps the times; DEMASTER's day leaves the night-only lamp out
		Lights back;
		std::string warnings;
		CHECK(ParseLights(WriteLights(merged, "test", "day and night"), back, warnings) && warnings.empty());
		CHECK(back.lights.size() == merged.lights.size());
		for (size_t i = 0; i < back.lights.size() && i < merged.lights.size(); i++)
			CHECK(back.lights[i].time == merged.lights[i].time);
		Lights dayOnly = back;
		KeepDayLights(dayOnly);
		CHECK(dayOnly.lights.size() + night.lights.size() == back.lights.size());
		for (const Light& l : dayOnly.lights)
			CHECK(l.time != LightTime::Night);
		//a lamp the day and the night both find (within 10 pixels, at about the same distance) is one lamp, on both
		if (!night.lights.empty())
		{
			Lights shared = found;
			Light same = night.lights[0];
			same.time = LightTime::Day;
			same.x += 3;
			shared.lights.push_back(same);
			const size_t n = shared.lights.size();
			CHECK(MergeDayNight(shared, night, space) == 1);
			CHECK(shared.lights.size() == n + night.lights.size() - 1 && shared.lights[n - 1].time == LightTime::Both);
		}
		//an old file without time: every light both
		Lights old;
		CHECK(ParseLights("[light]\nposition = 10 10 0.5\n", old, warnings) && old.lights.size() == 1 && old.lights[0].time == LightTime::Both);
	}
}

//the day/night clock, the lights on at each time of day, the [night] section and the field's fixed time
static void TestDayNight()
{
	DayNightCycle c; //480 + 240, 40 s fades
	//full day, full night, and the fades centred on the changes
	NEAR(NightAmount(c, 20), 0.0, 1e-12);
	NEAR(NightAmount(c, 240), 0.0, 1e-12);
	NEAR(NightAmount(c, 459.9), 0.0, 1e-12);
	NEAR(NightAmount(c, 480), 0.5, 1e-9);
	NEAR(NightAmount(c, 500), 1.0, 1e-9);
	NEAR(NightAmount(c, 600), 1.0, 1e-12);
	NEAR(NightAmount(c, 700), 1.0, 1e-9);
	NEAR(NightAmount(c, 0), 0.5, 1e-9);
	NEAR(NightAmount(c, 720), 0.5, 1e-9);
	NEAR(NightAmount(c, -200), 1.0, 1e-12); //wraps: 520, night
	CHECK(NightAmount(c, 470) > 0.0 && NightAmount(c, 470) < 0.5);
	CHECK(NightAmount(c, 710) < 1.0 && NightAmount(c, 710) > 0.5);
	CHECK(NightAmount(c, 10) < 0.5 && NightAmount(c, 10) > 0.0);
	//smooth and never going back the wrong way through a fade
	{
		double last = -1;
		bool rising = true;
		for (double t = 460; t <= 500; t += 0.5)
		{
			const double n = NightAmount(c, t);
			rising = rising && n >= last - 1e-12;
			last = n;
		}
		CHECK(rising);
	}
	//battles: three times slower, so a fade takes 120 s there
	NEAR(AdvanceClock(c, 100, 30, false), 130, 1e-9);
	NEAR(AdvanceClock(c, 100, 30, true), 110, 1e-9);
	NEAR(AdvanceClock(c, 700, 40, false), 20, 1e-9); //wraps
	NEAR(AdvanceClock(c, 100, -5, false), 100, 1e-9); //never back
	NEAR(AdvanceClock(c, 460, 120, true), 500, 1e-9);
	//swap: by day to the night's first full second, by night to the day's; through it and back
	NEAR(SwapTime(c, 240), 500, 1e-9);
	NEAR(SwapTime(c, 470), 500, 1e-9); //into dusk: night
	NEAR(SwapTime(c, 600), 20, 1e-9);
	NEAR(SwapTime(c, 5), 500, 1e-9); //the end of dawn's fade is mostly day already
	NEAR(SwapTime(c, 490), 20, 1e-9); //the end of dusk's is mostly night
	NEAR(NightAmount(c, SwapTime(c, 240)), 1.0, 1e-9);
	NEAR(NightAmount(c, SwapTime(c, 600)), 0.0, 1e-9);
	//just before the next fade
	NEAR(BeforeNextFade(c, 100, 5), 455, 1e-9);
	NEAR(BeforeNextFade(c, 600, 5), 695, 1e-9);
	NEAR(BeforeNextFade(c, 710, 5), 455, 1e-9); //in dawn's fade: the next is dusk
	NEAR(BeforeNextFade(c, 5, 5), 455, 1e-9);
	//text for the log
	CHECK(ClockText(c, 100).rfind("day, 1:20 in", 0) == 0);
	CHECK(ClockText(c, 600).rfind("night, 1:40 in", 0) == 0);
	CHECK(ClockText(c, 480).rfind("dusk (50% night)", 0) == 0);
	CHECK(ClockText(c, 715).rfind("dawn", 0) == 0);
	//other lengths from demaster.conf; nonsense is fixed
	{
		DayNightCycle d;
		d.day = 60;
		d.night = 30;
		d.fade = 100; //longer than the night: cut to 30
		const DayNightCycle k = d.Checked();
		NEAR(k.fade, 30, 1e-12);
		NEAR(NightAmount(d, 75), 1.0, 1e-9);
		NEAR(NightAmount(d, 60), 0.5, 1e-9);
		DayNightCycle bad;
		bad.day = -1;
		bad.battleSlower = 0;
		NEAR(bad.Checked().day, 480, 1e-12);
		NEAR(bad.Checked().battleSlower, 3, 1e-12);
	}
	//the clock's file
	{
		double t = -1;
		const std::string text = WriteClock(c, 812.5); //wraps to 92.5
		CHECK(ParseClock(text, t));
		NEAR(t, 92.5, 1e-9);
		CHECK(!ParseClock("; nothing\nspeed = 2\n", t));
		NEAR(t, 92.5, 1e-9);
	}

	//the lights on: 2 found + 6 yours per time of day, day and night cross-fading, a lamp on both always on
	{
		Lights l;
		const auto add = [&](LightTime time, LightSource source, bool baked = true)
		{
			Light light;
			light.time = time;
			light.source = source;
			light.baked = baked;
			light.name = "l" + std::to_string(l.lights.size());
			l.lights.push_back(light);
		};
		add(LightTime::Day, LightSource::Found); //0
		add(LightTime::Both, LightSource::Found); //1: one lamp, day and night
		add(LightTime::Night, LightSource::Found); //2
		add(LightTime::Night, LightSource::Yours, false); //3: a new light at night
		add(LightTime::Day, LightSource::Yours); //4
		const auto weightOf = [](const std::vector<ActiveLight>& on, size_t index) -> double
		{
			for (const ActiveLight& a : on)
				if (a.index == index)
					return a.weight;
			return 0.0;
		};
		std::vector<ActiveLight> day = ActiveLights(l, 0.0);
		CHECK(day.size() == 3);
		NEAR(weightOf(day, 0), 1.0, 1e-6);
		NEAR(weightOf(day, 1), 1.0, 1e-6);
		NEAR(weightOf(day, 4), 1.0, 1e-6);
		NEAR(weightOf(day, 2), 0.0, 1e-6);
		std::vector<ActiveLight> night = ActiveLights(l, 1.0);
		CHECK(night.size() == 3);
		NEAR(weightOf(night, 1), 1.0, 1e-6);
		NEAR(weightOf(night, 2), 1.0, 1e-6);
		NEAR(weightOf(night, 3), 1.0, 1e-6);
		for (const ActiveLight& a : night)
			CHECK(a.nightLamp == (a.index == 1 || a.index == 2)); //on at night and painted (3 is a new light)
		for (const ActiveLight& a : day)
			CHECK(a.nightLamp == (a.index == 1));
		std::vector<ActiveLight> fade = ActiveLights(l, 0.25);
		CHECK(fade.size() == 5);
		NEAR(weightOf(fade, 0), 0.75, 1e-6);
		NEAR(weightOf(fade, 1), 1.0, 1e-6);
		NEAR(weightOf(fade, 2), 0.25, 1e-6);
		bool inOrder = true;
		for (size_t i = 1; i < fade.size(); i++)
			inOrder = inOrder && fade[i - 1].index < fade[i].index;
		CHECK(inOrder);
		//a third found light at night isn't used, as the file's warning says
		add(LightTime::Night, LightSource::Found); //5
		NEAR(weightOf(ActiveLights(l, 1.0), 5), 0.0, 1e-6);
		//the day set is the same as KeepDayLights
		Lights kept = l;
		KeepDayLights(kept);
		CHECK(kept.lights.size() == ActiveLights(l, 0.0).size());
	}
	//16 lights in a fade: the 8 brightest stay
	{
		Lights l;
		for (int i = 0; i < 16; i++)
		{
			Light light;
			light.time = i < 8 ? (i < 2 ? LightTime::Day : LightTime::Day) : LightTime::Night;
			light.source = i % 8 < 2 ? LightSource::Found : LightSource::Yours;
			light.name = "l" + std::to_string(i);
			l.lights.push_back(light);
		}
		size_t dropped = 0;
		std::vector<ActiveLight> on = ActiveLights(l, 0.3, &dropped);
		CHECK(on.size() == static_cast<size_t>(MAX_LIGHTS) && dropped == 8);
		for (const ActiveLight& a : on)
			CHECK(a.index < 8); //the day's, at 0.7
		on = ActiveLights(l, 0.8, &dropped);
		for (const ActiveLight& a : on)
			CHECK(a.index >= 8);
		on = ActiveLights(l, 1.0, &dropped);
		CHECK(on.size() == 8 && dropped == 0);
	}
	//[night]: read, written, defaults, nonsense
	{
		Lights l;
		std::string w;
		CHECK(ParseLights("ambient = 0 0 0\n[night]\nlevel = 0.3\ntint = 0.5 0.6 0.9\nlamps = 0.75\n[light]\nposition = 1 2 0.5\n", l, w));
		CHECK(w.empty());
		CHECK(l.hasNight && l.lights.size() == 1);
		NEAR(l.night.level, 0.3f, 1e-6f);
		NEAR(l.night.tint[2], 0.9f, 1e-6f);
		NEAR(l.night.lamps, 0.75f, 1e-6f);
		Lights back;
		w.clear();
		CHECK(ParseLights(WriteLights(l, "test", ""), back, w) && w.empty());
		CHECK(back.hasNight && back.lights.size() == 1);
		NEAR(back.night.level, 0.3f, 1e-4f);
		NEAR(back.night.tint[0], 0.5f, 1e-4f);
		NEAR(back.night.lamps, 0.75f, 1e-4f);
		Lights none;
		w.clear();
		ParseLights("[light]\nposition = 1 2 0.5\n", none, w);
		CHECK(!none.hasNight);
		NEAR(none.night.level, 0.18f, 1e-6f);
		w.clear();
		ParseLights("[night]\nlevel = 2\nbrightness = 1\n", none, w);
		CHECK(w.find("can't read level") != std::string::npos && w.find("unknown setting brightness in [night]") != std::string::npos);
	}
	//the field's own time in _pbr.txt
	{
		Settings s;
		std::string w;
		CHECK(s.time == FieldTime::Cycle);
		ParseSettings("time = night\n", s, w);
		CHECK(s.time == FieldTime::Night && w.empty());
		Settings r;
		CHECK(ParseSettings(WriteSettings(s, "test", ""), r, w));
		CHECK(r.time == FieldTime::Night);
		ParseSettings("time = noon\n", r, w);
		CHECK(r.time == FieldTime::Night && w.find("can't read time") != std::string::npos);
	}
}

int main(int argc, char** argv)
{
	TestColour();
	TestSpace();
	TestSettings();
	TestLights();
	TestDayNight();
	TestFindLights();
	TestV15Files();
	TestCamera();
	TestDepthFit();
	TestOrientation();
	TestSplit();
	TestTint();
	TestCompose();
	TestPacking();
	TestBc7AndFiles();
	TestArchive(argc > 1 ? argv[1] : nullptr);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
