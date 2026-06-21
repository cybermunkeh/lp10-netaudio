# LP10 NetAudio Protocol v1

Each TCP connection carries exactly one PCM stream. The sender writes the
32-byte header first, then the raw PCM frame bytes. The connection close marks
the end of the stream. There is no metadata or control channel after the
header.

All multi-byte header fields are unsigned big-endian integers. PCM payload
bytes use the specified endianness and are never altered by either component.

| Bytes | Field | Value |
| --- | --- | --- |
| 0..7 | magic | ASCII `LP10NAU1` |
| 8..9 | version | `1` |
| 10..11 | header bytes | `32` |
| 12..15 | sample rate | `44100`, `48000`, `88200`, `96000`, `176400`, or `192000` |
| 16..17 | channels | 1..8 |
| 18..19 | sample format | see below |
| 20..21 | valid bit depth | 16, 24, or 32 |
| 22..23 | endian | `1` = little endian |
| 24..27 | frame bytes | channels × physical bytes/sample |
| 28..31 | flags | zero |

Format identifiers:

| ID | Name | Valid bits | Payload bytes/sample |
| --- | --- | --- | --- |
| 1 | `S16_LE` | 16 | 2 |
| 2 | `S24_LE` | 24 | 4 |
| 3 | `S24_3LE` | 24 | 3 |
| 4 | `S32_LE` | 32 | 4 |

`S24_LE` and `S24_3LE` are intentionally distinct. Standard 24-bit PCM WAV
uses packed three-byte samples, so the provided sender emits `S24_3LE` and
does not expand the samples into `S24_LE` containers. The receiving hardware
must accept the incoming native ALSA format; otherwise the stream is rejected.
