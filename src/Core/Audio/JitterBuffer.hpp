#pragma once

#include <QByteArray>
#include <QHash>

#include <cstdint>

namespace Acheron {
namespace Core {
namespace Audio {

class JitterBuffer
{
public:
    explicit JitterBuffer(int capacity = 10);

    /// Add a frame to the buffer.
    void push(uint16_t sequence, const QByteArray &data);

    /// Get the next frame in sequence order.
    /// Returns empty QByteArray if the frame is missing (packet loss).
    QByteArray pop();

    /// Inspect the next frame without consuming it. After pop() reports a
    /// missing packet this can expose packet N+1 for Opus in-band FEC recovery.
    [[nodiscard]] QByteArray peek() const;

    /// Reset the buffer state.
    void reset();

    /// Returns true once the buffer has pre-buffered enough frames for playback.
    [[nodiscard]] bool isReady() const { return initialized && !prebuffering; }
    /// True when the expected packet, or a newer packet proving a gap, is
    /// buffered. A timer firing before the next network packet is not loss.
    [[nodiscard]] bool hasPacketReady() const;
    [[nodiscard]] int targetDelayFrames() const { return targetDelay; }

private:
    /// Returns true if sequence a is "newer" than b (handles 16-bit wraparound).
    static bool seqNewer(uint16_t a, uint16_t b);

    QHash<uint16_t, QByteArray> frames;
    uint16_t nextSequence = 0;
    bool initialized = false;
    int capacity;

    static constexpr int MIN_TARGET_DELAY = 3;
    static constexpr int INITIAL_TARGET_DELAY = 3;
    static constexpr int MAX_TARGET_DELAY = 8;
    static constexpr int STABLE_HITS_TO_REDUCE = 1500;
    static constexpr int MAX_CONSECUTIVE_MISSES = 25;

    int targetDelay = INITIAL_TARGET_DELAY;
    bool prebuffering = true;
    int consecutiveMisses = 0;
    int consecutiveHits = 0;

};

} // namespace Audio
} // namespace Core
} // namespace Acheron
