#!/usr/bin/env python3
"""Gemmi musings: watches the bridge feed, asks Gemini for a short in-character
thought, and posts it as agent "gemmi" so the panel shows it next to the pet.

Reads GEMINI_API_KEY from the repo .env (or env). Posts to GEMMI_BRIDGE_URL.
"""
import json
import os
import random
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BRIDGE = os.environ.get("GEMMI_BRIDGE_URL", "http://127.0.0.1:8732")
MODEL = os.environ.get("GEMMI_GEMINI_MODEL", "gemini-3.5-flash-lite")


def load_env():
    for name in (".env", "gemware/.env", "gemware/bridge/.env"):
        path = ROOT / name
        if path.exists():
            for line in path.read_text().splitlines():
                if "=" in line and not line.startswith("#"):
                    key, _, val = line.partition("=")
                    os.environ.setdefault(key.strip(), val.strip().strip('"'))


def read_token():
    state = Path(os.environ.get("XDG_STATE_HOME", Path.home() / ".local" / "state"))
    path = state / "gemmi" / "bridge-token"
    return path.read_text().strip() if path.exists() else None


def bridge(path, payload=None):
    headers = {"Content-Type": "application/json"}
    token = read_token()
    if token:
        headers["Authorization"] = "Bearer " + token
    req = urllib.request.Request(
        BRIDGE + path,
        data=json.dumps(payload).encode() if payload else None,
        method="POST" if payload else "GET",
        headers=headers,
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            return json.loads(resp.read())
    except Exception:
        return None


def muse(api_key, context):
    prompt = (
        "You are Gemmi, a small desktop creature (Tamagotchi) watching software "
        "agents work on this human's projects. From this recent activity, say one "
        "short conversational line (max 90 chars, lowercase, casual, a little "
        "witty — like a curious pet commenting). No quotes, no emoji.\n\n" + context
    )
    req = urllib.request.Request(
        f"https://generativelanguage.googleapis.com/v1beta/models/{MODEL}:generateContent?key={api_key}",
        data=json.dumps({"contents": [{"parts": [{"text": prompt}]}]}).encode(),
        method="POST",
        headers={"Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(req, timeout=15) as resp:
            parts = json.loads(resp.read())["candidates"][0]["content"]["parts"]
            return parts[0]["text"].strip().replace("\n", " ")[:140]
    except Exception:
        return None


OFFLINE_MUSINGS = [
    "watching {who} do {tag} things again",
    "{who} is busy. i approve",
    "that '{msg}' looks important. probably",
    "humans and their terminals...",
    "{who} keeps typing. relentless",
    "i would also be worried about that",
    "somewhere a test just passed. hopefully",
    "i blinked. did i miss anything?",
    "{who} again. busy little hands",
]

ANSWER_MUSINGS = [
    "{who} asked and you said '{ans}'. noted",
    "'{ans}'? bold choice",
    "you said '{ans}'. i'll remember that",
]


def offline_muse(events):
    e = random.choice(events[-12:])
    pool = ANSWER_MUSINGS if e.get("answer") else OFFLINE_MUSINGS
    line = random.choice(pool)
    return line.format(
        who=e.get("agent", "?"),
        tag=e.get("kind", "?"),
        msg=(e.get("message") or "")[:40],
        ans=(e.get("answer") or "")[:30],
    )[:140]


def main():
    load_env()
    api_key = os.environ.get("GEMINI_API_KEY")
    last_seen = 0
    while True:
        data = bridge("/v1/events")
        events = (data or {}).get("events", [])
        fresh = [e for e in events if e.get("id", 0) > last_seen]
        if fresh:
            last_seen = max(e["id"] for e in fresh)
            lines = []
            for e in fresh[-12:]:
                tag = e.get("kind", "?")
                who = e.get("agent", "?")
                msg = (e.get("message") or "")[:80]
                ans = e.get("answer") or ""
                lines.append(f"{who}/{tag}: {msg}" + (f" -> human said: {ans}" if ans else ""))
            thought = muse(api_key, "\n".join(lines)) if api_key else None
            if not thought:
                thought = offline_muse(fresh)
            bridge("/v1/events", {"kind": "log", "agent": "gemmi",
                                  "state": "thinking", "message": thought})
            print(f"musing: {thought}", flush=True)
        time.sleep(90)


if __name__ == "__main__":
    main()
