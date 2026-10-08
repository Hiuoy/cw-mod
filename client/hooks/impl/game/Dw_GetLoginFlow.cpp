#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/dw_backend.hpp"

// Force the bdLogin flow/service selector to 9 ("studio auth") while the local Demonware backend
// is active. On a retail Battle.net build the real flow makes login block on Battle.net
// first-party tokens (session/account) that nothing provides, so it never advances to the
// Demonware auth POST — our server saw zero traffic and the menus sat in the trial/locked state.
// Flow 9 routes the login state machine straight to the Auth task (studio token is built from the
// local identity, no first-party needed). Every caller of this getter is login/auth, so returning
// 9 has no collateral. The hook is only installed when DwBackend::g_Enabled, and it still defers to
// the original when disabled, so normal offline behaviour is untouched.
template <>
std::uint64_t Client::Hook::Hooks::HK_Dw_GetLoginFlow::hkCallback(void* loginConfig) {
	if (Client::Game::DwBackend::g_Enabled) {
		return 9;
	}
	return m_Original(loginConfig);
}
