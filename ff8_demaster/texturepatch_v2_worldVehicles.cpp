//World map textures by name (WORLD_VEHICLES). See world_vehicles.h for the idea; this file is the game side.
//
//Remastered runs the 2000 PC game code as a static recompilation: each original function is a host function that takes
//the emulated CPU state. Two places are hooked:
//	GUEST_LOADIMAGE       the PlayStation LoadImage(RECT*, data) every copy of texels into VRAM goes through. When the
//	                      data is a texture of wmset (kept by the world map at WMSET_GUEST) or chara.one, DEMASTER
//	                      records "this texture went there". Colour tables (1 row high) are not recorded.
//	WM_CHARA_READ_CALL    the world map reads world\esk\chara.one into memory (to know where that buffer is)
//Neither changes anything the game does.
//
//When the renderer later uploads a world page (WtpCheck -> WorldVehiclesBuildPage), the recorded textures on that page
//are checked against the page pixels (palette-independent, so every palette texture of the page gets them). The HD page
//is then put together on the GPU (WtpGl -> WorldVehiclesUploadPending): the game's page or the page's wmset_XXX file
//stretched to the HD size, every named texture stamped on top from its HD file, resized once to its exact spot and
//kept as a texture. The game re-sends pages constantly (water colour animation), so per upload this costs a hash of
//the page and a few framebuffer blits, about what the game spends itself.

#include "config.h"
#include "coreHeader.h"
#include "opengl.h"
#include "texture.h"
#include "world_vehicles.h"
#include "minhook/include/MinHook.h"

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using namespace worldvehicles;

//Original target of the chara.one read call (used by the naked hook below)
DWORD wvCharaReadOriginal;

namespace
{
	//Emulated CPU state: EAX +0, ECX +4, EDX +8, EBX +0xC, EBP +0x28, ESP +0x2C, ESI +0x30, EDI +0x34
	constexpr size_t STATE_ESP = 0x2C;

	bool hooksInstalled = false;
	bool pagesDisabled = false; //set after an unexpected error: pages are left as the game makes them
	DWORD guestStackBaseAddress = 0; //host address of the DWORD holding the flat base the recompiled code uses for the stack
	DWORD guestPageTableAddress = 0; //host address of the page table: host = table[guest >> 12] + (guest & 0xFFF)
	DWORD wmsetGuest = 0; //guest address of wmsetXX.obj

	typedef DWORD(__cdecl* RecompiledFunction)(BYTE* state);
	RecompiledFunction loadImageOriginal = nullptr;

	VramTracker tracker;

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

	//guest stack (and anything the recompiled code reaches through the flat base)
	bool StackRead32(const DWORD guest, DWORD& value)
	{
		DWORD base = 0;
		if (!CopyHost(&base, reinterpret_cast<const void*>(guestStackBaseAddress), 4) || base == 0)
			return false;
		return CopyHost(&value, reinterpret_cast<const void*>(base + guest), 4);
	}

	//guest data (heap, files read by the game) through the page table, page by page
	bool GuestRead(void* destination, DWORD guest, size_t length)
	{
		BYTE* out = static_cast<BYTE*>(destination);
		const DWORD* table = reinterpret_cast<const DWORD*>(guestPageTableAddress);
		while (length > 0)
		{
			DWORD page = 0;
			if (!CopyHost(&page, &table[guest >> 12], 4) || page == 0)
				return false;
			const size_t inPage = 0x1000 - (guest & 0xFFF);
			const size_t chunk = length < inPage ? length : inPage;
			if (!CopyHost(out, reinterpret_cast<const void*>(page + (guest & 0xFFF)), chunk))
				return false;
			out += chunk;
			guest += static_cast<DWORD>(chunk);
			length -= chunk;
		}
		return true;
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
		char text[512];
		va_list args;
		va_start(args, format);
		vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		OutputDebug("%s", text);
	}

	//--- named textures in guest memory: wmset at a fixed address, chara.one wherever the game read it

	struct Source
	{
		DWORD guest = 0, size = 0; //buffer in guest memory
		std::vector<DWORD> header; //wmset: the section offsets the index was built from
		std::unordered_map<DWORD, std::shared_ptr<const NamedTexture>> byImage; //guest address of the first texel -> texture
		bool valid = false;
	};
	std::mutex sourceMutex;
	Source wmset, chara;
	DWORD charaBuffer = 0, charaSize = 0; //from the read hook
	uint64_t worldEntry = 1; //changes every time the world map loads chara.one (new visit: look for files again)

	void Index(Source& source, const std::vector<NamedTexture>& textures)
	{
		source.byImage.clear();
		for (const NamedTexture& t : textures)
			source.byImage[source.guest + static_cast<DWORD>(t.imageOffset)] = std::make_shared<const NamedTexture>(t);
		source.valid = true;
	}

