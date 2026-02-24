#pragma once

#include <string>

namespace paste {

// Capture the currently focused window address via hyprctl.
std::string capture_focus();

// Refocus a window by its address.
bool refocus(const std::string& addr);

// Type text into the focused window via zwp_virtual_keyboard_v1.
bool type_text(const std::string& text);

// Refocus the given window and type text.
bool refocus_and_type(const std::string& addr, const std::string& text);

} // namespace paste
