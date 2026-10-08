#include "material_writer.hpp"
#include "asset_loaders.hpp"
#include "xstream.hpp"
#include "zone.hpp"

#include <algorithm>
#include <cstring>
#include <format>

namespace MapKit::Zone {
	namespace {
		constexpr std::size_t kImageRootSize = 208;
		constexpr std::size_t kMaterialRootSize = 344;
		// +144 of Die Maschine's resident images with mips (the 2048 x 2048 set): not streamed.
		constexpr std::uint32_t kResidentImageFlags = 0x20;

		template <typename T>
		void Set(std::span<std::uint8_t> data, std::size_t offset, const T& value) {
			std::memcpy(data.data() + offset, &value, sizeof(value));
		}

		void WriteBytes(XWriter& w, std::span<const std::uint8_t> bytes) {
			w.Write(bytes.data(), bytes.size());
		}

		std::uint32_t FourCC(const char* text) {
			std::uint32_t value = 0;
			std::memcpy(&value, text, 4);
			return value;
		}
	}

	std::size_t ImageLevelBytes(std::uint32_t format, std::uint32_t width, std::uint32_t height) {
		const std::size_t w = std::max(width, 1u), h = std::max(height, 1u);
		const std::size_t blocks = ((w + 3) / 4) * ((h + 3) / 4);
		switch (format) {
		case 71: case 72: case 80: case 81: return blocks * 8;                     // BC1, BC4
		case 74: case 75: case 77: case 78: case 83: case 84:                    // BC2, BC3, BC5
		case 95: case 96: case 98: case 99: return blocks * 16;                  // BC6H, BC7
		case 24: case 28: case 29: case 87: case 91: return w * h * 4;           // 10:10:10:2, RGBA8, BGRA8
		case 10: return w * h * 8;                                               // RGBA16F
		case 61: return w * h;                                                   // R8
		default: return 0;
		}
	}

	std::uint8_t ResidentImageLevels(std::uint32_t width, std::uint32_t height, std::uint32_t available) {
		std::uint8_t levels = 1;
		while (levels < 7 && levels < available && (width >> levels) >= 4 && (height >> levels) >= 4) {
			++levels;
		}
		return levels;
	}

	// Mirrors LoadImageBody (asset_loaders.cpp): the root, then in block 6 the pixels; no mip records, no +8 data.
	void EncodeResidentImage(XWriter& w, const ResidentImage& image) {
		std::array<std::uint8_t, kImageRootSize> root{};
		const std::span<std::uint8_t> r(root);
		Set(r, 0, image.name);
		Set(r, 40, kPtrInline);
		Set(r, 144, kResidentImageFlags);
		Set(r, 148, image.Id());
		Set(r, 152, static_cast<std::uint32_t>(image.pixels.size()));
		Set(r, 156, image.format);
		Set(r, 160, image.width);
		Set(r, 162, image.height);
		Set(r, 164, static_cast<std::uint16_t>(1)); // depth
		root[166] = 0x02;
		root[168] = 1;
		root[169] = 1;
		Set(r, 172, image.width);
		Set(r, 174, image.height);
		root[177] = image.levels;
		root[179] = 4;
		root[180] = image.usage;
		root[181] = 1; // 2D
		root[182] = image.levels;

		w.Push(XBlockTemp);
		w.Alloc(8);
		WriteBytes(w, root);
		w.Push(XBlockVirtual);
		w.Push(XBlockPhysical);
		w.Alloc(256);
		WriteBytes(w, image.pixels);
		w.Pop();
		w.Pop();
		w.Pop();
	}

