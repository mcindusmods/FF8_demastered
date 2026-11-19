//Field characters and 3D models on PBR backgrounds (FIELD_PBR_CHARACTERS, FIELD_PBR_SHADOWS, FIELD_PBR_REFLECTIONS,
//FIELD_AVATARS). The shared logic (back to view space, smooth normals, which triangles are which character) is in
//field_chars.h; this file is the game side.
//
//The game draws every field model first in a frame, in runs of up to 32 polygons, already on its screen (TLVERTEX), with
//main_char.frag. On a field with PBR files DEMASTER keeps those draws instead of letting them through, and right before the
//next thing the game draws (the first background tile) it:
//	1. puts every vertex back into the room's view space (the background tiles of the last frame said where the field's
//	   canvas is on the screen) and gives the whole frame's models smooth normals;
//	2. tells the characters apart (one texture per model; the game's round shadows and its script entities for twins);
//	3. sends the frame's models to the GPU once (one vertex and one index buffer: every pass below draws from there), then
//	   draws the pictures reflections use (each character from the front and from behind), the characters as the field's
//	   lights see them (shadow maps), and the face pictures (FIELD_AVATARS). With instancing (GL_ARB_draw_instanced), one
//	   draw makes all of a character's shadow maps and one both of its pictures, each copy into its own tile;
//	4. draws the models on the game's screen with its own program (demaster_chara.vert/.frag): lit by the room's light grid
//	   and the field's lights, or exactly as main_char.frag would (FIELD_PBR_CHARACTERS=0), in the game's order: opaque
//	   parts first, semi-transparent ones after, each with the game's state;
//	5. hands the background program (texturepatch_v2_fieldPbr.cpp) the characters' cards, shadows and pictures.
//Back faces: the game throws them away in its model code (two 6-byte jumps). With shadows, reflections or avatars on, those
//jumps are turned off on PBR fields so the whole model arrives; the screen pass hides back faces on the GPU instead.
//The round shadows the game draws after the background are left out when DEMASTER's shadows are on; their size and
//darkness are kept (the next frame's contact darkening).
//Fields without PBR files: nothing here happens, the game draws as it always did.

#include "config.h"
#include "coreHeader.h"
#include "opengl.h"
#include "texture.h"
#include "field_pbr_runtime.h"
#include "field_chars.h"
#include "gl_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <numeric>
#include <unordered_map>
#include <vector>

using namespace fieldchars;
using namespace fieldrt;

namespace
{
	constexpr int UNIT_MODEL_N = 1, UNIT_MODEL_ORME = 2, UNIT_PROBES = 3, UNIT_ROOM = 4;
	constexpr int CAPTURE_SIZE = 1024, CAPTURE_TILE = 256; //4 x 4 tiles: character i front 2i, back 2i + 1
	//the shadow map atlas: FIELD_PBR_QUALITY's size and tile (CurrentQuality), up to its maxShadowMaps used
	constexpr int AVATAR_TILE = 512, AVATAR_COLUMNS = 4, AVATAR_ROWS = 2;
	constexpr float CREASE_DEGREES = 65.0f;
	constexpr int MAX_DRAWS = 1024;
	constexpr size_t MAX_VERTICES = 120000;

	//--- matrices (column-major, as OpenGL takes them)

	struct Mat4
	{
		float m[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
		float& At(const int row, const int col) { return m[col * 4 + row]; }
		float At(const int row, const int col) const { return m[col * 4 + row]; }
	};

	Mat4 Multiply(const Mat4& a, const Mat4& b)
	{
		Mat4 r;
		for (int row = 0; row < 4; row++)
			for (int col = 0; col < 4; col++)
			{
				float sum = 0;
				for (int k = 0; k < 4; k++)
					sum += a.At(row, k) * b.At(k, col);
				r.At(row, col) = sum;
			}
		return r;
	}

	//rows: x = axis[0] . (p - centre) / scale[0], and so on
	Mat4 Box(const V3 axes[3], const V3& centre, const float scale[3])
	{
		Mat4 r;
		for (int row = 0; row < 3; row++)
		{
			const V3& a = axes[row];
			r.At(row, 0) = a.x / scale[row];
			r.At(row, 1) = a.y / scale[row];
			r.At(row, 2) = a.z / scale[row];
			r.At(row, 3) = -Dot(a, centre) / scale[row];
		}
		return r;
	}

	Mat4 LookAt(const V3& eye, const V3& centre, const V3& upHint)
	{
		const V3 f = Normalize(centre - eye, { 0, 0, 1 });
		V3 s = Cross(f, upHint);
		if (Length(s) < 1e-4f)
			s = Cross(f, std::fabs(f.x) < 0.9f ? V3{ 1, 0, 0 } : V3{ 0, 0, 1 });
		s = Normalize(s, { 1, 0, 0 });
		const V3 u = Cross(s, f);
		Mat4 r;
		const V3 rows[3] = { s, u, f * -1.0f };
		for (int row = 0; row < 3; row++)
		{
			r.At(row, 0) = rows[row].x;
			r.At(row, 1) = rows[row].y;
			r.At(row, 2) = rows[row].z;
			r.At(row, 3) = -Dot(rows[row], eye);
		}
		return r;
	}

	Mat4 Perspective(const float halfAngle, const float nearZ, const float farZ)
	{
		const float f = 1.0f / std::tan(halfAngle);
		Mat4 r;
		r.m[0] = f;
		r.m[5] = f;
		r.m[10] = (farZ + nearZ) / (nearZ - farZ);
		r.m[11] = -1.0f;
		r.m[14] = 2.0f * farZ * nearZ / (nearZ - farZ);
		r.m[15] = 0.0f;
		return r;
	}

	//--- the game's state around a draw: gl_state.h (remembered as the game sets it, FIELD_GL_TRACK)
	using glstate::RenderState;
	using glstate::ClientState;
	using glstate::Apply;
	using glstate::RawViewport;
	using glstate::RawScissor;
	using glstate::Enable;
	using glstate::SaveClient;
	using glstate::RestoreClient;
	using glstate::Capture;

	//--- what is collected in a frame

	struct Vertex
	{
		float pos[4]; //as the game sent it
		uint8_t color[4];
		float uv[2];
		float view[3];
		float normal[3];
	};

	struct CharDraw
	{
		RenderState state;
		GLint program = 0; //the game's
		GLuint texture = 0;
		float ambient[3] = {};
		GLint blendMode = 0;
		float projection[16] = {};
		uint32_t first = 0, count = 0; //vertices (3 per triangle)
		bool opaque = true;
	};

	std::vector<Vertex> vertices;
	std::vector<uint64_t> keys;
	std::vector<CharDraw> draws;
	bool pending = false;

	//the round shadows of the last frame (the game draws them after the background)
	std::vector<Disc> lastDiscs;
	uint64_t lastDiscsEntry = 0;

	//--- the game's back-face cull in its model code (FIELD_CULL_TRI, FIELD_CULL_QUAD: 6-byte jumps)
	struct CullSite
	{
		DWORD at = 0;
		BYTE original[6] = {};
		bool known = false;
	};
	CullSite cullSites[2];
	std::atomic<bool> cullPatched{ false };
	std::atomic<bool> charsBroken{ false }; //DEMASTER's character programs could not be made

	//--- programs

	struct CharProgram
	{
		GLuint id = 0;
		GLint charMode = -1, screenMatrix = -1, viewMatrix = -1, blendMode = -1, model = -1, game = -1, shade = -1, probeBox = -1,
			probeInfo = -1, space = -1, depth = -1, roomSize = -1, features = -1, lightCount = -1, lightPos = -1, lightColor = -1,
			lightFlicker = -1, viewer = -1, quality = -1, instanceMatrix = -1, instanceTile = -1, instanceLook = -1, night = -1,
			nightTint = -1, effectAt = -1, effectColour = -1;
	};
	CharProgram charProgram, shadowProgram;
	bool programsTried = false;
	//instancing (GL_ARB_draw_instanced, GL 3.1): every shadow map of a character in one draw, both of its reflection pictures
	//in one. null: one draw each, as before
	PFNGLDRAWELEMENTSINSTANCEDPROC drawInstanced = nullptr;
	constexpr int MAX_INSTANCES = 8; //demasterInstanceMatrix[8] in demaster_chara.vert
	const char* const INSTANCED_DEFINES = "#extension GL_ARB_draw_instanced : require\n#define DEMASTER_INSTANCED 1\n";

	bool Wanted()
	{
		return FIELD_PBR && FIELD_LAYERS && (FIELD_PBR_CHARACTERS || FIELD_PBR_SHADOWS || FIELD_PBR_REFLECTIONS || FIELD_AVATARS
			|| FIELD_PBR_DEPTH);
	}

	bool WholeModels()
	{
		return FIELD_PBR_SHADOWS || FIELD_PBR_REFLECTIONS || FIELD_AVATARS;
	}

	void LookUp(CharProgram& p)
	{
		const GLuint id = p.id;
		const auto at = [id](const char* name) { return glGetUniformLocation(id, name); };
		p.charMode = at("demasterCharMode");
		p.screenMatrix = at("demasterScreenMatrix");
		p.viewMatrix = at("demasterViewMatrix");
		p.blendMode = at("demasterBlendMode");
		p.model = at("demasterModel");
		p.game = at("demasterGame");
		p.shade = at("demasterShade");
		p.probeBox = at("demasterProbeBox");
		p.probeInfo = at("demasterProbeInfo");
		p.space = at("demasterSpace");
		p.depth = at("demasterDepth");
		p.roomSize = at("demasterRoomSize");
		p.features = at("demasterFeatures");
		p.lightCount = at("demasterLightCount");
		p.lightPos = at("demasterLightPos");
		p.lightColor = at("demasterLightColor");
		p.lightFlicker = at("demasterLightFlicker");
		p.viewer = at("demasterViewer");
		p.effectAt = at("demasterEffectAt");
		p.effectColour = at("demasterEffectColour");
		p.quality = at("demasterQuality");
		p.instanceMatrix = at("demasterInstanceMatrix");
		p.instanceTile = at("demasterInstanceTile");
		p.instanceLook = at("demasterInstanceLook");
		p.night = at("demasterNight");
		p.nightTint = at("demasterNightTint");
		const GLint previous = glstate::Program();
		glUseProgram(id);
		const char* samplers[] = { "tex", "demasterModelN", "demasterModelORME", "demasterProbes", "demasterRoom" };
		const int units[] = { 0, UNIT_MODEL_N, UNIT_MODEL_ORME, UNIT_PROBES, UNIT_ROOM };
		for (int i = 0; i < 5; i++)
			if (const GLint location = at(samplers[i]); location >= 0)
				glUniform1i(location, units[i]);
		glUseProgram(static_cast<GLuint>(previous));
	}

