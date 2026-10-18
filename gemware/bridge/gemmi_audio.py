"""Laptop-side audio. Bluetooth routing belongs to the operating system.

Chimes use only the standard library; optional Gemini TTS uses the REST API.
The bounded worker never delays event delivery or plays historical events.
"""
from __future__ import annotations

import base64
import io
import importlib.util
import json
import math
import os
import queue
import re
import shutil
import shlex
import struct
import subprocess
import tempfile
import threading
import time
import urllib.request
import urllib.error
import wave
from pathlib import Path
from typing import Callable

CUES = {
    "ready": (1047, 1568, 2093), "ask": (1319, 1047, 1319),
    "success": (1047, 1319, 1568, 2093), "error": (784, 622),
    "artifact": (1568, 2093), "level-up": (1047, 1319, 1568, 2093, 2637),
}
DEFAULT_MODEL = "gemini-3.8-flash-tts"


def load_env_key(path: Path) -> bool:
    """Load only the speech key from a local .env; existing environment wins."""
    if os.environ.get("GEMINI_API_KEY"):
        return True
    if not path.exists():
        return False
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line.startswith("export "):
            line = line[7:].lstrip()
        name, separator, value = line.partition("=")
        if separator and name.strip() == "GEMINI_API_KEY":
            try:
                words = shlex.split(value, comments=True)
            except ValueError:
                raise ValueError("Invalid quoting for GEMINI_API_KEY in .env") from None
            if len(words) > 1:
                raise ValueError("GEMINI_API_KEY in .env must be a single value")
            if words and words[0]:
                os.environ["GEMINI_API_KEY"] = words[0]
                return True
    return False


def event_cue(event: dict) -> str | None:
    if event.get("kind") == "log":
        return None
    if event.get("kind") == "ask" or event.get("state") == "needs-attention":
        return "ask"
    if event.get("state") in ("error", "failed"):
        return "error"
    if event.get("kind") == "artifact":
        return "artifact"
    if event.get("kind") == "status" and event.get("state") in ("success", "done", "completed"):
        return "success"
    return None


def speech_text(event: dict, cue: str) -> str:
    # Only fixed, nonsensitive notification lines leave the computer.
    # Agent names, prompts, commands, messages, and artifact paths stay local.
    return {
        "ask": "Gemmi needs your input!",
        "success": "Task complete! Nice work!",
        "error": "An agent needs your help.",
        "artifact": "There is something new to look at!",
        "ready": "Hi! I am Gemmi. Your little coding companion.",
        "level-up": "Level up! Look at us go!",
    }[cue]


def chime(cue: str) -> bytes:
    notes = CUES[cue]
    rate = 24000
    frames = bytearray()
    for frequency in notes:
        count = int(rate * 0.075)
        # Band-limited square-wave bleeps: an original virtual-pet sound,
        # with a quick chirp and soft edges rather than a harsh full-scale buzz.
        harmonics = [h for h in (1, 3, 5, 7) if h * frequency * 1.08 < rate / 2]
        for i in range(count):
            t = i / rate
            envelope = min(1.0, i / (rate * 0.003), (count - i) / (rate * 0.02))
            phase = 2 * math.pi * frequency * (t + 0.0008 * (1 - math.exp(-t / 0.01)))
            pulse = sum(math.sin(h * phase) / h for h in harmonics) / 1.2
            # A small stepped amplitude gives it a pocket-toy character.
            pulse = round(pulse * 32) / 32
            sample = int(32767 * 0.12 * envelope * pulse)
            frames.extend(struct.pack("<h", sample))
        frames.extend(b"\0\0" * int(rate * 0.035))
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(rate)
        wav.writeframes(frames)
    return output.getvalue()


def validate_wav(data: bytes) -> bytes:
    with wave.open(io.BytesIO(data), "rb") as wav:
        if wav.getcomptype() != "NONE" or wav.getsampwidth() != 2 or wav.getnchannels() not in (1, 2):
            raise ValueError("Unsupported speech audio format")
        if not 8000 <= wav.getframerate() <= 96000 or not 0 < wav.getnframes() <= wav.getframerate() * 30:
            raise ValueError("Speech must be between zero and 30 seconds")
        if len(wav.readframes(wav.getnframes())) != wav.getnframes() * wav.getnchannels() * 2:
            raise ValueError("Truncated speech audio")
    return data


