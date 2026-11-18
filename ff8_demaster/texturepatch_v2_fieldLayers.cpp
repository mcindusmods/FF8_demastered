//Field backgrounds from Field-Map-Editor's deswizzled layers (FIELD_LAYERS). See field_layers.h for the idea; this file
//is the game side.
//
//Three hooks:
//	FIELD_READ_FILE  the recompiled sm_pc_read(filename, buffer), every file the game reads from its archives.
//	                 .mim: kept (the field's VRAM, to tell which palette each page texture of the game was made with).
//	                 .map: the tiles are read from game memory, matched to the layer images on disk, tiles that share a
//	                 cell but have their own image are given free cells (written back into the .map before the game uses
//	                 it), and the plan for every page and palette is made.
//	_bsp (UPLOADVRAM, texturepatch_v2_battleScenery.cpp)  when the game uploads a field page (texture type 25): which
//	                 .map palette it is (the game numbers them its own way, see MatchPalette), then the page is put
//	                 together from the layer images in the texture the game has bound: BC7 files are copied block by block
//	                 on the GPU (nothing decoded or re-encoded), anything else is put together on the CPU.
//	glDrawElements   (FIELD_SMOOTH) while one of those pages is drawn, the shader's demasterTileClamp uniform makes main.frag
//	                 filter smoothly but only inside each tile's own 16x16 cell, so nothing bleeds in from the cells around
//	                 it (the page itself stays point-filtered). (FIELD_LAYERS_LIVE) once a second, layer images changed on
//	                 disk are read again and the pages using them rebuilt.
//FIELD_BG_RESOLUTION sets the size of the buffer Remastered draws field backgrounds into (ApplyFieldBgResolution).

#include "config.h"
#include "coreHeader.h"
#include "opengl.h"
#include "texture.h"
#include "field_layers.h"
#include "field_pbr_runtime.h"
#include "day_night.h"
#include "gl_state.h"
#include "minhook/include/MinHook.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cstdio>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace fieldlayers;
using namespace fieldrt;

#ifndef GL_COMPRESSED_RGBA_BPTC_UNORM
#define GL_COMPRESSED_RGBA_BPTC_UNORM 0x8E8C
#endif

namespace
{
	//Emulated CPU state: EAX +0, ECX +4, EDX +8, EBX +0xC, EBP +0x28, ESP +0x2C, ESI +0x30, EDI +0x34
	constexpr size_t STATE_EAX = 0;
	constexpr size_t STATE_ESP = 0x2C;
	constexpr int FIELDBG_TEXTURE_TYPE = 25;

	bool hooksInstalled = false;
	bool disabled = false; //set after an unexpected error: field pages are left as the game makes them
	DWORD guestStackBaseAddress = 0;
	DWORD guestPageTableAddress = 0;
	DWORD psxVram = 0; //guest address of the game's PlayStation VRAM (PSX_VRAM), 0 when not known

	typedef DWORD(__cdecl* RecompiledFunction)(BYTE* state);
	RecompiledFunction readFileOriginal = nullptr;
	RecompiledFunction readVramPaletteOriginal = nullptr, writePaletteOriginal = nullptr;
	bool paletteHooks = false;

	bool CopyHost(void* destination, const void* source, const size_t length)
	{
#ifdef _MSC_VER
		__try
		{
			memcpy(destination, source, length);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
#else
		if (IsBadReadPtr(source, length))
			return false;
		memcpy(destination, source, length);
		return true;
#endif
	}

	DWORD StateRegister(const BYTE* state, const size_t offset)
	{
		DWORD value = 0;
		CopyHost(&value, state + offset, 4);
		return value;
	}

	bool StackRead32(const DWORD guest, DWORD& value)
	{
		DWORD base = 0;
		if (!CopyHost(&base, reinterpret_cast<const void*>(guestStackBaseAddress), 4) || base == 0)
			return false;
		return CopyHost(&value, reinterpret_cast<const void*>(base + guest), 4);
	}

	//host address of a guest byte (the recompiled code reaches all guest memory, stack included, through this table)
	BYTE* GuestHost(const DWORD guest)
	{
		DWORD page = 0;
		const DWORD* table = reinterpret_cast<const DWORD*>(guestPageTableAddress);
		if (!CopyHost(&page, &table[guest >> 12], 4) || page == 0)
			return nullptr;
		return reinterpret_cast<BYTE*>(page + (guest & 0xFFF));
	}

	bool GuestRead(void* destination, DWORD guest, size_t length)
	{
		BYTE* out = static_cast<BYTE*>(destination);
		while (length > 0)
		{
			BYTE* host = GuestHost(guest);
			const size_t inPage = 0x1000 - (guest & 0xFFF);
			const size_t chunk = length < inPage ? length : inPage;
			if (host == nullptr || !CopyHost(out, host, chunk))
				return false;
			out += chunk;
			guest += static_cast<DWORD>(chunk);
			length -= chunk;
		}
		return true;
	}

	bool GuestWrite(DWORD guest, const void* source, size_t length)
	{
		const BYTE* in = static_cast<const BYTE*>(source);
		while (length > 0)
		{
			BYTE* host = GuestHost(guest);
			const size_t inPage = 0x1000 - (guest & 0xFFF);
			const size_t chunk = length < inPage ? length : inPage;
			if (host == nullptr || !CopyHost(host, in, chunk))
				return false;
			in += chunk;
			guest += static_cast<DWORD>(chunk);
			length -= chunk;
		}
		return true;
	}

	std::string GuestString(DWORD guest, const size_t limit = 260)
	{
		std::string out;
		while (out.size() < limit)
		{
			char chunk[64];
			const size_t inPage = 0x1000 - (guest & 0xFFF);
			const size_t length = std::min<size_t>({ sizeof(chunk), inPage, limit - out.size() });
			if (!GuestRead(chunk, guest, length))
				break;
			const size_t end = std::find(chunk, chunk + length, '\0') - chunk;
			out.append(chunk, end);
			if (end < length)
				break;
			guest += static_cast<DWORD>(length);
		}
		return out;
	}

}

//--- shared with the PBR module (field_pbr_runtime.h)
namespace fieldrt
{
	void Log(const char* format, ...)
	{
		char text[1024];
		va_list args;
		va_start(args, format);
		vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		OutputDebug("%s", text);
	}

	void LogOnce(const std::string& key, const char* format, ...)
	{
		static std::mutex logMutex;
		static std::set<std::string> logged;
		{
			std::lock_guard<std::mutex> lock(logMutex);
			if (!logged.insert(key).second)
				return;
		}
		char text[1024];
		va_list args;
		va_start(args, format);
		vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		OutputDebug("%s", text);
	}

	//--- layer files: <stem>.png or <stem>.dds; with both, the newer one (edit the PNG, then make the DDS when it is done)

	ImageFile ResolveImage(const std::string& stem)
	{
		ImageFile dds{ stem + ".dds" }, png{ stem + ".png" };
		std::error_code error;
		const bool hasDds = std::filesystem::is_regular_file(dds.path, error);
		const bool hasPng = std::filesystem::is_regular_file(png.path, error);
		if (hasDds)
			dds.time = std::filesystem::last_write_time(dds.path, error);
		if (hasPng)
			png.time = std::filesystem::last_write_time(png.path, error);
		if (hasDds && hasPng)
			return png.time > dds.time ? png : dds;
		if (hasDds)
			return dds;
		if (hasPng)
			return png;
		return {};
	}

	bool ImageFileSize(const std::string& path, int& width, int& height)
	{
		if (path.empty())
			return false;
		FILE* f = fopen(path.c_str(), "rb");
		if (f == nullptr)
			return false;
		uint8_t header[24]{};
		const size_t read = fread(header, 1, sizeof(header), f);
		fclose(f);
		const auto be32 = [&](const int at) { return header[at] << 24 | header[at + 1] << 16 | header[at + 2] << 8 | header[at + 3]; };
		const auto le32 = [&](const int at) { return header[at] | header[at + 1] << 8 | header[at + 2] << 16 | header[at + 3] << 24; };
		if (read >= 24 && header[0] == 0x89 && header[1] == 'P' && header[2] == 'N' && header[3] == 'G' && header[12] == 'I' && header[13] == 'H')
		{
			//PNG: IHDR right after the signature
			width = be32(16);
			height = be32(20);
		}
		else if (read >= 20 && header[0] == 'D' && header[1] == 'D' && header[2] == 'S' && header[3] == ' ')
		{
			//DDS: "DDS ", then DDS_HEADER (size, flags, height, width)
			height = le32(12);
			width = le32(16);
		}
		else
			return false;
		return width > 0 && height > 0 && width <= 65536 && height <= 65536;
	}

	//--- memory that may not be readable (the game's client arrays), and the game's memory (light groups, script entities)
	bool SafeRead(void* destination, const void* source, const size_t length)
	{
		return source != nullptr && CopyHost(destination, source, length);
	}

	bool GuestRead(void* destination, const uint32_t guest, const size_t length)
	{
		return guestPageTableAddress != 0 && ::GuestRead(destination, static_cast<DWORD>(guest), length);
	}

	bool GuestWrite(const uint32_t guest, const void* source, const size_t length)
	{
		return guestPageTableAddress != 0 && ::GuestWrite(static_cast<DWORD>(guest), source, length);
	}
}

namespace
{

	//--- the field being played (game thread writes, renderer reads)

	struct Field
	{
		bool active = false;
		uint64_t entry = 0; //changes every time a .map is read
		std::string name, language;
		Layout layout;
		std::vector<Tile> shipped; //the .map as read, before cells were moved: what the .mim holds
		std::shared_ptr<const std::vector<uint8_t>> mim;
		std::unordered_map<uint32_t, std::string> imageStem; //pupu -> folder\name[_language]_PUPU (no extension)
		std::unordered_map<uint32_t, std::string> imagePath; //pupu -> file in use (empty: none)
		std::shared_ptr<const PbrField> pbr; //PBR layers (null: none)
		std::unordered_map<uint32_t, std::string> paintingPath; //PBR layers: their painting (imagePath has their albedo)
		std::unordered_map<uint32_t, std::string> paintingStem;
		std::map<uint32_t, PagePlan> pages; //PageKey(texture page, .map palette)
		//the game's palette slots (texture page, depth, tex_header palette_index) -> .map palette | depth << 8. The game numbers
		//slots per page and colour depth, in the order it first needs each palette.
		std::map<std::tuple<int, int, int>, int> slots;
		std::unordered_map<uint64_t, int> identified; //page content -> .map palette | depth << 8, -1: none
	};
	std::mutex fieldMutex;
	Field field;
	uint64_t entryCounter = 0;
	std::atomic<uint64_t> latestEntry{ 0 }; //for the renderer, without the lock

