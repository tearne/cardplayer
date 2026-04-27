#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <string>
#include <algorithm>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_freertos_hooks.h>
#include <esp_timer.h>

#include <AudioFileSourceSD.h>
#include <AudioGenerator.h>
#include <AudioGeneratorWAV.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorFLAC.h>
#include <AudioGeneratorAAC.h>
#include "AudioFileSourceM4A.h"

#include "audio_output_m5.h"

#define SD_SCK  40
#define SD_MISO 39
#define SD_MOSI 14
#define SD_CS   12

static constexpr const char* APP_VERSION = "0.12.0";

static constexpr int SCREEN_W     = 240;
static constexpr int SCREEN_H     = 135;

static constexpr int HEADER_MIN_H      = 9;
static constexpr int HEADER_ROW2_Y     = 10;
static constexpr int HEADER_ROW2_H     = 16;
static constexpr int HEADER_FULL_H     = HEADER_ROW2_Y + HEADER_ROW2_H;  // 26

// Diagnostics row layout: three regions across the screen.
// Cells 1 and 2 hold two stacked numeric readouts each; cell 3 carries
// per-core CPU readouts and sparklines using the full row height.
//
//  | stk:NNN% | ram:NNN% | cpu 0:NNN%  ▁▂▃▂▁_▁▂▃ |
//  | buf:NNN% | u:NN     | cpu 1:NNN%  _▁_▂_▁▁▁_ |
//
static constexpr int DIAG_NUM_CELL_W  = SCREEN_W / 4;                       // 60
static constexpr int DIAG_CPU_CELL_X  = DIAG_NUM_CELL_W * 2;                // 120
static constexpr int DIAG_TOP_Y       = HEADER_ROW2_Y;                      // 10
static constexpr int DIAG_BOT_Y       = HEADER_ROW2_Y + 8;                  // 18
static constexpr int DIAG_CPU_LABEL_W = 48;                                 // 8 chars × 6 px (worst case "cpu:100%") + gutter
static constexpr int DIAG_CPU_GRAPH_X = DIAG_CPU_CELL_X + DIAG_CPU_LABEL_W; // 168
static constexpr int DIAG_CPU_GRAPH_W = SCREEN_W - DIAG_CPU_GRAPH_X - 2;    // 70
static constexpr int DIAG_CPU_GRAPH_H = 7;

// Footer slots — left to right within the footer band (see footerH).
// Play/pause state is conveyed by the progress bar's colour (slate-blue while
// playing, grey while paused or stopped) rather than a dedicated indicator.
static constexpr int FOOTER_NAME_X = 1;
static constexpr int FOOTER_NAME_W = 106;
static constexpr int FOOTER_PROG_X = FOOTER_NAME_X + FOOTER_NAME_W + 2;  // 109
static constexpr int FOOTER_PROG_W = 96;
static constexpr int FOOTER_VOL_X  = FOOTER_PROG_X + FOOTER_PROG_W + 2;  // 207
static constexpr int FOOTER_VOL_W  = SCREEN_W - FOOTER_VOL_X - 1;        // 32
static constexpr int FOOTER_BAR_H  = 6;

static constexpr uint32_t AUDIO_TASK_STACK    = 8 * 1024;
static constexpr UBaseType_t AUDIO_TASK_PRIO  = 3;
static constexpr BaseType_t AUDIO_TASK_CORE   = 1;
static constexpr uint32_t DIAGNOSTICS_POLL_MS = 250;

static constexpr int COL_CUR_W    = (SCREEN_W * 4) / 5;       // 192
static constexpr int COL_PREV_X   = COL_CUR_W;
static constexpr int COL_PREV_W   = SCREEN_W - COL_CUR_W;     // 48

// Active column reserves a narrow gutter on its right edge for the scrollbar.
// Content (names, wrap, clip) uses COL_CUR_CONTENT_W; the gutter starts at
// COL_CUR_W - SCROLLBAR_W and ends at the column divider.
static constexpr int SCROLLBAR_W       = 3;
static constexpr int COL_CUR_CONTENT_W = COL_CUR_W - SCROLLBAR_W;
static constexpr int SCROLLBAR_X       = COL_CUR_W - SCROLLBAR_W;
static constexpr int SCROLLBAR_MIN_H   = 6;

static constexpr int BASE_ROW_H   = 9;
static constexpr int BASE_CHAR_W  = 6;
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
static constexpr bool BATTERY_DEV_MODE = false;
static constexpr int LOADED_EMPTY_MV   = BATTERY_DEV_MODE ? 3700 : 3400;
static constexpr int CRITICAL_EMPTY_MV = BATTERY_DEV_MODE ? 3600 : 3300;
static constexpr int LOADED_FULL_MV    = 3830;

