# Voice and Go Live implementation checkpoint

Baseline: `6976b2c9cc8055ee0e5689e9746fd009e77affe1` (`main`).
Branch: `feat/voice-golive`. This is a reviewable implementation checkpoint, not a release or a claim of compatibility across all Discord clients/platforms.

## Voice changes

- The miniaudio callback writes PCM into a preallocated single-producer/single-consumer ring. Allocation, gain processing, Qt notifications and encoding happen on the audio worker.
- Estimated first-sample monotonic timestamps survive capture, Opus encoding and transport. They are derived from callback delivery time and sample count; OS/device capture latency is not measured without a hardware clock. Audio older than 80 ms at capture drain or 100 ms before encode/send is discarded.
- The outgoing audio ring has eight entries and one outstanding Qt drain notification. After a worker stall it chooses the freshest speech packet, preserves trailing Opus silence, and reanchors RTP timestamps after discontinuity. This deliberately sacrifices old speech rather than replaying a delayed burst.
- Capture overflow/stale drops, send drops, microphone-to-encode/send percentiles, mixer lateness and playback underruns are observable. Microphone-to-send is **not** mouth-to-ear latency. Initial measurements include assembling the 20 ms PCM frame.
- The receive jitter buffer is bounded, ignores duplicate packets and resets when RTP timestamps indicate an Opus DTX talkspurt gap. Existing Opus FEC/PLC remain in use. Deadline-aware concealment, receive clock drift compensation and AEC are still follow-up work.
- Mute discards unsent microphone data before submitting trailing silence. Teardown stops device callbacks before resetting the capture ring.
- 64-bit flags now use the compatibility implementation through Qt 6.8; generic JSON flag handling preserves high bits. Full local application builds use the supported Qt 6.10 toolchain.

## Experimental screen sharing and stream viewer

The voice window now has a visible **Watch streams / Share screen (experimental)** button. Open it from **View → Voice Controls / Go Live**, **Ctrl+Shift+V**, or a voice channel’s context menu. This experimental implementation has separate viewer/publisher transports. The user has reported both publishing and viewing working on their Windows setup; the build environment itself has no signed-in Discord session. The diagnostic test card/tone alone requires `ACHERON_GOLIVE_PROBE=1`.

Implemented:

- Main gateway `STREAM_CREATE`, `STREAM_WATCH`, `STREAM_DELETE`, and `STREAM_SET_PAUSED` requests and stream event routing.
- Session ownership/context validation, either arrival order of CREATE/SERVER_UPDATE, duplicate suppression, timeout, token replacement and cleanup on leaving voice.
- Separate native UDP RTC session using the existing transport encryption and libdave implementation. Normal voice continues in its own workers.
- Explicit monitor selection, one-shot local preview, and real desktop capture through Qt QScreen on supported desktops. Capture starts only on Share, stops on Stop/close/leave/monitor removal, and is never resumed automatically after the stream is deleted. A single replaceable frame connects GUI capture to the separate encoding worker; images older than 150 ms are discarded.
- VP8 publishing and VP8/H264 receive negotiation: monitor video is letterboxed to the selected 720p or 1080p resolution, with 15/30/60 FPS targets (30 default) and manual 4–16 Mbps profiles and Auto (default: 1080p / 30 FPS / 6 Mbps). Auto can lower FPS, bitrate and resolution when the local encoder/sender falls behind; actual completed-frame FPS, current target and encode time are displayed. The optional diagnostic test card stays at 640×360 / 15 fps / 600 kbps. There is no system-audio or microphone capture in the stream publisher; the main voice call remains separate.
- Watchers are listed by display name when available; watching and sharing have separate Stop controls. A receive worker decodes VP8/H264 and optionally plays stream audio. Original decoded images support zoom, drag-to-pan, fitting to a resizable window and fullscreen. A red TRỰC TIẾP badge in voice participant rows follows the server's sharing flag.
- DAVE encryption of a complete VP8 frame **before** RTP fragmentation; full reassembly **before** DAVE decryption. No plaintext outbound video during MLS setup.
- Packet pacing with one bounded pending frame; bounded reassembly, deadline expiry, sequence/timestamp wrap handling, extension parsing, RTX mapping from session metadata, duplicate/late-frame suppression and latest-image rendering.

Limitations that prevent treating this as production Go Live:

