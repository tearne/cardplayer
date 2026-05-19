# Resume playback position

**Mode:** Formal

## Intent

The persisted-state change (`0.16.0`) restores the playing track on boot but loads it from byte 0 — the playhead position within the track is not preserved. So a user mid-album who powers off has to scrub back to where they were when they next power on. The intent is to make the resume feel like genuinely picking up where the listener left off: the saved track loads paused, *at the position it was at when the save happened*, ready to continue on space.

The mechanism already exists in the codebase — `g_src->getPos()` reads the current byte offset and `g_src->seek()` moves to a position, both used by the existing `[`/`]` and digit-key seek bindings. Persistence wraps these.

The same operating envelope as `persisted-state` applies: emergency shutdown does not save (whatever is dirty at cutoff is lost); corrupt or unreadable saved data falls back silently to defaults.

## Approach

### Hook into the existing dirty/flush model

`flushStateIfDirty` already runs once `PERSIST_FLUSH_MS = 5 s` after the most recent `markStateDirty()` call, coalescing bursts of state changes into one NVS write. The same pattern fits the playhead: a periodic "force dirty" tick while playing, and `flushStateIfDirty` captures `g_src->getPos()` at write time.

### 10 s save cadence

A timer in `loop()` calls `markStateDirty()` every 10 s while playback is active and not paused. With the existing 5 s debounce, the user loses at most ~10 s of position on a hard power-cut. Frequent enough to feel "where I left off"; gentle enough on NVS wear (worst case ~360 writes/hour, well within the wear-levelling budget).

### Position read under the audio mutex

`g_src` is mutated by the audio task (core 0). `flushStateIfDirty` runs on core 1. The position read is wrapped in `xSemaphoreTake(g_audio_mutex)` so we never read while the audio task is reconfiguring the source.

### Boot-side: seek after `startPlayback`

Boot-resume already calls `startPlayback(saved_track)` then sets `g_paused = true`. Add: if a saved position is present and ≥ `g_audio_start_offset` and < the source's size, `seekToByte(saved_pos)`. The user's first `space` resumes from that point.

If the saved position is out of range (track shrank, position never set, etc.), fall back to byte 0 — exact existing behaviour. No surprise jumps.

### Reset position on new-track / stop

`startPlayback` for a new file should clear the saved playhead (next save tick rewrites it with the new track's current position). `stopPlayback` likewise. Otherwise a stale position from track A could be applied to a freshly-started track B.

## Plan

### Tasks

- [x] Add a `g_playhead_dirty_ms` timer; in `loop()`, if playing and not paused, mark state dirty every 10 s
- [x] `flushStateIfDirty` captures `g_src->getPos()` under the audio mutex and writes it as a new NVS key (`trkpos`)
- [x] `loadState` reads the saved position into a `g_saved_playhead` variable
- [x] Boot-resume seeks to `g_saved_playhead` after `startPlayback` succeeds, with range validation
- [~] ~~`startPlayback` (new track path) and `stopPlayback` clear the saved playhead so it isn't re-applied to the wrong track~~ — turned out unnecessary: `g_saved_playhead` is only consumed once at boot-resume, then zeroed. Subsequent flushes write the live `g_src->getPos()`. NVS `trkpos` always reflects the currently-playing track's offset.
- [x] `resetState` (the user-confirmed reset modal) clears the saved playhead too

## Log

- 0.17.41: built clean. Handing back for the live test.
- One task became a no-op during build: I'd planned to clear `g_saved_playhead` on `startPlayback` / `stopPlayback` to prevent stale playhead from being applied to a new track. But since `g_saved_playhead` is consumed once at boot-resume (set to 0 immediately after) and never read again at runtime, that defensive clear is dead code. `flushStateIfDirty` always writes the live `g_src->getPos()` so NVS naturally tracks the current track's offset.

## Conclusion

Playhead position now persists alongside the track path. While playback is active and not paused, `loop()` calls `markStateDirty()` every 10 s; `flushStateIfDirty` captures `g_src->getPos()` under the audio mutex and writes it as the NVS key `trkpos`. On boot, `loadState` reads the value into `g_saved_playhead`; after the existing boot-resume's `startPlayback` succeeds, the position is applied via `seekToByte` with a range check (must be ≥ `g_audio_start_offset` and < the source's size, else falls back to byte 0).

The dirty-flush coalesces position writes with everything else the model already persists, so NVS-wear is bounded the same way as before.

**Changelog entry:**

> Resume playback at the saved position. After a power-cycle, the previously-playing track loads paused near where you left off (within ~10 s) — press space to continue. Position is saved every 10 s during playback alongside the rest of the persisted state.
