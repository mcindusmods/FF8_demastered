//DEMASTER's day/night clock (DAY_NIGHT): see day_night.h. The clock's rules (lengths, fades, the battle's slower time, the
//file) are in field_pbr.cpp, where tests/field_pbr_test.cpp checks them.

#include "day_night.h"

#include "config.h"
#include "coreHeader.h"
#include "debug.h"
#include "gl_state.h"

#include <GL/glew.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>

namespace
{
	constexpr double SPEEDS[] = { 1.0, 10.0, 60.0 }; //Ctrl+F9
	constexpr double MAX_STEP = 0.25; //seconds one frame may move the clock (a load or a hitch doesn't jump it)
	constexpr double SAVE_EVERY = 30.0; //seconds of real time between writes of the file
	constexpr double BAR_SECONDS = 4.0; //the bar stays this long after a key

	fieldpbr::DayNightCycle cycle;
	bool started = false;
	double clockTime = 0; //seconds into the cycle
	bool paused = false;
	int speed = 0; //index into SPEEDS
	std::atomic<int> fieldTime{ static_cast<int>(fieldpbr::FieldTime::Cycle) }; //the place you are: its _pbr.txt time
	LARGE_INTEGER frequency{}, lastCount{};
	double realTime = 0; //seconds since the clock started (for the bar and saving)
	double lastSave = 0;
	bool unsaved = false;
	double barUntil = -1;
	bool keyWasDown[4] = {};
	int lastMode = -1;

	std::string ClockPath()
	{
		return std::string(DIRECT_IO_EXPORT_DIR) + "daynight.txt";
	}

	void Save()
	{
		FILE* file = nullptr;
		if (fopen_s(&file, ClockPath().c_str(), "wb") != 0 || file == nullptr)
			return;
		const std::string text = fieldpbr::WriteClock(cycle, clockTime);
		fwrite(text.data(), 1, text.size(), file);
		fclose(file);
		lastSave = realTime;
		unsaved = false;
	}

