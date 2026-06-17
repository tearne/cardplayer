# Auto-rescan the file index on content change

## Intent

The fuzzy file index only rebuilds at boot when the SD card's *capacity* fingerprint changes, or on a manual Settings → Rebuild index. Adding or removing music therefore doesn't update search — new tracks silently won't appear until the user remembers to rebuild. The user would like the index to notice content changes and rescan itself.

Whether this is worth building is genuinely open and should be weighed before any work starts. The one cheap signal to hand — the filesystem's used-bytes figure — is a blunt heuristic: it shifts on any write, not just library changes; it misses edits that net to the same size; and it still isn't a true content check. A detector accurate enough to trust approaches the cost of just rebuilding, which is why the rescan is manual today. This change may legitimately conclude that the status quo is the right answer.
