#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <memory>

namespace Acheron::Core::Media {

// Experimental stream codec. No delayed/B frames or internal encode backlog.
class RealtimeVp8
{
public:
    RealtimeVp8();
    ~RealtimeVp8();
    RealtimeVp8(const RealtimeVp8 &) = delete;
    RealtimeVp8 &operator=(const RealtimeVp8 &) = delete;
    bool openEncoder(QSize size = QSize(640, 360), int fps = 15, int bitrate = 600000);
    bool openDecoder();
    QByteArray encode(const QImage &image, bool keyframe);
    QImage decode(const QByteArray &frame);
    QString error() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace Acheron::Core::Media
