#include "game.h"

Train *gTrains = nullptr;
int gNTrains = 0;
long gScore = 0;
long gTotalScore = 0;
Powerup gPU[PU_COUNT];
uint8_t gDirRev = 0;
float gChaos = 0;
bool gGameOver = false;
int gBonusColorIdx = -1;
int gBonusPhase = 0;
uint32_t gBonusChargeStart = 0;
static uint32_t gBonusNextAt = 0;

int gFrozen[MAX_FROZEN] = { -1, -1, -1 };
int gNFrozen = 0;

bool stationFrozen(int s) {
  for (int k = 0; k < gNFrozen; k++) if (gFrozen[k] == s) return true;
  return false;
}

static uint32_t gStartMs = 0;
static uint32_t gLastMs = 0;
static uint32_t gNextSpawn = 0;
static int gDeviceCount = 1;

// Train count the match started with. Trains are only ever appended and popped at the
// tail, because the client keys its per-train interpolation buffers by array index --
// shifting the array would re-index every train and snap them all across the screen.
static int gBaseNTrains = 0;

// ---------------------------------------------------------------------------
// Sim clock
//
// While a powerup waits for a target the whole game holds still, so nobody loses ground
// deciding where to put it. Rather than shifting every deadline (spawn, bonus, overcrowd,
// cooldowns, derail, boost, drift phase) this simply stops sim time: every timer in here
// is expressed against it, so they all freeze together and resume exactly where they were.
// Wall time is still used for the network layer, which must keep running.
// ---------------------------------------------------------------------------
static uint32_t gPauseAccum = 0;  // total wall ms spent paused
static uint32_t gPauseStart = 0;  // wall ms the current pause began
static bool gPausedNow = false;

bool simAwaitingTarget() {
  for (int k = 0; k < PU_COUNT; k++)
    if (gPU[k].armed) return true;
  return false;
}

// Safe to call any number of times per millisecond, and from a socket callback as well as
// the tick loop: it only acts on a pause/resume transition.
uint32_t simClock(uint32_t wall) {
  bool p = simAwaitingTarget();
  if (p && !gPausedNow) {
    gPauseStart = wall;
    gPausedNow = true;
  } else if (!p && gPausedNow) {
    gPauseAccum += wall - gPauseStart;
    gPausedNow = false;
    gPauseStart = 0;
  }
  return wall - gPauseAccum - (gPausedNow ? wall - gPauseStart : 0);
}

struct XY { float x, y; };
static XY gPos[MAX_NODES];
static float gSeg[MAX_LINKS];

uint32_t simGameStartMs() { return gStartMs; }
float simNodeX(int i) { return gPos[i].x; }
float simNodeY(int i) { return gPos[i].y; }

static int carsPerTrain() { return CFG_BASE_CARS + gPU[PU_CARS].level; }

// Speed rides gChaos, capped once chaos reaches 1 so the ceiling stays at the 200 px/s the
// client's extrapolation budget is sized against (LAG-REPORT §1.4).
static float trainSpeed() {
  float c = fminf(gChaos, CFG_SPEED_CHAOS_CAP);
  return CFG_TRAIN_SPEED + (CFG_TRAIN_SPEED_MAX - CFG_TRAIN_SPEED) * c;
}

static float serviceMult() { return CFG_PU_BOARD_MULT[gPU[PU_BOARD].level]; }

static void makeTrain(Train &tr, int startLi) {
  int N = gRail.N;
  int li = ((startLi % N) + N) % N;
  tr.li = li;
  // Ring link i runs node i -> node i+1, so a counter-clockwise board wants `to` at the
  // link's own index. Spawning a clockwise train into a reversed board put it nose-to-nose
  // with every other train, and the follow scan derails both of them forever.
  tr.to = gDirRev ? li : (li + 1) % N;
  tr.prog = 0;
  tr.stopped = 0;
  tr.station = -1;
  tr.svcActive = 0;
  tr.svcPhase = 0;
  tr.svcLast = 0;
  tr.nearLead = 0;
  tr.derailUntil = 0;
  tr.color = -1;
  tr.full = 0;
  tr.boostUntil = 0;
  tr.ghostUntil = 0;
  tr.tunnelJct = -1;
  tr.gap = CFG_CAR_SPACING;
  tr.hasPrev = 0;
  tr.nCars = carsPerTrain();
  for (int c = 0; c < tr.nCars; c++) tr.cars[c] = -1;
  tr.derailedFlag = 0;
}

struct SpawnEvent { uint32_t at, every, durationMs, fadeMs; float mult; const char *label; };
static const SpawnEvent SPAWN_EVENTS[] = {
  {      0, 0,          0, 0,    1.0f, "" },
};
static const int N_SPAWN_EVENTS = sizeof(SPAWN_EVENTS) / sizeof(SPAWN_EVENTS[0]);

