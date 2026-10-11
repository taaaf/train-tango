#include "protocol.h"

#include <cstdarg>
#include <cstdio>

// See protocol.h. Everything below moved verbatim out of esp32.ino; the only edits were to
// cut the socket lookup off the front of the two blob builders and to take millis() and
// tickCount as arguments, because neither exists off the board.

char gProtoBuf[16384];
int gProtoLen;

// tbuf/tpos keep their old names so the moved bodies read exactly as they did in esp32.ino.
#define tbuf gProtoBuf
#define tpos gProtoLen
static bool tovf;

static void tap(const char *fmt, ...) {
  if (tpos >= (int)sizeof(gProtoBuf) - 1) { tovf = true; return; }
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tbuf + tpos, sizeof(gProtoBuf) - tpos, fmt, ap);
  va_end(ap);
  if (n > 0) tpos += n;
  if (tpos >= (int)sizeof(gProtoBuf)) { tpos = sizeof(gProtoBuf) - 1; tovf = true; }
}

static const char *sideStr(int s) {
  switch (s) { case SIDE_LEFT: return "left"; case SIDE_RIGHT: return "right"; case SIDE_TOP: return "top"; default: return "bottom"; }
}


bool buildLayoutMsg(int slot, bool placing, bool placed) {
  tpos = 0;
  tovf = false;
  tap("{\"type\":\"layout\",\"placing\":%s,\"placed\":%s,\"board\":{\"w\":%.1f,\"h\":%.1f},",
      placing ? "true" : "false", placed ? "true" : "false",
      gLayout.boardW, gLayout.boardH);

  int youTile = -1;
  for (int i = 0; i < gLayout.nTiles; i++)
    if (gLayout.tiles[i].phone == slot) youTile = i;
  if (youTile >= 0) {
    Tile &t = gLayout.tiles[youTile];
    // "slot" lets the phone tell which powerups were dealt to it; gPU[].owner is a slot index.
    tap("\"you\":{\"slot\":%d,\"orientation\":\"%s\",\"x\":%.1f,\"y\":%.1f,\"w\":%.1f,\"h\":%.1f},",
        slot, t.orientation == 0 ? "landscape" : "portrait", t.x, t.y, t.w, t.h);
  }

  tap("\"edges\":[");
  bool firstEdge = true;
  for (int e = 0; e < gLayout.nEdges; e++) {
    Edge &ed = gLayout.edges[e];
    if (ed.tile != youTile) continue;
    tap("%s{\"side\":\"%s\",\"start\":%.1f,\"end\":%.1f,\"hue\":%d}",
        firstEdge ? "" : ",", sideStr(ed.side), ed.start, ed.end, gLayout.pairHue[ed.colorIdx]);
    firstEdge = false;
  }
  tap("],");

  // "N" is the main-ring node/link count: nodes below it are on the loop itself, above it
  // are spur and tunnel interiors. The client needs it to keep extra-train placement on
  // the ring.
  tap("\"rail\":{\"carR\":11,\"colors\":%d,\"maxQ\":%d,\"N\":%d,\"nodes\":[", STATION_COLORS, MAX_QUEUE, gRail.N);
  for (int i = 0; i < gRail.nNodes; i++)
    tap("%s%d,%d", i ? "," : "", (int)lroundf(gRail.nodes[i].x), (int)lroundf(gRail.nodes[i].y));
  tap("],\"links\":[");
  for (int i = 0; i < gRail.nLinks; i++)
    tap("%s%d,%d", i ? "," : "", gRail.links[i].a, gRail.links[i].b);
  tap("],\"stations\":[");
  for (int i = 0; i < gRail.nStations; i++) {
    Station &st = gRail.stations[i];
    tap("%s{\"n0\":%d,\"n1\":%d,\"nx\":%.3f,\"ny\":%.3f,\"colorIdx\":%d,\"open\":%s}",
        i ? "," : "", st.a, st.b, st.nx, st.ny, st.colorIdx, st.open ? "true" : "false");
  }
  tap("],\"junctions\":[");
  for (int i = 0; i < gRail.nJunctions; i++) {
    Junction &j = gRail.junctions[i];
    tap("%s{\"node\":%d,\"exit\":%d,\"straightTo\":%d,\"mIn\":%d,\"mOut\":%d,"
        "\"inDir\":[%.3f,%.3f],\"outDir\":[%.3f,%.3f],\"active\":%d,\"incoming\":%s}",
        i ? "," : "", j.node, j.exit, j.straightTo, j.mIn, j.mOut,
        j.inDirX, j.inDirY, j.outDirX, j.outDirY, (int)j.active, j.incoming ? "true" : "false");
  }
  tap("],\"switches\":[");
  for (int i = 0; i < gRail.nSwitches; i++) {
    Switch &sw = gRail.switches[i];
    // "exit"/"outDir" are the far-end pair the client draws the lever against once the
    // board is reversed, matching the junction payload above.
    tap("%s{\"node\":%d,\"exit\":%d,\"inDir\":[%.3f,%.3f],\"outDir\":[%.3f,%.3f],"
        "\"station\":%d,\"entryLink\":%d,\"straightLinks\":[",
        i ? "," : "", sw.node, sw.exit, sw.inDirX, sw.inDirY, sw.outDirX, sw.outDirY,
        sw.station, sw.entryLink);
    for (int k = 0; k < sw.nStraight; k++) tap("%s%d", k ? "," : "", sw.straightLinks[k]);
    tap("],\"active\":%d}", (int)sw.active);
  }
  tap("],\"boosts\":[");
  for (int i = 0; i < gRail.nBoosts; i++)
    tap("%s{\"node\":%d,\"armed\":%s}", i ? "," : "", gRail.boosts[i].node,
        gRail.boosts[i].armed ? "true" : "false");
  tap("],\"hidden\":[");
  bool firstHidden = true;
  for (int i = 0; i < gRail.nLinks; i++)
    if (gRail.hidden[i]) { tap("%s%d", firstHidden ? "" : ",", i); firstHidden = false; }
  tap("],\"teleport\":[");
  bool firstTele = true;
  for (int i = 0; i < gRail.nLinks; i++)
    if (gRail.teleport[i]) { tap("%s%d", firstTele ? "" : ",", i); firstTele = false; }
  tap("]}}");

  return !tovf;
}

