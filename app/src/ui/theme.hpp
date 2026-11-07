#pragma once

namespace dsptb::ui {

// Loads the UI font once: Segoe UI from the system fonts folder, or ImGui's built-in font if it is missing.
// Returns true if Segoe UI was used. Call after ImGui_ImplDX11_Init (the atlas is backend-managed).
bool loadFonts();

// Applies the dark theme and scales all sizes and the font by `scale` (monitor DPI x UI scale).
// Safe to call repeatedly: it always starts again from the unscaled base style.
void applyTheme(float scale);

} // namespace dsptb::ui
