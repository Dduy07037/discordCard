#include "GoLiveSignaling.hpp"

#include <chrono>

namespace Acheron::Core::Audio {
namespace {
qint64 nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::optional<Snowflake> parseId(const QString &text)
{
    if (text.isEmpty() || text.size() > 20)
        return {};
    for (const auto character : text)
        if (character < QLatin1Char('0') || character > QLatin1Char('9'))
            return {};
    bool ok = false;
    const auto id = text.toULongLong(&ok);
    if (!ok || id == 0)
        return {};
    return Snowflake(id);
}
} // namespace

QString StreamKey::toString() const
{
    if (!channelId.isValid() || !ownerId.isValid())
        return {};
    return guildId.isValid()
        ? QStringLiteral("guild:%1:%2:%3").arg(QString::number(guildId), QString::number(channelId), QString::number(ownerId))
        : QStringLiteral("call:%1:%2").arg(QString::number(channelId), QString::number(ownerId));
}

std::optional<StreamKey> StreamKey::parse(const QString &value)
{
    const auto parts = value.split(QLatin1Char(':'));
    const bool guild = parts.size() == 4 && parts[0] == QStringLiteral("guild");
    const bool call = parts.size() == 3 && parts[0] == QStringLiteral("call");
    if (!guild && !call)
        return {};
    StreamKey key;
    if (guild) {
        const auto id = parseId(parts[1]);
        if (!id)
            return {};
        key.guildId = *id;
    }
    const auto channel = parseId(parts[guild ? 2 : 1]);
    const auto owner = parseId(parts[guild ? 3 : 2]);
    if (!channel || !owner)
        return {};
    key.channelId = *channel;
    key.ownerId = *owner;
    return key;
}

GoLiveSignaling::GoLiveSignaling(Snowflake accountId, QObject *parent)
    : QObject(parent), accountId(accountId)
{
    expiryTimer.setParent(this);
    expiryTimer.setInterval(1000);
    connect(&expiryTimer, &QTimer::timeout, this, &GoLiveSignaling::expireRequests);
}

void GoLiveSignaling::setVoiceContext(Snowflake guild, Snowflake channel,
                                    const QString &voiceSessionId, bool connected)
{
    if (guild != guildId || channel != channelId || voiceSessionId != sessionId || !connected)
        stopAll();
    guildId = guild;
    channelId = channel;
    sessionId = voiceSessionId;
    voiceConnected = connected && channel.isValid() && !sessionId.isEmpty();
}

bool GoLiveSignaling::matchesContext(const StreamKey &key) const
{
    return voiceConnected && key.guildId == guildId && key.channelId == channelId;
}

bool GoLiveSignaling::watch(const QString &value)
{
    const auto key = StreamKey::parse(value);
    if (!key || !matchesContext(*key) || key->ownerId == accountId)
        return false;
    const QString canonical = key->toString();
    if (viewerKey == canonical)
        return true;
    if (!viewerKey.isEmpty())
        stop(viewerKey);
    if (!matchesContext(*key))
        return false; // closing the old viewer may have disconnected voice
    viewerKey = canonical;
    pending.insert(canonical, Pending{{}, {}, false, nowMs()});
    expiryTimer.start();
    emit gatewayRequest(20, {{"stream_key", canonical}});
    return true;
}

bool GoLiveSignaling::publish()
{
    if (!voiceConnected || !accountId.isValid())
        return false;
    if (!publisherKey.isEmpty())
        return true;
    publisherKey = StreamKey{guildId, channelId, accountId}.toString();
    pending.insert(publisherKey, Pending{{}, {}, false, nowMs()});
    expiryTimer.start();
    QJsonObject data{{"type", guildId.isValid() ? "guild" : "call"},
                     {"channel_id", QString::number(channelId)}, {"preferred_region", QJsonValue::Null}};
    if (guildId.isValid())
        data.insert("guild_id", QString::number(guildId));
    emit gatewayRequest(18, data);
    emit gatewayRequest(22, {{"stream_key", publisherKey}, {"paused", false}});
    return true;
}

void GoLiveSignaling::stop(QString key)
{
    if (!pending.remove(key))
        return;
    if (publisherKey == key)
        publisherKey.clear();
    if (viewerKey == key)
        viewerKey.clear();
    emit gatewayRequest(19, {{"stream_key", key}});
    emit rtcSessionClosed(key);
    if (pending.isEmpty())
        expiryTimer.stop();
}

void GoLiveSignaling::stopAll()
{
    const auto keys = pending.keys();
    for (const auto &key : keys)
        stop(key);
}

void GoLiveSignaling::handleEvent(const QString &event, const QJsonObject &data)
{
    const QString key = data.value("stream_key").toString();
    if (!pending.contains(key))
        return; // includes responses to old channel/account requests
    auto it = pending.find(key);
    if (event == QStringLiteral("STREAM_CREATE")) {
        it->create = data;
        tryDeliver(key);
    } else if (event == QStringLiteral("STREAM_SERVER_UPDATE")) {
        const bool changed = it->server.value("token") != data.value("token") ||
                             it->server.value("endpoint") != data.value("endpoint");
        const bool reconnect = changed && it->delivered;
        if (reconnect) {
            it->delivered = false;
            it->requestedAtMs = nowMs();
        }
        it->server = data;
        if (reconnect)
            emit rtcSessionClosed(key);
        tryDeliver(key);
    } else if (event == QStringLiteral("STREAM_UPDATE")) {
        emit streamChanged(key, data.value("paused").toBool());
    } else if (event == QStringLiteral("STREAM_DELETE")) {
        pending.remove(key);
        if (publisherKey == key)
            publisherKey.clear();
        if (viewerKey == key)
            viewerKey.clear();
        emit rtcSessionClosed(key);
        if (pending.isEmpty())
            expiryTimer.stop();
    }
}

void GoLiveSignaling::tryDeliver(const QString &key)
{
    auto it = pending.find(key);
    if (it == pending.end() || it->delivered)
        return;
    const auto serverId = parseId(it->create.value("rtc_server_id").toString());
    const auto endpoint = it->server.value("endpoint").toString();
    const auto token = it->server.value("token").toString();
    if (!serverId || endpoint.isEmpty() || token.isEmpty())
        return;
    it->delivered = true;
    QJsonObject connection{{"rtc_server_id", QString::number(*serverId)},
                           {"rtc_channel_id", it->create.value("rtc_channel_id")},
                           {"voice_session_id", sessionId}, {"endpoint", endpoint},
                           {"token", token}, {"publisher", key == publisherKey}};
    emit rtcSessionReady(key, connection);
}

void GoLiveSignaling::expireRequests()
{
    const auto keys = pending.keys();
    for (const auto &key : keys) {
        const auto request = pending.constFind(key);
        if (request != pending.cend() && !request->delivered &&
            nowMs() - request->requestedAtMs >= 20000) {
            stop(key);
            emit requestFailed(key, tr("The stream server did not become ready."));
        }
    }
}

} // namespace Acheron::Core::Audio
