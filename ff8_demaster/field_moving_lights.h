#pragma once
//The moving lights (FIELD_MOVING_LIGHTS in demaster.conf): lights that are not at a fixed place in a field's _lights.txt.
//  the sun          crosses the sky with the day/night clock (fields open to the sky); the moon at night
//  the party light  carried by the party leader
//This file is the maths, with nothing of Windows or OpenGL in it (the tests run it): where the sun is, which script entity
//is the leader, and each of them as a light the shaders can draw. The DLL's side (the clock, the game's memory, the
//uniforms) is texturepatch_v2_fieldMovingLights.cpp. Their per-field settings are the [sun] and [party] sections of
//<field>_lights.txt (field_pbr.h: Sun, PartyLight).
//
//To the shaders a moving light is one more light among the field's (at most MAX_LIGHTS at a time; the field's own give up
//places for them). The sun is a point far away along its direction, with a range so long its light is the same everywhere
//in the field: no shader code of its own, and the characters' shadow maps work for it as for any lamp.

#include "field_pbr.h"

#include <vector>

namespace fieldpbr
{
	//--- the sky

	//Where the clock is on the sun's or the moon's arc. The day runs from the middle of dawn's fade (0) to the middle of
	//dusk's (1), the night from there (0) to the next dawn (1).
	struct SkyTime
	{
		bool night = false;
		double phase = 0.5; //0 rising, 0.5 highest, 1 setting
	};
	SkyTime SkyTimeOf(const DayNightCycle& cycle, double clock);
	//Towards the sun (or moon) in view space (x right, y down, z away from the camera), unit length. up: the world's up
	//there (Settings::up). It rises at `azimuth` degrees around up from the camera's right, climbs to `height` degrees at
	//phase 0.5, leaning towards the camera, and sets opposite where it rose.
	Vec3 SunDirection(const Vec3& up, double azimuthDegrees, double heightDegrees, double phase);
	//its light's colour (linear) for how high it stands (the sine of its elevation): orange at the horizon, white high up;
	//the moon's is a cool blue
	void SunColor(double sineOfElevation, bool moon, float color[3]);
	//0 at the horizon and below, 1 from about 10 degrees up
	double HorizonFade(double sineOfElevation);

	//The field as a box in view space: its middle, and how wide it is (the canvas's diagonal at the middle distance)
	void FieldExtent(const Settings& settings, int canvasWidth, int canvasHeight, Vec3& centre, double& extent);

	//--- a moving light as the shaders get it (the same numbers as a light of the field: texturepatch_v2_fieldPbr.cpp)
	struct MovingLight
	{
		bool on = false;
		Vec3 position; //view space
		double range = 1; //view units
		float color[3] = { 0, 0, 0 }; //light added now (colour x intensity x how much of it is on)
		float specular = 1.0f;
		float gloss[3] = { 0, 0, 0 }; //its colour for the highlights dial (FIELD_PBR_HIGHLIGHTS)
		float shade = 0.0f; //its weight in the relight dial (0: it doesn't move the painted light)
		float shadow = 0.0f; //how much a character's shadow takes away of the painted light around it
		bool castsShadow = false; //the characters get shadow maps from it
	};
	//The sun, or at night the moon, for a field. time: the field's own (day: it stands at sun.fixed; night: the moon stands
	//highest) or the clock's. nightAmount: how night it is (0-1). centre, extent: the middle of the field in view space and
	//how big it is (view units); strength: FIELD_SUN. Off (on = false) when the settings say so or it is below the horizon.
	MovingLight SunLight(const Sun& sun, const Settings& settings, FieldTime time, const SkyTime& sky, double nightAmount, const Vec3& centre,
		double extent, double strength);

	//--- the party leader

	//A character DEMASTER drew this frame, as a candidate for carrying the party light
	struct LeaderCandidate
	{
		int key = 0; //the same character keeps it from frame to frame: its script entity, else its texture
		int entity = -1; //script entity (-1: not known: the field's camera isn't measured)
		int pc = -1; //the entity's "playable character" byte (set by the script's SETPC); -1: not known
		Vec3 center, up; //view space
		double halfHeight = 0;
	};
	//remembered between frames
	struct LeaderTrack
	{
		struct Seen
		{
			int key = 0;
			Vec3 center;
			double movedAt = -1e9; //seconds
		};
		std::vector<Seen> seen;
		int current = 0; //the key it follows
		bool has = false;
	};
	//Which candidate carries the light (its index; -1: none). follow >= 0: that script entity, if it is drawn. Else the
	//script entity the player moves (controlled, from the game; -1: not known), if it is drawn. Else, as before v15.12: among
	//the characters whose pc byte differs from what most entities of the field have (those the script made playable; all of
	//them when the byte tells nothing), the one that moved last; the one it followed before stays until another moves.
	//pcOfMost: the pc byte most script entities of the field have (-1: not known). now: seconds, any clock that runs on.
	int ChooseLeader(const std::vector<LeaderCandidate>& candidates, int follow, int pcOfMost, double now, LeaderTrack& track,
		int controlled = -1);
	//The light the leader carries. mode: 0 off, 1 at night (it comes on with nightAmount), 2 always. strength:
	//FIELD_PARTY_LIGHT_STRENGTH. It sits above the leader's middle and a little towards the camera, so the leader is lit by
	//it too instead of standing in it.
	MovingLight PartyLightFor(const PartyLight& party, const LeaderCandidate& leader, const Space& space, int mode, double nightAmount,
		double strength);
}
