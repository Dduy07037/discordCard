#pragma once

#include <QObject>
#include <QTimer>
#include <QSet>
#include <QSize>

#include <atomic>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "Core/ProxyConfig.hpp"
#include "Core/Snowflake.hpp"
#include "VoiceEnums.hpp"
#include "Core/Audio/AudioSendQueue.hpp"
#include "VoiceEntities.hpp"
#include "Vp8Rtp.hpp"
#include "H264Rtp.hpp"

namespace Acheron {
namespace Discord {
namespace Voice {

class VoiceGateway;
class UdpTransport;
class VoiceEncryption;
class DaveSession;

class VoiceClient : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Disconnected,
        Connecting,
        Identifying,
        WaitingForReady,
        DiscoveringIP,
        SelectingProtocol,
        WaitingForSession,
        Connected,
    };
    Q_ENUM(State)

    VoiceClient(const QString &endpoint, const QString &token, Core::Snowflake serverId,
                Core::Snowflake channelId, Core::Snowflake userId, const QString &sessionId,
                const Core::ProxyConfig &proxy, QObject *parent = nullptr);
    ~VoiceClient() override;

    void start();
    void stop();

    [[nodiscard]] State state() const { return currentState; }
    [[nodiscard]] quint32 ssrc() const { return localSsrc; }
    [[nodiscard]] const QString &encryptionMode() const { return selectedMode; }
    [[nodiscard]] const QByteArray &secretKey() const { return sessionKey; }

    void seedConnectedUsers(const QList<Core::Snowflake> &userIds);

    void sendAudio(const QByteArray &opusData, qint64 capturedAtMs = -1);
    void setSendQueue(const std::shared_ptr<Core::Audio::AudioSendQueue> &queue) { sendQueue = queue; }
    void drainAudio();

    void setSpeaking(bool speaking);
    // Experimental Go Live adapter: call before start(), on its own worker.
    void configureVideoSession(Core::Snowflake daveGroupId, bool publisher, bool desktop = false,
                               int fps = 30, bool h264Decode = false,
                               QSize resolution = QSize(1280, 720), int bitrate = 3000000);
    bool sendVideoFrame(const QByteArray &vp8Frame, uint32_t timestamp);
    bool canSendVideoFrame() const;
    void advertiseVideo();
    void requestVideoKeyframe(quint32 ssrc);
    QJsonObject videoDiagnostics() const;

    bool isDaveEnabled() const;
    void requestVerificationCode(Core::Snowflake targetUserId, std::function<void(const QString &)> callback);

signals:
    void stateChanged(State newState);
    void connected();
    void disconnected();

    void speakingReceived(const SpeakingData &data);
    void clientConnected(const ClientConnectData &data);
    void clientDisconnected(Core::Snowflake userId);

    void audioReceived(quint32 ssrc, uint16_t sequence, uint32_t timestamp, const QByteArray &opusData);

    void videoReceived(quint32 ssrc, uint32_t timestamp, const QByteArray &vp8Frame);
    void videoError(const QString &reason);
    void videoCodecChanged(const QString &codec);
    void videoKeyframeRequested();
    void privacyCodeChanged(const QString &code);
    void sendDiagnosticsUpdated(const QJsonObject &stats);

private slots:
    void onGatewayConnected();
    void onGatewayDisconnected(VoiceCloseCode code, const QString &reason);
    void onGatewayReady(const VoiceReady &data);
    void onSessionDescription(const SessionDescription &desc);
    void onSessionUpdate(const QJsonObject &data);
    void onSpeaking(const SpeakingData &data);
    void onClientsConnect(const QStringList &userIds);
    void onClientConnect(const ClientConnectData &data);
    void onClientDisconnect(Core::Snowflake userId);
    void onGatewayResumed();
    void onIpDiscovered(const QString &ip, int port);
    void onIpDiscoveryFailed(const QString &error);
    void onDatagram(const QByteArray &data);

private:
    void setState(State state);
    void sendSilence();
    void cleanupTransport();
    void ensureDaveSession(uint16_t protocolVersion);
    bool selectVideoCodec(const QString &codec);

private:
    bool videoSession = false;
    bool videoPublisher = false;
    Core::Snowflake daveGroupId;
    quint32 localVideoSsrc = 0;
    bool desktopVideo = false;
    bool canDecodeH264 = false;
    int videoFps = 30;
    QSize videoResolution{1280, 720};
    int videoBitrate = 3000000;
    QString selectedVideoCodec;
    quint64 videoPackets = 0, videoFrames = 0, videoSentFrames = 0, videoDecryptFailures = 0, videoTransportFailures = 0, unknownVideoSources = 0;
    QSet<quint32> remoteVideoSsrcs;
    QHash<quint32, qint64> lastKeyframeRequests;
    quint32 localRtxSsrc = 0;
    uint16_t videoSequence = 0;
    QHash<quint32, Vp8Reassembler> videoAssemblers;
    QHash<quint32, H264Reassembler> h264Assemblers;
    QHash<quint32, quint32> rtxToVideoSsrc;
    QList<QByteArray> videoFragments;
    uint32_t pendingVideoTimestamp = 0;
    int videoFragmentIndex = 0;
    qint64 pendingVideoAtMs = 0;
    QTimer *videoPacer = nullptr;
    std::shared_ptr<Core::Audio::AudioSendQueue> sendQueue;
    VoiceGateway *gateway = nullptr;
    UdpTransport *udpTransport = nullptr;

    QString endpoint;
    QString token;
    Core::ProxyConfig proxy;
    Core::Snowflake serverId;
    Core::Snowflake channelId;
    Core::Snowflake userId;
    QString sessionId;

    std::atomic<State> currentState{ State::Disconnected };

    // ready
    quint32 localSsrc = 0;
    QString serverIp;
    int serverPort = 0;
    QStringList serverModes;

    // protocol selection
    QString selectedMode;
    QByteArray sessionKey;

    std::unique_ptr<VoiceEncryption> encryption;
    QTimer *keepaliveTimer = nullptr;
    uint16_t rtpSequence = 0;
    uint32_t rtpTimestamp = 0;

    std::chrono::steady_clock::time_point rtpEpoch;
    std::chrono::steady_clock::time_point lastAudioSendTime;
    bool newTalkspurt = false;
    std::array<qint64, 256> sendAges{};
    unsigned sendAgeCount = 0;
    qint64 lastSendDiagnosticsMs = 0;

    std::unique_ptr<DaveSession> daveSession;
    std::set<std::string> connectedUserIds;
    QHash<quint32, uint64_t> ssrcToUserIdMap;

    static constexpr int KEEPALIVE_INTERVAL_MS = 10000;
    static constexpr qint64 MAX_CAPTURE_QUEUE_AGE_MS = Core::Audio::AudioSendQueue::MaxAgeMs;
};

} // namespace Voice
} // namespace Discord
} // namespace Acheron
