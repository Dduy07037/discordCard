#pragma once
#include <QSize>

namespace Acheron::Core::Media {

// Encoder and gateway metadata use the same profile, including Auto adjustments.
struct ScreenShareSettings {
    QSize resolution{1920, 1080};
    int bitrate = 6000000;
    bool automatic = false;
    static ScreenShareSettings forPreset(int preset, int fps)
    {
        if (preset == 3) return {{1920, 1080}, fps > 30 ? 8000000 : 6000000, true};
        if (preset == 0) return {{1280, 720}, fps > 30 ? 6000000 : 4000000};
        if (preset == 2) return {{1920, 1080}, fps > 30 ? 16000000 : 12000000};
        return {{1920, 1080}, fps > 30 ? 12000000 : 8000000};
    }
};

} // namespace Acheron::Core::Media
