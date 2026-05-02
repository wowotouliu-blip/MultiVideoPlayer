#include "player_window.h"
#include "wayland_window.h"
#include "egl_renderer.h"
#include <iostream>
#include <vector>
#include <memory>
#include <csignal>
#include <chrono>
#include <thread>
#include <algorithm>
#include <filesystem>

static std::atomic<bool> g_running{true};
static const int   BORDER       = 4;
static const float BORDER_COLOR[] = {0.12f, 0.12f, 0.12f, 1.0f};

void signalHandler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        g_running = false;
    }
}

void printUsage(const char* programName) {
    std::cout << "Usage: " << programName << " <video1> [video2] [video3] ..." << std::endl;
    std::cout << std::endl;
    std::cout << "Multi-video player — all videos in a single window, grid layout" << std::endl;
    std::cout << "  zero-copy DMA-BUF rendering with CPU fallback." << std::endl;
}

static void computeGrid(int n, int& cols, int& rows) {
    if (n <= 1) { cols = 1; rows = 1; return; }
    if (n == 2) { cols = 2; rows = 1; return; }
    if (n <= 4) { cols = 2; rows = 2; return; }
    if (n <= 6) { cols = 3; rows = 2; return; }
    if (n <= 9) { cols = 3; rows = 3; return; }
    cols = 4;
    rows = (n + 3) / 4;
}

static void aspectFit(int videoW, int videoH, int cellW, int cellH,
                       int& outW, int& outH, int& outX, int& outY) {
    float vA = (float)videoW / (float)videoH;
    float cA = (float)cellW  / (float)cellH;
    if (vA > cA) {
        outW = cellW;
        outH = (int)(cellW / vA);
        outX = 0;
        outY = (cellH - outH) / 2;
    } else {
        outH = cellH;
        outW = (int)(cellH * vA);
        outX = (cellW - outW) / 2;
        outY = 0;
    }
}

