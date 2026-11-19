#pragma once
//SCENE_LIGHT's rules (no game, no Windows, no OpenGL): the time of day and colour overrides for battles and the world map.
//
//Battles and the world map are drawn by the game's main.frag from textures times vertex colours; there is no lighting to
//redo as in PBR fields. What DEMASTER changes is in DEMASTER_EXP\shaders\main.frag: the picture is multiplied by a colour
//(in linear light), and a battle stage page can have a picture of its glows (<page>_EC, as fields have) and an ORM map
//whose AO darkens it. This file says what that colour is: from the day/night clock, the place's settings in
//DEMASTER_EXP\scene_light.txt, and rules there that apply to some stages or some part of the story (the Lunar Cry).
//texturepatch_v2_sceneLight.cpp is the DLL's side; tests/field_pbr_test.cpp and tests/scene_shader_test.cpp test this.
//
//scene_light.txt ("; comment", "key = value", sections in [brackets]):
//	[battle] / [world]   the place's own look
//	  night       = auto | 0 | 1   does the night come here. auto: the world map always; a battle when it started on the
//	                               world map or in a field open to the sky (sky = 1 in its _pbr.txt). Battles from other
//	                               fields, and from a field with a fixed time (time = day or night in its _pbr.txt, or a
//	                               rule of field_time.txt), stay as painted.
//	  time        = cycle | day | night   cycle: the clock's; day / night: always that
//	  day_tint    = r g b   the picture times this by day (linear; 1 1 1: as painted)
//	  night_tint  = r g b   by night: times night_level x this
//	  night_level = 0-1
//	  ao          = 0-1     how much of an ORM map's AO is used
//	  glow        = 0-4     how bright the glows' picture is (1: as painted)
//	  lit from the stage's own space, or the world's (the camera is read from the game each frame):
//	  sun         = 0-1     how much a surface turned away from the sun (or moon) darkens; 0: the stage as painted
//	                        ([world]: 0.25 by default)
//	  relief      = 0-4     how strongly a page's normal map (<page>_N) shows in the sun's light
//	  specular    = 0-4     the sun's highlight on pages with an ORM map
//	  parallax    = 0-64    how deep the height in a page's ORMH map (its alpha: 1 the surface) goes, in the place's
//	                        units: what is lower shifts with the view (0: off)
//	A battle stage page's maps are <page>_N, _ORMH (or _ORM), _EC; a world map page's (texl_NNN_P, wmset_NNN_P) _N,
//	_ORMH (or _ORM) and _E (the glows' colour on black, added at night) or _EC
//	  sun_azimuth = degrees, sun_height = degrees   where it rises (0: along the stage's +x) and how high it climbs
//	[rule <name>]        the same keys (only the ones written change anything), for where its conditions hold:
//	  where  = battle | world | both      (left out: both)
//	  stage  = 12, 40-45                  battle stages (a0stgNNN); left out: every stage. With stages: battles only
//	  moment = 3000-65535                 the story's progress (the game's own counter); left out: always
//	  light  = x up z  r g b  range [night|day]   a light of the stage's own, up to four per stage (later rules add to
//	                                      earlier ones): its place in the stage's units (the stage's middle is 0 0 0, "up"
//	                                      is above its floor), its colour (linear, may be above 1), how far it reaches,
//	                                      and optionally when it is on. For battles only
//	Rules are applied in the file's order, later ones over earlier ones.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace scenelight
{
	enum class Where { Battle, World };
	enum class Time { Cycle, Day, Night };

	struct Light
	{
		float x = 0, up = 0, z = 0;
		float colour[3] = { 1, 1, 1 };
		float range = 2000;
		int when = 0; //0 always, 1 by night, 2 by day
	};
	constexpr size_t MAX_LIGHTS = 4;

	struct Look
	{
		int night = -1; //-1 auto, 0 never, 1 the night comes here
		Time time = Time::Cycle;
		float dayTint[3] = { 1.0f, 1.0f, 1.0f };
		float nightTint[3] = { 0.55f, 0.65f, 1.0f };
		float nightLevel = 0.3f;
		float ao = 0.5f;
		float glow = 1.0f;
		float sun = 0.35f, relief = 1.0f, specular = 0.5f, sunAzimuth = 30.0f, sunHeight = 60.0f;
		float fighterSun = 0.5f; //the fighters' share of the sun's darkening (their smooth normals; 0: as the game lights them)
		float shadow = 0.5f; //how much of the sun the fighters' shadows take off the stage (0: none)
		float parallax = 4.0f; //how deep a page's ORMH height goes (the place's units; 0: no parallax)
		std::vector<Light> lights; //the stage's own (from rules), four at most
	};

	struct Rule
	{
		std::string name;
		bool battle = true, world = true;
		std::vector<std::pair<int, int>> stages; //inclusive ranges; empty: every stage
		int momentFrom = -1, momentTo = -1; //inclusive; -1: no condition
		Look look; //only the members flagged below are used
		bool hasNight = false, hasTime = false, hasDayTint = false, hasNightTint = false, hasNightLevel = false, hasAo = false,
			hasGlow = false, hasSun = false, hasRelief = false, hasSpecular = false, hasSunAzimuth = false, hasSunHeight = false,
			hasFighterSun = false, hasShadow = false, hasParallax = false;
	};

	struct Settings
	{
		Look battle, world;
		std::vector<Rule> rules;
		//the world map's ground is big flat triangles: its sun darkens less by default (each triangle is lit flat)
		Settings() { world.sun = 0.25f; }
	};

	//warnings: one line each ("line 12: ..."), empty when all was understood. Unknown keys are skipped
	void Parse(const std::string& text, Settings& settings, std::string& warnings);
	//the file DEMASTER writes when there is none: the defaults, explained, with a rule for the Lunar Cry (left off: ; before it)
	std::string DefaultText();

	//The look in effect. stage: the battle stage (-1: not known); moment: the story's progress (-1: not known: rules with
	//a moment don't apply)
	Look Resolve(const Settings& settings, Where where, int stage, int moment);
	//How night it is here (0-1). clock: what the day/night clock says (0 day, 1 night); outdoors: what "night = auto"
	//means for this battle (see above; the world map: true)
	double NightHere(const Look& look, double clock, bool outdoors);
	//What time it is here, for the colour grade (LUT): the look's own (time = day | night), the day for a battle started in a
	//field with a fixed time (fixedPlace) and where the night doesn't come, else the clock's (Time::Cycle)
	Time TimeHere(const Look& look, bool fixedPlace, bool outdoors);
	//What the picture is multiplied by, in linear light: day_tint by day, night_level x night_tint by night
	void Multiplier(const Look& look, double night, float out[3]);

	//--- the battle camera
	//The game projects with the PlayStation's registers (GTE_CONTROL in mymemory.h): view = rotation x stage + translation
	//(x right, y down, z away), screen = centre + distance x (x, y) / z, in the PlayStation's 320-wide pixels; a vertex's
	//rhw is 1 / z. When the game hands its draws to OpenGL the registers hold what was set last: the battle camera, as a
	//frame log and the registers of the same moment show (the sky's wall comes out as a cylinder around the stage's middle).
	struct Camera
	{
		double r[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 }; //rows
		double t[3] = { 0, 0, 0 };
		double distance = 0, centreX = 0, centreY = 0;
	};
	//false: the registers don't hold a rotation (rows not of length 1 or not at right angles) or no distance
	bool CameraFromRegisters(const int32_t registers[32], Camera& camera);
	//The battle camera as the game builds it (BATTLE_CAMERA: a PlayStation MATRIX, 32 bytes), with the distance and the
	//screen's centre of `projection` (the registers': they don't change within a battle). false: not a rotation
	bool CameraFromMatrix(const uint8_t matrix[32], const Camera& projection, Camera& camera);
	//how far two cameras are apart: the largest difference of the rotations' entries, and of the translations
	void CameraDifference(const Camera& a, const Camera& b, double& rotation, double& translation);
	//a direction, or a place, of the stage's space in view space
	void DirectionToView(const Camera& camera, const double stage[3], double view[3]);
	void PlaceToView(const Camera& camera, const double stage[3], double view[3]);
	void PlaceToStage(const Camera& camera, const double view[3], double stage[3]);
	//a corner the game hands OpenGL (x, y on its 640 x 480 screen, rhw = 1 / z) in view space
	void ScreenToView(const Camera& camera, double x, double y, double rhw, double view[3]);
	//The sun's shadow map: an orthographic camera looking along -towards (towards the sun, the stage's space) around the
	//given places (x, y, z each, the stage's space), `margin` wider on each side. out: column-major, the stage's space to
	//the map: x, y 0-1 across it, z 0-1 its depth (0 nearest the sun). false: nothing to fit
	bool FitSunShadow(const double towards[3], const std::vector<float>& places, double margin, float out[16]);
	//The registers of one frame are the camera's unless something else was set last. A camera moves a little from frame to
	//frame, or cuts and then stays: a reading is taken when it is close to the one in use, or when two frames in a row
	//agree with each other. Else the one in use stays (for that frame).
	class CameraFilter
	{
	public:
		//true: `use` is this frame's camera; false: there is none yet
		bool Frame(bool readable, const Camera& read, Camera& use);
		void Reset() { has = hasPending = false; }
	private:
		static bool Close(const Camera& a, const Camera& b);
		Camera current, pending;
		bool has = false, hasPending = false;
	};

	//--- the world map: its camera is the game's own too (WORLD_CAMERA, the same layout as BATTLE_CAMERA), with the
	//registers' distance and centre; its picture is 224 lines of the PlayStation's starting 16 down the game's 480, so the
	//centre DEMASTER uses is the registers' + 8 (WORLD_CENTRE_DOWN). Which way is up in its space is measured: most of the
	//world map is ground, so the triangles it draws face up on the whole.
	constexpr double WORLD_CENTRE_DOWN = 8.0;
	//adds the facing (towards the camera, area-weighted, view space) of triangles: three corners each, x and y on the game's
	//screen and rhw (1 / depth), as the game hands them over
	void AddFacing(const Camera& camera, const std::vector<double>& corners, double sum[3]);
	//how far a facing summed in view space agrees with the world space's -y (up, as the PlayStation has it): -1 to 1, 0 when
	//there is none
	double FacingAlongUp(const Camera& camera, const double facing[3]);

	//--- lights from the effects (EFFECT_LIGHTS): a spell, a GF's fire, a limit's flash lights the stage and the fighters
	//What the frame's glowing draws (the ones that add light) put on the screen, gathered on a small grid over the game's
	//640 x 480 screen (DEMASTER draws them a second time into it, on the GPU): per cell the light added (linear r g b), its
	//brightness, and its brightness times its depth (the cell's depth is that / the brightness)
	struct GlowGrid
	{
		int width = 0, height = 0;
		std::vector<float> cells; //5 per cell, rows top first
	};
	//one glow: where on the screen (640 x 480) and how deep, the light it adds (summed over its cells), how far it spreads
	//on the screen (pixels: the brightness-weighted distance from its middle)
	struct Glow
	{
		double x = 0, y = 0, depth = 0;
		double rgb[3] = { 0, 0, 0 };
		double brightness = 0, spread = 0;
	};
	//The shaders DEMASTER gathers with (GLSL 1.20; uniforms: tex, demasterGlowAlpha 1 = the light is times the texture's
	//alpha, demasterGlowShare its share; two targets: the light, and its brightness with brightness x depth). The corners
	//are the game's own (TLVERTEX: x, y on its 640 x 480 screen, w = rhw), colour as the game sends it (BGRA)
	extern const char* const GLOW_VERTEX_SHADER;
	extern const char* const GLOW_FRAGMENT_SHADER;
	//The brightest few: the brightest cell left and the cells joined to it that are at least a twentieth as bright, up to
	//`radius` cells from it, are one glow (and taken off the grid), until there are `most` or nothing brighter than
	//`least` is left
	std::vector<Glow> FindGlows(GlowGrid grid, int most, int radius, double least);
	constexpr size_t MAX_EFFECT_LIGHTS = 4;
	struct EffectLight
	{
		double stage[3] = { 0, 0, 0 }; //the stage's space
		float colour[3] = { 0, 0, 0 }; //linear
		float range = 0;
	};
	//A glow as a light in the stage's space: back through the camera from its place on the screen and its depth, its colour
	//the summed light x strength / 16 (a glow of sixteen cells at full white is a light of 1), at most 4 bright; its range
	//three times its spread in the stage's units, 1500 to 6000
	EffectLight GlowToLight(const Camera& camera, const Glow& glow, double strength);
	//The effect lights from frame to frame. A spell flickers and moves: a light of this frame takes the place of the one
	//of the last frame nearest it (within its range), keeping the brighter of the two, the last one's faded by `fade`;
	//lights not seen again fade by `fade` each frame and go below a brightness of 0.02. The brightest MAX_EFFECT_LIGHTS stay
	class EffectLights
	{
	public:
		void Frame(const std::vector<EffectLight>& seen, double fade);
		const std::vector<EffectLight>& Lights() const { return lights; }
		void Clear() { lights.clear(); }
	private:
		std::vector<EffectLight> lights;
	};

	//The sun or the moon over the stage, from the clock: towards it in the stage's space (unit; y is down there), its
	//light's colour (linear), and how much of it there is (0 at the horizon, 1 from about 10 degrees up)
	struct Sun
	{
		double direction[3] = { 0, -1, 0 };
		float colour[3] = { 1, 1, 1 };
		double strength = 0;
		bool moon = false;
	};
	Sun SunOver(const Look& look, bool night, double phase);
}
