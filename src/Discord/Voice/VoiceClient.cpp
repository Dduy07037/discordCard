#include "VoiceClient.hpp"
#include "VoiceGateway.hpp"
#include "UdpTransport.hpp"
#include "VoiceEncryption.hpp"
#include "RtpPacket.hpp"

#include "DaveSession.hpp"
#include <QtEndian>

#include "Core/Audio/IAudioBackend.hpp"
#include "Core/Logging.hpp"

#include <array>
#include <algorithm>
#include <cstring>

namespace Acheron {
namespace Discord {
namespace Voice {

// 20ms at 48khz
static constexpr uint32_t OPUS_FRAME_SAMPLES = 960;

static QString formatDisplayableCode(const std::vector<uint8_t> &data, int bytesToConsume = 30, int groupSize = 5)
{
    int numGroups = bytesToConsume / groupSize;
    if (static_cast<int>(data.size()) < bytesToConsume)
        return {};

    uint64_t modulus = 1;
    for (int i = 0; i < groupSize; i++)
        modulus *= 10;

    QString result;
    for (int g = 0; g < numGroups; g++) {
        uint64_t value = 0;
        for (int b = 0; b < groupSize; b++)
            value = (value << 8) | data[g * groupSize + b];
        value %= modulus;

        if (g > 0)
            result += QLatin1Char(' ');
        result += QStringLiteral("%1").arg(value, groupSize, 10, QLatin1Char('0'));
    }
    return result;
}

VoiceClient::VoiceClient(const QString &endpoint, const QString &token,
                         Core::Snowflake serverId, Core::Snowflake channelId,
                         Core::Snowflake userId, const QString &sessionId,
                         const Core::ProxyConfig &proxy, QObject *parent)
    : QObject(parent),
      endpoint(endpoint),
      token(token),
      proxy(proxy),
      serverId(serverId),
      channelId(channelId),
      userId(userId),
      sessionId(sessionId)
{
}

VoiceClient::~VoiceClient()
{
    stop();
}

void VoiceClient::seedConnectedUsers(const QList<Core::Snowflake> &userIds)
{
    for (auto id : userIds)
        connectedUserIds.insert(std::to_string(id));
}

void VoiceClient::start()
{
    if (currentState != State::Disconnected) {
        qCWarning(LogVoice) << "VoiceClient::start called in non-disconnected state";
        return;
    }

    VoiceEncryption::initialize();

    setState(State::Connecting);

    gateway = new VoiceGateway(endpoint, serverId, channelId, userId, sessionId, token, proxy, this);

    gateway->setVideoSession(videoSession, videoPublisher, canDecodeH264);

    connect(gateway, &VoiceGateway::connected, this, &VoiceClient::onGatewayConnected);
    connect(gateway, &VoiceGateway::disconnected, this, &VoiceClient::onGatewayDisconnected);
    connect(gateway, &VoiceGateway::readyReceived, this, &VoiceClient::onGatewayReady);
    connect(gateway, &VoiceGateway::sessionDescriptionReceived, this, &VoiceClient::onSessionDescription);
    connect(gateway, &VoiceGateway::sessionUpdated, this, &VoiceClient::onSessionUpdate);
    connect(gateway, &VoiceGateway::speakingReceived, this, &VoiceClient::onSpeaking);
    connect(gateway, &VoiceGateway::clientConnected, this, &VoiceClient::onClientConnect);
    connect(gateway, &VoiceGateway::clientsConnected, this, &VoiceClient::onClientsConnect);
    connect(gateway, &VoiceGateway::clientDisconnected, this, &VoiceClient::onClientDisconnect);
    connect(gateway, &VoiceGateway::resumed, this, &VoiceClient::onGatewayResumed);

    connect(gateway, &VoiceGateway::binaryPayloadReceived,
            this, [this](int opcode, const QByteArray &payload) {
                if (!daveSession)
                    return;
                switch (opcode) {
                case static_cast<int>(VoiceOpCode::DAVE_MLS_EXTERNAL_SENDER_PACKAGE):
                    daveSession->onExternalSenderPackage(payload);
                    break;
                case static_cast<int>(VoiceOpCode::DAVE_MLS_PROPOSALS):
                    daveSession->onProposals(payload);
                    break;
                case static_cast<int>(VoiceOpCode::DAVE_MLS_ANNOUNCE_COMMIT_TRANSITION):
                    daveSession->onAnnounceCommitTransition(payload);
                    break;
                case static_cast<int>(VoiceOpCode::DAVE_MLS_WELCOME):
                    daveSession->onWelcome(payload);
                    break;
                default:
                    qCDebug(LogVoice) << "Unhandled DAVE binary opcode:" << opcode;
                    break;
                }
            });
    connect(gateway, &VoiceGateway::daveTransitionPrepare,
            this, [this](int protocolVersion, int transitionId) {
                if (daveSession)
                    daveSession->onPrepareTransition(protocolVersion, transitionId);
            });
    connect(gateway, &VoiceGateway::daveTransitionExecute,
            this, [this](int transitionId) {
                if (daveSession)
                    daveSession->onExecuteTransition(transitionId);
            });
    connect(gateway, &VoiceGateway::daveEpochPrepare,
            this, [this](int protocolVersion, int epoch) {
                if (!daveSession && protocolVersion > 0) {
                    // mid-call upgrade attempt. return cuz onPrepareEpoch also reinits
                    ensureDaveSession(static_cast<uint16_t>(protocolVersion));
                    return;
                }
                if (daveSession)
                    daveSession->onPrepareEpoch(protocolVersion, epoch);
            });

    gateway->start();
}

void VoiceClient::stop()
{
    if (gateway) {
        gateway->hardStop();
        delete gateway;
        gateway = nullptr;
    }

    cleanupTransport();

    localSsrc = 0;
    selectedMode.clear();
    sessionKey.clear();

    connectedUserIds.clear();
    ssrcToUserIdMap.clear();

    setState(State::Disconnected);
}

void VoiceClient::cleanupTransport()
{
    if (videoPacer) {
        videoPacer->stop();
        delete videoPacer;
        videoPacer = nullptr;
    }
    videoFragments.clear();
    videoAssemblers.clear();
    h264Assemblers.clear();
    lastKeyframeRequests.clear();
    selectedVideoCodec.clear();
    remoteVideoSsrcs.clear();
    videoPackets = videoFrames = videoSentFrames = videoDecryptFailures = videoTransportFailures = unknownVideoSources = 0;
    rtxToVideoSsrc.clear();
    videoSequence = 0;
    if (keepaliveTimer) {
        keepaliveTimer->stop();
        delete keepaliveTimer;
        keepaliveTimer = nullptr;
    }

    encryption.reset();

    daveSession.reset();

    delete udpTransport;
    udpTransport = nullptr;

    rtpSequence = 0;
    rtpTimestamp = 0;
    rtpEpoch = {};
}

void VoiceClient::onGatewayConnected()
{
    qCInfo(LogVoice) << "Voice gateway WebSocket connected, waiting for Hello + Identify";
    setState(State::Identifying);
}

void VoiceClient::onGatewayDisconnected(VoiceCloseCode code, const QString &reason)
{
    qCWarning(LogVoice) << "Voice gateway disconnected, code:" << code << "reason:" << reason;

    cleanupTransport();

    // done if not reconnected
    if (currentState != State::Disconnected) {
        setState(State::Disconnected);
        emit disconnected();
    }
}

void VoiceClient::onGatewayReady(const VoiceReady &data)
{
    qCInfo(LogVoice) << "Voice Ready: SSRC =" << data.ssrc
                     << "server =" << data.ip << ":" << data.port
                     << "modes =" << data.modes.get();

    localSsrc = data.ssrc;
    serverIp = data.ip;
    serverPort = data.port;
    serverModes = data.modes;
    localVideoSsrc = localRtxSsrc = 0;
    if (videoSession && !data.streams.isEmpty()) {
        const auto stream = data.streams.first().toObject();
        localVideoSsrc = quint32(stream.value("ssrc").toDouble());
        localRtxSsrc = quint32(stream.value("rtx_ssrc").toDouble());
    }

    static const std::array preferred = {
        EncryptionMode::AEAD_AES256_GCM_RTPSIZE,
        EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE,
    };

    EncryptionMode mode = EncryptionMode::UNKNOWN;
    for (auto candidate : preferred) {
        if (serverModes.contains(encryptionModeToString(candidate)) && VoiceEncryption::isModeAvailable(candidate)) {
            mode = candidate;
            break;
        }
    }

    if (mode == EncryptionMode::UNKNOWN) {
        qCCritical(LogVoice) << "No supported encryption mode found! Server offered:" << serverModes;
        stop();
        return;
    }
    selectedMode = encryptionModeToString(mode);
    qCInfo(LogVoice) << "Selected encryption mode:" << selectedMode;

    setState(State::DiscoveringIP);

    cleanupTransport();

    udpTransport = new UdpTransport(proxy, this);
    connect(udpTransport, &UdpTransport::ipDiscovered, this, &VoiceClient::onIpDiscovered);
    connect(udpTransport, &UdpTransport::ipDiscoveryFailed, this, &VoiceClient::onIpDiscoveryFailed);
    connect(udpTransport, &UdpTransport::datagramReceived, this, &VoiceClient::onDatagram);

    udpTransport->startIpDiscovery(serverIp, serverPort, localSsrc);
}

void VoiceClient::onSessionDescription(const SessionDescription &desc)
{
    qCInfo(LogVoice) << "Session established: mode =" << desc.mode
                     << "key length =" << desc.secretKey->size();

    if (videoSession && !selectVideoCodec(desc.videoCodec.get())) {
        stop();
        return;
    }
    sessionKey = desc.secretKey;
    selectedMode = desc.mode;

    EncryptionMode mode = encryptionModeFromString(selectedMode);
    encryption = std::make_unique<VoiceEncryption>(mode, sessionKey);

    int daveVersion = desc.daveProtocolVersion.hasValue() ? desc.daveProtocolVersion.get() : 0;
    qCInfo(LogVoice) << "dave_protocol_version =" << daveVersion;
    if (videoSession && daveVersion <= 0) {
        emit videoError(tr("This probe requires a DAVE stream session."));
        stop();
        return;
    }

    if (daveVersion > 0)
        ensureDaveSession(static_cast<uint16_t>(daveVersion));

    rtpEpoch = std::chrono::steady_clock::now();

    setState(State::Connected);
    emit connected();

    // send silence so discord sends us audio immediately
    sendSilence();

    if (!keepaliveTimer) {
        keepaliveTimer = new QTimer(this);
        connect(keepaliveTimer, &QTimer::timeout, this, &VoiceClient::sendSilence);
    }
    keepaliveTimer->start(KEEPALIVE_INTERVAL_MS);
}

void VoiceClient::drainAudio()
{
    if (!sendQueue)
        return;
    const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    bool discontinuity = false;
    const auto packets = sendQueue->take(nowMs, discontinuity);
    if (discontinuity)
        newTalkspurt = true;
    for (const auto &packet : packets)
        sendAudio(packet.data, packet.capturedAtMs);
    if (nowMs - lastSendDiagnosticsMs >= 5000) {
        auto ages = sendAges;
        const auto count = std::min<size_t>(sendAgeCount, ages.size());
        std::sort(ages.begin(), ages.begin() + count);
        auto percentile = [&](unsigned p) -> qint64 {
            return count ? ages[(count - 1) * p / 100] : 0;
        };
        emit sendDiagnosticsUpdated({
            {"capture_to_send_p50_ms", double(percentile(50))},
            {"capture_to_send_p95_ms", double(percentile(95))},
            {"capture_to_send_p99_ms", double(percentile(99))},
            {"capture_to_send_samples", double(count)}
        });
        sendAgeCount = 0;
        lastSendDiagnosticsMs = nowMs;
    }
}

void VoiceClient::sendAudio(const QByteArray &opusData, qint64 capturedAtMs)
{
    if (currentState != State::Connected || !encryption || !udpTransport)
        return;

    auto now = std::chrono::steady_clock::now();
    if (capturedAtMs >= 0) {
        const qint64 nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     now.time_since_epoch())
                                     .count();
        if (nowMs - capturedAtMs > MAX_CAPTURE_QUEUE_AGE_MS) {
            // Sending delayed capture frames in a burst sounds robotic at the
            // receiver. Drop stale speech and re-anchor the next fresh frame.
            newTalkspurt = true;
            return;
        }
    }

