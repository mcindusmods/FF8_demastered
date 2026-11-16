//Runs DEMASTER's character shaders on the GPU (Linux, EGL, no window) next to the game's own main.vert + main_char.frag:
//	1. every DEMASTER program compiles and links (demaster_chara + demaster_shadow, demaster_field);
//	2. with the room's light off (demasterModel.w = 0) demaster_chara draws exactly what main_char.frag draws, for random
//	   triangles, colours, darkening values and a cut-out texture;
//	3. lit by a flat room (no light grid) with white vertices and the darkening kept, it draws the texture as the game does
//	   (within 1 step: the lighting is done in linear light);
//	4. a picture of the model through a box matrix (the reflections' cards) covers what it should, from the front and from
//	   behind (mirrored);
//	5. instancing (DEMASTER_INSTANCED, as DEMASTER builds the programs when the driver has GL_ARB_draw_instanced): four
//	   pictures and four shadow maps of a model in one draw each, each into its tile, match drawing them one by one with a
//	   viewport per tile (lit, mirrored and not, ortho and perspective cameras; the shadow maps' depth too).
//Build (from tests/):
//	g++ -std=c++20 -O1 chara_shader_test.cpp -lEGL -lGL -o chara_shader_test
//Run: chara_shader_test <DEMASTER_EXP\shaders folder> <folder with the game's main.vert and main_char.frag>

#define GL_GLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <utility>
#include <sstream>
#include <string>
#include <vector>

namespace
{
	int checks = 0, failures = 0;

	void Check(const bool ok, const char* what)
	{
		checks++;
		if (ok && getenv("SHOW_ALL"))
			printf("ok: %s\n", what);
		if (!ok)
		{
			failures++;
			printf("FAILED: %s\n", what);
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
			char log[2048]{};
			glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
			printf("%s: %s\n", name.c_str(), log);
			return 0;
		}
		return shader;
	}

	//vertexDefines: put after the vertex shader's #version line (as DEMASTER's BuildProgram does)
	GLuint Link(const std::string& vertexPath, const std::string& fragmentPath, const char* vertexDefines = nullptr)
	{
		std::string vs = ReadFile(vertexPath);
		const std::string fs = ReadFile(fragmentPath);
		if (vertexDefines != nullptr && vs.compare(0, 8, "#version") == 0)
			vs.insert(vs.find('\n') + 1, vertexDefines);
		if (vs.empty() || fs.empty())
		{
			printf("missing %s or %s\n", vertexPath.c_str(), fragmentPath.c_str());
			return 0;
		}
		const GLuint v = Compile(GL_VERTEX_SHADER, vs, vertexPath), f = Compile(GL_FRAGMENT_SHADER, fs, fragmentPath);
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
			char log[2048]{};
			glGetProgramInfoLog(p, sizeof(log), nullptr, log);
			printf("link %s + %s: %s\n", vertexPath.c_str(), fragmentPath.c_str(), log);
			return 0;
		}
		return p;
	}

	struct Vertex
	{
		float pos[4];
		uint8_t color[4]; //blue, green, red, alpha
		float uv[2];
		float view[3];
		float normal[3];
	};

	constexpr int W = 320, H = 240;

	struct Target
	{
		GLuint framebuffer = 0, color = 0, depth = 0;
	};

	Target MakeTarget()
	{
		Target t;
		glGenFramebuffers(1, &t.framebuffer);
		glBindFramebuffer(GL_FRAMEBUFFER, t.framebuffer);
		glGenTextures(1, &t.color);
		glBindTexture(GL_TEXTURE_2D, t.color);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t.color, 0);
		glGenRenderbuffers(1, &t.depth);
		glBindRenderbuffer(GL_RENDERBUFFER, t.depth);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, W, H);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t.depth);
		return t;
	}

	std::vector<uint8_t> Read()
	{
		std::vector<uint8_t> rgba(static_cast<size_t>(W) * H * 4);
		glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
		return rgba;
	}

	void Arrays(const std::vector<Vertex>& v, const bool demaster)
	{
		glEnableClientState(GL_VERTEX_ARRAY);
		glVertexPointer(4, GL_FLOAT, sizeof(Vertex), v[0].pos);
		glEnableClientState(GL_COLOR_ARRAY);
		glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vertex), v[0].color);
		glClientActiveTexture(GL_TEXTURE0);
		glEnableClientState(GL_TEXTURE_COORD_ARRAY);
		glTexCoordPointer(2, GL_FLOAT, sizeof(Vertex), v[0].uv);
		glClientActiveTexture(GL_TEXTURE1);
		if (demaster)
		{
			glEnableClientState(GL_TEXTURE_COORD_ARRAY);
			glTexCoordPointer(3, GL_FLOAT, sizeof(Vertex), v[0].view);
			glEnableClientState(GL_NORMAL_ARRAY);
			glNormalPointer(GL_FLOAT, sizeof(Vertex), v[0].normal);
		}
		else
		{
			glDisableClientState(GL_TEXTURE_COORD_ARRAY);
			glDisableClientState(GL_NORMAL_ARRAY);
		}
		glClientActiveTexture(GL_TEXTURE0);
	}

	void Uniform4(const GLuint p, const char* name, const float x, const float y, const float z, const float w)
	{
		glUniform4f(glGetUniformLocation(p, name), x, y, z, w);
	}
}

