//FIELD_SMOOTH_SCROLL and FIELD_INTERPOLATION (game side). The rules are in field_tween.h, the second picture's
//framebuffers in field_mirror.h; this file is what happens around the game's draws in a field drawn from layer images.
//
//Every draw of the game comes through opengl.cpp's glDrawElements hook:
//  TweenBeforeDraw   which layer is this (the page's cells say), where did the game put it (the vertices say)? The first
//                    draw of a layer in a frame moves its filter on (fieldtween::Advance).
//  FieldCharsDraw    the character module keeps character draws and draws them before the next other draw
//  TweenDraw         the draw goes to the driver with the projection moved by the layer's shift; when this frame gets an
//                    in-between picture, it goes a second time, into the copies, with the in-between shift
//
//The characters are moved by the main layer's shift, and the game draws them before the background. So they wait: the
//character module asks for the shift (TweenCharacterShift) when the next other draw arrives, and that draw has been looked
//at by then. When it is the main layer's, the shift is known. When it is another layer's opaque tiles, the draw is kept
//back (Hold) and replayed right after the characters, which keeps the game's order (characters, then background) exactly;
//anything else ends the waiting with no shift for this frame (the main layer then stays on the game's place too, so the
//two never disagree).
//
//Not moved: things the game draws in the world that are neither tiles nor characters (most effects). They are at most
//3/4 of a PlayStation pixel off while the screen scrolls.
#include "config.h"
#include "coreHeader.h"
#include "opengl.h"
#include "texture.h"
#include "field_pbr_runtime.h"
#include "field_mirror.h"
#include "gl_state.h"
#include "minhook/include/MinHook.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

using namespace fieldrt;
using glstate::Apply;
using glstate::Capture;
using glstate::ClientState;
using glstate::RenderState;

namespace
{
	bool hooksMade = false;
	bool off = false; //something went wrong once: the game's draws go through untouched for the rest of the session

	//--- the field's pages and layers
	std::unordered_map<GLuint, fieldtween::PageCells> pages; //by the game's texture
	std::unordered_map<int, fieldtween::LayerTrack> tracks; //by layer id
	//how far each layer's tiles reach on its canvas (PlayStation pixels), from the pages' cells
	struct Extent
	{
		int minX = 1 << 30, minY = 1 << 30, maxX = -(1 << 30), maxY = -(1 << 30);
	};
	std::unordered_map<int, Extent> extents;
	int mainLayer = -1; //the layer with the most tiles in the last frame that drew any
	uint32_t frameNumber = 1;
	bool pagesDrawn = false; //this frame
	bool fieldLive = false; //the last frame drew layers and the main one is known
	//the main layer's shift for this frame, once something needed it
	bool mainFixed = false, mainTween = false;
	double mainNow[2] = { 0, 0 }, mainBetween[2] = { 0, 0 };

	//the draw being handled
	struct Current
	{
		int layer = -1; //of a page, placed as its layer is
		bool world = false; //the game's round shadows: they follow the main layer
		double x = 0, y = 0;
		int triangles = 0;
	};
	Current cur;

	//--- the in-between picture
	fieldtween::Pacer pacer;
	bool frameClosed = false; //SwapBuffers was called: what is drawn from here to the end of it is DEMASTER's own
	bool mirroredLast = false, doubled = false, beginSeen = false, restInField = false, sessionOff = false, vsync = false;
	int slowStrikes = 0, beginFrames = 0;
	long long ticksPerSecond = 0, lastFrameEnd = 0;
	uint32_t vsyncChecked = 0;
	std::vector<fieldtween::CharFrame> batchesBefore, batchesNow; //what the character module drew, flush by flush
	size_t batchIndex = 0;

	//the game's calls, as the driver has them
	void (APIENTRY* clearOriginal)(GLbitfield) = nullptr;
	void (APIENTRY* copySubOriginal)(GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei) = nullptr;
	void (APIENTRY* copyImageOriginal)(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint) = nullptr;
	typedef void (APIENTRY* BlitFunction)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
	BlitFunction blitOriginal = nullptr, blitExtOriginal = nullptr;
	void (APIENTRY* drawArraysOriginal)(GLenum, GLint, GLsizei) = nullptr;
	void (APIENTRY* beginOriginal)(GLenum) = nullptr;

