#include "imgui_impl_wayland.h"
#include "overlay.h"

#include "imgui.h"

#include <chrono>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>

static Overlay* g_overlay = nullptr;
static std::chrono::steady_clock::time_point g_last_time;

// Map XKB keysym to ImGuiKey
static ImGuiKey keysym_to_imgui(xkb_keysym_t sym)
{
    switch (sym) {
    case XKB_KEY_Tab:           return ImGuiKey_Tab;
    case XKB_KEY_Left:          return ImGuiKey_LeftArrow;
    case XKB_KEY_Right:         return ImGuiKey_RightArrow;
    case XKB_KEY_Up:            return ImGuiKey_UpArrow;
    case XKB_KEY_Down:          return ImGuiKey_DownArrow;
    case XKB_KEY_Page_Up:       return ImGuiKey_PageUp;
    case XKB_KEY_Page_Down:     return ImGuiKey_PageDown;
    case XKB_KEY_Home:          return ImGuiKey_Home;
    case XKB_KEY_End:           return ImGuiKey_End;
    case XKB_KEY_Insert:        return ImGuiKey_Insert;
    case XKB_KEY_Delete:        return ImGuiKey_Delete;
    case XKB_KEY_BackSpace:     return ImGuiKey_Backspace;
    case XKB_KEY_space:         return ImGuiKey_Space;
    case XKB_KEY_Return:        return ImGuiKey_Enter;
    case XKB_KEY_KP_Enter:      return ImGuiKey_KeypadEnter;
    case XKB_KEY_Escape:        return ImGuiKey_Escape;

    case XKB_KEY_Shift_L:       return ImGuiKey_LeftShift;
    case XKB_KEY_Shift_R:       return ImGuiKey_RightShift;
    case XKB_KEY_Control_L:     return ImGuiKey_LeftCtrl;
    case XKB_KEY_Control_R:     return ImGuiKey_RightCtrl;
    case XKB_KEY_Alt_L:         return ImGuiKey_LeftAlt;
    case XKB_KEY_Alt_R:         return ImGuiKey_RightAlt;
    case XKB_KEY_Super_L:       return ImGuiKey_LeftSuper;
    case XKB_KEY_Super_R:       return ImGuiKey_RightSuper;

    case XKB_KEY_a: case XKB_KEY_A: return ImGuiKey_A;
    case XKB_KEY_b: case XKB_KEY_B: return ImGuiKey_B;
    case XKB_KEY_c: case XKB_KEY_C: return ImGuiKey_C;
    case XKB_KEY_d: case XKB_KEY_D: return ImGuiKey_D;
    case XKB_KEY_e: case XKB_KEY_E: return ImGuiKey_E;
    case XKB_KEY_f: case XKB_KEY_F: return ImGuiKey_F;
    case XKB_KEY_g: case XKB_KEY_G: return ImGuiKey_G;
    case XKB_KEY_h: case XKB_KEY_H: return ImGuiKey_H;
    case XKB_KEY_i: case XKB_KEY_I: return ImGuiKey_I;
    case XKB_KEY_j: case XKB_KEY_J: return ImGuiKey_J;
    case XKB_KEY_k: case XKB_KEY_K: return ImGuiKey_K;
    case XKB_KEY_l: case XKB_KEY_L: return ImGuiKey_L;
    case XKB_KEY_m: case XKB_KEY_M: return ImGuiKey_M;
    case XKB_KEY_n: case XKB_KEY_N: return ImGuiKey_N;
    case XKB_KEY_o: case XKB_KEY_O: return ImGuiKey_O;
    case XKB_KEY_p: case XKB_KEY_P: return ImGuiKey_P;
    case XKB_KEY_q: case XKB_KEY_Q: return ImGuiKey_Q;
    case XKB_KEY_r: case XKB_KEY_R: return ImGuiKey_R;
    case XKB_KEY_s: case XKB_KEY_S: return ImGuiKey_S;
    case XKB_KEY_t: case XKB_KEY_T: return ImGuiKey_T;
    case XKB_KEY_u: case XKB_KEY_U: return ImGuiKey_U;
    case XKB_KEY_v: case XKB_KEY_V: return ImGuiKey_V;
    case XKB_KEY_w: case XKB_KEY_W: return ImGuiKey_W;
    case XKB_KEY_x: case XKB_KEY_X: return ImGuiKey_X;
    case XKB_KEY_y: case XKB_KEY_Y: return ImGuiKey_Y;
    case XKB_KEY_z: case XKB_KEY_Z: return ImGuiKey_Z;

    default: return ImGuiKey_None;
    }
}

