//The GL state copy (gl_state.h): hooks on the calls that set state, the copy, and its self-check.

#include "coreHeader.h"
#include "gl_state.h"

#include "config.h"
#include "debug.h"
#include "opengl.h"
#include "minhook/include/MinHook.h"

#include <cmath>
#include <cstring>

using namespace glstate;

namespace
{
	constexpr int UNITS = 16, CLIENT_UNITS = 8, WATCHED = 64;

	struct Copy
	{
		bool known = false; //everything below (but the program) is what the driver has
		bool programKnown = false;
		GLint program = 0;
		GLint activeUnit = 0;
		GLuint tex2d[UNITS] = {}, tex3d[UNITS] = {};
		RenderState draw{};
		GLint clientUnit = 0;
		ArrayState vertex, color, normal, uv[CLIENT_UNITS];
		GLint arrayBuffer = 0, elementBuffer = 0, vao = 0;
		GLint matrixMode = GL_MODELVIEW;
		bool projectionKnown = false;
		float projection[16] = {};
		float clearColor[4] = {};
		double clearDepth = 1.0;
	};
	Copy c;
	bool installed = false, broken = false;
	DWORD renderThread = 0; //the thread the copy follows (calls from others are on their own contexts)
	uint64_t frames = 0;
	bool checkDue = false;

	//uniforms of the game's programs the field code reads
	struct Watched
	{
		GLint program = 0, location = -1;
		int kind = 0; //1 int, 2 vec3
		bool known = false;
		GLint i = 0;
		float f[3] = {};
	};
	Watched watched[WATCHED];
	int watchedCount = 0;

