# WiFi USB Drive on the Waveshare ESP32-S3-LCD-1.47 — Plan

Goal: the board plugs into a PC (or 3D printer) and behaves like an ordinary USB
stick backed by the microSD card, while files can also be sent to it over WiFi
from a browser. Drafted 2026-10-21 from the board schematic, the Waveshare demo
code, Espressif's own USB-disk examples, and an earlier PlatformIO + Arduino
attempt that is kept outside this repository.

## 1. Where things stand today

**Board state.** It currently enumerates on this PC as a single-function USB
Mass Storage device (Espressif VID 0x303A, PID 0x1001, product string
"Espressif ESP32-S3-DevKitC-1-N8", i.e. the PlatformIO board name) reporting
**No Media**, with no serial port. That is the February firmware from the
`wifi-drive` project, sitting either in its WiFi-edit state or with the SD card
failed. Because there is no CDC port, reflashing needs the manual download-mode
entry: unplug, hold BOOT, plug in, release.

**Earlier attempt (`wifi-drive`, Feb 2026, PlatformIO + Arduino 2.0.17).** It
already had the right ideas: an exclusive USB-mode / WiFi-mode state machine,
the correct SD and LCD pins, an LCD status screen (Arduino_GFX), a web UI with
upload/delete/switch, mode persistence in NVS, BOOT-button toggle. What held it
back:

- Manual mode switching, so it never behaved like a "normal" stick.
- MSC read/write callbacks move one 512-byte sector per call (~0.3–0.5 MB/s;
  multi-sector calls roughly double that).
- Reaches the SD card handle through a `#define protected public` hack on the
  Arduino `SD_MMC` class.
- SD mounted 1-bit only, with the 4-bit attempt last.
- Arduino 2.0.x core (ESP-IDF 4.4) and an ESPHome fork of ESPAsyncWebServer.
- WiFi credentials hard-coded in `src/main.cpp`.
- Lives in a Google-Drive-synced folder; `.pio/` build output churns the sync.
- Uncommitted edits on top of its single commit.

**Toolchain on this PC.** Python 3.13 venv in `D:\Dev\esp32\.venv` with ESPHome
2026.9.0, PlatformIO Core 6.1.19 and esptool 5.3.1; VS Code with the PlatformIO
IDE extension; platform `espressif32@6.7.0` cached. No ESP-IDF, no Arduino IDE.
git 2.52 and gh 2.102 available.

## 2. Hardware facts (from the schematic, verified)

