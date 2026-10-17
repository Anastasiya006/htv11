# Gemware: tiny agent companion

Gemmi is a Tamagotchi-like companion for supervising several agents, not a
miniature desktop dashboard. It should make the important moment obvious:
which agent needs a human, what it wants, and what happened after the answer.

## Interactive mockup

Open [`mockup/index.html`](mockup/index.html) in a browser. It is deliberately
drawn at the device's 240 × 320 portrait size and uses the real Gemmi GIFs to
test the intended feel before firmware work begins.

## Product constraints agreed so far

- Gemmi is a shared human-facing inbox for multiple agents: command approvals,
  short questions/multiple choice, work status, and agent-produced screenshots.
- The screen is tiny and visually limited, so it should show one thing that
  matters now—not a dense feed, terminal, or dashboard.
- The hardware affordance budget is small: at most two controls in the top
  corners, plus one bottom row of three actions. A second row of three is an
  exception, not the default.
- The companion is the interface. Its expression and motion communicate state;
  labels are brief and high-contrast.
- Action animation must feel fluid. Keep controls and text in a stable region,
  redraw only the character/scene region, and use brief interruptible motions
  so a new urgent question can take over immediately.
- Screenshots should not be squeezed into a bad thumbnail. Gemmi should signal
  that an artifact arrived and offer a deliberately simple preview or handoff
  to a phone/local web page.

## Working interaction shape

- **Top-left:** agent roster / return to the calm home state.
- **Top-right:** urgent inbox / next item needing a decision; it can pulse when
  attention is required.
- **Bottom row:** context actions for the current card. The three slots become
  concise answer choices during a question or approval, rather than permanent
  navigation tabs.
- **No attention needed:** show the character and a tiny status line. Do not
  fill the screen merely because space exists.

The exact labels and priority order still need validation against the common
Codex CLI prompts; this is the next design decision.

## First: validate the interaction model

- [x] Establish the visual direction with a local interactive HTML mockup.
- [x] Constrain the interaction model to two top-corner controls and one 3-button bottom row.
- [ ] Verify Codex CLI's frequent human inputs and finalize the three contextual action labels.
- [ ] Decide which information deserves the tiny 240 × 320 screen.
- [ ] Map every attention state to a short, interruptible Gemmi animation sequence; keep the static controls out of the animated redraw region.
- [ ] Confirm the approval flow, multiple-choice flow, and artifact preview flow.

## Then: make the agent bridge dependable

- [x] Define the first versioned event envelope: an ID, timestamp, agent, kind, state, message, up to three choices, and optional artifact reference.
- [x] Run a local, durable JSONL-backed broker and ESP32 polling client for the first end-to-end connection.
- [x] Connect Codex/CLI tooling through explicit `gemmi notify` / `gemmi ask` commands first; evaluate native tool integration only after the protocol is proven.
- [x] Persist unanswered prompts and decisions so restarts never lose a request or submit an answer twice: per-device consumed sets plus recorded responses live in the same JSONL store, and open asks resurface after a device reboot.
- [x] Software-first testing: `gemware/bridge/test_bridge.py` (14 HTTP-level tests) and `gemmi sim` run the full ask/answer loop with no ESP32 attached.
- [x] Device ask flow on hardware: real choices on the dock, press-to-select then press-to-send, answer POSTs retried, `ask --wait` verified end-to-end.
- [ ] Add agent heartbeats, queue health, backpressure, and a safe offline/USB-disconnected mode.

### Current bridge status

- The computer-side bridge is a local `tmux` service on port `8732`; agents use
  `./gemware/bin/gemmi` to post meaningful status, question, or artifact events.
- `gemmi ask --wait <seconds>` blocks until the device answers and prints the
  chosen option; `gemmi clear` empties the queue and per-device state.
- Device protocol: `GET /v1/device/next` returns a compact newline view
  (id, kind, agent, state, choices, artifact, message). The device acks each
  displayed event (`POST /v1/device/ack`) and answers asks through
  `POST /v1/device/respond`.
- Gemware reaches the bridge through a token-gated Tailscale Funnel
  (`gemmi tunnel init/on/off`); `gemmi tunnel off` also locks down local access.

## Artifacts and hardware

- [ ] Store screenshots as durable artifacts, deliver a thumbnail/metadata event to Gemmi, and show a touch-to-open preview or QR/local-web handoff.
- [ ] Define the broker-to-ESP32 transport, reconnect semantics, and a compact display protocol.
- [ ] Stress-test with several concurrent agents, rapid prompt bursts, reconnects, and full queues.