static float spawnRateMult(uint32_t elapsedMs) {
  float m = 1.0f;
  for (int i = 0; i < N_SPAWN_EVENTS; i++) {
    const SpawnEvent &e = SPAWN_EVENTS[i];
    if (elapsedMs < e.at) continue;
    uint32_t local = elapsedMs - e.at;
    if (e.every > 0) local %= e.every;
    if (local >= e.durationMs) continue;
    float into = e.fadeMs > 0 ? fminf(1.0f, (float)local / e.fadeMs) : 1.0f;
    float outOf = e.fadeMs > 0 ? fminf(1.0f, (float)(e.durationMs - local) / e.fadeMs) : 1.0f;
    float k = fminf(into, outOf);
    k = k * k * (3 - 2 * k);
    m *= 1 + (e.mult - 1) * k;
  }
  return m;
}

static float nextSpawnDelay(uint32_t elapsedMs) {
  int s = gRail.nStations > 0 ? gRail.nStations : 1;
  float rate = (1 + gChaos * CFG_SPAWN_CHAOS) * spawnRateMult(elapsedMs);
  int d = gDeviceCount < 2 ? 2 : (gDeviceCount > 4 ? 4 : gDeviceCount);
  float base = CFG_SPAWN_MS_BY_DEV[d - 2] / rate / s;

  float f = 1 + (gameRandom() * 2 - 1) * CFG_SPAWN_VAR;
  return base * f;
}

static void computeDriftNormals() {
  Rail &R = gRail;
  int N = R.N;
  for (int i = 0; i < N; i++) {
    Node &p = R.nodes[(i - 1 + N) % N];
    Node &n = R.nodes[(i + 1) % N];
    float dx = n.x - p.x, dy = n.y - p.y;
    float L = hypotf(dx, dy); if (L < 1e-6f) L = 1;
    R.nodes[i].dnx = -dy / L;
    R.nodes[i].dny = dx / L;
  }
}

static long gScoreEvents[MAX_DEVICES];

void simPeekScoreEvents(long *out) {
  for (int i = 0; i < MAX_DEVICES; i++) out[i] = gScoreEvents[i];
}

void simClearScoreEvents() {
  for (int i = 0; i < MAX_DEVICES; i++) gScoreEvents[i] = 0;
}

void simInit(int deviceCount, uint32_t now) {
  gDeviceCount = deviceCount;
  gScore = 0;
  gTotalScore = 0;
  simClearScoreEvents();
  for (int k = 0; k < PU_COUNT; k++) {
    gPU[k].level = 0;
    gPU[k].activeUntil = 0;
    gPU[k].armed = 0;
    gPU[k].armWallUntil = 0;
    // owner is dealt by simDealPowerups at placement, not cleared here
  }
  gDirRev = 0;
  gChaos = 0;
  gGameOver = false;
  gStartMs = now;
  gLastMs = now;

  int n = deviceCount + CFG_EXTRA_TRAINS;
  if (n < 1) n = 1;
  if (n > MAX_TRAINS) n = MAX_TRAINS;
  gNTrains = n;
  gBaseNTrains = n;
  for (int j = 0; j < n; j++) makeTrain(gTrains[j], (int)lroundf((float)j * gRail.N / n));

  for (int i = 0; i < gRail.nStations; i++) {
    gRail.stations[i].fullSince = 0;
    gRail.stations[i].clearBy = -1;
    gRail.stations[i].clearDist = 0;
  }
  gNextSpawn = now + (uint32_t)nextSpawnDelay(0);
  gBonusColorIdx = -1;
  gBonusPhase = 0;
  gBonusChargeStart = 0;
  gBonusNextAt = now + CFG_BONUS_FIRST_MS;
  gNFrozen = 0;
  for (int k = 0; k < MAX_FROZEN; k++) gFrozen[k] = -1;
  computeDriftNormals();
}

static void updateBonus(uint32_t now) {
  Rail &R = gRail;
  if (gBonusPhase == 0) {
    if (now >= gBonusNextAt && R.nStations > 0) {
      int colors[MAX_STATIONS], nc = 0;
      for (int s = 0; s < R.nStations; s++) {
        int c = R.stations[s].colorIdx;
        bool seen = false;
        for (int k = 0; k < nc; k++) if (colors[k] == c) { seen = true; break; }
        if (!seen) colors[nc++] = c;
      }
      if (nc == 0) return;
      int choices[MAX_STATIONS], nch = 0;
      for (int k = 0; k < nc; k++) if (colors[k] != gBonusColorIdx) choices[nch++] = colors[k];
      if (nch == 0) for (int k = 0; k < nc; k++) choices[nch++] = colors[k];
      gBonusColorIdx = choices[(int)(gameRandom() * nch)];
      gBonusPhase = 1;
      gBonusChargeStart = now;
      gBonusNextAt = now + CFG_BONUS_PERIOD_MS;
    }
  } else if (gBonusPhase == 1) {
    if (now - gBonusChargeStart >= CFG_BONUS_CHARGE_MS) gBonusPhase = 2;
  } else if (now - gBonusChargeStart >= CFG_BONUS_CHARGE_MS + CFG_BONUS_ACTIVE_MS) {
    gBonusPhase = 0;
  }
}

static bool isIntangible(const Train &tr, uint32_t now) {
  return now < tr.ghostUntil;
}