| Function | GPIO / detail |
|---|---|
| MCU | ESP32-S3R8: 8 MB **octal** PSRAM in package, 16 MB flash (W25Q128) |
| USB | USB-A male plug. D- → 22R → GPIO19, D+ → 22R → GPIO20. **Native USB**, no UART bridge. Bus-powered via VBUS → ME6217 3.3 V LDO (800 mA). |
| microSD (SDMMC) | CLK 14, CMD 15, D0 16, D1 18, D2 17, D3 21. **10K pull-ups on all six lines**; 0R jumpers (R11, R18) in series with D2/D1. No card-detect. → wired for 4-bit SDMMC, though both Waveshare demos run it 1-bit. |
| LCD ST7789, 172×320 | MOSI 45, SCLK 40, CS 42, DC 41, RST 39. Backlight: GPIO48 → N-MOSFET, active high, PWM-able. Panel needs BGR order, inversion on, x offset 34 (confirmed by both Waveshare's driver and the Feb code). |
| RGB LED | WS2812B on GPIO38 |
| Buttons | BOOT on GPIO0 (10K pull-up). RESET key on CHIP_PU with 10K/1µF, but the wiki says the reset button is "reserved, enable by re-soldering a resistor" — assume power-cycle is the reset. |
| UART0 | TXD/RXD (GPIO43/44) on the 22-pin header → a USB-serial adapter gives a console and a flash path that does not depend on the USB port. |
| Not usable | GPIO35–37 (octal PSRAM). GPIO45/46 are strapping pins (45 is LCD MOSI, fine after boot). |

## 3. The core problem and the chosen design

A FAT volume cannot be written by two parties at once. The PC caches the FAT
and directory sectors; if the ESP32 writes a file underneath it, the next PC
write can allocate the same clusters and corrupt the card. Every existing
project deals with this somehow:

- Espressif's `usb_msc_wireless_disk` keeps FatFS mounted on the ESP32 *and*
  serves raw sectors to the host at the same time, and ships a "reset MSC"
  endpoint that fakes a USB unplug so the host re-reads. The README calls it a
  "function preview". Not safe for real data.
- ThingPulse's Pendrive S3 reboots the whole device after every web upload.
- The Feb `wifi-drive` code made the user switch modes by hand.

**Design: USB owns the card by default; WiFi operations borrow it.**

1. Normal state: the card is exposed to the host; the ESP32 does not mount the
   filesystem at all. The host sees an ordinary removable disk.
2. A web upload/delete/mkdir arrives → the firmware waits for USB to go idle
   (no READ/WRITE for ~1–2 s), then switches the storage to the application:
   the host sees "medium not present", exactly like a card pulled out of a
   reader. Windows keeps the drive letter and forgets its caches.
3. The ESP32 mounts FatFS, performs the operation(s), syncs and unmounts.
4. After an idle timeout (e.g. 5 s, configurable) or when the user clicks
   "Done", the storage goes back to the host, which re-reads the volume fresh.
5. A sticky **edit mode** (web button or BOOT long-press) keeps the card on the
   ESP32 side for bulk uploads; the LCD and LED show which side owns it.
6. Directory listing while USB owns the card is served from a cached index
   taken during the last borrow (with a "refresh" that borrows briefly).
7. For hosts that do not notice media changes (some 3D-printer firmwares) a
   "hard handoff" option forces a full USB re-enumeration by toggling the
   VBUS-valid signal, the trick Espressif's example uses in `reset_msc`.

esp_tinyusb 2.x implements steps 2–4 directly:
`tinyusb_msc_set_storage_mount_point(hdl, TINYUSB_MSC_STORAGE_MOUNT_APP)` /
`..._MOUNT_USB`, with `MOUNT_START/MOUNT_COMPLETE/MOUNT_FAILED/FORMAT_REQUIRED`
events. The ESP-IDF v5.5 example `examples/peripherals/usb/device/tusb_msc`
shows it with an SDMMC card (its `expose` console command is the handoff).

What this costs: the drive disappears from the host for a few seconds whenever
a WiFi write happens. That is the only data-safe behaviour short of making USB
read-only, and it is what makes the stick "normal" the rest of the time.

## 4. Expectations to set before starting

- **USB speed is capped by the chip.** ESP32-S3 USB is full-speed (12 Mbit/s):
  about 1 MB/s reads and 0.7–0.9 MB/s writes at best (atomic14's measurements
  with 4-bit SDIO and multi-sector transfers). A 100 MB file takes ~2 minutes.
- **WiFi is the fast path.** Espressif measured 4.2 MB/s average HTTP uploads
  on an S3 with octal PSRAM and tuned buffers. 4-bit SD matters here more than
  for USB.
- **FAT32 only.** FatFS in ESP-IDF/Arduino has no exFAT. Cards over 32 GB come
  exFAT and Windows refuses to format them FAT32 in the GUI; use a tool (Rufus,
  guiformat) or let the firmware format the card.
- **Console/flash change.** When TinyUSB owns the USB port there is no
  USB-Serial-JTAG console and esptool cannot auto-reset the board. Plan for
  (a) a CDC-ACM interface next to MSC for logs, (b) OTA from the first real
  build, (c) UART0 on the header as the fallback, and (d) BOOT-hold at plug-in
  for recovery.

## 5. Toolchain choice

**Recommended: ESP-IDF v5.5.6 (latest 5.5.x), esp_tinyusb 2.x.** Reasons:
Espressif's MSC storage layer does the ownership handoff, multi-sector
transfers and SDMMC 4-bit for us; the wireless-disk example's HTTP server, WiFi
settings page and NVS handling can be ported as-is; and the Waveshare ESP-IDF
demo gives the LCD/LED init. Both Espressif examples are tested on 5.x
(esp-iot-solution master needs ≥ 5.3; its configs were generated with 5.4; the
IDF 5.5 MSC example pins `esp_tinyusb ^2.0.0`). Skip 6.x for now: the "stable"
docs already point at v6.1, but neither example is validated there.

Install: `winget install Espressif.EIM` (GUI) or `winget install
Espressif.EIM-CLI` then `eim install -i v5.5.6`; the VS Code "ESP-IDF"
extension drives the same installer. Needs git and Python ≥ 3.10, both present.

**Alternative kept open: stay on PlatformIO/Arduino.** Upgrade `wifi-drive` to
the pioarduino platform (Arduino 3.3.12 / IDF 5.5.5), keep its state machine,
and fix the known problems: initialise the card with IDF `sdmmc_host` calls
instead of the `SD_MMC` hack, use `sdmmc_read_sectors`/`sdmmc_write_sectors`
for multi-sector MSC transfers, implement the borrow/return with
`USBMSC::mediaPresent(false/true)`, move to 4-bit, swap the web server for the
maintained ESP32Async fork, add `Update`-based OTA. Viable, familiar tooling,
but every piece of MSC plumbing stays hand-rolled. pioarduino also supports
`framework = espidf`, so the ESP-IDF project could be built from PlatformIO if
one IDE is preferred.

## 6. Phases

### Phase 0 — Prep (~30 min)

- Use `D:\Dev\esp32\wifi-usb-drive\` (this folder) as the home of the new
  project; copy only what is worth keeping from the Drive folder (`web_index.h`
  UI, LCD drawing code, README ideas). Keep secrets in a gitignored header or
  NVS, not in source.
- Format a ≤32 GB card FAT32 (or plan on the firmware's format path).
- Install ESP-IDF v5.5.6 + VS Code ESP-IDF extension.
- Prove the board can be flashed: unplug, hold BOOT, plug in → a "USB JTAG/
  serial debug unit" COM port appears → `esptool --port COMx chip_id`.
- Optional but worth it: wire a USB-serial adapter to the header TXD/RXD/GND
  for a console that survives whatever the USB port is doing.

### Phase 1 — Milestone A: plain USB stick with Espressif's example (~1 h)

Build `examples/peripherals/usb/device/tusb_msc` from ESP-IDF v5.5.6:

- menuconfig: storage media = SD/MMC card (not SPI flash), bus width 4, pins
  CLK 14 / CMD 15 / D0 16 / D1 18 / D2 17 / D3 21.
- `sdkconfig.defaults` additions for this board:
  `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`, `CONFIG_SPIRAM=y`,
  `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_80M=y`,
  `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y`, `CONFIG_TINYUSB_MSC_BUFSIZE=4096`
  (try 8192/16384 later). Keep the example's `SDMMC_SLOT_FLAG_INTERNAL_PULLUP`
  on; the board has external pull-ups anyway.
- Flash, confirm Windows mounts the card, copy 100 MB each way and note MB/s.
- If 4-bit fails to init, drop to 20 MHz (`SDMMC_FREQ_DEFAULT`) first, then
  1-bit, and record which works; the 0R jumpers R11/R18 are the suspects if
  only 1-bit works.
- Use the `expose`/`read`/`write` console commands to watch the handoff from
  the host side (drive goes "no media", comes back with fresh contents).

Done when: the card is a reliable USB stick and the handoff is observed.

### Phase 2 — Milestone B: wireless-disk preview (~1–2 h)

Build esp-iot-solution `examples/usb/device/usb_msc_wireless_disk` (master):

- Board "ESP32 S3 GENERIC", storage "External SDCard", interface SDIO, 4-line,
  the pins above; start from `sdkconfig.ci.esp32s3r8` (octal PSRAM, WiFi/LWIP
  buffers, 128 KB ring-buffer chunks) plus 16 MB flash.
- WiFi: STA SSID/password (menuconfig or the settings page); the AP
  `ESP-Wireless-Disk` at 192.168.4.1 stays as fallback.
- Measure HTTP upload speed with the bundled `tools/upload_speed_test.py`.

This is a throwaway: it validates WiFi throughput on this board and gives the
HTTP server (`app_http_server.c`), WiFi/NVS code (`app_wifi.c`) and web pages
(`spiffs/*.html`) to port. Do not keep real files on a card used by this build
while the PC also writes to it.

### Phase 3 — Milestone C: the real firmware (the bulk of the work, ~1–2 days)

New ESP-IDF project in this folder, components:

1. **Storage/USB**: Milestone A's SDMMC init + `tinyusb_msc_new_storage_sdmmc`
   with initial mount point USB. Composite descriptor MSC + CDC-ACM; route the
   console to CDC (`esp_tusb_init_console`) so `idf.py monitor` works again.
2. **HTTP file server**: port `app_http_server.c` / `app_wifi.c` / web pages
   from Milestone B. Every filesystem call goes through one `storage_borrow()`
   / `storage_release()` pair guarded by a mutex; the handlers never touch the
   VFS while the host owns the card.
3. **Ownership state machine** (new, the heart of the project):
   `USB_OWNED → BORROW_PENDING (waiting for USB idle) → APP_OWNED → RELEASE
   (fsync, unmount, idle timer)`; sticky edit mode; host-eject handling (after
   the host ejects, stay ejected until replug, as the example does); "soft"
   (media change) vs "hard" (VBUS-valid toggle re-enumeration) release,
   selectable in settings.
4. **Networking**: STA with AP fallback, mDNS name (`wifidrive.local`),
   settings persisted in NVS (SSID/password, idle timeout, handoff mode,
   read-only USB toggle).
5. **OTA**: `/ota` POST handler writing to the inactive OTA slot. Partition
   table for 16 MB: nvs, otadata, phy_init, ota_0 (3 MB), ota_1 (3 MB), rest
   free. From here on, flashing is over WiFi; BOOT-hold is for recovery only.
6. **Logging/diagnostics**: `/api/status` JSON (owner, card size/free, last
   transfer speed, WiFi RSSI, uptime) used by both the web UI and the LCD.

Done when: files dropped on the web page appear on the PC's drive within the
idle timeout, files copied on the PC appear in the web listing after a refresh,
and nothing in the test matrix below corrupts the card.

### Phase 4 — Milestone D: display, LED, button

- LCD via `esp_lcd` ST7789 with Waveshare's settings (BGR, mirror X, 34-px gap,
  backlight PWM on GPIO48). Either plain text with `esp_lcd` + a bitmap font,
  or LVGL 8.3 as the Waveshare demo does. Show: owner (USB / WiFi edit), IP or
  AP name, card used/free, last transfer rate, errors.
- WS2812 (`led_strip` component): green/red flashes on USB read/write (as
  ThingPulse does), blue while the ESP32 owns the card, amber while waiting
  for USB idle.
- BOOT long-press toggles sticky edit mode; short press shows IP/QR on the
  LCD.

### Phase 5 — Milestone E: validation

| Test | Expect |
|---|---|
| 100 MB copy PC→stick and stick→PC | ~0.7–1 MB/s, file hashes match |
| Web upload while PC idle | drive blinks out, returns, file visible on PC |
| Web upload while PC is copying | upload waits for idle, no interleaving |
| PC copies while card is borrowed | PC reports "no media", copy fails cleanly, resumes after release |
| Host "Eject" in Windows | stays ejected until replug |
| Power pull mid-upload | card still mounts; partial file only |
| exFAT card inserted | web page says "format needed", format works |
| 3D printer host (if still the target) | files visible after release; if not, hard handoff mode |
| 24 h soak with periodic uploads | no heap leak (`/api/status`), no reset |

## 7. Reference material used

- Waveshare wiki and demo zip (SD init `main/SD_Card/SD_MMC.c`, LCD init
  `main/LCD_Driver/ST7789.c`, Arduino `SD_Card.cpp`), schematic PDF.
- ESP-IDF `examples/peripherals/usb/device/tusb_msc` (release/v5.5) —
  esp_tinyusb 2.x SDMMC storage and app/USB mount-point switching.
- esp-iot-solution `examples/usb/device/usb_msc_wireless_disk` — HTTP file
  server, WiFi/NVS settings, tuned `sdkconfig.ci.esp32s3r8`, speed tools.
- ThingPulse `esp32-s3-pendrive-wireless-usb-disk` — Arduino reference,
  reboot-after-upload approach, LED activity scheme.
- JMcrafter26 `cloud-stick` — Arduino 3.x / pioarduino `platformio.ini` for
  an S3 + LCD + SD dongle (Path B template).
- atomic14, "SD Card Shenanigans — Using the ESP32-S3 in USBMSC" — throughput
  numbers and the multi-sector lesson.
- pioarduino `platform-espressif32` — Arduino 3.3.12 / IDF 5.5.5, `espidf`
  framework examples.

## 8. Progress log

### 2026-10-21 — Phase 0 done, Milestone A built (not yet flashed)

- ESP-IDF v5.5.6 installed with EIM 0.20.1 into `D:\esp\v5.5.6\esp-idf`, tools
  and Python venv under `C:\Espressif\tools`, target esp32s3 only.
  Activate in PowerShell 7 with:
  `. C:\Espressif\tools\Microsoft.v5.5.6.PowerShell_profile.ps1`
  (EIM's generated script had to be patched: the machine PATH contains a
  corrupted entry `C:\Program Files\PowerShell\7"` whose stray quote broke the
  script. The PATH entry itself still needs fixing in Environment Variables.)
- `a-usb-stick/` = ESP-IDF v5.5.6 `tusb_msc` example + `sdkconfig.defaults`
  for this board (SDMMC 4-bit on 14/15/16/18/17/21, 16 MB flash, octal PSRAM,
  console on UART0). Builds clean: `idf.py -C a-usb-stick build` →
  `build/tusb_msc.bin` (388 KB). Pulled esp_tinyusb 2.x via the component
  manager.
- Boot behaviour to remember: no card inserted → the firmware retries card
  detection forever and USB never starts. Plugged into a PC at boot → the card
  is handed to the PC automatically.
- Flash: board in download mode (unplug, hold BOOT, plug in, release), then
  `idf.py -C a-usb-stick -p COMx flash`.
- `tools/usb-speed-test.ps1 -Drive X` measures real write/read MB/s with the
  Windows cache bypassed.

### 2026-10-21 — Milestone A flashed; reset quirk found

- Flashed over the ROM's USB-Serial-JTAG port (COM7, VID 303A PID 1001 with a
  CDC interface) with `idf.py -p COM7 flash`. Write verified.
- **Quirk:** when download mode was entered with the physical BOOT button,
  every reset issued through the USB-JTAG control lines (esptool's
  `--after hard_reset`, or RTS toggling) comes back as
  `rst:0x15 (USB_UART_CHIP_RESET),boot:0x0 (DOWNLOAD(USB/UART0))`, i.e. straight
  back into download mode. Checked via the ROM: GPIO0 reads high between
  resets and `RTC_CNTL_OPTION1_REG` (force-download flag) is 0, so neither the
  button nor the flag is the cause. Only a power-cycle exits it, and this
  board's reset button is not populated.
- Consequence for the dev loop: BOOT-hold replug → flash → replug again.
  Mitigation for Milestone C: a software "reboot to bootloader" (set
  `RTC_CNTL_FORCE_DOWNLOAD_BOOT` and restart, triggered from the web UI or a
  BOOT long-press) so download mode is entered without the button, plus OTA.
- The build currently on the board is the diagnostic variant:
  `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y` mirrors bootloader/app logs
  to the USB-JTAG CDC port until TinyUSB takes over the USB PHY. Revert to
  `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` for the real firmware.

### 2026-10-21 — Milestone A reached (card mounts over USB); 4-bit SD does not work on this board

- After a power-cycle the app boots normally (`boot:0x8`). With the 4-bit
  config the SD init failed every time. Debug log (`sdkconfig.debug` overlay)
  shows the card answering fine in the 1-bit init phase (SDHC, OCR 0xc0ff8000),
  then the first data read after switching to 4-bit fails with SDMMC status
  0x2000 = start-bit error on the data lines. That is the signature of D1/D2
  not reaching the card: the 0R jumpers R11/R18 on those lines are evidently
  unpopulated. It also explains why both Waveshare demos use 1-bit mode.
  **Decision: 1-bit SDMMC is the board default** (`sdkconfig.defaults`).
  4-bit would need two 0R (or solder-bridge) links on R11/R18.
- With 1-bit the stock example works end to end: Windows sees "TinyUSB Device"
  (VID 303A, PID 4002), disk "TinyUSB TEST MSC Storage", volume `WIFI-DRIVE`
  FAT32 29.7 GB mounted as E:, existing gcode files visible, 50 MB round trip
  hash-verified.
- Speed test on the *debug* build (DEBUG log level, logs over 115200-baud
  UART, 1-bit at 20 MHz): 0.17 MB/s write, 0.42 MB/s read. Not representative;
  the clean build is being measured next.
- Flashing while TinyUSB owns the USB port needs the BOOT-hold replug; while
  the app is stuck before TinyUSB (e.g. no card), the ROM's USB-JTAG port stays
  up and `esptool --before default_reset` enters download mode by itself.
  `tools/flash-and-log.ps1` automates that case.
- Added to the example: hold BOOT for 1 s while running → restart into ROM
  download mode (`RTC_CNTL_FORCE_DOWNLOAD_BOOT`), so a flash cycle becomes
  "hold BOOT 1 s → esptool" with no replug. To be verified.

## 9. Hardware mod: enabling 4-bit SD (optional)

**Evidence.** In 4-bit mode the first data transfer fails with SDMMC status
0x2000 (start-bit error): the host saw no start bit on one or more of D1/D2/D3.
The card works perfectly in 1-bit mode on D0, so CLK/CMD/D0 are fine. On the
schematic the only lines with series 0R jumpers are D2 (R11, between SD_D2 and
SDIO_D2) and D1 (R18, between SD_D1 and SDIO_D1); D3 (GPIO21) runs straight to
the slot. Both Waveshare demos use 1-bit, consistent with those jumpers being
unfitted. Unverified visually: the pads could also be fitted and the fault
elsewhere, so check before soldering.

**Check (card removed, board unpowered, multimeter in continuity mode).**
- Find the two small two-pad footprints near the TF slot on the SD_D1/SD_D2
  traces (R11, R18; probably 0402). Empty pads = confirmed.
- Optionally confirm each pad pair: one side goes to the 10K pull-up (R12 for
  D2, R17 for D1) and on to the ESP32 (GPIO17 = D2, GPIO18 = D1); the other side
  goes to slot contact 1 (DAT2) or 8 (DAT1). microSD slot contacts: 1 DAT2,
  2 CD/DAT3, 3 CMD, 4 VDD, 5 CLK, 6 VSS, 7 DAT0, 8 DAT1.
- If R11/R18 turn out to be fitted, probe D3 instead: slot contact 2 to GPIO21
  / R13.

**Fix.** Bridge R11 and R18 with a solder blob or 0R 0402 parts. Then set
`CONFIG_EXAMPLE_SDMMC_BUS_WIDTH_4=y` (and drop `_1`) and rebuild; the debug
overlay shows "using 4-bit bus" followed by the card info if it worked.

**Worth it?** Not for USB: full-speed USB caps the stick at about 1 MB/s either
way. It matters for the WiFi path: 1-bit at 20 MHz tops out around 2 MB/s raw
(try 40 MHz, `SDMMC_FREQ_HIGHSPEED`, first; software only), 4-bit at 40 MHz is
closer to 8–10 MB/s raw, enough to keep the ~4 MB/s HTTP uploads card-bound
rather than bus-bound.

### 2026-10-21 — Milestone A measured on the clean build

Clean build = 1-bit SDMMC at 20 MHz, INFO logging, no secondary console,
`CONFIG_TINYUSB_MSC_BUFSIZE=8192` (the ESP32-S3 maximum; 16384 is out of range
and silently falls back to 512). Windows: "TinyUSB Device" VID 303A PID 4002,
volume `WIFI-DRIVE` FAT32 29.7 GB as E:.

| Test (Windows cache bypassed) | Write | Read | Hash |
|---|---|---|---|
| 50 MB  | 0.88 MB/s | 0.62 MB/s | OK |
| 100 MB | 0.87 MB/s | 0.68 MB/s | OK |
| (debug build, 512 B buffer, 50 MB) | 0.17 MB/s | 0.42 MB/s | OK |

Writes sit at the practical full-speed USB limit (~0.9 MB/s, atomic14 saw
0.66–0.9). Reads are below the ~1 MB/s that should be reachable; candidates
for a later tuning pass: 1-bit at 40 MHz (`host.max_freq_khz =
SDMMC_FREQ_HIGHSPEED`), and the synchronous read path in esp_tinyusb (each
8 KB USB chunk waits for its SD read). Not worth more time before Milestone C.

Also confirmed: `esptool run` does not get the chip out of the stuck download
mode either; only a power-cycle does.

### 2026-10-21 — BOOT-hold helper works; flashing in that mode needs `--before no_reset`

- Holding BOOT ~1 s while the app runs restarts the chip into the ROM download
  mode. Because the app had switched the USB PHY to OTG, the ROM comes up in its
  **USB-OTG** download flavour: Windows shows "USB Serial Device (COMx)" plus an
  "ESP32-S3" DFU interface (driver error, harmless), not the "USB JTAG/serial
  debug unit" seen after a BOOT-at-power-on entry.
- On that OTG serial port, esptool's default pre-flash DTR/RTS reset resets the
  chip straight back into the app (the force-download flag is one-shot), and
  esptool then fails with "A device attached to the system is not functioning".
  Fix: `--before no_reset` (now the project default via
  `CONFIG_ESPTOOLPY_BEFORE_NORESET=y`, so `idf.py flash` just works). The
  post-flash hard reset through the OTG port boots the app normally.
- Resulting dev loop: hold BOOT 1 s → `idf.py -p COMx flash` → app runs.
  No replugging. (Still to confirm end to end.)

### 2026-10-21 — Milestone A complete: ownership handoff observed from Windows

- `handoff_demo_task` in the example (20 s after boot, then once a minute):
  `tinyusb_msc_set_storage_mount_point(APP)` → append a line to
  `HANDOFF.TXT` via the ESP32's FatFS mount → hold 5 s → `..._MOUNT_USB`.
  Seen from Windows with `tools/watch-handoff.ps1`:

  ```
    11.4s  mounted as E: (WIFI-DRIVE)
    19.9s  no media         (ESP32 owns the card, writes the file)
    48.3s  mounted as E:    (first hand-back; Windows took ~23 s to re-read)
    85.7s  no media
    91.6s  mounted as E:    (second hand-back; 6 s)
  ```
  `HANDOFF.TXT` on E: contains both lines, no filesystem damage. While the
  app holds the card esp_tinyusb answers TEST UNIT READY with
  "medium not present" (`tinyusb_msc.c`), and a host eject hands the card to
  the app. The first re-mount lag is worth watching in Milestone C (Windows'
  media-change polling vs. a forced re-enumeration).
- Dev loop confirmed: hold BOOT 1 s → ROM download mode (USB-OTG flavour,
  VID 303A PID 0009, COMx) → `idf.py -p COMx flash` (before=no_reset is the
  project default) → app boots → drive back. One glitch seen in three cycles:
  Windows got "Invalid Device Descriptor" when the ROM device appeared right
  after the MSC device vanished; needed a replug. The helper now calls
  `tud_disconnect()` and waits 700 ms before restarting (flashed, not yet
  exercised).
- The firmware currently on the board is this demo: it takes the drive away
  for ~5 s once a minute. Rebuild without `handoff_demo_task` for normal use.

**Milestone A exit criteria: met.** Next: Phase 2 (esp-iot-solution
`usb_msc_wireless_disk` for WiFi throughput + HTTP server code), then
Milestone C.

### 2026-10-21 — Milestone B: wireless-disk example running, WiFi throughput measured

- `b-wireless-disk/` = esp-iot-solution `usb_msc_wireless_disk` (esp_tinyusb
  1.7.6) with the upstream r8 profile, adapted: 1-bit SD on 14/15/16 at 40 MHz
  (the example's `SDMMC_FREQ_HIGHSPEED`), 16 MB flash, octal PSRAM, station
  WiFi (credentials in gitignored `sdkconfig.secrets`, generated from
  `../secrets.yaml`), softAP off, format-on-mount-failure OFF, console on
  UART0 (+ USB-JTAG mirror), BOOT-hold helper, `before=no_reset` flashing.
  App is 1.46 MB (the esp32_s3_usb_otg BSP dependency drags LVGL in; unused).
- Runs: card mounts on the PC as E:, board joins the LAN as **<drive-ip>**
  (MAC <board-mac>; `tools/find-board-ip.ps1 -Subnet <lan-prefix>` finds it),
  web UI "ESP32 USB File Server" at http://<drive-ip>/ (list, upload,
  download, delete, settings page, `/reset_msc`).
- Throughput (PC on Ethernet, board on 2.4 GHz WiFi, PC idle on the drive):

  | Test | Rate |
  |---|---|
  | `tools/wifi-upload-test.py`, 2 × 50 MB upload | 1.18 / 1.15 MB/s |
  | same, download back + SHA-256 | 1.38 / 1.34 MB/s, hashes OK |
  | Espressif `upload_speed_test.py`, 10 × 10 MB | avg 1.18, max 1.50, min 0.89 MB/s |
  | (USB, Milestone A, for comparison) | 0.87 write / 0.68 read MB/s |

  WiFi is already 1.3–2× faster than USB on this board, but far below
  Espressif's 4.24 MB/s reference (ESP32-S3-USB-OTG, 4-bit SD). Suspects, in
  order: 1-bit SD (writes ~1.5–2.5 MB/s raw at 40 MHz), WiFi link rate (the
  board's small ceramic antenna; downloads top out at ~1.4 MB/s even though
  1-bit SD reads are faster than that), and the 16 KB HTTP read buffer.
  4-bit SD (R11/R18 bridge, PLAN §9) is the first thing to try if the WiFi
  path needs to be faster.
- Reminder: this example shares the card between USB and WiFi with no
  coordination. Uploads made over WiFi are invisible to the PC until a USB
  re-enumeration (`/reset_msc`) or replug, and PC writes during a WiFi write
  can corrupt the volume. It is a throughput/code reference only.
- Link check: 20/20 pings, latency 14 / 104 / 293 ms (min/avg/max): the
  signature of WiFi modem power-save, which ESP-IDF enables by default and the
  example never disables. Added `esp_wifi_set_ps(WIFI_PS_NONE)` after
  `esp_wifi_start()` for a re-measurement. Must be in the real firmware too.
- `/reset_msc` (VBUS-valid toggle → USB re-enumeration) works as the "hard
  handoff": the PC re-read the volume and showed the WiFi-written files.
- Espressif's `upload_speed_test.py` cleanup deletes its *local* file names,
  not the uploaded ones, so 10 × 10 MB `test_NNN_*.dat` stayed on the card;
  removed via POST `/delete/<name>` + `/reset_msc`.
- Re-measured with modem power-save off (`esp_wifi_set_ps(WIFI_PS_NONE)`):
  ping 2 / 15 / 103 ms (was 14 / 104 / 293), but throughput barely moved:
  upload 1.29 MB/s avg (max 1.35), download 1.35 MB/s avg (max 1.43);
  Espressif's tool avg 1.23, max 1.56 MB/s. So power-save was the latency
  problem, not the throughput ceiling. Remaining suspects: 1-bit SD writes and
  the WiFi link rate itself (downloads also cap near 1.4 MB/s). The clean way
  to split them in Milestone C: an iperf build (ESP-IDF `examples/wifi/iperf`)
  for raw TCP throughput at this spot, and 4-bit SD once R11/R18 are bridged.
- The corrected BOOT helper (eject media → 1 s → force-download restart)
  entered the ROM's USB-OTG download mode cleanly on this cycle; flash +
  auto-boot worked with no replug.

**Milestone B exit criteria: met.** WiFi path validated on this board
(~1.3 MB/s each way, ~1.5–2× USB), HTTP server / WiFi / settings code and web
pages available in `b-wireless-disk/` for Milestone C. The board currently
runs this example (shared-access caveat applies: don't write from the PC
while uploading over WiFi).

### 2026-10-21 — Milestone C firmware written and built (c-wifi-drive); flashing note

- `c-wifi-drive/` builds (1.56 MB, OTA layout ota_0/ota_1 3 MB each). Design as
  in §3: esp_tinyusb MSC driver installed with `auto_mount_off`, `owner.c`
  state machine (USB idle 1.5 s → borrow, hand back 5 s after the last WiFi op,
  sticky edit mode, hard-handoff option), cached directory listing, ported HTTP
  file server with borrow/release wrappers, `/api/status|owner|settings`,
  `/ota`, `/api/reboot`, `/api/bootloader`, CDC log port, LED, BOOT button.
- Tally of the software "reboot to ROM download mode" path (USB-OTG flavour of
  the ROM): 3 clean enumerations, 1 "Invalid Device Descriptor", 2 invisible
  on the bus, out of 6 tries on this PC. Not reliable enough as the primary
  flash path. From Milestone C on: **OTA (`POST /ota`) is the primary update
  path**; the physical button route (unplug, hold BOOT, plug in → USB-JTAG
  ROM mode → flash → replug) is the always-works fallback.

### 2026-10-21 — Milestone C on hardware: handoff test matrix

Firmware `c-wifi-drive` flashed (button route), then updated twice over the
air. Board: "WiFi Drive" composite (MSC + CDC log on COM9), card as E:, WiFi
<drive-ip> / http://wifidrive.local/, RSSI about -51 dBm.

| Test | Result |
|---|---|
| Boot | card scanned on the app side, handed to USB; Windows mounts E:; status API, mDNS OK |
| A: 20 MB WiFi upload, PC idle | card taken in 10 ms (USB idle), written, handed back 5 s later; file on E: with matching SHA-256 |
| B: edit mode (web button) | PC copy fails "device is not ready"; after leaving edit mode the PC copy of 50 MB succeeds |
| C1: PC copies 10 MB, WiFi upload 2 s later | upload waited for the PC copy + 1.5 s idle, then succeeded; PC file hash intact |
| C2: PC copies 40 MB, WiFi upload 2 s later | upload refused after the 15 s wait (503 "busy on the USB side"); PC copy finished, hash intact; no forced takeover |
| D: WiFi delete while PC has the drive | OK; listing rescanned; PC view refreshed after hand-back |
| F: download via WiFi | borrow → send → release; 10 MB hash OK at ~1 MB/s |
| H: hand-back latency seen by the PC | soft (media change): file visible ~24 s after the upload ended (hand-back at +5 s, Windows re-mount ~19 s). Hard (USB re-enumeration): ~15 s. Default stays soft; switch in Settings |
| OTA | `POST /ota` 1.56 MB: 19 s (slot erase up front), reboot, other slot boots, status shows partition + ELF hash. Now the normal update path (`tools/ota.py`) |
| Heap | 7.01 MB free before and after the matrix; no leak visible |

Not covered yet: host "Eject" (the Shell eject verb did not dismount the
drive in the test harness), power-pull mid-upload, BOOT short-press edit mode
(needs a finger), 24 h soak.

Bugs found and fixed on the way: listing stopped after the first row (an empty
chunk ends an esp_http_server response); NVS settings loaded before NVS init;
forced takeover after the borrow timeout is now opt-in (`WD_BORROW_FORCE`),
default is to fail the WiFi operation and leave the host alone.

Known limits: WiFi ~1.2 MB/s up / ~1 MB/s down on 1-bit SD (see Milestone B);
the "reboot to download mode" shortcut (BOOT 3 s, `/api/bootloader`) lands in
the ROM's USB-OTG mode, which Windows enumerates unreliably here. Use OTA; the
button route is the fallback.
- OTA erase strategy measured: up-front erase 19 s for 1.56 MB; sequential
  (erase-as-you-go) 34 s. Kept up-front. The `tools/ota.py` tool exits 2 when
  the ELF hash did not change (same image pushed), which is informational.

**Milestone C exit criteria: met** (files dropped on the web page appear on the
PC after the hand-back; PC-side files appear in the listing after a refresh;
no card corruption in the matrix). The board now runs `c-wifi-drive`.
Next: Milestone D (LCD status, button polish), the untested cases above, the
WiFi/SD throughput work (iperf, 4-bit SD), and a soak test.

### 2026-10-21 — Milestone D (LCD) first bring-up: memory lesson

- First LCD build (LVGL 9 + esp_lvgl_port) rebooted in a loop before USB came
  up: `start_file_server` hit `ESP_ERR_NO_MEM` and an `ESP_ERROR_CHECK` turned
  it into an abort. Cause: internal RAM. LVGL's built-in allocator keeps a
  64 KB static pool in .bss, on top of the throughput profile's IRAM-resident
  WiFi/LWIP code; free internal heap at boot fell from ~320 KB to ~153 KB and
  the file server's two 32 KB DMA buffers no longer fit.
- Fixes: `CONFIG_LV_USE_CLIB_MALLOC=y` (LVGL uses the system heap; large
  blocks land in PSRAM), HTTP read buffer 32 → 16 KB, LCD draw buffer 40 → 20
  lines, LVGL task stack 12 KB, screen init moved to the end of boot, the file
  server start made non-fatal, OTA rollback enabled (bootloader + app;
  `health_task` marks the image valid 20 s after boot). DIRAM use 242 → 177 KB.
- Recovery note: when the app crash-loops before TinyUSB starts, the ROM's
  USB-JTAG port stays visible and `esptool --before default_reset` flashes and
  reboots the board with no button at all (and no stuck-in-download quirk:
  that only happens when download mode was entered with the BOOT button).

### 2026-10-21 — Milestone D done: LCD status screen verified

- `main/ui.c`: ESP-IDF `esp_lcd` ST7789 driver (SPI2, 40 MHz, BGR, inverted,
  x-gap 34) + LVGL 9 via `esp_lvgl_port`, portrait 172×320 with the USB plug at
  the bottom (`CONFIG_WD_LCD_ROTATE_180` flips it; default off is correct on
  this unit). Backlight on GPIO48 via LEDC PWM (`CONFIG_WD_LCD_BRIGHTNESS`).
- Layout for reading at ~2 m: one 48 px status word in a colour-coded block
  (USB green, WIFI/EDIT blue, WAIT amber, NO SD red), an activity strip that
  flashes green/orange on host reads/writes and blue while the WiFi side owns
  the card, then IP (24 px), hostname (20 px), free space (24 px), usage bar.
  Colours match the WS2812 LED.
- Verified on the board: upright, green USB block, IP and free space correct.
- Button: short press toggles sticky edit mode (block turns blue EDIT), 3 s
  hold restarts into ROM download mode (fallback flashing path).
- Memory budget after the fixes: DIRAM 177 KB used of 342 KB; ~165 KB free
  heap at boot for WiFi, HTTP, TinyUSB, LVGL.

**Milestone D exit criteria: met.** Still open from the plan: host-eject
behaviour, power-pull mid-upload, 24 h soak, WiFi/SD throughput work (iperf,
4-bit SD after bridging R11/R18), and the machine PATH stray quote on the PC.

### 2026-10-21 — Open items: eject awareness, rollback proven, eject test

- Firmware now notices a host eject: TinyUSB's optional
  `tud_msc_scsi_complete_cb` (START STOP UNIT with LoEj) sets a flag; a USB
  attach/configure (`TINYUSB_EVENT_ATTACHED`) or a load clears it. While
  ejected, WiFi borrows skip the USB-idle wait, the screen shows a grey
  EJECT block, and `/api/status` has `"ejected"`.
- OTA rollback verified end to end: after an OTA the status shows
  `ota_state: pending-verify`, and `valid` once `health_task` marks the image
  20 s later. `ota_state` is now in the status JSON.
- Windows eject test (lock, dismount, `IOCTL_STORAGE_EJECT_MEDIA` from
  PowerShell; the Shell "Eject" verb does nothing for this device): firmware
  flagged ejected; a 3 MB WiFi upload went straight through at 1.7 MB/s;
  the drive stays away after the hand-back (Windows does not re-probe an
  ejected device) until `/reset_msc` (or hard handoff) re-enumerates it,
  after which the file was visible in ~10 s and the flag cleared.
- Reboot-mid-upload test (power-pull proxy): not achievable from the PC side.
  The HTTP server is single-threaded, so `/api/reboot` (and the status bar,
  listing, everything else) waits until a running upload finishes; the reboot
  then hit a complete file (40 MB intact, card fine afterwards). A real power
  pull mid-write is still untested; FatFS will at worst leave a truncated file
  and lost clusters (chkdsk can tidy), the card stays mountable.
  **Known limitation / future work:** move uploads to ESP-IDF's async request
  handlers so status, listing and control stay responsive during a transfer.
- Subdirectory test: folder + file created on the PC, `GET /soaktest/` listed
  the file after a brief borrow, WiFi delete inside the folder worked, PC
  removed the folder.
- Download timings right after an upload include the wait for USB to go idle
  (the drive has just been handed back and Windows is reading it), so they can
  read 0.3-0.6 MB/s in the tool; raw download throughput is ~1 MB/s.
- Soak (25 min, 5 MB upload + download-verify + delete every 90 s, PC write +
  read on E: every third cycle): 15 cycles, 0 failures, 0 reboots, every hash
  OK, heap constant at 6,971,155 B after the first cycle, uploads 0.9–1.5 MB/s.

### 2026-10-21 — A file went missing; findings and hardening

- After the day's tests the card held CFR Base…gcode, test.gcode, HANDOFF.TXT;
  **"UltraLightGridfinityBins 3x3 - 4x4_PLA_3h27m.gcode" (11,242,011 B) is
  gone.** Last seen in a listing at 15:40 (LCD build first boot); absent by
  20:29. In between: ~4.7 h idle with the drive mounted on the PC, the OTA to
  the eject-aware build (20:25), the Windows eject test (20:27).
- Filesystem state: Windows used-space figure equals the remaining files
  exactly; both gcode files read back fully with matching hashes over USB and
  WiFi (they are identical copies of each other). So the FAT is consistent and
  the file's clusters were freed: a clean delete, not corruption. No command
  of mine named that file (all WiFi deletes targeted wifitest_*/soak_*/
  pcbusy*/test_NNN_* names; the only PC-side delete was a test folder).
  Cause unknown; asked the user whether they removed it themselves.
