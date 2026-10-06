#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Audio/JitterBuffer.hpp"
#include "Core/Audio/OpusDecoder.hpp"
#include "Core/Audio/OpusEncoder.hpp"
#include "Discord/Voice/VoiceEntities.hpp"

#include <QTest>

#include <dave/dave_interfaces.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>

using namespace Acheron;
using namespace Acheron::Core::Audio;
using namespace Acheron::Discord::Voice;

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
};

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
