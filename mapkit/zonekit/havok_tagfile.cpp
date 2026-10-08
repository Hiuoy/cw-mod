#include "havok_tagfile.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <format>
#include <functional>

namespace MapKit::Zone {
	namespace {
		struct Section {
			std::string tag;
			std::size_t begin = 0; // the payload, after the 8-byte header
			std::size_t end = 0;
		};

		std::uint32_t BigEndian32(std::span<const std::uint8_t> b, std::size_t at) {
			return (std::uint32_t(b[at]) << 24) | (std::uint32_t(b[at + 1]) << 16) | (std::uint32_t(b[at + 2]) << 8) | b[at + 3];
		}

		// The sections directly inside [begin, end).
		bool Children(std::span<const std::uint8_t> b, std::size_t begin, std::size_t end, std::vector<Section>& out,
			std::string& error) {
			out.clear();
			for (std::size_t at = begin; at + 8 <= end;) {
				const std::size_t size = BigEndian32(b, at) & 0x3FFFFFFF;
				if (size < 8 || at + size > end) {
					error = std::format("the section at +0x{:X} runs past its parent", at);
					return false;
				}
				out.push_back({ std::string(reinterpret_cast<const char*>(b.data() + at + 4), 4), at + 8, at + size });
				at += size;
			}
			return true;
		}

		const Section* FindSection(const std::vector<Section>& sections, std::string_view tag) {
			const auto found = std::ranges::find(sections, tag, &Section::tag);
			return found == sections.end() ? nullptr : &*found;
		}

		// Havok's packed unsigned integers in the type sections.
		class VarReader {
		public:
			VarReader(std::span<const std::uint8_t> b, const Section& section) : m_B(b), m_At(section.begin), m_End(section.end) {}
			bool More() const { return m_At < m_End && !m_Failed; }
			bool Failed() const { return m_Failed; }
			std::uint64_t Next() {
				const std::uint8_t b0 = Byte();
				if (!(b0 & 0x80)) {
					return b0;
				}
				if ((b0 & 0xC0) == 0x80) {
					return (std::uint64_t(b0 & 0x3F) << 8) | Bytes(1);
				}
				if ((b0 & 0xE0) == 0xC0) {
					return (std::uint64_t(b0 & 0x1F) << 16) | Bytes(2);
				}
				if ((b0 & 0xF8) == 0xE0) {
					return (std::uint64_t(b0 & 0x07) << 24) | Bytes(3);
				}
				switch (b0) {
				case 0xE8: return Bytes(5);
				case 0xF0: return Bytes(7);
				case 0xF8: return Bytes(8);
				default:
					m_Failed = true;
					return 0;
				}
			}

		private:
			std::uint8_t Byte() {
				if (m_At >= m_End) {
					m_Failed = true;
					return 0;
				}
				return m_B[m_At++];
			}
			std::uint64_t Bytes(int count) {
				std::uint64_t value = 0;
				for (int i = 0; i < count; ++i) {
					value = (value << 8) | Byte();
				}
				return value;
			}

			std::span<const std::uint8_t> m_B;
			std::size_t m_At;
			std::size_t m_End;
			bool m_Failed = false;
		};

		// A string section split at every NUL, empty pieces and the padding after the last one included: the type
		// sections index the pieces by position.
		std::vector<std::string> SplitStrings(std::span<const std::uint8_t> b, const Section& section) {
			std::vector<std::string> out;
			std::size_t start = section.begin;
			for (std::size_t at = section.begin; at < section.end; ++at) {
				if (!b[at]) {
					out.emplace_back(reinterpret_cast<const char*>(b.data() + start), at - start);
					start = at + 1;
				}
			}
			out.emplace_back(reinterpret_cast<const char*>(b.data() + start), section.end - start);
			return out;
		}

		template <typename T>
		T Read(std::span<const std::uint8_t> b, std::size_t at) {
			T value{};
			std::memcpy(&value, b.data() + at, sizeof(T));
			return value;
		}

		template <typename T>
		void Append(std::vector<std::uint8_t>& out, const T& value) {
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
			out.insert(out.end(), bytes, bytes + sizeof(T));
		}

		void PadTo(std::vector<std::uint8_t>& out, std::size_t alignment) {
			out.resize((out.size() + alignment - 1) / alignment * alignment, 0);
		}

