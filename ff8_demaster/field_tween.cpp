#include "field_tween.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <utility>

namespace fieldtween
{
	namespace
	{
		//alpha-beta filter, critically damped: beta = 2 - alpha - 2 sqrt(1 - alpha). The smaller alpha, the smoother a slow
		//scroll and the longer the filter takes to catch a change of speed (0.2: about a third of a second)
		constexpr double ALPHA = 0.2;
		const double BETA = 2.0 - ALPHA - 2.0 * std::sqrt(1.0 - ALPHA);
		constexpr int REST_FRAMES = 20;
		constexpr double CATCH_UP = 0.25;
	}

	double Axis::Add(const double whole, const bool smooth)
	{
		if (!started || !std::isfinite(whole) || std::fabs(whole - last) > JUMP)
		{
			jumped = true;
			started = std::isfinite(whole);
			place = last = started ? whole : 0.0;
			speed = 0;
			still = 0;
			return place;
		}
		jumped = false;
		const double guess = place + speed, off = whole - guess;
		place = guess + ALPHA * off;
		speed += BETA * off;
		//never far from the game's own place. Being held back means the scroll changed speed and the filter has not caught
		//up: its speed takes a bigger share of that than of the rounding it normally sees
		const double held = std::clamp(place, whole - LIMIT, whole + LIMIT);
		speed += CATCH_UP * (held - place);
		place = held;
		//at rest: the game's place exactly
		still = whole == last ? still + 1 : 0;
		if (still >= REST_FRAMES && std::fabs(place - whole) < 0.05 && std::fabs(speed) < 0.02)
		{
			place = whole;
			speed = 0;
		}
		last = whole;
		return smooth ? place : whole;
	}

	void Advance(LayerTrack& track, const double wholeX, const double wholeY, const uint32_t frame, const bool smooth)
	{
		const bool follows = track.frame + 1 == frame;
		if (!follows)
		{
			track.x.Reset();
			track.y.Reset();
		}
		track.before[0] = track.exact[0];
		track.before[1] = track.exact[1];
		track.whole[0] = wholeX;
		track.whole[1] = wholeY;
		track.exact[0] = track.x.Add(wholeX, smooth);
		track.exact[1] = track.y.Add(wholeY, smooth);
		track.tween = follows && !track.x.Jumped() && !track.y.Jumped();
		track.frame = frame;
		track.triangles = 0;
	}

	void ShiftNow(const LayerTrack& track, double out[2])
	{
		out[0] = track.exact[0] - track.whole[0];
		out[1] = track.exact[1] - track.whole[1];
	}

	void ShiftBetween(const LayerTrack& track, double out[2])
	{
		if (!track.tween)
		{
			ShiftNow(track, out);
			return;
		}
		out[0] = (track.before[0] + track.exact[0]) * 0.5 - track.whole[0];
		out[1] = (track.before[1] + track.exact[1]) * 0.5 - track.whole[1];
	}

	double KeepCovering(const double lo, const double hi, double shift, const bool vertical)
	{
		//both the whole picture and the part the game shows (it cuts 8 PlayStation pixels off the top and the bottom)
		const double low[2] = { 0.0, 16.0 }, high[2] = { vertical ? 480.0 : 640.0, 464.0 };
		for (int i = 0; i < (vertical ? 2 : 1); i++)
		{
			if (lo <= low[i] + 1e-6)
				shift = std::min(shift, low[i] - lo);
			if (hi >= high[i] - 1e-6)
				shift = std::max(shift, high[i] - hi);
		}
		//a layer that is exactly the picture's size asks for both: it stays
		for (int i = 0; i < (vertical ? 2 : 1); i++)
			if (lo <= low[i] + 1e-6 && lo + shift > low[i] + 1e-6)
				shift = low[i] - lo;
		return shift;
	}

	double Snap(const double shift, const double pixelsPerUnit)
	{
		if (!(pixelsPerUnit > 0) || !std::isfinite(shift))
			return 0.0;
		return std::round(shift * pixelsPerUnit) / pixelsPerUnit;
	}

	void ShiftProjection(const float in[16], const double dx, const double dy, float out[16])
	{
		memmove(out, in, sizeof(float) * 16);
		for (int row = 0; row < 4; row++)
			out[12 + row] = static_cast<float>(in[12 + row] + in[row] * dx + in[4 + row] * dy);
	}