def gemini_speech(text: str, *, model: str = DEFAULT_MODEL, voice: str = "Puck") -> bytes:
    key = os.environ.get("GEMINI_API_KEY")
    if not key:
        raise ValueError("GEMINI_API_KEY is not configured")
    if not re.fullmatch(r"[A-Za-z0-9._-]+", model):
        raise ValueError("Invalid speech model")
    payload = {
        "contents": [{"role": "user", "parts": [{"text": text[:240]}]}],
        "generationConfig": {"responseModalities": ["AUDIO"],
            "speechConfig": {"voiceConfig": {"voice": voice}}},
    }
    request = urllib.request.Request(
        f"https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent",
        data=json.dumps(payload).encode(), method="POST",
        headers={"Content-Type": "application/json", "x-goog-api-key": key},
    )
    with urllib.request.urlopen(request, timeout=12) as response:
        body = response.read(8 * 1024 * 1024 + 1)
    if len(body) > 8 * 1024 * 1024:
        raise ValueError("Speech response is too large")
    parts = json.loads(body)["candidates"][0]["content"]["parts"]
    audio = next(part["inlineData"] for part in parts if "inlineData" in part)
    data = base64.b64decode(audio["data"], validate=True)
    if data.startswith(b"RIFF"):
        return validate_wav(data)
    # Older TTS models can return headerless mono, signed 16-bit LE PCM.
    mime = audio.get("mimeType", "")
    if not mime.lower().startswith(("audio/l16", "audio/pcm")) or len(data) % 2:
        raise ValueError("Unsupported speech response")
    match = re.search(r"rate=(\d+)", mime)
    rate = int(match.group(1)) if match else 24000
    output = io.BytesIO()
    with wave.open(output, "wb") as wav:
        wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(rate); wav.writeframes(data)
    return validate_wav(output.getvalue())


# Laptop audio players, in preference order. Each builds an argv to play a WAV
# file at a volume in 0..1 on the operating system's current default output.
# afplay comes first so macOS behavior is unchanged; the rest cover Linux.
def _afplay(exe: str, path: Path, volume: float) -> list[str]:
    return [exe, "-v", f"{volume:.3f}", str(path)]


def _paplay(exe: str, path: Path, volume: float) -> list[str]:
    return [exe, f"--volume={round(volume * 65536)}", str(path)]


def _pw_play(exe: str, path: Path, volume: float) -> list[str]:
    return [exe, f"--volume={volume:.3f}", str(path)]


def _ffplay(exe: str, path: Path, volume: float) -> list[str]:
    return [exe, "-nodisp", "-autoexit", "-loglevel", "quiet",
            "-volume", str(round(volume * 100)), str(path)]


def _sox_play(exe: str, path: Path, volume: float) -> list[str]:
    return [exe, "-q", "-V1", str(path), "vol", f"{volume:.3f}"]


def _aplay(exe: str, path: Path, volume: float) -> list[str]:
    return [exe, "-q", str(path)]  # ALSA plays at the system mixer volume


PLAYERS = (
    ("afplay", "macOS default output", _afplay),
    ("paplay", "PulseAudio default output", _paplay),
    ("pw-play", "PipeWire default output", _pw_play),
    ("ffplay", "ffmpeg default output", _ffplay),
    ("play", "SoX default output", _sox_play),
    ("aplay", "ALSA default output", _aplay),
)


def detect_player() -> tuple[str, str, Callable[[str, Path, float], list[str]]] | None:
    """Pick the first available system audio player, or None if none exist."""
    for name, output, builder in PLAYERS:
        executable = shutil.which(name)
        if executable:
            return executable, output, builder
    return None


