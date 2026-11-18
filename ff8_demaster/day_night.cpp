//DEMASTER's day/night clock (DAY_NIGHT): see day_night.h. The clock's rules (lengths, fades, the battle's slower time, the
//file) are in field_pbr.cpp, where tests/field_pbr_test.cpp checks them.

#include "day_night.h"

#include "config.h"
#include "coreHeader.h"
#include "debug.h"
#include "field_pbr_runtime.h"
#include "gl_state.h"

#include <GL/glew.h>
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "shell32.lib") //SHGetKnownFolderPath (the Documents folder)
#pragma comment(lib, "ole32.lib")   //CoTaskMemFree

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <map>
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
	double wallTime = 0; //the same, not held back on hitches (for the play time's jumps)
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
		c.evening = DAY_NIGHT_EVENING;
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

	//--- the time of day per save (DAY_NIGHT_SAVES)
	//
	//The game's play time (GAME_TIME, in its savemap) comes back with a save when the save is loaded. When the game writes a
	//save file (seen in the save folder: FindFirstChangeNotification, no hook), the play time and the clock are kept in
	//DEMASTER_EXP\daynight_saves.txt. When the play time jumps (a save was loaded) and a kept save matches it, the clock goes
	//back to that save's time of day. Saves made before this, or elsewhere, have none: the clock goes on as it is.
	namespace persave
	{
		bool tried = false, on = false;
		fieldpbr::SaveClocks clocks;
		HANDLE watch = INVALID_HANDLE_VALUE;
		std::filesystem::path folder;
		std::map<std::wstring, long long> seen; //the save files and when they were last written
		int64_t lastPlay = -1; //the play time at the last reading in the game; -1: none yet
		double lastPlayAt = 0; //realTime then
		int64_t segmentPlay = -1; //a stretch without jumps, for the counter's speed
		double segmentAt = 0;
		double rate = 1.0; //the counter's steps per second (seconds: 1)
		bool rateLogged = false;
		double lastRecord = -1e9;

		std::string Path()
		{
			return std::string(DIRECT_IO_EXPORT_DIR) + "daynight_saves.txt";
		}

		bool ReadPlay(uint32_t& out)
		{
			static const uint32_t at = HasAddress(GAME_TIME) ? GetAddress(GAME_TIME) : 0;
			return at != 0 && fieldrt::GuestRead(&out, at, 4);
		}

		//where the play time is the game's own: playing (fields, the world map, battles), and the menus a save is made in
		bool Playing(const int mode)
		{
			return mode == MODE_FIELD || mode == MODE_WORLDMAP || mode == MODE_BATTLE || mode == MODE_BATTLESWIRL;
		}

		bool Saving(const int mode)
		{
			return mode == MODE_FIELD || mode == MODE_WORLDMAP || mode == MODE_MENU || mode == MODE_MENU2;
		}

		//the save files there now (.ff8, or a name with "save" in it); true when one is new or was written since the last look
		bool Scan()
		{
			bool changed = false;
			std::error_code error;
			for (std::filesystem::recursive_directory_iterator it(folder, std::filesystem::directory_options::skip_permission_denied, error), end;
				!error && it != end; it.increment(error))
			{
				if (!it->is_regular_file(error))
					continue;
				std::wstring name = it->path().filename().wstring();
				std::transform(name.begin(), name.end(), name.begin(), [](const wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
				//the game's saves are .ff8 files (Steam's cloud: game_data\user\saves\*.ff8); anything named save* too
				const bool ff8 = name.size() > 4 && name.compare(name.size() - 4, 4, L".ff8") == 0;
				if ((!ff8 && name.find(L"save") == std::wstring::npos) || (name.size() >= 4 && name.compare(name.size() - 4, 4, L".tmp") == 0))
					continue;
				const auto written = it->last_write_time(error);
				if (error)
					continue;
				const long long stamp = static_cast<long long>(written.time_since_epoch().count());
				auto& known = seen[it->path().wstring()];
				changed = changed || known != stamp;
				known = stamp;
			}
			return changed;
		}

		void Write()
		{
			FILE* file = nullptr;
			if (fopen_s(&file, Path().c_str(), "wb") != 0 || file == nullptr)
				return;
			const std::string text = fieldpbr::WriteSaveClocks(clocks);
			fwrite(text.data(), 1, text.size(), file);
			fclose(file);
		}

		void Start()
		{
			if (tried)
				return;
			tried = true;
			if (!DAY_NIGHT_SAVES)
				return;
			uint32_t play = 0;
			if (!ReadPlay(play))
			{
				OutputDebug("DAY_NIGHT: the game's play time can't be read here: the time of day isn't kept per save\n");
				return;
			}
			if (!DAY_NIGHT_SAVE_FOLDER.empty())
				folder = std::filesystem::path(DAY_NIGHT_SAVE_FOLDER);
			else
			{
				PWSTR documents = nullptr;
				if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)) && documents != nullptr)
					folder = std::filesystem::path(documents) / L"My Games" / L"FINAL FANTASY VIII Remastered";
				CoTaskMemFree(documents);
			}
			std::error_code error;
			if (folder.empty() || !std::filesystem::is_directory(folder, error))
			{
				OutputDebug("DAY_NIGHT: no save folder at %s: the time of day isn't kept per save (DAY_NIGHT_SAVE_FOLDER says where it is)\n",
					folder.string().c_str());
				return;
			}
			watch = FindFirstChangeNotificationW(folder.wstring().c_str(), TRUE,
				FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE);
			if (watch == INVALID_HANDLE_VALUE)
			{
				OutputDebug("DAY_NIGHT: can't watch the save folder %s: the time of day isn't kept per save\n", folder.string().c_str());
				return;
			}
			Scan();
			if (FILE* file = nullptr; fopen_s(&file, Path().c_str(), "rb") == 0 && file != nullptr)
			{
				std::string text(1 << 16, '\0');
				text.resize(fread(text.data(), 1, text.size(), file));
				fclose(file);
				fieldpbr::ParseSaveClocks(text, clocks);
			}
			on = true;
			OutputDebug("DAY_NIGHT: the time of day is kept per save (DEMASTER_EXP\\daynight_saves.txt, %zu save(s) known); watching %s "
				"(%zu save file(s)); the play time reads %u now\n", clocks.entries.size(), folder.string().c_str(), seen.size(), play);
		}

		//once a frame, after the clock moved: a save written, a save loaded. now: seconds of real time (not held back on hitches)
		void Frame(const int mode, const double now)
		{
			Start();
			if (!on)
				return;
			uint32_t play = 0;
			const bool readable = ReadPlay(play);
			//a save file written: the play time and the clock now (the menu is open, the play time is the save's or a moment on)
			if (WaitForSingleObject(watch, 0) == WAIT_OBJECT_0)
			{
				FindNextChangeNotification(watch);
				if (Scan() && readable && Saving(mode) && now - lastRecord > 2.0)
				{
					lastRecord = now;
					clocks.Record(play, clockTime);
					Write();
					OutputDebug("DAY_NIGHT: a save was written (play time %u): its time of day is %s\n", play,
						fieldpbr::ClockText(cycle, clockTime).c_str());
				}
			}
			if (!readable || !Playing(mode))
				return;
			//the counter's speed, from a minute without jumps (for how far a save's play time may be from the one loaded)
			if (segmentPlay < 0)
			{
				segmentPlay = play;
				segmentAt = now;
			}
			//a jump: back, or on by more than the time that passed could explain (a save was loaded, or a new game)
			const bool first = lastPlay < 0;
			const double since = now - lastPlayAt;
			const bool jumped = !first && (static_cast<int64_t>(play) < lastPlay
				|| static_cast<double>(static_cast<int64_t>(play) - lastPlay) > since * rate * 2.0 + 5.0 * rate);
			if (first || jumped)
			{
				const uint32_t tolerance = static_cast<uint32_t>(std::ceil(10.0 * rate));
				if (const fieldpbr::SaveClock* kept = clocks.Find(play, tolerance))
				{
					clockTime = fieldpbr::AdvanceClock(cycle, kept->clock, 0, false);
					unsaved = true;
					OutputDebug("DAY_NIGHT: a save was loaded (play time %u): back to the time of day it was saved at, %s\n", play,
						fieldpbr::ClockText(cycle, clockTime).c_str());
				}
				else if (jumped && mode == MODE_FIELD && play < static_cast<uint32_t>(60.0 * rate))
				{
					clockTime = cycle.fade / 2; //a new game starts in the morning
					unsaved = true;
					OutputDebug("DAY_NIGHT: a new game (play time %u): the morning\n", play);
				}
				else if (jumped && FIELD_DEBUG)
					OutputDebug("DAY_NIGHT: the play time jumped to %u (a save loaded?), none kept for it: the clock goes on\n", play);
				segmentPlay = play;
				segmentAt = now;
			}
			else if (now - segmentAt >= 60.0 && play > segmentPlay)
			{
				rate = std::clamp(static_cast<double>(play - segmentPlay) / (now - segmentAt), 0.5, 120.0);
				if (!rateLogged)
				{
					rateLogged = true;
					OutputDebug("DAY_NIGHT: the game's play time counts %.2f a second (1 = seconds)\n", rate);
				}
				segmentPlay = play;
				segmentAt = now;
			}
			lastPlay = play;
			lastPlayAt = now;
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
	wallTime += std::max(0.0, seconds);
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
	persave::Frame(mode, wallTime);
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

double DayNightClockEvening()
{
	if (!DAY_NIGHT)
		return 0.0;
	Start();
	return fieldpbr::EveningAmount(cycle, clockTime);
}

double DayNightEvening(const fieldpbr::FieldTime time)
{
	if (!DAY_NIGHT)
		return 0.0;
	if (time == fieldpbr::FieldTime::Day)
		return 0.0;
	if (time == fieldpbr::FieldTime::Night)
		return 1.0;
	return DayNightClockEvening();
}

fieldpbr::SkyTime DayNightSky()
{
	if (!DAY_NIGHT)
		return { false, 0.3 };
	Start();
	return fieldpbr::SkyTimeOf(cycle, clockTime);
}

fieldpbr::FieldTime DayNightPlaceTime()
{
	return static_cast<fieldpbr::FieldTime>(fieldTime.load());
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
