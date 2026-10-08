#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/game.hpp"
#include "game/game_internal.hpp"
#include "game/dw_backend.hpp"
#include "game/dump_anchors.hpp"
#include "game/settings.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <cstring>
#include <string>

// DwLogin_BuildStudioToken is the flow-9 studio-auth JWT builder. On a retail Battle.net build it
// always fails: Jwt_Sign for alg 3 (asymmetric) needs a studio signing key inline at loginConfig+5416
// which the build does not ship, so the real builder returns 0 and the state machine prints "Studio
// auth login failed to build token" (code 26). Confirmed in-game: provider(+56084)=2 (gate passes),
// origRet=0 -> the failure is the JWT sign, exactly as reversed.
//
// Since we stand up our OWN local Demonware auth server, that server is the only real verifier of this
// token. So we do not need the studio private key at all: we synthesize a well-formed unsigned
// ("alg":"none") JWT that our server recognizes and hand it back. The engine itself implements the
// "none" algorithm (Jwt_Sign case 0xC emits an empty signature), so an empty-signature JWT is a shape
// its own code produces.
//
// The output buffer is consumed purely as a C string: Jwt_Build fills it with sprintf("%s.%s.%s", ...)
// and DwLogin_StoreStudioToken does a strlen()+copy of it into userObj+4. So a null-terminated ASCII
// JWT written into outBuf (a 6784-byte stack buffer) is all that is required for the state machine to
// take the success branch -> "Studio auth login detected, starting Auth task" (state 4).
//
// We still call the original first: if a studio signing key is ever provisioned (real token builds),
// we defer to it untouched and only substitute on failure.

namespace {
	// RFC 4648 base64url, no padding — the encoding JWT segments use.
	std::string Base64Url(std::string_view in) {
		static constexpr char kAlphabet[] =
			"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
		std::string out;
		out.reserve((in.size() + 2) / 3 * 4);
		std::size_t i = 0;
		for (; i + 3 <= in.size(); i += 3) {
			const std::uint32_t n = (static_cast<std::uint8_t>(in[i]) << 16)
				| (static_cast<std::uint8_t>(in[i + 1]) << 8)
				| static_cast<std::uint8_t>(in[i + 2]);
			out.push_back(kAlphabet[(n >> 18) & 0x3F]);
			out.push_back(kAlphabet[(n >> 12) & 0x3F]);
			out.push_back(kAlphabet[(n >> 6) & 0x3F]);
			out.push_back(kAlphabet[n & 0x3F]);
		}
		if (const std::size_t rem = in.size() - i; rem == 1) {
			const std::uint32_t n = static_cast<std::uint8_t>(in[i]) << 16;
			out.push_back(kAlphabet[(n >> 18) & 0x3F]);
			out.push_back(kAlphabet[(n >> 12) & 0x3F]);
		} else if (rem == 2) {
			const std::uint32_t n = (static_cast<std::uint8_t>(in[i]) << 16)
				| (static_cast<std::uint8_t>(in[i + 1]) << 8);
			out.push_back(kAlphabet[(n >> 18) & 0x3F]);
			out.push_back(kAlphabet[(n >> 12) & 0x3F]);
			out.push_back(kAlphabet[(n >> 6) & 0x3F]);
		}
		return out;
	}

	// Build the local studio token once. header.payload with an empty signature segment ("alg":"none").
	// The payload advertises this as a cw-mod-issued studio identity so our auth server can key on it.
	// "name" is the player's cw-mod.json name: the auth server bakes it into the ticket, which is the
	// name the game shows after login (the offline SetUsername one is overwritten by it).
	// "xuid" is this PC's cw-mod.json xuid: the auth server makes it the ticket's user id, which becomes
	// the XUID other PCs see. Without it every PC was user 1, and a join to a host with our own XUID is
	// a join to ourselves. A server too old to read it still answers with its --user-id.
	const std::string& LocalStudioToken() {
		static const std::string token = [] {
			const std::string header = Base64Url(R"({"alg":"none","typ":"JWT"})");
			nlohmann::ordered_json claims{
				{ "iss", "cw-mod" }, { "prov", "studio" }, { "sub", "cwmod-local" }, { "plat", "pc" },
				{ "name", Client::Game::Settings::PlayerName() },
				{ "xuid", std::format("0x{:016X}", Client::Game::Settings::PlayerXuid()) },
			};
			const std::string payload = Base64Url(claims.dump(-1, ' ', false, nlohmann::ordered_json::error_handler_t::replace));
			return header + "." + payload + ".";
		}();
		return token;
	}
}

template <>
std::uint8_t Client::Hook::Hooks::HK_DwLogin_BuildStudioToken::hkCallback(void* loginConfig, void* userObj, void* outBuf) {
	const std::uint8_t ret = m_Original(loginConfig, userObj, outBuf);
	if (ret)
		return ret; // a real, signed token was built (studio key present) — use it untouched.

	if (!Client::Game::DwBackend::g_Enabled || !outBuf)
		return ret;

	// Substitute our locally-minted token. outBuf is the state machine's 6784-byte buffer, consumed as
	// a C string, so copy the token plus its null terminator.
	const std::string& token = LocalStudioToken();
	std::memcpy(outBuf, token.c_str(), token.size() + 1);

	std::uint32_t provider = 0xFFFFFFFFu;
	if (userObj) {
		Client::Game::SafeRead(reinterpret_cast<const void*>(
			reinterpret_cast<std::uintptr_t>(userObj) + Client::Game::kStudioUserObj_ProviderOffset), provider);
	}
	LOG("Login", INFO, "BuildStudioToken: origRet=0 provider(+{})={} -> substituted local studio token ({} bytes, name \"{}\", "
		"xuid 0x{:016X}), returning 1", Client::Game::kStudioUserObj_ProviderOffset, static_cast<int>(provider), token.size(),
		Client::Game::Settings::PlayerName(), Client::Game::Settings::PlayerXuid());
	return 1;
}
