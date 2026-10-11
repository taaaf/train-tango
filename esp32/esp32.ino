#include <WiFi.h>
#include <LittleFS.h>
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"   // esp_wifi_ap_get_sta_list, for the [phy] diagnostic
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <new>
#include "game.h"
#include "protocol.h"

#define Log Serial

// heap_caps_check_integrity_all() walks every block in every heap region; it is a
// debug tool, not a runtime health check, and it runs inside the game loop.
#ifndef DEBUG_HEAP_CHECK
#define DEBUG_HEAP_CHECK 0
#endif
// [blob] length/heap line on every layout/drift send. Sizes MAX_BLOBS_IN_FLIGHT for
// the per-client gate (LAG-REPORT §1.3). Those sizes are now known (§0), and the line calls
// ESP.getMaxAllocHeap() -- a free-list walk -- on every blob send, so it is off.
#ifndef DEBUG_BLOB_LEN
#define DEBUG_BLOB_LEN 0
#endif
// [clients] + [phy] logging and the per-phone `cstat` message. This is the g-vs-s
// discriminator of LAG-REPORT §4, so it is GATED rather than deleted -- turn it on here AND
// in client.js for any session you intend to diagnose. Off it costs nothing: no inbound
// JSON parse on the priority-10 AsyncTCP task, no esp_wifi_ap_get_sta_list(), no uplink.
#ifndef DEBUG_CLIENT_STATS
#define DEBUG_CLIENT_STATS 0
#endif

static const char *AP_SSID = "Connect & go to 4.3.2.1 to play";

static const char *PALETTE[8] = {
  "#e74c3c", "#3498db", "#2ecc71", "#f1c40f",
  "#9b59b6", "#e67e22", "#1abc9c", "#ff6ec7",
};

static const uint32_t DISCONNECT_GRACE_MS = 15000;
static const uint16_t WS_KEEPALIVE_S = 5;

struct Phone {
  bool used = false;
  bool connected = false;
  uint32_t disconnectedAt = 0;
  uint32_t clientId = 0;
  char token[28] = {0};
  char id[6] = {0};
  const char *color = "#fff";
  int w = 0;
  int h = 0;
  bool hasScreen = false;
  bool ready = false;
  bool placed = false;
  bool needsLayout = false;
  bool needsDrift = false;
  // millis() when a layout/drift blob was queued for this phone, 0 if none.
  // Gates realtime sends for THIS phone only (see LAG-REPORT §1.3).
  uint32_t blobSentAt = 0;
  // Snapshots this phone was NOT sent because its queue was full or the socket had no
  // room. Compare against dbgGaps (what the CLIENT noticed missing): if they track each
  // other the loss is our own suppression; if gaps run ahead, the radio is losing frames.
  uint16_t dbgSkips = 0;
#if DEBUG_CLIENT_STATS
  // diagnostics reported by the client in its "cstat" message (see LAG-REPORT §4)
  uint16_t dbgFrameMax = 0;  // worst stepTrains+draw, ms
  uint16_t dbgFps = 0;       // rAF frames per second
  uint16_t dbgGaps = 0;      // trains snapshots missing from the seq stream
#endif
};

AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
static const IPAddress AP_IP(4, 3, 2, 1);
static const int AP_MAX_CONN = 8;

Phone phones[MAX_DEVICES];
int nextId = 1;
const char *phase = "lobby";

uint32_t lastTick = 0;
// The sim tick is scheduled from its own SLOT, not from "33 ms after the last one actually
// ran". A tick that overruns no longer pushes every tick after it back with it -- that
// accumulating drift is what gTickOverruns has been counting.
static const uint32_t TICK_PERIOD_MS = 33;
uint32_t gNextTickAt = 0;
// How often gameTask wakes. 2 ms, matching the old delay(2) at the bottom of loop(): the
// sim ticks every 33 ms but reapBlobs/drainLayouts want to move a queued blob on as soon
// as the socket drains, and waking only per tick would add up to 33 ms to every blob.
static const uint32_t GAME_TASK_PERIOD_MS = 2;
static void gameTask(void *);
int lastChaosStep = -1;
volatile bool pendingStart = false;
volatile bool pendingGo = false;
volatile bool pendingRetryKeep = false;
volatile bool pendingRetryNew = false;
// "reset" used to call resetToLobby() inline on the AsyncTCP task, tearing down sim state
// from under a running tick. Deferred like its three siblings.
volatile bool pendingReset = false;
int gGameoverScore = 0;
uint32_t gGameoverTimeMs = 0;
bool gStartGate = false;
uint32_t gStartGateAt = 0;
static const uint32_t START_GATE_TIMEOUT = 6000;
uint32_t tickCount = 0;
static const int TRAINS_SEND_EVERY = 6;
static const int WORLD_SEND_EVERY = 6;
static const size_t WS_QUEUE_SOFT_LIMIT = 2;
static const size_t WS_QUEUE_WORLD_LIMIT = 6;
// Trains snapshots only. Was sharing WS_QUEUE_SOFT_LIMIT = 2, which meant that the moment
// the radio stalled, every phone's queue hit the limit and the snapshot was discarded for
// all of them at once (three phones were observed losing the same snapshot with identical
// queue depth and TCP space). Allowing one more to stack delivers it ~66 ms late instead
// of never. Deliberately NOT raising WS_QUEUE_SOFT_LIMIT: that also gates the large blob
// sends, which should still wait for a clear link.
static const size_t WS_QUEUE_TRAINS_LIMIT = 3;

// What a realtime send is, which decides its queue limit, whether it yields to a draining
// blob, and how a discard is counted. Keeping these three together stops them being
// mismatched at a call site.
//
// Only a discarded `trains` snapshot can appear as a seq gap on the phone, so only that
// one is comparable with dbgGaps. Counting world and lobby discards in the same total
// (the original mistake) inflated it: one log sample read /g0/s20 — twenty discards, zero
// gaps — because they were world updates the phone had no way to notice missing.
enum SendKind : uint8_t {
  SEND_TRAINS,  // position snapshot; superseded ~198 ms later, and visible to the client
  SEND_WORLD,   // UI state; also superseded, but carries no seq so the client cannot see a loss
  SEND_LOBBY,   // roster; NOT resent on a cadence, so a discard leaves that phone stale
};
static uint32_t gSkipTrains = 0;
static uint32_t gSkipWorld = 0;
static uint32_t gSkipLobby = 0;

int phoneIndexByClient(uint32_t clientId) {
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (phones[i].used && phones[i].connected && phones[i].clientId == clientId) return i;
  }
  return -1;
}

int phoneIndexByToken(const char *token) {
  if (!token || !token[0]) return -1;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (phones[i].used && strncmp(phones[i].token, token, sizeof(phones[i].token)) == 0) return i;
  }
  return -1;
}

int connectedCount() {
  int n = 0;
  for (int i = 0; i < MAX_DEVICES; i++) if (phones[i].used && phones[i].connected) n++;
  return n;
}

const char *pickColor() {
  for (int p = 0; p < 8; p++) {
    bool taken = false;
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (phones[i].used && phones[i].color == PALETTE[p]) { taken = true; break; }
    }
    if (!taken) return PALETTE[p];
  }
  return PALETTE[connectedCount() % 8];
}

