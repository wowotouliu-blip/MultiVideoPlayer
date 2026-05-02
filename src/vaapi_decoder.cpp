#include "vaapi_decoder.h"
#include "audio_output.h"
#include <iostream>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <chrono>

extern "C" {
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/channel_layout.h>
}

void VideoFrame::release() {
    data.clear();
}

void DmaBufFrame::release() {
    if (!valid) return;
    for (uint32_t i = 0; i < desc.num_objects; i++) {
        if (desc.objects[i].fd >= 0) {
            ::close(desc.objects[i].fd);
            desc.objects[i].fd = -1;
        }
    }
    valid = false;
}

VaapiDecoder::VaapiDecoder() {
}

VaapiDecoder::~VaapiDecoder() {
    close();
}

bool VaapiDecoder::open(const std::string& filename) {
    m_filename = filename;

    m_formatContext = avformat_alloc_context();
    if (!m_formatContext) {
        std::cerr << "Failed to allocate format context" << std::endl;
        return false;
    }

    if (avformat_open_input(&m_formatContext, filename.c_str(), nullptr, nullptr) < 0) {
        std::cerr << "Failed to open file: " << filename << std::endl;
        close();
        return false;
    }

    if (avformat_find_stream_info(m_formatContext, nullptr) < 0) {
        std::cerr << "Failed to find stream info" << std::endl;
        close();
        return false;
    }

    for (unsigned int i = 0; i < m_formatContext->nb_streams; i++) {
        if (m_formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            m_videoStreamIndex = i;
            break;
        }
    }

    if (m_videoStreamIndex < 0) {
        std::cerr << "No video stream found" << std::endl;
        close();
        return false;
    }

    AVStream* stream = m_formatContext->streams[m_videoStreamIndex];
    m_width = stream->codecpar->width;
    m_height = stream->codecpar->height;

    if (stream->avg_frame_rate.den > 0 && stream->avg_frame_rate.num > 0) {
        m_fps = av_q2d(stream->avg_frame_rate);
    } else {
        m_fps = 30.0;
    }

    if (!initHardwareDevice()) {
        std::cerr << "Failed to initialize hardware device, falling back to software" << std::endl;
    }

    if (!initVideoDecoder()) {
        std::cerr << "Failed to initialize video decoder" << std::endl;
        close();
        return false;
    }

    // Audio is optional — file may not have an audio stream
    initAudioDecoder();

    return true;
}

bool VaapiDecoder::initHardwareDevice() {
    int err = av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI, "/dev/dri/renderD128", nullptr, 0);
    if (err < 0) {
        err = av_hwdevice_ctx_create(&m_hwDeviceCtx, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
        if (err < 0) {
            char errbuf[128];
            av_strerror(err, errbuf, sizeof(errbuf));
            std::cerr << "Failed to create VAAPI device: " << errbuf << std::endl;
            return false;
        }
    }
    return true;
}

bool VaapiDecoder::initVideoDecoder() {
    AVStream* stream = m_formatContext->streams[m_videoStreamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);

    if (!codec) {
        std::cerr << "Failed to find decoder" << std::endl;
        return false;
    }

    m_codecContext = avcodec_alloc_context3(codec);
    if (!m_codecContext) {
        std::cerr << "Failed to allocate codec context" << std::endl;
        return false;
    }

    if (avcodec_parameters_to_context(m_codecContext, stream->codecpar) < 0) {
        std::cerr << "Failed to copy codec parameters" << std::endl;
        return false;
    }

    if (m_hwDeviceCtx) {
        m_codecContext->hw_device_ctx = av_buffer_ref(m_hwDeviceCtx);
        m_codecContext->get_format = [](AVCodecContext*, const enum AVPixelFormat* pix_fmts) -> enum AVPixelFormat {
            for (const enum AVPixelFormat* p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
                if (*p == AV_PIX_FMT_VAAPI) {
                    return *p;
                }
            }
            std::cerr << "VAAPI pixel format not available, using software" << std::endl;
            return AV_PIX_FMT_YUV420P;
        };
    }

    if (avcodec_open2(m_codecContext, codec, nullptr) < 0) {
        std::cerr << "Failed to open codec" << std::endl;
        return false;
    }

    return true;
}

