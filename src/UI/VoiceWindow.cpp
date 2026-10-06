#include "VoiceWindow.hpp"
#ifdef ACHERON_HAVE_FFMPEG
#include "GoLiveProbeDialog.hpp"
#endif

#include "Core/Audio/VoiceManager.hpp"
#include "Core/ImageManager.hpp"
#include "Core/Theme/Manager.hpp"
#include "Core/Theme/Motion.hpp"
#include "Core/Theme/Tokens.hpp"

#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPainter>
#include <QPainterPath>
#include <QSettings>
#include <QVBoxLayout>

#include <opus.h>
#include <QHBoxLayout>
#include <QFrame>

namespace Acheron {
namespace UI {

VolumeMeter::VolumeMeter(QWidget *parent) : QWidget(parent)
{
    setFixedHeight(10);
    setAccessibleName(tr("Microphone input level"));
}

void VolumeMeter::setLevel(float rms)
{
    level = rms;
    update();
}

void VolumeMeter::setThreshold(float t)
{
    threshold = t;
    update();
}

void VolumeMeter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const QRectF trackRect = QRectF(rect()).adjusted(0, 1, 0, -1);
    p.setPen(Qt::NoPen);
    p.setBrush(palette().color(QPalette::Mid));
    p.drawRoundedRect(trackRect, 4, 4);

    float fraction = qBound(0.0f, level / MAX_RMS, 1.0f);
    int barWidth = static_cast<int>(fraction * width());

    if (barWidth > 0) {
        const auto &theme = Core::Theme::Manager::instance();
        const Core::Theme::Token levelToken = fraction < 0.7f
                                                      ? Core::Theme::Token::Success
                                                      : (fraction < 0.9f
                                                                 ? Core::Theme::Token::Warning
                                                                 : Core::Theme::Token::Danger);
        p.setBrush(theme.color(levelToken));
        p.drawRoundedRect(QRectF(0, 1, barWidth, height() - 2), 4, 4);
    }

    if (threshold >= 0.0f) {
        float threshFraction = qBound(0.0f, threshold / MAX_RMS, 1.0f);
        int threshX = static_cast<int>(threshFraction * width());
        p.setPen(QPen(palette().color(QPalette::WindowText), 1));
        p.drawLine(threshX, 1, threshX, height() - 1);
    }
}

VoiceAvatar::VoiceAvatar(QWidget *parent) : QWidget(parent)
{
    setFixedSize(OUTER_SIZE, OUTER_SIZE);
}

void VoiceAvatar::setPixmap(const QPixmap &pm)
{
    avatar = pm;
    update();
}

void VoiceAvatar::setSpeaking(bool s)
{
    if (speaking == s)
        return;
    speaking = s;
    update();
}

void VoiceAvatar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int s = qMin(width(), height());

    if (speaking) {
        p.setPen(QPen(Core::Theme::Manager::instance().color(Core::Theme::Token::Success), BORDER_WIDTH));
        p.setBrush(Qt::NoBrush);
        qreal half = BORDER_WIDTH / 2.0;
        p.drawRoundedRect(QRectF(half, half, s - BORDER_WIDTH, s - BORDER_WIDTH),
                          AVATAR_RADIUS + 1, AVATAR_RADIUS + 1);
    }

    int inset = BORDER_WIDTH + 1;
    QRectF avatarRect(inset, inset, s - inset * 2, s - inset * 2);

    QPainterPath clip;
    clip.addRoundedRect(avatarRect, AVATAR_RADIUS, AVATAR_RADIUS);
    p.setClipPath(clip);

    if (!avatar.isNull())
        p.drawPixmap(avatarRect.toRect(), avatar);
    else {
        p.setPen(Qt::NoPen);
        p.setBrush(palette().color(QPalette::Mid));
        p.drawRoundedRect(avatarRect, AVATAR_RADIUS, AVATAR_RADIUS);
    }
}

VoiceUserWidget::VoiceUserWidget(Core::Snowflake userId, QWidget *parent)
    : QWidget(parent), userId(userId)
{
    setObjectName("voiceUserRow");
    setFixedHeight(52);
    setAttribute(Qt::WA_StyledBackground, true);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(8, 6, 8, 6);
    row->setSpacing(8);

    avatarWidget = new VoiceAvatar(this);

    nameLabel = new QLabel(this);
    nameLabel->setMinimumWidth(40);
    nameLabel->setTextFormat(Qt::PlainText);

    meter = new VolumeMeter(this);
    meter->setFixedWidth(52);
    meter->setFixedHeight(8);

    volumeSlider = new QSlider(Qt::Horizontal, this);
    volumeSlider->setRange(0, 200);
    volumeSlider->setValue(100);
    volumeSlider->setMinimumWidth(72);
    volumeSlider->setToolTip(tr("Participant volume"));
    volumeSlider->setAccessibleName(tr("Participant volume"));

    muteBtn = new QPushButton(tr("Mute"), this);
    muteBtn->setObjectName("voiceMuteButton");
    muteBtn->setMinimumSize(58, 30);
    muteBtn->setCheckable(true);
    muteBtn->setToolTip(tr("Mute this participant locally"));
    muteBtn->setAccessibleName(tr("Mute participant"));

    stateLabel = new QLabel(this);
    stateLabel->setObjectName("voiceStateFlags");
    stateLabel->setFixedWidth(30);
    stateLabel->setAlignment(Qt::AlignCenter);
    stateLabel->setAccessibleName(tr("Participant voice state"));

    row->addWidget(avatarWidget);
    row->addWidget(nameLabel, 1);
    row->addWidget(meter);
    row->addWidget(volumeSlider, 1);
    row->addWidget(muteBtn);
    row->addWidget(stateLabel);

    speakingDecayTimer.setSingleShot(true);
    speakingDecayTimer.setInterval(SPEAKING_DECAY_MS);
    connect(&speakingDecayTimer, &QTimer::timeout, this, [this]() {
        avatarWidget->setSpeaking(false);
    });

    connect(volumeSlider, &QSlider::valueChanged, this, [this](int value) {
        if (locallyMuted)
            return;
        emit volumeChanged(this->userId, value);
    });

    connect(muteBtn, &QPushButton::toggled, this, [this](bool checked) {
        locallyMuted = checked;
        emit muteToggled(this->userId, checked);
    });
}

