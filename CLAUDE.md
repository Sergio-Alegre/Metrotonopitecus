# Metrotonopitecus

Metronome + tuner for the M5Stack Cardputer ADV. Sergio's first Cardputer project.
Working language with Sergio: Spanish. Code and comments: English.

## Hardware and environment

- Device: M5Stack **Cardputer ADV** (ESP32-S3). Not interchangeable with the original Cardputer: if keyboard/screen/audio fails, check the ADV differences first.
- The device runs **M5Launcher** (v2.8+ for ADV) as boot manager.
- Development on Windows with PlatformIO (CLI) + Git. GitHub repo is public.

## Critical deployment rule

**By default Claude does not flash over USB** (esptool / `pio run -t upload`): it would overwrite M5Launcher. Build the **application** binary (`.pio/build/<env>/firmware.bin`, NOT the "merged" one); Sergio installs it through Launcher: WebUI (Launcher's WUI option, upload from browser), SD card or OTA.

**USB flashing only with Sergio's permission.**
- A one-off order ("flash it", "upload it", "give me new firmware" with the Cardputer on USB) covers that firmware. New firmware requested with the device connected means flash.
- **Standing permission (Sergio, 2026-10-24):** if the Cardputer is on USB (`pio device list` shows the port), flash after every successful build without asking, using the normal `cardputer-adv` env (not the diagnostic variants unless asked). If Sergio does not want a flash, he unplugs the device. No visible port: do nothing.
- No serial port visible (`pio device list` empty): do not try anything; leave `firmware.bin` ready for WebUI upload and say so.
- USB flashing replaces Launcher in that slot. Sergio knows and accepts it; restoring Launcher is his job.
- Command: `pio run -t upload --upload-port <COM>`. **The COM number is not fixed**: it changes with the USB port and the computer. Always detect it with `pio device list` before flashing: it is the "USB Serial Device" port ("Standard Serial over Bluetooth link" ports are not the Cardputer). Never assume a COM or write one into repo files. No button press needed: esptool puts the device in download mode over native USB and resets it at the end. The screen stays black a few seconds, then the app starts; normal, not a failure.
- After flashing, tell Sergio what to check on screen/by ear; Claude cannot see or hear it.

## Commands

- Build: `pio run`
- Serial monitor (to debug what Claude cannot hear/see): `pio device monitor`
- Deploy: `pio run`, then upload `firmware.bin` through Launcher's WebUI (see `docs/DEPLOY.md` when it exists; until then the first deploy is manual), or flash over USB if Sergio asks (rule above).

## Conventions

- Small steps, one commit per working step. Build before each commit.
- **Claude always commits and pushes after every firmware build** (`pio run`, diagnostic envs included): after a successful build, `git add` the changes, commit with a descriptive message and `git push`, without waiting to be asked. Failed build: no commit. Failed push (network, auth): say so clearly and retry later; never report something as pushed without checking.
- No new dependencies without saying so. Pin library versions in `platformio.ini`.
- Claude cannot hear the speaker or see the screen: whatever can be verified should come out over serial.
- Design and acceptance criteria live in `docs/SPEC.md`. Read it before working.

## Firmware interface

- **All text shown by the firmware (screen and serial) is always English**, even if Sergio describes the feature in Spanish. Includes labels, messages, mode names and logs.
- **The interface is silent**: no beeps on key presses, no sounds in menus or games, to avoid accidental triggers in rehearsals. Only musical sounds: metronome click and reference note (drone).
- Fixed one-line bottom bar (1 px line above) for modes; the rest of the screen belongs to the active mode. Global keys: `M` metronome, `T` tuner, `G` game, `A` extra; `-`/`_` and `=`/`+` lower/raise global volume (both variants accepted so Shift is not needed).
