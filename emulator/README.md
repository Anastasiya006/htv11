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
`agents`, `error`, and `offline`. Choice submissions are printed to stdout; the desktop
emulator does not post to a real Gemmi bridge.

Build output stays under `emulator/build/` and is ignored by Git.

On macOS the emulator rasterizes your installed Menlo font at build time to
match the HTML simulator (`ui-monospace, Menlo, Consolas, monospace`). This needs
Python 3 with Pillow (`python3 -m pip install Pillow`). The generated font stays
in the ignored build directory; the system font is not bundled with the repo.
Other platforms and device firmware keep the built-in font fallback. The Mac
window supports Retina output. Its desktop coordinate adapter renders the same
logical layout at four times the LCD resolution, so text has smooth contours
instead of enlarged bitmap pixels. Sprite scaling stays nearest-neighbor.

## Native UI captures

Capture the SDL renderer directly, without macOS screen-recording permission:

```sh
./emulator/build/gemmi-emulator --scene ask --capture /tmp/gemmi-ask.bmp
./emulator/build/gemmi-emulator --scene ask --tap 40,299 --tap 40,299 --capture /tmp/gemmi-answer.bmp
```

Capture exits after rendering; up to eight `--tap x,y` points use logical 240 × 320 coordinates. The second example exercises the unchanged two-tap choice submission and prints the chosen answer.

## Virtual pet

The main screen keeps only a small level/gems readout and bag icon at the top.
Click Gemmi to pet it, or hold it to rest. The bag opens a compact tray with
illustrated cosmetics and a snack. Use **>** to browse the two shelves, click an
item to buy/equip it, and click **x** to close. Owned items stay available for
free switching. There are no separate stats or care pages; agent questions
immediately close the tray.

- **Progress:** a terminal `status` event (`success`, `done`, or `completed`)
  earns 25 XP and 5 gems. Every 100 XP raises the level and grants 10 bonus
  gems. Repeated event IDs do not earn rewards again. Logs, failures, and
  approvals do not earn XP. Without IDs, a new working/thinking/reading state
  must precede each completion.
- **Care:** pet Gemmi for mood, buy a 3-gem snack for energy, or rest for free.
  Care has short cooldowns. Energy falls a little after completed tasks; being
  away never penalizes the pet, and low energy never blocks agents.
- **Shop:** glasses (15 gems), crown (35), Mint/Rose themes (20), Sky (25),
  Dusk (30), a wearable ribbon bow (25), plus a snack (3). The two shelves
  each show four illustrated items. Bought cosmetics can be equipped or removed for free. One look at a time.
  New pets receive 20 starter gems.

Progress saves locally in `.gemmi-pet-state` (ignored by Git). On the device
it uses NVS instead. Override `GEMMI_PET_SAVE` to keep demo/test saves separate:

```sh
GEMMI_PET_SAVE=/tmp/gemmi-pet-demo ./emulator/build/gemmi-emulator --scene pet
```

`--scene pet` opens the shop tray for layout checks. Press **6** to simulate one completed task; this awards demo XP/gems to the
selected save. `--scene reward` exercises the same fixture on startup. Native
fixtures are local simulations, not a live connection to Gemini CLI.

Test the reward/shop/care model without launching SDL:

```sh
cc -std=c99 -Wall -Wextra -Werror -I gemware/gemware/main \
  emulator/tests/pet_test.c gemware/gemware/main/gemmi_pet.c -o /tmp/gemmi-pet-test
GEMMI_PET_SAVE=/tmp/gemmi-pet-test-save /tmp/gemmi-pet-test
```

Desktop test shortcuts: **7** stages and completes the next level (with a jump
celebration). `--test-gems 10000` sets the selected save's balance to 10,000,
keeping XP and purchases. These controls affect only the local emulator save;
normal task rewards and device firmware have no gem-grant shortcut.
