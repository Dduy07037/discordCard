# VoiceLite architecture baseline

## Baseline and scope

- Upstream: `https://github.com/ouwou/acheron.git`
- Baseline commit: `6edf5ee34f0797c8610640fad5bbd9788b030ee4` (`forgor resources`)
- Working branch: `voicelite-development`
- Baseline target: Windows x64, Qt 6.10.2, MSVC 2022, `MinSizeRel`
- Voice configuration: `ENABLE_VOICE=ON`, `ENABLE_RNNOISE=ON`, `ENABLE_FFMPEG=ON`

This document describes the upstream program before a VoiceLite target or runtime mode is introduced. Authentication and voice are protected areas: retain their protocol behavior until live baseline verification is complete.

## Process and ownership map

`src/main.cpp` constructs `App`, initializes storage/session state, creates `UI::MainWindow`, and asks the session to reconnect saved accounts. `Core::Session` owns the account-level `Core::ClientInstance` objects. Each `ClientInstance` owns one `Discord::Client`, and `Discord::Client` owns the REST client and main Gateway.

For voice, a `ClientInstance` owns one `Core::Audio::VoiceManager`. The manager creates a dedicated `QThread` and moves `Discord::Voice::VoiceClient`, `Core::Audio::AudioPipeline`, and the miniaudio backend to it. curl WebSocket loops and heartbeats use their own standard-library threads inside the Gateway classes. The miniaudio callbacks run on miniaudio's device threads.

## Authentication

### QR login

1. `UI::MainWindow::openAccountsWindow()` opens `UI::AccountsWindow` from **View > Accounts**.
2. `AccountsWindow::onQrLoginClicked()` first obtains the optional proxy through `UI::ProxyInputDialog`, then opens `UI::QRLoginDialog`.
3. `QRLoginDialog::showEvent()` starts `Discord::RemoteAuthClient`.
4. `RemoteAuthClient` creates an RSA key pair and connects through curl WebSocket to `wss://remote-auth-gateway.discord.gg/?v=2`.
5. The remote-auth nonce is signed/proved. The returned fingerprint is rendered by `QRLoginDialog` as `https://discord.com/ra/<fingerprint>` using Nayuki's QR generator.
6. After the phone approves the pending ticket, `RemoteAuthClient::postLogin()` posts to `https://discord.com/api/v9/users/@me/remote-auth/login` and decrypts the returned `encrypted_token` locally with its private key.
7. `QRLoginDialog` returns the token in memory. `AccountsWindow` derives/checks the user ID with `Core::TokenUtils` and passes the account to `UI::AccountsModel`.

Relevant files:

- `src/UI/Accounts/AccountsWindow.cpp`
- `src/UI/Dialogs/QRLoginDialog.cpp`
- `src/Discord/RemoteAuthClient.cpp`
- `src/Core/TokenUtils.hpp`

### Credential and session storage

`AccountsModel::addAccount()` stores the token through `Core::TokenStore`, whose service name is `Acheron`. It uses QtKeychain; the Windows build selects the Windows Credential Manager backend. Non-secret account metadata and proxy configuration are stored by `Storage::AccountRepository`. The model clears the token from its retained account object after saving it.

At startup, `Core::Session::connectAccount()` loads the token from `TokenStore` only when needed, obtains the current Discord client build metadata through `Discord::CurlUtils`, and constructs `Core::ClientInstance`. There is no need for VoiceLite to invent another credential store.

Relevant files:

- `src/Core/TokenStore.cpp`
- `src/Core/Session.cpp`
- `src/Storage/AccountRepository.cpp`
- `src/Core/ClientInstance.cpp`

## Discord Gateway and state

`Discord::Gateway` uses curl's WebSocket mode (`CURLOPT_CONNECT_ONLY=2`). `Discord::IngestThread` handles incoming compressed Gateway bytes and JSON framing. `Gateway::onPayloadReceived()` parses the envelope and dispatches opcodes; `Gateway::handleDispatch()` routes named Discord events. A heartbeat thread maintains the Gateway session. Resume/reconnect state is bounded by `maxReconnectAttempts`, with randomized retry delay.

