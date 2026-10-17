# Gemware — ESP32 Tamagotchi

An ESP32-WROOM-32 (FCC 2AC7Z devboard) running a little digital pet on a 2.8"
240x320 SPI TFT (ILI9341 + XPT2046 resistive touch), built with ESP-IDF and
LVGL 9.6.

Behavior: Gemmi idles on a white background. Tapping the screen plays a random
reaction animation (one of 22), then returns to idle. Animations are raw GIFs
loaded from the workspace's `../animations/` directory, decoded at runtime by LVGL's
`lv_gif` widget and scaled 2x (96 -> 192 px).

## Computer + agent bridge

Gemmi now has a small local bridge for agent status and questions. Start it on
the computer first:

```bash
./bin/gemmi serve
```

Set **Gemmi bridge → Computer-side Gemmi bridge URL** in `idf.py menuconfig` to
the computer's LAN address shown by the bridge instructions (for example,
`http://192.168.1.42:8732`), then build and flash. It cannot use `localhost`:
on the ESP32 that would mean the device itself.

Any agent can then announce a meaningful state change:

```bash
./bin/gemmi notify --agent codex --state working "Reading the firmware"
./bin/gemmi ask --agent codex --state needs-attention \
  --choice "Allow" --choice "Deny" "Flash the ESP32?"
```

See [`bridge/README.md`](bridge/README.md) for the event contract and why this
is an explicit wrapper before ACP/MCP integration.

## Connecting to eduroam

The credentials belong in the ESP-IDF local configuration, which is ignored by
Git. From `gemware/gemware/` run:

```bash
source /home/hiatus/Projects/esp-programming/esp-idf/export.sh
idf.py menuconfig
```

Set the following fields, then choose **Save** and **Exit**:

1. **Example Configuration → WiFi SSID**: `eduroam`.
2. **Gemmi Wi-Fi → Use WPA2-Enterprise (eduroam)**: enable it.
3. In the same **Gemmi Wi-Fi** menu, select the EAP method and enter the outer
   identity, inner username, and EAP password given by your institution.

PEAP (usually MSCHAPv2) is the default because it is common, but follow your
institution's exact eduroam documentation. If it provides a RADIUS certificate
domain, enter it in **Server certificate domain**; this pins the expected server
as an additional verification step. The old **WiFi Password** field is unused
when enterprise Wi-Fi is enabled.

Build and flash after saving:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

`sdkconfig` contains the credentials and is gitignored. Do not paste it into
chat or commit it.

## Contents

- `gemware/` — the ESP-IDF app
  - `main/gemmi_ui.c` — Gemmi's device UI, animations, and touch actions
  - `main/gemmi_anims.{c,h}` — all 23 animations as embedded GIF data (generated)
  - `main/gemmi_display.c` — ILI9341/XPT2046 driver, pin map, and LVGL port
  - `main/gemmi_wifi.c` — station and enterprise Wi-Fi connection
  - `main/app_main.c` — minimal application entry point and service startup
  - `sdkconfig` / `sdkconfig.defaults` — enables LV_USE_GIF, CLIB malloc
- `tools/gifs_to_carray.py` — regenerates `gemmi_anims.c` from `../animations/`
- `tools/gif_to_lvgl.py` — older single-animation RGB565 converter (kept for reference)

## Quick start (Arch Linux + fish)

```bash
sudo pacman -S --needed git cmake ninja python python-pip python-virtualenv \
    gcc make flex bison gperf ccache dfu-util libusb
sudo usermod -aG uucp,dialout $USER   # then log out/in
git clone --recursive https://github.com/espressif/esp-idf.git ~/esp-idf
~/esp-idf/install.sh esp32
```

```fish
source ~/esp-idf/export.fish
cd gemware
idf.py set-target esp32
idf.py menuconfig   # Partition Table -> Single factory app (large), no OTA
idf.py -p /dev/ttyUSB0 flash monitor   # Ctrl+] exits monitor
```

## Regenerating animations

```fish
python3 tools/gifs_to_carray.py 180   # rotation arg: 0/90/180/270
```

Reads all `*.gif` files from `../animations/` (relative to this project), with
`idle.gif` as the default state. Output goes to `gemware/main/gemmi_anims.{c,h}`;
rebuild after regenerating.
