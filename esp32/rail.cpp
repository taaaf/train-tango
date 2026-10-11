#include "game.h"
#include <new>

Rail *gRailPtr = nullptr;

static Node *sBuf = nullptr;
static int sMap[MAX_NODES];

bool gameAlloc() {
  if (!gRailPtr) gRailPtr = new (std::nothrow) Rail();
  if (!gTrains) gTrains = new (std::nothrow) Train[MAX_TRAINS];
  return gRailPtr && gTrains;
}

struct Pt { float x, y; int owner; };
static Pt sPts[256];

static Node makeNode(float x, float y, int owner, bool drift) {
  Node n;
  n.x = x; n.y = y; n.owner = owner;
  n.drift = drift ? 1 : 0;
  n.px = gameRandom() * 2 * M_PI;
  n.py = gameRandom() * 2 * M_PI;
  n.fx = 0.3f + gameRandom() * 0.6f;
  n.fy = 0.3f + gameRandom() * 0.6f;
  n.amp = 0.6f + gameRandom() * 0.4f;
  n.type = 0; n.st = -1; n.nx = 0; n.ny = 0; n.dnx = 0; n.dny = 0;
  n.jct = -1; n.swt = -1; n.jctOut = -1; n.swtOut = -1; n.spurSeg = 0; n.pad = -1;
  n.hasForward = 0; n.fLi = 0; n.fTo = 0;
  n.hasBack = 0; n.bLi = 0; n.bTo = 0;
  return n;
}

struct SpurSlot { int owner; float cx, cy, ux, uy; };
static SpurSlot sSpurSlots[MAX_STATIONS];
static int sNSpurSlots = 0;

static Node makeDriftNode(const Node &a, const Node &b, float t) {
  return makeNode(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.owner, true);
}

struct RS { bool isY; float fixed; float from; float to; int dir; };

static RS railSide(const Tile &t, int side) {
  float l = t.x + RAIL_MARGIN, r = t.x + t.w - RAIL_MARGIN;
  float tt = t.y + RAIL_MARGIN, bb = t.y + t.h - RAIL_MARGIN;
  RS s{};
  switch (side) {
    case SIDE_LEFT:   s = { true,  l, tt, bb,  1 }; break;
    case SIDE_BOTTOM: s = { false, bb, l, r,   1 }; break;
    case SIDE_RIGHT:  s = { true,  r, bb, tt, -1 }; break;
    case SIDE_TOP:    s = { false, tt, r, l,  -1 }; break;
  }
  return s;
}

static int nextSide(int s) {
  switch (s) {
    case SIDE_LEFT: return SIDE_BOTTOM;
    case SIDE_BOTTOM: return SIDE_RIGHT;
    case SIDE_RIGHT: return SIDE_TOP;
    case SIDE_TOP: return SIDE_LEFT;
  }
  return SIDE_LEFT;
}

static void axisPoint(const RS &s, float v, float &x, float &y) {
  if (s.isY) { x = s.fixed; y = v; } else { x = v; y = s.fixed; }
}

static void innerAxisRange(const Tile &t, int side, float &lo, float &hi) {
  if (side == SIDE_LEFT || side == SIDE_RIGHT) { lo = t.y + RAIL_MARGIN; hi = t.y + t.h - RAIL_MARGIN; }
  else { lo = t.x + RAIL_MARGIN; hi = t.x + t.w - RAIL_MARGIN; }
}

struct Opening { float lo, hi; int nb; int nbSide; };

static int railOpenings(int ti, int side, Opening *out, int maxOut) {
  Tile &t = gLayout.tiles[ti];
  bool vertical = (side == SIDE_LEFT || side == SIDE_RIGHT);
  float curLo, curHi;
  innerAxisRange(t, side, curLo, curHi);
  int n = 0;
  for (int ei = 0; ei < gLayout.nEdges; ei++) {
    Edge &e = gLayout.edges[ei];
    if (e.tile != ti || e.side != side) continue;
    float a = vertical ? t.y + e.start : t.x + e.start;
    float b = vertical ? t.y + e.end : t.x + e.end;
    int nb = e.neighbor;
    int nbSide = -1;
    for (int ej = 0; ej < gLayout.nEdges; ej++) {
      Edge &e2 = gLayout.edges[ej];
      if (e2.tile == nb && e2.neighbor == ti) { nbSide = e2.side; break; }
    }
    if (nbSide < 0) continue;
    float nbLo, nbHi;
    innerAxisRange(gLayout.tiles[nb], nbSide, nbLo, nbHi);
    float lo = fmaxf(fminf(a, b), fmaxf(curLo, nbLo));
    float hi = fminf(fmaxf(a, b), fminf(curHi, nbHi));
    if (hi - lo < 1) continue;
    if (n < maxOut) { out[n] = { lo, hi, nb, nbSide }; n++; }
  }
  return n;
}

