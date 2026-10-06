#include "JitterBuffer.hpp"

#include <algorithm>

namespace Acheron {
namespace Core {
namespace Audio {

bool JitterBuffer::seqNewer(uint16_t a, uint16_t b)
{
    return static_cast<int16_t>(a - b) > 0;
}

JitterBuffer::JitterBuffer(int capacity)
    : capacity(std::clamp(capacity, MAX_TARGET_DELAY, 100))
{
}

void JitterBuffer::push(uint16_t sequence, const QByteArray &data)
{
    if (!initialized) {
        nextSequence = sequence;
        initialized = true;
    }

    // If the sequence is way ahead of what we expect, the speaker resumed
    // after a long silence. Reset and resync.
    if (seqNewer(sequence, static_cast<uint16_t>(nextSequence + capacity * 10))) {
        reset();
        nextSequence = sequence;
        initialized = true;
    }

    // Discard if too old (already played or beyond buffer capacity)
    if (seqNewer(nextSequence, sequence))
        return;

    if (data.isEmpty() || frames.contains(sequence))
        return;
    if (frames.size() >= capacity) {
        // Do not retain a growing delayed talkspurt after the worker stalls.
        reset();
        nextSequence = sequence;
        initialized = true;
    }
    frames.insert(sequence, data);

    // Evict frames that are too far behind the playback pointer
    QList<uint16_t> stale;
    for (auto it = frames.begin(); it != frames.end(); ++it) {
        if (seqNewer(nextSequence, static_cast<uint16_t>(it.key() + capacity)))
            stale.append(it.key());
    }
    for (uint16_t seq : stale)
        frames.remove(seq);

    if (prebuffering && frames.size() >= targetDelay)
        prebuffering = false;
}

void JitterBuffer::push(uint16_t sequence, uint32_t timestamp, const QByteArray &data)
{
    if (data.isEmpty())
        return;
    if (hasTimestamp && seqNewer(sequence, lastReceivedSequence)) {
        const auto sequenceDistance = static_cast<uint16_t>(sequence - lastReceivedSequence);
        const auto timestampDistance = static_cast<int32_t>(timestamp - lastReceivedTimestamp);
        // RTP time keeps advancing during intentional silence, while sequence
        // numbers count sent packets. Do not play an old talkspurt into a new
        // one or mistake that silence for a long series of lost packets.
        if (timestampDistance > int32_t(sequenceDistance) * 960 + 1920)
            reset();
    }
    const bool newer = !hasTimestamp || seqNewer(sequence, lastReceivedSequence);
    push(sequence, data);
    if (newer && !data.isEmpty()) {
        hasTimestamp = true;
        lastReceivedSequence = sequence;
        lastReceivedTimestamp = timestamp;
    }
}

QByteArray JitterBuffer::pop()
{
    if (!initialized)
        return {};

    if (prebuffering)
        return {};

    QByteArray data = frames.take(nextSequence);
    nextSequence++;

    if (!data.isEmpty()) {
        consecutiveHits++;
        consecutiveMisses = 0;

        // Do not collapse to a fragile 40 ms network buffer after only a few
        // seconds of clean traffic. Reduce one frame only after 30 seconds of
        // uninterrupted audio and retain a 60 ms floor.
        if (consecutiveHits >= STABLE_HITS_TO_REDUCE && targetDelay > MIN_TARGET_DELAY) {
            targetDelay--;
            consecutiveHits = 0;
        }
    } else {
        consecutiveMisses++;
        consecutiveHits = 0;

        // Rebuffer once for a burst rather than repeating the pause for every
        // subsequent missing sequence. PLC continues advancing the sequence
        // pointer, and a long outage still reaches the hard resync below.
        if (consecutiveMisses == 3) {
            if (targetDelay < MAX_TARGET_DELAY)
                targetDelay++;
            prebuffering = true;
        }

        if (consecutiveMisses >= MAX_CONSECUTIVE_MISSES) {
            reset();
        }
    }

    return data;
}

QByteArray JitterBuffer::peek() const
{
    if (!initialized || prebuffering)
        return {};
    return frames.value(nextSequence);
}

bool JitterBuffer::hasPacketReady() const
{
    if (!isReady() || frames.isEmpty())
        return false;

    if (frames.contains(nextSequence))
        return true;

    // A newer sequence confirms that the expected packet is missing and lets
    // the decoder recover it using FEC/PLC. With no buffered packet at all we
    // simply wait; otherwise a slightly early mixer tick would invent loss.
    for (auto it = frames.constBegin(); it != frames.constEnd(); ++it)
        if (seqNewer(it.key(), nextSequence))
            return true;
    return false;
}

void JitterBuffer::reset()
{
    frames.clear();
    nextSequence = 0;
    initialized = false;
    targetDelay = INITIAL_TARGET_DELAY;
    prebuffering = true;
    consecutiveMisses = 0;
    consecutiveHits = 0;
    hasTimestamp = false;
}

} // namespace Audio
} // namespace Core
} // namespace Acheron
