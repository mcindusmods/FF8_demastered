//fieldprep model: see models.h.

#include "models.h"

#include "../ff8_demaster/field_pbr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace fieldprep
{
	namespace
	{
		//the maps' suffixes (the image itself: ""); _A/_AM albedo, _N normal, _O AO, _R/_RC/_RM roughness, _M/_MC/_MM metallic,
		//_E emissive, _ORME the four packed; battle: _H height, _ORMH AO roughness metallic height packed, _EC the glows'
		//picture with its coverage (longer ones first: _ORMH before _H, _EC before _E)
		const char* const SUFFIXES[] = { "_ORME", "_ORMH", "_AM", "_A", "_N", "_O", "_RC", "_RM", "_R", "_MC", "_MM", "_M", "_EC", "_E",
			"_H" };

		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) { return static_cast<char>(tolower(c)); });
			return s;
		}

		std::string Upper(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) { return static_cast<char>(toupper(c)); });
			return s;
		}

		//every source pixel a destination pixel covers, with the share of it (area weights): f(dx, dy, sx, sy, weight)
		template <typename F>
		void ForCover(const int sw, const int sh, const int dw, const int dh, F&& f)
		{
			const double scaleX = static_cast<double>(sw) / dw, scaleY = static_cast<double>(sh) / dh;
			for (int dy = 0; dy < dh; dy++)
			{
				const double y0 = dy * scaleY, y1 = (dy + 1) * scaleY;
				for (int dx = 0; dx < dw; dx++)
				{
					const double x0 = dx * scaleX, x1 = (dx + 1) * scaleX;
					for (int sy = static_cast<int>(std::floor(y0)); sy < std::min(sh, static_cast<int>(std::ceil(y1))); sy++)
					{
						const double wy = std::min(y1, sy + 1.0) - std::max(y0, static_cast<double>(sy));
						if (wy <= 0)
							continue;
						for (int sx = static_cast<int>(std::floor(x0)); sx < std::min(sw, static_cast<int>(std::ceil(x1))); sx++)
						{
							const double wx = std::min(x1, sx + 1.0) - std::max(x0, static_cast<double>(sx));
							if (wx > 0)
								f(dx, dy, sx, sy, wx * wy);
						}
					}
				}
			}
		}

		uint8_t Byte(const double v)
		{
			return static_cast<uint8_t>(std::clamp(std::lround(v), 0L, 255L));
		}

		//a level of floats, 4 per pixel
		struct Floats
		{
			int width = 0, height = 0;
			std::vector<double> v;
		};

		Floats Half(const Floats& from, const std::function<void(const double* in, double weight, double* sum)>& add,
			const std::function<void(double* sum, double weight)>& finish)
		{
			Floats to;
			to.width = std::max(1, from.width / 2);
			to.height = std::max(1, from.height / 2);
			to.v.assign(static_cast<size_t>(to.width) * to.height * 4, 0.0);
			std::vector<double> sum(static_cast<size_t>(to.width) * to.height * 8, 0.0); //4 sums + weight (+ spare)
			ForCover(from.width, from.height, to.width, to.height, [&](int dx, int dy, int sx, int sy, double w)
			{
				double* s = sum.data() + (static_cast<size_t>(dy) * to.width + dx) * 8;
				add(from.v.data() + (static_cast<size_t>(sy) * from.width + sx) * 4, w, s);
				s[7] += w;
			});
			for (size_t i = 0; i < static_cast<size_t>(to.width) * to.height; i++)
			{
				double* s = sum.data() + i * 8;
				finish(s, s[7]);
				for (int c = 0; c < 4; c++)
					to.v[i * 4 + c] = s[c];
			}
			return to;
		}

		//Uncovered texels take the average of their neighbours that have a value, ring by ring outwards from the covered ones,
		//until every texel that can be reached has one (each texel is visited once). channels: how many of the 4 (3: colour)
		void Pad(Floats& f, const std::vector<uint8_t>& covered, const int channels)
		{
			const int w = f.width, h = f.height;
			const size_t n = static_cast<size_t>(w) * h;
			if (covered.size() != n)
				return;
			std::vector<uint8_t> filled(covered), queued(n, 0);
			std::vector<size_t> ring, next;
			const auto around = [&](const size_t i, auto&& visit)
			{
				const int x = static_cast<int>(i % w), y = static_cast<int>(i / w);
				for (int dy = -1; dy <= 1; dy++)
					for (int dx = -1; dx <= 1; dx++)
					{
						const int sx = x + dx, sy = y + dy;
						if ((dx != 0 || dy != 0) && sx >= 0 && sy >= 0 && sx < w && sy < h)
							visit(static_cast<size_t>(sy) * w + sx);
					}
			};
			for (size_t i = 0; i < n; i++)
				if (!filled[i])
					around(i, [&](const size_t j)
					{
						if (filled[j] && !queued[i])
						{
							queued[i] = 1;
							ring.push_back(i);
						}
					});
			std::vector<double> values;
			while (!ring.empty())
			{
				values.assign(ring.size() * 4, 0.0);
				for (size_t k = 0; k < ring.size(); k++)
				{
					int count = 0;
					around(ring[k], [&](const size_t j)
					{
						if (!filled[j])
							return;
						for (int c = 0; c < channels; c++)
							values[k * 4 + c] += f.v[j * 4 + c];
						count++;
					});
					for (int c = 0; c < channels; c++)
						values[k * 4 + c] /= std::max(count, 1);
				}
				next.clear();
				for (size_t k = 0; k < ring.size(); k++)
				{
					for (int c = 0; c < channels; c++)
						f.v[ring[k] * 4 + c] = values[k * 4 + c];
					filled[ring[k]] = 1;
				}
				for (const size_t i : ring)
					around(i, [&](const size_t j)
					{
						if (!filled[j] && !queued[j])
						{
							queued[j] = 1;
							next.push_back(j);
						}
					});
				ring.swap(next);
			}
		}

		//the next level's coverage: a texel is covered when any texel it is made from is
		std::vector<uint8_t> HalfCoverage(const std::vector<uint8_t>& covered, const int width, const int height, const int toWidth,
			const int toHeight)
		{
			std::vector<uint8_t> out(static_cast<size_t>(toWidth) * toHeight, 0);
			if (covered.size() != static_cast<size_t>(width) * height)
				return {};
			ForCover(width, height, toWidth, toHeight, [&](const int dx, const int dy, const int sx, const int sy, double)
			{
				if (covered[static_cast<size_t>(sy) * width + sx])
					out[static_cast<size_t>(dy) * toWidth + dx] = 1;
			});
			return out;
		}

		//some texels covered and some not (else there is nothing to pad, or nothing to pad from)
		bool Mixed(const std::vector<uint8_t>& covered)
		{
			bool some = false, notAll = false;
			for (const uint8_t c : covered)
			{
				some |= c != 0;
				notAll |= c == 0;
			}
			return some && notAll;
		}

		//the alpha scale that makes `image`'s alpha pass the test (alpha >= ref) as often as `target`
		double CoverageScale(const std::vector<double>& alpha, const double ref, const double target)
		{
			const auto passing = [&](const double scale)
			{
				size_t n = 0;
				for (const double a : alpha)
					n += a * scale >= ref;
				return alpha.empty() ? 0.0 : static_cast<double>(n) / alpha.size();
			};
			double low = 0.0, high = 1.0;
			while (passing(high) < target && high < 1024.0)
				high *= 2.0;
			for (int i = 0; i < 24; i++)
			{
				const double mid = (low + high) / 2;
				if (passing(mid) >= target)
					high = mid;
				else
					low = mid;
			}
			return high;
		}

		//--- the files of one image

		struct ModelFiles
		{
			std::string stem; //as the image's file has it
			std::map<std::string, fs::path> files; //"" (the image), "_A", "_N", ... -> the file
		};

		std::map<std::string, ModelFiles> ScanFolder(const fs::path& folder)
		{
			std::map<std::string, ModelFiles> found; //by lower-case stem
			std::error_code error;
			for (const auto& entry : fs::directory_iterator(folder, error))
			{
				if (!entry.is_regular_file(error) || Lower(entry.path().extension().string()) != ".png")
					continue;
				std::string suffix;
				const std::string stem = ModelStem(entry.path().filename().string(), &suffix);
				ModelFiles& m = found[Lower(stem)];
				if (suffix.empty() || m.stem.empty())
					m.stem = stem;
				m.files[suffix] = entry.path();
			}
			return found;
		}

		fs::file_time_type TimeOf(const fs::path& path)
		{
			std::error_code error;
			const fs::file_time_type t = fs::last_write_time(path, error);
			return error ? fs::file_time_type::min() : t;
		}

		//the DDS is there and newer than all it's made from
		bool UpToDate(const fs::path& out, const std::vector<fs::path>& from)
		{
			std::error_code error;
			if (!fs::exists(out, error))
				return false;
			const fs::file_time_type t = TimeOf(out);
			for (const fs::path& p : from)
				if (TimeOf(p) >= t)
					return false;
			return true;
		}

		bool WriteLevels(const fs::path& out, const std::vector<Image>& levels, const bool colour, const ModelOptions& o, const int threads,
			std::string& error)
		{
			std::vector<Bc7Level> bc7;
			for (const Image& level : levels)
				bc7.push_back({ level.width, level.height, EncodeBc7(level, colour, threads, o.quality) });
			return SaveDdsLevels(out.string(), bc7, colour, error);
		}

		bool Load(const fs::path& path, const int width, const int height, Image& out, std::string& notes, std::string& error)
		{
			if (!LoadRgba(path.string(), out, error))
			{
				error = path.filename().string() + ": " + error;
				return false;
			}
			if (out.width != width || out.height != height)
			{
				notes += ", " + path.filename().string() + " resized from " + std::to_string(out.width) + "x" + std::to_string(out.height);
				out = ResizeImage(out, width, height);
			}
			return true;
		}

		struct Result
		{
			std::string text;
			int made = 0, upToDate = 0;
			bool failed = false, skipped = false;
		};

		Result OneModel(const fs::path& folder, const ModelFiles& m, const ModelOptions& o, const int threads)
		{
			Result r;
			const auto start = std::chrono::steady_clock::now();
			const auto has = [&](const char* suffix) { return m.files.count(suffix) > 0; };
			const auto file = [&](const char* suffix) { return m.files.at(suffix); };
			//battle: a page's maps without its image (an animated page's, for all its frames) are made at their own size
			const bool noImage = !has("");
			if (noImage && !o.battle)
			{
				r.skipped = true;
				r.text = "  " + m.stem + ": no " + m.stem + ".png next to its maps (the game places the image by it): skipped\n";
				return r;
			}
			const fs::path image = noImage ? m.files.begin()->second : file("");
			Image base;
			std::string error, notes, made;
			if (!LoadRgba(image.string(), base, error))
			{
				r.failed = true;
				r.text = "  " + m.stem + ": ERROR " + image.filename().string() + ": " + error + "\n";
				return r;
			}
			if (noImage)
				notes += ", no " + m.stem + ".png: its maps at " + image.filename().string() + "'s size";
			const int width = base.width, height = base.height;
			const auto fail = [&](const std::string& what)
			{
				r.failed = true;
				r.text = "  " + m.stem + ": ERROR " + what + "\n";
				return r;
			};
			const char* albedoSuffix = has("_A") ? "_A" : has("_AM") ? "_AM" : "";
			const bool albedoThere = !noImage || *albedoSuffix != '\0';
			const fs::path albedoSource = albedoThere ? file(albedoSuffix) : image;
			//the islands (for padding the normal and ORME maps): from the albedo's alpha, read once
			std::vector<uint8_t> islands;
			bool islandsRead = false;
			const auto readIslands = [&]() -> bool
			{
				if (islandsRead || !o.pad)
					return true;
				islandsRead = true;
				if (!albedoThere)
					return true; //(no albedo: nothing says where the islands are)
				Image albedo = base;
				std::string ignored;
				if (*albedoSuffix != '\0' && !Load(albedoSource, width, height, albedo, ignored, error))
					return false;
				islands = IslandCoverage(albedo, base);
				return true;
			};
			//the albedo
			if (albedoThere)
			{
				const fs::path source = albedoSource, out = folder / (m.stem + ".dds");
				if (!o.force && UpToDate(out, { source, image }))
					r.upToDate++;
				else
				{
					Image albedo = base;
					if (*albedoSuffix != '\0' && !Load(source, width, height, albedo, notes, error))
						return fail(error);
					const std::vector<Image> levels = AlbedoLevels(albedo, o.coverage, o.pad);
					if (!WriteLevels(out, levels, true, o, threads, error))
						return fail(error);
					bool cutouts = false;
					for (size_t i = 3; i < albedo.rgba.size() && !cutouts; i += 4)
						cutouts = albedo.rgba[i] < 255;
					char line[200];
					snprintf(line, sizeof(line), "albedo from %s (%zu levels%s%s)", source.filename().string().c_str(), levels.size(),
						cutouts && o.coverage > 0 ? ", cut-outs keep their coverage" : "", cutouts && o.pad ? ", padded under the see-through texels" : "");
					made += line;
					r.made++;
				}
			}
			//the normal map
			if (has("_N"))
			{
				const fs::path source = file("_N"), out = folder / (m.stem + "_N.dds");
				if (!o.force && UpToDate(out, { source, image, albedoSource }))
					r.upToDate++;
				else
				{
					Image normal;
					if (!Load(source, width, height, normal, notes, error))
						return fail(error);
					for (size_t i = 0; i < normal.rgba.size(); i += 4)
					{
						if (o.flipGreen)
							normal.rgba[i + 1] = static_cast<uint8_t>(255 - normal.rgba[i + 1]);
						normal.rgba[i + 3] = 255; //BC7 spends every bit on the normal
					}
					if (!readIslands())
						return fail(error);
					if (!WriteLevels(out, NormalLevels(normal, islands), false, o, threads, error))
						return fail(error);
					made += std::string(made.empty() ? "" : ", ") + "normal" + (o.flipGreen ? " (green flipped)" : "");
					r.made++;
				}
			}
			//AO, roughness, metallic, and emissive (a field model's ORME) or height (a battle texture's ORMH)
			const char* const packedSuffix = o.battle ? "_ORMH" : "_ORME";
			const char* const channels[4][3] = { { "_O", nullptr, nullptr }, { "_R", "_RC", "_RM" }, { "_M", "_MC", "_MM" },
				{ o.battle ? "_H" : "_E", nullptr, nullptr } };
			const char* chosen[4] = {};
			bool anySeparate = false;
			for (int k = 0; k < 4; k++)
				for (const char* s : channels[k])
					if (s != nullptr && chosen[k] == nullptr && has(s))
					{
						chosen[k] = s;
						anySeparate = true;
					}
			if (anySeparate || has(packedSuffix))
			{
				std::vector<fs::path> sources{ image, albedoSource };
				if (anySeparate)
				{
					for (const char* s : chosen)
						if (s != nullptr)
							sources.push_back(file(s));
				}
				else
					sources.push_back(file(packedSuffix));
				const fs::path out = folder / (m.stem + packedSuffix + ".dds");
				if (!o.force && UpToDate(out, sources))
					r.upToDate++;
				else
				{
					Image orme;
					std::string which;
					if (anySeparate)
					{
						Image maps[4];
						const char* names[4] = { "AO", "roughness", "metallic", o.battle ? "height" : "emissive" };
						for (int k = 0; k < 4; k++)
						{
							if (chosen[k] == nullptr)
								continue;
							if (!Load(file(chosen[k]), width, height, maps[k], notes, error))
								return fail(error);
							which += std::string(which.empty() ? "" : " ") + names[k];
						}
						orme = (o.battle ? PackOrmh : PackOrme)(width, height, chosen[0] ? &maps[0] : nullptr, chosen[1] ? &maps[1] : nullptr,
							chosen[2] ? &maps[2] : nullptr, chosen[3] ? &maps[3] : nullptr);
						if (has(packedSuffix))
							notes += ", " + file(packedSuffix).filename().string() + " not used (the separate maps are)";
					}
					else
					{
						if (!Load(file(packedSuffix), width, height, orme, notes, error))
							return fail(error);
						which = "packed";
					}
					if (!readIslands())
						return fail(error);
					if (!WriteLevels(out, DataLevels(orme, islands), false, o, threads, error))
						return fail(error);
					made += std::string(made.empty() ? "" : ", ") + (packedSuffix + 1) + std::string(" (") + which + ")";
					r.made++;
				}
			}
			//a battle texture's glows at night: _E, their colour (DEMASTER adds it: the DDS's alpha is left white, BC7 keeps the
			//colour better), _EC, a picture of the glows with its coverage. An _E.png with an alpha of its own is that picture:
			//it becomes _EC.dds (unless there is an _EC.png)
			if (o.battle)
				for (const char* suffix : { "_E", "_EC" })
				{
					if (!has(suffix))
						continue;
					const bool isEc = strcmp(suffix, "_EC") == 0;
					const fs::path source = file(suffix), asE = folder / (m.stem + "_E.dds"), asEc = folder / (m.stem + "_EC.dds");
					//(an _E.png's alpha decides which it makes: either up to date is enough)
					if (!o.force && (isEc ? UpToDate(asEc, { source, image })
						: UpToDate(asE, { source, image }) || (!has("_EC") && UpToDate(asEc, { source, image }))))
					{
						r.upToDate++;
						continue;
					}
					Image glows;
					if (!Load(source, width, height, glows, notes, error))
						return fail(error);
					bool covered = isEc;
					if (!isEc && fieldpbr::EmissiveHasAlpha(glows.rgba.data(), static_cast<size_t>(width) * height))
					{
						if (has("_EC"))
							notes += ", " + source.filename().string() + "'s alpha isn't read (there is an _EC.png: DEMASTER takes it)";
						else
							covered = true;
					}
					if (!isEc && !covered && !has("_EC") && FileExists(asEc.string()))
						notes += ", " + asEc.filename().string() + " from before is still there (made when " + source.filename().string()
							+ " had an alpha): DEMASTER takes it before _E.dds, delete it";
					if (!WriteLevels(covered ? asEc : asE, EmissiveLevels(glows, !covered), true, o, threads, error))
						return fail(error);
					made += std::string(made.empty() ? "" : ", ") + (isEc ? "the glows' picture (_EC)" : covered
						? "the glows' picture (_E.png has an alpha: its coverage, written as _EC.dds)" : "emissive (_E, added)");
					r.made++;
				}
			if (o.battle && has("_E") && has("_EC"))
				notes += ", both _E and _EC (DEMASTER takes _EC)";
			if (o.pad && islandsRead)
				notes += !albedoThere ? ", not padded (no albedo to tell its islands by)"
					: islands.empty() ? ", not padded (the albedo has no see-through texels to tell its islands by)"
					: ", padded around its islands";
			const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			char head[96];
			snprintf(head, sizeof(head), "  %s %dx%d: ", m.stem.c_str(), width, height);
			if (r.made == 0)
				r.text = head + std::string("up to date") + notes + "\n";
			else
			{
				char tail[48];
				snprintf(tail, sizeof(tail), " (%.1f s)", seconds);
				r.text = head + made + (r.upToDate ? ", the rest up to date" : "") + notes + tail + "\n";
			}
			return r;
		}
	}

	std::string ModelStem(const std::string& fileName, std::string* suffix)
	{
		std::string name = fileName;
		const size_t dot = name.rfind('.');
		if (dot != std::string::npos)
			name.resize(dot);
		const std::string upper = Upper(name);
		for (const char* s : SUFFIXES)
		{
			const size_t n = strlen(s);
			if (upper.size() > n && upper.compare(upper.size() - n, n, s) == 0)
			{
				if (suffix != nullptr)
					*suffix = s;
				return name.substr(0, name.size() - n);
			}
		}
		if (suffix != nullptr)
			suffix->clear();
		return name;
	}

	double Coverage(const Image& image, const float coverage)
	{
		const size_t n = static_cast<size_t>(image.width) * image.height;
		if (n == 0)
			return 0.0;
		size_t passing = 0;
		for (size_t i = 0; i < n; i++)
			passing += image.rgba[i * 4 + 3] / 255.0 >= coverage;
		return static_cast<double>(passing) / n;
	}

	std::vector<uint8_t> IslandCoverage(const Image& albedo, const Image& image)
	{
		for (const Image* from : { &albedo, &image })
		{
			std::vector<uint8_t> covered(static_cast<size_t>(from->width) * from->height);
			for (size_t i = 0; i < covered.size(); i++)
				covered[i] = from->rgba[i * 4 + 3] > 0;
			if (from->width == albedo.width && from->height == albedo.height && Mixed(covered))
				return covered;
		}
		return {};
	}

	std::vector<Image> AlbedoLevels(const Image& level0, const float coverage, const bool pad)
	{
		std::vector<Image> levels{ level0 };
		//linear light, colour weighted by alpha (see-through pixels don't bleed into their neighbours)
		Floats f;
		f.width = level0.width;
		f.height = level0.height;
		f.v.resize(level0.rgba.size());
		bool cutouts = false;
		for (size_t i = 0; i < level0.rgba.size(); i += 4)
		{
			for (int c = 0; c < 3; c++)
				f.v[i + c] = fieldpbr::SrgbToLinear(level0.rgba[i + c] / 255.0f);
			f.v[i + 3] = level0.rgba[i + 3] / 255.0;
			cutouts |= level0.rgba[i + 3] < 255;
		}
		const bool keepCoverage = cutouts && coverage > 0.0f;
		const double target = keepCoverage ? Coverage(level0, coverage) : 0.0;
		//the colour under the see-through texels: their island's edge's (the levels below are made from it too)
		const auto padLevel = [&](Image& out)
		{
			std::vector<uint8_t> covered(static_cast<size_t>(f.width) * f.height);
			for (size_t i = 0; i < covered.size(); i++)
				covered[i] = f.v[i * 4 + 3] > 0.0;
			if (!Mixed(covered))
				return;
			Pad(f, covered, 3);
			for (size_t i = 0; i < covered.size(); i++)
				if (!covered[i])
					for (int c = 0; c < 3; c++)
						out.rgba[i * 4 + c] = Byte(255.0 * fieldpbr::LinearToSrgb(static_cast<float>(f.v[i * 4 + c])));
		};
		if (pad && cutouts)
			padLevel(levels[0]);
		while (f.width > 1 || f.height > 1)
		{
			f = Half(f, [](const double* in, const double w, double* s)
			{
				for (int c = 0; c < 3; c++)
				{
					s[c] += in[c] * in[3] * w;
					s[4 + c] += in[c] * w; //plain, for pixels nothing covers
				}
				s[3] += in[3] * w;
			}, [](double* s, const double w)
			{
				for (int c = 0; c < 3; c++)
					s[c] = s[3] > 1e-9 ? s[c] / s[3] : s[4 + c] / w;
				s[3] /= w;
			});
			Image out;
			out.width = f.width;
			out.height = f.height;
			out.rgba.resize(f.v.size());
			std::vector<double> alpha(static_cast<size_t>(f.width) * f.height);
			for (size_t i = 0; i < alpha.size(); i++)
				alpha[i] = f.v[i * 4 + 3];
			//cut-outs keep how much of them passes the game's test (else hair and fringes thin out in the small levels)
			const double scale = keepCoverage ? CoverageScale(alpha, coverage, target) : 1.0;
			for (size_t i = 0; i < alpha.size(); i++)
			{
				for (int c = 0; c < 3; c++)
					out.rgba[i * 4 + c] = Byte(255.0 * fieldpbr::LinearToSrgb(static_cast<float>(f.v[i * 4 + c])));
				out.rgba[i * 4 + 3] = Byte(255.0 * std::min(1.0, alpha[i] * scale));
			}
			if (pad && cutouts)
				padLevel(out);
			levels.push_back(std::move(out));
		}
		return levels;
	}

	std::vector<Image> DataLevels(const Image& level0, const std::vector<uint8_t>& coveredIn)
	{
		std::vector<Image> levels{ level0 };
		Floats f;
		f.width = level0.width;
		f.height = level0.height;
		f.v.assign(level0.rgba.begin(), level0.rgba.end());
		std::vector<uint8_t> covered = coveredIn.size() == static_cast<size_t>(f.width) * f.height && Mixed(coveredIn) ? coveredIn
			: std::vector<uint8_t>();
		if (!covered.empty())
		{
			Pad(f, covered, 4);
			for (size_t i = 0; i < covered.size(); i++)
				if (!covered[i])
					for (int c = 0; c < 4; c++)
						levels[0].rgba[i * 4 + c] = Byte(f.v[i * 4 + c]);
		}
		while (f.width > 1 || f.height > 1)
		{
			const int fromWidth = f.width, fromHeight = f.height;
			f = Half(f, [](const double* in, const double w, double* s)
			{
				for (int c = 0; c < 4; c++)
					s[c] += in[c] * w;
			}, [](double* s, const double w)
			{
				for (int c = 0; c < 4; c++)
					s[c] /= w;
			});
			Image out;
			out.width = f.width;
			out.height = f.height;
			out.rgba.resize(f.v.size());
			if (!covered.empty())
			{
				covered = HalfCoverage(covered, fromWidth, fromHeight, f.width, f.height);
				if (Mixed(covered))
					Pad(f, covered, 4);
			}
			for (size_t i = 0; i < f.v.size(); i++)
				out.rgba[i] = Byte(f.v[i]);
			levels.push_back(std::move(out));
		}
		return levels;
	}

	std::vector<Image> NormalLevels(const Image& level0, const std::vector<uint8_t>& coveredIn)
	{
		std::vector<Image> levels{ level0 };
		Floats f;
		f.width = level0.width;
		f.height = level0.height;
		f.v.resize(level0.rgba.size());
		for (size_t i = 0; i < level0.rgba.size(); i += 4)
		{
			for (int c = 0; c < 3; c++)
				f.v[i + c] = level0.rgba[i + c] / 255.0 * 2.0 - 1.0;
			f.v[i + 3] = 1.0;
		}
		//a padded normal is the average of its neighbours', made unit length again
		const auto padNormals = [&](const std::vector<uint8_t>& covered, Image* out)
		{
			Pad(f, covered, 3);
			for (size_t i = 0; i < covered.size(); i++)
			{
				if (covered[i])
					continue;
				double* v = f.v.data() + i * 4;
				const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
				if (length > 1e-9)
					for (int c = 0; c < 3; c++)
						v[c] /= length;
				else
				{
					v[0] = v[1] = 0.0;
					v[2] = 1.0;
				}
				if (out != nullptr)
					for (int c = 0; c < 3; c++)
						out->rgba[i * 4 + c] = Byte((v[c] * 0.5 + 0.5) * 255.0);
			}
		};
		std::vector<uint8_t> covered = coveredIn.size() == static_cast<size_t>(f.width) * f.height && Mixed(coveredIn) ? coveredIn
			: std::vector<uint8_t>();
		if (!covered.empty())
			padNormals(covered, &levels[0]);
		while (f.width > 1 || f.height > 1)
		{
			const int fromWidth = f.width, fromHeight = f.height;
			f = Half(f, [](const double* in, const double w, double* s)
			{
				for (int c = 0; c < 3; c++)
					s[c] += in[c] * w;
			}, [](double* s, const double)
			{
				const double length = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
				if (length > 1e-9)
					for (int c = 0; c < 3; c++)
						s[c] /= length;
				else
				{
					s[0] = s[1] = 0.0;
					s[2] = 1.0;
				}
				s[3] = 1.0;
			});
			if (!covered.empty())
			{
				covered = HalfCoverage(covered, fromWidth, fromHeight, f.width, f.height);
				if (Mixed(covered))
					padNormals(covered, nullptr);
			}
			Image out;
			out.width = f.width;
			out.height = f.height;
			out.rgba.resize(f.v.size());
			for (size_t i = 0; i < f.v.size(); i += 4)
			{
				for (int c = 0; c < 3; c++)
					out.rgba[i + c] = Byte((f.v[i + c] * 0.5 + 0.5) * 255.0);
				out.rgba[i + 3] = 255;
			}
			levels.push_back(std::move(out));
		}
		return levels;
	}

	Image ResizeImage(const Image& image, const int width, const int height)
	{
		Image out;
		out.width = width;
		out.height = height;
		out.rgba.assign(static_cast<size_t>(width) * height * 4, 0);
		if (!image.IsValid() || width <= 0 || height <= 0)
			return out;
		if (width <= image.width && height <= image.height)
		{
			std::vector<double> sum(static_cast<size_t>(width) * height * 5, 0.0);
			ForCover(image.width, image.height, width, height, [&](int dx, int dy, int sx, int sy, double w)
			{
				double* s = sum.data() + (static_cast<size_t>(dy) * width + dx) * 5;
				const uint8_t* p = image.rgba.data() + (static_cast<size_t>(sy) * image.width + sx) * 4;
				for (int c = 0; c < 4; c++)
					s[c] += p[c] * w;
				s[4] += w;
			});
			for (size_t i = 0; i < static_cast<size_t>(width) * height; i++)
				for (int c = 0; c < 4; c++)
					out.rgba[i * 4 + c] = Byte(sum[i * 5 + c] / sum[i * 5 + 4]);
			return out;
		}
		for (int y = 0; y < height; y++)
			for (int x = 0; x < width; x++)
			{
				const double sx = std::clamp((x + 0.5) * image.width / width - 0.5, 0.0, image.width - 1.0);
				const double sy = std::clamp((y + 0.5) * image.height / height - 0.5, 0.0, image.height - 1.0);
				const int x0 = static_cast<int>(sx), y0 = static_cast<int>(sy);
				const int x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
				const double fx = sx - x0, fy = sy - y0;
				for (int c = 0; c < 4; c++)
				{
					const auto at = [&](int px, int py) { return image.rgba[(static_cast<size_t>(py) * image.width + px) * 4 + c]; };
					const double v = (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy) + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy;
					out.rgba[(static_cast<size_t>(y) * width + x) * 4 + c] = Byte(v);
				}
			}
		return out;
	}

	Image PackOrme(const int width, const int height, const Image* ao, const Image* roughness, const Image* metallic, const Image* emissive)
	{
		Image out;
		out.width = width;
		out.height = height;
		out.rgba.resize(static_cast<size_t>(width) * height * 4);
		for (size_t i = 0; i < static_cast<size_t>(width) * height; i++)
		{
			uint8_t* p = out.rgba.data() + i * 4;
			p[0] = ao != nullptr ? ao->rgba[i * 4] : 255; //grey maps: their first channel
			p[1] = roughness != nullptr ? roughness->rgba[i * 4] : 179; //0.7
			p[2] = metallic != nullptr ? metallic->rgba[i * 4] : 0;
			p[3] = emissive != nullptr ? std::max({ emissive->rgba[i * 4], emissive->rgba[i * 4 + 1], emissive->rgba[i * 4 + 2] }) : 0; //a coloured glow counts
		}
		return out;
	}

	Image PackOrmh(const int width, const int height, const Image* ao, const Image* roughness, const Image* metallic, const Image* heightMap)
	{
		Image out;
		out.width = width;
		out.height = height;
		out.rgba.resize(static_cast<size_t>(width) * height * 4);
		for (size_t i = 0; i < static_cast<size_t>(width) * height; i++)
		{
			uint8_t* p = out.rgba.data() + i * 4;
			p[0] = ao != nullptr ? ao->rgba[i * 4] : 255; //grey maps: their first channel
			p[1] = roughness != nullptr ? roughness->rgba[i * 4] : 179; //0.7
			p[2] = metallic != nullptr ? metallic->rgba[i * 4] : 0;
			p[3] = heightMap != nullptr ? heightMap->rgba[i * 4] : 255; //the surface (DEMASTER's parallax moves nothing)
		}
		return out;
	}

	std::vector<Image> EmissiveLevels(const Image& level0, const bool additive)
	{
		Image first = level0;
		if (additive)
			for (size_t i = 3; i < first.rgba.size(); i += 4)
				first.rgba[i] = 255;
		std::vector<Image> levels{ first };
		Floats f;
		f.width = first.width;
		f.height = first.height;
		f.v.resize(first.rgba.size());
		for (size_t i = 0; i < first.rgba.size(); i += 4)
		{
			for (int c = 0; c < 3; c++)
				f.v[i + c] = fieldpbr::SrgbToLinear(first.rgba[i + c] / 255.0f);
			f.v[i + 3] = first.rgba[i + 3] / 255.0;
		}
		while (f.width > 1 || f.height > 1)
		{
			f = Half(f, [](const double* in, const double w, double* sum)
			{
				for (int c = 0; c < 4; c++)
					sum[c] += in[c] * w;
			}, [](double* sum, const double w)
			{
				for (int c = 0; c < 4; c++)
					sum[c] /= w;
			});
			Image out;
			out.width = f.width;
			out.height = f.height;
			out.rgba.resize(f.v.size());
			for (size_t i = 0; i < f.v.size(); i += 4)
			{
				for (int c = 0; c < 3; c++)
					out.rgba[i + c] = Byte(255.0 * fieldpbr::LinearToSrgb(static_cast<float>(f.v[i + c])));
				out.rgba[i + 3] = Byte(255.0 * f.v[i + 3]);
			}
			levels.push_back(std::move(out));
		}
		return levels;
	}

	bool NameMatches(const std::string& patterns, const std::string& name)
	{
		std::string list = patterns;
		std::replace(list.begin(), list.end(), ',', ' ');
		bool any = false;
		size_t at = 0;
		while (at < list.size())
		{
			const size_t start = list.find_first_not_of(" \t", at);
			if (start == std::string::npos)
				break;
			const size_t end = list.find_first_of(" \t", start);
			const std::string one = list.substr(start, end == std::string::npos ? std::string::npos : end - start);
			any = true;
			if (fieldpbr::NameMatches(one, name))
				return true;
			at = end == std::string::npos ? list.size() : end;
		}
		return !any;
	}

	int Models(const std::string& target, const ModelOptions& o)
	{
		std::error_code error;
		fs::path path(target);
		const bool folderMode = fs::is_directory(path, error);
		if (!folderMode && !fs::exists(path, error) && fs::exists(fs::path(target + ".png"), error))
			path = fs::path(target + ".png");
		if (!folderMode && !fs::exists(path, error))
		{
			fprintf(stderr, "%s: no such image or folder\n", target.c_str());
			return 1;
		}
		const fs::path folder = folderMode ? path : (path.has_parent_path() ? path.parent_path() : fs::path("."));
		const std::map<std::string, ModelFiles> scanned = ScanFolder(folder);
		std::vector<const ModelFiles*> jobs;
		const char* const name = o.battle ? "battle" : "model";
		if (folderMode)
		{
			//a field model only when it has maps; a battle texture always (its albedo as a DDS), the ones that match
			for (const auto& [key, m] : scanned)
				if (o.battle ? NameMatches(o.match, m.stem) : m.files.size() > 1 || (m.files.size() == 1 && m.files.count("") == 0))
					jobs.push_back(&m);
		}
		else
		{
			const auto it = scanned.find(Lower(ModelStem(path.filename().string())));
			if (it != scanned.end())
				jobs.push_back(&it->second);
		}
		printf("fieldprep %s: %s, %zu image(s)%s%s, BC7 quality %d, %d threads\n", name, folder.string().c_str(), jobs.size(),
			o.battle ? "" : " with PBR maps", o.battle && folderMode && !o.match.empty() ? (" matching " + o.match).c_str() : "", o.quality, o.threads);
		if (jobs.empty())
		{
			if (o.battle)
				printf("  nothing to do: no PNG image there%s\n", folderMode && !o.match.empty() ? " whose name matches" : "");
			else
				printf("  nothing to do: an image needs at least one map next to it (<image>_A, _N, _O, _R, _M, _E or _ORME .png)\n");
			return 0;
		}
		std::vector<Result> results(jobs.size());
		std::atomic<size_t> next{ 0 };
		std::mutex print;
		//one image: all threads on its blocks; a folder: one image per thread
		const int workers = folderMode ? std::max(1, std::min(o.threads, static_cast<int>(jobs.size()))) : 1;
		const int encodeThreads = folderMode && workers > 1 ? 1 : o.threads;
		const auto work = [&]
		{
			for (size_t i = next++; i < jobs.size(); i = next++)
			{
				results[i] = OneModel(folder, *jobs[i], o, encodeThreads);
				std::lock_guard<std::mutex> lock(print);
				fputs(results[i].text.c_str(), stdout);
				fflush(stdout);
			}
		};
		std::vector<std::thread> pool;
		for (int t = 1; t < workers; t++)
			pool.emplace_back(work);
		work();
		for (std::thread& t : pool)
			t.join();
		int made = 0, upToDate = 0, failed = 0, skipped = 0;
		std::string log;
		for (const Result& r : results)
		{
			made += r.made;
			upToDate += r.upToDate;
			failed += r.failed;
			skipped += r.skipped;
			log += r.text;
		}
		char summary[200];
		snprintf(summary, sizeof(summary), "%zu image(s): %d DDS made, %d up to date%s%s\n", jobs.size(), made, upToDate,
			failed ? (", " + std::to_string(failed) + " failed").c_str() : "", skipped ? (", " + std::to_string(skipped) + " skipped").c_str() : "");
		fputs(summary, stdout);
		if (folderMode)
		{
			std::ofstream file(folder / (std::string("fieldprep_") + name + ".txt"), std::ios::binary | std::ios::trunc);
			file << log << summary;
		}
		return failed;
	}

	int Battle(const std::string& target, ModelOptions o)
	{
		o.battle = true;
		return Models(target, o);
	}
}
