//Runs DEMASTER's main.frag (the game's own program for backgrounds, battles and the world map, with DEMASTER's additions)
//on the GPU (Linux, EGL, Mesa, no window) and checks SCENE_LIGHT's part of it against the same sums worked out here:
//	1. with DEMASTER's uniforms at zero the picture is the game's: texture x vertex colour, to the level;
//	2. the multiplier works in linear light (a battle stage by night: night_level x night_tint);
//	3. an ORM map's AO darkens by the share asked for;
//	4. a picture of the glows: coverage 1 takes the darkened picture's place, coverage 0 adds, shown by how night it is.
//	5. a battle stage lit from its own space: a floor is drawn the way the game hands it to OpenGL (corners on the screen,
//	   rhw = 1 / distance), with the camera a real battle had (the registers DEMASTER logged there); the shader has to find
//	   the floor's place and direction again from that: the sun's part, one of the stage's own lights, a flat normal map.
//	6. the colour grade (LUT): two LUTs mixed, part of the grade, in main.frag and main_char.frag, as lut_grade.h has it.
//	7. EFFECT_LIGHTS: the gathering pass (two glowing quads drawn as the game does, found again where and how deep they are).
//	8. the world map's sky: DEMASTER's main_notex.frag, the game's plain colour, darkened at night as main.frag does.
//Run: scene_shader_test <DEMASTER_EXP/shaders folder>
#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include "../ff8_demaster/field_pbr.h"
#include "../ff8_demaster/scene_light.h"
#include "../ff8_demaster/lut_grade.h"

