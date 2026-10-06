#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QPointer>
#include <QThread>
#include <memory>
#include "Core/Snowflake.hpp"

class QLabel;
class QCheckBox;
class QComboBox;

namespace Acheron::Core::Audio { class VoiceManager; }

namespace Acheron::UI {
class ProbeMediaWorker;
struct ProbeFrameMailbox;

// Developer-only protocol probe, enabled by ACHERON_GOLIVE_PROBE=1.
// It publishes a generated test card and optional tone, never the desktop/mic.
class GoLiveProbeDialog : public QDialog
{
    Q_OBJECT
public:
    GoLiveProbeDialog(Core::Audio::VoiceManager *manager, Core::Snowflake accountId,
                      QWidget *parent = nullptr);
    ~GoLiveProbeDialog() override;
private:
    void openSession(const QString &key, const QJsonObject &connection);
    void closeSession(const QString &key);
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
    Session viewer;
    Session publisher;
    QString requestedViewer;
    QString requestedPublisher;
};
} // namespace Acheron::UI
