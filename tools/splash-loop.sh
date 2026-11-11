#!/bin/sh
i=0
while [ ! -e /dev/fb0 ] && [ "$i" -lt 300 ]; do i=$((i+1)); sleep 0.1; done
i=0
while [ ! -e /run/oir-ui-ready ] && [ "$i" -lt 80 ]; do
  /opt/radio/splash/bootsplash /dev/fb0 /opt/radio/splash/splash.raw >/dev/null 2>&1
  i=$((i+1)); sleep 0.25
done
