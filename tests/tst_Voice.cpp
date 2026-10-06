#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Audio/CaptureQueue.hpp"
#include "Core/Audio/AudioSendQueue.hpp"
#include "Core/Audio/AudioPipeline.hpp"
#include "Core/Audio/JitterBuffer.hpp"
#include "Core/Audio/OpusDecoder.hpp"
#include "Core/Audio/OpusEncoder.hpp"
#include "Discord/Voice/VoiceEntities.hpp"

#include <QTest>
#include <QSignalSpy>

#include <dave/dave_interfaces.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <thread>

using namespace Acheron;
using namespace Acheron::Core::Audio;
using namespace Acheron::Discord::Voice;

class FakeAudioBackend : public IAudioBackend
{
public:
    QList<AudioDeviceInfo> availableInputDevices() const override { return {}; }
    QList<AudioDeviceInfo> availableOutputDevices() const override { return {}; }
    QByteArray currentInputDevice() const override { return {}; }
    QByteArray currentOutputDevice() const override { return {}; }
    void setInputDevice(const QByteArray &) override {}
    void setOutputDevice(const QByteArray &) override {}
    bool startCapture() override { capturing = true; return true; }
    void stopCapture() override { capturing = false; }
    bool startPlayback() override { playing = true; return true; }
    void stopPlayback() override { playing = false; }
    bool isCapturing() const override { return capturing; }
    bool isPlaying() const override { return playing; }
    int nativeCaptureChannels() const override { return 2; }
    void setInputGain(float) override {}
    void setOutputVolume(float) override {}
    int queuedPlaybackFrames() const override { return 0; }
    unsigned int takePlaybackUnderruns() override { return 0; }
    bool pushPlaybackFrame(const int16_t *) override { return true; }
    bool capturing = false;
    bool playing = false;
};

class TestVoice : public QObject
{
    Q_OBJECT
private slots:
    void resumeIncludesSequenceAck();
    void jitterBufferExposesRecoveryPacket();
    void jitterBufferHandlesSequenceWrap();
    void jitterBufferKeepsSafeDelayAfterShortStableRun();
    void jitterBufferRaisesDelayOncePerLossBurst();
    void jitterBufferWaitsForPacketBeforeDeclaringLoss();
    void opusFecDecodeProducesOneFrame();
    void davePassthroughIsExplicitAndBounded();
    void captureQueuePreservesSamplesAndCaptureTime();
    void captureQueueBoundsOverflowAndRestarts();
    void captureQueueConcurrentProducerConsumer();
    void sendQueueCoalescesWakeupsAndDropsBacklog();
    void sendQueuePreservesTrailingSilence();
    void jitterBufferBoundsStalledReceiver();
    void jitterBufferResyncsAfterRtpSilence();
    void jitterBufferAcceptsRtpTimestampWrap();
    void pipelinePreservesCaptureTimestampAndRejectsStalePcm();
    void sendQueueBoundsOverflow();
};

void TestVoice::pipelinePreservesCaptureTimestampAndRejectsStalePcm()
{
    FakeAudioBackend backend;
    AudioPipeline pipeline;
    auto queue = std::make_shared<AudioSendQueue>();
    pipeline.setSendQueue(queue);
    pipeline.setNoiseSuppressionEnabled(false);
    pipeline.setUseRnnoiseVad(false);
    pipeline.setVadThreshold(0);
    pipeline.start(&backend, true);
    QSignalSpy available(&pipeline, &AudioPipeline::audioPacketsAvailable);
    QByteArray pcm(AUDIO_FRAME_SIZE, '\0');
    auto *samples = reinterpret_cast<int16_t *>(pcm.data());
    std::fill(samples, samples + AUDIO_FRAME_SIZE / sizeof(int16_t), 1000);
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    emit backend.audioCaptured(pcm, now - 150);
    QCOMPARE(available.size(), 0);
    emit backend.audioCaptured(pcm, now - 30);
    QCOMPARE(available.size(), 1);
    bool gap = false;
    const auto packets = queue->take(now, gap);
    QCOMPARE(packets.size(), 1);
    QCOMPARE(packets.first().capturedAtMs, qint64(now - 30));
    QVERIFY(!packets.first().data.isEmpty());
    // Muting must remove unsent microphone data and preserve the five-packet
    // silence boundary, even with a drain notification already pending.
    emit backend.audioCaptured(pcm, now - 20);
    pipeline.stopCapture();
    const auto silence = queue->take(now + 20, gap);
    QCOMPARE(silence.size(), 5);
    for (const auto &packet : silence)
        QVERIFY(packet.silence);
    pipeline.stop();
    QVERIFY(!backend.capturing);
    QVERIFY(!backend.playing);
    emit backend.audioCaptured(pcm, now);
    QCOMPARE(available.size(), 2);
}