	//wmset: (re)read when its header changed (another wmset loaded) or was never read. Only up to section 42 is needed.
	void RefreshWmset()
	{
		std::vector<DWORD> header(48);
		if (!GuestRead(header.data(), wmsetGuest, header.size() * 4))
			return;
		if (wmset.valid && header == wmset.header)
			return;
		if (header[0] < 48 * 4 || header[0] > 0x400)
			return;
		for (int i = 37; i <= 41; i++)
			if (header[i] < header[0] || header[i] > (8u << 20))
				return;
		const DWORD size = header[42] > header[41] && header[42] <= (8u << 20) ? header[42] : header[41] + 0x10000;
		std::vector<uint8_t> data(size);
		if (!GuestRead(data.data(), wmsetGuest, size))
			return;
		wmset.guest = wmsetGuest;
		wmset.size = size;
		wmset.header = header;
		const std::vector<NamedTexture> textures = ParseWmsetTextures(data.data(), data.size());
		Index(wmset, textures);
		if (WORLD_DEBUG)
			OutputDebug("WORLD_VEHICLES: wmset at %08X: %zu textures\n", wmsetGuest, textures.size());
	}

	void RefreshChara()
	{
		if (chara.valid && chara.guest == charaBuffer && chara.size == charaSize)
			return;
		chara = Source{};
		if (charaBuffer == 0 || charaSize < 8 || charaSize > (8u << 20))
			return;
		std::vector<uint8_t> data(charaSize);
		if (!GuestRead(data.data(), charaBuffer, charaSize))
		{
			OutputDebug("WORLD_VEHICLES: could not read chara.one from game memory\n");
			return;
		}
		chara.guest = charaBuffer;
		chara.size = charaSize;
		const std::vector<NamedTexture> textures = ParseCharaOne(data.data(), data.size());
		Index(chara, textures);
		if (WORLD_DEBUG)
			OutputDebug("WORLD_VEHICLES: chara.one has %zu textures\n", textures.size());
	}

	//Which named texture LoadImage is copying: by the address of its texels, or (chara.one, the game may copy the texels
	//somewhere first) by content
	std::shared_ptr<const NamedTexture> FindTexture(const DWORD data, const int widthUnits, const int height)
	{
		std::lock_guard<std::mutex> lock(sourceMutex);
		if (data >= wmsetGuest && data < wmsetGuest + (wmset.valid ? wmset.size : (8u << 20)))
		{
			if (!wmset.valid)
				RefreshWmset();
			auto it = wmset.byImage.find(data);
			if (it == wmset.byImage.end())
			{
				RefreshWmset(); //another wmset may have been loaded
				it = wmset.byImage.find(data);
			}
			if (it != wmset.byImage.end())
				return it->second;
		}
		RefreshChara();
		if (!chara.valid)
			return nullptr;
		if (const auto it = chara.byImage.find(data); it != chara.byImage.end())
			return it->second;
		std::vector<uint8_t> texels;
		for (const auto& [address, texture] : chara.byImage)
		{
			if (texture->tim.widthUnits != widthUnits || texture->tim.height != height)
				continue;
			if (texels.empty())
			{
				texels.resize(static_cast<size_t>(widthUnits) * 2 * height);
				if (!GuestRead(texels.data(), data, texels.size()))
					return nullptr;
			}
			if (texture->tim.image == texels)
				return texture;
		}
		return nullptr;
	}

	//the world map (or the mode is not known): other modes copy their own textures, not looked at
	bool InWorld()
	{
		return currentMode == MODE_WORLDMAP || currentMode == MODE_NONE;
	}

	void OnLoadImage(BYTE* state)
	{
		if (!InWorld())
			return;
		try
		{
			//LoadImage(RECT* rect, data): RECT = x, y, w, h (16-bit, VRAM units)
			const DWORD esp = StateRegister(state, STATE_ESP);
			DWORD rectGuest = 0, data = 0;
			int16_t rect[4]{};
			if (!StackRead32(esp + 4, rectGuest) || !StackRead32(esp + 8, data) || !GuestRead(rect, rectGuest, 8))
				return;
			const int x = rect[0], y = rect[1], w = rect[2], h = rect[3];
			if (w <= 0 || h <= 1 || x < 0 || y < 0 || x + w > 1024 || y + h > 512) //h == 1: colour tables
				return;
			const std::shared_ptr<const NamedTexture> texture = FindTexture(data, w, h);
			if (!texture || texture->tim.widthUnits != w || texture->tim.height != h)
				return;
			if (WORLD_DEBUG)
				LogOnce(texture->name + "@" + std::to_string(x) + "," + std::to_string(y),
					"WORLD_VEHICLES: %s -> VRAM (%d,%d) %dx%d %d-bit, %d palette(s)%s\n", texture->name.c_str(), x, y,
					texture->tim.Width(), texture->tim.height, texture->tim.bpp == 0 ? 4 : 8, texture->tim.PaletteCount(),
					texture->frames.empty() ? "" : ", colour animation");
			tracker.Record(texture, x, y);
		}
		catch (...)
		{
		}
	}

	DWORD __cdecl LoadImageDetour(BYTE* state)
	{
		OnLoadImage(state);
		return loadImageOriginal(state);
	}
}