	long long Ticks()
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return now.QuadPart;
	}

	void TurnOff(const char* where)
	{
		if (!off)
			Log("FIELD_SMOOTH_SCROLL / FIELD_INTERPOLATION: %s failed once: both are off for the rest of this session\n", where);
		off = true;
	}

	void RawDraw(const GLenum mode, const GLsizei count, const GLenum type, const void* indices)
	{
		reinterpret_cast<void(__stdcall*)(GLenum, GLsizei, GLenum, const void*)>(ogl_draw_elements)(mode, count, type, indices);
	}

	//--- what the mirror asks (field_mirror.h)
	fieldmirror::Name HostDrawFramebuffer() { return glstate::DrawFramebuffer(); }
	fieldmirror::Name HostReadFramebuffer() { return glstate::ReadFramebuffer(); }
	unsigned int HostActiveTexture() { return glstate::ActiveTexture(); }
	fieldmirror::Name HostTexture(const int unit) { return glstate::Texture(GL_TEXTURE_2D, unit); }
	void HostBindTexture(const fieldmirror::Name texture)
	{
		//past DEMASTER's own glBindTexture hook: the texture the game has bound is back before anything else looks
		if (ogl_bind_texture != nullptr)
			reinterpret_cast<void(__stdcall*)(GLenum, GLuint)>(ogl_bind_texture)(GL_TEXTURE_2D, texture);
		else
			glBindTexture(GL_TEXTURE_2D, texture);
	}
	void HostDrawElements(const unsigned int mode, const int count, const unsigned int type, const void* indices)
	{
		RawDraw(mode, count, type, indices);
	}
	void HostClear(const unsigned int mask) { clearOriginal(mask); }
	void HostBlit(const int a, const int b, const int c, const int d, const int e, const int f, const int g, const int h,
		const unsigned int mask, const unsigned int filter)
	{
		blitOriginal(a, b, c, d, e, f, g, h, mask, filter);
	}
	void HostCopySub(const unsigned int target, const int level, const int xo, const int yo, const int x, const int y, const int w,
		const int h)
	{
		copySubOriginal(target, level, xo, yo, x, y, w, h);
	}
	void HostCopyImage(const unsigned int target, const int level, const unsigned int format, const int x, const int y, const int w,
		const int h, const int border)
	{
		copyImageOriginal(target, level, format, x, y, w, h, border);
	}
	void HostTexImage(const int format, const int width, const int height, const unsigned int pixelFormat, const unsigned int type)
	{
		RawTexImage(format, width, height, pixelFormat, type, nullptr);
	}
	void HostTexParameter(const unsigned int name, const int value)
	{
		if (ogl_tex_parametri != nullptr)
			reinterpret_cast<void(__stdcall*)(GLenum, GLenum, GLint)>(ogl_tex_parametri)(GL_TEXTURE_2D, name, value);
		else
			glTexParameteri(GL_TEXTURE_2D, name, value);
	}
	void HostLog(const char* text) { Log("%s", text); }

	//a layer's shift, held back where it would uncover an edge of the picture the layer reaches as the game draws it
	void Cover(const int layer, const fieldtween::LayerTrack& track, double shift[2])
	{
		const auto extent = extents.find(layer);
		if (extent == extents.end() || extent->second.maxX < extent->second.minX)
			return;
		const Extent& e = extent->second;
		shift[0] = fieldtween::KeepCovering(track.whole[0] + fieldtween::PIXEL * e.minX,
			track.whole[0] + fieldtween::PIXEL * (e.maxX + fieldtween::TILE), shift[0], false);
		shift[1] = fieldtween::KeepCovering(track.whole[1] + fieldtween::PIXEL * e.minY,
			track.whole[1] + fieldtween::PIXEL * (e.maxY + fieldtween::TILE), shift[1], true);
	}

	//--- the main layer's shift for this frame
	void FixMain(const bool known)
	{
		mainFixed = true;
		mainTween = false;
		mainNow[0] = mainNow[1] = mainBetween[0] = mainBetween[1] = 0;
		if (!known || mainLayer < 0)
			return;
		const auto it = tracks.find(mainLayer);
		if (it == tracks.end() || it->second.frame != frameNumber)
			return;
		fieldtween::ShiftNow(it->second, mainNow);
		fieldtween::ShiftBetween(it->second, mainBetween);
		Cover(mainLayer, it->second, mainNow);
		Cover(mainLayer, it->second, mainBetween);
		mainTween = it->second.tween;
	}

	//--- which layer a draw is

	//the draw's indices and the vertices they reach (the game draws from its own memory: x, y, z, rhw ... u, v)
	struct DrawData
	{
		std::vector<uint32_t> indices; //from 0 (lowest subtracted)
		std::vector<uint8_t> positions, uvs;
		uint32_t lowest = 0;
		size_t vertexCount = 0;
		int stride = 0, uvStride = 0;
	};
	bool ReadDraw(const GLsizei count, const GLenum type, const void* indices, DrawData& d)
	{
		if (count < 3 || count > 6000 || indices == nullptr || (type != GL_UNSIGNED_SHORT && type != GL_UNSIGNED_INT))
			return false;
		if (LastVerticesPointer.pointer == nullptr || LastVerticesPointer.size != 4 || LastTexCoordPointer.pointer == nullptr)
			return false;
		d.stride = LastVerticesPointer.stride != 0 ? LastVerticesPointer.stride : 16;
		d.uvStride = LastTexCoordPointer.stride != 0 ? LastTexCoordPointer.stride : 8;
		d.indices.resize(static_cast<size_t>(count));
		if (type == GL_UNSIGNED_SHORT)
		{
			static std::vector<uint16_t> raw;
			raw.resize(static_cast<size_t>(count));
			if (!SafeRead(raw.data(), indices, raw.size() * 2))
				return false;
			std::copy(raw.begin(), raw.end(), d.indices.begin());
		}
		else if (!SafeRead(d.indices.data(), indices, d.indices.size() * 4))
			return false;
		uint32_t lo = 0xFFFFFFFF, hi = 0;
		for (const uint32_t i : d.indices)
		{
			lo = std::min(lo, i);
			hi = std::max(hi, i);
		}
		if (hi - lo > 65535)
			return false;
		d.lowest = lo;
		d.vertexCount = static_cast<size_t>(hi - lo) + 1;
		d.positions.resize(d.vertexCount * d.stride);
		d.uvs.resize(d.vertexCount * d.uvStride);
		if (!SafeRead(d.positions.data(), static_cast<const uint8_t*>(LastVerticesPointer.pointer) + static_cast<size_t>(lo) * d.stride,
				(d.vertexCount - 1) * d.stride + 16)
			|| !SafeRead(d.uvs.data(), static_cast<const uint8_t*>(LastTexCoordPointer.pointer) + static_cast<size_t>(lo) * d.uvStride,
				(d.vertexCount - 1) * d.uvStride + 8))
			return false;
		for (uint32_t& i : d.indices)
			i -= lo;
		return true;
	}

	//cur: what this draw is. A layer's first draw of the frame moves its filter on.
	void Classify(const GameProgram& game, const GLenum mode, const GLsizei count, const GLenum type, const void* indices)
	{
		if (mode != GL_TRIANGLES)
			return;
		if (game.kind == ProgramKind::NoTexture)
		{
			//the round shadows (8 triangles each, taken away from the picture): under the characters, on the main layer
			cur.world = fieldLive && count % 24 == 0 && glstate::BlendOn() && glstate::BlendEquationRgb() == GL_FUNC_REVERSE_SUBTRACT;
			return;
		}
		if (game.kind != ProgramKind::Main)
			return;
		const auto page = pages.find(glstate::BoundTexture2D());
		if (page == pages.end())
			return;
		static DrawData data; //kept between draws (no allocation per draw)
		if (!ReadDraw(count, type, indices, data))
			return;
		const fieldtween::DrawPlace place = fieldtween::PlaceOfDraw(page->second, data.positions.data(), static_cast<size_t>(data.stride),
			data.uvs.data(), static_cast<size_t>(data.uvStride), data.vertexCount, data.indices.data(), data.indices.size());
		if (!place.known)
			return;
		fieldtween::LayerTrack& track = tracks[place.layer];
		if (track.frame != frameNumber)
			fieldtween::Advance(track, place.x, place.y, frameNumber, FIELD_SMOOTH_SCROLL != 0);
		else if (std::fabs(place.x - track.whole[0]) > 0.5 || std::fabs(place.y - track.whole[1]) > 0.5)
			return; //tiles of the layer somewhere else than the layer is this frame: left where the game has them
		track.triangles += place.triangles;
		pagesDrawn = true;
		cur.layer = place.layer;
		cur.x = place.x;
		cur.y = place.y;
		cur.triangles = place.triangles;
	}

	//how far this draw is moved: in the game's frame, and in the in-between picture
	void ShiftsOf(const Current& c, double now[2], double between[2])
	{
		now[0] = now[1] = between[0] = between[1] = 0;
		if (c.layer >= 0 && c.layer == mainLayer)
		{
			if (!mainFixed)
				FixMain(true);
			memcpy(now, mainNow, sizeof(mainNow));
			memcpy(between, mainBetween, sizeof(mainBetween));
		}
		else if (c.layer >= 0)
		{
			const auto it = tracks.find(c.layer);
			if (it == tracks.end() || it->second.frame != frameNumber)
				return;
			fieldtween::ShiftNow(it->second, now);
			fieldtween::ShiftBetween(it->second, between);
			Cover(c.layer, it->second, now);
			Cover(c.layer, it->second, between);
		}
		else if (c.world && mainFixed)
		{
			memcpy(now, mainNow, sizeof(mainNow));
			memcpy(between, mainBetween, sizeof(mainBetween));
		}
	}

	//One draw of the game to the driver: with DEMASTER's field program when it is a PBR page (FieldLayersBeforeDraw), moved
	//when it is a layer's, and a second time into the copies when this frame gets an in-between picture.
	void DrawNow(const GLenum mode, const GLsizei count, const GLenum type, const void* indices)
	{
		const bool mirror = fieldmirror::Active();
		if (mirror)
			fieldmirror::BeforeDraw();
		FieldLayersBeforeDraw(count, type, indices);
		double now[2] = { 0, 0 }, between[2] = { 0, 0 };
		float game[16], moved[16];
		GLint matrixMode = GL_MODELVIEW;
		bool loaded = false;
		if (!off && (cur.layer >= 0 || cur.world))
		{
			ShiftsOf(cur, now, between);
			if (now[0] != 0 || now[1] != 0 || (mirror && (between[0] != 0 || between[1] != 0)))
			{
				RenderState state;
				Capture(state);
				glstate::ProjectionMatrix(game);
				TweenSnap(now, game, state.viewport[2], state.viewport[3]);
				TweenSnap(between, game, state.viewport[2], state.viewport[3]);
			}
			if (now[0] != 0 || now[1] != 0 || (mirror && (between[0] != 0 || between[1] != 0)))
			{
				matrixMode = glstate::MatrixMode();
				glMatrixMode(GL_PROJECTION);
				loaded = true;
			}
		}
		if (now[0] != 0 || now[1] != 0)
		{
			fieldtween::ShiftProjection(game, now[0], now[1], moved);
			glLoadMatrixf(moved);
		}
		RawDraw(mode, count, type, indices);
		if (mirror)
		{
			if (between[0] != now[0] || between[1] != now[1])
			{
				fieldtween::ShiftProjection(game, between[0], between[1], moved);
				glLoadMatrixf(moved);
			}
			fieldmirror::Draw(mode, count, type, indices);
		}
		if (loaded)
		{
			glLoadMatrixf(game);
			glMatrixMode(static_cast<GLenum>(matrixMode));
		}
		FieldLayersAfterDraw();
	}

	//--- draws kept back until the characters are drawn

	struct ArrayCopy
	{
		bool on = false;
		GLint size = 4, type = GL_FLOAT, stride = 0;
		std::vector<uint8_t> bytes; //the vertices lowest .. highest
	};
	struct Held
	{
		RenderState state;
		GLint program = 0, blendMode = 0, vertexType = 0;
		GLuint texture = 0;
		float projection[16] = {};
		GLsizei count = 0;
		std::vector<uint32_t> indices; //from 0
		ArrayCopy vertex, color, uv;
		Current what;
	};
	std::vector<Held> held;
	size_t heldBytes = 0;
	bool releasing = false;
	constexpr size_t MAX_HELD = 64, MAX_HELD_BYTES = 8u << 20;

	int TypeSize(const GLint type)
	{
		switch (type)
		{
		case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: return 4;
		case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
		case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
		case GL_DOUBLE: return 8;
		default: return 0;
		}
	}

	bool CopyArray(const glstate::ArrayState& a, const uint32_t lowest, const size_t vertexCount, ArrayCopy& out)
	{
		out.on = a.on != GL_FALSE;
		if (!out.on)
			return true;
		const int element = a.size * TypeSize(a.type);
		if (a.buffer != 0 || a.pointer == nullptr || element <= 0)
			return false;
		out.size = a.size;
		out.type = a.type;
		out.stride = a.stride != 0 ? a.stride : element;
		out.bytes.resize((vertexCount - 1) * out.stride + element);
		return SafeRead(out.bytes.data(), static_cast<const uint8_t*>(a.pointer) + static_cast<size_t>(lowest) * out.stride, out.bytes.size());
	}

	//An opaque draw of a layer that is not the main one, while characters wait for the main layer's place: kept (with
	//everything it needs to be drawn later exactly as now). false: it can't be, the waiting ends.
	bool Hold(const GameProgram& game, const GLint program, const GLint charactersFramebuffer, const GLsizei count, const GLenum type,
		const void* indices)
	{
		if (cur.layer < 0 || mainLayer < 0 || game.kind != ProgramKind::Main || held.size() >= MAX_HELD || heldBytes > MAX_HELD_BYTES)
			return false;
		//the main layer was drawn last frame, so it will come
		if (const auto mainTrack = tracks.find(mainLayer); mainTrack == tracks.end() || mainTrack->second.frame + 1 != frameNumber)
			return false;
		//nothing would move anyway
		if (!FIELD_SMOOTH_SCROLL && !fieldmirror::Active())
			return false;
		Held h;
		Capture(h.state);
		const RenderState& s = h.state;
		const bool opaque = !s.blend || (s.blendSrcRgb == GL_ONE && s.blendDstRgb == GL_ZERO);
		if (!opaque || !s.depthTest || !s.depthMask || s.drawFramebuffer != charactersFramebuffer)
			return false;
		ClientState client;
		glstate::SaveClient(client);
		if (client.elementBuffer != 0 || client.normal.on || client.uv1.on || !client.vertex.on || !client.uv0.on)
			return false;
		static DrawData data;
		if (!ReadDraw(count, type, indices, data))
			return false;
		if (!CopyArray(client.vertex, data.lowest, data.vertexCount, h.vertex) || !CopyArray(client.color, data.lowest, data.vertexCount, h.color)
			|| !CopyArray(client.uv0, data.lowest, data.vertexCount, h.uv))
			return false;
		h.program = program;
		if (game.blendMode >= 0)
			h.blendMode = glstate::UniformInt(program, game.blendMode);
		if (game.vertexType >= 0)
			h.vertexType = glstate::UniformInt(program, game.vertexType);
		h.texture = glstate::BoundTexture2D();
		glstate::ProjectionMatrix(h.projection);
		h.count = count;
		h.indices = data.indices;
		h.what = cur;
		heldBytes += h.vertex.bytes.size() + h.color.bytes.size() + h.uv.bytes.size() + h.indices.size() * 4;
		held.push_back(std::move(h));
		return true;
	}

	const void* ArrayStart(const ArrayCopy& a)
	{
		return a.bytes.data();
	}

	//the kept draws, now, as the game made them; everything of the game's is put back after
	void ReplayHeld()
	{
		if (held.empty())
			return;
		std::vector<Held> list;
		list.swap(held);
		heldBytes = 0;
		RenderState gameState;
		Capture(gameState);
		ClientState client;
		glstate::SaveClient(client);
		const GLint program = glstate::Program();
		const GLenum active = glstate::ActiveTexture();
		const GLint matrixMode = glstate::MatrixMode();
		GLfloat projection[16]{};
		glstate::ProjectionMatrix(projection);
		const GLuint unit0 = glstate::Texture(GL_TEXTURE_2D, 0);
		const Current triggering = cur;
		glActiveTexture(GL_TEXTURE0);
		glClientActiveTexture(GL_TEXTURE0);
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		for (const Held& h : list)
		{
			const GameProgram& game = GameProgramOf(h.program);
			glUseProgram(static_cast<GLuint>(h.program));
			//the game's uniforms of that draw (and back: the game has set them for the draw it is at now)
			GLint blendNow = h.blendMode, typeNow = h.vertexType;
			if (game.blendMode >= 0 && (blendNow = glstate::UniformInt(h.program, game.blendMode)) != h.blendMode)
				glUniform1i(game.blendMode, h.blendMode);
			if (game.vertexType >= 0 && (typeNow = glstate::UniformInt(h.program, game.vertexType)) != h.vertexType)
				glUniform1i(game.vertexType, h.vertexType);
			Apply(h.state);
			glMatrixMode(GL_PROJECTION);
			glLoadMatrixf(h.projection);
			glBindTexture(GL_TEXTURE_2D, h.texture);
			glVertexPointer(h.vertex.size, static_cast<GLenum>(h.vertex.type), h.vertex.stride, ArrayStart(h.vertex));
			glEnableClientState(GL_VERTEX_ARRAY);
			if (h.color.on)
			{
				glColorPointer(h.color.size, static_cast<GLenum>(h.color.type), h.color.stride, ArrayStart(h.color));
				glEnableClientState(GL_COLOR_ARRAY);
			}
			else
				glDisableClientState(GL_COLOR_ARRAY);
			glTexCoordPointer(h.uv.size, static_cast<GLenum>(h.uv.type), h.uv.stride, ArrayStart(h.uv));
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);
			cur = h.what;
			DrawNow(GL_TRIANGLES, h.count, GL_UNSIGNED_INT, h.indices.data());
			if (game.blendMode >= 0 && blendNow != h.blendMode)
				glUniform1i(game.blendMode, blendNow);
			if (game.vertexType >= 0 && typeNow != h.vertexType)
				glUniform1i(game.vertexType, typeNow);
		}
		cur = triggering;
		glstate::RestoreClient(client);
		glMatrixMode(GL_PROJECTION);
		glLoadMatrixf(projection);
		glMatrixMode(static_cast<GLenum>(matrixMode));
		glBindTexture(GL_TEXTURE_2D, unit0);
		glActiveTexture(active);
		glUseProgram(static_cast<GLuint>(program));
		Apply(gameState);
		FrameLogNote("%zu draw(s) of layers the game draws before the main one were kept back until the characters were drawn", list.size());
	}

	//the waiting ends: the characters are drawn (with the main layer's shift if it is known by now), then what was kept
	void ReleaseAll()
	{
		if (held.empty() || releasing)
			return;
		releasing = true;
		try
		{
			if (!mainFixed)
				FixMain(false);
			CharsFlush();
			ReplayHeld();
		}
		catch (...)
		{
			TurnOff("drawing kept-back layers");
		}
		releasing = false;
	}

	//something of the game's other than a draw (a clear, a copy) while draws are kept back
	void BeforeOther()
	{
		GLint framebuffer = 0;
		if (!held.empty() && !releasing && CharsPending(framebuffer))
			ReleaseAll();
	}

	//--- the hooks on the game's other framebuffer calls
	void APIENTRY HookClear(const GLbitfield mask)
	{
		BeforeOther();
		clearOriginal(mask);
		if (fieldmirror::Active())
			fieldmirror::Clear(mask);
	}
	void APIENTRY HookCopySub(const GLenum target, const GLint level, const GLint xo, const GLint yo, const GLint x, const GLint y,
		const GLsizei w, const GLsizei h)
	{
		BeforeOther();
		copySubOriginal(target, level, xo, yo, x, y, w, h);
		if (fieldmirror::Active())
			fieldmirror::CopyTexSubImage(target, level, xo, yo, x, y, w, h);
	}
	void APIENTRY HookCopyImage(const GLenum target, const GLint level, const GLenum format, const GLint x, const GLint y, const GLsizei w,
		const GLsizei h, const GLint border)
	{
		BeforeOther();
		copyImageOriginal(target, level, format, x, y, w, h, border);
		if (fieldmirror::Active())
			fieldmirror::CopyTexImage(target, level, format, x, y, w, h, border);
	}
	void APIENTRY HookBlit(const GLint a, const GLint b, const GLint c, const GLint d, const GLint e, const GLint f, const GLint g,
		const GLint h, const GLbitfield mask, const GLenum filter)
	{
		BeforeOther();
		blitOriginal(a, b, c, d, e, f, g, h, mask, filter);
		if (fieldmirror::Active())
			fieldmirror::Blit(a, b, c, d, e, f, g, h, mask, filter);
	}
	void APIENTRY HookBlitExt(const GLint a, const GLint b, const GLint c, const GLint d, const GLint e, const GLint f, const GLint g,
		const GLint h, const GLbitfield mask, const GLenum filter)
	{
		BeforeOther();
		blitExtOriginal(a, b, c, d, e, f, g, h, mask, filter);
		if (fieldmirror::Active())
			fieldmirror::Blit(a, b, c, d, e, f, g, h, mask, filter);
	}
	struct ArraysDraw
	{
		GLenum mode;
		GLint first;
		GLsizei count;
	};
	void APIENTRY HookDrawArrays(const GLenum mode, const GLint first, const GLsizei count)
	{
		ArraysDraw draw{ mode, first, count };
		TweenOtherDraw([](void* context)
		{
			const ArraysDraw& d = *static_cast<const ArraysDraw*>(context);
			drawArraysOriginal(d.mode, d.first, d.count);
		}, &draw);
	}
	void APIENTRY HookBegin(const GLenum mode)
	{
		TweenBeginDraw();
		beginOriginal(mode);
	}

	template <typename T>
	bool HookExport(const char* name, T detour, T& original)
	{
		LPVOID trampoline = nullptr;
		if (MH_CreateHookApi(L"OPENGL32", name, reinterpret_cast<LPVOID>(detour), &trampoline) != MH_OK || trampoline == nullptr)
			return false;
		original = reinterpret_cast<T>(trampoline);
		return true;
	}

	bool HookDriver(const char* name, BlitFunction detour, BlitFunction& original, const PROC other)
	{
		const PROC address = wglGetProcAddress(name);
		const auto value = reinterpret_cast<intptr_t>(address);
		if (address == nullptr || value == 1 || value == 2 || value == 3 || value == -1 || address == other)
			return false;
		LPVOID trampoline = nullptr;
		if (MH_CreateHook(reinterpret_cast<LPVOID>(address), reinterpret_cast<LPVOID>(detour), &trampoline) != MH_OK || trampoline == nullptr)
			return false;
		original = reinterpret_cast<BlitFunction>(trampoline);
		return true;
	}

	void WaitUntil(const long long ticks)
	{
		for (int i = 0; i < 64; i++)
		{
			const long long left = ticks - Ticks();
			if (left <= 0)
				return;
			if (left * 1000 > ticksPerSecond * 2)
				Sleep(1);
			else
				YieldProcessor();
		}
		while (Ticks() < ticks)
			YieldProcessor();
	}
}