static uint32_t gDroppedSends = 0;
static uint32_t gHeapBails = 0;

static bool heapCanSend(size_t len) {
  return ESP.getMaxAllocHeap() >= len + 16384 && ESP.getFreeHeap() >= len + 24576;
}

// Stricter guard for the 3-8 KB layout/drift blobs. getMaxAllocHeap() alone does
// not predict whether one of these can be framed: it read a steady 45 KB through a
// session where 6.9 KB sends were throwing bad_alloc 154 times, because TOTAL free heap
// had collapsed to 54 KB (the since-removed music patch cost ~38 KB of headroom versus a
// 5-instrument one). AsyncWebSocket needs materially more than `len` to frame and buffer
// a message this size. Refusing costs nothing: the needs* flag stays set and drainLayouts
// retries next loop() — which is far cheaper than throwing and catching bad_alloc.
static uint32_t gBlobBails = 0;
static bool heapCanSendBlob(size_t len) {
  if (heapCanSend(len) && ESP.getFreeHeap() >= len + 65536) return true;
  gBlobBails++;
  return false;
}

static inline void logBlob(const char *kind, size_t n) {
#if DEBUG_BLOB_LEN
  Log.printf("[blob] %s len=%u maxblk=%u\n", kind, (unsigned)n, ESP.getMaxAllocHeap());
#else
  (void)kind; (void)n;
#endif
}

void sendJson(AsyncWebSocketClient *client, JsonDocument &doc) {
  try {
    String out;
    serializeJson(doc, out);
    if (!heapCanSend(out.length())) return;
    client->text(out);
  } catch (const std::bad_alloc &) {
    gDroppedSends++;
  }
}

void broadcastJson(JsonDocument &doc) {
  try {
    String out;
    serializeJson(doc, out);
    if (!heapCanSend(out.length())) return;
    ws.textAll(out);
  } catch (const std::bad_alloc &) {
    gDroppedSends++;
  }
}

void sendTextToReady(const char *s, SendKind kind) {
  // Snapshots yield to a phone draining a blob, because the next one supersedes anything
  // missed. The lobby roster must NOT: it is not resent on a cadence, so skipping it
  // leaves that phone showing a stale roster indefinitely.
  const size_t queueLimit = (kind == SEND_TRAINS) ? WS_QUEUE_TRAINS_LIMIT : WS_QUEUE_WORLD_LIMIT;
  const bool yieldToBlob = (kind != SEND_LOBBY);
  size_t len = strlen(s);
  if (len == 0) return;
  // A blob in flight shrinks the largest free block; below ~32 KB this stops realtime
  // sends for *everyone*, which the per-client gate would not fix (LAG-REPORT §1.3).
  if (!heapCanSend(len)) { gHeapBails++; return; }
  try {
    AsyncWebSocketSharedBuffer buf =
        std::make_shared<std::vector<uint8_t>>((const uint8_t *)s, (const uint8_t *)s + len);
    for (int i = 0; i < MAX_DEVICES; i++) {
      if (!phones[i].used || !phones[i].connected || !phones[i].hasScreen) continue;
      // Let this phone's blob drain first — but only this phone. Previously a single
      // global flag stopped the feed for everyone (LAG-REPORT §1.3).
      if (yieldToBlob && phones[i].blobSentAt) continue;
      AsyncWebSocketClient *cl = ws.client(phones[i].clientId);
      if (!cl) continue;
      // Count the two silent discards. gDroppedSends only ever counted allocation
      // failures, so [status] read drops=0 while snapshots were being binned every
      // time the radio stalled — the loss was only visible from the phones.
      bool noRoom = cl->queueLen() >= queueLimit;
      AsyncClient *tcp = cl->client();
      if (!noRoom && tcp && tcp->space() < len) noRoom = true;
      if (noRoom) {
        if (kind == SEND_TRAINS) { gSkipTrains++; phones[i].dbgSkips++; }
        else if (kind == SEND_WORLD) gSkipWorld++;
        else gSkipLobby++;
        continue;
      }
      cl->text(buf);
    }
  } catch (const std::bad_alloc &) {
    gDroppedSends++;
  }
}

void broadcastLobby() {
  JsonDocument doc;
  doc["type"] = "lobby";
  doc["phase"] = phase;
  JsonArray arr = doc["phones"].to<JsonArray>();
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!phones[i].used || !phones[i].connected) continue;
    JsonObject o = arr.add<JsonObject>();
    // The slot, not this array's index: the array is packed (disconnected phones are
    // skipped) so position is not stable, and the lobby keys each player's SHAPE off this.
    // It is the same index "you":{"slot"} carries, so lobby and HUD agree on the shape.
    o["slot"] = i;
    o["id"] = phones[i].id;
    o["color"] = phones[i].color;
    o["ready"] = phones[i].ready;
    o["hasScreen"] = phones[i].hasScreen;
    if (phones[i].hasScreen) {
      o["w"] = phones[i].w;
      o["h"] = phones[i].h;
    } else {
      o["w"] = nullptr;
      o["h"] = nullptr;
    }
  }
  String out;
  serializeJson(doc, out);
  sendTextToReady(out.c_str(), SEND_LOBBY);
}

bool allReady() {
  int n = 0;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!phones[i].used || !phones[i].connected) continue;
    n++;
    if (!phones[i].ready || !phones[i].hasScreen) return false;
  }
  return n >= MIN_DEVICES;
}

bool allPlaced() {
  int n = 0;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!phones[i].used || !phones[i].connected || !phones[i].hasScreen) continue;
    n++;
    if (!phones[i].placed) return false;
  }
  return n > 0;
}

// Number of phones the current match was built for. Set at placement, read by simInit and
// the layout/drift builders.
int gDeviceCount = 0;

// The largest message in the protocol, and the source of the heap pressure behind
// LAG-REPORT §1.5. Sent once per phone per match, and again on every page reload.
//
// Still JSON (sent once, not 5x/second), but the bulk arrays are flattened and the
// per-object `id` dropped -- the array index is the id. client.js rehydrates all of it in
// its "layout" handler, so no drawing code depends on the wire shape:
//   nodes  [x,y,x,y,...]  integer px      links  [a,b,a,b,...]
//   edges  carry `hue`, not an "hsl(...)" string
// Do not re-add tiles[]/seams[]/you.id/you.color/edges[].neighbor/junctions[].color: all
// were dead payload. See TRIMMED.md §2b.
bool sendLayoutTo(int slot) {
  AsyncWebSocketClient *cl = ws.client(phones[slot].clientId);
  if (!cl) return false;
  if (!buildLayoutMsg(slot, strcmp(phase, "placing") == 0, phones[slot].placed)) return false;
  size_t n = (size_t)gProtoLen;
  if (!heapCanSendBlob(n) || cl->queueLen() >= WS_QUEUE_SOFT_LIMIT) return false;
  try {
    cl->text((const char *)gProtoBuf, n);
  } catch (const std::bad_alloc &) {
    gDroppedSends++;
    return false;
  }
  logBlob("layout", n);
  return true;
}

