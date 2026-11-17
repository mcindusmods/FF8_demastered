//Runs ff8_demaster/field_mirror.cpp (FIELD_INTERPOLATION's second picture) on the GPU (Linux, EGL, a pbuffer as the
//screen) against a small frame built like the game's field frame: a scene framebuffer with depth and stencil, a
//half-size framebuffer filled by a blit and drawn back over the scene, a framebuffer that is never cleared, a copy into a
//loose texture, a post pass and a final draw to the screen. The frame is drawn three times: with everything where the
//in-between picture has it, with everything where the game's frame has it, and once more as the game's frame with the
//mirror on and the two things moved for the second draw (a matrix, a vertex array). The mirror's picture must be the
//first one pixel for pixel, and the game's own picture must be untouched.
//  g++ -std=c++20 -O1 mirror_test.cpp ../ff8_demaster/field_mirror.cpp -lEGL -lGL -o mirror_test
#define GL_GLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include "../ff8_demaster/field_mirror.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int checks = 0, failures = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

constexpr int SCREEN_W = 320, SCREEN_H = 240, SCENE_W = 256, SCENE_H = 192;

//--- what the mirror asks its host
static fieldmirror::Name DrawFramebuffer() { GLint v = 0; glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &v); return static_cast<GLuint>(v); }
static fieldmirror::Name ReadFramebuffer() { GLint v = 0; glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &v); return static_cast<GLuint>(v); }
static unsigned int ActiveTexture() { GLint v = 0; glGetIntegerv(GL_ACTIVE_TEXTURE, &v); return static_cast<unsigned int>(v); }
static fieldmirror::Name Texture2D(const int unit)
{
	const unsigned int active = ActiveTexture();
	glActiveTexture(GL_TEXTURE0 + unit);
	GLint v = 0;
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &v);
	glActiveTexture(active);
	return static_cast<GLuint>(v);
}
static void BindTexture2D(const fieldmirror::Name texture) { glBindTexture(GL_TEXTURE_2D, texture); }
static void DrawElements(const unsigned int mode, const int count, const unsigned int type, const void* indices) { glDrawElements(mode, count, type, indices); }
static void ClearNow(const unsigned int mask) { glClear(mask); }
static void BlitNow(int a, int b, int c, int d, int e, int f, int g, int h, unsigned int mask, unsigned int filter) { glBlitFramebuffer(a, b, c, d, e, f, g, h, mask, filter); }
static void CopySub(unsigned int target, int level, int xo, int yo, int x, int y, int w, int h) { glCopyTexSubImage2D(target, level, xo, yo, x, y, w, h); }
static void CopyImage(unsigned int target, int level, unsigned int format, int x, int y, int w, int h, int border) { glCopyTexImage2D(target, level, format, x, y, w, h, border); }
static void TexImage(const int format, const int w, const int h, const unsigned int pixelFormat, const unsigned int type) { glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, pixelFormat, type, nullptr); }
static void TexParameter(const unsigned int name, const int value) { glTexParameteri(GL_TEXTURE_2D, name, value); }
static void LogLine(const char* text) { printf("%s", text); }

//--- the "game"
static bool mirrored = false;
struct Moved //what the second draw has differently
{
	const float* projection = nullptr; //for the layer
	const float* characterVertices = nullptr;
};
static Moved moved;

static void GameClear(const GLbitfield mask)
{
	glClear(mask);
	if (mirrored)
		fieldmirror::Clear(mask);
}
static void GameBlit(int a, int b, int c, int d, int e, int f, int g, int h, GLbitfield mask, GLenum filter)
{
	glBlitFramebuffer(a, b, c, d, e, f, g, h, mask, filter);
	if (mirrored)
		fieldmirror::Blit(a, b, c, d, e, f, g, h, mask, filter);
}
static const unsigned short QUAD[6] = { 0, 1, 2, 1, 3, 2 };
static void GameDraw()
{
	if (mirrored)
		fieldmirror::BeforeDraw();
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, QUAD);
	if (mirrored)
		fieldmirror::Draw(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, QUAD);
}