static int traceRail() {
  int ti = 0;
  for (int i = 1; i < gLayout.nTiles; i++)
    if (gLayout.tiles[i].x < gLayout.tiles[ti].x - 0.5f) ti = i;
  int side = SIDE_LEFT;
  RS si = railSide(gLayout.tiles[ti], side);
  float pos = si.from;
  float sx, sy;
  axisPoint(si, pos, sx, sy);
  int nPts = 0;
  sPts[nPts++] = { sx, sy, gLayout.tiles[ti].phone };
  const float EPS = 1e-6f;

  for (int it = 0; it < 5000; it++) {
    si = railSide(gLayout.tiles[ti], side);
    float bestAhead = fabsf(si.to - pos);
    bool haveCross = false;
    float crossAt = 0;
    int crossNb = 0, crossNbSide = 0;

    Opening ops[8];
    int nop = railOpenings(ti, side, ops, 8);
    for (int oi = 0; oi < nop; oi++) {
      float nearEdge = si.dir > 0 ? ops[oi].lo : ops[oi].hi;
      float ahead = (nearEdge - pos) * si.dir;
      if (ahead > -0.5f && ahead < bestAhead - EPS) {
        bestAhead = fmaxf(ahead, 0);
        haveCross = true;
        crossAt = nearEdge;
        crossNb = ops[oi].nb;
        crossNbSide = ops[oi].nbSide;
      }
    }

    if (!haveCross) {
      float cx, cy;
      axisPoint(si, si.to, cx, cy);
      if (nPts < 256) sPts[nPts++] = { cx, cy, gLayout.tiles[ti].phone };
      if (it > 0 && hypotf(cx - sx, cy - sy) < 0.5f) break;
      side = nextSide(side);
      pos = railSide(gLayout.tiles[ti], side).from;
    } else {
      float hx, hy;
      axisPoint(si, crossAt, hx, hy);
      if (nPts < 256) sPts[nPts++] = { hx, hy, gLayout.tiles[ti].phone };
      RS nsi = railSide(gLayout.tiles[crossNb], crossNbSide);
      float tx, ty;
      axisPoint(nsi, crossAt, tx, ty);
      if (nPts < 256) sPts[nPts++] = { tx, ty, gLayout.tiles[crossNb].phone };
      ti = crossNb;
      side = crossNbSide;
      pos = crossAt;
    }
    if (nPts >= 254) break;
  }

  const float MERGE_EPS = 6.0f;
  int w = 1;
  for (int i = 1; i < nPts; i++) {
    if (hypotf(sPts[i].x - sPts[w - 1].x, sPts[i].y - sPts[w - 1].y) >= MERGE_EPS) sPts[w++] = sPts[i];
  }
  nPts = w;

  if (nPts > 1) {
    float dx = sPts[0].x - sPts[nPts - 1].x;
    float dy = sPts[0].y - sPts[nPts - 1].y;
    if (hypotf(dx, dy) < MERGE_EPS) nPts--;
  }
  return nPts;
}

static int idxDist(int a, int b, int N) {
  int d = abs(a - b) % N;
  return d < N - d ? d : N - d;
}

static int stationCap(float L) {
  int c = (L >= STATION_OK_SEG ? 1 : 0) + (L >= STATION_PACK_SEG ? 1 : 0);
  if (c > STATIONS_PER_PHONE) c = STATIONS_PER_PHONE;
  return c;
}

// The stretch of a base segment a station centre may sit in, measured from the segment's
// start. Both ends keep STATION_CORNER_CLEAR off the corner so a platform never wraps one.
// A spur needs more on the FORWARD end: buildSpurs() finds the ring node nearest the slot
// and then walks forward to lay the siding, giving up at the next corner -- so a slot parked
// near the end of its segment has nowhere to put the siding and silently falls back inline.
static void stationBand(float L, bool spur, float &lo, float &hi) {
  float half = fminf(PLATFORM_LEN, L * 0.85f) / 2;
  lo = STATION_CORNER_CLEAR + half;
  hi = L - STATION_CORNER_CLEAR - half;
  if (spur) hi = fminf(hi, L - SPUR_SPAN_MIN);
  if (lo > hi) lo = hi = L / 2;
}

