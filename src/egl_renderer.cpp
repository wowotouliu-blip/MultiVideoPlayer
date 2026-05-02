#include "egl_renderer.h"
#include <wayland-client.h>
#include <wayland-egl.h>
#include <iostream>
#include <cstring>
#include <vector>
#include <cmath>
#include <drm_fourcc.h>

// --- YUV420P shader for CPU fallback path ---

static const char* g_vertexShaderSource = R"(
attribute vec2 aPosition;
attribute vec2 aTexCoord;
varying vec2 vTexCoord;
void main() {
    gl_Position = vec4(aPosition, 0.0, 1.0);
    vTexCoord = aTexCoord;
}
)";

static const char* g_fragmentShaderSourceYuv420p = R"(
precision mediump float;
varying vec2 vTexCoord;
uniform sampler2D uTextureY;
uniform sampler2D uTextureU;
uniform sampler2D uTextureV;

void main() {
    vec2 uv = vTexCoord;
    float y = texture2D(uTextureY, uv).r;
    float u = texture2D(uTextureU, uv).r - 0.5;
    float v = texture2D(uTextureV, uv).r - 0.5;

    float r = y + 1.402 * v;
    float g = y - 0.344 * u - 0.714 * v;
    float b = y + 1.772 * u;

    gl_FragColor = vec4(r, g, b, 1.0);
}
)";

// --- NV12 2-texture shader for DMABUF zero-copy path ---

static const char* g_fragmentShaderSourceNv12 = R"(
precision mediump float;
varying vec2 vTexCoord;
uniform sampler2D uTextureY;
uniform sampler2D uTextureUV;

void main() {
    vec2 uv = vTexCoord;
    float y = texture2D(uTextureY, uv).r;
    vec2 uv_vals = texture2D(uTextureUV, uv).rg;
    float u = uv_vals.r - 0.5;
    float v = uv_vals.g - 0.5;

    float r = y + 1.402 * v;
    float g = y - 0.344 * u - 0.714 * v;
    float b = y + 1.772 * u;

    gl_FragColor = vec4(r, g, b, 1.0);
}
)";

static inline uint32_t make_fourcc(char a, char b, char c, char d) {
    return ((uint32_t)d << 24) | ((uint32_t)c << 16) | ((uint32_t)b << 8) | (uint32_t)a;
}

static const uint32_t R8  = make_fourcc('R', '8', ' ', ' ');
static const uint32_t GR88 = make_fourcc('G', 'R', '8', '8');

static const float g_vertices[] = {
    -1.0f, -1.0f,  0.0f, 1.0f,
     1.0f, -1.0f,  1.0f, 1.0f,
     1.0f,  1.0f,  1.0f, 0.0f,
    -1.0f, -1.0f,  0.0f, 1.0f,
     1.0f,  1.0f,  1.0f, 0.0f,
    -1.0f,  1.0f,  0.0f, 0.0f
};

// --- EGL init ---

bool EGLRenderer::initEGL(wl_display* wlDisplay) {
    m_eglDisplay = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, wlDisplay, nullptr);
    if (m_eglDisplay == EGL_NO_DISPLAY) {
        std::cerr << "Failed to get EGL Wayland display" << std::endl;
        return false;
    }

    EGLint major, minor;
    if (!eglInitialize(m_eglDisplay, &major, &minor)) {
        std::cerr << "Failed to initialize EGL" << std::endl;
        return false;
    }

    std::cout << "EGL initialized: " << major << "." << minor << std::endl;

    EGLint configAttribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLConfig configs[10];
    EGLint numConfigs;
    if (!eglChooseConfig(m_eglDisplay, configAttribs, configs, 10, &numConfigs)) {
        std::cerr << "Failed to choose EGL config" << std::endl;
        eglTerminate(m_eglDisplay);
        m_eglDisplay = EGL_NO_DISPLAY;
        return false;
    }

    if (numConfigs == 0) {
        std::cerr << "No EGL configs found" << std::endl;
        eglTerminate(m_eglDisplay);
        m_eglDisplay = EGL_NO_DISPLAY;
        return false;
    }

    m_eglConfig = configs[0];

    EGLint contextAttribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };

    m_eglContext = eglCreateContext(m_eglDisplay, m_eglConfig, EGL_NO_CONTEXT, contextAttribs);
    if (m_eglContext == EGL_NO_CONTEXT) {
        std::cerr << "Failed to create EGL context: 0x" << std::hex << eglGetError() << std::dec << std::endl;
        eglTerminate(m_eglDisplay);
        m_eglDisplay = EGL_NO_DISPLAY;
        return false;
    }

    return true;
}

