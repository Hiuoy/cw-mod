#include "zone_writer.hpp"
#include "asset_loaders.hpp"
#include "xasset_list.hpp"
#include "xstream.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <format>
#include <unordered_map>

namespace MapKit::Zone {
	ZoneAsset CopiedAsset(std::uint64_t type, std::string label, std::vector<std::uint8_t> bytes,
		std::vector<CopiedString> strings, std::vector<CopiedLink> links, std::vector<CopiedPointer> pointers) {
		ZoneAsset asset;
		asset.type = type;
		asset.label = std::move(label);
		for (const CopiedString& string : strings) {
			if (std::ranges::find(asset.scriptStrings, string.text) == asset.scriptStrings.end()) {
				asset.scriptStrings.push_back(string.text);
			}
		}
		asset.encode = [type, bytes = std::move(bytes), strings = std::move(strings), links = std::move(links),
			pointers = std::move(pointers)](XWriter& w) mutable {
			for (const CopiedString& string : strings) {
				const std::uint32_t index = w.ScriptString(string.text);
				if (string.at + 4 <= bytes.size()) {
					std::memcpy(bytes.data() + string.at, &index, 4);
				}
			}
			// A link or pointer left pointing into the other zone fails the zone's walk (WriteZone), so it is not written.
			std::size_t unlinked = 0;
			for (const CopiedLink& link : links) {
				const auto entry = w.AssetEntry(link.type, link.name);
				if (entry && link.at + 8 <= bytes.size()) {
					std::memcpy(bytes.data() + link.at, &*entry, 8);
				}
				else {
					++unlinked;
				}
			}
			if (unlinked) {
				std::fprintf(stderr, "  copied %s: %zu link(s) have no entry in this zone\n", std::string(XAssetTypeName(type)).c_str(),
					unlinked);
			}
			// Read it back from where it lands, so the positions after it are the engine's (anything written later
			// may point into data by position).
			XStream s(bytes, 0);
			s.Restore(0, w.Block(), w.Positions());
			s.Detach();
			std::vector<XStream::LoadRecord> loads;
			if (!pointers.empty()) {
				s.LogLoads(&loads);
			}
			const AssetLoader loader = FindAssetLoader(type);
			const bool advanced = loader && loader(s, kPtrInline) && s.Cursor() == bytes.size();
			// Each pointer into the copy's own data: where the byte it points at was loaded this time. The read-back does
			// not check them (detached), so their old values could not change where anything lands.
			std::size_t unplaced = 0;
			for (const CopiedPointer& pointer : pointers) {
				const auto record = std::ranges::find_if(loads, [&](const XStream::LoadRecord& r) {
					return pointer.target >= r.cursor && pointer.target < r.cursor + r.size;
				});
				if (!advanced || record == loads.end() || pointer.at + 8 > bytes.size()) {
					++unplaced;
					continue;
				}
				const std::uint64_t value = XWriter::Reference(record->block, record->pos + (pointer.target - record->cursor));
				std::memcpy(bytes.data() + pointer.at, &value, 8);
			}
			if (unplaced) {
				std::fprintf(stderr, "  copied %s: %zu pointer(s) into its own data could not be placed\n",
					std::string(XAssetTypeName(type)).c_str(), unplaced);
			}
			w.AppendRaw(bytes);
			if (advanced) {
				w.SetPositions(s.Positions());
			}
		};
		return asset;
	}

