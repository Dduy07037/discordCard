#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

namespace Acheron::Core::Audio {

// One device callback produces; one audio worker consumes. Neither side writes
// the other side's cursor. reset() requires both sides to be stopped.
class CaptureQueue
{
public:
    static constexpr unsigned SampleRate = 48000;
    static constexpr unsigned Channels = 2;
    static constexpr unsigned SamplesPerFrame = 960;
    static constexpr unsigned Capacity = 6;
    struct Frame {
        std::array<int16_t, SamplesPerFrame * Channels> samples{};
        int64_t capturedAtUs = 0; // first sample, steady_clock domain
    };

    void append(const int16_t *input, unsigned sampleFrames, int64_t callbackEndUs)
    {
        unsigned consumed = 0;
        while (consumed < sampleFrames) {
            if (partialSamples == 0)
                partial.capturedAtUs = callbackEndUs -
                    int64_t(sampleFrames - consumed) * 1000000 / SampleRate;
            const unsigned count = std::min(SamplesPerFrame - partialSamples,
                                             sampleFrames - consumed);
            std::memcpy(partial.samples.data() + partialSamples * Channels,
                        input + consumed * Channels, count * Channels * sizeof(int16_t));
            partialSamples += count;
            consumed += count;
            if (partialSamples != SamplesPerFrame)
                continue;
            const auto write = writeIndex.load(std::memory_order_relaxed);
            if (write - readIndex.load(std::memory_order_acquire) < Capacity) {
                frames[write % Capacity] = partial;
                writeIndex.store(write + 1, std::memory_order_release);
            } else {
                overflows.fetch_add(1, std::memory_order_relaxed);
            }
            partialSamples = 0;
        }
    }

    bool pop(Frame &frame)
    {
        const auto read = readIndex.load(std::memory_order_relaxed);
        if (read == writeIndex.load(std::memory_order_acquire))
            return false;
        frame = frames[read % Capacity];
        readIndex.store(read + 1, std::memory_order_release);
        return true;
    }

    uint64_t overflowCount() const { return overflows.load(std::memory_order_relaxed); }
    void reset()
    {
        readIndex.store(0, std::memory_order_relaxed);
        writeIndex.store(0, std::memory_order_relaxed);
        overflows.store(0, std::memory_order_relaxed);
        partialSamples = 0;
    }

private:
    std::array<Frame, Capacity> frames{};
    Frame partial;
    unsigned partialSamples = 0;
    alignas(64) std::atomic<uint64_t> writeIndex{0};
    alignas(64) std::atomic<uint64_t> readIndex{0};
    std::atomic<uint64_t> overflows{0};
};

} // namespace Acheron::Core::Audio