#include <algorithm>
#include <vector>

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
	//(as the game's main.vert treats corners that are already on the screen: w arrives as rhw)
	glAttachShader(program, Compile(GL_VERTEX_SHADER, "#version 120\nuniform float onScreen;\nvoid main(){vec4 pos=gl_Vertex;"
		"if(onScreen>0.5){pos.w=1.0/pos.w;pos.xyz*=pos.w;pos=gl_ProjectionMatrix*pos;}gl_Position=pos;gl_TexCoord[0]=gl_MultiTexCoord0;"
		"gl_FrontColor=gl_Color;}\n"));
	glAttachShader(program, Compile(GL_FRAGMENT_SHADER, ReadFile(folder + "main.frag")));
	glLinkProgram(program);
	GLint linked = 0;
	glGetProgramiv(program, GL_LINK_STATUS, &linked);
	Check(linked != 0, "main.frag links");
	if (!linked)
		return 1;
	//DEMASTER's fighter program: demaster_fighter.vert with main.frag, DEMASTER_FIGHTER defined after its #version line
	{
		std::string fragment = ReadFile(folder + "main.frag");
		const size_t line = fragment.find('\n');
		fragment.insert(line == std::string::npos ? fragment.size() : line + 1, "#define DEMASTER_FIGHTER 1\n");
		const GLuint fighter = glCreateProgram();
		glAttachShader(fighter, Compile(GL_VERTEX_SHADER, ReadFile(folder + "demaster_fighter.vert")));
		glAttachShader(fighter, Compile(GL_FRAGMENT_SHADER, fragment));
		glLinkProgram(fighter);
		GLint fighterLinked = 0;
		glGetProgramiv(fighter, GL_LINK_STATUS, &fighterLinked);
		Check(fighterLinked != 0 && glGetUniformLocation(fighter, "demasterSceneDraw") >= 0, "the fighter program (demaster_fighter.vert + main.frag) links");
	}
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
	//an emissive map without coverage (a world page's _E: x = 2) only adds, even where its alpha is 1
	glActiveTexture(GL_TEXTURE10);
	glBindTexture(GL_TEXTURE_2D, over);
	scenelight::Multiplier(look, 1.0, m);
	glUniform4f(at("demasterScene"), m[0], m[1], m[2], 1);
	glUniform4f(at("demasterSceneMaps"), 2, 1, 1, 0);
	draw(got);
	for (int c = 0; c < 3; c++)
		want[c] = std::min(255, level(linear(painted[c]) * m[c] + linear(glowOver[c])));
	Check(near(got, want), "an emissive map (_E) at night: its colour added to the darkened picture");
	glUniform4f(at("demasterSceneMaps"), 2, 0, 1, 0);
	glUniform4f(at("demasterScene"), 1, 1, 1, 1);
	draw(got);
	for (int c = 0; c < 3; c++)
		want[c] = static_cast<int>(std::lround(painted[c]));
	Check(near(got, want), "an emissive map by day: nothing");

	//5. the stage, lit from its own space
	{
		//the registers of the battle in frame log nofield_20261117_212324 (stage 101, the camera at rest)
		int32_t reg[32]{};
		const auto pack = [](const int a, const int b) { return static_cast<int32_t>((static_cast<uint32_t>(b & 0xFFFF) << 16) | static_cast<uint32_t>(a & 0xFFFF)); };
		reg[0] = pack(-3921, 0); reg[1] = pack(1184, 129); reg[2] = pack(4071, 429); reg[3] = pack(-1177, 449); reg[4] = -3898;
		reg[5] = -547; reg[6] = 469; reg[7] = 8796; reg[24] = 160 << 16; reg[25] = 108 << 16; reg[26] = 512;
		scenelight::Camera camera;
		Check(scenelight::CameraFromRegisters(reg, camera), "the logged registers are a camera");
		const int W = 640, H = 480;
		GLuint big = 0;
		glGenTextures(1, &big);
		glActiveTexture(GL_TEXTURE0);
		glBindTexture(GL_TEXTURE_2D, big);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, big, 0);
		glViewport(0, 0, W, H);
		glMatrixMode(GL_PROJECTION);
		const float ortho[16] = { 0.003125f, 0, 0, 0, 0, -0.00416667f, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1 }; //the game's
		glLoadMatrixf(ortho);
		glUniform1f(at("onScreen"), 1.0f);
		//a floor in the stage: y = 300 (y runs down), where that battle's ground was
		const double floorY = 300.0, corners[4][3] = { { 0, floorY, -1500 }, { 2500, floorY, -1500 }, { 2500, floorY, 1500 }, { 0, floorY, 1500 } };
		float vertices[4][4];
		for (int i = 0; i < 4; i++)
		{
			double v[3];
			scenelight::PlaceToView(camera, corners[i], v);
			Check(v[2] > 100.0, "the floor is in front of the camera");
			vertices[i][0] = static_cast<float>(2.0 * (camera.centreX + camera.distance * v[0] / v[2]));
			vertices[i][1] = static_cast<float>(2.0 * (camera.centreY + camera.distance * v[1] / v[2]));
			vertices[i][2] = 0.5f;
			vertices[i][3] = static_cast<float>(1.0 / v[2]);
		}
		const auto drawFloor = [&](uint8_t out[4], const int px, const int py)
		{
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, albedo);
			glClearColor(0, 0, 0, 0);
			glClear(GL_COLOR_BUFFER_BIT);
			glBegin(GL_QUADS);
			glColor4ub(vertex[0], vertex[1], vertex[2], 255);
			for (int i = 0; i < 4; i++)
			{
				glTexCoord2f(i == 1 || i == 2 ? 1.0f : 0.0f, i >= 2 ? 1.0f : 0.0f);
				glVertex4fv(vertices[i]);
			}
			glEnd();
			glReadPixels(px, H - 1 - py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out); //(py counts from the top, as the game does)
		};
		//a pixel in the middle of the floor, and where the floor is there (the ray through the pixel's middle)
		double middle[3] = { 1250, floorY, 0 }, view[3];
		scenelight::PlaceToView(camera, middle, view);
		const int px = static_cast<int>(2.0 * (camera.centreX + camera.distance * view[0] / view[2])), py = static_cast<int>(2.0 * (camera.centreY + camera.distance * view[1] / view[2]));
		Check(px > 4 && px < W - 4 && py > 4 && py < H - 4, "the floor's middle is on the screen");
		const double sx = (px + 0.5) / 2.0, sy = (py + 0.5) / 2.0;
		const double ray[3] = { (sx - camera.centreX) / camera.distance, (sy - camera.centreY) / camera.distance, 1.0 };
		double upView[3];
		const double up[3] = { 0, -1, 0 };
		scenelight::DirectionToView(camera, up, upView);
		//the floor's plane in view space: upView . (p - middleView) = 0, p = ray * z
		const double zAt = (upView[0] * view[0] + upView[1] * view[1] + upView[2] * view[2]) / (upView[0] * ray[0] + upView[1] * ray[1] + upView[2] * ray[2]);
		const double place[3] = { ray[0] * zAt, ray[1] * zAt, zAt };
		double normal[3] = { upView[0], upView[1], upView[2] };
		if (normal[0] * place[0] + normal[1] * place[1] + normal[2] * place[2] > 0)
			for (double& c : normal)
				c = -c;
		const auto sendCamera = [&]()
		{
			glUniform4f(at("demasterSceneProj"), 320.0f / W, 240.0f / H, static_cast<float>(H), static_cast<float>(camera.distance));
			glUniform4f(at("demasterSceneCentre"), static_cast<float>(camera.centreX), static_cast<float>(camera.centreY), 0.0f, 20000.0f);
			float rot[9], trans[3];
			for (int i = 0; i < 9; i++)
				rot[i] = static_cast<float>(camera.r[i]);
			for (int i = 0; i < 3; i++)
				trans[i] = static_cast<float>(camera.t[i]);
			glUniformMatrix3fv(at("demasterSceneRot"), 1, GL_FALSE, rot);
			glUniform3fv(at("demasterSceneTrans"), 1, trans);
		};
		sendCamera();
		glUniform4f(at("demasterScene"), 1, 1, 1, 1);
		glUniform4f(at("demasterSceneMaps"), 0, 0, 0, 0);
		glUniform4f(at("demasterSceneDraw"), 1, 0, 0, 0);
		float none[16]{};
		glUniform4fv(at("demasterSceneLightAt[0]"), 4, none);
		glUniform4fv(at("demasterSceneLightColour[0]"), 4, none);
		//the sun straight above: the floor as painted
		const auto sun = [&](const double sx, const double sy, const double sz, const float darkens, const float relief)
		{
			const double l = std::sqrt(sx * sx + sy * sy + sz * sz), stage[3] = { sx / l, sy / l, sz / l };
			double v[3];
			scenelight::DirectionToView(camera, stage, v);
			glUniform4f(at("demasterSceneSun"), static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]), darkens);
			glUniform4f(at("demasterSceneSunLight"), 1, 1, 1, relief);
			return normal[0] * v[0] + normal[1] * v[1] + normal[2] * v[2];
		};
		double towards = sun(0, -1, 0, 1.0f, 1.0f);
		drawFloor(got, px, py);
		for (int c = 0; c < 3; c++)
			want[c] = static_cast<int>(std::lround(painted[c]));
		Check(std::fabs(towards - 1.0) < 1e-3 && near(got, want), "the sun straight above the floor: as painted");
		//low over the stage's +x: the floor darkens by half of what it is turned away
		towards = sun(1, -0.2, 0.3, 0.8f, 1.0f);
		drawFloor(got, px, py);
		for (int c = 0; c < 3; c++)
			want[c] = level(linear(painted[c]) * (1.0 - 0.8 * (0.5 - 0.5 * towards)));
		Check(near(got, want), "the sun low over the stage: the floor darkens by how far it is turned away");
		printf("stage, low sun: the floor is turned %.3f towards it; got %d %d %d, want %d %d %d\n", towards, got[0], got[1], got[2], want[0], want[1], want[2]);
		//the fighters' shadows: a map with something nearer the sun takes the shadow's share of the sun off the floor; one with
		//nothing nearer leaves it; the map fitted around fighters standing on that floor
		{
			glUniform1i(at("demasterSceneShadowMap"), 13);
			towards = sun(0, -1, 0, 1.0f, 1.0f);
			const double overhead[3] = { 0, -1, 0 };
			const std::vector<float> standing = { 750.0f, static_cast<float>(floorY), -500.0f, 1750.0f, static_cast<float>(floorY) - 1800.0f, 500.0f };
			float m[16];
			Check(scenelight::FitSunShadow(overhead, standing, 100.0, m), "a shadow map fits around the fighters");
			glUniformMatrix4fv(at("demasterSceneShadowMatrix"), 1, GL_FALSE, m);
			const GLuint nearer = Texture1(0, 0, 0, 255), farther = Texture1(255, 255, 255, 255);
			glActiveTexture(GL_TEXTURE13);
			glBindTexture(GL_TEXTURE_2D, nearer);
			glUniform4f(at("demasterSceneShadowInfo"), 0.5f, 0.004f, 1.0f / 1024.0f, 1.0f);
			drawFloor(got, px, py);
			for (int c = 0; c < 3; c++)
				want[c] = level(linear(painted[c]) * 0.5);
			Check(near(got, want), "a fighter's shadow (strength 0.5) takes half the sun off the floor");
			printf("stage, in a shadow: got %d %d %d, want %d %d %d\n", got[0], got[1], got[2], want[0], want[1], want[2]);
			glActiveTexture(GL_TEXTURE13);
			glBindTexture(GL_TEXTURE_2D, farther);
			drawFloor(got, px, py);
			for (int c = 0; c < 3; c++)
				want[c] = static_cast<int>(std::lround(painted[c]));
			Check(near(got, want), "nothing nearer the sun in the map: the floor as painted");
			glUniform4f(at("demasterSceneShadowInfo"), 0, 0, 0, 0);
			glActiveTexture(GL_TEXTURE0);
			//back to the low sun for the checks below
			towards = sun(1, -0.2, 0.3, 0.8f, 1.0f);
			for (int c = 0; c < 3; c++)
				want[c] = level(linear(painted[c]) * (1.0 - 0.8 * (0.5 - 0.5 * towards)));
		}
		//a flat normal map changes nothing
		const GLuint flatMap = Texture1(128, 128, 255, 255);
		glActiveTexture(GL_TEXTURE12);
		glBindTexture(GL_TEXTURE_2D, flatMap);
		glUniform1i(at("demasterSceneN"), 12);
		glUniform4f(at("demasterSceneDraw"), 1, 1, 0, 0);
		drawFloor(got, px, py);
		Check(near(got, want), "a flat normal map: the same");
		//one that leans all the way over is not the same
		const GLuint leaning = Texture1(230, 128, 160, 255);
		glActiveTexture(GL_TEXTURE12);
		glBindTexture(GL_TEXTURE_2D, leaning);
		uint8_t leaned[4];
		drawFloor(leaned, px, py);
		Check(std::abs(leaned[0] - got[0]) > 3, "a leaning normal map: brighter or darker");
		glUniform4f(at("demasterSceneDraw"), 1, 0, 0, 0);
		//one of the stage's own lights, 500 above the floor near the pixel, with the sun's part off
		sun(0, -1, 0, 0.0f, 0.0f);
		double stagePlace[3];
		scenelight::PlaceToStage(camera, place, stagePlace);
		Check(std::fabs(stagePlace[1] - floorY) < 1.0, "the pixel's place comes back onto the floor");
		const double lightStage[3] = { stagePlace[0] + 400.0, floorY - 500.0, stagePlace[2] - 300.0 }, range = 3000.0, colour[3] = { 2.0, 1.0, 0.5 };
		double lightView[3];
		scenelight::PlaceToView(camera, lightStage, lightView);
		float lightAt[16]{}, lightColour[16]{};
		for (int c = 0; c < 3; c++)
		{
			lightAt[c] = static_cast<float>(lightView[c]);
			lightColour[c] = static_cast<float>(colour[c]);
		}
		lightAt[3] = static_cast<float>(range);
		glUniform4fv(at("demasterSceneLightAt[0]"), 4, lightAt);
		glUniform4fv(at("demasterSceneLightColour[0]"), 4, lightColour);
		drawFloor(got, px, py);
		const double to[3] = { lightView[0] - place[0], lightView[1] - place[1], lightView[2] - place[2] };
		const double away = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
		const double reach = 1.0 - away / range, facing = std::max((normal[0] * to[0] + normal[1] * to[1] + normal[2] * to[2]) / away, 0.0);
		for (int c = 0; c < 3; c++)
			want[c] = level(linear(painted[c]) * (1.0 + colour[c] * reach * reach * (0.25 + 0.75 * facing)));
		Check(std::abs(got[0] - want[0]) <= 2 && std::abs(got[1] - want[1]) <= 2 && std::abs(got[2] - want[2]) <= 2, "a light of the stage's own above the floor");
		printf("stage, own light %.0f away: got %d %d %d, want %d %d %d\n", away, got[0], got[1], got[2], want[0], want[1], want[2]);
		//the same light as an effect's (EFFECT_LIGHTS: slots 4-7): the same
		{
			float effectAt[32]{}, effectColour[32]{};
			for (int c = 0; c < 4; c++)
			{
				effectAt[5 * 4 + c] = lightAt[c];
				effectColour[5 * 4 + c] = lightColour[c];
			}
			glUniform4fv(at("demasterSceneLightAt[0]"), 8, effectAt);
			glUniform4fv(at("demasterSceneLightColour[0]"), 8, effectColour);
			uint8_t effect[4];
			drawFloor(effect, px, py);
			Check(effect[0] == got[0] && effect[1] == got[1] && effect[2] == got[2], "an effect's light (slot 5) lights the floor as a stage light does");
			glUniform4fv(at("demasterSceneLightAt[0]"), 8, effectAt);
			float none32[32]{};
			glUniform4fv(at("demasterSceneLightAt[0]"), 8, none32);
			glUniform4fv(at("demasterSceneLightColour[0]"), 8, none32);
			glUniform4fv(at("demasterSceneLightAt[0]"), 4, lightAt);
			glUniform4fv(at("demasterSceneLightColour[0]"), 4, lightColour);
		}
		//a fighter (not the stage) gets the lights but not the sun's part
		sun(1, -0.2, 0.3, 0.8f, 1.0f);
		glUniform4f(at("demasterSceneDraw"), 0, 0, 0, 0);
		uint8_t fighter[4];
		drawFloor(fighter, px, py);
		Check(std::abs(fighter[0] - got[0]) <= 1 && std::abs(fighter[1] - got[1]) <= 1, "a fighter: the lights, not the sun's darkening");
		//beyond the sky's distance nothing is lit
		glUniform4f(at("demasterSceneCentre"), static_cast<float>(camera.centreX), static_cast<float>(camera.centreY), 0.0f, 100.0f);
		drawFloor(got, px, py);
		for (int c = 0; c < 3; c++)
			want[c] = static_cast<int>(std::lround(painted[c]));
		Check(near(got, want), "beyond the sky's distance: as painted");

		//9. an ORMH map's height (its alpha): parallax. A texture whose red climbs along u and green along v shows where the
		//floor is read
		{
			glUniform4f(at("demasterSceneCentre"), static_cast<float>(camera.centreX), static_cast<float>(camera.centreY), 0.0f, 20000.0f);
			sun(0, -1, 0, 0.0f, 0.0f);
			float none32[32]{};
			glUniform4fv(at("demasterSceneLightAt[0]"), 8, none32);
			glUniform4fv(at("demasterSceneLightColour[0]"), 8, none32);
			glUniform4f(at("demasterSceneMaps"), 0, 0, 0, 0);
			glUniform4f(at("demasterSceneDraw"), 0, 0, 0, 0);
			std::vector<uint8_t> ramp(256 * 256 * 4);
			for (int y = 0; y < 256; y++)
				for (int x = 0; x < 256; x++)
				{
					uint8_t* t = &ramp[(y * 256 + x) * 4];
					t[0] = static_cast<uint8_t>(x);
					t[1] = static_cast<uint8_t>(y);
					t[2] = 128;
					t[3] = 255;
				}
			GLuint rampTexture = 0;
			glGenTextures(1, &rampTexture);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, rampTexture);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, ramp.data());
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			const auto drawRamp = [&](uint8_t out[4])
			{
				glActiveTexture(GL_TEXTURE0);
				glBindTexture(GL_TEXTURE_2D, rampTexture);
				glClearColor(0, 0, 0, 0);
				glClear(GL_COLOR_BUFFER_BIT);
				glBegin(GL_QUADS);
				glColor4ub(255, 255, 255, 255);
				for (int i = 0; i < 4; i++)
				{
					glTexCoord2f(i == 1 || i == 2 ? 1.0f : 0.0f, i >= 2 ? 1.0f : 0.0f);
					glVertex4fv(vertices[i]);
				}
				glEnd();
				glReadPixels(px, H - 1 - py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
			};
			uint8_t plain[4], level1[4], deep[4];
			glUniform4f(at("demasterSceneSurface"), 0, 0, 0, 0);
			drawRamp(plain);
			//the surface everywhere (alpha 1: an ORM map without a height, packed as ORMH): nothing moves
			const GLuint top = Texture1(255, 128, 0, 255), bottom = Texture1(255, 128, 0, 0);
			glActiveTexture(GL_TEXTURE11);
			glBindTexture(GL_TEXTURE_2D, top);
			const float depth = 300.0f;
			glUniform4f(at("demasterSceneSurface"), depth, 0, 0, 0);
			drawRamp(level1);
			Check(level1[0] == plain[0] && level1[1] == plain[1], "parallax with the height 1 everywhere: the floor is read where it was");
			//the deepest everywhere: read further along the view's slant, by the depth (u runs along the stage's x over 2500,
			//v along its z over 3000)
			glActiveTexture(GL_TEXTURE11);
			glBindTexture(GL_TEXTURE_2D, bottom);
			drawRamp(deep);
			const double length = std::sqrt(place[0] * place[0] + place[1] * place[1] + place[2] * place[2]);
			const double toEye[3] = { -place[0] / length, -place[1] / length, -place[2] / length };
			const double facing = toEye[0] * normal[0] + toEye[1] * normal[1] + toEye[2] * normal[2];
			const double along[3] = { toEye[0] - normal[0] * facing, toEye[1] - normal[1] * facing, toEye[2] - normal[2] * facing };
			const double xAxis[3] = { 1, 0, 0 }, zAxis[3] = { 0, 0, 1 };
			double gu[3], gv[3];
			scenelight::DirectionToView(camera, xAxis, gu);
			scenelight::DirectionToView(camera, zAxis, gv);
			const double du = -(along[0] * gu[0] + along[1] * gu[1] + along[2] * gu[2]) / 2500.0 * depth;
			const double dv = -(along[0] * gv[0] + along[1] * gv[1] + along[2] * gv[2]) / 3000.0 * depth;
			const double wantRed = plain[0] + du * 256.0, wantGreen = plain[1] + dv * 256.0;
			Check(std::fabs(deep[0] - wantRed) <= 1.5 && std::fabs(deep[1] - wantGreen) <= 1.5 && std::hypot(du, dv) * 256.0 > 4.0,
				"parallax at the deepest: the floor read where the view's slant takes it");
			printf("parallax: plain red %d green %d; at depth %.0f %d %d (want %.1f %.1f: moved by %.4f %.4f)\n", plain[0], plain[1], depth,
				deep[0], deep[1], wantRed, wantGreen, du, dv);
			glUniform4f(at("demasterSceneSurface"), 0, 0, 0, 0);
			glActiveTexture(GL_TEXTURE0);
		}
	}

	//6. LUT: the colour grade in main.frag (and main_char.frag) against lut_grade.h's arithmetic: a 16 x 16 picture of
	//colours drawn through two LUTs (strips, as DEMASTER uploads them), mixed, with part of the grade
	{
		glUseProgram(program);
		glUniform1f(at("onScreen"), 0.0f);
		glMatrixMode(GL_PROJECTION);
		glLoadIdentity();
		glUniform4f(at("demasterScene"), 0, 0, 0, 0);
		glUniform4f(at("demasterSceneMaps"), 0, 0, 0, 0);
		glUniform4f(at("demasterSceneProj"), 0, 0, 0, 0);
		glUniform4f(at("demasterSceneDraw"), 0, 0, 0, 0);
		Check(at("demasterLut") >= 0 && at("demasterLutA") >= 0 && at("demasterLutB") >= 0, "main.frag has LUT's uniforms");
		const int n = 33, side = 16;
		const auto make = [&](const int which)
		{
			lutgrade::Table t = lutgrade::Identity(n);
			for (int g = 0; g < n; g++)
				for (int b = 0; b < n; b++)
					for (int r = 0; r < n; r++)
					{
						const double v[3] = { r / (n - 1.0), g / (n - 1.0), b / (n - 1.0) };
						double o[3];
						if (which == 0) //warm: gold highlights, lifted violet shadows
						{
							const double y = 0.3 * v[0] + 0.6 * v[1] + 0.1 * v[2];
							o[0] = v[0] * 0.9 + 0.1 * y + 0.05 * y * y;
							o[1] = v[1] * 0.85 + 0.05 * y;
							o[2] = v[2] * 0.7 + 0.06 * (1 - y);
						}
						else //cool and flat
						{
							o[0] = 0.1 + v[0] * 0.7;
							o[1] = 0.08 + v[1] * 0.8;
							o[2] = 0.15 + std::pow(v[2], 0.8) * 0.8;
						}
						uint16_t* to = &t.rgba[(static_cast<size_t>(g) * n * n + static_cast<size_t>(b) * n + r) * 4];
						for (int c = 0; c < 3; c++)
							to[c] = static_cast<uint16_t>(std::lround(std::clamp(o[c], 0.0, 1.0) * 65535));
					}
			return t;
		};
		const lutgrade::Table warm = make(0), cool = make(1);
		const auto upload = [&](const lutgrade::Table& t)
		{
			GLuint id = 0;
			glGenTextures(1, &id);
			glBindTexture(GL_TEXTURE_2D, id);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16, t.size * t.size, t.size, 0, GL_RGBA, GL_UNSIGNED_SHORT, t.rgba.data());
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			return id;
		};
		glActiveTexture(GL_TEXTURE14);
		glBindTexture(GL_TEXTURE_2D, upload(warm));
		glActiveTexture(GL_TEXTURE15);
		glBindTexture(GL_TEXTURE_2D, upload(cool));
		//the picture: colours spread over the cube (texture x a white vertex colour = the texture)
		std::vector<uint8_t> colours(side * side * 4);
		uint32_t seed = 12345;
		for (size_t i = 0; i < colours.size(); i++)
		{
			seed = seed * 1664525u + 1013904223u;
			colours[i] = i % 4 == 3 ? 255 : static_cast<uint8_t>(seed >> 24);
		}
		GLuint picture = 0;
		glActiveTexture(GL_TEXTURE0);
		glGenTextures(1, &picture);
		glBindTexture(GL_TEXTURE_2D, picture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, side, side, 0, GL_RGBA, GL_UNSIGNED_BYTE, colours.data());
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		GLuint grid = 0;
		glGenTextures(1, &grid);
		glBindTexture(GL_TEXTURE_2D, grid);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, side, side, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, grid, 0);
		glViewport(0, 0, side, side);
		const auto drawGrid = [&](const GLuint withProgram, std::vector<uint8_t>& out)
		{
			glUseProgram(withProgram);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, picture);
			glClearColor(0, 0, 0, 0);
			glClear(GL_COLOR_BUFFER_BIT);
			glBegin(GL_QUADS);
			glColor4ub(255, 255, 255, 255);
			glTexCoord2f(0, 0); glVertex2f(-1, -1);
			glTexCoord2f(1, 0); glVertex2f(1, -1);
			glTexCoord2f(1, 1); glVertex2f(1, 1);
			glTexCoord2f(0, 1); glVertex2f(-1, 1);
			glEnd();
			out.resize(side * side * 4);
			glReadPixels(0, 0, side, side, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
		};
		//the largest difference from lut_grade.h's (in levels)
		const auto compare = [&](const std::vector<uint8_t>& got, const double mix, const double strength)
		{
			int worst = 0;
			for (int i = 0; i < side * side; i++)
			{
				const float in[3] = { colours[i * 4] / 255.0f, colours[i * 4 + 1] / 255.0f, colours[i * 4 + 2] / 255.0f };
				float out[3];
				lutgrade::Apply(warm, cool, mix, strength, in, out);
				for (int c = 0; c < 3; c++)
					worst = std::max(worst, std::abs(got[i * 4 + c] - static_cast<int>(std::lround(out[c] * 255))));
			}
			return worst;
		};
		glUniform1i(at("demasterLutA"), 14);
		glUniform1i(at("demasterLutB"), 15);
		std::vector<uint8_t> got;
		glUniform4f(at("demasterLut"), 0, 0, static_cast<float>(n), 0);
		drawGrid(program, got);
		bool same = true;
		for (int i = 0; i < side * side * 4; i++)
			same = same && std::abs(got[i] - colours[i]) <= 1;
		Check(same, "LUT off (strength 0): the picture as it is");
		glUniform4f(at("demasterLut"), 0, 1, static_cast<float>(n), 0);
		drawGrid(program, got);
		int worst = compare(got, 0, 1);
		Check(worst <= 2, "LUT: one LUT, all of it, as lut_grade.h works it out (worst " + std::to_string(worst) + " levels)");
		glUniform4f(at("demasterLut"), 0.35f, 0.7f, static_cast<float>(n), 0);
		drawGrid(program, got);
		worst = compare(got, 0.35, 0.7);
		Check(worst <= 2, "LUT: two mixed, 70% of the grade (worst " + std::to_string(worst) + " levels)");
		printf("LUT: main.frag against lut_grade.h: within %d level(s)\n", worst);
		//main_char.frag (DEMASTER's copy of the game's, with the grade): the same
		const std::string charSource = ReadFile(folder + "main_char.frag");
		Check(!charSource.empty(), "main_char.frag is there");
		if (!charSource.empty())
		{
			const GLuint chara = glCreateProgram();
			glAttachShader(chara, Compile(GL_VERTEX_SHADER, "#version 120\nvoid main(){gl_Position=gl_Vertex;gl_TexCoord[0]=gl_MultiTexCoord0;gl_FrontColor=gl_Color;}\n"));
			glAttachShader(chara, Compile(GL_FRAGMENT_SHADER, charSource));
			glLinkProgram(chara);
			GLint ok = 0;
			glGetProgramiv(chara, GL_LINK_STATUS, &ok);
			Check(ok != 0, "main_char.frag links");
			if (ok)
			{
				glUseProgram(chara);
				const auto c = [&](const char* name) { return glGetUniformLocation(chara, name); };
				glUniform1i(c("tex"), 0);
				glUniform3f(c("ambientColor"), 0, 0, 0);
				glUniform1i(c("demasterLutA"), 14);
				glUniform1i(c("demasterLutB"), 15);
				glUniform4f(c("demasterLut"), 0.35f, 0.7f, static_cast<float>(n), 0);
				drawGrid(chara, got);
				worst = compare(got, 0.35, 0.7);
				Check(worst <= 2, "LUT: main_char.frag grades the same (worst " + std::to_string(worst) + " levels)");
			}
		}
	}

	//7. EFFECT_LIGHTS: the gathering pass as DEMASTER does it (its shaders, two float targets of 40 x 30, added up): two
	//glowing quads the way the game hands them over (corners on its 640 x 480 screen, w = rhw, colour BGRA), read back and
	//found again on the grid, each where it was drawn, at its depth, in its colour
	{
		const GLuint gather = glCreateProgram();
		glAttachShader(gather, Compile(GL_VERTEX_SHADER, scenelight::GLOW_VERTEX_SHADER));
		glAttachShader(gather, Compile(GL_FRAGMENT_SHADER, scenelight::GLOW_FRAGMENT_SHADER));
		glLinkProgram(gather);
		GLint ok = 0;
		glGetProgramiv(gather, GL_LINK_STATUS, &ok);
		Check(ok != 0, "EFFECT_LIGHTS: the gathering shaders link");
		if (ok)
		{
			const int W = 40, H = 30;
			GLuint targets[2], fb = 0;
			glGenTextures(2, targets);
			for (const GLuint t : targets)
			{
				glBindTexture(GL_TEXTURE_2D, t);
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, W, H, 0, GL_RGBA, GL_FLOAT, nullptr);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			}
			glGenFramebuffers(1, &fb);
			glBindFramebuffer(GL_FRAMEBUFFER, fb);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targets[0], 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, targets[1], 0);
			const GLenum both[2] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
			glDrawBuffers(2, both);
			Check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "EFFECT_LIGHTS: the float targets");
			glViewport(0, 0, W, H);
			glDisable(GL_DEPTH_TEST);
			glEnable(GL_BLEND);
			glBlendFunc(GL_ONE, GL_ONE);
			glClearColor(0, 0, 0, 0);
			glClear(GL_COLOR_BUFFER_BIT);
			glUseProgram(gather);
			glUniform1i(glGetUniformLocation(gather, "tex"), 0);
			glUniform1f(glGetUniformLocation(gather, "demasterGlowAlpha"), 1.0f);
			glUniform1f(glGetUniformLocation(gather, "demasterGlowShare"), 1.0f);
			const GLuint white = Texture1(255, 255, 255, 255);
			glActiveTexture(GL_TEXTURE0);
			glBindTexture(GL_TEXTURE_2D, white);
			//a quad 160 x 128 around (200, 150) at depth 2500, orange (sent BGRA: blue first); one 64 x 64 around (520, 360) at 1200, blue
			const auto quad = [](const float cx, const float cy, const float hw, const float hh, const float depth, const uint8_t b, const uint8_t g, const uint8_t r)
			{
				glBegin(GL_QUADS);
				glColor4ub(b, g, r, 255);
				glVertex4f(cx - hw, cy - hh, 0, 1.0f / depth);
				glVertex4f(cx + hw, cy - hh, 0, 1.0f / depth);
				glVertex4f(cx + hw, cy + hh, 0, 1.0f / depth);
				glVertex4f(cx - hw, cy + hh, 0, 1.0f / depth);
				glEnd();
			};
			quad(200, 150, 80, 64, 2500, 20, 128, 255);
			quad(520, 360, 32, 32, 1200, 255, 80, 30);
			std::vector<float> light(W * H * 4), info(W * H * 4);
			glReadBuffer(GL_COLOR_ATTACHMENT0);
			glReadPixels(0, 0, W, H, GL_RGBA, GL_FLOAT, light.data());
			glReadBuffer(GL_COLOR_ATTACHMENT1);
			glReadPixels(0, 0, W, H, GL_RGBA, GL_FLOAT, info.data());
			scenelight::GlowGrid grid;
			grid.width = W;
			grid.height = H;
			grid.cells.resize(W * H * 5);
			for (int y = 0; y < H; y++)
				for (int x = 0; x < W; x++)
				{
					const size_t from = (static_cast<size_t>(H - 1 - y) * W + x) * 4; //as DEMASTER turns it: rows top first
					float* to = &grid.cells[(static_cast<size_t>(y) * W + x) * 5];
					to[0] = light[from];
					to[1] = light[from + 1];
					to[2] = light[from + 2];
					to[3] = info[from];
					to[4] = info[from + 1];
				}
			const std::vector<scenelight::Glow> found = scenelight::FindGlows(grid, 4, 10, 0.02); //as DEMASTER does
			Check(found.size() == 2, "EFFECT_LIGHTS: two glows found (" + std::to_string(found.size()) + ")");
			if (found.size() == 2)
			{
				const scenelight::Glow& big = found[0];
				const scenelight::Glow& small = found[1];
				printf("EFFECT_LIGHTS: glows at %.0f %.0f depth %.0f (rgb %.1f %.1f %.1f) and %.0f %.0f depth %.0f\n", big.x, big.y, big.depth,
					big.rgb[0], big.rgb[1], big.rgb[2], small.x, small.y, small.depth);
				Check(std::fabs(big.x - 200) < 12 && std::fabs(big.y - 150) < 12 && std::fabs(big.depth - 2500) < 25, "EFFECT_LIGHTS: the orange one where it was drawn, at its depth");
				Check(big.rgb[0] > big.rgb[1] && big.rgb[1] > big.rgb[2], "EFFECT_LIGHTS: the orange one is orange (the colour sent BGRA)");
				Check(std::fabs(small.x - 520) < 12 && std::fabs(small.y - 360) < 12 && std::fabs(small.depth - 1200) < 15 && small.rgb[2] > small.rgb[0],
					"EFFECT_LIGHTS: the blue one where it was drawn, at its depth, blue");
			}
			glDisable(GL_BLEND);
			glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
		}
	}

	//8. the world map's sky (DEMASTER's main_notex.frag: plain colour): the night's multiplier and the grade, nothing else
	{
		const std::string plainSource = ReadFile(folder + "main_notex.frag");
		Check(!plainSource.empty(), "main_notex.frag is there");
		Check(plainSource.find("DEMASTER (main_notex.frag)") != std::string::npos, "main_notex.frag says it is DEMASTER's (so it stays the plain-colour kind)");
		const GLuint plain = glCreateProgram();
		glAttachShader(plain, Compile(GL_VERTEX_SHADER, "#version 120\nvoid main(){gl_Position=gl_Vertex;gl_TexCoord[0]=gl_MultiTexCoord0;gl_FrontColor=gl_Color;}\n"));
		glAttachShader(plain, Compile(GL_FRAGMENT_SHADER, plainSource));
		glLinkProgram(plain);
		GLint ok = 0;
		glGetProgramiv(plain, GL_LINK_STATUS, &ok);
		Check(ok != 0, "main_notex.frag links");
		if (ok)
		{
			glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, target, 0);
			glViewport(0, 0, 4, 4);
			glUseProgram(plain);
			const auto u = [&](const char* name) { return glGetUniformLocation(plain, name); };
			const auto drawPlain = [&](uint8_t out[4])
			{
				glClearColor(0, 0, 0, 0);
				glClear(GL_COLOR_BUFFER_BIT);
				glBegin(GL_QUADS);
				glColor4ub(120, 170, 230, 255);
				glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(1, 1); glVertex2f(-1, 1);
				glEnd();
				glReadPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
			};
			const uint8_t sky[3] = { 120, 170, 230 };
			glUniform4f(u("demasterScene"), 0, 0, 0, 0);
			glUniform4f(u("demasterLut"), 0, 0, 0, 0);
			drawPlain(got);
			Check(std::abs(got[0] - sky[0]) <= 1 && std::abs(got[1] - sky[1]) <= 1 && std::abs(got[2] - sky[2]) <= 1, "main_notex.frag off: the game's colour");
			scenelight::Look nightLook;
			float nm[3];
			scenelight::Multiplier(nightLook, 1.0, nm);
			glUniform4f(u("demasterScene"), nm[0], nm[1], nm[2], 1);
			drawPlain(got);
			int skyWant[3];
			for (int c = 0; c < 3; c++)
				skyWant[c] = level(linear(sky[c]) * nm[c]);
			Check(near(got, skyWant), "main_notex.frag at night: the sky x night_level x night_tint in linear light");
			printf("world sky at night: %d %d %d -> %d %d %d (want %d %d %d)\n", sky[0], sky[1], sky[2], got[0], got[1], got[2], skyWant[0], skyWant[1], skyWant[2]);
		}
	}

	printf("%d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
