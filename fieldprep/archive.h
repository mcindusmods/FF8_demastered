#pragma once
//FF8's archives: <name>.fl (one path per line), <name>.fi (12 bytes per file: uncompressed size, offset in the .fs,
//compression 0 none / 1 LZSS / 2 LZ4) and <name>.fs (the data). field.fs holds one such archive per field
//(mapdata\<xx>\<field>.fi/.fl/.fs), which holds the field's files (.map, .mim, .ca, .id, ...).

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fieldprep
{
	//FF8's LZSS (4 KB ring buffer, 18-byte matches). expected: the uncompressed size (0: until the data ends)
	std::vector<uint8_t> Lzss(const uint8_t* data, size_t size, size_t expected);
	//An LZ4 block (no frame), as the Remastered archives store them
	std::vector<uint8_t> Lz4Block(const uint8_t* data, size_t size, size_t expected);

	class Archive
	{
	public:
		//fl: the list's text; fi: the index; fs: a file on disk (read as needed) or bytes in memory
		bool OpenFiles(const std::string& flPath, const std::string& fiPath, const std::string& fsPath, std::string& error);
		bool OpenMemory(const std::vector<uint8_t>& fl, const std::vector<uint8_t>& fi, std::vector<uint8_t> fs, std::string& error);
		//the .fs is part of a bigger file (inside main.zzz): its bytes start at base
		bool OpenSlice(const std::vector<uint8_t>& fl, const std::vector<uint8_t>& fi, const std::string& file, uint64_t base, uint64_t size,
			std::string& error);
		size_t Count() const { return names.size(); }
		const std::string& Name(const size_t i) const { return names[i]; }
		//the entry whose file name (after the last \ or /) is this, any case; -1: none
		int Find(const std::string& fileName) const;
		bool Extract(size_t index, std::vector<uint8_t>& out, std::string& error) const;

	private:
		struct Entry
		{
			uint32_t size = 0, offset = 0, compression = 0;
		};
		bool ReadIndex(const std::vector<uint8_t>& fl, const std::vector<uint8_t>& fi, std::string& error);
		bool ReadData(uint64_t offset, size_t length, std::vector<uint8_t>& out) const;
		std::vector<std::string> names;
		std::vector<Entry> entries;
		std::string fsPath;
		uint64_t fsBase = 0;
		std::vector<uint8_t> fsData;
		bool inMemory = false;
		uint64_t fsSize = 0;
	};

	bool ReadFile(const std::string& path, std::vector<uint8_t>& out);

	//A field's own files, from field.fi/fl/fs
	struct FieldFiles
	{
		std::vector<uint8_t> map, mim, ca, id;
		std::string mapName; //which .map (with or without a language)
		std::string note; //when it is not the one asked for (a field with only per-language .map files)
	};
	//Remastered's main.zzz / other.zzz: uint32 count, count x (uint32 name length, name, uint64 offset, uint32 size), data
	struct ZzzEntry
	{
		std::string name;
		uint64_t offset = 0;
		uint32_t size = 0;
	};
	bool ReadZzz(const std::string& path, std::vector<ZzzEntry>& entries, std::string& error);
	//where: a folder with field.fl/fi/fs (DEMASTER_EXP\data), or the game's main.zzz.
	//field: "bvtr_1"; language: "" or "fr" (the .map of that language, else the plain one, else English's, else any other
	//language's). The .ca and .id are filled even when it returns false (no .map).
	bool LoadFromArchive(const std::string& where, const std::string& field, const std::string& language, FieldFiles& out,
		std::string& error);
}