	struct MimRead
	{
		std::string name;
		std::shared_ptr<const std::vector<uint8_t>> data;
	};
	MimRead lastMim; //under fieldMutex

	std::string FieldFolder(const std::string& name)
	{
		return std::string(DIRECT_IO_EXPORT_DIR) + "textures\\field_bg\\" + name.substr(0, 2) + "\\" + name + "\\";
	}

	//FME's and PuPu's names: <field>_<pupu>, or <field>_<language>_<pupu> for a field with a .map per language
	std::string FindStem(const std::string& folder, const std::string& name, const std::string& language, const uint32_t pupu)
	{
		const std::string hex = PupuHex(pupu);
		if (!language.empty())
		{
			const std::string stem = folder + name + "_" + language + "_" + hex;
			if (!ResolveImage(stem).path.empty())
				return stem;
		}
		const std::string stem = folder + name + "_" + hex;
		return ResolveImage(stem).path.empty() ? std::string() : stem;
	}

	//"...\\bghall_1_fr.map" -> "bghall_1" + "fr"
	void FieldName(const std::string& filename, std::string& name, std::string& language)
	{
		std::string stem = filename;
		if (const size_t slash = stem.find_last_of("\\/"); slash != std::string::npos)
			stem = stem.substr(slash + 1);
		stem = stem.substr(0, stem.size() - 4);
		std::transform(stem.begin(), stem.end(), stem.begin(), [](const unsigned char c) { return static_cast<char>(tolower(c)); });
		SplitLanguage(stem, name, language);
	}

	bool HasFolder(const std::string& name)
	{
		std::error_code error;
		return name.size() >= 2 && std::filesystem::is_directory(FieldFolder(name), error);
	}

	//sm_pc_read just read a .mim into game memory (in the game this comes before the .map)
	void OnMimRead(const std::string& filename, const DWORD buffer, const DWORD size)
	{
		std::string name, language;
		FieldName(filename, name, language);
		if (!HasFolder(name))
			return;
		if (size != MIM_SIZE && size != 0 && size < (1u << 20))
		{
			LogOnce("mimsize-" + name, "FIELD_LAYERS: %s: %s is %u bytes, not the %zu this works with: its pages are left as the game makes them\n",
				name.c_str(), filename.c_str(), size, MIM_SIZE);
			return;
		}
		auto data = std::make_shared<std::vector<uint8_t>>(MIM_SIZE);
		if (!GuestRead(data->data(), buffer, MIM_SIZE))
			return;
		std::lock_guard<std::mutex> lock(fieldMutex);
		lastMim = { name, data };
		if (field.name == name && field.mim == nullptr) //read after its .map
			field.mim = data;
	}

	//sm_pc_read just read a .map into game memory
	void OnMapRead(const std::string& filename, const DWORD buffer, const DWORD size)
	{
		std::string name, language;
		FieldName(filename, name, language);
		FieldCharsMapRead(false); //the game's own back-face cull, unless this field has PBR files (below)
		//DAY_NIGHT: the field's own time of day (time = day or night in its _pbr.txt holds the clock); else the clock's
		fieldpbr::FieldTime time = fieldpbr::FieldTime::Cycle;
		bool sky = false;
		if (FIELD_PBR && HasFolder(name))
			PbrFieldTime(FieldFolder(name), name, language, time, &sky);
		DayNightFieldEntered(name, time);
		SceneLightFieldEntered(sky); //SCENE_LIGHT: a battle started here is under the sky or not

		Field next;
		{
			std::lock_guard<std::mutex> lock(fieldMutex);
			next.entry = ++entryCounter;
			if (lastMim.name == name)
				next.mim = lastMim.data;
		}
		latestEntry = next.entry; //another field: the pages of the last one are not drawn any more
		next.name = name;
		next.language = language;
		const std::string folder = FieldFolder(name);
		if (!HasFolder(name))
		{
			std::lock_guard<std::mutex> lock(fieldMutex);
			field = std::move(next); //not active: no folder for this field
			return;
		}

		//the tiles, up to the end marker (the read size when the game gives a believable one)
		std::vector<uint8_t> bytes;
		const size_t limit = size >= TILE_BYTES && size <= (1u << 20) ? size : (1u << 20);
		for (size_t offset = 0; offset + TILE_BYTES <= limit; offset += TILE_BYTES)
		{
			uint8_t tile[TILE_BYTES];
			if (!GuestRead(tile, buffer + static_cast<DWORD>(offset), TILE_BYTES))
				break;
			bytes.insert(bytes.end(), tile, tile + TILE_BYTES);
			if (tile[0] == 0xFF && tile[1] == 0x7F)
				break;
		}
		std::vector<Tile> tiles = ParseMap(bytes.data(), bytes.size());
		if (tiles.empty())
		{
			if (FIELD_DEBUG)
				Log("FIELD_LAYERS: %s: no tiles read from %s\n", name.c_str(), filename.c_str());
			std::lock_guard<std::mutex> lock(fieldMutex);
			field = std::move(next);
			return;
		}
		next.shipped = tiles;
		//every tile's picture: its PuPu-named file in PuPu's frame for it, else its FME-named file on FME's canvas (which
		//one a file is, its size says: read from its header here, the game thread)
		const Layout fme = ComputeLayout(tiles);
		const PupuNames pupuNames = ComputePupuNames(tiles);
		const PictureSet pictures = FindPictures(tiles, fme, pupuNames, [&](const uint32_t pupu, int& width, int& height)
		{
			const std::string stem = FindStem(folder, name, language, pupu);
			return !stem.empty() && ImageFileSize(ResolveImage(stem).path, width, height);
		});
		//the canvas the field's own maps, lights and camera are on: fieldprep writes it into _pbr.txt; else FME's
		Frame canvas = fme.fme;
		const bool canvasFromSettings = FIELD_PBR && !PbrShaderMissing() && PbrCanvas(folder, name, language, canvas);
		next.layout = FramedLayout(tiles, fme, pictures, canvas);
		size_t pupuPictures = 0;
		for (const auto& [pupu, picture] : pictures.pictures)
			pupuPictures += picture.pupu;
		for (const auto& [pupu, why] : pictures.unfit)
			LogOnce("unfit-" + name + PupuHex(pupu), "FIELD_LAYERS: %s: %s_%s is %s: not used\n", name.c_str(), name.c_str(),
				PupuHex(pupu).c_str(), why.c_str());

		//PBR layers (their own _A and an _LF): drawn from their albedo and lit in the shader (texturepatch_v2_fieldPbr.cpp)
		if (FIELD_PBR && !PbrShaderMissing())
		{
			std::vector<std::pair<uint32_t, int>> layers;
			std::set<uint32_t> seen;
			for (size_t i = 0; i < tiles.size(); i++)
				if (seen.insert(next.layout.pupu[i]).second)
					layers.push_back({ next.layout.pupu[i], tiles[i].blend });
			next.pbr = PbrFindFiles(folder, name, language, next.layout, layers);
			FieldCharsMapRead(next.pbr != nullptr);
		}

		//which tiles have an image
		std::vector<bool> hasImage(tiles.size(), false);
		size_t images = 0, tilesWithImage = 0;
		std::vector<std::string> missing;
		for (size_t i = 0; i < tiles.size(); i++)
		{
			const uint32_t pupu = next.layout.pupu[i];
			if (i >= pictures.tile.size() || pictures.tile[i] < 0)
			{
				//no file for it in either tool's name and frame
				const std::string names = PupuHex(fme.pupu[i]) + (pupuNames.name[i] != fme.pupu[i] ? "/" + PupuHex(pupuNames.name[i]) : "");
				if (std::find(missing.begin(), missing.end(), names) == missing.end())
					missing.push_back(names);
				continue;
			}
			auto it = next.imagePath.find(pupu);
			if (it == next.imagePath.end())
			{
				std::string stem = FindStem(folder, name, language, pupu);
				std::string path = stem.empty() ? std::string() : ResolveImage(stem).path;
				if (next.pbr != nullptr)
					if (const auto layer = next.pbr->layers.find(pupu); layer != next.pbr->layers.end())
					{
						next.paintingPath[pupu] = path;
						next.paintingStem[pupu] = stem;
						path = layer->second.path[fieldpbr::ALBEDO];
						stem = layer->second.stem[fieldpbr::ALBEDO];
					}
				it = next.imagePath.emplace(pupu, path).first;
				if (it->second.empty())
					missing.push_back(PupuHex(pupu));
				else
				{
					next.imageStem.emplace(pupu, stem);
					images++;
				}
			}
			hasImage[i] = !it->second.empty();
			tilesWithImage += hasImage[i];
		}
		if (images == 0)
		{
			if (FIELD_DEBUG)
				Log("FIELD_LAYERS: %s: folder found but no layer image matches this .map (expected names like %s%s%s_%s.png from FME, "
					"%s_%s.png from PuPu)\n", name.c_str(), folder.c_str(), name.c_str(), language.empty() ? "" : ("_" + language).c_str(),
					PupuHex(fme.pupu[0]).c_str(), name.c_str(), PupuHex(pupuNames.name[0]).c_str());
			std::lock_guard<std::mutex> lock(fieldMutex);
			field = std::move(next);
			return;
		}

		//cells of their own, written into the .map before the game uses it
		size_t moved = 0;
		UnshareResult unshare;
		if (FIELD_LAYERS_UNSHARE)
		{
			unshare = Unshare(tiles, hasImage);
			std::vector<Relocation> done;
			for (const Relocation& r : unshare.moved)
			{
				uint8_t tile[TILE_BYTES];
				const DWORD at = buffer + static_cast<DWORD>(r.tile * TILE_BYTES);
				if (!GuestRead(tile, at, TILE_BYTES))
					break;
				StoreSource(tile, r.texturePage, r.palette, r.srcX, r.srcY);
				if (!GuestWrite(at, tile, TILE_BYTES))
					break;
				done.push_back(r);
			}
			ApplyRelocations(tiles, done);
			moved = done.size();
		}
		next.pages = PlanPages(tiles, next.layout, hasImage, &next.shipped);
		next.active = true;

		if (FIELD_DEBUG)
		{
			Log("FIELD_LAYERS: %s%s%s: %zu tiles, canvas %dx%d (%s), %zu images (%zu named and framed the PuPu way, %zu FME's), "
				"%zu tiles drawn from them, %zu given their own cell, %zu sharing, %zu without a free cell, %zu pages%s\n", name.c_str(),
				language.empty() ? "" : " ", language.c_str(), tiles.size(), next.layout.width, next.layout.height,
				canvasFromSettings ? "from _pbr.txt" : "FME's", images, pupuPictures, pictures.pictures.size() - pupuPictures,
				tilesWithImage, moved, unshare.shared, unshare.unresolved, next.pages.size(), next.mim ? "" : " (.mim not read yet)");
			if (next.pbr != nullptr)
				Log("FIELD_PBR: %s: %zu PBR layer(s) (drawn from their _A, lit with %s), %zu light(s)%s%s\n", name.c_str(),
					next.pbr->layers.size(), next.pbr->settingsPath.empty() ? "estimated depth: no _pbr.txt" : "its _pbr.txt",
					next.pbr->lights.lights.size(), next.pbr->lightsPath.empty() ? " (no _lights.txt)" : "",
					next.pbr->settings.space.measured ? ", camera measured" : "");
			if (!missing.empty())
			{
				std::string list;
				for (size_t i = 0; i < missing.size() && i < 24; i++)
					list += " " + missing[i];
				Log("FIELD_LAYERS: %s: %zu layer image(s) not found (FME's name/PuPu's; their tiles use the game's own pixels):%s%s\n",
					name.c_str(), missing.size(), list.c_str(), missing.size() > 24 ? " ..." : "");
			}
			for (const auto& [key, page] : next.pages)
				Log("FIELD_LAYERS:   page %d .map palette %d (%d-bit): %zu cells from images%s%s%s\n", page.texturePage, page.palette,
					page.depth == 0 ? 4 : page.depth == 1 ? 8 : 16, page.stamps.size(),
					page.complete ? "" : ", the rest from the swizzled page or the game's",
					page.aligned ? "" : ", cells off the 16 grid", page.mixedDepth ? ", MIXED DEPTHS" : "");
		}
		std::lock_guard<std::mutex> lock(fieldMutex);
		field = std::move(next);
	}

