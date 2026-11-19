#pragma once

#include <sstream>
#include <cstdio>

#define WIN32_LEAN_AND_MEAN

#include <INIReader.h>
#include "renderer.h"
#include <memory>
#include "debug.h"
#include "mymemory.h"
#include "image.h"
#include "file.h"
#include "server.h"


#define EXPORT __declspec(dllexport)

#define DEBUG_BREAK_IF_DEBUGGER if(IsDebuggerPresent()) DebugBreak();

// ReSharper disable once CppInconsistentNaming
BYTE* InjectJMP(DWORD address, DWORD functionAddress, int JMPsize = 5);
BOOL ModPage(DWORD address, int size = 5);
void ReplaceWithNOP(DWORD address, int instructionSize = 5);
void InjectDWORD(DWORD address, DWORD value);
void InjectWORD(DWORD address, WORD value);
void InjectBYTE(DWORD address, BYTE value);


/****************************************************************************/
//    Copyright (C) 2009 Aali132                                            //
//    Copyright (C) 2018 quantumpencil                                      //
//    Copyright (C) 2018 Maxime Bacoux                                      //
//    Copyright (C) 2020 myst6re                                            //
//    Copyright (C) 2020 Chris Rizzitello                                   //
//    Copyright (C) 2020 John Pritchard                                     //
//    Copyright (C) 2023 Julian Xhokaxhiu                                   //
/****************************************************************************/
//From FFNx : https://github.com/julianxhokaxhiu/FFNx/blob/master/src/patch.h

void ReplaceFunction(uint32_t offset, void *func);
uint32_t GetRelativeCall(uint32_t base, uint32_t offset);
uint32_t GetAbsoluteValue(uint32_t base, uint32_t offset);
void ReplaceCall(uint32_t offset, void *func);
void ReplaceCallFunction(uint32_t offset, void* func);

inline DWORD IMAGE_BASE;
inline DWORD OPENGL_HANDLE;
inline const char* DIRECT_IO_EXPORT_DIR = "DEMASTER_EXP\\";
inline const std::string HEXT_PATH = std::string("HEXT\\");
inline DWORD DIRECT_IO_EXPORT_DIR_LEN = strlen(DIRECT_IO_EXPORT_DIR);

extern bx::DefaultAllocator texAllocator;

#define UINT unsigned int

void ApplyDirectIO();
void ApplyUVPatch();

void ApplyBattleCharacterPatch();
void ApplyFieldEntityPatch();
void ApplyBattleHookPatch();
void ApplyBattleMonsterPatch();
void ApplyBattleFieldPatch();
void ApplyFieldBackgroundPatch();
void ApplyWorldPatch();
DWORD GetFieldBackgroundReplacementExist();
void ZzzUnpack();

inline int lastWidth;
inline int lastHeight;
inline UCHAR* lastrgbBuffer;
inline UINT OPENGL_TEXTURES;

inline DWORD* tex_header;
inline DWORD* tex_struct;
inline DWORD* gl_textures;
inline DWORD pixelsPtr;
inline DWORD texturesPtr;
//Width and height (EDI, ESI) of the page the game is about to upload in _bsp; pixels are BGRA8
inline DWORD uploadWidth;
inline DWORD uploadHeight;
//[EBP-4] in the upload function: 2 = the game re-renders this world/battle page at 2x with a shader after upload
inline DWORD gameUpscaleFactor;

inline DWORD TEX_TYPE;

inline int currentMode;
inline std::string currentModeStr;

inline std::string LastFilePath;

static enum Modes
{
    MODE_NONE,
    MODE_FIELD = 2,
    MODE_MENU,
    MODE_MENU2,
    MODE_WORLDMAP,
    MODE_CARDGAME,
    MODE_BATTLE,
    MODE_BATTLESWIRL,
    MODE_INTRO,
    MODE_CD_CHANGE,
    MODE_CHANGE_TO_MAIN,
    MODE_MAIN,
    MODE_TITLE,
    MODE_UNKNOWN
};

