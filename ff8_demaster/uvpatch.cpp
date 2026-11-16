#include "coreHeader.h"

//Filled in ApplyUVPatch: a global initializer would run before the game language is detected in DllMain
//and always pick the EFIGS addresses
static constexpr ADDRESS_NAME uvPatchNames[] =
{
	UVPATCH0,UVPATCH1,UVPATCH2,UVPATCH3,UVPATCH4,UVPATCH5,
	UVPATCH6,UVPATCH7,UVPATCH8,UVPATCH9,UVPATCH10,UVPATCH11,
};

BOOL uvPatchModMnemonic(DWORD address)
{
	if (!ModPage(address, 4))
		return FALSE;
	*((BYTE*)address + 3) = 0;
	return TRUE;
}

void ApplyUVPatch()
{
	if (!HasAddress(UVFUNCPNTR))
		return;
	BYTE* firstMnemonic = (BYTE*)(IMAGE_BASE + GetAddress(UVFUNCPNTR));
	DWORD addMnemonic = *(DWORD*)firstMnemonic & 0xFFFFFF; //get mnemonic without parameter
	if (addMnemonic != 0x084680)
	{
		OutputDebug("DEMASTER::ApplyUvPatch::The addresses are wrong! We found no ADD byte ptr[esi+8] at given rel call\n");
		return;
	}
	for (const ADDRESS_NAME name : uvPatchNames)
		if (!HasAddress(name) || !uvPatchModMnemonic(IMAGE_BASE + GetAddress(name)))
			return;
}