void VoiceUserWidget::setSpeaking(bool speaking)
{
    if (speaking) {
        avatarWidget->setSpeaking(true);
        speakingDecayTimer.start();
    } else {
        speakingDecayTimer.stop();
        avatarWidget->setSpeaking(false);
    }
}

void VoiceUserWidget::setAudioLevel(float rms)
{
    meter->setLevel(rms);

    if (rms > 0.0f) {
        avatarWidget->setSpeaking(true);
        speakingDecayTimer.start();
    }
}

void VoiceUserWidget::setVoiceStateFlags(bool selfMute, bool selfDeaf, bool serverMute, bool serverDeaf, bool suppress)
{
    QString flags;
    QStringList descriptions;
    if (serverDeaf || selfDeaf)
        flags += "D";
    if (serverDeaf || selfDeaf)
        descriptions += tr("deafened");
    if (serverMute || selfMute)
        flags += "M";
    if (serverMute || selfMute)
        descriptions += tr("muted");
    if (suppress)
        flags += "S";
    if (suppress)
        descriptions += tr("suppressed");
    stateLabel->setText(flags);
    stateLabel->setToolTip(descriptions.join(", "));
    stateLabel->setAccessibleDescription(descriptions.join(", "));
}

void VoiceUserWidget::setLocallyMuted(bool muted)
{
    locallyMuted = muted;
    muteBtn->setChecked(muted);
}

void VoiceUserWidget::setVolume(int pct)
{
    volumeSlider->blockSignals(true);
    volumeSlider->setValue(pct);
    volumeSlider->blockSignals(false);
}

void VoiceUserWidget::contextMenuEvent(QContextMenuEvent *event)
{
    if (!daveActive)
        return;

    QMenu menu(this);

    auto *verifyAction = menu.addAction(tr("View Verification Code"));
    connect(verifyAction, &QAction::triggered, this, [this]() {
        emit verificationCodeRequested(userId);
    });

    menu.exec(event->globalPos());
}

void VoiceUserWidget::setDaveActive(bool active)
{
    daveActive = active;
}

void VoiceUserWidget::setDisplayName(const QString &name)
{
    nameLabel->setText(name);
}

void VoiceUserWidget::setAvatar(const QPixmap &pm)
{
    avatarWidget->setPixmap(pm);
}

VoiceWindow::VoiceWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("Voice Settings"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    setupUi();
    connect(&Core::Theme::Manager::instance(), &Core::Theme::Manager::themeChanged,
            this, &VoiceWindow::applyTheme);
}