// Where along its segment each station sits, in the same index space as stnCount: filled by
// the spread pass below, -1 where that pass had nothing to say.
static float sStnCenter[MAX_NODES];

static int addStations(const Node *src, int nB, Node *dst) {
  Rail &R = gRail;
  R.nStations = 0;
  sNSpurSlots = 0;

  struct Seg { int owner, i; float len; };
  static Seg segs[256];
  int nSegs = 0;
  for (int i = 0; i < nB; i++) {
    int a = i, b = (i + 1) % nB;
    int owner = src[a].owner;
    if (owner < 0 || owner != src[b].owner) continue;
    float len = hypotf(src[b].x - src[a].x, src[b].y - src[a].y);
    if (nSegs < 256) segs[nSegs++] = { owner, i, len };
  }

  static int stnCount[MAX_NODES];
  for (int i = 0; i < nB; i++) { stnCount[i] = 0; sStnCenter[i] = -1; }

  bool ownerDone[MAX_DEVICES];
  for (int o = 0; o < MAX_DEVICES; o++) ownerDone[o] = false;

  for (int s = 0; s < nSegs; s++) {
    int owner = segs[s].owner;
    if (owner < 0 || owner >= MAX_DEVICES || ownerDone[owner]) continue;
    ownerDone[owner] = true;

    int local[256], nLocal = 0;
    for (int t = 0; t < nSegs; t++) if (segs[t].owner == owner) local[nLocal++] = t;
    for (int a = 0; a < nLocal; a++)
      for (int b = a + 1; b < nLocal; b++)
        if (segs[local[b]].len > segs[local[a]].len) { int tmp = local[a]; local[a] = local[b]; local[b] = tmp; }

    int assigned = 0;
    if (STATIONS_PER_PHONE == 2 && nLocal >= 2) {
      int pairA = -1, pairB = -1;
      float pairScore = -1e30f;
      for (int a = 0; a < nLocal; a++) {
        for (int b = a + 1; b < nLocal; b++) {
          Seg &SA = segs[local[a]], &SB = segs[local[b]];
          if (stationCap(SA.len) < 1 || stationCap(SB.len) < 1) continue;
          int spurA = SA.len >= SPUR_MIN_BASE ? 1 : 0;
          int spurB = SB.len >= SPUR_MIN_BASE ? 1 : 0;
          int sep = idxDist(SA.i, SB.i, nB);
          float score = (spurA + spurB) * 1e7f + sep * 100.0f + (SA.len + SB.len) * 0.01f;
          if (score > pairScore) { pairScore = score; pairA = local[a]; pairB = local[b]; }
        }
      }
      if (pairA >= 0 && pairB >= 0) {
        stnCount[segs[pairA].i] = 1;
        stnCount[segs[pairB].i] = 1;
        assigned = 2;
      }
    }
    while (assigned < STATIONS_PER_PHONE) {
      int best = -1;
      float bestScore = -1e30f;
      for (int t = 0; t < nLocal; t++) {
        int si = segs[local[t]].i;
        if (stationCap(segs[local[t]].len) <= stnCount[si]) continue;
        int sep = 1 << 20;
        bool anyUsed = false;
        for (int u = 0; u < nLocal; u++) {
          int sj = segs[local[u]].i;
          if (stnCount[sj] > 0) { anyUsed = true; int d = idxDist(sj, si, nB); if (d < sep) sep = d; }
        }
        if (!anyUsed) sep = 1 << 20;
        float score = (stnCount[si] == 0 ? 1e6f : 0.0f) + sep * 100.0f + segs[local[t]].len * 0.01f;
        if (score > bestScore) { bestScore = score; best = local[t]; }
      }
      if (best < 0) break;
      stnCount[segs[best].i]++;
      assigned++;
    }
    while (assigned < STATIONS_PER_PHONE && nLocal > 0) {
      int best = -1;
      for (int t = 0; t < nLocal; t++) {
        int si = segs[local[t]].i;
        if (best < 0 || stnCount[si] < stnCount[segs[best].i]) best = local[t];
      }
      if (best < 0) break;
      stnCount[segs[best].i]++;
      assigned++;
    }
  }

  // Spread each phone's two stations across its own stretch of track.
  //
  // Choosing the two SEGMENTS above is only half the job: a segment is a long run, and where
  // the station sits inside it is what decides whether the pair reads as spread out or as a
  // clump. That used to be a 0.25 / 0.75 alternation driven by a counter shared across every
  // phone, which cannot know where THIS phone's other station went -- and because the loop
  // runs down one edge and back up the next, 0.75 of one segment and 0.25 of the next meet at
  // the corner between them. Two stations in one corner of a screen, which is the clump
  // players hit.
  //
  // So pick both centres at once, as the pair that ends up furthest apart on screen. Each is
  // confined to its own segment's legal band, and the furthest-apart points of two straight
  // bands are always band ends, so the four end combinations are the entire search.
  for (int o = 0; o < MAX_DEVICES; o++) {
    int si[2], nSi = 0;
    for (int s = 0; s < nSegs && nSi < 2; s++)
      if (segs[s].owner == o && stnCount[segs[s].i] == 1) si[nSi++] = s;
    if (nSi < 2) continue;  // one segment, or both stations packed onto it: leave it alone

    float ax[2], ay[2], ux[2], uy[2], end[2][2];
    for (int k = 0; k < 2; k++) {
      int i = segs[si[k]].i;
      const Node &a = src[i], &b = src[(i + 1) % nB];
      float L = segs[si[k]].len;
      if (L < 1e-6f) L = 1;
      ax[k] = a.x; ay[k] = a.y;
      ux[k] = (b.x - a.x) / L; uy[k] = (b.y - a.y) / L;
      stationBand(L, L >= SPUR_MIN_BASE, end[k][0], end[k][1]);
    }
    float bestD = -1, bestC0 = 0, bestC1 = 0;
    for (int p = 0; p < 2; p++) {
      for (int q = 0; q < 2; q++) {
        float x0 = ax[0] + ux[0] * end[0][p], y0 = ay[0] + uy[0] * end[0][p];
        float x1 = ax[1] + ux[1] * end[1][q], y1 = ay[1] + uy[1] * end[1][q];
        float d = hypotf(x1 - x0, y1 - y0);
        if (d > bestD) { bestD = d; bestC0 = end[0][p]; bestC1 = end[1][q]; }
      }
    }
    sStnCenter[segs[si[0]].i] = bestC0;
    sStnCenter[segs[si[1]].i] = bestC1;
  }

  int nDst = 0;
  for (int i = 0; i < nB; i++) {
    dst[nDst++] = src[i];
    int c = stnCount[i];
    if (c <= 0) continue;
    const Node &a = src[i];
    const Node &b = src[(i + 1) % nB];
    float dx = b.x - a.x, dy = b.y - a.y;
    float L = hypotf(dx, dy);
    if (L < 1e-6f) L = 1;
    float ux = dx / L, uy = dy / L;
    float nx = -uy, ny = ux;
    int owner = src[i].owner;

    for (int k = 0; k < c && R.nStations < MAX_STATIONS; k++) {
      float center;
      float half = fminf(PLATFORM_LEN, (c == 1 ? L : L / c) * 0.85f) / 2;
      if (c == 1) {
        float lo, hi;
        stationBand(L, L >= SPUR_MIN_BASE, lo, hi);
        // The alternation is the fallback now: it only runs for a phone the spread pass left
        // alone, where there is no second station to be far from.
        float want = sStnCenter[i];
        if (want < 0)
          want = L * (((R.nStations + sNSpurSlots) % 2 == 0) ? STATION_FRAC : 1 - STATION_FRAC);
        center = fminf(fmaxf(want, lo), hi);
      } else {
        float slot = L / c;
        center = (k + 0.5f) * slot;
      }
      float cx = a.x + ux * center, cy = a.y + uy * center;

      if (c == 1 && L >= SPUR_MIN_BASE && sNSpurSlots < MAX_STATIONS) {
        sSpurSlots[sNSpurSlots++] = { owner, cx, cy, ux, uy };
        continue;
      }

      int sid = R.nStations;
      Node entry = makeNode(cx - ux * half, cy - uy * half, owner, false);
      entry.type = 1; entry.st = sid; entry.nx = nx; entry.ny = ny;
      Node exit = makeNode(cx + ux * half, cy + uy * half, owner, false);
      exit.type = 1; exit.st = sid; exit.nx = nx; exit.ny = ny;

      int ia = nDst;
      dst[nDst++] = entry;
      int ib = nDst;
      dst[nDst++] = exit;

      Station &st = R.stations[R.nStations++];
      st.a = ia; st.b = ib; st.open = 0; st.nx = nx; st.ny = ny;
      st.colorIdx = 0; st.qn = 0; st.fullSince = 0;
      st.clearBy = -1; st.clearDist = 0;
    }
  }
  return nDst;
}

