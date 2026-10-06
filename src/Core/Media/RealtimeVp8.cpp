#include "RealtimeVp8.hpp"
#include <QPainter>
#include <QThread>
#include <cstring>
#include <algorithm>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace Acheron::Core::Media {

struct RealtimeVp8::Impl {
    AVCodecContext *encoder = nullptr;
    AVCodecContext *decoder = nullptr;
    AVFrame *input = av_frame_alloc();
    AVFrame *output = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    SwsContext *encodeScale = nullptr;
    SwsContext *decodeScale = nullptr;
    int64_t pts = 0;
    QString error;
    ~Impl()
    {
        avcodec_free_context(&encoder);
        avcodec_free_context(&decoder);
        av_frame_free(&input);
        av_frame_free(&output);
        av_packet_free(&packet);
        sws_freeContext(encodeScale);
        sws_freeContext(decodeScale);
    }
};

RealtimeVp8::RealtimeVp8() : impl(std::make_unique<Impl>()) {}
RealtimeVp8::~RealtimeVp8() = default;
QString RealtimeVp8::error() const { return impl->error; }

bool RealtimeVp8::openEncoder(QSize size, int fps, int bitrate)
{
    if (size.width() < 2 || size.height() < 2 || size.width() > 1920 || size.height() > 1080
        || size.width() % 2 || size.height() % 2 || fps < 1 || fps > 60
        || bitrate < 100000 || bitrate > 5000000) {
        impl->error = QStringLiteral("Invalid realtime video encoder settings.");
        return false;
    }
    if (impl->encoder)
        return impl->encoder->width == size.width() && impl->encoder->height == size.height()
            && impl->encoder->framerate.num == fps && impl->encoder->bit_rate == bitrate;
    const auto *codec = avcodec_find_encoder_by_name("libvpx");
    if (!codec) {
        impl->error = QStringLiteral("This FFmpeg build has no libvpx VP8 encoder.");
        return false;
    }
    auto *context = avcodec_alloc_context3(codec);
    if (!context)
        return false;
    context->width = size.width();
    context->height = size.height();
    context->time_base = AVRational{1, fps};
    context->framerate = AVRational{fps, 1};
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->bit_rate = bitrate;
    context->rc_max_rate = bitrate;
    context->rc_buffer_size = bitrate;
    context->gop_size = fps;
    context->max_b_frames = 0;
    context->thread_count = std::max(1, std::min(4, QThread::idealThreadCount() / 2));
    av_opt_set(context->priv_data, "deadline", "realtime", 0);
    av_opt_set(context->priv_data, "cpu-used", "8", 0);
    av_opt_set(context->priv_data, "lag-in-frames", "0", 0);
    if (avcodec_open2(context, codec, nullptr) < 0 || !impl->input || !impl->packet) {
        avcodec_free_context(&context);
        impl->error = QStringLiteral("Could not open the realtime VP8 encoder.");
        return false;
    }
    impl->input->format = context->pix_fmt;
    impl->input->width = context->width;
    impl->input->height = context->height;
    if (av_frame_get_buffer(impl->input, 32) < 0) {
        avcodec_free_context(&context);
        return false;
    }
    impl->encoder = context;
    impl->error.clear();
    return true;
}

static AVCodecID decoderId(const QString &name)
{
    if (name.compare("VP8", Qt::CaseInsensitive) == 0) return AV_CODEC_ID_VP8;
    if (name.compare("H264", Qt::CaseInsensitive) == 0) return AV_CODEC_ID_H264;
    return AV_CODEC_ID_NONE;
}

bool RealtimeVp8::hasDecoder(const QString &name)
{
    return decoderId(name) != AV_CODEC_ID_NONE && avcodec_find_decoder(decoderId(name));
}

