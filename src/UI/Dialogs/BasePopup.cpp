#include "BasePopup.hpp"

#include "Core/Theme/Motion.hpp"

#include <QVBoxLayout>
#include <QPainter>
#include <QMouseEvent>
#include <QEvent>

namespace Acheron {
namespace UI {

BasePopup::BasePopup(QWidget *parent) : QDialog(parent, Qt::FramelessWindowHint | Qt::Dialog)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setModal(true);

    auto *overlayLayout = new QVBoxLayout(this);
    overlayLayout->setAlignment(Qt::AlignCenter);
    overlayLayout->setContentsMargins(20, 20, 20, 20);

    container = new QFrame(this);
    container->setObjectName("ContentFrame");
    container->setAutoFillBackground(true);
    container->setFrameShape(QFrame::NoFrame);

    container->setMinimumWidth(300);
    container->setMaximumWidth(600);
    container->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    overlayLayout->addWidget(container);
}

void BasePopup::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, 110));
}

void BasePopup::mousePressEvent(QMouseEvent *event)
{
    if (!container->geometry().contains(event->pos())) {
        reject();
    } else {
        QDialog::mousePressEvent(event);
    }
}

void BasePopup::showEvent(QShowEvent *event)
{
    Core::Theme::Motion::prepareFadeIn(this);
    if (parentWidget() && parentWidget()->window()) {
        QWidget *topLevel = parentWidget()->window();
        setGeometry(topLevel->geometry());
        topLevel->installEventFilter(this);
    }
    QDialog::showEvent(event);
    Core::Theme::Motion::startFadeIn(this);
}

bool BasePopup::eventFilter(QObject *obj, QEvent *event)
{
    if (parentWidget() && obj == parentWidget()->window()) {
        if (event->type() == QEvent::Resize || event->type() == QEvent::Move) {
            setGeometry(parentWidget()->window()->geometry());
        }
    }
    return QDialog::eventFilter(obj, event);
}

} // namespace UI
} // namespace Acheron
