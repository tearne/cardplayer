#include <M5Cardputer.h>
#include <Unit_RTC.h>
#include <SPI.h>
#include <SD.h>
#include <vector>
#include <string>
#include <algorithm>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_timer.h>
#include <Preferences.h>

#include <AudioFileSourceSD.h>
#include <AudioGenerator.h>
#include <AudioGeneratorWAV.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorFLAC.h>
#include <AudioGeneratorAAC.h>
#include <AudioLogger.h>
#include "AudioFileSourceM4A.h"

#include "audio_output_m5.h"
#include "fuzzy_harness.h"
#include "fuzzy_index.h"
#include "chess.h"

#define SD_SCK  40
#define SD_MISO 39
#define SD_MOSI 14
#define SD_CS   12

static constexpr const char* APP_VERSION = "0.26.1";

static constexpr int SCREEN_W     = 240;
static constexpr int SCREEN_H     = 135;

static constexpr int HEADER_MIN_H      = 9;
static constexpr int HEADER_ROW2_Y     = 10;
static constexpr int HEADER_ROW2_H     = 32;
static constexpr int HEADER_FULL_H     = HEADER_ROW2_Y + HEADER_ROW2_H;  // 42

// Diagnostics layout: two columns of numerics on the left over the full
// row-2 height; CPU sparkline on the right uses the full HEADER height
// (rows 1 and 2 combined) for maximum y-axis resolution. The sparkline
// draws over the path/version slot under high CPU — graph wins where they
// overlap. See drawHeader for render-order discipline.
//
//  y=0  | battery / volt | path/version (overdrawn by sparkline) |
//  y=10 | stk:NN% c0:NN% | ░░░░░░░░░░░░░░░░░░░░░░░ |
//  y=18 | buf:NN% c1:NN% | ░ CPU graph             |
//  y=26 | ram:NN% L:NN%  | ░ 148 × 42 px            |
//  y=34 | u:NN    M:NN%  | ░░░░░░░░░░░░░░░░░░░░░░░ |
//
static constexpr int DIAG_NUM_COL1_X  = 0;                                  // wider cell, fits stk:NN%
static constexpr int DIAG_NUM_COL2_X  = 50;                                 // narrower cell, fits c0:NN%
static constexpr int DIAG_NUM_W       = 90;                                 // total numerics column width
static constexpr int DIAG_ROW_H       = 8;                                  // each numerics sub-row (font ~7 px)
static constexpr int DIAG_ROW1_Y      = HEADER_ROW2_Y;                      // 10
static constexpr int DIAG_ROW2_Y      = HEADER_ROW2_Y + DIAG_ROW_H;         // 18
static constexpr int DIAG_ROW3_Y      = HEADER_ROW2_Y + DIAG_ROW_H * 2;     // 26
static constexpr int DIAG_ROW4_Y      = HEADER_ROW2_Y + DIAG_ROW_H * 3;     // 34
static constexpr int DIAG_GRAPH_X     = DIAG_NUM_W + 2;                     // 92
static constexpr int DIAG_GRAPH_Y     = 0;                                  // full header height
static constexpr int DIAG_GRAPH_W     = SCREEN_W - DIAG_GRAPH_X;            // 148
static constexpr int DIAG_GRAPH_H     = HEADER_FULL_H;                      // 42

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

static constexpr uint32_t AUDIO_TASK_STACK    = 6 * 1024;
static constexpr UBaseType_t AUDIO_TASK_PRIO  = 3;
static constexpr BaseType_t AUDIO_TASK_CORE   = 0;
// Visualisation render task — fires from an `esp_timer` periodic callback
// rather than `vTaskDelayUntil` so we get microsecond-precision scheduling.
// Locking the render period to the panel scan period eliminates the beat
// frequency between renders and panel refresh (which manifested as a slow
// left-to-right tear sweep at the beat frequency).
//
// Render derivations:
//   render_period_us = 1e6 / panel_scan_hz
//   cols_per_render  = display_cols_per_sec / panel_scan_hz
//   (must be a clean integer — static_assert below)
static constexpr uint32_t VIZ_TASK_STACK      = 3 * 1024;
static constexpr UBaseType_t VIZ_TASK_PRIO    = 2;
static constexpr BaseType_t VIZ_TASK_CORE     = 1;
// Render rate. Setting this to half the panel scan rate (= 30 Hz when
// panel is 60 Hz) makes each rendered frame persist on the panel for two
// scan cycles — one with tear at the push moment, one clean rescan. The
// eye blends and perceives the clean frame as dominant. At full panel
// rate, tear is constantly visible at the same stationary position.
static constexpr uint32_t VIZ_RENDER_HZ       = 60;
// Render period — calibrated empirically against the actual panel scan
// rate (~59.54 Hz on this Cardputer).
static constexpr uint32_t RENDER_PERIOD_US    = 16780;
static constexpr int      VIZ_COLS_PER_PUSH   =
    (int)(VIZ_COLS_PER_SEC / (float)VIZ_RENDER_HZ + 0.5f);
static_assert(VIZ_COLS_PER_PUSH * (int)VIZ_RENDER_HZ ==
              (int)(VIZ_COLS_PER_SEC + 0.5f),
              "VIZ_COLS_PER_SEC must be an integer multiple of VIZ_RENDER_HZ "
              "— adjust VIZ_ZOOM_SECONDS or VIZ_RENDER_HZ");
static constexpr uint32_t DIAGNOSTICS_POLL_MS = 250;

// Browser uses the full display width. The scrollbar reserves a narrow
// gutter on the right edge; content (names, wrap, clip) uses COL_CONTENT_W.
static constexpr int COL_W            = SCREEN_W;             // 240
static constexpr int SCROLLBAR_W      = 3;
static constexpr int COL_CONTENT_W    = COL_W - SCROLLBAR_W;
static constexpr int SCROLLBAR_X      = COL_W - SCROLLBAR_W;
static constexpr int SCROLLBAR_MIN_H  = 6;

static constexpr int BASE_ROW_H   = 9;
static constexpr int BASE_CHAR_W  = 6;
static constexpr int COL_PAD      = 2;

static constexpr int MAX_VOL      = 64;
// Floor on the user-tunable volume cap. 0 would mute the device permanently
// from inside Settings — not a useful state and easy to land in by accident.
static constexpr int VOLUME_MAX_MIN = 4;
static constexpr uint32_t BATTERY_POLL_MS = 5000;

static constexpr int BATTERY_ICON_W = 30;
static constexpr int BATTERY_ICON_H = 8;
static constexpr int BATTERY_ICON_X = 3;
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
// One colour per entry kind for the (single) browser column.
static constexpr uint16_t COL_DIR_NORMAL    = 0x4FDF;  // bright cyan — distinct from files
static constexpr uint16_t COL_FILE_BRIGHT   = 0xFFFF;  // white (used in help, "(empty)")
static constexpr uint16_t COL_FILE_NORMAL   = 0xC618;  // light grey — readable, clearly not cyan
static constexpr uint16_t COL_OTHER_NORMAL  = 0x2104;  // dim grey — unplayable, deliberately recessive
// Background tint behind the browser's selected row. Slate-blue, a notch
// dimmer than COL_BROWSE_FRAME so the row reads as filled-but-recessive
// rather than competing with the frame for attention. Every channel is
// above the RGB332 truncate-and-shift threshold (R5≥4, G6≥8, B5≥8) so the
// colour survives 8 bpp canvas quantisation.
static constexpr uint16_t COL_SELECTION_BG  = 0x4978;
// Background tint behind the settings screen's selected row. Dim yellow,
// distinguishing the settings mode at a glance from blue (browse) and
// green (search). Channels chosen to survive RGB332 quantisation.
static constexpr uint16_t COL_SETTINGS_SEL_BG = 0x6320;
// Search-mode selection — dark muted green, distinct from the browser's
// blue-grey so the user can see at a glance whether they're browsing or
// searching.
static constexpr uint16_t COL_SEARCH_SEL_BG = 0x0240;
// Search-mode prompt and caret — light green, the green counterpart to the
// directory cyan (COL_DIR_NORMAL).
static constexpr uint16_t COL_SEARCH_PROMPT = 0x4FE0;
static constexpr uint16_t COL_HAIRLINE   = 0x4208;  // mid grey for column dividers
static constexpr uint16_t COL_HEADER_TXT = 0x7BEF;
// Brighter mid-grey for the diagnostics numerics. 0x7BEF reads as too
// dim against the black header at the small (6 px) font size. Matches
// COL_FILE_NORMAL — clearly visible secondary text.
static constexpr uint16_t COL_DIAG_TXT   = 0xC618;
static constexpr uint16_t COL_FOOTER_TXT   = 0xFFFF;  // track name
static constexpr uint16_t COL_FOOTER_PROG  = 0x6979;  // progress bar while playing (slate-blue)
static constexpr uint16_t COL_FOOTER_IDLE  = 0x7BEF;  // progress bar while paused or stopped (mid-grey)
static constexpr uint16_t COL_FOOTER_VOL   = 0x34D0;  // volume bar (muted teal)
// Loudness-leveling accent (footer "L" + amplification trace). Warm amber so
// it reads clearly both over the cool slate-blue waveform and against black.
static constexpr uint16_t COL_LEVEL_ACCENT = 0xFD20;  // amber-orange
static constexpr uint16_t COL_WARN         = 0xFB00;  // deep orange — warnings (e.g. no RTC)
static constexpr uint16_t COL_FOOTER_FRAME = 0x6979;  // hairline above footer
// Mode-coloured frame elements (separators above and below the main area,
// and the scrollbar gutter) — blue in browse mode, green in search mode.
// The blue is the same slate-blue used for the footer hairline so frame
// pieces read as a continuous palette.
static constexpr uint16_t COL_BROWSE_FRAME = 0x6979;  // slate-blue
static constexpr uint16_t COL_SEARCH_FRAME = 0x652D;  // slate-green
// Track-pick mode reuses the Settings yellow so picking reads as a settings
// activity, not normal browse: blue = browse, green = search, yellow = pick.
static constexpr uint16_t COL_PICK_FRAME   = 0xA520;  // slate-yellow

// Off-screen back buffer. All draw operations target g_canvas; once a frame
// is composed, presentFrame() pushes it to the panel in one operation, so
// the panel transitions directly from old content to new — no fillRect-then-
// redraw flicker.
//
// The canvas is default-constructed at file-scope static init and the parent
// display is passed explicitly into pushSprite() at call time, sidestepping
// a static-init-order fiasco where binding `_parent` to `&M5Cardputer.Display`
// at construction can capture an unbound-reference address (Display is a
// reference member of M5Cardputer, only valid once M5Cardputer's own static
// init has run).
static M5Canvas g_canvas;
// Display push mutex — serialises every push so the main loop's header /
// browser / footer updates and the vizTask's per-frame push never start
// SPI transactions concurrently. Without this, occasional torn pushes
// produce visible glitches.
static SemaphoreHandle_t g_display_mutex = nullptr;

static inline void presentFrame() {
    if (g_display_mutex) xSemaphoreTake(g_display_mutex, portMAX_DELAY);
    g_canvas.pushSprite(&M5Cardputer.Display, 0, 0);
    if (g_display_mutex) xSemaphoreGive(g_display_mutex);
}

// Visualisation-overlay active flags. Full state (shared cursor etc.)
// lives further down with the rest of the visualisation block.
static bool g_waveform_active = false;
static bool g_spectrum_active  = false;
// Diagnostic test-pattern overlay — replaces audio-derived rendering
// with evenly-spaced scrolling bright bars so beat-frequency tear is
// directly observable for panel-rate calibration.
static bool g_viz_test_pattern = false;
static constexpr int VIZ_TEST_BAR_SPACING = 30;

// Push only rows [y, y + h) of the canvas to the panel. The canvas is at
// 8 bpp (RGB332) and the panel is 16 bpp (RGB565), so we can't pushImage
// the raw buffer — pushSprite handles the format conversion. Set a clip
// rect on the display so only the requested rows are written, even though
// pushSprite iterates the full canvas.
static inline void presentRows(int y, int h) {
    if (h <= 0) return;
    if (g_display_mutex) xSemaphoreTake(g_display_mutex, portMAX_DELAY);
    M5Cardputer.Display.setClipRect(0, y, SCREEN_W, h);
    g_canvas.pushSprite(&M5Cardputer.Display, 0, 0);
    M5Cardputer.Display.clearClipRect();
    M5Cardputer.Display.waitDisplay();
    if (g_display_mutex) xSemaphoreGive(g_display_mutex);
}

static int  g_font_notch     = 1;
static bool g_diagnostics_hidden = true;
static int  g_indexing_phase     = 0;
static uint32_t g_indexing_last_ms = 0;
static int  g_alarms_cursor  = 0;
static int  g_leveling_cursor = 0;
static int  g_alarm_editor_slot = 0;
static int  g_alarm_editor_cursor = 0;
static int  g_alarm_editor_top = 0;  // first visible row; the Track row's
                                     // variable height needs running-y scroll
static int  g_days_editor_cursor = 0;
// --- Screen state (single source of truth, replacing the g_show_* flags) ---
// One enum names the current screen. The Main screen's content (file browser
// / search / visualisation) stays a sub-state of Main, tracked by the existing
// g_search_active / visualisation flags. parentOf() encodes the navigation
// tree so back-out is one "go to parent" step. AlarmFiring/Snoozing are a
// system interrupt that pre-empts any screen and restores it on dismiss, so
// they sit outside the tree.
enum class Screen {
    Main, Settings, KeyReference, ResetModal,
    Alarms, SetTime, AlarmEditor, DaysEditor, TrackPicker,
    Leveling,
    Chess, ChessConfirm, Standby, AlarmFiring, AlarmSnoozing,
};
static Screen g_screen = Screen::Main;
// The reset modal can be opened from Settings or from the browser, so it
// remembers where to return on dismiss.
static Screen g_modal_return = Screen::Main;

// Render whatever screen g_screen names (the single render entry point).
static void renderScreen();
// Switch to a screen and render it.
static void goToScreen(Screen s);
// Back out one level to the parent; from the Main root this enters Standby.
static void backOut();

static Screen parentOf(Screen s) {
    switch (s) {
        case Screen::Settings:     return Screen::Main;
        case Screen::KeyReference: return Screen::Settings;
        case Screen::ResetModal:   return Screen::Settings;
        case Screen::Alarms:       return Screen::Settings;
        case Screen::Leveling:     return Screen::Settings;
        case Screen::ChessConfirm: return Screen::Main;
        case Screen::SetTime:      return Screen::Alarms;
        case Screen::AlarmEditor:  return Screen::Alarms;
        case Screen::DaysEditor:   return Screen::AlarmEditor;
        case Screen::TrackPicker:  return Screen::AlarmEditor;
        default:                   return Screen::Main;  // Main, Chess, Standby, alarm interrupt
    }
}

// Preview state — forward-declared here so activateAlarmEditorRow can set
// it; the firing logic lives near the fire helpers below.
static uint32_t g_alarm_preview_fire_at_ms = 0;
static int      g_alarm_preview_slot = -1;
// RTC defined here (rather than next to its helpers below) so the alarm
// editor / set-time / pollAlarms code can reach it.
static Unit_RTC g_rtc;
// RTC presence, latched once at boot (is the Port-A add-on fitted?). When
// false there is no timekeeping: the software clock has no seed, alarms never
// arm or fire, and the clock UI says so rather than showing a wrong time.
static bool     g_rtc_present   = false;
// Software wall-clock: epoch seconds captured at the last RTC sync, plus the
// millis() at that moment. Current time = epoch + elapsed seconds, so the
// clock free-runs between infrequent RTC reads.
static uint32_t g_clock_epoch   = 0;
static uint32_t g_clock_sync_ms = 0;
// Track picker — when `g_pick_slot >= 0` the browser is in pick mode:
// activating an audio file sets that slot's track and returns to the
// editor; navigation otherwise behaves as normal.
static int g_pick_slot = -1;
static std::string g_pick_saved_path;
static int         g_pick_saved_cursor = 0;
// Set-current-time editor: draft values held in these globals while the
// user adjusts; committed to the RTC on the Commit action row.
static int      g_sct_cursor = 0;
static uint16_t g_sct_year   = 2026;
static uint8_t  g_sct_month  = 1;
static uint8_t  g_sct_day    = 1;
static uint8_t  g_sct_hour   = 0;
static uint8_t  g_sct_min    = 0;
static int  g_help_top       = 0;
static int  g_settings_cursor = 0;
static bool g_hide_non_audio = true;
static bool g_auto_play_next = true;
static bool g_auto_waveform  = false;
static bool g_auto_spectrum   = false;
static int  g_volume_max     = 16;   // 0..MAX_VOL ceiling on live volume

// Loudness leveling — drive-into-a-limiter levelling of playback. The
// limiter itself lives in the audio-output object; these are the persisted
// user knobs, mirrored into it by applyLeveling. Drive in whole dB; release
// in tenths of a second (5 = 0.5 s).
static bool g_leveling_enabled     = false;
static int  g_leveling_drive_db    = 12;   // 0..24 dB
static int  g_leveling_release_ds  = 5;    // 1..20 → 0.1..2.0 s
static int  g_leveling_attack_hms  = 2;    // 1..20 → 0.5..10.0 ms (half-ms units)
static int  g_leveling_lookahead_ms = 5;   // 1..12 ms
static int  g_leveling_ceiling_hdb = 2;    // 1..12 → -0.5..-6.0 dBFS (half-dB units)
static constexpr int LEVELING_DRIVE_DB_MAX   = 24;
static constexpr int LEVELING_RELEASE_DS_MIN = 1;
static constexpr int LEVELING_RELEASE_DS_MAX = 20;
static constexpr int LEVELING_ATTACK_HMS_MIN   = 1;
static constexpr int LEVELING_ATTACK_HMS_MAX   = 20;
static constexpr int LEVELING_LOOKAHEAD_MS_MIN = 1;
static constexpr int LEVELING_LOOKAHEAD_MS_MAX = 12;
static constexpr int LEVELING_CEILING_HDB_MIN  = 1;
static constexpr int LEVELING_CEILING_HDB_MAX  = 12;


// Idle-screen timeout — discrete steps surfaced in settings. Index into
// IDLE_TIMEOUT_MS picks the time-to-dim; off-step lives 60 s past dim.
// `off` (0 ms) disables idle blanking entirely.
static constexpr int IDLE_TIMEOUT_COUNT = 5;
static constexpr uint32_t IDLE_TIMEOUT_MS_TABLE[IDLE_TIMEOUT_COUNT] = {15000, 30000, 60000, 300000, 0};
static const char* const IDLE_TIMEOUT_LABELS[IDLE_TIMEOUT_COUNT] = {"15s", "30s", "1m", "5m", "off"};
static int  g_idle_timeout_idx = 2;  // default 1m — matches original behaviour

// Idle-blank state machine. Three states: FULL (user-set brightness),
// FADING (ramp toward zero in progress; the panel is still showing
// content and other pollers keep redrawing it), OFF (ramp complete, panel
// dark, redraws suppressed). The earlier three-state "dim before off"
// model was replaced by a single slow fade; FADING is its successor —
// distinguishing "fade in progress" from "fade complete" so the display
// keeps updating right until it actually goes dark.
enum ScreenState { SCREEN_FULL, SCREEN_FADING, SCREEN_OFF };
static ScreenState g_screen_state  = SCREEN_FULL;
static uint32_t    g_last_activity_ms = 0;
static uint32_t    g_imu_last_ms   = 0;
static float       g_imu_prev_x = 0, g_imu_prev_y = 0, g_imu_prev_z = 0;
static bool        g_imu_seeded = false;
static inline uint32_t idleOffMs() { return IDLE_TIMEOUT_MS_TABLE[g_idle_timeout_idx]; }
static inline bool    idleEnabled() { return idleOffMs() > 0; }
static constexpr uint32_t IMU_POLL_MS = 100;
static constexpr float    IMU_MOTION_THRESHOLD = 0.05f;  // delta in g

// Brightness — user-tunable in 9 steps over the 6..255 hardware range.
// Log-ish spacing matches eye sensitivity. On this hardware, values up
// to and including 5 produce no visible backlight output, so 6 is the
// visible floor; 6, 7 and 8 all produce distinct but very dim levels
// worth offering for use in dark environments.
static constexpr int BRIGHTNESS_COUNT = 9;
static constexpr uint8_t BRIGHTNESS_LEVELS[BRIGHTNESS_COUNT] = {
    6, 7, 8, 16, 32, 64, 128, 192, 255
};
static int g_brightness_idx = BRIGHTNESS_COUNT - 1;
// Standby/clock-mode brightness is intentionally capped low: bedside use
// + battery life dominate, and the backlight is the biggest power draw.
// STANDBY_BRIGHTNESS_MAX_IDX caps the ladder index user-side; values in
// BRIGHTNESS_LEVELS[0..max] = {6, 7, 8, 16} are all dim levels — the warm
// red/orange palette reads as quite dim, so the cap goes one notch higher
// than the literal PWM-8 ceiling.
static constexpr int STANDBY_BRIGHTNESS_MAX_IDX = 3;
static int g_standby_brightness_idx = 3;  // PWM 16
static inline uint8_t userBrightness()    { return BRIGHTNESS_LEVELS[g_brightness_idx]; }
static inline uint8_t standbyBrightness() { return BRIGHTNESS_LEVELS[g_standby_brightness_idx]; }

// Ramp durations: slow when fading out so the transition reads as
// intentional rather than a glitch; quick when waking so the response
// feels immediate.
static constexpr uint32_t RAMP_OFF_MS  = 10000;
static constexpr uint32_t RAMP_WAKE_MS = 1000;
static constexpr uint32_t RAMP_SET_MS  = 200;  // user-initiated brightness change

// Brightness ramp state — linear interpolation from start → target over
// ramp_ms. setBrightnessRampTo() seeds; pollBrightnessRamp() applies.
static uint8_t  g_brightness_current  = 255;
static uint8_t  g_brightness_target   = 255;
static uint8_t  g_brightness_start    = 255;
static uint32_t g_brightness_start_ms = 0;
static uint32_t g_brightness_ramp_ms  = 0;
static bool g_wrap_names     = true;
// Header's top-left shows the version at boot, then swaps to the path
// breadcrumb on the user's first keypress — the user always sees the
// version on a fresh boot, then the slot is repurposed for orientation
// once they start interacting.
static bool g_first_keypress_seen = false;

// True whenever any full-screen overlay (settings, key reference, reset
// modal) is occluding the normal browser/header/footer composition.
// Used to suppress redraws and key handlers that don't apply while an
// overlay is on top.
static inline bool fullScreenClockActive() {
    return g_screen == Screen::Standby || g_screen == Screen::AlarmFiring
        || g_screen == Screen::AlarmSnoozing;
}
static inline bool overlayActive() {
    // Everything except the Main browser and the (browser-rendered) track
    // picker occludes the normal browser/header/footer composition.
    return g_screen != Screen::Main && g_screen != Screen::TrackPicker;
}

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
// Frame colour follows the active browse context (see COL_PICK_FRAME).
static inline uint16_t browseFrameColor() { return (g_pick_slot >= 0) ? COL_PICK_FRAME : COL_BROWSE_FRAME; }

enum EntryKind { KIND_DIR, KIND_AUDIO, KIND_OTHER };
struct Entry { std::string name; EntryKind kind; };

static std::string           g_cur_path = "/";
static std::vector<Entry>    g_entries;
static int                   g_cursor = 0;
static int                   g_top    = 0;

// Force a full redraw of the visualisation area on the next render
// (e.g. on activation, mode change, browser size change, or snap). Set
// by the various toggle / snap paths; cleared after a full redraw.
static bool     g_viz_sprite_dirty = true;

// Visualisation overlay state — drives both waveform and spectrum views.
// `g_waveform_active` and `g_spectrum_active` are independent on/off
// flags; both on yields the dual layout, either alone gives a single
// full-height overlay. A single shared wall-clock-paced cursor
// (`g_viz_disp_abs`) advances by elapsed-time / col-period each poll,
// so average display rate matches the audio column rate regardless of
// how fast or slow renders fire. Bursty FFT writes don't translate into
// bursty display motion; render rate caps translate into 1-or-2-pixel
// steps instead of accumulating lag that would eventually wrap the ring.
// g_waveform_active / g_spectrum_active are declared earlier (above
// presentRows) so the push-duration instrumentation can see them.
static double   g_viz_disp_abs    = 0.0;
static uint32_t g_viz_prev_us     = 0;  // 0 = re-anchor on next poll
static uint64_t g_viz_last_rendered_abs = ~(uint64_t)0;
// Anchor for the predicted-abs_head smoothing. Set ONCE at activation;
// predicted grows linearly from there at the column rate. Capped at
// `actual + lookahead - 1` so target_max can never read unwritten
// slots. Drift correction (re-anchor) only fires if predicted falls
// far behind actual — covers sample-rate mismatch over long playback.
static uint64_t g_viz_anchor_abs = 0;
static uint32_t g_viz_anchor_us  = 0;
static constexpr double VIZ_PRED_LAG_REANCHOR_COLS = 20.0;
// VIZ_COLS_PER_PUSH derived earlier from VIZ_COLS_PER_SEC / PANEL_SCAN_HZ.
// Lookahead serves as cap headroom: predicted can be up to `lookahead_cols`
// ahead of actual without target_max overshooting a written slot, so it
// must exceed the max decoder-burst size in cols (FLAC frame ~11 cols ≈
// 90 ms). It also positions the displayed column relative to audible:
// display ≈ abs_head − lookahead, and audible runs ~220 ms behind abs_head,
// so a 175 ms lookahead puts the viz ~45 ms ahead of audible — set by
// visual bisect on representative tracks.
static constexpr uint32_t VIZ_LOOKAHEAD_MS = 175;
// Dual-mode height fractions: waveform 2/5 on top, spectrum 3/5 below.
static constexpr int      DUAL_WAVEFORM_NUM = 2;
static constexpr int      DUAL_DENOM        = 5;

// Fuzzy search mode — alternative to the directory listing. Active state
// hides the directory and shows a query input + ranked results.
static bool                            g_search_active = false;
// Global playback transport (pause / volume / skip / seek) is live on every
// screen except text entry (search), the modal, and the bespoke clock/alarm
// screens (which handle those keys themselves — e.g. firing's volume carries a
// ramp-override side effect, and standby treats any key as exit).
static inline bool transportAllowed() {
    return !(g_screen == Screen::Main && g_search_active)
        && g_screen != Screen::ResetModal
        && g_screen != Screen::ChessConfirm
        && !fullScreenClockActive();
}
static String                          g_search_query;
static std::vector<FuzzyIndex::Hit>    g_search_hits;
static std::vector<std::string>        g_search_paths;
static int                             g_search_cursor = 0;
static int                             g_search_top    = 0;


static std::string           g_play_path;
static std::string           g_play_dir;
static std::vector<Entry>    g_play_entries;
static int                   g_play_idx = -1;

static int  g_volume = 16;   // a quarter of MAX_VOL (64) — soft start
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
static constexpr uint32_t MARQUEE_TICK_MS  = 100;   // ~10 Hz
static constexpr uint32_t MARQUEE_PAUSE_MS = 1000;  // start + end pauses

static uint32_t g_last_seek_ms = 0;
static uint32_t g_last_nav_ms  = 0;
// Time the current up/down hold started; 0 when no key is held. Used to gate
// the initial repeat behind a long delay so a normal tap (~150 ms dwell)
// doesn't double-fire.
static uint32_t g_nav_press_ms = 0;
static uint32_t g_audio_start_offset = 0;

