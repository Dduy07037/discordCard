#include <QTest>
#include <bytes/bytes.h>
#include "Discord/Voice/VoiceGateway.hpp"
#include "Discord/Voice/VoiceClient.hpp"
#include "Discord/Voice/VoiceEncryption.hpp"
#include "Discord/Voice/DaveSession.hpp"
#include "Discord/Voice/RtpPacket.hpp"
#include <QtEndian>

using namespace Acheron::Discord::Voice;

class TestVoiceGateway : public QObject
{
    Q_OBJECT
private slots:
    void encryptedVideoWithExtensionsAndRtxReachesReceiver()
    {
        QVERIFY(VoiceEncryption::initialize());
        VoiceClient client("", "", Acheron::Core::Snowflake(1), Acheron::Core::Snowflake(2),
                           Acheron::Core::Snowflake(3), "", {});
        client.configureVideoSession(Acheron::Core::Snowflake(99), false);
        const auto mapping = ClientConnectData::fromJson({{"user_id", "44"}, {"video_ssrc", 100},
            {"streams", QJsonArray{QJsonObject{{"ssrc", 100}, {"active", true}}}}});
        QVERIFY(QMetaObject::invokeMethod(&client, "onClientConnect", Qt::DirectConnection, Q_ARG(ClientConnectData, mapping)));
        const QByteArray transportKey(32, 'K');
        SessionDescription description;
        description.mode = QStringLiteral("aead_xchacha20_poly1305_rtpsize");
        description.secretKey = transportKey;
        description.daveProtocolVersion = 1;
        description.videoCodec = QStringLiteral("VP8");
        QVERIFY(QMetaObject::invokeMethod(&client, "onSessionDescription", Qt::DirectConnection, Q_ARG(SessionDescription, description)));
        auto *session = client.findChild<DaveSession *>();
        QVERIFY(session);
        // Model established media keys; MLS/WebSocket handshaking remains a
        // separate real-client gate. This test opens no socket.
        struct Ratchet : discord::dave::IKeyRatchet {
            discord::dave::EncryptionKey GetKey(discord::dave::KeyGeneration g) noexcept override
            { return discord::dave::EncryptionKey(std::vector<uint8_t>(16, uint8_t(42 + g))); }
            void DeleteKey(discord::dave::KeyGeneration) noexcept override {}
        };
        auto *decryptor = session->getOrCreateDecryptor(100, 44);
        decryptor->TransitionToKeyRatchet(std::make_unique<Ratchet>());
        decryptor->TransitionToPassthroughMode(false, std::chrono::seconds(0));
        auto encryptor = discord::dave::CreateEncryptor();
        encryptor->AssignSsrcToCodec(100, discord::dave::Codec::VP8);
        encryptor->SetKeyRatchet(std::make_unique<Ratchet>());
        encryptor->SetPassthroughMode(false);
        QByteArray input(25000, 'v'); input[0] = char(1);
        QByteArray cipher(int(encryptor->GetMaxCiphertextByteSize(discord::dave::MediaType::Video, input.size())), '\0');
        size_t written = 0;
        QCOMPARE(encryptor->Encrypt(discord::dave::MediaType::Video, 100,
            {reinterpret_cast<const uint8_t *>(input.constData()), size_t(input.size())},
            {reinterpret_cast<uint8_t *>(cipher.data()), size_t(cipher.size())}, &written), discord::dave::IEncryptor::Success);
        cipher.resize(int(written));
        const auto fragments = packetizeVp8(cipher);
        VoiceEncryption transport(EncryptionMode::AEAD_XCHACHA20_POLY1305_RTPSIZE, transportKey);
        QByteArray received;
        QObject::connect(&client, &VoiceClient::videoReceived, this, [&](quint32 ssrc, uint32_t timestamp, const QByteArray &frame) {
            QCOMPARE(ssrc, quint32(100)); QCOMPARE(timestamp, uint32_t(90000)); received = frame;
        });
        const auto send = [&](int index, bool repair) {
            RtpHeader h;
            h.ssrc = repair ? 101 : 100; h.payloadType = repair ? 104 : 103;
            h.sequence = uint16_t(1000 + index); h.timestamp = 90000;
            h.marker = index == fragments.size() - 1; h.extension = true;
            const auto header = h.serialize() + QByteArray::fromHex("bede0001");
            QByteArray body = QByteArray::fromHex("31000000");
            if (repair) {
                QByteArray original(2, '\0'); qToBigEndian(uint16_t(1000 + index), reinterpret_cast<uchar *>(original.data()));
                body += original;
            }
            body += fragments[index];
            const auto datagram = header + transport.encrypt(header, body);
            QVERIFY(QMetaObject::invokeMethod(&client, "onDatagram", Qt::DirectConnection, Q_ARG(QByteArray, datagram)));
        };
        for (int i = int(fragments.size()) - 1; i >= 0; --i) if (i != 2) send(i, false);
        QVERIFY(received.isEmpty());
        send(2, true);
        QCOMPARE(received, input);
        QCOMPARE(client.videoDiagnostics().value("frames").toInt(), 1);
        QCOMPARE(client.videoDiagnostics().value("transport_errors").toInt(), 0);
        QCOMPARE(client.videoDiagnostics().value("dave_errors").toInt(), 0);
        client.stop();
    }
    void videoLayersReachReceiverAndCodecUpdatesDoNotEraseThem()
    {
        VoiceGateway gateway("", Acheron::Core::Snowflake(1), Acheron::Core::Snowflake(2),
                             Acheron::Core::Snowflake(3), "", "", {});
        VoiceClient client("", "", Acheron::Core::Snowflake(1), Acheron::Core::Snowflake(2),
                           Acheron::Core::Snowflake(3), "", {});
        client.configureVideoSession(Acheron::Core::Snowflake(99), false, false, 30, true);
        QObject::connect(&gateway, &VoiceGateway::clientConnected, &client, [&](const ClientConnectData &data) {
            QVERIFY(QMetaObject::invokeMethod(&client, "onClientConnect", Qt::DirectConnection, Q_ARG(ClientConnectData, data)));
        });
        QObject::connect(&gateway, &VoiceGateway::sessionUpdated, &client, [&](const QJsonObject &data) {
            QVERIFY(QMetaObject::invokeMethod(&client, "onSessionUpdate", Qt::DirectConnection, Q_ARG(QJsonObject, data)));
        });
        gateway.payloadReceived({{"op", 12}, {"d", QJsonObject{{"user_id", "44"}, {"video_ssrc", 100},
            {"streams", QJsonArray{QJsonObject{{"ssrc", 100}, {"active", true}}, QJsonObject{{"ssrc", 200}, {"active", true}}}}}}});
        QCOMPARE(client.videoDiagnostics().value("sources").toInt(), 2);
        gateway.payloadReceived({{"op", 14}, {"d", QJsonObject{{"video_codec", "H264"}}}});
        QCOMPARE(client.videoDiagnostics().value("codec").toString(), QStringLiteral("H264"));
        QCOMPARE(client.videoDiagnostics().value("sources").toInt(), 2);
        // A disabled layer must no longer be wanted by the receiver.
        gateway.payloadReceived({{"op", 12}, {"d", QJsonObject{{"user_id", "44"}, {"video_ssrc", 100},
            {"streams", QJsonArray{QJsonObject{{"ssrc", 100}, {"active", true}}, QJsonObject{{"ssrc", 200}, {"active", false}}}}}}});
        QCOMPARE(client.videoDiagnostics().value("sources").toInt(), 1);
        client.stop();
        QCOMPARE(client.videoDiagnostics().value("sources").toInt(), 0);
    }
    void routesVideoAndCodecUpdatesSeparately()
    {
        VoiceGateway gateway("", Acheron::Core::Snowflake(1), Acheron::Core::Snowflake(2),
                             Acheron::Core::Snowflake(3), "", "", {});
        int videos = 0, codecChanges = 0, joins = 0;
        QObject::connect(&gateway, &VoiceGateway::clientConnected, this, [&](const ClientConnectData &data) {
            ++videos;
            QCOMPARE(quint64(data.userId.get()), quint64(44));
            QCOMPARE(quint32(data.videoSsrc.get()), quint32(100));
            QCOMPARE(data.streams.size(), 2);
        });
        QObject::connect(&gateway, &VoiceGateway::sessionUpdated, this, [&](const QJsonObject &data) {
            ++codecChanges;
            QCOMPARE(data.value("video_codec").toString(), QStringLiteral("H264"));
        });
        QObject::connect(&gateway, &VoiceGateway::clientsConnected, this, [&](const QStringList &ids) {
            ++joins;
            QCOMPARE(ids, QStringList{QStringLiteral("44")});
        });
        gateway.payloadReceived({{"op", 11}, {"d", QJsonObject{{"user_ids", QJsonArray{"44"}}}}});
        gateway.payloadReceived({{"op", 12}, {"d", QJsonObject{{"user_id", "44"}, {"video_ssrc", 100},
            {"streams", QJsonArray{QJsonObject{{"ssrc", 100}, {"active", true}}, QJsonObject{{"ssrc", 200}, {"active", true}}}}}}});
        gateway.payloadReceived({{"op", 14}, {"d", QJsonObject{{"video_codec", "H264"}}}});
        QCOMPARE(joins, 1);
        QCOMPARE(videos, 1);
        QCOMPARE(codecChanges, 1);
    }
};
QTEST_GUILESS_MAIN(TestVoiceGateway)
#include "tst_VoiceGateway.moc"
