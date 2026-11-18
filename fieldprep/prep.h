#pragma once
//What fieldprep works out from your maps. No file names or command line here (main.cpp), so the tests can call it.

#include "image_io.h"

#include "../ff8_demaster/field_layers.h"
#include "../ff8_demaster/field_pbr.h"

#include <map>
#include <string>
#include <vector>

namespace fieldprep
{
	//three channels, floats (linear light unless a name says otherwise)
	struct Rgb
	{
		int width = 0, height = 0;
		std::vector<float> v;
		bool IsValid() const { return width > 0 && height > 0 && v.size() >= static_cast<size_t>(width) * height * 3; }
		float* At(const int x, const int y) { return v.data() + (static_cast<size_t>(y) * width + x) * 3; }
		const float* At(const int x, const int y) const { return v.data() + (static_cast<size_t>(y) * width + x) * 3; }
	};

	inline float Luma(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

	Rgb SrgbToLinear(const Image& image); //colour maps
	Rgb AsStored(const Image& image); //data maps (/255, no curve)
	Image Resize(const Image& image, int width, int height); //bilinear, colour weighted by alpha
	Gray Resize(const Gray& image, int width, int height);
	//A picture covering frame `from` (at scale) put on frame `to` (fieldlayers::Frame: .map coordinates): cropped where `to`
	//is smaller, transparent black (0) where `from` doesn't reach. Nothing is resampled.
	Image Reframe(const Image& image, const fieldlayers::Frame& from, const fieldlayers::Frame& to, int scale);
	Gray Reframe(const Gray& image, const fieldlayers::Frame& from, const fieldlayers::Frame& to, int scale);
	Gray Luminance(const Rgb& image);
	void Blur(Gray& image, int radius); //box blur, three passes (about a Gaussian)
	void Blur(Rgb& image, int radius);

	//--- the field's layers

	//The opaque layers (blend 4) put together the way the game draws them: farthest tiles first (bigger z), later tiles of
	//the same z on top, each animation in its first state. layers: pupu -> image (canvas x scale)
	Image ComposeOpaque(const std::vector<fieldlayers::Tile>& tiles, const fieldlayers::Layout& layout,
		const std::map<uint32_t, const Image*>& layers, int scale);
	//the tiles ComposeOpaque puts together (opaque, each animation's first state), in the order it draws them
	std::vector<size_t> CompositeTiles(const std::vector<fieldlayers::Tile>& tiles);
	//The glow layers (blend 1 and 3): how much light they add, per pixel (luminance x coverage)
	Gray GlowLayers(const std::vector<fieldlayers::Tile>& tiles, const fieldlayers::Layout& layout,
		const std::map<uint32_t, const Image*>& layers, int scale);

	//--- which way round the depth and normal maps are

	struct Orientation
	{
		bool known = false;
		bool brightIsNear = true;
		bool normalYDown = false; //green points down (DirectX style)
		double corrX = 0, corrY = 0;
		size_t samples = 0;
	};
	//Compares the depth map's slopes with the normal map: a surface's depth changes towards where its normal leans
	Orientation DetectOrientation(const Gray& depth, const Image& normal, int scale);
	//normal map with green pointing up (what DEMASTER reads)
	Image NormalsYUp(const Image& normal, bool flipY);

	//--- depth from the field's camera and walkmesh

	struct DepthFit
	{
		bool ok = false;
		fieldpbr::Space space;
		int variant = 0;
		size_t samples = 0, inside = 0, inliers = 0;
		double medianError = 0;
		std::string report;
	};
	//Projects the walkmesh with the camera onto the canvas, reads the depth map there and fits the depth model to the real
	//distances (both models, the camera's possible screen orientations; the best wins)
	DepthFit FitDepth(const fieldpbr::Camera& camera, const fieldpbr::Walkmesh& walkmesh, const Gray& depth, int scale,
		const fieldlayers::Layout& layout);

	//--- lightmap

	struct TintSettings
	{
		float strength = 0.5f; //0: none; 1: all of the bounced light's tint moves from the albedo into the lightmap
		float gain = 2.0f; //Marigold's shading is greyer than the light: its tint, relative to the field's average, raised to this power
	};
	//How much each pixel's light is tinted (bounced light's colour relative to the field's), mean luminance 1 per pixel
	Rgb LightTint(const Rgb& bounced, int blurRadius, const TintSettings& settings);

	struct Split
	{
		Image albedo; //sRGB, alpha from coverage
		Rgb light; //linear, albedo x light = painting
		float scale = 1.0f; //the brightest light kept (what 255 in _LF means)
		size_t raised = 0; //pixels whose albedo was raised (by more than 2 steps of 255) so the light fits under the scale
		float typical = 0.0f; //the median of painting / albedo (when the scale was picked): about 1-2 when the two match
	};
	//albedo: the albedo wanted (linear; it is raised where the painting is brighter than it can hold), painting: sRGB with
	//coverage in alpha. The albedo is rounded to 8 bits first and the light worked out from that, so the two multiply back
	//to the painting. scale: 0 picks one (the light's 99.9th percentile, 1-16)
	Split SplitLight(const Image& painting, const Rgb& albedo, float scale);
	//The light again from a given albedo (BC7-decoded, say), so the pair the game loads still multiplies to the painting
	Rgb LightFrom(const Image& painting, const Image& albedo, float scale);

	//Is each painted pixel the composite's (that layer is what you see there)? A layer whose pixels all are can use the field's _LF
	size_t Mismatches(const Image& layer, const Image& composite, std::vector<uint8_t>* mask);

	//--- AO

	struct AoSettings
	{
		double radius = 10.0; //native pixels
		double strength = 1.0;
		int directions = 8, steps = 12;
		int threads = 4;
	};
	//Horizon-based AO from depth and normals (normals with green up): how open each point is, never any lighting
	Gray BakeAo(const Gray& depth, const Image& normal, const fieldpbr::Space& space, int scale, const AoSettings& settings);

	//--- lights

	//<field>_LIGHTS.png: marks painted on black, at the field's size (any scale works). Each spot of pure green (0, 255, 0)
	//is a day lamp, pure blue (0, 0, 255) a night lamp, cyan (0, 255, 255) a lamp on day and night, at the spot's middle;
	//pure red (255, 0, 0) keeps lamps, day and night, away from what it covers. Other colours are ignored
	struct LightMarks
	{
		std::vector<std::pair<double, double>> add, addNight, addBoth; //canvas, native pixels
		std::vector<uint8_t> remove; //native canvas pixels, 1: no light here (the red marks and a little round them)
		int width = 0, height = 0; //native
		size_t addPixels = 0, removePixels = 0;
		//the lamps marked for a time of day's search: its own and the both ones
		std::vector<std::pair<double, double>> Lamps(const fieldpbr::LightTime time) const
		{
			std::vector<std::pair<double, double>> out = time == fieldpbr::LightTime::Night ? addNight : add;
			out.insert(out.end(), addBoth.begin(), addBoth.end());
			return out;
		}
	};
	LightMarks ReadLightMarks(const Image& marks, int nativeWidth, int nativeHeight);

	struct LightSearch
	{
		int maxLights = fieldpbr::MAX_FOUND_LIGHTS; //the large lights kept per time of day (those reaching most of the field)
		int fitLights = 12; //searched for before the large ones are told from the small ones
		float minShare = 0.02f; //a light must explain at least this much of the light the ambient leaves unexplained
		float intensity = 0.2f; //what a found light adds on top (0: only what is painted)
		int threads = 4;
	};
	//Inverse lighting (Marschner & Greenberg 1997): the few point lights whose light (n.l / r^2 over the room, from the normal
	//and depth maps) best explains the diffuse light (the bounced light, else the lightmap: linear), on top of a smooth
	//ambient; greedy with non-negative least squares, each light then moved around to fit best. Found lights are in the
	//painting already (baked). Only the search.maxLights large ones stay (those reaching most of the field: the room's own
	//light), fitted again without the small lights' spots (those are for the emissive texture and the lights you add). They
	//add search.intensity on top and cast character shadows (shadow left out: 0.5), source = found. Lamps you mark are kept
	//even when the fit gives them nothing (source = mark).
	//time: Day (diffuse: the painting's light, _BL else the lightmap) or Night (diffuse: _RESM, the light the painting's glows
	//give off); the lights come out with that time, named "light n" / "night light n" ("your ..." for marked ones)
	fieldpbr::Lights FindLights(const Rgb& diffuse, const Image* normal, const Gray& depth, const fieldpbr::Space& space, int scale,
		const LightMarks* marks, const LightSearch& search, std::string& report, fieldpbr::LightTime time = fieldpbr::LightTime::Day);
	//The day and night lights in one file: a night light where a day light is (within 10 native pixels on the canvas and
	//15% of its distance) is the same lamp, on day and night (time = both; on if it is on in either). Returns how many were
	size_t MergeDayNight(fieldpbr::Lights& day, const fieldpbr::Lights& night, const fieldpbr::Space& space);
	//Lights found again keep the lights you wrote into the old file yourself (source = yours, or no source in a file that
	//has sources), after fieldprep's. A file without any source (before v15.7) was all fieldprep's: none kept. Returns how
	//many were kept
	size_t KeepYourLights(fieldpbr::Lights& lights, const fieldpbr::Lights& old, bool oldHasSources);

	//The part of each pixel's painted light that the nearest flickering light (baked, flicker on) gives it: the light above the
	//field's ambient level, faded out by the light's range and by surfaces facing away
	Gray FlickerMask(const fieldpbr::Lights& lights, const Rgb& light, const Gray& depth, const Image& normal,
		const fieldpbr::Space& space, int scale);

	//--- packing

	//_LF: lightmap (sRGB curve, / scale) and steady mask (255 - flicker mask)
	Image PackLightmap(const Rgb& light, float scale, const Gray* mask);
	//_ND: normal (green up) and depth
	Image PackNormalDepth(const Image& normal, const Gray& depth);
	//_ORME: AO, roughness, metallic, emissive; a missing one gets its default (1, 0.8, 0, 0)
	Image PackOrme(int width, int height, const Gray* ao, const Gray* roughness, const Gray* metallic, const Gray* emissive);
	//The emissive map from the non-diffuse residual (_RESM, linear) and the painting (linear): how much of the painting at
	//each pixel is its own glow. 1 where the residual is most of the painting (a lamp's bulb, a lit screen or sign), 0 where
	//it is a small part of it (a sheen, the light a lamp throws on a wall: the field's lights give that) or too dark to tell.
	//share: the part of the painting the residual has to be for a glow to start; full glow at share + 0.4
	Gray EmissiveFromResidual(const Rgb& painting, const Rgb& residual, float share = 0.35f);
	//coverage from another image's alpha
	void CopyAlpha(Image& image, const Image& from);
	//Colour under pixels the coverage leaves out (never drawn) spread from their covered neighbours, a few pixels out, so
	//BC7's 4x4 blocks along the edges don't have to hold a jump to black. alpha: what those pixels get in alpha (-1: kept)
	void PadUncovered(Image& image, const Image& coverage, int rings, int alpha);

	//--- for the characters (FIELD_PBR_DEPTH, FIELD_PBR_CHARACTERS)

	//Tiles in front of the base picture (opaque, Z < 4094): per Z, the view distance of the nearest part of what they show
	//(5th percentile of the depth map under their painted pixels). gameDepth: the median of 16 Z / that distance, what the
	//game's depth is per view unit if the depth map is right (16384 with a measured depth).
	struct Occlusion
	{
		std::map<int, double> nearest;
		std::map<int, size_t> pixels;
		double gameDepth = 0;
		std::string report;
	};
	Occlusion MeasureOcclusion(const std::vector<fieldlayers::Tile>& tiles, const fieldlayers::Layout& layout,
		const std::map<uint32_t, const Image*>& layers, const Gray& depth, const fieldpbr::Space& space, int scale);

	//The world's up as the camera sees it: the walkmesh's floor, facing up on screen
	fieldpbr::Vec3 WorldUp(const fieldpbr::CameraTransform& camera, const fieldpbr::Walkmesh& walkmesh);

	struct ProbeSettings
	{
		int cell = 16; //native pixels across per probe
		int slices = 8; //depth slices
		int directions = 128; //rays per probe
		int threads = 4;
	};
	//The room's light at a box of points in front of the camera (fieldpbr::ProbeGrid): rays from each point across the depth
	//map, the painting where they land (linear). Points behind the room's surface take the light of the nearest one in front.
	fieldpbr::ProbeGrid BakeProbes(const Rgb& painting, const Gray& depth, const Gray* flickerMask, const fieldpbr::Space& space,
		int scale, const ProbeSettings& settings, double& zNear, double& zFar, std::string& report);

	//--- the check image: the composite, the walkmesh as the camera sees it, the lights and their range
	Image CheckImage(const Image& composite, const fieldpbr::Camera* camera, const fieldpbr::Walkmesh* walkmesh, int variant,
		const fieldpbr::Space& space, const fieldpbr::Lights& lights, int scale);
}
