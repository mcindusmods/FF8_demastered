//SCENE_LIGHT (the DLL's side): the time of day and colour overrides in battles and on the world map.
//
//Everything the game draws there goes through its main.frag (DEMASTER's version: DEMASTER_EXP\shaders\main.frag), from
//textures times vertex colours. Before each of those draws DEMASTER sets two uniforms of that program:
//	demasterScene      what the picture is multiplied by, in linear light (scene_light.h: the day/night clock, the
//	                   settings and rules in DEMASTER_EXP\scene_light.txt), and "on"
//	demasterSceneMaps  whether the bound battle stage page has a picture of its glows (<page>_EC) and an ORM map
//	                   (<page>_ORM: its red channel, AO), which DEMASTER then has on texture units 10 and 11
//and sets them back to zero for every other draw of that program (the menus, the text, effects that are light themselves).
//
//Which draws: by the kind of texture bound (the game's own texture type, remembered when the texture is filled):
//	battle     the stage (type 11) and the fighters (type 35), unless the draw adds light (blend ONE, ONE)
//	world map  the world's own textures (type 18), unless the draw adds light
//Nothing here needs the camera: the picture keeps the light the game gave it and is only dimmed and tinted. Lighting a
//stage's normal map from a sun that stands still in the world needs the battle camera's rotation; SCENE_DEBUG=1 writes
//what is needed to find it (the projection's registers at a stage draw) to the log.
//
//Battles: the night only comes where "night = auto" allows it (scene_light.h): battles started on the world map, in a
//field open to the sky, or in a field that is always night.
#include "coreHeader.h"
#include "config.h"
#include "day_night.h"
#include "field_pbr_runtime.h"
#include "gl_state.h"
#include "opengl.h"
#include "scene_light.h"
#include "texture.h"
#include "image.h"
#include "mymemory.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

using namespace fieldrt;

namespace
{
	constexpr int TYPE_BATTLE_STAGE = 11, TYPE_WORLD = 18, TYPE_BATTLE_CHARACTER = 35;
	constexpr int UNIT_GLOWS = 10, UNIT_ORM = 11;

	struct SceneTexture
	{
		int type = 0; //the game's texture type at its last upload
		int stage = -1, page = -1; //a battle stage page DEMASTER replaced
		GLuint glows = 0, orm = 0; //its maps (0: none); the textures belong to `maps` below
	};
	std::unordered_map<GLuint, SceneTexture> textures;
	bool loadingOwn = false; //DEMASTER is filling one of its own map textures: not the game's
	//a stage's map files, read once per battle stage (an animated page comes back every few frames); 0: no such file
	std::unordered_map<std::string, GLuint> maps;
	int mapsStage = -1;

	struct ProgramState
	{
		bool looked = false;
		GLint scene = -1, maps = -1;
		float sceneValue[4] = { 0, 0, 0, 0 }, mapsValue[4] = { 0, 0, 0, 0 }; //as last sent
	};
	std::unordered_map<GLint, ProgramState> programs;
	int programsOn = 0; //programs whose demasterScene is not zero right now

	scenelight::Settings settings;
	std::string settingsPath;
	std::filesystem::file_time_type settingsTime{};
	ULONGLONG settingsChecked = 0;
	bool settingsRead = false;

	//the place the last battle started from
	bool fromWorld = false;
	bool fieldSky = false; //the last field entered is open to the sky (sky = 1 in its _pbr.txt)
	int lastMode = -1;

	//this moment's look (worked out a few times a second, not per draw)
	struct Now
	{
		ULONGLONG at = 0;
		int mode = -1, stage = -1;
		float multiplier[3] = { 1, 1, 1 };
		float night = 0, ao = 0, glow = 1;
		bool on = false;
	} now;

	void ForgetMaps()
	{
		for (auto& [path, texture] : maps)
			if (texture != 0)
				glDeleteTextures(1, &texture);
		maps.clear();
		for (auto& [id, t] : textures)
			t.glows = t.orm = 0;
	}

