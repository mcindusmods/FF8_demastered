#include "scene_light.h"
#include "field_moving_lights.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace scenelight
{
	namespace
	{
		std::string Trim(const std::string& s)
		{
			size_t a = 0, b = s.size();
			while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
				a++;
			while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
				b--;
			return s.substr(a, b - a);
		}

		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return s;
		}

		//count numbers, nothing after them
		bool Numbers(const std::string& value, double* out, const int count)
		{
			std::istringstream in(value);
			for (int i = 0; i < count; i++)
				if (!(in >> out[i]))
					return false;
			std::string rest;
			return !(in >> rest);
		}

		//"12, 40-45" -> ranges; false: not understood
		bool Ranges(const std::string& value, std::vector<std::pair<int, int>>& out)
		{
			out.clear();
			std::istringstream in(value);
			std::string part;
			while (std::getline(in, part, ','))
			{
				part = Trim(part);
				if (part.empty())
					continue;
				char* end = nullptr;
				const long from = std::strtol(part.c_str(), &end, 10);
				if (end == part.c_str() || from < 0)
					return false;
				long to = from;
				std::string rest = Trim(end);
				if (!rest.empty())
				{
					if (rest[0] != '-')
						return false;
					rest = Trim(rest.substr(1));
					char* end2 = nullptr;
					to = std::strtol(rest.c_str(), &end2, 10);
					if (end2 == rest.c_str() || !Trim(end2).empty() || to < from)
						return false;
				}
				out.emplace_back(static_cast<int>(from), static_cast<int>(to));
			}
			return !out.empty();
		}

		//"x up z  r g b  range [night|day]"
		bool ReadLight(const std::string& value, Light& light)
		{
			std::istringstream in(value);
			double v[7];
			for (double& number : v)
				if (!(in >> number))
					return false;
			std::string when;
			if (in >> when)
			{
				when = Lower(when);
				if (when == "night")
					light.when = 1;
				else if (when == "day")
					light.when = 2;
				else if (when != "always")
					return false;
				std::string rest;
				if (in >> rest)
					return false;
			}
			if (v[3] < 0 || v[4] < 0 || v[5] < 0 || v[6] <= 0)
				return false;
			light.x = static_cast<float>(v[0]);
			light.up = static_cast<float>(v[1]);
			light.z = static_cast<float>(v[2]);
			for (int i = 0; i < 3; i++)
				light.colour[i] = static_cast<float>(std::min(v[3 + i], 64.0));
			light.range = static_cast<float>(v[6]);
			return true;
		}

		//one of a look's keys. 0: not one of them, 1: set, -1: its value isn't understood
		int SetLook(const std::string& key, const std::string& value, Look& look, Rule* rule)
		{
			double v[3];
			const std::string word = Lower(value);
			if (key == "night")
			{
				if (word == "auto")
					look.night = -1;
				else if (word == "0" || word == "1")
					look.night = word == "1";
				else
					return -1;
				if (rule != nullptr)
					rule->hasNight = true;
			}
			else if (key == "time")
			{
				if (word == "cycle")
					look.time = Time::Cycle;
				else if (word == "day")
					look.time = Time::Day;
				else if (word == "night")
					look.time = Time::Night;
				else
					return -1;
				if (rule != nullptr)
					rule->hasTime = true;
			}
			else if (key == "day_tint" || key == "night_tint")
			{
				if (!Numbers(value, v, 3) || v[0] < 0 || v[1] < 0 || v[2] < 0)
					return -1;
				float* tint = key == "day_tint" ? look.dayTint : look.nightTint;
				for (int i = 0; i < 3; i++)
					tint[i] = static_cast<float>(std::min(v[i], 4.0));
				if (rule != nullptr)
					(key == "day_tint" ? rule->hasDayTint : rule->hasNightTint) = true;
			}
			else if (key == "night_level" || key == "ao")
			{
				if (!Numbers(value, v, 1) || v[0] < 0 || v[0] > 1)
					return -1;
				(key == "ao" ? look.ao : look.nightLevel) = static_cast<float>(v[0]);
				if (rule != nullptr)
					(key == "ao" ? rule->hasAo : rule->hasNightLevel) = true;
			}
			else if (key == "glow")
			{
				if (!Numbers(value, v, 1) || v[0] < 0 || v[0] > 4)
					return -1;
				look.glow = static_cast<float>(v[0]);
				if (rule != nullptr)
					rule->hasGlow = true;
			}
			else if (key == "sun")
			{
				if (!Numbers(value, v, 1) || v[0] < 0 || v[0] > 1)
					return -1;
				look.sun = static_cast<float>(v[0]);
				if (rule != nullptr)
					rule->hasSun = true;
			}
			else if (key == "fighter_sun" || key == "shadow")
			{
				if (!Numbers(value, v, 1) || v[0] < 0 || v[0] > 1)
					return -1;
				(key == "shadow" ? look.shadow : look.fighterSun) = static_cast<float>(v[0]);
				if (rule != nullptr)
					(key == "shadow" ? rule->hasShadow : rule->hasFighterSun) = true;
			}
			else if (key == "relief" || key == "specular")
			{
				if (!Numbers(value, v, 1) || v[0] < 0 || v[0] > 4)
					return -1;
				(key == "relief" ? look.relief : look.specular) = static_cast<float>(v[0]);
				if (rule != nullptr)
					(key == "relief" ? rule->hasRelief : rule->hasSpecular) = true;
			}
			else if (key == "sun_azimuth")
			{
				if (!Numbers(value, v, 1) || v[0] < -360 || v[0] > 360)
					return -1;
				look.sunAzimuth = static_cast<float>(v[0]);
				if (rule != nullptr)
					rule->hasSunAzimuth = true;
			}
			else if (key == "sun_height")
			{
				if (!Numbers(value, v, 1) || v[0] < 5 || v[0] > 90)
					return -1;
				look.sunHeight = static_cast<float>(v[0]);
				if (rule != nullptr)
					rule->hasSunHeight = true;
			}
			else
				return 0;
			return 1;
		}
	}

	void Parse(const std::string& text, Settings& settings, std::string& warnings)
	{
		settings = Settings();
		std::istringstream in(text);
		std::string line;
		int number = 0;
		Look* look = nullptr;
		Rule* rule = nullptr;
		bool skip = true; //before the first section, and inside one that isn't known
		while (std::getline(in, line))
		{
			number++;
			if (const size_t comment = line.find(';'); comment != std::string::npos)
				line.resize(comment);
			line = Trim(line);
			if (line.empty())
				continue;
			const std::string where = "line " + std::to_string(number) + ": ";
			if (line.front() == '[')
			{
				const size_t close = line.find(']');
				const std::string section = Trim(close == std::string::npos ? line.substr(1) : line.substr(1, close - 1));
				const std::string lower = Lower(section);
				look = nullptr;
				rule = nullptr;
				skip = false;
				if (lower == "battle")
					look = &settings.battle;
				else if (lower == "world")
					look = &settings.world;
				else if (lower.rfind("rule", 0) == 0 && (lower.size() == 4 || std::isspace(static_cast<unsigned char>(lower[4]))))
				{
					settings.rules.emplace_back();
					rule = &settings.rules.back();
					rule->name = Trim(section.substr(4));
					look = &rule->look;
				}
				else
				{
					skip = true;
					warnings += where + "unknown section [" + section + "] ([battle], [world], [rule <name>])\n";
				}
				continue;
			}
			const size_t equals = line.find('=');
			if (equals == std::string::npos)
			{
				warnings += where + "not \"key = value\"\n";
				continue;
			}
			if (skip || look == nullptr)
				continue;
			const std::string key = Lower(Trim(line.substr(0, equals))), value = Trim(line.substr(equals + 1));
			int result = SetLook(key, value, *look, rule);
			if (result == 0 && rule != nullptr)
			{
				result = 1;
				const std::string word = Lower(value);
				if (key == "where")
				{
					if (word == "battle" || word == "world" || word == "both")
					{
						rule->battle = word != "world";
						rule->world = word != "battle";
					}
					else
						result = -1;
				}
				else if (key == "stage")
					result = Ranges(value, rule->stages) ? 1 : -1;
				else if (key == "light")
				{
					Light light;
					if (!ReadLight(value, light))
						result = -1;
					else if (rule->look.lights.size() >= MAX_LIGHTS)
						warnings += where + "a stage has four lights at most: this one is left out\n";
					else
						rule->look.lights.push_back(light);
				}
				else if (key == "moment")
				{
					std::vector<std::pair<int, int>> one;
					if (Ranges(value, one) && one.size() == 1)
					{
						rule->momentFrom = one[0].first;
						rule->momentTo = one[0].second;
					}
					else
						result = -1;
				}
				else
					result = 0;
			}
			if (result == 0)
				warnings += where + "unknown setting " + key + "\n";
			else if (result < 0)
				warnings += where + "can't read " + key + " = " + value + "\n";
		}
	}

	std::string DefaultText()
	{
		return
			"; DEMASTER: the time of day and colour overrides in battles and on the world map (SCENE_LIGHT in demaster.conf).\n"
			"; Read again within a second of saving it. Colours are red green blue in linear light: 1 1 1 changes nothing.\n"
			";\n"
			"; night       = auto | 0 | 1   does the night come here. auto: the world map always; a battle when it started on\n"
			";                              the world map or in a field open to the sky (sky = 1 in its _pbr.txt). Battles from\n"
			";                              other fields stay as painted; from a field with a fixed time (time = day or night\n"
			";                              in its _pbr.txt) they are fought by day.\n"
			"; time        = cycle | day | night   cycle: the day/night clock; day or night: always that\n"
			"; day_tint    = what the picture is multiplied by in daylight\n"
			"; night_tint, night_level = by night the picture is multiplied by night_level x night_tint\n"
			"; ao          = how much of a stage page's ORM map's AO is used (0-1)\n"
			"; glow        = how bright a stage page's picture of its glows is (<page>_EC; 1: as painted)\n"
			"\n"
			"[battle]\n"
			"night = auto\n"
			"time = cycle\n"
			"day_tint = 1 1 1\n"
			"night_tint = 0.55 0.65 1\n"
			"night_level = 0.3\n"
			"ao = 0.5\n"
			"glow = 1\n"
			"; The sun and the moon cross the stage by the clock (the camera's place in the stage is read from the game):\n"
			"; sun         = how much a surface turned away from it darkens (0-1; 0: the stage as painted)\n"
			"; relief      = how strongly a stage page's normal map (<page>_N) shows in its light (0-4)\n"
			"; specular    = its highlight on pages with an ORM map (0-4)\n"
			"; sun_azimuth = where it rises, degrees around the stage (0: along the stage's +x); sun_height = how high it climbs\n"
			"; fighter_sun = how much of that darkening the fighters get (0-1; their normals are smoothed; 0: as the game lights them)\n"
			"; shadow      = how much of the sun the fighters' shadows take off the stage (0-1; 0: no shadows)\n"
			"sun = 0.35\n"
			"fighter_sun = 0.5\n"
			"shadow = 0.5\n"
			"relief = 1\n"
			"specular = 0.5\n"
			"sun_azimuth = 30\n"
			"sun_height = 60\n"
			"\n"
			"[world]\n"
			"night = auto\n"
			"time = cycle\n"
			"day_tint = 1 1 1\n"
			"night_tint = 0.55 0.65 1\n"
			"night_level = 0.3\n"
			"\n"
			"; Rules change the settings above where their conditions hold. Only the keys written in a rule change anything.\n"
			";   where  = battle | world | both     (left out: both)\n"
			";   stage  = 12, 40-45                 battle stages by number (a0stg012 is 12); left out: every stage. A rule\n"
			";                                      with stages is for battles only\n"
			";   moment = 3000-65535                the story's progress, the game's own counter (the log says what it is when a\n"
			";                                      battle or the world map starts, with SCENE_DEBUG=1); left out: always\n"
			";   light  = x up z  r g b  range [night|day]\n"
			";            a light of the stage's own, four per stage at most. x and z: its place on the stage's floor, in the\n"
			";            stage's units (0 0 is the middle; the fighters stand a few thousand apart); up: how far above the\n"
			";            floor; r g b: its colour (linear, may be above 1); range: how far it reaches; night or day: only then\n"
			"; Later rules go over earlier ones.\n"
			"\n"
			"; The Lunar Cry: off until the story counter's value there is known (from the game's code). Then take the ; away\n"
			"; and set its moment; the tints make the night red instead of blue.\n"
			";[rule lunar cry]\n"
			";where = both\n"
			";moment = 3000-65535\n"
			";day_tint = 1 0.94 0.92\n"
			";night_tint = 1 0.5 0.45\n"
			"\n"
			"; Examples (take the ; away):\n"
			";[rule a cave]\n"
			";where = battle\n"
			";stage = 7, 20-22\n"
			";night = 0\n"
			";[rule torches]\n"
			";where = battle\n"
			";stage = 12\n"
			";light = -1500 600 800   3 1.6 0.6   4000 night\n"
			";light = 1500 600 800    3 1.6 0.6   4000 night\n"
			";[rule always dusk]\n"
			";where = battle\n"
			";stage = 31\n"
			";time = night\n"
			";night_level = 0.6\n"
			";night_tint = 1 0.7 0.5\n";
	}

	Look Resolve(const Settings& settings, const Where where, const int stage, const int moment)
	{
		Look look = where == Where::Battle ? settings.battle : settings.world;
		for (const Rule& rule : settings.rules)
		{
			if (!(where == Where::Battle ? rule.battle : rule.world))
				continue;
			if (!rule.stages.empty())
			{
				bool in = false; //(a rule for some stages is not for the world map)
				for (const auto& [from, to] : rule.stages)
					in |= where == Where::Battle && stage >= from && stage <= to;
				if (!in)
					continue;
			}
			if (rule.momentFrom >= 0 && (moment < rule.momentFrom || moment > rule.momentTo))
				continue;
			if (rule.hasNight)
				look.night = rule.look.night;
			if (rule.hasTime)
				look.time = rule.look.time;
			if (rule.hasDayTint)
				std::copy(rule.look.dayTint, rule.look.dayTint + 3, look.dayTint);
			if (rule.hasNightTint)
				std::copy(rule.look.nightTint, rule.look.nightTint + 3, look.nightTint);
			if (rule.hasNightLevel)
				look.nightLevel = rule.look.nightLevel;
			if (rule.hasAo)
				look.ao = rule.look.ao;
			if (rule.hasGlow)
				look.glow = rule.look.glow;
			if (rule.hasSun)
				look.sun = rule.look.sun;
			if (rule.hasRelief)
				look.relief = rule.look.relief;
			if (rule.hasFighterSun)
				look.fighterSun = rule.look.fighterSun;
			if (rule.hasShadow)
				look.shadow = rule.look.shadow;
			if (rule.hasSpecular)
				look.specular = rule.look.specular;
			if (rule.hasSunAzimuth)
				look.sunAzimuth = rule.look.sunAzimuth;
			if (rule.hasSunHeight)
				look.sunHeight = rule.look.sunHeight;
			if (where == Where::Battle)
				for (const Light& light : rule.look.lights)
					if (look.lights.size() < MAX_LIGHTS)
						look.lights.push_back(light);
		}
		return look;
	}

	double NightHere(const Look& look, const double clock, const bool outdoors)
	{
		if (look.time == Time::Day)
			return 0.0;
		if (look.time == Time::Night)
			return 1.0;
		if (look.night == 0 || (look.night < 0 && !outdoors))
			return 0.0;
		return std::clamp(clock, 0.0, 1.0);
	}

	Time TimeHere(const Look& look, const bool fixedPlace, const bool outdoors)
	{
		if (look.time != Time::Cycle)
			return look.time;
		if (fixedPlace || NightHere(look, 1.0, outdoors) <= 0.0)
			return Time::Day;
		return Time::Cycle;
	}

	void Multiplier(const Look& look, const double night, float out[3])
	{
		const float n = static_cast<float>(std::clamp(night, 0.0, 1.0));
		for (int i = 0; i < 3; i++)
			out[i] = look.dayTint[i] * (1.0f - n) + look.nightLevel * look.nightTint[i] * n;
	}

	//--- the battle camera

	bool CameraFromRegisters(const int32_t reg[32], Camera& camera)
	{
		const auto low = [](const int32_t v) { return static_cast<double>(static_cast<int16_t>(v & 0xFFFF)) / 4096.0; };
		const auto high = [](const int32_t v) { return static_cast<double>(static_cast<int16_t>((v >> 16) & 0xFFFF)) / 4096.0; };
		double* r = camera.r;
		r[0] = low(reg[0]); r[1] = high(reg[0]); r[2] = low(reg[1]);
		r[3] = high(reg[1]); r[4] = low(reg[2]); r[5] = high(reg[2]);
		r[6] = low(reg[3]); r[7] = high(reg[3]); r[8] = low(reg[4]);
		for (int i = 0; i < 3; i++)
		{
			const double length = std::sqrt(r[i * 3] * r[i * 3] + r[i * 3 + 1] * r[i * 3 + 1] + r[i * 3 + 2] * r[i * 3 + 2]);
			if (std::fabs(length - 1.0) > 0.02)
				return false;
			const int j = (i + 1) % 3;
			if (std::fabs(r[i * 3] * r[j * 3] + r[i * 3 + 1] * r[j * 3 + 1] + r[i * 3 + 2] * r[j * 3 + 2]) > 0.02)
				return false;
		}
		for (int i = 0; i < 3; i++)
			camera.t[i] = reg[5 + i];
		camera.centreX = reg[24] / 65536.0;
		camera.centreY = reg[25] / 65536.0;
		camera.distance = static_cast<double>(reg[26] & 0xFFFF);
		return camera.distance >= 16.0;
	}

	bool CameraFromMatrix(const uint8_t m[32], const Camera& projection, Camera& camera)
	{
		Camera c = projection;
		for (int i = 0; i < 9; i++)
		{
			int16_t v;
			memcpy(&v, m + i * 2, 2);
			c.r[i] = v / 4096.0;
		}
		for (int i = 0; i < 3; i++)
		{
			int32_t v;
			memcpy(&v, m + 20 + i * 4, 4);
			c.t[i] = v;
		}
		for (int i = 0; i < 3; i++)
		{
			const double* r = c.r;
			const double length = std::sqrt(r[i * 3] * r[i * 3] + r[i * 3 + 1] * r[i * 3 + 1] + r[i * 3 + 2] * r[i * 3 + 2]);
			const int j = (i + 1) % 3;
			if (std::fabs(length - 1.0) > 0.02 || std::fabs(r[i * 3] * r[j * 3] + r[i * 3 + 1] * r[j * 3 + 1] + r[i * 3 + 2] * r[j * 3 + 2]) > 0.02)
				return false;
		}
		if (!(c.distance >= 16.0))
			return false;
		camera = c;
		return true;
	}

	void CameraDifference(const Camera& a, const Camera& b, double& rotation, double& translation)
	{
		rotation = 0;
		translation = 0;
		for (int i = 0; i < 9; i++)
			rotation = std::max(rotation, std::fabs(a.r[i] - b.r[i]));
		for (int i = 0; i < 3; i++)
			translation = std::max(translation, std::fabs(a.t[i] - b.t[i]));
	}

	void DirectionToView(const Camera& c, const double s[3], double v[3])
	{
		for (int i = 0; i < 3; i++)
			v[i] = c.r[i * 3] * s[0] + c.r[i * 3 + 1] * s[1] + c.r[i * 3 + 2] * s[2];
	}

	void PlaceToView(const Camera& c, const double s[3], double v[3])
	{
		DirectionToView(c, s, v);
		for (int i = 0; i < 3; i++)
			v[i] += c.t[i];
	}

	void ScreenToView(const Camera& camera, const double x, const double y, const double rhw, double view[3])
	{
		const double z = rhw > 1e-12 ? 1.0 / rhw : 1e12;
		const double d = camera.distance > 0 ? camera.distance : 1.0;
		//the screen is 640 x 480, the PlayStation's pixels (the projection's) half of that
		view[0] = (x * 0.5 - camera.centreX) * z / d;
		view[1] = (y * 0.5 - camera.centreY) * z / d;
		view[2] = z;
	}

	const char* const GLOW_VERTEX_SHADER = "#version 120\nvarying float demasterGlowDepth;\nvoid main() {\n\tvec4 p = gl_Vertex;\n"
		"\tdemasterGlowDepth = p.w > 0.0 ? 1.0 / p.w : 0.0;\n\tgl_Position = vec4(p.x / 320.0 - 1.0, 1.0 - p.y / 240.0, 0.0, 1.0);\n"
		"\tgl_FrontColor = gl_Color.bgra;\n\tgl_TexCoord[0] = gl_MultiTexCoord0;\n}\n";
	const char* const GLOW_FRAGMENT_SHADER = "#version 120\nuniform sampler2D tex;\nuniform float demasterGlowAlpha;\nuniform float demasterGlowShare;\n"
		"varying float demasterGlowDepth;\nvoid main() {\n\tvec4 t = texture2D(tex, gl_TexCoord[0].st);\n"
		"\tvec3 light = pow(clamp(t.rgb * gl_Color.rgb, 0.0, 1.0), vec3(2.2)) * (mix(1.0, t.a, demasterGlowAlpha) * demasterGlowShare);\n"
		"\tfloat b = dot(light, vec3(0.2126, 0.7152, 0.0722));\n\tgl_FragData[0] = vec4(light, 0.0);\n"
		"\tgl_FragData[1] = vec4(b, b * demasterGlowDepth, 0.0, 0.0);\n}\n";

	std::vector<Glow> FindGlows(GlowGrid grid, const int most, const int radius, const double least)
	{
		std::vector<Glow> glows;
		const int w = grid.width, h = grid.height;
		if (w <= 0 || h <= 0 || grid.cells.size() != static_cast<size_t>(w) * h * 5)
			return glows;
		const double cellW = 640.0 / w, cellH = 480.0 / h;
		while (static_cast<int>(glows.size()) < most)
		{
			int best = -1;
			double brightest = least;
			for (int i = 0; i < w * h; i++)
				if (grid.cells[static_cast<size_t>(i) * 5 + 3] > brightest)
				{
					brightest = grid.cells[static_cast<size_t>(i) * 5 + 3];
					best = i;
				}
			if (best < 0)
				break;
			//the glow: the cells joined to the brightest one (side by side) that are at least a twentieth as bright, not
			//farther from it than `radius` cells; they are taken off the grid
			const int bx = best % w, by = best / w;
			const double floor = std::max(least, brightest / 20.0);
			Glow g;
			double sx = 0, sy = 0, sxx = 0, syy = 0, depth = 0;
			std::vector<int> stack{ best };
			std::vector<uint8_t> seen(static_cast<size_t>(w) * h, 0);
			seen[best] = 1;
			while (!stack.empty())
			{
				const int i = stack.back();
				stack.pop_back();
				const int x = i % w, y = i / w;
				float* c = &grid.cells[static_cast<size_t>(i) * 5];
				const double b = c[3];
				for (int k = 0; k < 3; k++)
					g.rgb[k] += c[k];
				g.brightness += b;
				depth += c[4];
				const double px = (x + 0.5) * cellW, py = (y + 0.5) * cellH;
				sx += b * px;
				sy += b * py;
				sxx += b * px * px;
				syy += b * py * py;
				for (int k = 0; k < 5; k++)
					c[k] = 0;
				const int next[4][2] = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } };
				for (const auto& n : next)
				{
					if (n[0] < 0 || n[1] < 0 || n[0] >= w || n[1] >= h || std::abs(n[0] - bx) > radius || std::abs(n[1] - by) > radius)
						continue;
					const int j = n[1] * w + n[0];
					if (!seen[j] && grid.cells[static_cast<size_t>(j) * 5 + 3] >= floor)
					{
						seen[j] = 1;
						stack.push_back(j);
					}
				}
			}
			if (g.brightness <= 0)
				break;
			g.x = sx / g.brightness;
			g.y = sy / g.brightness;
			g.depth = depth / g.brightness;
			g.spread = std::sqrt(std::max(0.0, (sxx / g.brightness - g.x * g.x) + (syy / g.brightness - g.y * g.y)));
			glows.push_back(g);
		}
		return glows;
	}

	EffectLight GlowToLight(const Camera& camera, const Glow& glow, const double strength)
	{
		EffectLight light;
		double view[3];
		ScreenToView(camera, glow.x, glow.y, glow.depth > 0 ? 1.0 / glow.depth : 0.0, view);
		PlaceToStage(camera, view, light.stage);
		double rgb[3], most = 0;
		for (int k = 0; k < 3; k++)
		{
			rgb[k] = glow.rgb[k] * strength / 16.0;
			most = std::max(most, rgb[k]);
		}
		const double scale = most > 4.0 ? 4.0 / most : 1.0;
		for (int k = 0; k < 3; k++)
			light.colour[k] = static_cast<float>(rgb[k] * scale);
		//its spread on the screen at its depth, in view units (the stage's: the camera only turns and moves)
		const double d = camera.distance > 0 ? camera.distance : 1.0;
		const double spread = glow.spread * 0.5 * std::max(glow.depth, 1.0) / d;
		light.range = static_cast<float>(std::clamp(3.0 * spread, 1500.0, 6000.0));
		return light;
	}

	void EffectLights::Frame(const std::vector<EffectLight>& seen, const double fade)
	{
		const auto brightness = [](const EffectLight& l) { return std::max({ l.colour[0], l.colour[1], l.colour[2] }); };
		const auto apart = [](const EffectLight& a, const EffectLight& b)
		{
			double s = 0;
			for (int k = 0; k < 3; k++)
				s += (a.stage[k] - b.stage[k]) * (a.stage[k] - b.stage[k]);
			return std::sqrt(s);
		};
		const double keep = std::clamp(fade, 0.0, 1.0);
		std::vector<EffectLight> next;
		std::vector<bool> used(lights.size(), false);
		for (const EffectLight& now : seen)
		{
			EffectLight l = now;
			int nearest = -1;
			double best = 1e30;
			for (size_t i = 0; i < lights.size(); i++)
			{
				const double a = apart(lights[i], now);
				if (!used[i] && a < std::max(lights[i].range, now.range) * 0.5 && a < best)
				{
					best = a;
					nearest = static_cast<int>(i);
				}
			}
			if (nearest >= 0)
			{
				used[nearest] = true;
				const EffectLight& old = lights[nearest];
				for (int k = 0; k < 3; k++)
					l.colour[k] = std::max(now.colour[k], static_cast<float>(old.colour[k] * keep));
				l.range = std::max(now.range, static_cast<float>(old.range * keep));
			}
			next.push_back(l);
		}
		for (size_t i = 0; i < lights.size(); i++)
		{
			if (used[i])
				continue;
			EffectLight l = lights[i];
			for (float& c : l.colour)
				c = static_cast<float>(c * keep);
			if (brightness(l) >= 0.02f)
				next.push_back(l);
		}
		std::sort(next.begin(), next.end(), [&](const EffectLight& a, const EffectLight& b) { return brightness(a) > brightness(b); });
		if (next.size() > MAX_EFFECT_LIGHTS)
			next.resize(MAX_EFFECT_LIGHTS);
		lights = std::move(next);
	}

	bool FitSunShadow(const double towards[3], const std::vector<float>& places, const double margin, float out[16])
	{
		const double length = std::sqrt(towards[0] * towards[0] + towards[1] * towards[1] + towards[2] * towards[2]);
		if (places.size() < 3 || !(length > 1e-9))
			return false;
		//the map's axes: f along the light (away from the sun), r and u across it
		const double f[3] = { -towards[0] / length, -towards[1] / length, -towards[2] / length };
		double helper[3] = { 0, 1, 0 };
		if (std::fabs(f[1]) > 0.9)
		{
			helper[0] = 1;
			helper[1] = 0;
		}
		double r[3] = { helper[1] * f[2] - helper[2] * f[1], helper[2] * f[0] - helper[0] * f[2], helper[0] * f[1] - helper[1] * f[0] };
		const double rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
		for (double& c : r)
			c /= rl;
		const double u[3] = { f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0] };
		double lo[3] = { 1e300, 1e300, 1e300 }, hi[3] = { -1e300, -1e300, -1e300 };
		for (size_t i = 0; i + 2 < places.size(); i += 3)
		{
			const double p[3] = { places[i], places[i + 1], places[i + 2] };
			const double a[3] = { p[0] * r[0] + p[1] * r[1] + p[2] * r[2], p[0] * u[0] + p[1] * u[1] + p[2] * u[2],
				p[0] * f[0] + p[1] * f[1] + p[2] * f[2] };
			for (int k = 0; k < 3; k++)
			{
				lo[k] = std::min(lo[k], a[k]);
				hi[k] = std::max(hi[k], a[k]);
			}
		}
		//square across (the map's texels stay square), the depth with room behind for the ground the shadows fall on
		const double half = std::max(hi[0] - lo[0], hi[1] - lo[1]) * 0.5 + margin;
		const double cx = (lo[0] + hi[0]) * 0.5, cy = (lo[1] + hi[1]) * 0.5;
		const double nearest = lo[2] - margin, farthest = hi[2] + margin * 8.0;
		if (!(half > 0) || !(farthest > nearest))
			return false;
		const double sx = 0.5 / half, sz = 1.0 / (farthest - nearest);
		//row k of the map: (axis . p) scaled and moved; column-major out
		const double rows[3][4] = {
			{ r[0] * sx, r[1] * sx, r[2] * sx, 0.5 - cx * sx },
			{ u[0] * sx, u[1] * sx, u[2] * sx, 0.5 - cy * sx },
			{ f[0] * sz, f[1] * sz, f[2] * sz, -nearest * sz } };
		for (int c = 0; c < 4; c++)
		{
			for (int k = 0; k < 3; k++)
				out[c * 4 + k] = static_cast<float>(rows[k][c]);
			out[c * 4 + 3] = c == 3 ? 1.0f : 0.0f;
		}
		return true;
	}

	void PlaceToStage(const Camera& c, const double v[3], double s[3])
	{
		const double d[3] = { v[0] - c.t[0], v[1] - c.t[1], v[2] - c.t[2] };
		for (int i = 0; i < 3; i++)
			s[i] = c.r[i] * d[0] + c.r[3 + i] * d[1] + c.r[6 + i] * d[2];
	}

	bool CameraFilter::Close(const Camera& a, const Camera& b)
	{
		//the rotations within about 15 degrees of each other (the sum of the rows' dot products is 3 for the same one), and
		//the places within a quarter of their distance from the stage's middle (and 2000 units)
		double same = 0, moved = 0, far = 0;
		for (int i = 0; i < 9; i++)
			same += a.r[i] * b.r[i];
		for (int i = 0; i < 3; i++)
		{
			moved += (a.t[i] - b.t[i]) * (a.t[i] - b.t[i]);
			far += a.t[i] * a.t[i];
		}
		return same > 2.93 && std::sqrt(moved) < 2000.0 + 0.25 * std::sqrt(far) && std::fabs(a.distance - b.distance) < 0.5 * a.distance;
	}

	bool CameraFilter::Frame(const bool readable, const Camera& read, Camera& use)
	{
		if (readable)
		{
			if (!has || Close(read, current))
			{
				current = read;
				has = true;
				hasPending = false;
			}
			else if (hasPending && Close(read, pending))
			{
				current = read; //a cut: two frames agree
				hasPending = false;
			}
			else
			{
				pending = read;
				hasPending = true;
			}
		}
		if (has)
			use = current;
		return has;
	}

	Sun SunOver(const Look& look, const bool night, const double phase)
	{
		Sun sun;
		sun.moon = night;
		//the stage's space has the same axes as a view (x right, y down, z away), so the fields' arc serves: up is -y
		const fieldpbr::Vec3 towards = fieldpbr::SunDirection({ 0, -1, 0 }, look.sunAzimuth, look.sunHeight, phase);
		sun.direction[0] = towards.x;
		sun.direction[1] = towards.y;
		sun.direction[2] = towards.z;
		const double elevation = -towards.y; //the sine of how high it stands
		fieldpbr::SunColor(elevation, night, sun.colour);
		sun.strength = fieldpbr::HorizonFade(elevation);
		return sun;
	}
}
