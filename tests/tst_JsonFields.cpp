#include <QTest>
#include <cstring>
#include <new>
#include "Discord/Entities.hpp"

using namespace Acheron;
class TestJsonFields : public QObject
{
    Q_OBJECT
private slots:
    void absentScalarStorageAndCopiesAreInitialized()
    {
        struct Probe : Core::JsonUtils::JsonObject {
            Field<bool, true> flag;
            Field<quint64, true> count;
            Field<bool, true, true> nullable;
        };
        alignas(Probe) unsigned char storage[sizeof(Probe)];
        std::memset(storage, 0xff, sizeof(storage));
        auto *probe = new (storage) Probe;
        QVERIFY(probe->flag.isUndefined());
        QVERIFY(!probe->flag.get());
        QCOMPARE(probe->count.get(), quint64(0));
        Probe copy = *probe;
        QVERIFY(copy.flag.isUndefined());
        QVERIFY(!copy.flag.get());
        copy.nullable = nullptr;
        QVERIFY(copy.nullable.isNull());
        QVERIFY(!copy.nullable.get());
        probe->~Probe();
    }
    void voiceSharingRequiresAnExplicitTrueValue()
    {
        QJsonObject json{{"user_id", "1"}, {"channel_id", "2"}};
        auto state = Discord::VoiceState::fromJson(json);
        QVERIFY(state.selfStream.isUndefined());
        QVERIFY(!state.isStreaming());
        json.insert("self_stream", true);
        state = Discord::VoiceState::fromJson(json);
        QVERIFY(state.isStreaming());
        json.insert("self_stream", false);
        state = Discord::VoiceState::fromJson(json);
        QVERIFY(!state.isStreaming());
        json.remove("self_stream");
        state = Discord::VoiceState::fromJson(json);
        QVERIFY(!state.isStreaming());
        json.insert("self_stream", QJsonValue::Null);
        state = Discord::VoiceState::fromJson(json);
        QVERIFY(!state.isStreaming());
    }
};
QTEST_GUILESS_MAIN(TestJsonFields)
#include "tst_JsonFields.moc"
