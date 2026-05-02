#include "wayland_window.h"
#include <iostream>
#include <cstring>
#include <unistd.h>

// --- Registry listener (per-instance via user_data) ---

static void registry_global(void* data, wl_registry* registry, uint32_t name,
                            const char* interface, uint32_t) {
    auto* self = static_cast<WaylandWindow*>(data);
    if (strcmp(interface, "wl_compositor") == 0) {
        self->m_compositor = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
    } else if (strcmp(interface, "wl_shm") == 0) {
        self->m_shm = static_cast<wl_shm*>(
            wl_registry_bind(registry, name, &wl_shm_interface, 1));
    }
}

static void registry_global_remove(void*, wl_registry*, uint32_t) {}

const wl_registry_listener WaylandWindow::s_registryListener = {
    .global = registry_global,
    .global_remove = registry_global_remove
};

// --- Frame callback (vsync) ---

void WaylandWindow::onFrameDone(void* data, struct wl_callback*, uint32_t) {
    auto* self = static_cast<WaylandWindow*>(data);
    self->m_frameReady = true;
    self->m_frameCallback = nullptr;
}

const wl_callback_listener WaylandWindow::s_frameListener = {
    .done = WaylandWindow::onFrameDone
};

// --- libdecor callbacks ---

void WaylandWindow::handleLibdecorError(libdecor*, libdecor_error error,
                                         const char* message) {
    std::cerr << "libdecor error (" << (int)error << "): " << message << std::endl;
}

void WaylandWindow::handleLibdecorConfigure(libdecor_frame* frame,
                                             libdecor_configuration* configuration,
                                             void* user_data) {
    auto* self = static_cast<WaylandWindow*>(user_data);

    int w = 0, h = 0;
    if (!libdecor_configuration_get_content_size(configuration, frame, &w, &h)) {
        w = self->m_width;
        h = self->m_height;
    }
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    self->resize(w, h);
    if (self->m_resizeCallback) {
        self->m_resizeCallback(w, h);
    }
    self->m_configured = true;

    libdecor_state* state = libdecor_state_new(w, h);
    libdecor_frame_commit(frame, state, configuration);
    libdecor_state_free(state);
}

void WaylandWindow::handleLibdecorClose(libdecor_frame*, void* user_data) {
    auto* self = static_cast<WaylandWindow*>(user_data);
    std::cout << "Window close requested: " << self->m_title << std::endl;
    self->m_shouldClose = true;
}

void WaylandWindow::handleLibdecorCommit(libdecor_frame*, void*) {}

// Static interface definitions (zero-initialized, then callbacks set per-process once)

static void init_libdecor_callbacks() {
    static bool done = false;
    if (done) return;
    done = true;

    memset(&WaylandWindow::s_libdecorInterface, 0, sizeof(libdecor_interface));
    WaylandWindow::s_libdecorInterface.error = WaylandWindow::handleLibdecorError;

    memset(&WaylandWindow::s_libdecorFrameInterface, 0, sizeof(libdecor_frame_interface));
    WaylandWindow::s_libdecorFrameInterface.configure = WaylandWindow::handleLibdecorConfigure;
    WaylandWindow::s_libdecorFrameInterface.close = WaylandWindow::handleLibdecorClose;
    WaylandWindow::s_libdecorFrameInterface.commit = WaylandWindow::handleLibdecorCommit;
}

// Static member definitions
libdecor_interface WaylandWindow::s_libdecorInterface;
libdecor_frame_interface WaylandWindow::s_libdecorFrameInterface;

// --- Constructor / Destructor ---

WaylandWindow::WaylandWindow(const std::string& title, int width, int height)
    : m_title(title), m_width(width), m_height(height)
{
    initWayland();
    initLibdecor();
}

WaylandWindow::~WaylandWindow() {
    close();
}

void WaylandWindow::initWayland() {
    m_display = wl_display_connect(nullptr);
    if (!m_display) {
        std::cerr << "Failed to connect to Wayland display" << std::endl;
        return;
    }

    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &s_registryListener, this);
    wl_display_roundtrip(m_display);
    wl_display_roundtrip(m_display);

    if (!m_compositor) {
        std::cerr << "Missing wl_compositor global" << std::endl;
        return;
    }

    m_surface = wl_compositor_create_surface(m_compositor);
    m_eglWindow = wl_egl_window_create(m_surface, m_width, m_height);
}

