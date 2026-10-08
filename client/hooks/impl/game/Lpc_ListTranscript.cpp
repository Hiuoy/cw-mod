#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/dump_anchors.hpp"

#include <atomic>
#include <cstring>
#include <string>

// B3: why the game rejects our objectstore LPC list. Read-only transcript; every detour calls the
// original first and changes nothing.
//
// Boot 2026-09-16 22:56: the server answered `tu34_340c83f3b33348f5` with 4 objects (status 200, JSON
// content type), the game asked again 15 s later, and .manifest was never rewritten. So the list
// task ended in state 3 (failed) and Lpc_OnListFailed re-armed the retry. Four places can fail it:
//
//   BdRemoteHttpTask_FinishRow         status not 2xx, or the resource parser returned 0
//   PublisherObjectsResource_Parse     body not JSON, no "objects", or any metadata/tags parse failed
//   ObjectMetadata_ParseJson           one required key missing, wrong type, or over its buffer size
//   Lpc_WriteManifest / OnListFailed   which callback the task manager actually ran
//
// ObjectMetadata_ParseJson fills its output in key order (checksum, objectVersion, expiresOn, created,
// modified, acl, contentLength, context, name/owner, contentURL, category), so on a failure the first
// field still empty is the key it stopped at. Each detour logs its first 16 calls.

namespace {
	constexpr int kMaxLines = 16;
	std::atomic<int> g_FinishRowLines{ 0 };
	std::atomic<int> g_ParseLines{ 0 };
	std::atomic<int> g_MetadataLines{ 0 };

	bool Budget(std::atomic<int>& counter) {
		return counter.fetch_add(1, std::memory_order_relaxed) < kMaxLines;
	}

	std::string ReadCString(const void* base, std::size_t offset, std::size_t size) {
		std::string out(size, '\0');
		if (!Client::Game::SafeCopy(out.data(), static_cast<const char*>(base) + offset, size)) {
			return "<unreadable>";
		}
		out.resize(strnlen(out.data(), size));
		return out;
	}

	template <typename T>
	T ReadField(const void* base, std::size_t offset) {
		T value{};
		Client::Game::SafeRead(static_cast<const char*>(base) + offset, value);
		return value;
	}

	int TaskState(void** task) {
		void* result = nullptr;
		if (!task || !Client::Game::SafeRead(task + 6, result) || !result) return -1;
		return ReadField<int>(result, 36);
	}
}

template <>
std::uint64_t Client::Hook::Hooks::HK_BdRemoteHttpTask_FinishRow::hkCallback(void** task, void* response) {
	const int before = TaskState(task);
	const std::uint64_t ret = m_Original(task, response);
	if (Budget(g_FinishRowLines)) {
		void* resource = nullptr;
		Game::SafeRead(task + 3, resource);
		void* result = nullptr;
		Game::SafeRead(task + 6, result);
		LOG("LpcList", INFO,
			"http task finish: status={} state {} -> {} (2 done, 3 failed) errorCode={} resource={}",
			ReadField<std::uint32_t>(response, 8), before, TaskState(task),
			result ? ReadField<int>(result, 88) : -1, static_cast<void*>(resource));
	}
	return ret;
}

template <>
std::uint8_t Client::Hook::Hooks::HK_PublisherObjectsResource_Parse::hkCallback(void* resource, void* response) {
	const std::uint8_t ret = m_Original(resource, response);
	if (Budget(g_ParseLines)) {
		LOG("LpcList", INFO,
			"category list parse -> {} (1 ok; needs nextPageToken null|string): objects stored={} "
			"bodyContentType={} bodyLen={}",
			ret, ReadField<std::uint32_t>(resource, 76), ReadField<std::uint32_t>(response, 10804),
			ReadField<std::uint32_t>(response, 15208));
	}
	return ret;
}

template <>
std::uint8_t Client::Hook::Hooks::HK_ObjectMetadata_ParseJson::hkCallback(void* metadata, void* json,
	std::uint32_t ownerType) {
	const std::uint8_t ret = m_Original(metadata, json, ownerType);
	if (Budget(g_MetadataLines)) {
		LOG("LpcList", INFO,
			"metadata parse -> {} (1 ok) ownerType={} jsonType={} | checksum='{}' objectVersion='{}' "
			"expiresOn={} created={} modified={} acl={} contentLength={} context='{}' name='{}' "
			"contentURL='{}' category='{}'",
			ret, ownerType, ReadField<int>(json, 0),
			ReadCString(metadata, 128, 33), ReadCString(metadata, 161, 33),
			ReadField<std::int64_t>(metadata, 200), ReadField<std::int64_t>(metadata, 224),
			ReadField<std::int64_t>(metadata, 232), ReadField<std::uint32_t>(metadata, 216),
			ReadField<std::uint64_t>(metadata, 208), ReadCString(metadata, 0, 16),
			ReadCString(metadata, 50, 65), ReadCString(metadata, 240, 96), ReadCString(metadata, 2288, 65));
	}
	return ret;
}

template <>
std::uint64_t Client::Hook::Hooks::HK_Lpc_WriteManifest::hkCallback(std::uint32_t controller) {
	LOG("LpcList", INFO, "list task DONE -> Lpc_WriteManifest (controller {})", controller);
	return m_Original(controller);
}

template <>
std::uint64_t Client::Hook::Hooks::HK_Lpc_OnListFailed::hkCallback(std::uint32_t controller) {
	LOG("LpcList", WARN, "list task FAILED -> Lpc_OnListFailed, 15 s retry (controller {})", controller);
	return m_Original(controller);
}