bool fieldrt::TweenOn()
{
	return hooksMade && !off;
}

//opengl.cpp (ViewportBackdoorInject), before MH_EnableHook
void fieldrt::TweenCreateHooks()
{
	if (!FIELD_LAYERS || (!FIELD_SMOOTH_SCROLL && !FIELD_INTERPOLATION))
		return;
	bool ok = HookExport("glClear", &HookClear, clearOriginal);
	ok = HookExport("glCopyTexSubImage2D", &HookCopySub, copySubOriginal) && ok;
	ok = HookExport("glCopyTexImage2D", &HookCopyImage, copyImageOriginal) && ok;
	ok = HookDriver("glBlitFramebuffer", &HookBlit, blitOriginal, nullptr) && ok;
	//the same function under its older name, when the driver has it as another entry
	HookDriver("glBlitFramebufferEXT", &HookBlitExt, blitExtOriginal, wglGetProcAddress("glBlitFramebuffer"));
	if (!FIELD_FRAME_LOG)
	{
		//FIELD_FRAME_LOG has these two itself (frame_log.cpp), and passes them on
		HookExport("glDrawArrays", &HookDrawArrays, drawArraysOriginal);
		HookExport("glBegin", &HookBegin, beginOriginal);
	}
	if (!ok)
	{
		OutputDebug("FIELD_SMOOTH_SCROLL / FIELD_INTERPOLATION: not every OpenGL call they need could be hooked: both stay off\n");
		off = true;
		return;
	}
	hooksMade = true;
	LARGE_INTEGER frequency;
	QueryPerformanceFrequency(&frequency);
	ticksPerSecond = frequency.QuadPart;
	fieldmirror::Host host{};
	host.drawFramebuffer = HostDrawFramebuffer;
	host.readFramebuffer = HostReadFramebuffer;
	host.activeTexture = HostActiveTexture;
	host.texture2D = HostTexture;
	host.bindTexture2D = HostBindTexture;
	host.drawElements = HostDrawElements;
	host.clear = HostClear;
	host.blit = HostBlit;
	host.copyTexSubImage = HostCopySub;
	host.copyTexImage = HostCopyImage;
	host.texImage = HostTexImage;
	host.texParameter = HostTexParameter;
	host.log = HostLog;
	fieldmirror::Start(host);
	OutputDebug("FIELD_SMOOTH_SCROLL %d, FIELD_INTERPOLATION %d: on fields drawn from layer images\n", FIELD_SMOOTH_SCROLL ? 1 : 0,
		FIELD_INTERPOLATION ? 1 : 0);
}

