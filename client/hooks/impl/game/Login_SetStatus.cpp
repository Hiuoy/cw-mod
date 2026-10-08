#include "common.hpp"
#include "hooks/hook.hpp"
#include "game/dw_backend.hpp"

// Read-only mirror of the Demonware login state machine's status setter (sub_7FF729DECF80 calls this
// at every transition). The status strings it receives — "Fetching First-Party Token", "Studio auth
// login detected, starting Auth task", "Studio auth login failed to build token", "Authenticating to
// Demonware", "Authenticated to Demonware", "Connecting to LSG", "Login Complete", and every failure
// reason — normally go only to the game's internal log. We echo them into our console so we can see
// exactly how far login progresses (or that it never starts). This never alters behaviour: it always
// forwards to the original with the arguments untouched.
template <>
std::uint64_t Client::Hook::Hooks::HK_Login_SetStatus::hkCallback(void* ctx, const char* status, std::uint32_t code) {
	if (status) {
		LOG("Login", INFO, "[status code={}] {}", code, status);
	}
	return m_Original(ctx, status, code);
}
