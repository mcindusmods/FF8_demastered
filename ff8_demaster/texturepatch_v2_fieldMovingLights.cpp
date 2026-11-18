//The moving lights module (FIELD_MOVING_LIGHTS in demaster.conf): lights that aren't at a fixed place in a field's
//_lights.txt.
//  the sun          FIELD_SUN: crosses the sky with the day/night clock, on fields open to the sky (sky = 1 in _pbr.txt, or
//                   [sun] on = 1 in _lights.txt); at night the moon
//  the party light  FIELD_PARTY_LIGHT: carried by the party leader (1: at night, 2: always)
//
//How it fits together:
//  - field_moving_lights.h/.cpp   the maths, shared with the tests: where the sun is, which character is the leader, and
//                                 each as a light (fieldpbr::MovingLight)
//  - this file                    the DLL's side: asks the clock (day_night.h), keeps the leader the character module
//                                 reports, hands the lights to the PBR module
//  - texturepatch_v2_fieldPbr.cpp Lights(): puts them after the field's own lights, in the same arrays the shaders get
//                                 (LightUniforms). Nothing in the shaders is special to them.
//  - texturepatch_v2_fieldChars.cpp says each frame which characters it drew (MovingLightsCharacters), and gives the sun
//                                 shadow maps like any lamp that casts shadows
//Per-field settings: [sun] and [party] in <field>_lights.txt (README_field_pbr.md, "Moving lights").
//Everything here runs on the renderer thread.

#include "config.h"
#include "coreHeader.h"
#include "field_pbr_runtime.h"
#include "day_night.h"

#include <algorithm>
#include <string>

using namespace fieldrt;

namespace
{
	constexpr double PRESENCE_SECONDS = 0.35; //the party light comes on and goes out over this long (the leader appears, hides)
	constexpr uint64_t LEADER_FRAMES = 2; //the leader counts as seen for this many frames after the character module saw it

	uint64_t frame = 1; //SwapBuffers so far
	uint64_t serial = 1;
	//the leader, as the character module last saw it
	fieldpbr::LeaderTrack track;
	uint64_t trackEntry = 0;
	bool leaderKnown = false;
	uint64_t leaderEntry = 0, leaderFrame = 0;
	fieldpbr::LeaderCandidate leader;
	int loggedKey = -1;
	uint64_t loggedEntry = 0;
	double presence = 0; //0-1: how much of the party light is on (eased)
	ULONGLONG presenceTick = 0;

	bool SunWanted(const fieldpbr::Settings& settings, const fieldpbr::Lights& lights)
	{
		const fieldpbr::Sun& sun = lights.sun;
		return FIELD_SUN > 0 && (sun.on > 0 || (sun.on < 0 && settings.sky));
	}

	int PartyMode(const fieldpbr::Lights& lights)
	{
		if (!(FIELD_PARTY_LIGHT_STRENGTH > 0))
			return 0;
		return lights.party.on >= 0 ? lights.party.on : FIELD_PARTY_LIGHT;
	}
}

void fieldrt::MovingLightsCharacters(const uint64_t entry, const std::vector<fieldpbr::LeaderCandidate>& drawn, const int pcOfMost,
	const int controlled)
{
	if (!FIELD_MOVING_LIGHTS)
		return;
	if (entry != trackEntry)
	{
		track = fieldpbr::LeaderTrack();
		trackEntry = entry;
		leaderKnown = false;
		presence = 0;
	}
	const FieldView& view = PbrView();
	const int index = fieldpbr::ChooseLeader(drawn, view.partyFollow, pcOfMost, GetTickCount64() / 1000.0, track, controlled);
	if (index < 0)
		return;
	leader = drawn[static_cast<size_t>(index)];
	leaderKnown = true;
	leaderEntry = entry;
	leaderFrame = frame;
	serial++;
	if (loggedEntry != entry || loggedKey != leader.key)
	{
		loggedEntry = entry;
		loggedKey = leader.key;
		if (leader.entity >= 0)
		{
			char why[160] = "";
			if (view.partyFollow >= 0)
				snprintf(why, sizeof(why), ", as [party] follow says");
			else if (leader.entity == controlled)
				snprintf(why, sizeof(why), ": the one the player moves");
			else if (controlled >= 0)
				snprintf(why, sizeof(why), ": the one that moved last (the one the player moves, entity %d, is not drawn)", controlled);
			else
				snprintf(why, sizeof(why), ": the one that moved last (the game's word on who the player moves is not known)");
			Log("MOVING_LIGHTS: %s: the party light's carrier is script entity %d (playable-character byte %d; most entities here have %d)%s%s\n",
				view.name.c_str(), leader.entity, leader.pc, pcOfMost, why,
				view.partyFollow >= 0 ? "" : ". Another one: [party] follow = <entity> in _lights.txt");
		}
		else
			Log("MOVING_LIGHTS: %s: the party light's carrier is the character that moved last (the field's camera isn't measured, so "
				"its script entities aren't known)\n", view.name.c_str());
	}
}