void WvOnCharaRead(BYTE* state)
{
	const DWORD esp = StateRegister(state, STATE_ESP);
	DWORD buffer = 0, size = 0;
	if (!StackRead32(esp + 8, buffer) || !StackRead32(esp + 0xC, size))
		return;
	std::lock_guard<std::mutex> lock(sourceMutex);
	charaBuffer = buffer;
	charaSize = size;
	chara.valid = false;
	wmset.valid = false; //read it again on this visit (the game may have loaded another one)
	worldEntry++;
}

__declspec(naked) static void WvCharaReadHook()
{
	__asm
	{
		PUSHFD
		PUSHAD
		PUSH DWORD PTR[ESP + 0x28] //emulated CPU state, pushed by the caller
		CALL WvOnCharaRead
		ADD ESP, 4
		POPAD
		POPFD
		JMP wvCharaReadOriginal
	}
}

//--- page side (renderer thread): WtpCheck -> WorldVehiclesBuildPage, WtpGl -> WorldVehiclesUploadPending

namespace
{
	//HD files, decoded once (failures too, as empty images)
	std::map<std::string, RgbaImage> imageCache;
	size_t imageCacheBytes = 0;
	constexpr size_t IMAGE_CACHE_LIMIT = 256u << 20;

	const RgbaImage* LoadCached(const std::string& path)
	{
		if (const auto it = imageCache.find(path); it != imageCache.end())
			return it->second.IsValid() ? &it->second : nullptr;
		RgbaImage out;
		if (SafeBimg image = LoadImageFromFile(path.c_str()))
		{
			static bx::DefaultAllocator allocator;
			if (bimg::ImageContainer* rgba = bimg::imageConvert(&allocator, bimg::TextureFormat::RGBA8, *image, false))
			{
				out.width = static_cast<int>(rgba->m_width);
				out.height = static_cast<int>(rgba->m_height);
				const size_t bytes = static_cast<size_t>(out.width) * out.height * 4;
				if (rgba->m_size >= bytes)
					out.pixels.assign(static_cast<const uint8_t*>(rgba->m_data), static_cast<const uint8_t*>(rgba->m_data) + bytes);
				bimg::imageFree(rgba);
			}
			else
				OutputDebug("WORLD_VEHICLES: cannot convert %s to RGBA8\n", path.c_str());
		}
		if (imageCacheBytes > IMAGE_CACHE_LIMIT) //only between pages: nothing holds a pointer into the cache here
		{
			imageCache.clear();
			imageCacheBytes = 0;
		}
		imageCacheBytes += out.pixels.size();
		RgbaImage& stored = imageCache[path] = std::move(out);
		return stored.IsValid() ? &stored : nullptr;
	}

	//Which file (and which part of it) a matched texture uses. Looked up once per world map visit.
	struct FileRef
	{
		std::string path; //empty: no file
		bool slice = false; //chara.one strip: the texture's part of it
		int sliceX = 0, sliceWidth = 0, stripWidth = 0, stripHeight = 0, height = 0;
		bool atlas = false; //page atlas (256x256 texels): the texture's rectangle in it
		int atlasX = 0, atlasY = 0, width = 0;
	};
	std::unordered_map<std::string, FileRef> fileRefs;
	uint64_t fileRefsEntry = 0;

	std::string FindFile(const std::string& name)
	{
		char path[MAX_PATH]{ 0 };
		if (!name.empty() && !DDSorPNG(path, MAX_PATH, "%stextures\\%s", DIRECT_IO_EXPORT_DIR, name.c_str()))
			return path;
		return {};
	}

	uint64_t CurrentWorldEntry()
	{
		std::lock_guard<std::mutex> lock(sourceMutex);
		return worldEntry;
	}

	//<name>_f<frame>, <name>_p<palette>, <name>, then the same for its twin, then the same for its page atlas;
	//chara.one: the model's strip
	FileRef ResolveFile(const NamedTexture& texture, const int palette, const int frame)
	{
		if (const uint64_t entry = CurrentWorldEntry(); fileRefsEntry != entry)
		{
			fileRefs.clear(); //new world map visit: files may have been added or removed
			fileRefsEntry = entry;
		}
		const std::string key = texture.name + "|" + std::to_string(palette) + "|" + std::to_string(frame);
		if (const auto it = fileRefs.find(key); it != fileRefs.end())
			return it->second;
		FileRef ref;
		if (!texture.strip.empty())
		{
			for (const std::string& name : { texture.strip, TwinName(texture.strip) })
				if (ref.path.empty())
					ref.path = FindFile(name);
			ref.slice = true;
			ref.sliceX = texture.sliceX;
			ref.sliceWidth = texture.tim.Width();
			ref.stripWidth = texture.stripWidth;
			ref.stripHeight = texture.stripHeight;
			ref.height = texture.tim.height;
		}
		else
		{
			for (const std::string& name : { texture.name, TwinName(texture.name), texture.atlas })
			{
				if (name.empty() || !ref.path.empty())
					continue;
				if (frame >= 0)
					ref.path = FindFile(name + "_f" + std::to_string(frame));
				if (ref.path.empty() && palette >= 0)
					ref.path = FindFile(name + "_p" + std::to_string(palette));
				if (ref.path.empty())
					ref.path = FindFile(name);
				if (!ref.path.empty() && name == texture.atlas)
				{
					ref.atlas = true;
					ref.atlasX = texture.atlasX;
					ref.atlasY = texture.atlasY;
					ref.width = texture.tim.Width();
					ref.height = texture.tim.height;
				}
			}
		}
		fileRefs[key] = ref;
		return ref;
	}