// Colours (RGB565). Each kind has three brightness tiers:
// BRIGHT (selected), NORMAL (non-selected active column), DIM (preview column).
static constexpr uint16_t COL_DIR_BRIGHT    = 0x5D9F;
static constexpr uint16_t COL_DIR_NORMAL    = 0x3B7D;
static constexpr uint16_t COL_DIR_DIM       = 0x2CD3;
static constexpr uint16_t COL_FILE_BRIGHT   = 0xFFFF;
static constexpr uint16_t COL_FILE_NORMAL   = 0x6979;
static constexpr uint16_t COL_FILE_DIM      = 0x41D2;
static constexpr uint16_t COL_OTHER_BRIGHT  = 0x528A;
static constexpr uint16_t COL_OTHER_NORMAL  = 0x4208;
static constexpr uint16_t COL_OTHER_DIM     = 0x2104;
static constexpr uint16_t COL_HAIRLINE   = 0x4208;  // mid grey for column dividers
static constexpr uint16_t COL_HEADER_TXT = 0x7BEF;
static constexpr uint16_t COL_FOOTER_TXT   = 0xFFFF;  // track name
static constexpr uint16_t COL_FOOTER_PROG  = 0x6979;  // progress bar while playing (slate-blue)
static constexpr uint16_t COL_FOOTER_IDLE  = 0x7BEF;  // progress bar while paused or stopped (mid-grey)
static constexpr uint16_t COL_FOOTER_VOL   = 0x34D0;  // volume bar (muted teal)
static constexpr uint16_t COL_FOOTER_FRAME = 0x6979;  // hairline above footer

// Off-screen back buffer. All draw operations target g_canvas; once a frame
// is composed, presentFrame() pushes it to the panel in one operation, so
// the panel transitions directly from old content to new — no fillRect-then-
// redraw flicker.
static M5Canvas g_canvas(&M5Cardputer.Display);
static inline void presentFrame() { g_canvas.pushSprite(0, 0); }

static int  g_font_notch     = 1;
static bool g_diagnostics_hidden = false;
static bool g_show_help      = false;
static bool g_wrap_names     = true;

struct FontNotch {
    const lgfx::IFont* font;
    int text_size;
    int char_w;
    int row_h;
};
static const FontNotch FONT_NOTCHES[] = {
    { &fonts::Font0, 1,  6, 10 },  // 6x8 scaled x1
    { &fonts::Font0, 2, 12, 18 },  // 6x8 scaled x2
};
static constexpr int FONT_NOTCH_COUNT = sizeof(FONT_NOTCHES) / sizeof(FONT_NOTCHES[0]);

static inline const lgfx::IFont* notchFont() { return FONT_NOTCHES[g_font_notch].font; }
static inline int notchSize() { return FONT_NOTCHES[g_font_notch].text_size; }
static inline int rowH()      { return FONT_NOTCHES[g_font_notch].row_h; }
static inline int charW()     { return FONT_NOTCHES[g_font_notch].char_w; }
static inline int headerH()  { return g_diagnostics_hidden ? HEADER_MIN_H : HEADER_FULL_H; }
static inline int footerH()  { return 10; }
static inline int browserY() { return headerH(); }
static inline int browserH() { return SCREEN_H - headerH() - footerH(); }
static inline int footerY()  { return headerH() + browserH(); }

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

// Track-name marquee state. Persists across redraws so unrelated repaints
// don't reset the scroll. Reset (in pollMarquee) when g_play_path changes.
struct Marquee {
    enum Phase { PRE_PAUSE, SCROLLING, POST_PAUSE };
    std::string track_path;
    int         offset_px      = 0;
    int         max_offset_px  = 0;
    Phase       phase          = PRE_PAUSE;
    uint32_t    phase_start_ms = 0;
    uint32_t    last_tick_ms   = 0;
};
static Marquee g_marquee;
static constexpr uint32_t MARQUEE_TICK_MS  = 50;    // ~20 Hz
static constexpr uint32_t MARQUEE_PAUSE_MS = 1000;  // start + end pauses

static uint32_t g_last_seek_ms = 0;
static uint32_t g_last_nav_ms  = 0;
// Time the current up/down hold started; 0 when no key is held. Used to gate
// the initial repeat behind a long delay so a normal tap (~150 ms dwell)
// doesn't double-fire.
static uint32_t g_nav_press_ms = 0;
static uint32_t g_audio_start_offset = 0;

