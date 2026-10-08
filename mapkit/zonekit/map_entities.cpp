#include "map_entities.hpp"
#include "asset_loaders.hpp"
#include "hash.hpp"
#include "xasset_list.hpp"
#include "xwriter.hpp"
#include "zone_trace.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace MapKit::Zone {
	struct EntityStrings {
		// Inline strings by (block << 60 | position): what a reference to them holds, minus one.
		std::unordered_map<std::uint64_t, std::string> inlined;
		struct Pending {
			std::size_t entity = 0; // index in this decode's own entities
			std::size_t key = 0;
			bool isKey = true;      // the key's name, else its String value
			std::uint64_t pointer = 0;
		};
		std::vector<Pending> pending;
	};

	namespace {
		constexpr std::size_t kEntitySize = 48;
		constexpr std::size_t kKeySize = 32;
		constexpr std::size_t kNoEntity = ~std::size_t(0);

		// An XString field of a KeyValue (Load_MapEntityArray 0x7FF71E7DE900): -1 = Alloc(1) + the inline
		// string, 0 = null, anything else a reference to a string loaded earlier.
		void LoadEntityString(XStream& s, std::uint64_t pointer, std::string& text, EntityStrings* strings,
			std::size_t entity, std::size_t key, bool isKey) {
			if (pointer == kPtrInline) {
				const std::uint64_t where = (static_cast<std::uint64_t>(s.Block()) << 60) | s.Alloc(1);
				s.LoadString(&text);
				if (strings) {
					strings->inlined.emplace(where, text);
				}
			}
			else if (pointer != kPtrNull) {
				s.Reference(pointer, "entity string");
				if (strings && entity != kNoEntity) {
					strings->pending.push_back({ entity, key, isKey, pointer });
				}
			}
		}

		// Load_MapEntityArray 0x7FF71E7DE900, called with the array already Alloc(8)-ed: count x 48 B, then
		// per entity its keys (Alloc(8) + keyCount x 32 B) and per key its name and String value.
		bool LoadMapEntityArray(XStream& s, std::uint64_t count, bool triggers, std::vector<MapEntity>* out,
			EntityStrings* strings) {
			if (count > (s.Size() - s.Cursor()) / kEntitySize) {
				return s.Fail(std::format("{} entities run past the end of the stream", count));
			}
			const std::size_t arrayOffset = s.Cursor();
			std::vector<std::uint8_t> heads(count * kEntitySize);
			if (!s.Load(heads.data(), heads.size())) {
				return false;
			}
			const std::size_t first = out ? out->size() : 0;
			for (std::size_t i = 0; out && i < count; ++i) {
				const std::span<const std::uint8_t> head(heads.data() + i * kEntitySize, kEntitySize);
				MapEntity& entity = out->emplace_back();
				entity.trigger = triggers;
				entity.field16 = Get<std::int32_t>(head, 16);
				entity.id = Get<std::uint32_t>(head, 20);
				for (std::size_t axis = 0; axis < 3; ++axis) {
					entity.origin[axis] = Get<float>(head, 24 + 4 * axis);
					entity.angles[axis] = Get<float>(head, 36 + 4 * axis);
				}
				entity.offset = arrayOffset + i * kEntitySize;
				std::memcpy(entity.raw.data(), head.data(), kEntitySize);
			}

			for (std::size_t i = 0; i < count && !s.Failed(); ++i) {
				const std::span<const std::uint8_t> head(heads.data() + i * kEntitySize, kEntitySize);
				if (!Get<std::uint64_t>(head, 8)) {
					continue;
				}
				const std::uint32_t keyCount = Get<std::uint32_t>(head, 0);
				if (keyCount > (s.Size() - s.Cursor()) / kKeySize) {
					return s.Fail(std::format("entity {}: {} keys run past the end of the stream", i, keyCount));
				}
				s.Alloc(8);
				const std::size_t keysOffset = s.Cursor();
				std::vector<std::uint8_t> keys(static_cast<std::size_t>(keyCount) * kKeySize);
				if (!s.Load(keys.data(), keys.size())) {
					return false;
				}
				const std::size_t entity = out ? first + i : kNoEntity;
				for (std::size_t k = 0; k < keyCount && !s.Failed(); ++k) {
					const std::span<const std::uint8_t> kv(keys.data() + k * kKeySize, kKeySize);
					EntityKey item;
					item.type = static_cast<EntityValueType>(Get<std::uint16_t>(kv, 24));
					item.flags = Get<std::uint16_t>(kv, 26);
					item.offset = keysOffset + k * kKeySize;
					std::memcpy(item.raw.data(), kv.data(), kKeySize);
					LoadEntityString(s, Get<std::uint64_t>(kv, 0), item.key, strings, entity, k, true);
					switch (item.type) {
					case EntityValueType::String:
						LoadEntityString(s, Get<std::uint64_t>(kv, 8), item.text, strings, entity, k, false);
						break;
					case EntityValueType::Vector:
						for (std::size_t axis = 0; axis < 3; ++axis) {
							item.vector[axis] = Get<float>(kv, 8 + 4 * axis);
						}
						break;
					case EntityValueType::Hash:
						item.hash = Get<std::uint64_t>(kv, 8);
						break;
					case EntityValueType::Float:
						item.number = Get<float>(kv, 8);
						break;
					case EntityValueType::Int:
						item.integer = Get<std::int64_t>(kv, 8);
						break;
					default:
						item.hash = Get<std::uint64_t>(kv, 8);
						break;
					}
					if (out) {
						(*out)[entity].keys.push_back(std::move(item));
					}
				}
			}
			return !s.Failed();
		}

		// Each detached decode starts its block positions at 0, so a reference to one of its strings is off
		// by where the engine's block really stood (unknown without the full walk). Tables are solved in
		// stream order: every (reference - inline position) pair votes for a base, counting only references
		// no earlier table explains (the entity list's first references mostly name trigger-list strings,
		// and would otherwise drown its own base in chance matches).
		struct Table {
			const EntityStrings* strings = nullptr;
			std::uint64_t base = 0;
			bool solved = false;

			const std::string* Find(std::uint64_t pointer) const {
				if (!solved) {
					return nullptr;
				}
				const auto found = strings->inlined.find((pointer - 1) - base);
				return found == strings->inlined.end() ? nullptr : &found->second;
			}
		};

		void SolveBases(std::span<Table> tables) {
			for (Table& table : tables) {
				std::unordered_map<std::uint64_t, std::size_t> votes;
				std::size_t sampled = 0;
				for (const Table& walk : tables) {
					for (const EntityStrings::Pending& ref : walk.strings->pending) {
						if (sampled >= 1024) {
							break;
						}
						if (std::ranges::any_of(tables, [&](const Table& other) { return other.Find(ref.pointer); })) {
							continue;
						}
						++sampled;
						for (const auto& [where, text] : table.strings->inlined) {
							++votes[(ref.pointer - 1) - where];
						}
					}
				}
				std::size_t bestVotes = 0;
				for (const auto& [base, count] : votes) {
					if (count > bestVotes) {
						table.base = base;
						bestVotes = count;
					}
				}
				table.solved = bestVotes >= 3;
			}
		}

		bool PlausibleTriggerList(std::span<const std::uint8_t> stream, std::size_t at) {
			if (stream.size() - at < 72) {
				return false;
			}
			const auto root = stream.subspan(at, 72);
			for (std::size_t pointer : { 16, 32, 48, 64 }) {
				const auto value = Get<std::uint64_t>(root, pointer);
				if (value != kPtrNull && value != kPtrInline) {
					return false;
				}
			}
			return Get<std::uint64_t>(root, 64) == kPtrInline && Get<std::uint64_t>(root, 56) < 1'000'000
				&& Get<std::uint32_t>(root, 12) == 0 && Get<std::uint32_t>(root, 28) == 0 && Get<std::uint32_t>(root, 44) == 0;
		}

		bool PlausibleEntityList(std::span<const std::uint8_t> stream, std::size_t at) {
			if (stream.size() - at < 24) {
				return false;
			}
			const auto root = stream.subspan(at, 24);
			const auto count = Get<std::uint64_t>(root, 8);
			return count > 0 && count < 1'000'000 && Get<std::uint64_t>(root, 16) == kPtrInline;
		}

		// Load_<Type>Asset for an asset found in place: the root struct in the temp block, then its body.
		template <typename Body>
		bool DecodeAt(std::span<const std::uint8_t> stream, std::size_t at, std::size_t rootSize, Body&& body,
			std::size_t* end = nullptr) {
			XStream s(stream, at);
			s.Detach();
			s.Push(XBlockTemp);
			s.Alloc(8);
			std::vector<std::uint8_t> root(rootSize);
			if (s.Load(root.data(), root.size())) {
				body(s, std::span<const std::uint8_t>(root));
			}
			s.Pop();
			if (end) {
				*end = s.Cursor();
			}
			return !s.Failed();
		}
	}

	std::string EntityKey::ValueText() const {
		switch (type) {
		case EntityValueType::String:
			return text;
		case EntityValueType::Vector:
			return std::format("{} {} {}", vector[0], vector[1], vector[2]);
		case EntityValueType::Hash:
			return std::format("#{:016x}", hash);
		case EntityValueType::Float:
			return std::format("{}", number);
		case EntityValueType::Int:
			return std::format("{}", integer);
		}
		return std::format("<type {}: {:016x}>", static_cast<int>(type), hash);
	}

	const EntityKey* MapEntity::Find(std::string_view key) const {
		for (const EntityKey& item : keys) {
			if (item.key == key) {
				return &item;
			}
		}
		return nullptr;
	}

	std::string MapEntity::Text(std::string_view key) const {
		const EntityKey* item = Find(key);
		return item && item->type == EntityValueType::String ? item->text : std::string();
	}

	bool LoadEntityListBody(XStream& s, std::span<const std::uint8_t> root, std::vector<MapEntity>* out,
		EntityStrings* strings) {
		// Load_EntitylistAsset 0x7FF71E7D50F0.
		s.Push(XBlockVirtual);
		if (Get<std::uint64_t>(root, 16)) {
			s.Alloc(8);
			LoadMapEntityArray(s, Get<std::uint64_t>(root, 8), false, out, strings);
		}
		s.Pop();
		return !s.Failed();
	}

	bool LoadTriggerListBody(XStream& s, std::span<const std::uint8_t> root, std::vector<MapEntity>* out,
		EntityStrings* strings, TriggerShapes* shapes) {
		// Load_TriggerList 0x7FF71E7EEEE0: three arrays (-1 = Alloc(4) + count x stride, else a reference),
		// then the entity array.
		s.Push(XBlockVirtual);
		constexpr std::array<std::array<std::size_t, 3>, 3> kArrays = { { { 16, 8, 8 }, { 32, 24, 32 }, { 48, 40, 20 } } };
		for (std::size_t a = 0; a < kArrays.size(); ++a) {
			const auto& [pointer, count, stride] = kArrays[a];
			const auto value = Get<std::uint64_t>(root, pointer);
			if (value == kPtrInline) {
				s.Alloc(4);
				std::vector<std::uint8_t> bytes(static_cast<std::size_t>(Get<std::uint32_t>(root, count)) * stride);
				if (!s.Load(bytes.data(), bytes.size())) {
					break;
				}
				for (std::size_t at = 0; shapes && at < bytes.size(); at += stride) {
					if (a == 0) {
						std::memcpy(shapes->models.emplace_back().data(), bytes.data() + at, stride);
					}
					else if (a == 1) {
						std::memcpy(shapes->hulls.emplace_back().data(), bytes.data() + at, stride);
					}
					else {
						std::memcpy(shapes->slabs.emplace_back().data(), bytes.data() + at, stride);
					}
				}
			}
			else if (value != kPtrNull) {
				s.Reference(value, "trigger list array");
			}
		}
		if (Get<std::uint64_t>(root, 64)) {
			s.Alloc(8);
			LoadMapEntityArray(s, Get<std::uint64_t>(root, 56), true, out, strings);
		}
		s.Pop();
		return !s.Failed();
	}

	std::string MapBspName(std::string_view mapName) {
		return std::format("maps/{}/{}.d3dbsp", mapName.substr(0, mapName.find('_')), mapName);
	}

	bool FindMapEntities(std::span<const std::uint8_t> stream, std::string_view mapName, MapEntities& out,
		std::string& error) {
		const std::uint64_t name = HashName(MapBspName(mapName));
		std::uint8_t pattern[8];
		std::memcpy(pattern, &name, sizeof(pattern));

		std::vector<MapEntity> triggers;
		std::vector<MapEntity> entities;
		EntityStrings triggerStrings;
		EntityStrings entityStrings;
		bool haveTriggers = false;
		bool haveEntities = false;
		for (auto it = stream.begin(); !(haveTriggers && haveEntities);) {
			it = std::search(it, stream.end(), std::begin(pattern), std::end(pattern));
			if (it == stream.end()) {
				break;
			}
			const std::size_t at = static_cast<std::size_t>(it - stream.begin());
			++it;
			if (!haveTriggers && PlausibleTriggerList(stream, at)) {
				std::vector<MapEntity> found;
				EntityStrings strings;
				TriggerShapes shapes;
				if (DecodeAt(stream, at, 72, [&](XStream& s, std::span<const std::uint8_t> root) {
						LoadTriggerListBody(s, root, &found, &strings, &shapes);
					}) && !found.empty()) {
					triggers = std::move(found);
					triggerStrings = std::move(strings);
					out.shapes = std::move(shapes);
					out.triggerList = at;
					haveTriggers = true;
				}
			}
			else if (!haveEntities && PlausibleEntityList(stream, at)) {
				std::vector<MapEntity> found;
				EntityStrings strings;
				std::size_t end = 0;
				if (DecodeAt(stream, at, 24, [&](XStream& s, std::span<const std::uint8_t> root) {
						LoadEntityListBody(s, root, &found, &strings);
					}, &end) && !found.empty()
					&& std::ranges::any_of(found[0].keys, [](const EntityKey& key) { return key.text == "worldspawn"; })) {
					entities = std::move(found);
					entityStrings = std::move(strings);
					out.entityList = at;
					out.entityListEnd = end;
					haveEntities = true;
				}
			}
		}
		if (!haveEntities) {
			error = std::format("no entity list for {} ({:016x}) in this stream", MapBspName(mapName), name);
			return false;
		}

		// Resolve the references against both decodes' strings, the earlier asset in the stream first.
		std::array<Table, 2> tables = { { { &triggerStrings }, { &entityStrings } } };
		if (out.entityList < out.triggerList) {
			std::swap(tables[0], tables[1]);
		}
		SolveBases(tables);
		out.unresolved = 0;
		auto resolve = [&](std::vector<MapEntity>& list, const EntityStrings& strings) {
			for (const EntityStrings::Pending& ref : strings.pending) {
				const std::string* text = nullptr;
				for (const Table& table : tables) {
					if (!text) {
						text = table.Find(ref.pointer);
					}
				}
				EntityKey& key = list[ref.entity].keys[ref.key];
				std::string& field = ref.isKey ? key.key : key.text;
				if (text) {
					field = *text;
				}
				else {
					field = std::format("<ref {:x}>", ref.pointer);
					++out.unresolved;
				}
			}
		};
		resolve(triggers, triggerStrings);
		resolve(entities, entityStrings);

		out.entities = std::move(triggers);
		out.entities.insert(out.entities.end(), std::make_move_iterator(entities.begin()),
			std::make_move_iterator(entities.end()));
		return true;
	}

	void MoveEntity(std::span<std::uint8_t> stream, const MapEntity& entity, const std::array<float, 3>& origin,
		std::optional<float> yaw) {
		auto put = [&](std::size_t offset, float value) {
			if (offset + sizeof(value) <= stream.size()) {
				std::memcpy(stream.data() + offset, &value, sizeof(value));
			}
		};
		for (std::size_t axis = 0; axis < 3; ++axis) {
			put(entity.offset + 24 + 4 * axis, origin[axis]);
		}
		if (yaw) {
			put(entity.offset + 40, *yaw);
		}
		for (const EntityKey& key : entity.keys) {
			if (key.type != EntityValueType::Vector) {
				continue;
			}
			if (key.key == "origin") {
				for (std::size_t axis = 0; axis < 3; ++axis) {
					put(key.offset + 8 + 4 * axis, origin[axis]);
				}
			}
			else if (key.key == "angles" && yaw) {
				put(key.offset + 12, *yaw);
			}
		}
	}
	bool ReadMapEntitiesTraced(std::span<const std::uint8_t> stream, const XAssetList& list, const ZoneTrace& trace,
		MapEntities& out, std::string& error) {
		out = {};
		std::size_t triggerIndex = list.assets.size();
		std::size_t entityIndex = list.assets.size();
		for (std::size_t i = 0; i < list.assets.size(); ++i) {
			if (list.assets[i].type == 0x80 && triggerIndex == list.assets.size()) {
				triggerIndex = i;
			}
			else if (list.assets[i].type == 0x8E && entityIndex == list.assets.size()) {
				entityIndex = i;
			}
		}
		if (entityIndex == list.assets.size()) {
			error = "no entitylist asset in this zone";
			return false;
		}

		// Every inline string the decodes (and the replays below) meet, by the value a reference to it holds.
		std::unordered_map<std::uint64_t, std::string> log;
		auto decode = [&](std::size_t index, std::size_t rootSize, bool triggers, std::vector<MapEntity>& found,
			EntityStrings& strings) {
			const TracedAsset& start = trace.assets[index];
			XStream s(stream, 0);
			s.Restore(start.offset, start.block, start.pos);
			s.LogStrings(&log);
			const bool ok = LoadAssetHeader(s, list.assets[index].header, rootSize, 8, [&](std::span<std::uint8_t> root) {
				if (triggers) {
					LoadTriggerListBody(s, root, &found, &strings, &out.shapes);
				}
				else {
					LoadEntityListBody(s, root, &found, &strings);
				}
			});
			if (!ok) {
				error = std::format("asset {} ({}): {}", index, triggers ? "triggerlist" : "entitylist", s.Error());
			}
			return ok;
		};

		std::vector<MapEntity> triggers;
		std::vector<MapEntity> entities;
		EntityStrings triggerStrings;
		EntityStrings entityStrings;
		if (triggerIndex != list.assets.size() && !decode(triggerIndex, 72, true, triggers, triggerStrings)) {
			return false;
		}
		if (!decode(entityIndex, 24, false, entities, entityStrings)) {
			return false;
		}
		out.triggerList = triggerIndex != list.assets.size() ? trace.assets[triggerIndex].offset : 0;
		out.entityList = trace.assets[entityIndex].offset;
		out.entityListEnd = entityIndex + 1 < trace.assets.size() ? trace.assets[entityIndex + 1].offset : stream.size();

		// A reference no decode explains points into another asset's data: replay the asset whose block
		// range holds it (every position only grows through the load, so that is the last asset starting at
		// or before it) with its own loader, which logs its strings too.
		std::unordered_set<std::size_t> replayed;
		auto owner = [&](std::uint64_t target) -> std::size_t {
			const int block = static_cast<int>(target >> 60);
			const std::uint64_t position = target & 0x0FFFFFFFFFFFFFFFull;
			std::size_t lo = 0, hi = trace.assetCount;
			while (hi - lo > 1) {
				const std::size_t mid = (lo + hi) / 2;
				if (trace.assets[mid].pos[block] <= position) {
					lo = mid;
				}
				else {
					hi = mid;
				}
			}
			return lo;
		};
		for (const EntityStrings* strings : { &triggerStrings, &entityStrings }) {
			for (const EntityStrings::Pending& ref : strings->pending) {
				const std::uint64_t target = ref.pointer - 1;
				if (log.contains(target)) {
					continue;
				}
				const std::size_t index = owner(target);
				if (!replayed.insert(index).second) {
					continue;
				}
				const AssetLoader loader = FindAssetLoader(list.assets[index].type);
				if (!loader) {
					// No loader: when the asset's stream is its root followed by nothing but block-4 data (the
					// only stored block, other than temp and the preload roots' block 1, that it moved), a
					// block-4 position maps straight to a stream offset. Read the string there.
					const TracedAsset& begin = trace.assets[index];
					const TracedAsset& end = trace.assets[index + 1];
					bool onlyVirtual = true;
					for (std::size_t b = 2; b < kXBlockCount; ++b) {
						if (b != XBlockVirtual && XBlockIsStored(static_cast<int>(b)) && end.pos[b] != begin.pos[b]) {
							onlyVirtual = false;
						}
					}
					const std::uint64_t data = end.pos[XBlockVirtual] - begin.pos[XBlockVirtual];
					const std::size_t size = end.offset - begin.offset;
					if (!onlyVirtual || (target >> 60) != XBlockVirtual || data > size) {
						continue;
					}
					for (const EntityStrings::Pending& other : strings->pending) {
						const std::uint64_t at = other.pointer - 1;
						if ((at >> 60) != XBlockVirtual || owner(at) != index) {
							continue;
						}
						const std::uint64_t position = at & 0x0FFFFFFFFFFFFFFFull;
						const std::size_t offset = begin.offset + (size - static_cast<std::size_t>(data))
							+ static_cast<std::size_t>(position - begin.pos[XBlockVirtual]);
						if (offset >= end.offset) {
							continue;
						}
						const auto* text = reinterpret_cast<const char*>(stream.data() + offset);
						const std::size_t length = strnlen(text, end.offset - offset);
						if (offset + length < end.offset && std::all_of(text, text + length, [](char c) { return c >= 0x20 && c < 0x7F; })) {
							log.emplace(at, std::string(text, length));
						}
					}
					continue;
				}
				const TracedAsset& start = trace.assets[index];
				XStream s(stream, 0);
				s.Restore(start.offset, start.block, start.pos);
				s.LogStrings(&log);
				loader(s, list.assets[index].header);
			}
		}

		out.unresolved = 0;
		auto resolve = [&](std::vector<MapEntity>& found, const EntityStrings& strings) {
			for (const EntityStrings::Pending& ref : strings.pending) {
				EntityKey& key = found[ref.entity].keys[ref.key];
				std::string& field = ref.isKey ? key.key : key.text;
				const auto hit = log.find(ref.pointer - 1);
				if (hit != log.end()) {
					field = hit->second;
				}
				else {
					const std::size_t index = owner(ref.pointer - 1);
					field = std::format("<ref {:x} in asset {} ({})>", ref.pointer, index, XAssetTypeName(list.assets[index].type));
					++out.unresolved;
				}
			}
		};
		resolve(triggers, triggerStrings);
		resolve(entities, entityStrings);
		out.entities = std::move(triggers);
		out.entities.insert(out.entities.end(), std::make_move_iterator(entities.begin()),
			std::make_move_iterator(entities.end()));
		return true;
	}

	void SetEntityOrigin(MapEntity& entity, const std::array<float, 3>& origin, std::optional<float> yaw) {
		entity.origin = origin;
		if (yaw) {
			entity.angles[1] = *yaw;
		}
		for (EntityKey& key : entity.keys) {
			if (key.type != EntityValueType::Vector) {
				continue;
			}
			if (key.key == "origin") {
				key.vector = origin;
			}
			else if (key.key == "angles" && yaw) {
				key.vector[1] = *yaw;
			}
		}
	}

	namespace {
		// Load_MapEntityArray's data (the caller has written the inline pointer to it): the 48-byte entities,
		// then each one's keys and strings.
		void WriteMapEntityArray(XWriter& w, std::span<const MapEntity> entities) {
			w.Alloc(8);
			for (const MapEntity& entity : entities) {
				std::array<std::uint8_t, kEntitySize> head = entity.raw;
				PutAt(std::span<std::uint8_t>(head), 0, static_cast<std::uint32_t>(entity.keys.size()));
				PutAt(std::span<std::uint8_t>(head), 8, entity.keys.empty() ? kPtrNull : kPtrInline);
				PutAt(std::span<std::uint8_t>(head), 16, entity.field16);
				PutAt(std::span<std::uint8_t>(head), 20, entity.id);
				for (std::size_t axis = 0; axis < 3; ++axis) {
					PutAt(std::span<std::uint8_t>(head), 24 + 4 * axis, entity.origin[axis]);
					PutAt(std::span<std::uint8_t>(head), 36 + 4 * axis, entity.angles[axis]);
				}
				w.Write(head.data(), head.size());
			}
			for (const MapEntity& entity : entities) {
				if (entity.keys.empty()) {
					continue;
				}
				// A key or String value stored null stays null; any other becomes an inline string.
				// A key made here (raw all zero) with text is stored inline too.
				auto stored = [](const EntityKey& key, std::size_t at) {
					const std::string& text = at == 0 ? key.key : key.text;
					return Get<std::uint64_t>(key.raw, at) != kPtrNull || !text.empty();
				};
				w.Alloc(8);
				for (const EntityKey& key : entity.keys) {
					std::array<std::uint8_t, kKeySize> kv = key.raw;
					PutAt(std::span<std::uint8_t>(kv), 0, stored(key, 0) ? kPtrInline : kPtrNull);
					switch (key.type) {
					case EntityValueType::String:
						PutAt(std::span<std::uint8_t>(kv), 8, stored(key, 8) ? kPtrInline : kPtrNull);
						break;
					case EntityValueType::Vector:
						for (std::size_t axis = 0; axis < 3; ++axis) {
							PutAt(std::span<std::uint8_t>(kv), 8 + 4 * axis, key.vector[axis]);
						}
						break;
					case EntityValueType::Hash:
						PutAt(std::span<std::uint8_t>(kv), 8, key.hash);
						break;
					case EntityValueType::Float:
						PutAt(std::span<std::uint8_t>(kv), 8, key.number);
						break;
					case EntityValueType::Int:
						PutAt(std::span<std::uint8_t>(kv), 8, key.integer);
						break;
					default:
						break;
					}
					PutAt(std::span<std::uint8_t>(kv), 24, static_cast<std::uint16_t>(key.type));
					PutAt(std::span<std::uint8_t>(kv), 26, key.flags);
					w.Write(kv.data(), kv.size());
				}
				for (const EntityKey& key : entity.keys) {
					if (stored(key, 0)) {
						w.Alloc(1);
						w.WriteString(key.key);
					}
					if (key.type == EntityValueType::String && stored(key, 8)) {
						w.Alloc(1);
						w.WriteString(key.text);
					}
				}
			}
		}
	}

	void EncodeEntityList(XWriter& w, std::uint64_t name, std::span<const MapEntity> entities) {
		// Load_EntitylistAsset(0, &header): the root in the temp block, then (block 4) Load_MapEntityArray.
		w.Push(XBlockTemp);
		w.Alloc(8);
		w.Put(name);
		w.Put(static_cast<std::uint64_t>(entities.size()));
		w.Put(entities.empty() ? kPtrNull : kPtrInline);
		w.Push(XBlockVirtual);
		if (!entities.empty()) {
			WriteMapEntityArray(w, entities);
		}
		w.Pop();
		w.Pop();
	}

	void EncodeTriggerList(XWriter& w, std::uint64_t name, const TriggerShapes& shapes, std::span<const MapEntity> entities) {
		// Load_TriggerlistAsset: the 72-byte root in the temp block, then (block 4) Load_TriggerList: the three
		// shape arrays (Alloc(4) each), then the entity array.
		w.Push(XBlockTemp);
		w.Alloc(8);
		w.Put(name);
		const auto array = [&w](std::size_t count) {
			w.Put(static_cast<std::uint32_t>(count));
			w.Put(std::uint32_t(0));
			w.Put(count ? kPtrInline : kPtrNull);
		};
		array(shapes.models.size());
		array(shapes.hulls.size());
		array(shapes.slabs.size());
		w.Put(static_cast<std::uint64_t>(entities.size()));
		w.Put(entities.empty() ? kPtrNull : kPtrInline);
		w.Push(XBlockVirtual);
		const auto write = [&w](const auto& items) {
			if (!items.empty()) {
				w.Alloc(4);
				for (const auto& item : items) {
					w.Write(item.data(), item.size());
				}
			}
		};
		write(shapes.models);
		write(shapes.hulls);
		write(shapes.slabs);
		if (!entities.empty()) {
			WriteMapEntityArray(w, entities);
		}
		w.Pop();
		w.Pop();
	}

	std::size_t AddTriggerBox(TriggerShapes& shapes, std::uint32_t contents, const std::array<float, 3>& halfSize, float yaw) {
		const float radians = yaw * 3.14159265358979f / 180.0f;
		float c = std::cos(radians);
		float s = std::sin(radians);
		// A quarter turn is still an axis-aligned box: swap the sides, no slabs.
		const bool square = std::fabs(c) < 1e-4f || std::fabs(s) < 1e-4f;
		if (square) {
			c = std::round(c);
			s = std::round(s);
		}
		const float ax = std::fabs(c) * halfSize[0] + std::fabs(s) * halfSize[1];
		const float ay = std::fabs(s) * halfSize[0] + std::fabs(c) * halfSize[1];

		std::array<std::uint8_t, 32> hull{};
		const std::span<std::uint8_t> h(hull);
		PutAt(h, 12, ax);
		PutAt(h, 16, ay);
		PutAt(h, 20, halfSize[2]);
		PutAt(h, 24, contents);
		PutAt(h, 28, static_cast<std::uint16_t>(square ? 0 : 2));
		PutAt(h, 30, static_cast<std::uint16_t>(shapes.slabs.size()));
		if (!square) {
			// The box's own X and Y axes.
			const std::array<std::array<float, 3>, 2> dirs = { { { c, s, 0.0f }, { -s, c, 0.0f } } };
			for (std::size_t axis = 0; axis < 2; ++axis) {
				std::array<std::uint8_t, 20> slab{};
				const std::span<std::uint8_t> b(slab);
				PutAt(b, 0, dirs[axis][0]);
				PutAt(b, 4, dirs[axis][1]);
				PutAt(b, 8, dirs[axis][2]);
				PutAt(b, 16, halfSize[axis]);
				shapes.slabs.push_back(slab);
			}
		}

		std::array<std::uint8_t, 8> model{};
		const std::span<std::uint8_t> m(model);
		PutAt(m, 0, contents);
		PutAt(m, 4, std::uint16_t(1));
		PutAt(m, 6, static_cast<std::uint16_t>(shapes.hulls.size()));
		shapes.hulls.push_back(hull);
		shapes.models.push_back(model);
		return shapes.models.size() - 1;
	}
}
