#pragma once
//See address table at:
//https://docs.google.com/spreadsheets/d/1hFGSoQNbamIrqmhBwr8B9RvD43KO140Zyb5eiWm3O_4/edit?usp=sharing

#include <iostream>
#include <vector>
#include <map>


#pragma warning (disable : 26812 ) //this makes little sense, I'm ignoring it
enum ADDRESS_NAME
{
   UVPATCH0, UVPATCH1, UVPATCH2, UVPATCH3, UVPATCH4, UVPATCH5, UVPATCH6, UVPATCH7, UVPATCH8, UVPATCH9, UVPATCH10, UVPATCH11, UVPATCH12,
   UVFUNCPNTR, WORLDGETTEXIDX0, WORLDGETTEXIDX1, WORLDGETTEXIDX2, WORLDGETTEXIDX3, FCPBACKADD1, FIELDCHARENT1, FIELDCHARENT2, FCPBACKADD2,
   BGFILENAME1, BGFILENAME2, CHECKTEXREPAVAIL, _ASM_FIELDBGRETADDR3, DISABLETPAGELIMIT, _ASM_FIELDBGRETADDR2, DS_FREE, DS_TEXIMG,
   _BSPBACKADD1, _BSPBACKADD2, BATTLEJMPPATCH1, BATTLEJMPPATCH2, BTLMON0, BTLMON1, BTLMON2, BTLMON3, BTLMON4, BTLMON5, BTLMON6, BHP, BHP_ATLAS,
   _BHPBACKADD1, _BHPBACKADD2, _BHPBACKADD3, NEWGLTEX_CHARA, BCPBACKADD1, BCPBACKADD2, BCPBACKADD3, CLTBACKADD1, CLTBACKADD2, UPLOADVRAM, IOFUNC1,
   IOFUNC2, IOFUNC3, IOFUNC4, IOFUNC5, IOFUNC6, FILTERPATCH1, FILTERPATCH2, FILTERPATCH3, FILTERPATCH4, WINDOWTITLE, NULLSUB_DEBUG,
BGRESPATCH1, GLFWINIT, GLFWWINDOWHINT, CHANGEMODE, STEAMADDR, FIELD_CHAR_BLINK, SUB_116059B0, LOAD_BATTLE_EXT, SKIP_SLASHSCREEN,
	/*SUB_BATTLESWIRL_SLEEP*/MORE_DEBUG_LOG, SET_RESOLUTION, FILTERPATCH2_VALUE, WMSET014_PATH_PUSH,
	GUEST_STACK_BASE, GUEST_PAGE_TABLE, GUEST_LOADIMAGE, WMSET_GUEST, WM_CHARA_READ_CALL, WM_READ_FILE, FIELD_READ_FILE,
	PSX_VRAM, READ_VRAM_PALETTE, WRITE_PALETTE,
	FIELD_CULL_TRI, FIELD_CULL_QUAD, FIELD_ENTITIES, FIELD_ENTITY_COUNT, FIELD_BG_GROUPS,
	ANALOG_GET_VALUE, ANALOG_FIELD_TEST, ANALOG_FIELD_X, ANALOG_FIELD_Y,
	ANALOG_WORLD_FIRST, WM_KEYS, WM_KEY_INDEX, FIELD_PLAYER_ENTITY,
	ADDRESS_NAME_COUNT //keep last
};

enum ADDRESS_NAME_STEAM
{
	Magic_GetIDLoad_107FDF20,
};

// Which game DLL we were loaded into. Detected at runtime in DllMain (FFVIII_EFIGS.dll or FFVIII_JP.dll),
// so one ff8_demaster.dll serves both languages. Defining JAPANESE_PATCH=1 at compile time only changes the
// preferred module that is probed first; it no longer selects a different address table at compile time.
enum class GameLanguage
{
	Unknown,
	EFIGS,
	Japanese
};
inline GameLanguage g_gameLanguage = GameLanguage::Unknown;
inline bool IsJapanese() { return g_gameLanguage == GameLanguage::Japanese; }

void InitTable(unsigned int baseAddress);
//Forcing _stdcall for assembler PUSH based CALLS for EAX as return value and stack cleaned by function
//Returns 0 (and logs once) when the address is not known for the running game language/version.
//Callers that patch code must check HasAddress() first for anything that is not in both tables.
unsigned int _stdcall GetAddress(const ADDRESS_NAME name);
unsigned int _stdcall GetAddressBase(const ADDRESS_NAME name);
unsigned int _stdcall GetAddressFromSteam(const ADDRESS_NAME_STEAM name);
bool HasAddress(ADDRESS_NAME name);
const char* GetAddressName(ADDRESS_NAME name);

