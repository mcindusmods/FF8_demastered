#include "field_layers.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <tuple>

namespace fieldlayers
{
	namespace
	{
		uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
		void Put16(uint8_t* p, const uint16_t v)
		{
			p[0] = static_cast<uint8_t>(v & 0xFF);
			p[1] = static_cast<uint8_t>(v >> 8);
		}
	}

	std::vector<Tile> ParseMap(const uint8_t* data, const size_t size)
	{
		std::vector<Tile> tiles;
		if (data == nullptr)
			return tiles;
		for (size_t offset = 0; offset + TILE_BYTES <= size; offset += TILE_BYTES)
		{
			const uint8_t* p = data + offset;
			Tile t;
			t.x = static_cast<int16_t>(U16(p));
			if (t.x == 0x7FFF)
				break;
			t.y = static_cast<int16_t>(U16(p + 2));
			t.z = U16(p + 4);
			t.texId = U16(p + 6);
			t.palId = U16(p + 8);
			t.srcX = p[10];
			t.srcY = p[11];
			t.layer = p[12];
			t.blend = p[13];
			t.animId = p[14];
			t.animState = p[15];
			tiles.push_back(t);
		}
		return tiles;
	}

	void StoreSource(uint8_t* tileBytes, const int texturePage, const int palette, const int srcX, const int srcY)
	{
		const uint16_t texId = static_cast<uint16_t>((U16(tileBytes + 6) & ~0xFu) | (texturePage & 0xF));
		const uint16_t palId = static_cast<uint16_t>((U16(tileBytes + 8) & ~(0xFu << 6)) | ((palette & 0xF) << 6));
		Put16(tileBytes + 6, texId);
		Put16(tileBytes + 8, palId);
		tileBytes[10] = static_cast<uint8_t>(srcX);
		tileBytes[11] = static_cast<uint8_t>(srcY);
	}

	uint32_t BasePupu(const Tile& tile, const int x, const int y)
	{
		//OpenVIII PupuID(tile): layer << 24 | blend << 20 | animation id << 12 | animation state << 4, plus bit 31 / 23
		//for a tile off the 16 grid in x / y
		uint32_t raw = (static_cast<uint32_t>(tile.LayerId()) & 0x7Fu) << 24
			| (static_cast<uint32_t>(tile.blend) & 0x7u) << 20
			| static_cast<uint32_t>(tile.animId) << 12
			| static_cast<uint32_t>(tile.animState) << 4;
		if (x % TILE_SIZE != 0)
			raw |= 0x80000000u;
		if (y % TILE_SIZE != 0)
			raw |= 0x00800000u;
		return raw;
	}

	std::string PupuHex(const uint32_t pupu)
	{
		char text[16];
		snprintf(text, sizeof(text), "%08X", pupu);
		return text;
	}

	Layout ComputeLayout(const std::vector<Tile>& tiles)
	{
		Layout layout;
		if (tiles.empty())
			return layout;
		//Map::shift_to_origin: when the smallest x or y is negative, every tile moves by (|min x|, |min y|) - both, even
		//if only one of them is negative
		int minX = tiles[0].x, minY = tiles[0].y;
		for (const Tile& t : tiles)
		{
			minX = std::min<int>(minX, t.x);
			minY = std::min<int>(minY, t.y);
		}
		if (minX < 0 || minY < 0)
		{
			layout.shiftX = std::abs(minX);
			layout.shiftY = std::abs(minY);
		}
		layout.x.reserve(tiles.size());
		layout.y.reserve(tiles.size());
		for (const Tile& t : tiles)
		{
			//Point<int16_t> arithmetic
			layout.x.push_back(static_cast<int16_t>(t.x + layout.shiftX));
			layout.y.push_back(static_cast<int16_t>(t.y + layout.shiftY));
		}
		//Map::canvas: |max| + |min| + 16 of the shifted positions
		const auto [minIt, maxIt] = std::minmax_element(layout.x.begin(), layout.x.end());
		const auto [minYIt, maxYIt] = std::minmax_element(layout.y.begin(), layout.y.end());
		layout.width = std::abs(*maxIt) + std::abs(*minIt) + TILE_SIZE;
		layout.height = std::abs(*maxYIt) + std::abs(*minYIt) + TILE_SIZE;

		//UniquifyPupu: tiles with the same PupuID in the same 16x16 grid cell (x / 16, y / 16, truncated) get +1, +2...
		std::map<std::tuple<uint32_t, int, int>, uint8_t> counters;
		layout.pupu.reserve(tiles.size());
		for (size_t i = 0; i < tiles.size(); i++)
		{
			const int x = layout.x[i], y = layout.y[i];
			const uint32_t base = BasePupu(tiles[i], x, y);
			const auto key = std::make_tuple(base, static_cast<int>(static_cast<int16_t>(x / TILE_SIZE)),
				static_cast<int>(static_cast<int16_t>(y / TILE_SIZE)));
			auto it = counters.find(key);
			if (it == counters.end())
			{
				counters.emplace(key, uint8_t{ 0 });
				layout.pupu.push_back(base);
			}
			else
			{
				++it->second;
				layout.pupu.push_back(base + it->second);
			}
		}
		layout.canvas = { -layout.shiftX, -layout.shiftY, layout.width, layout.height };
		layout.fme = layout.canvas;
		return layout;
	}

