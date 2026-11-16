#pragma once
//fieldprep model: the field models' textures (DEMASTER_EXP\textures\field.fs\field_hd_new\d001_0.png and so on) made
//ready for DEMASTER (FIELD_PBR_CHARACTERS): BC7 DDS files with all their mip levels, next to the image.
//
//For an image d001_0.png, from your PNGs next to it:
//	d001_0_A.png (or _AM)                  -> d001_0.dds       the albedo (else d001_0.png itself); sRGB, levels in linear
//	                                                           light, see-through pixels don't bleed, cut-outs keep their
//	                                                           coverage in the small levels (the game's 44% test)
//	d001_0_N.png                           -> d001_0_N.dds     normal, green up (--flip-green for green down), levels
//	                                                           made unit length again
//	d001_0_O/_R/_M/_E.png (or _ORME.png)   -> d001_0_ORME.dds  AO, roughness, metallic, emissive packed; missing ones plain
//	                                                           (AO 1, roughness 0.7, not metal, no glow), every channel's
//	                                                           levels kept apart
//Maps at another size than d001_0.png are resized to it (the game places the image by the PNG's size). d001_0.png stays:
//the game reads it. A DDS newer than everything it's made from is left alone (--force makes it again).

#include "image_io.h"

#include <string>
#include <vector>

namespace fieldprep
{
	struct ModelOptions
	{
		int threads = 4;
		bool force = false; //make every DDS again
		bool flipGreen = false; //the normal maps are green down (DirectX)
		int quality = 2; //BC7 effort: bc7enc's uber level, 0-4
		float coverage = 0.44f; //the game's cut-out test (alpha below it is dropped); 0: the albedo's alpha averaged plainly
	};
	//target: an image (d001_0.png, or one of its maps), or a folder: every image in it that has PBR maps. Returns how many
	//images failed
	int Models(const std::string& target, const ModelOptions& o);

	//--- the pieces (tests/field_pbr_test.cpp)
	//each level half the last (at least 1), down to 1x1, level 0 first
	std::vector<Image> AlbedoLevels(const Image& level0, float coverage);
	std::vector<Image> DataLevels(const Image& level0);
	std::vector<Image> NormalLevels(const Image& level0);
	//how much of an image passes the cut-out test (alpha >= coverage x 255)
	double Coverage(const Image& image, float coverage);
	//area-weighted (smaller) or bilinear (larger), every channel as it is
	Image ResizeImage(const Image& image, int width, int height);
	//the ORME map from its four channels (each null: plain)
	Image PackOrme(int width, int height, const Image* ao, const Image* roughness, const Image* metallic, const Image* emissive);
	//"d001_0_ORME.png" -> "d001_0", "d001_0.png" -> "d001_0"; suffix: the map's ("" for the image itself)
	std::string ModelStem(const std::string& fileName, std::string* suffix = nullptr);
}
