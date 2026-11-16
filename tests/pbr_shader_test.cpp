//Runs DEMASTER_EXP/shaders/demaster_field.frag on a field's PBR maps the way DEMASTER binds them, with Mesa's software OpenGL
//(Linux, EGL, no window), and draws the field's opaque tiles through it. Checks that with the lights at rest the shader gives
//back the paintings, and saves what it draws (lit, at rest, every FIELD_PBR_VIEW) as PNG.
//
//	pbr_shader_test <field folder> <demaster_field.frag> <output folder>
//
//Linux with Mesa (llvmpipe works):
//	g++ -std=c++20 -O2 -pthread pbr_shader_test.cpp ../ff8_demaster/field_layers.cpp ../ff8_demaster/field_pbr.cpp
//	    ../fieldprep/image_io.cpp ../fieldprep/third_party/*.cpp -lEGL -lGL -o pbr_shader_test
//The field folder needs <field>.map, the layer images and fieldprep's output (PNG).

#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include "../ff8_demaster/field_layers.h"
#include "../ff8_demaster/field_pbr.h"
#include "../fieldprep/image_io.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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
				+ (page.map[fieldpbr::ORME] && !files->second.path[fieldpbr::ORME].empty() ? 4.0f + (files->second.emissive ? 8.0f : 0.0f) : 0.0f);
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
	//night: how night it is (DAY_NIGHT); the lights on then and their weights as DEMASTER picks them (ActiveLights)
	double night = 0.0;
	const fieldpbr::Lights* use = &lights;
	const auto draw = [&](const int view, const bool lit, const double seconds, const bool smooth, const std::string& file)
	{
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		float pos[64] = {}, color[64] = {}, flicker[64] = {};
		const std::vector<fieldpbr::ActiveLight> active = fieldpbr::ActiveLights(*use, night);
		const int count = static_cast<int>(std::min<size_t>(active.size(), fieldpbr::MAX_LIGHTS));
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
		}
		glUniform4f(at("demasterNight"), static_cast<float>(night), use->night.level, use->night.lamps, 0.0f);
		glUniform4f(at("demasterNightTint"), use->night.tint[0], use->night.tint[1], use->night.tint[2], 0.0f);
		glUniform1i(at("demasterLightCount"), count);
		glUniform4fv(at("demasterLightPos"), fieldpbr::MAX_LIGHTS, pos);
		glUniform4fv(at("demasterLightColor"), fieldpbr::MAX_LIGHTS, color);
		glUniform4fv(at("demasterLightFlicker"), fieldpbr::MAX_LIGHTS, flicker);
		glUniform4f(at("demasterSpace"), static_cast<float>(space.zoom), static_cast<float>(space.originX), static_cast<float>(space.originY),
			space.model == fieldpbr::DepthModel::Inverse ? 1.0f : 0.0f);
		glUniform4f(at("demasterDepth"), static_cast<float>(space.a), static_cast<float>(space.b), static_cast<float>(space.nearest), 0.0f);
		glUniform4f(at("demasterAmbient"), lit ? lights.ambient[0] : 0, lit ? lights.ambient[1] : 0, lit ? lights.ambient[2] : 0, 0);
		glUniform4f(at("demasterEmissive"), lights.emissive.hue / 360.0f, lights.emissive.strength, 0.0f, static_cast<float>(night));
		glUniform4f(at("demasterEmissiveTint"), lights.emissive.tint[0], lights.emissive.tint[1], lights.emissive.tint[2], 0.0f);
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
					if (composite.rgba.empty() || composite.rgba[i + 3] == 0 || !counts(x, y))
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
					if (composite.rgba.empty() || composite.rgba[i + 3] == 0)
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
			if (nearCount == 0 || nearWorst > 3)
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
					withLamp += lampLit.rgba[i] + lampLit.rgba[i + 1] + lampLit.rgba[i + 2];
					without += allDark.rgba[i] + allDark.rgba[i + 1] + allDark.rgba[i + 2];
				}
			printf("night, around the lamp: %.2f x as bright as without it\n", without > 0 ? withLamp / without : 0.0);
			if (!(withLamp > without * 1.5))
				failures++;
		}
		else
			printf("night: no %s_ND.png at the canvas size, lamp check skipped\n", name.c_str());
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
