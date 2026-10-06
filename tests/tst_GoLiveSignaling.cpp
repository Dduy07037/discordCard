#include <QTest>
#include <QSignalSpy>

#include "Core/Audio/GoLiveSignaling.hpp"

using namespace Acheron::Core;
using namespace Acheron::Core::Audio;

class TestGoLiveSignaling : public QObject
{
    Q_OBJECT
private slots:
    void validatesKeys();
    void rejectsReplacementIfVoiceClosesDuringStop();
    void waitsForBothEventsInEitherOrder();
    void keepsPublisherAndViewerSeparate();
    void rejectsCrossChannelAndOwnStream();
    void ignoresStaleResponsesAfterChannelMove();
    void replacesViewerWithoutDeletingPublisher();
    void reconnectsWhenTokenChanges();
    void serverDeleteDoesNotSendAnotherDelete();
};

void TestGoLiveSignaling::rejectsReplacementIfVoiceClosesDuringStop()
{
    GoLiveSignaling signaling(Snowflake(3));
    signaling.setVoiceContext(Snowflake(1), Snowflake(2), "session", true);
    QVERIFY(signaling.watch("guild:1:2:4"));
    connect(&signaling, &GoLiveSignaling::rtcSessionClosed, &signaling, [&] {
        signaling.setVoiceContext(Snowflake(1), Snowflake(2), "session", false);
    });
    QVERIFY(!signaling.watch("guild:1:2:5"));
    QVERIFY(signaling.watchingKey().isEmpty());
}

void TestGoLiveSignaling::validatesKeys()
{
    const auto guild = StreamKey::parse("guild:1:2:3");
    QVERIFY(guild);
    QCOMPARE(guild->toString(), QString("guild:1:2:3"));
    const auto call = StreamKey::parse("call:2:3");
    QVERIFY(call);
    QVERIFY(!call->guildId.isValid());
    QCOMPARE(call->toString(), QString("call:2:3"));
    for (const auto &key : {"", "guild:1:2", "guild:1:2:3:4", "call:0:3", "call:-2:3",
                            "call:2:3 ", "call:2:18446744073709551616", "other:2:3"})
        QVERIFY(!StreamKey::parse(key));
}

void TestGoLiveSignaling::waitsForBothEventsInEitherOrder()
{
    for (const bool serverFirst : {false, true}) {
        GoLiveSignaling signaling(Snowflake(3));
        signaling.setVoiceContext(Snowflake(1), Snowflake(2), "voice-session", true);
        QSignalSpy ready(&signaling, &GoLiveSignaling::rtcSessionReady);
        QVERIFY(signaling.watch("guild:1:2:4"));
        const QJsonObject create{{"stream_key", "guild:1:2:4"}, {"rtc_server_id", "100"}};
        const QJsonObject server{{"stream_key", "guild:1:2:4"}, {"endpoint", "rtc.example"}, {"token", "test-token"}};
        signaling.handleEvent(serverFirst ? "STREAM_SERVER_UPDATE" : "STREAM_CREATE", serverFirst ? server : create);
        QCOMPARE(ready.size(), 0);
        signaling.handleEvent(serverFirst ? "STREAM_CREATE" : "STREAM_SERVER_UPDATE", serverFirst ? create : server);
        QCOMPARE(ready.size(), 1);
        const auto connection = ready.first()[1].toJsonObject();
        QCOMPARE(connection.value("rtc_server_id").toString(), QString("100"));
        QCOMPARE(connection.value("voice_session_id").toString(), QString("voice-session"));
        QVERIFY(!connection.value("publisher").toBool());
        signaling.handleEvent("STREAM_CREATE", create);
        signaling.handleEvent("STREAM_SERVER_UPDATE", server);
        QCOMPARE(ready.size(), 1);
    }
}

void TestGoLiveSignaling::keepsPublisherAndViewerSeparate()
{
    GoLiveSignaling signaling(Snowflake(3));
    signaling.setVoiceContext(Snowflake(1), Snowflake(2), "voice-session", true);
    QSignalSpy requests(&signaling, &GoLiveSignaling::gatewayRequest);
    QVERIFY(signaling.publish());
    QCOMPARE(requests.size(), 2);
    QCOMPARE(requests[0][0].toInt(), 18);
    const auto create = requests[0][1].toJsonObject();
    QCOMPARE(create.value("guild_id").toString(), QString("1"));
    QCOMPARE(create.value("channel_id").toString(), QString("2"));
    QVERIFY(!create.contains("self_mute"));
    QVERIFY(!create.contains("self_deaf"));
    QVERIFY(signaling.watch("guild:1:2:4"));
    signaling.stop(signaling.watchingKey());
    QCOMPARE(signaling.publishingKey(), QString("guild:1:2:3"));
    QCOMPARE(requests.last()[0].toInt(), 19);
    QCOMPARE(requests.last()[1].toJsonObject().value("stream_key").toString(), QString("guild:1:2:4"));
    signaling.stopAll();
    QVERIFY(signaling.publishingKey().isEmpty());
    QVERIFY(signaling.watchingKey().isEmpty());
}

