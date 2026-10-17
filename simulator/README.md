# Gemmi / Agent companion

A 240 × 320 pet interface to Gemini CLI, with a basic browser terminal and an inspector built into the device screen. The screen stays white, the surrounding workspace stays black, and technical detail lives behind a tap on Gemmi’s top status strip. The CLI stays a plain terminal.

## Run

```sh
cd simulator
npm install
npm start
```

Open **http://127.0.0.1:3210**. Click **Start CLI**, sign in if needed, and send a prompt. Gemini runs in the project root with its normal approval policy. Use `127.0.0.1`, not `localhost` (the server checks the HTTP host and WebSocket origin).

A restart of the bridge stops its embedded Gemini process. Reloading the page preserves the process and replays its terminal buffer. Only one tab can control it at once. `GEMMI_PORT=3211 node simulator/server.cjs` runs an isolated bridge on a different port.

## Explore without a model

Click **Replay demo** below Gemmi. A clearly labeled, synthetic 15-second sequence demonstrates planning, reading a file, a failed command, editing, verification, a response, and a sample image. The replay does not start a CLI or send terminal input. Existing CLI processes can continue in the background. Click **Exit demo** to return to live state.

## The companion

- Reading tools → reading animation; edits and commands → working; between tools → thinking.
- Tool errors → error reaction; completed turns → celebration; permission notifications → needs attention.
- Idle Gemmi blinks and looks around, then sleeps after a minute. Tap to wake or show affection.
- The pixel console shows activity plus a two-line response preview. **MORE** opens a scrollable answer while keeping a small animated Gemmi visible.
- A new prompt clears the previous response and image cards. Disconnects clear content and show the disconnected animation.
- Tap the device’s top status strip to open connection diagnostics.

## On-device inspector

Tap Gemmi’s top status strip to open a compact inspector on the 240 × 320 display. A small reading Gemmi stays above the console. Tabs switch between:

- **TOOLS**: current tool and duration, completed calls, tool/error counts, and turn elapsed time.
- **EVENTS**: a timestamped execution timeline, newest first.
- **LINK**: CLI state, round-trip latency, hook age, session ID, protocol, and JSON event export.

All durations use hook receipt times, including hook overhead. Calls are paired by tool name and an argument hash in FIFO order. This is an observation journal, not a canonical execution trace with CLI-native call IDs. Token counts, costs, and test pass counts are not invented.

The internal display stays exactly 240 × 320. Desktop defaults to 2× magnification; select 1× for actual CSS size. Small screens default to 1×. **Escape** closes response, image, and inspector readers.

## Picking answers

Numbered radio menus with a visible selection marker can be mirrored onto Gemmi. Tap an option and then **CONFIRM**; arrow keys and Enter are sent to the terminal. Context and options are rechecked before submission. **BACK** keeps the prompt in the terminal. Unrecognized prompts and free-text input stay in the terminal.

This is terminal menu detection, not a structured approval API. Ordinary numbered response lists without a selection marker are excluded. Gemini’s native approvals remain authoritative.

## Screenshots

Ask Gemini to save PNG/JPEG/WebP files into the project’s `screenshots/` folder. Images written during the current turn appear as **View image** cards. Markdown image links in completed responses also discover images in project subfolders; these are imported into the ignored screenshots folder. The viewer supports fit, original-size zoom/pan, and navigation through up to eight images. Each image is limited to 10 MB. Symlinks and arbitrary file paths are rejected. Gemini needs an actual browser capture tool to take screenshots; this bridge displays files, not captures pages.

The replay uses the labeled sample `demo-preview.svg`; live screenshot serving accepts raster images only.

## Local data and privacy

The project `.gemini/settings.json` registers lifecycle hooks. Review/trust the project in Gemini if prompted.

Ignored `simulator/.runtime/` contains:

- Session state and completed assistant response text (up to 32 KB per session).
- A versioned journal bounded to 512 events per session. It records event/tool names, target basenames or command executable names, timestamps, error flags, and argument hashes for pairing. It does **not** store raw prompts, raw command arguments, tool output, or environment values in the journal. Environment assignments used as executable prefixes are redacted.

Responses can contain whatever Gemini wrote, including sensitive content; runtime files and screenshots are ignored by Git. The terminal replay buffer is kept in server memory, capped at 256 KB. The server binds to loopback and checks Host/Origin. Stop the bridge before deleting `.runtime/` to clear saved data.

## Validation

```sh
npm run check
npm test
npx playwright install chromium  # once, for browser checks
# start a separate server with GEMMI_PORT=3211, then:
npm run test:ui
```

Node tests cover event pairing, durations, failures, metadata minimization, and retention. Browser tests cover logical device dimensions, mobile layout, on-device inspector tabs, menu forwarding, the demo, response clearing, image viewing/zoom, and safe text rendering.