struct Vertex
{
	float x, y, z, w;
	unsigned char color[4];
	float u, v;
};
static void Quad(Vertex out[4], const float x, const float y, const float w, const float h, const float z, const unsigned char r,
	const unsigned char g, const unsigned char b)
{
	for (int k = 0; k < 4; k++)
	{
		out[k] = { x + (k & 1) * w, y + (k >> 1) * h, z, 1.0f, { r, g, b, 255 }, static_cast<float>(k & 1), static_cast<float>(k >> 1) };
	}
}
static void Arrays(const Vertex* v)
{
	glVertexPointer(4, GL_FLOAT, sizeof(Vertex), &v->x);
	glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), v->color);
	glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), &v->u);
}

static GLuint sceneFb, sceneColor, sceneDepth, halfFb, halfColor, halfDepth, trailFb, trailColor, postFb, postColor, looseTexture;

static GLuint Texture(const int w, const int h, const GLint format, const GLenum pixelFormat, const GLenum type, const GLint filter)
{
	GLuint t = 0;
	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, format, w, h, 0, pixelFormat, type, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
	return t;
}

static void MakeGame()
{
	sceneColor = Texture(SCENE_W, SCENE_H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
	glGenRenderbuffers(1, &sceneDepth);
	glBindRenderbuffer(GL_RENDERBUFFER, sceneDepth);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, SCENE_W, SCENE_H);
	glGenFramebuffers(1, &sceneFb);
	glBindFramebuffer(GL_FRAMEBUFFER, sceneFb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sceneColor, 0);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, sceneDepth);
	halfColor = Texture(SCENE_W / 2, SCENE_H / 2, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
	halfDepth = Texture(SCENE_W / 2, SCENE_H / 2, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, GL_NEAREST);
	glGenFramebuffers(1, &halfFb);
	glBindFramebuffer(GL_FRAMEBUFFER, halfFb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, halfColor, 0);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, halfDepth, 0);
	trailColor = Texture(SCENE_W, SCENE_H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST);
	glGenFramebuffers(1, &trailFb);
	glBindFramebuffer(GL_FRAMEBUFFER, trailFb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, trailColor, 0);
	postColor = Texture(SCENE_W, SCENE_H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_LINEAR);
	glGenFramebuffers(1, &postFb);
	glBindFramebuffer(GL_FRAMEBUFFER, postFb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, postColor, 0);
	looseTexture = Texture(SCENE_W, SCENE_H, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

//the game's field projection for a 640 x 480 unit screen
static const float PROJECTION[16] = { 2.0f / 640, 0, 0, 0, 0, -2.0f / 480, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1 };
static const float WHOLE[16] = { 2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 1, 0, -1, -1, 0, 1 }; //a quad 0..1 over everything

//the trail framebuffer as it is before each test frame (never cleared by the frame itself)
static void ResetTrail()
{
	glBindFramebuffer(GL_FRAMEBUFFER, trailFb);
	glViewport(0, 0, SCENE_W, SCENE_H);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0.1f, 0.0f, 0.2f, 1.0f);
	GameClear(GL_COLOR_BUFFER_BIT); //part of what the game does: with the mirror on, its copy is cleared too
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void TexturedWhole(const GLuint texture)
{
	static Vertex whole[4];
	Quad(whole, 0, 0, 1, 1, 0.5f, 255, 255, 255);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(WHOLE);
	Arrays(whole);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, texture);
	GameDraw();
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_TEXTURE_2D);
}

