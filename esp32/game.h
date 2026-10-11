#pragma once
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>

static const int MAX_DEVICES = 4;
static const int MIN_DEVICES = 2;
static const int LGAP = 28;
static const float LTOUCH = LGAP * 1.8f;
static const float LTOL = 1.0f;

static const int MAX_TILES = MAX_DEVICES;
static const int MAX_EDGES = 64;
static const int MAX_SEAMS = 32;
static const int MAX_PAIRS = 28;

enum Side { SIDE_LEFT = 0, SIDE_RIGHT = 1, SIDE_TOP = 2, SIDE_BOTTOM = 3 };

struct Tile {
  int phone;
  int orientation;
  float x, y, w, h;
};

struct Edge {
  int tile;
  uint8_t side;
  float start, end;
  int colorIdx;
  int neighbor;
};

struct Seam {
  float x0, y0, x1, y1;
  int colorIdx;
};

struct Layout {
  Tile tiles[MAX_TILES];
  int nTiles;
  float boardW, boardH;

  Edge edges[MAX_EDGES];
  int nEdges;
  Seam seams[MAX_SEAMS];
  int nSeams;

  int pairA[MAX_PAIRS], pairB[MAX_PAIRS], pairHue[MAX_PAIRS];
  int nPairs;
};

extern Layout gLayout;

float gameRandom();
void gameSeed(uint32_t s);

struct PhoneIn {
  int phone;
  float w, h;
};

void buildLayout(const PhoneIn *phones, int n);

static const float RAIL_MARGIN = 38.0f;
static const int STATIONS_PER_PHONE = 2;
static const float STATION_LEN = 60.0f;
static const float STATION_CORNER_CLEAR = 40.0f;
static const float STATION_MIN_SEG = STATION_LEN + 2 * STATION_CORNER_CLEAR;
static const float STATION_OK_SEG = STATION_LEN + STATION_CORNER_CLEAR;
static const float STATION_PACK_SEG = 2 * STATION_MIN_SEG;
static const float STATION_FRAC = 0.25f;
static const float NODE_SPACING = 80.0f;
static const int STATION_COLORS = 5;

static const int MAX_NODES = 192;
static const int MAX_LINKS = 216;
static const int MAX_STATIONS = 12;
static const int MAX_SWITCHES = MAX_STATIONS;
static const int MAX_QUEUE = 6;

static const float PLATFORM_LEN = 54.0f;
static const float STATION_INSET = 32.0f;
static const float SPUR_SPAN_TARGET = 150.0f;
static const float GATE_CLEAR = 20.0f;
static const float SPUR_SPAN_MIN = 2 * STATION_INSET + GATE_CLEAR + PLATFORM_LEN;
static const float SPUR_MIN_BASE = SPUR_SPAN_MIN + 2 * STATION_CORNER_CLEAR;
static const float SWITCH_CORNER_COS = 0.8f;
static const float BOOST_SPUR_FRAC = 0.3f;

struct Node {
  float x, y;
  int owner;
  uint8_t drift;
  float px, py, fx, fy, amp;
  uint8_t type;
  int st;
  float nx, ny;
  float dnx, dny;
  int jct;
  int swt;
  // Same special, but reached from its far end -- which is the end a reversed train
  // arrives at. Set on Junction.exit / Switch.exit, -1 elsewhere. int16_t because these
  // are indices into tables capped at MAX_NODES/MAX_LINKS, and Node is allocated 192
  // times inside the heap-resident Rail -- every byte here costs ~192.
  int16_t jctOut;
  int16_t swtOut;
  uint8_t spurSeg;
  int pad;
  uint8_t hasForward;
  int fLi, fTo;
  uint8_t hasBack;
  int16_t bLi, bTo;
};

struct Link {
  int a, b;
};

struct Station {
  int a, b;
  uint8_t open;
  float nx, ny;
  int colorIdx;
  int queue[MAX_QUEUE];
  int qn;
  uint32_t fullSince;
  int clearBy;
  float clearDist;
};

struct Switch {
  int node, exit;
  int entryLi, entryTo;
  int exitLi, exitTo;
  int entryLink;
  int straightLinks[8];
  int nStraight;
  int station;
  // inDir points off the ring into the spur at `node`; outDir does the same at `exit`,
  // which is the end a reversed train diverts from. Mirrors Junction's in/out pair.
  float inDirX, inDirY;
  float outDirX, outDirY;
  uint8_t active;
};

static const float CROSS_CLEAR = 70.0f;
static const float STATION_CLEAR = 70.0f;
static const float CORNER_CLEAR = 55.0f;
static const float SPECIAL_CLEAR = 55.0f;
static const float TUNNEL_STUB = 50.0f;
static const float TUNNEL_INTERIOR = 150.0f;

static const int MAX_JUNCTIONS = MAX_DEVICES;
static const int MAX_BOOSTS = MAX_SWITCHES;

