//Runs DEMASTER's main.frag (the game's own program for backgrounds, battles and the world map, with DEMASTER's additions)
//on the GPU (Linux, EGL, Mesa, no window) and checks SCENE_LIGHT's part of it against the same sums worked out here:
//	1. with DEMASTER's uniforms at zero the picture is the game's: texture x vertex colour, to the level;
//	2. the multiplier works in linear light (a battle stage by night: night_level x night_tint);
//	3. an ORM map's AO darkens by the share asked for;
//	4. a picture of the glows: coverage 1 takes the darkened picture's place, coverage 0 adds, shown by how night it is.
//Run: scene_shader_test <DEMASTER_EXP/shaders folder>
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include "../ff8_demaster/field_pbr.h"
#include "../ff8_demaster/scene_light.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

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
			char log[4096]{};
			glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
			printf("%s\n", log);
			return 0;
		}
		return shader;
	}

	GLuint Texture1(const uint8_t r, const uint8_t g, const uint8_t b, const uint8_t a)
	{
		const uint8_t pixel[4] = { r, g, b, a };
		GLuint t = 0;
		glGenTextures(1, &t);
		glBindTexture(GL_TEXTURE_2D, t);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		return t;
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
	{
		printf("scene_shader_test <DEMASTER_EXP/shaders folder>\n");
		return 2;
	}
	std::string folder = argv[1];
	if (folder.back() != '/')
		folder += '/';
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
	const GLuint program = glCreateProgram();
	//the game's vertex shader does more (its vertex types); what matters here: its colour arrives as BGRA
	glAttachShader(program, Compile(GL_VERTEX_SHADER, "#version 120\nvoid main(){gl_Position=gl_Vertex;gl_TexCoord[0]=gl_MultiTexCoord0;"
		"gl_FrontColor=gl_Color;}\n"));
	glAttachShader(program, Compile(GL_FRAGMENT_SHADER, ReadFile(folder + "main.frag")));
	glLinkProgram(program);
	GLint linked = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	Check(linked != 0, "main.frag links");
	if (!linked)
		return 1;
	glUseProgram(program);
	const auto at = [&](const char* name) { return glGetUniformLocation(program, name); };
	Check(at("demasterScene") >= 0 && at("demasterSceneMaps") >= 0 && at("demasterSceneEC") >= 0 && at("demasterSceneORM") >= 0,
		"main.frag has SCENE_LIGHT's uniforms");
	glUniform1i(at("tex"), 0);
	glUniform1i(at("demasterSceneEC"), 10);
	glUniform1i(at("demasterSceneORM"), 11);

	GLuint framebuffer = 0, target = 0;
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glGenTextures(1, &target);
	glBindTexture(GL_TEXTURE_2D, target);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
	glViewport(0, 0, 4, 4);

	const uint8_t tex[4] = { 200, 150, 90, 255 }, vertex[3] = { 255, 204, 128 };
	const GLuint albedo = Texture1(tex[0], tex[1], tex[2], tex[3]);
	const uint8_t ormPixel[3] = { 128, 200, 0 }, glowOver[4] = { 255, 220, 90, 255 }, glowAdd[4] = { 40, 30, 10, 0 };
	const GLuint orm = Texture1(ormPixel[0], ormPixel[1], ormPixel[2], 255);
	const GLuint over = Texture1(glowOver[0], glowOver[1], glowOver[2], glowOver[3]);
	const GLuint added = Texture1(glowAdd[0], glowAdd[1], glowAdd[2], glowAdd[3]);

	const auto draw = [&](uint8_t out[4])
	{
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, albedo);
		glClearColor(0, 0, 0, 0);
		glClear(GL_COLOR_BUFFER_BIT);
		glBegin(GL_QUADS);
		glColor4ub(vertex[0], vertex[1], vertex[2], 255);
		glTexCoord2f(0, 0); glVertex2f(-1, -1);
		glTexCoord2f(1, 0); glVertex2f(1, -1);
		glTexCoord2f(1, 1); glVertex2f(1, 1);
		glTexCoord2f(0, 1); glVertex2f(-1, 1);
		glEnd();
		glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
	};
	const auto linear = [](const double srgb8) { return static_cast<double>(fieldpbr::SrgbToLinear(static_cast<float>(srgb8 / 255.0))); };
	const auto level = [](const double lin) { return static_cast<int>(std::lround(255.0 * fieldpbr::LinearToSrgb(static_cast<float>(lin)))); };
	const auto near = [](const uint8_t got[4], const int want[3]) { return std::abs(got[0] - want[0]) <= 1 && std::abs(got[1] - want[1]) <= 1 && std::abs(got[2] - want[2]) <= 1; };
	double painted[3]; //the game's picture: texture x vertex colour, as levels
	for (int c = 0; c < 3; c++)
		painted[c] = tex[c] * vertex[c] / 255.0;
	uint8_t got[4];

	//1. off: the game's picture
	glUniform4f(at("demasterScene"), 0, 0, 0, 0);
	glUniform4f(at("demasterSceneMaps"), 0, 0, 0, 0);
	draw(got);
	int want[3] = { static_cast<int>(std::lround(painted[0])), static_cast<int>(std::lround(painted[1])), static_cast<int>(std::lround(painted[2])) };
	Check(near(got, want) && got[3] == 255, "off: texture x vertex colour");
	//on with 1 1 1: still the game's picture
	glUniform4f(at("demasterScene"), 1, 1, 1, 1);
	draw(got);
	Check(near(got, want), "on, multiplier 1 1 1: unchanged");

	//2. the night: scene_light.h's multiplier at full night, in linear light
	scenelight::Look look;
	float m[3];
	scenelight::Multiplier(look, 1.0, m);
	glUniform4f(at("demasterScene"), m[0], m[1], m[2], 1);
	draw(got);
	for (int c = 0; c < 3; c++)
		want[c] = level(linear(painted[c]) * m[c]);
	Check(near(got, want), "night: x night_level x night_tint in linear light");
	printf("night: painted %d %d %d -> %d %d %d (want %d %d %d)\n", static_cast<int>(painted[0]), static_cast<int>(painted[1]), static_cast<int>(painted[2]),
		got[0], got[1], got[2], want[0], want[1], want[2]);

	//3. AO at 0.5 of the map's
	glActiveTexture(GL_TEXTURE11);
	glBindTexture(GL_TEXTURE_2D, orm);
	glUniform4f(at("demasterScene"), 1, 1, 1, 1);
	glUniform4f(at("demasterSceneMaps"), 0, 0, 0, 0.5f);
	draw(got);
	for (int c = 0; c < 3; c++)
		want[c] = level(linear(painted[c]) * (1.0 - 0.5 * (1.0 - ormPixel[0] / 255.0)));
	Check(near(got, want), "AO: half of the map's");

	//4. the glows at night, brightness 1: coverage 1 replaces
	glActiveTexture(GL_TEXTURE10);
	glBindTexture(GL_TEXTURE_2D, over);
	glUniform4f(at("demasterScene"), m[0], m[1], m[2], 1);
	glUniform4f(at("demasterSceneMaps"), 1, 1, 1, 0);
	draw(got);
	for (int c = 0; c < 3; c++)
		want[c] = glowOver[c];
	Check(near(got, want), "glow with coverage 1 at night: the glow's own colour");
	//half shown (dusk): half the darkened picture, half the glow
	scenelight::Multiplier(look, 0.5, m);
	glUniform4f(at("demasterScene"), m[0], m[1], m[2], 1);
	glUniform4f(at("demasterSceneMaps"), 1, 0.5f, 1, 0);
	draw(got);
	for (int c = 0; c < 3; c++)
		want[c] = level(linear(painted[c]) * m[c] * 0.5 + linear(glowOver[c]) * 0.5);
	Check(near(got, want), "glow with coverage 1 at dusk: half and half");
	//coverage 0: added, twice as bright
	glActiveTexture(GL_TEXTURE10);
	glBindTexture(GL_TEXTURE_2D, added);
	scenelight::Multiplier(look, 1.0, m);
	glUniform4f(at("demasterScene"), m[0], m[1], m[2], 1);
	glUniform4f(at("demasterSceneMaps"), 1, 1, 2, 0);
	draw(got);
	for (int c = 0; c < 3; c++)
		want[c] = level(linear(painted[c]) * m[c] + linear(glowAdd[c]) * 2.0);
	Check(near(got, want), "glow with coverage 0: added to the darkened picture, times its brightness");
	//by day nothing of it shows
	glUniform4f(at("demasterScene"), 1, 1, 1, 1);
	glUniform4f(at("demasterSceneMaps"), 1, 0, 1, 0);
	draw(got);
	for (int c = 0; c < 3; c++)
		want[c] = static_cast<int>(std::lround(painted[c]));
	Check(near(got, want), "glow by day: nothing");

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
