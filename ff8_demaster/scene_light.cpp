#include "scene_light.h"

#include <algorithm>
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
			";                              the world map, in a field open to the sky (sky = 1 in its _pbr.txt) or in a field\n"
			";                              whose _pbr.txt says time = night. Battles from other fields stay as painted.\n"
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
			"; Later rules go over earlier ones.\n"
			"\n"
			"; After the Lunar Cry (the counter is 3000 once the Ragnarok is yours): the night is red instead of blue.\n"
			"; Change the colours to taste, or delete the rule.\n"
			"[rule lunar cry]\n"
			"where = both\n"
			"moment = 3000-65535\n"
			"day_tint = 1 0.94 0.92\n"
			"night_tint = 1 0.5 0.45\n"
			"\n"
			"; Examples (take the ; away):\n"
			";[rule a cave]\n"
			";where = battle\n"
			";stage = 7, 20-22\n"
			";night = 0\n"
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

	void Multiplier(const Look& look, const double night, float out[3])
	{
		const float n = static_cast<float>(std::clamp(night, 0.0, 1.0));
		for (int i = 0; i < 3; i++)
			out[i] = look.dayTint[i] * (1.0f - n) + look.nightLevel * look.nightTint[i] * n;
	}
}