	bool MakePrograms()
	{
		if (programsTried)
			return charProgram.id != 0;
		programsTried = true;
		std::string error;
		//with instancing when the driver has it (and the shaders take it), else without
		std::string instancedWhy;
		PFNGLDRAWELEMENTSINSTANCEDPROC instanced = nullptr;
		if (WholeModels())
		{
			CheckGlew();
			if (glDrawElementsInstanced != nullptr)
				instanced = glDrawElementsInstanced;
			else if (glDrawElementsInstancedARB != nullptr)
				instanced = reinterpret_cast<PFNGLDRAWELEMENTSINSTANCEDPROC>(glDrawElementsInstancedARB);
			else
				instancedWhy = "the driver has no glDrawElementsInstanced";
		}
		if (instanced != nullptr)
		{
			charProgram.id = BuildProgram("demaster_chara.vert", "demaster_chara.frag", instancedWhy, INSTANCED_DEFINES);
			if (charProgram.id != 0)
				shadowProgram.id = BuildProgram("demaster_chara.vert", "demaster_shadow.frag", instancedWhy, INSTANCED_DEFINES);
			if (charProgram.id != 0 && shadowProgram.id != 0)
			{
				charProgram.instanceMatrix = glGetUniformLocation(charProgram.id, "demasterInstanceMatrix");
				shadowProgram.instanceMatrix = glGetUniformLocation(shadowProgram.id, "demasterInstanceMatrix");
				if (charProgram.instanceMatrix < 0 || shadowProgram.instanceMatrix < 0)
					instancedWhy = "demaster_chara.vert has no instancing (copy DEMASTER_EXP\\shaders\\ from this release)";
				else
					drawInstanced = instanced;
			}
			if (drawInstanced == nullptr)
			{
				if (charProgram.id != 0)
					glDeleteProgram(charProgram.id);
				if (shadowProgram.id != 0)
					glDeleteProgram(shadowProgram.id);
				charProgram = CharProgram();
				shadowProgram = CharProgram();
			}
		}
		if (charProgram.id == 0)
			charProgram.id = BuildProgram("demaster_chara.vert", "demaster_chara.frag", error);
		if (charProgram.id != 0 && WholeModels() && shadowProgram.id == 0)
			shadowProgram.id = BuildProgram("demaster_chara.vert", "demaster_shadow.frag", error);
		if (charProgram.id == 0 || (WholeModels() && shadowProgram.id == 0))
		{
			charsBroken = true;
			Log("FIELD_PBR_CHARACTERS: %s. Field characters are drawn by the game as before. Copy DEMASTER_EXP\\shaders\\ from this "
				"release into the game's DEMASTER_EXP\\shaders\\.\n", error.c_str());
			if (charProgram.id != 0)
				glDeleteProgram(charProgram.id);
			charProgram.id = 0;
			return false;
		}
		LookUp(charProgram);
		if (shadowProgram.id != 0)
			LookUp(shadowProgram);
		Log("FIELD_PBR: field characters are drawn with DEMASTER's character program (demaster_chara.vert/.frag)%s%s%s%s\n",
			FIELD_PBR_CHARACTERS ? ", lit by the room" : ", as the game draws them", FIELD_PBR_SHADOWS ? ", shadows" : "",
			FIELD_PBR_REFLECTIONS ? ", reflections" : "", FIELD_AVATARS ? ", face pictures (Ctrl+F10 saves them)" : "");
		if (WholeModels())
		{
			if (drawInstanced != nullptr)
				Log("FIELD_PBR: instancing on: every shadow map of a character is one draw, both of its reflection pictures one\n");
			else
				Log("FIELD_PBR: no instancing (%s): one draw per shadow map and picture, as before\n", instancedWhy.c_str());
		}
		return true;
	}

	//--- render targets

	struct Target
	{
		GLuint framebuffer = 0, color = 0, depth = 0;
		int width = 0, height = 0;
		bool failed = false;
	};
	Target captureTarget, shadowTarget, avatarTarget;

	//FIELD_PBR_DEPTH: the depth buffer right after the characters are drawn (nothing of the background in it yet), for the
	//background's pixels to step aside where a character stands in front of what they show
	struct DepthCopy
	{
		GLuint texture = 0;
		int width = 0, height = 0;
		bool failed = false;
	};
	DepthCopy depthCopy;
	bool depthThisFrame = false; //copied since the last SwapBuffers (characters the game draws later keep using it)