void VoiceWindow::setupUi()
{
    setObjectName("voiceWindow");
    setMinimumWidth(420);
    setMinimumHeight(440);
    resize(480, 600);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 18, 20, 20);
    layout->setSpacing(12);

    auto *title = new QLabel(tr("Voice session"), this);
    title->setObjectName("voiceTitle");
    layout->addWidget(title);

    auto *subtitle = new QLabel(tr("Manage participants, devices and voice quality."), this);
    subtitle->setObjectName("voiceCaption");
    subtitle->setWordWrap(true);
    layout->addWidget(subtitle);

    auto *usersHeaderRow = new QHBoxLayout;
    usersHeaderRow->setContentsMargins(0, 0, 0, 0);
    auto *usersHeader = new QLabel(tr("Connected Users"), this);
    usersHeader->setObjectName("voiceSectionTitle");
    usersHeaderRow->addWidget(usersHeader);
    usersHeaderRow->addStretch();
    privacyCodeBtn = new QPushButton(tr("Privacy Code"), this);
    privacyCodeBtn->setObjectName("voiceLinkButton");
    privacyCodeBtn->setFlat(true);
    privacyCodeBtn->setCursor(Qt::PointingHandCursor);
    privacyCodeBtn->hide();
    privacyCodeBtn->setToolTip(tr("Compare end-to-end encryption codes"));
    connect(privacyCodeBtn, &QPushButton::clicked, this, &VoiceWindow::showPrivacyCode);
    usersHeaderRow->addWidget(privacyCodeBtn);
    layout->addLayout(usersHeaderRow);

    userScrollArea = new QScrollArea(this);
    userScrollArea->setObjectName("voiceParticipants");
    userScrollArea->setWidgetResizable(true);
    userScrollArea->setFrameShape(QFrame::NoFrame);
    userScrollArea->setMinimumHeight(72);

    userListContainer = new QWidget;
    userListLayout = new QVBoxLayout(userListContainer);
    userListLayout->setContentsMargins(4, 4, 4, 4);
    userListLayout->setSpacing(2);
    userListLayout->addStretch();
    userScrollArea->setWidget(userListContainer);
    layout->addWidget(userScrollArea, 1);

    auto *audioHeader = new QLabel(tr("Audio controls"), this);
    audioHeader->setObjectName("voiceSectionTitle");
    layout->addWidget(audioHeader);

    auto *meterLabel = new QLabel(tr("Microphone level"), this);
    meterLabel->setObjectName("voiceCaption");
    layout->addWidget(meterLabel);

    volumeMeter = new VolumeMeter(this);
    volumeMeter->setThreshold(100.0f);
    layout->addWidget(volumeMeter);

    auto *inputDevRow = new QHBoxLayout;
    inputDevRow->setSpacing(10);
    auto *inputDevLabel = new QLabel(tr("Input:"), this);
    inputDevLabel->setFixedWidth(72);
    inputDeviceCombo = new QComboBox(this);
    inputDeviceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    inputDeviceCombo->setAccessibleName(tr("Input device"));
    inputDevRow->addWidget(inputDevLabel);
    inputDevRow->addWidget(inputDeviceCombo, 1);
    layout->addLayout(inputDevRow);

    auto *outputDevRow = new QHBoxLayout;
    outputDevRow->setSpacing(10);
    auto *outputDevLabel = new QLabel(tr("Output:"), this);
    outputDevLabel->setFixedWidth(72);
    outputDeviceCombo = new QComboBox(this);
    outputDeviceCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    outputDeviceCombo->setAccessibleName(tr("Output device"));
    outputDevRow->addWidget(outputDevLabel);
    outputDevRow->addWidget(outputDeviceCombo, 1);
    layout->addLayout(outputDevRow);

    auto *inputRow = new QHBoxLayout;
    inputRow->setSpacing(10);
    auto *inputLabel = new QLabel(tr("Input"), this);
    inputLabel->setFixedWidth(72);
    inputGainSlider = new QSlider(Qt::Horizontal, this);
    inputGainSlider->setRange(0, 200);
    inputGainSlider->setValue(100);
    inputGainSlider->setAccessibleName(tr("Input gain"));
    inputGainValue = new QLabel("100%", this);
    inputGainValue->setObjectName("voiceValue");
    inputGainValue->setFixedWidth(48);
    inputGainValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    inputRow->addWidget(inputLabel);
    inputRow->addWidget(inputGainSlider, 1);
    inputRow->addWidget(inputGainValue);
    layout->addLayout(inputRow);

    auto *outputRow = new QHBoxLayout;
    outputRow->setSpacing(10);
    auto *outputLabel = new QLabel(tr("Output"), this);
    outputLabel->setFixedWidth(72);
    outputVolumeSlider = new QSlider(Qt::Horizontal, this);
    outputVolumeSlider->setRange(0, 200);
    outputVolumeSlider->setValue(100);
    outputVolumeSlider->setAccessibleName(tr("Output volume"));
    outputVolumeValue = new QLabel("100%", this);
    outputVolumeValue->setObjectName("voiceValue");
    outputVolumeValue->setFixedWidth(48);
    outputVolumeValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    outputRow->addWidget(outputLabel);
    outputRow->addWidget(outputVolumeSlider, 1);
    outputRow->addWidget(outputVolumeValue);
    layout->addLayout(outputRow);

    auto *vadRow = new QHBoxLayout;
    vadRow->setSpacing(10);
    auto *vadLabel = new QLabel(tr("VAD"), this);
    vadLabel->setFixedWidth(72);
    vadLabel->setToolTip(tr("Voice activity detection threshold"));
    vadThresholdSlider = new QSlider(Qt::Horizontal, this);
    vadThresholdSlider->setRange(0, 2000);
    vadThresholdSlider->setValue(100);
    vadThresholdSlider->setAccessibleName(tr("Voice activity detection threshold"));
    vadThresholdValue = new QLabel("100", this);
    vadThresholdValue->setObjectName("voiceValue");
    vadThresholdValue->setFixedWidth(48);
    vadThresholdValue->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    vadRow->addWidget(vadLabel);
    vadRow->addWidget(vadThresholdSlider, 1);
    vadRow->addWidget(vadThresholdValue);
    layout->addLayout(vadRow);

    connect(inputGainSlider, &QSlider::valueChanged, this, [this](int value) {
        inputGainValue->setText(QString("%1%").arg(value));
        if (voiceManager)
            voiceManager->setInputGain(static_cast<float>(value) / 100.0f);
    });

    connect(outputVolumeSlider, &QSlider::valueChanged, this, [this](int value) {
        outputVolumeValue->setText(QString("%1%").arg(value));
        if (voiceManager)
            voiceManager->setOutputVolume(static_cast<float>(value) / 100.0f);
    });

    connect(vadThresholdSlider, &QSlider::valueChanged, this, [this](int value) {
        vadThresholdValue->setText(QString::number(value));
        volumeMeter->setThreshold(static_cast<float>(value));
        if (voiceManager)
            voiceManager->setVadThreshold(static_cast<float>(value));
    });

    connect(inputDeviceCombo, qOverload<int>(&QComboBox::activated), this, [this](int index) {
        if (!voiceManager)
            return;
        QByteArray deviceId = inputDeviceCombo->itemData(index).toByteArray();
        voiceManager->setInputDevice(deviceId);
    });

    connect(outputDeviceCombo, qOverload<int>(&QComboBox::activated), this, [this](int index) {
        if (!voiceManager)
            return;
        QByteArray deviceId = outputDeviceCombo->itemData(index).toByteArray();
        voiceManager->setOutputDevice(deviceId);
    });

    buildAdvancedSection(layout);
    loadCodecSettings();
    applyTheme();
}