static int resample(const Node *src, int nB, Node *dst, int *mapIdx) {
  int nDst = 0;
  for (int i = 0; i < nB; i++) {
    mapIdx[i] = nDst;
    dst[nDst++] = src[i];
    const Node &a = src[i];
    const Node &b = src[(i + 1) % nB];
    if (a.st >= 0 && a.st == b.st) continue;
    float len = hypotf(b.x - a.x, b.y - a.y);
    int segs = (int)lroundf(len / NODE_SPACING);
    if (segs < 1) segs = 1;
    for (int k = 1; k < segs && nDst < MAX_NODES; k++) dst[nDst++] = makeDriftNode(a, b, (float)k / segs);
  }
  return nDst;
}

static void colorStationsByLoop() {
  Rail &R = gRail;
  int m = R.nStations;
  if (m <= 0) return;
  const int C = STATION_COLORS;

  int loopPos[MAX_STATIONS];
  for (int i = 0; i < m; i++) {
    float mx = (R.nodes[R.stations[i].a].x + R.nodes[R.stations[i].b].x) * 0.5f;
    float my = (R.nodes[R.stations[i].a].y + R.nodes[R.stations[i].b].y) * 0.5f;
    int bid = 0; float bd = 1e30f;
    for (int n = 0; n < R.N; n++) {
      float dx = R.nodes[n].x - mx, dy = R.nodes[n].y - my;
      float d = dx * dx + dy * dy;
      if (d < bd) { bd = d; bid = n; }
    }
    loopPos[i] = bid;
  }
  int order[MAX_STATIONS];
  for (int i = 0; i < m; i++) order[i] = i;
  for (int i = 1; i < m; i++) {
    int key = order[i], kp = loopPos[key], j = i - 1;
    while (j >= 0 && loopPos[order[j]] > kp) { order[j + 1] = order[j]; j--; }
    order[j + 1] = key;
  }
  int used[STATION_COLORS] = {0};
  for (int i = 0; i < m; i++) R.stations[i].colorIdx = -1;
  for (int p = 0; p < m; p++) {
    int prev = p > 0 ? R.stations[order[p - 1]].colorIdx : -1;
    int wrap = (p == m - 1) ? R.stations[order[0]].colorIdx : -1;
    int best = -1, bestUsed = 0x7fffffff;
    for (int t = 0; t < C; t++) {
      int k = (p + t) % C;
      if (k == prev || k == wrap) continue;
      if (used[k] < bestUsed) { bestUsed = used[k]; best = k; }
    }
    if (best < 0) best = p % C;
    R.stations[order[p]].colorIdx = best;
    used[best]++;
  }
}

