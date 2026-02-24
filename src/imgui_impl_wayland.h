#pragma once

struct Overlay;

namespace ImGui_ImplWayland {
    bool Init(Overlay* overlay);
    void Shutdown();
    void NewFrame();
}
