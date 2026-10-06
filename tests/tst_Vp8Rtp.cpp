#include <QTest>
#include <dave/dave_interfaces.h>
#include <bytes/bytes.h>
#include "Discord/Voice/Vp8Rtp.hpp"
#include "Core/Media/LatestVideoFrame.hpp"
#ifdef TEST_VP8_CODEC
#include "Core/Media/RealtimeVp8.hpp"
#endif
using namespace Acheron::Discord::Voice;

class TestVp8Rtp : public QObject
{
    Q_OBJECT
private slots:
    void daveBeforeFragmentation();
    void outOfOrderAndSequenceWrap();
    void discardsLateFramesAndHandlesTimestampWrap();
    void missingPacketAndDuplicates();
    void descriptorExtensions();
    void boundsAndExpiry();
    void conflictingBoundaries();
    void newestScreenFrameAndExpiry();
#ifdef TEST_VP8_CODEC
    void realtimeCodecRoundtrip();
    void desktopCodecKeepsAspectRatio();
#endif
};

void TestVp8Rtp::newestScreenFrameAndExpiry()
{
    Acheron::Core::Media::LatestVideoFrame queue;
    for (int i = 0; i < 1000; ++i) {
        QImage image(2, 2, QImage::Format_RGB32);
        image.fill(QColor(i % 256, 0, 0));
        queue.push(image, i);
    }
    const auto newest = queue.take(1000);
    QVERIFY(newest);
    QCOMPARE(newest->capturedAtMs, qint64(999));
    QCOMPARE(newest->image.pixelColor(0, 0).red(), 999 % 256);
    QVERIFY(!queue.take(1000));
    queue.push(QImage(2, 2, QImage::Format_RGB32), 1000);
    QVERIFY(!queue.take(1151));
    queue.push(QImage(2, 2, QImage::Format_RGB32), 2000);
    QVERIFY(!queue.take(1999));
    queue.push(QImage(2, 2, QImage::Format_RGB32), 3000);
    queue.clear();
    QVERIFY(!queue.take(3000));
}

void TestVp8Rtp::daveBeforeFragmentation()
{
    struct TestRatchet : discord::dave::IKeyRatchet {
        discord::dave::EncryptionKey GetKey(discord::dave::KeyGeneration generation) noexcept override
        { return discord::dave::EncryptionKey(std::vector<uint8_t>(16, uint8_t(42 + generation))); }
        void DeleteKey(discord::dave::KeyGeneration) noexcept override {}
    };
    auto encryptor = discord::dave::CreateEncryptor();
    auto decryptor = discord::dave::CreateDecryptor();
    encryptor->SetPassthroughMode(false);
    encryptor->SetKeyRatchet(std::make_unique<TestRatchet>());
    encryptor->AssignSsrcToCodec(7, discord::dave::Codec::VP8);
    decryptor->TransitionToKeyRatchet(std::make_unique<TestRatchet>());
    decryptor->TransitionToPassthroughMode(false, std::chrono::seconds(0));
    // VP8 delta frame has a single clear byte; keyframe has ten clear bytes.
    for (const auto first : {char(0), char(1)}) {
        QByteArray frame(25000, 'v');
        frame[0] = first;
        QByteArray ciphertext(int(encryptor->GetMaxCiphertextByteSize(discord::dave::MediaType::Video, frame.size())), '\0');
        size_t written = 0;
        QCOMPARE(encryptor->Encrypt(discord::dave::MediaType::Video, 7,
            discord::dave::ArrayView<const uint8_t>(reinterpret_cast<const uint8_t *>(frame.constData()), frame.size()),
            discord::dave::ArrayView<uint8_t>(reinterpret_cast<uint8_t *>(ciphertext.data()), ciphertext.size()), &written),
            discord::dave::IEncryptor::Success);
        ciphertext.resize(int(written));
        QVERIFY(ciphertext != frame);
        Vp8Reassembler receiver;
        const auto packets = packetizeVp8(ciphertext);
        std::optional<QByteArray> complete;
        for (int i = int(packets.size()) - 1; i >= 0; --i)
            complete = receiver.push(uint16_t(i), 1, i == packets.size() - 1, packets[i], 0);
        QVERIFY(complete);
        QByteArray plaintext(int(decryptor->GetMaxPlaintextByteSize(discord::dave::MediaType::Video, complete->size())), '\0');
        QCOMPARE(decryptor->Decrypt(discord::dave::MediaType::Video,
            discord::dave::ArrayView<const uint8_t>(reinterpret_cast<const uint8_t *>(complete->constData()), complete->size()),
            discord::dave::ArrayView<uint8_t>(reinterpret_cast<uint8_t *>(plaintext.data()), plaintext.size()), &written),
            discord::dave::IDecryptor::Success);
        plaintext.resize(int(written));
        QCOMPARE(plaintext, frame);
    }
}

