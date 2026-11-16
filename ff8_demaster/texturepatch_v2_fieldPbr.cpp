//PBR field backgrounds (FIELD_PBR): the game side. The rules (file names, lights, depth) are in field_pbr.h; fieldprep
//makes the files.
//
//A PBR layer is a layer with its own <field>_<pupu>_A and an _LF (its own or the field's). The field layer module
//(texturepatch_v2_fieldLayers.cpp) puts the layer's albedo into the game's page instead of the painting. For every such
//page this module makes three more textures with the same cells, from _LF, _ND and _ORME, and a small one that says, per
//16x16 cell, where its tile is on the field's canvas (so the shader knows where each pixel is), its .map Z, layer and how
//near what it shows is.
//
//While such a page is drawn, DEMASTER draws it with its own program (DEMASTER_EXP\shaders\demaster_field.vert/.frag)
//instead of the game's main.frag, only for that draw, and puts the game's back right after. The maps go on texture units
//1-8: _LF, _ND, _ORME, the cells, the whole field's _ND (the room, for shadows and walls between), the pictures of the
//characters (reflections), the characters' shadow maps and the characters' depth (texturepatch_v2_fieldChars.cpp makes
//those three). Lighting:
//	albedo x lightmap x (1 + flicker mask x (flicker - 1)) + AO x added light (Lambert + GGX)
//Nothing is drawn twice: the lighting happens inside the draw the game makes anyway.
//
//While drawing, the tiles also tell where the field's canvas is on the game's screen (the characters need it to be put
//back into the room), which layers move on their own (parallax: sky), and how the depth buffer follows the game's depth.

#include "config.h"
#include "coreHeader.h"
#include "opengl.h"
#include "texture.h"
#include "field_pbr_runtime.h"
#include "day_night.h"
#include "gl_state.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

using namespace fieldlayers;
using namespace fieldrt;

namespace
{
	constexpr int UNIT_LIGHTMAP = 1, UNIT_NORMAL = 2, UNIT_ORME = 3, UNIT_CELLS = 4, UNIT_ROOM = 5, UNIT_CAPTURE = 6, UNIT_SHADOW = 7,
		UNIT_CHAR_DEPTH = 8;
	constexpr int UNIT_FIRST = 1, UNIT_LAST = 8;
	constexpr float FLAG_PBR = 1.0f, FLAG_NORMAL = 2.0f, FLAG_ORME = 4.0f, FLAG_EMISSIVE = 8.0f; //EMISSIVE: the ORM map is an _ORME
	constexpr const char* PROBES_SUFFIX = "_PROBES.dds";

	bool ReadBytes(const std::string& path, std::string& text)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return false;
		std::ostringstream out;
		out << in.rdbuf();
		text = out.str();
		return true;
	}

	std::filesystem::file_time_type FileTime(const std::string& path)
	{
		std::error_code error;
		if (path.empty() || !std::filesystem::is_regular_file(path, error))
			return {};
		return std::filesystem::last_write_time(path, error);
	}

	//<field>_<language><suffix> if there is one, else <field><suffix>
	std::string FieldFile(const std::string& folder, const std::string& name, const std::string& language, const char* suffix)
	{
		std::error_code error;
		if (!language.empty())
		{
			const std::string path = fieldpbr::FieldStem(folder, name, language) + suffix;
			if (std::filesystem::is_regular_file(path, error))
				return path;
		}
		const std::string path = fieldpbr::FieldStem(folder, name, "") + suffix;
		return std::filesystem::is_regular_file(path, error) ? path : std::string();
	}

	//an image <stem><suffix> with the language, else without
	std::string FindImageStem(const std::string& folder, const std::string& name, const std::string& language, const bool layer,
		const uint32_t pupu, const char* suffix)
	{
		for (int withLanguage = language.empty() ? 0 : 1; withLanguage >= 0; withLanguage--)
		{
			const std::string& l = withLanguage ? language : std::string();
			const std::string stem = (layer ? fieldpbr::LayerStem(folder, name, l, pupu) : fieldpbr::FieldStem(folder, name, l)) + suffix;
			if (!ResolveImage(stem).path.empty())
				return stem;
		}
		return {};
	}

	void LoadSettings(const std::string& path, const Layout* layout, fieldpbr::Settings& settings, const std::string& name)
	{
		fieldpbr::Settings fresh;
		if (layout != nullptr)
			fresh.space = fieldpbr::EstimatedSpace(layout->shiftX, layout->shiftY, true);
		else
			fresh.space = settings.space;
		std::string text, warnings;
		if (!path.empty() && ReadBytes(path, text))
			fieldpbr::ParseSettings(text, fresh, warnings);
		if (!warnings.empty())
			Log("FIELD_PBR: %s: %s:\n%s", name.c_str(), path.c_str(), warnings.c_str());
		settings = fresh;
	}

	void LoadLights(const std::string& path, fieldpbr::Lights& lights, const std::string& name)
	{
		fieldpbr::Lights fresh;
		std::string text, warnings;
		if (!path.empty() && ReadBytes(path, text))
			fieldpbr::ParseLights(text, fresh, warnings);
		if (!warnings.empty())
			Log("FIELD_PBR: %s: %s:\n%s", name.c_str(), path.c_str(), warnings.c_str());
		//all of them: which are on (and how much) follows the time of day, every frame (Lights())
		lights = fresh;
	}
}

bool fieldrt::PbrCanvas(const std::string& folder, const std::string& name, const std::string& language, Frame& canvas)
{
	const std::string path = FieldFile(folder, name, language, fieldpbr::SETTINGS_SUFFIX);
	std::string text, warnings;
	if (path.empty() || !ReadBytes(path, text))
		return false;
	fieldpbr::Settings settings;
	fieldpbr::ParseSettings(text, settings, warnings);
	if (!settings.hasCanvas)
		return false;
	canvas = { settings.canvas[0], settings.canvas[1], settings.canvas[2], settings.canvas[3] };
	return true;
}

bool fieldrt::PbrFieldTime(const std::string& folder, const std::string& name, const std::string& language, fieldpbr::FieldTime& time)
{
	const std::string path = FieldFile(folder, name, language, fieldpbr::SETTINGS_SUFFIX);
	std::string text, warnings;
	if (path.empty() || !ReadBytes(path, text))
		return false;
	fieldpbr::Settings settings;
	fieldpbr::ParseSettings(text, settings, warnings);
	time = settings.time;
	return true;
}

//game thread, when a field's .map is read
std::shared_ptr<const PbrField> fieldrt::PbrFindFiles(const std::string& folder, const std::string& name, const std::string& language,
	const Layout& layout, const std::vector<std::pair<uint32_t, int>>& layers)
{
	auto field = std::make_shared<PbrField>();
	field->name = name;
	field->language = language;
	field->folder = folder;
	std::array<std::string, fieldpbr::KIND_COUNT> fieldStem;
	for (int k = fieldpbr::LIGHTMAP; k < fieldpbr::KIND_COUNT; k++)
		fieldStem[k] = FindImageStem(folder, name, language, false, 0, fieldpbr::KindSuffix(static_cast<fieldpbr::Kind>(k)));
	//an ORM map from before v15.7 (_ORMH: its alpha the height, which nothing uses) still gives AO, roughness and metallic
	const bool fieldEmissive = !fieldStem[fieldpbr::ORME].empty();
	if (!fieldEmissive)
		fieldStem[fieldpbr::ORME] = FindImageStem(folder, name, language, false, 0, fieldpbr::OLD_ORM_SUFFIX);
	std::vector<std::string> noLightmap;
	for (const auto& [pupu, blend] : layers)
	{
		if (blend != 4)
			continue; //only opaque layers: glows and shadows are drawn as they are
		PbrField::Layer layer;
		layer.stem[fieldpbr::ALBEDO] = FindImageStem(folder, name, language, true, pupu, fieldpbr::KindSuffix(fieldpbr::ALBEDO));
		if (layer.stem[fieldpbr::ALBEDO].empty())
			continue;
		for (int k = fieldpbr::LIGHTMAP; k < fieldpbr::KIND_COUNT; k++)
		{
			layer.stem[k] = FindImageStem(folder, name, language, true, pupu, fieldpbr::KindSuffix(static_cast<fieldpbr::Kind>(k)));
			if (k == fieldpbr::LIGHTMAP)
				layer.ownLightmap = !layer.stem[k].empty();
			if (k == fieldpbr::ORME)
			{
				layer.emissive = !layer.stem[k].empty();
				if (layer.stem[k].empty())
					layer.stem[k] = FindImageStem(folder, name, language, true, pupu, fieldpbr::OLD_ORM_SUFFIX);
				if (layer.stem[k].empty())
					layer.emissive = fieldEmissive;
			}
			if (layer.stem[k].empty())
				layer.stem[k] = fieldStem[k];
		}
		if (layer.stem[fieldpbr::LIGHTMAP].empty())
		{
			noLightmap.push_back(PupuHex(pupu));
			continue;
		}
		for (int k = 0; k < fieldpbr::KIND_COUNT; k++)
			if (!layer.stem[k].empty())
				layer.path[k] = ResolveImage(layer.stem[k]).path;
		field->layers.emplace(pupu, layer);
	}
	if (!noLightmap.empty() && FIELD_DEBUG)
	{
		std::string list;
		for (size_t i = 0; i < noLightmap.size() && i < 16; i++)
			list += " " + noLightmap[i];
		Log("FIELD_PBR: %s: %zu layer(s) have an _A but no _LF (theirs or %s_LF): drawn from their paintings:%s\n", name.c_str(),
			noLightmap.size(), name.c_str(), list.c_str());
	}
	if (field->layers.empty())
		return nullptr;
	field->settingsPath = FieldFile(folder, name, language, fieldpbr::SETTINGS_SUFFIX);
	field->lightsPath = FieldFile(folder, name, language, fieldpbr::LIGHTS_SUFFIX);
	field->roomStem = fieldStem[fieldpbr::NORMAL_DEPTH];
	field->probesPath = FieldFile(folder, name, language, PROBES_SUFFIX);
	field->canvasWidth = layout.width;
	field->canvasHeight = layout.height;
	LoadSettings(field->settingsPath, &layout, field->settings, name);
	LoadLights(field->lightsPath, field->lights, name);
	return field;
}

