#include "VoiceStatusBar.hpp"
#include "VoiceWindow.hpp"

#include "Core/Audio/VoiceManager.hpp"
#include "Core/Theme/Icons.hpp"
#include "Core/Theme/Manager.hpp"
#include "Core/Theme/Tokens.hpp"

#include <QHBoxLayout>
#include <QMouseEvent>

namespace Acheron {
namespace UI {

VoiceStatusBar::VoiceStatusBar(QWidget *parent) : QWidget(parent)
{
    setupUi();
    hide();

    connect(&Core::Theme::Manager::instance(), &Core::Theme::Manager::themeChanged,
            this, &VoiceStatusBar::applyTheme);

    closeTimer.setSingleShot(true);
    closeTimer.setInterval(500);
    connect(&closeTimer, &QTimer::timeout, this, [this]() {
        if (voiceWindow)
            voiceWindow->close();
    });
}

void VoiceStatusBar::setupUi()
{
    setObjectName("voiceStatusBar");
    setAttribute(Qt::WA_StyledBackground, true);
    setFixedHeight(44);
    setCursor(Qt::PointingHandCursor);
    setToolTip(tr("Open voice controls"));
    setAccessibleName(tr("Voice connection status"));

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 0, 8, 0);
    layout->setSpacing(8);

    statusDot = new QLabel(this);
    statusDot->setFixedSize(8, 8);
    statusDot->setObjectName("voiceStatusDot");

    statusLabel = new QLabel(this);
    statusLabel->setObjectName("voiceStatusLabel");

    channelLabel = new QLabel(this);
    channelLabel->setObjectName("voiceChannelLabel");

    disconnectBtn = new QPushButton(this);
    disconnectBtn->setObjectName("voiceDisconnectButton");
    disconnectBtn->setFixedSize(32, 32);
    disconnectBtn->setIcon(Core::Theme::Icons::icon(Core::Theme::Icons::Name::X,
                                                     Core::Theme::Token::Danger));
    disconnectBtn->setIconSize(QSize(16, 16));
    disconnectBtn->setCursor(Qt::PointingHandCursor);
    disconnectBtn->setToolTip(tr("Disconnect from voice"));
    disconnectBtn->setAccessibleName(tr("Disconnect from voice"));
    connect(disconnectBtn, &QPushButton::clicked, this, &VoiceStatusBar::disconnectRequested);

    layout->addWidget(statusDot);
    layout->addWidget(statusLabel);
    layout->addWidget(channelLabel);
    layout->addStretch();
    layout->addWidget(disconnectBtn);

