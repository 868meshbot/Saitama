// Saitama — MP3Player.cpp
// Copyright 2026 Saitama — MIT License
//
// Streams an MP3 file from SD to the MAX98357A I2S speaker via a FreeRTOS
// task pinned to Core 0.  Decoding is handled by ESP8266Audio's
// AudioGeneratorMP3 (helix fixed-point, heap-allocated state) so the task
// stack stays small (~12 KB vs ~17 KB for minimp3's stack-heavy synth path).
//
// I2S driver lifecycle: Sound::init() installs the driver at 8 kHz mono.
// OpsI2SOutput::begin() fully uninstalls and reinstalls it at the file's
// sample rate in stereo (I2S_CHANNEL_FMT_RIGHT_LEFT) so BCLK = hz×16×2 is
// always correct.  stop() reinstalls it back to Sound's 8 kHz config.
// i2s_set_clk() with I2S_CHANNEL_MONO was avoided because it miscalculates
// BCLK as hz×16×1, halving the bit clock and producing silence.
//
// SD cross-core note: AudioFileSourceSD is constructed inside the task on
// Core 0, so SD.open() is called from Core 0.  The Arduino SPI driver is
// internally thread-safe; the 80 ms yield before open lets any pending TFT
// DMA flush drain before we touch the shared bus.

#include "MP3Player.h"
#include "Log.h"

#include <SD.h>
#include <driver/i2s.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <cstring>

#define PLAYER_I2S_PORT  I2S_NUM_0
#define RESTORE_RATE     8000u
// MAX98357A speaker pins — must match Sound.cpp
#define SPK_BCLK  7
#define SPK_WS    5
#define SPK_DOUT  6

namespace ops {
namespace mp3player {

static volatile State  s_state    = State::Idle;
static volatile float  s_progress = 0.0f;
static char            s_filename[64] = {};
static char            s_pathBuf[128] = {};

static TaskHandle_t    s_task  = nullptr;
static volatile bool   s_stop  = false;
static volatile bool   s_pause = false;

// ── AudioOutput subclass ──────────────────────────────────────────────────────
// Bridges ESP8266Audio's sample callbacks to the I2S speaker.
// Holds pointers to the module-level s_stop/s_pause flags so ConsumeSample can
// signal AudioGeneratorMP3::loop() to exit mid-frame when a stop is requested,
// instead of waiting up to 200 ms × 5 flushes per frame for the outer while
// loop to notice.

class OpsI2SOutput : public AudioOutput
{
    // 512 int16_t slots = 256 stereo pairs (L then R interleaved)
    static constexpr int BUF = 512;
    int16_t _buf[BUF];
    int     _fill = 0;
    volatile bool*        const _pStop;
    volatile bool*        const _pPause;
    volatile State*       const _pState;
    AudioFileSource*      const _pSrc;
    volatile float*       const _pProgress;
    uint32_t                    _fileSize = 0;

    void _flush()
    {
        if (_fill == 0) return;
        size_t written;
        // 20 ms cap: a 256-sample DMA slot drains in 5.8 ms at 44.1 kHz, so
        // this never drops audio under normal conditions, but it keeps each
        // flush short enough that the task can check stop/pause within ~100 ms
        // (ceil(1152/256) = 5 flushes × 20 ms) instead of 5 × 200 ms = 1 s.
        i2s_write(PLAYER_I2S_PORT, _buf, (size_t)_fill * 2,
                  &written, pdMS_TO_TICKS(20));
        _fill = 0;
        // Progress: mp3->loop() is greedy (processes the whole file in one call),
        // so the outer task loop never gets a chance to update s_progress.
        // Update it here instead — every 256 decoded stereo pairs (~5.8 ms of audio).
        if (_fileSize > 0 && _pProgress)
            *_pProgress = (float)_pSrc->getPos() / (float)_fileSize;
        vTaskDelay(1);
    }

