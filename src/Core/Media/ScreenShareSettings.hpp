#pragma once
#include <QSize>

namespace Acheron::Core::Media {

// One immutable choice is used by both the encoder and the gateway metadata.
struct ScreenShareSettings {
    QSize resolution{1920, 1080};
    int bitrate = 12000000;
    static ScreenShareSettings forPreset(int preset, int fps)
    {
        if (preset == 0) return {{1280, 720}, fps > 30 ? 6000000 : 4000000};
        if (preset == 2) return {{1920, 1080}, fps > 30 ? 16000000 : 12000000};
        return {{1920, 1080}, fps > 30 ? 12000000 : 8000000};
    }
};

} // namespace Acheron::Core::Media