	void OnFileRead(const DWORD nameGuest, const DWORD buffer, const DWORD size)
	{
		const std::string filename = GuestString(nameGuest);
		if (filename.size() < 5)
			return;
		std::string extension = filename.substr(filename.size() - 4);
		std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char c) { return static_cast<char>(tolower(c)); });
		if (extension == ".map")
			OnMapRead(filename, buffer, size);
		else if (extension == ".mim")
			OnMimRead(filename, buffer, size);
	}

	DWORD __cdecl ReadFileDetour(BYTE* state)
	{
		DWORD nameGuest = 0, buffer = 0;
		const DWORD esp = StateRegister(state, STATE_ESP);
		const bool arguments = !disabled && StackRead32(esp + 4, nameGuest) && StackRead32(esp + 8, buffer);
		const DWORD result = readFileOriginal(state);
		if (arguments)
		{
			try
			{
				OnFileRead(nameGuest, buffer, StateRegister(state, STATE_EAX));
			}
			catch (const std::exception& e)
			{
				disabled = true;
				Log("FIELD_LAYERS: reading the field failed (%s), layers turned off for this session\n", e.what());
			}
			catch (...)
			{
				disabled = true;
				Log("FIELD_LAYERS: reading the field failed, layers turned off for this session\n");
			}
		}
		return result;
	}

	//The field's VRAM as it is now, laid out like a .mim (so MatchPalette can read it): the CLUT of every .map palette
	//(where the tiles say it is) and the 13 texture pages at y 256. The game converts its pages from this VRAM, so this
	//matches even after the field changed colours (palette animation, fades) or pixels since the .mim was loaded.
	bool ReadLiveVram(const Field& f, std::vector<uint8_t>& out)
	{
		if (psxVram == 0)
			return false;
		out.assign(MIM_SIZE, 0);
		int clutX[16], clutY[16];
		for (int p = 0; p < 16; p++)
		{
			clutX[p] = 0;
			clutY[p] = -1;
		}
		for (const Tile& t : f.shipped)
			if (clutY[t.Palette()] < 0)
			{
				clutX[t.Palette()] = (t.palId & 0x3F) * 16;
				clutY[t.Palette()] = (t.palId >> 6) & 0x1FF;
			}
		constexpr DWORD ROW = 1024 * 2;
		for (int p = 0; p < 16; p++)
		{
			const int y = clutY[p] < 0 ? 240 + p : clutY[p]; //where FF8 puts a field's palettes
			const int entries = std::min(256, 1024 - clutX[p]);
			if (!GuestRead(out.data() + MIM_PALETTES + static_cast<size_t>(p) * 512, psxVram + static_cast<DWORD>(y) * ROW + clutX[p] * 2,
				static_cast<size_t>(entries) * 2))
				return false;
		}
		for (int r = 0; r < 256; r++)
			if (!GuestRead(out.data() + MIM_TEXTURE + static_cast<size_t>(r) * MIM_ROW, psxVram + static_cast<DWORD>(256 + r) * ROW, MIM_ROW))
				return false;
		return true;
	}

	//--- which CLUT the game loads into each palette slot (READ_VRAM_PALETTE / WRITE_PALETTE). write_palette_texture_set reads
	//a CLUT from VRAM (read_vram_palette) and hands it to the driver for one slot of a texture (write_palette_to_driver);
	//the texture is then built from that palette, possibly frames later and after the field changed the CLUT again. So
	//this is what tells the slots of a palette the field animates apart. Key: the texture's image data (a guest pointer,
	//the same one its tex_header holds) and the slot (dest_offset / size / 2 = tex_header palette_index / 2, as in FFNx).
	std::mutex clutMutex;
	DWORD lastClut = 0xFFFFFFFF;
	std::unordered_map<uint64_t, uint32_t> slotCluts;
	size_t paletteWrites = 0;

	uint64_t SlotKey(const DWORD imageData, const DWORD slotHalf)
	{
		return static_cast<uint64_t>(imageData) << 32 | slotHalf;
	}

	DWORD __cdecl ReadVramPaletteDetour(BYTE* state)
	{
		DWORD clut = 0;
		if (!disabled && StackRead32(StateRegister(state, STATE_ESP) + 4, clut))
		{
			std::lock_guard<std::mutex> lock(clutMutex);
			lastClut = clut & 0xFFFF;
		}
		return readVramPaletteOriginal(state);
	}

	DWORD __cdecl WritePaletteDetour(BYTE* state)
	{
		DWORD size = 0, destOffset = 0, textureSet = 0, texHeader = 0, imageData = 0;
		const DWORD esp = StateRegister(state, STATE_ESP);
		if (!disabled && StackRead32(esp + 8, size) && StackRead32(esp + 0x10, destOffset) && StackRead32(esp + 0x14, textureSet)
			&& size > 0 && size <= 256 && textureSet != 0 && GuestRead(&texHeader, textureSet + 0x98, 4) && texHeader != 0
			&& GuestRead(&imageData, texHeader + 0xD8, 4) && imageData != 0)
		{
			std::lock_guard<std::mutex> lock(clutMutex);
			if (lastClut != 0xFFFFFFFF)
			{
				if (slotCluts.size() > 16384)
					slotCluts.clear();
				slotCluts[SlotKey(imageData, destOffset / size / 2)] = lastClut;
				paletteWrites++;
			}
		}
		return writePaletteOriginal(state);
	}

	//the CLUT the game loaded into this slot, as a .map palette of this page (palette, depth), or -1
	int SlotPalette(const Field& f, const int texturePage, const DWORD imageData, const int slot, uint32_t& clut)
	{
		if (imageData == 0 || slot < 0)
			return -1;
		{
			std::lock_guard<std::mutex> lock(clutMutex);
			const auto it = slotCluts.find(SlotKey(imageData, static_cast<DWORD>(slot) / 2));
			if (it == slotCluts.end())
				return -1;
			clut = it->second;
		}
		int found = -1;
		for (const Tile& t : f.shipped)
			if (t.TexturePage() == texturePage && (t.palId & 0x7FFF) == (clut & 0x7FFF) && t.Depth() <= 2)
			{
				const int value = t.Palette() | t.Depth() << 8;
				if (found >= 0 && found != value)
					return -1; //the same CLUT at two depths on this page: let the pixels decide
				found = value;
			}
		return found;
	}

	//Which .map palette (and depth) the game's page texture (texture page, palette slot) was made with. fieldMutex held.
	//fromGame: what the game's palette write said (palette | depth << 8), -1 when not known
	bool IdentifyPalette(Field& f, const int texturePage, const int slot, const int fromGame, const uint32_t clut,
		const uint8_t* pixels, const int width, const int height, int& palette, int& depth)
	{
		uint64_t key = XXH3_64bits(pixels, static_cast<size_t>(width) * height * 4);
		const int what[3] = { texturePage, slot, fromGame };
		key = XXH3_64bits_withSeed(what, sizeof(what), key);
		const std::string where = f.name + " page " + std::to_string(texturePage) + " palette slot " + std::to_string(slot);
		int result = -1;
		if (const auto known = f.identified.find(key); known != f.identified.end())
			result = known->second;
		else
		{
			struct Source { const std::vector<uint8_t>* data; const char* name; };
			std::vector<uint8_t> live;
			std::vector<Source> sources;
			if (ReadLiveVram(f, live))
				sources.push_back({ &live, "VRAM" });
			if (f.mim != nullptr)
				sources.push_back({ f.mim.get(), ".mim" });

			const auto value = [](const PaletteMatch& m) { return m.palette | m.depth << 8; };
			const auto claimedByOther = [&](const int v)
			{
				for (const auto& [other, otherValue] : f.slots)
					if (std::get<0>(other) == texturePage && std::get<1>(other) == (v >> 8) && std::get<2>(other) != slot && otherValue == v)
						return true;
				return false;
			};
			//among palettes that look the same here: the game's word, else the one this slot had, else one no other slot has
			const auto pick = [&](const std::vector<PaletteMatch>& matches) -> const PaletteMatch*
			{
				std::vector<const PaletteMatch*> tied;
				for (const PaletteMatch& m : matches)
					if (m.Score() == matches[0].Score())
						tied.push_back(&m);
				if (tied.size() == 1)
					return tied[0];
				for (const PaletteMatch* m : tied)
					if (value(*m) == fromGame)
						return m;
				for (const PaletteMatch* m : tied)
					if (const auto previous = f.slots.find(std::make_tuple(texturePage, m->depth, slot));
						previous != f.slots.end() && previous->second == value(*m))
						return m;
				for (const PaletteMatch* m : tied)
					if (!claimedByOther(value(*m)))
						return m;
				return tied[0];
			};
			const PaletteMatch* chosen = nullptr;
			std::vector<PaletteMatch> chosenFrom;
			std::string how, report;
			std::vector<PaletteMatch> first; //from the first source, for the last resort
			for (const Source& source : sources)
			{
				std::vector<PaletteMatch> matches = MatchPalette(source.data->data(), source.data->size(), f.shipped, texturePage, pixels,
					width, height);
				char line[200];
				snprintf(line, sizeof(line), "%s%s: best palette %d %.1f%%", report.empty() ? "" : ", ", source.name,
					matches.empty() ? -1 : matches[0].palette, matches.empty() ? 0.0 : matches[0].Score() * 100.0);
				report += line;
				if (first.empty())
					first = matches;
				if (matches.empty() || matches[0].opaque < 64 || matches[0].Score() < 0.9) //opaque < 64: all but transparent here
					continue;
				chosenFrom = std::move(matches);
				chosen = pick(chosenFrom);
				how = std::string("in ") + source.name;
				break;
			}
			if (chosen == nullptr && fromGame >= 0)
			{
				//the pixels match no palette as it is now or as shipped (the field changes it): the game's word, unless its
				//palette looks nothing like the page at all
				PaletteMatch m;
				m.palette = fromGame & 0xFF;
				m.depth = fromGame >> 8;
				for (const PaletteMatch& c : first)
					if (value(c) == fromGame)
						m = c;
				if (first.empty() || m.Score() >= 0.2)
				{
					chosenFrom = { m };
					chosen = &chosenFrom[0];
					how = "from the game's palette write";
				}
			}
			if (chosen == nullptr && !first.empty())
			{
				//last resort: a clearly best palette among those no other slot of the page has
				std::vector<PaletteMatch> open;
				for (const PaletteMatch& m : first)
					if (!claimedByOther(value(m)))
						open.push_back(m);
				const double second = open.size() > 1 ? open[1].Score() : 0.0;
				if (!open.empty() && open[0].opaque >= 64 && open[0].Score() >= 0.5 && open[0].Score() - second >= 0.2)
				{
					chosenFrom = std::move(open);
					chosen = &chosenFrom[0];
					how = "partly, among palettes no other slot has";
				}
			}
			if (chosen != nullptr)
			{
				result = value(*chosen);
				if (FIELD_DEBUG)
				{
					std::string game;
					char clutText[96];
					snprintf(clutText, sizeof(clutText), "CLUT %u,%u", (clut & 0x3F) * 16, (clut >> 6) & 0x1FF);
					if (fromGame < 0)
						game = paletteHooks ? "; no palette write seen for this slot" : "";
					else if (fromGame == result)
						game = std::string("; the game loaded ") + clutText + " into it, same palette";
					else
						game = std::string("; THE GAME LOADED ") + clutText + " INTO IT (.map palette " + std::to_string(fromGame & 0xFF) + ")";
					const double runnerUp = chosenFrom.size() > 1 ? (&chosenFrom[0] == chosen ? chosenFrom[1] : chosenFrom[0]).Score() : 0.0;
					LogOnce(where + "-" + std::to_string(f.entry) + "-" + std::to_string(result),
						"FIELD_LAYERS: %s is .map palette %d (%d-bit): %.1f%% of %zu texels match %s, next best %.1f%%%s\n",
						where.c_str(), chosen->palette, chosen->depth == 0 ? 4 : chosen->depth == 1 ? 8 : 16, chosen->Score() * 100.0,
						chosen->compared, how.c_str(), runnerUp * 100.0, game.c_str());
				}
			}
			else if (FIELD_DEBUG)
				LogOnce(where + "-none-" + std::to_string(f.entry), "FIELD_LAYERS: %s: no palette the .map uses on that page matches "
					"(%s)%s: left as the game makes it\n", where.c_str(), report.empty() ? "nothing to compare with" : report.c_str(),
					paletteHooks ? ", no palette write seen for this slot" : "");
			if (f.identified.size() > 4096)
				f.identified.clear();
			f.identified[key] = result;
		}
		if (result < 0)
			return false;
		palette = result & 0xFF;
		depth = result >> 8;
		f.slots[std::make_tuple(texturePage, depth, slot)] = result;
		return true;
	}
}

