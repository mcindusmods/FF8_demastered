#include "field_chars.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

namespace fieldchars
{
	float Length(const V3& a)
	{
		return std::sqrt(Dot(a, a));
	}

	V3 Normalize(const V3& a, const V3& fallback)
	{
		const float length = Length(a);
		return length > 1e-20f ? a * (1.0f / length) : fallback;
	}

	V3 Projection::ToView(const double x, const double y, const double rhw) const
	{
		const double depth = rhw > 0 ? 1.0 / rhw : 0.0;
		const double z = depth / gameDepth;
		const double canvasX = (x - offsetX) / screenPerCanvas, canvasY = (y - offsetY) / screenPerCanvas;
		return { static_cast<float>((canvasX - space.originX) * z / space.zoom), static_cast<float>((canvasY - space.originY) * z / space.zoom),
			static_cast<float>(z) };
	}

	bool Projection::ToScreen(const V3& view, double& x, double& y) const
	{
		if (view.z <= 1e-9f)
			return false;
		const double canvasX = space.originX + view.x * space.zoom / view.z, canvasY = space.originY + view.y * space.zoom / view.z;
		x = offsetX + canvasX * screenPerCanvas;
		y = offsetY + canvasY * screenPerCanvas;
		return true;
	}

	void SmoothNormals(const std::vector<V3>& positions, const std::vector<uint64_t>& keys, const float creaseDegrees, std::vector<V3>& normals)
	{
		const size_t faces = positions.size() / 3;
		normals.assign(faces * 3, V3{ 0, 0, -1 });
		if (faces == 0 || keys.size() < faces * 3)
			return;
		std::vector<V3> weighted(faces), unit(faces);
		std::vector<bool> flat(faces);
		for (size_t f = 0; f < faces; f++)
		{
			const V3& a = positions[f * 3];
			weighted[f] = Cross(positions[f * 3 + 1] - a, positions[f * 3 + 2] - a); //length: twice the area
			const float length = Length(weighted[f]);
			flat[f] = !(length > 1e-14f);
			unit[f] = flat[f] ? V3{ 0, 0, 0 } : weighted[f] * (1.0f / length);
		}
		//corners grouped by key
		std::vector<uint32_t> order(faces * 3);
		std::iota(order.begin(), order.end(), 0u);
		std::sort(order.begin(), order.end(), [&](const uint32_t a, const uint32_t b) { return keys[a] < keys[b]; });
		const float threshold = std::cos(creaseDegrees * 3.14159265f / 180.0f);
		size_t begin = 0;
		while (begin < order.size())
		{
			size_t end = begin + 1;
			while (end < order.size() && keys[order[end]] == keys[order[begin]])
				end++;
			for (size_t i = begin; i < end; i++)
			{
				const uint32_t corner = order[i];
				const uint32_t face = corner / 3;
				V3 sum{ 0, 0, 0 };
				for (size_t j = begin; j < end; j++)
				{
					const uint32_t other = order[j] / 3;
					if (flat[other])
						continue;
					if (flat[face] || Dot(unit[face], unit[other]) >= threshold)
						sum = sum + weighted[other];
				}
				normals[corner] = Normalize(sum, flat[face] ? V3{ 0, 0, -1 } : unit[face]);
			}
			begin = end;
		}
	}

	std::vector<Disc> ReadDiscs(const std::vector<GameVertex>& triangles)
	{
		struct Gather
		{
			Disc disc;
			bool any = false;
		};
		std::map<std::pair<float, float>, Gather> byCentre;
		for (size_t t = 0; t + 2 < triangles.size(); t += 3)
		{
			//the centre: the brightest corner
			size_t centre = t;
			int best = -1;
			for (size_t k = t; k < t + 3; k++)
			{
				const GameVertex& v = triangles[k];
				const int value = std::max({ v.color[0], v.color[1], v.color[2] });
				if (value > best)
				{
					best = value;
					centre = k;
				}
			}
			if (best <= 0)
				continue;
			const GameVertex& c = triangles[centre];
			Gather& g = byCentre[{ c.x, c.y }];
			g.any = true;
			g.disc.x = c.x;
			g.disc.y = c.y;
			g.disc.rhw = c.rhw;
			g.disc.darkness = std::max(g.disc.darkness, best / 255.0f);
			for (size_t k = t; k < t + 3; k++)
				if (k != centre)
					g.disc.radius = std::max(g.disc.radius, std::hypot(static_cast<double>(triangles[k].x - c.x), static_cast<double>(triangles[k].y - c.y)));
		}
		std::vector<Disc> discs;
		for (const auto& [where, g] : byCentre)
			if (g.any && g.disc.radius > 0)
				discs.push_back(g.disc);
		return discs;
	}

	namespace
	{
		//level (along the ground) distance
		float Level(const V3& a, const V3& b, const V3& up)
		{
			const V3 d = a - b;
			const V3 flat = d - up * Dot(d, up);
			return Length(flat);
		}

