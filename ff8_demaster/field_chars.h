#pragma once
//Characters and field models (FIELD_PBR_CHARACTERS, FIELD_PBR_SHADOWS, FIELD_PBR_REFLECTIONS, FIELD_AVATARS): what DEMASTER
//works out from the triangles the game sends. No game, Windows or OpenGL code, so tests/field_chars_test.cpp builds it on
//its own; texturepatch_v2_fieldChars.cpp is the game side.
//
//The game sends every model already projected (TLVERTEX): x, y on its 640 x 480 screen, the depth buffer value, and
//rhw = 1 / (4 x the camera distance its GTE worked out). That is enough to put every vertex back in view space (see
//field_pbr.h): the canvas spot from x and y, the distance from rhw. DEMASTER does that for the whole character, works out
//smooth normals, and keeps the characters apart (one texture per model, the game's round shadows for twins).

#include "field_pbr.h"

#include <cstdint>
#include <vector>

namespace fieldchars
{
	struct V3
	{
		float x = 0, y = 0, z = 0;
	};
	inline V3 operator+(const V3& a, const V3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	inline V3 operator-(const V3& a, const V3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	inline V3 operator*(const V3& a, const float s) { return { a.x * s, a.y * s, a.z * s }; }
	inline float Dot(const V3& a, const V3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	inline V3 Cross(const V3& a, const V3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
	float Length(const V3& a);
	V3 Normalize(const V3& a, const V3& fallback = { 0, 0, -1 });
	inline V3 ToV3(const fieldpbr::Vec3& v) { return { static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z) }; }

	//one vertex as the game sends it
	struct GameVertex
	{
		float x = 0, y = 0, z = 0, rhw = 0; //screen (640 x 480), depth buffer value, 1 / game depth
		uint8_t color[4] = { 255, 255, 255, 255 }; //as sent: blue, green, red, alpha
		float u = 0, v = 0;
	};

	//The game's screen to view space
	struct Projection
	{
		double offsetX = 0, offsetY = 0; //where canvas (0, 0) lands on the game's screen (measured from the background tiles)
		double screenPerCanvas = 2.0; //game screen pixels per native canvas pixel
		fieldpbr::Space space; //zoom and the camera axis on the canvas
		double gameDepth = fieldpbr::GAME_DEPTH_PER_VIEW;
		V3 ToView(double x, double y, double rhw) const;
		bool ToScreen(const V3& view, double& x, double& y) const;
	};

	//Smooth normals for a triangle list (positions: 3 per triangle, view space). Corners with the same key (the same model
	//vertex: the game sends it at exactly the same place every time) share the normals of their faces, but only of faces
	//within creaseDegrees of the corner's own face, so hard edges (a sole, a collar) stay hard. Outward is
	//cross(b - a, c - a): the game winds every polygon the same way in 3D. Flat (zero-area) faces get their corners' average.
	void SmoothNormals(const std::vector<V3>& positions, const std::vector<uint64_t>& keys, float creaseDegrees, std::vector<V3>& normals);

	//The game's round shadows: one draw, 8 triangles per character (centre vertex grey, the rim black)
	struct Disc
	{
		double x = 0, y = 0, rhw = 0; //centre on the game's screen
		double radius = 0; //game screen pixels (the widest)
		float darkness = 0; //how much it took away at the centre (0-1)
	};
	std::vector<Disc> ReadDiscs(const std::vector<GameVertex>& triangles);

	//Where a character stands (a round shadow, or a script entity's position), in view space
	struct Anchor
	{
		V3 position;
		float darkness = -1, radius = 0; //from a round shadow (view units); -1: not a round shadow
		int entity = -1; //script entity index
	};

	//A character: its triangles and its shape
	struct Character
	{
		unsigned texture = 0;
		std::vector<uint32_t> triangles; //into the frame's triangle list
		V3 right, up, forward; //its axes: up is the world's, forward away from the camera (level), right = up x forward
		V3 center, halfSize; //box along those axes
		V3 feet, head;
		float headSize = 0;
		float darkness = 0.38f; //contact darkening at the feet (the game's round shadow, else its usual)
		float shadowRadius = 0; //the game's round shadow's radius (view units), 0: none
		int anchor = -1; //Anchor index
		int entity = -1;
	};
	//textures: per triangle. Triangles of one texture are one character unless anchors (round shadows, entities) say there
	//are more of them there (two NPCs with the same model): then each triangle goes to the nearest anchor, level distance.
	std::vector<Character> Group(const std::vector<V3>& positions, const std::vector<unsigned>& textures, const std::vector<Anchor>& anchors,
		const V3& up);

	//Which way a script entity's facing angle (0-255) points, learnt from characters walking: while one walks, it faces
	//where it goes. World x and y are the ground (z is height).
	struct FacingCalibration
	{
		bool known = false;
		int sense = 1;
		double base = 0; //radians
		void Add(int facing, double dx, double dy);
		double Angle(int facing) const; //radians, atan2(y, x) on the ground
		size_t Samples() const { return samples.size(); }
	private:
		std::vector<std::pair<int, double>> samples;
		void Solve();
	};
}