	//--- the driver's functions (trampolines once hooked)
	using Enable_t = void(APIENTRY*)(GLenum);
	using BlendFunc_t = void(APIENTRY*)(GLenum, GLenum);
	using DepthFunc_t = void(APIENTRY*)(GLenum);
	using DepthMask_t = void(APIENTRY*)(GLboolean);
	using AlphaFunc_t = void(APIENTRY*)(GLenum, GLclampf);
	using ColorMask_t = void(APIENTRY*)(GLboolean, GLboolean, GLboolean, GLboolean);
	using Pointer_t = void(APIENTRY*)(GLint, GLenum, GLsizei, const void*);
	using NormalPointer_t = void(APIENTRY*)(GLenum, GLsizei, const void*);
	using ClearColor_t = void(APIENTRY*)(GLclampf, GLclampf, GLclampf, GLclampf);
	using ClearDepth_t = void(APIENTRY*)(GLclampd);
	using Void_t = void(APIENTRY*)();
	using LoadMatrixf_t = void(APIENTRY*)(const GLfloat*);
	using LoadMatrixd_t = void(APIENTRY*)(const GLdouble*);
	using Ortho_t = void(APIENTRY*)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble);
	using Rotatef_t = void(APIENTRY*)(GLfloat, GLfloat, GLfloat, GLfloat);
	using Translatef_t = void(APIENTRY*)(GLfloat, GLfloat, GLfloat);
	using Bitfield_t = void(APIENTRY*)(GLbitfield);
	using DeleteTextures_t = void(APIENTRY*)(GLsizei, const GLuint*);
	using MakeCurrent_t = BOOL(WINAPI*)(HDC, HGLRC);
	using UseProgram_t = void(APIENTRY*)(GLuint);
	using BindBuffer_t = void(APIENTRY*)(GLenum, GLuint);
	using BindFramebuffer_t = void(APIENTRY*)(GLenum, GLuint);
	using BlendFuncSeparate_t = void(APIENTRY*)(GLenum, GLenum, GLenum, GLenum);
	using BlendEquationSeparate_t = void(APIENTRY*)(GLenum, GLenum);
	using Uniform1i_t = void(APIENTRY*)(GLint, GLint);
	using Uniform1iv_t = void(APIENTRY*)(GLint, GLsizei, const GLint*);
	using Uniform3f_t = void(APIENTRY*)(GLint, GLfloat, GLfloat, GLfloat);
	using Uniform3fv_t = void(APIENTRY*)(GLint, GLsizei, const GLfloat*);
	using BindVertexArray_t = void(APIENTRY*)(GLuint);
	using LinkProgram_t = void(APIENTRY*)(GLuint);

	Enable_t o_glEnable, o_glDisable, o_glEnableClientState, o_glDisableClientState, o_glDepthFunc, o_glCullFace, o_glFrontFace,
		o_glMatrixMode, o_glActiveTexture, o_glClientActiveTexture, o_glBlendEquation;
	//The same calls under their older names. The game uses both glActiveTexture and glActiveTextureARB, only
	//glBindFramebufferEXT (never glBindFramebuffer), and glBlendEquationEXT / glBlendEquationSeparateEXT next to
	//glBlendEquation. A driver may give each name its own entry point, and a hook on one then never sees the other.
	Enable_t o_glActiveTextureARB, o_glBlendEquationEXT;
	BlendFunc_t o_glBlendFunc;
	DepthMask_t o_glDepthMask;
	AlphaFunc_t o_glAlphaFunc;
	ColorMask_t o_glColorMask;
	Pointer_t o_glColorPointer;
	NormalPointer_t o_glNormalPointer;
	ClearColor_t o_glClearColor;
	ClearDepth_t o_glClearDepth;
	Void_t o_glLoadIdentity, o_glPushMatrix, o_glPopMatrix, o_glPopAttrib, o_glPopClientAttrib;
	Bitfield_t o_glPushAttrib, o_glPushClientAttrib;
	LoadMatrixf_t o_glLoadMatrixf, o_glMultMatrixf;
	LoadMatrixd_t o_glLoadMatrixd, o_glMultMatrixd;
	Ortho_t o_glOrtho, o_glFrustum;
	Rotatef_t o_glRotatef;
	Translatef_t o_glTranslatef, o_glScalef;
	DeleteTextures_t o_glDeleteTextures;
	MakeCurrent_t o_wglMakeCurrent;
	UseProgram_t o_glUseProgram;
	BindBuffer_t o_glBindBuffer;
	BindFramebuffer_t o_glBindFramebuffer, o_glBindFramebufferEXT;
	BlendFuncSeparate_t o_glBlendFuncSeparate;
	BlendEquationSeparate_t o_glBlendEquationSeparate, o_glBlendEquationSeparateEXT;
	Uniform1i_t o_glUniform1i;
	Uniform1iv_t o_glUniform1iv;
	Uniform3f_t o_glUniform3f;
	Uniform3fv_t o_glUniform3fv;
	BindVertexArray_t o_glBindVertexArray;
	LinkProgram_t o_glLinkProgram;

	bool Tracking()
	{
		return installed && !broken;
	}

	void Lose()
	{
		c.known = false;
	}

	//a hook may update the copy: the copy is known and the call is on the render thread
	bool Mine()
	{
		return GetCurrentThreadId() == renderThread;
	}
	bool K()
	{
		return c.known && Mine();
	}

	//--- reading the driver (the copy's source, and the fallback)

	void QueryDraw(RenderState& s)
	{
		memset(&s, 0, sizeof(s));
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s.drawFramebuffer);
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s.readFramebuffer);
		glGetIntegerv(GL_VIEWPORT, s.viewport);
		s.blend = glIsEnabled(GL_BLEND);
		glGetIntegerv(GL_BLEND_SRC_RGB, &s.blendSrcRgb);
		glGetIntegerv(GL_BLEND_DST_RGB, &s.blendDstRgb);
		glGetIntegerv(GL_BLEND_SRC_ALPHA, &s.blendSrcAlpha);
		glGetIntegerv(GL_BLEND_DST_ALPHA, &s.blendDstAlpha);
		glGetIntegerv(GL_BLEND_EQUATION_RGB, &s.blendEqRgb);
		glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s.blendEqAlpha);
		s.depthTest = glIsEnabled(GL_DEPTH_TEST);
		glGetIntegerv(GL_DEPTH_FUNC, &s.depthFunc);
		glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
		s.alphaTest = glIsEnabled(GL_ALPHA_TEST);
		glGetIntegerv(GL_ALPHA_TEST_FUNC, &s.alphaFunc);
		glGetFloatv(GL_ALPHA_TEST_REF, &s.alphaRef);
		s.scissor = glIsEnabled(GL_SCISSOR_TEST);
		glGetIntegerv(GL_SCISSOR_BOX, s.scissorBox);
		glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
		s.cull = glIsEnabled(GL_CULL_FACE);
		glGetIntegerv(GL_CULL_FACE_MODE, &s.cullMode);
		glGetIntegerv(GL_FRONT_FACE, &s.frontFace);
		s.stencil = glIsEnabled(GL_STENCIL_TEST);
	}

	void QueryArray(ArrayState& a, const GLenum which, const GLenum size, const GLenum type, const GLenum stride, const GLenum pointer,
		const GLenum binding)
	{
		a.on = glIsEnabled(which);
		a.size = size == 0 ? 3 : 4;
		if (size != 0)
			glGetIntegerv(size, &a.size);
		glGetIntegerv(type, &a.type);
		glGetIntegerv(stride, &a.stride);
		glGetIntegerv(binding, &a.buffer);
		glGetPointerv(pointer, &a.pointer);
	}

	void QueryTexCoord(ArrayState& a)
	{
		QueryArray(a, GL_TEXTURE_COORD_ARRAY, GL_TEXTURE_COORD_ARRAY_SIZE, GL_TEXTURE_COORD_ARRAY_TYPE, GL_TEXTURE_COORD_ARRAY_STRIDE,
			GL_TEXTURE_COORD_ARRAY_POINTER, GL_TEXTURE_COORD_ARRAY_BUFFER_BINDING);
	}

	void QueryClient(ClientState& s)
	{
		glGetIntegerv(GL_CLIENT_ACTIVE_TEXTURE, &s.clientUnit);
		glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s.arrayBuffer);
		glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &s.elementBuffer);
		QueryArray(s.vertex, GL_VERTEX_ARRAY, GL_VERTEX_ARRAY_SIZE, GL_VERTEX_ARRAY_TYPE, GL_VERTEX_ARRAY_STRIDE, GL_VERTEX_ARRAY_POINTER,
			GL_VERTEX_ARRAY_BUFFER_BINDING);
		QueryArray(s.color, GL_COLOR_ARRAY, GL_COLOR_ARRAY_SIZE, GL_COLOR_ARRAY_TYPE, GL_COLOR_ARRAY_STRIDE, GL_COLOR_ARRAY_POINTER,
			GL_COLOR_ARRAY_BUFFER_BINDING);
		QueryArray(s.normal, GL_NORMAL_ARRAY, 0, GL_NORMAL_ARRAY_TYPE, GL_NORMAL_ARRAY_STRIDE, GL_NORMAL_ARRAY_POINTER,
			GL_NORMAL_ARRAY_BUFFER_BINDING);
		const Enable_t clientActive = o_glClientActiveTexture != nullptr ? o_glClientActiveTexture : glClientActiveTexture;
		clientActive(GL_TEXTURE0);
		QueryTexCoord(s.uv0);
		clientActive(GL_TEXTURE1);
		QueryTexCoord(s.uv1);
		clientActive(static_cast<GLenum>(s.clientUnit));
	}

	//every unit's 2D and 3D binding (back to the active unit after)
	void QueryTextures(GLint& activeUnit, GLuint* tex2d, GLuint* tex3d)
	{
		GLint active = GL_TEXTURE0;
		glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
		activeUnit = active - GL_TEXTURE0;
		const Enable_t select = o_glActiveTexture != nullptr ? o_glActiveTexture : glActiveTexture;
		for (int u = 0; u < UNITS; u++)
		{
			select(GL_TEXTURE0 + u);
			GLint t2 = 0, t3 = 0;
			glGetIntegerv(GL_TEXTURE_BINDING_2D, &t2);
			glGetIntegerv(GL_TEXTURE_BINDING_3D, &t3);
			tex2d[u] = static_cast<GLuint>(t2);
			tex3d[u] = static_cast<GLuint>(t3);
		}
		select(static_cast<GLenum>(active));
	}

	//the whole copy from the driver
	void Sync()
	{
		GLint program = 0;
		glGetIntegerv(GL_CURRENT_PROGRAM, &program);
		c.program = program;
		c.programKnown = true;
		QueryTextures(c.activeUnit, c.tex2d, c.tex3d);
		QueryDraw(c.draw);
		ClientState client;
		QueryClient(client);
		c.clientUnit = client.clientUnit - GL_TEXTURE0;
		c.arrayBuffer = client.arrayBuffer;
		c.elementBuffer = client.elementBuffer;
		c.vertex = client.vertex;
		c.color = client.color;
		c.normal = client.normal;
		for (ArrayState& a : c.uv)
			a = ArrayState();
		c.uv[0] = client.uv0;
		c.uv[1] = client.uv1;
		c.vao = 0;
		if (glBindVertexArray != nullptr)
			glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &c.vao);
		glGetIntegerv(GL_MATRIX_MODE, &c.matrixMode);
		glGetFloatv(GL_PROJECTION_MATRIX, c.projection);
		c.projectionKnown = true;
		glGetFloatv(GL_COLOR_CLEAR_VALUE, c.clearColor);
		glGetDoublev(GL_DEPTH_CLEAR_VALUE, &c.clearDepth);
		c.known = true;
	}

	//the copy is usable now (read again from the driver after it lost track: glPopAttrib, another context)
	bool Ready()
	{
		if (!Tracking() || !Mine())
			return false;
		if (!c.known || !c.programKnown)
			Sync();
		return true;
	}

	ArrayState* TexCoordArray()
	{
		return c.clientUnit >= 0 && c.clientUnit < CLIENT_UNITS ? &c.uv[c.clientUnit] : nullptr;
	}

	void SetEnabled(const GLenum cap, const GLboolean on)
	{
		if (!K())
			return;
		switch (cap)
		{
		case GL_BLEND: c.draw.blend = on; break;
		case GL_DEPTH_TEST: c.draw.depthTest = on; break;
		case GL_ALPHA_TEST: c.draw.alphaTest = on; break;
		case GL_SCISSOR_TEST: c.draw.scissor = on; break;
		case GL_CULL_FACE: c.draw.cull = on; break;
		case GL_STENCIL_TEST: c.draw.stencil = on; break;
		default: break;
		}
	}

	void SetClientEnabled(const GLenum cap, const GLboolean on)
	{
		if (!K() || c.vao != 0)
			return;
		switch (cap)
		{
		case GL_VERTEX_ARRAY: c.vertex.on = on; break;
		case GL_COLOR_ARRAY: c.color.on = on; break;
		case GL_NORMAL_ARRAY: c.normal.on = on; break;
		case GL_TEXTURE_COORD_ARRAY:
			if (ArrayState* a = TexCoordArray())
				a->on = on;
			break;
		default: break;
		}
	}

	void SetPointer(ArrayState& a, const GLint size, const GLenum type, const GLsizei stride, const void* pointer)
	{
		a.size = size;
		a.type = static_cast<GLint>(type);
		a.stride = stride;
		a.buffer = c.arrayBuffer;
		a.pointer = const_cast<void*>(pointer);
	}

	void ProjectionChanges()
	{
		if (Mine() && (c.matrixMode == GL_PROJECTION || !c.known))
			c.projectionKnown = false;
	}

	void SetUniform(const GLint location, const GLint* i, const GLfloat* f)
	{
		if (!c.programKnown || location < 0 || !Mine())
			return;
		for (int k = 0; k < watchedCount; k++)
		{
			Watched& w = watched[k];
			if (w.program != c.program || w.location != location)
				continue;
			if (i != nullptr)
				w.i = *i;
			if (f != nullptr)
				memcpy(w.f, f, sizeof(w.f));
			w.known = true;
		}
	}

	//--- the hooks
	void APIENTRY MyEnable(const GLenum cap) { SetEnabled(cap, GL_TRUE); o_glEnable(cap); }
	void APIENTRY MyDisable(const GLenum cap) { SetEnabled(cap, GL_FALSE); o_glDisable(cap); }
	void APIENTRY MyBlendFunc(const GLenum s, const GLenum d)
	{
		if (K())
		{
			c.draw.blendSrcRgb = c.draw.blendSrcAlpha = static_cast<GLint>(s);
			c.draw.blendDstRgb = c.draw.blendDstAlpha = static_cast<GLint>(d);
		}
		o_glBlendFunc(s, d);
	}
	void APIENTRY MyBlendFuncSeparate(const GLenum sr, const GLenum dr, const GLenum sa, const GLenum da)
	{
		if (K())
		{
			c.draw.blendSrcRgb = static_cast<GLint>(sr);
			c.draw.blendDstRgb = static_cast<GLint>(dr);
			c.draw.blendSrcAlpha = static_cast<GLint>(sa);
			c.draw.blendDstAlpha = static_cast<GLint>(da);
		}
		o_glBlendFuncSeparate(sr, dr, sa, da);
	}
	void APIENTRY MyBlendEquation(const GLenum m)
	{
		if (K())
			c.draw.blendEqRgb = c.draw.blendEqAlpha = static_cast<GLint>(m);
		o_glBlendEquation(m);
	}
	void APIENTRY MyBlendEquationEXT(const GLenum m)
	{
		if (K())
			c.draw.blendEqRgb = c.draw.blendEqAlpha = static_cast<GLint>(m);
		o_glBlendEquationEXT(m);
	}
	void APIENTRY MyBlendEquationSeparate(const GLenum r, const GLenum a)
	{
		if (K())
		{
			c.draw.blendEqRgb = static_cast<GLint>(r);
			c.draw.blendEqAlpha = static_cast<GLint>(a);
		}
		o_glBlendEquationSeparate(r, a);
	}
	void APIENTRY MyBlendEquationSeparateEXT(const GLenum r, const GLenum a)
	{
		if (K())
		{
			c.draw.blendEqRgb = static_cast<GLint>(r);
			c.draw.blendEqAlpha = static_cast<GLint>(a);
		}
		o_glBlendEquationSeparateEXT(r, a);
	}
	void APIENTRY MyDepthFunc(const GLenum f) { if (K()) c.draw.depthFunc = static_cast<GLint>(f); o_glDepthFunc(f); }
	void APIENTRY MyDepthMask(const GLboolean m) { if (K()) c.draw.depthMask = m; o_glDepthMask(m); }
	void APIENTRY MyAlphaFunc(const GLenum f, const GLclampf r)
	{
		if (K())
		{
			c.draw.alphaFunc = static_cast<GLint>(f);
			c.draw.alphaRef = r < 0 ? 0.0f : r > 1 ? 1.0f : r;
		}
		o_glAlphaFunc(f, r);
	}
	void APIENTRY MyColorMask(const GLboolean r, const GLboolean g, const GLboolean b, const GLboolean a)
	{
		if (K())
		{
			c.draw.colorMask[0] = r;
			c.draw.colorMask[1] = g;
			c.draw.colorMask[2] = b;
			c.draw.colorMask[3] = a;
		}
		o_glColorMask(r, g, b, a);
	}
	void APIENTRY MyCullFace(const GLenum m) { if (K()) c.draw.cullMode = static_cast<GLint>(m); o_glCullFace(m); }
	void APIENTRY MyFrontFace(const GLenum m) { if (K()) c.draw.frontFace = static_cast<GLint>(m); o_glFrontFace(m); }
	void APIENTRY MyEnableClientState(const GLenum cap) { SetClientEnabled(cap, GL_TRUE); o_glEnableClientState(cap); }
	void APIENTRY MyDisableClientState(const GLenum cap) { SetClientEnabled(cap, GL_FALSE); o_glDisableClientState(cap); }
	void APIENTRY MyColorPointer(const GLint size, const GLenum type, const GLsizei stride, const void* pointer)
	{
		if (K() && c.vao == 0)
			SetPointer(c.color, size, type, stride, pointer);
		o_glColorPointer(size, type, stride, pointer);
	}
	void APIENTRY MyNormalPointer(const GLenum type, const GLsizei stride, const void* pointer)
	{
		if (K() && c.vao == 0)
			SetPointer(c.normal, 3, type, stride, pointer);
		o_glNormalPointer(type, stride, pointer);
	}
	void APIENTRY MyClearColor(const GLclampf r, const GLclampf g, const GLclampf b, const GLclampf a)
	{
		if (K())
		{
			c.clearColor[0] = r;
			c.clearColor[1] = g;
			c.clearColor[2] = b;
			c.clearColor[3] = a;
		}
		o_glClearColor(r, g, b, a);
	}
	void APIENTRY MyClearDepth(const GLclampd d) { if (K()) c.clearDepth = d < 0 ? 0.0 : d > 1 ? 1.0 : d; o_glClearDepth(d); }
	void APIENTRY MyMatrixMode(const GLenum m) { if (K()) c.matrixMode = static_cast<GLint>(m); o_glMatrixMode(m); }
	void APIENTRY MyLoadIdentity()
	{
		if (K() && c.matrixMode == GL_PROJECTION)
		{
			static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
			memcpy(c.projection, identity, sizeof(identity));
			c.projectionKnown = true;
		}
		else
			ProjectionChanges();
		o_glLoadIdentity();
	}
	void APIENTRY MyLoadMatrixf(const GLfloat* m)
	{
		if (K() && c.matrixMode == GL_PROJECTION && m != nullptr)
		{
			memcpy(c.projection, m, sizeof(c.projection));
			c.projectionKnown = true;
		}
		else
			ProjectionChanges();
		o_glLoadMatrixf(m);
	}
	void APIENTRY MyLoadMatrixd(const GLdouble* m) { ProjectionChanges(); o_glLoadMatrixd(m); }
	void APIENTRY MyMultMatrixf(const GLfloat* m) { ProjectionChanges(); o_glMultMatrixf(m); }
	void APIENTRY MyMultMatrixd(const GLdouble* m) { ProjectionChanges(); o_glMultMatrixd(m); }
	void APIENTRY MyOrtho(const GLdouble l, const GLdouble r, const GLdouble b, const GLdouble t, const GLdouble n, const GLdouble f)
	{
		ProjectionChanges();
		o_glOrtho(l, r, b, t, n, f);
	}
	void APIENTRY MyFrustum(const GLdouble l, const GLdouble r, const GLdouble b, const GLdouble t, const GLdouble n, const GLdouble f)
	{
		ProjectionChanges();
		o_glFrustum(l, r, b, t, n, f);
	}
	void APIENTRY MyRotatef(const GLfloat a, const GLfloat x, const GLfloat y, const GLfloat z) { ProjectionChanges(); o_glRotatef(a, x, y, z); }
	void APIENTRY MyTranslatef(const GLfloat x, const GLfloat y, const GLfloat z) { ProjectionChanges(); o_glTranslatef(x, y, z); }
	void APIENTRY MyScalef(const GLfloat x, const GLfloat y, const GLfloat z) { ProjectionChanges(); o_glScalef(x, y, z); }
	void APIENTRY MyPushMatrix() { o_glPushMatrix(); }
	void APIENTRY MyPopMatrix() { ProjectionChanges(); o_glPopMatrix(); }
	//glPushAttrib / glPopAttrib: what the groups pushed hold comes back on the pop (the copy follows; a pop it can't match
	//makes it read the driver again)
	struct Pushed
	{
		GLbitfield mask = 0;
		bool known = false;
		Copy copy;
	};
	constexpr int STACK = 16;
	Pushed attribStack[STACK], clientStack[STACK];
	int attribDepth = 0, clientDepth = 0;

	void APIENTRY MyPushAttrib(const GLbitfield mask)
	{
		if (Mine())
		{
			if (attribDepth < STACK)
				attribStack[attribDepth] = { mask, c.known, c };
			attribDepth++;
		}
		o_glPushAttrib(mask);
	}

	void APIENTRY MyPopAttrib()
	{
		if (Mine())
		{
			if (attribDepth <= 0 || attribDepth > STACK || !attribStack[attribDepth - 1].known || !c.known)
				Lose();
			else
			{
				const Pushed& p = attribStack[attribDepth - 1];
				const Copy& o = p.copy;
				RenderState& d = c.draw;
				const RenderState& w = o.draw;
				const GLbitfield m = p.mask;
				if (m & (GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT))
				{
					d.blend = w.blend;
					d.alphaTest = w.alphaTest;
				}
				if (m & (GL_ENABLE_BIT | GL_DEPTH_BUFFER_BIT))
					d.depthTest = w.depthTest;
				if (m & (GL_ENABLE_BIT | GL_SCISSOR_BIT))
					d.scissor = w.scissor;
				if (m & (GL_ENABLE_BIT | GL_POLYGON_BIT))
					d.cull = w.cull;
				if (m & (GL_ENABLE_BIT | GL_STENCIL_BUFFER_BIT))
					d.stencil = w.stencil;
				if (m & GL_COLOR_BUFFER_BIT)
				{
					d.blendSrcRgb = w.blendSrcRgb;
					d.blendDstRgb = w.blendDstRgb;
					d.blendSrcAlpha = w.blendSrcAlpha;
					d.blendDstAlpha = w.blendDstAlpha;
					d.blendEqRgb = w.blendEqRgb;
					d.blendEqAlpha = w.blendEqAlpha;
					d.alphaFunc = w.alphaFunc;
					d.alphaRef = w.alphaRef;
					memcpy(d.colorMask, w.colorMask, sizeof(d.colorMask));
					memcpy(c.clearColor, o.clearColor, sizeof(c.clearColor));
				}
				if (m & GL_DEPTH_BUFFER_BIT)
				{
					d.depthFunc = w.depthFunc;
					d.depthMask = w.depthMask;
					c.clearDepth = o.clearDepth;
				}
				if (m & GL_SCISSOR_BIT)
					memcpy(d.scissorBox, w.scissorBox, sizeof(d.scissorBox));
				if (m & GL_VIEWPORT_BIT)
					memcpy(d.viewport, w.viewport, sizeof(d.viewport));
				if (m & GL_POLYGON_BIT)
				{
					d.cullMode = w.cullMode;
					d.frontFace = w.frontFace;
				}
				if (m & GL_TEXTURE_BIT)
				{
					c.activeUnit = o.activeUnit;
					memcpy(c.tex2d, o.tex2d, sizeof(c.tex2d));
					memcpy(c.tex3d, o.tex3d, sizeof(c.tex3d));
				}
				if (m & GL_TRANSFORM_BIT)
					c.matrixMode = o.matrixMode;
			}
			if (attribDepth > 0)
				attribDepth--;
		}
		o_glPopAttrib();
	}

	void APIENTRY MyPushClientAttrib(const GLbitfield mask)
	{
		if (Mine())
		{
			if (clientDepth < STACK)
				clientStack[clientDepth] = { mask, c.known, c };
			clientDepth++;
		}
		o_glPushClientAttrib(mask);
	}

	void APIENTRY MyPopClientAttrib()
	{
		if (Mine())
		{
			if (clientDepth <= 0 || clientDepth > STACK || !clientStack[clientDepth - 1].known || !c.known)
				Lose();
			else if (clientStack[clientDepth - 1].mask & GL_CLIENT_VERTEX_ARRAY_BIT)
			{
				const Copy& o = clientStack[clientDepth - 1].copy;
				c.clientUnit = o.clientUnit;
				c.vertex = o.vertex;
				c.color = o.color;
				c.normal = o.normal;
				memcpy(c.uv, o.uv, sizeof(c.uv));
				c.arrayBuffer = o.arrayBuffer;
				c.elementBuffer = o.elementBuffer;
				c.vao = o.vao;
			}
			if (clientDepth > 0)
				clientDepth--;
		}
		o_glPopClientAttrib();
	}
	void APIENTRY MyDeleteTextures(const GLsizei n, const GLuint* textures)
	{
		if (K() && textures != nullptr)
			for (GLsizei k = 0; k < n; k++)
				for (int u = 0; u < UNITS; u++)
				{
					if (c.tex2d[u] == textures[k])
						c.tex2d[u] = 0;
					if (c.tex3d[u] == textures[k])
						c.tex3d[u] = 0;
				}
		o_glDeleteTextures(n, textures);
	}
	BOOL WINAPI MyMakeCurrent(const HDC dc, const HGLRC context)
	{
		//another context on the render thread: its state is another one (the same one again changes nothing)
		if (Mine() && (context != wglGetCurrentContext() || dc != wglGetCurrentDC()))
		{
			Lose();
			c.programKnown = false;
			for (int k = 0; k < watchedCount; k++)
				watched[k].known = false;
		}
		return o_wglMakeCurrent(dc, context);
	}
	void APIENTRY MyUseProgram(const GLuint program)
	{
		if (Mine())
			c.program = static_cast<GLint>(program);
		o_glUseProgram(program);
	}
	void APIENTRY MyActiveTexture(const GLenum unit) { if (K()) c.activeUnit = static_cast<GLint>(unit) - GL_TEXTURE0; o_glActiveTexture(unit); }
	void APIENTRY MyActiveTextureARB(const GLenum unit) { if (K()) c.activeUnit = static_cast<GLint>(unit) - GL_TEXTURE0; o_glActiveTextureARB(unit); }
	void APIENTRY MyClientActiveTexture(const GLenum unit)
	{
		if (K())
			c.clientUnit = static_cast<GLint>(unit) - GL_TEXTURE0;
		o_glClientActiveTexture(unit);
	}
	void APIENTRY MyBindBuffer(const GLenum target, const GLuint buffer)
	{
		if (K())
		{
			if (target == GL_ARRAY_BUFFER)
				c.arrayBuffer = static_cast<GLint>(buffer);
			else if (target == GL_ELEMENT_ARRAY_BUFFER && c.vao == 0)
				c.elementBuffer = static_cast<GLint>(buffer);
		}
		o_glBindBuffer(target, buffer);
	}
	void APIENTRY MyBindFramebuffer(const GLenum target, const GLuint framebuffer)
	{
		if (K())
		{
			if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)
				c.draw.drawFramebuffer = static_cast<GLint>(framebuffer);
			if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER)
				c.draw.readFramebuffer = static_cast<GLint>(framebuffer);
		}
		o_glBindFramebuffer(target, framebuffer);
	}
	void APIENTRY MyBindFramebufferEXT(const GLenum target, const GLuint framebuffer)
	{
		if (K())
		{
			if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)
				c.draw.drawFramebuffer = static_cast<GLint>(framebuffer);
			if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER)
				c.draw.readFramebuffer = static_cast<GLint>(framebuffer);
		}
		o_glBindFramebufferEXT(target, framebuffer);
	}
	void APIENTRY MyBindVertexArray(const GLuint vao) { if (K()) c.vao = static_cast<GLint>(vao); o_glBindVertexArray(vao); }
	void APIENTRY MyUniform1i(const GLint location, const GLint v) { SetUniform(location, &v, nullptr); o_glUniform1i(location, v); }
	void APIENTRY MyUniform1iv(const GLint location, const GLsizei count, const GLint* v)
	{
		if (count > 0 && v != nullptr)
			SetUniform(location, v, nullptr);
		o_glUniform1iv(location, count, v);
	}
	void APIENTRY MyUniform3f(const GLint location, const GLfloat x, const GLfloat y, const GLfloat z)
	{
		const GLfloat v[3] = { x, y, z };
		SetUniform(location, nullptr, v);
		o_glUniform3f(location, x, y, z);
	}
	void APIENTRY MyUniform3fv(const GLint location, const GLsizei count, const GLfloat* v)
	{
		if (count > 0 && v != nullptr)
			SetUniform(location, nullptr, v);
		o_glUniform3fv(location, count, v);
	}
	void APIENTRY MyLinkProgram(const GLuint program)
	{
		//linking sets every uniform back to 0: the next read asks the driver
		for (int k = 0; k < watchedCount && Mine(); k++)
			if (watched[k].program == static_cast<GLint>(program))
				watched[k].known = false;
		o_glLinkProgram(program);
	}

	template <typename T>
	bool HookExport(const char* name, T detour, T& original)
	{
		LPVOID trampoline = nullptr;
		const MH_STATUS status = MH_CreateHookApi(L"OPENGL32", name, reinterpret_cast<LPVOID>(detour), &trampoline);
		if (status != MH_OK || trampoline == nullptr)
		{
			OutputDebug("FIELD_GL_TRACK: cannot hook %s (MinHook %d)\n", name, static_cast<int>(status));
			return false;
		}
		original = reinterpret_cast<T>(trampoline);
		return true;
	}

	//a function the driver gives through wglGetProcAddress (the game gets it the same way)
	template <typename T>
	bool HookDriver(const char* name, T detour, T& original)
	{
		const PROC address = wglGetProcAddress(name);
		const auto value = reinterpret_cast<intptr_t>(address);
		if (address == nullptr || value == 1 || value == 2 || value == 3 || value == -1)
		{
			OutputDebug("FIELD_GL_TRACK: the driver has no %s\n", name);
			return false;
		}
		LPVOID trampoline = nullptr;
		const MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(address), reinterpret_cast<LPVOID>(detour), &trampoline);
		if (status != MH_OK || trampoline == nullptr)
		{
			OutputDebug("FIELD_GL_TRACK: cannot hook %s (MinHook %d)\n", name, static_cast<int>(status));
			return false;
		}
		original = reinterpret_cast<T>(trampoline);
		return true;
	}

	//The same call under another name (see the o_gl...ARB / ...EXT pointers). Not having the name, or the name being the
	//entry that is hooked already, is fine; only a hook that can't be made isn't.
	template <typename T>
	bool HookOtherName(const char* name, T detour, T& original)
	{
		const PROC address = wglGetProcAddress(name);
		const auto value = reinterpret_cast<intptr_t>(address);
		if (address == nullptr || value == 1 || value == 2 || value == 3 || value == -1)
			return true;
		LPVOID trampoline = nullptr;
		const MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(address), reinterpret_cast<LPVOID>(detour), &trampoline);
		if (status == MH_ERROR_ALREADY_CREATED)
			return true; //one entry point for both names
		if (status != MH_OK || trampoline == nullptr)
		{
			OutputDebug("FIELD_GL_TRACK: cannot hook %s (MinHook %d)\n", name, static_cast<int>(status));
			return false;
		}
		original = reinterpret_cast<T>(trampoline);
		OutputDebug("FIELD_GL_TRACK: %s is its own entry in this driver: watched too\n", name);
		return true;
	}

	//--- the self-check
	char mismatch[700];
	bool different = false;

	//Every difference is written down (each kind once), not only the first: one log then says everything that is missed.
	//Always true, so the checks after a difference still run; Check looks at `different`.
	bool Same(const char* what, const double copy, const double driver)
	{
		if (copy == driver)
			return true;
		different = true;
		const size_t used = strlen(mismatch);
		if (strstr(mismatch, what) == nullptr && used + 100 < sizeof(mismatch))
			snprintf(mismatch + used, sizeof(mismatch) - used, "%s%s: remembered %g, the driver has %g", used ? "; " : "", what, copy, driver);
		return true;
	}

	bool SameArray(const char* what, const ArrayState& a, const ArrayState& b)
	{
		if (a.on != b.on)
			return Same(what, a.on, b.on);
		if (!a.on)
			return true;
		return Same(what, a.size, b.size) && Same(what, a.type, b.type) && Same(what, a.stride, b.stride) && Same(what, a.buffer, b.buffer)
			&& Same(what, static_cast<double>(reinterpret_cast<uintptr_t>(a.pointer)), static_cast<double>(reinterpret_cast<uintptr_t>(b.pointer)));
	}

	void Check(const char* where)
	{
		if (!Tracking())
			return;
		if (!c.known || !c.programKnown)
		{
			Sync();
			return;
		}
		mismatch[0] = 0;
		different = false;
		bool ok = true;
		GLint program = 0;
		glGetIntegerv(GL_CURRENT_PROGRAM, &program);
		ok &= Same("program", c.program, program);
		GLint activeUnit = 0;
		GLuint tex2d[UNITS], tex3d[UNITS];
		QueryTextures(activeUnit, tex2d, tex3d);
		ok &= Same("active texture unit", c.activeUnit, activeUnit);
		for (int u = 0; u < UNITS && ok; u++)
		{
			ok &= Same("2D texture binding", c.tex2d[u], tex2d[u]);
			ok &= Same("3D texture binding", c.tex3d[u], tex3d[u]);
		}
		RenderState d;
		QueryDraw(d);
		const RenderState& m = c.draw;
		ok = ok && Same("draw framebuffer", m.drawFramebuffer, d.drawFramebuffer) && Same("read framebuffer", m.readFramebuffer, d.readFramebuffer);
		for (int k = 0; k < 4 && ok; k++)
			ok &= Same("viewport", m.viewport[k], d.viewport[k]) && Same("scissor box", m.scissorBox[k], d.scissorBox[k])
			&& Same("colour mask", m.colorMask[k], d.colorMask[k]);
		ok = ok && Same("blend", m.blend, d.blend) && Same("blend source", m.blendSrcRgb, d.blendSrcRgb)
			&& Same("blend destination", m.blendDstRgb, d.blendDstRgb) && Same("blend alpha source", m.blendSrcAlpha, d.blendSrcAlpha)
			&& Same("blend alpha destination", m.blendDstAlpha, d.blendDstAlpha) && Same("blend equation", m.blendEqRgb, d.blendEqRgb)
			&& Same("blend alpha equation", m.blendEqAlpha, d.blendEqAlpha) && Same("depth test", m.depthTest, d.depthTest)
			&& Same("depth function", m.depthFunc, d.depthFunc) && Same("depth writes", m.depthMask, d.depthMask)
			&& Same("alpha test", m.alphaTest, d.alphaTest) && Same("alpha function", m.alphaFunc, d.alphaFunc)
			&& Same("alpha reference", std::round(m.alphaRef * 1000.0), std::round(d.alphaRef * 1000.0))
			&& Same("scissor test", m.scissor, d.scissor) && Same("cull", m.cull, d.cull) && Same("cull mode", m.cullMode, d.cullMode)
			&& Same("front face", m.frontFace, d.frontFace) && Same("stencil test", m.stencil, d.stencil);
		if (ok && c.vao == 0)
		{
			ClientState q;
			QueryClient(q);
			ok = Same("client texture unit", c.clientUnit, q.clientUnit - GL_TEXTURE0) && Same("array buffer", c.arrayBuffer, q.arrayBuffer)
				&& Same("element buffer", c.elementBuffer, q.elementBuffer) && SameArray("vertex array", c.vertex, q.vertex)
				&& SameArray("colour array", c.color, q.color) && SameArray("normal array", c.normal, q.normal)
				&& SameArray("texture coordinates 0", c.uv[0], q.uv0) && SameArray("texture coordinates 1", c.uv[1], q.uv1);
		}
		if (ok)
		{
			GLint mode = 0;
			glGetIntegerv(GL_MATRIX_MODE, &mode);
			ok = Same("matrix mode", c.matrixMode, mode);
		}
		if (ok && c.projectionKnown)
		{
			float projection[16];
			glGetFloatv(GL_PROJECTION_MATRIX, projection);
			for (int k = 0; k < 16 && ok; k++)
				ok = Same("projection matrix", c.projection[k], projection[k]);
		}
		for (int k = 0; k < watchedCount && ok; k++)
		{
			const Watched& w = watched[k];
			if (!w.known || w.program == 0)
				continue;
			if (w.kind == 1)
			{
				GLint v = 0;
				glGetUniformiv(static_cast<GLuint>(w.program), w.location, &v);
				ok = Same("a uniform of the game's", w.i, v);
			}
			else if (w.kind == 2)
			{
				float v[3] = {};
				glGetUniformfv(static_cast<GLuint>(w.program), w.location, v);
				for (int i = 0; i < 3 && ok; i++)
					ok = Same("a uniform of the game's", w.f[i], v[i]);
			}
		}
		if (different)
		{
			broken = true;
			OutputDebug("FIELD_GL_TRACK: the GL state DEMASTER remembers differs from the driver's (%s, %s): DEMASTER asks the driver "
				"from now on, as v15 did (slower). Please send this log.\n", mismatch, where);
		}
	}
}

