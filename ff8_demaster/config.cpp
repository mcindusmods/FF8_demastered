#include "config.h"
#include <INIReader.h>
#include <cctype>
#include "debug.h"
#include "GLFW/include/GLFW/glfw3.h"

void ReadConfigFile()
{
    if (GetFileAttributesA(DEMASTER_CONF) == INVALID_FILE_ATTRIBUTES)
    {
        OutputDebug("File " DEMASTER_CONF " not found- all failed\n");
        return;
    }
    OutputDebug("Reading config file " DEMASTER_CONF "\n");
    //ini_t* conf = ini_load(DEMASTER_CONF);

    const INIReader conf(DEMASTER_CONF);

    AUTOUNPACK = conf.GetInteger("", "AUTO_UNPACK", 0);
    UVPATCH = conf.GetInteger("", "UV_PATCH", 0);
    DIRECT_IO = conf.GetInteger("", "DIRECT_IO", 0);
    LOG = conf.GetInteger("", "LOG", 0);
    BATTLE_CHARA = conf.GetInteger("", "BATTLE_CHARACTER", 0);
    FIELD_ENTITY = conf.GetInteger("", "FIELD_ENTITY", 0);
    BATTLE_HOOK = conf.GetInteger("", "BATTLE_HOOK_MONSTER_FIELDS", 0);
    FIELD_BACKGROUND = conf.GetInteger("", "FIELD_BACKGROUND", 0);
    WORLD_TEXTURES = conf.GetInteger("", "WORLD_TEXTURES", 0);
    TEXTURE_PATCH = conf.GetInteger("", "TEXTURE_PATCH", 1); //this one lacks actual demaster.conf so default to 1
    LINEAR_PATCH = conf.GetInteger("", "LINEAR_PATCH", 1);
    LINEAR_PATCH_MIN_FILTER = conf.GetInteger("", "LINEAR_PATCH_MIN_FILTER", 0);
    KEEP_HD_RESOLUTION = conf.GetInteger("", "KEEP_HD_RESOLUTION", 1);
    DEBUG_PATCH = conf.GetInteger("", "DEBUG_PATCH", 0);
    OPENGL_HOOK = conf.GetInteger("", "OPENGL_HOOK", 0);
    FILL_ASPECT_RATIO = conf.GetInteger("", "FILL_ASPECT_RATIO", 0);
    BATTLE_STAGE_ANIMATION_DELAY = conf.GetInteger("", "BATTLE_STAGE_ANIMATION_DELAY", 100);
    BATTLE_STAGE_FORCE_RELOAD = conf.GetInteger("", "BATTLE_STAGE_FORCE_RELOAD", 0);
	HASH_ENABLED = conf.GetInteger("", "HASH_ENABLED", 1);
    HASH_OUTPUT = conf.GetInteger("", "HASH_OUTPUT", 0);
    HASH_OUTPUT_EXT = conf.GetInteger("", "HASH_OUTPUT_EXT", 0);
    HASH_LOAD_HD = conf.GetInteger("", "HASH_LOAD_HD", 1);
    HASH_LOAD_HD_EXT = conf.GetInteger("", "HASH_LOAD_HD_EXT", 1);
    IMGUI_DEBUG = conf.GetInteger("", "IMGUI_DEBUG", 0);
    SKIP_SPLASH = conf.GetInteger("", "SKIP_SPLASH", 0);
    WIDESCREEN_EXPERIMENTAL = conf.GetInteger("", "WIDESCREEN_EXPERIMENTAL", 0);
    DEBUG_GLTEXIMAGE2D = conf.GetInteger("", "DEBUG_GLTEXIMAGE2D", 0);
    
    WINDOW_OVERWRITE = conf.GetInteger("", "WINDOW_OVERWRITE", 0);

    WORLD_DEBUG = conf.GetInteger("", "WORLD_DEBUG", 0);
    WORLD_VEHICLES = conf.GetInteger("", "WORLD_VEHICLES", 0);
    REMASTER_WMSET014 = conf.Get("", "REMASTER_WMSET014", "");

    FIELD_LAYERS = conf.GetInteger("", "FIELD_LAYERS", 0);
    FIELD_LAYERS_UNSHARE = conf.GetInteger("", "FIELD_LAYERS_UNSHARE", 1);
    FIELD_SMOOTH = conf.GetInteger("", "FIELD_SMOOTH", 1);
    FIELD_BG_RESOLUTION = conf.GetInteger("", "FIELD_BG_RESOLUTION", 4);
    FIELD_LAYERS_LIVE = conf.GetInteger("", "FIELD_LAYERS_LIVE", 1);
    FIELD_DEBUG = conf.GetInteger("", "FIELD_DEBUG", 0);
    FIELD_PBR = conf.GetInteger("", "FIELD_PBR", 1);
    FIELD_PBR_VIEW = conf.GetInteger("", "FIELD_PBR_VIEW", 0);
    FIELD_PBR_DEPTH = conf.GetInteger("", "FIELD_PBR_DEPTH", 1);
    FIELD_PBR_SHADOWS = conf.GetInteger("", "FIELD_PBR_SHADOWS", 1);
    FIELD_PBR_REFLECTIONS = conf.GetInteger("", "FIELD_PBR_REFLECTIONS", 1);
    FIELD_PBR_CHARACTERS = conf.GetInteger("", "FIELD_PBR_CHARACTERS", 1);
    FIELD_AVATARS = conf.GetInteger("", "FIELD_AVATARS", 0);
    {
        //low, medium, high (or 0, 1, 2)
        std::string quality = conf.Get("", "FIELD_PBR_QUALITY", "medium");
        for (char& c : quality)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        FIELD_PBR_QUALITY = quality == "low" || quality == "0" ? 0 : quality == "high" || quality == "2" ? 2 : 1;
    }
    FIELD_GL_TRACK = conf.GetInteger("", "FIELD_GL_TRACK", 1);
    FIELD_PBR_LOOK = conf.GetReal("", "FIELD_PBR_LOOK", 0.0);
    FIELD_PBR_RELIGHT = conf.GetReal("", "FIELD_PBR_RELIGHT", 0.5);
    FIELD_PBR_HIGHLIGHTS = conf.GetReal("", "FIELD_PBR_HIGHLIGHTS", 0.5);
    FIELD_PBR_AO = conf.GetReal("", "FIELD_PBR_AO", 0.5);
    FIELD_MOVING_LIGHTS = conf.GetInteger("", "FIELD_MOVING_LIGHTS", 1);
    FIELD_SUN = conf.GetReal("", "FIELD_SUN", 1.0);
    FIELD_PARTY_LIGHT = conf.GetInteger("", "FIELD_PARTY_LIGHT", 1);
    FIELD_PARTY_LIGHT_STRENGTH = conf.GetReal("", "FIELD_PARTY_LIGHT_STRENGTH", 1.0);
    DAY_NIGHT = conf.GetInteger("", "DAY_NIGHT", 1);
    DAY_NIGHT_DAY = conf.GetReal("", "DAY_NIGHT_DAY", 480);
    DAY_NIGHT_NIGHT = conf.GetReal("", "DAY_NIGHT_NIGHT", 240);
    DAY_NIGHT_FADE = conf.GetReal("", "DAY_NIGHT_FADE", 40);
    DAY_NIGHT_BATTLE = conf.GetReal("", "DAY_NIGHT_BATTLE", 3);
    DAY_NIGHT_KEYS = conf.GetInteger("", "DAY_NIGHT_KEYS", 1);
    DAY_NIGHT_BAR = conf.GetInteger("", "DAY_NIGHT_BAR", 1);
    FIELD_FRAME_LOG = conf.GetInteger("", "FIELD_FRAME_LOG", 0);
    ANALOG_MOVEMENT = conf.GetInteger("", "ANALOG_MOVEMENT", 1);
    ANALOG_DEADZONE = conf.GetReal("", "ANALOG_DEADZONE", 0.25);
    ANALOG_WORLD_MAP = conf.GetInteger("", "ANALOG_WORLD_MAP", 1);
    MOVE_TURN_SPEED = conf.GetReal("", "MOVE_TURN_SPEED", 720.0);
    FIELD_SMOOTH_SCROLL = conf.GetInteger("", "FIELD_SMOOTH_SCROLL", 1);
    FIELD_INTERPOLATION = conf.GetInteger("", "FIELD_INTERPOLATION", 0);
}