#define FORCEGAME 3

//doing enum and address hardcoding at once? any ideas? also no double declaration, hmm...
//Use Excel for auto-generating such lines- no one is going to type this by hand lol

// FFVIII_EFIGS.dll (English, French, German, Italian, Spanish)
const std::map<ADDRESS_NAME, std::vector<unsigned int>> PATCHADDRESS_EFIGS =
{
   //KEY-ENUM		//VANILLA, 1010, 1020, 1030 (FORCEGAME = 3)
	{ _ASM_FIELDBGRETADDR2 , std::vector<unsigned int>{ 0x0       , 0x1606540 , 0x1606e20 ,0x1606A20,} },
	{ _ASM_FIELDBGRETADDR3 , std::vector<unsigned int>{ 0x0       , 0x1591B75 , 0x1592185 ,0x1591EA5,} },
	{ _BHPBACKADD1         , std::vector<unsigned int>{ 0x0       , 0x157DA5D , 0x157decd ,0x157DFBD,} },
	{ _BHPBACKADD2         , std::vector<unsigned int>{ 0x0       , 0x157DD05 , 0x157e175 ,0x157E265,} },
	{ _BSPBACKADD1         , std::vector<unsigned int>{ 0x0       , 0x1573AFF , 0x1573f7f ,0x1573F4F,} },
	{ _BSPBACKADD2         , std::vector<unsigned int>{ 0x0       , 0x1573B54 , 0x1573fd4 ,0x1573FA4,} },
	{ BATTLEJMPPATCH1      , std::vector<unsigned int>{ 0x0       , 0x156CED4 , 0x156d354 ,0x156D324,} },
	{ BATTLEJMPPATCH2      , std::vector<unsigned int>{ 0x0       , 0x156D30B , 0x156d78b ,0x156D75B,} },
	{ BCPBACKADD1          , std::vector<unsigned int>{ 0x0       , 0x16066BD , 0x1606f9d ,0x1606B9D,} },
	{ BCPBACKADD2          , std::vector<unsigned int>{ 0x0       , 0x1604087 , 0x1604967 ,0x1604567,} },
	{ BCPBACKADD3          , std::vector<unsigned int>{ 0x0       , 0x1605040 , 0x1605920 ,0x1605520,} },
	{ BGFILENAME1          , std::vector<unsigned int>{ 0x0       , 0x1782140 , 0x178b168 ,0x1782160,} },
	{ BGFILENAME2          , std::vector<unsigned int>{ 0x0       , 0x189559C , 0x18955bc ,0x18955BC,} },
	{ BGRESPATCH1          , std::vector<unsigned int>{ 0x0       , 0x0		 , 0x0		 ,0x156E2C7,} },
	{ BHP                  , std::vector<unsigned int>{ 0x0       , 0x16482A8 , 0x1648d78 ,0x1648938,} },
	{ BHP_ATLAS            , std::vector<unsigned int>{ 0x0       , 0x0		 , 0x0		 ,0x15FF640,} },
	{ BTLMON0              , std::vector<unsigned int>{ 0x0       , 0x15FF3D0 , 0x15ff9c0 ,0x15FF6F0,} },
	{ BTLMON1              , std::vector<unsigned int>{ 0x0       , 0x15FF3DA , 0x15ff9ca ,0x15FF6FA,} },
	{ BTLMON2              , std::vector<unsigned int>{ 0x0       , 0x15FF3F9 , 0x15ff9e9 ,0x15FF719,} },
	{ BTLMON3              , std::vector<unsigned int>{ 0x0       , 0x15FF400 , 0x15ff9f0 ,0x15FF720,} },
	{ BTLMON4              , std::vector<unsigned int>{ 0x0       , 0x1601E02 , 0x16026e2 ,0x16022E2,} },
	{ BTLMON5              , std::vector<unsigned int>{ 0x0       , 0x1601E06 , 0x16026e6 ,0x16022E6,} },
	{ BTLMON6              , std::vector<unsigned int>{ 0x0       , 0x1601CFE , 0x16025de ,0x16021DE,} },
	{ CHECKTEXREPAVAIL     , std::vector<unsigned int>{ 0x0       , 0x1782080 , 0x178b0a0 ,0x17820A0,} },
	{ CLTBACKADD1          , std::vector<unsigned int>{ 0x0       , 0x155CD05 , 0x155d0b5 ,0x155D1A5,} },
	{ CLTBACKADD2          , std::vector<unsigned int>{ 0x0       , 0x155CD7A , 0x155d12a ,0x155D21A,} },
	{ DISABLETPAGELIMIT    , std::vector<unsigned int>{ 0x0       , 0x1606595 , 0x1606e75 ,0x1606A75,} },
	{ DS_FREE              , std::vector<unsigned int>{ 0x0       , 0x166b2a8 , 0x166b2a8 ,0x166B2A8,} },
	{ DS_TEXIMG            , std::vector<unsigned int>{ 0x0       , 0x166b4a0 , 0x166b4b4 ,0x166B4AC,} },
	{ FCPBACKADD1          , std::vector<unsigned int>{ 0x0       , 0x16061CC , 0x1606aac ,0x16066AC,} },
	{ FCPBACKADD2          , std::vector<unsigned int>{ 0x0       , 0x160C4AD , 0x160cd8d ,0x160C94D,} },
	{ FIELDCHARENT1        , std::vector<unsigned int>{ 0x0       , 0x160C43A , 0x160cd1c ,0x160C8DA,} },
	{ FIELDCHARENT2        , std::vector<unsigned int>{ 0x0       , 0x160C467 , 0x160cd47 ,0x160C907,} },
	{ FILTERPATCH1         , std::vector<unsigned int>{ 0x0       , 0x15693EF , 0x156987f ,0x156985F,} },
	//FILTERPATCH2 points at "push GL_TEXTURE_MIN_FILTER" (the parameter name), not at the GL_LINEAR value 5 bytes
	//earlier, so LINEAR_PATCH turns that call into an invalid glTexParameteri. Kept as is so the look does not
	//change; FILTERPATCH2_VALUE is the value push, used when LINEAR_PATCH_MIN_FILTER=1 (checked on 1.0.3.0).
	{ FILTERPATCH2         , std::vector<unsigned int>{ 0x0       , 0x1569409 , 0x1569899 ,0x1569879,} },
	{ FILTERPATCH2_VALUE   , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x1569874,} },
	//push "FIELD.FS\field_hd_new\wmset_014_0" in Remastered's HD loader (type 18 case), see REMASTER_WMSET014
	{ WMSET014_PATH_PUSH   , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x1606AD4,} },
	//World map textures by name (WORLD_VEHICLES). Remastered runs the 2000 PC game code as a static recompilation: every
	//original function exists as a host function taking the emulated CPU state, and a table at RVA 0x16EED10 maps the
	//original (US 2000) addresses to them. Guest data is reached through GUEST_PAGE_TABLE (DWORD per 4 KB page), the
	//guest stack through the flat base stored at GUEST_STACK_BASE.
	//  GUEST_LOADIMAGE:    host function of 0x45BD30, LoadImage(RECT*, data): every copy of texels into VRAM (hooked at entry)
	//  WMSET_GUEST:        guest address (not an RVA) where the world map keeps wmsetXX.obj (0x542DA0 reads its 48 sections there)
	//  WM_CHARA_READ_CALL: E8 call in 0x653410 (world chara.one loader) -> WM_READ_FILE = 0x52D380 (read)
	{ GUEST_STACK_BASE     , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x16CB5E0,} },
	{ GUEST_PAGE_TABLE     , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x188EDD0,} },
	{ GUEST_LOADIMAGE      , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x021B5C0,} },
	{ WMSET_GUEST          , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x1E9DC3C,} },
	{ WM_CHARA_READ_CALL   , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x0D7CFB1,} },
	{ WM_READ_FILE         , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x086F1B0,} },
	//Field backgrounds from layers (FIELD_LAYERS): host function of 0x52D400, sm_pc_read(filename, buffer), every file the
	//game reads; read_field_data (0x471010, the caller of Remastered's hook_get_field_ext) calls it 16 times, the .map among them
	{ FIELD_READ_FILE      , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x086F400,} },
	//FIELD_LAYERS: guest address (not an RVA) of the game's PlayStation VRAM, 1024x512 16-bit (psxvram_buffer; read_vram_palette
	//0x467370 reads a CLUT at PSX_VRAM + y * 2048 + x * 2)
	{ PSX_VRAM             , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x1B47818,} },
	//FIELD_LAYERS: which CLUT the game loads into each palette slot of a texture. Hosts of read_vram_palette(clut, bgra, size)
	//0x467370 and write_palette_to_driver(src_offset, size, rgba, dest_offset, texture_set) 0x467310 (FFNx's names), both
	//called only from write_palette_texture_set 0x466190
	{ READ_VRAM_PALETTE    , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x024B5E0,} },
	{ WRITE_PALETTE        , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x024B450,} },
	//ANALOG_MOVEMENT: the host of 0x49F0A0 (the pad's analogue value: port, which stick axis, port; -1 when the pad record is
	//not an analogue pad's, which is always so in Remastered) and the three E8 calls to it in the host of 0x4789A0 (the
	//field's movement from the pad, FFNx's sub_4789A0): "is there a stick" (-1: walk by the d-pad bits), left X, left Y
	{ ANALOG_GET_VALUE     , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x031EB50,} },
	{ ANALOG_FIELD_TEST    , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x0291659,} },
	{ ANALOG_FIELD_X       , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x02918CB,} },
	{ ANALOG_FIELD_Y       , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x0291927,} },
	//the world map's input (the host of 0x559240, FFNx's worldmap_input_update_sub_559240) asks for all four axes of both
	//pads, eight E8 calls within 0x240 bytes: this is the first (pad 1, left X). Guest addresses (not RVAs): the world
	//map's key states (a DWORD per entry, d-pad in bits 12-15) and the s16 index of the entry this frame's keys go into
	{ ANALOG_WORLD_FIRST   , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x0929854,} },
	{ WM_KEYS              , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x203FDE8,} },
	{ WM_KEY_INDEX         , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x20409BC,} },
	//guest address: the script entity the player moves (s16; the field's movement code gives the pad to the entity whose
	//index this is)
	{ FIELD_PLAYER_ENTITY  , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x1CD8FD0,} },
	//FIELD_PBR_*: the back-face cull in the field model code, two 6-byte jumps (jz / jnz rel32) that skip a polygon facing away:
	//in the hosts of 0x533920 (triangles, its own cross product) and 0x533A90 (quads, NCLIP 0x45EE10)
	{ FIELD_CULL_TRI       , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x0889ED2,} },
	{ FIELD_CULL_QUAD      , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x088A4F6,} },
	//guest addresses (not RVAs): the pointer to the field's script entities (612 bytes each: position s32 x 3 >> 12 at 0x190,
	//model 0x218, facing 0x241, shade 0x259-0x261) and their count (byte); the background light groups (BGANIME/BGSHADE
	//state per group: R, G, B (128 = as painted), the frame shown)
	{ FIELD_ENTITIES       , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x1D9CF88,} },
	{ FIELD_ENTITY_COUNT   , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x1D9D019,} },
	{ FIELD_BG_GROUPS      , std::vector<unsigned int>{ 0x0       , 0x0       , 0x0       ,0x1A77238,} },
	{ FILTERPATCH3         , std::vector<unsigned int>{ 0x0       , 0x156A348 , 0x156a7d8 ,0x156A7B8,} },
	{ FILTERPATCH4         , std::vector<unsigned int>{ 0x0       , 0x156A359 , 0x156a7e9 ,0x156A7C9,} },
	{ IOFUNC1              , std::vector<unsigned int>{ 0x15D41EB , 0x364CC   , 0x364cc ,0x364CC,} },
	{ IOFUNC2              , std::vector<unsigned int>{ 0x15D4797 , 0x36B37   , 0x36B37   ,0x36B37,} },
	{ IOFUNC3              , std::vector<unsigned int>{ 0x15D47D8 , 0x36B78   , 0x36b78   ,0x36b78,} },
	{ IOFUNC4              , std::vector<unsigned int>{ 0x15D4947 , 0x36D37   , 0x36d37   ,0x36d37,} },
	{ IOFUNC5              , std::vector<unsigned int>{ 0x15D42B7 , 0x365F8   , 0x365f8   ,0x365F8,} },
	{ IOFUNC6              , std::vector<unsigned int>{ 0x15D42B7 , 0x3656A   , 0x3656a   ,0x3656A,} },
	{ NEWGLTEX_CHARA       , std::vector<unsigned int>{ 0x0       , 0x160b670 , 0x160bf50 ,0x160BB50,} },
	{ NULLSUB_DEBUG        , std::vector<unsigned int>{ 0x0       , 0x0       , 0x33DC0 ,0x33DC0,} },
	{ UPLOADVRAM           , std::vector<unsigned int>{ 0x0       , 0x1573A40 , 0x1573ec0 ,0x1573E90,} },
	{ UVFUNCPNTR           , std::vector<unsigned int>{ 0x8A2CB2  , 0x911A05  , 0x912725  ,0x9113B5,} },
	{ UVPATCH0             , std::vector<unsigned int>{ 0x8A2CB2  , 0x911A05  , 0x912725  ,0x9113B5,} },
	{ UVPATCH1             , std::vector<unsigned int>{ 0x8A2CB8  , 0x911A0B  , 0x91272b  ,0x9113bb,} },
	{ UVPATCH10            , std::vector<unsigned int>{ 0x8A2E6F  , 0x911BC6  , 0x9128e6  ,0x911576,} },
	{ UVPATCH11            , std::vector<unsigned int>{ 0x8A2E75  , 0x911BCC  , 0x9128ec  ,0x91157C,} },
	{ UVPATCH2             , std::vector<unsigned int>{ 0x8A2D17  , 0x911A67  , 0x912787  ,0x911417,} },
	{ UVPATCH3             , std::vector<unsigned int>{ 0x8A2D1D  , 0x911A6D  , 0x91278d  ,0x91141d,} },
	{ UVPATCH4             , std::vector<unsigned int>{ 0x8A2D6D  , 0x911AC4  , 0x9127e4  ,0x911474,} },
	{ UVPATCH5             , std::vector<unsigned int>{ 0x8A2D73  , 0x911ACA  , 0x9127ea  ,0x91147A,} },
	{ UVPATCH6             , std::vector<unsigned int>{ 0x8A2DC5  , 0x911B23  , 0x912843  ,0x9114D3,} },
	{ UVPATCH7             , std::vector<unsigned int>{ 0x8A2DCB  , 0x911B29  , 0x912849  ,0x9114D9,} },
	{ UVPATCH8             , std::vector<unsigned int>{ 0x8A2E1B  , 0x911B80  , 0x9128a0  ,0x911530,} },
	{ UVPATCH9             , std::vector<unsigned int>{ 0x8A2E21  , 0x911B86  , 0x9128a6  ,0x911536,} },
	{ WINDOWTITLE          , std::vector<unsigned int>{ 0x0       , 0x1601065 , 0x1601935 ,0x1601535,} },
	{ WORLDGETTEXIDX0      , std::vector<unsigned int>{ 0x0       , 0x17424B0 , 0x17424b0 ,0x17424B0,} },
	{ WORLDGETTEXIDX1      , std::vector<unsigned int>{ 0x0       , 0x17424B4 , 0x17424b4 ,0x17424B4,} },
	{ WORLDGETTEXIDX2      , std::vector<unsigned int>{ 0x0       , 0x17424B8 , 0x17424b8 ,0x17424B8,} },
	{ WORLDGETTEXIDX3      , std::vector<unsigned int>{ 0x0       , 0x17424BC , 0x17424bc ,0x17424Bc,} },
	{ CHANGEMODE      , std::vector<unsigned int>{ 0x0       , 0x0 , 0x0 ,0x1600EB0,} },
	{ STEAMADDR      , std::vector<unsigned int>{ 0x0       , 0x0 , 0x0 ,0x16EED10,} },
	{ FIELD_CHAR_BLINK      , std::vector<unsigned int>{ 0x0       , 0x0 , 0x0 ,0x160594F,} },
	{ SUB_116059B0      , std::vector<unsigned int>{ 0x0       , 0x0 , 0x0 ,0x16059B0,} }, 
	{ LOAD_BATTLE_EXT      , std::vector<unsigned int>{ 0x0       , 0x0 , 0x0 ,0x1604610,} },
	{ SKIP_SLASHSCREEN      , std::vector<unsigned int>{ 0x0       , 0x0 , 0x0 ,0x178206F,} },
	{ MORE_DEBUG_LOG      , std::vector<unsigned int>{ 0x0       , 0x0 , 0x0 ,0x351D0,} },
	{ SET_RESOLUTION, std::vector<unsigned int>{ 0x0       , 0x0 , 0x0 ,0x2190D0,} },
	//{ SUB_BATTLESWIRL_SLEEP      , std::vector<unsigned int>{ 0x0       , 0x0 , 0x0 ,0x1581790,} },
};