    // snap rtp timestamp back to wall clock after a period of silence
    // otherwise its a little behind and it will be played back delayed by discord
    if (newTalkspurt) {
        const auto epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            rtpEpoch.time_since_epoch()).count();
        const auto sampleTimeMs = capturedAtMs >= 0 ? capturedAtMs :
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        rtpTimestamp = static_cast<uint32_t>(std::max<qint64>(0, sampleTimeMs - epochMs) * 48);
    } else {
        rtpTimestamp += OPUS_FRAME_SAMPLES;
    }

    RtpHeader header;
    header.payloadType = 120;
    header.marker = newTalkspurt;
    header.sequence = rtpSequence++;
    header.timestamp = rtpTimestamp;
    header.ssrc = localSsrc;

    newTalkspurt = false;

    QByteArray payloadForTransport = opusData;

    if (isDaveEnabled()) {
        auto *enc = daveSession->encryptor();
        auto maxSize = enc->GetMaxCiphertextByteSize(discord::dave::MediaType::Audio, payloadForTransport.size());
        std::vector<uint8_t> daveEncrypted(maxSize);
        size_t bytesWritten = 0;
        auto result = enc->Encrypt(
                discord::dave::MediaType::Audio,
                localSsrc,
                discord::dave::ArrayView<const uint8_t>(
                        reinterpret_cast<const uint8_t *>(payloadForTransport.constData()),
                        payloadForTransport.size()),
                discord::dave::ArrayView<uint8_t>(daveEncrypted.data(), daveEncrypted.size()),
                &bytesWritten);

        if (result == discord::dave::IEncryptor::Success) {
            payloadForTransport = QByteArray(reinterpret_cast<const char *>(daveEncrypted.data()), bytesWritten);
        } else {
            qCWarning(LogVoice) << "DAVE encrypt failed: result =" << static_cast<int>(result);
            return;
        }
    }

