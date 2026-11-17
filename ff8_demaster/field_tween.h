#pragma once
//FIELD_SMOOTH_SCROLL and FIELD_INTERPOLATION: the rules (no game, no OpenGL), shared with tests/field_tween_test.cpp.
//
//What the game does: a field's scroll is a whole number of PlayStation pixels, and every background layer is placed by
//whole-number arithmetic from it, so layers move in steps of a pixel (8 screen pixels at 4x on a 1440p screen), and layers
//that move at other speeds than the main one (parallax) step at different moments, which is the shimmer between them.
//The characters are already placed finer than that, but on top of the stepping scroll. And the game makes 30 pictures a
//second in fields.
//
//What DEMASTER does with it:
//  - smooth scroll: every layer's place, as the game drew it frame after frame, goes through a filter (Axis) that turns
//    the staircase into the line it was rounded from; the layer is drawn at that place (never more than 3/4 of a
//    PlayStation pixel from the game's, and exactly the game's once the scroll rests). The main layer's difference moves
//    the characters too, so they stay on the floor.
//  - interpolation: after each of the game's frames an in-between picture is shown first: every layer halfway between its
//    place in the last frame and in this one, every character's vertices halfway between the two frames (TweenCharacters).
//The game side is texturepatch_v2_fieldTween.cpp (the layers, the frame) and field_mirror.cpp (the second picture).
//Units: the game's screen units, 640 x 480 for the whole picture: 2 per PlayStation pixel.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fieldtween
{
	constexpr double PIXEL = 2.0; //screen units per PlayStation pixel
	constexpr double LIMIT = 0.75 * PIXEL; //how far a layer is ever drawn from where the game put it
	constexpr double JUMP = 12.0 * PIXEL; //a move this big in one frame is a cut, not a scroll: nothing is smoothed across it
	constexpr int TILE = 16; //texels of a page's cell

	//One direction of one layer: the game's whole-number places in, the line they were rounded from out. An alpha-beta
	//filter (a place and a speed, each corrected by a share of how far the last guess was off), critically damped: it follows
	//a steady scroll without lag and settles on the game's own number when the scroll stops.
	class Axis
	{
	public:
		void Reset() { *this = Axis(); }
		//whole: where the game drew the layer this frame. smooth false: the game's place is passed through (the filter still
		//runs, so turning it on later starts from where things are). Returns where to draw it.
		double Add(double whole, bool smooth);
		bool Jumped() const { return jumped; } //the last Add was a cut (or the first)
	private:
		bool started = false, jumped = true;
		double place = 0, speed = 0, last = 0;
		int still = 0;
	};

	//A layer over the frames
	struct LayerTrack
	{
		Axis x, y;
		double whole[2] = { 0, 0 };  //this frame, as the game drew it
		double exact[2] = { 0, 0 };  //this frame, where DEMASTER draws it
		double before[2] = { 0, 0 }; //the frame before, where DEMASTER drew it
		bool tween = false;          //before is the frame right before this one, and no cut in between
		uint32_t frame = 0;          //the frame exact belongs to
		int triangles = 0;           //drawn this frame (the main layer is the one with the most)
	};
	//the layer's first draw of a frame: its place as the game drew it. frame: counts up by one per frame of the game.
	void Advance(LayerTrack& track, double wholeX, double wholeY, uint32_t frame, bool smooth);
	//how far from the game's place the layer is drawn: in this frame's picture, and in the in-between picture before it
	//(halfway from the last frame's place; the same as this frame's when there is nothing to come from)
	void ShiftNow(const LayerTrack& track, double out[2]);
	void ShiftBetween(const LayerTrack& track, double out[2]);

	//A layer that reaches past an edge of the picture must still reach it when it is moved (or the moved layer would show a
	//strip of nothing at that edge: at the end of a field, where the scroll stops dead and the filter is still slowing).
	//lo, hi: the layer's edges on the screen as the game drew it (screen units), along x (vertical false, the picture is
	//0..640) or y (0..480, of which the game shows 16..464). Returns the shift, held back where needed.
	double KeepCovering(double lo, double hi, double shift, bool vertical);
	//a shift in whole framebuffer pixels (tiles are sampled without filtering: a layer between pixels would shimmer).
	//pixelsPerUnit: of the framebuffer being drawn into, per axis
	double Snap(double shift, double pixelsPerUnit);
	//the game's projection (column-major, as glLoadMatrixf takes it) with everything moved by (dx, dy) screen units first
	void ShiftProjection(const float in[16], double dx, double dy, float out[16]);
	//framebuffer pixels per screen unit of a projection and viewport size (x and y)
	void PixelsPerUnit(const float projection[16], int viewportWidth, int viewportHeight, double out[2]);

	//--- which layer a draw of a page is, and where the game put it

	struct Cell
	{
		bool known = false, mixed = false; //mixed: tiles of different layers share the cell
		int layer = 0, layerX = 0, layerY = 0; //the tile's layer and its place on that layer's canvas (PlayStation pixels)
	};
	struct PageCells
	{
		int cellsX = 0, cellsY = 0;
		double logicalWidth = 256, logicalHeight = 256; //texels the page's UVs count in
		std::vector<Cell> cells;
		void Size(int logicalW, int logicalH);
		void Add(int cellX, int cellY, int layer, int layerX, int layerY); //cellX, cellY: in texels
	};
	struct DrawPlace
	{
		bool known = false; //every tile of the draw is of one layer and they agree on where its canvas is
		int layer = -1;
		double x = 0, y = 0; //the canvas' corner on the screen (screen units): position - 2 x canvas place
		int triangles = 0;
	};
	//positions: x, y, z, rhw per vertex; uvs: u, v per vertex (strides in bytes); indices: count of them, three per triangle,
	//each less than vertexCount
	DrawPlace PlaceOfDraw(const PageCells& page, const void* positions, size_t positionStride, const void* uvs, size_t uvStride,
		size_t vertexCount, const uint32_t* indices, size_t count);

	//--- the characters in between

	struct CharDrawRef
	{
		uint32_t texture = 0, first = 0, count = 0; //vertices, three per triangle
	};
	struct CharFrame
	{
		std::vector<float> shown; //x, y, z, rhw per vertex, as shown (the frame's shift of the main layer added)
		std::vector<CharDrawRef> draws;
		uint32_t frame = 0;
	};
	constexpr double CHAR_LIMIT = 24.0 * PIXEL; //a vertex (or a whole model) moving further than this in a frame is a cut
	//out: x, y, z, rhw per vertex of `now` for the in-between picture. A draw with the same texture and vertex count in the
	//frame before (the n-th such in both) is tweened vertex by vertex; any other moves as a whole by half of how far
	//everything with its texture moved; with nothing to come from it stays where it is. Returns how many draws were tweened
	//(either way).
	size_t TweenCharacters(const CharFrame& before, const CharFrame& now, std::vector<float>& out);

	//--- when to show two pictures per frame of the game

	class Pacer
	{
	public:
		//seconds between this frame of the game ending and the one before (SwapBuffers to SwapBuffers). doubled: the
		//in-between picture was shown in that frame.
		void Frame(double seconds, bool doubled);
		void Reset() { *this = Pacer(); }
		//the game makes about 30 pictures a second here (25-40), so an in-between picture has a place
		bool HalfRate() const;
		//the game got slower with the in-between pictures than it was without (more than 12%, for 2 seconds)
		bool TooSlow() const { return slowFrames >= 60; }
		double Interval() const { return doubledFrames >= 8 ? intervalDoubled : interval; } //seconds per frame of the game
		double Plain() const { return interval; } //without in-between pictures (0 until measured)
		int PlainFrames() const { return plainFrames; }
	private:
		double interval = 0, intervalDoubled = 0;
		int plainFrames = 0, doubledFrames = 0, slowFrames = 0;
	};
}