`Discord::Client` translates Gateway events into application signals and updates repository-backed state. Guild, channel, member, user, role, message, and account repositories live under `src/Storage`. VoiceLite requires guild/channel/member/user data only to the degree needed for server/channel discovery, permissions, and participant names. It does not require message rendering or message history.

Voice-related main-Gateway events are routed as follows:

- `VOICE_STATE_UPDATE` -> `Discord::Client` -> `Core::ClientInstance` -> `VoiceManager::handleVoiceStateUpdate()`
- `VOICE_SERVER_UPDATE` -> `Discord::Client` -> `Core::ClientInstance` -> `VoiceManager::handleVoiceServerUpdate()`

The manager waits for the matching state update (session/channel/user) and server update (endpoint/token) before constructing the voice connection.

Relevant files:

- `src/Discord/Gateway.cpp`
- `src/Discord/IngestThread.cpp`
- `src/Discord/Client.cpp`
- `src/Core/ClientInstance.cpp`
- `src/Storage/`

## Voice call flow

### Join and leave

Actual join path:

`UI::ChannelTreeView` activation signal
-> `UI::MainWindow` voice-channel handler
-> `Discord::Client::sendVoiceStateUpdate(guildId, channelId, false, false)`
-> `Discord::Gateway::sendVoiceStateUpdate()`
-> main Gateway opcode 4
-> Discord `VOICE_STATE_UPDATE` + `VOICE_SERVER_UPDATE`
-> `Core::ClientInstance`
-> `Core::Audio::VoiceManager`
-> `VoiceManager::connectToVoiceServer()`
-> voice thread containing `Discord::Voice::VoiceClient` and `AudioPipeline`.

Leaving sends the same main-Gateway voice-state update with an invalid channel ID. `VoiceManager` stops the pipeline/client, clears participants and SSRC mappings, and tears down the voice-thread objects.

### Voice Gateway negotiation

`VoiceClient::connectToServer()` constructs `VoiceGateway` from the endpoint, guild/channel/user IDs, session ID, voice token, and proxy. `VoiceGateway` opens `wss://<endpoint>/?v=<voice-version>` with curl.

Flow:

1. Voice `HELLO` supplies the heartbeat interval; `VoiceGateway` starts its heartbeat thread.
2. The client sends voice `IDENTIFY`, or `RESUME` when an existing session is resumable.
3. Voice `READY` provides SSRC, UDP address/port, and supported encryption modes.
4. `VoiceClient` prefers `aead_aes256_gcm_rtpsize`, then `aead_xchacha20_poly1305_rtpsize` when selecting the protocol.
5. `UdpTransport` opens UDP (or a SOCKS5 UDP association), performs external-IP discovery, and returns the discovered address/port.
6. The client sends `SELECT_PROTOCOL` for Opus payload type 120.
7. `SESSION_DESCRIPTION` provides the transport key and negotiated DAVE protocol version. `VoiceEncryption` initializes the selected RTP encryption, and `DaveSession` initializes the E2EE session.
8. The pipeline starts playback and, unless self-muted/deafened, capture.

HTTP proxies cannot carry voice UDP; the implementation explicitly supports direct UDP and SOCKS5 UDP association.

### RTP, SSRC, speaking, and participants

Outgoing audio is wrapped by `RtpPacket` with payload type 120, the voice SSRC, a 16-bit sequence, and a 48 kHz timestamp. `VoiceEncryption` encrypts/decrypts the RTP payload using the negotiated AEAD mode. `VoiceClient` emits the voice `SPEAKING` state when VAD changes.

