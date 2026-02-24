#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <xkbcommon/xkbcommon.h>

enum class EventType {
    Key,
    MouseMove,
    MouseButton,
    MouseScroll,
    Text,
};

struct WaylandEvent {
    EventType type;

    // Key
    xkb_keysym_t keysym = 0;
    bool         pressed = false;

    // Mouse
    double mx = 0, my = 0;
    int    button = 0;
    double scroll_x = 0, scroll_y = 0;

    // Text input (UTF-8)
    std::string text;
};

struct Overlay {
    Overlay();
    ~Overlay();

    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    bool init(int height = 200);
    void shutdown();

    // Dispatch Wayland events. Returns false if the surface was closed.
    bool dispatch();

    // Make the EGL context current and swap buffers.
    void make_current();
    void swap_buffers();

    // Drain pending input events.
    std::vector<WaylandEvent> drain_events();

    // Peek at pending events without draining.
    const std::vector<WaylandEvent>& peek_events() const;

    // Display dimensions (updated on configure).
    int width()  const;
    int height() const;

    bool should_close() const;
    void request_close();

    // Public for C callback access from the .cpp listener tables.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};
