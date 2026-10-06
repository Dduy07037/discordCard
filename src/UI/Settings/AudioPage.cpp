#include "AudioPage.hpp"

#include "Core/Audio/AudioBackends.hpp"

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace Acheron {
namespace UI {

AudioPage::AudioPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *description = new QLabel(
            tr("Choose how VoiceLite communicates with your Windows audio devices."), this);
    description->setWordWrap(true);
    description->setStyleSheet("color: palette(placeholder-text);");
    layout->addWidget(description);

    auto *row = new QHBoxLayout();
    row->setSpacing(12);
    auto *backendLabel = new QLabel(tr("Audio backend"), this);
    backendLabel->setMinimumWidth(110);
    row->addWidget(backendLabel);

    backendCombo = new QComboBox(this);
    backendCombo->setAccessibleName(tr("Audio backend"));
    backendCombo->setMinimumWidth(220);
    backendCombo->addItem(tr("Automatic"), QString());
    for (const QString &name : Core::Audio::supportedAudioBackends())
        backendCombo->addItem(name, name);

    const int current = backendCombo->findData(Core::Audio::configuredAudioBackend());
    backendCombo->setCurrentIndex(current >= 0 ? current : 0);

    row->addWidget(backendCombo);
    row->addStretch(1);
    layout->addLayout(row);

    auto *note = new QLabel(tr("Changes take effect after restart."), this);
    QFont noteFont = note->font();
    noteFont.setItalic(true);
    if (noteFont.pointSize() > 1)
        noteFont.setPointSize(noteFont.pointSize() - 1);
    note->setFont(noteFont);
    note->setStyleSheet("color: palette(placeholder-text);");
    layout->addWidget(note);

    layout->addStretch(1);

    connect(backendCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        Core::Audio::setConfiguredAudioBackend(backendCombo->itemData(index).toString());
    });
}

} // namespace UI
} // namespace Acheron
