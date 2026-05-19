#include "fuzzy_index.h"

#include <Arduino.h>
#include <SD.h>
#include <FS.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <vector>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <climits>

// File format —
//   /FZTI.idx : SdIndexHeader + null-separated audio paths in scan order.
//   /FZTI.pb  : PbHeader + page_starts[] + per-page Bloom filters
//                       + 256 × PB_UNI_K UniHit entries (top-K per char).
//
// PbHeader carries `card_fingerprint` so a card swap invalidates the index.

namespace {

constexpr const char* SD_INDEX_PATH    = "/FZTI.idx";
constexpr uint32_t    SD_INDEX_MAGIC   = 0x49545A46;  // "FZTI"
constexpr uint32_t    SD_INDEX_VERSION = 1;

constexpr const char* PB_PATH        = "/FZTI.pb";
constexpr uint32_t    PB_MAGIC       = 0x46425A46;  // "FZBF"
constexpr uint32_t    PB_VERSION     = 4;            // v4 indexes leaves only
constexpr uint32_t    PB_PAGE_SIZE   = 64;
constexpr uint32_t    PB_FILTER_BITS = 4096;
constexpr uint32_t    PB_FILTER_BYTES = PB_FILTER_BITS / 8;
constexpr uint32_t    PB_UNI_K       = 16;

struct SdIndexHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t n_paths;
    uint32_t reserved;
};

struct PbHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t n_paths;
    uint32_t n_pages;
    uint32_t page_size;
    uint32_t filter_bits;
    uint32_t paths_block_off;
    uint32_t uni_off;
    uint32_t uni_k;
    uint64_t card_fingerprint;
};

struct UniHit {
    uint16_t path_id;
    int16_t  score;
};

constexpr int SCORE_NONE = INT_MIN;

// Loaded index state. Owned by this TU; written by the bg task and read by
// the foreground (loopTask). Single-byte/aligned volatile reads are
// sufficient — state transitions only happen at known points in the bg task.
volatile FuzzyIndex::State s_state = FuzzyIndex::State::Idle;
PbHeader                   s_pb_hdr {};
std::vector<uint32_t>      s_pb_page_starts;
std::vector<uint8_t>       s_pb_filters;
std::vector<UniHit>        s_pb_uni;
uint64_t                   s_card_fingerprint = 0;
TaskHandle_t               s_bg_task = nullptr;

// ---- Helpers ----

char foldChar(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c + 32);
    return c;
}

bool isWordBoundary(char prev) {
    return prev == '\0' || prev == '/' || prev == ' ' ||
           prev == '_'  || prev == '-' || prev == '.';
}

bool isAudioExt(const char* name) {
    size_t n = strlen(name);
    auto cmp = [&](const char* ext) {
        size_t e = strlen(ext);
        if (n < e) return false;
        for (size_t i = 0; i < e; i++) {
            if (tolower((unsigned char)name[n - e + i]) != ext[i]) return false;
        }
        return true;
    };
    return cmp(".mp3") || cmp(".flac") || cmp(".wav") ||
           cmp(".aac") || cmp(".m4a")  || cmp(".mp4");
}

// Score a *leaf* (filename) against the query. Path/directory text is not
// considered — callers strip the leaf before invoking. Bonuses are
// concentrated on consecutive runs:
//   +1 base per matched query char,
//   +6 if the previous query char also matched the immediately-preceding
//     leaf char (consecutive),
//   +run_length extra on each subsequent char in the same run, so longer
//     runs gain super-linearly,
//   -(leaf_chars_after_last_match / 32) trailing-length penalty.
// No word-boundary bonus by design.
int scoreMatch(const char* leaf, const char* query) {
    if (*query == '\0') return 0;
    int score = 0;
    int run = 0;  // length of the current consecutive-match run
    const char* p = leaf;
    const char* q = query;
    while (*p) {
        char pc = (char)tolower((unsigned char)*p);
        char qc = (char)tolower((unsigned char)*q);
        if (pc == qc) {
            score += 1;
            if (run > 0) score += 6 + run;
            run++;
            q++;
            if (*q == '\0') {
                score -= (int)(strlen(p + 1)) / 32;
                return score;
            }
        } else {
            run = 0;
        }
        p++;
    }
    return SCORE_NONE;
}

