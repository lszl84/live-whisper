#include "overlay.h"

#include <wayland-client.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct Overlay::Impl {
    // Wayland globals
    wl_display*             display     = nullptr;
    wl_registry*            registry    = nullptr;
    wl_compositor*          compositor  = nullptr;
    wl_seat*                seat        = nullptr;
    wl_surface*             surface     = nullptr;
    wl_output*              output      = nullptr;
    zwlr_layer_shell_v1*    layer_shell = nullptr;
    zwlr_layer_surface_v1*  layer_surface = nullptr;

    // Input
    wl_keyboard*            keyboard    = nullptr;
    wl_pointer*             pointer     = nullptr;

    // XKB
    xkb_context*            xkb_ctx    = nullptr;
    xkb_keymap*             xkb_keymap_obj = nullptr;
    xkb_state*              xkb_state_obj  = nullptr;

    // EGL
    EGLDisplay              egl_display = EGL_NO_DISPLAY;
    EGLContext              egl_context = EGL_NO_CONTEXT;
    EGLSurface              egl_surface = EGL_NO_SURFACE;
    wl_egl_window*          egl_window  = nullptr;

    // State
    int          configured_width  = 0;
    int          configured_height = 0;
    int          requested_height  = 200;
    int          scale_factor      = 1;
    int          output_width      = 0;   // physical output resolution
    int          output_height     = 0;
    bool         closed            = false;
    bool         configured        = false;

    std::vector<WaylandEvent> events;

    // -- Wayland listener callbacks (static, forward to Impl*) --

    // Registry
    static void registry_global(void* data, wl_registry* reg,
                                uint32_t name, const char* iface, uint32_t version);
    static void registry_global_remove(void*, wl_registry*, uint32_t) {}

    // Layer surface
    static void layer_surface_configure(void* data, zwlr_layer_surface_v1* ls,
                                        uint32_t serial, uint32_t w, uint32_t h);
    static void layer_surface_closed(void* data, zwlr_layer_surface_v1*);

    // Seat
    static void seat_capabilities(void* data, wl_seat* s, uint32_t caps);
    static void seat_name(void*, wl_seat*, const char*) {}

    // Keyboard
    static void kb_keymap(void* data, wl_keyboard*, uint32_t format,
                          int32_t fd, uint32_t size);
    static void kb_enter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
    static void kb_leave(void*, wl_keyboard*, uint32_t, wl_surface*) {}
    static void kb_key(void* data, wl_keyboard*, uint32_t serial,
                       uint32_t time, uint32_t key, uint32_t state);
    static void kb_modifiers(void* data, wl_keyboard*, uint32_t serial,
                             uint32_t mods_depressed, uint32_t mods_latched,
                             uint32_t mods_locked, uint32_t group);
    static void kb_repeat(void*, wl_keyboard*, int32_t, int32_t) {}

    // Pointer
    static void ptr_enter(void* data, wl_pointer*, uint32_t serial,
                          wl_surface*, wl_fixed_t x, wl_fixed_t y);
    static void ptr_leave(void*, wl_pointer*, uint32_t, wl_surface*) {}
    static void ptr_motion(void* data, wl_pointer*, uint32_t,
                           wl_fixed_t x, wl_fixed_t y);
    static void ptr_button(void* data, wl_pointer*, uint32_t serial,
                           uint32_t time, uint32_t button, uint32_t state);
    static void ptr_axis(void* data, wl_pointer*, uint32_t time,
                         uint32_t axis, wl_fixed_t value);
    static void ptr_frame(void*, wl_pointer*) {}
    static void ptr_axis_source(void*, wl_pointer*, uint32_t) {}
    static void ptr_axis_stop(void*, wl_pointer*, uint32_t, uint32_t) {}
    static void ptr_axis_discrete(void*, wl_pointer*, uint32_t, int32_t) {}

    // Output
    static void output_geometry(void*, wl_output*, int32_t, int32_t, int32_t,
                                int32_t, int32_t, const char*, const char*, int32_t) {}
    static void output_mode(void* data, wl_output*, uint32_t flags,
                            int32_t width, int32_t height, int32_t);
    static void output_scale(void* data, wl_output*, int32_t factor);
    static void output_done(void*, wl_output*) {}

    bool init_egl();
};