bool buildDriftMsg() {
  tpos = 0;
  tovf = false;
  // D|startMs|node,amp,dnx,dny,fx,fy,px,py;...  one record per drifting node.
  // Decoder: parseDrift() in client.js.
  //
  // fx/fy are x1000, the rest x100, and that asymmetry is load-bearing: fx/fy are
  // frequencies, so error in them INTEGRATES into a phase difference between the client's
  // rendered nodes and the full-precision ones simTick paces trains against. x1000 matches
  // the %.3f it replaced; x100 would allow them to drift fully out of phase. Do not
  // coarsen fx/fy. See TRIMMED.md §2b.
  tap("D|%lu|", (unsigned long)simGameStartMs());
  bool first = true;
  for (int i = 0; i < gRail.nNodes; i++) {
    Node &n = gRail.nodes[i];
    if (!n.drift) continue;
    tap("%s%d,%d,%d,%d,%d,%d,%d,%d", first ? "" : ";", i,
        (int)lroundf(n.amp * 100.0f), (int)lroundf(n.dnx * 100.0f), (int)lroundf(n.dny * 100.0f),
        (int)lroundf(n.fx * 1000.0f), (int)lroundf(n.fy * 1000.0f),
        (int)lroundf(n.px * 100.0f), (int)lroundf(n.py * 100.0f));
    first = false;
  }

  return !tovf;
}

bool buildTrainsMsg(uint32_t wallMs, uint32_t tickCount) {
  tpos = 0;
  tovf = false;
  // Sim time, not wall: the "t" field drives the client's playout clock, so freezing it
  // during a pause freezes the trains on every phone without any extra signalling.
  uint32_t now = simClock(wallMs);
  // rev rides the seq-tracked snapshot because it changes the client's motion prediction;
  // powerup state goes on "world" instead, to keep this message lean.
  tap("T|%lu|%lu|%ld|%ld|%d|", (unsigned long)tickCount, (unsigned long)now, gScore, gTotalScore,
      gDirRev ? 1 : 0);
  for (int i = 0; i < gNTrains; i++) {
    Train &tr = gTrains[i];
    int bo = now < tr.boostUntil
                 ? (int)lroundf((float)(tr.boostUntil - now) * 100.0f / (float)CFG_BOOST_DURATION_MS)
                 : 0;
    int flags = (tr.full ? 1 : 0) | (tr.stopped ? 2 : 0) | (tr.derailedFlag ? 4 : 0);
    tap("%s%d,%d,%d,%d,%d,%d,%d,%d", i ? ";" : "", tr.li, tr.to, (int)lroundf(tr.prog),
        (int)lroundf(tr.vsend), (int)lroundf(tr.gap), tr.color, flags, bo);
    for (int k = 0; k < tr.nCars; k++) tap(",%d", tr.cars[k]);
  }
  // Points scored per phone slot since the last send, for the client's "+N" marker. Cleared
  // only once the whole message fit: on overflow nothing is sent, and the events have to
  // survive to ride the next one.
  long sc[MAX_DEVICES];
  simPeekScoreEvents(sc);
  tap("|");
  for (int i = 0; i < MAX_DEVICES; i++) tap("%s%ld", i ? "," : "", sc[i]);
  if (!tovf) simClearScoreEvents();
  return !tovf;
}

