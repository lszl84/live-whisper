#include "paste.h"

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "virtual-keyboard-unstable-v1-client-protocol.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string exec_cmd(const char* cmd)
{
    std::array<char, 512> buf;
    std::string result;
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return {};
    while (std::fgets(buf.data(), buf.size(), pipe))
        result += buf.data();
    pclose(pipe);
    return result;
}

static std::string json_string_value(const std::string& json, const char* key)
{
    std::string needle = std::string("\"") + key + "\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return {};
    ++pos;
    auto end = json.find('"', pos);
    if (end == std::string::npos) return {};
    return json.substr(pos, end - pos);
}

// ---------------------------------------------------------------------------
// Virtual keyboard typing
// ---------------------------------------------------------------------------

// Resolved key: an evdev keycode + the xkb modifier mask needed.
struct ResolvedKey {
    xkb_keycode_t keycode;
    xkb_mod_mask_t mods;
};

// Build the default XKB keymap string and return it.
static std::string get_default_keymap_string()
{
    xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) return {};

    xkb_rule_names names{};  // all nullptr → system defaults
    xkb_keymap* km = xkb_keymap_new_from_names(ctx, &names,
                                                XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) { xkb_context_unref(ctx); return {}; }

    char* str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    std::string result(str);
    free(str);

    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
    return result;
}

// Create an anonymous file (memfd) containing the keymap string.
// Returns fd and sets *out_size. Caller must close fd.
static int create_keymap_fd(const std::string& keymap_str, uint32_t* out_size)
{
    *out_size = static_cast<uint32_t>(keymap_str.size() + 1);  // include NUL

    int fd = memfd_create("xkb-keymap", MFD_CLOEXEC);
    if (fd < 0) return -1;

    if (write(fd, keymap_str.c_str(), *out_size) != static_cast<ssize_t>(*out_size)) {
        close(fd);
        return -1;
    }
    lseek(fd, 0, SEEK_SET);
    return fd;
}

