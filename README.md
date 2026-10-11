# Train Tango

This is a work in progress.

A multiplayer train game that runs entirely on an ESP32. The board hosts its own WiFi access point and a small web server; players join from their phones' browsers, each phone becomes a tile of the shared board.

See screenshot and gameplay [here.](https://traintango.com)

## How it works

- The ESP32 runs an access point ("Connect & go to 4.3.2.1 to play") and serves a small
  web app (`esp32/data/`) over HTTP + WebSocket.
- Each connecting phone is assigned a tile; `esp32/layout.cpp` arranges tiles into a
  board and `esp32/rail.cpp` lays track across the seams between them.
- `esp32/sim.cpp` + `esp32/specials.cpp` run the actual game (trains, passengers,
  powerups); `esp32/protocol.cpp` packs that state into the messages sent to clients.
- `esp32/data/client.js` renders the board and train motion in the browser and sends
  input back over the WebSocket.

## Requirements

- An ESP32 dev board, USB cable.
- [`arduino-cli`](https://arduino.github.io/arduino-cli/) with the `esp32` core
  installed (gives you the toolchain, `mklittlefs` and `esptool` that `flash.sh` uses).
- Node.js, to install `esbuild` (`npm install`) — used to minify `client.js`/`style.css`
  before flashing. Optional: without it, `flash.sh` flashes them unminified.
- Optional: Python [`fonttools`](https://github.com/fonttools/fonttools) (gives you
  `pyftsubset`) to re-subset the bundled fonts. The repo ships already-subsetted
  `.woff2.gz` files, so this is only needed if you change the font files or the glyph set
  in `flash.sh`.

## Building & flashing

```
npm install            # pulls in esbuild, used to minify the web assets
./flash.sh             # flash firmware only (game logic)
./flash.sh web         # flash web assets only (esp32/data/)
./flash.sh all         # flash both
./flash.sh build       # just build the .bin files into ./build/, no board needed
./flash.sh monitor     # open the serial monitor
```

`flash.sh` autodetects the board's serial port; override with `PORT=/dev/cu.xxxx` if you
have more than one connected.

## Debugging on hardware

```
./capture-logs.sh              # writes logs/esp32-<date>-<time>.log while you play
./capture-logs.sh baseline     # same, with a label so runs are easy to tell apart
```

Serial only — the firmware doesn't expose a network log path.
