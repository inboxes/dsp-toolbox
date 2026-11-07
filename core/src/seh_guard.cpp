#include "dsptb/core/seh_guard.hpp"

#include <Windows.h>

namespace dsptb {

bool sehGuarded(void (*fn)(void*), void* ctx, unsigned long* code) noexcept {
    __try {
        fn(ctx);
        return true;
    } __except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace dsptb
