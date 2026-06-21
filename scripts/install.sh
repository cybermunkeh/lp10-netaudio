#!/bin/sh
# Install only this independent service. It never modifies ALSA configuration
# or files owned by the LP10 vendor firmware.
set -eu

PREFIX="/opt/lp10-netaudio"
INIT_DEST="/etc/init.d/S95lp10-netaudio"
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
BIN_SOURCE="${BIN_SOURCE:-$ROOT_DIR/build/lp10-netaudio}"
CONFIG_SOURCE="$ROOT_DIR/config/config.json"
INIT_SOURCE="$ROOT_DIR/init.d/S95lp10-netaudio"

if [ "$(id -u)" != "0" ]; then
    echo "Run this installer as root." >&2
    exit 1
fi
if [ ! -f "$BIN_SOURCE" ] || [ ! -f "$CONFIG_SOURCE" ] || [ ! -f "$INIT_SOURCE" ]; then
    echo "Missing build artifact or project files. Build first with: make" >&2
    exit 1
fi

mkdir -p "$PREFIX"
cp "$BIN_SOURCE" "$PREFIX/lp10-netaudio.new"
chmod 0755 "$PREFIX/lp10-netaudio.new"
mv -f "$PREFIX/lp10-netaudio.new" "$PREFIX/lp10-netaudio"

if [ ! -e "$PREFIX/config.json" ]; then
    cp "$CONFIG_SOURCE" "$PREFIX/config.json"
    chmod 0644 "$PREFIX/config.json"
    echo "Installed initial configuration at $PREFIX/config.json"
else
    echo "Kept existing configuration at $PREFIX/config.json"
fi

touch "$PREFIX/lp10-netaudio.log"
chmod 0644 "$PREFIX/lp10-netaudio.log"
cp "$INIT_SOURCE" "$INIT_DEST"
chmod 0755 "$INIT_DEST"

echo "Installed lp10-netaudio. Review $PREFIX/config.json, then run:"
echo "  $INIT_DEST start"