//--- shaders and the game's programs (shared with the character module)

namespace
{
	bool ReadShader(const char* file, std::string& source)
	{
		const std::string path = std::string(DIRECT_IO_EXPORT_DIR) + "shaders\\" + file;
		return ReadBytes(path, source) && !source.empty();
	}

	//defines: lines put right after the #version line (#extension, #define)
	GLuint Compile(const GLenum type, const char* file, std::string& error, const char* defines = nullptr)
	{
		std::string source;
		if (!ReadShader(file, source))
		{
			error = std::string("DEMASTER_EXP\\shaders\\") + file + " is missing";
			return 0;
		}
		if (defines != nullptr && *defines != 0)
		{
			size_t at = 0;
			if (source.compare(0, 8, "#version") == 0)
			{
				at = source.find('\n');
				at = at == std::string::npos ? source.size() : at + 1;
			}
			source.insert(at, defines);
		}
		const GLuint shader = glCreateShader(type);
		const char* text = source.c_str();
		glShaderSource(shader, 1, &text, nullptr);
		glCompileShader(shader);
		GLint ok = GL_FALSE;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
		if (ok != GL_TRUE)
		{
			char log[700]{};
			glGetShaderInfoLog(shader, sizeof(log) - 1, nullptr, log);
			error = std::string(file) + " does not compile: " + log;
			glDeleteShader(shader);
			return 0;
		}
		return shader;
	}
}

//straight to the driver: DEMASTER's own glTexImage2D hook would hash (and maybe replace) it
void fieldrt::RawTexImage(const GLint internalFormat, const GLsizei width, const GLsizei height, const GLenum format, const GLenum type,
	const void* data, const GLint level)
{
	if (ogl_tex_image2d != nullptr)
		reinterpret_cast<void* (__stdcall*)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)>(ogl_tex_image2d)
		(GL_TEXTURE_2D, level, internalFormat, width, height, 0, format, type, data);
	else
		glTexImage2D(GL_TEXTURE_2D, level, internalFormat, width, height, 0, format, type, data);
}

void fieldrt::RawTexSubImage(const GLint level, const GLint x, const GLint y, const GLsizei width, const GLsizei height, const GLenum format,
	const GLenum type, const void* data)
{
	if (ogl_subTexImage2D != nullptr)
		reinterpret_cast<void(__stdcall*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)>(ogl_subTexImage2D)
		(GL_TEXTURE_2D, level, x, y, width, height, format, type, data);
	else
		glTexSubImage2D(GL_TEXTURE_2D, level, x, y, width, height, format, type, data);
}

GLuint fieldrt::BuildProgram(const char* vertexFile, const char* fragmentFile, std::string& error, const char* defines)
{
	CheckGlew();
	const GLuint vertex = Compile(GL_VERTEX_SHADER, vertexFile, error, defines);
	if (vertex == 0)
		return 0;
	const GLuint fragment = Compile(GL_FRAGMENT_SHADER, fragmentFile, error);
	if (fragment == 0)
	{
		glDeleteShader(vertex);
		return 0;
	}
	const GLuint program = glCreateProgram();
	glAttachShader(program, vertex);
	glAttachShader(program, fragment);
	glLinkProgram(program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	GLint ok = GL_FALSE;
	glGetProgramiv(program, GL_LINK_STATUS, &ok);
	if (ok != GL_TRUE)
	{
		char log[700]{};
		glGetProgramInfoLog(program, sizeof(log) - 1, nullptr, log);
		error = std::string(vertexFile) + " + " + fragmentFile + " do not link: " + log;
		glDeleteProgram(program);
		return 0;
	}
	return program;
}

const GameProgram& fieldrt::GameProgramOf(const GLint program)
{
	static std::unordered_map<GLint, GameProgram> programs;
	GameProgram& p = programs[program];
	if (p.looked || program == 0)
		return p;
	p.looked = true;
	const GLuint id = static_cast<GLuint>(program);
	GLuint shaders[8]{};
	GLsizei count = 0;
	if (glGetAttachedShaders != nullptr && glGetShaderSource != nullptr)
		glGetAttachedShaders(id, 8, &count, shaders);
	std::string fragment;
	for (GLsizei i = 0; i < count; i++)
	{
		GLint type = 0, length = 0;
		glGetShaderiv(shaders[i], GL_SHADER_TYPE, &type);
		glGetShaderiv(shaders[i], GL_SHADER_SOURCE_LENGTH, &length);
		if (type != GL_FRAGMENT_SHADER || length <= 0 || length > (1 << 20))
			continue;
		fragment.assign(static_cast<size_t>(length), '\0');
		glGetShaderSource(shaders[i], length, nullptr, fragment.data());
		fragment.resize(strlen(fragment.c_str()));
	}
	//main.frag first: the game's, DEMASTER's, or an older DEMASTER one (v13/v14 had the PBR code inside, with the same
	//uniform names as demaster_field.frag); DEMASTER's own programs never have effectoption
	if (fragment.empty())
		p.kind = ProgramKind::Unknown;
	else if (fragment.find("effectoption") != std::string::npos)
	{
		p.kind = ProgramKind::Main;
		if (fragment.find("demasterPbr") != std::string::npos)
			LogOnce("old-main-frag", "FIELD_PBR: the game's DEMASTER_EXP\\shaders\\main.frag is from v13/v14 (it has the PBR code inside). It "
				"still works, but copy main.frag and the demaster_* files from this release's DEMASTER_EXP\\shaders\\.\n");
	}
	else if (fragment.find("uniform sampler2D demasterCells") != std::string::npos || fragment.find("uniform vec4 demasterCharMode") != std::string::npos
		|| fragment.find("DEMASTER (FIELD_PBR_SHADOWS)") != std::string::npos)
		p.kind = ProgramKind::Demaster;
	else if (fragment.find("ambientColor0") == std::string::npos && fragment.find("ambientColor") != std::string::npos)
		p.kind = ProgramKind::Character;
	else if (fragment.find("texture2D") == std::string::npos)
		p.kind = ProgramKind::NoTexture;
	else
		p.kind = ProgramKind::Other;
	p.vertexType = glGetUniformLocation(id, "vertextype");
	p.blendMode = glGetUniformLocation(id, "blend_mode");
	p.ambient = glGetUniformLocation(id, "ambientColor");
	return p;
}

//--- renderer side

namespace
{
	struct CellInfo
	{
		bool valid = false;
		int layerX = 0, layerY = 0, layerId = 0;
	};

	struct PbrPage
	{
		uint64_t entry = 0;
		PageJob job; //the game page's job, without the game's pixels (to make the maps again)
		PageSize size; //of the albedo page
		GLuint map[fieldpbr::KIND_COUNT] = {}; //LIGHTMAP, NORMAL_DEPTH, ORMH (ALBEDO is the game's texture)
		PageSize mapSize[fieldpbr::KIND_COUNT];
		GLuint cells = 0;
		int cellsX = 16, cellsY = 16;
		std::vector<const CellStamp*> stamps; //PBR cells (pointers into job.plan.stamps)
		std::vector<CellInfo> where; //every opaque cell of the page (cellsX x cellsY): where its tile is on the canvas
	};
	std::unordered_map<GLuint, PbrPage> pbrPages;

	//the field being drawn, as the shader gets it (files read again when they change)
	struct Current
	{
		uint64_t entry = 0;
		std::shared_ptr<const PbrField> field;
		fieldpbr::Settings settings;
		fieldpbr::Lights lights;
		std::string settingsPath, lightsPath;
		std::filesystem::file_time_type settingsTime{}, lightsTime{};
		std::unordered_map<uint32_t, std::array<std::string, fieldpbr::KIND_COUNT>> path; //files in use per layer
		//the whole field's depth and the light grid (made the first time a page of the field is drawn)
		bool roomLoaded = false, probesLoaded = false;
		std::string roomPath;
		GLuint room = 0, probes = 0;
		int probeSlices = 0;
		float probeExposure = 1.0f;
		float probeMean = 0.0f; //the grid's average light (e0, luminance)
	};
	Current current;
	bool shaderMissing = false;

	FieldView view;
	CharFrame chars;
	bool charsSet = false;

	//measured while this frame's tiles are drawn (FieldView gets it at the end of the frame)
	struct Measure
	{
		bool drew = false;
		std::map<int, std::array<double, 3>> layerOffset; //layer id -> sum x, sum y, count
		double depthLo = 1e30, depthHi = -1e30, zLo = 0, zHi = 0;
	};
	Measure measure;

	void DeleteTexture(GLuint& texture)
	{
		if (texture != 0)
			glDeleteTextures(1, &texture);
		texture = 0;
	}

	void DeletePage(PbrPage& page)
	{
		for (GLuint& texture : page.map)
			DeleteTexture(texture);
		DeleteTexture(page.cells);
	}

	void ForgetField()
	{
		DeleteTexture(current.room);
		DeleteTexture(current.probes);
		current = Current();
		view = FieldView();
		measure = Measure();
		charsSet = false;
	}

	void UseField(const PageJob& job)
	{
		ForgetField();
		current.entry = job.entry;
		current.field = job.pbr;
		current.settings = job.pbr->settings;
		current.lights = job.pbr->lights;
		current.settingsPath = job.pbr->settingsPath;
		current.lightsPath = job.pbr->lightsPath;
		current.settingsTime = FileTime(current.settingsPath);
		current.lightsTime = FileTime(current.lightsPath);
		for (const auto& [pupu, layer] : job.pbr->layers)
			current.path[pupu] = layer.path;
	}

	struct UnpackState
	{
		GLint alignment = 4, rowLength = 0, buffer = 0;
		UnpackState()
		{
			glGetIntegerv(GL_UNPACK_ALIGNMENT, &alignment);
			glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLength);
			glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &buffer);
			if (buffer != 0)
				glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
			glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		}
		~UnpackState()
		{
			glPixelStorei(GL_UNPACK_ALIGNMENT, alignment);
			glPixelStorei(GL_UNPACK_ROW_LENGTH, rowLength);
			if (buffer != 0)
				glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(buffer));
		}
	};

	//--- the nearest thing each tile shows: the view distance a tenth of its covered pixels are nearer than (from the page's
	//_ND; a tenth, so a sliver of something nearer, a door frame's edge in a door's tile, doesn't count). The game's depth for
	//a tile is where that is (demaster_field.frag measures from there). A small pass on the GPU writes it into the cells
	//(block 2, blue) once per page; where it can't run, the per-Z value from _pbr.txt stays.
	const char* const NEAREST_VERT = "#version 120\nvoid main() { gl_Position = gl_Vertex; }\n";
	const char* const NEAREST_FRAG = R"(#version 120