	void ReadSettings()
	{
		const ULONGLONG tick = GetTickCount64();
		if (settingsRead && tick - settingsChecked < 1000)
			return;
		settingsChecked = tick;
		if (settingsPath.empty())
			settingsPath = std::string(DIRECT_IO_EXPORT_DIR) + "scene_light.txt";
		std::error_code error;
		if (!std::filesystem::exists(settingsPath, error))
		{
			//the first time: the defaults, explained, for you to change
			std::ofstream out(settingsPath, std::ios::binary);
			out << scenelight::DefaultText();
			OutputDebug("SCENE_LIGHT: %s written (the defaults; change it while you play)\n", settingsPath.c_str());
		}
		const auto time = std::filesystem::last_write_time(settingsPath, error);
		if (settingsRead && time == settingsTime)
			return;
		std::ifstream in(settingsPath, std::ios::binary);
		std::ostringstream text;
		text << in.rdbuf();
		std::string warnings;
		scenelight::Parse(in ? text.str() : scenelight::DefaultText(), settings, warnings);
		if (!warnings.empty())
			OutputDebug("SCENE_LIGHT: %s:\n%s", settingsPath.c_str(), warnings.c_str());
		OutputDebug("SCENE_LIGHT: %s read%s: %zu rule(s)\n", settingsPath.c_str(), settingsRead ? " again" : "", settings.rules.size());
		settingsRead = true;
		settingsTime = time;
		now.at = 0;
	}

	//guest memory (the game's own data), through the page table the recompiled code uses
	bool GuestRead(void* destination, const DWORD guest, const size_t length)
	{
		static const DWORD table = HasAddress(GUEST_PAGE_TABLE) ? IMAGE_BASE + GetAddress(GUEST_PAGE_TABLE) : 0;
		DWORD page = 0;
		if (table == 0 || (guest & 0xFFF) + length > 0x1000
			|| !SafeRead(&page, reinterpret_cast<const DWORD*>(table) + (guest >> 12), 4) || page == 0)
			return false;
		return SafeRead(destination, reinterpret_cast<const void*>(page + (guest & 0xFFF)), length);
	}

	//the story's progress (the game's own counter); -1: not known
	int Moment()
	{
		static const DWORD at = HasAddress(GAME_MOMENT) ? GetAddress(GAME_MOMENT) : 0;
		uint16_t value = 0;
		return at != 0 && GuestRead(&value, at, 2) ? value : -1;
	}

	void Work(const int mode)
	{
		const ULONGLONG tick = GetTickCount64();
		if (now.at != 0 && tick - now.at < 100 && now.mode == mode && now.stage == currentStage)
			return;
		ReadSettings();
		const bool battle = mode == MODE_BATTLE;
		const int moment = Moment();
		const scenelight::Look look = scenelight::Resolve(settings, battle ? scenelight::Where::Battle : scenelight::Where::World,
			currentStage, moment);
		//the clock, or the time of the place the battle started in when that is fixed (a field with time = day or night)
		const fieldpbr::FieldTime place = DayNightPlaceTime();
		const bool outdoors = !battle || fromWorld || fieldSky || place == fieldpbr::FieldTime::Night;
		const double night = scenelight::NightHere(look, battle ? DayNightAmount(place) : DayNightClockAmount(), outdoors);
		if (SCENE_DEBUG && (now.mode != mode || now.stage != currentStage))
			OutputDebug("SCENE_LIGHT: %s%s, the story's counter is %d; the night %s here (%s), now %.2f\n", battle ? "battle, stage " : "the world map",
				battle ? std::to_string(currentStage).c_str() : "", moment, look.time != scenelight::Time::Cycle ? "is fixed" : look.night == 0
				? "doesn't come" : look.night > 0 || outdoors ? "comes" : "doesn't come", battle ? (fromWorld ? "started on the world map"
				: fieldSky ? "started in a field open to the sky" : place == fieldpbr::FieldTime::Night ? "started in a field that is always night"
				: "started in a field that isn't open to the sky") : "the world map", night);
		now.at = tick;
		now.mode = mode;
		now.stage = currentStage;
		scenelight::Multiplier(look, night, now.multiplier);
		now.night = static_cast<float>(night);
		now.ao = look.ao;
		now.glow = look.glow;
		now.on = true;
	}