    QByteArray headerBytes = header.serialize();
    QByteArray encryptedSection = encryption->encrypt(headerBytes, payloadForTransport);
    if (encryptedSection.isEmpty())
        return;

    QByteArray packet = headerBytes + encryptedSection;
    udpTransport->send(packet);

    if (capturedAtMs >= 0 && (opusData.size() != sizeof(Core::Audio::OPUS_SILENCE) ||
            std::memcmp(opusData.constData(), Core::Audio::OPUS_SILENCE, sizeof(Core::Audio::OPUS_SILENCE)) != 0)) {
        const auto sentAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        sendAges[sendAgeCount++ % sendAges.size()] = sentAtMs - capturedAtMs;
    }

    lastAudioSendTime = now;
}

bool VoiceClient::selectVideoCodec(const QString &name)
{
    const auto codec = name.toUpper();
    if (codec != QStringLiteral("VP8") &&
        (videoPublisher || !canDecodeH264 || codec != QStringLiteral("H264"))) {
        emit videoError(tr("Unsupported stream codec: %1. This build supports VP8 and H264 viewing.").arg(name));
        return false;
    }
    if (selectedVideoCodec == codec) return true;
    selectedVideoCodec = codec;
    videoAssemblers.clear();
    h264Assemblers.clear();
    lastKeyframeRequests.clear();
    emit videoCodecChanged(codec);
    return true;
}

void VoiceClient::onSessionUpdate(const QJsonObject &data)
{
    if (!videoSession || !data.contains("video_codec")) return;
    if (!selectVideoCodec(data.value("video_codec").toString())) { stop(); return; }
    if (!videoPublisher) advertiseVideo();
}

void VoiceClient::configureVideoSession(Core::Snowflake groupId, bool publisher, bool desktop, int fps,
                                       bool h264Decode, QSize resolution, int bitrate)
{
    Q_ASSERT(currentState == State::Disconnected);
    videoSession = true;
    videoPublisher = publisher;
    desktopVideo = desktop;
    videoFps = qBound(15, fps, 60);
    videoResolution = QSize(qBound(2, resolution.width(), 1920), qBound(2, resolution.height(), 1080));
    videoBitrate = qBound(100000, bitrate, 20000000);
    canDecodeH264 = h264Decode;
    daveGroupId = groupId;
}

void VoiceClient::advertiseVideo()
{
    if (!gateway || !videoSession || currentState != State::Connected)
        return;
    if (!videoPublisher) {
        QJsonObject wants{{"any", 100}};
        for (const auto ssrc : remoteVideoSsrcs)
            wants.insert(QString::number(ssrc), 100);
        gateway->sendMediaSinkWants(wants);
        for (const auto ssrc : remoteVideoSsrcs) requestVideoKeyframe(ssrc);
        return;
    }
    if (!localVideoSsrc) {
        emit videoError(tr("The stream server did not assign a video SSRC."));
        return;
    }
    const QJsonObject stream{{"type", "video"}, {"rid", "100"}, {"ssrc", qint64(localVideoSsrc)},
        {"rtx_ssrc", qint64(localRtxSsrc)}, {"active", true}, {"quality", 100},
        {"max_bitrate", desktopVideo ? videoBitrate : 600000},
        {"max_framerate", desktopVideo ? videoFps : 15},
        {"max_resolution", QJsonObject{{"type", "fixed"}, {"width", desktopVideo ? videoResolution.width() : 640},
                                      {"height", desktopVideo ? videoResolution.height() : 360}}}};
    gateway->sendVideoState({{"audio_ssrc", qint64(localSsrc)}, {"video_ssrc", qint64(localVideoSsrc)},
        {"rtx_ssrc", qint64(localRtxSsrc)}, {"streams", QJsonArray{stream}}});
}

void VoiceClient::updateVideoSettings(QSize resolution, int fps, int bitrate)
{
    if (!videoSession || !videoPublisher || !videoFragments.isEmpty()) return;
    videoResolution = resolution;
    videoFps = qBound(15, fps, 60);
    videoBitrate = qBound(100000, bitrate, 20000000);
    advertiseVideo();
}

void VoiceClient::requestVideoKeyframe(quint32 ssrc)
{
    if (!videoSession || videoPublisher || !remoteVideoSsrcs.contains(ssrc) ||
        currentState != State::Connected || !encryption || !udpTransport) return;
    const qint64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (lastKeyframeRequests.contains(ssrc) && now - lastKeyframeRequests.value(ssrc) < 1000) return;
    // Native RTCP uses eight clear authenticated bytes. The target SSRC is
    // transport encrypted; RFC 4585 PLI carries no FCI body.
    QByteArray header = QByteArray::fromHex("81ce000200000000");
    qToBigEndian(localSsrc, reinterpret_cast<uchar *>(header.data() + 4));
    QByteArray body(4, '\0');
    qToBigEndian(ssrc, reinterpret_cast<uchar *>(body.data()));
    const auto encrypted = encryption->encrypt(header, body);
    if (encrypted.isEmpty()) return;
    udpTransport->send(header + encrypted);
    lastKeyframeRequests.insert(ssrc, now);
}

QJsonObject VoiceClient::videoDiagnostics() const
{
    return {{"codec", selectedVideoCodec}, {"dave_ready", isDaveEnabled()},
        {"sources", remoteVideoSsrcs.size()}, {"packets", qint64(videoPackets)},
        {"frames", qint64(videoFrames)}, {"transport_errors", qint64(videoTransportFailures)},
        {"sent_frames", qint64(videoSentFrames)},
        {"dave_errors", qint64(videoDecryptFailures)}, {"unknown_sources", qint64(unknownVideoSources)}};
}

bool VoiceClient::canSendVideoFrame() const
{
    return videoSession && videoPublisher && localVideoSsrc && isDaveEnabled() &&
        currentState == State::Connected && encryption && udpTransport && videoFragments.isEmpty();
}

bool VoiceClient::sendVideoFrame(const QByteArray &frame, uint32_t timestamp)
{
    // Do not send plaintext video during MLS setup. The probe waits for DAVE.
    if (!videoSession || !videoPublisher || !localVideoSsrc || !isDaveEnabled() ||
        currentState != State::Connected || !encryption || !udpTransport ||
        !videoFragments.isEmpty() || frame.isEmpty() || frame.size() > 512 * 1024)
        return false;
    auto *enc = daveSession->encryptor();
    QByteArray ciphertext(int(enc->GetMaxCiphertextByteSize(discord::dave::MediaType::Video, frame.size())), '\0');
    size_t written = 0;
    const auto result = enc->Encrypt(discord::dave::MediaType::Video, localVideoSsrc,
        discord::dave::ArrayView<const uint8_t>(reinterpret_cast<const uint8_t *>(frame.constData()), frame.size()),
        discord::dave::ArrayView<uint8_t>(reinterpret_cast<uint8_t *>(ciphertext.data()), ciphertext.size()), &written);
    if (result != discord::dave::IEncryptor::Success)
        return false;
    ciphertext.resize(int(written));
    videoFragments = packetizeVp8(ciphertext);
    videoFragmentIndex = 0;
    pendingVideoTimestamp = timestamp;
    pendingVideoAtMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (!videoPacer) {
        videoPacer = new QTimer(this);
        videoPacer->setTimerType(Qt::PreciseTimer);
        videoPacer->setInterval(2);
        connect(videoPacer, &QTimer::timeout, this, [this] {
            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (nowMs - pendingVideoAtMs > 150 || !encryption || !udpTransport) {
                videoFragments.clear();
                videoPacer->stop();
                emit videoKeyframeRequested();
                emit videoFrameFinished(false);
                return;
            }
            // Bounded pacing, never queue another encoded frame behind this one.
            // Give high-quality desktop keyframes enough bounded burst capacity;
            // two packets per tick previously capped throughput near 8 Mbps.
            const int burstPackets = desktopVideo ? 8 : 2;
            for (int i = 0; i < burstPackets && videoFragmentIndex < videoFragments.size(); ++i) {
                RtpHeader header;
                header.payloadType = 103;
                header.sequence = videoSequence++;
                header.timestamp = pendingVideoTimestamp;
                header.ssrc = localVideoSsrc;
                header.marker = videoFragmentIndex + 1 == videoFragments.size();
                const auto bytes = header.serialize();
                const auto encrypted = encryption->encrypt(bytes, videoFragments[videoFragmentIndex++]);
                if (!encrypted.isEmpty())
                    udpTransport->send(bytes + encrypted);
            }
            if (videoFragmentIndex >= videoFragments.size()) {
                videoFragments.clear();
                videoPacer->stop();
                ++videoSentFrames;
                emit videoFrameFinished(true);
            }
        });
    }
    videoPacer->start();
    return true;
}

void VoiceClient::setSpeaking(bool speaking)
{
    if (!gateway)
        return;

    if (speaking)
        newTalkspurt = true;

    int flags = speaking ? (videoSession ? 2 : static_cast<int>(SpeakingFlag::MICROPHONE)) : 0;
    gateway->sendSpeaking(flags, 0, localSsrc);
}

void VoiceClient::sendSilence()
{
    if (currentState != State::Connected || !encryption || !udpTransport)
        return;

    // no need to keepalive by sending silence if we spoke recently
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastAudioSendTime);
    if (elapsed.count() < 5)
        return;

