#!/usr/bin/env python3
"""Local event bridge between agents on this computer and a Gemmi device.

This first version deliberately uses only Python's standard library.  It is a
LAN-only HTTP service: agents post small events, while the ESP32 polls for the
next one.  Events are kept in a local JSONL file so restarting the bridge does
not silently discard the inbox.
"""
from __future__ import annotations

import argparse
import base64
import hmac
import json
import os
import re
import secrets
import subprocess
import threading
import time
import urllib.error
import urllib.request
from datetime import UTC, datetime
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse

DEFAULT_PORT = 8732
MAX_BODY_BYTES = 131072
MAX_IMAGE_B64 = 90000
MAX_EVENTS = 256
DEFAULT_URL = os.environ.get("GEMMI_BRIDGE_URL", f"http://127.0.0.1:{DEFAULT_PORT}")


def default_store_path() -> Path:
    state_home = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local" / "state"))
    return state_home / "gemmi" / "events.jsonl"


def default_token_path() -> Path:
    return default_store_path().with_name("bridge-token")


def default_lockdown_path() -> Path:
    return default_store_path().with_name("bridge.lockdown")


def read_token(path: Path) -> str | None:
    if not path.exists():
        return None
    token = path.read_text(encoding="utf-8").strip()
    if len(token) < 32:
        raise ValueError(f"bridge token at {path} is too short")
    return token


