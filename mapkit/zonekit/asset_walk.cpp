#include "asset_walk.hpp"
#include "asset_loaders.hpp"
#include "xasset_list.hpp"
#include "xstream.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace MapKit::Zone {
	WalkResult WalkStream(std::span<const std::uint8_t> stream, std::vector<XStream::RootRecord>* roots) {
		WalkResult result;
		std::array<std::uint8_t, kXAssetListSize> list{};
		if (stream.size() < list.size()) {
			result.error = "stream is smaller than the asset list header";
			return result;
		}
		// DB_LoadXFile_Internal (0x7FF72769FC00) reads the 40-byte XAssetList outside any block, then pushes
		// block 4 around Load_ScriptStringList (0x7FF71E7E8A80), the XAsset array and every asset.
		std::memcpy(list.data(), stream.data(), list.size());
		XStream s(stream, list.size());
		s.LogRoots(roots);
		s.Push(XBlockVirtual);

		const auto stringCount = Get<std::uint32_t>(list, 0);
		if (Get<std::uint64_t>(list, 8)) {
			s.Alloc(8);
			std::vector<std::uint64_t> strings(stringCount);
			s.Load(strings.data(), strings.size() * 8);
			for (std::uint64_t pointer : strings) {
				LoadXString(s, pointer, "script string");
			}
		}
		s.Push(XBlockRuntime);
		if (Get<std::uint64_t>(list, 16)) {
			s.Alloc(4);
			s.Load(nullptr, stringCount * 4ull);
		}
		s.Pop();

		const auto assetCount = Get<std::uint64_t>(list, 24);
		result.assetCount = static_cast<std::size_t>(assetCount);
		if (Get<std::uint64_t>(list, 32)) {
			s.Alloc(8);
			if ((s.Size() - s.Cursor()) / 16 < assetCount) {
				result.error = "asset array is truncated";
				return result;
			}
			std::vector<std::uint8_t> entries(static_cast<std::size_t>(assetCount) * 16);
			s.Load(entries.data(), entries.size());
			result.bodyOffset = s.Cursor();
			result.bodyPositions = s.Positions();

			// Load_XAsset(0, entry) -> Load_XAssetHeader (0x7FF71E7F75A0) -> Load_<Type>Asset(0, &entry.header).
			for (std::size_t i = 0; i < assetCount && !s.Failed(); ++i) {
				const auto type = Get<std::uint64_t>(entries, i * 16);
				const auto header = Get<std::uint64_t>(entries, i * 16 + 8);
				const AssetLoader loader = FindAssetLoader(type);
				// A type mapkit only writes as a by-name reference (lighting, say) walks through LoadAsset, which
				// accepts exactly that and fails on real content.
				const bool referenceOnly = !loader && IsReferenceRootType(type);
				if (!loader && !referenceOnly) {
					result.blockedType = type;
					break;
				}
				const std::size_t begin = s.Cursor();
				const auto positions = s.Positions();
				s.SetRootType(type);
				if (!(loader ? loader(s, header) : LoadAsset(s, type, header, "asset"))) {
					s.Fail(std::format("asset {} ({}) failed", i, XAssetTypeName(type)));
					break;
				}
				result.assets.push_back({ type, begin, s.Cursor(), positions });
			}
		}
		s.Pop();

		result.cursor = s.Cursor();
		result.highWater = s.HighWater();
		result.references = s.References();
		result.rootCopyBytes = s.RootCopyBytes();
		result.preloadBlock1 = s.PreloadBlock1();
		result.error = s.Error();
		if (result.error.empty() && result.blockedType == ~0ull) {
			if (s.Cursor() != s.Size()) {
				result.error = std::format("every asset walked but the stream has 0x{:X} bytes left", s.Size() - s.Cursor());
			}
			else if (s.Depth() != 0) {
				result.error = std::format("stream position stack left at depth {}", s.Depth());
			}
			else {
				result.complete = true;
			}
		}
		return result;
	}

	std::array<std::optional<std::uint64_t>, kXBlockCount> ExpectedBlockSizes(const WalkResult& walk) {
		std::array<std::optional<std::uint64_t>, kXBlockCount> out;
		for (std::size_t i = 0; i < kXBlockCount; ++i) {
			out[i] = walk.highWater[i];
		}
		out[XBlockTemp] = walk.highWater[XBlockTemp] + kXAssetListSize;
		out[1] = 208 + walk.rootCopyBytes;
		out[11] = std::nullopt;
		return out;
	}

	std::string CompareBlockSizes(const WalkResult& walk, const std::array<std::uint64_t, kXBlockCount>& declared) {
		const auto expected = ExpectedBlockSizes(walk);
		std::string out;
		for (std::size_t i = 0; i < kXBlockCount; ++i) {
			if (expected[i] && *expected[i] != declared[i]) {
				out += std::format(" [{}] expected 0x{:X} header 0x{:X}", i, *expected[i], declared[i]);
			}
		}
		return out;
	}
}
