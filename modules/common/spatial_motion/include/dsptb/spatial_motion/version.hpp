#pragma once

namespace dsptb::spatial {

// Version of the dsptb_spatial_motion library, as a string (e.g. "0.1.0").
// Allocation-free and noexcept: safe to call from any thread.
const char* version() noexcept;

} // namespace dsptb::spatial
