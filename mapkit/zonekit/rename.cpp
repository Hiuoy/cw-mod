#include "rename.hpp"
#include "hash.hpp"

#include <cstring>

namespace MapKit::Zone {
	namespace {
		// "\x80GSC\r\n\0" + VM 0x38 (client/scripting/cw_gsc.hpp T9GSCOBJ).
		constexpr std::uint8_t kGscMagic[8] = { 0x80, 0x47, 0x53, 0x43, 0x0D, 0x0A, 0x00, 0x38 };
		constexpr std::size_t kGscHeaderSize = 0x58;
		constexpr std::size_t kGscFileSizeOffset = 0x48;

		template <typename T>
		std::size_t ReplaceValue(std::span<std::uint8_t> data, T from, T to) {
			if (data.size() < sizeof(T) || from == to) {
				return 0;
			}
			std::uint8_t first;
			std::memcpy(&first, &from, 1);
			std::size_t count = 0;
			std::uint8_t* p = data.data();
			std::uint8_t* const last = data.data() + data.size() - sizeof(T);
			while (p <= last) {
				p = static_cast<std::uint8_t*>(std::memchr(p, first, static_cast<std::size_t>(last - p) + 1));
				if (!p) {
					break;
				}
				if (std::memcmp(p, &from, sizeof(T)) == 0) {
					std::memcpy(p, &to, sizeof(T));
					++count;
					p += sizeof(T);
				}
				else {
					++p;
				}
			}
			return count;
		}
	}

	std::size_t ReplaceValue64(std::span<std::uint8_t> data, std::uint64_t from, std::uint64_t to) {
		return ReplaceValue(data, from, to);
	}

	std::size_t ReplaceValue32(std::span<std::uint8_t> data, std::uint32_t from, std::uint32_t to) {
		return ReplaceValue(data, from, to);
	}

	std::vector<std::pair<std::size_t, std::size_t>> FindGscObjects(std::span<const std::uint8_t> stream) {
		std::vector<std::pair<std::size_t, std::size_t>> objects;
		std::size_t pos = 0;
		while (pos + kGscHeaderSize <= stream.size()) {
			const void* hit = std::memchr(stream.data() + pos, kGscMagic[0], stream.size() - kGscHeaderSize - pos + 1);
			if (!hit) {
				break;
			}
			pos = static_cast<std::size_t>(static_cast<const std::uint8_t*>(hit) - stream.data());
			if (std::memcmp(stream.data() + pos, kGscMagic, sizeof(kGscMagic)) != 0) {
				++pos;
				continue;
			}
			std::uint32_t size;
			std::memcpy(&size, stream.data() + pos + kGscFileSizeOffset, sizeof(size));
			if (size < kGscHeaderSize || size > stream.size() - pos) {
				++pos;
				continue;
			}
			objects.emplace_back(pos, size);
			pos += size;
		}
		return objects;
	}

	std::vector<HashRewrite> RenameMapInStream(std::vector<std::uint8_t>& stream, std::string_view from, std::string_view to) {
		const std::string_view prefix = from.substr(0, from.find('_'));
		const std::string_view newPrefix = to.substr(0, to.find('_'));
		std::vector<HashRewrite> report;

		auto rewrite64 = [&](const std::string& oldName, const std::string& newName) {
			HashRewrite entry{ oldName, newName, 64 };
			entry.count = ReplaceValue64(stream, HashName(oldName), HashName(newName));
			report.push_back(std::move(entry));
		};
		const std::string f(from), t(to), p(prefix), np(newPrefix);
		rewrite64("maps/" + p + "/" + f + ".d3dbsp", "maps/" + np + "/" + t + ".d3dbsp");
		rewrite64("scripts/" + p + "/" + f + ".gsc", "scripts/" + np + "/" + t + ".gsc");
		rewrite64("scripts/" + p + "/" + f + ".csc", "scripts/" + np + "/" + t + ".csc");
		rewrite64(f, t);

		HashRewrite ns{ f, t, 32 };
		for (const auto& [offset, size] : FindGscObjects(stream)) {
			ns.count += ReplaceValue32(std::span<std::uint8_t>(stream.data() + offset, size), HashScript32(from), HashScript32(to));
		}
		report.push_back(std::move(ns));
		return report;
	}
}