	//game: the game's state (its framebuffer and viewport). The texture is the framebuffer's size up to the viewport's far
	//corner, so the background reads it at gl_FragCoord
	bool CopyCharacterDepth(const RenderState& game, CharFrame& shared)
	{
		DepthCopy& d = depthCopy;
		const int x = game.viewport[0], y = game.viewport[1], w = game.viewport[2], h = game.viewport[3];
		if (d.failed || x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > 16384 || y + h > 16384)
			return false;
		glActiveTexture(GL_TEXTURE0);
		if (d.texture == 0 || d.width != x + w || d.height != y + h)
		{
			if (d.texture == 0)
				glGenTextures(1, &d.texture);
			d.width = x + w;
			d.height = y + h;
			glBindTexture(GL_TEXTURE_2D, d.texture);
			for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++) {}
			RawTexImage(GL_DEPTH_COMPONENT24, d.width, d.height, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
			if (glGetError() != GL_NO_ERROR)
			{
				d.failed = true;
				Log("FIELD_PBR_DEPTH: the characters' depth can't be copied on this GPU: the background hides them per tile, as the game does\n");
				return false;
			}
		}
		else
			glBindTexture(GL_TEXTURE_2D, d.texture);
		//from the game's framebuffer's depth (a depth texture takes the depth buffer's values)
		glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(game.drawFramebuffer));
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, x, y, x, y, w, h);
		shared.depth = d.texture;
		shared.depthWidth = d.width;
		shared.depthHeight = d.height;
		return true;
	}

	//color: RGBA8 texture (with mip levels when mipmapped); depthTexture: the depth as a texture (no colour)
	bool MakeTarget(Target& t, const int width, const int height, const bool color, const bool mipmapped, const bool depthTexture)
	{
		if (t.framebuffer != 0)
			return true;
		if (t.failed)
			return false;
		GLint bound = 0, framebuffer = 0, renderbuffer = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
		glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
		t.width = width;
		t.height = height;
		glGenFramebuffers(1, &t.framebuffer);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, t.framebuffer);
		if (color)
		{
			glGenTextures(1, &t.color);
			glBindTexture(GL_TEXTURE_2D, t.color);
			int levels = 1;
			if (mipmapped)
				for (int s = std::max(width, height); s > 1; s /= 2)
					levels++;
			for (int level = 0; level < levels; level++)
				RawTexImage(GL_RGBA8, std::max(1, width >> level), std::max(1, height >> level), GL_RGBA, GL_UNSIGNED_BYTE, nullptr, level);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mipmapped ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, levels - 1);
			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);
		}
		if (depthTexture)
		{
			glGenTextures(1, &t.depth);
			glBindTexture(GL_TEXTURE_2D, t.depth);
			RawTexImage(GL_DEPTH_COMPONENT24, width, height, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, t.depth, 0);
			glDrawBuffer(GL_NONE);
			glReadBuffer(GL_NONE);
		}
		else
		{
			glGenRenderbuffers(1, &t.depth);
			glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
			glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
			glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.depth);
		}
		const GLenum status = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
		glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(renderbuffer));
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			Log("FIELD_PBR: a %dx%d render target cannot be made (status 0x%X): that feature is off this session\n", width, height, status);
			glDeleteFramebuffers(1, &t.framebuffer);
			if (t.color != 0)
				glDeleteTextures(1, &t.color);
			if (depthTexture)
				glDeleteTextures(1, &t.depth);
			else
				glDeleteRenderbuffers(1, &t.depth);
			t = Target();
			t.failed = true;
			return false;
		}
		return true;
	}

	//--- reading the game's draw

	GLsizei ReadIndices(const GLsizei count, const GLenum type, const void* indices, std::vector<uint32_t>& out)
	{
		out.resize(static_cast<size_t>(count));
		const GLint elementBuffer = glstate::ElementBuffer();
		const size_t size = type == GL_UNSIGNED_INT ? 4 : type == GL_UNSIGNED_SHORT ? 2 : 1;
		static std::vector<uint8_t> raw; //kept between draws (no allocation per draw)
		raw.resize(static_cast<size_t>(count) * size);
		if (elementBuffer != 0)
			glGetBufferSubData(GL_ELEMENT_ARRAY_BUFFER, reinterpret_cast<GLintptr>(indices), static_cast<GLsizeiptr>(raw.size()), raw.data());
		else if (indices == nullptr || !SafeRead(raw.data(), indices, raw.size()))
			return 0;
		for (GLsizei i = 0; i < count; i++)
			out[i] = size == 4 ? *reinterpret_cast<const uint32_t*>(raw.data() + i * 4)
			: size == 2 ? *reinterpret_cast<const uint16_t*>(raw.data() + i * 2) : raw[i];
		return count;
	}

	//TLVERTEX in client memory: x, y, z, rhw (floats), colour (4 bytes), specular, u, v
	bool ReadGameVertices(const std::vector<uint32_t>& list, std::vector<GameVertex>& out)
	{
		if (LastVerticesPointer.pointer == nullptr || LastVerticesPointer.size != 4 || LastTexCoordPointer.pointer == nullptr)
			return false;
		if (glstate::VertexArrayBuffer() != 0)
			return false;
		void* colorPointer = nullptr;
		GLint colorStride = 0, colorType = 0;
		glstate::ColorArray(colorPointer, colorStride, colorType);
		if (colorType != GL_UNSIGNED_BYTE)
			colorPointer = nullptr;
		const int stride = LastVerticesPointer.stride != 0 ? LastVerticesPointer.stride : 16;
		const int uvStride = LastTexCoordPointer.stride != 0 ? LastTexCoordPointer.stride : 8;
		const int cStride = colorStride != 0 ? colorStride : 4;
		uint32_t lo = 0xFFFFFFFF, hi = 0;
		for (const uint32_t i : list)
		{
			lo = std::min(lo, i);
			hi = std::max(hi, i);
		}
		if (list.empty() || hi - lo > 65535)
			return false;
		const size_t n = static_cast<size_t>(hi - lo) + 1;
		static std::vector<uint8_t> pos, uv, col; //kept between draws
		pos.resize(n * stride);
		uv.resize(n * uvStride);
		col.clear();
		if (!SafeRead(pos.data(), static_cast<const uint8_t*>(LastVerticesPointer.pointer) + static_cast<size_t>(lo) * stride, (n - 1) * stride + 16)
			|| !SafeRead(uv.data(), static_cast<const uint8_t*>(LastTexCoordPointer.pointer) + static_cast<size_t>(lo) * uvStride, (n - 1) * uvStride + 8))
			return false;
		if (colorPointer != nullptr)
		{
			col.resize(n * cStride);
			if (!SafeRead(col.data(), static_cast<const uint8_t*>(colorPointer) + static_cast<size_t>(lo) * cStride, (n - 1) * cStride + 4))
				col.clear();
		}
		out.resize(list.size());
		for (size_t k = 0; k < list.size(); k++)
		{
			const size_t i = list[k] - lo;
			GameVertex& v = out[k];
			float p[4];
			memcpy(p, pos.data() + i * stride, 16);
			v.x = p[0];
			v.y = p[1];
			v.z = p[2];
			v.rhw = p[3];
			float t[2];
			memcpy(t, uv.data() + i * uvStride, 8);
			v.u = t[0];
			v.v = t[1];
			if (!col.empty())
				memcpy(v.color, col.data() + i * cStride, 4);
		}
		return true;
	}

	uint64_t VertexKey(const GameVertex& v)
	{
		uint32_t a, b, c;
		memcpy(&a, &v.x, 4);
		memcpy(&b, &v.y, 4);
		memcpy(&c, &v.rhw, 4);
		uint64_t h = 0x9E3779B97F4A7C15ull ^ a;
		h = (h ^ (static_cast<uint64_t>(b) << 21)) * 0xBF58476D1CE4E5B9ull;
		h = (h ^ (h >> 29) ^ c) * 0x94D049BB133111EBull;
		return h ^ (h >> 32);
	}

	//--- the field as the characters need it

	Projection CurrentProjection()
	{
		const FieldView& v = PbrView();
		Projection p;
		if (v.settings != nullptr)
		{
			p.space = v.settings->space;
			p.gameDepth = v.settings->gameDepth > 0 ? v.settings->gameDepth : fieldpbr::GAME_DEPTH_PER_VIEW;
		}
		p.offsetX = v.offsetKnown ? v.offsetX : 0.0;
		p.offsetY = v.offsetKnown ? v.offsetY : 0.0;
		p.screenPerCanvas = 2.0;
		return p;
	}

	V3 ViewOfWorld(const fieldpbr::Settings& s, const double x, const double y, const double z)
	{
		const fieldpbr::Vec3 d = fieldpbr::Vec3{ x / 4096.0, y / 4096.0, z / 4096.0 } - s.cameraEye;
		return ToV3(fieldpbr::Vec3{ fieldpbr::Dot(d, s.cameraRight), fieldpbr::Dot(d, s.cameraDown), fieldpbr::Dot(d, s.cameraForward) });
	}

	V3 ViewDirectionOfWorld(const fieldpbr::Settings& s, const double x, const double y, const double z)
	{
		const fieldpbr::Vec3 d{ x, y, z };
		return Normalize(ToV3(fieldpbr::Vec3{ fieldpbr::Dot(d, s.cameraRight), fieldpbr::Dot(d, s.cameraDown), fieldpbr::Dot(d, s.cameraForward) }));
	}

	//--- script entities (FIELD_ENTITIES: guest pointer to the 612-byte entities, FIELD_ENTITY_COUNT: how many)

	struct Entity
	{
		int index = -1;
		double x = 0, y = 0, z = 0; //world units
		int facing = 0, model = -1;
		//the "playable character" byte the script's SETPC sets (0x255 in the entity, as FFNx's ff8_field_state_other has it):
		//the party's characters have another value there than the field's other entities. For the party light
		int pc = -1;
	};

	std::vector<Entity> ReadEntities()
	{
		std::vector<Entity> out;
		static const uint32_t table = HasAddress(FIELD_ENTITIES) ? GetAddress(FIELD_ENTITIES) : 0;
		static const uint32_t countAt = HasAddress(FIELD_ENTITY_COUNT) ? GetAddress(FIELD_ENTITY_COUNT) : 0;
		if (table == 0 || countAt == 0)
			return out;
		uint8_t count = 0;
		uint32_t base = 0;
		if (!GuestRead(&count, countAt, 1) || !GuestRead(&base, table, 4) || base == 0 || count == 0)
			return out;
		for (int i = 0; i < std::min<int>(count, 24); i++)
		{
			const uint32_t at = base + static_cast<uint32_t>(i) * 612;
			int32_t position[3];
			uint16_t model = 0;
			uint8_t facing = 0;
			if (!GuestRead(position, at + 0x190, 12) || !GuestRead(&model, at + 0x218, 2) || !GuestRead(&facing, at + 0x241, 1))
				break;
			uint8_t pc = 0;
			Entity e;
			if (GuestRead(&pc, at + 0x255, 1))
				e.pc = pc;
			e.index = i;
			e.x = position[0] / 4096.0;
			e.y = position[1] / 4096.0;
			e.z = position[2] / 4096.0;
			e.facing = facing;
			e.model = model;
			out.push_back(e);
		}
		return out;
	}

	FacingCalibration facingCalibration; //how the facing byte points on the ground (the same in every field)
	struct EntityTrack
	{
		bool known = false;
		double x = 0, y = 0;
	};
	std::unordered_map<int, EntityTrack> entityTracks;
	uint64_t tracksEntry = 0;

	void LearnFacing(const std::vector<Entity>& entities, const uint64_t entry)
	{
		if (tracksEntry != entry)
		{
			entityTracks.clear();
			tracksEntry = entry;
		}
		for (const Entity& e : entities)
		{
			EntityTrack& t = entityTracks[e.index];
			if (!t.known)
			{
				t = { true, e.x, e.y };
				continue;
			}
			const double dx = e.x - t.x, dy = e.y - t.y, moved = std::hypot(dx, dy);
			if (moved > 200.0)
				t = { true, e.x, e.y }; //placed somewhere else
			else if (moved > 24.0)
			{
				facingCalibration.Add(e.facing, dx, dy);
				t = { true, e.x, e.y };
			}
		}
	}

	//--- the characters' darkening: the game's value when the field has settled, and what scripts change after that

	struct Baseline
	{
		uint64_t entry = 0;
		ULONGLONG first = 0, stableSince = 0;
		float value[3] = {}, base[3] = {};
		bool locked = false;
	};
	std::unordered_map<GLuint, Baseline> baselines;

	const float* BaselineOf(const GLuint texture, const float* ambient, const uint64_t entry, const ULONGLONG now)
	{
		Baseline& b = baselines[texture];
		if (b.entry != entry)
		{
			b = Baseline();
			b.entry = entry;
			b.first = b.stableSince = now;
			memcpy(b.value, ambient, sizeof(b.value));
		}
		if (!b.locked)
		{
			if (memcmp(b.value, ambient, sizeof(b.value)) != 0)
			{
				memcpy(b.value, ambient, sizeof(b.value));
				b.stableSince = now;
			}
			//settled: the same for half a second, or four seconds since it was first seen
			if (now - b.stableSince >= 500 || now - b.first >= 4000)
			{
				memcpy(b.base, b.value, sizeof(b.base));
				b.locked = true;
			}
		}
		return b.locked ? b.base : ambient;
	}

	//--- one frame's characters

	struct Frame
	{
		std::vector<V3> positions, normals;
		std::vector<unsigned> textures; //per triangle
		std::vector<Character> characters; //the first MAX_CHARACTERS go to the background program
		std::vector<std::vector<uint32_t>> indices; //per character, its vertices
		CharFrame shared;
		std::vector<Entity> entities; //the script entities (when the camera is known)
	};

	//avatars
	struct Avatar
	{
		bool valid = false;
		uint64_t entry = 0;
		int entity = -1;
		GLuint texture = 0;
		ULONGLONG updated = 0;
	};
	std::array<Avatar, AVATAR_COLUMNS * AVATAR_ROWS> avatars;
	bool avatarKeyDown = false, saveAvatars = false;

	void SetCommonUniforms(const CharProgram& p, const FieldView& v, const int viewMode)
	{
		const fieldpbr::Settings& s = *v.settings;
		const fieldpbr::Space& space = s.space;
		glUniform4f(p.space, static_cast<float>(space.zoom), static_cast<float>(space.originX), static_cast<float>(space.originY),
			space.model == fieldpbr::DepthModel::Inverse ? 1.0f : 0.0f);
		glUniform4f(p.depth, static_cast<float>(space.a), static_cast<float>(space.b), static_cast<float>(space.nearest), 0.0f);
		if (p.effectAt >= 0) //EFFECT_LIGHTS: what glows in the field
		{
			const EffectGlows& effects = FieldEffectGlows();
			glUniform4fv(p.effectAt, MAX_EFFECT_GLOWS, effects.at);
			glUniform4fv(p.effectColour, MAX_EFFECT_GLOWS, effects.colour);
		}
		glUniform4f(p.roomSize, static_cast<float>(std::max(1, v.canvasWidth)), static_cast<float>(std::max(1, v.canvasHeight)),
			v.room != 0 ? 1.0f : 0.0f, 0.0f);
		glUniform4f(p.features, 0.0f, 0.0f, 0.0f, FIELD_PBR_SHADOWS && v.room != 0 ? 1.0f : 0.0f);
		glUniform4f(p.probeBox, static_cast<float>(std::max(1, v.canvasWidth)), static_cast<float>(std::max(1, v.canvasHeight)),
			static_cast<float>(s.probeNear), static_cast<float>(s.probeFar > s.probeNear ? s.probeFar : s.probeNear + 1.0));
		glUniform4f(p.probeInfo, static_cast<float>(std::max(1, v.probeSlices)), v.probeExposure, 0.0f, 0.0f);
		glUniform4f(p.quality, static_cast<float>(CurrentQuality().lightWallSteps), 0.0f, 0.0f, 0.0f);
		const LightUniforms& lights = PbrLights();
		glUniform1i(p.lightCount, lights.count);
		if (lights.count > 0)
		{
			glUniform4fv(p.lightPos, lights.count, lights.pos);
			glUniform4fv(p.lightColor, lights.count, lights.color);
			glUniform4fv(p.lightFlicker, lights.count, lights.flicker);
		}
		//DAY_NIGHT: the room's light on the characters dims as the painting does
		glUniform4fv(p.night, 1, lights.night);
		glUniform4fv(p.nightTint, 1, lights.nightTint);
		(void)viewMode;
	}

	//per draw: its texture's maps, the game's darkening
	void SetDrawUniforms(const CharProgram& p, const CharDraw& d, const FieldView& v, const uint64_t entry, const ULONGLONG now)
	{
		GLuint normal = 0, orme = 0;
		FieldModelMaps(d.texture, normal, orme);
		const bool lit = FIELD_PBR_CHARACTERS != 0;
		const bool grid = v.probes != 0;
		glUniform4f(p.model, normal != 0 ? 1.0f : 0.0f, orme != 0 ? 1.0f : 0.0f, -1.0f, lit ? 1.0f : 0.0f);
		glUniform4f(p.game, d.ambient[0], d.ambient[1], d.ambient[2], static_cast<float>(FIELD_PBR_VIEW));
		float shade[3] = { d.ambient[0], d.ambient[1], d.ambient[2] };
		if (lit && grid && !v.settings->characterShadeKeep)
		{
			//the room's light replaces the field's starting darkening; what scripts change after it stays
			const float* base = BaselineOf(d.texture, d.ambient, entry, now);
			for (int c = 0; c < 3; c++)
				shade[c] = d.ambient[c] - base[c];
		}
		glUniform4f(p.shade, shade[0], shade[1], shade[2], grid ? 1.0f : 0.0f);
		glUniform1i(p.blendMode, d.blendMode);
		glActiveTexture(GL_TEXTURE0 + UNIT_MODEL_N);
		glBindTexture(GL_TEXTURE_2D, normal);
		glActiveTexture(GL_TEXTURE0 + UNIT_MODEL_ORME);
		glBindTexture(GL_TEXTURE_2D, orme);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, d.texture);
	}

	//--- the frame's models on the GPU: sent once a frame (after their normals are known), every pass draws from there.
	//The game animates its models itself and sends them already moved every frame, so once a frame is as few as it gets.
	struct FrameBuffers
	{
		GLuint vertices = 0, indices = 0;
		bool on = false; //this frame's models are in them
		bool failed = false; //no buffers: drawn from the game's memory, as before
	};
	FrameBuffers gpu;
	//0 .. n - 1 (the game's draws, as ranges), then each character's triangles
	std::vector<uint32_t> indexData;
	std::vector<uint32_t> characterFirst, characterCount; //into indexData

	//f: the frame's characters (null: only the game's draws, ReplayAsGame)
	void UploadFrame(const Frame* f)
	{
		const size_t n = vertices.size();
		indexData.resize(n);
		std::iota(indexData.begin(), indexData.end(), 0u);
		characterFirst.clear();
		characterCount.clear();
		if (f != nullptr)
			for (const std::vector<uint32_t>& list : f->indices)
			{
				characterFirst.push_back(static_cast<uint32_t>(indexData.size()));
				characterCount.push_back(static_cast<uint32_t>(list.size()));
				indexData.insert(indexData.end(), list.begin(), list.end());
			}
		gpu.on = false;
		if (gpu.failed || n == 0)
			return;
		if (gpu.vertices == 0)
		{
			glGenBuffers(1, &gpu.vertices);
			glGenBuffers(1, &gpu.indices);
			if (gpu.vertices == 0 || gpu.indices == 0)
			{
				gpu.failed = true;
				LogOnce("chars-no-buffers", "FIELD_PBR: no GPU buffers for the field characters: they are drawn from the game's memory\n");
				return;
			}
		}
		//a new store every frame (the driver doesn't wait for the last frame's draws to finish with the old one)
		glBindBuffer(GL_ARRAY_BUFFER, gpu.vertices);
		glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(n * sizeof(Vertex)), vertices.data(), GL_STREAM_DRAW);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.indices);
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(indexData.size() * sizeof(uint32_t)), indexData.data(), GL_STREAM_DRAW);
		gpu.on = true;
	}

	//the vertex arrays, on the GPU buffer when it has this frame (UploadFrame first)
	void PointArrays()
	{
		const uintptr_t base = gpu.on ? 0 : reinterpret_cast<uintptr_t>(vertices.data());
		const auto at = [base](const size_t offset) { return reinterpret_cast<const void*>(base + offset); };
		glBindBuffer(GL_ARRAY_BUFFER, gpu.on ? gpu.vertices : 0);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gpu.on ? gpu.indices : 0);
		glVertexPointer(4, GL_FLOAT, sizeof(Vertex), at(offsetof(Vertex, pos)));
		glEnableClientState(GL_VERTEX_ARRAY);
		glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), at(offsetof(Vertex, color)));
		glEnableClientState(GL_COLOR_ARRAY);
		glNormalPointer(GL_FLOAT, sizeof(Vertex), at(offsetof(Vertex, normal)));
		glEnableClientState(GL_NORMAL_ARRAY);
		glClientActiveTexture(GL_TEXTURE1);
		glTexCoordPointer(3, GL_FLOAT, sizeof(Vertex), at(offsetof(Vertex, view)));
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glClientActiveTexture(GL_TEXTURE0);
		glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), at(offsetof(Vertex, uv)));
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	}

	//FIELD_INTERPOLATION: the models' places in the in-between picture instead of this frame's (null: this frame's again).
	//x, y, z, rhw per vertex, in the order of `vertices`; the other arrays stay as PointArrays left them.
	void PointPositions(const float* between)
	{
		if (between != nullptr)
		{
			glBindBuffer(GL_ARRAY_BUFFER, 0);
			glVertexPointer(4, GL_FLOAT, 4 * sizeof(float), between);
		}
		else
		{
			glBindBuffer(GL_ARRAY_BUFFER, gpu.on ? gpu.vertices : 0);
			glVertexPointer(4, GL_FLOAT, sizeof(Vertex), gpu.on ? nullptr : static_cast<const void*>(vertices.data()));
		}
	}

	//FIELD_SMOOTH_SCROLL, FIELD_INTERPOLATION (texturepatch_v2_fieldTween.cpp): what the kept models need to move with the
	//main layer and to be drawn a second time for the in-between picture
	struct Moved
	{
		double shift[2] = { 0, 0 }; //how far the main layer is drawn from the game's place this frame, in whole pixels
		const float* between = nullptr; //the models' places in the in-between picture (null: this frame has none)
		GLuint copy = 0; //the copy of the framebuffer the models go to
		GLint framebuffer = 0; //that framebuffer
	};
	Moved MoveOfFrame()
	{
		Moved m;
		if (draws.empty() || !TweenOn())
			return m;
		const bool tween = TweenCharacterShift(m.shift);
		const CharDraw& first = draws[0];
		TweenSnap(m.shift, first.projection, first.state.viewport[2], first.state.viewport[3]);
		static std::vector<float> shown; //kept between frames
		static std::vector<fieldtween::CharDrawRef> refs;
		shown.resize(vertices.size() * 4);
		for (size_t i = 0; i < vertices.size(); i++)
		{
			shown[i * 4] = static_cast<float>(vertices[i].pos[0] + m.shift[0]);
			shown[i * 4 + 1] = static_cast<float>(vertices[i].pos[1] + m.shift[1]);
			shown[i * 4 + 2] = vertices[i].pos[2];
			shown[i * 4 + 3] = vertices[i].pos[3];
		}
		refs.clear();
		for (const CharDraw& d : draws)
		{
			fieldtween::CharDrawRef r;
			r.texture = d.texture;
			r.first = d.first;
			r.count = d.count;
			refs.push_back(r);
		}
		m.between = TweenCharacterPositions(shown, refs, tween);
		if (m.between != nullptr)
		{
			//before anything of this frame is drawn into the game's framebuffer (a new copy starts as what that holds)
			m.framebuffer = first.state.drawFramebuffer;
			m.copy = TweenFramebufferCopy(static_cast<GLuint>(m.framebuffer));
			if (m.copy == 0)
				m.between = nullptr;
		}
		return m;
	}

	//count indices of indexData from first; instances > 1: that many copies in one draw (instancing)
	void DrawIndices(const uint32_t first, const uint32_t count, const int instances = 1)
	{
		if (count == 0 || static_cast<size_t>(first) + count > indexData.size())
			return;
		const void* at = gpu.on ? reinterpret_cast<const void*>(static_cast<uintptr_t>(first) * sizeof(uint32_t)) : indexData.data() + first;
		if (instances > 1 && drawInstanced != nullptr)
			drawInstanced(GL_TRIANGLES, static_cast<GLsizei>(count), GL_UNSIGNED_INT, at, instances);
		else
			reinterpret_cast<void(__stdcall*)(GLenum, GLsizei, GLenum, const void*)>(ogl_draw_elements)
			(GL_TRIANGLES, static_cast<GLsizei>(count), GL_UNSIGNED_INT, at);
	}

	//vertices first .. first + count - 1, as the game sent them
	void DrawRange(const uint32_t first, const uint32_t count)
	{
		DrawIndices(first, count);
	}

	void DrawCharacterTriangles(const size_t c, const int instances = 1)
	{
		if (c < characterFirst.size())
			DrawIndices(characterFirst[c], characterCount[c], instances);
	}

	//draws one character's triangles, per texture (with each texture's maps and darkening)
	void DrawCharacter(const CharProgram& p, const Frame& f, const size_t c, const FieldView& v, const uint64_t entry, const ULONGLONG now,
		const int instances = 1)
	{
		const Character& ch = f.characters[c];
		//its draws: the ones whose texture is its texture (all the same for one character)
		for (const CharDraw& d : draws)
			if (d.texture == ch.texture)
			{
				SetDrawUniforms(p, d, v, entry, now);
				break;
			}
		DrawCharacterTriangles(c, instances);
	}

	//instancing: where tile (tx, ty) of a target, size w x h (fractions of the target), is in the target's clip space: its
	//size and its centre (demasterInstanceTile in demaster_chara.vert)
	void TileInClip(const int tx, const int ty, const float w, const float h, float out[4])
	{
		out[0] = w;
		out[1] = h;
		out[2] = w * (2 * tx + 1) - 1.0f;
		out[3] = h * (2 * ty + 1) - 1.0f;
	}

	//the pictures reflections use: each character from the front (as the camera sees it) and from behind (mirrored, so both
	//have the card's right and up)
	bool DrawCaptures(Frame& f, const FieldView& v, const uint64_t entry, const ULONGLONG now)
	{
		if (!MakeTarget(captureTarget, CAPTURE_SIZE, CAPTURE_SIZE, true, true, false))
			return false;
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, captureTarget.framebuffer);
		RawViewport(0, 0, CAPTURE_SIZE, CAPTURE_SIZE);
		glDisable(GL_SCISSOR_TEST);
		glClearColor(0, 0, 0, 0);
		glClearDepth(1.0);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glDepthMask(GL_TRUE);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glDisable(GL_CULL_FACE);
		glDisable(GL_STENCIL_TEST);
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GEQUAL, 0.439f);
		glEnable(GL_BLEND);
		glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA); //premultiplied
		const CharProgram& p = charProgram;
		glUseProgram(p.id);
		LutUse(p.id, false); //the pictures reflections use are graded where they show (the background's own draw)
		SetCommonUniforms(p, v, FIELD_PBR_VIEW);
		const int count = std::min<int>(static_cast<int>(f.characters.size()), MAX_CHARACTERS);
		const float tileSize = static_cast<float>(CAPTURE_TILE) / CAPTURE_SIZE;
		for (int c = 0; c < count; c++)
		{
			const Character& ch = f.characters[c];
			//front (as the camera sees it) and back (mirrored): the camera, the tile, which way it looks
			float matrices[2 * 16], tiles[2 * 4], looks[2 * 4];
			for (int side = 0; side < 2; side++)
			{
				const int tile = 2 * c + side;
				const V3 axes[3] = { ch.right, ch.up, side == 0 ? ch.forward : ch.forward * -1.0f };
				const float scale[3] = { ch.halfSize.x, ch.halfSize.y, ch.halfSize.z * 1.1f + 1e-4f };
				const Mat4 m = Box(axes, ch.center, scale);
				memcpy(matrices + side * 16, m.m, sizeof(m.m));
				TileInClip(tile % 4, tile / 4, tileSize, tileSize, tiles + side * 4);
				const V3 look = side == 0 ? ch.forward : ch.forward * -1.0f;
				looks[side * 4 + 0] = look.x;
				looks[side * 4 + 1] = look.y;
				looks[side * 4 + 2] = look.z;
				looks[side * 4 + 3] = side == 1 ? 1.0f : 0.0f;
			}
			if (drawInstanced != nullptr)
			{
				//both pictures in one draw
				RawViewport(0, 0, CAPTURE_SIZE, CAPTURE_SIZE);
				glUniformMatrix4fv(p.instanceMatrix, 2, GL_FALSE, matrices);
				glUniform4fv(p.instanceTile, 2, tiles);
				glUniform4fv(p.instanceLook, 2, looks);
				glUniform4f(p.charMode, 1.0f, 0.0f, 1.0f, 0.0f);
				DrawCharacter(p, f, static_cast<size_t>(c), v, entry, now, 2);
				continue;
			}
			for (int side = 0; side < 2; side++)
			{
				const int tile = 2 * c + side;
				RawViewport((tile % 4) * CAPTURE_TILE, (tile / 4) * CAPTURE_TILE, CAPTURE_TILE, CAPTURE_TILE);
				glUniformMatrix4fv(p.viewMatrix, 1, GL_FALSE, matrices + side * 16);
				glUniform4f(p.charMode, 1.0f, side == 1 ? 1.0f : 0.0f, 0.0f, 0.0f);
				glUniform4f(p.viewer, looks[side * 4], looks[side * 4 + 1], looks[side * 4 + 2], 0.0f);
				DrawCharacter(p, f, static_cast<size_t>(c), v, entry, now);
			}
		}
		glUniform4f(p.charMode, 1.0f, 0.0f, 0.0f, 0.0f);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, captureTarget.color);
		glGenerateMipmap(GL_TEXTURE_2D);
		f.shared.capture = captureTarget.color;
		return true;
	}

	//the characters as the field's lights see them: one shadow map per light and character that matter most
	bool DrawShadows(Frame& f, const FieldView& v)
	{
		const LightUniforms& lights = PbrLights();
		const int count = std::min<int>(static_cast<int>(f.characters.size()), MAX_CHARACTERS);
		struct Pair
		{
			int light = 0, character = 0;
			float weight = 0;
		};
		std::vector<Pair> pairs;
		for (int i = 0; i < lights.count; i++)
		{
			if (!lights.castsShadow[i])
				continue;
			const V3 lp{ lights.pos[i * 4], lights.pos[i * 4 + 1], lights.pos[i * 4 + 2] };
			const float range = lights.pos[i * 4 + 3];
			const float added = lights.color[i * 4] + lights.color[i * 4 + 1] + lights.color[i * 4 + 2];
			for (int c = 0; c < count; c++)
			{
				const Character& ch = f.characters[c];
				const float radius = Length(ch.halfSize);
				const float d = Length(ch.center - lp);
				if (d >= range + radius || d <= radius * 1.05f)
					continue;
				const float x = d / std::max(range, 1e-6f);
				const float window = std::max(0.0f, 1.0f - x * x);
				pairs.push_back({ i, c, lights.flicker[i * 4 + 2] * (added + 0.5f) * window });
			}
		}
		std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.weight > b.weight; });
		const Quality& q = CurrentQuality();
		const int perRow = std::max(1, q.shadowSize / q.shadowTile);
		const size_t most = static_cast<size_t>(std::min({ MAX_SHADOW_MAPS, q.maxShadowMaps, perRow * perRow }));
		if (pairs.size() > most)
			pairs.resize(most);
		if (pairs.empty())
			return false;
		if (!MakeTarget(shadowTarget, q.shadowSize, q.shadowSize, false, false, true))
			return false;
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, shadowTarget.framebuffer);
		RawViewport(0, 0, q.shadowSize, q.shadowSize);
		glDisable(GL_SCISSOR_TEST);
		glClearDepth(1.0);
		glDepthMask(GL_TRUE);
		glClear(GL_DEPTH_BUFFER_BIT);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glDisable(GL_CULL_FACE);
		glDisable(GL_STENCIL_TEST);
		glDisable(GL_BLEND);
		glDisable(GL_ALPHA_TEST);
		glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
		const CharProgram& p = shadowProgram;
		glUseProgram(p.id);
		glUniform4f(p.charMode, 1.0f, 0.0f, 0.0f, 0.0f);
		glUniform1i(p.blendMode, 0);
		const float tileSize = static_cast<float>(q.shadowTile) / q.shadowSize;
		//every shadow map: the light's camera on the character, its tile of the atlas
		int k = 0;
		float tiles[MAX_SHADOW_MAPS * 4]{};
		for (const Pair& pair : pairs)
		{
			const Character& ch = f.characters[pair.character];
			const V3 lp{ lights.pos[pair.light * 4], lights.pos[pair.light * 4 + 1], lights.pos[pair.light * 4 + 2] };
			const float radius = Length(ch.halfSize) * 1.1f;
			const float d = Length(ch.center - lp);
			const float halfAngle = std::asin(std::min(0.98f, radius / d));
			const float nearZ = std::max(d - radius * 1.6f, d * 0.08f);
			const float farZ = std::max(d * 8.0f, lights.pos[pair.light * 4 + 3] * 1.5f);
			const Mat4 m = Multiply(Perspective(halfAngle, nearZ, farZ), LookAt(lp, ch.center, ch.up));
			const int tx = k % perRow, ty = k / perRow;
			memcpy(f.shared.shadowMatrix + k * 16, m.m, sizeof(m.m));
			f.shared.shadowInfo[k * 4 + 0] = static_cast<float>(pair.light);
			f.shared.shadowInfo[k * 4 + 1] = tx * tileSize;
			f.shared.shadowInfo[k * 4 + 2] = ty * tileSize;
			f.shared.shadowInfo[k * 4 + 3] = tileSize;
			TileInClip(tx, ty, tileSize, tileSize, tiles + k * 4);
			k++;
		}
		//every character casts into every map, each with its own texture for the cut-out parts
		if (drawInstanced != nullptr && k <= MAX_INSTANCES)
		{
			//one draw per character makes all k maps: copy i through map i's camera into map i's tile
			RawViewport(0, 0, q.shadowSize, q.shadowSize);
			glUniformMatrix4fv(p.instanceMatrix, k, GL_FALSE, f.shared.shadowMatrix);
			glUniform4fv(p.instanceTile, k, tiles);
			glUniform4f(p.charMode, 1.0f, 0.0f, 1.0f, 0.0f);
			for (int c = 0; c < static_cast<int>(f.characters.size()); c++)
			{
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, f.characters[c].texture);
				DrawCharacterTriangles(static_cast<size_t>(c), k);
			}
			glUniform4f(p.charMode, 1.0f, 0.0f, 0.0f, 0.0f);
		}
		else
			for (int i = 0; i < k; i++)
			{
				const int tx = i % perRow, ty = i / perRow;
				RawViewport(tx * q.shadowTile, ty * q.shadowTile, q.shadowTile, q.shadowTile);
				glUniformMatrix4fv(p.viewMatrix, 1, GL_FALSE, f.shared.shadowMatrix + i * 16);
				for (int c = 0; c < static_cast<int>(f.characters.size()); c++)
				{
					glActiveTexture(GL_TEXTURE0);
					glBindTexture(GL_TEXTURE_2D, f.characters[c].texture);
					DrawCharacterTriangles(static_cast<size_t>(c));
				}
			}
		f.shared.shadowCount = k;
		f.shared.shadowMap = shadowTarget.depth;
		return k > 0;
	}

	//FIELD_AVATARS: a camera in front of each character's head (updated while the face is turned towards the game's camera)
	void DrawAvatars(Frame& f, const FieldView& v, const uint64_t entry, const ULONGLONG now)
	{
		if (!MakeTarget(avatarTarget, AVATAR_TILE * AVATAR_COLUMNS, AVATAR_TILE * AVATAR_ROWS, true, false, false))
			return;
		const fieldpbr::Settings& s = *v.settings;
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, avatarTarget.framebuffer);
		glDisable(GL_SCISSOR_TEST);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glDepthMask(GL_TRUE);
		glDisable(GL_CULL_FACE);
		glDisable(GL_STENCIL_TEST);
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GEQUAL, 0.439f);
		glEnable(GL_BLEND);
		glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glEnable(GL_SCISSOR_TEST);
		const CharProgram& p = charProgram;
		glUseProgram(p.id);
		LutUse(p.id, false); //the face pictures: not graded
		SetCommonUniforms(p, v, FIELD_PBR_VIEW);
		const int count = std::min<int>(static_cast<int>(f.characters.size()), static_cast<int>(avatars.size()));
		for (int c = 0; c < count; c++)
		{
			const Character& ch = f.characters[c];
			//which way the face looks: the entity's facing once the game's angles are learnt, else towards the camera
			V3 facing = ch.forward * -1.0f;
			bool known = false;
			if (ch.entity >= 0 && facingCalibration.known && s.cameraKnown)
			{
				for (const Entity& en : f.entities)
					if (en.index == ch.entity)
					{
						const double a = facingCalibration.Angle(en.facing);
						facing = ViewDirectionOfWorld(s, std::cos(a), std::sin(a), 0.0);
						known = true;
					}
			}
			const V3 toCamera = Normalize(ch.head * -1.0f);
			if (known && Dot(facing, toCamera) < 0.3f)
				continue; //turned away: the last picture stays
			//its picture: the one it had (same entity, else same model), else a free one, else the oldest
			int slot = -1;
			for (int i = 0; i < static_cast<int>(avatars.size()) && slot < 0; i++)
				if (avatars[i].valid && avatars[i].entry == entry && (ch.entity >= 0 ? avatars[i].entity == ch.entity
					: avatars[i].entity < 0 && avatars[i].texture == ch.texture))
					slot = i;
			for (int i = 0; i < static_cast<int>(avatars.size()) && slot < 0; i++)
				if (!avatars[i].valid || avatars[i].entry != entry)
					slot = i;
			if (slot < 0)
			{
				slot = 0;
				for (int i = 1; i < static_cast<int>(avatars.size()); i++)
					if (avatars[i].updated < avatars[slot].updated)
						slot = i;
			}
			const float size = std::max(ch.headSize, ch.halfSize.y * 0.14f);
			const V3 eye = ch.head + facing * (size * 6.0f) + ch.up * (size * 0.4f);
			const Mat4 m = Multiply(Perspective(0.25f, size * 0.5f, size * 30.0f), LookAt(eye, ch.head, ch.up));
			const int tx = slot % AVATAR_COLUMNS, ty = slot / AVATAR_COLUMNS;
			RawViewport(tx * AVATAR_TILE, ty * AVATAR_TILE, AVATAR_TILE, AVATAR_TILE);
			RawScissor(tx * AVATAR_TILE, ty * AVATAR_TILE, AVATAR_TILE, AVATAR_TILE);
			glClearColor(0, 0, 0, 0);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			glUniformMatrix4fv(p.viewMatrix, 1, GL_FALSE, m.m);
			glUniform4f(p.charMode, 1.0f, 0.0f, 0.0f, 0.0f);
			glUniform4f(p.viewer, eye.x, eye.y, eye.z, 1.0f);
			DrawCharacter(p, f, static_cast<size_t>(c), v, entry, now);
			Avatar& a = avatars[slot];
			a.valid = true;
			a.entry = entry;
			a.entity = ch.entity;
			a.texture = ch.texture;
			a.updated = now;
		}
		glDisable(GL_SCISSOR_TEST);
	}

	//Ctrl+F10: the face pictures into DEMASTER_EXP\avatars\ (after the frame, from the picture atlas)
	void SaveAvatarFiles(const std::string& field)
	{
		if (avatarTarget.color == 0)
			return;
		const int width = AVATAR_TILE * AVATAR_COLUMNS, height = AVATAR_TILE * AVATAR_ROWS;
		std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
		GLint bound = 0, pack = 4, packBuffer = 0;
		glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
		glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
		glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
		if (packBuffer != 0)
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		glPixelStorei(GL_PACK_ALIGNMENT, 4);
		glBindTexture(GL_TEXTURE_2D, avatarTarget.color);
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
		glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
		glPixelStorei(GL_PACK_ALIGNMENT, pack);
		if (packBuffer != 0)
			glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
		const std::string folder = std::string(DIRECT_IO_EXPORT_DIR) + "avatars\\";
		std::error_code error;
		std::filesystem::create_directories(folder, error);
		const time_t t = time(nullptr);
		char stamp[32];
		strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", localtime(&t));
		size_t saved = 0;
		for (size_t i = 0; i < avatars.size(); i++)
		{
			if (!avatars[i].valid)
				continue;
			const int tx = static_cast<int>(i) % AVATAR_COLUMNS, ty = static_cast<int>(i) / AVATAR_COLUMNS;
			std::vector<uint8_t> tile(static_cast<size_t>(AVATAR_TILE) * AVATAR_TILE * 4);
			for (int y = 0; y < AVATAR_TILE; y++)
			{
				//the picture's top row is the texture's last
				const uint8_t* row = rgba.data() + ((static_cast<size_t>(ty) * AVATAR_TILE + (AVATAR_TILE - 1 - y)) * width + tx * AVATAR_TILE) * 4;
				uint8_t* outRow = tile.data() + static_cast<size_t>(y) * AVATAR_TILE * 4;
				for (int x = 0; x < AVATAR_TILE; x++)
				{
					//un-premultiply
					const uint8_t a = row[x * 4 + 3];
					for (int c = 0; c < 3; c++)
						outRow[x * 4 + c] = a ? static_cast<uint8_t>(std::min(255, row[x * 4 + c] * 255 / a)) : 0;
					outRow[x * 4 + 3] = a;
				}
			}
			char name[160];
			snprintf(name, sizeof(name), "%s_%s_%zu%s.png", field.c_str(), stamp, i,
				avatars[i].entity >= 0 ? ("_entity" + std::to_string(avatars[i].entity)).c_str() : "");
			bx::FileWriter writer;
			if (bx::open(&writer, bx::FilePath((folder + name).c_str())))
			{
				bimg::imageWritePng(&writer, AVATAR_TILE, AVATAR_TILE, AVATAR_TILE * 4, tile.data(), bimg::TextureFormat::RGBA8, false);
				bx::close(&writer);
				saved++;
			}
		}
		Log("FIELD_AVATARS: %zu face picture(s) saved to %s\n", saved, folder.c_str());
	}

	//the kept draws exactly as the game would have made them (its program, its state): when the field went away between
	//the characters and the background, or drawing them failed
	void ReplayAsGame()
	{
		RenderState gameState;
		Capture(gameState);
		ClientState client;
		SaveClient(client);
		GLfloat projection[16]{};
		const GLint program = glstate::Program();
		const GLenum active = glstate::ActiveTexture();
		const GLint matrixMode = glstate::MatrixMode();
		glstate::ProjectionMatrix(projection);
		const GLuint unit0 = glstate::Texture(GL_TEXTURE_2D, 0);
		glActiveTexture(GL_TEXTURE0);
		UploadFrame(nullptr);
		PointArrays();
		glClientActiveTexture(GL_TEXTURE1);
		glDisableClientState(GL_TEXTURE_COORD_ARRAY);
		glClientActiveTexture(GL_TEXTURE0);
		glDisableClientState(GL_NORMAL_ARRAY);
		const Moved moved = MoveOfFrame();
		//intoCopy: the second time, for the in-between picture (their places there, the copy of the game's framebuffer)
		const auto drawAll = [&moved](const bool intoCopy)
		{
			for (const CharDraw& d : draws)
			{
				if (intoCopy && d.state.drawFramebuffer != moved.framebuffer)
					continue;
				const GameProgram& game = GameProgramOf(d.program);
				glUseProgram(static_cast<GLuint>(d.program));
				if (game.ambient >= 0)
					glUniform3fv(game.ambient, 1, d.ambient);
				if (game.blendMode >= 0)
					glUniform1i(game.blendMode, d.blendMode);
				RenderState state = d.state;
				if (intoCopy)
					state.drawFramebuffer = static_cast<GLint>(moved.copy);
				Apply(state);
				if (cullPatched)
				{
					glEnable(GL_CULL_FACE);
					glCullFace(GL_BACK);
					glFrontFace(GL_CCW);
				}
				float screen[16];
				fieldtween::ShiftProjection(d.projection, intoCopy ? 0.0 : moved.shift[0], intoCopy ? 0.0 : moved.shift[1], screen);
				glMatrixMode(GL_PROJECTION);
				glLoadMatrixf(screen);
				glBindTexture(GL_TEXTURE_2D, d.texture);
				DrawRange(d.first, d.count);
			}
		};
		drawAll(false);
		if (moved.between != nullptr)
		{
			PointPositions(moved.between);
			drawAll(true);
		}
		RestoreClient(client);
		glMatrixMode(GL_PROJECTION);
		glLoadMatrixf(projection);
		glMatrixMode(static_cast<GLenum>(matrixMode));
		glBindTexture(GL_TEXTURE_2D, unit0);
		glActiveTexture(active);
		glUseProgram(static_cast<GLuint>(program));
		Apply(gameState);
	}

	//--- the frame's characters, drawn right before the next thing the game draws
	void Flush()
	{
		if (!pending)
			return;
		pending = false;
		const FieldView& v = PbrView();
		if (draws.empty() || vertices.empty() || v.settings == nullptr || charProgram.id == 0)
		{
			if (!draws.empty() && !vertices.empty())
				ReplayAsGame();
			draws.clear();
			vertices.clear();
			keys.clear();
			return;
		}
		const ULONGLONG now = GetTickCount64();
		const uint64_t entry = v.entry;
		const fieldpbr::Settings& s = *v.settings;
		const long long started = FieldTimeNow(); //FIELD_FRAME_LOG's timing
		//1. back into view space, smooth normals over the whole frame
		const Projection projection = CurrentProjection();
		static Frame f; //kept between frames (its lists keep their room)
		f.shared = CharFrame();
		f.entities.clear();
		const size_t n = vertices.size();
		f.positions.resize(n);
		for (size_t i = 0; i < n; i++)
		{
			Vertex& vx = vertices[i];
			f.positions[i] = projection.ToView(vx.pos[0], vx.pos[1], vx.pos[3]);
			vx.view[0] = f.positions[i].x;
			vx.view[1] = f.positions[i].y;
			vx.view[2] = f.positions[i].z;
		}
		SmoothNormals(f.positions, keys, CREASE_DEGREES, f.normals);
		for (size_t i = 0; i < n; i++)
		{
			vertices[i].normal[0] = f.normals[i].x;
			vertices[i].normal[1] = f.normals[i].y;
			vertices[i].normal[2] = f.normals[i].z;
		}
		//2. which triangles are which character
		f.textures.resize(n / 3);
		for (const CharDraw& d : draws)
			for (uint32_t t = d.first / 3; t < (d.first + d.count) / 3 && t < f.textures.size(); t++)
				f.textures[t] = d.texture;
		std::vector<Anchor> anchors;
		if (lastDiscsEntry == entry)
			for (const Disc& disc : lastDiscs)
			{
				Anchor a;
				a.position = projection.ToView(disc.x, disc.y, disc.rhw);
				a.darkness = disc.darkness;
				a.radius = static_cast<float>(disc.radius / 2.0 * a.position.z / std::max(1e-6, s.space.zoom));
				anchors.push_back(a);
			}
		const V3 up = Normalize(ToV3(s.up), { 0, -1, 0 });
		f.characters = Group(f.positions, f.textures, anchors, up);
		//the script entities (only to name them: hidden ones stand around too, so they do not split models)
		if (s.cameraKnown)
		{
			f.entities = ReadEntities();
			LearnFacing(f.entities, entry);
			for (Character& ch : f.characters)
			{
				float best = std::max(ch.halfSize.x, ch.halfSize.z) * 1.5f;
				for (const Entity& e : f.entities)
				{
					const V3 at = ViewOfWorld(s, e.x, e.y, e.z);
					const V3 d = at - ch.feet;
					const float level = Length(d - ch.up * Dot(d, ch.up));
					if (at.z > 1e-4f && level < best)
					{
						best = level;
						ch.entity = e.index;
					}
				}
			}
		}
		//the moving lights module: who could be carrying the party light (texturepatch_v2_fieldMovingLights.cpp)
		if (FIELD_MOVING_LIGHTS)
		{
			//the playable-character byte most entities have: the ones that differ are the party's
			int pcOfMost = -1, pcCount = 0;
			for (const Entity& e : f.entities)
			{
				int same = 0;
				for (const Entity& other : f.entities)
					same += other.pc == e.pc;
				if (e.pc >= 0 && same > pcCount)
				{
					pcCount = same;
					pcOfMost = e.pc;
				}
			}
			std::vector<fieldpbr::LeaderCandidate> drawn;
			drawn.reserve(f.characters.size());
			for (const Character& ch : f.characters)
			{
				fieldpbr::LeaderCandidate c;
				c.entity = ch.entity;
				c.key = ch.entity >= 0 ? ch.entity : 1000 + static_cast<int>(ch.texture);
				for (const Entity& e : f.entities)
					if (e.index == ch.entity)
						c.pc = e.pc;
				c.center = { ch.center.x, ch.center.y, ch.center.z };
				c.up = { ch.up.x, ch.up.y, ch.up.z };
				c.halfHeight = ch.halfSize.y;
				drawn.push_back(c);
			}
			//the script entity the player moves (the game's own word: the field's movement code gives the pad to it)
			int controlled = -1;
			static const uint32_t playerAt = HasAddress(FIELD_PLAYER_ENTITY) ? GetAddress(FIELD_PLAYER_ENTITY) : 0;
			int16_t player = -1;
			if (playerAt != 0 && GuestRead(&player, playerAt, 2) && player >= 0 && player < 24)
				controlled = player;
			MovingLightsCharacters(entry, drawn, f.entities.size() >= 2 ? pcOfMost : -1, controlled);
		}
		//nearest first: the background program takes MAX_CHARACTERS
		std::sort(f.characters.begin(), f.characters.end(), [](const Character& a, const Character& b) { return a.center.z < b.center.z; });
		f.indices.resize(f.characters.size());
		for (size_t c = 0; c < f.characters.size(); c++)
		{
			std::vector<uint32_t>& list = f.indices[c];
			list.clear();
			list.reserve(f.characters[c].triangles.size() * 3);
			for (const uint32_t t : f.characters[c].triangles)
			{
				list.push_back(t * 3);
				list.push_back(t * 3 + 1);
				list.push_back(t * 3 + 2);
			}
		}
		//3. the cards the background program reflects and darkens with
		CharFrame& shared = f.shared;
		shared.count = std::min<int>(static_cast<int>(f.characters.size()), MAX_CHARACTERS);
		for (int c = 0; c < shared.count; c++)
		{
			const Character& ch = f.characters[c];
			float* centre = shared.cardCenter + c * 4;
			float* cardUp = shared.cardUp + c * 4;
			float* right = shared.cardRight + c * 4;
			float* dir = shared.cardDir + c * 4;
			float* contact = shared.contact + c * 4;
			centre[0] = ch.center.x; centre[1] = ch.center.y; centre[2] = ch.center.z; centre[3] = ch.halfSize.x;
			cardUp[0] = ch.up.x; cardUp[1] = ch.up.y; cardUp[2] = ch.up.z; cardUp[3] = ch.halfSize.y;
			right[0] = ch.right.x; right[1] = ch.right.y; right[2] = ch.right.z; right[3] = static_cast<float>(2 * c);
			dir[0] = ch.forward.x; dir[1] = ch.forward.y; dir[2] = ch.forward.z; dir[3] = ch.halfSize.z;
			contact[0] = ch.halfSize.x;
			contact[1] = ch.halfSize.y;
			contact[2] = std::max(ch.halfSize.z, ch.halfSize.x * 0.6f);
			contact[3] = FIELD_PBR_SHADOWS ? ch.darkness : 0.0f;
		}
		//the game's state: everything DEMASTER changes goes back
		RenderState gameState;
		Capture(gameState);
		ClientState client;
		SaveClient(client);
		GLuint units[5] = {};
		GLfloat clearColor[4]{};
		GLdouble clearDepth = 1.0;
		const GLint program = glstate::Program();
		const GLenum active = glstate::ActiveTexture();
		glstate::ClearValues(clearColor, clearDepth);
		for (int u = 0; u <= UNIT_ROOM; u++)
			units[u] = glstate::Texture(GL_TEXTURE_2D, u);
		const GLuint probes3d = glstate::Texture(GL_TEXTURE_3D, UNIT_PROBES);
		//the room's light grid and depth, for every pass
		glActiveTexture(GL_TEXTURE0 + UNIT_PROBES);
		glBindTexture(GL_TEXTURE_3D, v.probes);
		glActiveTexture(GL_TEXTURE0 + UNIT_ROOM);
		glBindTexture(GL_TEXTURE_2D, v.room);
		glActiveTexture(GL_TEXTURE0);
		UploadFrame(&f);
		PointArrays();
		const Moved moved = MoveOfFrame(); //FIELD_SMOOTH_SCROLL, FIELD_INTERPOLATION
		size_t captures = 0;
		bool failed = false;
		FieldTimeAdd(TIME_CHARS_PREP, started);
		FrameLogMark("characters: DEMASTER starts drawing them");
		try
		{
			//4. pictures, shadow maps, faces
			long long t = FieldTimeNow();
			if (FIELD_PBR_REFLECTIONS && CurrentQuality().reflections && shared.count > 0 && DrawCaptures(f, v, entry, now))
				captures = static_cast<size_t>(shared.count) * 2;
			FieldTimeAdd(TIME_CHARS_PICTURES, t);
			FrameLogMark("characters: reflection pictures");
			t = FieldTimeNow();
			if (FIELD_PBR_SHADOWS && shared.count > 0 && shadowProgram.id != 0)
				DrawShadows(f, v);
			FieldTimeAdd(TIME_CHARS_SHADOWS, t);
			FrameLogMark("characters: shadow maps");
			t = FieldTimeNow();
			if (FIELD_AVATARS && !f.characters.empty())
				DrawAvatars(f, v, entry, now);
			FieldTimeAdd(TIME_CHARS_PICTURES, t);
			t = FieldTimeNow();
			shared.features[0] = shared.shadowCount > 0 ? 1.0f : 0.0f;
			shared.features[1] = shared.capture != 0 ? 1.0f : 0.0f;
			shared.features[2] = FIELD_PBR_SHADOWS ? 1.0f : 0.0f;
			shared.features[3] = FIELD_PBR_SHADOWS && v.room != 0 ? 1.0f : 0.0f;
			//5. the models on the game's screen, in the game's order: opaque first, then the rest as the game drew them
			const CharProgram& p = charProgram;
			glUseProgram(p.id);
			LutUse(p.id, true); //LUT: the time of day's colour grade
			SetCommonUniforms(p, v, FIELD_PBR_VIEW);
			glUniform4f(p.charMode, 0.0f, 0.0f, 0.0f, 0.0f);
			glUniform4f(p.viewer, 0.0f, 0.0f, 0.0f, 1.0f);
			size_t passes = 0;
			//intoCopy: the second time, for the in-between picture (their places there, the copy of the game's framebuffer)
			const auto drawModels = [&](const bool intoCopy)
			{
				for (int opaquePass = 1; opaquePass >= 0; opaquePass--)
				{
					const CharDraw* last = nullptr;
					for (size_t i = 0; i < draws.size(); i++)
					{
						const CharDraw& d = draws[i];
						if (d.opaque != (opaquePass == 1) || (intoCopy && d.state.drawFramebuffer != moved.framebuffer))
							continue;
						//the same state as the draw before it: one draw for both
						size_t end = i + 1;
						while (end < draws.size() && draws[end].opaque == d.opaque && draws[end].texture == d.texture
							&& draws[end].first == draws[end - 1].first + draws[end - 1].count
							&& memcmp(&draws[end].state, &d.state, sizeof(RenderState)) == 0 && memcmp(draws[end].ambient, d.ambient, sizeof(d.ambient)) == 0
							&& draws[end].blendMode == d.blendMode && memcmp(draws[end].projection, d.projection, sizeof(d.projection)) == 0)
							end++;
						if (last == nullptr || memcmp(&last->state, &d.state, sizeof(RenderState)) != 0)
						{
							RenderState state = d.state;
							if (intoCopy)
								state.drawFramebuffer = static_cast<GLint>(moved.copy);
							Apply(state);
						}
						if (cullPatched)
						{
							glEnable(GL_CULL_FACE);
							glCullFace(GL_BACK);
							glFrontFace(GL_CCW); //the game's front faces turn counter-clockwise on the GL screen
						}
						//FIELD_SMOOTH_SCROLL: with the main layer's shift (the in-between places have theirs in them)
						float screen[16];
						fieldtween::ShiftProjection(d.projection, intoCopy ? 0.0 : moved.shift[0], intoCopy ? 0.0 : moved.shift[1], screen);
						glUniformMatrix4fv(p.screenMatrix, 1, GL_FALSE, screen);
						SetDrawUniforms(p, d, v, entry, now);
						DrawRange(d.first, draws[end - 1].first + draws[end - 1].count - d.first);
						if (!intoCopy)
							passes++;
						last = &draws[end - 1];
						i = end - 1;
					}
				}
			};
			drawModels(false);
			if (moved.between != nullptr)
			{
				PointPositions(moved.between);
				drawModels(true);
				PointPositions(nullptr);
			}
			FieldTimeAdd(TIME_CHARS_SCREEN, t);
			FrameLogMark("characters: on the game's screen");
			//6. their depth, for the background to step aside where they stand in front of what it shows (only before the
			//background is drawn: models the game draws after it, glows and the like, don't need it)
			t = FieldTimeNow();
			const bool depthCopied = FIELD_PBR_DEPTH && !PbrDrewThisFrame() && CopyCharacterDepth(gameState, shared);
			if (depthCopied)
				depthThisFrame = true;
			else if (FIELD_PBR_DEPTH && depthThisFrame)
			{
				//more models after some of the background: the background still drawn keeps the depth copied before it
				shared.depth = depthCopy.texture;
				shared.depthWidth = depthCopy.width;
				shared.depthHeight = depthCopy.height;
			}
			FieldTimeAdd(TIME_CHARS_DEPTH, t);
			if (depthCopied)
				FrameLogMark("characters: their depth copied");
			FrameLogNote("%zu character draw(s) of the game drawn here as %zu, %zu model(s): smooth normals%s%s%s%s%s%s%s", draws.size(), passes,
				f.characters.size(), FIELD_PBR_CHARACTERS ? ", lit by the room" : "", captures ? ", reflection pictures" : "",
				shared.shadowCount ? (", " + std::to_string(shared.shadowCount) + " shadow map(s)").c_str() : "", FIELD_AVATARS ? ", faces" : "",
				depthCopied ? ", their depth copied for the background" : "",
				gpu.on ? "; sent to the GPU once (" : "; drawn from the game's memory (",
				drawInstanced != nullptr ? "shadow maps and pictures instanced)" : "no instancing)");
		}
		catch (...)
		{
			LogOnce("chars-flush-failed", "FIELD_PBR: drawing the field characters failed once (drawn as the game draws them)\n");
			failed = true;
		}
		//the game's state back
		RestoreClient(client);
		for (int u = UNIT_ROOM; u >= 0; u--)
		{
			glActiveTexture(GL_TEXTURE0 + u);
			if (u == UNIT_PROBES)
				glBindTexture(GL_TEXTURE_3D, probes3d);
			glBindTexture(GL_TEXTURE_2D, units[u]);
		}
		glActiveTexture(active);
		glUseProgram(static_cast<GLuint>(program));
		glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
		glClearDepth(clearDepth);
		Apply(gameState);
		if (failed)
			ReplayAsGame();
		else
			PbrSetCharacters(&f.shared);
		draws.clear();
		vertices.clear();
		keys.clear();
		gpu.on = false;
	}

	//the round shadows (8 triangles each: a grey centre, a black rim, taken away from the picture)
	bool IsRoundShadow(const GLenum mode, const GLsizei count)
	{
		if (mode != GL_TRIANGLES || count <= 0 || count % 24 != 0 || !glstate::BlendOn())
			return false;
		return glstate::BlendEquationRgb() == GL_FUNC_REVERSE_SUBTRACT;
	}

	void ApplyCullPatch(const bool on)
	{
		if (on == cullPatched)
			return;
		for (const CullSite& site : cullSites)
			if (!site.known)
				return;
		for (const CullSite& site : cullSites)
		{
			ModPage(site.at, 6);
			if (on)
				memset(reinterpret_cast<void*>(site.at), 0x90, 6);
			else
				memcpy(reinterpret_cast<void*>(site.at), site.original, 6);
		}
		FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
		cullPatched = on;
	}
}