    applyTheme();
}

void VoiceStatusBar::applyTheme()
{
    const auto &theme = Core::Theme::Manager::instance();
    setStyleSheet(QStringLiteral(
            "#voiceStatusBar { background: %1; border-top: 1px solid %2; }"
            "#voiceStatusBar:hover { background: %3; }"
            "#voiceChannelLabel { color: %4; }"
            "#voiceDisconnectButton { background: transparent; border: 1px solid transparent; }"
            "#voiceDisconnectButton:hover { background: %3; border-color: %2; }"
            "#voiceDisconnectButton:focus { border-color: %5; }")
                          .arg(theme.color(Core::Theme::Token::AlternateBaseBg).name(),
                               theme.color(Core::Theme::Token::Divider).name(),
                               theme.color(Core::Theme::Token::SurfaceHover).name(),
                               theme.color(Core::Theme::Token::PlaceholderText).name(),
                               theme.color(Core::Theme::Token::Highlight).name()));
    updateConnectionState();
}

void VoiceStatusBar::setVoiceManager(Core::Audio::VoiceManager *manager)
{
    if (voiceManager == manager)
        return;

    disconnectManager();
    voiceManager = manager;

    if (!voiceManager) {
        if (voiceWindow)
            voiceWindow->setVoiceManager(nullptr);
        updateConnectionState();
        return;
    }

    connect(voiceManager, &Core::Audio::VoiceManager::voiceStateChanged,
            this, &VoiceStatusBar::updateConnectionState);

    configureVoiceWindow();
    updateConnectionState();
}

void VoiceStatusBar::setNameResolver(NameResolver resolver)
{
    nameResolver = std::move(resolver);
    if (voiceWindow)
        voiceWindow->setNameResolver(nameResolver);
}

void VoiceStatusBar::setAvatarResolver(AvatarResolver resolver)
{
    avatarResolver = std::move(resolver);
    if (voiceWindow)
        voiceWindow->setAvatarResolver(avatarResolver);
}

void VoiceStatusBar::setImageManager(Core::ImageManager *manager)
{
    imageManager = manager;
    if (voiceWindow)
        voiceWindow->setImageManager(manager);
}

void VoiceStatusBar::setAccount(Core::Snowflake id)
{
    accountId = id;
    if (voiceWindow)
        voiceWindow->setAccount(id);
}

void VoiceStatusBar::setChannelName(const QString &name)
{
    if (name.isEmpty())
        channelLabel->hide();
    else {
        channelLabel->setText(name);
        channelLabel->show();
    }
}

void VoiceStatusBar::updateConnectionState()
{
    using State = Discord::Voice::VoiceClient::State;

    State state = State::Disconnected;
    if (voiceManager)
        state = voiceManager->clientState();

    bool connected = (state == State::Connected);
    bool connecting = (state == State::Connecting || state == State::Identifying ||
                       state == State::WaitingForReady || state == State::DiscoveringIP ||
                       state == State::SelectingProtocol || state == State::WaitingForSession);
    bool active = connected || connecting;

    QColor statusColor;
    QString stateText;

    if (connected) {
        stateText = tr("Voice Connected");
        statusColor = Core::Theme::Manager::instance().color(Core::Theme::Token::Success);
    } else if (connecting) {
        if (state == State::Connecting)
            stateText = tr("Connecting...");
        else if (state == State::Identifying)
            stateText = tr("Identifying...");
        else
            stateText = tr("Securing...");
        statusColor = Core::Theme::Manager::instance().color(Core::Theme::Token::Warning);
    } else {
        stateText = tr("Disconnected");
        statusColor = Core::Theme::Manager::instance().color(Core::Theme::Token::DisabledText);
    }

    const QString color = statusColor.name();
    statusDot->setStyleSheet(QStringLiteral("background-color: %1; border-radius: 4px;").arg(color));
    statusLabel->setText(stateText);
    statusLabel->setStyleSheet(QStringLiteral("color: %1; font-weight: 650;").arg(color));
    setAccessibleDescription(channelLabel->isVisible()
                                     ? QStringLiteral("%1, %2").arg(stateText, channelLabel->text())
                                     : stateText);

    setVisible(active);

    if (active && wasDisconnected) {
        closeTimer.stop();
        showVoiceWindow();
        if (voiceWindow)
            voiceWindow->refreshDevices();
    } else if (active) {
        closeTimer.stop();
    }

    if (!active && voiceWindow)
        closeTimer.start();

    wasDisconnected = !active;
}

void VoiceStatusBar::mousePressEvent(QMouseEvent *event)
{
    if (disconnectBtn->geometry().contains(event->pos())) {
        QWidget::mousePressEvent(event);
        return;
    }

    toggleVoiceWindow();
}

void VoiceStatusBar::toggleVoiceWindow()
{
    if (voiceWindow && voiceWindow->isVisible())
        voiceWindow->close();
    else
        showVoiceWindow();
}

void VoiceStatusBar::showVoiceWindow()
{
    if (!voiceWindow) {
        voiceWindow = new VoiceWindow(window());
        configureVoiceWindow();
    }

    voiceWindow->show();
    voiceWindow->raise();
    voiceWindow->activateWindow();
}

void VoiceStatusBar::configureVoiceWindow()
{
    if (!voiceWindow)
        return;
    if (imageManager)
        voiceWindow->setImageManager(imageManager);
    voiceWindow->setAccount(accountId);
    if (nameResolver)
        voiceWindow->setNameResolver(nameResolver);
    if (avatarResolver)
        voiceWindow->setAvatarResolver(avatarResolver);
    if (voiceManager)
        voiceWindow->setVoiceManager(voiceManager);
}

void VoiceStatusBar::disconnectManager()
{
    if (voiceManager)
        disconnect(voiceManager, nullptr, this, nullptr);
    voiceManager = nullptr;
}

} // namespace UI
} // namespace Acheron
