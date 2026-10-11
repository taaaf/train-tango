#include "game.h"

static float gCenX[MAX_DEVICES], gCenY[MAX_DEVICES];
static int gCenK[MAX_DEVICES];

static float gBaseCum[MAX_NODES + 1];

struct PtXY { float x, y; };
static PtXY gCross[MAX_NODES];
static int gNCross;
static PtXY gCorner[MAX_NODES];
static int gNCorner;
static PtXY gStationPts[2 * MAX_STATIONS];
static int gNStationPts;

static Node tunnelMakeNode(float x, float y, int owner) {
  Node n;
  n.x = x; n.y = y; n.owner = owner;
  n.drift = 0;
  n.px = n.py = n.fx = n.fy = n.amp = 0;
  n.type = 0; n.st = -1; n.nx = 0; n.ny = 0; n.dnx = 0; n.dny = 0;
  n.jct = -1; n.swt = -1; n.jctOut = -1; n.swtOut = -1; n.spurSeg = 0; n.pad = -1;
  n.hasForward = 0; n.fLi = 0; n.fTo = 0;
  n.hasBack = 0; n.bLi = 0; n.bTo = 0;
  return n;
}

static float minGap(float px, float py, const PtXY *pts, int n, float d) {
  if (n == 0) return 1e30f;
  float m = 1e30f;
  for (int i = 0; i < n; i++) {
    float dist = hypotf(px - pts[i].x, py - pts[i].y);
    if (dist < m) m = dist;
  }
  return m - d;
}

static float placeClear(int idx) {
  Node &p = gRail.nodes[idx];
  float a = minGap(p.x, p.y, gCross, gNCross, CROSS_CLEAR);
  float b = minGap(p.x, p.y, gStationPts, gNStationPts, STATION_CLEAR);
  float c = minGap(p.x, p.y, gCorner, gNCorner, CORNER_CLEAR);
  return fminf(fminf(a, b), c);
}

static float exitClear(int idx) {
  Node &p = gRail.nodes[idx];
  float a = minGap(p.x, p.y, gCross, gNCross, CROSS_CLEAR);
  float b = minGap(p.x, p.y, gStationPts, gNStationPts, STATION_CLEAR);
  float c = minGap(p.x, p.y, gCorner, gNCorner, CORNER_CLEAR);
  return fminf(fminf(a, b), c);
}

static PtXY gOccupied[64];
static int gNOccupied = 0;
static void occupy(float x, float y) { if (gNOccupied < 64) gOccupied[gNOccupied++] = { x, y }; }
static float specialClear(int idx) {
  Node &p = gRail.nodes[idx];
  return minGap(p.x, p.y, gOccupied, gNOccupied, SPECIAL_CLEAR);
}

static void baseTangent(int idx, float &dx, float &dy) {
  int N = gRail.N;
  Node &p = gRail.nodes[(idx - 1 + N) % N];
  Node &n = gRail.nodes[(idx + 1) % N];
  float vx = n.x - p.x, vy = n.y - p.y;
  float L = hypotf(vx, vy);
  if (L < 1e-6f) L = 1;
  dx = vx / L; dy = vy / L;
}

static void innerDirAt(int idx, float &dx, float &dy) {
  Node &p = gRail.nodes[idx];
  int o = p.owner;
  if (o < 0 || o >= MAX_DEVICES || !gCenK[o]) { dx = 0; dy = 0; return; }
  float vx = gCenX[o] / gCenK[o] - p.x, vy = gCenY[o] / gCenK[o] - p.y;
  float L = hypotf(vx, vy);
  if (L < 1e-6f) L = 1;
  dx = vx / L; dy = vy / L;
}