	int Frame::ScaleOf(const int fileWidth, const int fileHeight) const
	{
		if (Empty() || fileWidth <= 0 || fileHeight <= 0 || fileWidth % width != 0 || fileHeight % height != 0)
			return 0;
		const int scale = fileWidth / width;
		return fileHeight / height == scale && scale >= 1 && scale <= 16 ? scale : 0;
	}

	Frame Frame::Union(const Frame& o) const
	{
		if (Empty())
			return o;
		if (o.Empty())
			return *this;
		const int l = std::min(left, o.left), t = std::min(top, o.top);
		const int r = std::max(left + width, o.left + o.width), b = std::max(top + height, o.top + o.height);
		return { l, t, r - l, b - t };
	}

	int FitPicture(const Layout& layout, const uint32_t name, const int width, const int height, Frame& frame)
	{
		if (const auto own = layout.frames.find(name); own != layout.frames.end())
			if (const int scale = own->second.ScaleOf(width, height))
			{
				frame = own->second;
				return scale;
			}
		for (const Frame& f : { layout.canvas, layout.fme })
			if (const int scale = f.ScaleOf(width, height))
			{
				frame = f;
				return scale;
			}
		return 0;
	}

	Frame PupuNames::FrameOf(const uint32_t pupu) const
	{
		const auto it = frame.find(static_cast<int>(pupu >> 24));
		return it == frame.end() ? Frame() : it->second;
	}

	PupuNames ComputePupuNames(const std::vector<Tile>& tiles)
	{
		PupuNames out;
		const size_t n = tiles.size();
		out.name.resize(n);
		for (size_t i = 0; i < n; i++)
		{
			//PuPu: ((((layer * 16 + blend) * 256 + animation id) * 256 + animation state) * 16), in 32 bits
			const Tile& t = tiles[i];
			uint32_t id = static_cast<uint32_t>(t.LayerId()) * 16u + t.blend;
			id = (id * 256u + t.animId) * 256u + t.animState;
			out.name[i] = id * 16u;
		}
		//a later tile at exactly the same place with the same name gets the earlier one's + 1 (PuPu's own pairwise loop,
		//names changing as it goes)
		for (size_t i = 0; i < n; i++)
			for (size_t j = i + 1; j < n; j++)
				if (tiles[i].x == tiles[j].x && tiles[i].y == tiles[j].y && ((out.name[i] ^ out.name[j]) & 0xFFFFFFF0u) == 0)
					out.name[j] = out.name[i] + 1;
		//one frame per layer number: the box around its tiles and 0, 0
		std::map<int, std::array<int, 4>> box; //min x, min y, max x, max y
		for (size_t i = 0; i < n; i++)
		{
			auto [it, added] = box.try_emplace(static_cast<int>(out.name[i] >> 24), std::array<int, 4>{ 0, 0, 0, 0 });
			std::array<int, 4>& b = it->second;
			b[0] = std::min<int>(b[0], tiles[i].x);
			b[1] = std::min<int>(b[1], tiles[i].y);
			b[2] = std::max<int>(b[2], tiles[i].x);
			b[3] = std::max<int>(b[3], tiles[i].y);
		}
		for (const auto& [layer, b] : box)
			out.frame[layer] = { b[0], b[1], b[2] - b[0] + TILE_SIZE, b[3] - b[1] + TILE_SIZE };
		return out;
	}

