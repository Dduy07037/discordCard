# Keeping VoiceLite synchronized with Acheron

## Remotes

This checkout uses:

- `upstream`: `https://github.com/ouwou/acheron.git`
- `origin`: intentionally unset until a VoiceLite fork URL exists

When a fork is created, add it without changing `upstream`:

```powershell
git remote add origin <VoiceLite-fork-URL>
git remote -v
```

Never force-push or rewrite upstream history.

## Update procedure

Start with a clean working tree and fetch both the main repository and submodules:

```powershell
git status --short
git fetch upstream
git switch voicelite-development
git rebase upstream/master
git submodule sync --recursive
git submodule update --init --recursive
```

If the VoiceLite branch has been shared, prefer a merge instead of rebasing it:

```powershell
git merge upstream/master
```

Resolve conflicts by keeping Acheron's Gateway, voice, DAVE, and authentication updates intact first, then reapplying the isolated VoiceLite UI/build boundaries. Do not resolve protocol conflicts by accepting an entire side without tracing the changed call flow.

## Verification after every upstream update

1. Record the new upstream SHA in `docs/VOICELITE_ARCHITECTURE.md` and the performance report.
2. Review changes under `src/Discord`, `src/Core/Audio`, `src/Core/TokenStore*`, `vendor/libdave`, and `vendor/mlspp`.
3. Run `scripts/build-windows.ps1` with voice, RNNoise, and tests enabled.
4. Run all offline tests.
5. Repeat the live checklist: restore/login, guild and voice-channel discovery, join, receive, transmit, mute/deafen, leave/rejoin, device switching, DAVE, and network recovery.
6. Re-run the same benchmark scenarios and retain raw samples. Do not compare measurements from materially different build configurations.

Keep VoiceLite-specific code in its own target/files and prefer compile-time/runtime exclusion over deleting upstream protocol code. This minimizes conflicts when Discord voice or DAVE changes.
