#pragma once
//The OpenGL state the field modules need around the game's draws, remembered as it is set instead of asked of the driver.
//
//Asking the driver (glGet*, glIsEnabled, glGetUniform*) makes the game wait until the driver has done everything queued
//before the question. With a threaded driver (NVIDIA's "Threaded optimization", AMD's and Intel's equivalents) that wait
//happens on every question, and the field code needs the state around hundreds of draws a frame. So hooks on the calls
//that set state (glEnable, glBlendFunc, glUseProgram, glBindTexture, ...) keep a copy, and the field code reads the copy.
//
//The copy checks itself: a few times early on and then every half a minute it is compared with what the driver says. If
//they ever differ (a way of setting state the hooks don't see), the log says what differed and the copy turns itself off:
//from then on everything is asked of the driver, as before. FIELD_GL_TRACK=0 in demaster.conf turns it off from the start.

#include <GL/glew.h>

#include <cstdint>

namespace glstate
{
	//what a draw needs set the way the game had it
	struct RenderState
	{
		GLint drawFramebuffer, readFramebuffer, viewport[4];
		GLboolean blend; GLint blendSrcRgb, blendDstRgb, blendSrcAlpha, blendDstAlpha, blendEqRgb, blendEqAlpha;
		GLboolean depthTest; GLint depthFunc; GLboolean depthMask;
		GLboolean alphaTest; GLint alphaFunc; GLfloat alphaRef;
		GLboolean scissor; GLint scissorBox[4];
		GLboolean colorMask[4];
		GLboolean cull; GLint cullMode, frontFace;
		GLboolean stencil;
	};

	//one client array (the game draws from client memory)
	struct ArrayState
	{
		GLboolean on = GL_FALSE;
		GLint size = 4, type = GL_FLOAT, stride = 0, buffer = 0;
		void* pointer = nullptr;
	};
	struct ClientState
	{
		ArrayState vertex, color, normal, uv0, uv1;
		GLint clientUnit = GL_TEXTURE0, arrayBuffer = 0, elementBuffer = 0;
	};

	//ViewportBackdoorInject (the context is current, GLEW is up), before MH_EnableHook: makes the hooks, reads the state once
	void Install();
	//the copy is in use (installed, not turned off, not found wrong)
	bool On();

	//--- reading (from the copy when it is on, else from the driver)
	GLint Program();
	GLenum ActiveTexture(); //GL_TEXTURE0 + unit
	GLuint Texture(GLenum target, int unit); //GL_TEXTURE_2D or GL_TEXTURE_3D on that unit
	GLuint BoundTexture2D(); //on the active unit
	void Capture(RenderState& s);
	GLuint DrawFramebuffer(); //bound for drawing, for reading
	GLuint ReadFramebuffer();
	void SaveClient(ClientState& c);
	bool BlendOn();
	GLint BlendEquationRgb();
	GLint ElementBuffer();
	GLint VertexArrayBuffer(); //the buffer the vertex array reads from (0: client memory)
	void ColorArray(void*& pointer, GLint& stride, GLint& type);
	void CullState(GLboolean& on, GLint& mode, GLint& front);
	void ProjectionMatrix(float out[16]);
	GLint MatrixMode();
	void ClearValues(float color[4], double& depth);
	//a uniform of one of the game's programs: the last value set (asked of the driver the first time)
	GLint UniformInt(GLint program, GLint location);
	void UniformVec3(GLint program, GLint location, float value[3]);

	//--- setting (DEMASTER's own calls go through the hooks too; these are for the ones that skip DEMASTER's hooks)
	void Apply(const RenderState& s);
	void RestoreClient(const ClientState& c);
	//straight to the driver: DEMASTER's glViewport / glScissor hooks change some values (FILL_ASPECT_RATIO, the forced scissor)
	void RawViewport(GLint x, GLint y, GLsizei width, GLsizei height);
	void RawScissor(GLint x, GLint y, GLsizei width, GLsizei height);
	void Enable(GLenum what, bool on);

	//--- from DEMASTER's existing hooks (opengl.cpp, texture.cpp): the values the driver was given
	void OnBindTexture(GLenum target, GLuint texture);
	void OnViewport(GLint x, GLint y, GLsizei width, GLsizei height);
	void OnScissor(GLint x, GLint y, GLsizei width, GLsizei height);
	void OnVertexPointer(GLint size, GLenum type, GLsizei stride, const void* pointer);
	void OnTexCoordPointer(GLint size, GLenum type, GLsizei stride, const void* pointer);

	//SwapBuffers: the copy compares itself with the driver now and then
	void FrameEnd();
	//a character draw: on a frame the copy checks itself, the first one compares too (the state in the middle of a frame)
	void CheckPoint();
}