//--- the public side

void glstate::Install()
{
	if (installed || broken)
		return;
	if (!FIELD_LAYERS || !(FIELD_PBR || FIELD_SMOOTH_SCROLL || FIELD_INTERPOLATION))
	{
		broken = true; //nothing would use it
		return;
	}
	if (!FIELD_GL_TRACK)
	{
		broken = true;
		OutputDebug("FIELD_GL_TRACK=0: DEMASTER asks the driver for the GL state around the field's draws\n");
		return;
	}
	bool ok = true;
	ok &= HookExport("glEnable", &MyEnable, o_glEnable);
	ok &= HookExport("glDisable", &MyDisable, o_glDisable);
	ok &= HookExport("glBlendFunc", &MyBlendFunc, o_glBlendFunc);
	ok &= HookExport("glDepthFunc", &MyDepthFunc, o_glDepthFunc);
	ok &= HookExport("glDepthMask", &MyDepthMask, o_glDepthMask);
	ok &= HookExport("glAlphaFunc", &MyAlphaFunc, o_glAlphaFunc);
	ok &= HookExport("glColorMask", &MyColorMask, o_glColorMask);
	ok &= HookExport("glCullFace", &MyCullFace, o_glCullFace);
	ok &= HookExport("glFrontFace", &MyFrontFace, o_glFrontFace);
	ok &= HookExport("glEnableClientState", &MyEnableClientState, o_glEnableClientState);
	ok &= HookExport("glDisableClientState", &MyDisableClientState, o_glDisableClientState);
	ok &= HookExport("glColorPointer", &MyColorPointer, o_glColorPointer);
	ok &= HookExport("glNormalPointer", &MyNormalPointer, o_glNormalPointer);
	ok &= HookExport("glClearColor", &MyClearColor, o_glClearColor);
	ok &= HookExport("glClearDepth", &MyClearDepth, o_glClearDepth);
	ok &= HookExport("glMatrixMode", &MyMatrixMode, o_glMatrixMode);
	ok &= HookExport("glLoadIdentity", &MyLoadIdentity, o_glLoadIdentity);
	ok &= HookExport("glLoadMatrixf", &MyLoadMatrixf, o_glLoadMatrixf);
	ok &= HookExport("glLoadMatrixd", &MyLoadMatrixd, o_glLoadMatrixd);
	ok &= HookExport("glMultMatrixf", &MyMultMatrixf, o_glMultMatrixf);
	ok &= HookExport("glMultMatrixd", &MyMultMatrixd, o_glMultMatrixd);
	ok &= HookExport("glOrtho", &MyOrtho, o_glOrtho);
	ok &= HookExport("glFrustum", &MyFrustum, o_glFrustum);
	ok &= HookExport("glRotatef", &MyRotatef, o_glRotatef);
	ok &= HookExport("glTranslatef", &MyTranslatef, o_glTranslatef);
	ok &= HookExport("glScalef", &MyScalef, o_glScalef);
	ok &= HookExport("glPushMatrix", &MyPushMatrix, o_glPushMatrix);
	ok &= HookExport("glPopMatrix", &MyPopMatrix, o_glPopMatrix);
	ok &= HookExport("glPushAttrib", &MyPushAttrib, o_glPushAttrib);
	ok &= HookExport("glPopAttrib", &MyPopAttrib, o_glPopAttrib);
	ok &= HookExport("glPushClientAttrib", &MyPushClientAttrib, o_glPushClientAttrib);
	ok &= HookExport("glPopClientAttrib", &MyPopClientAttrib, o_glPopClientAttrib);
	ok &= HookExport("glDeleteTextures", &MyDeleteTextures, o_glDeleteTextures);
	ok &= HookExport("wglMakeCurrent", &MyMakeCurrent, o_wglMakeCurrent);
	ok &= HookDriver("glUseProgram", &MyUseProgram, o_glUseProgram);
	ok &= HookDriver("glActiveTexture", &MyActiveTexture, o_glActiveTexture);
	ok &= HookDriver("glClientActiveTexture", &MyClientActiveTexture, o_glClientActiveTexture);
	ok &= HookDriver("glBindBuffer", &MyBindBuffer, o_glBindBuffer);
	ok &= HookDriver("glBindFramebuffer", &MyBindFramebuffer, o_glBindFramebuffer);
	ok &= HookDriver("glBlendFuncSeparate", &MyBlendFuncSeparate, o_glBlendFuncSeparate);
	ok &= HookDriver("glBlendEquation", &MyBlendEquation, o_glBlendEquation);
	ok &= HookDriver("glBlendEquationSeparate", &MyBlendEquationSeparate, o_glBlendEquationSeparate);
	ok &= HookDriver("glUniform1i", &MyUniform1i, o_glUniform1i);
	ok &= HookDriver("glUniform1iv", &MyUniform1iv, o_glUniform1iv);
	ok &= HookDriver("glUniform3f", &MyUniform3f, o_glUniform3f);
	ok &= HookDriver("glUniform3fv", &MyUniform3fv, o_glUniform3fv);
	ok &= HookDriver("glBindVertexArray", &MyBindVertexArray, o_glBindVertexArray);
	ok &= HookDriver("glLinkProgram", &MyLinkProgram, o_glLinkProgram);
	ok &= HookOtherName("glActiveTextureARB", &MyActiveTextureARB, o_glActiveTextureARB);
	ok &= HookOtherName("glBindFramebufferEXT", &MyBindFramebufferEXT, o_glBindFramebufferEXT);
	ok &= HookOtherName("glBlendEquationEXT", &MyBlendEquationEXT, o_glBlendEquationEXT);
	ok &= HookOtherName("glBlendEquationSeparateEXT", &MyBlendEquationSeparateEXT, o_glBlendEquationSeparateEXT);
	if (!ok)
	{
		broken = true;
		OutputDebug("FIELD_GL_TRACK: not every GL call could be hooked: DEMASTER asks the driver for the GL state instead (slower)\n");
		return;
	}
	installed = true;
	renderThread = GetCurrentThreadId();
	Sync();
	OutputDebug("FIELD_GL_TRACK: DEMASTER remembers the GL state as the game sets it (no questions to the driver around each draw)\n");
}

