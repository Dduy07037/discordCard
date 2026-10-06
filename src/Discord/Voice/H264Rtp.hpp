#pragma once

#include <QByteArray>
#include <QHash>
#include <optional>
#include <cstdint>

namespace Acheron::Discord::Voice {

// RFC 6184 non-interleaved mode: single NAL, STAP-A and FU-A. The output
// retains four-byte Annex B start codes required by DAVE authentication.
class H264Reassembler
{
public:
    std::optional<QByteArray> push(uint16_t sequence, uint32_t timestamp,
                                  bool marker, const QByteArray &payload, qint64 nowMs);
    void reset() { frames.clear(); lastCompleted.reset(); lastSequence.reset(); }
    int bufferedFrameCount() const { return int(frames.size()); }
    quint64 droppedFrames() const { return dropped; }
    static constexpr int MaxFrameBytes = 2 * 1024 * 1024;
    static constexpr int MaxFramePackets = 2048;
    static constexpr int MaxBufferedFrames = 3;
private:
    struct Frame {
        QHash<uint16_t, QByteArray> packets;
        std::optional<uint16_t> first;
        std::optional<uint16_t> last;
        qint64 createdAtMs = 0;
        int bytes = 0;
    };
    QHash<uint32_t, Frame> frames;
    std::optional<uint32_t> lastCompleted;
    std::optional<uint16_t> lastSequence;
    quint64 dropped = 0;
};

} // namespace Acheron::Discord::Voice