	void PixelsPerUnit(const float projection[16], const int viewportWidth, const int viewportHeight, double out[2])
	{
		out[0] = std::fabs(projection[0]) * viewportWidth * 0.5;
		out[1] = std::fabs(projection[5]) * viewportHeight * 0.5;
	}

	void PageCells::Size(const int logicalW, const int logicalH)
	{
		logicalWidth = std::max(TILE, logicalW);
		logicalHeight = std::max(TILE, logicalH);
		cellsX = static_cast<int>(logicalWidth) / TILE;
		cellsY = static_cast<int>(logicalHeight) / TILE;
		cells.assign(static_cast<size_t>(cellsX) * cellsY, Cell());
	}

	void PageCells::Add(const int cellX, const int cellY, const int layer, const int layerX, const int layerY)
	{
		const int cx = cellX / TILE, cy = cellY / TILE;
		if (cellX < 0 || cellY < 0 || cx >= cellsX || cy >= cellsY)
			return;
		Cell& cell = cells[static_cast<size_t>(cy) * cellsX + cx];
		if (cell.known && (cell.layer != layer || cell.layerX != layerX || cell.layerY != layerY))
			cell.mixed = true;
		cell.known = true;
		cell.layer = layer;
		cell.layerX = layerX;
		cell.layerY = layerY;
	}

	DrawPlace PlaceOfDraw(const PageCells& page, const void* positions, const size_t positionStride, const void* uvs,
		const size_t uvStride, const size_t vertexCount, const uint32_t* indices, const size_t count)
	{
		DrawPlace place;
		if (page.cells.empty() || positions == nullptr || uvs == nullptr || indices == nullptr || count < 3)
			return place;
		const uint8_t* p = static_cast<const uint8_t*>(positions);
		const uint8_t* t = static_cast<const uint8_t*>(uvs);
		double sumX = 0, sumY = 0, firstX = 0, firstY = 0;
		for (size_t i = 0; i + 2 < count; i += 3)
		{
			float position[3][2], uv[3][2];
			for (int k = 0; k < 3; k++)
			{
				if (indices[i + k] >= vertexCount)
					return DrawPlace();
				memcpy(position[k], p + indices[i + k] * positionStride, 8);
				memcpy(uv[k], t + indices[i + k] * uvStride, 8);
			}
			//the cell: from the triangle's middle (its corners lie on cell edges)
			const double u = (uv[0][0] + uv[1][0] + uv[2][0]) / 3.0 * page.logicalWidth;
			const double v = (uv[0][1] + uv[1][1] + uv[2][1]) / 3.0 * page.logicalHeight;
			const int cx = static_cast<int>(std::floor(u / TILE)), cy = static_cast<int>(std::floor(v / TILE));
			if (cx < 0 || cy < 0 || cx >= page.cellsX || cy >= page.cellsY)
				return DrawPlace();
			const Cell& cell = page.cells[static_cast<size_t>(cy) * page.cellsX + cx];
			if (!cell.known || cell.mixed)
				return DrawPlace();
			if (place.triangles > 0 && cell.layer != place.layer)
				return DrawPlace();
			place.layer = cell.layer;
			for (int k = 0; k < 3; k++)
			{
				const double x = position[k][0] - PIXEL * (cell.layerX + (uv[k][0] * page.logicalWidth - cx * TILE));
				const double y = position[k][1] - PIXEL * (cell.layerY + (uv[k][1] * page.logicalHeight - cy * TILE));
				if (!std::isfinite(x) || !std::isfinite(y))
					return DrawPlace();
				if (place.triangles == 0 && k == 0)
				{
					firstX = x;
					firstY = y;
				}
				else if (std::fabs(x - firstX) > 0.5 || std::fabs(y - firstY) > 0.5)
					return DrawPlace(); //tiles that don't agree: not one layer moving as one
				sumX += x;
				sumY += y;
			}
			place.triangles++;
		}
		if (place.triangles == 0)
			return DrawPlace();
		place.x = sumX / (place.triangles * 3.0);
		place.y = sumY / (place.triangles * 3.0);
		place.known = true;
		return place;
	}

