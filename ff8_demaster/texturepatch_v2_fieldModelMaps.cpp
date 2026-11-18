//Field model textures (FIELD_ENTITY's HD atlases): DDS with your own mip levels, and the models' PBR maps.
//
//The game makes a square atlas texture per model and copies each of its images (d001_0.png, o002_0.png, ...) into it with
//glTexSubImage2D, one call per image, at the image's own place. It draws the atlas with point sampling and no mip levels.
//For every image that has a .dds next to it (the newer of .png and .dds, as for field layers):
//	- the .dds goes into the atlas in place of the game's copy, every mip level of it at the matching place of the atlas's
//	  own levels. FIELD_MODEL_BC7: when every image in the atlas has a BC7 .dds whose place lines up with BC7's 4 x 4
//	  blocks, the atlas itself goes to the GPU as BC7 (its blocks copied as the files have them: a quarter of the video
//	  memory, nothing decoded), with its levels as far as every image's line up; otherwise BC7 is decoded once and the
//	  atlas stays RGBA, as before;
//	- the atlas is filtered GL_NEAREST_MIPMAP_LINEAR: inside a level exactly as before (no softening), between your own
//	  levels only when the model is drawn smaller than the texture;
//	- parts of the atlas no .dds covers get levels made from the game's own pixels.
//The models' PBR maps sit next to the images, laid out the same (texturepatch_v2_fieldChars.cpp draws with them):
//	<image>_N.dds     normal: x along the image's right, y up the image (OpenGL), z out of it; alpha kept for later
//	<image>_ORME.dds  ambient occlusion, roughness, metallic, emissive (the albedo glows where emissive is white)
//They get atlases of their own, the same size, with the same levels; where an image has no map the atlas is flat (no
//bumps) and plain (AO 1, roughness 0.7, not metal, no glow). PNG maps work too (their levels are made from them).

#include "config.h"
#include "coreHeader.h"
#include "opengl.h"
#include "texture.h"
#include "field_pbr_runtime.h"

#include <cstdio>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>

using namespace fieldrt;

bool FieldLayersIsPage(unsigned int texture); //texturepatch_v2_fieldLayers.cpp

namespace
{
	constexpr size_t DECODED_LIMIT = 384u << 20;

	//one image the game copied into an atlas
	struct Region
	{
		int x = 0, y = 0, width = 0, height = 0;
		std::string stem; //the image the game read, no extension
	};
	struct Atlas
	{
		int width = 0, height = 0;
		std::vector<Region> regions;
		bool mipmapped = false;
		bool compressed = false; //the atlas is BC7 on the GPU (FIELD_MODEL_BC7)
		int levels = 1; //levels in use
		GLuint normal = 0, orme = 0;
	};
	std::unordered_map<GLuint, Atlas> atlases;
	bool applying = false; //DEMASTER's own uploads into an atlas

	//decoded images (RGBA8, every level), kept: the game fills atlases again when it comes back to a field
	struct Decoded
	{
		std::filesystem::file_time_type time{};
		int width = 0, height = 0;
		std::vector<std::vector<uint8_t>> levels;
		std::vector<std::pair<int, int>> sizes;
	};
	std::unordered_map<std::string, Decoded> decoded;
	size_t decodedBytes = 0;

	std::filesystem::file_time_type TimeOf(const std::string& path)
	{
		std::error_code error;
		if (!std::filesystem::is_regular_file(path, error))
			return {};
		return std::filesystem::last_write_time(path, error);
	}

	bool Exists(const std::string& path)
	{
		std::error_code error;
		return !path.empty() && std::filesystem::is_regular_file(path, error);
	}

	//<stem>.dds when it is the newer of the two (or the only one)
	std::string DdsOf(const std::string& stem)
	{
		if (stem.empty())
			return {};
		const std::string dds = stem + ".dds", png = stem + ".png";
		if (!Exists(dds))
			return {};
		if (Exists(png) && TimeOf(png) > TimeOf(dds))
			return {};
		return dds;
	}

	//a map image: <stem><suffix>.dds or .png, the newer one
	std::string MapOf(const std::string& stem, const char* suffix)
	{
		if (stem.empty())
			return {};
		const std::string dds = stem + suffix + ".dds", png = stem + suffix + ".png";
		const bool hasDds = Exists(dds), hasPng = Exists(png);
		if (hasDds && hasPng)
			return TimeOf(png) > TimeOf(dds) ? png : dds;
		return hasDds ? dds : hasPng ? png : std::string();
	}

