#include "game.h"

Layout gLayout;

static uint32_t g_rng = 0x1234abcd;

void gameSeed(uint32_t s) { g_rng = s ? s : 1; }

float gameRandom() {
  uint32_t x = g_rng;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  g_rng = x;
  return (x & 0xFFFFFF) / (float)0x1000000;
}

static int oppositeOri(int o) { return o == 0 ? 1 : 0; }

static void rectDims(float shortSide, float longSide, int ori, float &w, float &h) {
  if (ori == 0) {
    w = longSide;
    h = shortSide;
  } else {
    w = shortSide;
    h = longSide;
  }
}

static bool rectsOverlap(float ax, float ay, float aw, float ah, float bx, float by, float bw, float bh) {
  float ix = fminf(ax + aw, bx + bw) - fmaxf(ax, bx);
  float iy = fminf(ay + ah, by + bh) - fmaxf(ay, by);
  return ix > LTOL && iy > LTOL;
}

static bool overlap1d(float a0, float a1, float b0, float b1, float &lo, float &hi) {
  lo = fmaxf(a0, b0);
  hi = fminf(a1, b1);
  return hi - lo > 1.0f;
}

static bool isLongSide(int ori, int side) {
  if (ori == 0) return side == SIDE_TOP || side == SIDE_BOTTOM;
  return side == SIDE_LEFT || side == SIDE_RIGHT;
}

static void usedSideTypes(const Tile &tile, const Tile *tiles, int n, bool &usedLong, bool &usedShort) {
  usedLong = false;
  usedShort = false;
  for (int k = 0; k < n; k++) {
    const Tile &o = tiles[k];
    if (o.phone == tile.phone || o.orientation == tile.orientation) continue;
    float lo, hi;
    if (overlap1d(tile.y, tile.y + tile.h, o.y, o.y + o.h, lo, hi)) {
      if (fabsf(tile.x + tile.w - o.x) <= LTOUCH) {
        if (isLongSide(tile.orientation, SIDE_RIGHT)) usedLong = true; else usedShort = true;
      }
      if (fabsf(o.x + o.w - tile.x) <= LTOUCH) {
        if (isLongSide(tile.orientation, SIDE_LEFT)) usedLong = true; else usedShort = true;
      }
    }
    if (overlap1d(tile.x, tile.x + tile.w, o.x, o.x + o.w, lo, hi)) {
      if (fabsf(tile.y + tile.h - o.y) <= LTOUCH) {
        if (isLongSide(tile.orientation, SIDE_BOTTOM)) usedLong = true; else usedShort = true;
      }
      if (fabsf(o.y + o.h - tile.y) <= LTOUCH) {
        if (isLongSide(tile.orientation, SIDE_TOP)) usedLong = true; else usedShort = true;
      }
    }
  }
}

static Tile placeOne(int phone, float shortSide, float longSide, const Tile *tiles, int n) {
  const int SAMPLES = 15;
  const int SIDES[4] = { SIDE_RIGHT, SIDE_LEFT, SIDE_TOP, SIDE_BOTTOM };
  Tile best;
  best.phone = phone;
  best.orientation = 0;
  best.x = best.y = best.w = best.h = 0;
  float bestScore = 1e30f;
  bool found = false;

  for (int hi = 0; hi < n; hi++) {
    const Tile &host = tiles[hi];
    int o = oppositeOri(host.orientation);
    float cw, ch;
    rectDims(shortSide, longSide, o, cw, ch);
    bool usedLong, usedShort;
    usedSideTypes(host, tiles, n, usedLong, usedShort);

    for (int si = 0; si < 4; si++) {
      int side = SIDES[si];
      bool vertical = (side == SIDE_RIGHT || side == SIDE_LEFT);
      float hostLen = vertical ? host.h : host.w;
      float childLen = vertical ? ch : cw;
      float slack = fabsf(hostLen - childLen);

      for (int s = 0; s <= SAMPLES; s++) {
        float f = (float)s / SAMPLES;
        float x, y;
        if (vertical) {
          x = (side == SIDE_RIGHT) ? host.x + host.w + LGAP : host.x - cw - LGAP;
          y = hostLen >= childLen ? host.y + f * slack : host.y - f * slack;
        } else {
          y = (side == SIDE_BOTTOM) ? host.y + host.h + LGAP : host.y - ch - LGAP;
          x = hostLen >= childLen ? host.x + f * slack : host.x - f * slack;
        }
        bool bad = false;
        for (int k = 0; k < n; k++) {
          if (rectsOverlap(x, y, cw, ch, tiles[k].x, tiles[k].y, tiles[k].w, tiles[k].h)) { bad = true; break; }
        }
        if (bad) continue;

        float minX = x, minY = y, maxX = x + cw, maxY = y + ch;
        for (int k = 0; k < n; k++) {
          minX = fminf(minX, tiles[k].x);
          minY = fminf(minY, tiles[k].y);
          maxX = fmaxf(maxX, tiles[k].x + tiles[k].w);
          maxY = fmaxf(maxY, tiles[k].y + tiles[k].h);
        }
        float bbox = (maxX - minX) * (maxY - minY);

        bool longSide = isLongSide(host.orientation, side);
        float lBonus = 0;
        if (usedShort && !usedLong && longSide) lBonus = -5e8f;
        if (usedLong && !usedShort && !longSide) lBonus = -5e8f;

        float score = lBonus + bbox * (0.6f + gameRandom() * 0.8f);
        if (score < bestScore) {
          bestScore = score;
          best.phone = phone;
          best.orientation = o;
          best.x = x;
          best.y = y;
          best.w = cw;
          best.h = ch;
          found = true;
        }
      }
    }
  }
  (void)found;
  return best;
}