	size_t TweenCharacters(const CharFrame& before, const CharFrame& now, std::vector<float>& out)
	{
		out = now.shown;
		if (before.frame + 1 != now.frame || before.draws.empty() || now.draws.empty())
			return 0;
		const auto inside = [](const CharFrame& f, const CharDrawRef& d)
		{
			return static_cast<size_t>(d.first) + d.count <= f.shown.size() / 4;
		};
		//everything with one texture: its middle, in both frames
		struct Middle
		{
			double x = 0, y = 0, n = 0;
		};
		std::map<uint32_t, Middle> middleBefore, middleNow;
		const auto middles = [&inside](const CharFrame& f, std::map<uint32_t, Middle>& into)
		{
			for (const CharDrawRef& d : f.draws)
			{
				if (!inside(f, d))
					continue;
				Middle& m = into[d.texture];
				for (uint32_t v = d.first; v < d.first + d.count; v++)
				{
					m.x += f.shown[static_cast<size_t>(v) * 4];
					m.y += f.shown[static_cast<size_t>(v) * 4 + 1];
					m.n += 1;
				}
			}
		};
		middles(before, middleBefore);
		middles(now, middleNow);
		//the n-th draw with a texture and a vertex count, in both frames
		std::map<std::pair<uint32_t, uint32_t>, std::vector<const CharDrawRef*>> same;
		for (const CharDrawRef& d : before.draws)
			if (inside(before, d))
				same[{ d.texture, d.count }].push_back(&d);
		std::map<std::pair<uint32_t, uint32_t>, size_t> seen;
		size_t tweened = 0;
		for (const CharDrawRef& d : now.draws)
		{
			if (!inside(now, d) || d.count == 0)
				continue;
			const size_t nth = seen[{ d.texture, d.count }]++;
			const auto candidates = same.find({ d.texture, d.count });
			bool done = false;
			if (candidates != same.end() && nth < candidates->second.size())
			{
				const CharDrawRef& b = *candidates->second[nth];
				double furthest = 0;
				for (uint32_t v = 0; v < d.count && furthest <= CHAR_LIMIT; v++)
				{
					const float* p0 = &before.shown[static_cast<size_t>(b.first + v) * 4];
					const float* p1 = &now.shown[static_cast<size_t>(d.first + v) * 4];
					furthest = std::max(furthest, std::max<double>(std::fabs(p1[0] - p0[0]), std::fabs(p1[1] - p0[1])));
					if (!(p0[3] > 0) || !(p1[3] > 0))
						furthest = CHAR_LIMIT * 2;
				}
				if (furthest <= CHAR_LIMIT)
				{
					for (uint32_t v = 0; v < d.count; v++)
					{
						const float* p0 = &before.shown[static_cast<size_t>(b.first + v) * 4];
						float* o = &out[static_cast<size_t>(d.first + v) * 4];
						for (int c = 0; c < 4; c++)
							o[c] = (p0[c] + o[c]) * 0.5f;
					}
					done = true;
				}
			}
			if (!done)
			{
				const auto m0 = middleBefore.find(d.texture), m1 = middleNow.find(d.texture);
				if (m0 == middleBefore.end() || m1 == middleNow.end() || m0->second.n <= 0 || m1->second.n <= 0)
					continue;
				const double dx = m1->second.x / m1->second.n - m0->second.x / m0->second.n;
				const double dy = m1->second.y / m1->second.n - m0->second.y / m0->second.n;
				if (std::fabs(dx) > CHAR_LIMIT || std::fabs(dy) > CHAR_LIMIT)
					continue;
				for (uint32_t v = 0; v < d.count; v++)
				{
					float* o = &out[static_cast<size_t>(d.first + v) * 4];
					o[0] -= static_cast<float>(dx * 0.5);
					o[1] -= static_cast<float>(dy * 0.5);
				}
			}
			tweened++;
		}
		return tweened;
	}

	void Pacer::Frame(const double seconds, const bool doubled)
	{
		if (!(seconds > 0.001) || seconds > 0.25)
			return; //a pause, a load: says nothing about the rate
		if (doubled)
		{
			intervalDoubled = doubledFrames == 0 ? seconds : intervalDoubled + (seconds - intervalDoubled) * 0.1;
			doubledFrames++;
			if (plainFrames >= 8 && doubledFrames >= 8 && intervalDoubled > interval * 1.12)
				slowFrames++;
			else
				slowFrames = 0;
		}
		else
		{
			interval = plainFrames == 0 ? seconds : interval + (seconds - interval) * 0.1;
			plainFrames++;
		}
	}

	bool Pacer::HalfRate() const
	{
		return plainFrames >= 8 && interval >= 1.0 / 40.0 && interval <= 1.0 / 25.0;
	}
}
