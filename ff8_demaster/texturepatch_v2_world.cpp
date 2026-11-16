#include "config.h"
#include "coreHeader.h"

#include <filesystem>
#include <set>
DWORD _asm_WorldRetAddr1;
DWORD _asm_WorldRetAddr2;
DWORD _asm_WorldRetAddr3;
DWORD _asm_WorldRetAddr4;


struct worldTextureStructure
{
	char localPath[256]{ 0 };
	DWORD tpage{};
	SafeBimg buffer{ SafeBimgInit() };
	//previous image of this slot, so a page that alternates between two files does not reload from disk
	char previousPath[256]{ 0 };
	SafeBimg previousBuffer{ SafeBimgInit() };
	int width{ -1 };
	int height{ -1 };
	int channels{ -1 };
	bool bActive{ false };
	worldTextureStructure(DWORD in_tpage) noexcept
		: tpage{ in_tpage }
	{

	}
};

worldTextureStructure ws[] =
{
{0},{1},{2},{3},{4},{5},{6},{7},{8},{9},{10},{11},{12},{13},{14},{15},{16},{17},{18},{19},{20},
	{21},   // TRAIN TRACKS
	{22},   // CLOUDS
	{23},   // MAP
	{24},   // moon texture + fx
	{25},   // WATER
	{26},   // characters
	{27},   // 16: textype=18 ???
	{28},	// 0:  textype=0
	{29},	// 13: textype=0 full screen wm, sm wm, clouds
	{30},	// 15: textype=0 sm wm
	{31},	// 20: textype=0
	{32},	// 22: textype=0 sm wm
	{33},	// 28: textype=0 sm wm
	{34},	// 29: textype=0
	{35},	// 17: far terrain (low-res texl copies), see GetTextureIndex
	{36},	// 18: far terrain
	{37},	// 19: far terrain
};

int GetTextureIndex();

