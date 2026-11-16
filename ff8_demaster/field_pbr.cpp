#include "field_pbr.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace fieldpbr
{
	//--- names

	const char* KindSuffix(const Kind kind)
	{
		switch (kind)
		{
		case ALBEDO: return "_A";
		case LIGHTMAP: return "_LF";
		case NORMAL_DEPTH: return "_ND";
		case ORME: return "_ORME";
		default: return "";
		}
	}

	std::string FieldStem(const std::string& folder, const std::string& field, const std::string& language)
	{
		return folder + field + (language.empty() ? std::string() : "_" + language);
	}

	std::string LayerStem(const std::string& folder, const std::string& field, const std::string& language, const uint32_t pupu)
	{
		char hex[16];
		snprintf(hex, sizeof(hex), "%08X", pupu);
		return FieldStem(folder, field, language) + "_" + hex;
	}

	//--- colour

	float SrgbToLinear(const float c)
	{
		const float v = c < 0 ? 0 : c > 1 ? 1 : c;
		return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f);
	}

	float LinearToSrgb(const float c)
	{
		const float v = c < 0 ? 0 : c > 1 ? 1 : c;
		return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
	}

	const float* SrgbTable()
	{
		static const std::array<float, 256> table = []
		{
			std::array<float, 256> t{};
			for (int i = 0; i < 256; i++)
				t[i] = SrgbToLinear(i / 255.0f);
			return t;
		}();
		return table.data();
	}

	uint8_t LinearToSrgb8(const float c)
	{
		//the nearest code in linear terms, so decoding gives back the closest value the 8 bits can hold
		const float* table = SrgbTable();
		int code = static_cast<int>(LinearToSrgb(c) * 255.0f + 0.5f);
		code = std::clamp(code, 0, 255);
		const float v = c < 0 ? 0 : c;
		while (code > 0 && std::fabs(table[code - 1] - v) < std::fabs(table[code] - v))
			code--;
		while (code < 255 && std::fabs(table[code + 1] - v) < std::fabs(table[code] - v))
			code++;
		return static_cast<uint8_t>(code);
	}

	uint8_t EncodeLightmap(const float light, const float scale)
	{
		if (scale <= 0)
			return 0;
		return LinearToSrgb8(light / scale);
	}

	//--- vectors

	Vec3 Cross(const Vec3& a, const Vec3& b)
	{
		return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
	}

	double Length(const Vec3& a)
	{
		return std::sqrt(Dot(a, a));
	}

	Vec3 Normalize(const Vec3& a)
	{
		const double length = Length(a);
		return length > 0 ? a * (1.0 / length) : Vec3{ 0, 0, 0 };
	}

	//--- space

	double Space::Distance(const double value) const
	{
		double z;
		if (model == DepthModel::Linear)
			z = a + b * value;
		else
		{
			const double inverse = a + b * value;
			z = inverse > 0 ? 1.0 / inverse : 1e9;
		}
		return std::max(z, nearest);
	}

	double Space::Value(const double distance) const
	{
		if (b == 0)
			return 0;
		if (model == DepthModel::Linear)
			return (distance - a) / b;
		return (1.0 / std::max(distance, 1e-9) - a) / b;
	}

	Vec3 Space::ViewPosition(const double canvasX, const double canvasY, const double value) const
	{
		const double z = Distance(value);
		return { (canvasX - originX) * z / zoom, (canvasY - originY) * z / zoom, z };
	}

	bool Space::Project(const Vec3& view, double& canvasX, double& canvasY) const
	{
		if (view.z <= 1e-9)
			return false;
		canvasX = originX + view.x * zoom / view.z;
		canvasY = originY + view.y * zoom / view.z;
		return true;
	}

	Space EstimatedSpace(const double originX, const double originY, const bool brightIsNear)
	{
		Space space;
		space.measured = false;
		space.originX = originX;
		space.originY = originY;
		space.zoom = 384.0;
		space.model = DepthModel::Inverse;
		//nearest point 1, farthest 4
		if (brightIsNear)
		{
			space.a = 0.25;
			space.b = 0.75;
		}
		else
		{
			space.a = 1.0;
			space.b = -0.75;
		}
		return space;
	}

	//--- camera and walkmesh

	namespace
	{
		int16_t S16(const uint8_t* p) { return static_cast<int16_t>(p[0] | p[1] << 8); }
		int32_t S32(const uint8_t* p) { return static_cast<int32_t>(static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24); }
	}

	std::vector<Camera> ParseCameras(const uint8_t* data, const size_t size)
	{
		std::vector<Camera> cameras;
		if (data == nullptr)
			return cameras;
		for (size_t offset = 0; offset + 38 <= size; offset += 40)
		{
			const uint8_t* p = data + offset;
			Camera c;
			for (int i = 0; i < 3; i++)
				for (int j = 0; j < 3; j++)
					c.axis[i][j] = S16(p + (i * 3 + j) * 2);
			for (int i = 0; i < 3; i++)
				c.position[i] = S32(p + 20 + i * 4);
			c.zoom = static_cast<uint16_t>(p[36] | p[37] << 8);
			cameras.push_back(c);
		}
		return cameras;
	}

	Walkmesh ParseWalkmesh(const uint8_t* data, const size_t size)
	{
		Walkmesh mesh;
		if (data == nullptr || size < 4)
			return mesh;
		const uint32_t count = static_cast<uint32_t>(S32(data));
		if (count == 0 || count > 100000 || 4 + static_cast<size_t>(count) * 24 > size)
			return mesh;
		for (uint32_t i = 0; i < count; i++)
		{
			std::array<Vec3, 3> triangle;
			for (int v = 0; v < 3; v++)
			{
				const uint8_t* p = data + 4 + static_cast<size_t>(i) * 24 + v * 8;
				triangle[v] = { S16(p) / 4096.0, S16(p + 2) / 4096.0, S16(p + 4) / 4096.0 };
			}
			mesh.triangles.push_back(triangle);
		}
		return mesh;
	}

	Vec3 CameraTransform::ToView(const Vec3& world) const
	{
		const Vec3 d = world - eye;
		return { Dot(d, right), Dot(d, down), Dot(d, forward) };
	}

	CameraTransform MakeCameraTransform(const Camera& camera, const int variant)
	{
		const Vec3 ax{ camera.axis[0][0] / 4096.0, camera.axis[0][1] / 4096.0, camera.axis[0][2] / 4096.0 };
		const Vec3 ay{ -camera.axis[1][0] / 4096.0, -camera.axis[1][1] / 4096.0, -camera.axis[1][2] / 4096.0 };
		const Vec3 az{ camera.axis[2][0] / 4096.0, camera.axis[2][1] / 4096.0, camera.axis[2][2] / 4096.0 };
		const Vec3 p{ camera.position[0] / 4096.0, -camera.position[1] / 4096.0, camera.position[2] / 4096.0 };
		CameraTransform t;
		t.eye = (ax * p.x + ay * p.y + az * p.z) * -1.0;
		t.forward = Normalize(az);
		const Vec3 side = Normalize(Cross(t.forward, ay)); //gluLookAt: s = f x up, u = s x f
		const Vec3 up = Cross(side, t.forward);
		t.right = (variant & 1) ? side * -1.0 : side;
		t.down = (variant & 2) ? up : up * -1.0;
		t.zoom = camera.zoom;
		return t;
	}

	//--- text helpers

	std::string Trim(const std::string& text)
	{
		size_t begin = 0, end = text.size();
		while (begin < end && isspace(static_cast<unsigned char>(text[begin])))
			begin++;
		while (end > begin && isspace(static_cast<unsigned char>(text[end - 1])))
			end--;
		return text.substr(begin, end - begin);
	}

	std::string Lower(std::string text)
	{
		std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char c) { return static_cast<char>(tolower(c)); });
		return text;
	}

	std::vector<std::string> SplitWords(const std::string& text)
	{
		std::vector<std::string> words;
		std::istringstream in(text);
		std::string word;
		while (in >> word)
			words.push_back(word);
		return words;
	}

	namespace
	{
		struct KeyValue
		{
			int line = 0;
			std::string section, key, value;
		};

		//"key = value" lines, "[section]" headers, ";" or "#" comments
		std::vector<KeyValue> ReadKeyValues(const std::string& text, std::string& warnings)
		{
			std::vector<KeyValue> out;
			std::istringstream in(text);
			std::string line, section;
			int number = 0;
			while (std::getline(in, line))
			{
				number++;
				if (const size_t comment = line.find_first_of(";#"); comment != std::string::npos)
					line = line.substr(0, comment);
				line = Trim(line);
				if (line.empty())
					continue;
				if (line.front() == '[' && line.back() == ']')
				{
					section = Lower(Trim(line.substr(1, line.size() - 2)));
					out.push_back({ number, section, "", "" });
					continue;
				}
				const size_t equals = line.find('=');
				if (equals == std::string::npos)
				{
					warnings += "line " + std::to_string(number) + ": no '=': " + line + "\n";
					continue;
				}
				out.push_back({ number, section, Lower(Trim(line.substr(0, equals))), Trim(line.substr(equals + 1)) });
			}
			return out;
		}

		bool Numbers(const std::string& value, double* out, const size_t count)
		{
			const std::vector<std::string> words = SplitWords(value);
			if (words.size() < count)
				return false;
			for (size_t i = 0; i < count; i++)
			{
				char* end = nullptr;
				out[i] = strtod(words[i].c_str(), &end);
				if (end == words[i].c_str() || !std::isfinite(out[i]))
					return false;
			}
			return true;
		}

		bool Number(const std::string& value, double& out)
		{
			return Numbers(value, &out, 1);
		}

		bool Bool(const std::string& value, bool& out)
		{
			const std::string v = Lower(value);
			if (v == "1" || v == "yes" || v == "true" || v == "on")
				out = true;
			else if (v == "0" || v == "no" || v == "false" || v == "off")
				out = false;
			else
				return false;
			return true;
		}

		std::string Format(const char* format, ...)
		{
			char text[512];
			va_list args;
			va_start(args, format);
			vsnprintf(text, sizeof(text), format, args);
			va_end(args);
			return text;
		}
	}

	//--- settings

	float Settings::LightmapScale(const uint32_t pupu, const bool ownLightmap) const
	{
		if (ownLightmap)
			if (const auto it = layerLightmapScale.find(pupu); it != layerLightmapScale.end())
				return it->second;
		return lightmapScale;
	}

	bool ParseSettings(const std::string& text, Settings& out, std::string& warnings)
	{
		Settings s = out; //what the file doesn't say stays as the caller set it
		s.layerLightmapScale.clear();
		s.occlusion.clear();
		bool any = false;
		for (const KeyValue& kv : ReadKeyValues(text, warnings))
		{
			if (kv.key.empty())
				continue;
			const std::string where = "line " + std::to_string(kv.line) + ": ";
			double v[3];
			bool ok = true;
			if (kv.key == "camera")
				s.space.measured = Lower(kv.value) == "measured";
			else if (kv.key == "zoom")
			{
				ok = Number(kv.value, v[0]) && v[0] > 0;
				if (ok)
					s.space.zoom = v[0];
			}
			else if (kv.key == "origin")
			{
				ok = Numbers(kv.value, v, 2);
				if (ok)
				{
					s.space.originX = v[0];
					s.space.originY = v[1];
				}
			}
			else if (kv.key == "canvas")
			{
				double c[4];
				ok = Numbers(kv.value, c, 4) && c[2] >= 16 && c[3] >= 16 && c[2] <= 4096 && c[3] <= 4096;
				if (ok)
				{
					s.hasCanvas = true;
					for (int i = 0; i < 4; i++)
						s.canvas[i] = static_cast<int>(std::lround(c[i]));
				}
			}
			else if (kv.key == "depth")
			{
				const std::vector<std::string> words = SplitWords(kv.value);
				ok = words.size() >= 3 && (Lower(words[0]) == "linear" || Lower(words[0]) == "inverse")
					&& Numbers(words[1] + " " + words[2], v, 2);
				if (ok)
				{
					s.space.model = Lower(words[0]) == "linear" ? DepthModel::Linear : DepthModel::Inverse;
					s.space.a = v[0];
					s.space.b = v[1];
				}
			}
			else if (kv.key == "nearest")
			{
				ok = Number(kv.value, v[0]) && v[0] > 0;
				if (ok)
					s.space.nearest = v[0];
			}
			else if (kv.key == "bright_is_near")
				ok = Bool(kv.value, s.brightIsNear);
			else if (kv.key == "normal_y_down")
				ok = Bool(kv.value, s.normalYDown);
			else if (kv.key == "lightmap_scale")
			{
				ok = Number(kv.value, v[0]) && v[0] > 0;
				if (ok)
					s.lightmapScale = static_cast<float>(v[0]);
			}
			else if (kv.key.rfind("lightmap_scale.", 0) == 0)
			{
				const std::string hex = kv.key.substr(15);
				char* end = nullptr;
				const unsigned long pupu = strtoul(hex.c_str(), &end, 16);
				ok = hex.size() == 8 && end == hex.c_str() + 8 && Number(kv.value, v[0]) && v[0] > 0;
				if (ok)
					s.layerLightmapScale[static_cast<uint32_t>(pupu)] = static_cast<float>(v[0]);
			}
			else if (kv.key == "game_depth")
			{
				ok = Number(kv.value, v[0]) && v[0] > 0;
				if (ok)
					s.gameDepth = v[0];
			}
			else if (kv.key.rfind("occlusion.", 0) == 0)
			{
				const std::string number = kv.key.substr(10);
				char* end = nullptr;
				const long z = strtol(number.c_str(), &end, 10);
				ok = !number.empty() && end == number.c_str() + number.size() && z > 0 && z < 4096 && Number(kv.value, v[0]) && v[0] > 0;
				if (ok)
					s.occlusion[static_cast<int>(z)] = v[0];
			}
			else if (kv.key == "camera_eye" || kv.key == "camera_right" || kv.key == "camera_down" || kv.key == "camera_forward" || kv.key == "up")
			{
				ok = Numbers(kv.value, v, 3);
				if (ok)
				{
					const Vec3 value{ v[0], v[1], v[2] };
					if (kv.key == "camera_eye")
						s.cameraEye = value;
					else if (kv.key == "camera_right")
						s.cameraRight = value;
					else if (kv.key == "camera_down")
						s.cameraDown = value;
					else if (kv.key == "camera_forward")
						s.cameraForward = value;
					else if (Length(value) > 1e-6)
						s.up = Normalize(value);
					if (kv.key != "up")
						s.cameraKnown = true;
				}
			}
			else if (kv.key == "sky_value")
			{
				ok = Number(kv.value, v[0]);
				if (ok)
					s.skyValue = v[0];
			}
			else if (kv.key == "probe_depth")
			{
				ok = Numbers(kv.value, v, 2) && v[0] > 0 && v[1] > v[0];
				if (ok)
				{
					s.probeNear = v[0];
					s.probeFar = v[1];
				}
			}
			else if (kv.key == "sky")
				ok = Bool(kv.value, s.sky);
			else if (kv.key == "time")
			{
				const std::string t = Lower(kv.value);
				ok = t == "cycle" || t == "day" || t == "night";
				if (ok)
					s.time = t == "day" ? FieldTime::Day : t == "night" ? FieldTime::Night : FieldTime::Cycle;
			}
			else if (kv.key == "character_light")
			{
				if (Lower(kv.value) == "auto")
					s.characterLight = 0;
				else
				{
					ok = Number(kv.value, v[0]) && v[0] > 0;
					if (ok)
						s.characterLight = v[0];
				}
			}
			else if (kv.key == "character_shade")
			{
				const std::string mode = Lower(kv.value);
				ok = mode == "keep" || mode == "replace";
				if (ok)
					s.characterShadeKeep = mode == "keep";
			}
			else
			{
				warnings += where + "unknown setting " + kv.key + "\n";
				continue;
			}
			if (!ok)
				warnings += where + "can't read " + kv.key + " = " + kv.value + "\n";
			else
				any = true;
		}
		out = s;
		return any;
	}

	std::string WriteSettings(const Settings& s, const std::string& field, const std::string& notes)
	{
		std::string out;
		out += "; " + field + ": how DEMASTER reads this field's PBR maps. Written by fieldprep, which rewrites it every time it runs:\n";
		out += "; put your own changes in " + field + LIGHTS_SUFFIX + ".\n";
		if (!notes.empty())
		{
			std::istringstream in(notes);
			std::string line;
			while (std::getline(in, line))
				out += "; " + line + "\n";
		}
		out += "\n";
		out += Format("camera = %s          ; measured: from the field's camera and walkmesh; estimated: guessed\n", s.space.measured ? "measured" : "estimated");
		out += Format("zoom = %.4f\n", s.space.zoom);
		if (s.hasCanvas)
			out += Format("canvas = %d %d %d %d     ; the part of the field the canvas covers: .map left top width height (native px)\n",
				s.canvas[0], s.canvas[1], s.canvas[2], s.canvas[3]);
		out += Format("origin = %.2f %.2f          ; canvas pixel on the camera axis\n", s.space.originX, s.space.originY);
		out += Format("depth = %s %.9g %.9g     ; distance = %s\n", s.space.model == DepthModel::Linear ? "linear" : "inverse", s.space.a, s.space.b,
			s.space.model == DepthModel::Linear ? "a + b * depth value" : "1 / (a + b * depth value)");
		out += Format("nearest = %.6g\n", s.space.nearest);
		out += Format("bright_is_near = %d\n", s.brightIsNear ? 1 : 0);
		out += Format("normal_y_down = %d       ; 1: the normal map you gave had green pointing down; it was flipped in _ND\n", s.normalYDown ? 1 : 0);
		out += Format("lightmap_scale = %.4f     ; the brightest the _LF holds (its RGB is that much at 255)\n", s.lightmapScale);
		for (const auto& [pupu, scale] : s.layerLightmapScale)
			out += Format("lightmap_scale.%08X = %.4f\n", pupu, scale);
		out += Format("game_depth = %.6g          ; the game's depth per view unit (16384 when the depth is measured)\n", s.gameDepth);
		if (!s.occlusion.empty())
			out += "; tiles in front of the base picture: the view distance of the nearest part of what each Z shows\n";
		for (const auto& [z, distance] : s.occlusion)
			out += Format("occlusion.%d = %.6g\n", z, distance);
		if (s.cameraKnown)
		{
			out += Format("camera_eye = %.9g %.9g %.9g\n", s.cameraEye.x, s.cameraEye.y, s.cameraEye.z);
			out += Format("camera_right = %.9g %.9g %.9g\n", s.cameraRight.x, s.cameraRight.y, s.cameraRight.z);
			out += Format("camera_down = %.9g %.9g %.9g\n", s.cameraDown.x, s.cameraDown.y, s.cameraDown.z);
			out += Format("camera_forward = %.9g %.9g %.9g\n", s.cameraForward.x, s.cameraForward.y, s.cameraForward.z);
		}
		out += Format("up = %.6f %.6f %.6f        ; the world's up as the camera sees it\n", s.up.x, s.up.y, s.up.z);
		if (s.skyValue >= 0)
			out += Format("sky_value = %.6f        ; depth map value where the sky starts (used with sky = 1)\n", s.skyValue);
		if (s.probeFar > s.probeNear && s.probeNear > 0)
			out += Format("probe_depth = %.6g %.6g    ; the light grid's slices, nearest and farthest\n", s.probeNear, s.probeFar);
		out += "\n; yours: fieldprep keeps these when it writes this file again\n";
		out += Format("time = %s               ; cycle: follows DEMASTER's day/night clock; day or night: always that here (the clock waits)\n",
			FieldTimeName(s.time));
		out += Format("sky = %d                  ; 1: pixels at the farthest depth are sky (no added light, shadows or reflections)\n", s.sky ? 1 : 0);
		out += Format("character_shade = %s     ; replace: the room's light replaces the game's darkening of the characters; keep: both\n",
			s.characterShadeKeep ? "keep" : "replace");
		if (s.characterLight > 0)
			out += Format("character_light = %.4g      ; auto: the characters as bright as the game draws them, on average; a number: the room's real light times it\n", s.characterLight);
		else
			out += "character_light = auto      ; auto: the characters as bright as the game draws them, on average; a number: the room's real light times it\n";
		return out;
	}

	const char* FieldTimeName(const FieldTime time)
	{
		return time == FieldTime::Day ? "day" : time == FieldTime::Night ? "night" : "cycle";
	}

	//--- lights

	const char* FlickerName(const Flicker flicker)
	{
		switch (flicker)
		{
		case Flicker::Fire: return "fire";
		case Flicker::Candle: return "candle";
		case Flicker::Pulse: return "pulse";
		case Flicker::Strobe: return "strobe";
		case Flicker::Fluorescent: return "fluorescent";
		default: return "none";
		}
	}

	bool ParseLights(const std::string& text, Lights& out, std::string& warnings)
	{
		Lights result;
		Light* light = nullptr;
		bool any = false;
		for (const KeyValue& kv : ReadKeyValues(text, warnings))
		{
			const std::string where = "line " + std::to_string(kv.line) + ": ";
			if (kv.key.empty())
			{
				if (kv.section == "light")
				{
					result.lights.emplace_back();
					light = &result.lights.back();
					light->name = "light " + std::to_string(result.lights.size());
				}
				else if (kv.section == "emissive")
				{
					result.hasEmissive = true;
					light = nullptr;
				}
				else if (kv.section == "night")
				{
					result.hasNight = true;
					light = nullptr;
				}
				else
				{
					warnings += where + "unknown section [" + kv.section + "]\n";
					light = nullptr;
				}
				continue;
			}
			double v[3];
			bool ok = true;
			if (light == nullptr && kv.section == "emissive")
			{
				Emissive& e = result.emissive;
				if (kv.key == "hue")
				{
					ok = Numbers(kv.value, v, 1) && v[0] >= -360 && v[0] <= 360;
					if (ok)
						e.hue = static_cast<float>(v[0]);
				}
				else if (kv.key == "tint")
				{
					ok = Numbers(kv.value, v, 3);
					if (ok)
						for (int i = 0; i < 3; i++)
							e.tint[i] = static_cast<float>(std::max(0.0, v[i]));
				}
				else if (kv.key == "strength")
				{
					ok = Numbers(kv.value, v, 1) && v[0] >= 0;
					if (ok)
						e.strength = static_cast<float>(v[0]);
				}
				else
				{
					warnings += where + "unknown setting " + kv.key + " in [emissive] (hue, tint, strength)\n";
					continue;
				}
			}
			else if (light == nullptr && kv.section == "night")
			{
				Night& n = result.night;
				if (kv.key == "level")
				{
					ok = Numbers(kv.value, v, 1) && v[0] >= 0 && v[0] <= 1;
					if (ok)
						n.level = static_cast<float>(v[0]);
				}
				else if (kv.key == "tint")
				{
					ok = Numbers(kv.value, v, 3);
					if (ok)
						for (int i = 0; i < 3; i++)
							n.tint[i] = static_cast<float>(std::max(0.0, v[i]));
				}
				else if (kv.key == "lamps")
				{
					ok = Numbers(kv.value, v, 1) && v[0] >= 0 && v[0] <= 1;
					if (ok)
						n.lamps = static_cast<float>(v[0]);
				}
				else
				{
					warnings += where + "unknown setting " + kv.key + " in [night] (level, tint, lamps)\n";
					continue;
				}
			}
			else if (light == nullptr)
			{
				if (kv.key == "ambient")
				{
					ok = Numbers(kv.value, v, 3);
					if (ok)
						for (int i = 0; i < 3; i++)
							result.ambient[i] = static_cast<float>(std::max(0.0, v[i]));
				}
				else if (!kv.section.empty())
					continue; //keys of an unknown section
				else
				{
					warnings += where + "unknown setting " + kv.key + " (lights start with [light])\n";
					continue;
				}
			}
			else if (kv.key == "name")
				light->name = kv.value;
			else if (kv.key == "position")
			{
				ok = Numbers(kv.value, v, 3);
				if (ok)
				{
					light->x = v[0];
					light->y = v[1];
					light->depth = v[2];
				}
			}
			else if (kv.key == "color" || kv.key == "colour")
			{
				ok = Numbers(kv.value, v, 3);
				if (ok)
					for (int i = 0; i < 3; i++)
						light->color[i] = static_cast<float>(std::max(0.0, v[i]));
			}
			else if (kv.key == "intensity")
			{
				ok = Number(kv.value, v[0]);
				if (ok)
					light->intensity = static_cast<float>(std::max(0.0, v[0]));
			}
			else if (kv.key == "range")
			{
				ok = Number(kv.value, v[0]) && v[0] > 0;
				if (ok)
					light->range = static_cast<float>(v[0]);
			}
			else if (kv.key == "baked")
				ok = Bool(kv.value, light->baked);
			else if (kv.key == "flicker")
			{
				const std::string f = Lower(kv.value);
				ok = true;
				if (f == "none" || f == "0" || f == "off")
					light->flicker = Flicker::None;
				else if (f == "fire")
					light->flicker = Flicker::Fire;
				else if (f == "candle")
					light->flicker = Flicker::Candle;
				else if (f == "pulse")
					light->flicker = Flicker::Pulse;
				else if (f == "strobe")
					light->flicker = Flicker::Strobe;
				else if (f == "fluorescent")
					light->flicker = Flicker::Fluorescent;
				else
					ok = false;
			}
			else if (kv.key == "amount")
			{
				ok = Number(kv.value, v[0]);
				if (ok)
					light->amount = static_cast<float>(std::clamp(v[0], 0.0, 1.0));
			}
			else if (kv.key == "speed")
			{
				ok = Number(kv.value, v[0]) && v[0] >= 0;
				if (ok)
					light->speed = static_cast<float>(v[0]);
			}
			else if (kv.key == "specular")
			{
				ok = Number(kv.value, v[0]);
				if (ok)
					light->specular = static_cast<float>(std::max(0.0, v[0]));
			}
			else if (kv.key == "shadow")
			{
				ok = Number(kv.value, v[0]);
				if (ok)
					light->shadow = static_cast<float>(std::clamp(v[0], 0.0, 1.0));
			}
			else if (kv.key == "time")
			{
				const std::string t = Lower(kv.value);
				ok = t == "day" || t == "night" || t == "both" || t == "always";
				if (ok)
					light->time = t == "day" ? LightTime::Day : t == "night" ? LightTime::Night : LightTime::Both;
			}
			else if (kv.key == "source")
			{
				const std::string t = Lower(kv.value);
				ok = t == "found" || t == "mark" || t == "yours";
				if (ok)
					light->source = t == "found" ? LightSource::Found : t == "mark" ? LightSource::Mark : LightSource::Yours;
			}
			else if (kv.key == "follows")
			{
				const std::string f = Lower(kv.value);
				if (f == "none" || f == "-1" || f.empty())
					light->follows = -1;
				else
				{
					char* end = nullptr;
					const long group = strtol(f.c_str(), &end, 16); //as the pupu shows it: 01, 0A
					ok = end == f.c_str() + f.size() && group >= 0 && group < 255;
					if (ok)
						light->follows = static_cast<int>(group);
				}
			}
			else if (kv.key == "on_states")
			{
				light->onStates.clear();
				std::string list = kv.value;
				std::replace(list.begin(), list.end(), ',', ' ');
				for (const std::string& word : SplitWords(list))
				{
					char* end = nullptr;
					const long state = strtol(word.c_str(), &end, 10);
					if (end != word.c_str() + word.size() || state < 0 || state > 255)
					{
						ok = false;
						break;
					}
					light->onStates.push_back(static_cast<int>(state));
				}
			}
			else
			{
				warnings += where + "unknown light setting " + kv.key + "\n";
				continue;
			}
			if (!ok)
				warnings += where + "can't read " + kv.key + " = " + kv.value + "\n";
			else
				any = true;
		}
		if (result.lights.size() > static_cast<size_t>(MAX_LIGHTS_IN_FILE))
		{
			warnings += "more than " + std::to_string(MAX_LIGHTS_IN_FILE) + " lights: only the first " + std::to_string(MAX_LIGHTS_IN_FILE)
				+ " are read\n";
			result.lights.resize(MAX_LIGHTS_IN_FILE);
		}
		//per time of day: at most MAX_FOUND_LIGHTS of fieldprep's, MAX_YOUR_LIGHTS of yours (marks count as yours)
		for (const LightTime time : { LightTime::Day, LightTime::Night })
		{
			size_t found = 0, yours = 0;
			for (const Light& l : result.lights)
				if (l.time == time || l.time == LightTime::Both)
					(l.source == LightSource::Found ? found : yours)++;
			const char* when = time == LightTime::Day ? "by day" : "by night";
			if (found > static_cast<size_t>(MAX_FOUND_LIGHTS))
				warnings += "more than " + std::to_string(MAX_FOUND_LIGHTS) + " of fieldprep's lights on " + when + ": only the first "
					+ std::to_string(MAX_FOUND_LIGHTS) + " are used\n";
			if (yours > static_cast<size_t>(MAX_YOUR_LIGHTS))
				warnings += "more than " + std::to_string(MAX_YOUR_LIGHTS) + " of your own lights on " + when + " (marks count too): only the "
					"first " + std::to_string(MAX_YOUR_LIGHTS) + " are used\n";
		}
		out = result;
		return any || !result.lights.empty();
	}

	void KeepDayLights(Lights& lights)
	{
		std::vector<Light> kept;
		int found = 0, yours = 0;
		for (const Light& l : lights.lights)
		{
			if (l.time == LightTime::Night)
				continue;
			int& count = l.source == LightSource::Found ? found : yours;
			if (count >= (l.source == LightSource::Found ? MAX_FOUND_LIGHTS : MAX_YOUR_LIGHTS))
				continue;
			count++;
			kept.push_back(l);
		}
		lights.lights = std::move(kept);
	}

	std::vector<ActiveLight> ActiveLights(const Lights& lights, const double night, size_t* dropped)
	{
		const double n = std::clamp(std::isfinite(night) ? night : 0.0, 0.0, 1.0);
		//each time of day's set: the first MAX_FOUND_LIGHTS of fieldprep's and MAX_YOUR_LIGHTS of yours on then
		const auto set = [&](const LightTime time)
		{
			std::vector<bool> in(lights.lights.size(), false);
			int found = 0, yours = 0;
			for (size_t i = 0; i < lights.lights.size(); i++)
			{
				const Light& l = lights.lights[i];
				if (l.time != time && l.time != LightTime::Both)
					continue;
				int& count = l.source == LightSource::Found ? found : yours;
				if (count >= (l.source == LightSource::Found ? MAX_FOUND_LIGHTS : MAX_YOUR_LIGHTS))
					continue;
				count++;
				in[i] = true;
			}
			return in;
		};
		const std::vector<bool> byDay = set(LightTime::Day), byNight = set(LightTime::Night);
		std::vector<ActiveLight> on;
		for (size_t i = 0; i < lights.lights.size(); i++)
		{
			const double weight = (byDay[i] ? 1.0 - n : 0.0) + (byNight[i] ? n : 0.0);
			if (weight <= 0.0)
				continue;
			on.push_back({ i, static_cast<float>(std::min(weight, 1.0)), byNight[i] && lights.lights[i].baked });
		}
		size_t left = 0;
		if (on.size() > static_cast<size_t>(MAX_LIGHTS))
		{
			std::stable_sort(on.begin(), on.end(), [](const ActiveLight& a, const ActiveLight& b) { return a.weight > b.weight; });
			left = on.size() - MAX_LIGHTS;
			on.resize(MAX_LIGHTS);
			std::sort(on.begin(), on.end(), [](const ActiveLight& a, const ActiveLight& b) { return a.index < b.index; });
		}
		if (dropped != nullptr)
			*dropped = left;
		return on;
	}

	const char* LightTimeName(const LightTime time)
	{
		return time == LightTime::Day ? "day" : time == LightTime::Night ? "night" : "both";
	}

	const char* LightSourceName(const LightSource source)
	{
		return source == LightSource::Found ? "found" : source == LightSource::Mark ? "mark" : "yours";
	}

	std::string WriteLights(const Lights& lights, const std::string& field, const std::string& header)
	{
		std::string out;
		out += "; " + field + ": the field's lights. fieldprep wrote this once; it doesn't overwrite it (fieldprep --relight does).\n";
		out += "; DEMASTER reads it again within a second of you saving it.\n";
		if (!header.empty())
		{
			std::istringstream in(header);
			std::string line;
			while (std::getline(in, line))
				out += "; " + line + "\n";
		}
		out += ";\n";
		out += "; position  = x y on the field's canvas (native pixels: layer image pixel / scale), then the depth map value there (0-1)\n";
		out += "; color     = red green blue (linear, 1 = full)\n";
		out += "; baked     = 1: the light is in the painting; flicker scales its painted light (the _LF mask)\n";
		out += ";             0: a new light, only what intensity adds\n";
		out += "; intensity = light added on top (Lambert + GGX from the normal and ORM maps); 0 adds nothing\n";
		out += "; range     = how far it reaches, in native pixels at the light's depth\n";
		out += "; flicker   = none | fire | candle | pulse | strobe | fluorescent; amount = how deep (0-1); speed = how fast (1 = normal)\n";
		out += "; specular  = strength of the added light's highlights\n";
		out += "; shadow    = how much a character's shadow takes away from this light, 0-1 (left out: 1 for a new light, 0.5 for a painted one)\n";
		out += "; follows   = the background light group it follows (the animation id in the pupu: 004 01 000 is group 01): its colour\n";
		out += ";             follows the group's scripted colour (BGSHADE)\n";
		out += "; on_states = with follows: on only while the group shows one of these states (BGANIME), e.g. on_states = 1, 2\n";
		out += "; time      = day | night | both: when it is on (left out: both). Day and night lights cross-fade with DEMASTER's\n";
		out += ";             day/night clock (DAY_NIGHT)\n";
		out += "; source    = found: one of fieldprep's large lights; mark: your mark in _LIGHTS.png (both written again whenever\n";
		out += ";             fieldprep finds the lights again); yours (or left out): one you wrote here, which fieldprep keeps\n";
		out += "; Per time of day DEMASTER uses at most " + std::to_string(MAX_FOUND_LIGHTS) + " of fieldprep's and " + std::to_string(MAX_YOUR_LIGHTS)
			+ " of yours (marks count as yours). Changing which lights flicker or follow a group needs fieldprep\n; run again";
		out += " (the mask is in _LF).\n\n";
		out += Format("ambient = %.4f %.4f %.4f\n", lights.ambient[0], lights.ambient[1], lights.ambient[2]);
		//the glows (kept when fieldprep finds the lights again)
		const Emissive& e = lights.emissive;
		out += "\n; The field's glows (the emissive map in _ORME's alpha: lamps, screens, signs), for the whole field. By day they are in\n";
		out += "; the painting already; this is how they look once the room is dark (the day/night cycle; FIELD_PBR_VIEW=16 shows it now).\n";
		out += "; hue      = degrees their painted colour turns around the colour wheel (-180 to 180; 0: as painted)\n";
		out += "; tint     = red green blue they're multiplied by (linear; 1 1 1: as painted): colours white glows, which hue can't\n";
		out += "; strength = how bright (1: as painted)\n";
		out += "[emissive]\n";
		out += Format("hue = %.1f\n", e.hue);
		out += Format("tint = %.3f %.3f %.3f\n", e.tint[0], e.tint[1], e.tint[2]);
		out += Format("strength = %.3f\n", e.strength);
		//the night (kept when fieldprep finds the lights again)
		const Night& n = lights.night;
		out += "\n; The field at night (DEMASTER's day/night clock; time = day or night in _pbr.txt fixes it), for the whole field.\n";
		out += "; level = how much of the painted light is left away from the night's lamps (0-1)\n";
		out += "; tint  = red green blue what's left is multiplied by (linear): the moonlight's colour\n";
		out += "; lamps = how much of the painted light stays around the lamps on at night (time = night or both, baked = 1),\n";
		out += ";         within their range (0: none, 1: all of it)\n";
		out += "[night]\n";
		out += Format("level = %.3f\n", n.level);
		out += Format("tint = %.3f %.3f %.3f\n", n.tint[0], n.tint[1], n.tint[2]);
		out += Format("lamps = %.3f\n", n.lamps);
		for (const Light& l : lights.lights)
		{
			out += "\n";
			if (!l.note.empty())
			{
				std::istringstream in(l.note);
				std::string line;
				while (std::getline(in, line))
					out += "; " + line + "\n";
			}
			out += "[light]\n";
			out += "name = " + l.name + "\n";
			out += Format("position = %.1f %.1f %.4f\n", l.x, l.y, l.depth);
			out += Format("color = %.3f %.3f %.3f\n", l.color[0], l.color[1], l.color[2]);
			out += Format("baked = %d\n", l.baked ? 1 : 0);
			out += Format("intensity = %.3f\n", l.intensity);
			out += Format("range = %.1f\n", l.range);
			out += std::string("flicker = ") + FlickerName(l.flicker) + "\n";
			out += Format("amount = %.3f\n", l.amount);
			out += Format("speed = %.3f\n", l.speed);
			out += Format("specular = %.3f\n", l.specular);
			if (l.shadow >= 0)
				out += Format("shadow = %.3f\n", l.shadow);
			out += std::string("time = ") + LightTimeName(l.time) + "\n";
			out += std::string("source = ") + LightSourceName(l.source) + "\n";
			if (l.follows >= 0)
			{
				out += Format("follows = %02X\n", l.follows);
				if (!l.onStates.empty())
				{
					std::string states;
					for (const int s : l.onStates)
						states += (states.empty() ? "" : ", ") + std::to_string(s);
					out += "on_states = " + states + "\n";
				}
			}
		}
		return out;
	}

	//--- the light grid

	uint16_t FloatToHalf(const float f)
	{
		uint32_t x;
		memcpy(&x, &f, 4);
		const uint32_t sign = (x >> 16) & 0x8000u;
		const int32_t exponent = static_cast<int32_t>((x >> 23) & 0xFF) - 127 + 15;
		uint32_t mantissa = x & 0x7FFFFFu;
		if (((x >> 23) & 0xFF) == 0xFF)
			return static_cast<uint16_t>(sign | 0x7C00u | (mantissa ? 0x200u : 0u));
		if (exponent <= 0)
		{
			if (exponent < -10)
				return static_cast<uint16_t>(sign);
			mantissa |= 0x800000u;
			const uint32_t shift = static_cast<uint32_t>(14 - exponent);
			uint32_t half = mantissa >> shift;
			if ((mantissa >> (shift - 1)) & 1u)
				half++;
			return static_cast<uint16_t>(sign | half);
		}
		if (exponent >= 31)
			return static_cast<uint16_t>(sign | 0x7C00u);
		uint32_t half = sign | static_cast<uint32_t>(exponent) << 10 | mantissa >> 13;
		if (mantissa & 0x1000u)
			half++; //round (a carry into the exponent is still right)
		return static_cast<uint16_t>(half);
	}

	float HalfToFloat(const uint16_t h)
	{
		const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
		uint32_t exponent = (h >> 10) & 0x1Fu;
		uint32_t mantissa = h & 0x3FFu;
		uint32_t x;
		if (exponent == 0)
		{
			if (mantissa == 0)
				x = sign;
			else
			{
				exponent = 127 - 15 + 1;
				while (!(mantissa & 0x400u))
				{
					mantissa <<= 1;
					exponent--;
				}
				mantissa &= 0x3FFu;
				x = sign | exponent << 23 | mantissa << 13;
			}
		}
		else if (exponent == 31)
			x = sign | 0x7F800000u | mantissa << 13;
		else
			x = sign | (exponent - 15 + 127) << 23 | mantissa << 13;
		float f;
		memcpy(&f, &x, 4);
		return f;
	}

	namespace
	{
		void Put32(std::vector<uint8_t>& out, const uint32_t v)
		{
			for (int i = 0; i < 4; i++)
				out.push_back(static_cast<uint8_t>(v >> (i * 8)));
		}
		uint32_t Get32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24; }
	}

	//DDS with the DX10 header: a 3D texture, R16G16B16A16_FLOAT (DXGI 10), nx x ny x (4 nz)
	std::vector<uint8_t> WriteProbes(const ProbeGrid& g)
	{
		std::vector<uint8_t> out;
		if (!g.IsValid())
			return out;
		const uint32_t depth = static_cast<uint32_t>(g.nz) * 4;
		Put32(out, 0x20534444); //"DDS "
		Put32(out, 124);
		Put32(out, 0x1 | 0x2 | 0x4 | 0x8 | 0x1000 | 0x800000); //caps, height, width, pitch, pixel format, depth
		Put32(out, static_cast<uint32_t>(g.ny));
		Put32(out, static_cast<uint32_t>(g.nx));
		Put32(out, static_cast<uint32_t>(g.nx) * 8);
		Put32(out, depth);
		Put32(out, 1); //mip levels
		for (int i = 0; i < 11; i++)
			Put32(out, 0);
		Put32(out, 32); //pixel format
		Put32(out, 0x4); //DDPF_FOURCC
		Put32(out, 0x30315844); //"DX10"
		for (int i = 0; i < 5; i++)
			Put32(out, 0);
		Put32(out, 0x1000 | 0x8); //DDSCAPS_TEXTURE | DDSCAPS_COMPLEX
		Put32(out, 0x200000); //DDSCAPS2_VOLUME
		Put32(out, 0);
		Put32(out, 0);
		Put32(out, 0);
		Put32(out, 10); //DXGI_FORMAT_R16G16B16A16_FLOAT
		Put32(out, 4); //D3D10_RESOURCE_DIMENSION_TEXTURE3D
		Put32(out, 0);
		Put32(out, 1);
		Put32(out, 0);
		for (int block = 0; block < 4; block++)
			for (int z = 0; z < g.nz; z++)
				for (int y = 0; y < g.ny; y++)
					for (int x = 0; x < g.nx; x++)
					{
						const float* p = g.At(block, x, y, z);
						for (int c = 0; c < 4; c++)
						{
							const uint16_t h = FloatToHalf(p[c]);
							out.push_back(static_cast<uint8_t>(h));
							out.push_back(static_cast<uint8_t>(h >> 8));
						}
					}
		return out;
	}

	bool ReadProbes(const uint8_t* data, const size_t size, ProbeGrid& out)
	{
		if (data == nullptr || size < 148 || Get32(data) != 0x20534444 || Get32(data + 4) != 124 || Get32(data + 84) != 0x30315844)
			return false;
		const uint32_t height = Get32(data + 12), width = Get32(data + 16), depth = Get32(data + 24);
		if (Get32(data + 128) != 10 || Get32(data + 132) != 4 || width == 0 || height == 0 || depth == 0 || depth % 4 != 0
			|| width > 512 || height > 512 || depth > 1024)
			return false;
		const size_t texels = static_cast<size_t>(width) * height * depth;
		if (size < 148 + texels * 8)
			return false;
		ProbeGrid g;
		g.nx = static_cast<int>(width);
		g.ny = static_cast<int>(height);
		g.nz = static_cast<int>(depth / 4);
		g.data.resize(texels * 4);
		const uint8_t* p = data + 148;
		for (size_t i = 0; i < texels * 4; i++)
			g.data[i] = HalfToFloat(static_cast<uint16_t>(p[i * 2] | p[i * 2 + 1] << 8));
		out = std::move(g);
		return true;
	}

	namespace
	{
		uint32_t Hash(uint32_t x)
		{
			x ^= x >> 16;
			x *= 0x7FEB352Du;
			x ^= x >> 15;
			x *= 0x846CA68Bu;
			x ^= x >> 16;
			return x;
		}

		//-1..1
		double Random(const uint32_t seed, const int64_t i)
		{
			const uint32_t h = Hash(seed ^ Hash(static_cast<uint32_t>(i) ^ Hash(static_cast<uint32_t>(i >> 32) + 0x9E3779B9u)));
			return h / 2147483647.5 - 1.0;
		}

		//smooth value noise, -1..1
		double Noise(const uint32_t seed, const double t)
		{
			const double f = std::floor(t);
			const double u = t - f;
			const double s = u * u * (3.0 - 2.0 * u);
			const int64_t i = static_cast<int64_t>(f);
			return Random(seed, i) * (1.0 - s) + Random(seed, i + 1) * s;
		}
	}

	double FlickerValue(const Flicker flicker, const float amount, const float speed, const uint32_t seed, const double seconds)
	{
		const double a = std::clamp(static_cast<double>(amount), 0.0, 1.0);
		const double t = seconds * std::max(0.0f, speed);
		double v = 1.0;
		switch (flicker)
		{
		case Flicker::Fire:
			v = 1.0 + a * (0.55 * Noise(seed, t * 7.0) + 0.3 * Noise(seed + 1, t * 15.0) + 0.15 * Noise(seed + 2, t * 31.0));
			break;
		case Flicker::Candle:
			v = 1.0 + a * (0.8 * Noise(seed, t * 2.5) + 0.2 * Noise(seed + 1, t * 11.0));
			break;
		case Flicker::Pulse:
			v = 1.0 + a * std::sin(t * 3.14159265358979);
			break;
		case Flicker::Strobe:
			v = t - std::floor(t) < 0.5 ? 1.0 : 1.0 - a;
			break;
		case Flicker::Fluorescent:
		{
			//steady, but in most 2-second spells a short burst (0.35 s) of quick drop-outs somewhere
			const int64_t spell = static_cast<int64_t>(std::floor(t / 2.0));
			const double into = t - spell * 2.0;
			const double start = (Random(seed + 11, spell) * 0.5 + 0.5) * 1.6;
			if (Random(seed + 7, spell) > -0.4 && into >= start && into < start + 0.35)
				v = Random(seed, static_cast<int64_t>(std::floor(t / 0.05))) > 0.0 ? 1.0 - a : 1.0;
			break;
		}
		default:
			break;
		}
		return std::max(0.0, v);
	}

	uint32_t LightSeed(const Light& light, const size_t index)
	{
		uint32_t h = 2166136261u;
		for (const char c : light.name)
			h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
		return Hash(h ^ static_cast<uint32_t>(index * 0x9E3779B9u));
	}
	//--- day and night

	DayNightCycle DayNightCycle::Checked() const
	{
		DayNightCycle c = *this;
		const auto ok = [](const double v, const double fallback) { return std::isfinite(v) && v > 0 ? v : fallback; };
		c.day = ok(day, 480);
		c.night = ok(night, 240);
		c.fade = std::min(std::isfinite(fade) && fade >= 0 ? fade : 40.0, std::min(c.day, c.night));
		c.battleSlower = ok(battleSlower, 3);
		return c;
	}

	namespace
	{
		double Wrap(const double time, const double length)
		{
			if (!std::isfinite(time))
				return 0;
			double t = std::fmod(time, length);
			if (t < 0)
				t += length;
			return t >= length ? 0 : t;
		}

		double Smooth(const double x)
		{
			const double t = std::clamp(x, 0.0, 1.0);
			return t * t * (3.0 - 2.0 * t);
		}
	}

	double NightAmount(const DayNightCycle& cycle, const double time)
	{
		const DayNightCycle c = cycle.Checked();
		const double t = Wrap(time, c.Length()), half = c.fade / 2;
		if (c.fade <= 0)
			return t < c.day ? 0.0 : 1.0;
		if (t < half)
			return 1.0 - Smooth((t + half) / c.fade); //the end of dawn's fade
		if (t < c.day - half)
			return 0.0;
		if (t < c.day + half)
			return Smooth((t - (c.day - half)) / c.fade); //dusk
		if (t < c.Length() - half)
			return 1.0;
		return 1.0 - Smooth((t - (c.Length() - half)) / c.fade); //the start of dawn's fade
	}

	double AdvanceClock(const DayNightCycle& cycle, const double time, const double seconds, const bool battle)
	{
		const DayNightCycle c = cycle.Checked();
		const double step = std::isfinite(seconds) && seconds > 0 ? seconds : 0.0;
		return Wrap(time + (battle ? step / c.battleSlower : step), c.Length());
	}

	double SwapTime(const DayNightCycle& cycle, const double time)
	{
		const DayNightCycle c = cycle.Checked();
		const double half = c.fade / 2;
		//mostly day now: the night's first full second; mostly night: the day's
		return NightAmount(c, time) < 0.5 ? Wrap(c.day + half, c.Length()) : half;
	}

	double BeforeNextFade(const DayNightCycle& cycle, const double time, const double lead)
	{
		const DayNightCycle c = cycle.Checked();
		const double t = Wrap(time, c.Length()), half = c.fade / 2;
		//fades start at day - half (dusk) and length - half (dawn)
		const double dusk = c.day - half, dawn = c.Length() - half;
		const double next = t < dusk ? dusk : t < dawn ? dawn : c.Length() + dusk;
		return Wrap(next - std::max(0.0, lead), c.Length());
	}

	std::string ClockText(const DayNightCycle& cycle, const double time)
	{
		const DayNightCycle c = cycle.Checked();
		const double t = Wrap(time, c.Length()), half = c.fade / 2;
		const auto mmss = [](const double seconds)
		{
			const long s = static_cast<long>(std::floor(std::max(0.0, seconds)));
			return Format("%ld:%02ld", s / 60, s % 60);
		};
		const double night = NightAmount(c, t);
		if (t >= c.day - half && t < c.day + half)
			return Format("dusk (%.0f%% night), night in %s", 100.0 * night, mmss(c.day + half - t).c_str());
		if (t >= c.Length() - half || t < half)
			return Format("dawn (%.0f%% night), day in %s", 100.0 * night, mmss(t < half ? half - t : c.Length() - t + half).c_str());
		if (t < c.day)
			return "day, " + mmss(t - half) + " in (dusk starts in " + mmss(c.day - half - t) + ")";
		return "night, " + mmss(t - (c.day + half)) + " in (dawn starts in " + mmss(c.Length() - half - t) + ")";
	}

	bool ParseClock(const std::string& text, double& time)
	{
		std::string warnings;
		for (const KeyValue& kv : ReadKeyValues(text, warnings))
			if (kv.key == "time")
			{
				double v = 0;
				if (Number(kv.value, v) && std::isfinite(v))
				{
					time = v;
					return true;
				}
			}
		return false;
	}

	std::string WriteClock(const DayNightCycle& cycle, const double time)
	{
		const DayNightCycle c = cycle.Checked();
		const double t = Wrap(time, c.Length());
		std::string out;
		out += "; DEMASTER's day/night clock (DAY_NIGHT in demaster.conf). DEMASTER writes this file while the game runs; to set the\n";
		out += "; time yourself, edit it while the game is closed.\n";
		out += Format("; time = seconds into the cycle: 0 is the middle of dawn's fade, %.0f the middle of dusk's, %.0f the next dawn\n",
			c.day, c.Length());
		out += Format("; now: %s\n", ClockText(c, t).c_str());
		out += Format("time = %.1f\n", t);
		return out;
	}
}