bool RealtimeVp8::openDecoder(const QString &codecName)
{
    if (!impl->output || !impl->packet) return false;
    const auto id = decoderId(codecName);
    if (impl->decoder && impl->decoder->codec_id == id)
        return true;
    avcodec_free_context(&impl->decoder);
    av_frame_unref(impl->output);
    const auto *codec = id == AV_CODEC_ID_NONE ? nullptr : avcodec_find_decoder(id);
    if (!codec) {
        impl->error = QStringLiteral("No decoder available for %1.").arg(codecName);
        return false;
    }
    auto *context = avcodec_alloc_context3(codec);
    if (!context)
        return false;
    context->thread_count = 1;
    context->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context->max_pixels = 4096 * 2160;
    if (avcodec_open2(context, codec, nullptr) < 0 || !impl->output || !impl->packet) {
        avcodec_free_context(&context);
        return false;
    }
    impl->decoder = context;
    impl->error.clear();
    return true;
}

QByteArray RealtimeVp8::encode(const QImage &image, bool keyframe)
{
    if (!impl->encoder || image.isNull() || av_frame_make_writable(impl->input) < 0)
        return {};
    // Letterbox non-16:9 monitors instead of stretching their desktop contents.
    QImage rgb(impl->encoder->width, impl->encoder->height, QImage::Format_RGBA8888);
    rgb.fill(Qt::black);
    const auto fitted = image.size().scaled(rgb.size(), Qt::KeepAspectRatio);
    {
        QPainter painter(&rgb);
        painter.drawImage(QRect(QPoint((rgb.width() - fitted.width()) / 2,
                                      (rgb.height() - fitted.height()) / 2), fitted), image);
    }
    impl->encodeScale = sws_getCachedContext(impl->encodeScale, rgb.width(), rgb.height(), AV_PIX_FMT_RGBA,
        impl->encoder->width, impl->encoder->height, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl->encodeScale)
        return {};
    const uint8_t *source[] = {rgb.constBits()};
    const int stride[] = {int(rgb.bytesPerLine())};
    sws_scale(impl->encodeScale, source, stride, 0, rgb.height(), impl->input->data, impl->input->linesize);
    impl->input->pts = impl->pts++;
    impl->input->pict_type = keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
    if (avcodec_send_frame(impl->encoder, impl->input) < 0)
        return {};
    av_packet_unref(impl->packet);
    if (avcodec_receive_packet(impl->encoder, impl->packet) < 0)
        return {};
    QByteArray encoded(reinterpret_cast<const char *>(impl->packet->data), impl->packet->size);
    av_packet_unref(impl->packet);
    return encoded;
}

QImage RealtimeVp8::decode(const QByteArray &frame)
{
    if (!impl->decoder || frame.isEmpty() || frame.size() > 2 * 1024 * 1024) return {};
    av_packet_unref(impl->packet);
    if (av_new_packet(impl->packet, int(frame.size())) < 0) return {};
    std::memcpy(impl->packet->data, frame.constData(), size_t(frame.size()));
    const auto sent = avcodec_send_packet(impl->decoder, impl->packet);
    av_packet_unref(impl->packet);
    if (sent < 0) return {};
    QImage latest;
    // Drain all available output; rendering keeps only the latest decoded
    // image. Never leave output queued until a later input packet arrives.
    for (int i = 0; i < 16 && avcodec_receive_frame(impl->decoder, impl->output) >= 0; ++i) {
        const auto *decoded = impl->output;
        if (decoded->width <= 0 || decoded->height <= 0 || decoded->width > 4096 || decoded->height > 2160) {
            av_frame_unref(impl->output);
            continue;
        }
        QImage image(decoded->width, decoded->height, QImage::Format_RGBA8888);
        if (!image.isNull()) {
            impl->decodeScale = sws_getCachedContext(impl->decodeScale, decoded->width, decoded->height,
                AVPixelFormat(decoded->format), image.width(), image.height(), AV_PIX_FMT_RGBA,
                SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
            if (impl->decodeScale) {
                uint8_t *destination[] = {image.bits()};
                const int stride[] = {int(image.bytesPerLine())};
                sws_scale(impl->decodeScale, decoded->data, decoded->linesize, 0, decoded->height, destination, stride);
                latest = std::move(image);
            }
        }
        av_frame_unref(impl->output);
    }
    return latest;
}

} // namespace Acheron::Core::Media
