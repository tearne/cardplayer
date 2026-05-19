#include "fuzzy_harness.h"

#ifdef FUZZY_HARNESS

#include "fuzzy_index.h"

#include <Arduino.h>
#include <SD.h>
#include <FS.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <vector>
#include <cstring>

// Investigation harness — thin wrapper around the production FuzzyIndex
// module plus a synthetic-library generator. Compiled only when the
// FUZZY_HARNESS build flag is defined.
//
// Commands:
//   help                 — list commands
//   gen <N>              — populate /SYNTH with ~N synthetic tracks
//   rmsynth              — remove /SYNTH tree
//   rebuild              — force background index rebuild
//   state                — current FuzzyIndex state + path count
//   q <text>             — query, print top 10 with timing
//   bench                — time a fixed mix of queries
//   mem                  — internal-heap probe

static const char* WORDS[] = {
    "Echo", "Mirror", "River", "Halcyon", "Crimson", "Velvet", "Ember",
    "Twilight", "Aurora", "Solstice", "Cipher", "Zenith", "Cascade",
    "Lantern", "Meadow", "Phoenix", "Quartz", "Ravine", "Saffron",
    "Tundra", "Umbra", "Vesper", "Willow", "Xeno", "Yarrow", "Zephyr",
    "Beacon", "Drift", "Forge", "Glade", "Harbour", "Iris", "Junction"
};
static constexpr size_t N_WORDS = sizeof(WORDS) / sizeof(WORDS[0]);

static String synthArtistName(int i) {
    String s = WORDS[i % N_WORDS];
    s += " ";
    s += WORDS[(i / N_WORDS + 3) % N_WORDS];
    return s;
}
static String synthAlbumName(int artist, int album) {
    String s = WORDS[(artist * 7 + album) % N_WORDS];
    s += " of ";
    s += WORDS[(artist * 11 + album * 3) % N_WORDS];
    return s;
}
static String synthTrackName(int artist, int album, int track) {
    String s;
    if (track + 1 < 10) s += "0";
    s += String(track + 1);
    s += " ";
    s += WORDS[(artist + album * 5 + track * 13) % N_WORDS];
    s += " ";
    s += WORDS[(artist * 3 + album + track * 7) % N_WORDS];
    s += ".mp3";
    return s;
}

static void synthGenerate(int target_tracks) {
    constexpr int TRACKS_PER_ALBUM   = 12;
    constexpr int ALBUMS_PER_ARTIST  = 6;
    constexpr int TRACKS_PER_ARTIST  = TRACKS_PER_ALBUM * ALBUMS_PER_ARTIST;
    int n_artists = (target_tracks + TRACKS_PER_ARTIST - 1) / TRACKS_PER_ARTIST;
    SD.mkdir("/SYNTH");
    int written = 0;
    int64_t t0 = esp_timer_get_time();
    for (int a = 0; a < n_artists && written < target_tracks; a++) {
        String artist_dir = "/SYNTH/" + synthArtistName(a);
        SD.mkdir(artist_dir);
        for (int al = 0; al < ALBUMS_PER_ARTIST && written < target_tracks; al++) {
            String album_dir = artist_dir + "/" + synthAlbumName(a, al);
            SD.mkdir(album_dir);
            for (int tr = 0; tr < TRACKS_PER_ALBUM && written < target_tracks; tr++) {
                String track_path = album_dir + "/" + synthTrackName(a, al, tr);
                File f = SD.open(track_path, FILE_WRITE);
                if (!f) {
                    Serial.printf("HARNESS: failed to open %s\n", track_path.c_str());
                    return;
                }
                f.write((uint8_t)0);
                f.close();
                written++;
                if ((written & 63) == 0) {
                    Serial.printf("HARNESS: gen %d/%d\n", written, target_tracks);
                }
            }
        }
    }
    int64_t t1 = esp_timer_get_time();
    Serial.printf("HARNESS: gen done %d tracks in %llu ms\n",
                  written, (unsigned long long)((t1 - t0) / 1000));
}

static void synthRemoveAll(const String& path) {
    File d = SD.open(path);
    if (!d) return;
    if (!d.isDirectory()) { d.close(); SD.remove(path); return; }
    for (File f = d.openNextFile(); f; f = d.openNextFile()) {
        const char* name = f.name();
        const char* slash = strrchr(name, '/');
        const char* leaf = slash ? slash + 1 : name;
        String child = path;
        if (!child.endsWith("/")) child += "/";
        child += leaf;
        if (f.isDirectory()) { f.close(); synthRemoveAll(child); }
        else                 { f.close(); SD.remove(child); }
    }
    d.close();
    SD.rmdir(path);
}

