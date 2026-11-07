#pragma once

namespace dsptb::dsp {

// Version of the dsptb_dsp_common library, as a string (e.g. "0.1.0").
// Allocation-free and noexcept: safe to call from any thread.
const char* version() noexcept;

} // namespace dsptb::dsp