	std::vector<std::uint8_t> BuildZoneStream(std::span<const std::optional<std::string>> scriptStrings,
		std::span<const ZoneAsset> assets, std::string& error) {
		error.clear();
		// A zone without a table has a null one, and any nonzero index then reads through null at link time
		// (DB_ConvertZoneScriptString 0x7FF72966F1E0; the D2 clip map's 0xF00 crash).
		std::vector<std::optional<std::string>> strings(scriptStrings.begin(), scriptStrings.end());
		std::unordered_map<std::string, std::uint32_t> indices;
		for (std::size_t i = 0; i < strings.size(); ++i) {
			if (strings[i]) {
				indices.try_emplace(*strings[i], static_cast<std::uint32_t>(i));
			}
		}
		for (const ZoneAsset& asset : assets) {
			for (const std::string& text : asset.scriptStrings) {
				if (indices.contains(text)) {
					continue;
				}
				if (strings.empty()) {
					strings.emplace_back(std::nullopt);
				}
				indices.emplace(text, static_cast<std::uint32_t>(strings.size()));
				strings.emplace_back(text);
			}
		}

		// The 40-byte XAssetList, read outside any block (DB_LoadXFile_Internal 0x7FF72769FC00):
		// { u32 stringCount; char** strings; u32* hashes; u64 assetCount; XAsset* assets }.
		std::array<std::uint8_t, kXAssetListSize> list{};
		const bool haveStrings = !strings.empty();
		PutAt(std::span<std::uint8_t>(list), 0, static_cast<std::uint32_t>(strings.size()));
		PutAt(std::span<std::uint8_t>(list), 8, haveStrings ? kPtrInline : kPtrNull);
		PutAt(std::span<std::uint8_t>(list), 16, haveStrings ? kPtrInline : kPtrNull);
		PutAt(std::span<std::uint8_t>(list), 24, static_cast<std::uint64_t>(assets.size()));
		PutAt(std::span<std::uint8_t>(list), 32, assets.empty() ? kPtrNull : kPtrInline);

		XWriter w(XBlockTemp);
		w.Bytes().assign(list.begin(), list.end());
		w.SetScriptStrings(std::move(indices));
		w.Push(XBlockVirtual);

		// Load_ScriptStringList (0x7FF71E7E8A80): the pointer array, each string inline, then the hashes
		// in block 2 (zero-filled at runtime, nothing stored).
		if (haveStrings) {
			w.Alloc(8);
			for (const auto& text : strings) {
				w.Put(text ? kPtrInline : kPtrNull);
			}
			for (const auto& text : strings) {
				if (text) {
					w.Alloc(1);
					w.WriteString(*text);
				}
			}
			w.Push(XBlockRuntime);
			w.Alloc(4);
			std::vector<std::uint8_t> hashes(4 * strings.size());
			w.Write(hashes.data(), hashes.size());
			w.Pop();
		}

		// The XAsset array (16 B each: u64 type, header pointer), then every asset in order.
		if (!assets.empty()) {
			const std::uint64_t arrayPos = w.Alloc(8);
			std::vector<std::pair<std::uint64_t, std::uint64_t>> links;
			for (const ZoneAsset& asset : assets) {
				links.emplace_back(asset.linkName ? asset.type : ~0ull, asset.linkName);
			}
			w.SetAssetEntries(arrayPos, std::move(links));
			for (const ZoneAsset& asset : assets) {
				w.Put(asset.type);
				w.Put(kPtrInline);
			}
			for (std::size_t i = 0; i < assets.size(); ++i) {
				w.SetCurrentAsset(i);
				assets[i].encode(w);
			}
			w.SetCurrentAsset(SIZE_MAX);
		}
		w.Pop();
		if (const auto& forward = w.ForwardLinks(); !forward.empty()) {
			error = std::format("{} link(s) to an asset written after the one linking it (the engine resolves a link while the "
				"linking asset loads):", forward.size());
			for (std::size_t i = 0; i < forward.size() && i < 8; ++i) {
				const XWriter::ForwardLink& link = forward[i];
				error += std::format(" asset {} ({}) links {} {:016X} at asset {};", link.from,
					assets[link.from].label.empty() ? std::string(XAssetTypeName(assets[link.from].type)) : assets[link.from].label,
					XAssetTypeName(link.type), link.name, link.to);
			}
			return {};
		}
		return std::move(w.Bytes());
	}

	bool WriteZone(FastFileHeader header, std::string_view zoneName, std::span<const std::uint8_t> stream,
		std::vector<std::uint8_t>& file, ZoneWriteResult& result) {
		result = {};
		result.walk = WalkStream(stream);
		if (!result.walk.complete) {
			if (!result.walk.error.empty()) {
				result.error = "the written zone does not read back: " + result.walk.error;
				// Where it went wrong: the asset that failed follows the last ones read.
				const auto& walked = result.walk.assets;
				result.error += std::format(" (in asset {} of {}", walked.size(), result.walk.assetCount);
				for (std::size_t i = walked.size() > 3 ? walked.size() - 3 : 0; i < walked.size(); ++i) {
					result.error += std::format("; after {} {} at +0x{:X}..+0x{:X}", i, XAssetTypeName(walked[i].type),
						walked[i].begin, walked[i].end);
				}
				result.error += ")";
			}
			else if (result.walk.blockedType != ~0ull) {
				result.error = std::format("mapkit has no loader for {} to check the written zone with",
					XAssetTypeName(result.walk.blockedType));
			}
			else {
				result.error = "the written zone does not read back";
			}
			return false;
		}

		const auto expected = ExpectedBlockSizes(result.walk);
		for (std::size_t i = 0; i < kXBlockCount; ++i) {
			result.blockSizes[i] = expected[i].value_or(0);
		}
		// Block 1 holds the lobby preload's asset roots and techset saves (XStream::PreloadRoot). The retail rule gives a
		// material 352 B there and the preload takes 448, so a zone of many materials can outgrow it, unchecked, into
		// block 2 (docs/mapkit-plan.md, "Third build": zm_debug's crash at launch).
		result.block1Rule = result.blockSizes[1];
		result.blockSizes[1] = std::max(result.blockSizes[1], result.walk.preloadBlock1);
		// Block 11 holds the lobby preload's pointer fixups (8 B per pointer field; the load flags 0x6A0 use
		// Load_XAsset_Preload). Not modelled yet, so it gets a size no zone can outgrow: the 208 every zone
		// starts with, plus one fixup for every 8 stream bytes.
		result.blockSizes[11] = 208 + ((stream.size() + 7) & ~std::size_t(7));

		header.SetZoneName(zoneName);
		header.SetBlockSizes(result.blockSizes);
		HeaderSection& second = header.Get(HeaderTag::BlockSizes2, 104);
		std::fill(second.data.begin(), second.data.end(), std::uint8_t(0));
		std::string error;
		if (!WriteFastFile(header, stream, file, error)) {
			result.error = error;
			return false;
		}
		return true;
	}
}