	//one of a stage page's maps in a texture of DEMASTER's own (0: no such file, or it can't be read)
	GLuint LoadMap(const std::string& stem, const char* suffix)
	{
		const std::string key = stem + suffix;
		if (const auto known = maps.find(key); known != maps.end())
			return known->second;
		GLuint& made = maps[key];
		char path[MAX_PATH]{ 0 };
		if (DDSorPNG(path, MAX_PATH, "%s%s", stem.c_str(), suffix))
			return 0;
		const SafeBimg image = LoadImageFromFile(path);
		if (!image)
			return 0;
		GLuint texture = 0;
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		loadingOwn = true;
		RenderTexture(image.get());
		loadingOwn = false;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		made = texture;
		return texture;
	}

	//a page's map: the one of this frame of an animated page (a0stg012_16_3_EC), else the page's (a0stg012_16_EC)
	GLuint PageMap(const std::string& stem, const int stage, const int page, const char* suffix)
	{
		if (const GLuint own = LoadMap(stem, suffix))
			return own;
		char name[64];
		snprintf(name, sizeof(name), "a0stg%03d_%d", stage, page);
		const size_t slash = stem.find_last_of("\\/");
		const std::string plain = (slash == std::string::npos ? std::string() : stem.substr(0, slash + 1)) + name;
		return plain == stem ? 0 : LoadMap(plain, suffix);
	}

	//SCENE_DEBUG: what a later build needs to light stages from a sun that stands still in the world: the projection's
	//registers (the game keeps the PlayStation's: rotation, translation, screen centre, distance) while a stage is drawn,
	//and the first corner of the draw. Once per battle.
	void LogProjection(const GLint program, const int count)
	{
		static int loggedStage = -2;
		static ULONGLONG loggedAt = 0;
		const ULONGLONG tick = GetTickCount64();
		if (loggedStage == currentStage && tick - loggedAt < 20000)
			return;
		loggedStage = currentStage;
		loggedAt = tick;
		static const DWORD at = HasAddress(GTE_CONTROL) ? GetAddress(GTE_CONTROL) : 0;
		int32_t r[32]{};
		if (at == 0 || !GuestRead(r, at, sizeof(r)))
		{
			OutputDebug("SCENE_LIGHT: the projection's registers can't be read in this game DLL\n");
			return;
		}
		const auto low = [](const int32_t v) { return static_cast<int>(static_cast<int16_t>(v & 0xFFFF)); };
		const auto high = [](const int32_t v) { return static_cast<int>(static_cast<int16_t>((v >> 16) & 0xFFFF)); };
		float first[4] = { 0, 0, 0, 0 };
		void* pointer = LastVerticesPointer.pointer != nullptr ? const_cast<void*>(static_cast<const void*>(LastVerticesPointer.pointer)) : nullptr;
		const bool haveVertex = pointer != nullptr && LastVerticesPointer.size == 4 && SafeRead(first, pointer, sizeof(first));
		const GLint vertexType = glGetUniformLocation(static_cast<GLuint>(program), "vertextype");
		OutputDebug("SCENE_LIGHT: stage %d, a stage draw of %d indices (vertex type %d). Rotation (4096 = 1): %d %d %d / %d %d %d / %d %d %d; "
			"translation %d %d %d; screen centre %.2f %.2f; distance %d. First corner: x %.2f y %.2f z %.5f rhw %.8f%s\n", currentStage, count,
			vertexType >= 0 ? glstate::UniformInt(program, vertexType) : -1, low(r[0]), high(r[0]), low(r[1]), high(r[1]), low(r[2]), high(r[2]),
			low(r[3]), high(r[3]), low(r[4]), r[5], r[6], r[7], r[24] / 65536.0, r[25] / 65536.0, r[26] & 0xFFFF, first[0], first[1], first[2],
			first[3], haveVertex ? "" : " (not read)");
	}

