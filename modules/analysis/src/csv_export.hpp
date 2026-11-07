#pragma once

// In its own header so Windows.h stays out of the modules' sources.
#include <string>

namespace dsptb::analysis {

// Writes <data dir>/exports/<module>-<date-time>.csv and returns its path as UTF-8, or empty on failure. Modules
// can't open a file dialog, because the ABI has no call for one.
std::string writeCsv(const char* moduleName, const std::string& text);

} // namespace dsptb::analysis