//--- renderer side

namespace
{
	//layer images: kept across fields (battles come back to the same field), trimmed by size
	struct LayerImage
	{
		SafeBimg file = SafeBimgInit();
		bool loaded = false, bc7 = false;
		int width = 0, height = 0;
		std::filesystem::file_time_type time{};
		GLuint bc7Texture = 0; //BC7 files, on the GPU as they are
		Image rgba; //everything else (and BC7 when the CPU has to put a page together)
		size_t bytes = 0;
	};
	std::unordered_map<std::string, LayerImage> layers;
	size_t layerBytes = 0;
	constexpr size_t LAYER_LIMIT = 768u << 20;

	void FreeLayer(LayerImage& layer)
	{
		if (layer.bc7Texture != 0)
			glDeleteTextures(1, &layer.bc7Texture);
		layer.bc7Texture = 0;
		layerBytes -= std::min(layerBytes, layer.bytes);
	}

	void TrimLayers()
	{
		if (layerBytes <= LAYER_LIMIT)
			return;
		for (auto& [path, layer] : layers)
			FreeLayer(layer);
		layers.clear();
		layerBytes = 0;
	}

	LayerImage LoadLayer(const std::string& path)
	{
		LayerImage layer;
		layer.loaded = true;
		std::error_code error;
		layer.time = std::filesystem::last_write_time(path, error);
		layer.file = LoadImageFromFile(path.c_str());
		if (layer.file)
		{
			layer.width = static_cast<int>(layer.file->m_width);
			layer.height = static_cast<int>(layer.file->m_height);
			layer.bc7 = layer.file->m_format == bimg::TextureFormat::BC7;
			layer.bytes = layer.file->m_size;
		}
		return layer;
	}

	LayerImage& Layer(const std::string& path)
	{
		LayerImage& layer = layers[path];
		if (layer.loaded)
			return layer;
		layer = LoadLayer(path);
		layerBytes += layer.bytes;
		if (!layer.file)
			LogOnce("bad-" + path, "FIELD_LAYERS: cannot read %s\n", path.c_str());
		return layer;
	}

	const Image* LayerRgba(LayerImage& layer)
	{
		if (layer.rgba.IsValid())
			return &layer.rgba;
		if (!layer.file)
			return nullptr;
		static bx::DefaultAllocator allocator;
		if (bimg::ImageContainer* rgba = bimg::imageConvert(&allocator, bimg::TextureFormat::RGBA8, *layer.file, false))
		{
			const size_t bytes = static_cast<size_t>(rgba->m_width) * rgba->m_height * 4;
			if (rgba->m_size >= bytes)
			{
				layer.rgba.width = static_cast<int>(rgba->m_width);
				layer.rgba.height = static_cast<int>(rgba->m_height);
				layer.rgba.rgba.assign(static_cast<const uint8_t*>(rgba->m_data), static_cast<const uint8_t*>(rgba->m_data) + bytes);
				layer.bytes += bytes;
				layerBytes += bytes;
			}
			bimg::imageFree(rgba);
		}
		return layer.rgba.IsValid() ? &layer.rgba : nullptr;
	}