void TestVp8Rtp::discardsLateFramesAndHandlesTimestampWrap()
{
    Vp8Reassembler receiver;
    const QByteArray first = QByteArray("\x10", 1) + "a";
    QVERIFY(!receiver.push(1, 0xfffffff0, false, first, 0));
    const auto newer = receiver.push(3, 0x10, true, first, 1);
    QVERIFY(newer);
    QVERIFY(!receiver.push(2, 0xfffffff0, true, QByteArray("\0", 1) + "b", 2));
    QCOMPARE(receiver.bufferedFrameCount(), 0);
}

void TestVp8Rtp::outOfOrderAndSequenceWrap()
{
    QByteArray frame(25000, '\0');
    for (int i = 0; i < frame.size(); ++i) frame[i] = char(i % 251);
    const auto packets = packetizeVp8(frame);
    QVERIFY(packets.size() > 20);
    Vp8Reassembler receiver;
    std::optional<QByteArray> decoded;
    for (int i = int(packets.size()) - 1; i >= 0; --i) {
        QVERIFY(packets[i].size() <= 1000);
        decoded = receiver.push(uint16_t(65530 + i), 12345, i == packets.size() - 1, packets[i], 0);
        QCOMPARE(decoded.has_value(), i == 0);
    }
    QCOMPARE(*decoded, frame);
    QCOMPARE(receiver.bufferedFrameCount(), 0);
    // Late retransmission must not deliver the same frame twice.
    QVERIFY(!receiver.push(65530, 12345, false, packets.first(), 1));
    QCOMPARE(receiver.bufferedFrameCount(), 0);
}

void TestVp8Rtp::missingPacketAndDuplicates()
{
    const QByteArray frame(5000, 'x');
    const auto packets = packetizeVp8(frame);
    Vp8Reassembler receiver;
    for (int i = 0; i < packets.size(); ++i) {
        if (i == 2) continue;
        QVERIFY(!receiver.push(uint16_t(i), 1, i == packets.size() - 1, packets[i], 0));
        QVERIFY(!receiver.push(uint16_t(i), 1, i == packets.size() - 1, packets[i], 1));
    }
    const auto decoded = receiver.push(2, 1, false, packets[2], 2);
    QVERIFY(decoded);
    QCOMPARE(*decoded, frame);
}

void TestVp8Rtp::descriptorExtensions()
{
    Vp8Reassembler receiver;
    // X,S; I,L,T,K; two-byte picture ID; TL0; T/K then frame bytes.
    const auto packet = QByteArray::fromHex("90f080010203") + QByteArray("frame");
    const auto decoded = receiver.push(1, 2, true, packet, 0);
    QVERIFY(decoded);
    QCOMPARE(*decoded, QByteArray("frame"));
    const QList<QByteArray> invalid = {{}, QByteArray::fromHex("80"), QByteArray::fromHex("9080"),
        QByteArray::fromHex("908080"), QByteArray::fromHex("90f0800102")};
    for (const auto &payload : invalid)
        QVERIFY(!receiver.push(1, 3, true, payload, 1));
    QCOMPARE(receiver.bufferedFrameCount(), 0);
}