		void AppendSection(std::vector<std::uint8_t>& out, const char (&tag)[5], bool leaf, std::span<const std::uint8_t> payload) {
			const std::uint32_t header = (leaf ? 0x40000000u : 0u) | static_cast<std::uint32_t>(8 + payload.size());
			for (int shift = 24; shift >= 0; shift -= 8) {
				out.push_back(static_cast<std::uint8_t>(header >> shift));
			}
			out.insert(out.end(), tag, tag + 4);
			out.insert(out.end(), payload.begin(), payload.end());
		}

		bool ReadTypes(std::span<const std::uint8_t> b, const std::vector<Section>& typeSections, std::vector<HkType>& types,
			std::string& error) {
			const Section* strings = FindSection(typeSections, "TST1");
			const Section* names = FindSection(typeSections, "TNA1");
			const Section* bodies = FindSection(typeSections, "TBDY");
			if (!strings || !names || !bodies) {
				error = "the TYPE section lacks TST1, TNA1 or TBDY";
				return false;
			}
			const std::vector<std::string> text = SplitStrings(b, *strings);
			const auto string = [&](std::uint64_t index) { return index < text.size() ? text[index] : std::string("?"); };

			struct Template {
				std::string name;
				std::uint64_t value = 0;
			};
			VarReader r(b, *names);
			const std::size_t count = r.Next();
			if (r.Failed() || count > 100000) {
				error = "TNA1 does not read";
				return false;
			}
			types.assign(count, {});
			std::vector<std::vector<Template>> templates(count);
			for (std::size_t i = 1; i < count && !r.Failed(); ++i) {
				types[i].name = string(r.Next());
				const std::size_t arguments = r.Next();
				for (std::size_t k = 0; k < arguments && !r.Failed(); ++k) {
					Template argument;
					argument.name = string(r.Next());
					argument.value = r.Next();
					templates[i].push_back(std::move(argument));
				}
			}

			struct Body {
				bool format = false;
				bool size = false;
			};
			std::vector<Body> own(count);
			VarReader body(b, *bodies);
			while (body.More()) {
				const std::uint64_t index = body.Next();
				if (!index) {
					continue;
				}
				if (index >= count) {
					error = std::format("TBDY names type {} of {}", index, count);
					return false;
				}
				HkType& type = types[index];
				type.parent = static_cast<std::uint32_t>(body.Next());
				const std::uint64_t options = body.Next();
				if (options & 0x1) {
					type.format = static_cast<std::uint32_t>(body.Next());
					own[index].format = true;
				}
				if (options & 0x2) {
					body.Next(); // subtype
				}
				if (options & 0x4) {
					body.Next(); // version
				}
				if (options & 0x8) {
					type.size = static_cast<std::uint32_t>(body.Next());
					type.alignment = static_cast<std::uint32_t>(body.Next());
					own[index].size = true;
				}
				if (options & 0x10) {
					body.Next(); // flags
				}
				if (options & 0x20) {
					const std::uint64_t fields = body.Next();
					for (std::uint64_t f = 0; f < fields * 4 && !body.Failed(); ++f) {
						body.Next(); // name, flags, offset, type
					}
				}
				if (options & 0x40) {
					const std::uint64_t interfaces = body.Next();
					for (std::uint64_t f = 0; f < interfaces * 2 && !body.Failed(); ++f) {
						body.Next();
					}
				}
				if (options & 0x80) {
					body.Next(); // attributes
				}
			}
			if (r.Failed() || body.Failed()) {
				error = "the type sections do not read";
				return false;
			}

			// Format, size and alignment come from the nearest type up the parent chain that has them.
			for (std::size_t i = 1; i < count; ++i) {
				std::size_t at = i;
				for (int depth = 0; at && !own[at].format && depth < 64; ++depth) {
					at = types[at].parent < count ? types[at].parent : 0;
				}
				types[i].format = at ? types[at].format : 0;
				at = i;
				for (int depth = 0; at && !own[at].size && depth < 64; ++depth) {
					at = types[at].parent < count ? types[at].parent : 0;
				}
				types[i].size = at ? types[at].size : 0;
				// The alignment's high bits are flags (hkVector4: 0x4010).
				types[i].alignment = at ? std::max<std::uint32_t>(types[at].alignment & 0xFF, 1) : 1;
			}

			std::vector<bool> named(count, false);
			std::function<std::string(std::size_t, int)> fullName = [&](std::size_t index, int depth) -> std::string {
				if (!index || index >= count || depth > 32) {
					return "void";
				}
				std::string name = types[index].name;
				if (!templates[index].empty()) {
					name += '<';
					for (std::size_t k = 0; k < templates[index].size(); ++k) {
						const Template& argument = templates[index][k];
						name += k ? ", " : "";
						name += argument.name.starts_with('t') ? fullName(static_cast<std::size_t>(argument.value), depth + 1)
							: std::to_string(argument.value);
					}
					name += '>';
				}
				return name;
			};
			std::vector<std::string> full(count);
			for (std::size_t i = 1; i < count; ++i) {
				full[i] = fullName(i, 0);
			}
			for (std::size_t i = 1; i < count; ++i) {
				types[i].name = std::move(full[i]);
			}
			return true;
		}
	}