// A train is full when every boardable car carries someone (car 0 is the loco and never
// boards). Worth 2x on delivery, so it must be RECOMPUTED -- not merely cleared -- every
// time the car count moves: a 4-car train holding 3 was not full, the same 3 in a train
// trimmed back to 3 cars is.
static void recomputeFull(Train &tr) {
  uint8_t full = tr.nCars > 1 ? 1 : 0;
  for (int c = 1; c < tr.nCars; c++) if (tr.cars[c] == -1) { full = 0; break; }
  tr.full = full;
}

static bool serviceTrain(Train &tr, uint32_t now) {
  if (!tr.stopped) { tr.svcActive = 0; return true; }
  Station &st = gRail.stations[tr.station];
  if (st.open) return true;
  if (!tr.svcActive) { tr.svcActive = 1; tr.svcPhase = 0; tr.svcLast = now; }

  if (tr.svcPhase == 0) {
    if (now - tr.svcLast >= (uint32_t)(CFG_ALIGHT_MS / serviceMult())) {
      int idx = -1;
      for (int c = 0; c < tr.nCars; c++) if (tr.cars[c] == st.colorIdx) { idx = c; break; }
      if (idx >= 0) {
        tr.cars[idx] = -1;
        int mult = ((gBonusPhase == 2 && st.colorIdx == gBonusColorIdx) ? 2 : 1) * (tr.full ? 2 : 1);
        gScore += CFG_DELIVERY_POINTS * mult;
        gTotalScore += CFG_DELIVERY_POINTS * mult;
        int owner = gRail.nodes[st.a].owner;
        if (owner >= 0 && owner < MAX_DEVICES) gScoreEvents[owner] += CFG_DELIVERY_POINTS * mult;
        bool allNull = true;
        for (int c = 0; c < tr.nCars; c++) if (tr.cars[c] != -1) allNull = false;
        if (allNull) { tr.color = -1; tr.full = 0; }
        tr.svcLast = now;
      } else {
        tr.svcPhase = 1;
        tr.svcLast = now;
      }
    }
    return false;
  }
  if (now - tr.svcLast >= (uint32_t)(CFG_BOARD_MS / serviceMult())) {
    int empty = -1;
    for (int c = 1; c < tr.nCars; c++) if (tr.cars[c] == -1) { empty = c; break; }
    int qi = -1;
    if (empty >= 0) {
      if (tr.color == -1) qi = st.qn > 0 ? 0 : -1;
      else { for (int q = 0; q < st.qn; q++) if (st.queue[q] == tr.color) { qi = q; break; } }
    }
    if (qi >= 0) {
      int color = st.queue[qi];
      for (int q = qi; q < st.qn - 1; q++) st.queue[q] = st.queue[q + 1];
      st.qn--;
      tr.cars[empty] = color;
      if (tr.color == -1) tr.color = color;
      recomputeFull(tr);
    }
    tr.svcLast = now;
  }
  return false;
}

// tr.prog is ALWAYS the distance from links[li].a, in both directions; tr.to says which
// endpoint the train is heading for. That keeps head positions direction-agnostic (see
// headState) and makes a reversal a matter of swapping `to`, leaving prog untouched.
static inline bool towardB(const Train &tr) { return tr.to == gRail.links[tr.li].b; }

// Distance still to cover on the current link.
static inline float remainingOnLink(const Train &tr) {
  return towardB(tr) ? gSeg[tr.li] - tr.prog : tr.prog;
}

// Entering a link at its far end means prog starts at the far end, not 0.
static void setLink(Train &tr, int li, int to) {
  tr.li = li;
  tr.to = to;
  tr.prog = (to == gRail.links[li].b) ? 0.0f : gSeg[li];
}

struct NextLink { int li, to; };

// Continuation that ignores diversions: the spur/tunnel chain if we are on one, else the
// next link around the main ring. Ring link i runs node i -> node i+1, so stepping back
// from `arrivedNode` means link (arrivedNode - 1), entered at its b end.
static NextLink plainNextLink(int arrivedNode) {
  Rail &R = gRail;
  Node &node = R.nodes[arrivedNode];
  if (gDirRev) {
    if (node.hasBack) return { node.bLi, node.bTo };
    int li = (arrivedNode - 1 + R.N) % R.N;
    return { li, li };
  }
  if (node.hasForward) return { node.fLi, node.fTo };
  return { arrivedNode, (arrivedNode + 1) % R.N };
}

// A reversed train meets a special at its far end, so it diverts off jctOut/swtOut rather
// than jct/swt, and enters the chain from the exit side.
static NextLink routeNextLink(int currentLi, int arrivedNode) {
  Rail &R = gRail;
  Node &node = R.nodes[arrivedNode];
  int jct = gDirRev ? node.jctOut : node.jct;
  int swt = gDirRev ? node.swtOut : node.swt;
  if (jct >= 0 && R.junctions[jct].active == 1 && currentLi < R.N) {
    Junction &j = R.junctions[jct];
    return gDirRev ? NextLink{ j.exitLi, j.exitTo } : NextLink{ j.entryLi, j.entryTo };
  }
  if (swt >= 0 && currentLi < R.N && R.switches[swt].active == 1) {
    Switch &sw = R.switches[swt];
    return gDirRev ? NextLink{ sw.exitLi, sw.exitTo } : NextLink{ sw.entryLi, sw.entryTo };
  }
  return plainNextLink(arrivedNode);
}

