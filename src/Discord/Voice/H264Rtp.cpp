#include "H264Rtp.hpp"

namespace Acheron::Discord::Voice {
namespace {
const QByteArray startCode = QByteArray::fromHex("00000001");

bool appendPacket(QByteArray &frame, const QByteArray &payload, std::optional<quint8> &fragment)
{
    const auto type = quint8(payload[0]) & 0x1f;
    if (type >= 1 && type <= 23) {
        if (fragment) return false;
        frame += startCode;
        frame += payload;
    } else if (type == 24) {
        if (fragment || payload.size() < 4) return false;
        int offset = 1;
        while (offset < payload.size()) {
            if (offset + 2 > payload.size()) return false;
            const int size = (quint8(payload[offset]) << 8) | quint8(payload[offset + 1]);
            offset += 2;
            if (!size || size > payload.size() - offset) return false;
            const auto nal = quint8(payload[offset]);
            if ((nal & 0x80) || !(nal & 0x1f) || (nal & 0x1f) > 23) return false;
            frame += startCode;
            frame += payload.mid(offset, size);
            offset += size;
            if (frame.size() > H264Reassembler::MaxFrameBytes) return false;
        }
    } else if (type == 28) {
        if (payload.size() < 3) return false;
        const auto flags = quint8(payload[1]);
        const bool start = flags & 0x80, end = flags & 0x40;
        const quint8 nal = (quint8(payload[0]) & 0xe0) | (flags & 0x1f);
        if ((flags & 0x20) || (start && end) || !(nal & 0x1f) || (nal & 0x1f) > 23) return false;
        if (start) {
            if (fragment) return false;
            fragment = nal;
            frame += startCode;
            frame += char(nal);
        } else if (!fragment || *fragment != nal) {
            return false;
        }
        frame += payload.mid(2);
        if (end) fragment.reset();
    } else {
        return false;
    }
    return frame.size() <= H264Reassembler::MaxFrameBytes;
}
}

std::optional<QByteArray> H264Reassembler::push(uint16_t sequence, uint32_t timestamp,
    bool marker, const QByteArray &payload, qint64 nowMs)
{
    for (auto it = frames.begin(); it != frames.end();) {
        if (nowMs - it->createdAtMs > 250) { it = frames.erase(it); ++dropped; }
        else ++it;
    }
    if (payload.isEmpty() || (quint8(payload[0]) & 0x80) ||
        (lastCompleted && int32_t(timestamp - *lastCompleted) <= 0)) return {};
    const int type = quint8(payload[0]) & 0x1f;
    if (!(type >= 1 && type <= 24) && type != 28) return {};
    if (type == 28 && payload.size() < 3) return {};
    if (!frames.contains(timestamp) && frames.size() >= MaxBufferedFrames) {
        auto oldest = frames.begin();
        for (auto it = frames.begin(); it != frames.end(); ++it)
            if (it->createdAtMs < oldest->createdAtMs) oldest = it;
        frames.erase(oldest); ++dropped;
    }
    auto it = frames.find(timestamp);
    if (it == frames.end()) {
        Frame f; f.createdAtMs = nowMs;
        it = frames.insert(timestamp, std::move(f));
    }
    auto &f = it.value();
    if (f.packets.contains(sequence)) return {};
    if ((marker && f.last && *f.last != sequence) || f.packets.size() >= MaxFramePackets
        || payload.size() > MaxFrameBytes - f.bytes) {
        frames.erase(it); ++dropped; return {};
    }
    f.bytes += payload.size();
    f.packets.insert(sequence, payload);
    if (!f.first || int16_t(sequence - *f.first) < 0) f.first = sequence;
    if (marker) f.last = sequence;
    if (!f.last) return {};
    // Joining midstream requires an SPS/keyframe. This also prevents reverse
    // arrival of an IDR's FU-A tail from delivering before its SPS/PPS arrive.
    bool parameterStart = false;
    for (auto packet = f.packets.cbegin(); packet != f.packets.cend(); ++packet) {
        const auto &head = packet.value();
        const int headType = quint8(head[0]) & 0x1f;
        if (headType == 7 || (headType == 24 && head.size() >= 4 && (quint8(head[3]) & 0x1f) == 7))
            parameterStart = true;
    }
    if (!lastSequence && !parameterStart) return {};
    const uint16_t firstSequence = parameterStart ? *f.first : uint16_t(*lastSequence + 1);
    const unsigned count = unsigned(uint16_t(*f.last - firstSequence)) + 1;
    if (count > MaxFramePackets) { frames.erase(it); ++dropped; return {}; }
    for (unsigned i = 0; i < count; ++i)
        if (!f.packets.contains(uint16_t(firstSequence + i))) return {};
    // A FU continuation arriving before its start must remain buffered.
    const auto &first = f.packets.value(firstSequence);
    if ((quint8(first[0]) & 0x1f) == 28 && !(quint8(first[1]) & 0x80)) return {};
    QByteArray complete;
    std::optional<quint8> fragment;
    for (unsigned i = 0; i < count; ++i) {
        if (!appendPacket(complete, f.packets.value(uint16_t(firstSequence + i)), fragment)) {
            frames.erase(it); ++dropped; return {};
        }
    }
    if (fragment) { frames.erase(it); ++dropped; return {}; }
    lastSequence = f.last;
    frames.erase(it);
    lastCompleted = timestamp;
    for (auto older = frames.begin(); older != frames.end();) {
        if (int32_t(older.key() - timestamp) <= 0) { older = frames.erase(older); ++dropped; }
        else ++older;
    }
    return complete;
}
} // namespace Acheron::Discord::Voice
