// Saitama — Board.cpp
// Copyright 2026 Saitama — MIT License
//
// Hardware abstraction for the LilyGo T-Deck Plus (ESP32-S3).
// All pin numbers verified against the official LilyGo schematic.

#include "Board.h"
#include "../utils/Config.h"
#include "../utils/Log.h"
#include <Wire.h>
#include <sys/time.h>

// ── T-Deck keyboard MCU (ESP32-C3, I2C 0x55) ─────────────────────────
// Protocol: bare Wire.requestFrom(0x55, 1).  Returns 0x00 = no key,
// non-zero = ASCII keycode.  No register write required.
static constexpr uint8_t KB_ADDR = 0x55;

namespace ops {

// ── Pin definitions (T-Deck Plus) ─────────────────────────────────────
//
// CRITICAL: BOARD_POWERON (GPIO10) must be driven HIGH before any
// peripheral (display, radio, GPS, SD) will receive power.  Forgetting
// this pin is the #1 cause of "nothing works" on the T-Deck Plus.

static constexpr gpio_num_t PIN_POWERON        = GPIO_NUM_10;  // peripheral rail enable

// ── SPI bus (shared by TFT + LoRa + SD card) ──────────────────────
static constexpr gpio_num_t PIN_SPI_SCK        = GPIO_NUM_40;  // BOARD_SPI_SCK
static constexpr gpio_num_t PIN_SPI_MOSI       = GPIO_NUM_41;  // BOARD_SPI_MOSI
static constexpr gpio_num_t PIN_SPI_MISO       = GPIO_NUM_38;  // BOARD_SPI_MISO

// ── SX1262 LoRa radio ─────────────────────────────────────────────
static constexpr gpio_num_t PIN_LORA_CS        = GPIO_NUM_9;   // RADIO_CS_PIN
static constexpr gpio_num_t PIN_LORA_RST       = GPIO_NUM_17;  // RADIO_RST_PIN
static constexpr gpio_num_t PIN_LORA_DIO1      = GPIO_NUM_45;  // RADIO_DIO1_PIN
static constexpr gpio_num_t PIN_LORA_BUSY      = GPIO_NUM_13;  // RADIO_BUSY_PIN

// ── ST7789 TFT display ────────────────────────────────────────────
static constexpr gpio_num_t PIN_TFT_CS         = GPIO_NUM_12;  // BOARD_TFT_CS
static constexpr gpio_num_t PIN_TFT_DC         = GPIO_NUM_11;  // BOARD_TFT_DC
// No dedicated TFT reset pin on T-Deck Plus — handled by power cycle
static constexpr gpio_num_t PIN_BACKLIGHT      = GPIO_NUM_42;  // BOARD_BL_PIN / BOARD_TFT_BACKLIGHT

// ── Trackball (4-directional optical encoder + press) ─────────────
// Directions: assign UP/DOWN/LEFT/RIGHT from the four TBOX GPIOs.
// If movement feels inverted, swap UP<->DOWN or LEFT<->RIGHT here.
static constexpr gpio_num_t PIN_TBALL_UP       = GPIO_NUM_3;   // BOARD_TBOX_G01
static constexpr gpio_num_t PIN_TBALL_DOWN     = GPIO_NUM_15;  // BOARD_TBOX_G03
static constexpr gpio_num_t PIN_TBALL_LEFT     = GPIO_NUM_1;   // BOARD_TBOX_G04
static constexpr gpio_num_t PIN_TBALL_RIGHT    = GPIO_NUM_2;   // BOARD_TBOX_G02
// Press = BOOT button = GPIO0 (active-low, same pin used for flashing)
static constexpr gpio_num_t PIN_TBALL_PRESS    = GPIO_NUM_0;   // BOARD_BOOT_PIN

// --- Shared SPI Bus (display + LoRa + SD) ---
static constexpr gpio_num_t SPI_SCK            = GPIO_NUM_40;
static constexpr gpio_num_t SPI_MISO           = GPIO_NUM_38;
static constexpr gpio_num_t SPI_MOSI           = GPIO_NUM_41;
static constexpr gpio_num_t PIN_SD_CS          = GPIO_NUM_39;  // BOARD_SDCARD_CS0
// --- I2C Bus (shared: keyboard + touchscreen) ---
static constexpr gpio_num_t I2C_SDA            = GPIO_NUM_18;
static constexpr gpio_num_t I2C_SCL            = GPIO_NUM_8;

// --- Touchscreen (GT911 capacitive) ---
static constexpr gpio_num_t  PIN_TOUCH_INT         = GPIO_NUM_16;
// GT911 I2C address: typically 0x5D or 0x14 (depends on INT state at boot)
static constexpr uint8_t TOUCH_I2C_ADDR_1      = 0x5D;
static constexpr uint8_t TOUCH_I2C_ADDR_2      = 0x14;

// --- Keyboard (ESP32-C3 over I2C) ---
static constexpr uint8_t KB_I2C_ADDR           = 0x55;
static constexpr gpio_num_t KB_INT             = GPIO_NUM_46;   // Interrupt pin

// ── GPS (T-Deck Plus built-in) ────────────────────────────────────
static constexpr gpio_num_t PIN_GPS_TX         = GPIO_NUM_43;  // BOARD_GPS_TX_PIN
static constexpr gpio_num_t PIN_GPS_RX         = GPIO_NUM_44;  // BOARD_GPS_RX_PIN

// ── Battery ADC (GPIO4 via 100k/100k voltage divider) ────────────
static constexpr int        PIN_BATT_ADC       = 4;            // BOARD_BAT_ADC

// ── Trackball ISR counters (IRAM — must survive cache eviction) ────────
static volatile uint32_t s_isrU = 0, s_isrD = 0, s_isrL = 0, s_isrR = 0;

static void IRAM_ATTR isr_tball_up()    { s_isrU++; }
static void IRAM_ATTR isr_tball_down()  { s_isrD++; }
static void IRAM_ATTR isr_tball_left()  { s_isrL++; }
static void IRAM_ATTR isr_tball_right() { s_isrR++; }

// ── Static instance ────────────────────────────────────────────────
static Board s_board;
bool Board::trackballDebug = false;

Board& Board::instance() {
    return s_board;
}

// ── init() ────────────────────────────────────────────────────────
void Board::init() {
    OPS_LOG("Board", "Initialising T-Deck Plus hardware");

    // 1) Power on the peripheral rail FIRST — nothing works without this
    pinMode(PIN_POWERON, OUTPUT);
    digitalWrite(PIN_POWERON, HIGH);
    delay(100);  // allow supply rails to stabilise

    // 2) Display backlight (TFT_eSPI handles the display init itself)
    pinMode(PIN_BACKLIGHT, OUTPUT);
    digitalWrite(PIN_BACKLIGHT, HIGH);

    // 3) Keyboard I2C bus — probe for BBQ10 at 0x1F to avoid Wire error spam
    Wire.begin(I2C_SDA , I2C_SCL);
    delay(50);  // allow I2C devices to finish startup
    Wire.beginTransmission(KB_ADDR);
    _keyboardPresent = (Wire.endTransmission(true) == 0);
    OPS_LOG("Board", "BBQ10 keyboard at 0x%02X: %s",
            KB_ADDR, _keyboardPresent ? "FOUND" : "NOT FOUND");

    // 4) Trackball GPIOs (4-direction optical encoder + press)
    //    The encoder generates active-low pulses; FALLING-edge ISRs count them.
    //    Press = GPIO0 (BOOT button), active-low; polled in tick().
    pinMode(PIN_TBALL_UP,    INPUT_PULLUP);
    pinMode(PIN_TBALL_DOWN,  INPUT_PULLUP);
    pinMode(PIN_TBALL_LEFT,  INPUT_PULLUP);
    pinMode(PIN_TBALL_RIGHT, INPUT_PULLUP);
    pinMode(PIN_TBALL_PRESS, INPUT_PULLUP);

    attachInterrupt(digitalPinToInterrupt(PIN_TBALL_UP),    isr_tball_up,    FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_TBALL_DOWN),  isr_tball_down,  FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_TBALL_LEFT),  isr_tball_left,  FALLING);
    attachInterrupt(digitalPinToInterrupt(PIN_TBALL_RIGHT), isr_tball_right, FALLING);
    OPS_LOG("Board", "Trackball ISRs attached (UP=%d DN=%d LT=%d RT=%d)",
            PIN_TBALL_UP, PIN_TBALL_DOWN, PIN_TBALL_LEFT, PIN_TBALL_RIGHT);

    // 5) Touch controller interrupt (active-low, INT → ESP32)
    pinMode(PIN_TOUCH_INT, INPUT_PULLUP);

    // 6) GPS serial (T-Deck Plus has built-in GPS module)