void TestVoice::sendQueueBoundsOverflow()
{
    AudioSendQueue queue;
    unsigned wakeups = 0;
    for (int i = 0; i < 1000; ++i)
        wakeups += queue.push(QByteArray::number(i), 1000);
    QCOMPARE(wakeups, 1u);
    bool gap = false;
    const auto packets = queue.take(1020, gap);
    QCOMPARE(packets.size(), 1);
    QCOMPARE(packets.first().data, QByteArray("999"));
    QCOMPARE(queue.droppedCount(), quint64(999));
    QVERIFY(gap);
}

void TestVoice::captureQueuePreservesSamplesAndCaptureTime()
{
    CaptureQueue queue;
    std::array<int16_t, 1920> input;
    for (unsigned i = 0; i < input.size(); ++i)
        input[i] = int16_t(i);
    // Device callbacks need not line up with the 20 ms Opus boundary.
    queue.append(input.data(), 240, 1005000);
    queue.append(input.data() + 480, 720, 1020000);
    CaptureQueue::Frame frame;
    QVERIFY(queue.pop(frame));
    QCOMPARE(frame.capturedAtUs, int64_t(1000000));
    QVERIFY(frame.samples == input);
    QVERIFY(!queue.pop(frame));
}

void TestVoice::captureQueueBoundsOverflowAndRestarts()
{
    CaptureQueue queue;
    std::array<int16_t, 1920> input{};
    for (unsigned i = 0; i < CaptureQueue::Capacity + 10; ++i)
        queue.append(input.data(), 960, 1000000 + i * 20000);
    QCOMPARE(queue.overflowCount(), uint64_t(10));
    CaptureQueue::Frame frame;
    unsigned consumed = 0;
    while (queue.pop(frame))
        ++consumed;
    QCOMPARE(consumed, CaptureQueue::Capacity);
    queue.reset();
    queue.append(input.data(), 960, 3000000);
    QVERIFY(queue.pop(frame));
    QCOMPARE(frame.capturedAtUs, int64_t(2980000));
    QCOMPARE(queue.overflowCount(), uint64_t(0));
}

void TestVoice::captureQueueConcurrentProducerConsumer()
{
    CaptureQueue queue;
    std::atomic<bool> done{false};
    std::thread producer([&] {
        std::array<int16_t, 1920> input;
        for (uint32_t n = 1; n <= 10000; ++n) {
            for (unsigned i = 0; i < input.size(); i += 2) {
                input[i] = int16_t(n & 0xffff);
                input[i + 1] = int16_t(n >> 16);
            }
            queue.append(input.data(), 960, int64_t(n) * 20000);
        }
        done.store(true, std::memory_order_release);
    });
    bool consistent = true;
    uint32_t previous = 0;
    unsigned consumed = 0;
    CaptureQueue::Frame frame;
    for (;;) {
        if (!queue.pop(frame)) {
            if (!done.load(std::memory_order_acquire))
                continue;
            if (!queue.pop(frame))
                break;
        }
        const uint32_t n = uint16_t(frame.samples[0]) | (uint32_t(uint16_t(frame.samples[1])) << 16);
        consistent &= n > previous && frame.capturedAtUs == int64_t(n - 1) * 20000;
        for (unsigned i = 0; i < frame.samples.size(); i += 2)
            consistent &= frame.samples[i] == frame.samples[0] && frame.samples[i + 1] == frame.samples[1];
        previous = n;
        ++consumed;
    }
    producer.join();
    QVERIFY(consistent);
    QVERIFY(consumed > 0);
    QCOMPARE(uint64_t(consumed) + queue.overflowCount(), uint64_t(10000));
}

void TestVoice::sendQueueCoalescesWakeupsAndDropsBacklog()
{
    AudioSendQueue queue;
    QVERIFY(queue.push("old", 1000));
    QVERIFY(!queue.push("middle", 1120));
    QVERIFY(!queue.push("fresh", 1140));
    bool gap = false;
    const auto batch = queue.take(1160, gap);
    QCOMPARE(batch.size(), 1);
    QCOMPARE(batch.first().data, QByteArray("fresh"));
    QCOMPARE(batch.first().capturedAtMs, qint64(1140));
    QVERIFY(gap);
    QCOMPARE(queue.droppedCount(), quint64(2));
    QVERIFY(queue.push("next", 1180));
    QCOMPARE(queue.take(1200, gap).size(), 1);
    QVERIFY(!gap);
}