// Point every train at the other end of its current link. prog is left alone, so each
// train stays exactly where it is and simply starts heading the other way.
//
// A berthed train is always standing on the platform link (that is the link it arrived on,
// in either direction), and the flip has just moved its berth to the platform's far end --
// see isBerthNode. So release it: it now runs the platform it is sitting on and re-berths
// facing the right way. Leaving it `stopped` instead made the departure route from the
// far node via plainNextLink and jump a whole link, tail-first, out of the station.
static void flipTrainDirections() {
  for (int i = 0; i < gNTrains; i++) {
    Train &tr = gTrains[i];
    const Link &l = gRail.links[tr.li];
    tr.to = (tr.to == l.b) ? l.a : l.b;
    tr.hasPrev = 0;
    if (tr.stopped) {
      tr.stopped = 0;
      tr.station = -1;
      tr.svcActive = 0;  // service restarts at the new end; alighting is idempotent
    }
  }
}

// How far along `li` a train at `prog` sits, measured from whichever end a train heading
// for `to` entered by. Lets the look-ahead scan work unchanged in either direction.
static inline float entryOffset(int li, float prog, int to) {
  return (to == gRail.links[li].b) ? prog : gSeg[li] - prog;
}

// Which end of a platform a train berths at. Cars follow the head, so the berth has
// to be the end the train reaches LAST: st.b running forward, st.a once gDirRev flips the
// board. A fixed berth end left a reversed train hanging off the platform with its tail
// across the spur rejoin, and with nothing but empty track beside the queue.
static inline bool isBerthNode(const Node &node, int idx) {
  if (node.type != 1 || node.st < 0) return false;
  const Station &st = gRail.stations[node.st];
  return idx == (gDirRev ? st.a : st.b);
}

static void armGateClose(Station &st, int ti) {
  st.clearBy = ti;
  st.clearDist = (gTrains[ti].nCars - 1) * CFG_CAR_SPACING + 11.0f;
}

static void advanceTrain(float budget, Train &tr, int ti, bool canLeave, uint32_t now) {
  Rail &R = gRail;
  if (tr.stopped) {
    if (!canLeave) return;
    int leftStation = tr.station;
    tr.stopped = 0;
    tr.station = -1;
    tr.svcActive = 0;
    recomputeFull(tr);
    // Departing a berth follows the plain chain, never a diversion: a train leaving a
    // spur station must not be re-diverted back into the spur it is standing on.
    NextLink out = plainNextLink(tr.to);
    setLink(tr, out.li, out.to);
    if (leftStation >= 0 && leftStation < R.nStations) armGateClose(R.stations[leftStation], ti);
  }
  for (int guard = 0; guard < 2000 && budget > 0; guard++) {
    bool fwd = towardB(tr);
    float remaining = remainingOnLink(tr);
    if (budget < remaining) { tr.prog += fwd ? budget : -budget; break; }
    budget -= fmaxf(remaining, 0);
    int arrived = tr.to;
    Node &node = R.nodes[arrived];

    int jct = gDirRev ? node.jctOut : node.jct;
    int swt = gDirRev ? node.swtOut : node.swt;
    if (jct >= 0 && R.junctions[jct].active == 1 && tr.li < R.N) {
      tr.tunnelJct = jct;
      Junction &j = R.junctions[jct];
      if (gDirRev) setLink(tr, j.exitLi, j.exitTo);
      else setLink(tr, j.entryLi, j.entryTo);
      continue;
    }
    if (swt >= 0 && tr.li < R.N && R.switches[swt].active == 1) {
      Switch &sw = R.switches[swt];
      if (gDirRev) setLink(tr, sw.exitLi, sw.exitTo);
      else setLink(tr, sw.entryLi, sw.entryTo);
      continue;
    }
    if (isBerthNode(node, arrived)) {
      if (!R.stations[node.st].open) {
        tr.prog = fwd ? gSeg[tr.li] : 0.0f;  // berth exactly on `arrived`
        tr.stopped = 1;
        tr.station = node.st;
        return;
      }
      armGateClose(R.stations[node.st], ti);
    }
    if (gDirRev ? node.hasBack : node.hasForward) {
      NextLink nx = plainNextLink(arrived);
      setLink(tr, nx.li, nx.to);
      continue;
    }
    if (node.pad >= 0 && R.boosts[node.pad].armed) {
      R.boosts[node.pad].armed = 0;
      R.boosts[node.pad].cooldownUntil = now + CFG_BOOST_COOLDOWN_MS;
      tr.boostUntil = now + CFG_BOOST_DURATION_MS;
    }
    NextLink nx = plainNextLink(arrived);
    setLink(tr, nx.li, nx.to);
  }
  if (tr.li < R.N) tr.tunnelJct = -1;
}

