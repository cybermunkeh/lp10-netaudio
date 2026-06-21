#!/bin/sh
# Remove only lp10-netaudio files. Persistent data is removed solely after an
# explicit affirmative answer on stdin.
set -u

PREFIX="/opt/lp10-netaudio"
INIT_DEST="/etc/init.d/S95lp10-netaudio"

if [ "$(id -u)" != "0" ]; then
    echo "Run this uninstaller as root." >&2
    exit 1
fi

if [ -x "$INIT_DEST" ]; then
    "$INIT_DEST" stop || true
fi
rm -f "$INIT_DEST"
echo "Removed $INIT_DEST"

printf 'Remove %s (binary, config, and logs)? [y/N] ' "$PREFIX"
answer=""
if ! read -r answer; then
    answer=""
fi
case "$answer" in
    y|Y|yes|YES)
        rm -rf "$PREFIX"
        echo "Removed $PREFIX"
        ;;
    *)
        echo "Kept $PREFIX"
        ;;
esac
