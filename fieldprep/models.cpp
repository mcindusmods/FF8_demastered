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
		//_E emissive, _ORME the four packed
		const char* const SUFFIXES[] = { "_ORME", "_AM", "_A", "_N", "_O", "_RC", "_RM", "_R", "_MC", "_MM", "_M", "_E" };

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
			if (!has(""))
			{
				r.skipped = true;
				r.text = "  " + m.stem + ": no " + m.stem + ".png next to its maps (the game places the image by it): skipped\n";
				return r;
			}
			const fs::path image = file("");
			Image base;
			std::string error, notes, made;
			if (!LoadRgba(image.string(), base, error))
			{
				r.failed = true;
				r.text = "  " + m.stem + ": ERROR " + image.filename().string() + ": " + error + "\n";
				return r;
			}
			const int width = base.width, height = base.height;
			const auto fail = [&](const std::string& what)
			{
				r.failed = true;
				r.text = "  " + m.stem + ": ERROR " + what + "\n";
				return r;
			};
			//the albedo
			{
				const char* from = has("_A") ? "_A" : has("_AM") ? "_AM" : "";
				const fs::path source = file(from), out = folder / (m.stem + ".dds");
				if (!o.force && UpToDate(out, { source, image }))
					r.upToDate++;
				else
				{
					Image albedo = base;
					if (*from != '\0' && !Load(source, width, height, albedo, notes, error))
						return fail(error);
					const std::vector<Image> levels = AlbedoLevels(albedo, o.coverage);
					if (!WriteLevels(out, levels, true, o, threads, error))
						return fail(error);
					bool cutouts = false;
					for (size_t i = 3; i < albedo.rgba.size() && !cutouts; i += 4)
						cutouts = albedo.rgba[i] < 255;
					char line[200];
					snprintf(line, sizeof(line), "albedo from %s (%zu levels%s)", source.filename().string().c_str(), levels.size(),
						cutouts && o.coverage > 0 ? ", cut-outs keep their coverage" : "");
					made += line;
					r.made++;
				}
			}
			//the normal map
			if (has("_N"))
			{
				const fs::path source = file("_N"), out = folder / (m.stem + "_N.dds");
				if (!o.force && UpToDate(out, { source, image }))
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
					if (!WriteLevels(out, NormalLevels(normal), false, o, threads, error))
						return fail(error);
					made += std::string(made.empty() ? "" : ", ") + "normal" + (o.flipGreen ? " (green flipped)" : "");
					r.made++;
				}
			}
			//AO, roughness, metallic, emissive
			const char* const channels[4][3] = { { "_O", nullptr, nullptr }, { "_R", "_RC", "_RM" }, { "_M", "_MC", "_MM" }, { "_E", nullptr, nullptr } };
			const char* chosen[4] = {};
			bool anySeparate = false;
			for (int k = 0; k < 4; k++)
				for (const char* s : channels[k])
					if (s != nullptr && chosen[k] == nullptr && has(s))
					{
						chosen[k] = s;
						anySeparate = true;
					}
			if (anySeparate || has("_ORME"))
			{
				std::vector<fs::path> sources{ image };
				if (anySeparate)
				{
					for (const char* s : chosen)
						if (s != nullptr)
							sources.push_back(file(s));
				}
				else
					sources.push_back(file("_ORME"));
				const fs::path out = folder / (m.stem + "_ORME.dds");
				if (!o.force && UpToDate(out, sources))
					r.upToDate++;
				else
				{
					Image orme;
					std::string which;
					if (anySeparate)
					{
						Image maps[4];
						const char* names[4] = { "AO", "roughness", "metallic", "emissive" };
						for (int k = 0; k < 4; k++)
						{
							if (chosen[k] == nullptr)
								continue;
							if (!Load(file(chosen[k]), width, height, maps[k], notes, error))
								return fail(error);
							which += std::string(which.empty() ? "" : " ") + names[k];
						}
						orme = PackOrme(width, height, chosen[0] ? &maps[0] : nullptr, chosen[1] ? &maps[1] : nullptr, chosen[2] ? &maps[2] : nullptr,
							chosen[3] ? &maps[3] : nullptr);
						if (has("_ORME"))
							notes += ", " + file("_ORME").filename().string() + " not used (the separate maps are)";
					}
					else
					{
						if (!Load(file("_ORME"), width, height, orme, notes, error))
							return fail(error);
						which = "packed";
					}
					if (!WriteLevels(out, DataLevels(orme), false, o, threads, error))
						return fail(error);
					made += std::string(made.empty() ? "" : ", ") + "ORME (" + which + ")";
					r.made++;
				}
			}
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

	std::vector<Image> AlbedoLevels(const Image& level0, const float coverage)
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
			levels.push_back(std::move(out));
		}
		return levels;
	}

	std::vector<Image> DataLevels(const Image& level0)
	{
		std::vector<Image> levels{ level0 };
		Floats f;
		f.width = level0.width;
		f.height = level0.height;
		f.v.assign(level0.rgba.begin(), level0.rgba.end());
		while (f.width > 1 || f.height > 1)
		{
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
			for (size_t i = 0; i < f.v.size(); i++)
				out.rgba[i] = Byte(f.v[i]);
			levels.push_back(std::move(out));
		}
		return levels;
	}

	std::vector<Image> NormalLevels(const Image& level0)
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
		while (f.width > 1 || f.height > 1)
		{
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
		if (folderMode)
		{
			for (const auto& [key, m] : scanned)
				if (m.files.size() > 1 || (m.files.size() == 1 && m.files.count("") == 0))
					jobs.push_back(&m);
		}
		else
		{
			const auto it = scanned.find(Lower(ModelStem(path.filename().string())));
			if (it != scanned.end())
				jobs.push_back(&it->second);
		}
		printf("fieldprep model: %s, %zu image(s) with PBR maps, BC7 quality %d, %d threads\n", folder.string().c_str(), jobs.size(), o.quality,
			o.threads);
		if (jobs.empty())
		{
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
			std::ofstream file(folder / "fieldprep_model.txt", std::ios::binary | std::ios::trunc);
			file << log << summary;
		}
		return failed;
	}
}