bool sendDriftTo(int slot) {
  AsyncWebSocketClient *cl = ws.client(phones[slot].clientId);
  if (!cl) return false;
  if (!buildDriftMsg()) return false;
  size_t n = (size_t)gProtoLen;
  if (!heapCanSendBlob(n) || cl->queueLen() >= WS_QUEUE_SOFT_LIMIT) return false;
  try {
    cl->text((const char *)gProtoBuf, n);
  } catch (const std::bad_alloc &) {
    gDroppedSends++;
    return false;
  }
  logBlob("drift", n);
  return true;
}

void broadcastLayout() {
  for (int i = 0; i < MAX_DEVICES; i++)
    if (phones[i].used && phones[i].hasScreen) phones[i].needsLayout = true;
}

bool layoutsPending() {
  for (int i = 0; i < MAX_DEVICES; i++)
    if (phones[i].used && phones[i].connected && phones[i].hasScreen &&
        (phones[i].needsLayout || phones[i].needsDrift)) return true;
  return false;
}

static const uint32_t LAYOUT_DRAIN_TIMEOUT = 800;
// How many phones may have a blob draining at once. Was MAX_DEVICES, sized against
// 2-phone blobs (~3.8 KB). At 4 phones a layout is ~7 KB, and four of those starting
// within ~8 ms at placement drove free heap to 54 KB, 154 bad_alloc drops, and one
// websocket framing error (code 1002) that disconnected two players. Two concurrent is
// ~14 KB and costs almost nothing: a blob drains in a few ms, so the third and fourth
// phone follow a couple of loop() iterations later.
static const int MAX_BLOBS_IN_FLIGHT = 2;
static int gBlobsInFlight = 0;

// Clear the in-flight marker for every phone whose blob has drained, timed out, or
// whose socket went away, and recount. MUST run over ALL phones every loop(), not
// just the ones with needs* flags set: once a phone's last blob is sent and its
// flags are false, no other code path would ever look at it again, and a stuck
// blobSentAt would mute that player's realtime feed permanently.
static void reapBlobs(uint32_t now) {
  int n = 0;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!phones[i].blobSentAt) continue;
    if (!phones[i].used || !phones[i].connected) { phones[i].blobSentAt = 0; continue; }
    AsyncWebSocketClient *cl = ws.client(phones[i].clientId);
    if (cl && cl->queueLen() > 0 && (now - phones[i].blobSentAt) < LAYOUT_DRAIN_TIMEOUT) { n++; continue; }
    phones[i].blobSentAt = 0;
  }
  gBlobsInFlight = n;
}

static void clearBlobs() {
  for (int i = 0; i < MAX_DEVICES; i++) phones[i].blobSentAt = 0;
  gBlobsInFlight = 0;
}

// Sends at most one blob per call (loop() runs every ~2 ms, so four phones each get
// one started within ~8 ms) but skips phones already draining, so one phone's blob
// no longer blocks another's.
void drainLayouts(uint32_t now) {
  if (gBlobsInFlight >= MAX_BLOBS_IN_FLIGHT) return;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!phones[i].used || !phones[i].connected || !phones[i].hasScreen || !phones[i].needsDrift) continue;
    if (phones[i].blobSentAt) continue;
    if (sendDriftTo(i)) {
      phones[i].needsDrift = false;
      phones[i].blobSentAt = now ? now : 1;
      gBlobsInFlight++;
    }
    return;
  }
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!phones[i].used || !phones[i].connected || !phones[i].hasScreen || !phones[i].needsLayout) continue;
    if (phones[i].blobSentAt) continue;
    if (sendLayoutTo(i)) {
      phones[i].needsLayout = false;
      if (strcmp(phase, "placing") != 0) phones[i].needsDrift = true;
      phones[i].blobSentAt = now ? now : 1;
      gBlobsInFlight++;
    }
    return;
  }
}

// Compact positional text, not JSON -- still a WS_TEXT frame, just without the punctuation.
// LAG-REPORT §2.1 measured the 4-phone constraint as transmit CAPACITY, so bytes on air are
// what matter; §2.7's "no binary protocol" is not violated. Rationale in TRIMMED.md §2b.
//
//   T|seq|t|score|total|rev|<train>;<train>;...
//   train = li,to,pr,sp,gap,col,flags,bo,pax...      (8 fixed fields, then one per car)
//   col  = -1 for none          pax entry = -1 for empty
//   flags = full | stopped<<1 | derailed<<2
//   pr,gap = integer px (the client extrapolates to a 90 px tolerance, so 0.1 px was noise)
//   bo     = integer percent of the boost window remaining
//
// Decoder: parseTrains() in client.js. Keep the two in step -- field ORDER is the contract.

void buildAndPlace() {
  PhoneIn pin[MAX_DEVICES];
  int n = 0;
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (phones[i].used && phones[i].hasScreen) {
      phones[i].placed = false;
      pin[n].phone = i;
      pin[n].w = phones[i].w;
      pin[n].h = phones[i].h;
      n++;
    }
  }
  if (n == 0) return;
  gDeviceCount = n;

  if (!gameAlloc()) {
    Log.printf("[mem] gameAlloc FAILED at beginPlacement, heap=%u largest=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    return;
  }

  gameSeed(esp_random() ^ micros());
  buildLayout(pin, n);
  buildRail();
  regenSpecials();

  // Deal one of each powerup to a single phone, reshuffled every match. A phone that
  // drops mid-match trips reapDisconnected -> resetToLobby, so an owner cannot go away
  // underneath a live match and orphan its powerup.
  int puSlots[MAX_DEVICES];
  for (int k = 0; k < n; k++) puSlots[k] = pin[k].phone;
  simDealPowerups(puSlots, n);
  gGameOver = false;
  phase = "placing";
  broadcastLayout();
  clearBlobs();
  Log.printf("placement: %d devices, %d nodes, %d junctions\n", n, gRail.nNodes, gRail.nJunctions);
}

void beginPlacement() {
  if (strcmp(phase, "lobby") != 0) return;
  buildAndPlace();
}

void resetRailDynamic() {
  Rail &R = gRail;
  for (int i = 0; i < R.nStations; i++) { R.stations[i].open = 0; R.stations[i].qn = 0; }
  for (int i = 0; i < R.nSwitches; i++) R.switches[i].active = 0;
  for (int i = 0; i < R.nJunctions; i++) { R.junctions[i].active = 0; R.junctions[i].incoming = 0; }
  for (int i = 0; i < R.nBoosts; i++) { R.boosts[i].armed = 0; R.boosts[i].cooldownUntil = 0; }
}

void restartKeepLayout() {
  resetRailDynamic();
  simInit(gDeviceCount, simClock(millis()));
  phase = "play";
  lastTick = millis();
  gNextTickAt = lastTick + TICK_PERIOD_MS;
  lastChaosStep = -1;
  JsonDocument d;
  d["type"] = "start";
  broadcastJson(d);
  broadcastLayout();
  clearBlobs();
  gStartGate = true;
  gStartGateAt = millis();
  Log.printf("retry (keep layout): %d devices\n", gDeviceCount);
}

