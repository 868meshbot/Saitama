// Saitama — Sound.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later

#include "Sound.h"
#include "Config.h"
#include "Log.h"

#include <driver/i2s.h>
#include <cmath>
#include <cstring>

// MAX98357A I2S amplifier — LilyGo T-Deck Plus
// Pins verified against T-Deck Plus schematic; not shared with TFT/LoRa/SD SPI.
#define SPK_I2S_PORT   I2S_NUM_0
#define SPK_BCLK       7
#define SPK_WS         5
#define SPK_DOUT       6

// 8 kHz mono 16-bit, 150 ms ping = 1200 samples
static constexpr int   SAMPLE_RATE  = 8000;
static constexpr int   PING_SAMPLES = 1200;
static constexpr float PING_HZ      = 880.0f;  // A5 — clean, short ping

// Pluck: sharp attack + exponential decay, 100 ms
static constexpr int PLUCK_SAMPLES = 800;

// Clear: 4-note C-major ascending arpeggio ("level complete"), ~200 ms
static constexpr int CLEAR_SAMPLES = 1600;

// Whoosh: exponential frequency sweep 80 Hz → 2400 Hz with bell envelope, ~150 ms
static constexpr int WHOOSH_SAMPLES = 1200;

// GBA-style startup jingle: B-major ascending arpeggio
// B2(bass hit 200ms) → B3 → D#4 → F#4 → B4 → D#5 → F#5 → B5(hold 600ms) = ~1.3 s
static constexpr int   JINGLE_NOTES = 8;
static constexpr float JINGLE_FREQS[JINGLE_NOTES] = {
    123.47f,   // B2  — characteristic low bass hit
    246.94f,   // B3
    311.13f,   // D#4
    369.99f,   // F#4
    493.88f,   // B4
    622.25f,   // D#5
    739.99f,   // F#5
    987.77f,   // B5  — sustained bright finish
};
static constexpr int   JINGLE_LENS[JINGLE_NOTES] = { 1600, 640, 640, 640, 640, 640, 640, 4800 };
static constexpr int   JINGLE_TOTAL = 10240;  // 1600 + 640*6 + 4800

// Applies cfg.speakerVolume (0–100) scaling when writing to I2S DMA.
// Chunks through a 256-sample stack buffer to avoid a heap allocation.
static void _writeScaled(const int16_t* buf, int count)
{
    const float scale = (float)ops::config::get().speakerVolume / 100.0f;
    int16_t tmp[256];
    size_t written;
    while (count > 0) {
        int n = (count < 256) ? count : 256;
        for (int i = 0; i < n; i++)
            tmp[i] = (int16_t)((float)buf[i] * scale);
        i2s_write(SPK_I2S_PORT, tmp, (size_t)n * sizeof(int16_t), &written, pdMS_TO_TICKS(100));
        buf += n;
        count -= n;
    }
}

// Waveform buffers live in PSRAM — allocated in init() to free ~29 KB of
// internal DRAM for the Bluedroid stack. CPU-read only; never DMA-accessed.
static int16_t* s_pingBuf   = nullptr;
static int16_t* s_pluckBuf  = nullptr;
static int16_t* s_clearBuf  = nullptr;
static int16_t* s_whooshBuf = nullptr;
static int16_t* s_jingleBuf = nullptr;
static bool     s_initialized = false;
static uint32_t s_soundEndMs  = 0;  // millis() deadline while DMA is draining

static void _generatePing()
{
    for (int i = 0; i < PING_SAMPLES; i++) {
        float t = (float)i / (float)SAMPLE_RATE;
        float env = 1.0f;
        if (i < 100)                  env = (float)i / 100.0f;           // 12.5 ms fade-in
        else if (i > PING_SAMPLES - 150) env = (float)(PING_SAMPLES - i) / 150.0f; // ~19 ms fade-out
        s_pingBuf[i] = (int16_t)(sinf(2.0f * M_PI * PING_HZ * t) * env * 14000.0f);
    }
}

