# Gemmi + Gemini CLI

Install once with `cd simulator && npm install`. From the project root, run:

```sh
node simulator/server.cjs
```

Open http://127.0.0.1:3210, then click **Start CLI** beside Gemmi. Gemini runs in the browser terminal, in this project. Type, use arrow keys, and answer permission prompts there. Opening index.html directly no longer provides a live connection.

The project `.gemini/settings.json` registers Gemini CLI lifecycle hooks. If Gemini asks you to trust the project/hooks, review and accept them in the CLI to enable the connection.

Gemmi thinks during a turn, works during tools, celebrates a completed response, and needs attention on CLI notifications. Tap for a love reaction. No extra screen controls. Permissions remain in Gemini CLI; this integration only observes events.

Hooks write only session ID, event name, animation, and receipt time into ignored `simulator/.runtime/` files. Prompts, responses, command text, and credentials are not stored. The loopback server serves only simulator assets and aggregate animation state. Multiple sessions prioritize attention, then work. No hook event for an hour is treated as stale; an abruptly killed CLI can therefore leave a stale working animation until that timeout. This is an initial activity connection, not a durable agent transport.

Stop the server with Ctrl+C. Remove the `gemmi-display` hook entries to disconnect the CLI. The terminal uses xterm.js, node-pty, and ws. Refreshing preserves the running CLI while the bridge stays up; only one tab can control it at a time. Stopping the bridge also stops its CLI. Use `127.0.0.1`, not `localhost`, for the terminal origin check.

The bottom of Gemmi’s screen mirrors the latest three nonempty lines from the embedded terminal’s parsed output. Terminal formatting and cursor redraws are handled by xterm. Long lines wrap and the text area scrolls to the newest text. Actual terminal content (including command text and any input the CLI echoes) can appear here. Before connecting the embedded terminal, the display falls back to hook activity labels.


The device text now shows only completed assistant responses from Gemini's `AfterAgent.prompt_response` hook. It does not mirror terminal prompts, input, menus, or status bars. Responses appear when a turn finishes, not while streaming. They are stored locally in ignored `.runtime/*.response.json` files (up to 32 KB per session); delete that folder to clear them. Restart the bridge after this update, and restart the Gemini session to pick up hook configuration changes if necessary.

Device text includes thinking/tool activity and completed responses as a small scrollable log. Activity labels come from lifecycle hooks; these are status descriptions, not the model’s private reasoning. Terminal menus and echoed user input remain excluded.

Completed answers now remain as a two-line preview below Gemmi. Click the preview to read the entire answer within the 240 × 320 screen; scroll to read and use Back or Escape to return. Live activity has its own line and cannot overwrite the answer.

Gemmi can mirror numbered radio menus that have a visible selection marker in the embedded terminal. Tap an option and then CONFIRM; the terminal receives arrow keys and Enter. The menu is rechecked before submission. BACK leaves the prompt available in the terminal. Unrecognized prompts and free text must be answered in the terminal. This uses terminal menu detection, not a structured Gemini approval API; a real permission prompt still needs end-to-end verification.

## Screenshot cards

Ask Gemini to save a screenshot in `screenshots/` at the project root. Images written during the current turn appear under Gemmi as **View image**. Tap to fit the image to the screen; use ZOOM + to view at original size and scroll to pan. Arrows step through up to eight images. New turns and disconnection clear the cards. PNG/JPEG/WebP up to 10 MB are supported. See `../screenshots/README.md`.