void fieldrt::TweenPageBuilt(const GLuint target, const PageJob& job, const PageSize& size)
{
	pages.erase(target);
	if (!hooksMade || job.kind >= 0 || job.plan.stamps.empty())
		return;
	fieldtween::PageCells cells;
	cells.Size(size.logicalWidth, size.logicalHeight);
	for (const fieldlayers::CellStamp& stamp : job.plan.stamps)
	{
		cells.Add(stamp.cellX, stamp.cellY, stamp.layerId, stamp.layerX, stamp.layerY);
		Extent& e = extents[stamp.layerId];
		e.minX = std::min(e.minX, stamp.layerX);
		e.minY = std::min(e.minY, stamp.layerY);
		e.maxX = std::max(e.maxX, stamp.layerX);
		e.maxY = std::max(e.maxY, stamp.layerY);
	}
	pages[target] = std::move(cells);
}

void fieldrt::TweenPageForget(const GLuint target)
{
	if (!pages.empty())
		pages.erase(target);
}

void fieldrt::TweenForgetAll()
{
	pages.clear();
	tracks.clear();
	extents.clear();
	mainLayer = -1;
	fieldLive = false;
	batchesBefore.clear();
	batchesNow.clear();
	//the next field is measured afresh
	pacer.Reset();
	restInField = false;
	beginFrames = 0;
}

