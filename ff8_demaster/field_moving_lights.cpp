#include "field_moving_lights.h"

#include <algorithm>
#include <cmath>

namespace fieldpbr
{
	namespace
	{
		constexpr double PI = 3.14159265358979323846;

		double Smoothstep(const double lo, const double hi, const double x)
		{
			const double t = std::clamp((x - lo) / (hi - lo), 0.0, 1.0);
			return t * t * (3.0 - 2.0 * t);
		}
	}

	SkyTime SkyTimeOf(const DayNightCycle& cycle, const double clock)
	{
		const DayNightCycle c = cycle.Checked();
		double t = std::fmod(std::isfinite(clock) ? clock : 0.0, c.Length());
		if (t < 0)
			t += c.Length();
		SkyTime sky;
		sky.night = t >= c.day;
		sky.phase = sky.night ? (t - c.day) / c.night : t / c.day;
		return sky;
	}

	Vec3 SunDirection(const Vec3& up, const double azimuthDegrees, const double heightDegrees, const double phase)
	{
		Vec3 u = Normalize(up);
		if (Length(u) < 0.5)
			u = { 0, -1, 0 };
		//the ground's axes as the camera sees them: east along the picture's right, north away from the camera
		Vec3 east = Vec3{ 1, 0, 0 } - u * u.x;
		if (Length(east) < 1e-6)
			east = Vec3{ 0, 0, 1 } - u * u.z; //looking along the ground's east: any level direction does
		east = Normalize(east);
		Vec3 north = Cross(east, u);
		if (north.z < 0)
			north = north * -1.0;
		const double a = azimuthDegrees * PI / 180.0;
		const Vec3 rise = east * std::cos(a) + north * std::sin(a);
		//at noon it leans to the side of its path that is nearer the camera
		Vec3 lean = Cross(u, rise);
		if (lean.z > 0)
			lean = lean * -1.0;
		const double h = std::clamp(heightDegrees, 5.0, 90.0) * PI / 180.0;
		const double angle = std::clamp(phase, 0.0, 1.0) * PI;
		return Normalize(rise * std::cos(angle) + (u * std::sin(h) + lean * std::cos(h)) * std::sin(angle));
	}

	double HorizonFade(const double sineOfElevation)
	{
		return Smoothstep(0.0, 0.17, sineOfElevation);
	}

	void SunColor(const double sineOfElevation, const bool moon, float color[3])
	{
		if (moon)
		{
			color[0] = 0.55f;
			color[1] = 0.68f;
			color[2] = 1.0f;
			return;
		}
		const double high = Smoothstep(0.0, 0.6, sineOfElevation);
		const double low[3] = { 1.0, 0.5, 0.2 }, noon[3] = { 1.0, 0.96, 0.9 };
		for (int c = 0; c < 3; c++)
			color[c] = static_cast<float>(low[c] + (noon[c] - low[c]) * high);
	}

	void FieldExtent(const Settings& settings, const int canvasWidth, const int canvasHeight, Vec3& centre, double& extent)
	{
		const Space& space = settings.space;
		const double middle = settings.probeFar > settings.probeNear && settings.probeNear > 0 ? 0.5 * (settings.probeNear + settings.probeFar)
			: space.Distance(0.5);
		const int width = std::max(1, canvasWidth), height = std::max(1, canvasHeight);
		centre = space.ViewPosition(width * 0.5, height * 0.5, space.Value(middle));
		extent = std::max(1e-6, space.ViewLength(std::hypot(static_cast<double>(width), static_cast<double>(height)), middle));
	}

	MovingLight SunLight(const Sun& sun, const Settings& settings, const FieldTime time, const SkyTime& clockSky, const double nightAmount,
		const Vec3& centre, const double extent, const double strength)
	{
		MovingLight light;
		if (sun.on == 0 || (sun.on < 0 && !settings.sky) || !(strength > 0) || !(extent > 0))
			return light;
		//a field with its own time: the sun stands still
		SkyTime sky = clockSky;
		double share = sky.night ? nightAmount : 1.0 - nightAmount; //the sun fades out as the night comes on, the moon in
		if (time == FieldTime::Day)
		{
			sky.night = false;
			sky.phase = sun.fixed;
			share = 1.0;
		}
		else if (time == FieldTime::Night)
		{
			sky.night = true;
			sky.phase = 0.5;
			share = 1.0;
		}
		const double moon = sky.night ? sun.moon : 1.0;
		const Vec3 direction = SunDirection(settings.up, sun.azimuth, sun.height, sky.phase);
		const double elevation = Dot(direction, Normalize(settings.up));
		const double amount = HorizonFade(elevation) * std::clamp(share, 0.0, 1.0) * moon * strength;
		if (!(amount > 0.001))
			return light;
		//4 field widths away: far enough that its rays are close to parallel across the field, near enough for the characters'
		//shadow maps (their depth bias grows with the distance). Its range is so long that the falloff is flat: the shaders'
		//falloff is (1 - x^4)^2 / (1 + 8 x^2) at x = distance / range, which the colour is divided by
		const double distance = extent * 4.0, range = distance * 8.0;
		const double x = distance / range, window = 1.0 - x * x * x * x;
		const double falloff = window * window / (1.0 + 8.0 * x * x);
		light.on = true;
		light.position = centre + direction * distance;
		light.range = range;
		float color[3];
		SunColor(elevation, sky.night, color);
		for (int c = 0; c < 3; c++)
		{
			light.color[c] = static_cast<float>(color[c] * sun.intensity * amount / falloff);
			light.gloss[c] = static_cast<float>(color[c] * amount / falloff);
		}
		light.shade = static_cast<float>(sun.shade * amount / falloff);
		light.shadow = static_cast<float>(0.5 * std::min(1.0, amount));
		light.castsShadow = true;
		return light;
	}