void TestGoLiveSignaling::rejectsCrossChannelAndOwnStream()
{
    GoLiveSignaling signaling(Snowflake(3));
    QSignalSpy requests(&signaling, &GoLiveSignaling::gatewayRequest);
    QVERIFY(!signaling.publish());
    QVERIFY(!signaling.watch("guild:1:2:4"));
    signaling.setVoiceContext(Snowflake(1), Snowflake(2), "voice-session", true);
    QVERIFY(!signaling.watch("guild:1:2:3"));
    QVERIFY(!signaling.watch("guild:1:5:4"));
    QVERIFY(!signaling.watch("guild:6:2:4"));
    QVERIFY(!signaling.watch("call:2:4"));
    QCOMPARE(requests.size(), 0);
}

void TestGoLiveSignaling::ignoresStaleResponsesAfterChannelMove()
{
    GoLiveSignaling signaling(Snowflake(3));
    signaling.setVoiceContext(Snowflake(1), Snowflake(2), "old-session", true);
    QSignalSpy ready(&signaling, &GoLiveSignaling::rtcSessionReady);
    QSignalSpy closed(&signaling, &GoLiveSignaling::rtcSessionClosed);
    QVERIFY(signaling.watch("guild:1:2:4"));
    signaling.setVoiceContext(Snowflake(1), Snowflake(5), "new-session", true);
    QCOMPARE(closed.size(), 1);
    signaling.handleEvent("STREAM_CREATE", {{"stream_key", "guild:1:2:4"}, {"rtc_server_id", "100"}});
    signaling.handleEvent("STREAM_SERVER_UPDATE", {{"stream_key", "guild:1:2:4"}, {"endpoint", "rtc.example"}, {"token", "old-token"}});
    QCOMPARE(ready.size(), 0);
    QVERIFY(signaling.watchingKey().isEmpty());
}

void TestGoLiveSignaling::replacesViewerWithoutDeletingPublisher()
{
    GoLiveSignaling signaling(Snowflake(3));
    signaling.setVoiceContext(Snowflake(1), Snowflake(2), "session", true);
    QVERIFY(signaling.publish());
    QVERIFY(signaling.watch("guild:1:2:4"));
    QSignalSpy requests(&signaling, &GoLiveSignaling::gatewayRequest);
    QVERIFY(signaling.watch("guild:1:2:5"));
    QCOMPARE(requests.size(), 2);
    QCOMPARE(requests[0][0].toInt(), 19);
    QCOMPARE(requests[0][1].toJsonObject().value("stream_key").toString(), QString("guild:1:2:4"));
    QCOMPARE(requests[1][0].toInt(), 20);
    QCOMPARE(signaling.publishingKey(), QString("guild:1:2:3"));
}

void TestGoLiveSignaling::reconnectsWhenTokenChanges()
{
    GoLiveSignaling signaling(Snowflake(3));
    signaling.setVoiceContext(Snowflake(1), Snowflake(2), "session", true);
    QVERIFY(signaling.watch("guild:1:2:4"));
    QSignalSpy ready(&signaling, &GoLiveSignaling::rtcSessionReady);
    QSignalSpy closed(&signaling, &GoLiveSignaling::rtcSessionClosed);
    signaling.handleEvent("STREAM_CREATE", {{"stream_key", "guild:1:2:4"}, {"rtc_server_id", "100"}});
    signaling.handleEvent("STREAM_SERVER_UPDATE", {{"stream_key", "guild:1:2:4"}, {"endpoint", "rtc.example"}, {"token", "first"}});
    signaling.handleEvent("STREAM_SERVER_UPDATE", {{"stream_key", "guild:1:2:4"}, {"endpoint", "rtc.example"}, {"token", "second"}});
    QCOMPARE(closed.size(), 1);
    QCOMPARE(ready.size(), 2);
    QCOMPARE(ready.last()[1].toJsonObject().value("token").toString(), QString("second"));
}

void TestGoLiveSignaling::serverDeleteDoesNotSendAnotherDelete()
{
    GoLiveSignaling signaling(Snowflake(3));
    signaling.setVoiceContext(Snowflake::Invalid, Snowflake(2), "session", true);
    QVERIFY(signaling.watch("call:2:4"));
    QSignalSpy requests(&signaling, &GoLiveSignaling::gatewayRequest);
    QSignalSpy closed(&signaling, &GoLiveSignaling::rtcSessionClosed);
    signaling.handleEvent("STREAM_DELETE", {{"stream_key", "call:2:4"}});
    QCOMPARE(closed.size(), 1);
    QCOMPARE(requests.size(), 0);
    QVERIFY(signaling.watchingKey().isEmpty());
}

QTEST_GUILESS_MAIN(TestGoLiveSignaling)
#include "tst_GoLiveSignaling.moc"
