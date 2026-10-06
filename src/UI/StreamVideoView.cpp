#include "StreamVideoView.hpp"
#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <functional>

namespace Acheron::UI {
class StreamVideoCanvas : public QWidget
{
public:
    explicit StreamVideoCanvas(QWidget *parent) : QWidget(parent)
    {
        setMinimumSize(240, 120);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setAccessibleName(tr("Live video. Scroll to zoom, drag to pan, double-click for fullscreen."));
    }
    QImage image;
    QString message = tr("No stream video yet");
    double factor = 1.0;
    QPointF pan;
    std::function<void()> zoomChanged, fullscreen;
    QSizeF displayedSize() const
    {
        if (image.isNull()) return {};
        const auto fit = std::min(double(width()) / image.width(), double(height()) / image.height());
        return QSizeF(image.size()) * (fit * factor);
    }
    void clampPan()
    {
        const auto size = displayedSize();
        const double maxX = std::max(0.0, (size.width() - width()) / 2);
        const double maxY = std::max(0.0, (size.height() - height()) / 2);
        pan = {std::clamp(pan.x(), -maxX, maxX), std::clamp(pan.y(), -maxY, maxY)};
    }
    void changeZoom(double value, const QPointF &anchor)
    {
        const auto previous = factor;
        factor = std::clamp(value, 0.25, 8.0);
        const QPointF center(width() / 2.0, height() / 2.0);
        pan = anchor - center - (anchor - center - pan) * (factor / previous);
        clampPan();
        if (zoomChanged) zoomChanged();
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(15, 16, 19));
        if (image.isNull()) {
            painter.setPen(Qt::white);
            painter.drawText(rect().adjusted(10, 10, -10, -10), Qt::AlignCenter | Qt::TextWordWrap, message);
            return;
        }
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        const auto size = displayedSize();
        const QPointF origin((width() - size.width()) / 2 + pan.x(), (height() - size.height()) / 2 + pan.y());
        painter.drawImage(QRectF(origin, size), image);
    }
    void resizeEvent(QResizeEvent *event) override
    { QWidget::resizeEvent(event); clampPan(); update(); }
    void wheelEvent(QWheelEvent *event) override
    {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
        const auto point = event->position();
#else
        const auto point = event->posF();
#endif
        changeZoom(factor * std::pow(1.2, event->angleDelta().y() / 120.0), point);
        event->accept();
    }
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) { dragging = true; lastMouse = event->pos(); setCursor(Qt::ClosedHandCursor); event->accept(); }
        else QWidget::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!dragging) return;
        pan += event->pos() - lastMouse;
        lastMouse = event->pos();
        clampPan(); update(); event->accept();
    }
    void mouseReleaseEvent(QMouseEvent *event) override
    { dragging = false; unsetCursor(); QWidget::mouseReleaseEvent(event); }
    void mouseDoubleClickEvent(QMouseEvent *event) override
    { if (event->button() == Qt::LeftButton && fullscreen) fullscreen(); event->accept(); }
private:
    bool dragging = false;
    QPoint lastMouse;
};

StreamVideoView::StreamVideoView(QWidget *parent, bool detached) : QWidget(parent)
{
    setObjectName(QStringLiteral("streamVideoView"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    canvas = new StreamVideoCanvas(this);
    canvas->setObjectName(QStringLiteral("streamVideoCanvas"));
    layout->addWidget(canvas, 1);
    auto *controls = new QHBoxLayout;
    const auto button = [this, controls](const QString &text, const QString &name) {
        auto *result = new QToolButton(this);
        result->setText(text); result->setAccessibleName(name);
        result->setToolTip(name); controls->addWidget(result); return result;
    };
    auto *minus = button(QStringLiteral("−"), tr("Zoom out"));
    zoomLabel = new QLabel(this);
    controls->addWidget(zoomLabel);
    auto *plus = button(QStringLiteral("+"), tr("Zoom in"));
    auto *reset = button(tr("Fit"), tr("Fit video to window"));
    controls->addStretch();
    auto *screen = button(detached ? tr("Exit fullscreen") : tr("Fullscreen"), tr("Fullscreen video"));
    layout->addLayout(controls);
    canvas->zoomChanged = [this] { zoomLabel->setText(tr("%1% of fit").arg(qRound(canvas->factor * 100))); };
    canvas->fullscreen = [this, detached] { if (detached) window()->close(); else openFullscreen(); };
    connect(minus, &QToolButton::clicked, this, [this] { setZoom(zoom() / 1.2); });
    connect(plus, &QToolButton::clicked, this, [this] { setZoom(zoom() * 1.2); });
    connect(reset, &QToolButton::clicked, this, &StreamVideoView::fit);
    connect(screen, &QToolButton::clicked, this, [this] { canvas->fullscreen(); });
    fit();
}
void StreamVideoView::setFrame(const QImage &image)
{
    if (image.isNull()) return;
    canvas->image = image;
    canvas->clampPan(); canvas->update();
    if (fullscreenView) fullscreenView->setFrame(image);
}
void StreamVideoView::clear(const QString &message)
{
    canvas->image = {};
    canvas->message = message.isEmpty() ? tr("No stream video yet") : message;
    fit();
    if (fullscreenView) fullscreenView->clear(canvas->message);
}
double StreamVideoView::zoom() const { return canvas->factor; }
QSize StreamVideoView::frameSize() const { return canvas->image.size(); }
void StreamVideoView::setZoom(double value)
{ canvas->changeZoom(value, QPointF(canvas->width() / 2.0, canvas->height() / 2.0)); }
void StreamVideoView::fit()
{ canvas->pan = {}; setZoom(1.0); }
void StreamVideoView::openFullscreen()
{
    if (fullscreenWindow) { fullscreenWindow->raise(); fullscreenWindow->activateWindow(); return; }
    fullscreenWindow = new QDialog(this, Qt::Window);
    fullscreenWindow->setAttribute(Qt::WA_DeleteOnClose);
    fullscreenWindow->setWindowTitle(tr("Live stream"));
    auto *layout = new QVBoxLayout(fullscreenWindow);
    fullscreenView = new StreamVideoView(fullscreenWindow, true);
    layout->addWidget(fullscreenView);
    if (canvas->image.isNull()) fullscreenView->clear(canvas->message);
    else fullscreenView->setFrame(canvas->image);
    fullscreenWindow->showFullScreen();
}
} // namespace Acheron::UI
