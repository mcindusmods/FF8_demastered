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
//All three are padded around their islands in every level (see IslandCoverage below).
//Maps at another size than d001_0.png are resized to it (the game places the image by the PNG's size). d001_0.png stays:
//the game reads it. A DDS newer than everything it's made from is left alone (--force makes it again).
//
//fieldprep battle: the same for the battle textures (DEMASTER_EXP\textures\battle.fs\hd_new): every image there, the
//stages' pages (a0stg012_16.png, an animated page's frames a0stg012_16_0.png ...) and the fighters', enemies', bosses' and
//GFs' (d0c000_0, c0m001_0, mag001_0 ...), each with whatever maps it has:
//	<image>.png (or _A, _AM)                -> <image>.dds        the albedo, as above
//	<image>_N.png                           -> <image>_N.dds      as above
//	<image>_O/_R/_M/_H.png (or _ORMH.png)   -> <image>_ORMH.dds   AO, roughness, metallic, height (DEMASTER: 1 the surface,
//	                                                              0 the deepest; parallax); missing ones plain (height 1)
//	<image>_E.png                           -> <image>_E.dds      the glows' colour at night (sRGB), on black: DEMASTER adds
//	                                                              it (an _E's alpha isn't read: the DDS's is white)
//	<image>_EC.png                          -> <image>_EC.dds     a picture of the glows with their coverage (its alpha); an
//	                                                              _E.png with an alpha of its own is one too: -> _EC.dds
//A page's maps without its image (an animated page's a0stg012_16_N.png for all its frames) are made at their own size.
//DEMASTER reads the DDS whenever there is one (DDS before PNG), so run it again after changing a PNG.

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
		bool pad = true; //fill what the islands don't cover from their edges, in every level (--no-pad: off)
		bool battle = false; //the battle textures' files (Battle), not the field models' (ORMH, _E and _EC; every image)
		std::string match; //battle, a folder: only the images whose names match (* for any letters, a comma between several)
	};
	//target: an image (d001_0.png, or one of its maps), or a folder: every image in it that has PBR maps. Returns how many
	//images failed
	int Models(const std::string& target, const ModelOptions& o);
	//target: an image (a0stg012_16.png, c0m001_0.png, or one of its maps), or a folder: every image in it (o.match: only
	//some). Returns how many images failed
	int Battle(const std::string& target, ModelOptions o);

	//--- the pieces (tests/fieldprep_model_test.cpp)
	//Island padding: a model's texture is islands (the parts its triangles use) on a background nothing uses. Small levels
	//and the GPU's filtering mix texels across an island's edge, so whatever is outside it (black, often) creeps onto the
	//model as a dark seam, more in each smaller level. Padded, what is outside an island is its edge's own colour (or
	//normal, or ORME), spread outwards until every uncovered texel has some, and again in every level.
	//What the islands cover: the albedo's alpha above 0 (the albedo's own file, else the image the game reads). A texture
	//without any see-through texel says nothing about its islands: nothing is padded.
	//covered: one per texel of level 0 (0: not covered); empty: none padded
	std::vector<uint8_t> IslandCoverage(const Image& albedo, const Image& image);
	//each level half the last (at least 1), down to 1x1, level 0 first. pad (albedo): the colour under its see-through
	//texels; covered (the others): those texels padded
	std::vector<Image> AlbedoLevels(const Image& level0, float coverage, bool pad = false);
	std::vector<Image> DataLevels(const Image& level0, const std::vector<uint8_t>& covered = {});
	std::vector<Image> NormalLevels(const Image& level0, const std::vector<uint8_t>& covered = {});
	//how much of an image passes the cut-out test (alpha >= coverage x 255)
	double Coverage(const Image& image, float coverage);
	//area-weighted (smaller) or bilinear (larger), every channel as it is
	Image ResizeImage(const Image& image, int width, int height);
	//the ORME map from its four channels (each null: plain)
	Image PackOrme(int width, int height, const Image* ao, const Image* roughness, const Image* metallic, const Image* emissive);
	//the ORMH map (battle): AO, roughness, metallic, height (each null: plain: 1, 0.7, 0, 1)
	Image PackOrmh(int width, int height, const Image* ao, const Image* roughness, const Image* metallic, const Image* heightMap);
	//an emissive map's levels: the colour averaged in linear light (not by its alpha: a glow's colour on black adds where
	//its alpha is 0), the alpha on its own. additive: the alpha made white everywhere (nothing reads it; BC7 keeps the
	//colour better)
	std::vector<Image> EmissiveLevels(const Image& level0, bool additive);
	//a name against a pattern list ("a0stg*, c0m*"): * for any letters, case blind; an empty list matches everything
	bool NameMatches(const std::string& patterns, const std::string& name);
	//"d001_0_ORME.png" -> "d001_0", "d001_0.png" -> "d001_0"; suffix: the map's ("" for the image itself)
	std::string ModelStem(const std::string& fileName, std::string* suffix = nullptr);
}