On receive, `VoiceClient` accepts Discord audio RTP, ignores its own SSRC, decrypts transport encryption, removes RTP extensions, applies DAVE decryption when active, and emits `(ssrc, sequence, timestamp, opusData)` to `AudioPipeline`. Voice Gateway speaking/client-connect events map SSRCs to Discord user IDs. `VoiceManager` turns those mappings and main-Gateway voice states into participant join/leave/update/speaking signals.

Relevant files:

- `src/Discord/Voice/VoiceClient.cpp`
- `src/Discord/Voice/VoiceGateway.cpp`
- `src/Discord/Voice/UdpTransport.cpp`
- `src/Discord/Voice/Socks5UdpAssociation.cpp`
- `src/Discord/Voice/RtpPacket.cpp`
- `src/Discord/Voice/VoiceEncryption.cpp`
- `src/Core/Audio/VoiceManager.cpp`

### DAVE / MLS E2EE

`Discord::Voice::DaveSession` wraps libdave's MLS session. `VoiceClient` forwards Voice Gateway DAVE transition/epoch events and binary external-sender, proposal, commit, and welcome messages to it. The session maintains the encryptor and per-SSRC decryptors, performs key ratcheting, and exposes verification/privacy codes. Transport encryption remains a separate layer in `VoiceEncryption`.

The implementation links the pinned `vendor/libdave` and `vendor/mlspp` submodules. This subsystem should remain upstream-shaped so future Discord compatibility fixes can be merged with low conflict.

Relevant files:

- `src/Discord/Voice/DaveSession.cpp`
- `src/Discord/Voice/VoiceClient.cpp`
- `src/Discord/Voice/VoiceGateway.cpp`
- `vendor/libdave/`
- `vendor/mlspp/`

## Audio pipeline

### Microphone transmit path

microphone
-> miniaudio capture callback
-> 48 kHz, signed 16-bit PCM, stereo, 20 ms frame
-> input gain
-> queued `AudioPipeline::onAudioCaptured()`
-> optional RNNoise preprocessing and RNNoise VAD (or RMS VAD)
-> `OpusEncoder`
-> optional DAVE frame encryption
-> RTP transport encryption
-> UDP to Discord.

The common frame is 960 samples per channel and 3,840 bytes. The default Opus configuration is VOIP application, 64 kbit/s, complexity 5, voice signal, in-band FEC enabled, DTX disabled, and 0% expected packet loss. VAD emits five trailing Opus-silence frames after speech ends. RNNoise and RNNoise VAD are runtime switches when compiled in.

### Participant receive path

Discord UDP
-> RTP parse and transport decrypt
-> DAVE decryptor for the sender SSRC
-> `AudioPipeline::onAudioReceived()`
-> per-SSRC `JitterBuffer`
-> per-SSRC `OpusDecoder`
-> 20 ms precise mix timer
-> `AudioMixer` with per-user gains
-> miniaudio playback ring buffer
-> output device.

The default jitter buffer capacity is 10 packets and starts with a three-frame target delay. The miniaudio playback ring holds eight 960-frame blocks (160 ms capacity). Capture and playback device periods are each 960 frames. `AudioPipeline::removeUser()` releases the user's decoder/jitter state; `stop()` clears all speaker and SSRC state.

Relevant files:

- `src/Core/Audio/IAudioBackend.hpp`
- `src/Core/Audio/MiniaudioAudioBackend.cpp`
- `src/Core/Audio/AudioPipeline.cpp`
- `src/Core/Audio/NoiseSuppressor.cpp`
- `src/Core/Audio/OpusEncoder.cpp`
- `src/Core/Audio/OpusDecoder.cpp`
- `src/Core/Audio/JitterBuffer.cpp`
- `src/Core/Audio/AudioMixer.cpp`

## Reconnect and state cleanup