	PictureSet FindPictures(const std::vector<Tile>& tiles, const Layout& fme, const PupuNames& pupu, const PictureSize& size)
	{
		PictureSet out;
		const size_t n = std::min({ tiles.size(), fme.pupu.size(), pupu.name.size() });
		out.tile.assign(tiles.size(), -1);
		//every name a file can have, with the frames it can cover (PuPu's first)
		std::map<uint32_t, std::vector<std::pair<Frame, bool>>> candidates;
		const auto add = [&](const uint32_t name, const Frame& frame, const bool isPupu)
		{
			std::vector<std::pair<Frame, bool>>& list = candidates[name];
			for (const auto& c : list)
				if (c.first == frame)
					return;
			list.push_back({ frame, isPupu });
		};
		for (size_t i = 0; i < n; i++)
		{
			add(pupu.name[i], pupu.FrameOf(pupu.name[i]), true);
			add(fme.pupu[i], fme.fme, false);
		}
		for (auto& [name, list] : candidates)
			std::stable_sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second && !b.second; });
		//each file once: the frame its size matches
		std::map<uint32_t, Picture> found;
		for (const auto& [name, list] : candidates)
		{
			int width = 0, height = 0;
			if (!size || !size(name, width, height))
				continue;
			bool fits = false;
			for (const auto& [frame, isPupu] : list)
				if (const int scale = frame.ScaleOf(width, height))
				{
					found[name] = { name, frame, scale, isPupu, 0 };
					fits = true;
					break;
				}
			if (!fits)
			{
				std::string why = std::to_string(width) + "x" + std::to_string(height) + ", not";
				for (size_t k = 0; k < list.size(); k++)
					why += std::string(k ? " or" : "") + " " + std::to_string(list[k].first.width) + "x" + std::to_string(list[k].first.height)
						+ (list[k].second ? " (PuPu's frame)" : " (FME's canvas)");
				out.unfit[name] = why + " times a whole number";
			}
		}
		//per tile: PuPu's, else FME's (the file has to cover the frame that tool gives this tile)
		for (size_t i = 0; i < n; i++)
		{
			const auto p = found.find(pupu.name[i]);
			if (p != found.end() && p->second.frame == pupu.FrameOf(pupu.name[i]))
			{
				out.tile[i] = p->first;
				continue;
			}
			const auto f = found.find(fme.pupu[i]);
			if (f != found.end() && f->second.frame == fme.fme)
				out.tile[i] = f->first;
		}
		for (size_t i = 0; i < n; i++)
			if (out.tile[i] >= 0)
			{
				Picture& picture = out.pictures.try_emplace(static_cast<uint32_t>(out.tile[i]), found[static_cast<uint32_t>(out.tile[i])]).first->second;
				picture.tiles++;
			}
		return out;
	}

	Frame FieldFrame(const std::vector<Tile>& tiles, const PictureSet& set, const Frame& fme)
	{
		Frame frame;
		for (size_t i = 0; i < tiles.size() && i < set.tile.size(); i++)
			if (tiles[i].blend == 4 && set.tile[i] >= 0)
				if (const auto p = set.pictures.find(static_cast<uint32_t>(set.tile[i])); p != set.pictures.end())
					frame = frame.Union(p->second.frame);
		return frame.Empty() ? fme : frame;
	}

	Layout FramedLayout(const std::vector<Tile>& tiles, const Layout& fme, const PictureSet& set, const Frame& canvas)
	{
		Layout out;
		out.canvas = canvas;
		out.fme = fme.fme;
		out.shiftX = -canvas.left;
		out.shiftY = -canvas.top;
		out.width = canvas.width;
		out.height = canvas.height;
		const size_t n = tiles.size();
		out.x.resize(n);
		out.y.resize(n);
		out.pupu.resize(n);
		for (size_t i = 0; i < n; i++)
		{
			out.x[i] = tiles[i].x - canvas.left;
			out.y[i] = tiles[i].y - canvas.top;
			out.pupu[i] = i < set.tile.size() && set.tile[i] >= 0 ? static_cast<uint32_t>(set.tile[i]) : (i < fme.pupu.size() ? fme.pupu[i] : 0);
		}
		for (const auto& [name, picture] : set.pictures)
			out.frames[name] = picture.frame;
		return out;
	}

	namespace
	{
		uint32_t CellKey(const int page, const int depth, const int palette, const int cellX, const int cellY)
		{
			return static_cast<uint32_t>(page & 0xF) << 16 | static_cast<uint32_t>(depth & 3) << 14
				| static_cast<uint32_t>(palette & 0xF) << 8 | static_cast<uint32_t>(cellY & 0xF) << 4 | static_cast<uint32_t>(cellX & 0xF);
		}
		uint32_t CellKeyOf(const Tile& t)
		{
			return CellKey(t.TexturePage(), t.Depth(), t.Palette(), t.srcX / TILE_SIZE, t.srcY / TILE_SIZE);
		}
	}

	UnshareResult Unshare(const std::vector<Tile>& tiles, const std::vector<bool>& hasImage)
	{
		UnshareResult result;
		std::set<uint32_t> used; //every cell any tile uses
		std::map<std::pair<int, int>, std::set<int>> palettesOnPage; //(page, depth) -> palettes
		for (const Tile& t : tiles)
		{
			used.insert(CellKeyOf(t));
			palettesOnPage[{ t.TexturePage(), t.Depth() }].insert(t.Palette());
		}
		std::map<uint32_t, size_t> owner;
		for (size_t i = 0; i < tiles.size(); i++)
		{
			const Tile& t = tiles[i];
			const uint32_t key = CellKeyOf(t);
			if (owner.emplace(key, i).second)
				continue;
			const bool image = i < hasImage.size() && hasImage[i];
			if (!image || t.Depth() > 2)
			{
				result.shared++;
				continue;
			}
			//where to look: this page and palette, this page with its other palettes, other pages of this depth
			std::vector<std::pair<int, int>> candidates;
			candidates.push_back({ t.TexturePage(), t.Palette() });
			for (const int palette : palettesOnPage[{ t.TexturePage(), t.Depth() }])
				if (palette != t.Palette())
					candidates.push_back({ t.TexturePage(), palette });
			for (const auto& [pageDepth, palettes] : palettesOnPage)
				if (pageDepth.second == t.Depth() && pageDepth.first != t.TexturePage())
					for (const int palette : palettes)
						candidates.push_back({ pageDepth.first, palette });
			bool found = false;
			for (const auto& [page, palette] : candidates)
			{
				for (int cellY = 0; cellY < 16 && !found; cellY++)
					for (int cellX = 0; cellX < 16 && !found; cellX++)
					{
						const uint32_t target = CellKey(page, t.Depth(), palette, cellX, cellY);
						if (used.count(target))
							continue;
						used.insert(target);
						owner.emplace(target, i);
						result.moved.push_back({ i, page, palette, cellX * TILE_SIZE, cellY * TILE_SIZE });
						found = true;
					}
				if (found)
					break;
			}
			if (!found)
				result.unresolved++;
		}
		return result;
	}

	void ApplyRelocations(std::vector<Tile>& tiles, const std::vector<Relocation>& moved)
	{
		for (const Relocation& r : moved)
		{
			if (r.tile >= tiles.size())
				continue;
			Tile& t = tiles[r.tile];
			t.texId = static_cast<uint16_t>((t.texId & ~0xFu) | (r.texturePage & 0xF));
			t.palId = static_cast<uint16_t>((t.palId & ~(0xFu << 6)) | ((r.palette & 0xF) << 6));
			t.srcX = static_cast<uint8_t>(r.srcX);
			t.srcY = static_cast<uint8_t>(r.srcY);
		}
	}

	uint32_t PageKey(const int texturePage, const int palette)
	{
		return static_cast<uint32_t>(texturePage & 0xFF) << 8 | static_cast<uint32_t>(palette & 0xFF);
	}

	std::map<uint32_t, PagePlan> PlanPages(const std::vector<Tile>& tiles, const Layout& layout, const std::vector<bool>& hasImage,
		const std::vector<Tile>* shipped)
	{
		std::map<uint32_t, PagePlan> pages;
		std::map<uint32_t, std::set<int>> referenced, stamped; //cells (srcY << 8 | srcX)
		for (size_t i = 0; i < tiles.size() && i < layout.pupu.size(); i++)
		{
			const Tile& t = tiles[i];
			const uint32_t key = PageKey(t.TexturePage(), t.Palette());
			auto [it, added] = pages.try_emplace(key);
			PagePlan& page = it->second;
			if (added)
			{
				page.texturePage = t.TexturePage();
				page.palette = t.Palette();
				page.depth = t.Depth();
			}
			else if (page.depth != t.Depth())
				page.mixedDepth = true;
			const int cell = t.srcY << 8 | t.srcX;
			referenced[key].insert(cell);
			if (t.srcX % TILE_SIZE != 0 || t.srcY % TILE_SIZE != 0)
				page.aligned = false;
			if (i < hasImage.size() && hasImage[i] && stamped[key].insert(cell).second)
			{
				CellStamp stamp{ i, layout.pupu[i], layout.x[i], layout.y[i], t.x, t.y, t.srcX, t.srcY };
				stamp.semi = t.blend < 4;
				stamp.z = t.z;
				stamp.layerId = t.LayerId();
				if (shipped != nullptr && i < shipped->size())
				{
					const Tile& before = (*shipped)[i];
					if (before.TexturePage() == t.TexturePage() && before.Palette() == t.Palette() && before.Depth() == t.Depth())
					{
						stamp.originX = before.srcX;
						stamp.originY = before.srcY;
					}
				}
				page.stamps.push_back(stamp);
			}
		}
		for (auto& [key, page] : pages)
		{
			const std::set<int>& done = stamped[key];
			for (const int cell : referenced[key])
				if (!done.count(cell))
				{
					page.complete = false;
					break;
				}
		}
		return pages;
	}

	Image ComposePage(const int logicalWidth, const int logicalHeight, const int scale, const uint8_t* base,
		const int baseWidth, const int baseHeight, const bool baseIsBgra, const std::vector<StampSource>& stamps)
	{
		Image out;
		if (logicalWidth <= 0 || logicalHeight <= 0 || scale <= 0)
			return out;
		out.width = logicalWidth * scale;
		out.height = logicalHeight * scale;
		out.rgba.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
		if (base != nullptr && baseWidth > 0 && baseHeight > 0)
			for (int y = 0; y < out.height; y++)
			{
				const int sy = static_cast<int>(static_cast<int64_t>(y) * baseHeight / out.height);
				for (int x = 0; x < out.width; x++)
				{
					const int sx = static_cast<int>(static_cast<int64_t>(x) * baseWidth / out.width);
					const uint8_t* s = base + (static_cast<size_t>(sy) * baseWidth + sx) * 4;
					uint8_t* d = out.rgba.data() + (static_cast<size_t>(y) * out.width + x) * 4;
					d[0] = s[baseIsBgra ? 2 : 0];
					d[1] = s[1];
					d[2] = s[baseIsBgra ? 0 : 2];
					d[3] = s[3];
				}
			}
		const int block = TILE_SIZE * scale;
		for (const StampSource& stamp : stamps)
		{
			if (stamp.image == nullptr || !stamp.image->IsValid() || stamp.scale <= 0)
				continue;
			const Image& image = *stamp.image;
			const int sourceBlock = TILE_SIZE * stamp.scale;
			const int sx0 = stamp.layerX * stamp.scale, sy0 = stamp.layerY * stamp.scale;
			const int dx0 = stamp.cellX * scale, dy0 = stamp.cellY * scale;
			for (int y = 0; y < block; y++)
			{
				const int dy = dy0 + y;
				if (dy < 0 || dy >= out.height)
					continue;
				for (int x = 0; x < block; x++)
				{
					const int dx = dx0 + x;
					if (dx < 0 || dx >= out.width)
						continue;
					uint8_t* d = out.rgba.data() + (static_cast<size_t>(dy) * out.width + dx) * 4;
					if (stamp.scale == scale)
					{
						const int sx = sx0 + x, sy = sy0 + y;
						if (sx < 0 || sy < 0 || sx >= image.width || sy >= image.height)
						{
							memset(d, 0, 4);
							continue;
						}
						memcpy(d, image.rgba.data() + (static_cast<size_t>(sy) * image.width + sx) * 4, 4);
						continue;
					}
					//another scale: bilinear inside the tile's own block (never reads a neighbour)
					const double fx = std::clamp((x + 0.5) * sourceBlock / block - 0.5, 0.0, sourceBlock - 1.0);
					const double fy = std::clamp((y + 0.5) * sourceBlock / block - 0.5, 0.0, sourceBlock - 1.0);
					const int ix = static_cast<int>(fx), iy = static_cast<int>(fy);
					const int ix1 = std::min(ix + 1, sourceBlock - 1), iy1 = std::min(iy + 1, sourceBlock - 1);
					const double ax = fx - ix, ay = fy - iy;
					double sum[4] = { 0, 0, 0, 0 }, weight = 0;
					const int px[4] = { ix, ix1, ix, ix1 }, py[4] = { iy, iy, iy1, iy1 };
					const double pw[4] = { (1 - ax) * (1 - ay), ax * (1 - ay), (1 - ax) * ay, ax * ay };
					for (int k = 0; k < 4; k++)
					{
						const int sx = sx0 + px[k], sy = sy0 + py[k];
						if (sx < 0 || sy < 0 || sx >= image.width || sy >= image.height || pw[k] <= 0)
							continue;
						const uint8_t* s = image.rgba.data() + (static_cast<size_t>(sy) * image.width + sx) * 4;
						//alpha-weighted so transparent texels do not darken edges (unless alpha holds data: PBR maps)
						const double a = stamp.coverage ? s[3] / 255.0 * pw[k] : pw[k];
						sum[0] += s[0] * a;
						sum[1] += s[1] * a;
						sum[2] += s[2] * a;
						sum[3] += s[3] * pw[k];
						weight += a;
					}
					for (int c = 0; c < 3; c++)
						d[c] = weight > 0 ? static_cast<uint8_t>(std::clamp(std::lround(sum[c] / weight), 0L, 255L)) : 0;
					d[3] = static_cast<uint8_t>(std::clamp(std::lround(sum[3]), 0L, 255L));
				}
			}
		}
		return out;
	}

	int MimTexel(const uint8_t* mim, const size_t size, const int texturePage, const int depth, const int palette, const int x, const int y)
	{
		if (mim == nullptr || size < MIM_SIZE || texturePage < 0 || x < 0 || y < 0 || y >= 256 || depth < 0 || depth > 2)
			return -1;
		const size_t column = static_cast<size_t>(texturePage) * 128 + (depth == 0 ? x / 2 : depth == 1 ? x : x * 2);
		if (column + (depth == 2 ? 1 : 0) >= MIM_ROW)
			return -1;
		const uint8_t* p = mim + MIM_TEXTURE + static_cast<size_t>(y) * MIM_ROW + column;
		if (depth == 2)
			return U16(p);
		const int index = depth == 1 ? p[0] : (x & 1) ? p[0] >> 4 : p[0] & 0xF;
		return U16(mim + MIM_PALETTES + (static_cast<size_t>(palette & 0xF) * 256 + index) * 2);
	}

	namespace
	{
		//8-bit channel from the game against a 5-bit one: the game's conversion (c << 3, c << 3 | c >> 2 or c * 255 / 31)
		//is not known, any of them counts
		bool SameChannel(const int value, const int five)
		{
			return (value >> 3) == five || (value * 31 + 127) / 255 == five;
		}

		bool SameColour(const int colour, const uint8_t* p, const bool rgba)
		{
			const int r = p[rgba ? 0 : 2], g = p[1], b = p[rgba ? 2 : 0], a = p[3];
			if (colour == 0) //transparent
				return a == 0 || (r < 8 && g < 8 && b < 8);
			const int cr = colour & 31, cg = colour >> 5 & 31, cb = colour >> 10 & 31;
			if (a == 0 && (cr | cg | cb) != 0)
				return false;
			return SameChannel(r, cr) && SameChannel(g, cg) && SameChannel(b, cb);
		}
	}

	std::vector<PaletteMatch> MatchPalette(const uint8_t* mim, const size_t mimSize, const std::vector<Tile>& tiles, const int texturePage,
		const uint8_t* bgra, const int width, const int height)
	{
		std::vector<PaletteMatch> out;
		if (mim == nullptr || mimSize < MIM_SIZE || bgra == nullptr || width < 256 || height < 256 || width % 256 != 0 || height % 256 != 0)
			return out;
		const int scaleX = width / 256, scaleY = height / 256;
		std::set<std::pair<int, int>> candidates; //depth, palette
		std::map<int, std::set<int>> cells; //depth -> srcY << 8 | srcX
		for (const Tile& t : tiles)
			if (t.TexturePage() == texturePage && t.Depth() <= 2)
			{
				candidates.insert({ t.Depth(), t.Depth() == 2 ? 0 : t.Palette() });
				cells[t.Depth()].insert(t.srcY << 8 | t.srcX);
			}
		for (const auto& [depth, palette] : candidates)
		{
			PaletteMatch match;
			match.palette = palette;
			match.depth = depth;
			size_t asBgra = 0, asRgba = 0;
			for (const int cell : cells[depth])
			{
				const int cellX = cell & 0xFF, cellY = cell >> 8;
				for (int y = 0; y < TILE_SIZE; y++)
				{
					const int ty = cellY + y;
					if (ty > 255)
						break;
					for (int x = 0; x < TILE_SIZE; x++)
					{
						const int tx = cellX + x;
						if (tx > 255)
							break;
						const int colour = MimTexel(mim, mimSize, texturePage, depth, palette, tx, ty);
						if (colour < 0)
							continue;
						const uint8_t* p = bgra + (static_cast<size_t>(ty) * scaleY * width + static_cast<size_t>(tx) * scaleX) * 4;
						match.compared++;
						match.opaque += colour != 0;
						asBgra += SameColour(colour, p, false);
						asRgba += SameColour(colour, p, true);
					}
				}
			}
			match.matching = std::max(asBgra, asRgba);
			out.push_back(match);
		}
		std::stable_sort(out.begin(), out.end(), [](const PaletteMatch& a, const PaletteMatch& b)
		{
			if (a.Score() != b.Score())
				return a.Score() > b.Score();
			return a.palette != b.palette ? a.palette < b.palette : a.depth < b.depth;
		});
		return out;
	}

	size_t InheritSemiTransparency(Image& page, const int scale, const std::vector<CellStamp>& stamps, const uint8_t* gameAlpha,
		const int gameWidth, const int gameHeight)
	{
		if (!page.IsValid() || gameAlpha == nullptr || scale <= 0 || gameWidth < 256 || gameHeight < 256)
			return 0;
		const int gameScaleX = gameWidth / 256, gameScaleY = gameHeight / 256, block = TILE_SIZE * scale;
		size_t changed = 0;
		for (const CellStamp& stamp : stamps)
		{
			if (!stamp.semi || stamp.originX < 0 || stamp.originY < 0)
				continue;
			for (int y = 0; y < block; y++)
			{
				const int dy = stamp.cellY * scale + y, gy = stamp.originY + y / scale;
				if (dy >= page.height || gy > 255)
					break;
				for (int x = 0; x < block; x++)
				{
					const int dx = stamp.cellX * scale + x, gx = stamp.originX + x / scale;
					if (dx >= page.width || gx > 255)
						break;
					const uint8_t a = gameAlpha[static_cast<size_t>(gy) * gameScaleY * gameWidth + static_cast<size_t>(gx) * gameScaleX];
					uint8_t& out = page.rgba[(static_cast<size_t>(dy) * page.width + dx) * 4 + 3];
					if (a == 0 || a == 255 || out == 0)
						continue;
					out = static_cast<uint8_t>((out * a + 127) / 255);
					changed++;
				}
			}
		}
		return changed;
	}

	void SplitLanguage(const std::string& stem, std::string& field, std::string& language)
	{
		static const char* const languages[] = { "en", "fr", "de", "it", "es", "jp" };
		field = stem;
		language.clear();
		if (stem.size() < 4 || stem[stem.size() - 3] != '_')
			return;
		std::string suffix = stem.substr(stem.size() - 2);
		std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](const unsigned char c) { return static_cast<char>(tolower(c)); });
		for (const char* l : languages)
			if (suffix == l)
			{
				field = stem.substr(0, stem.size() - 3);
				language = suffix;
				return;
			}
	}
}