static void finalizeStations() {
  Rail &R = gRail;
  float cx[MAX_DEVICES] = {0}, cy[MAX_DEVICES] = {0};
  int ck[MAX_DEVICES] = {0};
  for (int i = 0; i < R.nNodes; i++) {
    int o = R.nodes[i].owner;
    if (o < 0 || o >= MAX_DEVICES) continue;
    cx[o] += R.nodes[i].x; cy[o] += R.nodes[i].y; ck[o]++;
  }
  for (int i = 0; i < R.nStations; i++) {
    Station &s = R.stations[i];
    int o = R.nodes[s.a].owner;
    if (o >= 0 && o < MAX_DEVICES && ck[o]) {
      float ox = cx[o] / ck[o], oy = cy[o] / ck[o];
      float mx = (R.nodes[s.a].x + R.nodes[s.b].x) / 2;
      float my = (R.nodes[s.a].y + R.nodes[s.b].y) / 2;
      if (s.nx * (ox - mx) + s.ny * (oy - my) < 0) { s.nx = -s.nx; s.ny = -s.ny; }
      R.nodes[s.a].nx = s.nx; R.nodes[s.a].ny = s.ny;
      R.nodes[s.b].nx = s.nx; R.nodes[s.b].ny = s.ny;
    }
  }
  colorStationsByLoop();
}

static int addNodeR(const Node &n) {
  int id = gRail.nNodes;
  gRail.nodes[id] = n;
  gRail.nNodes++;
  return id;
}
static int addLinkR(int a, int b) {
  int li = gRail.nLinks;
  gRail.links[li].a = a; gRail.links[li].b = b;
  gRail.nLinks++;
  return li;
}

