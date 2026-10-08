#include "zone_trace.hpp"
#include "asset_loaders.hpp"
#include "xstream.hpp"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>

namespace MapKit::Zone {
	bool ReadZoneTrace(const std::filesystem::path& path, ZoneTrace& out, std::string& error) {
		std::ifstream in(path, std::ios::binary);
		if (!in) {
			error = std::format("cannot open {}", path.string());
			return false;
		}
		TraceFileHeader header{};
		in.read(reinterpret_cast<char*>(&header), sizeof(header));
		if (!in || std::memcmp(header.magic, "MKTR", 4) != 0) {
			error = "not a mapkit zone trace (no MKTR header)";
			return false;
		}
		if (header.version != kTraceVersion || header.recordSize != sizeof(TraceRecord)) {
			error = std::format("trace version {} with {}-byte records; this build reads version {} ({} bytes)",
				header.version, header.recordSize, kTraceVersion, sizeof(TraceRecord));
			return false;
		}
		std::vector<TraceRecord> records(header.recordCount);
		in.read(reinterpret_cast<char*>(records.data()), static_cast<std::streamsize>(records.size() * sizeof(TraceRecord)));
		if (!in) {
			error = std::format("trace is truncated: the header promises {} records", header.recordCount);
			return false;
		}

		out = {};
		out.zone.assign(header.zone, strnlen(header.zone, sizeof(header.zone)));
		out.flags = header.flags;
		out.assetCount = header.assetCount;
		for (std::size_t i = 0; i < records.size(); ++i) {
			const TraceRecord& r = records[i];
			const bool end = r.type == kTraceEndType;
			if (end ? r.index != header.assetCount || i + 1 != records.size() : r.index != i) {
				error = std::format("record {} is out of order (index {}, type 0x{:X})", i, r.index, r.type);
				return false;
			}
			if (r.block >= kXBlockCount) {
				error = std::format("record {} names block {}", i, r.block);
				return false;
			}
			TracedAsset a;
			a.type = r.type;
			a.header = r.header;
			a.offset = static_cast<std::size_t>(r.stream); // aligned by AlignZoneTrace
			a.block = static_cast<int>(r.block);
			std::copy(std::begin(r.pos), std::end(r.pos), a.pos.begin());
			out.assets.push_back(a);
			out.complete = end;
		}
		return true;
	}

	std::string AlignZoneTrace(ZoneTrace& trace, std::span<const std::uint8_t> stream, const XAssetList& list) {
		if (trace.assetCount != list.assets.size()) {
			return std::format("the trace has {} assets, this stream {}: not the same zone build", trace.assetCount,
				list.assets.size());
		}
		if (trace.assets.empty()) {
			return "the trace holds no assets";
		}
		// The client counts every byte read since the load began; the first asset starts at bodyOffset.
		const std::int64_t delta = static_cast<std::int64_t>(list.bodyOffset) - static_cast<std::int64_t>(trace.assets[0].offset);
		for (std::size_t i = 0; i < trace.assets.size(); ++i) {
			TracedAsset& a = trace.assets[i];
			a.offset = static_cast<std::size_t>(static_cast<std::int64_t>(a.offset) + delta);
			if (i < list.assets.size()) {
				const XAssetEntry& e = list.assets[i];
				if (static_cast<std::uint32_t>(e.type) != a.type || e.header != a.header) {
					return std::format("asset {}: the trace has type 0x{:X} header 0x{:X}, the stream 0x{:X} / 0x{:X}", i,
						a.type, a.header, e.type, e.header);
				}
				a.type = e.type;
			}
			if (i > 0 && a.offset < trace.assets[i - 1].offset) {
				return std::format("asset {} starts before asset {} (+0x{:X} < +0x{:X})", i, i - 1, a.offset,
					trace.assets[i - 1].offset);
			}
			if (a.offset > stream.size()) {
				return std::format("asset {} starts at +0x{:X}, past the stream's end (0x{:X})", i, a.offset, stream.size());
			}
		}
		if (trace.complete && trace.assets.back().offset != stream.size()) {
			return std::format("the trace ends at +0x{:X} but the stream is 0x{:X} bytes: the byte count is off",
				trace.assets.back().offset, stream.size());
		}
		return {};
	}

	bool ReplayTracedAsset(const ZoneTrace& trace, std::span<const std::uint8_t> stream, std::size_t index,
		TraceReplay& out) {
		out = {};
		out.index = index;
		if (index + 1 >= trace.assets.size()) {
			out.error = "no record after this asset to check its end against";
			return false;
		}
		const TracedAsset& a = trace.assets[index];
		const TracedAsset& next = trace.assets[index + 1];
		const AssetLoader loader = FindAssetLoader(a.type);
		if (!loader) {
			return false;
		}

		XStream s(stream, 0);
		s.Restore(a.offset, a.block, a.pos);
		if (!loader(s, a.header)) {
			out.error = s.Error();
			return false;
		}
		if (s.Cursor() != next.offset) {
			out.error = std::format("ends at +0x{:X}, the next asset starts at +0x{:X} ({:+} bytes)", s.Cursor(),
				next.offset, static_cast<std::int64_t>(s.Cursor()) - static_cast<std::int64_t>(next.offset));
			return false;
		}
		const auto positions = s.Positions();
		for (std::size_t b = 0; b < kXBlockCount; ++b) {
			if (trace.Comparable(b) && positions[b] != next.pos[b]) {
				out.error = std::format("block {} ends at 0x{:X}, the engine's at 0x{:X}", b, positions[b], next.pos[b]);
				return false;
			}
		}
		if (s.Block() != next.block && trace.Comparable(static_cast<std::size_t>(next.block))) {
			out.error = std::format("ends in block {}, the engine in block {}", s.Block(), next.block);
			return false;
		}
		out.ok = true;
		return true;
	}

