#pragma once
//LUT's rules (no game, no Windows, no OpenGL): the colour grade of the time of day.
//
//A LUT (look-up table) says what every colour becomes: a cube of colours, n points along each of red, green and blue, and
//in between the eight nearest are mixed (trilinear). DEMASTER grades the picture with one by the clock: a set of four, day,
//dusk, night and dawn, and four for the Lunar Cry (lunar_day ...), cross-fading from one to the next as the day goes. The
//grade is done by the shaders themselves (main.frag for what the game draws, demaster_field.frag and demaster_chara.frag
//for what DEMASTER draws), on the finished colour of each draw that is the scene (backgrounds, models, battle stages, the
//world map), not on the menus, the text or effects that add light. texturepatch_v2_lut.cpp is the DLL's side;
//tests/field_pbr_test.cpp and tests/scene_shader_test.cpp test this.
//
//The files: DEMASTER_EXP\lut\<name>.png (or .cube), name: day, dusk, night, dawn, lunar_day, lunar_dusk, lunar_night,
//lunar_dawn. One place can have its own: lut\field\<field>\, lut\battle\a0stgNNN\, lut\world\ (each file it has replaces
//the whole game's for that time; the others stay the whole game's). A missing time leaves the colours as they are (a
//missing lunar one: the ordinary one of that time).
//	.png   a strip of n squares of n x n side by side (n x n wide, n high; 1024 x 32 for n = 32, as ReShade's and Unreal's
//	       LUTs): in a square red goes right, green down; the squares are blue's steps. Also n wide and n x n high (the
//	       squares one under the other)
//	.cube  Adobe's / Resolve's text format (LUT_3D_SIZE, DOMAIN_MIN, DOMAIN_MAX, then the table, red fastest)
//The colours a LUT takes and gives are the picture's own (sRGB, as you see them), 0-1.

#include "field_pbr.h"

#include <cstdint>
#include <string>
#include <vector>

namespace lutgrade
{
	enum Slot { DAY, DUSK, NIGHT, DAWN, LUNAR_DAY, LUNAR_DUSK, LUNAR_NIGHT, LUNAR_DAWN, SLOT_COUNT };
	const char* SlotName(int slot); //"day", ..., "lunar_dawn" (the file's name)

	constexpr int MIN_SIZE = 2, MAX_SIZE = 65;

	//a LUT as DEMASTER keeps it: the horizontal strip (size x size wide, size high; x = red + size x blue, y = green),
	//4 values a point (RGBA, 65535 = 1; alpha 65535)
	struct Table
	{
		int size = 0;
		std::vector<uint16_t> rgba;
		bool Valid() const { return size >= MIN_SIZE && size <= MAX_SIZE && rgba.size() == static_cast<size_t>(size) * size * size * 4; }
	};
	//the colours as they are
	Table Identity(int size);
	//the table leaves every colour as it is, to within `tolerance` (0-1) at its points (DEMASTER then grades with nothing)
	bool IsIdentity(const Table& table, double tolerance);
	//a picture (RGBA8, rows top first) of either strip. why: what was wrong (false)
	bool FromStrip(const uint8_t* rgba, int width, int height, Table& table, std::string& why);
	//a .cube file's text
	bool FromCube(const std::string& text, Table& table, std::string& why);
	//What a colour becomes, as the shaders work it out: the two squares of blue's steps around it, each read bilinearly at
	//its red and green, mixed by blue (in, out: 0-1)
	void Apply(const Table& table, const float in[3], float out[3]);
	//the same with two tables mixed (0: a, 1: b) and only `strength` of the grade (0: as it was)
	void Apply(const Table& a, const Table& b, double mix, double strength, const float in[3], float out[3]);

	//Which two of the times are mixed, and how much of the second (0-1)
	struct Blend
	{
		int from = DAY, to = DAY;
		double mix = 0;
		bool operator==(const Blend& o) const { return from == o.from && to == o.to && mix == o.mix; }
	};
	//The grade at `time` seconds into the cycle (fieldpbr::DayNightCycle: the dawn's fade is around 0, the dusk's around
	//day). The dusk comes with the evening (DAY_NIGHT_EVENING: from `evening` of the day, as the day's lights go) and is
	//fullest in the middle of the dusk's fade, when the sun is down; it turns into the night over one and a half fades after
	//(the blue hour). The dawn comes over the one and a half fades before the middle of the dawn's fade, and goes into the
	//day over the next one and a half fades (at least a tenth of the day).
	Blend At(const fieldpbr::DayNightCycle& cycle, double time);
	//always day, or always night
	Blend Fixed(bool night);
	//the same times of the Lunar Cry's set
	Blend Lunar(const Blend& blend);
	//"dusk 40% into night" for the log
	std::string BlendText(const Blend& blend);
}
