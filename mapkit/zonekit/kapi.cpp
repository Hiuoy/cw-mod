#include "kapi.hpp"
#include "oodle.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>

namespace MapKit::Zone::Kapi {
	namespace {
		namespace fs = std::filesystem;

		constexpr std::uint32_t kMagic = 0x4950414B; // "KAPI"
		constexpr std::uint64_t kTypeData = 3;
		constexpr std::size_t kHeaderSize = 2024;
		constexpr std::size_t kCountsOffset = 32 + 1896;

		template <typename T>
		T Read(const std::uint8_t* p) {
			T v;
			std::memcpy(&v, p, sizeof(T));
			return v;
		}
	}

	bool PackageIndex::Open(const fs::path& zoneDir, std::string& error, const std::function<bool(const std::string&)>& filter) {
		m_Files.clear();
		m_Entries.clear();
		std::error_code ec;
		std::vector<fs::path> files;
		for (const auto& entry : fs::directory_iterator(zoneDir, ec)) {
			if (entry.path().extension() == ".xsub" && (!filter || filter(entry.path().stem().string()))) {
				files.push_back(entry.path());
			}
		}
		if (ec) {
			error = std::format("cannot list {}: {}", zoneDir.string(), ec.message());
			return false;
		}
		std::sort(files.begin(), files.end());

		for (const fs::path& path : files) {
			std::ifstream in(path, std::ios::binary);
			std::uint8_t header[kHeaderSize];
			if (!in.read(reinterpret_cast<char*>(header), sizeof(header))) {
				continue;
			}
			if (Read<std::uint32_t>(header) != kMagic || Read<std::uint64_t>(header + 16) != kTypeData) {
				continue;
			}
			const auto hashCount = Read<std::int64_t>(header + kCountsOffset + 24);
			const auto hashOffset = Read<std::int64_t>(header + kCountsOffset + 32);
			if (hashCount <= 0 || hashOffset <= 0) {
				continue;
			}
			std::vector<std::uint8_t> table(static_cast<std::size_t>(hashCount) * 16);
			in.seekg(hashOffset);
			if (!in.read(reinterpret_cast<char*>(table.data()), static_cast<std::streamsize>(table.size()))) {
				error = std::format("{}: hash table is truncated", path.string());
				return false;
			}
			const auto file = static_cast<std::uint32_t>(m_Files.size());
			m_Files.push_back(path);
			for (std::size_t i = 0; i < table.size(); i += 16) {
				const auto key = Read<std::uint64_t>(table.data() + i);
				const auto packed = Read<std::uint64_t>(table.data() + i + 8);
				m_Entries.try_emplace(key, Entry{ file, (packed >> 32) << 7, static_cast<std::uint32_t>((packed >> 1) & 0x3FFFFFFF) });
			}
		}
		return true;
	}

	const Entry* PackageIndex::Find(std::uint64_t key) const {
		const auto it = m_Entries.find(key);
		return it == m_Entries.end() ? nullptr : &it->second;
	}

	bool PackageIndex::Extract(std::uint64_t key, std::vector<std::uint8_t>& out, std::string& error) const {
		out.clear();
		const Entry* entry = Find(key);
		if (!entry) {
			error = std::format("no package entry for key {:016X}", key);
			return false;
		}
		std::ifstream in(m_Files[entry->file], std::ios::binary);
		std::vector<std::uint8_t> data(entry->size + 0x100);
		in.seekg(static_cast<std::streamoff>(entry->offset));
		in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
		const std::size_t got = static_cast<std::size_t>(in.gcount());
		if (got < entry->size) {
			error = std::format("{:016X}: entry runs past the end of {}", key, m_Files[entry->file].string());
			return false;
		}
		data.resize(got);

		std::size_t pos = 0;
		std::size_t consumed = 0;
		while (consumed < entry->size) {
			if (pos + 8 > data.size()) {
				error = std::format("{:016X}: segment header past the entry", key);
				return false;
			}
			const auto count = Read<std::uint32_t>(data.data() + pos);
			if (count == 0 || count > 256) {
				error = std::format("{:016X}: segment with {} blocks at +0x{:X}", key, count, pos);
				return false;
			}
			const std::size_t commandsAt = pos + 8;
			const std::size_t headerSize = count <= 30 ? 128 : 8 + 4 * count;
			if (commandsAt + 4 * count > data.size()) {
				error = std::format("{:016X}: segment commands past the entry", key);
				return false;
			}
			pos += headerSize;
			consumed += headerSize;
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto command = Read<std::uint32_t>(data.data() + commandsAt + 4 * i);
				const std::size_t size = command & 0xFFFFFF;
				const std::uint32_t kind = command >> 24;
				if (pos + size > data.size()) {
					error = std::format("{:016X}: block {} runs past the entry", key, i);
					return false;
				}
				const std::uint8_t* block = data.data() + pos;
				if (kind == 8 || kind == 9) {
					if (size < 4) {
						error = std::format("{:016X}: Oodle block of {} bytes", key, size);
						return false;
					}
					const auto rawSize = Read<std::uint32_t>(block);
					const std::size_t at = out.size();
					out.resize(at + rawSize);
					if (!Oodle::Decompress({ block + 4, size - 4 }, { out.data() + at, rawSize })) {
						error = std::format("{:016X}: Oodle block {} did not decompress", key, i);
						return false;
					}
				}
				else if (kind == 0) {
					out.insert(out.end(), block, block + size);
				}
				else if (kind == 3) {
					error = std::format("{:016X}: LZ4 block (not supported yet)", key);
					return false;
				}
				// Any other kind is padding.
				std::size_t next = pos + size;
				if (i + 1 == count) {
					next = (next + 0x7F) & ~std::size_t(0x7F);
				}
				consumed += next - pos;
				pos = next;
			}
		}
		return true;
	}
}