//game thread: a field's .map was read. The game's back-face cull goes off on PBR fields (shadows, reflections, faces need
//the whole model) and back on everywhere else.
void FieldCharsMapRead(const bool pbrField)
{
	static bool looked = false;
	if (!looked)
	{
		looked = true;
		const ADDRESS_NAME names[2] = { FIELD_CULL_TRI, FIELD_CULL_QUAD };
		for (int i = 0; i < 2; i++)
		{
			if (!HasAddress(names[i]))
				continue;
			CullSite& site = cullSites[i];
			site.at = IMAGE_BASE + GetAddress(names[i]);
			const BYTE* code = reinterpret_cast<const BYTE*>(site.at);
			//jz / jnz rel32
			if (code[0] == 0x0F && (code[1] == 0x84 || code[1] == 0x85))
			{
				memcpy(site.original, code, 6);
				site.known = true;
			}
			else
				OutputDebug("FIELD_PBR: unexpected code at %08X (%s): characters keep only the halves the camera sees\n", site.at,
					GetAddressName(names[i]));
		}
	}
	ApplyCullPatch(pbrField && Wanted() && WholeModels() && !charsBroken);
}

//glDrawElements hook (before anything else of DEMASTER's). true: DEMASTER takes the draw.
bool FieldCharsDraw(const unsigned int mode, const int count, const unsigned int type, const void* indices)
{
	//FIELD_SMOOTH_SCROLL, FIELD_INTERPOLATION: on a field drawn from layer images the characters are kept without PBR too
	//(they wait for the main layer's place, and are drawn as the game draws them: ReplayAsGame)
	const bool tweenKeeps = TweenKeepsCharacters();
	if (!Wanted() && !tweenKeeps && !pending)
		return false;
	const FieldView& v = PbrView();
	const bool field = Wanted() && v.active && v.settings != nullptr;
	if (!field && !tweenKeeps && !pending && !cullPatched)
		return false; //not on a PBR field: nothing to do (no GL calls)
	//FIELD_FRAME_LOG's timing: this hook's time, without the drawing (Flush times itself)
	struct ReadTime
	{
		long long start = FieldTimeNow(), drawing = 0;
		~ReadTime() { FieldTimeAdd(TIME_CHARS_READ, start + drawing); }
	} readTime;
	const auto flush = [&readTime]()
	{
		const long long t = FieldTimeNow();
		Flush();
		readTime.drawing += FieldTimeNow() - t;
	};
	try
	{
		const GLint program = glstate::Program();
		static const GameProgram none;
		const GameProgram& game = program != 0 ? GameProgramOf(program) : none;
		if (game.kind == ProgramKind::Character && ((field && !charsBroken && MakePrograms()) || (tweenKeeps && !charsBroken)))
		{
			glstate::CheckPoint(); //FIELD_GL_TRACK: now and then the remembered state is compared with the driver's here
			const GLint vertexType = game.vertexType >= 0 ? glstate::UniformInt(program, game.vertexType) : 3;
			if (mode == GL_TRIANGLES && vertexType == 3 && count > 0 && count % 3 == 0 && draws.size() < MAX_DRAWS
				&& vertices.size() + static_cast<size_t>(count) < MAX_VERTICES)
			{
				static std::vector<uint32_t> list; //kept between draws (no allocation per draw)
				static std::vector<GameVertex> gameVertices;
				if (ReadIndices(count, type, indices, list) == count && ReadGameVertices(list, gameVertices))
				{
					CharDraw d;
					Capture(d.state);
					d.program = program;
					d.texture = glstate::BoundTexture2D();
					if (game.ambient >= 0)
						glstate::UniformVec3(program, game.ambient, d.ambient);
					if (game.blendMode >= 0)
						d.blendMode = glstate::UniformInt(program, game.blendMode);
					glstate::ProjectionMatrix(d.projection);
					d.opaque = !d.state.blend || (d.state.blendSrcRgb == GL_ONE && d.state.blendDstRgb == GL_ZERO);
					d.first = static_cast<uint32_t>(vertices.size());
					d.count = static_cast<uint32_t>(gameVertices.size());
					for (const GameVertex& g : gameVertices)
					{
						Vertex vx{};
						vx.pos[0] = g.x;
						vx.pos[1] = g.y;
						vx.pos[2] = g.z;
						vx.pos[3] = g.rhw;
						memcpy(vx.color, g.color, 4);
						vx.uv[0] = g.u;
						vx.uv[1] = g.v;
						vertices.push_back(vx);
						keys.push_back(VertexKey(g));
					}
					draws.push_back(d);
					pending = true;
					return true;
				}
			}
		}
		//anything else: the characters kept so far are drawn first
		if (pending)
			flush();
		if (game.kind == ProgramKind::Character && cullPatched)
		{
			//a model DEMASTER does not draw itself while the game sends back faces: the GPU hides them
			GLboolean cull = GL_FALSE;
			GLint cullMode = GL_BACK, front = GL_CCW;
			glstate::CullState(cull, cullMode, front);
			glEnable(GL_CULL_FACE);
			glCullFace(GL_BACK);
			glFrontFace(GL_CCW);
			reinterpret_cast<void(__stdcall*)(GLenum, GLsizei, GLenum, const void*)>(ogl_draw_elements)(mode, count, type, indices);
			Enable(GL_CULL_FACE, cull != GL_FALSE);
			glCullFace(static_cast<GLenum>(cullMode));
			glFrontFace(static_cast<GLenum>(front));
			return true;
		}
		if (game.kind == ProgramKind::NoTexture && field && IsRoundShadow(mode, count))
		{
			static std::vector<uint32_t> list;
			static std::vector<GameVertex> gameVertices;
			if (ReadIndices(count, type, indices, list) == count && ReadGameVertices(list, gameVertices))
			{
				lastDiscs = ReadDiscs(gameVertices);
				lastDiscsEntry = v.entry;
			}
			if (FIELD_PBR_SHADOWS && !charsBroken && charProgram.id != 0)
			{
				FrameLogNote("round shadows left out (%zu character(s)): DEMASTER's shadows and contact darkening use their size and darkness",
					lastDiscs.size());
				return true;
			}
		}
	}
	catch (...)
	{
		charsBroken = true;
		pending = false;
		draws.clear();
		vertices.clear();
		keys.clear();
		Log("FIELD_PBR: reading the field characters failed: the game draws them itself for the rest of this session\n");
	}
	return false;
}