	bool ReadHkTagfile(std::span<const std::uint8_t> bytes, HkTagfile& out, std::string& error) {
		out = {};
		std::vector<Section> top;
		if (!Children(bytes, 0, bytes.size(), top, error)) {
			return false;
		}
		if (top.size() != 1 || top[0].tag != "TAG0") {
			error = "not a TAG0 tagfile";
			return false;
		}
		std::vector<Section> sections;
		std::vector<Section> typeSections;
		std::vector<Section> index;
		if (!Children(bytes, top[0].begin, top[0].end, sections, error)) {
			return false;
		}
		const Section* sdk = FindSection(sections, "SDKV");
		const Section* data = FindSection(sections, "DATA");
		const Section* type = FindSection(sections, "TYPE");
		const Section* indx = FindSection(sections, "INDX");
		if (!sdk || !data || !type || !indx) {
			error = "the tagfile lacks SDKV, DATA, TYPE or INDX";
			return false;
		}
		if (!Children(bytes, type->begin, type->end, typeSections, error) || !Children(bytes, indx->begin, indx->end, index, error)) {
			return false;
		}
		out.sdkVersion.assign(bytes.begin() + sdk->begin, bytes.begin() + sdk->end);
		out.typeSection.assign(bytes.begin() + type->begin - 8, bytes.begin() + type->end);
		if (!ReadTypes(bytes, typeSections, out.types, error)) {
			return false;
		}

		const Section* items = FindSection(index, "ITEM");
		const Section* patches = FindSection(index, "PTCH");
		if (!items || !patches || (items->end - items->begin) % 12) {
			error = "the tagfile lacks a well-formed ITEM or PTCH";
			return false;
		}
		const std::span<const std::uint8_t> dataBytes = bytes.subspan(data->begin, data->end - data->begin);
		struct Placed {
			std::size_t begin = 0;
			std::size_t end = 0;
			std::uint32_t item = 0;
		};
		std::vector<Placed> placed;
		for (std::size_t at = items->begin; at < items->end; at += 12) {
			const std::uint32_t typeAndFlags = Read<std::uint32_t>(bytes, at);
			const std::uint32_t offset = Read<std::uint32_t>(bytes, at + 4);
			HkItem item;
			item.type = typeAndFlags & 0xFFFFFF;
			item.flags = static_cast<std::uint8_t>(typeAndFlags >> 24);
			item.count = Read<std::uint32_t>(bytes, at + 8);
			if (item.type >= out.types.size()) {
				error = std::format("item {} has type {} of {}", out.items.size(), item.type, out.types.size());
				return false;
			}
			const std::size_t size = std::size_t(out.types[item.type].size) * item.count;
			if (offset + size > dataBytes.size()) {
				error = std::format("item {} runs past DATA", out.items.size());
				return false;
			}
			item.data.assign(dataBytes.begin() + offset, dataBytes.begin() + offset + size);
			if (size) {
				placed.push_back({ offset, offset + size, static_cast<std::uint32_t>(out.items.size()) });
			}
			out.items.push_back(std::move(item));
		}
		if (out.items.size() < 2) {
			error = "the tagfile holds no root item";
			return false;
		}
		std::ranges::sort(placed, {}, &Placed::begin);

		for (std::size_t at = patches->begin; at + 8 <= patches->end;) {
			const std::uint32_t patchType = Read<std::uint32_t>(bytes, at);
			const std::uint32_t count = Read<std::uint32_t>(bytes, at + 4);
			at += 8;
			if (at + std::size_t(count) * 4 > patches->end) {
				error = "PTCH runs past its end";
				return false;
			}
			for (std::uint32_t k = 0; k < count; ++k, at += 4) {
				const std::uint32_t slot = Read<std::uint32_t>(bytes, at);
				const auto owner = std::ranges::upper_bound(placed, std::size_t(slot), {}, &Placed::begin);
				if (owner == placed.begin() || slot + 8 > std::prev(owner)->end) {
					error = std::format("PTCH slot +0x{:X} is in no item", slot);
					return false;
				}
				const Placed& in = *std::prev(owner);
				const std::uint64_t target = Read<std::uint64_t>(dataBytes, slot);
				if (target >= out.items.size()) {
					error = std::format("PTCH slot +0x{:X} holds item {} of {}", slot, target, out.items.size());
					return false;
				}
				HkItem& item = out.items[in.item];
				item.refs[static_cast<std::uint32_t>(slot - in.begin)] = { patchType, static_cast<std::uint32_t>(target) };
				std::fill_n(item.data.begin() + static_cast<std::ptrdiff_t>(slot - in.begin), 8, std::uint8_t(0));
			}
		}
		return true;
	}