void VaapiDecoder::close() {
    stop();

    if (m_audioOutput) {
        audio_output_close(m_audioOutput);
        m_audioOutput = nullptr;
    }
    if (m_swrContext) {
        swr_free(&m_swrContext);
    }
    if (m_audioCodecContext) {
        avcodec_free_context(&m_audioCodecContext);
    }
    if (m_codecContext) {
        avcodec_free_context(&m_codecContext);
    }
    if (m_hwDeviceCtx) {
        av_buffer_unref(&m_hwDeviceCtx);
    }
    if (m_formatContext) {
        avformat_close_input(&m_formatContext);
    }
}

bool VaapiDecoder::initAudioDecoder() {
    for (unsigned int i = 0; i < m_formatContext->nb_streams; i++) {
        if (m_formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            m_audioStreamIndex = static_cast<int>(i);
            break;
        }
    }

    if (m_audioStreamIndex < 0) {
        return false;  // No audio stream — not an error
    }

    AVStream* stream = m_formatContext->streams[m_audioStreamIndex];
    const AVCodec* codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec) {
        std::cerr << "No audio decoder found for stream " << m_audioStreamIndex << std::endl;
        m_audioStreamIndex = -1;
        return false;
    }

    m_audioCodecContext = avcodec_alloc_context3(codec);
    if (!m_audioCodecContext) {
        m_audioStreamIndex = -1;
        return false;
    }

    if (avcodec_parameters_to_context(m_audioCodecContext, stream->codecpar) < 0) {
        m_audioStreamIndex = -1;
        return false;
    }

    if (avcodec_open2(m_audioCodecContext, codec, nullptr) < 0) {
        std::cerr << "Failed to open audio codec" << std::endl;
        m_audioStreamIndex = -1;
        return false;
    }

    m_audioSampleRate = m_audioCodecContext->sample_rate;
    m_audioChannels = m_audioCodecContext->channel_layout
                      ? av_get_channel_layout_nb_channels(m_audioCodecContext->channel_layout)
                      : m_audioCodecContext->channels;
    if (m_audioChannels <= 0) m_audioChannels = 2;
    if (m_audioSampleRate <= 0) m_audioSampleRate = 44100;

    // Set up swresample to convert to S16 interleaved
    int64_t out_ch_layout = av_get_default_channel_layout(m_audioChannels);

    m_swrContext = swr_alloc_set_opts(nullptr,
        out_ch_layout, AV_SAMPLE_FMT_S16, m_audioSampleRate,
        m_audioCodecContext->channel_layout, m_audioCodecContext->sample_fmt, m_audioSampleRate,
        0, nullptr);

    if (!m_swrContext) {
        std::cerr << "Failed to set up audio resampler" << std::endl;
        m_audioStreamIndex = -1;
        return false;
    }

    if (swr_init(m_swrContext) < 0) {
        std::cerr << "Failed to init audio resampler" << std::endl;
        swr_free(&m_swrContext);
        m_audioStreamIndex = -1;
        return false;
    }

    m_audioOutput = audio_output_open(m_audioSampleRate, m_audioChannels);
    if (!m_audioOutput) {
        std::cerr << "Audio output unavailable — video-only playback" << std::endl;
    }

    std::cout << "Audio stream opened: " << m_audioSampleRate << " Hz, "
              << m_audioChannels << " channels" << std::endl;
    return true;
}

void VaapiDecoder::decodeAudioPacket(AVPacket* packet) {
    if (m_audioStreamIndex < 0 || !m_audioCodecContext) return;

    int ret = avcodec_send_packet(m_audioCodecContext, packet);
    if (ret < 0) return;

    AVFrame* frame = av_frame_alloc();

    while (ret >= 0) {
        ret = avcodec_receive_frame(m_audioCodecContext, frame);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;

        if (m_swrContext && m_audioOutput) {
            int out_samples = swr_get_out_samples(m_swrContext, frame->nb_samples);
            int out_buf_size = out_samples * m_audioChannels * 2; // S16 = 2 bytes
            std::vector<uint8_t> out_buf(out_buf_size);

            uint8_t* out_ptr = out_buf.data();
            int converted = swr_convert(m_swrContext, &out_ptr, out_samples,
                                        (const uint8_t**)frame->data, frame->nb_samples);
            if (converted > 0) {
                audio_output_write(m_audioOutput, out_buf.data(),
                                   converted * m_audioChannels * 2);
            }
        }

        av_frame_unref(frame);
    }

    av_frame_free(&frame);
}

void VaapiDecoder::start(FrameCallback callback) {
    if (m_running) {
        return;
    }

    m_frameCallback = std::move(callback);
    m_running = true;
    m_eof = false;
    m_decodeThread = std::thread(&VaapiDecoder::decodeThread, this);
}

