#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <string>
#include <algorithm>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <AudioFileSourceSD.h>
#include <AudioGenerator.h>
#include <AudioGeneratorWAV.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorFLAC.h>
#include <AudioGeneratorAAC.h>

#include "audio_output_m5.h"

#define SD_SCK  40
#define SD_MISO 39
#define SD_MOSI 14
#define SD_CS   12

static constexpr const char* APP_VERSION = "0.5.0";

static constexpr int SCREEN_W     = 240;
static constexpr int SCREEN_H     = 135;

static constexpr int HEADER_H     = 18;
static constexpr int HEADER_ROW2_Y = 10;

static constexpr uint32_t AUDIO_TASK_STACK    = 8 * 1024;
static constexpr UBaseType_t AUDIO_TASK_PRIO  = 3;
static constexpr BaseType_t AUDIO_TASK_CORE   = 1;
static constexpr uint32_t DIAGNOSTICS_POLL_MS = 1000;
static constexpr int FOOTER_H     = 16;
static constexpr int BROWSER_Y    = HEADER_H;
static constexpr int BROWSER_H    = SCREEN_H - HEADER_H - FOOTER_H;
static constexpr int FOOTER_Y     = HEADER_H + BROWSER_H;

static constexpr int COL_CUR_W    = (SCREEN_W * 4) / 5;       // 192
static constexpr int COL_PREV_X   = COL_CUR_W;
static constexpr int COL_PREV_W   = SCREEN_W - COL_CUR_W;     // 48

static constexpr int ROW_H        = 9;
static constexpr int CHAR_W       = 6;
static constexpr int COL_PAD      = 2;

static constexpr int MAX_VOL      = 10;
static constexpr uint32_t BATTERY_POLL_MS = 5000;

static constexpr int BATTERY_ICON_W = 30;
static constexpr int BATTERY_ICON_H = 8;
static constexpr int BATTERY_ICON_X = SCREEN_W - BATTERY_ICON_W - 3;
static constexpr int BATTERY_ICON_Y = 1;
static constexpr uint32_t BATTERY_LOW_TIMEOUT_MS = 10000;

// Dev-mode flag: raises the empty thresholds so the warning + immediate-sleep
// paths can be exercised without depleting the cell. Production thresholds
// when false are 3400 / 3300 mV.
static constexpr bool BATTERY_DEV_MODE = true;
static constexpr int LOADED_EMPTY_MV   = BATTERY_DEV_MODE ? 3700 : 3400;
static constexpr int CRITICAL_EMPTY_MV = BATTERY_DEV_MODE ? 3600 : 3300;
static constexpr int LOADED_FULL_MV    = 3830;

// Colours (RGB565). Each kind has three brightness tiers:
// BRIGHT (selected), NORMAL (non-selected active column), DIM (preview column).
static constexpr uint16_t COL_DIR_BRIGHT    = 0x5D9F;
static constexpr uint16_t COL_DIR_NORMAL    = 0x3B7D;
static constexpr uint16_t COL_DIR_DIM       = 0x2CD3;
static constexpr uint16_t COL_FILE_BRIGHT   = 0xFFFF;
static constexpr uint16_t COL_FILE_NORMAL   = 0xBDF7;
static constexpr uint16_t COL_FILE_DIM      = 0x7BEF;
static constexpr uint16_t COL_OTHER_BRIGHT  = 0x528A;
static constexpr uint16_t COL_OTHER_NORMAL  = 0x4208;
static constexpr uint16_t COL_OTHER_DIM     = 0x2104;
static constexpr uint16_t COL_ZEBRA_A    = 0x0000;  // black
static constexpr uint16_t COL_ZEBRA_B    = 0x0120;  // faint green-tinged
static constexpr uint16_t COL_SEL_BORDER = 0x8410;  // mid grey outline for selection
static constexpr uint16_t COL_HEADER_TXT = 0x7BEF;
static constexpr uint16_t COL_FOOTER_TXT = 0xFFFF;
static constexpr uint16_t COL_FOOTER_DIM = 0x7BEF;