//texturepatch_v2_fieldTween.cpp: are characters waiting (and for which framebuffer); draw them now
bool fieldrt::CharsPending(GLint& framebuffer)
{
	if (!pending || draws.empty())
		return false;
	framebuffer = draws[0].state.drawFramebuffer;
	return true;
}

void fieldrt::CharsFlush()
{
	if (pending)
		Flush();
}

//SwapBuffers: characters still waiting are drawn; the background's measures become the next frame's; Ctrl+F10
void FieldCharsSwap()
{
	try
	{
		if (pending)
			Flush();
		PbrEndFrame();
		PbrSetCharacters(nullptr);
		depthThisFrame = false;
		glstate::FrameEnd(); //FIELD_GL_TRACK: now and then the remembered state is compared with the driver's
		if (FIELD_AVATARS)
		{
			const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
			if (down && !avatarKeyDown)
				saveAvatars = true;
			avatarKeyDown = down;
			if (saveAvatars)
			{
				saveAvatars = false;
				SaveAvatarFiles(PbrView().name.empty() ? std::string("field") : PbrView().name);
			}
		}
	}
	catch (...)
	{
		LogOnce("chars-swap-failed", "FIELD_PBR: the end of a frame failed once\n");
	}
}

bool fieldrt::ReadDrawCorners(const GLsizei count, const GLenum type, const void* indices, std::vector<GameVertex>& corners)
{
	static std::vector<uint32_t> list;
	return count > 0 && ReadIndices(count, type, indices, list) == count && ReadGameVertices(list, corners);
}
