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
//Which draws:
//	battle     by the kind of texture bound (the game's own texture type, remembered when the texture is filled): the
//	           stage and the enemies (type 11), the party (35) and, in a battle on the world map, the world's ground and
//	           sky the stage is made of (18, 27); not draws that add light (blend ONE, ONE: spells, glows)
//	world map  by depth: whatever is drawn at a depth of its own (the ground, the sea, the sky, the haze, the people and
//	           vehicles: its HD textures come in without the game's type), not the map, the menus or the screen's flashes
//	           (rhw 1/3 or 1). Its plain-colour sky and haze (main_notex.frag, DEMASTER's version) too
//
//Battles and the world map are also lit from their own space (scene_light.h, "the battle camera", "the world map"): the sun
//and the moon cross them by the clock. In battle a stage page's normal map (<page>_N) and ORM map answer to them, and a
//stage can have four lights of its own (scene_light.txt) and four from the effects (EFFECT_LIGHTS). For that the shader
//gets, once a frame, the camera the game built (BATTLE_CAMERA, WORLD_CAMERA) and the numbers its projection used, and the
//sun and the lights in view space. On the world map the sky isn't lit by the sun, and which way is up in its space is
//measured from its ground in its first frames (MeasureWorldUp).
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

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

using namespace fieldrt;

namespace
{
	//(11: battle stages and the enemies; 18 and 27: the world map's ground, its sky and horizon; 35: the party in battle)
	constexpr int TYPE_BATTLE_STAGE = 11, TYPE_WORLD = 18, TYPE_WORLD_SKY = 27, TYPE_BATTLE_CHARACTER = 35;
	constexpr int TYPE_FIELD_BACKGROUND = 25, TYPE_FIELD_BACKGROUND_2 = 41; //(EFFECT_LIGHTS: a field's own glowing tiles are not effects)
	constexpr int UNIT_GLOWS = 10, UNIT_ORM = 11, UNIT_NORMAL = 12, UNIT_SHADOW = 13;
	constexpr double FIGHTER_SIZE = 4000.0; //a draw larger than this (the stage's units) casts no shadow: it is the stage's
	constexpr float SKY_DISTANCE = 20000.0f; //a stage's sky stands about 30,000 units from its middle; the fighters within a few thousand

	constexpr int WORLD_PAGE = -2; //SceneTexture::stage of a world map page DEMASTER replaced (texl_, wmset_)
	struct SceneTexture
	{
		int type = 0; //the game's texture type at its last upload
		int stage = -1, page = -1; //a battle stage page DEMASTER replaced (stage WORLD_PAGE: a world map page)
		GLuint glows = 0, orm = 0, normal = 0; //its maps (0: none); the textures belong to `maps` or `worldMaps` below
		bool additive = false; //glows is an emissive map without coverage (_E without alpha): its light is only added
		bool height = false; //orm is an ORMH map: its alpha is the height (1 the surface, 0 the deepest)
	};
	std::unordered_map<GLuint, SceneTexture> textures;
	bool loadingOwn = false; //DEMASTER is filling one of its own map textures: not the game's
	//a stage's map files, read once per battle stage (an animated page comes back every few frames); 0: no such file
	struct MapFile
	{
		GLuint texture = 0;
		bool alpha = false; //the file has an alpha channel
	};
	std::unordered_map<std::string, MapFile> maps;
	int mapsStage = -1;
	//the world map's (kept while the game runs: its pages are filled again every time the world map comes back)
	std::unordered_map<std::string, MapFile> worldMaps;

	struct ProgramState
	{
		bool looked = false;
		GLint scene = -1, maps = -1, draw = -1, proj = -1, centre = -1, rot = -1, trans = -1, sun = -1, sunLight = -1, lightAt = -1,
			lightColour = -1, shadowMatrix = -1, shadowInfo = -1, surface = -1;
		uint64_t shadowSent = 0; //the serial of the shadow map this program has (0: none)
		bool plain = false; //main_notex.frag: only demasterScene
		float sceneValue[4] = { 0, 0, 0, 0 }, mapsValue[4] = { 0, 0, 0, 0 }, drawValue[4] = { 0, 0, 0, 0 }, surfaceValue[4] = { 0, 0, 0, 0 }; //as last sent
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
			sunLight[4] = { 0, 0, 0, 0 }, lightAt[32] = {}, lightColour[32] = {}; //0-3 the stage's own, 4-7 the effects'
	} stageLight;