// FFVIII_JP.dll. Checked against FFVIII_JP.dll 1.0.3.0 (sha256 36201046...d057) with tools/verify_addresses.py and
// tools/port_addresses.py (EFIGS -> JP port reproduces 55 of these entries exactly). Changes from Maki's table:
//   FIELDCHARENT1 0x163E2CC -> 0x163E2CA (old value is a "push esi"; patching it corrupted code)
//   SUB_116059B0  0x1638422 -> 0x1637310 (function start; the JP offsets 0x130F/0x1320 now hit the 0x300 operands)
//   added SET_RESOLUTION, SKIP_SLASHSCREEN, BHP_ATLAS, MORE_DEBUG_LOG, FILTERPATCH2_VALUE, WMSET014_PATH_PUSH
//   world textures: the JP DLL recompiles the JP 2000 exe (other guest addresses: LoadImage 0x45BD30 -> 0x45C760,
//   0x52D380 -> 0x531240, wmset kept at 0x23A1D1C by 0x546E80), found through the same renderer calls
// An address missing for a language makes GetAddress() return 0 and the feature is skipped.
const std::map<ADDRESS_NAME, std::vector<unsigned int>> PATCHADDRESS_JP =
{
{NULLSUB_DEBUG        , std::vector<unsigned int>{ 0x0 , 0x0 , 0x33E50   , 0x33E50   }},
{IOFUNC1              , std::vector<unsigned int>{ 0x0 , 0x0 , 0x3649C   , 0x3649C   }},
{IOFUNC6              , std::vector<unsigned int>{ 0x0 , 0x0 , 0x3653A   , 0x3653A   }},
{IOFUNC5              , std::vector<unsigned int>{ 0x0 , 0x0 , 0x365C8   , 0x365C8   }},
{IOFUNC2              , std::vector<unsigned int>{ 0x0 , 0x0 , 0x36B07   , 0x36B07   }},
{IOFUNC3              , std::vector<unsigned int>{ 0x0 , 0x0 , 0x36B48   , 0x36B48   }},
{IOFUNC4              , std::vector<unsigned int>{ 0x0 , 0x0 , 0x36D07   , 0x36D07   }},
{UVFUNCPNTR           , std::vector<unsigned int>{ 0x0 , 0x0 , 0x9452CC  , 0x94A8FC  }},
{UVPATCH0             , std::vector<unsigned int>{ 0x0 , 0x0 , 0x9452CC  , 0x94A8FC  }},
{UVPATCH1             , std::vector<unsigned int>{ 0x0 , 0x0 , 0x9452D2  , 0x94A902  }},
{UVPATCH2             , std::vector<unsigned int>{ 0x0 , 0x0 , 0x94532E  , 0x94A95E  }},
{UVPATCH3             , std::vector<unsigned int>{ 0x0 , 0x0 , 0x945334  , 0x94A964  }},
{UVPATCH4             , std::vector<unsigned int>{ 0x0 , 0x0 , 0x94538D  , 0x94A9BD  }},
{UVPATCH5             , std::vector<unsigned int>{ 0x0 , 0x0 , 0x945393  , 0x94A9C3  }},
{UVPATCH6             , std::vector<unsigned int>{ 0x0 , 0x0 , 0x9453EC  , 0x94AA1C  }},
{UVPATCH7             , std::vector<unsigned int>{ 0x0 , 0x0 , 0x9453F2  , 0x94AA22  }},
{UVPATCH8             , std::vector<unsigned int>{ 0x0 , 0x0 , 0x94544B  , 0x94AA7B  }},
{UVPATCH9             , std::vector<unsigned int>{ 0x0 , 0x0 , 0x945451  , 0x94AA81  }},
{UVPATCH10            , std::vector<unsigned int>{ 0x0 , 0x0 , 0x945491  , 0x94AAC1  }},
{UVPATCH11            , std::vector<unsigned int>{ 0x0 , 0x0 , 0x945497  , 0x94AAC7  }},
{CLTBACKADD1          , std::vector<unsigned int>{ 0x0 , 0x0 , 0x1593985 , 0x15938D5 }},
{CLTBACKADD2          , std::vector<unsigned int>{ 0x0 , 0x0 , 0x15939FA , 0x159394A }},
{FILTERPATCH1         , std::vector<unsigned int>{ 0x0 , 0x0 , 0x159FFBF , 0x159FE5F }},
{FILTERPATCH2         , std::vector<unsigned int>{ 0x0 , 0x0 , 0x159FFD9 , 0x159FE79 }},
{FILTERPATCH3         , std::vector<unsigned int>{ 0x0 , 0x0 , 0x15A0EE6 , 0x15A0D86 }},
{FILTERPATCH4         , std::vector<unsigned int>{ 0x0 , 0x0 , 0x15A0EF7 , 0x15A0D97 }},
{BATTLEJMPPATCH1      , std::vector<unsigned int>{ 0x0 , 0x0 , 0x15A3F14 , 0x15A3DB4 }},
{BATTLEJMPPATCH2      , std::vector<unsigned int>{ 0x0 , 0x0 , 0x15A434B , 0x15A41EB }},
{UPLOADVRAM           , std::vector<unsigned int>{ 0x0 , 0x0 , 0x15AAAB0 , 0x15AA930 }},
{_BSPBACKADD1         , std::vector<unsigned int>{ 0x0 , 0x0 , 0x15AAB6F , 0x15AA9EF }},
{_BSPBACKADD2         , std::vector<unsigned int>{ 0x0 , 0x0 , 0x15AABC4 , 0x15AAA44 }},
{_BHPBACKADD1         , std::vector<unsigned int>{ 0x0 , 0x0 , 0x15B4C4D , 0x15B495D }},
{_BHPBACKADD2         , std::vector<unsigned int>{ 0x0 , 0x0 , 0x15B4EF5 , 0x15B4C05 }},
{_ASM_FIELDBGRETADDR3 , std::vector<unsigned int>{ 0x0 , 0x0 , 0x15C3CE5 , 0x15C37E5 }},
{BTLMON0              , std::vector<unsigned int>{ 0x0 , 0x0 , 0x1631700 , 0x1631230 }},
{BTLMON1              , std::vector<unsigned int>{ 0x0 , 0x0 , 0x163170A , 0x163123A }},
{BTLMON2              , std::vector<unsigned int>{ 0x0 , 0x0 , 0x1631729 , 0x1631259 }},
{BTLMON3              , std::vector<unsigned int>{ 0x0 , 0x0 , 0x1631730 , 0x1631260 }},
{WINDOWTITLE          , std::vector<unsigned int>{ 0x0 , 0x0 , 0x1633405 , 0x1632E45 }},
{BTLMON6              , std::vector<unsigned int>{ 0x0 , 0x0 , 0x16340AE , 0x1633AEE }},
{BTLMON4              , std::vector<unsigned int>{ 0x0 , 0x0 , 0x16341B2 , 0x1633BF2 }},
{BTLMON5              , std::vector<unsigned int>{ 0x0 , 0x0 , 0x16341B6 , 0x1633BF6 }},
{BCPBACKADD2          , std::vector<unsigned int>{ 0x0 , 0x0 , 0x1636436 , 0x1635E77 }},
{BCPBACKADD3          , std::vector<unsigned int>{ 0x0 , 0x0 , 0x16373F0 , 0x1636E30 }},
{FCPBACKADD1          , std::vector<unsigned int>{ 0x0 , 0x0 , 0x16384EE , 0x1637F2E }},
{_ASM_FIELDBGRETADDR2 , std::vector<unsigned int>{ 0x0 , 0x0 , 0x1638862 , 0x16382A2 }},
{DISABLETPAGELIMIT    , std::vector<unsigned int>{ 0x0 , 0x0 , 0x16388B7 , 0x16382F7 }},
{BCPBACKADD1          , std::vector<unsigned int>{ 0x0 , 0x0 , 0x16389DF , 0x163841F }},
{NEWGLTEX_CHARA       , std::vector<unsigned int>{ 0x0 , 0x0 , 0x163DAC0 , 0x163D500 }},
{FIELDCHARENT1        , std::vector<unsigned int>{ 0x0 , 0x0 , 0x163E88C , 0x163E2CA }},
{FIELDCHARENT2        , std::vector<unsigned int>{ 0x0 , 0x0 , 0x163E8B7 , 0x163E2F7 }},
{FCPBACKADD2          , std::vector<unsigned int>{ 0x0 , 0x0 , 0x163E8FD , 0x163E33D }},
{BHP                  , std::vector<unsigned int>{ 0x0 , 0x0 , 0x167A558 , 0x1679F98 }},
{DS_FREE              , std::vector<unsigned int>{ 0x0 , 0x0 , 0x169D2A8 , 0x169D2A8 }},
{DS_TEXIMG            , std::vector<unsigned int>{ 0x0 , 0x0 , 0x169D4B4 , 0x169D4AC }},
{WORLDGETTEXIDX0      , std::vector<unsigned int>{ 0x0 , 0x0 , 0x1753F70 , 0x1753F70 }},
{WORLDGETTEXIDX1      , std::vector<unsigned int>{ 0x0 , 0x0 , 0x1753F74 , 0x1753F74 }},
{WORLDGETTEXIDX2      , std::vector<unsigned int>{ 0x0 , 0x0 , 0x1753F78 , 0x1753F78 }},
{WORLDGETTEXIDX3      , std::vector<unsigned int>{ 0x0 , 0x0 , 0x1753F7C , 0x1753F7C }},
{CHECKTEXREPAVAIL     , std::vector<unsigned int>{ 0x0 , 0x0 , 0x1793BA8 , 0x1793BB0 }},
{BGFILENAME1          , std::vector<unsigned int>{ 0x0 , 0x0 , 0x1793C94 , 0x1793CB4 }},
{BGRESPATCH1          , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0		 , 0x15A4D57 }},
{BGFILENAME2          , std::vector<unsigned int>{ 0x0 , 0x0 , 0x18A85A4 , 0x18A8418 }},
{ CHANGEMODE      , std::vector<unsigned int>{ 0x0       , 0x0 , 0x0 ,0x16327C0,} }, //+1FC for JP and EFIGS
{ STEAMADDR      , std::vector<unsigned int>{ 0x0       , 0x0 , 0x0 ,0x16FED48,} }, //+1FC for JP and EFIGS
{ FIELD_CHAR_BLINK      , std::vector<unsigned int>{ 0x0       , 0x0 , 0x0 ,0x1637192,} }, 
{ SUB_116059B0      , std::vector<unsigned int>{ 0x0       , 0x0 , 0x0 ,0x1637310,} },
{ LOAD_BATTLE_EXT      , std::vector<unsigned int>{ 0x0       , 0x0 , 0x0 ,0x1635F20,} },
{ SET_RESOLUTION       , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x219D70 }},
{ SKIP_SLASHSCREEN     , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x1793B97 }},
{ BHP_ATLAS            , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x1631180 }},
{ MORE_DEBUG_LOG       , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x35260 }},
{ FILTERPATCH2_VALUE   , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x159FE74 }},
{ WMSET014_PATH_PUSH   , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x1638356 }},
{ GUEST_STACK_BASE     , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x16DB880 }},
{ GUEST_PAGE_TABLE     , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x18A09A0 }},
{ GUEST_LOADIMAGE      , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x021C260 }},
{ WMSET_GUEST          , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x23A1D1C }},
{ WM_CHARA_READ_CALL   , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x0E01B61 }},
{ WM_READ_FILE         , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x08A97B0 }},
//sm_pc_read is 0x5312C0 in the JP game (read_field_data 0x474200, the caller of hook_get_field_ext_jp, calls it 17 times)
{ FIELD_READ_FILE      , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x08A9A00 }},
//guest address, same use (the JP read_vram_palette is host 0x24C240)
{ PSX_VRAM             , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x204D7B0 }},
//guests 0x467DA0 and 0x467D40, called only from 0x466BC0
{ READ_VRAM_PALETTE    , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x024C240 }},
{ WRITE_PALETTE        , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x024C0B0 }},
//the same cull jumps and entity table in the JP game (the light group table is not known there: lights cannot follow groups)
{ FIELD_CULL_TRI       , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x08C4482 }},
{ FIELD_CULL_QUAD      , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x08C4AA6 }},
{ FIELD_ENTITIES       , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x22A1068 }},
{ FIELD_ENTITY_COUNT   , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x22A10F9 }},
//the same function and its three calls in the JP game (found by tools/port_addresses.py and by listing every call to it:
//both DLLs have the same 31, in the same order)
{ ANALOG_GET_VALUE     , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x0333550 }},
{ ANALOG_FIELD_TEST    , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x029D1B0 }},
{ ANALOG_FIELD_X       , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x029D43F }},
{ ANALOG_FIELD_Y       , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x029D49B }},
//the world map's first call, its key states and the player's entity in the JP game (read from the same code there)
{ ANALOG_WORLD_FIRST   , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x0962CED }},
{ WM_KEYS              , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x2543EC8 }},
{ WM_KEY_INDEX         , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x2544A9C }},
{ FIELD_PLAYER_ENTITY  , std::vector<unsigned int>{ 0x0 , 0x0 , 0x0 , 0x21DEF68 }},
};




const std::map<ADDRESS_NAME_STEAM, unsigned int> PATCHADDRESSSTEAM_EFIGS =
{
	{ Magic_GetIDLoad_107FDF20 , 0x8580 },
};
const std::map<ADDRESS_NAME_STEAM, unsigned int> PATCHADDRESSSTEAM_JP =
{
	{ Magic_GetIDLoad_107FDF20 , 0xFFFFFFFF },
};