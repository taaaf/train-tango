#!/usr/bin/env bash
set -euo pipefail

autodetect_port() {
  local ports=()
  for p in /dev/cu.usbserial-* /dev/cu.wchusbserial* /dev/cu.usbmodem* /dev/cu.SLAB_USBtoUART*; do
    [ -e "$p" ] && ports+=("$p")
  done
  if [ "${#ports[@]}" -eq 0 ]; then
    echo "No ESP32 serial port found. Plug in the board, or set PORT=/dev/cu.xxxx" >&2
    echo "Available ports:" >&2; ls /dev/cu.* >&2 || true
    exit 1
  fi
  if [ "${#ports[@]}" -gt 1 ]; then
    echo "Multiple serial ports found; set PORT=/dev/cu.xxxx to pick one:" >&2
    printf '  %s\n' "${ports[@]}" >&2
    exit 1
  fi
  echo "${ports[0]}"
}
PORT="${PORT:-}"
PORT_ANNOUNCED=""
need_port() {
  [ -n "$PORT" ] || PORT="$(autodetect_port)"
  [ -n "$PORT_ANNOUNCED" ] || { echo ">> Using port: $PORT"; PORT_ANNOUNCED=1; }
}
UPLOAD_BAUD="${UPLOAD_BAUD:-115200}"
FQBN="esp32:esp32:esp32:UploadSpeed=${UPLOAD_BAUD}"
HERE="$(cd "$(dirname "$0")" && pwd)"
SKETCH="$HERE/esp32"
DATA="$SKETCH/data"

TOOLS="$HOME/Library/Arduino15/packages/esp32/tools"
MKLFS="$(ls "$TOOLS"/mklittlefs/*/mklittlefs | head -1)"
ESPTOOL="$(ls "$TOOLS"/esptool_py/*/esptool | head -1)"
FS_OFFSET=0x290000
FS_SIZE=0x160000

BUILD="${BUILD_DIR:-$HERE/build}"
CORE="$(ls -d "$HOME/Library/Arduino15/packages/esp32/hardware/esp32"/* 2>/dev/null | tail -1)"
BOOT_APP0="$CORE/tools/partitions/boot_app0.bin"

usage() {
  cat <<'EOF'
Usage: ./flash.sh [target]

  (no arg)   flash firmware only  -> game logic + CFG_* params in esp32/game.h
  web        flash web assets only -> esp32/data/ (client.js, index.html, style.css)
  all        flash firmware + web assets
  build      build the .bin files into ./build/ WITHOUT flashing (no board needed)
  flash-bin  flash the merged ./build/train-tango-full.bin (built by the build target)
  monitor    open the serial monitor (Ctrl-C to quit)

Override the port with:  PORT=/dev/cu.xxxx ./flash.sh
Override the output dir: BUILD_DIR=/path ./flash.sh build
EOF
}

flash_fw() {
  need_port
  echo ">> Flashing firmware..."
  arduino-cli compile --upload --fqbn "$FQBN" -p "$PORT" \
    --build-property "compiler.cpp.extra_flags=-DCONFIG_ASYNC_TCP_MAX_ACK_TIME=3000 -DCONFIG_ASYNC_TCP_RUNNING_CORE=0" \
    "$SKETCH"
}