	const Decoded* Decode(const std::string& path)
	{
		const auto time = TimeOf(path);
		if (const auto it = decoded.find(path); it != decoded.end() && it->second.time == time)
			return &it->second;
		const SafeBimg image = LoadImageFromFile(path.c_str());
		if (!image)
		{
			LogOnce("model-bad-" + path, "FIELD_ENTITY: cannot read %s\n", path.c_str());
			return nullptr;
		}
		static bx::DefaultAllocator allocator;
		bimg::ImageContainer* rgba = bimg::imageConvert(&allocator, bimg::TextureFormat::RGBA8, *image, true);
		if (rgba == nullptr)
			return nullptr;
		Decoded d;
		d.time = time;
		d.width = static_cast<int>(rgba->m_width);
		d.height = static_cast<int>(rgba->m_height);
		for (uint8_t lod = 0; lod < rgba->m_numMips; lod++)
		{
			bimg::ImageMip mip{};
			if (!bimg::imageGetRawData(*rgba, 0, lod, rgba->m_data, rgba->m_size, mip))
				break;
			const size_t bytes = static_cast<size_t>(mip.m_width) * mip.m_height * 4;
			if (mip.m_size < bytes)
				break;
			d.levels.emplace_back(mip.m_data, mip.m_data + bytes);
			d.sizes.emplace_back(static_cast<int>(mip.m_width), static_cast<int>(mip.m_height));
			decodedBytes += bytes;
		}
		bimg::imageFree(rgba);
		if (d.levels.empty())
			return nullptr;
		return &(decoded[path] = std::move(d));
	}

	int LevelsOf(int size);

	//--- FIELD_MODEL_BC7: a .dds's BC7 blocks, every level, as the file has them
	struct Blocks
	{
		std::filesystem::file_time_type time{};
		bool bc7 = false;
		std::vector<std::vector<uint8_t>> levels; //16 bytes per 4 x 4 block, rows of blocks
		std::vector<std::pair<int, int>> sizes; //each level's size in texels
	};
	std::unordered_map<std::string, Blocks> blockFiles;
	size_t blockBytes = 0;

	const Blocks* ReadBlocks(const std::string& path)
	{
		const auto time = TimeOf(path);
		if (const auto it = blockFiles.find(path); it != blockFiles.end() && it->second.time == time)
			return &it->second;
		Blocks b;
		b.time = time;
		const SafeBimg image = LoadImageFromFile(path.c_str());
		if (image && image->m_format == bimg::TextureFormat::BC7)
		{
			b.bc7 = true;
			for (uint8_t lod = 0; lod < image->m_numMips; lod++)
			{
				bimg::ImageMip mip{};
				if (!bimg::imageGetRawData(*image, 0, lod, image->m_data, image->m_size, mip))
					break;
				const size_t bytes = static_cast<size_t>((mip.m_width + 3) / 4) * ((mip.m_height + 3) / 4) * 16;
				if (mip.m_size < bytes)
					break;
				b.levels.emplace_back(mip.m_data, mip.m_data + bytes);
				b.sizes.emplace_back(static_cast<int>(mip.m_width), static_cast<int>(mip.m_height));
				blockBytes += bytes;
			}
			b.bc7 = !b.levels.empty();
		}
		return &(blockFiles[path] = std::move(b));
	}

