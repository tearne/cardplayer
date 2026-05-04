// Stripped-down measurement build for evaluating ESP32-audioI2S as a
// possible replacement for arduino-audio-tools. Plays a hard-coded
// reference file from SD and logs heap evolution at the same milestones
// the production firmware does, for direct comparison.
//
// Production cardputer-adv build is unaffected — this main only compiles
// in the eval-audioi2s PlatformIO environment.

#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <Audio.h>
#include <esp_heap_caps.h>

// SD pinout (matches production main)
static constexpr int SD_SCK  = 40;
static constexpr int SD_MISO = 39;
static constexpr int SD_MOSI = 14;
static constexpr int SD_CS   = 12;

// I2S pinout for Cardputer ADV — taken from M5Unified's per-board table.
// ES8311 codec is configured separately via I2C by M5.Speaker.begin().
static constexpr int I2S_BCLK = 41;
static constexpr int I2S_LRC  = 43;
static constexpr int I2S_DOUT = 42;

// Edit this to point at the file under test on the SD card.
static const char* TEST_FILE =
    "/Hostage (Plecta Edit 2020) [YaSXaptWg04]-Billie Eilish.m4a";

Audio audio;

// --- Memory tap ----------------------------------------------------------
// Same shape as the production firmware's tap so readings line up directly.

static volatile uint32_t g_fast_tap_start_ms = 0;
static volatile uint32_t g_fast_tap_until_ms = 0;
static uint32_t          g_fast_tap_last_ms  = 0;
static constexpr uint32_t FAST_TAP_INTERVAL_MS = 10;
static constexpr uint32_t FAST_TAP_DURATION_MS = 2000;

static void armFastMemoryTap() {
    uint32_t now = millis();
    g_fast_tap_start_ms = now;
    g_fast_tap_until_ms = now + FAST_TAP_DURATION_MS;
    g_fast_tap_last_ms  = 0;
}

static void sampleFastMemoryTap() {
    uint32_t now = millis();
    if (now >= g_fast_tap_until_ms) return;
    if (now - g_fast_tap_last_ms < FAST_TAP_INTERVAL_MS) return;
    g_fast_tap_last_ms = now;
    Serial.printf("[mem.fast] t=%4u  int free=%u largest=%u  8bit free=%u largest=%u\n",
                  (unsigned)(now - g_fast_tap_start_ms),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void logMem(const char* tag) {
    Serial.printf("[mem] %-13s free=%u  largest=%u\n", tag,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

// --- Setup / loop --------------------------------------------------------

void setup() {
    auto cfg = M5.config();
    cfg.internal_spk = true;
    M5Cardputer.begin(cfg);

    // M5.Speaker.begin() runs the ES8311 enable I2C sequence (DAC power,
    // output enable, volume). Calling end() releases the I2S driver but
    // the codec's disable callback is a no-op (only sends a terminator),
    // so the codec remains powered up. This frees I2S_NUM_1 for
    // ESP32-audioI2S.
    M5.Speaker.begin();
    M5.Speaker.end();

    SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS)) {
        Serial.println("SD mount failed");
        while (true) delay(1000);
    }

    logMem("boot_complete");

    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(8);  // 0..21

    Serial.printf("playing: %s\n", TEST_FILE);
    armFastMemoryTap();
    audio.connecttoFS(SD, TEST_FILE);
    logMem("post_start");
}

void loop() {
    audio.loop();
    sampleFastMemoryTap();

    static uint32_t last_steady = 0;
    if (millis() - last_steady > 1000) {
        last_steady = millis();
        logMem("steady");
    }
}
