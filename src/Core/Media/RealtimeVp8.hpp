#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <memory>

namespace Acheron::Core::Media {

// VP8 encoder and negotiated VP8/H264 stream decoder.
class RealtimeVp8
{
public:
    RealtimeVp8();
    ~RealtimeVp8();
    RealtimeVp8(const RealtimeVp8 &) = delete;
    RealtimeVp8 &operator=(const RealtimeVp8 &) = delete;
    bool openEncoder(QSize size = QSize(640, 360), int fps = 15, int bitrate = 600000);
    bool openDecoder(const QString &codecName = QStringLiteral("VP8"));
    static bool hasDecoder(const QString &codecName);
    QByteArray encode(const QImage &image, bool keyframe);
    QImage decode(const QByteArray &frame);
    QString error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Acheron::Core::Media
