#pragma once

#include <QObject>
#include <QHash>
#include <QJsonObject>
#include <QTimer>
#include <optional>

#include "Core/Snowflake.hpp"

namespace Acheron::Core::Audio {

struct StreamKey
{
    Snowflake guildId;
    Snowflake channelId;
    Snowflake ownerId;
    QString toString() const;
    static std::optional<StreamKey> parse(const QString &value);
};

// Signaling only. A media adapter must consume rtcSessionReady before exposing
// watch/share controls. Voice, viewer and publisher are separate sessions.
class GoLiveSignaling : public QObject
{
    Q_OBJECT
public:
    explicit GoLiveSignaling(Snowflake accountId, QObject *parent = nullptr);
    void setVoiceContext(Snowflake guildId, Snowflake channelId,
                         const QString &voiceSessionId, bool connected);
    bool watch(const QString &streamKey);
    bool publish();
    void stop(QString streamKey);
    void stopAll();
    void handleEvent(const QString &event, const QJsonObject &data);
    QString publishingKey() const { return publisherKey; }
    QString watchingKey() const { return viewerKey; }

signals:
    void gatewayRequest(int opcode, const QJsonObject &data);
    void rtcSessionReady(const QString &streamKey, const QJsonObject &connection);
    void rtcSessionClosed(const QString &streamKey);
    void streamChanged(const QString &streamKey, bool paused);
    void requestFailed(const QString &streamKey, const QString &reason);

private:
    struct Pending {
        QJsonObject create;
        QJsonObject server;
        bool delivered = false;
        qint64 requestedAtMs = 0;
    };
    bool matchesContext(const StreamKey &key) const;
    void tryDeliver(const QString &key);
    void expireRequests();
    Snowflake accountId;
    Snowflake guildId;
    Snowflake channelId;
    QString sessionId;
    bool voiceConnected = false;
    QString publisherKey;
    QString viewerKey;
    QHash<QString, Pending> pending;
    QTimer expiryTimer;
};

} // namespace Acheron::Core::Audio
