#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QPointer>
#include <QThread>
#include <memory>
#include <functional>
#include "Core/Snowflake.hpp"

class QLabel;
class QCheckBox;
class QComboBox;
class QPushButton;
class QScreen;
class QTimer;

namespace Acheron::Core::Audio { class VoiceManager; }
namespace Acheron::Core::Media { class LatestVideoFrame; }

namespace Acheron::UI {
class ProbeMediaWorker;
struct ProbeFrameMailbox;

// Experimental VP8/H264/DAVE viewer and explicit, user-selected monitor sharing.
// The protocol test card/tone is an optional diagnostic, not the default source.
class GoLiveProbeDialog : public QDialog
{
    Q_OBJECT
public:
    GoLiveProbeDialog(Core::Audio::VoiceManager *manager, Core::Snowflake accountId,
                      QWidget *parent = nullptr,
                      std::function<QString(Core::Snowflake)> nameResolver = {});
    ~GoLiveProbeDialog() override;
private:
    void openSession(const QString &key, const QJsonObject &connection);
    void closeSession(const QString &key);
    void refreshScreens();
    void captureDesktop();
    void stopCapture();
    struct Session {
        QString key;
        QThread *thread = nullptr;
        ProbeMediaWorker *worker = nullptr;
        std::shared_ptr<ProbeFrameMailbox> mailbox;
    };
    QPointer<Core::Audio::VoiceManager> manager;
    Core::Snowflake accountId;
    QLabel *status = nullptr;
    QLabel *video = nullptr;
    QCheckBox *tone = nullptr;
    QCheckBox *sound = nullptr;
    QComboBox *streams = nullptr;
    QComboBox *screens = nullptr;
    QComboBox *frameRate = nullptr;
    QList<QPointer<QScreen>> screenList;
    QPointer<QScreen> capturedScreen;
    QTimer *captureTimer = nullptr;
    QLabel *preview = nullptr;
    QLabel *sharingStatus = nullptr;
    QLabel *publisherStatus = nullptr;
    QLabel *publisherStats = nullptr;
    QLabel *viewerStats = nullptr;
    int publishingFps = 30;
    qint64 lastPreviewAtMs = 0;
    QPushButton *share = nullptr;
    QPushButton *testCard = nullptr;
    std::shared_ptr<Core::Media::LatestVideoFrame> captureFrames;
    Session viewer;
    Session publisher;
    QString requestedViewer;
    QString requestedPublisher;
};
} // namespace Acheron::UI