// Extract the leaf (filename) from a full path.
inline const char* leafOf(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

uint32_t pbHashTri(uint8_t a, uint8_t b, uint8_t c) {
    uint32_t h = 0x811c9dc5u;
    h = (h ^ a) * 0x01000193u;
    h = (h ^ b) * 0x01000193u;
    h = (h ^ c) * 0x01000193u;
    return h;
}

inline void pbSetBit(uint8_t* filter, uint32_t bit) {
    filter[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}
inline bool pbGetBit(const uint8_t* filter, uint32_t bit) {
    return (filter[bit >> 3] & (uint8_t)(1u << (bit & 7))) != 0;
}

void pbAddLeaf(const char* leaf, uint8_t* filter) {
    size_t len = strlen(leaf);
    if (len < 3) return;
    for (size_t i = 0; i + 3 <= len; i++) {
        uint32_t bit = pbHashTri(foldChar(leaf[i]),
                                 foldChar(leaf[i + 1]),
                                 foldChar(leaf[i + 2])) % PB_FILTER_BITS;
        pbSetBit(filter, bit);
    }
}

// Per-leaf unigram accumulation — for every distinct character that
// appears in the leaf, push its score onto the relevant top-K heap.
// Scoring mirrors scoreMatch for query-length-1: base +1 minus the
// trailing-length penalty (no word-boundary bonus, no consecutive bonus
// applicable to a single character).
void pbAccumulateUnigrams(const char* leaf, uint16_t path_id,
                          std::vector<std::vector<UniHit>>& heaps) {
    bool seen[256] = {false};
    size_t len = strlen(leaf);
    for (size_t i = 0; i < len; i++) {
        unsigned char idx = (unsigned char)foldChar(leaf[i]);
        if (seen[idx]) continue;
        seen[idx] = true;
        int score = 1 - (int)((len - i - 1) / 32);
        auto& heap = heaps[idx];
        UniHit hit{path_id, (int16_t)score};
        auto cmp = [](const UniHit& a, const UniHit& b){ return a.score > b.score; };
        if ((int)heap.size() < (int)PB_UNI_K) {
            heap.push_back(hit);
            std::push_heap(heap.begin(), heap.end(), cmp);
        } else if (score > heap.front().score) {
            std::pop_heap(heap.begin(), heap.end(), cmp);
            heap.back() = hit;
            std::push_heap(heap.begin(), heap.end(), cmp);
        }
    }
}

int pbQueryBits(const char* query, uint32_t* out_bits, int max_bits) {
    size_t len = strlen(query);
    if (len < 3) return 0;
    int n = 0;
    for (size_t i = 0; i + 3 <= len && n < max_bits; i++) {
        out_bits[n++] = pbHashTri(foldChar(query[i]),
                                  foldChar(query[i + 1]),
                                  foldChar(query[i + 2])) % PB_FILTER_BITS;
    }
    return n;
}

// ---- Build (paths file) ----

void sdBuild() {
    File out = SD.open(SD_INDEX_PATH, FILE_WRITE);
    if (!out) return;
    SdIndexHeader hdr {SD_INDEX_MAGIC, SD_INDEX_VERSION, 0, 0};
    out.write((const uint8_t*)&hdr, sizeof(hdr));

    uint32_t n_paths = 0;
    int yield_counter = 0;

    std::vector<String> worklist;
    worklist.push_back("/");
    while (!worklist.empty()) {
        String dir = worklist.back();
        worklist.pop_back();
        File d = SD.open(dir);
        if (!d || !d.isDirectory()) { if (d) d.close(); continue; }
        for (File f = d.openNextFile(); f; f = d.openNextFile()) {
            if ((++yield_counter & 31) == 0) vTaskDelay(1);
            const char* name = f.name();
            const char* slash = strrchr(name, '/');
            const char* leaf = slash ? slash + 1 : name;
            if (leaf[0] == '\0' || leaf[0] == '.') { f.close(); continue; }
            String full = dir;
            if (!full.endsWith("/")) full += "/";
            full += leaf;
            if (f.isDirectory()) {
                worklist.push_back(full);
            } else if (isAudioExt(leaf)) {
                size_t len = full.length();
                out.write((const uint8_t*)full.c_str(), len + 1);
                n_paths++;
            }
            f.close();
        }
        d.close();
    }
    out.seek(0);
    hdr.n_paths = n_paths;
    out.write((const uint8_t*)&hdr, sizeof(hdr));
    out.close();
}

bool sdLookupPath(uint32_t want_idx, char* out, size_t out_cap) {
    File f = SD.open(SD_INDEX_PATH, FILE_READ);
    if (!f) return false;
    SdIndexHeader hdr;
    f.read((uint8_t*)&hdr, sizeof(hdr));

    static constexpr size_t CHUNK = 1024;
    static uint8_t buf[CHUNK];
    uint32_t idx = 0;
    size_t out_len = 0;
    while (true) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            if (buf[i] == 0) {
                if (idx == want_idx) {
                    out[out_len < out_cap ? out_len : out_cap - 1] = 0;
                    f.close();
                    return true;
                }
                idx++;
                out_len = 0;
            } else if (idx == want_idx && out_len + 1 < out_cap) {
                out[out_len++] = (char)buf[i];
            }
        }
    }
    f.close();
    return false;
}

