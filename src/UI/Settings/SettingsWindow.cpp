#include "SettingsWindow.hpp"

#include "AppearancePage.hpp"
#include "GeneralPage.hpp"

#include <QLabel>
#include <QVBoxLayout>

#ifdef ACHERON_HAVE_MINIAUDIO
#include "AudioPage.hpp"
#endif

namespace Acheron {
namespace UI {

SettingsWindow::SettingsWindow(QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setWindowTitle(tr("Settings"));
    setMinimumSize(620, 440);
    resize(720, 520);

    setupUi();
}

void SettingsWindow::setupUi()
{
    setObjectName("settingsWindow");
    auto *mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    categoryList = new QListWidget(this);
    categoryList->setObjectName("settingsNavigation");
    categoryList->setFixedWidth(184);
    categoryList->setFrameShape(QFrame::NoFrame);
    categoryList->setSpacing(2);

    pages = new QStackedWidget(this);
    pages->setObjectName("settingsPages");

    auto addPage = [this](const QString &name, QWidget *page) {
        categoryList->addItem(name);

        auto *surface = new QWidget(pages);
        surface->setObjectName("settingsPage");
        auto *pageLayout = new QVBoxLayout(surface);
        pageLayout->setContentsMargins(28, 24, 28, 24);
        pageLayout->setSpacing(16);

        auto *title = new QLabel(name, surface);
        title->setObjectName("settingsTitle");
        pageLayout->addWidget(title);
        pageLayout->addWidget(page, 1);
        pages->addWidget(surface);
    };

    addPage(tr("General"), new GeneralPage(this));

    auto *appearance = new AppearancePage(this);
    addPage(tr("Appearance"), appearance);
    connect(appearance, &AppearancePage::channelListModeChanged, this, &SettingsWindow::channelListModeChanged);

#ifdef ACHERON_HAVE_MINIAUDIO
    addPage(tr("Audio"), new AudioPage(this));
#endif

    categoryList->setCurrentRow(0);

    connect(categoryList, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);

    mainLayout->addWidget(categoryList);
    mainLayout->addWidget(pages, 1);

    setStyleSheet(
            "#settingsWindow { background: palette(window); }"
            "#settingsNavigation { background: palette(base); border-right: 1px solid palette(mid); border-radius: 0; padding: 12px 8px; }"
            "#settingsNavigation::item { min-height: 38px; padding: 0 12px; border-radius: 6px; color: palette(placeholder-text); }"
            "#settingsNavigation::item:hover { background: palette(alternate-base); color: palette(window-text); }"
            "#settingsNavigation::item:selected { background: palette(button); color: palette(window-text); }"
            "#settingsPages, #settingsPage { background: palette(window); }"
            "#settingsTitle { color: palette(window-text); font-size: 19px; font-weight: 650; }");
}

} // namespace UI
} // namespace Acheron