uniform sampler2D albedo;
uniform sampler2D nd;
uniform vec4 cellInfo;   // cells across, down; page texels per cell across, down
uniform vec4 pageInfo;   // 1 / page width, 1 / page height
uniform vec4 depthModel; // a, b, 1 = inverse, nearest distance
// the view distance at sample (i, j) of the cell's 16 x 16; < 0: not covered
float distanceAt(vec2 cell, int i, int j)
{
	vec2 inside = floor((vec2(float(i), float(j)) + 0.5) * cellInfo.zw / 16.0);
	vec2 uv = (cell * cellInfo.zw + inside + 0.5) * pageInfo.xy;
	if (texture2D(albedo, uv).a < 0.5)
		return -1.0;
	float d = depthModel.x + depthModel.y * texture2D(nd, uv).a;
	float z = depthModel.z > 0.5 ? 1.0 / max(d, 0.000001) : d;
	return max(z, depthModel.w);
}

void main()
{
	vec2 cell = floor(gl_FragCoord.xy) - vec2(0.0, cellInfo.y);
	float lo = 1e9, hi = -1e9, covered = 0.0;
	for (int j = 0; j < 16; j++)
		for (int i = 0; i < 16; i++)
		{
			float z = distanceAt(cell, i, j);
			if (z < 0.0)
				continue;
			lo = min(lo, z);
			hi = max(hi, z);
			covered += 1.0;
		}
	if (covered < 1.0)
	{
		gl_FragColor = vec4(0.0);
		return;
	}
	// the distance a tenth of them are nearer than (halving the range twelve times)
	for (int k = 0; k < 12; k++)
	{
		float middle = 0.5 * (lo + hi);
		float nearer = 0.0;
		for (int j = 0; j < 16; j++)
			for (int i = 0; i < 16; i++)
			{
				float z = distanceAt(cell, i, j);
				if (z >= 0.0 && z <= middle)
					nearer += 1.0;
			}
		if (nearer >= 0.1 * covered)
			hi = middle;
		else
			lo = middle;
	}
	gl_FragColor = vec4(0.0, 0.0, hi, 0.0);
}
)";

	struct NearestPass
	{
		bool tried = false;
		GLuint program = 0, framebuffer = 0;
		GLint cellInfo = -1, pageInfo = -1, depthModel = -1;
	};
	NearestPass nearestPass;

	GLuint CompileProgram(const char* vertex, const char* fragment, std::string& error)
	{
		const auto compile = [&error](const GLenum kind, const char* source) -> GLuint
		{
			const GLuint shader = glCreateShader(kind);
			glShaderSource(shader, 1, &source, nullptr);
			glCompileShader(shader);
			GLint ok = 0;
			glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
			if (!ok)
			{
				char text[1024] = {};
				glGetShaderInfoLog(shader, sizeof(text) - 1, nullptr, text);
				error = text;
				glDeleteShader(shader);
				return 0;
			}
			return shader;
		};
		const GLuint vs = compile(GL_VERTEX_SHADER, vertex);
		const GLuint fs = vs != 0 ? compile(GL_FRAGMENT_SHADER, fragment) : 0;
		if (vs == 0 || fs == 0)
		{
			if (vs != 0)
				glDeleteShader(vs);
			return 0;
		}
		const GLuint program = glCreateProgram();
		glAttachShader(program, vs);
		glAttachShader(program, fs);
		glLinkProgram(program);
		glDeleteShader(vs);
		glDeleteShader(fs);
		GLint ok = 0;
		glGetProgramiv(program, GL_LINK_STATUS, &ok);
		if (!ok)
		{
			char text[1024] = {};
			glGetProgramInfoLog(program, sizeof(text) - 1, nullptr, text);
			error = text;
			glDeleteProgram(program);
			return 0;
		}
		return program;
	}

	bool MakeNearestPass()
	{
		NearestPass& n = nearestPass;
		if (n.tried)
			return n.program != 0;
		n.tried = true;
		std::string error;
		n.program = CompileProgram(NEAREST_VERT, NEAREST_FRAG, error);
		if (n.program == 0)
		{
			Log("FIELD_PBR: the tiles' nearest points can't be measured on this GPU (%s): characters are hidden per .map Z\n",
				error.c_str());
			return false;
		}
		n.cellInfo = glGetUniformLocation(n.program, "cellInfo");
		n.pageInfo = glGetUniformLocation(n.program, "pageInfo");
		n.depthModel = glGetUniformLocation(n.program, "depthModel");
		const GLint previous = glstate::Program();
		glUseProgram(n.program);
		glUniform1i(glGetUniformLocation(n.program, "albedo"), 0);
		glUniform1i(glGetUniformLocation(n.program, "nd"), 1);
		glUseProgram(static_cast<GLuint>(previous));
		glGenFramebuffers(1, &n.framebuffer);
		return true;
	}

	//albedo: the page (the game's texture, coverage in alpha)
	void MeasureNearest(const GLuint albedo, const PbrPage& page)
	{
		const GLuint nd = page.map[fieldpbr::NORMAL_DEPTH];
		if (albedo == 0 || nd == 0 || page.cells == 0 || page.size.width <= 0 || page.size.height <= 0 || !MakeNearestPass())
			return;
		const NearestPass& n = nearestPass;
		glstate::RenderState state;
		glstate::Capture(state);
		const GLint program = glstate::Program();
		const GLenum active = glstate::ActiveTexture();
		const GLuint unit0 = glstate::Texture(GL_TEXTURE_2D, 0), unit1 = glstate::Texture(GL_TEXTURE_2D, 1);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, n.framebuffer);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, page.cells, 0);
		const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
		if (status == GL_FRAMEBUFFER_COMPLETE)
		{
			glstate::RawViewport(0, page.cellsY, page.cellsX, page.cellsY);
			glDisable(GL_BLEND);
			glDisable(GL_DEPTH_TEST);
			glDisable(GL_ALPHA_TEST);
			glDisable(GL_SCISSOR_TEST);
			glDisable(GL_CULL_FACE);
			glDisable(GL_STENCIL_TEST);
			glColorMask(GL_FALSE, GL_FALSE, GL_TRUE, GL_FALSE);
			glUseProgram(n.program);
			glUniform4f(n.cellInfo, static_cast<float>(page.cellsX), static_cast<float>(page.cellsY),
				static_cast<float>(page.size.width) / page.cellsX, static_cast<float>(page.size.height) / page.cellsY);
			glUniform4f(n.pageInfo, 1.0f / page.size.width, 1.0f / page.size.height, 0.0f, 0.0f);
			const fieldpbr::Space& space = current.settings.space;
			glUniform4f(n.depthModel, static_cast<float>(space.a), static_cast<float>(space.b),
				space.model == fieldpbr::DepthModel::Inverse ? 1.0f : 0.0f, static_cast<float>(space.nearest));
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, albedo);
			glActiveTexture(GL_TEXTURE1);
			glBindTexture(GL_TEXTURE_2D, nd);
			glBegin(GL_TRIANGLE_STRIP);
			glVertex2f(-1.0f, -1.0f);
			glVertex2f(1.0f, -1.0f);
			glVertex2f(-1.0f, 1.0f);
			glVertex2f(1.0f, 1.0f);
			glEnd();
		}
		else
			LogOnce("nearest-fbo", "FIELD_PBR: the tiles' nearest points can't be measured on this GPU (render target status 0x%X): "
				"characters are hidden per .map Z\n", status);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, unit1);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, unit0);
		glActiveTexture(active);
		glUseProgram(static_cast<GLuint>(program));
		glstate::Apply(state);
	}

	//per 16x16 cell, two texels (see demaster_field.frag): where its tile is on the canvas, flags, lightmap scale; and, cellsY
	//rows further down, its .map Z, layer id and the view distance of the nearest thing the tile shows (MeasureNearest; until
	//then, or where it can't run, the nearest part of what tiles of that Z show, from _pbr.txt)
	void BuildCells(const GLuint target, PbrPage& page)
	{
		page.cellsX = std::max(1, page.size.logicalWidth / TILE_SIZE);
		page.cellsY = std::max(1, page.size.logicalHeight / TILE_SIZE);
		const size_t cells = static_cast<size_t>(page.cellsX) * page.cellsY;
		std::vector<float> data(cells * 2 * 4, 0.0f);
		for (const CellStamp* stamp : page.stamps)
		{
			const int cx = stamp->cellX / TILE_SIZE, cy = stamp->cellY / TILE_SIZE;
			if (cx >= page.cellsX || cy >= page.cellsY)
				continue;
			const auto layer = current.field->layers.find(stamp->pupu);
			if (layer == current.field->layers.end())
				continue;
			float flags = FLAG_PBR;
			if (page.map[fieldpbr::NORMAL_DEPTH] != 0 && !layer->second.stem[fieldpbr::NORMAL_DEPTH].empty())
				flags += FLAG_NORMAL;
			if (page.map[fieldpbr::ORME] != 0 && !layer->second.stem[fieldpbr::ORME].empty())
			{
				flags += FLAG_ORME;
				if (layer->second.emissive)
					flags += FLAG_EMISSIVE;
			}
			const size_t index = static_cast<size_t>(cy) * page.cellsX + cx;
			float* cell = data.data() + index * 4;
			cell[0] = static_cast<float>(stamp->layerX);
			cell[1] = static_cast<float>(stamp->layerY);
			cell[2] = flags;
			cell[3] = current.settings.LightmapScale(stamp->pupu, layer->second.ownLightmap);
			float* more = data.data() + (cells + index) * 4;
			more[0] = static_cast<float>(stamp->z);
			more[1] = static_cast<float>(stamp->layerId);
			const auto occlusion = current.settings.occlusion.find(stamp->z);
			more[2] = occlusion != current.settings.occlusion.end() ? static_cast<float>(occlusion->second) : 0.0f;
		}
		page.where.assign(cells, CellInfo());
		for (const CellStamp& stamp : page.job.plan.stamps)
		{
			const int cx = stamp.cellX / TILE_SIZE, cy = stamp.cellY / TILE_SIZE;
			if (stamp.semi || cx >= page.cellsX || cy >= page.cellsY)
				continue;
			CellInfo& info = page.where[static_cast<size_t>(cy) * page.cellsX + cx];
			info.valid = true;
			info.layerX = stamp.layerX;
			info.layerY = stamp.layerY;
			info.layerId = stamp.layerId;
		}
		if (page.cells == 0)
			glGenTextures(1, &page.cells);
		glBindTexture(GL_TEXTURE_2D, page.cells);
		{
			UnpackState unpack;
			RawTexImage(GL_RGBA32F, page.cellsX, page.cellsY * 2, GL_RGBA, GL_FLOAT, data.data());
		}
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		MeasureNearest(target, page);
	}

	//the page's _LF, _ND and _ORME pages (same cells as the game's page), then the cell table
	void BuildMaps(const GLuint target, PbrPage& page, std::string& report)
	{
		for (int k = fieldpbr::LIGHTMAP; k < fieldpbr::KIND_COUNT; k++)
		{
			PageJob job = page.job;
			job.kind = k;
			job.plan.stamps.clear();
			job.plan.complete = true;
			job.imagePath.clear();
			for (const CellStamp* stamp : page.stamps)
			{
				const auto path = current.path.find(stamp->pupu);
				if (path == current.path.end() || path->second[k].empty())
					continue;
				job.plan.stamps.push_back(*stamp);
				job.imagePath[stamp->pupu] = path->second[k];
			}
			const char* suffix = fieldpbr::KindSuffix(static_cast<fieldpbr::Kind>(k));
			if (job.plan.stamps.empty())
			{
				DeleteTexture(page.map[k]);
				report += std::string(", ") + suffix + " none";
				continue;
			}
			if (page.map[k] == 0)
				glGenTextures(1, &page.map[k]);
			std::string how;
			page.mapSize[k] = BuildPage(page.map[k], job, how);
			report += std::string(", ") + suffix + " " + how;
		}
		BuildCells(target, page);
		glBindTexture(GL_TEXTURE_2D, target);
	}

	//the whole field's _ND (the room's depth on the canvas), for shadows and walls between
	void LoadRoom()
	{
		current.roomLoaded = true;
		DeleteTexture(current.room);
		const std::string& stem = current.field->roomStem;
		if (stem.empty())
			return;
		const ImageFile file = ResolveImage(stem);
		if (file.path.empty())
			return;
		const SafeBimg image = LoadImageFromFile(file.path.c_str());
		if (!image)
		{
			Log("FIELD_PBR: %s: cannot read %s: no room shadows or walls between\n", current.field->name.c_str(), file.path.c_str());
			return;
		}
		current.roomPath = file.path;
		GLint bound = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
		glGenTextures(1, &current.room);
		glBindTexture(GL_TEXTURE_2D, current.room);
		bool ok = false;
		{
			UnpackState unpack;
			if (image->m_format == bimg::TextureFormat::BC7)
			{
				bimg::ImageMip mip{};
				if (bimg::imageGetRawData(*image, 0, 0, image->m_data, image->m_size, mip))
				{
					glCompressedTexImage2D(GL_TEXTURE_2D, 0, GL_COMPRESSED_RGBA_BPTC_UNORM, static_cast<GLsizei>(mip.m_width),
						static_cast<GLsizei>(mip.m_height), 0, static_cast<GLsizei>(mip.m_size), mip.m_data);
					ok = true;
				}
			}
			else
			{
				static bx::DefaultAllocator allocator;
				if (bimg::ImageContainer* rgba = bimg::imageConvert(&allocator, bimg::TextureFormat::RGBA8, *image, false))
				{
					RawTexImage(GL_RGBA8, static_cast<GLsizei>(rgba->m_width), static_cast<GLsizei>(rgba->m_height), GL_RGBA, GL_UNSIGNED_BYTE,
						rgba->m_data);
					bimg::imageFree(rgba);
					ok = true;
				}
			}
		}
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
		if (!ok)
			DeleteTexture(current.room);
		else if (FIELD_DEBUG)
			Log("FIELD_PBR: %s: the room's depth from %s (%ux%u)\n", current.field->name.c_str(), file.path.c_str(), image->m_width,
				image->m_height);
	}

	//<field>_PROBES.dds: the room's light in 3D, for the characters (a 3D texture, 4 blocks of slices)
	void LoadProbes()
	{
		current.probesLoaded = true;
		DeleteTexture(current.probes);
		current.probeSlices = 0;
		const std::string path = FieldFile(current.field->folder, current.field->name, current.field->language, PROBES_SUFFIX);
		if (path.empty())
			return;
		std::string bytes;
		fieldpbr::ProbeGrid grid;
		if (!ReadBytes(path, bytes) || !fieldpbr::ReadProbes(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), grid) || !grid.IsValid())
		{
			Log("FIELD_PBR: %s: %s is not a light grid fieldprep wrote: characters get the game's flat light\n", current.field->name.c_str(),
				path.c_str());
			return;
		}
		double sum = 0;
		size_t count = 0;
		for (int z = 0; z < grid.nz; z++)
			for (int y = 0; y < grid.ny; y++)
				for (int x = 0; x < grid.nx; x++)
				{
					const float* e0 = grid.At(0, x, y, z);
					sum += 0.2126 * e0[0] + 0.7152 * e0[1] + 0.0722 * e0[2];
					count++;
				}
		current.probeMean = count ? static_cast<float>(sum / static_cast<double>(count)) : 0.0f;
		if (glTexImage3D == nullptr)
			return;
		GLint bound = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_3D, &bound);
		glGenTextures(1, &current.probes);
		glBindTexture(GL_TEXTURE_3D, current.probes);
		{
			UnpackState unpack;
			glTexImage3D(GL_TEXTURE_3D, 0, GL_RGBA16F, grid.nx, grid.ny, grid.nz * 4, 0, GL_RGBA, GL_FLOAT, grid.data.data());
		}
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAX_LEVEL, 0);
		glBindTexture(GL_TEXTURE_3D, static_cast<GLuint>(bound));
		current.probeSlices = grid.nz;
		if (FIELD_DEBUG)
			Log("FIELD_PBR: %s: light grid %dx%dx%d from %s, average light %.3f\n", current.field->name.c_str(), grid.nx, grid.ny, grid.nz,
				path.c_str(), current.probeMean);
	}

	//how bright the light grid makes the characters: character_light in _pbr.txt, or (auto) as bright as the game on average
	float ProbeExposure()
	{
		const double setting = current.settings.characterLight;
		if (setting > 0)
			return static_cast<float>(setting);
		return current.probeMean > 1e-6f ? static_cast<float>(3.14159265 / current.probeMean) : 1.0f;
	}

	void UpdateView()
	{
		view.entry = current.entry;
		view.name = current.field != nullptr ? current.field->name : std::string();
		view.settings = &current.settings;
		view.canvasWidth = current.field != nullptr ? current.field->canvasWidth : 0;
		view.canvasHeight = current.field != nullptr ? current.field->canvasHeight : 0;
		view.room = current.room;
		view.probes = current.probes;
		view.probeSlices = current.probeSlices;
		view.probeExposure = ProbeExposure();
	}

	//--- drawing

	struct FieldProgram
	{
		bool tried = false;
		GLuint id = 0;
		GLint tileClamp = -1, pbr = -1, space = -1, depth = -1, ambient = -1, count = -1, lightPos = -1, lightColor = -1, lightFlicker = -1;
		GLint stepAside = -1, quality = -1, sky = -1, roomSize = -1, features = -1, charCount = -1, cardCenter = -1, cardUp = -1,
			cardRight = -1, cardDir = -1, contact = -1, shadowCount = -1, shadowMatrix = -1, shadowInfo = -1, blendMode = -1;
		GLint emissive = -1, emissiveTint = -1, night = -1, nightTint = -1;
	};
	FieldProgram fieldProgram;
	bool unitsBound = false, programSwapped = false;
	GLint savedBinding[UNIT_LAST + 1] = {};
	GLint gameProgram = 0;

	bool MakeFieldProgram()
	{
		if (fieldProgram.tried)
			return fieldProgram.id != 0;
		fieldProgram.tried = true;
		std::string error;
		const GLuint id = BuildProgram("demaster_field.vert", "demaster_field.frag", error);
		if (id == 0)
		{
			shaderMissing = true;
			Log("FIELD_PBR: %s. PBR layers are drawn from their paintings. Copy DEMASTER_EXP\\shaders\\ from this release into the "
				"game's DEMASTER_EXP\\shaders\\.\n", error.c_str());
			return false;
		}
		FieldProgram& p = fieldProgram;
		p.id = id;
		const auto at = [id](const char* name) { return glGetUniformLocation(id, name); };
		p.tileClamp = at("demasterTileClamp");
		p.pbr = at("demasterPbr");
		p.space = at("demasterSpace");
		p.depth = at("demasterDepth");
		p.ambient = at("demasterAmbient");
		p.emissive = at("demasterEmissive");
		p.emissiveTint = at("demasterEmissiveTint");
		p.night = at("demasterNight");
		p.nightTint = at("demasterNightTint");
		p.count = at("demasterLightCount");
		p.lightPos = at("demasterLightPos");
		p.lightColor = at("demasterLightColor");
		p.lightFlicker = at("demasterLightFlicker");
		p.stepAside = at("demasterStepAside");
		p.quality = at("demasterQuality");
		p.sky = at("demasterSky");
		p.roomSize = at("demasterRoomSize");
		p.features = at("demasterFeatures");
		p.charCount = at("demasterCharCount");
		p.cardCenter = at("demasterCardCenter");
		p.cardUp = at("demasterCardUp");
		p.cardRight = at("demasterCardRight");
		p.cardDir = at("demasterCardDir");
		p.contact = at("demasterContact");
		p.shadowCount = at("demasterShadowCount");
		p.shadowMatrix = at("demasterShadowMatrix");
		p.shadowInfo = at("demasterShadowInfo");
		p.blendMode = at("demasterBlendMode");
		const GLint previous = glstate::Program();
		glUseProgram(id);
		const char* samplers[] = { "tex", "demasterLF", "demasterND", "demasterORME", "demasterCells", "demasterRoom", "demasterCapture",
			"demasterShadowMap", "demasterCharDepth" };
		const int units[] = { 0, UNIT_LIGHTMAP, UNIT_NORMAL, UNIT_ORME, UNIT_CELLS, UNIT_ROOM, UNIT_CAPTURE, UNIT_SHADOW, UNIT_CHAR_DEPTH };
		for (int i = 0; i < 9; i++)
			if (const GLint location = at(samplers[i]); location >= 0)
				glUniform1i(location, units[i]);
		glUseProgram(static_cast<GLuint>(previous));
		Log("FIELD_PBR: PBR pages are drawn with DEMASTER's field program (DEMASTER_EXP\\shaders\\demaster_field.vert/.frag), "
			"FIELD_PBR_QUALITY %s\n", CurrentQuality().name);
		return true;
	}

	//where the lights are in view space and how bright they are now (flicker, the game's light groups)
	const LightUniforms& Lights()
	{
		static LightUniforms u;
		static ULONGLONG lastTick = ~0ull;
		static uint64_t lastEntry = 0;
		static const fieldpbr::Lights* lastLights = nullptr;
		const ULONGLONG now = GetTickCount64();
		if (now == lastTick && lastEntry == current.entry && lastLights == &current.lights)
			return u;
		lastTick = now;
		lastEntry = current.entry;
		lastLights = &current.lights;
		const double seconds = now / 1000.0;
		const fieldpbr::Space& space = current.settings.space;
		static const uint32_t groups = HasAddress(FIELD_BG_GROUPS) ? GetAddress(FIELD_BG_GROUPS) : 0; //a guest address
		//the time of day (DAY_NIGHT): the field's own (time = day or night in _pbr.txt) or the clock's; which lights are on and
		//how much, the night's darkness
		const double night = DayNightAmount(current.settings.time);
		size_t dropped = 0;
		const std::vector<fieldpbr::ActiveLight> active = fieldpbr::ActiveLights(current.lights, night, &dropped);
		if (dropped > 0)
			LogOnce("daynight-dropped-" + std::to_string(current.entry), "FIELD_PBR: %s: more than %d lights on during a fade between day and "
				"night: the %zu faintest left out until it ends\n", current.field != nullptr ? current.field->name.c_str() : "?", fieldpbr::MAX_LIGHTS,
				dropped);
		const fieldpbr::Night& dark = current.lights.night;
		u.night[0] = static_cast<float>(night);
		u.night[1] = dark.level;
		u.night[2] = dark.lamps;
		u.night[3] = 0.0f;
		for (int c = 0; c < 3; c++)
			u.nightTint[c] = dark.tint[c];
		u.count = static_cast<int>(std::min<size_t>(active.size(), fieldpbr::MAX_LIGHTS));
		for (int i = 0; i < u.count; i++)
		{
			const size_t index = active[i].index;
			const float weight = active[i].weight;
			const fieldpbr::Light& l = current.lights.lights[index];
			const fieldpbr::Vec3 p = space.ViewPosition(l.x, l.y, l.depth);
			const double flicker = fieldpbr::FlickerValue(l.flicker, l.amount, l.speed, fieldpbr::LightSeed(l, index), seconds);
			//its light group: the colour scripts give the group's tiles (128 = as painted) and the frame it shows
			float group[3] = { 1, 1, 1 };
			bool on = true;
			uint8_t entry[4];
			if (l.follows >= 0 && l.follows < 256 && groups != 0 && GuestRead(entry, groups + static_cast<uint32_t>(l.follows) * 4, 4))
			{
				for (int c = 0; c < 3; c++)
					group[c] = entry[c] / 128.0f;
				if (!l.onStates.empty())
					on = std::find(l.onStates.begin(), l.onStates.end(), static_cast<int>(entry[3])) != l.onStates.end();
			}
			const float lit = on ? 1.0f : 0.0f;
			u.pos[i * 4 + 0] = static_cast<float>(p.x);
			u.pos[i * 4 + 1] = static_cast<float>(p.y);
			u.pos[i * 4 + 2] = static_cast<float>(p.z);
			u.pos[i * 4 + 3] = static_cast<float>(std::max(1e-6, space.ViewLength(l.range, p.z)));
			for (int c = 0; c < 3; c++)
				u.color[i * 4 + c] = static_cast<float>(l.color[c] * l.intensity * flicker * group[c] * lit * weight);
			u.color[i * 4 + 3] = l.specular;
			const bool owns = l.OwnsMask();
			const float groupMean = (group[0] + group[1] + group[2]) / 3.0f;
			u.flicker[i * 4 + 0] = owns ? static_cast<float>(flicker * groupMean * lit - 1.0) : 0.0f;
			u.flicker[i * 4 + 1] = owns ? 1.0f : 0.0f;
			u.flicker[i * 4 + 2] = l.ShadowStrength() * weight; //a light fading out takes its shadows with it
			u.flicker[i * 4 + 3] = active[i].nightLamp && on ? 1.0f : 0.0f;
			u.castsShadow[i] = on && weight > 0.0f && l.ShadowStrength() > 0.0f && (l.intensity > 0.0f || l.baked);
		}
		return u;
	}

	//The tiles of a PBR page as drawn: where the canvas is on the game's screen (per layer: a layer somewhere else moves on its
	//own), and how the depth buffer follows the game's depth. TLVERTEX: x, y, z, rhw, colour, specular, u, v (32 bytes).
	void MeasureTiles(const PbrPage& page, const GLsizei count, const GLenum type, const void* indices)
	{
		if (count <= 0 || count > 6000 || indices == nullptr || (type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT))
			return;
		if (LastVerticesPointer.pointer == nullptr || LastVerticesPointer.size != 4 || LastTexCoordPointer.pointer == nullptr)
			return;
		const int stride = LastVerticesPointer.stride != 0 ? LastVerticesPointer.stride : 16;
		const int uvStride = LastTexCoordPointer.stride != 0 ? LastTexCoordPointer.stride : 8;
		static std::vector<uint32_t> list; //kept between draws
		list.resize(static_cast<size_t>(count));
		if (type == GL_UNSIGNED_SHORT)
		{
			static std::vector<uint16_t> raw;
			raw.resize(static_cast<size_t>(count));
			if (!SafeRead(raw.data(), indices, raw.size() * 2))
				return;
			std::copy(raw.begin(), raw.end(), list.begin());
		}
		else if (!SafeRead(list.data(), indices, list.size() * 4))
			return;
		//the vertices the indices reach, read once
		uint32_t lo = 0xFFFFFFFF, hi = 0;
		for (const uint32_t i : list)
		{
			lo = std::min(lo, i);
			hi = std::max(hi, i);
		}
		if (list.empty() || hi - lo > 65535)
			return;
		const size_t n = static_cast<size_t>(hi - lo) + 1;
		static std::vector<uint8_t> positions, uvs; //kept between draws
		positions.resize(n * stride);
		uvs.resize(n * uvStride);
		if (!SafeRead(positions.data(), static_cast<const uint8_t*>(LastVerticesPointer.pointer) + static_cast<size_t>(lo) * stride,
				(n - 1) * stride + 16)
			|| !SafeRead(uvs.data(), static_cast<const uint8_t*>(LastTexCoordPointer.pointer) + static_cast<size_t>(lo) * uvStride,
				(n - 1) * uvStride + 8))
			return;
		const float logicalW = static_cast<float>(page.size.logicalWidth), logicalH = static_cast<float>(page.size.logicalHeight);
		for (size_t t = 0; t + 2 < list.size(); t += 3)
		{
			float p[3][4], uv[3][2];
			for (int k = 0; k < 3; k++)
			{
				memcpy(p[k], positions.data() + static_cast<size_t>(list[t + k] - lo) * stride, 16);
				memcpy(uv[k], uvs.data() + static_cast<size_t>(list[t + k] - lo) * uvStride, 8);
			}
			//the cell: from the triangle's middle (its corners lie on cell edges)
			const float cu = (uv[0][0] + uv[1][0] + uv[2][0]) / 3.0f * logicalW, cv = (uv[0][1] + uv[1][1] + uv[2][1]) / 3.0f * logicalH;
			const int cx = static_cast<int>(std::floor(cu / TILE_SIZE)), cy = static_cast<int>(std::floor(cv / TILE_SIZE));
			if (cx < 0 || cy < 0 || cx >= page.cellsX || cy >= page.cellsY)
				continue;
			const CellInfo& cell = page.where[static_cast<size_t>(cy) * page.cellsX + cx];
			if (!cell.valid)
				continue;
			auto& sum = measure.layerOffset[cell.layerId];
			for (int k = 0; k < 3; k++)
			{
				const double canvasX = cell.layerX + (uv[k][0] * logicalW - cx * TILE_SIZE);
				const double canvasY = cell.layerY + (uv[k][1] * logicalH - cy * TILE_SIZE);
				sum[0] += p[k][0] - 2.0 * canvasX;
				sum[1] += p[k][1] - 2.0 * canvasY;
				sum[2] += 1.0;
				if (p[k][3] > 0)
				{
					const double d = 1.0 / p[k][3];
					if (d < measure.depthLo)
					{
						measure.depthLo = d;
						measure.zLo = p[k][2];
					}
					if (d > measure.depthHi)
					{
						measure.depthHi = d;
						measure.zHi = p[k][2];
					}
				}
			}
		}
	}

	void SetUniforms(const PbrPage& page, const float* tileClamp, const GLint blendMode)
	{
		const FieldProgram& p = fieldProgram;
		const fieldpbr::Settings& s = current.settings;
		const fieldpbr::Space& space = s.space;
		glUniform4f(p.tileClamp, tileClamp[0], tileClamp[1], tileClamp[2], tileClamp[3]);
		glUniform4f(p.pbr, 1.0f, static_cast<float>(FIELD_PBR_VIEW), static_cast<float>(page.cellsX), static_cast<float>(page.cellsY));
		glUniform4f(p.space, static_cast<float>(space.zoom), static_cast<float>(space.originX), static_cast<float>(space.originY),
			space.model == fieldpbr::DepthModel::Inverse ? 1.0f : 0.0f);
		glUniform4f(p.depth, static_cast<float>(space.a), static_cast<float>(space.b), static_cast<float>(space.nearest), 0.0f);
		glUniform4f(p.ambient, current.lights.ambient[0], current.lights.ambient[1], current.lights.ambient[2], 0.0f);
		const LightUniforms& lights = Lights();
		//the glows: by day nothing (the painting has them); at night they stay lit as the room goes dark
		const fieldpbr::Emissive& e = current.lights.emissive;
		glUniform4f(p.emissive, e.hue / 360.0f, e.strength, 0.0f, lights.night[0]);
		glUniform4f(p.emissiveTint, e.tint[0], e.tint[1], e.tint[2], 0.0f);
		glUniform4fv(p.night, 1, lights.night);
		glUniform4fv(p.nightTint, 1, lights.nightTint);
		glUniform1i(p.blendMode, blendMode);
		glUniform1i(p.count, lights.count);
		if (lights.count > 0)
		{
			glUniform4fv(p.lightPos, lights.count, lights.pos);
			glUniform4fv(p.lightColor, lights.count, lights.color);
			glUniform4fv(p.lightFlicker, lights.count, lights.flicker);
		}
		//stepping aside for characters: the depth buffer's window value per game depth unit (0.5 + z / 2 of the game's z), the
		//characters' depth (copied right after DEMASTER drew them)
		const bool stepAside = FIELD_PBR_DEPTH && charsSet && chars.depth != 0 && chars.depthWidth > 0 && chars.depthHeight > 0;
		glUniform4f(p.stepAside, stepAside ? 1.0f : 0.0f, static_cast<float>(view.depthSlope * 0.5),
			stepAside ? 1.0f / chars.depthWidth : 0.0f, stepAside ? 1.0f / chars.depthHeight : 0.0f);
		const Quality& q = CurrentQuality();
		glUniform4f(p.quality, static_cast<float>(q.lightWallSteps), static_cast<float>(q.reflectionWallSteps),
			static_cast<float>(q.shadowTaps), 0.0f);
		const float skyFrom = s.sky && s.skyValue >= 0 ? static_cast<float>(space.Distance(s.skyValue)) : 0.0f;
		glUniform4f(p.sky, skyFrom, static_cast<float>(view.parallaxLayers & 0xFFFFFF), s.sky ? 1.0f : 0.0f, 0.0f);
		glUniform4f(p.roomSize, static_cast<float>(std::max(1, view.canvasWidth)), static_cast<float>(std::max(1, view.canvasHeight)),
			current.room != 0 ? 1.0f : 0.0f, 0.0f);
		const bool roomShadows = FIELD_PBR_SHADOWS && current.room != 0;
		if (charsSet && chars.count > 0)
		{
			glUniform4f(p.features, chars.features[0], chars.features[1], chars.features[2], roomShadows ? 1.0f : 0.0f);
			glUniform1i(p.charCount, chars.count);
			glUniform4fv(p.cardCenter, chars.count, chars.cardCenter);
			glUniform4fv(p.cardUp, chars.count, chars.cardUp);
			glUniform4fv(p.cardRight, chars.count, chars.cardRight);
			glUniform4fv(p.cardDir, chars.count, chars.cardDir);
			glUniform4fv(p.contact, chars.count, chars.contact);
			glUniform1i(p.shadowCount, chars.shadowCount);
			if (chars.shadowCount > 0)
			{
				glUniformMatrix4fv(p.shadowMatrix, chars.shadowCount, GL_FALSE, chars.shadowMatrix);
				glUniform4fv(p.shadowInfo, chars.shadowCount, chars.shadowInfo);
			}
		}
		else
		{
			glUniform4f(p.features, 0.0f, 0.0f, 0.0f, roomShadows ? 1.0f : 0.0f);
			glUniform1i(p.charCount, 0);
			glUniform1i(p.shadowCount, 0);
		}
	}
}