static void buildSpur(int J, int E, float span, const int *straightLinks, int nStraight,
                      const float *cenX, const float *cenY, const int *cenK) {
  Rail &R = gRail;
  if (R.nSwitches >= MAX_SWITCHES || R.nStations >= MAX_STATIONS) return;
  if (R.nNodes + 4 > MAX_NODES || R.nLinks + 5 > MAX_LINKS) return;
  int swid = R.nSwitches;
  int sid = R.nStations;
  float ax = R.nodes[J].x, ay = R.nodes[J].y;
  float ex = R.nodes[E].x, ey = R.nodes[E].y;
  int owner = R.nodes[J].owner;
  float dx = ex - ax, dy = ey - ay;
  float L = hypotf(dx, dy); if (L < 1e-6f) L = 1;
  float ux = dx / L, uy = dy / L;
  float nx = -uy, ny = ux;
  if (owner >= 0 && owner < MAX_DEVICES && cenK[owner]) {
    float mx = (ax + ex) / 2, my = (ay + ey) / 2;
    if (nx * (cenX[owner] / cenK[owner] - mx) + ny * (cenY[owner] / cenK[owner] - my) < 0) { nx = -nx; ny = -ny; }
  }
  float D = STATION_INSET;
  float rejoinA = span - D, gateA = rejoinA - GATE_CLEAR, p1A = gateA - PLATFORM_LEN, entryA = D;

  Node nEntry = makeNode(ax + ux * entryA + nx * D, ay + uy * entryA + ny * D, owner, false);
  int idEntry = addNodeR(nEntry);
  Node nP1 = makeNode(ax + ux * p1A + nx * D, ay + uy * p1A + ny * D, owner, false);
  nP1.type = 1; nP1.st = sid; nP1.nx = nx; nP1.ny = ny;
  int idP1 = addNodeR(nP1);
  Node nGate = makeNode(ax + ux * gateA + nx * D, ay + uy * gateA + ny * D, owner, false);
  nGate.type = 1; nGate.st = sid; nGate.nx = nx; nGate.ny = ny;
  int idGate = addNodeR(nGate);
  Node nRej = makeNode(ax + ux * rejoinA + nx * D, ay + uy * rejoinA + ny * D, owner, false);
  int idRej = addNodeR(nRej);

  int liEntry = addLinkR(J, idEntry);
  int liLead = addLinkR(idEntry, idP1);
  int liPlat = addLinkR(idP1, idGate);
  int liClear = addLinkR(idGate, idRej);
  int liRejoin = addLinkR(idRej, E);

  R.nodes[idEntry].hasForward = 1; R.nodes[idEntry].fLi = liLead; R.nodes[idEntry].fTo = idP1;
  R.nodes[idP1].hasForward = 1; R.nodes[idP1].fLi = liPlat; R.nodes[idP1].fTo = idGate;
  R.nodes[idGate].hasForward = 1; R.nodes[idGate].fLi = liClear; R.nodes[idGate].fTo = idRej;
  R.nodes[idRej].hasForward = 1; R.nodes[idRej].fLi = liRejoin; R.nodes[idRej].fTo = E;

  // Mirror of the chain above, walked when gDirRev is set. Without it a reversed train
  // would run straight past every spur station.
  R.nodes[idRej].hasBack = 1; R.nodes[idRej].bLi = liClear; R.nodes[idRej].bTo = idGate;
  R.nodes[idGate].hasBack = 1; R.nodes[idGate].bLi = liPlat; R.nodes[idGate].bTo = idP1;
  R.nodes[idP1].hasBack = 1; R.nodes[idP1].bLi = liLead; R.nodes[idP1].bTo = idEntry;
  R.nodes[idEntry].hasBack = 1; R.nodes[idEntry].bLi = liEntry; R.nodes[idEntry].bTo = J;

  R.nodes[J].swt = swid;
  R.nodes[E].swtOut = swid;

  float ilx = R.nodes[idEntry].x - ax, ily = R.nodes[idEntry].y - ay;
  float il = hypotf(ilx, ily); if (il < 1e-6f) il = 1;

  // The far-end mirror of inDir: E -> idRej is the leg a reversed train takes off the ring,
  // and it is what the lever is drawn against once the board runs the other way.
  float olx = R.nodes[idRej].x - ex, oly = R.nodes[idRej].y - ey;
  float ol = hypotf(olx, oly); if (ol < 1e-6f) ol = 1;

  Station &st = R.stations[R.nStations++];
  st.a = idP1; st.b = idGate; st.open = 0; st.nx = nx; st.ny = ny;
  st.colorIdx = 0; st.qn = 0; st.fullSince = 0; st.clearBy = -1; st.clearDist = 0;

  Switch &sw = R.switches[R.nSwitches++];
  sw.node = J; sw.exit = E;
  sw.entryLi = liEntry; sw.entryTo = idEntry; sw.entryLink = liEntry;
  sw.exitLi = liRejoin; sw.exitTo = idRej;
  sw.nStraight = nStraight < 8 ? nStraight : 8;
  for (int i = 0; i < sw.nStraight; i++) sw.straightLinks[i] = straightLinks[i];
  sw.station = sid;
  sw.inDirX = ilx / il; sw.inDirY = ily / il;
  sw.outDirX = olx / ol; sw.outDirY = oly / ol;
  sw.active = 0;
}

