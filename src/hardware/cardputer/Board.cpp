// Saitama — cardputer/Board.cpp
// Copyright 2026 Saitama — GPL-3.0-or-later
//
// Board.h for the M5Stack Cardputer ADV (Stamp-S3A, ESP32-S3FN8) with the
// Cap LoRa-1262. Built instead of ../Board.cpp by [env:cardputer-adv].
// Pins from M5Stack's M5Unified / M5GFX / M5Cardputer sources and the
// Cap LoRa-1262 example.
//
// Input: there is no trackball. Fn + ; , . / are the arrow keys and feed the
// trackball delta (the shared UI already navigates with it); Enter and the G0
// button are the press. Every other key comes out of pollKeyboard() as the
// same ASCII the T-Deck keyboard sends (Enter '\r', Backspace 8, Esc 27).
// The Esc/` key is Esc (back, like Backspace on a button); Fn + it types `.

#include "../Board.h"
#include "../../utils/Config.h"
#include "../../utils/Log.h"
#include <Wire.h>
#include <sys/time.h>

namespace ops {

// ── Pins ────────────────────────────────────────────────────────────
static constexpr gpio_num_t PIN_I2C_SDA   = GPIO_NUM_8;   // internal I2C: keyboard, IO expander
static constexpr gpio_num_t PIN_I2C_SCL   = GPIO_NUM_9;
static constexpr gpio_num_t PIN_KB_INT    = GPIO_NUM_11;  // TCA8418 INT (active low)
static constexpr gpio_num_t PIN_BTN_G0    = GPIO_NUM_0;   // G0 button (active low)
static constexpr gpio_num_t PIN_GPS_RX    = GPIO_NUM_15;  // Cap LoRa-1262 GPS TX → ESP RX
static constexpr gpio_num_t PIN_GPS_TX    = GPIO_NUM_13;
static constexpr int        PIN_BATT_ADC  = 10;           // VBAT / 2

static constexpr uint8_t TCA8418_ADDR = 0x34;
static constexpr uint8_t PI4IO_ADDR   = 0x43;             // Cap LoRa-1262 IO expander

// ── TCA8418 registers ───────────────────────────────────────────────
static constexpr uint8_t TCA_CFG        = 0x01;
static constexpr uint8_t TCA_INT_STAT   = 0x02;
static constexpr uint8_t TCA_KEY_LCK_EC = 0x03;
static constexpr uint8_t TCA_KEY_EVENT  = 0x04;
static constexpr uint8_t TCA_INT_EN_1   = 0x1A;
static constexpr uint8_t TCA_KP_GPIO_1  = 0x1D;
static constexpr uint8_t TCA_KP_GPIO_2  = 0x1E;
static constexpr uint8_t TCA_KP_GPIO_3  = 0x1F;
static constexpr uint8_t TCA_GPI_EM_1   = 0x20;
static constexpr uint8_t TCA_GPIO_DIR_1 = 0x23;
static constexpr uint8_t TCA_INT_LVL_1  = 0x26;

// ── PI4IOE5V6408 registers ──────────────────────────────────────────
static constexpr uint8_t PI4_IO_DIR  = 0x03;   // 1 = output
static constexpr uint8_t PI4_OUT     = 0x05;
static constexpr uint8_t PI4_HIGH_Z  = 0x07;   // 1 = high impedance

// ── Keymap (4 rows × 14 columns, M5Cardputer layout) ────────────────
// Codes below 0x100 are characters; the rest are special keys.
enum : uint16_t {
    K_NONE = 0x100, K_FN, K_SHIFT, K_CTRL, K_ALT, K_OPT,
    K_BKSP, K_TAB, K_ENTER, K_ESC, K_DEL,
    K_UP, K_DOWN, K_LEFT, K_RIGHT,
};

struct KeyDef { uint16_t normal, shifted, fn; };

static const KeyDef kKeys[4][14] = {
    {{K_ESC,'~','`'}, {'1','!',K_NONE}, {'2','@',K_NONE}, {'3','#',K_NONE}, {'4','$',K_NONE},
     {'5','%',K_NONE}, {'6','^',K_NONE}, {'7','&',K_NONE}, {'8','*',K_NONE}, {'9','(',K_NONE},
     {'0',')',K_NONE}, {'-','_',K_NONE}, {'=','+',K_NONE}, {K_BKSP,K_BKSP,K_DEL}},
    {{K_TAB,K_TAB,K_NONE}, {'q','Q',K_NONE}, {'w','W',K_NONE}, {'e','E',K_NONE}, {'r','R',K_NONE},
     {'t','T',K_NONE}, {'y','Y',K_NONE}, {'u','U',K_NONE}, {'i','I',K_NONE}, {'o','O',K_NONE},
     {'p','P',K_NONE}, {'[','{',K_NONE}, {']','}',K_NONE}, {'\\','|',K_NONE}},
    {{K_FN,K_FN,K_FN}, {K_SHIFT,K_SHIFT,K_NONE}, {'a','A',K_NONE}, {'s','S',K_NONE}, {'d','D',K_NONE},
     {'f','F',K_NONE}, {'g','G',K_NONE}, {'h','H',K_NONE}, {'j','J',K_NONE}, {'k','K',K_NONE},
     {'l','L',K_NONE}, {';',':',K_UP}, {'\'','"',K_NONE}, {K_ENTER,K_ENTER,K_NONE}},
    {{K_CTRL,K_CTRL,K_NONE}, {K_OPT,K_OPT,K_NONE}, {K_ALT,K_ALT,K_NONE}, {'z','Z',K_NONE},
     {'x','X',K_NONE}, {'c','C',K_NONE}, {'v','V',K_NONE}, {'b','B',K_NONE}, {'n','N',K_NONE},
     {'m','M',K_NONE}, {',','<',K_LEFT}, {'.','>',K_DOWN}, {'/','?',K_RIGHT}, {' ',' ',K_NONE}},
};

// ── I2C helpers ─────────────────────────────────────────────────────
static bool _wr(uint8_t addr, uint8_t reg, uint8_t val)
{
    Wire.beginTransmission(addr);
    Wire.write(reg);
    Wire.write(val);
    return Wire.endTransmission(true) == 0;
}

static int _rd(uint8_t addr, uint8_t reg)
{
    Wire.beginTransmission(addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return -1;
    if (Wire.requestFrom(addr, (uint8_t)1) < 1 || !Wire.available()) return -1;
    return Wire.read();
}

// ── Static instance ─────────────────────────────────────────────────
static Board s_board;
bool Board::trackballDebug = false;

Board& Board::instance() { return s_board; }

// ── init() ──────────────────────────────────────────────────────────
void Board::init()
{
    OPS_LOG("Board", "Initialising M5Stack Cardputer ADV hardware");

    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    pinMode(PIN_BTN_G0, INPUT_PULLUP);
    pinMode(PIN_KB_INT, INPUT_PULLUP);

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);
    delay(20);

    // Keyboard: 7 rows × 8 columns of the TCA8418 key matrix, key-event
    // interrupts on. Same register sequence as M5Cardputer's driver.
    _keyboardPresent = _wr(TCA8418_ADDR, TCA_KP_GPIO_1, 0x7F);
    if (_keyboardPresent) {
        for (uint8_t i = 0; i < 3; i++) {
            _wr(TCA8418_ADDR, TCA_GPIO_DIR_1 + i, 0x00);
            _wr(TCA8418_ADDR, TCA_GPI_EM_1   + i, 0xFF);
            _wr(TCA8418_ADDR, TCA_INT_LVL_1  + i, 0x00);
            _wr(TCA8418_ADDR, TCA_INT_EN_1   + i, 0xFF);
        }
        _wr(TCA8418_ADDR, TCA_KP_GPIO_2, 0xFF);
        _wr(TCA8418_ADDR, TCA_KP_GPIO_3, 0x00);
        while ((_rd(TCA8418_ADDR, TCA_KEY_LCK_EC) & 0x0F) > 0)   // flush
            _rd(TCA8418_ADDR, TCA_KEY_EVENT);
        _wr(TCA8418_ADDR, TCA_INT_STAT, 0x03);
        int cfg = _rd(TCA8418_ADDR, TCA_CFG);
        if (cfg >= 0) _wr(TCA8418_ADDR, TCA_CFG, (uint8_t)cfg | 0x03);  // GPI_IEN | KE_IEN
    }
    OPS_LOG("Board", "TCA8418 keyboard at 0x%02X: %s",
            TCA8418_ADDR, _keyboardPresent ? "FOUND" : "NOT FOUND");

    // Cap LoRa-1262: P0 of the IO expander enables the antenna RF switch.
    {
        int dir = _rd(PI4IO_ADDR, PI4_IO_DIR);
        if (dir >= 0) {
            int hz  = _rd(PI4IO_ADDR, PI4_HIGH_Z);
            int out = _rd(PI4IO_ADDR, PI4_OUT);
            _wr(PI4IO_ADDR, PI4_IO_DIR, (uint8_t)dir | 0x01);
            _wr(PI4IO_ADDR, PI4_HIGH_Z, (uint8_t)(hz < 0 ? 0xFF : hz) & ~0x01);
            _wr(PI4IO_ADDR, PI4_OUT,    (uint8_t)(out < 0 ? 0 : out) | 0x01);
            OPS_LOG("Board", "Cap LoRa-1262 RF switch enabled (PI4IOE at 0x%02X)", PI4IO_ADDR);
        } else {
            OPS_LOG("Board", "No IO expander at 0x%02X: Cap LoRa868 or no cap", PI4IO_ADDR);
        }
    }

#ifdef OPS_HAS_BUILTIN_GPS
    _gpsSerial.begin(115200, SERIAL_8N1, PIN_GPS_RX, PIN_GPS_TX);
    OPS_LOG("Board", "GPS serial started (RX=%d TX=%d)", PIN_GPS_RX, PIN_GPS_TX);
#endif

    analogReadResolution(12);

    // Seed the clock from the build time until GPS or the mesh sets it.
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
        for (int i = 0; i < 12; i++)
            if (strcmp(mon, months[i]) == 0) { tm.tm_mon = i; break; }
        struct timeval tv = { mktime(&tm), 0 };
        settimeofday(&tv, nullptr);
    }