	GLuint LayerBc7Texture(LayerImage& layer)
	{
		if (layer.bc7Texture != 0 || !layer.bc7 || !layer.file)
			return layer.bc7Texture;
		bimg::ImageMip mip{};
		if (!bimg::imageGetRawData(*layer.file, 0, 0, layer.file->m_data, layer.file->m_size, mip))
			return 0;
		glGenTextures(1, &layer.bc7Texture);
		glBindTexture(GL_TEXTURE_2D, layer.bc7Texture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); //a complete texture (glCopyImageSubData needs one)
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_BPTC_UNORM, layer.width, layer.height, 0,
			static_cast<GLsizei>(mip.m_size), mip.m_data);
		return layer.bc7Texture;
	}

	//a layer file used for the tiles of picture `pupu` (its painting, its albedo, a PBR map): the frame it covers (the
	//picture's own, the canvas or FME's canvas: the one its size matches) and its scale; 0 when it matches none
	int FitLayer(const LayerImage& layer, const Layout& layout, const uint32_t pupu, Frame& frame)
	{
		return layer.width > 0 && layer.height > 0 ? FitPicture(layout, pupu, layer.width, layer.height, frame) : 0;
	}

	//pages DEMASTER put together (game texture -> what and how), for FIELD_SMOOTH and FIELD_LAYERS_LIVE
	struct FieldPage
	{
		int width = 0, height = 0; //HD size
		int logicalWidth = 256, logicalHeight = 256; //texels
		PageJob job;
		ULONGLONG checked = 0; //when its size was last compared with the texture's (FieldLayersBeforeDraw)
	};
	std::unordered_map<GLuint, FieldPage> fieldPages;
	uint64_t fieldPagesEntry = 0;
	//Pages taken off the list while the game gave their texture nothing new (the field's .map was read again, or the
	//texture could not be checked): the picture in the texture is still DEMASTER's, so the page is taken back the next time
	//it is drawn (TakeBack) instead of being drawn unlit by the game's program. A new upload by the game forgets it.
	std::unordered_map<GLuint, FieldPage> parkedPages;

	//Every page that leaves the list, with the reason (FIELD_DEBUG). Up to 400 lines a session.
	void LogDrop(const GLuint texture, const FieldPage& page, const char* why)
	{
		static int lines = 0;
		if (!FIELD_DEBUG || lines >= 400)
			return;
		lines++;
		Log("FIELD_LAYERS: %s page %u .map palette %d (texture %u, palette slot %d) is off DEMASTER's list: %s%s\n",
			page.job.name.c_str(), page.job.tPage - 16, page.job.palette, texture, page.job.slot, why,
			lines == 400 ? " (no more of these lines this session)" : "");
	}

	//A .map was read (the next field's, or this field's again): the list is for the new entry
	void ParkAll()
	{
		parkedPages.clear(); //only the pages of the last list are kept
		if (FIELD_DEBUG && !fieldPages.empty())
			Log("FIELD_LAYERS: %s: a .map was read, its %zu page(s) are off DEMASTER's list until the game uploads them again "
				"(or draws them as they are: then they are taken back)\n", fieldPages.begin()->second.job.name.c_str(), fieldPages.size());
		for (auto& [texture, page] : fieldPages)
			parkedPages.emplace(texture, std::move(page));
		fieldPages.clear();
	}

	//the pending page (_fbgCheck -> FbgGl)
	PageJob pending;
	bool pendingSet = false;

	//composed on the CPU, by page, palette and (when used) the game's page content
	std::unordered_map<uint64_t, Image> composed;
	size_t composedBytes = 0;
	constexpr size_t COMPOSED_LIMIT = 256u << 20;

	void RawTexImage2D(const GLsizei width, const GLsizei height, const GLenum format, const void* data)
	{
		//straight to the driver: DEMASTER's own glTexImage2D hook would hash (and maybe replace) these
		if (ogl_tex_image2d != nullptr)
			reinterpret_cast<void* (__stdcall*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)>(ogl_tex_image2d)
			(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, format, GL_UNSIGNED_BYTE, data);
		else
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, format, GL_UNSIGNED_BYTE, data);
	}

	void PageParameters()
	{
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	}

	//BC7 files at one scale, every cell on the 4x4 block grid, nothing needed from the game's page: copy blocks
	bool ComposeOnGpu(const GLuint target, const PageJob& job, const int logicalWidth, const int logicalHeight, const int scale, std::string& why)
	{
		CheckGlew();
		if (glCopyImageSubData == nullptr || glCompressedTexImage2D == nullptr)
		{
			why = "no glCopyImageSubData (OpenGL 4.3)";
			return false;
		}
		const int width = logicalWidth * scale, height = logicalHeight * scale, block = TILE_SIZE * scale;
		if (!job.gameAlpha.empty())
		{
			why = "semi-transparent tiles take their alpha from the game's page";
			return false;
		}
		GLuint baseTexture = 0;
		if (!job.plan.complete)
		{
			if (job.basePath.empty())
			{
				why = "part of the page has no image (the game's own pixels are used there)";
				return false;
			}
			LayerImage& base = Layer(job.basePath);
			if (!base.bc7 || base.width != width || base.height != height)
			{
				why = "part of the page comes from the swizzled page, which is not BC7 at the same size";
				return false;
			}
			baseTexture = LayerBc7Texture(base);
			if (baseTexture == 0)
			{
				why = "could not upload the swizzled page";
				return false;
			}
		}
		struct Copy { GLuint texture; int x, y, cx, cy; };
		std::vector<Copy> copies;
		for (const CellStamp& stamp : job.plan.stamps)
		{
			const auto path = job.imagePath.find(stamp.pupu);
			if (path == job.imagePath.end() || path->second.empty())
				continue;
			LayerImage& layer = Layer(path->second);
			if (!layer.bc7)
			{
				why = "not all images are BC7";
				return false;
			}
			Frame frame;
			if (FitLayer(layer, job.layout, stamp.pupu, frame) != scale)
			{
				why = "images of different scales";
				return false;
			}
			const int x = (stamp.mapX - frame.left) * scale, y = (stamp.mapY - frame.top) * scale, cx = stamp.cellX * scale,
				cy = stamp.cellY * scale;
			if (x % 4 || y % 4 || cx % 4 || cy % 4)
			{
				why = "a tile off the 4x4 block grid at this scale";
				return false;
			}
			const GLuint texture = LayerBc7Texture(layer);
			if (texture == 0)
			{
				why = "could not upload a BC7 image";
				return false;
			}
			copies.push_back({ texture, x, y, cx, cy });
		}
		if (width % 4 || height % 4)
		{
			why = "page size off the block grid";
			return false;
		}
		GLint unpackBuffer = 0;
		glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
		if (unpackBuffer != 0)
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++) {}
		static std::vector<uint8_t> zero;
		const size_t bytes = static_cast<size_t>(width) * height; //BC7: one byte per texel
		if (zero.size() < bytes)
			zero.assign(bytes, 0); //an all-zero BC7 block decodes to transparent black
		glBindTexture(GL_TEXTURE_2D, target);
		glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_BPTC_UNORM, width, height, 0, static_cast<GLsizei>(bytes), zero.data());
		PageParameters();
		if (baseTexture != 0)
			glCopyImageSubData(baseTexture, GL_TEXTURE_2D, 0, 0, 0, 0, target, GL_TEXTURE_2D, 0, 0, 0, 0, width, height, 1);
		for (const Copy& c : copies)
		{
			if (c.cx + block > width || c.cy + block > height)
				continue;
			glCopyImageSubData(c.texture, GL_TEXTURE_2D, 0, c.x, c.y, 0, target, GL_TEXTURE_2D, 0, c.cx, c.cy, 0, block, block, 1);
		}
		const bool ok = glGetError() == GL_NO_ERROR;
		if (unpackBuffer != 0)
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
		glBindTexture(GL_TEXTURE_2D, target);
		if (!ok)
			why = "OpenGL error while copying";
		return ok;
	}

	void ComposeOnCpu(const GLuint target, const PageJob& job, const int logicalWidth, const int logicalHeight, const int scale)
	{
		uint64_t key = XXH3_64bits_withSeed(&job.tPage, sizeof(job.tPage), job.entry);
		const int which[3] = { job.palette, job.depth, job.kind };
		key = XXH3_64bits_withSeed(which, sizeof(which), key);
		for (const CellStamp& stamp : job.plan.stamps) //the files (painting or albedo, PBR maps)
			if (const auto path = job.imagePath.find(stamp.pupu); path != job.imagePath.end())
				key = XXH3_64bits_withSeed(path->second.data(), path->second.size(), key);
		if (!job.base.empty())
			key = XXH3_64bits_withSeed(job.base.data(), job.base.size(), key);
		if (!job.basePath.empty())
			key = XXH3_64bits_withSeed(job.basePath.data(), job.basePath.size(), key);
		if (!job.gameAlpha.empty())
			key = XXH3_64bits_withSeed(job.gameAlpha.data(), job.gameAlpha.size(), key);
		auto it = composed.find(key);
		if (it == composed.end())
		{
			std::vector<StampSource> sources;
			for (const CellStamp& stamp : job.plan.stamps)
			{
				const auto path = job.imagePath.find(stamp.pupu);
				if (path == job.imagePath.end() || path->second.empty())
					continue;
				LayerImage& layer = Layer(path->second);
				Frame frame;
				const int layerScale = FitLayer(layer, job.layout, stamp.pupu, frame);
				if (layerScale == 0)
				{
					if (layer.file)
					{
						const auto own = job.layout.frames.find(stamp.pupu);
						const Frame picture = own != job.layout.frames.end() ? own->second : job.layout.canvas;
						LogOnce("size-" + path->second + std::to_string(layer.width) + "x" + std::to_string(layer.height),
							"FIELD_LAYERS: %s is %dx%d, not its picture's frame (%dx%d), the canvas (%dx%d) or FME's canvas (%dx%d) "
							"times a whole number: not used\n", path->second.c_str(), layer.width, layer.height, picture.width, picture.height,
							job.layout.canvas.width, job.layout.canvas.height, job.layout.fme.width, job.layout.fme.height);
					}
					continue;
				}
				const Image* rgba = LayerRgba(layer);
				if (rgba == nullptr)
					continue;
				//PBR maps other than albedo hold data in alpha, not coverage
				sources.push_back({ rgba, layerScale, stamp.mapX - frame.left, stamp.mapY - frame.top, stamp.cellX, stamp.cellY,
					job.kind <= fieldpbr::ALBEDO });
			}
			if (composedBytes > COMPOSED_LIMIT)
			{
				composed.clear();
				composedBytes = 0;
			}
			const Image* swizzled = job.basePath.empty() ? nullptr : LayerRgba(Layer(job.basePath));
			Image page = swizzled != nullptr
				? ComposePage(logicalWidth, logicalHeight, scale, swizzled->rgba.data(), swizzled->width, swizzled->height, false, sources)
				: ComposePage(logicalWidth, logicalHeight, scale, job.base.empty() ? nullptr : job.base.data(),
					job.width, job.height, true, sources);
			if (!job.gameAlpha.empty())
				InheritSemiTransparency(page, scale, job.plan.stamps, job.gameAlpha.data(), job.width, job.height);
			composedBytes += page.rgba.size();
			it = composed.emplace(key, std::move(page)).first;
		}
		glBindTexture(GL_TEXTURE_2D, target);
		GLint unpackBuffer = 0, alignment = 4, rowLength = 0;
		glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
		glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
		glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
		if (unpackBuffer != 0)
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		RawTexImage2D(it->second.width, it->second.height, GL_RGBA, it->second.rgba.data());
		PageParameters();
		glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
		if (unpackBuffer != 0)
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
	}

	//Fills target from the job (target is bound afterwards)
	PageSize BuildJob(const GLuint target, const PageJob& job, std::string& how)
	{
		//the page in texels (the game may already send it bigger than 256 wide), and the scale of its images
		const int pageScale = job.width % 256 == 0 && job.width >= 256 ? job.width / 256 : 1;
		const int logicalWidth = job.width / pageScale, logicalHeight = std::max(1, job.height / pageScale);
		int scale = 0;
		for (const CellStamp& stamp : job.plan.stamps)
			if (const auto path = job.imagePath.find(stamp.pupu); path != job.imagePath.end() && !path->second.empty())
			{
				Frame frame;
				scale = std::max(scale, FitLayer(Layer(path->second), job.layout, stamp.pupu, frame));
			}
		if (!job.basePath.empty())
		{
			const LayerImage& base = Layer(job.basePath);
			if (base.width > 0 && base.width % logicalWidth == 0)
				scale = std::max(scale, std::min(16, base.width / logicalWidth));
		}
		if (scale == 0)
			scale = std::max(1, pageScale);
		std::string why;
		const bool gpu = ComposeOnGpu(target, job, logicalWidth, logicalHeight, scale, why);
		if (!gpu)
			ComposeOnCpu(target, job, logicalWidth, logicalHeight, scale);
		PageSize size;
		size.width = logicalWidth * scale;
		size.height = logicalHeight * scale;
		size.logicalWidth = logicalWidth;
		size.logicalHeight = logicalHeight;
		char text[200];
		snprintf(text, sizeof(text), "%dx%d at %dx, %s%s%s", size.width, size.height, scale,
			gpu ? "BC7 block copies on the GPU" : "put together on the CPU", gpu ? "" : " (", gpu ? "" : (why + ")").c_str());
		how = text;
		return size;
	}

	//Fills target from page.job (target is bound afterwards) and sets the page's sizes
	void BuildInto(const GLuint target, FieldPage& page, std::string& how)
	{
		const PageSize size = BuildJob(target, page.job, how);
		page.width = size.width;
		page.height = size.height;
		page.logicalWidth = size.logicalWidth;
		page.logicalHeight = size.logicalHeight;
	}

	PageSize SizeOf(const FieldPage& page)
	{
		PageSize size;
		size.width = page.width;
		size.height = page.height;
		size.logicalWidth = page.logicalWidth;
		size.logicalHeight = page.logicalHeight;
		return size;
	}

	//main.frag without PBR: a page's PBR layers back to their paintings
	void ToPaintings(PageJob& job)
	{
		for (const auto& [pupu, path] : job.paintingPath)
			job.imagePath[pupu] = path;
		job.paintingPath.clear();
		job.pbr = nullptr;
	}

	//main.frag has no PBR: every PBR layer goes back to its painting, pages already made are made again (once)
	void UsePaintings()
	{
		static bool done = false;
		if (done)
			return;
		done = true;
		{
			std::lock_guard<std::mutex> lock(fieldMutex);
			for (const auto& [pupu, path] : field.paintingPath)
			{
				field.imagePath[pupu] = path;
				if (const auto stem = field.paintingStem.find(pupu); stem != field.paintingStem.end())
					field.imageStem[pupu] = stem->second;
			}
			field.paintingPath.clear();
			field.paintingStem.clear();
			field.pbr = nullptr;
		}
		GLint bound = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
		for (auto& [texture, page] : fieldPages)
		{
			if (page.job.paintingPath.empty())
				continue;
			ToPaintings(page.job);
			std::string how;
			BuildInto(texture, page, how);
		}
		PbrForgetAll();
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
	}

	//FIELD_DEBUG=2: every page DEMASTER puts together, once, as DEMASTER_EXP\field_dump\<field>_<page>_<.map palette>.png
	void DumpPage(const GLuint target, const PageJob& job, const int width, const int height)
	{
		static std::set<std::string> dumped;
		char name[160];
		snprintf(name, sizeof(name), "%s_%u_%d", job.name.c_str(), static_cast<unsigned>(job.tPage - 16), job.palette);
		if (!dumped.insert(name).second)
			return;
		std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
		GLint packAlignment = 4;
		glGetIntegerv(GL_PACK_ALIGNMENT, &packAlignment);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		glBindTexture(GL_TEXTURE_2D, target);
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data()); //the driver decodes BC7 here
		glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
		const std::string folder = std::string(DIRECT_IO_EXPORT_DIR) + "field_dump";
		std::error_code error;
		std::filesystem::create_directories(folder, error);
		const std::string path = folder + "\\" + name + ".png";
		bx::FileWriter writer;
		if (!bx::open(&writer, bx::FilePath(path.c_str())))
			return;
		bimg::imageWritePng(&writer, width, height, width * 4, pixels.data(), bimg::TextureFormat::RGBA8, false);
		bx::close(&writer);
	}

	//FIELD_LAYERS_LIVE: layer images of the field changed on disk (saved, or a newer .dds/.png next to them) are read again
	//and the field's pages that use them rebuilt in place. Renderer thread, between draws.
	void PollLayerFiles()
	{
		if (!FIELD_LAYERS_LIVE || fieldPages.empty())
			return;
		static ULONGLONG last = 0;
		const ULONGLONG now = GetTickCount64();
		if (now - last < 1000)
			return;
		last = now;
		std::unordered_map<uint32_t, std::string> stems, paths;
		std::string name;
		{
			std::lock_guard<std::mutex> lock(fieldMutex);
			if (!field.active || field.entry != fieldPagesEntry)
				return;
			stems = field.imageStem;
			paths = field.imagePath;
			name = field.name;
		}
		std::unordered_map<uint32_t, std::string> changed;
		std::vector<std::string> changedFiles;
		for (const auto& [pupu, stem] : stems)
		{
			const ImageFile file = ResolveImage(stem);
			if (file.path.empty())
				continue; //gone: what is loaded stays
			bool differs = file.path != paths[pupu];
			if (!differs)
			{
				const auto it = layers.find(file.path);
				differs = it != layers.end() && it->second.loaded && it->second.time != file.time;
			}
			if (!differs)
				continue;
			//read it now: a file still being written fails here and is tried again next time
			LayerImage fresh = LoadLayer(file.path);
			if (!fresh.file)
				continue;
			LayerImage& slot = layers[file.path];
			FreeLayer(slot);
			slot = std::move(fresh);
			layerBytes += slot.bytes;
			changed.emplace(pupu, file.path);
			changedFiles.push_back(std::filesystem::path(file.path).filename().string());
		}
		if (changed.empty())
			return;
		{
			std::lock_guard<std::mutex> lock(fieldMutex);
			if (field.entry == fieldPagesEntry)
				for (const auto& [pupu, path] : changed)
					field.imagePath[pupu] = path;
		}
		composed.clear();
		composedBytes = 0;
		GLint bound = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
		size_t rebuilt = 0;
		for (auto it = fieldPages.begin(); it != fieldPages.end();)
		{
			FieldPage& page = it->second;
			bool uses = false;
			for (const CellStamp& stamp : page.job.plan.stamps)
				if (changed.count(stamp.pupu))
				{
					uses = true;
					break;
				}
			if (!uses)
			{
				++it;
				continue;
			}
			//still the page DEMASTER filled? (the game may have given the texture to something else)
			GLint width = 0;
			if (glIsTexture(it->first))
			{
				glBindTexture(GL_TEXTURE_2D, it->first);
				glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
			}
			if (width != page.width)
			{
				PbrForget(it->first);
				it = fieldPages.erase(it);
				continue;
			}
			for (const auto& [pupu, path] : changed)
				if (const auto own = page.job.imagePath.find(pupu); own != page.job.imagePath.end())
					own->second = path;
			std::string how;
			BuildInto(it->first, page, how);
			rebuilt++;
			++it;
		}
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
		if (FIELD_DEBUG)
		{
			std::string list;
			for (size_t i = 0; i < changedFiles.size() && i < 8; i++)
				list += " " + changedFiles[i];
			Log("FIELD_LAYERS: %s: read again:%s%s; %zu page(s) rebuilt\n", name.c_str(), list.c_str(),
				changedFiles.size() > 8 ? " ..." : "", rebuilt);
		}
	}
}

