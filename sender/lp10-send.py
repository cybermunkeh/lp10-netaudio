#!/usr/bin/env python3
"""Send an unmodified classic PCM WAV data chunk to lp10-netaudio.

No decoding, resampling, channel remixing, volume processing, or sample-format
conversion occurs in this tool. It accepts only ordinary RIFF/WAVE PCM files
(format tag 1) with 16-, packed-24-, or 32-bit little-endian samples.
"""

from __future__ import annotations

import argparse
import os
import socket
import struct
import sys
from dataclasses import dataclass
from pathlib import Path

MAGIC = b"LP10NAU1"
VERSION = 1
HEADER_BYTES = 32
ENDIAN_LITTLE = 1
FORMAT_S16_LE = 1
FORMAT_S24_LE = 2
FORMAT_S24_3LE = 3
FORMAT_S32_LE = 4
ALLOWED_RATES = {44_100, 48_000, 88_200, 96_000, 176_400, 192_000}


class WavError(ValueError):
    """Raised for a WAV file which cannot be sent as untouched PCM."""


@dataclass(frozen=True)
class WavInfo:
    sample_rate: int
    channels: int
    bit_depth: int
    format_code: int
    frame_bytes: int
    data_offset: int
    data_bytes: int

    @property
    def format_name(self) -> str:
        return {
            FORMAT_S16_LE: "S16_LE",
            FORMAT_S24_LE: "S24_LE",
            FORMAT_S24_3LE: "S24_3LE",
            FORMAT_S32_LE: "S32_LE",
        }[self.format_code]


def read_exact(file_obj, count: int) -> bytes:
    data = file_obj.read(count)
    if len(data) != count:
        raise WavError("truncated RIFF/WAVE file")
    return data


def parse_wav(path: Path) -> WavInfo:
    """Parse a classic PCM RIFF/WAVE file without using an audio decoder."""
    with path.open("rb") as file_obj:
        riff = read_exact(file_obj, 12)
        if riff[:4] != b"RIFF" or riff[8:12] != b"WAVE":
            raise WavError("only little-endian RIFF/WAVE files are supported (not RF64/RIFX)")

        fmt_data: bytes | None = None
        data_offset: int | None = None
        data_bytes: int | None = None
        while True:
            chunk_header = file_obj.read(8)
            if not chunk_header:
                break
            if len(chunk_header) != 8:
                raise WavError("truncated WAV chunk header")
            chunk_id, chunk_size = struct.unpack("<4sI", chunk_header)
            if chunk_id == b"fmt ":
                fmt_data = read_exact(file_obj, chunk_size)
            elif chunk_id == b"data":
                data_offset = file_obj.tell()
                data_bytes = chunk_size
                break
            else:
                file_obj.seek(chunk_size, os.SEEK_CUR)
            if chunk_size & 1:
                file_obj.seek(1, os.SEEK_CUR)

    if fmt_data is None or data_offset is None or data_bytes is None:
        raise WavError("WAV must contain both fmt and data chunks")
    if len(fmt_data) < 16:
        raise WavError("WAV fmt chunk is too short")

    audio_format, channels, sample_rate, byte_rate, block_align, bits = struct.unpack(
        "<HHIIHH", fmt_data[:16]
    )
    if audio_format != 1:
        raise WavError(
            f"WAV format tag {audio_format} is not classic PCM (format tag 1); no conversion is available"
        )
    if channels < 1 or channels > 8:
        raise WavError(f"channel count {channels} is outside the supported 1..8 range")
    if sample_rate not in ALLOWED_RATES:
        raise WavError(
            f"sample rate {sample_rate} Hz is not in the protocol's supported set: "
            "44100, 48000, 88200, 96000, 176400, 192000"
        )
    if bits == 16:
        format_code, bytes_per_sample = FORMAT_S16_LE, 2
    elif bits == 24:
        # Classic PCM WAV stores 24-bit samples packed in three bytes. Sending
        # S24_3LE preserves those bytes exactly; S24_LE would require expansion.
        format_code, bytes_per_sample = FORMAT_S24_3LE, 3
    elif bits == 32:
        format_code, bytes_per_sample = FORMAT_S32_LE, 4
    else:
        raise WavError(f"{bits}-bit PCM is unsupported; no format conversion is available")

    frame_bytes = channels * bytes_per_sample
    if block_align != frame_bytes or byte_rate != sample_rate * frame_bytes:
        raise WavError("WAV block alignment or byte rate is inconsistent; refusing to reinterpret samples")
    if data_bytes % frame_bytes:
        raise WavError("WAV data chunk ends with a partial PCM frame")
    if path.stat().st_size < data_offset + data_bytes:
        raise WavError("WAV data chunk is truncated")
    return WavInfo(sample_rate, channels, bits, format_code, frame_bytes, data_offset, data_bytes)


def stream_file(path: Path, info: WavInfo, host: str, port: int) -> None:
    header = struct.pack(
        "!8sHHIHHHHII",
        MAGIC,
        VERSION,
        HEADER_BYTES,
        info.sample_rate,
        info.channels,
        info.format_code,
        info.bit_depth,
        ENDIAN_LITTLE,
        info.frame_bytes,
        0,  # flags/reserved
    )
    assert len(header) == HEADER_BYTES
    with socket.create_connection((host, port), timeout=10) as connection:
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        connection.sendall(header)
        with path.open("rb") as file_obj:
            file_obj.seek(info.data_offset)
            remaining = info.data_bytes
            while remaining:
                block = file_obj.read(min(64 * 1024, remaining))
                if not block:
                    raise WavError("WAV data became unavailable while transmitting")
                connection.sendall(block)
                remaining -= len(block)


def main() -> int:
    parser = argparse.ArgumentParser(description="Send unchanged PCM WAV samples to lp10-netaudio")
    parser.add_argument("--host", required=True, help="LP10 IP address or resolvable host name")
    parser.add_argument("--port", type=int, default=9100, help="TCP port (default: 9100)")
    parser.add_argument("--file", required=True, type=Path, help="classic PCM RIFF/WAVE input file")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535:
        parser.error("--port must be in 1..65535")
    try:
        info = parse_wav(args.file)
        frames = info.data_bytes // info.frame_bytes
        print(
            f"Sending {args.file}: {info.sample_rate} Hz, {info.channels} ch, "
            f"{info.format_name}, {frames} frames, unchanged PCM",
            file=sys.stderr,
        )
        stream_file(args.file, info, args.host, args.port)
    except (OSError, WavError, socket.error) as error:
        print(f"lp10-send: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
