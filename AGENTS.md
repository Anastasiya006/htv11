# Gemmi bridge

When work in this repository should be visible on the physical Gemmi device,
send only meaningful state changes through the local bridge. Do not report
every shell command or include secrets, credentials, or private source text.

```bash
./gemware/bin/gemmi notify --agent codex --state working "Short status"
./gemware/bin/gemmi ask --agent codex --state needs-attention \
  --choice "Allow" --choice "Deny" --wait 120 "Short question"
```

`--wait` blocks until the device answers and prints the chosen option; it
exits non-zero on timeout, and a device dismissal leaves the ask unanswered.

Use `GEMMI_BRIDGE_URL` when the bridge is not on its default local address.
The bridge must already be running; if it is unavailable, continue the task
and mention the failed notification only when it matters.

Testing without hardware:

```bash
python3 gemware/bridge/test_bridge.py   # bridge behaviour suite (13 tests)
./gemware/bin/gemmi sim --auto          # software device that auto-answers asks
./gemware/bin/gemmi sim                 # interactive: type 1-3 to answer asks
```

The simulator uses device_id `gemmi-sim` — separate consumption state from the
real `gemmi-esp32`, so both can be attached to one bridge.
