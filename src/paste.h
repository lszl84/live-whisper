#pragma once

#include <string>

namespace paste {

// Check whether the live-whisper GNOME Shell extension is loaded and
// available on D-Bus.
bool is_available();

// Try to enable the GNOME Shell extension via D-Bus. Returns true if
// the extension was already enabled or was successfully enabled.
// Call this before is_available() — it does NOT wait for the extension
// to load; call is_available() again after a short delay.
bool try_enable_extension();

// Type text into the currently focused window via the GNOME Shell
// extension's Clutter virtual keyboard. Returns true on success.
bool type_text(const std::string& text);

} // namespace paste
