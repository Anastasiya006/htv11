# Gemmi bridge

The bridge is a local queue between agents on this computer and Gemmi
devices. It has no cloud service and no third-party Python packages. By
default it listens only on `127.0.0.1`, so it is not accidentally exposed to
a LAN.

```bash
./gemware/bin/gemmi serve
```

It listens on port `8732` and stores events durably in
`~/.local/state/gemmi/events.jsonl` (events, per-device consumed marks, join
points, and answers survive restarts). Keep it running while Gemmi is in use.
For a trusted home LAN, pass `--host 0.0.0.0` and configure the ESP32 with the
computer's LAN URL. Do not do this on eduroam or another client-isolated
network.

An agent posts meaningful state changes through the same wrapper:

```bash
./gemware/bin/gemmi notify --agent codex --state working "Reading the firmware"
./gemware/bin/gemmi ask --agent codex --state needs-attention \
  --choice "Allow" --choice "Deny" "Flash the ESP32?"
./gemware/bin/gemmi ask --wait 120 --agent codex --choice "Yes" --choice "No" "Deploy?"
./gemware/bin/gemmi artifact --agent codex --state reading "Screenshot ready"
./gemware/bin/gemmi wait <id>       # block for an answer to an earlier ask
./gemware/bin/gemmi events          # inspect the queue and open asks
./gemware/bin/gemmi clear           # drop queued events and device cursors
```

`ask --wait` blocks until a device answers and prints the choice (exit 2 on
timeout). Testing without a device:

```bash
python3 gemware/bridge/test_bridge.py
./gemware/bin/gemmi sim             # interactive software device
./gemware/bin/gemmi sim --auto      # auto-answers every ask (scripting)
```

## HTTP contract

- `POST /v1/events` — `{kind, agent, state, message, choices[], artifact}`
  → `201` with the full event. `kind` is `status`, `ask`, or `artifact`;
  `choices` is 0–3 short strings.
- `GET /v1/device/next?device_id=<id>&after=<event-id>` — next unconsumed
  event or `204`. With `Accept: text/plain` returns the compact
  newline-framed view firmware parses: `id, kind, agent, state,
  choices(|), artifact, message`. A device the bridge has never seen joins
  at the latest still-open ask instead of replaying history.
- `POST /v1/device/ack` — `{device_id, event_id}` marks an event consumed
  for that device so it is never redelivered. `204`.
- `POST /v1/device/respond` — `{device_id, event_id, choice}` records the
  answer on an ask and consumes it. Same choice twice is idempotent; a
  different answer is rejected. `200` with the event.
- `GET /v1/events/{id}` — the full event, including a recorded `response`.
- `GET /v1/events` — `{events, open_asks}` listing for `gemmi events`.
- `DELETE /v1/events` — clears events, consumed marks, and join points.
  Event ids stay monotonic across restarts so live devices keep working.
- `GET /health` — liveness.

Asks are consumed only when answered or dismissed on the device, so an
unanswered ask resurfaces after a device reboot. Status and artifact events
are consumed as soon as they are displayed.

## Secure public tunnel (eduroam)

Tailscale Funnel is public-internet exposure, so Gemmi never enables it by
default. The secure path is deliberately fail-closed:

1. `./gemware/bin/gemmi tunnel init` creates a 256-bit token in the local
   state directory with owner-only permissions. The token is never printed.
2. Restart `gemmi serve`. It detects the token and requires
   `Authorization: Bearer <token>` on *every* API route, including event
   submission. Agent wrappers read the local token automatically.
3. Configure the same token only in Gemware's ignored `sdkconfig`, enable
   **Gemmi bridge → Require HTTPS and certificate validation** and
   **Require a bridge access token**, then use the HTTPS Funnel URL as the
   bridge URL.
4. Run `./gemware/bin/gemmi tunnel on` to expose only the localhost bridge
   through HTTPS Funnel.

The immediate kill switch is:

```bash
./gemware/bin/gemmi tunnel off
```