- Hardening done in response: (1) **hard handoff is now the default** (the
  USB device re-enumerates after each hand-back, so the host mounts the volume
  fresh instead of re-verifying a cached one); (2) `/api/events`: a ring of
  the last 48 firmware events (boot + reset reason, borrows/hand-backs,
  uploads, deletes, ejects, web/button actions, OTA) so the next "who touched
  the card when" has an answer; (3) free space: FatFS no longer trusts the
  FSInfo hint (it reported used = 0), recounted at boot and on Refresh only.
- Lesson for testing: use a scratch card, not the user's working card.
- Resolved: the user removed "UltraLightGridfinityBins…gcode" themselves. Not
  a firmware issue. The hardening above (hard handoff default, event log,
  free-space recount) stays, since it makes such questions answerable.

### 2026-10-22 — WiFi provisioning: setup network, QR code, settings form

- Question answered: how to give the drive WiFi credentials. Three paths:
  the Settings page (STA/AP/APSTA form, now POSTed so any characters work,
  SSID prefilled from `/api/status`), the setup access point, and
  `tools/set-wifi.py` (same form from the command line, `--from-secrets`).
- Setup access point (`app_wifi.c start_fallback_ap`): `WiFi-Drive-XXXX`
  from the softAP MAC (this board: WiFi-Drive-DA99), WPA2 password
  `CONFIG_WD_FALLBACK_AP_PASSWORD` (`wifidrive`), 192.168.4.1 with DHCP.
  Comes up after EXAMPLE_ESP_MAXIMUM_RETRY (10) failed joins or at boot with
  nothing configured; runs as APSTA next to the station and retries the home
  network every 30 s (the retry is skipped while the station is connected).
  Can be started/stopped by hand: `POST /api/wifi?ap=1|0`, or the new
  "Setup network" control on the Settings page. Stays up until restart.