	std::size_t TracedAssetOfReference(const ZoneTrace& trace, const XAssetList& list, std::uint64_t stored) {
		if (stored == kPtrNull || stored == kPtrInline || stored == kPtrInsert || trace.assets.empty()) {
			return SIZE_MAX;
		}
		const std::uint64_t value = stored - 1;
		const std::uint64_t offset = value & 0x0FFFFFFFFFFFFFFFull;
		const std::uint64_t tableEnd = trace.assets[0].pos[XBlockVirtual];
		const std::uint64_t tableBegin = tableEnd - 16ull * list.assets.size();
		if ((value >> 60) != XBlockVirtual || offset < tableBegin || offset >= tableEnd) {
			return SIZE_MAX;
		}
		return static_cast<std::size_t>((offset - tableBegin) / 16);
	}

	std::uint64_t TracedAssetName(const ZoneTrace& trace, std::span<const std::uint8_t> stream, std::size_t asset) {
		if (asset >= trace.assets.size()) {
			return 0;
		}
		const std::size_t at = trace.assets[asset].offset + XAssetNameOffset(trace.assets[asset].type);
		if (at + 8 > stream.size()) {
			return 0;
		}
		std::uint64_t name = 0;
		std::memcpy(&name, stream.data() + at, 8);
		return name & ~(1ull << 63);
	}

	std::size_t TracedAssetOfData(const ZoneTrace& trace, std::uint64_t stored) {
		const std::uint64_t value = stored - 1;
		const int block = static_cast<int>(value >> 60);
		const std::uint64_t target = value & 0x0FFFFFFFFFFFFFFFull;
		if (stored == 0 || stored == ~0ull || block <= 0 || block >= static_cast<int>(kXBlockCount)) {
			return SIZE_MAX;
		}
		const auto after = std::upper_bound(trace.assets.begin(), trace.assets.end(), target,
			[block](std::uint64_t position, const TracedAsset& asset) { return position < asset.pos[block]; });
		return after == trace.assets.begin() ? SIZE_MAX : static_cast<std::size_t>(after - trace.assets.begin() - 1);
	}

	bool ResolveStoredData(const ZoneTrace& trace, std::span<const std::uint8_t> stream, std::uint64_t stored,
		std::size_t size, std::vector<std::uint8_t>& out, std::string& error) {
		const std::uint64_t value = stored - 1;
		const int block = static_cast<int>(value >> 60);
		const std::uint64_t target = value & 0x0FFFFFFFFFFFFFFFull;
		if (stored == 0 || stored == ~0ull || block <= 0 || block >= static_cast<int>(kXBlockCount) || !XBlockIsStored(block)) {
			error = std::format("0x{:X} is not a reference into a stored block", stored);
			return false;
		}
		// The last asset that started at or before the target in that block, among those that moved it.
		std::size_t owner = SIZE_MAX;
		for (std::size_t i = 0; i + 1 < trace.assets.size(); ++i) {
			if (trace.assets[i].pos[block] <= target && trace.assets[i + 1].pos[block] > target) {
				owner = i;
				break;
			}
		}
		if (owner == SIZE_MAX) {
			error = std::format("no asset of the trace loaded block {} +0x{:X}", block, target);
			return false;
		}
		const TracedAsset& a = trace.assets[owner];
		const AssetLoader loader = FindAssetLoader(a.type);
		if (!loader) {
			error = std::format("asset {} ({}) stored it, and mapkit has no loader for that type", owner, XAssetTypeName(a.type));
			return false;
		}
		std::vector<XStream::LoadRecord> loads;
		XStream s(stream, 0);
		s.Restore(a.offset, a.block, a.pos);
		s.LogLoads(&loads);
		if (!loader(s, a.header)) {
			error = std::format("asset {} did not decode: {}", owner, s.Error());
			return false;
		}
		for (const XStream::LoadRecord& load : loads) {
			if (load.block == block && load.pos <= target && target + size <= load.pos + load.size) {
				const std::size_t at = load.cursor + static_cast<std::size_t>(target - load.pos);
				out.assign(stream.begin() + at, stream.begin() + at + size);
				return true;
			}
		}
		error = std::format("asset {} ({}) moved block {} past +0x{:X} without one load covering {} bytes there", owner,
			XAssetTypeName(a.type), block, target, size);
		return false;
	}
}