void startGame() {
  if (strcmp(phase, "placing") != 0) return;
  simInit(gDeviceCount, simClock(millis()));
  phase = "play";
  lastTick = millis();
  gNextTickAt = lastTick + TICK_PERIOD_MS;
  lastChaosStep = -1;
  JsonDocument d;
  d["type"] = "start";
  broadcastJson(d);
  for (int i = 0; i < MAX_DEVICES; i++)
    if (phones[i].used && phones[i].connected && phones[i].hasScreen) phones[i].needsDrift = true;
  gStartGate = true;
  gStartGateAt = millis();
  Log.printf("game started: %d devices\n", gDeviceCount);
}

void resetToLobby() {
  Log.printf("resetToLobby (was %s)\n", phase);
  phase = "lobby";
  gStartGate = false;
  clearBlobs();
  gGameOver = false;
  for (int i = 0; i < MAX_DEVICES; i++) { phones[i].ready = false; phones[i].placed = false; }
  broadcastLobby();
}

static const char HOLDING_HTML[] PROGMEM =
  "<!doctype html><html><head><meta charset=utf-8>"
  "<meta name=viewport content=\"width=device-width,initial-scale=1,viewport-fit=cover\">"
  "<title>Train Tango</title><style>"
  "@font-face{font-family:HostGrotesk;src:url(\"HostGrotesk-SemiBold.woff2\") format(\"woff2\")}"
  "html,body{margin:0;height:100%}"
  "body{display:flex;flex-direction:column;align-items:center;justify-content:center;gap:4rem;"
  "background:#000;color:#fff;font-family:HostGrotesk,system-ui,sans-serif;text-align:center;padding:2rem;box-sizing:border-box}"
  ".logo{width:min(86vw,640px);filter:brightness(0) invert(1)}"
  "#msg{font-size:1rem;font-weight:600;line-height:1.5;max-width:22rem;margin:0}"
  "#join{display:none;position:absolute;bottom:max(18vh,calc(2rem + env(safe-area-inset-bottom)));"
  "font-family:inherit;font-size:1rem;font-weight:600;line-height:1;"
  "padding:.6rem 1.2rem;border:1.5px solid #fff;border-radius:.55rem;background:transparent;color:#fff;cursor:pointer}"
  "#join.show{display:inline-block}"
  "</style></head><body>"
  "<img class=logo src=\"title.svg\" alt=\"Train Tango\">"
  "<p id=msg>A game is in progress.<br>You will be able to join once it finishes.</p>"
  "<button id=join>tap to join</button><script>"
  "var done=0,b=document.getElementById('join'),m=document.getElementById('msg');"
  "b.addEventListener('click',function(){location.replace('/')});"
  "setInterval(function(){if(done)return;fetch('/state').then(function(r){return r.text()})"
  ".then(function(p){if(p.trim()==='lobby'){done=1;m.style.display='none';b.className='show'}}).catch(function(){})},10000);"
  "</script></body></html>";

String cookieToken(AsyncWebServerRequest *req) {
  const AsyncWebHeader *h = req->getHeader("Cookie");
  if (!h) return String();
  String c = h->value();
  int k = c.indexOf("tg_token=");
  if (k < 0) return String();
  k += 9;
  int end = c.indexOf(';', k);
  return end < 0 ? c.substring(k) : c.substring(k, end);
}

void onWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_AP_STACONNECTED) {
    const uint8_t *m = info.wifi_ap_staconnected.mac;
    Log.printf("wifi: STA assoc %02x:%02x:%02x aid=%u apSta=%u\n",
               m[3], m[4], m[5], info.wifi_ap_staconnected.aid, WiFi.softAPgetStationNum());
  } else if (event == ARDUINO_EVENT_WIFI_AP_STADISCONNECTED) {
    const uint8_t *m = info.wifi_ap_stadisconnected.mac;
    Log.printf("wifi: STA disassoc %02x:%02x:%02x aid=%u reason=%u apSta=%u\n",
               m[3], m[4], m[5], info.wifi_ap_stadisconnected.aid,
               info.wifi_ap_stadisconnected.reason, WiFi.softAPgetStationNum());
  } else if (event == ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED) {
    const uint8_t *m = info.wifi_ap_staipassigned.mac;
    Log.printf("wifi: STA IP %s -> %02x:%02x:%02x\n",
               IPAddress(info.wifi_ap_staipassigned.ip.addr).toString().c_str(), m[3], m[4], m[5]);
  }
}

void onConnect(AsyncWebSocketClient *client) {
  client->keepAlivePeriod(WS_KEEPALIVE_S);
  Log.printf("ws connect id=%u\n", client->id());
}

void onDisconnect(uint32_t clientId, size_t qlen, uint32_t heapAtClose) {
  int i = phoneIndexByClient(clientId);
  if (i < 0) {
    Log.printf("ws disconnect id=%u (no slot) qlen=%u heap=%u\n", clientId, (unsigned)qlen, heapAtClose);
    return;
  }
  // Stamp the time BEFORE clearing `connected`, so the game task cannot catch the pair
  // half-updated: reapDisconnected() only acts once connected is false, and by then the
  // timestamp it reads is this one rather than whatever was in the field before.
  phones[i].disconnectedAt = millis();
  __sync_synchronize();
  phones[i].connected = false;
  Log.printf("ws disconnect %s (id=%u) qlen=%u heap=%u\n", phones[i].id, clientId, (unsigned)qlen, heapAtClose);
  if (strcmp(phase, "lobby") == 0) broadcastLobby();
}

