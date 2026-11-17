//FIELD_FRAME_LOG: press Ctrl+F11 and DEMASTER writes down everything the game draws in the next frame, for working out
//character reflections, shadows and lighting:
//	every draw (glDrawElements, glDrawArrays, glBegin), in order: the buffer it draws into, the shader and its uniforms,
//	blending, depth, culling, the texture, the vertex arrays, the vertices it uses (ranges, which way the triangles turn,
//	the first few in full), and whether DEMASTER put the texture together (field layers, PBR maps)
//	the source of every shader program used
//	at the end of the frame: every buffer drawn into (colour, and depth packed exactly into RGB), and every texture used
//into DEMASTER_EXP\frame_log\<field>_<time>\. Nothing runs while no frame is asked for.

#include "config.h"
#include "coreHeader.h"
#include "debug.h"
#include "opengl.h"
#include "texture.h"
#include "field_pbr_runtime.h"
#include "minhook/include/MinHook.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

extern int ffWindowWidth, ffWindowHeight; //opengl.cpp
bool FieldLayersIsPage(unsigned int texture);
bool FieldPbrHasPage(unsigned int texture);
std::string FieldLayersFieldName();

namespace
{
	LPVOID drawArraysOriginal = nullptr, beginOriginal = nullptr;
	bool armed = false, capturing = false, keyWasDown = false;
	FILE* out = nullptr;
	std::string folder;
	int drawIndex = 0;
	std::set<GLuint> framebuffers;
	std::map<GLuint, std::string> programNames;
	std::vector<GLuint> textures;
	std::set<GLuint> texturesSeen;
	std::unordered_map<GLuint, int> uploadType; //texture -> the game's texture type when it was last filled

	void Write(const char* format, ...)
	{
		if (out == nullptr)
			return;
		va_list args;
		va_start(args, format);
		vfprintf(out, format, args);
		va_end(args);
	}

	//--- timing
	constexpr int TIME_FRAMES = 120;
	long long ticksPerSecond = 0;
	double slotTicks[TIME_SLOTS] = {}; //this frame
	double history[TIME_FRAMES][TIME_SLOTS + 1] = {}; //the last frames (+ the whole frame, swap to swap)
	int historyCount = 0, historyNext = 0;
	long long lastSwap = 0;
	struct Mark
	{
		GLuint query = 0;
		std::string label;
	};
	std::vector<Mark> marks;

	double Ms(const double ticks)
	{
		return ticksPerSecond > 0 ? ticks * 1000.0 / static_cast<double>(ticksPerSecond) : 0.0;
	}

	//the average of the frames before this one
	void WriteCpuTimes()
	{
		if (historyCount == 0)
			return;
		double sum[TIME_SLOTS + 1] = {};
		for (int f = 0; f < historyCount; f++)
			for (int s = 0; s <= TIME_SLOTS; s++)
				sum[s] += history[f][s];
		const auto avg = [&](const int s) { return Ms(sum[s] / historyCount); };
		Write("DEMASTER's CPU time per frame, average of the %d frames before this one (the logged frame itself is slow):\n", historyCount);
		Write("  whole frame (SwapBuffers to SwapBuffers) %.2f ms\n", avg(TIME_SLOTS));
		Write("  characters: reading the game's draws %.2f ms, back into the room and normals %.2f ms, reflection and face pictures %.2f ms,\n"
			"    shadow maps %.2f ms, on screen %.2f ms, depth copy %.2f ms\n", avg(TIME_CHARS_READ), avg(TIME_CHARS_PREP), avg(TIME_CHARS_PICTURES),
			avg(TIME_CHARS_SHADOWS), avg(TIME_CHARS_SCREEN), avg(TIME_CHARS_DEPTH));
		Write("  background: PBR program set up around its draws %.2f ms; the field layers' draw hook %.2f ms (with that set-up in it)\n",
			avg(TIME_PBR_SETUP), avg(TIME_LAYERS_HOOK));
	}

	//the GPU's timestamps between DEMASTER's steps in the logged frame
	void WriteGpuTimes()
	{
		if (marks.empty())
			return;
		glFinish();
		std::vector<GLuint64> times(marks.size(), 0);
		for (size_t i = 0; i < marks.size(); i++)
			glGetQueryObjectui64v(marks[i].query, GL_QUERY_RESULT, &times[i]);
		Write("\nGPU time in this frame (timestamps the GPU wrote as it got there; ms since the first):\n");
		double background = 0;
		int backgroundDraws = 0;
		for (size_t i = 0; i < marks.size(); i++)
		{
			const double at = static_cast<double>(times[i] - times[0]) / 1e6;
			const double since = i > 0 ? static_cast<double>(times[i] - times[i - 1]) / 1e6 : 0.0;
			if (marks[i].label == "background draw end" && i > 0 && marks[i - 1].label == "background draw start")
			{
				background += since;
				backgroundDraws++;
				continue;
			}
			if (marks[i].label == "background draw start")
				continue;
			Write("  %8.3f ms  (+%.3f)  %s\n", at, since, marks[i].label.c_str());
		}
		Write("  PBR background draws: %.3f ms on the GPU over %d draw(s)\n", background, backgroundDraws);
		for (const Mark& m : marks)
			glDeleteQueries(1, &m.query);
		marks.clear();
	}


	//reads game memory without crashing when a pointer is bad
	bool SafeCopy(void* destination, const void* source, const size_t length)
	{
#ifdef _MSC_VER
		__try
		{
			memcpy(destination, source, length);
			return true;
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
			return false;
		}
#else
		if (IsBadReadPtr(source, length))
			return false;
		memcpy(destination, source, length);
		return true;
#endif
	}

	const char* Primitive(const GLenum mode)
	{
		switch (mode)
		{
		case GL_POINTS: return "POINTS";
		case GL_LINES: return "LINES";
		case GL_LINE_LOOP: return "LINE_LOOP";
		case GL_LINE_STRIP: return "LINE_STRIP";
		case GL_TRIANGLES: return "TRIANGLES";
		case GL_TRIANGLE_STRIP: return "TRIANGLE_STRIP";
		case GL_TRIANGLE_FAN: return "TRIANGLE_FAN";
		case GL_QUADS: return "QUADS";
		case GL_QUAD_STRIP: return "QUAD_STRIP";
		case GL_POLYGON: return "POLYGON";
		default: return "?";
		}
	}