enum EntryKind { KIND_DIR, KIND_AUDIO, KIND_OTHER };
struct Entry { std::string name; EntryKind kind; };

static std::string           g_cur_path = "/";
static std::vector<Entry>    g_entries;
static int                   g_cursor = 0;
static int                   g_top    = 0;

static std::vector<Entry>    g_preview;
static int                   g_preview_for_cursor = -1;

static std::string           g_play_path;
static std::string           g_play_dir;
static std::vector<Entry>    g_play_entries;
static int                   g_play_idx = -1;

static int  g_volume = 5;
static bool g_paused = false;
static volatile bool g_advance_pending = false;

static uint32_t g_last_seek_ms = 0;
static uint32_t g_audio_start_offset = 0;

static AudioFileSourceSD*             g_src = nullptr;
static AudioGenerator*                g_gen = nullptr;
static AudioOutputM5CardputerSpeaker* g_out = nullptr;

static uint32_t g_last_progress_ms = 0;

static int      g_battery_level     = -1;
static int      g_battery_voltage_mv = 0;
static uint32_t g_battery_last_ms   = 0;

static void enterBatteryLowState();

static SemaphoreHandle_t g_audio_mutex = nullptr;
static TaskHandle_t      g_audio_task  = nullptr;

static uint32_t g_diag_last_ms   = 0;
static uint32_t g_diag_stack_used = 0;
static uint32_t g_diag_underruns  = 0;
static uint32_t g_diag_wait_us    = 0;

static bool endsWith(const std::string& s, const char* ext) {
    std::string lower = s;
    for (auto& c : lower) c = tolower(c);
    auto n = strlen(ext);
    return lower.size() >= n && lower.compare(lower.size() - n, n, ext) == 0;
}

static bool isAudioName(const std::string& s) {
    return endsWith(s, ".wav") || endsWith(s, ".mp3") ||
           endsWith(s, ".flac") || endsWith(s, ".aac") ||
           endsWith(s, ".m4a") || endsWith(s, ".mp4");
}

static AudioGenerator* makeGenerator(const std::string& name) {
    if (endsWith(name, ".wav"))  return new AudioGeneratorWAV();
    if (endsWith(name, ".mp3"))  return new AudioGeneratorMP3();
    if (endsWith(name, ".flac")) return new AudioGeneratorFLAC();
    if (endsWith(name, ".aac"))  return new AudioGeneratorAAC();
    return nullptr;
}

static std::string basename(const std::string& path) {
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static std::string joinPath(const std::string& dir, const std::string& name) {
    if (dir == "/") return "/" + name;
    return dir + "/" + name;
}

static std::string parentPath(const std::string& path) {
    if (path == "/") return "/";
    auto slash = path.find_last_of('/');
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

static int kindOrder(EntryKind k) {
    switch (k) {
        case KIND_DIR:   return 0;
        case KIND_AUDIO: return 1;
        default:         return 2;
    }
}

static void scanDir(const std::string& path, std::vector<Entry>& out) {
    out.clear();
    File dir = SD.open(path.c_str());
    if (!dir || !dir.isDirectory()) return;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        std::string name = basename(std::string(f.name()));
        if (name.empty() || name[0] == '.') continue;
        EntryKind k;
        if (f.isDirectory())      k = KIND_DIR;
        else if (isAudioName(name)) k = KIND_AUDIO;
        else                      k = KIND_OTHER;
        out.push_back({name, k});
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
        int oa = kindOrder(a.kind), ob = kindOrder(b.kind);
        if (oa != ob) return oa < ob;
        return a.name < b.name;
    });
}

static int charsPerLine(int col_w) {
    int avail = col_w - COL_PAD;
    if (avail < CHAR_W) return 1;
    return avail / CHAR_W;
}

