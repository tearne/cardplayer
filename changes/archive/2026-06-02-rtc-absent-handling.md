# Handle a missing RTC gracefully

**Mode:** Formal

## Intent

A unit without the Port-A RTC add-on fitted floods the log with I²C errors and audio appears to stop. The RTC is also read far more often than it needs to be, and when the chip is absent every read fails on the shared I²C bus. The player should use the RTC sparingly, cope cleanly when it isn't fitted, and make clear to the user when timekeeping and alarms are unavailable.

## Approach

### Detect presence once at boot, latch a flag

Probe the RTC's I²C address (`0x51`) once after `Wire` is up; store the result in a `g_rtc_present` flag that gates every RTC access. *Reason: the Unit_RTC library returns nothing from `begin`/`getTime`/`getDate`, so a one-time address ACK is the only presence signal — and latching it avoids re-probing, which would itself be a failing transaction when the chip is absent.*

### Software wall-clock kept by `millis()`, re-synced from the RTC rarely

Maintain the time in software, advanced by `millis()`, seeded from the RTC at boot and re-synced on a slow cadence (a few minutes) and immediately after the user sets the time. All consumers — `pollAlarms`, the standby/clock screen, the next-alarm string, the header diagnostic clock — read this software clock; the chip is read only at boot and on re-sync. *Reason: the time only needs reading rarely — a free-running `millis()` clock gives seconds-resolution display and timely alarm checks while touching the I²C bus once every few minutes rather than every second.* A failed re-sync changes nothing — the software clock keeps running and the next good read corrects any drift; so a transient read error while the add-on is present is simply ridden out.

### When absent: no timekeeping, and say so

`g_rtc_present == false` means there is no seed for the software clock, alarms never arm or fire, and the alarm/time rows are hidden. The alarms sub-menu shows only a "no RTC / alarms unavailable" message in place of those rows, and the standby clock shows an explicit "no clock" state instead of a wrong time. *Reason: without timekeeping a displayed time would be wrong and alarms can't work; hiding the rows and stating it plainly stops the user trusting a bogus clock or configuring an alarm that can never fire.*

### Scope: audio symptom verified separately

This change commits to the I²C storm, the polling reduction, and the messaging. The reported audio stoppage is verified on hardware afterwards rather than assumed fixed, because the codec sits on a different I²C peripheral (internal, port 1) from the RTC (Port A, port 0) — so the failed reads can't be corrupting codec comms directly. *Reason: eliminating the failed reads is expected to help (likely via repeated read-blocking), but the causal chain isn't certain; if audio still fails it's a separate bug.*

## Plan

- [x] Probe the RTC at boot, latch `g_rtc_present`, and seed the RTC only when present.
- [x] Add a software wall-clock: seed from the RTC, advance by `millis()`, re-sync on a slow cadence; expose current date/time accessors. Touches the chip only at boot and re-sync, and only when present.
- [x] Route every time consumer (alarm poll, standby/clock screen, next-alarm string, header diagnostic clock, set-time editor seed) through the software clock.
- [x] Re-seed the software clock immediately after the user commits a new time.
- [x] Gate the alarm poll on `g_rtc_present` so alarms never arm or fire when absent.
- [x] Alarms sub-menu when absent: hide the time/alarm rows and show a "no RTC — alarms unavailable" message.
- [x] Standby clock when absent: show an explicit "no clock" state instead of a time.

## Log

- Software clock uses Howard Hinnant's `daysFromCivil`/`civilFromDays` to advance across day/month boundaries via an epoch-seconds count; re-sync cadence is 5 minutes (`RTC_RESYNC_MS`). The header diagnostic clock string stays `--:--:--` when absent (its poller early-returns), which doubles as a no-clock cue in the diag area.
- `pollRtcClock` rewritten in place (kept its name and call site): slow re-sync + once-a-second tick from the software clock, no per-second I²C.
- Alarms sub-menu when absent: rows hidden via an `alarmsRowShown` predicate; cursor movement skips hidden rows and `enterAlarms` opens on `AR_STANDBY`. Clock brightness stays reachable (it sets standby brightness, which still matters with no clock).
- Built clean for `cardplayer`. Not yet flashed — the audio-stoppage symptom in particular needs hardware confirmation (codec is on a separate I²C port from the RTC, per the Approach scope note).
- Post-test tweaks (user, on hardware): "RTC not connected" now drawn in a warn-orange (`COL_WARN` 0xFB00) on both the standby no-clock screen and the alarms sub-menu. The absent alarms sub-menu is now a bare warning — Clock brightness and Back rows dropped (standby brightness is still reachable via Fn+`-`/`=` in standby; `` ` ``/Del exit). Version 0.25.5 → 0.25.6.
- Confirmed on hardware: the FLAC load failures persist with the I²C storm gone and no I²C errors interleaved — so the audio bug is independent of the RTC, as the Approach scope note predicted. Out of scope here; to be chased as a separate change.

## Conclusion

Built to plan plus post-test UI tweaks (logged): the no-RTC messaging is warn-orange, and the absent alarms sub-menu is a bare warning with the Clock brightness and Back rows dropped (`` ` ``/Del exit; standby brightness stays reachable via Fn+`-`/`=`). Shipped 0.25.6.

The I²C storm and the over-polling are fixed — the hardware log came back free of I²C errors. The reported audio stoppage proved independent of the RTC (it persists with the storm gone), exactly as the Approach scope note anticipated; it's handed off to a separate change rather than chased here.

Documentation impact: RTC-absence handling and the software clock are new behaviour the map doesn't cover — a follow-up per-node negotiation.