void TestVoice::sendQueuePreservesTrailingSilence()
{
    AudioSendQueue queue;
    queue.push("speech", 1000);
    for (int i = 0; i < 5; ++i)
        queue.push("silence", 1001, true);
    bool gap = false;
    const auto batch = queue.take(1020, gap);
    QCOMPARE(batch.size(), 6);
    QCOMPARE(batch.first().data, QByteArray("speech"));
    for (int i = 1; i < batch.size(); ++i)
        QVERIFY(batch[i].silence);
    QVERIFY(!gap);
}

void TestVoice::jitterBufferBoundsStalledReceiver()
{
    JitterBuffer buffer;
    for (uint16_t i = 0; i < 100; ++i) {
        buffer.push(i, QByteArray::number(i));
        QVERIFY(buffer.bufferedPacketCount() <= 10);
    }
    QVERIFY(buffer.isReady());
    QCOMPARE(buffer.pop(), QByteArray("90"));
}

void TestVoice::jitterBufferResyncsAfterRtpSilence()
{
    JitterBuffer buffer;
    buffer.push(10, 1000, "old-a");
    buffer.push(11, 1960, "old-b");
    buffer.push(12, 2920, "old-c");
    buffer.push(13, 48000, "new-a");
    QVERIFY(!buffer.isReady());
    buffer.push(14, 48960, "new-b");
    buffer.push(15, 49920, "new-c");
    QVERIFY(buffer.isReady());
    QCOMPARE(buffer.pop(), QByteArray("new-a"));
}

void TestVoice::jitterBufferAcceptsRtpTimestampWrap()
{
    JitterBuffer buffer;
    buffer.push(65534, UINT32_MAX - 959, "a");
    buffer.push(65535, 0, "b");
    buffer.push(0, 960, "c");
    QCOMPARE(buffer.pop(), QByteArray("a"));
    QCOMPARE(buffer.pop(), QByteArray("b"));
    QCOMPARE(buffer.pop(), QByteArray("c"));
}

void TestVoice::resumeIncludesSequenceAck()
{
    VoiceResumeData data;
    data.serverId = Core::Snowflake(123456789ULL);
    data.sessionId = QStringLiteral("session");
    data.token = QStringLiteral("secret");
    data.seqAck = 42;

    const QJsonObject json = data.toJson();
    QCOMPARE(json.value("server_id").toString(), QStringLiteral("123456789"));
    QCOMPARE(json.value("session_id").toString(), QStringLiteral("session"));
    QCOMPARE(json.value("seq_ack").toInt(), 42);
}

void TestVoice::jitterBufferExposesRecoveryPacket()
{
    JitterBuffer buffer;
    buffer.push(100, QByteArrayLiteral("packet-100"));
    buffer.push(102, QByteArrayLiteral("packet-102"));
    buffer.push(103, QByteArrayLiteral("packet-103"));

    QVERIFY(buffer.isReady());
    QCOMPARE(buffer.pop(), QByteArrayLiteral("packet-100"));
    QVERIFY(buffer.pop().isEmpty());
    QCOMPARE(buffer.peek(), QByteArrayLiteral("packet-102"));
    QCOMPARE(buffer.pop(), QByteArrayLiteral("packet-102"));
}

void TestVoice::jitterBufferHandlesSequenceWrap()
{
    JitterBuffer buffer;
    buffer.push(65534, QByteArrayLiteral("a"));
    buffer.push(65535, QByteArrayLiteral("b"));
    buffer.push(0, QByteArrayLiteral("c"));

    QVERIFY(buffer.isReady());
    QCOMPARE(buffer.pop(), QByteArrayLiteral("a"));
    QCOMPARE(buffer.pop(), QByteArrayLiteral("b"));
    QCOMPARE(buffer.pop(), QByteArrayLiteral("c"));
}

void TestVoice::jitterBufferKeepsSafeDelayAfterShortStableRun()
{
    JitterBuffer buffer;
    buffer.push(1, QByteArrayLiteral("1"));
    buffer.push(2, QByteArrayLiteral("2"));
    buffer.push(3, QByteArrayLiteral("3"));

    for (uint16_t sequence = 1; sequence <= 250; ++sequence) {
        QCOMPARE(buffer.pop(), QByteArray::number(sequence));
        buffer.push(static_cast<uint16_t>(sequence + 3),
                    QByteArray::number(sequence + 3));
    }

    QCOMPARE(buffer.targetDelayFrames(), 3);
}