	bool ReadDds(const std::filesystem::path& path, DdsImage& out, std::string& error) {
		std::vector<std::uint8_t> file;
		if (!ReadWholeFile(path, file)) {
			error = std::format("cannot read {}", path.string());
			return false;
		}
		const std::span<const std::uint8_t> f(file);
		if (file.size() < 128 || std::memcmp(file.data(), "DDS ", 4) != 0 || Get<std::uint32_t>(f, 4) != 124) {
			error = std::format("{} is not a .dds file", path.string());
			return false;
		}
		out = {};
		out.height = Get<std::uint32_t>(f, 12);
		out.width = Get<std::uint32_t>(f, 16);
		out.levels = std::max(Get<std::uint32_t>(f, 28), 1u);
		const std::uint32_t pfFlags = Get<std::uint32_t>(f, 80);
		const std::uint32_t fourCC = Get<std::uint32_t>(f, 84);
		std::size_t data = 128;
		if ((pfFlags & 0x4) && fourCC == FourCC("DX10")) {
			if (file.size() < 148) {
				error = std::format("{}: truncated DX10 header", path.string());
				return false;
			}
			out.format = Get<std::uint32_t>(f, 128);
			if (Get<std::uint32_t>(f, 132) != 3 || Get<std::uint32_t>(f, 140) > 1) {
				error = std::format("{}: only a single 2D texture is supported", path.string());
				return false;
			}
			data = 148;
		}
		else if (pfFlags & 0x4) {
			const std::pair<const char*, std::uint32_t> known[] = {
				{ "DXT1", 71 }, { "DXT2", 74 }, { "DXT3", 74 }, { "DXT4", 77 }, { "DXT5", 77 }, { "ATI1", 80 }, { "BC4U", 80 },
				{ "ATI2", 83 }, { "BC5U", 83 },
			};
			for (const auto& [code, dxgi] : known) {
				if (fourCC == FourCC(code)) {
					out.format = dxgi;
				}
			}
		}
		else if ((pfFlags & 0x40) && Get<std::uint32_t>(f, 88) == 32) {
			// 32-bit RGB(A): R in the low byte is RGBA8, B in the low byte BGRA8.
			out.format = Get<std::uint32_t>(f, 92) == 0x000000FF ? 28 : Get<std::uint32_t>(f, 92) == 0x00FF0000 ? 87 : 0;
		}
		if (!ImageLevelBytes(out.format, 4, 4)) {
			error = std::format("{}: pixel format {} is not one mapkit writes (BC1-BC7, RGBA8)", path.string(), out.format);
			return false;
		}
		if (!out.width || !out.height || out.width > 16384 || out.height > 16384) {
			error = std::format("{}: size {} x {}", path.string(), out.width, out.height);
			return false;
		}
		std::size_t total = 0;
		for (std::uint32_t l = 0; l < out.levels; ++l) {
			total += ImageLevelBytes(out.format, out.width >> l, out.height >> l);
		}
		if (file.size() - data < total) {
			error = std::format("{}: {} levels of {} x {} need {} bytes, the file has {}", path.string(), out.levels, out.width,
				out.height, total, file.size() - data);
			return false;
		}
		out.pixels.assign(file.begin() + data, file.begin() + data + total);
		return true;
	}

	std::uint32_t MaterialCopy::TableImage::Semantic() const {
		return Get<std::uint32_t>(std::span<const std::uint8_t>(raw), 8);
	}

