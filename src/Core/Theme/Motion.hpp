#pragma once

#include <QAbstractAnimation>
#include <QEasingCurve>
#include <QPropertyAnimation>
#include <QWidget>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace Acheron {
namespace Core {
namespace Theme {
namespace Motion {

inline constexpr int FastDurationMs = 120;

inline bool animationsEnabled()
{
#ifdef Q_OS_WIN
    BOOL enabled = TRUE;
    if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &enabled, 0))
        return enabled == TRUE;
#endif
    return true;
}

inline void prepareFadeIn(QWidget *widget)
{
    widget->setWindowOpacity(animationsEnabled() ? 0.0 : 1.0);
}

inline void startFadeIn(QWidget *widget)
{
    if (!animationsEnabled()) {
        widget->setWindowOpacity(1.0);
        return;
    }
    if (widget->windowOpacity() > 0.0)
        return;

    auto *animation = new QPropertyAnimation(widget, "windowOpacity");
    animation->setDuration(FastDurationMs);
    animation->setStartValue(0.0);
    animation->setEndValue(1.0);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

} // namespace Motion
} // namespace Theme
} // namespace Core
} // namespace Acheron
