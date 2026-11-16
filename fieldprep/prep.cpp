#include "prep.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <thread>

namespace fieldprep
{
	using fieldlayers::Layout;
	using fieldlayers::Tile;
	using fieldlayers::TILE_SIZE;
	using fieldpbr::Vec3;

	namespace
	{
		constexpr double PI = 3.14159265358979323846;

		template <typename F>
		void ParallelRows(const int height, int threads, F&& row)
		{
			threads = std::clamp(threads, 1, 64);
			std::vector<std::thread> pool;
			for (int t = 1; t < threads; t++)
				pool.emplace_back([&, t] { for (int y = t; y < height; y += threads) row(y); });
			for (int y = 0; y < height; y += threads)
				row(y);
			for (std::thread& t : pool)
				t.join();
		}

		double Percentile(std::vector<float> values, const double p)
		{
			if (values.empty())
				return 0.0;
			const size_t k = std::min(values.size() - 1, static_cast<size_t>(p * (values.size() - 1)));
			std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(k), values.end());
			return values[k];
		}

		void BoxPass(std::vector<float>& v, const int width, const int height, const int channels, const int radius, const bool horizontal)
		{
			const int length = horizontal ? width : height, lines = horizontal ? height : width;
			std::vector<float> line(static_cast<size_t>(length) * channels), out(line.size());
			for (int l = 0; l < lines; l++)
			{
				for (int i = 0; i < length; i++)
				{
					const size_t at = horizontal ? (static_cast<size_t>(l) * width + i) : (static_cast<size_t>(i) * width + l);
					for (int c = 0; c < channels; c++)
						line[static_cast<size_t>(i) * channels + c] = v[at * channels + c];
				}
				for (int c = 0; c < channels; c++)
				{
					double sum = 0;
					int count = 0;
					for (int i = -radius; i <= radius; i++)
						if (i >= 0 && i < length)
						{
							sum += line[static_cast<size_t>(i) * channels + c];
							count++;
						}
					for (int i = 0; i < length; i++)
					{
						out[static_cast<size_t>(i) * channels + c] = static_cast<float>(sum / std::max(count, 1));
						const int leave = i - radius, enter = i + radius + 1;
						if (leave >= 0)
						{
							sum -= line[static_cast<size_t>(leave) * channels + c];
							count--;
						}
						if (enter < length)
						{
							sum += line[static_cast<size_t>(enter) * channels + c];
							count++;
						}
					}
				}
				for (int i = 0; i < length; i++)
				{
					const size_t at = horizontal ? (static_cast<size_t>(l) * width + i) : (static_cast<size_t>(i) * width + l);
					for (int c = 0; c < channels; c++)
						v[at * channels + c] = out[static_cast<size_t>(i) * channels + c];
				}
			}
		}

		Vec3 NormalAt(const Image& normal, const size_t i)
		{
			//map: x right, y up, z towards the camera -> view: x right, y down, z away
			const uint8_t* p = normal.rgba.data() + i * 4;
			return fieldpbr::Normalize({ p[0] / 127.5 - 1.0, 1.0 - p[1] / 127.5, 1.0 - p[2] / 127.5 });
		}

