#!/usr/bin/env bash
# LiteKern Go: puts music on the phone, over ssh, into /extras/music (the Music app's folder).
# Run in WSL, with the phone running. Wi-Fi works: set LKX_PHONE=root@<its address>.
#
#   bash tools/push-music.sh song.wav "another song.mp3" ...
#   bash tools/push-music.sh ~/Music/album/*
#
# The Music app plays WAV and MP3 as they are. Anything else (FLAC, M4A, ...) is turned into 16 bit stereo
# 48 kHz WAV first, which needs ffmpeg on this PC (apt install ffmpeg in WSL). No admin on the phone.
# Songs stay across restarts (they're in the extras area, not the system).
set -euo pipefail
[ $# -ge 1 ] || { echo "usage: bash tools/push-music.sh FILE..." >&2; exit 2; }
KEY=${LKX_PHONE_KEY:-$HOME/.ssh/litekern-phone}
PHONE=${LKX_PHONE:-root@172.16.42.1}
SSH=(ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=10 "$PHONE")
"${SSH[@]}" 'mkdir -p /extras/music' || { echo "the phone doesn't answer over ssh" >&2; exit 1; }
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
for f in "$@"; do
    [ -f "$f" ] || { echo "skip (not a file): $f" >&2; continue; }
    base=$(basename "$f"); ext=${base##*.}; ext=${ext,,}; stem=${base%.*}
    case "$ext" in
        wav|mp3) src=$f; out="$stem.$ext" ;;
        *)
            command -v ffmpeg >/dev/null || { echo "skip (needs ffmpeg to convert): $f" >&2; continue; }
            ffmpeg -loglevel error -y -i "$f" -ac 2 -ar 48000 -c:a pcm_s16le "$tmp/$stem.wav"
            src=$tmp/$stem.wav; out="$stem.wav" ;;
    esac
    printf 'copy: %s\n' "$out"
    "${SSH[@]}" "cat > '/extras/music/$out'" < "$src"
done
echo "done: $(${SSH[@]} 'ls /extras/music | wc -l') songs on the phone"
