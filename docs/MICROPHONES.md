# Microphones for future ESP32 tuners / metronomes

Note for phase 2 (own ESP32 designs porting this code). Date: 2026-10-19.
Figures come from the datasheets and product pages linked below, not measured by me.
Only the section "Measured with the Cardputer ADV" comes from real hardware.

## Measured with the Cardputer ADV

- Mic: per Espressif's Cardputer component readme, the ADV uses an **analog** MEMS mic wired to the ES8311 codec ADC (the original Cardputer uses a PDM SPM1423). **Exact mic model not found.**
- Viola at ~30 cm (rms, int16 units): silence 5–12, voice median 26 (max 86), viola median 190 (max 1043); high notes and pianos 90–200.
- Low strings give a weak fundamental: with standard pitchy (K = 0.9) F3 came out as F4 and pizzicati E3/D3/C3 one octave up. Fixed with a stricter K (0.97) below 400 Hz (see `TONEMAP_DESIGN.md` §12). **That the cause is the bass response of mic/case is an inference from this data, not a direct measurement.**
- At 16 kHz sampling octave 7 fails (octave errors); 32 kHz works (exact I2S clock).

## Tuner requirements

| Requirement | Recommended | Reason |
|---|---|---|
| Bass response | Flat down to ~60 Hz or lower | Viola reaches 130 Hz, cello 65 Hz; an attenuated fundamental causes octave errors |
| Acoustic overload point (AOP) | ≥ 120 dB SPL | Instrument is close; a saturated mic distorts the wave |
| SNR | ≥ 64 dBA | Quiet high notes arrive with little signal |
| Sampling | 32–48 kHz | Measured: 16 kHz fails octave 7 |
| Interface | I²S or digital PDM | ESP32 internal ADC is noisy and imprecise; ESP32-S3 supports both |
| High-pass filter | No aggressive bass cut | Check the codec/mic one |

## Candidates

| Mic | Interface | SNR | Bass | AOP | Notes |
|---|---|---|---|---|---|
| Infineon **IM69D130** | PDM | 69 dB(A) | roll-off 28 Hz | 130 dB SPL (distortion < 1 % at 128 dB SPL) | Best on paper; sensitivity −36 ±1 dB; needs breakout board or eval kit; would use the PDM input of the ESP32-S3 I2S peripheral |
| **INMP441** | I²S | 61 dBA | 60 Hz | – | Cheap, well-documented ESP32 modules |
| Knowles **SPH0645** | I²S | 65 dB(A) | 100 Hz | – | Figures from a reseller, not the manufacturer; worse bass |
| TDK **ICS-43434** | I²S | 64–65 dBA | 60 Hz | 120 dB SPL | Sensitivity −26 dBFS ±1 dB. **Avoid in new designs:** "Last Time Buy" (2026-06-15), out of stock at DigiKey; suggested replacement (MMICT5837-00-012) not drop-in |

Recommendation: **IM69D130** to start; **INMP441** as easy alternative; skip ICS-43434 (availability).

## Tips that matter more than the mic model

- **Case and acoustic port:** a good mic behind a small hole or inside a housing loses bass; keep it free of vibrations.
- **Contact sensor:** a clip-on piezo (bridge or scroll) ignores room noise and keeps the fundamental; needs preamp and external ADC/codec (not the ESP32 ADC). Different design.
- **Before buying:** record the calibration sequence with the candidate mic (open strings, low scale, quiet high notes) using the `w` key of the bench environment and analyze offline.

## Sources

- [Espressif m5stack-cardputer component readme](https://components.espressif.com/components/espp/m5stack-cardputer/versions/1.3.8/readme)
- [TDK ICS-43434 product page](https://invensense.tdk.com/products/ics-43434)
- [ICS-43434 datasheet v1.2 (SparkFun mirror)](https://cdn.sparkfun.com/assets/a/9/c/9/1/DS-000069-ICS-43434-v1.2.pdf)
- [DigiKey ICS-43434 (Last Time Buy)](https://www.digikey.be/en/products/detail/tdk-invensense/ICS-43434/6140298)
- [INMP441 module (Components101)](https://components101.com/modules/inmp441-mems-omnidirectional-microphone)
- [SPH0645 module listing](https://ifuturetech.org/?p=62955)
- [Infineon IM69D130 product page](https://www.infineon.com/cms/en/product/sensor/mems-microphones/im69d130/)
- [Infineon IM69D130 datasheet](https://www.infineon.cn/content/dam/infineon/row/public/documents/24/49/infineon-im69d130-datasheet-en.pdf)