// ---------------------------------------------------------------------------
// Listener tables
// ---------------------------------------------------------------------------
static const wl_registry_listener registry_listener = {
    Overlay::Impl::registry_global,
    Overlay::Impl::registry_global_remove,
};

static const zwlr_layer_surface_v1_listener layer_surface_listener = {
    Overlay::Impl::layer_surface_configure,
    Overlay::Impl::layer_surface_closed,
};

static const wl_seat_listener seat_listener = {
    Overlay::Impl::seat_capabilities,
    Overlay::Impl::seat_name,
};

static const wl_keyboard_listener keyboard_listener = {
    Overlay::Impl::kb_keymap,
    Overlay::Impl::kb_enter,
    Overlay::Impl::kb_leave,
    Overlay::Impl::kb_key,
    Overlay::Impl::kb_modifiers,
    Overlay::Impl::kb_repeat,
};

static const wl_pointer_listener pointer_listener = {
    Overlay::Impl::ptr_enter,
    Overlay::Impl::ptr_leave,
    Overlay::Impl::ptr_motion,
    Overlay::Impl::ptr_button,
    Overlay::Impl::ptr_axis,
    Overlay::Impl::ptr_frame,
    Overlay::Impl::ptr_axis_source,
    Overlay::Impl::ptr_axis_stop,
    Overlay::Impl::ptr_axis_discrete,
};

static const wl_output_listener output_listener = {
    Overlay::Impl::output_geometry,
    Overlay::Impl::output_mode,
    Overlay::Impl::output_done,
    Overlay::Impl::output_scale,
};

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
void Overlay::Impl::registry_global(void* data, wl_registry* reg,
                                     uint32_t name, const char* iface, uint32_t version)
{
    auto* self = static_cast<Impl*>(data);
    if (std::strcmp(iface, wl_compositor_interface.name) == 0) {
        self->compositor = static_cast<wl_compositor*>(
            wl_registry_bind(reg, name, &wl_compositor_interface, 4));
    } else if (std::strcmp(iface, wl_seat_interface.name) == 0) {
        self->seat = static_cast<wl_seat*>(
            wl_registry_bind(reg, name, &wl_seat_interface, 5));
        wl_seat_add_listener(self->seat, &seat_listener, self);
    } else if (std::strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
        self->layer_shell = static_cast<zwlr_layer_shell_v1*>(
            wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface,
                             version < 4 ? version : 4));
    } else if (std::strcmp(iface, wl_output_interface.name) == 0 && !self->output) {
        self->output = static_cast<wl_output*>(
            wl_registry_bind(reg, name, &wl_output_interface,
                             version < 3 ? version : 3));
        wl_output_add_listener(self->output, &output_listener, self);
    }
}

// ---------------------------------------------------------------------------
// Layer surface
// ---------------------------------------------------------------------------
void Overlay::Impl::layer_surface_configure(void* data, zwlr_layer_surface_v1* ls,
                                             uint32_t serial, uint32_t w, uint32_t h)
{
    auto* self = static_cast<Impl*>(data);
    zwlr_layer_surface_v1_ack_configure(ls, serial);

    self->configured_width  = static_cast<int>(w);
    self->configured_height = static_cast<int>(h);

    if (!self->configured) {
        self->configured = true;
        self->init_egl();
    } else if (self->egl_window) {
        wl_egl_window_resize(self->egl_window,
                             self->configured_width  * self->scale_factor,
                             self->configured_height * self->scale_factor, 0, 0);
    }
}

void Overlay::Impl::layer_surface_closed(void* data, zwlr_layer_surface_v1*)
{
    static_cast<Impl*>(data)->closed = true;
}