static int entryRows(const Entry& e, int col_w) {
    int cpl = charsPerLine(col_w);
    int n = (int)e.name.size();
    if (n <= 0) return 1;
    return (n + cpl - 1) / cpl;
}

enum Brightness { BRIGHT, NORMAL, DIM };

static uint16_t kindColour(EntryKind k, Brightness b) {
    switch (k) {
        case KIND_DIR:
            return b == BRIGHT ? COL_DIR_BRIGHT : b == NORMAL ? COL_DIR_NORMAL : COL_DIR_DIM;
        case KIND_AUDIO:
            return b == BRIGHT ? COL_FILE_BRIGHT : b == NORMAL ? COL_FILE_NORMAL : COL_FILE_DIM;
        default:
            return b == BRIGHT ? COL_OTHER_BRIGHT : b == NORMAL ? COL_OTHER_NORMAL : COL_OTHER_DIM;
    }
}

static void applyVolume() {
    uint8_t v = (uint8_t)((g_volume * 255) / MAX_VOL);
    M5Cardputer.Speaker.setVolume(v);
}

static void stopPlayback() {
    AudioGenerator* gen_to_free = nullptr;
    AudioFileSourceSD* src_to_free = nullptr;
    if (g_audio_mutex) xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
    if (g_gen && g_gen->isRunning()) g_gen->stop();
    gen_to_free = g_gen;
    src_to_free = g_src;
    g_gen = nullptr;
    g_src = nullptr;
    g_play_path.clear();
    g_play_dir.clear();
    g_play_entries.clear();
    g_play_idx = -1;
    g_paused = false;
    g_advance_pending = false;
    g_audio_start_offset = 0;
    if (g_audio_mutex) xSemaphoreGive(g_audio_mutex);
    delete gen_to_free;
    delete src_to_free;
}

static void audioTask(void*) {
    for (;;) {
        bool did_work = false;
        xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
        if (g_gen && g_gen->isRunning() && !g_paused) {
            if (!g_gen->loop()) {
                g_gen->stop();
                delete g_gen; g_gen = nullptr;
                delete g_src; g_src = nullptr;
                g_play_path.clear();
                g_advance_pending = true;
            }
            did_work = true;
        }
        xSemaphoreGive(g_audio_mutex);
        // Sleep one tick rather than taskYIELD(): yield only hands off to
        // equal-or-higher-priority tasks, so IDLE1 would never get a slot
        // for background cleanup.
        if (!did_work) vTaskDelay(10 / portTICK_PERIOD_MS);
        else           vTaskDelay(1);
    }
}

static uint16_t batteryColour(int level) {
    if (level > 80) return 0x07E0;
    if (level > 40) return 0x041F;
    if (level > 20) return 0xFFE0;
    if (level > 10) return 0xC000;
    return 0xF800;
}

static void drawDiagnosticsRow() {
    auto& d = M5Cardputer.Display;
    d.fillRect(0, HEADER_ROW2_Y, SCREEN_W, 8, BLACK);
    d.setTextSize(1);
    d.setTextColor(COL_HEADER_TXT, BLACK);

    d.setCursor(0, HEADER_ROW2_Y);
    d.printf("stk:%u", (unsigned)g_diag_stack_used);

    // Ring-buffer fill bar: width proportional to the last flush wait time
    // (longer wait = more slack ahead of the decoder). Clamped to 2000us.
    constexpr int BAR_X = 62, BAR_Y = HEADER_ROW2_Y + 1, BAR_W = 60, BAR_H = 6;
    d.drawRect(BAR_X, BAR_Y, BAR_W, BAR_H, COL_HEADER_TXT);
    d.fillRect(BAR_X + 1, BAR_Y + 1, BAR_W - 2, BAR_H - 2, BLACK);
    uint32_t clamped = g_diag_wait_us > 2000 ? 2000 : g_diag_wait_us;
    int fill = (int)((BAR_W - 2) * clamped / 2000);
    if (fill > 0) d.fillRect(BAR_X + 1, BAR_Y + 1, fill, BAR_H - 2, 0x07E0);

    d.setCursor(BAR_X + BAR_W + 4, HEADER_ROW2_Y);
    d.printf("u:%u", (unsigned)g_diag_underruns);

    // Battery voltage centred under the battery icon.
    constexpr int volt_w = 5 * CHAR_W;  // "N.NNv"
    constexpr int volt_x = BATTERY_ICON_X + BATTERY_ICON_W / 2 - volt_w / 2;
    d.setCursor(volt_x, HEADER_ROW2_Y);
    d.printf("%d.%02dv",
             g_battery_voltage_mv / 1000,
             (g_battery_voltage_mv % 1000) / 10);
}

