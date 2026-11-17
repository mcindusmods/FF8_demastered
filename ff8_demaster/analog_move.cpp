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
}
