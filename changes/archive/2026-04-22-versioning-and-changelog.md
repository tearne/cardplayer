# Versioning and changelog

## Intent

Give the project a version number and a changelog so the user can tell which build is running on the device and see a history of what has changed between builds.

## Approach

Two pieces: a version string baked into the firmware and shown in the header, and a `CHANGELOG.md` file at the project root.

**Version string.** A single `VERSION` constant in the source, starting at `0.1.0`. Semver. Bumped by hand when cutting a build.

**On-device display.** Rendered at the left end of the header in small text, always visible. The header's right end has the battery icon; version sits at the left.

**Changelog file.** `CHANGELOG.md` at project root, Keep-a-Changelog style — reverse-chronological, dated. Terse entries. Distinct from the framework changelog at `changes/agent/CHANGELOG.md`.

**Legacy section.** A pre-`0.1.0` entry captures the PoC baseline (list browsing, playback of WAV/MP3/FLAC/AAC, keyboard controls). `0.1.0` itself covers the battery monitor plus this versioning work.

### Map edits

A new **Version** node is added as a child of **Header**, sibling to Battery. This also resolves the Battery only-child situation.

**New node — Version (child of Header):**

```markdown
# Version

[Up](#header)

The application's semver version string, rendered at the left end of the header in small text. Always visible.

**Detail**

- Stored as a `VERSION` constant in source; bumped by hand when cutting a build.

- Tracked alongside `CHANGELOG.md` at the project root.
```

**Updated node — Header (adds second Down link):**

```markdown
# Header

[Up](#screen-layout)
[Down](#version)
[Down](#battery)

A 10px strip at the top of the display carrying status indicators and transient notification banners. Status is always visible; notifications appear briefly in response to events (e.g. a volume change) then fade.
```

**Updated tree overview in the root:**

```
Application
├ Screen Layout
│ ├ Header
│ │ ├ Version
│ │ └ Battery
│ └ Footer
└ Controls
```

## Plan

- [x] Add a `VERSION` constant (`"0.1.0"`) in the source.

- [x] Render the version string at the left end of the header in small text as part of `drawHeader()`.

- [x] Create `CHANGELOG.md` at the project root with a `0.1.0` section (battery monitor, versioning) and a pre-`0.1.0` "Legacy" section capturing the PoC baseline.

- [x] Add the **Version** node to `map.md` as a child of Header.

- [x] Update the **Header** node in `map.md` — add `[Down](#version)` link.

- [x] Update the root tree overview in `map.md` to include Version under Header.

## Conclusion

Minor deviation: the constant is named `APP_VERSION` rather than `VERSION` — the latter collides with a macro defined in the build toolchain. Updated the Version node's Detail to match.

Not yet verified on-device — firmware compiles but the version text hasn't been eyeballed in the header.