- Screen: while the setup network is up the lower half shows a 128 px QR code
  (`WIFI:T:WPA;S:<ssid>;P:<pw>;;`, lv_qrcode; the 42-byte default string is a
  version-3 code, 29 modules at 4 px, 6 px quiet zone, white border), the
  network's name in yellow (Montserrat 20, ~160 px wide) and `192.168.4.1`.
  Details (IP, mDNS name, space, bar) are hidden meanwhile; pill and strip stay.
- Verified from the PC (RZ616 adapter) on 2026-10-22: scan lists
  WiFi-Drive-DA99 as WPA2-Personal/CCMP, 2.4 GHz channel 1, 82%; joined on
  the first attempt, PC got 192.168.4.2; `/api/status`, `/settings.html` and
  the file list all served at 192.168.4.1 while the drive stayed on <home-ssid>
  (<drive-ip>) and the card stayed with USB. `ap=0` returned it to plain STA.
  Credentials path: `set-wifi.py --from-secrets` → HTTP 200, restart, rejoined.
- False alarm on the way: the first attempts showed an empty
  `netsh wlan show networks` and connect error 0x80342002. That is the WLAN
  "power state invalid" error: the PC's WiFi radio was software-off (Ethernet
  PC). Windows also only lists the last scan; force one with `WlanScan`
  (wlanapi.dll via Add-Type) and wait ~5 s before reading the list.