// ---------------------------------------------------------------------------
// Seat
// ---------------------------------------------------------------------------
void Overlay::Impl::seat_capabilities(void* data, wl_seat* s, uint32_t caps)
{
    auto* self = static_cast<Impl*>(data);

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !self->keyboard) {
        self->keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(self->keyboard, &keyboard_listener, self);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !self->pointer) {
        self->pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(self->pointer, &pointer_listener, self);
    }
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------
void Overlay::Impl::kb_keymap(void* data, wl_keyboard*, uint32_t format,
                               int32_t fd, uint32_t size)
{
    auto* self = static_cast<Impl*>(data);
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    char* map_str = static_cast<char*>(mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    if (map_str == MAP_FAILED) {
        close(fd);
        return;
    }

    if (self->xkb_keymap_obj) xkb_keymap_unref(self->xkb_keymap_obj);
    if (self->xkb_state_obj)  xkb_state_unref(self->xkb_state_obj);

    self->xkb_keymap_obj = xkb_keymap_new_from_string(
        self->xkb_ctx, map_str, XKB_KEYMAP_FORMAT_TEXT_V1,
        XKB_KEYMAP_COMPILE_NO_FLAGS);
    self->xkb_state_obj = xkb_state_new(self->xkb_keymap_obj);

    munmap(map_str, size);
    close(fd);
}

void Overlay::Impl::kb_key(void* data, wl_keyboard*, uint32_t /*serial*/,
                            uint32_t /*time*/, uint32_t key, uint32_t state)
{
    auto* self = static_cast<Impl*>(data);
    if (!self->xkb_state_obj) return;

    uint32_t keycode = key + 8;  // evdev to xkb offset
    xkb_keysym_t sym = xkb_state_key_get_one_sym(self->xkb_state_obj, keycode);
    bool pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);

    WaylandEvent ev{};
    ev.type    = EventType::Key;
    ev.keysym  = sym;
    ev.pressed = pressed;
    self->events.push_back(ev);

    // Also generate text event for printable chars on press
    if (pressed) {
        char buf[8]{};
        int len = xkb_state_key_get_utf8(self->xkb_state_obj, keycode, buf, sizeof(buf));
        if (len > 0 && static_cast<unsigned char>(buf[0]) >= 32) {
            WaylandEvent tev{};
            tev.type = EventType::Text;
            tev.text = std::string(buf, len);
            self->events.push_back(tev);
        }
    }
}

void Overlay::Impl::kb_modifiers(void* data, wl_keyboard*, uint32_t /*serial*/,
                                  uint32_t mods_depressed, uint32_t mods_latched,
                                  uint32_t mods_locked, uint32_t group)
{
    auto* self = static_cast<Impl*>(data);
    if (self->xkb_state_obj)
        xkb_state_update_mask(self->xkb_state_obj,
                              mods_depressed, mods_latched, mods_locked, 0, 0, group);
}

// ---------------------------------------------------------------------------
// Pointer
// ---------------------------------------------------------------------------
void Overlay::Impl::ptr_enter(void* data, wl_pointer*, uint32_t,
                               wl_surface*, wl_fixed_t x, wl_fixed_t y)
{
    auto* self = static_cast<Impl*>(data);
    WaylandEvent ev{};
    ev.type = EventType::MouseMove;
    ev.mx   = wl_fixed_to_double(x);
    ev.my   = wl_fixed_to_double(y);
    self->events.push_back(ev);
}

void Overlay::Impl::ptr_motion(void* data, wl_pointer*, uint32_t,
                                wl_fixed_t x, wl_fixed_t y)
{
    auto* self = static_cast<Impl*>(data);
    WaylandEvent ev{};
    ev.type = EventType::MouseMove;
    ev.mx   = wl_fixed_to_double(x);
    ev.my   = wl_fixed_to_double(y);
    self->events.push_back(ev);
}

void Overlay::Impl::ptr_button(void* data, wl_pointer*, uint32_t,
                                uint32_t, uint32_t button, uint32_t state)
{
    auto* self = static_cast<Impl*>(data);
    WaylandEvent ev{};
    ev.type    = EventType::MouseButton;
    ev.button  = static_cast<int>(button);
    ev.pressed = (state == WL_POINTER_BUTTON_STATE_PRESSED);
    self->events.push_back(ev);
}