static void inlineFallback(SpurSlot &slot, uint8_t *used, const uint8_t *isCorner,
                           const float *cenX, const float *cenY, const int *cenK) {
  Rail &R = gRail;
  if (R.nStations >= MAX_STATIONS) return;

  int bestI = -1;
  float bestD = 1e30f;
  for (int i = 0; i < R.N; i++) {
    if (used[i]) continue;
    Node &a = R.nodes[i];
    if (a.owner != slot.owner || a.type == 1 || isCorner[i]) continue;
    int nxt = (i + 1) % R.N;
    if (used[nxt]) continue;
    Node &b = R.nodes[nxt];
    if (b.owner != slot.owner || b.type == 1 || isCorner[nxt]) continue;
    float dx = b.x - a.x, dy = b.y - a.y, L = hypotf(dx, dy); if (L < 1e-6f) L = 1;
    if ((dx / L) * slot.ux + (dy / L) * slot.uy < 0.9f) continue;
    float mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2;
    float d = hypotf(mx - slot.cx, my - slot.cy);
    if (d < bestD) { bestD = d; bestI = i; }
  }
  if (bestI < 0) return;

  int nxt = (bestI + 1) % R.N;
  Node &a = R.nodes[bestI];
  Node &b = R.nodes[nxt];
  float half = PLATFORM_LEN / 2.0f;
  float nx = -slot.uy, ny = slot.ux;
  int owner = slot.owner;
  if (owner >= 0 && owner < MAX_DEVICES && cenK[owner]) {
    float mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2;
    if (nx * (cenX[owner] / cenK[owner] - mx) + ny * (cenY[owner] / cenK[owner] - my) < 0) {
      nx = -nx; ny = -ny;
    }
  }
  int sid = R.nStations;
  a.x = slot.cx - slot.ux * half; a.y = slot.cy - slot.uy * half;
  a.type = 1; a.st = sid; a.nx = nx; a.ny = ny; a.drift = 0;
  b.x = slot.cx + slot.ux * half; b.y = slot.cy + slot.uy * half;
  b.type = 1; b.st = sid; b.nx = nx; b.ny = ny; b.drift = 0;

  Station &st = R.stations[R.nStations++];
  st.a = bestI; st.b = nxt; st.open = 0; st.nx = nx; st.ny = ny;
  st.colorIdx = 0; st.qn = 0; st.fullSince = 0;
  st.clearBy = -1; st.clearDist = 0;
  used[bestI] = 1; used[nxt] = 1;
}