//--- shared with the PBR module (field_pbr_runtime.h), renderer thread
namespace fieldrt
{
	PageSize BuildPage(const GLuint target, const PageJob& job, std::string& how)
	{
		return BuildJob(target, job, how);
	}

	bool RefreshLayerFile(const ImageFile& file)
	{
		if (file.path.empty())
			return false;
		const auto it = layers.find(file.path);
		if (it != layers.end() && it->second.loaded && it->second.time == file.time)
			return false;
		LayerImage fresh = LoadLayer(file.path);
		if (!fresh.file)
			return false; //still being written: next time
		LayerImage& slot = layers[file.path];
		FreeLayer(slot);
		slot = std::move(fresh);
		layerBytes += slot.bytes;
		return true;
	}

	void ForgetComposed()
	{
		composed.clear();
		composedBytes = 0;
	}
}

//_bsp (renderer thread): the game is about to upload page tPage; header: its tex_header (palette_index at [52], image_data
//at [54], FFNx's ff8_tex_header). true: DEMASTER puts it together (FieldLayersUploadPending, from FbgGl).
bool FieldLayersBuildPage(const DWORD tPage, const int textureType, const DWORD* header)
{
	pendingSet = false;
	if (!FIELD_LAYERS || !hooksInstalled || disabled || textureType != FIELDBG_TEXTURE_TYPE || pixelsPtr == 0 || tPage < 16 || tPage >= 32
		|| header == nullptr)
		return false;
	try
	{
		const int width = static_cast<int>(uploadWidth), height = static_cast<int>(uploadHeight);
		if (width < 16 || height < 16 || width > 4096 || height > 4096)
			return false;
		const uint8_t* pixels = reinterpret_cast<const uint8_t*>(pixelsPtr);
		std::lock_guard<std::mutex> lock(fieldMutex);
		if (!field.active)
			return false;
		const int texturePage = static_cast<int>(tPage) - 16;
		const int slot = static_cast<int>(header[52]);
		uint32_t clut = 0;
		const int fromGame = paletteHooks ? SlotPalette(field, texturePage, header[54], slot, clut) : -1;
		int palette = 0, depth = 0;
		if (!IdentifyPalette(field, texturePage, slot, fromGame, clut, pixels, width, height, palette, depth))
			return false;
		auto it = field.pages.end();
		if (depth == 2)
		{
			//16-bit pages have no palette
			for (auto other = field.pages.begin(); other != field.pages.end(); ++other)
				if (other->second.texturePage == texturePage && other->second.depth == 2)
				{
					it = other;
					break;
				}
		}
		else
			it = field.pages.find(PageKey(texturePage, palette));
		if (it == field.pages.end())
		{
			if (FIELD_DEBUG)
				LogOnce(field.name + "-nopage-" + std::to_string(field.entry) + "-" + std::to_string(tPage) + "-" + std::to_string(palette),
					"FIELD_LAYERS: %s: no plan for page %d .map palette %d (%dx%d)\n", field.name.c_str(), texturePage, palette, width, height);
			return false;
		}
		pending = PageJob();
		pending.entry = field.entry;
		pending.name = field.name;
		pending.layout = field.layout;
		pending.plan = it->second;
		for (const CellStamp& stamp : it->second.stamps)
		{
			if (const auto path = field.imagePath.find(stamp.pupu); path != field.imagePath.end())
				pending.imagePath.emplace(stamp.pupu, path->second);
			if (const auto painting = field.paintingPath.find(stamp.pupu); painting != field.paintingPath.end())
				pending.paintingPath.emplace(stamp.pupu, painting->second);
		}
		pending.pbr = field.pbr;
		pending.width = width;
		pending.height = height;
		pending.tPage = tPage;
		pending.slot = slot;
		pending.palette = palette;
		pending.depth = depth;
		if (!it->second.complete)
		{
			//tiles without an image: from the field's swizzled HD page if there is one (so a field can move to layers a
			//few images at a time; named with the game's palette numbers, as FIELD_BACKGROUND does), else from the game's
			//own page
			char path[MAX_PATH]{ 0 };
			const std::string folder = FieldFolder(field.name);
			if (!DDSorPNG(path, MAX_PATH, "%s%s_%d_%d", folder.c_str(), field.name.c_str(), texturePage, slot)
				|| !DDSorPNG(path, MAX_PATH, "%s%s_%d", folder.c_str(), field.name.c_str(), texturePage))
				pending.basePath = path;
			else if (it->second.stamps.empty())
				return false; //nothing from images, no swizzled page: the game's own page as it is
			else
				pending.base.assign(pixels, pixels + static_cast<size_t>(width) * height * 4);
		}
		//semi-transparent tiles: which of their texels the game blends (alpha neither 0 nor 255 in its page)
		if (width % 256 == 0 && height % 256 == 0)
		{
			size_t counts[3] = { 0, 0, 0 }; //transparent, in between, opaque
			const int scaleX = width / 256, scaleY = height / 256;
			for (const CellStamp& stamp : it->second.stamps)
			{
				if (!stamp.semi || stamp.originX < 0)
					continue;
				for (int y = stamp.originY; y < stamp.originY + TILE_SIZE && y < 256; y++)
					for (int x = stamp.originX; x < stamp.originX + TILE_SIZE && x < 256; x++)
					{
						const uint8_t a = pixels[(static_cast<size_t>(y) * scaleY * width + static_cast<size_t>(x) * scaleX) * 4 + 3];
						counts[a == 0 ? 0 : a == 255 ? 2 : 1]++;
					}
			}
			if (counts[1] > 0)
			{
				pending.gameAlpha.resize(static_cast<size_t>(width) * height);
				for (size_t i = 0; i < pending.gameAlpha.size(); i++)
					pending.gameAlpha[i] = pixels[i * 4 + 3];
			}
			if (FIELD_DEBUG && counts[0] + counts[1] + counts[2] > 0)
				LogOnce(field.name + "-semi-" + std::to_string(field.entry) + "-" + std::to_string(texturePage) + "-" + std::to_string(palette),
					"FIELD_LAYERS: %s page %d .map palette %d: semi-transparent tiles, the game's texels there: %zu transparent, %zu "
					"blended, %zu opaque%s\n", field.name.c_str(), texturePage, palette, counts[0], counts[1], counts[2],
					counts[1] > 0 ? " (the images get the same alpha)" : "");
		}
		pendingSet = true;
		return true;
	}
	catch (const std::exception& e)
	{
		disabled = true;
		Log("FIELD_LAYERS: page %u failed (%s), layers turned off for this session\n", tPage, e.what());
	}
	catch (...)
	{
		disabled = true;
		Log("FIELD_LAYERS: page %u failed, layers turned off for this session\n", tPage);
	}
	pendingSet = false;
	return false;
}