void handleHello(AsyncWebSocketClient *client, JsonDocument &doc) {
  const char *token = doc["token"] | "";
  JsonObject s = doc["screen"];
  int w = s["w"] | 0;
  int h = s["h"] | 0;

  int i = phoneIndexByToken(token);
  bool rebind = i >= 0;
  if (i >= 0) {
    uint32_t oldId = phones[i].clientId;
    // Keeps the invariant the reap guard relies on: disconnectedAt is non-zero only while
    // the phone is actually away. onDisconnect() restamps it if this socket drops again.
    phones[i].disconnectedAt = 0;
    phones[i].connected = true;
    phones[i].clientId = client->id();
    phones[i].w = w;
    phones[i].h = h;
    phones[i].hasScreen = true;
    // The old socket is about to close, so any blob queued on it is gone. Clear the
    // marker or this phone would stay muted until LAYOUT_DRAIN_TIMEOUT expired.
    phones[i].blobSentAt = 0;
    // A rebind is a fresh page load, so the client's own gap counter restarted. Reset
    // ours too or the two stop being comparable.
    phones[i].dbgSkips = 0;
    if (oldId && oldId != client->id()) {
      AsyncWebSocketClient *old = ws.client(oldId);
      if (old) old->close();
    }
    Log.printf("rebind %s (id=%u)\n", phones[i].id, client->id());
  } else {
    if (strcmp(phase, "lobby") != 0) {
      Log.printf("busy: refused new device id=%u (%s in progress)\n", client->id(), phase);
      JsonDocument d;
      d["type"] = "busy";
      sendJson(client, d);
      client->close();
      return;
    }
    if (connectedCount() >= MAX_DEVICES) {
      JsonDocument d;
      d["type"] = "full";
      d["max"] = MAX_DEVICES;
      sendJson(client, d);
      client->close();
      return;
    }
    int slot = -1;
    for (int k = 0; k < MAX_DEVICES; k++) if (!phones[k].used) { slot = k; break; }
    if (slot < 0) return;
    Phone &p = phones[slot];
    p.connected = true;
    p.clientId = client->id();
    strncpy(p.token, token, sizeof(p.token) - 1);
    p.token[sizeof(p.token) - 1] = 0;
    snprintf(p.id, sizeof(p.id), "P%d", nextId++);
    p.color = pickColor();
    p.w = w;
    p.h = h;
    p.hasScreen = true;
    p.ready = false;
    p.blobSentAt = 0;   // slots are reused; never inherit a previous occupant's marker
    p.dbgSkips = 0;
    // Same reasoning as blobSentAt: a reused slot still carries the moment its PREVIOUS
    // occupant dropped, and reapDisconnected() reads that against DISCONNECT_GRACE_MS.
    p.disconnectedAt = 0;
#if DEBUG_CLIENT_STATS
    p.dbgGaps = 0;
    p.dbgFrameMax = 0;
    p.dbgFps = 0;
#endif
    // `used` is published LAST, once every field above is in place. It used to be the
    // FIRST store, which left a window -- two instructions wide, but reapDisconnected()
    // runs on the game task every 2 ms -- where that task saw used=true, connected=false,
    // and a disconnectedAt belonging to someone else (or 0 on a never-used slot, which
    // reads as "dropped at boot", i.e. always expired). It would reap the slot out from
    // under this function, log the PREVIOUS occupant's id, and leave the joining phone
    // holding a welcome for a slot marked free -- which the next phone to join is then
    // handed as well, putting two phones on one slot.
    __sync_synchronize();
    p.used = true;
    i = slot;
    Log.printf("new %s (id=%u)\n", p.id, client->id());
  }

  JsonDocument wel;
  wel["type"] = "welcome";
  wel["id"] = phones[i].id;
  wel["color"] = phones[i].color;
  sendJson(client, wel);

  if (strcmp(phase, "lobby") != 0) {
    bool clientHasLayout = doc["hasLayout"] | false;
    if (strcmp(phase, "play") == 0 && rebind && phones[i].hasScreen && !clientHasLayout)
      phones[i].needsLayout = true;
    else if (strcmp(phase, "placing") == 0 && rebind && phones[i].hasScreen)
      phones[i].needsLayout = true;
    else if (strcmp(phase, "gameover") == 0) {
      JsonDocument d;
      d["type"] = "gameover";
      d["score"] = gGameoverScore;
      d["timeMs"] = gGameoverTimeMs;
      sendJson(client, d);
    }
  } else {
    broadcastLobby();
    if (allReady()) pendingStart = true;
  }
}

// --- input queue: the network task hands gameplay inputs to the game task --------------
// Every sim* entry point below used to be called straight out of onMessage, i.e. from the
// AsyncTCP task, while gameTask walked the same trains, nodes and stations 30x a second.
// Two tasks mutating one structure with nothing in between. The case that actually bites
// is an add-train powerup growing the train array mid-simTick: the array can move, and the
// tick keeps reading where it used to be.
//
// Worse, simClock() is not a pure read -- it moves gPauseStart/gPausedNow/gPauseAccum
// (sim.cpp:55). Called off-task it can skew the game clock for every phone at once.
//
// So the network task records what arrived and when, and the game task does the work. A
// mutex is the obvious alternative and the wrong one here: it would park the priority-10
// AsyncTCP task behind a 33 ms tick, which is the stall LAG-REPORT spent its length
// chasing. One producer, one consumer, no allocation, nobody waits.
enum InputKind : uint8_t {
  IN_STATION_TOGGLE,
  IN_JUNCTION_TOGGLE,
  IN_SWITCH_TOGGLE,
  IN_BOOST_ARM,
  IN_BUY_POWERUP,
  IN_PU_TARGET,
  IN_PU_CANCEL,
};

struct InputEvent {
  uint8_t kind;
  int16_t a;      // station / junction / switch / boost / powerup id
  int16_t b;      // powerup target: a station for a freeze, a rail node otherwise
  uint32_t wall;  // millis() AT RECEIPT, stored raw -- simClock() is the game task's to call
};

// Far more than four phones can produce in one 33 ms tick. The depth is not for throughput,
// it is so a burst during a stall cannot overwrite events the game task has not read yet.
static const uint8_t INPUT_Q_LEN = 32;
static InputEvent gInputQ[INPUT_Q_LEN];
static volatile uint8_t gInputHead = 0;  // written by the AsyncTCP task, never by gameTask
static volatile uint8_t gInputTail = 0;  // written by gameTask, never by the AsyncTCP task
uint32_t gInputDropped = 0;              // surfaced in [status] as qdrop

static void queueInput(uint8_t kind, int a, int b, uint32_t wall) {
  const uint8_t head = gInputHead;
  const uint8_t next = (uint8_t)((head + 1) % INPUT_Q_LEN);
  // Full: drop the input. The one thing never worth doing on this task is waiting.
  if (next == gInputTail) { gInputDropped++; return; }
  gInputQ[head].kind = kind;
  gInputQ[head].a = (int16_t)a;
  gInputQ[head].b = (int16_t)b;
  gInputQ[head].wall = wall;
  // Publish the slot only once it is filled. Without the barrier the compiler may hoist
  // the index store above the payload stores and gameTask reads a half-written event.
  __sync_synchronize();
  gInputHead = next;
}

// gameTask only. Ordering is load-bearing: this must run AFTER simCheckArmLapse(now) and
// BEFORE the tick's own simClock(now). Every simClock() argument has to be non-decreasing,
// and these carry receipt timestamps a couple of ms older than `now`.
static void drainInputs() {
  while (gInputTail != gInputHead) {
    const InputEvent e = gInputQ[gInputTail];
    __sync_synchronize();
    gInputTail = (uint8_t)((gInputTail + 1) % INPUT_Q_LEN);
    // The phase can move on between receipt and here -- a gameover lands on the tick that
    // follows. An input for a game that is no longer running is dropped, not applied.
    if (strcmp(phase, "play") != 0) continue;
    switch (e.kind) {
      case IN_STATION_TOGGLE:  simStationToggle(e.a); break;
      case IN_JUNCTION_TOGGLE: simJunctionToggle(e.a); break;
      case IN_SWITCH_TOGGLE:   simSwitchToggle(e.a); break;
      // simClock() is called in exactly the places it was called before. It has side
      // effects, so the three plain toggles must not start calling it.
      case IN_BOOST_ARM:       simBoostArm(e.a, simClock(e.wall)); break;
      case IN_BUY_POWERUP:     simBuyPowerup(e.a, simClock(e.wall), e.wall); break;
      case IN_PU_TARGET:       simPowerupTarget(e.a, e.b, simClock(e.wall)); break;
      // Disarms, so it must land before the simClock() below it rather than after: that
      // call is what notices the pause is over and restarts sim time on this same pass.
      case IN_PU_CANCEL:       simCancelArmedPowerup(e.a); simClock(e.wall); break;
    }
  }
}