#ifdef OPS_HAS_BUILTIN_GPS
    _gpsSerial.begin(38400, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    OPS_LOG("Board", "GPS serial started (RX=%d TX=%d)", PIN_GPS_RX, PIN_GPS_TX);
#endif

    // 7) Seed ESP32 RTC from compile timestamp so the clock shows something
    //    useful before GPS/NTP sets it properly.
    {
        static const char* months[] = {
            "Jan","Feb","Mar","Apr","May","Jun",
            "Jul","Aug","Sep","Oct","Nov","Dec"
        };
        struct tm tm = {};
        char mon[4] = {};
        sscanf(__DATE__, "%3s %d %d", mon, &tm.tm_mday, &tm.tm_year);
        sscanf(__TIME__, "%d:%d:%d",  &tm.tm_hour, &tm.tm_min, &tm.tm_sec);
        tm.tm_year -= 1900;
        for (int i = 0; i < 12; i++) {
            if (strcmp(mon, months[i]) == 0) { tm.tm_mon = i; break; }
        }
        time_t ct = mktime(&tm);
        struct timeval tv = { ct, 0 };
        settimeofday(&tv, nullptr);
        OPS_LOG("Board", "RTC seeded from compile time: %s %s", __DATE__, __TIME__);
    }

    _initialized = true;
    OPS_LOG("Board", "Hardware ready (POWERON=GPIO%d, BL=GPIO%d)",
            PIN_POWERON, PIN_BACKLIGHT);
}

