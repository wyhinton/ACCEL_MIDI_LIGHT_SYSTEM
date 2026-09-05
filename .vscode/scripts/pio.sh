#!/usr/bin/env bash
# Locates the PlatformIO CLI and runs it with the given arguments.
# Works whether pio is on PATH or only installed via the PlatformIO IDE extension.
set -e

if command -v pio >/dev/null 2>&1; then
  PIO="pio"
elif [ -x "$HOME/.platformio/penv/bin/pio" ]; then
  PIO="$HOME/.platformio/penv/bin/pio"
else
  echo "Error: could not find the PlatformIO 'pio' executable." >&2
  echo "Install PlatformIO Core, or the PlatformIO IDE VS Code extension." >&2
  exit 1
fi

exec "$PIO" "$@"
