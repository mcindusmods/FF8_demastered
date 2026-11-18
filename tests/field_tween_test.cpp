//Tests for ff8_demaster/field_tween.cpp (smooth scroll, the in-between picture's places) and analog_move.cpp.
//  g++ -std=c++20 -O2 field_tween_test.cpp ../ff8_demaster/field_tween.cpp ../ff8_demaster/analog_move.cpp -o field_tween_test

#include "../ff8_demaster/analog_move.h"
#include "../ff8_demaster/field_tween.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace fieldtween;

static int checks = 0, failures = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define NEAR(a, b, tolerance) CHECK(std::fabs((a) - (b)) <= (tolerance))

//the game's arithmetic: a scroll in whole PlayStation pixels, two screen units each
static double Whole(const double truth)
{
	return std::floor(truth / PIXEL) * PIXEL;
}

//a steady scroll: how uneven the steps from frame to frame are, as the game draws it and as DEMASTER does
static void TestSteadyScroll()
{
	for (const double speed : { 0.3, 0.7, 1.1, 2.6, 4.3, -1.7 })
	{
		Axis axis;
		double before = 0, beforeWhole = 0, uneven = 0, unevenWhole = 0, furthest = 0;
		int counted = 0;
		for (int frame = 0; frame < 400; frame++)
		{
			const double whole = Whole(1000.0 + speed * frame);
			const double place = axis.Add(whole, true);
			furthest = std::max(furthest, std::fabs(place - whole));
			if (frame >= 60)
			{
				uneven += (place - before - speed) * (place - before - speed);
				unevenWhole += (whole - beforeWhole - speed) * (whole - beforeWhole - speed);
				counted++;
			}
			before = place;
			beforeWhole = whole;
		}
		uneven = std::sqrt(uneven / counted);
		unevenWhole = std::sqrt(unevenWhole / counted);
		CHECK(furthest <= LIMIT + 1e-9);
		CHECK(uneven < unevenWhole * 0.4); //the steps are at least 2.5 times more even
		if (!(uneven < unevenWhole * 0.4))
			printf("  speed %g: uneven %g, the game's %g\n", speed, uneven, unevenWhole);
	}
}

static void TestRestAndCuts()
{
	Axis axis;
	double place = 0;
	for (int frame = 0; frame < 100; frame++)
		place = axis.Add(Whole(500.0 + 1.3 * frame), true);
	//the scroll stops: the layer ends up exactly where the game has it, without ever going past the limit
	const double rest = Whole(500.0 + 1.3 * 99);
	double furthest = 0, lastMove = 0;
	for (int frame = 0; frame < 120; frame++)
	{
		const double next = axis.Add(rest, true);
		furthest = std::max(furthest, std::fabs(next - rest));
		lastMove = std::fabs(next - place);
		place = next;
	}
	CHECK(place == rest);
	CHECK(furthest <= LIMIT + 1e-9);
	CHECK(lastMove == 0);
	//a cut: taken as it is, and nothing tweened across it
	const double cut = axis.Add(rest + 300, true);
	CHECK(cut == rest + 300);
	CHECK(axis.Jumped());
	CHECK(!(axis.Add(rest + 302, true) == rest) && !axis.Jumped());
	//off: the game's places, untouched
	Axis plain;
	for (int frame = 0; frame < 50; frame++)
	{
		const double whole = Whole(100.0 + 0.9 * frame);
		CHECK(plain.Add(whole, false) == whole);
	}
	//a speed change the filter has to catch up with: never further than the limit
	Axis turning;
	double worst = 0;
	for (int frame = 0; frame < 300; frame++)
	{
		const double truth = frame < 100 ? 0.0 : frame < 200 ? (frame - 100) * 7.0 : 700.0 - (frame - 200) * 3.0;
		const double whole = Whole(truth);
		worst = std::max(worst, std::fabs(turning.Add(whole, true) - whole));
	}
	CHECK(worst <= LIMIT + 1e-9);
}