DWORD WtpCheck();
void WtpGl();
//Fields: the left stick walks at any angle (texturepatch_v2_analogMove.cpp)
void ApplyAnalogMovementPatch();
//World map vehicles (texturepatch_v2_worldVehicles.cpp)
void ApplyWorldVehiclesPatch();
//textureIndex: GetTextureIndex() (the page has a wmset_XXX / wmset0_XXX file under it when it equals tPage)
bool WorldVehiclesBuildPage(DWORD tPage, int textureType, int textureIndex, int palette);
void WorldDumpPage(int textureType, DWORD tPage, int palette);
bool WorldVehiclesUploadPending();
void FbgGl();
//Field backgrounds from Field-Map-Editor's layers (texturepatch_v2_fieldLayers.cpp)
void ApplyFieldLayersPatch();
bool ApplyFieldBgResolution(); //false: FIELD_BG_RESOLUTION is 4 (or not 6/8) or unexpected code (DEMASTER's 4x stays)
bool FieldLayersBuildPage(DWORD tPage, int textureType, const DWORD* texHeader); //the upload's tex_header
bool FieldLayersUploadPending();
void FieldLayersBeforeDraw(int count, unsigned int type, const void* indices); //the draw's
void FieldLayersAfterDraw();
//Field characters and models with PBR backgrounds (texturepatch_v2_fieldChars.cpp): true when DEMASTER takes the draw (it
//draws the characters itself, before the next thing the game draws)
bool FieldCharsDraw(unsigned int mode, int count, unsigned int type, const void* indices);
void FieldCharsSwap(); //SwapBuffers
//FIELD_SMOOTH_SCROLL, FIELD_INTERPOLATION (texturepatch_v2_fieldTween.cpp), in SwapBuffers, in this order: nothing stays
//kept back; when the in-between picture is ready it goes on the screen (true: show it, then TweenShowFrame waits half a
//frame and puts the game's own back); after the game's picture was shown the frame is over (hdc: the window's)
void TweenEndOfDraws();
bool TweenShowBetween();
void TweenShowFrame();
void TweenFrameEnd(void* hdc);
void FieldCharsMapRead(bool pbrField); //game thread, a field's .map was read (with PBR files or not)
//Field model textures (texturepatch_v2_fieldModelMaps.cpp)
bool FieldModelTexImage(unsigned int texture, int width, int height, const void* data); //glTexImage2D of the game, after it
void FieldModelTexSubImage(unsigned int texture, int x, int y, int width, int height); //glTexSubImage2D of the game, after it
int FieldModelFilter(unsigned int texture, unsigned int name, int param); //glTexParameteri: the value to use
bool FieldModelIsBc7(unsigned int texture); //glTexSubImage2D of the game, before it: a model atlas that is BC7 (FIELD_MODEL_BC7)
bool FieldModelMaps(unsigned int texture, unsigned int& normal, unsigned int& orme); //the atlas's _N and _ORME (0: none)
//FIELD_FRAME_LOG (frame_log.cpp)
void FrameLogDraw(unsigned int mode, int count, unsigned int type, const void* indices);
void FrameLogSwap();
void FrameLogCreateHooks();
void FrameLogTextureFilled(unsigned int texture);
//SCENE_LIGHT (texturepatch_v2_sceneLight.cpp): battles and the world map
void SceneLightTextureFilled(unsigned int texture, bool whole); //the game (or a replacement of DEMASTER's) fills a texture
void SceneLightTextureForget(unsigned int texture); //the texture is not one of the game's pictures any more
void SceneLightStagePage(int stage, int page, const char* file); //DEMASTER's picture of a battle stage page is in the bound texture
void SceneLightFieldEntered(bool sky); //a field's .map is read
bool SceneLightBeforeDraw(unsigned int mode, int count, unsigned int type, const void* indices); //before every draw of the game (true: drawn)
void SceneLightFrameEnd(); //SwapBuffers
int SceneLightTimeHere(int mode); //for LUT: 0 the clock's time here, 1 always day, 2 always night, -1 SCENE_LIGHT off
int SceneLightMoment(); //the story's progress (the game's counter); -1: not known
//LUT (texturepatch_v2_lut.cpp): the time of day's colour grade, done by the shaders on the scene's draws
void LutTextureFilled(unsigned int texture, bool whole); //as SceneLightTextureFilled
void LutTextureForget(unsigned int texture);
void LutFieldEntered(const char* field, bool sky); //game thread, a field's .map is read
void LutBeforeDraw(); //before every draw of the game: the game's main.frag / main_char.frag graded or not
void LutUse(unsigned int program, bool scene); //one of DEMASTER's programs, in use: its draws are the scene (graded) or not
bool LutDrawAddsLight(); //the blend set adds light or takes it away (such draws are not graded)
void LutFrameEnd(); //SwapBuffers
void FrameLogNote(const char* format, ...); //a line about what DEMASTER did, in the frame being logged
//How long DEMASTER's field steps take: CPU time every frame (QueryPerformanceCounter; the frame log writes the average of
//the frames before the logged one), GPU time in the logged frame (timestamps between the steps)
enum FieldTimeSlot
{
	TIME_CHARS_READ,     //reading the game's character draws (the glDrawElements hook)
	TIME_CHARS_PREP,     //back into the room, smooth normals, which triangles are whose
	TIME_CHARS_PICTURES, //the pictures reflections use, the face pictures
	TIME_CHARS_SHADOWS,  //shadow maps
	TIME_CHARS_SCREEN,   //the characters on the game's screen
	TIME_CHARS_DEPTH,    //their depth copied for the background
	TIME_PBR_SETUP,      //setting up DEMASTER's field program around each PBR background draw
	TIME_LAYERS_HOOK,    //the field layers' glDrawElements hook (smoothing, the PBR setup inside it)
	TIME_SLOTS
};
long long FieldTimeNow();
void FieldTimeAdd(int slot, long long since); //the time from since (FieldTimeNow) to now goes to slot
void FrameLogMark(const char* label); //a GPU timestamp in the logged frame
void FieldLayersForgetTexture(unsigned int texture);
bool FieldLayersHasFiles(const char* fieldBgName);

void HookSetResolution();
inline DWORD oSetResolution;

inline DWORD parm1; //arg+8

inline DWORD* langIdent_ESI;

inline int currentStage;

inline server serverInst("127.0.0.1", 1337);

void MoreDebugLog(DWORD* a1, const char* pFormat, ...);
using oLogFunctionSig = void(__cdecl*)(DWORD*, const char*, ...);
inline oLogFunctionSig oLogFunction;

struct CapturedTextureInfo {
    GLuint id;
    int width;
    int height;
    GLint internalFormat;
    std::string estimatedFilename;
};

// A map to store all unique textures, using the GLuint ID as the key
extern std::map<GLuint, CapturedTextureInfo> g_capturedTextures;