#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <optional>
#include <cstdint>

namespace Acheron::Discord::Voice {

QList<QByteArray> packetizeVp8(const QByteArray &frame, int payloadBudget = 1000);

// Frame E2EE must run after reassembly, never on individual RTP fragments.
class Vp8Reassembler
{
public:
    std::optional<QByteArray> push(uint16_t sequence, uint32_t timestamp,
                                  bool marker, const QByteArray &payload, qint64 nowMs);
    void reset() { frames.clear(); completed.clear(); lastCompleted.reset(); }
    quint64 droppedFrames() const { return dropped; }
    int bufferedFrameCount() const { return int(frames.size()); }
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
    QList<uint32_t> completed;
    std::optional<uint32_t> lastCompleted;
    quint64 dropped = 0;
};

} // namespace Acheron::Discord::Voice
