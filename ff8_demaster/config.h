#pragma once

#include <string>

#define DEMASTER_CONF "demaster.conf"

inline int AUTOUNPACK;
inline int UVPATCH;
inline int DIRECT_IO;
inline int TEXTURE_PATCH;
inline int DEBUG_PATCH;
inline int IMGUI_DEBUG;
inline int LOG;
inline int BATTLE_CHARA;
inline int FIELD_ENTITY;
inline int BATTLE_HOOK;
inline int FIELD_BACKGROUND;
inline int WORLD_TEXTURES;
inline int LINEAR_PATCH;
inline int LINEAR_PATCH_MIN_FILTER; //also force nearest minification (FILTERPATCH2_VALUE)
inline int KEEP_HD_RESOLUTION;     //skip the game's 2x shader pass on world/battle pages DEMASTER replaced
inline int OPENGL_HOOK;
inline int FILL_ASPECT_RATIO;
inline int BATTLE_STAGE_FORCE_RELOAD;
inline int HASH_ENABLED;
inline int HASH_OUTPUT;
inline int HASH_LOAD_HD;
inline int SKIP_SPLASH;
inline int WIDESCREEN_EXPERIMENTAL; //Maki's "Initial aggresive widescreen support" (SetResolution hook + forced scissor)

inline int BATTLE_STAGE_ANIMATION_DELAY;
inline int HASH_OUTPUT_EXT;
inline int HASH_LOAD_HD_EXT;

inline int WINDOW_OVERWRITE;

inline int DEBUG_GLTEXIMAGE2D;

//World map (texturepatch_v2_world.cpp)
inline int WORLD_DEBUG;               //1: log every world texture page once (type, page, palette, size); 2: also save it as PNG
inline int WORLD_VEHICLES;            //world textures by name: vehicles, effects, water, select map (world_vehicles.h)
inline std::string REMASTER_WMSET014; //new name for Remastered's own HD world texture (empty = keep wmset_014_0)

//Field backgrounds from Field-Map-Editor's layers (texturepatch_v2_fieldLayers.cpp)
inline int FIELD_LAYERS;              //load field backgrounds from FME's deswizzled images (<field>_<pupu>.dds|png)
inline int FIELD_LAYERS_UNSHARE;      //give tiles that share a cell but have their own image a cell of their own
inline int FIELD_SMOOTH;              //smooth filtering inside each tile's cell (needs the main.frag from this release)
inline int FIELD_BG_RESOLUTION;       //buffer field backgrounds are drawn into: 4, 6 or 8 times the PlayStation size (4 = DEMASTER's)
inline int FIELD_LAYERS_LIVE;         //re-read layer images that change on disk while the game runs (checked once a second)
inline int FIELD_DEBUG;               //1: log what FIELD_LAYERS does per field and page; 2: also save every page as PNG
inline int FIELD_PBR;                 //light PBR layers (their own _A and an _LF next to the layer images) in the shader
inline int FIELD_FRAME_LOG;           //Ctrl+F11 writes down everything the game draws in the next frame (DEMASTER_EXP\frame_log\)
inline int FIELD_PBR_VIEW;            //0: lit; 1 albedo, 2 lightmap, 3 normals, 4 depth, 5 AO, 6 roughness, 7 metallic, 8 flicker mask, 9 added light, 10 lights at rest,
                                      //11 character light, 12 shadows, 13 reflections, 14 depth pushed back, 15 character normals
inline int FIELD_PBR_DEPTH;           //PBR tiles hide characters per pixel (from _ND), not per 16x16 tile
inline int FIELD_PBR_SHADOWS;         //character shadows from the field's lights, contact darkening, room shadows (replace the round ones)
inline int FIELD_PBR_REFLECTIONS;     //characters reflected in glossy and metal background surfaces
inline int FIELD_PBR_CHARACTERS;      //characters lit by the room (its light grid, the field's lights, their maps)
inline int FIELD_AVATARS;             //face pictures of the characters (Ctrl+F10 saves them to DEMASTER_EXP\avatars\)
inline int FIELD_PBR_QUALITY;         //0 low (integrated graphics), 1 medium (GTX 1060 class, the default), 2 high
//Day and night (day_night.cpp)
inline int DAY_NIGHT;                 //one day/night clock for the whole game; PBR fields follow it (or their _pbr.txt time)
inline double DAY_NIGHT_DAY;          //seconds of day in fields, the world map and menus (480)
inline double DAY_NIGHT_NIGHT;        //seconds of night (240)
inline double DAY_NIGHT_FADE;         //seconds a fade between them takes (40)
inline double DAY_NIGHT_BATTLE;       //battles run the clock this many times slower (3)
inline double DAY_NIGHT_EVENING;      //the share of the day after which the day's lights start going, the glows coming (0.9)
inline int DAY_NIGHT_SAVES;           //the time of day is kept per save (DEMASTER_EXP\daynight_saves.txt) (1)
inline std::string DAY_NIGHT_SAVE_FOLDER; //where the game writes its saves ("": Documents\My Games\FINAL FANTASY VIII Remastered)
inline int DAY_NIGHT_KEYS;            //Ctrl+F7 swap, Ctrl+F8 pause, Ctrl+F9 speed (1x, 10x, 60x), Ctrl+F6 to just before the next fade
inline int DAY_NIGHT_BAR;             //a thin bar across the top while the clock is paused or sped up (and after a key)
inline double FIELD_PBR_LOOK;         //the look's master dial: how far the maps change the painting itself (0: not at all; 1; up to 2)
inline double FIELD_PBR_RELIGHT;      //the look: the painted light follows the normal map (times FIELD_PBR_LOOK)
inline double FIELD_PBR_HIGHLIGHTS;   //the look: highlights from the painted lamps (times FIELD_PBR_LOOK)
inline double FIELD_PBR_AO;           //the look: the AO map darkens the painting too (times FIELD_PBR_LOOK)
inline int FIELD_MOVING_LIGHTS;       //the moving lights module: the sun and the party light
inline double FIELD_SUN;              //the sun's strength (fields with sky = 1, or [sun] on = 1 in their _lights.txt); 0: off
inline int FIELD_PARTY_LIGHT;         //the light the party leader carries: 0 off, 1 at night, 2 always
inline double FIELD_PARTY_LIGHT_STRENGTH;
//Movement and motion (texturepatch_v2_analogMove.cpp, texturepatch_v2_fieldTween.cpp)
inline int SCENE_LIGHT;               //battles and the world map: the time of day, scene_light.txt, stage pages' _EC and _ORM maps
inline int SCENE_DEBUG;               //log what SCENE_LIGHT decides, and the projection's registers at a stage draw
inline int ANALOG_MOVEMENT;           //fields: the pad's left stick walks at any angle (the game's own analogue code, answered for it)
inline double ANALOG_DEADZONE;        //how far the stick moves before it counts (0.05-0.9 of its throw)
inline int ANALOG_WORLD_MAP;          //the world map: the left stick moves at any angle too
inline double MOVE_TURN_SPEED;        //fields: degrees a second the walking direction swings towards the one held (d-pad, keys, stick); 0: at once, as the game does
inline int FIELD_SMOOTH_SCROLL;       //field layers and characters at the scroll's unrounded place (no more whole-pixel steps)
inline int FIELD_INTERPOLATION;       //an in-between frame after every field frame (60 pictures a second from the game's 30); 0: off
inline int FIELD_GL_TRACK;            //1: remember the GL state as it is set instead of asking the driver around every draw (gl_state.h)

void ReadConfigFile();