bool fieldrt::TweenBeforeDraw(const unsigned int mode, const int count, const unsigned int type, const void* indices)
{
	cur = Current();
	if (off || (pages.empty() && held.empty()))
		return false;
	try
	{
		const GLint program = glstate::Program();
		static const GameProgram none;
		const GameProgram& game = program != 0 ? GameProgramOf(program) : none;
		if (game.kind == ProgramKind::Character)
		{
			//a model while layers are kept back: from here on everything in the game's order again
			ReleaseAll();
			return false;
		}
		Classify(game, mode, count, type, indices);
		GLint charactersFramebuffer = 0;
		const bool waiting = CharsPending(charactersFramebuffer);
		if (!mainFixed && cur.layer >= 0 && cur.layer == mainLayer)
			FixMain(true);
		if (waiting && !mainFixed)
		{
			if (Hold(game, program, charactersFramebuffer, count, type, indices))
				return true;
			FixMain(false);
		}
		ReleaseAll();
	}
	catch (...)
	{
		TurnOff("looking at a draw");
		cur = Current();
	}
	return false;
}

void fieldrt::TweenDraw(const unsigned int mode, const int count, const unsigned int type, const void* indices)
{
	DrawNow(mode, count, type, indices);
}

void fieldrt::TweenOtherDraw(void (*draw)(void* context), void* context)
{
	BeforeOther();
	if (!fieldmirror::Active())
	{
		draw(context);
		return;
	}
	fieldmirror::BeforeDraw();
	draw(context);
	fieldmirror::Entered entered;
	if (fieldmirror::Enter(entered))
	{
		draw(context);
		fieldmirror::Leave(entered);
	}
}