	const char* Name(const GLenum value)
	{
		switch (value)
		{
		case GL_ZERO: return "ZERO";
		case GL_ONE: return "ONE";
		case GL_SRC_COLOR: return "SRC_COLOR";
		case GL_ONE_MINUS_SRC_COLOR: return "ONE_MINUS_SRC_COLOR";
		case GL_DST_COLOR: return "DST_COLOR";
		case GL_ONE_MINUS_DST_COLOR: return "ONE_MINUS_DST_COLOR";
		case GL_SRC_ALPHA: return "SRC_ALPHA";
		case GL_ONE_MINUS_SRC_ALPHA: return "ONE_MINUS_SRC_ALPHA";
		case GL_DST_ALPHA: return "DST_ALPHA";
		case GL_ONE_MINUS_DST_ALPHA: return "ONE_MINUS_DST_ALPHA";
		case GL_CONSTANT_COLOR: return "CONSTANT_COLOR";
		case GL_CONSTANT_ALPHA: return "CONSTANT_ALPHA";
		case GL_FUNC_ADD: return "ADD";
		case GL_FUNC_SUBTRACT: return "SUBTRACT";
		case GL_FUNC_REVERSE_SUBTRACT: return "REVERSE_SUBTRACT";
		case GL_MIN: return "MIN";
		case GL_MAX: return "MAX";
		case GL_NEVER: return "NEVER";
		case GL_LESS: return "LESS";
		case GL_EQUAL: return "EQUAL";
		case GL_LEQUAL: return "LEQUAL";
		case GL_GREATER: return "GREATER";
		case GL_NOTEQUAL: return "NOTEQUAL";
		case GL_GEQUAL: return "GEQUAL";
		case GL_ALWAYS: return "ALWAYS";
		case GL_FRONT: return "FRONT";
		case GL_BACK: return "BACK";
		case GL_FRONT_AND_BACK: return "FRONT_AND_BACK";
		case GL_CW: return "CW";
		case GL_CCW: return "CCW";
		case GL_FLOAT: return "FLOAT";
		case GL_UNSIGNED_BYTE: return "UNSIGNED_BYTE";
		case GL_UNSIGNED_SHORT: return "UNSIGNED_SHORT";
		case GL_UNSIGNED_INT: return "UNSIGNED_INT";
		case GL_SHORT: return "SHORT";
		case GL_INT: return "INT";
		case GL_BYTE: return "BYTE";
		case GL_DOUBLE: return "DOUBLE";
		case GL_RGBA8: return "RGBA8";
		case GL_RGBA: return "RGBA";
		case GL_RGB8: return "RGB8";
		case GL_RGB: return "RGB";
		case GL_DEPTH_COMPONENT: return "DEPTH";
		case GL_DEPTH_COMPONENT16: return "DEPTH16";
		case GL_DEPTH_COMPONENT24: return "DEPTH24";
		case GL_DEPTH_COMPONENT32: return "DEPTH32";
		case GL_DEPTH_COMPONENT32F: return "DEPTH32F";
		case GL_DEPTH24_STENCIL8: return "DEPTH24_STENCIL8";
		case GL_DEPTH32F_STENCIL8: return "DEPTH32F_STENCIL8";
		case GL_RGBA16F: return "RGBA16F";
		case GL_RGBA32F: return "RGBA32F";
		case GL_COMPRESSED_RGBA_BPTC_UNORM: return "BC7";
		case GL_COMPRESSED_RGBA_S3TC_DXT1_EXT: return "BC1";
		case GL_COMPRESSED_RGBA_S3TC_DXT3_EXT: return "BC2";
		case GL_COMPRESSED_RGBA_S3TC_DXT5_EXT: return "BC3";
		default:
		{
			static char text[16];
			snprintf(text, sizeof(text), "0x%X", value);
			return text;
		}
		}
	}

	const char* TextureTypeName(const int type)
	{
		switch (type)
		{
		case 11: return "BATTLE";
		case 18: return "WORLDMAP";
		case 25: return "FIELDBG";
		case 35: return "BATTLECHARACTER";
		case 41: return "FIELDBG 41";
		case 57: return "FIELDENTITY";
		default: return "";
		}
	}

	//a short name for a shader program, from what its fragment shader has in it
	std::string Guess(const std::string& source)
	{
		//main.frag first (an old DEMASTER one has the PBR uniforms of demaster_field.frag)
		if (source.find("effectoption") != std::string::npos)
			return source.find("demasterPbr") != std::string::npos ? "main.frag (DEMASTER v13/v14, PBR inside: old)"
				: source.find("demasterTileClamp") != std::string::npos ? "main.frag (DEMASTER)" : "main.frag (the game's)";
		if (source.find("uniform sampler2D demasterCells") != std::string::npos)
			return "demaster_field.frag (DEMASTER, PBR pages)";
		if (source.find("uniform vec4 demasterCharMode") != std::string::npos)
			return "demaster_chara.frag (DEMASTER, characters)";
		if (source.find("DEMASTER (FIELD_PBR_SHADOWS)") != std::string::npos)
			return "demaster_shadow.frag (DEMASTER, shadow maps)";
		if (source.find("demasterPbr") != std::string::npos)
			return "main.frag (DEMASTER, PBR)";
		if (source.find("demasterTileClamp") != std::string::npos)
			return "main.frag (DEMASTER)";
		if (source.find("ambientColor0") != std::string::npos)
			return "main_extend.frag?";
		if (source.find("ambientColor") != std::string::npos)
			return "main_char.frag?";
		if (source.find("texD") != std::string::npos)
			return "main_2xsal_color_depth.frag?";
		if (source.find("gl_FragDepth") != std::string::npos)
			return "main_2xsal_depth.frag?";
		if (source.find("kernel[") != std::string::npos)
			return "main_pcsettings_bg.frag?";
		if (source.find("effectoption") != std::string::npos)
			return "main.frag (the game's)";
		if (source.find("SMAA") != std::string::npos || source.find("smaa") != std::string::npos)
			return "SMAA";
		if (source.find("FXAA") != std::string::npos || source.find("fxaa") != std::string::npos)
			return "FXAA";
		if (source.find("ProjMtx") != std::string::npos)
			return "ImGui (DEMASTER)";
		return "";
	}