bool fieldrt::MovingSunOut(const fieldpbr::Settings& settings, const fieldpbr::Lights& lights)
{
	return FIELD_MOVING_LIGHTS && FIELD_PBR && SunWanted(settings, lights);
}

int fieldrt::MovingLightsWanted(const fieldpbr::Settings& settings, const fieldpbr::Lights& lights, const double night)
{
	if (!FIELD_MOVING_LIGHTS || !FIELD_PBR)
		return 0;
	int wanted = SunWanted(settings, lights) ? 1 : 0;
	const int mode = PartyMode(lights);
	if (mode >= 2 || (mode == 1 && night > 0.001))
		wanted++;
	return std::min(wanted, MAX_MOVING_LIGHTS);
}

int fieldrt::MovingLightsNow(const FieldView& view, const fieldpbr::Settings& settings, const fieldpbr::Lights& lights, const double night,
	fieldpbr::MovingLight* out)
{
	if (!FIELD_MOVING_LIGHTS || !FIELD_PBR)
		return 0;
	int count = 0;
	if (SunWanted(settings, lights))
	{
		fieldpbr::Vec3 centre;
		double extent = 1.0;
		fieldpbr::FieldExtent(settings, view.canvasWidth, view.canvasHeight, centre, extent);
		//without the clock (DAY_NIGHT=0) every field is a day field: the sun stands at [sun] fixed
		const fieldpbr::FieldTime time = DAY_NIGHT ? settings.time : fieldpbr::FieldTime::Day;
		const fieldpbr::MovingLight sun = fieldpbr::SunLight(lights.sun, settings, time, DayNightSky(), night, centre, extent, FIELD_SUN);
		if (sun.on)
		{
			out[count++] = sun;
			LogOnce("moving-sun-" + std::to_string(view.entry), "MOVING_LIGHTS: %s: the sun is on (azimuth %.0f, height %.0f, intensity %.2f, "
				"shade %.2f, moon %.2f; [sun] in _lights.txt)\n", view.name.c_str(), lights.sun.azimuth, lights.sun.height, lights.sun.intensity,
				lights.sun.shade, lights.sun.moon);
		}
	}
	const int mode = PartyMode(lights);
	if (mode > 0 && leaderKnown && leaderEntry == view.entry && presence > 0.001 && count < MAX_MOVING_LIGHTS)
	{
		const fieldpbr::MovingLight carried = fieldpbr::PartyLightFor(lights.party, leader, settings.space, mode, night,
			FIELD_PARTY_LIGHT_STRENGTH * presence);
		if (carried.on)
			out[count++] = carried;
	}
	return count;
}

uint64_t fieldrt::MovingLightsSerial()
{
	return serial;
}

void fieldrt::MovingLightsEndFrame()
{
	if (!FIELD_MOVING_LIGHTS)
		return;
	//the party light eases in while its carrier is being drawn, and out when it isn't (hidden by a script, off screen)
	const ULONGLONG now = GetTickCount64();
	const double seconds = presenceTick != 0 ? std::min(0.25, (now - presenceTick) / 1000.0) : 0.0;
	presenceTick = now;
	const bool seen = leaderKnown && frame - leaderFrame < LEADER_FRAMES;
	const double before = presence;
	presence = std::clamp(presence + (seen ? seconds : -seconds) / PRESENCE_SECONDS, 0.0, 1.0);
	if (presence != before)
		serial++;
	frame++;
}