static void mouthDir(int idx, bool isExit, float &dx, float &dy) {
  float ux, uy, ix, iy;
  baseTangent(idx, ux, uy);
  innerDirAt(idx, ix, iy);
  float pnx = -uy, pny = ux;
  if (pnx * ix + pny * iy < 0) { pnx = -pnx; pny = -pny; }
  float vx = (isExit ? -ux : ux) + pnx, vy = (isExit ? -uy : uy) + pny;
  float L = hypotf(vx, vy); if (L < 1e-6f) L = 1;
  dx = vx / L; dy = vy / L;
}
static void mouthTip(int idx, bool isExit, float &x, float &y) {
  Node &p = gRail.nodes[idx];
  float dx, dy;
  mouthDir(idx, isExit, dx, dy);
  x = p.x + dx * TUNNEL_STUB; y = p.y + dy * TUNNEL_STUB;
}
static float specialClearAt(float x, float y) { return minGap(x, y, gOccupied, gNOccupied, SPECIAL_CLEAR); }
static float mouthStationGap(int idx, bool isExit) {
  float x, y; mouthTip(idx, isExit, x, y);
  return minGap(x, y, gStationPts, gNStationPts, STATION_CLEAR);
}
static float juncClear(int idx) {
  float tx, ty; mouthTip(idx, false, tx, ty);
  return fminf(fminf(placeClear(idx), mouthStationGap(idx, false)), specialClearAt(tx, ty));
}

static int addLink(int a, int b) {
  int li = gRail.nLinks;
  gRail.links[li].a = a;
  gRail.links[li].b = b;
  gRail.nLinks++;
  return li;
}

static int addNode(const Node &n) {
  int id = gRail.nNodes;
  gRail.nodes[id] = n;
  gRail.nNodes++;
  return id;
}

static void buildTunnel(int J, int E) {
  Rail &R = gRail;
  int jid = R.nJunctions;
  Node &pj = R.nodes[J];
  Node &pe = R.nodes[E];

  float uJx, uJy, inJx, inJy;
  baseTangent(J, uJx, uJy);
  innerDirAt(J, inJx, inJy);
  float pnx = -uJy, pny = uJx;
  if (pnx * inJx + pny * inJy < 0) { pnx = -pnx; pny = -pny; }
  float edx = uJx + pnx, edy = uJy + pny;
  float eL = hypotf(edx, edy); if (eL < 1e-6f) eL = 1;
  float inx = edx / eL, iny = edy / eL;
  float mInx = pj.x + inx * TUNNEL_STUB, mIny = pj.y + iny * TUNNEL_STUB;

  float uEx, uEy, inEx, inEy;
  baseTangent(E, uEx, uEy);
  innerDirAt(E, inEx, inEy);
  float pnEx = -uEy, pnEy = uEx;
  if (pnEx * inEx + pnEy * inEy < 0) { pnEx = -pnEx; pnEy = -pnEy; }
  float odx = -uEx + pnEx, ody = -uEy + pnEy;
  float oL = hypotf(odx, ody); if (oL < 1e-6f) oL = 1;
  float outx = odx / oL, outy = ody / oL;
  float mOutx = pe.x + outx * TUNNEL_STUB, mOuty = pe.y + outy * TUNNEL_STUB;

  int ownerJ = R.nodes[J].owner;
  int ownerE = R.nodes[E].owner;
  int mIn = addNode(tunnelMakeNode(mInx, mIny, ownerJ));
  int mInDeep = addNode(tunnelMakeNode(mInx + inx * TUNNEL_INTERIOR, mIny + iny * TUNNEL_INTERIOR, ownerJ));
  int mOut = addNode(tunnelMakeNode(mOutx, mOuty, ownerE));
  int mOutDeep = addNode(tunnelMakeNode(mOutx + outx * TUNNEL_INTERIOR, mOuty + outy * TUNNEL_INTERIOR, ownerE));

  int visIn = addLink(J, mIn);
  int hidIn = addLink(mIn, mInDeep);
  int tele = addLink(mInDeep, mOutDeep);
  int hidOut = addLink(mOutDeep, mOut);
  int visOut = addLink(mOut, E);

  R.hidden[hidIn] = 1; R.hidden[tele] = 1; R.hidden[hidOut] = 1;
  R.teleport[tele] = 1;

  R.nodes[mIn].hasForward = 1; R.nodes[mIn].fLi = hidIn; R.nodes[mIn].fTo = mInDeep;
  R.nodes[mInDeep].hasForward = 1; R.nodes[mInDeep].fLi = tele; R.nodes[mInDeep].fTo = mOutDeep;
  R.nodes[mOutDeep].hasForward = 1; R.nodes[mOutDeep].fLi = hidOut; R.nodes[mOutDeep].fTo = mOut;
  R.nodes[mOut].hasForward = 1; R.nodes[mOut].fLi = visOut; R.nodes[mOut].fTo = E;

  // Mirror of the chain above, so a reversed train entering at E runs the tunnel back to J.
  R.nodes[mOut].hasBack = 1; R.nodes[mOut].bLi = hidOut; R.nodes[mOut].bTo = mOutDeep;
  R.nodes[mOutDeep].hasBack = 1; R.nodes[mOutDeep].bLi = tele; R.nodes[mOutDeep].bTo = mInDeep;
  R.nodes[mInDeep].hasBack = 1; R.nodes[mInDeep].bLi = hidIn; R.nodes[mInDeep].bTo = mIn;
  R.nodes[mIn].hasBack = 1; R.nodes[mIn].bLi = visIn; R.nodes[mIn].bTo = J;

  R.nodes[J].jct = jid;
  R.nodes[E].jctOut = jid;
  Junction &j = R.junctions[jid];
  j.node = J; j.exit = E;
  j.entryLi = visIn; j.entryTo = mIn;
  j.exitLi = visOut; j.exitTo = mOut;
  j.straightTo = (J + 1) % R.N;
  j.mIn = mIn; j.mOut = mOut;
  j.inDirX = inx; j.inDirY = iny;
  j.outDirX = outx; j.outDirY = outy;
  j.active = 0; j.incoming = 0;
  j.colorHue = (int)lroundf(jid * 75.0f) % 360;
  R.nJunctions++;
}

