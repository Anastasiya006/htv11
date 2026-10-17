"""Optional local voice transformations. No reference audio is downloaded."""
import io
import wave


def _note_speech(data: bytes, *, chip: bool = False) -> bytes:
    """Re-synthesize speech energy as short, pitched keyboard-like chords.

    This is a stylized talking-piano effect, not a realistic sampled piano.
    Only the optional effect requires NumPy; ordinary speech/chimes do not.
    """
    import numpy as np

    with wave.open(io.BytesIO(data), "rb") as wav:
        rate, channels = wav.getframerate(), wav.getnchannels()
        if wav.getsampwidth() != 2 or channels not in (1, 2):
            raise ValueError("Piano effect requires 16-bit PCM")
        original = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").astype(np.float64) / 32768
    if channels == 2:
        original = original.reshape(-1, 2).mean(axis=1)
    length = len(original)
    if not length:
        raise ValueError("Empty voice audio")
    size, hop = 1024, 256
    padded = np.pad(original, (size // 2, size))
    frames = np.lib.stride_tricks.sliding_window_view(padded, size)[::hop]
    window = np.hanning(size)
    spectrum = np.abs(np.fft.rfft(frames * window, axis=1)) ** 2
    bins = np.fft.rfftfreq(size, 1 / rate)
    midi = np.arange(45, 85 if chip else 97)
    notes = 440 * 2 ** ((midi - 69) / 12)
    notes = notes[notes * (10 if chip else 3) < rate / 2]
    bandwidth = np.maximum(notes * 0.035, rate / size)
    bands = np.exp(-0.5 * ((bins[:, None] - notes[None, :]) / bandwidth[None, :]) ** 2)
    bands /= np.maximum(bands.sum(axis=0), 1e-10)
    amplitudes = np.sqrt(spectrum @ bands)
    rms = np.sqrt(np.mean(frames ** 2, axis=1))
    output = np.zeros(len(padded))
    overlap = np.zeros(len(padded))
    samples = np.arange(size) / rate
    carriers = notes * (2 if chip else 1)
    harmonics = (1, 3, 5) if chip else (1, 2, 3)
    strengths = (1, 0.33, 0.2) if chip else (1, 0.25, 0.08)
    cosines = [np.cos(2 * np.pi * h * carriers[:, None] * samples) for h in harmonics]
    sines = [np.sin(2 * np.pi * h * carriers[:, None] * samples) for h in harmonics]
    previous = np.zeros(len(notes))
    for i, amplitude in enumerate(amplitudes):
        # Sparse chords follow the changing vowels/formants and syllable timing.
        if len(amplitude) > 8:
            cutoff = np.partition(amplitude, -8)[-8]
            amplitude = np.where(amplitude >= cutoff, amplitude, 0)
        amplitude = 0.45 * previous + 0.55 * amplitude
        previous = amplitude
        norm = np.linalg.norm(amplitude)
        amplitude = amplitude / max(norm, 1e-10) * rms[i] * 1.2 if rms[i] > 0.003 else np.zeros_like(amplitude)
        if chip:
            # One square-wave channel rapidly arpeggiates the strongest notes.
            choices = np.argsort(amplitude)[-3:]
            chosen = choices[(i // 3) % len(choices)]
            loudness = np.linalg.norm(amplitude)
            amplitude = np.zeros_like(amplitude)
            amplitude[chosen] = loudness
        start = i * hop
        phase = 2 * np.pi * carriers * start / rate
        tone = np.zeros(size)
        for h, strength, cosine, sine in zip(harmonics, strengths, cosines, sines):
            tone += strength * ((amplitude * np.cos(h * phase)) @ cosine - (amplitude * np.sin(h * phase)) @ sine)
        output[start:start + size] += tone * window
        overlap[start:start + size] += window
    output /= np.maximum(overlap, 1e-8)
    output = output[size // 2:size // 2 + length]
    # Keep chiptune as a light musical layer so consonants remain intelligible.
    # Quantize only the synth, never the natural voice.
    if chip:
        output = np.round(output * 128) / 128
    dry = 0.75 if chip else 0.08
    output = output * (1 - dry) + original * dry
    peak = np.max(np.abs(output))
    if peak > 0:
        output *= min(1.0, 0.35 / peak)
    result = io.BytesIO()
    with wave.open(result, "wb") as wav:
        wav.setnchannels(1); wav.setsampwidth(2); wav.setframerate(rate)
        wav.writeframes((np.clip(output, -0.35, 0.35) * 32767).astype("<i2").tobytes())
    return result.getvalue()


def piano_speech(data: bytes) -> bytes:
    return _note_speech(data)


def chiptune_speech(data: bytes) -> bytes:
    """Bright, stepped square-wave arpeggios following the speech rhythm."""
    return _note_speech(data, chip=True)
