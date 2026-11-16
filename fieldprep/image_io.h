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
	bool SaveGrayPng(const std::string& path, const Gray& image, bool sixteen, std::string& error);

	//BC7 (4x4 blocks; width and height multiples of 4). perceptual: colour (weights for the eye), else data (every channel the same)
	std::vector<uint8_t> EncodeBc7(const Image& image, bool perceptual, int threads);
	Image DecodeBc7(const std::vector<uint8_t>& blocks, int width, int height);
	bool SaveDds(const std::string& path, int width, int height, const std::vector<uint8_t>& bc7, std::string& error);

	//<stem>.png or <stem>.dds, the newer one (as DEMASTER does); "" when neither
	std::string NewestImage(const std::string& stem);
	bool FileExists(const std::string& path);
}