    _initialized = true;
    OPS_LOG("Board", "Hardware ready (BL=GPIO%d)", TFT_BL);
}

// ── Keyboard ────────────────────────────────────────────────────────
// Reads every queued TCA8418 event. Modifiers update state, arrows go to the
// trackball delta, Enter also counts as the press, the rest is queued.
void Board::_drainKeyEvents()
{
    if (!_keyboardPresent) return;
    if (digitalRead(PIN_KB_INT) == HIGH) return;   // nothing pending

    for (int guard = 0; guard < 16; guard++) {
        int n = _rd(TCA8418_ADDR, TCA_KEY_LCK_EC);
        if (n <= 0 || (n & 0x0F) == 0) break;
        int ev = _rd(TCA8418_ADDR, TCA_KEY_EVENT);
        if (ev <= 0) break;

        bool    pressed = ev & 0x80;
        uint8_t b       = (uint8_t)((ev & 0x7F) - 1);
        uint8_t r = b / 10, c = b % 10;
        int x = r * 2 + (c > 3 ? 1 : 0);
        int y = c % 4;
        if (x < 0 || x > 13 || y < 0 || y > 3) continue;

        const KeyDef& kd = kKeys[y][x];
        if (kd.normal == K_FN)    { _fnHeld    = pressed; continue; }
        if (kd.normal == K_SHIFT) { _shiftHeld = pressed; continue; }
        if (kd.normal == K_CTRL)  { _ctrlHeld  = pressed; continue; }
        if (kd.normal == K_ALT || kd.normal == K_OPT) continue;
        if (!pressed) continue;

        uint16_t code = _fnHeld && kd.fn != K_NONE ? kd.fn
                      : _shiftHeld                 ? kd.shifted
                      :                              kd.normal;
        char out = 0;
        switch (code) {
            case K_UP:    _trackballY--; continue;
            case K_DOWN:  _trackballY++; continue;
            case K_LEFT:  _trackballX--; continue;
            case K_RIGHT: _trackballX++; continue;
            case K_ENTER: out = '\r'; break;
            case K_BKSP:  out = 8;    break;
            case K_DEL:   out = 127;  break;
            case K_ESC:   out = 27;   break;
            case K_TAB:   out = '\t'; break;
            case K_NONE:  continue;
            default:      out = (char)code; break;
        }
        uint8_t next = (uint8_t)((_keyHead + 1) % sizeof(_keyQueue));
        if (next != _keyTail) { _keyQueue[_keyHead] = out; _keyHead = next; }
    }
    _wr(TCA8418_ADDR, TCA_INT_STAT, 0x03);
}

