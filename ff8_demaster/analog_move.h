#pragma once
//ANALOG_MOVEMENT's rules (no game, no Windows): the pad's left stick as the game's own analogue movement code wants it.
//
//The 2000 PC game still has the PlayStation's analogue walking in its field code: it asks "is there a stick" and, when
//there is, walks in the stick's direction (any angle) instead of the d-pad's eight. Remastered never answers yes.
//texturepatch_v2_analogMove.cpp answers for it; this file is the arithmetic, shared with tests/field_tween_test.cpp.
//The d-pad and the arrow keys go the same way (as a stick pushed all the way), so their turns can swing like the stick's.
//The game's numbers: 0..255 per axis, 128 at rest, left and up smaller (as the PlayStation pad reports them).

namespace analogmove
{
	struct Stick
	{
		int x = 128, y = 128;
		bool active = false; //outside the dead zone: the game walks by the stick; inside it: by the d-pad, as before
	};

	//the smallest push the game is given, as a share of a full one (FFNx's analogue patch starts there too: 40 of 128)
	constexpr double SMALLEST = 0.3125;

	//padX, padY: the stick, -1..1, right and up positive (XInput's numbers / 32767). deadZone: the radius the stick rests
	//in (0.05..0.9). From its edge to the stick's full throw the push grows from SMALLEST to 1, the direction is kept.
	Stick FromPad(double padX, double padY, double deadZone);

	//The d-pad (or the arrow keys) as a stick pushed all the way: bits 12-15 of the game's buttons word (up, right, down,
	//left, as the PlayStation pad has them). Opposite ones cancel. Nothing held: at rest (not active).
	Stick FromDpad(unsigned buttons);

	//The swing: on the PlayStation a character turned towards the direction held instead of facing it at once. The
	//direction the game is given turns towards the one held, the short way round, at most `speed` degrees a second; how
	//hard the stick is pushed is passed on as it is. After REST seconds with nothing held the next push starts in its own
	//direction (the character has stopped; where it faces is the game's business again).
	class Swing
	{
	public:
		static constexpr double REST = 0.3;
		//wanted: this step's stick; speed: degrees a second (0 or less: no swing); now: seconds, any clock that runs on
		Stick Turn(const Stick& wanted, double speed, double now);
		void Reset() { has = false; }
	private:
		bool has = false;
		double angle = 0.0, last = 0.0; //the direction given last (radians, x right, y down), and when
	};
}