void fieldrt::TweenBeginDraw()
{
	if (frameClosed)
		return; //DEMASTER's own (the day/night bar)
	BeforeOther();
	beginSeen = true;
}

bool fieldrt::TweenKeepsCharacters()
{
	//only when something would move them: the smooth scroll, or this frame's in-between picture
	return hooksMade && !off && fieldLive && (FIELD_SMOOTH_SCROLL || fieldmirror::Active());
}

bool fieldrt::TweenCharacterShift(double shift[2])
{
	shift[0] = shift[1] = 0;
	if (!hooksMade || off)
		return false;
	if (!mainFixed)
		FixMain(false);
	shift[0] = mainNow[0];
	shift[1] = mainNow[1];
	return mainTween;
}

void fieldrt::TweenSnap(double shift[2], const float projection[16], const int viewportWidth, const int viewportHeight)
{
	double perUnit[2];
	fieldtween::PixelsPerUnit(projection, viewportWidth, viewportHeight, perUnit);
	shift[0] = fieldtween::Snap(shift[0], perUnit[0]);
	shift[1] = fieldtween::Snap(shift[1], perUnit[1]);
}

const float* fieldrt::TweenCharacterPositions(const std::vector<float>& shown, const std::vector<fieldtween::CharDrawRef>& draws,
	const bool tween)
{
	if (!hooksMade || off || !FIELD_INTERPOLATION)
		return nullptr;
	const size_t batch = batchIndex++;
	if (batch >= 16)
		return nullptr;
	if (batchesNow.size() <= batch)
		batchesNow.resize(batch + 1);
	fieldtween::CharFrame& now = batchesNow[batch];
	now.shown = shown;
	now.draws = draws;
	now.frame = frameNumber;
	if (!fieldmirror::Active())
		return nullptr;
	static std::vector<float> between;
	if (tween && batch < batchesBefore.size())
		fieldtween::TweenCharacters(batchesBefore[batch], now, between);
	else
		between = shown;
	return between.data();
}