static void drawBorders(int cols, int rows, int windowW, int windowH) {
    if (cols <= 1 && rows <= 1) return;

    glClearColor(BORDER_COLOR[0], BORDER_COLOR[1], BORDER_COLOR[2], BORDER_COLOR[3]);
    glEnable(GL_SCISSOR_TEST);

    int totalBorderW = (cols - 1) * BORDER;
    int totalBorderH = (rows - 1) * BORDER;
    int cellW = (windowW - totalBorderW) / cols;
    int cellH = (windowH - totalBorderH) / rows;

    // Vertical separators
    for (int c = 1; c < cols; c++) {
        int x = c * (cellW + BORDER) - BORDER;
        glScissor(x, 0, BORDER, windowH);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    // Horizontal separators
    for (int r = 1; r < rows; r++) {
        int y = windowH - r * (cellH + BORDER);
        glScissor(0, y, windowW, BORDER);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glDisable(GL_SCISSOR_TEST);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::vector<std::string> videoFiles;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage(argv[0]);
            return 0;
        }
        videoFiles.push_back(arg);
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // Phase 1: Init all decoders (serial, before any decode thread starts)
    std::vector<std::unique_ptr<PlayerWindow>> players;
    for (int i = 0; i < (int)videoFiles.size(); i++) {
        auto player = std::make_unique<PlayerWindow>(videoFiles[i], i);
        if (!player->init()) {
            std::cerr << "Failed to initialize player for: " << videoFiles[i] << std::endl;
            continue;
        }
        players.push_back(std::move(player));
    }

    if (players.empty()) {
        std::cerr << "No players could be initialized" << std::endl;
        return 1;
    }

    int N = static_cast<int>(players.size());

    // Compute grid and window size
    int cols, rows;
    computeGrid(N, cols, rows);

    int maxVW = 0, maxVH = 0;
    for (auto& p : players) {
        maxVW = std::max(maxVW, p->videoWidth() / 2);
        maxVH = std::max(maxVH, p->videoHeight() / 2);
    }
    if (maxVW < 320) maxVW = 320;
    if (maxVH < 240) maxVH = 240;

    int totalBorderW = (cols - 1) * BORDER;
    int totalBorderH = (rows - 1) * BORDER;
    int windowW = maxVW * cols + totalBorderW;
    int windowH = maxVH * rows + totalBorderH;
    if (windowW > 1920) {
        float scale = 1920.0f / (float)windowW;
        windowW = 1920;
        windowH = (int)(windowH * scale);
    }

    // Create single Wayland window + EGL renderer
    auto window = std::make_unique<WaylandWindow>("Multi Video Player", windowW, windowH);
    window->waitForConfigure();
    if (window->shouldClose()) {
        std::cerr << "Window closed before configure" << std::endl;
        return 1;
    }

    auto renderer = std::make_unique<EGLRenderer>();
    if (!renderer->init(window->display(), window->eglWindow())) {
        std::cerr << "Failed to initialize EGL renderer" << std::endl;
        return 1;
    }
    renderer->setViewport(window->width(), window->height());
    renderer->setPlayerCount(N);

    int actualW = window->width(), actualH = window->height();
    window->setResizeCallback([&](int w, int h) {
        actualW = w; actualH = h;
        renderer->setViewport(w, h);
    });

    renderer->releaseContext();

    // Phase 2: Start all decoders
    for (auto& player : players) {
        player->start();
    }

    std::cout << "Playing " << N << " video(s) in " << cols << "\u00D7" << rows
              << " grid — close window or Ctrl+C to exit" << std::endl;

    enum LastType { NONE, DMA, CPU };
    std::vector<LastType> playerLastType(N, NONE);

    // Render loop — always render at vsync, re-use textures when no new frame
    while (g_running && !window->shouldClose()) {
        renderer->beginFrame();

        int cellW = (actualW - totalBorderW) / cols;
        int cellH = (actualH - totalBorderH) / rows;

        for (int i = 0; i < N; i++) {
            int row = i / cols, col = i % cols;
            int cx = col * (cellW + BORDER);
            int cy = (rows - 1 - row) * (cellH + BORDER);

            // Clear cell interior to black
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glEnable(GL_SCISSOR_TEST);
            glScissor(cx, cy, cellW, cellH);
            glClear(GL_COLOR_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);

            // Aspect-fit: all three render paths use the same letterboxed viewport
            int vw = players[i]->videoWidth();
            int vh = players[i]->videoHeight();
            int dw = 0, dh = 0, dx = 0, dy = 0;
            aspectFit(vw, vh, cellW, cellH, dw, dh, dx, dy);

            DmaBufFrame dmaFrame;
            if (players[i]->getLatestFrame(dmaFrame) && dmaFrame.valid) {
                playerLastType[i] = DMA;
                renderer->renderDmaBufToRect(dmaFrame, i, cx + dx, cy + dy, dw, dh);
            } else {
                VideoFrame cpuFrame;
                if (players[i]->getLatestCpuFrame(cpuFrame)) {
                    playerLastType[i] = CPU;
                    renderer->renderCpuFrameToRect(cpuFrame, i, cx + dx, cy + dy, dw, dh);
                } else if (playerLastType[i] == DMA) {
                    renderer->renderDmaBufToRectPrev(i, cx + dx, cy + dy, dw, dh);
                } else if (playerLastType[i] == CPU) {
                    renderer->renderCpuFrameToRectPrev(i, cx + dx, cy + dy, dw, dh);
                }
                // else NONE: no frame ever received — cell stays black (cleared above)
            }
        }

        drawBorders(cols, rows, actualW, actualH);

        window->scheduleFrame();
        renderer->swapBuffers();
        window->awaitFrame();

        window->dispatchEvents();
        window->flush();
    }

    // Cleanup
    for (auto& p : players) {
        p->stop();
    }
    players.clear();

    if (renderer) renderer->cleanup();
    if (window) window->close();

    std::cout << "Exited successfully" << std::endl;
    return 0;
}
