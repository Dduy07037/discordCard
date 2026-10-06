#include "AudioPipeline.hpp"
#include "IAudioBackend.hpp"
#include "OpusEncoder.hpp"
#include "OpusDecoder.hpp"
#include "JitterBuffer.hpp"
#include "AudioMixer.hpp"
#include "NoiseSuppressor.hpp"

#include "Core/Logging.hpp"

#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace Acheron {
namespace Core {
namespace Audio {

namespace {

qint64 monotonicMilliseconds()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

} // namespace

AudioPipeline::AudioPipeline(QObject *parent)
    : QObject(parent)
{
}

AudioPipeline::~AudioPipeline()
{
}

void AudioPipeline::start(IAudioBackend *backend, bool capturing)
{
    if (audioBackend)
        return;

    audioBackend = backend;
    connect(audioBackend, &IAudioBackend::audioCaptured, this, &AudioPipeline::onAudioCaptured, Qt::DirectConnection);

    initializeEncoder();

    noiseSuppressor = std::make_unique<NoiseSuppressor>();
    if (!noiseSuppressor->init())
        noiseSuppressor.reset();

    rmsThrottleTimer.start();
    userRmsThrottleTimer.start();
    playbackTargetFrameCount = MIN_PLAYBACK_TARGET_FRAMES;
    stablePlaybackTicks = 0;
    captureAgeCount = 0;
    staleBeforeEncode = 0;
    measuredUnderruns = 0;
    lastDiagnosticsMs = monotonicMilliseconds();
    lastMixMs = 0;
    maxMixLatenessMs = 0;

    if (capturing)
        audioBackend->startCapture();
    reconfigureNoiseSuppressorChannels();

    audioBackend->startPlayback();

    mixTimer = new QTimer(this);
    mixTimer->setTimerType(Qt::PreciseTimer);
    // Check twice per Opus packet. We still consume only complete 20 ms frames
    // below, but get another chance to refill before the device reserve runs out.
    mixTimer->setInterval(MIX_TIMER_INTERVAL_MS);
    connect(mixTimer, &QTimer::timeout, this, &AudioPipeline::onMixTick);
    mixTimer->start();

    qCInfo(LogVoice) << "Audio pipeline started";
}

void AudioPipeline::stop()
{
    if (sendQueue)
        sendQueue->clear();
    delete mixTimer;
    mixTimer = nullptr;

    if (audioBackend) {
        disconnect(audioBackend, &IAudioBackend::audioCaptured, this, &AudioPipeline::onAudioCaptured);
        audioBackend->stopCapture();
        audioBackend->stopPlayback();
    }

    speakers.clear();
    ssrcToUser.clear();
    pendingUserRms.clear();

    encoder.reset();
    noiseSuppressor.reset();

    audioBackend = nullptr;

    isSpeaking = false;
    vadHoldoffCounter = 0;
    playbackTargetFrameCount = MIN_PLAYBACK_TARGET_FRAMES;
    stablePlaybackTicks = 0;

    qCDebug(LogVoice) << "Audio pipeline stopped";
}

void AudioPipeline::startCapture()
{
    if (!audioBackend)
        return;

    audioBackend->startCapture();
    reconfigureNoiseSuppressorChannels();
}

void AudioPipeline::stopCapture()
{
    if (!audioBackend)
        return;

    audioBackend->stopCapture();

    if (sendQueue)
        sendQueue->clear();

    if (isSpeaking) {
        sendTrailingSilence();
        isSpeaking = false;
        vadHoldoffCounter = 0;
        emit speakingChanged(false);
    }
}

void AudioPipeline::onAudioReceived(quint32 ssrc, uint16_t sequence, uint32_t timestamp, const QByteArray &opusData)
{
    auto it = speakers.find(ssrc);
    if (it == speakers.end()) {
        SpeakerState state;
        state.decoder = std::make_unique<OpusDecoder>();
        if (!state.decoder->init(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS)) {
            qCWarning(LogVoice) << "Failed to init decoder for SSRC" << ssrc;
            return;
        }
        state.jitterBuffer = std::make_unique<JitterBuffer>();

        auto [inserted, _] = speakers.emplace(ssrc, std::move(state));
        it = inserted;
    }

    it->second.jitterBuffer->push(sequence, timestamp, opusData);
}

void AudioPipeline::setDeafened(bool deafened)
{
    this->deafened = deafened;
}

void AudioPipeline::setSsrcUserId(quint32 ssrc, Snowflake userId)
{
    ssrcToUser[ssrc] = userId;
}

void AudioPipeline::removeUser(Snowflake userId)
{
    for (auto it = ssrcToUser.begin(); it != ssrcToUser.end();) {
        if (it.value() == userId) {
            speakers.erase(it.key());
            it = ssrcToUser.erase(it);
        } else {
            ++it;
        }
    }
    userVolumes.remove(userId);
    pendingUserRms.remove(userId);
}

void AudioPipeline::setUserVolume(Snowflake userId, float volume)
{
    if (volume == 1.0f)
        userVolumes.remove(userId);
    else
        userVolumes.insert(userId, volume);
}

void AudioPipeline::setInputDevice(const QByteArray &deviceId)
{
    if (!audioBackend)
        return;

    audioBackend->setInputDevice(deviceId);
    reconfigureNoiseSuppressorChannels();
}

void AudioPipeline::reconfigureNoiseSuppressorChannels()
{
    if (noiseSuppressor && audioBackend && audioBackend->isCapturing())
        noiseSuppressor->reconfigure(audioBackend->nativeCaptureChannels());
}

void AudioPipeline::setOutputDevice(const QByteArray &deviceId)
{
    if (audioBackend)
        audioBackend->setOutputDevice(deviceId);
}

void AudioPipeline::setInputGain(float gain)
{
    if (audioBackend)
        audioBackend->setInputGain(gain);
}

void AudioPipeline::setOutputVolume(float volume)
{
    if (audioBackend)
        audioBackend->setOutputVolume(volume);
}

void AudioPipeline::setVadThreshold(float threshold)
{
    vadThreshold = threshold;
}

void AudioPipeline::setNoiseSuppressionEnabled(bool enabled)
{
    noiseSuppressionEnabled = enabled;
}

void AudioPipeline::setUseRnnoiseVad(bool enabled)
{
    useRnnoiseVad = enabled;
}

void AudioPipeline::initializeEncoder()
{
    encoder = std::make_unique<OpusEncoder>();
    if (!encoder->init(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, opusApplication)) {
        qCCritical(LogVoice) << "Failed to initialize Opus encoder";
        encoder.reset();
        return;
    }
    encoder->setBitrate(opusBitrate);
    encoder->setComplexity(opusComplexity);
    encoder->setSignalType(opusSignalType);
    encoder->setFec(opusFec);
    encoder->setPacketLossPercent(opusPacketLossPercent);
}

void AudioPipeline::setOpusApplication(int application)
{
    if (opusApplication == application)
        return;

    opusApplication = application;

    if (encoder)
        initializeEncoder();
}

void AudioPipeline::setOpusBitrate(int bitrate)
{
    opusBitrate = bitrate;

    if (encoder)
        encoder->setBitrate(bitrate);
}

void AudioPipeline::setOpusComplexity(int complexity)
{
    opusComplexity = complexity;

    if (encoder)
        encoder->setComplexity(complexity);
}

void AudioPipeline::setOpusSignalType(int signalType)
{
    opusSignalType = signalType;

    if (encoder)
        encoder->setSignalType(signalType);
}

void AudioPipeline::setOpusFec(bool enabled)
{
    opusFec = enabled;

    if (encoder)
        encoder->setFec(enabled);
}

void AudioPipeline::setOpusPacketLossPercent(int percent)
{
    opusPacketLossPercent = percent;

    if (encoder)
        encoder->setPacketLossPercent(percent);
}

void AudioPipeline::onAudioCaptured(const QByteArray &pcmData, qint64 capturedAtMs)
{
    if (!encoder || pcmData.size() != AUDIO_FRAME_SIZE)
        return;
    const auto age = monotonicMilliseconds() - capturedAtMs;
    if (age < 0 || age > AudioSendQueue::MaxAgeMs) {
        ++staleBeforeEncode;
        return;
    }

    QByteArray frame = pcmData;
    float voiceProb = -1.0f;
    if (noiseSuppressor && (noiseSuppressionEnabled || useRnnoiseVad)) {
        QByteArray denoised = noiseSuppressor->process(pcmData, voiceProb);
        if (noiseSuppressionEnabled)
            frame = denoised;
    }

    float rms = 0.0f;
    bool rmsVoice = detectVoiceActivity(frame, rms);

    bool voiceDetected;
    if (useRnnoiseVad && voiceProb >= 0.0f)
        voiceDetected = voiceProb > vadProbabilityThreshold;
    else
        voiceDetected = rmsVoice;

    if (rmsThrottleTimer.elapsed() >= RMS_EMIT_INTERVAL_MS) {
        emit audioLevelChanged(rms);
        rmsThrottleTimer.restart();
    }

    if (voiceDetected) {
        vadHoldoffCounter = vadHoldoffFrames;
        if (!isSpeaking) {
            isSpeaking = true;
            emit speakingChanged(true);
        }
    } else if (vadHoldoffCounter > 0) {
        vadHoldoffCounter--;
    } else if (isSpeaking) {
        sendTrailingSilence();
        isSpeaking = false;
        emit speakingChanged(false);
        return;
    }

    if (!isSpeaking)
        return;

    QByteArray encoded = encoder->encode(frame);
    if (!encoded.isEmpty()) {
        captureAges[captureAgeCount++ % captureAges.size()] = monotonicMilliseconds() - capturedAtMs;
        submitEncodedAudio(encoded, capturedAtMs);
    }
}

void AudioPipeline::onMixTick()
{
    const auto nowMs = monotonicMilliseconds();
    if (lastMixMs)
        maxMixLatenessMs = std::max(maxMixLatenessMs, nowMs - lastMixMs - MIX_TIMER_INTERVAL_MS);
    lastMixMs = nowMs;
    publishDiagnostics(nowMs);
    if (deafened || !audioBackend)
        return;

    const unsigned int underruns = audioBackend->takePlaybackUnderruns();
    measuredUnderruns += underruns;
    if (underruns > 0) {
        playbackTargetFrameCount = std::min(MAX_PLAYBACK_TARGET_FRAMES,
                                            playbackTargetFrameCount + 1);
        stablePlaybackTicks = 0;
        qCDebug(LogVoice) << "Playback underrun; adaptive target is now"
                         << playbackTargetFrameCount * AUDIO_FRAME_DURATION_MS << "ms";
    } else if (playbackTargetFrameCount > MIN_PLAYBACK_TARGET_FRAMES) {
        if (++stablePlaybackTicks >= STABLE_TICKS_TO_REDUCE_TARGET) {
            playbackTargetFrameCount--;
            stablePlaybackTicks = 0;
        }
    }

    // The device and Qt timers use different clocks. Keep a small adaptive
    // reserve: 60 ms normally, rising only after a measured underrun and then
    // decaying slowly. The ring buffer remains bounded at 160 ms.
    const int playbackTargetFrames = AUDIO_FRAME_SAMPLES * playbackTargetFrameCount;
    int queuedFrames = audioBackend->queuedPlaybackFrames();
    int missingFrames = std::max(0, playbackTargetFrames - queuedFrames);
    // Do not consume a 20 ms network frame just because one 10 ms device
    // period elapsed. Waiting for a complete frame keeps sequence pacing exact.
    int framesToMix = std::min(playbackTargetFrameCount,
                               missingFrames / AUDIO_FRAME_SAMPLES);

    for (int i = 0; i < framesToMix; ++i) {
        if (!mixOneFrame())
            break;
    }

    // emit periodically
    if (userRmsThrottleTimer.elapsed() >= RMS_EMIT_INTERVAL_MS) {
        for (auto it = pendingUserRms.constBegin(); it != pendingUserRms.constEnd(); ++it)
            emit userAudioLevelChanged(it.key(), it.value());
        pendingUserRms.clear();
        userRmsThrottleTimer.restart();
    }
}

bool AudioPipeline::mixOneFrame()
{
    QVector<std::pair<QByteArray, float>> streams;

    for (auto it = speakers.begin(); it != speakers.end(); ++it) {
        quint32 ssrc = it->first;
        SpeakerState &state = it->second;

        QByteArray pcm;

        if (!state.pendingFrames.isEmpty()) {
            pcm = state.pendingFrames.takeFirst();
        } else {
            if (!state.jitterBuffer->isReady() ||
                !state.jitterBuffer->hasPacketReady())
                continue;

            QByteArray opusData = state.jitterBuffer->pop();

            if (opusData.isEmpty()) {
                // Opus packet N can carry recovery data for N-1. The jitter
                // buffer deliberately peeks without consuming N so it is
                // decoded normally on the following mix tick.
                QByteArray nextOpusData = state.jitterBuffer->peek();
                if (!nextOpusData.isEmpty())
                    pcm = state.decoder->decodeFec(nextOpusData);
                if (pcm.isEmpty())
                    pcm = state.decoder->decodePlc();
            } else {
                QVector<QByteArray> frames = state.decoder->decode(opusData);
                if (frames.isEmpty())
                    continue;
                pcm = frames.first();
                for (int i = 1; i < frames.size(); i++)
                    state.pendingFrames.append(frames[i]);
            }
        }

        if (pcm.isEmpty())
            continue;

        // rms before gain
        auto userIt = ssrcToUser.constFind(ssrc);
        Snowflake userId;
        if (userIt != ssrcToUser.constEnd())
            userId = userIt.value();

        if (userId.isValid()) {
            const auto *samples = reinterpret_cast<const int16_t *>(pcm.constData());
            int count = pcm.size() / static_cast<int>(sizeof(int16_t));
            if (count > 0) {
                float rms = computeRms(samples, count);

                auto rmsIt = pendingUserRms.find(userId);
                if (rmsIt == pendingUserRms.end())
                    pendingUserRms.insert(userId, rms);
                else if (rms > rmsIt.value())
                    rmsIt.value() = rms;
            }
        }

        float gain = 1.0f;
        if (userId.isValid())
            gain = userVolumes.value(userId, 1.0f);

        streams.append({ pcm, gain });
    }

    if (streams.isEmpty())
        return false;

    QByteArray mixed = AudioMixer::mix(streams);
    if (mixed.isEmpty())
        return false;

    return audioBackend->pushPlaybackFrame(reinterpret_cast<const int16_t *>(mixed.constData()));
}

bool AudioPipeline::detectVoiceActivity(const QByteArray &pcmFrame, float &outRms) const
{
    const auto *samples = reinterpret_cast<const int16_t *>(pcmFrame.constData());
    int count = pcmFrame.size() / static_cast<int>(sizeof(int16_t));
    if (count == 0) {
        outRms = 0.0f;
        return false;
    }

    outRms = computeRms(samples, count);
    return outRms > vadThreshold;
}

float AudioPipeline::computeRms(const int16_t *samples, int count)
{
    double sum = 0;
    for (int i = 0; i < count; i++)
        sum += static_cast<double>(samples[i]) * samples[i];
    return static_cast<float>(std::sqrt(sum / count));
}

void AudioPipeline::submitEncodedAudio(const QByteArray &data, qint64 capturedAtMs, bool silence)
{
    if (sendQueue && sendQueue->push(data, capturedAtMs, silence))
        emit audioPacketsAvailable();
}

void AudioPipeline::publishDiagnostics(qint64 nowMs)
{
    if (!audioBackend || nowMs - lastDiagnosticsMs < 5000)
        return;
    auto ages = captureAges;
    const auto count = std::min<size_t>(captureAgeCount, ages.size());
    std::sort(ages.begin(), ages.begin() + count);
    auto percentile = [&](unsigned p) -> qint64 {
        return count ? ages[(count - 1) * p / 100] : 0;
    };
    QJsonObject stats{
        {"capture_to_encode_p50_ms", double(percentile(50))},
        {"capture_to_encode_p95_ms", double(percentile(95))},
        {"capture_to_encode_p99_ms", double(percentile(99))},
        {"capture_dropped_frames", double(audioBackend->captureDroppedFrames())},
        {"stale_before_encode", double(staleBeforeEncode)},
        {"send_queue_dropped_frames", double(sendQueue ? sendQueue->droppedCount() : 0)},
        {"playback_underruns", double(measuredUnderruns)},
        {"playback_queued_ms", double(audioBackend->queuedPlaybackFrames()) * 1000 / AUDIO_SAMPLE_RATE},
        {"mix_max_lateness_ms", double(maxMixLatenessMs)},
        {"capture_age_samples", double(count)}
    };
    emit diagnosticsUpdated(stats);
    captureAgeCount = 0;
    maxMixLatenessMs = 0;
    lastDiagnosticsMs = nowMs;
}

void AudioPipeline::sendTrailingSilence()
{
    QByteArray silence(reinterpret_cast<const char *>(OPUS_SILENCE), sizeof(OPUS_SILENCE));
    for (int i = 0; i < TRAILING_SILENCE_FRAMES; i++)
        submitEncodedAudio(silence, monotonicMilliseconds(), true);
}

} // namespace Audio
} // namespace Core
} // namespace Acheron