static void TestLayerTrack()
{
	LayerTrack track;
	uint32_t frame = 10;
	Advance(track, 100, 50, frame, true);
	CHECK(!track.tween);
	double now[2], between[2];
	ShiftNow(track, now);
	ShiftBetween(track, between);
	CHECK(now[0] == 0 && now[1] == 0 && between[0] == 0 && between[1] == 0);
	double lastExact = track.exact[0];
	for (int i = 1; i <= 80; i++)
	{
		Advance(track, Whole(100.0 + 0.9 * i), 50, ++frame, true);
		CHECK(track.tween);
		ShiftNow(track, now);
		ShiftBetween(track, between);
		//the in-between picture's place is halfway between the two frames' places
		NEAR(track.whole[0] + between[0], (lastExact + track.exact[0]) * 0.5, 1e-9);
		NEAR(track.whole[0] + now[0], track.exact[0], 1e-12);
		CHECK(between[1] == 0);
		lastExact = track.exact[0];
	}
	//a frame missed (the layer was not drawn): no tween from the old place
	frame += 2;
	Advance(track, 400, 50, frame, true);
	CHECK(!track.tween);
	ShiftBetween(track, between);
	CHECK(between[0] == 0);
	//a cut
	Advance(track, 402, 50, ++frame, true);
	CHECK(track.tween);
	Advance(track, 900, 50, ++frame, true);
	CHECK(!track.tween);
	//smoothing off, interpolation on: halfway between the game's own places
	LayerTrack plain;
	Advance(plain, 10, 0, 1, false);
	Advance(plain, 14, 0, 2, false);
	ShiftBetween(plain, between);
	NEAR(between[0], -2.0, 1e-12);
	ShiftNow(plain, now);
	CHECK(now[0] == 0);
}

static void TestCovering()
{
	//a layer far past both edges: moved freely
	NEAR(KeepCovering(-100, 900, 1.2, false), 1.2, 1e-12);
	NEAR(KeepCovering(-100, 900, -1.2, false), -1.2, 1e-12);
	//its left edge exactly at the picture's: not to the right; to the left is fine
	NEAR(KeepCovering(0, 900, 1.2, false), 0.0, 1e-12);
	NEAR(KeepCovering(0, 900, -1.2, false), -1.2, 1e-12);
	NEAR(KeepCovering(-0.5, 900, 1.2, false), 0.5, 1e-12);
	//its right edge at the picture's
	NEAR(KeepCovering(-300, 640, -1.0, false), 0.0, 1e-12);
	NEAR(KeepCovering(-300, 640, 1.0, false), 1.0, 1e-12);
	//exactly the picture: stays
	NEAR(KeepCovering(0, 640, 1.0, false), 0.0, 1e-12);
	NEAR(KeepCovering(0, 640, -1.0, false), 0.0, 1e-12);
	//a layer that ends inside the picture anyway: its edge may move
	NEAR(KeepCovering(40, 300, -1.0, false), -1.0, 1e-12);
	//top and bottom: the part the game shows starts 16 units in
	NEAR(KeepCovering(16, 900, 1.0, true), 0.0, 1e-12);
	NEAR(KeepCovering(10, 900, 1.5, true), 1.5 > 6.0 ? 6.0 : 1.5, 1e-12);
	NEAR(KeepCovering(15, 900, 1.5, true), 1.0, 1e-12);
	NEAR(KeepCovering(-200, 464, -1.0, true), 0.0, 1e-12);
	NEAR(KeepCovering(-200, 470, -1.5, true), -1.5, 1e-12);
	NEAR(KeepCovering(-200, 480, -1.5, true), 0.0, 1e-12);
}

static void TestProjection()
{
	//the game's field projection (frame log of bvtr_1)
	const float game[16] = { 0.003125f, 0, 0, 0, 0, -0.00416667f, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1 };
	float moved[16];
	ShiftProjection(game, 1.5, -0.5, moved);
	//a vertex as the game's shader makes it: (x w, y w, z w, w) with w = 1 / rhw
	const double x = 321.25, y = 100.5, w = 4000.0;
	const auto clip = [&](const float* m, const double px, const double py, double out[2])
	{
		const double cx = m[0] * px * w + m[4] * py * w + m[12] * w, cy = m[1] * px * w + m[5] * py * w + m[13] * w;
		const double cw = m[3] * px * w + m[7] * py * w + m[15] * w;
		out[0] = cx / cw;
		out[1] = cy / cw;
	};
	double a[2], b[2];
	clip(moved, x, y, a);
	clip(game, x + 1.5, y - 0.5, b);
	NEAR(a[0], b[0], 1e-6);
	NEAR(a[1], b[1], 1e-6);
	double perUnit[2];
	PixelsPerUnit(game, 2560, 1920, perUnit);
	NEAR(perUnit[0], 4.0, 1e-4);
	NEAR(perUnit[1], 4.0, 1e-4);
	NEAR(Snap(0.3, 4.0), 0.25, 1e-12);
	NEAR(Snap(-0.4, 4.0), -0.5, 1e-12);
	CHECK(Snap(0.1, 0.0) == 0.0);
	//in place
	float same[16];
	for (int i = 0; i < 16; i++)
		same[i] = game[i];
	ShiftProjection(same, 2, 2, same);
	NEAR(same[12], -1 + 2 * 0.003125, 1e-7);
}

