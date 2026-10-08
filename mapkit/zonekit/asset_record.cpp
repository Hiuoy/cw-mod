// The splice writer for recorded assets (asset_record.hpp), first written for the gfx_map (gfx_world.cpp).
#include "asset_record.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace MapKit::Zone {
	const RecordSubtree* AssetRecord::Subtree(std::string_view subtreeName) const {
		for (const RecordSubtree& subtree : subtrees) {
			if (subtree.name == subtreeName) {
				return &subtree;
			}
		}
		return nullptr;
	}

	RecordTarget ResolveRecordRef(const AssetRecord& record, const RecordRef& ref, std::uint64_t assetArrayPos,
		std::size_t assetCount) {
		RecordTarget target;
		const std::uint64_t value = ref.stored - 1;
		const int block = static_cast<int>(value >> 60);
		const std::uint64_t offset = value & 0x0FFFFFFFFFFFFFFFull;
		if (block == XBlockVirtual && offset >= assetArrayPos && offset < assetArrayPos + 16 * assetCount
			&& (offset - assetArrayPos) % 16 == 8) {
			target.kind = RecordTarget::Asset;
			target.asset = static_cast<std::size_t>((offset - assetArrayPos) / 16);
			return target;
		}
		for (std::size_t i = 0; i < record.chunks.size(); ++i) {
			const RecordChunk& chunk = record.chunks[i];
			if (chunk.block == block && offset >= chunk.pos && offset < chunk.pos + std::max<std::uint64_t>(chunk.size, 1)) {
				target.kind = RecordTarget::Internal;
				target.chunk = i;
				target.offset = offset - chunk.pos;
				return target;
			}
		}
		return target;
	}

	namespace {
		bool Cut(const RecordSplice& splice, std::size_t at) {
			for (const RecordCut& cut : splice.cuts) {
				const RecordSubtree* subtree = splice.record->Subtree(cut.name);
				if (subtree && at >= subtree->begin && at < subtree->end) {
					return true;
				}
			}
			return false;
		}

		// The asset an asset link of the copy goes to: the retail one, or a relink's.
		RecordLink LinkOf(const RecordSplice& splice, const RecordRef& ref, std::size_t asset) {
			const std::uint64_t type = splice.assets[asset].type;
			for (const RecordSplice::Relink& relink : splice.relinks) {
				if (relink.type != type) {
					continue;
				}
				for (const RecordSubtree& subtree : splice.record->subtrees) {
					if (subtree.name == relink.subtree && ref.at >= subtree.begin && ref.at < subtree.end) {
						return { type, relink.name };
					}
				}
			}
			return { type, splice.assetNames[asset] };
		}

		// The block position of a stream byte of the record (nullopt when no chunk of it holds that byte).
		std::optional<std::uint64_t> BlockPosition(const AssetRecord& record, std::size_t at) {
			for (const RecordChunk& chunk : record.chunks) {
				if (chunk.at != kNoStream && at >= chunk.at && at < chunk.at + chunk.size) {
					return chunk.pos + (at - chunk.at);
				}
			}
			return std::nullopt;
		}
	}

	bool RecordLinks(const RecordSplice& splice, std::vector<RecordLink>& links, std::vector<std::string>& strings,
		std::string& error, std::vector<std::uint64_t>* shared) {
		const AssetRecord& record = *splice.record;
		for (const RecordCut& cut : splice.cuts) {
			if (!record.Subtree(cut.name)) {
				error = std::format("no sub-array '{}' to cut", cut.name);
				return false;
			}
		}
		// Nested assets stored inline in a sub-array the copy cuts go with it; any other one cannot travel.
		std::size_t inlined = record.inlineAssets - std::min(record.inlineAssets, record.inlineAssetFields.size());
		for (const std::size_t field : record.inlineAssetFields) {
			inlined += field == kNoStream || !Cut(splice, field);
		}
		if (inlined) {
			error = std::format("{} nested assets are stored inline: their references are not recorded", inlined);
			return false;
		}
		for (const RecordRef& ref : record.refs) {
			if (Cut(splice, ref.at)) {
				continue;
			}
			const RecordTarget target = ResolveRecordRef(record, ref, splice.assetArrayPos, splice.assets.size());
			if (target.kind == RecordTarget::External) {
				if (shared && ref.assetType == ~0ull) {
					shared->push_back(ref.stored);
					continue;
				}
				error = std::format("the pointer at stream +0x{:X} points into data of another asset (0x{:X})", ref.at, ref.stored);
				return false;
			}
			if (target.kind == RecordTarget::Internal) {
				// An asset field may link a nested asset the asset stored inline earlier, through its DB_InsertPointer slot
				// (Ctx::Nested records the slot: block 4, no stream bytes); the copy re-points it to the slot's copy.
				const RecordChunk& chunk = record.chunks[target.chunk];
				const bool slot = chunk.block == XBlockVirtual && chunk.at == kNoStream;
				if (ref.assetType != ~0ull && !slot) {
					error = std::format("the asset field at stream +0x{:X} points into the asset itself", ref.at);
					return false;
				}
				continue;
			}
			const std::uint64_t type = splice.assets[target.asset].type;
			if (ref.assetType != ~0ull && ref.assetType != type) {
				error = std::format("the {} field at stream +0x{:X} links to a {}", XAssetTypeName(ref.assetType), ref.at,
					XAssetTypeName(type));
				return false;
			}
			const RecordLink link = LinkOf(splice, ref, target.asset);
			if (std::find(links.begin(), links.end(), link) == links.end()) {
				links.push_back(link);
			}
		}
		// Every link the copied bytes hold must be one the reader recorded: a missed one (it loads no stream bytes, so the
		// reader still reads the zone exactly) would be copied as the base map's stored value, which leads anywhere in
		// the new zone. A pointer field sits at an 8-aligned block position: the stream has no padding, so a match at any
		// other one is data that happens to read as a link (zero runs and a 0x40 byte in texture blocks: 26 of
		// zm_silver's images, 2026-10-01).
		std::vector<std::size_t> recorded;
		for (const RecordRef& ref : record.refs) {
			recorded.push_back(ref.at);
		}
		std::sort(recorded.begin(), recorded.end());
		for (const std::size_t at : FindAssetLinks(splice.stream, record.begin, record.end, splice.assetArrayPos,
				splice.assets.size())) {
			if (const auto pos = BlockPosition(record, at); pos && *pos % 8) {
				continue;
			}
			if (!Cut(splice, at) && !std::binary_search(recorded.begin(), recorded.end(), at)) {
				std::uint64_t stored = 0;
				std::memcpy(&stored, splice.stream.data() + at, 8);
				const std::size_t asset = static_cast<std::size_t>((((stored - 1) & 0x0FFFFFFFFFFFFFFFull) - splice.assetArrayPos) / 16);
				error = std::format("stream +0x{:X} (asset +0x{:X}) links to asset {} ({}), a field the reader does not record", at,
					at - record.begin, asset, XAssetTypeName(splice.assets[asset].type));
				return false;
			}
		}
		for (const RecordString& string : record.strings) {
			if (Cut(splice, string.at) || !string.index) {
				continue;
			}
			if (string.index >= splice.strings.size() || !splice.strings[string.index]) {
				error = std::format("script string {} (stream +0x{:X}) is not in the zone's table", string.index, string.at);
				return false;
			}
			if (std::find(strings.begin(), strings.end(), *splice.strings[string.index]) == strings.end()) {
				strings.push_back(*splice.strings[string.index]);
			}
		}
		return true;
	}

	bool EncodeRecordedAsset(XWriter& w, const RecordSplice& splice, const RecordReader& read, std::string& error) {
		const AssetRecord& record = *splice.record;
		if (record.rootAt == kNoStream || record.begin != record.rootAt) {
			error = "the root is not stored at the asset's start";
			return false;
		}

		// The cut ranges, in stream order, and the chunks they hold.
		std::vector<const RecordSubtree*> cuts;
		for (const RecordCut& cut : splice.cuts) {
			const RecordSubtree* subtree = record.Subtree(cut.name);
			if (!subtree) {
				error = std::format("no sub-array '{}' to cut", cut.name);
				return false;
			}
			cuts.push_back(subtree);
		}
		std::sort(cuts.begin(), cuts.end(), [](auto* a, auto* b) { return a->begin < b->begin; });
		auto newAt = [&](std::size_t at) {
			std::size_t removed = 0;
			for (const RecordSubtree* cut : cuts) {
				if (cut->end <= at) {
					removed += cut->end - cut->begin;
				}
			}
			return at - record.begin - removed;
		};

		// 1. The bytes, cuts taken out.
		std::vector<std::uint8_t> bytes;
		std::size_t at = record.begin;
		for (const RecordSubtree* cut : cuts) {
			if (cut->begin < at) {
				continue; // nested in an earlier cut
			}
			bytes.insert(bytes.end(), splice.stream.begin() + at, splice.stream.begin() + cut->begin);
			at = cut->end;
		}
		bytes.insert(bytes.end(), splice.stream.begin() + at, splice.stream.begin() + record.end);
		auto put64 = [&](std::size_t streamAt, std::uint64_t value) { PutAt(std::span<std::uint8_t>(bytes), newAt(streamAt), value); };
		auto put32 = [&](std::size_t streamAt, std::uint32_t value) { PutAt(std::span<std::uint8_t>(bytes), newAt(streamAt), value); };

		// 2. Cut pointers null, their counts zero.
		for (const RecordCut& cut : splice.cuts) {
			const RecordSubtree* subtree = record.Subtree(cut.name);
			if (subtree->field == kNoStream || Cut(splice, subtree->field)) {
				continue;
			}
			put64(subtree->field, kPtrNull);
			for (const auto& [offset, size] : cut.counts) {
				const std::size_t field = record.rootAt + offset;
				std::vector<std::uint8_t> zero(size);
				std::copy(zero.begin(), zero.end(), bytes.begin() + static_cast<std::ptrdiff_t>(newAt(field)));
			}
		}
		for (const auto& [offset, value] : splice.rootEdits) {
			put64(record.rootAt + offset, value);
		}
		for (const auto& [streamAt, value] : splice.edits) {
			if (streamAt < record.begin || streamAt + value.size() > record.end) {
				error = std::format("an edit at stream +0x{:X} lies outside the asset", streamAt);
				return false;
			}
			if (!Cut(splice, streamAt)) {
				std::copy(value.begin(), value.end(), bytes.begin() + static_cast<std::ptrdiff_t>(newAt(streamAt)));
			}
		}

		// 3. Links into the writer's XAsset array, strings into its table.
		for (const RecordRef& ref : record.refs) {
			if (Cut(splice, ref.at)) {
				continue;
			}
			const RecordTarget target = ResolveRecordRef(record, ref, splice.assetArrayPos, splice.assets.size());
			if (target.kind != RecordTarget::Asset) {
				continue;
			}
			const RecordLink link = LinkOf(splice, ref, target.asset);
			const auto entry = w.AssetEntry(link.type, link.name);
			if (!entry) {
				error = std::format("the zone holds no {} {:016X} to link the copy to", XAssetTypeName(link.type), link.name);
				return false;
			}
			put64(ref.at, *entry);
		}
		for (const RecordString& string : record.strings) {
			if (Cut(splice, string.at) || !string.index) {
				continue;
			}
			put32(string.at, w.ScriptString(*splice.strings[string.index]));
		}

		// 4. Read the copy back from where the writer puts it: where each chunk lands now. Chunks of cut subtrees
		// are gone, the rest keep their order.
		const auto start = w.Positions();
		const int startBlock = w.Block();
		AssetRecord copy;
		{
			XStream s(bytes, 0);
			s.Restore(0, startBlock, start);
			s.Detach(); // references into the zone written so far cannot be checked from here
			if (!read(s, kPtrInline, copy) || s.Cursor() != bytes.size()) {
				error = std::format("the copy does not read back: {}", s.Failed() ? s.Error() : "it ends early");
				return false;
			}
		}
		std::vector<std::size_t> chunkMap(record.chunks.size(), SIZE_MAX);
		{
			std::size_t next = 0;
			for (std::size_t i = 0; i < record.chunks.size(); ++i) {
				bool cutChunk = false;
				for (const RecordSubtree* cut : cuts) {
					cutChunk |= i >= cut->firstChunk && i < cut->endChunk;
				}
				if (!cutChunk) {
					chunkMap[i] = next++;
				}
			}
			if (next != copy.chunks.size()) {
				error = std::format("the copy loads {} pieces of data, the retail one {} (after cuts)", copy.chunks.size(), next);
				return false;
			}
		}

		// 5. Pointers into the asset itself follow their data; pointers into data it shares with an asset copied earlier go
		// where that copy's data landed.
		for (const RecordRef& ref : record.refs) {
			if (Cut(splice, ref.at)) {
				continue;
			}
			const RecordTarget target = ResolveRecordRef(record, ref, splice.assetArrayPos, splice.assets.size());
			if (target.kind == RecordTarget::External) {
				const auto moved = splice.shared ? splice.shared(ref.stored) : std::nullopt;
				if (!moved) {
					error = std::format("the pointer at stream +0x{:X} shares data (0x{:X}) with an asset the zone holds no copy of",
						ref.at, ref.stored);
					return false;
				}
				put64(ref.at, *moved);
				continue;
			}
			if (target.kind != RecordTarget::Internal) {
				continue;
			}
			const std::size_t moved = chunkMap[target.chunk];
			if (moved == SIZE_MAX) {
				error = std::format("the pointer at stream +0x{:X} points into a cut sub-array", ref.at);
				return false;
			}
			const RecordChunk& chunk = copy.chunks[moved];
			put64(ref.at, XWriter::Reference(chunk.block, chunk.pos + target.offset));
		}
		if (splice.landings) {
			for (std::size_t i = 0; i < record.chunks.size(); ++i) {
				if (chunkMap[i] != SIZE_MAX) {
					const RecordChunk& from = record.chunks[i];
					const RecordChunk& to = copy.chunks[chunkMap[i]];
					splice.landings->push_back({ from.block, from.pos, from.size, to.block, to.pos });
				}
			}
		}

		w.AppendRaw(bytes);
		auto end = start;
		{
			XStream s(bytes, 0);
			s.Restore(0, startBlock, start);
			s.Detach();
			AssetRecord again;
			read(s, kPtrInline, again);
			end = s.Positions();
		}
		w.SetPositions(end);
		return true;
	}
}
