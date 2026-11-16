//Tests for ff8_demaster/field_chars.cpp (characters from the game's projected triangles).
//  g++ -std=c++20 -O2 field_chars_test.cpp ../ff8_demaster/field_chars.cpp ../ff8_demaster/field_pbr.cpp -o field_chars_test

#include "../ff8_demaster/field_chars.h"

#include <cmath>
#include <cstdio>

using namespace fieldchars;

static int checks = 0, failures = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NEAR(a, b, tolerance) CHECK(std::fabs((a) - (b)) <= (tolerance))

static Projection Proj()
{
	Projection p;
	p.offsetX = 306;
	p.offsetY = 224;
	p.space.zoom = 400;
	p.space.originX = 168;
	p.space.originY = 120;
	return p;
}

static void TestProjection()
{
	const Projection p = Proj();
	//a vertex of the bvtr_1 log: (399.75, 380.5), rhw 0.000252525 -> game depth 3960
	const V3 v = p.ToView(399.75, 380.5, 0.000252525);
	NEAR(v.z, 3960.0 / 16384.0, 1e-4);
	double x = 0, y = 0;
	CHECK(p.ToScreen(v, x, y));
	NEAR(x, 399.75, 1e-3);
	NEAR(y, 380.5, 1e-3);
	//the camera axis is the canvas origin
	const V3 axis = p.ToView(306 + 168 * 2, 224 + 120 * 2, 1.0 / 2000.0);
	NEAR(axis.x, 0.0, 1e-6);
	NEAR(axis.y, 0.0, 1e-6);
}

static void TestNormals()
{
	//a front-facing triangle (screen area < 0 with y down) gets a normal towards the camera (-z)
	const Projection p = Proj();
	std::vector<V3> pos = { p.ToView(100, 100, 1.0 / 3000), p.ToView(100, 140, 1.0 / 3000), p.ToView(140, 100, 1.0 / 3000) };
	const double area = (100 - 100) * (100 - 100) - (140 - 100) * (140 - 100); //(b-a) x (c-a) on screen: negative
	CHECK(area < 0);
	std::vector<uint64_t> keys = { 1, 2, 3 };
	std::vector<V3> n;
	SmoothNormals(pos, keys, 70, n);
	CHECK(n.size() == 3 && n[0].z < -0.9f);

	//a roof: two faces sharing an edge, 20 degrees apart -> the shared corners average; 100 degrees apart -> they don't
	for (const float fold : { 20.0f, 100.0f })
	{
		const float r = fold * 3.14159265f / 180.0f;
		const V3 a{ 0, 0, 1 }, b{ 0, 1, 1 }; //the ridge
		const V3 c{ -1, 0, 1 }; //face 1 lies flat (normal -z or +z)
		const V3 d{ std::cos(r), 0, 1 - std::sin(r) }; //face 2 tilted by fold
		std::vector<V3> roof = { a, b, c, a, d, b };
		std::vector<uint64_t> k = { 10, 11, 12, 10, 13, 11 };
		std::vector<V3> out;
		SmoothNormals(roof, k, 70, out);
		const V3 n1 = Normalize(Cross(b - a, c - a)), n2 = Normalize(Cross(d - a, b - a));
		const float shared = Dot(out[0], n1);
		if (fold < 70)
		{
			CHECK(shared < 0.999f); //bent towards face 2
			NEAR(Dot(out[0], out[3]), 1.0f, 1e-4f); //the same normal at the shared vertex
		}
		else
		{
			NEAR(shared, 1.0f, 1e-4f);
			NEAR(Dot(out[3], n2), 1.0f, 1e-4f);
		}
		NEAR(Dot(out[2], n1), 1.0f, 1e-4f); //a corner of face 1 only
	}
	//a flat face takes its corners' average instead of nothing
	std::vector<V3> degenerate = { { 0, 0, 1 }, { 1, 0, 1 }, { 2, 0, 1 }, { 0, 0, 1 }, { 0, 1, 1 }, { 1, 0, 1 } };
	std::vector<uint64_t> dk = { 1, 2, 3, 1, 4, 2 };
	std::vector<V3> dn;
	SmoothNormals(degenerate, dk, 70, dn);
	NEAR(std::fabs(dn[0].z), 1.0f, 1e-4f);
}

