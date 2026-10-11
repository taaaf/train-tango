#pragma once
#include "game.h"

// Every message the board sends a phone is packed here, and ONLY here. The firmware and the
// host dev server (tools/) both compile this file, so the wire format cannot drift between
// them -- that is the whole reason it is not in esp32.ino any more. See tools/README.md.
//
// Nothing in here touches WiFi, websockets or Arduino: each builder fills gProtoBuf and
// returns whether it fit. Delivery -- heap guards, queue limits, which socket -- stays with
// whoever called it.
//
// THREADING: these share one buffer and are not reentrant. On the firmware every caller is
// gameStep(), which is also the only owner of the sim and the phone table. Keep it that way;
// calling a builder from the AsyncTCP task would corrupt a message mid-write and race the
// sim state it reads.

extern char gProtoBuf[16384];
extern int gProtoLen;

// Layout blob: the largest message in the protocol, sent once per phone per match and again
// on every reload. `slot` selects whose tile and edges are included.
bool buildLayoutMsg(int slot, bool placing, bool placed);

// Per-node drift parameters, sent once alongside the layout.
bool buildDriftMsg();

// The two hot broadcasts. wallMs is what millis() returns on the board; the builders convert
// to sim time themselves, so a paused game freezes without any extra signalling.
bool buildTrainsMsg(uint32_t wallMs, uint32_t tickCount);
bool buildWorldMsg(uint32_t wallMs);
