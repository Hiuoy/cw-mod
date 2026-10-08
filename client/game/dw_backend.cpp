#include "common.hpp"
#include "game/dw_backend.hpp"
#include "game/dump_anchors.hpp"
#include "game/settings.hpp"
#include <utility/nt.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace Client::Game::DwBackend {
	bool g_Enabled = false;
	static PatchStatus s_Status{};

	namespace {
		// Standard prefix of a DER SubjectPublicKeyInfo for an RSA-2048 key (SEQUENCE, AlgorithmId
		// rsaEncryption, BIT STRING wrapper). Any valid RSA-2048 SPKI — the game's or ours — starts
		// with these exact 24 bytes, so matching them at the target address confirms we are pointed
		// at a real embedded key and not clobbering something else on a build mismatch.
		constexpr std::array<std::uint8_t, 24> kRsa2048SpkiPrefix = {
			0x30,0x82,0x01,0x22,0x30,0x0D,0x06,0x09,0x2A,0x86,0x48,0x86,
			0xF7,0x0D,0x01,0x01,0x01,0x05,0x00,0x03,0x82,0x01,0x0F,0x00
		};

		std::filesystem::path KeysDir() {
			std::error_code ec;
			return std::filesystem::current_path(ec) / "cw-mod" / "dwserver";
		}

		// Read exactly kDwPubKeyDerLen bytes of a DER key; empty vector on any problem.
		std::vector<std::uint8_t> ReadKey(const std::filesystem::path& path) {
			std::ifstream in(path, std::ios::binary);
			if (!in) return {};
			std::vector<std::uint8_t> buf((std::istreambuf_iterator<char>(in)),
				std::istreambuf_iterator<char>());
			if (buf.size() != kDwPubKeyDerLen) {
				LOG("DwBackend", WARN, "{} is {} bytes, expected {} — ignoring.",
					path.string(), buf.size(), kDwPubKeyDerLen);
				return {};
			}
			return buf;
		}

		bool PrefixMatches(const std::uint8_t* p) {
			for (std::size_t i = 0; i < kRsa2048SpkiPrefix.size(); ++i) {
				if (p[i] != kRsa2048SpkiPrefix[i]) return false;
			}
			return true;
		}

		// Guarded in-memory replace of a 294-byte key blob. Verifies the target holds a real RSA
		// SPKI first, flips the page to writable, copies, restores protection, flushes the icache,
		// and reads back to confirm. Returns false (image untouched) on any mismatch.
		bool ReplaceKey(const char* label, std::uintptr_t dumpAbs, std::uintptr_t moduleBase,
			const std::vector<std::uint8_t>& newKey) {
			if (newKey.size() != kDwPubKeyDerLen) return false;

			const Common::Utility::NT::Library game;
			const std::uintptr_t size = game.GetOptionalHeader()->SizeOfImage;
			const std::uintptr_t addr = moduleBase + (dumpAbs - kDumpImagebase);
			if (addr < moduleBase || addr + kDwPubKeyDerLen > moduleBase + size) {
				LOG("DwBackend", ERROR, "{}: target 0x{:X} out of module range (build mismatch?).", label, addr);
				return false;
			}

			auto* target = reinterpret_cast<std::uint8_t*>(addr);
			if (!PrefixMatches(target)) {
				LOG("DwBackend", ERROR, "{}: no RSA-2048 SPKI at 0x{:X} — refusing to patch (build mismatch?).",
					label, addr);
				return false;
			}
			if (std::equal(newKey.begin(), newKey.end(), target)) {
				LOG("DwBackend", INFO, "{}: already holds our key; nothing to do.", label);
				return true;
			}

			DWORD oldProtect = 0;
			if (!VirtualProtect(target, kDwPubKeyDerLen, PAGE_EXECUTE_READWRITE, &oldProtect)) {
				LOG("DwBackend", ERROR, "{}: VirtualProtect(RW) failed (err {}).", label, GetLastError());
				return false;
			}
			std::memcpy(target, newKey.data(), kDwPubKeyDerLen);
			DWORD tmp = 0;
			VirtualProtect(target, kDwPubKeyDerLen, oldProtect, &tmp);
			FlushInstructionCache(GetCurrentProcess(), target, kDwPubKeyDerLen);

			const bool ok = std::equal(newKey.begin(), newKey.end(), target);
			if (ok) {
				LOG("DwBackend", INFO, "{}: key replaced and verified at 0x{:X}.", label, addr);
			}
			else {
				LOG("DwBackend", ERROR, "{}: read-back MISMATCH at 0x{:X} (Arxan reverted?).", label, addr);
			}
			return ok;
		}
	}

	PatchStatus PatchEmbeddedKeys(std::uintptr_t moduleBase) {
		s_Status = PatchStatus{};

		// Opt-in is the presence of the key files: a user who has stood up the local server drops
		// material/*_pub.der into <game>/cw-mod/dwserver/. Absent that, or with "backend": false in
		// cw-mod.json, we do nothing and leave the image (and the normal offline path) untouched.
		if (!Settings::Get().backend) {
			s_Status.detail = "\"backend\" is false in cw-mod.json";
			LOG("DwBackend", INFO, "Key patch skipped: {}.", s_Status.detail);
			return s_Status;
		}
		const std::filesystem::path dir = KeysDir();
		const auto authKey = ReadKey(dir / "auth_pub.der");
		const auto lsgKey = ReadKey(dir / "lsg_pub.der");
		s_Status.authKeyFileFound = !authKey.empty();
		s_Status.lsgKeyFileFound = !lsgKey.empty();

		if (authKey.empty() && lsgKey.empty()) {
			s_Status.detail = "no key files in " + dir.string() +
				" (run tools/dwserver/gen_keys.py and copy material/*_pub.der there)";
			LOG("DwBackend", WARN, "Key patch skipped: {}.", s_Status.detail);
			return s_Status;
		}

		if (!authKey.empty()) {
			s_Status.authReplaced = ReplaceKey("auth", kDump_g_dwAuthSigPubKey_DER, moduleBase, authKey);
		}
		if (!lsgKey.empty()) {
			s_Status.lsgReplaced = ReplaceKey("lsg", kDump_g_lsgHandshakePubKey_DER, moduleBase, lsgKey);
		}

		// "Enabled" means we actually installed our auth key: only then should the startup path
		// leave the DW connection active (skip com_noDW) and expect our server to answer.
		g_Enabled = s_Status.authReplaced;

		s_Status.detail = std::format("enabled={} auth={} lsg={}",
			g_Enabled,
			s_Status.authReplaced ? "replaced" : "no",
			s_Status.lsgReplaced ? "replaced" : "no");
		LOG("DwBackend", INFO, "Key patch done: {}.", s_Status.detail);
		return s_Status;
	}

	const PatchStatus& LastStatus() { return s_Status; }
}
