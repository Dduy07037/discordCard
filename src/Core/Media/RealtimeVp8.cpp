#include "RealtimeVp8.hpp"
#include <cstring>

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

bool RealtimeVp8::openEncoder()
{
    if (impl->encoder)
        return true;
    const auto *codec = avcodec_find_encoder_by_name("libvpx");
    if (!codec) {
        impl->error = QStringLiteral("This FFmpeg build has no libvpx VP8 encoder.");
        return false;
    }
    auto *context = avcodec_alloc_context3(codec);
    if (!context)
        return false;
    context->width = 640;
    context->height = 360;
    context->time_base = AVRational{1, 15};
    context->framerate = AVRational{15, 1};
    context->pix_fmt = AV_PIX_FMT_YUV420P;
    context->bit_rate = 600000;
    context->rc_max_rate = 600000;
    context->rc_buffer_size = 600000;
    context->gop_size = 15;
    context->max_b_frames = 0;
    context->thread_count = 1;
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
    return true;
}

bool RealtimeVp8::openDecoder()
{
    if (impl->decoder)
        return true;
    const auto *codec = avcodec_find_decoder(AV_CODEC_ID_VP8);
    if (!codec)
        return false;
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
    return true;
}

QByteArray RealtimeVp8::encode(const QImage &image, bool keyframe)
{
    if (!impl->encoder || image.isNull() || av_frame_make_writable(impl->input) < 0)
        return {};
    const auto rgb = image.convertToFormat(QImage::Format_RGBA8888);
    impl->encodeScale = sws_getCachedContext(impl->encodeScale, rgb.width(), rgb.height(), AV_PIX_FMT_RGBA,
        640, 360, AV_PIX_FMT_YUV420P, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
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
    if (!impl->decoder || frame.isEmpty() || frame.size() > 2 * 1024 * 1024)
        return {};
    av_packet_unref(impl->packet);
    if (av_new_packet(impl->packet, int(frame.size())) < 0)
        return {};
    std::memcpy(impl->packet->data, frame.constData(), size_t(frame.size()));
    const auto sent = avcodec_send_packet(impl->decoder, impl->packet);
    av_packet_unref(impl->packet);
    if (sent < 0 || avcodec_receive_frame(impl->decoder, impl->output) < 0)
        return {};
    const auto *decoded = impl->output;
    if (decoded->width <= 0 || decoded->height <= 0 || decoded->width > 4096 || decoded->height > 2160)
        return {};
    QImage image(decoded->width, decoded->height, QImage::Format_RGBA8888);
    if (image.isNull())
        return {};
    impl->decodeScale = sws_getCachedContext(impl->decodeScale, decoded->width, decoded->height,
        AVPixelFormat(decoded->format), image.width(), image.height(), AV_PIX_FMT_RGBA,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!impl->decodeScale)
        return {};
    uint8_t *destination[] = {image.bits()};
    const int stride[] = {int(image.bytesPerLine())};
    sws_scale(impl->decodeScale, decoded->data, decoded->linesize, 0, decoded->height, destination, stride);
    av_frame_unref(impl->output);
    return image;
}

} // namespace Acheron::Core::Media
