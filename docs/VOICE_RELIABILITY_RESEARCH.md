# Voice reliability research and hardening

This document records evidence-driven changes made after the first live Acheron voice test reported robotic/chopped transmit and receive audio plus excessive perceived latency. Live audio quality is still a manual verification item; offline tests do not claim to prove it.

## Evidence from the first live run

The rotated application log at `%APPDATA%\ouwou\Acheron\acheron.log.1` showed:

- Both selected Windows endpoints ran at 48 kHz stereo.
- WASAPI exposed a maximum shared-stream period of 480 frames (10 ms), while Acheron requested 960 frames (20 ms). miniaudio therefore fell back to a 2,880-frame (60 ms) buffer.
- The capture callback discarded all but roughly one 20 ms Discord frame whenever more than two frames were buffered. A 60 ms callback could therefore discard 40 ms of microphone audio.
- 10,028 Acheron DAVE-decrypt failures were logged. More than 99% of the inspected failures had no DAVE `0xFAFA` marker, matching transport-only frames being rejected while passthrough was disabled.
- Release logging persisted high-volume Gateway and libdave diagnostic messages during the call, adding avoidable work to the voice path.

No credentials, encryption keys, or raw tokens are copied into this document.

## Primary-source findings

- Discord Voice Gateway v8 and newer requires the last received sequence number as `seq_ack` in both Heartbeat and Resume payloads. Voice Gateway v8 also supports replaying buffered signaling messages after a successful resume. Source: <https://docs.discord.com/developers/topics/voice-connections>
- Discord sends Opus at 48 kHz stereo over RTP and requires five silence frames at the end of a talkspurt. The current Acheron framing already follows these requirements. Source: <https://docs.discord.com/developers/topics/voice-connections>
- During a DAVE upgrade/downgrade transition, receive transforms must permit passthrough; previous epoch keys are retained for up to ten seconds for in-flight media. Receive ratchets must be prepared before reporting transition readiness, while the send ratchet switches when Execute Transition arrives. Source: <https://github.com/discord/dave-protocol/blob/main/protocol.md>
- Opus in-band FEC for packet N-1 is carried by packet N. A receiver must keep packet N available, decode it once with `decode_fec=1` for the missing packet, then decode packet N normally. Source: <https://datatracker.ietf.org/doc/html/rfc7587> and <https://opus-codec.org/docs/html_api/group__opusdecoder.html>
- miniaudio PCM ring buffers are lock-free only under a single-producer/single-consumer model. The Acheron playback ring retains that model: the voice thread produces and the device callback consumes. Source: <https://miniaud.io/docs/manual/index.html#RingBuffers>

## Implemented hardening

### Windows audio timing

- Request a 480-frame device period so the observed Windows endpoint can use low-latency shared mode.
- Preserve every complete capture frame rather than deleting old frames inside a multi-frame callback.
- Maintain a bounded two-frame playback target to absorb Qt timer jitter and device-clock drift.
- Restore saved input/output endpoints before starting the audio devices, avoiding an open/close/reopen cycle.

### Packet-loss behavior

- On a missing jitter-buffer packet, peek at the following packet and try Opus in-band FEC before PLC.
- Keep the recovery packet in the jitter buffer so it is decoded normally on the next playout tick.
- Add offline tests for missing-packet lookup, 16-bit RTP sequence wrap, and the Opus FEC decode path.

### DAVE transitions

- Accept transport-only frames only before initial DAVE activation, while downgraded, or during libdave's bounded transition grace period.
- Prepare receiver key ratchets before sending Transition Ready.
- Switch the sender ratchet only at Execute Transition.
- Keep previous receiver ratchets and passthrough state through libdave's transition window.
- Refuse to mark DAVE enabled if the local sender ratchet is unavailable; restart MLS negotiation instead of silently dropping every outbound frame.

### Reconnect

- Include `seq_ack` in Resume.
- Parse the v8+ Heartbeat ACK object and ignore stale nonces.
- Reset the retry counter only after Ready or Resumed, not merely after Hello.
- Retry transient failures during both initial connect and resume, with an interruptible randomized delay so Leave/Exit does not wait for the backoff to finish.

### Runtime overhead

- Suppress Acheron debug categories and libdave info/stat spam in release builds unless `ACHERON_VERBOSE_LOGGING` is explicitly set.
- Preserve warnings and errors in release builds.

## Verification status

- Windows x64 MinSizeRel build: pass.
- Voice, RNNoise, FFmpeg: enabled.
- Offline tests: 7/7 pass, including the VoiceTests target.
- Live QR/account/voice/DAVE/reconnect validation: pending user availability.

## Deferred changes

- Do not lower the jitter target further until live latency and underrun evidence is available.
- Do not force a non-zero Opus packet-loss estimate without an outbound loss signal; the encoder setting is an estimate and unnecessary redundancy can reduce clean-network quality at a fixed bitrate.
- Do not allow indefinite plaintext passthrough while a session is established as E2EE.
- Do not change RTP framing, DAVE cipher behavior, or Voice Gateway version without new protocol evidence and live regression coverage.
# Follow-up: intermittent micro-stutter (2026-09-15)

## Evidence