static std::vector<GameVertex> DiscAt(const float cx, const float cy, const float radius, const uint8_t grey)
{
	std::vector<GameVertex> out;
	for (int i = 0; i < 8; i++)
	{
		const float a0 = i * 3.14159265f / 4, a1 = (i + 1) * 3.14159265f / 4;
		GameVertex c, r0, r1;
		c.x = cx;
		c.y = cy;
		c.rhw = 1.0f / 3960;
		c.color[0] = c.color[1] = c.color[2] = grey;
		r0.x = cx + radius * std::cos(a0);
		r0.y = cy + radius * 0.4f * std::sin(a0);
		r1.x = cx + radius * std::cos(a1);
		r1.y = cy + radius * 0.4f * std::sin(a1);
		r0.color[0] = r0.color[1] = r0.color[2] = 0;
		r1.color[0] = r1.color[1] = r1.color[2] = 0;
		out.push_back(c);
		out.push_back(r0);
		out.push_back(r1);
	}
	return out;
}

static void TestDiscs()
{
	std::vector<GameVertex> tris = DiscAt(400, 380, 42, 96);
	const std::vector<GameVertex> second = DiscAt(330, 430, 50, 96);
	tris.insert(tris.end(), second.begin(), second.end());
	const std::vector<Disc> discs = ReadDiscs(tris);
	CHECK(discs.size() == 2);
	for (const Disc& d : discs)
	{
		CHECK(d.radius > 40 && d.radius < 51);
		NEAR(d.darkness, 96 / 255.0f, 1e-4f);
	}
}

//a box of triangles standing at x, z (view), height h
static void AddBody(std::vector<V3>& pos, std::vector<unsigned>& tex, const unsigned texture, const float x, const float z, const float h)
{
	for (int i = 0; i < 20; i++)
	{
		const float y0 = -h * i / 20.0f, y1 = -h * (i + 1) / 20.0f;
		pos.push_back({ x - 0.02f, y0, z });
		pos.push_back({ x + 0.02f, y0, z });
		pos.push_back({ x, y1, z - 0.01f });
		tex.push_back(texture);
	}
}

static void TestGroup()
{
	std::vector<V3> pos;
	std::vector<unsigned> tex;
	AddBody(pos, tex, 7, -0.1f, 0.25f, 0.12f);
	AddBody(pos, tex, 7, 0.1f, 0.25f, 0.12f); //a twin: same texture
	AddBody(pos, tex, 9, 0.0f, 0.4f, 0.12f);
	const V3 up{ 0, -1, 0 };
	//no anchors: one character per texture
	std::vector<Character> c = Group(pos, tex, {}, up);
	CHECK(c.size() == 2);
	//two anchors under the twins: they come apart
	std::vector<Anchor> anchors(2);
	anchors[0].position = { -0.1f, 0, 0.25f };
	anchors[0].darkness = 0.3f;
	anchors[1].position = { 0.1f, 0, 0.25f };
	anchors[1].darkness = 0.4f;
	c = Group(pos, tex, anchors, up);
	CHECK(c.size() == 3);
	int twins = 0;
	for (const Character& ch : c)
		if (ch.texture == 7)
		{
			twins++;
			CHECK(ch.triangles.size() == 20);
			CHECK(ch.anchor >= 0);
			NEAR(ch.darkness, ch.anchor == 0 ? 0.3f : 0.4f, 1e-6f);
			NEAR(ch.halfSize.y, 0.06f, 1e-4f);
			NEAR(ch.feet.y, 0.0f, 1e-4f);
			CHECK(ch.head.y < -0.1f); //near the top (y down)
		}
		else
			CHECK(ch.anchor < 0 && ch.darkness > 0.37f && ch.darkness < 0.39f);
	CHECK(twins == 2);
}

static void TestFacing()
{
	FacingCalibration f;
	const double base = 1.0;
	for (int i = 0; i < 40; i++)
	{
		const int facing = (i * 37) & 0xFF;
		const double angle = base - facing * 2 * 3.14159265358979 / 256.0;
		f.Add(facing, std::cos(angle) * 2.0, std::sin(angle) * 2.0);
	}
	CHECK(f.known);
	CHECK(f.sense == -1);
	NEAR(std::remainder(f.Angle(64) - (base - 3.14159265358979 / 2), 2 * 3.14159265358979), 0.0, 1e-6);
	//walks that don't agree with any facing: not known
	FacingCalibration g;
	for (int i = 0; i < 48; i++)
		g.Add((i * 91) & 0xFF, std::cos(i * 1.7), std::sin(i * 1.7));
	CHECK(!g.known);
}

int main()
{
	TestProjection();
	TestNormals();
	TestDiscs();
	TestGroup();
	TestFacing();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