		void Shape(Character& c, const std::vector<V3>& positions, const V3& up)
		{
			V3 sum{ 0, 0, 0 };
			size_t n = 0;
			for (const uint32_t t : c.triangles)
				for (int k = 0; k < 3; k++)
				{
					sum = sum + positions[t * 3 + k];
					n++;
				}
			const V3 mean = n ? sum * (1.0f / static_cast<float>(n)) : V3{ 0, 0, 1 };
			c.up = Normalize(up, { 0, -1, 0 });
			c.forward = Normalize(mean - c.up * Dot(mean, c.up), { 0, 0, 1 });
			c.right = Normalize(Cross(c.up, c.forward), { 1, 0, 0 });
			if (c.right.x < 0)
				c.right = c.right * -1.0f; //right as the camera sees it
			float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
			const V3 axes[3] = { c.right, c.up, c.forward };
			for (const uint32_t t : c.triangles)
				for (int k = 0; k < 3; k++)
				{
					const V3& p = positions[t * 3 + k];
					for (int a = 0; a < 3; a++)
					{
						const float d = Dot(p, axes[a]);
						lo[a] = std::min(lo[a], d);
						hi[a] = std::max(hi[a], d);
					}
				}
			c.center = V3{ 0, 0, 0 };
			for (int a = 0; a < 3; a++)
			{
				c.center = c.center + axes[a] * ((lo[a] + hi[a]) * 0.5f);
				(&c.halfSize.x)[a] = std::max((hi[a] - lo[a]) * 0.5f, 1e-6f);
			}
			c.feet = c.center - c.up * c.halfSize.y;
			//the head: the top 14% of the height
			const float top = hi[1], cut = hi[1] - (hi[1] - lo[1]) * 0.14f;
			V3 headSum{ 0, 0, 0 };
			size_t headCount = 0;
			for (const uint32_t t : c.triangles)
				for (int k = 0; k < 3; k++)
				{
					const V3& p = positions[t * 3 + k];
					if (Dot(p, c.up) >= cut)
					{
						headSum = headSum + p;
						headCount++;
					}
				}
			c.head = headCount ? headSum * (1.0f / static_cast<float>(headCount)) : c.center + c.up * c.halfSize.y * 0.8f;
			c.headSize = std::max((top - cut) * 1.0f, 1e-6f);
		}
	}

	std::vector<Character> Group(const std::vector<V3>& positions, const std::vector<unsigned>& textures, const std::vector<Anchor>& anchors,
		const V3& up)
	{
		std::vector<Character> out;
		const size_t faces = std::min(positions.size() / 3, textures.size());
		std::map<unsigned, std::vector<uint32_t>> byTexture;
		for (size_t f = 0; f < faces; f++)
			byTexture[textures[f]].push_back(static_cast<uint32_t>(f));
		const V3 u = Normalize(up, { 0, -1, 0 });
		for (auto& [texture, list] : byTexture)
		{
			//which anchors stand within this texture's triangles (level box, with some room)
			Character whole;
			whole.texture = texture;
			whole.triangles = list;
			Shape(whole, positions, u);
			std::vector<int> inside;
			for (size_t a = 0; a < anchors.size(); a++)
			{
				const V3 d = anchors[a].position - whole.center;
				const float x = std::fabs(Dot(d, whole.right)), z = std::fabs(Dot(d, whole.forward));
				if (x <= whole.halfSize.x * 1.3f + 1e-4f && z <= whole.halfSize.z * 1.3f + whole.halfSize.x * 0.5f + 1e-4f)
					inside.push_back(static_cast<int>(a));
			}
			if (inside.size() < 2)
			{
				if (!inside.empty())
					whole.anchor = inside[0];
				out.push_back(std::move(whole));
				continue;
			}
			std::vector<Character> split(inside.size());
			for (const uint32_t f : list)
			{
				const V3 centroid = (positions[f * 3] + positions[f * 3 + 1] + positions[f * 3 + 2]) * (1.0f / 3.0f);
				size_t best = 0;
				float bestDistance = 1e30f;
				for (size_t i = 0; i < inside.size(); i++)
				{
					const float d = Level(centroid, anchors[inside[i]].position, u);
					if (d < bestDistance)
					{
						bestDistance = d;
						best = i;
					}
				}
				split[best].triangles.push_back(f);
			}
			for (size_t i = 0; i < split.size(); i++)
			{
				if (split[i].triangles.empty())
					continue;
				split[i].texture = texture;
				split[i].anchor = inside[i];
				Shape(split[i], positions, u);
				out.push_back(std::move(split[i]));
			}
		}
		//what the anchors say about each
		for (Character& c : out)
		{
			if (c.anchor < 0)
			{
				//the nearest anchor under it
				float best = 1e30f;
				for (size_t a = 0; a < anchors.size(); a++)
				{
					const float d = Level(anchors[a].position, c.feet, c.up);
					if (d < best && d <= std::max(c.halfSize.x, c.halfSize.z) * 1.5f)
					{
						best = d;
						c.anchor = static_cast<int>(a);
					}
				}
			}
			if (c.anchor >= 0)
			{
				const Anchor& a = anchors[c.anchor];
				if (a.darkness >= 0)
				{
					c.darkness = a.darkness;
					c.shadowRadius = a.radius;
				}
				c.entity = a.entity;
			}
		}
		return out;
	}

	void FacingCalibration::Add(const int facing, const double dx, const double dy)
	{
		if (std::hypot(dx, dy) <= 1e-9)
			return;
		samples.emplace_back(facing & 0xFF, std::atan2(dy, dx));
		if (samples.size() > 96)
			samples.erase(samples.begin(), samples.begin() + 32);
		if (samples.size() >= 24 && samples.size() % 8 == 0)
			Solve();
	}

	void FacingCalibration::Solve()
	{
		constexpr double unit = 2.0 * 3.14159265358979 / 256.0;
		double bestLength = 0;
		for (const int s : { 1, -1 })
		{
			double cx = 0, cy = 0;
			for (const auto& [facing, angle] : samples)
			{
				const double b = angle - s * facing * unit;
				cx += std::cos(b);
				cy += std::sin(b);
			}
			const double length = std::hypot(cx, cy) / static_cast<double>(samples.size());
			if (length > bestLength)
			{
				bestLength = length;
				sense = s;
				base = std::atan2(cy, cx);
			}
		}
		known = bestLength > 0.85; //the walks agree
	}

	double FacingCalibration::Angle(const int facing) const
	{
		return base + sense * (facing & 0xFF) * (2.0 * 3.14159265358979 / 256.0);
	}
}
