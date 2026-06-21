<!-- cybermunkeh-standard-readme: 2026-06-21 -->
<p align="center"><img src="assets/cyber-global-guardian-banner-v1.png" alt="Globale Vernetzung unter Schutz eines heraldischen Adlers" width="100%"></p>

# LP10 NetAudio

> **Dienstliche Kurzlage:** Privates Entwicklungs- und Dokumentationsvorhaben. Keine amtliche Zugehörigkeit, keine Einsatzfreigabe und keine operative Verwendung.

| Merkmal | Festlegung |
| --- | --- |
| Repository | cybermunkeh/lp10-netaudio |
| Status | Bestandsaufnahme und technische Weiterentwicklung |
| Vertraulichkeit | Privat |
| Technologiebezug | C/POSIX, ALSA, TCP, Arylic LP10 |
| Fachliche Schlagworte | `arylic-lp10`, `audio`, `alsa`, `tcp`, `networking` |

## 1. Auftrag

Dieses Repository dokumentiert und entwickelt einen schlanken PCM-Empfänger für Arylic/Rakoit-LP10-Geräte. Ein einzelner TCP-Stream wird ohne Resampling oder DSP direkt auf ein konfiguriertes ALSA-`hw:X,Y`-Gerät ausgegeben.

## 2. Einsatzrahmen

- Nutzung ausschließlich im privaten Entwicklungs- und Testkontext.
- Keine amtliche, militärische oder behördliche Verwendung impliziert.
- Externe Schnittstellen, Zugangsdaten und produktive Konfigurationen dürfen nicht eingecheckt werden.

## 3. Technischer Bestand

- Primäre Technologien: **C/POSIX, ALSA, TCP und Arylic LP10**
- Repositoriumsthemen: `arylic-lp10`, `audio`, `alsa`, `tcp`, `networking`
- Gemeinsames Gestaltungsmerkmal: Banner mit globaler Vernetzung und Schutzmotiv.

## 4. Sicherheits- und Verfahrenshinweise

1. Vor jedem Push sind Geheimnisse, lokale Datenbanken, Logs, Build-Artefakte und Gerätezugangsdaten auszuschließen.
2. Drittanbieter-Code, Lizenzen und Upstream-Bezüge sind vor einer Veröffentlichung zu prüfen.
3. Änderungen sind mit einem nachvollziehbaren Änderungsgrund und einer technischen Prüfung zu dokumentieren.

## 5. Betriebsstatus

**Bearbeitungsstand:** Dokumentation vorbereitet; Quellimport und Veröffentlichung erfolgen erst nach projektbezogener Sicherheits- und Lizenzprüfung.
## 6. Vorhandene Projektdokumentation

# lp10-netaudio

`lp10-netaudio` is a deliberately small, independent PCM receiver for an
Arylic/Rakoit LP10. It accepts one TCP stream at a time and writes the received
frames directly to a configured ALSA `hw:X,Y` device. It does not replace or
modify the manufacturer firmware pipeline. A single stream is intentional:
direct `hw:` playback is exclusive and this prototype does not mix streams.

There is no resampling, format conversion, channel mixing, DSP, volume control,
ALSA `plug`, `dmix`, `default`, `plughw`, cloud service, telemetry, or web UI.
If the exact requested stream cannot be opened on the selected device, the
connection is closed and the reason is logged. The listener remains available
for the next connection.

## Project layout

```text
lp10-netaudio/
├── src/lp10_netaudio.c          # POSIX/ALSA receiver daemon
├── sender/lp10-send.py          # Windows/Linux PCM WAV sender
├── sender/lp10-send.cmd         # Windows wrapper
├── config/config.json           # first-install configuration template
├── init.d/S95lp10-netaudio      # BusyBox init.d service script
├── scripts/install.sh
├── scripts/uninstall.sh
├── scripts/diagnose-lp10.sh     # read-only diagnostics
├── docs/PROTOCOL.md
└── Makefile
```

## Before deployment: identify a free hardware ALSA device

Copy the diagnostic script first. This script has no write operations.

```sh
scp scripts/diagnose-lp10.sh root@192.168.1.50:/tmp/
ssh root@192.168.1.50 'chmod 0755 /tmp/diagnose-lp10.sh && /tmp/diagnose-lp10.sh'
```

Set `alsa_device` in `config/config.json` to the actual codec hardware device,
for example `hw:0,0`, only after inspecting `aplay -l` and `/proc/asound/pcm`.
Do not point this service at `default`, `plug:*`, `plughw:*`, `dmix:*`, or
`rate:*`; the daemon rejects those names.

`hw:X,Y` is exclusive. If the vendor pipeline currently owns that PCM device,
the daemon will log `Device or resource busy`. Do **not** kill vendor processes
to make it work. Choose an available hardware endpoint or arrange normal,
manual service ownership before testing.

## Build

The receiver must be built for the LP10's CPU architecture and linked against
the device-compatible ALSA ABI. The daemon only depends on libc and libasound.