bool glstate::On()
{
	return Tracking();
}

GLint glstate::Program()
{
	if (Ready())
		return c.program;
	GLint program = 0;
	glGetIntegerv(GL_CURRENT_PROGRAM, &program);
	return program;
}

GLenum glstate::ActiveTexture()
{
	if (Ready())
		return static_cast<GLenum>(GL_TEXTURE0 + c.activeUnit);
	GLint active = GL_TEXTURE0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	return static_cast<GLenum>(active);
}

GLuint glstate::Texture(const GLenum target, const int unit)
{
	if (Ready() && unit >= 0 && unit < UNITS)
		return target == GL_TEXTURE_3D ? c.tex3d[unit] : c.tex2d[unit];
	GLint active = GL_TEXTURE0, bound = 0;
	glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
	glActiveTexture(GL_TEXTURE0 + unit);
	glGetIntegerv(target == GL_TEXTURE_3D ? GL_TEXTURE_BINDING_3D : GL_TEXTURE_BINDING_2D, &bound);
	glActiveTexture(static_cast<GLenum>(active));
	return static_cast<GLuint>(bound);
}

GLuint glstate::BoundTexture2D()
{
	if (Ready() && c.activeUnit >= 0 && c.activeUnit < UNITS)
		return c.tex2d[c.activeUnit];
	GLint bound = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
	return static_cast<GLuint>(bound);
}

