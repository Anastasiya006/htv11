# Firmware UI emulator

This is a native SDL desktop window that compiles and runs the firmware's
`gemmi_ui.c` and `gemmi_anims.c` with the exact LVGL version resolved for the
ESP-IDF project. It replaces only the ILI9341/touch driver and the HTTP bridge,
so rendering and touch interaction are not reimplemented in the browser.

## Run

```sh
cmake -S emulator -B emulator/build
cmake --build emulator/build
./emulator/build/gemmi-emulator
```

The window is 240 x 320 logical pixels at 2x desktop scale. Click it exactly
as on the device. The default fixture seeds two agents; click **agents** and a
row to exercise the shared list/detail UI.

Use a focused window's hotkeys to switch fixture events while iterating:

- `1` thinking
- `2` working
- `3` ask (click a choice twice to submit)
- `4` artifact
- `5` seed agent activity
- `O` offline; `I` online

An initial scene can also be selected without rebuilding:

```sh
./emulator/build/gemmi-emulator --scene ask
```

Available scenes are `home`, `thinking`, `working`, `ask`, `artifact`,
`agents`, and `offline`. Choice submissions are printed to stdout; the desktop
emulator does not post to a real Gemmi bridge.

Build output stays under `emulator/build/` and is ignored by Git.