	//Part of the file a texture uses, in file pixels
	struct Region
	{
		int x = 0, y = 0, w = 0, h = 0;
	};
	Region RegionOf(const FileRef& ref, const RgbaImage& image)
	{
		if (ref.atlas)
		{
			//the atlas is a whole page, 256x256 texels at any scale
			const double sx = image.width / 256.0, sy = image.height / 256.0;
			const int x0 = static_cast<int>(std::lround(ref.atlasX * sx)), x1 = static_cast<int>(std::lround((ref.atlasX + ref.width) * sx));
			const int y0 = static_cast<int>(std::lround(ref.atlasY * sy)), y1 = static_cast<int>(std::lround((ref.atlasY + ref.height) * sy));
			return { x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0) };
		}
		if (!ref.slice || ref.stripWidth <= 0 || ref.stripHeight <= 0)
			return { 0, 0, image.width, image.height };
		const double sx = static_cast<double>(image.width) / ref.stripWidth, sy = static_cast<double>(image.height) / ref.stripHeight;
		const int x0 = static_cast<int>(std::lround(ref.sliceX * sx)), x1 = static_cast<int>(std::lround((ref.sliceX + ref.sliceWidth) * sx));
		return { x0, 0, std::max(1, x1 - x0), std::max(1, static_cast<int>(std::lround(ref.height * sy))) };
	}

	//One page to put together: what to draw, in order. Built in WtpCheck (page pixels valid), drawn in WtpGl.
	struct Stamp
	{
		std::string path;
		Region region; //part of the file
		int texelX = 0, texelY = 0, texelWidth = 0, texelHeight = 0;
		bool restore = false; //no file: put the layer below back where an older stamp covers this texture
	};
	struct PagePlan
	{
		int scale = 1;
		std::string basePath; //wmset_XXX file under everything; empty: the game's page
		std::vector<Stamp> stamps;
	};
	std::unordered_map<uint64_t, PagePlan> plans; //key: page content, page, tracker state, base file, world visit
	constexpr size_t PLAN_LIMIT = 512;

	//the pending page (WtpCheck -> WtpGl)
	bool pending = false;
	PagePlan pendingPlan;
	std::vector<uint8_t> pendingPixels; //game page (BGRA) when there is no base file
	int pendingWidth = 0, pendingHeight = 0;

	PagePlan MakePlan(const DWORD tPage, const uint8_t* pixels, const int width, const int height,
		const std::vector<VramTexture>& textures, const char* basePath)
	{
		PagePlan plan;
		const int pageX = static_cast<int>(tPage % 16) * 64, pageY = static_cast<int>(tPage / 16) * 256;
		const MatchReport report = MatchPage(pixels, width, height, pageX, pageY, textures);
		if (WORLD_DEBUG)
			for (const PageMatch& miss : report.nearMisses)
				LogOnce("miss-" + std::to_string(tPage) + "-" + textures[miss.textureIndex].Name(),
					"WORLD_VEHICLES: page %u: %s only %.0f percent there (not used)\n", tPage,
					textures[miss.textureIndex].Name().c_str(), miss.agreement * 100.0);
		int scale = 1;
		bool eightBit = false, any = false;
		std::vector<Stamp> stamps;
		for (const PageMatch& match : report.matches)
		{
			const NamedTexture& texture = *textures[match.textureIndex].texture;
			const FileRef ref = ResolveFile(texture, match.palette, match.frame);
			if (WORLD_DEBUG)
			{
				const std::string variant = match.frame >= 0 ? " frame " + std::to_string(match.frame)
					: match.palette >= 0 ? " palette " + std::to_string(match.palette) : std::string();
				LogOnce("match-" + std::to_string(tPage) + "-" + texture.name + variant + "-" + ref.path,
					"WORLD_VEHICLES: page %u: %s at %d,%d%s -> %s\n", tPage, texture.name.c_str(), match.texelX, match.texelY,
					variant.c_str(), ref.path.empty() ? "(no file)" : ref.path.c_str());
			}
			Stamp stamp;
			stamp.texelX = match.texelX;
			stamp.texelY = match.texelY;
			stamp.texelWidth = match.texelWidth;
			stamp.texelHeight = match.texelHeight;
			const RgbaImage* image = ref.path.empty() ? nullptr : LoadCached(ref.path);
			if (image == nullptr)
			{
				//no file: matters only where an older stamp covers it (put the layer below back there)
				for (const Stamp& older : stamps)
					if (!older.restore && older.texelX < stamp.texelX + stamp.texelWidth && stamp.texelX < older.texelX + older.texelWidth
						&& older.texelY < stamp.texelY + stamp.texelHeight && stamp.texelY < older.texelY + older.texelHeight)
					{
						stamp.restore = true;
						stamps.push_back(stamp);
						break;
					}
				continue;
			}
			stamp.path = ref.path;
			stamp.region = RegionOf(ref, *image);
			scale = std::max(scale, static_cast<int>(std::lround(static_cast<double>(stamp.region.w) / match.texelWidth)));
			eightBit |= texture.tim.bpp == 1;
			any = true;
			stamps.push_back(stamp);
		}
		if (!any)
			return plan;
		//Maki's page file (wmset_XXX) under everything, if this page has one. Those are 4-bit pages, so not under an
		//8-bit texture such as Balamb Garden.
		if (basePath != nullptr && basePath[0] != '\0' && !eightBit)
			if (const RgbaImage* base = LoadCached(basePath))
			{
				plan.basePath = basePath;
				scale = std::max(scale, static_cast<int>(std::lround(base->width / 256.0)));
			}
		plan.scale = std::clamp(scale, 1, 8);
		plan.stamps = std::move(stamps);
		return plan;
	}

	//--- GPU

	struct GpuTexture
	{
		GLuint id = 0;
		int width = 0, height = 0;
	};
	std::unordered_map<std::string, GpuTexture> gpuTextures; //stamps at their exact size, base files at theirs
	size_t gpuBytes = 0;
	constexpr size_t GPU_LIMIT = 768u << 20;
	GLuint readFbo = 0, drawFbo = 0;
	GLuint pageTexture = 0; //the game's page of the current upload
	bool gpuFailed = false;

	void RawTexImage2D(const GLsizei width, const GLsizei height, const GLenum format, const void* data)
	{
		//straight to the driver: DEMASTER's own glTexImage2D hook would hash (and maybe replace) these
		if (ogl_tex_image2d != nullptr)
			reinterpret_cast<void* (__stdcall*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)>(ogl_tex_image2d)
			(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, format, GL_UNSIGNED_BYTE, data);
		else
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, format, GL_UNSIGNED_BYTE, data);
	}

	GLuint NewTexture()
	{
		GLuint id = 0;
		glGenTextures(1, &id);
		glBindTexture(GL_TEXTURE_2D, id);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		return id;
	}

	void TrimGpu()
	{
		if (gpuBytes <= GPU_LIMIT)
			return;
		for (auto& [key, texture] : gpuTextures)
			glDeleteTextures(1, &texture.id);
		gpuTextures.clear();
		gpuBytes = 0;
	}

	const GpuTexture* GpuImage(const std::string& key, const RgbaImage& image)
	{
		if (const auto it = gpuTextures.find(key); it != gpuTextures.end())
			return &it->second;
		if (!image.IsValid())
			return nullptr;
		const GpuTexture texture{ NewTexture(), image.width, image.height };
		RawTexImage2D(image.width, image.height, GL_RGBA, image.pixels.data());
		gpuBytes += image.pixels.size();
		return &(gpuTextures[key] = texture);
	}

	//a stamp: its part of the file resized once (on the CPU, alpha-weighted) to exactly its size on the page
	const GpuTexture* GpuStamp(const Stamp& stamp, const int scale)
	{
		const int w = stamp.texelWidth * scale, h = stamp.texelHeight * scale;
		const std::string key = stamp.path + "|" + std::to_string(stamp.region.x) + "," + std::to_string(stamp.region.y) + ","
			+ std::to_string(stamp.region.w) + "," + std::to_string(stamp.region.h) + "|" + std::to_string(w) + "x" + std::to_string(h);
		if (const auto it = gpuTextures.find(key); it != gpuTextures.end())
			return &it->second;
		const RgbaImage* image = LoadCached(stamp.path);
		if (image == nullptr)
			return nullptr;
		RgbaImage sized;
		sized.width = w;
		sized.height = h;
		sized.pixels.assign(static_cast<size_t>(w) * h * 4, 0);
		const bool whole = stamp.region.x == 0 && stamp.region.y == 0 && stamp.region.w == image->width && stamp.region.h == image->height;
		if (whole && image->width == w && image->height == h)
			sized.pixels = image->pixels;
		else if (whole)
			ResampleInto(*image, sized, 0, 0, w, h);
		else
			ResampleInto(Crop(*image, stamp.region.x, stamp.region.y, stamp.region.w, stamp.region.h), sized, 0, 0, w, h);
		return GpuImage(key, sized);
	}

	void Blit(const GLuint source, const int sx0, const int sy0, const int sx1, const int sy1,
		const int dx0, const int dy0, const int dx1, const int dy1)
	{
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, source, 0);
		const int sw = std::max(1, sx1 - sx0), sh = std::max(1, sy1 - sy0);
		const bool integerScale = (dx1 - dx0) % sw == 0 && (dy1 - dy0) % sh == 0;
		glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, GL_COLOR_BUFFER_BIT, integerScale ? GL_NEAREST : GL_LINEAR);
	}

	//Puts the pending page together in the texture the game has bound. false: could not (the caller falls back).
	bool DrawPendingOnGpu(const GLuint target)
	{
		CheckGlew();
		if (gpuFailed || target == 0 || glBlitFramebuffer == nullptr || glGenFramebuffers == nullptr)
			return false;
		const PagePlan& plan = pendingPlan;
		const int pageScale = std::max(1, pendingWidth / 256);
		const int outWidth = 256 * plan.scale, outHeight = pendingHeight / pageScale * plan.scale;

		//state to put back
		GLint readBinding = 0, drawBinding = 0, rowLength = 0, skipRows = 0, skipPixels = 0, alignment = 4, unpackBuffer = 0;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readBinding);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawBinding);
		glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
		glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
		glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPixels);
		glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
		glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpackBuffer);
		const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST), srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB),
			discard = glIsEnabled(GL_RASTERIZER_DISCARD);
		for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++) {} //so the check below sees only our own errors
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		if (unpackBuffer != 0)
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		if (scissor) glDisable(GL_SCISSOR_TEST);
		if (srgb) glDisable(GL_FRAMEBUFFER_SRGB);
		if (discard) glDisable(GL_RASTERIZER_DISCARD);

		TrimGpu();
		//layer under the stamps: the page's wmset_XXX file, or the game's own page
		GLuint layer = 0;
		int layerWidth = 0, layerHeight = 0;
		if (!plan.basePath.empty())
			if (const RgbaImage* base = LoadCached(plan.basePath))
				if (const GpuTexture* gpu = GpuImage(plan.basePath, *base))
				{
					layer = gpu->id;
					layerWidth = gpu->width;
					layerHeight = gpu->height;
				}
		if (layer == 0 && !pendingPixels.empty())
		{
			if (pageTexture == 0)
				pageTexture = NewTexture();
			glBindTexture(GL_TEXTURE_2D, pageTexture);
			RawTexImage2D(pendingWidth, pendingHeight, GL_BGRA, pendingPixels.data());
			layer = pageTexture;
			layerWidth = pendingWidth;
			layerHeight = pendingHeight;
		}
		std::vector<std::pair<const GpuTexture*, const Stamp*>> stamps;
		for (const Stamp& stamp : plan.stamps)
			stamps.push_back({ stamp.restore ? nullptr : GpuStamp(stamp, plan.scale), &stamp });
		bool ok = layer != 0;

		//the page itself: the HD size in the game's texture, then draw into it
		glBindTexture(GL_TEXTURE_2D, target);
		if (ok)
		{
			RawTexImage2D(outWidth, outHeight, GL_RGBA, nullptr);
			if (readFbo == 0)
				glGenFramebuffers(1, &readFbo);
			if (drawFbo == 0)
				glGenFramebuffers(1, &drawFbo);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
			ok = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		}
		if (ok)
		{
			Blit(layer, 0, 0, layerWidth, layerHeight, 0, 0, outWidth, outHeight);
			const double lx = layerWidth / 256.0, ly = layerHeight * static_cast<double>(plan.scale) / outHeight;
			for (const auto& [gpu, stamp] : stamps)
			{
				const int dx = stamp->texelX * plan.scale, dy = stamp->texelY * plan.scale;
				const int dw = stamp->texelWidth * plan.scale, dh = stamp->texelHeight * plan.scale;
				if (stamp->restore)
					Blit(layer, static_cast<int>(std::lround(stamp->texelX * lx)), static_cast<int>(std::lround(stamp->texelY * ly)),
						static_cast<int>(std::lround((stamp->texelX + stamp->texelWidth) * lx)),
						static_cast<int>(std::lround((stamp->texelY + stamp->texelHeight) * ly)), dx, dy, dx + dw, dy + dh);
				else if (gpu != nullptr)
					Blit(gpu->id, 0, 0, gpu->width, gpu->height, dx, dy, dx + dw, dy + dh);
			}
			ok = glGetError() == GL_NO_ERROR;
		}
		//detach, so nothing here keeps the game's texture alive
		if (readFbo != 0 && drawFbo != 0)
		{
			glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
			glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		}
		glBindFramebuffer(GL_READ_FRAMEBUFFER, readBinding);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawBinding);
		glBindTexture(GL_TEXTURE_2D, target);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
		glPixelStorei(GL_UNPACK_SKIP_ROWS, skipRows);
		glPixelStorei(GL_UNPACK_SKIP_PIXELS, skipPixels);
		glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
		if (unpackBuffer != 0)
			glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpackBuffer);
		if (scissor) glEnable(GL_SCISSOR_TEST);
		if (srgb) glEnable(GL_FRAMEBUFFER_SRGB);
		if (discard) glEnable(GL_RASTERIZER_DISCARD);
		if (!ok)
		{
			gpuFailed = true;
			OutputDebug("WORLD_VEHICLES: drawing pages on the GPU failed, using the slower CPU path from now on\n");
		}
		return ok;
	}

	//Same result on the CPU (fallback)
	void DrawPendingOnCpu()
	{
		const PagePlan& plan = pendingPlan;
		std::vector<RgbaImage> parts;
		parts.reserve(plan.stamps.size());
		std::vector<Placement> placements;
		for (const Stamp& stamp : plan.stamps)
		{
			const RgbaImage* image = stamp.restore ? nullptr : LoadCached(stamp.path);
			if (image == nullptr)
				continue;
			parts.push_back(Crop(*image, stamp.region.x, stamp.region.y, stamp.region.w, stamp.region.h));
			placements.push_back({ &parts.back(), stamp.texelX, stamp.texelY, stamp.texelWidth, stamp.texelHeight });
		}
		const RgbaImage* base = plan.basePath.empty() ? nullptr : LoadCached(plan.basePath);
		const RgbaImage page = ComposePage(pendingPixels.empty() ? nullptr : pendingPixels.data(), pendingWidth, pendingHeight,
			plan.scale, base, placements);
		if (page.IsValid())
			RawTexImage2D(page.width, page.height, GL_RGBA, page.pixels.data());
	}
}

