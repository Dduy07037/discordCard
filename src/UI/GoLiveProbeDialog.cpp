#include "GoLiveProbeDialog.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/Audio/AudioPipeline.hpp"
#include "Core/Audio/OpusEncoder.hpp"
#include "Core/Media/RealtimeVp8.hpp"
#include "Discord/Voice/VoiceClient.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <mutex>
#ifdef Q_OS_WIN
#include <objbase.h>
#endif

namespace Acheron::UI {

struct ProbeFrameMailbox {
    std::mutex mutex;
    QImage image;
    quint64 generation = 0;
};

class ProbeMediaWorker : public QObject
{
    Q_OBJECT
public:
    ProbeMediaWorker(const QJsonObject &connection, Core::Snowflake accountId,
                     const Core::ProxyConfig &proxy, std::shared_ptr<ProbeFrameMailbox> mailbox)
        : connection(connection), accountId(accountId), proxy(proxy), mailbox(std::move(mailbox)) {}
    void start()
    {
        const bool publisher = connection.value("publisher").toBool();
        if (!codec.openDecoder() || (publisher && !codec.openEncoder())) {
            emit statusChanged(codec.error().isEmpty() ? tr("VP8 decoder unavailable.") : codec.error());
            return;
        }
        const Core::Snowflake rtcServer(connection.value("rtc_server_id").toString().toULongLong());
        const Core::Snowflake channel(connection.value("rtc_channel_id").toString().toULongLong());
        client = new Discord::Voice::VoiceClient(connection.value("endpoint").toString(),
            connection.value("token").toString(), rtcServer, channel, accountId,
            connection.value("voice_session_id").toString(), proxy, this);
        // Reverse-engineered stream MLS group convention. This needs official
        // Discord interoperability verification; never used for normal voice.
        client->configureVideoSession(Core::Snowflake(quint64(rtcServer) - 1), publisher);
        connect(client, &Discord::Voice::VoiceClient::connected, this, [this, publisher] {
            if (frameTimer) { delete frameTimer; frameTimer = nullptr; }
            if (audioTimer) { delete audioTimer; audioTimer = nullptr; }
            if (pipeline) { pipeline->stop(); delete pipeline; pipeline = nullptr; }
            backend.reset();
            needKeyframe = true;
            client->advertiseVideo();
            clock.start();
            emit statusChanged(tr("Stream transport connected. Waiting for DAVE/media…"));
            if (publisher) {
                frameTimer = new QTimer(this);
                frameTimer->setTimerType(Qt::PreciseTimer);
                frameTimer->setInterval(67);
                connect(frameTimer, &QTimer::timeout, this, &ProbeMediaWorker::sendTestCard);
                frameTimer->start();
                toneEncoder.init(48000, 2, OPUS_APPLICATION_AUDIO);
                toneEncoder.setDtx(false);
                audioTimer = new QTimer(this);
                audioTimer->setTimerType(Qt::PreciseTimer);
                audioTimer->setInterval(20);
                connect(audioTimer, &QTimer::timeout, this, &ProbeMediaWorker::sendTone);
                audioTimer->start();
            } else {
                backend = Core::Audio::IAudioBackend::create();
                pipeline = new Core::Audio::AudioPipeline(this);
                pipeline->start(backend.get(), false);
                pipeline->setDeafened(!soundEnabled);
                connect(client, &Discord::Voice::VoiceClient::audioReceived,
                    pipeline, &Core::Audio::AudioPipeline::onAudioReceived);
            }
        });
        connect(client, &Discord::Voice::VoiceClient::disconnected, this, [this] {
            emit statusChanged(tr("Stream disconnected. Stop and retry the probe."));
            if (frameTimer) frameTimer->stop();
            if (audioTimer) audioTimer->stop();
        });
        connect(client, &Discord::Voice::VoiceClient::videoError, this, &ProbeMediaWorker::statusChanged);
        connect(client, &Discord::Voice::VoiceClient::videoReceived, this,
            [this](quint32, uint32_t, const QByteArray &encoded) {
                const auto image = codec.decode(encoded);
                if (image.isNull())
                    return;
                std::lock_guard lock(mailbox->mutex);
                mailbox->image = image;
                ++mailbox->generation;
            });
        client->start();
    }
    void stop()
    {
        if (frameTimer) frameTimer->stop();
        if (audioTimer) audioTimer->stop();
        if (pipeline) pipeline->stop();
        if (client) client->stop();
        backend.reset();
    }
    void setTone(bool enabled) { toneEnabled = enabled; }
    void setSound(bool enabled)
    {
        soundEnabled = enabled;
        if (pipeline) pipeline->setDeafened(!enabled);
    }
signals:
    void statusChanged(const QString &text);
private:
    void sendTestCard()
    {
        if (!client->isDaveEnabled())
            return;
        QImage image(640, 360, QImage::Format_RGBA8888);
        image.fill(QColor(25, 28, 40));
        QPainter painter(&image);
        const QColor colors[] = {Qt::white, Qt::yellow, Qt::cyan, Qt::green, Qt::magenta, Qt::red, Qt::blue};
        for (int i = 0; i < 7; ++i)
            painter.fillRect(i * 640 / 7, 0, 640 / 7 + 1, 200, colors[i]);
        painter.fillRect(int(clock.elapsed() / 4 % 600), 225, 40, 40, Qt::cyan);
        painter.setPen(Qt::white);
        painter.setFont(QFont(QStringLiteral("Sans"), 16));
        painter.drawText(QRect(20, 285, 600, 60), Qt::AlignCenter,
            tr("Go Live probe • %1 ms").arg(clock.elapsed()));
        painter.end();
        const auto encoded = codec.encode(image, needKeyframe || frames++ % 15 == 0);
        needKeyframe = !client->sendVideoFrame(encoded, uint32_t(clock.elapsed() * 90));
    }
    void sendTone()
    {
        if (!toneEnabled || !client->isDaveEnabled()) {
            if (toneWasActive) {
                client->setSpeaking(false);
                toneWasActive = false;
            }
            return;
        }
        if (!toneWasActive) {
            client->setSpeaking(true);
            toneWasActive = true;
        }
        QByteArray pcm(Core::Audio::AUDIO_FRAME_SIZE, '\0');
        auto *samples = reinterpret_cast<int16_t *>(pcm.data());
        for (int i = 0; i < 960; ++i) {
            const double phase = 2 * 3.141592653589793 * 440 * (toneSample++) / 48000;
            samples[i * 2] = samples[i * 2 + 1] = int16_t(2000 * std::sin(phase));
        }
        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        client->sendAudio(toneEncoder.encode(pcm), nowMs - 20);
    }
    QJsonObject connection;
    Core::Snowflake accountId;
    Core::ProxyConfig proxy;
    std::shared_ptr<ProbeFrameMailbox> mailbox;
    Core::Media::RealtimeVp8 codec;
    Core::Audio::OpusEncoder toneEncoder;
    Discord::Voice::VoiceClient *client = nullptr;
    std::unique_ptr<Core::Audio::IAudioBackend> backend;
    Core::Audio::AudioPipeline *pipeline = nullptr;
    QTimer *frameTimer = nullptr;
    QTimer *audioTimer = nullptr;
    QElapsedTimer clock;
    unsigned frames = 0;
    quint64 toneSample = 0;
    bool needKeyframe = true;
    bool toneEnabled = false;
    bool toneWasActive = false;
    bool soundEnabled = false;
};

GoLiveProbeDialog::GoLiveProbeDialog(Core::Audio::VoiceManager *voiceManager, Core::Snowflake account,
                                   QWidget *parent)
    : QDialog(parent), manager(voiceManager), accountId(account)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Go Live interoperability probe"));
    resize(700, 560);
    auto *layout = new QVBoxLayout(this);
    auto *explanation = new QLabel(tr("Experimental: publishes a generated test card and optional tone. "
        "Join a voice channel first. This probe does not capture your desktop or microphone."), this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);
    streams = new QComboBox(this);
    layout->addWidget(streams);
    const auto refresh = [this] {
        const auto selection = streams->currentData().toString();
        streams->clear();
        if (!manager) return;
        for (const auto &participant : manager->currentParticipants()) {
            const auto state = manager->voiceStateForUser(participant.userId);
            if (participant.userId == accountId || !state || !state->selfStream.get()) continue;
            const Core::Audio::StreamKey key{manager->currentGuildId(), manager->currentChannelId(), participant.userId};
            streams->addItem(QString::number(participant.userId), key.toString());
        }
        const auto previous = streams->findData(selection);
        if (previous >= 0) streams->setCurrentIndex(previous);
    };
    auto *buttons = new QHBoxLayout;
    auto *watch = new QPushButton(tr("Watch selected stream"), this);
    auto *publish = new QPushButton(tr("Publish test card"), this);
    auto *stop = new QPushButton(tr("Stop probe"), this);
    buttons->addWidget(watch); buttons->addWidget(publish); buttons->addWidget(stop);
    layout->addLayout(buttons);
    tone = new QCheckBox(tr("Send 440 Hz test tone"), this);
    sound = new QCheckBox(tr("Play stream audio"), this);
    layout->addWidget(tone); layout->addWidget(sound);
    status = new QLabel(tr("Idle"), this);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    video = new QLabel(this);
    video->setAlignment(Qt::AlignCenter);
    video->setMinimumSize(640, 360);
    layout->addWidget(video, 1);
    connect(watch, &QPushButton::clicked, this, [this] {
        if (!manager || !manager->goLive()->watch(streams->currentData().toString())) {
            status->setText(tr("Select an active stream in your current voice channel."));
            return;
        }
        requestedViewer = manager->goLive()->watchingKey();
        status->setText(tr("Requesting stream viewer…"));
    });
    connect(publish, &QPushButton::clicked, this, [this] {
        if (!manager || !manager->goLive()->publish()) {
            status->setText(tr("Connect to voice before publishing."));
            return;
        }
        requestedPublisher = manager->goLive()->publishingKey();
        status->setText(tr("Requesting test stream…"));
    });
    connect(stop, &QPushButton::clicked, this, [this] {
        if (!manager) return;
        manager->goLive()->stop(requestedViewer);
        manager->goLive()->stop(requestedPublisher);
        status->setText(tr("Stopped"));
    });
    connect(tone, &QCheckBox::toggled, this, [this](bool value) {
        if (publisher.worker) QMetaObject::invokeMethod(publisher.worker,
            [worker = publisher.worker, value] { worker->setTone(value); }, Qt::QueuedConnection);
    });
    connect(sound, &QCheckBox::toggled, this, [this](bool value) {
        if (viewer.worker) QMetaObject::invokeMethod(viewer.worker,
            [worker = viewer.worker, value] { worker->setSound(value); }, Qt::QueuedConnection);
    });
    connect(manager->goLive(), &Core::Audio::GoLiveSignaling::rtcSessionReady, this, &GoLiveProbeDialog::openSession);
    connect(manager->goLive(), &Core::Audio::GoLiveSignaling::rtcSessionClosed, this, &GoLiveProbeDialog::closeSession);
    connect(manager->goLive(), &Core::Audio::GoLiveSignaling::requestFailed, this,
        [this](const QString &, const QString &reason) { status->setText(reason); });
    connect(voiceManager, &Core::Audio::VoiceManager::voiceStateChanged, this, refresh);
    connect(voiceManager, &Core::Audio::VoiceManager::participantUpdated, this, refresh);
    connect(voiceManager, &QObject::destroyed, this, &QDialog::reject);
    refresh();
    auto *render = new QTimer(this);
    render->setInterval(67);
    connect(render, &QTimer::timeout, this, [this] {
        if (!viewer.mailbox) return;
        QImage image;
        {
            std::lock_guard lock(viewer.mailbox->mutex);
            image = std::move(viewer.mailbox->image);
            viewer.mailbox->image = {};
        }
        if (!image.isNull()) video->setPixmap(QPixmap::fromImage(image).scaled(
            video->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    });
    render->start();
}

GoLiveProbeDialog::~GoLiveProbeDialog()
{
    if (manager) {
        manager->goLive()->stop(requestedViewer);
        manager->goLive()->stop(requestedPublisher);
    }
    closeSession(viewer.key);
    closeSession(publisher.key);
}

void GoLiveProbeDialog::openSession(const QString &key, const QJsonObject &connection)
{
    if (!manager || (key != requestedViewer && key != requestedPublisher))
        return;
    auto &session = connection.value("publisher").toBool() ? publisher : viewer;
    closeSession(session.key);
    session.key = key;
    session.mailbox = std::make_shared<ProbeFrameMailbox>();
    session.thread = new QThread(this);
    session.worker = new ProbeMediaWorker(connection, accountId, manager->proxyConfig(), session.mailbox);
    session.worker->setTone(tone->isChecked());
    session.worker->setSound(sound->isChecked());
    session.worker->moveToThread(session.thread);
#ifdef Q_OS_WIN
    connect(session.thread, &QThread::started, session.worker, [] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }, Qt::DirectConnection);
    connect(session.thread, &QThread::finished, session.worker, [] { CoUninitialize(); }, Qt::DirectConnection);
#endif
    connect(session.thread, &QThread::started, session.worker, &ProbeMediaWorker::start);
    connect(session.thread, &QThread::finished, session.worker, &QObject::deleteLater);
    connect(session.worker, &ProbeMediaWorker::statusChanged, this, [this, key](const QString &text) {
        status->setText(key + QStringLiteral(": ") + text);
    });
    session.thread->start();
}

void GoLiveProbeDialog::closeSession(const QString &key)
{
    for (auto *session : {&viewer, &publisher}) {
        if (session->key != key || !session->thread) continue;
        if (session->thread->isRunning()) {
            QMetaObject::invokeMethod(session->worker, [worker = session->worker] { worker->stop(); }, Qt::BlockingQueuedConnection);
            session->thread->quit();
            session->thread->wait();
        }
        delete session->thread;
        *session = {};
    }
}

} // namespace Acheron::UI
#include "GoLiveProbeDialog.moc"
