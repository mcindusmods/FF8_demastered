//fieldprep: gets a field's PBR maps ready for DEMASTER (FIELD_PBR). Preparation only: DEMASTER loads what this writes;
//nothing here runs in the game. The rules both agree on are in ff8_demaster/field_pbr.h.
//
//	fieldprep [prep|orme|all|compose] <folder> [options]
//
//See usage() below and README_field_pbr.md.

#include "archive.h"
#include "image_io.h"
#include "prep.h"

#include "../ff8_demaster/field_layers.h"
#include "../ff8_demaster/field_pbr.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

using namespace fieldprep;
namespace fs = std::filesystem;

namespace
{
	struct Options
	{
		std::string command = "prep";
		std::string target;
		std::string data; //where field.fl/fi/fs are ("" = find DEMASTER_EXP\data)
		bool dds = true;
		bool relight = false, rebake = false, force = false;
		TintSettings tint;
		AoSettings ao;
		LightSearch lights;
		float emissiveShare = 0.35f; //orme: the residual's part of the painting where a glow starts
		int threads = 4;
	};

	//--- log: the console and <field>_fieldprep.txt
	std::string logText;
	void Say(const char* format, ...)
	{
		char text[2048];
		va_list args;
		va_start(args, format);
		vsnprintf(text, sizeof(text), format, args);
		va_end(args);
		fputs(text, stdout);
		fflush(stdout);
		logText += text;
	}

