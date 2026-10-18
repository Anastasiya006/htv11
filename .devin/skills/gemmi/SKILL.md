---
name: gemmi
description: Report agent activity to the Gemmi physical device (ESP32 panel) via the local bridge. Use when an agent should show status, stream activity lines, ask yes/no or multiple-choice questions, share artifacts/images, or receive doodles from the human watching Gemmi.
---

# Gemmi

Gemmi is a physical Tamagotchi-style panel supervising agents. The `gemmi` CLI
talks to a local bridge (`http://127.0.0.1:8732`, auth token from
`$GEMMI_TOKEN` or `bridge/.env`) which queues events for the device.

CLI: `./gemware/bin/gemmi` (repo root: `/home/hiatus/Projects/htv11`).

## Commands

```bash
gemmi notify --agent NAME --state STATE "short message"   # status event
gemmi log    --agent NAME "tool: pytest -x"               # activity line (feeds agents view)
gemmi ask    --agent NAME --state needs-attention \
             --choice "Allow" --choice "Deny" "question"  # up to 3 choices
gemmi artifact --agent NAME --artifact NAME "desc" --file pic.png
gemmi drawing --out doodle.png                            # newest device doodle → PNG
gemmi events                                            # queue + open asks
gemmi wait   EVENT_ID                                     # collect a posted ask's answer
gemmi clear                                             # wipe the queue (dev only)
gemmi audio-test --sound CUE                              # play a chime on the host
gemmi sim    --auto                                       # software device (see Testing)
```

Every sender takes `--wait SECONDS`: it blocks until the device consumes the
event (asks wait for the answer and print it, images wait for the human's
`okay`). Chain `--wait` calls to pace delivery — the panel holds one item at
a time; fire-and-forget bursts just queue behind whatever is on screen.

## States

`idle` `working` `thinking` `reading` `needs-attention` `success` `done`
`completed` `error` `failed` `level-up`

Posting `success`/`done`/`completed` after the agent has been active earns the
pet gems/XP — report real terminal states, not noise. Log lines prefixed
`tool:` `think:` `warn:` `err:` render color-coded in the per-agent feed.

## Media

- `artifact --file img.png` — the host converts to RGB565; the panel shows it
  fullscreen-ish until the human taps `okay`. It blocks the queue until then.
- `drawing` — the human can doodle on the panel (agent detail → `draw`) and
  send it; it lands as an `artifact` event (`artifact: "doodle"`, agent
  `gemmi`). `gemmi drawing` decodes the newest one to a PNG (Pillow required).

## Audio

The bridge plays chimes on the host when served with `--audio chimes`
(speech mode needs `GEMINI_API_KEY`). Cues fire on event POST:

| Event | Cue |
|---|---|
| `kind=ask` or `state=needs-attention` | ask (queue-priority) |
| `state=error`/`failed` | error |
| `kind=artifact` | artifact (`doodle` if a device doodle) |
| `status` + `state=success/done/completed` | success |
| `state=level-up` | level-up jingle (the device posts this itself) |
| `kind=log`, working/thinking statuses | silent |

Device responses (ask answers, image `okay` acks) also play a `respond` blip.
Test any cue: `gemmi audio-test --sound level-up`.

## Testing without hardware

```bash
gemmi serve --port 8732 --store /tmp/gemmi-store.jsonl --audio chimes &
gemmi sim --auto &        # software device, answers every ask with choice 1
gemmi ask --agent me --choice yes --choice no --wait 30 "ping?"   # → "yes"
```

The simulator polls like the real device but uses `device_id=gemmi-sim`, so it
has independent consumption state and can run alongside the real panel
(`gemmi-esp32`). Options: `--device-id NAME`, `--interval SECS`, `--url`,
`--token-file`. Without `--auto` it prints each ask and waits for you to type
`1`-`3`. The suite: `python3 gemware/bridge/test_bridge.py` (bridge behaviour),
`python3 gemware/bridge/test_audio.py` (audio, fake players — no speaker or
key needed).

## Rules

- Keep messages short — the panel is 240px wide, ASCII-only (non-ASCII bytes
  render as `-`).
- Never post secrets, tokens, or private source content.
- If the bridge is unreachable, continue the task; mention the failure only
  if it matters.
- Asks persist on the device until answered; don't repost the same question.
  Unanswered asks and uncleared images survive device reboots — they resurface.