class EventStore:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.lock = threading.Lock()
        self.events: list[dict[str, Any]] = []
        self.consumed: dict[str, set[int]] = {}
        self.joined: dict[str, int] = {}
        self.next_id = 1
        responses: list[dict[str, Any]] = []
        if path.exists():
            for line in path.read_text(encoding="utf-8").splitlines():
                try:
                    record = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if not isinstance(record, dict):
                    continue
                if isinstance(record.get("id"), int):
                    self.events.append(record)
                    self.next_id = max(self.next_id, record["id"] + 1)
                elif isinstance(record.get("response_for"), int):
                    responses.append(record)
                elif isinstance(record.get("consumed_for"), dict):
                    mark = record["consumed_for"]
                    device, event_id = mark.get("device"), mark.get("event")
                    if isinstance(device, str) and isinstance(event_id, int):
                        self.consumed.setdefault(device, set()).add(event_id)
                elif isinstance(record.get("joined_for"), dict):
                    mark = record["joined_for"]
                    device, after = mark.get("device"), mark.get("after")
                    if isinstance(device, str) and isinstance(after, int):
                        self.joined[device] = max(self.joined.get(device, 0), after)
                elif isinstance(record.get("next_id"), int) and record.get("kind") is None:
                    self.next_id = max(self.next_id, record["next_id"])
        self.events = self.events[-MAX_EVENTS:]
        by_id = {event["id"]: event for event in self.events}
        for record in responses:
            event = by_id.get(record["response_for"])
            if event is not None and "response" not in event:
                event["response"] = {
                    "choice": record.get("choice", ""),
                    "device_id": record.get("device_id", ""),
                    "answered_at": record.get("answered_at", ""),
                }

    def _write_line(self, record: dict[str, Any]) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.path.open("a", encoding="utf-8") as file:
            file.write(json.dumps(record, separators=(",", ":")) + "\n")

    def append(self, payload: dict[str, Any]) -> dict[str, Any]:
        with self.lock:
            event = {
                "version": 1,
                "id": self.next_id,
                "received_at": datetime.now(UTC).isoformat(timespec="seconds"),
                **payload,
            }
            self.next_id += 1
            self.events.append(event)
            self.events = self.events[-MAX_EVENTS:]
            self._write_line(event)
            return event

    def get(self, event_id: int) -> dict[str, Any] | None:
        with self.lock:
            return next((event for event in self.events if event["id"] == event_id), None)

    def ack(self, device_id: str, event_id: int) -> None:
        """Mark one event as consumed for a device so it is never redelivered."""
        with self.lock:
            seen = self.consumed.setdefault(device_id, set())
            if event_id not in seen:
                seen.add(event_id)
                self._write_line({"consumed_for": {"device": device_id, "event": event_id}})

    def respond(self, device_id: str, event_id: int, choice: str) -> dict[str, Any]:
        """Attach a device's answer to an ask. Raises ValueError when invalid."""
        with self.lock:
            event = next((item for item in self.events if item["id"] == event_id), None)
            if event is None:
                raise ValueError("unknown event id")
            if event["kind"] != "ask":
                raise ValueError("only ask events can be answered")
            if event["choices"] and choice not in event["choices"]:
                raise ValueError("choice is not one of the event's choices")
            existing = event.get("response")
            if existing is not None:
                if existing["choice"] == choice:
                    return event
                raise ValueError(f"event is already answered with '{existing['choice']}'")
            event["response"] = {
                "choice": choice,
                "device_id": device_id,
                "answered_at": datetime.now(UTC).isoformat(timespec="seconds"),
            }
            self._write_line({"response_for": event_id, **event["response"]})
        self.ack(device_id, event_id)
        return event

    def next_for_device(self, device_id: str | None, after: int) -> dict[str, Any] | None:
        with self.lock:
            seen = self.consumed.get(device_id, set()) if device_id else set()
            base = after
            if device_id:
                if device_id not in self.joined and after == 0:
                    # A device we have never seen joins at the latest still-open
                    # ask, or at the tip of history: it never walks the backlog.
                    # The join point is recorded once so later polls stream in
                    # everything queued after it.
                    open_asks = [e["id"] for e in self.events
                                 if e["kind"] == "ask" and "response" not in e]
                    self.joined[device_id] = (open_asks[-1] - 1) if open_asks else (
                        self.events[-1]["id"] if self.events else 0)
                    self._write_line({"joined_for": {"device": device_id,
                                                   "after": self.joined[device_id]}})
                base = max(base, self.joined.get(device_id, 0))
            for event in self.events:
                if event["id"] <= base or event["id"] in seen:
                    continue
                if event["kind"] == "ask" and "response" in event:
                    continue
                return event
            return None

    def clear(self) -> None:
        with self.lock:
            self.events.clear()
            self.consumed.clear()
            self.joined.clear()
            # Keep id numbering monotonic across a restart so devices holding
            # a higher last-seen id do not skip the new epoch's events.
            # Write-then-rename so a kill mid-write cannot leave a torn file.
            self.path.parent.mkdir(parents=True, exist_ok=True)
            temp = self.path.with_suffix(".tmp")
            temp.write_text(json.dumps({"next_id": self.next_id}) + "\n",
                            encoding="utf-8")
            temp.replace(self.path)


def clean_string(payload: dict[str, Any], key: str, *, required: bool = False, limit: int = 160) -> str:
    value = payload.get(key, "")
    if not isinstance(value, str):
        raise ValueError(f"{key} must be a string")
    value = " ".join(value.split())
    if required and not value:
        raise ValueError(f"{key} is required")
    if len(value) > limit:
        raise ValueError(f"{key} must be {limit} characters or fewer")
    return value


def normalise_event(payload: Any) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise ValueError("event must be a JSON object")
    kind = clean_string(payload, "kind", required=True, limit=32)
    if kind not in {"status", "ask", "artifact", "log"}:
        raise ValueError("kind must be status, ask, artifact, or log")
    choices = payload.get("choices", [])
    if not isinstance(choices, list) or len(choices) > 3 or not all(isinstance(item, str) for item in choices):
        raise ValueError("choices must be a list of at most three strings")
    return {
        "kind": kind,
        "agent": clean_string(payload, "agent", required=True, limit=32),
        "state": clean_string(payload, "state", limit=32) or "working",
        "message": clean_string(payload, "message", required=True, limit=160),
        "choices": [" ".join(choice.split())[:32] for choice in choices],
        "artifact": clean_string(payload, "artifact", limit=200),
    } | ({"image_b64": payload["image_b64"]} if (
        kind == "artifact" and isinstance(payload.get("image_b64"), str)
        and len(payload["image_b64"]) <= MAX_IMAGE_B64
        and payload["image_b64"].strip()) else {})


