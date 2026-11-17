#pragma once
//Inside the DLL: what the field layer module (texturepatch_v2_fieldLayers.cpp) and the PBR module
//(texturepatch_v2_fieldPbr.cpp) share. The rules both sides of PBR agree on (file names, lights, depth) are in
//field_pbr.h, which fieldprep uses too.

#include "field_layers.h"
#include "field_pbr.h"
#include "field_moving_lights.h"
#include "field_tween.h"

#include <GL/glew.h>

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace fieldrt
{
	void Log(const char* format, ...);
	void LogOnce(const std::string& key, const char* format, ...);

	//<stem>.png or <stem>.dds; with both, the newer one
	struct ImageFile
	{
		std::string path;
		std::filesystem::file_time_type time{};
	};
	ImageFile ResolveImage(const std::string& stem);
	//a PNG's or DDS's size from its header (nothing decoded); false: not one, or unreadable
	bool ImageFileSize(const std::string& path, int& width, int& height);
	//Reads a layer file again when it changed on disk since it was loaded (or loads it). false: unchanged, or unreadable
	//right now (still being written: try again later). Renderer thread.
	bool RefreshLayerFile(const ImageFile& file);
	//Forgets pages put together on the CPU (after files changed)
	void ForgetComposed();

	//The PBR files of a field (game thread makes it when the field's .map is read; read-only afterwards)
	struct PbrField
	{
		std::string name, language, folder;
		std::string settingsPath, lightsPath; //empty: none
		fieldpbr::Settings settings;
		fieldpbr::Lights lights;
		struct Layer
		{
			std::array<std::string, fieldpbr::KIND_COUNT> stem; //no extension; empty: none
			std::array<std::string, fieldpbr::KIND_COUNT> path; //file in use
			bool ownLightmap = false; //<field>_<pupu>_LF (else the field's)
		bool emissive = false; //its ORM map is an _ORME (alpha: emissive); an older _ORMH's alpha (height) isn't used
		};
		std::unordered_map<uint32_t, Layer> layers; //PBR layers only
		std::string roomStem; //<field>_ND (the whole field on the canvas: the room's depth); empty: none
		std::string probesPath; //<field>_PROBES.dds (the room's light in 3D); empty: none
		int canvasWidth = 0, canvasHeight = 0; //native pixels
	};
	//Which opaque layers (pupu, blend) of the field have PBR files. null: none
	std::shared_ptr<const PbrField> PbrFindFiles(const std::string& folder, const std::string& name, const std::string& language,
		const fieldlayers::Layout& layout, const std::vector<std::pair<uint32_t, int>>& layers);

	//one page texture of the game, and how DEMASTER fills it
	struct PageJob
	{
		uint64_t entry = 0;
		std::string name;
		fieldlayers::Layout layout;
		fieldlayers::PagePlan plan;
		std::unordered_map<uint32_t, std::string> imagePath;
		std::unordered_map<uint32_t, std::string> paintingPath; //PBR layers: the painting (imagePath has the albedo)
		std::shared_ptr<const PbrField> pbr;
		std::vector<uint8_t> base; //the game's page (BGRA), when part of it is needed and there is no swizzled HD page
		std::vector<uint8_t> gameAlpha; //the game's page alpha, when semi-transparent tiles need it (InheritSemiTransparency)
		std::string basePath; //the field's swizzled HD page (FIELD_BACKGROUND names), under the stamps when part is needed
		int width = 0, height = 0; //as the game uploads it
		uint32_t tPage = 0;
		int slot = 0; //the game's palette number (tex_header)
		int palette = 0, depth = 0; //the .map's
		int kind = -1; //-1: the game's page; else the PBR map (fieldpbr::Kind) of that page
	};

	struct PageSize
	{
		int width = 0, height = 0; //HD size
		int logicalWidth = 256, logicalHeight = 256; //texels
	};
	//Fills target from the job (target is left bound). how: for the log
	PageSize BuildPage(GLuint target, const PageJob& job, std::string& how);

	//--- PBR module (renderer thread)
	void PbrPageBuilt(GLuint target, const PageJob& job, const PageSize& size); //after DEMASTER filled a page of the game
	void PbrForget(GLuint target);
	void PbrForgetAll();
	void PbrPoll(); //files changed on disk (FIELD_LAYERS_LIVE), once a second
	//Before a draw: a page with PBR layers is drawn with DEMASTER's own program (demaster_field.vert/.frag) instead of the
	//game's, with its maps and the field's lights. tileClamp: FIELD_SMOOTH's demasterTileClamp for the page (zeros: off).
	//count, type, indices: the draw's (its tiles say where the canvas is on the screen).
	//true: DEMASTER's program is in use for this draw (PbrAfterDraw puts the game's back).
	bool PbrBeforeDraw(GLuint boundTexture, GLint program, const float* tileClamp, GLsizei count, GLenum type, const void* indices);
	void PbrAfterDraw();
	bool PbrActive(); //PBR pages exist
	//true once DEMASTER's field program could not be made: layers are drawn from their paintings
	bool PbrShaderMissing();
	//the canvas fieldprep wrote into the field's _pbr.txt (canvas = left top width height); false: no file, or none in it
	bool PbrCanvas(const std::string& folder, const std::string& name, const std::string& language, fieldlayers::Frame& canvas);
	//the field's time of day from its _pbr.txt (time = cycle | day | night); false: no file
	bool PbrFieldTime(const std::string& folder, const std::string& name, const std::string& language, fieldpbr::FieldTime& time);
	void PbrEndFrame(); //SwapBuffers: what the frame's tile draws measured becomes the next frame's

	//--- what the character module (texturepatch_v2_fieldChars.cpp) gets of the field being drawn
	struct LightUniforms
	{
		int count = 0;
		float pos[fieldpbr::MAX_LIGHTS * 4] = {}; //view x, y, z, range
		float color[fieldpbr::MAX_LIGHTS * 4] = {}; //light added now; specular
		float flicker[fieldpbr::MAX_LIGHTS * 4] = {}; //painted light's change now, owns the mask, shadow strength, a night lamp (1: the
		                                              //painting keeps its light around it at night)
		//the look (FIELD_PBR_LOOK): the lamp's own colour now, for the highlights dial; its weight in the relight dial (negative:
		//a moving light)
		float paint[fieldpbr::MAX_LIGHTS * 4] = {};
		bool castsShadow[fieldpbr::MAX_LIGHTS] = {}; //characters cast shadows from it (shadow maps)
		int fieldCount = 0; //the first of them are the field's own; the rest (count - fieldCount) the moving lights
		//DAY_NIGHT: how night it is (0-1), the light left away from the night's lamps, how much the lamps keep; the tint (rgb)
		float night[4] = {};
		float nightTint[4] = { 1, 1, 1, 0 };
	};
	struct FieldView
	{
		bool active = false; //a field with PBR pages is being drawn
		uint64_t entry = 0;
		std::string name;
		const fieldpbr::Settings* settings = nullptr;
		int canvasWidth = 0, canvasHeight = 0;
		bool offsetKnown = false;
		double offsetX = 0, offsetY = 0; //where canvas (0, 0) lands on the game's screen (the main layer, last frame)
		double depthSlope = 0.9985 / 65536.0; //depth buffer z (before the 0.5 + z / 2 window mapping) per game depth unit
		GLuint room = 0; //the whole field's _ND
		GLuint probes = 0; //3D light grid
		int probeSlices = 0;
		float probeExposure = 1.0f;
		uint32_t parallaxLayers = 0; //layer ids (bits) that moved on their own last frame
		int partyFollow = -1; //the lights file's [party] follow: the script entity that carries the party light (-1: DEMASTER picks)
	};
	const FieldView& PbrView();
	const LightUniforms& PbrLights(); //the field's lights now (flicker, light groups)

	//--- what the field program gets of the characters (filled by the character module before the background is drawn)
	constexpr int MAX_CHARACTERS = 8;
	constexpr int MAX_SHADOW_MAPS = 8;
	struct CharFrame
	{
		int count = 0;
		GLuint depth = 0; //the depth buffer with only the characters in it (FIELD_PBR_DEPTH); 0: none this frame
		int depthWidth = 0, depthHeight = 0;
		float cardCenter[MAX_CHARACTERS * 4] = {}, cardUp[MAX_CHARACTERS * 4] = {}, cardRight[MAX_CHARACTERS * 4] = {},
			cardDir[MAX_CHARACTERS * 4] = {}, contact[MAX_CHARACTERS * 4] = {};
		GLuint capture = 0;
		int shadowCount = 0;
		float shadowMatrix[MAX_SHADOW_MAPS * 16] = {};
		float shadowInfo[MAX_SHADOW_MAPS * 4] = {};
		GLuint shadowMap = 0;
		float features[4] = {}; //shadows, reflections, contact, room shadows
	};
	void PbrSetCharacters(const CharFrame* frame); //null: none this frame
	bool PbrDrewThisFrame(); //a PBR page was drawn since the last SwapBuffers

	//--- the moving lights module (texturepatch_v2_fieldMovingLights.cpp; the maths: field_moving_lights.h)
	//
	//The sun and the party light, as lights added after the field's own. The PBR module asks for them whenever it works out
	//the lights (Lights() in texturepatch_v2_fieldPbr.cpp); the character module says each frame which characters it drew.
	constexpr int MAX_MOVING_LIGHTS = 2;
	//the character module, once per frame before anything asks for the lights: the characters it drew this frame, and the
	//"playable character" byte most of the field's script entities have (-1: not known)
	void MovingLightsCharacters(uint64_t entry, const std::vector<fieldpbr::LeaderCandidate>& drawn, int pcOfMost);
	//how many places among the field's lights they need now (0 to MAX_MOVING_LIGHTS)
	int MovingLightsWanted(const fieldpbr::Settings& settings, const fieldpbr::Lights& lights, double night);
	//the moving lights that are on now, into out (MAX_MOVING_LIGHTS of them); how many
	int MovingLightsNow(const FieldView& view, const fieldpbr::Settings& settings, const fieldpbr::Lights& lights, double night,
		fieldpbr::MovingLight* out);
	//changes whenever MovingLightsNow would give something else (the leader moved, the sun moved on)
	uint64_t MovingLightsSerial();
	void MovingLightsEndFrame(); //SwapBuffers

	//--- FIELD_PBR_QUALITY: how much of the GPU the PBR field spends
	struct Quality
	{
		const char* name;
		int lightWallSteps; //steps of the check for walls between a pixel and an added light (0: no check)
		int reflectionWallSteps; //the same between a surface and the character it reflects
		int shadowTaps; //1: one sample per shadow map, 4: four (softer edges)
		int maxShadowMaps; //per frame, the light and character pairs that matter most
		int shadowSize, shadowTile; //the shadow map atlas and one map in it (texels)
		bool reflections; //characters reflected in the background (pictures of them each frame)
	};
	const Quality& CurrentQuality();

	//--- textures straight to the driver (past DEMASTER's own glTexImage2D / glTexSubImage2D hooks), into GL_TEXTURE_2D
	void RawTexImage(GLint internalFormat, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* data, GLint level = 0);
	void RawTexSubImage(GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* data);

	//--- shaders (DEMASTER_EXP\shaders\) and the game's programs
	//0 and error set when a file is missing or does not compile or link
	//defines: lines put right after the vertex shader's #version line (#extension, #define)
	GLuint BuildProgram(const char* vertexFile, const char* fragmentFile, std::string& error, const char* defines = nullptr);
	enum class ProgramKind
	{
		Unknown,   //no source
		Main,      //main.frag: backgrounds, effects
		Character, //main_char.frag: field characters and models (ambientColor)
		NoTexture, //main_notex.frag: the round shadows, plain colour
		Demaster,  //one of DEMASTER's own
		Other
	};
	struct GameProgram
	{
		bool looked = false;
		ProgramKind kind = ProgramKind::Unknown;
		GLint vertexType = -1, blendMode = -1, ambient = -1; //uniform locations
	};
	const GameProgram& GameProgramOf(GLint program);

	//--- smooth scroll and the in-between picture (texturepatch_v2_fieldTween.cpp; the rules: field_tween.h, the second
	//picture: field_mirror.h)
	//
	//FIELD_SMOOTH_SCROLL draws every layer of a field from layer images at the unrounded place of its scroll, and the
	//characters with the main layer's difference. FIELD_INTERPOLATION shows an in-between picture before each of the game's
	//frames. Both work on fields drawn from layer images (FIELD_LAYERS pages: those say which layer a tile belongs to).
	bool TweenOn(); //either is on (and its hooks were made)
	void TweenCreateHooks(); //opengl.cpp, with the other OpenGL hooks
	void TweenPageBuilt(GLuint target, const PageJob& job, const PageSize& size); //the page's cells: which layer, where on its canvas
	void TweenPageForget(GLuint target);
	void TweenForgetAll(); //another field
	//glDrawElements hook, before the character module sees the draw. true: the draw is kept back and drawn later (a layer
	//the game draws before the main one, while characters wait for the main layer's place)
	bool TweenBeforeDraw(unsigned int mode, int count, unsigned int type, const void* indices);
	//the draw goes to the driver, moved when it is a layer's (and a second time for the in-between picture)
	void TweenDraw(unsigned int mode, int count, unsigned int type, const void* indices);
	//a draw of the game by another call (glDrawArrays): draw is called once or twice (the second time into the copies)
	void TweenOtherDraw(void (*draw)(void* context), void* context);
	void TweenBeginDraw(); //a glBegin of the game: its frame gets no in-between picture (those draws can't be made twice)
	//the character module keeps the game's character draws on this field even without PBR (they wait for the main layer)
	bool TweenKeepsCharacters();
	//The character module is about to draw what it kept. shift: how far the main layer is drawn from the game's place this
	//frame (screen units; the main layer keeps to it from here on). The return says whether the characters may be tweened.
	bool TweenCharacterShift(double shift[2]);
	//a shift in whole pixels of the framebuffer a draw with that projection and viewport size goes to
	void TweenSnap(double shift[2], const float projection[16], int viewportWidth, int viewportHeight);
	//The kept characters as they are shown this frame (x, y, z, rhw per vertex, the shift added) and the game's draws they
	//came in. Returns their places in the in-between picture (same layout; valid until the next call), or null when this
	//frame has none. tween false: they stand where they are shown.
	const float* TweenCharacterPositions(const std::vector<float>& shown, const std::vector<fieldtween::CharDrawRef>& draws, bool tween);
	GLuint TweenFramebufferCopy(GLuint gameFramebuffer); //for the second picture (0: none this frame)
	//the character module, for the kept-back draws: characters are waiting (and the framebuffer they go to); draw them now
	bool CharsPending(GLint& framebuffer);
	void CharsFlush();

	//--- memory (texturepatch_v2_fieldLayers.cpp)
	bool SafeRead(void* destination, const void* source, size_t length); //false: not readable
	bool GuestRead(void* destination, uint32_t guest, size_t length);
	bool GuestWrite(uint32_t guest, const void* source, size_t length);
}