	bool CopyMaterial(const ZoneTrace& trace, std::span<const std::uint8_t> stream, const XAssetList& list,
		std::size_t asset, MaterialCopy& out, std::string& error) {
		out = {};
		if (asset >= list.assets.size() || list.assets[asset].type != 0x0A || list.assets[asset].header != kPtrInline) {
			error = std::format("asset {} is not a material stored in this zone", asset);
			return false;
		}
		const TracedAsset& start = trace.assets[asset];
		XStream s(stream, 0);
		s.Restore(start.offset, start.block, start.pos);

		// Mirrors LoadMaterialBody (model_assets.cpp), keeping every piece: inline ones from the stream, shared ones
		// resolved from the asset that stored them. `shared` tells the caller the piece came from elsewhere: a
		// pointer inside it marked inline was inline THERE, where it cannot be followed.
		auto fetch = [&](std::uint64_t pointer, std::size_t size, std::uint64_t alignment, const char* what,
			std::vector<std::uint8_t>& bytes, bool* shared = nullptr) {
			if (shared) {
				*shared = false;
			}
			if (pointer == kPtrNull || !error.empty()) {
				return false;
			}
			if (pointer == kPtrInline) {
				s.Alloc(alignment);
				bytes.assign(size, 0);
				if (!s.Load(bytes.data(), size)) {
					error = std::format("{}: {}", what, s.Error());
					return false;
				}
				return true;
			}
			std::string resolveError;
			if (!ResolveStoredData(trace, stream, pointer, size, bytes, resolveError)) {
				error = std::format("{}: {}", what, resolveError);
				return false;
			}
			if (shared) {
				*shared = true;
			}
			return true;
		};
		auto linkedName = [&](std::uint64_t pointer, const char* what, std::uint32_t* id = nullptr) -> std::uint64_t {
			const std::size_t linked = TracedAssetOfReference(trace, list, pointer);
			if (linked == SIZE_MAX) {
				if (error.empty()) {
					error = std::format("{} is not a link to another asset (0x{:X}): an asset stored inside the material is not "
						"handled", what, pointer);
				}
				return 0;
			}
			if (id && list.assets[linked].header == kPtrInline && trace.assets[linked].offset + kImageRootSize <= stream.size()) {
				std::memcpy(id, stream.data() + trace.assets[linked].offset + 148, 4);
			}
			return TracedAssetName(trace, stream, linked);
		};

		std::vector<std::uint8_t> root(kMaterialRootSize);
		s.Push(XBlockTemp);
		s.Alloc(16);
		if (!s.Load(root.data(), root.size())) {
			error = s.Error();
			return false;
		}
		std::memcpy(out.root.data(), root.data(), root.size());
		const std::span<const std::uint8_t> m(root);
		s.Push(XBlockVirtual);

		for (std::size_t i = 0; i < 13; ++i) {
			std::vector<std::uint8_t> raw;
			bool shared = false;
			if (!fetch(Get<std::uint64_t>(m, 168 + 8 * i), 64, 8, "material record", raw, &shared)) {
				continue;
			}
			MaterialCopy::Record& record = out.records[i].emplace();
			std::memcpy(record.raw.data(), raw.data(), 64);
			for (std::size_t v = 0; v < 8; ++v) {
				const std::uint64_t pointer = Get<std::uint64_t>(std::span<const std::uint8_t>(raw), 8 * v);
				if (shared && pointer == kPtrInline) {
					error = "a shared material record holds inline vectors: not handled";
					break;
				}
				std::vector<std::uint8_t> vector;
				if (fetch(pointer, 12, 4, "material record vector", vector)) {
					std::memcpy(record.vectors[v].emplace().data(), vector.data(), 12);
				}
			}
		}

		s.Push(XBlockPhysical);
		fetch(Get<std::uint64_t>(m, 280), static_cast<std::size_t>(Get<std::uint64_t>(m, 272)), 256, "material constant buffer",
			out.constants);
		s.Pop();

		const std::uint64_t techset = Get<std::uint64_t>(m, 40);
		if (techset) {
			out.techset = linkedName(techset, "the material's techset");
		}

		std::vector<std::uint8_t> table;
		if (fetch(Get<std::uint64_t>(m, 48), 24ull * m[328], 8, "material image table", table)) {
			for (std::size_t e = 0; e + 24 <= table.size() && error.empty(); e += 24) {
				MaterialCopy::TableImage& image = out.images.emplace_back();
				std::memcpy(image.raw.data(), table.data() + e, 24);
				image.image = linkedName(Get<std::uint64_t>(std::span<const std::uint8_t>(table), e), "a material image", &image.id);
			}
		}
		fetch(Get<std::uint64_t>(m, 56), 8ull * m[329], 16, "material +56 list", out.list56);

		for (std::size_t i = 0; i < 8; ++i) {
			std::vector<std::uint8_t> raw;
			bool shared = false;
			if (!fetch(Get<std::uint64_t>(m, 64 + 8 * i), 24, 8, "material texture set", raw, &shared)) {
				continue;
			}
			MaterialCopy::TextureSet& set = out.sets[i].emplace();
			std::memcpy(set.root.data(), raw.data(), 24);
			const std::span<const std::uint8_t> r(raw);
			if (shared && (Get<std::uint64_t>(r, 0) == kPtrInline || Get<std::uint64_t>(r, 8) == kPtrInline)) {
				error = "a shared texture set holds inline data: not handled";
				break;
			}
			fetch(Get<std::uint64_t>(r, 0), 4ull * Get<std::uint32_t>(r, 16), 64, "texture set data", set.data);
			if (fetch(Get<std::uint64_t>(r, 8), 16ull * Get<std::uint32_t>(r, 20), 8, "texture set images", set.entries)) {
				for (std::size_t e = 0; e + 16 <= set.entries.size() && error.empty(); e += 16) {
					set.entryImages.push_back(linkedName(Get<std::uint64_t>(std::span<const std::uint8_t>(set.entries), e),
						"a texture set image"));
				}
			}
		}

		fetch(Get<std::uint64_t>(m, 296), 24ull * Get<std::uint32_t>(m, 304), 4, "material +296 array", out.tail296);
		fetch(Get<std::uint64_t>(m, 312), 8ull * Get<std::uint32_t>(m, 320), 4, "material +312 array", out.tail312);
		if (Get<std::uint64_t>(m, 336) && error.empty()) {
			error = "the material has its own local techset: not handled";
		}
		s.Pop();
		s.Pop();
		return error.empty();
	}

	bool ReplaceMaterialImage(MaterialCopy& copy, std::uint32_t semantic, const ReplacedImage& image) {
		for (MaterialCopy::TableImage& entry : copy.images) {
			if (entry.Semantic() != semantic) {
				continue;
			}
			const std::uint64_t retail = entry.image;
			const std::uint32_t retailId = entry.id;
			entry.image = image.name;
			entry.id = image.id;
			for (auto& set : copy.sets) {
				if (!set) {
					continue;
				}
				for (std::uint64_t& linked : set->entryImages) {
					linked = linked == retail ? image.name : linked;
				}
				for (std::size_t at = 0; retailId && at + 4 <= set->data.size(); at += 4) {
					if (Get<std::uint32_t>(std::span<const std::uint8_t>(set->data), at) == retailId) {
						Set(std::span<std::uint8_t>(set->data), at, image.id);
					}
				}
			}
			return true;
		}
		return false;
	}