bool FieldLayersUploadPending()
{
	if (!pendingSet)
		return false;
	pendingSet = false;
	try
	{
		CheckGlew();
		GLint bound = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
		const GLuint target = static_cast<GLuint>(bound);
		if (fieldPagesEntry != pending.entry)
		{
			ParkAll();
			PbrForgetAll();
			fieldPagesEntry = pending.entry;
		}
		TrimLayers();
		parkedPages.erase(target);
		FieldPage& page = fieldPages[target];
		page.job = std::move(pending);
		if (PbrShaderMissing())
			ToPaintings(page.job);
		std::string how;
		BuildInto(target, page, how);
		PbrPageBuilt(target, page.job, SizeOf(page)); //its PBR maps (when it has PBR layers)
		const PageJob& job = page.job;
		if (FIELD_DEBUG)
			LogOnce(job.name + "-" + std::to_string(job.entry) + "-" + std::to_string(job.tPage) + "-" + std::to_string(job.slot),
				"FIELD_LAYERS: %s page %u palette slot %d (.map palette %d): %zu cells from images%s, %s\n", job.name.c_str(),
				job.tPage - 16, job.slot, job.palette, job.plan.stamps.size(),
				job.plan.complete ? "" : job.basePath.empty() ? ", the rest from the game" : ", the rest from the swizzled page",
				how.c_str());
		if (FIELD_DEBUG >= 2)
			DumpPage(target, job, page.width, page.height);
		glBindTexture(GL_TEXTURE_2D, target);
	}
	catch (...)
	{
		disabled = true;
		Log("FIELD_LAYERS: putting a page together failed, layers turned off for this session\n");
	}
	pending = PageJob();
	return true;
}

//FIELD_FRAME_LOG: is this texture a field page DEMASTER put together; the field being played
bool FieldLayersIsPage(const unsigned int texture)
{
	return fieldPages.count(static_cast<GLuint>(texture)) != 0;
}

std::string FieldLayersFieldName()
{
	std::lock_guard<std::mutex> lock(fieldMutex);
	return field.name;
}

//The game re-specified a texture (glTexImage2D hook): if it was one of ours, it is not any more
void FieldLayersForgetTexture(const unsigned int texture)
{
	if (const auto it = fieldPages.find(static_cast<GLuint>(texture)); it != fieldPages.end())
	{
		LogDrop(it->first, it->second, "the game gave the texture a picture of its own (an upload DEMASTER did not recognise)");
		fieldPages.erase(it);
	}
	parkedPages.erase(static_cast<GLuint>(texture));
	PbrForget(static_cast<GLuint>(texture));
}

namespace
{
	//The width of a texture the game has bound for the draw, from the driver. false: the driver has another texture bound
	//there than the one remembered (FIELD_GL_TRACK), so the answer would be about the wrong texture.
	bool DriverWidth(const GLuint texture, GLint& width)
	{
		GLint real = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &real);
		if (static_cast<GLuint>(real) != texture)
			return false;
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
		return true;
	}

	//A parked page is drawn: if its texture still has the size DEMASTER gave it and the field is the same, it is a page
	//again (with its PBR maps). The game's own uploads go through glTexImage2D, which forgets a parked page, so a texture
	//still parked here has had nothing new from the game.
	bool TakeBack(const GLuint texture)
	{
		const auto parked = parkedPages.find(texture);
		if (parked == parkedPages.end())
			return false;
		GLint width = 0;
		if (!DriverWidth(texture, width))
			return false; //asked again at its next draw
		FieldPage page = std::move(parked->second);
		parkedPages.erase(parked);
		bool sameField = false;
		{
			std::lock_guard<std::mutex> lock(fieldMutex);
			sameField = field.active && field.name == page.job.name && field.entry == fieldPagesEntry;
			if (sameField)
			{
				page.job.entry = field.entry;
				page.job.pbr = field.pbr;
			}
		}
		if (!sameField || width != page.width)
			return false;
		page.checked = GetTickCount64();
		FieldPage& back = fieldPages[texture];
		back = std::move(page);
		PbrPageBuilt(texture, back.job, SizeOf(back));
		glBindTexture(GL_TEXTURE_2D, texture); //PbrPageBuilt binds its own
		if (FIELD_DEBUG)
			Log("FIELD_LAYERS: %s page %u .map palette %d (texture %u): taken back onto DEMASTER's list (the game drew it "
				"without giving it a new picture)\n", back.job.name.c_str(), back.job.tPage - 16, back.job.palette, texture);
		return true;
	}
}

//glDrawElements hook, before the draw. FIELD_SMOOTH: smooth filtering inside each tile's cell while a page from layers is
//drawn. FIELD_PBR: a page with PBR layers is drawn with DEMASTER's field program (its maps, the field's lights). Every draw
//of the game goes through glDrawElements, so the uniforms are always back to zero before anything else is drawn. Also where
//changed files are picked up (FIELD_LAYERS_LIVE). count, type, indices: the draw's.
void FieldLayersBeforeDraw(const int count, const unsigned int type, const void* indices)
{
	if (!FIELD_LAYERS || disabled)
		return;
	struct Time //FIELD_FRAME_LOG's timing
	{
		long long started = FieldTimeNow();
		~Time() { FieldTimeAdd(TIME_LAYERS_HOOK, started); }
	} timing;
	if (const uint64_t entry = latestEntry; fieldPagesEntry != entry && !pendingSet)
	{
		//another field (its textures get new uploads, which forget the parked pages), or this field's .map read again
		ParkAll();
		PbrForgetAll();
		fieldPagesEntry = entry;
	}
	try
	{
		PollLayerFiles();
		PbrPoll();
	}
	catch (...)
	{
		LogOnce("live-failed", "FIELD_LAYERS: reading changed layer files failed, FIELD_LAYERS_LIVE stops for this session\n");
		FIELD_LAYERS_LIVE = 0;
	}
	if (PbrShaderMissing())
		UsePaintings();
	struct ProgramState
	{
		GLint location = -2; //-2: not looked up yet
		bool on = false;
		float value[4] = { 0, 0, 0, 0 };
	};
	static std::unordered_map<GLint, ProgramState> programs;
	static bool anyOn = false;
	if (fieldPages.empty() && !anyOn && !PbrActive())
		return;
	const GLint program = glstate::Program();
	if (program == 0)
		return;
	const GLint bound = static_cast<GLint>(glstate::BoundTexture2D());
	auto it = fieldPages.find(static_cast<GLuint>(bound));
	bool ours = it != fieldPages.end();
	//only main.frag draws pages; another program with a page bound (the round shadows) has nothing to do with it
	const bool mainProgram = GameProgramOf(program).kind == ProgramKind::Main;
	if (!ours && mainProgram && !parkedPages.empty())
	{
		ours = TakeBack(static_cast<GLuint>(bound));
		it = fieldPages.find(static_cast<GLuint>(bound));
	}
	//the game giving the texture new content (glTexImage2D) forgets the page already; this catches other ways, twice a
	//second (asking the driver about a texture waits for it)
	const ULONGLONG now = ours ? GetTickCount64() : 0;
	if (ours && now - it->second.checked >= 500)
	{
		it->second.checked = now;
		GLint width = 0;
		if (!DriverWidth(static_cast<GLuint>(bound), width))
			LogOnce("page-unchecked-" + std::to_string(fieldPagesEntry), "FIELD_LAYERS: a page's texture (%d) is not the one the "
				"driver has bound for this draw: its size is not checked now, and the page stays on DEMASTER's list\n", bound);
		else if (width != it->second.width)
		{
			LogDrop(it->first, it->second, "its texture has another size now: the game uses it for something else");
			PbrForget(it->first);
			fieldPages.erase(it); //the game uses that texture for something else now
			ours = false;
		}
	}
	float clamp[4] = { 0, 0, 0, 0 };
	if (ours && FIELD_SMOOTH)
	{
		const FieldPage& page = it->second;
		clamp[0] = static_cast<float>(TILE_SIZE) / page.logicalWidth;
		clamp[1] = static_cast<float>(TILE_SIZE) / page.logicalHeight;
		clamp[2] = 1.0f / page.width;
		clamp[3] = 1.0f / page.height;
	}
	if (ours && mainProgram && PbrBeforeDraw(static_cast<GLuint>(bound), program, clamp, count, type, indices))
		return; //drawn with DEMASTER's field program (PbrAfterDraw puts the game's back)
	if (!FIELD_SMOOTH)
		return;
	ProgramState& state = programs[program];
	if (state.location == -2)
		state.location = glGetUniformLocation(static_cast<GLuint>(program), "demasterTileClamp");
	if (ours && mainProgram && state.location < 0)
		LogOnce("noclamp-" + std::to_string(program), "FIELD_LAYERS: field pages are drawn with shader program %d, an old main.frag "
			"without demasterTileClamp: filtering left as it is. Copy DEMASTER_EXP\\shaders\\main.frag from this release.\n", program);
	if (state.location < 0)
		return;
	if (ours && mainProgram)
	{
		if (!state.on || memcmp(state.value, clamp, sizeof(clamp)) != 0)
		{
			glUniform4f(state.location, clamp[0], clamp[1], clamp[2], clamp[3]);
			memcpy(state.value, clamp, sizeof(clamp));
		}
		state.on = true;
		anyOn = true;
	}
	else if (state.on)
	{
		glUniform4f(state.location, 0.0f, 0.0f, 0.0f, 0.0f);
		state.on = false;
		memset(state.value, 0, sizeof(state.value));
	}
}

//glDrawElements hook, after the draw: the texture units the PBR maps used get back what the game had there
void FieldLayersAfterDraw()
{
	if (FIELD_LAYERS && !disabled)
		PbrAfterDraw();
}

