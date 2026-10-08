#include "ref_graph.hpp"
#include "xstream.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <format>

namespace MapKit::Zone {
	RefGraph BuildRefGraph(const ZoneTrace& trace, std::span<const std::uint8_t> stream, const XAssetList& list) {
		RefGraph graph;
		const std::size_t count = list.assets.size();
		if (!trace.complete || trace.assets.size() != count + 1 || count == 0) {
			graph.error = "the trace is not complete or does not match the zone's asset list";
			return graph;
		}
		graph.links.resize(count);
		graph.inner.resize(count);
		graph.bytes.resize(count);

		// Every block's position at each asset's start, and at the end of the load: [block][asset].
		std::array<std::vector<std::uint64_t>, kXBlockCount> starts;
		std::array<bool, kXBlockCount> usable{};
		for (std::size_t b = 2; b < kXBlockCount; ++b) {
			starts[b].resize(count + 1);
			for (std::size_t i = 0; i <= count; ++i) {
				starts[b][i] = trace.assets[i].pos[b];
			}
			// Only a block whose position never goes back can say who loaded a position (not true of a
			// preload-family trace's block 11).
			usable[b] = std::ranges::is_sorted(starts[b]) && starts[b][count] > starts[b][0];
		}
		const std::uint64_t tableEnd = trace.assets[0].pos[XBlockVirtual];
		const std::uint64_t tableBegin = tableEnd - 16ull * count;

		std::vector<std::uint32_t> found, inner;
		for (std::size_t i = 0; i < count; ++i) {
			const std::size_t begin = trace.assets[i].offset;
			const std::size_t end = trace.assets[i + 1].offset;
			graph.bytes[i] = end - begin;
			found.clear();
			inner.clear();
			for (std::size_t at = begin; at + 8 <= end; ++at) {
				std::uint64_t stored = 0;
				std::memcpy(&stored, stream.data() + at, 8);
				if (stored < (2ull << 60) + 1 || stored >= kPtrInsert) {
					continue;
				}
				const std::uint64_t value = stored - 1;
				const std::size_t block = static_cast<std::size_t>(value >> 60);
				const std::uint64_t position = value & 0x0FFFFFFFFFFFFFFFull;
				if ((position >> 32) != 0 || block >= kXBlockCount || !usable[block]) {
					continue;
				}
				if (block == XBlockVirtual && position >= tableBegin && position < tableEnd) {
					if ((position - tableBegin) % 16 != 8) {
						continue;
					}
					const auto target = static_cast<std::uint32_t>((position - tableBegin) / 16);
					if (target != i) {
						found.push_back(target);
						++graph.entryLinks;
					}
					continue;
				}
				// The asset whose load reached this position: the last one that started at or before it. It must
				// have started before this one (a stored pointer only reaches data loaded earlier) and the position
				// must be one it loaded, not the gap before the first asset (script strings, the list itself).
				const std::vector<std::uint64_t>& s = starts[block];
				if (position < s[0] || position >= s[i]) {
					continue;
				}
				const auto owner = static_cast<std::size_t>(std::upper_bound(s.begin(), s.begin() + i + 1, position) - s.begin()) - 1;
				if (owner >= i) {
					continue;
				}
				// Its first allocation in the block: the position the block had reached, rounded up to an alignment
				// larger than the gap (so the position is a multiple of it).
				const std::uint64_t gap = position - s[owner];
				const std::uint64_t alignment = position & (~position + 1);
				if (gap == 0 || (gap < 128 && alignment > gap)) {
					found.push_back(static_cast<std::uint32_t>(owner));
					++graph.rootLinks;
				}
				else {
					inner.push_back(static_cast<std::uint32_t>(owner));
					++graph.innerPointers;
				}
			}
			std::ranges::sort(found);
			found.erase(std::unique(found.begin(), found.end()), found.end());
			std::ranges::sort(inner);
			inner.erase(std::unique(inner.begin(), inner.end()), inner.end());
			std::erase_if(inner, [&](std::uint32_t j) { return std::ranges::binary_search(found, j); });
			graph.links[i] = found;
			graph.inner[i] = inner;
		}
		return graph;
	}

	std::uint64_t ByNameReference(std::span<const std::uint8_t> root) {
		for (const std::size_t at : { std::size_t(0), std::size_t(8), std::size_t(16) }) {
			if (at + 8 > root.size()) {
				break;
			}
			std::uint64_t name = 0;
			std::memcpy(&name, root.data() + at, 8);
			if (!(name >> 63)) {
				continue;
			}
			bool rest = true;
			for (std::size_t b = 0; b < root.size() && rest; ++b) {
				rest = (b >= at && b < at + 8) || root[b] == 0;
			}
			return rest ? (name & ~(1ull << 63)) : 0;
		}
		return 0;
	}
}
