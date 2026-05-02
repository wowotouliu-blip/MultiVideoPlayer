#ifndef PLAYER_WINDOW_H
#define PLAYER_WINDOW_H

#include "vaapi_decoder.h"
#include <string>
#include <memory>
#include <mutex>
#include <atomic>
#include <thread>

class PlayerWindow {
public:
    PlayerWindow(const std::string& filename, int index);
    ~PlayerWindow();

    bool init();
    void start();              // Start decode thread with both DMA and CPU callbacks
    void stop();
    bool isRunning() const { return m_running; }

    bool getLatestFrame(DmaBufFrame& out);       // DMA-BUF zero-copy frame
    bool getLatestCpuFrame(VideoFrame& out);     // CPU fallback frame
    int videoWidth() const { return m_width; }
    int videoHeight() const { return m_height; }
    int index() const { return m_index; }

private:
    void onDmaBufFrameReceived(DmaBufFrame&& frame);
    void onCpuFrameReceived(const VideoFrame& frame);

    std::string m_filename;
    int m_index;
    int m_width = 0;
    int m_height = 0;

    std::unique_ptr<VaapiDecoder> m_decoder;

    std::mutex m_mutex;
    DmaBufFrame m_latestDmaFrame;
    VideoFrame m_latestCpuFrame;
    bool m_dmaFrameReady = false;
    bool m_cpuFrameReady = false;

    std::atomic<bool> m_running{false};
};

#endif // PLAYER_WINDOW_H