    static void _installI2S(uint32_t hz, i2s_channel_fmt_t fmt,
                             int dmaCount, int dmaBufLen, bool apll)
    {
        i2s_driver_uninstall(PLAYER_I2S_PORT);

        i2s_config_t cfg    = {};
        cfg.mode            = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
        cfg.sample_rate     = hz;
        cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
        cfg.channel_format  = fmt;
        cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
        cfg.intr_alloc_flags     = 0;
        cfg.dma_buf_count        = dmaCount;
        cfg.dma_buf_len          = dmaBufLen;
        cfg.use_apll             = apll;
        cfg.tx_desc_auto_clear   = true;

        i2s_pin_config_t pins = {};
        pins.mck_io_num   = I2S_PIN_NO_CHANGE;
        pins.bck_io_num   = SPK_BCLK;
        pins.ws_io_num    = SPK_WS;
        pins.data_out_num = SPK_DOUT;
        pins.data_in_num  = I2S_PIN_NO_CHANGE;

        i2s_driver_install(PLAYER_I2S_PORT, &cfg, 0, nullptr);
        i2s_set_pin(PLAYER_I2S_PORT, &pins);
    }

public:
    OpsI2SOutput(volatile bool* pStop, volatile bool* pPause, volatile State* pState,
                 AudioFileSource* pSrc, volatile float* pProgress)
        : _pStop(pStop), _pPause(pPause), _pState(pState),
          _pSrc(pSrc), _pProgress(pProgress), _fill(0) {}

    // Called by library after SetRate/SetChannels/SetBitsPerSample are resolved.
    bool begin() override
    {
        _fill = 0;
        _fileSize = _pSrc ? _pSrc->getSize() : 0;
        uint32_t hz = (hertz > 0) ? (uint32_t)hertz : 44100u;
        // Stereo interleaved (RIGHT_LEFT) so BCLK = hz×16×2 — correct for I2S wire.
        // 8 DMA buffers × 256 samples each = 46 ms of headroom at 44.1 kHz.
        _installI2S(hz, I2S_CHANNEL_FMT_RIGHT_LEFT, 8, 256, false);
        OPS_LOG("MP3", "I2S -> %u Hz stereo", hz);
        return true;
    }

    // Library calls this when the first MP3 frame reveals the sample rate.
    bool SetRate(int hz) override
    {
        hertz = hz;
        _installI2S((uint32_t)hz, I2S_CHANNEL_FMT_RIGHT_LEFT, 8, 256, false);
        return true;
    }

    // Called once per decoded stereo sample pair.
    bool ConsumeSample(int16_t sample[2]) override
    {
        // Stop: signal the library to exit mp3->loop() immediately.
        if (*_pStop) return false;

        // Pause: flush the partial DMA buffer so audio goes silent quickly, then
        // spin here (inside mp3->loop()) until resumed.  Updating _pState directly
        // means the UI timer sees the Paused state without waiting for the outer
        // while loop to return from mp3->loop().
        if (*_pPause) {
            _flush();
            *_pState = State::Paused;
            while (*_pPause && !*_pStop)
                vTaskDelay(pdMS_TO_TICKS(20));
            if (*_pStop) return false;
            *_pState = State::Playing;
        }

        int16_t mono = (int16_t)(((int32_t)sample[0] + sample[1]) >> 1);
        _buf[_fill++] = mono;  // L
        _buf[_fill++] = mono;  // R
        if (_fill >= BUF) _flush();
        return true;
    }

    // Called by AudioGeneratorMP3::loop() when ConsumeSample returns false.
    // Returning false propagates: running=false → loop()=false → task breaks.
    bool loop() override { return false; }