// Compact positional text, same rationale as buildTrains(). Sections are pipe-separated and
// fixed in order; records within a section are ";"-separated, fields ",".
//
//   W|chaos|stations|switches|junctions|boosts|bonus|frz|pu
//     chaos    integer percent
//     station  open,q0,q1,...            (queue colours, may be empty)
//     switch   active                    (index IS the id, so no id field)
//     junction active,incoming
//     boost    armed,cd                  (cd = integer percent of cooldown remaining)
//     bonus    color,phase,t,left        (empty section when no bonus is running)
//     frz      comma-separated station ids
//     pu       lv,own,arm,act            one per powerup, in PowerupId order
//                                        act = fuel left in HUNDREDTHS OF ONE TIER, so it
//                                        runs 0..lv*100 (see updatePowerups)
//
// `cost` is NOT sent: it is CFG_PU_COST, a compile-time constant. client.js mirrors the
// table as PU_COSTS and derives it from lv -- the two must stay in step.
//
// Decoder: parseWorld() in client.js. Field ORDER is the contract.
bool buildWorldMsg(uint32_t wallMs) {
  tpos = 0;
  tovf = false;
  // Sim time: every deadline compared against it below (boost cooldowns, bonus, powerup
  // windows) was set in sim time and must stop ticking while the game is paused.
  uint32_t now = simClock(wallMs);

  tap("W|%d|", (int)lroundf(gChaos * 100.0f));
  for (int i = 0; i < gRail.nStations; i++) {
    Station &st = gRail.stations[i];
    tap("%s%d", i ? ";" : "", st.open ? 1 : 0);
    for (int q = 0; q < st.qn; q++) tap(",%d", st.queue[q]);
  }
  tap("|");
  for (int i = 0; i < gRail.nSwitches; i++) tap("%s%d", i ? ";" : "", gRail.switches[i].active);
  tap("|");
  for (int i = 0; i < gRail.nJunctions; i++)
    tap("%s%d,%d", i ? ";" : "", gRail.junctions[i].active, gRail.junctions[i].incoming ? 1 : 0);
  tap("|");
  for (int i = 0; i < gRail.nBoosts; i++) {
    Boost &b = gRail.boosts[i];
    int cd = b.cooldownUntil > now
                 ? (int)lroundf((float)(b.cooldownUntil - now) * 100.0f / (float)CFG_BOOST_COOLDOWN_MS)
                 : 0;
    if (cd > 100) cd = 100;
    tap("%s%d,%d", i ? ";" : "", b.armed ? 1 : 0, cd);
  }
  tap("|");
  if (gBonusPhase) {
    int bt = (int)lroundf((float)(now - gBonusChargeStart) * 100.0f / (float)CFG_BONUS_CHARGE_MS);
    if (bt > 100) bt = 100;
    long bleft = (long)gBonusChargeStart + CFG_BONUS_CHARGE_MS + CFG_BONUS_ACTIVE_MS - (long)now;
    if (bleft < 0) bleft = 0;
    tap("%d,%d,%d,%ld", gBonusColorIdx, gBonusPhase, bt, bleft);
  }
  tap("|");
  for (int k = 0; k < gNFrozen; k++) tap("%s%d", k ? "," : "", gFrozen[k]);
  tap("|");
  for (int i = 0; i < PU_COUNT; i++) {
    Powerup &pu = gPU[i];
    // Fuel, not a window fraction: every tier bought appended its own CFG_PU_TIER_MS, so
    // this is 100 per tier still unburnt and the client lays it straight across the ring's
    // per-tier bands. Capped at the level so a clock rounding up can never overfill one.
    int act = 0;
    if (pu.level > 0 && pu.activeUntil > now)
      act = (int)lroundf((float)(pu.activeUntil - now) * 100.0f / (float)CFG_PU_TIER_MS);
    int cap = pu.level * 100;
    if (act > cap) act = cap;
    tap("%s%d,%d,%d,%d", i ? ";" : "", pu.level, pu.owner, pu.armed ? 1 : 0, act);
  }
  return !tovf;
}