namespace
{
	//Maki's whole-page file for this page (wmset_XXX_P for type 18, wmset0_XXX_P for type 0), looked up once per visit
	std::unordered_map<uint32_t, std::string> basePaths;
	uint64_t basePathsEntry = 0;

	const char* BasePath(const int textureType, const int textureIndex, const DWORD tPage, const int palette)
	{
		if (textureIndex <= 0 || static_cast<DWORD>(textureIndex) != tPage)
			return nullptr;
		if (const uint64_t entry = CurrentWorldEntry(); basePathsEntry != entry)
		{
			basePaths.clear();
			basePathsEntry = entry;
		}
		const uint32_t key = static_cast<uint32_t>(textureType & 0xFF) << 24 | static_cast<uint32_t>(textureIndex & 0xFFF) << 12
			| static_cast<uint32_t>(palette & 0xFFF);
		auto it = basePaths.find(key);
		if (it == basePaths.end())
		{
			char path[256]{ 0 };
			const char* prefix = textureType == 18 ? "wmset" : "wmset0";
			if (DDSorPNG(path, 256, "%stextures\\world\\dat\\wmset\\%s_%03d_%d", DIRECT_IO_EXPORT_DIR, prefix, textureIndex, palette)
				&& DDSorPNG(path, 256, "%stextures\\world\\dat\\wmset\\%s_%03d_%d", DIRECT_IO_EXPORT_DIR, prefix, textureIndex, 0))
				path[0] = '\0';
			it = basePaths.emplace(key, path).first;
		}
		return it->second.empty() ? nullptr : it->second.c_str();
	}
}