GLuint fieldrt::TweenFramebufferCopy(const GLuint gameFramebuffer)
{
	return fieldmirror::Active() ? fieldmirror::CopyOf(gameFramebuffer) : 0;
}

//SwapBuffers, before anything else of DEMASTER's: nothing stays kept back
void TweenEndOfDraws()
{
	if (hooksMade)
		ReleaseAll();
	frameClosed = true;
}

//SwapBuffers, when everything is on the screen. true: the in-between picture is on the screen instead and the game's is put
//aside: show it (the real SwapBuffers), call TweenShowFrame, then show the game's.
bool TweenShowBetween()
{
	doubled = false;
	if (!hooksMade || !fieldmirror::Active())
		return false;
	bool shown = false;
	try
	{
		if (!off && pagesDrawn && !beginSeen)
			shown = fieldmirror::ShowBetween();
		fieldmirror::EndFrame();
	}
	catch (...)
	{
		TurnOff("showing the in-between picture");
	}
	doubled = shown;
	return shown;
}

//the in-between picture is on the screen now: half a frame later the game's own goes back on
void TweenShowFrame()
{
	const double frame = pacer.Plain();
	if (frame > 0 && ticksPerSecond > 0)
	{
		//with vertical sync the next SwapBuffers waits for the screen anyway: only most of the way there
		const double wait = std::min(0.03, frame * 0.5 - (vsync ? 0.004 : 0.0));
		if (wait > 0)
			WaitUntil(Ticks() + static_cast<long long>(wait * ticksPerSecond));
	}
	fieldmirror::ShowFrame();
}

