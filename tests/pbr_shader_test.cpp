//Runs DEMASTER_EXP/shaders/demaster_field.frag on a field's PBR maps the way DEMASTER binds them, with Mesa's software OpenGL
//(Linux, EGL, no window), and draws the field's opaque tiles through it. Checks that with the lights at rest the shader gives
//back the paintings, that added light rolls off towards white on the curve it should, that the night dims the painting and
//keeps lamps' light and glows as it should, and saves what it draws (lit, at rest, every FIELD_PBR_VIEW) as PNG.
//
//	pbr_shader_test <field folder> <demaster_field.frag> <output folder>
//
//Linux with Mesa (llvmpipe works):
//	g++ -std=c++20 -O2 -pthread pbr_shader_test.cpp ../ff8_demaster/field_layers.cpp ../ff8_demaster/field_pbr.cpp
//	    ../ff8_demaster/field_moving_lights.cpp ../fieldprep/image_io.cpp ../fieldprep/third_party/*.cpp -lEGL -lGL -o pbr_shader_test
//	(or tests/build_tests.sh, which builds every test)
//BENCH=<frames> in the environment: also times the shader (the whole canvas drawn that many times per setup) and prints
//milliseconds per frame for each setup: the field as it is, the look, the moving lights, wall checks at each quality.
//The field folder needs <field>.map, the layer images and fieldprep's output (PNG).

#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include "../ff8_demaster/field_layers.h"
#include "../ff8_demaster/field_pbr.h"
#include "../ff8_demaster/field_moving_lights.h"
#include "../fieldprep/image_io.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace fieldlayers;
using fieldprep::Image;

namespace
{
	std::string ReadText(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		std::ostringstream out;
		out << in.rdbuf();
		return out.str();
	}

	bool Exists(const std::string& path)
	{
		std::error_code e;
		return std::filesystem::is_regular_file(path, e);
	}

	GLuint Compile(const GLenum type, const std::string& source)
	{
		const GLuint shader = glCreateShader(type);
		const char* text = source.c_str();
		glShaderSource(shader, 1, &text, nullptr);
		glCompileShader(shader);
		GLint ok = 0;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
		if (!ok)
		{
			char log[4096];
			glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
			fprintf(stderr, "shader: %s\n", log);
			exit(1);
		}
		return shader;
	}

	GLuint Texture(const Image& image, const GLenum filter)
	{
		GLuint t = 0;
		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, image.width, image.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		return t;
	}

	struct LayerFiles
	{
		std::string path[fieldpbr::KIND_COUNT];
		bool ownLightmap = false;
		bool emissive = false; //its ORM map is an _ORME (an older _ORMH's alpha is the height: not used)
	};
}

