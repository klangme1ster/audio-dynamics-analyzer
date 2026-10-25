"""Streaming audio decoding.

libsndfile (via soundfile) handles WAV, FLAC, AIFF, OGG and MP3. M4A / AAC (AAC or ALAC
in an MP4 container, or raw ADTS AAC) are decoded with PyAV, which bundles FFmpeg and
applies the container's edit list, so encoder priming samples are removed.
Both paths yield float32 blocks of shape (frames, channels) with bounded memory.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

import numpy as np
import soundfile as sf

from .layout import ChannelLayout, layout_from_channel_names, layout_from_mask

FFMPEG_EXTENSIONS = {".m4a", ".aac"}
SNDFILE_EXTENSIONS = {".wav", ".flac", ".aif", ".aiff", ".ogg", ".mp3"}
AUDIO_EXTENSIONS = SNDFILE_EXTENSIONS | FFMPEG_EXTENSIONS


@dataclass
class AudioInfo:
    sample_rate: int
    channels: int
    layout: ChannelLayout | None = None  # from the container's channel mask / layout, if any


def wav_channel_mask(path: str | Path) -> int | None:
    """dwChannelMask of a WAVE_FORMAT_EXTENSIBLE file (RIFF / RF64), else None.

    libsndfile does not expose it, so the fmt chunk is read directly.
    """
    try:
        with open(path, "rb") as f:
            head = f.read(12)
            if len(head) < 12 or head[:4] not in (b"RIFF", b"RF64") or head[8:12] != b"WAVE":
                return None
            while True:
                chunk = f.read(8)
                if len(chunk) < 8:
                    return None
                cid, size = chunk[:4], struct.unpack("<I", chunk[4:])[0]
                if cid == b"fmt ":
                    fmt = f.read(min(size, 40))
                    if len(fmt) >= 24 and struct.unpack("<H", fmt[:2])[0] == 0xFFFE:
                        return struct.unpack("<I", fmt[20:24])[0]
                    return None
                f.seek(size + (size & 1), 1)
    except OSError:
        return None


def open_audio(path: str | Path, block_seconds: float = 5.0
               ) -> tuple[AudioInfo, Iterator[np.ndarray]]:
    """Return stream info and an iterator of float32 (frames, channels) blocks.

    block_seconds sets the libsndfile block size (a whole number of 100 ms bins);
    FFmpeg yields its native frame sizes. The engine accepts any block length.
    """
    path = Path(path)
    if path.suffix.lower() in FFMPEG_EXTENSIONS:
        return _open_ffmpeg(path)
    info = sf.info(str(path))
    layout = None
    if info.channels > 2 and path.suffix.lower() == ".wav":
        mask = wav_channel_mask(path)
        layout = layout_from_mask(mask) if mask else None
    block_frames = max(1, round(block_seconds * 10)) * max(1, info.samplerate // 10)
    blocks = sf.blocks(str(path), blocksize=block_frames, dtype="float32", always_2d=True)
    return AudioInfo(info.samplerate, info.channels, layout), blocks


def _open_ffmpeg(path: Path) -> tuple[AudioInfo, Iterator[np.ndarray]]:
    import av  # imported lazily: only needed for M4A / AAC

    container = av.open(str(path))
    try:
        stream = container.streams.audio[0]
    except IndexError:
        container.close()
        raise ValueError(f"{path.name}: no audio stream") from None
    channels = stream.codec_context.layout.nb_channels
    layout = None
    if channels > 2:
        try:
            layout = layout_from_channel_names([c.name for c in stream.codec_context.layout.channels])
        except AttributeError:  # PyAV without per-channel information
            layout = None
    info = AudioInfo(stream.codec_context.sample_rate, channels, layout)

    def blocks() -> Iterator[np.ndarray]:
        # packed float32, same rate and layout: a format conversion only, never a resample
        resampler = av.AudioResampler(format="flt", layout=stream.codec_context.layout,
                                      rate=info.sample_rate)
        try:
            for frame in container.decode(stream):
                for out in resampler.resample(frame):
                    yield out.to_ndarray().reshape(-1, channels)
            for out in resampler.resample(None):
                yield out.to_ndarray().reshape(-1, channels)
        finally:
            container.close()

    return info, blocks()