// --- EGLRenderer ---

EGLRenderer::EGLRenderer() {
}

EGLRenderer::~EGLRenderer() {
    cleanup();
}

bool EGLRenderer::init(wl_display* display, wl_egl_window* nativeWindow) {
    if (!display || !nativeWindow) {
        std::cerr << "Display or native window is null" << std::endl;
        return false;
    }

    if (!initEGL(display)) {
        std::cerr << "Failed to initialize EGL" << std::endl;
        return false;
    }

    m_eglSurface = eglCreateWindowSurface(m_eglDisplay, m_eglConfig,
                                           (EGLNativeWindowType)nativeWindow, nullptr);
    if (m_eglSurface == EGL_NO_SURFACE) {
        std::cerr << "Failed to create EGL surface: 0x" << std::hex << eglGetError() << std::dec << std::endl;
        return false;
    }

    if (!eglMakeCurrent(m_eglDisplay, m_eglSurface, m_eglSurface, m_eglContext)) {
        std::cerr << "Failed to make EGL context current" << std::endl;
        return false;
    }

    m_dmaBufSupported = initDmaBufExtensions();
    if (m_dmaBufSupported) {
        std::cout << "DMABUF import supported, zero-copy path enabled" << std::endl;
    } else {
        std::cout << "DMABUF import not supported, using CPU fallback only" << std::endl;
    }

    if (!initShaders()) {
        std::cerr << "Failed to initialize shaders" << std::endl;
        return false;
    }

    createTextures();
    m_initialized = true;
    return true;
}

bool EGLRenderer::initDmaBufExtensions() {
    const char* eglExtensions = eglQueryString(m_eglDisplay, EGL_EXTENSIONS);
    if (!eglExtensions || !strstr(eglExtensions, "EGL_EXT_image_dma_buf_import")) {
        std::cerr << "EGL_EXT_image_dma_buf_import not available" << std::endl;
        return false;
    }

    m_hasDmaBufModifiers = strstr(eglExtensions, "EGL_EXT_image_dma_buf_import_modifiers") != nullptr;

    const char* glExtensions = (const char*)glGetString(GL_EXTENSIONS);
    if (!glExtensions || !strstr(glExtensions, "GL_OES_EGL_image")) {
        std::cerr << "GL_OES_EGL_image not available" << std::endl;
        return false;
    }

    m_glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    if (!m_glEGLImageTargetTexture2DOES) {
        std::cerr << "glEGLImageTargetTexture2DOES not available" << std::endl;
        return false;
    }

    return true;
}

// --- Shader compilation ---

static GLuint compileShader(GLenum type, const char* src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint compiled;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        GLchar log[1024];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::cerr << "Shader compile error: " << log << std::endl;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint linkProgram(GLuint vs, GLuint fs) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);

    GLint linked;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        GLchar log[1024];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::cerr << "Program link error: " << log << std::endl;
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