static void packPhones(const PhoneIn *phones, int n) {
  Layout &L = gLayout;
  L.nTiles = 0;
  L.boardW = L.boardH = 0;
  if (n == 0) return;

  for (int i = 0; i < n; i++) {
    float shortSide = fminf(phones[i].w, phones[i].h);
    float longSide = fmaxf(phones[i].w, phones[i].h);
    if (i == 0) {
      Tile t;
      t.phone = phones[0].phone;
      t.orientation = 0;
      rectDims(shortSide, longSide, 0, t.w, t.h);
      t.x = 0;
      t.y = 0;
      L.tiles[0] = t;
      L.nTiles = 1;
    } else {
      L.tiles[L.nTiles] = placeOne(phones[i].phone, shortSide, longSide, L.tiles, L.nTiles);
      L.nTiles++;
    }
  }

  float minX = L.tiles[0].x, minY = L.tiles[0].y;
  for (int i = 1; i < L.nTiles; i++) {
    minX = fminf(minX, L.tiles[i].x);
    minY = fminf(minY, L.tiles[i].y);
  }
  for (int i = 0; i < L.nTiles; i++) {
    L.tiles[i].x -= minX;
    L.tiles[i].y -= minY;
  }
  float bw = 0, bh = 0;
  for (int i = 0; i < L.nTiles; i++) {
    bw = fmaxf(bw, L.tiles[i].x + L.tiles[i].w);
    bh = fmaxf(bh, L.tiles[i].y + L.tiles[i].h);
  }
  L.boardW = bw;
  L.boardH = bh;
}

static int colorFor(int a, int b) {
  Layout &L = gLayout;
  if (a > b) { int t = a; a = b; b = t; }
  for (int i = 0; i < L.nPairs; i++) {
    if (L.pairA[i] == a && L.pairB[i] == b) return i;
  }
  int idx = L.nPairs;
  L.pairA[idx] = a;
  L.pairB[idx] = b;
  L.pairHue[idx] = (int)lroundf(idx * 137.508f) % 360;
  L.nPairs++;
  return idx;
}

static void addEdge(int tile, int side, float lo, float hi, bool axisX, int colorIdx, int neighbor) {
  Layout &L = gLayout;
  if (L.nEdges >= MAX_EDGES) return;
  float origin = axisX ? L.tiles[tile].x : L.tiles[tile].y;
  Edge &e = L.edges[L.nEdges++];
  e.tile = tile;
  e.side = side;
  e.start = lo - origin;
  e.end = hi - origin;
  e.colorIdx = colorIdx;
  e.neighbor = neighbor;
}

static void addSeam(float x0, float y0, float x1, float y1, int colorIdx) {
  Layout &L = gLayout;
  if (L.nSeams >= MAX_SEAMS) return;
  Seam &s = L.seams[L.nSeams++];
  s.x0 = x0; s.y0 = y0; s.x1 = x1; s.y1 = y1; s.colorIdx = colorIdx;
}

static void detectEdges() {
  Layout &L = gLayout;
  L.nEdges = 0;
  L.nSeams = 0;
  L.nPairs = 0;

  for (int i = 0; i < L.nTiles; i++) {
    for (int j = i + 1; j < L.nTiles; j++) {
      Tile &a = L.tiles[i];
      Tile &b = L.tiles[j];
      if (a.orientation == b.orientation) continue;
      int color = colorFor(a.phone, b.phone);

      float lo, hi;
      if (overlap1d(a.y, a.y + a.h, b.y, b.y + b.h, lo, hi)) {
        bool has = true;
        float boundary = 0;
        if (fabsf(a.x + a.w - b.x) <= LTOUCH) {
          addEdge(i, SIDE_RIGHT, lo, hi, false, color, j);
          addEdge(j, SIDE_LEFT, lo, hi, false, color, i);
          boundary = (a.x + a.w + b.x) / 2;
        } else if (fabsf(b.x + b.w - a.x) <= LTOUCH) {
          addEdge(j, SIDE_RIGHT, lo, hi, false, color, i);
          addEdge(i, SIDE_LEFT, lo, hi, false, color, j);
          boundary = (b.x + b.w + a.x) / 2;
        } else {
          has = false;
        }
        if (has) addSeam(boundary, lo, boundary, hi, color);
      }

      if (overlap1d(a.x, a.x + a.w, b.x, b.x + b.w, lo, hi)) {
        bool has = true;
        float boundary = 0;
        if (fabsf(a.y + a.h - b.y) <= LTOUCH) {
          addEdge(i, SIDE_BOTTOM, lo, hi, true, color, j);
          addEdge(j, SIDE_TOP, lo, hi, true, color, i);
          boundary = (a.y + a.h + b.y) / 2;
        } else if (fabsf(b.y + b.h - a.y) <= LTOUCH) {
          addEdge(j, SIDE_BOTTOM, lo, hi, true, color, i);
          addEdge(i, SIDE_TOP, lo, hi, true, color, j);
          boundary = (b.y + b.h + a.y) / 2;
        } else {
          has = false;
        }
        if (has) addSeam(lo, boundary, hi, boundary, color);
      }
    }
  }
}

void buildLayout(const PhoneIn *phones, int n) {
  gameAlloc();
  packPhones(phones, n);
  detectEdges();
}