	//--- the effects' light (EFFECT_LIGHTS): the frame's glowing draws are kept (their corners, texture and how they add),
	//drawn again at the frame's end onto a 40 x 30 grid of the screen (two float targets: the light, and its brightness
	//with brightness x depth), read back without waiting (the next frame's end picks it up), and the brightest few become
	//lights of the stage (scene_light.h, FindGlows) for the frames after
	constexpr int GLOW_WIDTH = 40, GLOW_HEIGHT = 30;
	constexpr size_t GLOW_MOST_CORNERS = 12000; //a frame's glowing corners kept at most (the rest: not gathered)
	struct GlowVertex
	{
		float pos[4];
		uint8_t color[4];
		float uv[2];
	};
	struct GlowRun
	{
		GLuint texture = 0;
		size_t first = 0, count = 0;
		float alpha = 0, share = 1; //alpha 1: the light is times the texture's alpha (blend SRC_ALPHA, ONE); share: of it
	};
	struct Glows
	{
		bool tried = false, failed = false;
		GLuint program = 0, framebuffer = 0, targets[2] = { 0, 0 }, pixels[2] = { 0, 0 }, timer[2] = { 0, 0 };
		GLint alphaAt = -1, shareAt = -1;
		std::vector<GlowVertex> vertices;
		std::vector<GlowRun> runs;
		bool waiting[2] = { false, false }, timing[2] = { false, false }; //a read back (a GPU time) is on its way in that slot
		scenelight::Camera camera[2]; //the camera of the frame each slot was gathered in (battles)
		fieldchars::Projection projection[2]; //the field's screen to its room (PBR fields)
		int mode[2] = { -1, -1 }; //where each slot was gathered
		uint64_t frame = 0;
		scenelight::EffectLights lights; //battles: the stage's space
		scenelight::EffectLights fieldLights; //PBR fields: the room's view space
		fieldrt::EffectGlows fieldOut;
		//what it costs (SCENE_DEBUG, every 10 s of battle)
		long long cpu = 0; //QueryPerformanceCounter ticks, keeping and gathering
		uint64_t draws = 0, corners = 0, frames = 0, lit = 0, timed = 0;
		double gpu = 0; //ms, the gathering pass
		ULONGLONG statsFrom = 0;
	} glows;
	uint64_t sceneFrame = 0; //counts SwapBuffers
	scenelight::CameraFilter cameraFilter;
	scenelight::Look battleLook; //this moment's, for the stage's light
	scenelight::Look worldLook; //the world map's, for its sun
	//the world map's up (scenelight::FacingAlongUp): measured from its first frames; +1: -y of its space (the PlayStation's),
	//-1: +y. Until it is measured: +1
	struct WorldUp
	{
		double facing[3] = { 0, 0, 0 };
		int triangles = 0, draws = 0;
		int sign = 1;
		bool decided = false;
	} worldUp;
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
		scenelight::Time timeHere = scenelight::Time::Cycle; //for the colour grade (LUT)
	} now;

	//a battle stage's maps go (not the world map's)
	void ForgetMaps()
	{
		for (auto& [path, file] : maps)
			if (file.texture != 0)
				glDeleteTextures(1, &file.texture);
		maps.clear();
		for (auto& [id, t] : textures)
			if (t.stage != WORLD_PAGE)
			{
				t.glows = t.orm = t.normal = 0;
				t.additive = t.height = false;
			}
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
		now.timeHere = scenelight::TimeHere(look, fixedPlace, outdoors);
		if (battle)
			battleLook = look;
		else
			worldLook = look;
	}

	//the world map's party and the point its camera follows (WORLD_PLAYER, WORLD_TARGET: x, z, height as int32), as x,
	//height, z
	bool ReadWorldPlace(const ADDRESS_NAME name, double out[3])
	{
		const DWORD at = HasAddress(name) ? GetAddress(name) : 0;
		int32_t v[3]{};
		if (at == 0 || !GuestRead(v, at, sizeof(v)))
			return false;
		out[0] = v[0];
		out[1] = v[2];
		out[2] = v[1];
		return true;
	}

	struct WorldLightCount
	{
		ULONGLONG said = 0;
		bool warned = false;
	} worldLightCount;

	void WorldLights(const scenelight::Camera& camera, StageLight& next)
	{
		const std::vector<scenelight::Light>& lights = worldLook.lights;
		double target[3];
		const bool known = ReadWorldPlace(WORLD_TARGET, target);
		if (!known)
		{
			if (!lights.empty() && !worldLightCount.warned)
			{
				worldLightCount.warned = true;
				OutputDebug("SCENE_LIGHT: the world map's lights: the camera's place (WORLD_TARGET) can't be read in this game: not lit\n");
			}
			return;
		}
		const double origin[3] = { 0, 0, 0 };
		double eye[3];
		scenelight::PlaceToStage(camera, origin, eye);
		const std::vector<size_t> lit = scenelight::NearestLights(lights, target, eye, 8);
		size_t slot = 0;
		for (const size_t i : lit)
		{
			const scenelight::Light& light = lights[i];
			const float on = light.when == 1 ? now.night : light.when == 2 ? 1.0f - now.evening : 1.0f;
			if (on <= 0.0f)
				continue;
			const double place[3] = { light.x, light.up, light.z };
			double stage[3], view[3];
			scenelight::WorldToStage(target, place, stage);
			scenelight::PlaceToView(camera, stage, view);
			for (int k = 0; k < 3; k++)
			{
				next.lightAt[slot * 4 + k] = static_cast<float>(view[k]);
				next.lightColour[slot * 4 + k] = light.colour[k] * on;
			}
			next.lightAt[slot * 4 + 3] = light.range;
			slot++;
		}
		//every 10 s (SCENE_DEBUG): where the party is, in the game's numbers and on the screen (to check the places' sums
		//against the picture), and what is lit
		const ULONGLONG tick = GetTickCount64();
		if (SCENE_DEBUG && tick - worldLightCount.said >= 10000)
		{
			worldLightCount.said = tick;
			double party[3], stage[3], view[3];
			if (ReadWorldPlace(WORLD_PLAYER, party))
			{
				scenelight::WorldToStage(target, party, stage);
				scenelight::PlaceToView(camera, stage, view);
				const double sx = view[2] > 1 ? camera.centreX + camera.distance * view[0] / view[2] : -1;
				const double sy = view[2] > 1 ? camera.centreY + camera.distance * view[1] / view[2] : -1;
				OutputDebug("SCENE_LIGHT: the world map: the party at x %.0f z %.0f height %.0f (the camera follows x %.0f z %.0f), %.0f in "
					"front of the camera, on the screen at %.0f %.0f of 320 x 240 (it should be where the party is drawn); %zu of %zu world "
					"light(s) lit now\n", party[0], party[2], party[1], target[0], target[2], view[2], sx, sy, slot, lights.size());
			}
		}
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
		const bool registersRead = at != 0 && GuestRead(registers, at, sizeof(registers));
		const bool readable = registersRead && scenelight::CameraFromRegisters(registers, read);
		//in battle the camera the game built for this frame (BATTLE_CAMERA): every pan and cut of the spells and the GFs as
		//it is, whatever model's matrix the registers hold. The distance and the screen's centre are the registers' (they
		//don't change in a battle). On the world map (and if it can't be read) the registers, through the filter
		bool own = false;
		const bool world = currentMode == MODE_WORLDMAP;
		const scenelight::Look& look = world ? worldLook : battleLook;
		if (world && registersRead)
		{
			//the world map's camera (WORLD_CAMERA), with the registers' distance and centre; its picture starts 16 lines down
			static const DWORD worldAt = HasAddress(WORLD_CAMERA) ? GetAddress(WORLD_CAMERA) : 0;
			uint8_t matrix[32];
			scenelight::Camera projection;
			projection.centreX = registers[24] / 65536.0;
			projection.centreY = registers[25] / 65536.0 + scenelight::WORLD_CENTRE_DOWN;
			projection.distance = static_cast<double>(registers[26] & 0xFFFF);
			own = worldAt != 0 && GuestRead(matrix, worldAt, sizeof(matrix)) && scenelight::CameraFromMatrix(matrix, projection, camera);
			static int said = 0;
			if (SCENE_DEBUG && said < 3)
			{
				said++;
				OutputDebug("SCENE_LIGHT: the world map's camera (WORLD_CAMERA) %s: rotation %.3f %.3f %.3f / %.3f %.3f %.3f / %.3f %.3f %.3f, "
					"translation %.0f %.0f %.0f, distance %.0f, centre %.1f %.1f\n", own ? "read" : "can't be read", camera.r[0], camera.r[1],
					camera.r[2], camera.r[3], camera.r[4], camera.r[5], camera.r[6], camera.r[7], camera.r[8], camera.t[0], camera.t[1], camera.t[2],
					projection.distance, projection.centreX, projection.centreY);
			}
		}
		if (currentMode == MODE_BATTLE && registersRead)
		{
			static const DWORD matrixAt = HasAddress(BATTLE_CAMERA) ? GetAddress(BATTLE_CAMERA) : 0;
			uint8_t matrix[32];
			scenelight::Camera projection;
			projection.centreX = registers[24] / 65536.0;
			projection.centreY = registers[25] / 65536.0;
			projection.distance = static_cast<double>(registers[26] & 0xFFFF);
			own = matrixAt != 0 && GuestRead(matrix, matrixAt, sizeof(matrix)) && scenelight::CameraFromMatrix(matrix, projection, camera);
			if (own)
			{
				cameraFilter.Reset();
				static int compared = 0;
				if (SCENE_DEBUG && readable && compared < 6)
				{
					compared++;
					double rotation = 0, translation = 0;
					scenelight::CameraDifference(camera, read, rotation, translation);
					OutputDebug("SCENE_LIGHT: the battle camera from the game's own (BATTLE_CAMERA) %s the registers (rotation %.4f apart, "
						"translation %.0f)\n", rotation < 0.01 && translation < 2 ? "agrees with" : "differs from (a model's matrix in them)",
						rotation, translation);
				}
			}
		}
		GLint viewport[4] = { 0, 0, 0, 0 };
		glGetIntegerv(GL_VIEWPORT, viewport);
		//(the world map: only its own camera; its registers hold the models' matrices as often as not)
		if ((own || (!world && cameraFilter.Frame(readable, read, camera))) && viewport[2] > 0 && viewport[3] > 0)
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
			next.centre[2] = look.specular;
			next.centre[3] = world ? 1e9f : SKY_DISTANCE; //(the world map's sky is told by its depth, draw by draw)
			for (int i = 0; i < 9; i++)
				next.rot[i] = static_cast<float>(camera.r[i]); //rows sent as columns: the transpose, view to stage
			for (int i = 0; i < 3; i++)
				next.trans[i] = static_cast<float>(camera.t[i]);
			//where the night doesn't come (or the time is fixed) the sun, or the moon, stands at its highest
			fieldpbr::SkyTime sky;
			sky.night = look.time == scenelight::Time::Night;
			if (now.sunByClock)
				sky = DayNightSky();
			scenelight::Sun sun = scenelight::SunOver(look, sky.night, sky.phase);
			if (world && worldUp.sign < 0)
				sun.direction[1] = -sun.direction[1]; //the world map's up is +y of its space
			if (world)
				sun.strength *= worldUp.decided ? 1.0 : 0.0; //no sun until up is known
			double towards[3];
			scenelight::DirectionToView(camera, sun.direction, towards);
			const double strength = sun.strength;
			for (int i = 0; i < 3; i++)
				next.sunStage[i] = sun.direction[i];
			next.sunShadow = world ? 0.0f : look.shadow * static_cast<float>(strength);
			for (int i = 0; i < 3; i++)
			{
				next.sun[i] = static_cast<float>(towards[i]);
				next.sunLight[i] = sun.colour[i] * static_cast<float>(strength);
			}
			next.sun[3] = look.sun * static_cast<float>(strength);
			next.sunLight[3] = look.relief * static_cast<float>(strength);
			size_t count = 0;
			static const std::vector<scenelight::Light> noLights;
			for (const scenelight::Light& light : world ? noLights : battleLook.lights)
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
			//the world map's own (scene_light.txt, its places the game's: WORLD_TARGET's cell): the eight that reach nearest
			//the camera, in all the slots
			if (world)
				WorldLights(camera, next);
			//the effects' (EFFECT_LIGHTS), in slots 4-7 (in battle)
			size_t slot = scenelight::MAX_LIGHTS;
			static const std::vector<scenelight::EffectLight> noEffects;
			for (const scenelight::EffectLight& light : world ? noEffects : glows.lights.Lights())
			{
				if (slot >= scenelight::MAX_LIGHTS + scenelight::MAX_EFFECT_LIGHTS)
					break;
				double view[3];
				scenelight::PlaceToView(camera, light.stage, view);
				if (view[2] <= 1.0)
					continue;
				for (int i = 0; i < 3; i++)
				{
					next.lightAt[slot * 4 + i] = static_cast<float>(view[i]);
					next.lightColour[slot * 4 + i] = light.colour[i];
				}
				next.lightAt[slot * 4 + 3] = light.range;
				slot++;
			}
		}
		if (memcmp(next.proj, stageLight.proj, sizeof(float) * (4 + 4 + 9 + 3 + 4 + 4 + 32 + 32)) != 0 || next.camera != stageLight.camera)
			next.serial = stageLight.serial + 1;
		stageLight = next;
	}

	//The world map's up, from its ground: the facing of the opaque draws of its first frames, in its own space, summed. The
	//terrain faces up on the whole, so the sum says whether up is -y of its space (the PlayStation's way) or +y
	void MeasureWorldUp(const GLenum drawMode, const int count, const GLenum indexType, const void* indices)
	{
		WorldUp& w = worldUp;
		if (w.decided || drawMode != GL_TRIANGLES || count < 3 || count % 3 != 0 || count > 3000)
			return;
		static std::vector<fieldchars::GameVertex> corners;
		if (!ReadDrawCorners(count, indexType, indices, corners) || corners.size() != static_cast<size_t>(count))
			return;
		static std::vector<double> screen;
		screen.clear();
		for (const fieldchars::GameVertex& c : corners)
			screen.insert(screen.end(), { static_cast<double>(c.x), static_cast<double>(c.y), static_cast<double>(c.rhw) });
		double view[3] = { 0, 0, 0 };
		scenelight::AddFacing(stageLight.view, screen, view);
		//into the world's own space (the camera turns from frame to frame): the rotation's transpose
		const scenelight::Camera& c = stageLight.view;
		for (int i = 0; i < 3; i++)
			w.facing[i] += c.r[i] * view[0] + c.r[3 + i] * view[1] + c.r[6 + i] * view[2];
		w.triangles += count / 3;
		w.draws++;
		if (w.triangles < 600 || w.draws < 30)
			return;
		const double length = std::sqrt(w.facing[0] * w.facing[0] + w.facing[1] * w.facing[1] + w.facing[2] * w.facing[2]);
		const double along = length > 0 ? -w.facing[1] / length : 0.0; //along -y
		w.decided = true;
		w.sign = along < -0.5 ? -1 : 1;
		OutputDebug("SCENE_LIGHT: the world map's up, from its ground (%d triangles): %s of its space (the ground faces %.2f along it)%s\n",
			w.triangles, w.sign > 0 ? "-y" : "+y", std::fabs(along), std::fabs(along) < 0.5 ? "; not clear: -y taken" : "");
	}

	//one of a page's maps in a texture of DEMASTER's own (texture 0: no such file, or it can't be read). With every mip level
	//(the file's, else made here), so the world's ground far off doesn't sparkle
	MapFile LoadMap(const std::string& stem, const char* suffix, std::unordered_map<std::string, MapFile>& cache = maps)
	{
		const std::string key = stem + suffix;
		if (const auto known = cache.find(key); known != cache.end())
			return known->second;
		MapFile& made = cache[key];
		char path[MAX_PATH]{ 0 };
		if (DDSorPNG(path, MAX_PATH, "%s%s", stem.c_str(), suffix))
			return made;
		const SafeBimg image = LoadImageFromFile(path);
		if (!image)
			return made;
		GLuint texture = 0;
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		loadingOwn = true;
		RenderTexture(image.get());
		loadingOwn = false;
		const bool levels = image->m_numMips > 1;
		if (!levels && !bimg::isCompressed(image->m_format))
			glGenerateMipmap(GL_TEXTURE_2D);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, levels || !bimg::isCompressed(image->m_format) ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		made.texture = texture;
		made.alpha = image->m_hasAlpha;
		return made;
	}

	//a page's map: the one of this frame of an animated page (a0stg012_16_3_EC), else the page's (a0stg012_16_EC)
	MapFile PageMap(const std::string& stem, const int stage, const int page, const char* suffix)
	{
		if (const MapFile own = LoadMap(stem, suffix); own.texture != 0)
			return own;
		char name[64];
		snprintf(name, sizeof(name), "a0stg%03d_%d", stage, page);
		const size_t slash = stem.find_last_of("\\/");
		const std::string plain = (slash == std::string::npos ? std::string() : stem.substr(0, slash + 1)) + name;
		return plain == stem ? MapFile() : LoadMap(plain, suffix);
	}

	//a world page's map: its own (texl_003_2_N), else its first palette's (texl_003_0_N: the same ground in other colours)
	MapFile WorldMap(const std::string& stem, const char* suffix)
	{
		if (const MapFile own = LoadMap(stem, suffix, worldMaps); own.texture != 0)
			return own;
		const size_t underscore = stem.find_last_of('_');
		if (underscore == std::string::npos || underscore + 1 >= stem.size() || stem.substr(underscore + 1) == "0")
			return MapFile();
		return LoadMap(stem.substr(0, underscore + 1) + "0", suffix, worldMaps);
	}

	//SCENE_DEBUG: what a later build needs to light stages from a sun that stands still in the world: the projection's
	//registers (the game keeps the PlayStation's: rotation, translation, screen centre, distance) while a stage is drawn,
	//and the first corner of the draw. Once per battle.
	//world: a draw of the world map's own textures (the first step of its light: is the projection the same as in battle?)
	void LogProjection(const GLint program, const int count, const bool world = false)
	{
		static int loggedStage = -2;
		static ULONGLONG loggedAt = 0;
		const ULONGLONG tick = GetTickCount64();
		const int key = world ? -100 : currentStage;
		if (loggedStage == key && tick - loggedAt < 20000)
			return;
		loggedStage = key;
		loggedAt = tick;
		if (world)
			OutputDebug("SCENE_LIGHT: the world map (the registers and the first corner of one of its draws follow):\n");
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

	//surface: demasterSceneSurface (the parallax's depth); nullptr: none
	void Send(const GLint program, ProgramState& p, const float scene[4], const float maps[4], const float draw[4], const bool stage,
		const float* surface = nullptr)
	{
		if (p.draw >= 0 && memcmp(p.drawValue, draw, sizeof(p.drawValue)) != 0)
		{
			memcpy(p.drawValue, draw, sizeof(p.drawValue));
			glUniform4fv(p.draw, 1, draw);
		}
		static const float flat[4] = { 0, 0, 0, 0 };
		if (surface == nullptr)
			surface = flat;
		if (p.surface >= 0 && memcmp(p.surfaceValue, surface, sizeof(p.surfaceValue)) != 0)
		{
			memcpy(p.surfaceValue, surface, sizeof(p.surfaceValue));
			glUniform4fv(p.surface, 1, surface);
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
				glUniform4fv(p.lightAt, 8, stageLight.lightAt);
				glUniform4fv(p.lightColour, 8, stageLight.lightColour);
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
		p.surface = at("demasterSceneSurface");
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
		LutUse(f.program, true); //LUT: the time of day's colour grade
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

	//--- the world map's ground, drawn by DEMASTER with smooth normals (scenelight::GroundNormals: the faces around each
	//corner, kept from frame to frame by place in the world's space) through the fighters' program: the game's corners,
	//texture and state as they are, the normals in view space with them
	scenelight::GroundNormals ground;
	struct GroundCount
	{
		uint64_t frame = ~0ull;
		size_t draws = 0, corners = 0, found = 0, missed = 0, frames = 0;
		ULONGLONG from = 0;
		bool said = false;
	} groundCount;

	//this draw writes its colour as it is (no blending, or blending that changes nothing)
	bool Opaque()
	{
		if (!glstate::BlendOn())
			return true;
		GLint source = 0, destination = 0;
		glstate::BlendFactors(source, destination);
		return source == GL_ONE && destination == GL_ZERO;
	}

	//true when DEMASTER drew it (the game's draw is skipped)
	bool DrawGround(const GLint gameProgram, const GLenum mode, const GLsizei count, const GLenum type, const void* indices,
		const float scene[4], const float maps[4], float draw[4], const float surface[4])
	{
		Fighters& f = fighters;
		if (mode != GL_TRIANGLES || count < 3 || count % 3 != 0 || !stageLight.camera || !MakeFighterProgram())
			return false;
		static std::vector<fieldchars::GameVertex> corners;
		if (!ReadDrawCorners(count, type, indices, corners) || corners.size() != static_cast<size_t>(count))
			return false;
		const scenelight::Camera& camera = stageLight.view;
		if (groundCount.frame != sceneFrame)
		{
			//a new frame: the camera's place in the world's space (the store follows the game's cells with it)
			groundCount.frame = sceneFrame;
			if (ground.Begun())
			{
				groundCount.found += ground.found;
				groundCount.missed += ground.missed;
			}
			const double origin[3] = { 0, 0, 0 };
			double eye[3];
			scenelight::PlaceToStage(camera, origin, eye);
			ground.BeginFrame(eye);
			groundCount.frames++;
		}
		static std::vector<double> places;
		static std::vector<float> normals;
		places.resize(corners.size() * 3);
		for (size_t i = 0; i < corners.size(); i++)
		{
			double v[3];
			scenelight::ScreenToView(camera, corners[i].x, corners[i].y, corners[i].rhw, v);
			scenelight::PlaceToStage(camera, v, &places[i * 3]);
		}
		ground.Smooth(places, 60.0f, normals);
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
			const double world[3] = { normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2] };
			double view[3];
			scenelight::DirectionToView(camera, world, view);
			v.normal[0] = static_cast<float>(view[0]);
			v.normal[1] = static_cast<float>(view[1]);
			v.normal[2] = static_cast<float>(view[2]);
		}
		//the game's uniforms and its client arrays as they are, DEMASTER's arrays for this one draw
		const GLint program = gameProgram;
		glUseProgram(f.program);
		LutUse(f.program, true); //LUT: the time of day's colour grade
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
		draw[3] = 0.0f; //(not a fighter's share of the sun: the ground's own normal takes the sun)
		Send(static_cast<GLint>(f.program), f.state, scene, maps, draw, true, surface);
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
		groundCount.draws++;
		groundCount.corners += corners.size();
		if (!groundCount.said)
		{
			groundCount.said = true;
			OutputDebug("SCENE_LIGHT: the world map's ground is drawn by DEMASTER with smooth normals (smooth = 0 in [world] of "
				"scene_light.txt: each triangle flat, as the game draws it)\n");
		}
		//what it does, every 10 s (SCENE_DEBUG)
		const ULONGLONG tick = GetTickCount64();
		if (groundCount.from == 0)
			groundCount.from = tick;
		else if (SCENE_DEBUG && tick - groundCount.from >= 10000 && groundCount.frames > 1)
		{
			const double frames = static_cast<double>(groundCount.frames);
			const size_t looked = groundCount.found + groundCount.missed;
			OutputDebug("SCENE_LIGHT: the world's ground, the last %.0f s: %.1f smoothed draws a frame (%.0f corners), %.1f%% of the corners "
				"found from the frame before, %zu kept\n", (tick - groundCount.from) / 1000.0, groundCount.draws / frames,
				groundCount.corners / frames, looked > 0 ? 100.0 * groundCount.found / looked : 0.0, ground.Kept());
			groundCount.from = tick;
			groundCount.draws = groundCount.corners = groundCount.found = groundCount.missed = groundCount.frames = 0;
		}
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

	//--- the effects' light (EFFECT_LIGHTS)
	long long Ticks()
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return t.QuadPart;
	}

	bool MakeGlows()
	{
		Glows& g = glows;
		if (g.tried)
			return !g.failed;
		g.tried = true;
		g.failed = true;
		//the game's corners straight onto the grid (scene_light.h: GLOW_VERTEX_SHADER, GLOW_FRAGMENT_SHADER)
		const char* const vertex = scenelight::GLOW_VERTEX_SHADER;
		const char* const fragment = scenelight::GLOW_FRAGMENT_SHADER;
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
			g.program = glCreateProgram();
			glAttachShader(g.program, vs);
			glAttachShader(g.program, fs);
			glLinkProgram(g.program);
			GLint ok = GL_FALSE;
			glGetProgramiv(g.program, GL_LINK_STATUS, &ok);
			if (ok != GL_TRUE)
			{
				glDeleteProgram(g.program);
				g.program = 0;
			}
		}
		if (vs != 0)
			glDeleteShader(vs);
		if (fs != 0)
			glDeleteShader(fs);
		if (g.program == 0)
		{
			OutputDebug("EFFECT_LIGHTS: the gathering shader doesn't compile here: spells don't light the stage\n");
			return false;
		}
		g.alphaAt = glGetUniformLocation(g.program, "demasterGlowAlpha");
		g.shareAt = glGetUniformLocation(g.program, "demasterGlowShare");
		const GLint previous = glstate::Program();
		glUseProgram(g.program);
		glUniform1i(glGetUniformLocation(g.program, "tex"), 0);
		glUseProgram(static_cast<GLuint>(previous));
		const GLenum active = glstate::ActiveTexture();
		const GLuint unit0 = glstate::Texture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE0);
		glGenTextures(2, g.targets);
		for (const GLuint t : g.targets)
		{
			glBindTexture(GL_TEXTURE_2D, t);
			RawTexImage(GL_RGBA16F, GLOW_WIDTH, GLOW_HEIGHT, GL_RGBA, GL_FLOAT, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		}
		glBindTexture(GL_TEXTURE_2D, unit0);
		glActiveTexture(active);
		const GLuint drawBefore = glstate::DrawFramebuffer(), readBefore = glstate::ReadFramebuffer();
		glGenFramebuffers(1, &g.framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, g.framebuffer);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.targets[0], 0);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, g.targets[1], 0);
		static const GLenum both[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
		glDrawBuffers(2, both);
		const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawBefore);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, readBefore);
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			OutputDebug("EFFECT_LIGHTS: the gathering targets can't be drawn into here (0x%x): spells don't light the stage\n", status);
			return false;
		}
		glGenBuffers(2, g.pixels);
		for (const GLuint b : g.pixels)
		{
			glBindBuffer(GL_PIXEL_PACK_BUFFER, b);
			glBufferData(GL_PIXEL_PACK_BUFFER, GLOW_WIDTH * GLOW_HEIGHT * 4 * sizeof(float) * 2, nullptr, GL_STREAM_READ);
		}
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		if (SCENE_DEBUG && glGenQueries != nullptr)
			glGenQueries(2, g.timer);
		g.failed = false;
		OutputDebug("EFFECT_LIGHTS: spells and other glowing effects light the stage and the fighters (a %d x %d grid, up to %zu lights)\n",
			GLOW_WIDTH, GLOW_HEIGHT, scenelight::MAX_EFFECT_LIGHTS);
		return true;
	}

	//the PBR field's screen to its room, as the character module has it
	fieldchars::Projection FieldProjection()
	{
		const FieldView& v = PbrView();
		fieldchars::Projection p;
		if (v.settings != nullptr)
		{
			p.space = v.settings->space;
			p.gameDepth = v.settings->gameDepth > 0 ? v.settings->gameDepth : fieldpbr::GAME_DEPTH_PER_VIEW;
		}
		p.offsetX = v.offsetKnown ? v.offsetX : 0.0;
		p.offsetY = v.offsetKnown ? v.offsetY : 0.0;
		p.screenPerCanvas = 2.0;
		return p;
	}

	//a field's glow as a simple light of the room: where it is (its place on the screen and its depth, back into the room),
	//its colour as in battle, reaching three times as far as it spreads (60-320 of the field's own pixels)
	bool FieldGlowToLight(const fieldchars::Projection& projection, const scenelight::Glow& glow, const double strength,
		scenelight::EffectLight& light)
	{
		const fieldchars::V3 v = projection.ToView(glow.x, glow.y, glow.depth > 0 ? 1.0 / glow.depth : 0.0);
		if (!(v.z > 0.0f) || projection.space.zoom <= 0)
			return false;
		scenelight::Camera none;
		light = scenelight::GlowToLight(none, glow, strength); //(its colour; the place and range are the field's, below)
		light.stage[0] = v.x;
		light.stage[1] = v.y;
		light.stage[2] = v.z;
		const double spread = glow.spread / projection.screenPerCanvas;
		light.range = static_cast<float>(std::clamp(3.0 * spread, 60.0, 320.0) * v.z / projection.space.zoom);
		return true;
	}

	//a draw of the game that adds light, in battle or a PBR field: its corners kept for the frame's end
	void KeepGlow(const GLint program, const GLenum mode, const GLsizei count, const GLenum type, const void* indices)
	{
		Glows& g = glows;
		if (mode != GL_TRIANGLES || count < 3 || g.failed || g.vertices.size() + static_cast<size_t>(count) > GLOW_MOST_CORNERS)
			return;
		const long long started = Ticks();
		static std::vector<fieldchars::GameVertex> corners;
		if (!ReadDrawCorners(count, type, indices, corners) || corners.size() != static_cast<size_t>(count))
			return;
		//the menus and the screen's flashes have no depth of their own (rhw 1): not in the stage
		double rhw = 0;
		for (const fieldchars::GameVertex& c : corners)
			rhw += c.rhw;
		rhw /= static_cast<double>(corners.size());
		if (!(rhw > 0.0) || rhw > 0.05)
		{
			g.cpu += Ticks() - started;
			return;
		}
		GLint source = 0, destination = 0;
		glstate::BlendFactors(source, destination);
		const GameProgram& game = GameProgramOf(program);
		const int blendMode = game.blendMode >= 0 ? glstate::UniformInt(program, game.blendMode) : 0;
		GlowRun run;
		run.texture = glstate::BoundTexture2D();
		run.first = g.vertices.size();
		run.count = corners.size();
		run.alpha = source == GL_SRC_ALPHA ? 1.0f : 0.0f;
		run.share = blendMode == 3 ? 0.25f : 1.0f; //the game's BLEND_25P
		for (const fieldchars::GameVertex& c : corners)
		{
			GlowVertex v;
			v.pos[0] = c.x;
			v.pos[1] = c.y;
			v.pos[2] = 0.0f;
			v.pos[3] = c.rhw;
			memcpy(v.color, c.color, 4);
			v.uv[0] = c.u;
			v.uv[1] = c.v;
			g.vertices.push_back(v);
		}
		if (!g.runs.empty() && g.runs.back().texture == run.texture && g.runs.back().alpha == run.alpha && g.runs.back().share == run.share
			&& g.runs.back().first + g.runs.back().count == run.first)
			g.runs.back().count += run.count;
		else
			g.runs.push_back(run);
		g.draws++;
		g.corners += corners.size();
		g.cpu += Ticks() - started;
	}

	//this frame's glowing draws onto the grid, and its read back started (picked up at the next frame's end)
	void GatherGlows(const int slot)
	{
		Glows& g = glows;
		glstate::RenderState game;
		glstate::Capture(game);
		glstate::ClientState client;
		glstate::SaveClient(client);
		float clear[4];
		double clearDepth = 1.0;
		glstate::ClearValues(clear, clearDepth);
		const GLint program = glstate::Program();
		const GLenum active = glstate::ActiveTexture();
		const GLuint unit0 = glstate::Texture(GL_TEXTURE_2D, 0);
		const bool timing = g.timer[slot] != 0 && !g.timing[slot];
		if (timing)
			glBeginQuery(GL_TIME_ELAPSED, g.timer[slot]);
		glBindFramebuffer(GL_FRAMEBUFFER, g.framebuffer);
		glstate::RawViewport(0, 0, GLOW_WIDTH, GLOW_HEIGHT);
		glstate::Enable(GL_SCISSOR_TEST, false);
		glstate::Enable(GL_DEPTH_TEST, false);
		glstate::Enable(GL_ALPHA_TEST, false);
		glstate::Enable(GL_CULL_FACE, false);
		glstate::Enable(GL_STENCIL_TEST, false);
		glstate::Enable(GL_BLEND, true);
		glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ONE, GL_ONE);
		glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		glUseProgram(g.program);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		const uint8_t* base = reinterpret_cast<const uint8_t*>(g.vertices.data());
		glVertexPointer(4, GL_FLOAT, sizeof(GlowVertex), base + offsetof(GlowVertex, pos));
		glEnableClientState(GL_VERTEX_ARRAY);
		glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(GlowVertex), base + offsetof(GlowVertex, color));
		glEnableClientState(GL_COLOR_ARRAY);
		glDisableClientState(GL_NORMAL_ARRAY);
		glClientActiveTexture(GL_TEXTURE0);
		glTexCoordPointer(2, GL_FLOAT, sizeof(GlowVertex), base + offsetof(GlowVertex, uv));
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glActiveTexture(GL_TEXTURE0);
		for (const GlowRun& run : g.runs)
		{
			glBindTexture(GL_TEXTURE_2D, run.texture);
			glUniform1f(g.alphaAt, run.alpha);
			glUniform1f(g.shareAt, run.share);
			glDrawArrays(GL_TRIANGLES, static_cast<GLint>(run.first), static_cast<GLsizei>(run.count));
		}
		//the read back, into a buffer: the driver copies it when the drawing is done, nothing waits for it here
		glBindBuffer(GL_PIXEL_PACK_BUFFER, g.pixels[slot]);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glReadPixels(0, 0, GLOW_WIDTH, GLOW_HEIGHT, GL_RGBA, GL_FLOAT, nullptr);
		glReadBuffer(GL_COLOR_ATTACHMENT1);
		glReadPixels(0, 0, GLOW_WIDTH, GLOW_HEIGHT, GL_RGBA, GL_FLOAT, reinterpret_cast<void*>(GLOW_WIDTH * GLOW_HEIGHT * 4 * sizeof(float)));
		glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		if (timing)
		{
			glEndQuery(GL_TIME_ELAPSED);
			g.timing[slot] = true;
		}
		glstate::RestoreClient(client);
		glBindTexture(GL_TEXTURE_2D, unit0);
		glActiveTexture(active);
		glUseProgram(static_cast<GLuint>(program));
		glClearColor(clear[0], clear[1], clear[2], clear[3]);
		glstate::Apply(game);
		g.waiting[slot] = true;
		g.mode[slot] = currentMode;
		if (currentMode == MODE_BATTLE)
			g.camera[slot] = stageLight.view;
		else
			g.projection[slot] = FieldProjection();
	}

	//the field's effect lights for its shaders (two at most), when they changed
	void SendFieldGlows()
	{
		Glows& g = glows;
		fieldrt::EffectGlows next;
		next.serial = g.fieldOut.serial;
		const std::vector<scenelight::EffectLight>& lights = g.fieldLights.Lights();
		for (size_t i = 0; i < lights.size() && i < static_cast<size_t>(MAX_EFFECT_GLOWS); i++)
			for (int k = 0; k < 3; k++)
			{
				next.at[i * 4 + k] = static_cast<float>(lights[i].stage[k]);
				next.colour[i * 4 + k] = lights[i].colour[k];
				next.at[i * 4 + 3] = lights[i].range;
			}
		if (memcmp(next.at, g.fieldOut.at, sizeof(next.at)) != 0 || memcmp(next.colour, g.fieldOut.colour, sizeof(next.colour)) != 0)
		{
			next.serial++;
			g.fieldOut = next;
		}
	}

	//glowing effects are gathered here: battles, and PBR fields
	bool GlowPlace(const int mode)
	{
		return mode == MODE_BATTLE || (mode == MODE_FIELD && PbrView().active && PbrView().settings != nullptr);
	}

	//SwapBuffers: the last frame's grid into lights, this frame's onto the grid
	void GlowsFrameEnd()
	{
		Glows& g = glows;
		const int mode = currentMode;
		if (!EFFECT_LIGHTS || !GlowPlace(mode))
		{
			g.vertices.clear();
			g.runs.clear();
			g.waiting[0] = g.waiting[1] = false;
			g.lights.Clear();
			g.fieldLights.Clear();
			SendFieldGlows();
			g.statsFrom = 0;
			return;
		}
		if (mode != MODE_BATTLE)
			g.lights.Clear();
		if (mode != MODE_FIELD)
		{
			g.fieldLights.Clear();
			SendFieldGlows();
		}
		const long long started = Ticks();
		const int write = static_cast<int>(g.frame & 1), read = write ^ 1;
		std::vector<scenelight::EffectLight> seen;
		if (g.waiting[read] && g.mode[read] != mode)
			g.waiting[read] = false; //gathered somewhere else (a battle has just started or ended)
		if (g.waiting[read])
		{
			g.waiting[read] = false;
			glBindBuffer(GL_PIXEL_PACK_BUFFER, g.pixels[read]);
			if (const float* data = static_cast<const float*>(glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY)))
			{
				scenelight::GlowGrid grid;
				grid.width = GLOW_WIDTH;
				grid.height = GLOW_HEIGHT;
				grid.cells.resize(static_cast<size_t>(GLOW_WIDTH) * GLOW_HEIGHT * 5);
				const float* info = data + GLOW_WIDTH * GLOW_HEIGHT * 4;
				for (int y = 0; y < GLOW_HEIGHT; y++)
					for (int x = 0; x < GLOW_WIDTH; x++)
					{
						//the grid's rows top first; OpenGL's bottom first
						const size_t from = (static_cast<size_t>(GLOW_HEIGHT - 1 - y) * GLOW_WIDTH + x) * 4;
						float* to = &grid.cells[(static_cast<size_t>(y) * GLOW_WIDTH + x) * 5];
						to[0] = data[from];
						to[1] = data[from + 1];
						to[2] = data[from + 2];
						to[3] = info[from];
						to[4] = info[from + 1];
					}
				glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
				for (const scenelight::Glow& glow : scenelight::FindGlows(grid, static_cast<int>(scenelight::MAX_EFFECT_LIGHTS), 10, 0.02))
				{
					scenelight::EffectLight light;
					if (mode == MODE_BATTLE)
						seen.push_back(scenelight::GlowToLight(g.camera[read], glow, EFFECT_LIGHTS_STRENGTH));
					else if (FieldGlowToLight(g.projection[read], glow, EFFECT_LIGHTS_STRENGTH, light))
						seen.push_back(light);
				}
			}
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		}
		if (mode == MODE_BATTLE)
			g.lights.Frame(seen, 0.75);
		else
		{
			g.fieldLights.Frame(seen, 0.75);
			SendFieldGlows();
		}
		if (g.timing[read])
		{
			GLint available = 0;
			glGetQueryObjectiv(g.timer[read], GL_QUERY_RESULT_AVAILABLE, &available);
			if (available)
			{
				GLuint64 ns = 0;
				glGetQueryObjectui64v(g.timer[read], GL_QUERY_RESULT, &ns);
				g.gpu += ns / 1e6;
				g.timed++;
				g.timing[read] = false;
			}
		}
		if (!g.runs.empty() && (mode != MODE_BATTLE || stageLight.camera) && MakeGlows())
			GatherGlows(write);
		g.vertices.clear();
		g.runs.clear();
		g.frame++;
		g.cpu += Ticks() - started;
		g.frames++;
		g.lit += mode == MODE_BATTLE ? g.lights.Lights().size() : std::min(g.fieldLights.Lights().size(), static_cast<size_t>(MAX_EFFECT_GLOWS));
		//what it costs, every 10 s (SCENE_DEBUG)
		const ULONGLONG tick = GetTickCount64();
		if (g.statsFrom == 0)
			g.statsFrom = tick;
		else if (SCENE_DEBUG && tick - g.statsFrom >= 10000 && g.frames > 0)
		{
			LARGE_INTEGER frequency;
			QueryPerformanceFrequency(&frequency);
			const double frames = static_cast<double>(g.frames);
			OutputDebug("EFFECT_LIGHTS: %s, the last %.0f s: %.1f glowing draws a frame (%.0f corners), %.1f lights on; CPU %.3f ms a frame, GPU %s\n",
				mode == MODE_BATTLE ? "battle" : "field", (tick - g.statsFrom) / 1000.0, g.draws / frames, g.corners / frames, g.lit / frames, g.cpu * 1000.0 / frequency.QuadPart / frames,
				g.timed > 0 ? (std::to_string(g.gpu / g.timed).substr(0, 5) + " ms a gathering pass").c_str() : "not measured");
			g.statsFrom = tick;
			g.draws = g.corners = g.frames = g.lit = g.timed = 0;
			g.cpu = 0;
			g.gpu = 0;
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
		if (type == TYPE_BATTLE_STAGE || type == TYPE_WORLD || type == TYPE_WORLD_SKY || type == TYPE_BATTLE_CHARACTER
			|| type == TYPE_FIELD_BACKGROUND || type == TYPE_FIELD_BACKGROUND_2)
			textures[texture].type = type;
		return;
	}
	if (whole)
	{
		//a new picture: the maps of the page it was are not its maps (DEMASTER's replacement sets its own after this)
		known->second.glows = known->second.orm = known->second.normal = 0;
		known->second.additive = known->second.height = false;
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
		t.glows = PageMap(stem, stage, page, "_EC").texture;
		t.additive = false;
		const MapFile ormh = PageMap(stem, stage, page, "_ORMH");
		t.orm = ormh.texture != 0 ? ormh.texture : PageMap(stem, stage, page, "_ORM").texture;
		t.height = ormh.texture != 0;
		t.normal = PageMap(stem, stage, page, "_N").texture;
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
		static std::string said;
		if ((t.glows != 0 || t.orm != 0 || t.normal != 0 || SCENE_DEBUG) && said != stem)
		{
			said = stem;
			OutputDebug("SCENE_LIGHT: stage %d page %d: %s%s%s%s\n", stage, page, t.normal != 0 ? "a normal map (_N)" : "no _N",
				t.glows != 0 ? ", a picture of its glows (_EC)" : ", no _EC", t.orm != 0 ? (t.height ? ", an ORMH map (the height in its alpha)" : ", an ORM map") : ", no _ORM",
				t.glows == 0 && t.orm == 0 && t.normal == 0 ? " (next to its picture; the time of day and the stage's lights work without them)" : "");
		}
	}
	catch (...)
	{
		OutputDebug("SCENE_LIGHT: reading a stage page's maps failed\n");
	}
}

