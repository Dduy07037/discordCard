#pragma once

#include <QWidget>

class QCheckBox;

namespace Acheron {
namespace UI {

class GeneralPage : public QWidget
{
    Q_OBJECT
public:
    explicit GeneralPage(QWidget *parent = nullptr);

private:
    QCheckBox *inMemoryCacheCheckbox;
};

} // namespace UI
} // namespace Acheron