static void _generatePluck()
{
    for (int i = 0; i < PLUCK_SAMPLES; i++) {
        float t = (float)i / (float)SAMPLE_RATE;
        float env;
        if (i < 5) env = (float)i / 5.0f;
        else env = expf(-7.0f * (float)(i - 5) / (float)(PLUCK_SAMPLES - 5));
        s_pluckBuf[i] = (int16_t)(sinf(2.0f * M_PI * 880.0f * t) * env * 16000.0f);
    }
}

static void _generateClear()
{
    // C-major ascending arpeggio: C5 → E5 → G5 → C6 (level-complete fanfare)
    // Notes 1-3: 320 samples each (40 ms); Note 4: 640 samples (80 ms) with exp decay.
    const float freqs[4] = { 523.25f, 659.25f, 783.99f, 1046.5f };
    const int   lens[4]  = { 320, 320, 320, 640 };
    int pos = 0;
    for (int n = 0; n < 4; n++) {
        const float freq = freqs[n];
        const int   len  = lens[n];
        const int   fade = 12;
        for (int i = 0; i < len; i++) {
            float t = (float)pos / (float)SAMPLE_RATE;
            float env;
            if (n < 3) {
                // Short notes: linear fade in/out
                if (i < fade)        env = (float)i / (float)fade;
                else if (i > len-fade) env = (float)(len - i) / (float)fade;
                else                   env = 1.0f;
            } else {
                // Final note: sharp attack then exponential decay
                if (i < fade) env = (float)i / (float)fade;
                else          env = expf(-4.0f * (float)(i - fade) / (float)(len - fade));
            }
            s_clearBuf[pos++] = (int16_t)(sinf(2.0f * M_PI * freq * t) * env * 14000.0f);
        }
    }
}

static void _generateWhoosh()
{
    // Exponential sweep 80 Hz → 2400 Hz with bell amplitude — sounds like a swoosh.
    float phase = 0.0f;
    const float f0 = 80.0f, f1 = 2400.0f;
    for (int i = 0; i < WHOOSH_SAMPLES; i++) {
        float frac = (float)i / (float)(WHOOSH_SAMPLES - 1);
        float freq = f0 * powf(f1 / f0, frac);   // exponential sweep
        phase += 2.0f * M_PI * freq / (float)SAMPLE_RATE;
        if (phase > 2.0f * M_PI) phase -= 2.0f * M_PI;
        float env = sinf(M_PI * frac);             // bell: 0 → 1 → 0
        s_whooshBuf[i] = (int16_t)(sinf(phase) * env * 16000.0f);
    }
}

static void _generateJingle()
{
    int pos = 0;
    for (int n = 0; n < JINGLE_NOTES; n++) {
        const float freq = JINGLE_FREQS[n];
        const int   len  = JINGLE_LENS[n];
        // Proportional fade: 1/8 of note length — gives ~25ms on bass, ~10ms on
        // middle notes, and ~75ms natural decay on the final sustained note.
        const int   fade = len / 8;
        for (int i = 0; i < len; i++) {
            float t   = (float)pos / (float)SAMPLE_RATE;
            float env = 1.0f;
            if (i < fade)          env = (float)i / (float)fade;
            else if (i > len-fade) env = (float)(len - i) / (float)fade;
            s_jingleBuf[pos++] = (int16_t)(sinf(2.0f * M_PI * freq * t) * env * 14000.0f);
        }
    }
}