// Map Linux input button code to ImGui mouse button index
static int linux_button_to_imgui(int button)
{
    switch (button) {
    case BTN_LEFT:   return 0;
    case BTN_RIGHT:  return 1;
    case BTN_MIDDLE: return 2;
    default:         return -1;
    }
}

bool ImGui_ImplWayland::Init(Overlay* overlay)
{
    g_overlay = overlay;
    g_last_time = std::chrono::steady_clock::now();

    ImGuiIO& io = ImGui::GetIO();
    io.BackendPlatformName = "imgui_impl_wayland";
    io.DisplaySize = ImVec2(static_cast<float>(overlay->width()),
                            static_cast<float>(overlay->height()));
    float s = static_cast<float>(overlay->scale());
    io.DisplayFramebufferScale = ImVec2(s, s);
    return true;
}

void ImGui_ImplWayland::Shutdown()
{
    g_overlay = nullptr;
}

void ImGui_ImplWayland::NewFrame()
{
    ImGuiIO& io = ImGui::GetIO();

    // Update display size (logical) and framebuffer scale
    io.DisplaySize = ImVec2(static_cast<float>(g_overlay->width()),
                            static_cast<float>(g_overlay->height()));
    float s = static_cast<float>(g_overlay->scale());
    io.DisplayFramebufferScale = ImVec2(s, s);

    // Update delta time
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - g_last_time).count();
    io.DeltaTime = dt > 0.0f ? dt : 1.0f / 60.0f;
    g_last_time = now;

    // Drain events from overlay
    auto events = g_overlay->drain_events();
    for (const auto& ev : events) {
        switch (ev.type) {
        case EventType::Key: {
            ImGuiKey key = keysym_to_imgui(ev.keysym);
            if (key != ImGuiKey_None)
                io.AddKeyEvent(key, ev.pressed);

            // Update modifier keys
            if (ev.keysym == XKB_KEY_Shift_L || ev.keysym == XKB_KEY_Shift_R)
                io.AddKeyEvent(ImGuiMod_Shift, ev.pressed);
            if (ev.keysym == XKB_KEY_Control_L || ev.keysym == XKB_KEY_Control_R)
                io.AddKeyEvent(ImGuiMod_Ctrl, ev.pressed);
            if (ev.keysym == XKB_KEY_Alt_L || ev.keysym == XKB_KEY_Alt_R)
                io.AddKeyEvent(ImGuiMod_Alt, ev.pressed);
            if (ev.keysym == XKB_KEY_Super_L || ev.keysym == XKB_KEY_Super_R)
                io.AddKeyEvent(ImGuiMod_Super, ev.pressed);
            break;
        }
        case EventType::Text:
            io.AddInputCharactersUTF8(ev.text.c_str());
            break;
        case EventType::MouseMove:
            io.AddMousePosEvent(static_cast<float>(ev.mx),
                                static_cast<float>(ev.my));
            break;
        case EventType::MouseButton: {
            int btn = linux_button_to_imgui(ev.button);
            if (btn >= 0)
                io.AddMouseButtonEvent(btn, ev.pressed);
            break;
        }
        case EventType::MouseScroll:
            io.AddMouseWheelEvent(
                static_cast<float>(-ev.scroll_x / 10.0),
                static_cast<float>(-ev.scroll_y / 10.0));
            break;
        }
    }
}