struct GameVertex
{
	float x, y, z, rhw;
	uint32_t color, specular;
	float u, v;
};

//a 16x16 tile as the game draws it: cell (cx, cy) of the page at the layer's place plus the tile's place on its canvas
static void Tile(std::vector<GameVertex>& vertices, std::vector<uint32_t>& indices, const int cx, const int cy, const double screenX,
	const double screenY)
{
	const uint32_t first = static_cast<uint32_t>(vertices.size());
	for (int k = 0; k < 4; k++)
	{
		const int dx = k & 1, dy = k >> 1;
		GameVertex v{};
		v.x = static_cast<float>(screenX + dx * 32);
		v.y = static_cast<float>(screenY + dy * 32);
		v.rhw = 1;
		v.u = (cx * 16 + dx * 16) / 256.0f;
		v.v = (cy * 16 + dy * 16) / 256.0f;
		vertices.push_back(v);
	}
	for (const uint32_t i : { 0u, 1u, 2u, 1u, 3u, 2u })
		indices.push_back(first + i);
}

static void TestPlaceOfDraw()
{
	PageCells page;
	page.Size(256, 256);
	CHECK(page.cellsX == 16 && page.cellsY == 16);
	//layer 0: cells (0,0), (1,0) show canvas (160,96), (176,96); layer 3: cell (5,2) shows (16,32)
	page.Add(0, 0, 0, 160, 96);
	page.Add(16, 0, 0, 176, 96);
	page.Add(80, 32, 3, 16, 32);
	//a cell two layers share
	page.Add(96, 32, 3, 0, 0);
	page.Add(96, 32, 4, 0, 0);
	std::vector<GameVertex> vertices;
	std::vector<uint32_t> indices;
	const double layerX = -38, layerY = 14; //the canvas' corner on the screen
	Tile(vertices, indices, 0, 0, layerX + 2 * 160, layerY + 2 * 96);
	Tile(vertices, indices, 1, 0, layerX + 2 * 176, layerY + 2 * 96);
	const auto place = [&]()
	{
		return PlaceOfDraw(page, &vertices[0].x, sizeof(GameVertex), &vertices[0].u, sizeof(GameVertex), vertices.size(), indices.data(),
			indices.size());
	};
	DrawPlace p = place();
	CHECK(p.known && p.layer == 0 && p.triangles == 4);
	NEAR(p.x, layerX, 1e-4);
	NEAR(p.y, layerY, 1e-4);
	//a tile of another layer in the same draw: not one layer
	const size_t one = vertices.size(), oneIndices = indices.size();
	Tile(vertices, indices, 5, 2, 100, 100);
	CHECK(!place().known);
	//alone it is its own layer
	std::vector<GameVertex> other(vertices.begin() + one, vertices.end());
	const uint32_t otherIndices[6] = { 0, 1, 2, 1, 3, 2 };
	p = PlaceOfDraw(page, &other[0].x, sizeof(GameVertex), &other[0].u, sizeof(GameVertex), other.size(), otherIndices, 6);
	CHECK(p.known && p.layer == 3);
	NEAR(p.x, 100 - 2 * 16, 1e-4);
	NEAR(p.y, 100 - 2 * 32, 1e-4);
	vertices.resize(one);
	indices.resize(oneIndices);
	//a tile of the same layer that is somewhere else than its place says: not moving as one
	Tile(vertices, indices, 1, 0, layerX + 2 * 176 + 6, layerY + 2 * 96);
	CHECK(!place().known);
	vertices.resize(one);
	indices.resize(oneIndices);
	//a cell nothing is known about, a shared cell, an index past the end
	Tile(vertices, indices, 9, 9, 0, 0);
	CHECK(!place().known);
	vertices.resize(one);
	indices.resize(oneIndices);
	Tile(vertices, indices, 6, 2, 0, 0);
	CHECK(!place().known);
	vertices.resize(one);
	indices.resize(oneIndices);
	indices[3] = 99;
	CHECK(!place().known);
}

static void AddDraw(CharFrame& f, const uint32_t texture, const std::vector<float>& xy)
{
	CharDrawRef d;
	d.texture = texture;
	d.first = static_cast<uint32_t>(f.shown.size() / 4);
	d.count = static_cast<uint32_t>(xy.size() / 2);
	for (size_t i = 0; i < xy.size(); i += 2)
	{
		f.shown.push_back(xy[i]);
		f.shown.push_back(xy[i + 1]);
		f.shown.push_back(0.5f);
		f.shown.push_back(0.001f);
	}
	f.draws.push_back(d);
}

