#pragma once
#include <QWidget>
#include <QImage>
#include <QPointer>

class QDialog;
class QLabel;
namespace Acheron::UI {
class StreamVideoCanvas;

// Retain the decoded image at its original resolution. Zoom and window resize
// never rescale a previously reduced thumbnail or create another stream session.
class StreamVideoView : public QWidget
{
    Q_OBJECT
public:
    explicit StreamVideoView(QWidget *parent = nullptr, bool detached = false);
    void setFrame(const QImage &image);
    void clear(const QString &message = {});
    double zoom() const;
    QSize frameSize() const;
    void setZoom(double value);
    void fit();
    void openFullscreen();
private:
    StreamVideoCanvas *canvas;
    QLabel *zoomLabel;
    QPointer<QDialog> fullscreenWindow;
    QPointer<StreamVideoView> fullscreenView;
};
} // namespace Acheron::UI
