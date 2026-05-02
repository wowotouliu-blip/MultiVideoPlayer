#ifndef EGL_RENDERER_H
#define EGL_RENDERER_H

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <string>
#include <vector>
#include "vaapi_decoder.h"

struct wl_display;
struct wl_egl_window;

class EGLRenderer {
public:
    EGLRenderer();
    ~EGLRenderer();

    bool init(wl_display* display, wl_egl_window* nativeWindow);
    void cleanup();

    void setViewport(int width, int height);
    void setPlayerCount(int count);                    // Allocate per-player textures
    void beginFrame();                                 // Clear + make context current
    void renderFrame(const VideoFrame& frame);         // CPU fallback (full window)
    void renderCpuFrame(const VideoFrame& frame);      // CPU upload + render only (no clear)
    void renderDmaBufFrame(DmaBufFrame& frame);         // DMABUF (full window)
    void renderDmaBufFramePrev(int playerIndex = 0);    // Re-render last DMA-BUF without update
    void renderCpuFramePrev();                           // Re-render last CPU texture without update
    void renderDmaBufToRect(DmaBufFrame& frame,         // DMABUF to specific region
                            int playerIndex,
                            int vp_x, int vp_y, int vp_w, int vp_h);
    void renderDmaBufToRectPrev(int playerIndex,         // Re-render last DMA-BUF at sub-rect
                                int vp_x, int vp_y, int vp_w, int vp_h);
    void renderCpuFrameToRect(const VideoFrame& frame,   // CPU upload + render to sub-rect
                              int playerIndex,
                              int vp_x, int vp_y, int vp_w, int vp_h);
    void renderCpuFrameToRectPrev(int playerIndex,        // Re-render last CPU frame at sub-rect
                                   int vp_x, int vp_y, int vp_w, int vp_h);

    void releaseContext();
    void swapBuffers();
    bool isInitialized() const { return m_initialized; }

private:
    bool initEGL(wl_display* display);
    bool initShaders();
    bool initDmaBufExtensions();
    void createTextures();
    void destroyTextures();

    void updateTextureFromFrame(const VideoFrame& frame);
    void renderQuadCpu();

    bool createEGLImages(DmaBufFrame& frame, int playerIndex);
    void destroyEGLImages();
    void bindEGLImages();
    void renderQuadDmaBuf(int playerIndex);

    EGLDisplay m_eglDisplay = EGL_NO_DISPLAY;
    EGLConfig m_eglConfig = nullptr;
    EGLContext m_eglContext = EGL_NO_CONTEXT;
    EGLSurface m_eglSurface = EGL_NO_SURFACE;

    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC m_glEGLImageTargetTexture2DOES = nullptr;

    // NV12 shader (used for both per-player and full-window renders)
    GLuint m_programNv12 = 0;
    GLint m_nv12PositionLoc = -1;
    GLint m_nv12TexCoordLoc = -1;
    GLint m_nv12TextureYLoc = -1;
    GLint m_nv12TextureUVLoc = -1;

    // CPU fallback shader
    GLuint m_programYuv420p = 0;
    GLint m_yuvPositionLoc = -1;
    GLint m_yuvTexCoordLoc = -1;
    GLint m_yuvTextureYLoc = -1;
    GLint m_yuvTextureULoc = -1;
    GLint m_yuvTextureVLoc = -1;

    // Per-player DMABUF textures (playerIndex → texture pair)
    std::vector<GLuint> m_playerTexY;
    std::vector<GLuint> m_playerTexUV;

    // Per-player CPU fallback textures
    std::vector<GLuint> m_cpuTexY;
    std::vector<GLuint> m_cpuTexU;
    std::vector<GLuint> m_cpuTexV;

    // CPU fallback textures (shared, simple use case)
    GLuint m_textureY = 0;
    GLuint m_textureU = 0;
    GLuint m_textureV = 0;

    GLuint m_vertexBuffer = 0;
    bool m_dmaBufSupported = false;
    bool m_hasDmaBufModifiers = false;

    int m_viewportWidth = 0;
    int m_viewportHeight = 0;
    bool m_initialized = false;
    bool m_texturesCreated = false;
};

#endif // EGL_RENDERER_H