// Resolve a Unicode codepoint to an evdev keycode + required modifier mask.
static bool resolve_char(xkb_keymap* keymap, xkb_state* state,
                         uint32_t codepoint, ResolvedKey* out)
{
    xkb_keysym_t target = xkb_utf32_to_keysym(codepoint);
    if (target == XKB_KEY_NoSymbol) return false;

    // Special case: newline → Return key
    if (codepoint == '\n') {
        target = XKB_KEY_Return;
    }

    xkb_keycode_t min = xkb_keymap_min_keycode(keymap);
    xkb_keycode_t max = xkb_keymap_max_keycode(keymap);

    for (xkb_keycode_t kc = min; kc <= max; ++kc) {
        int num_layouts = xkb_keymap_num_layouts_for_key(keymap, kc);
        for (int layout = 0; layout < num_layouts; ++layout) {
            int num_levels = xkb_keymap_num_levels_for_key(keymap, kc, layout);
            for (int level = 0; level < num_levels; ++level) {
                const xkb_keysym_t* syms;
                int nsyms = xkb_keymap_key_get_syms_by_level(keymap, kc, layout,
                                                              level, &syms);
                for (int s = 0; s < nsyms; ++s) {
                    if (syms[s] == target) {
                        out->keycode = kc;
                        // Figure out which modifiers produce this level
                        xkb_mod_mask_t masks[64];
                        int nmasks = xkb_keymap_key_get_mods_for_level(
                            keymap, kc, layout, level, masks, 64);
                        out->mods = nmasks > 0 ? masks[0] : 0;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

// Extract UTF-32 codepoints from a UTF-8 string.
static std::vector<uint32_t> utf8_to_codepoints(const std::string& s)
{
    std::vector<uint32_t> result;
    const auto* p = reinterpret_cast<const unsigned char*>(s.data());
    const auto* end = p + s.size();
    while (p < end) {
        uint32_t cp;
        int len;
        if (*p < 0x80)      { cp = *p; len = 1; }
        else if (*p < 0xE0) { cp = *p & 0x1F; len = 2; }
        else if (*p < 0xF0) { cp = *p & 0x0F; len = 3; }
        else                { cp = *p & 0x07; len = 4; }
        for (int i = 1; i < len && p + i < end; ++i)
            cp = (cp << 6) | (p[i] & 0x3F);
        result.push_back(cp);
        p += len;
    }
    return result;
}

// Registry listener data for virtual keyboard setup.
struct VkbdRegistryData {
    wl_seat*                          seat = nullptr;
    zwp_virtual_keyboard_manager_v1*  mgr  = nullptr;
};

static void vkbd_registry_global(void* data, wl_registry* reg,
                                  uint32_t name, const char* iface, uint32_t /*version*/)
{
    auto* d = static_cast<VkbdRegistryData*>(data);
    if (std::strcmp(iface, wl_seat_interface.name) == 0 && !d->seat)
        d->seat = static_cast<wl_seat*>(
            wl_registry_bind(reg, name, &wl_seat_interface, 1));
    else if (std::strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name) == 0)
        d->mgr = static_cast<zwp_virtual_keyboard_manager_v1*>(
            wl_registry_bind(reg, name,
                             &zwp_virtual_keyboard_manager_v1_interface, 1));
}

static void vkbd_registry_global_remove(void*, wl_registry*, uint32_t) {}

static const wl_registry_listener vkbd_registry_listener = {
    vkbd_registry_global,
    vkbd_registry_global_remove,
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace paste {

std::string capture_focus()
{
    std::string output = exec_cmd("hyprctl -j activewindow");
    return json_string_value(output, "address");
}

bool refocus(const std::string& addr)
{
    if (addr.empty()) return false;
    std::string cmd = "hyprctl dispatch focuswindow address:" + addr + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

bool type_text(const std::string& text)
{
    if (text.empty()) return true;

    // Connect to Wayland
    wl_display* display = wl_display_connect(nullptr);
    if (!display) {
        std::fprintf(stderr, "paste: wl_display_connect failed\n");
        return false;
    }

    VkbdRegistryData rdata;
    wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &vkbd_registry_listener, &rdata);
    wl_display_roundtrip(display);

    if (!rdata.seat || !rdata.mgr) {
        std::fprintf(stderr, "paste: missing seat or virtual-keyboard-manager\n");
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return false;
    }

    // Create virtual keyboard
    zwp_virtual_keyboard_v1* vkbd =
        zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(rdata.mgr, rdata.seat);

    // Set up XKB keymap
    std::string keymap_str = get_default_keymap_string();
    if (keymap_str.empty()) {
        std::fprintf(stderr, "paste: failed to get keymap\n");
        zwp_virtual_keyboard_v1_destroy(vkbd);
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return false;
    }

    uint32_t keymap_size;
    int keymap_fd = create_keymap_fd(keymap_str, &keymap_size);
    if (keymap_fd < 0) {
        std::fprintf(stderr, "paste: failed to create keymap fd\n");
        zwp_virtual_keyboard_v1_destroy(vkbd);
        wl_registry_destroy(registry);
        wl_display_disconnect(display);
        return false;
    }

    zwp_virtual_keyboard_v1_keymap(vkbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                   keymap_fd, keymap_size);
    close(keymap_fd);
    wl_display_roundtrip(display);

    // Build XKB state for resolving characters to keycodes
    xkb_context* xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    xkb_keymap* keymap = xkb_keymap_new_from_string(
        xkb_ctx, keymap_str.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    xkb_state* state = xkb_state_new(keymap);

    // Type each character
    auto codepoints = utf8_to_codepoints(text);
    uint32_t time_ms = 0;

    for (uint32_t cp : codepoints) {
        ResolvedKey rk;
        if (!resolve_char(keymap, state, cp, &rk)) continue;

        uint32_t evdev_key = rk.keycode - 8;  // xkb keycode to evdev

        // Press modifiers if needed
        if (rk.mods != 0) {
            zwp_virtual_keyboard_v1_modifiers(vkbd, rk.mods, 0, 0, 0);
        }

        // Key press
        zwp_virtual_keyboard_v1_key(vkbd, time_ms++, evdev_key, 1);
        wl_display_roundtrip(display);

        // Key release
        zwp_virtual_keyboard_v1_key(vkbd, time_ms++, evdev_key, 0);

        // Release modifiers
        if (rk.mods != 0) {
            zwp_virtual_keyboard_v1_modifiers(vkbd, 0, 0, 0, 0);
        }
        wl_display_roundtrip(display);
    }

    // Cleanup
    xkb_state_unref(state);
    xkb_keymap_unref(keymap);
    xkb_context_unref(xkb_ctx);

    zwp_virtual_keyboard_v1_destroy(vkbd);
    wl_display_roundtrip(display);

    zwp_virtual_keyboard_manager_v1_destroy(rdata.mgr);
    wl_seat_destroy(rdata.seat);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);

    return true;
}

bool refocus_and_type(const std::string& addr, const std::string& text)
{
    if (!refocus(addr)) return false;
    usleep(50000);  // 50ms for focus to settle
    return type_text(text);
}

} // namespace paste