def device_payload(body: bytes) -> tuple[str, int, str | None]:
    """Parse {device_id, event_id, choice?} from a device POST."""
    payload = json.loads(body)
    if not isinstance(payload, dict):
        raise ValueError("payload must be a JSON object")
    device_id = payload.get("device_id")
    if not isinstance(device_id, str) or not device_id.strip():
        raise ValueError("device_id is required")
    event_id = payload.get("event_id")
    if not isinstance(event_id, int) or isinstance(event_id, bool):
        raise ValueError("event_id must be an integer")
    choice = payload.get("choice")
    if choice is not None:
        if not isinstance(choice, str) or not choice.strip() or len(choice) > 32:
            raise ValueError("choice must be a string of 32 characters or fewer")
        choice = " ".join(choice.split())
    return device_id.strip()[:32], event_id, choice


def compact_view(event: dict[str, Any]) -> bytes:
    """Newline-framed fields the ESP32 parses without a JSON stack.

    Layout: id, kind, agent, state, choices joined by '|', artifact, then the
    message (kept last so it may contain newlines).
    """
    choices = "|".join(choice.replace("|", "/") for choice in event.get("choices", []))
    artifact = str(event.get("artifact", "")).replace("\n", " ")
    lines = [str(event["id"]), event["kind"], event["agent"], event["state"],
             choices, artifact, event["message"]]
    return "\n".join(lines).encode()