void VoiceWindow::showEvent(QShowEvent *event)
{
    Core::Theme::Motion::prepareFadeIn(this);
    QWidget::showEvent(event);
    Core::Theme::Motion::startFadeIn(this);
}

void VoiceWindow::applyTheme()
{
    const auto &theme = Core::Theme::Manager::instance();
    const QString danger = theme.color(Core::Theme::Token::Danger).name();
    const QString onDanger = theme.color(Core::Theme::Token::HighlightedText).name();
    setStyleSheet(QStringLiteral(
            "#voiceWindow { background: palette(window); }"
            "#voiceTitle { font-size: 17px; font-weight: 650; color: palette(window-text); }"
            "#voiceSectionTitle { font-size: 11px; font-weight: 650; color: palette(window-text); }"
            "#voiceCaption, #voiceValue, #voiceStateFlags { color: palette(placeholder-text); }"
            "#voiceParticipants { background: palette(base); border: 1px solid palette(mid); border-radius: 8px; }"
            "#voiceParticipants > QWidget > QWidget { background: transparent; }"
            "#voiceUserRow { background: transparent; border-radius: 6px; }"
            "#voiceUserRow:hover { background: palette(alternate-base); }"
            "#voiceLinkButton { min-height: 26px; color: palette(highlight); background: transparent; border: none; padding: 0 4px; }"
            "#voiceLinkButton:hover { text-decoration: underline; }"
            "#voiceMuteButton:checked { color: %1; background: %2; border-color: %2; }"
            "#voiceAdvanced { color: palette(placeholder-text); background: transparent; border: none; padding: 4px 0; }"
            "#voiceAdvanced:hover, #voiceAdvanced:checked { color: palette(window-text); background: transparent; }"
            "#voiceAdvancedPanel { background: palette(base); border: 1px solid palette(mid); border-radius: 8px; }")
                          .arg(onDanger, danger));
}

void VoiceWindow::buildAdvancedSection(QVBoxLayout *parentLayout)
{
    advancedToggle = new QToolButton(this);
    advancedToggle->setObjectName("voiceAdvanced");
    advancedToggle->setText(tr("Advanced"));
    advancedToggle->setCheckable(true);
    advancedToggle->setChecked(false);
    advancedToggle->setArrowType(Qt::RightArrow);
    advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    advancedToggle->setAutoRaise(true);
    advancedToggle->setCursor(Qt::PointingHandCursor);
    parentLayout->addWidget(advancedToggle);

    advancedContainer = new QWidget(this);
    advancedContainer->setObjectName("voiceAdvancedPanel");
    advancedContainer->setAttribute(Qt::WA_StyledBackground, true);
    advancedContainer->setVisible(false);
    auto *advLayout = new QFormLayout(advancedContainer);
    advLayout->setContentsMargins(12, 12, 12, 12);
    advLayout->setHorizontalSpacing(12);
    advLayout->setVerticalSpacing(8);
    advLayout->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    advLayout->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);

    auto makeSliderRow = [this](QSlider *&slider, QLabel *&value, int min, int max,
                                int valueFieldWidth) {
        auto *row = new QWidget(advancedContainer);
        auto *l = new QHBoxLayout(row);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(4);
        slider = new QSlider(Qt::Horizontal, row);
        slider->setRange(min, max);
        value = new QLabel(row);
        value->setFixedWidth(valueFieldWidth);
        value->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        l->addWidget(slider, 1);
        l->addWidget(value);
        return row;
    };

    applicationCombo = new QComboBox(advancedContainer);
    applicationCombo->addItem(tr("VOIP"), OPUS_APPLICATION_VOIP);
    applicationCombo->addItem(tr("Audio"), OPUS_APPLICATION_AUDIO);
    advLayout->addRow(tr("Application"), applicationCombo);

    advLayout->addRow(tr("Bitrate"),
                      makeSliderRow(bitrateSlider, bitrateValue, 8, 510, 56));
    advLayout->addRow(tr("Complexity"),
                      makeSliderRow(complexitySlider, complexityValue, 0, 10, 36));

    signalTypeCombo = new QComboBox(advancedContainer);
    signalTypeCombo->addItem(tr("Auto"), OPUS_AUTO);
    signalTypeCombo->addItem(tr("Voice"), OPUS_SIGNAL_VOICE);
    signalTypeCombo->addItem(tr("Music"), OPUS_SIGNAL_MUSIC);
    advLayout->addRow(tr("Signal"), signalTypeCombo);

    fecCheckbox = new QCheckBox(tr("Forward Error Correction"), advancedContainer);
    advLayout->addRow(QString(), fecCheckbox);

    advLayout->addRow(tr("Packet Loss"),
                      makeSliderRow(packetLossSlider, packetLossValue, 0, 100, 36));

    diagnosticsLabel = new QLabel(tr("Waiting for audio measurements…"), advancedContainer);
    diagnosticsLabel->setWordWrap(true);
    diagnosticsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    advLayout->addRow(tr("Diagnostics"), diagnosticsLabel);
