#include "ElidedLabel.hpp"
#include <QPainter>
#include <QFontMetrics>

namespace Acheron {
namespace UI {

ElidedLabel::ElidedLabel(QWidget *parent) : QLabel(parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

ElidedLabel::ElidedLabel(const QString &text, QWidget *parent) : QLabel(text, parent)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

void ElidedLabel::setText(const QString &text)
{
    QLabel::setText(text);
    setToolTip(text);
    updateGeometry();
}

QSize ElidedLabel::sizeHint() const
{
    return QLabel::sizeHint();
}

QSize ElidedLabel::minimumSizeHint() const
{
    QSize s = QLabel::minimumSizeHint();
    s.setWidth(0);
    return s;
}

void ElidedLabel::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    QFontMetrics metrics(font());

    QString elidedText = metrics.elidedText(text(), Qt::ElideRight, width());

    painter.drawText(rect(), alignment(), elidedText);
}

} // namespace UI
} // namespace Acheron