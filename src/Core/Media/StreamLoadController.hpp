#pragma once
#include "ScreenShareSettings.hpp"
#include <algorithm>

namespace Acheron::Core::Media {

// CPU/sender scheduling adaptation, not an estimate of remote network capacity.
// Auto only steps down during a session, avoiding repeated resize/FPS oscillation.
class StreamLoadController {
public:
    StreamLoadController(ScreenShareSettings settings, int fps) : settings(settings), fps(fps) {}
    bool observe(double encodeMs, double cycleMs, bool delivered, qint64 nowMs, bool keyframe)
    {
        if (!settings.automatic || (keyframe && delivered)) return false;
        if (delivered) {
            averageEncodeMs = sampled ? averageEncodeMs * 0.8 + encodeMs * 0.2 : encodeMs;
            averageCycleMs = sampled ? averageCycleMs * 0.8 + cycleMs * 0.2 : cycleMs;
            sampled = true;
        }
        const double period = 1000.0 / fps;
        const bool overloaded = !delivered || averageEncodeMs > period * 0.8 || averageCycleMs > period * 1.1;
        slowFrames = overloaded ? slowFrames + 1 : 0;
        if (slowFrames < 6 || nowMs - lastChangeMs < 4000) return false;
        slowFrames = 0;
        lastChangeMs = nowMs;
        const int oldFps = fps;
        if (fps > 30) fps = 30;
        else if (fps > 20) fps = 20;
        else if (fps > 15) fps = 15;
        else if (settings.resolution.width() > 1280) {
            settings.resolution = QSize(1280, 720);
            settings.bitrate = std::max(2000000, settings.bitrate * 4 / 9);
            sampled = false;
            return true;
        } else return false;
        settings.bitrate = std::max(2000000, settings.bitrate * fps / oldFps);
        sampled = false;
        return true;
    }
    ScreenShareSettings settings;
    int fps;
    double averageEncodeMs = 0;
private:
    double averageCycleMs = 0;
    bool sampled = false;
    int slowFrames = 0;
    qint64 lastChangeMs = 0;
};
} // namespace Acheron::Core::Media
