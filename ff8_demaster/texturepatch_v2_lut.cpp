//LUT (the DLL's side): the time of day's colour grade. The rules (the files, when each time's LUT is used): lut_grade.h.
//
//The LUTs are textures of DEMASTER's own (the strips, 16 bits a value, filtered linearly) on texture units 14 and 15, and a
//program that grades has three uniforms: demasterLut (x: how much of the second LUT, y: how much of the grade, 0 = none,
//z: the LUT's points along each colour) and demasterLutA, demasterLutB (the two strips). The shaders do the grading:
//	main.frag, main_char.frag, main_notex.frag   (DEMASTER's versions of the game's shaders) before every draw of the
//	                           game, y is set for draws of the scene: in battles and fields by the kind of texture bound
//	                           (the game's texture type, remembered when it is filled): battle stages and enemies (11), the
//	                           party (35), the world's ground in a battle there (18, 27), field backgrounds (25, 41), field
//	                           models (57); on the world map by depth (what is drawn at a depth of its own: the ground, the
//	                           sea, the sky, whatever its texture; not the map or the menus); never draws that add light or
//	                           take it away (blend ONE, ONE; reverse subtract). Zero for everything else
//	demaster_field.frag, demaster_chara.frag, the fighters' main.frag   DEMASTER's own draws (LutUse)
//
//Which grade: by the clock, as the place has its time (a field's _pbr.txt time = day | night; a battle's as SCENE_LIGHT has
//it, scene_light.txt and where it started), the Lunar Cry's set while the story's counter is in LUT_LUNAR_FROM to
//LUT_LUNAR_TO, LUT_STRENGTH of it (LUT_INSIDE of that where the place is not open to the sky).
#include "coreHeader.h"
#include "config.h"
#include "day_night.h"
#include "field_pbr_runtime.h"
#include "gl_state.h"
#include "image.h"
#include "lut_grade.h"

#include <array>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <unordered_map>

using namespace fieldrt;

namespace
{
	constexpr int UNIT_A = 14, UNIT_B = 15;
	constexpr int TYPE_BATTLE_STAGE = 11, TYPE_WORLD = 18, TYPE_FIELD_BACKGROUND = 25, TYPE_WORLD_SKY = 27, TYPE_BATTLE_CHARACTER = 35,
		TYPE_FIELD_BACKGROUND_2 = 41, TYPE_FIELD_MODEL = 57;
	constexpr ULONGLONG CHECK_EVERY = 2000; //ms between looks at the files (a LUT changed on disk comes in by itself)

	std::unordered_map<GLuint, int> types; //the game's textures of the scene: their type at the last upload

	struct ProgramLut
	{
		bool looked = false;
		GLint lut = -1;
		float sent[4] = { 0, 0, 0, 0 };
	};
	std::unordered_map<GLuint, ProgramLut> programs;

	//a LUT file in a texture
	struct Loaded
	{
		std::filesystem::file_time_type time{};
		GLuint texture = 0;
		int size = 0;
		bool failed = false;
		bool read = false; //read (texture 0 with this: the colours as they are)
	};
	std::unordered_map<std::string, Loaded> loaded; //by path
	std::unordered_map<int, GLuint> identities; //the colours as they are, by size (for a time without a LUT)

	//the place, and the file each time has there
	std::mutex fieldLock;
	std::string fieldName; //the last field entered (game thread)
	std::atomic<bool> fieldSky{ false };
	std::atomic<uint64_t> fieldSerial{ 0 };
	struct Place
	{
		std::string folder; //lut\field\<field>, lut\battle\a0stgNNN, lut\world; empty: no place of its own
		std::string name;
		uint64_t serial = ~0ull;
		std::array<std::string, lutgrade::SLOT_COUNT> path; //the file for each time; empty: none
		std::array<ULONGLONG, lutgrade::SLOT_COUNT> checked{};
		bool said = false;
	} place;

	//this frame's grade
	struct Grade
	{
		bool on = false;
		GLuint a = 0, b = 0;
		float value[4] = { 0, 0, 0, 0 };
		lutgrade::Blend blend;
	} grade;
	uint64_t frame = 0, gradeFrame = ~0ull;
	bool fromWorld = false;
	int lastMode = -1;
	int keyMode = 0; //Ctrl+F5: 0 as the story has it, 1 the Lunar Cry's set, 2 no grade
	bool keyWasDown = false;
	std::string lastText;

