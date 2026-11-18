#include "mymemory.h"
#include "coreHeader.h"

#include <set>

//this is a good place for managing game version and language indexes
static const std::map<ADDRESS_NAME, std::vector<unsigned int>>& ActiveAddressTable()
{
	return IsJapanese() ? PATCHADDRESS_JP : PATCHADDRESS_EFIGS;
}

static const char* const ADDRESS_NAMES[] =
{
	"UVPATCH0", "UVPATCH1", "UVPATCH2", "UVPATCH3", "UVPATCH4", "UVPATCH5",
	"UVPATCH6", "UVPATCH7", "UVPATCH8", "UVPATCH9", "UVPATCH10", "UVPATCH11",
	"UVPATCH12", "UVFUNCPNTR", "WORLDGETTEXIDX0", "WORLDGETTEXIDX1", "WORLDGETTEXIDX2", "WORLDGETTEXIDX3",
	"FCPBACKADD1", "FIELDCHARENT1", "FIELDCHARENT2", "FCPBACKADD2", "BGFILENAME1", "BGFILENAME2",
	"CHECKTEXREPAVAIL", "_ASM_FIELDBGRETADDR3", "DISABLETPAGELIMIT", "_ASM_FIELDBGRETADDR2", "DS_FREE", "DS_TEXIMG",
	"_BSPBACKADD1", "_BSPBACKADD2", "BATTLEJMPPATCH1", "BATTLEJMPPATCH2", "BTLMON0", "BTLMON1",
	"BTLMON2", "BTLMON3", "BTLMON4", "BTLMON5", "BTLMON6", "BHP",
	"BHP_ATLAS", "_BHPBACKADD1", "_BHPBACKADD2", "_BHPBACKADD3", "NEWGLTEX_CHARA", "BCPBACKADD1",
	"BCPBACKADD2", "BCPBACKADD3", "CLTBACKADD1", "CLTBACKADD2", "UPLOADVRAM", "IOFUNC1",
	"IOFUNC2", "IOFUNC3", "IOFUNC4", "IOFUNC5", "IOFUNC6", "FILTERPATCH1",
	"FILTERPATCH2", "FILTERPATCH3", "FILTERPATCH4", "WINDOWTITLE", "NULLSUB_DEBUG", "BGRESPATCH1",
	"GLFWINIT", "GLFWWINDOWHINT", "CHANGEMODE", "STEAMADDR", "FIELD_CHAR_BLINK", "SUB_116059B0",
	"LOAD_BATTLE_EXT", "SKIP_SLASHSCREEN", "MORE_DEBUG_LOG", "SET_RESOLUTION", "FILTERPATCH2_VALUE",
	"WMSET014_PATH_PUSH", "GUEST_STACK_BASE", "GUEST_PAGE_TABLE", "GUEST_LOADIMAGE", "WMSET_GUEST",
	"WM_CHARA_READ_CALL", "WM_READ_FILE", "FIELD_READ_FILE",
	"PSX_VRAM", "READ_VRAM_PALETTE", "WRITE_PALETTE",
	"FIELD_CULL_TRI", "FIELD_CULL_QUAD", "FIELD_ENTITIES", "FIELD_ENTITY_COUNT", "FIELD_BG_GROUPS",
	"ANALOG_GET_VALUE", "ANALOG_FIELD_TEST", "ANALOG_FIELD_X", "ANALOG_FIELD_Y",
	"ANALOG_WORLD_FIRST", "WM_KEYS", "WM_KEY_INDEX", "FIELD_PLAYER_ENTITY",
};
static_assert(sizeof(ADDRESS_NAMES) / sizeof(ADDRESS_NAMES[0]) == ADDRESS_NAME_COUNT,
	"ADDRESS_NAMES must list every ADDRESS_NAME in enum order");

const char* GetAddressName(const ADDRESS_NAME name)
{
	const int index = static_cast<int>(name);
	if (index < 0 || index >= ADDRESS_NAME_COUNT)
		return "UNKNOWN_ADDRESS";
	return ADDRESS_NAMES[index];
}

bool HasAddress(const ADDRESS_NAME name)
{
	const auto& table = ActiveAddressTable();
	const auto it = table.find(name);
	return it != table.end() && it->second.size() > FORCEGAME && it->second[FORCEGAME] != 0;
}

//Returns 0 and logs once instead of throwing std::out_of_range (which used to terminate the game on JP,
//where several entries are missing)
unsigned int _stdcall GetAddress(const ADDRESS_NAME name)
{
	if (!HasAddress(name))
	{
		static std::set<int> warned;
		if (warned.insert(static_cast<int>(name)).second)
			OutputDebug("GetAddress: address %s is unknown for %s- the feature using it is skipped\n",
				GetAddressName(name), IsJapanese() ? "FFVIII_JP" : "FFVIII_EFIGS");
		return 0;
	}
	return ActiveAddressTable().at(name)[FORCEGAME];
}

unsigned int _stdcall GetAddressBase(const ADDRESS_NAME name)
{
	const unsigned int address = GetAddress(name);
	if (address == 0)
		return 0;
	return IMAGE_BASE + address;
}

unsigned int _stdcall GetAddressFromSteam(const ADDRESS_NAME_STEAM name)
{
	const auto& table = IsJapanese() ? PATCHADDRESSSTEAM_JP : PATCHADDRESSSTEAM_EFIGS;
	const auto it = table.find(name);
	return it == table.end() ? 0xFFFFFFFF : it->second;
}