bool EGLRenderer::initShaders() {
    GLuint vs = compileShader(GL_VERTEX_SHADER, g_vertexShaderSource);
    if (!vs) return false;

    // YUV420P program (CPU fallback)
    GLuint fsYuv = compileShader(GL_FRAGMENT_SHADER, g_fragmentShaderSourceYuv420p);
    if (!fsYuv) { glDeleteShader(vs); return false; }

    m_programYuv420p = linkProgram(vs, fsYuv);
    glDeleteShader(fsYuv);
    if (!m_programYuv420p) { glDeleteShader(vs); return false; }

    m_yuvPositionLoc = glGetAttribLocation(m_programYuv420p, "aPosition");
    m_yuvTexCoordLoc = glGetAttribLocation(m_programYuv420p, "aTexCoord");
    m_yuvTextureYLoc = glGetUniformLocation(m_programYuv420p, "uTextureY");
    m_yuvTextureULoc = glGetUniformLocation(m_programYuv420p, "uTextureU");
    m_yuvTextureVLoc = glGetUniformLocation(m_programYuv420p, "uTextureV");

    // NV12 program (DMABUF zero-copy)
    GLuint fsNv12 = compileShader(GL_FRAGMENT_SHADER, g_fragmentShaderSourceNv12);
    if (!fsNv12) { glDeleteShader(vs); return false; }

    m_programNv12 = linkProgram(vs, fsNv12);
    glDeleteShader(fsNv12);
    glDeleteShader(vs);
    if (!m_programNv12) return false;

    m_nv12PositionLoc = glGetAttribLocation(m_programNv12, "aPosition");
    m_nv12TexCoordLoc = glGetAttribLocation(m_programNv12, "aTexCoord");
    m_nv12TextureYLoc = glGetUniformLocation(m_programNv12, "uTextureY");
    m_nv12TextureUVLoc = glGetUniformLocation(m_programNv12, "uTextureUV");

    glGenBuffers(1, &m_vertexBuffer);
    glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(g_vertices), g_vertices, GL_STATIC_DRAW);

    return true;
}

// --- Texture management ---

void EGLRenderer::createTextures() {
    // CPU fallback textures (3 textures, shared)
    GLuint cpuTex[3];
    glGenTextures(3, cpuTex);
    m_textureY = cpuTex[0];
    m_textureU = cpuTex[1];
    m_textureV = cpuTex[2];
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, cpuTex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    m_texturesCreated = true;
}