bool WorldVehiclesBuildPage(const DWORD tPage, const int textureType, const int textureIndex, const int palette)
{
	pending = false;
	if (!hooksInstalled || pagesDisabled || pixelsPtr == 0 || tPage >= 32 || !InWorld())
		return false;
	try
	{
		const int width = static_cast<int>(uploadWidth), height = static_cast<int>(uploadHeight);
		if (width < 256 || height < 256 || width % 256 != 0 || height % 256 != 0 || width > 2048 || height > 2048)
			return false;
		const int pageX = static_cast<int>(tPage % 16) * 64, pageY = static_cast<int>(tPage / 16) * 256;
		uint64_t generation = 0;
		const std::vector<VramTexture> textures = tracker.Snapshot(pageX, pageY, 128, 256, &generation);
		if (textures.empty())
			return false;
		const uint8_t* pixels = reinterpret_cast<const uint8_t*>(pixelsPtr);
		const size_t bytes = static_cast<size_t>(width) * height * 4;
		const char* basePath = BasePath(textureType, textureIndex, tPage, palette);

		//same page content, same textures, same base file, same visit: same plan
		const uint64_t facts[5] = { tPage, static_cast<uint64_t>(textureType), generation,
			static_cast<uint64_t>(width) << 32 | static_cast<uint32_t>(height), CurrentWorldEntry() };
		uint64_t key = XXH3_64bits_withSeed(facts, sizeof(facts), XXH3_64bits(pixels, bytes));
		if (basePath != nullptr)
			key = XXH3_64bits_withSeed(basePath, strlen(basePath), key);
		auto it = plans.find(key);
		if (it == plans.end())
		{
			if (plans.size() >= PLAN_LIMIT)
				plans.clear();
			it = plans.emplace(key, MakePlan(tPage, pixels, width, height, textures, basePath)).first;
		}
		if (it->second.stamps.empty())
			return false;
		pendingPlan = it->second;
		if (pendingPlan.basePath.empty())
			pendingPixels.assign(pixels, pixels + bytes);
		else
			pendingPixels.clear();
		pendingWidth = width;
		pendingHeight = height;
		pending = true;
		return true;
	}
	catch (const std::exception& e)
	{
		pagesDisabled = true;
		OutputDebug("WORLD_VEHICLES: page %u failed (%s), named world textures turned off for this session\n", tPage, e.what());
	}
	catch (...)
	{
		pagesDisabled = true;
		OutputDebug("WORLD_VEHICLES: page %u failed, named world textures turned off for this session\n", tPage);
	}
	pending = false;
	return false;
}