static void TestCharacters()
{
	CharFrame before, now;
	before.frame = 7;
	now.frame = 8;
	//texture 5: the same triangles, moved and bent; texture 6: one triangle fewer this frame (the game's own culling);
	//texture 7: new this frame; texture 8: far away from where it was (a cut)
	AddDraw(before, 5, { 0, 0, 10, 0, 0, 10 });
	AddDraw(before, 5, { 20, 0, 30, 0, 20, 10 });
	AddDraw(before, 6, { 100, 100, 110, 100, 100, 110, 100, 100, 110, 100, 110, 110 });
	AddDraw(before, 8, { 0, 300, 10, 300, 0, 310 });
	AddDraw(now, 5, { 4, 2, 14, 2, 4, 14 });
	AddDraw(now, 5, { 24, 2, 34, 2, 24, 12 });
	AddDraw(now, 6, { 104, 100, 114, 100, 104, 110 });
	AddDraw(now, 7, { 50, 50, 60, 50, 50, 60 });
	AddDraw(now, 8, { 400, 300, 410, 300, 400, 310 });
	std::vector<float> out;
	const size_t tweened = TweenCharacters(before, now, out);
	CHECK(out.size() == now.shown.size());
	CHECK(tweened == 3);
	//vertex by vertex
	NEAR(out[0], 2, 1e-6);
	NEAR(out[1], 1, 1e-6);
	NEAR(out[2 * 4 + 1], 12, 1e-6);
	NEAR(out[3 * 4], 22, 1e-6);
	NEAR(out[3], 0.001f, 1e-9);
	//as a whole: the middles are (105, 103.33) before and (107.33, 103.33) now
	NEAR(out[6 * 4], 104 - (107.3333 - 105.0) / 2, 1e-3);
	//new, and cut: where they are
	NEAR(out[9 * 4], 50, 1e-6);
	NEAR(out[12 * 4], 400, 1e-6);
	//not the frame right before: nothing moves
	before.frame = 5;
	CHECK(TweenCharacters(before, now, out) == 0);
	CHECK(out == now.shown);
}

static void TestPacer()
{
	Pacer pacer;
	CHECK(!pacer.HalfRate());
	for (int i = 0; i < 20; i++)
		pacer.Frame(1.0 / 30.0, false);
	CHECK(pacer.HalfRate() && !pacer.TooSlow());
	pacer.Frame(3.0, false); //a load
	CHECK(pacer.HalfRate());
	for (int i = 0; i < 200; i++)
		pacer.Frame(1.0 / 29.0, true);
	CHECK(!pacer.TooSlow());
	NEAR(pacer.Interval(), 1.0 / 29.0, 1e-4);
	for (int i = 0; i < 200; i++)
		pacer.Frame(1.0 / 24.0, true);
	CHECK(pacer.TooSlow());
	Pacer sixty;
	for (int i = 0; i < 20; i++)
		sixty.Frame(1.0 / 60.0, false);
	CHECK(!sixty.HalfRate());
}