//One frame of the "game". layerX: where the background layer is; character: its four vertices.
static void Frame(const float layerX, const Vertex* character, const int trailStep)
{
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	//the scene: a character first (nearer), then the layer behind it, depth tested; part of it scissored like the game's
	glBindFramebuffer(GL_FRAMEBUFFER, sceneFb);
	glViewport(0, 0, SCENE_W, SCENE_H);
	glDisable(GL_SCISSOR_TEST);
	glClearColor(0, 0, 0, 1);
	glClearStencil(0);
	GameClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	glEnable(GL_SCISSOR_TEST);
	glScissor(0, 8, SCENE_W, SCENE_H - 16);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_ALWAYS, 1, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(PROJECTION);
	Arrays(character);
	if (mirrored)
		fieldmirror::BeforeDraw();
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, QUAD);
	if (mirrored)
	{
		if (moved.characterVertices != nullptr)
			glVertexPointer(4, GL_FLOAT, sizeof(Vertex), moved.characterVertices);
		fieldmirror::Draw(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, QUAD);
		glVertexPointer(4, GL_FLOAT, sizeof(Vertex), &character->x);
	}
	glStencilFunc(GL_ALWAYS, 2, 0xFF);
	static Vertex layer[4];
	Quad(layer, layerX, 96, 256, 192, 0.9f, 40, 160, 90);
	Arrays(layer);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, QUAD);
	if (mirrored)
	{
		if (moved.projection != nullptr)
			glLoadMatrixf(moved.projection);
		fieldmirror::Draw(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, QUAD);
		glLoadMatrixf(PROJECTION);
	}
	glDisable(GL_DEPTH_TEST);
	//the half-size pass: the scene blitted down (colour and depth), something added, drawn back where the stencil says
	//"layer" (the game's transparent layers go this way)
	glDisable(GL_SCISSOR_TEST);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneFb);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, halfFb);
	GameBlit(0, 0, SCENE_W, SCENE_H, 0, 0, SCENE_W / 2, SCENE_H / 2, GL_COLOR_BUFFER_BIT, GL_LINEAR);
	GameBlit(0, 0, SCENE_W, SCENE_H, 0, 0, SCENE_W / 2, SCENE_H / 2, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	glBindFramebuffer(GL_FRAMEBUFFER, halfFb);
	glViewport(0, 0, SCENE_W / 2, SCENE_H / 2);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	glEnable(GL_DEPTH_TEST);
	glDisable(GL_STENCIL_TEST);
	static Vertex glow[4];
	Quad(glow, 200, 150, 240, 160, 0.95f, 60, 20, 0); //behind the character, in front of the layer
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(PROJECTION);
	Arrays(glow);
	GameDraw();
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_BLEND);
	glBindFramebuffer(GL_FRAMEBUFFER, sceneFb);
	glViewport(0, 0, SCENE_W, SCENE_H);
	glEnable(GL_STENCIL_TEST);
	glStencilFunc(GL_EQUAL, 2, 0xFF);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	TexturedWhole(halfColor);
	glDisable(GL_STENCIL_TEST);
	//a framebuffer the game never clears: something is added to it every frame, and it is added onto the scene
	glBindFramebuffer(GL_FRAMEBUFFER, trailFb);
	static Vertex mark[4];
	Quad(mark, 20.0f + 30.0f * trailStep, 20, 24, 24, 0.5f, 200, 200, 0);
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(PROJECTION);
	Arrays(mark);
	GameDraw();
	glBindFramebuffer(GL_FRAMEBUFFER, sceneFb);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE);
	TexturedWhole(trailColor);
	glDisable(GL_BLEND);
	//the scene copied into a loose texture, which the post pass reads (next to the scene's own texture on unit 1)
	glBindTexture(GL_TEXTURE_2D, looseTexture);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, sceneFb);
	if (trailStep % 2 == 1)
	{
		//the call the game itself uses (it imports glCopyTexImage2D, not the Sub one): the texture is made anew each time
		glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, SCENE_W, SCENE_H, 0);
		if (mirrored)
			fieldmirror::CopyTexImage(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, SCENE_W, SCENE_H, 0);
	}
	else
	{
		//DEMASTER's own (the characters' depth copy)
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, SCENE_W, SCENE_H);
		if (mirrored)
			fieldmirror::CopyTexSubImage(GL_TEXTURE_2D, 0, 0, 0, 0, 0, SCENE_W, SCENE_H);
	}
	glBindTexture(GL_TEXTURE_2D, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, postFb);
	GameClear(GL_COLOR_BUFFER_BIT);
	glActiveTexture(GL_TEXTURE1);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, sceneColor);
	glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
	glClientActiveTexture(GL_TEXTURE1);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	static Vertex whole[4];
	Quad(whole, 0, 0, 1, 1, 0.5f, 255, 255, 255);
	glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), &whole[0].u);
	glClientActiveTexture(GL_TEXTURE0);
	glActiveTexture(GL_TEXTURE0);
	TexturedWhole(looseTexture);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, 0);
	glDisable(GL_TEXTURE_2D);
	glClientActiveTexture(GL_TEXTURE1);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glClientActiveTexture(GL_TEXTURE0);
	glActiveTexture(GL_TEXTURE0);
	//the screen
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, SCREEN_W, SCREEN_H);
	glClearColor(0, 0, 0.3f, 1);
	GameClear(GL_COLOR_BUFFER_BIT);
	glViewport(32, 0, SCENE_W, SCREEN_H);
	TexturedWhole(postColor);
	glViewport(0, 0, SCREEN_W, SCREEN_H);
}