	std::string Root()
	{
		return std::string(DIRECT_IO_EXPORT_DIR) + "lut\\";
	}

	bool Exists(const std::string& path)
	{
		std::error_code error;
		return std::filesystem::is_regular_file(path, error);
	}

	//<stem>.png or <stem>.cube (empty: neither)
	std::string FileOf(const std::string& stem)
	{
		for (const char* extension : { ".png", ".cube" })
			if (Exists(stem + extension))
				return stem + extension;
		return std::string();
	}

	GLuint Upload(const lutgrade::Table& table)
	{
		GLuint texture = 0;
		glGenTextures(1, &texture);
		const GLenum active = glstate::ActiveTexture();
		const GLuint before = glstate::Texture(GL_TEXTURE_2D, UNIT_A);
		glActiveTexture(GL_TEXTURE0 + UNIT_A);
		glBindTexture(GL_TEXTURE_2D, texture);
		RawTexImage(GL_RGBA16, table.size * table.size, table.size, GL_RGBA, GL_UNSIGNED_SHORT, table.rgba.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glBindTexture(GL_TEXTURE_2D, before);
		glActiveTexture(active);
		return texture;
	}

	bool Read(const std::string& path, lutgrade::Table& table, std::string& why)
	{
		if (path.size() > 5 && _stricmp(path.c_str() + path.size() - 5, ".cube") == 0)
		{
			std::ifstream in(path, std::ios::binary);
			std::ostringstream text;
			text << in.rdbuf();
			if (!in)
			{
				why = "can't be read";
				return false;
			}
			return lutgrade::FromCube(text.str(), table, why);
		}
		const SafeBimg image = LoadImageFromFile(path.c_str());
		if (!image)
		{
			why = "can't be read as a picture";
			return false;
		}
		static bx::DefaultAllocator allocator;
		bimg::ImageContainer* rgba = bimg::imageConvert(&allocator, bimg::TextureFormat::RGBA8, *image, false);
		if (rgba == nullptr)
		{
			why = "can't be turned into RGBA";
			return false;
		}
		const bool ok = lutgrade::FromStrip(static_cast<const uint8_t*>(rgba->m_data), static_cast<int>(rgba->m_width),
			static_cast<int>(rgba->m_height), table, why);
		bimg::imageFree(rgba);
		return ok;
	}

	//the texture of a LUT file (read again when it changed on disk); 0: it can't be read
	GLuint TextureOf(const std::string& path, int& size)
	{
		size = 0;
		if (path.empty())
			return 0;
		std::error_code error;
		const auto time = std::filesystem::last_write_time(path, error);
		Loaded& l = loaded[path];
		if (l.read && time == l.time)
		{
			size = l.size;
			return l.texture;
		}
		l.time = time;
		lutgrade::Table table;
		std::string why;
		if (!Read(path, table, why))
		{
			if (!l.failed || l.texture != 0)
				OutputDebug("LUT: %s: %s (that time's colours stay as they are)\n", path.c_str(), why.c_str());
			if (l.texture != 0)
				glDeleteTextures(1, &l.texture);
			l.texture = 0;
			l.failed = true;
			l.read = true;
			return 0;
		}
		if (l.texture != 0)
			glDeleteTextures(1, &l.texture);
		//a LUT that leaves the colours as they are (the day's in this release) is no LUT: nothing to read on the GPU
		const bool same = lutgrade::IsIdentity(table, 1.0 / 255.0);
		l.texture = same ? 0 : Upload(table);
		l.size = table.size;
		l.failed = false;
		l.read = true;
		if (LUT_DEBUG)
			OutputDebug("LUT: %s read (%d points along each colour)%s\n", path.c_str(), table.size, same ? ": the colours as they are" : "");
		size = l.size;
		return l.texture;
	}

	//the place we are at now: its folder of its own, if it has one
	void PlaceNow(const int mode)
	{
		std::string folder, name;
		uint64_t serial = 0;
		if (mode == MODE_FIELD)
		{
			std::lock_guard<std::mutex> lock(fieldLock);
			name = fieldName;
			folder = name.empty() ? std::string() : "field\\" + name;
			serial = fieldSerial.load();
		}
		else if (mode == MODE_BATTLE)
		{
			char text[32];
			snprintf(text, sizeof(text), "a0stg%03d", currentStage);
			name = std::string("battle ") + text;
			folder = std::string("battle\\") + text;
			serial = 0x100000000ull + static_cast<uint32_t>(currentStage);
		}
		else
		{
			name = "the world map";
			folder = "world";
			serial = 0x200000000ull;
		}
		if (serial == place.serial && name == place.name)
			return;
		place = Place();
		place.folder = folder;
		place.name = name;
		place.serial = serial;
	}

	//the file a time has here: the place's own, the whole game's, or (a lunar one) the ordinary one of that time
	const std::string& SlotFile(const int slot, const ULONGLONG tick)
	{
		std::string& path = place.path[slot];
		if (place.checked[slot] != 0 && tick - place.checked[slot] < CHECK_EVERY)
			return path;
		place.checked[slot] = tick;
		const std::string root = Root(), name = lutgrade::SlotName(slot);
		path = place.folder.empty() ? std::string() : FileOf(root + place.folder + "\\" + name);
		if (path.empty())
			path = FileOf(root + name);
		if (path.empty() && slot >= lutgrade::LUNAR_DAY)
			path = SlotFile(slot - lutgrade::LUNAR_DAY, tick);
		return path;
	}

	GLuint SlotTexture(const int slot, const ULONGLONG tick, int& size, std::string& file)
	{
		file = SlotFile(slot, tick);
		return TextureOf(file, size);
	}

	void Keys()
	{
		if (!DAY_NIGHT_KEYS)
			return;
		const HWND window = GetForegroundWindow();
		DWORD process = 0;
		const bool front = window != nullptr && GetWindowThreadProcessId(window, &process) != 0 && process == GetCurrentProcessId();
		const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
		const bool pressed = front && down && !keyWasDown;
		keyWasDown = down;
		if (!pressed)
			return;
		keyMode = (keyMode + 1) % 3;
		OutputDebug("LUT: Ctrl+F5: %s\n", keyMode == 0 ? "the grade as the story has it" : keyMode == 1 ? "the Lunar Cry's set (to see it)"
			: "no grade (to compare)");
		gradeFrame = ~0ull;
	}

	//once a frame: which two LUTs, how much of the second, how much of the grade
	void WorkGrade()
	{
		if (gradeFrame == frame)
			return;
		gradeFrame = frame;
		grade.on = false;
		const int mode = currentMode;
		if (!LUT || keyMode == 2 || (mode != MODE_FIELD && mode != MODE_BATTLE && mode != MODE_WORLDMAP))
			return;
		//the time here
		lutgrade::Blend blend;
		const fieldpbr::FieldTime fieldTime = DayNightPlaceTime();
		const bool sky = fieldSky.load();
		bool inside = false;
		int fixed = 0; //0 the clock's, 1 day, 2 night
		if (mode == MODE_FIELD)
		{
			fixed = fieldTime == fieldpbr::FieldTime::Day ? 1 : fieldTime == fieldpbr::FieldTime::Night ? 2 : 0;
			inside = !sky;
		}
		else
		{
			const int scene = SceneLightTimeHere(mode);
			if (scene >= 0)
				fixed = scene;
			else if (mode == MODE_BATTLE) //without SCENE_LIGHT: as its "night = auto" has it
				fixed = !fromWorld && (fieldTime != fieldpbr::FieldTime::Cycle || !sky) ? 1 : 0;
			inside = mode == MODE_BATTLE && !fromWorld && !sky;
		}
		if (!DAY_NIGHT)
			fixed = 1;
		blend = fixed == 0 ? lutgrade::At(DayNightCycleInUse(), DayNightClockTime()) : lutgrade::Fixed(fixed == 2);
		const int moment = LUT_LUNAR_FROM > 0 || keyMode == 1 || LUT_DEBUG ? SceneLightMoment() : -1;
		static int loggedMoment = -2;
		if (LUT_DEBUG && moment != loggedMoment)
		{
			//to find the Lunar Cry's value: play (or load a save) from just before it, and note where the counter goes
			loggedMoment = moment;
			OutputDebug("LUT: the story's counter is %d (for LUT_LUNAR_FROM, LUT_LUNAR_TO)\n", moment);
		}
		const bool lunar = keyMode == 1 || (LUT_LUNAR_FROM > 0 && moment >= LUT_LUNAR_FROM && moment <= LUT_LUNAR_TO);
		if (lunar)
			blend = lutgrade::Lunar(blend);
		if (blend.mix <= 0.0)
			blend.to = blend.from;
		else if (blend.mix >= 1.0)
		{
			blend.from = blend.to;
			blend.mix = 0.0;
		}
		//the files
		PlaceNow(mode);
		const ULONGLONG tick = GetTickCount64();
		int sizeA = 0, sizeB = 0;
		std::string fileA, fileB;
		GLuint a = SlotTexture(blend.from, tick, sizeA, fileA);
		GLuint b = blend.to == blend.from ? a : SlotTexture(blend.to, tick, sizeB, fileB);
		if (blend.to == blend.from)
		{
			sizeB = sizeA;
			fileB = fileA;
		}
		if (!place.said)
		{
			//the files of each time here: the first place, places with LUTs of their own, and every place with LUT_DEBUG
			place.said = true;
			static bool first = true;
			std::string list;
			bool own = false;
			for (int slot = 0; slot < lutgrade::SLOT_COUNT; slot++)
			{
				const std::string& file = SlotFile(slot, tick);
				const bool inRoot = file.rfind(Root(), 0) == 0;
				own = own || (!file.empty() && file.find('\\', Root().size()) != std::string::npos);
				list += std::string(slot == 0 ? "" : ", ") + lutgrade::SlotName(slot) + " "
					+ (file.empty() ? std::string("none") : inRoot ? "lut\\" + file.substr(Root().size()) : file);
			}
			if (first || own || LUT_DEBUG)
				OutputDebug("LUT: %s: %s\n", place.name.c_str(), list.c_str());
			first = false;
		}
		//a LUT of a different size than the other: the two can't share the shader's size, so only the nearer one is used
		if (a != 0 && b != 0 && a != b && sizeA != sizeB)
		{
			if (blend.mix < 0.5)
				b = a, sizeB = sizeA;
			else
				a = b, sizeA = sizeB;
		}
		if (a == 0 && b == 0)
			return; //no LUT for either time here: the colours as they are
		//a time without a LUT is the colours as they are: an identity LUT of the other one's size (the shader reads both with
		//one size)
		const int size = a != 0 ? sizeA : sizeB;
		if (a == 0 || b == 0)
		{
			GLuint& same = identities[size];
			if (same == 0)
				same = Upload(lutgrade::Identity(size));
			if (a == 0)
				a = same;
			if (b == 0)
				b = same;
		}
		const double share = inside ? std::clamp(LUT_INSIDE, 0.0, 1.0) : 1.0;
		const double strength = std::clamp(LUT_STRENGTH, 0.0, 1.0) * share;
		if (strength <= 0.0)
			return;
		grade.on = true;
		grade.a = a;
		grade.b = b;
		grade.blend = blend;
		grade.value[0] = static_cast<float>(blend.mix);
		grade.value[1] = static_cast<float>(strength);
		grade.value[2] = static_cast<float>(size);
		grade.value[3] = 0.0f;
		if (LUT_DEBUG)
		{
			const std::string text = place.name + ": " + lutgrade::BlendText(blend) + (lunar ? " (the Lunar Cry)" : "");
			const std::string coarse = place.name + lutgrade::SlotName(blend.from) + lutgrade::SlotName(blend.to);
			if (coarse != lastText)
			{
				lastText = coarse;
				OutputDebug("LUT: %s, %.0f%% of the grade%s\n", text.c_str(), strength * 100.0, inside ? " (not open to the sky: LUT_INSIDE)" : "");
			}
		}
	}

	void BindTextures()
	{
		if (glstate::Texture(GL_TEXTURE_2D, UNIT_A) == grade.a && glstate::Texture(GL_TEXTURE_2D, UNIT_B) == grade.b)
			return;
		const GLenum active = glstate::ActiveTexture();
		if (glstate::Texture(GL_TEXTURE_2D, UNIT_A) != grade.a)
		{
			glActiveTexture(GL_TEXTURE0 + UNIT_A);
			glBindTexture(GL_TEXTURE_2D, grade.a);
		}
		if (glstate::Texture(GL_TEXTURE_2D, UNIT_B) != grade.b)
		{
			glActiveTexture(GL_TEXTURE0 + UNIT_B);
			glBindTexture(GL_TEXTURE_2D, grade.b);
		}
		glActiveTexture(active);
	}

	//the program in use: graded or not
	void Send(const GLuint program, ProgramLut& p, const bool graded)
	{
		if (!p.looked)
		{
			p.looked = true;
			p.lut = glGetUniformLocation(program, "demasterLut");
			if (p.lut >= 0)
			{
				if (const GLint a = glGetUniformLocation(program, "demasterLutA"); a >= 0)
					glUniform1i(a, UNIT_A);
				if (const GLint b = glGetUniformLocation(program, "demasterLutB"); b >= 0)
					glUniform1i(b, UNIT_B);
			}
		}
		if (p.lut < 0)
			return;
		static const float none[4] = { 0, 0, 0, 0 };
		const bool on = graded && grade.on;
		const float* want = on ? grade.value : none;
		if (on)
			BindTextures();
		if (memcmp(p.sent, want, sizeof(p.sent)) != 0)
		{
			memcpy(p.sent, want, sizeof(p.sent));
			glUniform4fv(p.lut, 1, want);
		}
	}

	bool SceneType(const int type, const int mode)
	{
		if (mode == MODE_FIELD)
			return type == TYPE_FIELD_BACKGROUND || type == TYPE_FIELD_BACKGROUND_2 || type == TYPE_FIELD_MODEL;
		if (mode == MODE_BATTLE)
			return type == TYPE_BATTLE_STAGE || type == TYPE_BATTLE_CHARACTER || type == TYPE_WORLD || type == TYPE_WORLD_SKY;
		if (mode == MODE_WORLDMAP)
			return type == TYPE_WORLD || type == TYPE_WORLD_SKY;
		return false;
	}
}

void LutTextureFilled(const unsigned int texture, const bool whole)
{
	if (!LUT || texture == 0)
		return;
	const int type = static_cast<int>(TEX_TYPE);
	const bool scene = type == TYPE_BATTLE_STAGE || type == TYPE_WORLD || type == TYPE_FIELD_BACKGROUND || type == TYPE_WORLD_SKY
		|| type == TYPE_BATTLE_CHARACTER || type == TYPE_FIELD_BACKGROUND_2 || type == TYPE_FIELD_MODEL;
	const auto known = types.find(texture);
	if (known == types.end())
	{
		if (scene)
			types[texture] = type;
		return;
	}
	if (!scene && whole)
		types.erase(known); //another kind of picture now
	else
		known->second = type;
	if (types.size() > 8192)
		types.clear(); //textures the game deleted long ago
}

void LutTextureForget(const unsigned int texture)
{
	if (LUT && !types.empty())
		types.erase(texture);
}

void LutFieldEntered(const char* field, const bool sky)
{
	if (!LUT || field == nullptr)
		return;
	{
		std::lock_guard<std::mutex> lock(fieldLock);
		fieldName = field;
	}
	fieldSky = sky;
	fieldSerial++;
}

bool LutDrawAddsLight()
{
	if (!glstate::BlendOn())
		return false;
	GLint source = 0, destination = 0;
	glstate::BlendFactors(source, destination);
	return destination == GL_ONE || glstate::BlendEquationRgb() == GL_FUNC_REVERSE_SUBTRACT;
}

void LutBeforeDraw(const unsigned int indexType, const void* indices)
{
	if (!LUT)
		return;
	const int mode = currentMode;
	if (mode != lastMode)
	{
		if (mode == MODE_WORLDMAP)
			fromWorld = true;
		else if (mode == MODE_FIELD)
			fromWorld = false;
		lastMode = mode;
	}
	const GLint program = glstate::Program();
	if (program <= 0)
		return;
	ProgramLut& p = programs[static_cast<GLuint>(program)];
	if (!p.looked)
	{
		const ProgramKind kind = GameProgramOf(program).kind;
		if (kind != ProgramKind::Main && kind != ProgramKind::Character && kind != ProgramKind::NoTexture)
		{
			p.looked = true;
			p.lut = -1;
			return;
		}
	}
	else if (p.lut < 0)
		return;
	WorkGrade();
	bool graded = false;
	if (grade.on && mode == MODE_WORLDMAP)
	{
		float rhw = 0;
		graded = fieldrt::FirstCornerRhw(static_cast<GLenum>(indexType), indices, rhw) && rhw > 0.0f && rhw < fieldrt::SCENE_MOST_RHW
			&& !LutDrawAddsLight();
	}
	else if (grade.on && GameProgramOf(program).kind != ProgramKind::NoTexture)
	{
		const auto known = types.find(glstate::BoundTexture2D());
		graded = known != types.end() && SceneType(known->second, mode) && !LutDrawAddsLight();
	}
	Send(static_cast<GLuint>(program), p, graded);
}

void LutUse(const unsigned int program, const bool scene)
{
	if (!LUT || program == 0)
		return;
	WorkGrade();
	Send(program, programs[program], scene);
}

void LutFrameEnd()
{
	frame++;
	if (LUT)
		Keys();
}