It disables Funnel and creates a local lockdown marker; even a mistakenly
still-running proxy receives no bridge data. Re-enabling requires an explicit
`gemmi tunnel on`. Tailscale documents the corresponding Funnel shutdown as
`tailscale funnel --https=443 off`.

The token protects the public endpoint and TLS protects it in transit. The
ESP32 does not yet use flash encryption or secure boot, so physical possession
of the board should be treated as credential compromise until those irreversible
hardware protections are deliberately enabled.

## Laptop / Bluetooth speaker audio

Pair the JBL Go 3 with the **Mac**, then select it under **System Settings →
Sound → Output**. The server uses the Mac's current default output; the ESP32
needs no Bluetooth audio code or additional speaker hardware. This output
selection applies to other Mac applications too.

Start the bridge with local notification sounds (no API key or packages):

```sh
./gemware/bin/gemmi serve --audio chimes
./gemware/bin/gemmi audio-test
./gemware/bin/gemmi audio-test --sound ask
./gemware/bin/gemmi audio-test --sound success
```

New questions, completed tasks, errors, and artifacts trigger distinct retro
virtual-pet bleeps: short, stepped square-wave chirps with soft volume envelopes.
These are synthesized sounds, not recordings from a Tamagotchi.
Routine working/thinking updates and log lines stay silent. Audio has a separate,
bounded priority worker, so playback or speech generation never delays HTTP
responses or ESP32 polling. Duplicate events and rapid informational repeats
are suppressed; old queued cues and questions already answered before playback
are skipped. Existing stored events are not replayed on startup. If the audio
queue is full, only the sound is skipped—the agent event remains in the bridge.

For **Gemini speech**, put `GEMINI_API_KEY=your-key` in the repository root
`.env` (ignored by Git), or export it in your terminal, then start:

```sh
./gemware/bin/gemmi serve --audio speech --voice Puck
./gemware/bin/gemmi audio-test
```

Speech uses Google's `gemini-3.8-flash-tts` REST API, with friendly delivery.
Only fixed notification lines (for example, "Gemmi needs your input!") are
sent to Gemini. Agent names, questions, prompts, commands, and artifact paths
stay local. Speech generation uses the API account's quota. The bridge loads only this key from `.env` in speech mode; an existing
environment value takes precedence. The key stays server-side in the environment and request header; do not
commit it. Audio files are temporary and removed after playback. If synthesis
fails, a local chime plays instead. No API key is needed in chime mode.

Options: `--audio off` (default), `--audio-volume 0.6` (range 0–1),
`--speech-model gemini-3.8-flash-tts`, and `--voice Puck`. Audio playback currently
supports macOS via `afplay`; changing the default sound output redirects the
next playback. If Bluetooth disconnects, macOS may route audio to the laptop.
Stop the server with Ctrl+C to stop local audio.

`GET /health` includes audio mode, queue length, successful playback count,
skipped cue count, and a sanitized error message. `POST /v1/audio/test` accepts
`{"sound":"ready"}`; it shares the bridge's existing authentication/lockdown
checks and returns 409 when audio is disabled. A queued response does not mean
the speaker has already played it; check the health counters afterward.

Run audio tests without a speaker or Gemini credentials:

```sh
python3 gemware/bridge/test_audio.py
python3 gemware/bridge/test_bridge.py
```

Reference: [Gemini speech generation](https://ai.google.dev/gemini-api/docs/generate-content/speech-generation).

### Musical talking-piano preview

`--audio speech --voice Leda --speech-effect piano` adds local note-based
resynthesis after Gemini speech generation. It follows syllable rhythm with
keyboard-like chords, with reduced intelligibility compared with regular speech.
This optional effect requires NumPy (`python3 -m pip install numpy`); it uses no
YouTube audio or samples. Omit `--speech-effect` for the regular voice.

For a brighter handheld-game character, use `--speech-effect chiptune` instead:
this locally re-synthesizes the speech rhythm with octave-up square-wave
arpeggios and stepped amplitude. It requires NumPy and blends mostly clear speech with a lighter musical layer
so the words remain easier to understand.