	//first time a program is drawn with: its shaders' sources into programs.txt, and a short name
	const std::string& Program(const GLuint program)
	{
		const auto known = programNames.find(program);
		if (known != programNames.end())
			return known->second;
		std::string label;
		const std::string path = folder + "programs.txt";
		FILE* file = fopen(path.c_str(), "ab");
		if (file != nullptr)
			fprintf(file, "\n=============== program %u\n", program);
		GLuint shaders[8]{};
		GLsizei count = 0;
		if (program != 0)
			glGetAttachedShaders(program, 8, &count, shaders);
		for (GLsizei i = 0; i < count; i++)
		{
			GLint type = 0, length = 0;
			glGetShaderiv(shaders[i], GL_SHADER_TYPE, &type);
			glGetShaderiv(shaders[i], GL_SHADER_SOURCE_LENGTH, &length);
			std::string source;
			if (length > 0 && length < (1 << 20))
			{
				source.assign(static_cast<size_t>(length), '\0');
				glGetShaderSource(shaders[i], length, nullptr, source.data());
				source.resize(strlen(source.c_str()));
			}
			if (type == GL_FRAGMENT_SHADER)
				label = Guess(source);
			if (file != nullptr)
				fprintf(file, "----- %s shader %u\n%s\n", type == GL_FRAGMENT_SHADER ? "fragment" : type == GL_VERTEX_SHADER ? "vertex" : "other",
					shaders[i], source.empty() ? "(source not available)" : source.c_str());
		}
		if (file != nullptr)
			fclose(file);
		return programNames.emplace(program, label).first->second;
	}