- Seen by the user on 2026-10-22: the QR code shows on the screen with the
  network's name under it, and their phone joined the setup network from it.
  Provisioning works end to end (screen, phone, web form, command line).
- Follow-up fix the same evening: after the setup network had been stopped,
  `/api/status` kept reporting its name as `ap_ssid`, so the Settings page
  prefilled the WiFi form as AP+STA with "WiFi-Drive-DA99" and an empty
  password; a Submit would have saved an open access point by that name.
  Now `ap_ssid` is the running access point's name only (the user's own AP,
  the setup network while it is up, else empty), `setup_ssid` carries the
  fixed WiFi-Drive-XXXX name (the Settings page shows it), and the setup
  network never replaces a user-configured access point. The Settings page
  gained a "Setup network" start/stop control (verified in the browser: state
  line and button label follow `/api/status`).

### 2026-10-22 — Captive portal on the setup network
- Goal: joining WiFi-Drive-XXXX opens the OS "sign in to network" pop-up with
  the Settings page, so nobody has to type 192.168.4.1.
- Built on ESP-IDF's `examples/protocols/http_server/captive_portal`, in
  `main/captive_portal.c` (+ `.h`), running only while the setup network is up:
  - DNS server on UDP 53 bound to the AP address only (192.168.4.1), answering
    every A query with that address (TTL 10 s) and any other type with an empty
    NOERROR. Rewritten rather than copied: the example sets ANCOUNT for skipped
    non-A questions (malformed replies for AAAA) and puts the answer after any
    EDNS record. Task stack 3 KB internal, 528-byte buffer in PSRAM, 1 s receive
    timeout so `captive_portal_stop()` can end it cleanly (waits up to 3 s).
  - DHCP option 114 (RFC 8910) = `http://192.168.4.1/settings.html` via
    `esp_netif_dhcps_option(..., ESP_NETIF_CAPTIVEPORTAL_URI, ...)`; the option
    keeps the pointer, so the string is static. No dhcps stop/start needed: the
    option is read when each reply is built. DNS = 192.168.4.1 is already
    offered by `CONFIG_LWIP_DHCPS_ADD_DNS`.
  - Redirects: `captive_portal_redirect()` runs first in the catch-all GET
    handler (`/*`) instead of eight extra URI handlers (still 14 of 16). On the
    AP side only (socket's local address is the AP address, IPv4-mapped on the
    dual-stack listener), it sends `302 Found` + `Location: .../settings.html`
    + a short body (iOS wants one) for `/generate_204`, `/gen_204`,
    `/hotspot-detect.html`, `/library/test/success.html`, `/connecttest.txt`,
    `/ncsi.txt`, `/canonical.html`, `/success.txt`, and for any request whose
    Host is neither 192.168.4.1 nor `*.local`. The station side never redirects.
  - Hooks: `start_fallback_ap()` (runtime) and the end of `app_wifi_main()`
    (setup network at boot) start it; `app_wifi_fallback_ap_set(false)` stops it.
- Verified 2026-10-22 (OTA to ota_0, elf 5cf3e7ad, `ota_state` valid): from the
  PC joined to WiFi-Drive-DA99 (got 192.168.4.2), `nslookup example.com
  192.168.4.1` → 192.168.4.1; generate_204, hotspot-detect.html,
  connecttest.txt, success.txt and `Host: example.com` all → 302 to the
  settings page; settings page and file list at 192.168.4.1 still 200. Home
  network side with the setup network up: `/generate_204` 404, foreign Host
  header served normally, `wifidrive.local/` 200, upload/download/delete of a
  test file 200.
- Seen by the user on 2026-10-22: their phone joined WiFi-Drive-DA99 and got the
  "sign in to network" pop-up showing the Settings page. Captive portal done.