// Audio path. The output sink (g_out, ~9 KB ring) stays for the firmware's
// life; the source and the generator are built fresh in startPlayback and
// torn down in stopPlayback, so each track-change fully releases the
// decoder's working memory.
static AudioOutputM5CardputerSpeaker  g_out;
static AudioFileSource*               g_src = nullptr;
static AudioGenerator*                g_gen = nullptr;
static bool                           g_audio_active = false;

static uint32_t g_last_progress_ms = 0;

static int      g_battery_level     = -1;
static int      g_battery_voltage_mv = 0;
static uint32_t g_battery_last_ms   = 0;

static void enterBatteryLowState();
static void markStateDirty();  // forward decl — used by mutators below; defined after audioTask

static SemaphoreHandle_t g_audio_mutex = nullptr;
static TaskHandle_t      g_audio_task  = nullptr;

static uint32_t g_diag_last_ms   = 0;
static uint32_t g_diag_stack_used = 0;
static uint32_t g_diag_underruns  = 0;

// One sample per pixel of graph width; new samples land on the right and
// the buffer slides left once full. Stored as 0..255 so each bar's height
// and threshold colour come straight out of one byte. Only the CPU cell
// has a sparkline; stk / buf / ram are shown as raw numbers since they
// move slowly and a graph adds little.
struct Sparkline {
    uint8_t  samples[DIAG_GRAPH_W] = {0};
    int      count                 = 0;
};

static Sparkline g_spark_cpu0;
static Sparkline g_spark_cpu1;

// Latest sampled values, displayed as numerics each tick.
static int g_diag_stk_pct  = 0;
static int g_diag_buf_pct  = 0;
static int g_diag_ram_pct  = 0;
static int g_diag_largest_pct = 0;  // largest contiguous free block / total heap
static int g_diag_minfree_pct = 0;  // (total - low-water-mark free) / total — peak pressure since boot
static int g_diag_imu_mg   = 0;   // latest IMU delta magnitude in mg
static char g_diag_clk[9]  = "--:--:--";  // HH:MM:SS from external RTC, refreshed once per second

// ESP-IDF per-core IPC tasks. System code (not ours) runs on these to
// service cross-core requests — most notably interrupt allocation during
// boot. The stack peak crept past the configured size once before; we
// surface the current high-water-mark free bytes so creep is visible
// before it tips into a canary panic at boot.
static TaskHandle_t g_ipc_task_0      = nullptr;
static TaskHandle_t g_ipc_task_1      = nullptr;
static uint32_t     g_diag_ipc0_free  = 0;
static uint32_t     g_diag_ipc1_free  = 0;

// CPU sampling: each core's IDLE task carries a run-time counter (in real
// microseconds) thanks to FreeRTOS run-time-stats being enabled at the
// build level (`CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS=y`). We read each
// core's idle counter via `vTaskGetInfo()` and divide the per-tick delta
// by the wall-clock delta to get the idle fraction. First sample is honest
// — no calibration warmup.
static TaskHandle_t g_idle_task_0 = nullptr;
static TaskHandle_t g_idle_task_1 = nullptr;
static uint32_t g_idle_us_0_prev  = 0;
static uint32_t g_idle_us_1_prev  = 0;
static int64_t  g_cpu_sample_us_prev = 0;

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
        if (k == KIND_OTHER && g_hide_non_audio) continue;
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

static uint16_t kindColour(EntryKind k) {
    switch (k) {
        case KIND_DIR:   return COL_DIR_NORMAL;
        case KIND_AUDIO: return COL_FILE_NORMAL;
        default:         return COL_OTHER_NORMAL;
    }
}

// Static-trim a path to fit `max_chars`, prefixing "..." when the left
// side has been chopped. Newest (rightmost) segments stay visible; older
// ones drop off the left edge. No animation — keeps the header redraw
// cost flat, in keeping with the responsiveness goal.
static std::string crumbForWidth(const std::string& path, int max_chars) {
    if ((int)path.size() <= max_chars) return path;
    if (max_chars < 4) return path.substr(path.size() - max_chars);
    int keep = max_chars - 3;
    return std::string("...") + path.substr(path.size() - keep);
}

// Forward decl so stopPlayback can force an immediate spectrum redraw
// after hardFlush; the full definition lives further down.
static void composeBrowser();
// Forward decls so exitChess can rebuild the set-aside track (definitions
// live further down with startPlayback's default argument).
static bool startPlayback(const std::string& full_path, bool start_paused);
static void seekToByte(uint32_t target);
static void drawChessConfirm();

