#pragma once
//SCENE_LIGHT's rules (no game, no Windows, no OpenGL): the time of day and colour overrides for battles and the world map.
//
//Battles and the world map are drawn by the game's main.frag from textures times vertex colours; there is no lighting to
//redo as in PBR fields. What DEMASTER changes is in DEMASTER_EXP\shaders\main.frag: the picture is multiplied by a colour
//(in linear light), and a battle stage page can have a picture of its glows (<page>_EC, as fields have) and an ORM map
//whose AO darkens it. This file says what that colour is: from the day/night clock, the place's settings in
//DEMASTER_EXP\scene_light.txt, and rules there that apply to some stages or some part of the story (the Lunar Cry).
//texturepatch_v2_sceneLight.cpp is the DLL's side; tests/field_pbr_test.cpp and tests/scene_shader_test.cpp test this.
//
//scene_light.txt ("; comment", "key = value", sections in [brackets]):
//	[battle] / [world]   the place's own look
//	  night       = auto | 0 | 1   does the night come here. auto: the world map always; a battle when it started on the
//	                               world map, in a field open to the sky (sky = 1 in its _pbr.txt) or in a field whose
//	                               _pbr.txt says time = night. Battles from other fields stay as painted.
//	  time        = cycle | day | night   cycle: the clock's; day / night: always that
//	  day_tint    = r g b   the picture times this by day (linear; 1 1 1: as painted)
//	  night_tint  = r g b   by night: times night_level x this
//	  night_level = 0-1
//	  ao          = 0-1     how much of an ORM map's AO is used
//	  glow        = 0-4     how bright the glows' picture is (1: as painted)
//	[rule <name>]        the same keys (only the ones written change anything), for where its conditions hold:
//	  where  = battle | world | both      (left out: both)
//	  stage  = 12, 40-45                  battle stages (a0stgNNN); left out: every stage. With stages: battles only
//	  moment = 3000-65535                 the story's progress (the game's own counter); left out: always
//	Rules are applied in the file's order, later ones over earlier ones.

#include <string>
#include <utility>
#include <vector>

namespace scenelight
{
	enum class Where { Battle, World };
	enum class Time { Cycle, Day, Night };

	struct Look
	{
		int night = -1; //-1 auto, 0 never, 1 the night comes here
		Time time = Time::Cycle;
		float dayTint[3] = { 1.0f, 1.0f, 1.0f };
		float nightTint[3] = { 0.55f, 0.65f, 1.0f };
		float nightLevel = 0.3f;
		float ao = 0.5f;
		float glow = 1.0f;
	};

	struct Rule
	{
		std::string name;
		bool battle = true, world = true;
		std::vector<std::pair<int, int>> stages; //inclusive ranges; empty: every stage
		int momentFrom = -1, momentTo = -1; //inclusive; -1: no condition
		Look look; //only the members flagged below are used
		bool hasNight = false, hasTime = false, hasDayTint = false, hasNightTint = false, hasNightLevel = false, hasAo = false,
			hasGlow = false;
	};

	struct Settings
	{
		Look battle, world;
		std::vector<Rule> rules;
	};

	//warnings: one line each ("line 12: ..."), empty when all was understood. Unknown keys are skipped
	void Parse(const std::string& text, Settings& settings, std::string& warnings);
	//the file DEMASTER writes when there is none: the defaults, explained, with a rule for the Lunar Cry
	std::string DefaultText();

	//The look in effect. stage: the battle stage (-1: not known); moment: the story's progress (-1: not known: rules with
	//a moment don't apply)
	Look Resolve(const Settings& settings, Where where, int stage, int moment);
	//How night it is here (0-1). clock: what the day/night clock says (0 day, 1 night); outdoors: what "night = auto"
	//means for this battle (see above; the world map: true)
	double NightHere(const Look& look, double clock, bool outdoors);
	//What the picture is multiplied by, in linear light: day_tint by day, night_level x night_tint by night
	void Multiplier(const Look& look, double night, float out[3]);
}