void glstate::Capture(RenderState& s)
{
	if (Ready())
		s = c.draw;
	else
		QueryDraw(s);
}

GLuint glstate::DrawFramebuffer()
{
	if (Ready())
		return static_cast<GLuint>(c.draw.drawFramebuffer);
	GLint bound = 0;
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &bound);
	return static_cast<GLuint>(bound);
}

GLuint glstate::ReadFramebuffer()
{
	if (Ready())
		return static_cast<GLuint>(c.draw.readFramebuffer);
	GLint bound = 0;
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &bound);
	return static_cast<GLuint>(bound);
}

void glstate::SaveClient(ClientState& s)
{
	if (Ready() && c.vao == 0)
	{
		s.clientUnit = GL_TEXTURE0 + c.clientUnit;
		s.arrayBuffer = c.arrayBuffer;
		s.elementBuffer = c.elementBuffer;
		s.vertex = c.vertex;
		s.color = c.color;
		s.normal = c.normal;
		s.uv0 = c.uv[0];
		s.uv1 = c.uv[1];
	}
	else
		QueryClient(s);
}

bool glstate::BlendOn()
{
	if (Ready())
		return c.draw.blend != GL_FALSE;
	return glIsEnabled(GL_BLEND) != GL_FALSE;
}

GLint glstate::BlendEquationRgb()
{
	if (Ready())
		return c.draw.blendEqRgb;
	GLint equation = GL_FUNC_ADD;
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &equation);
	return equation;
}