static void buildSpurs() {
  Rail &R = gRail;
  R.nSwitches = 0;
  if (sNSpurSlots == 0) return;

  static float cenX[MAX_DEVICES], cenY[MAX_DEVICES];
  static int cenK[MAX_DEVICES];
  for (int o = 0; o < MAX_DEVICES; o++) { cenX[o] = cenY[o] = 0; cenK[o] = 0; }
  for (int i = 0; i < R.N; i++) {
    int o = R.nodes[i].owner;
    if (o < 0 || o >= MAX_DEVICES) continue;
    cenX[o] += R.nodes[i].x; cenY[o] += R.nodes[i].y; cenK[o]++;
  }
  static uint8_t isCorner[MAX_NODES];
  for (int i = 0; i < R.N; i++) {
    Node &p = R.nodes[(i - 1 + R.N) % R.N], &a = R.nodes[i], &b = R.nodes[(i + 1) % R.N];
    float v1x = a.x - p.x, v1y = a.y - p.y, v2x = b.x - a.x, v2y = b.y - a.y;
    float l1 = hypotf(v1x, v1y); if (l1 < 1e-6f) l1 = 1;
    float l2 = hypotf(v2x, v2y); if (l2 < 1e-6f) l2 = 1;
    isCorner[i] = ((v1x * v2x + v1y * v2y) / (l1 * l2) < SWITCH_CORNER_COS) ? 1 : 0;
  }
  static uint8_t used[MAX_NODES];
  for (int i = 0; i < R.N; i++) used[i] = 0;

  struct JCand { int j; float d; };
  static JCand cands[MAX_NODES];

  for (int s = 0; s < sNSpurSlots; s++) {
    SpurSlot &slot = sSpurSlots[s];
    int nCands = 0;
    for (int j = 0; j < R.N; j++) {
      if (used[j]) continue;
      Node &a = R.nodes[j];
      if (a.owner != slot.owner || a.type == 1 || isCorner[j]) continue;
      Node &e1 = R.nodes[(j + 1) % R.N];
      float dx = e1.x - a.x, dy = e1.y - a.y, L = hypotf(dx, dy); if (L < 1e-6f) L = 1;
      if ((dx / L) * slot.ux + (dy / L) * slot.uy < 0.99f) continue;
      cands[nCands++] = { j, hypotf(a.x - slot.cx, a.y - slot.cy) };
    }
    for (int i = 1; i < nCands; i++) {
      JCand key = cands[i];
      int k = i - 1;
      while (k >= 0 && cands[k].d > key.d) { cands[k + 1] = cands[k]; k--; }
      cands[k + 1] = key;
    }

    bool placed = false;
    for (int ci = 0; ci < nCands && !placed; ci++) {
      int J = cands[ci].j;
      float span = 0;
      int E = -1, nStraight = 0, straightLinks[8];
      int cur = J;
      for (int step = 0; step < R.N; step++) {
        int nxt = (cur + 1) % R.N;
        if (used[nxt] && nxt != J) break;
        Node &a = R.nodes[cur], &b = R.nodes[nxt];
        float dx = b.x - a.x, dy = b.y - a.y, L = hypotf(dx, dy); if (L < 1e-6f) L = 1;
        if ((dx / L) * slot.ux + (dy / L) * slot.uy < 0.99f) break;
        if (b.type == 1) break;
        if (b.owner != slot.owner) break;
        if (nStraight >= 8) break;
        straightLinks[nStraight++] = cur;
        span += L;
        E = nxt;
        cur = nxt;
        if (isCorner[nxt]) break;
        if (span >= SPUR_SPAN_TARGET) break;
      }
      if (E < 0 || span < SPUR_SPAN_MIN) continue;

      for (int n = J;; n = (n + 1) % R.N) {
        used[n] = 1;
        R.nodes[n].spurSeg = 1;
        if (n == E) break;
      }
      buildSpur(J, E, span, straightLinks, nStraight, cenX, cenY, cenK);
      placed = true;
    }
    if (!placed) inlineFallback(slot, used, isCorner, cenX, cenY, cenK);
  }
}

bool buildRail() {
  Rail &R = gRail;
  R.nNodes = 0; R.nLinks = 0; R.nStations = 0; R.nSwitches = 0; R.N = 0;
  if (gLayout.nTiles == 0) return false;

  int nPts = traceRail();
  if (nPts < 2) return false;

  if (!sBuf) sBuf = new (std::nothrow) Node[MAX_NODES];
  if (!sBuf) return false;

  for (int i = 0; i < nPts; i++) sBuf[i] = makeNode(sPts[i].x, sPts[i].y, sPts[i].owner, true);

  int nB = addStations(sBuf, nPts, R.nodes);

  int nC = resample(R.nodes, nB, sBuf, sMap);
  for (int i = 0; i < nC; i++) R.nodes[i] = sBuf[i];
  R.nNodes = nC;
  R.N = nC;

  delete[] sBuf; sBuf = nullptr;

  for (int i = 0; i < R.nStations; i++) {
    R.stations[i].a = sMap[R.stations[i].a];
    R.stations[i].b = sMap[R.stations[i].b];
  }

  R.nLinks = nC;
  for (int i = 0; i < nC; i++) { R.links[i].a = i; R.links[i].b = (i + 1) % nC; }

  buildSpurs();
  R.permNodes = R.nNodes;
  R.permLinks = R.nLinks;

  finalizeStations();
  return true;
}