	std::vector<std::uint8_t> WriteHkTagfile(const HkTagfile& file) {
		const std::vector<HkItem>& items = file.items;
		std::vector<std::uint32_t> newIndex(items.size(), 0);
		std::vector<std::uint32_t> order = { 0 }; // old index by new index
		std::vector<std::uint32_t> placement;     // old indices in DATA order
		const auto number = [&](std::uint32_t old) {
			newIndex[old] = static_cast<std::uint32_t>(order.size());
			order.push_back(old);
		};
		// An item's references in numbering order: by offset, and for an array of records one field of every
		// element before the next field.
		const auto references = [&](const HkItem& item) {
			std::vector<std::pair<std::uint32_t, std::uint32_t>> refs; // {offset, target}
			for (const auto& [offset, ref] : item.refs) {
				if (ref.target) {
					refs.emplace_back(offset, ref.target);
				}
			}
			const std::size_t stride = item.count > 1 ? item.data.size() / item.count : 0;
			if (stride) {
				std::ranges::stable_sort(refs, [stride](const auto& a, const auto& b) {
					return std::pair(a.first % stride, a.first / stride) < std::pair(b.first % stride, b.first / stride);
				});
			}
			return refs;
		};
		std::deque<std::uint32_t> objects;
		if (items.size() > 1) {
			number(1);
			objects.push_back(1);
		}
		while (!objects.empty()) {
			const std::uint32_t object = objects.front();
			objects.pop_front();
			placement.push_back(object);
			std::vector<std::uint32_t> level = { object };
			while (!level.empty()) {
				std::vector<std::uint32_t> next;
				for (const std::uint32_t owner : level) {
					for (const auto& [offset, target] : references(items[owner])) {
						if (target >= items.size() || newIndex[target]) {
							continue;
						}
						number(target);
						if (items[target].flags & HkItem::kArray) {
							placement.push_back(target);
							next.push_back(target);
						}
						else {
							objects.push_back(target);
						}
					}
				}
				level = std::move(next);
			}
		}

		std::vector<std::uint8_t> data;
		std::vector<std::size_t> offsetOf(items.size(), 0);
		std::map<std::uint32_t, std::vector<std::uint32_t>> patches;
		for (const std::uint32_t old : placement) {
			const HkItem& item = items[old];
			const std::uint32_t alignment = (item.flags & HkItem::kArray) ? 16
				: (item.type < file.types.size() ? file.types[item.type].alignment : 8);
			PadTo(data, std::max<std::uint32_t>(alignment, 1));
			offsetOf[old] = data.size();
			data.insert(data.end(), item.data.begin(), item.data.end());
			for (const auto& [offset, ref] : item.refs) {
				if (!ref.target || ref.target >= items.size() || offset + 8 > item.data.size()) {
					continue;
				}
				const std::uint64_t index = newIndex[ref.target];
				std::memcpy(data.data() + offsetOf[old] + offset, &index, 8);
				patches[ref.patchType].push_back(static_cast<std::uint32_t>(offsetOf[old] + offset));
			}
		}
		PadTo(data, 16);

		std::vector<std::uint8_t> itemTable(12, 0);
		for (std::size_t k = 1; k < order.size(); ++k) {
			const HkItem& item = items[order[k]];
			Append(itemTable, static_cast<std::uint32_t>((std::uint32_t(item.flags) << 24) | item.type));
			Append(itemTable, static_cast<std::uint32_t>(offsetOf[order[k]]));
			Append(itemTable, item.count);
		}
		std::vector<std::uint8_t> patchTable;
		for (auto& [type, offsets] : patches) {
			std::ranges::sort(offsets);
			Append(patchTable, type);
			Append(patchTable, static_cast<std::uint32_t>(offsets.size()));
			for (const std::uint32_t offset : offsets) {
				Append(patchTable, offset);
			}
		}

		std::vector<std::uint8_t> index;
		AppendSection(index, "ITEM", true, itemTable);
		AppendSection(index, "PTCH", true, patchTable);
		std::vector<std::uint8_t> body;
		AppendSection(body, "SDKV", true, file.sdkVersion);
		AppendSection(body, "DATA", true, data);
		body.insert(body.end(), file.typeSection.begin(), file.typeSection.end());
		AppendSection(body, "INDX", false, index);
		std::vector<std::uint8_t> out;
		AppendSection(out, "TAG0", false, body);
		return out;
	}

