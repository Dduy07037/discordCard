# VoiceLite UI/UX audit and redesign

Recorded: 2026-09-12 (UTC+7)

## Scope and method

This pass audits the current Qt Widgets interface and implements a production-oriented visual system without changing Discord, chat, account, or voice behavior. The review combined source inspection, a clean Windows `MinSizeRel` build, an isolated idle-process benchmark, and the existing automated test suite.

Direct native-window capture was unavailable in the current automation session, so no claim below is based on an invented screenshot. Live visual and voice-room checks remain in the manual verification list.

## Findings before the redesign

1. The default palette used several close purple surfaces. Text contrast was acceptable, but navigation, content, controls, and hover states were difficult to distinguish at a glance.
2. Styling was fragmented between a very small application stylesheet and many widget-local hard-coded colors. Connection, warning, error, slow-mode, reply, and voice states could disagree with custom themes.
3. The voice window was only 320 px wide. Participant volume, audio level, mute, state flags, device selectors, and codec controls competed for the same narrow rows.
4. Several primary controls used 16–24 px hit targets. Focus styling was sparse, labels were abbreviated without accessible descriptions, and some icon-only controls lacked an explicit accessible name.
5. Settings presented navigation and content as two generic widgets with little hierarchy. Account management had the same issue.
6. Modal popups used a large real-time blur shadow. It added rendering work while the overlay and border already provided enough separation.
7. The microphone meter used a continuous three-color gradient. The result was visually busy and made its threshold harder to scan.

## Implemented design system

### Visual direction

VoiceLite now uses a restrained Windows utility aesthetic: neutral charcoal surfaces, a single cool-blue interaction accent, and semantic green/amber/red only for status. There are no decorative gradients, glow effects, glass blur, or continuous ornamental animations.

### Tokens

- Surfaces: window, base, alternate, button, hover, and pressed.
- Text: high-emphasis window text, primary text, muted placeholder text, and disabled text.
- Borders: normal and strong divider colors.
- Status: success, warning, and danger.
- Accent: one highlight color shared by focus, selection, active sliders, links, and speaking emphasis.

Generated light and dark themes populate every new token, while existing exported themes remain compatible because missing tokens fall back to defaults.

### Typography and spacing

- Windows preference order: Segoe UI Variable Text, Segoe UI, then the system font.
- Default interface size: 9.5 pt; page titles 17–19 px; small secondary copy uses the muted text token.
- Spacing follows a compact 2/4/8/12/16/20/24/28 scale.
- Common controls have a 30 px minimum height; critical icon targets use 28–32 px.
- Corners use 4–10 px radii according to hierarchy, with no pill-shaped default controls.

### States and interaction

- Buttons, tool buttons, inputs, menus, tabs, list items, sliders, and scrollbars have consistent hover, pressed, selected, disabled, and keyboard-focus states.
- Voice connection colors and account connection colors use semantic tokens instead of literal named colors.
- Participant state abbreviations now expose readable tooltips and accessibility descriptions.
- Icon-only reply cancellation and voice disconnection controls have explicit accessible names and larger targets.
- Advanced voice settings remain collapsed by default and open instantly, avoiding layout animation and background timers.
- Floating voice and modal windows use one shared 120 ms compositor-opacity fade. The animation deletes itself on completion and follows the Windows client-area animation accessibility setting.

## Core flow improvements

### Main application

- The content area is full-bleed with one-pixel structural dividers rather than default layout gutters.
- Navigation, conversation, and member surfaces are visually distinct but use the same restrained palette.
- Channel toolbar and guild header borders now follow the active theme.

### Voice

- The voice window now opens at 480 x 600 px with a safe 420 x 440 px minimum.
- Information hierarchy is explicit: session summary, participants, microphone level, devices, gain/output/VAD, then advanced codec controls.
- Participant rows are taller, easier to scan, and provide larger volume/mute targets.
- The microphone meter uses a single semantic color at a time: green for safe level, amber near clipping, red at clipping.
- Speaking indication uses the shared success color. Device and slider controls have accessible names.
- The compact voice status bar is now 44 px tall, themed, keyboard-focus visible, and uses the existing cached Lucide icon system instead of painting a one-off icon.

### Settings and accounts

- Settings now has a stable 184 px navigation rail and a padded content surface with a clear page title.
- General and Audio pages include short explanatory copy and better field alignment.
- Account management has explicit list/details headings, a real empty state, consistent spacing, and semantic connection colors.

### Popups

- The GPU-backed 25 px drop-shadow blur was removed. A dim overlay plus tokenized 1 px border and 10 px radius now provides separation at lower cost.

## Performance baseline and acceptance criteria

Pre-redesign Scenario A, measured on the current voice-reliability branch with a hidden clean profile:

| Metric | Min | Average | Max |
| --- | ---: | ---: | ---: |
| Working Set | 59.82 MiB | 59.82 MiB | 59.82 MiB |
| Private Working Set | 13.93 MiB | 13.93 MiB | 13.93 MiB |
| Private Bytes | 41.91 MiB | 41.91 MiB | 41.91 MiB |
| CPU | 0.00% | 0.00% | 0.00% |
| Threads | 10 | 10 | 10 |
| Handles | 377 | 377.17 | 379 |

Post-redesign Scenario A used the same hidden profile, warm-up, sample count, and interval:

| Metric | Before average | After average | Observed change |
| --- | ---: | ---: | ---: |
| Working Set | 59.82 MiB | 58.04 MiB | -1.78 MiB (-3.0%) |
| Private Working Set | 13.93 MiB | 13.30 MiB | -0.63 MiB (-4.5%) |
| Private Bytes | 41.91 MiB | 40.46 MiB | -1.45 MiB (-3.5%) |
| CPU | 0.00% | 0.00% | no measured change |
| Threads | 10 | 10 | no change |
| Handles | 377.17 | 377.17 | no change |

This is a short idle sample, so the small memory reduction should be treated as “no regression observed,” not as a guaranteed optimization result. The important acceptance criteria passed: no persistent idle CPU, background thread, or handle increase was introduced.

The executable changed from 21,430,272 bytes to 21,456,896 bytes: +26,624 bytes, or about +0.12%. No UI dependency, asset bundle, animation library, image, or font file was added.

## Manual visual verification still required

- Check the main window at 100%, 125%, 150%, and 200% Windows scaling.
- Resize the main window through narrow and wide layouts; verify labels elide rather than overlap.
- Open Settings, Accounts, QR login, confirmation popups, and a user profile popup.
- Verify keyboard Tab traversal and visible focus on buttons, inputs, lists, and voice controls.
- Join a live voice room and verify participant rows with long names, speaking, mute/deafen/suppress, DAVE privacy code, advanced settings, and device hot-plug.
- Confirm the live audio fixes separately using `LIVE_VOICE_CHECKLIST.md`; this UI pass intentionally does not alter the audio pipeline.
