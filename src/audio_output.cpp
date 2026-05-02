#include "audio_output.h"
#include <iostream>
#include <dlfcn.h>
#include <cstring>

// ---------------------------------------------------------------------------
// Minimal PulseAudio type definitions (no -dev headers required)
// ---------------------------------------------------------------------------

enum pa_sample_format {
    PA_SAMPLE_U8,
    PA_SAMPLE_ALAW,
    PA_SAMPLE_ULAW,
    PA_SAMPLE_S16LE,
    PA_SAMPLE_S16BE,
    PA_SAMPLE_FLOAT32LE,
    PA_SAMPLE_FLOAT32BE,
    PA_SAMPLE_S32LE,
    PA_SAMPLE_S32BE,
    PA_SAMPLE_S24LE,
    PA_SAMPLE_S24BE,
    PA_SAMPLE_S24_32LE,
    PA_SAMPLE_S24_32BE,
    PA_SAMPLE_MAX,
    PA_SAMPLE_INVALID = -1
};

struct pa_sample_spec {
    int format;
    uint32_t rate;
    uint8_t channels;
};

struct pa_channel_map {
    uint8_t channels;
    int map[8];
};

struct pa_buffer_attr {
    uint32_t maxlength;
    uint32_t tlength;
    uint32_t prebuf;
    uint32_t minreq;
    uint32_t fragsize;
};

// ---------------------------------------------------------------------------
// DSO function pointers
// ---------------------------------------------------------------------------

typedef void* (*pa_simple_new_t)(const char*, const char*,
    int, const char*, const char*,
    const pa_sample_spec*, const pa_channel_map*,
    const pa_buffer_attr*, int*);
typedef int  (*pa_simple_write_t)(void*, const void*, size_t, int*);
typedef int  (*pa_simple_drain_t)(void*, int*);
typedef void (*pa_simple_free_t)(void*);
typedef const char* (*pa_strerror_t)(int);

// ---------------------------------------------------------------------------
// Shared library handle (loaded once)
// ---------------------------------------------------------------------------

static void* g_pa_handle = nullptr;

static pa_simple_new_t   g_pa_simple_new   = nullptr;
static pa_simple_write_t g_pa_simple_write = nullptr;
static pa_simple_drain_t g_pa_simple_drain = nullptr;
static pa_simple_free_t  g_pa_simple_free  = nullptr;
static pa_strerror_t     g_pa_strerror     = nullptr;

static bool load_pulse_library() {
    static bool attempted = false;
    if (attempted) return g_pa_handle != nullptr;
    attempted = true;

    g_pa_handle = dlopen("libpulse-simple.so.0", RTLD_LAZY);
    if (!g_pa_handle) {
        std::cerr << "libpulse-simple not found — audio disabled" << std::endl;
        return false;
    }

    auto load = [](const char* name) {
        void* sym = dlsym(g_pa_handle, name);
        if (!sym) {
            std::cerr << "Failed to resolve PulseAudio symbol: " << name << std::endl;
        }
        return sym;
    };

    g_pa_simple_new   = (pa_simple_new_t)  load("pa_simple_new");
    g_pa_simple_write = (pa_simple_write_t)load("pa_simple_write");
    g_pa_simple_drain = (pa_simple_drain_t)load("pa_simple_drain");
    g_pa_simple_free  = (pa_simple_free_t) load("pa_simple_free");
    g_pa_strerror     = (pa_strerror_t)    load("pa_strerror");

    if (!g_pa_simple_new || !g_pa_simple_write || !g_pa_simple_free) {
        dlclose(g_pa_handle);
        g_pa_handle = nullptr;
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Per-instance AudioOutput
// ---------------------------------------------------------------------------

struct AudioOutput {
    void*   handle      = nullptr;   // pa_simple*
    int     sample_rate = 0;
    int     channels    = 0;
    bool    active      = false;
};

AudioOutput* audio_output_open(int sample_rate, int channels) {
    if (!load_pulse_library()) return nullptr;

    auto* out = new AudioOutput();
    out->sample_rate = sample_rate;
    out->channels    = channels;

    pa_sample_spec ss = {};
    ss.format   = PA_SAMPLE_S16LE;
    ss.rate     = (uint32_t)sample_rate;
    ss.channels = (uint8_t)channels;

    pa_buffer_attr attr = {};
    attr.maxlength = uint32_t(-1);
    attr.tlength   = uint32_t(-1);
    attr.minreq    = uint32_t(-1);
    attr.prebuf    = uint32_t(-1);
    attr.fragsize  = uint32_t(-1);

    int error = 0;
    out->handle = g_pa_simple_new(nullptr, "multi_video_player",
                                  1, // PA_STREAM_PLAYBACK
                                  nullptr, "Audio", &ss, nullptr, &attr, &error);
    if (!out->handle) {
        std::cerr << "pa_simple_new failed: "
                  << (g_pa_strerror ? g_pa_strerror(error) : "unknown")
                  << " — audio disabled for this player" << std::endl;
        delete out;
        return nullptr;
    }

    out->active = true;
    std::cout << "Audio output opened: " << sample_rate << " Hz, "
              << channels << " channels" << std::endl;
    return out;
}

void audio_output_write(AudioOutput* ctx, const uint8_t* data, size_t bytes) {
    if (!ctx || !ctx->active) return;

    int error = 0;
    if (g_pa_simple_write(ctx->handle, data, bytes, &error) < 0) {
        std::cerr << "pa_simple_write: "
                  << (g_pa_strerror ? g_pa_strerror(error) : "unknown") << std::endl;
    }
}

void audio_output_close(AudioOutput* ctx) {
    if (!ctx) return;
    if (ctx->active && ctx->handle) {
        g_pa_simple_drain(ctx->handle, nullptr);
        g_pa_simple_free(ctx->handle);
    }
    delete ctx;
}
