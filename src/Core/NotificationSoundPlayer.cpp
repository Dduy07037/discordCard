#include "NotificationSoundPlayer.hpp"

#include <QApplication>

#include <algorithm>
#include <cmath>
#include <cstdint>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#endif

namespace Acheron {
namespace Core {

namespace {

void appendLe16(QByteArray &bytes, quint16 value)
{
    bytes.append(static_cast<char>(value & 0xff));
    bytes.append(static_cast<char>((value >> 8) & 0xff));
}

void appendLe32(QByteArray &bytes, quint32 value)
{
    bytes.append(static_cast<char>(value & 0xff));
    bytes.append(static_cast<char>((value >> 8) & 0xff));
    bytes.append(static_cast<char>((value >> 16) & 0xff));
    bytes.append(static_cast<char>((value >> 24) & 0xff));
}

} // namespace

NotificationSoundPlayer::NotificationSoundPlayer()
    : soundData(makeMessageSound())
{
    throttle.start();
}

void NotificationSoundPlayer::play()
{
    // Collapse message storms into one chime, like Discord, and avoid stacking
    // native playback jobs for several messages arriving in the same batch.
    if (throttle.elapsed() < 120)
        return;
    throttle.restart();

#ifdef Q_OS_WIN
    PlaySoundW(reinterpret_cast<LPCWSTR>(soundData.constData()), nullptr,
               SND_ASYNC | SND_MEMORY | SND_NODEFAULT);
#else
    QApplication::beep();
#endif
}

QByteArray NotificationSoundPlayer::makeMessageSound()
{
    constexpr int sampleRate = 22050;
    constexpr int durationMs = 180;
    constexpr int sampleCount = sampleRate * durationMs / 1000;
    constexpr int dataBytes = sampleCount * static_cast<int>(sizeof(qint16));
    constexpr double pi = 3.14159265358979323846;

    QByteArray wav;
    wav.reserve(44 + dataBytes);
    wav.append("RIFF", 4);
    appendLe32(wav, 36 + dataBytes);
    wav.append("WAVEfmt ", 8);
    appendLe32(wav, 16);
    appendLe16(wav, 1); // PCM
    appendLe16(wav, 1); // mono
    appendLe32(wav, sampleRate);
    appendLe32(wav, sampleRate * 2);
    appendLe16(wav, 2);
    appendLe16(wav, 16);
    wav.append("data", 4);
    appendLe32(wav, dataBytes);

    for (int i = 0; i < sampleCount; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const double attack = std::min(1.0, t / 0.008);
        const double release = std::max(0.0, 1.0 - t / (durationMs / 1000.0));
        const double secondNote = t >= 0.065 ? 1.0 : 0.0;
        const double tone = std::sin(2.0 * pi * 659.25 * t) * 0.55 +
                            secondNote * std::sin(2.0 * pi * 880.0 * (t - 0.065)) * 0.45;
        const double envelope = attack * release * release;
        const auto sample = static_cast<qint16>(std::clamp(tone * envelope * 9000.0,
                                                          -32768.0, 32767.0));
        appendLe16(wav, static_cast<quint16>(sample));
    }

    return wav;
}

} // namespace Core
} // namespace Acheron