void EGLRenderer::setPlayerCount(int count) {
    if (count <= 0) return;

    // Free existing per-player textures
    if (!m_playerTexY.empty()) {
        glDeleteTextures(m_playerTexY.size(), m_playerTexY.data());
        glDeleteTextures(m_playerTexUV.size(), m_playerTexUV.data());
        glDeleteTextures(m_cpuTexY.size(), m_cpuTexY.data());
        glDeleteTextures(m_cpuTexU.size(), m_cpuTexU.data());
        glDeleteTextures(m_cpuTexV.size(), m_cpuTexV.data());
    }

    m_playerTexY.resize(count);
    m_playerTexUV.resize(count);
    m_cpuTexY.resize(count);
    m_cpuTexU.resize(count);
    m_cpuTexV.resize(count);
    glGenTextures(count, m_playerTexY.data());
    glGenTextures(count, m_playerTexUV.data());
    glGenTextures(count, m_cpuTexY.data());
    glGenTextures(count, m_cpuTexU.data());
    glGenTextures(count, m_cpuTexV.data());

    for (int i = 0; i < count; i++) {
        // DMA-BUF textures
        glBindTexture(GL_TEXTURE_2D, m_playerTexY[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindTexture(GL_TEXTURE_2D, m_playerTexUV[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // CPU fallback textures
        glBindTexture(GL_TEXTURE_2D, m_cpuTexY[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindTexture(GL_TEXTURE_2D, m_cpuTexU[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        glBindTexture(GL_TEXTURE_2D, m_cpuTexV[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
}

void EGLRenderer::destroyTextures() {
    if (!m_texturesCreated) return;

    if (!m_playerTexY.empty()) {
        glDeleteTextures(m_playerTexY.size(), m_playerTexY.data());
        glDeleteTextures(m_playerTexUV.size(), m_playerTexUV.data());
        m_playerTexY.clear();
        m_playerTexUV.clear();
    }

    if (!m_cpuTexY.empty()) {
        glDeleteTextures(m_cpuTexY.size(), m_cpuTexY.data());
        glDeleteTextures(m_cpuTexU.size(), m_cpuTexU.data());
        glDeleteTextures(m_cpuTexV.size(), m_cpuTexV.data());
        m_cpuTexY.clear();
        m_cpuTexU.clear();
        m_cpuTexV.clear();
    }

    GLuint cpuTex[] = { m_textureY, m_textureU, m_textureV };
    glDeleteTextures(3, cpuTex);

    m_texturesCreated = false;
}

// --- Cleanup ---

void EGLRenderer::cleanup() {
    if (!m_initialized) return;

    destroyEGLImages();
    destroyTextures();

    if (m_vertexBuffer) {
        glDeleteBuffers(1, &m_vertexBuffer);
        m_vertexBuffer = 0;
    }
    if (m_programYuv420p) {
        glDeleteProgram(m_programYuv420p);
        m_programYuv420p = 0;
    }
    if (m_programNv12) {
        glDeleteProgram(m_programNv12);
        m_programNv12 = 0;
    }

    if (m_eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    if (m_eglSurface != EGL_NO_SURFACE) {
        eglDestroySurface(m_eglDisplay, m_eglSurface);
        m_eglSurface = EGL_NO_SURFACE;
    }
    if (m_eglContext != EGL_NO_CONTEXT) {
        eglDestroyContext(m_eglDisplay, m_eglContext);
        m_eglContext = EGL_NO_CONTEXT;
    }
    if (m_eglDisplay != EGL_NO_DISPLAY) {
        eglTerminate(m_eglDisplay);
        m_eglDisplay = EGL_NO_DISPLAY;
    }

    m_initialized = false;
}

// --- Viewport ---

void EGLRenderer::setViewport(int width, int height) {
    m_viewportWidth = width;
    m_viewportHeight = height;
    glViewport(0, 0, width, height);
}

// --- CPU fallback path ---

void EGLRenderer::updateTextureFromFrame(const VideoFrame& frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.data.empty()) return;

    int y_size = frame.width * frame.height;
    int uv_width = frame.width / 2;
    int uv_height = frame.height / 2;

    glBindTexture(GL_TEXTURE_2D, m_textureY);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frame.width, frame.height,
                 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame.data.data());

    glBindTexture(GL_TEXTURE_2D, m_textureU);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, uv_width, uv_height,
                 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame.data.data() + y_size);

    glBindTexture(GL_TEXTURE_2D, m_textureV);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, uv_width, uv_height,
                 0, GL_LUMINANCE, GL_UNSIGNED_BYTE,
                 frame.data.data() + y_size + uv_width * uv_height);
}

void EGLRenderer::renderQuadCpu() {
    glUseProgram(m_programYuv420p);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textureY);
    glUniform1i(m_yuvTextureYLoc, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textureU);
    glUniform1i(m_yuvTextureULoc, 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textureV);
    glUniform1i(m_yuvTextureVLoc, 2);

    glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);

    if (m_yuvPositionLoc >= 0) {
        glEnableVertexAttribArray(m_yuvPositionLoc);
        glVertexAttribPointer(m_yuvPositionLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), (void*)0);
    }
    if (m_yuvTexCoordLoc >= 0) {
        glEnableVertexAttribArray(m_yuvTexCoordLoc);
        glVertexAttribPointer(m_yuvTexCoordLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), (void*)(2 * sizeof(float)));
    }

    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (m_yuvPositionLoc >= 0) glDisableVertexAttribArray(m_yuvPositionLoc);
    if (m_yuvTexCoordLoc >= 0) glDisableVertexAttribArray(m_yuvTexCoordLoc);
}

void EGLRenderer::renderFrame(const VideoFrame& frame) {
    if (!m_initialized) return;
    eglMakeCurrent(m_eglDisplay, m_eglSurface, m_eglSurface, m_eglContext);
    glViewport(0, 0, m_viewportWidth, m_viewportHeight);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    if (frame.width > 0 && frame.height > 0 && !frame.data.empty()) {
        updateTextureFromFrame(frame);
    }
    renderQuadCpu();
}

void EGLRenderer::renderCpuFrame(const VideoFrame& frame) {
    if (!m_initialized) return;
    if (frame.width > 0 && frame.height > 0 && !frame.data.empty()) {
        updateTextureFromFrame(frame);
    }
    renderQuadCpu();
}

// --- DMABUF path with per-player textures ---

bool EGLRenderer::createEGLImages(DmaBufFrame& frame, int playerIndex) {
    if (!m_dmaBufSupported || !frame.valid) return false;
    if (playerIndex < 0 || playerIndex >= (int)m_playerTexY.size()) return false;

    VADRMPRIMESurfaceDescriptor& desc = frame.desc;

    if (desc.num_layers < 2) {
        std::cerr << "VA export returned " << desc.num_layers << " layers" << std::endl;
        return false;
    }

    const uint32_t formats[2] = { R8, GR88 };
    const int div[2] = { 1, 2 };

    for (int i = 0; i < 2; i++) {
        const uint32_t layer = i;

        int fds[4];
        uint32_t offsets[4];
        uint32_t pitches[4];
        uint64_t modifiers[4];
        for (uint32_t j = 0; j < desc.layers[layer].num_planes; j++) {
            fds[j] = desc.objects[desc.layers[layer].object_index[j]].fd;
            offsets[j] = desc.layers[layer].offset[j];
            pitches[j] = desc.layers[layer].pitch[j];
            modifiers[j] = desc.objects[desc.layers[layer].object_index[j]].drm_format_modifier;
        }

        intptr_t img_attr[44];
        int idx = 0;

        img_attr[idx++] = EGL_LINUX_DRM_FOURCC_EXT;
        img_attr[idx++] = (intptr_t)formats[i];

        img_attr[idx++] = EGL_WIDTH;
        img_attr[idx++] = (intptr_t)(desc.width / div[i]);

        img_attr[idx++] = EGL_HEIGHT;
        img_attr[idx++] = (intptr_t)(desc.height / div[i]);

        for (uint32_t j = 0; j < desc.layers[layer].num_planes; j++) {
            const intptr_t plane_fd_attrs[] = {
                EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
                EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT
            };
            const intptr_t plane_offset_attrs[] = {
                EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT
            };
            const intptr_t plane_pitch_attrs[] = {
                EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT
            };

            img_attr[idx++] = plane_fd_attrs[j];
            img_attr[idx++] = (intptr_t)fds[j];
            img_attr[idx++] = plane_offset_attrs[j];
            img_attr[idx++] = (intptr_t)offsets[j];
            img_attr[idx++] = plane_pitch_attrs[j];
            img_attr[idx++] = (intptr_t)pitches[j];

            if (m_hasDmaBufModifiers) {
                const intptr_t plane_mod_lo[] = {
                    EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                    EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT
                };
                const intptr_t plane_mod_hi[] = {
                    EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                    EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT
                };

                img_attr[idx++] = plane_mod_lo[j];
                img_attr[idx++] = (intptr_t)(modifiers[j] & 0xFFFFFFFFULL);
                img_attr[idx++] = plane_mod_hi[j];
                img_attr[idx++] = (intptr_t)(modifiers[j] >> 32ULL);
            }
        }

        img_attr[idx++] = EGL_NONE;

        while (eglGetError() != EGL_SUCCESS) {}

        EGLImage image = eglCreateImage(m_eglDisplay, EGL_NO_CONTEXT,
                                         EGL_LINUX_DMA_BUF_EXT, nullptr, img_attr);
        if (!image) {
            std::cerr << "Failed to create EGLImage for player " << playerIndex
                      << " layer " << i << ": 0x" << std::hex << eglGetError() << std::dec << std::endl;
            return false;
        }

        // Bind to this player's own texture
        GLuint tex = (i == 0) ? m_playerTexY[playerIndex] : m_playerTexUV[playerIndex];
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

        while (glGetError() != 0) {}
        while (eglGetError() != EGL_SUCCESS) {}

        m_glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, (GLeglImageOES)image);

        GLenum glErr = glGetError();
        EGLint eglErr = eglGetError();
        if (glErr != 0 || eglErr != EGL_SUCCESS) {
            std::cerr << "Failed to bind EGLImage for player " << playerIndex
                      << " layer " << i << std::endl;
            eglDestroyImage(m_eglDisplay, image);
            return false;
        }

        eglDestroyImage(m_eglDisplay, image);
    }

    return true;
}

void EGLRenderer::destroyEGLImages() {
    glBindTexture(GL_TEXTURE_2D, 0);
}

void EGLRenderer::bindEGLImages() {
}

void EGLRenderer::renderQuadDmaBuf(int playerIndex) {
    if (playerIndex < 0 || playerIndex >= (int)m_playerTexY.size()) return;

    glUseProgram(m_programNv12);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_playerTexY[playerIndex]);
    glUniform1i(m_nv12TextureYLoc, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_playerTexUV[playerIndex]);
    glUniform1i(m_nv12TextureUVLoc, 1);

    glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);

    if (m_nv12PositionLoc >= 0) {
        glEnableVertexAttribArray(m_nv12PositionLoc);
        glVertexAttribPointer(m_nv12PositionLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), (void*)0);
    }
    if (m_nv12TexCoordLoc >= 0) {
        glEnableVertexAttribArray(m_nv12TexCoordLoc);
        glVertexAttribPointer(m_nv12TexCoordLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), (void*)(2 * sizeof(float)));
    }

    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (m_nv12PositionLoc >= 0) glDisableVertexAttribArray(m_nv12PositionLoc);
    if (m_nv12TexCoordLoc >= 0) glDisableVertexAttribArray(m_nv12TexCoordLoc);
}

