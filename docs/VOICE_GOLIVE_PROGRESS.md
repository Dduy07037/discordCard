# Voice and Go Live implementation checkpoint

Baseline: `6976b2c9cc8055ee0e5689e9746fd009e77affe1` (`main`).
Branch: `feat/voice-golive`. This is a reviewable implementation checkpoint, not a release or a claim of proven Discord interoperability.

## Voice changes

- The miniaudio callback writes PCM into a preallocated single-producer/single-consumer ring. Allocation, gain processing, Qt notifications and encoding happen on the audio worker.
- Estimated first-sample monotonic timestamps survive capture, Opus encoding and transport. They are derived from callback delivery time and sample count; OS/device capture latency is not measured without a hardware clock. Audio older than 80 ms at capture drain or 100 ms before encode/send is discarded.
- The outgoing audio ring has eight entries and one outstanding Qt drain notification. After a worker stall it chooses the freshest speech packet, preserves trailing Opus silence, and reanchors RTP timestamps after discontinuity. This deliberately sacrifices old speech rather than replaying a delayed burst.
- Capture overflow/stale drops, send drops, microphone-to-encode/send percentiles, mixer lateness and playback underruns are observable. Microphone-to-send is **not** mouth-to-ear latency. Initial measurements include assembling the 20 ms PCM frame.
- The receive jitter buffer is bounded, ignores duplicate packets and resets when RTP timestamps indicate an Opus DTX talkspurt gap. Existing Opus FEC/PLC remain in use. Deadline-aware concealment, receive clock drift compensation and AEC are still follow-up work.
- Mute discards unsent microphone data before submitting trailing silence. Teardown stops device callbacks before resetting the capture ring.
- 64-bit flags now use the compatibility implementation through Qt 6.8; generic JSON flag handling preserves high bits. Full local application builds use the supported Qt 6.10 toolchain.

## Experimental Go Live probe

This is an interoperability probe behind `ACHERON_GOLIVE_PROBE=1`, with separate viewer/publisher transports. It is not the finished screen-sharing feature.

Implemented:

- Main gateway `STREAM_CREATE`, `STREAM_WATCH`, `STREAM_DELETE`, and `STREAM_SET_PAUSED` requests and stream event routing.
- Session ownership/context validation, either arrival order of CREATE/SERVER_UPDATE, duplicate suppression, timeout, token replacement and cleanup on leaving voice.
- Separate native UDP RTC session using the existing transport encryption and libdave implementation. Normal voice continues in its own workers.
- VP8-only negotiation, 640×360 / 15 fps / 600 kbps test-card encoder, VP8 receiver, optional generated 440 Hz tone, and optional stream-audio playback. No desktop/microphone capture in the probe.
- DAVE encryption of a complete VP8 frame **before** RTP fragmentation; full reassembly **before** DAVE decryption. No plaintext outbound video during MLS setup.
- Packet pacing with one bounded pending frame; bounded reassembly, deadline expiry, sequence/timestamp wrap handling, extension parsing, RTX mapping from session metadata, duplicate/late-frame suppression and latest-image rendering.

Limitations that prevent treating this as production Go Live:

- Official Discord send/receive interoperability has not been exercised here. No signed-in Discord account or second real client is available in the build environment.
- Stream MLS group `rtc_server_id - 1` is a reverse-engineered convention and must pass the real-client gate. It is isolated from normal voice sessions.
- VP8 negotiation may fail against particular publishers/server offers; H264/AV1 negotiation is not implemented.
- No RTCP PLI/NACK feedback, sender RTX cache, congestion control, adaptive bitrate or video/audio clock synchronization yet. Periodic keyframes provide only a basic probe recovery path.
- Probe encoding and stream-audio playback share their own worker. Production encoding needs a separate worker or a measured scheduling budget so decode/encode cannot starve playout.
- Stream audio is local playback only; it does not yet obey the main voice deaf/volume/device controls. Leave it disabled unless explicitly testing audio. It never captures microphone or loopback audio.
- Windows Graphics Capture, window/screen picker, application-loopback capture, hardware encoding and finished watch/share controls are gated follow-up work.

### Running the probe

Use a voice-enabled, FFmpeg-enabled build with a libvpx encoder. Join the same voice channel as a second client running official Discord. Set the environment variable **before launching** the app:

Windows PowerShell:

```powershell
$env:ACHERON_GOLIVE_PROBE = '1'
.\acheron.exe
```

Linux:

```sh
ACHERON_GOLIVE_PROBE=1 ./acheron
```

Open the voice window, expand Advanced, then open the interoperability probe.

1. Publish test card. On official Discord, open this account's stream. Verify moving video for 10 minutes and optional tone; continue talking in the main voice call.
2. Stop publishing. Verify the stream disappears and voice remains connected.
3. On official Discord, start sharing a window. Refresh state by waiting for its voice update, select the streaming user's ID in the probe, then watch. Verify moving video and separately enable stream audio.
4. Watch and publish simultaneously, then stop the viewer alone. Check the publisher and main voice are unaffected.
5. Have the publisher stop/restart its stream. Leave/change voice channel, reconnect and close the dialog during connection setup. Check that no stale media/session remains.
6. Record failures with codec, DAVE readiness, platform, whether audio/video appeared, and voice diagnostics. Never include account tokens, stream tokens, transport keys or private screen contents in logs/issues.

The probe is intentionally opt-in and has no automatic screen or microphone sharing.

## Verification

Automated groups include voice, signaling and VP8 RTP/codec tests alongside the pre-existing suite. They verify local mechanics, not network performance or account interoperability.

Voice tests cover fragmented capture timestamps, ring capacity/concurrent access, outgoing queue coalescing/expiry, stale PCM rejection, mute cleanup, jitter DTX/sequence/RTP wrap, Opus FEC/PLC and teardown.

Stream tests cover signaling ordering/ownership, token changes, parser bounds, out-of-order/duplicate/missing packets, descriptor extensions, memory/expiry limits, timestamp wrap and DAVE-before-fragmentation roundtrips. VP8 encode/decode is checked over 30 frames without an encode backlog.

Local validation configuration: Linux x86_64, GCC 13, Qt 6.10.2, FFmpeg 6.1.1, Opus 1.4, libdave at the repository pin; full build with RNNoise on, and minimal build with voice/FFmpeg off. Development builds use `ALLOW_PLAIN_CURL=ON`; production CI retains curl-impersonate. Plain-curl development builds are not a substitute for Discord connectivity verification. See the PR for completed build/test results and CI status.

## Reference implementations

- [discord/libdave](https://github.com/discord/libdave): existing pinned frame E2EE dependency.
- [Discord-RE/Discord-video-stream](https://github.com/Discord-RE/Discord-video-stream): Go Live signaling, video metadata and stream DAVE convention.
- [dolfies/discord.py-self](https://github.com/dolfies/discord.py-self): stream lifecycle/signaling and native stream-session metadata.
- [dolfies/discord-native-voice](https://github.com/dolfies/discord-native-voice): native RTC reference.
- [paullouisageneau/libdatachannel](https://github.com/paullouisageneau/libdatachannel): candidate alternative WebRTC transport. This checkpoint tests existing native UDP first to reuse its existing voice/DAVE code; it does not establish native UDP as the final production design.
- [microsoft/Windows-classic-samples](https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/ApplicationLoopback) and [robmikh/Win32CaptureSample](https://github.com/robmikh/Win32CaptureSample): next-stage Windows capture references.

No third-party source was copied into the project in this checkpoint. Existing dependency pins are unchanged.
