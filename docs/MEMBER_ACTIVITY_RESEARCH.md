# Member activity sidebar research

Date: 2026-09-15

## Product goal

Make the right member sidebar useful at a glance, like Discord: show a concise current
activity below a member's name and reveal the available details on hover. Keep the
feature compatible with VoiceLite's low-memory goal.

## Protocol findings

- Discord presence payloads carry an `activities` array. Each activity can contain
  a type, name, details, state, party size, and `status_display_type`.
- The documented activity types are Playing, Streaming, Listening, Watching,
  Custom, and Competing.
- `status_display_type` selects whether member-list text is based on the activity
  name, state, or details.
- VoiceLite already requests guild activities in its guild subscription. The old
  member-list parser simply discarded the presence portion of each member item.
- Standard `PRESENCE_UPDATE` events are also needed so a visible row changes without
  waiting for the member list to be re-subscribed.

Primary references:

- https://docs.discord.com/developers/events/gateway-events#presence-update
- https://docs.discord.com/developers/events/gateway#tracking-state
- https://docs.discord.com/developers/discord-social-sdk/development-guides/setting-rich-presence

## Implemented behavior

- A short activity line is always visible when Discord supplies one.
- The existing item delegate draws the second line; no per-member QWidget is created.
- The existing native item tooltip shows app, details, state, and party size when
  those fields are available.
- Live presence events update only the affected row.
- Playing is preferred over Listening, Competing, Watching, and Custom when several
  activities are present. Streaming has the highest priority.
- All remote strings are whitespace-normalized and bounded before being retained.

## Memory and background-work constraints

- No activity images or external asset URLs are downloaded or cached.
- No new polling loop, worker thread, animation, or timer was added.
- No global presence cache or user-to-row hash table was added. A presence change
  scans only the currently subscribed member-list window, which trades a very small
  bounded amount of CPU on an event for lower persistent RAM.
- Each loaded member retains at most two implicitly shared `QString` values: a
  128-character display line and a 512-character tooltip.
- If the tooltip would be identical to the visible line (the common simple-game
  case), only the visible string is retained and reused for hover text.
- Member-list virtualization and range eviction remain intact, so activity data is
  released together with rows outside subscribed ranges.

## Verification plan

- Unit-test activity selection, formatting, detail extraction, empty presence, and
  hostile oversized strings.
- Build the MinSizeRel application and run the full test suite.
- Compare idle process memory before and after. A logged-in field test is still
  required to verify real Discord accounts, games, Spotify/custom statuses, and
  rapid presence changes.

## Latest automated result

- Windows x64 `MinSizeRel` build: passed.
- Test executables: 7/7 passed, including the new activity formatting and Gateway
  payload parser coverage.
- Clean-profile Scenario A, hidden window, 10-second warm-up and 12 samples:
  83.01 MiB average working set, 15.21 MiB average private working set,
  41.29 MiB private bytes, 0.00% observed normalized CPU, 10 threads.
- Final executable: 21,470,208 bytes. These idle figures are recorded as a current
  measurement, not as proof of logged-in/member-list memory behavior; that requires
  the planned live Scenario C test.