	std::uint32_t HkTagfile::FindType(std::string_view name) const {
		for (std::size_t i = 1; i < types.size(); ++i) {
			if (types[i].name == name) {
				return static_cast<std::uint32_t>(i);
			}
		}
		return 0;
	}

	std::uint32_t HkTagfile::Target(std::uint32_t item, std::uint32_t offset) const {
		if (item >= items.size()) {
			return 0;
		}
		const auto found = items[item].refs.find(offset);
		return found == items[item].refs.end() ? 0 : found->second.target;
	}

	bool HkTagfile::SetArray(std::uint32_t item, std::uint32_t offset, std::uint32_t elementType, std::vector<std::uint8_t> bytes,
		std::uint32_t count, std::uint32_t patchType) {
		if (item >= items.size()) {
			return false;
		}
		if (!count) {
			SetNull(item, offset);
			return true;
		}
		auto& refs = items[item].refs;
		const auto found = refs.find(offset);
		if (!patchType && found != refs.end()) {
			patchType = found->second.patchType;
		}
		if (!patchType) {
			return false;
		}
		HkItem array;
		array.type = elementType;
		array.flags = HkItem::kArray;
		array.count = count;
		array.data = std::move(bytes);
		items.push_back(std::move(array));
		items[item].refs[offset] = { patchType, static_cast<std::uint32_t>(items.size() - 1) };
		return true;
	}

	void HkTagfile::SetNull(std::uint32_t item, std::uint32_t offset) {
		if (item < items.size()) {
			items[item].refs.erase(offset);
		}
	}

	std::vector<std::uint8_t> NavPayload(std::span<const std::uint8_t> tagfile) {
		std::vector<std::uint8_t> out(kNavPayloadHeader + tagfile.size(), 0);
		const std::uint64_t rest = tagfile.size() + (kNavPayloadHeader - 0x100);
		std::memcpy(out.data(), &rest, 8);
		const auto size = static_cast<std::uint32_t>(tagfile.size());
		std::memcpy(out.data() + 0x108, &size, 4);
		std::memcpy(out.data() + kNavPayloadHeader, tagfile.data(), tagfile.size());
		return out;
	}

	std::span<const std::uint8_t> NavPayloadTagfile(std::span<const std::uint8_t> payload) {
		if (payload.size() < kNavPayloadHeader + 8) {
			return {};
		}
		const std::uint32_t size = Read<std::uint32_t>(payload, 0x108);
		if (kNavPayloadHeader + std::size_t(size) > payload.size() || std::memcmp(payload.data() + kNavPayloadHeader + 4, "TAG0", 4)) {
			return {};
		}
		return payload.subspan(kNavPayloadHeader, size);
	}
}