prepare_web() {
  if [ -d "$HERE/public" ]; then
    echo ">> Syncing web assets (client.js, index.html, style.css) from public/..."
    cp "$HERE/public/client.js"  "$DATA/client.js"
    cp "$HERE/public/index.html" "$DATA/index.html"
    cp "$HERE/public/style.css"  "$DATA/style.css"
    cp "$HERE/public/title.svg"  "$DATA/title.svg"
    cp "$HERE/public/cascque-labs.svg" "$DATA/cascque-labs.svg"
    cp "$HERE"/public/powerup-*.svg "$DATA"/
    cp "$HERE"/public/HostGrotesk-SemiBold.woff2 "$DATA"/
  else
    echo ">> No public/ in this repo — using esp32/data/ as the web asset source."
  fi
  local ESB PYS
  # Probe with --version, not -x: .buildtools/bin/pyftsubset is a venv shim whose
  # shebang can point at an interpreter that no longer exists.
  ESB="$HERE/node_modules/.bin/esbuild"
  "$ESB" --version >/dev/null 2>&1 || ESB="$(command -v esbuild 2>/dev/null || true)"
  [ -n "$ESB" ] && { "$ESB" --version >/dev/null 2>&1 || ESB=""; }
  PYS="$HERE/.buildtools/bin/pyftsubset"
  "$PYS" --help >/dev/null 2>&1 || PYS="$(command -v pyftsubset 2>/dev/null || true)"
  [ -n "$PYS" ] && { "$PYS" --help >/dev/null 2>&1 || PYS=""; }
  local GLYPHS="U+0020-007E,U+00A0,U+00E8,U+00D7,U+2013,U+2014,U+2018-201F,U+2022,U+2026,U+2039,U+203A,U+2190,U+2192"

  local opt; opt="$(mktemp -d)"
  # Keep the committed .gz around: without the subsetting/minifying tools we reuse
  # them rather than regenerating bigger ones from the full sources.
  local keep; keep="$(mktemp -d)"
  cp "$DATA"/*.gz "$keep"/ 2>/dev/null || true
  rm -f "$DATA"/*.gz

  if [ -n "$ESB" ]; then
    echo "   minifying client.js + style.css (esbuild)"
    "$ESB" "$DATA/client.js" --minify --legal-comments=none > "$opt/client.js"
    "$ESB" "$DATA/style.css" --minify                       > "$opt/style.css"
  else
    echo "   !! esbuild not found — client.js/style.css will be flashed UN-MINIFIED"
    echo "      (client.js.gz ~25 KB instead of ~20 KB; fix with: npm i -D esbuild)"
    cp "$DATA/client.js" "$DATA/style.css" "$opt"/
  fi

  if [ -n "$PYS" ]; then
    echo "   subsetting fonts (pyftsubset)"
    for f in HostGrotesk-SemiBold; do
      "$PYS" "$DATA/$f.woff2" --unicodes="$GLYPHS" --flavor=woff2 \
        --output-file="$opt/$f.woff2" --layout-features='*' --no-hinting --desubroutinize
    done
  else
    echo "   (pyftsubset not found — reusing the existing subsetted font .gz)"
  fi

  for f in client.js style.css HostGrotesk-SemiBold.woff2; do
    if [ -f "$opt/$f" ]; then
      gzip -9 -c "$opt/$f" > "$DATA/$f.gz"
    elif [ -f "$keep/$f.gz" ]; then
      cp "$keep/$f.gz" "$DATA/$f.gz"
    else
      echo "!! no source and no existing .gz for $f" >&2; rm -rf "$opt" "$keep"; exit 1
    fi
  done
  rm -rf "$keep"
  for f in index.html title.svg cascque-labs.svg powerup-speed-dropoff.svg powerup-add-car.svg powerup-add-train.svg powerup-switch-direction.svg powerup-freeze.svg \
           icon-player-1.svg icon-player-2.svg icon-player-3.svg icon-player-4.svg; do
    gzip -9 -c "$DATA/$f" > "$DATA/$f.gz"
  done
  rm -rf "$opt"
  # The music subsystem was removed (see TRIMMED.md); its patches are no longer read by the
  # firmware, and leaving them in LittleFS both wasted ~39 KB and left them HTTP-reachable
  # through serveStatic. The sources stay in music/ for a future rebuild.
  rm -f "$DATA"/*.json
}

make_fs_image() {
  local img="$1"
  echo ">> Building LittleFS image -> $img"
  local stage; stage="$(mktemp -d)"
  # .gz only: the "/" handler prefers index.html.gz and serveStatic resolves the rest from
  # their .gz, so staging raw copies alongside just spent LittleFS space.
  cp "$DATA"/*.gz "$stage"/
  "$MKLFS" -c "$stage" -s "$FS_SIZE" "$img"
  rm -rf "$stage"
}

flash_web() {
  need_port
  prepare_web
  make_fs_image "/tmp/littlefs.bin"
  echo ">> Flashing web assets (LittleFS)..."
  "$ESPTOOL" --chip esp32 -p "$PORT" -b "$UPLOAD_BAUD" write-flash "$FS_OFFSET" "/tmp/littlefs.bin"
}

build_bins() {
  mkdir -p "$BUILD"
  echo ">> Compiling firmware -> $BUILD"
  arduino-cli compile --fqbn "$FQBN" --output-dir "$BUILD" \
    --build-property "compiler.cpp.extra_flags=-DCONFIG_ASYNC_TCP_MAX_ACK_TIME=3000 -DCONFIG_ASYNC_TCP_RUNNING_CORE=0" \
    "$SKETCH"
  rm -f "$BUILD/esp32.ino.merged.bin"
  prepare_web
  make_fs_image "$BUILD/littlefs.bin"
  [ -f "$BOOT_APP0" ] || { echo "boot_app0.bin not found at $BOOT_APP0" >&2; exit 1; }
  cp "$BOOT_APP0" "$BUILD/boot_app0.bin"
  echo ">> Merging -> $BUILD/train-tango-full.bin"
  "$ESPTOOL" --chip esp32 merge-bin -o "$BUILD/train-tango-full.bin" \
    --flash-mode keep --flash-freq keep --flash-size keep \
    0x1000    "$BUILD/esp32.ino.bootloader.bin" \
    0x8000    "$BUILD/esp32.ino.partitions.bin" \
    0xe000    "$BUILD/boot_app0.bin" \
    0x10000   "$BUILD/esp32.ino.bin" \
    "$FS_OFFSET" "$BUILD/littlefs.bin"
  cat > "$BUILD/FLASH.txt" <<TXT
Train Tango — ESP32 flash instructions (esp32 4MB, generated $(date +%Y-%m-%d))

One-shot (everything, erases the board's flash layout in these regions):
  esptool --chip esp32 -p <PORT> -b 115200 write-flash 0x0 train-tango-full.bin

Or piece by piece (same result):
  esptool --chip esp32 -p <PORT> -b 115200 write-flash \\
    0x1000 esp32.ino.bootloader.bin \\
    0x8000 esp32.ino.partitions.bin \\
    0xe000 boot_app0.bin \\
    0x10000 esp32.ino.bin \\
    $FS_OFFSET littlefs.bin

Web assets (LittleFS) only, keeping the firmware:
  esptool --chip esp32 -p <PORT> -b 115200 write-flash $FS_OFFSET littlefs.bin

<PORT> is e.g. /dev/cu.usbserial-XXXX (macOS), /dev/ttyUSB0 (Linux), COM5 (Windows).
Use -b 115200: this board's USB drops out at the default 921600.
Older esptool (v4) spells the command "write_flash" instead of "write-flash".
TXT
  echo ">> Artefacts in $BUILD:"
  ( cd "$BUILD" && ls -lh *.bin | awk '{printf "   %-28s %s\n", $NF, $5}' )
  echo ">> Flash them with: ./flash.sh flash-bin   (or see $BUILD/FLASH.txt)"
}

flash_bin() {
  need_port
  local img="$BUILD/train-tango-full.bin"
  [ -f "$img" ] || { echo "$img not found — run ./flash.sh build first" >&2; exit 1; }
  echo ">> Flashing merged image $img"
  "$ESPTOOL" --chip esp32 -p "$PORT" -b "$UPLOAD_BAUD" write-flash 0x0 "$img"
}

case "${1:-fw}" in
  fw)        flash_fw ;;
  web)       flash_web ;;
  all)       flash_fw; flash_web ;;
  build)     build_bins ;;
  flash-bin) flash_bin ;;
  monitor)   need_port; arduino-cli monitor -p "$PORT" -c baudrate=115200 ;;
  -h|--help|help) usage ;;
  *) usage; exit 1 ;;
esac

echo ">> Done."