	int ChooseLeader(const std::vector<LeaderCandidate>& candidates, const int follow, const int pcOfMost, const double now, LeaderTrack& track,
		const int controlled)
	{
		//who moved since the last frame (more than a hair: standing animations sway)
		std::vector<LeaderTrack::Seen> seen;
		seen.reserve(candidates.size());
		for (const LeaderCandidate& c : candidates)
		{
			LeaderTrack::Seen s;
			s.key = c.key;
			s.center = c.center;
			for (const LeaderTrack::Seen& before : track.seen)
				if (before.key == c.key)
				{
					s.movedAt = before.movedAt;
					if (Length(c.center - before.center) > std::max(1e-6, c.halfHeight * 0.02))
						s.movedAt = now;
					break;
				}
			seen.push_back(s);
		}
		track.seen = seen;
		if (candidates.empty())
			return -1;
		if (follow >= 0)
		{
			for (size_t i = 0; i < candidates.size(); i++)
				if (candidates[i].entity == follow)
				{
					track.current = candidates[i].key;
					track.has = true;
					return static_cast<int>(i);
				}
			return -1;
		}
		//the one the player moves (the followers move too, and after it: "who moved last" picked them)
		if (controlled >= 0)
			for (size_t i = 0; i < candidates.size(); i++)
				if (candidates[i].entity == controlled)
				{
					track.current = candidates[i].key;
					track.has = true;
					return static_cast<int>(i);
				}
		//the playable ones: their pc byte isn't what the field's other entities have
		std::vector<size_t> pool;
		for (size_t i = 0; i < candidates.size(); i++)
			if (pcOfMost >= 0 && candidates[i].pc >= 0 && candidates[i].pc != pcOfMost)
				pool.push_back(i);
		if (pool.empty())
			for (size_t i = 0; i < candidates.size(); i++)
				pool.push_back(i);
		//the one it follows stays, unless another moved after it did
		int best = -1;
		double bestMoved = -1e18;
		for (const size_t i : pool)
		{
			double moved = seen[i].movedAt;
			if (track.has && candidates[i].key == track.current)
				moved += 0.5; //half a second's lead: two walking side by side don't trade the light
			if (moved > bestMoved)
			{
				bestMoved = moved;
				best = static_cast<int>(i);
			}
		}
		if (best >= 0)
		{
			track.current = candidates[static_cast<size_t>(best)].key;
			track.has = true;
		}
		return best;
	}

	MovingLight PartyLightFor(const PartyLight& party, const LeaderCandidate& leader, const Space& space, const int mode, const double nightAmount,
		const double strength)
	{
		MovingLight light;
		const int on = party.on >= 0 ? party.on : mode;
		const double amount = (on >= 2 ? 1.0 : on == 1 ? std::clamp(nightAmount, 0.0, 1.0) : 0.0) * strength;
		if (!(amount > 0.001) || !(party.intensity > 0) || !(leader.center.z > 0))
			return light;
		const Vec3 up = Normalize(leader.up);
		//above the middle, and towards the camera by a third of the body's height
		const Vec3 toCamera = Normalize(leader.center * -1.0);
		light.position = leader.center + up * (leader.halfHeight * party.height) + toCamera * (leader.halfHeight * 0.66);
		if (light.position.z < space.nearest)
			light.position.z = space.nearest;
		light.range = std::max(1e-6, space.ViewLength(party.range, light.position.z));
		light.on = true;
		for (int c = 0; c < 3; c++)
			light.color[c] = static_cast<float>(party.color[c] * party.intensity * amount);
		light.specular = 1.0f;
		return light;
	}
}
