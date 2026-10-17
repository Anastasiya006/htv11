# Gemmi visual design reference

Start with [the contact sheet](contact-sheet.jpg), or open [the browsable gallery](index.html).
This folder documents the **native C/LVGL emulator**, captured on 2026-10-17
from commit `8ef1857`. It is the current design reference, rather than the older
HTML simulator. The screenshots and videos contain local fixture interactions,
not live Gemini CLI output.

## Screenshots and recordings

- [Screenshots](screenshots/): 41 full-resolution PNGs, numbered in viewing order.
- [Videos](videos/): 11 H.264 MP4 recordings with actual native animation and input.
- [Manifest](manifest.json): starting scene, timing, clicks/keys, save overrides,
  screenshot names, and answer output for each recording.

| Recording | What it shows |
| --- | --- |
| [01 — Fresh pet](videos/01-fresh-pet.mp4) | Fresh main screen, tap and hold reactions |
| [02 — Task activity](videos/02-task-activity.mp4) | Thinking, working, completion reward |
| [03 — Multiple agents](videos/03-multiple-agents.mp4) | Agent list, individual logs, next agent, Back |
| [04 — Two-choice question](videos/04-two-choice-question.mp4) | Selection, second-tap confirmation, answer, empty Asks |
| [05 — Back and reopen](videos/05-question-back-and-reopen.mp4) | Three choices, explicit Back, pending question, reopening and answering |
| [06 — Artifacts and View](videos/06-artifact-and-view.mp4) | Empty View, artifact notification/reference, Back |
| [07 — Starter shop](videos/07-starter-shop.mp4) | 20 starter gems, insufficient funds, glasses purchase and wear |
| [08 — Shop catalog](videos/08-shop-themes-and-bow.mp4) | Both full shelves, theme changes, wearable bow |
| [09 — Crown](videos/09-crown-and-remove.mp4) | Buying, wearing, removing an owned cosmetic |
| [10 — Level-up](videos/10-level-up.mp4) | Jump celebration, reward text, updated level/gems |
| [11 — Connectivity](videos/11-connectivity.mp4) | Disconnected and reconnected states |

Videos are portrait 480 × 640 at 20 fps, without audio. PNGs are the direct SDL
Retina output, 960 × 1280. Both represent the same logical **240 × 320** layout.
No desktop, terminals, credentials, or other applications are recorded.

## Design constraints for future agents

- Keep the main screen sparse. Gemmi is the focus; a short console sits below.
- The top bar has the current action on the left, level/gems and a small bag on
  the right, with balanced side margins. Do not replace the action with a fixed
  demo label.
- Use smooth monospace text, thin grey square frames, and compact outlined
  navigation buttons. The emulator renders macOS Menlo at Retina resolution;
  the physical device uses its configured fallback fonts.
- No separate pet stats/care pages, or mood/energy/XP meters under Gemmi. Those
  designs were removed. Click Gemmi to pet it; hold to rest.
- The bag opens a small illustrated shop tray over the main screen. Each shelf
  has four equally sized cards; **>** changes shelf and **x** closes it.
- Shelf 1: Specs, Crown, Mint, Snack. Shelf 2: Rose, Sky, Dusk, Bow. Purchased
  cosmetics stay in the shop with Owned/On status and can be switched for free.
  One cosmetic is equipped at a time.
- Asks and View must have Back controls even when empty. Three-choice questions
  have a separate Back control above the character. Back keeps a question
  pending and sends no answer. Choices require two taps: select, then submit.
- Agent questions take priority over shopping. Keep the existing agent list,
  per-agent logs, artifact flow, and disconnected state accessible.

## Progress and fixture boundaries

The real local pet was reset to **level 1, 0 XP, 20 gems, no purchases**. Source
and committed changes were preserved. Every recording uses its own temporary
save, so purchases/rewards in these videos do not affect that reset.

Shop catalog and crown clips explicitly start with 1,000 test gems to show all
items. The level-up clip uses the emulator's level-up fixture. Other clips
start fresh. A completed task earns 25 XP and 5 gems; every 100 XP grants a level
and another 10 gems. Test funding is not the normal starting balance.

The native artifact view currently displays an artifact **reference/path**, not
an image preview. The recording shows this existing limitation accurately.
Likewise, the emulator's connectivity fixture changes its status; these clips
do not demonstrate a live bridge or CLI connection.

## Reproduce

From the repository root, with SDL2, CMake, Swift/Xcode command-line tools, Python 3, and Pillow installed:

```sh
cmake -S emulator -B emulator/build
cmake --build emulator/build --parallel 8
python3 docs/design-context/capture.py
```

The capture script compiles a temporary launcher with `capture_driver.inc` and
links the normal firmware UI, animations, pet model, LVGL, and font build
objects. It captures SDL output directly, streams it to the AVFoundation H.264 encoder in `encode.swift`, and injects
scheduled native mouse/key events. It does not edit product UI source or use
macOS screen-recording permission. The capture setup currently expects a Mac
Retina output of 960 × 1280. Temporary saves, raw frames, and the temporary
launcher are not kept in this folder. Re-running replaces the media files.

## Capture verification

All 11 MP4s were checked with AVFoundation for 480 × 640 dimensions, 20 fps,
expected duration, and actual frame decoding. See [validation.json](validation.json).
All 41 PNGs were checked for dimensions and nonblank content. The hold sequence
shows the native pet reaction; it should not be used as proof that a distinct
rest animation or care stat change occurred.