static AudioFileSource*               g_src = nullptr;
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

// One sample per pixel of graph width; new samples land on the right and
// the buffer slides left once full. Stored as 0..255 so each bar's height
// and threshold colour come straight out of one byte. Only the CPU cell
// has a sparkline; stk / buf / ram are shown as raw numbers since they
// move slowly and a graph adds little.
struct Sparkline {
    uint8_t  samples[DIAG_CPU_GRAPH_W] = {0};
    int      count                     = 0;
};

static Sparkline g_spark_cpu0;
static Sparkline g_spark_cpu1;

// Latest sampled values, displayed as numerics each tick.
static int g_diag_stk_pct = 0;
static int g_diag_buf_pct = 0;
static int g_diag_ram_pct = 0;
static int g_diag_cpu0_pct = 0;
static int g_diag_cpu1_pct = 0;

// CPU sampling: a FreeRTOS idle hook on each core ticks a counter every
// time the idle task gets a turn. Per-second delta is the call rate, which
// inversely correlates with load. We normalise against the highest rate
// observed so far — that's the "fully-idle" reference. Load = 1 − (rate /
// max_rate).
//
// Why this and not a direct time-based measurement? The idle task on this
// build uses WAITI to actually sleep the CPU between FreeRTOS ticks (1 kHz),
// so the hook fires at ~1 ms intervals when idle. Gaps between consecutive
// hook calls don't distinguish "1 ms tick wakeup" (idle) from "1 ms
// preemption" (busy). The rate-counter approach sidesteps that.
//
// Cost: a brief calibration period — the first sample shows 0 % load on
// each core until that core has seen one mostly-idle second to set its
// max-rate baseline.
static volatile uint32_t g_idle_calls_0 = 0;
static volatile uint32_t g_idle_calls_1 = 0;
static uint32_t g_idle_calls_0_prev = 0;
static uint32_t g_idle_calls_1_prev = 0;
static uint32_t g_idle_max_rate_0   = 0;
static uint32_t g_idle_max_rate_1   = 0;

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
    if (endsWith(name, ".m4a") ||
        endsWith(name, ".mp4")) return new AudioGeneratorAAC();
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
    int cw = charW();
    if (avail < cw) return 1;
    return avail / cw;
}

