# Slow MP3 open

## Intent

Some MP3 tracks take a noticeable delay to start playing after selection. Investigation during the `glitch-free-audio` change traced this to MP3 files with large ID3v2 tags (typically embedded album art): the decoder byte-scans through the tag data searching for the first audio frame, and at SD-read speeds this takes several seconds for megabyte-scale tags.

With the core-swap fix in `glitch-free-audio`, this no longer crashes — but the UI still freezes while the decoder scans, because the initial `begin()` runs on the main task.

Aim: playback should start promptly regardless of the file's tagging, and selection should remain responsive throughout.

## Approach

### Skip the ID3v2 tag before `gen->begin()`

If the file starts with an ID3v2 header (`ID3` magic plus a 10-byte header carrying a synchsafe 32-bit tag size), seek the source past the tag before handing it to the decoder. The decoder then sees the first audio frame immediately rather than byte-scanning through embedded artwork. Files without a valid ID3v2 header are untouched and behave as today.

This addresses both the slow-open and the UI-freeze together, because almost all problematic files in practice are ones with embedded artwork. Pathological non-ID3 cases (rare, malformed files) would still cause a brief freeze — defer reworking the threading model until we hit one in the wild.

Parser is strict: only skip when the header is well-formed and the size field plausible. If anything looks off, leave the source untouched and let the decoder do its usual thing — same behaviour as today, no regression.

## Plan

- [x] Add an ID3v2-skip helper that, given an `AudioFileSource`, peeks at the first 10 bytes, validates the `ID3` magic and version, parses the synchsafe 32-bit size, and seeks the source past the tag. Leaves the source untouched when the header is absent, mismatched, or implausibly sized.

- [x] Call the helper in `startPlayback` (`main.cpp`) immediately after constructing the `AudioFileSourceSD` and before `gen->begin()`.

## Log

- The skip is gated on `.mp3` extension at the call site rather than applied to all formats. The Approach text said "files without a valid ID3v2 header are untouched"; other formats (FLAC, AAC) can in theory carry ID3v2 prefixes too, but in practice it's an MP3-specific issue and other decoders may not gracefully resume from a pre-stripped stream. Conservative scoping. If FLAC/AAC files start showing slow opens, broaden then.

## Conclusion

Spot-checked across files with and without embedded artwork: tracks that previously took several seconds to open now start within a fraction of a second; tag-less files behave as before. The only deviation from the Approach (`.mp3`-only scoping at the call site) is captured in the Log.