static void TestStick()
{
	using analogmove::FromPad;
	CHECK(!FromPad(0, 0, 0.25).active);
	CHECK(!FromPad(0.1, -0.2, 0.25).active && FromPad(0.1, -0.2, 0.25).x == 128);
	//just outside the dead zone: the smallest push, in the stick's direction
	analogmove::Stick s = FromPad(0.26, 0, 0.25);
	CHECK(s.active && s.y == 128);
	NEAR(s.x, 128 + 127 * analogmove::SMALLEST, 2.5);
	//full throws: right is more, up is less
	s = FromPad(1, 0, 0.25);
	CHECK(s.x == 255 && s.y == 128);
	s = FromPad(0, 1, 0.25);
	CHECK(s.x == 128 && s.y == 1);
	s = FromPad(-1, -1, 0.25);
	CHECK(s.x < 60 && s.y > 196 && (128 - s.x) == (s.y - 128));
	//any angle keeps its angle
	for (int degrees = 0; degrees < 360; degrees += 7)
	{
		const double a = degrees * 3.14159265358979 / 180.0;
		s = FromPad(0.8 * std::cos(a), 0.8 * std::sin(a), 0.25);
		CHECK(s.active);
		const double back = std::atan2(-(s.y - 128.0), s.x - 128.0);
		double off = std::fabs(back - a);
		if (off > 3.14159265358979)
			off = 2 * 3.14159265358979 - off;
		CHECK(off < 0.02);
	}
	CHECK(!FromPad(std::nan(""), 0, 0.25).active);
	//the d-pad as a stick pushed all the way (up 0x1000, right 0x2000, down 0x4000, left 0x8000)
	{
		using analogmove::FromDpad;
		CHECK(!FromDpad(0).active && !FromDpad(0x0FFF).active);
		CHECK(!FromDpad(0x1000 | 0x4000).active && !FromDpad(0xF000).active); //opposites cancel
		analogmove::Stick d = FromDpad(0x2000);
		CHECK(d.active && d.x == 255 && d.y == 128);
		d = FromDpad(0x1000);
		CHECK(d.active && d.x == 128 && d.y == 1);
		d = FromDpad(0x8000 | 0x4000 | 0x40); //left and down, with another button
		CHECK(d.active && d.x == 38 && d.y == 218);
		d = FromDpad(0x1000 | 0x4000 | 0x2000); //up and down cancel: right
		CHECK(d.x == 255 && d.y == 128);
	}
	//the swing: the direction given turns towards the one held, the short way round, at the speed set
	{
		using analogmove::FromDpad;
		const auto angle = [](const analogmove::Stick& s) { return std::atan2(s.y - 128.0, s.x - 128.0) * 180.0 / 3.14159265358979323846; };
		const auto push = [](const analogmove::Stick& s) { return std::hypot(s.x - 128.0, s.y - 128.0); };
		analogmove::Swing swing;
		const double step = 1.0 / 30.0;
		double now = 10.0;
		//from rest: the first push goes straight where it points
		analogmove::Stick s = swing.Turn(FromDpad(0x2000), 720.0, now);
		CHECK(s.x == 255 && s.y == 128);
		//right to up (90 degrees) at 720 a second: 24 degrees a step, there on the fourth step and exact from then on
		double before = 0.0;
		for (int i = 1; i <= 5; i++)
		{
			now += step;
			s = swing.Turn(FromDpad(0x1000), 720.0, now);
			CHECK(s.active);
			NEAR(push(s), 127.0, 1.5);
			if (i < 4)
			{
				NEAR(angle(s), -24.0 * i, 1.0);
				CHECK(angle(s) < before);
				before = angle(s);
			}
			else
				CHECK(s.x == 128 && s.y == 1);
		}
		//up to down-left (135 degrees): the short way goes through left, never through right
		for (int i = 0; i < 8; i++)
		{
			now += step;
			s = swing.Turn(FromDpad(0x4000 | 0x8000), 720.0, now);
			CHECK(s.x <= 128);
		}
		CHECK(s.x == 38 && s.y == 218);
		//released, and pushed again within a moment: still swinging from where it was
		now += step;
		CHECK(!swing.Turn(FromDpad(0), 720.0, now).active);
		now += step;
		s = swing.Turn(FromDpad(0x2000), 720.0, now);
		CHECK(s.active && !(s.x == 255 && s.y == 128));
		//after a rest: straight where it points again
		now += 1.0;
		s = swing.Turn(FromDpad(0x8000), 720.0, now);
		CHECK(s.x == 1 && s.y == 128);
		//speed 0: no swing at all
		now += step;
		s = swing.Turn(FromDpad(0x2000), 0.0, now);
		CHECK(s.x == 255 && s.y == 128);
		//a half-pushed stick keeps its push while its direction swings
		analogmove::Swing soft;
		analogmove::Stick half;
		half.x = 128 + 60;
		half.y = 128;
		half.active = true;
		soft.Turn(half, 360.0, 20.0);
		half.x = 128;
		half.y = 128 + 60;
		s = soft.Turn(half, 360.0, 20.0 + step);
		NEAR(push(s), 60.0, 1.5);
		NEAR(angle(s), 12.0, 1.5);
		//a long gap between steps (a hitch) turns no more than a tenth of a second's worth
		analogmove::Swing hitch;
		hitch.Turn(FromDpad(0x2000), 100.0, 30.0);
		s = hitch.Turn(FromDpad(0x8000), 100.0, 30.25);
		NEAR(std::fabs(angle(s)), 10.0, 1.5);
	}
}

int main()
{
	TestSteadyScroll();
	TestRestAndCuts();
	TestLayerTrack();
	TestProjection();
	TestCovering();
	TestPlaceOfDraw();
	TestCharacters();
	TestPacer();
	TestStick();
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