static int entryRows(const Entry& e, int col_w, bool wrap) {
    if (!wrap) return 1;
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
    AudioFileSource* src_to_free = nullptr;
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

static void sparklinePush(Sparkline& s, float frac) {
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    uint8_t v = (uint8_t)(frac * 255.0f + 0.5f);
    if (s.count < DIAG_CPU_GRAPH_W) {
        s.samples[s.count++] = v;
    } else {
        memmove(s.samples, s.samples + 1, DIAG_CPU_GRAPH_W - 1);
        s.samples[DIAG_CPU_GRAPH_W - 1] = v;
    }
}

static uint16_t sparklineColour(uint8_t v) {
    if (v < 153) return COL_HEADER_TXT;  // <60% — calm
    if (v < 217) return 0xFE60;          // 60–85% — yellow
    return 0xF800;                       // >85% — red
}

// Both cores' load shares a single graph spanning the full row height,
// drawn as two overlaid lines. Identity is by colour (cyan = core 0,
// orange = core 1); magnitude is by vertical position. The row's full
// 16 px gives each line useful Y resolution.
static void drawCpuOverlay(int x, int y, int h,
                           const Sparkline& s0, const Sparkline& s1) {
    auto& d = g_canvas;
    d.fillRect(x, y, DIAG_CPU_GRAPH_W, h, BLACK);
    auto plot = [&](const Sparkline& s, uint16_t c) {
        int start = DIAG_CPU_GRAPH_W - s.count;
        int prev_dy = -1;
        for (int i = 0; i < s.count; ++i) {
            uint8_t v = s.samples[i];
            int bar = (v * (h - 1) + 127) / 255;
            int dy = y + h - 1 - bar;
            int dx = x + start + i;
            if (prev_dy >= 0) d.drawLine(dx - 1, prev_dy, dx, dy, c);
            else              d.drawPixel(dx, dy, c);
            prev_dy = dy;
        }
    };
    plot(s0, 0x07FF);  // core 0 — cyan
    plot(s1, 0xFC00);  // core 1 — orange
}

static void drawDiagnosticsRow() {
    auto& d = g_canvas;
    d.fillRect(0, HEADER_ROW2_Y, SCREEN_W, HEADER_ROW2_H, BLACK);
    d.setTextSize(1);
    d.setTextColor(COL_HEADER_TXT, BLACK);

    d.setCursor(0, DIAG_TOP_Y);
    d.printf("stk:%d%%", g_diag_stk_pct);
    d.setCursor(0, DIAG_BOT_Y);
    d.printf("buf:%d%%", g_diag_buf_pct);

    d.setCursor(DIAG_NUM_CELL_W, DIAG_TOP_Y);
    d.printf("ram:%d%%", g_diag_ram_pct);
    d.setCursor(DIAG_NUM_CELL_W, DIAG_BOT_Y);
    d.printf("u:%u", (unsigned)g_diag_underruns);

    // Top row carries the "cpu:" label and core 0's percent; bottom row
    // carries core 1's percent aligned beneath it. Numbers are tinted to
    // match their respective lines in the overlaid graph (cyan = core 0,
    // orange = core 1) so the reader knows which is which without a label.
    d.setCursor(DIAG_CPU_CELL_X, DIAG_TOP_Y);
    d.print("cpu:");
    d.setTextColor(0x07FF, BLACK);
    d.printf("%d%%", g_diag_cpu0_pct);

    d.setCursor(DIAG_CPU_CELL_X + 4 * BASE_CHAR_W, DIAG_BOT_Y);
    d.setTextColor(0xFC00, BLACK);
    d.printf("%d%%", g_diag_cpu1_pct);

    d.setTextColor(COL_HEADER_TXT, BLACK);

    drawCpuOverlay(DIAG_CPU_GRAPH_X, HEADER_ROW2_Y, HEADER_ROW2_H,
                   g_spark_cpu0, g_spark_cpu1);
}

static void drawHeader() {
    auto& d = g_canvas;
    d.fillRect(0, 0, SCREEN_W, headerH(), BLACK);

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

    // Voltage readout sits just left of the battery icon: was on row 2, but
    // row 2 is now the sparkline strip and the voltage's natural neighbour
    // is the icon anyway.
    constexpr int VOLT_W = 5 * BASE_CHAR_W;
    int vx = BATTERY_ICON_X - VOLT_W - 2;
    d.setCursor(vx, 1);
    d.printf("%d.%02dv",
             g_battery_voltage_mv / 1000,
             (g_battery_voltage_mv % 1000) / 10);

    if (!g_diagnostics_hidden) drawDiagnosticsRow();
    presentFrame();
}

static bool IRAM_ATTR idleHookCore0() { ++g_idle_calls_0; return true; }
static bool IRAM_ATTR idleHookCore1() { ++g_idle_calls_1; return true; }

// Returns the per-core idle fraction over the interval since the previous
// call, [0..1]. Each core's max rate self-calibrates to the highest delta
// seen so far; once it has seen a mostly-idle second the readings are
// stable.
static void sampleCpuIdleFractions(float& idle_0, float& idle_1) {
    uint32_t now_0 = g_idle_calls_0;
    uint32_t now_1 = g_idle_calls_1;
    uint32_t d0 = now_0 - g_idle_calls_0_prev;
    uint32_t d1 = now_1 - g_idle_calls_1_prev;
    g_idle_calls_0_prev = now_0;
    g_idle_calls_1_prev = now_1;

    if (d0 > g_idle_max_rate_0) g_idle_max_rate_0 = d0;
    if (d1 > g_idle_max_rate_1) g_idle_max_rate_1 = d1;

    idle_0 = g_idle_max_rate_0 > 0 ? (float)d0 / (float)g_idle_max_rate_0 : 0.0f;
    idle_1 = g_idle_max_rate_1 > 0 ? (float)d1 / (float)g_idle_max_rate_1 : 0.0f;
    if (idle_0 > 1.0f) idle_0 = 1.0f;
    if (idle_1 > 1.0f) idle_1 = 1.0f;
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

    g_diag_stk_pct = (int)(100.0f * (float)g_diag_stack_used
                           / (float)AUDIO_TASK_STACK + 0.5f);

    // Buffer headroom: time the audio task spent waiting for the speaker to
    // drain before submitting the next chunk. More wait = more headroom.
    // Capped at 2 ms (the original full-bar mark).
    float buf_frac = g_diag_wait_us > 2000 ? 1.0f
                                            : (float)g_diag_wait_us / 2000.0f;
    g_diag_buf_pct = (int)(100.0f * buf_frac + 0.5f);

    uint32_t heap_total = ESP.getHeapSize();
    uint32_t heap_free  = ESP.getFreeHeap();
    g_diag_ram_pct = heap_total > 0
        ? (int)(100.0f * (float)(heap_total - heap_free)
                / (float)heap_total + 0.5f) : 0;

    float idle0, idle1;
    sampleCpuIdleFractions(idle0, idle1);
    float load0 = 1.0f - idle0;
    float load1 = 1.0f - idle1;
    sparklinePush(g_spark_cpu0, load0);
    sparklinePush(g_spark_cpu1, load1);
    g_diag_cpu0_pct = (int)(100.0f * load0 + 0.5f);
    g_diag_cpu1_pct = (int)(100.0f * load1 + 0.5f);

    if (g_show_help || g_diagnostics_hidden) return;
    drawDiagnosticsRow();
    presentFrame();
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
        if (!g_show_help) drawHeader();
    }

    if (level == 0) {
        enterBatteryLowState();
    }
}

