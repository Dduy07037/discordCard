#pragma once

#include <QtWidgets>

namespace Acheron {
namespace UI {

class SettingsWindow : public QWidget
{
    Q_OBJECT
public:
    explicit SettingsWindow(QWidget *parent = nullptr);

signals:
    void channelListModeChanged(bool classic);

private:
    void setupUi();

    QListWidget *categoryList;
    QStackedWidget *pages;
};

} // namespace UI
} // namespace Acheron