	//every active uniform of the program but DEMASTER's own, with its value now
	void Uniforms(const GLuint program)
	{
		if (program == 0)
			return;
		GLint count = 0;
		glGetProgramiv(program, GL_ACTIVE_UNIFORMS, &count);
		std::string line;
		for (GLint i = 0; i < count && i < 64; i++)
		{
			char name[128]{};
			GLsizei length = 0;
			GLint size = 0;
			GLenum type = 0;
			glGetActiveUniform(program, static_cast<GLuint>(i), sizeof(name), &length, &size, &type, name);
			if (strncmp(name, "demaster", 8) == 0 || strncmp(name, "gl_", 3) == 0)
				continue;
			const GLint location = glGetUniformLocation(program, name);
			if (location < 0)
				continue;
			char text[512];
			if (type == GL_INT || type == GL_BOOL || type == GL_SAMPLER_2D)
			{
				GLint v = 0;
				glGetUniformiv(program, location, &v);
				snprintf(text, sizeof(text), "%s%s %d", type == GL_SAMPLER_2D ? "sampler " : "", name, v);
			}
			else if (type == GL_FLOAT || type == GL_FLOAT_VEC2 || type == GL_FLOAT_VEC3 || type == GL_FLOAT_VEC4)
			{
				GLfloat v[4]{};
				glGetUniformfv(program, location, v);
				const int n = type == GL_FLOAT ? 1 : type == GL_FLOAT_VEC2 ? 2 : type == GL_FLOAT_VEC3 ? 3 : 4;
				snprintf(text, sizeof(text), "%s%s (", name, size > 1 ? "[0]" : "");
				for (int k = 0; k < n; k++)
					snprintf(text + strlen(text), sizeof(text) - strlen(text), "%s%.6g", k ? ", " : "", v[k]);
				snprintf(text + strlen(text), sizeof(text) - strlen(text), ")%s", size > 1 ? (" of " + std::to_string(size)).c_str() : "");
			}
			else if (type == GL_FLOAT_MAT4)
			{
				GLfloat m[16]{};
				glGetUniformfv(program, location, m);
				snprintf(text, sizeof(text), "%s (columns) [%.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g]",
					name, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
			}
			else
				snprintf(text, sizeof(text), "%s (type %s, size %d)", name, Name(type), size);
			line += std::string(line.empty() ? "" : "; ") + text;
		}
		if (!line.empty())
			Write("   uniforms: %s\n", line.c_str());
	}

	std::string Attachment(const GLenum attachment)
	{
		GLint type = GL_NONE, object = 0;
		glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
		if (type == GL_NONE)
			return "";
		glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object);
		GLint width = 0, height = 0, format = 0;
		if (type == GL_TEXTURE)
		{
			GLint active = 0, bound = 0;
			glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
			glActiveTexture(GL_TEXTURE0);
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
			glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(object));
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
			glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
			glActiveTexture(static_cast<GLenum>(active));
		}
		else
		{
			GLint bound = 0;
			glGetIntegerv(GL_RENDERBUFFER_BINDING, &bound);
			glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(object));
			glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
			glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
			glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &format);
			glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(bound));
		}
		char text[128];
		snprintf(text, sizeof(text), "%s %d %dx%d %s", type == GL_TEXTURE ? "texture" : "renderbuffer", object, width, height,
			Name(static_cast<GLenum>(format)));
		return text;
	}

	//the buffer this draw goes into
	void Target()
	{
		GLint framebuffer = 0, viewport[4]{};
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
		glGetIntegerv(GL_VIEWPORT, viewport);
		std::string description;
		if (framebuffer != 0 && framebuffers.insert(static_cast<GLuint>(framebuffer)).second)
		{
			const std::string color = Attachment(GL_COLOR_ATTACHMENT0), depth = Attachment(GL_DEPTH_ATTACHMENT),
				stencil = Attachment(GL_STENCIL_ATTACHMENT), color1 = Attachment(GL_COLOR_ATTACHMENT1);
			description = " (first time: colour " + (color.empty() ? "none" : color) + ", depth " + (depth.empty() ? "none" : depth)
				+ (stencil.empty() ? "" : ", stencil " + stencil) + (color1.empty() ? "" : ", colour 1 " + color1) + ")";
		}
		else if (framebuffer == 0)
			framebuffers.insert(0);
		Write("   into: %s%s, viewport %d,%d %dx%d\n", framebuffer == 0 ? "the screen" : ("framebuffer " + std::to_string(framebuffer)).c_str(),
			description.c_str(), viewport[0], viewport[1], viewport[2], viewport[3]);
	}

	void State()
	{
		std::string line;
		char text[256];
		if (glIsEnabled(GL_BLEND))
		{
			GLint srcRgb = 0, dstRgb = 0, srcAlpha = 0, dstAlpha = 0, equation = 0;
			glGetIntegerv(GL_BLEND_SRC_RGB, &srcRgb);
			glGetIntegerv(GL_BLEND_DST_RGB, &dstRgb);
			glGetIntegerv(GL_BLEND_SRC_ALPHA, &srcAlpha);
			glGetIntegerv(GL_BLEND_DST_ALPHA, &dstAlpha);
			glGetIntegerv(GL_BLEND_EQUATION_RGB, &equation);
			snprintf(text, sizeof(text), "blend %s %s / alpha %s %s, %s", Name(srcRgb), Name(dstRgb), Name(srcAlpha), Name(dstAlpha), Name(equation));
			line += text;
		}
		else
			line += "no blend";
		if (glIsEnabled(GL_DEPTH_TEST))
		{
			GLint func = 0;
			GLboolean writes = GL_FALSE;
			GLfloat range[2]{};
			glGetIntegerv(GL_DEPTH_FUNC, &func);
			glGetBooleanv(GL_DEPTH_WRITEMASK, &writes);
			glGetFloatv(GL_DEPTH_RANGE, range);
			snprintf(text, sizeof(text), " | depth test %s, %s, range %.3g-%.3g", Name(func), writes ? "writes" : "no writes", range[0], range[1]);
			line += text;
		}
		else
		{
			GLboolean writes = GL_FALSE;
			glGetBooleanv(GL_DEPTH_WRITEMASK, &writes);
			line += writes ? " | no depth test (writes on, but nothing written without the test)" : " | no depth test";
		}
		if (glIsEnabled(GL_CULL_FACE))
		{
			GLint mode = 0, front = 0;
			glGetIntegerv(GL_CULL_FACE_MODE, &mode);
			glGetIntegerv(GL_FRONT_FACE, &front);
			snprintf(text, sizeof(text), " | cull %s (front %s)", Name(mode), Name(front));
			line += text;
		}
		else
			line += " | no culling";
		if (glIsEnabled(GL_ALPHA_TEST))
		{
			GLint func = 0;
			GLfloat ref = 0;
			glGetIntegerv(GL_ALPHA_TEST_FUNC, &func);
			glGetFloatv(GL_ALPHA_TEST_REF, &ref);
			snprintf(text, sizeof(text), " | alpha test %s %.3g", Name(func), ref);
			line += text;
		}
		if (glIsEnabled(GL_STENCIL_TEST))
			line += " | stencil test";
		if (glIsEnabled(GL_SCISSOR_TEST))
		{
			GLint box[4]{};
			glGetIntegerv(GL_SCISSOR_BOX, box);
			snprintf(text, sizeof(text), " | scissor %d,%d %dx%d", box[0], box[1], box[2], box[3]);
			line += text;
		}
		GLboolean mask[4]{};
		glGetBooleanv(GL_COLOR_WRITEMASK, mask);
		if (!mask[0] || !mask[1] || !mask[2] || !mask[3])
		{
			snprintf(text, sizeof(text), " | colour writes %c%c%c%c", mask[0] ? 'R' : '-', mask[1] ? 'G' : '-', mask[2] ? 'B' : '-', mask[3] ? 'A' : '-');
			line += text;
		}
		Write("   %s\n", line.c_str());
	}

	void Texture()
	{
		GLint active = 0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
		std::string line;
		for (int unit = 0; unit < 4; unit++)
		{
			glActiveTexture(GL_TEXTURE0 + unit);
			GLint bound = 0;
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
			if (bound == 0 || (unit > 0 && !glIsEnabled(GL_TEXTURE_2D) && unit != 0))
			{
				if (unit == 0)
					line += "unit 0: none";
				continue;
			}
			GLint width = 0, height = 0, format = 0, minFilter = 0;
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &format);
			glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &minFilter);
			char text[256];
			const auto type = uploadType.find(static_cast<GLuint>(bound));
			const char* what = FieldLayersIsPage(static_cast<GLuint>(bound)) ? (FieldPbrHasPage(static_cast<GLuint>(bound))
				? ", DEMASTER field page from layers, with PBR maps" : ", DEMASTER field page from layers") : "";
			snprintf(text, sizeof(text), "%sunit %d: texture %d %dx%d %s%s%s%s%s", line.empty() ? "" : "; ", unit, bound, width, height,
				Name(static_cast<GLenum>(format)), minFilter == GL_NEAREST ? " nearest" : " filtered",
				type != uploadType.end() ? (", filled as type " + std::to_string(type->second)).c_str() : "",
				type != uploadType.end() && TextureTypeName(type->second)[0] ? (std::string(" ") + TextureTypeName(type->second)).c_str() : "", what);
			line += text;
			if (unit == 0 && texturesSeen.insert(static_cast<GLuint>(bound)).second)
				textures.push_back(static_cast<GLuint>(bound));
		}
		glActiveTexture(static_cast<GLenum>(active));
		Write("   texture %s\n", line.c_str());
	}

	struct Array
	{
		bool enabled = false;
		GLint size = 0, type = 0, stride = 0, buffer = 0;
		const uint8_t* pointer = nullptr;
		int Bytes() const { return size * (type == GL_FLOAT || type == GL_INT || type == GL_UNSIGNED_INT ? 4 : type == GL_DOUBLE ? 8 : type == GL_SHORT || type == GL_UNSIGNED_SHORT ? 2 : 1); }
		int Stride() const { return stride != 0 ? stride : Bytes(); }
	};

	Array ReadArray(const GLenum which, const GLenum size, const GLenum type, const GLenum stride, const GLenum pointer, const GLenum binding)
	{
		Array a;
		a.enabled = glIsEnabled(which) == GL_TRUE;
		if (!a.enabled)
			return a;
		if (size != 0)
			glGetIntegerv(size, &a.size);
		else
			a.size = 3;
		glGetIntegerv(type, &a.type);
		glGetIntegerv(stride, &a.stride);
		glGetIntegerv(binding, &a.buffer);
		void* p = nullptr;
		glGetPointerv(pointer, &p);
		a.pointer = static_cast<const uint8_t*>(p);
		return a;
	}

	//one vertex's bytes of an array (client memory or a buffer object)
	bool Fetch(const Array& a, const uint32_t index, uint8_t* bytes)
	{
		const size_t offset = static_cast<size_t>(index) * a.Stride();
		if (a.buffer == 0)
			return a.pointer != nullptr && SafeCopy(bytes, a.pointer + offset, static_cast<size_t>(a.Bytes()));
		GLint bound = 0;
		glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &bound);
		glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(a.buffer));
		glGetBufferSubData(GL_ARRAY_BUFFER, reinterpret_cast<GLintptr>(a.pointer) + static_cast<GLintptr>(offset), a.Bytes(), bytes);
		glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(bound));
		return true;
	}

	void Floats(const Array& a, const uint8_t* bytes, double* out)
	{
		for (int k = 0; k < a.size && k < 4; k++)
		{
			switch (a.type)
			{
			case GL_FLOAT: { float f; memcpy(&f, bytes + k * 4, 4); out[k] = f; break; }
			case GL_DOUBLE: { double d; memcpy(&d, bytes + k * 8, 8); out[k] = d; break; }
			case GL_SHORT: { int16_t s; memcpy(&s, bytes + k * 2, 2); out[k] = s; break; }
			case GL_INT: { int32_t s; memcpy(&s, bytes + k * 4, 4); out[k] = s; break; }
			case GL_UNSIGNED_BYTE: out[k] = bytes[k]; break;
			default: out[k] = 0; break;
			}
		}
	}

	//the vertices a draw uses: arrays, ranges, triangle winding, the first few in full
	void Vertices(const GLenum mode, const GLsizei count, const GLenum indexType, const void* indices, const bool elements, const GLint first)
	{
		const Array position = ReadArray(GL_VERTEX_ARRAY, GL_VERTEX_ARRAY_SIZE, GL_VERTEX_ARRAY_TYPE, GL_VERTEX_ARRAY_STRIDE,
			GL_VERTEX_ARRAY_POINTER, GL_VERTEX_ARRAY_BUFFER_BINDING);
		const Array color = ReadArray(GL_COLOR_ARRAY, GL_COLOR_ARRAY_SIZE, GL_COLOR_ARRAY_TYPE, GL_COLOR_ARRAY_STRIDE,
			GL_COLOR_ARRAY_POINTER, GL_COLOR_ARRAY_BUFFER_BINDING);
		const Array normal = ReadArray(GL_NORMAL_ARRAY, 0, GL_NORMAL_ARRAY_TYPE, GL_NORMAL_ARRAY_STRIDE,
			GL_NORMAL_ARRAY_POINTER, GL_NORMAL_ARRAY_BUFFER_BINDING);
		GLint clientUnit = 0;
		glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &clientUnit);
		glClientActiveTexture(GL_TEXTURE0);
		const Array uv = ReadArray(GL_TEXTURE_COORD_ARRAY, GL_TEXTURE_COORD_ARRAY_SIZE, GL_TEXTURE_COORD_ARRAY_TYPE, GL_TEXTURE_COORD_ARRAY_STRIDE,
			GL_TEXTURE_COORD_ARRAY_POINTER, GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING);
		glClientActiveTexture(static_cast<GLenum>(clientUnit));
		const auto describe = [](const char* what, const Array& a) -> std::string
		{
			if (!a.enabled)
				return std::string(what) + " off";
			char text[160];
			snprintf(text, sizeof(text), "%s %d x %s, stride %d, %s", what, a.size, Name(static_cast<GLenum>(a.type)), a.stride,
				a.buffer ? ("buffer " + std::to_string(a.buffer)).c_str() : "client memory");
			return text;
		};
		GLint elementBuffer = 0;
		glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &elementBuffer);
		Write("   arrays: %s; %s; %s; %s%s\n", describe("position", position).c_str(), describe("colour", color).c_str(),
			describe("uv", uv).c_str(), describe("normal", normal).c_str(),
			elements ? (std::string("; indices ") + Name(indexType) + (elementBuffer ? " in buffer " + std::to_string(elementBuffer) : " in client memory")).c_str() : "");
		if (!position.enabled || count <= 0 || count > 200000)
			return;
		//the indices
		std::vector<uint32_t> list(static_cast<size_t>(count));
		if (!elements)
			for (GLsizei i = 0; i < count; i++)
				list[i] = static_cast<uint32_t>(first + i);
		else
		{
			const int size = indexType == GL_UNSIGNED_INT ? 4 : indexType == GL_UNSIGNED_SHORT ? 2 : 1;
			std::vector<uint8_t> raw(static_cast<size_t>(count) * size);
			bool ok;
			if (elementBuffer != 0)
			{
				glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, reinterpret_cast<GLintptr>(indices), static_cast<GLsizeiptr>(raw.size()), raw.data());
				ok = true;
			}
			else
				ok = indices != nullptr && SafeCopy(raw.data(), indices, raw.size());
			if (!ok)
			{
				Write("   indices can't be read\n");
				return;
			}
			for (GLsizei i = 0; i < count; i++)
				list[i] = size == 4 ? *reinterpret_cast<const uint32_t*>(raw.data() + i * 4)
				: size == 2 ? *reinterpret_cast<const uint16_t*>(raw.data() + i * 2) : raw[i];
		}
		std::vector<uint32_t> unique(list);
		std::sort(unique.begin(), unique.end());
		unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
		std::unordered_map<uint32_t, std::array<double, 4>> positions;
		double lo[4] = { 1e300, 1e300, 1e300, 1e300 }, hi[4] = { -1e300, -1e300, -1e300, -1e300 };
		uint8_t bytes[64];
		if (unique.size() > 8192)
			Write("   (only the first 8192 of %zu vertices read)\n", unique.size());
		size_t done = 0;
		for (const uint32_t index : unique)
		{
			if (done++ >= 8192)
				break;
			if (position.Bytes() > static_cast<int>(sizeof(bytes)) || !Fetch(position, index, bytes))
				continue;
			std::array<double, 4> p{ 0, 0, 0, 1 };
			Floats(position, bytes, p.data());
			positions[index] = p;
			for (int k = 0; k < position.size && k < 4; k++)
			{
				lo[k] = std::min(lo[k], p[k]);
				hi[k] = std::max(hi[k], p[k]);
			}
		}
		if (positions.empty())
		{
			Write("   vertices can't be read\n");
			return;
		}
		//triangles, which way they turn on screen (x, y as stored: for vertices already on screen that is the screen)
		size_t one = 0, other = 0, flat = 0;
		const auto triangle = [&](const uint32_t a, const uint32_t b, const uint32_t c)
		{
			const auto pa = positions.find(a), pb = positions.find(b), pc = positions.find(c);
			if (pa == positions.end() || pb == positions.end() || pc == positions.end())
				return;
			const double area = (pb->second[0] - pa->second[0]) * (pc->second[1] - pa->second[1])
				- (pc->second[0] - pa->second[0]) * (pb->second[1] - pa->second[1]);
			if (std::fabs(area) < 1e-9)
				flat++;
			else if (area > 0)
				one++;
			else
				other++;
		};
		if (mode == GL_TRIANGLES)
			for (size_t i = 0; i + 2 < list.size(); i += 3)
				triangle(list[i], list[i + 1], list[i + 2]);
		else if (mode == GL_TRIANGLE_STRIP)
			for (size_t i = 0; i + 2 < list.size(); i++)
				(i % 2) ? triangle(list[i + 1], list[i], list[i + 2]) : triangle(list[i], list[i + 1], list[i + 2]);
		else if (mode == GL_TRIANGLE_FAN)
			for (size_t i = 1; i + 1 < list.size(); i++)
				triangle(list[0], list[i], list[i + 1]);
		else if (mode == GL_QUADS)
			for (size_t i = 0; i + 3 < list.size(); i += 4)
				triangle(list[i], list[i + 1], list[i + 2]);
		Write("   %zu indices, %zu different vertices; triangles: %zu turn one way, %zu the other, %zu flat\n", list.size(), unique.size(),
			one, other, flat);
		const char* axis[4] = { "x", "y", "z", "w" };
		std::string ranges;
		for (int k = 0; k < position.size && k < 4; k++)
		{
			char text[80];
			snprintf(text, sizeof(text), "%s%s %.6g..%.6g", k ? ", " : "", axis[k], lo[k], hi[k]);
			ranges += text;
		}
		Write("   position ranges: %s\n", ranges.c_str());
		for (size_t i = 0; i < unique.size() && i < 4; i++)
		{
			const uint32_t index = unique[i];
			const auto& p = positions[index];
			std::string line;
			char text[200];
			snprintf(text, sizeof(text), "   vertex %u: position (%.6g, %.6g, %.6g, %.6g)", index, p[0], p[1], p[2], p[3]);
			line = text;
			if (color.enabled && color.Bytes() <= static_cast<int>(sizeof(bytes)) && Fetch(color, index, bytes))
			{
				double c[4]{};
				Floats(color, bytes, c);
				snprintf(text, sizeof(text), " colour (%.4g, %.4g, %.4g, %.4g)", c[0], c[1], c[2], c[3]);
				line += text;
			}
			if (uv.enabled && uv.Bytes() <= static_cast<int>(sizeof(bytes)) && Fetch(uv, index, bytes))
			{
				double t[4]{};
				Floats(uv, bytes, t);
				snprintf(text, sizeof(text), " uv (%.6g, %.6g)", t[0], t[1]);
				line += text;
			}
			if (normal.enabled && normal.Bytes() <= static_cast<int>(sizeof(bytes)) && Fetch(normal, index, bytes))
			{
				double n[4]{};
				Floats(normal, bytes, n);
				snprintf(text, sizeof(text), " normal (%.4g, %.4g, %.4g)", n[0], n[1], n[2]);
				line += text;
			}
			Write("%s\n", line.c_str());
		}
	}

	void Matrices()
	{
		GLfloat modelview[16]{}, projection[16]{};
		glGetFloatv(GL_MODELVIEW_MATRIX, modelview);
		glGetFloatv(GL_PROJECTION_MATRIX, projection);
		const auto identity = [](const GLfloat* m)
		{
			for (int i = 0; i < 16; i++)
				if (std::fabs(m[i] - (i % 5 == 0 ? 1.0f : 0.0f)) > 1e-6f)
					return false;
			return true;
		};
		const auto print = [](const char* name, const GLfloat* m)
		{
			Write("   %s (columns) [%.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g]\n", name,
				m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
		};
		if (!identity(modelview))
			print("GL modelview", modelview);
		if (!identity(projection))
			print("GL projection", projection);
	}

	void Draw(const char* call, const GLenum mode, const GLsizei count, const GLenum indexType, const void* indices, const bool elements, const GLint first)
	{
		GLint program = 0;
		glGetIntegerv(GL_CURRENT_PROGRAM, &program);
		const std::string& label = Program(static_cast<GLuint>(program));
		Write("\n#%d %s %s %d\n", drawIndex++, call, Primitive(mode), count);
		Target();
		Write("   program %d%s%s\n", program, label.empty() ? "" : ": ", label.c_str());
		Uniforms(static_cast<GLuint>(program));
		State();
		Texture();
		Matrices();
		if (call[2] != 'B') //glBegin: the vertices come one by one, not from arrays
			Vertices(mode, count, indexType, indices, elements, first);
	}

	void WritePng(const std::string& path, const int width, const int height, const std::vector<uint8_t>& rgba)
	{
		bx::FileWriter writer;
		if (!bx::open(&writer, bx::FilePath(path.c_str())))
			return;
		bimg::imageWritePng(&writer, static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(width * 4), rgba.data(),
			bimg::TextureFormat::RGBA8, true);
		bx::close(&writer);
	}

	//end of the frame: what each buffer holds now, and the textures
	void SaveBuffers()
	{
		GLint read = 0, pack = 4, packBuffer = 0, readBuffer = 0;
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read);
		glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
		glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
		glGetIntegerv(GL_READ_BUFFER, &readBuffer);
		if (packBuffer != 0)
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		Write("\n=============== end of the frame: %d draws\n", drawIndex);
		for (const GLuint framebuffer : framebuffers)
		{
			glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
			int width = 0, height = 0;
			if (framebuffer == 0)
			{
				GLint viewport[4]{};
				glGetIntegerv(GL_VIEWPORT, viewport);
				width = ffWindowWidth > 0 ? ffWindowWidth : viewport[2];
				height = ffWindowHeight > 0 ? ffWindowHeight : viewport[3];
				glReadBuffer(GL_BACK);
			}
			else
			{
				for (const GLenum attachment : { GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT })
				{
					GLint type = GL_NONE, object = 0;
					glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
					if (type == GL_NONE || width > 0)
						continue;
					glGetFramebufferAttachmentParameteriv(GL_READ_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &object);
					if (type == GL_TEXTURE)
					{
						GLint bound = 0;
						glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
						glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(object));
						glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
						glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
						glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
					}
					else
					{
						GLint bound = 0;
						glGetIntegerv(GL_RENDERBUFFER_BINDING, &bound);
						glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(object));
						glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
						glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
						glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(bound));
					}
				}
				glReadBuffer(GL_COLOR_ATTACHMENT0);
			}
			if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
				continue;
			const std::string name = framebuffer == 0 ? "screen" : "framebuffer_" + std::to_string(framebuffer);
			for (int k = 0; k < 4 && glGetError() != GL_NO_ERROR; k++) {}
			std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
			glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
			const bool colorOk = glGetError() == GL_NO_ERROR;
			if (colorOk)
				WritePng(folder + name + "_colour.png", width, height, rgba);
			//depth: exact, 24 bits packed into red (high), green, blue; and a picture of it stretched over its range
			std::vector<float> depth(static_cast<size_t>(width) * height);
			glReadPixels(0, 0, width, height, GL_DEPTH_COMPONENT, GL_FLOAT, depth.data());
			const bool depthOk = glGetError() == GL_NO_ERROR;
			float lo = 1, hi = 0;
			if (depthOk)
			{
				for (const float d : depth)
				{
					lo = std::min(lo, d);
					hi = std::max(hi, d);
				}
				std::vector<uint8_t> packed(rgba.size()), view(rgba.size());
				for (size_t i = 0; i < depth.size(); i++)
				{
					const uint32_t v = static_cast<uint32_t>(std::clamp(depth[i], 0.0f, 1.0f) * 16777215.0f + 0.5f);
					packed[i * 4] = static_cast<uint8_t>(v >> 16);
					packed[i * 4 + 1] = static_cast<uint8_t>(v >> 8);
					packed[i * 4 + 2] = static_cast<uint8_t>(v);
					packed[i * 4 + 3] = 255;
					const uint8_t g = hi > lo ? static_cast<uint8_t>((depth[i] - lo) / (hi - lo) * 255.0f + 0.5f) : 0;
					view[i * 4] = view[i * 4 + 1] = view[i * 4 + 2] = g;
					view[i * 4 + 3] = 255;
				}
				WritePng(folder + name + "_depth24.png", width, height, packed);
				WritePng(folder + name + "_depth_view.png", width, height, view);
			}
			char range[96] = "";
			if (depthOk)
				snprintf(range, sizeof(range), " (from %.7f to %.7f)", lo, hi);
			Write("%s %dx%d: colour %s, depth %s%s\n", name.c_str(), width, height, colorOk ? "saved" : "can't be read",
				depthOk ? "saved" : "can't be read", range);
		}
		glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read));
		glReadBuffer(static_cast<GLenum>(readBuffer));
		//the textures drawn with on unit 0 (not DEMASTER's field pages: you have those)
		GLint active = 0, bound = 0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
		glActiveTexture(GL_TEXTURE0);
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
		size_t saved = 0;
		for (const GLuint texture : textures)
		{
			if (saved >= 120 || !glIsTexture(texture) || FieldLayersIsPage(texture))
				continue;
			glBindTexture(GL_TEXTURE_2D, texture);
			GLint width = 0, height = 0;
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &width);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &height);
			if (width <= 0 || height <= 0 || width > 2048 || height > 2048)
				continue;
			std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
			for (int k = 0; k < 4 && glGetError() != GL_NO_ERROR; k++) {}
			glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
			if (glGetError() != GL_NO_ERROR)
				continue;
			bx::FileWriter writer;
			const std::string path = folder + "texture_" + std::to_string(texture) + ".png";
			if (bx::open(&writer, bx::FilePath(path.c_str())))
			{
				bimg::imageWritePng(&writer, static_cast<uint32_t>(width), static_cast<uint32_t>(height), static_cast<uint32_t>(width * 4),
					rgba.data(), bimg::TextureFormat::RGBA8, false);
				bx::close(&writer);
				saved++;
			}
		}
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
		glActiveTexture(static_cast<GLenum>(active));
		glPixelStorei(GL_PACK_ALIGNMENT, pack);
		if (packBuffer != 0)
			glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
		Write("%zu textures saved (texture_<number>.png; the numbers are the ones above)\n", saved);
	}

	void Begin()
	{
		CheckGlew();
		const std::string field = FieldLayersFieldName();
		const time_t now = time(nullptr);
		char stamp[32];
		strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", localtime(&now));
		folder = std::string(DIRECT_IO_EXPORT_DIR) + "frame_log\\" + (field.empty() ? "nofield" : field) + "_" + stamp + "\\";
		std::error_code error;
		std::filesystem::create_directories(folder, error);
		out = fopen((folder + "frame.txt").c_str(), "wb");
		if (out == nullptr)
		{
			OutputDebug("FIELD_FRAME_LOG: can't write %s\n", folder.c_str());
			return;
		}
		drawIndex = 0;
		framebuffers.clear();
		programNames.clear();
		textures.clear();
		texturesSeen.clear();
		capturing = true;
		Write("DEMASTER frame log: field %s, window %dx%d, FIELD_BG_RESOLUTION %d, FIELD_PBR %d, FIELD_PBR_QUALITY %d, FIELD_GL_TRACK %d\n",
			field.empty() ? "(none)" : field.c_str(), ffWindowWidth, ffWindowHeight, FIELD_BG_RESOLUTION, FIELD_PBR, FIELD_PBR_QUALITY, FIELD_GL_TRACK);
		Write("OpenGL %s, %s, %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER), glGetString(GL_VENDOR));
		WriteCpuTimes();
		marks.clear();
		FrameLogMark("frame start (SwapBuffers)");
		Write("Every draw of one frame, in order. Shader sources are in programs.txt, the buffers and textures in this folder.\n");
		OutputDebug("FIELD_FRAME_LOG: logging one frame into %s\n", folder.c_str());
	}
}