namespace ops {

void sound::init()
{
    s_pingBuf   = (int16_t*)ps_malloc(PING_SAMPLES   * sizeof(int16_t));
    s_pluckBuf  = (int16_t*)ps_malloc(PLUCK_SAMPLES  * sizeof(int16_t));
    s_clearBuf  = (int16_t*)ps_malloc(CLEAR_SAMPLES  * sizeof(int16_t));
    s_whooshBuf = (int16_t*)ps_malloc(WHOOSH_SAMPLES * sizeof(int16_t));
    s_jingleBuf = (int16_t*)ps_malloc(JINGLE_TOTAL   * sizeof(int16_t));
    if (!s_pingBuf || !s_pluckBuf || !s_clearBuf || !s_whooshBuf || !s_jingleBuf) {
        OPS_LOG("Sound", "ps_malloc failed for waveform buffers");
        return;
    }
    _generatePing();
    _generatePluck();
    _generateClear();
    _generateWhoosh();
    _generateJingle();

    i2s_config_t cfg = {};
    cfg.mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate          = SAMPLE_RATE;
    cfg.bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT;
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags     = 0;
    // 4 × 512 = 2048 samples of DMA capacity — whole ping fits, so i2s_write
    // returns as soon as samples are queued (no long block on the main loop).
    cfg.dma_buf_count        = 4;
    cfg.dma_buf_len          = 512;
    cfg.use_apll             = false;
    cfg.tx_desc_auto_clear   = true;

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = I2S_PIN_NO_CHANGE;
    pins.bck_io_num   = SPK_BCLK;
    pins.ws_io_num    = SPK_WS;
    pins.data_out_num = SPK_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;

    esp_err_t err = i2s_driver_install(SPK_I2S_PORT, &cfg, 0, nullptr);
    if (err != ESP_OK) {
        OPS_LOG("Sound", "i2s_driver_install failed: %d", (int)err);
        return;
    }
    i2s_set_pin(SPK_I2S_PORT, &pins);
    i2s_zero_dma_buffer(SPK_I2S_PORT);
    s_initialized = true;
    OPS_LOG("Sound", "I2S ready BCLK=%d WS=%d DATA=%d @ %dHz", SPK_BCLK, SPK_WS, SPK_DOUT, SAMPLE_RATE);
}

bool sound::isPlaying()
{
    return (millis() < s_soundEndMs);
}

void sound::playPing()
{
    const auto& cfg = ops::config::get();
    // speakerEnabled is the master switch; notifySound gates DM pings
    if (!cfg.speakerEnabled || !cfg.notifySound) return;
    if (!s_initialized) return;

    // I2S APB clock is only stable at ≥80 MHz CPU. Governors 0/1 can drop below
    // that during screensaver. Boost momentarily; _applyGovFreq() will re-lower
    // only after isPlaying() clears.
    if (getCpuFrequencyMhz() < 80) setCpuFrequencyMhz(80);
    s_soundEndMs = millis() + 300;

    _writeScaled(s_pingBuf, PING_SAMPLES);
}

void sound::playNotification()
{
    const auto& cfg = ops::config::get();
    if (!cfg.speakerEnabled || !cfg.notifySound) return;
    if (!s_initialized) return;

    if (getCpuFrequencyMhz() < 80) setCpuFrequencyMhz(80);
    s_soundEndMs = millis() + 300;

    switch (cfg.notifySoundChoice) {
        case 1:  _writeScaled(s_pluckBuf,  PLUCK_SAMPLES);  break;
        case 2:  _writeScaled(s_clearBuf,  CLEAR_SAMPLES);  break;
        case 3:  _writeScaled(s_whooshBuf, WHOOSH_SAMPLES); break;
        default: _writeScaled(s_pingBuf,   PING_SAMPLES);   break;
    }
}

void sound::playPreview(uint8_t choice)
{
    if (!ops::config::get().speakerEnabled) return;
    if (!s_initialized) return;
    if (getCpuFrequencyMhz() < 80) setCpuFrequencyMhz(80);
    s_soundEndMs = millis() + 300;
    switch (choice) {
        case 1:  _writeScaled(s_pluckBuf,  PLUCK_SAMPLES);  break;
        case 2:  _writeScaled(s_clearBuf,  CLEAR_SAMPLES);  break;
        case 3:  _writeScaled(s_whooshBuf, WHOOSH_SAMPLES); break;
        default: _writeScaled(s_pingBuf,   PING_SAMPLES);   break;
    }
}

void sound::playStartupJingle()
{
    if (!s_initialized) return;
    if (!ops::config::get().speakerEnabled) return;

    // 10240 samples at 8 kHz = ~1.3 s. Called before ui::init() — blocking is acceptable.
    _writeScaled(s_jingleBuf, JINGLE_TOTAL);
}

}  // namespace ops