- Official Discord send/receive interoperability has not been exercised here. No signed-in Discord account or second real client is available in the build environment.
- Stream MLS group `rtc_server_id - 1` is a reverse-engineered convention and must pass the real-client gate. It is isolated from normal voice sessions.
- Viewing supports VP8 and H264; H265, VP9 and AV1 are not advertised. Unsupported server codec changes fail visibly rather than feeding the wrong decoder.
- Receiver startup, failed decrypt/decode and dropped frame assembly request an RTCP PLI, limited to one per source per second. Publisher PLI requests force a keyframe. NACK recovery, sender RTX cache, congestion control, adaptive bitrate and video/audio clock synchronization remain follow-up work.
- Probe encoding and stream-audio playback share their own worker. Production encoding needs a separate worker or a measured scheduling budget so decode/encode cannot starve playout.
- Stream audio is local playback only; it does not yet obey the main voice deaf/volume/device controls. Leave it disabled unless explicitly testing audio. It never captures microphone or loopback audio.
- This first monitor capture path uses Qt’s platform screenshot API, not Windows Graphics Capture or a GPU encoder. Wayland, protected video, fullscreen games, high-DPI/multi-monitor behavior and macOS permissions need native validation. Window-only selection, cursor overlay, system audio, hardware encoding and RTCP-based adaptation remain follow-up work.

### Running the experimental screen-sharing UI

Use a voice-enabled, FFmpeg-enabled build with a libvpx encoder. Join the same voice channel as a second client running official Discord. Launch the full build normally:

Windows PowerShell:

```powershell
.\acheron.exe
```

Linux:

```sh
./acheron
```

Open **View → Voice Controls / Go Live** (or **Ctrl+Shift+V**), then **Watch streams / Share screen (experimental)**. No environment variable is needed for the screen-sharing UI. Select a monitor and press **Preview screen** before sharing; then **Share selected screen**. Stop sharing and Stop watching are independent. The full voice/FFmpeg build is required; the minimal build has neither feature.

1. Share a selected monitor (or enable the diagnostic environment variable and publish a test card). On official Discord, open this account's stream. Verify moving video for 10 minutes and optional tone; continue talking in the main voice call.
2. Stop publishing. Verify the stream disappears and voice remains connected.
3. On official Discord, start sharing a window. Refresh state by waiting for its voice update, select the streaming user's name in the dialog, then watch. Verify moving video and separately enable stream audio.
4. Watch and publish simultaneously, then stop the viewer alone. Check the publisher and main voice are unaffected.
5. Have the publisher stop/restart its stream. Leave/change voice channel, reconnect and close the dialog during connection setup. Check that no stale media/session remains.
6. Record failures with codec, DAVE readiness, platform, whether audio/video appeared, and voice diagnostics. Never include account tokens, stream tokens, transport keys or private screen contents in logs/issues.

Screen sharing always requires an explicit Share click. The dialog performs no capture on construction or monitor selection, and no screen/microphone sharing starts automatically. Preview captures one frame locally without creating a Discord stream.

## Verification

Automated groups include voice, signaling and VP8 RTP/codec tests alongside the pre-existing suite. They verify local mechanics, not network performance or account interoperability.

Voice tests cover fragmented capture timestamps, ring capacity/concurrent access, outgoing queue coalescing/expiry, stale PCM rejection, mute cleanup, jitter DTX/sequence/RTP wrap, Opus FEC/PLC and teardown.

Stream tests cover signaling ordering/ownership, token changes, parser bounds, out-of-order/duplicate/missing packets, descriptor extensions, memory/expiry limits, timestamp wrap and DAVE-before-fragmentation roundtrips. VP8 encode/decode is checked over 30 frames without an encode backlog.

Local validation configuration: Linux x86_64, GCC 13, Qt 6.10.2, FFmpeg 6.1.1, Opus 1.4, libdave at the repository pin; full build with RNNoise on, and minimal build with voice/FFmpeg off. Development builds use `ALLOW_PLAIN_CURL=ON`; production CI retains curl-impersonate. Plain-curl development builds are not a substitute for Discord connectivity verification. See the commit and Actions run for completed build/test results and CI status.

## Reference implementations

