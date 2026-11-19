#include "lut_grade.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace lutgrade
{
	namespace
	{
		const char* const NAMES[SLOT_COUNT] = { "day", "dusk", "night", "dawn", "lunar_day", "lunar_dusk", "lunar_night", "lunar_dawn" };

		uint16_t ToShort(const double v)
		{
			return static_cast<uint16_t>(std::lround(std::clamp(v, 0.0, 1.0) * 65535.0));
		}

		double Smooth(const double x)
		{
			const double t = std::clamp(x, 0.0, 1.0);
			return t * t * (3.0 - 2.0 * t);
		}

		double Wrap(const double time, const double length)
		{
			if (!std::isfinite(time) || length <= 0)
				return 0;
			double t = std::fmod(time, length);
			if (t < 0)
				t += length;
			return t >= length ? 0 : t;
		}

		//the share of the way from `from` to `to` (1 when there is no way)
		double Along(const double t, const double from, const double to)
		{
			return to > from ? Smooth((t - from) / (to - from)) : 1.0;
		}

		//one square (blue's step `slice`) read bilinearly at red r, green g (both in points: 0 to size - 1)
		void ReadSquare(const Table& table, const int slice, const double r, const double g, double out[3])
		{
			const int n = table.size, width = n * n;
			const int r0 = std::clamp(static_cast<int>(std::floor(r)), 0, n - 1), g0 = std::clamp(static_cast<int>(std::floor(g)), 0, n - 1);
			const int r1 = std::min(r0 + 1, n - 1), g1 = std::min(g0 + 1, n - 1);
			const double fr = std::clamp(r - r0, 0.0, 1.0), fg = std::clamp(g - g0, 0.0, 1.0);
			const auto at = [&](const int rr, const int gg, const int c)
			{
				return table.rgba[(static_cast<size_t>(gg) * width + static_cast<size_t>(slice) * n + rr) * 4 + c] / 65535.0;
			};
			for (int c = 0; c < 3; c++)
			{
				const double top = at(r0, g0, c) * (1 - fr) + at(r1, g0, c) * fr;
				const double bottom = at(r0, g1, c) * (1 - fr) + at(r1, g1, c) * fr;
				out[c] = top * (1 - fg) + bottom * fg;
			}
		}

		std::string Trim(const std::string& s)
		{
			size_t a = 0, b = s.size();
			while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r'))
				a++;
			while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
				b--;
			return s.substr(a, b - a);
		}

		bool ThreeNumbers(const std::string& text, double out[3])
		{
			const char* p = text.c_str();
			for (int i = 0; i < 3; i++)
			{
				char* end = nullptr;
				out[i] = std::strtod(p, &end);
				if (end == p || !std::isfinite(out[i]))
					return false;
				p = end;
			}
			while (*p == ' ' || *p == '\t')
				p++;
			return *p == 0;
		}
	}

	const char* SlotName(const int slot)
	{
		return slot >= 0 && slot < SLOT_COUNT ? NAMES[slot] : "?";
	}

	Table Identity(const int size)
	{
		Table t;
		t.size = std::clamp(size, MIN_SIZE, MAX_SIZE);
		const int n = t.size, width = n * n;
		t.rgba.resize(static_cast<size_t>(width) * n * 4);
		for (int g = 0; g < n; g++)
			for (int b = 0; b < n; b++)
				for (int r = 0; r < n; r++)
				{
					uint16_t* p = &t.rgba[(static_cast<size_t>(g) * width + static_cast<size_t>(b) * n + r) * 4];
					p[0] = ToShort(r / (n - 1.0));
					p[1] = ToShort(g / (n - 1.0));
					p[2] = ToShort(b / (n - 1.0));
					p[3] = 65535;
				}
		return t;
	}

	bool IsIdentity(const Table& table, const double tolerance)
	{
		if (!table.Valid())
			return false;
		const int n = table.size, width = n * n;
		const double most = tolerance * 65535.0;
		for (int g = 0; g < n; g++)
			for (int b = 0; b < n; b++)
				for (int r = 0; r < n; r++)
				{
					const uint16_t* p = &table.rgba[(static_cast<size_t>(g) * width + static_cast<size_t>(b) * n + r) * 4];
					const int index[3] = { r, g, b };
					for (int c = 0; c < 3; c++)
						if (std::fabs(p[c] - index[c] / (n - 1.0) * 65535.0) > most)
							return false;
				}
		return true;
	}

	bool FromStrip(const uint8_t* rgba, const int width, const int height, Table& table, std::string& why)
	{
		if (rgba == nullptr || width <= 0 || height <= 0)
		{
			why = "no picture";
			return false;
		}
		const bool across = width == height * height, down = height == width * width;
		const int n = across ? height : width;
		if ((!across && !down) || n < MIN_SIZE || n > MAX_SIZE)
		{
			char text[200];
			snprintf(text, sizeof(text), "%d x %d is not a LUT strip (n squares of n x n side by side: n x n wide and n high, such as "
				"1024 x 32; or one under the other), n %d-%d", width, height, MIN_SIZE, MAX_SIZE);
			why = text;
			return false;
		}
		Table t;
		t.size = n;
		t.rgba.resize(static_cast<size_t>(n) * n * n * 4);
		for (int g = 0; g < n; g++)
			for (int b = 0; b < n; b++)
				for (int r = 0; r < n; r++)
				{
					const size_t x = across ? static_cast<size_t>(b) * n + r : static_cast<size_t>(r);
					const size_t y = across ? static_cast<size_t>(g) : static_cast<size_t>(b) * n + g;
					const uint8_t* from = rgba + (y * width + x) * 4;
					uint16_t* to = &t.rgba[(static_cast<size_t>(g) * n * n + static_cast<size_t>(b) * n + r) * 4];
					for (int c = 0; c < 3; c++)
						to[c] = static_cast<uint16_t>(from[c] * 257);
					to[3] = 65535;
				}
		table = std::move(t);
		return true;
	}

	bool FromCube(const std::string& text, Table& table, std::string& why)
	{
		std::istringstream in(text);
		std::string line;
		int size = 0, lineNumber = 0;
		double domainMin[3] = { 0, 0, 0 }, domainMax[3] = { 1, 1, 1 };
		std::vector<double> values;
		const auto fail = [&](const std::string& what)
		{
			why = "line " + std::to_string(lineNumber) + ": " + what;
			return false;
		};
		while (std::getline(in, line))
		{
			lineNumber++;
			line = Trim(line);
			if (line.empty() || line[0] == '#')
				continue;
			const size_t space = line.find_first_of(" \t");
			const std::string word = line.substr(0, space), rest = space == std::string::npos ? std::string() : Trim(line.substr(space));
			if (word == "TITLE")
				continue;
			if (word == "LUT_1D_SIZE")
				return fail("a 1D LUT (LUT_1D_SIZE): only 3D ones are graded with");
			if (word == "LUT_3D_SIZE")
			{
				size = std::atoi(rest.c_str());
				if (size < MIN_SIZE || size > MAX_SIZE)
					return fail("LUT_3D_SIZE " + rest + ": " + std::to_string(MIN_SIZE) + "-" + std::to_string(MAX_SIZE) + " are read");
				values.reserve(static_cast<size_t>(size) * size * size * 3);
				continue;
			}
			if (word == "DOMAIN_MIN" || word == "DOMAIN_MAX")
			{
				if (!ThreeNumbers(rest, word == "DOMAIN_MIN" ? domainMin : domainMax))
					return fail(word + " needs three numbers");
				continue;
			}
			if (word == "LUT_3D_INPUT_RANGE" || word == "LUT_1D_INPUT_RANGE")
			{
				char* end = nullptr;
				const double low = std::strtod(rest.c_str(), &end), high = std::strtod(end, nullptr);
				for (int c = 0; c < 3; c++)
				{
					domainMin[c] = low;
					domainMax[c] = high;
				}
				continue;
			}
			double v[3];
			if (!ThreeNumbers(line, v))
			{
				if ((line[0] >= 'A' && line[0] <= 'Z') || (line[0] >= 'a' && line[0] <= 'z'))
					continue; //a keyword of another program's
				return fail("not three numbers: " + line.substr(0, 40));
			}
			if (size == 0)
				return fail("the table starts before LUT_3D_SIZE");
			values.insert(values.end(), v, v + 3);
		}
		if (size == 0)
		{
			why = "no LUT_3D_SIZE";
			return false;
		}
		const size_t want = static_cast<size_t>(size) * size * size * 3;
		if (values.size() != want)
		{
			why = std::to_string(values.size() / 3) + " points, LUT_3D_SIZE " + std::to_string(size) + " needs " + std::to_string(want / 3);
			return false;
		}
		for (int c = 0; c < 3; c++)
			if (!(domainMax[c] > domainMin[c]))
			{
				why = "DOMAIN_MAX is not above DOMAIN_MIN";
				return false;
			}
		//the file's order: red fastest, then green, then blue
		Table t;
		t.size = size;
		t.rgba.resize(static_cast<size_t>(size) * size * size * 4);
		const int width = size * size;
		for (int b = 0; b < size; b++)
			for (int g = 0; g < size; g++)
				for (int r = 0; r < size; r++)
				{
					const double* v = &values[((static_cast<size_t>(b) * size + g) * size + r) * 3];
					uint16_t* to = &t.rgba[(static_cast<size_t>(g) * width + static_cast<size_t>(b) * size + r) * 4];
					for (int c = 0; c < 3; c++)
						to[c] = ToShort(v[c]);
					to[3] = 65535;
				}
		const bool plainDomain = domainMin[0] == 0 && domainMin[1] == 0 && domainMin[2] == 0 && domainMax[0] == 1 && domainMax[1] == 1
			&& domainMax[2] == 1;
		if (!plainDomain)
		{
			//the table is for colours from DOMAIN_MIN to DOMAIN_MAX: read again at 0-1
			Table remapped = t;
			for (int g = 0; g < size; g++)
				for (int b = 0; b < size; b++)
					for (int r = 0; r < size; r++)
					{
						const int index[3] = { r, g, b };
						float in[3], out[3];
						for (int c = 0; c < 3; c++)
							in[c] = static_cast<float>(std::clamp((index[c] / (size - 1.0) - domainMin[c]) / (domainMax[c] - domainMin[c]), 0.0, 1.0));
						Apply(t, in, out);
						uint16_t* to = &remapped.rgba[(static_cast<size_t>(g) * width + static_cast<size_t>(b) * size + r) * 4];
						for (int c = 0; c < 3; c++)
							to[c] = ToShort(out[c]);
					}
			t = std::move(remapped);
		}
		table = std::move(t);
		return true;
	}

	void Apply(const Table& table, const float in[3], float out[3])
	{
		if (!table.Valid())
		{
			for (int c = 0; c < 3; c++)
				out[c] = in[c];
			return;
		}
		const int n = table.size;
		const double r = std::clamp(static_cast<double>(in[0]), 0.0, 1.0) * (n - 1), g = std::clamp(static_cast<double>(in[1]), 0.0, 1.0) * (n - 1);
		const double b = std::clamp(static_cast<double>(in[2]), 0.0, 1.0) * (n - 1);
		const int slice = std::min(static_cast<int>(std::floor(b)), n - 1), next = std::min(slice + 1, n - 1);
		const double f = b - slice;
		double low[3], high[3];
		ReadSquare(table, slice, r, g, low);
		ReadSquare(table, next, r, g, high);
		for (int c = 0; c < 3; c++)
			out[c] = static_cast<float>(low[c] * (1 - f) + high[c] * f);
	}

	void Apply(const Table& a, const Table& b, const double mix, const double strength, const float in[3], float out[3])
	{
		float fromA[3], fromB[3];
		Apply(a, in, fromA);
		Apply(b, in, fromB);
		const double s = std::clamp(strength, 0.0, 1.0), m = std::clamp(mix, 0.0, 1.0);
		for (int c = 0; c < 3; c++)
		{
			const double graded = fromA[c] * (1 - m) + fromB[c] * m;
			out[c] = static_cast<float>(in[c] * (1 - s) + graded * s);
		}
	}

	Blend At(const fieldpbr::DayNightCycle& cycle, const double time)
	{
		const fieldpbr::DayNightCycle c = cycle.Checked();
		const double length = c.Length(), t = Wrap(time, length), half = c.fade / 2, along = 1.5 * c.fade;
		const double duskFrom = std::max(0.0, std::min(c.day * std::clamp(c.evening, 0.0, 1.0), c.day - half));
		const double middleOfNight = c.day + c.night / 2;
		const double nightFrom = std::min(c.day + along, middleOfNight), nightTo = std::max(length - along, middleOfNight);
		const double dayFrom = std::min(std::max(along, 0.1 * c.day), duskFrom / 2);
		Blend blend;
		if (t < dayFrom)
			blend = { DAWN, DAY, Along(t, 0, dayFrom) };
		else if (t < duskFrom)
			blend = { DAY, DAY, 0 };
		else if (t < c.day)
			blend = { DAY, DUSK, Along(t, duskFrom, c.day) };
		else if (t < nightFrom)
			blend = { DUSK, NIGHT, Along(t, c.day, nightFrom) };
		else if (t < nightTo)
			blend = { NIGHT, NIGHT, 0 };
		else
			blend = { NIGHT, DAWN, Along(t, nightTo, length) };
		return blend;
	}

	Blend Fixed(const bool night)
	{
		return night ? Blend{ NIGHT, NIGHT, 0 } : Blend{ DAY, DAY, 0 };
	}

	Blend Lunar(const Blend& blend)
	{
		Blend b = blend;
		if (b.from < LUNAR_DAY)
			b.from += LUNAR_DAY;
		if (b.to < LUNAR_DAY)
			b.to += LUNAR_DAY;
		return b;
	}

	std::string BlendText(const Blend& blend)
	{
		if (blend.from == blend.to || blend.mix <= 0.0)
			return SlotName(blend.from);
		if (blend.mix >= 1.0)
			return SlotName(blend.to);
		char text[96];
		snprintf(text, sizeof(text), "%s %d%% into %s", SlotName(blend.from), static_cast<int>(std::lround(blend.mix * 100)), SlotName(blend.to));
		return text;
	}
}
