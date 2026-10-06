#pragma once

#include <QImage>
#include <chrono>
#include <mutex>
#include <optional>

namespace Acheron::Core::Media {

inline qint64 videoClockMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// GUI capture and video encoding share one replaceable frame, never a queue of
// Qt events. An encoder stall cannot build an unbounded screen-capture backlog.
class LatestVideoFrame
{
public:
    struct Frame { QImage image; qint64 capturedAtMs; };
    void push(QImage image, qint64 capturedAtMs)
    {
        std::lock_guard lock(mutex);
        frame = Frame{std::move(image), capturedAtMs};
    }
    std::optional<Frame> take(qint64 nowMs, qint64 maxAgeMs = 150)
    {
        std::lock_guard lock(mutex);
        auto result = std::move(frame);
        frame.reset();
        if (result && (result->image.isNull() || nowMs < result->capturedAtMs
                       || nowMs - result->capturedAtMs > maxAgeMs))
            result.reset();
        return result;
    }
    void clear()
    {
        std::lock_guard lock(mutex);
        frame.reset();
    }
private:
    std::mutex mutex;
    std::optional<Frame> frame;
};
} // namespace Acheron::Core::Media