void fieldrt::PbrPageBuilt(const GLuint target, const PageJob& job, const PageSize& size)
{
	PbrForget(target);
	if (!FIELD_PBR || shaderMissing || job.pbr == nullptr || job.kind >= 0)
		return;
	try
	{
		CheckGlew();
		if (current.entry != job.entry || current.field != job.pbr)
			UseField(job);
		PbrPage page;
		page.entry = job.entry;
		page.job = job;
		page.job.base.clear();
		page.job.gameAlpha.clear();
		page.job.basePath.clear();
		page.job.paintingPath.clear();
		page.size = size;
		bool any = false;
		for (const CellStamp& stamp : page.job.plan.stamps)
			any |= !stamp.semi && job.pbr->layers.count(stamp.pupu) != 0;
		if (!any)
			return;
		if (!page.job.plan.aligned)
		{
			LogOnce(job.name + "-pbr-unaligned-" + std::to_string(job.tPage), "FIELD_PBR: %s page %u: cells off the 16 grid, "
				"its PBR layers are drawn unlit\n", job.name.c_str(), job.tPage - 16);
			return;
		}
		auto [it, added] = pbrPages.emplace(target, std::move(page));
		PbrPage& stored = it->second;
		//the stamp pointers point into the stored copy
		stored.stamps.clear();
		for (const CellStamp& stamp : stored.job.plan.stamps)
			if (!stamp.semi && job.pbr->layers.count(stamp.pupu))
				stored.stamps.push_back(&stamp);
		std::string report;
		BuildMaps(target, stored, report);
		if (FIELD_DEBUG)
			LogOnce(job.name + "-pbr-" + std::to_string(job.entry) + "-" + std::to_string(job.tPage) + "-" + std::to_string(job.slot),
				"FIELD_PBR: %s page %u (.map palette %d): %zu PBR cells%s\n", job.name.c_str(), job.tPage - 16, job.palette,
				stored.stamps.size(), report.c_str());
	}
	catch (...)
	{
		Log("FIELD_PBR: making the PBR maps of %s page %u failed: drawn unlit\n", job.name.c_str(), job.tPage - 16);
		PbrForget(target);
	}
}