//you can't just create new folder because >WEEP< - too lazy to find the cause
DWORD lastKnownTextureId{ 0 };
bool LoadImageIntoWorldStruct(size_t texIndex, const char* const localn, int tPage, int palette)
{
	//If you want to be able to swap out a texture at a given index you could set bActive to false and it would force load it.
	if (ws[texIndex].bActive && ws[texIndex].buffer)//texture loaded skip
	{
		if (strcmp(ws[texIndex].localPath, localn) == 0)
			return true;
		//Used to keep the first file forever (only this warning was logged), so a palette variant (_2, _5...) or a page
		//the game refilled never showed. Switch to the requested file instead, keeping the previous one in memory.
		if (ws[texIndex].previousBuffer && strcmp(ws[texIndex].previousPath, localn) == 0)
		{
			std::swap(ws[texIndex].buffer, ws[texIndex].previousBuffer);
			std::swap(ws[texIndex].localPath, ws[texIndex].previousPath);
			ws[texIndex].width = ws[texIndex].buffer->m_width;
			ws[texIndex].height = ws[texIndex].buffer->m_height;
			ws[texIndex].channels = ws[texIndex].buffer->m_hasAlpha ? 4 : 3;
			return true;
		}
		OutputDebug("%s::%d::slot %zu now uses:\n\t%s\n\t(was %s)\n", __func__,__LINE__, texIndex, localn, ws[texIndex].localPath);
	}
	SafeBimg img = LoadImageFromFile(localn);
	if (!img)
		return false;
	if (ws[texIndex].bActive && ws[texIndex].buffer)
	{
		ws[texIndex].previousBuffer = std::move(ws[texIndex].buffer);
		strcpy(ws[texIndex].previousPath, ws[texIndex].localPath);
	}
	ws[texIndex].width = img->m_width;
	ws[texIndex].height = img->m_height;
	ws[texIndex].channels = img->m_hasAlpha ? 4 : 3;
	ws[texIndex].buffer = std::move(img);
	ws[texIndex].bActive = true;
	strcpy(ws[texIndex].localPath, localn);

	OutputDebug("\t%s::%d::Tpage: %d, Palette: %d, TexIndex: %d\n", __func__, __LINE__, tPage, palette, texIndex);
	OutputDebug("\t%s::%d::w: %d; h: %d; channels: %d\n", __func__, __LINE__, ws[texIndex].width, ws[texIndex].height, ws[texIndex].channels);
	return true;
}
void WtpGl()
{
	if (WorldVehiclesUploadPending()) //page built by WtpCheck with the vehicles drawn in
		return;
	DWORD tPage = gl_textures[50];
	int palette = tex_header[52];
	int textureType = gl_textures[48];
	char localn[256]{ 0 };

	size_t texIndex = lastKnownTextureId;

	int getTexIndex = GetTextureIndex();
	if (getTexIndex <= 0) return;

	if (textureType == 18 && texIndex == getTexIndex)
		//if (getTexIndex < 20 && (tPage > 14 && tPage < 26) )
	{

		if (DDSorPNG(localn, 256, "%stextures\\world\\dat\\texl\\texl_%03d_%d", DIRECT_IO_EXPORT_DIR, /*tPage*/texIndex, palette))
			if (DDSorPNG(localn, 256, "%stextures\\world\\dat\\texl\\texl_%03d_%d", DIRECT_IO_EXPORT_DIR, /*tPage*/texIndex, 0))
			{
				OutputDebug("%s::%d::Failed to Load: %s\n\tgetTexIndex: %d, tPage: %d, texIndex: %d, palette: %d", __func__, __LINE__, localn, getTexIndex, tPage, texIndex, palette);
				return;
			}

		LoadImageIntoWorldStruct(texIndex, localn, tPage, palette);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		RenderTexture(ws[texIndex].buffer.get());

		return;
	}
	else if (textureType == 18)
	{
		if (DDSorPNG(localn, 256, "%stextures\\world\\dat\\wmset\\wmset_%03d_%d", DIRECT_IO_EXPORT_DIR, getTexIndex, palette))
			if (DDSorPNG(localn, 256, "%stextures\\world\\dat\\wmset\\wmset_%03d_%d", DIRECT_IO_EXPORT_DIR, getTexIndex, 0))
			{
				OutputDebug("%s::%d::Failed to Load: %s\n\tgetTexIndex: %d\n palette: %d", __func__, __LINE__, localn, getTexIndex, palette);
				return;
			}
		int wmStructPointer = 21;
		switch (getTexIndex)
		{
		case 13: //moon texture + fx
			wmStructPointer = 24;
			break;
		case 14: //clouds or atleast is used to be.
			wmStructPointer = 22;
			break;
		case 15: //characters and vehicles
			wmStructPointer = 26;
			break;
		case 16: //far terrain: low-res copies of texl, drawn for distant land
			wmStructPointer = 27;
			break;
		case 17: //far terrain
			wmStructPointer = 35;
			break;
		case 18: //far terrain
			wmStructPointer = 36;
			break;
		case 19: //far terrain
			wmStructPointer = 37;
			break;
		case 24: //???
			wmStructPointer = 23;
			break;
		case 28: // water
			wmStructPointer = 25;
			break;
		case 29: //train tracks
			wmStructPointer = 21;
			break;
		}
		if (LoadImageIntoWorldStruct(wmStructPointer, localn, tPage, palette))
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			RenderTexture(ws[wmStructPointer].buffer.get());
		}
	}
	else if (textureType == 0)
	{
		if (DDSorPNG(localn, 256, "%stextures\\world\\dat\\wmset\\wmset0_%03d_%d", DIRECT_IO_EXPORT_DIR, getTexIndex, palette))
			if (DDSorPNG(localn, 256, "%stextures\\world\\dat\\wmset\\wmset0_%03d_%d", DIRECT_IO_EXPORT_DIR, getTexIndex, 0))
			{
				OutputDebug("%s::%d::Failed to Load: %s\n\tgetTexIndex: %d\n palette: %d", __func__, __LINE__, localn, getTexIndex, palette);
				return;
			}
		int wmStructPointer = 28;
		switch (getTexIndex)
		{
		case 0: //
			wmStructPointer = 28;
			break;
		case 13: //full screen wm, sm wm, clouds
			wmStructPointer = 28;
			break;
		case 15: //sm wm
			wmStructPointer = 30;
			break;
		case 20: //
			wmStructPointer = 31;
			break;
		case 22: //sm wm
			wmStructPointer = 32;
			break;
		case 28: //sm wm
			wmStructPointer = 33;
			break;
		case 29: //
			wmStructPointer = 34;
			break;
		}
		if (LoadImageIntoWorldStruct(wmStructPointer, localn, tPage, palette))
		{
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			RenderTexture(ws[wmStructPointer].buffer.get());
		}
	}
	return;
}