// Footer text always renders at size 1 with the default Font0, regardless of
// the browser's font notch — the footer band is fixed-height.
static inline void setFooterText(uint16_t fg) {
    auto& d = g_canvas;
    d.setFont(&fonts::Font0);
    d.setTextSize(1);
    d.setTextWrap(false, false);
    d.setTextColor(fg, BLACK);
}

static void drawSlotName() {
    auto& d = g_canvas;
    int fy = footerY();
    int fh = footerH();
    d.fillRect(FOOTER_NAME_X, fy, FOOTER_NAME_W, fh, BLACK);
    d.setClipRect(FOOTER_NAME_X, fy, FOOTER_NAME_W, fh);
    setFooterText(COL_FOOTER_TXT);
    std::string name = g_play_path.empty() ? "stopped" : basename(g_play_path);
    d.setCursor(FOOTER_NAME_X - g_marquee.offset_px, fy + (fh - 8) / 2);
    d.print(name.c_str());
    d.clearClipRect();
}

static void drawSlotProgress() {
    auto& d = g_canvas;
    int fy = footerY();
    int fh = footerH();
    int by = fy + (fh - FOOTER_BAR_H) / 2;
    bool playing = !g_play_path.empty() && !g_paused;
    uint16_t col = playing ? COL_FOOTER_PROG : COL_FOOTER_IDLE;
    d.fillRect(FOOTER_PROG_X, fy, FOOTER_PROG_W, fh, BLACK);
    d.drawRect(FOOTER_PROG_X, by, FOOTER_PROG_W, FOOTER_BAR_H, col);
    if (g_src && !g_play_path.empty()) {
        uint32_t pos = g_src->getPos();
        uint32_t sz  = g_src->getSize();
        if (sz > 0) {
            int inner = FOOTER_PROG_W - 2;
            int w = (int)((uint64_t)pos * inner / sz);
            if (w < 0) w = 0;
            if (w > inner) w = inner;
            if (w > 0) {
                d.fillRect(FOOTER_PROG_X + 1, by + 1, w, FOOTER_BAR_H - 2, col);
            }
        }
    }
}

static void drawSlotVolume() {
    auto& d = g_canvas;
    int fy = footerY();
    int fh = footerH();
    int by = fy + (fh - FOOTER_BAR_H) / 2;
    d.fillRect(FOOTER_VOL_X, fy, FOOTER_VOL_W, fh, BLACK);
    d.drawRect(FOOTER_VOL_X, by, FOOTER_VOL_W, FOOTER_BAR_H, COL_FOOTER_VOL);
    int inner = FOOTER_VOL_W - 2;
    int w = (g_volume * inner) / MAX_VOL;
    if (w > 0) {
        d.fillRect(FOOTER_VOL_X + 1, by + 1, w, FOOTER_BAR_H - 2,
                   COL_FOOTER_VOL);
    }
}

static void drawFooter() {
    auto& d = g_canvas;
    d.fillRect(0, footerY(), SCREEN_W, footerH(), BLACK);
    drawSlotName();
    drawSlotProgress();
    drawSlotVolume();
    presentFrame();
}