static void drawHeader() {
    auto& d = M5Cardputer.Display;
    d.startWrite();
    d.fillRect(0, 0, SCREEN_W, HEADER_H, BLACK);

    d.setTextSize(1);
    d.setTextColor(COL_HEADER_TXT, BLACK);
    d.setCursor(0, 1);
    d.print(APP_VERSION);

    constexpr int ICON_W = BATTERY_ICON_W, ICON_H = BATTERY_ICON_H;
    int x = BATTERY_ICON_X;
    int y = BATTERY_ICON_Y;
    d.fillRect(x + ICON_W, y + 2, 2, ICON_H - 4, COL_HEADER_TXT);
    d.drawRect(x, y, ICON_W, ICON_H, COL_HEADER_TXT);
    if (g_battery_level > 0) {
        int fw = ((ICON_W - 2) * g_battery_level) / 100;
        if (fw < 1) fw = 1;
        d.fillRect(x + 1, y + 1, fw, ICON_H - 2, batteryColour(g_battery_level));
    }

    drawDiagnosticsRow();
    d.endWrite();
}

static void pollDiagnostics() {
    uint32_t now = millis();
    if (now - g_diag_last_ms < DIAGNOSTICS_POLL_MS) return;
    g_diag_last_ms = now;

    if (g_audio_task) {
        UBaseType_t hw = uxTaskGetStackHighWaterMark(g_audio_task);
        uint32_t free_bytes = (uint32_t)hw * sizeof(StackType_t);
        g_diag_stack_used = (AUDIO_TASK_STACK > free_bytes)
            ? (AUDIO_TASK_STACK - free_bytes) : 0;
    }
    if (g_out) {
        g_diag_underruns = g_out->underruns();
        g_diag_wait_us   = g_out->lastWaitMicros();
    }
    M5Cardputer.Display.startWrite();
    drawDiagnosticsRow();
    M5Cardputer.Display.endWrite();
}

static int displayedBatteryLevel(int mv) {
    if (mv <= 0) return -1;
    int level = (mv - LOADED_EMPTY_MV) * 100 / (LOADED_FULL_MV - LOADED_EMPTY_MV);
    if (level < 0)   level = 0;
    if (level > 100) level = 100;
    return level;
}

static void pollBattery(bool force = false) {
    uint32_t now = millis();
    if (!force && (now - g_battery_last_ms) < BATTERY_POLL_MS) return;
    g_battery_last_ms = now;

    int16_t mv = M5.Power.getBatteryVoltage();
    g_battery_voltage_mv = (mv > 0) ? mv : 0;

    // Below the warning band: the cell has dropped past the empty cutoff
    // during a previous off-then-on cycle without charging. Skip the warning
    // and sleep immediately — further on-time would only stress the cell.
    if (mv > 0 && mv <= CRITICAL_EMPTY_MV) {
        M5.Power.powerOff();
    }

    int level = displayedBatteryLevel(mv);

    if (level != g_battery_level) {
        g_battery_level = level;
        drawHeader();
    }

    if (level == 0) {
        enterBatteryLowState();
    }
}

