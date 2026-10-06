# Acheron live voice baseline checklist

Use the built executable at `build-baseline\MinSizeRel\acheron.exe`. This checklist is intentionally manual: QR approval, microphone speech, and perceived audio cannot be validated by offline tests.

## Login and discovery

1. In Acheron, open **View > Accounts**.
2. Choose **QR Login**.
3. Leave the optional proxy empty and continue unless this account actually requires SOCKS5.
4. Scan the QR code with the Discord mobile app and approve the login.
5. Confirm the account connects and the guild/channel tree appears.
6. Confirm text and voice channels are visually distinguishable.

Do not paste a token into chat, terminal, issue tracker, or this document. The token is stored through QtKeychain/Windows Credential Manager.

## Voice

1. Right-click a voice channel and choose **Join Voice Channel**.
2. Confirm the account appears in that channel from another Discord client/account if available.
3. Click the voice status bar to open **Voice Settings**.
4. Confirm the expected input and output devices are listed; switch each once if practical.
5. Speak and confirm another participant can hear the microphone.
6. Have another participant speak and confirm their audio is audible.
7. Confirm the participant list and speaking indicator update.
8. If **Privacy Code** or a participant verification code is available, confirm the DAVE UI opens without an error.
9. Disconnect using the voice status bar or the channel context menu.
10. Join the same room again and repeat one receive/transmit check.
11. Keep the second call active for at least 10 minutes. If practical, briefly interrupt networking and report whether it resumes, reconnects, or stalls.
12. With at least one other participant present, have that participant leave and rejoin once. Confirm receive audio continues across the DAVE epoch transition.
13. Repeat a 30-second microphone/receive check with **Noise Suppression** on and off. Record whether either setting changes robotic audio.

## Known baseline gap

Upstream Acheron at the recorded baseline has no UI control for **own** mute/deafen and no push-to-talk implementation. The `Mute` button beside a participant is local playback mute for that participant. Do not count it as microphone mute. The audio/state core can honor Discord self-mute/self-deafen flags, so the missing controls will be added in VoiceLite only after the rest of live baseline voice is confirmed.

## Results to return

Reply with a short line for each item:

```text
QR login: pass/fail + error
Guild/channel load: pass/fail
Join + account visible: pass/fail
Microphone transmit: pass/fail
Audio receive: pass/fail
Input device switch: pass/fail/not tested
Output device switch: pass/fail/not tested
DAVE code UI: pass/fail/not available
Leave + rejoin: pass/fail
10-minute/reconnect observation: result
DAVE participant leave/rejoin: pass/fail/not tested
Noise suppression on/off: same/better/worse/not tested
```

Keep the app running in each requested state if Scenario B-F benchmark sampling will be performed next.