GLint glstate::ElementBuffer()
{
	if (Ready() && c.vao == 0)
		return c.elementBuffer;
	GLint buffer = 0;
	glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &buffer);
	return buffer;
}

GLint glstate::VertexArrayBuffer()
{
	if (Ready() && c.vao == 0)
		return c.vertex.buffer;
	GLint buffer = 0;
	glGetIntegerv(GL_VERTEX_ARRAY_BUFFER_BINDING, &buffer);
	return buffer;
}

void glstate::ColorArray(void*& pointer, GLint& stride, GLint& type)
{
	if (Ready() && c.vao == 0)
	{
		pointer = c.color.buffer == 0 ? c.color.pointer : nullptr;
		stride = c.color.stride;
		type = c.color.type;
		return;
	}
	glGetPointerv(GL_COLOR_ARRAY_POINTER, &pointer);
	glGetIntegerv(GL_COLOR_ARRAY_STRIDE, &stride);
	glGetIntegerv(GL_COLOR_ARRAY_TYPE, &type);
	GLint buffer = 0;
	glGetIntegerv(GL_COLOR_ARRAY_BUFFER_BINDING, &buffer);
	if (buffer != 0)
		pointer = nullptr;
}

void glstate::CullState(GLboolean& on, GLint& mode, GLint& front)
{
	if (Ready())
	{
		on = c.draw.cull;
		mode = c.draw.cullMode;
		front = c.draw.frontFace;
		return;
	}
	on = glIsEnabled(GL_CULL_FACE);
	glGetIntegerv(GL_CULL_FACE_MODE, &mode);
	glGetIntegerv(GL_FRONT_FACE, &front);
}