	//the bound atlas made again as BC7 from every image's blocks; its levels as far as every image's line up with the 4 x 4
	//blocks there (and the image has that level). false (nothing changed): an image without a BC7 .dds of its size, or
	//one whose place doesn't line up even at full size
	//suffix: "" for the images themselves, "_N" or "_ORME" for one of their maps (each image's map, as a BC7 .dds)
	bool PutBc7Atlas(const Atlas& atlas, int& levelsUsed, const char* suffix = "")
	{
		if (!FIELD_MODEL_BC7 || !GLEW_ARB_texture_compression_bptc)
			return false;
		std::vector<const Blocks*> images(atlas.regions.size(), nullptr);
		for (size_t i = 0; i < atlas.regions.size(); i++)
		{
			const Region& r = atlas.regions[i];
			std::string dds = *suffix == 0 ? DdsOf(r.stem) : MapOf(r.stem, suffix);
			if (dds.size() < 4 || dds.compare(dds.size() - 4, 4, ".dds") != 0)
				dds.clear();
			images[i] = dds.empty() ? nullptr : ReadBlocks(dds);
			if (images[i] == nullptr || !images[i]->bc7 || images[i]->sizes[0].first != r.width || images[i]->sizes[0].second != r.height)
				return false;
		}
		const int most = LevelsOf(std::max(atlas.width, atlas.height));
		int usable = 0;
		for (int level = 0; level < most; level++)
		{
			bool fits = true;
			for (size_t i = 0; i < images.size() && fits; i++)
			{
				const Region& r = atlas.regions[i];
				const int x = r.x >> level, y = r.y >> level;
				fits = static_cast<int>(images[i]->levels.size()) > level && x % 4 == 0 && y % 4 == 0;
			}
			if (!fits)
				break;
			usable = level + 1;
		}
		if (usable == 0)
			return false;
		static std::vector<uint8_t> data;
		for (int level = 0; level < usable; level++)
		{
			const int width = std::max(1, atlas.width >> level), height = std::max(1, atlas.height >> level);
			const int across = (width + 3) / 4, down = (height + 3) / 4;
			data.assign(static_cast<size_t>(across) * down * 16, 0); //(a block of zeros is transparent black)
			for (size_t i = 0; i < images.size(); i++)
			{
				const Region& r = atlas.regions[i];
				const int bx = (r.x >> level) / 4, by = (r.y >> level) / 4;
				const int imageAcross = (images[i]->sizes[level].first + 3) / 4, imageDown = (images[i]->sizes[level].second + 3) / 4;
				const int copyAcross = std::min(imageAcross, across - bx), copyDown = std::min(imageDown, down - by);
				for (int row = 0; row < copyDown && copyAcross > 0; row++)
					memcpy(data.data() + (static_cast<size_t>(by + row) * across + bx) * 16,
						images[i]->levels[level].data() + static_cast<size_t>(row) * imageAcross * 16, static_cast<size_t>(copyAcross) * 16);
			}
			glCompressedTexImage2D(GL_TEXTURE_2D, level, GL_COMPRESSED_RGBA_BPTC_UNORM, width, height, 0, static_cast<GLsizei>(data.size()),
				data.data());
		}
		levelsUsed = usable;
		return true;
	}

	int LevelsOf(const int size)
	{
		int levels = 1;
		for (int s = size; s > 1; s /= 2)
			levels++;
		return levels;
	}

	//an image's levels into the bound texture (an atlas of width x height) at its region; from: the first level to put
	void PutLevels(const Decoded& image, const Region& region, const int atlasWidth, const int atlasHeight, const size_t from)
	{
		for (size_t level = from; level < image.levels.size(); level++)
		{
			const int levelWidth = std::max(1, atlasWidth >> level), levelHeight = std::max(1, atlasHeight >> level);
			const int x = region.x >> level, y = region.y >> level;
			const int width = std::min({ image.sizes[level].first, levelWidth - x, std::max(1, region.width >> level) });
			const int height = std::min({ image.sizes[level].second, levelHeight - y, std::max(1, region.height >> level) });
			if (width <= 0 || height <= 0)
				break;
			glPixelStorei(GL_UNPACK_ROW_LENGTH, image.sizes[level].first);
			RawTexSubImage(static_cast<GLint>(level), x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, image.levels[level].data());
		}
		glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	}

