# Cardputer IRC Client

- This is a PlatformIO project targeting the M5Stack Cardputer (`m5stack-stamps3` environment).
- Build command: `pio run -e m5stack-stamps3`
- All code comments must be in English.
- The application requires an SD card with `/irc/config.txt` for runtime configuration.
- On-device configuration menu is accessible via the `G0` / `BtnA` button.
- Build artifact definitions (launcher vs. fullflash) are managed in `.github/workflows/build-cardputer.yml`.
