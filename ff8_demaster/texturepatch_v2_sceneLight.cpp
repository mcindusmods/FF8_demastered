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
//	battle     the stage and the enemies (type 11), the party (35) and, in a battle on the world map, the world's ground
//	           and sky the stage is made of (18, 27); not draws that add light (blend ONE, ONE: spells, glows)
//	world map  the world's own textures (18, 27): the ground, the sky, the haze, the people and vehicles on it
//
//Battles are also lit from the stage's own space (scene_light.h, "the battle camera"): the sun and the moon cross the
//stage by the clock, a stage page's normal map (<page>_N) and ORM map answer to them, and a stage can have four lights of
//its own (scene_light.txt). For that the shader gets, once a frame, the numbers the game's projection used (read from the
//game's memory when the first battle draw of the frame arrives) and the sun and the lights in view space. The world map
//is only dimmed and tinted.
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
	//(11: battle stages and the enemies; 18 and 27: the world map's ground, its sky and horizon; 35: the party in battle)
	constexpr int TYPE_BATTLE_STAGE = 11, TYPE_WORLD = 18, TYPE_WORLD_SKY = 27, TYPE_BATTLE_CHARACTER = 35;
	constexpr int UNIT_GLOWS = 10, UNIT_ORM = 11, UNIT_NORMAL = 12, UNIT_SHADOW = 13;
	constexpr double FIGHTER_SIZE = 4000.0; //a draw larger than this (the stage's units) casts no shadow: it is the stage's
	constexpr float SKY_DISTANCE = 20000.0f; //a stage's sky stands about 30,000 units from its middle; the fighters within a few thousand

	struct SceneTexture
	{
		int type = 0; //the game's texture type at its last upload
		int stage = -1, page = -1; //a battle stage page DEMASTER replaced
		GLuint glows = 0, orm = 0, normal = 0; //its maps (0: none); the textures belong to `maps` below
	};
	std::unordered_map<GLuint, SceneTexture> textures;
	bool loadingOwn = false; //DEMASTER is filling one of its own map textures: not the game's
	//a stage's map files, read once per battle stage (an animated page comes back every few frames); 0: no such file
	std::unordered_map<std::string, GLuint> maps;
	int mapsStage = -1;

	struct ProgramState
	{
		bool looked = false;
		GLint scene = -1, maps = -1, draw = -1, proj = -1, centre = -1, rot = -1, trans = -1, sun = -1, sunLight = -1, lightAt = -1,
			lightColour = -1, shadowMatrix = -1, shadowInfo = -1;
		uint64_t shadowSent = 0; //the serial of the shadow map this program has (0: none)
		float sceneValue[4] = { 0, 0, 0, 0 }, mapsValue[4] = { 0, 0, 0, 0 }, drawValue[4] = { 0, 0, 0, 0 }; //as last sent
		uint64_t stageSent = 0; //the serial of the stage's light (below) this program has
	};

	//The stage's light, worked out once a frame: the camera, the sun and the stage's own lights in view space
	struct StageLight
	{
		uint64_t serial = 1, frame = ~0ull;
		bool camera = false;
		scenelight::Camera view; //the camera itself (for the fighters' corners)
		double sunStage[3] = { 0, -1, 0 }; //towards the sun or moon, the stage's space
		float sunShadow = 0; //how much of the sun a shadow takes (the look's shadow x the sun's strength)
		float proj[4] = { 0, 0, 0, 0 }, centre[4] = { 0, 0, 0, 0 }, rot[9] = {}, trans[3] = {}, sun[4] = { 0, 0, 0, 0 },
			sunLight[4] = { 0, 0, 0, 0 }, lightAt[16] = {}, lightColour[16] = {};
	} stageLight;
	uint64_t sceneFrame = 0; //counts SwapBuffers
	scenelight::CameraFilter cameraFilter;
	scenelight::Look battleLook; //this moment's, for the stage's light
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
		float evening = 0; //the glows and the day-only lights go with this (DAY_NIGHT_EVENING: earlier than the night)
		bool on = false;
		bool sunByClock = false; //the night comes here: the sun and the moon go by the clock (else they stand at their highest)
	} now;

	void ForgetMaps()
	{
		for (auto& [path, texture] : maps)
			if (texture != 0)
				glDeleteTextures(1, &texture);
		maps.clear();
		for (auto& [id, t] : textures)
			t.glows = t.orm = t.normal = 0;
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
		//the clock; a battle started in a field with a fixed time (time = day or night in its _pbr.txt: an inside place, a
		//story scene) is fought by day: the field's time is for its own lights, the stage has its own
		const fieldpbr::FieldTime place = DayNightPlaceTime();
		const bool fixedPlace = battle && !fromWorld && place != fieldpbr::FieldTime::Cycle;
		const bool outdoors = !battle || fromWorld || fieldSky;
		const double night = scenelight::NightHere(look, fixedPlace ? 0.0 : DayNightClockAmount(), outdoors);
		const double evening = scenelight::NightHere(look, fixedPlace ? 0.0 : DayNightClockEvening(), outdoors);
		if (SCENE_DEBUG && (now.mode != mode || now.stage != currentStage))
			OutputDebug("SCENE_LIGHT: %s%s, the story's counter is %d; the night %s here (%s), now %.2f\n", battle ? "battle, stage " : "the world map",
				battle ? std::to_string(currentStage).c_str() : "", moment, look.time != scenelight::Time::Cycle ? "is fixed" : look.night == 0
				? "doesn't come" : look.night > 0 || outdoors ? "comes" : "doesn't come", battle ? (fromWorld ? "started on the world map"
				: fixedPlace ? "started in a field with a fixed time: fought by day" : fieldSky ? "started in a field open to the sky"
				: "started in a field that isn't open to the sky") : "the world map", night);
		now.at = tick;
		now.mode = mode;
		now.stage = currentStage;
		scenelight::Multiplier(look, night, now.multiplier);
		now.night = static_cast<float>(night);
		now.evening = static_cast<float>(evening);
		now.ao = look.ao;
		now.glow = look.glow;
		now.on = true;
		now.sunByClock = !fixedPlace && look.time == scenelight::Time::Cycle && scenelight::NightHere(look, 1.0, outdoors) > 0.0;
		if (battle)
			battleLook = look;
	}

	//Once a frame in battle: the camera from the game's projection registers, the sun and the stage's lights in view space
	void WorkStageLight()
	{
		if (stageLight.frame == sceneFrame)
			return;
		stageLight.frame = sceneFrame;
		StageLight next;
		next.serial = stageLight.serial;
		next.frame = sceneFrame;
		static const DWORD at = HasAddress(GTE_CONTROL) ? GetAddress(GTE_CONTROL) : 0;
		int32_t registers[32]{};
		scenelight::Camera read, camera;
		const bool readable = at != 0 && GuestRead(registers, at, sizeof(registers)) && scenelight::CameraFromRegisters(registers, read);
		GLint viewport[4] = { 0, 0, 0, 0 };
		glGetIntegerv(GL_VIEWPORT, viewport);
		if (cameraFilter.Frame(readable, read, camera) && viewport[2] > 0 && viewport[3] > 0)
		{
			next.camera = true;
			next.view = camera;
			//the game's 640 x 480 units fill the viewport; the PlayStation's pixels are half of those
			next.proj[0] = 320.0f / static_cast<float>(viewport[2]);
			next.proj[1] = 240.0f / static_cast<float>(viewport[3]);
			next.proj[2] = static_cast<float>(viewport[3]);
			next.proj[3] = static_cast<float>(camera.distance);
			next.centre[0] = static_cast<float>(camera.centreX);
			next.centre[1] = static_cast<float>(camera.centreY);
			next.centre[2] = battleLook.specular;
			next.centre[3] = SKY_DISTANCE;
			for (int i = 0; i < 9; i++)
				next.rot[i] = static_cast<float>(camera.r[i]); //rows sent as columns: the transpose, view to stage
			for (int i = 0; i < 3; i++)
				next.trans[i] = static_cast<float>(camera.t[i]);
			//where the night doesn't come (or the time is fixed) the sun, or the moon, stands at its highest
			fieldpbr::SkyTime sky;
			sky.night = battleLook.time == scenelight::Time::Night;
			if (now.sunByClock)
				sky = DayNightSky();
			const scenelight::Sun sun = scenelight::SunOver(battleLook, sky.night, sky.phase);
			double towards[3];
			scenelight::DirectionToView(camera, sun.direction, towards);
			const double strength = sun.strength;
			for (int i = 0; i < 3; i++)
				next.sunStage[i] = sun.direction[i];
			next.sunShadow = battleLook.shadow * static_cast<float>(strength);
			for (int i = 0; i < 3; i++)
			{
				next.sun[i] = static_cast<float>(towards[i]);
				next.sunLight[i] = sun.colour[i] * static_cast<float>(strength);
			}
			next.sun[3] = battleLook.sun * static_cast<float>(strength);
			next.sunLight[3] = battleLook.relief * static_cast<float>(strength);
			size_t count = 0;
			for (const scenelight::Light& light : battleLook.lights)
			{
				const float on = light.when == 1 ? now.night : light.when == 2 ? 1.0f - now.evening : 1.0f;
				if (on <= 0.0f || count >= scenelight::MAX_LIGHTS)
					continue;
				const double place[3] = { light.x, -light.up, light.z }; //the stage's y runs down
				double view[3];
				scenelight::PlaceToView(camera, place, view);
				for (int i = 0; i < 3; i++)
				{
					next.lightAt[count * 4 + i] = static_cast<float>(view[i]);
					next.lightColour[count * 4 + i] = light.colour[i] * on;
				}
				next.lightAt[count * 4 + 3] = light.range;
				count++;
			}
		}
		if (memcmp(next.proj, stageLight.proj, sizeof(float) * (4 + 4 + 9 + 3 + 4 + 4 + 16 + 16)) != 0 || next.camera != stageLight.camera)
			next.serial = stageLight.serial + 1;
		stageLight = next;
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

	//--- the fighters' shadows on the stage: last frame's fighters as the sun sees them (a depth map), sampled by the stage
	struct ShadowMap
	{
		bool tried = false, failed = false;
		GLuint framebuffer = 0, depth = 0, program = 0;
		GLint uvLocation = -1;
		int size = 0;
		bool on = false; //this frame's map holds something
		uint64_t serial = 0, frame = ~0ull;
		float matrix[16] = {};
		float info[4] = { 0, 0, 0, 0 }; //strength, bias, one texel, on
	} shadow;

	void SendShadow(ProgramState& p, const bool stage)
	{
		if (p.shadowInfo < 0)
			return;
		const uint64_t want = stage && shadow.on ? shadow.serial : 0;
		if (p.shadowSent == want)
			return;
		p.shadowSent = want;
		static const float none[4] = { 0, 0, 0, 0 };
		if (want != 0)
		{
			glUniformMatrix4fv(p.shadowMatrix, 1, GL_FALSE, shadow.matrix);
			glUniform4fv(p.shadowInfo, 1, shadow.info);
		}
		else
			glUniform4fv(p.shadowInfo, 1, none);
	}

	void Send(const GLint program, ProgramState& p, const float scene[4], const float maps[4], const float draw[4], const bool stage)
	{
		if (p.draw >= 0 && memcmp(p.drawValue, draw, sizeof(p.drawValue)) != 0)
		{
			memcpy(p.drawValue, draw, sizeof(p.drawValue));
			glUniform4fv(p.draw, 1, draw);
		}
		//the stage's light: when it changed since this program last got it (once a frame while the camera moves). Outside
		//battles the distance goes to zero and the shader skips all of it
		const uint64_t want = stage ? stageLight.serial : 0;
		if (p.proj >= 0 && p.stageSent != want)
		{
			p.stageSent = want;
			static const float none[4] = { 0, 0, 0, 0 };
			const bool camera = stage && stageLight.camera;
			glUniform4fv(p.proj, 1, camera ? stageLight.proj : none);
			if (camera)
			{
				glUniform4fv(p.centre, 1, stageLight.centre);
				glUniformMatrix3fv(p.rot, 1, GL_FALSE, stageLight.rot);
				glUniform3fv(p.trans, 1, stageLight.trans);
				glUniform4fv(p.sun, 1, stageLight.sun);
				glUniform4fv(p.sunLight, 1, stageLight.sunLight);
				glUniform4fv(p.lightAt, 4, stageLight.lightAt);
				glUniform4fv(p.lightColour, 4, stageLight.lightColour);
			}
		}
		SendShadow(p, stage && draw[0] > 0.5f);
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

	//the uniforms of a program with this release's main.frag (the game's main program, or DEMASTER's fighter program)
	void LookUpUniforms(const GLint program, ProgramState& p)
	{
		const auto at = [&](const char* name) { return glGetUniformLocation(static_cast<GLuint>(program), name); };
		p.scene = at("demasterScene");
		p.maps = at("demasterSceneMaps");
		p.draw = at("demasterSceneDraw");
		p.proj = at("demasterSceneProj");
		p.centre = at("demasterSceneCentre");
		p.rot = at("demasterSceneRot");
		p.trans = at("demasterSceneTrans");
		p.sun = at("demasterSceneSun");
		p.sunLight = at("demasterSceneSunLight");
		p.lightAt = at("demasterSceneLightAt[0]");
		p.lightColour = at("demasterSceneLightColour[0]");
		p.shadowMatrix = at("demasterSceneShadowMatrix");
		p.shadowInfo = at("demasterSceneShadowInfo");
		if (p.scene < 0 || p.proj < 0)
		{
			p.scene = -1;
			return;
		}
		const GLint previous = glstate::Program();
		if (previous != program)
			glUseProgram(static_cast<GLuint>(program));
		if (const GLint normal = at("demasterSceneN"); normal >= 0)
			glUniform1i(normal, UNIT_NORMAL);
		if (const GLint glows = at("demasterSceneEC"); glows >= 0)
			glUniform1i(glows, UNIT_GLOWS);
		if (const GLint orm = at("demasterSceneORM"); orm >= 0)
			glUniform1i(orm, UNIT_ORM);
		if (const GLint map = at("demasterSceneShadowMap"); map >= 0)
			glUniform1i(map, UNIT_SHADOW);
		if (previous != program)
			glUseProgram(static_cast<GLuint>(previous));
	}

	//--- the fighters, drawn by DEMASTER with smooth normals (the game's corners, its texture and state, main.frag with
	//DEMASTER_FIGHTER and demaster_fighter.vert, which hands the normals on). The field characters' smoothing
	//(fieldchars::SmoothNormals): corners at the same place on the screen are one corner, faces less than 60 degrees apart
	//are smoothed together. What they cover goes into the next frame's shadow map, in the stage's space.
	struct FighterVertex
	{
		float pos[4];
		uint8_t color[4];
		float uv[2];
		float normal[3];
	};
	struct Caster
	{
		GLuint texture = 0;
		size_t first = 0, count = 0; //corners in places / 3
	};
	struct Fighters
	{
		bool tried = false;
		GLuint program = 0;
		ProgramState state;
		GLint vertextype = -1, blendMode = -1, effecttype = -1, effectoption = -1, tileClamp = -1;
		std::vector<FighterVertex> vertices;
		std::vector<float> places, uv, lastPlaces, lastUv; //the stage's space, 3 per corner; 2 per corner
		std::vector<Caster> casters, lastCasters;
		uint64_t frame = ~0ull;
		size_t drawn = 0, logged = 0;
	} fighters;

	uint64_t CornerKey(const fieldchars::GameVertex& v)
	{
		uint32_t a, b, c;
		memcpy(&a, &v.x, 4);
		memcpy(&b, &v.y, 4);
		memcpy(&c, &v.rhw, 4);
		uint64_t h = 0x9E3779B97F4A7C15ull ^ a;
		h = (h ^ (static_cast<uint64_t>(b) << 21)) * 0xBF58476D1CE4E5B9ull;
		h = (h ^ (h >> 29) ^ c) * 0x94D049BB133111EBull;
		return h ^ (h >> 32);
	}

	bool MakeFighterProgram()
	{
		Fighters& f = fighters;
		if (f.tried)
			return f.program != 0;
		f.tried = true;
		std::string error;
		f.program = BuildProgram("demaster_fighter.vert", "main.frag", error, nullptr, "#define DEMASTER_FIGHTER 1\n");
		if (f.program == 0)
		{
			OutputDebug("SCENE_LIGHT: the fighters are drawn by the game (faceted under the lights): %s. Copy DEMASTER_EXP\\shaders\\ from "
				"this release\n", error.c_str());
			return false;
		}
		LookUpUniforms(static_cast<GLint>(f.program), f.state);
		f.state.looked = true;
		const auto at = [&](const char* name) { return glGetUniformLocation(f.program, name); };
		f.vertextype = at("vertextype");
		f.blendMode = at("blend_mode");
		f.effecttype = at("effecttype");
		f.effectoption = at("effectoption");
		f.tileClamp = at("demasterTileClamp");
		const GLint previous = glstate::Program();
		glUseProgram(f.program);
		if (const GLint tex = at("tex"); tex >= 0)
			glUniform1i(tex, 0);
		glUseProgram(static_cast<GLuint>(previous));
		if (f.state.scene < 0)
		{
			OutputDebug("SCENE_LIGHT: DEMASTER_EXP\\shaders\\main.frag is not this release's: the fighters are drawn by the game\n");
			glDeleteProgram(f.program);
			f.program = 0;
			return false;
		}
		OutputDebug("SCENE_LIGHT: the fighters are drawn by DEMASTER with smooth normals (program %u); their shadows fall on the stage\n",
			f.program);
		return true;
	}

	//a fighter's draw: true when DEMASTER drew it (the game's draw is skipped)
	bool DrawFighter(const GLint gameProgram, const GLenum mode, const GLsizei count, const GLenum type, const void* indices,
		const float scene[4], const float maps[4], float draw[4])
	{
		Fighters& f = fighters;
		if (mode != GL_TRIANGLES || count < 3 || count % 3 != 0 || !stageLight.camera || !MakeFighterProgram())
			return false;
		static std::vector<fieldchars::GameVertex> corners;
		if (!ReadDrawCorners(count, type, indices, corners) || corners.size() != static_cast<size_t>(count))
			return false;
		if (f.frame != sceneFrame)
		{
			f.frame = sceneFrame;
			f.drawn = 0;
		}
		const scenelight::Camera& camera = stageLight.view;
		static std::vector<fieldchars::V3> positions, normals;
		static std::vector<uint64_t> keys;
		positions.resize(corners.size());
		keys.resize(corners.size());
		for (size_t i = 0; i < corners.size(); i++)
		{
			double v[3];
			scenelight::ScreenToView(camera, corners[i].x, corners[i].y, corners[i].rhw, v);
			positions[i] = { static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]) };
			keys[i] = CornerKey(corners[i]);
		}
		fieldchars::SmoothNormals(positions, keys, 60.0f, normals);
		f.vertices.resize(corners.size());
		for (size_t i = 0; i < corners.size(); i++)
		{
			FighterVertex& v = f.vertices[i];
			v.pos[0] = corners[i].x;
			v.pos[1] = corners[i].y;
			v.pos[2] = corners[i].z;
			v.pos[3] = corners[i].rhw;
			memcpy(v.color, corners[i].color, 4);
			v.uv[0] = corners[i].u;
			v.uv[1] = corners[i].v;
			v.normal[0] = normals[i].x;
			v.normal[1] = normals[i].y;
			v.normal[2] = normals[i].z;
		}
		//for the next frame's shadow map: its corners in the stage's space (the sky's far wall stays out)
		const GLuint texture = glstate::BoundTexture2D();
		if (battleLook.shadow > 0.0f)
		{
			const size_t first = f.places.size() / 3;
			bool inside = true;
			double lo[3] = { 1e300, 1e300, 1e300 }, hi[3] = { -1e300, -1e300, -1e300 };
			for (size_t i = 0; i < corners.size() && inside; i++)
			{
				const double v[3] = { positions[i].x, positions[i].y, positions[i].z };
				double st[3];
				scenelight::PlaceToStage(camera, v, st);
				inside = st[0] * st[0] + st[2] * st[2] < 0.25 * SKY_DISTANCE * SKY_DISTANCE;
				for (int k = 0; k < 3; k++)
				{
					lo[k] = std::min(lo[k], st[k]);
					hi[k] = std::max(hi[k], st[k]);
				}
				f.places.insert(f.places.end(), { static_cast<float>(st[0]), static_cast<float>(st[1]), static_cast<float>(st[2]) });
				f.uv.insert(f.uv.end(), { corners[i].u, corners[i].v });
			}
			//a fighter is a few thousand units at most; larger is the stage's own (a page without an HD picture)
			inside = inside && std::max({ hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] }) < FIGHTER_SIZE;
			if (inside)
				f.casters.push_back({ texture, first, corners.size() });
			else
			{
				f.places.resize(first * 3);
				f.uv.resize(first * 2);
			}
		}
		//the game's uniforms and its client arrays as they are, DEMASTER's arrays for this one draw
		const GLint program = gameProgram;
		glUseProgram(f.program);
		if (f.vertextype >= 0)
			glUniform1i(f.vertextype, 3);
		const auto copyInt = [&](const GLint ours, const char* name)
		{
			if (ours < 0)
				return;
			const GLint theirs = glGetUniformLocation(static_cast<GLuint>(program), name);
			glUniform1i(ours, theirs >= 0 ? glstate::UniformInt(program, theirs) : 0);
		};
		copyInt(f.blendMode, "blend_mode");
		copyInt(f.effecttype, "effecttype");
		copyInt(f.effectoption, "effectoption");
		if (f.tileClamp >= 0)
			glUniform4f(f.tileClamp, 0.0f, 0.0f, 0.0f, 0.0f);
		draw[3] = battleLook.fighterSun;
		Send(static_cast<GLint>(f.program), f.state, scene, maps, draw, true);
		glstate::ClientState saved;
		glstate::SaveClient(saved);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		const uint8_t* base = reinterpret_cast<const uint8_t*>(f.vertices.data());
		glVertexPointer(4, GL_FLOAT, sizeof(FighterVertex), base + offsetof(FighterVertex, pos));
		glEnableClientState(GL_VERTEX_ARRAY);
		glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(FighterVertex), base + offsetof(FighterVertex, color));
		glEnableClientState(GL_COLOR_ARRAY);
		glNormalPointer(GL_FLOAT, sizeof(FighterVertex), base + offsetof(FighterVertex, normal));
		glEnableClientState(GL_NORMAL_ARRAY);
		glClientActiveTexture(GL_TEXTURE0);
		glTexCoordPointer(2, GL_FLOAT, sizeof(FighterVertex), base + offsetof(FighterVertex, uv));
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(f.vertices.size()));
		glstate::RestoreClient(saved);
		glUseProgram(static_cast<GLuint>(program));
		f.drawn++;
		return true;
	}

	bool MakeShadowMap()
	{
		ShadowMap& m = shadow;
		if (m.tried)
			return !m.failed;
		m.tried = true;
		m.failed = true;
		m.size = FIELD_PBR_QUALITY >= 2 ? 2048 : 1024;
		static const char* vertex = "#version 120\nvoid main() { gl_Position = gl_Vertex; gl_TexCoord[0] = gl_MultiTexCoord0; }\n";
		static const char* fragment = "#version 120\nuniform sampler2D tex;\nvoid main() { if (texture2D(tex, gl_TexCoord[0].st).a < 0.44) "
			"discard; gl_FragColor = vec4(1.0); }\n";
		const auto compile = [](const GLenum kind, const char* text) -> GLuint
		{
			const GLuint shader = glCreateShader(kind);
			glShaderSource(shader, 1, &text, nullptr);
			glCompileShader(shader);
			GLint ok = GL_FALSE;
			glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
			if (ok != GL_TRUE)
			{
				glDeleteShader(shader);
				return 0;
			}
			return shader;
		};
		const GLuint vs = compile(GL_VERTEX_SHADER, vertex), fs = compile(GL_FRAGMENT_SHADER, fragment);
		if (vs != 0 && fs != 0)
		{
			m.program = glCreateProgram();
			glAttachShader(m.program, vs);
			glAttachShader(m.program, fs);
			glLinkProgram(m.program);
			GLint ok = GL_FALSE;
			glGetProgramiv(m.program, GL_LINK_STATUS, &ok);
			if (ok != GL_TRUE)
			{
				glDeleteProgram(m.program);
				m.program = 0;
			}
		}
		if (vs != 0)
			glDeleteShader(vs);
		if (fs != 0)
			glDeleteShader(fs);
		if (m.program == 0)
		{
			OutputDebug("SCENE_LIGHT: the fighters' shadow map's shader doesn't compile here: no shadows in battle\n");
			return false;
		}
		const GLint previous = glstate::Program();
		glUseProgram(m.program);
		glUniform1i(glGetUniformLocation(m.program, "tex"), 0);
		glUseProgram(static_cast<GLuint>(previous));
		const GLenum active = glstate::ActiveTexture();
		const GLuint unit0 = glstate::Texture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE0);
		glGenTextures(1, &m.depth);
		glBindTexture(GL_TEXTURE_2D, m.depth);
		RawTexImage(GL_DEPTH_COMPONENT24, m.size, m.size, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
		glBindTexture(GL_TEXTURE_2D, unit0);
		glActiveTexture(active);
		const GLuint before = glstate::DrawFramebuffer();
		glGenFramebuffers(1, &m.framebuffer);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m.framebuffer);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m.depth, 0);
		glDrawBuffer(GL_NONE);
		const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, before);
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			OutputDebug("SCENE_LIGHT: the fighters' shadow map can't be drawn on this GPU (status 0x%X): no shadows in battle\n", status);
			return false;
		}
		m.failed = false;
		OutputDebug("SCENE_LIGHT: the fighters' shadows: a %d x %d map from the sun, a frame behind\n", m.size, m.size);
		return true;
	}

	//before the stage's first draw of a frame: last frame's fighters into the map
	void RenderShadowMap()
	{
		ShadowMap& m = shadow;
		if (m.frame == sceneFrame)
			return;
		m.frame = sceneFrame;
		m.on = false;
		const Fighters& f = fighters;
		if (battleLook.shadow <= 0.0f || stageLight.sunShadow <= 0.0f || f.lastCasters.empty() || !MakeShadowMap())
			return;
		float matrix[16];
		if (!scenelight::FitSunShadow(stageLight.sunStage, f.lastPlaces, 150.0, matrix))
			return;
		static std::vector<float> clip;
		clip.resize(f.lastPlaces.size());
		for (size_t i = 0; i + 2 < f.lastPlaces.size(); i += 3)
		{
			const float x = f.lastPlaces[i], y = f.lastPlaces[i + 1], z = f.lastPlaces[i + 2];
			for (int k = 0; k < 3; k++)
				clip[i + k] = (matrix[k] * x + matrix[4 + k] * y + matrix[8 + k] * z + matrix[12 + k]) * 2.0f - 1.0f;
		}
		glstate::RenderState state;
		glstate::Capture(state);
		glstate::ClientState client;
		glstate::SaveClient(client);
		const GLint program = glstate::Program();
		const GLenum active = glstate::ActiveTexture();
		const GLuint unit0 = glstate::Texture(GL_TEXTURE_2D, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m.framebuffer);
		glstate::RawViewport(0, 0, m.size, m.size);
		glDisable(GL_SCISSOR_TEST);
		glDisable(GL_BLEND);
		glDisable(GL_ALPHA_TEST);
		glDisable(GL_CULL_FACE);
		glDisable(GL_STENCIL_TEST);
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glDepthMask(GL_TRUE);
		glClearDepth(1.0);
		glClear(GL_DEPTH_BUFFER_BIT);
		glUseProgram(m.program);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glVertexPointer(3, GL_FLOAT, 0, clip.data());
		glEnableClientState(GL_VERTEX_ARRAY);
		glDisableClientState(GL_COLOR_ARRAY);
		glDisableClientState(GL_NORMAL_ARRAY);
		glClientActiveTexture(GL_TEXTURE0);
		glTexCoordPointer(2, GL_FLOAT, 0, f.lastUv.data());
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glActiveTexture(GL_TEXTURE0);
		for (const Caster& c : f.lastCasters)
		{
			glBindTexture(GL_TEXTURE_2D, c.texture);
			glDrawArrays(GL_TRIANGLES, static_cast<GLint>(c.first), static_cast<GLsizei>(c.count));
		}
		glBindTexture(GL_TEXTURE_2D, unit0);
		//the map on its unit for the stage's draws
		glActiveTexture(GL_TEXTURE0 + UNIT_SHADOW);
		glBindTexture(GL_TEXTURE_2D, m.depth);
		glActiveTexture(active);
		glUseProgram(static_cast<GLuint>(program));
		glstate::RestoreClient(client);
		glstate::Apply(state);
		memcpy(m.matrix, matrix, sizeof(matrix));
		m.info[0] = std::min(1.0f, stageLight.sunShadow);
		m.info[1] = 0.004f; //the depth's bias (of the map's whole depth)
		m.info[2] = 1.0f / static_cast<float>(m.size);
		m.info[3] = 1.0f;
		m.on = true;
		m.serial++;
	}

	//a frame is over: this frame's fighters are the next frame's shadow casters
	void FightersFrameEnd()
	{
		Fighters& f = fighters;
		std::swap(f.places, f.lastPlaces);
		std::swap(f.uv, f.lastUv);
		std::swap(f.casters, f.lastCasters);
		f.places.clear();
		f.uv.clear();
		f.casters.clear();
		if (SCENE_DEBUG && f.drawn > 0 && f.logged < 3 && currentMode == MODE_BATTLE)
		{
			f.logged++;
			OutputDebug("SCENE_LIGHT: %zu fighter draw(s) with smooth normals this frame, %zu shadow caster(s)%s\n", f.drawn, f.lastCasters.size(),
				shadow.on ? ", the shadow map on" : "");
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
		if (type == TYPE_BATTLE_STAGE || type == TYPE_WORLD || type == TYPE_WORLD_SKY || type == TYPE_BATTLE_CHARACTER)
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
		t.normal = PageMap(stem, stage, page, "_N");
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
		static std::string said;
		if ((t.glows != 0 || t.orm != 0 || t.normal != 0 || SCENE_DEBUG) && said != stem)
		{
			said = stem;
			OutputDebug("SCENE_LIGHT: stage %d page %d: %s%s%s%s\n", stage, page, t.normal != 0 ? "a normal map (_N)" : "no _N",
				t.glows != 0 ? ", a picture of its glows (_EC)" : ", no _EC", t.orm != 0 ? ", an ORM map" : ", no _ORM",
				t.glows == 0 && t.orm == 0 && t.normal == 0 ? " (next to its picture; the time of day and the stage's lights work without them)" : "");
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

//glDrawElements hook, before every draw of the game. true: DEMASTER drew it (a fighter with smooth normals)
bool SceneLightBeforeDraw(const GLenum drawMode, const int count, const GLenum indexType, const void* indices)
{
	if (!SCENE_LIGHT)
		return false;
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
		return false;
	const GLint program = glstate::Program();
	if (program == 0)
		return false;
	ProgramState& p = programs[program];
	if (!p.looked)
	{
		p.looked = true;
		if (GameProgramOf(program).kind == ProgramKind::Main)
		{
			LookUpUniforms(program, p);
			if (p.scene < 0)
				OutputDebug("SCENE_LIGHT: the game's main.frag (program %d) is not this release's: battles and the world map stay as they "
					"are. Copy DEMASTER_EXP\\shaders\\main.frag from this release.\n", program);
		}
		else
			p.scene = -1;
	}
	if (p.scene < 0)
		return false;
	float sceneValue[4] = { 0, 0, 0, 0 }, mapsValue[4] = { 0, 0, 0, 0 }, drawValue[4] = { 0, 0, 0, 0 };
	bool stageLit = false, knownFighter = false;
	if (scene)
	{
		//(a battle on the world map stands on the world's own textures)
		const auto known = textures.find(glstate::BoundTexture2D());
		const bool world = known != textures.end() && (known->second.type == TYPE_WORLD || known->second.type == TYPE_WORLD_SKY);
		bool lit = world || (known != textures.end() && mode == MODE_BATTLE
			&& (known->second.type == TYPE_BATTLE_STAGE || known->second.type == TYPE_BATTLE_CHARACTER));
		//(on the world map what is added is its haze and clouds: scenery, dimmed with the rest)
		if (lit && mode == MODE_BATTLE && glstate::BlendOn())
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
			if (mode == MODE_BATTLE)
			{
				//the party (35) and the enemies (11 without a stage page of DEMASTER's: a stage page without an HD picture comes
				//here too; it gets smooth normals, and casts no shadow when it is large)
				knownFighter = !world && t.stage < 0 && (t.type == TYPE_BATTLE_CHARACTER || t.type == TYPE_BATTLE_STAGE);
				WorkStageLight();
				stageLit = true;
				//the stage (its own pages, or the world's ground under a battle there) gets the sun and the fighters' shadows; the
				//fighters the lights and their share of the sun, with smooth normals
				drawValue[0] = t.stage >= 0 || world ? 1.0f : 0.0f;
				drawValue[1] = t.normal != 0 ? 1.0f : 0.0f;
				drawValue[2] = t.orm != 0 ? 1.0f : 0.0f;
				if (drawValue[0] > 0.5f)
					RenderShadowMap();
			}
			if (t.glows != 0 || t.orm != 0 || t.normal != 0)
			{
				//its maps on DEMASTER's units (the game uses none of them)
				const GLenum active = glstate::ActiveTexture();
				if (t.normal != 0 && glstate::Texture(GL_TEXTURE_2D, UNIT_NORMAL) != t.normal)
				{
					glActiveTexture(GL_TEXTURE0 + UNIT_NORMAL);
					glBindTexture(GL_TEXTURE_2D, t.normal);
				}
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
				mapsValue[1] = now.evening;
				mapsValue[2] = now.glow;
				mapsValue[3] = t.orm != 0 ? now.ao : 0.0f;
			}
			if (SCENE_DEBUG && mode == MODE_BATTLE && t.type == TYPE_BATTLE_STAGE)
				LogProjection(program, count);
		}
	}
	//a fighter: drawn by DEMASTER with smooth normals (its maps' units are set above, as for the game's draw)
	if (stageLit && drawValue[0] < 0.5f && knownFighter && DrawFighter(program, drawMode, count, indexType, indices, sceneValue, mapsValue, drawValue))
		return true;
	Send(program, p, sceneValue, mapsValue, drawValue, stageLit);
	return false;
}

//SwapBuffers hook: a frame is over
void SceneLightFrameEnd()
{
	sceneFrame++;
	if (SCENE_LIGHT)
		FightersFrameEnd();
	if (SCENE_LIGHT && currentMode != MODE_BATTLE)
		cameraFilter.Reset(); //the next battle starts with its own camera
}