bool Board::pollKeyboard(char& outKey)
{
    _drainKeyEvents();
    if (_keyTail == _keyHead) return false;
    outKey = _keyQueue[_keyTail];
    _keyTail = (uint8_t)((_keyTail + 1) % sizeof(_keyQueue));
    return true;
}

// ── tick() ──────────────────────────────────────────────────────────
void Board::tick()
{
    if (!_initialized) return;

    // Arrow keys only reach the trackball delta when the keyboard is read,
    // so read it here as well as from pollKeyboard().
    _drainKeyEvents();

    // G0 button = press, falling-edge debounced like the T-Deck trackball click.
    uint32_t now_ms = millis();
    bool curDown = !digitalRead(PIN_BTN_G0);
    if (curDown && !_trackballPrevDown && (now_ms - _trackballPressMs >= 150UL)) {
        _trackballPressed = true;
        _trackballPressMs = now_ms;
    }
    _trackballPrevDown = curDown;

#ifdef OPS_HAS_BUILTIN_GPS
    while (_gpsSerial.available()) _gps.encode(_gpsSerial.read());
    if (_gps.time.isUpdated() && _gps.date.isValid() && _gps.time.isValid()) {
        if (_gpsLastSync == 0 || now_ms - _gpsLastSync >= 600000UL) {
            struct tm t = {};
            t.tm_year = _gps.date.year() - 1900;
            t.tm_mon  = _gps.date.month() - 1;
            t.tm_mday = _gps.date.day();
            t.tm_hour = _gps.time.hour();
            t.tm_min  = _gps.time.minute();
            t.tm_sec  = _gps.time.second();
            if (t.tm_year >= 124) {
                struct timeval tv = { mktime(&t), 0 };
                settimeofday(&tv, nullptr);
                _gpsLastSync = now_ms;
                OPS_LOG("GPS", "RTC synced from GPS");
            }
        }
    }
#endif
}