//texturepatch_v2_world.cpp: DEMASTER just put its own picture of a world map page (texl_NNN_P, wmset_NNN_P) into the bound
//texture. Its maps are next to it: <file without extension>_N (normals, green up), _ORMH (AO, roughness, metalness, height in
//the alpha: 1 the surface; or _ORM, without the height), and the glows at night: _E (their colour on black: added) or _EC (a
//picture of the glows with its coverage, as battle stages have). A palette's own (texl_003_2_N), else the first palette's
void SceneLightWorldPage(const char* file)
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
		SceneTexture& t = textures[static_cast<GLuint>(bound)];
		t.type = TYPE_WORLD;
		t.stage = WORLD_PAGE;
		t.page = -1;
		std::string stem = file;
		if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos)
			stem.resize(dot);
		t.normal = WorldMap(stem, "_N").texture;
		const MapFile ormh = WorldMap(stem, "_ORMH");
		t.orm = ormh.texture != 0 ? ormh.texture : WorldMap(stem, "_ORM").texture;
		t.height = ormh.texture != 0;
		const MapFile covered = WorldMap(stem, "_EC");
		const MapFile emissive = covered.texture != 0 ? covered : WorldMap(stem, "_E");
		t.glows = emissive.texture;
		t.additive = covered.texture == 0 && emissive.texture != 0 && !emissive.alpha;
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
		static std::unordered_map<std::string, bool> said;
		if ((t.glows != 0 || t.orm != 0 || t.normal != 0 || SCENE_DEBUG) && !said[stem])
		{
			said[stem] = true;
			const size_t slash = stem.find_last_of("\\/");
			OutputDebug("SCENE_LIGHT: world page %s: %s%s%s\n", stem.c_str() + (slash == std::string::npos ? 0 : slash + 1),
				t.normal != 0 ? "a normal map (_N)" : "no _N", t.orm != 0 ? (t.height ? ", an ORMH map (the height in its alpha)" : ", an ORM map")
				: ", no _ORMH", t.glows != 0 ? (covered.texture != 0 ? ", its glows' picture (_EC)" : t.additive ? ", an emissive map (_E, added)"
				: ", an emissive map (_E, with its coverage)") : ", no _E");
		}
	}
	catch (...)
	{
		OutputDebug("SCENE_LIGHT: reading a world page's maps failed\n");
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
		{
			fromWorld = true;
			ground.Reset(); //(the world map again: its ground's corners from the start)
		}
		else if (mode == MODE_FIELD)
			fromWorld = false;
		lastMode = mode;
	}
	//EFFECT_LIGHTS: a draw of the game's main.frag that adds light in battle or a PBR field (a spell, a GF's fire, a save
	//point) is kept to light the stage, or the room, in the next frames; not a field's own glowing tiles
	if (EFFECT_LIGHTS && GlowPlace(mode) && glstate::BlendOn() && glstate::BlendEquationRgb() == GL_FUNC_ADD)
	{
		GLint source = 0, destination = 0;
		glstate::BlendFactors(source, destination);
		const GLint drawing = glstate::Program();
		if (destination == GL_ONE && drawing != 0 && GameProgramOf(drawing).kind == ProgramKind::Main)
		{
			const auto known = textures.find(glstate::BoundTexture2D());
			const bool tile = known != textures.end() && (known->second.type == TYPE_FIELD_BACKGROUND || known->second.type == TYPE_FIELD_BACKGROUND_2);
			if (!(mode == MODE_FIELD && tile))
				KeepGlow(drawing, drawMode, count, indexType, indices);
		}
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
		const ProgramKind kind = GameProgramOf(program).kind;
		if (kind == ProgramKind::Main)
		{
			LookUpUniforms(program, p);
			if (p.scene < 0)
				OutputDebug("SCENE_LIGHT: the game's main.frag (program %d) is not this release's: battles and the world map stay as they "
					"are. Copy DEMASTER_EXP\\shaders\\main.frag from this release.\n", program);
		}
		else if (kind == ProgramKind::NoTexture)
		{
			//main_notex.frag (plain colour: the world map's sky and haze): only the night's multiplier (DEMASTER's version)
			p.scene = glGetUniformLocation(static_cast<GLuint>(program), "demasterScene");
			p.plain = true;
			if (p.scene < 0)
				OutputDebug("SCENE_LIGHT: the game's main_notex.frag (program %d) is not this release's: the world map's sky stays as it is "
					"at night. Copy DEMASTER_EXP\\shaders\\main_notex.frag from this release.\n", program);
		}
		else
			p.scene = -1;
	}
	if (p.scene < 0)
		return false;
	float sceneValue[4] = { 0, 0, 0, 0 }, mapsValue[4] = { 0, 0, 0, 0 }, drawValue[4] = { 0, 0, 0, 0 }, surfaceValue[4] = { 0, 0, 0, 0 };
	bool stageLit = false, knownFighter = false;
	if (scene && (!p.plain || mode == MODE_WORLDMAP))
	{
		const auto known = textures.find(glstate::BoundTexture2D());
		static const SceneTexture none;
		const SceneTexture& t = known != textures.end() ? known->second : none;
		bool world = false, lit = false, sky = false;
		if (mode == MODE_WORLDMAP)
		{
			//the world map: what is drawn at a depth of its own is the scene (the ground, the sea, the sky, the vehicles, the
			//people, the haze), whatever its texture (the HD ones come in without the game's type); the map, the menus and the
			//screen's flashes are at rhw 1/3 or 1. The sky stands 4096 away: it isn't lit by the sun (only darkened at night)
			float rhw = 0;
			world = fieldrt::FirstCornerRhw(indexType, indices, rhw) && rhw > 0.0f && rhw < fieldrt::SCENE_MOST_RHW;
			sky = world && rhw < 1.0f / 4000.0f; //(the sky at 4096; the farthest ground seen from the Ragnarok about 3000)
			lit = world;
		}
		else
		{
			//(a battle on the world map stands on the world's own textures)
			world = known != textures.end() && (t.type == TYPE_WORLD || t.type == TYPE_WORLD_SKY);
			lit = world || (known != textures.end() && (t.type == TYPE_BATTLE_STAGE || t.type == TYPE_BATTLE_CHARACTER));
		}
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
			if (mode == MODE_WORLDMAP && !p.plain)
			{
				//lit from the world's own space: the sun, or the moon, by the clock (not the sky)
				WorkStageLight();
				stageLit = true;
				drawValue[0] = sky ? 0.0f : 1.0f;
				drawValue[3] = sky ? -1.0f : 0.0f; //(the sky: no lights on it)
				//its page's maps (SceneLightWorldPage): the normal map and the ORMH map answer to the sun, not on the sky
				drawValue[1] = !sky && t.normal != 0 ? 1.0f : 0.0f;
				drawValue[2] = !sky && t.orm != 0 ? 1.0f : 0.0f;
				if (!sky && t.height && t.orm != 0)
					surfaceValue[0] = worldLook.parallax;
				if (!sky && stageLight.camera && !worldUp.decided && !glstate::BlendOn())
					MeasureWorldUp(drawMode, count, indexType, indices);
			}
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
				if (t.height && t.orm != 0 && drawValue[0] > 0.5f)
					surfaceValue[0] = battleLook.parallax;
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
				mapsValue[0] = t.glows == 0 ? 0.0f : t.additive ? 2.0f : 1.0f;
				mapsValue[1] = now.evening;
				mapsValue[2] = now.glow;
				mapsValue[3] = t.orm != 0 ? now.ao : 0.0f;
			}
			if (SCENE_DEBUG && mode == MODE_BATTLE && t.type == TYPE_BATTLE_STAGE)
				LogProjection(program, count);
			else if (SCENE_DEBUG && mode == MODE_WORLDMAP && world)
				LogProjection(program, count, true);
		}
	}
	//a fighter: drawn by DEMASTER with smooth normals (its maps' units are set above, as for the game's draw)
	if (stageLit && drawValue[0] < 0.5f && knownFighter && DrawFighter(program, drawMode, count, indexType, indices, sceneValue, mapsValue, drawValue))
		return true;
	//the world map's ground (and what stands on it) where it is opaque: drawn by DEMASTER with smooth normals
	if (stageLit && mode == MODE_WORLDMAP && drawValue[0] > 0.5f && worldLook.smooth && Opaque()
		&& DrawGround(program, drawMode, count, indexType, indices, sceneValue, mapsValue, drawValue, surfaceValue))
		return true;
	Send(program, p, sceneValue, mapsValue, drawValue, stageLit, surfaceValue);
	return false;
}