	void Send(const GLint program, ProgramState& p, const float scene[4], const float maps[4])
	{
		if (memcmp(p.sceneValue, scene, sizeof(p.sceneValue)) != 0)
		{
			programsOn += (scene[3] > 0) - (p.sceneValue[3] > 0);
			memcpy(p.sceneValue, scene, sizeof(p.sceneValue));
			glUniform4fv(p.scene, 1, scene);
		}
		if (p.maps >= 0 && memcmp(p.mapsValue, maps, sizeof(p.mapsValue)) != 0)
		{
			memcpy(p.mapsValue, maps, sizeof(p.mapsValue));
			glUniform4fv(p.maps, 1, maps);
		}
	}
}

//the glTexImage2D / glTexSubImage2D hooks: the game fills a texture (its type is the one of the upload under way)
void SceneLightTextureFilled(const unsigned int texture, const bool whole)
{
	if (!SCENE_LIGHT || loadingOwn || texture == 0)
		return;
	const int type = static_cast<int>(TEX_TYPE);
	const auto known = textures.find(texture);
	if (known == textures.end())
	{
		if (type == TYPE_BATTLE_STAGE || type == TYPE_WORLD || type == TYPE_BATTLE_CHARACTER)
			textures[texture].type = type;
		return;
	}
	if (whole)
	{
		//a new picture: the maps of the page it was are not its maps
		known->second.glows = known->second.orm = 0;
		known->second.stage = known->second.page = -1;
	}
	known->second.type = type;
	if (textures.size() > 4096) //textures the game deleted long ago
		textures.clear();
}

void SceneLightTextureForget(const unsigned int texture)
{
	if (SCENE_LIGHT && !loadingOwn && !textures.empty())
		textures.erase(texture);
}

//texturepatch_v2_battleScenery.cpp: DEMASTER just put its own picture of a battle stage page into the bound texture.
//file: that picture's file. Its maps are next to it: <file without extension>_EC and _ORM (.dds or .png)
void SceneLightStagePage(const int stage, const int page, const char* file)
{
	if (!SCENE_LIGHT || file == nullptr)
		return;
	try
	{
		CheckGlew();
		GLint bound = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
		if (bound == 0)
			return;
		if (mapsStage != stage)
		{
			ForgetMaps(); //another stage: the last one's maps go
			mapsStage = stage;
		}
		SceneTexture& t = textures[static_cast<GLuint>(bound)];
		t.type = TYPE_BATTLE_STAGE;
		t.stage = stage;
		t.page = page;
		std::string stem = file;
		if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos)
			stem.resize(dot);
		t.glows = PageMap(stem, stage, page, "_EC");
		t.orm = PageMap(stem, stage, page, "_ORM");
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
		static std::string said;
		if ((t.glows != 0 || t.orm != 0 || SCENE_DEBUG) && said != stem)
		{
			said = stem;
			OutputDebug("SCENE_LIGHT: stage %d page %d: %s%s%s\n", stage, page, t.glows != 0 ? "a picture of its glows (_EC)" : "no _EC",
				t.orm != 0 ? ", an ORM map" : ", no _ORM", t.glows == 0 && t.orm == 0 ? " (next to its picture; the time of day works without them)" : "");
		}
	}
	catch (...)
	{
		OutputDebug("SCENE_LIGHT: reading a stage page's maps failed\n");
	}
}

//game thread, when a field's .map is read: is that field open to the sky (sky = 1 in its _pbr.txt)
void SceneLightFieldEntered(const bool sky)
{
	fieldSky = sky;
}

