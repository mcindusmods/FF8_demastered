//ANALOG_MOVEMENT (game side): walking at any angle with the pad's left stick in fields.
//
//The field's movement code (the host of 0x4789A0) calls the game's "analogue value of the pad" function three times a
//frame: once to ask whether there is a stick at all (-1: no, walk by the d-pad bits), then for the left stick's X and Y.
//In Remastered that function always says -1, so a stick only ever walks in the d-pad's eight directions. Those three
//calls are pointed here: the game's function still runs first (it also puts the game's stack back), and while the left
//stick of an XInput pad is outside its dead zone its answer is replaced by the stick's. With the stick at rest, no pad, or
//ANALOG_MOVEMENT = 0, the game gets its own -1 and everything is as before. FFNx does the same for the 2000 game (its
//"Analog 360 patch"); the numbers the game wants are in analog_move.h.
//
//MOVE_TURN_SPEED: the d-pad and the arrow keys go the same way. The movement code has the buttons word as an argument (on
//the guest stack, 0x54 above where its own locals end: it tests bits 12-15 of it right after the stick test says no);
//while a direction is held there, the stick test is answered with a stick pushed all the way in that direction, and the
//direction given swings towards the one held (analogmove::Swing), as on the PlayStation. The real stick swings the same.
//
//ANALOG_WORLD_MAP: the world map's input code asks for all four axes of both pads every frame (eight calls). With a pad
//connected, pad 1's are answered: the left stick as it is (128 at rest, where the game goes by the d-pad again), the
//right stick at rest (Remastered turns the camera by buttons). While the left stick is pushed the d-pad bits are taken
//out of this frame's key state, which the game reads after those calls, so the two don't add up (as FFNx does).
#include "config.h"
#include "coreHeader.h"
#include "analog_move.h"
#include "field_pbr_runtime.h"

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
	analogmove::Swing swing;
	analogmove::Stick worldLatched;
	bool worldAnswers = false; //a pad is connected: the world map's calls of this frame are answered

	constexpr size_t STATE_ESP = 0x2C; //the emulated CPU's ESP (a guest address)
	DWORD stackBaseAt = 0, pageTableAt = 0; //host addresses of GUEST_STACK_BASE, GUEST_PAGE_TABLE
	DWORD worldKeys = 0, worldKeyIndex = 0; //guest addresses

	bool StackRead32(const DWORD guest, DWORD& value)
	{
		DWORD base = 0;
		return stackBaseAt != 0 && fieldrt::SafeRead(&base, reinterpret_cast<const void*>(stackBaseAt), 4) && base != 0
			&& fieldrt::SafeRead(&value, reinterpret_cast<const void*>(base + guest), 4);
	}

	//host address of a guest byte (nullptr: not mapped)
	BYTE* GuestHost(const DWORD guest)
	{
		DWORD page = 0;
		if (pageTableAt == 0 || !fieldrt::SafeRead(&page, reinterpret_cast<const DWORD*>(pageTableAt) + (guest >> 12), 4) || page == 0)
			return nullptr;
		return reinterpret_cast<BYTE*>(page + (guest & 0xFFF));
	}

	DWORD StateEsp(const BYTE* state)
	{
		DWORD esp = 0;
		memcpy(&esp, state + STATE_ESP, 4);
		return esp;
	}

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
		//the movement code's buttons word: its arguments are 0x54 above its locals, and this call has pushed 0x10 more
		DWORD buttons = 0;
		const bool haveButtons = MOVE_TURN_SPEED > 0 && StackRead32(StateEsp(state) + 0x64, buttons);
		const DWORD result = getValue(state);
		analogmove::Stick wanted = ANALOG_MOVEMENT ? ReadStick() : analogmove::Stick();
		if (!wanted.active && haveButtons)
		{
			wanted = analogmove::FromDpad(buttons);
			static bool said = false;
			if (wanted.active && !said)
			{
				said = true;
				OutputDebug("MOVE_TURN_SPEED: the d-pad and the arrow keys walk through the game's analogue code (buttons word %04X at "
					"the first step), turning %.0f degrees a second\n", buttons & 0xFFFF, MOVE_TURN_SPEED);
			}
		}
		latched = swing.Turn(wanted, MOVE_TURN_SPEED, GetTickCount64() / 1000.0);
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

	//the world map: one of the eight calls (port, which axis, port)
	DWORD __cdecl WorldStick(BYTE* state)
	{
		DWORD port = 0xFF, type = 0xFF;
		const DWORD esp = StateEsp(state);
		const bool arguments = StackRead32(esp + 4, port) && StackRead32(esp + 8, type);
		const DWORD result = getValue(state);
		if (!arguments || (port & 0xFF) != 0)
			return result;
		type &= 0xFF;
		if (type == 2) //left X: the first call of the frame
		{
			worldLatched = ReadStick();
			worldAnswers = pad >= 0;
			if (worldLatched.active)
			{
				//the d-pad out of this frame's keys (the left stick also arrives as d-pad presses in Remastered)
				int16_t index = 0;
				if (BYTE* at = GuestHost(worldKeyIndex); at != nullptr && fieldrt::SafeRead(&index, at, 2) && index >= 0 && index < 8)
					if (BYTE* keys = GuestHost(worldKeys + static_cast<DWORD>(index) * 4))
					{
						DWORD value = 0;
						if (fieldrt::SafeRead(&value, keys, 4) && (value & 0xF000u) != 0)
						{
							value &= ~0xF000u;
							memcpy(keys, &value, 4);
						}
					}
			}
		}
		if (!worldAnswers)
			return result; //no pad: as before
		if (type == 2)
			Answer(state, worldLatched.x);
		else if (type == 3)
			Answer(state, worldLatched.y);
		else if (type == 0 || type == 1)
			Answer(state, 128);
		return result;
	}

	//the eight calls, from the first one on; false: not what was expected (nothing changed)
	bool PatchWorld(const DWORD function)
	{
		if (!HasAddress(ANALOG_WORLD_FIRST) || !HasAddress(WM_KEYS) || !HasAddress(WM_KEY_INDEX) || pageTableAt == 0)
			return false;
		const DWORD first = IMAGE_BASE + GetAddress(ANALOG_WORLD_FIRST);
		DWORD calls[8];
		int found = 0;
		for (DWORD at = first; at < first + 0x240 && found <= 8; at++)
			if (*reinterpret_cast<const BYTE*>(at) == 0xE8 && at + 5 + *reinterpret_cast<const DWORD*>(at + 1) == function)
			{
				if (found < 8)
					calls[found] = at;
				found++;
				at += 4;
			}
		if (found != 8 || calls[0] != first)
			return false;
		worldKeys = GetAddress(WM_KEYS);
		worldKeyIndex = GetAddress(WM_KEY_INDEX);
		for (const DWORD call : calls)
			ReplaceCallFunction(call, reinterpret_cast<void*>(&WorldStick));
		return true;
	}
}