//SwapBuffers, after the game's picture was shown: the frame is over. hdc: the window's.
void TweenFrameEnd(void* hdc)
{
	frameClosed = false;
	if (!hooksMade)
		return;
	const long long now = Ticks();
	if (lastFrameEnd != 0 && ticksPerSecond > 0)
		pacer.Frame(static_cast<double>(now - lastFrameEnd) / ticksPerSecond, doubled);
	lastFrameEnd = now;
	if (pagesDrawn)
	{
		int most = 0;
		for (const auto& [layer, track] : tracks)
			if (track.frame == frameNumber && track.triangles > most)
			{
				most = track.triangles;
				mainLayer = layer;
			}
	}
	fieldLive = pagesDrawn && mainLayer >= 0;
	batchesBefore.swap(batchesNow);
	batchesBefore.resize(batchIndex < batchesBefore.size() ? batchIndex : batchesBefore.size());
	batchesNow.clear();
	batchIndex = 0;
	//a field that draws with glBegin every frame can't have in-between pictures
	beginFrames = beginSeen && pagesDrawn ? beginFrames + 1 : 0;
	if (beginFrames == 60)
		LogOnce("tween-begin-" + PbrView().name, "FIELD_INTERPOLATION: something here is drawn with glBegin, which can't be drawn twice: "
			"no in-between pictures while it is on the screen\n");
	frameNumber++;
	frameClosed = false;
	mainFixed = false;
	pagesDrawn = false;
	beginSeen = false;
	cur = Current();
	//the next frame
	bool mirror = FIELD_INTERPOLATION && !off && !sessionOff && !restInField && fieldLive && !fieldmirror::Broken();
	if (mirror && !glstate::On())
	{
		LogOnce("tween-no-track", "FIELD_INTERPOLATION needs FIELD_GL_TRACK = 1 (and it to be working): no in-between pictures\n");
		mirror = false;
	}
	if (mirror && pacer.TooSlow())
	{
		slowStrikes++;
		restInField = true;
		mirror = false;
		Log("FIELD_INTERPOLATION: with in-between pictures the game slowed down here (%.1f pictures of its own a second, %.1f without): "
			"off until the next field%s\n", 1.0 / std::max(1e-6, pacer.Interval()), 1.0 / std::max(1e-6, pacer.Plain()),
			slowStrikes >= 3 ? ", and after three fields like this, for the rest of this session" : "");
		if (slowStrikes >= 3)
			sessionOff = true;
	}
	if (mirror && !pacer.HalfRate())
	{
		if (pacer.PlainFrames() >= 30)
			LogOnce("tween-rate-" + PbrView().name, "FIELD_INTERPOLATION: the game shows %.1f pictures a second here, not about 30: no "
				"in-between pictures\n", 1.0 / std::max(1e-6, pacer.Plain()));
		mirror = false;
	}
	if (mirror)
	{
		if (frameNumber - vsyncChecked >= 120)
		{
			vsyncChecked = frameNumber;
			typedef int (WINAPI* GetSwapInterval)();
			static const GetSwapInterval get = reinterpret_cast<GetSwapInterval>(wglGetProcAddress("wglGetSwapIntervalEXT"));
			const auto value = reinterpret_cast<intptr_t>(get);
			vsync = get != nullptr && value != 1 && value != 2 && value != 3 && value != -1 && get() > 0;
		}
		RECT client{};
		const HWND window = WindowFromDC(static_cast<HDC>(hdc));
		if (window == nullptr || !GetClientRect(window, &client) || client.right <= 0 || client.bottom <= 0)
			mirror = false;
		else
		{
			try
			{
				fieldmirror::BeginFrame(client.right, client.bottom, mirroredLast);
				mirror = fieldmirror::Active();
				if (mirror && !mirroredLast)
					LogOnce("tween-on-" + PbrView().name, "FIELD_INTERPOLATION: in-between pictures here (the game shows %.1f pictures a second, "
						"%s vertical sync)\n", 1.0 / std::max(1e-6, pacer.Plain()), vsync ? "with" : "without");
			}
			catch (...)
			{
				TurnOff("starting an in-between picture");
				mirror = false;
			}
		}
	}
	mirroredLast = mirror;
}