#ifdef ACHERON_HAVE_FFMPEG
    if (qEnvironmentVariable("ACHERON_GOLIVE_PROBE") == QStringLiteral("1")) {
        auto *probe = new QPushButton(tr("Open Go Live interoperability probe"), advancedContainer);
        advLayout->addRow(probe);
        connect(probe, &QPushButton::clicked, this, [this, probe] {
            if (!voiceManager || !voiceManager->isConnected())
                return;
            auto *dialog = new GoLiveProbeDialog(voiceManager, accountId, this);
            probe->setEnabled(false);
            connect(dialog, &QObject::destroyed, probe, [probe] { probe->setEnabled(true); });
            dialog->show();
        });
    }
#endif

#ifdef ACHERON_HAVE_RNNOISE
    noiseSuppressionCheckbox = new QCheckBox(tr("Noise Suppression"), advancedContainer);
    advLayout->addRow(QString(), noiseSuppressionCheckbox);

    rnnoiseVadCheckbox = new QCheckBox(tr("Use RNNoise voice detection"), advancedContainer);
    advLayout->addRow(QString(), rnnoiseVadCheckbox);
#endif

    parentLayout->addWidget(advancedContainer);

    installResetOnDoubleClick(applicationCombo, OPUS_APPLICATION_VOIP);
    installResetOnDoubleClick(bitrateSlider, 64);
    installResetOnDoubleClick(complexitySlider, 5);
    installResetOnDoubleClick(signalTypeCombo, OPUS_SIGNAL_VOICE);
    installResetOnDoubleClick(fecCheckbox, true);
    installResetOnDoubleClick(packetLossSlider, 10);
#ifdef ACHERON_HAVE_RNNOISE
    installResetOnDoubleClick(noiseSuppressionCheckbox, true);
    installResetOnDoubleClick(rnnoiseVadCheckbox, true);
#endif

    connect(advancedToggle, &QToolButton::toggled, this, [this](bool checked) {
        advancedToggle->setArrowType(checked ? Qt::DownArrow : Qt::RightArrow);
        advancedContainer->setVisible(checked);
        adjustSize();
    });

    connect(applicationCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        int value = applicationCombo->itemData(index).toInt();
        QSettings().setValue("voice/codec/application", value);
        if (voiceManager)
            voiceManager->setOpusApplication(value);
    });

    connect(bitrateSlider, &QSlider::valueChanged, this, [this](int value) {
        bitrateValue->setText(QString("%1 kbps").arg(value));
        QSettings().setValue("voice/codec/bitrate", value);
        if (voiceManager)
            voiceManager->setOpusBitrate(value * 1000);
    });

    connect(complexitySlider, &QSlider::valueChanged, this, [this](int value) {
        complexityValue->setText(QString::number(value));
        QSettings().setValue("voice/codec/complexity", value);
        if (voiceManager)
            voiceManager->setOpusComplexity(value);
    });

    connect(signalTypeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
        int value = signalTypeCombo->itemData(index).toInt();
        QSettings().setValue("voice/codec/signal_type", value);
        if (voiceManager)
            voiceManager->setOpusSignalType(value);
    });

    connect(fecCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        QSettings().setValue("voice/codec/fec", checked);
        if (voiceManager)
            voiceManager->setOpusFec(checked);
    });

    connect(packetLossSlider, &QSlider::valueChanged, this, [this](int value) {
        packetLossValue->setText(QString("%1%").arg(value));
        QSettings().setValue("voice/codec/packet_loss", value);
        if (voiceManager)
            voiceManager->setOpusPacketLossPercent(value);
    });

#ifdef ACHERON_HAVE_RNNOISE
    connect(noiseSuppressionCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        QSettings().setValue("voice/noise_suppression", checked);
        if (voiceManager)
            voiceManager->setNoiseSuppressionEnabled(checked);
    });

    connect(rnnoiseVadCheckbox, &QCheckBox::toggled, this, [this](bool checked) {
        QSettings().setValue("voice/rnnoise_vad", checked);
        if (voiceManager)
            voiceManager->setUseRnnoiseVad(checked);
    });
#endif
}

