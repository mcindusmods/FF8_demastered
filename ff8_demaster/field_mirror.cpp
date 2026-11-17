//FIELD_INTERPOLATION: the second picture (see field_mirror.h).
#include "field_mirror.h"

#ifdef _WIN32
#include <GL/glew.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <cstdint>
#include <cstdio>
#include <unordered_map>

namespace fieldmirror
{
	namespace
	{
		Host host{};
		bool started = false, active = false, broken = false;
		int screenW = 0, screenH = 0;
		uint32_t frame = 0; //mirrored frames so far
		uint32_t generation = 0; //goes up when every copy is thrown away
		uint32_t spoiled = 0; //the frame that happened in: its second picture is not whole
		uint32_t run = 1; //goes up when mirrored frames stop following each other: the copies are filled again from the game's

		struct Attached
		{
			GLint type = GL_NONE; //GL_NONE, GL_TEXTURE, GL_RENDERBUFFER
			GLuint name = 0;
			bool operator==(const Attached& o) const { return type == o.type && name == o.name; }
		};
		constexpr GLenum POINTS[3] = { GL_COLOR_ATTACHMENT0, GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT };
		constexpr GLbitfield BITS[3] = { GL_COLOR_BUFFER_BIT, GL_DEPTH_BUFFER_BIT, GL_STENCIL_BUFFER_BIT };
		constexpr uint32_t CHECK_EVERY = 32; //frames between looks at whether a framebuffer of the game is still what was copied

		struct FramebufferCopy
		{
			GLuint framebuffer = 0;
			Attached game[3]; //what the game has attached (colour 0, depth, stencil)
			int width = 0, height = 0;
			uint32_t checked = 0, used = 0, filled = 0; //frame, frame, run
		};
		std::unordered_map<GLuint, FramebufferCopy> framebuffers; //by the game's framebuffer (0: the screen)

		struct TextureCopy
		{
			GLuint copy = 0;
			int width = 0, height = 0;
			GLint format = 0, minFilter = 0, magFilter = 0;
		};
		std::unordered_map<GLuint, TextureCopy> textures; //by the game's texture
		struct BufferCopy
		{
			GLuint copy = 0;
			int width = 0, height = 0;
			GLint format = 0, samples = 0;
		};
		std::unordered_map<GLuint, BufferCopy> renderbuffers; //by the game's renderbuffer
		GLuint screenTexture = 0, screenDepth = 0;
		//the game's own picture while the in-between one is on the screen
		GLuint asideFramebuffer = 0, asideTexture = 0;
		int asideW = 0, asideH = 0;
		bool asideHeld = false;

		void Fail(const char* what)
		{
			if (!broken && host.log != nullptr)
			{
				char line[300];
				snprintf(line, sizeof(line), "FIELD_INTERPOLATION: %s: no in-between pictures for the rest of this session\n", what);
				host.log(line);
			}
			broken = true;
			active = false;
		}

		void ClearErrors()
		{
			for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; i++) {}
		}

		int ActiveUnit()
		{
			return static_cast<int>(host.activeTexture()) - GL_TEXTURE0;
		}

		//scissoring cuts blits too: off around DEMASTER's own
		struct NoScissor
		{
			GLboolean was = glIsEnabled(GL_SCISSOR_TEST);
			NoScissor() { if (was) glDisable(GL_SCISSOR_TEST); }
			~NoScissor() { if (was) glEnable(GL_SCISSOR_TEST); }
		};

