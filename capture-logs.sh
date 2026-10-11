#!/usr/bin/env bash
set -euo pipefail

# Capture the ESP32's serial log to a timestamped file while you play.
#
#   ./capture-logs.sh              # writes logs/esp32-<date>-<time>.log
#   ./capture-logs.sh baseline     # ...-baseline.log, so runs are easy to tell apart
#
# Ctrl-C to stop. Each run writes a NEW file; logs/latest.log always points at
# the newest one.
#
# Serial only: the board must be on USB. There is no network log path in this
# firmware (nothing serves a log port), so a board on a power bank cannot be read.
#
# Overrides:  PORT=/dev/cu.xxxx   BAUD=115200

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LABEL="${1:-}"
BAUD="${BAUD:-115200}"

case "$LABEL" in
  # Print only the header block, stopping at the first line of real code.
  -h|--help|help)
    awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 && !/^(#|set |$)/ {exit}' "$0"
    exit 0 ;;
esac

# Same detection as flash.sh, so both agree on which board they mean.
if [ -z "${PORT:-}" ]; then
  ports=()
  for p in /dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART*; do
    [ -e "$p" ] && ports+=("$p")
  done
  if [ "${#ports[@]}" -eq 0 ]; then
    echo "No ESP32 serial port found. Plug the board into USB, or set PORT=/dev/cu.xxxx" >&2
    echo "Available ports:" >&2; ls /dev/cu.* >&2 || true
    exit 1
  fi
  if [ "${#ports[@]}" -gt 1 ]; then
    echo "Multiple serial ports found; set PORT=/dev/cu.xxxx to pick one:" >&2
    printf '  %s\n' "${ports[@]}" >&2
    exit 1
  fi
  PORT="${ports[0]}"
fi

mkdir -p "$HERE/logs"
STAMP="$(date +%Y%m%d-%H%M%S)"
LOG="$HERE/logs/esp32-${STAMP}${LABEL:+-$LABEL}.log"
ln -sf "$(basename "$LOG")" "$HERE/logs/latest.log"

echo ">> Port:    $PORT @ ${BAUD} baud"
echo ">> Writing: $LOG"
echo ">>          (also reachable as logs/latest.log)"
echo ">> Play your session now. Ctrl-C when done."
echo

# arduino-cli monitor matches flash.sh; fall back to raw serial if it is absent.
if command -v arduino-cli >/dev/null 2>&1; then
  arduino-cli monitor -p "$PORT" -c baudrate="$BAUD" 2>&1 | tee "$LOG"
else
  stty -f "$PORT" "$BAUD" raw -echo
  cat "$PORT" | tee "$LOG"
fi