struct Junction {
  int node;
  int exit;
  int entryLi, entryTo;
  int exitLi, exitTo;
  int straightTo;
  int mIn, mOut;
  float inDirX, inDirY, outDirX, outDirY;
  uint8_t active;
  uint8_t incoming;
  int colorHue;
};

struct Boost {
  int node;
  uint8_t armed;
  uint32_t cooldownUntil;
};

struct Rail {
  Node nodes[MAX_NODES];
  int nNodes;
  Link links[MAX_LINKS];
  int nLinks;
  Station stations[MAX_STATIONS];
  int nStations;
  int N;

  Junction junctions[MAX_JUNCTIONS];
  int nJunctions;
  Switch switches[MAX_SWITCHES];
  int nSwitches;
  Boost boosts[MAX_BOOSTS];
  int nBoosts;
  uint8_t hidden[MAX_LINKS];
  uint8_t teleport[MAX_LINKS];
  int permNodes, permLinks;
};

extern Rail *gRailPtr;
#define gRail (*gRailPtr)

bool gameAlloc();
bool buildRail();
void regenSpecials();

static const int CFG_BASE_CARS = 4;
static const int CFG_EXTRA_TRAINS = 1;
static const float CFG_CAR_SPACING = 30.0f;
static const float CFG_CAR_SPACING_MIN = 24.0f;
static const float CFG_CAR_SPACING_MAX = 52.0f;
static const float CFG_CAR_GAP_EASE = 0.08f;
static const float CFG_TRAIN_SPEED = 100.0f;
static const float CFG_TRAIN_SPEED_MAX = 200.0f;
static const float CFG_SPEED_CHAOS_CAP = 1.0f;
static const uint32_t CFG_DERAIL_MS = 4000;
static const float CFG_BOOST_MULT = 3.0f;
static const uint32_t CFG_BOOST_DURATION_MS = 6000;
static const uint32_t CFG_BOOST_COOLDOWN_MS = 8000;
static const uint32_t CFG_TELEPORT_GRACE_MS = 500;
static const float CFG_DRIFT_PX = 18.0f;
static const float CFG_DRIFT_CHAOS_CAP = 1.0f;
static const float CFG_DRIFT_ALONG = 0.25f;
static const float CFG_SPAWN_MS_BY_DEV[3] = {
    14000.0f,
    18000.0f,
    20000.0f,
};
static const float CFG_SPAWN_VAR = 0.2f;
static const float CFG_SPAWN_CHAOS = 1.0f;
static const float CFG_SPAWN_FLATNESS = 1.2f;
static const float CFG_CHAOS_RAMP_MS = 480000.0f;
static const float CFG_CHAOS_TAIL_PER_MIN = 0.08f;
static const uint32_t CFG_ALIGHT_MS = 350;
static const uint32_t CFG_BOARD_MS = 350;
static const int CFG_DELIVERY_POINTS = 10;
static const uint32_t CFG_OVERCROWD_GRACE_MS = 12000;

static const uint32_t CFG_BONUS_PERIOD_MS = 180000;
static const uint32_t CFG_BONUS_FIRST_MS = 90000;
static const uint32_t CFG_BONUS_CHARGE_MS = 8000;
static const uint32_t CFG_BONUS_ACTIVE_MS = 40000;

// Each powerup exists exactly once per match and is dealt to a single phone, so players
// hold different tools.
//
// EVERY TIER BUYS ITS OWN CFG_PU_TIER_MS OF FUEL, and the fuel burns one tier at a time:
// three tiers of boarding speed run level 3, then level 2, then level 1, 30 s apiece. So
// `level` is not stored as a thing that decays -- it is read back out of the clock, as
// ceil(remaining / CFG_PU_TIER_MS), and `activeUntil` is the only state (see
// updatePowerups). There is no cooldown, so score is the only thing rationing a powerup.
//
// This replaced a single fixed window that tiers 2 and 3 did NOT extend. That rule was
// unreadable on the button: buying a tier mid-window spent points and moved nothing, and
// the one ring had to mean "time left" and "tiers bought" at once. Now a tier is always
// worth exactly one more band of ring and one more CFG_PU_TIER_MS, which is what the HUD
// draws -- see the band comment above RING_COLORS in client.js.
//
// It is also the more generous rule, since a late top-up is no longer mostly wasted: at a
// flat price a full stack now delivers roughly 1.4x what it did (boarding speed, summing
// CFG_PU_BOARD_MULT over each tier's 30 s, against the old 40 s at level 3 alone). The
// balance lever is the price curve below, which no longer has to stay flat -- the ring
// bands each tier separately, so tiers may be priced apart without confusing it.
enum PowerupId { PU_BOARD = 0, PU_CARS, PU_FREEZE, PU_TRAIN, PU_REVERSE, PU_COUNT };