bool WorldVehiclesUploadPending()
{
	if (!pending)
		return false;
	pending = false;
	try
	{
		GLint target = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &target);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		if (!DrawPendingOnGpu(static_cast<GLuint>(target)))
			DrawPendingOnCpu();
	}
	catch (...)
	{
		pagesDisabled = true;
		OutputDebug("WORLD_VEHICLES: drawing a page failed, named world textures turned off for this session\n");
	}
	pendingPixels.clear();
	return true;
}

//WORLD_DEBUG=2: every world page the game uploads, once per type/page/palette, as DEMASTER_EXP\world_dump\*.png (BGRA)
void WorldDumpPage(const int textureType, const DWORD tPage, const int palette)
{
	if (WORLD_DEBUG < 2 || pixelsPtr == 0 || uploadWidth == 0 || uploadHeight == 0 || uploadWidth > 4096 || uploadHeight > 4096)
		return;
	static std::set<std::string> dumped;
	char name[64];
	snprintf(name, sizeof(name), "type%d_tpage%02u_palette%02d", textureType, static_cast<unsigned>(tPage), palette);
	if (!dumped.insert(name).second)
		return;
	const std::string folder = std::string(DIRECT_IO_EXPORT_DIR) + "world_dump";
	std::error_code error;
	std::filesystem::create_directories(folder, error);
	const std::string path = folder + "\\" + name + ".png";
	bx::FileWriter writer;
	if (!bx::open(&writer, bx::FilePath(path.c_str())))
		return;
	bimg::imageWritePng(&writer, uploadWidth, uploadHeight, uploadWidth * 4, reinterpret_cast<const void*>(pixelsPtr),
		bimg::TextureFormat::BGRA8, false);
	bx::close(&writer);
}