// ── tick() ────────────────────────────────────────────────────────
void Board::tick() {
    if (!_initialized) return;

    // Atomically drain ISR counters
    uint32_t u, d, l, r;
    noInterrupts();
    u = s_isrU; s_isrU = 0;
    d = s_isrD; s_isrD = 0;
    l = s_isrL; s_isrL = 0;
    r = s_isrR; s_isrR = 0;
    interrupts();

    if (u || d || l || r) {
        _trackballX += (int16_t)(r - l);
        _trackballY += (int16_t)(d - u);

        if (trackballDebug) {
            // Determine dominant direction for the log label
            uint32_t maxVal = u;
            const char* dir = "UP";
            if (d > maxVal) { maxVal = d; dir = "DOWN";  }
            if (l > maxVal) { maxVal = l; dir = "LEFT";  }
            if (r > maxVal) {             dir = "RIGHT"; }

            // Read current pin states for extra visibility
            uint8_t pinStates =
                (!digitalRead(PIN_TBALL_UP)    << 3) |
                (!digitalRead(PIN_TBALL_DOWN)  << 2) |
                (!digitalRead(PIN_TBALL_LEFT)  << 1) |
                (!digitalRead(PIN_TBALL_RIGHT) << 0);

            OPS_LOG("TRACKBALL-GPIO",
                    "dir=%s (pin_down_state=0x%X, isr_counts: U=%lu D=%lu L=%lu R=%lu)",
                    dir, pinStates, (unsigned long)u, (unsigned long)d,
                    (unsigned long)l, (unsigned long)r);
        }
    }

    // Poll trackball press with falling-edge debounce (GPIO0 active-low).
    // Without edge detection the flag is set every tick while held, causing
    // rapid-fire clicks that toggle settings items back and forth.
    {
        uint32_t now_ms = millis();
        bool curDown = !digitalRead(PIN_TBALL_PRESS);
        if (curDown && !_trackballPrevDown && (now_ms - _trackballPressMs >= 150UL)) {
            _trackballPressed = true;
            _trackballPressMs = now_ms;
        }
        _trackballPrevDown = curDown;
    }

    // GPS feed + RTC sync
#ifdef OPS_HAS_BUILTIN_GPS
    while (_gpsSerial.available()) {
        _gps.encode(_gpsSerial.read());
    }
    // Sync system RTC from GPS: immediately on first valid fix, then at most once per 10 minutes.
    // GPS provides UTC; ESP32 defaults to UTC timezone so mktime() is correct here.
    if (_gps.time.isUpdated() && _gps.date.isValid() && _gps.time.isValid()) {
        uint32_t now_ms = millis();
        if (_gpsLastSync == 0 || now_ms - _gpsLastSync >= 600000UL) {
            struct tm t = {};
            t.tm_year = _gps.date.year() - 1900;
            t.tm_mon  = _gps.date.month() - 1;
            t.tm_mday = _gps.date.day();
            t.tm_hour = _gps.time.hour();
            t.tm_min  = _gps.time.minute();
            t.tm_sec  = _gps.time.second();
            struct timeval tv = { mktime(&t), 0 };
            settimeofday(&tv, nullptr);
            _gpsLastSync = now_ms;
            OPS_LOG("GPS", "RTC synced: %04d-%02d-%02d %02d:%02d:%02d UTC",
                    _gps.date.year(), _gps.date.month(), _gps.date.day(),
                    _gps.time.hour(), _gps.time.minute(), _gps.time.second());
        }
    }
#endif
}

// ── Trackball ─────────────────────────────────────────────────────
bool Board::consumeTrackballPress() {
    if (_trackballPressed) {
        _trackballPressed = false;
        return true;
    }
    return false;
}