void VoiceWindow::loadCodecSettings()
{
    QSettings settings;
    int application = settings.value("voice/codec/application", OPUS_APPLICATION_VOIP).toInt();
    int bitrate = settings.value("voice/codec/bitrate", 64).toInt();
    int complexity = settings.value("voice/codec/complexity", 5).toInt();
    int signalType = settings.value("voice/codec/signal_type", OPUS_SIGNAL_VOICE).toInt();
    bool fec = settings.value("voice/codec/fec", true).toBool();
    int packetLoss = settings.value("voice/codec/packet_loss", 10).toInt();

    int appIdx = applicationCombo->findData(application);
    if (appIdx >= 0)
        applicationCombo->setCurrentIndex(appIdx);
    bitrateSlider->setValue(bitrate);
    bitrateValue->setText(QString("%1 kbps").arg(bitrate));
    complexitySlider->setValue(complexity);
    complexityValue->setText(QString::number(complexity));
    int idx = signalTypeCombo->findData(signalType);
    if (idx >= 0)
        signalTypeCombo->setCurrentIndex(idx);
    fecCheckbox->setChecked(fec);
    packetLossSlider->setValue(packetLoss);
    packetLossValue->setText(QString("%1%").arg(packetLoss));

#ifdef ACHERON_HAVE_RNNOISE
    noiseSuppressionCheckbox->setChecked(settings.value("voice/noise_suppression", true).toBool());
    rnnoiseVadCheckbox->setChecked(settings.value("voice/rnnoise_vad", true).toBool());
#endif
}

void VoiceWindow::applyCodecSettingsToManager()
{
    if (!voiceManager)
        return;
    voiceManager->setOpusApplication(applicationCombo->currentData().toInt());
    voiceManager->setOpusBitrate(bitrateSlider->value() * 1000);
    voiceManager->setOpusComplexity(complexitySlider->value());
    voiceManager->setOpusSignalType(signalTypeCombo->currentData().toInt());
    voiceManager->setOpusFec(fecCheckbox->isChecked());
    voiceManager->setOpusPacketLossPercent(packetLossSlider->value());
#ifdef ACHERON_HAVE_RNNOISE
    voiceManager->setNoiseSuppressionEnabled(noiseSuppressionCheckbox->isChecked());
    voiceManager->setUseRnnoiseVad(rnnoiseVadCheckbox->isChecked());
#endif
}

bool VoiceWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() != QEvent::MouseButtonDblClick)
        return QWidget::eventFilter(obj, event);

    QVariant defaultValue = obj->property("defaultValue");
    if (!defaultValue.isValid())
        return QWidget::eventFilter(obj, event);

    if (auto *slider = qobject_cast<QSlider *>(obj)) {
        slider->setValue(defaultValue.toInt());
        return true;
    }
    if (auto *combo = qobject_cast<QComboBox *>(obj)) {
        int idx = combo->findData(defaultValue);
        if (idx >= 0)
            combo->setCurrentIndex(idx);
        return true;
    }
    if (auto *check = qobject_cast<QCheckBox *>(obj)) {
        check->setChecked(defaultValue.toBool());
        return true;
    }
    return QWidget::eventFilter(obj, event);
}

void VoiceWindow::installResetOnDoubleClick(QWidget *widget, const QVariant &defaultValue)
{
    widget->setProperty("defaultValue", defaultValue);
    widget->installEventFilter(this);
    widget->setToolTip(tr("Double-click to reset to default"));
}

void VoiceWindow::setVoiceManager(Core::Audio::VoiceManager *manager)
{
    if (voiceManager == manager)
        return;

    disconnectManager();
    voiceManager = manager;

    diagnosticsLabel->setText(tr("Waiting for audio measurements…"));

    if (!voiceManager)
        return;

    connect(voiceManager, &Core::Audio::VoiceManager::audioLevelChanged, volumeMeter, &VolumeMeter::setLevel);
    connect(voiceManager, &Core::Audio::VoiceManager::diagnosticsUpdated, this,
            [this](const QJsonObject &stats) {
                const QString send = stats.value("capture_to_send_samples").toInt() > 0
                    ? tr("%1 ms").arg(stats.value("capture_to_send_p95_ms").toInt()) : tr("No speech samples");
                diagnosticsLabel->setText(tr("Microphone to send, p95: %1\n"
                    "Capture/send drops: %2 / %3\nPlayback underruns: %4; queued: %5 ms")
                    .arg(send).arg(stats.value("capture_dropped_frames").toVariant().toULongLong())
                    .arg(stats.value("send_queue_dropped_frames").toVariant().toULongLong())
                    .arg(stats.value("playback_underruns").toVariant().toULongLong())
                    .arg(stats.value("playback_queued_ms").toInt()));
            });
    connect(voiceManager, &Core::Audio::VoiceManager::devicesChanged, this, &VoiceWindow::refreshDevices);
    connect(voiceManager, &Core::Audio::VoiceManager::participantJoined, this, &VoiceWindow::onParticipantJoined);
    connect(voiceManager, &Core::Audio::VoiceManager::participantLeft, this, &VoiceWindow::onParticipantLeft);
    connect(voiceManager, &Core::Audio::VoiceManager::participantUpdated, this, &VoiceWindow::onParticipantUpdated);
    connect(voiceManager, &Core::Audio::VoiceManager::participantSpeakingChanged, this, &VoiceWindow::onParticipantSpeakingChanged);
    connect(voiceManager, &Core::Audio::VoiceManager::userAudioLevelChanged, this, &VoiceWindow::onUserAudioLevelChanged);
    connect(voiceManager, &Core::Audio::VoiceManager::participantsCleared, this, &VoiceWindow::onParticipantsCleared);
    connect(voiceManager, &Core::Audio::VoiceManager::privacyCodeChanged,
            this, [this](const QString &code) {
                bool active = !code.isEmpty();
                privacyCodeBtn->setVisible(active);
                for (auto *w : userWidgets)
                    w->setDaveActive(active);
            });

    privacyCodeBtn->setVisible(!voiceManager->privacyCode().isEmpty());

    const auto existing = voiceManager->currentParticipants();
    for (const auto &p : existing)
        onParticipantJoined(p.userId);

    applyCodecSettingsToManager();
    refreshDevices();
}