On the LP10 (if it has a compiler and ALSA development headers):

```sh
cd /tmp/lp10-netaudio
make
```

The available LP10 audit identifies the target as `armv7l`; its kernel was
built with the `arm-none-linux-gnueabihf` toolchain family. Cross-build on a
Linux host with the matching ARM hard-float toolchain and an LP10-compatible
sysroot:

```sh
make clean
make CC=arm-none-linux-gnueabihf-gcc \
  CPPFLAGS="--sysroot=/path/to/lp10-sysroot -I/path/to/lp10-sysroot/usr/include" \
  LDFLAGS="--sysroot=/path/to/lp10-sysroot"
```

Do not copy a Windows or x86 Linux build to the player. Before deployment,
verify that the target has `libasound.so.2` with `ssh root@LP10 'ls -l
/usr/lib/libasound.so* /lib/libasound.so* 2>/dev/null'`. The sysroot must
provide both `alsa/asoundlib.h` at build time and a compatible `libasound.so.2`
at runtime.

## Exact SCP deployment and service start

The following native-build route is the least ambiguous when the LP10 contains
`gcc`, `make`, and ALSA headers. From the directory *containing* this project,
copy the named directory (this guarantees the remote path below):

```sh
cd /path/to/development
scp -r lp10-netaudio root@192.168.1.50:/tmp/
ssh root@192.168.1.50 'cd /tmp/lp10-netaudio && make && chmod 0755 scripts/*.sh init.d/S95lp10-netaudio sender/lp10-send.py && ./scripts/install.sh'
ssh root@192.168.1.50 '/etc/init.d/S95lp10-netaudio start'
ssh root@192.168.1.50 '/etc/init.d/S95lp10-netaudio status'
```

The same copy command from Windows PowerShell is:

```powershell
scp -r C:\development\lp10-netaudio root@192.168.1.50:/tmp/
```

For a cross-built binary, retain the same layout so the installer can find
`build/lp10-netaudio`, then use:

```sh
cd /path/to/development
scp -r lp10-netaudio root@192.168.1.50:/tmp/
ssh root@192.168.1.50 'cd /tmp/lp10-netaudio && chmod 0755 scripts/*.sh init.d/S95lp10-netaudio && ./scripts/install.sh'
ssh root@192.168.1.50 '/etc/init.d/S95lp10-netaudio start'
```

The installer creates only `/opt/lp10-netaudio/` and
`/etc/init.d/S95lp10-netaudio`. It preserves an existing
`/opt/lp10-netaudio/config.json`, does not edit `/etc/asound.conf`, does not
start automatically, and does not touch any vendor service.

To tail the receiver log:

```sh
ssh root@192.168.1.50 'tail -f /opt/lp10-netaudio/lp10-netaudio.log'
```

To stop or remove it:

```sh
ssh root@192.168.1.50 '/etc/init.d/S95lp10-netaudio stop'
scp scripts/uninstall.sh root@192.168.1.50:/tmp/
ssh root@192.168.1.50 'chmod 0755 /tmp/uninstall.sh && /tmp/uninstall.sh'
```

The uninstaller asks before it removes `/opt/lp10-netaudio`; entering anything
other than `y`, `Y`, `yes`, or `YES` keeps the binary, config, and logs.

## Send a WAV file

The sender uses only Python's standard library and streams the WAV `data` chunk
unchanged. It accepts classic little-endian RIFF PCM (format tag 1) at 44.1,
48, 88.2, 96, 176.4, or 192 kHz with 16-, packed-24-, or 32-bit samples. It
rejects compressed WAV, floating-point WAV, RF64/RIFX, bad block alignment, and
other rates rather than converting them.

Linux/macOS:

```sh
python3 sender/lp10-send.py --host 192.168.1.50 --port 9100 --file test_44100.wav
```

Windows:

```powershell
python .\sender\lp10-send.py --host 192.168.1.50 --port 9100 --file .\test_44100.wav
# or
.\sender\lp10-send.cmd --host 192.168.1.50 --port 9100 --file .\test_44100.wav
```

The protocol is documented in [docs/PROTOCOL.md](docs/PROTOCOL.md). It does not
include discovery, authentication, encryption, or a control channel. Use it on
a trusted LAN/VLAN, or set `listen_host` to a specific trusted interface.

## Verification criteria

1. `status` reports the daemon running and `netstat -lnt`/`ss -lnt` shows TCP
   port 9100 if either utility is available on the LP10.
2. Run the sender with a supported WAV file.
3. Confirm `ALSA direct hw stream configured: ... rate=44100 ...` (or the
   chosen rate) appears in `/opt/lp10-netaudio/lp10-netaudio.log`.
4. If the codec rejects the rate or native format, the log states the ALSA error
   and the sender connection ends. No fallback or resampling is attempted.

The selected ALSA device decides the physical output route. A successful ALSA
open only proves direct hardware playback; use the LP10's normal hardware
routing and the diagnostic output to confirm that `hw:X,Y` is the WM8904 path.