void VaapiDecoder::startZeroCopy(DmaBufCallback callback) {
    if (m_running) {
        return;
    }

    m_dmaBufCallback = std::move(callback);
    m_running = true;
    m_eof = false;
    m_decodeThread = std::thread(&VaapiDecoder::decodeThread, this);
}

void VaapiDecoder::startAll(DmaBufCallback dmaCb, FrameCallback cpuCb) {
    if (m_running) {
        return;
    }

    m_dmaBufCallback = std::move(dmaCb);
    m_frameCallback = std::move(cpuCb);
    m_running = true;
    m_eof = false;
    m_decodeThread = std::thread(&VaapiDecoder::decodeThread, this);
}

void VaapiDecoder::stop() {
    m_running = false;
    if (m_decodeThread.joinable()) {
        m_decodeThread.join();
    }
}

void VaapiDecoder::decodeThread() {
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    AVFrame* sw_frame = av_frame_alloc();

    // Frame pacing: pace decode to video's natural FPS
    auto lastFrameTime = std::chrono::steady_clock::now();
    double frameDuration = (m_fps > 0.0) ? (1.0 / m_fps) : (1.0 / 30.0);

    while (m_running && !m_eof) {
        int ret = av_read_frame(m_formatContext, packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                m_eof = true;
            } else {
                break;
            }
        }

        if (packet->stream_index == m_audioStreamIndex) {
            decodeAudioPacket(packet);
        }

        if (packet->stream_index == m_videoStreamIndex) {
            ret = avcodec_send_packet(m_codecContext, packet);
            if (ret < 0) {
                std::cerr << "Error sending packet" << std::endl;
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(m_codecContext, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    std::cerr << "Error decoding frame" << std::endl;
                    break;
                }

                // --- Zero-copy DMABUF path ---
                bool frameDelivered = false;
                if (m_dmaBufCallback && frame->format == AV_PIX_FMT_VAAPI && m_hwDeviceCtx) {
                    DmaBufFrame dmaFrame;
                    if (exportFrameDmaBuf(frame, dmaFrame)) {
                        m_dmaBufCallback(std::move(dmaFrame));
                        av_frame_unref(frame);
                        frameDelivered = true;
                    }
                    // exportFrameDmaBuf failed — fall through to CPU path
                }

                if (!frameDelivered) {
                    // --- CPU fallback path ---
                    AVFrame* displayFrame = frame;
                    if (frame->format == AV_PIX_FMT_VAAPI && m_hwDeviceCtx) {
                        ret = av_hwframe_transfer_data(sw_frame, frame, 0);
                        if (ret < 0) {
                            char errbuf[128];
                            av_strerror(ret, errbuf, sizeof(errbuf));
                            std::cerr << "Error transferring frame from hardware: " << errbuf << std::endl;
                            break;
                        }
                        displayFrame = sw_frame;
                    }

                    VideoFrame videoFrame;
                    if (exportFrameCpu(displayFrame, videoFrame)) {
                        if (m_frameCallback) {
                            m_frameCallback(videoFrame);
                        }
                        frameDelivered = true;
                    }
                }

                if (frameDelivered) {
                    // Pace to video framerate
                    auto now = std::chrono::steady_clock::now();
                    auto elapsed = std::chrono::duration<double>(now - lastFrameTime).count();
                    double sleepTime = frameDuration - elapsed;
                    if (sleepTime > 0.0) {
                        std::this_thread::sleep_for(
                            std::chrono::duration<double>(sleepTime));
                    }
                    lastFrameTime = std::chrono::steady_clock::now();
                }
            }
        }

        av_packet_unref(packet);
    }

    av_frame_free(&sw_frame);
    av_frame_free(&frame);
    av_packet_free(&packet);
}