void Overlay::Impl::ptr_axis(void* data, wl_pointer*, uint32_t,
                              uint32_t axis, wl_fixed_t value)
{
    auto* self = static_cast<Impl*>(data);
    WaylandEvent ev{};
    ev.type = EventType::MouseScroll;
    double v = wl_fixed_to_double(value);
    if (axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL)
        ev.scroll_x = v;
    else
        ev.scroll_y = v;
    self->events.push_back(ev);
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------
void Overlay::Impl::output_mode(void* data, wl_output*, uint32_t flags,
                                 int32_t width, int32_t height, int32_t)
{
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        auto* self = static_cast<Impl*>(data);
        self->output_width  = width;
        self->output_height = height;
    }
}

void Overlay::Impl::output_scale(void* data, wl_output*, int32_t factor)
{
    static_cast<Impl*>(data)->scale_factor = factor;
}

// ---------------------------------------------------------------------------
// EGL init
// ---------------------------------------------------------------------------
bool Overlay::Impl::init_egl()
{
    egl_display = eglGetDisplay(static_cast<EGLNativeDisplayType>(display));
    if (egl_display == EGL_NO_DISPLAY) {
        std::fprintf(stderr, "overlay: eglGetDisplay failed\n");
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(egl_display, &major, &minor)) {
        std::fprintf(stderr, "overlay: eglInitialize failed\n");
        return false;
    }

    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };
    EGLConfig config;
    EGLint num_configs;
    eglChooseConfig(egl_display, config_attribs, &config, 1, &num_configs);

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, ctx_attribs);

    // Create EGL window at physical pixel size for crisp rendering
    int phys_w = configured_width  * scale_factor;
    int phys_h = configured_height * scale_factor;
    egl_window = wl_egl_window_create(surface, phys_w, phys_h);
    egl_surface = eglCreateWindowSurface(egl_display, config,
                                         static_cast<EGLNativeWindowType>(egl_window), nullptr);

    // Tell compositor our buffer is at higher resolution
    wl_surface_set_buffer_scale(surface, scale_factor);

    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
    return true;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
Overlay::Overlay() : impl_(std::make_unique<Impl>()) {}

Overlay::~Overlay() { shutdown(); }

