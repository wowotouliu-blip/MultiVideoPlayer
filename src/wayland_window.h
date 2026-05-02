#ifndef WAYLAND_WINDOW_H
#define WAYLAND_WINDOW_H

#include <libdecor-0/libdecor.h>
#include <wayland-client.h>
#include <wayland-egl.h>
#include <string>
#include <functional>
#include <atomic>

struct wl_shm;

class WaylandWindow {
public:
    using ResizeCallback = std::function<void(int width, int height)>;

    WaylandWindow(const std::string& title, int width, int height);
    ~WaylandWindow();

    void setTitle(const std::string& title);
    void resize(int width, int height);
    void close();

    bool shouldClose() const { return m_shouldClose; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    wl_surface* surface() const { return m_surface; }
    wl_egl_window* eglWindow() const { return m_eglWindow; }
    wl_display* display() const { return m_display; }

    void setResizeCallback(ResizeCallback callback) { m_resizeCallback = std::move(callback); }

    void dispatchEvents();
    void flush();
    void scheduleFrame();                          // Request frame callback BEFORE eglSwapBuffers
    void awaitFrame();                             // Block until scheduled frame callback fires
    void waitForFrame();                           // DEPRECATED: use scheduleFrame + awaitFrame instead
    bool isConfigured() const { return m_configured; }
    void waitForConfigure();

    // Owned Wayland resources (accessed by C callbacks)
    wl_display* m_display = nullptr;
    wl_registry* m_registry = nullptr;
    wl_compositor* m_compositor = nullptr;
    wl_shm* m_shm = nullptr;
    struct libdecor* m_libdecor_context = nullptr;

    // Static callbacks (public for init_libdecor_callbacks)
    static libdecor_interface s_libdecorInterface;
    static libdecor_frame_interface s_libdecorFrameInterface;

    static void handleLibdecorError(libdecor*, libdecor_error, const char*);
    static void handleLibdecorConfigure(libdecor_frame*, libdecor_configuration*, void*);
    static void handleLibdecorClose(libdecor_frame*, void*);
    static void handleLibdecorCommit(libdecor_frame*, void*);

private:
    void initWayland();
    void initLibdecor();

    wl_surface* m_surface = nullptr;
    libdecor_frame* m_libdecorFrame = nullptr;
    wl_egl_window* m_eglWindow = nullptr;

    std::string m_title;
    int m_width;
    int m_height;
    bool m_shouldClose = false;
    bool m_configured = false;
    bool m_frameReady = false;
    struct wl_callback* m_frameCallback = nullptr;
    ResizeCallback m_resizeCallback;

    static void onFrameDone(void* data, struct wl_callback* cb, uint32_t time);
    static const wl_callback_listener s_frameListener;

    static const wl_registry_listener s_registryListener;
};

#endif // WAYLAND_WINDOW_H
