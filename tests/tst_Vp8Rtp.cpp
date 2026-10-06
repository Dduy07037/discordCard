#include <QTest>
#include <dave/dave_interfaces.h>
#include <bytes/bytes.h>
#include "Discord/Voice/Vp8Rtp.hpp"
#include "Discord/Voice/H264Rtp.hpp"
#include <QElapsedTimer>
#include <QPainter>
#include "Core/Media/LatestVideoFrame.hpp"
#include "Core/Media/ScreenShareSettings.hpp"
#include "Core/Media/StreamLoadController.hpp"
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
    void h264StapFuLossAndBounds();
    void automaticLoadControl();
#ifdef TEST_VP8_CODEC
    void realtimeCodecRoundtrip();
    void desktopCodecKeepsAspectRatio();
    void higherFrameRates_data();
    void higherFrameRates();
    void highQualityDesktopKeepsFineDetail();
    void movingDesktopAndEncoderReconfiguration();
    void h264DecodeAndDavePacketization();
#endif
};

void TestVp8Rtp::automaticLoadControl()
{
    using namespace Acheron::Core::Media;
    StreamLoadController automatic(ScreenShareSettings::forPreset(3, 60), 60);
    for (int i = 0; i < 20; ++i) QVERIFY(!automatic.observe(200, 250, true, 10000 + i, true));
    QCOMPARE(automatic.fps, 60); // keyframe bursts alone do not reduce FPS
    for (int i = 0; i < 6; ++i) automatic.observe(30, 40, true, 11000 + i, false);
    QCOMPARE(automatic.fps, 30);
    for (int i = 0; i < 20; ++i) QVERIFY(!automatic.observe(40, 55, true, 12000 + i, false));
    QCOMPARE(automatic.fps, 30); // cooldown prevents oscillation/repeated resets
    automatic.observe(40, 55, true, 16000, false);
    QCOMPARE(automatic.fps, 20);
    for (int i = 0; i < 6; ++i) automatic.observe(80, 100, true, 21000 + i, false);
    QCOMPARE(automatic.fps, 15);
    for (int i = 0; i < 6; ++i) automatic.observe(80, 100, true, 26000 + i, false);
    QCOMPARE(automatic.settings.resolution, QSize(1280, 720));
    for (int i = 0; i < 100; ++i) automatic.observe(500, 500, false, 31000 + i * 100, false);
    QCOMPARE(automatic.fps, 15);
    QVERIFY(automatic.settings.bitrate >= 2000000);
    StreamLoadController healthy(ScreenShareSettings::forPreset(3, 30), 30);
    for (int i = 0; i < 200; ++i) QVERIFY(!healthy.observe(10, 20, true, 10000 + i * 33, false));
    QCOMPARE(healthy.settings.resolution, QSize(1920, 1080));
    StreamLoadController manual(ScreenShareSettings::forPreset(2, 60), 60);
    for (int i = 0; i < 100; ++i) QVERIFY(!manual.observe(500, 500, false, 10000 + i * 100, false));
    QCOMPARE(manual.fps, 60);
    QCOMPARE(manual.settings.bitrate, 16000000);
}

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
        QByteArray frame(300000, 'v');
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


