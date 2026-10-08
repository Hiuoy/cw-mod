#pragma once
#include "common.hpp"

// Calling an Arxan-guarded engine function from our module.
//
// A whole class of T9 functions opens with a caller check. lua_getfield (dump 0x7FF729E3CC50) is
// the clearest example:
//
//     rax = [rsp+28h]                          ; our return address
//     if (rax <  &__ImageBase)      goto bail  ; 0x7FF71CBC0000
//     if (rax >  0x7FF73CBC0000)    goto bail  ; imagebase + 0x20000000
//     if ([rax-5] == 0xE8) goto ok             ; return address follows a call rel32
//     if ([rax-2] == 0xFF) goto ok             ; ...or a call r/m64, any ModRM length
//     if ([rax-3] == 0xFF) goto ok
//     if ([rax-4] == 0xFF) goto ok
//     if ([rax-6] == 0xFF) goto ok
//     if ([rax-7] == 0xFF) goto ok
//     goto bail
//
// `bail` is not a crash and not an error return - the function skips its body and returns as if it
// had run. Called straight from our DLL these are SILENT no-ops, which is far worse than a fault:
// the walk appears to work and produces nothing. (Same family as the Dvar_GetInt split thunk noted
// in game.hpp, which faults instead.)
//
// The check only ever looks at the return address on the stack, so the fix is to give it one that
// lives inside the game image. We do NOT patch game code and we do NOT need a code cave - the image
// already contains the two instructions we want, at the tail of sub_7FF71D0B3D10:
//
//     7FF71D0B3D27  E8 44 7E 03 0D    call sub_7FF72A0EBC70
//     7FF71D0B3D2C  48 83 C4 28       add rsp, 28h        <-- kImageRet, RVA 0x4F3D2C
//     7FF71D0B3D30  C3                retn
//
// kImageRet is in-image and [kImageRet-5] == 0xE8, so it satisfies both halves of the check. The
// thunk hands it to the callee as the return address and parks OUR real return address 0x30 bytes
// up the stack, exactly where `add rsp,28h; retn` will go looking for it.
//
// Thunk body (30 bytes), entered by a normal call so rsp == E, E % 16 == 8, [E] = real return:
//
//     48 83 EC 30          sub  rsp, 30h        ; S = E - 0x30, still %16 == 8
//     49 BB <kImageRet>    mov  r11, imm64
//     4C 89 1C 24          mov  [rsp], r11      ; [S] = kImageRet, the return address callee sees
//     48 B8 <target>       mov  rax, imm64
//     FF E0                jmp  rax             ; callee entered with rsp == S
//
// and unwinds callee ret -> rsp = S+8, rip = kImageRet -> add rsp,28h -> rsp = S+0x30 = E ->
// retn pops the real return address. The caller is left in exactly the state a direct call would
// have left it in, rax (the return value) untouched by the gadget.
//
// Alignment and scratch: the callee is entered with rsp % 16 == 8 like any normal callee, and owns
// [S+8, S+0x30) - 0x28 bytes, comfortably more than the 0x20-byte register home space the ABI
// requires. r11 and rax are volatile and are not argument registers, so rcx/rdx/r8/r9 pass through
// untouched.
//
// LIMIT: register arguments only, so at most 4. A 5th argument is passed on the stack relative to
// the CALLER's frame, and the thunk shifts the frame by 0x30, so stack arguments would not land
// where the callee looks for them. Every function we route through this takes 4 or fewer.
namespace Client::Game::ArxanCall {
	// Resolve and validate the in-image return gadget. Verifies the bytes actually read
	// `add rsp,28h; retn` preceded by an E8, so a build bump degrades to "not ready" rather than
	// to a jump into whatever moved there. Safe to call more than once.
	bool Init(std::uintptr_t moduleBase, std::size_t imageSize);

	// True once Init found a valid gadget. When false, MakeThunk returns null and every caller
	// should report a build mismatch rather than calling the target directly - a direct call is
	// the silent-no-op case, which produces plausible-looking empty results.
	bool Ready();

	// Emit a thunk for `target`. Returns executable memory that can be called with the target's own
	// signature (<= 4 register arguments). Null if !Ready() or the thunk arena is exhausted.
	// Thunks are permanent for the life of the process; call this once per target at startup.
	void* MakeThunk(void* target);

	template <typename> struct Arity;
	template <typename R, typename... A> struct Arity<R(A...)> { static constexpr std::size_t value = sizeof...(A); };
	template <typename R, typename... A> struct Arity<R(A...) noexcept> : Arity<R(A...)> {};

	// Typed convenience wrapper. `Fn` is the plain function type, e.g. Functions::lua_getfieldT.
	template <typename Fn>
	Fn* MakeThunk(void* target) {
		// See LIMIT above. Ddl_InitInstance (7 args) went through here once: its 5th argument was
		// read off our stack and became a callback the engine later jumped to.
		static_assert(Arity<Fn>::value <= 4, "ArxanCall thunks forward register arguments only (<= 4). "
			"Call a function with stack arguments directly; it must not carry the caller check.");
		return reinterpret_cast<Fn*>(MakeThunk(target));
	}

	// The validated gadget address, for logging. Null until Init succeeds.
	void* ImageRetGadget();
}
