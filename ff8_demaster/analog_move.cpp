#include "analog_move.h"

#include <algorithm>
#include <cmath>

namespace analogmove
{
	Stick FromPad(const double padX, const double padY, double deadZone)
	{
		Stick s;
		if (!std::isfinite(padX) || !std::isfinite(padY))
			return s;
		deadZone = std::clamp(deadZone, 0.05, 0.9);
		const double length = std::sqrt(padX * padX + padY * padY);
		if (length <= deadZone)
			return s;
		const double push = SMALLEST + (1.0 - SMALLEST) * std::min(1.0, (length - deadZone) / (1.0 - deadZone));
		s.x = std::clamp(static_cast<int>(std::lround(128.0 + 127.0 * push * padX / length)), 0, 255);
		s.y = std::clamp(static_cast<int>(std::lround(128.0 - 127.0 * push * padY / length)), 0, 255);
		s.active = s.x != 128 || s.y != 128;
		return s;
	}

	Stick FromDpad(const unsigned buttons)
	{
		const int dx = ((buttons & 0x2000u) ? 1 : 0) - ((buttons & 0x8000u) ? 1 : 0);
		const int dy = ((buttons & 0x4000u) ? 1 : 0) - ((buttons & 0x1000u) ? 1 : 0);
		Stick s;
		if (dx == 0 && dy == 0)
			return s;
		const double length = std::sqrt(static_cast<double>(dx * dx + dy * dy));
		s.x = static_cast<int>(std::lround(128.0 + 127.0 * dx / length));
		s.y = static_cast<int>(std::lround(128.0 + 127.0 * dy / length));
		s.active = true;
		return s;
	}

	Stick Swing::Turn(const Stick& wanted, const double speed, const double now)
	{
		if (!wanted.active)
			return wanted; //at rest: `last` stays, so REST is counted from the last push
		const double dx = wanted.x - 128.0, dy = wanted.y - 128.0;
		const double target = std::atan2(dy, dx);
		const double elapsed = now - last;
		if (!has || !(speed > 0.0) || !(elapsed >= 0.0) || elapsed > REST)
		{
			has = true;
			angle = target;
			last = now;
			return wanted;
		}
		last = now;
		const double PI = 3.14159265358979323846;
		const double difference = std::remainder(target - angle, 2.0 * PI); //-pi..pi: the short way round
		const double step = speed * PI / 180.0 * std::min(elapsed, 0.1);
		if (std::fabs(difference) <= step)
		{
			angle = target;
			return wanted;
		}
		angle = std::remainder(angle + (difference > 0.0 ? step : -step), 2.0 * PI);
		const double push = std::sqrt(dx * dx + dy * dy);
		Stick s;
		s.x = std::clamp(static_cast<int>(std::lround(128.0 + push * std::cos(angle))), 0, 255);
		s.y = std::clamp(static_cast<int>(std::lround(128.0 + push * std::sin(angle))), 0, 255);
		s.active = s.x != 128 || s.y != 128;
		return s;
	}
}