	void Start()
	{
		if (started)
			return;
		started = true;
		fieldpbr::DayNightCycle c;
		c.day = DAY_NIGHT_DAY;
		c.night = DAY_NIGHT_NIGHT;
		c.fade = DAY_NIGHT_FADE;
		c.battleSlower = DAY_NIGHT_BATTLE;
		cycle = c.Checked();
		clockTime = cycle.fade / 2; //a new game: the morning
		bool fromFile = false;
		if (FILE* file = nullptr; fopen_s(&file, ClockPath().c_str(), "rb") == 0 && file != nullptr)
		{
			std::string text(4096, '\0');
			text.resize(fread(text.data(), 1, text.size(), file));
			fclose(file);
			double t = 0;
			if (fieldpbr::ParseClock(text, t))
			{
				clockTime = fieldpbr::AdvanceClock(cycle, t, 0, false);
				fromFile = true;
			}
		}
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&lastCount);
		OutputDebug("DAY_NIGHT: on, %.0f s day + %.0f s night, %.0f s fades, battles %.3g times slower; %s: %s. Keys%s: Ctrl+F7 swap, "
			"Ctrl+F8 pause, Ctrl+F9 speed (1x, 10x, 60x), Ctrl+F6 to just before the next fade\n", cycle.day, cycle.night, cycle.fade,
			cycle.battleSlower, fromFile ? "from DEMASTER_EXP\\daynight.txt" : "new clock (morning)", fieldpbr::ClockText(cycle, clockTime).c_str(),
			DAY_NIGHT_KEYS ? "" : " (off: DAY_NIGHT_KEYS=0)");
	}

	bool Battle()
	{
		return currentMode == MODE_BATTLE || currentMode == MODE_BATTLESWIRL;
	}

	//a fixed-time field (or a battle started from one) holds the clock; the world map is never fixed
	bool Held()
	{
		return fieldTime.load() != static_cast<int>(fieldpbr::FieldTime::Cycle);
	}

	bool GameInFront()
	{
		const HWND window = GetForegroundWindow();
		DWORD process = 0;
		return window != nullptr && GetWindowThreadProcessId(window, &process) != 0 && process == GetCurrentProcessId();
	}

	constexpr int KEYS[4] = { VK_F7, VK_F8, VK_F9, VK_F6 };

	bool Down(const int slot)
	{
		return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(KEYS[slot]) & 0x8000) != 0;
	}

	//Ctrl + the slot's key went down this frame
	bool Pressed(const int slot)
	{
		const bool down = Down(slot);
		const bool pressed = down && !keyWasDown[slot];
		keyWasDown[slot] = down;
		return pressed;
	}

	void Keys()
	{
		if (!DAY_NIGHT_KEYS)
			return;
		if (!GameInFront())
		{
			for (int slot = 0; slot < 4; slot++)
				keyWasDown[slot] = Down(slot); //keys pressed in another window don't count when the game comes back
			return;
		}
		const char* what = nullptr;
		char text[64];
		if (Pressed(0))
		{
			clockTime = fieldpbr::SwapTime(cycle, clockTime);
			what = "swapped";
		}
		if (Pressed(1))
		{
			paused = !paused;
			what = paused ? "paused" : "running again";
		}
		if (Pressed(2))
		{
			speed = (speed + 1) % static_cast<int>(std::size(SPEEDS));
			snprintf(text, sizeof(text), "speed %.0fx", SPEEDS[speed]);
			what = text;
		}
		if (Pressed(3))
		{
			clockTime = fieldpbr::BeforeNextFade(cycle, clockTime, 5.0);
			what = "5 s before the next fade";
		}
		if (what != nullptr)
		{
			barUntil = realTime + BAR_SECONDS;
			unsaved = true;
			Save();
			OutputDebug("DAY_NIGHT: %s: %s%s%s\n", what, fieldpbr::ClockText(cycle, clockTime).c_str(), paused ? " (paused)" : "",
				Held() ? " (this place has a fixed time of day: the clock waits here)" : "");
		}
	}

	//--- the bar (testing): the cycle across the top of the screen, amber day to blue night, and where the clock is
	GLuint barProgram = 0;
	GLint barColour = -1;
	bool barTried = false;

	bool MakeBarProgram()
	{
		if (barTried)
			return barProgram != 0;
		barTried = true;
		const char* vs = "#version 110\nvoid main() { gl_Position = vec4(gl_Vertex.xy, 0.0, 1.0); }\n";
		const char* fs = "#version 110\nuniform vec4 colour;\nvoid main() { gl_FragColor = colour; }\n";
		const auto compile = [](const GLenum type, const char* source) -> GLuint
		{
			const GLuint shader = glCreateShader(type);
			glShaderSource(shader, 1, &source, nullptr);
			glCompileShader(shader);
			GLint ok = 0;
			glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
			if (!ok)
			{
				glDeleteShader(shader);
				return 0;
			}
			return shader;
		};
		const GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
		if (v == 0 || f == 0)
		{
			OutputDebug("DAY_NIGHT: the test bar's shader didn't compile; no bar\n");
			return false;
		}
		const GLuint program = glCreateProgram();
		glAttachShader(program, v);
		glAttachShader(program, f);
		glLinkProgram(program);
		glDeleteShader(v);
		glDeleteShader(f);
		GLint ok = 0;
		glGetProgramiv(program, GL_LINK_STATUS, &ok);
		if (!ok)
		{
			glDeleteProgram(program);
			OutputDebug("DAY_NIGHT: the test bar's shader didn't link; no bar\n");
			return false;
		}
		barProgram = program;
		barColour = glGetUniformLocation(program, "colour");
		return true;
	}

	void Quad(const float x0, const float y0, const float x1, const float y1, const float r, const float g, const float b)
	{
		glUniform4f(barColour, r, g, b, 1.0f);
		glBegin(GL_QUADS);
		glVertex2f(x0, y0);
		glVertex2f(x1, y0);
		glVertex2f(x1, y1);
		glVertex2f(x0, y1);
		glEnd();
	}

	void DrawBar()
	{
		const HWND window = WindowFromDC(wglGetCurrentDC());
		RECT client{};
		if (window == nullptr || !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0 || !MakeBarProgram())
			return;
		const int width = client.right, height = client.bottom;
		glstate::RenderState saved{};
		glstate::Capture(saved);
		const GLint previousProgram = glstate::Program();
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glstate::RawViewport(0, 0, width, height);
		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);
		glDepthMask(GL_FALSE);
		glDisable(GL_ALPHA_TEST);
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_CULL_FACE);
		glDisable(GL_STENCIL_TEST);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glUseProgram(barProgram);
		//the cycle: 0 (dawn's middle) at the left
		const float top = 1.0f, bottom = 1.0f - 2.0f * 8.0f / static_cast<float>(height);
		const float left = -0.98f, right = 0.98f;
		constexpr int PIECES = 120;
		for (int i = 0; i < PIECES; i++)
		{
			const double t = (i + 0.5) / PIECES * cycle.Length();
			const float n = static_cast<float>(fieldpbr::NightAmount(cycle, t));
			const float x0 = left + (right - left) * i / PIECES, x1 = left + (right - left) * (i + 1) / PIECES;
			Quad(x0, bottom, x1, top, 0.95f + (0.10f - 0.95f) * n, 0.70f + (0.16f - 0.70f) * n, 0.25f + (0.45f - 0.25f) * n);
		}
		//the clock: white at 1x, yellow at 10x, orange at 60x, red when paused; grey when this place holds it
		const float at = left + (right - left) * static_cast<float>(clockTime / cycle.Length());
		const float half = 2.0f / static_cast<float>(width) * 2.0f;
		float r = 1, g = 1, b = 1;
		if (Held())
			r = g = b = 0.55f;
		else if (paused)
			g = b = 0.15f;
		else if (speed == 1)
			b = 0.2f;
		else if (speed == 2)
		{
			g = 0.5f;
			b = 0.1f;
		}
		Quad(at - half, bottom - 2.0f * 4.0f / static_cast<float>(height), at + half, top, r, g, b);
		glUseProgram(static_cast<GLuint>(previousProgram));
		glstate::Apply(saved);
	}
}

