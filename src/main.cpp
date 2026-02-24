#include "audio.h"
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
    // Try a few common model paths
    std::string model_path = "models/ggml-tiny.bin";
    if (!transcriber.init(model_path)) {
        // Try relative to executable
        std::fprintf(stderr, "Failed to init transcriber with %s\n", model_path.c_str());
        return 1;
    }

    // Init ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // Don't save layout

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 0.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.1f, 0.1f, 0.1f, 0.95f);

    ImGui_ImplWayland::Init(&overlay);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    // Text buffer for the editable area
    static char text_buf[64 * 1024] = {};
    bool accepted = false;

    // Audio read buffer
    std::vector<float> audio_buf(READ_BUF_SIZE);

    // Live text update callback
    transcriber.set_callback([&](const std::string& text) {
        // Copy transcribed text into buffer (only if user hasn't manually edited)
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
                     ImGuiWindowFlags_NoCollapse);

        ImGui::Text("live-whisper  |  Enter: accept  |  Escape: cancel");
        ImGui::Separator();

        float footer_height = ImGui::GetFrameHeightWithSpacing();
        float text_height = ImGui::GetContentRegionAvail().y - footer_height;
        ImGui::InputTextMultiline("##text", text_buf, sizeof(text_buf),
                                  ImVec2(-1.0f, text_height));

        // Status line
        ImGui::TextDisabled("Audio: %u frames buffered", audio.available());

        ImGui::End();

        // Render
        glViewport(0, 0, overlay.width(), overlay.height());
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

    // Paste if accepted (overlay is gone, target window can receive input)
    if (accepted && text_buf[0] != '\0') {
        paste::refocus_and_type(focus_addr, text_buf);
    }

    return 0;
}