class AudioService:
    def __init__(self, mode: str, *, volume: float = 0.6, model: str = DEFAULT_MODEL,
                 voice: str = "Puck", effect: str = "none", player: Callable[[Path], None] | None = None,
                 synthesizer: Callable[[str], bytes] | None = None) -> None:
        if mode not in ("chimes", "speech") or not 0 <= volume <= 1:
            raise ValueError("Invalid audio settings")
        detected = detect_player()
        if player is None and detected is None:
            raise ValueError("Bridge audio requires a system audio player: macOS afplay, "
                             "or Linux paplay, pw-play, ffplay, play, or aplay")
        if mode == "speech" and synthesizer is None and not os.environ.get("GEMINI_API_KEY"):
            raise ValueError("Set GEMINI_API_KEY before enabling speech, or use --audio chimes")
        if effect not in ("none", "piano", "chiptune"):
            raise ValueError("Invalid speech effect")
        if effect != "none" and importlib.util.find_spec("numpy") is None:
            raise ValueError("Musical voice effects require NumPy: python3 -m pip install numpy")
        self.mode, self.voice, self.effect = mode, voice, effect
        self.executable = detected[0] if detected else None
        self.output = detected[1] if detected else "custom output"
        self._build_command = detected[2] if detected else None
        self.volume = volume
        self.player = player or self._play_file
        self.synthesizer = synthesizer or (lambda text: gemini_speech(text, model=model, voice=voice))
        self.jobs: queue.PriorityQueue = queue.PriorityQueue(maxsize=8)
        self.lock = threading.Lock()
        self.seen: set[int] = set()
        self.recent: dict[tuple[str, str], float] = {}
        self.sequence = 0
        self.played = self.dropped = 0
        self.last_error: str | None = None
        self.stop = threading.Event()
        self.worker = threading.Thread(target=self._run, name="gemmi-audio", daemon=True)
        self.worker.start()

    def submit(self, event: dict) -> bool:
        cue = event_cue(event)
        if cue is None:
            return False
        return self._enqueue(cue, speech_text(event, cue), event.get("id"), str(event.get("agent", "")), event)

    def test(self, cue: str) -> bool:
        if not isinstance(cue, str) or cue not in CUES:
            raise ValueError("Unknown audio cue")
        return self._enqueue(cue, speech_text({}, cue), None, "test")

    def _enqueue(self, cue: str, text: str, event_id: int | None, agent: str, event: dict | None = None) -> bool:
        with self.lock:
            if self.stop.is_set() or (event_id is not None and event_id in self.seen):
                return False
            now = time.monotonic()
            pair = (agent, cue)
            # New questions are never suppressed by the informational cue cooldown.
            if event_id is not None and cue != "ask" and now - self.recent.get(pair, -100) < 3:
                return False
            self.sequence += 1
            try:
                self.jobs.put_nowait((0 if cue == "ask" else 1, self.sequence, cue, text, now, event))
            except queue.Full:
                self.dropped += 1
                return False
            if event_id is not None:
                self.seen.add(event_id)
                if len(self.seen) > 256:
                    self.seen.remove(min(self.seen))
                self.recent[pair] = now
                if len(self.recent) > 256:
                    self.recent.pop(min(self.recent, key=self.recent.get))
            return True

    def status(self) -> dict:
        with self.lock:
            return {"mode": self.mode, "voice": self.voice, "effect": self.effect, "output": self.output, "queued": self.jobs.qsize(),
                    "played": self.played, "dropped": self.dropped, "last_error": self.last_error}

    def _play_file(self, path: Path) -> None:
        with subprocess.Popen(self._build_command(self.executable, path, self.volume),
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) as process:
            deadline = time.monotonic() + 35
            while process.poll() is None:
                if self.stop.wait(0.1) or time.monotonic() > deadline:
                    process.terminate()
                    try:
                        process.wait(timeout=1)
                    except subprocess.TimeoutExpired:
                        process.kill(); process.wait()
                    return
            if process.returncode:
                raise RuntimeError("Audio player failed")

    def _run(self) -> None:
        while not self.stop.is_set():
            try:
                _, _, cue, text, queued_at, event = self.jobs.get(timeout=0.2)
            except queue.Empty:
                continue
            try:
                if time.monotonic() - queued_at > 30 or (cue == "ask" and event and event.get("response")):
                    with self.lock:
                        self.dropped += 1
                    continue
                error = None
                if self.mode == "speech":
                    try:
                        data = validate_wav(self.synthesizer(text))
                        if self.effect == "piano":
                            from gemmi_voice_effects import piano_speech
                            data = piano_speech(data)
                        elif self.effect == "chiptune":
                            from gemmi_voice_effects import chiptune_speech
                            data = chiptune_speech(data)
                    except urllib.error.HTTPError as failure:
                        error = f"Gemini HTTP {failure.code}; used chime instead"
                        failure.close()
                        data = chime(cue)
                    except Exception:
                        # Do not log response bodies, credentials, or spoken content.
                        error = "Speech unavailable; used chime instead"
                        data = chime(cue)
                else:
                    data = chime(cue)
                if not self.stop.is_set():
                    with tempfile.TemporaryDirectory(prefix="gemmi-audio-") as folder:
                        path = Path(folder) / "notification.wav"
                        path.write_bytes(data)
                        self.player(path)
                    with self.lock:
                        self.played += 1
                        self.last_error = error
            except Exception:
                with self.lock:
                    self.last_error = "Audio playback failed; check your system sound output"
            finally:
                self.jobs.task_done()

    def close(self) -> None:
        self.stop.set()
        self.worker.join(timeout=1)