static void pollMarquee() {
    if (g_show_help) return;
    uint32_t now = millis();
    if (now - g_marquee.last_tick_ms < MARQUEE_TICK_MS) return;
    g_marquee.last_tick_ms = now;

    if (g_play_path != g_marquee.track_path) {
        g_marquee.track_path    = g_play_path;
        g_marquee.offset_px     = 0;
        g_marquee.phase         = Marquee::PRE_PAUSE;
        g_marquee.phase_start_ms = now;
        std::string name = g_play_path.empty() ? "stopped"
                                               : basename(g_play_path);
        int text_w = (int)name.size() * BASE_CHAR_W;
        g_marquee.max_offset_px = std::max(0, text_w - FOOTER_NAME_W);
        drawSlotName();
        presentFrame();
        return;
    }

    if (g_marquee.max_offset_px == 0) return;  // fits, no scrolling

    int prev_offset = g_marquee.offset_px;
    switch (g_marquee.phase) {
        case Marquee::PRE_PAUSE:
            if (now - g_marquee.phase_start_ms >= MARQUEE_PAUSE_MS) {
                g_marquee.phase = Marquee::SCROLLING;
                g_marquee.phase_start_ms = now;
            }
            break;
        case Marquee::SCROLLING:
            ++g_marquee.offset_px;
            if (g_marquee.offset_px >= g_marquee.max_offset_px) {
                g_marquee.offset_px = g_marquee.max_offset_px;
                g_marquee.phase = Marquee::POST_PAUSE;
                g_marquee.phase_start_ms = now;
            }
            break;
        case Marquee::POST_PAUSE:
            if (now - g_marquee.phase_start_ms >= MARQUEE_PAUSE_MS) {
                g_marquee.offset_px = 0;
                g_marquee.phase = Marquee::PRE_PAUSE;
                g_marquee.phase_start_ms = now;
            }
            break;
    }
    if (g_marquee.offset_px != prev_offset) {
        drawSlotName();
        presentFrame();
    }
}

static void drawEntry(int x, int col_w, int y, const Entry& e,
                      bool selected, bool preview, bool wrap) {
    auto& d = g_canvas;
    int cpl   = charsPerLine(col_w);
    int rows  = entryRows(e, col_w, wrap);
    int rh    = rowH();
    int h     = rows * rh;

    uint16_t bg = selected ? WHITE : BLACK;
    uint16_t fg = selected ? BLACK
                           : kindColour(e.kind, preview ? DIM : NORMAL);

    d.fillRect(x, y, col_w, h, bg);
    d.setFont(notchFont());
    d.setTextSize(notchSize());
    d.setTextColor(fg, bg);

    const char* s = e.name.c_str();
    int n = (int)e.name.size();
    if (wrap) {
        for (int row = 0; row < rows; ++row) {
            int off = row * cpl;
            int chunk = std::min(cpl, n - off);
            d.setCursor(x + 1, y + row * rh + 2);
            for (int i = 0; i < chunk; ++i) d.print(s[off + i]);
        }
    } else {
        // Single-line: rely on the column's clip rect to cut off overflow.
        // Reads as "name continues off-screen" rather than as a truncation.
        d.setCursor(x + 1, y + 2);
        d.print(s);
    }
}

// Returns the number of entries that were rendered (started before the
// column ran out of room). Callers use this for the scrollbar viewport.
static int drawColumn(int x, int col_w, const std::vector<Entry>& items,
                      int cursor, int top, bool is_active, bool dim) {
    auto& d = g_canvas;
    int by = browserY();
    int bh = browserH();
    d.fillRect(x, by, col_w, bh, BLACK);

    // Active column reserves a gutter on its right edge for the scrollbar;
    // names wrap and clip against the narrower content width so the thumb
    // never overpaints them.
    int content_w = is_active ? COL_CUR_CONTENT_W : col_w;

    int y = by;
    // Row by+bh-1 is reserved for the bottom separator (drawn by drawBrowser).
    int y_max = by + bh - 1;
    int rh = rowH();
    // Preview column (narrow, on the right) never wraps — wrapping would
    // contradict the "mostly off-screen" look. Active column wraps if the
    // user has the wrap toggle on.
    bool wrap = is_active && g_wrap_names;
    // Clip so the entry that crosses the bottom edge renders only its visible
    // portion — the cut-off look signals "more below" without leaving a gap.
    d.setClipRect(x, by, content_w, bh);
    // Disable M5GFX's auto text-wrap so a long single-line name clips at the
    // column edge instead of being pushed onto a second row by the renderer.
    d.setTextWrap(false, false);
    int visible = 0;
    for (int i = top; i < (int)items.size() && y < y_max; ++i) {
        int rows = entryRows(items[i], content_w, wrap);
        int h = rows * rh;
        bool selected = is_active && (i == cursor);
        drawEntry(x, content_w, y, items[i], selected, dim, wrap);
        // Hairline above every entry except the first visible one. Drawn
        // after drawEntry so its bg fill doesn't overwrite the line.
        if (i > top) {
            d.drawFastHLine(x, y, content_w, COL_HEADER_TXT);
        }
        y += h;
        ++visible;
    }
    d.clearClipRect();
    // Restore default font so subsequent setTextSize() calls in header /
    // footer draws aren't affected by the browser notch font.
    d.setFont(&fonts::Font0);
    return visible;
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
    if (g_cursor < g_top) { g_top = g_cursor; return; }
    int rh = rowH();
    int by = browserY();
    int bh = browserH();
    while (true) {
        int y = by;
        int y_max = by + bh - 1;  // last usable row, see drawColumn
        bool fits = false;
        for (int i = g_top; i <= g_cursor; ++i) {
            int h = entryRows(g_entries[i], COL_CUR_CONTENT_W, g_wrap_names) * rh;
            if (i == g_cursor) { fits = (y + h) <= y_max; break; }
            y += h;
        }
        if (fits || g_top >= g_cursor) return;
        ++g_top;
    }
}