static void drawFooter() {
    auto& d = M5Cardputer.Display;
    d.startWrite();
    d.fillRect(0, FOOTER_Y, SCREEN_W, FOOTER_H, BLACK);

    d.setTextSize(1);
    d.setCursor(0, FOOTER_Y + 1);
    d.setTextColor(COL_FOOTER_TXT, BLACK);

    const char* marker = " ";
    std::string track = "stopped";
    if (!g_play_path.empty()) {
        marker = g_paused ? "=" : ">";
        track  = basename(g_play_path);
    }

    std::string vol = " vol:" + std::to_string(g_volume);
    int vol_chars = (int)vol.size();
    int track_budget = charsPerLine(SCREEN_W) - vol_chars - 2;
    if ((int)track.size() > track_budget && track_budget > 0) {
        track = track.substr(0, track_budget);
    }

    d.printf("%s%s", marker, track.c_str());
    d.setCursor(SCREEN_W - vol_chars * CHAR_W, FOOTER_Y + 1);
    d.setTextColor(COL_FOOTER_DIM, BLACK);
    d.print(vol.c_str());

    // progress bar
    int bar_y = FOOTER_Y + 11;
    d.fillRect(0, bar_y, SCREEN_W, 4, BLACK);
    if (g_src && !g_play_path.empty()) {
        uint32_t pos = g_src->getPos();
        uint32_t sz  = g_src->getSize();
        if (sz > 0) {
            int w = (int)((uint64_t)pos * SCREEN_W / sz);
            if (w < 0) w = 0;
            if (w > SCREEN_W) w = SCREEN_W;
            d.drawRect(0, bar_y, SCREEN_W, 4, 0x4208);
            d.fillRect(0, bar_y, w, 4, WHITE);
        }
    }
    d.endWrite();
}

static void drawEntry(int x, int col_w, int y, const Entry& e,
                      bool selected, bool preview, uint16_t zebra_bg) {
    auto& d = M5Cardputer.Display;
    int cpl   = charsPerLine(col_w);
    int rows  = entryRows(e, col_w);
    int h     = rows * ROW_H;

    Brightness b = selected ? BRIGHT : preview ? DIM : NORMAL;
    uint16_t fg = kindColour(e.kind, b);

    d.fillRect(x, y, col_w, h, zebra_bg);
    d.setTextColor(fg, zebra_bg);

    const char* s = e.name.c_str();
    int n = (int)e.name.size();
    for (int row = 0; row < rows; ++row) {
        int off = row * cpl;
        int chunk = std::min(cpl, n - off);
        d.setCursor(x + 1, y + row * ROW_H + 1);
        for (int i = 0; i < chunk; ++i) d.print(s[off + i]);
    }

    if (selected) {
        d.drawRect(x, y, col_w, h, COL_SEL_BORDER);
    }
}

static void drawColumn(int x, int col_w, const std::vector<Entry>& items,
                       int cursor, int top, bool is_active, bool dim) {
    auto& d = M5Cardputer.Display;
    d.fillRect(x, BROWSER_Y, col_w, BROWSER_H, BLACK);

    int y = BROWSER_Y;
    int y_bottom = BROWSER_Y + BROWSER_H;
    for (int i = top; i < (int)items.size() && y < y_bottom; ++i) {
        int rows = entryRows(items[i], col_w);
        int h = rows * ROW_H;
        if (y + h > y_bottom) h = y_bottom - y;  // clip last entry vertically
        uint16_t zebra = (i & 1) ? COL_ZEBRA_B : COL_ZEBRA_A;
        bool selected = is_active && (i == cursor);
        drawEntry(x, col_w, y, items[i], selected, dim, zebra);
        y += rows * ROW_H;
    }
}

static void refreshPreview() {
    if (g_cursor < 0 || g_cursor >= (int)g_entries.size()) {
        g_preview.clear();
        g_preview_for_cursor = -2;
        return;
    }
    if (g_preview_for_cursor == g_cursor) return;
    g_preview_for_cursor = g_cursor;
    const Entry& e = g_entries[g_cursor];
    if (e.kind == KIND_DIR) {
        scanDir(joinPath(g_cur_path, e.name), g_preview);
    } else {
        g_preview.clear();
    }
}