- Main Gateway: heartbeat, resume, bounded reconnect attempts, randomized backoff.
- Voice Gateway: heartbeat/ACK tracking, resume when permitted, bounded reconnect attempts, randomized 1-5 second delay.
- A new `VOICE_SERVER_UPDATE` for the active guild causes the manager to reconnect using the new endpoint/token.
- Leave and terminal disconnect stop capture/playback, clear participants, remove SSRC/decryptor mappings, and release voice-thread objects.

These claims are from source tracing. Network-loss and long-call behavior still require the live Phase 2 checklist.

## Existing voice UI and MVP gaps

`UI::VoiceStatusBar` displays connection state and provides disconnect. Clicking it opens `UI::VoiceWindow`, which exposes devices, volumes, VAD/noise suppression, Opus tuning, participants, per-participant local mute/volume, and DAVE verification/privacy codes.

The audio core already honors own `selfMute`/`selfDeaf` state by stopping capture and suppressing playback. However, the current UI does **not** expose dedicated controls that send updated own mute/deafen voice state; the visible `Mute` button in `VoiceWindow` is local mute for another participant. Push-to-talk is also absent. Therefore mute, deafen, and PTT are VoiceLite MVP work, not baseline features to assume are verified.

## UI classification

### Required for VoiceLite

- `AccountsWindow`, `QRLoginDialog`, `ProxyInputDialog`, and session/account plumbing.
- A minimal native main window.
- Server selection using guild metadata.
- Voice-channel selection using channel type and permissions.
- `VoiceStatusBar`/`VoiceWindow` behavior worth reusing: connection state, participant list, speaking state, device selectors, gain/volume, RNNoise switch, DAVE codes, disconnect.
- `ConnectionBanner` or an equivalent small error/status surface.
- Minimal user/member lookup for participant names; optional static avatars should be measured before retention.

### Potentially removable or compile-time disabled

Do not remove these until a VoiceLite target runs and live voice regression passes:

- Message history/model/delegate/layout/view and composer (`src/UI/Chat`, `src/UI/Input`).
- Markdown/parser/segments, embeds, reactions, reply/edit/pin/typing presentation.
- Inline video/player/FFmpeg media stack (`src/Core/Media`, video controls/windows).
- Image viewer, image cache, GIF/animated image paths, attachment previews and upload.
- DM/group-DM presentation, tabs, forum and thread browser UI.
- Emoji UI, emoji autocomplete/frecency, theme customization beyond a fixed minimal style.
- Message repository/cache, read-state and typing trackers when no longer needed by the selected event subscriptions.
- Full member/image/avatar caches after profiling; keep only data required for permissions and voice participant identity.

## Build system and pinned dependencies

The top-level CMake project uses the vcpkg toolchain from `CMakePresets.json`. Windows CI builds with Visual Studio/MSVC x64, Qt 6.10.2 plus `qtimageformats`, vcpkg triplet `x64-windows-static-md`, curl-impersonate v2.0.0, and pinned FFmpeg 8.1 build artifacts. Voice links Opus, libsodium, libdave/mlspp, miniaudio, and optional RNNoise. `rnnoise.cmake` builds RNNoise and its model; `ffmpeg.cmake` locates the media libraries.

The reproducible local path is captured by `scripts/build-windows.ps1`.

## Security findings before live-account testing

At this baseline SHA, diagnostic logging serializes complete Voice Gateway JSON and complete main-Gateway inbound payloads. Those payloads can include the voice token and `SESSION_DESCRIPTION.secret_key`; `ClientInstance` also logs a prefix of a voice token. This violates the project's credential/logging requirements even though it is upstream baseline behavior. A narrow redaction patch is required before joining a real voice channel; it must not change protocol data sent on the wire.

## Next architectural step (blocked on live baseline verification)

After QR login, guild/channel discovery, bidirectional audio, device switching, leave/rejoin, DAVE, and reconnect are verified, introduce an isolated target or compile-time option such as `ACHERON_VOICELITE=ON`. First avoid constructing message/media/DM UI at runtime; measure the result against this baseline before deleting upstream code.
