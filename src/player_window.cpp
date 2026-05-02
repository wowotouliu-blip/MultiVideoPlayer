#include "player_window.h"
#include <iostream>
#include <filesystem>

PlayerWindow::PlayerWindow(const std::string& filename, int index)
    : m_filename(filename), m_index(index)
{
}

PlayerWindow::~PlayerWindow() {
    stop();
}

bool PlayerWindow::init() {
    m_decoder = std::make_unique<VaapiDecoder>();
    if (!m_decoder->open(m_filename)) {
        std::cerr << "Failed to open video: " << m_filename << std::endl;
        return false;
    }

    m_width = m_decoder->width();
    m_height = m_decoder->height();
    if (m_width <= 0 || m_height <= 0) {
        m_width = 640;
        m_height = 480;
    }

    std::string title = "Player " + std::to_string(m_index) + ": " +
                        std::filesystem::path(m_filename).filename().string();
    std::cout << title << " initialized" << std::endl;
    return true;
}

void PlayerWindow::onDmaBufFrameReceived(DmaBufFrame&& frame) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_latestDmaFrame = std::move(frame);
    m_dmaFrameReady = true;
}

void PlayerWindow::onCpuFrameReceived(const VideoFrame& frame) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_latestCpuFrame = frame;
    m_cpuFrameReady = true;
}

void PlayerWindow::start() {
    if (!m_decoder) return;
    m_running = true;
    m_decoder->startAll(
        [this](DmaBufFrame&& frame) {
            onDmaBufFrameReceived(std::move(frame));
        },
        [this](const VideoFrame& frame) {
            onCpuFrameReceived(frame);
        }
    );
}

void PlayerWindow::stop() {
    m_running = false;
    if (m_decoder) {
        m_decoder->stop();
    }
}

bool PlayerWindow::getLatestFrame(DmaBufFrame& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_dmaFrameReady) return false;
    out = std::move(m_latestDmaFrame);
    m_dmaFrameReady = false;
    return true;
}

bool PlayerWindow::getLatestCpuFrame(VideoFrame& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_cpuFrameReady) return false;
    out = std::move(m_latestCpuFrame);
    m_cpuFrameReady = false;
    return true;
}