		double SmoothStep(const double a, const double b, const double x)
		{
			const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
			return t * t * (3.0 - 2.0 * t);
		}
	}

	//--- basics

	Rgb SrgbToLinear(const Image& image)
	{
		Rgb out;
		out.width = image.width;
		out.height = image.height;
		const size_t n = static_cast<size_t>(image.width) * image.height;
		out.v.resize(n * 3);
		const float* table = fieldpbr::SrgbTable();
		for (size_t i = 0; i < n; i++)
			for (int c = 0; c < 3; c++)
				out.v[i * 3 + c] = table[image.rgba[i * 4 + c]];
		return out;
	}

	Rgb AsStored(const Image& image)
	{
		Rgb out;
		out.width = image.width;
		out.height = image.height;
		const size_t n = static_cast<size_t>(image.width) * image.height;
		out.v.resize(n * 3);
		for (size_t i = 0; i < n; i++)
			for (int c = 0; c < 3; c++)
				out.v[i * 3 + c] = image.rgba[i * 4 + c] / 255.0f;
		return out;
	}

	Image Resize(const Image& image, const int width, const int height)
	{
		if (image.width == width && image.height == height)
			return image;
		Image out;
		out.width = width;
		out.height = height;
		out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
		for (int y = 0; y < height; y++)
			for (int x = 0; x < width; x++)
			{
				const double fx = std::clamp((x + 0.5) * image.width / width - 0.5, 0.0, image.width - 1.0);
				const double fy = std::clamp((y + 0.5) * image.height / height - 0.5, 0.0, image.height - 1.0);
				const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
				const int x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
				const double ax = fx - x0, ay = fy - y0;
				const int px[4] = { x0, x1, x0, x1 }, py[4] = { y0, y0, y1, y1 };
				const double pw[4] = { (1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay };
				double sum[4] = { 0, 0, 0, 0 }, weight = 0;
				for (int k = 0; k < 4; k++)
				{
					const uint8_t* s = image.rgba.data() + (static_cast<size_t>(py[k]) * image.width + px[k]) * 4;
					const double a = s[3] / 255.0 * pw[k];
					for (int c = 0; c < 3; c++)
						sum[c] += s[c] * a;
					sum[3] += s[3] * pw[k];
					weight += a;
				}
				uint8_t* d = out.rgba.data() + (static_cast<size_t>(y) * width + x) * 4;
				for (int c = 0; c < 3; c++)
					d[c] = static_cast<uint8_t>(std::clamp(std::lround(weight > 0 ? sum[c] / weight : 0.0), 0L, 255L));
				d[3] = static_cast<uint8_t>(std::clamp(std::lround(sum[3]), 0L, 255L));
			}
		return out;
	}

	Gray Resize(const Gray& image, const int width, const int height)
	{
		if (image.width == width && image.height == height)
			return image;
		Gray out;
		out.width = width;
		out.height = height;
		out.sixteen = image.sixteen;
		out.v.resize(static_cast<size_t>(width) * height);
		for (int y = 0; y < height; y++)
			for (int x = 0; x < width; x++)
				out.v[static_cast<size_t>(y) * width + x] = image.Sample((x + 0.5) * image.width / width, (y + 0.5) * image.height / height);
		return out;
	}

	namespace
	{
		//copies the rows of `from` that `to` shares with it: channels values per pixel, in place in out
		template <typename T>
		void CopyFrames(const std::vector<T>& in, const int inWidth, std::vector<T>& out, const int outWidth, const int channels,
			const fieldlayers::Frame& from, const fieldlayers::Frame& to, const int scale)
		{
			const int left = std::max(from.left, to.left), right = std::min(from.left + from.width, to.left + to.width);
			const int top = std::max(from.top, to.top), bottom = std::min(from.top + from.height, to.top + to.height);
			if (right <= left || bottom <= top)
				return;
			const size_t row = static_cast<size_t>(right - left) * scale * channels;
			for (int y = (top - from.top) * scale, y2 = (top - to.top) * scale; y < (bottom - from.top) * scale; y++, y2++)
			{
				const size_t sx = static_cast<size_t>(left - from.left) * scale, dx = static_cast<size_t>(left - to.left) * scale;
				std::copy_n(in.begin() + static_cast<std::ptrdiff_t>((static_cast<size_t>(y) * inWidth + sx) * channels), row,
					out.begin() + static_cast<std::ptrdiff_t>((static_cast<size_t>(y2) * outWidth + dx) * channels));
			}
		}
	}

	Image Reframe(const Image& image, const fieldlayers::Frame& from, const fieldlayers::Frame& to, const int scale)
	{
		if (from == to || !image.IsValid())
			return image;
		Image out;
		out.width = to.width * scale;
		out.height = to.height * scale;
		out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
		if (image.width == from.width * scale && image.height == from.height * scale)
			CopyFrames(image.rgba, image.width, out.rgba, out.width, 4, from, to, scale);
		return out;
	}

	Gray Reframe(const Gray& image, const fieldlayers::Frame& from, const fieldlayers::Frame& to, const int scale)
	{
		if (from == to || !image.IsValid())
			return image;
		Gray out;
		out.width = to.width * scale;
		out.height = to.height * scale;
		out.sixteen = image.sixteen;
		out.v.assign(static_cast<size_t>(out.width) * out.height, 0.0f);
		if (image.width == from.width * scale && image.height == from.height * scale)
			CopyFrames(image.v, image.width, out.v, out.width, 1, from, to, scale);
		return out;
	}

	Gray Luminance(const Rgb& image)
	{
		Gray out;
		out.width = image.width;
		out.height = image.height;
		out.v.resize(static_cast<size_t>(image.width) * image.height);
		for (size_t i = 0; i < out.v.size(); i++)
			out.v[i] = Luma(image.v.data() + i * 3);
		return out;
	}

	void Blur(Gray& image, const int radius)
	{
		if (radius <= 0 || !image.IsValid())
			return;
		for (int pass = 0; pass < 3; pass++)
		{
			BoxPass(image.v, image.width, image.height, 1, radius, true);
			BoxPass(image.v, image.width, image.height, 1, radius, false);
		}
	}

	void Blur(Rgb& image, const int radius)
	{
		if (radius <= 0 || !image.IsValid())
			return;
		for (int pass = 0; pass < 3; pass++)
		{
			BoxPass(image.v, image.width, image.height, 3, radius, true);
			BoxPass(image.v, image.width, image.height, 3, radius, false);
		}
	}

	//--- layers

	namespace
	{
		//tile order as the game draws: bigger z first, then .map order; only each animation's first state
		std::vector<size_t> DrawOrder(const std::vector<Tile>& tiles)
		{
			std::map<int, int> firstState;
			for (const Tile& t : tiles)
				if (t.animId != 0xFF)
				{
					const auto it = firstState.find(t.animId);
					if (it == firstState.end() || t.animState < it->second)
						firstState[t.animId] = t.animState;
				}
			std::vector<size_t> order;
			for (size_t i = 0; i < tiles.size(); i++)
				if (tiles[i].animId == 0xFF || tiles[i].animState == firstState[tiles[i].animId])
					order.push_back(i);
			std::stable_sort(order.begin(), order.end(), [&](const size_t a, const size_t b) { return tiles[a].z > tiles[b].z; });
			return order;
		}
	}

	Image ComposeOpaque(const std::vector<Tile>& tiles, const Layout& layout, const std::map<uint32_t, const Image*>& layers, const int scale)
	{
		Image out;
		out.width = layout.width * scale;
		out.height = layout.height * scale;
		out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
		const int block = TILE_SIZE * scale;
		for (const size_t i : DrawOrder(tiles))
		{
			if (tiles[i].blend != 4 || i >= layout.pupu.size())
				continue;
			const auto it = layers.find(layout.pupu[i]);
			if (it == layers.end() || it->second == nullptr || it->second->width != out.width || it->second->height != out.height)
				continue;
			const Image& image = *it->second;
			const int x0 = layout.x[i] * scale, y0 = layout.y[i] * scale;
			for (int y = std::max(0, y0); y < std::min(out.height, y0 + block); y++)
				for (int x = std::max(0, x0); x < std::min(out.width, x0 + block); x++)
				{
					const size_t at = (static_cast<size_t>(y) * out.width + x) * 4;
					const uint8_t* s = image.rgba.data() + at;
					uint8_t* d = out.rgba.data() + at;
					const int a = s[3];
					if (a == 0)
						continue;
					if (a == 255)
					{
						memcpy(d, s, 4);
						continue;
					}
					for (int c = 0; c < 3; c++)
						d[c] = static_cast<uint8_t>((s[c] * a + d[c] * (255 - a) + 127) / 255);
					d[3] = static_cast<uint8_t>(a + (d[3] * (255 - a) + 127) / 255);
				}
		}
		return out;
	}

	Gray GlowLayers(const std::vector<Tile>& tiles, const Layout& layout, const std::map<uint32_t, const Image*>& layers, const int scale)
	{
		Gray out;
		out.width = layout.width * scale;
		out.height = layout.height * scale;
		out.v.assign(static_cast<size_t>(out.width) * out.height, 0.0f);
		const int block = TILE_SIZE * scale;
		const float* table = fieldpbr::SrgbTable();
		for (const size_t i : DrawOrder(tiles))
		{
			const int blend = tiles[i].blend;
			if ((blend != 1 && blend != 3) || i >= layout.pupu.size())
				continue;
			const auto it = layers.find(layout.pupu[i]);
			if (it == layers.end() || it->second == nullptr || it->second->width != out.width || it->second->height != out.height)
				continue;
			const Image& image = *it->second;
			const float weight = blend == 3 ? 0.25f : 1.0f;
			const int x0 = layout.x[i] * scale, y0 = layout.y[i] * scale;
			for (int y = std::max(0, y0); y < std::min(out.height, y0 + block); y++)
				for (int x = std::max(0, x0); x < std::min(out.width, x0 + block); x++)
				{
					const size_t at = static_cast<size_t>(y) * out.width + x;
					const uint8_t* s = image.rgba.data() + at * 4;
					const float rgb[3] = { table[s[0]], table[s[1]], table[s[2]] };
					out.v[at] += Luma(rgb) * (s[3] / 255.0f) * weight;
				}
		}
		return out;
	}

	//--- orientation

	Orientation DetectOrientation(const Gray& depth, const Image& normal, const int scale)
	{
		Orientation o;
		if (!depth.IsValid() || !normal.IsValid() || depth.width != normal.width || depth.height != normal.height)
			return o;
		const int step = std::max(1, scale);
		double sxn = 0, sxx = 0, snx = 0, syn = 0, syy = 0, sny = 0;
		for (int y = 2 * step; y < depth.height - 2 * step; y += step)
			for (int x = 2 * step; x < depth.width - 2 * step; x += step)
			{
				const double gx = depth.At(x + step, y) - depth.At(x - step, y);
				const double gy = depth.At(x, y + step) - depth.At(x, y - step);
				const double g = std::fabs(gx) + std::fabs(gy);
				if (g < 1e-5 || g > 0.08) //flat, or an edge between objects
					continue;
				const uint8_t* n = normal.rgba.data() + (static_cast<size_t>(y) * normal.width + x) * 4;
				const double nx = n[0] / 127.5 - 1.0, ny = n[1] / 127.5 - 1.0;
				sxn += gx * nx;
				sxx += gx * gx;
				snx += nx * nx;
				syn += gy * ny;
				syy += gy * gy;
				sny += ny * ny;
				o.samples++;
			}
		if (o.samples < 200 || sxx <= 0 || snx <= 0 || syy <= 0 || sny <= 0)
			return o;
		o.corrX = sxn / std::sqrt(sxx * snx);
		o.corrY = syn / std::sqrt(syy * sny);
		o.known = std::fabs(o.corrX) > 0.05;
		//bright is near: the depth value rises where the surface comes towards the camera, which is the way its normal leans
		//away from: rising to the right on a surface whose normal points left
		o.brightIsNear = o.known ? o.corrX < 0 : true;
		if (std::fabs(o.corrY) > 0.05)
			o.normalYDown = o.brightIsNear ? o.corrY < 0 : o.corrY > 0;
		return o;
	}

	Image NormalsYUp(const Image& normal, const bool flipY)
	{
		Image out = normal;
		if (flipY)
			for (size_t i = 1; i < out.rgba.size(); i += 4)
				out.rgba[i] = static_cast<uint8_t>(255 - out.rgba[i]);
		return out;
	}

	//--- depth fit

	namespace
	{
		struct Sample
		{
			double v, z;
		};

		struct Model
		{
			fieldpbr::DepthModel model = fieldpbr::DepthModel::Linear;
			double a = 0, b = 0;
			double Predict(const double v) const
			{
				if (model == fieldpbr::DepthModel::Linear)
					return a + b * v;
				const double w = a + b * v;
				return w > 1e-9 ? 1.0 / w : -1.0;
			}
		};

		double Error(const Model& m, const Sample& s)
		{
			const double p = m.Predict(s.v);
			return p <= 0 ? 1e9 : std::fabs(p - s.z) / s.z;
		}

		bool LeastSquares(const std::vector<Sample>& samples, const std::vector<size_t>& use, Model& m)
		{
			double sv = 0, sy = 0, svv = 0, svy = 0;
			for (const size_t i : use)
			{
				const double y = m.model == fieldpbr::DepthModel::Linear ? samples[i].z : 1.0 / samples[i].z;
				sv += samples[i].v;
				sy += y;
				svv += samples[i].v * samples[i].v;
				svy += samples[i].v * y;
			}
			const double n = static_cast<double>(use.size());
			const double den = n * svv - sv * sv;
			if (use.size() < 2 || std::fabs(den) < 1e-12)
				return false;
			m.b = (n * svy - sv * sy) / den;
			m.a = (sy - m.b * sv) / n;
			return true;
		}

		//RANSAC: the model most samples agree with (the floor can be hidden behind things, which makes outliers)
		Model Robust(const std::vector<Sample>& samples, const fieldpbr::DepthModel kind, size_t& inliers, double& median)
		{
			constexpr double TOLERANCE = 0.06;
			Model best;
			best.model = kind;
			inliers = 0;
			median = 1e9;
			uint32_t seed = 12345;
			const auto next = [&seed] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
			for (int iteration = 0; iteration < 600 && samples.size() >= 2; iteration++)
			{
				const Sample& p = samples[next() % samples.size()];
				const Sample& q = samples[next() % samples.size()];
				if (std::fabs(p.v - q.v) < 0.01)
					continue;
				Model m;
				m.model = kind;
				const double yp = kind == fieldpbr::DepthModel::Linear ? p.z : 1.0 / p.z;
				const double yq = kind == fieldpbr::DepthModel::Linear ? q.z : 1.0 / q.z;
				m.b = (yq - yp) / (q.v - p.v);
				m.a = yp - m.b * p.v;
				size_t count = 0;
				for (const Sample& s : samples)
					count += Error(m, s) < TOLERANCE;
				if (count > inliers)
				{
					inliers = count;
					best = m;
				}
			}
			if (inliers < 2)
				return best;
			for (int refine = 0; refine < 3; refine++)
			{
				std::vector<size_t> use;
				for (size_t i = 0; i < samples.size(); i++)
					if (Error(best, samples[i]) < TOLERANCE)
						use.push_back(i);
				Model m = best;
				if (!LeastSquares(samples, use, m))
					break;
				best = m;
			}
			std::vector<float> errors;
			inliers = 0;
			for (const Sample& s : samples)
			{
				const double e = Error(best, s);
				if (e < TOLERANCE)
				{
					inliers++;
					errors.push_back(static_cast<float>(e));
				}
			}
			median = errors.empty() ? 1e9 : Percentile(errors, 0.5);
			return best;
		}
	}

	DepthFit FitDepth(const fieldpbr::Camera& camera, const fieldpbr::Walkmesh& walkmesh, const Gray& depth, const int scale, const Layout& layout)
	{
		DepthFit best;
		char text[400];
		if (camera.zoom == 0 || walkmesh.triangles.empty() || !depth.IsValid())
		{
			best.report = "no camera or walkmesh";
			return best;
		}
		std::string tried;
		for (int variant = 0; variant < 4; variant++)
		{
			const fieldpbr::CameraTransform transform = fieldpbr::MakeCameraTransform(camera, variant);
			fieldpbr::Space space;
			space.measured = true;
			space.zoom = camera.zoom;
			space.originX = layout.shiftX;
			space.originY = layout.shiftY;
			std::vector<Sample> samples;
			size_t total = 0;
			constexpr int N = 4;
			for (const auto& t : walkmesh.triangles)
				for (int i = 0; i <= N; i++)
					for (int j = 0; i + j <= N; j++)
					{
						const double u = static_cast<double>(i) / N, w = static_cast<double>(j) / N;
						const Vec3 world = t[0] * (1 - u - w) + t[1] * u + t[2] * w;
						const Vec3 view = transform.ToView(world);
						double cx, cy;
						if (!space.Project(view, cx, cy))
							continue;
						total++;
						if (cx < 0 || cy < 0 || cx >= layout.width || cy >= layout.height)
							continue;
						samples.push_back({ depth.Sample(cx * scale, cy * scale), view.z });
					}
			for (const fieldpbr::DepthModel kind : { fieldpbr::DepthModel::Linear, fieldpbr::DepthModel::Inverse })
			{
				size_t inliers = 0;
				double median = 0;
				const Model m = samples.size() >= 20 ? Robust(samples, kind, inliers, median) : Model();
				snprintf(text, sizeof(text), "  camera %s, %s depth: %zu of %zu walkmesh points on the canvas, %zu fit within 6%%\n",
					variant == 0 ? "as Deling draws it" : variant == 1 ? "mirrored" : variant == 2 ? "upside down" : "turned round",
					kind == fieldpbr::DepthModel::Linear ? "linear" : "inverse", samples.size(), total, inliers);
				tried += text;
				const bool better = inliers > best.inliers || (inliers == best.inliers && inliers > 0 && median < best.medianError);
				if (!better)
					continue;
				best.inliers = inliers;
				best.medianError = median;
				best.samples = total;
				best.inside = samples.size();
				best.variant = variant;
				best.space = space;
				best.space.model = kind;
				best.space.a = m.a;
				best.space.b = m.b;
				double nearest = 1e9;
				for (const Sample& s : samples)
					nearest = std::min(nearest, s.z);
				best.space.nearest = std::max(1e-4, nearest * 0.05);
			}
		}
		best.ok = best.inliers >= 20 && best.inliers * 10 >= best.inside * 3;
		snprintf(text, sizeof(text), "%s: %zu of %zu points agree (median error %.1f%%)\n", best.ok ? "measured" : "no good fit",
			best.inliers, best.inside, best.medianError * 100.0);
		best.report = text + tried;
		return best;
	}

	//--- lightmap

	Rgb LightTint(const Rgb& bounced, const int blurRadius, const TintSettings& settings)
	{
		Rgb b = bounced;
		Blur(b, blurRadius);
		Rgb out;
		out.width = b.width;
		out.height = b.height;
		out.v.assign(b.v.size(), 1.0f);
		double sum[3] = { 0, 0, 0 };
		const size_t n = static_cast<size_t>(b.width) * b.height;
		for (size_t i = 0; i < n; i++)
			for (int c = 0; c < 3; c++)
				sum[c] += b.v[i * 3 + c];
		const float average[3] = { static_cast<float>(sum[0]), static_cast<float>(sum[1]), static_cast<float>(sum[2]) };
		const float averageLuma = Luma(average);
		if (averageLuma <= 0 || settings.strength <= 0)
			return out;
		float averageChroma[3];
		for (int c = 0; c < 3; c++)
			averageChroma[c] = std::max(1e-4f, average[c] / averageLuma);
		for (size_t i = 0; i < n; i++)
		{
			const float* c = b.v.data() + i * 3;
			const float l = Luma(c);
			if (l <= 1e-5f)
				continue;
			float t[3];
			for (int k = 0; k < 3; k++)
				t[k] = std::pow(std::max(1e-4f, c[k] / l / averageChroma[k]), settings.gain);
			float lt = Luma(t);
			for (int k = 0; k < 3; k++)
				t[k] = 1.0f + (t[k] / lt - 1.0f) * settings.strength;
			for (int k = 0; k < 3; k++)
				t[k] = std::clamp(t[k], 0.25f, 4.0f);
			lt = Luma(t);
			for (int k = 0; k < 3; k++)
				out.v[i * 3 + k] = t[k] / lt;
		}
		return out;
	}

	Split SplitLight(const Image& painting, const Rgb& albedo, float scale)
	{
		Split out;
		const size_t n = static_cast<size_t>(painting.width) * painting.height;
		const float* table = fieldpbr::SrgbTable();
		if (scale <= 0)
		{
			std::vector<float> values;
			for (size_t i = 0; i < n; i += 3) //every third pixel is plenty for a percentile
			{
				if (painting.rgba[i * 4 + 3] == 0)
					continue;
				float most = 0;
				for (int c = 0; c < 3; c++)
					most = std::max(most, table[painting.rgba[i * 4 + c]] / std::max(albedo.v[i * 3 + c], 1e-3f));
				values.push_back(most);
			}
			scale = static_cast<float>(std::ceil(Percentile(values, 0.999) * 20.0) / 20.0);
			scale = std::clamp(scale, 1.0f, 16.0f);
		}
		out.scale = scale;
		out.albedo.width = painting.width;
		out.albedo.height = painting.height;
		out.albedo.rgba.assign(n * 4, 0);
		out.light.width = painting.width;
		out.light.height = painting.height;
		out.light.v.assign(n * 3, 0.0f);
		for (size_t i = 0; i < n; i++)
		{
			const uint8_t alpha = painting.rgba[i * 4 + 3];
			out.albedo.rgba[i * 4 + 3] = alpha;
			if (alpha == 0)
				continue;
			bool raised = false;
			for (int c = 0; c < 3; c++)
			{
				const float p = table[painting.rgba[i * 4 + c]];
				const float lowest = p / scale; //below this the light would have to be brighter than the scale
				float a = std::clamp(albedo.v[i * 3 + c], 0.0f, 1.0f);
				if (a < lowest)
				{
					a = lowest;
					raised = true;
				}
				int code = fieldpbr::LinearToSrgb8(a);
				while (code < 255 && table[code] < lowest)
					code++;
				out.albedo.rgba[i * 4 + c] = static_cast<uint8_t>(code);
				const float q = table[code];
				out.light.v[i * 3 + c] = q > 0 ? std::min(p / q, scale) : 0.0f;
			}
			out.raised += raised;
		}
		return out;
	}

	Rgb LightFrom(const Image& painting, const Image& albedo, const float scale)
	{
		Rgb out;
		out.width = painting.width;
		out.height = painting.height;
		const size_t n = static_cast<size_t>(painting.width) * painting.height;
		out.v.assign(n * 3, 0.0f);
		const float* table = fieldpbr::SrgbTable();
		for (size_t i = 0; i < n; i++)
		{
			if (painting.rgba[i * 4 + 3] == 0)
				continue;
			for (int c = 0; c < 3; c++)
			{
				const float p = table[painting.rgba[i * 4 + c]], a = table[albedo.rgba[i * 4 + c]];
				out.v[i * 3 + c] = a > 0 ? std::min(p / a, scale) : (p > 0 ? scale : 0.0f);
			}
		}
		return out;
	}

	size_t Mismatches(const Image& layer, const Image& composite, std::vector<uint8_t>* mask)
	{
		const size_t n = static_cast<size_t>(layer.width) * layer.height;
		if (mask != nullptr)
			mask->assign(n, 0);
		size_t count = 0;
		const bool sameSize = composite.width == layer.width && composite.height == layer.height;
		for (size_t i = 0; i < n; i++)
		{
			if (layer.rgba[i * 4 + 3] == 0)
				continue;
			bool differs = !sameSize || composite.rgba[i * 4 + 3] == 0;
			for (int c = 0; c < 3 && !differs; c++)
				differs = std::abs(static_cast<int>(layer.rgba[i * 4 + c]) - composite.rgba[i * 4 + c]) > 3;
			if (differs)
			{
				count++;
				if (mask != nullptr)
					(*mask)[i] = 1;
			}
		}
		return count;
	}

	//--- AO

	Gray BakeAo(const Gray& depth, const Image& normal, const fieldpbr::Space& space, const int scale, const AoSettings& s)
	{
		Gray out;
		out.width = depth.width;
		out.height = depth.height;
		const size_t n = static_cast<size_t>(depth.width) * depth.height;
		out.v.assign(n, 1.0f);
		if (!depth.IsValid() || normal.width != depth.width || normal.height != depth.height)
			return out;
		std::vector<Vec3> position(n), normals(n);
		for (int y = 0; y < depth.height; y++)
			for (int x = 0; x < depth.width; x++)
			{
				const size_t i = static_cast<size_t>(y) * depth.width + x;
				position[i] = space.ViewPosition((x + 0.5) / scale, (y + 0.5) / scale, depth.v[i]);
				normals[i] = NormalAt(normal, i);
			}
		const double radiusPixels = s.radius * scale;
		const int directions = std::max(2, s.directions), steps = std::max(2, s.steps);
		ParallelRows(depth.height, s.threads, [&](const int y)
		{
			for (int x = 0; x < depth.width; x++)
			{
				const size_t i = static_cast<size_t>(y) * depth.width + x;
				const Vec3& p = position[i];
				const Vec3& nrm = normals[i];
				const double radiusView = space.ViewLength(s.radius, p.z);
				//interleaved gradient noise turns the pattern of directions per pixel (the blur below evens it out)
				const double noise = std::fmod(52.9829189 * std::fmod(0.06711056 * x + 0.00583715 * y, 1.0), 1.0);
				double occlusion = 0;
				for (int d = 0; d < directions; d++)
				{
					const double angle = 2.0 * PI * (d + noise) / directions;
					const double dx = std::cos(angle), dy = std::sin(angle);
					double highest = 0;
					for (int k = 1; k <= steps; k++)
					{
						const double t = radiusPixels * (k - 0.5 + 0.5 * noise) / steps;
						const int sx = static_cast<int>(std::lround(x + dx * t)), sy = static_cast<int>(std::lround(y + dy * t));
						if (sx < 0 || sy < 0 || sx >= depth.width || sy >= depth.height)
							break;
						const Vec3 h = position[static_cast<size_t>(sy) * depth.width + sx] - p;
						const double length = fieldpbr::Length(h);
						if (length < 1e-12 || length > radiusView)
							continue;
						const double rise = fieldpbr::Dot(h, nrm) / length;
						const double falloff = 1.0 - (length / radiusView) * (length / radiusView);
						highest = std::max(highest, rise * falloff);
					}
					occlusion += highest;
				}
				out.v[i] = static_cast<float>(std::pow(std::clamp(1.0 - occlusion / directions, 0.0, 1.0), s.strength));
			}
		});
		Blur(out, std::max(1, scale / 2));
		return out;
	}

	//--- lights
	//
	//Inverse lighting (Marschner & Greenberg 1997, "Inverse Lighting for Photography"): which few point lights, shining on
	//the room as the normal and depth maps shape it (n.l / r^2), best explain the diffuse light in the painting, on top of a
	//smooth ambient (a constant, the normal and a slope across the screen). Greedy: the candidate light that would take the
	//most out of what is left unexplained is added, all lights are fitted again (non-negative), and so on while a new light
	//still explains enough; then each light is moved around where it is to fit best.

	LightMarks ReadLightMarks(const Image& marks, const int nativeWidth, const int nativeHeight)
	{
		LightMarks out;
		out.width = nativeWidth;
		out.height = nativeHeight;
		if (!marks.IsValid() || nativeWidth <= 0 || nativeHeight <= 0)
			return out;
		out.remove.assign(static_cast<size_t>(nativeWidth) * nativeHeight, 0);
		const size_t n = static_cast<size_t>(marks.width) * marks.height;
		//which lamp colour each pixel is: 1 green (day), 2 blue (night), 3 cyan (both); red goes straight into remove
		std::vector<uint8_t> lamp(n, 0);
		const auto high = [](const int v) { return v >= 180; };
		const auto low = [](const int v) { return v <= 110; };
		for (size_t i = 0; i < n; i++)
		{
			const uint8_t* p = marks.rgba.data() + i * 4;
			if (p[3] < 128)
				continue;
			const int r = p[0], g = p[1], b = p[2];
			const int x = static_cast<int>(i % marks.width), y = static_cast<int>(i / marks.width);
			if (high(g) && high(b) && low(r))
				lamp[i] = 3;
			else if (high(g) && low(r) && low(b))
				lamp[i] = 1;
			else if (high(b) && low(r) && low(g))
				lamp[i] = 2;
			else if (high(r) && low(g) && low(b))
			{
				const int nx = std::clamp(x * nativeWidth / marks.width, 0, nativeWidth - 1);
				const int ny = std::clamp(y * nativeHeight / marks.height, 0, nativeHeight - 1);
				out.remove[static_cast<size_t>(ny) * nativeWidth + nx] = 1;
				out.removePixels++;
			}
			if (lamp[i] != 0)
				out.addPixels++;
		}
		//each spot of one colour (pixels touching each other) is one lamp, at its middle
		std::vector<size_t> stack;
		for (size_t start = 0; start < n; start++)
		{
			const uint8_t kind = lamp[start];
			if (kind == 0)
				continue;
			double sx = 0, sy = 0;
			size_t count = 0;
			stack.assign(1, start);
			lamp[start] = 0;
			while (!stack.empty())
			{
				const size_t i = stack.back();
				stack.pop_back();
				const int x = static_cast<int>(i % marks.width), y = static_cast<int>(i / marks.width);
				sx += x + 0.5;
				sy += y + 0.5;
				count++;
				const int nx[4] = { x - 1, x + 1, x, x }, ny[4] = { y, y, y - 1, y + 1 };
				for (int k = 0; k < 4; k++)
					if (nx[k] >= 0 && ny[k] >= 0 && nx[k] < marks.width && ny[k] < marks.height)
					{
						const size_t j = static_cast<size_t>(ny[k]) * marks.width + nx[k];
						if (lamp[j] == kind)
						{
							lamp[j] = 0;
							stack.push_back(j);
						}
					}
			}
			const std::pair<double, double> at(sx / count * nativeWidth / marks.width, sy / count * nativeHeight / marks.height);
			(kind == 1 ? out.add : kind == 2 ? out.addNight : out.addBoth).push_back(at);
		}
		//a red mark keeps lights a little way off it
		if (out.removePixels > 0)
		{
			std::vector<uint8_t> grown = out.remove;
			for (int y = 0; y < nativeHeight; y++)
				for (int x = 0; x < nativeWidth; x++)
					if (out.remove[static_cast<size_t>(y) * nativeWidth + x])
						for (int dy = -3; dy <= 3; dy++)
							for (int dx = -3; dx <= 3; dx++)
							{
								const int gx = x + dx, gy = y + dy;
								if (gx >= 0 && gy >= 0 && gx < nativeWidth && gy < nativeHeight)
									grown[static_cast<size_t>(gy) * nativeWidth + gx] = 1;
							}
			out.remove = std::move(grown);
		}
		return out;
	}

	namespace
	{
		struct FitSample
		{
			float px, py, pz; //view position
			float nx, ny, nz; //normal (view space)
			float t; //diffuse light (luminance)
			float rgb[3];
			float ambient[6]; //1, normal, screen slope
		};

		struct Candidate
		{
			Vec3 position;
			double soft = 0; //softening distance (keeps a light close to a surface finite)
			bool forced = false; //a lamp you marked
		};

		//n.l / r^2 of a light at c, at every sample
		void LightColumn(const Candidate& c, const std::vector<FitSample>& samples, std::vector<float>& out)
		{
			out.resize(samples.size());
			const float cx = static_cast<float>(c.position.x), cy = static_cast<float>(c.position.y), cz = static_cast<float>(c.position.z);
			const float soft2 = static_cast<float>(c.soft * c.soft);
			for (size_t i = 0; i < samples.size(); i++)
			{
				const FitSample& s = samples[i];
				const float dx = cx - s.px, dy = cy - s.py, dz = cz - s.pz;
				const float facing = dx * s.nx + dy * s.ny + dz * s.nz;
				if (facing <= 0)
				{
					out[i] = 0;
					continue;
				}
				const float r2 = dx * dx + dy * dy + dz * dz + soft2;
				out[i] = facing / (r2 * std::sqrt(r2));
			}
		}

		//min |t - A a - B c|^2 with c >= 0 (a, the ambient, free), from the normal equations: gram = M^T M, rhs = M^T t, tt =
		//t^T t, where M is the 6 ambient columns, then one column per light
		struct Normal
		{
			size_t m = 6;
			std::vector<double> gram, rhs[4]; //rhs: luminance, then red, green, blue
			double tt[4] = {};
		};

		double AmbientAt(const FitSample& s, const size_t k)
		{
			return s.ambient[k];
		}

		Normal BuildNormal(const std::vector<FitSample>& samples, const std::vector<const std::vector<float>*>& columns)
		{
			Normal e;
			const size_t lights = columns.size(), m = 6 + lights;
			e.m = m;
			e.gram.assign(m * m, 0.0);
			for (auto& r : e.rhs)
				r.assign(m, 0.0);
			std::vector<double> row(m);
			for (size_t i = 0; i < samples.size(); i++)
			{
				const FitSample& s = samples[i];
				for (size_t k = 0; k < 6; k++)
					row[k] = AmbientAt(s, k);
				for (size_t l = 0; l < lights; l++)
					row[6 + l] = (*columns[l])[i];
				const double t[4] = { s.t, s.rgb[0], s.rgb[1], s.rgb[2] };
				for (int c = 0; c < 4; c++)
					e.tt[c] += t[c] * t[c];
				for (size_t a = 0; a < m; a++)
				{
					if (row[a] == 0)
						continue;
					for (int c = 0; c < 4; c++)
						e.rhs[c][a] += row[a] * t[c];
					for (size_t b = a; b < m; b++)
						e.gram[a * m + b] += row[a] * row[b];
				}
			}
			for (size_t a = 0; a < m; a++)
				for (size_t b = 0; b < a; b++)
					e.gram[a * m + b] = e.gram[b * m + a];
			return e;
		}

		//light column l of the system replaced by another light's column
		void ReplaceColumn(Normal& e, const size_t l, const std::vector<float>& column, const std::vector<FitSample>& samples,
			const std::vector<const std::vector<float>*>& columns)
		{
			const size_t m = e.m, at = 6 + l;
			std::vector<double> dots(m, 0.0);
			double rhs[4] = {};
			for (size_t i = 0; i < samples.size(); i++)
			{
				const double v = column[i];
				if (v == 0)
					continue;
				const FitSample& s = samples[i];
				for (size_t k = 0; k < 6; k++)
					dots[k] += v * AmbientAt(s, k);
				for (size_t o = 0; o < columns.size(); o++)
					if (o != l)
						dots[6 + o] += v * (*columns[o])[i];
				dots[at] += v * v;
				rhs[0] += v * s.t;
				for (int c = 0; c < 3; c++)
					rhs[1 + c] += v * s.rgb[c];
			}
			for (size_t b = 0; b < m; b++)
			{
				e.gram[at * m + b] = dots[b];
				e.gram[b * m + at] = dots[b];
			}
			for (int c = 0; c < 4; c++)
				e.rhs[c][at] = rhs[c];
		}

		struct Solution
		{
			std::vector<double> coefficients;
			double energy = 0;
		};

		//projected Gauss-Seidel (the lights >= 0); skip: a light column left out (-1: none). channel 0 luminance, 1-3 rgb
		Solution Solve(const Normal& e, const int channel, const long skip = -1)
		{
			const size_t m = e.m;
			const std::vector<double>& rhs = e.rhs[channel];
			Solution out;
			out.coefficients.assign(m, 0.0);
			std::vector<double>& c = out.coefficients;
			for (int sweep = 0; sweep < 500; sweep++)
			{
				double change = 0;
				for (size_t a = 0; a < m; a++)
				{
					const double diagonal = e.gram[a * m + a];
					if (diagonal <= 1e-30 || static_cast<long>(a) == skip + 6)
						continue;
					double sum = rhs[a];
					for (size_t b = 0; b < m; b++)
						if (b != a)
							sum -= e.gram[a * m + b] * c[b];
					double value = sum / diagonal;
					if (a >= 6)
						value = std::max(0.0, value);
					change = std::max(change, std::fabs(value - c[a]) * std::sqrt(diagonal));
					c[a] = value;
				}
				if (change < 1e-10)
					break;
			}
			//|t - M c|^2 = t.t - 2 c.(M^T t) + c.(M^T M) c
			double energy = e.tt[channel];
			for (size_t a = 0; a < m; a++)
			{
				energy -= 2.0 * c[a] * rhs[a];
				for (size_t b = 0; b < m; b++)
					energy += c[a] * e.gram[a * m + b] * c[b];
			}
			out.energy = std::max(0.0, energy);
			return out;
		}

		//the room at a canvas spot: its view position and normal (normal map with green up, averaged over a few pixels)
		bool SurfaceAt(const double canvasX, const double canvasY, const Gray& depth, const Image* normal, const fieldpbr::Space& space,
			const int scale, Vec3& position, Vec3& n)
		{
			const double x = canvasX * scale, y = canvasY * scale;
			if (x < 0 || y < 0 || x >= depth.width || y >= depth.height)
				return false;
			position = space.ViewPosition(canvasX, canvasY, depth.Sample(x, y));
			n = { 0, 0, -1 };
			if (normal != nullptr && normal->IsValid() && normal->width == depth.width && normal->height == depth.height)
			{
				Vec3 sum{ 0, 0, 0 };
				const int r = std::max(1, scale / 2);
				for (int dy = -r; dy <= r; dy++)
					for (int dx = -r; dx <= r; dx++)
					{
						const int sx = std::clamp(static_cast<int>(x) + dx, 0, normal->width - 1);
						const int sy = std::clamp(static_cast<int>(y) + dy, 0, normal->height - 1);
						const uint8_t* p = normal->rgba.data() + (static_cast<size_t>(sy) * normal->width + sx) * 4;
						sum = sum + Vec3{ p[0] / 127.5 - 1.0, 1.0 - p[1] / 127.5, 1.0 - p[2] / 127.5 };
					}
				if (fieldpbr::Length(sum) > 1e-6)
					n = fieldpbr::Normalize(sum);
			}
			return true;
		}
	}

	fieldpbr::Lights FindLights(const Rgb& diffuse, const Image* normal, const Gray& depth, const fieldpbr::Space& space, const int scale,
		const LightMarks* marks, const LightSearch& search, std::string& report, const fieldpbr::LightTime time)
	{
		fieldpbr::Lights out;
		const bool night = time == fieldpbr::LightTime::Night;
		char text[400];
		if (!diffuse.IsValid() || !depth.IsValid() || scale <= 0)
			return out;
		const int nw = diffuse.width / scale, nh = diffuse.height / scale;
		if (nw < 8 || nh < 8)
			return out;
		const bool haveNormals = normal != nullptr && normal->IsValid() && normal->width == diffuse.width && normal->height == diffuse.height;
		//samples: blocks of step x step native pixels, averaged
		const int step = std::max(2, static_cast<int>(std::lround(std::sqrt(static_cast<double>(nw) * nh / 16000.0))));
		std::vector<FitSample> samples;
		for (int by = 0; by + step <= nh; by += step)
			for (int bx = 0; bx + step <= nw; bx += step)
			{
				double rgb[3] = { 0, 0, 0 }, value = 0;
				Vec3 nsum{ 0, 0, 0 };
				int count = 0;
				for (int y = by * scale; y < (by + step) * scale; y++)
					for (int x = bx * scale; x < (bx + step) * scale; x++)
					{
						const float* c = diffuse.At(x, y);
						for (int k = 0; k < 3; k++)
							rgb[k] += c[k];
						value += depth.At(x, y);
						if (haveNormals)
						{
							const uint8_t* p = normal->rgba.data() + (static_cast<size_t>(y) * normal->width + x) * 4;
							nsum = nsum + Vec3{ p[0] / 127.5 - 1.0, 1.0 - p[1] / 127.5, 1.0 - p[2] / 127.5 };
						}
						count++;
					}
				FitSample s{};
				for (int k = 0; k < 3; k++)
					s.rgb[k] = static_cast<float>(rgb[k] / count);
				s.t = Luma(s.rgb);
				if (!(s.t >= 0.0f) || (!night && s.t <= 1e-4f))
					continue; //by day, nothing painted (or black) says nothing about the light; at night, dark is what most of it is
				const double cx = bx + step * 0.5, cy = by + step * 0.5;
				const Vec3 p = space.ViewPosition(cx, cy, value / count);
				const Vec3 n = haveNormals && fieldpbr::Length(nsum) > 1e-6 ? fieldpbr::Normalize(nsum) : Vec3{ 0, 0, -1 };
				s.px = static_cast<float>(p.x);
				s.py = static_cast<float>(p.y);
				s.pz = static_cast<float>(p.z);
				s.nx = static_cast<float>(n.x);
				s.ny = static_cast<float>(n.y);
				s.nz = static_cast<float>(n.z);
				const float ambient[6] = { 1.0f, s.nx, s.ny, s.nz, static_cast<float>(cx / nw - 0.5), static_cast<float>(cy / nh - 0.5) };
				memcpy(s.ambient, ambient, sizeof(ambient));
				samples.push_back(s);
			}
		const auto removed = [&](const double canvasX, const double canvasY)
		{
			if (marks == nullptr || marks->remove.empty())
				return false;
			const int x = static_cast<int>(std::floor(canvasX)), y = static_cast<int>(std::floor(canvasY));
			return x >= 0 && y >= 0 && x < marks->width && y < marks->height && marks->remove[static_cast<size_t>(y) * marks->width + x] != 0;
		};
		//under a red mark there is nothing to explain either (a reflection, a glowing sign: no lamp's light)
		if (marks != nullptr && !marks->remove.empty())
			samples.erase(std::remove_if(samples.begin(), samples.end(), [&](const FitSample& s)
			{
				double cx = 0, cy = 0;
				return space.Project({ s.px, s.py, s.pz }, cx, cy) && removed(cx, cy);
			}), samples.end());
		if (samples.size() < 64)
		{
			report += "lights: too little of the field is painted to find lights in\n";
			return out;
		}
		//a light h of the surface's distance in front of the surface (along its normal)
		//a light can be anywhere in front of the room's surfaces (out of view too), not inside them, and not on a red mark
		const auto valid = [&](const Vec3& at)
		{
			if (!(at.z > space.nearest * 1.05))
				return false;
			double cx = 0, cy = 0;
			if (!space.Project(at, cx, cy))
				return false;
			if (removed(cx, cy))
				return false;
			if (cx >= 0 && cy >= 0 && cx < nw && cy < nh)
				return at.z < 0.995 * space.Distance(depth.Sample(cx * scale, cy * scale));
			return true;
		};
		const auto make = [&](const Vec3& at, Candidate& c)
		{
			c.position = at;
			c.soft = 0.002 * at.z; //only keeps it finite on a surface
			return valid(at);
		};
		std::vector<Candidate> candidates;
		//in front of the room's surfaces, along their normals
		for (int cy = 2; cy < nh; cy += 4)
			for (int cx = 2; cx < nw; cx += 4)
			{
				Vec3 p, n;
				if (!SurfaceAt(cx + 0.5, cy + 0.5, depth, haveNormals ? normal : nullptr, space, scale, p, n))
					continue;
				for (const double h : { 0.03, 0.08, 0.16, 0.3 })
				{
					Candidate c;
					if (make(p + n * (h * p.z), c))
						candidates.push_back(c);
				}
			}
		//beyond the canvas's edges (a lamp out of view lights what is in view), at the room's depths
		{
			std::vector<float> zs;
			for (const FitSample& s : samples)
				zs.push_back(s.pz);
			const double levels[5] = { Percentile(zs, 0.1), Percentile(zs, 0.3), Percentile(zs, 0.5), Percentile(zs, 0.7), Percentile(zs, 0.9) };
			const int margin = std::max(nw, nh) / 4;
			for (int cy = -margin; cy < nh + margin; cy += 6)
				for (int cx = -margin; cx < nw + margin; cx += 6)
				{
					if (cx >= 0 && cy >= 0 && cx < nw && cy < nh)
						continue;
					for (const double z : levels)
					{
						Candidate c;
						if (make({ (cx + 0.5 - space.originX) * z / space.zoom, (cy + 0.5 - space.originY) * z / space.zoom, z }, c))
							candidates.push_back(c);
					}
				}
		}
		//the lamps you marked go in first
		std::vector<Candidate> chosen;
		if (marks != nullptr)
			for (const auto& [x, y] : marks->Lamps(time))
			{
				Vec3 p, n;
				if (!SurfaceAt(x, y, depth, haveNormals ? normal : nullptr, space, scale, p, n))
					continue;
				Candidate c;
				c.position = p * 0.97; //a little in front of what it is painted on, towards the camera
				c.soft = 0.01 * p.z;
				c.forced = true;
				chosen.push_back(c);
			}
		std::vector<std::vector<float>> columns;
		std::vector<const std::vector<float>*> pointers;
		const auto refresh = [&]()
		{
			pointers.clear();
			for (const auto& column : columns)
				pointers.push_back(&column);
		};
		columns.resize(chosen.size());
		for (size_t i = 0; i < chosen.size(); i++)
			LightColumn(chosen[i], samples, columns[i]);
		refresh();
		double e0 = std::max(Solve(BuildNormal(samples, {}), 0).energy, 1e-12);
		Normal system = BuildNormal(samples, pointers);
		Solution current = Solve(system, 0);
		//greedy: the candidate that takes the most out of what is left
		//searched for: more than are kept (the large ones are told from the small ones once all are there)
		const int most = std::clamp(std::max(search.fitLights, search.maxLights), 0, fieldpbr::MAX_LIGHTS_IN_FILE);
		const auto found = [&]() { return static_cast<int>(std::count_if(chosen.begin(), chosen.end(), [](const Candidate& c) { return !c.forced; })); };
		std::vector<float> residual(samples.size());
		const int threads = std::clamp(search.threads, 1, 64);
		//light l moved around where it is, to where the fit is best: steps of 4%, 2%, 1% and 0.5% of its distance (round 0-3)
		const auto refine = [&](const size_t l, const int round)
		{
			if (chosen[l].forced)
				return;
			const Vec3 at = chosen[l].position;
			const double stride = at.z * 0.04 / (1 << round);
			//never onto another light's spot on the screen (two lights there only bend one's falloff)
			const auto apart = [&](const Vec3& p)
			{
				double x = 0, y = 0;
				space.Project(p, x, y);
				for (size_t o = 0; o < chosen.size(); o++)
				{
					double ox = 0, oy = 0;
					if (o != l && space.Project(chosen[o].position, ox, oy) && std::hypot(x - ox, y - oy) < 10.0)
						return false;
				}
				return true;
			};
			std::vector<Candidate> tries;
			for (int k = -2; k <= 2; k++)
				for (int j = -2; j <= 2; j++)
					for (int i = -2; i <= 2; i++)
					{
						Candidate c;
						if ((i || j || k) && make(at + Vec3{ i * stride, j * stride, k * stride }, c) && apart(c.position))
							tries.push_back(c);
					}
			std::vector<double> energy(tries.size(), 1e300);
			ParallelRows(static_cast<int>(tries.size()), threads, [&](const int k)
			{
				std::vector<float> column;
				LightColumn(tries[k], samples, column);
				Normal local = system;
				ReplaceColumn(local, l, column, samples, pointers);
				energy[k] = Solve(local, 0).energy;
			});
			const size_t best = static_cast<size_t>(std::min_element(energy.begin(), energy.end()) - energy.begin());
			if (!tries.empty() && energy[best] < current.energy)
			{
				chosen[l] = tries[best];
				LightColumn(chosen[l], samples, columns[l]);
				refresh();
				system = BuildNormal(samples, pointers);
				current = Solve(system, 0);
			}
		};
		for (int attempt = 0; attempt < 4 * most + 8 && found() < most && static_cast<int>(chosen.size()) < fieldpbr::MAX_LIGHTS_IN_FILE
			&& !candidates.empty(); attempt++)
		{
			for (size_t i = 0; i < samples.size(); i++)
			{
				const FitSample& s = samples[i];
				double fit = 0;
				for (size_t k = 0; k < 6; k++)
					fit += current.coefficients[k] * AmbientAt(s, k);
				for (size_t l = 0; l < columns.size(); l++)
					fit += current.coefficients[6 + l] * columns[l][i];
				residual[i] = static_cast<float>(s.t - fit);
			}
			std::vector<double> gain(candidates.size(), 0.0);
			ParallelRows(static_cast<int>(candidates.size()), threads, [&](const int k)
			{
				std::vector<float> column;
				LightColumn(candidates[k], samples, column);
				double along = 0, size = 0;
				for (size_t i = 0; i < samples.size(); i++)
				{
					along += column[i] * residual[i];
					size += static_cast<double>(column[i]) * column[i];
				}
				gain[k] = along > 0 && size > 0 ? along * along / size : 0.0;
			});
			const size_t best = static_cast<size_t>(std::max_element(gain.begin(), gain.end()) - gain.begin());
			if (gain[best] < search.minShare * e0)
				break;
			chosen.push_back(candidates[best]);
			columns.emplace_back();
			LightColumn(candidates[best], samples, columns.back());
			refresh();
			Normal next = BuildNormal(samples, pointers);
			const Solution solved = Solve(next, 0);
			//a light the fit gives (almost) nothing to, the others explaining its part, goes again
			const size_t added = chosen.size() - 1;
			if (solved.coefficients[6 + added] <= 0 || current.energy - solved.energy < 0.5 * search.minShare * e0)
			{
				chosen.pop_back();
				columns.pop_back();
				refresh();
				candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(best));
				continue;
			}
			system = std::move(next);
			current = solved;
			//placed better at once, so what is left near it isn't taken for another light
			for (int round = 0; round < 4; round++)
				refine(added, round);
			//candidates near the new light go, and those on the same spot of the screen at another depth (they would only split
			//it, or bend its falloff into some other shape)
			const Vec3 at = chosen.back().position;
			double ax = 0, ay = 0;
			space.Project(at, ax, ay);
			candidates.erase(std::remove_if(candidates.begin(), candidates.end(), [&](const Candidate& c)
			{
				double x = 0, y = 0;
				return fieldpbr::Length(c.position - at) < 0.1 * at.z || (space.Project(c.position, x, y) && std::hypot(x - ax, y - ay) < 10.0);
			}), candidates.end());
		}
		//every found light moved again (they have all moved since the first was placed)
		for (int round = 0; round < 4; round++)
			for (size_t l = 0; l < chosen.size(); l++)
				refine(l, round);
		//the light at a sample a tenth of which counts as lighting it: the ambient there, or the field's average where the
		//ambient is darker (at night it is nearly black)
		double average = 0;
		for (const FitSample& s : samples)
			average += s.t;
		average /= static_cast<double>(samples.size());
		const auto levelAt = [&](const size_t i)
		{
			double fit = 0;
			for (size_t k = 0; k < 6; k++)
				fit += current.coefficients[k] * AmbientAt(samples[i], k);
			return std::max(fit, average);
		};
		const auto reachOf = [&](const size_t l)
		{
			size_t lit = 0;
			for (size_t i = 0; i < samples.size(); i++)
				lit += current.coefficients[6 + l] * columns[l][i] >= 0.1 * levelAt(i);
			return static_cast<double>(lit) / static_cast<double>(samples.size());
		};
		//only the large ones stay (search.maxLights, those reaching most of the field: the room's own light). The small ones'
		//spots are left to the emissive texture and to lights you add, and the large ones are fitted again without those
		//spots, so they don't lean towards them
		{
			std::vector<size_t> foundOnes;
			for (size_t l = 0; l < chosen.size(); l++)
				if (!chosen[l].forced)
					foundOnes.push_back(l);
			const size_t keep = static_cast<size_t>(std::max(0, search.maxLights));
			if (foundOnes.size() > keep)
			{
				std::vector<double> reach(chosen.size(), 0.0);
				for (const size_t l : foundOnes)
					reach[l] = reachOf(l);
				std::stable_sort(foundOnes.begin(), foundOnes.end(), [&](const size_t a, const size_t b) { return reach[a] > reach[b]; });
				std::vector<bool> stays(chosen.size(), true);
				for (size_t k = keep; k < foundOnes.size(); k++)
					stays[foundOnes[k]] = false;
				//a sample in a small light's pool, where it outshines the lights that stay, goes
				std::vector<bool> goes(samples.size(), false);
				size_t gone = 0;
				for (size_t i = 0; i < samples.size(); i++)
				{
					double kept = 0;
					for (size_t l = 0; l < chosen.size(); l++)
						if (stays[l])
							kept += current.coefficients[6 + l] * columns[l][i];
					const double level = levelAt(i);
					for (size_t l = 0; l < chosen.size() && !goes[i]; l++)
					{
						const double v = current.coefficients[6 + l] * columns[l][i];
						goes[i] = !stays[l] && v >= 0.1 * level && v > kept;
					}
					gone += goes[i];
				}
				std::vector<Candidate> keptLights;
				for (size_t l = 0; l < chosen.size(); l++)
					if (stays[l])
						keptLights.push_back(chosen[l]);
				snprintf(text, sizeof(text), "%s lights: kept the %zu largest of %zu found; the small ones' spots (%.0f%% of the field) are left "
					"to the emissive texture and your own lights\n", night ? "night" : "day", keep, foundOnes.size(),
					100.0 * gone / static_cast<double>(samples.size()));
				report += text;
				chosen = std::move(keptLights);
				if (gone > 0 && gone + 64 < samples.size())
				{
					std::vector<FitSample> left;
					for (size_t i = 0; i < samples.size(); i++)
						if (!goes[i])
							left.push_back(samples[i]);
					samples = std::move(left);
					residual.resize(samples.size());
				}
				columns.assign(chosen.size(), {});
				for (size_t l = 0; l < chosen.size(); l++)
					LightColumn(chosen[l], samples, columns[l]);
				refresh();
				e0 = std::max(Solve(BuildNormal(samples, {}), 0).energy, 1e-12);
				system = BuildNormal(samples, pointers);
				current = Solve(system, 0);
				for (int round = 0; round < 4; round++)
					for (size_t l = 0; l < chosen.size(); l++)
						refine(l, round);
			}
		}
		//how much each light explains: what goes unexplained without it. A found light that, now the others have moved, hardly
		//explains anything goes (the weakest first, one at a time)
		std::vector<double> share;
		for (;;)
		{
			share.assign(chosen.size(), 0.0);
			for (size_t l = 0; l < chosen.size(); l++)
				share[l] = std::max(0.0, Solve(system, 0, static_cast<long>(l)).energy - current.energy) / e0;
			long weakest = -1;
			for (size_t l = 0; l < chosen.size(); l++)
				if (!chosen[l].forced && share[l] < 0.5 * search.minShare && (weakest < 0 || share[l] < share[weakest]))
					weakest = static_cast<long>(l);
			if (weakest < 0)
				break;
			chosen.erase(chosen.begin() + weakest);
			columns.erase(columns.begin() + weakest);
			refresh();
			system = BuildNormal(samples, pointers);
			current = Solve(system, 0);
		}
		//colour: the same fit per channel
		Solution channel[3];
		for (int c = 0; c < 3; c++)
			channel[c] = Solve(system, 1 + c);
		//how bright the ambient is, for the range
		std::vector<float> ambientLevel;
		ambientLevel.reserve(samples.size());
		for (const FitSample& s : samples)
		{
			double fit = 0;
			for (size_t k = 0; k < 6; k++)
				fit += current.coefficients[k] * AmbientAt(s, k);
			ambientLevel.push_back(static_cast<float>(std::max(fit, 1e-4)));
		}
		//how much of the field each light lights (where it adds a tenth or more to the light there)
		average = 0;
		for (const FitSample& s : samples)
			average += s.t;
		average /= static_cast<double>(samples.size());
		const double ambient = std::max({ Percentile(ambientLevel, 0.5), average, 1e-3 }); //the level the range is measured against
		std::vector<double> coverage(chosen.size(), 0.0);
		for (size_t l = 0; l < chosen.size(); l++)
			coverage[l] = reachOf(l);
		std::vector<size_t> order(chosen.size());
		std::iota(order.begin(), order.end(), 0);
		std::stable_sort(order.begin(), order.end(), [&](const size_t a, const size_t b)
		{
			if (chosen[a].forced != chosen[b].forced)
				return chosen[a].forced;
			return share[a] > share[b];
		});
		const double explained = std::max(0.0, 1.0 - current.energy / e0);
		snprintf(text, sizeof(text), "%s lights: %zu found by inverse lighting (they explain %.0f%% of the light above the ambient), "
			"%zu marked by you; %zu sample(s), %zu candidate position(s)\n", night ? "night" : "day",
			static_cast<size_t>(std::count_if(chosen.begin(), chosen.end(), [](const Candidate& c) { return !c.forced; })),
			100.0 * explained, static_cast<size_t>(std::count_if(chosen.begin(), chosen.end(), [](const Candidate& c) { return c.forced; })),
			samples.size(), candidates.size());
		report += text;
		for (const size_t l : order)
		{
			const Candidate& c = chosen[l];
			fieldpbr::Light light;
			light.name = std::string(c.forced ? "your " : "") + (night ? "night light " : "light ") + std::to_string(out.lights.size() + 1);
			light.time = time;
			double cx = 0, cy = 0;
			space.Project(c.position, cx, cy);
			light.x = cx;
			light.y = cy;
			light.depth = std::clamp(space.Value(c.position.z), 0.0, 1.0);
			double rgb[3];
			for (int k = 0; k < 3; k++)
				rgb[k] = std::max(0.0, channel[k].coefficients[6 + l]);
			const double top = std::max({ rgb[0], rgb[1], rgb[2] });
			for (int k = 0; k < 3; k++)
				light.color[k] = top > 0 ? static_cast<float>(rgb[k] / top) : 1.0f;
			//range: where its light falls to a tenth of the ambient
			const double strength = current.coefficients[6 + l];
			const double reach = strength > 0 ? std::sqrt(strength / (0.1 * ambient)) : 0.0;
			const double pixels = reach * space.zoom / std::max(c.position.z, 1e-6);
			light.range = static_cast<float>(std::clamp(pixels > 0 ? pixels : 64.0, 16.0, 320.0));
			light.baked = true;
			//on: they add search.intensity, characters cast shadows from them (shadow left out: 0.5, as for every painted light)
			light.intensity = search.intensity;
			light.shadow = -1.0f;
			light.source = c.forced ? fieldpbr::LightSource::Mark : fieldpbr::LightSource::Found;
			if (c.forced)
				snprintf(text, sizeof(text), "your %s mark in _LIGHTS.png: explains %.0f%% of the %s light above the ambient, lights %.0f%% of the "
					"field", night ? "night" : "day", 100.0 * share[l], night ? "night's" : "day's", 100.0 * coverage[l]);
			else
				snprintf(text, sizeof(text), "found by fieldprep in the %s: one of its large lights, explains %.0f%% of its light above the ambient, "
					"lights %.0f%% of the field", night ? "night (_RESM)" : "day", 100.0 * share[l], 100.0 * coverage[l]);
			light.note = text;
			snprintf(text, sizeof(text), "  %s at %.1f, %.1f (depth %.3f): explains %.0f%%, lights %.0f%% of the field, colour %.2f %.2f %.2f, "
				"range %.0f\n", light.name.c_str(), light.x, light.y, light.depth, 100.0 * share[l], 100.0 * coverage[l],
				light.color[0], light.color[1], light.color[2], light.range);
			report += text;
			out.lights.push_back(light);
		}
		return out;
	}

	size_t MergeDayNight(fieldpbr::Lights& day, const fieldpbr::Lights& night, const fieldpbr::Space& space)
	{
		using fieldpbr::LightTime;
		const auto on = [](const fieldpbr::Light& l) { return l.intensity > 0 || l.shadow != 0.0f; };
		size_t merged = 0;
		std::vector<bool> taken(day.lights.size(), false);
		std::vector<fieldpbr::Light> nightOnly;
		for (const fieldpbr::Light& n : night.lights)
		{
			const double nz = space.Distance(n.depth);
			long best = -1;
			double nearest = 10.0;
			for (size_t d = 0; d < day.lights.size(); d++)
			{
				const fieldpbr::Light& l = day.lights[d];
				const double dz = space.Distance(l.depth);
				const double gap = std::hypot(l.x - n.x, l.y - n.y);
				if (!taken[d] && gap < nearest && std::fabs(dz - nz) <= 0.15 * std::max(dz, nz))
				{
					nearest = gap;
					best = static_cast<long>(d);
				}
			}
			if (best < 0)
			{
				nightOnly.push_back(n);
				continue;
			}
			//the same lamp: on day and night, on if it is on in either
			fieldpbr::Light& l = day.lights[static_cast<size_t>(best)];
			taken[static_cast<size_t>(best)] = true;
			if (!on(l) && on(n))
			{
				l.intensity = n.intensity;
				l.shadow = n.shadow;
			}
			l.time = LightTime::Both;
			l.note += "\nalso in the night light (_RESM), as " + n.name + ": on day and night";
			merged++;
		}
		int number = 0;
		for (fieldpbr::Light& n : nightOnly)
		{
			const bool yours = n.name.rfind("your ", 0) == 0;
			n.name = std::string(yours ? "your " : "") + "night light " + std::to_string(++number);
			n.time = LightTime::Night;
			day.lights.push_back(n);
		}
		return merged;
	}

	size_t KeepYourLights(fieldpbr::Lights& lights, const fieldpbr::Lights& old, const bool oldHasSources)
	{
		if (!oldHasSources)
			return 0;
		size_t kept = 0;
		for (const fieldpbr::Light& l : old.lights)
			if (l.source == fieldpbr::LightSource::Yours)
			{
				lights.lights.push_back(l);
				kept++;
			}
		return kept;
	}

	Gray FlickerMask(const fieldpbr::Lights& lights, const Rgb& light, const Gray& depth, const Image& normal, const fieldpbr::Space& space,
		const int scale)
	{
		Gray out;
		out.width = light.width;
		out.height = light.height;
		const size_t n = static_cast<size_t>(light.width) * light.height;
		out.v.assign(n, 0.0f);
		struct Source
		{
			Vec3 position;
			double range;
		};
		std::vector<Source> sources;
		for (const fieldpbr::Light& l : lights.lights)
			if (l.OwnsMask()) //flickers, or its light group can turn it off
			{
				const Vec3 p = space.ViewPosition(l.x, l.y, l.depth);
				sources.push_back({ p, space.ViewLength(l.range, p.z) });
			}
		if (sources.empty() || depth.width != light.width || depth.height != light.height)
			return out;
		std::vector<float> lumas;
		for (size_t i = 0; i < n; i += 7)
		{
			const float l = Luma(light.v.data() + i * 3);
			if (l > 0)
				lumas.push_back(l);
		}
		const double ambient = Percentile(lumas, 0.3);
		const bool haveNormals = normal.width == light.width && normal.height == light.height;
		for (int y = 0; y < light.height; y++)
			for (int x = 0; x < light.width; x++)
			{
				const size_t i = static_cast<size_t>(y) * light.width + x;
				const Vec3 p = space.ViewPosition((x + 0.5) / scale, (y + 0.5) / scale, depth.v[i]);
				const Source* nearest = nullptr;
				double best = 1e300;
				for (const Source& s : sources)
				{
					const Vec3 d = s.position - p;
					const double dd = fieldpbr::Dot(d, d);
					if (dd < best)
					{
						best = dd;
						nearest = &s;
					}
				}
				const double distance = std::sqrt(best);
				if (nearest == nullptr || distance >= nearest->range)
					continue;
				const double x2 = (distance / nearest->range) * (distance / nearest->range);
				const double falloff = (1.0 - x2) * (1.0 - x2);
				double facing = 1.0;
				if (haveNormals && distance > 1e-9)
					facing = SmoothStep(-0.2, 0.3, fieldpbr::Dot(NormalAt(normal, i), (nearest->position - p) * (1.0 / distance)));
				const double lum = Luma(light.v.data() + i * 3);
				const double above = lum > 1e-6 ? std::clamp((lum - ambient) / lum, 0.0, 1.0) : 0.0;
				out.v[i] = static_cast<float>(above * falloff * facing);
			}
		return out;
	}

	//--- packing

	Image PackLightmap(const Rgb& light, const float scale, const Gray* mask)
	{
		Image out;
		out.width = light.width;
		out.height = light.height;
		const size_t n = static_cast<size_t>(light.width) * light.height;
		out.rgba.resize(n * 4);
		for (size_t i = 0; i < n; i++)
		{
			for (int c = 0; c < 3; c++)
				out.rgba[i * 4 + c] = fieldpbr::EncodeLightmap(light.v[i * 3 + c], scale);
			out.rgba[i * 4 + 3] = mask != nullptr && mask->IsValid() ? fieldpbr::EncodeFlickerMask(mask->v[i]) : 255;
		}
		return out;
	}

	Image PackNormalDepth(const Image& normal, const Gray& depth)
	{
		Image out = normal;
		const size_t n = static_cast<size_t>(out.width) * out.height;
		for (size_t i = 0; i < n; i++)
			out.rgba[i * 4 + 3] = static_cast<uint8_t>(std::clamp(std::lround(depth.v[i] * 255.0f), 0L, 255L));
		return out;
	}

	Image PackOrme(const int width, const int height, const Gray* ao, const Gray* roughness, const Gray* metallic, const Gray* emissive)
	{
		Image out;
		out.width = width;
		out.height = height;
		const size_t n = static_cast<size_t>(width) * height;
		out.rgba.resize(n * 4);
		const Gray* maps[4] = { ao, roughness, metallic, emissive };
		const float defaults[4] = { 1.0f, 0.8f, 0.0f, 0.0f };
		for (size_t i = 0; i < n; i++)
			for (int c = 0; c < 4; c++)
			{
				const float v = maps[c] != nullptr && maps[c]->IsValid() ? maps[c]->v[i] : defaults[c];
				out.rgba[i * 4 + c] = static_cast<uint8_t>(std::clamp(std::lround(v * 255.0f), 0L, 255L));
			}
		return out;
	}

	Gray EmissiveFromResidual(const Rgb& painting, const Rgb& residual, const float share)
	{
		Gray out;
		if (!painting.IsValid() || !residual.IsValid() || painting.width != residual.width || painting.height != residual.height)
			return out;
		out.width = painting.width;
		out.height = painting.height;
		const size_t n = static_cast<size_t>(out.width) * out.height;
		out.v.assign(n, 0.0f);
		const float from = std::clamp(share, 0.0f, 0.95f), full = std::min(1.0f, from + 0.4f);
		const auto smooth = [](const float a, const float b, const float x)
		{
			const float t = std::clamp((x - a) / std::max(b - a, 1e-6f), 0.0f, 1.0f);
			return t * t * (3.0f - 2.0f * t);
		};
		const auto luminance = [](const float* c)
		{
			return 0.2126f * std::max(c[0], 0.0f) + 0.7152f * std::max(c[1], 0.0f) + 0.0722f * std::max(c[2], 0.0f);
		};
		for (size_t i = 0; i < n; i++)
		{
			const float p = luminance(painting.v.data() + i * 3), r = luminance(residual.v.data() + i * 3);
			//(too dark to tell a glow from noise below about 1% of white)
			out.v[i] = smooth(from, full, r / std::max(p, 1e-4f)) * smooth(0.01f, 0.04f, r);
		}
		return out;
	}

	void CopyAlpha(Image& image, const Image& from)
	{
		const size_t n = static_cast<size_t>(image.width) * image.height;
		for (size_t i = 0; i < n && i * 4 + 3 < from.rgba.size(); i++)
			image.rgba[i * 4 + 3] = from.rgba[i * 4 + 3];
	}

	void PadUncovered(Image& image, const Image& coverage, const int rings, const int alpha)
	{
		const int w = image.width, h = image.height;
		if (coverage.width != w || coverage.height != h)
			return;
		std::vector<uint8_t> filled(static_cast<size_t>(w) * h);
		for (size_t i = 0; i < filled.size(); i++)
			filled[i] = coverage.rgba[i * 4 + 3] != 0;
		for (int ring = 0; ring < rings; ring++)
		{
			std::vector<uint8_t> next = filled;
			for (int y = 0; y < h; y++)
				for (int x = 0; x < w; x++)
				{
					const size_t i = static_cast<size_t>(y) * w + x;
					if (filled[i])
						continue;
					int sum[3] = { 0, 0, 0 }, count = 0;
					for (int dy = -1; dy <= 1; dy++)
						for (int dx = -1; dx <= 1; dx++)
						{
							const int sx = x + dx, sy = y + dy;
							if (sx < 0 || sy < 0 || sx >= w || sy >= h || !filled[static_cast<size_t>(sy) * w + sx])
								continue;
							const uint8_t* p = image.rgba.data() + (static_cast<size_t>(sy) * w + sx) * 4;
							for (int c = 0; c < 3; c++)
								sum[c] += p[c];
							count++;
						}
					if (count == 0)
						continue;
					for (int c = 0; c < 3; c++)
						image.rgba[i * 4 + c] = static_cast<uint8_t>((sum[c] + count / 2) / count);
					next[i] = 1;
				}
			filled.swap(next);
		}
		if (alpha >= 0)
			for (size_t i = 0; i < filled.size(); i++)
				if (coverage.rgba[i * 4 + 3] == 0)
					image.rgba[i * 4 + 3] = static_cast<uint8_t>(alpha);
	}

	//--- check image

	namespace
	{
		void Plot(Image& image, const int x, const int y, const uint8_t r, const uint8_t g, const uint8_t b)
		{
			if (x < 0 || y < 0 || x >= image.width || y >= image.height)
				return;
			uint8_t* p = image.rgba.data() + (static_cast<size_t>(y) * image.width + x) * 4;
			p[0] = r;
			p[1] = g;
			p[2] = b;
			p[3] = 255;
		}

		void Line(Image& image, double x0, double y0, const double x1, const double y1, const uint8_t r, const uint8_t g, const uint8_t b)
		{
			const double length = std::max(std::fabs(x1 - x0), std::fabs(y1 - y0));
			if (length > 20000)
				return;
			const int steps = std::max(1, static_cast<int>(std::ceil(length)));
			for (int i = 0; i <= steps; i++)
			{
				const double t = static_cast<double>(i) / steps;
				Plot(image, static_cast<int>(std::lround(x0 + (x1 - x0) * t)), static_cast<int>(std::lround(y0 + (y1 - y0) * t)), r, g, b);
			}
		}
	}

	Image CheckImage(const Image& composite, const fieldpbr::Camera* camera, const fieldpbr::Walkmesh* walkmesh, const int variant,
		const fieldpbr::Space& space, const fieldpbr::Lights& lights, const int scale)
	{
		Image out = composite;
		for (size_t i = 0; i < out.rgba.size(); i += 4)
		{
			for (int c = 0; c < 3; c++)
				out.rgba[i + c] = static_cast<uint8_t>(out.rgba[i + c] / 2);
			out.rgba[i + 3] = 255;
		}
		if (camera != nullptr && walkmesh != nullptr && camera->zoom != 0)
		{
			const fieldpbr::CameraTransform transform = fieldpbr::MakeCameraTransform(*camera, variant);
			fieldpbr::Space projection = space;
			projection.zoom = camera->zoom;
			for (const auto& t : walkmesh->triangles)
				for (int e = 0; e < 3; e++)
				{
					double ax, ay, bx, by;
					if (projection.Project(transform.ToView(t[e]), ax, ay) && projection.Project(transform.ToView(t[(e + 1) % 3]), bx, by))
						Line(out, ax * scale, ay * scale, bx * scale, by * scale, 0, 255, 0);
				}
		}
		for (const fieldpbr::Light& l : lights.lights)
		{
			const double cx = l.x * scale, cy = l.y * scale, r = l.range * scale;
			const uint8_t red = static_cast<uint8_t>(std::lround(255 * l.color[0])), green = static_cast<uint8_t>(std::lround(255 * l.color[1])),
				blue = static_cast<uint8_t>(std::lround(255 * l.color[2]));
			for (int k = 0; k < 256; k++)
			{
				const double a0 = 2 * PI * k / 256, a1 = 2 * PI * (k + 1) / 256;
				Line(out, cx + r * std::cos(a0), cy + r * std::sin(a0), cx + r * std::cos(a1), cy + r * std::sin(a1), 255, 255, 0);
			}
			const double arm = 4.0 * scale;
			Line(out, cx - arm, cy, cx + arm, cy, red, green, blue);
			Line(out, cx, cy - arm, cx, cy + arm, red, green, blue);
			Line(out, cx - arm, cy - 1, cx + arm, cy - 1, 255, 0, 0);
		}
		return out;
	}
	//--- for the characters

	Occlusion MeasureOcclusion(const std::vector<fieldlayers::Tile>& tiles, const fieldlayers::Layout& layout,
		const std::map<uint32_t, const Image*>& layers, const Gray& depth, const fieldpbr::Space& space, const int scale)
	{
		Occlusion out;
		std::map<int, std::vector<float>> distances;
		if (!depth.IsValid() || scale < 1)
			return out;
		for (size_t i = 0; i < tiles.size() && i < layout.pupu.size(); i++)
		{
			const fieldlayers::Tile& t = tiles[i];
			if (t.blend != 4 || t.z == 0 || t.z >= fieldpbr::FAR_TILE_Z)
				continue;
			const auto layer = layers.find(layout.pupu[i]);
			if (layer == layers.end() || layer->second == nullptr || !layer->second->IsValid())
				continue;
			const Image& image = *layer->second;
			for (int y = 0; y < 16 * scale; y++)
				for (int x = 0; x < 16 * scale; x++)
				{
					const int px = layout.x[i] * scale + x, py = layout.y[i] * scale + y;
					if (px < 0 || py < 0 || px >= image.width || py >= image.height || px >= depth.width || py >= depth.height)
						continue;
					if (image.rgba[(static_cast<size_t>(py) * image.width + px) * 4 + 3] < 128)
						continue;
					distances[t.z].push_back(static_cast<float>(space.Distance(depth.At(px, py))));
				}
		}
		std::vector<float> ratios;
		char line[200];
		for (auto& [z, list] : distances)
		{
			if (list.size() < 16)
				continue;
			const double nearest = Percentile(list, 0.05);
			if (!(nearest > 0))
				continue;
			out.nearest[z] = nearest;
			out.pixels[z] = list.size();
			const double ratio = 16.0 * z / nearest;
			ratios.push_back(static_cast<float>(ratio));
			snprintf(line, sizeof(line), "    Z %d: %zu pixels, nearest at %.5f: the game's depth there is %.0f x that\n", z, list.size(), nearest, ratio);
			out.report += line;
		}
		if (!ratios.empty())
			out.gameDepth = Percentile(ratios, 0.5);
		return out;
	}

	fieldpbr::Vec3 WorldUp(const fieldpbr::CameraTransform& camera, const fieldpbr::Walkmesh& walkmesh)
	{
		Vec3 sum{ 0, 0, 0 };
		for (const auto& t : walkmesh.triangles)
		{
			const Vec3 a = camera.ToView(t[0]), b = camera.ToView(t[1]), c = camera.ToView(t[2]);
			Vec3 n = fieldpbr::Cross(b - a, c - a); //length: twice the area
			if (n.y > 0)
				n = n * -1.0; //facing up on screen (y down)
			sum = sum + n;
		}
		if (fieldpbr::Length(sum) < 1e-12)
			return { 0, -1, 0 };
		return fieldpbr::Normalize(sum);
	}

	fieldpbr::ProbeGrid BakeProbes(const Rgb& painting, const Gray& depth, const Gray* flickerMask, const fieldpbr::Space& space,
		const int scale, const ProbeSettings& settings, double& zNear, double& zFar, std::string& report)
	{
		fieldpbr::ProbeGrid grid;
		report.clear();
		if (!painting.IsValid() || !depth.IsValid() || scale < 1 || depth.width != painting.width || depth.height != painting.height)
			return grid;
		//the room at native pixels: distance, light (linear) and flickering share
		const int w = depth.width / scale, h = depth.height / scale;
		if (w < 2 || h < 2)
			return grid;
		std::vector<float> room(static_cast<size_t>(w) * h), radiance(static_cast<size_t>(w) * h * 3, 0.0f), share(static_cast<size_t>(w) * h, 0.0f);
		const bool haveMask = flickerMask != nullptr && flickerMask->width == depth.width && flickerMask->height == depth.height;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++)
			{
				const size_t i = static_cast<size_t>(y) * w + x;
				room[i] = static_cast<float>(space.Distance(depth.At(x * scale + scale / 2, y * scale + scale / 2)));
				double rgb[3] = { 0, 0, 0 }, m = 0;
				for (int sy = 0; sy < scale; sy++)
					for (int sx = 0; sx < scale; sx++)
					{
						const float* c = painting.At(x * scale + sx, y * scale + sy);
						for (int k = 0; k < 3; k++)
							rgb[k] += c[k];
						if (haveMask)
							m += flickerMask->At(x * scale + sx, y * scale + sy);
					}
				const double n = static_cast<double>(scale) * scale;
				for (int k = 0; k < 3; k++)
					radiance[i * 3 + k] = static_cast<float>(rgb[k] / n);
				share[i] = static_cast<float>(m / n);
			}
		std::vector<float> sorted(room);
		const float nearestRoom = Percentile(sorted, 0.01), farRoom = Percentile(sorted, 0.98);
		zNear = std::max(space.nearest, 0.5 * nearestRoom);
		zFar = std::max(zNear * 1.5, static_cast<double>(farRoom));
		//what rays that leave the picture see: the room's average (the part behind the camera is unseen)
		double average[3] = { 0, 0, 0 };
		for (size_t i = 0; i < room.size(); i++)
			for (int k = 0; k < 3; k++)
				average[k] += radiance[i * 3 + k];
		for (double& a : average)
			a = a / static_cast<double>(room.size()) * 0.6;

		grid.nx = std::max(1, (w + settings.cell - 1) / settings.cell);
		grid.ny = std::max(1, (h + settings.cell - 1) / settings.cell);
		grid.nz = std::max(1, settings.slices);
		grid.data.assign(static_cast<size_t>(grid.nx) * grid.ny * grid.nz * 16, 0.0f);
		std::vector<uint8_t> valid(static_cast<size_t>(grid.nx) * grid.ny * grid.nz, 0);
		//directions: a Fibonacci sphere
		const int count = std::max(16, settings.directions);
		std::vector<Vec3> dirs(count);
		for (int i = 0; i < count; i++)
		{
			const double y = 1.0 - 2.0 * (i + 0.5) / count, r = std::sqrt(std::max(0.0, 1.0 - y * y)), phi = i * 2.39996322972865332;
			dirs[i] = { r * std::cos(phi), y, r * std::sin(phi) };
		}
		const double Y0 = 0.282094792, Y1 = 0.488602512, PI_ = 3.14159265358979, dw = 4.0 * PI_ / count;
		const auto probe = [&](const int ix, const int iy, const int iz)
		{
			const double cx = (ix + 0.5) * w / grid.nx, cy = (iy + 0.5) * h / grid.ny;
			const double z = zNear + (iz + 0.5) / grid.nz * (zFar - zNear);
			const int rx = std::clamp(static_cast<int>(cx), 0, w - 1), ry = std::clamp(static_cast<int>(cy), 0, h - 1);
			if (z >= room[static_cast<size_t>(ry) * w + rx] * 0.98)
				return; //behind the room's surface
			const Vec3 p{ (cx - space.originX) * z / space.zoom, (cy - space.originY) * z / space.zoom, z };
			double l00[3] = {}, lx[3] = {}, ly[3] = {}, lz[3] = {}, total = 0, flickering = 0;
			for (const Vec3& d : dirs)
			{
				double colour[3] = { average[0], average[1], average[2] }, m = 0;
				double t = z * 0.02, step = z * 0.02;
				for (int s = 0; s < 48; s++)
				{
					const Vec3 q = p + d * t;
					if (q.z <= space.nearest || q.z > zFar * 4.0)
						break; //towards the camera, or far out: the average
					const double sx = space.originX + q.x * space.zoom / q.z, sy = space.originY + q.y * space.zoom / q.z;
					if (sx < 0 || sy < 0 || sx >= w || sy >= h)
						break; //out of the picture
					const size_t i = static_cast<size_t>(sy) * w + static_cast<size_t>(sx);
					if (q.z >= room[i])
					{
						for (int k = 0; k < 3; k++)
							colour[k] = radiance[i * 3 + k];
						m = share[i];
						break;
					}
					t += step;
					step *= 1.12;
				}
				for (int k = 0; k < 3; k++)
				{
					l00[k] += colour[k] * Y0 * dw;
					lx[k] += colour[k] * Y1 * d.x * dw;
					ly[k] += colour[k] * Y1 * d.y * dw;
					lz[k] += colour[k] * Y1 * d.z * dw;
				}
				const double luma = 0.2126 * colour[0] + 0.7152 * colour[1] + 0.0722 * colour[2];
				total += luma;
				flickering += luma * m;
			}
			//irradiance: E(n) = pi Y0 L00 + 2pi/3 Y1 (L1 . n)
			const double a0 = PI_ * Y0, a1 = 2.0 * PI_ / 3.0 * Y1;
			float* b0 = grid.At(0, ix, iy, iz);
			float* bx = grid.At(1, ix, iy, iz);
			float* by = grid.At(2, ix, iy, iz);
			float* bz = grid.At(3, ix, iy, iz);
			for (int k = 0; k < 3; k++)
			{
				b0[k] = static_cast<float>(a0 * l00[k]);
				bx[k] = static_cast<float>(a1 * lx[k]);
				by[k] = static_cast<float>(a1 * ly[k]);
				bz[k] = static_cast<float>(a1 * lz[k]);
			}
			b0[3] = total > 0 ? static_cast<float>(flickering / total) : 0.0f;
			valid[(static_cast<size_t>(iz) * grid.ny + iy) * grid.nx + ix] = 1;
		};
		const int threads = std::max(1, settings.threads);
		std::vector<std::thread> workers;
		for (int t = 0; t < threads; t++)
			workers.emplace_back([&, t]()
			{
				for (int row = t; row < grid.ny * grid.nz; row += threads)
					for (int ix = 0; ix < grid.nx; ix++)
						probe(ix, row % grid.ny, row / grid.ny);
			});
		for (std::thread& worker : workers)
			worker.join();
		//probes behind the surface: the nearest in front of it, else the neighbours'
		size_t validCount = 0;
		const auto copy = [&](const int fx, const int fy, const int fz, const int tx, const int ty, const int tz)
		{
			for (int b = 0; b < 4; b++)
				memcpy(grid.At(b, tx, ty, tz), grid.At(b, fx, fy, fz), sizeof(float) * 4);
		};
		for (int iy = 0; iy < grid.ny; iy++)
			for (int ix = 0; ix < grid.nx; ix++)
				for (int iz = 0; iz < grid.nz; iz++)
				{
					const size_t v = (static_cast<size_t>(iz) * grid.ny + iy) * grid.nx + ix;
					if (valid[v])
					{
						validCount++;
						continue;
					}
					for (int back = iz - 1; back >= 0; back--)
						if (valid[(static_cast<size_t>(back) * grid.ny + iy) * grid.nx + ix])
						{
							copy(ix, iy, back, ix, iy, iz);
							valid[v] = 2;
							break;
						}
				}
		for (int pass = 0; pass < grid.nx + grid.ny; pass++)
		{
			bool missing = false;
			for (int iz = 0; iz < grid.nz; iz++)
				for (int iy = 0; iy < grid.ny; iy++)
					for (int ix = 0; ix < grid.nx; ix++)
					{
						const size_t v = (static_cast<size_t>(iz) * grid.ny + iy) * grid.nx + ix;
						if (valid[v])
							continue;
						missing = true;
						static const int offsets[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
						for (const auto& o : offsets)
						{
							const int nx = ix + o[0], ny = iy + o[1];
							if (nx < 0 || ny < 0 || nx >= grid.nx || ny >= grid.ny)
								continue;
							const size_t n = (static_cast<size_t>(iz) * grid.ny + ny) * grid.nx + nx;
							if (valid[n] && valid[n] != 3)
							{
								copy(nx, ny, iz, ix, iy, iz);
								valid[v] = 3;
								break;
							}
						}
					}
			for (uint8_t& v : valid)
				if (v == 3)
					v = 2;
			if (!missing)
				break;
		}
		char line[200];
		snprintf(line, sizeof(line), "%d x %d x %d probes from %.4f to %.4f, %zu in the open (the rest take a neighbour's)\n", grid.nx, grid.ny,
			grid.nz, zNear, zFar, validCount);
		report = line;
		return grid;
	}
}