//texture.cpp: the game filled a texture (its type is what the game was loading)
void FrameLogTextureFilled(const unsigned int texture)
{
	if (FIELD_FRAME_LOG && texture != 0)
		uploadType[texture] = static_cast<int>(TEX_TYPE);
}

//DEMASTER's own steps in the logged frame (the characters it draws itself, the round shadows it leaves out)
void FrameLogNote(const char* format, ...)
{
	if (!capturing || out == nullptr)
		return;
	char text[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(text, sizeof(text), format, args);
	va_end(args);
	Write("   DEMASTER: %s\n", text);
}

long long FieldTimeNow()
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return now.QuadPart;
}

void FieldTimeAdd(const int slot, const long long since)
{
	if (slot >= 0 && slot < TIME_SLOTS && since != 0)
		slotTicks[slot] += static_cast<double>(FieldTimeNow() - since);
}

void FrameLogMark(const char* label)
{
	if (!capturing || out == nullptr || glQueryCounter == nullptr || glGenQueries == nullptr)
		return;
	Mark m;
	glGenQueries(1, &m.query);
	glQueryCounter(m.query, GL_TIMESTAMP);
	m.label = label;
	marks.push_back(m);
}

//opengl.cpp, before every glDrawElements
void FrameLogDraw(const unsigned int mode, const int count, const unsigned int type, const void* indices)
{
	if (!capturing)
		return;
	Draw("glDrawElements", mode, count, type, indices, true, 0);
}

