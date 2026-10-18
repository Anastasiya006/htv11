# Gemmi handoff

Tamagotchi-style ESP32 panel (240x320 touch) that supervises multiple software
agents: shows their activity, asks questions on-device, displays images they
push, and lets the human draw doodles back to an agent.

## Layout

- `gemware/gemware/main/gemmi_ui.c` — all UI: sprite, nav, asks, images, shop, agents, draw
- `gemware/gemware/main/gemmi_bridge.c` — poll task, HTTP client, event parse, image fetch
- `gemware/gemware/main/gemmi_anims.c` — generated animation table (14 live anims)
- `gemware/gemware/main/gemmi_pet.{c,h}` — Tamagotchi stats (XP/gems/level, NVS)
- `gemware/bridge/gemmi_bridge.py` — host bridge + `gemmi` CLI (`./gemware/bin/gemmi`)
- `gemware/bridge/gemmi_musings.py` — offline Gemini personality worker (402 credits out)
- Store: `/tmp/gemmi-store.jsonl`; token: `~/.local/state/gemmi/bridge-token`

## Event flow

agent → POST /v1/events → device polls GET /v1/device/next?device_id=gemmi-esp32&after=N
→ show_event → ack (or holds unacked while foregrounded) → respond for asks.

- **Blocking queue**: `gemmi_ui_is_blocked()` (ask_foreground || image_pending || draw_mode)
  pauses the poll, so asks/images/drawing are sequential. Unacked items resurface
  on reboot; dismissed asks resurface after ~60s.
- **Watchdog**: ~30s stuck blocked logs `poll blocked ~Ns on <reason>` with the flag.
- **device_id is `gemmi-esp32`** (not `gemmi`) — consumption checks must use it.
- **Keep-alive off + 3s timeout + reset-and-retry once** on dead sockets.
- **Image fetch uses the shared client** with `collecting_image` diversion — a second
  TLS connection through the funnel gets refused (`ESP_ERR_HTTP_CONNECT`).

## CLI

```
gemmi notify --agent X --state working "msg" [--wait SECS]
gemmi log --agent X "tool: ..." [--wait SECS]
gemmi ask --agent X --state needs-attention --choice A --choice B "q?" [--wait SECS]
gemmi artifact --agent X --artifact name --file img.png [--wait SECS]
gemmi drawing --out out.png          # newest image/doodle → PNG
gemmi events | wait | clear | serve | sim | tunnel
```

`--wait` holds until the device's cursor passes the event (consumption) —
pace delivery so the ESP32 only holds one item. Asks wait for the answer.

## Firmware notes

- **Sprite modes**: 0 big centered (home + ask), 1 small corner peek (image view),
  2 detail (96px top-left), 3 shop, 4 top-anchored (unused now).
- **Ask view**: `ask_panel` — white bordered box like the home bubble, "GEMMI"
  notched in the top border, question + row-by-row choices inside, content-sized,
  grows up from the bottom. Single-tap a row sends. `back` row dismisses/snoozes.
- **Images**: host renders RGB565 (≤96x72, 4-byte w/h header). `image_raw[14000]`
  static buffer (heap is fragmented — big mallocs fail). Display ~1.7x via
  `lv_image_set_scale`. Stays until `okay`. Doodles (`artifact=="doodle"`) skip
  the image view and auto-ack.
- **Draw**: `draw` button in agent detail → white pad (12,60 216x162 in scene),
  strokes as `lv_line` + rasterized 72x54 RGB565 with segment interpolation,
  send → POST /v1/drawing (JSON body malloc'd at send only — TLS needs heap).
- **Anim cycles**: `play_state_cycle` — working: loading→look-left→look-right;
  thinking: thinking→reading; detail view monitor: same cycle unconditional.
  Idle 45s → sleeping. play_state kills cycles+sleep timers.
- **Text**: uniform 0x444444; pressed label blue 0x4A8AF4 on black bg
  (`on_press_style`). Status text+marker diamond share a state color
  (purple input/viewing, blue activity, green success, red offline).
- **Nav**: two dock buttons `agents` + `Shop`; gem badge `Lv.N / count` + black
  rhombus (10x10 RGB565 image).
- **Fonts are ASCII-only** — parse_event replaces bytes ≥0x80 with `-`.

## Gotchas

- TLS needs ~40KB heap contiguous — keep big buffers static-but-small or malloc
  briefly; `-0x008D` = SSL_ALLOC_FAILED means you ate the heap.
- `idf.py build flash` in that shell breaks Pillow's site-packages — run the
  `gemmi` CLI from a clean shell/env.
- LVGL here has no text-grad API; press feedback is solid blue.
- Monitor: `idf.py -p /dev/ttyUSB0 monitor > /tmp/gemmi-serial.log &`
- Device is on a phone hotspot (eduroam WPA2-Enterprise was the earlier flap —
  `CONFIG_GEMMI_WIFI_ENTERPRISE=n` + `CONFIG_ESP_WIFI_SSID/PASSWORD` in sdkconfig).

## Known rough edges

- Poll drains one event per ~4s cycle — big backlogs take a while.
- Serial forwarder (UART bridge) was designed but not built.
- Shop `open_pet_room` still refuses while `ask_pending` — cleared asks needed.