- [discord/libdave](https://github.com/discord/libdave): existing pinned frame E2EE dependency.
- [Discord-RE/Discord-video-stream](https://github.com/Discord-RE/Discord-video-stream): Go Live signaling, video metadata and stream DAVE convention.
- [dolfies/discord.py-self](https://github.com/dolfies/discord.py-self): stream lifecycle/signaling and native stream-session metadata.
- [dolfies/discord-native-voice](https://github.com/dolfies/discord-native-voice): native RTC reference.
- [paullouisageneau/libdatachannel](https://github.com/paullouisageneau/libdatachannel): candidate alternative WebRTC transport. This checkpoint tests existing native UDP first to reuse its existing voice/DAVE code; it does not establish native UDP as the final production design.
- [microsoft/Windows-classic-samples](https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/ApplicationLoopback) and [robmikh/Win32CaptureSample](https://github.com/robmikh/Win32CaptureSample): next-stage Windows capture references.

No third-party source was copied into the project in this checkpoint. Existing dependency pins are unchanged.

## Screen-sharing checkpoint validation

- An offscreen UI smoke check opens the dialog, checks that Share is disabled without voice, and previews locally without creating a stream. This does not verify native screen capture.
- Full Qt 6.10.2/RNNoise/FFmpeg build: 10/10 groups pass, including the new protocol regression group. Voice/FFmpeg-off build: 6/6 groups pass.
- Added checks for latest-frame replacement, expiration and stop cleanup; 720p low-delay VP8 roundtrips with portrait inputs verify preserved aspect ratio/black margins.
- Linux build/test results do not establish Windows desktop capture correctness or official Discord compatibility. Native Windows validation requires the CI artifact and two real clients.
- The previous remote run still failed in AppImage Test and Qt5 Build; their full logs are needed for diagnosis. Modern Windows Qt6 builds passed. No job is disabled by this checkpoint.

## Viewer and frame-rate update

User validation of the previous Windows checkpoint: another Discord client can see the shared monitor, while watching an existing stream remains black after transport connection. This is evidence for publishing on that setup, not proof of all codec/platform combinations.

The receiver previously ignored Voice opcode 12 Video and interpreted opcode 14 Session Update as video SSRC/user metadata. It now receives Video source/layer mappings and handles codec changes independently. Active primary sources receive explicit sink wants; RTX uses the advertised mapping or the native +1 default. Stale sources are retired without restarting assembly on repeated state messages.

H264 depacketization supports single NAL, STAP-A and FU-A with bounded/reordered assembly. Annex B four-byte start codes survive reassembly before DAVE authentication. Joining midstream waits for parameter sets and requests a fresh keyframe. Decoder output is drained and only the latest image is rendered.

Publishing avoids encoding while its bounded packet pacer is busy; skipped capture slots do not introduce unsent reference frames. A one-slot capture queue, 150 ms expiry and an independent voice worker remain. Local preview is refreshed at 10 FPS to limit GUI work. 60 FPS is a target, not a guarantee from the Qt screenshot API or software encoder.

Validation covers opcode 11/12/14 routing, mapping updates, disabled layers and cleanup, H264 STAP/FU loss/wrap/bounds, own three-frame H264 fixture through DAVE/depacketization/FFmpeg, and immediate 720p VP8 encode/decode at 30/60 FPS. An encrypted RTP regression feeds out-of-order VP8 with header extensions and repairs a missing fragment through RTX into VoiceClient, then verifies the decrypted frame. The offscreen UI check confirms all three FPS choices, the 30 FPS default, and local-only preview. Native Discord receive and Windows capture performance still need the updated CI artifact and real clients.

The script `scripts/update-and-run-windows.ps1` downloads the full Windows artifact for a chosen commit and launches a fresh executable path. It requires no checkpoint ZIP and does not apply/push patches.

The updater was exercised with a mocked GitHub CLI: it selects the requested commit and full Windows artifact even when another job fails, and refuses to launch when the full Windows job fails. This check used PowerShell 7 on Linux with only the Windows host guard bypassed; actual Windows PowerShell 5.1 execution remains a native validation step.

Additional implementation references: [RFC 6184](https://www.rfc-editor.org/rfc/rfc6184) (H264 RTP), [RFC 4585](https://www.rfc-editor.org/rfc/rfc4585) (PLI), [Discord DAVE protocol](https://github.com/discord/dave-protocol/blob/main/protocol.md) (codec clear ranges), and the current [discord-native-voice](https://github.com/dolfies/discord-native-voice) receiver/RTCP code. No third-party implementation source was copied; the test fixture was generated locally.


## Previous checkpoint: zoom, quality profiles and live badges

The user reported that watching streams works after the receive/FPS update. This update addresses viewer sizing, publishing quality and identifying live participants.

The viewer retains the decoded image at its original resolution. The minus/plus buttons and mouse wheel change zoom from 25% to 800% of the fitted image; dragging pans an enlarged image. Fit resets zoom and centers it. Window resize recomputes the fitted size, while new frames preserve zoom/pan. Fullscreen displays the same frames in a separate window without another RTC session; Esc or Exit fullscreen closes only that window. End/stop clears both views, and the Go Live window can be maximized.

Publishing profiles use one immutable resolution/bitrate choice for the encoder and the advertised video state:

| Profile | Resolution | 15/30 FPS target bitrate | 60 FPS target bitrate |
| --- | --- | --- | --- |
| Balanced | 1280 × 720 | 4 Mbps | 6 Mbps |
| High | 1920 × 1080 | 8 Mbps | 12 Mbps |
| Maximum (default) | 1920 × 1080 | 12 Mbps | 16 Mbps |

The frame-rate default remains 30 FPS. Higher resolution cannot recover detail absent in the captured monitor or a received lower-resolution stream. Bitrate is a codec target; actual wire rate, delivered FPS and quality depend on content, CPU, capture and available bandwidth. Libvpx uses a slower quality setting at up to 30 FPS, a faster setting at 60 FPS, bounded quantization and smooth letterboxing. It remains software VP8 encoding; there is no new hardware encoder or congestion controller.

The desktop pacer allows at most eight packets per 2 ms tick, compared with the previous two-packet ceiling near 8 Mbps. Encoded frame capacity increases from 128 KiB to 512 KiB for larger 1080p keyframes. One pending frame, 150 ms expiry, DAVE-before-fragmentation and independent main-voice workers remain.

Voice participant rows paint a red TRỰC TIẾP badge beside users whose server-confirmed self_stream is true. The local participant now retains this flag too. Start/stop changes trigger the participant row update, and the live-stream picker also labels active streamers. Names are elided with space reserved for the badge and mute/deafen icons.

Validation: full application build and 10/10 test groups, minimal build and 6/6 groups. Added 1080p maximum-profile roundtrips at 30/60 FPS, native one-pixel contrast retention, and 300 KiB DAVE/RTP frame roundtrips. An offscreen harness checks quality/FPS defaults, original frame resolution, zoom persistence across resize/new frames, wheel zoom/bounds, fullscreen open/update/clear/close/reopen, local/remote live start/stop notifications, badge rendering alongside muted/deafened icons and sharing disabled without voice. These checks do not measure native Windows capture or a live high-bitrate Discord stream.

References for this update: [FFmpeg libvpx options](https://ffmpeg.org/ffmpeg-codecs.html#libvpx) and Qt QWidget/QPainter event/rendering APIs. No third-party implementation code was copied.


## Live status and streaming load correction

The user reported false TRỰC TIẾP badges and stutter after the high-quality update. `self_stream` is optional: the previous badge code read its scalar value even when absent. Optional/nullable field storage is now value-initialized, and every live-state consumer requires `hasValue() && value`. Missing, null or false flags hide the badge; transitions back from true update both local and remote participant rows and the stream picker.

Auto is the new default at 30 FPS. It starts at 1920×1080 / 6 Mbps (8 Mbps if the user selects 60 FPS). Balanced, High and Maximum remain available manually at their previous bitrate/resolution targets. Manual profiles do not adapt.

The local load controller measures encode time and the encode-plus-pacing cycle. After six consecutive overloaded samples, with four seconds between adjustments, Auto steps from 60 to 30 to 20 to 15 FPS, then from 1080p to 720p if still overloaded. Bitrate decreases proportionally, bounded below by 2 Mbps. Delivered periodic keyframe bursts alone do not trigger adaptation. It only steps down until the stream is restarted, preventing repeated quality oscillation. Encoder, capture timer and gateway metadata change together without replacing the RTC/DAVE session.

For matching aspect ratios, native RGB32/BGRA or RGBA pixels go directly through swscale into YUV, removing the full-resolution QPainter copy; swscale resizes if necessary. Other aspect ratios retain smooth letterboxing. Desktop VP8 uses the fastest realtime CPU setting. Periodic desktop keyframes are spaced two seconds apart; recovery requests still trigger an immediate keyframe. RTP timestamps use capture time rather than encoding completion time.

The encode timer now schedules the next frame after local packet pacing finishes, using the remaining frame budget. It no longer waits another entire frame interval when a repeating timer fires while the previous frame is pending. Readiness retries, a single capture slot, one pending encoded frame and 150 ms stale-frame expiry remain bounded.

This is CPU/local sender adaptation, not remote bandwidth estimation: successful local UDP submission does not establish remote delivery. There is no new hardware encoder or feedback-driven network congestion controller. CPU, Qt monitor capture, available uplink and the viewer's device can still limit performance. Prefer Auto / 30 FPS for native validation before manually selecting Maximum.

Validation includes missing/null/false/true JSON and poisoned-memory scalar initialization, local/remote true-to-missing UI transitions, controller overload/cooldown/keyframe/manual cases, alternating native RGB32/RGBA moving desktop frames and live 1080p-to-720p encoder/decoder reconfiguration. The test environment has no signed-in Discord session; actual Windows capture and end-to-end streaming require the new CI artifact and real clients.

Local verification for this correction: full Qt 6.10.2/RNNoise/FFmpeg application build and 11/11 CTest groups pass; voice/FFmpeg-off build and 7/7 groups pass. The offscreen UI harness passes Auto/30 FPS defaults, local/remote live-to-missing transitions, badge visibility, original-frame zoom/pan/fullscreen lifecycle and sharing guard. Windows updater PowerShell syntax parses successfully. These are local checks, not native end-to-end latency/FPS measurements.
