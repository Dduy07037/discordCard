#pragma once

#include <QByteArray>
#include <QList>

#include <array>
#include <mutex>

namespace Acheron::Core::Audio {

struct AudioPacket {
    QByteArray data;
    qint64 capturedAtMs = 0;
    bool silence = false;
};

// Encoding and networking may stall independently. Keep media out of Qt's
// unbounded event queue; at most one drain notification is outstanding.
class AudioSendQueue
{
public:
    static constexpr int Capacity = 8;
    static constexpr qint64 MaxAgeMs = 100;

    bool push(const QByteArray &data, qint64 capturedAtMs, bool silence = false)
    {
        std::lock_guard lock(mutex);
        if (count == Capacity) {
            head = (head + 1) % Capacity;
            --count;
            ++dropped;
            gap = true;
        }
        packets[(head + count++) % Capacity] = {data, capturedAtMs, silence};
        const bool notify = !notified;
        notified = true;
        return notify;
    }

    QList<AudioPacket> take(qint64 nowMs, bool &discontinuity)
    {
        std::lock_guard lock(mutex);
        QList<AudioPacket> result;
        int latestSpeech = -1;
        for (int i = 0; i < count; ++i) {
            const auto &packet = packets[(head + i) % Capacity];
            if (!packet.silence && nowMs - packet.capturedAtMs <= MaxAgeMs)
                latestSpeech = i;
        }
        // Keep the freshest speech and its trailing silence. The five Opus
        // silence packets are a protocol boundary, not a speech backlog.
        for (int i = 0; i < count; ++i) {
            auto &packet = packets[(head + i) % Capacity];
            if (nowMs - packet.capturedAtMs > MaxAgeMs ||
                (latestSpeech >= 0 && i < latestSpeech)) {
                ++dropped;
                gap = true;
            } else {
                result.append(std::move(packet));
            }
            packet = {};
        }
        discontinuity = gap;
        gap = false;
        head = 0;
        count = 0;
        notified = false;
        return result;
    }

    quint64 droppedCount() const
    {
        std::lock_guard lock(mutex);
        return dropped;
    }

    void clear()
    {
        std::lock_guard lock(mutex);
        dropped += count;
        gap = gap || count != 0;
        for (auto &packet : packets)
            packet = {};
        head = count = 0;
        // Retain the pending notification: it still exists in the receiver's
        // event queue and will drain any trailing silence added after mute.
    }

private:
    mutable std::mutex mutex;
    std::array<AudioPacket, Capacity> packets;
    int head = 0;
    int count = 0;
    bool notified = false;
    bool gap = false;
    quint64 dropped = 0;
};

} // namespace Acheron::Core::Audio
