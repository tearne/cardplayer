# Auto-play next track

## Intent

When a track finishes naturally, the player currently stops. The aim is for it to continue automatically to the next audio file in the same folder, in filename order, until reaching the end of the folder. Manual skip with `<` / `?` remains available.

## Approach

### Audio task signals; main task advances

The audio task already detects end-of-track when `g_gen->loop()` returns false and clears `g_play_path`. It will additionally set an `advance_pending` flag at that moment. The main loop checks the flag once per iteration (after keyboard handling) and, if set, clears it and calls the existing `skipTrack(+1)`.

`skipTrack(+1)` already does the right thing at the end of the folder — it calls `stopPlayback` and refreshes the footer.

The trigger lives in the main task because starting a new track involves SD I/O (opening the file, parsing ID3v2, decoder init), which is awkward to do inside the audio task's mutex-protected critical section. The flag pattern keeps the audio task simple and reuses `skipTrack` rather than duplicating its end-of-folder logic.

`stopPlayback` clears the flag too, so any user-initiated stop/start (which routes through `stopPlayback`) cancels a pending advance and avoids a surprise advance during deliberate user action.

### Map edits

**Updated node — Playback (one Detail bullet added):**

```markdown
# Playback

[Up](#application)

Decoding of the current track runs on its own FreeRTOS task, separate from the main UI loop. The task must not risk blocking the ESP32 task watchdog — large ID3v2 tags, for example, can pin the decoder in a multi-second scan for the first audio frame. Decoded samples flow through a small ring buffer into the I2S driver.

**Detail**

- Audio task has 8KB stack, priority above the main loop and below driver tasks.

- Audio runs on core 1 with the main loop on core 0, set via `-DARDUINO_RUNNING_CORE=0` (the Arduino-ESP32 default is core 1). The task watchdog watches core 0's idle task only, so keeping audio off core 0 satisfies the no-blocking rule.

- Ring buffer has three slots of 1536 samples, giving the decoder ~100ms of slack before a stall is heard.

- At natural track end, advances to the next audio file in the playing folder (filename order); stops when the folder is exhausted. `<` / `?` provides manual override at any time.
```

## Plan

- [x] Add an `advance_pending` file-static `volatile bool`; set it in the audio task at end-of-track alongside the existing cleanup; clear it in `stopPlayback`.

- [x] In the main `loop()`, after keyboard handling, check the flag — if set, clear it and call `skipTrack(+1)`.

- [x] Update the **Playback** node in `map.md` — add the Detail bullet about end-of-track auto-advance.

## Conclusion

Verified on-device: a short track plays through and the next audio file in the folder starts automatically.
