#include "audio.h"
#include "font.h"
#include "imgui_impl_wayland.h"
#include "overlay.h"
#include "paste.h"
#include "transcriber.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include <GLES3/gl3.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <xkbcommon/xkbcommon-keysyms.h>

static constexpr int    OVERLAY_HEIGHT = 200;
static constexpr int    SAMPLE_RATE    = 16000;
static constexpr int    READ_BUF_SIZE  = SAMPLE_RATE / 10;  // 100ms chunks
static constexpr float  BASE_FONT_SIZE = 10.0f;

static void apply_style(float scale)
{
    auto& style = ImGui::GetStyle();

    // Scale all default sizes for HiDPI, then override specific values
    style.ScaleAllSizes(scale);

    // Geometry (set after ScaleAllSizes — these are direct overrides)
    style.WindowRounding    = 12.0f;
    style.WindowBorderSize  = 0.0f;
    style.WindowPadding     = ImVec2(16.0f, 12.0f);
    style.FrameRounding     = 6.0f;
    style.FramePadding      = ImVec2(12.0f, 8.0f);
    style.ItemSpacing       = ImVec2(8.0f, 8.0f);
    style.ScrollbarSize     = 10.0f;
    style.ScrollbarRounding = 4.0f;
    style.GrabRounding      = 4.0f;

    // Colors — dark translucent overlay
    auto& c = style.Colors;
    c[ImGuiCol_WindowBg]          = ImVec4(0.08f, 0.08f, 0.10f, 0.95f);
    c[ImGuiCol_Border]            = ImVec4(0.20f, 0.20f, 0.25f, 0.50f);

    // Text
    c[ImGuiCol_Text]              = ImVec4(0.90f, 0.90f, 0.93f, 1.00f);
    c[ImGuiCol_TextDisabled]      = ImVec4(0.45f, 0.45f, 0.50f, 1.00f);

    // Frame (text input background)
    c[ImGuiCol_FrameBg]           = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    c[ImGuiCol_FrameBgHovered]    = ImVec4(0.16f, 0.16f, 0.20f, 1.00f);
    c[ImGuiCol_FrameBgActive]     = ImVec4(0.14f, 0.14f, 0.18f, 1.00f);

    // Scrollbar
    c[ImGuiCol_ScrollbarBg]       = ImVec4(0.08f, 0.08f, 0.10f, 0.50f);
    c[ImGuiCol_ScrollbarGrab]     = ImVec4(0.25f, 0.25f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.35f, 0.40f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.40f, 0.40f, 0.45f, 1.00f);

    // Separator
    c[ImGuiCol_Separator]         = ImVec4(0.22f, 0.22f, 0.28f, 1.00f);

    // Header (for the status bar area)
    c[ImGuiCol_Header]            = ImVec4(0.15f, 0.15f, 0.20f, 1.00f);
    c[ImGuiCol_HeaderHovered]     = ImVec4(0.20f, 0.20f, 0.26f, 1.00f);
    c[ImGuiCol_HeaderActive]      = ImVec4(0.18f, 0.18f, 0.24f, 1.00f);

    // Text selection
    c[ImGuiCol_TextSelectedBg]    = ImVec4(0.22f, 0.35f, 0.55f, 0.60f);
    c[ImGuiCol_NavHighlight]      = ImVec4(0.30f, 0.50f, 0.80f, 1.00f);
}

int main()
{
    // Capture focus before overlay appears
    std::string focus_addr = paste::capture_focus();

    // Init overlay
    Overlay overlay;
    if (!overlay.init(OVERLAY_HEIGHT)) {
        std::fprintf(stderr, "Failed to init overlay\n");
        return 1;
    }

    // Init audio
    AudioCapture audio;
    if (!audio.init()) {
        std::fprintf(stderr, "Failed to init audio capture\n");
        return 1;
    }

    // Init transcriber
    Transcriber transcriber;
    std::string model_path = "models/ggml-tiny.bin";
    if (!transcriber.init(model_path)) {
        std::fprintf(stderr, "Failed to init transcriber with %s\n", model_path.c_str());
        return 1;
    }

    // Init ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    // DPI scaling from Wayland output
    float scale = static_cast<float>(overlay.scale());
    if (scale < 1.0f) scale = 1.0f;

    apply_style(scale);
    ImGui::UseCustomFont(io, BASE_FONT_SIZE * scale);

    ImGui_ImplWayland::Init(&overlay);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Text buffer for the editable area
    static char text_buf[64 * 1024] = {};
    bool accepted = false;

    // Audio read buffer
    std::vector<float> audio_buf(READ_BUF_SIZE);

    // Live text update callback
    transcriber.set_callback([&](const std::string& text) {
        std::strncpy(text_buf, text.c_str(), sizeof(text_buf) - 1);
        text_buf[sizeof(text_buf) - 1] = '\0';
    });

    // Main loop
    while (overlay.dispatch()) {
        // Read audio and feed to transcriber
        uint32_t avail = audio.available();
        while (avail > 0) {
            uint32_t to_read = avail < READ_BUF_SIZE ? avail : READ_BUF_SIZE;
            uint32_t got = audio.read(audio_buf.data(), to_read);
            if (got == 0) break;
            transcriber.process(audio_buf.data(), got);
            avail = audio.available();
        }

        // Check for Enter/Escape from raw events before ImGui consumes them
        for (const auto& ev : overlay.peek_events()) {
            if (ev.type != EventType::Key || !ev.pressed) continue;
            if (ev.keysym == XKB_KEY_Escape) {
                overlay.request_close();
            }
            if (ev.keysym == XKB_KEY_Return || ev.keysym == XKB_KEY_KP_Enter) {
                accepted = true;
                overlay.request_close();
            }
        }

        // Begin ImGui frame
        overlay.make_current();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWayland::NewFrame();
        ImGui::NewFrame();

        // Full-window overlay UI
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##overlay", nullptr,
                     ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        // Header bar
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.55f, 0.60f, 1.0f));
        ImGui::Text("LIVE-WHISPER");
        ImGui::PopStyleColor();
        ImGui::SameLine(io.DisplaySize.x - ImGui::CalcTextSize("Enter: accept  |  Esc: cancel").x
                        - ImGui::GetStyle().WindowPadding.x);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.40f, 0.40f, 0.45f, 1.0f));
        ImGui::Text("Enter: accept  |  Esc: cancel");
        ImGui::PopStyleColor();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Text area fills remaining space minus status line
        float status_height = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
        float text_height = ImGui::GetContentRegionAvail().y - status_height;
        ImGui::InputTextMultiline("##text", text_buf, sizeof(text_buf),
                                  ImVec2(-1.0f, text_height),
                                  ImGuiInputTextFlags_AllowTabInput);

        // Status line
        ImGui::TextDisabled("Audio: %u frames buffered", audio.available());

        ImGui::End();

        // Render at physical framebuffer resolution
        glViewport(0, 0, overlay.fb_width(), overlay.fb_height());
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        overlay.swap_buffers();
    }

    // Tear down overlay first so keyboard grab is released
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplWayland::Shutdown();
    ImGui::DestroyContext();
    overlay.shutdown();

    audio.shutdown();
    transcriber.shutdown();

    // Type text if accepted (overlay is gone, target window can receive input)
    if (accepted && text_buf[0] != '\0') {
        paste::refocus_and_type(focus_addr, text_buf);
    }

    return 0;
}
