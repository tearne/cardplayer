#pragma once

#include <stdint.h>
#include <stddef.h>
#include <vector>

// Fuzzy track-finding index — production interface.
//
// Backed by two files on the SD card:
//   /FZTI.idx — null-separated audio paths in scan order.
//   /FZTI.pb  — page-bloom filter + per-character precomputed top-K table.
//
// The index is built once per card; subsequent boots reuse it as long as
// the SD card's fingerprint (currently `SD.cardSize()`) matches the value
// stored in the page-bloom header. Manual rebuild is via startRebuild().

namespace FuzzyIndex {

struct Hit {
    int      score;
    uint32_t idx;   // path index within the index file
};

enum class State : uint8_t {
    Idle,       // no index files on disk
    Building,   // background build in progress
    Ready,      // files on disk valid, header cached; body NOT loaded
    Active,     // filters + unigram loaded in RAM, queries serve
};

// Load filters + unigram table into RAM. Synchronous SD read (~30 ms).
// No-op when already Active. Returns true on success / no-op, false when
// state isn't Ready (e.g. Idle or Building) or the read fails.
bool activate();

// Free filter and unigram vectors from RAM. Header stays cached so
// `pathCount()` keeps working. Transitions Active → Ready.
void deactivate();

// At SD-mount time, call once with the current card's fingerprint. If the
// existing index files match, the page-bloom is loaded into RAM and state
// becomes Ready. Otherwise a background build is kicked off and state stays
// Building until it completes.
void initAtBoot(uint64_t card_fingerprint);

// Kick off a background rebuild. No-op while a build is already running.
void startRebuild();

State    state();
uint32_t pathCount();   // 0 unless Ready

// Run a query against the loaded index. `out` is cleared at start. No-op
// (out stays empty) when state is not Ready.
void query(const char* q, std::vector<Hit>& out, int top_k);

// Resolve a hit's `idx` back to its path string. Reads from /FZTI.idx.
// Returns false if not Ready or `idx` out of range.
bool lookupPath(uint32_t idx, char* out, size_t cap);

}  // namespace FuzzyIndex
