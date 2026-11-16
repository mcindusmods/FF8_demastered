#include "archive.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fieldprep
{
	std::vector<uint8_t> Lzss(const uint8_t* data, const size_t size, const size_t expected)
	{
		constexpr unsigned N = 4096, F = 18;
		std::vector<uint8_t> out;
		if (data == nullptr)
			return out;
		out.reserve(expected);
		uint8_t ring[N + F - 1] = {};
		unsigned r = N - F, flags = 0;
		size_t p = 0;
		while (p < size)
		{
			if (((flags >>= 1) & 0x100) == 0)
			{
				flags = data[p++] | 0xFF00u;
				if (p >= size)
					break;
			}
			if (flags & 1)
			{
				const uint8_t c = data[p++];
				out.push_back(c);
				ring[r++] = c;
				r &= N - 1;
			}
			else
			{
				if (p + 1 >= size)
					break;
				unsigned offset = data[p++];
				unsigned count = data[p++];
				offset |= (count & 0xF0u) << 4;
				count = (count & 0x0Fu) + 2;
				for (unsigned k = 0; k <= count; k++)
				{
					const uint8_t c = ring[(offset + k) & (N - 1)];
					out.push_back(c);
					ring[r++] = c;
					r &= N - 1;
				}
			}
			if (expected != 0 && out.size() >= expected)
				break;
		}
		if (expected != 0 && out.size() > expected)
			out.resize(expected);
		return out;
	}

	std::vector<uint8_t> Lz4Block(const uint8_t* data, const size_t size, const size_t expected)
	{
		std::vector<uint8_t> out;
		if (data == nullptr)
			return out;
		out.reserve(expected);
		size_t p = 0;
		while (p < size)
		{
			const uint8_t token = data[p++];
			size_t literals = token >> 4;
			if (literals == 15)
			{
				uint8_t b;
				do
				{
					if (p >= size)
						return out;
					b = data[p++];
					literals += b;
				} while (b == 255);
			}
			if (p + literals > size)
				return out;
			out.insert(out.end(), data + p, data + p + literals);
			p += literals;
			if (p >= size)
				break; //the last sequence is literals only
			if (p + 2 > size)
				return out;
			const size_t offset = data[p] | data[p + 1] << 8;
			p += 2;
			size_t length = token & 15;
			if (length == 15)
			{
				uint8_t b;
				do
				{
					if (p >= size)
						return out;
					b = data[p++];
					length += b;
				} while (b == 255);
			}
			length += 4;
			if (offset == 0 || offset > out.size())
				return out; //broken
			const size_t from = out.size() - offset;
			for (size_t i = 0; i < length; i++)
				out.push_back(out[from + i]);
		}
		return out;
	}

	bool ReadFile(const std::string& path, std::vector<uint8_t>& out)
	{
		std::ifstream in(path, std::ios::binary);
		if (!in)
			return false;
		in.seekg(0, std::ios::end);
		const std::streamoff size = in.tellg();
		if (size < 0)
			return false;
		in.seekg(0, std::ios::beg);
		out.resize(static_cast<size_t>(size));
		if (size > 0)
			in.read(reinterpret_cast<char*>(out.data()), size);
		return static_cast<bool>(in) || in.eof();
	}

	namespace
	{
		std::string Lower(std::string s)
		{
			std::transform(s.begin(), s.end(), s.begin(), [](const unsigned char c) { return static_cast<char>(tolower(c)); });
			return s;
		}

		uint32_t U32(const uint8_t* p)
		{
			return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
		}

		//a file in a folder, whatever case it is in
		std::string FindInFolder(const std::string& folder, const std::string& name)
		{
			std::error_code error;
			const std::filesystem::path exact = std::filesystem::path(folder) / name;
			if (std::filesystem::is_regular_file(exact, error))
				return exact.string();
			const std::string want = Lower(name);
			for (const auto& entry : std::filesystem::directory_iterator(folder, error))
				if (Lower(entry.path().filename().string()) == want)
					return entry.path().string();
			return {};
		}
	}

	bool Archive::ReadIndex(const std::vector<uint8_t>& fl, const std::vector<uint8_t>& fi, std::string& error)
	{
		names.clear();
		entries.clear();
		std::string line;
		for (const uint8_t c : fl)
		{
			if (c == '\n' || c == '\r' || c == 0)
			{
				if (!line.empty())
					names.push_back(line);
				line.clear();
			}
			else
				line += static_cast<char>(c);
		}
		if (!line.empty())
			names.push_back(line);
		const size_t count = fi.size() / 12;
		if (count == 0)
		{
			error = "empty index";
			return false;
		}
		for (size_t i = 0; i < count; i++)
			entries.push_back({ U32(fi.data() + i * 12), U32(fi.data() + i * 12 + 4), U32(fi.data() + i * 12 + 8) });
		if (names.size() > entries.size())
			names.resize(entries.size());
		while (names.size() < entries.size())
			names.push_back("");
		return true;
	}

	bool Archive::OpenFiles(const std::string& flPath, const std::string& fiPath, const std::string& fsPath_, std::string& error)
	{
		std::vector<uint8_t> fl, fi;
		if (!ReadFile(flPath, fl) || !ReadFile(fiPath, fi))
		{
			error = "can't read " + flPath + " / " + fiPath;
			return false;
		}
		std::error_code ec;
		fsSize = std::filesystem::file_size(fsPath_, ec);
		if (ec)
		{
			error = "can't read " + fsPath_;
			return false;
		}
		fsPath = fsPath_;
		inMemory = false;
		return ReadIndex(fl, fi, error);
	}

	bool Archive::OpenSlice(const std::vector<uint8_t>& fl, const std::vector<uint8_t>& fi, const std::string& file, const uint64_t base,
		const uint64_t size, std::string& error)
	{
		fsPath = file;
		fsBase = base;
		fsSize = size;
		inMemory = false;
		return ReadIndex(fl, fi, error);
	}

	bool Archive::OpenMemory(const std::vector<uint8_t>& fl, const std::vector<uint8_t>& fi, std::vector<uint8_t> fs, std::string& error)
	{
		fsData = std::move(fs);
		fsSize = fsData.size();
		inMemory = true;
		return ReadIndex(fl, fi, error);
	}

	int Archive::Find(const std::string& fileName) const
	{
		const std::string want = Lower(fileName);
		for (size_t i = 0; i < names.size(); i++)
		{
			std::string name = names[i];
			if (const size_t slash = name.find_last_of("\\/"); slash != std::string::npos)
				name = name.substr(slash + 1);
			if (Lower(name) == want)
				return static_cast<int>(i);
		}
		return -1;
	}

	bool Archive::ReadData(const uint64_t offset, const size_t length, std::vector<uint8_t>& out) const
	{
		if (offset + length > fsSize)
			return false;
		out.resize(length);
		if (inMemory)
		{
			if (length > 0)
				memcpy(out.data(), fsData.data() + offset, length);
			return true;
		}
		std::ifstream in(fsPath, std::ios::binary);
		if (!in)
			return false;
		in.seekg(static_cast<std::streamoff>(fsBase + offset));
		if (length > 0)
			in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(length));
		return static_cast<bool>(in);
	}

	bool Archive::Extract(const size_t index, std::vector<uint8_t>& out, std::string& error) const
	{
		if (index >= entries.size())
		{
			error = "no such entry";
			return false;
		}
		const Entry& e = entries[index];
		std::vector<uint8_t> header;
		switch (e.compression)
		{
		case 0:
			if (!ReadData(e.offset, e.size, out))
			{
				error = "entry past the end of the .fs";
				return false;
			}
			return true;
		case 1:
		{
			if (!ReadData(e.offset, 4, header))
				break;
			const uint32_t compressed = U32(header.data());
			std::vector<uint8_t> packed;
			if (!ReadData(e.offset + 4ull, compressed, packed))
				break;
			out = Lzss(packed.data(), packed.size(), e.size);
			if (out.size() != e.size)
			{
				error = "LZSS data ended early";
				return false;
			}
			return true;
		}
		case 2:
		{
			if (!ReadData(e.offset, 12, header))
				break;
			const uint32_t section = U32(header.data());
			const uint32_t size = U32(header.data() + 8);
			if (section < 8)
				break;
			std::vector<uint8_t> packed;
			if (!ReadData(e.offset + 12ull, section - 8, packed))
				break;
			out = Lz4Block(packed.data(), packed.size(), size);
			if (out.size() != size)
			{
				error = "LZ4 data ended early";
				return false;
			}
			return true;
		}
		default:
			error = "unknown compression " + std::to_string(e.compression);
			return false;
		}
		error = "entry past the end of the .fs";
		return false;
	}

	bool ReadZzz(const std::string& path, std::vector<ZzzEntry>& entries, std::string& error)
	{
		entries.clear();
		std::ifstream in(path, std::ios::binary);
		if (!in)
		{
			error = "can't read " + path;
			return false;
		}
		in.seekg(0, std::ios::end);
		const uint64_t fileSize = static_cast<uint64_t>(in.tellg());
		in.seekg(0);
		uint8_t word[12];
		if (!in.read(reinterpret_cast<char*>(word), 4))
		{
			error = path + " is empty";
			return false;
		}
		const uint32_t count = U32(word);
		if (count == 0 || count > 1000000)
		{
			error = path + " is not a zzz archive";
			return false;
		}
		for (uint32_t i = 0; i < count; i++)
		{
			if (!in.read(reinterpret_cast<char*>(word), 4))
				break;
			const uint32_t length = U32(word);
			if (length == 0 || length > 1024)
				break;
			std::string name(length, '\0');
			if (!in.read(name.data(), length) || !in.read(reinterpret_cast<char*>(word), 12))
				break;
			ZzzEntry e;
			e.name = name;
			e.offset = static_cast<uint64_t>(U32(word)) | static_cast<uint64_t>(U32(word + 4)) << 32;
			e.size = U32(word + 8);
			if (e.offset + e.size > fileSize)
				break;
			entries.push_back(e);
		}
		if (entries.size() != count)
		{
			error = path + ": its list of files ends early";
			return false;
		}
		return true;
	}

	namespace
	{
		//field.fl/fi from a folder or a zzz, and the field.fs opened from either
		bool OpenFieldArchive(const std::string& where, Archive& all, std::string& error)
		{
			std::error_code ec;
			if (std::filesystem::is_directory(where, ec))
			{
				const std::string fl = FindInFolder(where, "field.fl"), fi = FindInFolder(where, "field.fi"), fs = FindInFolder(where, "field.fs");
				if (fl.empty() || fi.empty() || fs.empty())
				{
					error = "no field.fl/fi/fs in " + where;
					return false;
				}
				return all.OpenFiles(fl, fi, fs, error);
			}
			std::vector<ZzzEntry> entries;
			if (!ReadZzz(where, entries, error))
				return false;
			//data\field.fl etc. (the first ones when there are several)
			const ZzzEntry* found[3] = {};
			const char* names[3] = { "field.fl", "field.fi", "field.fs" };
			for (const ZzzEntry& e : entries)
			{
				std::string name = e.name;
				if (const size_t slash = name.find_last_of("\\/"); slash != std::string::npos)
					name = name.substr(slash + 1);
				name = Lower(name);
				for (int k = 0; k < 3; k++)
					if (name == names[k] && found[k] == nullptr)
						found[k] = &e;
			}
			if (found[0] == nullptr || found[1] == nullptr || found[2] == nullptr)
			{
				error = "no field.fl/fi/fs in " + where;
				return false;
			}
			std::vector<uint8_t> fl, fi;
			std::ifstream in(where, std::ios::binary);
			for (int k = 0; k < 2; k++)
			{
				std::vector<uint8_t>& out = k == 0 ? fl : fi;
				out.resize(found[k]->size);
				in.seekg(static_cast<std::streamoff>(found[k]->offset));
				if (!in.read(reinterpret_cast<char*>(out.data()), found[k]->size))
				{
					error = "can't read " + found[k]->name + " from " + where;
					return false;
				}
			}
			return all.OpenSlice(fl, fi, where, found[2]->offset, found[2]->size, error);
		}
	}

	bool LoadFromArchive(const std::string& where, const std::string& field, const std::string& language, FieldFiles& out,
		std::string& error)
	{
		Archive all;
		if (!OpenFieldArchive(where, all, error))
			return false;
		const int innerFl = all.Find(field + ".fl"), innerFi = all.Find(field + ".fi"), innerFs = all.Find(field + ".fs");
		if (innerFl < 0 || innerFi < 0 || innerFs < 0)
		{
			error = field + " is not in the field archive in " + where;
			return false;
		}
		std::vector<uint8_t> innerList, innerIndex, innerData;
		if (!all.Extract(static_cast<size_t>(innerFl), innerList, error) || !all.Extract(static_cast<size_t>(innerFi), innerIndex, error)
			|| !all.Extract(static_cast<size_t>(innerFs), innerData, error))
		{
			error = field + ": " + error;
			return false;
		}
		Archive inner;
		if (!inner.OpenMemory(innerList, innerIndex, std::move(innerData), error))
		{
			error = field + ": " + error;
			return false;
		}
		std::string problems; //entries that are there but can't be read
		const auto get = [&](const std::string& name, std::vector<uint8_t>& data)
		{
			const int i = inner.Find(name);
			if (i < 0)
				return false;
			std::string why;
			if (inner.Extract(static_cast<size_t>(i), data, why))
				return true;
			problems += (problems.empty() ? "" : "; ") + name + ": " + why;
			data.clear();
			return false;
		};
		out = FieldFiles();
		//camera and walkmesh first: they are used even when the .map comes from somewhere else
		get(field + ".mim", out.mim);
		get(field + ".ca", out.ca);
		get(field + ".id", out.id);
		//the .map: the images' language, the plain one, else a language's (fields with text in their background, like
		//bvtr_3, have only <field>_<language>.map: images without a language go with English first)
		std::vector<std::string> candidates;
		if (!language.empty())
			candidates.push_back(field + "_" + language + ".map");
		candidates.push_back(field + ".map");
		for (const char* l : { "en", "fr", "de", "it", "es", "jp" })
			if (language != l)
				candidates.push_back(field + "_" + l + ".map");
		std::vector<std::string> maps; //every .map of the field in the archive
		for (size_t i = 0; i < inner.Count(); i++)
		{
			std::string name = inner.Name(i);
			if (const size_t slash = name.find_last_of("\\/"); slash != std::string::npos)
				name = name.substr(slash + 1);
			const std::string lower = Lower(name);
			if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".map") == 0)
			{
				maps.push_back(name);
				if (std::find(candidates.begin(), candidates.end(), lower) == candidates.end())
					candidates.push_back(lower);
			}
		}
		for (const std::string& name : candidates)
			if (get(name, out.map))
			{
				out.mapName = name;
				break;
			}
		if (out.map.empty())
		{
			std::string list;
			for (size_t i = 0; i < inner.Count() && i < 24; i++)
			{
				std::string name = inner.Name(i);
				if (const size_t slash = name.find_last_of("\\/"); slash != std::string::npos)
					name = name.substr(slash + 1);
				list += (list.empty() ? "" : ", ") + name;
			}
			error = field + ": no .map in its archive" + (problems.empty() ? std::string() : " that can be read (" + problems + ")")
				+ ". Its archive holds: " + (list.empty() ? std::string("nothing") : list);
			return false;
		}
		const std::string plain = Lower(field + ".map"), wanted = Lower(language.empty() ? plain : field + "_" + language + ".map");
		if (Lower(out.mapName) != wanted)
			out.note = "no " + (language.empty() ? field + ".map" : field + "_" + language + ".map") + " in the archive (" + std::to_string(maps.size())
				+ " .map file(s) there): using " + out.mapName;
		return true;
	}
}
