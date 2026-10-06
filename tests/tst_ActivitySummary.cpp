#include <QtTest>

#include "Discord/ActivitySummary.hpp"
#include "Discord/Events.hpp"

using namespace Acheron::Discord;

class TestActivitySummary : public QObject
{
    Q_OBJECT

private slots:
    void formatsPlayingActivity();
    void honorsStatusDisplayType();
    void formatsCustomStatus();
    void choosesHighestPriorityActivity();
    void boundsUntrustedText();
    void handlesEmptyPresence();
    void parsesMemberListPresence();
    void parsesLivePresenceUpdate();
};

void TestActivitySummary::formatsPlayingActivity()
{
    const QJsonObject presence {
        { "activities", QJsonArray { QJsonObject {
            { "type", 0 }, { "name", "VALORANT" }
        } } }
    };

    const ActivitySummary summary = summarizeActivities(presence);
    QCOMPARE(summary.text, QStringLiteral("Playing VALORANT"));
    QVERIFY(summary.tooltip.isEmpty());
}

void TestActivitySummary::honorsStatusDisplayType()
{
    const QJsonObject presence {
        { "activities", QJsonArray { QJsonObject {
            { "type", 0 },
            { "name", "A Game" },
            { "details", "Ranked match" },
            { "state", "In a party" },
            { "status_display_type", 2 },
            { "party", QJsonObject { { "size", QJsonArray { 2, 5 } } } }
        } } }
    };

    const ActivitySummary summary = summarizeActivities(presence);
    QCOMPARE(summary.text, QStringLiteral("Playing Ranked match"));
    QVERIFY(summary.tooltip.contains(QStringLiteral("App: A Game")));
    QVERIFY(summary.tooltip.contains(QStringLiteral("Status: In a party")));
    QVERIFY(summary.tooltip.contains(QStringLiteral("Party: 2/5")));
}

void TestActivitySummary::formatsCustomStatus()
{
    const QJsonObject presence {
        { "activities", QJsonArray { QJsonObject {
            { "type", 4 },
            { "state", "Taking a break" },
            { "emoji", QJsonObject { { "name", "coffee" } } }
        } } }
    };

    QCOMPARE(summarizeActivities(presence).text,
             QStringLiteral("coffee Taking a break"));
}

void TestActivitySummary::choosesHighestPriorityActivity()
{
    const QJsonObject presence {
        { "activities", QJsonArray {
            QJsonObject { { "type", 4 }, { "state", "Custom" } },
            QJsonObject { { "type", 2 }, { "name", "Spotify" } },
            QJsonObject { { "type", 0 }, { "name", "Minecraft" } }
        } }
    };

    QCOMPARE(summarizeActivities(presence).text,
             QStringLiteral("Playing Minecraft"));
}

void TestActivitySummary::boundsUntrustedText()
{
    const QString oversized(300, QLatin1Char('x'));
    const QJsonObject presence {
        { "activities", QJsonArray { QJsonObject {
            { "type", 0 }, { "name", oversized }
        } } }
    };

    const ActivitySummary summary = summarizeActivities(presence);
    QVERIFY(summary.text.size() <= 128);
    QVERIFY(summary.tooltip.size() <= 512);
    QVERIFY(summary.text.endsWith(QChar(0x2026)));
}

void TestActivitySummary::handlesEmptyPresence()
{
    QVERIFY(summarizeActivities(QJsonObject()).isEmpty());
}

void TestActivitySummary::parsesMemberListPresence()
{
    const QJsonObject rawItem {
        { "member", QJsonObject {
            { "user", QJsonObject { { "id", "42" }, { "username", "Ada" } } },
            { "presence", QJsonObject {
                { "activities", QJsonArray { QJsonObject {
                    { "type", 3 }, { "name", "a livestream" }
                } } }
            } }
        } }
    };

    const GuildMemberListUpdate::SyncItem item =
            GuildMemberListUpdate::SyncItem::fromJson(rawItem);
    QVERIFY(item.hasPresence);
    QCOMPARE(item.activity.text, QStringLiteral("Watching a livestream"));
}

void TestActivitySummary::parsesLivePresenceUpdate()
{
    const QJsonObject payload {
        { "guild_id", "100" },
        { "user", QJsonObject { { "id", "42" } } },
        { "activities", QJsonArray { QJsonObject {
            { "type", 2 }, { "name", "Spotify" }
        } } }
    };

    const PresenceUpdate update = PresenceUpdate::fromJson(payload);
    QCOMPARE(static_cast<quint64>(update.guildId.get()), quint64(100));
    QCOMPARE(static_cast<quint64>(update.userId.get()), quint64(42));
    QCOMPARE(update.activity.text, QStringLiteral("Listening to Spotify"));
}

QTEST_MAIN(TestActivitySummary)
#include "tst_ActivitySummary.moc"
