#!/bin/sh
# Read-only LP10 audio diagnostics. It intentionally makes no system changes.

echo '=== uname -a ==='
uname -a 2>&1

echo '=== /proc/asound/cards ==='
cat /proc/asound/cards 2>&1

echo '=== /proc/asound/pcm ==='
cat /proc/asound/pcm 2>&1

echo '=== aplay -l ==='
aplay -l 2>&1

echo '=== lsmod | grep aloop ==='
lsmod 2>&1 | grep aloop || true

echo '=== fuser -v /dev/snd/* ==='
fuser -v /dev/snd/* 2>&1 || true

echo '=== running lp10-netaudio processes ==='
ps w 2>&1 | grep '[l]p10-netaudio' || true