//glDrawElements hook, before every draw of the game
void SceneLightBeforeDraw(const int count)
{
	if (!SCENE_LIGHT)
		return;
	const int mode = currentMode;
	if (mode != lastMode)
	{
		//where a battle starts from: the last of the two it can start from
		if (mode == MODE_WORLDMAP)
			fromWorld = true;
		else if (mode == MODE_FIELD)
			fromWorld = false;
		lastMode = mode;
	}
	const bool scene = mode == MODE_BATTLE || mode == MODE_WORLDMAP;
	if (!scene && programsOn == 0)
		return;
	const GLint program = glstate::Program();
	if (program == 0)
		return;
	ProgramState& p = programs[program];
	if (!p.looked)
	{
		p.looked = true;
		if (GameProgramOf(program).kind == ProgramKind::Main)
		{
			p.scene = glGetUniformLocation(static_cast<GLuint>(program), "demasterScene");
			p.maps = glGetUniformLocation(static_cast<GLuint>(program), "demasterSceneMaps");
			if (p.scene < 0)
				OutputDebug("SCENE_LIGHT: the game's main.frag (program %d) is not this release's: battles and the world map stay as they "
					"are. Copy DEMASTER_EXP\\shaders\\main.frag from this release.\n", program);
			else
			{
				if (const GLint glows = glGetUniformLocation(static_cast<GLuint>(program), "demasterSceneEC"); glows >= 0)
					glUniform1i(glows, UNIT_GLOWS);
				if (const GLint orm = glGetUniformLocation(static_cast<GLuint>(program), "demasterSceneORM"); orm >= 0)
					glUniform1i(orm, UNIT_ORM);
			}
		}
	}
	if (p.scene < 0)
		return;
	float sceneValue[4] = { 0, 0, 0, 0 }, mapsValue[4] = { 0, 0, 0, 0 };
	if (scene)
	{
		const auto known = textures.find(glstate::BoundTexture2D());
		bool lit = known != textures.end() && (mode == MODE_BATTLE ? known->second.type == TYPE_BATTLE_STAGE
			|| known->second.type == TYPE_BATTLE_CHARACTER : known->second.type == TYPE_WORLD);
		if (lit && glstate::BlendOn())
		{
			GLint source = 0, destination = 0;
			glstate::BlendFactors(source, destination);
			lit = !(destination == GL_ONE); //the draw adds light (a glow, a spell): it is light, not something lit
		}
		if (lit)
		{
			Work(mode);
			sceneValue[0] = now.multiplier[0];
			sceneValue[1] = now.multiplier[1];
			sceneValue[2] = now.multiplier[2];
			sceneValue[3] = 1.0f;
			const SceneTexture& t = known->second;
			if (t.glows != 0 || t.orm != 0)
			{
				//its maps on DEMASTER's two units (the game uses neither)
				const GLenum active = glstate::ActiveTexture();
				if (t.glows != 0 && glstate::Texture(GL_TEXTURE_2D, UNIT_GLOWS) != t.glows)
				{
					glActiveTexture(GL_TEXTURE0 + UNIT_GLOWS);
					glBindTexture(GL_TEXTURE_2D, t.glows);
				}
				if (t.orm != 0 && glstate::Texture(GL_TEXTURE_2D, UNIT_ORM) != t.orm)
				{
					glActiveTexture(GL_TEXTURE0 + UNIT_ORM);
					glBindTexture(GL_TEXTURE_2D, t.orm);
				}
				if (glstate::ActiveTexture() != active)
					glActiveTexture(active);
				mapsValue[0] = t.glows != 0 ? 1.0f : 0.0f;
				mapsValue[1] = now.night;
				mapsValue[2] = now.glow;
				mapsValue[3] = t.orm != 0 ? now.ao : 0.0f;
			}
			if (SCENE_DEBUG && mode == MODE_BATTLE && t.type == TYPE_BATTLE_STAGE)
				LogProjection(program, count);
		}
	}
	Send(program, p, sceneValue, mapsValue);
}
