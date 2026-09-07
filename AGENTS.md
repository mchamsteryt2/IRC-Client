# Cardputer IRC Client

## Build & verify
- PlatformIO only env `m5stack-stamps3` (`platform=espressif32`, `board=m5stack-stamps3`, `framework=arduino`): `pio run -e m5stack-stamps3`
- `platformio.ini:6-11` — `lib_deps` `M5Cardputer` / `M5GFX` / `M5Unified`, `monitor_speed=115200`, `build_flags=-D CORE_DEBUG_LEVEL=3`; no test/lint/typecheck/formatter config exists — build is the only verification.
- CI installs `pip install platformio esptool` then same `pio run` (`.github/workflows/build-cardputer.yml:35`).

## Architecture
- Target `m5stack-stamps3` (ESP32-S3): **512 KB SRAM, no PSRAM** — heap ~320 KB free at boot (`src/main.cpp:3847`), `largest<20KB` warns (`src/main.cpp:827`), `largest<35KB`/`free<45KB` guards TLS and recycles history (`src/main.cpp:2944`), `brownout` disabled (`WRITE_PERI_REG RTC_CNTL_BROWN_OUT_REG:3881`). Never assume PSRAM; `String` heap fragments quickly and is avoided in hot paths (`src/main.cpp:301,100,3149`). Debug ring is Serial-only when `debug_log_enabled` to save RAM (`src/main.cpp:100`).
- Single source file `src/main.cpp` (~4000 LOC) — all logic in one translation unit.
- `MAX_NETWORKS 3` (`src/main.cpp:167`, was 5 saves ~50KB TLS), `MSG_BUFFER_SIZE 8` (was 10 saves ~3KB), `MAX_TABS 8` (was 10 saves ~3.2KB), `input_history 6` (was 10), `gLogQueue 10` (was 20 saves 1.2KB), stacks `10240` (was 12288 saves 4KB) — tuned for 512KB; truncates 4th/5th bouncer net with WARN `src/main.cpp:2947`.
- Dual FreeRTOS tasks `15ms` poll pinned to cores: `NetworkTask` core 0 and `CustomUITask` core 1 (`src/main.cpp:3963`), both under TWDT (10s init, tasks added after create) with `wdt_emergency_flush()`; `loop()` deletes itself (`src/main.cpp:3971`). Poll was 10ms, flush was 2s now 5s to save SD wear/CPU.
- Shared state guarded by `irc_mutex` / `sd_mutex` and `gLogQueue`; 256-byte `log_sector_cache` batches SD writes — respect mutexes when adding SD/IRC access; `handle_vault_scan_complete` has `busy` guard to avoid dual-core race (`src/main.cpp:573`).
- Display: `M5Canvas` sprite `240x109` (~52 KB, fallback `135x214`/`240x135`) is the safe max without PSRAM (`src/main.cpp:3848,1010` re-creates on `width==0` with trim); full-screen `240x135` OOMs. Brightness forced to `180` — hardware LED on GPIO 21 is tied to backlight rail (`src/main.cpp:281`).

## SD runtime (trust code over `README.md`)
- Repo template `irc/config.txt` → device requires `SD:/irc/config.txt`. If `wifi_ssid=YOUR_WIFI` device opens on-device config instead of connecting.
- Runtime files under `SD:/irc/` (lowercase, not `README`'s `/IRC`/`/serialLog.txt`): `config.txt`, `wifi_cache.txt` (3-entry `SSID:PASS` vault, was 5 saves 256B, LRU via `save_wifi_vault_lru()`), `session_state.tmp` (`"<netIdx> <tabIdx> <scrollFlag>"`), `history.txt`, `ignore.txt`, `highlight.txt`, `alias.txt`, `logs/<server>/<room>_YYYY_MM_DD.log`, `system/system_YYYY_MM_DD.log` (`src/main.cpp:482`). SD is self-healed `3×` at `SPI 40/39/14/12 CS=12` (`src/main.cpp:3798`).
- `purge_old_logs()` caps per-file `512KB` / system purge; do not add unbounded logging.

## CI artifacts (`.github/workflows/build-cardputer.yml`)
- `PIO_ENV=m5stack-stamps3`, `ESP_CHIP=esp32s3`, `FLASH_MODE=qio`, `FLASH_FREQ=80m`, `FLASH_SIZE=8MB`.
- Outputs in `dist/`: `launcher/cardputer-launcher.bin` (= `firmware.bin` at `0x10000`, for M5Launcher/M5Apps), `fullflash/cardputer-fullflash.bin` (merged `0x1000 bootloader + 0x8000 partitions + 0xe000 boot_app0 + 0x10000 firmware`), `split/{firmware,bootloader,partitions}.bin`. Fullflash flashed at `0x0`, never via Launcher.

## Conventions & gotchas
- All code comments must be English.
- On-device config: `Fn+O` cycles `SETTINGS → BOUNCER → THEME → WI-FI → CHAT` (`src/main.cpp:1809`); `` ` `` or `Alt+Del` saves & exits via `SD.open("/irc/config.txt", FILE_WRITE)` (`src/main.cpp:1889`). Inside menus: `;` up / `.` down / `,` decrement / `/` increment / `ENTER` toggle (`src/main.cpp:2145,2185,2269`).
  - `SETTINGS`: `current_tz_idx`, `use_12_hour_format`, `use_dst`, `channel_log_enabled`, `debug_log_enabled` (`src/main.cpp:1136`)
  - `BOUNCER`: `bnc_host`, `bnc_port`, `bnc_user` ( `bnc_pass` persisted but not rendered ) (`src/main.cpp:1145`)
  - `THEME`: `use_light_theme`, `theme_accent` (`DEFAULT/AMBER/CYAN/PURPLE/EPAPER` mono paper `EPAPER` `0x0000/0xFFFF` `160` to highlights), `text_scale`, `speaker_enabled`, `sound_profile` (now persisted) (`src/main.cpp:1155,160`) — `EPAPER` keeps `neopixelWrite 21:281` same, UI `leds` `heap 14/vault 18` visible both themes
  - `WI-FI`: `wifi_ssid`/`wifi_pass` + `wifi_ssid2`/`wifi_pass2` (backup, now persisted) (`src/main.cpp:1176,344`)
  - Persisted `SD:/irc/config.txt` keys are exactly the 20 parsed in `load_settings_from_sd:344` / written `src/main.cpp:1896`: `wifi_ssid`, `wifi_pass`, `wifi_ssid2`, `wifi_pass2`, `irc_nick`, `bnc_host`, `bnc_port`, `bnc_user`, `bnc_pass`, `channel_log_enabled`, `screen_brightness`, `current_tz_idx`, `use_12_hour_format`, `use_dst`, `use_light_theme`, `theme_accent`, `text_scale`, `speaker_enabled`, `sound_profile`, `debug_log_enabled`. Repo template `irc/config.txt` / `README.md` list stale keys (`irc_host`, `irc_server_preset`, `proxy_*`, `sasl_*`, `autojoin`, etc.) that are ignored — do not copy them expecting effect.
- Config parsing is `char[64]` + `strncpy` with manual `=` split (`src/main.cpp:301`); keep `update_config_string()` bounds, do not introduce `String` heap in hot paths (heap <20KB triggers warning `src/main.cpp:817`).
- TLS via `WiFiClientSecure`; TLS-through-proxy is not implemented; SASL only `PLAIN`.
