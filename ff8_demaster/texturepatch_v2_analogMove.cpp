//ANALOG_MOVEMENT (game side): walking at any angle with the pad's left stick in fields.
//
//The field's movement code (the host of 0x4789A0) calls the game's "analogue value of the pad" function three times a
//frame: once to ask whether there is a stick at all (-1: no, walk by the d-pad bits), then for the left stick's X and Y.
//In Remastered that function always says -1, so a stick only ever walks in the d-pad's eight directions. Those three
//calls are pointed here: the game's function still runs first (it also puts the game's stack back), and while the left
//stick of an XInput pad is outside its dead zone its answer is replaced by the stick's. With the stick at rest, no pad, or
//ANALOG_MOVEMENT = 0, the game gets its own -1 and everything is as before. FFNx does the same for the 2000 game (its
//"Analog 360 patch"); the numbers the game wants are in analog_move.h. The world map has the same kind of calls but also
//needs the d-pad bits taken out of the game's key state, whose address is not known for Remastered: fields only.
#include "config.h"
#include "coreHeader.h"
#include "analog_move.h"

namespace
{
	typedef DWORD(__cdecl* RecompiledFunction)(BYTE* state); //every function of the game: the emulated CPU, EAX first
	RecompiledFunction getValue = nullptr;

	//XInput without its header or library: the DLL Windows has is loaded when the first pad is looked for
	struct PadState
	{
		DWORD packet;
		WORD buttons;
		BYTE leftTrigger, rightTrigger;
		SHORT leftX, leftY, rightX, rightY;
	};
	typedef DWORD(WINAPI* GetStateFunction)(DWORD index, PadState* state);
	GetStateFunction getState = nullptr;
	bool looked = false;
	int pad = -1; //the pad in use (0-3), -1: none found yet
	ULONGLONG lastSearch = 0;
	analogmove::Stick latched; //what the three calls of one frame answer with

	analogmove::Stick ReadStick()
	{
		if (!looked)
		{
			looked = true;
			for (const char* name : { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" })
				if (const HMODULE module = LoadLibraryA(name))
				{
					getState = reinterpret_cast<GetStateFunction>(GetProcAddress(module, "XInputGetState"));
					if (getState != nullptr)
						break;
				}
			if (getState == nullptr)
				OutputDebug("ANALOG_MOVEMENT: XInput not found on this system, movement stays as it is\n");
		}
		if (getState == nullptr)
			return {};
		PadState state{};
		if (pad >= 0 && getState(static_cast<DWORD>(pad), &state) == ERROR_SUCCESS)
			return analogmove::FromPad(state.leftX / 32767.0, state.leftY / 32767.0, ANALOG_DEADZONE);
		//no pad (or it went away): asking for one that isn't there is slow, so every two seconds
		pad = -1;
		const ULONGLONG now = GetTickCount64();
		if (lastSearch != 0 && now - lastSearch < 2000)
			return {};
		lastSearch = now;
		for (int i = 0; i < 4; i++)
			if (getState(static_cast<DWORD>(i), &state) == ERROR_SUCCESS)
			{
				pad = i;
				OutputDebug("ANALOG_MOVEMENT: pad %d\n", i + 1);
				return analogmove::FromPad(state.leftX / 32767.0, state.leftY / 32767.0, ANALOG_DEADZONE);
			}
		return {};
	}

	void Answer(BYTE* state, const int value)
	{
		const DWORD eax = static_cast<DWORD>(value);
		memcpy(state, &eax, 4);
	}

	DWORD __cdecl FieldStickTest(BYTE* state)
	{
		const DWORD result = getValue(state);
		latched = ReadStick();
		if (latched.active)
			Answer(state, latched.x); //anything but -1: there is a stick
		return result;
	}

	DWORD __cdecl FieldStickX(BYTE* state)
	{
		const DWORD result = getValue(state);
		if (latched.active)
			Answer(state, latched.x);
		return result;
	}

	DWORD __cdecl FieldStickY(BYTE* state)
	{
		const DWORD result = getValue(state);
		if (latched.active)
			Answer(state, latched.y);
		return result;
	}
}

//dllmain.cpp, once, before the game runs
void ApplyAnalogMovementPatch()
{
	if (!ANALOG_MOVEMENT)
		return;
	static const ADDRESS_NAME needed[] = { ANALOG_GET_VALUE, ANALOG_FIELD_TEST, ANALOG_FIELD_X, ANALOG_FIELD_Y };
	for (const ADDRESS_NAME name : needed)
		if (!HasAddress(name))
		{
			OutputDebug("ANALOG_MOVEMENT: %s is not known for this game DLL, movement stays as it is\n", GetAddressName(name));
			return;
		}
	const DWORD function = IMAGE_BASE + GetAddress(ANALOG_GET_VALUE);
	const DWORD calls[3] = { IMAGE_BASE + GetAddress(ANALOG_FIELD_TEST), IMAGE_BASE + GetAddress(ANALOG_FIELD_X),
		IMAGE_BASE + GetAddress(ANALOG_FIELD_Y) };
	//each must be a call to that function before anything is changed
	for (const DWORD call : calls)
		if (*reinterpret_cast<const BYTE*>(call) != 0xE8 || call + 5 + *reinterpret_cast<const DWORD*>(call + 1) != function)
		{
			OutputDebug("ANALOG_MOVEMENT: unexpected code at %08X, movement stays as it is\n", call);
			return;
		}
	getValue = reinterpret_cast<RecompiledFunction>(function);
	ReplaceCallFunction(calls[0], reinterpret_cast<void*>(&FieldStickTest));
	ReplaceCallFunction(calls[1], reinterpret_cast<void*>(&FieldStickX));
	ReplaceCallFunction(calls[2], reinterpret_cast<void*>(&FieldStickY));
	OutputDebug("ANALOG_MOVEMENT: on (fields: the left stick walks at any angle, dead zone %.2f)\n", ANALOG_DEADZONE);
}