void VoiceWindow::setNameResolver(NameResolver resolver)
{
    nameResolver = std::move(resolver);

    for (auto it = userWidgets.constBegin(); it != userWidgets.constEnd(); ++it) {
        QString name = nameResolver ? nameResolver(it.key()) : QString::number(it.key());
        it.value()->setDisplayName(name);
    }
}

void VoiceWindow::setAvatarResolver(AvatarResolver resolver)
{
    avatarResolver = std::move(resolver);

    for (auto it = userWidgets.begin(); it != userWidgets.end(); ++it)
        requestAvatar(it.key(), it.value());
}

void VoiceWindow::setImageManager(Core::ImageManager *manager)
{
    if (imageManager == manager)
        return;

    if (imageFetchedConn)
        QObject::disconnect(imageFetchedConn);

    imageManager = manager;
    avatarTracker.clear();

    if (imageManager) {
        imageFetchedConn = connect(imageManager, &Core::ImageManager::imageFetched,
                                   this, &VoiceWindow::onImageFetched);
    }
}

void VoiceWindow::setAccount(Core::Snowflake id)
{
    if (accountId == id)
        return;

    accountId = id;
    avatarTracker.clear();
}

void VoiceWindow::refreshDevices()
{
    if (!voiceManager)
        return;

    auto populateCombo = [](QComboBox *combo, const QList<Core::Audio::AudioDeviceInfo> &devices,
                            const QByteArray &currentId) {
        combo->blockSignals(true);
        combo->clear();
        int selectedIndex = 0;
        for (int i = 0; i < devices.size(); i++) {
            const auto &dev = devices[i];
            QString label = dev.description;
            if (dev.isDefault)
                label += QObject::tr(" (Default)");
            combo->addItem(label, dev.id);
            if (!currentId.isEmpty() && dev.id == currentId)
                selectedIndex = i;
            else if (currentId.isEmpty() && dev.isDefault)
                selectedIndex = i;
        }
        combo->setCurrentIndex(selectedIndex);
        combo->blockSignals(false);
    };

    populateCombo(inputDeviceCombo, voiceManager->availableInputDevices(), voiceManager->currentInputDevice());
    populateCombo(outputDeviceCombo, voiceManager->availableOutputDevices(), voiceManager->currentOutputDevice());
}

void VoiceWindow::disconnectManager()
{
    if (voiceManager) {
        disconnect(voiceManager, nullptr, this, nullptr);
        disconnect(voiceManager, nullptr, volumeMeter, nullptr);
    }
    voiceManager = nullptr;

    qDeleteAll(userWidgets);
    userWidgets.clear();
    avatarTracker.clear();
    privacyCodeBtn->hide();
}

void VoiceWindow::onParticipantJoined(Core::Snowflake userId)
{
    if (userWidgets.contains(userId))
        return;

    auto *widget = new VoiceUserWidget(userId, userListContainer);

    QString name = nameResolver ? nameResolver(userId) : QString::number(userId);
    widget->setDisplayName(name);

    if (voiceManager) {
        widget->setLocallyMuted(voiceManager->isUserMuted(userId));
        widget->setDaveActive(!voiceManager->privacyCode().isEmpty());

        const auto *p = voiceManager->participant(userId);
        if (p)
            widget->setVoiceStateFlags(p->selfMute, p->selfDeaf, p->serverMute, p->serverDeaf, p->suppress);
    }

    connect(widget, &VoiceUserWidget::volumeChanged, this, [this](Core::Snowflake uid, int pct) {
        if (voiceManager)
            voiceManager->setUserVolume(uid, static_cast<float>(pct) / 100.0f);
    });

    connect(widget, &VoiceUserWidget::muteToggled, this, [this](Core::Snowflake uid, bool muted) {
        if (!voiceManager)
            return;
        voiceManager->setUserMuted(uid, muted);

        if (!muted) {
            auto it = userWidgets.constFind(uid);
            if (it != userWidgets.constEnd())
                voiceManager->setUserVolume(uid, static_cast<float>((*it)->volume()) / 100.0f);
        }
    });

    connect(widget, &VoiceUserWidget::verificationCodeRequested,
            this, &VoiceWindow::showVerificationCode);

    userListLayout->insertWidget(userListLayout->count() - 1, widget);
    userWidgets.insert(userId, widget);

    requestAvatar(userId, widget);
}

void VoiceWindow::onParticipantLeft(Core::Snowflake userId)
{
    auto it = userWidgets.find(userId);
    if (it == userWidgets.end())
        return;

    delete it.value();
    userWidgets.erase(it);
}

void VoiceWindow::onParticipantUpdated(Core::Snowflake userId)
{
    auto it = userWidgets.constFind(userId);
    if (it == userWidgets.constEnd())
        return;

    if (!voiceManager)
        return;

    const auto *p = voiceManager->participant(userId);
    if (p)
        it.value()->setVoiceStateFlags(p->selfMute, p->selfDeaf, p->serverMute, p->serverDeaf, p->suppress);
}