static std::vector<unsigned char> Screen()
{
	std::vector<unsigned char> pixels(static_cast<size_t>(SCREEN_W) * SCREEN_H * 4);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, SCREEN_W, SCREEN_H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
	return pixels;
}

static size_t Different(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b)
{
	size_t n = 0;
	for (size_t i = 0; i < a.size(); i += 4)
		n += memcmp(&a[i], &b[i], 4) != 0;
	return n;
}

int main()
{
	auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
	EGLDisplay display = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint major = 0, minor = 0;
	if (!eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API))
	{
		printf("no EGL\n");
		return 2;
	}
	const EGLint configAttributes[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8,
		EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
	EGLConfig config;
	EGLint configs = 0;
	eglChooseConfig(display, configAttributes, &config, 1, &configs);
	const EGLint surfaceAttributes[] = { EGL_WIDTH, SCREEN_W, EGL_HEIGHT, SCREEN_H, EGL_NONE };
	EGLSurface surface = configs ? eglCreatePbufferSurface(display, config, surfaceAttributes) : EGL_NO_SURFACE;
	const EGLint contextAttributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
	EGLContext context = configs ? eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes) : EGL_NO_CONTEXT;
	if (surface == EGL_NO_SURFACE || context == EGL_NO_CONTEXT || !eglMakeCurrent(display, surface, surface, context))
	{
		printf("no OpenGL context with a screen (pbuffer)\n");
		return 2;
	}
	MakeGame();
	fieldmirror::Host host{};
	host.drawFramebuffer = DrawFramebuffer;
	host.readFramebuffer = ReadFramebuffer;
	host.activeTexture = ActiveTexture;
	host.texture2D = Texture2D;
	host.bindTexture2D = BindTexture2D;
	host.drawElements = DrawElements;
	host.clear = ClearNow;
	host.blit = BlitNow;
	host.copyTexSubImage = CopySub;
	host.copyTexImage = CopyImage;
	host.texImage = TexImage;
	host.texParameter = TexParameter;
	host.log = LogLine;
	fieldmirror::Start(host);

	//three frames of the game in a row; frames 2 and 3 get an in-between picture
	const float layerAt[3] = { -64, -48, -40 };
	Vertex character[3][4];
	Quad(character[0], 240, 200, 40, 80, 0.3f, 220, 60, 60);
	Quad(character[1], 256, 204, 40, 80, 0.3f, 220, 60, 60);
	Quad(character[2], 268, 204, 44, 76, 0.3f, 220, 60, 60); //moved and changed shape
	for (int frame = 1; frame <= 2; frame++)
	{
		const float layerBetween = (layerAt[frame - 1] + layerAt[frame]) / 2;
		Vertex characterBetween[4];
		for (int k = 0; k < 4; k++)
		{
			characterBetween[k] = character[frame][k];
			characterBetween[k].x = (character[frame - 1][k].x + character[frame][k].x) / 2;
			characterBetween[k].y = (character[frame - 1][k].y + character[frame][k].y) / 2;
		}
		//what each picture should be: drawn plainly
		mirrored = false;
		ResetTrail();
		Frame(layerBetween, characterBetween, frame);
		const std::vector<unsigned char> wantBetween = Screen();
		ResetTrail();
		Frame(layerAt[frame], character[frame], frame);
		const std::vector<unsigned char> wantFrame = Screen();
		CHECK(Different(wantBetween, wantFrame) > 500); //the two pictures do differ
		//the game's frame with the mirror on
		float projection[16];
		memcpy(projection, PROJECTION, sizeof(projection));
		projection[12] += PROJECTION[0] * (layerBetween - layerAt[frame]);
		moved.projection = projection;
		moved.characterVertices = &characterBetween[0].x;
		fieldmirror::BeginFrame(SCREEN_W, SCREEN_H, frame == 2);
		CHECK(fieldmirror::Active());
		mirrored = true;
		ResetTrail();
		Frame(layerAt[frame], character[frame], frame);
		mirrored = false;
		const std::vector<unsigned char> gotFrameBefore = Screen();
		const bool shown = fieldmirror::ShowBetween();
		CHECK(shown);
		const std::vector<unsigned char> gotBetween = Screen();
		fieldmirror::ShowFrame();
		const std::vector<unsigned char> gotFrame = Screen();
		fieldmirror::EndFrame();
		CHECK(!fieldmirror::Broken());
		const size_t wrongBetween = Different(gotBetween, wantBetween), wrongFrame = Different(gotFrame, wantFrame);
		printf("frame %d: in-between picture %zu pixel(s) off, the game's own %zu off (of %d)\n", frame + 1, wrongBetween, wrongFrame,
			SCREEN_W * SCREEN_H);
		if (wrongBetween != 0)
		{
			int x0 = SCREEN_W, x1 = -1, y0 = SCREEN_H, y1 = -1, shownCount = 0;
			for (int y = 0; y < SCREEN_H; y++)
				for (int x = 0; x < SCREEN_W; x++)
				{
					const size_t i = (static_cast<size_t>(y) * SCREEN_W + x) * 4;
					if (memcmp(&gotBetween[i], &wantBetween[i], 4) == 0)
						continue;
					x0 = x < x0 ? x : x0; x1 = x > x1 ? x : x1; y0 = y < y0 ? y : y0; y1 = y > y1 ? y : y1;
					if (shownCount++ < 4)
						printf("  (%d, %d): got %d %d %d, want %d %d %d\n", x, y, gotBetween[i], gotBetween[i + 1], gotBetween[i + 2], wantBetween[i],
							wantBetween[i + 1], wantBetween[i + 2]);
				}
			printf("  inside x %d..%d, y %d..%d\n", x0, x1, y0, y1);
		}
		CHECK(wrongBetween == 0);
		CHECK(wrongFrame == 0);
		CHECK(Different(gotFrameBefore, wantFrame) == 0);
		CHECK(glGetError() == GL_NO_ERROR);
		//the game's bindings are what the frame left them as
		CHECK(DrawFramebuffer() == 0 && ReadFramebuffer() == 0 && ActiveTexture() == GL_TEXTURE0 && Texture2D(0) == 0 && Texture2D(1) == 0);
	}
	//the game makes its scene texture again with another filter: the copies follow within the frames it is checked in
	glBindTexture(GL_TEXTURE_2D, sceneColor);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glBindTexture(GL_TEXTURE_2D, 0);
	size_t wrong = 1;
	for (int i = 0; i < 40; i++)
	{
		mirrored = false;
		ResetTrail();
		Frame(layerAt[2], character[2], 2);
		const std::vector<unsigned char> want = Screen();
		moved = Moved();
		fieldmirror::BeginFrame(SCREEN_W, SCREEN_H, true);
		mirrored = true;
		ResetTrail();
		Frame(layerAt[2], character[2], 2);
		mirrored = false;
		wrong = 1;
		if (fieldmirror::ShowBetween())
		{
			wrong = Different(Screen(), want);
			fieldmirror::ShowFrame();
		}
		fieldmirror::EndFrame();
	}
	CHECK(wrong == 0);
	CHECK(!fieldmirror::Broken());
	fieldmirror::Forget();
	CHECK(glGetError() == GL_NO_ERROR);
	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
