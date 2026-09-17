#!/usr/bin/env python3
"""Create the two embedded Codex notification tones from deterministic sine waves.

No recordings or downloaded assets are used. ffmpeg with libopus is required only
when regenerating the checked-in Ogg files.
"""

from __future__ import annotations

import argparse
import math
import shutil
import subprocess
import tempfile
import wave
from pathlib import Path


SAMPLE_RATE = 16_000
FRAME_SAMPLES = 960  # 60 ms; matches AudioService's Ogg packet reader.
ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "main" / "assets" / "common"


def make_pcm(pitches: tuple[int, ...]) -> bytes:
    samples: list[int] = []
    for frame, pitch in enumerate(pitches):
        for index in range(FRAME_SAMPLES):
            absolute = frame * FRAME_SAMPLES + index
            envelope = min(1.0, index / 80.0, (FRAME_SAMPLES - index) / 100.0)
            value = int(7000 * envelope * math.sin(2 * math.pi * pitch * absolute / SAMPLE_RATE))
            samples.append(value)
    return b"".join(value.to_bytes(2, "little", signed=True) for value in samples)


def write_tone(path: Path, pitches: tuple[int, ...]) -> None:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise SystemExit("ffmpeg with libopus is required to regenerate notification tones")
    with tempfile.TemporaryDirectory() as temp_dir:
        wav_path = Path(temp_dir) / "tone.wav"
        with wave.open(str(wav_path), "wb") as output:
            output.setnchannels(1)
            output.setsampwidth(2)
            output.setframerate(SAMPLE_RATE)
            output.writeframes(make_pcm(pitches))
        subprocess.run([
            ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-i", str(wav_path),
            "-ac", "1", "-ar", str(SAMPLE_RATE), "-c:a", "libopus", "-b:a", "16k",
            "-frame_duration", "60", str(path),
        ], check=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-dir", type=Path, default=ASSETS)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    write_tone(args.output_dir / "codex_attention.ogg", (880, 880, 660, 660))
    write_tone(args.output_dir / "codex_success.ogg", (660, 660, 1040, 1040, 1040))


if __name__ == "__main__":
    main()