	// Mirrors LoadMaterialBody (model_assets.cpp) with every piece inline.
	void EncodeMaterialCopy(XWriter& w, const MaterialCopy& copy, std::uint64_t name, std::uint32_t materialId) {
		const auto link = [&w](std::uint64_t type, std::uint64_t linked) {
			const auto entry = linked ? w.AssetEntry(type, linked) : std::nullopt;
			return entry ? *entry : kPtrNull;
		};
		const auto inlineIf = [](bool present) { return present ? kPtrInline : kPtrNull; };

		std::array<std::uint8_t, kMaterialRootSize> root = copy.root;
		const std::span<std::uint8_t> r(root);
		Set(r, 0, name);
		Set(r, 16, materialId);
		Set(r, 40, link(0x0F, copy.techset));
		Set(r, 48, inlineIf(!copy.images.empty()));
		root[328] = static_cast<std::uint8_t>(copy.images.size());
		Set(r, 56, inlineIf(!copy.list56.empty()));
		for (std::size_t i = 0; i < 8; ++i) {
			Set(r, 64 + 8 * i, inlineIf(copy.sets[i].has_value()));
		}
		for (std::size_t i = 0; i < 13; ++i) {
			Set(r, 168 + 8 * i, inlineIf(copy.records[i].has_value()));
		}
		Set(r, 272, static_cast<std::uint64_t>(copy.constants.size()));
		Set(r, 280, inlineIf(!copy.constants.empty()));
		Set(r, 296, inlineIf(!copy.tail296.empty()));
		Set(r, 304, static_cast<std::uint32_t>(copy.tail296.size() / 24));
		Set(r, 312, inlineIf(!copy.tail312.empty()));
		Set(r, 320, static_cast<std::uint32_t>(copy.tail312.size() / 8));
		Set(r, 336, kPtrNull);

		w.Push(XBlockTemp);
		w.Alloc(16);
		WriteBytes(w, root);
		w.Push(XBlockVirtual);

		for (const auto& record : copy.records) {
			if (!record) {
				continue;
			}
			std::array<std::uint8_t, 64> raw = record->raw;
			for (std::size_t v = 0; v < 8; ++v) {
				Set(std::span<std::uint8_t>(raw), 8 * v, inlineIf(record->vectors[v].has_value()));
			}
			w.Alloc(8);
			WriteBytes(w, raw);
			for (const auto& vector : record->vectors) {
				if (vector) {
					w.Alloc(4);
					WriteBytes(w, *vector);
				}
			}
		}

		w.Push(XBlockPhysical);
		if (!copy.constants.empty()) {
			w.Alloc(256);
			WriteBytes(w, copy.constants);
		}
		w.Pop();

		if (!copy.images.empty()) {
			w.Alloc(8);
			for (const MaterialCopy::TableImage& image : copy.images) {
				std::array<std::uint8_t, 24> raw = image.raw;
				Set(std::span<std::uint8_t>(raw), 0, link(0x10, image.image));
				WriteBytes(w, raw);
			}
		}
		if (!copy.list56.empty()) {
			w.Alloc(16);
			WriteBytes(w, copy.list56);
		}

		for (const auto& set : copy.sets) {
			if (!set) {
				continue;
			}
			std::array<std::uint8_t, 24> raw = set->root;
			const std::span<std::uint8_t> sr(raw);
			Set(sr, 0, inlineIf(!set->data.empty()));
			Set(sr, 8, inlineIf(!set->entries.empty()));
			Set(sr, 16, static_cast<std::uint32_t>(set->data.size() / 4));
			Set(sr, 20, static_cast<std::uint32_t>(set->entries.size() / 16));
			w.Alloc(8);
			WriteBytes(w, raw);
			if (!set->data.empty()) {
				w.Alloc(64);
				WriteBytes(w, set->data);
			}
			if (!set->entries.empty()) {
				std::vector<std::uint8_t> entries = set->entries;
				for (std::size_t e = 0; e < set->entryImages.size() && 16 * e + 8 <= entries.size(); ++e) {
					Set(std::span<std::uint8_t>(entries), 16 * e, link(0x10, set->entryImages[e]));
				}
				w.Alloc(8);
				WriteBytes(w, entries);
			}
		}

		if (!copy.tail296.empty()) {
			w.Alloc(4);
			WriteBytes(w, copy.tail296);
		}
		if (!copy.tail312.empty()) {
			w.Alloc(4);
			WriteBytes(w, copy.tail312);
		}
		w.Pop();
		w.Pop();
	}
}