    QByteArray silencePayload(reinterpret_cast<const char *>(Core::Audio::OPUS_SILENCE), sizeof(Core::Audio::OPUS_SILENCE));
    sendAudio(silencePayload);

    qCDebug(LogVoice) << "Sent keepalive silence frame";
}

void VoiceClient::onDatagram(const QByteArray &data)
{
    if (data.size() < RtpHeader::FIXED_SIZE)
        return;

    const auto *p = reinterpret_cast<const uint8_t *>(data.constData());

    // rtp version = 2
    if (((p[0] >> 6) & 0x03) != 2)
        return;

    // RTP/RTCP are multiplexed. A feedback packet is not video RTP.
    if (p[1] >= 192 && p[1] <= 223) {
        if (videoSession && videoPublisher && p[1] == 206 && (p[0] & 0x1f) == 1 && encryption) {
            const auto body = encryption->decrypt(data.left(8), data.mid(8));
            if (body.size() == 4 && qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(body.constData())) == localVideoSsrc)
                emit videoKeyframeRequested();
        }
        return;
    }

    // These payload numbers match our advertised native UDP codec list.
    const uint8_t payloadType = p[1] & 0x7F;
    const bool h264 = selectedVideoCodec == QStringLiteral("H264");
    const bool video = videoSession && (h264 ? (payloadType == 101 || payloadType == 102)
                                                         : (payloadType == 103 || payloadType == 104));
    if (payloadType != 120 && !video)
        return;
    if (video) ++videoPackets;

    // rtp header and extension header are unencrypted and used for aad in rtpsize.
    // account for CSRC entries between fixed header and extension header.
    int csrcCount = p[0] & 0x0F;
    bool hasExtension = (p[0] >> 4) & 1;
    int headerSize = RtpHeader::FIXED_SIZE + csrcCount * 4 + (hasExtension ? 4 : 0);

    if (data.size() <= headerSize + 4)
        return;

    RtpHeader header = RtpHeader::parse(data);

    // ignore our own packets
    if (header.ssrc == localSsrc)
        return;

    QByteArray rtpHeaderBytes = data.left(headerSize);
    QByteArray encryptedSection = data.mid(headerSize);

    if (!encryption) {
        qCDebug(LogVoice) << "Received RTP but no encryption context, SSRC =" << header.ssrc;
        return;
    }

    QByteArray decrypted = encryption->decrypt(rtpHeaderBytes, encryptedSection);
    if (decrypted.isEmpty()) {
        if (video) ++videoTransportFailures;
        qCDebug(LogVoice) << "Decrypt failed: SSRC =" << header.ssrc
                          << "seq =" << header.sequence
                          << "pktSize =" << data.size()
                          << "hdrSize =" << headerSize
                          << "ext =" << hasExtension
                          << "encSize =" << encryptedSection.size();
        return;
    }

    if (hasExtension) {
        int extOffset = RtpHeader::FIXED_SIZE + csrcCount * 4;
        if (data.size() < extOffset + 4)
            return;
        uint16_t extWords = (p[extOffset + 2] << 8) | p[extOffset + 3];
        int extBytes = extWords * 4;
        if (decrypted.size() <= extBytes)
            return;
        decrypted = decrypted.mid(extBytes);
    }

    if (header.padding) {
        const auto padding = quint8(decrypted.back());
        if (padding == 0 || padding >= decrypted.size())
            return;
        decrypted.chop(padding);
    }

    if (video) {
        quint32 mediaSsrc = header.ssrc;
        uint16_t sequence = header.sequence;
        if (payloadType == 104 || payloadType == 102) {
            if (decrypted.size() < 3 || !ssrcToUserIdMap.contains(header.ssrc))
                return;
            sequence = (quint8(decrypted[0]) << 8) | quint8(decrypted[1]);
            decrypted.remove(0, 2);
            // RTX SSRCs are mapped from SESSION_UPDATE, not guessed.
            mediaSsrc = rtxToVideoSsrc.value(header.ssrc, 0);
            if (!mediaSsrc)
                return;
        }
        const auto owner = ssrcToUserIdMap.value(mediaSsrc, 0);
        if (!owner) { ++unknownVideoSources; return; }
        if ((!h264 && !videoAssemblers.contains(mediaSsrc) && videoAssemblers.size() >= 4) ||
            (h264 && !h264Assemblers.contains(mediaSsrc) && h264Assemblers.size() >= 4)) return;
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        const auto dropsBefore = h264 ? h264Assemblers[mediaSsrc].droppedFrames() : videoAssemblers[mediaSsrc].droppedFrames();
        auto frame = h264 ? h264Assemblers[mediaSsrc].push(sequence, header.timestamp, header.marker, decrypted, nowMs)
                         : videoAssemblers[mediaSsrc].push(sequence, header.timestamp, header.marker, decrypted, nowMs);
        const auto dropsAfter = h264 ? h264Assemblers[mediaSsrc].droppedFrames() : videoAssemblers[mediaSsrc].droppedFrames();
        if (dropsAfter > dropsBefore) requestVideoKeyframe(mediaSsrc);
        if (!frame || !daveSession)
            return;
        auto *dec = daveSession->getOrCreateDecryptor(mediaSsrc, owner);
        QByteArray plaintext(int(dec->GetMaxPlaintextByteSize(discord::dave::MediaType::Video, frame->size())), '\0');
        size_t written = 0;
        const auto result = dec->Decrypt(discord::dave::MediaType::Video,
            discord::dave::ArrayView<const uint8_t>(reinterpret_cast<const uint8_t *>(frame->constData()), frame->size()),
            discord::dave::ArrayView<uint8_t>(reinterpret_cast<uint8_t *>(plaintext.data()), plaintext.size()), &written);
        if (result != discord::dave::IDecryptor::Success) {
            ++videoDecryptFailures;
            requestVideoKeyframe(mediaSsrc);
            return;
        }
        plaintext.resize(int(written));
        ++videoFrames;
        emit videoReceived(mediaSsrc, header.timestamp, plaintext);
        return;
    }

    if (daveSession) {
        // https://daveprotocol.com/#silence-packets
        if (decrypted.size() == sizeof(Core::Audio::OPUS_SILENCE) && memcmp(decrypted.constData(), Core::Audio::OPUS_SILENCE, sizeof(Core::Audio::OPUS_SILENCE)) == 0) {
            emit audioReceived(header.ssrc, header.sequence, header.timestamp, decrypted);
            return;
        }

        uint64_t ssrcUid = ssrcToUserIdMap.value(header.ssrc, 0);
        auto *dec = daveSession->getOrCreateDecryptor(header.ssrc, ssrcUid);
        auto maxSize = dec->GetMaxPlaintextByteSize(discord::dave::MediaType::Audio, decrypted.size());
        std::vector<uint8_t> davePlaintext(maxSize);
        size_t bytesWritten = 0;
        auto result = dec->Decrypt(
                discord::dave::MediaType::Audio,
                discord::dave::ArrayView<const uint8_t>(
                        reinterpret_cast<const uint8_t *>(decrypted.constData()),
                        decrypted.size()),
                discord::dave::ArrayView<uint8_t>(davePlaintext.data(), davePlaintext.size()),
                &bytesWritten);

        if (result == discord::dave::IDecryptor::Success) {
            decrypted = QByteArray(reinterpret_cast<const char *>(davePlaintext.data()), bytesWritten);
        } else {
            static constexpr const char *kResultNames[] = {
                "Success",
                "DecryptionFailure",
                "MissingKeyRatchet",
                "InvalidNonce",
                "MissingCryptor",
            };
            int ri = static_cast<int>(result);
            const char *rn = (ri >= 0 && ri < 5) ? kResultNames[ri] : "Unknown";
            bool hasMagic = decrypted.size() >= 2 && static_cast<uint8_t>(decrypted[decrypted.size() - 2]) == 0xFA && static_cast<uint8_t>(decrypted[decrypted.size() - 1]) == 0xFA;
            qCDebug(LogDave) << "DAVE decrypt failed: SSRC =" << header.ssrc
                             << "result =" << rn
                             << "frameSize =" << decrypted.size()
                             << "hasMagicMarker =" << hasMagic;
            return;
        }
    }

    emit audioReceived(header.ssrc, header.sequence, header.timestamp, decrypted);
}