//FIELD_FRAME_LOG: does this page have PBR maps
bool FieldPbrHasPage(const unsigned int texture)
{
	return pbrPages.count(static_cast<GLuint>(texture)) != 0;
}

void fieldrt::PbrForget(const GLuint target)
{
	const auto it = pbrPages.find(target);
	if (it == pbrPages.end())
		return;
	DeletePage(it->second);
	pbrPages.erase(it);
}

void fieldrt::PbrForgetAll()
{
	for (auto& [target, page] : pbrPages)
		DeletePage(page);
	pbrPages.clear();
}

bool fieldrt::PbrShaderMissing()
{
	return shaderMissing;
}

bool fieldrt::PbrActive()
{
	return !pbrPages.empty();
}

//FIELD_LAYERS_LIVE: _pbr.txt, _lights.txt, _LF, _ND, _ORME, _PROBES.dds saved while playing. Once a second.
void fieldrt::PbrPoll()
{
	if (!FIELD_LAYERS_LIVE || pbrPages.empty() || current.field == nullptr)
		return;
	static ULONGLONG last = 0;
	const ULONGLONG now = GetTickCount64();
	if (now - last < 1000)
		return;
	last = now;
	const PbrField& f = *current.field;
	bool rebuildCells = false;
	std::set<uint32_t> changed;
	//settings and lights (a file that appeared is picked up too)
	const std::string settingsPath = FieldFile(f.folder, f.name, f.language, fieldpbr::SETTINGS_SUFFIX);
	if (const auto time = FileTime(settingsPath); settingsPath != current.settingsPath || time != current.settingsTime)
	{
		current.settingsPath = settingsPath;
		current.settingsTime = time;
		const fieldpbr::FieldTime timeBefore = current.settings.time;
		LoadSettings(settingsPath, nullptr, current.settings, f.name);
		rebuildCells = true;
		if (current.settings.time != timeBefore)
			DayNightFieldEntered(f.name, current.settings.time); //time = day | night | cycle changed: the clock waits or goes on
		Log("FIELD_PBR: %s: %s read again\n", f.name.c_str(), settingsPath.empty() ? "_pbr.txt (gone)" : settingsPath.c_str());
	}
	const std::string lightsPath = FieldFile(f.folder, f.name, f.language, fieldpbr::LIGHTS_SUFFIX);
	if (const auto time = FileTime(lightsPath); lightsPath != current.lightsPath || time != current.lightsTime)
	{
		current.lightsPath = lightsPath;
		current.lightsTime = time;
		LoadLights(lightsPath, current.lights, f.name);
		Log("FIELD_PBR: %s: %s read again, %zu light(s)\n", f.name.c_str(), lightsPath.empty() ? "_lights.txt (gone)" : lightsPath.c_str(),
			current.lights.lights.size());
	}
	//the light grid
	static std::filesystem::file_time_type probesTime{};
	static uint64_t probesEntry = 0;
	const std::string probesPath = FieldFile(f.folder, f.name, f.language, PROBES_SUFFIX);
	if (const auto time = FileTime(probesPath); probesEntry == current.entry && time != probesTime && current.probesLoaded)
	{
		LoadProbes();
		Log("FIELD_PBR: %s: %s read again\n", f.name.c_str(), probesPath.empty() ? "_PROBES.dds (gone)" : probesPath.c_str());
	}
	probesEntry = current.entry;
	probesTime = FileTime(probesPath);
	//the maps (the albedo is the game's page: FIELD_LAYERS_LIVE does that one)
	std::map<std::string, ImageFile> checked; //stem -> file (field-wide maps are shared by many layers)
	bool roomChanged = false;
	for (const auto& [pupu, layer] : f.layers)
		for (int k = fieldpbr::LIGHTMAP; k < fieldpbr::KIND_COUNT; k++)
		{
			const std::string& stem = layer.stem[k];
			if (stem.empty())
				continue;
			auto known = checked.find(stem);
			if (known == checked.end())
			{
				ImageFile file = ResolveImage(stem);
				if (!file.path.empty() && !RefreshLayerFile(file) && file.path == current.path[pupu][k])
					file.path.clear(); //unchanged
				known = checked.emplace(stem, file).first;
				roomChanged |= stem == f.roomStem && !known->second.path.empty();
			}
			if (known->second.path.empty())
				continue;
			current.path[pupu][k] = known->second.path;
			changed.insert(pupu);
		}
	if (roomChanged)
		LoadRoom();
	if (!rebuildCells && changed.empty())
		return;
	if (!changed.empty())
		ForgetComposed();
	GLint bound = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
	size_t rebuilt = 0;
	for (auto& [target, page] : pbrPages)
	{
		if (page.entry != current.entry)
			continue;
		bool uses = false;
		for (const CellStamp* stamp : page.stamps)
			if (changed.count(stamp->pupu))
			{
				uses = true;
				break;
			}
		std::string report;
		if (uses)
		{
			BuildMaps(target, page, report);
			rebuilt++;
		}
		else if (rebuildCells)
			BuildCells(target, page);
	}
	glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
	UpdateView();
	if (FIELD_DEBUG && rebuilt)
		Log("FIELD_PBR: %s: PBR maps changed on disk, %zu page(s) made again\n", f.name.c_str(), rebuilt);
}