static void headState(const Train &tr, float &hx, float &hy, float &hdx, float &hdy, bool &hidden) {
  Link &link = gRail.links[tr.li];
  XY a = gPos[link.a], b = gPos[link.b];
  float seg = gSeg[tr.li];
  bool fwd = tr.to == link.b;
  float t = seg > 0 ? fmaxf(0.0f, fminf(tr.prog / seg, 1.0f)) : (fwd ? 1.0f : 0.0f);
  float dx = b.x - a.x, dy = b.y - a.y;
  float L = hypotf(dx, dy); if (L < 1e-6f) L = 1;
  hx = a.x + dx * t; hy = a.y + dy * t;
  // The link is drawn a->b either way; only which way the train faces along it changes.
  hdx = fwd ? dx / L : -dx / L;
  hdy = fwd ? dy / L : -dy / L;
  hidden = gRail.hidden[tr.li] != 0;
}

static void spawnPassengers(uint32_t now) {
  Rail &R = gRail;
  if (R.nStations == 0 || now < gNextSpawn) return;
  gNextSpawn = now + (uint32_t)nextSpawnDelay(now - gStartMs);

  int best = -1;
  float total = 0;
  for (int s = 0; s < R.nStations; s++) {
    if (stationFrozen(s)) continue;
    int qn = R.stations[s].qn;
    if (qn >= MAX_QUEUE) continue;
    float w = (MAX_QUEUE - qn) + CFG_SPAWN_FLATNESS;
    total += w;
    if (gameRandom() * total < w) best = s;
  }
  if (best < 0) return;

  Station &st = R.stations[best];
  int others[MAX_STATIONS], no = 0;
  for (int o = 0; o < R.nStations; o++)
    if (R.stations[o].colorIdx != st.colorIdx) others[no++] = R.stations[o].colorIdx;
  if (no == 0) return;
  st.queue[st.qn++] = others[(int)(gameRandom() * no)];
}

static bool checkOvercrowd(uint32_t now, uint32_t dtMs) {
  Rail &R = gRail;
  for (int s = 0; s < R.nStations; s++) {
    Station &st = R.stations[s];
    if (stationFrozen(s)) {
      if (st.fullSince) st.fullSince += dtMs;
      continue;
    }
    if (st.qn >= MAX_QUEUE) {
      if (!st.fullSince) st.fullSince = now;
      else if (now - st.fullSince >= CFG_OVERCROWD_GRACE_MS) return true;
    } else {
      st.fullSince = 0;
    }
  }
  return false;
}

