#pragma once
//ANALOG_MOVEMENT's rules (no game, no Windows): the pad's left stick as the game's own analogue movement code wants it.
//
//The 2000 PC game still has the PlayStation's analogue walking in its field code: it asks "is there a stick" and, when
//there is, walks in the stick's direction (any angle) instead of the d-pad's eight. Remastered never answers yes.
//texturepatch_v2_analogMove.cpp answers for it; this file is the arithmetic, shared with tests/field_tween_test.cpp.
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
}
