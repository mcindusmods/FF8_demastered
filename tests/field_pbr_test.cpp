//Tests for ff8_demaster/field_pbr.cpp (what DEMASTER and fieldprep agree on) and fieldprep's own parts (archives,
//lightmap split, depth fit, orientation, packing, BC7).
//  g++ -std=c++20 -O2 -pthread field_pbr_test.cpp ../ff8_demaster/field_pbr.cpp ../ff8_demaster/field_moving_lights.cpp ../ff8_demaster/field_layers.cpp
//      ../fieldprep/prep.cpp ../fieldprep/image_io.cpp ../fieldprep/archive.cpp ../fieldprep/third_party/*.cpp -o field_pbr_test
//  ./field_pbr_test [folder with a real world.fl/fi/fs or field.fl/fi/fs, to read every entry]

#include "../ff8_demaster/field_layers.h"
#include "../ff8_demaster/field_pbr.h"
#include "../ff8_demaster/field_moving_lights.h"
#include "../ff8_demaster/lut_grade.h"
#include "../ff8_demaster/scene_light.h"
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
		light.time = i == 0 ? LightTime::Day : i == 1 ? LightTime::Both : LightTime::Night; //two by day, two by night: no warning
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
	CHECK(warnings.find("20 lights on by day: DEMASTER uses 2") != std::string::npos);
	CHECK(warnings.find("20 lights on by night: DEMASTER uses 6") != std::string::npos);
	{
		Lights day = r;
		KeepDayLights(day);
		CHECK(day.lights.size() == static_cast<size_t>(MAX_DAY_LIGHTS));
		day = r;
		KeepDayLights(day, 1); //the sun has the other place
		CHECK(day.lights.size() == 1 && day.lights[0].name == "light 1");
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
		CHECK(h.lights.size() == 16);
		CHECK(w.find("8 lights on by day: DEMASTER uses 2") != std::string::npos && w.find("8 lights on by night: DEMASTER uses 6") != std::string::npos);
		CHECK(h.lights[0].source == LightSource::Mark && h.lights[1].source == LightSource::Yours && h.lights[12].source == LightSource::Found);
		//by day: yours first (the first two in the file), fieldprep's left out
		Lights day = h;
		KeepDayLights(day);
		CHECK(day.lights.size() == 2 && day.lights[0].source != LightSource::Found && day.lights[1].source != LightSource::Found);
		//day and night sets together: 2 + 6, all yours
		Lights both = h;
		KeepDayAndNightLights(both);
		CHECK(both.lights.size() == 8 && std::none_of(both.lights.begin(), both.lights.end(), [](const Light& l) { return l.source == LightSource::Found; }));
		//only fieldprep's: they fill the places
		std::string found;
		for (int i = 0; i < 4; i++)
			found += std::string("[light]\nposition = 1 2 0.5\nsource = found\ntime = ") + (i < 2 ? "day" : "night") + "\n";
		w.clear();
		ParseLights(found, h, w);
		CHECK(w.empty());
		day = h;
		KeepDayLights(day);
		CHECK(day.lights.size() == 2 && day.lights[0].source == LightSource::Found);
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
	//the time of day per save: kept, found again by the play time a loaded save brings back, written and read
	{
		SaveClocks k;
		k.Record(1000, 100.0);
		k.Record(5000, 300.5);
		k.Record(1000, 120.0); //saved again at the same play time: the newer one
		CHECK(k.entries.size() == 2 && k.entries.back().play == 1000);
		const SaveClock* f = k.Find(998, 10); //the save's own play time is a moment before we saw the file written
		CHECK(f != nullptr && f->clock == 120.0);
		CHECK(k.Find(1001, 10) == nullptr); //later than any save at that time: not that save
		CHECK(k.Find(980, 10) == nullptr);
		f = k.Find(4995, 10);
		CHECK(f != nullptr && f->play == 5000);
		const std::string text = WriteSaveClocks(k);
		SaveClocks again;
		CHECK(ParseSaveClocks(text, again) && again.entries.size() == 2);
		CHECK(again.entries[0].play == 5000 && again.entries[1].play == 1000);
		NEAR(again.entries[0].clock, 300.5, 1e-6);
		CHECK(!ParseSaveClocks("; nothing\nplay = x\n", again) && again.entries.empty());
		for (uint32_t i = 0; i < SaveClocks::MOST + 20; i++)
			k.Record(10000 + i, 1.0);
		CHECK(k.entries.size() == SaveClocks::MOST && k.entries.front().play == 10000 + 20); //the two from before and the first 20 went
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
		//by day two: yours (4) first, then fieldprep's first (0); 1 (fieldprep's second) is left out by day
		std::vector<ActiveLight> day = ActiveLights(l, 0.0);
		CHECK(day.size() == 2);
		NEAR(weightOf(day, 4), 1.0, 1e-6);
		NEAR(weightOf(day, 0), 1.0, 1e-6);
		NEAR(weightOf(day, 1), 0.0, 1e-6);
		NEAR(weightOf(day, 2), 0.0, 1e-6);
		//with the sun out only yours
		std::vector<ActiveLight> sunny = ActiveLights(l, 0.0, nullptr, MAX_LIGHTS, -1.0, 1);
		CHECK(sunny.size() == 1 && sunny[0].index == 4);
		std::vector<ActiveLight> night = ActiveLights(l, 1.0);
		CHECK(night.size() == 3);
		NEAR(weightOf(night, 1), 1.0, 1e-6);
		NEAR(weightOf(night, 2), 1.0, 1e-6);
		NEAR(weightOf(night, 3), 1.0, 1e-6);
		for (const ActiveLight& a : night)
			CHECK(a.nightLamp == (a.index == 1 || a.index == 2)); //on at night and painted (3 is a new light)
		for (const ActiveLight& a : day)
			CHECK(!a.nightLamp);
		std::vector<ActiveLight> fade = ActiveLights(l, 0.25);
		CHECK(fade.size() == 5);
		NEAR(weightOf(fade, 0), 0.75, 1e-6);
		NEAR(weightOf(fade, 1), 0.25, 1e-6); //in the night's set only (the day's two places are taken)
		NEAR(weightOf(fade, 2), 0.25, 1e-6);
		//the evening: the day's lights go with dayOn, the night's with the night; a light in both sets stays at 1
		std::vector<ActiveLight> evening = ActiveLights(l, 0.1, nullptr, MAX_LIGHTS, 0.6);
		NEAR(weightOf(evening, 4), 0.6, 1e-6);
		NEAR(weightOf(evening, 2), 0.1, 1e-6);
		bool inOrder = true;
		for (size_t i = 1; i < fade.size(); i++)
			inOrder = inOrder && fade[i - 1].index < fade[i].index;
		CHECK(inOrder);
		//by night six: more are left out (yours first)
		for (int i = 0; i < 4; i++)
			add(LightTime::Night, LightSource::Yours); //5-8
		std::vector<ActiveLight> full = ActiveLights(l, 1.0);
		CHECK(full.size() == 6);
		NEAR(weightOf(full, 1), 1.0, 1e-6); //five of yours, then fieldprep's first one on at night (1, on day and night)
		NEAR(weightOf(full, 2), 0.0, 1e-6); //fieldprep's second: no place left
		//the day set is the same as KeepDayLights
		Lights kept = l;
		KeepDayLights(kept);
		CHECK(kept.lights.size() == ActiveLights(l, 0.0).size());
	}
	//16 lights: the day's 2 and the night's 6 fit MAX_LIGHTS in a fade, so none drops out; with two moving lights the faintest go
	{
		Lights l;
		for (int i = 0; i < 16; i++)
		{
			Light light;
			light.time = i < 8 ? LightTime::Day : LightTime::Night;
			light.source = i % 8 < 2 ? LightSource::Found : LightSource::Yours;
			light.name = "l" + std::to_string(i);
			l.lights.push_back(light);
		}
		size_t dropped = 0;
		std::vector<ActiveLight> on = ActiveLights(l, 0.3, &dropped);
		CHECK(on.size() == static_cast<size_t>(MAX_DAY_LIGHTS + MAX_NIGHT_LIGHTS) && dropped == 0);
		on = ActiveLights(l, 0.3, &dropped, MAX_LIGHTS - 2);
		CHECK(on.size() == static_cast<size_t>(MAX_LIGHTS - 2) && dropped == 2);
		for (const ActiveLight& a : on)
			CHECK(a.index < 8 || a.weight >= 0.3f - 1e-6f);
		on = ActiveLights(l, 1.0, &dropped);
		CHECK(on.size() == static_cast<size_t>(MAX_NIGHT_LIGHTS) && dropped == 0);
		for (const ActiveLight& a : on)
			CHECK(a.index >= 10); //yours (10-15); fieldprep's 8 and 9 come after them
	}
	//the evening (DAY_NIGHT_EVENING): starts at 0.9 of the day, done with the dusk's fade, the dawn as the night's
	{
		DayNightCycle c; //480 + 240, 40 s fades
		NEAR(EveningAmount(c, 400), 0.0, 1e-9);
		NEAR(EveningAmount(c, 432), 0.0, 1e-9);
		CHECK(EveningAmount(c, 450) > 0.05 && NightAmount(c, 450) == 0.0);
		CHECK(EveningAmount(c, 470) > NightAmount(c, 470));
		NEAR(EveningAmount(c, 500), 1.0, 1e-9);
		NEAR(EveningAmount(c, 600), 1.0, 1e-9);
		NEAR(EveningAmount(c, 715), NightAmount(c, 715), 1e-9);
		NEAR(EveningAmount(c, 10), NightAmount(c, 10), 1e-9);
		c.evening = 1.0; //with the fade itself: the night's
		for (double t = 0; t < 720; t += 7)
			NEAR(EveningAmount(c, t), NightAmount(c, t), 1e-9);
	}
	//night_radius: read, written, and what the keep window reaches
	{
		Lights l;
		std::string w;
		CHECK(ParseLights("[light]\nposition = 1 2 0.5\nrange = 40\nnight_radius = 90\ntime = night\n[light]\nposition = 3 4 0.5\n", l, w));
		CHECK(w.empty() && l.lights.size() == 2);
		NEAR(l.lights[0].KeepRadius(), 90.0f, 1e-6f);
		NEAR(l.lights[1].KeepRadius(), l.lights[1].range, 1e-6f);
		const std::string text = WriteLights(l, "f", "");
		CHECK(text.find("night_radius = 90.0") != std::string::npos);
		Lights again;
		ParseLights(text, again, w);
		NEAR(again.lights[0].nightRadius, 90.0f, 1e-6f);
		CHECK(again.lights[1].nightRadius < 0);
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

//the look (FIELD_PBR_LOOK): the dials, the field's own settings, the relight's scale
static void TestLook()
{
	//demaster.conf's dials times the master dial times the field's look; a field's own dial replaces demaster.conf's
	{
		Settings s;
		Look look = FieldLook(s, 0.0, 0.5, 0.5, 0.5);
		CHECK(!look.Any()); //master 0: the painting as it is
		look = FieldLook(s, 1.0, 0.5, 0.25, 0.75);
		NEAR(look.relight, 0.5, 1e-6);
		NEAR(look.highlights, 0.25, 1e-6);
		NEAR(look.ao, 0.75, 1e-6);
		CHECK(look.Any());
		s.look = 2.0f;
		s.lookAo = 0.1f;
		look = FieldLook(s, 1.5, 0.5, 0.25, 0.75);
		NEAR(look.relight, 1.5, 1e-6);
		NEAR(look.highlights, 0.75, 1e-6);
		NEAR(look.ao, 0.3, 1e-6); //its own 0.1 x 1.5 x 2
		s.look = 0.0f;
		CHECK(!FieldLook(s, 2.0, 1.0, 1.0, 1.0).Any()); //look = 0 in _pbr.txt: this field stays as painted
		s.look = 100.0f;
		CHECK(FieldLook(s, 100.0, 1.0, 1.0, 1.0).relight <= 4.0f); //bounded
	}
	//_pbr.txt: read, written, read again; whatever it doesn't say stays
	{
		Settings s;
		std::string w;
		CHECK(ParseSettings("look = 0.5\nlook_relight = 1.25\nlook_highlights = auto\nlook_ao = 0\n", s, w) && w.empty());
		NEAR(s.look, 0.5, 1e-6);
		NEAR(s.lookRelight, 1.25, 1e-6);
		CHECK(s.lookHighlights < 0);
		NEAR(s.lookAo, 0.0, 1e-6);
		Settings again;
		std::string w2;
		CHECK(ParseSettings(WriteSettings(s, "test", ""), again, w2) && w2.empty());
		NEAR(again.look, 0.5, 1e-6);
		NEAR(again.lookRelight, 1.25, 1e-6);
		CHECK(again.lookHighlights < 0);
		NEAR(again.lookAo, 0.0, 1e-6);
		ParseSettings("look = much\n", again, w2);
		CHECK(!w2.empty());
		NEAR(again.look, 0.5, 1e-6);
		Settings fresh;
		NEAR(fresh.look, 1.0, 1e-6);
		CHECK(fresh.lookRelight < 0 && fresh.lookHighlights < 0 && fresh.lookAo < 0);
	}
	//the relight's scale: with it, the average of (rest + scale x S) / (rest + W) over the samples is 1
	{
		std::mt19937 random(7);
		std::uniform_real_distribution<double> unit(-1.0, 1.0);
		std::vector<ShadeSample> samples;
		for (int i = 0; i < 400; i++)
		{
			ShadeSample sample;
			sample.position = { unit(random) * 0.5, unit(random) * 0.4, 1.0 + unit(random) * 0.3 };
			sample.normal = Normalize({ unit(random), unit(random), -0.2 - std::fabs(unit(random)) });
			samples.push_back(sample);
		}
		const float pos[8] = { 0.2f, -0.5f, 0.8f, 1.2f, -0.6f, 0.1f, 1.3f, 0.9f };
		const float paint[8] = { 0, 0, 0, 1.0f, 0, 0, 0, -0.6f }; //a lamp of the field and a moving light
		const double scale = RelightScale(samples, pos, paint, 2);
		CHECK(scale > 0.75 && scale < 4.0);
		double mean = 0;
		for (const ShadeSample& sample : samples)
		{
			double weightSum = 0, shadeSum = 0;
			for (int i = 0; i < 2; i++)
			{
				const Vec3 toLight = Vec3{ pos[i * 4], pos[i * 4 + 1], pos[i * 4 + 2] } - sample.position;
				const double dd = Dot(toLight, toLight), x2 = dd / (pos[i * 4 + 3] * pos[i * 4 + 3]);
				if (x2 >= 1)
					continue;
				const double weight = std::fabs(paint[i * 4 + 3]) * (1 - x2 * x2) * (1 - x2 * x2) / (1 + 8 * x2);
				shadeSum += weight * std::clamp((Dot(sample.normal, toLight) / std::sqrt(dd) + 0.5) / 1.5, 0.0, 1.0);
				weightSum += weight;
			}
			mean += (RELIGHT_REST + scale * shadeSum) / (RELIGHT_REST + weightSum);
		}
		NEAR(mean / samples.size(), 1.0, 1e-9);
		//nothing to go by: 1.5
		NEAR(RelightScale({}, pos, paint, 2), 1.5, 1e-12);
		const float none[8] = {};
		NEAR(RelightScale(samples, pos, none, 2), 1.5, 1e-12);
	}
	//the samples from a grid of _ND: positions over the canvas at the map's depths, normals as the shader reads them, no sky
	{
		Settings s;
		s.space = EstimatedSpace(160, 112, true);
		std::vector<uint8_t> nd(4 * 3 * 4);
		for (size_t i = 0; i < nd.size(); i += 4)
		{
			nd[i] = 128;
			nd[i + 1] = 255; //green up
			nd[i + 2] = 128;
			nd[i + 3] = 200;
		}
		nd[3] = 0; //one far point
		std::vector<ShadeSample> samples = ShadeSamples(nd.data(), 4, 3, s, 320, 224);
		CHECK(samples.size() == 12);
		NEAR(samples[5].normal.y, -1.0, 0.01); //up in the map is y down's opposite in view space
		NEAR(samples[5].position.z, s.space.Distance(200 / 255.0), 1e-9);
		double cx = 0, cy = 0;
		CHECK(s.space.Project(samples[5].position, cx, cy));
		NEAR(cx, 1.5 * 320 / 4, 1e-6);
		NEAR(cy, 1.5 * 224 / 3, 1e-6);
		s.sky = true;
		s.skyValue = 0.1; //the far point is sky
		CHECK(ShadeSamples(nd.data(), 4, 3, s, 320, 224).size() == 11);
		CHECK(ShadeSamples(nullptr, 4, 3, s, 320, 224).empty());
	}
}

//the moving lights (FIELD_MOVING_LIGHTS): the sun's path and light, the party leader and its light, their settings
static void TestMovingLights()
{
	const DayNightCycle c; //480 s of day, 240 of night
	//the clock on the sun's and the moon's arcs
	{
		SkyTime sky = SkyTimeOf(c, 0);
		CHECK(!sky.night);
		NEAR(sky.phase, 0.0, 1e-12);
		sky = SkyTimeOf(c, 240);
		CHECK(!sky.night);
		NEAR(sky.phase, 0.5, 1e-12);
		sky = SkyTimeOf(c, 600);
		CHECK(sky.night);
		NEAR(sky.phase, 0.5, 1e-12);
		sky = SkyTimeOf(c, 720 + 120);
		CHECK(!sky.night);
		NEAR(sky.phase, 0.25, 1e-12);
		sky = SkyTimeOf(c, -60);
		CHECK(sky.night);
		NEAR(sky.phase, 0.75, 1e-12);
	}
	//the sun's path: view space is x right, y down, z away from the camera
	{
		const Vec3 up{ 0, -1, 0 };
		const Vec3 rise = SunDirection(up, 0, 60, 0), noon = SunDirection(up, 0, 60, 0.5), set = SunDirection(up, 0, 60, 1);
		NEAR(rise.x, 1.0, 1e-9); //azimuth 0: it rises on the right of the picture
		NEAR(Dot(rise, up), 0.0, 1e-9);
		NEAR(set.x, -1.0, 1e-9);
		NEAR(Dot(noon, up), std::sin(60 * 3.14159265358979 / 180), 1e-9);
		CHECK(noon.z < 0); //at noon it leans towards the camera
		for (double phase = 0; phase <= 1.0; phase += 0.05)
		{
			const Vec3 d = SunDirection(up, 37, 50, phase);
			NEAR(Length(d), 1.0, 1e-9);
			CHECK(Dot(d, up) >= -1e-9); //never below the horizon on its arc
		}
		const Vec3 behind = SunDirection(up, 90, 60, 0);
		NEAR(behind.z, 1.0, 1e-9); //azimuth 90: it rises behind the scene
		NEAR(SunDirection(up, 180, 60, 0).x, -1.0, 1e-9);
		//a camera looking down at the ground: up tilted towards the camera
		const Vec3 tilted = Normalize({ 0, -0.8, -0.6 });
		NEAR(Dot(SunDirection(tilted, 20, 45, 0.5), tilted), std::sin(45 * 3.14159265358979 / 180), 1e-9);
		NEAR(Dot(SunDirection(tilted, 20, 45, 0.0), tilted), 0.0, 1e-9);
		//its colour: orange low, near white high; the moon blue
		float low[3], high[3], moon[3];
		SunColor(0.02, false, low);
		SunColor(0.9, false, high);
		SunColor(0.9, true, moon);
		CHECK(low[2] < 0.4f && low[0] > 0.95f && high[2] > 0.85f && moon[2] > moon[0]);
		NEAR(HorizonFade(0.0), 0.0, 1e-12);
		NEAR(HorizonFade(0.3), 1.0, 1e-12);
		CHECK(HorizonFade(0.08) > 0.0 && HorizonFade(0.08) < 1.0);
	}
	//the sun as a light
	{
		Settings s;
		s.space = EstimatedSpace(168, 120, true);
		Sun sun;
		Vec3 centre;
		double extent = 0;
		FieldExtent(s, 336, 240, centre, extent);
		CHECK(extent > 0 && centre.z > 0);
		double cx = 0, cy = 0;
		CHECK(s.space.Project(centre, cx, cy));
		NEAR(cx, 168.0, 1e-6);
		NEAR(cy, 120.0, 1e-6);
		//auto: only fields open to the sky
		CHECK(!SunLight(sun, s, FieldTime::Cycle, { false, 0.5 }, 0.0, centre, extent, 1.0).on);
		s.sky = true;
		const MovingLight noon = SunLight(sun, s, FieldTime::Cycle, { false, 0.5 }, 0.0, centre, extent, 1.0);
		CHECK(noon.on && noon.castsShadow);
		NEAR(Length(noon.position - centre), extent * 4.0, 1e-9);
		//the shaders' falloff at the field's middle gives back the settings: intensity x colour, and shade as the weight
		{
			const double x = Length(noon.position - centre) / noon.range, window = 1 - x * x * x * x;
			const double falloff = window * window / (1 + 8 * x * x);
			float colour[3];
			SunColor(Dot(Normalize(noon.position - centre), Normalize(s.up)), false, colour);
			for (int k = 0; k < 3; k++)
				NEAR(noon.color[k] * falloff, colour[k] * sun.intensity, 1e-5);
			NEAR(noon.shade * falloff, sun.shade, 1e-5);
			//and it is nearly the same at the field's edges (within 4%)
			const double edge = (Length(noon.position - centre) + extent * 0.5) / noon.range, edgeWindow = 1 - edge * edge * edge * edge;
			CHECK(std::fabs(edgeWindow * edgeWindow / (1 + 8 * edge * edge) / falloff - 1.0) < 0.04);
		}
		//at the horizon and below: off; fading with the night; the moon at night, fainter and blue
		CHECK(!SunLight(sun, s, FieldTime::Cycle, { false, 0.0 }, 0.5, centre, extent, 1.0).on);
		const MovingLight dusk = SunLight(sun, s, FieldTime::Cycle, { false, 0.9 }, 0.3, centre, extent, 1.0);
		CHECK(dusk.on && dusk.color[0] < noon.color[0] && dusk.color[2] / dusk.color[0] < noon.color[2] / noon.color[0]);
		const MovingLight moon = SunLight(sun, s, FieldTime::Cycle, { true, 0.5 }, 1.0, centre, extent, 1.0);
		CHECK(moon.on && moon.color[2] > moon.color[0] && moon.shade < noon.shade * 0.3f);
		sun.moon = 0.0f;
		CHECK(!SunLight(sun, s, FieldTime::Cycle, { true, 0.5 }, 1.0, centre, extent, 1.0).on);
		sun.moon = 0.25f;
		//a field with its own time: the sun stands at `fixed` whatever the clock says; the moon highest in a night field
		const MovingLight fixedDay = SunLight(sun, s, FieldTime::Day, { true, 0.1 }, 1.0, centre, extent, 1.0);
		const MovingLight atFixed = SunLight(sun, s, FieldTime::Cycle, { false, sun.fixed }, 0.0, centre, extent, 1.0);
		CHECK(fixedDay.on);
		NEAR(Length(fixedDay.position - atFixed.position), 0.0, 1e-9);
		CHECK(SunLight(sun, s, FieldTime::Night, { false, 0.5 }, 0.0, centre, extent, 1.0).color[2]
			> SunLight(sun, s, FieldTime::Night, { false, 0.5 }, 0.0, centre, extent, 1.0).color[0]);
		//off: by the field, by FIELD_SUN
		sun.on = 0;
		CHECK(!SunLight(sun, s, FieldTime::Cycle, { false, 0.5 }, 0.0, centre, extent, 1.0).on);
		sun.on = 1;
		s.sky = false;
		CHECK(SunLight(sun, s, FieldTime::Cycle, { false, 0.5 }, 0.0, centre, extent, 1.0).on); //on = 1: without sky too
		CHECK(!SunLight(sun, s, FieldTime::Cycle, { false, 0.5 }, 0.0, centre, extent, 0.0).on);
		//FIELD_SUN scales it
		NEAR(SunLight(sun, s, FieldTime::Cycle, { false, 0.5 }, 0.0, centre, extent, 0.5).color[0], noon.color[0] * 0.5, 1e-5);
	}
	//who carries the party light
	{
		const auto candidate = [](const int entity, const int pc, const double x)
		{
			LeaderCandidate c;
			c.key = entity;
			c.entity = entity;
			c.pc = pc;
			c.center = { x, 0, 1 };
			c.up = { 0, -1, 0 };
			c.halfHeight = 0.05;
			return c;
		};
		LeaderTrack track;
		//three entities drawn: 0 and 2 are the field's (pc byte 255, as most have), 1 is playable
		std::vector<LeaderCandidate> drawn = { candidate(0, 255, -0.3), candidate(1, 0, 0.0), candidate(2, 255, 0.3) };
		CHECK(ChooseLeader(drawn, -1, 255, 0.0, track) == 1);
		//an NPC walks: the playable one keeps it
		drawn[0].center.x += 0.05;
		CHECK(ChooseLeader(drawn, -1, 255, 0.1, track) == 1);
		//[party] follow = 2
		CHECK(ChooseLeader(drawn, 2, 255, 0.2, track) == 2);
		CHECK(ChooseLeader(drawn, 7, 255, 0.3, track) == -1); //not drawn: no light
		//the game says who the player moves: that one, whoever moved last (a follower walks after the leader stops)
		{
			LeaderTrack led;
			std::vector<LeaderCandidate> party3 = { candidate(3, 0, -0.2), candidate(4, 1, 0.0), candidate(5, 2, 0.2) };
			CHECK(ChooseLeader(party3, -1, 255, 0.0, led, 4) == 1);
			party3[2].center.x += 0.05; //a follower moves, the leader stands
			CHECK(ChooseLeader(party3, -1, 255, 1.0, led, 4) == 1);
			party3[0].center.x += 0.05;
			CHECK(ChooseLeader(party3, -1, 255, 2.0, led, 4) == 1);
			CHECK(ChooseLeader(party3, 5, 255, 3.0, led, 4) == 2); //[party] follow still wins
			CHECK(ChooseLeader(party3, -1, 255, 4.0, led, 9) >= 0); //the player's entity isn't drawn: as before, someone
			party3[0].center.x += 0.05;
			CHECK(ChooseLeader(party3, -1, 255, 6.0, led, 9) == 0); //...the one that moved last
		}
		//two playable ones (an event with the party on screen): the one that moved last, and it stays with it
		LeaderTrack two;
		std::vector<LeaderCandidate> party = { candidate(3, 0, -0.2), candidate(4, 1, 0.2), candidate(5, 255, 0.5) };
		const int first = ChooseLeader(party, -1, 255, 0.0, two);
		CHECK(first == 0 || first == 1);
		party[1].center.x += 0.05;
		CHECK(ChooseLeader(party, -1, 255, 1.0, two) == 1);
		CHECK(ChooseLeader(party, -1, 255, 2.0, two) == 1); //nobody moves: it stays
		party[0].center.x += 0.05;
		CHECK(ChooseLeader(party, -1, 255, 3.0, two) == 0);
		party[2].center.x += 0.05; //the NPC: not playable
		CHECK(ChooseLeader(party, -1, 255, 4.0, two) == 0);
		//a sway smaller than a fiftieth of the body isn't moving
		party[1].center.x += 0.0005;
		CHECK(ChooseLeader(party, -1, 255, 5.0, two) == 0);
		//the byte tells nothing (every entity has the same, or the entities aren't known): the one that moved last of all
		LeaderTrack blind;
		std::vector<LeaderCandidate> same = { candidate(0, -1, -0.3), candidate(1, -1, 0.0) };
		same[0].entity = same[1].entity = -1;
		CHECK(ChooseLeader(same, -1, -1, 0.0, blind) >= 0);
		same[1].center.x += 0.05;
		CHECK(ChooseLeader(same, -1, -1, 1.0, blind) == 1);
		same[0].center.x += 0.05;
		CHECK(ChooseLeader(same, -1, -1, 2.0, blind) == 0);
		CHECK(ChooseLeader({}, -1, -1, 3.0, blind) == -1);
	}
	//the party light
	{
		Space space = EstimatedSpace(168, 120, true);
		LeaderCandidate leader;
		leader.center = { 0.1, 0.05, 1.0 };
		leader.up = { 0, -1, 0 };
		leader.halfHeight = 0.05;
		PartyLight party;
		//mode 1: with the night; 2: always; 0: off
		CHECK(!PartyLightFor(party, leader, space, 1, 0.0, 1.0).on);
		const MovingLight half = PartyLightFor(party, leader, space, 1, 0.5, 1.0), full = PartyLightFor(party, leader, space, 1, 1.0, 1.0);
		CHECK(half.on && full.on);
		NEAR(half.color[0] * 2, full.color[0], 1e-6);
		CHECK(PartyLightFor(party, leader, space, 2, 0.0, 1.0).on);
		CHECK(!PartyLightFor(party, leader, space, 0, 1.0, 1.0).on);
		NEAR(full.color[1], party.color[1] * party.intensity, 1e-6);
		//above the leader's middle and nearer the camera; it doesn't cast the characters' shadows or move the painted light
		CHECK(full.position.y < leader.center.y && full.position.z < leader.center.z);
		CHECK(Length(full.position - leader.center) < leader.halfHeight * 2);
		CHECK(!full.castsShadow && full.shade == 0.0f && full.shadow == 0.0f);
		NEAR(full.range, space.ViewLength(party.range, full.position.z), 1e-9);
		//the field's own setting wins over demaster.conf's
		party.on = 2;
		CHECK(PartyLightFor(party, leader, space, 0, 0.0, 1.0).on);
		party.on = 0;
		CHECK(!PartyLightFor(party, leader, space, 2, 1.0, 1.0).on);
	}
	//[sun] and [party] in _lights.txt: read, written, read again; files without them: the defaults
	{
		Lights lights;
		std::string w;
		CHECK(ParseLights("ambient = 0 0 0\n[sun]\non = 1\nazimuth = 135\nheight = 40\nintensity = 0.6\nshade = 0.5\nmoon = 0\nfixed = 0.7\n"
			"[party]\non = always\ncolor = 0.5 0.6 1\nintensity = 2\nrange = 90\nheight = 0.2\nfollow = 3\n", lights, w) && w.empty());
		CHECK(lights.hasSun && lights.hasParty);
		CHECK(lights.sun.on == 1);
		NEAR(lights.sun.azimuth, 135, 1e-6);
		NEAR(lights.sun.height, 40, 1e-6);
		NEAR(lights.sun.intensity, 0.6, 1e-6);
		NEAR(lights.sun.shade, 0.5, 1e-6);
		NEAR(lights.sun.moon, 0.0, 1e-6);
		NEAR(lights.sun.fixed, 0.7, 1e-6);
		CHECK(lights.party.on == 2 && lights.party.follow == 3);
		NEAR(lights.party.color[2], 1.0, 1e-6);
		NEAR(lights.party.intensity, 2.0, 1e-6);
		NEAR(lights.party.range, 90.0, 1e-6);
		NEAR(lights.party.height, 0.2, 1e-6);
		Lights again;
		std::string w2;
		CHECK(ParseLights(WriteLights(lights, "test", ""), again, w2) && w2.empty());
		CHECK(again.hasSun && again.hasParty && again.sun.on == 1 && again.party.on == 2 && again.party.follow == 3);
		NEAR(again.sun.azimuth, 135, 1e-3);
		NEAR(again.sun.fixed, 0.7, 1e-3);
		NEAR(again.party.range, 90.0, 1e-3);
		Lights old;
		CHECK(ParseLights("ambient = 0 0 0\n", old, w2));
		CHECK(!old.hasSun && !old.hasParty && old.sun.on == -1 && old.party.on == -1 && old.party.follow == -1);
		std::string bad;
		ParseLights("[sun]\nheight = 200\nwarp = 1\n[party]\non = sometimes\n", old, bad);
		CHECK(bad.find("height") != std::string::npos && bad.find("warp") != std::string::npos && bad.find("on") != std::string::npos);
	}
	//the moving lights take places among the lights: the field's own get the rest
	{
		Lights lights;
		for (int i = 0; i < 8; i++)
		{
			Light l;
			l.source = i < 2 ? LightSource::Found : LightSource::Yours;
			l.time = LightTime::Night;
			lights.lights.push_back(l);
		}
		size_t dropped = 0;
		CHECK(ActiveLights(lights, 1.0, &dropped).size() == static_cast<size_t>(MAX_NIGHT_LIGHTS) && dropped == 0);
		CHECK(ActiveLights(lights, 1.0, &dropped, 4).size() == 4 && dropped == 2);
		CHECK(ActiveLights(lights, 1.0, &dropped, 100).size() == static_cast<size_t>(MAX_NIGHT_LIGHTS));
	}
}

//the glows' picture (_EC) and its "day" setting
static void TestEmissivePicture()
{
	//the old mask (grey, no alpha) is not a picture; colour or an alpha of its own is
	std::vector<uint8_t> mask(64 * 4, 255), colour, alpha;
	for (size_t i = 0; i < 64; i++)
		mask[i * 4] = mask[i * 4 + 1] = mask[i * 4 + 2] = static_cast<uint8_t>(i * 4);
	mask[5 * 4 + 1] += 6; //an editor's noise
	CHECK(!EmissiveIsPicture(mask.data(), 64) && !EmissiveHasAlpha(mask.data(), 64));
	colour = mask;
	colour[10 * 4 + 2] = 0; //one coloured pixel in 64
	CHECK(EmissiveIsPicture(colour.data(), 64) && !EmissiveHasAlpha(colour.data(), 64));
	alpha = mask;
	alpha[20 * 4 + 3] = 0;
	CHECK(EmissiveIsPicture(alpha.data(), 64) && EmissiveHasAlpha(alpha.data(), 64));
	//on black, no alpha: the colour stays, coverage from the brightest channel (none below a fifth of "full", all from it up)
	const uint8_t in[5 * 4] = { 0, 0, 0, 255,   20, 10, 0, 255,   128, 64, 0, 255,   255, 220, 90, 255,   76, 0, 0, 255 };
	uint8_t out[5 * 4];
	PackEmissive(in, 5, false, 0.5f, out);
	for (int i = 0; i < 5; i++)
		CHECK(out[i * 4] == in[i * 4] && out[i * 4 + 1] == in[i * 4 + 1] && out[i * 4 + 2] == in[i * 4 + 2]);
	CHECK(out[3] == 0 && out[7] == 0); //black, and a dim halo (20/255 is under 0.1): adds only
	CHECK(out[11] == 255 && out[15] == 255); //half bright and up: takes the painting's place
	CHECK(out[19] > 100 && out[19] < 160); //0.3: halfway between 0.1 and 0.5
	PackEmissive(in, 5, false, 1.0f, out);
	CHECK(out[11] > 0 && out[11] < 128 && out[15] == 255);
	//its own alpha: a layer over the painting. The colour is stored times its coverage, worked out in linear light
	const uint8_t layer[3 * 4] = { 255, 200, 100, 255,   255, 200, 100, 128,   255, 200, 100, 0 };
	PackEmissive(layer, 3, true, 0.5f, out);
	CHECK(out[0] == 255 && out[1] == 200 && out[2] == 100 && out[3] == 255);
	CHECK(out[7] == 128 && out[11] == 0 && out[8] == 0 && out[9] == 0 && out[10] == 0);
	for (int c = 0; c < 3; c++)
		NEAR(SrgbToLinear(out[4 + c] / 255.0f), SrgbToLinear(layer[4 + c] / 255.0f) * 128.0f / 255.0f, 0.004);
	//[emissive] day
	Lights lights;
	std::string warnings;
	ParseLights("[emissive]\nday = 0.5\nstrength = 2\n", lights, warnings);
	CHECK(lights.hasEmissive && lights.emissive.day == 0.5f && lights.emissive.strength == 2.0f && warnings.empty());
	Lights again;
	ParseLights(WriteLights(lights, "test", ""), again, warnings);
	CHECK(again.emissive.day == 0.5f && warnings.empty());
	Lights bad;
	ParseLights("[emissive]\nday = 3\n", bad, warnings);
	CHECK(bad.emissive.day == 0.0f && warnings.find("day") != std::string::npos);
	CHECK(std::string(KindSuffix(EMISSIVE)) == "_EC" && std::string(KindSuffix(NIGHT)) == "_nt_A" && KIND_COUNT == 6);
}

//SCENE_LIGHT's rules (scene_light.h): battles and the world map
static void TestSceneLight()
{
	using scenelight::Where;
	using scenelight::Time;
	using scenelight::Resolve;
	using scenelight::NightHere;
	scenelight::Settings s;
	std::string w;
	//the file DEMASTER writes is understood without a warning and holds the defaults; the Lunar Cry's rule is there but off
	scenelight::Parse(scenelight::DefaultText(), s, w);
	CHECK(w.empty());
	CHECK(s.battle.night == -1 && s.battle.time == Time::Cycle && s.world.nightLevel == 0.3f && s.battle.ao == 0.5f);
	CHECK(s.rules.empty());
	CHECK(s.battle.parallax == 4.0f && s.world.parallax == 4.0f && s.world.ao == 0.5f && s.world.specular == 0.5f && s.world.relief == 1.0f);
	{
		scenelight::Settings p;
		std::string pw;
		scenelight::Parse("[world]\nparallax = 12\n[rule deep]\nwhere = battle\nparallax = 0\n[rule bad]\nparallax = 65\n", p, pw);
		CHECK(p.world.parallax == 12.0f && p.battle.parallax == 4.0f && pw.find("parallax") != std::string::npos);
		CHECK(scenelight::Resolve(p, Where::Battle, 3, -1).parallax == 0.0f && scenelight::Resolve(p, Where::World, -1, -1).parallax == 12.0f);
	}
	//the file shipped in DEMASTER_EXP is the defaults (when the tests run from the repository's folder)
	if (FILE* f = fopen("DEMASTER_EXP/scene_light.txt", "rb"))
	{
		std::string shipped;
		char buffer[4096];
		size_t got = 0;
		while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0)
			shipped.append(buffer, got);
		fclose(f);
		CHECK(shipped == scenelight::DefaultText());
	}
	CHECK(scenelight::DefaultText().find(";[rule lunar cry]") != std::string::npos);
	//the rule as it was written before, taken away again, for the checks below
	s = {};
	scenelight::Parse("[rule lunar cry]\nwhere = both\nmoment = 3000-65535\nday_tint = 1 0.94 0.92\nnight_tint = 1 0.5 0.45\n", s, w);
	CHECK(s.rules.size() == 1 && s.rules[0].name == "lunar cry" && s.rules[0].momentFrom == 3000 && s.rules[0].momentTo == 65535);
	CHECK(s.rules[0].hasNightTint && s.rules[0].hasDayTint && !s.rules[0].hasNightLevel && !s.rules[0].hasNight);
	//before the Lunar Cry: the place's own; after: the rule's tints over it, the rest untouched; unknown moment: not applied
	scenelight::Look before = Resolve(s, Where::World, -1, 2500), after = Resolve(s, Where::World, -1, 3150), unknown = Resolve(s, Where::Battle, 5, -1);
	CHECK(before.nightTint[2] == 1.0f && after.nightTint[0] == 1.0f && after.nightTint[2] == 0.45f && after.nightLevel == 0.3f);
	CHECK(unknown.nightTint[2] == 1.0f);
	float m[3];
	scenelight::Multiplier(before, 0.0, m);
	CHECK(m[0] == 1.0f && m[1] == 1.0f && m[2] == 1.0f); //by day: as painted
	scenelight::Multiplier(before, 1.0, m);
	NEAR(m[0], 0.3f * 0.55f, 1e-6);
	NEAR(m[2], 0.3f, 1e-6);
	scenelight::Multiplier(after, 0.5, m);
	NEAR(m[0], 0.5f * 1.0f + 0.5f * 0.3f * 1.0f, 1e-6);
	//does the night come here: the world always; a battle only outdoors unless a rule says so; time fixes it
	CHECK(NightHere(before, 0.7, true) == 0.7 && NightHere(before, 0.7, false) == 0.0 && NightHere(before, 3.0, true) == 1.0);
	//rules: stages, where, order, the keys
	scenelight::Parse("[battle]\nnight_level = 0.5\n[world]\nday_tint = 0.9 0.9 1\n"
		"[rule cave]\nwhere = battle\nstage = 7, 20-22\nnight = 0\n"
		"[rule dusk]\nstage = 22\ntime = night\nnight_tint = 1 0.7 0.5\nglow = 2\nao = 1\n"
		"[rule later]\nwhere = world\nmoment = 100-200\nnight = 1\nnight_level = 0.1\n", s, w);
	CHECK(w.empty() && s.rules.size() == 3);
	scenelight::Look cave = Resolve(s, Where::Battle, 21, 0), open = Resolve(s, Where::Battle, 8, 0), dusk = Resolve(s, Where::Battle, 22, 0);
	CHECK(cave.night == 0 && open.night == -1 && cave.nightLevel == 0.5f);
	CHECK(NightHere(cave, 1.0, true) == 0.0 && NightHere(open, 1.0, true) == 1.0);
	CHECK(dusk.night == 0 && dusk.time == Time::Night && NightHere(dusk, 0.0, false) == 1.0 && dusk.glow == 2.0f && dusk.ao == 1.0f
		&& dusk.nightTint[1] == 0.7f);
	scenelight::Look world = Resolve(s, Where::World, 22, 150), worldLater = Resolve(s, Where::World, 22, 201);
	CHECK(world.nightLevel == 0.1f && world.night == 1 && world.time == Time::Cycle && world.dayTint[0] == 0.9f);
	CHECK(worldLater.nightLevel == 0.3f && worldLater.night == -1);
	//what isn't understood is said and skipped
	scenelight::Parse("stray = 1\n[battle]\nnight = maybe\nnight_level = 2\ncolour = 1\n[weather]\nrain = 1\n[rule x]\nstage = a\nmoment = 5-3\nwhere = sea\n", s, w);
	CHECK(s.battle.night == -1 && s.battle.nightLevel == 0.3f && s.rules.size() == 1 && s.rules[0].stages.empty() && s.rules[0].momentFrom == -1);
	for (const char* part : { "can't read night", "can't read night_level", "unknown setting colour", "unknown section [weather]", "can't read stage",
		"can't read moment", "can't read where" })
		CHECK(w.find(part) != std::string::npos);
}

//SCENE_LIGHT: the battle camera from the game's projection registers, the sun over the stage, a stage's own lights
static void TestSceneCamera()
{
	//the registers DEMASTER logged in a battle on stage 101 with the camera at rest, and corners of that stage's sky and far
	//hills from the frame log of the same moment (nofield_20261117_212324, draws #330 and #331: x, y in the game's 640 x 480
	//units, rhw). If the registers are the camera that projected them, the sky's wall is a cylinder around the stage's middle
	int32_t reg[32]{};
	const auto pack = [](const int a, const int b) { return static_cast<int32_t>((static_cast<uint32_t>(b & 0xFFFF) << 16) | static_cast<uint32_t>(a & 0xFFFF)); };
	reg[0] = pack(-3921, 0); reg[1] = pack(1184, 129); reg[2] = pack(4071, 429); reg[3] = pack(-1177, 449); reg[4] = -3898;
	reg[5] = -547; reg[6] = 469; reg[7] = 8796; reg[24] = 160 << 16; reg[25] = 108 << 16; reg[26] = 512;
	scenelight::Camera camera;
	CHECK(scenelight::CameraFromRegisters(reg, camera));
	CHECK(camera.distance == 512.0 && camera.centreX == 160.0 && camera.centreY == 108.0 && camera.t[2] == 8796.0);
	const double sky[4][3] = { { 356.75, 165.5, 2.5943e-05 }, { 357.25, 3, 2.63929e-05 }, { 685.5, 168.75, 2.81302e-05 }, { 692.25, -7.5, 2.86607e-05 } };
	double radius[4];
	for (int i = 0; i < 4; i++)
	{
		const double z = 1.0 / sky[i][2];
		const double inView[3] = { (sky[i][0] / 2.0 - camera.centreX) * z / camera.distance, (sky[i][1] / 2.0 - camera.centreY) * z / camera.distance, z };
		double inStage[3];
		scenelight::PlaceToStage(camera, inView, inStage);
		radius[i] = std::sqrt(inStage[0] * inStage[0] + inStage[2] * inStage[2]);
	}
	for (int i = 0; i < 4; i++)
		CHECK(radius[i] > 29000.0 && radius[i] < 31000.0 && std::fabs(radius[i] - radius[0]) < 300.0); //one wall, about 30,000 out
	//a place survives the round trip, and a direction keeps its length
	const double place[3] = { 1250, 300, -400 };
	double view[3], back[3];
	scenelight::PlaceToView(camera, place, view);
	scenelight::PlaceToStage(camera, view, back);
	for (int i = 0; i < 3; i++)
		NEAR(back[i], place[i], 1.5); //(the registers' rotation has 12 bits)
	const double up[3] = { 0, -1, 0 };
	scenelight::DirectionToView(camera, up, view);
	NEAR(std::sqrt(view[0] * view[0] + view[1] * view[1] + view[2] * view[2]), 1.0, 0.002);
	CHECK(view[1] < -0.9); //that camera looks nearly level: the stage's up is up on the screen
	//the stage's middle is where the translation says, in front of the camera
	const double middle[3] = { 0, 0, 0 };
	scenelight::PlaceToView(camera, middle, view);
	CHECK(view[0] == -547.0 && view[1] == 469.0 && view[2] == 8796.0);
	//the game's own camera matrix (BATTLE_CAMERA): the same camera as those registers, from its 32 bytes
	{
		uint8_t m[32]{};
		for (int i = 0; i < 9; i++)
		{
			const int16_t v = static_cast<int16_t>(std::lround(camera.r[i] * 4096.0));
			memcpy(m + i * 2, &v, 2);
		}
		for (int i = 0; i < 3; i++)
		{
			const int32_t v = static_cast<int32_t>(camera.t[i]);
			memcpy(m + 20 + i * 4, &v, 4);
		}
		scenelight::Camera fromMatrix;
		CHECK(scenelight::CameraFromMatrix(m, camera, fromMatrix));
		double rotation = 1, translation = 1;
		scenelight::CameraDifference(fromMatrix, camera, rotation, translation);
		CHECK(rotation < 1e-3 && translation == 0 && fromMatrix.distance == camera.distance && fromMatrix.centreX == camera.centreX);
		uint8_t zero[32]{};
		CHECK(!scenelight::CameraFromMatrix(zero, camera, fromMatrix)); //not a rotation
	}
	//a corner the game hands OpenGL comes back to the place it was projected from (640 x 480 screen, rhw = 1 / z)
	{
		double v[3], st[3];
		scenelight::PlaceToView(camera, place, view);
		const double x = 2.0 * (camera.centreX + camera.distance * view[0] / view[2]), y = 2.0 * (camera.centreY + camera.distance * view[1] / view[2]);
		scenelight::ScreenToView(camera, x, y, 1.0 / view[2], v);
		for (int i = 0; i < 3; i++)
			NEAR(v[i], view[i], 1e-6 * std::fabs(view[2]));
		scenelight::PlaceToStage(camera, v, st);
		for (int i = 0; i < 3; i++)
			NEAR(st[i], place[i], 1.5);
	}
	//the sun's shadow map: around the fighters, the place under a fighter farther from the sun at the same spot of the map
	{
		const double overhead[3] = { 0.3, -0.9, 0.2 }; //towards the sun (the stage's y runs down)
		std::vector<float> fighters;
		for (const float x : { -1500.0f, 1500.0f })
			for (const float yy : { 0.0f, -1800.0f })
				for (const float z : { -300.0f, 300.0f })
					fighters.insert(fighters.end(), { x, yy, z });
		float m[16];
		CHECK(scenelight::FitSunShadow(overhead, fighters, 100.0, m));
		const auto map = [&](const double x, const double yy, const double z, double out[3])
		{
			for (int k = 0; k < 3; k++)
				out[k] = m[k] * x + m[4 + k] * yy + m[8 + k] * z + m[12 + k];
		};
		bool inside = true;
		for (size_t i = 0; i < fighters.size(); i += 3)
		{
			double o[3];
			map(fighters[i], fighters[i + 1], fighters[i + 2], o);
			inside = inside && o[0] > 0 && o[0] < 1 && o[1] > 0 && o[1] < 1 && o[2] > 0 && o[2] < 1;
		}
		CHECK(inside);
		//a fighter's head, and the ground the sun's ray through it reaches (y = 0 is the floor)
		const double len = std::sqrt(0.3 * 0.3 + 0.9 * 0.9 + 0.2 * 0.2);
		const double head[3] = { 1500, -1800, 300 }, t = 1800.0 / (0.9 / len);
		const double ground[3] = { head[0] - 0.3 / len * t, 0.0, head[2] - 0.2 / len * t };
		double a[3], b[3];
		map(head[0], head[1], head[2], a);
		map(ground[0], ground[1], ground[2], b);
		NEAR(a[0], b[0], 1e-4);
		NEAR(a[1], b[1], 1e-4);
		CHECK(b[2] > a[2] && b[2] < 1.0); //the ground is behind the head as the sun sees it, still on the map
		CHECK(!scenelight::FitSunShadow(overhead, std::vector<float>{}, 100.0, m));
	}
	//not a rotation: a row too long, rows not at right angles, no distance
	int32_t bad[32];
	std::copy(reg, reg + 32, bad);
	bad[0] = pack(-3921, 2000);
	CHECK(!scenelight::CameraFromRegisters(bad, camera));
	std::copy(reg, reg + 32, bad);
	bad[1] = pack(1184, -3921);
	bad[2] = pack(0, 1184);
	CHECK(!scenelight::CameraFromRegisters(bad, camera));
	std::copy(reg, reg + 32, bad);
	bad[26] = 0;
	CHECK(!scenelight::CameraFromRegisters(bad, camera));
	CHECK(scenelight::CameraFromRegisters(reg, camera));

	//the filter: a camera that moves a little is followed; a single odd reading is passed over; a cut is taken on its second frame
	scenelight::CameraFilter filter;
	scenelight::Camera use, other = camera, moved = camera;
	CHECK(!filter.Frame(false, camera, use)); //nothing yet
	CHECK(filter.Frame(true, camera, use) && use.t[2] == 8796.0);
	moved.t[0] += 150;
	CHECK(filter.Frame(true, moved, use) && use.t[0] == camera.t[0] + 150);
	//(something else's rotation: a quarter turn about the vertical, far away)
	const double turned[9] = { 0, 0, 1, 0, 1, 0, -1, 0, 0 };
	std::copy(turned, turned + 9, other.r);
	other.t[2] = 30000;
	CHECK(filter.Frame(true, other, use) && use.t[0] == camera.t[0] + 150); //one odd frame: the camera in use stays
	CHECK(filter.Frame(true, moved, use) && use.t[0] == camera.t[0] + 150);
	CHECK(filter.Frame(true, other, use) && use.t[2] == moved.t[2]); //a cut, first frame: not yet
	CHECK(filter.Frame(true, other, use) && use.t[2] == 30000.0); //second frame: taken
	CHECK(filter.Frame(false, other, use) && use.t[2] == 30000.0); //unreadable: the one in use stays
	filter.Reset();
	CHECK(!filter.Frame(false, other, use));

	//the sun over the stage: rises along the stage's +x (azimuth 0), stands sun_height high at noon, sets opposite; up is -y
	scenelight::Look look;
	look.sunAzimuth = 0;
	look.sunHeight = 60;
	scenelight::Sun rise = scenelight::SunOver(look, false, 0.0), noon = scenelight::SunOver(look, false, 0.5), set = scenelight::SunOver(look, false, 1.0);
	CHECK(rise.direction[0] > 0.99 && std::fabs(rise.direction[1]) < 0.01 && rise.strength < 0.05);
	NEAR(-noon.direction[1], std::sin(60.0 * 3.14159265358979 / 180.0), 1e-6);
	CHECK(noon.strength > 0.99 && set.direction[0] < -0.99 && !noon.moon);
	CHECK(noon.colour[0] > 0.5 && rise.colour[0] > rise.colour[2]); //white high up, orange at the horizon
	scenelight::Sun moon = scenelight::SunOver(look, true, 0.5);
	CHECK(moon.moon && moon.colour[2] > moon.colour[0]);

	//a stage's own lights in scene_light.txt
	scenelight::Settings s;
	std::string w;
	scenelight::Parse("[battle]\nsun = 0.5\nrelief = 2\nspecular = 0\nsun_azimuth = 90\nsun_height = 45\n"
		"[rule torches]\nstage = 12\nlight = -1500 600 800   3 1.6 0.6   4000 night\nlight = 1500 600 800  1 1 1  2500\n"
		"[rule more]\nstage = 12-13\nsun = 0\nlight = 0 100 0  2 2 2  900 day\nlight = 1 2 3  1 1 1  10\nlight = 4 5 6  1 1 1  10\n"
		"[rule world]\nwhere = world\nlight = 0 0 0  1 1 1  100\n", s, w);
	CHECK(w.empty() && s.battle.sun == 0.5f && s.battle.relief == 2.0f && s.battle.specular == 0.0f && s.battle.sunAzimuth == 90.0f && s.battle.sunHeight == 45.0f);
	scenelight::Look twelve = scenelight::Resolve(s, scenelight::Where::Battle, 12, 0), thirteen = scenelight::Resolve(s, scenelight::Where::Battle, 13, 0),
		elsewhere = scenelight::Resolve(s, scenelight::Where::Battle, 5, 0), map = scenelight::Resolve(s, scenelight::Where::World, -1, 0);
	CHECK(twelve.lights.size() == 4 && twelve.lights[0].when == 1 && twelve.lights[0].up == 600.0f && twelve.lights[0].colour[1] == 1.6f
		&& twelve.lights[1].when == 0 && twelve.lights[1].range == 2500.0f && twelve.lights[2].when == 2 && twelve.sun == 0.0f);
	CHECK(thirteen.lights.size() == 3 && elsewhere.lights.empty() && elsewhere.sun == 0.5f && map.lights.empty());
	scenelight::Parse("[rule x]\nlight = 1 2 3  1 1 1\nlight = 1 2 3  1 1 1  -5\nlight = 1 2 3  1 1 1  10 dusk\nlight = 1 2 3  1 1 1  10\nlight = 1 2 3  1 1 1  10\n"
		"light = 1 2 3  1 1 1  10\nlight = 1 2 3  1 1 1  10\nlight = 1 2 3  1 1 1  10\n[battle]\nsun = 2\nsun_height = 2\n", s, w);
	CHECK(s.rules.size() == 1 && s.rules[0].look.lights.size() == 4 && s.battle.sun == 0.35f && s.battle.sunHeight == 60.0f);
	for (const char* part : { "line 2: can't read light", "line 3: can't read light", "line 4: can't read light", "four lights at most", "can't read sun = 2",
		"can't read sun_height" })
		CHECK(w.find(part) != std::string::npos);
	w.clear();
	scenelight::Parse(scenelight::DefaultText(), s, w);
	CHECK(w.empty() && s.battle.sun == 0.35f && s.battle.sunAzimuth == 30.0f && s.rules.empty());
}

//The world map: which way is up in its space, from the facing of its ground as the camera sees it
static void TestWorldUp()
{
	//a camera above the ground looking down 35 degrees, turned 40 degrees about the world's up (the PlayStation's: -y)
	const double pitch = 35 * 3.14159265358979 / 180, yaw = 40 * 3.14159265358979 / 180;
	scenelight::Camera camera;
	camera.distance = 1024;
	camera.centreX = 160;
	camera.centreY = 112 + scenelight::WORLD_CENTRE_DOWN;
	//view = R x (world - eye): R = pitch about x after yaw about y (x right, y down, z ahead)
	const double cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
	const double yawM[9] = { cy, 0, -sy, 0, 1, 0, sy, 0, cy };
	const double pitchM[9] = { 1, 0, 0, 0, cp, -sp, 0, sp, cp };
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
		{
			double v = 0;
			for (int k = 0; k < 3; k++)
				v += pitchM[i * 3 + k] * yawM[k * 3 + j];
			camera.r[i * 3 + j] = v;
		}
	const double eye[3] = { 1000, -2000, -500 }; //2000 above the ground (y = 0; up is -y)
	for (int i = 0; i < 3; i++)
		camera.t[i] = -(camera.r[i * 3] * eye[0] + camera.r[i * 3 + 1] * eye[1] + camera.r[i * 3 + 2] * eye[2]);
	//the ground ahead of the camera, triangles of a gently rolling field, as the game hands them over
	std::vector<double> corners;
	const auto corner = [&](const double x, const double z)
	{
		const double world[3] = { x, -40.0 * std::sin(x / 300.0) * std::cos(z / 400.0), z };
		double view[3];
		scenelight::PlaceToView(camera, world, view);
		corners.insert(corners.end(), { 2.0 * (camera.centreX + camera.distance * view[0] / view[2]),
			2.0 * (camera.centreY + camera.distance * view[1] / view[2]), 1.0 / view[2] });
		return view[2] > 10;
	};
	const double forward[3] = { camera.r[6], camera.r[7], camera.r[8] }; //the view's z in the world
	bool allAhead = true;
	for (int a = 0; a < 12; a++)
		for (int b = 0; b < 12; b++)
		{
			const double x = eye[0] + forward[0] * 3000 + (a - 6) * 200, z = eye[2] + forward[2] * 3000 + (b - 6) * 200;
			allAhead = corner(x, z) && corner(x + 200, z) && corner(x, z + 200) && allAhead;
		}
	CHECK(allAhead);
	double facing[3] = { 0, 0, 0 };
	scenelight::AddFacing(camera, corners, facing);
	const double along = scenelight::FacingAlongUp(camera, facing);
	CHECK(along > 0.95);
	//the same picture from a camera whose space has up at +y (y flipped): the ground faces the other way along its y
	scenelight::Camera flipped = camera;
	for (int i = 0; i < 3; i++)
		flipped.r[i * 3 + 1] = -flipped.r[i * 3 + 1];
	CHECK(scenelight::FacingAlongUp(flipped, facing) < -0.95);
	double nothing[3] = { 0, 0, 0 };
	CHECK(scenelight::FacingAlongUp(camera, nothing) == 0);
	printf("world up: the ground faces %.3f along -y\n", along);
}

//DAY_NIGHT_SAVES: one time of day per save file, kept sealed (daynight.dat), and the time a save without one gets
static void TestSaveSlots()
{
	using namespace fieldpbr;
	SaveClocks k;
	k.Record("slot1_save01.ff8", 913, 227.0);
	k.Record("slot1_save02.ff8", 5000, 600.0);
	k.Record("slot1_save01.ff8", 1500, 100.0); //saved over slot 1, save 1: its entry is replaced
	CHECK(k.entries.size() == 2);
	CHECK(k.Find(913, 10) == nullptr); //the old save of that slot is gone
	const SaveClock* one = k.Find(1495, 10);
	CHECK(one != nullptr && one->file == "slot1_save01.ff8" && one->clock == 100.0);
	k.Record(7000, 50.0); //(no file: from v15.14's text)
	CHECK(k.entries.size() == 3);
	//sealed: back as it was; a changed byte, another file or the wrong start: nothing
	const std::string packed = PackSaveClocks(k, 0x1234567u);
	SaveClocks back;
	CHECK(UnpackSaveClocks(packed, back) && back.entries.size() == 3);
	if (back.entries.size() == 3)
	{
		CHECK(back.entries[0].file == "slot1_save02.ff8" && back.entries[0].play == 5000 && back.entries[0].clock == 600.0);
		CHECK(back.entries[2].file.empty() && back.entries[2].play == 7000);
	}
	CHECK(packed.find("slot1") == std::string::npos); //not readable as text
	bool allCaught = true;
	for (size_t i = 0; i < packed.size(); i++)
	{
		std::string changed = packed;
		changed[i] = static_cast<char>(changed[i] ^ 0x10);
		SaveClocks none;
		none.entries.push_back({});
		const bool read = UnpackSaveClocks(changed, none);
		allCaught = allCaught && !read && none.entries.size() == 1; //and `out` untouched
	}
	CHECK(allCaught);
	CHECK(!UnpackSaveClocks(packed.substr(0, packed.size() - 1), back));
	CHECK(!UnpackSaveClocks("play = 913 clock = 227\n", back));
	CHECK(PackSaveClocks(k, 1) != PackSaveClocks(k, 2)); //the nonce changes every byte after it
	SaveClocks empty;
	CHECK(UnpackSaveClocks(PackSaveClocks(empty, 9), back) && back.entries.empty());
	//a save without a time of its own: the new game's morning, and its play time on from there
	DayNightCycle cycle;
	NEAR(ClockFromPlay(cycle, 0), cycle.fade / 2, 1e-9);
	NEAR(ClockFromPlay(cycle, 100), cycle.fade / 2 + 100, 1e-9);
	NEAR(ClockFromPlay(cycle, static_cast<uint32_t>(cycle.Length()) * 3 + 7), cycle.fade / 2 + 7, 1e-9);
	CHECK(ClockFromPlay(cycle, 123456) == ClockFromPlay(cycle, 123456));
}

//EFFECT_LIGHTS: glows found on the grid, turned into lights of the stage, and kept steady from frame to frame
static void TestEffectLights()
{
	//a 40 x 30 grid with two glows: a big orange one around cell (10, 8) at depth 3000, a small blue one at (32, 20), 1500
	scenelight::GlowGrid grid;
	grid.width = 40;
	grid.height = 30;
	grid.cells.assign(40 * 30 * 5, 0.0f);
	const auto put = [&](const int x, const int y, const float r, const float g, const float b, const float depth)
	{
		float* c = &grid.cells[(static_cast<size_t>(y) * 40 + x) * 5];
		const float bright = 0.2126f * r + 0.7152f * g + 0.0722f * b;
		c[0] += r;
		c[1] += g;
		c[2] += b;
		c[3] += bright;
		c[4] += bright * depth;
	};
	for (int y = 6; y <= 10; y++)
		for (int x = 8; x <= 12; x++)
			put(x, y, 1.0f, 0.5f, 0.1f, 3000.0f);
	put(32, 20, 0.1f, 0.2f, 0.9f, 1500.0f);
	const std::vector<scenelight::Glow> glows = scenelight::FindGlows(grid, 4, 5, 0.02);
	CHECK(glows.size() == 2);
	if (glows.size() == 2)
	{
		NEAR(glows[0].x, 10.5 * 16, 1e-6); //the middle of cell 10: 168 on the 640-wide screen
		NEAR(glows[0].y, 8.5 * 16, 1e-6);
		NEAR(glows[0].depth, 3000, 1e-3);
		NEAR(glows[0].rgb[0], 25.0, 1e-4);
		CHECK(glows[0].spread > 16 && glows[0].spread < 40);
		NEAR(glows[1].x, 32.5 * 16, 1e-6);
		NEAR(glows[1].depth, 1500, 1e-3);
		CHECK(glows[1].spread == 0);
	}
	CHECK(scenelight::FindGlows(grid, 1, 5, 0.02).size() == 1);
	CHECK(scenelight::FindGlows(grid, 4, 5, 10.0).empty()); //nothing that bright
	//a glow back into the stage: where the camera saw it
	scenelight::Camera camera;
	camera.distance = 512;
	camera.centreX = 160;
	camera.centreY = 112;
	const double c = std::cos(0.3), s = std::sin(0.3);
	const double r[9] = { c, 0, -s, 0, 1, 0, s, 0, c };
	for (int i = 0; i < 9; i++)
		camera.r[i] = r[i];
	camera.t[0] = 100;
	camera.t[1] = 300;
	camera.t[2] = 6000;
	if (!glows.empty())
	{
		const scenelight::EffectLight light = scenelight::GlowToLight(camera, glows[0], 1.0);
		double view[3];
		scenelight::PlaceToView(camera, light.stage, view);
		NEAR(view[2], 3000, 1e-3);
		NEAR(2.0 * (camera.centreX + camera.distance * view[0] / view[2]), glows[0].x, 1e-6);
		NEAR(2.0 * (camera.centreY + camera.distance * view[1] / view[2]), glows[0].y, 1e-6);
		NEAR(light.colour[0], 25.0 / 16.0, 1e-5); //the summed light / 16
		CHECK(light.range >= 1500 && light.range <= 6000);
		//very bright: at most 4
		scenelight::Glow blaze = glows[0];
		for (double& v : blaze.rgb)
			v *= 100;
		const scenelight::EffectLight capped = scenelight::GlowToLight(camera, blaze, 1.0);
		NEAR(std::max({ capped.colour[0], capped.colour[1], capped.colour[2] }), 4.0, 1e-5);
	}
	//frame to frame: a light seen again keeps the brighter of now and before (faded); one not seen fades and goes
	scenelight::EffectLights lights;
	scenelight::EffectLight a;
	a.stage[0] = 0;
	a.colour[0] = 1.0f;
	a.range = 2000;
	lights.Frame({ a }, 0.75);
	CHECK(lights.Lights().size() == 1);
	scenelight::EffectLight dimmer = a;
	dimmer.stage[0] = 300; //moved a little
	dimmer.colour[0] = 0.2f;
	lights.Frame({ dimmer }, 0.75);
	CHECK(lights.Lights().size() == 1);
	if (!lights.Lights().empty())
	{
		NEAR(lights.Lights()[0].colour[0], 0.75, 1e-6);
		NEAR(lights.Lights()[0].stage[0], 300, 1e-9);
	}
	for (int i = 0; i < 20; i++)
		lights.Frame({}, 0.75);
	CHECK(lights.Lights().empty());
	//never more than four
	std::vector<scenelight::EffectLight> many;
	for (int i = 0; i < 7; i++)
	{
		scenelight::EffectLight l;
		l.stage[0] = i * 10000.0;
		l.colour[1] = 0.1f * (i + 1);
		l.range = 2000;
		many.push_back(l);
	}
	lights.Frame(many, 0.75);
	CHECK(lights.Lights().size() == scenelight::MAX_EFFECT_LIGHTS && lights.Lights()[0].colour[1] > lights.Lights()[3].colour[1]);
}

//LUT: the tables (strips, .cube), the shaders' arithmetic on the CPU, and which times are mixed when
static void TestLut()
{
	//the identity gives back what it gets, at any size
	std::mt19937 random(7);
	std::uniform_real_distribution<float> any(0.0f, 1.0f);
	for (const int size : { 2, 17, 32 })
	{
		const lutgrade::Table id = lutgrade::Identity(size);
		CHECK(id.Valid() && id.size == size);
		double worst = 0;
		for (int i = 0; i < 200; i++)
		{
			const float in[3] = { any(random), any(random), any(random) };
			float out[3];
			lutgrade::Apply(id, in, out);
			for (int c = 0; c < 3; c++)
				worst = std::max(worst, std::fabs(static_cast<double>(out[c] - in[c])));
		}
		CHECK(worst < 1e-4);
		CHECK(lutgrade::IsIdentity(id, 1e-5)); //16 bits a value
	}
	//a strip picture: 32 squares of 32 x 32 side by side (red right, green down, blue from square to square), and the same
	//one under the other
	const int n = 32;
	const auto f = [](const double r, const double g, const double b, double out[3])
	{
		out[0] = std::min(1.0, std::pow(r, 0.9) * 1.05);
		out[1] = g * 0.95 + 0.04 * b;
		out[2] = std::pow(b, 1.1) * 0.9 + 0.05;
	};
	std::vector<uint8_t> across(static_cast<size_t>(n) * n * n * 4), down(across.size());
	for (int b = 0; b < n; b++)
		for (int g = 0; g < n; g++)
			for (int r = 0; r < n; r++)
			{
				double v[3];
				f(r / (n - 1.0), g / (n - 1.0), b / (n - 1.0), v);
				uint8_t* a = &across[(static_cast<size_t>(g) * n * n + static_cast<size_t>(b) * n + r) * 4];
				uint8_t* d = &down[((static_cast<size_t>(b) * n + g) * n + r) * 4];
				for (int c = 0; c < 3; c++)
					a[c] = d[c] = static_cast<uint8_t>(std::lround(v[c] * 255));
				a[3] = d[3] = 255;
			}
	lutgrade::Table fromAcross, fromDown;
	std::string why;
	CHECK(lutgrade::FromStrip(across.data(), n * n, n, fromAcross, why));
	CHECK(lutgrade::FromStrip(down.data(), n, n * n, fromDown, why));
	CHECK(fromAcross.size == n && fromAcross.rgba == fromDown.rgba);
	CHECK(!lutgrade::IsIdentity(fromAcross, 1.0 / 255.0));
	{
		double worst = 0;
		for (int i = 0; i < 300; i++)
		{
			const float in[3] = { any(random), any(random), any(random) };
			float out[3];
			double want[3];
			lutgrade::Apply(fromAcross, in, out);
			f(in[0], in[1], in[2], want);
			for (int c = 0; c < 3; c++)
				worst = std::max(worst, std::fabs(out[c] - want[c]));
		}
		CHECK(worst < 0.01); //8 bits a value and the straight lines between points
	}
	CHECK(!lutgrade::FromStrip(across.data(), 100, 12, fromDown, why) && !why.empty()); //not n x n by n
	//.cube: the identity of size 3 in the file's order (red fastest); a domain of 0-2 is read again at 0-1
	{
		std::string text = "TITLE \"test\"\n# a comment\nLUT_3D_SIZE 3\n";
		for (int b = 0; b < 3; b++)
			for (int g = 0; g < 3; g++)
				for (int r = 0; r < 3; r++)
					text += std::to_string(r / 2.0) + " " + std::to_string(g / 2.0) + " " + std::to_string(b / 2.0) + "\n";
		lutgrade::Table cube;
		CHECK(lutgrade::FromCube(text, cube, why));
		CHECK(cube.size == 3 && cube.rgba == lutgrade::Identity(3).rgba);
		lutgrade::Table wide;
		CHECK(lutgrade::FromCube("DOMAIN_MIN 0 0 0\nDOMAIN_MAX 2 2 2\n" + text, wide, why));
		const float in[3] = { 0.8f, 0.2f, 0.5f };
		float out[3];
		lutgrade::Apply(wide, in, out);
		NEAR(out[0], 0.4, 0.002);
		NEAR(out[1], 0.1, 0.002);
		NEAR(out[2], 0.25, 0.002);
		CHECK(!lutgrade::FromCube("LUT_1D_SIZE 3\n0 0 0\n0.5 0.5 0.5\n1 1 1\n", cube, why));
		CHECK(!lutgrade::FromCube("LUT_3D_SIZE 3\n0 0 0\n", cube, why) && why.find("27") != std::string::npos);
		CHECK(!lutgrade::FromCube("0 0 0\n", cube, why));
	}
	//two LUTs mixed, and the strength
	{
		const lutgrade::Table id = lutgrade::Identity(n);
		const float in[3] = { 0.3f, 0.6f, 0.9f };
		float graded[3], half[3], none[3], mixed[3];
		lutgrade::Apply(fromAcross, in, graded);
		lutgrade::Apply(id, fromAcross, 1.0, 0.5, in, half);
		lutgrade::Apply(id, fromAcross, 1.0, 0.0, in, none);
		lutgrade::Apply(id, fromAcross, 0.25, 1.0, in, mixed);
		for (int c = 0; c < 3; c++)
		{
			NEAR(half[c], (in[c] + graded[c]) / 2, 1e-5);
			NEAR(none[c], in[c], 1e-6);
			NEAR(mixed[c], in[c] * 0.75 + graded[c] * 0.25, 1e-4);
		}
	}
	//when: the default cycle (480 s of day, 240 of night, 40 s fades, the evening at 0.9 of the day)
	fieldpbr::DayNightCycle cycle;
	const auto is = [&](const double t, const int from, const int to, const double mix)
	{
		const lutgrade::Blend b = lutgrade::At(cycle, t);
		return b.from == from && b.to == to && std::fabs(b.mix - mix) < 1e-6;
	};
	CHECK(is(0, lutgrade::DAWN, lutgrade::DAY, 0)); //the middle of the dawn's fade: all dawn
	CHECK(is(30, lutgrade::DAWN, lutgrade::DAY, 0.5)); //into the day over one and a half fades
	CHECK(is(200, lutgrade::DAY, lutgrade::DAY, 0));
	CHECK(is(432, lutgrade::DAY, lutgrade::DUSK, 0)); //the evening
	CHECK(is(456, lutgrade::DAY, lutgrade::DUSK, 0.5));
	CHECK(is(480, lutgrade::DUSK, lutgrade::NIGHT, 0)); //the middle of the dusk's fade: all dusk
	CHECK(is(510, lutgrade::DUSK, lutgrade::NIGHT, 0.5)); //the blue hour
	CHECK(is(600, lutgrade::NIGHT, lutgrade::NIGHT, 0));
	CHECK(is(690, lutgrade::NIGHT, lutgrade::DAWN, 0.5));
	CHECK(is(720 + 30, lutgrade::DAWN, lutgrade::DAY, 0.5)); //wrapped
	//no jumps anywhere in the cycle: each time's share changes a little from one moment to the next
	{
		const auto shares = [&](const double t, double out[lutgrade::SLOT_COUNT])
		{
			for (int i = 0; i < lutgrade::SLOT_COUNT; i++)
				out[i] = 0;
			const lutgrade::Blend b = lutgrade::At(cycle, t);
			out[b.from] += 1 - b.mix;
			out[b.to] += b.mix;
		};
		double worst = 0;
		for (double t = 0; t < cycle.Length(); t += 0.05)
		{
			double a[lutgrade::SLOT_COUNT], b[lutgrade::SLOT_COUNT];
			shares(t, a);
			shares(t + 0.05, b);
			for (int i = 0; i < lutgrade::SLOT_COUNT; i++)
				worst = std::max(worst, std::fabs(a[i] - b[i]));
		}
		CHECK(worst < 0.005);
		//a short night and long fades: still in order and without jumps
		fieldpbr::DayNightCycle odd;
		odd.day = 100;
		odd.night = 30;
		odd.fade = 25;
		worst = 0;
		for (double t = 0; t < odd.Length(); t += 0.05)
		{
			double a[lutgrade::SLOT_COUNT], b[lutgrade::SLOT_COUNT];
			const lutgrade::Blend x = lutgrade::At(odd, t), y = lutgrade::At(odd, t + 0.05);
			for (int i = 0; i < lutgrade::SLOT_COUNT; i++)
				a[i] = b[i] = 0;
			a[x.from] += 1 - x.mix;
			a[x.to] += x.mix;
			b[y.from] += 1 - y.mix;
			b[y.to] += y.mix;
			for (int i = 0; i < lutgrade::SLOT_COUNT; i++)
				worst = std::max(worst, std::fabs(a[i] - b[i]));
		}
		CHECK(worst < 0.02);
	}
	CHECK(lutgrade::Fixed(true) == (lutgrade::Blend{ lutgrade::NIGHT, lutgrade::NIGHT, 0 }));
	const lutgrade::Blend lunar = lutgrade::Lunar({ lutgrade::DUSK, lutgrade::NIGHT, 0.3 });
	CHECK(lunar.from == lutgrade::LUNAR_DUSK && lunar.to == lutgrade::LUNAR_NIGHT && lunar.mix == 0.3);
	CHECK(lutgrade::BlendText({ lutgrade::DUSK, lutgrade::NIGHT, 0.4 }) == "dusk 40% into night");
	CHECK(std::string(lutgrade::SlotName(lutgrade::LUNAR_DAWN)) == "lunar_dawn");
}

//DAY_NIGHT: the story's time of day (field_time.txt): Deling City follows the clock, night for two parts of the story
static void TestTimeRules()
{
	using namespace fieldpbr;
	CHECK(NameMatches("gl*", "glmall1") && NameMatches("GL*", "glMall1") && !NameMatches("gl*", "bgroom_1"));
	CHECK(NameMatches("*", "anything") && NameMatches("gl*1", "glmall1") && !NameMatches("gl*2", "glmall1"));
	CHECK(NameMatches("glyagu1", "glyagu1") && !NameMatches("glyagu1", "glyagu10") && NameMatches("g*a*u1", "glyagu1"));
	CHECK(NameMatches("*1", "glyagu1") && !NameMatches("", "glyagu1") && NameMatches("", ""));
	std::vector<TimeRule> rules;
	std::string warnings;
	ParseTimeRules(DefaultTimeRulesText(), rules, warnings);
	CHECK(warnings.empty());
	CHECK(rules.size() == 3);
	if (rules.size() == 3)
	{
		CHECK(rules[0].name == "deling city follows the clock" && rules[0].fields.size() == 1 && rules[0].notFields.size() == 1
			&& rules[0].notFields[0] == "glwitch3" && rules[0].momentFrom < 0 && rules[0].time == FieldTime::Cycle);
		CHECK(rules[1].momentFrom == 221 && rules[1].momentTo == 234 && rules[1].time == FieldTime::Night);
		CHECK(rules[2].momentFrom == 359 && rules[2].momentTo == 419 && rules[2].time == FieldTime::Night);
	}
	int which = -2;
	//the parade (the parade's field, the story at the float) and Laguna's dream: night
	CHECK(TimeByRules(rules, "glyagu1", 392, FieldTime::Cycle, &which) == FieldTime::Night && which == 2);
	CHECK(TimeByRules(rules, "glhtl1", 223, FieldTime::Cycle, &which) == FieldTime::Night && which == 1);
	CHECK(TimeByRules(rules, "glclub1", 226, FieldTime::Day, &which) == FieldTime::Night && which == 1);
	//the rest of the story in Deling follows the clock, even where a _pbr.txt says night
	CHECK(TimeByRules(rules, "glmall1", 350, FieldTime::Night, &which) == FieldTime::Cycle && which == 0);
	CHECK(TimeByRules(rules, "glmall1", 420, FieldTime::Cycle, &which) == FieldTime::Cycle && which == 0);
	CHECK(TimeByRules(rules, "glmall1", 3000, FieldTime::Cycle) == FieldTime::Cycle);
	//not Deling: its own time; the counter not known: only the rules without a moment
	CHECK(TimeByRules(rules, "glwitch3", 392, FieldTime::Day, &which) == FieldTime::Day && which == -1);
	CHECK(TimeByRules(rules, "bghall_1", 392, FieldTime::Night, &which) == FieldTime::Night && which == -1);
	CHECK(TimeByRules(rules, "glyagu1", -1, FieldTime::Night, &which) == FieldTime::Cycle && which == 0);
	//the edges of a range, a single moment, later rules over earlier ones
	CHECK(TimeByRules(rules, "glgate1", 359, FieldTime::Cycle) == FieldTime::Night);
	CHECK(TimeByRules(rules, "glgate1", 358, FieldTime::Cycle) == FieldTime::Cycle);
	CHECK(TimeByRules(rules, "glgate1", 419, FieldTime::Cycle) == FieldTime::Night);
	ParseTimeRules("[rule a]\nfield = bg*\ntime = night\n[rule b]\nfield = bghall_1 bgroom_1\nmoment = 100\ntime = day\n", rules, warnings);
	CHECK(rules.size() == 2 && warnings.empty());
	CHECK(TimeByRules(rules, "bgroom_1", 100, FieldTime::Cycle) == FieldTime::Day);
	CHECK(TimeByRules(rules, "bgroom_1", 101, FieldTime::Cycle) == FieldTime::Night);
	CHECK(TimeByRules(rules, "bgmd1_1", 100, FieldTime::Cycle) == FieldTime::Night);
	//what isn't understood is said and left out
	warnings.clear();
	ParseTimeRules("time = night\n[rule no field]\ntime = night\n[rule no time]\nfield = gl*\n[rule bad]\nfield = gl*\n"
		"moment = 400-300\ntime = dusk\ntime = day\ncolour = red\n[battle]\n", rules, warnings);
	CHECK(rules.size() == 1 && rules[0].name == "bad" && rules[0].momentFrom < 0 && rules[0].time == FieldTime::Day);
	CHECK(warnings.find("outside a [rule") != std::string::npos && warnings.find("has no field") != std::string::npos
		&& warnings.find("has no time") != std::string::npos && warnings.find("moment = 400-300") != std::string::npos
		&& warnings.find("time = dusk") != std::string::npos && warnings.find("unknown key colour") != std::string::npos
		&& warnings.find("[battle]") != std::string::npos);
	//the file shipped in DEMASTER_EXP is the defaults (when the tests run from the repository's folder)
	if (FILE* f = fopen("DEMASTER_EXP/field_time.txt", "rb"))
	{
		std::string shipped;
		char buffer[4096];
		size_t got = 0;
		while ((got = fread(buffer, 1, sizeof(buffer), f)) > 0)
			shipped.append(buffer, got);
		fclose(f);
		CHECK(shipped == DefaultTimeRulesText());
	}
}

int main(int argc, char** argv)
{
	TestTimeRules();
	TestLut();
	TestEffectLights();
	TestSaveSlots();
	TestWorldUp();
	TestSceneCamera();
	TestSceneLight();
	TestEmissivePicture();
	TestColour();
	TestSpace();
	TestSettings();
	TestLights();
	TestDayNight();
	TestLook();
	TestMovingLights();
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
