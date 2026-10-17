# Gemmi bridge (first vertical slice)

The bridge is a local queue between agents on this computer and the Gemware
ESP32. It has no cloud service and no third-party Python packages. By default
it listens only on `127.0.0.1`, so it is not accidentally exposed to a LAN.

```bash
./gemware/bin/gemmi serve
```

It listens on port `8732` and stores events in
`~/.local/state/gemmi/events.jsonl`. Keep it running while Gemmi is in use.
For a trusted home LAN, pass `--host 0.0.0.0` and configure the ESP32 with the
computer's LAN URL. Do not do this on eduroam or another client-isolated
network.

An agent posts meaningful state changes through the same wrapper:

```bash
./gemware/bin/gemmi notify --agent codex --state working "Reading the firmware"
./gemware/bin/gemmi ask --agent codex --state needs-attention \
  --choice "Allow" --choice "Deny" "Flash the ESP32?"
./gemware/bin/gemmi artifact --agent codex --state reading "Screenshot ready"
```

For now this is an explicit wrapper, rather than ACP integration. It is easier
to inspect, works with any local agent, and keeps device protocol independent
from agent vendors. Once the event contract is proven, an ACP/MCP adapter can
call these same HTTP endpoints without changing Gemware.

## HTTP contract

- `POST /v1/events` adds a compact JSON event.
- `GET /v1/device/next?after=<event-id>` returns the next queued event or
  `204 No Content`.
- `GET /health` confirms the bridge is alive.

The current device client polls and holds its most recent event ID in memory.
Durable acknowledgements, targeted queues, screenshots, and user answers are
the next protocol increment; they are intentionally not faked in this first
connection test.

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
