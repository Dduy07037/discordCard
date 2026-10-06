#pragma once
#include <QLabel>

namespace Acheron {
namespace UI {

class ElidedLabel : public QLabel
{
    Q_OBJECT
public:
    explicit ElidedLabel(QWidget *parent = nullptr);
    explicit ElidedLabel(const QString &text, QWidget *parent = nullptr);

    void setText(const QString &text);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
};

} // namespace UI
} // namespace Acheron