// Flat scan fallback for queries < 3 chars (no trigrams to filter on).
void sdQueryFlat(const char* query, int top_k, std::vector<FuzzyIndex::Hit>& out) {
    out.clear();
    File f = SD.open(SD_INDEX_PATH, FILE_READ);
    if (!f) return;
    SdIndexHeader hdr;
    f.read((uint8_t*)&hdr, sizeof(hdr));
    if (hdr.magic != SD_INDEX_MAGIC) { f.close(); return; }

    static constexpr size_t CHUNK = 4096;
    static constexpr size_t MAX_PATH = 256;
    static uint8_t buf[CHUNK];
    static char    carry[MAX_PATH];
    size_t   carry_len = 0;
    uint32_t path_idx = 0;

    auto cmp = [](const FuzzyIndex::Hit& a, const FuzzyIndex::Hit& b){
        return a.score > b.score;
    };

    while (true) {
        int n = f.read(buf, sizeof(buf));
        if (n <= 0) break;
        size_t start = 0;
        for (int i = 0; i < n; i++) {
            if (buf[i] == 0) {
                size_t seg = i - start;
                const char* path_str;
                if (carry_len) {
                    if (carry_len + seg + 1 > MAX_PATH) {
                        carry_len = 0; start = i + 1; continue;
                    }
                    memcpy(carry + carry_len, buf + start, seg);
                    carry[carry_len + seg] = 0;
                    path_str = carry;
                    carry_len = 0;
                } else {
                    buf[i] = 0;
                    path_str = (const char*)(buf + start);
                }
                int s = scoreMatch(leafOf(path_str), query);
                if (s != SCORE_NONE) {
                    if ((int)out.size() < top_k) {
                        out.push_back({s, path_idx});
                        std::push_heap(out.begin(), out.end(), cmp);
                    } else if (s > out.front().score) {
                        std::pop_heap(out.begin(), out.end(), cmp);
                        out.back() = {s, path_idx};
                        std::push_heap(out.begin(), out.end(), cmp);
                    }
                }
                path_idx++;
                start = i + 1;
            }
        }
        size_t tail = n - start;
        if (tail) {
            if (carry_len + tail > MAX_PATH) carry_len = 0;
            else { memcpy(carry + carry_len, buf + start, tail); carry_len += tail; }
        }
        if ((path_idx & 31) == 0) vTaskDelay(1);
    }
    f.close();
    std::sort(out.begin(), out.end(), cmp);
}

// ---- Build (page-bloom + unigram table) ----

void pbBuild(uint64_t card_fingerprint) {
    File in = SD.open(SD_INDEX_PATH, FILE_READ);
    if (!in) return;
    SdIndexHeader sh;
    in.read((uint8_t*)&sh, sizeof(sh));
    if (sh.magic != SD_INDEX_MAGIC) { in.close(); return; }
    uint32_t n_paths = sh.n_paths;
    uint32_t n_pages = (n_paths + PB_PAGE_SIZE - 1) / PB_PAGE_SIZE;
    uint32_t paths_block_off = sizeof(sh);

    std::vector<uint32_t> page_starts(n_pages, 0);
    std::vector<uint8_t>  filters(n_pages * PB_FILTER_BYTES, 0);
    std::vector<std::vector<UniHit>> uni_heaps(256);
    for (auto& h : uni_heaps) h.reserve(PB_UNI_K);

    char     path_buf[256];
    size_t   path_len = 0;
    uint32_t path_idx = 0;
    uint32_t cur_page = 0;
    uint32_t bytes_in_block = 0;
    page_starts[0] = 0;

    static uint8_t buf[2048];
    while (true) {
        int n = in.read(buf, sizeof(buf));
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            uint8_t b = buf[i];
            bytes_in_block++;
            if (b == 0) {
                path_buf[path_len < 255 ? path_len : 255] = 0;
                const char* leaf = leafOf(path_buf);
                pbAddLeaf(leaf, &filters[cur_page * PB_FILTER_BYTES]);
                pbAccumulateUnigrams(leaf, (uint16_t)path_idx, uni_heaps);
                path_len = 0;
                path_idx++;
                if ((path_idx % PB_PAGE_SIZE) == 0 && path_idx < n_paths) {
                    cur_page++;
                    page_starts[cur_page] = bytes_in_block;
                }
            } else if (path_len < 255) {
                path_buf[path_len++] = (char)b;
            }
        }
        if ((path_idx & 127) == 0) vTaskDelay(1);
    }
    in.close();

    std::vector<UniHit> uni_table(256 * PB_UNI_K, {0, (int16_t)-32768});
    for (int c = 0; c < 256; c++) {
        auto& heap = uni_heaps[c];
        std::sort_heap(heap.begin(), heap.end(),
            [](const UniHit& a, const UniHit& b){ return a.score > b.score; });
        for (size_t i = 0; i < heap.size(); i++) {
            uni_table[c * PB_UNI_K + i] = heap[i];
        }
    }

    File out = SD.open(PB_PATH, FILE_WRITE);
    if (!out) return;
    uint32_t uni_off = sizeof(PbHeader) + n_pages * sizeof(uint32_t)
                     + filters.size();
    PbHeader ph{PB_MAGIC, PB_VERSION, n_paths, n_pages, PB_PAGE_SIZE,
                PB_FILTER_BITS, paths_block_off, uni_off, PB_UNI_K,
                card_fingerprint};
    out.write((const uint8_t*)&ph, sizeof(ph));
    out.write((const uint8_t*)page_starts.data(), n_pages * sizeof(uint32_t));
    out.write(filters.data(), filters.size());
    out.write((const uint8_t*)uni_table.data(),
              uni_table.size() * sizeof(UniHit));
    out.close();
}