int main(int argc, char** argv)
{
	if (argc < 3)
	{
		printf("usage: chara_shader_test <DEMASTER_EXP shaders folder> <folder with the game's main.vert and main_char.frag>\n");
		return 2;
	}
	const std::string ours = std::string(argv[1]) + "/", game = std::string(argv[2]) + "/";
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
	const EGLint contextAttributes[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
	EGLContext context = eglCreateContext(display, configs ? config : nullptr, EGL_NO_CONTEXT, contextAttributes);
	if (context == EGL_NO_CONTEXT || !eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context))
	{
		printf("no GL context\n");
		return 2;
	}
	printf("OpenGL: %s, %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

	//1. the programs
	const GLuint gameProgram = Link(game + "main.vert", game + "main_char.frag");
	const GLuint chara = Link(ours + "demaster_chara.vert", ours + "demaster_chara.frag");
	const GLuint shadow = Link(ours + "demaster_chara.vert", ours + "demaster_shadow.frag");
	const GLuint field = Link(ours + "demaster_field.vert", ours + "demaster_field.frag");
	Check(gameProgram != 0, "the game's main.vert + main_char.frag link");
	Check(chara != 0, "demaster_chara.vert + demaster_chara.frag link");
	Check(shadow != 0, "demaster_chara.vert + demaster_shadow.frag link");
	Check(field != 0, "demaster_field.vert + demaster_field.frag link");
	if (!gameProgram || !chara || !shadow || !field)
	{
		printf("%d checks, %d failures\n", checks, failures);
		return 1;
	}
	const char* fieldUniforms[] = { "demasterTileClamp", "demasterPbr", "demasterStepAside", "demasterQuality", "demasterSky", "demasterCharCount",
		"demasterCardCenter", "demasterShadowMatrix", "demasterShadowInfo" };
	for (const char* name : fieldUniforms)
		Check(glGetUniformLocation(field, name) >= 0, (std::string("demaster_field has ") + name).c_str());
	const char* charaUniforms[] = { "demasterCharMode", "demasterScreenMatrix", "demasterViewMatrix", "demasterModel", "demasterGame",
		"demasterShade", "demasterViewer", "demasterLightCount" };
	for (const char* name : charaUniforms)
		Check(glGetUniformLocation(chara, name) >= 0, (std::string("demaster_chara has ") + name).c_str());
	Check(glGetUniformLocation(shadow, "demasterViewMatrix") >= 0, "demaster_shadow keeps demasterViewMatrix");
	//every sampler on its own unit, as DEMASTER sets them (a 2D and a 3D sampler on one unit draw nothing)
	glUseProgram(chara);
	const char* samplers[] = { "tex", "demasterModelN", "demasterModelORME", "demasterProbes", "demasterRoom" };
	for (int i = 0; i < 5; i++)
		glUniform1i(glGetUniformLocation(chara, samplers[i]), i);
	glUseProgram(0);

	//a cut-out texture
	std::mt19937 random(12345);
	const int T = 64;
	std::vector<uint8_t> texels(T * T * 4);
	for (size_t i = 0; i < texels.size(); i += 4)
	{
		for (int c = 0; c < 3; c++)
			texels[i + c] = static_cast<uint8_t>(random() % 256);
		texels[i + 3] = random() % 5 == 0 ? 0 : 255;
	}
	GLuint texture = 0;
	glActiveTexture(GL_TEXTURE0);
	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, T, T, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels.data());
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

	//random TLVERTEX triangles on the game's 640 x 480 screen
	const auto triangles = [&](const bool white)
	{
		std::vector<Vertex> v;
		for (int t = 0; t < 60; t++)
		{
			const float cx = 40 + random() % 560, cy = 40 + random() % 400;
			const float depth = 2000.0f + random() % 6000;
			for (int k = 0; k < 3; k++)
			{
				Vertex x{};
				x.pos[0] = cx + static_cast<float>(random() % 80) - 40;
				x.pos[1] = cy + static_cast<float>(random() % 80) - 40;
				const float d = depth + static_cast<float>(random() % 200);
				x.pos[2] = 0.0015f + 0.9985f * d / 65536.0f;
				x.pos[3] = 1.0f / d;
				for (int c = 0; c < 3; c++)
					x.color[c] = white ? 255 : static_cast<uint8_t>(128 + random() % 128);
				x.color[3] = 255;
				x.uv[0] = (random() % 1000) / 1000.0f;
				x.uv[1] = (random() % 1000) / 1000.0f;
				x.view[0] = (x.pos[0] - 320) / 2 * d / 16384.0f / 384.0f;
				x.view[1] = (x.pos[1] - 240) / 2 * d / 16384.0f / 384.0f;
				x.view[2] = d / 16384.0f;
				x.normal[0] = 0;
				x.normal[1] = 0;
				x.normal[2] = -1;
				v.push_back(x);
			}
			//white: lit tests, every triangle facing the camera (the game's front faces turn counter-clockwise on the GL screen,
			//clockwise with y down)
			Vertex* t3 = &v[v.size() - 3];
			const float area = (t3[1].pos[0] - t3[0].pos[0]) * (t3[2].pos[1] - t3[0].pos[1]) - (t3[2].pos[0] - t3[0].pos[0]) * (t3[1].pos[1] - t3[0].pos[1]);
			if (white && area > 0)
				std::swap(t3[1], t3[2]);
		}
		return v;
	};
	const float projection[16] = { 0.003125f, 0, 0, 0, 0, -1.0f / 240.0f, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1 };
	glMatrixMode(GL_PROJECTION);
	glLoadMatrixf(projection);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();

	MakeTarget();
	glViewport(0, 0, W, H);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glEnable(GL_ALPHA_TEST);
	glAlphaFunc(GL_GEQUAL, 0.439f);
	glDisable(GL_BLEND);

	const auto drawGame = [&](const std::vector<Vertex>& v, const float ambient)
	{
		glClearColor(0.2f, 0.3f, 0.4f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glUseProgram(gameProgram);
		glUniform1i(glGetUniformLocation(gameProgram, "vertextype"), 3);
		glUniform1i(glGetUniformLocation(gameProgram, "blend_mode"), 0);
		glUniform1i(glGetUniformLocation(gameProgram, "tex"), 0);
		glUniform3f(glGetUniformLocation(gameProgram, "ambientColor"), ambient, ambient, ambient);
		Arrays(v, false);
		glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size()));
		return Read();
	};
	float nightNow[4] = { 0, 0, 0, 0 }, nightTint[4] = { 1, 1, 1, 0 }; //DAY_NIGHT uniforms for drawOurs
	const auto drawOurs = [&](const std::vector<Vertex>& v, const float ambient, const bool lit, const float shadeAdd)
	{
		glClearColor(0.2f, 0.3f, 0.4f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		glUseProgram(chara);
		glUniform1i(glGetUniformLocation(chara, "tex"), 0);
		glUniformMatrix4fv(glGetUniformLocation(chara, "demasterScreenMatrix"), 1, GL_FALSE, projection);
		Uniform4(chara, "demasterCharMode", 0, 0, 0, 0);
		Uniform4(chara, "demasterViewer", 0, 0, 0, 1);
		glUniform1i(glGetUniformLocation(chara, "demasterBlendMode"), 0);
		Uniform4(chara, "demasterModel", 0, 0, -1, lit ? 1.0f : 0.0f);
		Uniform4(chara, "demasterGame", ambient, ambient, ambient, 0);
		Uniform4(chara, "demasterShade", shadeAdd, shadeAdd, shadeAdd, 0);
		Uniform4(chara, "demasterSpace", 384, 160, 120, 1);
		Uniform4(chara, "demasterDepth", 0.25f, 0.75f, 0.05f, 0);
		Uniform4(chara, "demasterRoomSize", 320, 240, 0, 0);
		Uniform4(chara, "demasterFeatures", 0, 0, 0, 0);
		glUniform1i(glGetUniformLocation(chara, "demasterLightCount"), 0);
		glUniform4fv(glGetUniformLocation(chara, "demasterNight"), 1, nightNow);
		glUniform4fv(glGetUniformLocation(chara, "demasterNightTint"), 1, nightTint);
		Arrays(v, true);
		glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size()));
		if (const GLenum error = glGetError(); error != GL_NO_ERROR)
			printf("GL error 0x%X drawing with demaster_chara\n", error);
		return Read();
	};
	const auto compare = [](const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, int& worst, size_t& differing)
	{
		worst = 0;
		differing = 0;
		for (size_t i = 0; i < a.size(); i++)
		{
			const int d = std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i]));
			worst = std::max(worst, d);
			differing += d > 0;
		}
	};

	//2. the game's look, exactly
	for (const float ambient : { 0.0f, -0.09375f, 0.25f, -0.6f })
	{
		const std::vector<Vertex> v = triangles(false);
		const auto a = drawGame(v, ambient), b = drawOurs(v, ambient, false, 0);
		int worst = 0;
		size_t differing = 0;
		compare(a, b, worst, differing);
		char what[160];
		snprintf(what, sizeof(what), "game look with ambient %.3f matches main_char.frag (worst %d, %zu values differ)", ambient, worst, differing);
		Check(worst == 0, what);
		size_t covered = 0;
		for (size_t i = 0; i < a.size(); i += 4)
			covered += a[i] != 51 || a[i + 1] != 76 || a[i + 2] != 102;
		Check(covered > 2000, "the triangles cover the picture");
	}

	//3. lit by a flat room, white vertices, the darkening added as the game adds it
	{
		const std::vector<Vertex> v = triangles(true);
		const float ambient = -0.09375f;
		const auto a = drawGame(v, ambient), b = drawOurs(v, ambient, true, ambient);
		int worst = 0;
		size_t differing = 0;
		compare(a, b, worst, differing);
		char what[160];
		snprintf(what, sizeof(what), "flat room light, white vertices: as the game within 1 (worst %d)", worst);
		Check(worst <= 1, what);
	}

	//3b. the same at night (DAY_NIGHT, no lamps): the room's light times [night] level x tint; by day (night 0) unchanged
	{
		const std::vector<Vertex> v = triangles(true);
		const auto day = drawOurs(v, 0.0f, true, 0.0f);
		const float level = 0.3f;
		const float tint[3] = { 0.5f, 0.75f, 1.0f };
		nightNow[0] = 1.0f;
		nightNow[1] = level;
		nightNow[2] = 0.9f;
		for (int c = 0; c < 3; c++)
			nightTint[c] = tint[c];
		const auto night = drawOurs(v, 0.0f, true, 0.0f);
		nightNow[0] = 0.0f;
		const auto dayAgain = drawOurs(v, 0.0f, true, 0.0f);
		int worst = 0;
		size_t covered = 0;
		for (size_t i = 0; i < day.size(); i += 4)
		{
			if (day[i] == 51 && day[i + 1] == 76 && day[i + 2] == 102)
				continue; //the clear colour
			covered++;
			for (int c = 0; c < 3; c++)
			{
				const float linear = day[i + c] / 255.0f <= 0.04045f ? day[i + c] / 255.0f / 12.92f : std::pow((day[i + c] / 255.0f + 0.055f) / 1.055f, 2.4f);
				const float l = linear * level * tint[c];
				const float srgb = l <= 0.0031308f ? l * 12.92f : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
				worst = std::max(worst, std::abs(static_cast<int>(night[i + c]) - static_cast<int>(std::lround(srgb * 255.0f))));
			}
		}
		char what[160];
		snprintf(what, sizeof(what), "night, flat room: the room's light x level x tint within 2 (worst %d, %zu pixels)", worst, covered);
		Check(covered > 2000 && worst <= 2, what);
		Check(day == dayAgain, "night 0: exactly the day");
	}

	//4. pictures through a box (front and mirrored back), with the model's view positions
	{
		std::vector<Vertex> v;
		//a quad facing the camera: left half red, right half green (view x < 0: red)
		const float z = 0.5f;
		const float corners[4][2] = { { -0.1f, -0.2f }, { 0.1f, -0.2f }, { 0.1f, 0.2f }, { -0.1f, 0.2f } };
		const int order[6] = { 0, 2, 1, 0, 3, 2 };
		std::vector<uint8_t> two(4 * 4);
		for (int i = 0; i < 4; i++)
		{
			const bool left = (i % 2) == 0;
			two[i * 4 + 0] = left ? 255 : 0;
			two[i * 4 + 1] = left ? 0 : 255;
			two[i * 4 + 2] = 0;
			two[i * 4 + 3] = 255;
		}
		glBindTexture(GL_TEXTURE_2D, texture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, two.data());
		for (const int k : order)
		{
			Vertex x{};
			x.view[0] = corners[k][0];
			x.view[1] = corners[k][1];
			x.view[2] = z;
			x.uv[0] = corners[k][0] < 0 ? 0.25f : 0.75f;
			x.uv[1] = 0.5f;
			x.color[0] = x.color[1] = x.color[2] = x.color[3] = 255;
			x.normal[2] = -1;
			x.pos[3] = 1;
			v.push_back(x);
		}
		for (int side = 0; side < 2; side++)
		{
			//rows: right, up (world up is -y in view space), forward (away from the camera; behind: towards it)
			const float s = side == 0 ? 1.0f : -1.0f;
			const float m[16] = { 1 / 0.1f, 0, 0, 0, 0, -1 / 0.2f, 0, 0, 0, 0, s / 0.05f, 0, 0, 0, -s * z / 0.05f, 1 };
			glClearColor(0, 0, 0, 0);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
			glDepthFunc(GL_LESS);
			glUseProgram(chara);
			glUniformMatrix4fv(glGetUniformLocation(chara, "demasterViewMatrix"), 1, GL_FALSE, m);
			Uniform4(chara, "demasterCharMode", 1, side == 1 ? 1.0f : 0.0f, 0, 0);
			Uniform4(chara, "demasterViewer", 0, 0, s, 0);
			Uniform4(chara, "demasterModel", 0, 0, -1, 0);
			Uniform4(chara, "demasterGame", 0, 0, 0, 0);
			Arrays(v, true);
			glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size()));
			const auto picture = Read();
			const uint8_t* left = picture.data() + (static_cast<size_t>(H / 2) * W + W / 4) * 4;
			const uint8_t* right = picture.data() + (static_cast<size_t>(H / 2) * W + 3 * W / 4) * 4;
			char what[160];
			snprintf(what, sizeof(what), "%s picture: the card's left is the model's left (red %d,%d,%d / green %d,%d,%d)",
				side == 0 ? "front" : "back", left[0], left[1], left[2], right[0], right[1], right[2]);
			Check(left[0] > 200 && left[1] < 50 && right[1] > 200 && right[0] < 50, what);
		}
	}

	//5. instancing: four copies in one draw, each into its tile, against one draw per tile
	{
		const char* defines = "#extension GL_ARB_draw_instanced : require\n#define DEMASTER_INSTANCED 1\n";
		const GLuint charaI = Link(ours + "demaster_chara.vert", ours + "demaster_chara.frag", defines);
		const GLuint shadowI = Link(ours + "demaster_chara.vert", ours + "demaster_shadow.frag", defines);
		Check(charaI != 0 && shadowI != 0, "demaster_chara.vert with DEMASTER_INSTANCED links with both fragment shaders");
		if (charaI != 0 && shadowI != 0)
		{
			Check(glGetUniformLocation(charaI, "demasterInstanceMatrix") >= 0 && glGetUniformLocation(charaI, "demasterInstanceTile") >= 0
				&& glGetUniformLocation(charaI, "demasterInstanceLook") >= 0, "the instanced picture program has its instance uniforms");
			Check(glGetUniformLocation(shadowI, "demasterInstanceMatrix") >= 0, "the instanced shadow program has its instance uniforms");
			//a model bigger than the cameras see (so the tiles cut it), facing the camera, a light in front
			std::vector<Vertex> v;
			std::uniform_real_distribution<float> spread(-0.35f, 0.35f), depth(0.3f, 0.8f), uv(0.0f, 1.0f);
			for (int t = 0; t < 60; t++)
			{
				const float cx = spread(random), cy = spread(random), cz = depth(random);
				for (int k = 0; k < 3; k++)
				{
					Vertex x{};
					x.view[0] = cx + spread(random) * 0.4f;
					x.view[1] = cy + spread(random) * 0.4f;
					x.view[2] = cz + spread(random) * 0.2f;
					const float nx = spread(random), ny = spread(random), length = std::sqrt(nx * nx + ny * ny + 1.0f);
					x.normal[0] = nx / length;
					x.normal[1] = ny / length;
					x.normal[2] = -1.0f / length;
					x.uv[0] = uv(random);
					x.uv[1] = uv(random);
					x.color[0] = x.color[1] = x.color[2] = x.color[3] = 255;
					x.pos[3] = 1;
					v.push_back(x);
				}
			}
			std::vector<uint8_t> flat(4 * 4, 200);
			glBindTexture(GL_TEXTURE_2D, texture);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, flat.data());
			//the cameras: two boxes (orthographic, one turned), two perspective ones from in front
			float matrices[4 * 16]{}, tiles[4 * 4], looks[4 * 4];
			for (int i = 0; i < 4; i++)
			{
				float* m = matrices + i * 16;
				if (i < 2)
				{
					const float a = i * 0.6f, c = std::cos(a), s = std::sin(a);
					m[0] = c / 0.2f; m[1] = s / 0.25f; m[4] = -s / 0.2f; m[5] = c / 0.25f;
					m[10] = 1 / 0.3f; m[14] = -0.55f / 0.3f; m[15] = 1;
				}
				else
				{
					const float focal = i == 2 ? 1.6f : 2.4f, n = 0.1f, far = 2.0f;
					m[0] = focal; m[5] = -focal;
					m[10] = (far + n) / (far - n); m[11] = 1; m[14] = -2 * far * n / (far - n);
				}
				const int tx = i % 2, ty = i / 2;
				tiles[i * 4 + 0] = 0.5f;
				tiles[i * 4 + 1] = 0.5f;
				tiles[i * 4 + 2] = 0.5f * (2 * tx + 1) - 1;
				tiles[i * 4 + 3] = 0.5f * (2 * ty + 1) - 1;
				const float lx = i == 1 ? 0.3f : 0.0f, length = std::sqrt(lx * lx + 1.0f);
				looks[i * 4 + 0] = lx / length;
				looks[i * 4 + 1] = 0;
				looks[i * 4 + 2] = 1.0f / length;
				looks[i * 4 + 3] = i % 2 == 1 ? 1.0f : 0.0f; //mirrored
			}
			const auto common = [&](const GLuint p)
			{
				glUseProgram(p);
				//every sampler on its own unit (a 2D and a 3D sampler on one unit draw nothing)
				for (int s = 0; s < 5; s++)
					if (const GLint at = glGetUniformLocation(p, samplers[s]); at >= 0)
						glUniform1i(at, s);
				glUniform1i(glGetUniformLocation(p, "demasterBlendMode"), 0);
				Uniform4(p, "demasterModel", 0, 0, -1, 1);
				Uniform4(p, "demasterGame", 0, 0, 0, 0);
				Uniform4(p, "demasterShade", 0, 0, 0, 0);
				Uniform4(p, "demasterSpace", 384, 160, 120, 1);
				Uniform4(p, "demasterDepth", 0.25f, 0.75f, 0.05f, 0);
				Uniform4(p, "demasterRoomSize", 320, 240, 0, 0);
				Uniform4(p, "demasterFeatures", 0, 0, 0, 0);
				glUniform1i(glGetUniformLocation(p, "demasterLightCount"), 1);
				const float pos[4] = { 0.1f, -0.1f, 0.1f, 2.0f }, colour[4] = { 0.8f, 0.7f, 0.6f, 1.0f }, flicker[4] = { 0, 0, 1, 1 };
				glUniform4fv(glGetUniformLocation(p, "demasterLightPos"), 1, pos);
				glUniform4fv(glGetUniformLocation(p, "demasterLightColor"), 1, colour);
				glUniform4fv(glGetUniformLocation(p, "demasterLightFlicker"), 1, flicker);
			};
			const auto oneByOne = [&](const GLuint p, const int viewMode)
			{
				glClearColor(0, 0, 0, 0);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
				common(p);
				Uniform4(p, "demasterGame", 0, 0, 0, static_cast<float>(viewMode));
				for (int i = 0; i < 4; i++)
				{
					glViewport((i % 2) * (W / 2), (i / 2) * (H / 2), W / 2, H / 2);
					glUniformMatrix4fv(glGetUniformLocation(p, "demasterViewMatrix"), 1, GL_FALSE, matrices + i * 16);
					Uniform4(p, "demasterCharMode", 1, looks[i * 4 + 3], 0, 0);
					Uniform4(p, "demasterViewer", looks[i * 4], looks[i * 4 + 1], looks[i * 4 + 2], 0);
					Arrays(v, true);
					glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size()));
				}
				glViewport(0, 0, W, H);
			};
			const auto instanced = [&](const GLuint p, const int viewMode)
			{
				glClearColor(0, 0, 0, 0);
				glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
				common(p);
				Uniform4(p, "demasterGame", 0, 0, 0, static_cast<float>(viewMode));
				glViewport(0, 0, W, H);
				glUniformMatrix4fv(glGetUniformLocation(p, "demasterInstanceMatrix"), 4, GL_FALSE, matrices);
				glUniform4fv(glGetUniformLocation(p, "demasterInstanceTile"), 4, tiles);
				glUniform4fv(glGetUniformLocation(p, "demasterInstanceLook"), 4, looks);
				Uniform4(p, "demasterCharMode", 1, 0, 1, 0);
				Arrays(v, true);
				glDrawArraysInstanced(GL_TRIANGLES, 0, static_cast<GLsizei>(v.size()), 4);
				Uniform4(p, "demasterCharMode", 1, 0, 0, 0);
			};
			const auto readDepth = []()
			{
				std::vector<float> d(static_cast<size_t>(W) * H);
				glReadPixels(0, 0, W, H, GL_DEPTH_COMPONENT, GL_FLOAT, d.data());
				return d;
			};
			glEnable(GL_DEPTH_TEST);
			glDepthFunc(GL_LESS);
			glDisable(GL_BLEND);
			for (const int viewMode : { 0, 15 })
			{
				oneByOne(charaI, viewMode);
				const auto a = Read();
				instanced(charaI, viewMode);
				const auto b = Read();
				int worst = 0;
				size_t differing = 0, covered = 0, off = 0;
				compare(a, b, worst, differing);
				for (size_t i = 0; i < a.size(); i += 4)
				{
					covered += a[i + 3] != 0;
					int most = 0;
					for (int c = 0; c < 4; c++)
						most = std::max(most, std::abs(static_cast<int>(a[i + c]) - static_cast<int>(b[i + c])));
					off += most > 2;
				}
				for (size_t i = 0; i < a.size() && getenv("SHOW_DIFF"); i++)
					if (a[i] != b[i])
						printf("  differs at x %zu y %zu channel %zu: %d vs %d\n", (i / 4) % W, (i / 4) / W, i % 4, a[i], b[i]);
				char what[200];
				//(a pixel or two on a triangle's very edge may go the other way: the copy is moved into its tile in float)
				snprintf(what, sizeof(what), "pictures (%s): 4 in one draw match one by one (%zu of %zu pixels off by more than 2, worst %d; %zu covered)",
					viewMode == 15 ? "normals" : "lit", off, a.size() / 4, worst, covered);
				Check(covered > 5000 && off <= a.size() / 4 / 1000, what);
				//every tile has its own picture, none spills into the next
				for (int i = 0; i < 4; i++)
				{
					size_t inTile = 0;
					for (int y = (i / 2) * (H / 2); y < (i / 2 + 1) * (H / 2); y++)
						for (int x = (i % 2) * (W / 2); x < (i % 2 + 1) * (W / 2); x++)
							inTile += b[(static_cast<size_t>(y) * W + x) * 4 + 3] != 0;
					snprintf(what, sizeof(what), "pictures: tile %d has its copy (%zu pixels)", i, inTile);
					Check(inTile > 500, what);
				}
			}
			//the plain program (no instancing) draws what the instanced one draws one by one
			{
				oneByOne(chara, 0);
				const auto a = Read();
				oneByOne(charaI, 0);
				const auto b = Read();
				int worst = 0;
				size_t differing = 0;
				compare(a, b, worst, differing);
				Check(worst == 0, "the program without instancing draws the same pictures");
			}
			//shadow maps: depth only
			glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
			oneByOne(shadowI, 0);
			const auto da = readDepth();
			instanced(shadowI, 0);
			const auto db = readDepth();
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			float worst = 0;
			size_t written = 0, differing = 0;
			for (size_t i = 0; i < da.size(); i++)
			{
				worst = std::max(worst, std::fabs(da[i] - db[i]));
				written += da[i] < 1.0f;
				differing += std::fabs(da[i] - db[i]) > 1e-5f;
			}
			char what[200];
			snprintf(what, sizeof(what), "shadow maps: 4 in one draw match one by one (worst %.2g, %zu of %zu differ, %zu written)", worst,
				differing, da.size(), written);
			Check(written > 5000 && differing <= da.size() / 1000, what);
		}
	}

	printf("%d checks, %d failures\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