int main(int argc, char** argv)
{
	if (argc < 4)
	{
		fprintf(stderr, "pbr_shader_test <field folder> <demaster_field.frag> <output folder>\n");
		return 2;
	}
	std::string folder = argv[1];
	if (folder.back() != '/')
		folder += '/';
	const std::string name = std::filesystem::path(argv[1]).filename().string(), out = std::string(argv[3]) + "/";
	std::filesystem::create_directories(out);

	//--- OpenGL without a window
	auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
	EGLDisplay display = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint major = 0, minor = 0;
	if (!eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API))
	{
		fprintf(stderr, "no EGL\n");
		return 1;
	}
	const EGLint configAttributes[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
	EGLConfig config;
	EGLint configs = 0;
	eglChooseConfig(display, configAttributes, &config, 1, &configs);
	const EGLint contextAttributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
	EGLContext context = eglCreateContext(display, configs ? config : nullptr, EGL_NO_CONTEXT, contextAttributes);
	if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
	{
		fprintf(stderr, "no OpenGL context\n");
		return 1;
	}
	printf("OpenGL: %s, %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

	//--- the field as DEMASTER plans it
	std::vector<uint8_t> map;
	{
		std::ifstream in(folder + name + ".map", std::ios::binary);
		map.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
	}
	std::vector<Tile> tiles = ParseMap(map.data(), map.size());
	std::map<std::string, Image> images;
	const auto load = [&](const std::string& path) -> const Image*
	{
		auto it = images.find(path);
		if (it == images.end())
		{
			Image image;
			std::string error;
			if (!fieldprep::LoadRgba(path, image, error))
				return nullptr;
			it = images.emplace(path, std::move(image)).first;
		}
		return &it->second;
	};
	//every tile's picture (PuPu's names and frames, else FME's) and the canvas from _pbr.txt, as OnMapRead does it
	const Layout fme = ComputeLayout(tiles);
	const PupuNames pupuNames = ComputePupuNames(tiles);
	const PictureSet pictures = FindPictures(tiles, fme, pupuNames, [&](const uint32_t pupu, int& w, int& h)
	{
		const std::string path = fieldpbr::LayerStem(folder, name, "", pupu) + ".png";
		const Image* image = Exists(path) ? load(path) : nullptr;
		if (image == nullptr)
			return false;
		w = image->width;
		h = image->height;
		return true;
	});
	std::string warnings;
	fieldpbr::Settings fromFile;
	fieldpbr::ParseSettings(ReadText(folder + name + fieldpbr::SETTINGS_SUFFIX), fromFile, warnings);
	const Frame canvas = fromFile.hasCanvas ? Frame{ fromFile.canvas[0], fromFile.canvas[1], fromFile.canvas[2], fromFile.canvas[3] } : fme.fme;
	const Layout layout = FramedLayout(tiles, fme, pictures, canvas);
	fieldpbr::Settings settings;
	settings.space = fieldpbr::EstimatedSpace(layout.shiftX, layout.shiftY, true);
	warnings.clear();
	fieldpbr::ParseSettings(ReadText(folder + name + fieldpbr::SETTINGS_SUFFIX), settings, warnings);
	fieldpbr::Lights lights;
	fieldpbr::ParseLights(ReadText(folder + name + fieldpbr::LIGHTS_SUFFIX), lights, warnings);
	if (!warnings.empty())
		printf("%s", warnings.c_str());
	//files per layer, as PbrFindFiles finds them (PNG here)
	std::map<uint32_t, std::string> painting;
	std::map<uint32_t, LayerFiles> pbr;
	std::string fieldMap[fieldpbr::KIND_COUNT];
	for (int k = 1; k < fieldpbr::KIND_COUNT; k++)
		if (const std::string p = folder + name + fieldpbr::KindSuffix(static_cast<fieldpbr::Kind>(k)) + ".png"; Exists(p))
			fieldMap[k] = p;
	const bool fieldEmissive = !fieldMap[fieldpbr::ORME].empty();
	if (!fieldEmissive && Exists(folder + name + fieldpbr::OLD_ORM_SUFFIX + ".png"))
		fieldMap[fieldpbr::ORME] = folder + name + fieldpbr::OLD_ORM_SUFFIX + ".png";
	std::vector<bool> hasImage(tiles.size());
	for (size_t i = 0; i < tiles.size(); i++)
	{
		if (pictures.tile[i] < 0)
			continue;
		const uint32_t pupu = layout.pupu[i];
		const std::string stem = fieldpbr::LayerStem(folder, name, "", pupu);
		if (Exists(stem + ".png"))
			painting[pupu] = stem + ".png";
		if (tiles[i].blend == 4 && Exists(stem + "_A.png"))
		{
			LayerFiles f;
			f.path[fieldpbr::ALBEDO] = stem + "_A.png";
			for (int k = 1; k < fieldpbr::KIND_COUNT; k++)
			{
				const std::string own = stem + fieldpbr::KindSuffix(static_cast<fieldpbr::Kind>(k)) + ".png";
				f.path[k] = Exists(own) ? own : fieldMap[k];
				if (k == fieldpbr::LIGHTMAP)
					f.ownLightmap = Exists(own);
				if (k == fieldpbr::ORME)
				{
					const std::string old = stem + fieldpbr::OLD_ORM_SUFFIX + ".png";
					f.emissive = Exists(own) || (!Exists(old) && fieldEmissive);
					if (!Exists(own) && Exists(old))
						f.path[k] = old;
				}
			}
			if (!f.path[fieldpbr::LIGHTMAP].empty())
				pbr[pupu] = f;
		}
		hasImage[i] = painting.count(pupu) || pbr.count(pupu);
	}
	const std::vector<Tile> shipped = tiles;
	ApplyRelocations(tiles, Unshare(tiles, hasImage).moved);
	const std::map<uint32_t, PagePlan> plans = PlanPages(tiles, layout, hasImage, &shipped);
	printf("%s: %zu tiles, %zu pages, %zu PBR layers, %zu lights, canvas %dx%d (%s)\n", name.c_str(), tiles.size(), plans.size(),
		pbr.size(), lights.lights.size(), layout.width, layout.height, fromFile.hasCanvas ? "from _pbr.txt" : "FME's");
	const int scale = pictures.pictures.empty() ? 1 : pictures.pictures.begin()->second.scale;

	//--- every page: albedo (or painting) page, _LF, _ND, _ORME pages, the cell table
	struct Page
	{
		GLuint map[fieldpbr::KIND_COUNT] = {};
		GLuint cells = 0;
	};
	std::map<uint32_t, Page> pages;
	for (const auto& [key, plan] : plans)
	{
		Page page;
		for (int k = 0; k < fieldpbr::KIND_COUNT; k++)
		{
			std::vector<StampSource> sources;
			for (const CellStamp& stamp : plan.stamps)
			{
				const auto files = pbr.find(stamp.pupu);
				std::string path;
				if (k == fieldpbr::ALBEDO)
					path = files != pbr.end() && !stamp.semi ? files->second.path[k] : (painting.count(stamp.pupu) ? painting[stamp.pupu] : "");
				else if (files != pbr.end() && !stamp.semi)
					path = files->second.path[k];
				const Image* image = path.empty() ? nullptr : load(path);
				Frame frame;
				const int fit = image != nullptr ? FitPicture(layout, stamp.pupu, image->width, image->height, frame) : 0;
				if (image != nullptr && fit == scale)
					sources.push_back({ image, scale, stamp.mapX - frame.left, stamp.mapY - frame.top, stamp.cellX, stamp.cellY,
						k == fieldpbr::ALBEDO });
				else if (image != nullptr)
					printf("  %s is %dx%d: fits no frame at %dx\n", path.c_str(), image->width, image->height, scale);
			}
			if (sources.empty() && k > 0)
				continue;
			page.map[k] = Texture(ComposePage(256, 256, scale, nullptr, 0, 0, false, sources), GL_NEAREST);
		}
		std::vector<float> cells(16 * 32 * 4, 0.0f); //demaster_field.frag: a second block of 16 rows (Z, layer, nearest)
		for (const CellStamp& stamp : plan.stamps)
		{
			const auto files = pbr.find(stamp.pupu);
			if (files == pbr.end() || stamp.semi)
				continue;
			float* c = cells.data() + ((stamp.cellY / 16) * 16 + stamp.cellX / 16) * 4;
			c[0] = static_cast<float>(stamp.layerX);
			c[1] = static_cast<float>(stamp.layerY);
			c[2] = 1.0f + (page.map[fieldpbr::NORMAL_DEPTH] && !files->second.path[fieldpbr::NORMAL_DEPTH].empty() ? 2.0f : 0.0f)
				+ (page.map[fieldpbr::ORME] && !files->second.path[fieldpbr::ORME].empty() ? 4.0f : 0.0f);
			//a picture of the glows (_EC): +16, and the ORM map's alpha isn't read (no +8)
			const bool glowPicture = page.map[fieldpbr::EMISSIVE] && !files->second.path[fieldpbr::EMISSIVE].empty();
			if (glowPicture)
				c[2] += 16.0f;
			else if (page.map[fieldpbr::ORME] && !files->second.path[fieldpbr::ORME].empty() && files->second.emissive)
				c[2] += 8.0f;
			c[3] = settings.LightmapScale(stamp.pupu, files->second.ownLightmap);
			float* more = cells.data() + ((stamp.cellY / 16 + 16) * 16 + stamp.cellX / 16) * 4;
			more[0] = static_cast<float>(stamp.z);
			more[1] = static_cast<float>(stamp.layerId);
			const auto anchor = settings.occlusion.find(stamp.z);
			more[2] = anchor != settings.occlusion.end() ? static_cast<float>(anchor->second) : 0.0f;
		}
		glGenTextures(1, &page.cells);
		glBindTexture(GL_TEXTURE_2D, page.cells);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 16, 32, 0, GL_RGBA, GL_FLOAT, cells.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		pages[key] = page;
	}

	//--- shader
	const GLuint program = glCreateProgram();
	glAttachShader(program, Compile(GL_VERTEX_SHADER, "#version 120\nvoid main(){gl_Position=ftransform();gl_TexCoord[0]=gl_MultiTexCoord0;gl_FrontColor=gl_Color;}\n"));
	glAttachShader(program, Compile(GL_FRAGMENT_SHADER, ReadText(argv[2])));
	glLinkProgram(program);
	GLint linked = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	if (!linked)
	{
		fprintf(stderr, "link failed\n");
		return 1;
	}
	glUseProgram(program);
	const auto at = [&](const char* uniform) { return glGetUniformLocation(program, uniform); };
	glUniform1i(at("tex"), 0);
	glUniform1i(at("demasterLF"), 1);
	glUniform1i(at("demasterND"), 2);
	glUniform1i(at("demasterORME"), 3);
	glUniform1i(at("demasterEC"), 9);
	glUniform1i(at("demasterCells"), 4);

	//--- target: the canvas at the layers' scale
	const int width = layout.width * scale, height = layout.height * scale;
	GLuint target = 0, fbo = 0;
	glGenTextures(1, &target);
	glBindTexture(GL_TEXTURE_2D, target);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
	glViewport(0, 0, width, height);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	//opaque tiles, far first, each animation's first state (as fieldprep composes the field)
	std::map<int, int> firstState;
	for (const Tile& t : tiles)
		if (t.animId != 0xFF && (!firstState.count(t.animId) || t.animState < firstState[t.animId]))
			firstState[t.animId] = t.animState;
	std::vector<size_t> order;
	for (size_t i = 0; i < tiles.size(); i++)
		if (tiles[i].blend == 4 && (tiles[i].animId == 0xFF || tiles[i].animState == firstState[tiles[i].animId]))
			order.push_back(i);
	std::stable_sort(order.begin(), order.end(), [&](const size_t a, const size_t b) { return tiles[a].z > tiles[b].z; });

	const fieldpbr::Space& space = settings.space;
	//the whole field's _ND: the room (wall checks, unit 5) and the samples for the relight's scale, as DEMASTER takes them
	Image room;
	std::string roomError;
	const bool hasRoom = fieldprep::LoadRgba(folder + name + "_ND.png", room, roomError) && room.width == width && room.height == height;
	std::vector<fieldpbr::ShadeSample> samples;
	if (hasRoom)
	{
		constexpr int GRID_X = 32, GRID_Y = 24;
		std::vector<uint8_t> grid(GRID_X * GRID_Y * 4);
		for (int gy = 0; gy < GRID_Y; gy++)
			for (int gx = 0; gx < GRID_X; gx++)
			{
				const int px = std::min(width - 1, static_cast<int>((gx + 0.5) * width / GRID_X));
				const int py = std::min(height - 1, static_cast<int>((gy + 0.5) * height / GRID_Y));
				memcpy(grid.data() + (gy * GRID_X + gx) * 4, room.rgba.data() + (static_cast<size_t>(py) * width + px) * 4, 4);
			}
		samples = fieldpbr::ShadeSamples(grid.data(), GRID_X, GRID_Y, settings, layout.width, layout.height);
		const GLuint roomTexture = Texture(room, GL_LINEAR);
		glActiveTexture(GL_TEXTURE5);
		glBindTexture(GL_TEXTURE_2D, roomTexture);
		glActiveTexture(GL_TEXTURE0);
		glUniform1i(at("demasterRoom"), 5);
		glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	}
	glUniform4f(at("demasterRoomSize"), static_cast<float>(layout.width), static_cast<float>(layout.height), hasRoom ? 1.0f : 0.0f, 0.0f);
	fieldpbr::Vec3 fieldCentre;
	double fieldExtent = 1.0;
	fieldpbr::FieldExtent(settings, layout.width, layout.height, fieldCentre, fieldExtent);
	//night: how night it is (DAY_NIGHT); the lights on then and their weights as DEMASTER picks them (ActiveLights)
	double night = 0.0;
	const fieldpbr::Lights* use = &lights;
	//the look's dials, the moving lights after the field's own, wall-check steps (FIELD_PBR_QUALITY: 0, 6, 12), as DEMASTER
	//sends them (Lights() and SetUniforms in texturepatch_v2_fieldPbr.cpp)
	fieldpbr::Look lookNow;
	std::vector<fieldpbr::MovingLight> movingNow;
	int wallSteps = 0;
	bool timing = false; //draw without reading the picture back
	double lastScale = 1.5;
	const auto draw = [&](const int view, const bool lit, const double seconds, const bool smooth, const std::string& file)
	{
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		float pos[64] = {}, color[64] = {}, flicker[64] = {}, paint[64] = {};
		int movingCount = 0;
		for (const fieldpbr::MovingLight& m : movingNow)
			movingCount += m.on;
		const std::vector<fieldpbr::ActiveLight> active = fieldpbr::ActiveLights(*use, night, nullptr,
			static_cast<size_t>(fieldpbr::MAX_LIGHTS - movingCount));
		int count = static_cast<int>(std::min<size_t>(active.size(), fieldpbr::MAX_LIGHTS - movingCount));
		for (int i = 0; i < count; i++)
		{
			const fieldpbr::Light& l = use->lights[active[i].index];
			const fieldpbr::Vec3 p = space.ViewPosition(l.x, l.y, l.depth);
			const double f = lit ? fieldpbr::FlickerValue(l.flicker, l.amount, l.speed, fieldpbr::LightSeed(l, active[i].index), seconds) : 1.0;
			pos[i * 4] = static_cast<float>(p.x);
			pos[i * 4 + 1] = static_cast<float>(p.y);
			pos[i * 4 + 2] = static_cast<float>(p.z);
			pos[i * 4 + 3] = static_cast<float>(space.ViewLength(l.range, p.z));
			for (int c = 0; c < 3; c++)
				color[i * 4 + c] = lit ? static_cast<float>(l.color[c] * l.intensity * f * active[i].weight) : 0.0f;
			color[i * 4 + 3] = l.specular;
			const bool owns = l.OwnsMask();
			flicker[i * 4] = owns ? static_cast<float>(f - 1.0) : 0.0f;
			flicker[i * 4 + 1] = owns ? 1.0f : 0.0f;
			flicker[i * 4 + 2] = l.ShadowStrength() * active[i].weight;
			flicker[i * 4 + 3] = active[i].nightLamp ? 1.0f : 0.0f;
			for (int c = 0; c < 3; c++)
				paint[i * 4 + c] = l.baked ? static_cast<float>(l.color[c] * f * active[i].weight) : 0.0f;
			paint[i * 4 + 3] = l.baked ? active[i].weight : 0.0f;
		}
		for (const fieldpbr::MovingLight& m : movingNow)
		{
			if (!m.on || count >= fieldpbr::MAX_LIGHTS)
				continue;
			const int i = count++;
			pos[i * 4] = static_cast<float>(m.position.x);
			pos[i * 4 + 1] = static_cast<float>(m.position.y);
			pos[i * 4 + 2] = static_cast<float>(m.position.z);
			pos[i * 4 + 3] = static_cast<float>(m.range);
			for (int c = 0; c < 3; c++)
			{
				color[i * 4 + c] = lit ? m.color[c] : 0.0f;
				paint[i * 4 + c] = m.gloss[c];
			}
			color[i * 4 + 3] = m.specular;
			paint[i * 4 + 3] = -m.shade;
			flicker[i * 4 + 2] = m.shadow;
		}
		lastScale = lookNow.relight > 0 ? fieldpbr::RelightScale(samples, pos, paint, count) : 1.5;
		glUniform4f(at("demasterLook"), lookNow.relight, lookNow.highlights, lookNow.ao, static_cast<float>(lastScale));
		glUniform4fv(at("demasterLightPaint"), fieldpbr::MAX_LIGHTS, paint);
		glUniform4f(at("demasterQuality"), static_cast<float>(wallSteps), 0.0f, 1.0f, static_cast<float>(fieldExtent));
		glUniform4f(at("demasterFeatures"), 0.0f, 0.0f, 0.0f, wallSteps > 0 && hasRoom ? 1.0f : 0.0f);
		glUniform4f(at("demasterNight"), static_cast<float>(night), use->night.level, use->night.lamps, 0.0f);
		glUniform4f(at("demasterNightTint"), use->night.tint[0], use->night.tint[1], use->night.tint[2], 0.0f);
		glUniform1i(at("demasterLightCount"), count);
		glUniform4fv(at("demasterLightPos"), fieldpbr::MAX_LIGHTS, pos);
		glUniform4fv(at("demasterLightColor"), fieldpbr::MAX_LIGHTS, color);
		glUniform4fv(at("demasterLightFlicker"), fieldpbr::MAX_LIGHTS, flicker);
		glUniform4f(at("demasterSpace"), static_cast<float>(space.zoom), static_cast<float>(space.originX), static_cast<float>(space.originY),
			space.model == fieldpbr::DepthModel::Inverse ? 1.0f : 0.0f);
		glUniform4f(at("demasterDepth"), static_cast<float>(space.a), static_cast<float>(space.b), static_cast<float>(space.nearest), 0.0f);
		glUniform4f(at("demasterAmbient"), lit ? use->ambient[0] : 0, lit ? use->ambient[1] : 0, lit ? use->ambient[2] : 0, 0);
		glUniform4f(at("demasterEmissive"), use->emissive.hue / 360.0f, use->emissive.strength, 0.0f,
			std::max(static_cast<float>(night), use->emissive.day));
		glUniform4f(at("demasterEmissiveTint"), use->emissive.tint[0], use->emissive.tint[1], use->emissive.tint[2], 0.0f);
		const float texel = 1.0f / (256 * scale);
		glUniform4f(at("demasterTileClamp"), smooth ? 16.0f / 256 : 0.0f, smooth ? 16.0f / 256 : 0.0f, texel, texel);
		for (const size_t i : order)
		{
			const Tile& t = tiles[i];
			const auto page = pages.find(PageKey(t.TexturePage(), t.Palette()));
			if (page == pages.end())
				continue;
			const bool isPbr = pbr.count(layout.pupu[i]) && page->second.map[fieldpbr::LIGHTMAP] != 0;
			glUniform4f(at("demasterPbr"), isPbr ? 1.0f : 0.0f, static_cast<float>(view), 16.0f, 16.0f);
			const GLuint* m = page->second.map;
			const GLuint bind[5] = { m[0], m[1], m[2], m[3], page->second.cells };
			glActiveTexture(GL_TEXTURE9);
			glBindTexture(GL_TEXTURE_2D, m[fieldpbr::EMISSIVE]);
			for (int u = 4; u >= 0; u--)
			{
				glActiveTexture(GL_TEXTURE0 + u);
				glBindTexture(GL_TEXTURE_2D, bind[u]);
			}
			const float x0 = layout.x[i] * 2.0f / layout.width - 1.0f, x1 = (layout.x[i] + 16) * 2.0f / layout.width - 1.0f;
			const float y0 = 1.0f - layout.y[i] * 2.0f / layout.height, y1 = 1.0f - (layout.y[i] + 16) * 2.0f / layout.height;
			const float u0 = t.srcX / 256.0f, u1 = (t.srcX + 16) / 256.0f, v0 = t.srcY / 256.0f, v1 = (t.srcY + 16) / 256.0f;
			glBegin(GL_QUADS);
			glColor4f(1, 1, 1, 1);
			glTexCoord2f(u0, v0); glVertex2f(x0, y0);
			glTexCoord2f(u1, v0); glVertex2f(x1, y0);
			glTexCoord2f(u1, v1); glVertex2f(x1, y1);
			glTexCoord2f(u0, v1); glVertex2f(x0, y1);
			glEnd();
		}
		Image result;
		if (timing)
			return result;
		result.width = width;
		result.height = height;
		result.rgba.resize(static_cast<size_t>(width) * height * 4);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, result.rgba.data());
		//bottom-up rows
		for (int y = 0; y < height / 2; y++)
			std::swap_ranges(result.rgba.begin() + static_cast<size_t>(y) * width * 4, result.rgba.begin() + static_cast<size_t>(y + 1) * width * 4,
				result.rgba.begin() + static_cast<size_t>(height - 1 - y) * width * 4);
		std::string error;
		if (!file.empty())
			fieldprep::SavePng(out + file, result, error);
		return result;
	};

	//at rest: the shader must give back the paintings (fieldprep's composite is the paintings put together the same way)
	Image composite;
	std::string error;
	fieldprep::LoadRgba(folder + name + ".png", composite, error);
	int failures = 0;
	for (const bool smooth : { false, true })
	{
		const Image rest = draw(0, false, 0.0, smooth, smooth ? "rest_smooth.png" : "rest.png");
		int worst = 0;
		double sum = 0;
		size_t n = 0, over = 0;
		for (size_t i = 0; i < rest.rgba.size(); i += 4)
		{
			if (composite.rgba.empty() || composite.rgba[i + 3] == 0)
				continue;
			int d = 0;
			for (int c = 0; c < 3; c++)
				d = std::max(d, std::abs(rest.rgba[i + c] - composite.rgba[i + c]));
			worst = std::max(worst, d);
			sum += d;
			over += d > 2;
			n++;
		}
		printf("at rest%s: against the paintings, max %d, mean %.3f, %.4f%% of pixels off by more than 2\n", smooth ? " (FIELD_SMOOTH)" : "",
			worst, n ? sum / n : 0.0, n ? 100.0 * over / n : 0.0);
		if (!smooth && (n == 0 || sum / n > 0.6 || 100.0 * over / n > 0.5))
			failures++;
	}
	draw(0, true, 1.25, true, "lit.png");
	const char* views[] = { "", "albedo", "lightmap", "normal", "depth", "ao", "roughness", "metallic", "flicker_mask", "added", "rest_view" };
	for (int v = 1; v <= 10; v++)
		draw(v, true, 1.25, true, std::string("view_") + std::to_string(v) + "_" + views[v] + ".png");
	//the glows (FIELD_PBR_VIEW 16: as they'll show at night; 17: the emissive map)
	draw(16, true, 1.25, true, "view_16_glow.png");
	draw(17, true, 1.25, true, "view_17_emissive.png");
	//flicker: the first light that flickers, at its darkest and brightest over 4 seconds
	for (size_t i = 0; i < lights.lights.size(); i++)
		if (lights.lights[i].flicker != fieldpbr::Flicker::None)
		{
			double low = 1e9, high = -1e9, tLow = 0, tHigh = 0;
			for (double t = 0; t < 4; t += 0.01)
			{
				const double f = fieldpbr::FlickerValue(lights.lights[i].flicker, lights.lights[i].amount, lights.lights[i].speed,
					fieldpbr::LightSeed(lights.lights[i], i), t);
				if (f < low) { low = f; tLow = t; }
				if (f > high) { high = f; tHigh = t; }
			}
			draw(0, true, tLow, true, "flicker_low.png");
			draw(0, true, tHigh, true, "flicker_high.png");
			printf("flicker: %s from %.2f to %.2f\n", lights.lights[i].name.c_str(), low, high);
			break;
		}
	//--- added light rolls off towards white instead of being cut off at it (demasterRollOff): with u the light added to a
	//painting P, the shader draws P + (1 - P)(1 - exp(-u / (1 - P))). So from the picture at rest (P) and the picture with a
	//light on (B), the picture with that light 4 times as strong is known: 1 - (1 - P)((1 - B) / (1 - P))^4
	{
		Image nd;
		std::string ndError;
		if (fieldprep::LoadRgba(folder + name + "_ND.png", nd, ndError) && nd.width == width && nd.height == height)
		{
			const int cx = width / 2, cy = height / 2;
			fieldpbr::Lights one;
			one.night = lights.night;
			fieldpbr::Light lamp;
			lamp.name = "test light";
			lamp.baked = false;
			lamp.specular = 0.0f; //highlights depend on nothing but the light's strength too, but are too sharp for 8-bit steps
			lamp.x = cx / static_cast<double>(scale);
			lamp.y = cy / static_cast<double>(scale);
			//in front of the surface in the middle of the canvas
			const double surface = nd.rgba[(static_cast<size_t>(cy) * width + cx) * 4 + 3] / 255.0;
			const double up = std::min(1.0, surface + 0.08), down = std::max(0.0, surface - 0.08);
			lamp.depth = space.ViewPosition(lamp.x, lamp.y, up).z < space.ViewPosition(lamp.x, lamp.y, down).z ? up : down;
			lamp.range = 120.0f;
			lamp.intensity = 1.5f;
			one.lights.push_back(lamp);
			use = &one;
			const Image rest = draw(0, false, 0.0, false, "");
			const Image low = draw(0, true, 0.0, false, "rolloff_light.png");
			one.lights[0].intensity *= 4.0f;
			const Image high = draw(0, true, 0.0, false, "rolloff_light_x4.png");
			use = &lights;
			const auto lin = [](const uint8_t v) { return static_cast<double>(fieldpbr::SrgbToLinear(v / 255.0f)); };
			size_t n = 0, over = 0, darker = 0, wouldClip = 0, clipped = 0;
			double sum = 0;
			int worst = 0;
			for (size_t i = 0; i < rest.rgba.size(); i += 4)
			{
				if (rest.rgba[i + 3] == 0)
					continue;
				for (int c = 0; c < 3; c++)
				{
					const int a = rest.rgba[i + c], b = low.rgba[i + c], h = high.rgba[i + c];
					darker += b < a - 1 || h < b - 1;
					//enough light to measure, and both pictures off white (at 255 the amount of light can't be read back)
					if (b - a < 6 || b > 250 || a > 235)
						continue;
					const double p = lin(static_cast<uint8_t>(a)), room = 1.0 - p;
					const double left = (1.0 - lin(static_cast<uint8_t>(b))) / room;
					const double added = -room * std::log(left); //the light the first picture added
					const double want = 1.0 - room * left * left * left * left;
					const int e = static_cast<int>(std::lround(255.0 * fieldpbr::LinearToSrgb(static_cast<float>(want))));
					const int d = std::abs(h - e);
					worst = std::max(worst, d);
					sum += d;
					over += d > 6;
					n++;
					//4 times that light on top of the painting passes white: cut off it would be 255
					if (p + 4.0 * added > 1.0)
					{
						wouldClip++;
						clipped += h >= 255;
					}
				}
			}
			printf("roll-off: %zu values, the light x4 against what the curve gives: max %d, mean %.3f, %.3f%% off by more than 6; "
				"%zu darker than without the light; %zu past white if cut off, %zu of them drawn white\n",
				n, worst, n ? sum / n : 0.0, n ? 100.0 * over / n : 0.0, darker, wouldClip, clipped);
			if (n < 1000 || sum / n > 1.5 || 100.0 * over / n > 1.0 || darker > 0 || wouldClip < 100 || clipped * 2 > wouldClip)
				failures++;
		}
		else
			printf("roll-off: no %s_ND.png at the canvas size, check skipped\n", name.c_str());
	}
	//--- the look (FIELD_PBR_LOOK): relight, highlights, AO on the painting
	{
		if (hasRoom)
		{
			const int cx = width / 2, cy = height / 2;
			const auto lin = [](const uint8_t v) { return static_cast<double>(fieldpbr::SrgbToLinear(v / 255.0f)); };
			const auto srgb = [](const double v)
			{
				return static_cast<int>(std::lround(255.0 * fieldpbr::LinearToSrgb(static_cast<float>(std::clamp(v, 0.0, 1.0)))));
			};
			//a painted lamp (baked, no light of its own added) in front of the surface in the middle of the canvas
			fieldpbr::Lights one;
			one.night = lights.night;
			fieldpbr::Light lamp;
			lamp.name = "painted lamp";
			lamp.baked = true;
			lamp.intensity = 0.0f;
			lamp.color[0] = 1.0f;
			lamp.color[1] = 0.9f;
			lamp.color[2] = 0.75f;
			lamp.x = cx / static_cast<double>(scale);
			lamp.y = cy / static_cast<double>(scale);
			const double surface = room.rgba[(static_cast<size_t>(cy) * width + cx) * 4 + 3] / 255.0;
			const double up = std::min(1.0, surface + 0.08), down = std::max(0.0, surface - 0.08);
			lamp.depth = space.ViewPosition(lamp.x, lamp.y, up).z < space.ViewPosition(lamp.x, lamp.y, down).z ? up : down;
			lamp.range = 140.0f;
			one.lights.push_back(lamp);
			use = &one;
			const Image rest = draw(0, false, 0.0, false, "");
			//what the relight does to each pixel, worked out here from the field's _ND the way the shader does it
			const auto relightFactor = [&](const int x, const int y, const float* pos, const float* paint, const int count, const double share,
				const double shadeScale)
			{
				const uint8_t* nd = room.rgba.data() + (static_cast<size_t>(y) * width + x) * 4;
				const fieldpbr::Vec3 p = space.ViewPosition((x + 0.5) / scale, (y + 0.5) / scale, nd[3] / 255.0);
				const fieldpbr::Vec3 n = fieldpbr::Normalize({ nd[0] / 255.0 * 2 - 1, 1 - nd[1] / 255.0 * 2, 1 - nd[2] / 255.0 * 2 });
				double weightSum = 0, shadeSum = 0;
				for (int i = 0; i < count; i++)
				{
					const double w0 = std::fabs(paint[i * 4 + 3]);
					if (w0 <= 0)
						continue;
					const fieldpbr::Vec3 toLight = fieldpbr::Vec3{ pos[i * 4], pos[i * 4 + 1], pos[i * 4 + 2] } - p;
					const double dd = fieldpbr::Dot(toLight, toLight), x2 = dd / (static_cast<double>(pos[i * 4 + 3]) * pos[i * 4 + 3]);
					if (x2 >= 1)
						continue;
					const double weight = w0 * (1 - x2 * x2) * (1 - x2 * x2) / (1 + 8 * x2);
					shadeSum += weight * std::clamp((fieldpbr::Dot(n, toLight) / std::sqrt(dd) + 0.5) / 1.5, 0.0, 1.0);
					weightSum += weight;
				}
				if (weightSum <= 0)
					return 1.0;
				return std::max(1.0 + share * ((fieldpbr::RELIGHT_REST + shadeSum * shadeScale) / (fieldpbr::RELIGHT_REST + weightSum) - 1.0), 0.05);
			};
			const auto against = [&](const Image& got, const char* what, const auto& factorAt, const double meanLimit)
			{
				int worst = 0;
				double sum = 0, factorSum = 0;
				size_t n = 0, over = 0, changed = 0;
				for (int y = 0; y < height; y++)
					for (int x = 0; x < width; x++)
					{
						const size_t i = (static_cast<size_t>(y) * width + x) * 4;
						if (rest.rgba[i + 3] == 0)
							continue;
						const double factor = factorAt(x, y);
						int d = 0;
						for (int c = 0; c < 3; c++)
							d = std::max(d, std::abs(got.rgba[i + c] - srgb(lin(rest.rgba[i + c]) * factor)));
						worst = std::max(worst, d);
						sum += d;
						factorSum += factor;
						over += d > 3;
						changed += std::fabs(factor - 1.0) > 0.02;
						n++;
					}
				printf("look, %s: %zu pixels (%.0f%% changed), against the shader's sums worked out here: max %d, mean %.3f, %.4f%% off by more "
					"than 3; the field's brightness x %.3f\n", what, n, n ? 100.0 * changed / n : 0.0, worst, n ? sum / n : 0.0,
					n ? 100.0 * over / n : 0.0, n ? factorSum / n : 0.0);
				if (n == 0 || sum / n > meanLimit || 100.0 * over / n > 1.0)
					failures++;
				return n ? factorSum / n : 0.0;
			};
			//the lamp as the shader gets it
			float lampPos[4], lampPaint[4] = { lamp.color[0], lamp.color[1], lamp.color[2], 1.0f };
			{
				const fieldpbr::Vec3 p = space.ViewPosition(lamp.x, lamp.y, lamp.depth);
				lampPos[0] = static_cast<float>(p.x);
				lampPos[1] = static_cast<float>(p.y);
				lampPos[2] = static_cast<float>(p.z);
				lampPos[3] = static_cast<float>(space.ViewLength(lamp.range, p.z));
			}
			//every dial at 0, lamps or not: the painting, exactly
			{
				const Image off = draw(0, true, 0.0, false, "");
				size_t different = 0;
				for (size_t i = 0; i < off.rgba.size(); i++)
					different += off.rgba[i] != rest.rgba[i];
				printf("look, every dial at 0 with a painted lamp: %zu values differ from the painting\n", different);
				if (different > 0)
					failures++;
			}
			//relight: each pixel times what its normal says, and the field as a whole as bright as before
			for (const float share : { 1.0f, 0.5f })
			{
				lookNow = {};
				lookNow.relight = share;
				const Image relit = draw(0, true, 0.0, false, share == 1.0f ? "look_relight.png" : "look_relight_half.png");
				const double scaleUsed = lastScale;
				const double mean = against(relit, share == 1.0f ? "relight 1" : "relight 0.5",
					[&](const int x, const int y) { return relightFactor(x, y, lampPos, lampPaint, 1, share, scaleUsed); }, 1.0);
				if (std::fabs(mean - 1.0) > 0.06)
				{
					printf("look, relight: the field's brightness moved (scale %.3f)\n", scaleUsed);
					failures++;
				}
			}
			//AO on the painting: times the AO map (not where a pixel glows). ao and the glows as the shader reads them
			{
				lookNow = {};
				const Image aoMap = draw(5, false, 0.0, false, ""), glowsMap = draw(17, false, 0.0, false, "");
				size_t shaded = 0;
				for (size_t i = 0; i < aoMap.rgba.size(); i += 4)
					shaded += aoMap.rgba[i + 3] != 0 && aoMap.rgba[i] < 250;
				if (shaded == 0)
					printf("look, AO: the field has no AO map with anything in it (_ORME), check skipped\n");
				else
				{
					lookNow.ao = 1.0f;
					const Image dark = draw(0, true, 0.0, false, "look_ao.png");
					against(dark, "AO 1", [&](const int x, const int y)
					{
						const size_t i = (static_cast<size_t>(y) * width + x) * 4;
						const double ao = aoMap.rgba[i] / 255.0, e = glowsMap.rgba[i] / 255.0;
						return ao + (1.0 - ao) * e;
					}, 1.0);
				}
			}
			//highlights: the painted lamp shines on what faces it and never darkens anything; nothing where it doesn't reach
			{
				lookNow = {};
				lookNow.highlights = 1.0f;
				const Image shiny = draw(0, true, 0.0, false, "look_highlights.png");
				size_t darker = 0, brighter = 0, outside = 0;
				const double reach = lamp.range * scale * 3.0;
				for (int y = 0; y < height; y++)
					for (int x = 0; x < width; x++)
					{
						const size_t i = (static_cast<size_t>(y) * width + x) * 4;
						if (rest.rgba[i + 3] == 0)
							continue;
						int more = 0;
						for (int c = 0; c < 3; c++)
						{
							darker += shiny.rgba[i + c] < rest.rgba[i + c];
							more = std::max(more, shiny.rgba[i + c] - rest.rgba[i + c]);
						}
						brighter += more > 1;
						outside += more > 1 && std::hypot(x - cx, y - cy) > reach;
					}
				printf("look, highlights 1: %zu pixels brighter, %zu values darker, %zu brighter far outside the lamp's range\n", brighter, darker,
					outside);
				if (brighter == 0 || darker > 0 || outside > 0)
					failures++;
			}
			lookNow = {};
			use = &lights;
		}
		else
			printf("look: no %s_ND.png at the canvas size, checks skipped\n", name.c_str());
	}
	//--- the moving lights (FIELD_MOVING_LIGHTS): the sun and the party light, after the field's own lights
	if (hasRoom)
	{
		fieldpbr::Lights none;
		none.night = lights.night;
		use = &none;
		const Image rest = draw(0, false, 0.0, false, "");
		const auto mean = [&](const Image& image)
		{
			double sum = 0;
			size_t n = 0;
			for (size_t i = 0; i < image.rgba.size(); i += 4)
				if (rest.rgba[i + 3] != 0)
				{
					sum += fieldpbr::SrgbToLinear(image.rgba[i] / 255.0f) + fieldpbr::SrgbToLinear(image.rgba[i + 1] / 255.0f)
						+ fieldpbr::SrgbToLinear(image.rgba[i + 2] / 255.0f);
					n++;
				}
			return n ? sum / n : 0.0;
		};
		const auto difference = [&](const Image& a, const Image& b, size_t* brighter = nullptr, size_t* darker = nullptr)
		{
			double sum = 0;
			size_t n = 0, up = 0, down = 0;
			for (size_t i = 0; i < a.rgba.size(); i += 4)
				if (rest.rgba[i + 3] != 0)
				{
					for (int c = 0; c < 3; c++)
					{
						sum += std::abs(a.rgba[i + c] - b.rgba[i + c]);
						up += a.rgba[i + c] > b.rgba[i + c] + 1;
						down += a.rgba[i + c] < b.rgba[i + c] - 1;
					}
					n += 3;
				}
			if (brighter)
				*brighter = up;
			if (darker)
				*darker = down;
			return n ? sum / n : 0.0;
		};
		//the sun: with the relight dial the painted light follows it across the day; the field keeps its brightness
		fieldpbr::Sun sun;
		sun.on = 1;
		fieldpbr::Sun shadeOnly = sun;
		shadeOnly.intensity = 0.0f;
		lookNow = {};
		lookNow.relight = 1.0f;
		movingNow = { fieldpbr::SunLight(shadeOnly, settings, fieldpbr::FieldTime::Cycle, { false, 0.2 }, 0.0, fieldCentre, fieldExtent, 1.0) };
		const Image morning = draw(0, true, 0.0, false, "sun_morning.png");
		movingNow = { fieldpbr::SunLight(shadeOnly, settings, fieldpbr::FieldTime::Cycle, { false, 0.8 }, 0.0, fieldCentre, fieldExtent, 1.0) };
		const Image evening = draw(0, true, 0.0, false, "sun_evening.png");
		const double restMean = mean(rest), morningMean = mean(morning), eveningMean = mean(evening), swing = difference(morning, evening);
		printf("sun, relight 1: morning x %.3f and evening x %.3f the painting's brightness; morning to evening the picture changes by %.2f "
			"levels on average\n", morningMean / restMean, eveningMean / restMean, swing);
		if (!movingNow[0].on || std::fabs(morningMean / restMean - 1.0) > 0.08 || std::fabs(eveningMean / restMean - 1.0) > 0.08 || swing < 1.0)
			failures++;
		//without the relight dial and with no light of its own, the sun changes nothing
		lookNow = {};
		if (difference(draw(0, true, 0.0, false, ""), rest) != 0.0)
		{
			printf("sun: with the look off and no intensity the picture changed\n");
			failures++;
		}
		//its own light only adds
		movingNow = { fieldpbr::SunLight(sun, settings, fieldpbr::FieldTime::Cycle, { false, 0.2 }, 0.0, fieldCentre, fieldExtent, 1.0) };
		{
			size_t brighter = 0, darker = 0;
			const Image added = draw(0, true, 0.0, false, "sun_added.png");
			difference(added, rest, &brighter, &darker);
			printf("sun, its own light (intensity %.2f): %zu values brighter, %zu darker\n", sun.intensity, brighter, darker);
			if (brighter == 0 || darker > 0)
				failures++;
		}
		//walls of the room keep it out: with wall checks nothing gets brighter than without
		lookNow.relight = 1.0f;
		{
			const Image open = draw(0, true, 0.0, false, "");
			wallSteps = 6;
			const Image walled = draw(0, true, 0.0, false, "sun_walls.png");
			wallSteps = 0;
			size_t brighter = 0, darker = 0;
			difference(walled, open, &brighter, &darker);
			printf("sun, wall checks (6 steps): %zu values darker than without them (the room's own shadows), %zu brighter\n", darker, brighter);
			if (brighter > 0)
				failures++;
		}
		lookNow = {};
		//the party light: light around its carrier, nothing far from it
		{
			const int cx = width / 2, cy = height / 2;
			fieldpbr::LeaderCandidate leader;
			const double surface = room.rgba[(static_cast<size_t>(cy) * width + cx) * 4 + 3] / 255.0;
			leader.center = space.ViewPosition(cx / static_cast<double>(scale), cy / static_cast<double>(scale), surface);
			leader.halfHeight = space.ViewLength(24.0, leader.center.z); //a character 48 native pixels tall
			leader.center = leader.center * ((leader.center.z - leader.halfHeight) / leader.center.z); //standing in front of the surface
			leader.up = fieldpbr::Normalize(settings.up);
			fieldpbr::PartyLight party;
			movingNow = { fieldpbr::PartyLightFor(party, leader, space, 2, 0.0, 1.0) };
			const Image carried = draw(0, true, 0.0, false, "party_light.png");
			double nearSum = 0, farSum = 0;
			size_t nearCount = 0, farCount = 0, darker = 0;
			const double reach = party.range * scale;
			for (int y = 0; y < height; y++)
				for (int x = 0; x < width; x++)
				{
					const size_t i = (static_cast<size_t>(y) * width + x) * 4;
					if (rest.rgba[i + 3] == 0)
						continue;
					double more = 0;
					for (int c = 0; c < 3; c++)
					{
						more += carried.rgba[i + c] - rest.rgba[i + c];
						darker += carried.rgba[i + c] < rest.rgba[i + c];
					}
					const double d = std::hypot(x - cx, y - cy);
					if (d < reach * 0.4)
					{
						nearSum += more;
						nearCount++;
					}
					else if (d > reach * 2.0)
					{
						farSum += more;
						farCount++;
					}
				}
			printf("party light: +%.2f levels around its carrier, +%.3f far from it, %zu values darker\n", nearCount ? nearSum / nearCount / 3 : 0.0,
				farCount ? farSum / farCount / 3 : 0.0, darker);
			if (!movingNow[0].on || nearCount == 0 || nearSum / nearCount / 3 < 2.0 || (farCount && farSum / farCount / 3 > 0.05) || darker > 0)
				failures++;
		}
		movingNow.clear();
		use = &lights;
	}
	//--- how long the shader takes (BENCH=<frames>): the whole canvas, software rendering here, so only the ratios mean much
	if (const char* bench = std::getenv("BENCH"); bench != nullptr && hasRoom && std::atoi(bench) > 0)
	{
		const int frames = std::atoi(bench);
		const int cx = width / 2, cy = height / 2;
		//a field like fieldprep leaves it: two painted lamps that add a little (intensity 0.2), and one of yours
		fieldpbr::Lights typical;
		typical.night = lights.night;
		for (int k = 0; k < 3; k++)
		{
			fieldpbr::Light l;
			l.baked = k < 2;
			l.source = k < 2 ? fieldpbr::LightSource::Found : fieldpbr::LightSource::Yours;
			l.intensity = k < 2 ? 0.2f : 0.8f;
			l.x = (cx + (k - 1) * width / 4) / static_cast<double>(scale);
			l.y = (cy + (k == 1 ? -1 : 1) * height / 6) / static_cast<double>(scale);
			l.depth = std::clamp(room.rgba[(static_cast<size_t>(cy) * width + cx) * 4 + 3] / 255.0 + (settings.brightIsNear ? 0.08 : -0.08), 0.0, 1.0);
			l.range = k < 2 ? 160.0f : 70.0f;
			typical.lights.push_back(l);
		}
		fieldpbr::Sun sun;
		sun.on = 1;
		fieldpbr::LeaderCandidate leader;
		leader.center = space.ViewPosition(cx / static_cast<double>(scale), cy / static_cast<double>(scale),
			room.rgba[(static_cast<size_t>(cy) * width + cx) * 4 + 3] / 255.0);
		leader.halfHeight = space.ViewLength(24.0, leader.center.z);
		leader.up = fieldpbr::Normalize(settings.up);
		const std::vector<fieldpbr::MovingLight> both = {
			fieldpbr::SunLight(sun, settings, fieldpbr::FieldTime::Cycle, { false, 0.3 }, 0.0, fieldCentre, fieldExtent, 1.0),
			fieldpbr::PartyLightFor(fieldpbr::PartyLight(), leader, space, 2, 0.0, 1.0) };
		fieldpbr::Look full;
		full.relight = full.highlights = full.ao = 0.5f;
		struct Setup
		{
			const char* what;
			const fieldpbr::Lights* lights;
			fieldpbr::Look look;
			bool moving;
			int walls;
		};
		fieldpbr::Lights none;
		const Setup setups[] = {
			{ "no lights (the painting)", &none, {}, false, 0 },
			{ "3 lights, low (no wall checks)", &typical, {}, false, 0 },
			{ "3 lights + the look, low", &typical, full, false, 0 },
			{ "3 lights + the look + sun and party light, low", &typical, full, true, 0 },
			{ "3 lights, medium (6-step wall checks)", &typical, {}, false, 6 },
			{ "3 lights + the look, medium", &typical, full, false, 6 },
			{ "3 lights + the look + sun and party light, medium", &typical, full, true, 6 },
			{ "3 lights, high (12-step wall checks)", &typical, {}, false, 12 },
			{ "3 lights + the look + sun and party light, high", &typical, full, true, 12 },
		};
		timing = true;
		printf("bench: %dx%d, %d frame(s) per setup\n", width, height, frames);
		for (const Setup& setup : setups)
		{
			use = setup.lights;
			lookNow = setup.look;
			movingNow = setup.moving ? both : std::vector<fieldpbr::MovingLight>();
			wallSteps = setup.walls;
			draw(0, true, 1.25, true, "");
			glFinish();
			const auto started = std::chrono::steady_clock::now();
			for (int f = 0; f < frames; f++)
				draw(0, true, 1.25, true, "");
			glFinish();
			const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count() / frames;
			printf("bench: %7.2f ms  %s\n", ms, setup.what);
		}
		timing = false;
		use = &lights;
		lookNow = {};
		movingNow.clear();
		wallSteps = 0;
	}
	//--- the night (DAY_NIGHT)
	{
		//no lamps on at night: the painting times [night] level x tint, everywhere
		fieldpbr::Lights dark = lights;
		dark.lights.clear();
		dark.night.level = 0.25f;
		dark.night.tint[0] = 0.5f;
		dark.night.tint[1] = 0.75f;
		dark.night.tint[2] = 1.0f;
		dark.night.lamps = 0.9f;
		use = &dark;
		//the emissive map as the shader reads it (FIELD_PBR_VIEW 17): glowing pixels keep their own light at night, so the
		//checks of the dimmed painting leave them out; they have their own check below
		const Image glowMap = draw(17, false, 0.0, false, "");
		//(a picture of the glows also adds light where it covers nothing: its halos)
		const Image* halos = fieldMap[fieldpbr::EMISSIVE].empty() ? nullptr : load(fieldMap[fieldpbr::EMISSIVE]);
		if (halos != nullptr && halos->rgba.size() != glowMap.rgba.size())
			halos = nullptr;
		const auto glows = [&](const int x, const int y)
		{
			const size_t i = (static_cast<size_t>(y) * width + x) * 4;
			return glowMap.rgba[i] > 0 || (halos != nullptr && (halos->rgba[i] | halos->rgba[i + 1] | halos->rgba[i + 2]) != 0);
		};
		night = 1.0;
		const auto expected = [&](const uint8_t painted, const int c, const double left)
		{
			const double v = fieldpbr::SrgbToLinear(painted / 255.0f) * left * (c == 0 ? dark.night.tint[0] : c == 1 ? dark.night.tint[1] : dark.night.tint[2]);
			return static_cast<int>(std::lround(255.0 * fieldpbr::LinearToSrgb(static_cast<float>(v))));
		};
		const auto compare = [&](const Image& got, const char* what, const auto& leftAt, const auto& counts)
		{
			int worst = 0;
			double sum = 0;
			size_t n = 0, over = 0;
			for (int y = 0; y < height; y++)
				for (int x = 0; x < width; x++)
				{
					const size_t i = (static_cast<size_t>(y) * width + x) * 4;
					if (composite.rgba.empty() || composite.rgba[i + 3] == 0 || !counts(x, y) || glows(x, y))
						continue;
					int d = 0;
					for (int c = 0; c < 3; c++)
					{
						const double left = leftAt(x, y, c);
						const int e = left >= 1.0 ? composite.rgba[i + c] : expected(composite.rgba[i + c], c, left);
						d = std::max(d, std::abs(got.rgba[i + c] - e));
					}
					worst = std::max(worst, d);
					sum += d;
					over += d > 3;
					n++;
				}
			printf("night, %s: max %d, mean %.3f, %.4f%% of pixels off by more than 3 (%zu pixels)\n", what, worst, n ? sum / n : 0.0,
				n ? 100.0 * over / n : 0.0, n);
			return n > 0 && sum / n <= 1.0 && 100.0 * over / n <= 0.5;
		};
		const Image allDark = draw(0, false, 0.0, false, "night_no_lamps.png");
		if (!compare(allDark, "no lamps (the painting x level x tint)", [&](int, int, int) { return dark.night.level; }, [](int, int) { return true; }))
			failures++;
		//a night lamp in the middle of the canvas, on the surface there: around it the painting keeps [night] lamps of its light
		Image nd;
		std::string ndError;
		if (fieldprep::LoadRgba(folder + name + "_ND.png", nd, ndError) && nd.width == width && nd.height == height)
		{
			const int cx = width / 2, cy = height / 2;
			fieldpbr::Light lamp;
			lamp.name = "test lamp";
			lamp.time = fieldpbr::LightTime::Night;
			lamp.baked = true;
			lamp.intensity = 0.0f;
			lamp.x = cx / static_cast<double>(scale);
			lamp.y = cy / static_cast<double>(scale);
			lamp.depth = nd.rgba[(static_cast<size_t>(cy) * width + cx) * 4 + 3] / 255.0;
			lamp.range = 30.0f;
			dark.lights.push_back(lamp);
			const Image lampLit = draw(0, false, 0.0, false, "night_lamp.png");
			const double reach = 3.0 * lamp.range * scale; //screen pixels: far enough on the canvas to be out of its range
			const double keep = dark.night.lamps;
			//at the lamp: mix(level x tint, 1, lamps)
			int nearWorst = 0;
			size_t nearCount = 0;
			for (int y = cy - 1; y <= cy + 1; y++)
				for (int x = cx - 1; x <= cx + 1; x++)
				{
					const size_t i = (static_cast<size_t>(y) * width + x) * 4;
					if (composite.rgba.empty() || composite.rgba[i + 3] == 0 || glows(x, y))
						continue;
					for (int c = 0; c < 3; c++)
					{
						const double t = c == 0 ? dark.night.tint[0] : c == 1 ? dark.night.tint[1] : dark.night.tint[2];
						const double left = dark.night.level * t + (1.0 - dark.night.level * t) * keep;
						const double v = fieldpbr::SrgbToLinear(composite.rgba[i + c] / 255.0f) * left;
						const int e = static_cast<int>(std::lround(255.0 * fieldpbr::LinearToSrgb(static_cast<float>(v))));
						nearWorst = std::max(nearWorst, std::abs(lampLit.rgba[i + c] - e));
					}
					nearCount++;
				}
			printf("night, at the lamp: %zu pixels, max %d off mix(level x tint, painting, lamps)\n", nearCount, nearWorst);
			if (nearWorst > 3)
				failures++;
			if (!compare(lampLit, "far from the lamp", [&](int, int, int) { return dark.night.level; },
				[&](int x, int y) { return std::hypot(x - cx, y - cy) > reach; }))
				failures++;
			//brighter around the lamp than the night without it
			double withLamp = 0, without = 0;
			for (int y = cy - 8; y <= cy + 8; y++)
				for (int x = cx - 8; x <= cx + 8; x++)
				{
					const size_t i = (static_cast<size_t>(y) * width + x) * 4;
					if (glows(x, y))
						continue;
					withLamp += lampLit.rgba[i] + lampLit.rgba[i + 1] + lampLit.rgba[i + 2];
					without += allDark.rgba[i] + allDark.rgba[i + 1] + allDark.rgba[i + 2];
				}
			printf("night, around the lamp: %.2f x as bright as without it\n", without > 0 ? withLamp / without : 0.0);
			if (without > 0 && !(withLamp > without * 1.5))
				failures++;
		}
		else
			printf("night: no %s_ND.png at the canvas size, lamp check skipped\n", name.c_str());
		//the glows at night: where the emissive map says, the glow (the painting's colour x [emissive] tint x strength) takes
		//the dimmed painting's place, e of it: mix(painting x level x tint, glow, e). As painted (tint 1, strength 1) a
		//fully glowing pixel is the painting's own colour, never brighter
		{
			dark.lights.clear();
			dark.emissive.hue = 0.0f;
			use = &dark;
			const Image& map = glowMap;
			size_t glowing = 0;
			for (size_t i = 0; i < map.rgba.size(); i += 4)
				glowing += map.rgba[i + 3] != 0 && map.rgba[i] > 0;
			//a picture of the glows for the whole field (<field>_EC.png, at the canvas size): painting x level x tint x
			//(1 - coverage) + the picture's colour x tint x strength; half of both at "day = 0.5" in daylight
			const std::string picturePath = fieldMap[fieldpbr::EMISSIVE];
			const Image* picture = picturePath.empty() ? nullptr : load(picturePath);
			if (picture != nullptr && (composite.rgba.empty() || picture->rgba.size() != composite.rgba.size()))
			{
				printf("night glows: %s is not the composite's size, its check skipped\n", picturePath.c_str());
				picture = nullptr;
			}
			for (int pass = 0; pass < 3 && picture != nullptr; pass++)
			{
				const float tint[3] = { 1.0f, pass == 1 ? 0.5f : 1.0f, pass == 1 ? 0.25f : 1.0f };
				dark.emissive.strength = pass == 1 ? 0.5f : 1.0f;
				for (int c = 0; c < 3; c++)
					dark.emissive.tint[c] = tint[c];
				dark.emissive.day = pass == 2 ? 0.5f : 0.0f;
				night = pass == 2 ? 0.0 : 1.0;
				const double showing = pass == 2 ? 0.5 : 1.0;
				const Image got = draw(0, false, 0.0, false, pass == 0 ? "night_glow_picture.png" : pass == 1 ? "night_glow_picture_tinted.png"
					: "day_glow_picture_half.png");
				int worst = 0;
				double sum = 0;
				size_t n = 0, over = 0, covered = 0;
				for (size_t i = 0; i < got.rgba.size(); i += 4)
				{
					const uint8_t* e = picture->rgba.data() + i;
					if (composite.rgba[i + 3] == 0 || (e[0] < 8 && e[1] < 8 && e[2] < 8 && e[3] == 0))
						continue;
					const double a = e[3] / 255.0;
					covered += e[3] >= 128;
					int d = 0;
					for (int c = 0; c < 3; c++)
					{
						const double p = fieldpbr::SrgbToLinear(composite.rgba[i + c] / 255.0f);
						const double t = c == 0 ? dark.night.tint[0] : c == 1 ? dark.night.tint[1] : dark.night.tint[2];
						const double left = pass == 2 ? 1.0 : dark.night.level * t;
						const double v = p * left * (1.0 - a * showing) + fieldpbr::SrgbToLinear(e[c] / 255.0f) * tint[c] * dark.emissive.strength * showing;
						d = std::max(d, std::abs(got.rgba[i + c] - static_cast<int>(std::lround(255.0 * fieldpbr::LinearToSrgb(static_cast<float>(v))))));
					}
					worst = std::max(worst, d);
					sum += d;
					over += d > 3;
					n++;
				}
				printf("glow picture%s: %zu glowing pixels (%zu take the painting's place), max %d, mean %.3f, %.4f%% off by more than 3\n",
					pass == 0 ? " (night, as painted)" : pass == 1 ? " (night, tint 1 0.5 0.25, strength 0.5)" : " (daylight, day = 0.5)", n, covered,
					worst, n ? sum / n : 0.0, n ? 100.0 * over / n : 0.0);
				if (n == 0 || sum / n > 1.0 || 100.0 * over / n > 0.5)
					failures++;
			}
			dark.emissive.day = 0.0f;
			night = 1.0;
			if (picture != nullptr)
				glowing = 0; //the old mask's check below is for fields without a picture
			else if (glowing == 0)
				printf("night glows: the field has no emissive map (_ORME or _EC), check skipped\n");
			for (int pass = 0; pass < 2 && glowing; pass++)
			{
				const float tint[3] = { 1.0f, pass ? 0.5f : 1.0f, pass ? 0.25f : 1.0f };
				dark.emissive.strength = pass ? 0.5f : 1.0f;
				for (int c = 0; c < 3; c++)
					dark.emissive.tint[c] = tint[c];
				const Image got = draw(0, false, 0.0, false, pass ? "night_glows_tinted.png" : "night_glows.png");
				int worst = 0;
				double sum = 0;
				size_t n = 0, over = 0, brighter = 0;
				for (size_t i = 0; i < got.rgba.size(); i += 4)
				{
					if (composite.rgba.empty() || composite.rgba[i + 3] == 0 || map.rgba[i] == 0)
						continue;
					const double e = map.rgba[i] / 255.0;
					int d = 0;
					for (int c = 0; c < 3; c++)
					{
						const double p = fieldpbr::SrgbToLinear(composite.rgba[i + c] / 255.0f);
						const double t = c == 0 ? dark.night.tint[0] : c == 1 ? dark.night.tint[1] : dark.night.tint[2];
						const double v = p * dark.night.level * t * (1.0 - e) + p * tint[c] * dark.emissive.strength * e;
						d = std::max(d, std::abs(got.rgba[i + c] - static_cast<int>(std::lround(255.0 * fieldpbr::LinearToSrgb(static_cast<float>(v))))));
						brighter += got.rgba[i + c] > composite.rgba[i + c] + 2;
					}
					worst = std::max(worst, d);
					sum += d;
					over += d > 3;
					n++;
				}
				printf("night glows%s: %zu glowing pixels, max %d, mean %.3f, %.4f%% off by more than 3; %zu values brighter than painted\n",
					pass ? " (tint 1 0.5 0.25, strength 0.5)" : " (as painted)", n, worst, n ? sum / n : 0.0, n ? 100.0 * over / n : 0.0, brighter);
				if (n == 0 || sum / n > 1.0 || 100.0 * over / n > 0.5 || brighter > 0)
					failures++;
			}
			dark.emissive = lights.emissive;
		}
		//the field's own lights and [night]: night and the middle of dusk, for looking at
		use = &lights;
		night = 1.0;
		draw(0, true, 1.25, true, "night.png");
		night = 0.5;
		draw(0, true, 1.25, true, "dusk.png");
		night = 0.0;
	}
	printf("%s\n", failures ? "FAILED" : "ok");
	return failures ? 1 : 0;
}