void VoiceWindow::onParticipantSpeakingChanged(Core::Snowflake userId, bool speaking)
{
    auto it = userWidgets.constFind(userId);
    if (it != userWidgets.constEnd())
        it.value()->setSpeaking(speaking);
}

void VoiceWindow::onUserAudioLevelChanged(Core::Snowflake userId, float rms)
{
    auto it = userWidgets.constFind(userId);
    if (it != userWidgets.constEnd())
        it.value()->setAudioLevel(rms);
}

void VoiceWindow::onParticipantsCleared()
{
    qDeleteAll(userWidgets);
    userWidgets.clear();
    avatarTracker.clear();
}

void VoiceWindow::onImageFetched(const QUrl &url, const QSize &size, const QPixmap &pixmap)
{
    if (size != AVATAR_REQUEST_SIZE)
        return;

    avatarTracker.notify(url, [this, &pixmap](Core::Snowflake userId) {
        auto widgetIt = userWidgets.constFind(userId);
        if (widgetIt != userWidgets.constEnd())
            widgetIt.value()->setAvatar(pixmap);
    });
}

static QDialog *createCodeDialog(QWidget *parent, const QString &title,
                                 const QString &description, const QString &code)
{
    auto *dlg = new QDialog(parent);
    dlg->setWindowTitle(title);
    dlg->setAttribute(Qt::WA_DeleteOnClose);

    auto *layout = new QVBoxLayout(dlg);
    layout->setSpacing(12);
    layout->setContentsMargins(20, 16, 20, 16);

    auto *desc = new QLabel(description, dlg);
    desc->setWordWrap(true);
    layout->addWidget(desc);

    auto *codeLabel = new QLabel(code, dlg);
    codeLabel->setAlignment(Qt::AlignCenter);
    codeLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont codeFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    codeFont.setPointSize(codeFont.pointSize() + 4);
    codeFont.setLetterSpacing(QFont::AbsoluteSpacing, 1);
    codeLabel->setFont(codeFont);
    codeLabel->setStyleSheet(
            "QLabel { background: palette(base); border: 1px solid palette(mid);"
            "  border-radius: 6px; padding: 12px 16px; }");
    layout->addWidget(codeLabel);

    auto *btnBox = new QDialogButtonBox(QDialogButtonBox::Close, dlg);
    QObject::connect(btnBox, &QDialogButtonBox::rejected, dlg, &QDialog::close);
    layout->addWidget(btnBox);

    dlg->setMinimumWidth(340);
    return dlg;
}

void VoiceWindow::showVerificationCode(Core::Snowflake userId)
{
    if (!voiceManager) {
        QMessageBox::information(this, tr("Verification Code"), tr("Not connected to voice."));
        return;
    }

    if (!voiceManager->isDaveEnabled()) {
        QMessageBox::information(this, tr("Verification Code"),
                                 tr("End-to-end encryption is not active in this call."));
        return;
    }

    QString userName = nameResolver ? nameResolver(userId) : QString::number(userId);
    QPointer<VoiceWindow> self = this;

    voiceManager->requestVerificationCode(userId, [self, userName](const QString &code) {
        // callback triggered from dave created thread
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, userName, code]() {
            if (!self)
                return;

            if (code.isEmpty()) {
                QMessageBox::warning(self, VoiceWindow::tr("Verification Code"),
                                     VoiceWindow::tr("Could not generate verification code for %1.\n"
                                                     "They may not be in the MLS group yet.")
                                             .arg(userName));
                return;
            }

            auto *dlg = createCodeDialog(
                    self, VoiceWindow::tr("Verification Code"),
                    VoiceWindow::tr("Compare this code with <b>%1</b> to verify end-to-end encryption. "
                                    "Both sides should see the same code.")
                            .arg(userName.toHtmlEscaped()),
                    code);
            dlg->show();
        });
    });
}

void VoiceWindow::showPrivacyCode()
{
    if (!voiceManager) {
        QMessageBox::information(this, tr("Privacy Code"), tr("Not connected to voice."));
        return;
    }

    if (!voiceManager->isDaveEnabled()) {
        QMessageBox::information(this, tr("Privacy Code"),
                                 tr("End-to-end encryption is not active in this call."));
        return;
    }

    QString code = voiceManager->privacyCode();
    if (code.isEmpty()) {
        QMessageBox::warning(this, tr("Privacy Code"),
                             tr("Privacy code is not yet available."));
        return;
    }

    auto *dlg = createCodeDialog(
            this, tr("Privacy Code"),
            tr("This code is shared by everyone in this call. "
               "If everyone sees the same code, the call is end-to-end encrypted."),
            code);
    dlg->show();
}

void VoiceWindow::requestAvatar(Core::Snowflake userId, VoiceUserWidget *widget)
{
    if (!imageManager || !avatarResolver)
        return;

    QUrl url = avatarResolver(userId);
    if (!url.isValid())
        return;

    QPixmap pm = avatarTracker.fetch(imageManager, url, AVATAR_REQUEST_SIZE, userId, accountId);
    widget->setAvatar(pm);
}

} // namespace UI
} // namespace Acheron