// A frozen bonus-colour station must not burn bonus time. Unchanged in intent from the
// single-station version, but now any one of the held stations pushes the timers.
static void updateFreeze(uint32_t dtMs) {
  if (gNFrozen <= 0 || gBonusPhase == 0) return;
  for (int k = 0; k < gNFrozen; k++) {
    int s = gFrozen[k];
    if (s < 0 || s >= gRail.nStations) continue;
    if (gRail.stations[s].colorIdx == gBonusColorIdx) {
      gBonusChargeStart += dtMs;
      gBonusNextAt += dtMs;
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// Powerups
//
// One of each per match, dealt to a single phone. Level 1 opens the active window; the
// other tiers cost the same and do NOT extend it. On expiry the level resets to 0 and the
// powerup is buyable again on the very next tick -- there is no cooldown, so score is the
// only thing rationing it. PU_FREEZE and PU_TRAIN arm on purchase and wait for a target
// tap, which may come from ANY phone.
// ---------------------------------------------------------------------------


long puCost(int id) {
  if (id < 0 || id >= PU_COUNT) return -1;
  int lv = gPU[id].level;
  if (lv >= CFG_PU_MAX_LEVEL[id]) return -1;
  return CFG_PU_COST[id][lv];
}

// Grow every train to the current target. Safe to call repeatedly.
static void growCars() {
  int target = carsPerTrain();
  for (int j = 0; j < gNTrains; j++) {
    while (gTrains[j].nCars < target && gTrains[j].nCars < MAX_CARS) gTrains[j].cars[gTrains[j].nCars++] = -1;
    recomputeFull(gTrains[j]);
  }
}

// Shrinking cannot drop passengers, so an oversized train only gives up EMPTY tail cars.
// A train still carrying someone in its rear car keeps that car until they alight, which
// makes the powerup tail off visibly instead of snapping. Called every tick.
static void trimCars() {
  int target = carsPerTrain();
  for (int j = 0; j < gNTrains; j++) {
    Train &tr = gTrains[j];
    bool shrank = false;
    while (tr.nCars > target && tr.cars[tr.nCars - 1] < 0) {
      tr.nCars--;
      shrank = true;
    }
    // Only on an actual trim: recomputing every tick would clear `full` mid-unload at a
    // station, and the whole unload is meant to pay 2x once the train arrived full.
    if (shrank) recomputeFull(tr);
  }
}

static void removeExtraTrains() {
  if (gBaseNTrains > 0 && gNTrains > gBaseNTrains) gNTrains = gBaseNTrains;
}

static void expirePowerup(int id) {
  switch (id) {
    case PU_CARS:
      break;  // trimCars() drains the tail as rear passengers alight
    case PU_FREEZE:
      gNFrozen = 0;
      for (int k = 0; k < MAX_FROZEN; k++) gFrozen[k] = -1;
      break;
    case PU_TRAIN:
      removeExtraTrains();
      break;
    case PU_REVERSE:
      break;  // the flip is permanent until someone buys it again
    default:
      break;
  }
}

// Giving up ONE tier, with the powerup still running at the level below. Only freeze holds
// state that has to be handed back a piece at a time; the others read gPU[].level straight
// out of the clock (boarding) or are drained by their own tick (cars, via trimCars).
static void stepDownPowerup(int id, int newLevel) {
  if (id != PU_FREEZE) return;
  // Thaw oldest-first: the ice melts in the order it was laid, so the station someone
  // rescued most recently is the one that keeps its cover longest.
  while (gNFrozen > newLevel) {
    for (int k = 0; k < gNFrozen - 1; k++) gFrozen[k] = gFrozen[k + 1];
    gFrozen[--gNFrozen] = -1;
  }
}

// The level is not stored, it is read back out of the clock: every tier bought pushed
// activeUntil one CFG_PU_TIER_MS further out, so the tiers burn off one at a time on the
// way down. A powerup bought to level 3 therefore runs 3, then 2, then 1.
static void updatePowerups(uint32_t now) {
  for (int id = 0; id < PU_COUNT; id++) {
    Powerup &pu = gPU[id];
    if (pu.level == 0) continue;
    int32_t left = (int32_t)(pu.activeUntil - now);
    if (left <= 0) {
      expirePowerup(id);
      pu.level = 0;
      pu.armed = 0;
      pu.armWallUntil = 0;
      pu.activeUntil = 0;
      continue;
    }
    // Armed holds sim time still, so `left` cannot move while a target is awaited.
    uint8_t lv = (uint8_t)(((uint32_t)left + CFG_PU_TIER_MS - 1) / CFG_PU_TIER_MS);
    if (lv < pu.level) {
      pu.level = lv;
      stepDownPowerup(id, lv);
    }
  }
}

// An armed powerup holds sim time still for EVERYONE, and its activeUntil is in sim time,
// so updatePowerups can never reach it while armed -- the freeze would be permanent. This
// is the backstop, and it is the only powerup deadline read from the wall clock.
//
// A lapse refunds the level just bought rather than eating the score, because nothing was
// delivered -- and since every tier now appends its own fuel, the refund must take that
// tier's CFG_PU_TIER_MS back out as well, or a cancelled buy would still have bought time.
// Rolling 1 -> 0 clears activeUntil outright so the next buy starts clean.
//
// Note there is deliberately no disarm-on-disconnect: a target may be tapped by ANY phone,
// so the owner leaving does not make the prompt unresolvable. This deadline covers it.
// Disarm and hand back what the buy cost. Shared by the wall deadline above and the
// player-pressed cancel below so the two can never drift into refunding differently.
static void disarmRefund(int id) {
  Powerup &pu = gPU[id];
  pu.armed = 0;
  pu.armWallUntil = 0;
  if (pu.level > 0) {
    gScore += CFG_PU_COST[id][pu.level - 1];
    pu.level--;
    if (pu.level == 0) pu.activeUntil = 0;
    else pu.activeUntil -= CFG_PU_TIER_MS;
  }
}

bool simCheckArmLapse(uint32_t wall) {
  bool lapsed = false;
  for (int id = 0; id < PU_COUNT; id++) {
    Powerup &pu = gPU[id];
    if (!pu.armed) continue;
    if ((int32_t)(wall - pu.armWallUntil) < 0) continue;
    disarmRefund(id);
    lapsed = true;
  }
  return lapsed;
}

// A cancel is a lapse the players asked for early: same disarm, same refund, so the board
// resumes exactly as it would have when the 8 s ran out. Any phone may cancel -- whoever
// is looking at the prompt is as entitled to back out as the buyer, which is already true
// of supplying the target. id < 0 cancels whichever powerup is armed, since only one ever
// is; the caller is a tap on a prompt that names no id of its own.
bool simCancelArmedPowerup(int id) {
  if (id >= PU_COUNT) return false;
  for (int k = 0; k < PU_COUNT; k++) {
    if (id >= 0 && k != id) continue;
    if (!gPU[k].armed) continue;
    disarmRefund(k);
    return true;
  }
  return false;
}

void simBuyPowerup(int id, uint32_t now, uint32_t wall) {
  if (id < 0 || id >= PU_COUNT) return;
  Powerup &pu = gPU[id];
  if (pu.level >= CFG_PU_MAX_LEVEL[id]) return;
  long cost = puCost(id);
  if (cost < 0 || gScore < cost) return;
  gScore -= cost;
  pu.level++;
  // Every tier buys its own fuel and appends it, so a top-up is never wasted and the level
  // the player paid for always gets a full CFG_PU_TIER_MS at the top of the stack.
  if (pu.level == 1) pu.activeUntil = now + CFG_PU_TIER_MS;
  else pu.activeUntil += CFG_PU_TIER_MS;

  switch (id) {
    case PU_CARS:
      growCars();
      break;
    case PU_FREEZE:
    case PU_TRAIN:
      pu.armed = 1;  // wait for a target tap from any phone
      pu.armWallUntil = wall + CFG_PU_ARM_MS;
      break;
    case PU_REVERSE:
      gDirRev = gDirRev ? 0 : 1;
      flipTrainDirections();
      pu.activeUntil = now;  // one-shot: closes on the next tick, then buyable again
      break;
    default:
      break;
  }
}

void simPowerupTarget(int id, int target, uint32_t now) {
  if (id < 0 || id >= PU_COUNT) return;
  Powerup &pu = gPU[id];
  if (!pu.armed || pu.level == 0) return;
  if ((int32_t)(now - pu.activeUntil) >= 0) return;

  if (id == PU_FREEZE) {
    if (gNFrozen >= MAX_FROZEN) { pu.armed = 0; pu.armWallUntil = 0; return; }  // nothing left to hold
    if (target < 0 || target >= gRail.nStations) return;   // missed: stay armed
    if (stationFrozen(target)) return;                     // already iced: pick another
    gFrozen[gNFrozen++] = target;
    pu.armed = 0;
    pu.armWallUntil = 0;
    return;
  }

  if (id == PU_TRAIN) {
    // The game is held still until this resolves, so every path here must disarm --
    // never leave the players stuck staring at a paused board.
    if (gNTrains >= MAX_TRAINS) { pu.armed = 0; pu.armWallUntil = 0; return; }
    int N = gRail.N;
    if (N <= 0) { pu.armed = 0; return; }
    if (target < 0 || target >= N) return;  // not on the ring: stay armed

    // The tap already snapped to the nearest ring node. Walk outward from there for the
    // first node with room, so a crowded spot slides the train clear instead of refusing.
    // Straight-line distance understates distance along the track, so clearing this test
    // guarantees at least as much room on the rail itself.
    float need = carsPerTrain() * CFG_CAR_SPACING;
    int chosen = -1;
    for (int step = 0; step <= N / 2 && chosen < 0; step++) {
      for (int dir = 0; dir < 2; dir++) {
        int n = ((target + (dir ? -step : step)) % N + N) % N;
        bool clear = true;
        for (int j = 0; j < gNTrains && clear; j++) {
          float hx, hy, hdx, hdy; bool hidden;
          headState(gTrains[j], hx, hy, hdx, hdy, hidden);
          if (hypotf(hx - gPos[n].x, hy - gPos[n].y) < need) clear = false;
        }
        if (clear) { chosen = n; break; }
        if (step == 0) break;  // both directions are the same node
      }
    }
    if (chosen < 0) chosen = target;  // ring is packed; place it rather than hang
    makeTrain(gTrains[gNTrains], chosen);
    gNTrains++;
    pu.armed = 0;
    pu.armWallUntil = 0;
    return;
  }
}

// Shuffle the five powerups and deal them round-robin across the placed phone slots, so
// each match hands players a different set. Mirrors the per-owner allocation regenSpecials
// does for tunnels.
void simDealPowerups(const int *slots, int nSlots) {
  for (int k = 0; k < PU_COUNT; k++) gPU[k].owner = -1;
  if (!slots || nSlots <= 0) return;
  int order[PU_COUNT];
  for (int k = 0; k < PU_COUNT; k++) order[k] = k;
  for (int k = PU_COUNT - 1; k > 0; k--) {
    int j = (int)(gameRandom() * (k + 1));
    if (j < 0) j = 0;
    if (j > k) j = k;
    int t = order[k]; order[k] = order[j]; order[j] = t;
  }
  for (int k = 0; k < PU_COUNT; k++) gPU[order[k]].owner = (int8_t)slots[k % nSlots];
}


bool simTick(uint32_t now) {
  if (gGameOver) return true;
  Rail &R = gRail;
  float dt = (now - gLastMs) / 1000.0f;
  if (dt > 0.05f) dt = 0.05f;
  gLastMs = now;
  float t = (now - gStartMs) / 1000.0f;
  {
    float elapsed = (float)(now - gStartMs);
    if (elapsed < CFG_CHAOS_RAMP_MS) {
      float u = 1.0f - elapsed / CFG_CHAOS_RAMP_MS;
      gChaos = 1.0f - u * u;
    } else {
      gChaos = 1.0f + (CFG_CHAOS_TAIL_PER_MIN / 60000.0f) * (elapsed - CFG_CHAOS_RAMP_MS);
    }
  }

  for (int i = 0; i < R.nNodes; i++) {
    Node &nd = R.nodes[i];
    float c = fminf(gChaos, CFG_DRIFT_CHAOS_CAP) * nd.drift;
    if (c <= 0) { gPos[i] = { nd.x, nd.y }; }
    else {
      float A = c * CFG_DRIFT_PX * nd.amp;
      float across = A * sinf(t * nd.fx + nd.px);
      float along = A * CFG_DRIFT_ALONG * sinf(t * nd.fy + nd.py);
      gPos[i] = { nd.x + nd.dnx * across - nd.dny * along, nd.y + nd.dny * across + nd.dnx * along };
    }
  }
  for (int li = 0; li < R.nLinks; li++) {
    gSeg[li] = hypotf(gPos[R.links[li].b].x - gPos[R.links[li].a].x, gPos[R.links[li].b].y - gPos[R.links[li].a].y);
    if (R.teleport[li]) gSeg[li] = 0;
  }
  uint32_t dtMs = (uint32_t)(dt * 1000.0f);
  updatePowerups(now);
  trimCars();
  updateFreeze(dtMs);
  updateBonus(now);
  spawnPassengers(now);

  for (int i = 0; i < gNTrains; i++) {
    Train &tr = gTrains[i];
    bool canLeave = serviceTrain(tr, now);
    bool intangible = R.hidden[tr.li] || isIntangible(tr, now);

    float followBudget = 1e30f;
    int lead = -1;
    if (!intangible && gNTrains > 1) {
      const float LOOK_AHEAD = 420.0f;
      int wli = tr.li, wto = tr.to;
      float dist = -entryOffset(tr.li, tr.prog, tr.to);
      float bestGap = 1e30f;
      for (int guard = 0; guard < 60; guard++) {
        for (int k = 0; k < gNTrains; k++) {
          if (k == i || R.hidden[gTrains[k].li] || isIntangible(gTrains[k], now)) continue;
          if (gTrains[k].li != wli) continue;
          float gap = dist + entryOffset(wli, gTrains[k].prog, wto);
          if (gap > 0 && gap < bestGap) { bestGap = gap; lead = k; }
        }
        dist += gSeg[wli];
        if (dist >= LOOK_AHEAD) break;
        NextLink next = routeNextLink(wli, wto);
        if (next.li == tr.li && guard > 0) break;
        wli = next.li; wto = next.to;
      }
      if (lead >= 0) {
        float carSpan = (gTrains[lead].nCars - 1) * gTrains[lead].gap;
        followBudget = fmaxf(0, bestGap - carSpan - tr.gap);
      }
    }

    bool derailed = now < tr.derailUntil;
    bool nearLead = !intangible && !tr.stopped && lead >= 0 && followBudget <= 0.5f;
    if (nearLead && !tr.nearLead && !derailed) {
      tr.derailUntil = now + CFG_DERAIL_MS;
      tr.boostUntil = 0;
      derailed = true;
    }
    tr.nearLead = nearLead ? 1 : 0;

    bool boosting = now < tr.boostUntil;
    float speed = trainSpeed() * (boosting ? CFG_BOOST_MULT : 1.0f);
    float budget = derailed ? 0 : fminf(speed * dt, followBudget);
    int li0 = tr.li; float prog0 = tr.prog;
    bool wasStopped = tr.stopped;
    advanceTrain(budget, tr, i, canLeave && !derailed, now);

    if (tr.stopped && !wasStopped && !derailed) {
      for (int k = 0; k < gNTrains; k++) {
        if (k != i && gTrains[k].stopped && gTrains[k].station == tr.station) {
          tr.derailUntil = now + CFG_DERAIL_MS;
          tr.boostUntil = 0;
          derailed = true;
          break;
        }
      }
    }

    if (tr.stopped || derailed) tr.vsend = 0;
    else if (tr.li == li0) tr.vsend = (dt > 0 ? fabsf(tr.prog - prog0) / dt : 0);
    else tr.vsend = speed;

    float hx, hy, hdx, hdy; bool hidden;
    headState(tr, hx, hy, hdx, hdy, hidden);
    float moved = tr.hasPrev ? hypotf(hx - tr.prevX, hy - tr.prevY) : 0;
    tr.prevX = hx; tr.prevY = hy; tr.hasPrev = 1;
    for (int s = 0; s < R.nStations; s++) {
      if (R.stations[s].clearBy == i) {
        R.stations[s].clearDist -= moved;
        if (R.stations[s].clearDist <= 0) { R.stations[s].open = 0; R.stations[s].clearBy = -1; }
      }
    }
    float ratio = dt > 0 ? moved / dt / trainSpeed() : 0;
    float targetGap = CFG_CAR_SPACING_MIN + ratio * (CFG_CAR_SPACING - CFG_CAR_SPACING_MIN);
    if (targetGap < CFG_CAR_SPACING_MIN) targetGap = CFG_CAR_SPACING_MIN;
    if (targetGap > CFG_CAR_SPACING_MAX) targetGap = CFG_CAR_SPACING_MAX;
    tr.gap += (targetGap - tr.gap) * CFG_CAR_GAP_EASE;

    tr.derailedFlag = derailed ? 1 : 0;
  }

  for (int j = 0; j < R.nJunctions; j++) R.junctions[j].incoming = 0;
  for (int i = 0; i < gNTrains; i++) if (gTrains[i].tunnelJct >= 0) R.junctions[gTrains[i].tunnelJct].incoming = 1;

  if (checkOvercrowd(now, dtMs)) { gGameOver = true; return true; }
  return false;
}

void simStationToggle(int id) {
  if (id >= 0 && id < gRail.nStations) gRail.stations[id].open = !gRail.stations[id].open;
}

void simJunctionToggle(int id) {
  if (id >= 0 && id < gRail.nJunctions) gRail.junctions[id].active = gRail.junctions[id].active ? 0 : 1;
}

void simSwitchToggle(int id) {
  if (id >= 0 && id < gRail.nSwitches) gRail.switches[id].active = gRail.switches[id].active ? 0 : 1;
}

void simBoostArm(int id, uint32_t now) {
  if (id < 0 || id >= gRail.nBoosts) return;
  Boost &b = gRail.boosts[id];
  if (now >= b.cooldownUntil) b.armed = !b.armed;
}