static void ensureCursorVisible() {
    auto& d = M5Cardputer.Display;
    if (g_cursor < g_top) { g_top = g_cursor; return; }
    while (true) {
        int y = BROWSER_Y;
        int y_bottom = BROWSER_Y + BROWSER_H;
        bool fits = false;
        for (int i = g_top; i <= g_cursor; ++i) {
            int h = entryRows(g_entries[i], COL_CUR_W) * ROW_H;
            if (i == g_cursor) { fits = (y + h) <= y_bottom; break; }
            y += h;
        }
        if (fits || g_top >= g_cursor) return;
        ++g_top;
    }
}

static void drawBrowser() {
    auto& d = M5Cardputer.Display;
    d.startWrite();
    if (g_entries.empty()) {
        d.fillRect(0, BROWSER_Y, SCREEN_W, BROWSER_H, BLACK);
        d.setTextColor(COL_FILE_BRIGHT, BLACK);
        d.setCursor(2, BROWSER_Y + 2);
        d.print("(empty)");
    } else {
        refreshPreview();
        ensureCursorVisible();
        drawColumn(0, COL_CUR_W, g_entries, g_cursor, g_top, true, false);
        drawColumn(COL_PREV_X, COL_PREV_W, g_preview, -1, 0, false, true);
    }
    d.endWrite();
}

static void draw() {
    auto& d = M5Cardputer.Display;
    d.startWrite();
    d.fillScreen(BLACK);
    d.endWrite();
    drawHeader();
    drawBrowser();
    drawFooter();
}

static void loadDir(const std::string& path) {
    g_cur_path = path;
    scanDir(path, g_entries);
    g_cursor = 0;
    g_top = 0;
    g_preview_for_cursor = -1;
}

static void moveCursor(int delta) {
    if (g_entries.empty()) return;
    g_cursor += delta;
    if (g_cursor < 0) g_cursor = 0;
    if (g_cursor >= (int)g_entries.size()) g_cursor = (int)g_entries.size() - 1;
    drawBrowser();
}

// Returns the offset of the first audio byte if the file starts with
// a valid ID3v2 tag; returns 0 if absent or malformed.
static uint32_t id3v2HeaderSize(AudioFileSource* src) {
    uint8_t hdr[10];
    if (src->read(hdr, sizeof(hdr)) != sizeof(hdr)) return 0;
    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3') return 0;
    if (hdr[3] < 2 || hdr[3] > 4) return 0;
    if ((hdr[6] | hdr[7] | hdr[8] | hdr[9]) & 0x80) return 0;
    uint32_t size = ((uint32_t)hdr[6] << 21)
                  | ((uint32_t)hdr[7] << 14)
                  | ((uint32_t)hdr[8] << 7)
                  | (uint32_t)hdr[9];
    return 10 + size;
}

// MP3 files often prefix audio data with a large ID3v2 tag (embedded
// album art). The decoder otherwise byte-scans through it looking for
// the first audio frame, which costs seconds at SD-read speeds. Seek
// past the tag if valid; otherwise rewind to the start.
static void skipID3v2(AudioFileSource* src) {
    uint32_t skip = id3v2HeaderSize(src);
    bool ok = (skip > 0 && skip < src->getSize());
    src->seek(ok ? skip : 0, SEEK_SET);
}