//dllmain.cpp, once, before the game runs
void ApplyAnalogMovementPatch()
{
	if (!ANALOG_MOVEMENT && !(MOVE_TURN_SPEED > 0))
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
	stackBaseAt = HasAddress(GUEST_STACK_BASE) ? IMAGE_BASE + GetAddress(GUEST_STACK_BASE) : 0;
	pageTableAt = HasAddress(GUEST_PAGE_TABLE) ? IMAGE_BASE + GetAddress(GUEST_PAGE_TABLE) : 0;
	if (ANALOG_MOVEMENT)
		OutputDebug("ANALOG_MOVEMENT: on (fields: the left stick walks at any angle, dead zone %.2f)\n", ANALOG_DEADZONE);
	if (MOVE_TURN_SPEED > 0)
		OutputDebug("MOVE_TURN_SPEED: %.0f degrees a second (fields: the walking direction swings towards the one held; the d-pad "
			"and the arrow keys%s)\n", MOVE_TURN_SPEED, stackBaseAt != 0 ? " too" : " not: GUEST_STACK_BASE is not known for this game DLL");
	if (ANALOG_MOVEMENT && ANALOG_WORLD_MAP)
		OutputDebug(PatchWorld(function) ? "ANALOG_WORLD_MAP: on (the left stick moves at any angle on the world map)\n"
			: "ANALOG_WORLD_MAP: the world map's calls are not where they are expected in this game DLL, it stays as it is\n");
}
