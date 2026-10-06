#include "GeneralPage.hpp"

#include <QCheckBox>
#include <QLabel>
#include <QSettings>
#include <QVBoxLayout>

namespace Acheron {
namespace UI {

GeneralPage::GeneralPage(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    inMemoryCacheCheckbox = new QCheckBox(tr("Keep the cache database in memory"), this);
    inMemoryCacheCheckbox->setToolTip(tr("Requires an application restart"));
    inMemoryCacheCheckbox->setChecked(QSettings().value("general/in_memory_cache", false).toBool());
    layout->addWidget(inMemoryCacheCheckbox);
    auto *description = new QLabel(
            tr("Can reduce disk activity during a session. The change takes effect after restart."),
            this);
    description->setWordWrap(true);
    description->setStyleSheet("color: palette(placeholder-text);");
    layout->addWidget(description);
    layout->addStretch();

    connect(inMemoryCacheCheckbox, &QCheckBox::toggled, this, [](bool checked) {
        QSettings settings;
        settings.setValue("general/in_memory_cache", checked);
    });
}

} // namespace UI
} // namespace Acheron
