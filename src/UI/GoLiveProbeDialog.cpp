#include "GoLiveProbeDialog.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/Audio/AudioPipeline.hpp"
#include "Core/Audio/OpusEncoder.hpp"
#include "Core/Media/RealtimeVp8.hpp"
#include "Core/Media/LatestVideoFrame.hpp"
#include "Discord/Voice/VoiceClient.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QScreen>
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
                     const Core::ProxyConfig &proxy, std::shared_ptr<ProbeFrameMailbox> mailbox,
                     std::shared_ptr<Core::Media::LatestVideoFrame> captureFrames = {})
        : connection(connection), accountId(accountId), proxy(proxy), mailbox(std::move(mailbox)),
          captureFrames(std::move(captureFrames)) {}
    void start()
    {
        const bool publisher = connection.value("publisher").toBool();
        if ((!publisher && !codec.openDecoder()) || (publisher &&
            !(captureFrames ? codec.openEncoder(QSize(1280, 720), 15, 1800000) : codec.openEncoder()))) {
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
        client->configureVideoSession(Core::Snowflake(quint64(rtcServer) - 1), publisher, bool(captureFrames));
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
                connect(frameTimer, &QTimer::timeout, this, &ProbeMediaWorker::sendFrame);
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
    void sendFrame()
    {
        if (!client->isDaveEnabled())
            return;
        if (captureFrames) {
            const auto frame = captureFrames->take(Core::Media::videoClockMs());
            if (!frame) return;
            const auto encoded = codec.encode(frame->image, needKeyframe || frames++ % 15 == 0);
            needKeyframe = !client->sendVideoFrame(encoded, uint32_t(clock.elapsed() * 90));
            return;
        }
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
    std::shared_ptr<Core::Media::LatestVideoFrame> captureFrames;
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
                                   QWidget *parent, std::function<QString(Core::Snowflake)> nameResolver)
    : QDialog(parent), manager(voiceManager), accountId(account)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Go Live — experimental"));
    resize(800, 720);
    auto *layout = new QVBoxLayout(this);
    auto *explanation = new QLabel(tr("Watch a stream in your voice channel or share a monitor you select. "
        "Discord interoperability is still being tested. Screen sharing does not include system audio."), this);
    explanation->setWordWrap(true);
    layout->addWidget(explanation);

    layout->addWidget(new QLabel(tr("Share your screen"), this));
    screens = new QComboBox(this);
    screens->setAccessibleName(tr("Screen to share"));
    layout->addWidget(screens);
    auto *shareButtons = new QHBoxLayout;
    auto *previewButton = new QPushButton(tr("Preview screen"), this);
    share = new QPushButton(tr("Share selected screen"), this);
    auto *stopSharing = new QPushButton(tr("Stop sharing"), this);
    shareButtons->addWidget(previewButton);
    shareButtons->addWidget(share);
    shareButtons->addWidget(stopSharing);
    layout->addLayout(shareButtons);
    sharingStatus = new QLabel(tr("Screen sharing is off"), this);
    sharingStatus->setTextFormat(Qt::PlainText);
    layout->addWidget(sharingStatus);
    preview = new QLabel(tr("Preview appears only after you click Preview screen or Share."), this);
    preview->setAlignment(Qt::AlignCenter);
    preview->setWordWrap(true);
    preview->setMinimumSize(320, 80);
    preview->setMaximumHeight(120);
    layout->addWidget(preview);
    captureTimer = new QTimer(this);
    captureTimer->setInterval(67);
    captureTimer->setTimerType(Qt::PreciseTimer);
    connect(captureTimer, &QTimer::timeout, this, &GoLiveProbeDialog::captureDesktop);
    refreshScreens();
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] { refreshScreens(); });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen *removed) {
        if (capturedScreen == removed) {
            stopCapture();
            if (manager) manager->goLive()->stop(requestedPublisher);
            sharingStatus->setText(tr("Sharing stopped: monitor disconnected."));
        }
        refreshScreens();
    });
    connect(previewButton, &QPushButton::clicked, this, [this] {
        if (captureTimer->isActive()) return;
        const int index = screens->currentIndex();
        if (index < 0 || index >= screenList.size() || !screenList[index]) return;
        const auto pixmap = screenList[index]->grabWindow(0);
        if (pixmap.isNull()) {
            sharingStatus->setText(tr("Screen capture is unavailable on this desktop."));
            return;
        }
        preview->setPixmap(pixmap.scaled(preview->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        sharingStatus->setText(tr("Local preview — this does not start a screen stream."));
    });
    connect(share, &QPushButton::clicked, this, [this] {
        if (!manager || !manager->isConnected()) {
            sharingStatus->setText(tr("Join a voice channel before sharing."));
            return;
        }
        const int index = screens->currentIndex();
        if (index < 0 || index >= screenList.size() || !screenList[index]) return;
        capturedScreen = screenList[index];
        const auto capturedAt = Core::Media::videoClockMs();
        auto image = capturedScreen->grabWindow(0).toImage();
        if (image.isNull()) {
            capturedScreen.clear();
            sharingStatus->setText(tr("Screen capture is unavailable on this desktop."));
            return;
        }
        captureFrames = std::make_shared<Core::Media::LatestVideoFrame>();
        captureFrames->push(std::move(image), capturedAt);
        if (!manager->goLive()->publish()) {
            stopCapture();
            sharingStatus->setText(tr("Could not start sharing. Stop any existing stream first."));
            return;
        }
        requestedPublisher = manager->goLive()->publishingKey();
        screens->setEnabled(false);
        share->setEnabled(false);
        testCard->setEnabled(false);
        captureTimer->start();
        sharingStatus->setText(tr("Capturing %1 • 720p / 15 fps").arg(capturedScreen->name()));
        status->setText(tr("Connecting screen stream…"));
    });
    connect(stopSharing, &QPushButton::clicked, this, [this] {
        stopCapture();
        if (manager) manager->goLive()->stop(requestedPublisher);
        requestedPublisher.clear();
    });

    layout->addWidget(new QLabel(tr("Watch another person's stream"), this));
    streams = new QComboBox(this);
    streams->setAccessibleName(tr("Live streams in this voice channel"));
    layout->addWidget(streams);
    auto *watchButtons = new QHBoxLayout;
    auto *watch = new QPushButton(tr("Watch selected stream"), this);
    auto *stopWatching = new QPushButton(tr("Stop watching"), this);
    watchButtons->addWidget(watch);
    watchButtons->addWidget(stopWatching);
    layout->addLayout(watchButtons);
    sound = new QCheckBox(tr("Play stream audio"), this);
    sound->setToolTip(tr("Stream audio currently uses the default output device."));
    layout->addWidget(sound);
    status = new QLabel(tr("Join voice to watch or share. No stream selected."), this);
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    layout->addWidget(status);
    video = new QLabel(tr("No stream video yet"), this);
    video->setAlignment(Qt::AlignCenter);
    video->setMinimumSize(640, 220);
    layout->addWidget(video, 1);

    // Preserve the protocol test source as an explicit developer-only option.
    testCard = new QPushButton(tr("Publish diagnostic test card"), this);
    tone = new QCheckBox(tr("Send 440 Hz diagnostic tone"), this);
    const bool diagnostic = qEnvironmentVariable("ACHERON_GOLIVE_PROBE") == QStringLiteral("1");
    testCard->setVisible(diagnostic);
    tone->setVisible(diagnostic);
    layout->addWidget(testCard);
    layout->addWidget(tone);
    connect(testCard, &QPushButton::clicked, this, [this] {
        if (!manager || !manager->isConnected() || !manager->goLive()->publish()) return;
        requestedPublisher = manager->goLive()->publishingKey();
        share->setEnabled(false);
        testCard->setEnabled(false);
        status->setText(tr("Connecting diagnostic stream…"));
    });
    const auto refresh = [this, watch, nameResolver] {
        const auto selection = streams->currentData().toString();
        streams->clear();
        if (!manager) return;
        for (const auto &participant : manager->currentParticipants()) {
            const auto state = manager->voiceStateForUser(participant.userId);
            if (participant.userId == accountId || !state || !state->selfStream.get()) continue;
            const Core::Audio::StreamKey key{manager->currentGuildId(), manager->currentChannelId(), participant.userId};
            const auto name = nameResolver ? nameResolver(participant.userId) : QString::number(participant.userId);
            streams->addItem(name, key.toString());
        }
        const auto previous = streams->findData(selection);
        if (previous >= 0) streams->setCurrentIndex(previous);
        watch->setEnabled(manager->isConnected() && streams->count() > 0);
        share->setEnabled(manager->isConnected() && requestedPublisher.isEmpty() && screens->count() > 0);
        testCard->setEnabled(manager->isConnected() && requestedPublisher.isEmpty());
        if (streams->count() == 0) streams->setPlaceholderText(tr("No one is sharing in this voice channel"));
    };
    connect(watch, &QPushButton::clicked, this, [this] {
        if (!manager || !manager->goLive()->watch(streams->currentData().toString())) {
            status->setText(tr("Select an active stream in your current voice channel."));
            return;
        }
        requestedViewer = manager->goLive()->watchingKey();
        status->setText(tr("Connecting stream viewer…"));
    });
    connect(stopWatching, &QPushButton::clicked, this, [this] {
        if (manager) manager->goLive()->stop(requestedViewer);
        requestedViewer.clear();
        video->clear();
        video->setText(tr("No stream video yet"));
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
    connect(manager->goLive(), &Core::Audio::GoLiveSignaling::rtcSessionClosed, this, [this, refresh](const QString &key) {
        closeSession(key);
        if (key == requestedPublisher && manager->goLive()->publishingKey() != key) {
            stopCapture();
            requestedPublisher.clear();
        }
        if (key == requestedViewer && manager->goLive()->watchingKey() != key) {
            requestedViewer.clear();
            video->clear();
            video->setText(tr("Stream ended"));
        }
        refresh();
    });
    connect(manager->goLive(), &Core::Audio::GoLiveSignaling::requestFailed, this,
        [this, refresh](const QString &key, const QString &reason) {
            if (key == requestedPublisher) { stopCapture(); requestedPublisher.clear(); }
            if (key == requestedViewer) requestedViewer.clear();
            status->setText(reason);
            refresh();
        });
    connect(voiceManager, &Core::Audio::VoiceManager::voiceStateChanged, this, refresh);
    connect(voiceManager, &Core::Audio::VoiceManager::participantUpdated, this, refresh);
    connect(voiceManager, &Core::Audio::VoiceManager::participantJoined, this, refresh);
    connect(voiceManager, &Core::Audio::VoiceManager::participantLeft, this, refresh);
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

void GoLiveProbeDialog::refreshScreens()
{
    QScreen *previous = capturedScreen.data();
    if (!previous && screens->currentIndex() >= 0 && screens->currentIndex() < screenList.size())
        previous = screenList[screens->currentIndex()].data();
    screenList.clear();
    screens->clear();
    for (auto *screen : QGuiApplication::screens()) {
        const auto size = screen->size();
        screens->addItem(tr("%1 (%2 × %3)").arg(screen->name()).arg(size.width()).arg(size.height()));
        screenList.append(QPointer<QScreen>(screen));
        if (screen == previous) screens->setCurrentIndex(screens->count() - 1);
    }
}

void GoLiveProbeDialog::captureDesktop()
{
    if (!capturedScreen || !captureFrames) return;
    const auto capturedAt = Core::Media::videoClockMs();
    auto image = capturedScreen->grabWindow(0).toImage();
    if (image.isNull()) {
        stopCapture();
        if (manager) manager->goLive()->stop(requestedPublisher);
        sharingStatus->setText(tr("Sharing stopped: screen capture failed."));
        return;
    }
    preview->setPixmap(QPixmap::fromImage(image).scaled(preview->size(), Qt::KeepAspectRatio, Qt::FastTransformation));
    captureFrames->push(std::move(image), capturedAt);
}

void GoLiveProbeDialog::stopCapture()
{
    captureTimer->stop();
    capturedScreen.clear();
    if (captureFrames) captureFrames->clear();
    captureFrames.reset();
    screens->setEnabled(true);
    share->setEnabled(manager && manager->isConnected() && screens->count() > 0);
    if (testCard) testCard->setEnabled(manager && manager->isConnected());
    preview->clear();
    preview->setText(tr("Screen sharing is off"));
    sharingStatus->setText(tr("Screen sharing is off"));
}

GoLiveProbeDialog::~GoLiveProbeDialog()
{
    stopCapture();
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
    session.worker = new ProbeMediaWorker(connection, accountId, manager->proxyConfig(), session.mailbox,
        connection.value("publisher").toBool() ? captureFrames : nullptr);
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
        status->setText((key == requestedPublisher ? tr("Sharing: ") : tr("Watching: ")) + text);
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