static void drawScrollbar(int visible, int total) {
    if (total <= 0 || visible >= total) return;
    auto& d = g_canvas;
    int by = browserY();
    int bh = browserH();
    // Track height excludes the bottom separator row.
    int track_h = bh - 1;
    int thumb_h = (track_h * visible) / total;
    if (thumb_h < SCROLLBAR_MIN_H) thumb_h = SCROLLBAR_MIN_H;
    if (thumb_h > track_h) thumb_h = track_h;
    int thumb_top = by + (int)((int64_t)(track_h - thumb_h) * g_top / (total - visible));
    if (thumb_top < by) thumb_top = by;
    if (thumb_top + thumb_h > by + track_h) thumb_top = by + track_h - thumb_h;
    d.fillRect(SCROLLBAR_X, thumb_top, SCROLLBAR_W, thumb_h, COL_HEADER_TXT);
}

static void drawBrowser() {
    auto& d = g_canvas;
    int by = browserY();
    int bh = browserH();
    if (g_entries.empty()) {
        d.fillRect(0, by, SCREEN_W, bh, BLACK);
        d.setTextSize(1);
        d.setTextColor(COL_FILE_BRIGHT, BLACK);
        d.setCursor(2, by + 2);
        d.print("(empty)");
    } else {
        refreshPreview();
        ensureCursorVisible();
        int visible = drawColumn(0, COL_CUR_W, g_entries, g_cursor, g_top, true, false);
        drawColumn(COL_PREV_X, COL_PREV_W, g_preview, -1, 0, false, true);
        drawScrollbar(visible, (int)g_entries.size());
        d.drawFastVLine(COL_PREV_X, by, bh, COL_HEADER_TXT);
    }
    d.drawFastHLine(0, by,          SCREEN_W, COL_HEADER_TXT);
    d.drawFastHLine(0, by + bh - 1, SCREEN_W, COL_FOOTER_FRAME);
    presentFrame();
}