		struct TextureFacts
		{
			GLint width = 0, height = 0, format = 0, minFilter = GL_NEAREST, magFilter = GL_NEAREST, wrapS = GL_CLAMP_TO_EDGE,
				wrapT = GL_CLAMP_TO_EDGE;
		};
		//the bound 2D texture of the active unit
		TextureFacts BoundTextureFacts()
		{
			TextureFacts f;
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &f.width);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &f.height);
			glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &f.format);
			glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &f.minFilter);
			glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &f.magFilter);
			glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &f.wrapS);
			glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &f.wrapT);
			return f;
		}

		bool IsDepthFormat(const GLint format, GLenum& pixelFormat, GLenum& pixelType)
		{
			switch (format)
			{
			case GL_DEPTH_COMPONENT: case GL_DEPTH_COMPONENT16: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT32:
				pixelFormat = GL_DEPTH_COMPONENT; pixelType = GL_UNSIGNED_INT; return true;
			case GL_DEPTH_COMPONENT32F:
				pixelFormat = GL_DEPTH_COMPONENT; pixelType = GL_FLOAT; return true;
			case GL_DEPTH_STENCIL: case GL_DEPTH24_STENCIL8:
				pixelFormat = GL_DEPTH_STENCIL; pixelType = GL_UNSIGNED_INT_24_8; return true;
			case GL_DEPTH32F_STENCIL8:
				pixelFormat = GL_DEPTH_STENCIL; pixelType = GL_FLOAT_32_UNSIGNED_INT_24_8_REV; return true;
			default:
				pixelFormat = GL_RGBA; pixelType = GL_UNSIGNED_BYTE; return false;
			}
		}

		//a texture like the game's one (made or found). The game's texture is looked at through the active unit, which gets
		//its own texture back.
		TextureCopy* TextureCopyOf(const GLuint texture, const bool make)
		{
			if (const auto it = textures.find(texture); it != textures.end())
				return &it->second;
			if (!make || texture == 0)
				return nullptr;
			const GLuint bound = host.texture2D(ActiveUnit());
			ClearErrors();
			host.bindTexture2D(texture);
			const TextureFacts facts = BoundTextureFacts();
			TextureCopy copy;
			if (glGetError() == GL_NO_ERROR && facts.width > 0 && facts.height > 0)
			{
				copy.width = facts.width;
				copy.height = facts.height;
				copy.format = facts.format;
				copy.minFilter = facts.minFilter;
				copy.magFilter = facts.magFilter;
				glGenTextures(1, &copy.copy);
				host.bindTexture2D(copy.copy);
				GLenum pixelFormat = GL_RGBA, pixelType = GL_UNSIGNED_BYTE;
				IsDepthFormat(facts.format, pixelFormat, pixelType);
				host.texImage(facts.format, facts.width, facts.height, pixelFormat, pixelType);
				//no mip levels below the first (a filter of the game's that wants them reads the first one)
				host.texParameter(GL_TEXTURE_MAX_LEVEL, 0);
				host.texParameter(GL_TEXTURE_MIN_FILTER, facts.minFilter == GL_NEAREST || facts.minFilter == GL_NEAREST_MIPMAP_NEAREST
					|| facts.minFilter == GL_NEAREST_MIPMAP_LINEAR ? GL_NEAREST : GL_LINEAR);
				host.texParameter(GL_TEXTURE_MAG_FILTER, facts.magFilter);
				host.texParameter(GL_TEXTURE_WRAP_S, facts.wrapS);
				host.texParameter(GL_TEXTURE_WRAP_T, facts.wrapT);
				if (glGetError() != GL_NO_ERROR)
				{
					glDeleteTextures(1, &copy.copy);
					copy.copy = 0;
				}
			}
			host.bindTexture2D(bound);
			if (copy.copy == 0)
			{
				Fail("a texture of the game's framebuffers can't be copied");
				return nullptr;
			}
			return &(textures[texture] = copy);
		}

		BufferCopy* BufferCopyOf(const GLuint renderbuffer)
		{
			if (const auto it = renderbuffers.find(renderbuffer); it != renderbuffers.end())
				return &it->second;
			GLint bound = 0;
			glGetIntegerv(GL_RENDERBUFFER_BINDING, &bound);
			ClearErrors();
			BufferCopy copy;
			glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
			glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &copy.width);
			glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &copy.height);
			glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_INTERNAL_FORMAT, &copy.format);
			glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &copy.samples);
			if (glGetError() == GL_NO_ERROR && copy.width > 0 && copy.height > 0)
			{
				glGenRenderbuffers(1, &copy.copy);
				glBindRenderbuffer(GL_RENDERBUFFER, copy.copy);
				if (copy.samples > 0)
					glRenderbufferStorageMultisample(GL_RENDERBUFFER, copy.samples, static_cast<GLenum>(copy.format), copy.width, copy.height);
				else
					glRenderbufferStorage(GL_RENDERBUFFER, static_cast<GLenum>(copy.format), copy.width, copy.height);
				if (glGetError() != GL_NO_ERROR)
				{
					glDeleteRenderbuffers(1, &copy.copy);
					copy.copy = 0;
				}
			}
			glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(bound));
			if (copy.copy == 0)
			{
				Fail("a renderbuffer of the game's framebuffers can't be copied");
				return nullptr;
			}
			return &(renderbuffers[renderbuffer] = copy);
		}

		//the framebuffer bound for drawing: what is attached
		void ReadAttached(Attached out[3], bool& more)
		{
			for (int i = 0; i < 3; i++)
			{
				out[i] = Attached();
				glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, POINTS[i], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &out[i].type);
				if (out[i].type != GL_NONE)
				{
					GLint name = 0;
					glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, POINTS[i], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
					out[i].name = static_cast<GLuint>(name);
				}
			}
			GLint second = GL_NONE;
			glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &second);
			more = second != GL_NONE;
		}

		//game framebuffer -> copy, everything it holds (the copy starts as the same picture)
		void Fill(const GLuint game, FramebufferCopy& copy)
		{
			copy.filled = run;
			const GLuint read = host.readFramebuffer(), draw = host.drawFramebuffer();
			GLbitfield mask = 0;
			if (game == 0)
				mask = GL_COLOR_BUFFER_BIT;
			else
				for (int i = 0; i < 3; i++)
					if (copy.game[i].type != GL_NONE)
						mask |= BITS[i];
			NoScissor noScissor;
			glBindFramebuffer(GL_READ_FRAMEBUFFER, game);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, copy.framebuffer);
			host.blit(0, 0, copy.width, copy.height, 0, 0, copy.width, copy.height, mask, GL_NEAREST);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
			ClearErrors(); //a framebuffer that can't be read (nothing drawn into it yet) just stays empty
		}

		bool Build(const GLuint game, FramebufferCopy& copy)
		{
			const GLuint draw = host.drawFramebuffer();
			ClearErrors();
			bool ok = true;
			if (game == 0)
			{
				copy.width = screenW;
				copy.height = screenH;
				const GLuint bound = host.texture2D(ActiveUnit());
				GLint boundBuffer = 0;
				glGetIntegerv(GL_RENDERBUFFER_BINDING, &boundBuffer);
				glGenTextures(1, &screenTexture);
				host.bindTexture2D(screenTexture);
				host.texImage(GL_RGBA8, screenW, screenH, GL_RGBA, GL_UNSIGNED_BYTE);
				host.texParameter(GL_TEXTURE_MAX_LEVEL, 0);
				host.texParameter(GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				host.texParameter(GL_TEXTURE_MAG_FILTER, GL_NEAREST);
				host.bindTexture2D(bound);
				glGenRenderbuffers(1, &screenDepth);
				glBindRenderbuffer(GL_RENDERBUFFER, screenDepth);
				glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, screenW, screenH);
				glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(boundBuffer));
				glGenFramebuffers(1, &copy.framebuffer);
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, copy.framebuffer);
				glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, screenTexture, 0);
				glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, screenDepth);
			}
			else
			{
				bool more = false;
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, game);
				ReadAttached(copy.game, more);
				if (glGetError() != GL_NO_ERROR || more)
					ok = false;
				GLuint parts[3] = {};
				for (int i = 0; i < 3 && ok; i++)
				{
					if (copy.game[i].type == GL_TEXTURE)
					{
						const TextureCopy* t = TextureCopyOf(copy.game[i].name, true);
						ok = t != nullptr;
						if (ok)
						{
							parts[i] = t->copy;
							copy.width = t->width;
							copy.height = t->height;
						}
					}
					else if (copy.game[i].type == GL_RENDERBUFFER)
					{
						const BufferCopy* b = BufferCopyOf(copy.game[i].name);
						ok = b != nullptr;
						if (ok)
						{
							parts[i] = b->copy;
							copy.width = b->width;
							copy.height = b->height;
						}
					}
					else if (copy.game[i].type != GL_NONE)
						ok = false;
				}
				if (ok)
				{
					glGenFramebuffers(1, &copy.framebuffer);
					glBindFramebuffer(GL_DRAW_FRAMEBUFFER, copy.framebuffer);
					for (int i = 0; i < 3; i++)
						if (copy.game[i].type == GL_TEXTURE)
							glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, POINTS[i], GL_TEXTURE_2D, parts[i], 0);
						else if (copy.game[i].type == GL_RENDERBUFFER)
							glFramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, POINTS[i], GL_RENDERBUFFER, parts[i]);
					if (copy.game[0].type == GL_NONE)
					{
						glDrawBuffer(GL_NONE);
						glReadBuffer(GL_NONE);
					}
				}
			}
			ok = ok && copy.width > 0 && copy.height > 0 && glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE
				&& glGetError() == GL_NO_ERROR;
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
			if (!ok)
			{
				Fail(game == 0 ? "a copy of the screen can't be made" : "one of the game's framebuffers can't be copied");
				return false;
			}
			copy.checked = frame;
			return true;
		}

		//the game's framebuffer and its textures are still what was copied (the game makes them again when the window changes)
		bool StillSame(const GLuint game, const FramebufferCopy& copy)
		{
			if (game == 0)
				return true;
			const GLuint draw = host.drawFramebuffer();
			Attached now[3];
			bool more = false, same = true;
			ClearErrors();
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, game);
			ReadAttached(now, more);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
			if (glGetError() != GL_NO_ERROR || more)
				return false;
			const GLuint bound = host.texture2D(ActiveUnit());
			for (int i = 0; i < 3 && same; i++)
			{
				same = now[i] == copy.game[i];
				if (same && now[i].type == GL_TEXTURE)
				{
					const auto t = textures.find(now[i].name);
					host.bindTexture2D(now[i].name);
					const TextureFacts facts = BoundTextureFacts();
					same = t != textures.end() && facts.width == t->second.width && facts.height == t->second.height
						&& facts.format == t->second.format;
					if (same && (facts.minFilter != t->second.minFilter || facts.magFilter != t->second.magFilter))
						same = false; //the game filters it another way now: made again with that
				}
			}
			host.bindTexture2D(bound);
			return same;
		}

		//The copy of one of the game's framebuffers, ready to be drawn into this frame. make false: only when it exists
		//already (a clear of a framebuffer the game never drew into is not the game's frame).
		FramebufferCopy* Ready(const GLuint game, const bool make)
		{
			if (!active)
				return nullptr;
			auto it = framebuffers.find(game);
			if (it != framebuffers.end() && it->second.used != frame && frame - it->second.checked >= CHECK_EVERY)
			{
				it->second.checked = frame;
				if (!StillSame(game, it->second))
				{
					Forget();
					it = framebuffers.end();
				}
			}
			if (it == framebuffers.end())
			{
				if (!make)
					return nullptr;
				FramebufferCopy copy;
				if (!Build(game, copy))
					return nullptr;
				it = framebuffers.emplace(game, copy).first;
			}
			FramebufferCopy& copy = it->second;
			if (copy.filled != run)
				Fill(game, copy);
			copy.used = frame;
			return &copy;
		}

		//the copies of the game's framebuffer textures on the units, for one draw
		void SwapIn(Entered& s)
		{
			s.count = 0;
			if (textures.empty())
				return;
			for (int unit = 0; unit < UNITS; unit++)
			{
				const GLuint bound = host.texture2D(unit);
				if (bound == 0)
					continue;
				const auto it = textures.find(bound);
				if (it == textures.end())
					continue;
				glActiveTexture(GL_TEXTURE0 + unit);
				host.bindTexture2D(it->second.copy);
				s.unit[s.count] = unit;
				s.texture[s.count++] = bound;
			}
		}
		void SwapOut(const Entered& s)
		{
			if (s.count == 0)
				return;
			for (int i = 0; i < s.count; i++)
			{
				glActiveTexture(GL_TEXTURE0 + s.unit[i]);
				host.bindTexture2D(s.texture[i]);
			}
			glActiveTexture(s.active);
		}
	}

	void Start(const Host& h)
	{
		host = h;
		started = host.drawFramebuffer != nullptr && host.readFramebuffer != nullptr && host.activeTexture != nullptr
			&& host.texture2D != nullptr && host.bindTexture2D != nullptr && host.drawElements != nullptr && host.clear != nullptr
			&& host.blit != nullptr && host.texParameter != nullptr && host.copyTexSubImage != nullptr && host.copyTexImage != nullptr && host.texImage != nullptr;
	}

	void BeginFrame(const int screenWidth, const int screenHeight, const bool follows)
	{
		if (!started || broken || screenWidth <= 0 || screenHeight <= 0)
		{
			active = false;
			return;
		}
		if (screenWidth != screenW || screenHeight != screenH)
		{
			Forget();
			screenW = screenWidth;
			screenH = screenHeight;
		}
		if (!follows)
			run++;
		frame++;
		active = true;
	}

	void EndFrame()
	{
		active = false;
	}

	bool Active()
	{
		return active && !broken;
	}

	bool Broken()
	{
		return broken;
	}

	void BeforeDraw()
	{
		if (active)
			Ready(host.drawFramebuffer(), true);
	}

	void Draw(const unsigned int mode, const int count, const unsigned int type, const void* indices)
	{
		if (!active)
			return;
		Entered entered;
		if (!Enter(entered))
			return;
		host.drawElements(mode, count, type, indices);
		Leave(entered);
	}

	bool Enter(Entered& entered)
	{
		if (!active)
			return false;
		entered.game = host.drawFramebuffer();
		const FramebufferCopy* copy = Ready(entered.game, true);
		if (copy == nullptr)
			return false;
		entered.active = host.activeTexture();
		SwapIn(entered);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, copy->framebuffer);
		return true;
	}

	void Leave(const Entered& entered)
	{
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, entered.game);
		SwapOut(entered);
	}

	void Clear(const unsigned int mask)
	{
		if (!active)
			return;
		const GLuint game = host.drawFramebuffer();
		const FramebufferCopy* copy = Ready(game, false);
		if (copy == nullptr)
			return;
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, copy->framebuffer);
		host.clear(mask);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, game);
	}

	void Blit(const int srcX0, const int srcY0, const int srcX1, const int srcY1, const int dstX0, const int dstY0, const int dstX1,
		const int dstY1, const unsigned int mask, const unsigned int filter)
	{
		if (!active)
			return;
		const GLuint read = host.readFramebuffer(), draw = host.drawFramebuffer();
		if (read == draw)
			return;
		//from a framebuffer that has no copy (not part of this frame's drawing so far) into one that has: from the game's own
		const uint32_t before = generation;
		const FramebufferCopy* from = Ready(read, false);
		const FramebufferCopy* to = Ready(draw, from != nullptr);
		if (generation != before)
		{
			//the copies were made again on the way: the first answer is gone
			from = Ready(read, false);
			to = Ready(draw, from != nullptr);
		}
		if (to == nullptr)
			return;
		glBindFramebuffer(GL_READ_FRAMEBUFFER, from != nullptr ? from->framebuffer : read);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to->framebuffer);
		host.blit(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
	}

	namespace
	{
		//a copy out of a framebuffer into the 2D texture on the active unit: when that framebuffer has a copy, the texture
		//gets one too, filled from it
		template <typename Call>
		void CopyIntoTexture(const unsigned int target, Call call)
		{
			if (!active || target != GL_TEXTURE_2D)
				return;
			const GLuint read = host.readFramebuffer();
			const FramebufferCopy* from = Ready(read, false);
			if (from == nullptr)
				return;
			const GLuint texture = host.texture2D(ActiveUnit());
			const TextureCopy* copy = TextureCopyOf(texture, true);
			if (copy == nullptr)
				return;
			host.bindTexture2D(copy->copy);
			glBindFramebuffer(GL_READ_FRAMEBUFFER, from->framebuffer);
			call();
			glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
			host.bindTexture2D(texture);
		}
	}

	void CopyTexSubImage(const unsigned int target, const int level, const int xOffset, const int yOffset, const int x, const int y,
		const int width, const int height)
	{
		//a copy that is smaller than what goes into it now: the game's texture was made again in another size
		if (active && target == GL_TEXTURE_2D && level == 0)
			if (const auto it = textures.find(host.texture2D(ActiveUnit())); it != textures.end()
				&& (xOffset + width > it->second.width || yOffset + height > it->second.height))
			{
				Forget();
				return;
			}
		CopyIntoTexture(target, [&]() { host.copyTexSubImage(target, level, xOffset, yOffset, x, y, width, height); });
	}

	void CopyTexImage(const unsigned int target, const int level, const unsigned int internalFormat, const int x, const int y,
		const int width, const int height, const int border)
	{
		//the game's texture has its new size already (the game's call came first): a copy made before that is the wrong size
		if (active && target == GL_TEXTURE_2D && level == 0)
			if (const auto it = textures.find(host.texture2D(ActiveUnit())); it != textures.end()
				&& (it->second.width != width || it->second.height != height))
			{
				Forget();
				return; //this frame's second picture misses it; the next one starts from the game's
			}
		CopyIntoTexture(target, [&]() { host.copyTexImage(target, level, internalFormat, x, y, width, height, border); });
	}

	Name CopyOf(const Name gameFramebuffer)
	{
		const FramebufferCopy* copy = Ready(gameFramebuffer, true);
		return copy != nullptr ? copy->framebuffer : 0;
	}

	Name CopyOfTexture(const Name gameTexture)
	{
		const auto it = textures.find(gameTexture);
		return it != textures.end() ? it->second.copy : 0;
	}

	bool ShowBetween()
	{
		asideHeld = false;
		if (broken || !started)
			return false;
		const auto screen = framebuffers.find(0);
		if (screen == framebuffers.end() || screen->second.used != frame || screen->second.filled != run || spoiled == frame)
			return false;
		const GLuint read = host.readFramebuffer(), draw = host.drawFramebuffer();
		ClearErrors();
		if (asideFramebuffer == 0 || asideW != screenW || asideH != screenH)
		{
			if (asideFramebuffer != 0)
			{
				glDeleteFramebuffers(1, &asideFramebuffer);
				glDeleteTextures(1, &asideTexture);
			}
			asideW = screenW;
			asideH = screenH;
			const GLuint bound = host.texture2D(ActiveUnit());
			glGenTextures(1, &asideTexture);
			host.bindTexture2D(asideTexture);
			host.texImage(GL_RGBA8, asideW, asideH, GL_RGBA, GL_UNSIGNED_BYTE);
			host.texParameter(GL_TEXTURE_MAX_LEVEL, 0);
			host.texParameter(GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			host.texParameter(GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			host.bindTexture2D(bound);
			glGenFramebuffers(1, &asideFramebuffer);
			glBindFramebuffer(GL_DRAW_FRAMEBUFFER, asideFramebuffer);
			glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, asideTexture, 0);
			if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE || glGetError() != GL_NO_ERROR)
			{
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
				Fail("the game's picture can't be put aside");
				return false;
			}
		}
		NoScissor noScissor;
		//the game's picture aside
		glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, asideFramebuffer);
		host.blit(0, 0, screenW, screenH, 0, 0, screenW, screenH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		//the in-between one on the screen
		glBindFramebuffer(GL_READ_FRAMEBUFFER, screen->second.framebuffer);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		host.blit(0, 0, screenW, screenH, 0, 0, screenW, screenH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
		if (glGetError() != GL_NO_ERROR)
		{
			Fail("the in-between picture can't be put on the screen");
			return false;
		}
		asideHeld = true;
		return true;
	}

	void ShowFrame()
	{
		if (!asideHeld)
			return;
		asideHeld = false;
		const GLuint read = host.readFramebuffer(), draw = host.drawFramebuffer();
		NoScissor noScissor;
		glBindFramebuffer(GL_READ_FRAMEBUFFER, asideFramebuffer);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		host.blit(0, 0, screenW, screenH, 0, 0, screenW, screenH, GL_COLOR_BUFFER_BIT, GL_NEAREST);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, read);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw);
	}

	void Forget()
	{
		generation++;
		spoiled = frame;
		for (auto& [game, copy] : framebuffers)
			if (copy.framebuffer != 0)
				glDeleteFramebuffers(1, &copy.framebuffer);
		for (auto& [game, copy] : textures)
			glDeleteTextures(1, &copy.copy);
		for (auto& [game, copy] : renderbuffers)
			glDeleteRenderbuffers(1, &copy.copy);
		if (screenTexture != 0)
			glDeleteTextures(1, &screenTexture);
		if (screenDepth != 0)
			glDeleteRenderbuffers(1, &screenDepth);
		screenTexture = screenDepth = 0;
		framebuffers.clear();
		textures.clear();
		renderbuffers.clear();
	}
}