static void __stdcall HookGlDrawArrays(const GLenum mode, const GLint first, const GLsizei count)
{
	if (capturing)
		Draw("glDrawArrays", mode, count, 0, nullptr, false, first);
	//FIELD_INTERPOLATION: the draw goes to the driver, and into the second picture when this frame has one
	struct Arrays
	{
		GLenum mode;
		GLint first;
		GLsizei count;
	} arrays{ mode, first, count };
	fieldrt::TweenOtherDraw([](void* context)
	{
		const Arrays& a = *static_cast<const Arrays*>(context);
		reinterpret_cast<void(__stdcall*)(GLenum, GLint, GLsizei)>(drawArraysOriginal)(a.mode, a.first, a.count);
	}, &arrays);
}

static void __stdcall HookGlBegin(const GLenum mode)
{
	if (capturing)
		Draw("glBegin", mode, 0, 0, nullptr, false, 0);
	fieldrt::TweenBeginDraw(); //FIELD_INTERPOLATION: this frame gets no in-between picture
	reinterpret_cast<void(__stdcall*)(GLenum)>(beginOriginal)(mode);
}

//opengl.cpp, with the other OpenGL hooks
void FrameLogCreateHooks()
{
	if (!FIELD_FRAME_LOG)
		return;
	MH_CreateHookApi(L"OPENGL32", "glDrawArrays", reinterpret_cast<LPVOID>(&HookGlDrawArrays), &drawArraysOriginal);
	MH_CreateHookApi(L"OPENGL32", "glBegin", reinterpret_cast<LPVOID>(&HookGlBegin), &beginOriginal);
	OutputDebug("FIELD_FRAME_LOG: on, Ctrl+F11 logs the next frame into DEMASTER_EXP\\frame_log\\\n");
}