void ApplyWorldVehiclesPatch()
{
	if (!WORLD_VEHICLES)
		return;
	static const ADDRESS_NAME needed[] = { GUEST_STACK_BASE, GUEST_PAGE_TABLE, GUEST_LOADIMAGE, WMSET_GUEST,
		WM_CHARA_READ_CALL, WM_READ_FILE };
	for (const ADDRESS_NAME name : needed)
		if (!HasAddress(name))
		{
			OutputDebug("WORLD_VEHICLES: %s is not known for this game DLL, world textures stay as they are\n", GetAddressName(name));
			return;
		}
	guestStackBaseAddress = IMAGE_BASE + GetAddress(GUEST_STACK_BASE);
	guestPageTableAddress = IMAGE_BASE + GetAddress(GUEST_PAGE_TABLE);
	wmsetGuest = GetAddress(WMSET_GUEST); //a guest address, not in the DLL

	//both must be what tools/verify_addresses.py checked before anything is changed
	const DWORD loadImage = IMAGE_BASE + GetAddress(GUEST_LOADIMAGE);
	static const BYTE loadImageStart[] = { 0x55, 0x8B, 0xEC, 0x56, 0x8B, 0x75, 0x08, 0x8B, 0x46, 0x2C, 0x83, 0xC0, 0x04, 0x50, 0xE8 };
	const DWORD charaCall = IMAGE_BASE + GetAddress(WM_CHARA_READ_CALL);
	const DWORD readFile = IMAGE_BASE + GetAddress(WM_READ_FILE);
	if (memcmp(reinterpret_cast<const void*>(loadImage), loadImageStart, sizeof(loadImageStart)) != 0)
	{
		OutputDebug("WORLD_VEHICLES: unexpected code at %08X (GUEST_LOADIMAGE), world textures stay as they are\n", loadImage);
		return;
	}
	if (*reinterpret_cast<const BYTE*>(charaCall) != 0xE8 || charaCall + 5 + *reinterpret_cast<const DWORD*>(charaCall + 1) != readFile)
	{
		OutputDebug("WORLD_VEHICLES: unexpected code at %08X (WM_CHARA_READ_CALL), world textures stay as they are\n", charaCall);
		return;
	}
	if (MH_CreateHook(reinterpret_cast<LPVOID>(loadImage), reinterpret_cast<LPVOID>(&LoadImageDetour),
		reinterpret_cast<LPVOID*>(&loadImageOriginal)) != MH_OK)
	{
		OutputDebug("WORLD_VEHICLES: could not hook LoadImage, world textures stay as they are\n");
		return;
	}
	wvCharaReadOriginal = readFile;
	ReplaceCallFunction(charaCall, reinterpret_cast<void*>(WvCharaReadHook));
	hooksInstalled = true;
	OutputDebug("WORLD_VEHICLES: on (world textures copied to VRAM are tracked by name)\n");
}