	struct Unpack
	{
		GLint alignment = 4, rowLength = 0, buffer = 0;
		Unpack()
		{
			glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
			glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
			glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer);
			if (buffer != 0)
				glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		}
		~Unpack()
		{
			glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
			glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
			if (buffer != 0)
				glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(buffer));
		}
	};

	void SetFilter(const bool mipmapped, const int levels)
	{
		reinterpret_cast<void* (__stdcall*)(GLenum, GLenum, GLint)>(ogl_tex_parametri)
			(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmapped ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST);
		reinterpret_cast<void* (__stdcall*)(GLenum, GLenum, GLint)>(ogl_tex_parametri)(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		reinterpret_cast<void* (__stdcall*)(GLenum, GLenum, GLint)>(ogl_tex_parametri)(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mipmapped ? levels - 1 : 0);
	}

	//one of the model's map atlases: flat everywhere, the images' maps where they have one
	GLuint MakeMap(const Atlas& atlas, const char* suffix, const uint8_t neutral[4], const char* what)
	{
		//FIELD_MODEL_BC7: every image's map a BC7 .dds that lines up: the map atlas as BC7 too (nothing decoded)
		{
			GLuint texture = 0;
			glGenTextures(1, &texture);
			glBindTexture(GL_TEXTURE_2D, texture);
			if (int bc7Levels = 0; PutBc7Atlas(atlas, bc7Levels, suffix))
			{
				SetFilter(bc7Levels > 1, bc7Levels);
				reinterpret_cast<void* (__stdcall*)(GLenum, GLenum, GLint)>(ogl_tex_parametri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
				reinterpret_cast<void* (__stdcall*)(GLenum, GLenum, GLint)>(ogl_tex_parametri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
				if (FIELD_DEBUG)
					Log("FIELD_ENTITY: %s for %s%s: BC7 on the GPU, %d level(s)\n", what, atlas.regions[0].stem.c_str(),
						atlas.regions.size() > 1 ? " ..." : "", bc7Levels);
				return texture;
			}
			glDeleteTextures(1, &texture);
		}
		std::vector<const Decoded*> maps(atlas.regions.size(), nullptr);
		size_t found = 0;
		bool levels = false;
		for (size_t i = 0; i < atlas.regions.size(); i++)
		{
			const std::string path = MapOf(atlas.regions[i].stem, suffix);
			if (!path.empty())
				maps[i] = Decode(path);
			if (maps[i] != nullptr)
			{
				found++;
				levels |= maps[i]->levels.size() > 1;
			}
		}
		if (found == 0)
			return 0;
		GLuint texture = 0;
		glGenTextures(1, &texture);
		glBindTexture(GL_TEXTURE_2D, texture);
		std::vector<uint8_t> flat(static_cast<size_t>(atlas.width) * atlas.height * 4);
		for (size_t i = 0; i < flat.size(); i += 4)
			memcpy(flat.data() + i, neutral, 4);
		RawTexImage(GL_RGBA8, atlas.width, atlas.height, GL_RGBA, GL_UNSIGNED_BYTE, flat.data());
		for (size_t i = 0; i < maps.size(); i++)
			if (maps[i] != nullptr)
				PutLevels(*maps[i], atlas.regions[i], atlas.width, atlas.height, 0);
		//the levels: made from level 0, then the maps' own where they have them
		glGenerateMipmap(GL_TEXTURE_2D);
		for (size_t i = 0; i < maps.size(); i++)
			if (maps[i] != nullptr && maps[i]->levels.size() > 1)
				PutLevels(*maps[i], atlas.regions[i], atlas.width, atlas.height, 1);
		SetFilter(true, LevelsOf(std::max(atlas.width, atlas.height)));
		reinterpret_cast<void* (__stdcall*)(GLenum, GLenum, GLint)>(ogl_tex_parametri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		reinterpret_cast<void* (__stdcall*)(GLenum, GLenum, GLint)>(ogl_tex_parametri)(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		if (FIELD_DEBUG)
			Log("FIELD_ENTITY: %s for %s%s: %zu of %zu image(s)%s\n", what, atlas.regions[0].stem.c_str(), atlas.regions.size() > 1 ? " ..." : "",
				found, atlas.regions.size(), levels ? ", with their levels" : "");
		return texture;
	}

	void DeleteMaps(Atlas& atlas)
	{
		if (atlas.normal != 0)
			glDeleteTextures(1, &atlas.normal);
		if (atlas.orme != 0)
			glDeleteTextures(1, &atlas.orme);
		atlas.normal = atlas.orme = 0;
	}

	//the game just copied an image into the atlas (bound): every .dds image in place of the game's copy, with all its
	//levels; the maps made again
	void Apply(const GLuint texture, Atlas& atlas)
	{
		if (decodedBytes > DECODED_LIMIT) //only here: the pointers Decode hands out stay good until the next Apply
		{
			decoded.clear();
			decodedBytes = 0;
		}
		if (blockBytes > DECODED_LIMIT)
		{
			blockFiles.clear();
			blockBytes = 0;
		}
		applying = true;
		Unpack unpack;
		//FIELD_MODEL_BC7: every image a BC7 .dds that lines up: the atlas as BC7, nothing decoded
		int bc7Levels = 0;
		if (PutBc7Atlas(atlas, bc7Levels))
		{
			const bool first = !atlas.compressed;
			atlas.compressed = true;
			atlas.levels = bc7Levels;
			atlas.mipmapped = bc7Levels > 1;
			SetFilter(atlas.mipmapped, bc7Levels);
			if (first)
				LogOnce("model-bc7-" + atlas.regions[0].stem + "-" + std::to_string(texture), "FIELD_ENTITY: atlas %u (%dx%d, %s ...) is BC7 on "
					"the GPU, %d level(s)\n", texture, atlas.width, atlas.height, atlas.regions[0].stem.c_str(), bc7Levels);
		}
		else
		{
		//BC7 no longer fits (an image without a BC7 .dds came in): RGBA again, every image from its file
		const bool wasCompressed = atlas.compressed;
		if (wasCompressed)
		{
			atlas.compressed = false;
			RawTexImage(GL_RGBA8, atlas.width, atlas.height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
			LogOnce("model-rgba-" + std::to_string(texture), "FIELD_ENTITY: atlas %u is RGBA again (%s has no BC7 .dds that fits)\n", texture,
				atlas.regions.back().stem.c_str());
		}
		std::vector<const Decoded*> images(atlas.regions.size(), nullptr);
		size_t count = 0;
		for (size_t i = 0; i < atlas.regions.size(); i++)
		{
			const std::string dds = DdsOf(atlas.regions[i].stem);
			if (!dds.empty())
				images[i] = Decode(dds);
			else if (wasCompressed)
				if (const Decoded* png = Decode(atlas.regions[i].stem + ".png"); png != nullptr)
					PutLevels(*png, atlas.regions[i], atlas.width, atlas.height, 0); //the game's copy didn't go into the BC7 atlas
			count += images[i] != nullptr;
		}
		const int levels = LevelsOf(std::max(atlas.width, atlas.height));
		atlas.levels = levels;
		atlas.mipmapped = count > 0;
		if (atlas.mipmapped)
		{
			for (size_t i = 0; i < images.size(); i++)
				if (images[i] != nullptr)
					PutLevels(*images[i], atlas.regions[i], atlas.width, atlas.height, 0);
			glGenerateMipmap(GL_TEXTURE_2D); //every level (from the game's pixels where no .dds is)
			for (size_t i = 0; i < images.size(); i++)
				if (images[i] != nullptr && images[i]->levels.size() > 1)
					PutLevels(*images[i], atlas.regions[i], atlas.width, atlas.height, 1);
			SetFilter(true, levels);
			const Region& last = atlas.regions.back();
			if (images.back() != nullptr)
				LogOnce("model-dds-" + last.stem + "-" + std::to_string(texture), "FIELD_ENTITY: %s.dds into atlas %u at %d,%d (%zu level(s))\n",
					last.stem.c_str(), texture, last.x, last.y, images.back()->levels.size());
		}
		}
		DeleteMaps(atlas);
		static const uint8_t flatNormal[4] = { 128, 128, 255, 255 };
		static const uint8_t plain[4] = { 255, 179, 0, 0 };
		atlas.normal = MakeMap(atlas, "_N", flatNormal, "normal map");
		atlas.orme = MakeMap(atlas, "_ORME", plain, "ORME map");
		glBindTexture(GL_TEXTURE_2D, texture);
		applying = false;
	}

	void Forget(const GLuint texture)
	{
		const auto it = atlases.find(texture);
		if (it == atlases.end())
			return;
		DeleteMaps(it->second);
		atlases.erase(it);
	}

	//a PNG's size from its header (IHDR right after the 8-byte signature)
	bool PngSize(const std::string& path, int& width, int& height)
	{
		FILE* f = fopen(path.c_str(), "rb");
		if (f == nullptr)
			return false;
		uint8_t header[24];
		const bool read = fread(header, 1, sizeof(header), f) == sizeof(header);
		fclose(f);
		if (!read || header[0] != 0x89 || header[1] != 'P' || header[12] != 'I' || header[13] != 'H')
			return false;
		width = header[16] << 24 | header[17] << 16 | header[18] << 8 | header[19];
		height = header[20] << 24 | header[21] << 16 | header[22] << 8 | header[23];
		return width > 0 && height > 0;
	}

	//"...\\field_hd_new\\d001_0.png" -> "...\\field_hd_new\\d001_0" (only the game's HD model images)
	std::string ImageStem(const std::string& path)
	{
		if (path.size() < 5)
			return {};
		std::string lower = path;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) { return static_cast<char>(tolower(c)); });
		if (lower.compare(lower.size() - 4, 4, ".png") != 0 || lower.find("field_hd") == std::string::npos)
			return {};
		return path.substr(0, path.size() - 4);
	}
}

//glTexImage2D of the game (after it), level 0 of GL_TEXTURE_2D, texture bound: the texture is made again, so whatever
//DEMASTER knew of it as a model atlas is gone (the texture type the game last loaded can't tell: it stays 57 while the
//field's background pages are made, so atlases are found from the images copied into them instead)
bool FieldModelTexImage(const unsigned int texture, const int width, const int height, const void* data)
{
	if (applying || texture == 0)
		return false;
	Forget(texture);
	(void)width;
	(void)height;
	(void)data;
	return false;
}

//glTexSubImage2D of the game into a texture (after it), level 0, texture bound: an image copied into a model atlas
void FieldModelTexSubImage(const unsigned int texture, const int x, const int y, const int width, const int height)
{
	if (applying || texture == 0)
		return;
	try
	{
		const std::string stem = ImageStem(LastFilePath);
		auto it = atlases.find(texture);
		if (stem.empty() || FieldLayersIsPage(texture))
			return;
		//only when the copy is that image, whole (the last file read can be a model image while something else is filled)
		int imageWidth = 0, imageHeight = 0;
		if (!PngSize(stem + ".png", imageWidth, imageHeight) || imageWidth != width || imageHeight != height)
			return;
		if (it == atlases.end())
		{
			//a model atlas: its size from the driver
			GLint atlasWidth = 0, atlasHeight = 0;
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &atlasWidth);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &atlasHeight);
			if (atlasWidth < 64 || atlasHeight < 64)
				return;
			Atlas atlas;
			atlas.width = atlasWidth;
			atlas.height = atlasHeight;
			it = atlases.emplace(texture, atlas).first;
		}
		Atlas& atlas = it->second;
		Region region;
		region.x = x;
		region.y = y;
		region.width = width;
		region.height = height;
		region.stem = stem;
		auto same = std::find_if(atlas.regions.begin(), atlas.regions.end(), [&](const Region& r) { return r.x == x && r.y == y; });
		if (same != atlas.regions.end())
			*same = region;
		else
			atlas.regions.push_back(region);
		Apply(texture, atlas);
	}
	catch (...)
	{
		LogOnce("model-failed", "FIELD_ENTITY: putting a model's .dds into its atlas failed once\n");
		applying = false;
	}
}

//glTexSubImage2D of the game, before it: the texture is a model atlas DEMASTER made BC7 (FIELD_MODEL_BC7). The game's copy
//(RGBA) can't go into it: it is skipped, and FieldModelTexSubImage puts the image's .dds in instead (or makes the atlas
//RGBA again when the image has none)
bool FieldModelIsBc7(const unsigned int texture)
{
	if (atlases.empty())
		return false;
	const auto it = atlases.find(texture);
	return it != atlases.end() && it->second.compressed;
}

//glTexParameteri: an atlas with levels keeps GL_NEAREST_MIPMAP_LINEAR (the game asks for GL_NEAREST)
int FieldModelFilter(const unsigned int texture, const unsigned int name, const int param)
{
	if (name != GL_TEXTURE_MIN_FILTER || atlases.empty())
		return param;
	const auto it = atlases.find(texture);
	if (it == atlases.end() || !it->second.mipmapped)
		return param;
	return param == GL_NEAREST || param == GL_LINEAR ? GL_NEAREST_MIPMAP_LINEAR : param;
}

bool FieldModelMaps(const unsigned int texture, unsigned int& normal, unsigned int& orme)
{
	normal = orme = 0;
	const auto it = atlases.find(texture);
	if (it == atlases.end())
		return false;
	normal = it->second.normal;
	orme = it->second.orme;
	return normal != 0 || orme != 0;
}