void regenSpecials() {
  Rail &R = gRail;
  R.nNodes = R.permNodes;
  R.nLinks = R.permLinks;
  for (int i = 0; i < R.N; i++) {
    R.nodes[i].jct = -1;
    R.nodes[i].jctOut = -1;
    R.nodes[i].pad = -1;
  }
  R.nJunctions = 0;
  R.nBoosts = 0;
  for (int i = 0; i < MAX_LINKS; i++) { R.hidden[i] = 0; R.teleport[i] = 0; }
  if (R.nStations < 2) return;

  int N = R.N;
  gNCross = 0; gNCorner = 0;
  gBaseCum[0] = 0;
  for (int i = 0; i < N; i++) {
    Node &a = R.nodes[i];
    Node &b = R.nodes[(i + 1) % N];
    if (a.owner != b.owner) gCross[gNCross++] = { (a.x + b.x) / 2, (a.y + b.y) / 2 };
    Node &prev = R.nodes[(i - 1 + N) % N];
    float v1x = a.x - prev.x, v1y = a.y - prev.y;
    float v2x = b.x - a.x, v2y = b.y - a.y;
    float l1 = hypotf(v1x, v1y); if (l1 < 1e-6f) l1 = 1;
    float l2 = hypotf(v2x, v2y); if (l2 < 1e-6f) l2 = 1;
    if ((v1x * v2x + v1y * v2y) / (l1 * l2) < 0.5f) gCorner[gNCorner++] = { a.x, a.y };
    gBaseCum[i + 1] = gBaseCum[i] + hypotf(b.x - a.x, b.y - a.y);
  }
  float Ltot = gBaseCum[N]; if (Ltot < 1e-6f) Ltot = 1;

  gNStationPts = 0;
  for (int i = 0; i < R.nStations; i++) {
    gStationPts[gNStationPts++] = { R.nodes[R.stations[i].a].x, R.nodes[R.stations[i].a].y };
    gStationPts[gNStationPts++] = { R.nodes[R.stations[i].b].x, R.nodes[R.stations[i].b].y };
  }

  static int exitCands[MAX_NODES];
  int nExit = 0;
  for (int i = 0; i < N; i++) {
    if (R.nodes[i].type == 1 || R.nodes[i].spurSeg) continue;
    if (exitClear(i) >= 0) exitCands[nExit++] = i;
  }
  if (nExit == 0) {
    for (int i = 0; i < N; i++) if (R.nodes[i].type != 1 && !R.nodes[i].spurSeg) exitCands[nExit++] = i;
  }

  for (int o = 0; o < MAX_DEVICES; o++) { gCenX[o] = gCenY[o] = 0; gCenK[o] = 0; }
  for (int i = 0; i < N; i++) {
    int o = R.nodes[i].owner;
    if (o < 0 || o >= MAX_DEVICES) continue;
    gCenX[o] += R.nodes[i].x; gCenY[o] += R.nodes[i].y; gCenK[o]++;
  }

  gNOccupied = 0;
  for (int i = 0; i < gNStationPts; i++) occupy(gStationPts[i].x, gStationPts[i].y);
  int jct[MAX_DEVICES];
  for (int o = 0; o < MAX_DEVICES; o++) jct[o] = -1;

  for (int o = 0; o < MAX_DEVICES; o++) {
    int best = -1;
    float bestSc = -1e30f;
    for (int i = 0; i < N; i++) {
      Node &nd = R.nodes[i];
      if (nd.type == 1 || nd.spurSeg || nd.owner != o || placeClear(i) < 0) continue;
      float sc = juncClear(i);
      if (sc > bestSc) { bestSc = sc; best = i; }
    }
    if (best < 0) continue;
    jct[o] = best;
    occupy(R.nodes[best].x, R.nodes[best].y);
    float etx, ety; mouthTip(best, false, etx, ety);
    occupy(etx, ety);
  }

  for (int o = 0; o < MAX_DEVICES; o++) {
    int J = jct[o];
    if (J < 0) continue;
    float target = fmodf(gBaseCum[J] + Ltot / 2, Ltot);
    int exit = -1;
    float bestScore = 1e30f;
    for (int e = 0; e < nExit; e++) {
      int c = exitCands[e];
      if (c == J) continue;
      float d = fabsf(gBaseCum[c] - target);
      d = fmodf(d, Ltot);
      float s = fminf(d, Ltot - d);
      if (R.nodes[c].owner == o) s += Ltot * 0.1f;
      float ctx, cty; mouthTip(c, true, ctx, cty);
      if (specialClearAt(ctx, cty) < 0) s += Ltot;
      if (mouthStationGap(c, true) < 0) s += Ltot;
      if (s < bestScore) { bestScore = s; exit = c; }
    }
    if (exit < 0) continue;
    buildTunnel(J, exit);
    occupy(R.nodes[exit].x, R.nodes[exit].y);
    Junction &j = R.junctions[R.nJunctions - 1];
    occupy(R.nodes[j.mIn].x, R.nodes[j.mIn].y);
    occupy(R.nodes[j.mOut].x, R.nodes[j.mOut].y);
  }

  for (int si = 0; si < R.nSwitches; si++) {
    if (R.nBoosts >= MAX_BOOSTS) break;
    Switch &sw = R.switches[si];
    int o = R.nodes[sw.node].owner;
    int best = -1;
    float mx = R.nodes[sw.node].x + (R.nodes[sw.exit].x - R.nodes[sw.node].x) * BOOST_SPUR_FRAC;
    float my = R.nodes[sw.node].y + (R.nodes[sw.exit].y - R.nodes[sw.node].y) * BOOST_SPUR_FRAC;
    float bestDist = 1e30f;
    for (int i = 0; i < N; i++) {
      Node &nd = R.nodes[i];
      if (!nd.spurSeg || nd.owner != o || i == sw.node || i == sw.exit) continue;
      if (nd.pad >= 0) continue;
      float d = hypotf(nd.x - mx, nd.y - my);
      if (d < bestDist) { bestDist = d; best = i; }
    }
    if (best < 0) {
      int J = (o >= 0 && o < MAX_DEVICES) ? jct[o] : -1;
      float bestSc = -1e30f;
      for (int i = 0; i < N; i++) {
        Node &nd = R.nodes[i];
        if (nd.type == 1 || nd.spurSeg || nd.owner != o || i == J || nd.pad >= 0 || placeClear(i) < 0) continue;
        float sc = fminf(placeClear(i), specialClear(i));
        if (sc < 0) continue;
        if (sc > bestSc) { bestSc = sc; best = i; }
      }
    }
    if (best < 0) continue;
    int pid = R.nBoosts;
    R.nodes[best].pad = pid;
    R.boosts[pid].node = best;
    R.boosts[pid].armed = 0;
    R.boosts[pid].cooldownUntil = 0;
    R.nBoosts++;
    occupy(R.nodes[best].x, R.nodes[best].y);
  }
}