void Board::consumeTrackballDelta(int16_t &dx, int16_t &dy) {
    dx = _trackballX;  dy = _trackballY;
    _trackballX = 0;   _trackballY = 0;
}

// ── Battery ───────────────────────────────────────────────────────
// GPIO4 reads VBAT through a 100k/100k divider → ADC sees VBAT/2.
//   4.20 V full  → 2.10 V → raw ≈ 2607
//   3.50 V empty → 1.75 V → raw ≈ 2172  (Li-Ion safe cutoff ~3.5V)
// Average 16 samples to reduce ADC noise.
static int _battRaw() {
    int32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += analogRead(4);
    return (int)(sum / 16);
}

bool Board::batteryCharging() const {
    if (!_initialized) return false;
    // Hysteresis band prevents ADC noise from flickering the charging icon.
    // The uncalibrated ESP32 ADC has ~60-count noise at this voltage range,
    // so a single threshold is unreliable near a full battery (~4.2 V, raw 2607).
    // Latch ON  above 4.15 V (raw 2575) — sustained USB input.
    // Latch OFF below 4.06 V (raw 2519) — battery discharging on its own.
    static bool s_charging = false;
    const int raw = _battRaw();
    if (!s_charging && raw > 2575) s_charging = true;
    if ( s_charging && raw < 2519) s_charging = false;
    return s_charging;
}

int Board::batteryPercent() const {
    if (!_initialized) return 0;
    const int raw = _battRaw();
    // Map 3.5 V (raw 2172) → 0 %  to  4.2 V (raw 2607) → 100 %
    const int pct = (int)map((long)raw, 2172, 2607, 0, 100);
    return constrain(pct, 0, 100);
}

// ── GPS ───────────────────────────────────────────────────────────
bool Board::hasGPSFix() const {
#ifdef OPS_HAS_BUILTIN_GPS
    return _gps.location.isValid();
#else
    return false;
#endif
}

float Board::gpsLat() const {
#ifdef OPS_HAS_BUILTIN_GPS
    return static_cast<float>(_gps.location.lat());
#else
    return 0.0f;
#endif
}

float Board::gpsLng() const {
#ifdef OPS_HAS_BUILTIN_GPS
    return static_cast<float>(_gps.location.lng());
#else
    return 0.0f;
#endif
}

float Board::gpsAltM() const {
#ifdef OPS_HAS_BUILTIN_GPS
    return _gps.altitude.isValid() ? static_cast<float>(_gps.altitude.meters()) : 0.0f;
#else
    return 0.0f;
#endif
}

float Board::gpsHdop() const {
#ifdef OPS_HAS_BUILTIN_GPS
    return _gps.hdop.isValid() ? static_cast<float>(_gps.hdop.hdop()) : 99.9f;
#else
    return 99.9f;
#endif
}

uint8_t Board::gpsSatellites() const {
#ifdef OPS_HAS_BUILTIN_GPS
    return _gps.satellites.isValid() ? (uint8_t)_gps.satellites.value() : 0;
#else
    return 0;
#endif
}

uint32_t Board::gpsNmeaCount() const {
#ifdef OPS_HAS_BUILTIN_GPS
    return _gps.passedChecksum();
#else
    return 0;
#endif
}

bool Board::gpsDateTime(uint16_t& year, uint8_t& month, uint8_t& day,
                         uint8_t& hour, uint8_t& minute, uint8_t& sec) const {
#ifdef OPS_HAS_BUILTIN_GPS
    if (!_gps.date.isValid() || !_gps.time.isValid()) return false;
    year   = _gps.date.year();
    month  = _gps.date.month();
    day    = _gps.date.day();
    hour   = _gps.time.hour();
    minute = _gps.time.minute();
    sec    = _gps.time.second();
    return true;
#else
    (void)year; (void)month; (void)day;
    (void)hour; (void)minute; (void)sec;
    return false;
#endif
}

// ── T-Deck keyboard ───────────────────────────────────────────────
// Returns true and fills outKey when a key is available.
// The T-Deck keyboard MCU (0x55) uses a simple protocol:
// single-byte requestFrom — 0x00 = no key, non-zero = ASCII keycode.
bool Board::pollKeyboard(char& outKey) {
    if (!_keyboardPresent) return false;

    if (Wire.requestFrom((uint8_t)KB_ADDR, (uint8_t)1) < 1) return false;
    if (!Wire.available()) return false;
    uint8_t raw = Wire.read();
    while (Wire.available()) Wire.read();  // drain extras

    if (raw == 0) return false;

    OPS_LOG("KB-RAW", "0x%02X ('%c')", raw, (raw >= 32 && raw < 127) ? (char)raw : '?');

    outKey = (char)raw;
    return true;
}

}  // namespace ops