- A captured voice session showed incoming packets and DAVE work arriving in bursts.
- Network/DAVE handling, Opus/RNNoise processing, and the 20 ms mixer timer all ran
  on the same Qt worker thread. A burst could therefore delay capture encoding and
  playback mixing at the same time, matching reports of both robotic transmit audio
  and intermittent receive gaps.
- The adaptive jitter buffer reduced its network reserve from 60 ms to 40 ms after
  only 200 successful frames (about four seconds). After three misses it increased
  the reserve and rebuffered, then quickly reduced it again. That formed a plausible
  repeating stable/stutter cycle.
- The playback ring had a fixed 40 ms target but no underrun feedback, so it could
  not retain a slightly larger reserve after the Windows device clock outran the Qt
  mixer timer.

## Changes

- Voice networking/DAVE remains on `VoiceThread`; Opus, RNNoise, jitter, mixing, and
  device coordination now run on a separate high-priority
  `AudioProcessingThread`. The additional thread exists only while connected.
- Jitter delay now has a 60 ms floor, rises once per loss burst, and decreases by one
  frame only after 30 seconds of uninterrupted audio.
- The unreachable long-loss reset was fixed by retaining the consecutive-miss count
  after the one-time rebuffer trigger.
- The playback callback records a bounded atomic underrun count. Playback starts at
  a 60 ms reserve, grows up to 100 ms only after a measured underrun, then slowly
  returns to 60 ms.
- The callback still only copies audio and updates atomics; all decoding and mixing
  remains outside miniaudio's real-time device callback.

Primary references:

- https://miniaud.io/docs/manual/index.html
- https://opus-codec.org/docs/opus_api-1.6.pdf

## Remaining live checks

- Rejoin the same room and listen continuously for at least five minutes.
- Have another participant listen to continuous local speech for at least one minute.
- Record Scenario D/E/F memory, CPU, thread, handle, and underrun behavior on the
  final build. Automated tests cannot reproduce the user's actual route, DAVE group,
  Wi-Fi jitter, or HDMI endpoint clock.

# Follow-up: two-way periodic stalls (2026-09-17)

## Evidence and diagnosis

- A ten-second live process sample while connected showed negligible total CPU use,
  so general CPU saturation was not the cause.
- `UdpTransport` drained every pending datagram in one event-loop callback and
  `VoiceClient` synchronously performed transport and DAVE decryption for each one.
  Outbound mic frames were queued onto that same thread. A receive burst could
  therefore delay both playback delivery and mic transmission, then release both in
  a burst. This directly matches the reported two-way “stalls, recovers, stalls”
  pattern.
- A delayed group of mic frames was previously transmitted back-to-back. Preserving
  stale speech is worse than dropping it because the remote jitter buffer hears a
  robotic burst and latency grows.
- The audio device consumes 10 ms periods while playback refill was checked only
  every 20 ms. The callback itself was already lightweight, but the refill had only
  one scheduling opportunity per Opus frame.

## Changes

- Process at most eight UDP datagrams per event-loop turn, then queue a continuation.
  This keeps receive work bounded and lets outbound mic, gateway, and timer events
  interleave. The kernel receive buffer is fixed at 256 KiB; no unbounded userspace
  packet queue was added.
- Timestamp encoded mic frames at capture time. Frames delayed more than 120 ms are
  discarded and the next fresh frame starts a new RTP talkspurt anchored to the
  monotonic clock instead of sending an old burst.
- Check playback reserve every 10 ms, while consuming only complete 20 ms Opus
  frames. The 60–100 ms adaptive reserve and sequence pacing remain unchanged.
- Do not interpret an early mixer tick as packet loss. The jitter buffer advances
  only when the expected packet is present or a newer buffered sequence confirms a
  real gap; this prevents self-inflicted PLC/FEC and late-packet drops.
- Register Windows WASAPI capture and playback callbacks with miniaudio's `Pro Audio`
  usage so MMCSS prioritizes their deadlines during short system scheduling bursts.
- Keep Opus in-band FEC enabled and use a 10% expected-loss default. Opus documents
  that higher expected loss trades some clean-link quality for progressively more
  loss-resistant encoding; the advanced setting remains user-adjustable.

Primary references:

- https://miniaud.io/docs/manual/index.html
- https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service
- https://opus-codec.org/docs/opus_api-1.6/group__opus__encoderctls.html

## Lightweight message notifications

- Incoming messages now use a generated 180 ms mono PCM chime held in about 8 KiB
  and played by the native Windows asynchronous sound API. No Qt Multimedia module,
  decoder, worker process, or persistent audio thread was added.
- Existing Discord guild/channel notification levels, mention-only settings, server
  and channel mute state, ignored users, and `SUPPRESS_NOTIFICATIONS` messages are
  respected.
- Channel, thread, forum, category, and DM context menus expose a persistent local
  **Mute notification sound** switch. Parent/category sound mutes apply to children.
- A 120 ms coalescing window prevents message bursts from stacking many sounds.

Discord's documented channel overrides distinguish All, Mentions, Nothing, and Mute:

- https://support.discord.com/hc/en-us/articles/215253258-Notifications-Settings-101

## Verification status

- Windows x64 MinSizeRel build: pass.
- Seven offline test executables: pass, including VoiceTests.
- Live two-participant voice quality and real message-notification behavior: pending
  user testing on the actual Discord route and audio devices.