bool VoiceClient::isDaveEnabled() const
{
    return daveSession && daveSession->isDaveEnabled();
}

void VoiceClient::requestVerificationCode(Core::Snowflake targetUserId,
                                          std::function<void(const QString &)> callback)
{
    if (!isDaveEnabled()) {
        callback(QString());
        return;
    }
    std::string uid = std::to_string(targetUserId);
    daveSession->getPairwiseFingerprint(uid,
                                        [cb = std::move(callback)](const std::vector<uint8_t> &fingerprint) {
                                            if (fingerprint.empty()) {
                                                cb(QString());
                                                return;
                                            }
                                            cb(formatDisplayableCode(fingerprint, 45));
                                        });
}

void VoiceClient::onSpeaking(const SpeakingData &data)
{
    if (data.userId.hasValue() && data.userId->isValid()) {
        std::string uid = std::to_string(data.userId.get());
        connectedUserIds.insert(uid);
        if (data.ssrc.get() != 0)
            ssrcToUserIdMap.insert(data.ssrc, data.userId.get());
        if (daveSession) {
            daveSession->addConnectedUser(uid);
            if (data.ssrc.get() != 0)
                daveSession->applyKeyRatchetForSsrc(data.ssrc, data.userId.get());
        }
    }
    emit speakingReceived(data);
}