void TestVoice::jitterBufferRaisesDelayOncePerLossBurst()
{
    JitterBuffer buffer;
    buffer.push(100, QByteArrayLiteral("100"));
    buffer.push(101, QByteArrayLiteral("101"));
    buffer.push(102, QByteArrayLiteral("102"));

    QCOMPARE(buffer.pop(), QByteArrayLiteral("100"));
    QCOMPARE(buffer.pop(), QByteArrayLiteral("101"));
    QCOMPARE(buffer.pop(), QByteArrayLiteral("102"));
    QVERIFY(buffer.pop().isEmpty());
    QVERIFY(buffer.pop().isEmpty());
    QVERIFY(buffer.pop().isEmpty());
    QCOMPARE(buffer.targetDelayFrames(), 4);
    QVERIFY(!buffer.isReady());

    buffer.push(106, QByteArrayLiteral("106"));
    buffer.push(107, QByteArrayLiteral("107"));
    buffer.push(108, QByteArrayLiteral("108"));
    buffer.push(109, QByteArrayLiteral("109"));
    QVERIFY(buffer.isReady());
    QCOMPARE(buffer.pop(), QByteArrayLiteral("106"));
    QCOMPARE(buffer.targetDelayFrames(), 4);
}

void TestVoice::jitterBufferWaitsForPacketBeforeDeclaringLoss()
{
    JitterBuffer buffer;
    buffer.push(10, QByteArrayLiteral("10"));
    buffer.push(11, QByteArrayLiteral("11"));
    buffer.push(12, QByteArrayLiteral("12"));

    QVERIFY(buffer.hasPacketReady());
    QCOMPARE(buffer.pop(), QByteArrayLiteral("10"));
    QCOMPARE(buffer.pop(), QByteArrayLiteral("11"));
    QCOMPARE(buffer.pop(), QByteArrayLiteral("12"));

    // A mixer tick may run just before packet 13 arrives. That must not be
    // interpreted as loss or advance the expected sequence.
    QVERIFY(!buffer.hasPacketReady());
    buffer.push(13, QByteArrayLiteral("13"));
    QVERIFY(buffer.hasPacketReady());
    QCOMPARE(buffer.pop(), QByteArrayLiteral("13"));
    QCOMPARE(buffer.targetDelayFrames(), 3);
}

void TestVoice::opusFecDecodeProducesOneFrame()
{
    Core::Audio::OpusEncoder encoder;
    QVERIFY(encoder.init(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS));
    encoder.setFec(true);
    encoder.setPacketLossPercent(10);

    auto makeFrame = [](double phase) {
        QByteArray pcm(AUDIO_FRAME_SIZE, '\0');
        auto *samples = reinterpret_cast<int16_t *>(pcm.data());
        for (int i = 0; i < AUDIO_FRAME_SAMPLES; ++i) {
            const double angle = phase + (2.0 * 3.14159265358979323846 * 440.0 * i / AUDIO_SAMPLE_RATE);
            const int16_t sample = static_cast<int16_t>(std::sin(angle) * 12000.0);
            samples[i * 2] = sample;
            samples[i * 2 + 1] = sample;
        }
        return pcm;
    };

    const QByteArray first = encoder.encode(makeFrame(0.0));
    const QByteArray second = encoder.encode(makeFrame(0.25));
    QVERIFY(!first.isEmpty());
    QVERIFY(!second.isEmpty());

    Core::Audio::OpusDecoder decoder;
    QVERIFY(decoder.init(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS));
    const QByteArray recovered = decoder.decodeFec(second);
    QCOMPARE(recovered.size(), AUDIO_FRAME_SIZE);
}

void TestVoice::davePassthroughIsExplicitAndBounded()
{
    using namespace std::chrono_literals;

    auto decryptor = discord::dave::CreateDecryptor();
    const std::array<uint8_t, 4> plaintext = { 0x01, 0x02, 0x03, 0x04 };
    std::array<uint8_t, 16> output = {};
    size_t bytesWritten = 0;

    auto decrypt = [&] {
        bytesWritten = 0;
        return decryptor->Decrypt(
                discord::dave::MediaType::Audio,
                discord::dave::ArrayView<const uint8_t>(plaintext.data(), plaintext.size()),
                discord::dave::ArrayView<uint8_t>(output.data(), output.size()),
                &bytesWritten);
    };

    QCOMPARE(decrypt(), discord::dave::IDecryptor::DecryptionFailure);

    decryptor->TransitionToPassthroughMode(true);
    QCOMPARE(decrypt(), discord::dave::IDecryptor::Success);
    QCOMPARE(bytesWritten, plaintext.size());
    QVERIFY(std::equal(plaintext.begin(), plaintext.end(), output.begin()));

    decryptor->TransitionToPassthroughMode(false, 0s);
    QCOMPARE(decrypt(), discord::dave::IDecryptor::DecryptionFailure);
}

QTEST_MAIN(TestVoice)
#include "tst_Voice.moc"