void TestVp8Rtp::boundsAndExpiry()
{
    QVERIFY(packetizeVp8({}).isEmpty());
    QVERIFY(packetizeVp8(QByteArray(Vp8Reassembler::MaxFrameBytes + 1, 'x')).isEmpty());
    QVERIFY(packetizeVp8(QByteArray(5000, 'x'), 2).isEmpty());
    Vp8Reassembler receiver;
    for (uint32_t t = 0; t < 1000; ++t) {
        QVERIFY(!receiver.push(1, t, false, QByteArray("\x10", 1) + "incomplete", t));
        QVERIFY(receiver.bufferedFrameCount() <= Vp8Reassembler::MaxBufferedFrames);
    }
    QVERIFY(receiver.droppedFrames() >= 997);
    QVERIFY(!receiver.push(1, 2000, false, QByteArray("\x10", 1) + "next", 2000));
    QCOMPARE(receiver.bufferedFrameCount(), 1);
    QVERIFY(!receiver.push(2, 2000, true, QByteArray("\0", 1) + QByteArray(Vp8Reassembler::MaxFrameBytes, 'x'), 2001));
    QCOMPARE(receiver.bufferedFrameCount(), 0);
}

void TestVp8Rtp::conflictingBoundaries()
{
    Vp8Reassembler receiver;
    QVERIFY(!receiver.push(1, 1, false, QByteArray("\x10", 1) + "a", 0));
    QVERIFY(!receiver.push(2, 1, false, QByteArray("\x10", 1) + "b", 1));
    QCOMPARE(receiver.droppedFrames(), quint64(1));
    QCOMPARE(receiver.bufferedFrameCount(), 0);
}

#ifdef TEST_VP8_CODEC
void TestVp8Rtp::realtimeCodecRoundtrip()
{
    Acheron::Core::Media::RealtimeVp8 encoder, decoder;
    QVERIFY2(encoder.openEncoder(), qPrintable(encoder.error()));
    QVERIFY(decoder.openDecoder());
    QImage image(640, 360, QImage::Format_RGBA8888);
    image.fill(QColor(30, 100, 180));
    for (int i = 0; i < 30; ++i) {
        const auto encoded = encoder.encode(image, i % 15 == 0);
        QVERIFY(!encoded.isEmpty()); // no lag-in-frames backlog
        Vp8Reassembler receiver;
        const auto packets = packetizeVp8(encoded);
        std::optional<QByteArray> frame;
        for (int p = 0; p < packets.size(); ++p)
            frame = receiver.push(uint16_t(p), uint32_t(i), p == packets.size() - 1, packets[p], i);
        QVERIFY(frame);
        const auto decoded = decoder.decode(*frame);
        QCOMPARE(decoded.size(), image.size());
        const auto pixel = decoded.pixelColor(320, 180);
        QVERIFY(std::abs(pixel.red() - 30) < 8);
        QVERIFY(std::abs(pixel.green() - 100) < 8);
        QVERIFY(std::abs(pixel.blue() - 180) < 8);
    }
    QVERIFY(decoder.decode(QByteArray("invalid")).isNull());
}
void TestVp8Rtp::desktopCodecKeepsAspectRatio()
{
    Acheron::Core::Media::RealtimeVp8 encoder, decoder;
    QVERIFY(!encoder.openEncoder(QSize(1281, 720), 15, 1800000));
    QVERIFY(encoder.openEncoder(QSize(1280, 720), 15, 1800000));
    QVERIFY(decoder.openDecoder());
    QImage portrait(600, 1000, QImage::Format_RGB32);
    portrait.fill(QColor(30, 100, 180));
    for (int i = 0; i < 15; ++i) {
        const auto encoded = encoder.encode(portrait, i == 0);
        QVERIFY(!encoded.isEmpty());
        const auto decoded = decoder.decode(encoded);
        QCOMPARE(decoded.size(), QSize(1280, 720));
        QVERIFY(decoded.pixelColor(100, 360).red() < 8);
        const auto center = decoded.pixelColor(640, 360);
        QVERIFY(std::abs(center.red() - 30) < 8);
        QVERIFY(std::abs(center.green() - 100) < 8);
        QVERIFY(std::abs(center.blue() - 180) < 8);
    }
}
#endif
QTEST_GUILESS_MAIN(TestVp8Rtp)
#include "tst_Vp8Rtp.moc"