void onMessage(AsyncWebSocketClient *client, const char *data, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) return;
  const char *type = doc["type"] | "";

  if (strcmp(type, "hello") == 0) {
    handleHello(client, doc);
    return;
  }

  int i = phoneIndexByClient(client->id());
  if (i < 0) return;
  if (strcmp(type, "cstat") == 0) {
#if DEBUG_CLIENT_STATS
    // client-side render/loss diagnostics, surfaced in the [clients] line
    phones[i].dbgFrameMax = (uint16_t)(doc["fmax"] | 0);
    phones[i].dbgFps = (uint16_t)(doc["fps"] | 0);
    phones[i].dbgGaps = (uint16_t)(doc["gaps"] | 0);
#endif
    return;  // a phone built against DEBUG_CLIENT_STATS=1 must not be an error here
  }
  if (strcmp(type, "ready") == 0) {
    phones[i].ready = doc["value"] | false;
    broadcastLobby();
    if (allReady()) pendingStart = true;
  } else if (strcmp(type, "placed") == 0) {
    if (strcmp(phase, "placing") != 0) return;
    phones[i].placed = doc["value"] | false;
    if (allPlaced()) pendingGo = true;
  } else if (strcmp(type, "reset") == 0) {
    Log.printf("reset msg from %s (id=%u)\n", phones[i].id, client->id());
    pendingReset = true;
  } else if (strcmp(type, "retry") == 0) {
    if (strcmp(phase, "gameover") != 0) return;
    const char *mode = doc["mode"] | "";
    if (strcmp(mode, "keep") == 0) pendingRetryKeep = true;
    else pendingRetryNew = true;
  } else if (strcmp(type, "kick") == 0) {
    if (strcmp(phase, "lobby") != 0) return;
    const char *tid = doc["id"] | "";
    for (int k = 0; k < MAX_DEVICES; k++) {
      if (phones[k].used && k != i && strcmp(phones[k].id, tid) == 0) {
        Log.printf("kick %s (by %s)\n", phones[k].id, phones[i].id);
        AsyncWebSocketClient *cl = ws.client(phones[k].clientId);
        if (cl) cl->close();
        phones[k].used = false;
        phones[k].connected = false;
        break;
      }
    }
    broadcastLobby();
    if (allReady()) pendingStart = true;
  } else if (strcmp(phase, "play") == 0) {
    // Nothing in here touches the simulation. queueInput() records the input and the
    // timestamp; drainInputs() applies it at the top of the next tick.
    const uint32_t w = millis();
    if (strcmp(type, "stationToggle") == 0) queueInput(IN_STATION_TOGGLE, doc["station"] | -1, -1, w);
    else if (strcmp(type, "junctionToggle") == 0) queueInput(IN_JUNCTION_TOGGLE, doc["junction"] | -1, -1, w);
    else if (strcmp(type, "switchToggle") == 0) queueInput(IN_SWITCH_TOGGLE, doc["switch"] | -1, -1, w);
    else if (strcmp(type, "boostArm") == 0) queueInput(IN_BOOST_ARM, doc["boost"] | -1, -1, w);
    else if (strcmp(type, "buyPowerup") == 0) queueInput(IN_BUY_POWERUP, doc["id"] | -1, -1, w);
    else if (strcmp(type, "puTarget") == 0) {
      // One message for both targeted powerups; the payload key differs because a freeze
      // names a station and an extra train names a rail node. Picking the key reads only
      // the JSON, so it stays here.
      int id = doc["id"] | -1;
      int target = id == PU_FREEZE ? (doc["station"] | -1) : (doc["node"] | -1);
      queueInput(IN_PU_TARGET, id, target, w);
    }
    // Any phone may back out of an armed powerup, same as any phone may answer it.
    else if (strcmp(type, "puCancel") == 0) queueInput(IN_PU_CANCEL, doc["id"] | -1, -1, w);
  }
}

void onWsEvent(AsyncWebSocket *s, AsyncWebSocketClient *client, AwsEventType type,
               void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    onConnect(client);
  } else if (type == WS_EVT_DISCONNECT) {
    onDisconnect(client->id(), client->queueLen(), ESP.getFreeHeap());
  } else if (type == WS_EVT_ERROR) {
    uint16_t code = arg ? *((uint16_t *)arg) : 0;
    Log.printf("ws error id=%u code=%u reason=%.*s heap=%u\n",
               client->id(), code, (int)len, data ? (const char *)data : "", ESP.getFreeHeap());
  } else if (type == WS_EVT_DATA) {
    AwsFrameInfo *info = (AwsFrameInfo *)arg;
    if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
      onMessage(client, (const char *)data, len);
    }
  }
}

// Channel 11 was hardcoded. If the venue is busy on 11, every frame waits for a clear
// channel, which is what shared multi-phone stalls look like. Survey 1 / 6 / 11 once at
// boot and take the quietest. Only the three non-overlapping channels are considered:
// anything between them overlaps two of these and is worse than either.
//
// Scoring: each visible network costs more the stronger it is and the closer it sits,
// since an adjacent-channel network still leaks energy into ours. Returns
// AP_CHANNEL_FALLBACK if the scan fails or finds nothing, so behaviour is never worse
// than before. Set AP_CHANNEL_SCAN 0 to go back to a fixed channel.
#ifndef AP_CHANNEL_SCAN
#define AP_CHANNEL_SCAN 1
#endif
static const int AP_CHANNEL_FALLBACK = 11;

static int pickQuietestChannel() {
#if !AP_CHANNEL_SCAN
  return AP_CHANNEL_FALLBACK;
#else
  const int cands[3] = {1, 6, 11};
  long score[3] = {0, 0, 0};

  // Scanning needs station mode; the AP is started by the caller afterwards.
  WiFi.mode(WIFI_STA);
  int n = WiFi.scanNetworks(false /*async*/, false /*hidden*/);
  if (n <= 0) {
    Log.printf("[wifi] scan found nothing (n=%d) — using channel %d\n", n, AP_CHANNEL_FALLBACK);
    WiFi.scanDelete();
    return AP_CHANNEL_FALLBACK;
  }

  for (int i = 0; i < n; i++) {
    int ch = WiFi.channel(i);
    int rssi = WiFi.RSSI(i);
    // -30 dBm (very close) -> ~100, -90 dBm (far) -> ~1. Never below 1.
    long weight = rssi + 100;
    if (weight < 1) weight = 1;
    for (int c = 0; c < 3; c++) {
      int sep = abs(ch - cands[c]);
      if (sep == 0) score[c] += weight * 4;        // same channel: full collision
      else if (sep <= 2) score[c] += weight * 2;   // heavy overlap
      else if (sep <= 4) score[c] += weight;       // partial overlap
    }
  }
  WiFi.scanDelete();

  int best = 0;
  for (int c = 1; c < 3; c++) if (score[c] < score[best]) best = c;
  Log.printf("[wifi] scan: %d nets, congestion ch1=%ld ch6=%ld ch11=%ld -> channel %d\n",
             n, score[0], score[1], score[2], cands[best]);
  return cands[best];
#endif
}