// --- Public render API ---

void EGLRenderer::beginFrame() {
    if (!m_initialized) return;
    eglMakeCurrent(m_eglDisplay, m_eglSurface, m_eglSurface, m_eglContext);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

void EGLRenderer::renderDmaBufFrame(DmaBufFrame& frame) {
    renderDmaBufToRect(frame, 0, 0, 0, m_viewportWidth, m_viewportHeight);
}

void EGLRenderer::renderDmaBufFramePrev(int playerIndex) {
    if (!m_initialized) return;
    glViewport(0, 0, m_viewportWidth, m_viewportHeight);
    renderQuadDmaBuf(playerIndex);
}

void EGLRenderer::renderCpuFramePrev() {
    if (!m_initialized) return;
    renderQuadCpu();
}

void EGLRenderer::renderDmaBufToRect(DmaBufFrame& frame,
                                      int playerIndex,
                                      int vp_x, int vp_y, int vp_w, int vp_h) {
    if (!m_initialized) return;

    glViewport(vp_x, vp_y, vp_w, vp_h);

    if (frame.valid) {
        if (createEGLImages(frame, playerIndex)) {
            // Player's textures now reference this frame's DMA-BUF
        }
    }

    renderQuadDmaBuf(playerIndex);
}

void EGLRenderer::renderDmaBufToRectPrev(int playerIndex,
                                          int vp_x, int vp_y, int vp_w, int vp_h) {
    if (!m_initialized) return;
    glViewport(vp_x, vp_y, vp_w, vp_h);
    renderQuadDmaBuf(playerIndex);
}

void EGLRenderer::renderCpuFrameToRect(const VideoFrame& frame,
                                        int playerIndex,
                                        int vp_x, int vp_y, int vp_w, int vp_h) {
    if (!m_initialized) return;
    if (playerIndex < 0 || playerIndex >= (int)m_cpuTexY.size()) return;

    if (frame.width > 0 && frame.height > 0 && !frame.data.empty()) {
        int y_size  = frame.width * frame.height;
        int uv_w    = frame.width / 2;
        int uv_h    = frame.height / 2;

        glBindTexture(GL_TEXTURE_2D, m_cpuTexY[playerIndex]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, frame.width, frame.height,
                     0, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame.data.data());

        glBindTexture(GL_TEXTURE_2D, m_cpuTexU[playerIndex]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, uv_w, uv_h,
                     0, GL_LUMINANCE, GL_UNSIGNED_BYTE, frame.data.data() + y_size);

        glBindTexture(GL_TEXTURE_2D, m_cpuTexV[playerIndex]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, uv_w, uv_h,
                     0, GL_LUMINANCE, GL_UNSIGNED_BYTE,
                     frame.data.data() + y_size + uv_w * uv_h);
    }

    glViewport(vp_x, vp_y, vp_w, vp_h);
    glUseProgram(m_programYuv420p);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_cpuTexY[playerIndex]);
    glUniform1i(m_yuvTextureYLoc, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_cpuTexU[playerIndex]);
    glUniform1i(m_yuvTextureULoc, 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_cpuTexV[playerIndex]);
    glUniform1i(m_yuvTextureVLoc, 2);

    glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);

    if (m_yuvPositionLoc >= 0) {
        glEnableVertexAttribArray(m_yuvPositionLoc);
        glVertexAttribPointer(m_yuvPositionLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), (void*)0);
    }
    if (m_yuvTexCoordLoc >= 0) {
        glEnableVertexAttribArray(m_yuvTexCoordLoc);
        glVertexAttribPointer(m_yuvTexCoordLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), (void*)(2 * sizeof(float)));
    }

    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (m_yuvPositionLoc >= 0) glDisableVertexAttribArray(m_yuvPositionLoc);
    if (m_yuvTexCoordLoc >= 0) glDisableVertexAttribArray(m_yuvTexCoordLoc);
}

