# Security Notes — Saitama / SX1262

Findings from a cross-reference of the SX1262 datasheet against the MeshCore library and Saitama project code.  Last reviewed: 2026-06-02.

---

## Summary

| # | Finding | Severity | Location | Actionable? |
|---|---|---|---|---|
| 1 | LoRa private sync word provides no PHY-layer confidentiality | **Medium** (architectural) | MeshCore config | No — LoRa architecture constraint |
| 2 | `GetRssiInst` used in LoRa RX mode (datasheet §9.8 says FSK/FS/STDBY only) | Low | MeshCore wrapper + our `spectrumScan` | Works in practice; documented |
| 3 | `setFrequency()` written to PLL register while radio in RX mode | Low | `spectrumScan()` | Works in practice; documented |
| 4 | `spectrumScan()` could corrupt live TX if called mid-transmission | Medium | `MeshService::spectrumScan()` | **Fixed** — `isInRecvMode()` guard added |
| 5 | Wide-zoom spectrum sweep crosses calibration band boundaries without recalibration | Low | Spectrum scanner only | Acceptable for debug tool |

---

## Finding 1 — PHY sync word is not encryption

**What it is:**  MeshCore configures the SX1262 with `RADIOLIB_SX126X_SYNC_WORD_PRIVATE` (`0x12`).  This byte is prepended to every LoRa packet and prevents accidental decoding by LoRaWAN devices (which use `0x34`), but it is **not a secret** and provides no confidentiality.  Any attacker with an SX1262-based device configured with sync word `0x12` can receive and replay all packets in cleartext at the PHY layer.

**Real security layer:**  MeshCore uses ED25519 key pairs for node identity.  All routed mesh packets are authenticated at the application layer.  PHY-layer encryption is not part of the LoRa standard.

**Mitigations in place:**  Application-layer authentication via ED25519.  Message content security depends entirely on MeshCore's implementation — not on the radio hardware.

**Action:**  No code change possible or necessary.  Operators should be aware that radio traffic is not encrypted on the air and can be received by any compatible receiver in range.

---

## Finding 2 — `GetRssiInst` in LoRa RX mode

**Datasheet requirement (§9.8):**  The `GetRssiInst` SPI command is documented as valid only in FSK modem RX mode, FS mode, or STDBY\_XOSC mode.  It is explicitly noted as unsupported in LoRa modem mode.

**What the code does:**
- `CustomSX1262Wrapper::getCurrentRSSI()` calls `getRSSI(false)` → `GetRssiInst` while in LoRa continuous RX.  Used by MeshCore's Dispatcher for software LBT (channel-busy detection).  **In MeshCore — cannot change.**
- `MeshService::spectrumScan()` calls `getRSSI(false)` per frequency step, also while in LoRa RX.  **In our code — documented.**

**In practice:**  Both usages return plausible RSSI values on the T-Deck Plus SX1262 silicon.  ScreenSignal's noise floor display and the spectrum scan produce consistent readings.  The datasheet restriction may be conservative or relate to older silicon revisions.

**Risk:**  Returned RSSI values may be slightly inaccurate or reflect stale ADC readings rather than the true instantaneous level.  Not a security issue — a signal-quality measurement accuracy concern.

---

## Finding 3 — `setFrequency()` while in RX mode

**Datasheet requirement (§13.4.1):**  `SetRfFrequency` should be issued from STDBY mode.

**What the code does:**  `spectrumScan()` steps through 296 frequencies in 25–500 kHz increments.  When the step is less than 20 MHz, RadioLib's auto-calibration does not trigger and `setFrequencyRaw()` writes the PLL register while the chip is in RX mode.

**Note on RadioLib auto-calibration:**  `SX1262::setFrequency()` automatically calls `calibrateImage()` for jumps ≥ 20 MHz (`RADIOLIB_SX126X_CAL_IMG_FREQ_TRIG_MHZ = 20.0`).  Profile switches between regions (e.g., EU 869 MHz → US 915 MHz = 46 MHz jump) are therefore handled correctly.

**In practice:**  Confirmed working on this hardware.  PLL re-lock delay (75 µs, vs 44 µs typical in datasheet) provides adequate settling time.

---

## Finding 4 — TX-busy race in `spectrumScan()` ✅ Fixed

**What it was:**  MeshCore's Dispatcher initiates TX asynchronously via `startTransmit()` and polls for completion on the next loop iteration.  If `spectrumScan()` ran between `startTransmit()` and TX completion, `setFrequency()` would change the PLL mid-packet, corrupting the transmission and leaving the radio state undefined.

**Fix applied (`MeshService.cpp`):**
```cpp
if (!radio_driver.isInRecvMode()) return false;
```
Added as the first check in `spectrumScan()` and `cadCheck()`.  If the radio is not in RX mode (e.g., actively transmitting), the scan is skipped for that tick and retried on the next.

---

## Finding 5 — Wide-zoom scan image rejection

**What it is:**  In wide-zoom mode (500 kHz step size, ~148 MHz span), the spectrum sweep crosses defined image-calibration band boundaries (e.g., 863–870 MHz → 902–928 MHz).  Because each step is only 500 kHz (well below the 20 MHz auto-calibration trigger), RadioLib does not recalibrate mid-sweep.

**Effect:**  After leaving the calibrated band, image rejection may degrade.  Strong out-of-band signals could appear as ghost signals ±2×BW away.  RSSI accuracy outside the calibrated band is reduced.

**Scope:**  Affects the `/spectrum` debug tool only.  Normal mesh operation always stays on the configured channel; profile switches trigger the ≥ 20 MHz auto-calibration.  No impact on mesh reliability.

---

## What RadioLib handles automatically

These items were investigated and confirmed to be correctly handled by RadioLib 7.6.0 — no project-level action required:

| Item | How it is handled |
|---|---|
| Image calibration on band changes | `SX1262::setFrequency()` auto-calibrates on jumps ≥ 20 MHz |
| PA clamping (datasheet §15.2) | `fixPaClamping()` called in `SX1262::begin()` |
| 500 kHz BW sensitivity fix (§15.1) | Not applicable — project uses BW = 62.5 kHz |
| TCXO startup time (DIO3 voltage) | Configured via `-DSX126X_DIO3_TCXO_VOLTAGE=1.8f`; RadioLib handles delay |
| BUSY pin before SPI transactions | Handled by RadioLib's `Module` SPI wrapper |
| Sleep / wakeup sequence | Handled by RadioLib state machine |
| OCP (over-current protection) | Set to 140 mA via `-DSX126X_CURRENT_LIMIT=140` |
| DIO2 as RF switch | Disabled (`-DSX126X_DIO2_AS_RF_SWITCH=false`); T-Deck Plus has no external RF switch |