bool fieldrt::PbrBeforeDraw(const GLuint boundTexture, const GLint program, const float* tileClamp, const GLsizei count, const GLenum type,
	const void* indices)
{
	const auto it = boundTexture != 0 ? pbrPages.find(boundTexture) : pbrPages.end();
	if (it == pbrPages.end() || it->second.entry != current.entry || !FIELD_PBR || shaderMissing)
		return false;
	const GameProgram& game = GameProgramOf(program);
	if (game.kind != ProgramKind::Main)
		return false; //another shader with the page bound (the round shadows): left as it is
	const long long started = FieldTimeNow(); //FIELD_FRAME_LOG's timing
	const GLint vertexType = game.vertexType >= 0 ? glstate::UniformInt(program, game.vertexType) : 3;
	if (vertexType != 3)
		return false;
	if (!MakeFieldProgram())
		return false;
	const GLint blendMode = game.blendMode >= 0 ? glstate::UniformInt(program, game.blendMode) : 0;
	PbrPage& page = it->second;
	if (!current.roomLoaded)
		LoadRoom();
	if (!current.probesLoaded)
		LoadProbes();
	UpdateView();
	view.active = true;
	measure.drew = true;
	MeasureTiles(page, count, type, indices);
	//texture units 1-8 (what was there goes back after the draw)
	const GLenum active = glstate::ActiveTexture();
	const bool capture = charsSet && chars.capture != 0, shadows = charsSet && chars.shadowMap != 0;
	const bool charDepth = FIELD_PBR_DEPTH && charsSet && chars.depth != 0;
	const GLuint textures[UNIT_LAST + 1] = { 0, page.map[fieldpbr::LIGHTMAP], page.map[fieldpbr::NORMAL_DEPTH], page.map[fieldpbr::ORME],
		page.cells, current.room, capture ? chars.capture : 0, shadows ? chars.shadowMap : 0, charDepth ? chars.depth : 0 };
	for (int unit = UNIT_FIRST; unit <= UNIT_LAST; unit++)
	{
		savedBinding[unit] = static_cast<GLint>(glstate::Texture(GL_TEXTURE_2D, unit));
		if (static_cast<GLuint>(savedBinding[unit]) == textures[unit])
			continue;
		glActiveTexture(GL_TEXTURE0 + unit);
		glBindTexture(GL_TEXTURE_2D, textures[unit]);
	}
	glActiveTexture(active);
	unitsBound = true;
	gameProgram = program;
	glUseProgram(fieldProgram.id);
	programSwapped = true;
	static const float zeros[4] = { 0, 0, 0, 0 };
	SetUniforms(page, tileClamp != nullptr ? tileClamp : zeros, blendMode);
	FrameLogNote("drawn with DEMASTER's field program %u instead (PBR page, %zu PBR cells; %d character card(s), %d shadow map(s)%s)",
		fieldProgram.id, page.stamps.size(), charsSet ? chars.count : 0, charsSet ? chars.shadowCount : 0,
		FIELD_PBR_DEPTH && charsSet && chars.depth != 0 ? "; steps aside for characters in front of what it shows" : "");
	FieldTimeAdd(TIME_PBR_SETUP, started);
	FrameLogMark("background draw start");
	return true;
}