static bool startPlayback(const std::string& full_path) {
    stopPlayback();

    AudioGenerator* gen = makeGenerator(full_path);
    if (!gen) {
        Serial.printf("unsupported: %s\n", full_path.c_str());
        return false;
    }

    auto* src = new AudioFileSourceSD(full_path.c_str());
    if (endsWith(full_path, ".mp3")) {
        skipID3v2(src);
    }
    uint32_t audio_start_offset = src->getPos();
    if (!gen->begin(src, g_out)) {
        Serial.printf("decoder begin failed: %s\n", full_path.c_str());
        delete gen;
        delete src;
        return false;
    }

    g_out->resetFormatLog();

    std::string play_dir = parentPath(full_path);
    std::vector<Entry> play_entries;
    scanDir(play_dir, play_entries);
    std::string name = basename(full_path);
    int play_idx = -1;
    for (int i = 0; i < (int)play_entries.size(); ++i) {
        if (play_entries[i].name == name) { play_idx = i; break; }
    }

    if (g_audio_mutex) xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
    g_gen = gen;
    g_src = src;
    g_audio_start_offset = audio_start_offset;
    g_play_path = full_path;
    g_play_dir  = play_dir;
    g_play_entries = std::move(play_entries);
    g_play_idx = play_idx;
    g_paused = false;
    if (g_audio_mutex) xSemaphoreGive(g_audio_mutex);

    Serial.printf("playing: %s\n", full_path.c_str());
    return true;
}

static void activateSelection() {
    if (g_entries.empty()) return;
    const Entry& e = g_entries[g_cursor];
    if (e.kind == KIND_DIR) {
        loadDir(joinPath(g_cur_path, e.name));
        draw();
    } else if (e.kind == KIND_AUDIO) {
        std::string full = joinPath(g_cur_path, e.name);
        if (g_play_path == full && g_gen && g_gen->isRunning()) {
            stopPlayback();
        } else {
            startPlayback(full);
        }
        drawFooter();
    }
}

static void descend() {
    if (g_entries.empty()) return;
    const Entry& e = g_entries[g_cursor];
    if (e.kind == KIND_DIR) {
        loadDir(joinPath(g_cur_path, e.name));
        draw();
    }
}

static void ascend() {
    if (g_cur_path == "/") return;
    std::string prev_name = basename(g_cur_path);
    loadDir(parentPath(g_cur_path));
    for (int i = 0; i < (int)g_entries.size(); ++i) {
        if (g_entries[i].name == prev_name) { g_cursor = i; break; }
    }
    draw();
}

static void togglePause() {
    if (g_play_path.empty()) return;
    g_paused = !g_paused;
    drawFooter();
}

static void skipTrack(int delta) {
    if (g_play_entries.empty() || g_play_idx < 0) return;
    int target = g_play_idx + delta;
    while (target >= 0 && target < (int)g_play_entries.size() &&
           g_play_entries[target].kind != KIND_AUDIO) {
        target += delta;
    }
    if (target < 0 || target >= (int)g_play_entries.size()) {
        stopPlayback();
        drawFooter();
        return;
    }
    std::string full = joinPath(g_play_dir, g_play_entries[target].name);
    startPlayback(full);
    drawFooter();
}

static void seekToByte(uint32_t target) {
    if (!g_audio_mutex) return;
    xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
    if (g_src) g_src->seek(target, SEEK_SET);
    xSemaphoreGive(g_audio_mutex);
}

// Audio range = file size minus the leading metadata (e.g. ID3v2 tag)
// captured at file-open time. Percentage seeks operate on this range so
// that "0%" lands at the first audio byte rather than the file's byte 0,
// which avoids a re-scan through any tag.
static uint32_t audioRangeBytes() {
    return g_src ? (g_src->getSize() - g_audio_start_offset) : 0;
}

static void jumpToTenth(int digit_index) {
    if (!g_src) return;
    uint64_t range = audioRangeBytes();
    seekToByte(g_audio_start_offset + (uint32_t)(range * digit_index / 10));
}

static void pollSeekKeys() {
    auto state = M5Cardputer.Keyboard.keysState();
    int direction = 0;
    for (char c : state.word) {
        if (c == '[') direction = -1;
        else if (c == ']') direction = +1;
    }
    if (direction == 0) return;
    uint32_t now = millis();
    if (now - g_last_seek_ms < 100) return;
    if (!g_src) return;
    int64_t range = (int64_t)audioRangeBytes();
    int64_t audio_start = (int64_t)g_audio_start_offset;
    int64_t pos = (int64_t)g_src->getPos();
    int64_t target = pos + direction * (range / 200);
    if (target < audio_start) target = audio_start;
    if (target > audio_start + range) target = audio_start + range;
    seekToByte((uint32_t)target);
    g_last_seek_ms = now;
    drawFooter();
}

