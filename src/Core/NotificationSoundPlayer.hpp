#pragma once

#include <QByteArray>
#include <QElapsedTimer>

namespace Acheron {
namespace Core {

// A tiny, dependency-free notification chime. On Windows this uses the native
// asynchronous sound API and keeps one small generated WAV in memory.
class NotificationSoundPlayer
{
public:
    NotificationSoundPlayer();

    void play();

private:
    static QByteArray makeMessageSound();

    QByteArray soundData;
    QElapsedTimer throttle;
};

} // namespace Core
} // namespace Acheron