void TestVp8Rtp::h264StapFuLossAndBounds()
{
    H264Reassembler receiver;
    // STAP-A carries a parameter set and an IDR; start codes are restored.
    const auto stap = QByteArray::fromHex("780003674201000265aa");
    const auto initial = receiver.push(65534, 0xfffffff0, true, stap, 0);
    QVERIFY(initial);
    QCOMPARE(*initial, QByteArray::fromHex("000000016742010000000165aa"));
    // FU-A fragments arrive in reverse order and across a sequence wrap.
    QVERIFY(!receiver.push(1, 0x10, true, QByteArray::fromHex("7c41cc"), 1));
    QVERIFY(!receiver.push(0, 0x10, false, QByteArray::fromHex("7c01bb"), 2));
    const auto next = receiver.push(65535, 0x10, false, QByteArray::fromHex("7c81aa"), 3);
    QVERIFY(next);
    QCOMPARE(*next, QByteArray::fromHex("0000000161aabbcc"));
    QVERIFY(!receiver.push(65535, 0x10, false, QByteArray::fromHex("7c81aa"), 4));
    QVERIFY(!receiver.push(2, 0x20, false, QByteArray::fromHex("7c81aa"), 5));
    QVERIFY(!receiver.push(4, 0x20, true, QByteArray::fromHex("7c41cc"), 6));
    // A newer parameter frame permits recovery after a missing frame.
    QVERIFY(receiver.push(10, 0x30, true, stap, 7));
    QVERIFY(!receiver.push(3, 0x20, false, QByteArray::fromHex("7c01bb"), 8));
    QCOMPARE(receiver.bufferedFrameCount(), 0);
    for (uint32_t t = 1; t < 1000; ++t) {
        QVERIFY(!receiver.push(20, 0x100 + t, false, QByteArray::fromHex("7c81aa"), t));
        QVERIFY(receiver.bufferedFrameCount() <= H264Reassembler::MaxBufferedFrames);
    }
    QVERIFY(receiver.droppedFrames() > 990);
    receiver.reset();
    for (const auto &bad : {QByteArray::fromHex("78000467"), QByteArray::fromHex("78800067"),
                           QByteArray::fromHex("78000167000178"), QByteArray::fromHex("fc85aa")}) {
        QVERIFY(!receiver.push(1, 1, true, bad, 2000));
        receiver.reset();
    }
    QVERIFY(!receiver.push(1, 1, true, QByteArray(H264Reassembler::MaxFrameBytes + 1, 'x'), 2000));
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
        QVERIFY2(std::abs(pixel.blue() - 180) < 8, qPrintable(QStringLiteral("frame %1: blue=%2").arg(i).arg(pixel.blue())));
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

void TestVp8Rtp::higherFrameRates_data()
{
    QTest::addColumn<int>("fps");
    QTest::addColumn<QSize>("size");
    QTest::addColumn<int>("bitrate");
    QTest::newRow("720p30") << 30 << QSize(1280, 720) << 3000000;
    QTest::newRow("720p60") << 60 << QSize(1280, 720) << 4500000;
    for (const int fps : {30, 60}) {
        const auto settings = Acheron::Core::Media::ScreenShareSettings::forPreset(2, fps);
        QTest::newRow(fps == 30 ? "1080p-maximum30" : "1080p-maximum60") << fps << settings.resolution << settings.bitrate;
    }
}
void TestVp8Rtp::higherFrameRates()
{
    QFETCH(int, fps);
    QFETCH(QSize, size);
    QFETCH(int, bitrate);
    Acheron::Core::Media::RealtimeVp8 encoder, decoder;
    QVERIFY(!encoder.openEncoder(QSize(1280, 720), 61, 3000000));
    QVERIFY2(encoder.openEncoder(size, fps, bitrate), qPrintable(encoder.error()));
    QVERIFY(decoder.openDecoder());
    QImage image(size, QImage::Format_RGB32);
    QElapsedTimer elapsed; elapsed.start();
    for (int i = 0; i < fps; ++i) {
        image.fill(QColor(30 + i, 100, 180));
        const auto encoded = encoder.encode(image, i == 0);
        QVERIFY(!encoded.isEmpty());
        const auto decoded = decoder.decode(encoded);
        QCOMPARE(decoded.size(), image.size());
        QVERIFY(std::abs(decoded.pixelColor(size.width() / 2, size.height() / 2).red() - (30 + i)) < 10);
    }
    qInfo() << size << "target" << fps << "FPS:" << fps << "immediate codec roundtrips in" << elapsed.elapsed() << "ms";
}

void TestVp8Rtp::highQualityDesktopKeepsFineDetail()
{
    const auto settings = Acheron::Core::Media::ScreenShareSettings::forPreset(2, 30);
    Acheron::Core::Media::RealtimeVp8 encoder, decoder;
    QVERIFY(encoder.openEncoder(settings.resolution, 30, settings.bitrate));
    QVERIFY(decoder.openDecoder());
    QImage desktop(settings.resolution, QImage::Format_RGB32);
    desktop.fill(Qt::white);
    {
        QPainter painter(&desktop);
        // Native one-pixel strokes lose contrast when downscaled to 720p.
        for (int x = 100; x < 200; x += 2) painter.fillRect(x, 100, 1, 300, Qt::black);
        painter.fillRect(250, 200, 400, 1, Qt::black);
    }
    const auto encoded = encoder.encode(desktop, true);
    QVERIFY(!encoded.isEmpty());
    QVERIFY(encoded.size() <= 512 * 1024);
    const auto decoded = decoder.decode(encoded);
    QCOMPARE(decoded.size(), desktop.size());
    QVERIFY(decoded.pixelColor(101, 250).red() - decoded.pixelColor(100, 250).red() > 160);
}

void TestVp8Rtp::movingDesktopAndEncoderReconfiguration()
{
    Acheron::Core::Media::RealtimeVp8 encoder, decoder;
    QVERIFY(encoder.openEncoder(QSize(1920, 1080), 30, 6000000));
    QVERIFY(decoder.openDecoder());
    double totalEncodeMs = 0;
    for (int i = 0; i < 16; ++i) {
        const auto target = i < 8 ? QSize(1920, 1080) : QSize(1280, 720);
        if (i == 8) QVERIFY(encoder.openEncoder(target, 20, 4000000));
        QImage image(1920, 1080, i % 2 ? QImage::Format_RGBA8888 : QImage::Format_RGB32);
        image.fill(QColor(210, 210, 210));
        {
            QPainter p(&image);
            for (int x = 100; x < 700; x += 4) p.fillRect(x, 100, 1, 500, Qt::black);
            p.fillRect(i * 40, 700, 300, 120, Qt::cyan);
        }
        QElapsedTimer timer; timer.start();
        const auto encoded = encoder.encode(image, i == 0 || i == 8);
        totalEncodeMs += double(timer.nsecsElapsed()) / 1000000;
        QVERIFY(!encoded.isEmpty());
        const auto decoded = decoder.decode(encoded);
        QCOMPARE(decoded.size(), target);
        const auto background = decoded.pixelColor(target.width() - 20, target.height() - 20);
        QVERIFY(std::abs(background.red() - 210) < 15);
        QVERIFY(std::abs(background.blue() - 210) < 15);
        const auto cyan = decoded.pixelColor((i * 40 + 150) * target.width() / 1920,
                                             760 * target.height() / 1080);
        QVERIFY(cyan.red() < 30 && cyan.green() > 220 && cyan.blue() > 220);
    }
    qInfo() << "Moving desktop/native RGB32+RGBA and 1080p->720p reconfiguration:" << totalEncodeMs / 16 << "ms mean encode";
}

void TestVp8Rtp::h264DecodeAndDavePacketization()
{
    // Own fixture: three 320x180 solid-color frames encoded with FFmpeg/x264.
    // Embedded to require only a decoder on CI, not an H264 encoder.
    const auto fixture = QByteArray::fromBase64("AAAAAQkQAAAAAWdCwA3aBQZ+fARAAAADAEAAAA8jxQqoAAAAAWjOD8gAAAEGBf//V9xF6b3m2Ui3lizYINkj7u94MjY0IC0gY29yZSAxNjQgcjMxMDggMzFlMTlmOSAtIEguMjY0L01QRUctNCBBVkMgY29kZWMgLSBDb3B5bGVmdCAyMDAzLTIwMjMgLSBodHRwOi8vd3d3LnZpZGVvbGFuLm9yZy94MjY0Lmh0bWwgLSBvcHRpb25zOiBjYWJhYz0wIHJlZj0xIGRlYmxvY2s9MDowOjAgYW5hbHlzZT0wOjAgbWU9ZGlhIHN1Ym1lPTAgcHN5PTEgcHN5X3JkPTEuMDA6MC4wMCBtaXhlZF9yZWY9MCBtZV9yYW5nZT0xNiBjaHJvbWFfbWU9MSB0cmVsbGlzPTAgOHg4ZGN0PTAgY3FtPTAgZGVhZHpvbmU9MjEsMTEgZmFzdF9wc2tpcD0xIGNocm9tYV9xcF9vZmZzZXQ9MCB0aHJlYWRzPTMgbG9va2FoZWFkX3RocmVhZHM9MyBzbGljZWRfdGhyZWFkcz0xIHNsaWNlcz0zIG5yPTAgZGVjaW1hdGU9MSBpbnRlcmxhY2VkPTAgYmx1cmF5X2NvbXBhdD0wIGNvbnN0cmFpbmVkX2ludHJhPTAgYmZyYW1lcz0wIHdlaWdodHA9MCBrZXlpbnQ9MzAga2V5aW50X21pbj0zIHNjZW5lY3V0PTAgaW50cmFfcmVmcmVzaD0wIHJjPWNyZiBtYnRyZWU9MCBjcmY9MjMuMCBxY29tcD0wLjYwIHFwbWluPTAgcXBtYXg9NjkgcXBzdGVwPTQgaXBfcmF0aW89MS40MCBhcT0wAIAAAAFliIQ6EYoAAg7xwABBgjgACANJycnJycnJycnJycnJycnJycnJ1111111111111111111111111111111111111111111111111111111111114AAAAWUCiIhDoRigACDvHAAEGCOAAIA0nJycnJycnJycnJycnJycnJycnXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXgAAAWUBQiIQ6EYoAAg7xwABBgjgACANJycnJycnJycnJycnJycnJycnJ1111111111111111111111111111111111111111111111111111111111114AAAAABCTAAAAFBmiAqgKMAAAFBAomiAqgKMAAAAUEBQmiAqgKMAAAAAQkwAAABQZpAKoCjAAABQQKJpAKoCjAAAAFBAUJpAKoCjA==");
    QList<QByteArray> nalus;
    for (int i = 0; i + 3 < fixture.size();) {
        int prefix = 0;
        if (fixture.mid(i, 4) == QByteArray::fromHex("00000001")) prefix = 4;
        else if (fixture.mid(i, 3) == QByteArray::fromHex("000001")) prefix = 3;
        if (!prefix) { ++i; continue; }
        const int start = i + prefix;
        int end = start;
        while (end + 3 < fixture.size() && fixture.mid(end, 3) != QByteArray::fromHex("000001") &&
               fixture.mid(end, 4) != QByteArray::fromHex("00000001")) ++end;
        if (end + 3 >= fixture.size()) end = int(fixture.size());
        nalus.append(fixture.mid(start, end - start));
        i = end;
    }
    QList<QByteArray> frames;
    QByteArray frame;
    for (const auto &nal : nalus) {
        if ((quint8(nal[0]) & 0x1f) == 9 && !frame.isEmpty()) { frames.append(frame); frame.clear(); }
        frame += QByteArray::fromHex("00000001") + nal;
    }
    if (!frame.isEmpty()) frames.append(frame);
    QCOMPARE(frames.size(), 3);
    struct Ratchet : discord::dave::IKeyRatchet {
        discord::dave::EncryptionKey GetKey(discord::dave::KeyGeneration g) noexcept override
        { return discord::dave::EncryptionKey(std::vector<uint8_t>(16, uint8_t(42 + g))); }
        void DeleteKey(discord::dave::KeyGeneration) noexcept override {}
    };
    auto enc = discord::dave::CreateEncryptor(); auto dec = discord::dave::CreateDecryptor();
    enc->SetPassthroughMode(false); enc->SetKeyRatchet(std::make_unique<Ratchet>());
    enc->AssignSsrcToCodec(7, discord::dave::Codec::H264);
    dec->TransitionToKeyRatchet(std::make_unique<Ratchet>());
    dec->TransitionToPassthroughMode(false, std::chrono::seconds(0));
    Acheron::Core::Media::RealtimeVp8 decoder;
    QVERIFY(decoder.openDecoder(QStringLiteral("H264")));
    H264Reassembler receiver;
    uint16_t sequence = 65000;
    for (int f = 0; f < frames.size(); ++f) {
        const auto &input = frames[f];
        QByteArray encrypted(int(enc->GetMaxCiphertextByteSize(discord::dave::MediaType::Video, input.size())), '\0');
        size_t written = 0;
        QCOMPARE(enc->Encrypt(discord::dave::MediaType::Video, 7,
            {reinterpret_cast<const uint8_t *>(input.constData()), size_t(input.size())},
            {reinterpret_cast<uint8_t *>(encrypted.data()), size_t(encrypted.size())}, &written), discord::dave::IEncryptor::Success);
        encrypted.resize(int(written));
        QList<QByteArray> packets;
        int pos = 0;
        while (pos < encrypted.size()) {
            QCOMPARE(encrypted.mid(pos, 4), QByteArray::fromHex("00000001"));
            int end = encrypted.indexOf(QByteArray::fromHex("00000001"), pos + 4);
            if (end < 0) end = int(encrypted.size());
            const auto nal = encrypted.mid(pos + 4, end - pos - 4);
            if (nal.size() <= 80) packets.append(nal);
            else {
                for (int offset = 1; offset < nal.size(); offset += 78) {
                    const bool last = offset + 78 >= nal.size();
                    QByteArray p;
                    p += char((quint8(nal[0]) & 0xe0) | 28);
                    p += char((offset == 1 ? 0x80 : 0) | (last ? 0x40 : 0) | (quint8(nal[0]) & 0x1f));
                    p += nal.mid(offset, 78);
                    packets.append(p);
                }
            }
            pos = end;
        }
        std::optional<QByteArray> assembled;
        // Parameter/AUD packets lead, remaining fragmented data is reversed.
        int lead = 0;
        while (lead < packets.size() && (quint8(packets[lead][0]) & 0x1f) != 28) {
            auto ready = receiver.push(sequence + lead, 90000 + f * 3000, lead == packets.size() - 1, packets[lead], f);
            if (ready) assembled = std::move(ready);
            ++lead;
        }
        for (int p = int(packets.size()) - 1; p >= lead; --p) {
            auto ready = receiver.push(sequence + p, 90000 + f * 3000, p == packets.size() - 1, packets[p], f);
            if (ready) assembled = std::move(ready);
        }
        QVERIFY(assembled);
        QByteArray plain(int(dec->GetMaxPlaintextByteSize(discord::dave::MediaType::Video, assembled->size())), '\0');
        QCOMPARE(dec->Decrypt(discord::dave::MediaType::Video,
            {reinterpret_cast<const uint8_t *>(assembled->constData()), size_t(assembled->size())},
            {reinterpret_cast<uint8_t *>(plain.data()), size_t(plain.size())}, &written), discord::dave::IDecryptor::Success);
        plain.resize(int(written));
        QCOMPARE(plain, input);
        const auto image = decoder.decode(plain);
        QCOMPARE(image.size(), QSize(320, 180));
        const auto pixel = image.pixelColor(160, 90);
        QVERIFY(std::abs(pixel.red() - 120) < 10);
        QVERIFY(std::abs(pixel.green() - 60) < 10);
        QVERIFY(std::abs(pixel.blue() - 200) < 10);
        sequence += uint16_t(packets.size());
    }
    QVERIFY(decoder.openDecoder(QStringLiteral("VP8")));
    QVERIFY(decoder.openDecoder(QStringLiteral("H264")));
    QVERIFY(!decoder.openDecoder(QStringLiteral("AV1")));
}
#endif
QTEST_GUILESS_MAIN(TestVp8Rtp)
#include "tst_Vp8Rtp.moc"
