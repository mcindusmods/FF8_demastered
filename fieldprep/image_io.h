#pragma once
//Reading and writing fieldprep's images: PNG (8 or 16 bits, lodepng) and BC7 DDS (bc7enc / bc7decomp).

#include "../ff8_demaster/field_layers.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fieldprep
{
	using fieldlayers::Image; //RGBA, 8 bits per channel

	//one channel, 0-1 (16-bit files keep their precision)
	struct Gray
	{
		int width = 0, height = 0;
		std::vector<float> v;
		bool sixteen = false; //read from a 16-bit file
		bool IsValid() const { return width > 0 && height > 0 && v.size() >= static_cast<size_t>(width) * height; }
		float At(const int x, const int y) const { return v[static_cast<size_t>(y) * width + x]; }
		float Sample(double x, double y) const; //bilinear, pixel centres at +0.5
	};

	//PNG or DDS (BC7, or uncompressed 32-bit). false: missing or unreadable (error says why)
	bool LoadRgba(const std::string& path, Image& out, std::string& error);
	//PNG (8 or 16 bits, grey or colour: the first channel); bits: what the file had
	bool LoadGray(const std::string& path, Gray& out, std::string& error);

	bool SavePng(const std::string& path, const Image& image, std::string& error);
	//the same with a text note in the file (a tEXt chunk: keyword, then text), e.g. who wrote it
	bool SavePngNoted(const std::string& path, const Image& image, const char* keyword, const char* text, std::string& error);
	//a PNG's tEXt note under this keyword (read from the chunks, the picture isn't decoded); "" when it has none
	std::string PngNote(const std::string& path, const char* keyword);
	bool SaveGrayPng(const std::string& path, const Gray& image, bool sixteen, std::string& error);

	//BC7 (4x4 blocks; other sizes are padded). perceptual: colour (weights for the eye), else data (every channel the same).
	//quality: bc7enc's uber level, 0 (fast) to 4 (best)
	std::vector<uint8_t> EncodeBc7(const Image& image, bool perceptual, int threads, int quality = 1);
	Image DecodeBc7(const std::vector<uint8_t>& blocks, int width, int height);
	bool SaveDds(const std::string& path, int width, int height, const std::vector<uint8_t>& bc7, std::string& error);
	//a BC7 DDS with mip levels (level 0 first, each half the last, down to 1x1). srgb: BC7_UNORM_SRGB (colour), else BC7_UNORM
	struct Bc7Level
	{
		int width = 0, height = 0;
		std::vector<uint8_t> blocks;
	};
	bool SaveDdsLevels(const std::string& path, const std::vector<Bc7Level>& levels, bool srgb, std::string& error);

	//<stem>.png or <stem>.dds, the newer one (as DEMASTER does); "" when neither
	std::string NewestImage(const std::string& stem);
	//<stem>.png if there is one (the master: a DDS of it is BC7, which loses a little), else <stem>.dds
	std::string MasterImage(const std::string& stem);
	bool FileExists(const std::string& path);
}
