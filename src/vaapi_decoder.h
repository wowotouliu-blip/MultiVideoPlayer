#ifndef VAAPI_DECODER_H
#define VAAPI_DECODER_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <va/va.h>
#include <va/va_drmcommon.h>
}

#include <functional>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <atomic>
#include <vector>

extern "C" {
#include <libswresample/swresample.h>
}

struct AudioOutput;

// CPU-side YUV420P frame (fallback path)
struct VideoFrame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data;
    int64_t pts = 0;
    AVRational time_base = {0, 1};

    void release();
};

// Zero-copy DMABUF frame — VADRMPRIMESurfaceDescriptor owns open DMA-BUF FDs
struct DmaBufFrame {
    int width = 0;
    int height = 0;
    int64_t pts = 0;
    AVRational time_base = {0, 1};
    VADRMPRIMESurfaceDescriptor desc = {};
    bool valid = false;

    DmaBufFrame() = default;

    DmaBufFrame(DmaBufFrame&& other) noexcept
        : width(other.width), height(other.height)
        , pts(other.pts), time_base(other.time_base)
        , desc(other.desc), valid(other.valid)
    {
        other.valid = false;
        for (uint32_t i = 0; i < other.desc.num_objects; i++) {
            other.desc.objects[i].fd = -1;
        }
    }

    DmaBufFrame& operator=(DmaBufFrame&& other) noexcept {
        if (this != &other) {
            release();
            width = other.width;
            height = other.height;
            pts = other.pts;
            time_base = other.time_base;
            desc = other.desc;
            valid = other.valid;
            other.valid = false;
            for (uint32_t i = 0; i < other.desc.num_objects; i++) {
                other.desc.objects[i].fd = -1;
            }
        }
        return *this;
    }

    DmaBufFrame(const DmaBufFrame&) = delete;
    DmaBufFrame& operator=(const DmaBufFrame&) = delete;

    ~DmaBufFrame() { release(); }

    void release();
};

class VaapiDecoder {
public:
    using FrameCallback = std::function<void(const VideoFrame&)>;
    using DmaBufCallback = std::function<void(DmaBufFrame&&)>;

    VaapiDecoder();
    ~VaapiDecoder();

    bool open(const std::string& filename);
    void close();

    void start(FrameCallback callback);
    void startZeroCopy(DmaBufCallback callback);
    void startAll(DmaBufCallback dmaCb, FrameCallback cpuCb);
    void stop();

    bool isOpen() const { return m_formatContext != nullptr; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    double fps() const { return m_fps; }
    bool isEOF() const { return m_eof; }

private:
    void decodeThread();
    bool initHardwareDevice();
    bool initVideoDecoder();
    bool initAudioDecoder();
    void decodeAudioPacket(AVPacket* packet);
    bool exportFrameCpu(AVFrame* frame, VideoFrame& outFrame);
    bool exportFrameDmaBuf(AVFrame* frame, DmaBufFrame& outFrame);

    AVFormatContext* m_formatContext = nullptr;
    AVCodecContext* m_codecContext = nullptr;
    AVBufferRef* m_hwDeviceCtx = nullptr;
    int m_videoStreamIndex = -1;

    // Audio
    int m_audioStreamIndex = -1;
    AVCodecContext* m_audioCodecContext = nullptr;
    SwrContext* m_swrContext = nullptr;
    AudioOutput* m_audioOutput = nullptr;
    int m_audioSampleRate = 0;
    int m_audioChannels = 0;

    int m_width = 0;
    int m_height = 0;
    double m_fps = 0.0;

    std::string m_filename;
    FrameCallback m_frameCallback;
    DmaBufCallback m_dmaBufCallback;

    std::thread m_decodeThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_eof{false};
};

#endif // VAAPI_DECODER_H