void EGLRenderer::renderCpuFrameToRectPrev(int playerIndex,
                                            int vp_x, int vp_y, int vp_w, int vp_h) {
    if (!m_initialized) return;
    if (playerIndex < 0 || playerIndex >= (int)m_cpuTexY.size()) return;

    glViewport(vp_x, vp_y, vp_w, vp_h);
    glUseProgram(m_programYuv420p);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_cpuTexY[playerIndex]);
    glUniform1i(m_yuvTextureYLoc, 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_cpuTexU[playerIndex]);
    glUniform1i(m_yuvTextureULoc, 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_cpuTexV[playerIndex]);
    glUniform1i(m_yuvTextureVLoc, 2);

    glBindBuffer(GL_ARRAY_BUFFER, m_vertexBuffer);

    if (m_yuvPositionLoc >= 0) {
        glEnableVertexAttribArray(m_yuvPositionLoc);
        glVertexAttribPointer(m_yuvPositionLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), (void*)0);
    }
    if (m_yuvTexCoordLoc >= 0) {
        glEnableVertexAttribArray(m_yuvTexCoordLoc);
        glVertexAttribPointer(m_yuvTexCoordLoc, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), (void*)(2 * sizeof(float)));
    }

    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (m_yuvPositionLoc >= 0) glDisableVertexAttribArray(m_yuvPositionLoc);
    if (m_yuvTexCoordLoc >= 0) glDisableVertexAttribArray(m_yuvTexCoordLoc);
}

// --- Release context ---

void EGLRenderer::releaseContext() {
    if (m_eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
}

// --- Swap buffers ---

void EGLRenderer::swapBuffers() {
    if (m_initialized && m_eglSurface != EGL_NO_SURFACE) {
        eglSwapBuffers(m_eglDisplay, m_eglSurface);
    }
}