static void reportMemory(const char* tag) {
    size_t free_internal    = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t largest_internal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t min_free         = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("HARNESS: mem[%s] free=%u largest=%u minfree=%u\n",
                  tag, (unsigned)free_internal,
                  (unsigned)largest_internal, (unsigned)min_free);
}

static const char* stateName(FuzzyIndex::State s) {
    switch (s) {
        case FuzzyIndex::State::Idle:     return "Idle";
        case FuzzyIndex::State::Building: return "Building";
        case FuzzyIndex::State::Ready:    return "Ready";
    }
    return "?";
}

static void printHits(const std::vector<FuzzyIndex::Hit>& hits) {
    char path[256];
    for (size_t i = 0; i < hits.size(); i++) {
        if (FuzzyIndex::lookupPath(hits[i].idx, path, sizeof(path))) {
            Serial.printf("  %4d  %s\n", hits[i].score, path);
        } else {
            Serial.printf("  %4d  <idx %u>\n", hits[i].score, hits[i].idx);
        }
    }
}

static void cmdQuery(const char* arg) {
    std::vector<FuzzyIndex::Hit> hits;
    int64_t t0 = esp_timer_get_time();
    FuzzyIndex::query(arg, hits, 10);
    int64_t t1 = esp_timer_get_time();
    Serial.printf("HARNESS: q='%s' matches=%u in %llu us\n",
                  arg, (unsigned)hits.size(),
                  (unsigned long long)(t1 - t0));
    printHits(hits);
}

static void cmdBench() {
    static const char* QUERIES[] = {
        "a", "ab", "abc", "abcd", "abcde",
        "rivem", "saffrn", "01", "mp3", "of"
    };
    std::vector<FuzzyIndex::Hit> hits;
    for (auto* q : QUERIES) {
        int64_t t0 = esp_timer_get_time();
        FuzzyIndex::query(q, hits, 10);
        int64_t t1 = esp_timer_get_time();
        Serial.printf("HARNESS: bench q='%-8s' len=%u matches=%-2u in %6llu us\n",
                      q, (unsigned)strlen(q), (unsigned)hits.size(),
                      (unsigned long long)(t1 - t0));
    }
}

static String g_serial_line;

static void handleLine(const String& raw) {
    String line = raw; line.trim();
    if (line.length() == 0) return;
    int sp = line.indexOf(' ');
    String cmd = (sp < 0) ? line : line.substring(0, sp);
    String arg = (sp < 0) ? String("") : line.substring(sp + 1);
    arg.trim();

    if      (cmd == "help") {
        Serial.println("HARNESS commands:");
        Serial.println("  help, gen <N>, rmsynth, rebuild, state,");
        Serial.println("  q <text>, bench, mem");
    }
    else if (cmd == "gen")     synthGenerate(arg.toInt());
    else if (cmd == "rmsynth") { synthRemoveAll("/SYNTH"); Serial.println("HARNESS: /SYNTH removed"); }
    else if (cmd == "rebuild") { FuzzyIndex::startRebuild(); Serial.println("HARNESS: rebuild kicked off"); }
    else if (cmd == "state")   {
        Serial.printf("HARNESS: state=%s paths=%u\n",
                      stateName(FuzzyIndex::state()),
                      (unsigned)FuzzyIndex::pathCount());
    }
    else if (cmd == "q")       cmdQuery(arg.c_str());
    else if (cmd == "bench")   cmdBench();
    else if (cmd == "mem")     reportMemory("probe");
    else Serial.printf("HARNESS: unknown '%s' (try 'help')\n", cmd.c_str());
}

void fuzzyHarnessSetup() {
    Serial.println();
    Serial.println("HARNESS: ready. Type 'help'.");
}

void fuzzyHarnessPoll() {
    while (Serial.available()) {
        int c = Serial.read();
        if (c < 0) break;
        if (c == '\r') continue;
        if (c == '\n') {
            handleLine(g_serial_line);
            g_serial_line = "";
        } else {
            g_serial_line += (char)c;
            if (g_serial_line.length() > 256) g_serial_line = "";
        }
    }
}

#endif // FUZZY_HARNESS