void fieldrt::PbrAfterDraw()
{
	if (programSwapped)
		FrameLogMark("background draw end");
	const long long started = FieldTimeNow();
	struct Done
	{
		long long started;
		~Done() { FieldTimeAdd(TIME_PBR_SETUP, started); }
	} done{ started };
	if (programSwapped)
	{
		glUseProgram(static_cast<GLuint>(gameProgram));
		programSwapped = false;
	}
	if (!unitsBound)
		return;
	unitsBound = false;
	//the game's textures back where they were
	const GLenum active = glstate::ActiveTexture();
	bool changed = false;
	for (int unit = UNIT_FIRST; unit <= UNIT_LAST; unit++)
	{
		if (glstate::Texture(GL_TEXTURE_2D, unit) == static_cast<GLuint>(savedBinding[unit]))
			continue;
		glActiveTexture(GL_TEXTURE0 + unit);
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(savedBinding[unit]));
		changed = true;
	}
	if (changed)
		glActiveTexture(active);
}

//SwapBuffers: what this frame's tiles measured is what the characters of the next frame are put back with
void fieldrt::PbrEndFrame()
{
	const bool drew = measure.drew;
	view.active = drew && current.field != nullptr;
	if (drew)
	{
		//the main layer: the one with the most tiles; a layer far from where it puts the canvas moves on its own
		int mainLayer = -1;
		double most = 0;
		for (const auto& [layer, sum] : measure.layerOffset)
			if (sum[2] > most)
			{
				most = sum[2];
				mainLayer = layer;
			}
		if (mainLayer >= 0)
		{
			const auto& m = measure.layerOffset[mainLayer];
			view.offsetX = m[0] / m[2];
			view.offsetY = m[1] / m[2];
			view.offsetKnown = true;
			uint32_t parallax = 0;
			for (const auto& [layer, sum] : measure.layerOffset)
			{
				if (layer == mainLayer || layer < 0 || layer >= 24 || sum[2] <= 0)
					continue;
				if (std::fabs(sum[0] / sum[2] - view.offsetX) > 1.0 || std::fabs(sum[1] / sum[2] - view.offsetY) > 1.0)
					parallax |= 1u << layer;
			}
			if (parallax != view.parallaxLayers && FIELD_DEBUG)
				LogOnce(view.name + "-parallax-" + std::to_string(view.entry) + "-" + std::to_string(parallax),
					"FIELD_PBR: %s: layers moving on their own (drawn as sky): %s0x%06X\n", view.name.c_str(), parallax ? "" : "none ", parallax);
			view.parallaxLayers = parallax;
		}
		//the depth buffer per game depth unit (z = 0.0015 + 0.9985 x depth / 65536 in this game)
		if (measure.depthHi - measure.depthLo > 256.0)
		{
			const double slope = (measure.zHi - measure.zLo) / (measure.depthHi - measure.depthLo);
			if (slope > 0 && slope < 1e-3)
				view.depthSlope = slope;
		}
	}
	measure = Measure();
}

const FieldView& fieldrt::PbrView()
{
	return view;
}

const LightUniforms& fieldrt::PbrLights()
{
	return Lights();
}

void fieldrt::PbrSetCharacters(const CharFrame* frame)
{
	charsSet = frame != nullptr;
	if (frame != nullptr)
		chars = *frame;
}

bool fieldrt::PbrDrewThisFrame()
{
	return measure.drew;
}

const Quality& fieldrt::CurrentQuality()
{
	//low: integrated graphics; medium: GTX 1060 class; high: everything
	static const Quality levels[3] = {
		{ "low", 0, 0, 1, 2, 1024, 512, false },
		{ "medium", 6, 3, 4, 4, 2048, 512, true },
		{ "high", 12, 6, 4, 8, 2048, 512, true },
	};
	return levels[FIELD_PBR_QUALITY < 0 ? 0 : FIELD_PBR_QUALITY > 2 ? 2 : FIELD_PBR_QUALITY];
}