//Used by the swizzled field module (FIELD_BACKGROUND): a field with layer images is not loaded from its swizzled files.
//fieldBgName: "field_bg\\xx\\name\\name_"
bool FieldLayersHasFiles(const char* fieldBgName)
{
	if (!FIELD_LAYERS || fieldBgName == nullptr)
		return false;
	std::string relative = fieldBgName;
	const size_t slash = relative.find_last_of('\\');
	if (slash == std::string::npos)
		return false;
	const std::string folder = std::string(DIRECT_IO_EXPORT_DIR) + "textures\\" + relative.substr(0, slash + 1);
	std::string name = relative.substr(slash + 1);
	if (!name.empty() && name.back() == '_')
		name.pop_back();
	std::error_code error;
	if (name.empty() || !std::filesystem::is_directory(folder, error))
		return false;
	const std::regex pattern(name + "(_[a-z]{2})?_[0-9A-F]{8}\\.(png|dds)", std::regex::icase);
	for (const auto& entry : std::filesystem::directory_iterator(folder, error))
		if (std::regex_match(entry.path().filename().string(), pattern))
			return true;
	return false;
}

//--- FIELD_BG_RESOLUTION: the buffer field backgrounds are drawn into (4, 6 or 8 times the PlayStation's 320x240)
//
//At BGRESPATCH1 (the "shr eax, 1" DEMASTER turns into "shl" for its 4x):
//	-0xC    mov eax, [640]           (the game's logical width)
//	-0x1    shr eax, 1               -> width / 2: 320
//	+0x1    mov [buffer width], eax
//	+0x6    mov eax, [480]
//	+0xB    shr eax, 1
//	+0x12   mov [buffer height], eax
//For 6x and 8x the two "mov eax, [...]" become calls into DEMASTER and the shifts become NOPs.

namespace
{
	DWORD bgLogicalWidth = 0, bgLogicalHeight = 0; //addresses
}

static DWORD __cdecl FieldBgSize(const int height)
{
	const DWORD logicalWidth = bgLogicalWidth ? *reinterpret_cast<const DWORD*>(bgLogicalWidth) : 640;
	const DWORD logicalHeight = bgLogicalHeight ? *reinterpret_cast<const DWORD*>(bgLogicalHeight) : 480;
	const DWORD nativeWidth = std::max<DWORD>(1, logicalWidth / 2), nativeHeight = std::max<DWORD>(1, logicalHeight / 2);
	const DWORD scale = FIELD_BG_RESOLUTION == 6 || FIELD_BG_RESOLUTION == 8 ? static_cast<DWORD>(FIELD_BG_RESOLUTION) : 4;
	return (height ? nativeHeight : nativeWidth) * scale;
}

__declspec(naked) static void FieldBgWidthThunk()
{
	__asm
	{
		PUSH ECX
		PUSH EDX
		PUSH 0
		CALL FieldBgSize
		ADD ESP, 4
		POP EDX
		POP ECX
		RET
	}
}

__declspec(naked) static void FieldBgHeightThunk()
{
	__asm
	{
		PUSH ECX
		PUSH EDX
		PUSH 1
		CALL FieldBgSize
		ADD ESP, 4
		POP EDX
		POP ECX
		RET
	}
}

//false: FIELD_BG_RESOLUTION is 4 (or anything but 6 and 8), or the code is not as expected: the caller keeps DEMASTER's 4x
bool ApplyFieldBgResolution()
{
	if (FIELD_BG_RESOLUTION != 6 && FIELD_BG_RESOLUTION != 8)
	{
		if (FIELD_BG_RESOLUTION != 4 && FIELD_BG_RESOLUTION != 0)
			OutputDebug("FIELD_BG_RESOLUTION=%d: only 4, 6 and 8 are supported, field backgrounds drawn at 4x\n", FIELD_BG_RESOLUTION);
		return false;
	}
	if (!HasAddress(BGRESPATCH1))
		return false;
	const DWORD at = IMAGE_BASE + GetAddress(BGRESPATCH1);
	const BYTE* p = reinterpret_cast<const BYTE*>(at);
	const bool expected = p[-0xC] == 0xA1 && p[-1] == 0xD1 && p[0] == 0xE8 && p[1] == 0xA3 && p[6] == 0xA1 && p[0xB] == 0xD1
		&& p[0xC] == 0xE8 && p[0x12] == 0xA3;
	if (!expected)
	{
		OutputDebug("FIELD_BG_RESOLUTION: unexpected code at %08X, field backgrounds stay at DEMASTER's 4x\n", at);
		return false;
	}
	bgLogicalWidth = *reinterpret_cast<const DWORD*>(at - 0xB);
	bgLogicalHeight = *reinterpret_cast<const DWORD*>(at + 0x7);
	const auto writeCall = [](const DWORD site, const void* function)
	{
		ModPage(site, 5);
		*reinterpret_cast<BYTE*>(site) = 0xE8;
		*reinterpret_cast<DWORD*>(site + 1) = reinterpret_cast<DWORD>(function) - (site + 5);
	};
	writeCall(at - 0xC, reinterpret_cast<const void*>(&FieldBgWidthThunk));
	writeCall(at + 0x6, reinterpret_cast<const void*>(&FieldBgHeightThunk));
	ModPage(at - 1, 2);
	*reinterpret_cast<BYTE*>(at - 1) = 0x90;
	*reinterpret_cast<BYTE*>(at) = 0x90;
	ModPage(at + 0xB, 2);
	*reinterpret_cast<BYTE*>(at + 0xB) = 0x90;
	*reinterpret_cast<BYTE*>(at + 0xC) = 0x90;
	OutputDebug("FIELD_BG_RESOLUTION=%d: field backgrounds drawn at %dx the PlayStation size (%dx%d)\n", FIELD_BG_RESOLUTION,
		FIELD_BG_RESOLUTION, 320 * FIELD_BG_RESOLUTION, 240 * FIELD_BG_RESOLUTION);
	return true;
}

void ApplyFieldLayersPatch()
{
	if (!FIELD_LAYERS)
		return;
	if (!BATTLE_HOOK)
	{
		OutputDebug("FIELD_LAYERS needs BATTLE_HOOK=1 (the page upload hook it shares with the world and battle modules): off\n");
		return;
	}
	static const ADDRESS_NAME needed[] = { GUEST_STACK_BASE, GUEST_PAGE_TABLE, FIELD_READ_FILE };
	for (const ADDRESS_NAME name : needed)
		if (!HasAddress(name))
		{
			OutputDebug("FIELD_LAYERS: %s is not known for this game DLL, layers off\n", GetAddressName(name));
			return;
		}
	guestStackBaseAddress = IMAGE_BASE + GetAddress(GUEST_STACK_BASE);
	guestPageTableAddress = IMAGE_BASE + GetAddress(GUEST_PAGE_TABLE);
	psxVram = HasAddress(PSX_VRAM) ? GetAddress(PSX_VRAM) : 0; //a guest address, not in the DLL
	if (psxVram == 0)
		OutputDebug("FIELD_LAYERS: PSX_VRAM not known for this game DLL: palettes are matched against the .mim only\n");
	//the recompiled sm_pc_read (what tools/verify_addresses.py checked)
	const DWORD readFile = IMAGE_BASE + GetAddress(FIELD_READ_FILE);
	static const BYTE readFileStart[] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08, 0x56, 0x81, 0x46, 0x2C, 0xFC, 0xFE, 0xFF, 0xFF };
	if (memcmp(reinterpret_cast<const void*>(readFile), readFileStart, sizeof(readFileStart)) != 0)
	{
		OutputDebug("FIELD_LAYERS: unexpected code at %08X (FIELD_READ_FILE), layers off\n", readFile);
		return;
	}
	if (MH_CreateHook(reinterpret_cast<LPVOID>(readFile), reinterpret_cast<LPVOID>(&ReadFileDetour),
		reinterpret_cast<LPVOID*>(&readFileOriginal)) != MH_OK)
	{
		OutputDebug("FIELD_LAYERS: could not hook the game's file reads, layers off\n");
		return;
	}
	hooksInstalled = true;

	//which CLUT goes into which palette slot (optional: without it, palettes are told apart by their pixels only)
	static const BYTE readVramPaletteStart[] = { 0x55, 0x8B, 0xEC, 0xA1, 0, 0, 0, 0, 0x83, 0xEC, 0x08, 0x56, 0x8B, 0x75, 0x08 };
	static const BYTE writePaletteStart[] = { 0x55, 0x8B, 0xEC, 0xA1, 0, 0, 0, 0, 0x56, 0x8B, 0x75, 0x08, 0x56, 0x8B, 0x56, 0x2C };
	const auto expected = [](const DWORD at, const BYTE* start, const size_t length)
	{
		//bytes 4-7: the address of the guest stack base (relocated with the DLL)
		const DWORD stackBase = IMAGE_BASE + GetAddress(GUEST_STACK_BASE);
		return memcmp(reinterpret_cast<const void*>(at), start, 4) == 0 && *reinterpret_cast<const DWORD*>(at + 4) == stackBase
			&& memcmp(reinterpret_cast<const void*>(at + 8), start + 8, length - 8) == 0;
	};
	if (HasAddress(READ_VRAM_PALETTE) && HasAddress(WRITE_PALETTE))
	{
		const DWORD readVramPalette = IMAGE_BASE + GetAddress(READ_VRAM_PALETTE), writePalette = IMAGE_BASE + GetAddress(WRITE_PALETTE);
		if (!expected(readVramPalette, readVramPaletteStart, sizeof(readVramPaletteStart)) || !expected(writePalette, writePaletteStart, sizeof(writePaletteStart)))
			OutputDebug("FIELD_LAYERS: unexpected code at %08X / %08X (READ_VRAM_PALETTE / WRITE_PALETTE): palettes told apart by their pixels only\n",
				readVramPalette, writePalette);
		else if (MH_CreateHook(reinterpret_cast<LPVOID>(readVramPalette), reinterpret_cast<LPVOID>(&ReadVramPaletteDetour),
			reinterpret_cast<LPVOID*>(&readVramPaletteOriginal)) != MH_OK
			|| MH_CreateHook(reinterpret_cast<LPVOID>(writePalette), reinterpret_cast<LPVOID>(&WritePaletteDetour),
				reinterpret_cast<LPVOID*>(&writePaletteOriginal)) != MH_OK)
			OutputDebug("FIELD_LAYERS: could not hook the game's palette writes: palettes told apart by their pixels only\n");
		else
			paletteHooks = true;
	}
	OutputDebug("FIELD_LAYERS: on (DEMASTER_EXP\\textures\\field_bg\\<xx>\\<field>\\<field>_<pupu>.png|dds)%s%s%s\n",
		FIELD_LAYERS_UNSHARE ? ", tiles get cells of their own" : "", FIELD_SMOOTH ? ", smooth filtering" : "",
		FIELD_LAYERS_LIVE ? ", changed files picked up while playing" : "");
}