// ---- Load / unload ----

// Validate the header on disk without loading filters or unigram into RAM.
// Caches `s_pb_hdr` + `s_pb_page_starts` (the small structures) so that
// pathCount() and per-page seek math work without the body in RAM.
bool pbVerifyHeaderOnDisk(uint64_t card_fingerprint) {
    File f = SD.open(PB_PATH, FILE_READ);
    if (!f) return false;
    f.read((uint8_t*)&s_pb_hdr, sizeof(s_pb_hdr));
    if (s_pb_hdr.magic != PB_MAGIC || s_pb_hdr.version != PB_VERSION) {
        f.close();
        s_pb_hdr = {};
        return false;
    }
    if (s_pb_hdr.card_fingerprint != card_fingerprint) {
        f.close();
        s_pb_hdr = {};
        return false;
    }
    s_pb_page_starts.assign(s_pb_hdr.n_pages, 0);
    f.read((uint8_t*)s_pb_page_starts.data(),
           s_pb_hdr.n_pages * sizeof(uint32_t));
    f.close();
    return true;
}

// Load filters + unigram table into RAM. Header and page_starts already
// cached by `pbVerifyHeaderOnDisk`.
bool pbLoadBody() {
    File f = SD.open(PB_PATH, FILE_READ);
    if (!f) return false;
    s_pb_filters.assign(s_pb_hdr.n_pages * (s_pb_hdr.filter_bits / 8), 0);
    f.seek(sizeof(PbHeader) + s_pb_hdr.n_pages * sizeof(uint32_t));
    f.read(s_pb_filters.data(), s_pb_filters.size());
    s_pb_uni.assign(256 * s_pb_hdr.uni_k, {0, (int16_t)-32768});
    f.seek(s_pb_hdr.uni_off);
    f.read((uint8_t*)s_pb_uni.data(), s_pb_uni.size() * sizeof(UniHit));
    f.close();
    return true;
}

// Free only the body (filters + unigram) — header and page_starts stay
// cached. Used by deactivate(); pbUnload() is the full reset for rebuilds.
void pbFreeBody() {
    s_pb_filters.clear();
    s_pb_filters.shrink_to_fit();
    s_pb_uni.clear();
    s_pb_uni.shrink_to_fit();
}

void pbUnload() {
    s_pb_page_starts.clear();
    s_pb_page_starts.shrink_to_fit();
    pbFreeBody();
    s_pb_hdr = {};
}

// ---- Query ----