void setup() {
  // HardwareSerial defaults to _txBufferSize(0), which makes uart_write_bytes block
  // loop() until the bytes reach the 128-byte hardware FIFO (~6 ms for a status line).
  // Must be set before begin(); it refuses to resize once Serial is running.
  Serial.setTxBufferSize(1024);
  Serial.begin(115200);
  delay(200);

  if (!LittleFS.begin(true)) {
    Log.println("LittleFS mount failed");
  }

  Log.printf("[mem] sizeof Rail=%u Node[%d]=%u Train[%d]=%u total=%u\n",
                (unsigned)sizeof(Rail), MAX_NODES, (unsigned)(sizeof(Node) * MAX_NODES),
                MAX_TRAINS, (unsigned)(sizeof(Train) * MAX_TRAINS),
                (unsigned)(sizeof(Rail) + sizeof(Node) * MAX_NODES + sizeof(Train) * MAX_TRAINS));
  Log.printf("[mem] pre-alloc heap=%u largest=%u\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  bool ok = gameAlloc();
  Log.printf("[mem] gameAlloc %s, heap=%u largest=%u\n",
                ok ? "OK" : "FAILED", ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  int apChannel = pickQuietestChannel();
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, nullptr, apChannel, 0, AP_MAX_CONN);
  WiFi.onEvent(onWifiEvent);
  esp_netif_t *apif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (apif) {
    esp_netif_dhcps_stop(apif);
    uint32_t leaseMin = 30;
    esp_netif_dhcps_option(apif, ESP_NETIF_OP_SET, ESP_NETIF_IP_ADDRESS_LEASE_TIME,
                           &leaseMin, sizeof(leaseMin));
    esp_netif_dhcps_start(apif);
  }
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);
  Log.print("AP \"");
  Log.print(AP_SSID);
  Log.print("\" at http://");
  Log.println(WiFi.softAPIP());

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);
  server.on("/state", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "text/plain", phase);
  });
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    Log.printf("http GET / from %s\n", req->client()->remoteIP().toString().c_str());
    if (strcmp(phase, "lobby") != 0) {
      String tok = cookieToken(req);
      bool isPlayer = tok.length() && phoneIndexByToken(tok.c_str()) >= 0;
      if (!isPlayer) { req->send_P(200, "text/html", HOLDING_HTML); return; }
    }
    bool gz = LittleFS.exists("/index.html.gz");
    AsyncWebServerResponse *resp =
        req->beginResponse(LittleFS, gz ? "/index.html.gz" : "/index.html", "text/html");
    if (gz) resp->addHeader("Content-Encoding", "gzip");
    resp->addHeader("Cache-Control", "no-cache");
    req->send(resp);
  });
  server.serveStatic("/", LittleFS, "/")
      .setDefaultFile("index.html")
      .setCacheControl("no-cache")
      .setFilter([](AsyncWebServerRequest *req) -> bool {
        if (strcmp(phase, "lobby") == 0) return true;
        const String &u = req->url();
        if (u == "/title.svg" || u == "/HostGrotesk-SemiBold.woff2") return true;
        String tok = cookieToken(req);
        return tok.length() && phoneIndexByToken(tok.c_str()) >= 0;
      });
  server.onNotFound([](AsyncWebServerRequest *req) { req->send(404, "text/plain", "not found"); });
  server.begin();

  // Core 1 is where Arduino's loopTask already ran, so the sim keeps the core it had; what
  // changes is that AsyncTCP is pinned to core 0 by flash.sh's
  // -DCONFIG_ASYNC_TCP_RUNNING_CORE=0 instead of floating onto this one mid-tick.
  // Priority 2 puts it above loopTask's 1. It cannot outrank AsyncTCP's 10 and is not
  // meant to -- the queue, not the priority, is what makes the two safe to interleave.
  // 8192 matches loopTask's own stack: this body used to run on exactly that.
  xTaskCreatePinnedToCore(gameTask, "game", 8192, nullptr, 2, nullptr, 1);
}

void reapDisconnected(uint32_t now) {
  bool changed = false;
  for (int i = 0; i < MAX_DEVICES; i++) {
    // disconnectedAt != 0 is the "has actually dropped" test. Without it a zero reads as a
    // drop that happened at boot -- i.e. always past the grace period -- which is what let
    // a slot mid-registration be reaped.
    if (phones[i].used && !phones[i].connected && phones[i].disconnectedAt != 0 &&
        now - phones[i].disconnectedAt > DISCONNECT_GRACE_MS) {
      Log.printf("reap %s\n", phones[i].id);
      phones[i].used = false;
      changed = true;
    }
  }
  if (!changed) return;
  if (strcmp(phase, "play") == 0 || strcmp(phase, "placing") == 0) resetToLobby();
  else broadcastLobby();
}

// Which Wi-Fi generation each associated phone supports, and how strong its signal is.
// Tests the premise behind disabling 802.11b rates (LAG-REPORT §3.4): a phone limping at
// 1 Mbps holds the shared channel ~50x longer per frame than one at full speed, which
// stalls everyone.
//
// Read it honestly: the phy_* flags report what the station has ENABLED, not the rate it
// is transmitting at right now — the driver does not expose the live rate. So `11n` means
// "capable of modern rates", not "currently fast". `rssi` is the better clue: a phone
// much below about -70 dBm is the one likely falling back to slow rates.
//
// Verdict to look for: every phone 11n with healthy rssi -> 802.11b is NOT the problem
// and §3.4 can be dropped. Any phone showing 11b/11g only, or a weak rssi -> that is the
// airtime hog, and moving it closer may fix it without touching radio settings.
#if DEBUG_CLIENT_STATS
static void logStationPhy() {
  wifi_sta_list_t list;
  if (esp_wifi_ap_get_sta_list(&list) != ESP_OK || list.num <= 0) return;
  char buf[288];
  int off = 0;
  for (int i = 0; i < list.num && off < (int)sizeof(buf) - 40; i++) {
    const wifi_sta_info_t &s = list.sta[i];
    const char *mode = s.phy_11n ? "11n" : s.phy_11g ? "11g" : s.phy_11b ? "11b"
                     : s.phy_lr  ? "LR"  : "?";
    off += snprintf(buf + off, sizeof(buf) - off, "%02x:%02x:%02x/%s/%ddBm ",
                    s.mac[3], s.mac[4], s.mac[5], mode, (int)s.rssi);
  }
  if (off) Log.printf("[phy] %s\n", buf);
}
#endif

static const char *resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_SW:       return "SW";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_DEEPSLEEP:return "DEEPSLEEP";
    case ESP_RST_EXT:      return "EXT";
    default:               return "OTHER";
  }
}

uint32_t lastStatus = 0;
uint32_t lastCleanup = 0;

uint32_t gPrevLoopMs = 0;
uint32_t gMaxLoopGap = 0;
uint32_t gMaxTickWork = 0;
uint32_t gTickOverruns = 0;
static const uint32_t TICK_LATE_MS = 50;