static void changeVolume(int delta) {
    g_volume += delta;
    if (g_volume < 0) g_volume = 0;
    if (g_volume > MAX_VOL) g_volume = MAX_VOL;
    applyVolume();
    drawFooter();
}

static void enterBatteryLowState() {
    stopPlayback();

    auto& d = M5Cardputer.Display;
    d.fillScreen(BLACK);
    d.setTextSize(2);
    constexpr int CELL_W = 12, CELL_H = 16;  // size-2 character cell

    auto drawLine = [&](const char* s, int y, uint16_t fg) {
        d.setTextColor(fg, BLACK);
        int w = (int)strlen(s) * CELL_W;
        d.setCursor((SCREEN_W - w) / 2, y);
        d.print(s);
    };

    int y = 8;
    drawLine("Battery empty.",     y, 0xF800); y += CELL_H + 4;
    drawLine("Powering off.",      y, COL_HEADER_TXT); y += CELL_H + 8;
    drawLine("To charge:",         y, COL_HEADER_TXT); y += CELL_H + 2;
    drawLine("plug in USB,",       y, COL_HEADER_TXT); y += CELL_H + 2;
    drawLine("switch power ON.",   y, COL_HEADER_TXT);

    delay(BATTERY_LOW_TIMEOUT_MS);
    M5.Power.powerOff();
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);

    Serial.begin(115200);
    Serial.println("cardplayer: boot ok");

    M5Cardputer.Speaker.begin();
    applyVolume();

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, SPI, 25000000) || SD.cardType() == CARD_NONE) {
        M5Cardputer.Display.fillScreen(BLACK);
        M5Cardputer.Display.setCursor(0, 0);
        M5Cardputer.Display.println("SD init failed");
        Serial.println("SD init failed");
        return;
    }

    loadDir("/");
    g_out = new AudioOutputM5CardputerSpeaker(&M5Cardputer.Speaker);
    g_audio_mutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(audioTask, "audio", AUDIO_TASK_STACK / sizeof(StackType_t),
                            nullptr, AUDIO_TASK_PRIO, &g_audio_task, AUDIO_TASK_CORE);
    pollBattery(true);
    draw();
}

void loop() {
    M5Cardputer.update();
    pollBattery();
    pollDiagnostics();

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        auto state = M5Cardputer.Keyboard.keysState();

        for (char c : state.word) {
            switch (c) {
                case ';': moveCursor(-1); break;
                case '.': moveCursor(+1); break;
                case ',': ascend(); break;
                case '/': descend(); break;
                case '<': skipTrack(-1); break;
                case '?': skipTrack(+1); break;
                case ' ': togglePause(); break;
                case '=': case '+': changeVolume(+1); break;
                case '-': case '_': changeVolume(-1); break;
                case '1': case '2': case '3': case '4': case '5':
                case '6': case '7': case '8': case '9': case '0': {
                    int digit_index = (c == '0') ? 9 : (c - '1');
                    jumpToTenth(digit_index);
                    break;
                }
                default: break;
            }
        }
        if (state.enter) activateSelection();
    }

    pollSeekKeys();

    if (g_advance_pending) {
        g_advance_pending = false;
        skipTrack(+1);
    }

    bool playing = false;
    if (g_audio_mutex) {
        xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
        playing = (g_gen && g_gen->isRunning() && !g_paused);
        xSemaphoreGive(g_audio_mutex);
    }
    if (playing && millis() - g_last_progress_ms > 500) {
        g_last_progress_ms = millis();
        drawFooter();
    }
    delay(10);
}