void glstate::ProjectionMatrix(float out[16])
{
	if (Ready() && c.projectionKnown)
	{
		memcpy(out, c.projection, sizeof(c.projection));
		return;
	}
	glGetFloatv(GL_PROJECTION_MATRIX, out);
	if (Tracking() && c.known)
	{
		memcpy(c.projection, out, sizeof(c.projection));
		c.projectionKnown = true;
	}
}

GLint glstate::MatrixMode()
{
	if (Ready())
		return c.matrixMode;
	GLint mode = GL_MODELVIEW;
	glGetIntegerv(GL_MATRIX_MODE, &mode);
	return mode;
}

void glstate::ClearValues(float color[4], double& depth)
{
	if (Ready())
	{
		memcpy(color, c.clearColor, sizeof(c.clearColor));
		depth = c.clearDepth;
		return;
	}
	glGetFloatv(GL_COLOR_CLEAR_VALUE, color);
	glGetDoublev(GL_DEPTH_CLEAR_VALUE, &depth);
}

namespace
{
	Watched* Watch(const GLint program, const GLint location)
	{
		for (int k = 0; k < watchedCount; k++)
			if (watched[k].program == program && watched[k].location == location)
				return &watched[k];
		if (watchedCount >= WATCHED)
			return nullptr;
		Watched& w = watched[watchedCount++];
		w = Watched();
		w.program = program;
		w.location = location;
		return &w;
	}
}

GLint glstate::UniformInt(const GLint program, const GLint location)
{
	GLint value = 0;
	if (program == 0 || location < 0)
		return value;
	Watched* w = Ready() ? Watch(program, location) : nullptr;
	if (w != nullptr)
		w->kind = 1;
	if (w != nullptr && w->known)
		return w->i;
	glGetUniformiv(static_cast<GLuint>(program), location, &value);
	if (w != nullptr)
	{
		w->i = value;
		w->known = true;
	}
	return value;
}

void glstate::UniformVec3(const GLint program, const GLint location, float value[3])
{
	value[0] = value[1] = value[2] = 0;
	if (program == 0 || location < 0)
		return;
	Watched* w = Ready() ? Watch(program, location) : nullptr;
	if (w != nullptr)
		w->kind = 2;
	if (w != nullptr && w->known)
	{
		memcpy(value, w->f, sizeof(w->f));
		return;
	}
	glGetUniformfv(static_cast<GLuint>(program), location, value);
	if (w != nullptr)
	{
		memcpy(w->f, value, sizeof(w->f));
		w->known = true;
	}
}

