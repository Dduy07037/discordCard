#include "Vp8Rtp.hpp"

#include <algorithm>

namespace Acheron::Discord::Voice {

QList<QByteArray> packetizeVp8(const QByteArray &frame, int payloadBudget)
{
    QList<QByteArray> packets;
    if (frame.isEmpty() || frame.size() > Vp8Reassembler::MaxFrameBytes || payloadBudget < 2)
        return packets;
    payloadBudget = std::min(payloadBudget, 1100);
    if ((frame.size() + payloadBudget - 2) / (payloadBudget - 1) > Vp8Reassembler::MaxFramePackets)
        return packets;
    for (int offset = 0; offset < frame.size(); offset += payloadBudget - 1) {
        QByteArray packet(1, offset == 0 ? '\x10' : '\0');
        packet.append(frame.mid(offset, payloadBudget - 1));
        packets.append(std::move(packet));
    }
    return packets;
}

std::optional<QByteArray> Vp8Reassembler::push(uint16_t sequence, uint32_t timestamp,
    bool marker, const QByteArray &payload, qint64 nowMs)
{
    for (auto it = frames.begin(); it != frames.end();) {
        if (nowMs - it->createdAtMs > 250) {
            it = frames.erase(it);
            ++dropped;
        } else {
            ++it;
        }
    }
    if (payload.isEmpty() || completed.contains(timestamp) ||
        (lastCompleted && int32_t(timestamp - *lastCompleted) <= 0))
        return {};
    const auto *p = reinterpret_cast<const uint8_t *>(payload.constData());
    int offset = 1;
    const bool start = (p[0] & 0x10) && (p[0] & 0x0f) == 0;
    if (p[0] & 0x80) {
        if (offset >= payload.size())
            return {};
        const auto extensions = p[offset++];
        if (extensions & 0x80) {
            if (offset >= payload.size())
                return {};
            const bool longPictureId = p[offset++] & 0x80;
            if (longPictureId)
                ++offset;
        }
        if (extensions & 0x40)
            ++offset;
        if (extensions & 0x30)
            ++offset;
    }
    if (offset >= payload.size())
        return {};
    if (!frames.contains(timestamp) && frames.size() >= MaxBufferedFrames) {
        auto oldest = frames.begin();
        for (auto it = frames.begin(); it != frames.end(); ++it)
            if (it->createdAtMs < oldest->createdAtMs)
                oldest = it;
        frames.erase(oldest);
        ++dropped;
    }
    auto it = frames.find(timestamp);
    if (it == frames.end()) {
        Frame frame;
        frame.createdAtMs = nowMs;
        it = frames.insert(timestamp, std::move(frame));
    }
    auto &frame = it.value();
    if ((start && frame.first && *frame.first != sequence) ||
        (marker && frame.last && *frame.last != sequence)) {
        frames.erase(it);
        ++dropped;
        return {};
    }
    if (frame.packets.contains(sequence))
        return {};
    const auto body = payload.mid(offset);
    if (frame.bytes + body.size() > MaxFrameBytes || frame.packets.size() >= MaxFramePackets) {
        frames.erase(it);
        ++dropped;
        return {};
    }
    frame.bytes += body.size();
    frame.packets.insert(sequence, body);
    if (start)
        frame.first = sequence;
    if (marker)
        frame.last = sequence;
    if (!frame.first || !frame.last)
        return {};
    const auto count = unsigned(uint16_t(*frame.last - *frame.first)) + 1;
    if (count > MaxFramePackets) {
        frames.erase(it);
        ++dropped;
        return {};
    }
    for (unsigned i = 0; i < count; ++i)
        if (!frame.packets.contains(uint16_t(*frame.first + i)))
            return {};
    QByteArray complete;
    complete.reserve(frame.bytes);
    for (unsigned i = 0; i < count; ++i)
        complete.append(frame.packets.value(uint16_t(*frame.first + i)));
    frames.erase(it);
    lastCompleted = timestamp;
    for (auto buffered = frames.begin(); buffered != frames.end();) {
        if (int32_t(buffered.key() - timestamp) <= 0) {
            buffered = frames.erase(buffered);
            ++dropped;
        } else {
            ++buffered;
        }
    }
    completed.append(timestamp);
    if (completed.size() > 16)
        completed.removeFirst();
    return complete;
}

} // namespace Acheron::Discord::Voice