bool VaapiDecoder::exportFrameDmaBuf(AVFrame* frame, DmaBufFrame& outFrame) {
    // Extract VASurfaceID from AVFrame
    VASurfaceID surface_id = (VASurfaceID)(uintptr_t)frame->data[3];

    // Get VADisplay from the hardware device context
    AVHWDeviceContext* device_ctx = (AVHWDeviceContext*)m_hwDeviceCtx->data;
    AVVAAPIDeviceContext* hwctx = (AVVAAPIDeviceContext*)device_ctx->hwctx;
    VADisplay va_display = hwctx->display;

    // Export VA surface as DMABUF FDs (order: export first, then sync — matching reference)
    VADRMPRIMESurfaceDescriptor desc = {};
    VAStatus status = vaExportSurfaceHandle(
        va_display, surface_id,
        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
        VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS,
        &desc);

    if (status != VA_STATUS_SUCCESS) {
        std::cerr << "vaExportSurfaceHandle failed: 0x" << std::hex << status << std::dec << std::endl;
        return false;
    }

    // Sync after export to ensure surface is ready
    VAStatus sync_st = vaSyncSurface(va_display, surface_id);
    if (sync_st != VA_STATUS_SUCCESS) {
        std::cerr << "vaSyncSurface failed: 0x" << std::hex << sync_st << std::dec << std::endl;
        // Close FDs on failure
        for (uint32_t i = 0; i < desc.num_objects; i++) {
            if (desc.objects[i].fd >= 0) ::close(desc.objects[i].fd);
        }
        return false;
    }

    // Validate we got usable layers (NV12 SEPARATE: 2 layers: R8 Y + GR88 UV)
    if (desc.num_layers < 2) {
        std::cerr << "vaExportSurfaceHandle returned " << desc.num_layers
                  << " layers, layer 0 planes=" << desc.layers[0].num_planes << std::endl;
        // Close any FDs that were opened
        for (uint32_t i = 0; i < desc.num_objects; i++) {
            if (desc.objects[i].fd >= 0) ::close(desc.objects[i].fd);
        }
        return false;
    }

    outFrame.width = frame->width;
    outFrame.height = frame->height;
    outFrame.pts = frame->pts;
    outFrame.time_base = m_formatContext->streams[m_videoStreamIndex]->time_base;
    outFrame.desc = desc;
    outFrame.valid = true;

    return true;
}

bool VaapiDecoder::exportFrameCpu(AVFrame* frame, VideoFrame& outFrame) {
    if (!frame || !frame->data[0] || !frame->data[1]) {
        std::cerr << "Invalid frame data pointers" << std::endl;
        return false;
    }

    outFrame.width = frame->width;
    outFrame.height = frame->height;
    outFrame.pts = frame->pts;
    outFrame.time_base = m_formatContext->streams[m_videoStreamIndex]->time_base;

    int y_size = frame->width * frame->height;
    int uv_width = frame->width / 2;
    int uv_height = frame->height / 2;
    int uv_size = uv_width * uv_height;
    outFrame.data.resize(y_size + uv_size * 2);

    int ls0 = frame->linesize[0] >= 0 ? frame->linesize[0] : -frame->linesize[0];
    int ls1 = frame->linesize[1] >= 0 ? frame->linesize[1] : -frame->linesize[1];

    // Copy Y plane (strip line padding for contiguous upload)
    uint8_t* dst = outFrame.data.data();
    for (int i = 0; i < frame->height; i++) {
        memcpy(dst + i * frame->width, frame->data[0] + i * ls0, frame->width);
    }

    bool is_nv12 = (frame->format == AV_PIX_FMT_NV12 || frame->format == AV_PIX_FMT_NV21);

    if (is_nv12) {
        // NV12: U and V are interleaved in plane 1 (UVUVUV...)
        uint8_t* dst_u = outFrame.data.data() + y_size;
        uint8_t* dst_v = outFrame.data.data() + y_size + uv_size;
        for (int i = 0; i < uv_height; i++) {
            uint8_t* src = frame->data[1] + i * ls1;
            for (int j = 0; j < uv_width; j++) {
                dst_u[i * uv_width + j] = src[j * 2];
                dst_v[i * uv_width + j] = src[j * 2 + 1];
            }
        }
    } else {
        // YUV420P: three separate planes
        if (!frame->data[2]) {
            std::cerr << "Missing V plane in non-NV12 format" << std::endl;
            return false;
        }
        int ls2 = frame->linesize[2] >= 0 ? frame->linesize[2] : -frame->linesize[2];

        // Copy U plane
        dst = outFrame.data.data() + y_size;
        for (int i = 0; i < uv_height; i++) {
            memcpy(dst + i * uv_width, frame->data[1] + i * ls1, uv_width);
        }

        // Copy V plane
        dst = outFrame.data.data() + y_size + uv_size;
        for (int i = 0; i < uv_height; i++) {
            memcpy(dst + i * uv_width, frame->data[2] + i * ls2, uv_width);
        }
    }

    return true;
}