void glstate::Enable(const GLenum what, const bool on)
{
	if (on)
		glEnable(what);
	else
		glDisable(what);
}

void glstate::RawViewport(const GLint x, const GLint y, const GLsizei width, const GLsizei height)
{
	OnViewport(x, y, width, height);
	if (oglViewport != nullptr)
		reinterpret_cast<void(__stdcall*)(GLint, GLint, GLsizei, GLsizei)>(oglViewport)(x, y, width, height);
	else
		glViewport(x, y, width, height);
}

void glstate::RawScissor(const GLint x, const GLint y, const GLsizei width, const GLsizei height)
{
	OnScissor(x, y, width, height);
	if (ogl_scissors != nullptr)
		reinterpret_cast<void(__stdcall*)(GLint, GLint, GLsizei, GLsizei)>(ogl_scissors)(x, y, width, height);
	else
		glScissor(x, y, width, height);
}

void glstate::Apply(const RenderState& s)
{
	//only what differs from the copy (everything when the copy is off)
	const bool known = Ready();
	const RenderState& m = c.draw;
	if (!known || m.drawFramebuffer != s.drawFramebuffer)
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(s.drawFramebuffer));
	if (!known || m.readFramebuffer != s.readFramebuffer)
		glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(s.readFramebuffer));
	if (!known || memcmp(m.viewport, s.viewport, sizeof(s.viewport)) != 0)
		RawViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
	if (!known || m.blend != s.blend)
		Enable(GL_BLEND, s.blend != GL_FALSE);
	if (!known || m.blendSrcRgb != s.blendSrcRgb || m.blendDstRgb != s.blendDstRgb || m.blendSrcAlpha != s.blendSrcAlpha
		|| m.blendDstAlpha != s.blendDstAlpha)
		glBlendFuncSeparate(static_cast<GLenum>(s.blendSrcRgb), static_cast<GLenum>(s.blendDstRgb), static_cast<GLenum>(s.blendSrcAlpha),
			static_cast<GLenum>(s.blendDstAlpha));
	if (!known || m.blendEqRgb != s.blendEqRgb || m.blendEqAlpha != s.blendEqAlpha)
		glBlendEquationSeparate(static_cast<GLenum>(s.blendEqRgb), static_cast<GLenum>(s.blendEqAlpha));
	if (!known || m.depthTest != s.depthTest)
		Enable(GL_DEPTH_TEST, s.depthTest != GL_FALSE);
	if (!known || m.depthFunc != s.depthFunc)
		glDepthFunc(static_cast<GLenum>(s.depthFunc));
	if (!known || m.depthMask != s.depthMask)
		glDepthMask(s.depthMask);
	if (!known || m.alphaTest != s.alphaTest)
		Enable(GL_ALPHA_TEST, s.alphaTest != GL_FALSE);
	if (!known || m.alphaFunc != s.alphaFunc || m.alphaRef != s.alphaRef)
		glAlphaFunc(static_cast<GLenum>(s.alphaFunc), s.alphaRef);
	if (!known || m.scissor != s.scissor)
		Enable(GL_SCISSOR_TEST, s.scissor != GL_FALSE);
	if (!known || memcmp(m.scissorBox, s.scissorBox, sizeof(s.scissorBox)) != 0)
		RawScissor(s.scissorBox[0], s.scissorBox[1], s.scissorBox[2], s.scissorBox[3]);
	if (!known || memcmp(m.colorMask, s.colorMask, sizeof(s.colorMask)) != 0)
		glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
	if (!known || m.cull != s.cull)
		Enable(GL_CULL_FACE, s.cull != GL_FALSE);
	if (!known || m.cullMode != s.cullMode)
		glCullFace(static_cast<GLenum>(s.cullMode));
	if (!known || m.frontFace != s.frontFace)
		glFrontFace(static_cast<GLenum>(s.frontFace));
	if (!known || m.stencil != s.stencil)
		Enable(GL_STENCIL_TEST, s.stencil != GL_FALSE);
}

void glstate::RestoreClient(const ClientState& s)
{
	const auto put = [](const ArrayState& a, const GLenum which, void (*set)(const ArrayState&))
	{
		glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(a.buffer));
		set(a);
		if (a.on)
			glEnableClientState(which);
		else
			glDisableClientState(which);
	};
	put(s.vertex, GL_VERTEX_ARRAY, [](const ArrayState& a) { glVertexPointer(a.size, static_cast<GLenum>(a.type), a.stride, a.pointer); });
	put(s.color, GL_COLOR_ARRAY, [](const ArrayState& a) { glColorPointer(a.size, static_cast<GLenum>(a.type), a.stride, a.pointer); });
	put(s.normal, GL_NORMAL_ARRAY, [](const ArrayState& a) { glNormalPointer(static_cast<GLenum>(a.type), a.stride, a.pointer); });
	glClientActiveTexture(GL_TEXTURE1);
	put(s.uv1, GL_TEXTURE_COORD_ARRAY, [](const ArrayState& a) { glTexCoordPointer(a.size, static_cast<GLenum>(a.type), a.stride, a.pointer); });
	glClientActiveTexture(GL_TEXTURE0);
	put(s.uv0, GL_TEXTURE_COORD_ARRAY, [](const ArrayState& a) { glTexCoordPointer(a.size, static_cast<GLenum>(a.type), a.stride, a.pointer); });
	glClientActiveTexture(static_cast<GLenum>(s.clientUnit));
	glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(s.arrayBuffer));
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLuint>(s.elementBuffer));
}

void glstate::OnBindTexture(const GLenum target, const GLuint texture)
{
	if (!K() || c.activeUnit < 0 || c.activeUnit >= UNITS)
		return;
	if (target == GL_TEXTURE_2D)
		c.tex2d[c.activeUnit] = texture;
	else if (target == GL_TEXTURE_3D)
		c.tex3d[c.activeUnit] = texture;
}

void glstate::OnViewport(const GLint x, const GLint y, const GLsizei width, const GLsizei height)
{
	if (!K())
		return;
	c.draw.viewport[0] = x;
	c.draw.viewport[1] = y;
	c.draw.viewport[2] = width;
	c.draw.viewport[3] = height;
}

void glstate::OnScissor(const GLint x, const GLint y, const GLsizei width, const GLsizei height)
{
	if (!K())
		return;
	c.draw.scissorBox[0] = x;
	c.draw.scissorBox[1] = y;
	c.draw.scissorBox[2] = width;
	c.draw.scissorBox[3] = height;
}

void glstate::OnVertexPointer(const GLint size, const GLenum type, const GLsizei stride, const void* pointer)
{
	if (K() && c.vao == 0)
		SetPointer(c.vertex, size, type, stride, pointer);
}

void glstate::OnTexCoordPointer(const GLint size, const GLenum type, const GLsizei stride, const void* pointer)
{
	if (!K() || c.vao != 0)
		return;
	if (ArrayState* a = TexCoordArray())
		SetPointer(*a, size, type, stride, pointer);
}

void glstate::FrameEnd()
{
	if (!Tracking())
		return;
	frames++;
	//early (a wrong copy shows up at once), then every half a minute or so
	if (frames == 3 || frames == 30 || frames == 300 || frames % 1800 == 0)
	{
		Check("at the end of a frame");
		checkDue = true;
	}
}

void glstate::CheckPoint()
{
	if (!checkDue || !Tracking())
		return;
	checkDue = false;
	Check("at a character draw");
}