class BridgeHandler(BaseHTTPRequestHandler):
    store: EventStore
    token: str | None
    lockdown_path: Path

    def log_message(self, format: str, *args: object) -> None:
        print(f"gemmi bridge: {self.address_string()} - {format % args}")

    def send_json(self, status: HTTPStatus, data: dict[str, Any] | None = None) -> None:
        body = b"" if data is None else json.dumps(data, separators=(",", ":")).encode()
        self.send_bytes(status, body, "application/json")

    def send_bytes(self, status: HTTPStatus, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def authorise(self) -> bool:
        if self.lockdown_path.exists():
            self.send_json(HTTPStatus.SERVICE_UNAVAILABLE, {"error": "bridge lockdown is active"})
            return False
        if self.token is None:
            return True
        expected = f"Bearer {self.token}"
        supplied = self.headers.get("Authorization", "")
        if not hmac.compare_digest(supplied, expected):
            self.send_json(HTTPStatus.UNAUTHORIZED, {"error": "unauthorised"})
            return False
        return True

    def read_body(self) -> bytes:
        content_length = int(self.headers.get("Content-Length", "0"))
        if not 0 < content_length <= MAX_BODY_BYTES:
            raise ValueError("body must be between 1 and 4096 bytes")
        return self.rfile.read(content_length)

    def do_GET(self) -> None:
        if not self.authorise():
            return
        parsed = urlparse(self.path)
        if parsed.path == "/health":
            self.send_json(HTTPStatus.OK, {"ok": True, "version": 1})
            return
        if parsed.path.startswith("/v1/agent/"):
            agent = parsed.path.rsplit("/", 1)[1].strip()
            with self.store.lock:
                events = [e for e in self.store.events
                          if agent and e["agent"] == agent][-10:]
            self.send_json(HTTPStatus.OK, {"agent": agent, "events": events})
            return
        if parsed.path == "/v1/events":
            with self.store.lock:
                self.send_json(HTTPStatus.OK, {
                    "events": list(self.store.events),
                    "open_asks": [e["id"] for e in self.store.events
                                  if e["kind"] == "ask" and "response" not in e],
                })
            return
        if parsed.path.startswith("/v1/events/"):
            tail = parsed.path.rsplit("/", 1)[1].strip()
            event = None
            if tail.endswith("/image") or "/image" in parsed.path:
                event_id_str = parsed.path.split("/")[3]
                try:
                    event = self.store.get(int(event_id_str))
                except (ValueError, IndexError):
                    event = None
                if event is None or not event.get("image_b64"):
                    self.send_json(HTTPStatus.NOT_FOUND, {"error": "no image"})
                else:
                    self.send_bytes(HTTPStatus.OK,
                                    base64.b64decode(event["image_b64"]),
                                    "application/octet-stream")
                return
            try:
                event = self.store.get(int(tail))
            except ValueError:
                event = None
            if event is None:
                self.send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
            else:
                self.send_json(HTTPStatus.OK, event)
            return
        if parsed.path != "/v1/device/next":
            self.send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
            return
        query = parse_qs(parsed.query)
        try:
            after = int(query.get("after", ["0"])[0])
        except ValueError:
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": "after must be an integer"})
            return
        device_id = query.get("device_id", [""])[0].strip() or None
        event = self.store.next_for_device(device_id, after)
        if not event:
            self.send_json(HTTPStatus.NO_CONTENT)
            return
        if self.headers.get("Accept") == "text/plain":
            # The ESP32 deliberately consumes this compact, newline-safe view;
            # regular tools continue to receive the complete JSON envelope.
            self.send_bytes(HTTPStatus.OK, compact_view(event), "text/plain; charset=utf-8")
            return
        self.send_json(HTTPStatus.OK, event)

    def do_POST(self) -> None:
        if not self.authorise():
            return
        parsed = urlparse(self.path)
        try:
            if parsed.path == "/v1/events":
                event = self.store.append(normalise_event(json.loads(self.read_body())))
                self.send_json(HTTPStatus.CREATED, event)
                return
            if parsed.path == "/v1/drawing":
                # Device -> agent: a doodle drawn on the panel. Stored as a
                # normal artifact event so agents can fetch it via the feed.
                payload = json.loads(self.read_body())
                event = self.store.append(normalise_event({
                    "kind": "artifact",
                    "agent": "gemmi",
                    "state": "idle",
                    "message": f"doodle for {payload.get('agent', 'agent')}",
                    "artifact": "doodle",
                    "image_b64": payload.get("image_b64", ""),
                }))
                self.send_json(HTTPStatus.CREATED, event)
                return
            if parsed.path in ("/v1/device/ack", "/v1/device/respond"):
                device_id, event_id, choice = device_payload(self.read_body())
                if parsed.path.endswith("ack"):
                    self.store.ack(device_id, event_id)
                    self.send_json(HTTPStatus.NO_CONTENT)
                else:
                    if choice is None:
                        raise ValueError("choice is required")
                    self.send_json(HTTPStatus.OK, self.store.respond(device_id, event_id, choice))
                return
            self.send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
        except (ValueError, json.JSONDecodeError) as error:
            self.send_json(HTTPStatus.BAD_REQUEST, {"error": str(error)})

    def do_DELETE(self) -> None:
        if not self.authorise():
            return
        if self.path != "/v1/events":
            self.send_json(HTTPStatus.NOT_FOUND, {"error": "not found"})
            return
        self.store.clear()
        self.send_json(HTTPStatus.NO_CONTENT)


def serve(args: argparse.Namespace) -> int:
    BridgeHandler.store = EventStore(Path(args.store).expanduser())
    BridgeHandler.token = read_token(Path(args.token_file).expanduser())
    BridgeHandler.lockdown_path = Path(args.lockdown_file).expanduser()
    server = ThreadingHTTPServer((args.host, args.port), BridgeHandler)
    print(f"Gemmi bridge listening on http://{args.host}:{args.port}")
    print(f"Event store: {BridgeHandler.store.path}")
    print(f"Bridge token gate: {'enabled' if BridgeHandler.token else 'disabled (localhost only)'}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nGemmi bridge stopped.")
    finally:
        server.server_close()
    return 0


def bridge_request(args: argparse.Namespace, path: str, *,
                   method: str = "GET", payload: dict[str, Any] | None = None) -> Any:
    headers = {"Content-Type": "application/json"}
    token = read_token(Path(args.token_file).expanduser())
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(
        f"{args.url.rstrip('/')}{path}",
        data=None if payload is None else json.dumps(payload).encode(),
        headers=headers,
        method=method,
    )
    try:
        with urllib.request.urlopen(request, timeout=args.timeout) as response:
            body = response.read()
    except urllib.error.HTTPError as error:
        detail = error.read().decode(errors="replace")[:200]
        raise SystemExit(f"Gemmi bridge at {args.url} rejected the request ({error.code}): {detail}") from error
    except urllib.error.URLError as error:
        raise SystemExit(f"Could not reach Gemmi bridge at {args.url}: {error}") from error
    if not body:
        return None
    try:
        return json.loads(body)
    except json.JSONDecodeError as error:
        raise SystemExit(f"Gemmi bridge at {args.url} returned invalid JSON") from error


def submit(args: argparse.Namespace, kind: str) -> int:
    event = {
        "kind": kind,
        "agent": args.agent,
        "state": args.state,
        "message": args.message,
        "choices": getattr(args, "choice", []),
        "artifact": getattr(args, "artifact", ""),
    }
    image_path = getattr(args, "file", None)
    if image_path:
        """The device can't decode compressed images — render to RGB565 here.
        Layout: 2-byte LE width, 2-byte LE height, then packed RGB565 LE. """
        try:
            from PIL import Image
        except ImportError:
            raise SystemExit("gemmi artifact --file needs Pillow (pip install pillow)")
        im = Image.open(image_path).convert("RGB")
        im.thumbnail((96, 72))
        w, h = im.size
        pixels = bytearray()
        raw = im.tobytes()
        for i in range(0, len(raw), 3):
            px = ((raw[i] >> 3) << 11) | ((raw[i + 1] >> 2) << 5) | (raw[i + 2] >> 3)
            pixels += px.to_bytes(2, "little")
        packed = w.to_bytes(2, "little") + h.to_bytes(2, "little") + bytes(pixels)
        event["image_b64"] = base64.b64encode(packed).decode()
    created = bridge_request(args, "/v1/events", method="POST", payload=event)
    print(f"Queued Gemmi event #{created['id']} ({created['kind']} from {created['agent']}).")
    wait = getattr(args, "wait", 0)
    if kind != "ask" or wait <= 0:
        return 0
    deadline = time.monotonic() + wait
    while time.monotonic() < deadline:
        latest = bridge_request(args, f"/v1/events/{created['id']}")
        answer = latest.get("response")
        if answer:
            print(f"Gemmi answered #{created['id']}: {answer['choice']}")
            return 0
        time.sleep(0.7)
    print(f"Timed out after {wait:g}s waiting for a Gemmi answer to #{created['id']}.")
    return 2


def wait_answer(args: argparse.Namespace) -> int:
    """Block until a previously posted ask has a recorded response."""
    deadline = time.monotonic() + args.wait
    while True:
        event = bridge_request(args, f"/v1/events/{args.event_id}")
        if event is None:
            raise SystemExit(f"Gemmi event #{args.event_id} does not exist.")
        answer = event.get("response")
        if answer:
            print(f"Gemmi answered #{event['id']}: {answer['choice']}")
            return 0
        if time.monotonic() >= deadline:
            print(f"Timed out after {args.wait:g}s waiting for a Gemmi answer to #{args.event_id}.")
            return 2
        time.sleep(0.7)


def list_events(args: argparse.Namespace) -> int:
    view = bridge_request(args, "/v1/events")
    open_asks = set(view["open_asks"])
    for event in view["events"]:
        marker = "??" if event["id"] in open_asks else ("ok" if event.get("response") else "..")
        answer = event.get("response", {}).get("choice", "")
        tail = f" -> {answer}" if answer else ""
        print(f"{marker} #{event['id']} {event['kind']} {event['agent']}: "
              f"{event['message'][:60]}{tail}")
    if open_asks:
        print(f"open asks waiting on a device: {sorted(open_asks)}")
    return 0


def clear_events(args: argparse.Namespace) -> int:
    bridge_request(args, "/v1/events", method="DELETE")
    print("Cleared every queued Gemmi event and device cursor.")
    return 0


def fetch_drawing(args: argparse.Namespace) -> int:
    """Save the newest drawing/artifact image to a PNG the agent can open."""
    events = bridge_request(args, "/v1/events").get("events", [])
    candidates = [e for e in events
                  if e.get("image_b64") and len(base64.b64decode(e["image_b64"])) > 4]
    if not candidates:
        raise SystemExit("No drawings or image artifacts yet.")
    event = candidates[-1]
    try:
        from PIL import Image
    except ImportError:
        raise SystemExit("gemmi drawing needs Pillow (pip install pillow)")
    packed = base64.b64decode(event["image_b64"])
    w = packed[0] | (packed[1] << 8)
    h = packed[2] | (packed[3] << 8)
    if len(packed) < 4 + w * h * 2:
        raise SystemExit(f"#{event['id']}: image payload truncated")
    px = packed[4:4 + w * h * 2]
    im = Image.new("RGB", (w, h))
    data = im.load()
    for i in range(w * h):
        v = px[2 * i] | (px[2 * i + 1] << 8)
        x, y = i % w, i // w
        data[x, y] = ((v >> 11 & 31) << 3, (v >> 5 & 63) << 2, (v & 31) << 3)
    out = Path(args.out)
    im.save(out)
    print(f"#{event['id']} {event['agent']}: {event['message']} -> {out}")
    return 0


def simulate_device(args: argparse.Namespace) -> int:
    """A software Gemmi: polls like firmware, acks statuses, answers asks.

    Lets the whole agent->bridge->device->answer loop run with no ESP32
    connected. Uses its own device id so it never disturbs a real device.
    """
    device_id = args.device_id
    last_shown = 0
    print(f"Simulated device '{device_id}' polling {args.url} every {args.interval:g}s (Ctrl-C to stop).")
    try:
        while True:
            event = bridge_request(
                args, f"/v1/device/next?device_id={device_id}&after={last_shown}")
            if event is None:
                time.sleep(args.interval)
                continue
            last_shown = event["id"]
            if event["kind"] == "ask":
                print(f"\n?? #{event['id']} {event['agent']}: {event['message']}")
            elif event["kind"] == "artifact":
                print(f"\n<> #{event['id']} {event['agent']} [{event['state']}]: "
                      f"{event['message']} (artifact: {event['artifact'] or 'none'})")
            else:
                print(f".. #{event['id']} {event['agent']} [{event['state']}]: {event['message']}")
            if event["kind"] != "ask" or not event["choices"]:
                bridge_request(args, "/v1/device/ack", method="POST",
                               payload={"device_id": device_id, "event_id": event["id"]})
                continue
            choices = event["choices"]
            for index, choice in enumerate(choices):
                print(f"   [{index + 1}] {choice}")
            if args.auto:
                pick = choices[0]
                print(f"   auto-answering: {pick}")
            else:
                pick = None
                while pick is None:
                    try:
                        typed = input("   answer 1-%d: " % len(choices)).strip()
                    except EOFError:
                        typed = ""
                        pick = choices[0]
                    if pick is None:
                        if typed in choices:
                            pick = typed
                        elif typed.isdigit() and 1 <= int(typed) <= len(choices):
                            pick = choices[int(typed) - 1]
                        else:
                            print("   type a number or the choice text")
            bridge_request(args, "/v1/device/respond", method="POST",
                           payload={"device_id": device_id,
                                    "event_id": event["id"], "choice": pick})
            print(f"   -> answered #{event['id']}: {pick}")
    except KeyboardInterrupt:
        print("\nSimulated device stopped.")
        return 0


def tunnel(args: argparse.Namespace) -> int:
    token_path = Path(args.token_file).expanduser()
    lockdown_path = Path(args.lockdown_file).expanduser()
    if args.tunnel_action == "init":
        if token_path.exists():
            raise SystemExit(f"Refusing to replace existing bridge token: {token_path}")
        token_path.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(token_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as file:
            file.write(secrets.token_urlsafe(32) + "\n")
        print(f"Created protected bridge token at {token_path}.")
        print("Restart `gemmi serve` to activate token checking, then provision the same token into Gemware's ignored sdkconfig.")
        return 0
    if args.tunnel_action == "off":
        lockdown_path.parent.mkdir(parents=True, exist_ok=True)
        lockdown_path.touch(mode=0o600, exist_ok=True)
        subprocess.run(["tailscale", "funnel", "--https=443", "off"], check=True)
        print("Gemmi public tunnel is off and the local bridge is locked down.")
        return 0
    if read_token(token_path) is None:
        raise SystemExit("Run `gemmi tunnel init` before enabling a public tunnel.")
    lockdown_path.unlink(missing_ok=True)
    subprocess.run(["tailscale", "funnel", "--bg", "--https=443", f"http://127.0.0.1:{args.port}"], check=True)
    print("Gemmi public tunnel is enabled. Use `gemmi tunnel off` to close it immediately.")
    return 0


def set_sdkconfig_value(contents: str, name: str, value: str) -> str:
    line = f"CONFIG_{name}={value}"
    pattern = re.compile(rf"^(?:CONFIG_{re.escape(name)}=.*|# CONFIG_{re.escape(name)} is not set)$", re.MULTILINE)
    if pattern.search(contents):
        return pattern.sub(line, contents)
    return contents.rstrip() + "\n" + line + "\n"


def provision_device(args: argparse.Namespace) -> int:
    token = read_token(Path(args.token_file).expanduser())
    if token is None:
        raise SystemExit("Run `gemmi tunnel init` before provisioning Gemware.")
    try:
        status = json.loads(subprocess.check_output(["tailscale", "status", "--json"], text=True))
        hostname = status["Self"]["DNSName"].rstrip(".")
    except (KeyError, json.JSONDecodeError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"Could not determine this node's Funnel hostname: {error}") from error
    config_path = Path(args.sdkconfig).expanduser()
    contents = config_path.read_text(encoding="utf-8")
    contents = set_sdkconfig_value(contents, "GEMMI_BRIDGE_URL", json.dumps(f"https://{hostname}"))
    contents = set_sdkconfig_value(contents, "GEMMI_BRIDGE_TOKEN", json.dumps(token))
    contents = set_sdkconfig_value(contents, "GEMMI_BRIDGE_REQUIRE_TLS", "y")
    contents = set_sdkconfig_value(contents, "GEMMI_BRIDGE_REQUIRE_TOKEN", "y")
    config_path.write_text(contents, encoding="utf-8")
    print("Provisioned Gemware's ignored local config for token-gated HTTPS Funnel.")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Gemmi local agent bridge")
    subcommands = parser.add_subparsers(dest="command", required=True)
    server = subcommands.add_parser("serve", help="run the computer-side bridge")
    server.add_argument("--host", default="127.0.0.1")
    server.add_argument("--port", default=DEFAULT_PORT, type=int)
    server.add_argument("--store", default=str(default_store_path()))
    server.add_argument("--token-file", default=str(default_token_path()))
    server.add_argument("--lockdown-file", default=str(default_lockdown_path()))
    server.set_defaults(func=serve)
    for command, kind, help_text in (
        ("notify", "status", "send a status update"),
        ("ask", "ask", "send a question with up to three choices"),
        ("artifact", "artifact", "announce a screenshot or other artifact"),
        ("log", "log", "stream one activity line (CoT, tool call) to the agent feed"),
    ):
        sender = subcommands.add_parser(command, help=help_text)
        sender.add_argument("message", help="short text Gemmi should show")
        sender.add_argument("--agent", required=True, help="short agent name, e.g. codex")
        sender.add_argument("--state", default="working", help="working, thinking, needs-attention, success, error, …")
        sender.add_argument("--url", default=DEFAULT_URL, help="bridge URL (or GEMMI_BRIDGE_URL)")
        sender.add_argument("--timeout", default=3, type=float)
        sender.add_argument("--token-file", default=str(default_token_path()))
        if kind == "ask":
            sender.add_argument("--choice", action="append", default=[], help="one of up to three choices")
            sender.add_argument("--wait", type=float, default=0, metavar="SECONDS",
                                help="block until the device answers and print the choice")
        if kind == "artifact":
            sender.add_argument("--artifact", default="", help="artifact name or URL Gemmi should reference")
            sender.add_argument("--file", default=None, help="local JPEG file to upload for on-device viewing")
        sender.set_defaults(func=lambda parsed, selected=kind: submit(parsed, selected))
    drawing = subcommands.add_parser("drawing", help="save the latest device doodle/artifact image to PNG")
    drawing.add_argument("--out", default="/tmp/gemmi-doodle.png")
    drawing.add_argument("--url", default=DEFAULT_URL)
    drawing.add_argument("--timeout", default=3, type=float)
    drawing.add_argument("--token-file", default=str(default_token_path()))
    drawing.set_defaults(func=fetch_drawing)
    waiter = subcommands.add_parser("wait", help="block until an ask event is answered")
    waiter.add_argument("event_id", type=int)
    waiter.add_argument("--wait", type=float, default=60, metavar="SECONDS")
    waiter.add_argument("--url", default=DEFAULT_URL, help="bridge URL (or GEMMI_BRIDGE_URL)")
    waiter.add_argument("--timeout", default=3, type=float)
    waiter.add_argument("--token-file", default=str(default_token_path()))
    waiter.set_defaults(func=wait_answer)
    events_parser = subcommands.add_parser("events", help="list queued events and open asks")
    events_parser.add_argument("--url", default=DEFAULT_URL, help="bridge URL (or GEMMI_BRIDGE_URL)")
    events_parser.add_argument("--timeout", default=3, type=float)
    events_parser.add_argument("--token-file", default=str(default_token_path()))
    events_parser.set_defaults(func=list_events)
    clearer = subcommands.add_parser("clear", help="drop every queued event and device cursor")
    clearer.add_argument("--url", default=DEFAULT_URL, help="bridge URL (or GEMMI_BRIDGE_URL)")
    clearer.add_argument("--timeout", default=3, type=float)
    clearer.add_argument("--token-file", default=str(default_token_path()))
    clearer.set_defaults(func=clear_events)
    sim = subcommands.add_parser("sim", help="run a software Gemmi device (no ESP32 needed)")
    sim.add_argument("--device-id", default="gemmi-sim")
    sim.add_argument("--interval", default=1.5, type=float, help="poll interval in seconds")
    sim.add_argument("--auto", action="store_true", help="answer every ask with the first choice")
    sim.add_argument("--url", default=DEFAULT_URL, help="bridge URL (or GEMMI_BRIDGE_URL)")
    sim.add_argument("--timeout", default=3, type=float)
    sim.add_argument("--token-file", default=str(default_token_path()))
    sim.set_defaults(func=simulate_device)
    tunnel_parser = subcommands.add_parser("tunnel", help="manage the token-gated public Funnel")
    tunnel_parser.add_argument("tunnel_action", choices=("init", "on", "off"))
    tunnel_parser.add_argument("--port", default=DEFAULT_PORT, type=int)
    tunnel_parser.add_argument("--token-file", default=str(default_token_path()))
    tunnel_parser.add_argument("--lockdown-file", default=str(default_lockdown_path()))
    tunnel_parser.set_defaults(func=tunnel)
    provision = subcommands.add_parser("provision-device", help="provision ignored ESP config for secure Funnel")
    provision.add_argument("--sdkconfig", default=str(Path(__file__).resolve().parents[1] / "gemware" / "sdkconfig"))
    provision.add_argument("--token-file", default=str(default_token_path()))
    provision.set_defaults(func=provision_device)
    return parser


if __name__ == "__main__":
    arguments = build_parser().parse_args()
    raise SystemExit(arguments.func(arguments))