	bool WriteText(const std::string& path, const std::string& text)
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out)
			return false;
		//Windows line ends, so Notepad shows it right
		std::string crlf;
		for (const char c : text)
		{
			if (c == '\n')
				crlf += '\r';
			crlf += c;
		}
		out << crlf;
		return static_cast<bool>(out);
	}

	bool ReadText(const std::string& path, std::string& text)
	{
		std::vector<uint8_t> bytes;
		if (!ReadFile(path, bytes))
			return false;
		text.assign(bytes.begin(), bytes.end());
		return true;
	}

	std::string Lower(std::string s)
	{
		std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) { return static_cast<char>(tolower(c)); });
		return s;
	}

	std::string Name(const std::string& path)
	{
		return fs::path(path).filename().string();
	}

	//--- writing maps: <stem>.png, then <stem>.dds (BC7) so the DDS is the newer one DEMASTER picks
	bool WriteMap(const std::string& stem, const Image& png, const Image* forDds, const bool perceptual, const Options& o)
	{
		std::string error;
		if (!SavePng(stem + ".png", png, error))
		{
			Say("  ERROR writing %s.png: %s\n", Name(stem).c_str(), error.c_str());
			return false;
		}
		if (!o.dds)
			return true;
		const Image& source = forDds != nullptr ? *forDds : png;
		if (source.width % 4 || source.height % 4)
		{
			Say("  %s: %dx%d is not a multiple of 4, no DDS\n", Name(stem).c_str(), source.width, source.height);
			return true;
		}
		const std::vector<uint8_t> blocks = EncodeBc7(source, perceptual, o.threads);
		if (!SaveDds(stem + ".dds", source.width, source.height, blocks, error))
		{
			Say("  ERROR writing %s.dds: %s\n", Name(stem).c_str(), error.c_str());
			return false;
		}
		return true;
	}

	//Where a map goes: onto `target` at the field's scale. A file made for one of the other frames the field knows (FME's
	//canvas, a layer picture's own frame) is cropped or padded into place by its exact offset, not stretched; a file at
	//another scale is resized; a file that fits no frame is resized with a warning to check that it lines up.
	struct Framing
	{
		fieldlayers::Frame target;
		std::vector<std::pair<fieldlayers::Frame, std::string>> others; //frame, what it is ("FME's canvas")
		int scale = 1;
		int Width() const { return target.width * scale; }
		int Height() const { return target.height * scale; }
	};

	template <typename T>
	void FitToFraming(T& image, const Framing& framing, const std::string& name)
	{
		if (image.width == framing.Width() && image.height == framing.Height())
			return;
		const int width = image.width, height = image.height;
		if (const int s = framing.target.ScaleOf(width, height))
		{
			image = Resize(image, framing.Width(), framing.Height());
			Say("  %s is %dx%d (at %dx): resized to %dx%d\n", name.c_str(), width, height, s, framing.Width(), framing.Height());
			return;
		}
		for (const auto& [frame, what] : framing.others)
			if (const int s = frame.ScaleOf(width, height); s != 0 && frame != framing.target)
			{
				image = Reframe(image, frame, framing.target, s);
				if (s != framing.scale)
					image = Resize(image, framing.Width(), framing.Height());
				Say("  %s is %dx%d: made for %s, moved into place (%dx%d)\n", name.c_str(), width, height, what.c_str(), framing.Width(),
					framing.Height());
				return;
			}
		image = Resize(image, framing.Width(), framing.Height());
		const bool sameShape = std::abs(static_cast<double>(width) / height - static_cast<double>(framing.Width()) / framing.Height()) < 0.01;
		Say("  %s is %dx%d: resized to %dx%d%s\n", name.c_str(), width, height, framing.Width(), framing.Height(),
			sameShape ? "" : " (its shape fits neither the canvas nor FME's: check that it lines up)");
	}

	//an optional input map, fitted to its framing. names: suffixes to try in order
	bool LoadInput(const std::string& stem, const std::vector<std::string>& suffixes, const Framing& framing, Image& out,
		std::string& used)
	{
		for (const std::string& suffix : suffixes)
		{
			const std::string path = NewestImage(stem + suffix);
			if (path.empty())
				continue;
			std::string error;
			if (!LoadRgba(path, out, error))
			{
				Say("  can't read %s: %s\n", Name(path).c_str(), error.c_str());
				continue;
			}
			FitToFraming(out, framing, Name(path));
			used = Name(path);
			return true;
		}
		return false;
	}

	bool LoadGrayInput(const std::string& stem, const std::vector<std::string>& suffixes, const Framing& framing, Gray& out,
		std::string& used)
	{
		for (const std::string& suffix : suffixes)
		{
			std::string path = stem + suffix + ".png";
			if (!FileExists(path))
				path = NewestImage(stem + suffix);
			if (path.empty())
				continue;
			std::string error;
			if (!LoadGray(path, out, error))
			{
				Say("  can't read %s: %s\n", Name(path).c_str(), error.c_str());
				continue;
			}
			FitToFraming(out, framing, Name(path));
			used = Name(path) + (out.sixteen ? " (16-bit)" : " (8-bit)");
			return true;
		}
		return false;
	}

	//DEMASTER_EXP\data (unpacked by AUTO_UNPACK=1), else the game's main.zzz, found from a folder under DEMASTER_EXP\textures
	std::string FindDataFolder(const std::string& folder)
	{
		std::error_code error;
		fs::path p = fs::absolute(folder, error);
		for (int up = 0; up < 8 && !p.empty(); up++)
		{
			if (Lower(p.filename().string()) == "textures")
			{
				const fs::path data = p.parent_path() / "data";
				if (fs::is_directory(data, error))
				{
					for (const auto& entry : fs::directory_iterator(data, error))
						if (Lower(entry.path().filename().string()) == "field.fl")
							return data.string();
					for (const auto& sub : fs::directory_iterator(data, error))
						if (sub.is_directory())
							for (const auto& entry : fs::directory_iterator(sub.path(), error))
								if (Lower(entry.path().filename().string()) == "field.fl")
									return sub.path().string();
				}
				const fs::path game = p.parent_path().parent_path();
				for (const auto& entry : fs::directory_iterator(game, error))
					if (Lower(entry.path().filename().string()) == "main.zzz")
						return entry.path().string();
				return {};
			}
			if (p == p.parent_path())
				break;
			p = p.parent_path();
		}
		return {};
	}

	//--- one field (one .map: the plain one or a language's)
	struct FieldInput
	{
		std::string folder, name, language, stem;
		std::vector<fieldlayers::Tile> tiles;
		fieldlayers::Layout layout; //on the canvas: the box around the opaque layers' pictures (FME's canvas for FME's files)
		fieldlayers::Layout fme; //FME's canvas and names
		std::map<uint32_t, fieldlayers::Frame> pictureFrame; //per layer picture: the frame its file covers
		std::vector<fieldlayers::Frame> opaqueFrames; //the frames of the opaque layers' pictures (each size once: a map made
		//for one of them, its main layer's say, is moved into place; two frames of one size can't be told apart: neither)
		bool haveCamera = false;
		fieldpbr::Camera camera;
		fieldpbr::Walkmesh walkmesh;
		std::map<uint32_t, Image> layers; //paintings
		std::map<uint32_t, std::string> layerPath;
		std::map<uint32_t, std::string> layerStem; //folder + <field>[_<language>]_<pupu>, as the painting is named
		int scale = 0;
		Image composite;
	};

	//maps of the whole field: on the canvas; ones made for FME's canvas are moved into place
	Framing CanvasFraming(const FieldInput& f)
	{
		Framing framing;
		framing.target = f.layout.canvas;
		framing.scale = std::max(1, f.scale);
		if (f.layout.fme != f.layout.canvas)
			framing.others.push_back({ f.layout.fme, "FME's canvas" });
		for (const fieldlayers::Frame& frame : f.opaqueFrames)
			framing.others.push_back({ frame, "one layer's frame (" + std::to_string(frame.width) + "x" + std::to_string(frame.height) + ")" });
		return framing;
	}

	//a layer's own maps: read onto the canvas too (the work is done there); made for its picture's frame or FME's canvas
	Framing LayerFraming(const FieldInput& f, const uint32_t pupu)
	{
		Framing framing = CanvasFraming(f);
		if (const auto it = f.pictureFrame.find(pupu); it != f.pictureFrame.end() && it->second != f.layout.canvas)
			framing.others.insert(framing.others.begin(), { it->second, "its picture's frame" });
		return framing;
	}

	//a layer's own output: written in its picture's frame (the size of its painting)
	Image ToPicture(const FieldInput& f, const uint32_t pupu, const Image& onCanvas)
	{
		const auto it = f.pictureFrame.find(pupu);
		return it == f.pictureFrame.end() ? onCanvas : Reframe(onCanvas, f.layout.canvas, it->second, f.scale);
	}
	bool WriteLayerMap(const FieldInput& f, const uint32_t pupu, const std::string& stem, const Image& png, const Image* forDds,
		const bool perceptual, const Options& o)
	{
		const Image dds = forDds != nullptr ? ToPicture(f, pupu, *forDds) : Image();
		return WriteMap(stem, ToPicture(f, pupu, png), forDds != nullptr ? &dds : nullptr, perceptual, o);
	}

	bool LoadField(FieldInput& f, const Options& o)
	{
		const std::string folder = f.folder;
		//the .map, camera and walkmesh: next to the images, else from the game's archive
		std::vector<uint8_t> map, ca, id;
		std::string from;
		const std::string mapStem = f.language.empty() ? f.name : f.name + "_" + f.language;
		for (const std::string& candidate : { mapStem, f.name })
			if (ReadFile(folder + candidate + ".map", map))
			{
				from = candidate + ".map in the folder";
				break;
			}
		ReadFile(folder + f.name + ".ca", ca);
		ReadFile(folder + f.name + ".id", id);
		if (map.empty() || ca.empty() || id.empty())
		{
			const std::string data = o.data.empty() ? FindDataFolder(folder) : o.data;
			if (!data.empty())
			{
				FieldFiles files;
				std::string error;
				const bool found = LoadFromArchive(data, f.name, f.language, files, error);
				if (found && map.empty())
				{
					map = files.map;
					from = files.mapName + " from " + data;
					if (!files.note.empty())
						Say("  %s\n", files.note.c_str());
				}
				else if (!found && map.empty())
					Say("  %s\n", error.c_str());
				if (ca.empty())
					ca = files.ca;
				if (id.empty())
					id = files.id;
			}
		}
		if (map.empty())
		{
			Say("  ERROR: no %s.map. fieldprep reads it from DEMASTER_EXP\\data\\field.fs or the game's main.zzz (found from the folder's\n"
				"  place under DEMASTER_EXP\\textures), or from %s.map next to the images. --data <folder or main.zzz> says where.\n", mapStem.c_str(), mapStem.c_str());
			return false;
		}
		f.tiles = fieldlayers::ParseMap(map.data(), map.size());
		if (f.tiles.empty())
		{
			Say("  ERROR: the .map has no tiles\n");
			return false;
		}
		f.fme = fieldlayers::ComputeLayout(f.tiles);
		f.layout = f.fme;
		const std::vector<fieldpbr::Camera> cameras = fieldpbr::ParseCameras(ca.data(), ca.size());
		f.walkmesh = fieldpbr::ParseWalkmesh(id.data(), id.size());
		if (!cameras.empty() && cameras[0].zoom != 0 && !f.walkmesh.triangles.empty())
		{
			f.haveCamera = true;
			f.camera = cameras[0];
		}
		Say("  %s: %zu tiles, FME's canvas %dx%d; camera %s, walkmesh %zu triangles\n", from.c_str(), f.tiles.size(), f.fme.width,
			f.fme.height, cameras.empty() ? "none" : ("zoom " + std::to_string(cameras[0].zoom)).c_str(), f.walkmesh.triangles.size());
		//the paintings: per tile, its PuPu-named file in PuPu's frame for it (PuPu gives each layer number its own frame), else
		//its FME-named file on FME's canvas; the file's size says which (every file read once)
		const fieldlayers::PupuNames pupuNames = fieldlayers::ComputePupuNames(f.tiles);
		std::map<uint32_t, Image> loaded;
		std::map<uint32_t, std::string> loadedPath, loadedStem;
		const auto size = [&](const uint32_t pupu, int& width, int& height)
		{
			auto it = loaded.find(pupu);
			if (it == loaded.end())
			{
				std::string path, stem;
				if (!f.language.empty())
				{
					stem = fieldpbr::LayerStem(folder, f.name, f.language, pupu);
					path = NewestImage(stem);
				}
				if (path.empty())
				{
					stem = fieldpbr::LayerStem(folder, f.name, "", pupu);
					path = NewestImage(stem);
				}
				Image image;
				std::string error;
				if (!path.empty() && !LoadRgba(path, image, error))
					Say("  can't read %s: %s\n", Name(path).c_str(), error.c_str());
				it = loaded.emplace(pupu, std::move(image)).first;
				loadedPath[pupu] = path;
				loadedStem[pupu] = stem;
			}
			width = it->second.width;
			height = it->second.height;
			return it->second.IsValid();
		};
		fieldlayers::PictureSet set = fieldlayers::FindPictures(f.tiles, f.fme, pupuNames, size);
		for (const auto& [pupu, why] : set.unfit)
			Say("  %s is %s: skipped\n", Name(loadedPath[pupu]).c_str(), why.c_str());
		//one scale for the field: the one most tiles' pictures have
		std::map<int, size_t> scales;
		for (const auto& [pupu, picture] : set.pictures)
			scales[picture.scale] += picture.tiles;
		size_t most = 0;
		for (const auto& [scale, tiles] : scales)
			if (tiles > most)
			{
				most = tiles;
				f.scale = scale;
			}
		for (auto it = set.pictures.begin(); it != set.pictures.end();)
			if (it->second.scale != f.scale)
			{
				Say("  %s is at %dx, the others at %dx: skipped\n", Name(loadedPath[it->first]).c_str(), it->second.scale, f.scale);
				for (int64_t& t : set.tile)
					if (t == static_cast<int64_t>(it->first))
						t = -1;
				it = set.pictures.erase(it);
			}
			else
				++it;
		if (set.pictures.empty())
		{
			Say("  ERROR: no layer images for this .map (%s_<pupu>.png, named by FME or by PuPu: %s_%s.png or %s_%s.png)\n",
				f.stem.c_str(), f.stem.c_str(), fieldlayers::PupuHex(f.fme.pupu[0]).c_str(), f.stem.c_str(),
				fieldlayers::PupuHex(pupuNames.name[0]).c_str());
			return false;
		}
		//the canvas: the box around the opaque layers' pictures
		const fieldlayers::Frame canvas = fieldlayers::FieldFrame(f.tiles, set, f.fme.fme);
		f.layout = fieldlayers::FramedLayout(f.tiles, f.fme, set, canvas);
		size_t pupuPictures = 0, tilesWithout = 0;
		for (const auto& [pupu, picture] : set.pictures)
		{
			f.layers[pupu] = Reframe(loaded[pupu], picture.frame, canvas, f.scale);
			f.layerPath[pupu] = loadedPath[pupu];
			f.layerStem[pupu] = loadedStem[pupu];
			f.pictureFrame[pupu] = picture.frame;
			pupuPictures += picture.pupu;
		}
		for (const int64_t t : set.tile)
			tilesWithout += t < 0;
		std::map<std::pair<int, int>, std::vector<fieldlayers::Frame>> bySize;
		for (size_t i = 0; i < f.tiles.size(); i++)
			if (f.tiles[i].blend == 4 && set.tile[i] >= 0)
			{
				const fieldlayers::Frame& frame = set.pictures[static_cast<uint32_t>(set.tile[i])].frame;
				std::vector<fieldlayers::Frame>& list = bySize[{ frame.width, frame.height }];
				if (frame != canvas && frame != f.fme.fme && std::find(list.begin(), list.end(), frame) == list.end())
					list.push_back(frame);
			}
		for (const auto& [size, list] : bySize)
			if (list.size() == 1)
				f.opaqueFrames.push_back(list[0]);
		Say("  %zu layer images at %dx: %zu named and framed the PuPu way, %zu FME's%s\n", f.layers.size(), f.scale, pupuPictures,
			f.layers.size() - pupuPictures, tilesWithout ? (", " + std::to_string(tilesWithout) + " tile(s) without one").c_str() : "");
		Say("  canvas (your maps, the lights and the camera are on it): %dx%d (%dx%d at %dx)%s\n", canvas.width, canvas.height,
			canvas.width * f.scale, canvas.height * f.scale, f.scale,
			canvas == f.fme.fme ? ", FME's" : ", the box around the opaque layers' pictures");
		return true;
	}

	//<field>.png: the opaque layers put together (yours if it's there)
	bool LoadComposite(FieldInput& f, const Options& o, const bool write)
	{
		const std::string path = NewestImage(f.folder + f.stem);
		std::string error;
		if (!path.empty() && !(write && o.force))
		{
			if (LoadRgba(path, f.composite, error))
			{
				FitToFraming(f.composite, CanvasFraming(f), Name(path));
				for (size_t i = 3; i < f.composite.rgba.size(); i += 4)
					f.composite.rgba[i] = 255;
				Say("  the whole field: %s (yours)\n", Name(path).c_str());
				return true;
			}
			Say("  can't read %s: %s\n", Name(path).c_str(), error.c_str());
		}
		std::map<uint32_t, const Image*> layers;
		for (const auto& [pupu, image] : f.layers)
			layers[pupu] = &image;
		f.composite = ComposeOpaque(f.tiles, f.layout, layers, f.scale);
		size_t holes = 0;
		for (size_t i = 3; i < f.composite.rgba.size(); i += 4)
			holes += f.composite.rgba[i] == 0;
		if (!SavePng(f.folder + f.stem + ".png", f.composite, error))
			Say("  ERROR writing %s.png: %s\n", f.stem.c_str(), error.c_str());
		else
			Say("  the whole field: %s.png, put together from the opaque layers (first animation states)%s\n", f.stem.c_str(),
				holes ? (", " + std::to_string(holes) + " pixels nothing covers").c_str() : "");
		for (size_t i = 3; i < f.composite.rgba.size(); i += 4)
			f.composite.rgba[i] = 255;
		return true;
	}

	//Are the bounced light and the residual stored linear, or with the sRGB curve? The painting is about albedo x bounced +
	//residual in linear light under only one of the two readings.
	bool LightingIsSrgb(const Rgb& painting, const Rgb& albedo, const Image& bounced, const Image* residual)
	{
		const float* table = fieldpbr::SrgbTable();
		double errorLinear = 0, errorSrgb = 0;
		const size_t n = static_cast<size_t>(painting.width) * painting.height;
		for (size_t i = 0; i < n; i += 5)
			for (int c = 0; c < 3; c++)
			{
				const uint8_t b = bounced.rgba[i * 4 + c], r = residual != nullptr ? residual->rgba[i * 4 + c] : 0;
				const double p = painting.v[i * 3 + c], a = albedo.v[i * 3 + c];
				errorLinear += std::fabs(p - (a * (b / 255.0) + r / 255.0));
				errorSrgb += std::fabs(p - (a * table[b] + table[r]));
			}
		return errorSrgb < errorLinear;
	}

	void Prep(FieldInput& f, const Options& o)
	{
		const int width = f.composite.width, height = f.composite.height, scale = f.scale;
		const std::string stem = f.folder + f.stem;
		std::string used;
		//albedo
		Image albedoInput;
		if (!LoadInput(stem, { "_AM" }, CanvasFraming(f), albedoInput, used))
		{
			if (LoadInput(stem, { "_A" }, CanvasFraming(f), albedoInput, used))
				Say("  albedo: %s (fieldprep reads %s_AM; %s_A is used as long as there is no _AM)\n", used.c_str(), f.stem.c_str(), f.stem.c_str());
			else
			{
				Say("  ERROR: no albedo (%s_AM.png): nothing to make\n", f.stem.c_str());
				return;
			}
		}
		else
			Say("  albedo: %s\n", used.c_str());
		Image normalInput, bouncedInput, residualInput;
		Gray depth;
		const bool haveNormal = LoadInput(stem, { "_N" }, CanvasFraming(f), normalInput, used);
		if (haveNormal)
			Say("  normal: %s\n", used.c_str());
		const bool haveDepth = LoadGrayInput(stem, { "_DEPTH" }, CanvasFraming(f), depth, used);
		if (haveDepth)
			Say("  depth: %s\n", used.c_str());
		else
		{
			depth.width = width;
			depth.height = height;
			depth.v.assign(static_cast<size_t>(width) * height, 0.5f);
			Say("  no %s_DEPTH: every pixel at the same depth\n", f.stem.c_str());
		}
		const bool haveBounced = LoadInput(stem, { "_BL" }, CanvasFraming(f), bouncedInput, used);
		if (haveBounced)
			Say("  bounced light: %s\n", used.c_str());
		const bool haveResidual = LoadInput(stem, { "_RESM" }, CanvasFraming(f), residualInput, used);
		if (haveResidual)
			Say("  residual: %s\n", used.c_str());

		//which way round depth and normals are
		Orientation orientation;
		if (haveNormal && haveDepth)
		{
			orientation = DetectOrientation(depth, normalInput, scale);
			Say("  depth against normals (%zu samples): x %.2f, y %.2f -> %s, normal green points %s\n", orientation.samples,
				orientation.corrX, orientation.corrY, orientation.known ? (orientation.brightIsNear ? "bright is near" : "bright is far") : "can't tell (bright is near assumed)",
				orientation.normalYDown ? "down: flipped" : "up");
		}
		//where things are
		fieldpbr::Settings settings;
		settings.brightIsNear = orientation.brightIsNear;
		settings.normalYDown = orientation.normalYDown;
		settings.space = fieldpbr::EstimatedSpace(f.layout.shiftX, f.layout.shiftY, orientation.brightIsNear);
		//the canvas for DEMASTER: what _LF, _ND, _ORME, the lights and the origin are on
		settings.hasCanvas = true;
		settings.canvas[0] = f.layout.canvas.left;
		settings.canvas[1] = f.layout.canvas.top;
		settings.canvas[2] = f.layout.canvas.width;
		settings.canvas[3] = f.layout.canvas.height;
		int cameraVariant = 0;
		if (haveDepth && f.haveCamera)
		{
			const DepthFit fit = FitDepth(f.camera, f.walkmesh, depth, scale, f.layout);
			Say("  depth from the camera and walkmesh: %s", fit.report.c_str());
			cameraVariant = fit.variant;
			if (fit.ok)
			{
				settings.space = fit.space;
				const bool near = fit.space.model == fieldpbr::DepthModel::Linear ? fit.space.b < 0 : fit.space.b > 0;
				if (orientation.known && near != orientation.brightIsNear)
					Say("  note: the walkmesh says bright is %s, the normals said the opposite: going with the walkmesh\n", near ? "near" : "far");
				settings.brightIsNear = near;
			}
			else
				Say("  depth: estimated (the walkmesh didn't fit well enough)\n");
		}
		else
			Say("  depth: estimated (%s)\n", haveDepth ? "no camera or walkmesh" : "no depth map");
		const Image normal = haveNormal ? NormalsYUp(normalInput, orientation.normalYDown) : Image();

		//the light's tint, from the bounced light
		const Rgb painting = SrgbToLinear(f.composite);
		const Rgb albedoLinear = SrgbToLinear(albedoInput);
		Rgb tint;
		Rgb bounced, residual;
		if (haveBounced)
		{
			const bool srgb = LightingIsSrgb(painting, albedoLinear, bouncedInput, haveResidual ? &residualInput : nullptr);
			Say("  bounced light and residual read as %s (the painting fits albedo x bounced + residual better that way)\n",
				srgb ? "sRGB" : "linear");
			bounced = srgb ? SrgbToLinear(bouncedInput) : AsStored(bouncedInput);
			if (haveResidual)
				residual = srgb ? SrgbToLinear(residualInput) : AsStored(residualInput);
			if (o.tint.strength > 0)
				tint = LightTint(bounced, 2 * scale, o.tint);
		}
		else if (haveResidual)
			residual = AsStored(residualInput);
		Rgb wanted = albedoLinear;
		if (tint.IsValid())
			for (size_t i = 0; i < wanted.v.size(); i++)
				wanted.v[i] /= tint.v[i];

		//the field's lightmap
		Split field = SplitLight(f.composite, wanted, 0);
		settings.lightmapScale = field.scale;
		Say("  lightmap: up to %.2f x the albedo%s%s\n", field.scale, tint.IsValid() ? ", the bounced light's tint moved into it" : "",
			field.raised ? (", albedo raised in " + std::to_string(field.raised) + " pixels so the painting fits").c_str() : "");
		Image fieldAlbedoDecoded;
		if (o.dds)
			fieldAlbedoDecoded = DecodeBc7(EncodeBc7(field.albedo, true, o.threads), width, height);

		//AO (baked once; yours if you've changed it)
		const std::string aoPath = stem + "_O.png";
		if (haveDepth && haveNormal && (o.rebake || !FileExists(aoPath)))
		{
			const auto start = std::chrono::steady_clock::now();
			AoSettings ao = o.ao;
			ao.threads = o.threads;
			const Gray baked = BakeAo(depth, normal, settings.space, scale, ao);
			std::string error;
			if (SaveGrayPng(aoPath, baked, false, error))
				Say("  AO baked from depth and normals: %s_O.png (%.1f s)\n", f.stem.c_str(),
					std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
			else
				Say("  ERROR writing %s_O.png: %s\n", f.stem.c_str(), error.c_str());
		}
		else if (FileExists(aoPath))
			Say("  AO: %s_O.png kept (--rebake makes it again)\n", f.stem.c_str());

		//lights: found again with --relight, when there is no _lights.txt yet, or when your marks (_LIGHTS.png) are newer
		const std::string lightsPath = stem + fieldpbr::LIGHTS_SUFFIX;
		const std::string marksPath = NewestImage(stem + "_LIGHTS");
		fieldpbr::Lights lights;
		std::string text, warnings;
		const bool haveLights = ReadText(lightsPath, text);
		std::error_code timeError;
		const bool marksNewer = haveLights && !marksPath.empty()
			&& std::filesystem::last_write_time(marksPath, timeError) > std::filesystem::last_write_time(lightsPath, timeError) && !timeError;
		if (!o.relight && haveLights && !marksNewer)
		{
			fieldpbr::ParseLights(text, lights, warnings);
			Say("  lights: %s kept, %zu light(s)%s\n", Name(lightsPath).c_str(), lights.lights.size(), warnings.empty() ? "" : ":");
			if (!warnings.empty())
				Say("%s", warnings.c_str());
		}
		else
		{
			if (haveLights)
			{
				//what you had stays next to it
				const std::string old = stem + "_lights_old.txt";
				if (WriteText(old, text))
					Say("  lights: found again (%s); the old %s is kept as %s\n", o.relight ? "--relight" : "your _LIGHTS marks are newer",
						Name(lightsPath).c_str(), Name(old).c_str());
			}
			LightMarks marks;
			bool haveMarks = false;
			if (!marksPath.empty())
			{
				Image marksImage;
				std::string error;
				if (LoadRgba(marksPath, marksImage, error))
				{
					FitToFraming(marksImage, CanvasFraming(f), Name(marksPath));
					marks = ReadLightMarks(marksImage, width / scale, height / scale);
					haveMarks = true;
					Say("  your light marks: %s: %zu day lamp(s) (green), %zu night lamp(s) (blue), %zu day and night (cyan), %s\n",
						Name(marksPath).c_str(), marks.add.size(), marks.addNight.size(), marks.addBoth.size(),
						marks.removePixels ? "some places kept free of lights (red)" : "nothing kept free (no red)");
				}
				else
					Say("  can't read %s: %s\n", Name(marksPath).c_str(), error.c_str());
			}
			const auto start = std::chrono::steady_clock::now();
			LightSearch search = o.lights;
			search.threads = o.threads;
			std::string report;
			//the day's lamps from the painting's light, the night's from what its glows give off (_RESM)
			lights = FindLights(haveBounced ? bounced : field.light, haveNormal ? &normal : nullptr, depth, settings.space, scale,
				haveMarks ? &marks : nullptr, search, report, fieldpbr::LightTime::Day);
			Say("  %s", report.c_str());
			Say("    (%.1f s, from the %s)\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
				haveBounced ? "bounced light" : "lightmap");
			if (haveResidual)
			{
				const auto nightStart = std::chrono::steady_clock::now();
				std::string nightReport;
				const fieldpbr::Lights night = FindLights(residual, haveNormal ? &normal : nullptr, depth, settings.space, scale,
					haveMarks ? &marks : nullptr, search, nightReport, fieldpbr::LightTime::Night);
				Say("  %s", nightReport.c_str());
				const size_t both = MergeDayNight(lights, night, settings.space);
				Say("    (%.1f s, from the residual) %zu lamp(s) on day and night, %zu by night only\n",
					std::chrono::duration<double>(std::chrono::steady_clock::now() - nightStart).count(), both, night.lights.size() - both);
			}
			else
				Say("  night lights: no %s_RESM, none found (blue marks need it too)\n", f.stem.c_str());
			//the lights you wrote into the old file yourself stay
			if (haveLights)
			{
				fieldpbr::Lights old;
				std::string oldWarnings;
				fieldpbr::ParseLights(text, old, oldWarnings);
				const bool hasSources = text.find("\nsource") != std::string::npos || text.rfind("source", 0) == 0;
				const size_t kept = KeepYourLights(lights, old, hasSources);
				if (kept > 0)
					Say("  lights: your own %zu light(s) from the old file kept\n", kept);
				if (old.hasEmissive)
				{
					lights.emissive = old.emissive;
					lights.hasEmissive = true;
					Say("  lights: the old file's [emissive] settings kept\n");
				}
				if (old.hasNight)
				{
					lights.night = old.night;
					lights.hasNight = true;
					Say("  lights: the old file's [night] settings kept\n");
				}
				else if (!hasSources && !old.lights.empty())
					Say("  lights: the old file is from before fieldprep marked whose lights are whose: all of it was taken as fieldprep's "
						"(see %s_lights_old.txt for anything you added)\n", f.stem.c_str());
			}
			const std::string written = fieldpbr::WriteLights(lights, f.stem, "Lights fieldprep found by inverse lighting: the large lamps whose "
				"light best explains the painting's (the\nday) and _RESM's glows (the night), at most " + std::to_string(fieldpbr::MAX_FOUND_LIGHTS)
				+ " each; the same lamp in both is on day and night (time = both).\nSmall lights are for the emissive texture and for your own: "
				"add up to " + std::to_string(fieldpbr::MAX_YOUR_LIGHTS) + " per time of day here (source =\nyours, or leave source out; "
				"fieldprep keeps them when it finds the lights again) or mark them in " + f.stem + "_LIGHTS.png\n(on black: green a day lamp, "
				"blue a night lamp, cyan both, red keeps lamps away). Day and night lights cross-fade with\nDEMASTER's day/night clock. None flicker yet: set flicker on the ones "
				"that should, then run fieldprep again (the flicker mask goes into _LF).");
			if (WriteText(lightsPath, written))
				Say("  lights: %s written, %zu light(s)\n", Name(lightsPath).c_str(), lights.lights.size());
			//what DEMASTER will say about it (more of your own lights than it uses at a time, ...)
			{
				fieldpbr::Lights check;
				std::string checkWarnings;
				fieldpbr::ParseLights(written, check, checkWarnings);
				for (size_t at = 0, end; at < checkWarnings.size(); at = end + 1)
				{
					end = checkWarnings.find('\n', at);
					if (end == std::string::npos)
						end = checkWarnings.size();
					Say("  lights: %s\n", checkWarnings.substr(at, end - at).c_str());
				}
			}
		}
		//the day's lights for the flicker mask and the light grid: the painting is the day (at night DEMASTER dims it and keeps
		//the painted light around the night's lamps)
		fieldpbr::Lights dayLights = lights;
		fieldpbr::KeepDayLights(dayLights);
		const Gray mask = FlickerMask(dayLights, field.light, depth, normal, settings.space, scale);
		size_t flickering = 0;
		for (const fieldpbr::Light& l : dayLights.lights)
			flickering += l.OwnsMask();

		//for the characters: where the foreground tiles really are, the game's depth scale, the camera, the sky, the light grid
		if (haveDepth)
		{
			std::map<uint32_t, const Image*> layerImages;
			for (const auto& [pupu, image] : f.layers)
				layerImages[pupu] = &image;
			const Occlusion occlusion = MeasureOcclusion(f.tiles, f.layout, layerImages, depth, settings.space, scale);
			settings.occlusion = occlusion.nearest;
			if (settings.space.measured)
			{
				settings.gameDepth = fieldpbr::GAME_DEPTH_PER_VIEW;
				if (occlusion.gameDepth > 0)
				{
					const double off = occlusion.gameDepth / fieldpbr::GAME_DEPTH_PER_VIEW;
					Say("  foreground tiles: %zu Z value(s); by your depth map the game's depth is %.0f per view unit (it is 16384%s)\n",
						occlusion.nearest.size(), occlusion.gameDepth, off < 0.7 || off > 1.4 ? ": the depth map and the tiles disagree there; "
						"characters are still hidden as the game does it, per Z" : ", fine");
				}
			}
			else if (occlusion.gameDepth > 0)
			{
				settings.gameDepth = occlusion.gameDepth;
				Say("  foreground tiles: %zu Z value(s); the game's depth from them: %.0f per view unit (estimated depth)\n",
					occlusion.nearest.size(), occlusion.gameDepth);
			}
			if (!occlusion.report.empty())
				Say("%s", occlusion.report.c_str());
			//the farthest depth there is: where the sky would start
			double farthest = 0;
			for (const float v : depth.v)
				farthest = std::max(farthest, settings.space.Distance(v));
			settings.skyValue = std::clamp(settings.space.Value(farthest * 0.995), 0.0, 1.0);
			if (f.haveCamera && settings.space.measured)
			{
				const fieldpbr::CameraTransform transform = fieldpbr::MakeCameraTransform(f.camera, cameraVariant);
				settings.cameraKnown = true;
				settings.cameraEye = transform.eye;
				settings.cameraRight = transform.right;
				settings.cameraDown = transform.down;
				settings.cameraForward = transform.forward;
				settings.up = WorldUp(transform, f.walkmesh);
			}
			//the light grid
			const auto start = std::chrono::steady_clock::now();
			ProbeSettings probeSettings;
			probeSettings.threads = o.threads;
			double zNear = 0, zFar = 0;
			std::string report;
			const fieldpbr::ProbeGrid grid = BakeProbes(painting, depth, &mask, settings.space, scale, probeSettings, zNear, zFar, report);
			if (grid.IsValid())
			{
				const std::vector<uint8_t> bytes = fieldpbr::WriteProbes(grid);
				std::ofstream out(stem + "_PROBES.dds", std::ios::binary);
				if (out && out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
				{
					settings.probeNear = zNear;
					settings.probeFar = zFar;
					Say("  %s_PROBES.dds: the room's light for the characters, %s", f.stem.c_str(), report.c_str());
					Say("    (%.1f s)\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
				}
				else
					Say("  ERROR writing %s_PROBES.dds\n", f.stem.c_str());
			}
		}

		//_LF and _ND for the whole field
		const Image lightmap = PackLightmap(field.light, field.scale, &mask);
		Image lightmapForDds;
		if (o.dds)
			lightmapForDds = PackLightmap(LightFrom(f.composite, fieldAlbedoDecoded, field.scale), field.scale, &mask);
		WriteMap(stem + "_LF", lightmap, o.dds ? &lightmapForDds : nullptr, true, o);
		Say("  %s_LF: lightmap%s\n", f.stem.c_str(), flickering ? (" and the flicker mask of " + std::to_string(flickering) + " light(s)").c_str() : "");
		if (haveNormal || haveDepth)
		{
			Image flat;
			if (!haveNormal)
			{
				flat.width = width;
				flat.height = height;
				flat.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
				for (size_t i = 0; i < flat.rgba.size(); i += 4)
				{
					flat.rgba[i] = 128;
					flat.rgba[i + 1] = 128;
					flat.rgba[i + 2] = 255;
				}
			}
			WriteMap(stem + "_ND", PackNormalDepth(haveNormal ? normal : flat, depth), nullptr, false, o);
			Say("  %s_ND: normal%s and depth\n", f.stem.c_str(), haveNormal ? "" : " (flat: no _N)");
		}

		//each opaque layer: its albedo, and its own lightmap where it isn't what the whole field shows
		std::set<uint32_t> opaque;
		for (size_t i = 0; i < f.tiles.size(); i++)
			if (f.tiles[i].blend == 4)
				opaque.insert(f.layout.pupu[i]);
		size_t own = 0, shared = 0;
		const float* table = fieldpbr::SrgbTable();
		for (const auto& [pupu, layer] : f.layers)
		{
			if (!opaque.count(pupu))
				continue;
			const std::string layerStem = f.layerStem[pupu];
			Image ownAlbedo;
			std::string ownUsed;
			const bool hasOwnAlbedo = LoadInput(layerStem, { "_AM" }, LayerFraming(f, pupu), ownAlbedo, ownUsed);
			std::vector<uint8_t> differs;
			const size_t mismatches = Mismatches(layer, f.composite, &differs);
			if (!hasOwnAlbedo && mismatches == 0)
			{
				//what you see of this layer is the whole field: the field's albedo, and the field's _LF
				Image albedo = field.albedo;
				CopyAlpha(albedo, layer);
				PadUncovered(albedo, layer, 4, 0);
				WriteLayerMap(f, pupu, layerStem + "_A", albedo, nullptr, true, o);
				std::error_code error;
				fs::remove(layerStem + "_LF.png", error); //an old one of its own would be used instead of the field's
				fs::remove(layerStem + "_LF.dds", error);
				shared++;
				continue;
			}
			//the albedo wanted per pixel: yours for this layer, the field's where it shows, else what the field's light there makes of it
			Rgb wantedLayer;
			wantedLayer.width = width;
			wantedLayer.height = height;
			wantedLayer.v.assign(static_cast<size_t>(width) * height * 3, 0.0f);
			const Rgb ownLinear = hasOwnAlbedo ? SrgbToLinear(ownAlbedo) : Rgb();
			const size_t n = static_cast<size_t>(width) * height;
			for (size_t i = 0; i < n; i++)
			{
				if (layer.rgba[i * 4 + 3] == 0)
					continue;
				for (int c = 0; c < 3; c++)
				{
					float a;
					if (hasOwnAlbedo)
						a = ownLinear.v[i * 3 + c] / (tint.IsValid() ? tint.v[i * 3 + c] : 1.0f);
					else if (!differs[i])
						a = table[field.albedo.rgba[i * 4 + c]];
					else
					{
						const float light = field.light.v[i * 3 + c];
						a = light > 1e-4f ? table[layer.rgba[i * 4 + c]] / light : table[layer.rgba[i * 4 + c]];
					}
					wantedLayer.v[i * 3 + c] = std::clamp(a, 0.0f, 1.0f);
				}
			}
			Split split = SplitLight(layer, wantedLayer, 0);
			settings.layerLightmapScale[pupu] = split.scale;
			PadUncovered(split.albedo, layer, 4, 0);
			WriteLayerMap(f, pupu, layerStem + "_A", split.albedo, nullptr, true, o);
			Image layerLightmap = PackLightmap(split.light, split.scale, &mask);
			PadUncovered(layerLightmap, layer, 4, 255);
			Image layerLightmapDds;
			if (o.dds)
			{
				const Image decoded = DecodeBc7(EncodeBc7(split.albedo, true, o.threads), width, height);
				layerLightmapDds = PackLightmap(LightFrom(layer, decoded, split.scale), split.scale, &mask);
				PadUncovered(layerLightmapDds, layer, 4, 255);
			}
			WriteLayerMap(f, pupu, layerStem + "_LF", layerLightmap, o.dds ? &layerLightmapDds : nullptr, true, o);
			Say("  %s: its own _A and _LF (%s; lightmap up to %.2f)\n", fieldlayers::PupuHex(pupu).c_str(),
				hasOwnAlbedo ? ("albedo " + ownUsed).c_str() : (std::to_string(mismatches) + " pixels differ from the whole field").c_str(), split.scale);
			own++;
			//normal and depth of its own
			Image ownNormal;
			Gray ownDepth;
			std::string normalUsed, depthUsed;
			const bool layerNormal = LoadInput(layerStem, { "_N" }, LayerFraming(f, pupu), ownNormal, normalUsed);
			const bool layerDepth = LoadGrayInput(layerStem, { "_DEPTH" }, LayerFraming(f, pupu), ownDepth, depthUsed);
			if (layerNormal || layerDepth)
			{
				const Image nrm = layerNormal ? NormalsYUp(ownNormal, orientation.normalYDown) : normal;
				if (nrm.IsValid())
				{
					WriteLayerMap(f, pupu, layerStem + "_ND", PackNormalDepth(nrm, layerDepth ? ownDepth : depth), nullptr, false, o);
					Say("  %s: its own _ND\n", fieldlayers::PupuHex(pupu).c_str());
				}
			}
		}
		Say("  layers: %zu use the field's _LF, %zu have their own\n", shared, own);

		//settings for DEMASTER (your time, sky, character_shade and character_light stay as they were)
		{
			std::string old, oldWarnings;
			fieldpbr::Settings previous;
			if (ReadText(stem + fieldpbr::SETTINGS_SUFFIX, old) && fieldpbr::ParseSettings(old, previous, oldWarnings))
			{
				settings.sky = previous.sky;
				settings.time = previous.time;
				settings.characterShadeKeep = previous.characterShadeKeep;
				settings.characterLight = previous.characterLight;
			}
		}
		char notes[400];
		snprintf(notes, sizeof(notes), "Layer images at %dx. Depth %s.", scale,
			settings.space.measured ? "measured with the field's camera and walkmesh" : "estimated");
		if (WriteText(stem + fieldpbr::SETTINGS_SUFFIX, fieldpbr::WriteSettings(settings, f.stem, notes)))
			Say("  %s%s written\n", f.stem.c_str(), fieldpbr::SETTINGS_SUFFIX);
		//the check image
		std::string error;
		const Image check = CheckImage(f.composite, f.haveCamera ? &f.camera : nullptr, f.haveCamera ? &f.walkmesh : nullptr, cameraVariant,
			settings.space, lights, scale);
		if (SavePng(stem + "_fieldprep_check.png", check, error))
			Say("  %s_fieldprep_check.png: the walkmesh as the camera sees it (green), the lights (crosses) and their range (yellow)\n", f.stem.c_str());
	}

	//the emissive map from _RESM (how much of the painting there is its own glow); false: no _RESM
	bool EmissiveFromResidualFiles(const FieldInput& f, const Options& o, Gray& out, std::string& how)
	{
		const int width = f.composite.width, height = f.composite.height;
		const std::string stem = f.folder + f.stem;
		std::string used;
		Image residualInput;
		if (width <= 0 || !LoadInput(stem, { "_RESM" }, CanvasFraming(f), residualInput, used))
			return false;
		const Rgb painting = SrgbToLinear(f.composite);
		//stored with the sRGB curve or linear: as prep reads it (needs the bounced light and the albedo to tell)
		Image bouncedInput, albedoInput;
		std::string unused;
		bool srgb = false, known = false;
		if (LoadInput(stem, { "_BL" }, CanvasFraming(f), bouncedInput, unused)
			&& (LoadInput(stem, { "_AM" }, CanvasFraming(f), albedoInput, unused) || LoadInput(stem, { "_A" }, CanvasFraming(f), albedoInput, unused)))
		{
			srgb = LightingIsSrgb(painting, SrgbToLinear(albedoInput), bouncedInput, &residualInput);
			known = true;
		}
		const Rgb residual = srgb ? SrgbToLinear(residualInput) : AsStored(residualInput);
		out = EmissiveFromResidual(painting, residual, o.emissiveShare);
		size_t glowing = 0;
		for (const float v : out.v)
			glowing += v >= 0.5f;
		char text[300];
		snprintf(text, sizeof(text), "from %s (%s; where it is over %.0f%% of the painting: %.2f%% of the field glows)", used.c_str(),
			known ? (srgb ? "read as sRGB" : "read as linear") : "read as stored", o.emissiveShare * 100.0f,
			out.v.empty() ? 0.0 : 100.0 * static_cast<double>(glowing) / static_cast<double>(out.v.size()));
		how = text;
		return true;
	}

	//<field>_ORME: AO, roughness, metallic and the emissive map (yours, _E, else from _RESM)
	void Orme(FieldInput& f, const Options& o)
	{
		const int width = f.layout.width * f.scale, height = f.layout.height * f.scale;
		const std::string stem = f.folder + f.stem;
		Gray maps[4];
		bool have[4] = {};
		std::string used;
		const std::vector<std::string> names[4] = { { "_O" }, { "_R", "_RC", "_RM" }, { "_M", "_MC", "_MM" }, { "_E" } };
		const char* what[4] = { "AO", "roughness", "metallic", "emissive" };
		std::string list;
		for (int k = 0; k < 4; k++)
		{
			have[k] = LoadGrayInput(stem, names[k], CanvasFraming(f), maps[k], used);
			std::string from = have[k] ? used : "default";
			if (k == 3 && !have[k])
			{
				std::string how;
				have[k] = EmissiveFromResidualFiles(f, o, maps[k], how);
				from = have[k] ? how : "none (no _E or _RESM)";
			}
			list += std::string(k ? ", " : "") + what[k] + " " + from;
		}
		if (!have[0] && !have[1] && !have[2] && !have[3])
		{
			Say("  ORME: no %s_O, _R, _M, _E or _RESM: nothing to pack\n", f.stem.c_str());
			return;
		}
		WriteMap(stem + "_ORME", PackOrme(width, height, have[0] ? &maps[0] : nullptr, have[1] ? &maps[1] : nullptr,
			have[2] ? &maps[2] : nullptr, have[3] ? &maps[3] : nullptr), nullptr, false, o);
		Say("  %s_ORME: %s\n", f.stem.c_str(), list.c_str());
		if (!NewestImage(stem + fieldpbr::OLD_ORM_SUFFIX).empty())
			Say("  (%s%s from before is no longer read: DEMASTER takes _ORME; you can delete it)\n", f.stem.c_str(), fieldpbr::OLD_ORM_SUFFIX);
		if (!NewestImage(stem + "_H").empty())
			Say("  (%s_H is no longer packed: the alpha holds the emissive map now)\n", f.stem.c_str());
		//layers with maps of their own (a layer's own _E; else the field's emissive map)
		for (const auto& [pupu, layer] : f.layers)
		{
			const std::string layerStem = f.layerStem[pupu];
			Gray own[4];
			bool any = false;
			bool ownHave[4] = {};
			for (int k = 0; k < 4; k++)
				any |= ownHave[k] = LoadGrayInput(layerStem, names[k], LayerFraming(f, pupu), own[k], used);
			if (!any)
				continue;
			const Gray* pick[4];
			for (int k = 0; k < 4; k++)
				pick[k] = ownHave[k] ? &own[k] : have[k] ? &maps[k] : nullptr;
			WriteLayerMap(f, pupu, layerStem + "_ORME", PackOrme(width, height, pick[0], pick[1], pick[2], pick[3]), nullptr, false, o);
			Say("  %s: its own _ORME\n", fieldlayers::PupuHex(pupu).c_str());
		}
	}

	//the languages a field's images come in ("" for the plain names)
	std::set<std::string> Languages(const std::string& folder, const std::string& name)
	{
		std::set<std::string> out;
		const std::regex pattern(name + "(_([a-z]{2}))?_[0-9A-F]{8}\\.(png|dds)", std::regex::icase);
		std::error_code error;
		for (const auto& entry : fs::directory_iterator(folder, error))
		{
			std::smatch m;
			const std::string file = entry.path().filename().string();
			if (std::regex_match(file, m, pattern))
			{
				std::string field, language;
				fieldlayers::SplitLanguage(name + (m[2].matched ? "_" + Lower(m[2].str()) : std::string()), field, language);
				out.insert(field == name ? language : std::string());
			}
		}
		return out;
	}

	bool IsFieldFolder(const fs::path& folder)
	{
		const std::string name = Lower(folder.filename().string());
		return !Languages(folder.string() + "/", name).empty();
	}

	void Field(const std::string& folderPath, const Options& o)
	{
		std::string folder = folderPath;
		if (!folder.empty() && folder.back() != '\\' && folder.back() != '/')
#ifdef _WIN32
			folder += '\\';
#else
			folder += '/';
#endif
		const std::string name = Lower(fs::path(folderPath).filename().string());
		for (const std::string& language : Languages(folder, name))
		{
			logText.clear();
			FieldInput f;
			f.folder = folder;
			f.name = name;
			f.language = language;
			f.stem = fieldpbr::FieldStem("", name, language);
			Say("\n%s%s%s (%s)\n", name.c_str(), language.empty() ? "" : " ", language.c_str(), o.command.c_str());
			const auto start = std::chrono::steady_clock::now();
			if (LoadField(f, o))
			{
				LoadComposite(f, o, o.command == "compose");
				if (o.command == "prep" || o.command == "all")
					Prep(f, o);
				if (o.command == "orme" || o.command == "all")
					Orme(f, o);
			}
			Say("  done in %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
			WriteText(folder + f.stem + "_fieldprep.txt", logText);
		}
	}

	void Usage()
	{
		puts(
			"fieldprep: gets field backgrounds ready for DEMASTER's PBR lighting (FIELD_PBR)\n"
			"\n"
			"  fieldprep [command] <folder> [options]\n"
			"\n"
			"<folder>: a field's folder (DEMASTER_EXP\\textures\\field_bg\\bv\\bvtr_1), or a folder of them (field_bg, field_bg\\bv):\n"
			"          every field in it.\n"
			"\n"
			"Commands:\n"
			"  prep     (the default) from your maps: <field>_<pupu>_A for every opaque layer, <field>_LF, <field>_ND,\n"
			"           <field>_O (AO, baked from depth and normals), <field>_lights.txt and <field>_pbr.txt\n"
			"  orme     packs <field>_O, _R, _M and the emissive map into <field>_ORME (do it after prep, once you're happy with _O).\n"
			"           The emissive map is your <field>_E.png (white: glows), else made from _RESM: where the non-diffuse\n"
			"           residual is most of the painting (a lamp's bulb, a lit screen). (ormh: the same, its old name)\n"
			"  all      prep, then orme\n"
			"  compose  only writes <field>.png: the opaque layers put together (what to run Marigold/CHORD on)\n"
			"\n"
			"Your maps, in the field's folder, all PNG, at the canvas size (layer images from PuPu or FME: the canvas is FME's\n"
			"when all of them are FME's, else the box around the opaque layers' pictures; maps made for the other one are moved\n"
			"into place, not stretched):\n"
			"  <field>.png          the whole field, opaque layers only (made by compose/prep if missing)\n"
			"  <field>_AM.png       albedo of the whole field (needed)\n"
			"  <field>_N.png        normals          <field>_DEPTH.png  depth (16-bit is best)\n"
			"  <field>_BL.png       bounced light    <field>_RESM.png   non-diffuse residual\n"
			"  <field>_R.png  _M.png  _E.png        roughness, metallic, emissive (_RC/_RM and _MC/_MM are read if _R/_M are missing)\n"
			"  <field>_<pupu>_AM.png (and _N, _DEPTH, _O, _R, _M, _E)   one layer's own, where it differs\n"
			"  <field>_LIGHTS.png   your light marks on black: pure green (0,255,0) dots add a day lamp each, blue (0,0,255) a night\n"
			"                       lamp, cyan (0,255,255) one on day and night; pure red (255,0,0) keeps lamps away from what it covers\n"
			"The field's .map, camera and walkmesh come from <field>.map/.ca/.id in the folder, else DEMASTER_EXP\\data\\field.fs,\n"
			"else the game's main.zzz.\n"
			"\n"
			"Options:\n"
			"  --data <where>       a folder with field.fl/fi/fs, or the game's main.zzz (default: DEMASTER_EXP\\data, else main.zzz)\n"
			"  --no-dds             PNG only (default: PNG and BC7 DDS)\n"
			"  --relight            find the lights again and rewrite <field>_lights.txt (it's kept otherwise; the old one is kept\n"
			"                       as <field>_lights_old.txt). A <field>_LIGHTS.png newer than _lights.txt does the same.\n"
			"  --rebake             bake <field>_O again (it's kept otherwise)\n"
			"  --force              compose: write <field>.png even if there is one\n"
			"  --tint <0-1>         how much of the light's colour moves from the albedo into the lightmap (default 0.5)\n"
			"  --ao-radius <px>     AO reach, native pixels (default 10)    --ao-strength <x>  (default 1)\n"
			"  --light-intensity <x>  light each found light adds on top (default 0.2; 0 keeps the painting as it is)\n"
			"  --max-lights <n>     the large lights fieldprep keeps per time of day (default 2, at most 2)\n"
			"  --light-share <%>    how much of the painting's light (above the ambient) a light must explain to count (default 2)\n"
			"  --emissive-share <%> orme: how much of the painting the residual has to be for a pixel to start glowing (default 35;\n"
			"                       full glow 40 points above)\n"
			"  --threads <n>\n");
	}
}

int main(int argc, char** argv)
{
	Options o;
	o.threads = std::max(1u, std::thread::hardware_concurrency());
	std::vector<std::string> plain;
	for (int i = 1; i < argc; i++)
	{
		const std::string a = argv[i];
		const auto value = [&](double& out) -> bool
		{
			if (i + 1 >= argc)
				return false;
			char* end = nullptr;
			out = strtod(argv[++i], &end);
			return end != argv[i];
		};
		double v = 0;
		if (a == "--help" || a == "-h" || a == "/?")
		{
			Usage();
			return 0;
		}
		else if (a == "--data" && i + 1 < argc)
			o.data = argv[++i];
		else if (a == "--no-dds")
			o.dds = false;
		else if (a == "--relight")
			o.relight = true;
		else if (a == "--rebake")
			o.rebake = true;
		else if (a == "--force")
			o.force = true;
		else if (a == "--tint" && value(v))
			o.tint.strength = static_cast<float>(std::clamp(v, 0.0, 1.0));
		else if (a == "--ao-radius" && value(v))
			o.ao.radius = std::clamp(v, 1.0, 200.0);
		else if (a == "--ao-strength" && value(v))
			o.ao.strength = std::clamp(v, 0.1, 8.0);
		else if (a == "--light-intensity" && value(v))
			o.lights.intensity = static_cast<float>(std::max(0.0, v));
		else if (a == "--light-share" && value(v))
			o.lights.minShare = static_cast<float>(std::clamp(v, 0.1, 50.0) / 100.0);
		else if (a == "--emissive-share" && value(v))
			o.emissiveShare = static_cast<float>(std::clamp(v, 1.0, 95.0) / 100.0);
		else if (a == "--max-lights" && value(v))
			o.lights.maxLights = std::clamp(static_cast<int>(v), 0, fieldpbr::MAX_FOUND_LIGHTS);
		else if (a == "--threads" && value(v))
			o.threads = std::clamp(static_cast<int>(v), 1, 64);
		else if (!a.empty() && a[0] == '-')
		{
			fprintf(stderr, "unknown option %s (fieldprep --help)\n", a.c_str());
			return 2;
		}
		else
			plain.push_back(a);
	}
	if (!plain.empty() && (plain[0] == "prep" || plain[0] == "orme" || plain[0] == "ormh" || plain[0] == "all" || plain[0] == "compose"))
	{
		o.command = plain[0] == "ormh" ? "orme" : plain[0];
		plain.erase(plain.begin());
	}
	if (plain.size() != 1)
	{
		Usage();
		return 2;
	}
	o.target = plain[0];
	std::error_code error;
	if (!fs::is_directory(o.target, error))
	{
		fprintf(stderr, "%s is not a folder\n", o.target.c_str());
		return 2;
	}
	std::vector<std::string> fields;
	const fs::path root = fs::path(o.target);
	if (IsFieldFolder(root))
		fields.push_back(root.string());
	else
	{
		//field_bg\<xx>\<field> or <xx>\<field>
		for (const auto& a : fs::directory_iterator(root, error))
		{
			if (!a.is_directory())
				continue;
			if (IsFieldFolder(a.path()))
				fields.push_back(a.path().string());
			else
				for (const auto& b : fs::directory_iterator(a.path(), error))
					if (b.is_directory() && IsFieldFolder(b.path()))
						fields.push_back(b.path().string());
		}
		std::sort(fields.begin(), fields.end());
	}
	if (fields.empty())
	{
		fprintf(stderr, "no field folders in %s (a field's folder has its layer images, <field>_<pupu>.png)\n", o.target.c_str());
		return 1;
	}
	printf("fieldprep %s: %zu field(s), %d threads%s\n", o.command.c_str(), fields.size(), o.threads, o.dds ? "" : ", PNG only");
	for (const std::string& folder : fields)
		Field(folder, o);
	return 0;
}
