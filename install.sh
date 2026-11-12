#!/bin/bash
# iRadio installer: build UI, install assets, services and helper scripts.
# Tested on Raspberry Pi OS Lite 64-bit (Debian 13 "trixie"), Pi 4B, arm64.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PREFIX="/opt/radio"
JOBS="$(nproc)"

say() { printf '\n\033[1;36m== %s\033[0m\n' "$*"; }
die() { printf '\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

[ -d "$ROOT/vendor/lvgl" ] || die "vendor/lvgl missing. Run: git clone --depth 1 -b v9.2.2 https://github.com/lvgl/lvgl.git vendor/lvgl"

say "Building radio-ui"
cmake -S "$ROOT" -B "$ROOT/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$ROOT/build" -j"$JOBS"

say "Installing to $PREFIX"
sudo mkdir -p "$PREFIX/ui/build" "$PREFIX/ui/src" "$PREFIX/assets" "$PREFIX/splash"

sudo install -m 755 "$ROOT/build/radio-ui" "$PREFIX/ui/build/radio-ui"
sudo install -m 644 "$ROOT/src/"*.c "$ROOT/src/"*.h "$PREFIX/ui/src/"
sudo install -m 644 "$ROOT/CMakeLists.txt" "$ROOT/lv_conf.h" "$PREFIX/ui/"

say "Assets"
sudo install -m 644 "$ROOT/assets/splash800.rgb565" "$PREFIX/assets/splash800.rgb565"
sudo install -m 644 "$ROOT/assets/bg800.rgb565" "$PREFIX/assets/bg800.rgb565"
sudo install -m 644 "$ROOT/assets/nocover320.rgb565" "$PREFIX/assets/nocover320.rgb565"
sudo install -m 644 "$ROOT/assets/splash800.rgb565" "$PREFIX/splash/splash.raw"
if [ -d "$ROOT/assets/logos565" ]; then
    sudo mkdir -p "$PREFIX/assets/logos565"
    sudo cp -f "$ROOT/assets/logos565/"*.rgb565 "$PREFIX/assets/logos565/"
fi

say "Boot splash helper"
gcc -O2 -o "$ROOT/build/bootsplash" "$ROOT/tools/bootsplash.c"
sudo install -m 755 "$ROOT/build/bootsplash" "$PREFIX/splash/bootsplash"
sudo install -m 755 "$ROOT/tools/splash-loop.sh" "$PREFIX/splash/splash-loop.sh"

say "Helper scripts"
for s in pulse-switch.sh bt-pair.sh bt-autoconnect.sh; do
    [ -f "$ROOT/$s" ] && sudo install -m 755 "$ROOT/$s" "$PREFIX/$s"
done
sudo mkdir -p /var/lib/radio-bt

say "systemd units"
sudo install -m 644 "$ROOT/systemd/"*.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable radio-ui.service oir-splash.service pulseaudio.service
sudo systemctl enable oir-bt-autoconnect.service oir-out-restore.service 2>/dev/null || true
sudo systemctl restart radio-ui.service

say "Done"
cat <<EOF
Installed:
  binary   : $PREFIX/ui/build/radio-ui
  assets   : $PREFIX/assets/ (splash800.rgb565, bg800.rgb565, logos565/)
  splash   : $PREFIX/splash/ (bootsplash, splash.raw, splash-loop.sh)
  services : radio-ui, oir-splash (enabled)

Remaining manual steps (boot config) — see README.md, section 4:
  /boot/firmware/config.txt   : dtoverlay=hifiberry-dac, disable_splash=1
  /boot/firmware/cmdline.txt  : quiet loglevel=0 logo.nologo vt.global_cursor_default=0 console=tty3
  /boot/firmware/splash.png   : optional firmware splash (<=224 KB, 24-bit PNG)
  systemctl mask getty@tty1
EOF