//opengl.cpp, at the start of SwapBuffers: the frame being logged ends here; Ctrl+F11 asks for the next one
void FrameLogSwap()
{
	if (!FIELD_FRAME_LOG)
		return;
	//the frame's CPU times into the history
	if (ticksPerSecond == 0)
	{
		LARGE_INTEGER frequency;
		QueryPerformanceFrequency(&frequency);
		ticksPerSecond = frequency.QuadPart;
	}
	const long long now = FieldTimeNow();
	if (!capturing)
	{
		for (int s = 0; s < TIME_SLOTS; s++)
			history[historyNext][s] = slotTicks[s];
		history[historyNext][TIME_SLOTS] = lastSwap != 0 ? static_cast<double>(now - lastSwap) : 0.0;
		historyNext = (historyNext + 1) % TIME_FRAMES;
		historyCount = std::min(historyCount + 1, TIME_FRAMES);
	}
	memset(slotTicks, 0, sizeof(slotTicks));
	lastSwap = now;
	try
	{
		if (capturing)
		{
			FrameLogMark("frame end (SwapBuffers)");
			WriteGpuTimes();
			SaveBuffers();
			fclose(out);
			out = nullptr;
			capturing = false;
			OutputDebug("FIELD_FRAME_LOG: %d draws written to %sframe.txt\n", drawIndex, folder.c_str());
		}
		const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
		if (down && !keyWasDown)
			armed = true;
		keyWasDown = down;
		if (armed)
		{
			armed = false;
			Begin();
		}
	}
	catch (...)
	{
		if (out != nullptr)
			fclose(out);
		out = nullptr;
		capturing = false;
		OutputDebug("FIELD_FRAME_LOG: logging the frame failed\n");
	}
}
