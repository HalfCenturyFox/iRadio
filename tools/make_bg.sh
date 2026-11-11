#!/bin/bash
# Konvertuje sliku (avif/jpg/png) u 800x480 RGB565 pozadinu za OIR UI.
# Upotreba: ./make_bg.sh slika.avif [user@host]
set -e
SRC="${1:?upotreba: make_bg.sh slika.avif [host]}"
HOST="${2:-${IRADIO_HOST:-}}"
OUT="$(cd "$(dirname "$0")/.." && pwd)/assets/bg800.rgb565"

ffmpeg -hide_banner -loglevel error -y -i "$SRC" \
  -vf "scale=800:480:force_original_aspect_ratio=increase,crop=800:480" \
  -pix_fmt rgb565le -f rawvideo "$OUT"
echo "napravljeno: $OUT ($(stat -c%s "$OUT") B)"

if [ -n "$HOST" ]; then
  scp -q "$OUT" "$HOST:/tmp/bg800.rgb565"
  ssh "$HOST" 'sudo install -m 644 /tmp/bg800.rgb565 /opt/radio/assets/bg800.rgb565 && sudo systemctl restart radio-ui'
  echo "installed on $HOST, radio-ui restarted"
else
  echo "local only - copy $OUT to /opt/radio/assets/ and restart radio-ui (set IRADIO_HOST=user@host to auto-deploy)"
fi