void pbQuery(const char* query, int top_k, std::vector<FuzzyIndex::Hit>& out) {
    out.clear();
    size_t qlen = strlen(query);
    if (qlen == 0) return;

    if (qlen == 1) {
        unsigned char c = (unsigned char)foldChar(query[0]);
        const UniHit* row = &s_pb_uni[c * s_pb_hdr.uni_k];
        for (uint32_t i = 0; i < s_pb_hdr.uni_k && (int)out.size() < top_k; i++) {
            if (row[i].score == (int16_t)-32768) break;
            out.push_back({row[i].score, row[i].path_id});
        }
        return;
    }

    uint32_t qbits[64];
    int n_qbits = pbQueryBits(query, qbits, 64);
    if (n_qbits == 0) {
        sdQueryFlat(query, top_k, out);
        return;
    }

    std::vector<uint32_t> hit_pages;
    hit_pages.reserve(s_pb_hdr.n_pages);
    uint32_t fb = s_pb_hdr.filter_bits / 8;
    for (uint32_t p = 0; p < s_pb_hdr.n_pages; p++) {
        const uint8_t* filter = &s_pb_filters[p * fb];
        bool all = true;
        for (int b = 0; b < n_qbits; b++) {
            if (!pbGetBit(filter, qbits[b])) { all = false; break; }
        }
        if (all) hit_pages.push_back(p);
    }

    File f = SD.open(SD_INDEX_PATH, FILE_READ);
    if (!f) return;

    static uint8_t buf[4096];
    static char    path_buf[256];

    auto cmp = [](const FuzzyIndex::Hit& a, const FuzzyIndex::Hit& b){
        return a.score > b.score;
    };

    for (uint32_t hp : hit_pages) {
        uint32_t start = s_pb_page_starts[hp];
        uint32_t end   = (hp + 1 < s_pb_hdr.n_pages) ? s_pb_page_starts[hp + 1] : 0;
        uint32_t length = end ? (end - start) : 0;
        f.seek(s_pb_hdr.paths_block_off + start);

        uint32_t page_path_id = hp * s_pb_hdr.page_size;
        size_t   path_len = 0;
        uint32_t consumed = 0;

        while (true) {
            int want = sizeof(buf);
            if (length && consumed + want > length) want = length - consumed;
            if (length && want <= 0) break;
            int n = f.read(buf, want);
            if (n <= 0) break;
            consumed += n;
            for (int i = 0; i < n; i++) {
                if (buf[i] == 0) {
                    path_buf[path_len < 255 ? path_len : 255] = 0;
                    int s = scoreMatch(leafOf(path_buf), query);
                    if (s != SCORE_NONE) {
                        if ((int)out.size() < top_k) {
                            out.push_back({s, page_path_id});
                            std::push_heap(out.begin(), out.end(), cmp);
                        } else if (s > out.front().score) {
                            std::pop_heap(out.begin(), out.end(), cmp);
                            out.back() = {s, page_path_id};
                            std::push_heap(out.begin(), out.end(), cmp);
                        }
                    }
                    page_path_id++;
                    path_len = 0;
                } else if (path_len < 255) {
                    path_buf[path_len++] = (char)buf[i];
                }
            }
            if (length == 0 && n < want) break;
        }
    }
    f.close();
    std::sort(out.begin(), out.end(), cmp);
}

// ---- Background build ----

void buildTaskFn(void*) {
    sdBuild();
    pbBuild(s_card_fingerprint);
    // Verify only — lazy-load. Body is read into RAM later by activate().
    if (pbVerifyHeaderOnDisk(s_card_fingerprint)) {
        s_state = FuzzyIndex::State::Ready;
    } else {
        s_state = FuzzyIndex::State::Idle;
    }
    s_bg_task = nullptr;
    vTaskDelete(nullptr);
}

void startBuildTask() {
    if (s_bg_task) return;
    pbUnload();
    s_state = FuzzyIndex::State::Building;
    xTaskCreatePinnedToCore(buildTaskFn, "fuzzyidx", 8 * 1024, nullptr, 1,
                            &s_bg_task, 1);
}

}  // anonymous namespace

namespace FuzzyIndex {

void initAtBoot(uint64_t card_fingerprint) {
    s_card_fingerprint = card_fingerprint;
    // Verify only — load body on demand via activate().
    if (pbVerifyHeaderOnDisk(card_fingerprint)) {
        s_state = State::Ready;
        return;
    }
    startBuildTask();
}

void startRebuild() { startBuildTask(); }

State    state()     { return s_state; }

uint32_t pathCount() {
    // Header is cached as long as state is Ready or Active.
    if (s_state == State::Ready || s_state == State::Active) {
        return s_pb_hdr.n_paths;
    }
    return 0;
}

bool activate() {
    if (s_state == State::Active) return true;
    if (s_state != State::Ready) return false;
    if (!pbLoadBody()) return false;
    s_state = State::Active;
    return true;
}

void deactivate() {
    if (s_state != State::Active) return;
    pbFreeBody();
    s_state = State::Ready;
}

void query(const char* q, std::vector<Hit>& out, int top_k) {
    out.clear();
    if (s_state != State::Active) return;
    pbQuery(q, top_k, out);
}

bool lookupPath(uint32_t idx, char* out, size_t cap) {
    if (s_state != State::Active) return false;
    return sdLookupPath(idx, out, cap);
}

}  // namespace FuzzyIndex