int GetTextureIndex()
{
	//void* pixels = (void*)pixelsPtr;
	const DWORD tPage = gl_textures[50];
	const int palette = tex_header[52];
	const int textureType = gl_textures[48];
	const DWORD texA = *reinterpret_cast<DWORD*>(IMAGE_BASE + GetAddress(WORLDGETTEXIDX0)) + 1;
	const DWORD texB = *reinterpret_cast<DWORD*>(IMAGE_BASE + GetAddress(WORLDGETTEXIDX1)) + 1;
	const DWORD texC = *reinterpret_cast<DWORD*>(IMAGE_BASE + GetAddress(WORLDGETTEXIDX2)) + 1;
	const DWORD texD = *reinterpret_cast<DWORD*>(IMAGE_BASE + GetAddress(WORLDGETTEXIDX3)) + 1;

	if (textureType == 18)
	{
		lastKnownTextureId = 0;
		switch (tPage)
		{
		case 20:
			lastKnownTextureId = texA;
			return texA;
		case 22:
			lastKnownTextureId = texB;
			return texB;
		case 24:
			lastKnownTextureId = texC;
			return texC;
		case 26:
			lastKnownTextureId = texD;
			return texD;
		case 13: //moon texture + fx
		case 14: //clouds or atleast used to be
		case 28: //water textures
		case 29: //tracks, bridge, vehicle
		//16-19: far terrain. The game draws distant land from these pages: half-size copies of texl 0-15, arranged as
		//in VRAM (texl_001/003 in the left column of wmset_016, texl_002/004 on the right, and so on). Without them
		//distant land stayed vanilla until it entered a texl slot (20/22/24/26) and popped to HD.
		case 16:
		case 17:
		case 18:
		case 19:
		//case 15: //vehicles + character
				 //this one is special the game mixes different textures into one and they overlap.
			// character texture is top left. some cars are top left.
			//Maki- looks like I need to find pre-atlas generator for this
			return tPage;
		}
		//OutputDebug("%s::tPage: %d\n", __func__, tPage);
	}
	else if (textureType == 0) //map
	{
		switch (tPage)
		{
		case 13: //full screen wm, sm wm, clouds
		case 15: // sm wm
		case 22: // sm wm
		case 28: // sm wm
			// when small map up 15,22,28 swaps / flickers with 13
			//return 13;
			return -1; //disabled to due to cloulds colliding with minimap
		case 0: //??? probably more minimaps 
		case 20: //???
		case 29: //???
			return tPage;
		default:
			OutputDebug("%s::%d::tPage: %d, textureType: %d, palette: %d\n", __func__, __LINE__, tPage, textureType, palette);
			break;
		}
	}
	else
	{
		OutputDebug("%s::%d::tPage: %d, textureType: %d, palette: %d\n", __func__, __LINE__, tPage, textureType, palette);
		//return 24;
	}
	return -1;
}

DWORD LOCALDEB = 0;

