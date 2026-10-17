# Gemware — ESP32 Tamagotchi

An ESP32-WROOM-32 (FCC 2AC7Z devboard) running a little digital pet on a 2.8"
240x320 SPI TFT (ILI9341 + XPT2046 resistive touch), built with ESP-IDF and
LVGL 9.6.

Behavior: Gemmi idles on a white background. Tapping the screen plays a random
reaction animation (one of 22), then returns to idle. Animations are raw GIFs
loaded from the workspace's `../animations/` directory, decoded at runtime by LVGL's
`lv_gif` widget and scaled 2x (96 -> 192 px).

## Contents

- `gemware/` — the ESP-IDF app
  - `main/lvgl_demo_ui.c` — idle loop + tap-to-react logic
  - `main/gemmi_anims.{c,h}` — all 23 animations as embedded GIF data (generated)
  - `main/lcd_display.c` — ILI9341/XPT2046 driver, pin map, BGR color order fix
  - `main/gemware_main.c` — app entry (Wi-Fi station + display init)
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
