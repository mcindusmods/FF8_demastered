//Runs DEMASTER's background shader (demaster_field.vert/.frag) and the tiles' nearest-point pass (in
//texturepatch_v2_fieldPbr.cpp) on the GPU (Linux, EGL, Mesa, no window), the way DEMASTER draws a field:
//	1. the nearest-point pass writes each tile's nearest covered distance into the cells (masked by the albedo's coverage; a
//	   sliver of something nearer, under a tenth of the tile, doesn't count);
//	2. a character behind a door tile stays hidden (the door is flat: its pixels are where the tile's depth says), also with
//	   a sliver of the nearer door frame in the door's tile;
//	3. a character between a pillar and the floor in the pillar's tile shows where the floor is, not where the pillar is;
//	   behind the floor, the floor hides it;
//	4. tiles keep the game's depth exactly: a far tile drawn after a near one never covers it, and the depth buffer holds
//	   the tiles' own depth (nothing writes gl_FragDepth).
//Build (from tests/):
//	g++ -std=c++20 -O1 field_depth_test.cpp -lEGL -lGL -o field_depth_test
//Run: field_depth_test <DEMASTER_EXP/shaders folder> <ff8_demaster folder>

#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	int checks = 0, failures = 0;

	void Check(const bool ok, const std::string& what)
	{
		checks++;
		if (!ok)
		{
			failures++;
			printf("FAILED: %s\n", what.c_str());
		}
	}

	std::string ReadFile(const std::string& path)
	{
		std::ifstream in(path, std::ios::binary);
		std::ostringstream out;
		out << in.rdbuf();
		return out.str();
	}

	GLuint Compile(const GLenum type, const std::string& source, const std::string& name)
	{
		const GLuint shader = glCreateShader(type);
		const char* text = source.c_str();
		glShaderSource(shader, 1, &text, nullptr);
		glCompileShader(shader);
		GLint ok = 0;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
		if (!ok)
		{
			char log[4096]{};
			glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
			printf("%s: %s\n", name.c_str(), log);
			return 0;
		}
		return shader;
	}

	GLuint Link(const std::string& vs, const std::string& fs, const std::string& name)
	{
		const GLuint v = Compile(GL_VERTEX_SHADER, vs, name + " (vertex)"), f = Compile(GL_FRAGMENT_SHADER, fs, name + " (fragment)");
		if (!v || !f)
			return 0;
		const GLuint p = glCreateProgram();
		glAttachShader(p, v);
		glAttachShader(p, f);
		glLinkProgram(p);
		GLint ok = 0;
		glGetProgramiv(p, GL_LINK_STATUS, &ok);
		if (!ok)
		{
			char log[4096]{};
			glGetProgramInfoLog(p, sizeof(log), nullptr, log);
			printf("%s: %s\n", name.c_str(), log);
			return 0;
		}
		return p;
	}

	//the nearest-point pass's shaders, as texturepatch_v2_fieldPbr.cpp has them
	bool NearestSources(const std::string& cpp, std::string& vertex, std::string& fragment)
	{
		const size_t v = cpp.find("NEAREST_VERT = \"");
		const size_t f = cpp.find("NEAREST_FRAG = R\"(");
		if (v == std::string::npos || f == std::string::npos)
			return false;
		const size_t vStart = v + strlen("NEAREST_VERT = \""), vEnd = cpp.find("\";", vStart);
		const size_t fStart = f + strlen("NEAREST_FRAG = R\"("), fEnd = cpp.find(")\";", fStart);
		if (vEnd == std::string::npos || fEnd == std::string::npos)
			return false;
		vertex = cpp.substr(vStart, vEnd - vStart);
		for (size_t at; (at = vertex.find("\\n")) != std::string::npos;)
			vertex.replace(at, 2, "\n");
		fragment = cpp.substr(fStart, fEnd - fStart);
		return true;
	}

	GLuint Texture(const int width, const int height, const std::vector<uint8_t>& rgba)
	{
		GLuint t = 0;
		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
		return t;
	}

	//the game's depth buffer value for a depth (1 / rhw)
	float GameZ(const double depth)
	{
		return static_cast<float>(0.0015 + 0.9985 * depth / 65536.0);
	}

	//a TLVERTEX quad over the game's screen rectangle, uv over [u0, u1] x [v0, v1]
	void Quad(const float x0, const float y0, const float x1, const float y1, const double depth, const float u0, const float v0, const float u1,
		const float v1)
	{
		const float z = GameZ(depth), w = static_cast<float>(1.0 / depth);
		glBegin(GL_TRIANGLE_STRIP);
		glColor4ub(255, 255, 255, 255);
		glTexCoord2f(u0, v0);
		glVertex4f(x0, y0, z, w);
		glTexCoord2f(u1, v0);
		glVertex4f(x1, y0, z, w);
		glTexCoord2f(u0, v1);
		glVertex4f(x0, y1, z, w);
		glTexCoord2f(u1, v1);
		glVertex4f(x1, y1, z, w);
		glEnd();
	}
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		printf("field_depth_test <DEMASTER_EXP/shaders folder> <ff8_demaster folder>\n");
		return 2;
	}
	std::string ours = argv[1], source = argv[2];
	if (ours.back() != '/')
		ours += '/';
	if (source.back() != '/')
		source += '/';
	auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
	EGLDisplay display = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : eglGetDisplay(EGL_DEFAULT_DISPLAY);
	EGLint major = 0, minor = 0;
	if (!eglInitialize(display, &major, &minor) || !eglBindAPI(EGL_OPENGL_API))
	{
		printf("no EGL\n");
		return 2;
	}
	const EGLint configAttributes[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
	EGLConfig config;
	EGLint configs = 0;
	eglChooseConfig(display, configAttributes, &config, 1, &configs);
	EGLContext context = eglCreateContext(display, configs ? config : nullptr, EGL_NO_CONTEXT, nullptr);
	if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
	{
		printf("no GL context\n");
		return 2;
	}
	printf("OpenGL: %s, %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

	const GLuint field = Link(ReadFile(ours + "demaster_field.vert"), ReadFile(ours + "demaster_field.frag"), "demaster_field");
	std::string nearestVertex, nearestFragment;
	Check(NearestSources(ReadFile(source + "texturepatch_v2_fieldPbr.cpp"), nearestVertex, nearestFragment),
		"the nearest-point pass's shaders are in texturepatch_v2_fieldPbr.cpp");
	const GLuint nearest = Link(nearestVertex, nearestFragment, "nearest-point pass");
	Check(field != 0, "demaster_field links");
	Check(nearest != 0, "the nearest-point pass links");
	if (!field || !nearest)
	{
		printf("%d checks, %d failures\n", checks, failures);
		return 1;
	}
	const std::string frag = ReadFile(ours + "demaster_field.frag");
	Check(frag.find("gl_FragDepth") == std::string::npos, "demaster_field.frag never writes gl_FragDepth");

	//--- the page: 3 x 1 cells of 16 x 16 texels at 4x. Cell 0: a door (red, flat at 0.329). Cell 1: a pillar (green, 0.298)
	//on its left, the floor behind it on its right (blue, 0.6), and a corner the layer doesn't cover (nearer still: 0.1).
	//Cell 2: a door (magenta, 0.475) with a sliver of its frame (yellow, 0.329) down its left edge
	const int PW = 192, PH = 64;
	std::vector<uint8_t> albedo(PW * PH * 4), nd(PW * PH * 4), lf(PW * PH * 4, 255);
	for (int y = 0; y < PH; y++)
		for (int x = 0; x < PW; x++)
		{
			uint8_t* a = &albedo[(y * PW + x) * 4];
			uint8_t* n = &nd[(y * PW + x) * 4];
			n[0] = 128;
			n[1] = 128;
			n[2] = 255;
			a[3] = 255;
			if (x < 64)
			{
				a[0] = 255, a[1] = 0, a[2] = 0;
				n[3] = 171; //1 - 0.329
			}
			else if (x >= 128)
			{
				const bool frame = x < 132;
				a[0] = 255, a[1] = frame ? 255 : 0, a[2] = frame ? 0 : 255;
				n[3] = frame ? 171 : 134; //1 - 0.329, 1 - 0.475
			}
			else if (x < 96)
			{
				a[0] = 0, a[1] = 255, a[2] = 0;
				n[3] = 179; //1 - 0.298
			}
			else
			{
				a[0] = 0, a[1] = 0, a[2] = 255;
				n[3] = 102; //1 - 0.6
			}
			if (x >= 120 && x < 128 && y < 8)
			{
				a[3] = 0;
				n[3] = 230; //0.1, not covered: not the tile's nearest
			}
		}
	glActiveTexture(GL_TEXTURE0);
	const GLuint albedoTexture = Texture(PW, PH, albedo);
	const GLuint ndTexture = Texture(PW, PH, nd);
	const GLuint lfTexture = Texture(PW, PH, lf);
	//cells: block 1 (canvas x, y, flags PBR + normal, lightmap scale), block 2 (Z, layer, nearest, 0)
	const float cellData[3 * 2 * 4] = { 100, 60, 3, 1, 116, 60, 3, 1, 132, 60, 3, 1, 337, 0, 0, 0, 174, 0, 0, 0, 337, 0, 0, 0 };
	GLuint cells = 0;
	glGenTextures(1, &cells);
	glBindTexture(GL_TEXTURE_2D, cells);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 3, 2, 0, GL_RGBA, GL_FLOAT, cellData);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	const double a = 1.0, b = -1.0, zoom = 290, originX = 168, originY = 120; //distance = 1 - value

	//--- 1. the nearest-point pass, as MeasureNearest runs it
	{
		GLuint framebuffer = 0;
		glGenFramebuffers(1, &framebuffer);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, cells, 0);
		Check(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "the cells texture can be drawn into");
		glViewport(0, 1, 3, 1);
		glDisable(GL_BLEND);
		glDisable(GL_DEPTH_TEST);
		glColorMask(GL_FALSE, GL_FALSE, GL_TRUE, GL_FALSE);
		glUseProgram(nearest);
		glUniform1i(glGetUniformLocation(nearest, "albedo"), 0);
		glUniform1i(glGetUniformLocation(nearest, "nd"), 1);
		glUniform4f(glGetUniformLocation(nearest, "cellInfo"), 3, 1, 64, 64);
		glUniform4f(glGetUniformLocation(nearest, "pageInfo"), 1.0f / PW, 1.0f / PH, 0, 0);
		glUniform4f(glGetUniformLocation(nearest, "depthModel"), static_cast<float>(a), static_cast<float>(b), 0, 0.01f);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, albedoTexture);
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_2D, ndTexture);
		glBegin(GL_TRIANGLE_STRIP);
		glVertex2f(-1, -1);
		glVertex2f(1, -1);
		glVertex2f(-1, 1);
		glVertex2f(1, 1);
		glEnd();
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		float back[24];
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, cells);
		glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, back);
		char text[200];
		snprintf(text, sizeof(text), "the door tile's nearest point is 0.329 (got %.4f), the other channels kept (%.0f, %.0f)", back[14], back[12], back[13]);
		Check(std::fabs(back[14] - (1.0 - 171 / 255.0)) < 0.003 && back[12] == 337 && back[13] == 0, text);
		snprintf(text, sizeof(text), "the pillar tile's nearest covered point is 0.298, not the uncovered corner's 0.1 (got %.4f)", back[18]);
		Check(std::fabs(back[18] - (1.0 - 179 / 255.0)) < 0.003, text);
		snprintf(text, sizeof(text), "the second door's nearest point is the door's 0.475, not its frame's sliver (got %.4f)", back[22]);
		Check(std::fabs(back[22] - (1.0 - 134 / 255.0)) < 0.003, text);
		Check(back[0] == 100 && back[2] == 3 && back[8] == 132, "block 1 of the cells is left as it was");
	}
	const double doorNear = 1.0 - 171 / 255.0, pillarNear = 1.0 - 179 / 255.0;

	//--- the game's screen: 640 x 480 into 128 x 96, colour and depth
	const int W = 128, H = 96;
	GLuint target = 0, colour = 0, depthBuffer = 0, charDepth = 0;
	glGenFramebuffers(1, &target);
	glBindFramebuffer(GL_FRAMEBUFFER, target);
	glGenTextures(1, &colour);
	glBindTexture(GL_TEXTURE_2D, colour);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colour, 0);
	glGenRenderbuffers(1, &depthBuffer);
	glBindRenderbuffer(GL_RENDERBUFFER, depthBuffer);
	glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
	glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depthBuffer);
	Check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "the screen can be drawn into");
	glGenTextures(1, &charDepth);
	glBindTexture(GL_TEXTURE_2D, charDepth);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, W, H, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_NONE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
	glViewport(0, 0, W, H);
	glMatrixMode(GL_PROJECTION);
	const float projection[16] = { 0.003125f, 0, 0, 0, 0, -1.0f / 240.0f, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1 };
	glLoadMatrixf(projection);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	//the field program, as SetUniforms sets it
	glUseProgram(field);
	const auto at = [&](const char* name) { return glGetUniformLocation(field, name); };
	glUniform1i(at("tex"), 0);
	glUniform1i(at("demasterLF"), 1);
	glUniform1i(at("demasterND"), 2);
	glUniform1i(at("demasterORME"), 3);
	glUniform1i(at("demasterCells"), 4);
	glUniform1i(at("demasterRoom"), 5);
	glUniform1i(at("demasterCapture"), 6);
	glUniform1i(at("demasterShadowMap"), 7);
	glUniform1i(at("demasterCharDepth"), 8);
	glUniform4f(at("demasterTileClamp"), 0, 0, 0, 0);
	glUniform4f(at("demasterPbr"), 1, 0, 3, 1);
	glUniform4f(at("demasterSpace"), static_cast<float>(zoom), static_cast<float>(originX), static_cast<float>(originY), 0);
	glUniform4f(at("demasterDepth"), static_cast<float>(a), static_cast<float>(b), 0.01f, 0);
	glUniform1i(at("demasterLightCount"), 0);
	glUniform4f(at("demasterStepAside"), 1, static_cast<float>(0.9985 / 65536.0 * 0.5), 1.0f / W, 1.0f / H);
	glUniform4f(at("demasterQuality"), 6, 3, 4, 0);
	glUniform4f(at("demasterSky"), 0, 0, 0, 0);
	glUniform4f(at("demasterRoomSize"), 336, 240, 0, 0);
	glUniform4f(at("demasterFeatures"), 0, 0, 0, 0);
	glUniform1i(at("demasterCharCount"), 0);
	glUniform1i(at("demasterShadowCount"), 0);
	glUseProgram(0);
	const GLuint units[9] = { albedoTexture, lfTexture, ndTexture, 0, cells, 0, 0, 0, charDepth };
	for (int u = 8; u >= 0; u--)
	{
		glActiveTexture(GL_TEXTURE0 + u);
		glBindTexture(GL_TEXTURE_2D, units[u]);
	}

	const double PER_VIEW = 16384.0;
	//a frame: a white character (depth = its distance x 16384) over the screen rectangle, its depth copied, then a tile
	const auto frame = [&](const double characterDistance, const float cx0, const float cx1, const int cell, const double tileDepth, const bool second,
		const double secondDepth)
	{
		glBindFramebuffer(GL_FRAMEBUFFER, target);
		glClearColor(0, 0, 0, 1);
		glClearDepth(1);
		glDepthMask(GL_TRUE);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LEQUAL);
		glDisable(GL_BLEND);
		glEnable(GL_ALPHA_TEST);
		glAlphaFunc(GL_GEQUAL, 0.439f);
		glActiveTexture(GL_TEXTURE0);
		if (characterDistance > 0)
		{
			glUseProgram(0);
			glDisable(GL_TEXTURE_2D);
			const float z = GameZ(characterDistance * PER_VIEW);
			glBegin(GL_TRIANGLE_STRIP);
			glColor4ub(255, 255, 255, 255);
			glVertex4f(cx0, 160, z, 1);
			glVertex4f(cx1, 160, z, 1);
			glVertex4f(cx0, 320, z, 1);
			glVertex4f(cx1, 320, z, 1);
			glEnd();
		}
		//the characters' depth, as CopyCharacterDepth takes it
		glBindTexture(GL_TEXTURE_2D, charDepth);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, target);
		glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, W, H);
		glBindTexture(GL_TEXTURE_2D, albedoTexture);
		glUseProgram(field);
		const float u0 = cell / 3.0f, u1 = (cell + 1) / 3.0f;
		Quad(200, 140, 400, 340, tileDepth, u0, 0, u1, 1);
		if (second)
			Quad(200, 140, 400, 340, secondDepth, 1 / 3.0f, 0, 2 / 3.0f, 1);
		glUseProgram(0);
	};
	const auto pixel = [&](const float sx, const float sy, uint8_t rgb[3], float* depth)
	{
		const int x = static_cast<int>(sx * W / 640.0f), y = H - 1 - static_cast<int>(sy * H / 480.0f);
		uint8_t p[4];
		glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p);
		memcpy(rgb, p, 3);
		if (depth != nullptr)
			glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, depth);
	};
	const auto is = [](const uint8_t* rgb, const int r, const int g, const int bl)
	{
		return std::abs(rgb[0] - r) < 40 && std::abs(rgb[1] - g) < 40 && std::abs(rgb[2] - bl) < 40;
	};
	uint8_t rgb[3];
	float depth = 0;
	char text[300];

	//--- 2. the door: Zell behind it (0.40) stays hidden
	frame(0.40, 220, 380, 0, doorNear * PER_VIEW, false, 0);
	pixel(300, 240, rgb, &depth);
	snprintf(text, sizeof(text), "a character behind a door tile is hidden by the door (got %d %d %d)", rgb[0], rgb[1], rgb[2]);
	Check(is(rgb, 255, 0, 0), text);
	snprintf(text, sizeof(text), "the depth buffer holds the door tile's own depth (%.6f, the tile's %.6f)", depth, 0.5 + 0.5 * GameZ(doorNear * PER_VIEW));
	Check(std::fabs(depth - (0.5 + 0.5 * GameZ(doorNear * PER_VIEW))) < 2e-6, text);
	//in front of the door, the character is drawn over it (the door fails the depth test there)
	frame(0.30, 220, 380, 0, doorNear * PER_VIEW, false, 0);
	pixel(300, 240, rgb, nullptr);
	Check(is(rgb, 255, 255, 255), "a character in front of a door tile is seen");

	//the door with a sliver of its frame: the tile's depth is the frame's (as the game has it); Zell behind the door stays
	//hidden even though the frame is nearer than the door
	frame(0.40, 220, 380, 2, doorNear * PER_VIEW, false, 0);
	pixel(320, 240, rgb, nullptr);
	snprintf(text, sizeof(text), "a character behind a door with a sliver of nearer frame in its tile stays hidden (got %d %d %d)", rgb[0], rgb[1], rgb[2]);
	Check(is(rgb, 255, 0, 255), text);

	//--- 3. the pillar's tile: the floor on its right (0.6) is far behind its nearest point (0.298)
	frame(0.45, 200, 400, 1, pillarNear * PER_VIEW, false, 0);
	pixel(250, 240, rgb, nullptr);
	snprintf(text, sizeof(text), "the pillar hides a character behind it (got %d %d %d)", rgb[0], rgb[1], rgb[2]);
	Check(is(rgb, 0, 255, 0), text);
	pixel(350, 240, rgb, &depth);
	snprintf(text, sizeof(text), "the floor in the pillar's tile steps aside for a character in front of it (got %d %d %d)", rgb[0], rgb[1], rgb[2]);
	Check(is(rgb, 255, 255, 255), text);
	snprintf(text, sizeof(text), "where it steps aside, the character's depth stays (%.6f)", depth);
	Check(std::fabs(depth - (0.5 + 0.5 * GameZ(0.45 * PER_VIEW))) < 2e-6, text);
	frame(0.75, 200, 400, 1, pillarNear * PER_VIEW, false, 0);
	pixel(350, 240, rgb, nullptr);
	snprintf(text, sizeof(text), "the floor hides a character behind the floor (got %d %d %d)", rgb[0], rgb[1], rgb[2]);
	Check(is(rgb, 0, 0, 255), text);
	//no character: nothing steps aside
	frame(0, 0, 0, 1, pillarNear * PER_VIEW, false, 0);
	pixel(350, 240, rgb, nullptr);
	Check(is(rgb, 0, 0, 255), "with no character there, the floor is drawn");

	//--- 4. the order: the door first, then a far tile over it: the door stays, with its depth
	frame(0, 0, 0, 0, doorNear * PER_VIEW, true, 65000);
	pixel(300, 240, rgb, &depth);
	snprintf(text, sizeof(text), "a far tile drawn after a near one doesn't cover it (got %d %d %d)", rgb[0], rgb[1], rgb[2]);
	Check(is(rgb, 255, 0, 0), text);
	Check(std::fabs(depth - (0.5 + 0.5 * GameZ(doorNear * PER_VIEW))) < 2e-6, "and the depth stays the near tile's");
	//the far tile first, then the near one: the near one covers it
	frame(0, 0, 0, 1, 65000, false, 0);
	glUseProgram(field);
	glBindTexture(GL_TEXTURE_2D, albedoTexture);
	Quad(200, 140, 400, 340, doorNear * PER_VIEW, 0, 0, 1 / 3.0f, 1);
	glUseProgram(0);
	pixel(300, 240, rgb, nullptr);
	Check(is(rgb, 255, 0, 0), "a near tile drawn after a far one covers it");

	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
