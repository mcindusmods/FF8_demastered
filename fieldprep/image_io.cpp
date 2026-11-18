#include "image_io.h"

#include "third_party/bc7decomp.h"
#include "third_party/bc7enc.h"
#include "third_party/lodepng.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace fieldprep
{
	float Gray::Sample(const double x, const double y) const
	{
		const double fx = std::clamp(x - 0.5, 0.0, width - 1.0), fy = std::clamp(y - 0.5, 0.0, height - 1.0);
		const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
		const int x1 = std::min(x0 + 1, width - 1), y1 = std::min(y0 + 1, height - 1);
		const double ax = fx - x0, ay = fy - y0;
		return static_cast<float>((At(x0, y0) * (1 - ax) + At(x1, y0) * ax) * (1 - ay) + (At(x0, y1) * (1 - ax) + At(x1, y1) * ax) * ay);
	}

	bool FileExists(const std::string& path)
	{
		std::error_code error;
		return !path.empty() && std::filesystem::is_regular_file(path, error);
	}

	std::string MasterImage(const std::string& stem)
	{
		const std::string png = stem + ".png", dds = stem + ".dds";
		return FileExists(png) ? png : FileExists(dds) ? dds : std::string();
	}

	std::string NewestImage(const std::string& stem)
	{
		const std::string png = stem + ".png", dds = stem + ".dds";
		const bool hasPng = FileExists(png), hasDds = FileExists(dds);
		if (hasPng && hasDds)
		{
			std::error_code error;
			return std::filesystem::last_write_time(png, error) > std::filesystem::last_write_time(dds, error) ? png : dds;
		}
		return hasPng ? png : hasDds ? dds : std::string();
	}

	namespace
	{
		uint32_t U32(const uint8_t* p)
		{
			return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
		}

		void Put32(std::vector<uint8_t>& out, const uint32_t v)
		{
			for (int i = 0; i < 4; i++)
				out.push_back(static_cast<uint8_t>(v >> (i * 8)));
		}

		bool ReadAll(const std::string& path, std::vector<uint8_t>& out)
		{
			std::ifstream in(path, std::ios::binary);
			if (!in)
				return false;
			out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
			return true;
		}

		bool LoadDds(const std::string& path, Image& out, std::string& error)
		{
			std::vector<uint8_t> file;
			if (!ReadAll(path, file) || file.size() < 128 || memcmp(file.data(), "DDS ", 4) != 0)
			{
				error = "not a DDS file";
				return false;
			}
			const uint8_t* h = file.data() + 4;
			const int height = static_cast<int>(U32(h + 8)), width = static_cast<int>(U32(h + 12));
			const uint8_t* pf = h + 72; //DDS_PIXELFORMAT
			const uint32_t pfFlags = U32(pf + 4), fourCC = U32(pf + 8);
			size_t offset = 128;
			if (width <= 0 || height <= 0 || width > 16384 || height > 16384)
			{
				error = "bad DDS size";
				return false;
			}
			if ((pfFlags & 4) && fourCC == 0x30315844u) //"DX10"
			{
				if (file.size() < 148)
				{
					error = "short DDS";
					return false;
				}
				const uint32_t dxgi = U32(file.data() + 128);
				offset = 148;
				if (dxgi == 98 || dxgi == 99)
				{
					const int bw = (width + 3) / 4, bh = (height + 3) / 4;
					if (file.size() < offset + static_cast<size_t>(bw) * bh * 16)
					{
						error = "short DDS";
						return false;
					}
					const std::vector<uint8_t> blocks(file.begin() + offset, file.begin() + offset + static_cast<size_t>(bw) * bh * 16);
					out = DecodeBc7(blocks, width, height);
					return true;
				}
				if (dxgi == 28 || dxgi == 29 || dxgi == 87 || dxgi == 91) //RGBA8 / BGRA8
				{
					const bool bgra = dxgi == 87 || dxgi == 91;
					if (file.size() < offset + static_cast<size_t>(width) * height * 4)
					{
						error = "short DDS";
						return false;
					}
					out.width = width;
					out.height = height;
					out.rgba.assign(file.begin() + offset, file.begin() + offset + static_cast<size_t>(width) * height * 4);
					if (bgra)
						for (size_t i = 0; i < out.rgba.size(); i += 4)
							std::swap(out.rgba[i], out.rgba[i + 2]);
					return true;
				}
				error = "DDS format " + std::to_string(dxgi) + " (fieldprep reads BC7 and 32-bit DDS)";
				return false;
			}
			if ((pfFlags & 0x40) && U32(pf + 12) == 32) //uncompressed 32-bit
			{
				const uint32_t rMask = U32(pf + 16);
				if (file.size() < offset + static_cast<size_t>(width) * height * 4)
				{
					error = "short DDS";
					return false;
				}
				out.width = width;
				out.height = height;
				out.rgba.assign(file.begin() + offset, file.begin() + offset + static_cast<size_t>(width) * height * 4);
				if (rMask == 0x00FF0000u)
					for (size_t i = 0; i < out.rgba.size(); i += 4)
						std::swap(out.rgba[i], out.rgba[i + 2]);
				if ((pfFlags & 1) == 0)
					for (size_t i = 3; i < out.rgba.size(); i += 4)
						out.rgba[i] = 255;
				return true;
			}
			error = "DDS format not read by fieldprep (BC7 or 32-bit only)";
			return false;
		}
	}

	bool LoadRgba(const std::string& path, Image& out, std::string& error)
	{
		out = Image();
		std::string extension = std::filesystem::path(path).extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char c) { return static_cast<char>(tolower(c)); });
		if (extension == ".dds")
			return LoadDds(path, out, error);
		std::vector<unsigned char> pixels;
		unsigned width = 0, height = 0;
		const unsigned result = lodepng::decode(pixels, width, height, path, LCT_RGBA, 8);
		if (result != 0)
		{
			error = lodepng_error_text(result);
			return false;
		}
		out.width = static_cast<int>(width);
		out.height = static_cast<int>(height);
		out.rgba.assign(pixels.begin(), pixels.end());
		return true;
	}

	bool LoadGray(const std::string& path, Gray& out, std::string& error)
	{
		out = Gray();
		std::string extension = std::filesystem::path(path).extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](const unsigned char c) { return static_cast<char>(tolower(c)); });
		if (extension == ".dds")
		{
			Image rgba;
			if (!LoadDds(path, rgba, error))
				return false;
			out.width = rgba.width;
			out.height = rgba.height;
			out.v.resize(static_cast<size_t>(rgba.width) * rgba.height);
			for (size_t i = 0; i < out.v.size(); i++)
				out.v[i] = rgba.rgba[i * 4] / 255.0f;
			return true;
		}
		std::vector<unsigned char> file;
		unsigned result = lodepng::load_file(file, path);
		if (result != 0)
		{
			error = lodepng_error_text(result);
			return false;
		}
		lodepng::State state;
		unsigned width = 0, height = 0;
		result = lodepng_inspect(&width, &height, &state, file.data(), file.size());
		if (result != 0)
		{
			error = lodepng_error_text(result);
			return false;
		}
		const bool sixteen = state.info_png.color.bitdepth == 16;
		std::vector<unsigned char> pixels;
		result = lodepng::decode(pixels, width, height, file, LCT_RGBA, sixteen ? 16 : 8);
		if (result != 0)
		{
			error = lodepng_error_text(result);
			return false;
		}
		out.width = static_cast<int>(width);
		out.height = static_cast<int>(height);
		out.sixteen = sixteen;
		out.v.resize(static_cast<size_t>(width) * height);
		for (size_t i = 0; i < out.v.size(); i++)
			out.v[i] = sixteen ? (pixels[i * 8] << 8 | pixels[i * 8 + 1]) / 65535.0f : pixels[i * 4] / 255.0f;
		return true;
	}

	bool SavePng(const std::string& path, const Image& image, std::string& error)
	{
		const unsigned result = lodepng::encode(path, image.rgba, static_cast<unsigned>(image.width), static_cast<unsigned>(image.height), LCT_RGBA, 8);
		if (result != 0)
		{
			error = lodepng_error_text(result);
			return false;
		}
		return true;
	}

	bool SavePngNoted(const std::string& path, const Image& image, const char* keyword, const char* text, std::string& error)
	{
		lodepng::State state;
		state.info_raw.colortype = LCT_RGBA;
		state.info_raw.bitdepth = 8;
		state.info_png.color.colortype = LCT_RGBA;
		state.info_png.color.bitdepth = 8;
		state.encoder.auto_convert = 0;
		state.encoder.text_compression = 0; //a plain tEXt chunk (PngNote reads those)
		lodepng_add_text(&state.info_png, keyword, text);
		std::vector<unsigned char> png;
		unsigned result = lodepng::encode(png, image.rgba, static_cast<unsigned>(image.width), static_cast<unsigned>(image.height), state);
		if (result == 0)
			result = lodepng::save_file(png, path);
		if (result != 0)
		{
			error = lodepng_error_text(result);
			return false;
		}
		return true;
	}

	std::string PngNote(const std::string& path, const char* keyword)
	{
		FILE* file = fopen(path.c_str(), "rb");
		if (file == nullptr)
			return "";
		std::string found;
		unsigned char head[8];
		if (fread(head, 1, 8, file) == 8 && memcmp(head, "\x89PNG\r\n\x1a\n", 8) == 0)
		{
			const std::string want = keyword;
			//the chunks: length (big-endian), type, data, CRC; lodepng writes its notes after the picture's data, so every
			//chunk is passed over (by seeking: the picture isn't read) up to the end
			for (int chunks = 0; chunks < 100000; chunks++)
			{
				unsigned char c[8];
				if (fread(c, 1, 8, file) != 8)
					break;
				const uint32_t length = static_cast<uint32_t>(c[0]) << 24 | c[1] << 16 | c[2] << 8 | c[3];
				const std::string type(reinterpret_cast<const char*>(c + 4), 4);
				if (type == "IEND" || length > (1u << 30))
					break;
				if (type != "tEXt" || length > (1u << 20))
				{
					if (fseek(file, static_cast<long>(length) + 4, SEEK_CUR) != 0)
						break;
					continue;
				}
				std::string data(length, '\0');
				if (fread(data.data(), 1, length, file) != length || fseek(file, 4, SEEK_CUR) != 0)
					break;
				const size_t zero = data.find('\0');
				if (zero != std::string::npos && data.compare(0, zero, want) == 0 && zero == want.size())
				{
					found = data.substr(zero + 1);
					break;
				}
			}
		}
		fclose(file);
		return found;
	}

	bool SaveGrayPng(const std::string& path, const Gray& image, const bool sixteen, std::string& error)
	{
		std::vector<unsigned char> pixels(static_cast<size_t>(image.width) * image.height * (sixteen ? 2 : 1));
		for (size_t i = 0; i < static_cast<size_t>(image.width) * image.height; i++)
		{
			const float v = std::clamp(image.v[i], 0.0f, 1.0f);
			if (sixteen)
			{
				const unsigned s = static_cast<unsigned>(std::lround(v * 65535.0f));
				pixels[i * 2] = static_cast<unsigned char>(s >> 8);
				pixels[i * 2 + 1] = static_cast<unsigned char>(s & 0xFF);
			}
			else
				pixels[i] = static_cast<unsigned char>(std::lround(v * 255.0f));
		}
		const unsigned result = lodepng::encode(path, pixels, static_cast<unsigned>(image.width), static_cast<unsigned>(image.height), LCT_GREY, sixteen ? 16 : 8);
		if (result != 0)
		{
			error = lodepng_error_text(result);
			return false;
		}
		return true;
	}

	std::vector<uint8_t> EncodeBc7(const Image& image, const bool perceptual, int threads, const int quality)
	{
		static std::once_flag once;
		std::call_once(once, [] { bc7enc_compress_block_init(); });
		const int bw = (image.width + 3) / 4, bh = (image.height + 3) / 4;
		std::vector<uint8_t> out(static_cast<size_t>(bw) * bh * 16);
		bc7enc_compress_block_params params;
		bc7enc_compress_block_params_init(&params);
		if (!perceptual)
			bc7enc_compress_block_params_init_linear_weights(&params);
		params.m_uber_level = static_cast<uint32_t>(std::clamp(quality, 0, static_cast<int>(BC7ENC_MAX_UBER_LEVEL)));
		threads = std::clamp(threads, 1, std::max(1, bh));
		const auto work = [&](const int first, const int step)
		{
			uint8_t block[64];
			for (int by = first; by < bh; by += step)
				for (int bx = 0; bx < bw; bx++)
				{
					for (int y = 0; y < 4; y++)
						for (int x = 0; x < 4; x++)
						{
							const int sx = std::min(bx * 4 + x, image.width - 1), sy = std::min(by * 4 + y, image.height - 1);
							memcpy(block + (y * 4 + x) * 4, image.rgba.data() + (static_cast<size_t>(sy) * image.width + sx) * 4, 4);
						}
					bc7enc_compress_block(out.data() + (static_cast<size_t>(by) * bw + bx) * 16, block, &params);
				}
		};
		std::vector<std::thread> pool;
		for (int t = 1; t < threads; t++)
			pool.emplace_back(work, t, threads);
		work(0, threads);
		for (std::thread& t : pool)
			t.join();
		return out;
	}

	Image DecodeBc7(const std::vector<uint8_t>& blocks, const int width, const int height)
	{
		Image out;
		out.width = width;
		out.height = height;
		out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
		const int bw = (width + 3) / 4, bh = (height + 3) / 4;
		if (blocks.size() < static_cast<size_t>(bw) * bh * 16)
			return out;
		bc7decomp::color_rgba pixels[16];
		for (int by = 0; by < bh; by++)
			for (int bx = 0; bx < bw; bx++)
			{
				bc7decomp::unpack_bc7(blocks.data() + (static_cast<size_t>(by) * bw + bx) * 16, pixels);
				for (int y = 0; y < 4; y++)
					for (int x = 0; x < 4; x++)
					{
						const int dx = bx * 4 + x, dy = by * 4 + y;
						if (dx < width && dy < height)
							memcpy(out.rgba.data() + (static_cast<size_t>(dy) * width + dx) * 4, &pixels[y * 4 + x], 4);
					}
			}
		return out;
	}

	bool SaveDdsLevels(const std::string& path, const std::vector<Bc7Level>& levels, const bool srgb, std::string& error)
	{
		if (levels.empty())
		{
			error = "no levels";
			return false;
		}
		std::vector<uint8_t> out;
		out.insert(out.end(), { 'D', 'D', 'S', ' ' });
		const int width = levels[0].width, height = levels[0].height;
		const int bw = (width + 3) / 4, bh = (height + 3) / 4;
		Put32(out, 124);
		Put32(out, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000); //caps, height, width, pixel format, mip count, linear size
		Put32(out, static_cast<uint32_t>(height));
		Put32(out, static_cast<uint32_t>(width));
		Put32(out, static_cast<uint32_t>(bw * bh * 16));
		Put32(out, 0); //depth
		Put32(out, static_cast<uint32_t>(levels.size())); //mips
		for (int i = 0; i < 11; i++)
			Put32(out, 0);
		Put32(out, 32); //DDS_PIXELFORMAT
		Put32(out, 0x4); //fourCC
		out.insert(out.end(), { 'D', 'X', '1', '0' });
		for (int i = 0; i < 5; i++)
			Put32(out, 0);
		Put32(out, 0x1000 | (levels.size() > 1 ? 0x8 | 0x400000 : 0)); //texture (complex, mipmap)
		for (int i = 0; i < 4; i++)
			Put32(out, 0);
		Put32(out, srgb ? 99 : 98); //DXGI_FORMAT_BC7_UNORM_SRGB, DXGI_FORMAT_BC7_UNORM
		Put32(out, 3); //2D texture
		Put32(out, 0);
		Put32(out, 1); //array size
		Put32(out, 0);
		for (const Bc7Level& level : levels)
		{
			const size_t expected = static_cast<size_t>((level.width + 3) / 4) * ((level.height + 3) / 4) * 16;
			if (level.blocks.size() != expected)
			{
				error = "a level's size doesn't match its blocks";
				return false;
			}
			out.insert(out.end(), level.blocks.begin(), level.blocks.end());
		}
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			error = "can't write " + path;
			return false;
		}
		file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
		if (!file)
		{
			error = "can't write " + path;
			return false;
		}
		return true;
	}

	bool SaveDds(const std::string& path, const int width, const int height, const std::vector<uint8_t>& bc7, std::string& error)
	{
		std::vector<uint8_t> out;
		out.insert(out.end(), { 'D', 'D', 'S', ' ' });
		const int bw = (width + 3) / 4, bh = (height + 3) / 4;
		Put32(out, 124);
		Put32(out, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000); //caps, height, width, pixel format, mip count, linear size
		Put32(out, static_cast<uint32_t>(height));
		Put32(out, static_cast<uint32_t>(width));
		Put32(out, static_cast<uint32_t>(bw * bh * 16));
		Put32(out, 0); //depth
		Put32(out, 1); //mips
		for (int i = 0; i < 11; i++)
			Put32(out, 0);
		Put32(out, 32); //DDS_PIXELFORMAT
		Put32(out, 0x4); //fourCC
		out.insert(out.end(), { 'D', 'X', '1', '0' });
		for (int i = 0; i < 5; i++)
			Put32(out, 0);
		Put32(out, 0x1000); //texture
		for (int i = 0; i < 4; i++)
			Put32(out, 0);
		Put32(out, 98); //DXGI_FORMAT_BC7_UNORM
		Put32(out, 3); //2D texture
		Put32(out, 0);
		Put32(out, 1); //array size
		Put32(out, 0);
		out.insert(out.end(), bc7.begin(), bc7.end());
		std::ofstream file(path, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			error = "can't write " + path;
			return false;
		}
		file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
		if (!file)
		{
			error = "can't write " + path;
			return false;
		}
		return true;
	}
}