    // Called by mp3->stop() — restore Sound's 8 kHz mono driver config.
    bool stop() override
    {
        _flush();
        vTaskDelay(pdMS_TO_TICKS(150));
        i2s_zero_dma_buffer(PLAYER_I2S_PORT);
        _installI2S(RESTORE_RATE, I2S_CHANNEL_FMT_ONLY_LEFT, 4, 512, false);
        return true;
    }
};

// ── Player task (Core 0) ──────────────────────────────────────────────────────

static void _playerTask(void* /*arg*/)
{
    // Yield so Core 1 finishes LVGL label updates and any TFT DMA flush before
    // we start SD SPI transactions on the shared SCK/MOSI/MISO bus.
    vTaskDelay(pdMS_TO_TICKS(80));

    // MP3 decode is CPU-intensive; IDLE0 on Core 0 is starved and would
    // trigger the Task Watchdog.  Remove IDLE0 from TWDT monitoring for the
    // duration of playback and restore it on exit.
    TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCPU(0);
    bool wdtSuspended  = (esp_task_wdt_delete(idle0) == ESP_OK);

    // Open file on Core 0 — AudioFileSourceSD calls SD.open() in its ctor.
    AudioFileSourceSD*  src = new AudioFileSourceSD(s_pathBuf);
    OpsI2SOutput*       out = new OpsI2SOutput(&s_stop, &s_pause, &s_state, src, &s_progress);
    AudioGeneratorMP3*  mp3 = new AudioGeneratorMP3();

    if (!src || !out || !mp3) {
        OPS_LOG("MP3", "Task: alloc failed");
        goto done_error;
    }

    if (!mp3->begin(src, out)) {
        OPS_LOG("MP3", "Task: begin() failed [%s]", s_pathBuf);
        goto done_error;
    }

    {
        OPS_LOG("MP3", "Task: playing [%s]", s_pathBuf);
        s_state = State::Playing;

        // mp3->loop() is greedy — it processes the entire file in one call,
        // looping inside AudioGeneratorMP3 until ConsumeSample returns false
        // or the file ends.  Progress is tracked inside OpsI2SOutput::_flush().
        while (!s_stop) {
            if (!mp3->isRunning() || !mp3->loop())
                break;
            vTaskDelay(1);
        }
    }

    mp3->stop();
    delete mp3; mp3 = nullptr;
    delete out; out = nullptr;
    delete src; src = nullptr;

    s_progress = s_stop ? s_progress : 1.0f;
    s_state    = s_stop ? State::Idle : State::Done;
    s_task     = nullptr;
    OPS_LOG("MP3", "Done (%s)", s_stop ? "stopped" : "complete");
    if (wdtSuspended) esp_task_wdt_add(idle0);
    vTaskDelete(nullptr);
    return;

done_error:
    if (mp3) { delete mp3; }
    if (out) { delete out; }
    if (src) { delete src; }
    s_state = State::Error;
    s_task  = nullptr;
    if (wdtSuspended) esp_task_wdt_add(idle0);
    vTaskDelete(nullptr);
}

// ── Public API ────────────────────────────────────────────────────────────────

bool play(const char* sdPath)
{
    OPS_LOG("MP3", "play() -> [%s]", sdPath);

    if (s_state == State::Playing || s_state == State::Paused || s_state == State::Stopping) {
        for (int i = 0; i < 30 && s_task != nullptr; i++)
            vTaskDelay(pdMS_TO_TICKS(10));
        if (s_state == State::Playing || s_state == State::Paused || s_state == State::Stopping) {
            OPS_LOG("MP3", "Still busy");
            return false;
        }
    }

    const char* base = strrchr(sdPath, '/');
    strncpy(s_filename, base ? base + 1 : sdPath, sizeof(s_filename) - 1);
    s_filename[sizeof(s_filename) - 1] = '\0';

    strncpy(s_pathBuf, sdPath, sizeof(s_pathBuf) - 1);
    s_pathBuf[sizeof(s_pathBuf) - 1] = '\0';

    s_stop     = false;
    s_pause    = false;
    s_progress = 0.0f;
    s_state    = State::Idle;

    BaseType_t ok = xTaskCreatePinnedToCore(
        _playerTask, "mp3play",
        12288,
        nullptr,
        2, &s_task,
        0
    );

    if (ok != pdPASS) {
        OPS_LOG("MP3", "Task create failed");
        s_state = State::Error;
        return false;
    }
    OPS_LOG("MP3", "Task spawned");
    return true;
}

void pause()  { if (s_state == State::Playing) s_pause = true; }
void resume() { if (s_state == State::Paused)  s_pause = false; }

void stop()
{
    // Transition to Stopping so the UI timer doesn't overwrite "Stopped" feedback
    // while the task is still winding down its final i2s_write / mp3->stop() call.
    if (s_state == State::Playing || s_state == State::Paused)
        s_state = State::Stopping;
    s_stop  = true;
    s_pause = false;
}

State       state()    { return s_state; }
const char* filename() { return s_filename; }
float       progress() { return s_progress; }

}  // namespace mp3player
}  // namespace ops
