# Acheron performance baseline

## Measurement status

Baseline commit: `6edf5ee34f0797c8610640fad5bbd9788b030ee4`

Build: Windows x64 `MinSizeRel`, Qt 6.10.2, voice/RNNoise/FFmpeg enabled. The executable is the unmodified upstream application; no VoiceLite runtime mode exists yet.

Only Scenario A can be measured without a Discord account. Scenarios B-F remain deliberately blank until QR login and live voice verification are performed. No value below is an estimate.

## Test environment and method

- Recorded: 2026-09-07 16:38 (UTC+7)
- Windows: 25H2, build 26200.9168
- CPU: Intel Core i5-13420H, 12 logical processors
- Warm-up: 10 seconds
- Samples: 30
- Requested interval: 1 second
- Process: one `acheron.exe`
- Account isolation: clean temporary `APPDATA` and `LOCALAPPDATA`; no saved account was loaded

Working Set, Private Bytes, threads, handles, and CPU time come from the Windows process APIs exposed by PowerShell. Private Working Set comes from Windows performance counters matched by PID. CPU is the process CPU-time delta divided by wall time and logical processor count, so 100% means the whole machine is saturated. Values are MiB (1,048,576 bytes), not decimal MB.

The Scenario A process created the normal application object/window but was launched hidden for unattended measurement. That avoids UI interaction and repaint noise; a visible-window rerun should be retained later if UI repaint cost becomes material.

## Baseline results

| Scenario | State | Working Set MiB min/avg/max | Private WS MiB min/avg/max | Private Bytes MiB min/avg/max | CPU % min/avg/max | Processes | Threads min/avg/max | Handles min/avg/max |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| A | Open, not logged in | 76.41 / 76.44 / 76.46 | 13.15 / 13.19 / 13.21 | 40.54 / 40.60 / 40.64 | 0.00 / 0.00 / 0.00 | 1 | 7 / 8.90 / 10 | 404 / 404.93 / 406 |
| B | Logged in, idle | Not measured | Not measured | Not measured | Not measured | Not measured | Not measured | Not measured |
| C | Guild/channel view, not in voice | Not measured | Not measured | Not measured | Not measured | Not measured | Not measured | Not measured |
| D | Connected to voice | Not measured | Not measured | Not measured | Not measured | Not measured | Not measured | Not measured |
| E | Receiving voice audio | Not measured | Not measured | Not measured | Not measured | Not measured | Not measured | Not measured |
| F | Transmitting microphone | Not measured | Not measured | Not measured | Not measured | Not measured | Not measured | Not measured |

The reported 0.00% is below the resolution observed during this idle sample; it must not be interpreted as proof that the process consumes literally zero CPU.

Raw Scenario A data from this run is under the ignored local directory `artifacts/benchmarks/`. Each future run produces a timestamped sample CSV and summary JSON.

## Artifact size

- `acheron.exe`: 20.43 MiB
- Current deployed build directory: 179.01 MiB across 45 files

The directory size is a development deployment, not a final package measurement: it still contains test executables and Qt plugins that upstream CI removes from its artifact. Use the same packaging contents when comparing Acheron and VoiceLite.

## Reproducing a scenario

Start the intended executable, put it in exactly one state, and run:

```powershell
.\scripts\benchmark-memory.ps1 -Scenario A
```

For a specific process when multiple builds are open:

```powershell
.\scripts\benchmark-memory.ps1 -ProcessId <PID> -Scenario D -WarmupSeconds 15 -Samples 60
```

Keep these state definitions consistent:

- A: application open with no account loaded.
- B: login complete, leave the application idle.
- C: guild/channel UI visible, not connected to voice.
- D: connected to a voice room with no intentional receive/transmit activity.
- E: another participant continuously produces audible speech.
- F: local microphone continuously transmits speech.

For RNNoise comparison, repeat D and F with suppression on and off; name the resulting files/notes explicitly rather than overwriting either result.

## Comparison table

Fill this only with measurements made under the same build type, machine, sampling interval, and voice-room conditions.

| Metric | Acheron baseline | VoiceLite | Improvement |
| --- | ---: | ---: | ---: |
| Startup Working Set | 76.44 MiB average (Scenario A, hidden window) | Not measured | Not measured |
| Logged-in idle Working Set | Not measured | Not measured | Not measured |
| Voice Working Set | Not measured | Not measured | Not measured |
| Voice Private Bytes | Not measured | Not measured | Not measured |
| Idle CPU | Not measured | Not measured | Not measured |
| Voice CPU | Not measured | Not measured | Not measured |
| Startup time | Not measured | Not measured | Not measured |
| Binary/package size | 20.43 MiB executable; package not measured | Not measured | Not measured |
| Voice threads | Not measured | Not measured | Not measured |
| Voice handles | Not measured | Not measured | Not measured |

## Live measurement checklist

After the user confirms QR login and voice behavior, record B and C first, then D-F in the same voice room. Keep the call alive long enough to observe steady state and run a second D sample after leave/rejoin to catch retained decoders, threads, handles, or reconnect objects.
