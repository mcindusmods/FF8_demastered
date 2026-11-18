#pragma once
//PBR field backgrounds (FIELD_PBR): what DEMASTER and fieldprep agree on. No game, Windows or OpenGL code, so fieldprep
//and tests/field_pbr_test.cpp build it on their own.
//
//The game draws a PBR layer as albedo x lightmap, then adds the lights of the field on top:
//	visible = albedo x lightmap x (1 + flicker mask x (flicker - 1))  +  AO x added light (Lambert + GGX, from the normals)
//With every light at rest, albedo x lightmap is the painting, pixel for pixel (fieldprep makes the lightmap as painting /
//albedo). Only opaque layers (blend 4) are PBR; the glow and shadow layers (blend 0-3) are drawn as they are.
//
//Files, next to the layer images in DEMASTER_EXP\textures\field_bg\<xx>\<field>\ (PNG or BC7 DDS; with both, the newer
//one, as for layers). <field> is "bvtr_1", or "bghall_1_fr" for a field with a .map per language.
//	<field>_<pupu>_A      that layer's albedo (sRGB colour) and coverage (alpha)            written by fieldprep
//	<field>_LF            the field's lightmap (RGB) and steady mask (alpha)                written by fieldprep
//	<field>_<pupu>_LF     one layer's lightmap, where it differs from the whole field     written by fieldprep
//	<field>_ND            normal XYZ and depth (alpha)                                    written by fieldprep
//	<field>_ORME          AO, roughness, metallic, emissive (an older _ORMH: height, unused) written by fieldprep
//	<field>_EC            the glows as a picture: colour (sRGB, already times its coverage) and coverage (alpha: how
//	                      much of the painting the glow takes the place of). With it _ORME's alpha isn't read. written by fieldprep
//	<field>_<pupu>_ND / _ORME / _EC   the same for one layer (from per-layer inputs)
//	A picture of an animation state without an _ND, _ORME or _EC of its own takes the one of its state's first picture
//	(the name with its last digit 0), then the field's.
//	<field>_PROBES.dds    the room's light in 3D, for the characters                      written by fieldprep
//	<field>_pbr.txt       how to read them (camera, depth, lightmap scales)               written by fieldprep
//	<field>_lights.txt    the field's lights                                              written once by fieldprep, then yours
//A layer is PBR when it has its own _A and an _LF (its own or the field's).

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fieldpbr
{
	//--- names

	//NIGHT: a layer's night picture (_nt_A; "nt" so it isn't taken for _N): how the layer looks at night, shown as it is
	//painted (the clock cross-fades to it; the glows and the lamps' added light still go on top)
	enum Kind { ALBEDO, LIGHTMAP, NORMAL_DEPTH, ORME, EMISSIVE, NIGHT, KIND_COUNT };
	const char* KindSuffix(Kind kind); //"_A", "_LF", "_ND", "_ORME", "_EC", "_nt_A"
	//before v15.7 the ORM map was _ORMH, its alpha the height (nothing used it): still read, its alpha left out
	constexpr const char* OLD_ORM_SUFFIX = "_ORMH";

	//folder + field (+ "_" + language): "...\\bvtr_1"
	std::string FieldStem(const std::string& folder, const std::string& field, const std::string& language);
	//folder + field (+ "_" + language) + "_" + PUPU: "...\\bvtr_1_00400000"
	std::string LayerStem(const std::string& folder, const std::string& field, const std::string& language, uint32_t pupu);
	constexpr const char* SETTINGS_SUFFIX = "_pbr.txt";
	constexpr const char* LIGHTS_SUFFIX = "_lights.txt";
	constexpr int MAX_LIGHTS = 8; //drawn at a time: the moving lights (the sun or moon, the party light) and the field's own
	constexpr int MAX_FOUND_LIGHTS = 2; //fieldprep's large lights it finds per time of day
	constexpr int MAX_YOUR_LIGHTS = 6; //your own (marked in _LIGHTS.png or written into _lights.txt) it keeps per time of day
	//on at a time: by day two, the sun taking one place in a field that has it (so the sun and your light); by night six (the
	//moon has the sun's place). 1 + 1 + 6 = MAX_LIGHTS, so no light drops out while day and night cross-fade. Within a time
	//of day yours come first (in file order), then fieldprep's
	constexpr int MAX_DAY_LIGHTS = 2;
	constexpr int MAX_NIGHT_LIGHTS = 6;

	//--- colour

	float SrgbToLinear(float c); //0-1
	float LinearToSrgb(float c); //0-1
	const float* SrgbTable(); //256 entries: SrgbToLinear(i / 255)
	uint8_t LinearToSrgb8(float c);

	//--- the glows' picture (_EC), from your <field>_E.png
	//Is it a picture of the glows (colour, or an alpha of its own), or the mask from before v15.12 (grey on black, no alpha:
	//white = the painting glows there)? rgba: 8-bit RGBA
	bool EmissiveIsPicture(const uint8_t* rgba, size_t pixels);
	//Packs it. ownAlpha: its alpha is yours and the colour is as you see it (a layer over the night painting, as in a paint
	//program); else it is painted on black and the coverage comes from its brightness: nothing below a fifth of coverFull
	//(a soft halo adds to the painting), all of it from coverFull (0-1, of the brightest channel) up (a lit window takes the
	//painting's place). out: sRGB colour times coverage (worked out in linear light), coverage in alpha
	bool EmissiveHasAlpha(const uint8_t* rgba, size_t pixels);
	void PackEmissive(const uint8_t* rgba, size_t pixels, bool ownAlpha, float coverFull, uint8_t* out);

	//The lightmap is stored as LinearToSrgb(light / scale): 0..scale with the darks kept fine (scale per image, in _pbr.txt)
	uint8_t EncodeLightmap(float light, float scale);
	inline float DecodeLightmap(const uint8_t value, const float scale) { return SrgbTable()[value] * scale; }
	//_LF alpha: 255 = steady light; lower = the part of the light that follows the nearest flickering light. (Stored this way
	//round so the file stays opaque where nothing flickers: image editors throw away colour under zero alpha.)
	inline uint8_t EncodeFlickerMask(const float mask) { return static_cast<uint8_t>(255.0f - (mask < 0 ? 0 : mask > 1 ? 1 : mask) * 255.0f + 0.5f); }
	inline float DecodeFlickerMask(const uint8_t value) { return 1.0f - value / 255.0f; }

	//--- where a pixel is
	//
	//View space: x right, y down (as on screen), z away from the camera; one unit is 1/4096 of the game's world units.
	//A pixel at (x, y) on the field's canvas (native pixels: layer image pixels / scale) at distance z from the camera is at
	//	((x - originX) * z / zoom, (y - originY) * z / zoom, z)
	//(the game projects with screen offset = view x * zoom / z, centred on the camera axis). The depth map gives z through
	//the depth model: linear (z = a + b * value) or inverse (z = 1 / (a + b * value), for depth maps that store disparity,
	//like Depth Anything). Value: the depth map's 0-1. fieldprep measures zoom and the model from the field's camera and
	//walkmesh; without them it estimates.

	struct Vec3
	{
		double x = 0, y = 0, z = 0;
	};
	inline Vec3 operator+(const Vec3& a, const Vec3& b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
	inline Vec3 operator-(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
	inline Vec3 operator*(const Vec3& a, const double s) { return { a.x * s, a.y * s, a.z * s }; }
	inline double Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	Vec3 Cross(const Vec3& a, const Vec3& b);
	double Length(const Vec3& a);
	Vec3 Normalize(const Vec3& a);

	enum class DepthModel { Linear, Inverse };

	struct Space
	{
		bool measured = false; //from the field's camera and walkmesh, else estimated
		double zoom = 384.0;
		double originX = 0, originY = 0; //canvas pixel on the camera axis
		DepthModel model = DepthModel::Inverse;
		double a = 0.25, b = 0.75; //estimated: bright is near, the far wall 4 times as far as the nearest point
		double nearest = 0.05; //smallest distance used (keeps a bad fit finite)

		double Distance(double value) const;
		double Value(double distance) const; //the depth map value at that distance (inverse of Distance)
		Vec3 ViewPosition(double canvasX, double canvasY, double value) const;
		//view units for a length of `pixels` canvas pixels at that distance
		double ViewLength(const double pixels, const double distance) const { return pixels * distance / zoom; }
		bool Project(const Vec3& view, double& canvasX, double& canvasY) const;
	};
	//canvas origin: Layout::shiftX/Y (the tile coordinates are relative to the camera axis)
	Space EstimatedSpace(double originX, double originY, bool brightIsNear);

	//--- the field's camera (.ca) and walkmesh (.id)

	struct Camera
	{
		int16_t axis[3][3]{}; //x, y, z axis, 4096 = 1
		int32_t position[3]{};
		uint16_t zoom = 0;
	};
	//40 bytes per camera (38 for the last one is accepted, as Deling does)
	std::vector<Camera> ParseCameras(const uint8_t* data, size_t size);

	struct Walkmesh
	{
		std::vector<std::array<Vec3, 3>> triangles; //world units / 4096
	};
	//uint32 count, count x 3 x (int16 x, y, z, pad), then count x 3 int16 neighbours
	Walkmesh ParseWalkmesh(const uint8_t* data, size_t size);

	//World (/4096) to view space for a camera. Variant 0 is the convention Deling draws its walkmesh with over the background
	//(axis Y and position Y negated, look-at along axis Z); 1-3 flip the screen axes, for fieldprep to check against.
	struct CameraTransform
	{
		Vec3 eye, right, down, forward;
		double zoom = 0;
		Vec3 ToView(const Vec3& world) const;
	};
	CameraTransform MakeCameraTransform(const Camera& camera, int variant = 0);

	//--- the game's own depth
	//
	//The game sorts by the camera distance its GTE works out (SZ, in world units): a character vertex arrives with
	//1 / rhw = 4 x SZ, a background tile with 16 x its .map Z (Z is that distance / 4). View units here are world / 4096,
	//so the game's depth is 16384 x the view z.
	constexpr double GAME_DEPTH_PER_VIEW = 16384.0;
	constexpr int FAR_TILE_Z = 4094; //the base picture: behind everything

	//--- per-field settings, written by fieldprep: <field>_pbr.txt

	//a field's time of day: the day/night clock's, or always day (as painted) or always night (a story scene, a boss)
	enum class FieldTime
	{
		Cycle,
		Day,
		Night
	};
	const char* FieldTimeName(FieldTime time);

	struct Settings
	{
		Space space;
		//the canvas: the part of the field (.map coordinates: left, top, width, height in native pixels) that the field's own
		//maps (_LF, _ND, _ORME), the lights and the camera's origin are on. fieldprep writes it; a file without it: FME's
		//canvas (every file before v15.8)
		bool hasCanvas = false;
		int canvas[4] = { 0, 0, 0, 0 };
		bool brightIsNear = true; //how the depth map was found (informational)
		bool normalYDown = false; //the normal map fieldprep read had green pointing down (it was flipped when packed)
		float lightmapScale = 1.0f; //<field>_LF
		std::map<uint32_t, float> layerLightmapScale; //<field>_<pupu>_LF
		float LightmapScale(uint32_t pupu, bool ownLightmap) const;

		//the game's depth per view unit (GAME_DEPTH_PER_VIEW when the depth is measured; fieldprep's measure when it isn't)
		double gameDepth = GAME_DEPTH_PER_VIEW;
		//tiles with a nearer Z than the base picture: the view distance of the nearest part of what they show (FIELD_PBR_DEPTH
		//keeps that part at the tile's own depth and pushes what lies behind it back)
		std::map<int, double> occlusion;
		//the camera in view space (fieldprep writes it when it measured the depth): world (/4096) to view
		bool cameraKnown = false;
		Vec3 cameraEye, cameraRight, cameraDown, cameraForward;
		Vec3 up{ 0, -1, 0 }; //the world's up in view space (from the walkmesh's floor)
		//depth map value where the sky starts (fieldprep: the farthest depth there is); used when sky is on
		double skyValue = -1;
		//the light grid (<field>_PROBES.dds): the view distances its slices run between
		double probeNear = 0, probeFar = 0;
		//yours (fieldprep keeps them when it writes the file again)
		FieldTime time = FieldTime::Cycle; //cycle: follows the day/night clock; day or night: always that, the clock waits
		bool sky = false; //pixels at the farthest depth are sky: no added light, shadows or reflections on them
		bool characterShadeKeep = false; //characters keep the game's own darkening on top of the room's light
		//how bright the room's light makes the characters: <= 0 (auto) matches the game's brightness on average (the room
		//gives the direction and colour); a number scales the room's real light (1: as bright as the room is)
		double characterLight = 0;
		//the look (FIELD_PBR_LOOK and its three dials in demaster.conf; see "the look" below): this field's share of the
		//master dial (0: this field stays as painted), and its own value for a dial in place of demaster.conf's (< 0: none)
		float look = 1.0f;
		float lookRelight = -1.0f, lookHighlights = -1.0f, lookAo = -1.0f;
	};
	//Whatever the text doesn't set keeps the value out had (set the defaults first)
	bool ParseSettings(const std::string& text, Settings& out, std::string& warnings);
	std::string WriteSettings(const Settings& settings, const std::string& field, const std::string& notes);

	//--- lights: <field>_lights.txt

	enum class Flicker { None, Fire, Candle, Pulse, Strobe, Fluorescent };
	const char* FlickerName(Flicker flicker);

	//when a light is on: by day, by night or both (DEMASTER cross-fades them with its day/night clock)
	enum class LightTime
	{
		Both,
		Day,
		Night
	};
	constexpr int MAX_LIGHTS_IN_FILE = 32; //a field's day and night lights together (MAX_LIGHTS of them are on at a time)
	//who a light is from: fieldprep's fit, your mark in _LIGHTS.png (both written again when fieldprep finds the lights
	//again), or you (written into _lights.txt by hand: fieldprep keeps it)
	enum class LightSource
	{
		Yours,
		Found,
		Mark
	};

	struct Light
	{
		std::string name;
		double x = 0, y = 0; //canvas, native pixels
		double depth = 0.5; //depth map value at the light
		float color[3] = { 1, 1, 1 }; //linear
		float intensity = 0.0f; //light added on top of the painting (0: none)
		float range = 64.0f; //native pixels at the light's depth
		float nightRadius = -1.0f; //a night lamp: how far around it the painting keeps its light at night (native pixels); -1: range
		float KeepRadius() const { return nightRadius > 0 ? nightRadius : range; }
		bool baked = true; //in the painting already: flicker scales its painted light (the _LF mask)
		Flicker flicker = Flicker::None;
		float amount = 0.25f; //how deep the flicker goes (0-1)
		float speed = 1.0f;
		float specular = 1.0f; //added light's highlights
		float shadow = -1.0f; //how much a character's shadow takes away from this light (0-1); -1: 1 for a new light, 0.5 for a painted one
		int follows = -1; //background light group (the animation id in the pupu: 004 01 000 is group 01) it follows; -1: none
		std::vector<int> onStates; //on only while its group shows one of these states (empty: always on, only its colour follows)
		LightTime time = LightTime::Both; //left out of the file: both
		LightSource source = LightSource::Yours; //left out of the file: yours
		std::string note; //written as a comment above it
		float ShadowStrength() const { return shadow >= 0 ? (shadow > 1 ? 1.0f : shadow) : (baked ? 0.5f : 1.0f); }
		//its painted light is under the _LF mask (it flickers, or its group can turn it off)
		bool OwnsMask() const { return baked && (flicker != Flicker::None || follows >= 0); }
	};
	//the field's glows (the picture of them, _EC; before v15.12 a mask in the alpha of _ORME that lit up the painting's own
	//colour): their colour and strength, for the whole field
	struct Emissive
	{
		float day = 0.0f; //how much of them shows by day (0-1); at night they show in full whatever this says
		float hue = 0.0f; //degrees the glows' painted colour turns around the colour wheel (-180 to 180)
		float tint[3] = { 1, 1, 1 }; //times this colour (linear): gives white glows a colour, which hue can't
		float strength = 1.0f; //how bright, 1 = as painted
	};
	//the field at night, for the whole field: the painted light goes down to level x tint, except around the lamps that are
	//on at night (time = night or both, painted), which keep `lamps` of it
	struct Night
	{
		float level = 0.18f; //how much of the painted light is left away from the night's lamps (0-1)
		float tint[3] = { 0.6f, 0.7f, 1.0f }; //what's left times this (linear): moonlight
		float lamps = 0.9f; //how much of the painted light the night's lamps keep around them, within their range (0-1)
	};
	//--- the moving lights (FIELD_MOVING_LIGHTS in demaster.conf; the maths is in field_moving_lights.h)
	//The sun: a light that crosses the sky with the day/night clock, for fields open to the sky. on: -1 auto (fields with
	//sky = 1 in _pbr.txt), 0 off, 1 on
	struct Sun
	{
		int on = -1;
		float azimuth = 0.0f; //degrees: where it rises, turned around the world's up from the camera's right (0: it rises on
		                      //the right of the picture and sets on the left; 180: the other way; 90: it rises behind the scene)
		float height = 60.0f; //degrees above the horizon at noon (it leans towards the camera, so what faces you is lit)
		float intensity = 0.35f; //light it adds on top of the painting (as a light's intensity)
		float shade = 1.0f; //its weight in the relight dial: how much the painted light follows it across the day
		float moon = 0.25f; //the moon's share of all that at night (0: no moon)
		float fixed = 0.3f; //where on its arc it stands in a field with time = day (0 rising, 0.5 noon, 1 setting)
	};
	//The party light: a light the party leader carries. on: -1 as FIELD_PARTY_LIGHT says, 0 off here, 1 at night, 2 always
	struct PartyLight
	{
		int on = -1;
		float color[3] = { 1.0f, 0.74f, 0.45f }; //linear: a warm lantern
		float intensity = 1.0f;
		float range = 72.0f; //native pixels at the leader's depth
		float height = 0.7f; //above the leader's middle, in half body heights (1: the top of the head)
		int follow = -1; //the script entity that carries it; -1: DEMASTER picks the leader
	};
	struct Lights
	{
		float ambient[3] = { 0, 0, 0 }; //added everywhere (light units, times albedo)
		std::vector<Light> lights;
		Emissive emissive;
		bool hasEmissive = false; //the file has an [emissive] section
		Night night;
		bool hasNight = false; //the file has a [night] section
		Sun sun;
		bool hasSun = false; //the file has a [sun] section
		PartyLight party;
		bool hasParty = false; //the file has a [party] section
	};
	bool ParseLights(const std::string& text, Lights& out, std::string& warnings);
	//only the day's set: the lights on by day (time = day or both), at most MAX_DAY_LIGHTS (yours first, then fieldprep's;
	//places: fewer when the sun takes one). fieldprep's light grid is the day's
	void KeepDayLights(Lights& lights, int places = MAX_DAY_LIGHTS);
	//the lights in the day's set or the night's (each as ActiveLights picks them)
	void KeepDayAndNightLights(Lights& lights);
	//A light on now. weight: how much of it is on (fades with the time of day); nightLamp: on at night and painted, so the
	//painting keeps its light around it at night
	struct ActiveLight
	{
		size_t index = 0; //in Lights::lights
		float weight = 1.0f;
		bool nightLamp = false;
	};
	//The lights on at `night` (0: day, 1: night, between: a fade). Each time of day has its set among the lights on then
	//(time = day or both, or night or both): by day at most dayPlaces (MAX_DAY_LIGHTS, one fewer with the sun), by night
	//MAX_NIGHT_LIGHTS; yours first (in file order), then fieldprep's. The night's set is on at `night`, the day's at dayOn
	//(< 0: 1 - night; the clock's evening starts it going before the dusk), a light in both sets at 1. At most `limit`, in
	//file order; more: the faintest are left out, `dropped` says how many.
	//limit: fewer than MAX_LIGHTS when the moving lights need places among them.
	std::vector<ActiveLight> ActiveLights(const Lights& lights, double night, size_t* dropped = nullptr, size_t limit = MAX_LIGHTS,
		double dayOn = -1.0, int dayPlaces = MAX_DAY_LIGHTS);

	//--- the look (FIELD_PBR_LOOK in demaster.conf): how far the maps change the painting itself
	//
	//With everything at 0 a PBR field at rest is its painting, exactly. The dials (each times the master dial, times the
	//field's own look in _pbr.txt):
	//  relight     the painted light follows the normal map: brighter where a surface faces the lamps that are on, darker
	//              where it faces away (demaster_field.frag, "the look")
	//  highlights  the painted lamps make highlights (GGX, from the roughness and metallic maps) without adding their
	//              diffuse light again
	//  ao          the AO map darkens the painting too (without it, only the added light)
	struct Look
	{
		float relight = 0, highlights = 0, ao = 0;
		bool Any() const { return relight > 0 || highlights > 0 || ao > 0; }
	};
	//demaster.conf's dials (master, relight, highlights, ao) and the field's settings, as what the shader gets
	Look FieldLook(const Settings& settings, double master, double relight, double highlights, double ao);
	//One spot of the field (view space), for RelightScale
	struct ShadeSample
	{
		Vec3 position, normal;
	};
	//The relight dial multiplies the painted light by (0.25 + scale x S) / (0.25 + W): W adds up the weights of the lamps
	//that reach a pixel (weight x falloff), S the same times how the pixel's normal faces each lamp (0 away, 1 towards it;
	//demaster_field.frag has the same sums). This is the scale that leaves the field's average brightness where it was,
	//worked out over `samples`. lightPos (x, y, z, range per light) and lightPaint (.., .., .., weight) are the shader's
	//arrays. 1.5 (a surface half turned to its lamps stays as it is) when there is nothing to go by.
	//The samples from a grid of the field's _ND (RGBA, 4 bytes a point, rows from the top: the normal as the map stores it,
	//the depth value in alpha), spread evenly over the canvas. Sky (settings.sky) is left out.
	std::vector<ShadeSample> ShadeSamples(const uint8_t* nd, int gridX, int gridY, const Settings& settings, int canvasWidth, int canvasHeight);
	constexpr double RELIGHT_REST = 0.25; //the share of the painted light that no lamp moves
	double RelightScale(const std::vector<ShadeSample>& samples, const float* lightPos, const float* lightPaint, int count);
	const char* LightTimeName(LightTime time);
	const char* LightSourceName(LightSource source);
	std::string WriteLights(const Lights& lights, const std::string& field, const std::string& header);
	//Brightness of a light over time, 1 = as painted. seed: makes lights flicker out of step.
	double FlickerValue(Flicker flicker, float amount, float speed, uint32_t seed, double seconds);
	uint32_t LightSeed(const Light& light, size_t index);

	//--- day and night (DAY_NIGHT in demaster.conf): one clock for the whole game
	//
	//The clock counts seconds into the cycle: day, then night, with a fade centred on each change; 0 is the middle of the
	//dawn's fade. Fields, the world map and menus run it at 1 second per second, battles slower (the same day lasts longer
	//there), so the time of day goes on from wherever the battle left it.
	struct DayNightCycle
	{
		double day = 480; //seconds of day (8 minutes), from the middle of dawn's fade to the middle of dusk's
		double night = 240; //seconds of night (4 minutes)
		double fade = 40; //seconds each fade takes
		double battleSlower = 3; //battles run the clock this many times slower (24 + 12 minutes, 120 s fades)
		double evening = 0.9; //the share of the day after which the day's own lights start going and the glows start coming on
		double Length() const { return day + night; }
		//no zero or negative lengths, fades no longer than the shorter of day and night
		DayNightCycle Checked() const;
	};
	//how night it is at `time` seconds into the cycle: 0 day, 1 night, smooth across the fades
	double NightAmount(const DayNightCycle& cycle, double time);
	//the evening: like NightAmount, but its dusk starts at `evening` of the day (the day's own lights go 1 - this, the glows
	//come on with it) and ends with the dusk's fade; the dawn is NightAmount's
	double EveningAmount(const DayNightCycle& cycle, double time);
	//the clock after `seconds` of play (in battle: battleSlower times slower), wrapped into the cycle
	double AdvanceClock(const DayNightCycle& cycle, double time, double seconds, bool battle);
	//mostly day now: the start of the full night; mostly night: the start of the full day
	double SwapTime(const DayNightCycle& cycle, double time);
	//`lead` seconds before the next fade starts
	double BeforeNextFade(const DayNightCycle& cycle, double time, double lead);
	//"day, 2:15 in (dusk in 5:05)" for the log
	std::string ClockText(const DayNightCycle& cycle, double time);
	//The time of day each save was made at (DEMASTER_EXP\daynight_saves.txt, DAY_NIGHT_SAVES): the game's play time when the
	//save was written, and the clock then. A save that is loaded brings its play time back, and with it its time of day.
	struct SaveClock
	{
		uint32_t play = 0; //the game's play time counter at the save
		double clock = 0; //seconds into the cycle
	};
	struct SaveClocks
	{
		std::vector<SaveClock> entries; //oldest first
		static constexpr size_t MOST = 400;
		//a save at this play time: one already there for the same play time is replaced; the oldest go past MOST
		void Record(uint32_t play, double clock);
		//the save a loaded play time belongs to: its play time at most `tolerance` before ours (the game counts on for a
		//moment after it makes the save's picture, before the file is written and seen); the nearest. nullptr: none
		const SaveClock* Find(uint32_t loaded, uint32_t tolerance) const;
	};
	bool ParseSaveClocks(const std::string& text, SaveClocks& out);
	std::string WriteSaveClocks(const SaveClocks& clocks);
	//the clock's file (DEMASTER_EXP\daynight.txt): "time = <seconds into the cycle>"
	bool ParseClock(const std::string& text, double& time);
	std::string WriteClock(const DayNightCycle& cycle, double time);

	//--- the light grid: <field>_PROBES.dds, written by fieldprep
	//
	//The room's light at points in front of the camera, so the characters get the light around them. A box of probes:
	//nx x ny across the canvas (each at the middle of its part of the canvas), nz slices of view distance from probeNear to
	//probeFar (in _pbr.txt). Each probe holds the light arriving at it as irradiance: E(n) = e0 + ex nx + ey ny + ez nz
	//for a surface facing n (view space), so a white wall's colour times E / pi is its brightness; and the share of that
	//light that follows the field's flickering lights.
	//The file is a 3D DDS (RGBA16F), nz x 4 slices deep: block 0 (e0 rgb, share), 1 (ex rgb), 2 (ey rgb), 3 (ez rgb).
	struct ProbeGrid
	{
		int nx = 0, ny = 0, nz = 0;
		std::vector<float> data; //[block][z][y][x][4]
		float* At(int block, int x, int y, int z) { return data.data() + ((((static_cast<size_t>(block) * nz + z) * ny + y) * nx + x) * 4); }
		const float* At(int block, int x, int y, int z) const { return data.data() + ((((static_cast<size_t>(block) * nz + z) * ny + y) * nx + x) * 4); }
		bool IsValid() const { return nx > 0 && ny > 0 && nz > 0 && data.size() == static_cast<size_t>(nx) * ny * nz * 16; }
	};
	std::vector<uint8_t> WriteProbes(const ProbeGrid& grid);
	bool ReadProbes(const uint8_t* data, size_t size, ProbeGrid& out);
	uint16_t FloatToHalf(float f);
	float HalfToFloat(uint16_t h);

	//--- small text helpers (shared by both files)
	std::string Trim(const std::string& text);
	std::string Lower(std::string text);
	std::vector<std::string> SplitWords(const std::string& text);
}