void VoiceClient::onClientsConnect(const QStringList &userIds)
{
    for (const auto &id : userIds) {
        std::string uid = id.toStdString();
        connectedUserIds.insert(uid);
        if (daveSession)
            daveSession->addConnectedUser(uid);
    }
}

void VoiceClient::onClientConnect(const ClientConnectData &data)
{
    if (data.userId.hasValue() && data.userId->isValid()) {
        std::string uid = std::to_string(data.userId.get());
        connectedUserIds.insert(uid);
        QSet<quint32> announced;
        if (data.videoSsrc.get()) announced.insert(data.videoSsrc);
        for (const auto &entry : data.streams) {
            const auto ssrc = quint32(entry.toObject().value("ssrc").toDouble());
            if (ssrc) announced.insert(ssrc);
        }
        // Preserve live reassembly and PLI throttling on repeated Video state
        // messages; retire only this user's obsolete sources.
        for (auto it = remoteVideoSsrcs.begin(); it != remoteVideoSsrcs.end();) {
            if (ssrcToUserIdMap.value(*it, 0) == quint64(data.userId.get()) && !announced.contains(*it)) {
                videoAssemblers.remove(*it);
                h264Assemblers.remove(*it);
                lastKeyframeRequests.remove(*it);
                ssrcToUserIdMap.remove(*it);
                it = remoteVideoSsrcs.erase(it);
            } else ++it;
        }
        for (auto it = rtxToVideoSsrc.begin(); it != rtxToVideoSsrc.end();) {
            if (ssrcToUserIdMap.value(it.key(), 0) == quint64(data.userId.get()) && !announced.contains(it.value())) {
                ssrcToUserIdMap.remove(it.value());
                ssrcToUserIdMap.remove(it.key());
                it = rtxToVideoSsrc.erase(it);
            } else ++it;
        }
        if (data.audioSsrc.get() != 0)
            ssrcToUserIdMap.insert(data.audioSsrc, data.userId.get());
        if (data.videoSsrc.get() != 0) {
            ssrcToUserIdMap.insert(data.videoSsrc, data.userId.get());
            remoteVideoSsrcs.insert(data.videoSsrc);
        }
        for (const auto &entry : data.streams) {
            const auto stream = entry.toObject();
            const auto videoSsrc = quint32(stream.value("ssrc").toDouble());
            const auto rtxSsrc = quint32(stream.value("rtx_ssrc").toDouble());
            if (videoSsrc) {
                ssrcToUserIdMap.insert(videoSsrc, data.userId.get());
                if (stream.value("active").toBool(true)) remoteVideoSsrcs.insert(videoSsrc);
                else remoteVideoSsrcs.remove(videoSsrc);
                // Native Discord omits rtx_ssrc on some layers; its documented
                // default is the primary SSRC plus one, unless explicitly set.
                const auto repairSsrc = rtxSsrc ? rtxSsrc : videoSsrc + 1;
                if (repairSsrc) {
                    ssrcToUserIdMap.insert(repairSsrc, data.userId.get());
                    rtxToVideoSsrc.insert(repairSsrc, videoSsrc);
                }
            }
        }
        if (daveSession) {
            daveSession->addConnectedUser(uid);
            for (auto it = ssrcToUserIdMap.cbegin(); it != ssrcToUserIdMap.cend(); ++it)
                if (it.value() == quint64(data.userId.get()))
                    daveSession->applyKeyRatchetForSsrc(it.key(), it.value());
        }
        if (videoSession && !videoPublisher)
            advertiseVideo();
    }
    emit clientConnected(data);
}