bool Overlay::init(int height)
{
    impl_->requested_height = height;

    // XKB context
    impl_->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!impl_->xkb_ctx) {
        std::fprintf(stderr, "overlay: xkb_context_new failed\n");
        return false;
    }

    // Connect to Wayland
    impl_->display = wl_display_connect(nullptr);
    if (!impl_->display) {
        std::fprintf(stderr, "overlay: wl_display_connect failed\n");
        return false;
    }

    impl_->registry = wl_display_get_registry(impl_->display);
    wl_registry_add_listener(impl_->registry, &registry_listener, impl_.get());
    wl_display_roundtrip(impl_->display);  // binds globals, adds output listener
    wl_display_roundtrip(impl_->display);  // receives output mode/scale events

    if (!impl_->compositor || !impl_->layer_shell) {
        std::fprintf(stderr, "overlay: missing required globals (compositor=%p, layer_shell=%p)\n",
                     static_cast<void*>(impl_->compositor),
                     static_cast<void*>(impl_->layer_shell));
        return false;
    }

    // Create surface
    impl_->surface = wl_compositor_create_surface(impl_->compositor);

    // Compute overlay size: half the logical output width, centered at bottom
    int logical_output_w = impl_->output_width / impl_->scale_factor;
    int overlay_w = logical_output_w / 2;
    if (overlay_w < 600) overlay_w = 600;  // minimum usable width
    int margin_bottom = 32;

    // Create layer surface
    impl_->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        impl_->layer_shell, impl_->surface, nullptr,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "live-whisper");

    // Anchor bottom only → compositor centers horizontally
    zwlr_layer_surface_v1_set_anchor(impl_->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
    zwlr_layer_surface_v1_set_size(impl_->layer_surface, overlay_w, height);
    zwlr_layer_surface_v1_set_margin(impl_->layer_surface,
        0, 0, margin_bottom, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(impl_->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);

    zwlr_layer_surface_v1_add_listener(impl_->layer_surface,
                                        &layer_surface_listener, impl_.get());

    wl_surface_commit(impl_->surface);
    wl_display_roundtrip(impl_->display);

    return true;
}

void Overlay::shutdown()
{
    if (impl_->egl_surface != EGL_NO_SURFACE) {
        eglMakeCurrent(impl_->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(impl_->egl_display, impl_->egl_surface);
        impl_->egl_surface = EGL_NO_SURFACE;
    }
    if (impl_->egl_window) {
        wl_egl_window_destroy(impl_->egl_window);
        impl_->egl_window = nullptr;
    }
    if (impl_->egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(impl_->egl_display, impl_->egl_context);
        impl_->egl_context = EGL_NO_CONTEXT;
    }
    if (impl_->egl_display != EGL_NO_DISPLAY) {
        eglTerminate(impl_->egl_display);
        impl_->egl_display = EGL_NO_DISPLAY;
    }

    if (impl_->xkb_state_obj)  { xkb_state_unref(impl_->xkb_state_obj);   impl_->xkb_state_obj = nullptr; }
    if (impl_->xkb_keymap_obj) { xkb_keymap_unref(impl_->xkb_keymap_obj); impl_->xkb_keymap_obj = nullptr; }
    if (impl_->xkb_ctx)        { xkb_context_unref(impl_->xkb_ctx);       impl_->xkb_ctx = nullptr; }

    if (impl_->keyboard) { wl_keyboard_destroy(impl_->keyboard); impl_->keyboard = nullptr; }
    if (impl_->pointer)  { wl_pointer_destroy(impl_->pointer);   impl_->pointer = nullptr; }

    if (impl_->layer_surface) { zwlr_layer_surface_v1_destroy(impl_->layer_surface); impl_->layer_surface = nullptr; }
    if (impl_->surface)       { wl_surface_destroy(impl_->surface);                  impl_->surface = nullptr; }
    if (impl_->output)        { wl_output_destroy(impl_->output);                    impl_->output = nullptr; }
    if (impl_->seat)          { wl_seat_destroy(impl_->seat);                        impl_->seat = nullptr; }
    if (impl_->layer_shell)   { zwlr_layer_shell_v1_destroy(impl_->layer_shell);     impl_->layer_shell = nullptr; }
    if (impl_->compositor)    { wl_compositor_destroy(impl_->compositor);             impl_->compositor = nullptr; }
    if (impl_->registry)      { wl_registry_destroy(impl_->registry);                impl_->registry = nullptr; }

    if (impl_->display) {
        wl_display_disconnect(impl_->display);
        impl_->display = nullptr;
    }
}

bool Overlay::dispatch()
{
    if (impl_->closed) return false;

    // Poll with 16ms timeout for ~60fps
    struct pollfd pfd{};
    pfd.fd     = wl_display_get_fd(impl_->display);
    pfd.events = POLLIN;

    wl_display_flush(impl_->display);
    poll(&pfd, 1, 16);

    if (pfd.revents & POLLIN)
        wl_display_dispatch(impl_->display);
    else
        wl_display_dispatch_pending(impl_->display);

    return !impl_->closed;
}

void Overlay::make_current()
{
    eglMakeCurrent(impl_->egl_display, impl_->egl_surface,
                   impl_->egl_surface, impl_->egl_context);
}

void Overlay::swap_buffers()
{
    eglSwapBuffers(impl_->egl_display, impl_->egl_surface);
}

std::vector<WaylandEvent> Overlay::drain_events()
{
    std::vector<WaylandEvent> out;
    std::swap(out, impl_->events);
    return out;
}

const std::vector<WaylandEvent>& Overlay::peek_events() const { return impl_->events; }

int Overlay::width() const     { return impl_->configured_width; }
int Overlay::height() const    { return impl_->configured_height; }
int Overlay::fb_width() const  { return impl_->configured_width  * impl_->scale_factor; }
int Overlay::fb_height() const { return impl_->configured_height * impl_->scale_factor; }
int Overlay::scale() const     { return impl_->scale_factor; }

bool Overlay::should_close() const { return impl_->closed; }
void Overlay::request_close()      { impl_->closed = true; }