// The former loop() body, unchanged except where marked. gameTask is its only caller, and
// that is the whole point: this function is the single owner of the phones array, the
// phase, the blob markers and the simulation. Nothing may touch those from another task --
// so resist putting any of this back on loopTask, and route new inbound work through the
// input queue instead.
static void gameStep() {
  uint32_t now = millis();
  if (gPrevLoopMs) { uint32_t gap = now - gPrevLoopMs; if (gap > gMaxLoopGap) gMaxLoopGap = gap; }
  gPrevLoopMs = now;
  if (now - lastCleanup >= 1000) { lastCleanup = now; ws.cleanupClients(); }
  reapDisconnected(now);

  if (pendingReset) {
    pendingReset = false;
    resetToLobby();
    now = millis();
  }
  if (pendingStart) {
    pendingStart = false;
    beginPlacement();
    now = millis();
  }
  if (pendingGo) {
    pendingGo = false;
    startGame();
    now = millis();
  }
  if (pendingRetryNew) {
    pendingRetryNew = false;
    if (strcmp(phase, "gameover") == 0) buildAndPlace();
    now = millis();
  }
  if (pendingRetryKeep) {
    pendingRetryKeep = false;
    if (strcmp(phase, "gameover") == 0) restartKeepLayout();
    now = millis();
  }

  // Order matters: reapDisconnected may free a slot, reapBlobs then clears its marker,
  // and only then may drainLayouts start anything new or a send consult the markers.
  reapBlobs(now);
  drainLayouts(now);

  if (now - lastStatus >= 3000) {
    lastStatus = now;
    bool heapOk = DEBUG_HEAP_CHECK ? heap_caps_check_integrity_all(true) : true;
    Log.printf("[status] t=%lus heap=%u minheap=%u maxblk=%u wsClients=%u phones=%d apSta=%d phase=%s heapOk=%d drops=%lu qdrop=%lu bails=%lu/%lu skip=%lu/%lu/%lu blobs=%d maxloop=%lu maxwork=%lu overruns=%lu lastReset=%s\n",
                  now / 1000, ESP.getFreeHeap(), ESP.getMinFreeHeap(), ESP.getMaxAllocHeap(),
                  ws.count(), connectedCount(), WiFi.softAPgetStationNum(), phase, heapOk,
                  (unsigned long)gDroppedSends, (unsigned long)gInputDropped,
                  (unsigned long)gHeapBails,
                  (unsigned long)gBlobBails,
                  (unsigned long)gSkipTrains, (unsigned long)gSkipWorld,
                  (unsigned long)gSkipLobby, gBlobsInFlight,
                  (unsigned long)gMaxLoopGap, (unsigned long)gMaxTickWork, (unsigned long)gTickOverruns,
                  resetReasonStr());
    gMaxLoopGap = 0; gMaxTickWork = 0; gTickOverruns = 0;

#if DEBUG_CLIENT_STATS
    if (strcmp(phase, "play") == 0 || strcmp(phase, "placing") == 0) {
      char cbuf[320]; int off = 0;
      for (int i = 0; i < MAX_DEVICES && off < (int)sizeof(cbuf) - 56; i++) {
        if (!phones[i].used || !phones[i].connected || !phones[i].hasScreen) continue;
        AsyncWebSocketClient *cl = ws.client(phones[i].clientId);
        unsigned q = cl ? (unsigned)cl->queueLen() : 0;
        unsigned sp = (cl && cl->client()) ? (unsigned)cl->client()->space() : 0;
        // Last octet of the phone's IP, so a phone can be tied to a MAC in [phy] via the
        // "wifi: STA IP 4.3.2.x -> <mac>" line already logged when it joined.
        unsigned ipLast = cl ? (unsigned)cl->remoteIP()[3] : 0;
        off += snprintf(cbuf + off, sizeof(cbuf) - off, "%s@%u:q%u/sp%u/f%u@%ufps/g%u/s%u%s ",
                        phones[i].id, ipLast, q, sp,
                        phones[i].dbgFrameMax, phones[i].dbgFps, phones[i].dbgGaps,
                        phones[i].dbgSkips, phones[i].blobSentAt ? "/BLOB" : "");
      }
      if (off) Log.printf("[clients] %s\n", cbuf);
      logStationPhy();
    }
#endif
  }

  // Before any simClock() this iteration: an armed powerup stops sim time, so clearing a
  // lapsed arm here is what lets the clock resume on this same pass rather than the next.
  if (strcmp(phase, "play") == 0 && simCheckArmLapse(now))
    Log.printf("powerup arm lapsed -- refunded, board resumed\n");

  // After the arm lapse, which can restart sim time, and before the tick's own
  // simClock(now): the receipt timestamps these events carry are older than `now`.
  drainInputs();

  if (gStartGate) {
    bool waiting = layoutsPending() || gBlobsInFlight;
    if (waiting && (now - gStartGateAt) < START_GATE_TIMEOUT) { lastTick = now; gNextTickAt = now + TICK_PERIOD_MS; }
    else { gStartGate = false; Log.printf("ticking (%s)\n", waiting ? "gate timeout" : "layouts delivered"); }
  }

  if (strcmp(phase, "play") == 0 && !gStartGate && (int32_t)(now - gNextTickAt) >= 0) {
    uint32_t tickDt = now - lastTick;
    if (tickDt > TICK_LATE_MS) gTickOverruns++;
    lastTick = now;
    // Advance from the slot we just served, not from `now`, so a tick that ran long does
    // not drag its successors with it. If we are already a full period past the next slot
    // -- a real stall, not a late tick -- abandon the missed slots and re-base, or this
    // would spin trying to catch up on a game that has moved on.
    gNextTickAt += TICK_PERIOD_MS;
    if ((int32_t)(now - gNextTickAt) >= 0) gNextTickAt = now + TICK_PERIOD_MS;
    uint32_t workStart = now;
    bool over = simTick(simClock(now));

    int step = (int)(gChaos * 20);
    if (step > lastChaosStep) { lastChaosStep = step; Log.printf("chaos: %.2f\n", gChaos); }

    if (over) {
      gGameoverScore = gTotalScore;
      gGameoverTimeMs = (uint32_t)(simClock(millis()) - simGameStartMs());
      JsonDocument d;
      d["type"] = "gameover";
      d["score"] = gGameoverScore;
      d["timeMs"] = gGameoverTimeMs;
      broadcastJson(d);
      phase = "gameover";
      Log.printf("gameover (score=%d)\n", gTotalScore);
    } else {
      tickCount++;
      // No global gate: sendTextToReady now skips only the phones still draining a blob.
      uint32_t wall = millis();
      if (tickCount % TRAINS_SEND_EVERY == 0 && buildTrainsMsg(wall, tickCount))
        sendTextToReady(gProtoBuf, SEND_TRAINS);
      if (tickCount % WORLD_SEND_EVERY == 2 && buildWorldMsg(wall))
        sendTextToReady(gProtoBuf, SEND_WORLD);
    }
    uint32_t work = millis() - workStart;
    if (work > gMaxTickWork) gMaxTickWork = work;
  }
}

// Wakes on a fixed cadence measured from the previous WAKE, not from whenever the body
// finished, so gameStep()'s own duration stops pushing the next wake later. That is the
// difference from the old `delay(2)`, which was relative and let lateness accumulate.
static void gameTask(void *) {
  TickType_t wake = xTaskGetTickCount();
  const TickType_t period = pdMS_TO_TICKS(GAME_TASK_PERIOD_MS);
  for (;;) {
    vTaskDelayUntil(&wake, period);
    gameStep();
  }
}

// Deliberately idle. Giving loopTask work would put a second task back on the state
// gameStep() owns -- including ws.cleanupClients(), which walks the same AsyncWebSocket
// client list the sends consult, and which is why it moved into gameStep() with the rest.
void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