void VoiceClient::onClientDisconnect(Core::Snowflake uid)
{
    std::string uidStr = std::to_string(uid);
    connectedUserIds.erase(uidStr);

    if (daveSession)
        daveSession->removeConnectedUser(uidStr);

    for (auto it = ssrcToUserIdMap.begin(); it != ssrcToUserIdMap.end();) {
        if (it.value() == static_cast<quint64>(uid)) {
            videoAssemblers.remove(it.key());
            h264Assemblers.remove(it.key());
            remoteVideoSsrcs.remove(it.key());
            lastKeyframeRequests.remove(it.key());
            rtxToVideoSsrc.remove(it.key());
            it = ssrcToUserIdMap.erase(it);
        } else {
            ++it;
        }
    }

    emit clientDisconnected(uid);
}

void VoiceClient::onGatewayResumed()
{
    qCInfo(LogVoice) << "Voice session resumed, restoring to Connected state";

    if (localSsrc == 0 || sessionKey.isEmpty() || !encryption || !udpTransport) {
        cleanupTransport();
        if (currentState != State::Disconnected) {
            setState(State::Disconnected);
            emit disconnected();
        }
        return;
    }

    rtpEpoch = std::chrono::steady_clock::now();
    setState(State::Connected);

    sendSilence();
    if (!keepaliveTimer) {
        keepaliveTimer = new QTimer(this);
        connect(keepaliveTimer, &QTimer::timeout, this, &VoiceClient::sendSilence);
    }
    if (!keepaliveTimer->isActive())
        keepaliveTimer->start(KEEPALIVE_INTERVAL_MS);
}

