#include "usage_file.hpp"

#include <cstring>
#include <format>
#include <fstream>

namespace MapKit::Zone {
	namespace {
		template <typename T>
		bool ReadArray(std::ifstream& f, std::uint32_t count, std::vector<T>& out) {
			out.resize(count);
			if (count) {
				f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(sizeof(T) * count));
			}
			return static_cast<bool>(f);
		}

		std::string Text(const char* chars, std::size_t size) {
			return std::string(chars, strnlen(chars, size));
		}
	}

	std::string UsageFile::Map() const { return Text(header.map, sizeof(header.map)); }

	std::string UsageFile::ZoneName(std::uint32_t index) const {
		for (const UsageZone& zone : zones) {
			if (zone.index == index) return Text(zone.name, sizeof(zone.name));
		}
		return {};
	}

	std::uint32_t UsageFile::TableType(std::uint32_t table) const {
		for (const UsageTable& t : tables) {
			if (t.index == table) return t.type;
		}
		return kBgCacheNoAsset;
	}

	std::string UsageFile::TableName(std::uint32_t table) const {
		for (const UsageTable& t : tables) {
			if (t.index == table) return Text(t.name, sizeof(t.name));
		}
		return std::format("table {}", table);
	}

	bool ReadUsageFile(const std::filesystem::path& path, UsageFile& out, std::string& error) {
		std::ifstream f(path, std::ios::binary);
		if (!f) {
			error = std::format("cannot open {}", path.string());
			return false;
		}
		f.read(reinterpret_cast<char*>(&out.header), sizeof(out.header));
		if (!f || std::memcmp(out.header.magic, "MKUS", 4) != 0) {
			error = std::format("{} is not an asset-usage census (no MKUS header)", path.string());
			return false;
		}
		if (out.header.version != kUsageVersion) {
			error = std::format("{}: census version {}, this tool reads {}", path.string(), out.header.version, kUsageVersion);
			return false;
		}
		const UsageFileHeader& h = out.header;
		if (!ReadArray(f, h.zoneCount, out.zones) || !ReadArray(f, h.entryCount, out.entries)
			|| !ReadArray(f, h.tableCount, out.tables) || !ReadArray(f, h.listCount, out.lists)
			|| !ReadArray(f, h.findCount, out.finds) || !ReadArray(f, h.cacheCount, out.caches)) {
			error = std::format("{} is truncated", path.string());
			return false;
		}
		return true;
	}

	bool IsRegistrationCaller(std::uint64_t caller) {
		return (caller >= 0x7FF726213DB0ull && caller < 0x7FF726213DB0ull + 0x24D)
			|| (caller >= 0x7FF726213850ull && caller < 0x7FF726213850ull + 0x42B);
	}

	bool IsStreamInCaller(std::uint64_t caller) {
		return caller >= 0x7FF727F504C0ull && caller < 0x7FF727F504C0ull + 0x14F;
	}
}