void DayNightSwap()
{
	if (!DAY_NIGHT)
		return;
	Start();
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	const double seconds = frequency.QuadPart > 0 ? static_cast<double>(now.QuadPart - lastCount.QuadPart) / frequency.QuadPart : 0.0;
	lastCount = now;
	const double step = std::clamp(seconds, 0.0, MAX_STEP);
	realTime += step;
	//the world map is never fixed: leaving a fixed field for it lets the clock go again
	const int mode = currentMode;
	if (mode == MODE_WORLDMAP && Held())
		fieldTime = static_cast<int>(fieldpbr::FieldTime::Cycle);
	if (mode != lastMode)
	{
		lastMode = mode;
		if (mode == MODE_BATTLE && FIELD_DEBUG)
			OutputDebug("DAY_NIGHT: battle: the clock runs %.3g times slower (%s)\n", cycle.battleSlower, fieldpbr::ClockText(cycle, clockTime).c_str());
	}
	if (!paused && !Held() && step > 0)
	{
		clockTime = fieldpbr::AdvanceClock(cycle, clockTime, step * SPEEDS[speed], Battle());
		unsaved = true;
	}
	Keys();
	if (unsaved && realTime - lastSave >= SAVE_EVERY)
		Save();
	if (DAY_NIGHT_BAR && (paused || speed != 0 || realTime < barUntil))
		DrawBar();
}

void DayNightFieldEntered(const std::string& field, const fieldpbr::FieldTime time)
{
	const int before = fieldTime.exchange(static_cast<int>(time));
	if (DAY_NIGHT && before != static_cast<int>(time) && time != fieldpbr::FieldTime::Cycle)
		OutputDebug("DAY_NIGHT: %s is always %s (time = %s in its _pbr.txt): the clock waits until you leave\n", field.c_str(),
			fieldpbr::FieldTimeName(time), fieldpbr::FieldTimeName(time));
	else if (DAY_NIGHT && before != static_cast<int>(time) && FIELD_DEBUG)
		OutputDebug("DAY_NIGHT: %s follows the clock\n", field.c_str());
}

double DayNightClockAmount()
{
	if (!DAY_NIGHT)
		return 0.0;
	Start();
	return fieldpbr::NightAmount(cycle, clockTime);
}

double DayNightAmount(const fieldpbr::FieldTime time)
{
	if (!DAY_NIGHT)
		return 0.0;
	if (time == fieldpbr::FieldTime::Day)
		return 0.0;
	if (time == fieldpbr::FieldTime::Night)
		return 1.0;
	return DayNightClockAmount();
}