void VoiceClient::onIpDiscovered(const QString &ip, int port)
{
    qCInfo(LogVoice) << "IP Discovery: external" << ip << ":" << port;

    setState(State::SelectingProtocol);

    gateway->sendSelectProtocol(ip, port, selectedMode);

    setState(State::WaitingForSession);
}

void VoiceClient::onIpDiscoveryFailed(const QString &error)
{
    qCCritical(LogVoice) << "IP Discovery failed:" << error;
    stop();
}

void VoiceClient::ensureDaveSession(uint16_t protocolVersion)
{
    if (daveSession)
        return;

    qCInfo(LogVoice) << "Creating DAVE session, protocol version =" << protocolVersion;

    daveSession = std::make_unique<DaveSession>(videoSession ? daveGroupId : channelId, userId, ssrcToUserIdMap, this);
    daveSession->setLocalSsrc(localSsrc);
    if (videoSession)
        daveSession->setVideoSsrc(localVideoSsrc);

    for (const auto &uid : connectedUserIds)
        daveSession->addConnectedUser(uid);

    // binary
    connect(daveSession.get(), &DaveSession::sendKeyPackage,
            gateway, &VoiceGateway::sendBinaryPayload);
    connect(daveSession.get(), &DaveSession::sendCommitWelcome,
            gateway, &VoiceGateway::sendBinaryPayload);

    // json
    connect(daveSession.get(), &DaveSession::sendReadyForTransition,
            gateway, &VoiceGateway::sendDaveReadyForTransition);
    connect(daveSession.get(), &DaveSession::sendInvalidCommitWelcome,
            gateway, &VoiceGateway::sendDaveInvalidCommitWelcome);

    connect(daveSession.get(), &DaveSession::daveStateChanged,
            this, [this](bool enabled) {
                if (enabled) {
                    auto auth = daveSession->lastEpochAuthenticator();
                    if (!auth.empty())
                        emit privacyCodeChanged(formatDisplayableCode(auth));
                    else
                        emit privacyCodeChanged(QString());
                } else {
                    emit privacyCodeChanged(QString());
                }
            });

    daveSession->init(protocolVersion);
}

void VoiceClient::setState(State state)
{
    if (currentState == state)
        return;

    qCDebug(LogVoice) << "VoiceClient state:" << currentState.load() << "->" << state;
    currentState = state;
    emit stateChanged(state);
}

} // namespace Voice
} // namespace Discord
} // namespace Acheron