static void draw() {
    auto& d = g_canvas;
    d.fillScreen(BLACK);
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

    auto* sd = new AudioFileSourceSD(full_path.c_str());
    AudioFileSource* src = sd;
    if (endsWith(full_path, ".mp3")) {
        skipID3v2(sd);
    } else if (endsWith(full_path, ".m4a") || endsWith(full_path, ".mp4")) {
        auto* m4a = new AudioFileSourceM4A(sd);  // takes ownership of sd
        if (!m4a->isOpen()) {
            Serial.printf("m4a parse failed: %s\n", full_path.c_str());
            delete m4a;
            delete gen;
            return false;
        }
        src = m4a;
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
    if (g_show_help) return;
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

static void pollBrowserNavKeys() {
    if (g_show_help) return;
    auto state = M5Cardputer.Keyboard.keysState();
    int direction = 0;
    for (char c : state.word) {
        if (c == ';') direction = -1;
        else if (c == '.') direction = +1;
    }
    if (direction == 0) {
        g_nav_press_ms = 0;
        return;
    }
    uint32_t now = millis();
    if (g_nav_press_ms == 0) {
        // Fresh press — fire once, then suppress until the initial-delay
        // window has elapsed.
        moveCursor(direction);
        g_nav_press_ms = now;
        g_last_nav_ms = now;
        return;
    }
    if (now - g_nav_press_ms < 400) return;
    if (now - g_last_nav_ms < 100) return;
    moveCursor(direction);
    g_last_nav_ms = now;
}

static void changeVolume(int delta) {
    g_volume += delta;
    if (g_volume < 0) g_volume = 0;
    if (g_volume > MAX_VOL) g_volume = MAX_VOL;
    applyVolume();
    drawFooter();
}

static void changeFontNotch(int delta) {
    int n = g_font_notch + delta;
    if (n < 0) n = 0;
    if (n >= FONT_NOTCH_COUNT) n = FONT_NOTCH_COUNT - 1;
    if (n == g_font_notch) return;
    g_font_notch = n;
    draw();
}

static void toggleDiagnostics() {
    g_diagnostics_hidden = !g_diagnostics_hidden;
    draw();
}

static void toggleWrapNames() {
    g_wrap_names = !g_wrap_names;
    draw();
}

static void jumpToPlaying() {
    if (g_play_path.empty() || g_play_dir.empty()) return;
    g_cur_path = g_play_dir;
    scanDir(g_cur_path, g_entries);
    std::string name = basename(g_play_path);
    g_cursor = 0;
    for (int i = 0; i < (int)g_entries.size(); ++i) {
        if (g_entries[i].name == name) { g_cursor = i; break; }
    }
    g_top = 0;
    g_preview_for_cursor = -1;
    draw();
}

static void drawHelp() {
    auto& d = g_canvas;
    d.fillScreen(BLACK);
    d.setTextSize(1);
    d.setTextColor(COL_FILE_BRIGHT, BLACK);

    static const char* lines[] = {
        "Browse",
        " ; .    up / down",
        " , /    parent / enter",
        " '      jump to playing",
        "Playback",
        " Enter  play / stop",
        " Space  pause",
        " Fn+,   prev track",
        " Fn+/   next track",
        " [ ]    seek (hold)",
        " 1..0   jump to tenth",
        " = -    volume",
        " + _    font size",
        " ` \\ ?  diags  wrap help",
    };
    int n = (int)(sizeof(lines) / sizeof(lines[0]));
    int y = 1;
    for (int i = 0; i < n; ++i) {
        d.setCursor(2, y);
        d.print(lines[i]);
        y += 9;
    }
    presentFrame();
}

static void showHelp() {
    g_show_help = true;
    drawHelp();
}

static void enterBatteryLowState() {
    stopPlayback();

    auto& d = g_canvas;
    d.fillScreen(BLACK);
    d.setTextSize(2);
    constexpr int CELL_W = 12, CELL_H = 16;  // size-2 character cell

    auto drawLine = [&](const char* s, int y, uint16_t fg) {
        d.setTextColor(fg, BLACK);
        int w = (int)strlen(s) * CELL_W;
        d.setCursor((SCREEN_W - w) / 2, y);
        d.print(s);
    };

    int y = 24;
    drawLine("Battery Empty",   y, 0xF800);          y += CELL_H + 16;
    drawLine("Charge with",     y, COL_HEADER_TXT);  y += CELL_H + 2;
    drawLine("power switch ON", y, COL_HEADER_TXT);
    presentFrame();

    delay(BATTERY_LOW_TIMEOUT_MS);
    M5.Power.powerOff();
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);

    // Create the back-buffer sprite once the display knows its rotation
    // and resolution. All subsequent draws target this sprite.
    g_canvas.createSprite(SCREEN_W, SCREEN_H);

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

    // Idle hooks per core tick a counter on every idle-task turn; the
    // diagnostics poll samples each delta and normalises against the
    // highest delta seen so far.
    esp_register_freertos_idle_hook_for_cpu(idleHookCore0, 0);
    esp_register_freertos_idle_hook_for_cpu(idleHookCore1, 1);

    pollBattery(true);
    draw();
}

void loop() {
    M5Cardputer.update();
    pollBattery();
    pollDiagnostics();
    pollMarquee();

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        auto state = M5Cardputer.Keyboard.keysState();

        if (g_show_help) {
            // Dismiss only on an actual character keypress. Releasing `?`
            // while Shift is still held also fires isChange(), but state.word
            // is empty in that case — without this guard, help would flash
            // away the moment the user lets go of Shift+/.
            if (!state.word.empty() || state.enter) {
                g_show_help = false;
                draw();
            }
        } else {
            for (char c : state.word) {
                switch (c) {
                    case ',': if (state.fn) skipTrack(-1); else ascend();   break;
                    case '/': if (state.fn) skipTrack(+1); else descend();  break;
                    case ' ': togglePause(); break;
                    case '=': changeVolume(+1); break;
                    case '-': changeVolume(-1); break;
                    case '+': changeFontNotch(+1); break;
                    case '_': changeFontNotch(-1); break;
                    case '`':  toggleDiagnostics(); break;
                    case '\\': toggleWrapNames(); break;
                    case '?':  showHelp(); break;
                    case '\'': jumpToPlaying(); break;
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
    }

    pollSeekKeys();
    pollBrowserNavKeys();

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
        if (!g_show_help) {
            drawSlotProgress();
            presentFrame();
        }
    }
    delay(10);
}
