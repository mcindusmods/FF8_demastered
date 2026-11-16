#include "world_vehicles.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace worldvehicles
{
	namespace
	{
		bool ReadU32(const uint8_t* data, const size_t size, const size_t offset, uint32_t& out)
		{
			if (offset > size || size - offset < 4)
				return false;
			std::memcpy(&out, data + offset, 4);
			return true;
		}

		bool ReadU16(const uint8_t* data, const size_t size, const size_t offset, uint16_t& out)
		{
			if (offset > size || size - offset < 2)
				return false;
			std::memcpy(&out, data + offset, 2);
			return true;
		}

		struct AxisTap
		{
			int index;
			float weight;
		};

		//Weights for one target coordinate: box filter when shrinking, linear when enlarging
		std::vector<AxisTap> AxisWeights(const int sourceLength, const int targetLength, const int targetIndex)
		{
			std::vector<AxisTap> taps;
			if (sourceLength >= targetLength)
			{
				const double start = static_cast<double>(targetIndex) * sourceLength / targetLength;
				const double end = static_cast<double>(targetIndex + 1) * sourceLength / targetLength;
				for (int k = static_cast<int>(std::floor(start)); k < static_cast<int>(std::ceil(end)); k++)
				{
					const double overlap = std::min(end, k + 1.0) - std::max(start, static_cast<double>(k));
					if (overlap > 0.0 && k >= 0 && k < sourceLength)
						taps.push_back({ k, static_cast<float>(overlap) });
				}
			}
			else
			{
				const double center = (targetIndex + 0.5) * sourceLength / targetLength - 0.5;
				const int k0 = static_cast<int>(std::floor(center));
				const float fraction = static_cast<float>(center - k0);
				taps.push_back({ std::clamp(k0, 0, sourceLength - 1), 1.0f - fraction });
				taps.push_back({ std::clamp(k0 + 1, 0, sourceLength - 1), fraction });
			}
			if (taps.empty())
				taps.push_back({ std::clamp(targetIndex * sourceLength / std::max(targetLength, 1), 0, sourceLength - 1), 1.0f });
			return taps;
		}
	}

	int Tim::PaletteCount() const
	{
		const int per = ColorsPerPalette();
		if (clut.empty())
			return 0;
		const int perLine = std::max(1, clutWidth / per);
		return perLine * clutHeight;
	}

	uint8_t Tim::IndexAt(const int texelX, const int texelY) const
	{
		const size_t row = static_cast<size_t>(texelY) * widthUnits * 2;
		if (bpp == 0)
		{
			const uint8_t b = image[row + texelX / 2];
			return (texelX & 1) ? static_cast<uint8_t>(b >> 4) : static_cast<uint8_t>(b & 15);
		}
		return image[row + texelX];
	}

	uint16_t Tim::Color(const int palette, const int index) const
	{
		const int per = ColorsPerPalette();
		const int perLine = std::max(1, clutWidth / per);
		const size_t base = static_cast<size_t>(palette / perLine) * clutWidth + static_cast<size_t>(palette % perLine) * per;
		const size_t at = base + index;
		return at < clut.size() ? clut[at] : 0;
	}

	bool ParseTim(const uint8_t* data, const size_t size, Tim& out, size_t* imageOffset)
	{
		uint32_t magic = 0, flags = 0;
		if (!ReadU32(data, size, 0, magic) || !ReadU32(data, size, 4, flags) || magic != 0x10)
			return false;
		Tim tim;
		tim.bpp = static_cast<int>(flags & 3);
		if (tim.bpp > 1)
			return false;
		size_t pos = 8;
		if (flags & 8)
		{
			uint32_t block = 0;
			uint16_t w = 0, h = 0;
			if (!ReadU32(data, size, pos, block) || !ReadU16(data, size, pos + 4, tim.clutX) || !ReadU16(data, size, pos + 6, tim.clutY)
				|| !ReadU16(data, size, pos + 8, w) || !ReadU16(data, size, pos + 10, h) || block < 12 || block > size - pos)
				return false;
			const size_t colors = (block - 12) / 2;
			const int per = tim.ColorsPerPalette();
			if (static_cast<size_t>(w) * h != colors && colors >= static_cast<size_t>(per))
			{
				w = static_cast<uint16_t>(per);
				h = static_cast<uint16_t>(colors / per);
			}
			tim.clutWidth = w;
			tim.clutHeight = h;
			tim.clut.resize(static_cast<size_t>(w) * h);
			if (tim.clut.size() > colors)
				return false;
			std::memcpy(tim.clut.data(), data + pos + 12, tim.clut.size() * 2);
			pos += block;
		}
		uint32_t block = 0;
		if (!ReadU32(data, size, pos, block) || !ReadU16(data, size, pos + 4, tim.x) || !ReadU16(data, size, pos + 6, tim.y)
			|| !ReadU16(data, size, pos + 8, tim.widthUnits) || !ReadU16(data, size, pos + 10, tim.height))
			return false;
		const size_t bytes = static_cast<size_t>(tim.widthUnits) * 2 * tim.height;
		if (bytes == 0 || pos + 12 > size || bytes > size - pos - 12)
			return false;
		tim.image.assign(data + pos + 12, data + pos + 12 + bytes);
		if (imageOffset != nullptr)
			*imageOffset = pos + 12;
		out = std::move(tim);
		return true;
	}

	std::vector<CharaOneTexture> ParseCharaOneTextures(const uint8_t* data, const size_t size)
	{
		std::vector<CharaOneTexture> result;
		if (size < 8)
			return result;
		size_t cur = size - 4;
		uint32_t count = 0;
		ReadU32(data, size, cur, count);
		for (uint32_t model = 0; model < count && cur >= 8; model++)
		{
			int texture = 0;
			while (cur >= 8)
			{
				cur -= 4;
				uint32_t value = 0;
				ReadU32(data, size, cur, value);
				if (static_cast<int32_t>(value) < 0)
					break;
				result.push_back({ static_cast<int>(model), texture++, value & 0x0FFFFFFF });
			}
			if (cur < 8)
				break;
			cur -= 4; //offset of the model data
		}
		return result;
	}


	bool AtlasOf(const Tim& tim, std::string& name, int& texelX, int& texelY)
	{
		const int page = tim.x / 64 + 16 * (tim.y / 256), row = tim.y / 256;
		if (tim.widthUnits == 0 || tim.height == 0 || (tim.y % 256) + tim.height > 256)
			return false;
		if (tim.bpp == 0)
		{
			if ((tim.x % 64) + tim.widthUnits > 64)
				return false;
			name = "world\\dat\\wmset\\atlas\\page" + std::to_string(page);
			texelX = (tim.x % 64) * 4;
		}
		else
		{
			//8-bit pages are 128 units wide: the one on a 128-unit boundary, else the one starting at the texture's page
			int first = (tim.x / 128) * 2;
			if (tim.x + tim.widthUnits > first * 64 + 128)
				first = tim.x / 64;
			if (tim.x + tim.widthUnits > first * 64 + 128 || first > 15)
				return false;
			name = "world\\dat\\wmset\\atlas\\page" + std::to_string(first + 16 * row) + "_8bit";
			texelX = (tim.x - first * 64) * 2;
		}
		texelY = tim.y % 256;
		return true;
	}

	std::vector<NamedTexture> ParseWmsetTextures(const uint8_t* data, const size_t size)
	{
		std::vector<NamedTexture> out;
		constexpr int SECTIONS = 48;
		if (size < SECTIONS * 4)
			return out;
		uint32_t offsets[SECTIONS]{};
		for (int i = 0; i < SECTIONS; i++)
			ReadU32(data, size, static_cast<size_t>(i) * 4, offsets[i]);
		auto sectionEnd = [&](const int i) {
			uint32_t end = static_cast<uint32_t>(size);
			for (const uint32_t o : offsets)
				if (o > offsets[i] && o < end)
					end = o;
			return end;
		};
		//TIMs: sections 38, 39, 40 and 42 (index 37, 38, 39, 41), each starts with a table of offsets
		for (const int index : { 37, 38, 39, 41 })
		{
			const uint32_t start = offsets[index], end = sectionEnd(index);
			if (start < SECTIONS * 4 || start >= end)
				continue;
			for (uint32_t k = 0; start + 4 * k + 4 <= end; k++)
			{
				uint32_t rel = 0;
				ReadU32(data, size, start + 4 * k, rel);
				if (rel == 0 || rel >= end - start)
					break;
				NamedTexture texture;
				size_t imageOffset = 0;
				if (!ParseTim(data + start + rel, end - start - rel, texture.tim, &imageOffset))
					break;
				texture.name = "world\\dat\\wmset\\section" + std::to_string(index + 1) + "\\texture" + std::to_string(k);
				texture.imageOffset = start + rel + imageOffset;
				//effects never share a VRAM spot, so they can live in page atlases. Not section 38's textures 0-7 and
				//section 40 (far terrain: section 40 replaces texture 2 later in the story) and not vehicles (section 42,
				//several share each spot).
				if ((index == 37 && k >= 8) || index == 38)
					AtlasOf(texture.tim, texture.atlas, texture.atlasX, texture.atlasY);
				out.push_back(std::move(texture));
			}
		}
		//section 41 (index 40): colour animations. Entry: u8 id, u8 speed, u8 frames, u8 -, u16 clut x, u16 clut y,
		//u16 -, u16 -, u32 frame offsets (from entry + 12); frame: 8-byte header, u32 length, u16 x, y, w, h, colours
		const uint32_t start = offsets[40], end = sectionEnd(40);
		for (uint32_t k = 0; start >= SECTIONS * 4 && start + 4 * k + 4 <= end; k++)
		{
			uint32_t rel = 0;
			ReadU32(data, size, start + 4 * k, rel);
			if (rel == 0 || rel >= end - start)
				break;
			const size_t entry = start + rel;
			uint16_t clutX = 0, clutY = 0;
			if (entry + 12 > end || !ReadU16(data, size, entry + 4, clutX) || !ReadU16(data, size, entry + 6, clutY))
				break;
			const int frameCount = data[entry + 2];
			std::vector<std::vector<uint16_t>> frames;
			for (int f = 0; f < frameCount; f++)
			{
				uint32_t frameOffset = 0;
				uint16_t w = 0, h = 0;
				if (!ReadU32(data, size, entry + 12 + 4 * static_cast<size_t>(f), frameOffset))
					break;
				const size_t frame = entry + 12 + frameOffset;
				if (!ReadU16(data, size, frame + 16, w) || !ReadU16(data, size, frame + 18, h))
					break;
				const size_t colours = static_cast<size_t>(w) * h;
				if (colours == 0 || colours > 4096 || frame + 20 + colours * 2 > end)
					break;
				std::vector<uint16_t> clut(colours);
				std::memcpy(clut.data(), data + frame + 20, colours * 2);
				frames.push_back(std::move(clut));
			}
			for (NamedTexture& texture : out)
				if (texture.tim.clutX == clutX && texture.tim.clutY == clutY && !frames.empty()
					&& frames[0].size() >= static_cast<size_t>(texture.tim.ColorsPerPalette()))
					texture.frames = frames;
		}
		return out;
	}

	std::vector<NamedTexture> ParseCharaOne(const uint8_t* data, const size_t size)
	{
		std::vector<NamedTexture> out;
		for (const CharaOneTexture& entry : ParseCharaOneTextures(data, size))
		{
			if (entry.timOffset >= size)
				continue;
			NamedTexture texture;
			size_t imageOffset = 0;
			if (!ParseTim(data + entry.timOffset, size - entry.timOffset, texture.tim, &imageOffset))
				continue;
			texture.name = "world\\esk\\chara_one\\model" + std::to_string(entry.model) + "-" + std::to_string(entry.texture);
			texture.strip = "world\\esk\\chara_one\\model" + std::to_string(entry.model);
			texture.imageOffset = entry.timOffset + imageOffset;
			out.push_back(std::move(texture));
		}
		//strip: the model's textures left to right in file order
		for (NamedTexture& texture : out)
		{
			int x = 0;
			for (const NamedTexture& other : out)
			{
				if (other.strip != texture.strip)
					continue;
				if (&other == &texture)
					texture.sliceX = x;
				x += other.tim.Width();
				texture.stripHeight = std::max<int>(texture.stripHeight, other.tim.height);
			}
			texture.stripWidth = x;
		}
		return out;
	}

	void VramTracker::Record(std::shared_ptr<const NamedTexture> texture, const int x, const int y)
	{
		if (!texture)
			return;
		const int w = texture->tim.widthUnits, h = texture->tim.height;
		std::lock_guard<std::mutex> lock(mutex);
		textures.erase(std::remove_if(textures.begin(), textures.end(), [&](const VramTexture& old) {
			return old.x >= x && old.x + old.WidthUnits() <= x + w && old.y >= y && old.y + old.Height() <= y + h;
		}), textures.end());
		if (textures.size() >= 4096)
			textures.erase(textures.begin());
		textures.push_back({ std::move(texture), x, y, nextSerial++ });
	}

	std::vector<VramTexture> VramTracker::Snapshot() const
	{
		std::lock_guard<std::mutex> lock(mutex);
		return textures;
	}

	std::vector<VramTexture> VramTracker::Snapshot(const int x, const int y, const int widthUnits, const int height,
		uint64_t* generation) const
	{
		std::vector<VramTexture> out;
		std::lock_guard<std::mutex> lock(mutex);
		for (const VramTexture& t : textures)
			if (t.x < x + widthUnits && x < t.x + t.WidthUnits() && t.y < y + height && y < t.y + t.Height())
				out.push_back(t);
		if (generation != nullptr)
			*generation = nextSerial;
		return out;
	}

	void VramTracker::Clear()
	{
		std::lock_guard<std::mutex> lock(mutex);
		textures.clear();
		nextSerial++;
	}

	MatchReport MatchPage(const uint8_t* pageBgra, const int width, const int height, const int pageX, const int pageY,
		const std::vector<VramTexture>& textures, const double minAgreement)
	{
		MatchReport report;
		if (pageBgra == nullptr || width < 256 || height < 256 || width % 256 != 0 || height % 256 != 0)
			return report;
		const int scale = width / 256;
		struct Cover
		{
			int x0, y0, x1, y1;
		};
		std::vector<Cover> covers;
		uint16_t candidate[256];
		uint32_t votes[256], agreeing[256];
		for (size_t i = 0; i < textures.size(); i++)
		{
			const VramTexture& texture = textures[i];
			const Tim& tim = texture.GetTim();
			const int unitsPerPage = tim.bpp == 0 ? 64 : 128;
			if (tim.image.empty() || texture.x < pageX || texture.y < pageY
				|| texture.x + texture.WidthUnits() > pageX + unitsPerPage || texture.y + texture.Height() > pageY + 256)
				continue;
			const int tpu = tim.TexelsPerUnit();
			PageMatch match;
			match.textureIndex = i;
			match.texelX = (texture.x - pageX) * tpu;
			match.texelY = texture.y - pageY;
			match.texelWidth = tim.Width();
			match.texelHeight = tim.height;
			if (match.texelY + match.texelHeight > height / scale)
				continue;

			//texels hidden by newer textures
			covers.clear();
			for (size_t j = i + 1; j < textures.size(); j++)
			{
				const VramTexture& o = textures[j];
				const int x0 = std::max(texture.x, o.x), x1 = std::min(texture.x + texture.WidthUnits(), o.x + o.WidthUnits());
				const int y0 = std::max(texture.y, o.y), y1 = std::min(texture.y + texture.Height(), o.y + o.Height());
				if (x0 < x1 && y0 < y1)
					covers.push_back({ (x0 - texture.x) * tpu, y0 - texture.y, (x1 - texture.x) * tpu, y1 - texture.y });
			}
			auto hidden = [&](const int tx, const int ty) {
				for (const Cover& c : covers)
					if (tx >= c.x0 && tx < c.x1 && ty >= c.y0 && ty < c.y1)
						return true;
				return false;
			};

			//(index -> page colour) the way each colour index is drawn: majority vote, then count who agrees.
			//Colour: 5-bit RGB, bit 15 = transparent.
			const int colours = tim.ColorsPerPalette();
			std::fill(candidate, candidate + colours, static_cast<uint16_t>(0));
			std::fill(votes, votes + colours, 0u);
			std::fill(agreeing, agreeing + colours, 0u);
			const uint8_t* image = tim.image.data();
			const size_t rowBytes = static_cast<size_t>(tim.widthUnits) * 2;
			auto indexAt = [&](const int tx, const int ty) -> int {
				if (tim.bpp == 0)
				{
					const uint8_t b = image[ty * rowBytes + tx / 2];
					return (tx & 1) ? b >> 4 : b & 15;
				}
				return image[ty * rowBytes + tx];
			};
			auto colourAt = [&](const int tx, const int ty) -> uint16_t {
				const uint8_t* p = pageBgra + (static_cast<size_t>(match.texelY + ty) * scale * width
					+ static_cast<size_t>(match.texelX + tx) * scale) * 4;
				return static_cast<uint16_t>((p[2] >> 3) | ((p[1] >> 3) << 5) | ((p[0] >> 3) << 10) | (p[3] == 0 ? 0x8000 : 0));
			};
			//big textures: every second texel each way is plenty of evidence and 4 times less work
			const int step = static_cast<size_t>(match.texelWidth) * match.texelHeight > 8192 ? 2 : 1;
			size_t total = 0;
			for (int ty = 0; ty < match.texelHeight; ty += step)
				for (int tx = 0; tx < match.texelWidth; tx += step)
				{
					if (!covers.empty() && hidden(tx, ty))
						continue;
					const int index = indexAt(tx, ty);
					const uint16_t colour = colourAt(tx, ty);
					if (votes[index] == 0)
					{
						candidate[index] = colour;
						votes[index] = 1;
					}
					else if (candidate[index] == colour)
						votes[index]++;
					else
						votes[index]--;
					total++;
				}
			//not enough of it left to tell
			if (total < 16 || total * 4 * step * step < static_cast<size_t>(match.texelWidth) * match.texelHeight)
				continue;
			std::fill(votes, votes + colours, 0u); //reused: 1 = index used
			size_t agree = 0;
			for (int ty = 0; ty < match.texelHeight; ty += step)
				for (int tx = 0; tx < match.texelWidth; tx += step)
				{
					if (!covers.empty() && hidden(tx, ty))
						continue;
					const int index = indexAt(tx, ty);
					votes[index] = 1;
					if (colourAt(tx, ty) == candidate[index])
					{
						agreeing[index]++;
						agree++;
					}
				}
			match.agreement = static_cast<double>(agree) / static_cast<double>(total);

			//one flat colour proves nothing (cleared VRAM, or a palette that is all one colour)
			int used = 0;
			uint16_t first = 0;
			bool distinct = false;
			for (int index = 0; index < colours; index++)
				if (votes[index])
				{
					if (used++ == 0)
						first = candidate[index];
					else if (candidate[index] != first)
						distinct = true;
				}
			const bool flat = used > 1 && !distinct;

			//which colour table this page texture was made with: animation frames first (frame 0 is the texture's own)
			auto sameAs = [&](const uint16_t* table, const size_t count) {
				for (int index = 0; index < colours; index++)
					if (votes[index] && (index >= static_cast<int>(count) || (candidate[index] & 0x7FFF) != (table[index] & 0x7FFF)))
						return false;
				return true;
			};
			const auto& frames = texture.texture->frames;
			for (size_t f = 0; f < frames.size() && match.frame < 0; f++)
				if (sameAs(frames[f].data(), frames[f].size()))
					match.frame = static_cast<int>(f);
			for (int palette = 0; palette < tim.PaletteCount() && match.palette < 0 && match.frame < 0; palette++)
			{
				const int per = tim.ColorsPerPalette();
				const int perLine = std::max(1, tim.clutWidth / per);
				const size_t base = static_cast<size_t>(palette / perLine) * tim.clutWidth + static_cast<size_t>(palette % perLine) * per;
				if (base + per <= tim.clut.size() && sameAs(tim.clut.data() + base, per))
					match.palette = palette;
			}

			if (!flat && match.agreement >= minAgreement)
				report.matches.push_back(match);
			else if (!flat && match.agreement >= 0.5)
				report.nearMisses.push_back(match);
		}
		return report;
	}

	void ResampleInto(const RgbaImage& source, RgbaImage& target, const int x, const int y, const int w, const int h)
	{
		if (!source.IsValid() || !target.IsValid() || w <= 0 || h <= 0)
			return;
		std::vector<std::vector<AxisTap>> columns(w), rows(h);
		for (int i = 0; i < w; i++)
			columns[i] = AxisWeights(source.width, w, i);
		for (int j = 0; j < h; j++)
			rows[j] = AxisWeights(source.height, h, j);
		for (int j = 0; j < h; j++)
		{
			const int ty = y + j;
			if (ty < 0 || ty >= target.height)
				continue;
			for (int i = 0; i < w; i++)
			{
				const int tx = x + i;
				if (tx < 0 || tx >= target.width)
					continue;
				double weightSum = 0.0, alphaSum = 0.0, r = 0.0, g = 0.0, b = 0.0;
				for (const AxisTap& row : rows[j])
					for (const AxisTap& column : columns[i])
					{
						const double weight = static_cast<double>(row.weight) * column.weight;
						const uint8_t* s = source.At(column.index, row.index);
						const double a = s[3] / 255.0;
						weightSum += weight;
						alphaSum += weight * a;
						r += weight * a * s[0];
						g += weight * a * s[1];
						b += weight * a * s[2];
					}
				uint8_t* d = target.At(tx, ty);
				if (weightSum <= 0.0)
					continue;
				d[3] = static_cast<uint8_t>(std::lround(std::clamp(alphaSum / weightSum * 255.0, 0.0, 255.0)));
				if (alphaSum > 0.0)
				{
					d[0] = static_cast<uint8_t>(std::lround(std::clamp(r / alphaSum, 0.0, 255.0)));
					d[1] = static_cast<uint8_t>(std::lround(std::clamp(g / alphaSum, 0.0, 255.0)));
					d[2] = static_cast<uint8_t>(std::lround(std::clamp(b / alphaSum, 0.0, 255.0)));
				}
				else
					d[0] = d[1] = d[2] = 0;
			}
		}
	}

	RgbaImage Crop(const RgbaImage& source, const int x, const int y, const int w, const int h)
	{
		RgbaImage out;
		const int x0 = std::clamp(x, 0, source.width), y0 = std::clamp(y, 0, source.height);
		const int x1 = std::clamp(x + w, 0, source.width), y1 = std::clamp(y + h, 0, source.height);
		if (!source.IsValid() || x1 <= x0 || y1 <= y0)
			return out;
		out.width = x1 - x0;
		out.height = y1 - y0;
		out.pixels.resize(static_cast<size_t>(out.width) * out.height * 4);
		for (int row = 0; row < out.height; row++)
			std::memcpy(out.At(0, row), source.At(x0, y0 + row), static_cast<size_t>(out.width) * 4);
		return out;
	}

	RgbaImage ComposePage(const uint8_t* pageBgra, const int width, const int height, const int scale, const RgbaImage* base,
		const std::vector<Placement>& placements)
	{
		RgbaImage canvas;
		if (width <= 0 || height <= 0 || scale <= 0 || width % 256 != 0)
			return canvas;
		const int pageScale = width / 256; //the game's own page size, normally 1
		canvas.width = 256 * scale;
		canvas.height = height / pageScale * scale;
		canvas.pixels.assign(static_cast<size_t>(canvas.width) * canvas.height * 4, 0);
		if (base != nullptr && base->IsValid())
			ResampleInto(*base, canvas, 0, 0, canvas.width, canvas.height);
		else if (pageBgra != nullptr)
		{
			for (int y = 0; y < canvas.height; y++)
			{
				const int sy = y * pageScale / scale;
				uint8_t* d = canvas.At(0, y);
				for (int x = 0; x < canvas.width; x++, d += 4)
				{
					const uint8_t* s = pageBgra + (static_cast<size_t>(sy) * width + static_cast<size_t>(x) * pageScale / scale) * 4;
					d[0] = s[2];
					d[1] = s[1];
					d[2] = s[0];
					d[3] = s[3];
				}
			}
		}
		for (const Placement& placement : placements)
		{
			if (placement.image == nullptr || !placement.image->IsValid())
				continue;
			//replace the rectangle: transparent parts of the HD file do not show the old texels underneath
			for (int y = placement.texelY * scale; y < (placement.texelY + placement.texelHeight) * scale && y < canvas.height; y++)
				for (int x = placement.texelX * scale; x < (placement.texelX + placement.texelWidth) * scale && x < canvas.width; x++)
					std::memset(canvas.At(x, y), 0, 4);
			ResampleInto(*placement.image, canvas, placement.texelX * scale, placement.texelY * scale,
				placement.texelWidth * scale, placement.texelHeight * scale);
		}
		return canvas;
	}

	std::string TwinName(const std::string& name)
	{
		static const char* const pairs[][2] = {
			{ "world\\dat\\wmset\\section42\\texture1", "world\\dat\\wmset\\section42\\texture0" },
			{ "world\\dat\\wmset\\section42\\texture16", "world\\dat\\wmset\\section42\\texture15" },
			{ "world\\dat\\wmset\\section42\\texture32", "world\\dat\\wmset\\section42\\texture29" },
			{ "world\\esk\\chara_one\\model3", "world\\esk\\chara_one\\model2" },
		};
		for (const auto& pair : pairs)
		{
			if (name == pair[0])
				return pair[1];
			if (name == pair[1])
				return pair[0];
		}
		return {};
	}
}