DWORD WtpCheck()
{
	if (!WORLD_TEXTURES)
		return 0;

	int textureType = gl_textures[48];
	if (textureType != 18 && textureType != 0) //we want only world textures
		return 0;

	DWORD tPage = gl_textures[50];
	int palette = tex_header[52];

	if (WORLD_DEBUG)
	{
		static std::set<DWORD> logged;
		if (logged.insert((static_cast<DWORD>(textureType) << 24) | (tPage << 12) | static_cast<DWORD>(palette)).second)
			OutputDebug("World page: type %d tpage %u palette %d size %ux%u game upscale %u\n", textureType, tPage, palette,
				uploadWidth, uploadHeight, gameUpscaleFactor);
		WorldDumpPage(textureType, tPage, palette); //WORLD_DEBUG=2
	}

	char localn[256]{ 0 };
	int textureIndex = GetTextureIndex();

	//Named textures (WORLD_VEHICLES: vehicles, characters, water, effects, the select map): drawn into the page wherever
	//the game put them, over this page's wmset_XXX file if any. Runs before the "disabled page" check below because
	//pages such as 15 (vehicles and characters) are disabled for page files.
	if (WORLD_VEHICLES && (textureType == 18 || textureType == 0))
		if (WorldVehiclesBuildPage(tPage, textureType, textureIndex, palette))
			return 1;

	//OutputDebug("%s::%d::tPage: %d, textureType: %d, palette: %d\n", __func__, __LINE__, tPage, textureType, palette);

	if (textureIndex < 0) //disabled textures are set to -1;
		return 0;
	if (textureType == 18 && lastKnownTextureId == textureIndex)//(textureIndex < 20 && (tPage > 14 && tPage < 26))
	{
		if (DDSorPNG(localn, 256, "%stextures\\world\\dat\\texl\\texl_%03d_%d", DIRECT_IO_EXPORT_DIR, textureIndex, palette))
			DDSorPNG(localn, 256, "%stextures\\world\\dat\\texl\\texl_%03d_%d", DIRECT_IO_EXPORT_DIR, textureIndex, 0);
	}
	else if (textureType == 18)
	{
		if (DDSorPNG(localn, 256, "%stextures\\world\\dat\\wmset\\wmset_%03d_%d", DIRECT_IO_EXPORT_DIR, textureIndex, palette))
			DDSorPNG(localn, 256, "%stextures\\world\\dat\\wmset\\wmset_%03d_%d", DIRECT_IO_EXPORT_DIR, textureIndex, 0);
	}
	else if (textureType == 0)
	{
		//giving different name to avoid collisions as some of these have the same textureIndex.
		// I do wonder if the texture index does anything for this part. Minimap seems to use many different ones.
		if (DDSorPNG(localn, 256, "%stextures\\world\\dat\\wmset\\wmset0_%03d_%d", DIRECT_IO_EXPORT_DIR, textureIndex, palette))
			DDSorPNG(localn, 256, "%stextures\\world\\dat\\wmset\\wmset0_%03d_%d", DIRECT_IO_EXPORT_DIR, textureIndex, 0);

		//return 1; //force attempt to override
	}

	if (GetFileAttributesA(localn) == INVALID_FILE_ATTRIBUTES)
	{
		//OutputDebug("%s::Failed to Load: %s\n", __func__, localn); //spamming message!
		return 0;
	}
	return 1;
}

//Remastered's HD loader asks for textures\FIELD.FS\field_hd_new\wmset_014_0.png for its remodelled vehicle (the one taken
//to Dollet). That name collides with the wmset_014 world page, so REMASTER_WMSET014 points the request at another file.
//The request is a single "push <string>" in the loader (WMSET014_PATH_PUSH); the loader adds "textures\" and ".png".
static char remasterWmset014Name[MAX_PATH]{ 0 };

static void RenameRemasterWmset014()
{
	if (REMASTER_WMSET014.empty())
		return;
	if (!HasAddress(WMSET014_PATH_PUSH))
		return;
	const DWORD push = IMAGE_BASE + GetAddress(WMSET014_PATH_PUSH);
	const char* const pushed = *reinterpret_cast<const char* const*>(push + 1);
	if (*reinterpret_cast<const BYTE*>(push) != 0x68 || pushed == nullptr
		|| strcmp(pushed, "FIELD.FS\\field_hd_new\\wmset_014_0") != 0)
	{
		OutputDebug("REMASTER_WMSET014: unexpected code at %08X, name not changed\n", push);
		return;
	}
	const std::string file = std::string(DIRECT_IO_EXPORT_DIR) + "textures\\" + REMASTER_WMSET014 + ".png";
	if (!std::filesystem::exists(file))
	{
		OutputDebug("REMASTER_WMSET014: %s not found, Remastered keeps loading wmset_014_0\n", file.c_str());
		return;
	}
	strncpy(remasterWmset014Name, REMASTER_WMSET014.c_str(), MAX_PATH - 1);
	InjectDWORD(push + 1, reinterpret_cast<DWORD>(remasterWmset014Name));
	OutputDebug("REMASTER_WMSET014: Remastered's vehicle texture now loads from %s\n", file.c_str());
}

void ApplyWorldPatch()
{
	RenameRemasterWmset014();
	ApplyWorldVehiclesPatch();
}
#undef CRASHLOG