static const uint8_t CFG_PU_MAX_LEVEL[PU_COUNT] = { 3, 3, 3, 1, 1 };
static const long CFG_PU_COST[PU_COUNT][3] = {
      { 150, 150, 150 },   // PU_BOARD    — 3 tiers
      { 200, 200, 200 },   // PU_CARS     — 3 tiers
      { 200, 200, 200 },   // PU_FREEZE   — 3 tiers
      { 400,   0,   0 },   // PU_TRAIN    — single tier
      { 200,   0,   0 },   // PU_REVERSE  — single tier
};
// Fuel one tier buys. Total run time is level * this, burnt down a tier at a time.
static const uint32_t CFG_PU_TIER_MS = 30000;
// How long an armed powerup may hold the board waiting for a target tap. Measured in WALL
// ms, not sim ms -- see armWallUntil below.
static const uint32_t CFG_PU_ARM_MS = 8000;

// Boarding/alighting divisor per level: 350 ms -> 280 / 219 / 175 ms per passenger.
static const float CFG_PU_BOARD_MULT[4] = { 1.0f, 1.25f, 1.6f, 2.0f };

// How many stations PU_FREEZE holds at once, one per level.
static const int MAX_FROZEN = 3;

struct Powerup {
  uint8_t level;        // 0 = dormant. Derived from activeUntil every tick, never free-standing
  uint32_t activeUntil; // when the LAST tier of fuel runs out; each buy pushes it back one tier
  int8_t owner;         // phone slot whose HUD shows this button, -1 if unowned
  uint8_t armed;        // awaiting a target tap (PU_FREEZE / PU_TRAIN only)
  // Arming stops sim time, and activeUntil is expressed IN sim time -- so an arm nobody
  // answers can never expire and the board stays frozen for every player with no way out.
  // This deadline is therefore the one powerup field kept in wall ms; it must never be fed
  // through simClock. Enforced by simCheckArmLapse().
  uint32_t armWallUntil;
};

static const int MAX_TRAINS = MAX_DEVICES + 2;
static const int MAX_CARS = CFG_BASE_CARS + 3;

struct Train {
  int li, to;
  float prog;
  uint8_t stopped;
  int station;
  uint8_t svcActive;
  int svcPhase;
  uint32_t svcLast;
  uint8_t nearLead;
  uint32_t derailUntil;
  int color;
  uint8_t full;
  uint32_t boostUntil;
  uint32_t ghostUntil;
  int tunnelJct;
  // Spacing between cars, eased toward the current speed. The ONLY car-position state the
  // server keeps: the client walks back along the rail from the head by multiples of this
  // (see pointBehind in client.js), so no per-car coordinates are computed or sent.
  float gap;
  uint8_t hasPrev;
  float prevX, prevY;
  int cars[MAX_CARS];
  int nCars;
  uint8_t derailedFlag;
  float vsend;
};

extern Train *gTrains;
extern int gNTrains;
extern long gScore;
extern long gTotalScore;
extern Powerup gPU[PU_COUNT];
extern uint8_t gDirRev;
extern float gChaos;
extern bool gGameOver;

extern int gBonusColorIdx;
extern int gBonusPhase;
extern uint32_t gBonusChargeStart;

extern int gFrozen[MAX_FROZEN];
extern int gNFrozen;
bool stationFrozen(int s);

void simInit(int deviceCount, uint32_t now);
bool simTick(uint32_t now);
uint32_t simGameStartMs();
float simNodeX(int i);
float simNodeY(int i);
void simStationToggle(int id);
void simJunctionToggle(int id);
void simSwitchToggle(int id);
void simBoostArm(int id, uint32_t now);
// Sim time: wall time minus everything spent paused waiting for a powerup target. EVERY
// value handed to a sim* entry point must come through this, or its deadline lands in a
// different clock from the one simTick reads.
uint32_t simClock(uint32_t wall);
bool simAwaitingTarget();
long puCost(int id);
// `wall` is millis(); `now` is simClock(millis()). Arming needs both -- see Powerup.armWallUntil.
void simBuyPowerup(int id, uint32_t now, uint32_t wall);
// Clears any armed powerup whose wall deadline has passed, refunding the level it bought.
// MUST be called before this iteration's simClock(), or the clock cannot resume until the
// next pass. Returns true if anything lapsed.
bool simCheckArmLapse(uint32_t wall);
// Gives up on an armed powerup before its deadline -- the cancel on the "paused" prompt.
// Refunds exactly as a lapse does. id < 0 means "whatever is armed". Returns true if
// something was armed. Must be applied in the same window as simCheckArmLapse(), i.e.
// before this iteration's simClock(), or sim time cannot resume until the next pass.
bool simCancelArmedPowerup(int id);
void simPowerupTarget(int id, int target, uint32_t now);
void simDealPowerups(const int *slots, int nSlots);
// Points scored per phone slot since the last clear, for the client's "+N" marker. Credit
// goes to the owner of the TILE the station sits on -- gRail.nodes[].owner is already a
// phone slot (rail.cpp traces the loop from gLayout.tiles[].phone), so it matches the
// "you":{"slot"} the layout message carries. Peek/clear are separate because the caller
// only clears once the message carrying them actually fits in the buffer.
void simPeekScoreEvents(long *out);
void simClearScoreEvents();