void WaylandWindow::initLibdecor() {
    init_libdecor_callbacks();
    m_libdecor_context = libdecor_new(m_display, &s_libdecorInterface);
    if (!m_libdecor_context) {
        std::cerr << "Failed to create libdecor context" << std::endl;
        return;
    }

    m_libdecorFrame = libdecor_decorate(m_libdecor_context, m_surface,
                                         &s_libdecorFrameInterface, this);
    libdecor_frame_set_title(m_libdecorFrame, m_title.c_str());
    libdecor_frame_set_app_id(m_libdecorFrame, "multi_video_player");
    libdecor_frame_set_capabilities(m_libdecorFrame,
        static_cast<libdecor_capabilities>(
            LIBDECOR_ACTION_MOVE | LIBDECOR_ACTION_RESIZE |
            LIBDECOR_ACTION_MINIMIZE | LIBDECOR_ACTION_FULLSCREEN |
            LIBDECOR_ACTION_CLOSE));

    libdecor_frame_map(m_libdecorFrame);
    wl_display_flush(m_display);
}

void WaylandWindow::setTitle(const std::string& title) {
    m_title = title;
    if (m_libdecorFrame) {
        libdecor_frame_set_title(m_libdecorFrame, m_title.c_str());
    }
}

void WaylandWindow::resize(int width, int height) {
    m_width = width;
    m_height = height;
    if (m_eglWindow) {
        wl_egl_window_resize(m_eglWindow, m_width, m_height, 0, 0);
    }
}

void WaylandWindow::close() {
    m_shouldClose = true;

    if (m_eglWindow) {
        wl_egl_window_destroy(m_eglWindow);
        m_eglWindow = nullptr;
    }
    if (m_libdecorFrame) {
        libdecor_frame_unref(m_libdecorFrame);
        m_libdecorFrame = nullptr;
    }
    if (m_surface && m_compositor) {
        wl_surface_destroy(m_surface);
        m_surface = nullptr;
    }
    if (m_libdecor_context) {
        libdecor_unref(m_libdecor_context);
        m_libdecor_context = nullptr;
    }
    if (m_shm) {
        wl_shm_destroy(m_shm);
        m_shm = nullptr;
    }
    if (m_compositor) {
        wl_compositor_destroy(m_compositor);
        m_compositor = nullptr;
    }
    if (m_registry) {
        wl_registry_destroy(m_registry);
        m_registry = nullptr;
    }
    if (m_display) {
        wl_display_disconnect(m_display);
        m_display = nullptr;
    }
}

void WaylandWindow::dispatchEvents() {
    if (m_libdecor_context) {
        libdecor_dispatch(m_libdecor_context, 0);
    }
}

void WaylandWindow::flush() {
    if (m_display) {
        wl_display_flush(m_display);
    }
}

void WaylandWindow::scheduleFrame() {
    if (m_frameCallback) {
        wl_callback_destroy(m_frameCallback);
    }
    m_frameReady = false;
    m_frameCallback = wl_surface_frame(m_surface);
    wl_callback_add_listener(m_frameCallback, &s_frameListener, this);
}

void WaylandWindow::awaitFrame() {
    if (!m_display) return;
    wl_display_flush(m_display);
    while (!m_frameReady && !m_shouldClose && m_display) {
        if (wl_display_dispatch(m_display) < 0) {
            break;
        }
    }
}

void WaylandWindow::waitForFrame() {
    // Legacy: use scheduleFrame+awaitFrame for correct ordering
    if (m_frameCallback) {
        wl_callback_destroy(m_frameCallback);
    }
    m_frameReady = false;
    m_frameCallback = wl_surface_frame(m_surface);
    wl_callback_add_listener(m_frameCallback, &s_frameListener, this);
    wl_display_flush(m_display);

    while (!m_frameReady && !m_shouldClose && m_display) {
        if (wl_display_dispatch(m_display) < 0) {
            break;
        }
    }
}

void WaylandWindow::waitForConfigure() {
    while (!m_configured && !m_shouldClose) {
        if (libdecor_dispatch(m_libdecor_context, -1) < 0) {
            break;
        }
    }
}