// On-demand heap census over USB serial — press 'h' in the monitor. Works
// anytime without a reboot (native USB-CDC drops the boot log on reset), and
// breaks the heap down by capability so the DMA-reserved portion (I²S buffers)
// is visible separately from general internal RAM.
static void pollHeapDiag() {
    if (!Serial.available()) return;
    int c = Serial.read();
    if (c != 'h' && c != 'H') return;
    Serial.printf("[heap] default  free=%6u largest=%6u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DEFAULT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    Serial.printf("[heap] internal free=%6u largest=%6u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    Serial.printf("[heap] dma      free=%6u largest=%6u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

static void applyVolume() {
    // Volume goes through M5.Speaker's own digital control on the codec,
    // matching the original ESP8266Audio path. Range 0..255 maps from
    // 0..MAX_VOL.
    M5Cardputer.Speaker.setVolume((uint8_t)((g_volume * 255) / MAX_VOL));
}

// Push the persisted leveling knobs into the limiter. Called after loadState
// and whenever a knob changes; the setters are lock-free scalar writes.
static void applyLeveling() {
    LoudnessLimiter& lim = g_out.limiter();
    lim.setDriveDb((float)g_leveling_drive_db);
    lim.setReleaseSeconds((float)g_leveling_release_ds / 10.0f);
    lim.setAttackMs((float)g_leveling_attack_hms * 0.5f);
    lim.setLookaheadMs((float)g_leveling_lookahead_ms);
    lim.setCeilingDb(-(float)g_leveling_ceiling_hdb * 0.5f);
    lim.setEnabled(g_leveling_enabled);
}

static void stopPlayback() {
    AudioGenerator* gen_to_free = nullptr;
    AudioFileSource* src_to_free = nullptr;
    if (g_audio_mutex) xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
    // Drop pre-buffered samples + stop the speaker queue so user-initiated
    // stop / skip feels instant. Without this the ~220 ms of audio in the
    // path would play out after the keypress before any new track starts.
    g_out.hardFlush();
    // Note: hardFlush no longer wipes the visualisation rings — the
    // existing on-screen spectrum / waveform content is left in place to
    // scroll off naturally as new audio arrives.
    if (g_gen && g_gen->isRunning()) g_gen->stop();
    gen_to_free = g_gen;
    src_to_free = g_src;
    g_gen = nullptr;
    g_src = nullptr;
    g_audio_active = false;
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
    markStateDirty();
}

static void audioTask(void*) {
    for (;;) {
        bool did_work = false;
        xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
        if (g_gen && g_gen->isRunning() && !g_paused) {
            if (!g_gen->loop()) {
                uint32_t srcpos = g_src ? g_src->getPos()  : 0;
                uint32_t srcsz  = g_src ? g_src->getSize() : 0;
                Serial.printf("loop fail: samples=%u srcPos=%u/%u heap=%u largest=%u: %s\n",
                              (unsigned)g_out.samplesConsumed(),
                              (unsigned)srcpos, (unsigned)srcsz,
                              (unsigned)ESP.getFreeHeap(),
                              (unsigned)ESP.getMaxAllocHeap(),
                              g_play_path.c_str());
                g_gen->stop();
                delete g_gen; g_gen = nullptr;
                delete g_src; g_src = nullptr;
                g_audio_active = false;
                g_play_path.clear();
                g_advance_pending = true;
            }
            did_work = true;
        }
        // Ship whatever the decoder has put into the pre-buffer toward the
        // speaker. Always called when not paused so the pre-buffer drains
        // even between decoder iterations.
        if (!g_paused) g_out.shipBuffered();
        xSemaphoreGive(g_audio_mutex);
        // Sleep one tick rather than taskYIELD(): yield only hands off to
        // equal-or-higher-priority tasks, so IDLE1 would never get a slot
        // for background cleanup.
        if (!did_work) vTaskDelay(10 / portTICK_PERIOD_MS);
        else           vTaskDelay(1);
    }
}

// Forward decl — defined later in the file.
static void pollVisualisation();

// Render task — woken by an esp_timer periodic notification rather than
// vTaskDelayUntil so the render period is precise to microseconds (not
// 1 ms FreeRTOS ticks). This lets us lock the period exactly to the
// panel scan period and eliminates the beat-frequency tear sweep we saw
// at coarser scheduling.
static TaskHandle_t g_viz_task_handle = nullptr;

static void vizTimerCallback(void*) {
    if (g_viz_task_handle) xTaskNotifyGive(g_viz_task_handle);
}

static void vizTask(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        pollVisualisation();
    }
}

// -- Persisted state ------------------------------------------------------
// User-facing state (volume, current track, current folder + cursor, font
// size, diagnostics-row visibility, wrap-names) survives power cycles via
// NVS. Mutators set a dirty flag; the main loop flushes at most once per
// PERSIST_FLUSH_MS so a burst of keypresses or fast scrolling coalesces
// into one write. Emergency shutdown does not save — whatever was dirty
// at cutoff is lost, consistent with the no-shutdown-step intent.

static Preferences        g_prefs;
static bool               g_state_dirty = false;
static uint32_t           g_state_dirty_since_ms = 0;
static constexpr uint32_t PERSIST_FLUSH_MS = 5000;

// Alarms — five slots; each is independent and persisted in `player` NVS.
// `enabled=false` disarms a slot without losing its configuration.
struct Alarm {
    uint8_t  hour    = 7;
    uint8_t  minute  = 0;
    uint8_t  days    = 0;     // bitmask Mon=bit0..Sun=bit6
    uint8_t  volume  = 8;
    uint8_t  ramp_s  = 0;     // 0..60; 0 disables ramp
    bool     enabled = false;
    std::string track;        // absolute path; empty → beep fallback
};
static constexpr int ALARM_COUNT = 5;
static Alarm g_alarms[ALARM_COUNT];

// Playhead resume — saved byte offset within the currently-playing track,
// loaded from NVS at boot and applied after startPlayback succeeds. After
// boot-resume the value is consumed; subsequent flushes write the live
// `g_src->getPos()` so the NVS record tracks the playhead while playing.
static uint32_t           g_saved_playhead = 0;
static uint32_t           g_playhead_save_ms = 0;
static constexpr uint32_t PLAYHEAD_SAVE_INTERVAL_MS = 10000;

static void markStateDirty() {
    if (!g_state_dirty) {
        g_state_dirty_since_ms = millis();
        g_state_dirty = true;
    }
}

// Read every persisted item, falling back to the existing in-memory
// default when a key is absent. Folder fallback to "/" is enforced by
// the caller after the SD scan; track fallback (saved path missing) is
// enforced when we attempt to resume.
static void loadState() {
    if (!g_prefs.begin("player", true)) return;  // RO
    g_volume             = g_prefs.getInt   ("vol",    g_volume);
    g_font_notch         = g_prefs.getInt   ("font",   g_font_notch);
    g_diagnostics_hidden = g_prefs.getBool  ("diag",   g_diagnostics_hidden);
    g_wrap_names         = g_prefs.getBool  ("wrap",   g_wrap_names);
    g_hide_non_audio     = g_prefs.getBool  ("hidena", g_hide_non_audio);
    g_auto_play_next     = g_prefs.getBool  ("autonx", g_auto_play_next);
    g_auto_waveform      = g_prefs.getBool  ("autowv", g_auto_waveform);
    g_auto_spectrum      = g_prefs.getBool  ("autosp", g_auto_spectrum);
    g_volume_max         = g_prefs.getInt   ("volmax", g_volume_max);
    g_idle_timeout_idx   = g_prefs.getInt   ("idleto", g_idle_timeout_idx);
    g_brightness_idx     = g_prefs.getInt   ("bright", g_brightness_idx);
    g_standby_brightness_idx = g_prefs.getInt("stbybr", g_standby_brightness_idx);
    g_leveling_enabled   = g_prefs.getBool  ("lvl",    g_leveling_enabled);
    g_leveling_drive_db  = g_prefs.getInt   ("lvldb",  g_leveling_drive_db);
    g_leveling_release_ds = g_prefs.getInt  ("lvlrel", g_leveling_release_ds);
    g_leveling_attack_hms = g_prefs.getInt  ("lvlat",  g_leveling_attack_hms);
    g_leveling_lookahead_ms = g_prefs.getInt("lvlla",  g_leveling_lookahead_ms);
    g_leveling_ceiling_hdb = g_prefs.getInt ("lvlce",  g_leveling_ceiling_hdb);
    for (int i = 0; i < ALARM_COUNT; ++i) {
        char k[8];
        snprintf(k, sizeof(k), "a%d_hm", i);
        uint16_t hm = g_prefs.getUShort(k, (uint16_t)((g_alarms[i].hour << 8) | g_alarms[i].minute));
        g_alarms[i].hour   = (hm >> 8) & 0xFF;
        g_alarms[i].minute =  hm       & 0xFF;
        snprintf(k, sizeof(k), "a%d_d", i);   g_alarms[i].days    = g_prefs.getUChar(k, g_alarms[i].days);
        snprintf(k, sizeof(k), "a%d_v", i);   g_alarms[i].volume  = g_prefs.getUChar(k, g_alarms[i].volume);
        snprintf(k, sizeof(k), "a%d_r", i);   g_alarms[i].ramp_s  = g_prefs.getUChar(k, g_alarms[i].ramp_s);
        snprintf(k, sizeof(k), "a%d_e", i);   g_alarms[i].enabled = g_prefs.getUChar(k, 0) != 0;
        snprintf(k, sizeof(k), "a%d_t", i);   g_alarms[i].track   = std::string(g_prefs.getString(k, "").c_str());
    }
    g_cur_path           = std::string(g_prefs.getString("folder", g_cur_path.c_str()).c_str());
    g_cursor             = g_prefs.getInt   ("cursor", g_cursor);
    g_play_path          = std::string(g_prefs.getString("track",  g_play_path.c_str()).c_str());
    g_saved_playhead     = g_prefs.getUInt  ("trkpos", 0);
    g_prefs.end();
    // Clamp loaded values defensively — corrupt or out-of-range values
    // fall back to safe defaults rather than indexing past array ends.
    if (g_volume_max < VOLUME_MAX_MIN) g_volume_max = VOLUME_MAX_MIN;
    if (g_volume_max > MAX_VOL)        g_volume_max = MAX_VOL;
    if (g_volume > g_volume_max) g_volume = g_volume_max;
    if (g_idle_timeout_idx < 0 || g_idle_timeout_idx >= IDLE_TIMEOUT_COUNT) g_idle_timeout_idx = 2;
    if (g_brightness_idx   < 0 || g_brightness_idx   >= BRIGHTNESS_COUNT)   g_brightness_idx   = BRIGHTNESS_COUNT - 1;
    if (g_standby_brightness_idx < 0) g_standby_brightness_idx = 0;
    if (g_standby_brightness_idx > STANDBY_BRIGHTNESS_MAX_IDX) g_standby_brightness_idx = STANDBY_BRIGHTNESS_MAX_IDX;
    if (g_leveling_drive_db < 0) g_leveling_drive_db = 0;
    if (g_leveling_drive_db > LEVELING_DRIVE_DB_MAX) g_leveling_drive_db = LEVELING_DRIVE_DB_MAX;
    if (g_leveling_release_ds < LEVELING_RELEASE_DS_MIN) g_leveling_release_ds = LEVELING_RELEASE_DS_MIN;
    if (g_leveling_release_ds > LEVELING_RELEASE_DS_MAX) g_leveling_release_ds = LEVELING_RELEASE_DS_MAX;
    if (g_leveling_attack_hms < LEVELING_ATTACK_HMS_MIN) g_leveling_attack_hms = LEVELING_ATTACK_HMS_MIN;
    if (g_leveling_attack_hms > LEVELING_ATTACK_HMS_MAX) g_leveling_attack_hms = LEVELING_ATTACK_HMS_MAX;
    if (g_leveling_lookahead_ms < LEVELING_LOOKAHEAD_MS_MIN) g_leveling_lookahead_ms = LEVELING_LOOKAHEAD_MS_MIN;
    if (g_leveling_lookahead_ms > LEVELING_LOOKAHEAD_MS_MAX) g_leveling_lookahead_ms = LEVELING_LOOKAHEAD_MS_MAX;
    if (g_leveling_ceiling_hdb < LEVELING_CEILING_HDB_MIN) g_leveling_ceiling_hdb = LEVELING_CEILING_HDB_MIN;
    if (g_leveling_ceiling_hdb > LEVELING_CEILING_HDB_MAX) g_leveling_ceiling_hdb = LEVELING_CEILING_HDB_MAX;
    for (int i = 0; i < ALARM_COUNT; ++i) {
        Alarm& a = g_alarms[i];
        if (a.hour > 23)   a.hour = 7;
        if (a.minute > 59) a.minute = 0;
        if (a.ramp_s > 60) a.ramp_s = 0;
        if (a.volume > MAX_VOL) a.volume = 8;
    }
}

static void flushStateIfDirty() {
    if (!g_state_dirty) return;
    if (millis() - g_state_dirty_since_ms < PERSIST_FLUSH_MS) return;
    if (!g_prefs.begin("player", false)) return;  // RW
    g_prefs.putInt   ("vol",    g_volume);
    g_prefs.putInt   ("font",   g_font_notch);
    g_prefs.putBool  ("diag",   g_diagnostics_hidden);
    g_prefs.putBool  ("wrap",   g_wrap_names);
    g_prefs.putBool  ("hidena", g_hide_non_audio);
    g_prefs.putBool  ("autonx", g_auto_play_next);
    g_prefs.putBool  ("autowv", g_auto_waveform);
    g_prefs.putBool  ("autosp", g_auto_spectrum);
    g_prefs.putInt   ("volmax", g_volume_max);
    g_prefs.putInt   ("idleto", g_idle_timeout_idx);
    g_prefs.putInt   ("bright", g_brightness_idx);
    g_prefs.putInt   ("stbybr", g_standby_brightness_idx);
    g_prefs.putBool  ("lvl",    g_leveling_enabled);
    g_prefs.putInt   ("lvldb",  g_leveling_drive_db);
    g_prefs.putInt   ("lvlrel", g_leveling_release_ds);
    g_prefs.putInt   ("lvlat",  g_leveling_attack_hms);
    g_prefs.putInt   ("lvlla",  g_leveling_lookahead_ms);
    g_prefs.putInt   ("lvlce",  g_leveling_ceiling_hdb);
    for (int i = 0; i < ALARM_COUNT; ++i) {
        char k[8];
        snprintf(k, sizeof(k), "a%d_hm", i);
        g_prefs.putUShort(k, (uint16_t)((g_alarms[i].hour << 8) | g_alarms[i].minute));
        snprintf(k, sizeof(k), "a%d_d", i);   g_prefs.putUChar (k, g_alarms[i].days);
        snprintf(k, sizeof(k), "a%d_v", i);   g_prefs.putUChar (k, g_alarms[i].volume);
        snprintf(k, sizeof(k), "a%d_r", i);   g_prefs.putUChar (k, g_alarms[i].ramp_s);
        snprintf(k, sizeof(k), "a%d_e", i);   g_prefs.putUChar (k, g_alarms[i].enabled ? 1 : 0);
        snprintf(k, sizeof(k), "a%d_t", i);   g_prefs.putString(k, g_alarms[i].track.c_str());
    }
    g_prefs.putString("folder", g_cur_path.c_str());
    g_prefs.putInt   ("cursor", g_cursor);
    g_prefs.putString("track",  g_play_path.c_str());
    // Current playhead byte offset, captured under the audio mutex so the
    // audio task can't reconfigure g_src under us.
    uint32_t pos = 0;
    if (g_audio_mutex) {
        xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
        if (g_src) pos = g_src->getPos();
        xSemaphoreGive(g_audio_mutex);
    }
    g_prefs.putUInt  ("trkpos", pos);
    g_prefs.end();
    g_state_dirty = false;
}

// Wipe the saved record and restore in-memory defaults. The reset modal's
// confirm path additionally stops playback and reloads the root directory
// so the device looks as it did at first boot.
static void resetState() {
    if (g_prefs.begin("player", false)) {
        g_prefs.clear();
        g_prefs.end();
    }
    g_state_dirty = false;
    g_volume             = 16;
    g_font_notch         = 1;
    g_diagnostics_hidden = true;
    g_wrap_names         = true;
    g_hide_non_audio     = true;
    g_auto_play_next     = true;
    g_auto_waveform      = false;
    g_auto_spectrum       = false;
    g_volume_max         = 16;
    g_idle_timeout_idx   = 2;
    g_brightness_idx     = BRIGHTNESS_COUNT - 1;
    g_leveling_enabled   = false;
    g_leveling_drive_db  = 12;
    g_leveling_release_ds = 5;
    g_leveling_attack_hms = 2;
    g_leveling_lookahead_ms = 5;
    g_leveling_ceiling_hdb = 2;
    g_cur_path           = "/";
    g_cursor             = 0;
    g_play_path.clear();
    g_saved_playhead = 0;
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
    if (s.count < DIAG_GRAPH_W) {
        s.samples[s.count++] = v;
    } else {
        memmove(s.samples, s.samples + 1, DIAG_GRAPH_W - 1);
        s.samples[DIAG_GRAPH_W - 1] = v;
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
static void drawCpuOverlay(int x, int y, int w, int h,
                           const Sparkline& s0, const Sparkline& s1) {
    auto& d = g_canvas;
    // No background fill — the caller (drawHeader) is the sole owner of
    // the header rectangle and has already cleared it. Filling here would
    // wipe the path/version text we want the sparkline to overlay.
    auto plot = [&](const Sparkline& s, uint16_t c) {
        int start = w - s.count;
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
    // Header area is cleared by drawHeader before this runs; no fill here.
    d.setTextSize(1);
    d.setTextColor(COL_DIAG_TXT, BLACK);

    // Left column: audio / heap percent readouts.
    d.setCursor(DIAG_NUM_COL1_X, DIAG_ROW1_Y);
    d.printf("stk:%d%%", g_diag_stk_pct);
    d.setCursor(DIAG_NUM_COL1_X, DIAG_ROW2_Y);
    d.printf("buf:%d%%", g_diag_buf_pct);
    d.setCursor(DIAG_NUM_COL1_X, DIAG_ROW3_Y);
    d.printf("ram:%d%%", g_diag_ram_pct);
    d.setCursor(DIAG_NUM_COL1_X, DIAG_ROW4_Y);
    d.printf("u:%u", (unsigned)g_diag_underruns);

    // Right column: per-core IPC task stack free bytes (tinted), then
    // idle countdown / IMU motion in the remaining slots. The CPU sparkline
    // to the right of these cells carries the per-core load — these slots
    // used to mirror it as text, but that was redundant.
    d.setTextColor(0x07FF, BLACK);
    d.setCursor(DIAG_NUM_COL2_X, DIAG_ROW1_Y);
    d.printf("i0:%u", (unsigned)g_diag_ipc0_free);
    d.setTextColor(0xFC00, BLACK);
    d.setCursor(DIAG_NUM_COL2_X, DIAG_ROW2_Y);
    d.printf("i1:%u", (unsigned)g_diag_ipc1_free);
    d.setTextColor(COL_DIAG_TXT, BLACK);
    d.setCursor(DIAG_NUM_COL2_X, DIAG_ROW3_Y);
    // `to` — seconds remaining before the idle fade fires. `-` when the
    // backlight-off setting is disabled; "0" once the threshold is past
    // (during the fade or once OFF).
    if (!idleEnabled()) {
        d.printf("to:-");
    } else {
        uint32_t idle = millis() - g_last_activity_ms;
        uint32_t off  = idleOffMs();
        int remain_s = (idle >= off) ? 0 : (int)((off - idle) / 1000);
        d.printf("to:%ds", remain_s);
    }
    d.setCursor(DIAG_NUM_COL2_X, DIAG_ROW4_Y);
    // RTC wall clock — verifies the Port-A HYM8563 is keeping time without
    // needing a serial monitor. Replaces the IMU magnitude readout: motion
    // events that exceed threshold already surface via the `to:` countdown.
    d.print(g_diag_clk);

    // CPU sparkline at full row-2 height — 32 vertical levels for 0–100 %,
    // double the y-resolution of the previous 16 px graph.
    drawCpuOverlay(DIAG_GRAPH_X, DIAG_GRAPH_Y, DIAG_GRAPH_W, DIAG_GRAPH_H,
                   g_spark_cpu0, g_spark_cpu1);
}

static void drawHeaderToCanvas() {
    auto& d = g_canvas;
    d.fillRect(0, 0, SCREEN_W, headerH(), BLACK);

    d.setTextSize(1);
    d.setTextColor(COL_HEADER_TXT, BLACK);

    // Battery icon on the top-left, with the voltage label directly to its
    // right. The "+" terminal nub sits on the right of the icon, facing
    // outward away from the screen edge.
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

    constexpr int VOLT_W = 5 * BASE_CHAR_W;
    int volt_x = BATTERY_ICON_X + ICON_W + 2 + 2;  // icon + nub + gap
    d.setCursor(volt_x, 1);
    d.printf("%d.%02dv",
             g_battery_voltage_mv / 1000,
             (g_battery_voltage_mv % 1000) / 10);

    // Version (boot, until first keypress) sits right-justified at the
    // screen edge; once the user starts navigating, the slot becomes the
    // path breadcrumb, left-justified just after the voltage label so it
    // reads as the orientation strip rather than a header tag. Both in
    // white so they stay legible when the sparkline crosses.
    //
    // While the fuzzy index is being built, a spinner takes the rightmost
    // char cell, so the slot shrinks by one char.
    int slot_left  = volt_x + VOLT_W + 4 + BASE_CHAR_W;  // small gap + one char of breathing room
    int slot_right = SCREEN_W - 1;
    bool indexing  = (FuzzyIndex::state() == FuzzyIndex::State::Building);
    if (indexing) slot_right -= BASE_CHAR_W + 1;
    int slot_w     = slot_right - slot_left;
    int max_chars  = slot_w / BASE_CHAR_W;
    d.setTextColor(COL_FILE_BRIGHT, BLACK);
    if (g_first_keypress_seen) {
        d.setCursor(slot_left, 1);
        d.print(crumbForWidth(g_cur_path, max_chars).c_str());
    } else {
        int len = (int)strlen(APP_VERSION);
        d.setCursor(slot_right - len * BASE_CHAR_W, 1);
        d.print(APP_VERSION);
    }
    if (indexing) {
        static const char SPIN_CHARS[] = {'|', '/', '-', '\\'};
        char ch = SPIN_CHARS[g_indexing_phase & 3];
        d.setCursor(SCREEN_W - 1 - BASE_CHAR_W, 1);
        d.print(ch);
    }
    d.setTextColor(COL_HEADER_TXT, BLACK);

    // Sparkline draws last so it overlays text in its overlap region under
    // high CPU. Header's mid-grey text reads as background against the
    // saturated cyan/orange sparkline lines.
    if (!g_diagnostics_hidden) drawDiagnosticsRow();
}

static void drawHeader() {
    drawHeaderToCanvas();
    presentFrame();
}

// Returns the per-core idle fraction over the interval since the previous
// call, [0..1]. Honest from the first sample — no warmup needed.
static void sampleCpuIdleFractions(float& idle_0, float& idle_1) {
    idle_0 = idle_1 = 0.0f;
    if (!g_idle_task_0 || !g_idle_task_1) return;

    TaskStatus_t info0, info1;
    vTaskGetInfo(g_idle_task_0, &info0, pdFALSE, eRunning);
    vTaskGetInfo(g_idle_task_1, &info1, pdFALSE, eRunning);

    int64_t  now_us  = esp_timer_get_time();
    uint32_t d0      = info0.ulRunTimeCounter - g_idle_us_0_prev;
    uint32_t d1      = info1.ulRunTimeCounter - g_idle_us_1_prev;
    int64_t  dt      = now_us - g_cpu_sample_us_prev;
    g_idle_us_0_prev      = info0.ulRunTimeCounter;
    g_idle_us_1_prev      = info1.ulRunTimeCounter;
    g_cpu_sample_us_prev  = now_us;

    if (dt <= 0) return;
    idle_0 = (float)d0 / (float)dt;
    idle_1 = (float)d1 / (float)dt;
    if (idle_0 > 1.0f) idle_0 = 1.0f;
    if (idle_1 > 1.0f) idle_1 = 1.0f;
}

static void pollDiagnostics() {
    if (g_screen_state == SCREEN_OFF) return;
    uint32_t now = millis();
    if (now - g_diag_last_ms < DIAGNOSTICS_POLL_MS) return;
    g_diag_last_ms = now;

    if (g_audio_task) {
        UBaseType_t hw = uxTaskGetStackHighWaterMark(g_audio_task);
        uint32_t free_bytes = (uint32_t)hw * sizeof(StackType_t);
        g_diag_stack_used = (AUDIO_TASK_STACK > free_bytes)
            ? (AUDIO_TASK_STACK - free_bytes) : 0;
    }
    g_diag_underruns = g_out.underruns();

    g_diag_stk_pct = (int)(100.0f * (float)g_diag_stack_used
                           / (float)AUDIO_TASK_STACK + 0.5f);

    // Buffer headroom: minimum pre-buffer fill since the last sample,
    // as a percentage of the pre-buffer capacity. Better than the
    // previous "playRaw blocking time" snapshot — captures the
    // worst-case headroom in the window rather than a single moment.
    size_t prebuf_min = g_out.prebufMinAndReset();
    g_diag_buf_pct = (int)(100.0f * (float)prebuf_min
                           / (float)g_out.prebufCapacity() + 0.5f);

    uint32_t heap_total = ESP.getHeapSize();
    uint32_t heap_free  = ESP.getFreeHeap();
    g_diag_ram_pct = heap_total > 0
        ? (int)(100.0f * (float)(heap_total - heap_free)
                / (float)heap_total + 0.5f) : 0;

    // Largest free contiguous block as percent of total — fragmentation
    // signal. `ram` may say half-free while the largest run is too small
    // for the next big allocation.
    uint32_t heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    g_diag_largest_pct = heap_total > 0
        ? (int)(100.0f * (float)heap_largest / (float)heap_total + 0.5f) : 0;

    // Minimum-ever-free since boot — peak pressure mark. Reported as
    // percent USED at the worst point, so it reads in the same direction
    // as `ram`: bigger number = closer to OOM at some point.
    uint32_t heap_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    g_diag_minfree_pct = heap_total > 0
        ? (int)(100.0f * (float)(heap_total - heap_min_free)
                / (float)heap_total + 0.5f) : 0;

    float idle0, idle1;
    sampleCpuIdleFractions(idle0, idle1);
    float load0 = 1.0f - idle0;
    float load1 = 1.0f - idle1;
    sparklinePush(g_spark_cpu0, load0);
    sparklinePush(g_spark_cpu1, load1);

    if (g_ipc_task_0) {
        UBaseType_t hw = uxTaskGetStackHighWaterMark(g_ipc_task_0);
        g_diag_ipc0_free = (uint32_t)hw * sizeof(StackType_t);
    }
    if (g_ipc_task_1) {
        UBaseType_t hw = uxTaskGetStackHighWaterMark(g_ipc_task_1);
        g_diag_ipc1_free = (uint32_t)hw * sizeof(StackType_t);
    }

    if (overlayActive() || g_diagnostics_hidden) return;
    // Re-render the entire header so the path/version text is fresh
    // before the sparkline overlays it. During visualisation use a
    // header-rows-only push so we don't shove a full-canvas presentFrame
    // through the display mutex against the vizTask's per-frame push.
    if (g_waveform_active || g_spectrum_active || g_viz_test_pattern) {
        drawHeaderToCanvas();
        presentRows(0, headerH());
    } else {
        drawHeader();
    }
}

static int displayedBatteryLevel(int mv) {
    if (mv <= 0) return -1;
    int level = (mv - LOADED_EMPTY_MV) * 100 / (LOADED_FULL_MV - LOADED_EMPTY_MV);
    if (level < 0)   level = 0;
    if (level > 100) level = 100;
    return level;
}

static void draw();
static void composeBrowser();

static void toggleWaveform() {
    g_waveform_active = !g_waveform_active;
    g_viz_prev_us = 0;            // re-anchor shared cursor on next poll
    g_viz_sprite_dirty = true;    // layout changed, force full redraw
    draw();
}

static void toggleSpectrum() {
    g_spectrum_active = !g_spectrum_active;
    g_viz_prev_us = 0;
    g_viz_sprite_dirty = true;
    draw();
}

static void toggleLeveling() {
    g_leveling_enabled = !g_leveling_enabled;
    g_out.limiter().setEnabled(g_leveling_enabled);
    markStateDirty();
    g_viz_sprite_dirty = true;  // repaint the waveform so the amp trace shows/clears at once
    draw();                     // footer indicator reflects the new state
}

// Last-shown viz combination, restored by `Tab` when no overlay is
// visible. Captured by every dismiss path so the restore matches what was
// on screen at the moment it was cleared. Defaults to both views on so
// the very first Tab press reveals the full viz layout.
static bool g_last_viz_waveform = true;
static bool g_last_viz_spectrum = true;

static void snapshotAndDismissViz() {
    g_last_viz_waveform = g_waveform_active;
    g_last_viz_spectrum = g_spectrum_active;
    g_waveform_active  = false;
    g_spectrum_active  = false;
    g_viz_test_pattern = false;
    draw();
}

static void restoreVizFromSnapshot() {
    g_waveform_active = g_last_viz_waveform;
    g_spectrum_active = g_last_viz_spectrum;
    g_viz_prev_us = 0;
    g_viz_sprite_dirty = true;
    draw();
}

// Chess mode entry / exit. The chess screen owns the panel while active;
// any visualisation overlay is dismissed (and not auto-restored on exit —
// the user brings it back with Tab, matching the snapshotAndDismissViz
// convention).
// A track set aside when entering chess, so it can be reloaded (paused, at the
// same spot) on the way out. Chess's search needs ~24 KB; tearing the decoder
// down frees the memory it would otherwise be competing for.
static std::string g_chess_resume_path;
static uint32_t    g_chess_resume_pos = 0;

static void enterChess() {
    if (g_waveform_active || g_spectrum_active || g_viz_test_pattern) {
        snapshotAndDismissViz();
    }
    chess::enter();
    g_screen = Screen::Chess;
    draw();
}

// Enter chess having first torn the audio decoder down (freeing its memory for
// the chess search), remembering the track + position so it can be restored
// paused on exit. Called once the user confirms the pause.
static void enterChessFreeingAudio() {
    g_chess_resume_path.clear();
    if (!g_play_path.empty()) {
        g_chess_resume_path = g_play_path;
        if (g_audio_mutex) xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
        g_chess_resume_pos = g_src ? g_src->getPos() : 0;
        if (g_audio_mutex) xSemaphoreGive(g_audio_mutex);
    }
    stopPlayback();      // releases the decoder's heap before chess claims its scratchpad
    enterChess();
}

// Ctrl+H entry point: a loaded track means chess would have to pause it to free
// memory, so confirm first; otherwise enter straight away.
static void requestChess() {
    if (!g_play_path.empty()) {
        g_screen = Screen::ChessConfirm;
        drawChessConfirm();
    } else {
        enterChess();
    }
}

static void exitChess() {
    chess::exit();       // frees the ~24 KB search scratchpad first
    g_screen = Screen::Main;
    // Restore the set-aside track, paused at its spot — the user un-pauses to
    // resume. Rebuilt only now that chess has released its memory.
    if (!g_chess_resume_path.empty()) {
        std::string path = g_chess_resume_path;
        uint32_t    pos  = g_chess_resume_pos;
        g_chess_resume_path.clear();
        if (startPlayback(path, /*start_paused=*/true) && pos > 0 && g_src) {
            uint32_t sz = g_src->getSize();
            if (pos >= g_audio_start_offset && pos < sz) seekToByte(pos);
        }
    }
    draw();
}

static void toggleTestPattern() {
    g_viz_test_pattern = !g_viz_test_pattern;
    g_viz_sprite_dirty = true;
    draw();
    presentFrame();
    Serial.printf("test pattern = %d, browserY=%d browserH=%d\n",
                  g_viz_test_pattern ? 1 : 0, browserY(), browserH());
}

// Periodic redraw of the waveform view. Only pushes the browser rows, not
// the full frame — same shape as the marquee partial-push pattern. Skips
// the redraw entirely when the audio output's ring head hasn't moved
// since the last frame, so a frozen waveform (playback stopped, viz left
// on as a static snapshot) costs essentially nothing.
static void setBrightnessRampTo(uint8_t target, uint32_t ramp_ms) {
    if (target == g_brightness_target) return;
    g_brightness_start    = g_brightness_current;
    g_brightness_target   = target;
    g_brightness_start_ms = millis();
    g_brightness_ramp_ms  = ramp_ms;
}

static void pollBrightnessRamp() {
    if (g_brightness_current == g_brightness_target) return;
    uint32_t now = millis();
    uint32_t elapsed = now - g_brightness_start_ms;
    uint8_t next;
    if (g_brightness_ramp_ms == 0 || elapsed >= g_brightness_ramp_ms) {
        next = g_brightness_target;
    } else {
        int32_t delta = (int32_t)g_brightness_target - (int32_t)g_brightness_start;
        int32_t step  = (delta * (int32_t)elapsed) / (int32_t)g_brightness_ramp_ms;
        next = (uint8_t)((int32_t)g_brightness_start + step);
    }
    if (next == g_brightness_current) return;
    g_brightness_current = next;
    M5Cardputer.Display.setBrightness(next);
    // Ramp complete: if we were fading toward zero, the panel is now dark
    // — commit to OFF state so redraws stop until the user wakes us.
    if (g_screen_state == SCREEN_FADING &&
        g_brightness_current == 0 &&
        g_brightness_target  == 0) {
        g_screen_state = SCREEN_OFF;
    }
}

// Record that the user is interacting. Wakes the screen back to full
// brightness (over a short ramp) if it was dimmed or off.
static void markActivity() {
    g_last_activity_ms = millis();
    // Standby and alarm-firing screens own their own brightness — wake-up
    // ramps would fight enterStandby/exitStandby and flash the user.
    if (fullScreenClockActive()) return;
    if (g_screen_state != SCREEN_FULL) {
        bool was_off = (g_screen_state == SCREEN_OFF);
        g_screen_state = SCREEN_FULL;
        setBrightnessRampTo(userBrightness(), RAMP_WAKE_MS);
        if (was_off) {
            draw();   // restore the visible state
        }
    } else if (g_brightness_target != userBrightness()) {
        // FULL state but the ramp is still settling (e.g. the user is in
        // the middle of a fade-out and pressed a key). Retarget to the
        // current brightness setting so we don't drift toward zero.
        setBrightnessRampTo(userBrightness(), RAMP_WAKE_MS);
    }
}

// Sample the IMU; if movement exceeds the threshold versus the previous
// sample, treat it as activity. Polled at ~10 Hz.
static void pollIMUActivity() {
    uint32_t now = millis();
    if (now - g_imu_last_ms < IMU_POLL_MS) return;
    g_imu_last_ms = now;
    M5.Imu.update();
    float ax, ay, az;
    if (!M5.Imu.getAccel(&ax, &ay, &az)) return;
    if (!g_imu_seeded) {
        g_imu_prev_x = ax; g_imu_prev_y = ay; g_imu_prev_z = az;
        g_imu_seeded = true;
        return;
    }
    float dx = ax - g_imu_prev_x;
    float dy = ay - g_imu_prev_y;
    float dz = az - g_imu_prev_z;
    float mag2 = dx * dx + dy * dy + dz * dz;
    g_imu_prev_x = ax; g_imu_prev_y = ay; g_imu_prev_z = az;
    // Expose the motion magnitude for the `im` diagnostics readout — mg.
    g_diag_imu_mg = (int)(sqrtf(mag2) * 1000.0f);
    if (mag2 > IMU_MOTION_THRESHOLD * IMU_MOTION_THRESHOLD) {
        markActivity();
    }
}

// Step the screen state machine based on idle time. State transitions
// kick off a brightness ramp; the brightness poll applies the changes
// frame by frame.
static void pollIdleScreen() {
    if (!idleEnabled()) return;
    if (g_screen_state != SCREEN_FULL) return;
    // Standby and alarm-firing screens own their own brightness; the idle
    // FSM stays out of the way until the user exits them.
    if (fullScreenClockActive()) return;
    uint32_t idle = millis() - g_last_activity_ms;
    if (idle >= idleOffMs()) {
        g_screen_state = SCREEN_FADING;
        setBrightnessRampTo(0, RAMP_OFF_MS);
    }
}

// Forward decls — canvas-side viz helpers defined further down.
static void drawClockScreen();
static void enterAlarms();
static void exitAlarms();
static void enterAlarmEditor(int slot);
static void enterStandby();
static uint8_t dayOfWeekMonFirst(uint16_t y, uint8_t m, uint8_t d);
static void fullRedrawVizIntoCanvas(int by, int inner_h);
static void drawVizColIntoCanvas(int y_top, int inner_h, int x, uint64_t col_abs);

// Wall-clock-paced poll that advances the shared visualisation cursor
// and drives either or both overlays via `composeBrowser`. Both rings
// commit at the same audio rate, so a single cursor is sufficient.
static void pollVisualisation() {
    if (!g_waveform_active && !g_spectrum_active && !g_viz_test_pattern) return;
    if (g_screen_state == SCREEN_OFF) return;
    // The full-screen clock (standby / alarm firing / snooze) owns the whole
    // panel; viz columns would overpaint it. State is left intact so the
    // visualisation resumes when the clock screen exits.
    if (overlayActive()) return;
    uint32_t now_us = micros();
    uint64_t abs    = g_out.spectrumRing().abs_head;

    uint32_t col_period_us = (uint32_t)(
        (uint64_t)1000000 * SPEC_COL_SAMPLES / g_out.sampleRate());
    if (col_period_us < 1) col_period_us = 1;

    // Predicted abs_head: anchored once at activation, then grows
    // linearly via wall-clock at the column rate. Bursts of actual
    // commits don't perturb predicted — that's the smoothing. Capped
    // at `actual + lookahead - 1` so target_max never overshoots a
    // written slot. Drift correction below for sample-rate mismatch.
    uint32_t lookahead_cols = (VIZ_LOOKAHEAD_MS * 1000) / col_period_us;
    double   elapsed_anchor = (double)(uint32_t)(now_us - g_viz_anchor_us);
    double   predicted_abs  = (double)g_viz_anchor_abs
                            + elapsed_anchor / (double)col_period_us;
    double   pred_cap       = (double)abs + (double)lookahead_cols - 1.0;
    if (predicted_abs > pred_cap) predicted_abs = pred_cap;
    // Drift correction: if predicted has fallen far behind actual
    // (e.g. file sample rate didn't match our col_period assumption),
    // re-anchor to catch up. Threshold > any normal burst size.
    if ((double)abs - predicted_abs > VIZ_PRED_LAG_REANCHOR_COLS) {
        g_viz_anchor_abs = abs;
        g_viz_anchor_us  = now_us;
        predicted_abs    = (double)abs;
    }

    double   target_max     = predicted_abs - (double)lookahead_cols;
    if (target_max < 0.0) target_max = 0.0;

    // First poll after activation / track switch — anchor prediction
    // and disp_abs together, full redraw via sprite, push, return.
    if (g_viz_prev_us == 0) {
        g_viz_anchor_abs = abs;
        g_viz_anchor_us  = now_us;
        g_viz_disp_abs   = target_max;
        g_viz_prev_us    = now_us;
        g_viz_last_rendered_abs = (uint64_t)g_viz_disp_abs;
        int inner_h = browserH() - 2;
        int by = browserY();
        // Both canvas-write and push are funnelled through the display
        // mutex (one logical lock). presentRows takes it internally, so
        // we release it before calling presentRows.
        if (g_display_mutex) xSemaphoreTake(g_display_mutex, portMAX_DELAY);
        fullRedrawVizIntoCanvas(by, inner_h);
        if (g_display_mutex) xSemaphoreGive(g_display_mutex);
        presentRows(by + 1, inner_h);
        return;
    }

    uint32_t threshold_us = col_period_us * VIZ_COLS_PER_PUSH;
    if ((int32_t)(now_us - g_viz_prev_us) < (int32_t)threshold_us) return;

    // Deterministic per-render advance: prev_us steps by exactly the
    // threshold (not "now"), and disp_abs grows by exactly the integer
    // VIZ_COLS_PER_PUSH each render. Eliminates the 1-vs-2-pixel chatter
    // that fractional-elapsed / truncation introduced.
    g_viz_prev_us  += threshold_us;
    g_viz_disp_abs += (double)VIZ_COLS_PER_PUSH;

    // Safety: if disp_abs falls far behind target_max (main loop stalled,
    // long burst), snap forward and force full sprite redraw — too many
    // cols missed to recover via scroll-and-draw.
    constexpr double VIZ_LAG_SNAP_COLS = 8.0;
    if (target_max - g_viz_disp_abs > VIZ_LAG_SNAP_COLS) {
        g_viz_disp_abs = target_max;
        g_viz_prev_us  = now_us;
        g_viz_sprite_dirty = true;
    }
    if (g_viz_disp_abs > target_max) g_viz_disp_abs = target_max;

    // Skip the render when the displayed position hasn't changed — e.g.
    // during pause, disp_abs parks at target_max and re-rendering
    // identical pixels just burns CPU.
    uint64_t disp_int = (uint64_t)g_viz_disp_abs;
    if (disp_int == g_viz_last_rendered_abs) return;
    g_viz_last_rendered_abs = disp_int;

    int inner_h = browserH() - 2;
    int by = browserY();

    if (g_display_mutex) xSemaphoreTake(g_display_mutex, portMAX_DELAY);
    if (g_viz_sprite_dirty) {
        fullRedrawVizIntoCanvas(by, inner_h);
    } else {
        // Scroll the canvas's browser-inner block left by VIZ_COLS_PER_PUSH
        // via copyRect (handles overlap correctly), then redraw the
        // newly-exposed rightmost columns.
        g_canvas.copyRect(0, by + 1, SCREEN_W - VIZ_COLS_PER_PUSH, inner_h,
                          VIZ_COLS_PER_PUSH, by + 1);
        uint64_t abs_int = (uint64_t)g_viz_disp_abs;
        for (int x = SCREEN_W - VIZ_COLS_PER_PUSH; x < SCREEN_W; x++) {
            uint64_t col_abs = abs_int - (uint64_t)SCREEN_W + (uint64_t)x;
            drawVizColIntoCanvas(by + 1, inner_h, x, col_abs);
        }
    }
    if (g_display_mutex) xSemaphoreGive(g_display_mutex);

    presentRows(by + 1, inner_h);
}

// Animate the header spinner while a fuzzy-index build is running. Also
// catches the Building→Ready transition with one final redraw so the
// spinner cell clears and the breadcrumb expands back to full width.
static void pollIndexingSpinner() {
    static FuzzyIndex::State last = FuzzyIndex::State::Idle;
    FuzzyIndex::State now_state = FuzzyIndex::state();
    bool transitioned = (now_state != last);
    last = now_state;
    if (now_state == FuzzyIndex::State::Building) {
        uint32_t now = millis();
        if (transitioned || now - g_indexing_last_ms >= 150) {
            g_indexing_last_ms = now;
            g_indexing_phase++;
            if (!overlayActive()) drawHeader();
        }
    } else if (transitioned && !overlayActive()) {
        drawHeader();
    }
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
        if (g_screen != Screen::KeyReference) drawHeader();
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
    // Amber while leveling is active (replaces the old "L" glyph as the on/off
    // cue); otherwise slate-blue while playing, grey while paused / stopped.
    uint16_t col = !playing            ? COL_FOOTER_IDLE
                 : g_leveling_enabled  ? COL_LEVEL_ACCENT
                                       : COL_FOOTER_PROG;
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
    // Two-char numeric to the left of the bar (e.g. "12"), so the user
    // builds muscle memory for their preferred level.
    constexpr int NUM_W = 2 * BASE_CHAR_W + 1;  // "NN" + 1 px gap
    int bar_x = FOOTER_VOL_X + NUM_W;
    int bar_w = FOOTER_VOL_W - NUM_W;
    d.setTextSize(1);
    d.setTextColor(COL_FOOTER_VOL, BLACK);
    d.setCursor(FOOTER_VOL_X, fy + (fh - 8) / 2);
    d.printf("%2d", g_volume);
    d.drawRect(bar_x, by, bar_w, FOOTER_BAR_H, COL_FOOTER_VOL);
    int inner = bar_w - 2;
    int denom = (g_volume_max > 0) ? g_volume_max : 1;
    int w = (g_volume * inner) / denom;
    if (w > inner) w = inner;
    if (w > 0) {
        d.fillRect(bar_x + 1, by + 1, w, FOOTER_BAR_H - 2, COL_FOOTER_VOL);
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
    if (overlayActive()) return;
    if (g_screen_state == SCREEN_OFF) return;
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
        presentRows(footerY(), footerH());
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
        presentRows(footerY(), footerH());
    }
}

static void drawEntry(int x, int col_w, int y, const Entry& e,
                      bool selected, bool wrap) {
    auto& d = g_canvas;
    int cpl   = charsPerLine(col_w);
    int rows  = entryRows(e, col_w, wrap);
    int rh    = rowH();
    int h     = rows * rh;

    // Pick mode (browsing to pick an alarm track) uses the Settings yellow
    // selection so the user sees at a glance that `/` here picks rather than
    // plays — a different affordance for the same key.
    uint16_t sel_bg_col = (g_pick_slot >= 0) ? COL_SETTINGS_SEL_BG : COL_SELECTION_BG;
    uint16_t bg = selected ? sel_bg_col : BLACK;
    uint16_t fg = kindColour(e.kind);

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
                      int cursor, int top) {
    auto& d = g_canvas;
    int by = browserY();
    int bh = browserH();
    d.fillRect(x, by, col_w, bh, BLACK);

    // Reserve a gutter on the right edge for the scrollbar; names wrap and
    // clip against the narrower content width so the thumb never overpaints.
    int content_w = COL_CONTENT_W;

    int y = by;
    // Row by+bh-1 is reserved for the bottom separator (drawn by drawBrowser).
    int y_max = by + bh - 1;
    int rh = rowH();
    bool wrap = g_wrap_names;
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
        bool selected = (i == cursor);
        drawEntry(x, content_w, y, items[i], selected, wrap);
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
            int h = entryRows(g_entries[i], COL_CONTENT_W, g_wrap_names) * rh;
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
    d.fillRect(SCROLLBAR_X, thumb_top, SCROLLBAR_W, thumb_h, browseFrameColor());
}

// Render the browser onto the canvas. Does not push to the panel — callers
// pair this with presentFrame() (full push) or presentRows() (partial push)
// depending on what's actually changed.
static void composeSearchView();
static void composeVisualisationOverlay();

static void composeBrowser() {
    if (g_waveform_active || g_spectrum_active || g_viz_test_pattern) {
        composeVisualisationOverlay();
        return;
    }
    if (g_search_active)   { composeSearchView();   return; }
    auto& d = g_canvas;
    int by = browserY();
    int bh = browserH();
    if (g_entries.empty()) {
        d.fillRect(0, by, SCREEN_W, bh, BLACK);
        d.setFont(notchFont());
        d.setTextSize(notchSize());
        d.setTextColor(COL_FILE_BRIGHT, BLACK);
        d.setCursor(2, by + 2);
        d.print("(empty)");
    } else {
        ensureCursorVisible();
        int visible = drawColumn(0, COL_W, g_entries, g_cursor, g_top);
        drawScrollbar(visible, (int)g_entries.size());
    }
    d.drawFastHLine(0, by,          SCREEN_W, browseFrameColor());
    d.drawFastHLine(0, by + bh - 1, SCREEN_W, browseFrameColor());
}

static void drawBrowser() {
    composeBrowser();
    presentFrame();
}

// Leaf (filename only) of a result path — search shows track names without
// the directory prefix, since the column is too narrow for full paths.
static const char* searchLeaf(const std::string& full) {
    auto slash = full.find_last_of('/');
    return (slash == std::string::npos) ? full.c_str()
                                        : full.c_str() + slash + 1;
}

// Number of rows the result at `i` occupies, taking the wrap-mode toggle
// into account. Single-row when wrap is off; ceil(len / cpl) when on.
static int searchResultRows(int i, int col_w, bool wrap) {
    if (!wrap) return 1;
    const char* leaf = searchLeaf(g_search_paths[i]);
    int n = (int)strlen(leaf);
    int cpl = charsPerLine(col_w);
    if (n <= 0) return 1;
    return (n + cpl - 1) / cpl;
}

static void ensureSearchCursorVisible() {
    int rh = rowH();
    int bh = browserH();
    int list_h = bh - rh - 2;
    int y_max  = list_h;
    if (g_search_cursor < g_search_top) { g_search_top = g_search_cursor; return; }
    while (true) {
        int y = 0;
        bool fits = false;
        for (int i = g_search_top; i <= g_search_cursor; ++i) {
            int h = searchResultRows(i, COL_CONTENT_W, g_wrap_names) * rh;
            if (i == g_search_cursor) { fits = (y + h) <= y_max; break; }
            y += h;
        }
        if (fits || g_search_top >= g_search_cursor) return;
        ++g_search_top;
    }
}

static void composeSearchView() {
    auto& d = g_canvas;
    int by = browserY();
    int bh = browserH();
    int rh = rowH();
    int cw = charW();

    d.fillRect(0, by, SCREEN_W, bh, BLACK);
    d.setFont(notchFont());
    d.setTextSize(notchSize());

    // Input row: prompt + query + caret on the left. "N indexed" is
    // right-justified as a quiet sanity-check on the index size. It stays
    // visible until the typed query approaches within 2 char-widths of
    // the indicator's left edge, at which point it disappears so the
    // query text owns the full row.
    d.setTextColor(COL_SEARCH_PROMPT, BLACK);
    d.setCursor(2, by + 2);
    d.print("> ");
    d.setTextColor(COL_FILE_BRIGHT, BLACK);
    d.print(g_search_query);
    d.setTextColor(COL_SEARCH_PROMPT, BLACK);
    d.print('_');

    char info[24];
    snprintf(info, sizeof(info), "%u indexed", (unsigned)FuzzyIndex::pathCount());
    int info_len_px = (int)strlen(info) * cw;
    int info_x = SCREEN_W - 1 - info_len_px - 2;
    int input_end_px = 2 + (2 + (int)g_search_query.length() + 1) * cw;
    if (input_end_px + 2 * cw <= info_x) {
        d.setTextColor(COL_HEADER_TXT, BLACK);
        d.setCursor(info_x, by + 2);
        d.print(info);
    }

    d.drawFastHLine(0, by + rh, SCREEN_W, COL_HAIRLINE);

    // Result list area.
    int list_y = by + rh + 1;
    int list_h = bh - rh - 2;
    int y_max  = list_y + list_h;

    if (g_search_hits.empty()) {
        d.setTextColor(COL_HEADER_TXT, BLACK);
        d.setCursor(2, list_y + 2);
        if (g_search_query.length() == 0) {
            d.print("(type to search)");
        } else if (FuzzyIndex::state() != FuzzyIndex::State::Active) {
            d.print("(indexing...)");
        } else {
            d.print("(no matches)");
        }
    } else {
        ensureSearchCursorVisible();
        d.setClipRect(0, list_y, COL_CONTENT_W, list_h);
        d.setTextWrap(false, false);
        int y = list_y;
        int cpl = charsPerLine(COL_CONTENT_W);
        int rendered = 0;
        for (int i = g_search_top;
             i < (int)g_search_hits.size() && y < y_max;
             ++i) {
            int rows = searchResultRows(i, COL_CONTENT_W, g_wrap_names);
            int h = rows * rh;
            bool selected = (i == g_search_cursor);
            uint16_t bg = selected ? COL_SEARCH_SEL_BG : BLACK;
            d.fillRect(0, y, COL_CONTENT_W, h, bg);
            d.setTextColor(COL_FILE_NORMAL, bg);
            const char* leaf = searchLeaf(g_search_paths[i]);
            int n = (int)strlen(leaf);
            if (g_wrap_names) {
                for (int row = 0; row < rows; ++row) {
                    int off = row * cpl;
                    int chunk = std::min(cpl, n - off);
                    d.setCursor(1, y + row * rh + 2);
                    for (int k = 0; k < chunk; ++k) d.print(leaf[off + k]);
                }
            } else {
                d.setCursor(1, y + 2);
                d.print(leaf);
            }
            // Hairline above every entry except the first visible — same
            // visual grammar as the browser's row separators.
            if (i > g_search_top) {
                d.drawFastHLine(0, y, COL_CONTENT_W, COL_HEADER_TXT);
            }
            y += h;
            rendered++;
        }
        d.clearClipRect();
        d.setFont(&fonts::Font0);

        // Scrollbar — thumb sized to visible-rendered fraction.
        int total = (int)g_search_hits.size();
        if (total > rendered) {
            int track_h = list_h;
            int thumb_h = (track_h * rendered) / total;
            if (thumb_h < SCROLLBAR_MIN_H) thumb_h = SCROLLBAR_MIN_H;
            if (thumb_h > track_h) thumb_h = track_h;
            int thumb_top = list_y
                + (int)((int64_t)(track_h - thumb_h) * g_search_top
                        / (total - rendered));
            if (thumb_top < list_y) thumb_top = list_y;
            if (thumb_top + thumb_h > list_y + track_h)
                thumb_top = list_y + track_h - thumb_h;
            d.fillRect(SCROLLBAR_X, thumb_top, SCROLLBAR_W,
                       thumb_h, COL_SEARCH_FRAME);
        }
    }

    d.drawFastHLine(0, by,          SCREEN_W, COL_SEARCH_FRAME);
    d.drawFastHLine(0, by + bh - 1, SCREEN_W, COL_SEARCH_FRAME);
}

// Viridis-style colour ramp: 5 hand-picked anchor stops, linearly
// interpolated. Input 0..255 maps to a 16-bit RGB565 colour. Used by the
// spectrum to encode bin intensity. Computed per-pixel (~7 k calls/frame
// at this size, well within budget).
static uint16_t viridis565(uint8_t v) {
    static const uint8_t stops[5][3] = {
        { 68,   1,  84},
        { 59,  82, 139},
        { 33, 145, 140},
        { 94, 201,  98},
        {253, 231,  37},
    };
    int seg = (int)v * 4 / 256;
    if (seg > 3) seg = 3;
    int t = ((int)v * 4) - seg * 256;
    const uint8_t* a = stops[seg];
    const uint8_t* b = stops[seg + 1];
    int r  = a[0] + ((b[0] - a[0]) * t) / 256;
    int g  = a[1] + ((b[1] - a[1]) * t) / 256;
    int bl = a[2] + ((b[2] - a[2]) * t) / 256;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (bl >> 3));
}

// Draws one spectrum column directly into g_canvas at canvas-absolute
// position (x, y_top..y_top+h-1).
static void drawSpectrumColIntoCanvas(int y_top, int h, int x, uint64_t col_abs) {
    if (h <= 0) return;
    const SpectrumRing& ring = g_out.spectrumRing();
    int idx = (int)(col_abs % SPEC_COLS);
    for (int b = 0; b < SPEC_BINS; b++) {
        int yt = y_top + (h * (SPEC_BINS - b - 1)) / SPEC_BINS;
        int yb = y_top + (h * (SPEC_BINS - b))     / SPEC_BINS;
        uint8_t v = ring.intensity[idx][b];
        g_canvas.fillRect(x, yt, 1, yb - yt, viridis565(v));
    }
}

// Draws one waveform column directly into g_canvas (clear + vertical line).
static void drawWaveformColIntoCanvas(int y_top, int h, int x, int64_t col_abs) {
    if (h <= 0) return;
    const int cy     = y_top + h / 2;
    const int half_h = h / 2;
    const int yb_max = y_top + h - 1;
    const WaveformRing& ring = g_out.waveformRing();
    int idx = (int)(((col_abs % WV_COLS) + WV_COLS) % WV_COLS);
    int mn  = ring.min_v[idx];
    int mx  = ring.max_v[idx];
    int y0  = cy + (mn * half_h) / 128;
    int y1  = cy + (mx * half_h) / 128;
    if (y0 < y_top) y0 = y_top;
    if (y1 > yb_max) y1 = yb_max;
    if (y1 < y0)    y1 = y0;
    g_canvas.fillRect(x, y_top, 1, h, BLACK);
    g_canvas.drawFastVLine(x, y0, y1 - y0 + 1, COL_FOOTER_PROG);

    // Loudness-leveling amplification trace: a line across the upper half of
    // the waveform whose height tracks net gain. Scaled to the current drive
    // (full drive ≈ top, falling as the limiter pulls peaks down) rather than a
    // fixed 0…+24 dB span, so the line uses the whole band and its dips — the
    // limiter working — are legible. Drawn as a per-column segment bridging to
    // the previous column so it reads as a continuous scrolling line; same
    // accent colour as the footer cue.
    if (g_leveling_enabled) {
        int drive_db = g_leveling_drive_db > 0 ? g_leveling_drive_db : 1;
        auto traceY = [&](uint8_t a) {
            float ndb  = (float)a * (WV_AMP_MAX_DB / 255.0f);
            float frac = ndb / (float)drive_db;
            if (frac > 1.0f) frac = 1.0f;
            if (frac < 0.0f) frac = 0.0f;
            return cy - (int)(frac * half_h);
        };
        int prev_idx = (int)((((col_abs - 1) % WV_COLS) + WV_COLS) % WV_COLS);
        int cur_y  = traceY(ring.amp[idx]);
        int prev_y = traceY(ring.amp[prev_idx]);
        int lo = (cur_y < prev_y) ? cur_y : prev_y;
        int hi = (cur_y < prev_y) ? prev_y : cur_y;
        if (lo < y_top) lo = y_top;
        if (hi > cy)    hi = cy;
        g_canvas.drawFastVLine(x, lo, hi - lo + 1, COL_LEVEL_ACCENT);
    }
}

// Helper for the current dual / single layout: splits the inner area
// into waveform + spectrum heights. One side is 0 in single-view mode.
static void vizLayout(int inner_h, int& wave_h, int& spec_h) {
    if (g_waveform_active && g_spectrum_active) {
        wave_h = (inner_h * DUAL_WAVEFORM_NUM) / DUAL_DENOM;
        spec_h = inner_h - wave_h;
    } else if (g_waveform_active) {
        wave_h = inner_h;
        spec_h = 0;
    } else {
        wave_h = 0;
        spec_h = inner_h;
    }
}

// Draw one column (both views, layout-aware) into the canvas at x.
// y_top is canvas-absolute top of the browser inner area.
static void drawVizColIntoCanvas(int y_top, int inner_h, int x, uint64_t col_abs) {
    if (g_viz_test_pattern) {
        bool bar = (col_abs % VIZ_TEST_BAR_SPACING) == 0;
        g_canvas.fillRect(x, y_top, 1, inner_h, bar ? (uint16_t)0xFFFF : (uint16_t)BLACK);
        return;
    }
    int wave_h, spec_h;
    vizLayout(inner_h, wave_h, spec_h);
    if (wave_h > 0) drawWaveformColIntoCanvas(y_top, wave_h, x, (int64_t)col_abs);
    if (spec_h > 0) drawSpectrumColIntoCanvas(y_top + wave_h, spec_h, x, col_abs);
}

// Full redraw of all 240 columns from current disp_abs into the canvas
// browser inner. Used on activation, mode change, size change, snap.
static void fullRedrawVizIntoCanvas(int by, int inner_h) {
    g_canvas.fillRect(0, by + 1, SCREEN_W, inner_h, BLACK);
    uint64_t abs = (uint64_t)g_viz_disp_abs;
    for (int x = 0; x < SCREEN_W; x++) {
        uint64_t col_abs = abs - (uint64_t)SCREEN_W + (uint64_t)x;
        drawVizColIntoCanvas(by + 1, inner_h, x, col_abs);
    }
    g_viz_sprite_dirty = false;
}

// Composite of the active visualisation views into the browser slot of
// the canvas. Frame lines drawn directly at top/bottom of the slot.
static void composeVisualisationOverlay() {
    auto& d = g_canvas;
    int by = browserY();
    int bh = browserH();
    int inner_h = bh - 2;

    if (g_display_mutex) xSemaphoreTake(g_display_mutex, portMAX_DELAY);
    fullRedrawVizIntoCanvas(by, inner_h);
    if (g_display_mutex) xSemaphoreGive(g_display_mutex);

    d.drawFastHLine(0, by,          SCREEN_W, COL_BROWSE_FRAME);
    d.drawFastHLine(0, by + bh - 1, SCREEN_W, COL_BROWSE_FRAME);
}

// Push only the y-range covering the previous and new cursor rows. The
// canvas has been fully recomposed by composeBrowser, so unchanged rows
// already match what the panel is showing — pushing them again would be
// wasted SPI bandwidth. Falls back to a full push on edge cases (entries
// empty, cursor not in visible window) where computing the y-range is
// either undefined or not worth the saving.
static void presentCursorRows(int prev_cursor) {
    if (g_entries.empty()) { presentFrame(); return; }
    int by = browserY();
    int bh = browserH();
    int rh = rowH();
    bool wrap = g_wrap_names;
    int y = by;
    int y_max = by + bh - 1;
    int prev_y0 = -1, prev_y1 = -1;
    int new_y0  = -1, new_y1  = -1;
    for (int i = g_top; i < (int)g_entries.size() && y < y_max; ++i) {
        int rows = entryRows(g_entries[i], COL_CONTENT_W, wrap);
        int h = rows * rh;
        if (i == prev_cursor) { prev_y0 = y; prev_y1 = y + h; }
        if (i == g_cursor)    { new_y0  = y; new_y1  = y + h; }
        y += h;
    }
    if (prev_y0 < 0 || new_y0 < 0) { presentFrame(); return; }
    int y0 = std::min(prev_y0, new_y0);
    int y1 = std::max(prev_y1, new_y1);
    // Clamp to the panel.
    if (y1 > by + bh) y1 = by + bh;
    presentRows(y0, y1 - y0);
}

static void draw() {
    if (fullScreenClockActive()) {
        drawClockScreen();
        return;
    }
    if (chess::active()) {
        chess::render(g_canvas);
        presentFrame();
        return;
    }
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
    markStateDirty();
}

static void moveCursor(int delta) {
    if (g_entries.empty()) return;
    int prev_cursor = g_cursor;
    int prev_top    = g_top;
    g_cursor += delta;
    if (g_cursor < 0) g_cursor = 0;
    if (g_cursor >= (int)g_entries.size()) g_cursor = (int)g_entries.size() - 1;
    if (g_cursor != prev_cursor) markStateDirty();
    composeBrowser();
    if (g_top != prev_top) {
        // Page scrolled — every visible row's content shifted; full push.
        presentFrame();
    } else {
        // Cursor moved within the visible page — push only the affected
        // row pair. Most cursor moves take this fast path.
        presentCursorRows(prev_cursor);
    }
}

// Returns the offset of the first audio byte if the file starts with a
// valid ID3v2 tag; returns 0 if absent or malformed.
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
// album art). The decoder otherwise byte-scans through it looking for the
// first audio frame, which costs seconds at SD-read speeds. Seek past the
// tag if valid; otherwise rewind to the start.
static void skipID3v2(AudioFileSource* src) {
    uint32_t skip = id3v2HeaderSize(src);
    bool ok = (skip > 0 && skip < src->getSize());
    src->seek(ok ? skip : 0, SEEK_SET);
}

static bool startPlayback(const std::string& full_path, bool start_paused = false) {
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
    g_out.resetFormatLog();
    if (!gen->begin(src, &g_out)) {
        Serial.printf("decoder begin failed: %s\n", full_path.c_str());
        delete gen;
        delete src;
        return false;
    }

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
    g_audio_active = true;
    g_audio_start_offset = audio_start_offset;
    g_play_path = full_path;
    g_play_dir  = play_dir;
    g_play_entries = std::move(play_entries);
    g_play_idx = play_idx;
    // Setting paused inside the critical section ensures the audio task
    // sees the flag the moment it sees the new generator — no transient
    // burst of audio while the caller catches up after startPlayback
    // returns.
    g_paused = start_paused;
    g_advance_pending = false;
    if (g_audio_mutex) xSemaphoreGive(g_audio_mutex);

    Serial.printf("playing: %s\n", full_path.c_str());
    markStateDirty();
    if (!start_paused) {
        bool changed = false;
        if (g_auto_waveform && !g_waveform_active) { g_waveform_active = true; changed = true; }
        if (g_auto_spectrum  && !g_spectrum_active)  { g_spectrum_active  = true; changed = true; }
        if (changed) {
            g_viz_prev_us = 0;
            draw();
        }
    }
    return true;
}

static void exitPickMode(bool restore_path);

static void activateSelection() {
    if (g_entries.empty()) return;
    const Entry& e = g_entries[g_cursor];
    if (e.kind == KIND_DIR) {
        loadDir(joinPath(g_cur_path, e.name));
        draw();
    } else if (e.kind == KIND_AUDIO) {
        std::string full = joinPath(g_cur_path, e.name);
        if (g_pick_slot >= 0) {
            // Pick-mode commit: set the alarm slot's track to this file and
            // return to the editor, restoring the browser to where it was
            // before the pick — choosing an alarm track is a Settings detour
            // and shouldn't move the user's music-browsing position.
            g_alarms[g_pick_slot].track = full;
            markStateDirty();
            int slot = g_pick_slot;
            exitPickMode(/*restore_path=*/true);
            enterAlarmEditor(slot);
            return;
        }
        // `/` on any audio entry — including the currently-playing one —
        // starts it from the beginning. Pause/resume stays on `space`.
        startPlayback(full);
        drawFooter();
    }
}

static void exitPickMode(bool restore_path) {
    if (g_pick_slot < 0) return;
    int slot = g_pick_slot;
    g_pick_slot = -1;
    if (restore_path) {
        loadDir(g_pick_saved_path);
        g_cursor = g_pick_saved_cursor;
        if (g_cursor >= (int)g_entries.size()) g_cursor = (int)g_entries.size() - 1;
        if (g_cursor < 0) g_cursor = 0;
    }
    enterAlarmEditor(slot);
}

static void searchRunQuery() {
    g_search_hits.clear();
    g_search_paths.clear();
    g_search_cursor = 0;
    g_search_top    = 0;
    if (g_search_query.length() == 0) return;
    // Catch the case where a background build completed mid-search: state
    // moves Ready → Active on next activate() call.
    FuzzyIndex::activate();
    FuzzyIndex::query(g_search_query.c_str(), g_search_hits, 32);
    char buf[256];
    g_search_paths.reserve(g_search_hits.size());
    for (auto& h : g_search_hits) {
        if (FuzzyIndex::lookupPath(h.idx, buf, sizeof(buf))) {
            g_search_paths.emplace_back(buf);
        } else {
            g_search_paths.emplace_back();
        }
    }
}

static void enterSearch(char seed) {
    // Search takes over the browser slot, so dismiss any visualisation
    // overlay first — otherwise search would be live but invisible.
    g_waveform_active = false;
    g_spectrum_active  = false;
    g_search_active   = true;
    g_search_query    = String(seed);
    // Lazy-load the index — ~30 ms SD read pays for itself in ~42 KB of
    // heap available the rest of the time.
    FuzzyIndex::activate();
    searchRunQuery();
    draw();
}

static void exitSearch() {
    g_search_active = false;
    g_search_query  = "";
    g_search_hits.clear();
    g_search_paths.clear();
    g_search_cursor = 0;
    g_search_top    = 0;
    // Free the filter + unigram tables (~42 KB) back to the heap so
    // playback / next-track decoder allocations have room.
    FuzzyIndex::deactivate();
    draw();
}

static void searchAppend(char c) {
    g_search_query += c;
    searchRunQuery();
    drawBrowser();
}

static void searchBackspace() {
    if (g_search_query.length() == 0) { exitSearch(); return; }
    g_search_query.remove(g_search_query.length() - 1);
    searchRunQuery();
    drawBrowser();
}

static void searchMoveCursor(int delta) {
    int n = (int)g_search_hits.size();
    if (n == 0) return;
    int nc = g_search_cursor + delta;
    if (nc < 0) nc = 0;
    if (nc >= n) nc = n - 1;
    if (nc == g_search_cursor) return;
    g_search_cursor = nc;
    drawBrowser();
}

// Activate the currently-selected search result: load the containing
// directory, select the track within it, start playback, and exit search
// so the user lands in normal browser context.
static void searchActivate() {
    if (g_search_hits.empty()) return;
    const std::string& path = g_search_paths[g_search_cursor];
    if (path.empty()) return;
    auto slash = path.find_last_of('/');
    if (slash == std::string::npos) return;
    std::string dir  = (slash == 0) ? "/" : path.substr(0, slash);
    std::string leaf = path.substr(slash + 1);
    loadDir(dir);
    for (int i = 0; i < (int)g_entries.size(); ++i) {
        if (g_entries[i].name == leaf) { g_cursor = i; break; }
    }
    startPlayback(path);
    exitSearch();
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
    // The audio task gates copy() on g_paused, so the player stops feeding
    // the output while paused. setActive(false) would also work but would
    // run end()-style teardown; keeping the flag-gate model preserves
    // resume-from-mid-decoder behaviour.
    // Footer is hidden under an overlay; skip the redraw so we don't paint
    // a flicker through it. Settings does not show playback state.
    if (!overlayActive()) drawFooter();
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
    // Preserve paused state: skipping while paused should leave the
    // next track also paused, ready to resume from the new track's
    // start when the user hits space.
    bool was_paused = g_paused;
    startPlayback(full, was_paused);
    drawFooter();
}

static void seekToByte(uint32_t target) {
    if (!g_audio_mutex) return;
    xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
    if (g_src) g_src->seek(target, SEEK_SET);
    xSemaphoreGive(g_audio_mutex);
    // Visualisation rings are intentionally NOT wiped — existing
    // on-screen content scrolls off naturally as new audio arrives.
}

// Audio range = source size minus the leading metadata (e.g. ID3v2 tag)
// captured at file-open time. Percentage seeks operate on this range so
// that "0%" lands at the first audio byte rather than the file's byte 0,
// which avoids a re-scan through any tag.
static uint32_t audioRangeBytes() {
    return g_src ? ((uint32_t)g_src->getSize() - g_audio_start_offset) : 0;
}

static void jumpToTenth(int digit_index) {
    if (!g_src) return;
    uint64_t range = audioRangeBytes();
    seekToByte(g_audio_start_offset + (uint32_t)(range * digit_index / 10));
    // The 500 ms progress-bar redraw in loop() is gated on `playing`, so
    // a jump while paused would leave the bar stale. Update it here for
    // immediate visual feedback regardless of paused state.
    drawSlotProgress();
    presentRows(footerY(), footerH());
}

static void pollSeekKeys() {
    if (!transportAllowed()) return;  // seek is global transport, sans clock/modal/text
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
    // Update only the progress slot rather than the whole footer — held-
    // seek fires every 100 ms, so cost matters. Works while paused, when
    // the loop()'s 500 ms-cadence redraw wouldn't fire.
    drawSlotProgress();
    presentRows(footerY(), footerH());
}

static void moveSettingsCursor(int delta);
static void adjustSettingsRow(int delta);
static void changeVolume(int delta);
static void drawSettings();

// Auto-repeat for `;` / `.` (move cursor) and `,` / `/` (adjust value)
// while Settings is shown. Mirrors pollBrowserNavKeys' 400 ms initial
// delay then 100 ms repeat. Lets the user race through a 0..64 numeric
// like volume max without 64 individual presses.
static uint32_t g_settings_press_ms = 0;
static uint32_t g_settings_last_ms  = 0;
static char     g_settings_repeat_key = 0;

static void moveAlarmsCursor(int delta);
static void adjustAlarmsRow(int delta);
static void syncClockFromRtc();
static void currentClock(rtc_date_type& d, rtc_time_type& t);
static void enterLeveling();
static void exitLeveling();
static void moveLevelingCursor(int delta);
static void adjustLevelingRow(int delta);
static void activateLevelingRow();
static void moveAlarmEditorCursor(int delta);
static void adjustAlarmEditorRow(int delta);
static void activateAlarmEditorRow();
static void enterAlarmEditor(int slot);
static void moveDaysEditorCursor(int delta);
static void activateDaysEditorRow();
static void exitDaysEditor();
static void enterSetTime();
static void exitSetTime();
static void moveSetTimeCursor(int delta);
static void adjustSetTimeRow(int delta);
static void activateSetTimeRow();

static void pollSettingsKeys() {
    bool menu_screen = g_screen == Screen::Settings || g_screen == Screen::Alarms
        || g_screen == Screen::AlarmEditor || g_screen == Screen::DaysEditor
        || g_screen == Screen::SetTime || g_screen == Screen::Leveling;
    if (!menu_screen) {
        g_settings_press_ms  = 0;
        g_settings_repeat_key = 0;
        return;
    }
    auto state = M5Cardputer.Keyboard.keysState();
    if (state.fn) {
        g_settings_press_ms  = 0;
        g_settings_repeat_key = 0;
        return;
    }
    char active = 0;
    for (char c : state.word) {
        if (c == ';' || c == '.' || c == ',' || c == '/') { active = c; break; }
    }
    if (active == 0 || active != g_settings_repeat_key) {
        // Either nothing held or a different key in this poll than last —
        // resetting the timer means a fresh key gets the initial delay
        // before repeats kick in, not the tail of the previous one.
        g_settings_press_ms   = 0;
        g_settings_repeat_key = active;
        return;
    }
    uint32_t now = millis();
    auto fire = [&]() {
        if (g_screen == Screen::SetTime) {
            if      (active == ';') moveSetTimeCursor(-1);
            else if (active == '.') moveSetTimeCursor(+1);
            else if (active == ',') adjustSetTimeRow(-1);
            else if (active == '/') adjustSetTimeRow(+1);
            return;
        }
        if (g_screen == Screen::DaysEditor) {
            // Only the cursor repeats; auto-repeating a day toggle would
            // flip-flop the bit.
            if      (active == ';') moveDaysEditorCursor(-1);
            else if (active == '.') moveDaysEditorCursor(+1);
        } else if (g_screen == Screen::AlarmEditor) {
            if      (active == ';') moveAlarmEditorCursor(-1);
            else if (active == '.') moveAlarmEditorCursor(+1);
            else if (active == ',') adjustAlarmEditorRow(-1);
            else if (active == '/') adjustAlarmEditorRow(+1);
        } else if (g_screen == Screen::Alarms) {
            if      (active == ';') moveAlarmsCursor(-1);
            else if (active == '.') moveAlarmsCursor(+1);
            else if (active == ',') adjustAlarmsRow(-1);
            else if (active == '/') adjustAlarmsRow(+1);
        } else if (g_screen == Screen::Leveling) {
            if      (active == ';') moveLevelingCursor(-1);
            else if (active == '.') moveLevelingCursor(+1);
            else if (active == ',') adjustLevelingRow(-1);
            else if (active == '/') adjustLevelingRow(+1);
        } else {
            if      (active == ';') moveSettingsCursor(-1);
            else if (active == '.') moveSettingsCursor(+1);
            else if (active == ',') adjustSettingsRow(-1);
            else if (active == '/') adjustSettingsRow(+1);
        }
    };
    if (g_settings_press_ms == 0) {
        // First poll seeing this key — the on-change dispatch has already
        // fired the initial press. Just start the repeat timer.
        g_settings_press_ms = now;
        g_settings_last_ms  = now;
        return;
    }
    if (now - g_settings_press_ms < 400) return;
    if (now - g_settings_last_ms  < 100) return;
    fire();
    g_settings_last_ms = now;
}

static void pollBrowserNavKeys() {
    if (overlayActive()) return;
    auto state = M5Cardputer.Keyboard.keysState();
    if (state.fn) {
        // Fn+;/. has no binding; treat as released so the auto-repeat
        // gate doesn't carry state over from a prior plain hold.
        g_nav_press_ms = 0;
        return;
    }
    int direction = 0;
    for (char c : state.word) {
        if      (c == ';') direction = -1;
        else if (c == '.') direction = +1;
    }
    if (direction == 0) {
        g_nav_press_ms = 0;
        return;
    }
    uint32_t now = millis();
    auto fire = [&]() {
        if (g_search_active) searchMoveCursor(direction);
        else                 moveCursor(direction);
    };
    if (g_nav_press_ms == 0) {
        fire();
        g_nav_press_ms = now;
        g_last_nav_ms = now;
        return;
    }
    if (now - g_nav_press_ms < 400) return;
    if (now - g_last_nav_ms < 100) return;
    fire();
    g_last_nav_ms = now;
}

// Volume auto-repeat. Runs even while Settings is open so the user can
// keep tuning playback alongside settings changes. The reset modal and
// key reference still block — those screens treat any key as an exit.
static uint32_t g_vol_press_ms = 0;
static uint32_t g_vol_last_ms  = 0;

static void pollVolumeKeys() {
    if (g_screen == Screen::ResetModal || g_screen == Screen::KeyReference
        || g_screen == Screen::ChessConfirm) {
        g_vol_press_ms = 0;
        return;
    }
    auto state = M5Cardputer.Keyboard.keysState();
    if (state.fn) {
        g_vol_press_ms = 0;
        return;
    }
    int vol_dir = 0;
    for (char c : state.word) {
        if      (c == '-') vol_dir = -1;
        else if (c == '=') vol_dir = +1;
    }
    if (vol_dir == 0) {
        g_vol_press_ms = 0;
        return;
    }
    uint32_t now = millis();
    if (g_vol_press_ms == 0) {
        // Initial press already fired from on-change dispatch — just seed
        // the timer so repeats kick in after the delay.
        g_vol_press_ms = now;
        g_vol_last_ms  = now;
        return;
    }
    if (now - g_vol_press_ms < 400) return;
    if (now - g_vol_last_ms  < 100) return;
    changeVolume(vol_dir);
    g_vol_last_ms = now;
}

static void changeVolume(int delta) {
    g_volume += delta;
    if (g_volume < 0) g_volume = 0;
    if (g_volume > g_volume_max) g_volume = g_volume_max;
    applyVolume();
    markStateDirty();
    // Footer is hidden behind a full-screen overlay; skip the redraw.
    if (!overlayActive()) drawFooter();
}

// Step user-set brightness. Drives both the Settings adjust path and the
// Fn+=/Fn+_ global shortcut so the two routes stay in lockstep.
static void changeBrightness(int delta) {
    int n = g_brightness_idx + delta;
    if (n < 0) n = 0;
    if (n >= BRIGHTNESS_COUNT) n = BRIGHTNESS_COUNT - 1;
    if (n == g_brightness_idx) return;
    g_brightness_idx = n;
    setBrightnessRampTo(userBrightness(), RAMP_SET_MS);
    markStateDirty();
    if (g_screen == Screen::Settings) drawSettings();
}

static void changeFontNotch(int delta) {
    int n = g_font_notch + delta;
    if (n < 0) n = 0;
    if (n >= FONT_NOTCH_COUNT) n = FONT_NOTCH_COUNT - 1;
    if (n == g_font_notch) return;
    g_font_notch = n;
    markStateDirty();
    draw();
}

static void toggleDiagnostics() {
    g_diagnostics_hidden = !g_diagnostics_hidden;
    g_viz_sprite_dirty = true;  // browser slot just resized
    markStateDirty();
    draw();
}

static void toggleWrapNames() {
    g_wrap_names = !g_wrap_names;
    markStateDirty();
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
    markStateDirty();
    draw();
}

// Key-reference content as (key, desc) entries. An empty `key` marks a
// section header; the `desc` then holds the section name and renders at
// the left margin. Reached from the Settings screen — not by `?` directly
// any more.
struct HelpEntry { const char* key; const char* desc; };
static const HelpEntry HELP_ENTRIES[] = {
    {"",      "Browse"},
    {"; .",   "Up / down"},
    {", /",   "Up lvl / enter"},
    {"`",     "Back / clock"},
    {"'",     "Playing track"},
    {"a-z",   "Search"},
    {"",      "Playback"},
    {"Space", "Pause"},
    {"^[ ]",  "Prev / next"},
    {"[ ]",   "Seek (hold)"},
    {"1..0",  "Jump to %"},
    {"",      "Adjust"},
    {"= -",   "Volume"},
    {"^= -",  "Brightness"},
    {"\\",    "Wrap names"},
    {"",      "Misc (^ = Ctrl)"},
    {"^/",    "Settings"},
    {"^W ^S", "Wave / spectrum"},
    {"^H",    "Chess"},
    {"^D",    "Diagnostics"},
};
static constexpr int HELP_ENTRY_COUNT = (int)(sizeof(HELP_ENTRIES) / sizeof(HELP_ENTRIES[0]));

// Description column starts after enough room for the widest binding key
// at the current font (`Enter` = 5 chars, plus a one-char gap = ~6 chars).
static int helpDescColumnX() { return 2 + 6 * charW(); }

// Number of visual rows one entry occupies, given the description-column
// width budget. Section headers (empty key) are always one row.
static int helpEntryRows(const HelpEntry& e) {
    if (e.key[0] == '\0') return 1;
    int desc_x  = helpDescColumnX();
    int avail_w = SCROLLBAR_X - desc_x;
    int cw      = charW();
    int max_per_row = avail_w / cw;
    int len = (int)strlen(e.desc);
    if (len <= max_per_row) return 1;
    return (len + max_per_row - 1) / max_per_row;
}

static int helpTotalRows() {
    int n = 0;
    for (int i = 0; i < HELP_ENTRY_COUNT; ++i) n += helpEntryRows(HELP_ENTRIES[i]);
    return n;
}

static int helpVisibleRows() { return SCREEN_H / rowH(); }

static int helpMaxTop() {
    return std::max(0, helpTotalRows() - helpVisibleRows());
}

static void drawHelp() {
    auto& d = g_canvas;
    d.fillScreen(BLACK);
    d.setFont(notchFont());
    d.setTextSize(notchSize());
    d.setTextWrap(false, false);
    d.setTextColor(COL_FILE_BRIGHT, BLACK);

    int rh       = rowH();
    int cw       = charW();
    int desc_x   = helpDescColumnX();
    int avail_w  = SCROLLBAR_X - desc_x;
    int max_per_row = avail_w / cw;
    int n_vis    = helpVisibleRows();
    int top      = g_help_top;
    if (top > helpMaxTop()) top = helpMaxTop();
    if (top < 0) top = 0;

    // Walk entries, emitting visual rows from `top` for `n_vis` rows.
    // Each entry's first rendered row gets a hairline above it — except
    // the very first visible row, matching the browser's row separators.
    int row_index = 0;
    int draw_row  = 0;
    for (int i = 0; i < HELP_ENTRY_COUNT && draw_row < n_vis; ++i) {
        const HelpEntry& e = HELP_ENTRIES[i];
        int entry_rows = helpEntryRows(e);
        if (row_index + entry_rows <= top) {
            row_index += entry_rows;
            continue;
        }
        // Render this entry, skipping any internal rows that are above top.
        if (e.key[0] == '\0') {
            if (row_index >= top) {
                int y = draw_row * rh;
                if (draw_row > 0) d.drawFastHLine(0, y, SCREEN_W, COL_HEADER_TXT);
                // Grey strip behind section-header text so it reads as a
                // distinct band. Text re-renders with the same grey bg so
                // each glyph's clear-cell doesn't punch back to black.
                d.fillRect(0, y + 1, SCREEN_W, rh - 1, COL_HAIRLINE);
                d.setTextColor(COL_FILE_BRIGHT, COL_HAIRLINE);
                d.setCursor(2, y + 2);
                d.print(e.desc);
                d.setTextColor(COL_FILE_BRIGHT, BLACK);
                draw_row++;
            }
        } else {
            int desc_len = (int)strlen(e.desc);
            for (int r = 0; r < entry_rows && draw_row < n_vis; ++r) {
                int abs_row = row_index + r;
                if (abs_row < top) continue;
                int y = draw_row * rh;
                // Hairline only above an entry's first row, not its wrap
                // continuations.
                if (r == 0 && draw_row > 0) {
                    d.drawFastHLine(0, y, SCREEN_W, COL_HEADER_TXT);
                }
                if (r == 0) {
                    d.setCursor(2, y + 2);
                    d.print(e.key);
                }
                int seg_start = r * max_per_row;
                int seg_len   = std::min(max_per_row, desc_len - seg_start);
                d.setCursor(desc_x, y + 2);
                for (int k = 0; k < seg_len; ++k) d.print(e.desc[seg_start + k]);
                draw_row++;
            }
        }
        row_index += entry_rows;
    }

    // Scrollbar gutter on the right when the list overflows the screen.
    int total = helpTotalRows();
    if (total > n_vis) {
        int track_h  = SCREEN_H;
        int thumb_h  = (track_h * n_vis) / total;
        if (thumb_h < SCROLLBAR_MIN_H) thumb_h = SCROLLBAR_MIN_H;
        int thumb_top = (int)((int64_t)(track_h - thumb_h) * top
                              / std::max(1, total - n_vis));
        d.fillRect(SCROLLBAR_X, thumb_top, SCROLLBAR_W,
                   thumb_h, COL_BROWSE_FRAME);
    }

    d.setFont(&fonts::Font0);
    presentFrame();
}

static void showHelp() {
    g_screen = Screen::KeyReference;
    g_help_top  = 0;
    drawHelp();
}

static void scrollHelp(int delta) {
    int target = g_help_top + delta;
    int max_top = helpMaxTop();
    if (target < 0) target = 0;
    if (target > max_top) target = max_top;
    if (target == g_help_top) return;
    g_help_top = target;
    drawHelp();
}

static void showResetModal();  // defined below

// -- Settings screen -----------------------------------------------------
// Settings is a full-screen overlay reached by `?`. It surfaces every
// user-tunable behaviour with its current state at a glance, plus a
// handful of actions (rebuild index, reset, key reference).
//
// Rows have three kinds, distinguished by a state glyph:
//   - toggle:  ■ (on) / □ (off)
//   - numeric: current value as text (e.g. "10", "1m")
//   - action:  › ("press enter")
//
// `;` / `.` move the cursor between rows. `enter` toggles or activates.
// `,` / `/` adjust numeric rows while selected. `Fn+\`` or `?` dismisses.

enum SettingsRowId {
    SR_DIAG = 0, SR_WRAP, SR_HIDE_NA, SR_AUTO_NEXT, SR_AUTO_WAVE, SR_AUTO_SPEC,
    SR_FONT, SR_VOLMAX, SR_LEVELING,
    SR_BRIGHT, SR_IDLE, SR_CHESS_LEVEL,
    SR_ALARMS, SR_KEY_REF, SR_REBUILD, SR_RESET,
    SR_COUNT
};

static const char* CHESS_LEVEL_LABELS[3] = { "Easy", "Medium", "Hard" };
enum SettingsRowKind { SRK_TOGGLE, SRK_NUMERIC, SRK_ACTION };

struct SettingsRow {
    SettingsRowKind kind;
    const char*     name;
};

static const SettingsRow SETTINGS_ROWS[SR_COUNT] = {
    {SRK_TOGGLE,  "Diagnostics"},
    {SRK_TOGGLE,  "Wrap names"},
    {SRK_TOGGLE,  "Hide non-audio"},
    {SRK_TOGGLE,  "Auto-play next"},
    {SRK_TOGGLE,  "Auto waveform"},
    {SRK_TOGGLE,  "Auto spectrum"},
    {SRK_NUMERIC, "Font size"},
    {SRK_NUMERIC, "Volume max"},
    {SRK_ACTION,  "Leveling..."},
    {SRK_NUMERIC, "Brightness"},
    {SRK_NUMERIC, "Backlight off"},
    {SRK_NUMERIC, "Chess level"},
    {SRK_ACTION,  "Alarms..."},
    {SRK_ACTION,  "Key reference"},
    {SRK_ACTION,  "Rebuild index"},
    {SRK_ACTION,  "Reset all"},
};

static bool settingsRowBool(SettingsRowId id) {
    switch (id) {
        case SR_DIAG:      return !g_diagnostics_hidden;
        case SR_WRAP:      return g_wrap_names;
        case SR_HIDE_NA:   return g_hide_non_audio;
        case SR_AUTO_NEXT: return g_auto_play_next;
        case SR_AUTO_WAVE: return g_auto_waveform;
        case SR_AUTO_SPEC: return g_auto_spectrum;
        default:           return false;
    }
}

static void settingsRowNumStr(SettingsRowId id, char* buf, size_t buflen) {
    switch (id) {
        case SR_FONT:   snprintf(buf, buflen, "%d", g_font_notch); break;
        case SR_VOLMAX: snprintf(buf, buflen, "%d", g_volume_max); break;
        case SR_BRIGHT: snprintf(buf, buflen, "%d", userBrightness()); break;
        case SR_IDLE:
            snprintf(buf, buflen, "%s", IDLE_TIMEOUT_LABELS[g_idle_timeout_idx]);
            break;
        case SR_CHESS_LEVEL:
            snprintf(buf, buflen, "%s", CHESS_LEVEL_LABELS[chess::getDifficulty()]);
            break;
        default: buf[0] = '\0'; break;
    }
}

static void drawSettings() {
    auto& d = g_canvas;
    d.fillScreen(BLACK);
    d.setFont(notchFont());
    d.setTextSize(notchSize());
    d.setTextWrap(false, false);

    int rh = rowH();
    int cw = charW();
    int n_vis = SCREEN_H / rh;
    if (n_vis < 1) n_vis = 1;

    // Reserve top row for the title strip; rows render below it.
    int rows_avail = n_vis - 1;
    if (rows_avail < 1) rows_avail = 1;

    // Keep cursor visible.
    static int s_top = 0;
    if (g_settings_cursor < s_top) s_top = g_settings_cursor;
    if (g_settings_cursor >= s_top + rows_avail) {
        s_top = g_settings_cursor - rows_avail + 1;
    }

    // Title strip.
    d.fillRect(0, 0, SCREEN_W, rh, COL_HAIRLINE);
    d.setTextColor(COL_FILE_BRIGHT, COL_HAIRLINE);
    d.setCursor(2, 2);
    d.print("Settings");
    d.setTextColor(COL_FILE_BRIGHT, BLACK);

    int y = rh;
    constexpr int RIGHT_MARGIN_PX = 4;  // breathing room from the right edge

    for (int row_idx = 0; row_idx < SR_COUNT && (row_idx - s_top) < rows_avail; ++row_idx) {
        if (row_idx < s_top) continue;
        int draw_y = y + (row_idx - s_top) * rh;
        bool selected = (row_idx == g_settings_cursor);

        // Settings has its own selection colour (yellow) so the mode is
        // unambiguous — browser is blue, search is green, settings is yellow.
        if (selected) {
            d.fillRect(0, draw_y, SCREEN_W, rh, COL_SETTINGS_SEL_BG);
        }
        uint16_t bg = selected ? COL_SETTINGS_SEL_BG : BLACK;
        d.setTextColor(COL_FILE_BRIGHT, bg);

        const SettingsRow& r = SETTINGS_ROWS[row_idx];

        // Name on the left.
        d.setCursor(2, draw_y + 2);
        d.print(r.name);

        // State glyph / value right-justified. Width depends on the
        // row's kind: toggles render as "[x]" / "[ ]", numerics as their
        // value, actions as ">". Right-aligned avoids overlap with names
        // whose length varies.
        char buf[8];
        const char* state_str = buf;
        if (r.kind == SRK_TOGGLE) {
            state_str = settingsRowBool((SettingsRowId)row_idx) ? "[x]" : "[ ]";
        } else if (r.kind == SRK_NUMERIC) {
            settingsRowNumStr((SettingsRowId)row_idx, buf, sizeof(buf));
        } else {
            state_str = ">";
        }
        int state_w = (int)strlen(state_str) * cw;
        // Leave a gutter for the scrollbar when one is drawn, so the value
        // column doesn't overlap the thumb.
        int gutter = (SR_COUNT > rows_avail) ? (SCROLLBAR_W + 2) : RIGHT_MARGIN_PX;
        int state_x = SCREEN_W - gutter - state_w;
        d.setCursor(state_x, draw_y + 2);
        d.print(state_str);
    }

    // Scrollbar gutter when rows exceed the viewport. Track spans the
    // rows region (everything below the title strip); thumb size and
    // position track which rows are visible.
    if (SR_COUNT > rows_avail) {
        int track_y = rh;
        int track_h = SCREEN_H - rh;
        int thumb_h = (track_h * rows_avail) / SR_COUNT;
        if (thumb_h < SCROLLBAR_MIN_H) thumb_h = SCROLLBAR_MIN_H;
        int thumb_top = track_y + (int)((int64_t)(track_h - thumb_h) * s_top
                                        / std::max(1, SR_COUNT - rows_avail));
        d.fillRect(SCREEN_W - SCROLLBAR_W, thumb_top, SCROLLBAR_W,
                   thumb_h, COL_SETTINGS_SEL_BG);
    }

    d.setFont(&fonts::Font0);
    presentFrame();
}

static void showSettings() {
    g_screen = Screen::Settings;
    drawSettings();
}

static void dismissSettings() {
    g_screen = Screen::Main;
    draw();
}

static void moveSettingsCursor(int delta) {
    int target = ((g_settings_cursor + delta) % SR_COUNT + SR_COUNT) % SR_COUNT;
    if (target == g_settings_cursor) return;
    g_settings_cursor = target;
    drawSettings();
}

static void activateSettingsRow() {
    SettingsRowId id = (SettingsRowId)g_settings_cursor;
    const SettingsRow& r = SETTINGS_ROWS[id];
    if (r.kind == SRK_TOGGLE) {
        switch (id) {
            case SR_DIAG:      toggleDiagnostics(); break;
            case SR_WRAP:      toggleWrapNames();   break;
            case SR_HIDE_NA:
                g_hide_non_audio = !g_hide_non_audio;
                markStateDirty();
                // Re-scan current folder so the filter takes effect now.
                loadDir(g_cur_path);
                break;
            case SR_AUTO_NEXT:
                g_auto_play_next = !g_auto_play_next;
                markStateDirty();
                break;
            case SR_AUTO_WAVE:
                g_auto_waveform = !g_auto_waveform;
                markStateDirty();
                break;
            case SR_AUTO_SPEC:
                g_auto_spectrum = !g_auto_spectrum;
                markStateDirty();
                break;
            default: break;
        }
        drawSettings();
    } else if (r.kind == SRK_ACTION) {
        switch (id) {
            case SR_KEY_REF:
                g_screen    = Screen::KeyReference;
                g_help_top  = 0;
                drawHelp();
                break;
            case SR_REBUILD:
                FuzzyIndex::startRebuild();
                drawSettings();
                break;
            case SR_ALARMS:
                enterAlarms();
                break;
            case SR_LEVELING:
                enterLeveling();
                break;
            case SR_RESET:
                // Reset modal renders over the settings screen; on dismiss
                // we'll redraw whichever overlay is still active.
                showResetModal();
                break;
            default: break;
        }
    }
    // SRK_NUMERIC rows ignore enter — adjusted via , / / instead.
}

static void adjustSettingsRow(int delta) {
    SettingsRowId id = (SettingsRowId)g_settings_cursor;
    const SettingsRow& r = SETTINGS_ROWS[id];
    // Left/right also act on toggles — `,` turns off, `/` turns on. Lets the
    // user think of every row as adjustable with the same key pair, no
    // separate "enter to flip a bool" rule to remember.
    if (r.kind == SRK_TOGGLE) {
        bool want_on = (delta > 0);
        if (settingsRowBool(id) == want_on) return;
        activateSettingsRow();  // reuses the toggle path and redraws
        return;
    }
    if (r.kind == SRK_ACTION) {
        // `/` activates action rows; `,` is a no-op (actions have no
        // sense of "off").
        if (delta > 0) activateSettingsRow();
        return;
    }
    if (r.kind != SRK_NUMERIC) return;
    switch (id) {
        case SR_FONT: changeFontNotch(delta); break;
        case SR_BRIGHT:
            changeBrightness(delta);
            return;  // changeBrightness already redrew settings
        case SR_VOLMAX: {
            int n = g_volume_max + delta;
            if (n < VOLUME_MAX_MIN) n = VOLUME_MAX_MIN;
            if (n > MAX_VOL) n = MAX_VOL;
            if (n == g_volume_max) return;
            g_volume_max = n;
            // Clamp live volume down so the new ceiling is respected
            // immediately — otherwise the cap wouldn't bite until the
            // user next pressed `=`.
            if (g_volume > g_volume_max) {
                g_volume = g_volume_max;
                applyVolume();
            }
            markStateDirty();
            break;
        }
        case SR_IDLE: {
            int n = g_idle_timeout_idx + delta;
            if (n < 0) n = 0;
            if (n >= IDLE_TIMEOUT_COUNT) n = IDLE_TIMEOUT_COUNT - 1;
            if (n == g_idle_timeout_idx) return;
            g_idle_timeout_idx = n;
            markStateDirty();
            break;
        }
        case SR_CHESS_LEVEL: {
            int n = (int)chess::getDifficulty() + delta;
            if (n < chess::EASY) n = chess::EASY;
            if (n > chess::HARD) n = chess::HARD;
            if (n == (int)chess::getDifficulty()) return;
            chess::setDifficulty((chess::Difficulty)n);
            break;
        }
        default: return;
    }
    drawSettings();
}

static void drawResetModal() {
    auto& d = g_canvas;
    d.fillScreen(BLACK);
    d.setTextSize(2);
    constexpr int CELL_W = 12, CELL_H = 16;
    auto drawLine = [&](const char* s, int y, uint16_t fg) {
        d.setTextColor(fg, BLACK);
        int w = (int)strlen(s) * CELL_W;
        d.setCursor((SCREEN_W - w) / 2, y);
        d.print(s);
    };
    int y = 18;
    drawLine("Reset settings?", y, COL_FILE_BRIGHT);  y += CELL_H + 14;
    drawLine("Enter = yes",     y, COL_FILE_BRIGHT);  y += CELL_H + 4;
    drawLine("other = no",      y, COL_HEADER_TXT);
    presentFrame();
}

static void showResetModal() {
    g_modal_return = g_screen;
    g_screen = Screen::ResetModal;
    drawResetModal();
}

// Confirm gate before chess takes over: chess needs ~24 KB the playing track is
// holding, so it must pause to free it.
static void drawChessConfirm() {
    auto& d = g_canvas;
    d.fillScreen(BLACK);
    d.setTextSize(2);
    constexpr int CELL_W = 12, CELL_H = 16;
    auto drawLine = [&](const char* s, int y, uint16_t fg) {
        d.setTextColor(fg, BLACK);
        d.setCursor((SCREEN_W - (int)strlen(s) * CELL_W) / 2, y);
        d.print(s);
    };
    int y = 14;
    drawLine("Pause playback",  y, COL_FILE_BRIGHT);  y += CELL_H + 2;
    drawLine("for chess?",      y, COL_FILE_BRIGHT);  y += CELL_H + 14;
    drawLine("Enter = yes",     y, COL_FILE_BRIGHT);  y += CELL_H + 4;
    drawLine("other = no",      y, COL_HEADER_TXT);
    presentFrame();
}

// Reset confirmed: wipe NVS, reset in-memory state, stop playback, return
// to root, dismiss the modal and any open settings, redraw.
static void confirmReset() {
    g_screen = Screen::Main;
    stopPlayback();
    resetState();
    loadDir(g_cur_path);  // already "/" after resetState
    applyVolume();
    applyLeveling();
    draw();
}

static void dismissResetModal() {
    goToScreen(g_modal_return);
}

// -- Alarms sub-menu -----------------------------------------------------
// One row per "thing the user might want to touch":
//   AR_SET_TIME            — opens the time/date editor (TODO: editor)
//   AR_ALARM_0 .. AR_ALARM_4 — five slot rows; selecting one opens the
//                            alarm editor (TODO: editor)
//   AR_STANDBY             — standby brightness numeric (functional now)
//   AR_BACK                — explicit return to Settings
enum AlarmsRowId {
    AR_SET_TIME = 0,
    AR_ALARM_0, AR_ALARM_1, AR_ALARM_2, AR_ALARM_3, AR_ALARM_4,
    AR_STANDBY, AR_BACK,
    AR_COUNT
};

static const char DOW_LETTERS[7] = { 'M','T','W','T','F','S','S' };

// "MTWTFSS" with an underscore for each off day — the day-set summary shared
// by the Alarms list and the alarm editor's Days row.
static void formatDaysMask(uint8_t days, char buf[8]) {
    for (int i = 0; i < 7; ++i) buf[i] = (days & (1 << i)) ? DOW_LETTERS[i] : '_';
    buf[7] = '\0';
}

static void alarmsRowLabel(int row, char* buf, size_t buflen) {
    switch (row) {
        case AR_SET_TIME: snprintf(buf, buflen, "Set current time"); return;
        case AR_STANDBY:  snprintf(buf, buflen, "Clock brightness"); return;
        case AR_BACK:     snprintf(buf, buflen, "Back"); return;
        default: {
            int slot = row - AR_ALARM_0;
            const Alarm& a = g_alarms[slot];
            char days[8];
            formatDaysMask(a.days, days);
            snprintf(buf, buflen, "%02u:%02u %s", a.hour, a.minute, days);
            return;
        }
    }
}

static void alarmsRowValueStr(int row, char* buf, size_t buflen) {
    switch (row) {
        case AR_SET_TIME: snprintf(buf, buflen, ">"); return;
        case AR_STANDBY:  snprintf(buf, buflen, "%d", standbyBrightness()); return;
        case AR_BACK:     snprintf(buf, buflen, ""); return;
        default:          snprintf(buf, buflen, ">"); return;
    }
}

// With no RTC there's no timekeeping, so every alarms row is hidden — the
// screen is a bare warning, exited with `` ` `` / Del.
static bool alarmsRowShown(int row) {
    (void)row;
    return g_rtc_present;
}

static void drawAlarms() {
    auto& d = g_canvas;
    d.fillScreen(BLACK);
    d.setFont(notchFont());
    d.setTextSize(notchSize());
    d.setTextWrap(false, false);
    int rh = rowH();
    int cw = charW();
    int n_vis = SCREEN_H / rh;
    if (n_vis < 1) n_vis = 1;
    int rows_avail = n_vis - 1;
    if (rows_avail < 1) rows_avail = 1;

    static int s_top = 0;
    if (g_alarms_cursor < s_top) s_top = g_alarms_cursor;
    if (g_alarms_cursor >= s_top + rows_avail) s_top = g_alarms_cursor - rows_avail + 1;
    if (s_top + rows_avail > AR_COUNT) s_top = AR_COUNT - rows_avail;
    if (s_top < 0) s_top = 0;

    // Title strip.
    d.setTextColor(COL_HEADER_TXT, BLACK);
    d.setCursor(2, 0);
    d.print("Alarms");

    // No RTC: nothing here works, so the screen is purely a warning. Exit with
    // `` ` `` / Del — no rows to navigate.
    if (!g_rtc_present) {
        d.setTextColor(COL_WARN, BLACK);
        d.setCursor(2, rh * 2);
        d.print("RTC not connected");
        d.setCursor(2, rh * 3);
        d.print("Alarms unavailable");
        presentFrame();
        return;
    }

    for (int i = 0; i < rows_avail && (s_top + i) < AR_COUNT; ++i) {
        int row = s_top + i;
        int y = rh + i * rh;
        bool selected = (row == g_alarms_cursor);
        bool armed_slot = (row >= AR_ALARM_0 && row <= AR_ALARM_4)
                          && g_alarms[row - AR_ALARM_0].enabled;
        uint16_t fg;
        if (row >= AR_ALARM_0 && row <= AR_ALARM_4) {
            // Slot rows colour-code by armed state — pink-magenta when
            // enabled (distinct from yellow selection bar and the cyan used
            // for directories elsewhere), mid-grey when off. The same text
            // colour is used selected or not, so the active/inactive signal
            // survives the highlight.
            fg = armed_slot ? 0xFA1F : COL_HEADER_TXT;
        } else {
            fg = COL_FILE_BRIGHT;
        }
        if (selected) {
            d.fillRect(0, y, SCREEN_W, rh, COL_SETTINGS_SEL_BG);
            // Inactive slot text in mid-grey washes out against the yellow
            // selection bar; use black for selected+inactive so the contrast
            // holds. Active slots keep their pink text (already high-contrast
            // against yellow).
            uint16_t sel_fg = (row >= AR_ALARM_0 && row <= AR_ALARM_4 && !armed_slot)
                              ? 0x0000 : fg;
            d.setTextColor(sel_fg, COL_SETTINGS_SEL_BG);
        } else {
            d.setTextColor(fg, BLACK);
        }
        char name[24];
        alarmsRowLabel(row, name, sizeof(name));
        d.setCursor(2, y);
        d.print(name);
        char val[12];
        alarmsRowValueStr(row, val, sizeof(val));
        int vw = (int)strlen(val) * cw;
        d.setCursor(SCREEN_W - vw - 2, y);
        d.print(val);
    }
    presentFrame();
}

static void enterAlarms() {
    g_screen = Screen::Alarms;
    g_alarms_cursor = 0;
    drawAlarms();
}

static void exitAlarms() {
    goToScreen(Screen::Settings);
}

static void moveAlarmsCursor(int delta) {
    int n = g_alarms_cursor;
    do {
        n = ((n + delta) % AR_COUNT + AR_COUNT) % AR_COUNT;
    } while (!alarmsRowShown(n) && n != g_alarms_cursor);
    if (n == g_alarms_cursor) return;
    g_alarms_cursor = n;
    drawAlarms();
}

static void activateAlarmsRow() {
    int row = g_alarms_cursor;
    if (row == AR_BACK) { exitAlarms(); return; }
    if (row == AR_SET_TIME) {
        enterSetTime();
        return;
    }
    if (row >= AR_ALARM_0 && row <= AR_ALARM_4) {
        enterAlarmEditor(row - AR_ALARM_0);
        return;
    }
    // AR_STANDBY is numeric; enter is a no-op.
}

static void adjustAlarmsRow(int delta) {
    int row = g_alarms_cursor;
    if (row == AR_STANDBY) {
        int n = g_standby_brightness_idx + delta;
        if (n < 0) n = 0;
        if (n > STANDBY_BRIGHTNESS_MAX_IDX) n = STANDBY_BRIGHTNESS_MAX_IDX;
        if (n == g_standby_brightness_idx) return;
        g_standby_brightness_idx = n;
        markStateDirty();
        drawAlarms();
        return;
    }
    if (delta > 0) activateAlarmsRow();  // `/` activates action rows
}

// -- Leveling sub-menu ---------------------------------------------------
// Loudness leveling lives on its own screen rather than as flat Settings
// rows so the tuning knobs and a "Reset to default" action group together.
// Same nav idiom as Settings/Alarms.
enum LevelingRowId {
    LV_ENABLED = 0,
    LV_DRIVE, LV_RELEASE, LV_ATTACK, LV_LOOKAHEAD, LV_CEILING,
    LV_RESET, LV_BACK,
    LV_COUNT
};

static void levelingRowLabel(int row, char* buf, size_t buflen) {
    switch (row) {
        case LV_ENABLED:   snprintf(buf, buflen, "Leveling");        return;
        case LV_DRIVE:     snprintf(buf, buflen, "Drive gain");      return;
        case LV_RELEASE:   snprintf(buf, buflen, "Release");         return;
        case LV_ATTACK:    snprintf(buf, buflen, "Attack");          return;
        case LV_LOOKAHEAD: snprintf(buf, buflen, "Lookahead");       return;
        case LV_CEILING:   snprintf(buf, buflen, "Ceiling");         return;
        case LV_RESET:     snprintf(buf, buflen, "Reset to default");return;
        case LV_BACK:      snprintf(buf, buflen, "Back");            return;
    }
}

static void levelingRowValueStr(int row, char* buf, size_t buflen) {
    switch (row) {
        case LV_ENABLED: snprintf(buf, buflen, "[%c]", g_leveling_enabled ? 'x' : ' '); return;
        case LV_DRIVE:   snprintf(buf, buflen, "+%d dB", g_leveling_drive_db); return;
        case LV_RELEASE: snprintf(buf, buflen, "%d.%d s",
                                  g_leveling_release_ds / 10, g_leveling_release_ds % 10); return;
        case LV_ATTACK:  snprintf(buf, buflen, "%d.%d ms",
                                  g_leveling_attack_hms / 2, (g_leveling_attack_hms % 2) * 5); return;
        case LV_LOOKAHEAD: snprintf(buf, buflen, "%d ms", g_leveling_lookahead_ms); return;
        case LV_CEILING: snprintf(buf, buflen, "-%d.%d dB",
                                  g_leveling_ceiling_hdb / 2, (g_leveling_ceiling_hdb % 2) * 5); return;
        case LV_RESET:   snprintf(buf, buflen, ">"); return;
        case LV_BACK:    buf[0] = '\0'; return;
    }
}

static void drawLeveling() {
    auto& d = g_canvas;
    d.fillScreen(BLACK);
    d.setFont(notchFont());
    d.setTextSize(notchSize());
    d.setTextWrap(false, false);
    int rh = rowH();
    int cw = charW();

    // Scroll to keep the cursor visible — the knob list can exceed the screen
    // at the larger font. One row reserved for the title strip.
    int rows_avail = SCREEN_H / rh - 1;
    if (rows_avail < 1) rows_avail = 1;
    static int s_top = 0;
    if (g_leveling_cursor < s_top) s_top = g_leveling_cursor;
    if (g_leveling_cursor >= s_top + rows_avail) s_top = g_leveling_cursor - rows_avail + 1;
    if (s_top + rows_avail > LV_COUNT) s_top = LV_COUNT - rows_avail;
    if (s_top < 0) s_top = 0;

    d.setTextColor(COL_HEADER_TXT, BLACK);
    d.setCursor(2, 0);
    d.print("Leveling");

    for (int i = 0; i < rows_avail && (s_top + i) < LV_COUNT; ++i) {
        int row = s_top + i;
        int y = rh + i * rh;
        bool selected = (row == g_leveling_cursor);
        if (selected) {
            d.fillRect(0, y, SCREEN_W, rh, COL_SETTINGS_SEL_BG);
            d.setTextColor(COL_FILE_BRIGHT, COL_SETTINGS_SEL_BG);
        } else {
            d.setTextColor(COL_FILE_BRIGHT, BLACK);
        }
        char name[20];
        levelingRowLabel(row, name, sizeof(name));
        d.setCursor(2, y);
        d.print(name);
        char val[12];
        levelingRowValueStr(row, val, sizeof(val));
        int vw = (int)strlen(val) * cw;
        d.setCursor(SCREEN_W - vw - 2, y);
        d.print(val);
    }
    presentFrame();
}

static void enterLeveling() {
    g_screen = Screen::Leveling;
    g_leveling_cursor = 0;
    drawLeveling();
}

static void exitLeveling() {
    goToScreen(Screen::Settings);
}

static void moveLevelingCursor(int delta) {
    int n = ((g_leveling_cursor + delta) % LV_COUNT + LV_COUNT) % LV_COUNT;
    if (n == g_leveling_cursor) return;
    g_leveling_cursor = n;
    drawLeveling();
}

// Restore the tuning knobs to their factory values, leaving the on/off
// state as the user set it — they're in here to re-tune, not to disarm.
static void resetLevelingToDefault() {
    g_leveling_drive_db   = 12;
    g_leveling_release_ds = 5;
    g_leveling_attack_hms = 2;
    g_leveling_lookahead_ms = 5;
    g_leveling_ceiling_hdb = 2;
    applyLeveling();
    markStateDirty();
    drawLeveling();
}

static void activateLevelingRow() {
    switch (g_leveling_cursor) {
        case LV_ENABLED:
            g_leveling_enabled = !g_leveling_enabled;
            g_out.limiter().setEnabled(g_leveling_enabled);
            markStateDirty();
            drawLeveling();
            break;
        case LV_RESET: resetLevelingToDefault(); break;
        case LV_BACK:  exitLeveling(); break;
        default: break;  // numeric rows adjust via , / /
    }
}

static void adjustLevelingRow(int delta) {
    switch (g_leveling_cursor) {
        case LV_ENABLED: {
            bool want_on = (delta > 0);
            if (g_leveling_enabled == want_on) return;
            activateLevelingRow();  // reuses the toggle path
            return;
        }
        case LV_DRIVE: {
            int n = g_leveling_drive_db + delta;
            if (n < 0) n = 0;
            if (n > LEVELING_DRIVE_DB_MAX) n = LEVELING_DRIVE_DB_MAX;
            if (n == g_leveling_drive_db) return;
            g_leveling_drive_db = n;
            g_out.limiter().setDriveDb((float)g_leveling_drive_db);
            markStateDirty();
            break;
        }
        case LV_RELEASE: {
            int n = g_leveling_release_ds + delta;
            if (n < LEVELING_RELEASE_DS_MIN) n = LEVELING_RELEASE_DS_MIN;
            if (n > LEVELING_RELEASE_DS_MAX) n = LEVELING_RELEASE_DS_MAX;
            if (n == g_leveling_release_ds) return;
            g_leveling_release_ds = n;
            g_out.limiter().setReleaseSeconds((float)g_leveling_release_ds / 10.0f);
            markStateDirty();
            break;
        }
        case LV_ATTACK: {
            int n = g_leveling_attack_hms + delta;
            if (n < LEVELING_ATTACK_HMS_MIN) n = LEVELING_ATTACK_HMS_MIN;
            if (n > LEVELING_ATTACK_HMS_MAX) n = LEVELING_ATTACK_HMS_MAX;
            if (n == g_leveling_attack_hms) return;
            g_leveling_attack_hms = n;
            g_out.limiter().setAttackMs((float)g_leveling_attack_hms * 0.5f);
            markStateDirty();
            break;
        }
        case LV_LOOKAHEAD: {
            int n = g_leveling_lookahead_ms + delta;
            if (n < LEVELING_LOOKAHEAD_MS_MIN) n = LEVELING_LOOKAHEAD_MS_MIN;
            if (n > LEVELING_LOOKAHEAD_MS_MAX) n = LEVELING_LOOKAHEAD_MS_MAX;
            if (n == g_leveling_lookahead_ms) return;
            g_leveling_lookahead_ms = n;
            g_out.limiter().setLookaheadMs((float)g_leveling_lookahead_ms);
            markStateDirty();
            break;
        }
        case LV_CEILING: {
            int n = g_leveling_ceiling_hdb + delta;
            if (n < LEVELING_CEILING_HDB_MIN) n = LEVELING_CEILING_HDB_MIN;
            if (n > LEVELING_CEILING_HDB_MAX) n = LEVELING_CEILING_HDB_MAX;
            if (n == g_leveling_ceiling_hdb) return;
            g_leveling_ceiling_hdb = n;
            g_out.limiter().setCeilingDb(-(float)g_leveling_ceiling_hdb * 0.5f);
            markStateDirty();
            break;
        }
        default:  // LV_RESET / LV_BACK — `/` activates, `,` is a no-op
            if (delta > 0) activateLevelingRow();
            return;
    }
    drawLeveling();
}

// -- Alarm editor --------------------------------------------------------
// One row per editable field. Days are seven independent toggle rows so
// the user never wonders how to flip just one — same pattern as every
// other toggle in Settings.
enum AlarmEditorRowId {
    AE_ENABLED = 0,
    AE_HOUR, AE_MIN,
    AE_DAYS,
    AE_VOL, AE_RAMP,
    AE_TRACK,
    AE_PREVIEW, AE_BACK,
    AE_COUNT
};

static void enterAlarmEditor(int slot);
static void exitAlarmEditor();
static void drawAlarmEditor();
static void enterDaysEditor();
static void navigateToPickTarget(const std::string& track);

static void aeRowLabel(int row, char* buf, size_t buflen) {
    const Alarm& a = g_alarms[g_alarm_editor_slot];
    switch (row) {
        case AE_HOUR:    snprintf(buf, buflen, "Hour"); return;
        case AE_MIN:     snprintf(buf, buflen, "Minute"); return;
        case AE_DAYS:    snprintf(buf, buflen, "Days"); return;
        case AE_TRACK:   snprintf(buf, buflen, "Track"); return;
        case AE_VOL:     snprintf(buf, buflen, "Volume"); return;
        case AE_RAMP:    snprintf(buf, buflen, "Ramp (s)"); return;
        case AE_ENABLED: snprintf(buf, buflen, "Enabled"); return;
        case AE_PREVIEW: snprintf(buf, buflen, "Preview"); return;
        case AE_BACK:    snprintf(buf, buflen, "Back"); return;
    }
    (void)a;
}

static void aeRowValueStr(int row, char* buf, size_t buflen) {
    const Alarm& a = g_alarms[g_alarm_editor_slot];
    switch (row) {
        case AE_HOUR:    snprintf(buf, buflen, "%02u", a.hour); return;
        case AE_MIN:     snprintf(buf, buflen, "%02u", a.minute); return;
        case AE_DAYS: {
            char days[8];
            formatDaysMask(a.days, days);
            snprintf(buf, buflen, "%s", days);
            return;
        }
        case AE_TRACK: {
            if (a.track.empty()) { snprintf(buf, buflen, "(beep)"); return; }
            const char* slash = strrchr(a.track.c_str(), '/');
            snprintf(buf, buflen, "%s", slash ? slash + 1 : a.track.c_str());
            return;
        }
        case AE_VOL:     snprintf(buf, buflen, "%u", a.volume); return;
        case AE_RAMP:    snprintf(buf, buflen, "%u", a.ramp_s); return;
        case AE_ENABLED: snprintf(buf, buflen, "[%c]", a.enabled ? 'x' : ' '); return;
        case AE_PREVIEW: snprintf(buf, buflen, ">"); return;
        case AE_BACK:    buf[0] = '\0'; return;
    }
}

static constexpr int AE_TRACK_MAX_LINES = 3;  // wrapped name lines under "Track:"

// Wraps the track basename into up to AE_TRACK_MAX_LINES indented lines for
// the editor's multi-line Track display; the last line gets a trailing "..."
// when the name overruns. Returns the number of lines filled.
static int trackWrapLines(const std::string& track, int cw, char lines[][64]) {
    const char* slash = strrchr(track.c_str(), '/');
    const char* base  = slash ? slash + 1 : track.c_str();
    int per = (SCREEN_W - cw - 2) / cw;   // one-char indent on the left
    if (per < 1) per = 1;
    int len = (int)strlen(base);
    int n = 0, off = 0;
    while (off < len && n < AE_TRACK_MAX_LINES) {
        int remaining = len - off;
        bool last_line = (n == AE_TRACK_MAX_LINES - 1);
        if (last_line && remaining > per) {
            int keep = (per > 3) ? per - 3 : per;
            snprintf(lines[n], 64, "%.*s...", keep, base + off);
            off = len;
        } else {
            int chunk = std::min(per, remaining);
            snprintf(lines[n], 64, "%.*s", chunk, base + off);
            off += chunk;
        }
        ++n;
    }
    return n;
}

// Editor rows are one line tall except the Track row, which expands to show
// the wrapped track name beneath its "Track:" label line.
static int aeRowLines(int row) {
    if (row != AE_TRACK) return 1;
    const std::string& t = g_alarms[g_alarm_editor_slot].track;
    if (t.empty()) return 1;  // "(beep)" sits on the label line
    char lines[AE_TRACK_MAX_LINES][64];
    return 1 + trackWrapLines(t, charW(), lines);
}

// Advance the scroll top until the selected row's label line is on-screen,
// accounting for the Track row's variable height (mirrors the browser's
// ensureCursorVisible, which the uniform-row scroll math couldn't).
static void ensureAeCursorVisible() {
    int rh = rowH();
    int avail = SCREEN_H - rh;  // the title occupies the first row
    if (g_alarm_editor_cursor < g_alarm_editor_top) {
        g_alarm_editor_top = g_alarm_editor_cursor;
        return;
    }
    while (g_alarm_editor_top < g_alarm_editor_cursor) {
        int y = 0;
        for (int row = g_alarm_editor_top; row < g_alarm_editor_cursor; ++row)
            y += aeRowLines(row) * rh;
        if (y + rh <= avail) return;
        ++g_alarm_editor_top;
    }
}

static void drawAlarmEditor() {
    auto& d = g_canvas;
    d.fillScreen(BLACK);
    d.setFont(notchFont());
    d.setTextSize(notchSize());
    d.setTextWrap(false, false);
    int rh = rowH();
    int cw = charW();

    ensureAeCursorVisible();

    d.setTextColor(COL_HEADER_TXT, BLACK);
    d.setCursor(2, 0);
    char title[16];
    snprintf(title, sizeof(title), "Alarm %d", g_alarm_editor_slot + 1);
    d.print(title);

    const std::string& track = g_alarms[g_alarm_editor_slot].track;
    int y = rh;
    for (int row = g_alarm_editor_top; row < AE_COUNT && y + rh <= SCREEN_H; ++row) {
        bool selected = (row == g_alarm_editor_cursor);
        if (selected) {
            d.fillRect(0, y, SCREEN_W, rh, COL_SETTINGS_SEL_BG);
            d.setTextColor(0x0000, COL_SETTINGS_SEL_BG);
        } else {
            d.setTextColor(COL_FILE_BRIGHT, BLACK);
        }
        char name[24]; aeRowLabel(row, name, sizeof(name));
        d.setCursor(2, y);
        d.print(name);
        if (row == AE_TRACK && !track.empty()) {
            // ">" marks the row actionable; the full name wraps below it,
            // dimmed and indented, so a long name reads in full.
            d.setCursor(SCREEN_W - cw - 2, y);
            d.print(">");
            char lines[AE_TRACK_MAX_LINES][64];
            int nl = trackWrapLines(track, cw, lines);
            d.setTextColor(COL_HEADER_TXT, BLACK);
            for (int li = 0; li < nl; ++li) {
                d.setCursor(2 + cw, y + (li + 1) * rh);
                d.print(lines[li]);
            }
            y += (1 + nl) * rh;
        } else {
            char val[24]; aeRowValueStr(row, val, sizeof(val));
            int vw = (int)strlen(val) * cw;
            d.setCursor(SCREEN_W - vw - 2, y);
            d.print(val);
            y += rh;
        }
    }
    presentFrame();
}

static void moveAlarmEditorCursor(int delta) {
    int n = ((g_alarm_editor_cursor + delta) % AE_COUNT + AE_COUNT) % AE_COUNT;
    if (n == g_alarm_editor_cursor) return;
    g_alarm_editor_cursor = n;
    drawAlarmEditor();
}

static void enterAlarmEditor(int slot) {
    g_alarm_editor_slot = slot;
    g_alarm_editor_cursor = 0;
    g_alarm_editor_top = 0;
    g_screen = Screen::AlarmEditor;
    drawAlarmEditor();
}

static void exitAlarmEditor() {
    goToScreen(Screen::Alarms);
}

static void adjustAlarmEditorRow(int delta) {
    Alarm& a = g_alarms[g_alarm_editor_slot];
    int row = g_alarm_editor_cursor;
    switch (row) {
        case AE_HOUR:    a.hour    = (a.hour    + 24 + delta) % 24;  markStateDirty(); break;
        case AE_MIN:     a.minute  = (a.minute  + 60 + delta) % 60;  markStateDirty(); break;
        case AE_VOL: {
            int n = (int)a.volume + delta;
            if (n < 0) n = 0;
            if (n > g_volume_max) n = g_volume_max;
            a.volume = (uint8_t)n;
            markStateDirty();
            break;
        }
        case AE_RAMP: {
            int n = (int)a.ramp_s + delta;
            if (n < 0) n = 0;
            if (n > 60) n = 60;
            a.ramp_s = (uint8_t)n;
            markStateDirty();
            break;
        }
        case AE_ENABLED:
            a.enabled = (delta > 0);
            markStateDirty();
            break;
        case AE_TRACK:
            // `/` (delta > 0) opens the picker via activateAlarmEditorRow;
            // `,` (delta < 0) clears the track back to the built-in beep —
            // there's no other value to scroll, so left means "no track".
            if (delta < 0 && !a.track.empty()) {
                a.track.clear();
                markStateDirty();
                break;
            }
            return;
        default:
            // Action rows (Days, Track, Preview, Back) are reached through
            // activateAlarmEditorRow on `/`, not here.
            return;
    }
    drawAlarmEditor();
}

static void activateAlarmEditorRow() {
    int row = g_alarm_editor_cursor;
    if (row == AE_BACK)    { exitAlarmEditor(); return; }
    if (row == AE_ENABLED) {
        Alarm& a = g_alarms[g_alarm_editor_slot];
        a.enabled = !a.enabled;
        markStateDirty();
        drawAlarmEditor();
        return;
    }
    if (row == AE_DAYS) {
        enterDaysEditor();
        return;
    }
    if (row == AE_TRACK) {
        g_pick_slot = g_alarm_editor_slot;
        g_pick_saved_path   = g_cur_path;
        g_pick_saved_cursor = g_cursor;
        g_screen = Screen::TrackPicker;
        // Open the picker on the slot's current track (folder navigated, file
        // highlighted) so editing an existing alarm starts in context; an
        // empty slot or a vanished folder falls back to the last position.
        navigateToPickTarget(g_alarms[g_alarm_editor_slot].track);
        draw();
        return;
    }
    if (row == AE_PREVIEW) {
        // Arm a fire of this slot in 5 s. We dim to clock brightness and
        // show the clock so the user sees the same wake framing they would
        // for a real fire; any key in that 5 s window cancels.
        g_alarm_preview_slot       = g_alarm_editor_slot;
        g_alarm_preview_fire_at_ms = millis() + 5000;
        enterStandby();
        return;
    }
}

// Point the browser at the folder holding `track`, with that file under the
// cursor. No-op (keeps the last browser position) when there's no track or
// the folder is gone — e.g. the SD card was swapped or the file moved.
static void navigateToPickTarget(const std::string& track) {
    if (track.empty()) return;
    std::string dir = parentPath(track);
    if (dir.empty() || !SD.exists(dir.c_str())) return;
    loadDir(dir);
    std::string name = basename(track);
    for (int i = 0; i < (int)g_entries.size(); ++i) {
        if (g_entries[i].name == name) { g_cursor = i; break; }
    }
}

// -- Days editor (toggle which days an alarm fires; nested under the alarm
// editor, reached from its single "Days" row) ---------------------------
enum DaysEditorRowId { DE_MON = 0, DE_TUE, DE_WED, DE_THU, DE_FRI, DE_SAT, DE_SUN,
                       DE_BACK, DE_COUNT };

static const char* const DOW_NAMES[7] = {
    "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday"
};

static void drawDaysEditor() {
    auto& d = g_canvas;
    d.fillScreen(BLACK);
    d.setFont(notchFont());
    d.setTextSize(notchSize());
    d.setTextWrap(false, false);
    int rh = rowH();
    int cw = charW();
    const Alarm& a = g_alarms[g_alarm_editor_slot];

    int rows_avail = (SCREEN_H / rh) - 1;
    if (rows_avail < 1) rows_avail = 1;
    static int de_top = 0;
    if (g_days_editor_cursor < de_top) de_top = g_days_editor_cursor;
    if (g_days_editor_cursor >= de_top + rows_avail) de_top = g_days_editor_cursor - rows_avail + 1;
    if (de_top + rows_avail > DE_COUNT) de_top = DE_COUNT - rows_avail;
    if (de_top < 0) de_top = 0;

    d.setTextColor(COL_HEADER_TXT, BLACK);
    d.setCursor(2, 0);
    d.print("Days");

    for (int i = 0; i < rows_avail && (de_top + i) < DE_COUNT; ++i) {
        int row = de_top + i;
        int y = rh + i * rh;
        bool selected = (row == g_days_editor_cursor);
        if (selected) {
            d.fillRect(0, y, SCREEN_W, rh, COL_SETTINGS_SEL_BG);
            d.setTextColor(0x0000, COL_SETTINGS_SEL_BG);
        } else {
            d.setTextColor(COL_FILE_BRIGHT, BLACK);
        }
        d.setCursor(2, y);
        if (row == DE_BACK) {
            d.print("Back");
        } else {
            d.print(DOW_NAMES[row]);
            char val[4];
            snprintf(val, sizeof(val), "[%c]", (a.days & (1 << row)) ? 'x' : ' ');
            int vw = (int)strlen(val) * cw;
            d.setCursor(SCREEN_W - vw - 2, y);
            d.print(val);
        }
    }
    presentFrame();
}

static void enterDaysEditor() {
    g_days_editor_cursor = 0;
    g_screen = Screen::DaysEditor;
    drawDaysEditor();
}

static void exitDaysEditor() {
    goToScreen(Screen::AlarmEditor);
}

static void moveDaysEditorCursor(int delta) {
    int n = ((g_days_editor_cursor + delta) % DE_COUNT + DE_COUNT) % DE_COUNT;
    if (n == g_days_editor_cursor) return;
    g_days_editor_cursor = n;
    drawDaysEditor();
}

static void activateDaysEditorRow() {
    if (g_days_editor_cursor == DE_BACK) { exitDaysEditor(); return; }
    g_alarms[g_alarm_editor_slot].days ^= (1 << g_days_editor_cursor);
    markStateDirty();
    drawDaysEditor();
}

// -- Set Current Time editor --------------------------------------------
enum SctRowId { SCT_HOUR = 0, SCT_MIN, SCT_YEAR, SCT_MONTH, SCT_DAY,
                SCT_COMMIT, SCT_BACK, SCT_COUNT };

static int daysInMonth(uint16_t y, uint8_t m) {
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m < 1 || m > 12) return 31;
    if (m == 2) {
        bool leap = (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
        return leap ? 29 : 28;
    }
    return days[m - 1];
}

static void sctRowLabel(int row, char* buf, size_t buflen) {
    switch (row) {
        case SCT_HOUR:   snprintf(buf, buflen, "Hour"); return;
        case SCT_MIN:    snprintf(buf, buflen, "Minute"); return;
        case SCT_YEAR:   snprintf(buf, buflen, "Year"); return;
        case SCT_MONTH:  snprintf(buf, buflen, "Month"); return;
        case SCT_DAY:    snprintf(buf, buflen, "Day"); return;
        case SCT_COMMIT: snprintf(buf, buflen, "Commit"); return;
        case SCT_BACK:   snprintf(buf, buflen, "Back"); return;
    }
}

static void sctRowValueStr(int row, char* buf, size_t buflen) {
    switch (row) {
        case SCT_HOUR:   snprintf(buf, buflen, "%02u", g_sct_hour); return;
        case SCT_MIN:    snprintf(buf, buflen, "%02u", g_sct_min); return;
        case SCT_YEAR:   snprintf(buf, buflen, "%04u", g_sct_year); return;
        case SCT_MONTH:  snprintf(buf, buflen, "%02u", g_sct_month); return;
        case SCT_DAY:    snprintf(buf, buflen, "%02u", g_sct_day); return;
        case SCT_COMMIT:
        case SCT_BACK:   buf[0] = '\0'; return;
    }
}

static void drawSetTime() {
    auto& d = g_canvas;
    d.fillScreen(BLACK);
    d.setFont(notchFont());
    d.setTextSize(notchSize());
    d.setTextWrap(false, false);
    int rh = rowH();
    int cw = charW();
    d.setTextColor(COL_HEADER_TXT, BLACK);
    d.setCursor(2, 0);
    d.print("Set current time");
    for (int row = 0; row < SCT_COUNT; ++row) {
        int y = rh + row * rh;
        bool selected = (row == g_sct_cursor);
        if (selected) {
            d.fillRect(0, y, SCREEN_W, rh, COL_SETTINGS_SEL_BG);
            d.setTextColor(0x0000, COL_SETTINGS_SEL_BG);
        } else {
            d.setTextColor(COL_FILE_BRIGHT, BLACK);
        }
        char name[24]; sctRowLabel(row, name, sizeof(name));
        char val[12];  sctRowValueStr(row, val, sizeof(val));
        d.setCursor(2, y);
        d.print(name);
        int vw = (int)strlen(val) * cw;
        d.setCursor(SCREEN_W - vw - 2, y);
        d.print(val);
    }
    presentFrame();
}

static void enterSetTime() {
    // Seed the draft from the software clock so the user adjusts deltas, not types.
    rtc_date_type d; rtc_time_type t; currentClock(d, t);
    g_sct_year  = d.Year;
    g_sct_month = d.Month;
    g_sct_day   = d.Date;
    g_sct_hour  = t.Hours;
    g_sct_min   = t.Minutes;
    g_sct_cursor = 0;
    g_screen = Screen::SetTime;
    drawSetTime();
}

static void exitSetTime() {
    goToScreen(Screen::Alarms);
}

static void commitSetTime() {
    // Clamp day to the new month/year before writing.
    int dim = daysInMonth(g_sct_year, g_sct_month);
    if (g_sct_day > dim) g_sct_day = dim;
    rtc_date_type d;
    d.Year    = g_sct_year;
    d.Month   = g_sct_month;
    d.Date    = g_sct_day;
    d.WeekDay = dayOfWeekMonFirst(g_sct_year, g_sct_month, g_sct_day);
    g_rtc.setDate(&d);
    rtc_time_type t;
    t.Hours   = g_sct_hour;
    t.Minutes = g_sct_min;
    t.Seconds = 0;
    g_rtc.setTime(&t);
    syncClockFromRtc();  // re-anchor the software clock to the just-set time
    exitSetTime();
}

static void moveSetTimeCursor(int delta) {
    int n = ((g_sct_cursor + delta) % SCT_COUNT + SCT_COUNT) % SCT_COUNT;
    if (n == g_sct_cursor) return;
    g_sct_cursor = n;
    drawSetTime();
}

static void adjustSetTimeRow(int delta) {
    auto mod = [](int v, int hi) { return ((v % hi) + hi) % hi; };
    switch (g_sct_cursor) {
        case SCT_HOUR:  g_sct_hour  = (uint8_t)mod(g_sct_hour  + delta, 24); break;
        case SCT_MIN:   g_sct_min   = (uint8_t)mod(g_sct_min   + delta, 60); break;
        case SCT_YEAR:  g_sct_year  = (uint16_t)(g_sct_year + delta); break;
        case SCT_MONTH: g_sct_month = (uint8_t)(1 + mod(g_sct_month - 1 + delta, 12)); break;
        case SCT_DAY: {
            int dim = daysInMonth(g_sct_year, g_sct_month);
            g_sct_day = (uint8_t)(1 + mod(g_sct_day - 1 + delta, dim));
            break;
        }
        case SCT_COMMIT: if (delta > 0) commitSetTime(); return;
        case SCT_BACK:   if (delta > 0) exitSetTime();  return;
    }
    drawSetTime();
}

static void activateSetTimeRow() {
    if (g_sct_cursor == SCT_COMMIT) { commitSetTime(); return; }
    if (g_sct_cursor == SCT_BACK)   { exitSetTime();  return; }
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

// Zeller's congruence — returns Mon=0..Sun=6 for Gregorian (y,m,d).
static uint8_t dayOfWeekMonFirst(uint16_t y, uint8_t m, uint8_t d) {
    if (m < 3) { m += 12; y -= 1; }
    uint16_t K = y % 100;
    uint16_t J = y / 100;
    int h = (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    // Zeller h: 0=Sat..6=Fri. Convert to Mon=0..Sun=6.
    static const uint8_t to_mon_first[7] = { 5, 6, 0, 1, 2, 3, 4 };
    return to_mon_first[h];
}

static bool parseCompileDateTime(rtc_date_type& d, rtc_time_type& t) {
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = { __DATE__[0], __DATE__[1], __DATE__[2], 0 };
    const char* ms = strstr(months, mon);       // __DATE__: "Mmm DD YYYY"
    if (!ms) return false;
    d.Month   = ((ms - months) / 3) + 1;
    d.Date    = atoi(__DATE__ + 4);             // day (space-padded ok)
    d.Year    = atoi(__DATE__ + 7);
    d.WeekDay = dayOfWeekMonFirst(d.Year, d.Month, d.Date);
    t.Hours   = atoi(__TIME__ + 0);             // __TIME__: "HH:MM:SS"
    t.Minutes = atoi(__TIME__ + 3);
    t.Seconds = atoi(__TIME__ + 6);
    return true;
}

// Civil date <-> days-since-1970 (Howard Hinnant's algorithms). The basis for
// advancing the software wall-clock across minute / day / month boundaries
// without per-redraw RTC reads.
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static void civilFromDays(int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yr = (int)yoe + (int)(era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = yr + (m <= 2);
}

// Read the chip and re-anchor the software clock to it. No-op when absent.
static void syncClockFromRtc() {
    if (!g_rtc_present) return;
    rtc_date_type d; g_rtc.getDate(&d);
    rtc_time_type t; g_rtc.getTime(&t);
    g_clock_epoch = (uint32_t)(daysFromCivil(d.Year, d.Month, d.Date) * 86400
                  + t.Hours * 3600 + t.Minutes * 60 + t.Seconds);
    g_clock_sync_ms = millis();
}

// Current wall-clock, derived from the last sync plus elapsed millis() — the
// single source every time consumer reads instead of the chip.
static void currentClock(rtc_date_type& d, rtc_time_type& t) {
    uint32_t e = g_clock_epoch + (millis() - g_clock_sync_ms) / 1000;
    int      y; unsigned m, dd;
    civilFromDays(e / 86400, y, m, dd);
    uint32_t secs = e % 86400;
    d.Year = (uint16_t)y; d.Month = (uint8_t)m; d.Date = (uint8_t)dd;
    d.WeekDay = dayOfWeekMonFirst(d.Year, d.Month, d.Date);
    t.Hours = (uint8_t)(secs / 3600);
    t.Minutes = (uint8_t)((secs % 3600) / 60);
    t.Seconds = (uint8_t)(secs % 60);
}

static void seedRtcIfUnset() {
    rtc_date_type d; rtc_time_type t;
    g_rtc.getDate(&d);
    if (d.Year >= 2025) return;                 // already seeded on a prior boot
    if (!parseCompileDateTime(d, t)) return;
    g_rtc.setDate(&d);
    g_rtc.setTime(&t);
    Serial.printf("[rtc] seeded from build timestamp: %04u-%02u-%02u %02u:%02u:%02u\n",
                  d.Year, d.Month, d.Date, t.Hours, t.Minutes, t.Seconds);
}

// Re-sync the software clock from the chip on a slow cadence (drift
// correction), and once a second refresh the diagnostic clock string and the
// full-screen clock from the software clock. The chip is touched only on the
// re-sync, never per second.
static constexpr uint32_t RTC_RESYNC_MS = 5 * 60 * 1000;  // 5 minutes

static void pollRtcClock() {
    if (!g_rtc_present) return;
    static uint32_t last_tick = 0, last_sync = 0;
    uint32_t now = millis();
    if ((uint32_t)(now - last_sync) >= RTC_RESYNC_MS) { syncClockFromRtc(); last_sync = now; }
    if ((uint32_t)(now - last_tick) < 1000) return;
    last_tick = now;
    rtc_date_type d; rtc_time_type t; currentClock(d, t);
    snprintf(g_diag_clk, sizeof(g_diag_clk), "%02u:%02u:%02u",
             t.Hours, t.Minutes, t.Seconds);
    if (fullScreenClockActive()) drawClockScreen();
}

static void setupRtc() {
    // Cardputer ADV Port A: SDA=GPIO2, SCL=GPIO1. Init Wire as master ourselves;
    // Unit_RTC's 4-arg begin() invokes the Wire slave-mode overload (library
    // bug — TODO: upstream a fix to m5stack/M5Unit-RTC).
    Wire.begin(/*sda=*/2, /*scl=*/1, /*freq=*/100000);
    // Probe the RTC's I²C address (0x51) once. Everything else gates on
    // g_rtc_present, so an absent add-on is never spoken to again — no error
    // storm on the shared bus.
    Wire.beginTransmission(0x51);
    g_rtc_present = (Wire.endTransmission() == 0);
    if (!g_rtc_present) {
        Serial.println("[rtc] no RTC on Port A — timekeeping and alarms disabled");
        return;
    }
    g_rtc.begin(&Wire);
    seedRtcIfUnset();
    syncClockFromRtc();
}

// -- Standby clock screen ------------------------------------------------
static constexpr const char* WEEKDAY_NAMES[7] = { "Mon","Tue","Wed","Thu","Fri","Sat","Sun" };
static uint32_t g_snooze_until_ms = 0;   // 0 = not snoozing
static uint32_t g_standby_entered_ms = 0;

// Alarm fire state. `g_alarm_fire_slot` is the slot that armed; the
// `active_ms` counter accumulates only while playing (snooze paused).
static int      g_alarm_fire_slot     = -1;
static uint32_t g_alarm_active_start  = 0;     // millis when current play interval started
static uint32_t g_alarm_active_total  = 0;     // cumulative play time across snoozes
static uint32_t g_alarm_beep_last_ms  = 0;
static bool     g_alarm_beep_on       = false;
static uint8_t  g_alarm_prior_brightness_idx = 0;
// Pre-alarm playback snapshot, taken on a fresh fire and restored on dismiss
// so the alarm puts the user back where they were. Volume is always restored;
// the track (paused/playing at its byte position) only when one was playing.
static int         g_alarm_prior_volume = 0;
static std::string g_alarm_prior_track;
static uint32_t    g_alarm_prior_pos    = 0;
static bool        g_alarm_prior_playing = false;  // a track was loaded pre-alarm
// Volume ramp: linear from 0 → configured over ramp_s seconds. Once the
// user presses +/- mid-fire, `ramp_overridden` latches and the auto-ramp
// stops touching the volume.
static bool     g_alarm_ramp_overridden = false;
// Set when the slot's configured track can't be opened (no track, SD missing,
// file gone, unsupported) — the fire falls back to the built-in beep even
// though the slot may carry a non-empty track path.
static bool     g_alarm_beep_fallback   = false;
// (Preview globals — g_alarm_preview_fire_at_ms / g_alarm_preview_slot —
// are forward-declared near the alarm editor globals above; preview firing
// is driven from pollAlarms below.)
// Per-slot watermark (today's yyyymmddhhmm) so an alarm only fires once
// per matching minute. RAM-only; a cold boot within the fire minute will
// re-fire (accepted risk, per Approach).
static uint32_t g_alarm_last_fire_key[ALARM_COUNT] = { 0 };

// Weekday + time of the next armed alarm relative to now (e.g. "Sun 08:30"),
// scanning the coming week. Empty string if no alarm is armed. The "next:"
// label is added by the caller so it can sit on its own line.
static std::string nextAlarmString() {
    if (!g_rtc_present) return {};
    rtc_date_type d; rtc_time_type t; currentClock(d, t);
    uint8_t today_dow = dayOfWeekMonFirst(d.Year, d.Month, d.Date);
    int now_min = t.Hours * 60 + t.Minutes;
    int best_offset_min = INT_MAX;
    int best_dow = -1;
    int best_hm  = 0;
    for (int dow_offset = 0; dow_offset < 7; ++dow_offset) {
        uint8_t dow = (today_dow + dow_offset) % 7;
        for (int i = 0; i < ALARM_COUNT; ++i) {
            const Alarm& a = g_alarms[i];
            if (!a.enabled) continue;
            if (!(a.days & (1 << dow))) continue;
            int alarm_min = a.hour * 60 + a.minute;
            int offset = dow_offset * 24 * 60 + (alarm_min - now_min);
            if (dow_offset == 0 && alarm_min <= now_min) offset += 7 * 24 * 60;
            if (offset < best_offset_min) {
                best_offset_min = offset;
                best_dow = dow;
                best_hm  = alarm_min;
            }
        }
    }
    if (best_dow < 0) return {};
    char buf[24];
    snprintf(buf, sizeof(buf), "%s %02d:%02d",
             WEEKDAY_NAMES[best_dow], best_hm / 60, best_hm % 60);
    return buf;
}

// Full-screen 24-hr clock used by standby and alarm-firing modes.
// Draw `text` centred at `size` starting at `top`, wrapping onto a second
// line (split at the most central space) when it's too wide for one line.
static void drawClockSubtext(const char* text, int top, int size, uint16_t col) {
    auto& d = g_canvas;
    d.setTextSize(size);
    d.setTextColor(col, BLACK);
    int cw  = 6 * size;
    int chh = 8 * size;
    int len = (int)strlen(text);
    if (len * cw <= SCREEN_W - 4) {
        d.setCursor((SCREEN_W - len * cw) / 2, top);
        d.print(text);
        return;
    }
    // Too wide: split at the space nearest the middle.
    int mid = len / 2, brk = -1;
    for (int off = 0; off <= mid && brk < 0; ++off) {
        if (mid + off < len && text[mid + off] == ' ') brk = mid + off;
        else if (text[mid - off] == ' ')               brk = mid - off;
    }
    if (brk < 0) brk = mid;  // no space — hard cut
    char l1[40], l2[40];
    snprintf(l1, sizeof(l1), "%.*s", brk, text);
    snprintf(l2, sizeof(l2), "%s", text + brk + (text[brk] == ' ' ? 1 : 0));
    d.setCursor((SCREEN_W - (int)strlen(l1) * cw) / 2, top);
    d.print(l1);
    d.setCursor((SCREEN_W - (int)strlen(l2) * cw) / 2, top + chh + 2);
    d.print(l2);
}

static void drawClockScreen() {
    auto& d = g_canvas;
    d.fillScreen(BLACK);

    // Warm, bright palette (high red + green, no blue): legible at low standby
    // brightness so the user needn't raise the backlight, while keeping blue
    // light minimal for night use.
    constexpr uint16_t COL_CLOCK_PRIMARY   = 0xFEC0;  // bright amber — the time
    constexpr uint16_t COL_CLOCK_SECONDARY = 0xFD40;  // amber — day / battery / hint
    constexpr uint16_t COL_CLOCK_ALERT     = 0xFB00;  // deep orange — fire / snooze
    constexpr uint16_t COL_CLOCK_FIRING    = 0xFFFF;  // white — the time while the alarm sounds

    // Battery top-right — shown in both the clock and the no-clock states.
    if (g_battery_level >= 0) {
        char bat[10];
        snprintf(bat, sizeof(bat), "bat %d%%", g_battery_level);
        d.setTextSize(2);
        d.setTextColor(COL_CLOCK_SECONDARY, BLACK);
        d.setCursor(SCREEN_W - (int)strlen(bat) * 12 - 2, 2);
        d.print(bat);
    }

    // No RTC fitted: no timekeeping, so say so plainly rather than show a wrong
    // time. Alarms are disabled too, so only standby reaches here.
    if (!g_rtc_present) {
        const char* nc = "no clock";
        d.setTextSize(4);
        d.setTextColor(COL_WARN, BLACK);
        d.setCursor((SCREEN_W - (int)strlen(nc) * 24) / 2, 44);
        d.print(nc);
        drawClockSubtext("RTC not connected", 100, 2, COL_WARN);
        presentFrame();
        return;
    }

    rtc_date_type rd; rtc_time_type rt; currentClock(rd, rt);
    uint8_t dow = dayOfWeekMonFirst(rd.Year, rd.Month, rd.Date);

    // Status strip: today's weekday top-left, matched to the battery above.
    d.setTextSize(2);
    d.setTextColor(COL_CLOCK_SECONDARY, BLACK);
    d.setCursor(2, 2);
    d.print(WEEKDAY_NAMES[dow]);

    // Big HH:MM centred — size 7 → 42 px char width × 56 px tall.
    // Firing turns the time white as an unmistakable "alarm is live" cue;
    // standby/snooze keep the warm amber.
    bool firing = g_screen == Screen::AlarmFiring;
    char hhmm[6];
    snprintf(hhmm, sizeof(hhmm), "%02u:%02u", rt.Hours, rt.Minutes);
    d.setTextSize(7);
    d.setTextColor(firing ? COL_CLOCK_FIRING : COL_CLOCK_PRIMARY, BLACK);
    int big_w = (int)strlen(hhmm) * 42;
    int big_h = 56;
    // One fixed vertical position across standby / firing / snooze so the time
    // doesn't jump as the screen changes, balanced below the status strip.
    // Firing's three hint lines are packed against the bottom edge to clear it.
    int big_y = 28;
    d.setCursor((SCREEN_W - big_w) / 2, big_y);
    d.print(hhmm);

    int below_y = big_y + big_h + 2;
    if (firing) {
        // Three "label: action" hints near the bottom. Enter and Esc now do
        // different things, so each is spelled out. ` reads as Esc from the
        // user's side.
        drawClockSubtext("Enter: dismiss",      SCREEN_H - 49, 2, COL_CLOCK_ALERT);
        drawClockSubtext("Esc: stop",           SCREEN_H - 33, 2, COL_CLOCK_ALERT);
        drawClockSubtext("any key: snooze",     SCREEN_H - 17, 2, COL_CLOCK_SECONDARY);
    } else if (g_screen == Screen::AlarmSnoozing) {
        uint32_t now = millis();
        uint32_t remain = (g_snooze_until_ms > now) ? (g_snooze_until_ms - now) : 0;
        int rs = (int)(remain / 1000);
        char snz[20];
        snprintf(snz, sizeof(snz), "Snooze %d:%02d", rs / 60, rs % 60);
        // Sits lower than the standby next-alarm hint so it's centred in the
        // space below the time rather than crowding it.
        drawClockSubtext(snz, below_y + 14, 3, COL_CLOCK_ALERT);  // alarm still pending
    } else {
        // Next-alarm hint: "next:" label on its own line, the alarm's weekday
        // and time on the line below.
        std::string detail = nextAlarmString();
        if (!detail.empty()) {
            drawClockSubtext("next:", below_y, 3, COL_CLOCK_SECONDARY);
            drawClockSubtext(detail.c_str(), below_y + 24, 3, COL_CLOCK_SECONDARY);
        }
    }
    presentFrame();
}

// Single render entry point — dispatches on the current screen. The Main and
// TrackPicker screens both render the browser (TrackPicker is the browser in
// pick theme); draw() handles that path including search / visualisation.
static void renderScreen() {
    switch (g_screen) {
        case Screen::Settings:      drawSettings();   break;
        case Screen::KeyReference:  drawHelp();        break;
        case Screen::ResetModal:    drawResetModal();  break;
        case Screen::Alarms:        drawAlarms();      break;
        case Screen::Leveling:      drawLeveling();    break;
        case Screen::SetTime:       drawSetTime();     break;
        case Screen::AlarmEditor:   drawAlarmEditor(); break;
        case Screen::DaysEditor:    drawDaysEditor();  break;
        case Screen::Standby:
        case Screen::AlarmFiring:
        case Screen::AlarmSnoozing: drawClockScreen(); break;
        case Screen::Chess:         chess::render(g_canvas); presentFrame(); break;
        case Screen::ChessConfirm:  drawChessConfirm(); break;
        case Screen::Main:
        case Screen::TrackPicker:   draw();            break;
    }
}

static void goToScreen(Screen s) {
    g_screen = s;
    renderScreen();
}

static void enterStandby() {
    if (g_screen == Screen::Standby) return;
    g_screen = Screen::Standby;
    g_standby_entered_ms = millis();
    // Short ramp (1 s) — long enough to hide the redraw-vs-PWM-step beat
    // that reads as a flash, short enough that the linear-in-PWM ramp
    // doesn't visibly bunch all the change into the tail.
    setBrightnessRampTo(standbyBrightness(), 1000);
    drawClockScreen();
}

static void exitStandby() {
    if (g_screen != Screen::Standby) return;
    g_screen = Screen::Main;
    // Cancel any pending preview — the user actively exited.
    g_alarm_preview_fire_at_ms = 0;
    g_alarm_preview_slot = -1;
    setBrightnessRampTo(userBrightness(), RAMP_SET_MS);
    draw();
}

// -- Alarm fire ----------------------------------------------------------
// Minimum-viable fire: every armed alarm beeps via the speaker tone API
// (track playback comes once the picker lands). `` ` `` / Enter dismisses;
// any other key snoozes for 8 minutes; `+` / `-` adjust volume mid-fire.
static constexpr uint32_t ALARM_SNOOZE_MS    = 8 * 60 * 1000;
static constexpr uint32_t ALARM_AUTOSTOP_MS  = 60 * 60 * 1000;  // 1 hr cumulative
static constexpr uint32_t ALARM_BEEP_PERIOD_MS = 700;           // on/off cycle

static void beepStop() {
    M5Cardputer.Speaker.stop();
    g_alarm_beep_on = false;
}

// `fresh` is false only for a re-fire out of snooze, which must keep the
// pre-alarm snapshot taken at the original fire rather than overwrite it with
// the (paused) alarm track.
static void fireAlarm(int slot, bool fresh = true) {
    g_alarm_fire_slot = slot;
    // Snapshot the pre-alarm state so dismiss can restore it. Volume always;
    // the track (with its byte position) only if one was loaded. Captured
    // before stopPlayback tears the source down.
    if (fresh) {
        g_alarm_prior_volume  = g_volume;
        g_alarm_prior_playing = g_audio_active && !g_play_path.empty();
        g_alarm_prior_track.clear();
        if (g_alarm_prior_playing) {
            g_alarm_prior_track  = g_play_path;
            uint32_t pos = 0;
            if (g_audio_mutex) {
                xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
                if (g_src) pos = g_src->getPos();
                xSemaphoreGive(g_audio_mutex);
            }
            g_alarm_prior_pos = pos;
        }
    }
    // Stop any music playback before the alarm takes over — otherwise the
    // beep mixes under the track instead of replacing it.
    stopPlayback();
    g_screen = Screen::AlarmFiring;
    g_alarm_active_start = millis();
    g_alarm_active_total = 0;
    g_snooze_until_ms = 0;
    g_alarm_beep_last_ms = 0;
    g_alarm_beep_on = false;
    g_alarm_prior_brightness_idx = (uint8_t)g_brightness_idx;
    setBrightnessRampTo(userBrightness(), RAMP_WAKE_MS);
    // Start the configured track; fall back to the beep tone when the slot
    // has no track or the file can't be opened (SD missing, file gone).
    const std::string& track = g_alarms[slot].track;
    g_alarm_beep_fallback = track.empty() ? false : !startPlayback(track);
    // Apply the alarm's volume. With ramp_s > 0 the auto-ramp starts at 0
    // and climbs to the configured volume; otherwise start at the target.
    g_alarm_ramp_overridden = false;
    g_volume = (g_alarms[slot].ramp_s > 0) ? 0 : g_alarms[slot].volume;
    applyVolume();
    drawClockScreen();
}

static void snoozeAlarm() {
    if (g_screen != Screen::AlarmFiring) return;
    beepStop();
    // Pause (don't stop) the track so the clock goes silent during the snooze
    // gap but the track stays loaded — a dismiss from snooze then leaves it
    // paused-and-resumable, same as dismiss while firing. Re-fire restarts it.
    if (g_audio_active) g_paused = true;
    g_alarm_active_total += (millis() - g_alarm_active_start);
    g_screen = Screen::AlarmSnoozing;
    g_snooze_until_ms = millis() + ALARM_SNOOZE_MS;
    // Keep the screen at the user's normal brightness during snooze — the
    // alarm just happened, the user wants to be able to see the clock easily.
    drawClockScreen();
}

static void dismissAlarm() {
    if (g_screen != Screen::AlarmFiring && g_screen != Screen::AlarmSnoozing) return;
    beepStop();
    // Restore the pre-alarm state: reload the track that was playing before
    // the alarm (at its byte position), or stop cleanly if nothing was playing.
    // Either way restore the user's volume so the alarm's volume doesn't stick.
    // Returns paused regardless of the prior play state — a dismissed alarm
    // shouldn't leave music suddenly playing at the bedside.
    if (g_alarm_prior_playing && SD.exists(g_alarm_prior_track.c_str())) {
        if (startPlayback(g_alarm_prior_track, /*start_paused=*/true)) {
            uint32_t sz = g_src ? g_src->getSize() : 0;
            if (g_alarm_prior_pos >= g_audio_start_offset && g_alarm_prior_pos < sz) {
                seekToByte(g_alarm_prior_pos);
            }
            g_paused = true;  // come back paused even if it was playing
        }
    } else {
        stopPlayback();
    }
    g_volume = g_alarm_prior_volume;
    applyVolume();
    g_alarm_prior_playing = false;
    g_alarm_fire_slot = -1;
    g_snooze_until_ms = 0;
    g_alarm_active_total = 0;
    // Always leave clock mode on dismiss — the user just confirmed they're
    // interacting, so the bedside view shouldn't linger.
    g_screen = Screen::Main;
    g_brightness_idx = g_alarm_prior_brightness_idx;
    setBrightnessRampTo(userBrightness(), RAMP_SET_MS);
    draw();
}

// Shared tail for the "wake to the music" exits (Enter from firing or snooze):
// leave clock mode for Main at the user's brightness without the snapshot
// restore that dismiss does. The caller has already arranged the track and
// volume; this just clears the alarm bookkeeping and returns.
static void wakeToMain() {
    g_alarm_prior_playing = false;
    g_alarm_fire_slot = -1;
    g_snooze_until_ms = 0;
    g_alarm_active_total = 0;
    g_screen = Screen::Main;
    g_brightness_idx = g_alarm_prior_brightness_idx;
    setBrightnessRampTo(userBrightness(), RAMP_SET_MS);
    draw();
}

// The auto-ramp stops the moment we leave the firing screen, so a wake settles
// the volume on the slot's configured target — unless the user already nudged
// it mid-fire, in which case their level stands.
static void settleAlarmWakeVolume() {
    if (g_alarm_ramp_overridden) return;
    g_volume = g_alarms[g_alarm_fire_slot].volume;
    applyVolume();
}

// Enter while firing: wake to the alarm track. Unlike dismiss, the pre-alarm
// state is *not* restored — the alarm track keeps playing where it is and
// stops on its own when it reaches the end.
static void acceptAlarm() {
    if (g_screen != Screen::AlarmFiring) return;
    beepStop();
    if (g_alarm_beep_fallback) stopPlayback();  // no real track to wake to
    else                       settleAlarmWakeVolume();
    wakeToMain();
}

// Enter while snoozing: wake to the alarm track played from the top. The
// snoozed track is paused partway through, so restart it from the beginning
// rather than resuming mid-track; it then plays out and stops on its own.
static void wakeFromSnooze() {
    if (g_screen != Screen::AlarmSnoozing) return;
    beepStop();
    const std::string& track = g_alarms[g_alarm_fire_slot].track;
    if (!track.empty() && startPlayback(track)) settleAlarmWakeVolume();
    else                                        stopPlayback();
    wakeToMain();
}

static void pollAlarmBeep() {
    if (g_screen != Screen::AlarmFiring) return;
    if (!g_alarm_beep_fallback) return;  // a real track is playing; no beep
    uint32_t now = millis();
    if (now - g_alarm_beep_last_ms < ALARM_BEEP_PERIOD_MS) return;
    g_alarm_beep_last_ms = now;
    g_alarm_beep_on = !g_alarm_beep_on;
    if (g_alarm_beep_on) {
        M5Cardputer.Speaker.tone(880.0f, ALARM_BEEP_PERIOD_MS);
    } else {
        M5Cardputer.Speaker.stop();
    }
}

// Per-second alarm scan. Fires the highest-priority matching slot when
// today's weekday and the current HH:MM match an armed alarm; the per-slot
// watermark prevents repeated fires within the same minute. Also drives
// snooze re-fire and the 1-hour cumulative auto-stop.
static void pollAlarms() {
    static uint32_t last_ms = 0;
    uint32_t now = millis();
    if ((uint32_t)(now - last_ms) < 1000) return;
    last_ms = now;

    if (g_alarm_preview_fire_at_ms && now >= g_alarm_preview_fire_at_ms) {
        int slot = g_alarm_preview_slot;
        g_alarm_preview_fire_at_ms = 0;
        g_alarm_preview_slot = -1;
        fireAlarm(slot);
        return;
    }
    if (g_screen == Screen::AlarmFiring) {
        uint32_t live = (now - g_alarm_active_start) + g_alarm_active_total;
        if (live >= ALARM_AUTOSTOP_MS) { dismissAlarm(); return; }
        // Volume ramp: scale 0 → target linearly over ramp_s seconds based
        // on time spent live in *this* fire interval (snooze re-fires reset
        // the live counter via the active_start reset in fireAlarm).
        if (!g_alarm_ramp_overridden) {
            const Alarm& a = g_alarms[g_alarm_fire_slot];
            if (a.ramp_s > 0 && g_volume < a.volume) {
                uint32_t interval = now - g_alarm_active_start;
                uint32_t total_ms = (uint32_t)a.ramp_s * 1000u;
                int new_vol = (int)(((uint32_t)a.volume * interval) / total_ms);
                if (new_vol > a.volume) new_vol = a.volume;
                if (new_vol > g_volume) {
                    g_volume = new_vol;
                    M5Cardputer.Speaker.setVolume((uint8_t)((g_volume * 255) / MAX_VOL));
                }
            }
        }
        pollAlarmBeep();
        return;
    }
    if (g_screen == Screen::AlarmSnoozing) {
        if (now >= g_snooze_until_ms) {
            // Resume firing the same slot — keep the pre-alarm snapshot.
            fireAlarm(g_alarm_fire_slot, /*fresh=*/false);
        }
        return;
    }

    if (!g_rtc_present) return;  // no clock → alarms never arm or fire
    rtc_date_type d; rtc_time_type t; currentClock(d, t);
    uint8_t dow = dayOfWeekMonFirst(d.Year, d.Month, d.Date);
    uint32_t key = ((uint32_t)d.Year * 10000u + (uint32_t)d.Month * 100u + d.Date) * 10000u
                 + (uint32_t)t.Hours * 100u + t.Minutes;
    for (int i = 0; i < ALARM_COUNT; ++i) {
        const Alarm& a = g_alarms[i];
        if (!a.enabled) continue;
        if (!(a.days & (1 << dow))) continue;
        if (a.hour != t.Hours || a.minute != t.Minutes) continue;
        if (g_alarm_last_fire_key[i] == key) continue;
        g_alarm_last_fire_key[i] = key;
        fireAlarm(i);
        return;
    }
}

// Adjust the standby/clock brightness from within standby or while an alarm
// is firing — Fn+`-`/`=` retargets the clock dim level instead of the
// regular brightness, so the user can dial it in without leaving the screen.
static void changeStandbyBrightness(int delta) {
    int n = g_standby_brightness_idx + delta;
    if (n < 0) n = 0;
    if (n > STANDBY_BRIGHTNESS_MAX_IDX) n = STANDBY_BRIGHTNESS_MAX_IDX;
    if (n == g_standby_brightness_idx) return;
    g_standby_brightness_idx = n;
    markStateDirty();
    setBrightnessRampTo(standbyBrightness(), RAMP_SET_MS);
}

// Brightness is context-sensitive: the standby/clock dim level while the
// full-screen clock is up, the normal screen brightness otherwise.
static void changeBrightnessContext(int delta) {
    if (fullScreenClockActive()) changeStandbyBrightness(delta);
    else                         changeBrightness(delta);
}

// Universal back-out (the Esc key). Each screen knows where "back" goes; from
// the Main root, backing out past the top enters the Standby clock.
static void backOut() {
    switch (g_screen) {
        case Screen::AlarmFiring:
        case Screen::AlarmSnoozing: dismissAlarm();        return;
        case Screen::Standby:       exitStandby();         return;
        case Screen::Chess:         exitChess();           return;
        case Screen::ResetModal:    dismissResetModal();   return;
        case Screen::TrackPicker:   exitPickMode(true);    return;
        case Screen::Main:
            if (g_search_active) { exitSearch(); return; }
            if (g_waveform_active || g_spectrum_active || g_viz_test_pattern) {
                snapshotAndDismissViz(); return;
            }
            enterStandby();  // nothing above the root → the clock
            return;
        default:  // Settings, KeyReference, Alarms, SetTime, AlarmEditor, DaysEditor
            goToScreen(parentOf(g_screen));
            return;
    }
}

void setup() {
    auto cfg = M5.config();
    M5Cardputer.begin(cfg, true);
    M5Cardputer.Display.setRotation(1);
    M5Cardputer.Display.setBrightness(userBrightness());
    // M5GFX's autodetect inits the Cardputer panel SPI at 40 MHz. We
    // override after begin() — the bus reconfigures on next transaction.
    // Walking up to narrow the spectrum push window vs panel scan tear.
    static constexpr uint32_t PANEL_SPI_HZ = 80000000;
    M5Cardputer.Display.getPanel()->getBus()->setClock(PANEL_SPI_HZ);
    g_last_activity_ms = millis();

    Serial.begin(115200);

    // ESP8266Audio's diagnostic prints go to `audioLogger` which is
    // silenced by default. Route to Serial so libFLAC error_cb and
    // similar internal messages surface during investigation.
    audioLogger = &Serial;

    // Off-screen back buffer at 8 bpp (RGB332). Halves canvas memory from
    // ~65 KB to ~32 KB at the cost of slight colour quantisation — 256
    // fixed colours instead of 65k. The StampS3FN8 has no PSRAM, so the
    // canvas must live in internal RAM regardless.
    g_canvas.setColorDepth(lgfx::v1::color_depth_t::rgb332_1Byte);
    g_canvas.createSprite(SCREEN_W, SCREEN_H);

    // Increase the speaker's I2S DMA queue: defaults are dma_buf_len=256 /
    // dma_buf_count=8 = ~46 ms at 44.1 kHz mono, which underruns under
    // bursty FLAC decode. 1024 × 8 ≈ 186 ms gives ~4× the headroom while
    // keeping the per-config allocation small enough for internal RAM.
    {
        auto cfg = M5Cardputer.Speaker.config();
        cfg.dma_buf_len   = 1024;
        cfg.dma_buf_count = 8;
        M5Cardputer.Speaker.config(cfg);
    }
    M5Cardputer.Speaker.begin();
    g_out.setSpeaker(&M5Cardputer.Speaker);
    g_out.initSpectrum();

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, SPI, 25000000) || SD.cardType() == CARD_NONE) {
        M5Cardputer.Display.fillScreen(BLACK);
        M5Cardputer.Display.setCursor(0, 0);
        M5Cardputer.Display.println("SD init failed");
        return;
    }

    // Fuzzy search index — reuse if /FZTI.idx + /FZTI.pb match this card,
    // otherwise kick off a background rebuild. Card identity uses
    // SD.cardSize() as a coarse fingerprint (the SD library doesn't expose
    // the true CID); same-size card swap requires the user to press `~` to
    // refresh.
    FuzzyIndex::initAtBoot(SD.cardSize());

    // Restore persisted state before any consumer (volume apply, dir load,
    // playback resume) runs — each falls back to the compiled-in default
    // when its key is absent or the saved value is no longer reachable.
    loadState();
    chess::initAtBoot();
    chess::setRedrawCallback([]() { draw(); });

    // Apply persisted brightness to the panel — and re-seed the ramp
    // state to match — now that loadState has updated g_brightness_idx.
    // The boot-time setBrightness() above used the compiled-in default;
    // without this re-apply the saved value wouldn't take effect until
    // the user's first interaction triggered markActivity's retarget.
    g_brightness_current = userBrightness();
    g_brightness_target  = userBrightness();
    g_brightness_start   = userBrightness();
    M5Cardputer.Display.setBrightness(userBrightness());

    // Try the saved folder; if the SD scan comes back empty (folder gone
    // since last save, or saved value was never valid), fall back to root.
    // Saved cursor restored when the folder loaded; out-of-range gets
    // clamped to the last entry. loadDir resets g_cursor to 0; capture the
    // saved value first and re-apply afterwards.
    int saved_cursor = g_cursor;
    loadDir(g_cur_path);
    if (g_entries.empty() && g_cur_path != "/") loadDir("/");
    if (saved_cursor > 0 && !g_entries.empty()) {
        g_cursor = std::min(saved_cursor, (int)g_entries.size() - 1);
    }

    applyVolume();
    applyLeveling();

    g_audio_mutex   = xSemaphoreCreateMutex();
    g_display_mutex = xSemaphoreCreateMutex();

    // Boot-resume: if a saved track path still exists on the SD card,
    // start it paused via the normal playback path so the audio task
    // (created next) doesn't decode until the user presses space. After
    // setup, also seek to the saved playhead position if one is recorded
    // and lands inside the audio range. The user picks up where they
    // left off (within ~10 s of resolution).
    if (!g_play_path.empty() && SD.exists(g_play_path.c_str())) {
        std::string saved_track = g_play_path;
        if (startPlayback(saved_track, /*start_paused=*/true)) {
            if (g_saved_playhead > 0 && g_src) {
                uint32_t sz = g_src->getSize();
                if (g_saved_playhead >= g_audio_start_offset &&
                    g_saved_playhead < sz) {
                    seekToByte(g_saved_playhead);
                }
            }
        }
        g_saved_playhead = 0;  // consumed
    } else {
        g_play_path.clear();  // saved path is stale; don't show it as the playing track
        g_saved_playhead = 0;
    }

    xTaskCreatePinnedToCore(audioTask, "audio", AUDIO_TASK_STACK / sizeof(StackType_t),
                            nullptr, AUDIO_TASK_PRIO, &g_audio_task, AUDIO_TASK_CORE);
    xTaskCreatePinnedToCore(vizTask, "viz", VIZ_TASK_STACK / sizeof(StackType_t),
                            nullptr, VIZ_TASK_PRIO, &g_viz_task_handle, VIZ_TASK_CORE);
    // Periodic timer at exactly the panel scan rate so renders don't
    // beat against the scan, eliminating the slow tear sweep.
    esp_timer_create_args_t viz_timer_args = {};
    viz_timer_args.callback = vizTimerCallback;
    viz_timer_args.name     = "viz_period";
    esp_timer_handle_t viz_timer = nullptr;
    esp_timer_create(&viz_timer_args, &viz_timer);
    esp_timer_start_periodic(viz_timer, RENDER_PERIOD_US);

    // Per-core idle task handles so the diagnostics poll can read each
    // core's accumulated idle microseconds via vTaskGetInfo().
    g_idle_task_0 = xTaskGetIdleTaskHandleForCPU(0);
    g_idle_task_1 = xTaskGetIdleTaskHandleForCPU(1);
    g_cpu_sample_us_prev = esp_timer_get_time();

    g_ipc_task_0 = xTaskGetHandle("ipc0");
    g_ipc_task_1 = xTaskGetHandle("ipc1");
    if (g_ipc_task_0 && g_ipc_task_1) {
        uint32_t free0 = (uint32_t)uxTaskGetStackHighWaterMark(g_ipc_task_0)
                         * sizeof(StackType_t);
        uint32_t free1 = (uint32_t)uxTaskGetStackHighWaterMark(g_ipc_task_1)
                         * sizeof(StackType_t);
        Serial.printf("[ipc] ipc0 min_free=%u ipc1 min_free=%u\n",
                      (unsigned)free0, (unsigned)free1);
    }

    setupRtc();

    pollBattery(true);
    draw();

    fuzzyHarnessSetup();
}

void loop() {
    M5Cardputer.update();
    pollBattery();
    pollDiagnostics();
    pollMarquee();
    flushStateIfDirty();
    pollIndexingSpinner();
    // pollVisualisation() now runs in its own RTOS task — see vizTask.
    pollIMUActivity();
    pollRtcClock();
    pollAlarms();
    pollIdleScreen();
    pollBrightnessRamp();
    pollHeapDiag();
    fuzzyHarnessPoll();

    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        auto state = M5Cardputer.Keyboard.keysState();

        // Any keypress counts as activity — wakes the screen back to
        // full brightness if it had dimmed or slept.
        markActivity();

        // First-keypress: swap the version out of the header for the
        // breadcrumb. Force a header redraw so the change shows now —
        // not all keypresses trigger a full screen refresh, so we can't
        // rely on later draws to pick this one-shot transition up.
        if (!g_first_keypress_seen) {
            g_first_keypress_seen = true;
            drawHeader();
        }

        // --- Universal pre-pass, before the per-screen dispatch -----------
        // Category 1 (Back): `` ` `` backs out on any non-clock screen; the
        // clock screens keep their own `` ` `` (dismiss / timed exit) below.
        // Category 2 (global transport): pause / volume / skip / seek and
        // brightness reach every screen per their scope, so menus and chess
        // keep playback control without intercepting it per-branch.
        bool consumed = false;
        // Standby is excluded so its post-entry guard (below) governs `` ` ``;
        // every other screen backs out (firing/snooze → dismiss).
        if (g_screen != Screen::Standby) {
            for (char c : state.word) if (c == '`') { backOut(); consumed = true; break; }
        }
        // Brightness rides Ctrl everywhere but the modal (context-sensitive).
        // Skip rides Ctrl wherever transport is allowed.
        if (!consumed && state.ctrl && g_screen != Screen::ResetModal) {
            for (char c : state.word) {
                if      (c == '-' || c == '_') { changeBrightnessContext(-1); consumed = true; }
                else if (c == '=' || c == '+') { changeBrightnessContext(+1); consumed = true; }
                else if ((c == '[' || c == '{') && transportAllowed()) { skipTrack(-1); consumed = true; }
                else if ((c == ']' || c == '}') && transportAllowed()) { skipTrack(+1); consumed = true; }
            }
        }
        // Plain transport — pause / volume / seek — on every transport-allowed
        // screen (browser, menus, chess, viz; not the clock/modal/text screens,
        // which handle these keys themselves). Seek itself is driven by the
        // held-key poll; the tap is consumed here so it doesn't fall through.
        if (!consumed && !state.ctrl && transportAllowed()) {
            if (state.space) { togglePause(); consumed = true; }
            for (char c : state.word) {
                if      (c == '-') { changeVolume(-1); consumed = true; }
                else if (c == '=') { changeVolume(+1); consumed = true; }
                else if (c == '[' || c == ']') { consumed = true; }
            }
        }
        // Mode switches (Ctrl + letter / Ctrl+/) live here in one place rather
        // than duplicated per browser/viz branch — the duplication was the
        // source of repeated missing-binding bugs. They act from the Main
        // screen (browser / search / visualisation); the track picker just
        // absorbs Ctrl so a stray combo doesn't leak through to search.
        if (!consumed && state.ctrl &&
            (g_screen == Screen::Main || g_screen == Screen::TrackPicker)) {
            if (g_screen == Screen::Main) {
                for (char c : state.word) {
                    if      (c == 'W' || c == 'w') toggleWaveform();
                    else if (c == 'S' || c == 's') toggleSpectrum();
                    else if (c == 'T' || c == 't') toggleTestPattern();
                    else if (c == 'H' || c == 'h') requestChess();
                    else if (c == 'D' || c == 'd') toggleDiagnostics();
                    else if (c == 'L' || c == 'l') toggleLeveling();
                    else if (c == '?')             showSettings();   // Ctrl+/
                }
            }
            consumed = true;
        }

        if (consumed) {
            // Handled by the universal pre-pass above.
        } else if (g_screen == Screen::AlarmFiring) {
            // Alarm sounding. `` ` `` (pre-pass) stops and restores the prior
            // track (paused); Enter wakes to the alarm track, leaving it
            // playing; +/- adjust volume; any other key snoozes for 8 minutes.
            if (state.enter) {
                acceptAlarm();
            } else {
                bool snooze = false;
                for (char c : state.word) {
                    if      (c == '-') { changeVolume(-1); g_alarm_ramp_overridden = true; }
                    else if (c == '=') { changeVolume(+1); g_alarm_ramp_overridden = true; }
                    else { snooze = true; }
                }
                if (snooze) snoozeAlarm();
            }
        } else if (g_screen == Screen::AlarmSnoozing) {
            // Snoozing: clock-only, waiting for re-fire. `` ` `` (pre-pass)
            // stops and restores the prior track (paused); Enter wakes to the
            // alarm track from the top; everything else is ignored.
            if (state.enter) wakeFromSnooze();
        } else if (g_screen == Screen::Standby) {
            // Standby: any key exits (after a short post-entry guard so the
            // entry keypress's own transients don't immediately exit). `` ` ``
            // is intercepted by the pre-pass only on non-clock screens, so it
            // reaches here and exits like any other key.
            bool too_soon = (millis() - g_standby_entered_ms) < 400;
            if (!too_soon && !state.word.empty()) exitStandby();
        } else if (chess::active()) {
            // Chess owns the keyboard while up. handleKey returns true when
            // the key was consumed; a false return means the user pressed
            // something chess doesn't recognise, so we drop the mode and
            // let the next press land in the regular dispatch.
            bool kept = chess::handleKey(state);
            if (!kept) {
                exitChess();
            } else {
                draw();
            }
        } else if (g_screen == Screen::ChessConfirm) {
            // Enter confirms (pause playback, free memory, open chess); any
            // other key cancels back to the browser. `` ` `` cancels via the
            // pre-pass (backOut → Main).
            if (state.enter) {
                enterChessFreeingAudio();
            } else if (!state.word.empty() || state.del || state.space) {
                goToScreen(Screen::Main);
            }
        } else if (g_screen == Screen::ResetModal) {
            // `/` confirms the destructive action; any other key — letter,
            // del, space, anything — cancels and dismisses.
            bool confirm = false;
            for (char c : state.word) if (c == '/') confirm = true;
            if (confirm) {
                confirmReset();
            } else if (!state.word.empty() || state.del || state.space) {
                dismissResetModal();
            }
        } else if (g_screen == Screen::KeyReference) {
            // Key-reference sub-screen: `;` / `.` scroll, any other key
            // returns to wherever we came from (Settings, normally).
            // Releasing `?` while Shift is still held also fires
            // isChange() with empty state.word — without the non-empty
            // guard, the screen would flash away the moment the user lets
            // go of Shift+/.
            bool handled = false;
            for (char c : state.word) {
                if (c == ';') { scrollHelp(-1); handled = true; }
                else if (c == '.') { scrollHelp(+1); handled = true; }
            }
            if (!handled && !state.word.empty()) {
                goToScreen(Screen::Settings);
            }
        } else if (g_screen == Screen::SetTime) {
            if (state.enter) {
                activateSetTimeRow();
            } else if (state.del) {
                exitSetTime();
            } else {
                for (char c : state.word) {
                    if      (c == ';') moveSetTimeCursor(-1);
                    else if (c == '.') moveSetTimeCursor(+1);
                    else if (c == ',') adjustSetTimeRow(-1);
                    else if (c == '/') {
                        int row = g_sct_cursor;
                        if (row == SCT_COMMIT || row == SCT_BACK) activateSetTimeRow();
                        else adjustSetTimeRow(+1);
                    }
                }
            }
        } else if (g_screen == Screen::DaysEditor) {
            // Days toggles (nested under the alarm editor). `;`/`.` move,
            // `,`/`/`/Enter toggle the highlighted day (or Back), Del backs out.
            // (Back via `` ` `` and transport are handled by the pre-pass.)
            if (state.enter) {
                activateDaysEditorRow();
            } else if (state.del) {
                exitDaysEditor();
            } else {
                for (char c : state.word) {
                    if      (c == ';') moveDaysEditorCursor(-1);
                    else if (c == '.') moveDaysEditorCursor(+1);
                    else if (c == ',' || c == '/') activateDaysEditorRow();
                }
            }
        } else if (g_screen == Screen::AlarmEditor) {
            if (state.enter) {
                activateAlarmEditorRow();
            } else if (state.del) {
                exitAlarmEditor();
            } else {
                for (char c : state.word) {
                    if      (c == ';') moveAlarmEditorCursor(-1);
                    else if (c == '.') moveAlarmEditorCursor(+1);
                    else if (c == ',') adjustAlarmEditorRow(-1);
                    else if (c == '/') {
                        // `/` increments numerics / activates actions, mirroring
                        // Alarms/Settings.
                        int row = g_alarm_editor_cursor;
                        if (row == AE_TRACK || row == AE_PREVIEW || row == AE_BACK
                            || row == AE_ENABLED || row == AE_DAYS) {
                            activateAlarmEditorRow();
                        } else {
                            adjustAlarmEditorRow(+1);
                        }
                    }
                }
            }
        } else if (g_screen == Screen::Alarms) {
            // Alarms screen: same nav idiom as Settings, plus Enter as a
            // sub-menu activator and Del as an explicit back. With no RTC the
            // screen is a bare warning — only Del / `` ` `` (pre-pass) exit.
            if (!g_rtc_present) {
                if (state.del) exitAlarms();
            } else if (state.enter) {
                activateAlarmsRow();
            } else if (state.del) {
                exitAlarms();
            } else {
                for (char c : state.word) {
                    if      (c == ';') moveAlarmsCursor(-1);
                    else if (c == '.') moveAlarmsCursor(+1);
                    else if (c == ',') adjustAlarmsRow(-1);
                    else if (c == '/') adjustAlarmsRow(+1);
                }
            }
        } else if (g_screen == Screen::Leveling) {
            // Leveling sub-menu: same nav idiom as Alarms.
            if (state.enter) {
                activateLevelingRow();
            } else if (state.del) {
                exitLeveling();
            } else {
                for (char c : state.word) {
                    if      (c == ';') moveLevelingCursor(-1);
                    else if (c == '.') moveLevelingCursor(+1);
                    else if (c == ',') adjustLevelingRow(-1);
                    else if (c == '/') adjustLevelingRow(+1);
                }
            }
        } else if (g_screen == Screen::Settings) {
            // Settings screen: ; / . move cursor, , / / adjust the row (toggle
            // off/on, numeric -/+ , or activate an action row). Back is `` ` ``
            // (pre-pass) or Del; transport passes through the pre-pass.
            if (state.del) {
                dismissSettings();
            } else {
                for (char c : state.word) {
                    if      (c == ';') moveSettingsCursor(-1);
                    else if (c == '.') moveSettingsCursor(+1);
                    else if (c == ',') adjustSettingsRow(-1);
                    else if (c == '/') adjustSettingsRow(+1);
                    // Any letter dismisses — the user is reaching for fuzzy
                    // search or just wants out. The letter itself is
                    // consumed; the next press lands in the browser.
                    else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                        dismissSettings();
                        break;
                    }
                }
            }
        } else if (g_waveform_active || g_spectrum_active || g_viz_test_pattern) {
            // Visualisation overlay (waveform or spectrum): transport,
            // volume, and brightness keys pass through so the user can
            // keep controlling playback while watching. Everything else
            // dismisses — the user is leaving the overlay to navigate,
            // search, or do something structural.
            bool dismiss = false;
            if (state.del || state.tab) {
                dismiss = true;
            } else {
                for (char c : state.word) {
                    switch (c) {
                        // Transport (pause / volume / skip / seek) and back are
                        // handled by the pre-pass; here only the viz-specific
                        // playback jumps remain.
                        case '\'': jumpToPlaying(); break;
                        case '1': case '2': case '3': case '4': case '5':
                        case '6': case '7': case '8': case '9': case '0': {
                            int digit_index = (c == '0') ? 9 : (c - '1');
                            jumpToTenth(digit_index);
                            break;
                        }
                        // Everything else — navigation, structural — dismisses.
                        default: dismiss = true; break;
                    }
                    if (dismiss) break;
                }
            }
            if (dismiss) {
                snapshotAndDismissViz();
            }
        } else if (state.tab) {
            // Tab outside viz overlay restores the last-shown combination
            // — flicks the visualisations back in without re-selecting which
            // ones. The in-overlay Tab branch handles the hide direction.
            if (!g_search_active) {
                restoreVizFromSnapshot();
            }
        } else if (state.del) {
            // In search mode, backspace edits the query and exits when
            // empty. In track-pick mode, Del cancels back to the editor.
            // Otherwise, Del opens the reset confirmation.
            if (g_search_active)    searchBackspace();
            else if (g_pick_slot >= 0) exitPickMode(/*restore_path=*/true);
            else                    showResetModal();
        } else {
            // Plain bindings — fire only when Fn is not held. In search
            // mode, alphanumeric characters and space append to the query;
            // every other key keeps its existing browser binding.
            for (char c : state.word) {
                if (g_search_active) {
                    bool textual = (c >= 'a' && c <= 'z') ||
                                   (c >= 'A' && c <= 'Z') ||
                                   (c >= '0' && c <= '9') ||
                                   c == ' ';
                    if (textual) { searchAppend(c); continue; }
                }
                switch (c) {
                    case ',': ascend();   break;
                    case '/':
                        if (g_search_active) searchActivate();
                        else                 activateSelection();
                        break;
                    case ' ': togglePause(); break;
                    case '=': changeVolume(+1); break;
                    case '-': changeVolume(-1); break;
                    case '\\': toggleWrapNames(); break;
                    case '\'': jumpToPlaying(); break;
                    case '1': case '2': case '3': case '4': case '5':
                    case '6': case '7': case '8': case '9': case '0': {
                        int digit_index = (c == '0') ? 9 : (c - '1');
                        jumpToTenth(digit_index);
                        break;
                    }
                    default:
                        // Any letter (a–z / A–Z) opens fuzzy search and seeds
                        // the query with that character. The waveform-overlay
                        // dismissal is handled higher up so we never reach
                        // this case while the waveform is on top.
                        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
                            enterSearch(c);
                        }
                        break;
                }
            }
            // `enter` is unused throughout — `/` is the universal
            // activate key (descends dirs, starts audio, activates
            // search results, confirms reset modal, activates Settings
            // action rows).
        }
    }

    pollSeekKeys();
    pollBrowserNavKeys();
    pollSettingsKeys();
    pollVolumeKeys();

    if (g_advance_pending) {
        g_advance_pending = false;
        if (g_screen == Screen::AlarmFiring && !g_alarm_beep_fallback) {
            // Loop the alarm track: restart it from the top rather than
            // advancing to the next file in the folder.
            startPlayback(g_alarms[g_alarm_fire_slot].track);
            applyVolume();
        } else if (g_auto_play_next) skipTrack(+1);
        else                         drawFooter();
    }

    bool playing = false;
    if (g_audio_mutex) {
        xSemaphoreTake(g_audio_mutex, portMAX_DELAY);
        playing = (g_audio_active && !g_paused);
        xSemaphoreGive(g_audio_mutex);
    }
    if (playing && millis() - g_last_progress_ms > 500) {
        g_last_progress_ms = millis();
        if (!overlayActive() && g_screen_state != SCREEN_OFF) {
            drawSlotProgress();
            presentFrame();
        }
    }
    // Periodic playhead persistence — marks state dirty every 10 s while
    // playback is active so the saved position tracks the listener within
    // ~10 s on power-cut.
    if (playing && millis() - g_playhead_save_ms > PLAYHEAD_SAVE_INTERVAL_MS) {
        g_playhead_save_ms = millis();
        markStateDirty();
    }
    delay(10);
}
