#pragma once

#include <type_traits>

namespace dsptb {

// SEH trampoline. Calls fn(ctx) inside __try/__except. Returns false when a structured
// exception (an access violation, a divide by zero, or a C++ exception) escaped, with its code in *code.
//
// This is best-effort containment for a misbehaving module: it cannot repair memory corruption, and C++
// objects between the throw and this frame are not destroyed (SEH does not unwind them). It lives in its own
// small function because MSVC does not allow __try in functions that need C++ unwinding.
bool sehGuarded(void (*fn)(void*), void* ctx, unsigned long* code) noexcept;

// guarded([&]{ ...call module code... }) -> true if it ran to completion.
template <class F> bool guarded(F&& f, unsigned long* code = nullptr) noexcept {
    using Fn = std::remove_reference_t<F>;
    const auto thunk = +[](void* p) { (*static_cast<Fn*>(p))(); };
    unsigned long dummy = 0;
    return sehGuarded(thunk, const_cast<void*>(static_cast<const void*>(&f)), code ? code : &dummy);
}

} // namespace dsptb