//LUT: what time it is in this battle or on the world map as SCENE_LIGHT has it (scene_light.txt's time and night, where the
//battle started): 0 the clock's, 1 always day, 2 always night; -1: SCENE_LIGHT is off
int SceneLightTimeHere(const int mode)
{
	if (!SCENE_LIGHT || (mode != MODE_BATTLE && mode != MODE_WORLDMAP))
		return -1;
	Work(mode);
	return now.timeHere == scenelight::Time::Day ? 1 : now.timeHere == scenelight::Time::Night ? 2 : 0;
}

int SceneLightMoment()
{
	return Moment();
}

const fieldrt::EffectGlows& fieldrt::FieldEffectGlows()
{
	return glows.fieldOut;
}

namespace
{
	//Ctrl+F3 on the world map (DAY_NIGHT_KEYS): a light where the party stands, a little above it, written into
	//scene_light.txt as a rule of its own (move it, colour it or take it out there; the file is read again at once)
	bool worldKeyWasDown = false;

	void WorldLightKey()
	{
		if (!DAY_NIGHT_KEYS || currentMode != MODE_WORLDMAP)
		{
			worldKeyWasDown = false;
			return;
		}
		const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
		const bool pressed = down && !worldKeyWasDown;
		worldKeyWasDown = down;
		if (!pressed)
			return;
		const HWND window = GetForegroundWindow();
		DWORD process = 0;
		if (window == nullptr || GetWindowThreadProcessId(window, &process) == 0 || process != GetCurrentProcessId())
			return;
		double party[3];
		if (!ReadWorldPlace(WORLD_PLAYER, party))
		{
			OutputDebug("SCENE_LIGHT: Ctrl+F3: the party's place (WORLD_PLAYER) can't be read in this game: no light added\n");
			return;
		}
		ReadSettings();
		int number = 1;
		for (const scenelight::Rule& rule : settings.rules)
			if (rule.name.rfind("world light", 0) == 0)
				number++;
		//300 above the ground, up as the ground showed it (-y of its space unless measured otherwise)
		const double up = worldUp.decided && worldUp.sign < 0 ? 1.0 : -1.0;
		char text[512];
		snprintf(text, sizeof(text), "\n[rule world light %d]\n; put where the party stood with Ctrl+F3: x, y (the height, the game's way), z; "
			"colour; how far it reaches\nwhere = world\nlight = %.0f %.0f %.0f  1.6 1.15 0.7  2500  night\n", number, party[0],
			party[1] + 300.0 * up, party[2]);
		FILE* file = nullptr;
		if (fopen_s(&file, settingsPath.c_str(), "ab") != 0 || file == nullptr)
		{
			OutputDebug("SCENE_LIGHT: Ctrl+F3: %s can't be written: no light added\n", settingsPath.c_str());
			return;
		}
		fwrite(text, 1, strlen(text), file);
		fclose(file);
		settingsChecked = 0; //(read again on the next draw)
		OutputDebug("SCENE_LIGHT: Ctrl+F3: [rule world light %d] added to %s: a warm light at night 300 above the party (x %.0f z %.0f, "
			"height %.0f)\n", number, settingsPath.c_str(), party[0], party[2], party[1]);
	}
}

//SwapBuffers hook: a frame is over
void SceneLightFrameEnd()
{
	sceneFrame++;
	if (SCENE_LIGHT)
		WorldLightKey();
	if (SCENE_LIGHT)
	{
		FightersFrameEnd();
		GlowsFrameEnd(); //EFFECT_LIGHTS
	}
	if (SCENE_LIGHT && currentMode != MODE_BATTLE)
		cameraFilter.Reset(); //the next battle starts with its own camera
}
