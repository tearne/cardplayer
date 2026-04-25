# Intra-track seek

## Intent

Add two ways to jump within the currently-playing track:

- Hold **`[`** or **`]`** to seek backward or forward in small increments while the key is held.
- Press a **number key** to jump to the corresponding tenth of the track — `1` = start, `2` = 10%, ..., `9` = 80%, `0` = 90%.

Existing controls (track-to-track skip with `<` / `?`, browser navigation, play/pause, volume) are unchanged.

## Approach

### Seek by byte offset on `g_src`

Both mechanisms call `g_src->seek(target_bytes, SEEK_SET)` under `g_audio_mutex` (same lifecycle protection as start/stop). The decoder picks up from the new position on its next `loop()` iteration — for MP3, that's a brief audible glitch then re-sync. For VBR files the byte position is an approximation of time position; accept this rather than parsing duration.

For percentage jump, target = `(g_src->getSize() * digit_index) / 10`, where `digit_index` is `0..9` mapping from keys `1..0` (key `1` → 0%, key `0` → 90%).

For incremental seek, each polling tick applies a fixed-byte delta proportional to the file size — proposed `1%` per tick, rate-limited to roughly ten ticks per second so a half-second hold moves about 5% through the track.

### Polling path for held seek keys

Existing keyboard handling fires only on `isChange()`. Held-to-seek needs to sample state every main-loop pass regardless. Add a separate path that runs once per iteration: if the current `word` contains `[` or `]`, apply a seek delta with the rate limit above. The change-driven `switch` doesn't add cases for `[` / `]`, so they're handled only by the polling path — no double-fire on initial press.

The polling path doesn't check pause state — scrubbing while paused is permitted (move the source position; on resume, audio plays from the new spot).

### Map edits

**Updated node — Controls (two new bindings added):**

```markdown
# Controls

[Up](#application)

Everything the user does is via the Cardputer's keyboard — there is no touch or rotary input. The current bindings:

- `;` / `.` — move selection up / down

- `,` / `/` — step out to parent / enter highlighted directory (left / right arrow)

- `<` / `?` — skip to previous / next track within the playing directory

- `[` / `]` — held: seek backward / forward within the current track

- `1`–`0` — jump to a position within the current track (`1` = 0%, `2` = 10%, …, `0` = 90%)

- `enter` — play the selected track (or descend into a directory)

- `space` — pause / resume

- `-` / `=` — volume down / up
```

## Plan

- [x] Add a `seekToByte(uint32_t target)` helper that takes `g_audio_mutex`, calls `g_src->seek(target, SEEK_SET)` if `g_src` is non-null, and releases.

- [x] In the change-driven keyboard switch in `loop()`, handle digit keys `0`–`9`: compute `digit_index` (key `1`→0, `2`→1, …, `9`→8, `0`→9) and seek to `(g_src->getSize() * digit_index) / 10` via `seekToByte`. Ignored when nothing is playing.

- [x] Add a polling block in `loop()` after the change-driven handling: if `[` or `]` is currently in `keysState().word` and at least 100ms since the last seek-tick, apply ±1% of file size via `seekToByte`. Track time via a file-static `g_last_seek_ms`.

- [x] Update the **Controls** node in `map.md` — add the `[` / `]` and `1`–`0` bindings.

## Log

- On-device test: paused scrubbing moved the source position but the progress bar didn't reflect it (only redraws when `playing`). Added a `drawFooter()` after each successful seek-tick so the bar tracks the source while scrubbing.

- Per-tick delta halved from 1% to 0.5% of file size — 1% felt too coarse on test tracks.

- Percentage seeks now operate on the audio range (file size minus the leading ID3v2 tag captured at file open) rather than the whole file. Without this, key `1` would seek to byte 0 of the file and the decoder would have to re-scan the tag — slow. The `g_audio_start_offset` is captured between `skipID3v2` and `gen->begin()` and reset in `stopPlayback`. Relative `[`/`]` seeks also use the audio range so the increment is consistent regardless of tag size.

- Map: Controls binding for `[` / `]` clarified — silent scrub during pause is now spelled out on the line.

## Conclusion

Verified on device: digit jumps and held `[` / `]` seek behave as expected, paused scrub updates the progress bar live, and `1` (jump to 0%) is now near-instant on tagged files.