// ── Trackball (arrow keys + G0/Enter) ───────────────────────────────
bool Board::consumeTrackballPress()
{
    if (_trackballPressed) { _trackballPressed = false; return true; }
    return false;
}

void Board::consumeTrackballDelta(int16_t& dx, int16_t& dy)
{
    dx = _trackballX;  dy = _trackballY;
    _trackballX = 0;   _trackballY = 0;
}

// ── Battery (GPIO10 = VBAT / 2) ─────────────────────────────────────
static int _battMv()
{
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) sum += analogReadMilliVolts(PIN_BATT_ADC);
    return (int)(sum / 8) * 2;
}

bool Board::batteryCharging() const
{
    if (!_initialized) return false;
    // Same hysteresis idea as the T-Deck: USB holds VBAT near 4.2 V.
    static bool s_charging = false;
    int mv = _battMv();
    if (!s_charging && mv > 4150) s_charging = true;
    if ( s_charging && mv < 4060) s_charging = false;
    return s_charging;
}

int Board::batteryPercent() const
{
    if (!_initialized) return 0;
    int pct = (int)map((long)_battMv(), 3500, 4200, 0, 100);
    return constrain(pct, 0, 100);
}

// ── GPS ─────────────────────────────────────────────────────────────
bool Board::hasGPSFix() const
{
#ifdef OPS_HAS_BUILTIN_GPS
    return _gps.location.isValid();
#else
    return false;
#endif
}

float Board::gpsLat() const
{
#ifdef OPS_HAS_BUILTIN_GPS
    return (float)_gps.location.lat();
#else
    return 0.0f;
#endif
}

float Board::gpsLng() const
{
#ifdef OPS_HAS_BUILTIN_GPS
    return (float)_gps.location.lng();
#else
    return 0.0f;
#endif
}

float Board::gpsAltM() const
{
#ifdef OPS_HAS_BUILTIN_GPS
    return _gps.altitude.isValid() ? (float)_gps.altitude.meters() : 0.0f;
#else
    return 0.0f;
#endif
}

float Board::gpsHdop() const
{
#ifdef OPS_HAS_BUILTIN_GPS
    return _gps.hdop.isValid() ? (float)_gps.hdop.hdop() : 99.9f;
#else
    return 99.9f;
#endif
}

uint8_t Board::gpsSatellites() const
{
#ifdef OPS_HAS_BUILTIN_GPS
    return _gps.satellites.isValid() ? (uint8_t)_gps.satellites.value() : 0;
#else
    return 0;
#endif
}

uint32_t Board::gpsNmeaCount() const
{
#ifdef OPS_HAS_BUILTIN_GPS
    return _gps.passedChecksum();
#else
    return 0;
#endif
}

bool Board::gpsDateTime(uint16_t& year, uint8_t& month, uint8_t& day,
                        uint8_t& hour, uint8_t& minute, uint8_t& sec) const
{
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
    (void)year; (void)month; (void)day; (void)hour; (void)minute; (void)sec;
    return false;
#endif
}

}  // namespace ops
