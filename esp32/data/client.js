const $ = (id) => document.getElementById(id);

const lobbyEl = $("lobby");
const playEl = $("play");
const statusEl = $("status");
const phoneListEl = $("phoneList");
const readyToggle = $("readyToggle");
const readyKnob = readyToggle?.querySelector(".rt-knob");
const trainEl = document.querySelector(".train");
const reconnectModal = $("reconnectModal");
const clockEl = $("clock");
const canvas = $("board");
// alpha:false lets the compositor skip blending the board layer against the page behind
// it. Nothing is ever meant to show through: draw() lays down an opaque full-bleed fill as
// its first act, and the HUD is fixed-position DOM ON TOP of the canvas, not behind it.
// Revert this the day the board is supposed to be see-through.
const ctx = canvas.getContext("2d", { alpha: false });
const scoreEl = $("scoreVal");
const scorePopsEl = $("scorePops");
const scoreLabelEl = document.querySelector(".score-label");
const upgradesEl = $("upgrades");
const puPromptEl = $("puPrompt");
const puPromptTextEl = $("puPromptText");
const puCancelBtn = $("puCancelBtn");
const gameoverEl = $("gameover");
const finalScoreEl = $("finalScore");
const finalTimeEl = $("finalTime");
const leagueRomanEl = $("leagueRoman");
const leagueTypeEl = $("leagueType");
const leagueProgEl = $("leagueProg");
const gameoverBtn = $("gameoverBtn");
const gameoverMainEl = $("gameoverMain");
const retryBtn = $("retryBtn");
const retryChoiceEl = $("retryChoice");
const retryKeepBtn = $("retryKeepBtn");
const retryNewBtn = $("retryNewBtn");
const retryCancelBtn = $("retryCancelBtn");
const menuBtn = $("menuBtn");
const menuOverlayEl = $("menuOverlay");
const closeMenuBtn = $("closeMenuBtn");
const flipHudBtn = $("flipHudBtn");
const lobbyBtn = $("lobbyBtn");
const helpBtn = $("helpBtn");
const placeConfirmEl = $("placeConfirm");
const placeBtn = $("placeBtn");
const placeLobbyBtn = $("placeLobbyBtn");
const placeStatusEl = $("placeStatus");
const hudEl = $("hud");
const tutorialEl = $("tutorial");
const tutBackBtn = $("tutBackBtn");
const tutCloseBtn = $("tutCloseBtn");

// Index MUST match the PowerupId enum in game.h; the server addresses these by number.
const PU_BOARD = 0,
  PU_CARS = 1,
  PU_FREEZE = 2,
  PU_TRAIN = 3,
  PU_REVERSE = 4;

// Mirrors CFG_PU_COST / CFG_PU_MAX_LEVEL in game.h. These are compile-time constants, so
// the server no longer spends bytes resending them ~5x/second inside every `world` message
// (see buildWorld()). puCostOf() reproduces the firmware's puCost(): -1 once maxed.
const PU_COSTS = [
  [150, 150, 150],
  [250, 250, 250],
  [200, 200, 200],
  [600, 0, 0],
  [400, 0, 0],
];
const PU_MAX_LEVEL = [3, 3, 3, 1, 1];
function puCostOf(id, level) {
  if (id < 0 || id >= PU_COSTS.length) return -1;
  if (level >= PU_MAX_LEVEL[id]) return -1;
  return PU_COSTS[id][level];
}

const POWERUP_DEFS = [
  { id: PU_BOARD, icon: "powerup-speed-dropoff.svg", max: 3, label: "boarding speed" },
  { id: PU_CARS, icon: "powerup-add-car.svg", max: 3, label: "more cars" },
  { id: PU_FREEZE, icon: "powerup-freeze.svg", max: 3, target: "station", label: "freeze" },
  { id: PU_TRAIN, icon: "powerup-add-train.svg", max: 1, target: "node", label: "extra train" },
  { id: PU_REVERSE, icon: "powerup-switch-direction.svg", max: 1, label: "reverse direction" },
];
const powerupBtns = {};

// Latest server view: one [lv, own, arm, cost, act] per powerup, from the world
// message. Score arrives on the trains message instead, so both are cached and either
// arrival re-renders.
let puState = null;
let puScore = 0;
let mySlot = -1;
let frozenStations = new Set();
let dirRev = 0;

function buildPowerups() {
  upgradesEl.innerHTML = "";
  for (const k of Object.keys(powerupBtns)) delete powerupBtns[k];
  for (const def of POWERUP_DEFS) {
    const btn = document.createElement("button");
    btn.className = "upg";
    btn.hidden = true; // shown once the deal says this phone owns it
    const icon = document.createElement("img");
    icon.className = "upg-icon";
    icon.src = def.icon;
    icon.alt = "";
    btn.appendChild(icon);
    const ring = buildRing(def.max);
    btn.appendChild(ring.svg);
    // Every tap is a buy. Levels 2 and 3 are just further taps on the same button.
    btn.addEventListener("click", () => send({ type: "buyPowerup", id: def.id }));
    upgradesEl.appendChild(btn);
    powerupBtns[def.id] = { btn, icon, slots: ring.slots };
  }
  renderPowerups();
}

// Backing out of an arm. The sim refunds the level the buy charged for, exactly as the
// 8 s deadline would have, and sim time resumes -- so this is the deadline brought forward
// rather than a new rule. Any phone's cancel counts, like any phone's target tap.
puCancelBtn?.addEventListener("click", (ev) => {
  ev.stopPropagation();
  const arming = armedPowerup();
  if (arming) send({ type: "puCancel", id: arming.id });
});

// Which powerup, if any, is waiting for a target tap. Server state, so every phone can
// see it and any phone can supply the target.
function armedPowerup() {
  if (!puState) return null;
  for (const def of POWERUP_DEFS) {
    if (!def.target) continue;
    const e = puState[def.id];
    if (e && e[2]) return def;
  }
  return null;
}

// The ring is cut into one BAND PER TIER, and the band a thing is drawn in says which tier
// it is about -- so colour is only ever reinforcing position, never carrying a count on its
// own. That is the whole reason the level dots are gone: the ring already says how far up
// the powerup you are, in two layers that never share a band.
//
//   below `lv`  FUEL: orange, over the tier's own colour dimmed. Each tier bought appended
//               one band of it (CFG_PU_TIER_MS), and it burns from the top down -- so the
//               bands still holding orange ARE the level, and buying a tier mid-run simply
//               adds a band without changing how fast the orange recedes.
//   from `lv`   BANKED: bright, in the tier's colour, filling with score toward its price.
//
// Fuel can never reach past band lv-1 and the bank never starts below band lv, so the two
// cannot collide. When the last of the fuel goes and lv drops to 0 the banked fill re-bases
// down the ring on its own -- nothing is converted, it is just re-rendered from `lv`.
const RING_COLORS = ["#ffd24a", "#8ede55", "#4aa8ff"];
const RING_SPENT = ["rgba(255,210,74,0.3)", "rgba(142,222,85,0.3)", "rgba(74,168,255,0.3)"];
const RING_TRACK = "rgba(255,255,255,0.14)";

const RING_ACTIVE = "#ff9f4a";

// Ring geometry, in user units of the 0..100 viewBox the button scales the <svg> to. Every
// length here is a distance ALONG the circumference, because a band and its fill are both
// drawn by walking one <circle> with stroke-dasharray rather than by any arc path.
const SVG_NS = "http://www.w3.org/2000/svg";
const RING_R = 45;
const RING_C = 2 * Math.PI * RING_R;
const RING_GAP = 5; // blank track between bands, ~6 degrees: without it a part-filled ring
                    // is ambiguous between "one band and a bit" and "a fraction of one".

// A dash of `len` starting `start` along the circle, and nothing else. stroke-linecap must
// stay butt (set in CSS) or a zero-length dash still paints a dot.
function ringArc(start, len) {
  const c = document.createElementNS(SVG_NS, "circle");
  c.setAttribute("cx", "50");
  c.setAttribute("cy", "50");
  c.setAttribute("r", String(RING_R));
  c.setAttribute("stroke-dashoffset", String(-start));
  c.setAttribute("stroke-dasharray", `${len} ${RING_C - len}`);
  return c;
}

// n bands for an n-tier powerup; a single-tier one is just an ungapped full circle.
function buildRing(n) {
  const svg = document.createElementNS(SVG_NS, "svg");
  svg.setAttribute("class", "upg-ring");
  svg.setAttribute("viewBox", "0 0 100 100");
  svg.setAttribute("aria-hidden", "true");
  const g = document.createElementNS(SVG_NS, "g");
  g.setAttribute("transform", "rotate(-90 50 50)"); // band 0 starts at twelve o'clock
  const gap = n > 1 ? RING_GAP : 0;
  const span = RING_C / n - gap;
  const slots = [];
  for (let i = 0; i < n; i++) {
    const start = (i * RING_C) / n + gap / 2;
    const track = ringArc(start, span);
    const fill = ringArc(start, 0);
    fill._span = span;
    g.appendChild(track);
    g.appendChild(fill);
    slots.push({ track, fill });
  }
  svg.appendChild(g);
  return { svg, slots };
}

// renderPowerups() runs on every trains message, not just the 5 Hz world one, so both
// setters drop writes that would not change anything: an attribute set to the value it
// already holds still costs a style invalidation.
function setStroke(el, color) {
  if (el._col === color) return;
  el._col = color;
  el.setAttribute("stroke", color);
}

// `f` is 0..1 of this band, not of the ring. A null colour means draw nothing.
function setFill(el, color, f) {
  if (color) setStroke(el, color);
  const len = color ? Math.max(0, Math.min(1, f)) * el._span : 0;
  const q = len.toFixed(1);
  if (el._len === q) return;
  el._len = q;
  el.setAttribute("stroke-dasharray", `${q} ${RING_C - q}`);
}

// The ring means different things in each state, which is what keeps the button
// numberless: how many tiers you can afford, then how much of the window is left.
function renderPowerups() {
  // Every phone is told what the board is waiting for, not just the owner -- the target
  // may only be reachable from someone else's screen.
  if (puPromptEl) {
    const arming = armedPowerup();
    puPromptEl.hidden = !arming;
    if (arming && puPromptTextEl) {
      puPromptTextEl.textContent =
        arming.target === "station" ? "paused — tap a station to freeze" : "paused — tap the track to add a train";
    }
  }
  for (const def of POWERUP_DEFS) {
    const u = powerupBtns[def.id];
    if (!u) continue;
    const e = puState ? puState[def.id] : null;
    if (!e) {
      u.btn.hidden = true;
      continue;
    }
    const [lv, own, arm, cost, act] = e;
    u.btn.hidden = !(mySlot >= 0 && own === mySlot);
    if (u.btn.hidden) continue;

    u.btn.classList.toggle("armed", !!arm);
    u.btn.classList.toggle("active", lv > 0);
    u.btn.classList.toggle("maxed", lv >= def.max);
    u.btn.disabled = cost < 0 || puScore < cost;

    // `act` is fuel in BANDS, not a fraction of one window (see protocol.cpp), so it lays
    // straight across the ring. Score then spills across the bands from `lv` up: a full
    // band means that tier is paid for, and a ring filled to the top means "I can max this
    // out from here". The fuel stays orange while armed -- arming stops sim time, so it is
    // held rather than draining, and .upg.armed's blue glow already says so loudly.
    const affordable = cost > 0 ? puScore / cost : 0;
    for (let i = 0; i < u.slots.length; i++) {
      const slot = u.slots[i];
      const tier = Math.min(i, RING_COLORS.length - 1);
      setStroke(slot.track, i < lv ? RING_SPENT[tier] : RING_TRACK);
      if (i < lv) setFill(slot.fill, RING_ACTIVE, act - i);
      else if (cost > 0) setFill(slot.fill, RING_COLORS[tier], affordable - (i - lv));
      else setFill(slot.fill, null, 0);
    }
  }
}

const STATION_OFFSET = 16;
const STATION_DEPTH = 20;
const QUEUE_R = 5;
const QUEUE_GAP = 3;
const QUEUE_START = 10;
const STATION_PLAT = STATION_OFFSET + 2 * QUEUE_R + 8;
const GATE_HALF = 13;
const GATE_HIT = 42;
const JUNCTION_HIT = 50;
// A boost pad is driven by a swipe along the rail, not a tap: its region sits on top of a
// station gate often enough that sharing the tap meant one quietly stealing the other.
// The geometry below is a run of arrowheads laid along the track -- a lane for the thumb to
// follow -- rather than the pair of chevrons that used to mark the node.
const BOOST_TRI_N = 5;
const BOOST_TRI_STEP = 11; // base to base, along travel
const BOOST_TRI_H = 9; // base to tip
const BOOST_TRI_HALF = 8; // half width at the base
// Bases run -2..+2 steps about the node and the tips add BOOST_TRI_H on top of that, so
// the whole run is pulled back half a triangle to sit optically centred on the pad.
const BOOST_TRI_SHIFT = -BOOST_TRI_H / 2;
// The lane that claims a press, as reach along the rail and across it. Along: the run plus
// a little margin. Across: wide enough to catch a thumb that lands off the track.
const BOOST_LANE_L = ((BOOST_TRI_N - 1) * BOOST_TRI_STEP) / 2 + BOOST_TRI_H + 12;
const BOOST_LANE_W = 26;
// Board px of travel along the rail that counts as a whole swipe, and the first few that
// count for nothing -- so a tap that wobbles under the finger lights no triangles.
const BOOST_SWIPE_DIST = 40;
const BOOST_SWIPE_SLOP = 6;
// How much of a swipe commits. 0.8 is where the last triangle lights (see nLit in
// drawBoosts), which is the whole point: if you lit them all, it took.
const BOOST_SWIPE_DONE = 0.8;
// Under this much total movement the press was a tap meant for whatever else shares the
// lane, and is handed on. Above it but short of BOOST_SWIPE_DONE, the gesture is abandoned.
const BOOST_TAP_SLOP = 10;
// Idle pads are drawn down so that an armed run reading at full strength is unmistakable;
// a pad still recharging is dimmer again.
const BOOST_ALPHA_IDLE = 0.5;
const BOOST_ALPHA_CD = 0.4;
const BOOST_ACTIVE = "#6fd2ff";
// The swipe in flight, if any: { id, prog }, prog signed -1..1 along travel. Only ever one
// -- the board takes one finger at a time -- and drawBoosts reads it to decide how much of
// a pad's run is lit.
let boostSwipe = null;
// What a committed swipe asked for, per pad id, until the server's echo confirms it or the
// window lapses: { armed, until }. Arming is a round trip, and without this the run a swipe
// just lit would drop back to idle for 100-odd ms before lighting again.
const boostWish = new Map();
const BOOST_WISH_MS = 1200;
// Live rail and rail a lever currently has closed. Shared by drawRail() and the tunnel
// stubs so a branch cannot be grey on the main line and white on its way into the mouth.
const RAIL_LIVE = "#ffffff";
const RAIL_CUT = "#3b4149";
const LEVER_BACK = 26;
const LEVER_OFF = 13;
const LEVER_ARM = 15;
const LEVER_COLOR = "#ffffff";
const MOUTH_DEPTH = 18;
const MOUTH_W = 15;
const MOUTH_ARC = 1.95;
const TUNNEL_RIM = "#9aa3b2";

let stationColors = 5;
let maxQueue = 6;
const HUE_OVERRIDE = { 1: 48 };
function hueOf(idx) {
  return HUE_OVERRIDE[idx] ?? Math.round((idx * 360) / stationColors);
}

function formatDuration(ms) {
  const total = Math.max(0, Math.round((ms || 0) / 1000));
  const m = Math.floor(total / 60);
  const s = total % 60;
  return m + ":" + String(s).padStart(2, "0");
}
function colorOf(idx) {
  if (idx == null || idx < 0) return null;
  return `hsl(${hueOf(idx)}, 70%, 55%)`;
}

let ws;
let reconnectAttempts = 0;
const STABLE_MS = 5000;
let me = null;
let ready = false;
let placing = false;
let placed = false;
const _pParam = new URLSearchParams(location.search).get("p") || new URLSearchParams(location.search).get("player");
let token;
if (_pParam) {
  token = `p${_pParam}`;
} else {
  token = localStorage.getItem("tg_token");
  if (!token) {
    token = Math.random().toString(36).slice(2) + Date.now().toString(36);
    localStorage.setItem("tg_token", token);
  }
}
document.cookie = `tg_token=${token}; path=/; max-age=86400; SameSite=Lax`;
let lastLayout = null;
let trainStates = [];
let serverFull = false;
let gameInProgress = false;
let bonusColor = null;
let bonusPhase = 0;
let bonusT = 0;
let bonusLeftMs = Infinity;
let bonusLeftStamp = 0;
let bonusDashPhase = 0;
let bonusDashLast = 0;
const BONUS_SLOWDOWN_MS = 8000;

const DELIVERY_POINTS = 10;
const COMBO_LIFE_MS = 2000;
const stationCombos = new Map();
let nodeToStation = null;

const DRIFT_PX = 18,
  DRIFT_ALONG = 0.25,
  DRIFT_CHAOS_CAP = 1;
let driftReady = false;
let driftStartMs = 0;
let currentChaos = 0;

let wakeLock = null;
async function requestWakeLock() {
  try {
    if ("wakeLock" in navigator && document.visibilityState === "visible") {
      wakeLock = await navigator.wakeLock.request("screen");
    }
  } catch {
  }
}
function releaseWakeLock() {
  try {
    wakeLock?.release();
  } catch {}
  wakeLock = null;
}

function viewport() {
  return {
    w: Math.round(window.innerWidth),
    h: Math.round(window.innerHeight),
    orientation: window.innerWidth >= window.innerHeight ? "landscape" : "portrait",
  };
}

// Decoders for the compact positional broadcasts. Field ORDER is the wire contract --
// change these and buildTrains()/buildWorld() in esp32.ino together, never one alone.
//
//   T|seq|t|score|total|rev|li,to,pr,sp,gap,col,flags,bo,pax... ; ...
//
// Returns the same shape the JSON message used to have, so handle()/syncTrains() are
// unchanged: col -1 becomes null, pax -1 becomes null, bo is rescaled to 0..1.
function parseTrains(d) {
  const p = d.split("|");
  const trains = [];
  const body = p[6];
  if (body) {
    for (const rec of body.split(";")) {
      const f = rec.split(",");
      const col = +f[5];
      const fl = +f[6];
      const pax = [];
      for (let k = 8; k < f.length; k++) {
        const v = +f[k];
        pax.push(v < 0 ? null : v);
      }
      trains.push({
        li: +f[0], to: +f[1], pr: +f[2], sp: +f[3], gap: +f[4],
        col: col < 0 ? null : col,
        fu: fl & 1, st: (fl >> 1) & 1, de: (fl >> 2) & 1,
        bo: +f[7] / 100,
        pax,
      });
    }
  }
  // p[7] is the per-slot score section buildTrains() appends. Guarded: a phone running this
  // client against firmware that predates it just gets no "+N".
  const sc = p[7] ? p[7].split(",").map(Number) : null;
  return { type: "trains", seq: +p[1], t: +p[2], score: +p[3], total: +p[4], rev: +p[5], trains, sc };
}

//   W|chaos|stations|switches|junctions|boosts|bonus|frz|pu
//
// Rebuilt into the shape the JSON message had, so the "world" case in handle() is unchanged.
// `cost` is derived locally via puCostOf() rather than sent -- see PU_COSTS above.
function parseWorld(d) {
  const p = d.split("|");
  const ints = (t) => (t ? t.split(",").map(Number) : []);

  const stations = [];
  if (p[2]) {
    const recs = p[2].split(";");
    for (let i = 0; i < recs.length; i++) {
      const f = recs[i].split(",");
      const queue = [];
      for (let k = 1; k < f.length; k++) queue.push(+f[k]);
      stations.push({ id: i, open: +f[0] === 1, queue });
    }
  }

  const switches = [];
  if (p[3]) {
    const recs = p[3].split(";");
    for (let i = 0; i < recs.length; i++) switches.push({ id: i, active: +recs[i] });
  }

  const junctions = [];
  if (p[4]) {
    const recs = p[4].split(";");
    for (let i = 0; i < recs.length; i++) {
      const f = recs[i].split(",");
      junctions.push({ id: i, active: +f[0], incoming: +f[1] === 1 });
    }
  }

  const boosts = [];
  if (p[5]) {
    const recs = p[5].split(";");
    for (let i = 0; i < recs.length; i++) {
      const f = recs[i].split(",");
      boosts.push({ id: i, armed: +f[0] === 1, cd: +f[1] / 100 });
    }
  }

  let bonus = null;
  if (p[6]) {
    const f = p[6].split(",");
    bonus = { color: +f[0], phase: +f[1], t: +f[2] / 100, left: +f[3] };
  }

  const pu = [];
  if (p[8]) {
    const recs = p[8].split(";");
    for (let i = 0; i < recs.length; i++) {
      const f = recs[i].split(",");
      const lv = +f[0];
      // [lv, own, arm, cost, act] -- same tuple layout renderPowerups() destructures.
      pu.push([lv, +f[1], +f[2], puCostOf(i, lv), +f[3] / 100]);
    }
  }

  return {
    type: "world",
    chaos: +p[1] / 100,
    stations, switches, junctions, boosts, bonus,
    frz: ints(p[7]),
    pu,
  };
}

//   D|startMs|node,amp,dnx,dny,fx,fy,px,py;...
//
// Integer-scaled on the wire (see sendDriftTo): fx/fy by 1000, the rest by 100. Returns the
// same [index, amp, dnx, dny, fx, fy, px, py] tuples the JSON message carried, so the
// "drift" handler is unchanged.
function parseDrift(d) {
  const p = d.split("|");
  const nodes = [];
  if (p[2]) {
    for (const rec of p[2].split(";")) {
      const f = rec.split(",");
      nodes.push([
        +f[0],
        +f[1] / 100, +f[2] / 100, +f[3] / 100,
        +f[4] / 1000, +f[5] / 1000,
        +f[6] / 100, +f[7] / 100,
      ]);
    }
  }
  return { type: "drift", startMs: +p[1], nodes };
}

function connect() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(`${proto}://${location.host}/ws`);

  const openWatchdog = setTimeout(() => {
    if (ws && ws.readyState === WebSocket.CONNECTING) {
      try {
        ws.close();
      } catch {}
    }
  }, 3000);

  let stableTimer = null;

  ws.onopen = () => {
    clearTimeout(openWatchdog);
    setReconnecting(false);
    statusEl.textContent = "Connected. Waiting for everyone…";
    sendHello();
    stableTimer = setTimeout(() => {
      reconnectAttempts = 0;
    }, STABLE_MS);
  };

  ws.onclose = () => {
    clearTimeout(openWatchdog);
    clearTimeout(stableTimer);
    if (serverFull) return;
    if (!gameInProgress) setReconnecting(true);
    const base = Math.min(800 * 2 ** reconnectAttempts, 8000);
    reconnectAttempts++;
    setTimeout(connect, base + Math.random() * 1700);
  };

  ws.onmessage = (ev) => {
    // The two hot broadcasts are compact positional text, not JSON -- see buildTrains() /
    // buildWorld() in esp32.ino. Everything else is still JSON, so dispatch on the tag.
    const d = ev.data;
    const tag = d.charCodeAt(0);
    const msg = d.charCodeAt(1) !== 124 ? JSON.parse(d)
              : tag === 84 /* T */ ? parseTrains(d)
              : tag === 87 /* W */ ? parseWorld(d)
              : tag === 68 /* D */ ? parseDrift(d)
              : JSON.parse(d);
    handle(msg);
  };
}

function send(msg) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(msg));
}

function tickClock() {
  if (clockEl && lobbyEl && !lobbyEl.classList.contains("hidden")) {
    const d = new Date();
    const p = (n, l = 2) => String(n).padStart(l, "0");
    clockEl.textContent = `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`;
  }
  requestAnimationFrame(tickClock);
}
tickClock();

function setReconnecting(on) {
  reconnectModal?.classList.toggle("hidden", !on);
}


let lastHelloDims = { w: 0, h: 0 };
function sendHello() {
  const v = viewport();
  lastHelloDims = { w: v.w, h: v.h };
  send({ type: "hello", token, screen: v, hasLayout: !!lastLayout });
}

function handle(msg) {
  switch (msg.type) {
    case "welcome":
      gameInProgress = false;
      me = { id: msg.id, color: msg.color };
      break;

    case "full":
      serverFull = true;
      statusEl.textContent = `Server full — max ${msg.max} devices connected.`;
      break;

    case "busy":
      gameInProgress = true;
      statusEl.textContent = "A game is already in progress. You'll join when it finishes…";
      break;

    case "lobby":
      gameInProgress = false;
      sessionStorage.removeItem("tg_ingame");
      showLobby(msg);
      break;

    case "layout":
      lastLayout = msg;
      if (msg.you && typeof msg.you.slot === "number") {
        mySlot = msg.you.slot;
        setOwnIcon(hudEl, mySlot + 1);
        renderPowerups();
      }
      orientHud();
      sessionStorage.setItem("tg_ingame", "1");
      // Rehydrate what sendLayoutTo() flattened or dropped, so every draw* function below
      // still sees the object shapes it always did. nodes/links arrive as flat int arrays;
      // the per-object `id` is the array index; edge colours arrive as a bare hue.
      for (let i = 0; i < (msg.edges || []).length; i++) {
        const e = msg.edges[i];
        if (e.hue != null && e.color == null) e.color = `hsl(${e.hue}, 85%, 55%)`;
      }
      if (lastLayout.rail) {
        const rail = lastLayout.rail;
        if (typeof rail.colors === "number") stationColors = rail.colors;
        if (typeof rail.maxQ === "number") maxQueue = rail.maxQ;
        if (rail.nodes.length && typeof rail.nodes[0] === "number") {
          const flat = rail.nodes;
          const nodes = new Array(flat.length / 2);
          for (let i = 0; i < nodes.length; i++) {
            const x = flat[i * 2], y = flat[i * 2 + 1];
            nodes[i] = { x, y, bx: x, by: y };
          }
          rail.nodes = nodes;
        }
        if (rail.links.length && typeof rail.links[0] === "number") {
          const flat = rail.links;
          const links = new Array(flat.length / 2);
          for (let i = 0; i < links.length; i++) links[i] = [flat[i * 2], flat[i * 2 + 1]];
          rail.links = links;
        }
        for (const key of ["stations", "junctions", "switches", "boosts"]) {
          const arr = rail[key];
          if (arr) for (let i = 0; i < arr.length; i++) if (arr[i].id == null) arr[i].id = i;
        }
        rail.hiddenSet = new Set(rail.hidden || []);
        rail.teleportSet = new Set(rail.teleport || []);
        const inL = {};
        const outL = {};
        (rail.links || []).forEach((l, i) => {
          (inL[l[1]] = inL[l[1]] || []).push(i);
          (outL[l[0]] = outL[l[0]] || []).push(i);
        });
        rail.inLinks = inL;
        rail.outLinks = outL;
        for (const n of rail.nodes) {
          n.bx = n.x;
          n.by = n.y;
        }
        nodeToStation = new Map();
        // Keyed by berth node, and the berth swaps ends with the direction, so map both.
        for (let i = 0; i < rail.stations.length; i++) {
          nodeToStation.set(rail.stations[i].n0, i);
          nodeToStation.set(rail.stations[i].n1, i);
        }
      }
      stationCombos.clear();
      driftReady = false;
      trainStates = [];
      timelineReady = false;
      showPlay();
      if (msg.placing) showPlacement(!!msg.placed);
      else hidePlacement();
      break;

    case "start":
      hidePlacement();
      break;

    case "gameover":
      lobbyEl.classList.add("hidden");
      playEl.classList.remove("hidden");
      finalScoreEl.textContent = msg.score;
      finalTimeEl.textContent = formatDuration(msg.timeMs);
      showLeague(msg.score);
      resetGameoverButtons();
      gameoverEl.classList.remove("hidden");
      orientGameover();
      stopRender();
      break;

    case "trains":
      {
        const shown = typeof msg.total === "number" ? msg.total : typeof msg.score === "number" ? msg.score : null;
        if (shown != null) {
          scoreEl.textContent = shown;
          if (scoreLabelEl) scoreLabelEl.style.color = shown > 0 ? RING_COLORS[0] : "#8a97a8";
        }
        // Slots that scored in the same message all land in the one bubble: their shapes sit
        // side by side and the number is the sum.
        if (msg.sc) for (let i = 0; i < msg.sc.length; i++) popScore(i, msg.sc[i]);
      }
      // Powerup rings need the spendable score, which only rides this message.
      if (typeof msg.score === "number") {
        puScore = msg.score;
        renderPowerups();
      }
      if (typeof msg.rev === "number" && msg.rev !== dirRev) {
        dirRev = msg.rev;
        // Buffered samples were taken heading the other way. Don't drop them outright --
        // an empty buffer leaves ts.head null and drawTrains skips the train entirely,
        // so every train would blink out until the next snapshot ~200 ms later. Keep the
        // newest sample and zero its speed instead: the train holds position for at most
        // one snapshot, then resumes smoothly, with no wrong-way dead reckoning.
        for (const ts of trainStates) {
          if (!ts || !ts.buf.length) continue;
          const last = { ...ts.buf[ts.buf.length - 1], sp: 0 };
          ts.buf.length = 0;
          ts.buf.push(last);
          ts.path.length = 0;
        }
      }
      noteSeq(msg.seq);
      syncTrains(msg.trains || [], msg.t);
      break;

    case "drift":
      if (lastLayout?.rail?.nodes && Array.isArray(msg.nodes)) {
        const ns = lastLayout.rail.nodes;
        for (const n of ns) n.drift = 0;
        for (const e of msg.nodes) {
          const n = ns[e[0]];
          if (!n) continue;
          n.drift = 1;
          n.amp = e[1];
          n.dnx = e[2];
          n.dny = e[3];
          n.fx = e[4];
          n.fy = e[5];
          n.px = e[6];
          n.py = e[7];
        }
        if (typeof msg.startMs === "number") driftStartMs = msg.startMs;
        driftReady = true;
      }
      break;

    case "world":
      if (msg.stations && lastLayout?.rail?.stations) {
        const nowT = performance.now();
        for (const s of msg.stations) {
          const st = lastLayout.rail.stations[s.id];
          if (st) {
            st.open = s.open;
            const prev = st.queue || [];
            const prevT = st.spawnT || [];
            const newT = new Array(s.queue.length);
            for (let i = 0; i < s.queue.length; i++) {
              newT[i] = i < prev.length ? prevT[i] : nowT;
            }
            st.queue = s.queue;
            st.spawnT = newT;
          }
        }
      }
      if (msg.junctions && lastLayout?.rail?.junctions) {
        for (const j of msg.junctions) {
          const jc = lastLayout.rail.junctions[j.id];
          if (jc) {
            jc.active = j.active;
            jc.incoming = j.incoming;
          }
        }
      }
      if (msg.switches && lastLayout?.rail?.switches) {
        for (const s of msg.switches) {
          const sw = lastLayout.rail.switches[s.id];
          if (sw) sw.active = s.active;
        }
      }
      if (msg.boosts && lastLayout?.rail?.boosts) {
        for (const b of msg.boosts) {
          const bp = lastLayout.rail.boosts[b.id];
          if (bp) {
            bp.armed = b.armed;
            bp.cd = b.cd || 0;
          }
        }
      }
      bonusColor = msg.bonus ? msg.bonus.color : null;
      bonusPhase = msg.bonus ? msg.bonus.phase : 0;
      bonusT = msg.bonus ? msg.bonus.t : 0;
      bonusLeftMs = msg.bonus && typeof msg.bonus.left === "number" ? msg.bonus.left : Infinity;
      bonusLeftStamp = performance.now();
      if (Array.isArray(msg.frz)) frozenStations = new Set(msg.frz);
      if (Array.isArray(msg.pu)) {
        puState = msg.pu;
        renderPowerups();
      }
      if (typeof msg.chaos === "number") currentChaos = msg.chaos;
      break;
  }
}

const SWIPE_REVEAL = 84;
let openSwipeRow = null;
function closeSwipe(row) {
  if (!row) return;
  row.style.transition = "";
  row.style.transform = "";
  row.classList.remove("swiped");
  if (openSwipeRow === row) openSwipeRow = null;
}
function attachSwipe(row) {
  let startX = 0,
    startY = 0,
    dragging = false,
    decided = false,
    horiz = false,
    base = 0,
    cur = 0;
  const setT = (t) => {
    cur = t;
    row.style.transform = t ? `translateX(${t}px)` : "";
  };
  row.addEventListener("pointerdown", (e) => {
    if (openSwipeRow && openSwipeRow !== row) closeSwipe(openSwipeRow);
    startX = e.clientX;
    startY = e.clientY;
    dragging = true;
    decided = false;
    horiz = false;
    base = row.classList.contains("swiped") ? -SWIPE_REVEAL : 0;
    row.style.transition = "none";
  });
  row.addEventListener("pointermove", (e) => {
    if (!dragging) return;
    const dx = e.clientX - startX,
      dy = e.clientY - startY;
    if (!decided) {
      if (Math.hypot(dx, dy) > 8) {
        decided = true;
        horiz = Math.abs(dx) > Math.abs(dy);
      } else return;
    }
    if (!horiz) return;
    e.preventDefault();
    setT(Math.max(-SWIPE_REVEAL, Math.min(0, base + dx)));
  });
  const end = () => {
    if (!dragging) return;
    dragging = false;
    row.style.transition = "";
    if (cur < -SWIPE_REVEAL / 2) {
      setT(-SWIPE_REVEAL);
      row.classList.add("swiped");
      openSwipeRow = row;
    } else {
      setT(0);
      row.classList.remove("swiped");
      if (openSwipeRow === row) openSwipeRow = null;
    }
  };
  row.addEventListener("pointerup", end);
  row.addEventListener("pointercancel", end);
}
const MAX_SLOTS = 4;

// Each slot's shape IS its identity now -- the icon replaces the "PLAYER n" text, so the
// number only survives as the icon filename and as hidden text for screen readers.
function slotIcon(slotNum) {
  return `url(icon-player-${slotNum}.svg)`;
}

// The two standing icons (lobby "You are", in-game HUD) live in index.html, so they exist
// before a slot is known. Without the class they would paint as a plain white box -- an
// unset --picon means mask-image:none, which masks nothing rather than everything.
// One "+N" under the total, counting only the points of the current burst -- never the
// running total, which is the big number right above it. Points landing while it is up are
// added to it and, if a different slot scored, that slot's shape joins the row.
// POP_IDLE_MS of quiet and it is removed outright. No animation anywhere: it appears, the
// number changes in place, it vanishes.
const POP_IDLE_MS = 600;
let popBubble = null;

function popScore(slot, pts) {
  if (!scorePopsEl || !(pts > 0)) return;

  if (!popBubble) {
    const el = document.createElement("span");
    el.className = "pop";
    el.innerHTML = `<span class="pop-icons"></span><span class="pop-n"></span>`;
    scorePopsEl.appendChild(el);
    popBubble = { el, icons: el.firstElementChild, n: el.lastElementChild, total: 0, slots: [], timer: 0 };
  }

  const b = popBubble;
  b.total += pts;
  if (!b.slots.includes(slot)) {
    b.slots.push(slot);
    b.slots.sort((x, y) => x - y);
    b.icons.innerHTML = b.slots
      .map((sl) => `<span class="pslot-icon pop-icon" style="--picon:${slotIcon(sl + 1)}"></span>`)
      .join("");
  }
  b.n.textContent = "+" + b.total;

  clearTimeout(b.timer);
  b.timer = setTimeout(() => {
    if (popBubble !== b) return;
    popBubble = null;
    b.el.remove();
  }, POP_IDLE_MS);
}

function clearPops() {
  if (popBubble) {
    clearTimeout(popBubble.timer);
    popBubble = null;
  }
  if (scorePopsEl) scorePopsEl.innerHTML = "";
}

function setOwnIcon(host, slotNum) {
  if (!host) return;
  host.style.setProperty("--picon", slotIcon(slotNum));
  host.classList.add("has-icon");
}

function makeSlot(slotNum, p, isMe) {
  const item = document.createElement("div");
  item.className = "phone-item";
  const card = document.createElement("div");
  card.className = p ? "phone-row" : "phone-row empty";
  if (p) {
    card.style.setProperty("--picon", slotIcon(slotNum));
    card.innerHTML =
      `<span class="pslot-icon"></span>` +
      `<span class="visually-hidden">Player ${slotNum}${isMe ? " (you)" : ""}</span>` +
      (isMe ? `<span class="pslot-you">you</span>` : "") +
      `<span class="state ${p.ready ? "ready" : "waiting"}">${p.ready ? "Ready" : "Waiting"}</span>`;
  } else {
    // An empty slot is the dashed outline and nothing else -- the shape and the "not
    // connected" label are both gone, so only assistive tech still gets the slot number.
    card.innerHTML = `<span class="visually-hidden">Player ${slotNum} — not connected</span>`;
  }
  item.appendChild(card);
  if (p && !isMe) {
    const rm = document.createElement("button");
    rm.className = "phone-remove";
    rm.textContent = "Remove";
    rm.addEventListener("click", () => {
      send({ type: "kick", id: p.id });
    });
    item.appendChild(rm);
    attachSwipe(card);
  }
  return item;
}

function showLobby(msg) {
  stopRender();
  releaseWakeLock();
  hidePlacement();
  lastLayout = null;
  trainStates = [];
  timelineReady = false;
  bonusPhase = 0;
  stationCombos.clear();
  playEl.classList.add("hidden");
  if (tutorialEl?.classList.contains("hidden") !== false) lobbyEl.classList.remove("hidden");
  gameoverEl.classList.add("hidden");
  menuOverlayEl?.classList.add("hidden");

  clearPops();
  phoneListEl.innerHTML = "";
  openSwipeRow = null;
  const list = msg.phones;
  // Two different orderings, deliberately. CELLS pack from the top-left, so the dashed
  // empties are always the trailing ones -- msg.phones is already packed, so position in it
  // is the cell. The SHAPE comes from the server slot instead, so it stays with the player
  // when someone ahead of them drops and the list closes up. Older firmware omits `slot`;
  // then the two coincide, which is the old behaviour.
  const shapeOf = (p, pos) => (typeof p.slot === "number" ? p.slot : pos) + 1;

  let meReady = ready;
  for (let i = 0; i < MAX_SLOTS; i++) {
    const p = list[i] || null;
    const isMe = !!(p && me && p.id === me.id);
    if (isMe) meReady = !!p.ready;
    phoneListEl.appendChild(makeSlot(p ? shapeOf(p, i) : 0, p, isMe));
  }

  ready = meReady;
  syncReadyBtn();

  statusEl.textContent = "";
  setReconnecting(false);
}

function syncReadyBtn() {
  readyToggle.classList.toggle("on", ready);
  if (readyKnob) {
    readyKnob.style.transition = "";
    readyKnob.style.transform = "";
  }
  animateTrain(ready);
}

let trainFlown = false;
let trainInited = false;
function animateTrain(readyNow) {
  if (!trainEl) return;
  if (!trainInited) {
    trainInited = true;
    trainFlown = readyNow;
    if (readyNow) trainEl.classList.add("tg-go");
    else trainEnterFromLeft();
    return;
  }
  if (readyNow === trainFlown) return;
  trainFlown = readyNow;
  if (readyNow) {
    trainEl.classList.remove("tg-warp", "tg-enter");
    trainEl.classList.add("tg-go");
  } else {
    trainEnterFromLeft();
  }
}
function trainEnterFromLeft() {
  trainEl.classList.remove("tg-go", "tg-enter");
  trainEl.classList.add("tg-warp");
  requestAnimationFrame(() =>
    requestAnimationFrame(() => {
      trainEl.classList.remove("tg-warp");
      trainEl.classList.add("tg-enter");
    }),
  );
}

function showPlacement(alreadyPlaced) {
  placing = true;
  placed = !!alreadyPlaced;
  if (placeConfirmEl) placeConfirmEl.classList.remove("hidden");
  hudEl?.classList.add("hidden");
  syncPlaceBtn();
}
function hidePlacement() {
  placing = false;
  placed = false;
  if (placeConfirmEl) placeConfirmEl.classList.add("hidden");
  hudEl?.classList.remove("hidden");
  orientHud();
}
function syncPlaceBtn() {
  if (!placeBtn) return;
  placeBtn.disabled = false;
  placeBtn.classList.toggle("is-ready", placed);
  placeBtn.textContent = placed ? "Cancel" : "Placement is correct";
  if (placeStatusEl)
    placeStatusEl.textContent = placed
      ? "Waiting for the other phones to confirm…"
      : "Line the phones up side by side so the matching colored edges touch.";
}
placeBtn?.addEventListener("click", () => {
  if (!placing) return;
  placed = !placed;
  send({ type: "placed", value: placed });
  syncPlaceBtn();
});
placeLobbyBtn?.addEventListener("click", () => {
  send({ type: "reset" });
});

function setReady(value) {
  if (ready === value) {
    syncReadyBtn();
    return;
  }
  ready = value;
  send({ type: "ready", value: ready });
  syncReadyBtn();
}

(function initReadyToggle() {
  if (!readyToggle || !readyKnob) return;
  let dragging = false,
    moved = false,
    startX = 0,
    rect = null,
    maxX = 0;
  readyToggle.addEventListener("pointerdown", (e) => {
    dragging = true;
    moved = false;
    startX = e.clientX;
    rect = readyToggle.getBoundingClientRect();
    maxX = rect.width / 2;
    readyKnob.style.transition = "none";
    try {
      readyToggle.setPointerCapture(e.pointerId);
    } catch {}
  });
  readyToggle.addEventListener("pointermove", (e) => {
    if (!dragging) return;
    const dx = e.clientX - startX;
    if (Math.abs(dx) > 4) moved = true;
    const x = Math.max(0, Math.min(maxX, (ready ? maxX : 0) + dx));
    readyKnob.style.transform = `translateX(${x}px)`;
    readyToggle.classList.toggle("on", x > maxX / 2);
  });
  const finish = (e) => {
    if (!dragging) return;
    dragging = false;
    let wantOn;
    if (moved) {
      const x = Math.max(0, Math.min(maxX, (ready ? maxX : 0) + (e.clientX - startX)));
      wantOn = x > maxX / 2;
    } else {
      wantOn = e.clientX - rect.left > rect.width / 2;
    }
    setReady(wantOn);
  };
  readyToggle.addEventListener("pointerup", finish);
  readyToggle.addEventListener("pointercancel", finish);
})();

const LEAGUE_BASE = ["Local", "Commuter", "Intercity", "Express", "Bullet"];
const LEAGUE_TIER = 500;
const LEAGUE_ROMAN = ["I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"];
function leagueRoman(n) {
  return LEAGUE_ROMAN[n] || String(n + 1);
}
function scoreToLeague(score) {
  const i = Math.floor(Math.max(0, score) / LEAGUE_TIER);
  const name = LEAGUE_BASE[i % LEAGUE_BASE.length];
  const mark = Math.floor(i / LEAGUE_BASE.length);
  // The word "Mark" is in the markup -- only the numeral and the tier name change, and the
  // numeral is kept apart from the rest so the row's lowercasing cannot reach it.
  return { roman: leagueRoman(mark), type: name };
}
function showLeague(score) {
  const { roman, type } = scoreToLeague(score);
  leagueRomanEl.textContent = roman;
  leagueTypeEl.textContent = type;
  leagueProgEl.textContent = `${LEAGUE_TIER - (Math.max(0, score) % LEAGUE_TIER)} points to next rank`;
}

function outwardScreenDir() {
  const you = lastLayout?.you,
    board = lastLayout?.board;
  if (!you || !board) return { x: 0, y: -1 };
  const ox = you.x + you.w / 2 - board.w / 2;
  const oy = you.y + you.h / 2 - board.h / 2;
  let tx = 0,
    ty = 0;
  if (Math.abs(ox) >= Math.abs(oy)) tx = ox >= 0 ? 1 : -1;
  else ty = oy >= 0 ? 1 : -1;
  const PW = window.innerWidth,
    PH = window.innerHeight;
  const deviceOri = PW >= PH ? "landscape" : "portrait";
  const rotated = you.orientation && deviceOri !== you.orientation;
  return rotated ? { x: -ty, y: tx } : { x: tx, y: ty };
}

function topVecFor(deg) {
  const r = (deg * Math.PI) / 180;
  return { x: Math.sin(r), y: -Math.cos(r) };
}
function bestRotDeg(candidates, out) {
  let best = candidates[0],
    bestDot = -Infinity;
  for (const d of candidates) {
    const t = topVecFor(d);
    const dot = t.x * out.x + t.y * out.y;
    if (dot > bestDot + 1e-6) {
      bestDot = dot;
      best = d;
    }
  }
  return best;
}

// The HUD normally points itself away from the middle of the board, which assumes whoever
// reads it is sitting on the outside of that phone. Someone leaning over from the other
// side wants it the other way up, so they can flip their own phone's HUD from the menu.
// Per-device and sticky across reloads -- where you sit does not change between games.
let hudFlipped = localStorage.getItem("tg_hudflip") === "1";

function orientHud() {
  if (!hudEl) return;
  const portrait = window.innerHeight > window.innerWidth;
  const out = outwardScreenDir();
  const deg = bestRotDeg(portrait ? [90, 270] : [0, 180], { x: -out.x, y: -out.y });
  hudEl.style.setProperty("--hud-rot", (deg + (hudFlipped ? 180 : 0)) + "deg");
}

function orientGameover() {
  const portrait = window.innerHeight > window.innerWidth;
  gameoverEl?.classList.toggle("rotated", portrait);
  const out = outwardScreenDir();
  const deg = bestRotDeg(portrait ? [90, 270] : [0, 180], { x: -out.x, y: -out.y });
  gameoverEl?.style.setProperty("--go-rot", deg + "deg");
}

function resetGameoverButtons() {
  gameoverMainEl?.classList.remove("hidden");
  retryChoiceEl?.classList.add("hidden");
}
gameoverBtn.addEventListener("click", () => {
  gameoverEl.classList.add("hidden");
  send({ type: "reset" });
});
retryBtn?.addEventListener("click", () => {
  gameoverMainEl.classList.add("hidden");
  retryChoiceEl.classList.remove("hidden");
});
retryKeepBtn?.addEventListener("click", () => send({ type: "retry", mode: "keep" }));
retryNewBtn?.addEventListener("click", () => send({ type: "retry", mode: "new" }));
retryCancelBtn?.addEventListener("click", resetGameoverButtons);

function openMenu() {
  menuOverlayEl?.classList.remove("hidden");
}
menuBtn?.addEventListener("click", openMenu);
closeMenuBtn?.addEventListener("click", () => menuOverlayEl?.classList.add("hidden"));
// Close on the way out: the overlay sits over the HUD, so leaving it open would hide the
// very thing that just turned around.
flipHudBtn?.addEventListener("click", () => {
  hudFlipped = !hudFlipped;
  localStorage.setItem("tg_hudflip", hudFlipped ? "1" : "0");
  orientHud();
  menuOverlayEl?.classList.add("hidden");
});

menuOverlayEl?.addEventListener("click", (ev) => {
  if (ev.target === menuOverlayEl) menuOverlayEl.classList.add("hidden");
});
lobbyBtn?.addEventListener("click", () => {
  menuOverlayEl?.classList.add("hidden");
  send({ type: "reset" });
});

let tutorialRaf = 0;
function openTutorial() {
  lobbyEl.classList.add("hidden");
  tutorialEl.classList.remove("hidden");
  renderTutorial();
}
helpBtn?.addEventListener("click", openTutorial);
function closeTutorial() {
  tutorialEl.classList.add("hidden");
  lobbyEl.classList.remove("hidden");
  if (tutorialRaf) {
    cancelAnimationFrame(tutorialRaf);
    tutorialRaf = 0;
  }
}
tutBackBtn?.addEventListener("click", closeTutorial);
tutCloseBtn?.addEventListener("click", closeTutorial);

const tutStates = new Map();
function renderTutorial() {
  tutStates.clear();
  const canvases = tutorialEl.querySelectorAll("canvas[data-tut]");
  const setups = [];
  for (const c of canvases) {
    const dpr = window.devicePixelRatio || 1;
    if (!c.dataset.dw) {
      c.dataset.dw = c.width;
      c.dataset.dh = c.height;
    }
    const dw = +c.dataset.dw,
      dh = +c.dataset.dh;
    c.style.aspectRatio = `${dw} / ${dh}`;
    const rect = c.getBoundingClientRect();
    const cssW = rect.width || dw;
    const cssH = (cssW * dh) / dw;
    c.width = Math.round(cssW * dpr);
    c.height = Math.round(cssH * dpr);
    const scale = (cssW / dw) * dpr;
    const cx = c.getContext("2d");
    cx.setTransform(scale, 0, 0, scale, 0, 0);
    const kind = c.dataset.tut;
    tutStates.set(kind, initTutState(kind, dw, dh));
    setups.push({ kind, ctx: cx, w: dw, h: dh, canvas: c });
    if (!c.dataset.bound) {
      c.dataset.bound = "1";
      // Illustration coordinates, not CSS ones: every tut* helper draws in the canvas's
      // nominal dw x dh space and the context is scaled to fit.
      const toIll = (ev) => {
        const r = c.getBoundingClientRect();
        return [((ev.clientX - r.left) / r.width) * dw, ((ev.clientY - r.top) / r.height) * dh];
      };
      // The boost illustration is swiped, like the real pad; everything else is tapped.
      // Nothing on the pad illustration answers to a tap, so a press short of a swipe
      // simply unwinds -- which is itself part of what the panel is teaching.
      let drag = null;
      c.addEventListener("pointerdown", (ev) => {
        ev.preventDefault();
        const [px, py] = toIll(ev);
        if (c.dataset.tut !== "boost") {
          handleTutTap(c.dataset.tut, px, py, dw, dh);
          return;
        }
        drag = { id: ev.pointerId, x0: px };
        try {
          c.setPointerCapture(ev.pointerId);
        } catch {}
      });
      c.addEventListener("pointermove", (ev) => {
        if (!drag || ev.pointerId !== drag.id) return;
        const [px] = toIll(ev);
        const st = tutStates.get("boost");
        // The illustrated pad always runs left to right, so the swipe axis is simply x.
        if (st) st.prog = boostProgress(px - drag.x0);
      });
      const endDrag = (ev) => {
        if (!drag || ev.pointerId !== drag.id) return;
        drag = null;
        const st = tutStates.get("boost");
        const prog = st ? st.prog : 0;
        if (st) st.prog = 0;
        if (Math.abs(prog) >= BOOST_SWIPE_DONE && st && st.cd <= 0) st.armed = prog > 0;
      };
      c.addEventListener("pointerup", endDrag);
      c.addEventListener("pointercancel", () => {
        drag = null;
        const st = tutStates.get("boost");
        if (st) st.prog = 0;
      });
    }
  }
  let lastT = performance.now();
  const tick = (now) => {
    if (typeof now !== "number") now = performance.now();
    const dt = Math.min(0.05, (now - lastT) / 1000);
    lastT = now;
    for (const s of setups) drawTutIllustration(s.kind, s.ctx, s.w, s.h, now, dt);
    tutorialRaf = requestAnimationFrame(tick);
  };
  tick();
}

function initTutState(kind, W, H) {
  if (kind === "station") {
    return {
      gateOpen: false,
      trainX: -30,
      speed: 70,
      queue: [TUT_COLORS[0], TUT_COLORS[0]],
      carrying: [null, null, null, null],
      stopped: false,
      dwell: 0,
      boarded: 0,
      passedOpen: false,
    };
  }
  if (kind === "junction") {
    return { diverted: false, trainX: -30, speed: 90, hideT: 0, lockedDivert: null };
  }
  if (kind === "boost") {
    return { armed: false, trainX: -30, speed: 90, boostLeft: 0, cd: 0, prog: 0 };
  }
  if (kind === "freeze") {
    return { armed: false, lockLeft: 0 };
  }
  return null;
}

const TUT_FREEZE_DUR = 3;
const TUT_FREEZE_CD = 3;
const TUT_FREEZE_TOTAL = TUT_FREEZE_DUR + TUT_FREEZE_CD;

function handleTutTap(kind, x, y, W, H) {
  const s = tutStates.get(kind);
  if (!s) return;
  if (kind === "station") {
    const trackY = H - 46,
      half = 30;
    const gx = W / 2 + half,
      gy = trackY;
    if (Math.hypot(x - gx, y - gy) < 44) s.gateOpen = true;
    return;
  }
  if (kind === "junction") {
    const trackY = H / 2 - 12;
    const jx = W * 0.3,
      lx = jx - 10,
      ly = trackY + 8;
    if (Math.hypot(x - lx, y - ly) < 44) s.diverted = !s.diverted;
    return;
  }
  if (kind === "freeze") {
    const bcx = 34,
      bcy = 34;
    if (Math.hypot(x - bcx, y - bcy) < 30) {
      if (s.lockLeft <= 0) s.armed = !s.armed;
      return;
    }
    const trackY = H - 46;
    if (s.armed && Math.hypot(x - W / 2, y - (trackY - 27)) < 55) {
      s.armed = false;
      s.lockLeft = TUT_FREEZE_TOTAL;
    }
    return;
  }
}

const TUT_COLORS = ["hsl(0,70%,55%)", "hsl(48,70%,55%)", "hsl(144,70%,55%)", "hsl(216,70%,55%)", "hsl(288,70%,55%)"];

function tutTrack(cx, path) {
  cx.save();
  cx.lineJoin = "round";
  cx.lineCap = "round";
  cx.beginPath();
  path();
  cx.lineWidth = 5;
  cx.strokeStyle = "rgba(255,255,255,0.15)";
  cx.stroke();
  cx.beginPath();
  path();
  cx.lineWidth = 2;
  cx.strokeStyle = "#ffffff";
  cx.stroke();
  cx.restore();
}

function tutStation(cx, x, y, nx, ny, color, open, queue, platHidden, frozen) {
  const PLAT = 20,
    DEPTH = 15,
    QR = 4,
    QG = 3,
    OFF_Q = 10;
  const tx = -ny,
    ty = nx;
  const half = 30;
  const ax = x - tx * half,
    ay = y - ty * half;
  const bx = x + tx * half,
    by = y + ty * half;
  const o1x = nx * PLAT,
    o1y = ny * PLAT;
  const o2x = nx * (PLAT + DEPTH),
    o2y = ny * (PLAT + DEPTH);
  cx.save();
  if (!platHidden) {
    cx.beginPath();
    cx.moveTo(ax + o1x, ay + o1y);
    cx.lineTo(bx + o1x, by + o1y);
    cx.lineTo(bx + o2x, by + o2y);
    cx.lineTo(ax + o2x, ay + o2y);
    cx.closePath();
    cx.fillStyle = color;
    cx.strokeStyle = "#11151c";
    cx.lineWidth = 2;
    cx.fill();
    cx.stroke();
    if (frozen) {
      cx.fillStyle = "rgba(150, 220, 255, 0.55)";
      cx.fill();
      cx.strokeStyle = "#bfe9ff";
      cx.lineWidth = 2.5;
      cx.stroke();
    }
  }
  const GH = 10;
  const gdx = open ? tx : nx,
    gdy = open ? ty : ny;
  cx.beginPath();
  cx.moveTo(bx - gdx * GH, by - gdy * GH);
  cx.lineTo(bx + gdx * GH, by + gdy * GH);
  cx.strokeStyle = "#11151c";
  cx.lineWidth = 5;
  cx.lineCap = "round";
  cx.stroke();
  cx.strokeStyle = "#f4f7fb";
  cx.lineWidth = 2.5;
  cx.stroke();
  const START = 8;
  for (let k = 0; k < queue.length; k++) {
    const along = START + QR + k * (2 * QR + QG);
    const qx = bx - tx * along + nx * OFF_Q;
    const qy = by - ty * along + ny * OFF_Q;
    cx.beginPath();
    cx.arc(qx, qy, QR, 0, Math.PI * 2);
    cx.fillStyle = queue[k];
    cx.strokeStyle = "#11151c";
    cx.lineWidth = 1.2;
    cx.fill();
    cx.stroke();
  }
  cx.restore();
  return { ax, ay, bx, by, tx, ty };
}

function tutTrain(cx, x, y, dx, dy, pax, tint, carStroke) {
  const R = 9;
  const px = -dy,
    py = dx;
  const base = tint || "#ffffff";
  cx.save();
  cx.strokeStyle = carStroke || base;
  cx.lineWidth = 2.2;
  cx.lineJoin = "round";
  for (let k = pax.length - 1; k >= 0; k--) {
    const cxp = x - dx * (k * 22),
      cyp = y - dy * (k * 22);
    if (k === 0) {
      const tipx = cxp + dx * R * 1.6,
        tipy = cyp + dy * R * 1.6;
      const r1x = cxp - dx * R * 0.9 + px * R * 1.15,
        r1y = cyp - dy * R * 0.9 + py * R * 1.15;
      const r2x = cxp - dx * R * 0.9 - px * R * 1.15,
        r2y = cyp - dy * R * 0.9 - py * R * 1.15;
      cx.beginPath();
      cx.moveTo(tipx, tipy);
      cx.lineTo(r1x, r1y);
      cx.lineTo(r2x, r2y);
      cx.closePath();
      cx.fillStyle = pax[0] || base;
      cx.fill();
    } else {
      cx.beginPath();
      cx.arc(cxp, cyp, R, 0, Math.PI * 2);
      cx.stroke();
    }
    if (pax[k] && k !== 0) {
      cx.beginPath();
      cx.arc(cxp, cyp, R * 0.55, 0, Math.PI * 2);
      cx.fillStyle = pax[k];
      cx.fill();
    }
  }
  cx.restore();
}

function tutMouth(cx, mx, my, fx, fy) {
  cx.save();
  cx.translate(mx, my);
  cx.rotate(Math.atan2(fy, fx));
  cx.beginPath();
  cx.ellipse(0, 0, 11, 9, 0, -1.95, 1.95);
  cx.strokeStyle = "#9aa3b2";
  cx.lineWidth = 2.5;
  cx.stroke();
  cx.restore();
}

// The pad as the board draws it -- a run of arrowheads lit by the swipe, brightened as one
// when it commits -- so the thing the tutorial teaches you to swipe looks like the thing
// you will swipe. `prog` is the swipe in flight, signed along travel; see drawBoosts.
function tutBoost(cx, x, y, dx, dy, armed, recharging, prog) {
  const px = -dy,
    py = dx;
  cx.save();
  cx.lineJoin = "round";
  cx.globalAlpha = armed ? 1 : BOOST_ALPHA_IDLE;
  if (recharging) cx.globalAlpha *= BOOST_ALPHA_CD;
  const p = prog || 0;
  const nLit = p === 0
    ? 0
    : Math.min(BOOST_TRI_N, Math.floor(Math.abs(p) * BOOST_TRI_N) + 1);
  for (let i = 0; i < BOOST_TRI_N; i++) {
    const swept = p > 0 ? i < nLit : i >= BOOST_TRI_N - nLit;
    const live = p < 0 ? armed && !swept : armed || swept;
    cx.fillStyle = live ? BOOST_ACTIVE : "#ffffff";
    const base = (i - (BOOST_TRI_N - 1) / 2) * BOOST_TRI_STEP + BOOST_TRI_SHIFT;
    const tip = base + BOOST_TRI_H;
    cx.beginPath();
    cx.moveTo(x + dx * base - px * BOOST_TRI_HALF, y + dy * base - py * BOOST_TRI_HALF);
    cx.lineTo(x + dx * tip, y + dy * tip);
    cx.lineTo(x + dx * base + px * BOOST_TRI_HALF, y + dy * base + py * BOOST_TRI_HALF);
    cx.closePath();
    cx.fill();
  }
  cx.restore();
}

function drawTutIllustration(kind, cx, W, H, t, dt) {
  cx.clearRect(0, 0, W, H);

  if (kind === "board") {
    const pad = 22,
      gap = 6,
      tileW = (W - 2 * pad - gap) / 2,
      tileH = (H - 2 * pad - gap) / 2;
    cx.fillStyle = "#141a23";
    cx.strokeStyle = "#2a3340";
    cx.lineWidth = 1.5;
    for (let r = 0; r < 2; r++)
      for (let c = 0; c < 2; c++) {
        const tx = pad + c * (tileW + gap),
          ty = pad + r * (tileH + gap);
        cx.beginPath();
        cx.rect(tx, ty, tileW, tileH);
        cx.fill();
        cx.stroke();
      }
    const x0 = pad - 8,
      y0 = pad - 8,
      x1 = W - pad + 8,
      y1 = H - pad + 8,
      rr = 14;
    tutTrack(cx, () => {
      cx.moveTo(x0 + rr, y0);
      cx.lineTo(x1 - rr, y0);
      cx.arcTo(x1, y0, x1, y0 + rr, rr);
      cx.lineTo(x1, y1 - rr);
      cx.arcTo(x1, y1, x1 - rr, y1, rr);
      cx.lineTo(x0 + rr, y1);
      cx.arcTo(x0, y1, x0, y1 - rr, rr);
      cx.lineTo(x0, y0 + rr);
      cx.arcTo(x0, y0, x0 + rr, y0, rr);
      cx.closePath();
    });
    tutTrain(cx, W / 2 + 30, y0, 1, 0, [null, null, null, null]);
    return;
  }

  if (kind === "train") {
    tutTrack(cx, () => {
      cx.moveTo(10, H / 2);
      cx.lineTo(W - 10, H / 2);
    });
    tutTrain(cx, W - 40, H / 2, 1, 0, [TUT_COLORS[2], TUT_COLORS[2], TUT_COLORS[2], null]);
    return;
  }

  if (kind === "fulltrain") {
    const c = TUT_COLORS[2];
    tutTrack(cx, () => {
      cx.moveTo(10, H / 2 - 6);
      cx.lineTo(W - 10, H / 2 - 6);
    });
    tutTrain(cx, W - 40, H / 2 - 6, 1, 0, [c, c, c, c], null, c);
    cx.fillStyle = "#ffd24a";
    cx.font = 'bold 12px "HostGrotesk", system-ui, sans-serif';
    cx.textAlign = "center";
    cx.fillText("every car full → 2× on delivery", W / 2, H - 12);
    return;
  }

  if (kind === "station") {
    const s = tutStates.get(kind);
    const trackY = H - 46;
    const half = 30;
    const stopX = W / 2 + half;
    tutTrack(cx, () => {
      cx.moveTo(10, trackY);
      cx.lineTo(W - 10, trackY);
    });

    if (s) {
      if (!s.stopped) {
        s.trainX += s.speed * dt;
        if (!s.gateOpen && s.trainX >= stopX - 2 && s.trainX < stopX + 6) {
          s.trainX = stopX;
          s.stopped = true;
          s.dwell = 0;
          s.boarded = 0;
        }
        if (s.gateOpen && s.trainX >= stopX) s.passedOpen = true;
        if (s.trainX > W + 40) {
          s.trainX = -30;
          if (s.passedOpen) s.gateOpen = false;
          s.passedOpen = false;
          s.carrying = [null, null, null, null];
          const c = TUT_COLORS[Math.floor(Math.random() * 4) + 1];
          s.queue = [c, c];
        }
      } else {
        s.dwell += dt;
        const target = Math.floor((s.dwell - 0.25) / 0.35);
        while (s.boarded < target && s.queue.length > 0) {
          const c = s.queue.shift();
          const slot = s.carrying.indexOf(null, 1);
          if (slot >= 0) s.carrying[slot] = c;
          if (s.carrying[0] == null) s.carrying[0] = c;
          s.boarded++;
        }
        if (s.gateOpen) {
          s.stopped = false;
          s.passedOpen = true;
        }
      }
    }

    tutStation(cx, W / 2, trackY, 0, -1, TUT_COLORS[3], s?.gateOpen, s?.queue || []);
    const pax = s?.carrying || [null, null, null, null];
    tutTrain(cx, s?.trainX ?? -30, trackY, 1, 0, pax);

    cx.fillStyle = s?.gateOpen ? "#6fd2ff" : "#8a97a8";
    cx.font = '11px "HostGrotesk", system-ui, sans-serif';
    cx.textAlign = "center";
    let hint;
    if (s?.stopped) {
      const stillBoarding = s.queue.length > 0;
      hint = stillBoarding ? "boarding…" : "boarded — tap the gate to release the train";
    } else {
      hint = s?.gateOpen ? "gate open — train blows past" : "gate closed — train will stop";
    }
    cx.fillText(hint, W / 2, H - 14);
    return;
  }

  if (kind === "passengers") {
    const trackY = H - 40;
    tutTrack(cx, () => {
      cx.moveTo(10, trackY);
      cx.lineTo(W - 10, trackY);
    });
    tutStation(cx, 55, trackY, 0, -1, TUT_COLORS[0], false, [TUT_COLORS[2]]);
    tutStation(cx, W - 55, trackY, 0, -1, TUT_COLORS[2], false, []);
    tutTrain(cx, W / 2 + 22, trackY, 1, 0, [TUT_COLORS[2], TUT_COLORS[2], null]);
    return;
  }

  if (kind === "overflow") {
    const trackY = H / 2 + 8;
    tutTrack(cx, () => {
      cx.moveTo(10, trackY);
      cx.lineTo(W - 10, trackY);
    });
    tutStation(cx, 70, trackY, 0, -1, TUT_COLORS[3], false, [TUT_COLORS[3], TUT_COLORS[1]]);
    const platHidden = (Math.floor(t / 160) & 1) !== 0;
    tutStation(
      cx,
      W - 70,
      trackY,
      0,
      -1,
      TUT_COLORS[0],
      false,
      [TUT_COLORS[2], TUT_COLORS[1], TUT_COLORS[3], TUT_COLORS[4], TUT_COLORS[2], TUT_COLORS[1]],
      platHidden,
    );
    cx.fillStyle = "#ff6b6b";
    cx.font = 'bold 11px "HostGrotesk", system-ui, sans-serif';
    cx.textAlign = "center";
    cx.fillText("FULL", W - 70, H - 12);
    cx.fillStyle = "#8a97a8";
    cx.font = '11px "HostGrotesk", system-ui, sans-serif';
    cx.fillText("normal", 70, H - 12);
    return;
  }

  if (kind === "junction") {
    const s = tutStates.get(kind);
    const trackY = H / 2 - 12;
    const jx = W * 0.3,
      ex = W * 0.72;
    const s2 = Math.SQRT1_2;
    const stubLen = 26;
    const m1x = jx + stubLen * s2,
      m1y = trackY - stubLen * s2;
    const m2x = ex - stubLen * s2,
      m2y = trackY - stubLen * s2;

    if (s) {
      s.trainX += s.speed * dt;
      if (s.trainX >= jx - 1 && s.lockedDivert == null) s.lockedDivert = s.diverted;
      if (s.lockedDivert && s.trainX >= jx && s.trainX <= ex) {
        if (s.hideT < 0.55) {
          s.hideT += dt;
          s.trainX = jx;
        } else {
          s.trainX = ex + 1;
          s.hideT = 0;
        }
      }
      if (s.trainX > W + 30) {
        s.trainX = -30;
        s.lockedDivert = null;
        s.hideT = 0;
      }
    }

    const diverted = !!s?.diverted;
    if (diverted) {
      tutTrack(cx, () => {
        cx.moveTo(10, trackY);
        cx.lineTo(jx, trackY);
      });
      tutTrack(cx, () => {
        cx.moveTo(ex, trackY);
        cx.lineTo(W - 10, trackY);
      });
    } else {
      tutTrack(cx, () => {
        cx.moveTo(10, trackY);
        cx.lineTo(W - 10, trackY);
      });
    }
    tutTrack(cx, () => {
      cx.moveTo(jx, trackY);
      cx.lineTo(m1x, m1y);
    });
    tutTrack(cx, () => {
      cx.moveTo(m2x, m2y);
      cx.lineTo(ex, trackY);
    });
    tutMouth(cx, m1x, m1y, s2, -s2);
    tutMouth(cx, m2x, m2y, -s2, -s2);

    const lvx = jx - 10,
      lvy = trackY + 8;
    const dir = diverted ? { x: 0.4, y: 1 } : { x: -1, y: 0.6 };
    const dl = Math.hypot(dir.x, dir.y);
    const hx = lvx + (dir.x / dl) * 14,
      hy = lvy + (dir.y / dl) * 14;
    cx.save();
    cx.strokeStyle = "#ffffff";
    cx.fillStyle = "#ffffff";
    cx.lineWidth = 2;
    cx.lineCap = "round";
    cx.beginPath();
    cx.moveTo(lvx, lvy);
    cx.lineTo(hx, hy);
    cx.stroke();
    cx.beginPath();
    cx.arc(hx, hy, 3.2, 0, Math.PI * 2);
    cx.fill();
    cx.restore();

    if (s && !(s.lockedDivert && s.hideT > 0)) {
      tutTrain(cx, s.trainX, trackY, 1, 0, [null, null, null, null]);
    }

    cx.fillStyle = diverted ? "#6fd2ff" : "#8a97a8";
    cx.font = '11px "HostGrotesk", system-ui, sans-serif';
    cx.textAlign = "center";
    cx.fillText(
      diverted ? "diverting — trains take the tunnel" : "straight — trains stay on the main line",
      W / 2,
      H - 10,
    );
    return;
  }

  if (kind === "boost") {
    const s = tutStates.get(kind);
    const trackY = H / 2 - 10;
    const bx = W * 0.55;
    tutTrack(cx, () => {
      cx.moveTo(10, trackY);
      cx.lineTo(W - 10, trackY);
    });

    if (s) {
      const mult = s.boostLeft > 0 ? 3 : 1;
      s.trainX += s.speed * mult * dt;
      if (s.boostLeft > 0) s.boostLeft = Math.max(0, s.boostLeft - dt);
      if (s.cd > 0) s.cd = Math.max(0, s.cd - dt);
      if (s.armed && s.trainX >= bx - 4 && s.trainX <= bx + 4) {
        s.armed = false;
        s.boostLeft = 1.6;
        s.cd = 2.5;
      }
      if (s.trainX > W + 30) {
        s.trainX = -30;
        s.boostLeft = 0;
      }
    }

    tutBoost(cx, bx, trackY, 1, 0, s?.armed, s?.cd > 0, s?.prog);
    const tint = s && s.boostLeft > 0 ? "#6fd2ff" : null;
    tutTrain(cx, s?.trainX ?? -30, trackY, 1, 0, [null, null, null, null], tint);

    cx.fillStyle = s?.armed ? "#6fd2ff" : "#8a97a8";
    cx.font = '11px "HostGrotesk", system-ui, sans-serif';
    cx.textAlign = "center";
    cx.fillText(
      s?.boostLeft > 0
        ? "boosting!"
        : s?.armed
          ? "armed — next train gets a 3× burst"
          : s?.cd > 0
            ? "recharging…"
            : "swipe along the arrows to arm the pad",
      W / 2,
      H - 10,
    );
    return;
  }

  if (kind === "freeze") {
    const s = tutStates.get(kind);
    const trackY = H - 46;
    if (s && s.lockLeft > 0) s.lockLeft = Math.max(0, s.lockLeft - dt);
    const frozen = !!s && s.lockLeft > TUT_FREEZE_CD;

    tutTrack(cx, () => {
      cx.moveTo(10, trackY);
      cx.lineTo(W - 10, trackY);
    });
    tutStation(cx, W / 2, trackY, 0, -1, TUT_COLORS[3], false, [TUT_COLORS[3], TUT_COLORS[3]], false, frozen);

    const bcx = 34,
      bcy = 34,
      br = 20;
    cx.save();
    cx.beginPath();
    cx.arc(bcx, bcy, br, 0, Math.PI * 2);
    cx.fillStyle = "#1b212b";
    cx.strokeStyle = "#2a3340";
    cx.lineWidth = 1.5;
    cx.fill();
    cx.stroke();
    if (s && s.lockLeft > 0) {
      const p = s.lockLeft / TUT_FREEZE_TOTAL;
      cx.beginPath();
      cx.arc(bcx, bcy, br - 1, -Math.PI / 2, -Math.PI / 2 + p * Math.PI * 2);
      cx.strokeStyle = "#4aa8ff";
      cx.lineWidth = 3;
      cx.lineCap = "round";
      cx.stroke();
    }
    if (s?.armed) {
      cx.beginPath();
      cx.arc(bcx, bcy, br + 3, 0, Math.PI * 2);
      cx.strokeStyle = "#6fd2ff";
      cx.lineWidth = 2.5;
      cx.stroke();
    }
    const dim = !!s && s.lockLeft > 0;
    cx.strokeStyle = dim ? "rgba(191,233,255,0.35)" : "#bfe9ff";
    cx.lineWidth = 2;
    cx.lineCap = "round";
    for (let a = 0; a < 6; a++) {
      const ang = a * (Math.PI / 3);
      const ex = Math.cos(ang),
        ey = Math.sin(ang);
      cx.beginPath();
      cx.moveTo(bcx, bcy);
      cx.lineTo(bcx + ex * 9, bcy + ey * 9);
      cx.stroke();
      const armx = bcx + ex * 5,
        army = bcy + ey * 5;
      const perpx = -ey,
        perpy = ex;
      cx.beginPath();
      cx.moveTo(armx - perpx * 3 + ex * 2.5, army - perpy * 3 + ey * 2.5);
      cx.lineTo(armx, army);
      cx.lineTo(armx + perpx * 3 + ex * 2.5, army + perpy * 3 + ey * 2.5);
      cx.stroke();
    }
    cx.restore();

    cx.fillStyle = frozen ? "#bfe9ff" : s?.armed ? "#6fd2ff" : "#8a97a8";
    cx.font = '11px "HostGrotesk", system-ui, sans-serif';
    cx.textAlign = "center";
    let msg;
    if (frozen) msg = "frozen — no new passengers, timers paused";
    else if (s && s.lockLeft > 0) msg = "locked out…";
    else if (s?.armed) msg = "armed — any phone can tap the station";
    else msg = "buy the freeze, then tap a station";
    cx.fillText(msg, W / 2, H - 12);
    return;
  }

  if (kind === "bonus") {
    const trackY = H / 2 + 8;
    tutTrack(cx, () => {
      cx.moveTo(10, trackY);
      cx.lineTo(W - 10, trackY);
    });
    tutStation(cx, W / 2, trackY, 0, -1, TUT_COLORS[1], false, [TUT_COLORS[0], TUT_COLORS[3]]);
    const PLAT = 20,
      DEPTH = 15,
      half = 30,
      m = 5;
    const x = W / 2,
      y = trackY,
      nx = 0,
      ny = -1,
      tx = 1,
      ty = 0;
    const ax = x - tx * half,
      ay = y - ty * half,
      bx = x + tx * half,
      by = y + ty * half;
    const o1 = PLAT - m,
      o2 = PLAT + DEPTH + m;
    const pts = [
      [ax + nx * o1 - tx * m, ay + ny * o1 - ty * m],
      [bx + nx * o1 + tx * m, by + ny * o1 + ty * m],
      [bx + nx * o2 + tx * m, by + ny * o2 + ty * m],
      [ax + nx * o2 - tx * m, ay + ny * o2 - ty * m],
    ];
    cx.save();
    cx.beginPath();
    cx.moveTo(pts[0][0], pts[0][1]);
    for (let i = 1; i < 4; i++) cx.lineTo(pts[i][0], pts[i][1]);
    cx.closePath();
    cx.strokeStyle = TUT_COLORS[1];
    cx.lineWidth = 2;
    cx.setLineDash([7, 5]);
    cx.lineDashOffset = -((t * 0.04) % 12);
    cx.stroke();
    cx.setLineDash([]);
    cx.restore();
    cx.fillStyle = "#ffd24a";
    cx.font = 'bold 12px "HostGrotesk", system-ui, sans-serif';
    cx.textAlign = "center";
    cx.fillText("2× points on this color", W / 2, H - 12);
    return;
  }

  if (kind === "upgrades") {
    // The same banded ring the HUD draws, in three states. Keep in step with
    // renderPowerups(): one band per tier, bought bands dim, band 0 is the clock.
    const r = 26,
      cyy = 40,
      gap = 30;
    const N = 3;
    const gapAng = (RING_GAP / RING_C) * Math.PI * 2;
    const bandAng = (Math.PI * 2) / N;
    const states = [
      { lv: 0, fuel: 0, bank: [1, 0.4, 0], cap: "ready" },
      { lv: 2, fuel: 1.55, bank: [0, 0, 0.3], cap: "running" },
      { lv: 0, fuel: 0, bank: [0.45, 0, 0], cap: "saving up" },
    ];
    const band = (x, i, from, to, color) => {
      const a0 = -Math.PI / 2 + i * bandAng + gapAng / 2;
      const a1 = -Math.PI / 2 + (i + 1) * bandAng - gapAng / 2;
      cx.beginPath();
      cx.arc(x, cyy, r, a0 + (a1 - a0) * from, a0 + (a1 - a0) * to);
      cx.strokeStyle = color;
      cx.stroke();
    };
    const totalW = states.length * (r * 2) + (states.length - 1) * gap;
    let x = (W - totalW) / 2 + r;
    cx.lineWidth = 5;
    cx.lineCap = "butt";
    for (const st of states) {
      cx.beginPath();
      cx.arc(x, cyy, r + 3, 0, Math.PI * 2);
      cx.fillStyle = "#1b212b";
      cx.fill();
      for (let i = 0; i < N; i++) {
        band(x, i, 0, 1, i < st.lv ? RING_SPENT[i] : RING_TRACK);
        if (i < st.lv) band(x, i, 0, Math.min(1, st.fuel - i), RING_ACTIVE);
        else if (st.bank[i] > 0) band(x, i, 0, st.bank[i], RING_COLORS[i]);
      }
      cx.fillStyle = "#8a97a8";
      cx.font = '9px "HostGrotesk", system-ui, sans-serif';
      cx.textAlign = "center";
      cx.textBaseline = "middle";
      cx.fillText(st.cap, x, cyy + r + 16);
      x += r * 2 + gap;
    }
    return;
  }

  if (kind === "reverse") {
    // One loop, one train, running the other way every few seconds.
    const cxx = W / 2,
      cyy = H / 2 - 6;
    const rx = Math.min(W * 0.33, 78),
      ry = Math.min(H * 0.3, 44);
    cx.save();
    cx.beginPath();
    cx.ellipse(cxx, cyy, rx, ry, 0, 0, Math.PI * 2);
    cx.strokeStyle = "rgba(255,255,255,0.15)";
    cx.lineWidth = 5;
    cx.stroke();
    cx.strokeStyle = "#ffffff";
    cx.lineWidth = 2;
    cx.stroke();

    // Flip direction every 2.6 s so the effect is visible without interaction.
    const period = 2600;
    const flipped = Math.floor(t / period) % 2 === 1;
    const u = ((t % period) / period) * Math.PI * 2;
    const ang = flipped ? -u : u;
    for (let k = 0; k < 4; k++) {
      const a = ang - k * 0.34 * (flipped ? -1 : 1);
      const px = cxx + Math.cos(a) * rx,
        py = cyy + Math.sin(a) * ry;
      cx.beginPath();
      cx.arc(px, py, k === 0 ? 7 : 5.5, 0, Math.PI * 2);
      cx.fillStyle = k === 0 ? "#ffffff" : TUT_COLORS[3];
      cx.fill();
    }

    // Direction arrows on the loop, pointing the way the train is going.
    cx.strokeStyle = flipped ? TUT_COLORS[0] : TUT_COLORS[2];
    cx.lineWidth = 2.5;
    cx.lineCap = "round";
    for (const base of [Math.PI * 0.5, Math.PI * 1.5]) {
      const px = cxx + Math.cos(base) * rx,
        py = cyy + Math.sin(base) * ry;
      // Tangent of the ellipse, flipped with the direction.
      let tx = -Math.sin(base) * rx,
        ty = Math.cos(base) * ry;
      const L = Math.hypot(tx, ty) || 1;
      tx = (tx / L) * (flipped ? -1 : 1);
      ty = (ty / L) * (flipped ? -1 : 1);
      const nx2 = -ty,
        ny2 = tx;
      cx.beginPath();
      cx.moveTo(px - tx * 8 + nx2 * 6, py - ty * 8 + ny2 * 6);
      cx.lineTo(px + tx * 8, py + ty * 8);
      cx.lineTo(px - tx * 8 - nx2 * 6, py - ty * 8 - ny2 * 6);
      cx.stroke();
    }

    cx.fillStyle = flipped ? TUT_COLORS[0] : TUT_COLORS[2];
    cx.font = 'bold 12px "HostGrotesk", system-ui, sans-serif';
    cx.textAlign = "center";
    cx.textBaseline = "alphabetic";
    cx.fillText(flipped ? "counter-clockwise" : "clockwise", cxx, H - 10);
    cx.restore();
    return;
  }
}


if (typeof location !== "undefined" && location.hash === "#tut") openTutorial();

function showPlay() {
  lobbyEl.classList.add("hidden");
  playEl.classList.remove("hidden");
  gameoverEl.classList.add("hidden");
  menuOverlayEl?.classList.add("hidden");
  scoreEl.textContent = "0";
  ready = false;
  syncReadyBtn();
  buildPowerups();
  resizeCanvas();
  startRender();
  requestWakeLock();
}

// Phones report 3x and up. At 3x a full-screen redraw is ~3 M physical pixels of fill
// every frame, for art that is flat lines and solid discs -- the one thing that buys
// nothing above 2x. Capping drops the pixel count 2.25x on a 3x handset and is invisible
// here; the only thing that softens is the 2 px rail stroke and the 16 px "menu" label.
// Nothing else needs to change: eventToBoard() hit-tests in CSS pixels off
// getBoundingClientRect(), and the viewport is user-scalable=no so this never shifts
// under us mid-game.
const DPR_CAP = 2;
// draw() MUST use this and never read window.devicePixelRatio itself. The backing-store
// size and the context transform have to agree; if one is capped and the other is not the
// board draws oversized and clipped.
let renderDpr = 1;

function resizeCanvas() {
  renderDpr = Math.min(window.devicePixelRatio || 1, DPR_CAP);
  canvas.width = Math.round(window.innerWidth * renderDpr);
  canvas.height = Math.round(window.innerHeight * renderDpr);
  ctx.setTransform(renderDpr, 0, 0, renderDpr, 0, 0);
}

// Lowered from 120/90 now that a late packet extrapolates instead of freezing. This is
// where most of the felt-latency win comes from, and the first knob to raise again if
// motion looks jittery on the slower phones.
let playoutDelay = 80;
const PLAYOUT_MIN = 60,
  PLAYOUT_MAX = 400;
const GAP_SMOOTH_MS = 90;
let renderServerTime = 0;
let lastServerT = 0;
let timelineReady = false;
let lastRealMs = 0;

let serverClockOffset = null;

function syncTrains(arr, serverT) {
  if (trainStates.length > arr.length) trainStates.length = arr.length;
  while (trainStates.length < arr.length) trainStates.push(null);
  if (typeof serverT !== "number") serverT = performance.now();
  for (let i = 0; i < arr.length; i++) {
    const s = arr[i];
    let ts = trainStates[i];
    if (!ts) ts = trainStates[i] = { buf: [], head: null, dir: { dx: 1, dy: 0 }, path: [] };
    // sp is px/second along the link (sim.cpp:519 measures actual progress/dt, so it
    // already reflects a train slowed behind another, and is 0 when stopped/derailed).
    ts.buf.push({ t: serverT, li: s.li, to: s.to, pr: s.pr, sp: s.sp });
    if (ts.buf.length > 12) ts.buf.shift();
    if (ts.path[ts.path.length - 1] !== s.li) {
      ts.path.push(s.li);
      if (ts.path.length > 64) ts.path.shift();
    }
    const prevPax = ts.pax || [];
    const curPax = s.pax || [];
    if (s.st && nodeToStation && lastLayout?.rail?.stations && prevPax.length && curPax.length) {
      let removed = 0;
      const cnt = new Map();
      for (const c of prevPax) if (c != null) cnt.set(c, (cnt.get(c) || 0) + 1);
      for (const c of curPax) if (c != null) cnt.set(c, (cnt.get(c) || 0) - 1);
      for (const n of cnt.values()) if (n > 0) removed += n;
      if (removed > 0) {
        const sid = nodeToStation.get(s.to);
        const st = sid != null ? lastLayout.rail.stations[sid] : null;
        if (st) {
          const isBonus = bonusPhase === 2 && bonusColor != null && st.colorIdx === bonusColor;
          const per = DELIVERY_POINTS * (isBonus ? 2 : 1) * (ts.fu ? 2 : 1);
          const now = performance.now();
          let combo = stationCombos.get(sid);
          if (!combo || combo.until <= now) combo = { amount: 0, until: 0 };
          combo.amount += per * removed;
          combo.until = now + COMBO_LIFE_MS;
          stationCombos.set(sid, combo);
        }
      }
    }
    ts.gap = s.gap;
    ts.pax = curPax;
    ts.col = s.col;
    ts.fu = s.fu;
    ts.st = s.st;
    ts.de = s.de;
    ts.bo = s.bo;
  }
  lastServerT = serverT;
  const offSample = serverT - performance.now();
  serverClockOffset =
    serverClockOffset == null ? offSample : serverClockOffset + 0.05 * (offSample - serverClockOffset);
  if (!timelineReady) {
    renderServerTime = serverT - playoutDelay;
    timelineReady = true;
  }
}

// `pr` is always the distance from links[li][0], in either direction, so the position is
// orientation-free. Only the facing changes, so `to` (the endpoint being approached) is
// carried through and flips dx/dy when the train is running the loop backwards.
function headAt(rail, li, pr, to) {
  const link = rail.links[li];
  if (!link) return null;
  const a = rail.nodes[link[0]],
    b = rail.nodes[link[1]];
  if (!a || !b) return null;
  const dx = b.x - a.x,
    dy = b.y - a.y;
  const seg = Math.hypot(dx, dy) || 1;
  const t = Math.max(0, Math.min(pr / seg, 1));
  const hidden = rail.hiddenSet ? rail.hiddenSet.has(li) : false;
  const fwd = to == null ? true : to === link[1];
  const sx = fwd ? 1 : -1;
  return { x: a.x + dx * t, y: a.y + dy * t, dx: (dx / seg) * sx, dy: (dy / seg) * sx, hidden, seg, li, pr, to };
}

function segLenOf(rail, li) {
  const l = rail.links[li];
  return Math.hypot(rail.nodes[l[1]].x - rail.nodes[l[0]].x, rail.nodes[l[1]].y - rail.nodes[l[0]].y) || 1;
}

function linkDir(rail, li) {
  const l = rail.links[li];
  const a = rail.nodes[l[0]],
    b = rail.nodes[l[1]];
  const dx = b.x - a.x,
    dy = b.y - a.y,
    L = Math.hypot(dx, dy) || 1;
  return { dx: dx / L, dy: dy / L };
}

function clientSegLen(rail, li) {
  if (rail.teleportSet && rail.teleportSet.has(li)) return 0;
  return segLenOf(rail, li);
}

// ---------------------------------------------------------------------------
// Travel geometry. Direction of travel is global (the reverse powerup flips the whole
// board), so the graph walks pick their adjacency table off `dirRev`; the current link's
// orientation comes from the sample's own `to`, which stays correct even mid-flip.
// ---------------------------------------------------------------------------
function travelFwd() {
  return !dirRev;
}

function isFwdOn(rail, li, to) {
  const l = rail.links[li];
  if (!l || to == null) return travelFwd();
  return to === l[1];
}

// Distance still to cover on `li` before reaching `to`.
function remOnLink(rail, li, pr, to) {
  return isFwdOn(rail, li, to) ? Math.max(0, clientSegLen(rail, li) - pr) : Math.max(0, pr);
}

// How far along `li` a train at `pr` sits, measured from the end it entered by.
function entryOffOn(rail, li, pr, to) {
  return isFwdOn(rail, li, to) ? pr : Math.max(0, clientSegLen(rail, li) - pr);
}

// Links leaving `node` in the direction of travel, as {li, to}. Forward travel leaves by
// a link's `a` end; reversed travel leaves by its `b` end -- the same tables, swapped.
function succLinks(rail, node) {
  const fwd = travelFwd();
  const src = fwd ? rail.outLinks : rail.inLinks;
  const ids = (src && src[node]) || [];
  const out = [];
  for (const li of ids) {
    const l = rail.links[li];
    if (l) out.push({ li, to: fwd ? l[1] : l[0] });
  }
  return out;
}

// Links arriving AT `node` in the direction of travel -- the mirror of succLinks, used to
// walk back down the train from its head.
function predLinks(rail, node) {
  const fwd = travelFwd();
  const src = fwd ? rail.inLinks : rail.outLinks;
  const ids = (src && src[node]) || [];
  const out = [];
  for (const li of ids) {
    const l = rail.links[li];
    if (l) out.push({ li, to: fwd ? l[1] : l[0] });
  }
  return out;
}

function travelDir(rail, li, to) {
  const d = linkDir(rail, li);
  return isFwdOn(rail, li, to) ? d : { dx: -d.dx, dy: -d.dy };
}

// Straightest of several candidates relative to `cur`.
function straightest(rail, cands, cur) {
  let best = cands[0],
    bestDot = -Infinity;
  for (const c of cands) {
    const d = travelDir(rail, c.li, c.to);
    const dot = d.dx * cur.dx + d.dy * cur.dy;
    if (dot > bestDot) {
      bestDot = dot;
      best = c;
    }
  }
  return best;
}

// The node a train on `li` heading for `to` entered by.
function entryNodeOf(rail, li, to) {
  const l = rail.links[li];
  if (!l) return to;
  return to === l[1] ? l[0] : l[1];
}

// Walks `dist` back down the train from its head to place a car. Behind means toward the
// end the train entered by, which reverses with the direction of travel.
function pointBehind(rail, ts, dist) {
  const head = ts.head;
  if (!head) return { x: 0, y: 0, hidden: 1 };
  const path = ts.path || [];
  let idx = -1;
  for (let i = path.length - 1; i >= 0; i--)
    if (path[i] === head.li) {
      idx = i;
      break;
    }
  let curLi = head.li;
  let curTo = head.to;
  let fwd = isFwdOn(rail, curLi, curTo);
  // Offset-from-a of the point that is zero distance behind the head on this link.
  let origin = head.pr;
  let avail = fwd ? origin : clientSegLen(rail, curLi) - origin;
  let onPath = idx >= 0;
  let guard = 0;
  while (dist > avail && guard++ < 64) {
    dist -= avail;
    let nextLi = -1;
    if (onPath && idx > 0) {
      idx--;
      nextLi = path[idx];
    } else {
      onPath = false;
      // Step to whichever link feeds into the end we entered this one by.
      const node = entryNodeOf(rail, curLi, curTo);
      const preds = predLinks(rail, node).filter((c) => c.li !== curLi);
      if (preds.length === 0) return headAt(rail, curLi, fwd ? 0 : clientSegLen(rail, curLi), curTo);
      const cur = travelDir(rail, curLi, curTo);
      nextLi = (preds.length === 1 ? preds[0] : straightest(rail, preds, cur)).li;
    }
    curLi = nextLi;
    const l = rail.links[curLi];
    // On the link we came from, travel ran in the global direction.
    curTo = l ? (travelFwd() ? l[1] : l[0]) : curTo;
    fwd = isFwdOn(rail, curLi, curTo);
    const seg = clientSegLen(rail, curLi);
    // Zero-behind now sits at that link's exit end.
    origin = fwd ? seg : 0;
    avail = seg;
  }
  return headAt(rail, curLi, fwd ? origin - dist : origin + dist, curTo);
}

// Shortest run of links from `fromNode` to `targetLi` following the direction of travel.
function forwardChain(rail, fromNode, targetLi, maxDepth) {
  const tl = rail.links[targetLi];
  if (!tl) return null;
  const targetStart = entryNodeOf(rail, targetLi, travelFwd() ? tl[1] : tl[0]);
  if (fromNode === targetStart) return [targetLi];
  const seen = new Set([fromNode]);
  const q = [{ node: fromNode, path: [] }];
  for (let h = 0; h < q.length; h++) {
    const cur = q[h];
    if (cur.path.length >= maxDepth) continue;
    for (const c of succLinks(rail, cur.node)) {
      const nb = c.to;
      const np = cur.path.concat(c.li);
      if (nb === targetStart) return np.concat(targetLi);
      if (c.li === targetLi) return np;
      if (seen.has(nb)) continue;
      seen.add(nb);
      q.push({ node: nb, path: np });
    }
  }
  return null;
}

// Interpolates between two consecutive snapshots. Distances are measured from each link's
// entry end so the same arithmetic covers both directions.
function interpHead(rail, A, B, f) {
  if (A.li === B.li) return headAt(rail, A.li, A.pr + (B.pr - A.pr) * f, B.to);
  if (B.chain === undefined || B.chainFrom !== A.li) {
    const chain = forwardChain(rail, A.to, B.li, 12);
    B.chain = chain ? [A.li].concat(chain) : null;
    B.chainFrom = A.li;
  }
  const chain = B.chain;
  // Offset-from-a for a point `d` into link `li`, measured from its entry end.
  const at = (li, d) => {
    const l = rail.links[li];
    const fwd = travelFwd();
    const to = l ? (fwd ? l[1] : l[0]) : null;
    return headAt(rail, li, fwd ? d : clientSegLen(rail, li) - d, to);
  };
  if (chain) {
    const rem0 = remOnLink(rail, A.li, A.pr, A.to);
    let total = rem0;
    for (let k = 1; k < chain.length - 1; k++) total += clientSegLen(rail, chain[k]);
    const intoB = entryOffOn(rail, B.li, B.pr, B.to);
    total += intoB;
    let d = f * total;
    if (d <= rem0) {
      const fwdA = isFwdOn(rail, A.li, A.to);
      return headAt(rail, A.li, fwdA ? A.pr + d : A.pr - d, A.to);
    }
    d -= rem0;
    for (let k = 1; k < chain.length - 1; k++) {
      const L = clientSegLen(rail, chain[k]);
      if (d <= L) return at(chain[k], d);
      d -= L;
    }
    return at(B.li, Math.min(d, intoB));
  }
  // No chain found: treat B as directly adjacent if its entry node is where A was headed.
  const entryB = entryNodeOf(rail, B.li, B.to);
  if (entryB === A.to) {
    const remA = remOnLink(rail, A.li, A.pr, A.to);
    const intoB = entryOffOn(rail, B.li, B.pr, B.to);
    const d = f * (remA + intoB);
    if (d <= remA) {
      const fwdA = isFwdOn(rail, A.li, A.to);
      return headAt(rail, A.li, fwdA ? A.pr + d : A.pr - d, A.to);
    }
    return at(B.li, d - remA);
  }
  return headAt(rail, B.li, B.pr, B.to);
}

// Dead reckoning past the newest sample, so a late or lost snapshot becomes a small
// position error corrected on the next packet instead of a freeze-then-snap.
// EXTRAP_MAX_MS bounds time; EXTRAP_MAX_DIST bounds distance, which is the one that
// matters — peak speed is ~600 px/s (trainSpeed 200 max x CFG_BOOST_MULT 3), so 250 ms
// of time alone would be ~150 px, far enough to cross a link and guess a wrong branch.
// Measured server stalls are 91-125 ms, so 40 px covers them at normal speed and
// deliberately under-runs a boosted train rather than risk a visible wrong turn.
// Sized to cover ONE entirely missed snapshot. Snapshots arrive every ~198 ms, so a
// single loss means the next one lands 396 ms after the previous; with playoutDelay at
// 80 ms the renderer needs ~316 ms of reckoning to bridge it. The original 250 ms / 40 px
// covered only part of that, so a lost snapshot still froze for ~100-250 ms — which is
// exactly what one phone losing a packet every ~10 s looked like in the 22:33 session.
// 90 px is 450 ms at the 200 px/s upgraded speed; a boosted train (600 px/s) still
// under-runs rather than risking a longer wrong-branch guess.
const EXTRAP_MAX_MS = 450;
const EXTRAP_MAX_DIST = 90;

// Straightest continuation. Only consulted when extrapolation runs off the end of a
// link, which EXTRAP_MAX_DIST makes uncommon.
// Straightest continuation past the end of a link. Only consulted when extrapolation runs
// off the end of one, which EXTRAP_MAX_DIST makes uncommon.
function nextLinkFrom(rail, li, to) {
  const link = rail.links[li];
  if (!link) return null;
  const node = to == null ? (travelFwd() ? link[1] : link[0]) : to;
  const outs = succLinks(rail, node).filter((c) => c.li !== li);
  if (outs.length === 0) return null;
  if (outs.length === 1) return outs[0];
  return straightest(rail, outs, travelDir(rail, li, to));
}

function extrapolateHead(rail, last, dtMs) {
  if (!last.sp) return headAt(rail, last.li, last.pr, last.to); // stopped, derailed, or pre-sp sample
  let dist = Math.min((last.sp * Math.min(dtMs, EXTRAP_MAX_MS)) / 1000, EXTRAP_MAX_DIST);
  let li = last.li,
    pr = last.pr,
    to = last.to,
    guard = 0;
  // clientSegLen is 0 for teleport links, so each one costs a guard step without
  // consuming distance; the guard also bounds any cycle in the adjacency tables.
  while (guard++ < 8) {
    const fwd = isFwdOn(rail, li, to);
    const rem = remOnLink(rail, li, pr, to);
    if (dist <= rem) return headAt(rail, li, fwd ? pr + dist : pr - dist, to);
    dist -= rem;
    const nx = nextLinkFrom(rail, li, to);
    if (!nx) return headAt(rail, li, fwd ? clientSegLen(rail, li) : 0, to);
    li = nx.li;
    to = nx.to;
    // Enter the new link at whichever end travel brings us in by.
    pr = isFwdOn(rail, li, to) ? 0 : clientSegLen(rail, li);
  }
  return headAt(rail, li, pr, to);
}

function sampleTrain(rail, ts, rt) {
  const buf = ts.buf;
  if (rt <= buf[0].t) return headAt(rail, buf[0].li, buf[0].pr, buf[0].to);
  const last = buf[buf.length - 1];
  // Covers buf.length === 1 too: there buf[0] is last, so a single sample extrapolates
  // instead of pinning the train in place.
  if (rt >= last.t) return extrapolateHead(rail, last, rt - last.t);
  for (let i = 0; i < buf.length - 1; i++) {
    const A = buf[i],
      B = buf[i + 1];
    if (rt >= A.t && rt <= B.t) {
      const f = (rt - A.t) / (B.t - A.t || 1);
      return interpHead(rail, A, B, f);
    }
  }
  return headAt(rail, last.li, last.pr, last.to);
}

function applyNodeDrift(rail, cursorMs) {
  if (!driftReady) return;
  const t = (cursorMs - driftStartMs) / 1000;
  const chaos = Math.min(currentChaos, DRIFT_CHAOS_CAP);
  for (const n of rail.nodes) {
    if (!n.drift) continue;
    const c = chaos * n.drift;
    if (c <= 0) {
      n.x = n.bx;
      n.y = n.by;
      continue;
    }
    const A = c * DRIFT_PX * n.amp;
    const across = A * Math.sin(t * n.fx + n.px);
    const along = A * DRIFT_ALONG * Math.sin(t * n.fy + n.py);
    n.x = n.bx + n.dnx * across - n.dny * along;
    n.y = n.by + n.dny * across + n.dnx * along;
  }
}

function stepTrains(rail) {
  if (!rail || !rail.links || !timelineReady) return;
  const realNow = performance.now();
  const dtReal = lastRealMs ? Math.min(realNow - lastRealMs, 100) : 16;
  lastRealMs = realNow;

  const desired = lastServerT - playoutDelay;
  let error = renderServerTime - desired;
  if (error > 1000 || error < -1000) {
    renderServerTime = desired;
    error = 0;
  }
  const rate = 1 - Math.max(-0.1, Math.min(0.1, error / 200));
  renderServerTime += dtReal * rate;
  // Allow the render clock to run ahead of the newest sample, bounded by how far the
  // extrapolator is willing to reckon. Pinning it at lastServerT (the old behaviour) is
  // exactly what caused freeze-then-snap, and would leave extrapolateHead unreachable.
  const maxAhead = lastServerT + EXTRAP_MAX_MS;
  if (renderServerTime > maxAhead) {
    renderServerTime = maxAhead;
    playoutDelay = Math.min(PLAYOUT_MAX, playoutDelay + dtReal * 0.5);
  } else {
    playoutDelay = Math.max(PLAYOUT_MIN, playoutDelay - dtReal * 0.003);
  }

  applyNodeDrift(rail, renderServerTime);

  for (const ts of trainStates) {
    if (!ts || !ts.buf || ts.buf.length === 0) continue;
    const h = sampleTrain(rail, ts, renderServerTime);
    if (!h) continue;
    ts.head = { x: h.x, y: h.y, li: h.li, pr: h.pr, to: h.to };
    ts.dir = { dx: h.dx, dy: h.dy };
    if (ts.gap != null) {
      if (ts.gapS == null) ts.gapS = ts.gap;
      else ts.gapS += (ts.gap - ts.gapS) * Math.min(1, dtReal / GAP_SMOOTH_MS);
    }
  }
}

// --- baseline diagnostics (LAG-REPORT §1.8, §4) -------------------------
// Reported to the server every 3 s so all phones land in one [clients] log line
// instead of needing an on-screen overlay read off four handsets.
const diag = { frameMax: 0, frames: 0, gaps: 0, lastSeq: null, seqStep: null, sentAt: 0 };

// A gap means a trains snapshot never arrived: either real loss or server-side
// suppression (gLayoutInFlight, the heap bail, the queue limit). All are worth seeing.
function noteSeq(seq) {
  if (typeof seq !== "number") return;
  if (diag.lastSeq != null) {
    const d = seq - diag.lastSeq;
    if (d > 0) {
      if (diag.seqStep == null || d < diag.seqStep) diag.seqStep = d;
      if (d > diag.seqStep) diag.gaps++;
    }
  }
  diag.lastSeq = seq;
}

// Must match DEBUG_CLIENT_STATS in esp32.ino. Off, no `cstat` is sent: that is one inbound
// WebSocket message per phone every 3 s, each costing a deserializeJson on the ESP32's
// priority-10 AsyncTCP task plus uplink airtime, purely to feed the [clients] log line.
// Turn both on together for any session you intend to diagnose (LAG-REPORT §4).
const DEBUG_CLIENT_STATS = false;

function reportDiag(now) {
  if (!DEBUG_CLIENT_STATS) return;
  if (!diag.sentAt) { diag.sentAt = now; return; }
  const dt = now - diag.sentAt;
  if (dt < 3000) return;
  send({
    type: "cstat",
    fmax: Math.round(diag.frameMax),
    fps: Math.round((diag.frames * 1000) / dt),
    gaps: diag.gaps,
  });
  diag.frameMax = 0;
  diag.frames = 0;
  diag.sentAt = now;
}

let rafId = null;
function frame() {
  rafId = requestAnimationFrame(frame);
  if (!lastLayout) return;
  const t0 = performance.now();
  stepTrains(lastLayout.rail);
  draw();
  const ms = performance.now() - t0;
  if (ms > diag.frameMax) diag.frameMax = ms;
  diag.frames++;
  reportDiag(t0);
}
function startRender() {
  if (rafId == null) {
    rafId = requestAnimationFrame(frame);
  }
}
function stopRender() {
  if (rafId != null) {
    cancelAnimationFrame(rafId);
    rafId = null;
  }
}

function draw() {
  if (!lastLayout) return;
  const PW = window.innerWidth;
  const PH = window.innerHeight;
  const { you, edges } = lastLayout;

  const deviceOri = PW >= PH ? "landscape" : "portrait";
  const rotated = you.orientation && deviceOri !== you.orientation;
  const LW = rotated ? PH : PW;
  const LH = rotated ? PW : PH;

  ctx.setTransform(renderDpr, 0, 0, renderDpr, 0, 0);
  // No clearRect: the context is opaque (alpha:false) and the black fillRect below covers
  // the whole logical viewport. Rotated, LW/LH are swapped but the translate+rotate maps
  // them onto the same area, so coverage is identical -- a rotated phone is where an
  // uncleared edge strip would show up if that ever stopped being true.

  ctx.save();
  if (rotated) {
    ctx.translate(PW, 0);
    ctx.rotate(Math.PI / 2);
  }

  ctx.fillStyle = "#000";
  ctx.fillRect(0, 0, LW, LH);

  if (DEBUG) drawBoardGrid(you, LW, LH);

  drawEdges(edges, LW, LH);
  if (!placing) {
    drawTunnelStubs(lastLayout.rail, you);
    drawRail(lastLayout.rail, you);
    drawStations(lastLayout.rail, you);
    drawBoosts(lastLayout.rail, you);
    drawSwitches(lastLayout.rail, you);
    drawJunctions(lastLayout.rail, you);
    drawTrains(you);
    drawArmedTargets(lastLayout.rail, you, LW, LH);
    if (window.DEBUG_HIT_AREAS) drawHitAreas(you);
    drawMenuLabel(edges, LW, LH, you);
  }

  if (DEBUG) {
    ctx.fillStyle = me ? me.color : "#fff";
    ctx.font = 'bold 22px "HostGrotesk", system-ui, sans-serif';
    ctx.textBaseline = "top";
    ctx.textAlign = "left";
    // `me` comes from the lobby roster; `you` no longer carries an id (see sendLayoutTo).
    ctx.fillText(me ? me.id : "?", 14, 14);
  }

  ctx.restore();
}

function drawBoardGrid(you, W, H) {
  const STEP = 50;
  const sx = W / you.w;
  const sy = H / you.h;

  ctx.strokeStyle = "#2b3442";
  ctx.lineWidth = 1;
  ctx.beginPath();
  const startX = Math.ceil(you.x / STEP) * STEP;
  for (let gx = startX; gx <= you.x + you.w; gx += STEP) {
    const px = (gx - you.x) * sx;
    ctx.moveTo(px, 0);
    ctx.lineTo(px, H);
  }
  const startY = Math.ceil(you.y / STEP) * STEP;
  for (let gy = startY; gy <= you.y + you.h; gy += STEP) {
    const py = (gy - you.y) * sy;
    ctx.moveTo(0, py);
    ctx.lineTo(W, py);
  }
  ctx.stroke();
}

function drawEdges(edges, W, H) {
  const BAR = DEBUG ? 22 : placing ? 24 : 9;
  for (const e of edges) {
    ctx.fillStyle = e.color;
    if (e.side === "left" || e.side === "right") {
      const sy = H / lastLayout.you.h;
      const y0 = e.start * sy;
      const y1 = e.end * sy;
      const x = e.side === "left" ? 0 : W - BAR;
      ctx.fillRect(x, y0, BAR, y1 - y0);
    } else {
      const sx = W / lastLayout.you.w;
      const x0 = e.start * sx;
      const x1 = e.end * sx;
      const y = e.side === "top" ? 0 : H - BAR;
      ctx.fillRect(x0, y, x1 - x0, BAR);
    }
  }
}

let menuHit = null;
function drawMenuLabel(edges, LW, LH, you) {
  menuHit = null;
  if (!edges || !edges.length) return;
  const span = (e) => Math.abs((e.end ?? 0) - (e.start ?? 0));
  let edge = edges[0];
  for (const e of edges) if (span(e) > span(edge)) edge = e;

  const INSET = 16;
  const TEXTH = 16;
  const NEAR = INSET + TEXTH;
  let x, y, rot;
  if (edge.side === "left" || edge.side === "right") {
    const sy = LH / you.h;
    y = ((edge.start + edge.end) / 2) * sy;
    if (edge.side === "left") {
      x = NEAR;
      rot = -Math.PI / 2;
    } else {
      x = LW - NEAR;
      rot = Math.PI / 2;
    }
  } else {
    const sx = LW / you.w;
    x = ((edge.start + edge.end) / 2) * sx;
    if (edge.side === "top") {
      y = NEAR;
      rot = 0;
    } else {
      y = LH - NEAR;
      rot = Math.PI;
    }
  }

  ctx.save();
  ctx.translate(x, y);
  ctx.rotate(rot);
  ctx.fillStyle = "rgba(255,255,255,0.72)";
  ctx.font = '700 16px "HostGrotesk", system-ui, sans-serif';
  ctx.textAlign = "center";
  ctx.textBaseline = "alphabetic";
  ctx.fillText("menu", 0, 0);
  ctx.restore();

  menuHit = { x, y, r: 40 };
}

function drawRail(rail, you) {
  if (!rail || !rail.nodes || rail.nodes.length < 2) return;
  const { nodes, links } = rail;
  const hidden = new Set(rail.hidden || []);
  const linkIdx = new Map();
  links.forEach((l, i) => linkIdx.set(l[0] + ":" + l[1], i));
  const cut = new Set();
  for (const j of rail.junctions || []) {
    // The two spur links -- ring node to entry mouth, exit mouth back to the ring -- belong
    // to drawTunnelStubs(), which stops them short of the mouth arcs. Marked skipped here
    // rather than cut: this used to add them to `cut`, which painted them grey EVERY frame
    // whatever the lever said, and left the stub fighting the rail for the same pixels.
    const vi = linkIdx.get(j.node + ":" + j.mIn);
    const vo = linkIdx.get(j.mOut + ":" + j.exit);
    if (vi != null) hidden.add(vi);
    if (vo != null) hidden.add(vo);
    // Diverting closes the straight line instead. That half was always right.
    if (j.active === 1) {
      const st = linkIdx.get(j.node + ":" + j.straightTo);
      if (st != null) cut.add(st);
    }
  }
  for (const sw of rail.switches || []) {
    if (sw.active === 1) {
      for (const li of sw.straightLinks || []) cut.add(li);
    } else if (sw.entryLink != null) {
      cut.add(sw.entryLink);
    }
  }
  ctx.save();
  ctx.lineJoin = "round";
  ctx.lineCap = "round";

  // The branches a switch or junction currently has closed, laid down first and dark. They
  // used to be drawn not at all, which left a player guessing where a lever would send a
  // train; dark grey shows the shape of the route without reading as live rail.
  // rail.hidden is a different thing and stays invisible -- tunnel interiors, not disabled
  // track; a train inside one is meant to have vanished.
  ctx.beginPath();
  for (let i = 0; i < links.length; i++) {
    if (!cut.has(i) || hidden.has(i)) continue;
    const na = nodes[links[i][0]],
      nb = nodes[links[i][1]];
    ctx.moveTo(na.x - you.x, na.y - you.y);
    ctx.lineTo(nb.x - you.x, nb.y - you.y);
  }
  ctx.lineWidth = 2;
  ctx.strokeStyle = RAIL_CUT;
  ctx.stroke();

  ctx.beginPath();
  for (let i = 0; i < links.length; i++) {
    if (hidden.has(i) || cut.has(i)) continue;
    const na = nodes[links[i][0]],
      nb = nodes[links[i][1]];
    ctx.moveTo(na.x - you.x, na.y - you.y);
    ctx.lineTo(nb.x - you.x, nb.y - you.y);
  }

  ctx.lineWidth = 2;
  ctx.strokeStyle = RAIL_LIVE;
  ctx.stroke();

  ctx.restore();
}

// A powerup waiting for a target pulses its candidates on EVERY phone, not just the one
// that bought it -- the target may well be on someone else's screen, and they need to see
// where to tap without being told.
// While a powerup waits for a target the whole game is held still, so the board is dimmed
// down to just the things worth tapping: stations for a freeze, the track for an extra
// train. Everything is dimmed with one flat fill and the targets are then redrawn over it
// at full strength -- punching holes with compositing would erase the rail underneath too.
// Every thing on the board a press can reach, in board coordinates. The hit test reads
// this, and so does the DEBUG_HIT_AREAS overlay -- which is the point: an overlay drawn
// from its own copy of the geometry would eventually disagree with the thing it claims
// to show.
//
// Two shapes live here. A tap target is a circle of radius `hit` and carries the `msg` its
// tap sends. A boost pad instead carries `swipe`, a lane: a capsule reaching `half` along
// the rail and `hit` across it, claimed by a drag rather than a tap. The tap hit test skips
// lanes entirely, which is what lets a pad and a station gate share the same patch of board.
function tapTargets() {
  const rail = lastLayout && lastLayout.rail;
  if (!rail) return [];
  const { nodes, stations, junctions, boosts, switches } = rail;
  const out = [];
  for (const j of junctions || [])
    out.push({ at: junctionLever(rail, j), hit: JUNCTION_HIT, kind: "junction",
               msg: { type: "junctionToggle", junction: j.id } });
  for (const sw of switches || [])
    out.push({ at: junctionLever(rail, sw), hit: JUNCTION_HIT, kind: "switch",
               msg: { type: "switchToggle", switch: sw.id } });
  for (const b of boosts || []) {
    if (!nodes[b.node]) continue;
    const [ux, uy] = boostAxis(rail, b.node);
    out.push({ at: nodes[b.node], hit: BOOST_LANE_W, half: BOOST_LANE_L, kind: "boost",
               swipe: true, boost: b, ux, uy });
  }
  for (const st of stations || [])
    out.push({ at: stationGate(nodes, st), hit: GATE_HIT, kind: "gate",
               msg: { type: "stationToggle", station: st.id } });
  return out.filter((t) => t.at);
}

// Travel direction over a boost pad, in board space. The reverse powerup flips travel for
// the whole board (see dirRev), so this is read fresh every frame and every pointer move
// rather than cached with the layout -- a swipe has to run the way the trains now run.
function boostAxis(rail, node) {
  const [tx, ty] = tangentAt(rail, node);
  return dirRev ? [-tx, -ty] : [tx, ty];
}

// The boost lane under a board point, nearest first, or null. Signed offsets come back with
// it: `along` runs with the traffic, which is the direction a swipe has to go to arm.
function boostLaneAt(p) {
  let best = null,
    bestD = Infinity;
  for (const t of tapTargets()) {
    if (!t.swipe) continue;
    const dx = p.x - t.at.x,
      dy = p.y - t.at.y;
    const along = dx * t.ux + dy * t.uy;
    const across = dx * -t.uy + dy * t.ux;
    if (Math.abs(along) > t.half || Math.abs(across) > t.hit) continue;
    const d = Math.hypot(along, across);
    if (d < bestD) {
      bestD = d;
      best = t;
    }
  }
  return best;
}

// Design aid, off by default: outlines every region the hit test accepts, so you can see
// where a target actually is rather than where its icon is.
//
// Turn it on with ?hit=1 in the URL -- that works on a phone, which is where this actually
// needs looking at, and survives a reload. `window.DEBUG_HIT_AREAS = true` from a console
// also works; plain `DEBUG_HIT_AREAS = true` does NOT reliably, because a top-level `let` in
// a classic script is a lexical binding rather than a property of window.
//
// Overlapping rings are worth looking for: the tap hit test picks the NEAREST target, so two
// levers whose circles overlap will quietly steal taps from each other. A boost lane is
// drawn as a capsule with a nick at the end a swipe has to reach, and is allowed to overlap
// anything -- it answers to drags, which nothing else on the board does.
window.DEBUG_HIT_AREAS =
  new URLSearchParams(location.search).get("hit") === "1";
const HIT_COLORS = { junction: "#ffd166", switch: "#06d6a0", boost: "#6fd2ff", gate: "#ef476f" };

function drawHitAreas(you) {
  ctx.save();
  ctx.lineWidth = 1;
  for (const t of tapTargets()) {
    const x = t.at.x - you.x,
      y = t.at.y - you.y;
    ctx.strokeStyle = HIT_COLORS[t.kind] || "#ffffff";
    ctx.beginPath();
    if (t.swipe) {
      ctx.save();
      ctx.translate(x, y);
      ctx.rotate(Math.atan2(t.uy, t.ux));
      ctx.roundRect(-t.half, -t.hit, t.half * 2, t.hit * 2, t.hit);
      ctx.moveTo(BOOST_SWIPE_DIST, -t.hit);
      ctx.lineTo(BOOST_SWIPE_DIST, t.hit);
      ctx.stroke();
      ctx.restore();
    } else {
      ctx.arc(x, y, t.hit, 0, Math.PI * 2);
      ctx.stroke();
    }
    ctx.beginPath();
    ctx.arc(x, y, 2, 0, Math.PI * 2);
    ctx.fillStyle = ctx.strokeStyle;
    ctx.fill();
  }
  ctx.restore();
}

function drawArmedTargets(rail, you, LW, LH) {
  const arming = armedPowerup();
  if (!arming || !rail) return;

  ctx.save();
  ctx.fillStyle = "rgba(0,0,0,0.66)";
  ctx.fillRect(0, 0, LW, LH);
  ctx.restore();

  if (arming.target === "station") {
    drawStations(rail, you);
  } else {
    drawRail(rail, you);
  }

  const pulse = 0.45 + 0.35 * Math.sin(performance.now() / 260);
  ctx.save();
  ctx.strokeStyle = `rgba(122, 200, 255, ${pulse.toFixed(3)})`;
  ctx.setLineDash([7, 6]);
  if (arming.target === "station") {
    ctx.lineWidth = 3;
    for (const st of rail.stations || []) {
      if (frozenStations.has(st.id)) continue;
      const g = stationGate(rail.nodes, st);
      if (!g) continue;
      ctx.beginPath();
      ctx.arc(g.x - you.x, g.y - you.y, GATE_HIT * 0.8, 0, Math.PI * 2);
      ctx.stroke();
    }
  } else {
    // Any main-ring node will do, so trace the ring itself rather than ringing each node.
    const ringN = rail.N ?? 0;
    ctx.lineWidth = 6;
    ctx.beginPath();
    for (let i = 0; i < ringN && i < (rail.links?.length || 0); i++) {
      if (rail.hiddenSet?.has(i)) continue;
      const na = rail.nodes[rail.links[i][0]],
        nb = rail.nodes[rail.links[i][1]];
      if (!na || !nb) continue;
      ctx.moveTo(na.x - you.x, na.y - you.y);
      ctx.lineTo(nb.x - you.x, nb.y - you.y);
    }
    ctx.stroke();
  }
  ctx.restore();
}

// The tappable gate, and everything that rings or targets it, sit at the platform end
// trains berth against -- st.n1 forward, st.n0 reversed. Mirrors isBerthNode in the sim.
function stationGate(nodes, st) {
  return nodes[dirRev ? st.n0 : st.n1];
}

function drawStations(rail, you) {
  if (!rail || !rail.stations) return;
  for (const st of rail.stations) {
    const a = rail.nodes[st.n0],
      b = rail.nodes[st.n1];
    if (!a || !b) continue;
    const ax = a.x - you.x,
      ay = a.y - you.y;
    const bx = b.x - you.x,
      by = b.y - you.y;
    const off1 = STATION_PLAT,
      off2 = STATION_PLAT + STATION_DEPTH;
    const i1x = st.nx * off1,
      i1y = st.ny * off1;
    const i2x = st.nx * off2,
      i2y = st.ny * off2;

    const frozen = frozenStations.has(st.id);
    const overflowing = (st.queue?.length || 0) >= maxQueue;
    const platHidden = overflowing && !frozen && Math.floor(performance.now() / 160) & 1;

    ctx.save();
    const stColor = colorOf(st.colorIdx);
    if (!platHidden) {
      ctx.beginPath();
      ctx.moveTo(ax + i1x, ay + i1y);
      ctx.lineTo(bx + i1x, by + i1y);
      ctx.lineTo(bx + i2x, by + i2y);
      ctx.lineTo(ax + i2x, ay + i2y);
      ctx.closePath();
      ctx.fillStyle = stColor || "#cdd6e2";
      ctx.strokeStyle = "#11151c";
      ctx.lineWidth = 2;
      ctx.fill();
      ctx.stroke();
      if (frozen) {
        ctx.fillStyle = "rgba(150, 220, 255, 0.55)";
        ctx.fill();
        ctx.strokeStyle = "#bfe9ff";
        ctx.lineWidth = 2.5;
        ctx.stroke();
      }
    }

    const ul = Math.hypot(bx - ax, by - ay) || 1;
    const ux = (bx - ax) / ul,
      uy = (by - ay) / ul;

    // The gate stands at the end trains reach last, which swaps with the board's direction
    // (the sim berths them by the same rule). The queue always trails back down the
    // platform from the gate, so its direction swaps with it.
    const gx = dirRev ? ax : bx,
      gy = dirRev ? ay : by;
    const qux = dirRev ? ux : -ux,
      quy = dirRev ? uy : -uy;

    const gdx = st.open ? ux : st.nx,
      gdy = st.open ? uy : st.ny;
    const g1x = gx - gdx * GATE_HALF,
      g1y = gy - gdy * GATE_HALF;
    const g2x = gx + gdx * GATE_HALF,
      g2y = gy + gdy * GATE_HALF;
    ctx.beginPath();
    ctx.moveTo(g1x, g1y);
    ctx.lineTo(g2x, g2y);
    ctx.strokeStyle = "#11151c";
    ctx.lineWidth = 6;
    ctx.lineCap = "round";
    ctx.stroke();
    ctx.strokeStyle = "#f4f7fb";
    ctx.lineWidth = 3;
    ctx.stroke();

    const qoff = STATION_OFFSET + QUEUE_R;
    const q = st.queue || [];
    const spawnT = st.spawnT;
    const nowMs = performance.now();
    for (let k = 0; k < q.length; k++) {
      const along = QUEUE_START + QUEUE_R + k * (2 * QUEUE_R + QUEUE_GAP);
      let qx = gx + qux * along + st.nx * qoff;
      let qy = gy + quy * along + st.ny * qoff;
      const t0 = spawnT?.[k];
      if (t0 != null) {
        const p = (nowMs - t0) / 300;
        if (p >= 0 && p < 1) {
          const e = (1 - p) * (1 - p);
          qx += qux * 8 * e;
          qy += quy * 8 * e;
        }
      }
      ctx.beginPath();
      ctx.arc(qx, qy, QUEUE_R, 0, Math.PI * 2);
      ctx.fillStyle = colorOf(q[k]) || "#cdd6e2";
      ctx.fill();
      ctx.strokeStyle = "#11151c";
      ctx.lineWidth = 1.5;
      ctx.stroke();
    }

    if (bonusPhase && bonusColor != null && st.colorIdx === bonusColor) {
      const m = 5;
      const o1 = off1 - m,
        o2 = off2 + m;
      const pts = [
        [ax + st.nx * o1 - ux * m, ay + st.ny * o1 - uy * m],
        [bx + st.nx * o1 + ux * m, by + st.ny * o1 + uy * m],
        [bx + st.nx * o2 + ux * m, by + st.ny * o2 + uy * m],
        [ax + st.nx * o2 - ux * m, ay + st.ny * o2 - uy * m],
      ];
      let perim = 0;
      for (let i = 0; i < 4; i++) {
        const j = (i + 1) % 4;
        perim += Math.hypot(pts[j][0] - pts[i][0], pts[j][1] - pts[i][1]);
      }
      ctx.save();
      ctx.beginPath();
      ctx.moveTo(pts[0][0], pts[0][1]);
      for (let i = 1; i < 4; i++) ctx.lineTo(pts[i][0], pts[i][1]);
      ctx.closePath();
      ctx.strokeStyle = stColor || "#cdd6e2";
      ctx.lineWidth = 2;
      if (bonusPhase === 1) {
        const drawn = perim * Math.max(0.001, Math.min(1, bonusT));
        ctx.setLineDash([drawn, perim]);
      } else {
        const nowMs = performance.now();
        if (nowMs !== bonusDashLast) {
          const dt = Math.min(100, nowMs - bonusDashLast);
          bonusDashLast = nowMs;
          const left = bonusLeftMs - (nowMs - bonusLeftStamp);
          const slow = 0.05 + 0.95 * Math.max(0, Math.min(1, left / BONUS_SLOWDOWN_MS));
          bonusDashPhase += dt * 0.04 * slow;
        }
        ctx.setLineDash([7, 5]);
        ctx.lineDashOffset = frozen ? 0 : -(bonusDashPhase % 12);
      }
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.restore();
    }

    const combo = stationCombos.get(st.id);
    if (combo && combo.until > performance.now()) {
      const mx = (ax + bx) / 2,
        my = (ay + by) / 2;
      const off = STATION_PLAT + STATION_DEPTH + 8;
      const tx = mx + st.nx * off,
        ty = my + st.ny * off;
      ctx.save();
      ctx.translate(tx, ty);
      ctx.rotate(Math.atan2(st.nx, -st.ny));
      ctx.font = 'bold 13px "HostGrotesk", system-ui, sans-serif';
      ctx.textAlign = "center";
      ctx.textBaseline = "middle";
      const text = "+" + combo.amount;
      ctx.lineWidth = 2.5;
      ctx.strokeStyle = "#11151c";
      ctx.strokeText(text, 0, 0);
      ctx.fillStyle = stColor || "#ffffff";
      ctx.fillText(text, 0, 0);
      ctx.restore();
    }
    ctx.restore();
  }
}

function tangentAt(rail, idx) {
  const links = rail.links || [];
  let prev = -1,
    next = -1;
  for (const l of links) {
    if (l[0] === idx && next < 0)
      next = l[1];
    else if (l[1] === idx && prev < 0) prev = l[0];
  }
  if (prev < 0) prev = idx;
  if (next < 0) next = idx;
  const p = rail.nodes[prev],
    n = rail.nodes[next];
  let dx = n.x - p.x,
    dy = n.y - p.y;
  const L = Math.hypot(dx, dy) || 1;
  return [dx / L, dy / L];
}

function drawMouth(mx, my, fx, fy, rim, glow) {
  ctx.save();
  ctx.translate(mx, my);
  ctx.rotate(Math.atan2(fy, fx));
  ctx.beginPath();
  ctx.ellipse(0, 0, MOUTH_DEPTH, MOUTH_W, 0, -MOUTH_ARC, MOUTH_ARC);
  ctx.lineWidth = glow ? 4 : 3;
  ctx.strokeStyle = rim;
  ctx.stroke();
  ctx.restore();
}

// The short run between a junction/switch node and its tunnel mouth. `colour` is the branch
// it belongs to: drawRail() already paints this approach RAIL_CUT whenever the lever is not
// sending trains down it, and a hardcoded white stub on top left the same branch grey on the
// main line and white going into the arc.
function junctionStub(ax, ay, bx, by, startGap, colour) {
  const L = Math.hypot(bx - ax, by - ay) || 1;
  const t = Math.min(startGap / L, 0.6);
  const sx = ax + (bx - ax) * t,
    sy = ay + (by - ay) * t;
  ctx.lineCap = "round";
  ctx.beginPath();
  ctx.moveTo(sx, sy);
  ctx.lineTo(bx, by);
  ctx.lineWidth = 2;
  ctx.strokeStyle = colour || RAIL_LIVE;
  ctx.stroke();
}

// A reversed train diverts at the special's FAR end (the server reads jctOut/swtOut there),
// so the lever has to stand at that end too -- otherwise you are throwing a lever the
// trains have already passed. `exit`/`outDir` are the far-end mirror of `node`/`inDir`, and
// the approach tangent comes from the ring node beyond the exit instead of before the entry.
function junctionLever(rail, j) {
  const nodes = rail.nodes,
    N = rail.N ?? nodes.length;
  const at = dirRev ? j.exit : j.node;
  const J = nodes[at],
    prev = nodes[dirRev ? (at + 1) % N : (at - 1 + N) % N];
  const dir = dirRev ? j.outDir : j.inDir;
  if (!J || !prev || !dir) return null;
  let tx = J.x - prev.x,
    ty = J.y - prev.y;
  const tl = Math.hypot(tx, ty) || 1;
  tx /= tl;
  ty /= tl;
  let nx = -ty,
    ny = tx;
  const side = nx * dir[0] + ny * dir[1] >= 0 ? 1 : -1;
  nx *= side;
  ny *= side;
  return { x: J.x - tx * LEVER_BACK + nx * LEVER_OFF, y: J.y - ty * LEVER_BACK + ny * LEVER_OFF, tx, ty, nx, ny };
}

// Both tunnel spurs: ring node -> entry mouth, and exit mouth -> back to the ring. drawRail()
// skips these two links entirely, so this is the only thing that draws them and the colour
// here is the whole truth -- white end to end when the junction is diverting, grey when it
// is not. Drawn before the rail so a spur crossing the loop passes under it instead of
// notching it.
function drawTunnelStubs(rail, you) {
  if (!rail || !rail.junctions) return;
  ctx.save();
  ctx.lineCap = "round";
  for (const j of rail.junctions) {
    const J = rail.nodes[j.node],
      mIn = rail.nodes[j.mIn],
      mOut = rail.nodes[j.mOut],
      E = rail.nodes[j.exit];
    if (!J) continue;
    const Jx = J.x - you.x,
      Jy = J.y - you.y;
    // Only a diverting junction actually feeds the tunnel; otherwise this whole approach is
    // closed rail and has to read as closed rail.
    const colour = j.active === 1 ? RAIL_LIVE : RAIL_CUT;
    if (mIn) {
      const cx = mIn.x - you.x + j.inDir[0] * MOUTH_DEPTH * 0.85;
      const cy = mIn.y - you.y + j.inDir[1] * MOUTH_DEPTH * 0.85;
      const midX = (Jx + cx) / 2,
        midY = (Jy + cy) / 2;
      junctionStub(midX, midY, cx, cy, 0, colour);
      junctionStub(Jx, Jy, midX, midY, 0, colour);
    }
    if (mOut && E) {
      const ox = mOut.x - you.x,
        oy = mOut.y - you.y;
      junctionStub(
        ox + j.outDir[0] * MOUTH_DEPTH * 0.85,
        oy + j.outDir[1] * MOUTH_DEPTH * 0.85,
        E.x - you.x,
        E.y - you.y,
        0,
        colour,
      );
    }
  }
  ctx.restore();
}

function drawJunctions(rail, you) {
  if (!rail || !rail.junctions) return;
  ctx.save();
  ctx.lineCap = "round";
  const flashOn = Date.now() % 560 < 280;

  for (const j of rail.junctions) {
    const J = rail.nodes[j.node],
      mIn = rail.nodes[j.mIn],
      mOut = rail.nodes[j.mOut];
    const E = rail.nodes[j.exit];
    if (!J) continue;
    const Jx = J.x - you.x,
      Jy = J.y - you.y;
    const dive = j.active === 1;

    if (mIn) drawMouth(mIn.x - you.x, mIn.y - you.y, j.inDir[0], j.inDir[1], TUNNEL_RIM, false);
    if (mOut) {
      const flashing = j.incoming && flashOn;
      drawMouth(
        mOut.x - you.x,
        mOut.y - you.y,
        j.outDir[0],
        j.outDir[1],
        flashing ? "#ffffff" : TUNNEL_RIM,
        j.incoming,
      );
    }

    const lv = junctionLever(rail, j);
    if (lv) {
      const px = lv.x - you.x,
        py = lv.y - you.y;
      const f = dive ? -1 : 1;
      let ax = f * lv.tx + lv.nx,
        ay = f * lv.ty + lv.ny;
      const al = Math.hypot(ax, ay) || 1;
      ax /= al;
      ay /= al;
      const hx = px + ax * LEVER_ARM,
        hy = py + ay * LEVER_ARM;
      ctx.lineCap = "round";
      ctx.strokeStyle = LEVER_COLOR;
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.moveTo(px, py);
      ctx.lineTo(hx, hy);
      ctx.stroke();
      ctx.beginPath();
      ctx.arc(hx, hy, 3.5, 0, Math.PI * 2);
      ctx.fillStyle = LEVER_COLOR;
      ctx.fill();
    }
  }
  ctx.restore();
}

function drawSwitches(rail, you) {
  if (!rail || !rail.switches) return;
  ctx.save();
  ctx.lineCap = "round";
  for (const sw of rail.switches) {
    const lv = junctionLever(rail, sw);
    if (!lv) continue;
    const px = lv.x - you.x,
      py = lv.y - you.y;
    const f = sw.active === 1 ? -1 : 1;
    let ax = f * lv.tx + lv.nx,
      ay = f * lv.ty + lv.ny;
    const al = Math.hypot(ax, ay) || 1;
    ax /= al;
    ay /= al;
    const hx = px + ax * LEVER_ARM,
      hy = py + ay * LEVER_ARM;
    ctx.strokeStyle = LEVER_COLOR;
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(px, py);
    ctx.lineTo(hx, hy);
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(hx, hy, 3.5, 0, Math.PI * 2);
    ctx.fillStyle = LEVER_COLOR;
    ctx.fill();
  }
  ctx.restore();
}

// What a pad should be drawn as, which is not always what the server last said: for the
// round trip after a swipe commits, the optimistic wish wins so the run does not flick back
// to idle under the finger that just armed it. See boostWish.
function boostArmedShown(b) {
  const w = boostWish.get(b.id);
  if (!w) return !!b.armed;
  if (!!b.armed === w.armed || performance.now() > w.until) {
    boostWish.delete(b.id);
    return !!b.armed;
  }
  return w.armed;
}

function drawBoosts(rail, you) {
  if (!rail || !rail.boosts) return;
  ctx.save();
  ctx.lineCap = "round";
  ctx.lineJoin = "round";
  for (const b of rail.boosts) {
    const P = rail.nodes[b.node];
    if (!P) continue;
    const cx = P.x - you.x,
      cy = P.y - you.y;
    // The arrowheads point the way a train runs over the pad -- which is also the way a
    // finger has to swipe to arm it -- so they turn around with the board: the reverse
    // powerup flips travel globally, and a run still aiming the old way would be asking
    // for a swipe against the traffic.
    const [ux, uy] = boostAxis(rail, b.node);
    const px = -uy,
      py = ux;
    let onCut = false;
    for (const sw of rail.switches || []) {
      if (sw.active !== 1) continue;
      for (const li of sw.straightLinks || []) {
        if (b.node === li) {
          onCut = true;
          break;
        }
      }
      if (onCut) break;
    }
    const armed = boostArmedShown(b);
    // Colour and opacity carry different things on purpose. Colour is per triangle and says
    // how far the swipe has got: blue for the ones the finger has already passed, idle for
    // the rest. Opacity is for the whole run and says whether the pad is actually live. So
    // the triangles recolour one at a time as you drag, and then the whole mark brightens
    // in one step at the moment the swipe commits -- two signals that cannot be confused.
    //   idle, live rail  white
    //   idle, cut rail   grey
    //   swept / armed    light blue
    const idleCol = onCut ? "#6b7079" : "#ffffff";
    // Signed 0..1 progress of a swipe in flight on THIS pad: + runs with the traffic and
    // arms, - runs against it and disarms.
    const prog = boostSwipe && boostSwipe.id === b.id ? boostSwipe.prog : 0;
    // Triangles light from the end the finger started at, so the run fills the way the hand
    // is moving. The first lights on any real movement and the last at BOOST_SWIPE_DONE,
    // which is exactly where the swipe takes -- a full run means it worked.
    const nLit = prog === 0
      ? 0
      : Math.min(BOOST_TRI_N, Math.floor(Math.abs(prog) * BOOST_TRI_N) + 1);
    ctx.globalAlpha = armed ? 1 : BOOST_ALPHA_IDLE;
    if (b.cd > 0) ctx.globalAlpha *= BOOST_ALPHA_CD;
    for (let i = 0; i < BOOST_TRI_N; i++) {
      // Swiping forward paints idle triangles live from the back; swiping back off an armed
      // pad unpaints them from the front. Either way the swept ones are the ones changing.
      const swept = prog > 0 ? i < nLit : i >= BOOST_TRI_N - nLit;
      const live = prog < 0 ? armed && !swept : armed || swept;
      ctx.fillStyle = live ? BOOST_ACTIVE : idleCol;
      const base = (i - (BOOST_TRI_N - 1) / 2) * BOOST_TRI_STEP + BOOST_TRI_SHIFT;
      const tip = base + BOOST_TRI_H;
      ctx.beginPath();
      ctx.moveTo(cx + ux * base - px * BOOST_TRI_HALF, cy + uy * base - py * BOOST_TRI_HALF);
      ctx.lineTo(cx + ux * tip, cy + uy * tip);
      ctx.lineTo(cx + ux * base + px * BOOST_TRI_HALF, cy + uy * base + py * BOOST_TRI_HALF);
      ctx.closePath();
      ctx.fill();
    }
  }
  ctx.restore();
}

function lerpHex(a, b, t) {
  const pa = parseInt(a.slice(1), 16),
    pb = parseInt(b.slice(1), 16);
  const ar = (pa >> 16) & 255,
    ag = (pa >> 8) & 255,
    ab = pa & 255;
  const br = (pb >> 16) & 255,
    bg = (pb >> 8) & 255,
    bb = pb & 255;
  const m = Math.max(0, Math.min(1, t));
  return `rgb(${Math.round(ar + (br - ar) * m)},${Math.round(ag + (bg - ag) * m)},${Math.round(ab + (bb - ab) * m)})`;
}

function drawTrains(you) {
  const rail = lastLayout?.rail;
  if (!rail) return;
  const carR = rail.carR || 11;
  for (const ts of trainStates) {
    if (!ts || !ts.head) continue;
    const pass = ts.pax || [];
    const nCars = pass.length;
    const cars = [];
    for (let k = 0; k < nCars; k++) {
      const p = pointBehind(rail, ts, k * (ts.gapS ?? ts.gap));
      cars.push([p.x, p.y, p.hidden ? 1 : 0]);
    }
    const dx = ts.dir.dx,
      dy = ts.dir.dy;
    const px = -dy,
      py = dx;
    let locoFill = colorOf(ts.col) || "#ffffff";
    let carColor = ts.fu ? locoFill : "#ffffff";
    if (ts.bo > 0 && !ts.de) {
      const boostCol = lerpHex("#6fd2ff", "#ffffff", 1 - ts.bo);
      carColor = boostCol;
      locoFill = boostCol;
    }
    if (ts.de && Math.floor(performance.now() / 160) & 1) continue;

    for (let k = cars.length - 1; k >= 0; k--) {
      if (cars[k][2]) continue;
      const cx = cars[k][0] - you.x,
        cy = cars[k][1] - you.y;
      ctx.save();
      ctx.strokeStyle = carColor;
      ctx.lineWidth = 2.5;
      ctx.lineJoin = "round";

      if (k === 0) {
        const tipx = cx + dx * carR * 1.6,
          tipy = cy + dy * carR * 1.6;
        const r1x = cx - dx * carR * 0.9 + px * carR * 1.15,
          r1y = cy - dy * carR * 0.9 + py * carR * 1.15;
        const r2x = cx - dx * carR * 0.9 - px * carR * 1.15,
          r2y = cy - dy * carR * 0.9 - py * carR * 1.15;
        ctx.beginPath();
        ctx.moveTo(tipx, tipy);
        ctx.lineTo(r1x, r1y);
        ctx.lineTo(r2x, r2y);
        ctx.closePath();
        ctx.fillStyle = locoFill;
        ctx.fill();
      } else {
        ctx.beginPath();
        ctx.arc(cx, cy, carR, 0, Math.PI * 2);
        ctx.stroke();
      }

      if (pass[k] != null) {
        ctx.beginPath();
        ctx.arc(cx, cy, carR * 0.55, 0, Math.PI * 2);
        ctx.fillStyle = colorOf(pass[k]) || "#ffffff";
        ctx.fill();
      }
      ctx.restore();
    }
  }
}


window.addEventListener("resize", () => {
  if (gameoverEl && !gameoverEl.classList.contains("hidden")) orientGameover();
  if (!lastLayout) return;
  orientHud();
  resizeCanvas();
  draw();
  const v = viewport();
  if (Math.abs(v.w - lastHelloDims.w) > 4 || Math.abs(v.h - lastHelloDims.h) > 4) {
    sendHello();
  }
});

function eventToBoard(ev) {
  if (!lastLayout) return null;
  const rect = canvas.getBoundingClientRect();
  const src = ev.touches?.[0] || ev.changedTouches?.[0] || ev;
  const cx = src.clientX - rect.left;
  const cy = src.clientY - rect.top;
  const PW = window.innerWidth;
  const PH = window.innerHeight;
  const { you } = lastLayout;
  const deviceOri = PW >= PH ? "landscape" : "portrait";
  const rotated = you.orientation && deviceOri !== you.orientation;
  const lx = rotated ? cy : cx;
  const ly = rotated ? PW - cx : cy;
  return { x: you.x + lx, y: you.y + ly };
}

function pointInStationPlatform(p, st, nodes) {
  const a = nodes[st.n0],
    b = nodes[st.n1];
  if (!a || !b) return false;
  const o1 = STATION_PLAT,
    o2 = STATION_PLAT + STATION_DEPTH;
  const q = [
    { x: a.x + st.nx * o1, y: a.y + st.ny * o1 },
    { x: b.x + st.nx * o1, y: b.y + st.ny * o1 },
    { x: b.x + st.nx * o2, y: b.y + st.ny * o2 },
    { x: a.x + st.nx * o2, y: a.y + st.ny * o2 },
  ];
  let sign = 0;
  for (let i = 0; i < 4; i++) {
    const c = q[i],
      d = q[(i + 1) % 4];
    const cross = (d.x - c.x) * (p.y - c.y) - (d.y - c.y) * (p.x - c.x);
    if (cross !== 0) {
      if (sign === 0) sign = cross > 0 ? 1 : -1;
      else if ((cross > 0 ? 1 : -1) !== sign) return false;
    }
  }
  return true;
}

// Resolves a tap at a board point: menu, then a powerup looking for a target, then the
// tap targets. Returns true if anything took it. Taking a point rather than an event is
// what lets a press that started in a boost lane be handed on here later, once the gesture
// has turned out not to be a swipe -- see onBoardUp.
function resolveTap(p) {
  if (!p || !lastLayout?.rail) return false;
  const { nodes, stations, junctions, boosts, switches } = lastLayout.rail;

  const you = lastLayout.you;
  if (menuHit && Math.hypot(p.x - you.x - menuHit.x, p.y - you.y - menuHit.y) <= menuHit.r) {
    openMenu();
    return true;
  }

  // A powerup awaiting a target takes over the tap on EVERY phone -- the player who bought
  // it may not be the one whose screen shows the target. The game is held still until this
  // resolves, so nothing else on the board should respond: the tap is consumed either way.
  const arming = armedPowerup();
  if (arming) {
    if (arming.target === "station") {
      // A freeze has to name a station, so a tap on empty board does nothing and the
      // board stays held until someone picks one.
      let bestSt = null,
        bestStD = Infinity;
      for (const st of stations) {
        const g = stationGate(nodes, st);
        if (!g) continue;
        const d = Math.hypot(p.x - g.x, p.y - g.y);
        if (d <= GATE_HIT || pointInStationPlatform(p, st, nodes)) {
          if (d < bestStD) {
            bestStD = d;
            bestSt = st;
          }
        }
      }
      if (bestSt) send({ type: "puTarget", id: arming.id, station: bestSt.id });
    } else {
      // An extra train can go anywhere: snap to the nearest main-ring node, however far
      // the tap landed. The server then slides it clear of any train already sitting there.
      const ringN = Math.min(lastLayout.rail.N ?? nodes.length, nodes.length);
      let best = -1,
        bestD = Infinity;
      for (let n = 0; n < ringN; n++) {
        const nd = nodes[n];
        if (!nd) continue;
        const d = Math.hypot(p.x - nd.x, p.y - nd.y);
        if (d < bestD) {
          bestD = d;
          best = n;
        }
      }
      if (best >= 0) send({ type: "puTarget", id: arming.id, node: best });
    }
    return true;
  }

  let best = null;
  let bestD = Infinity;
  for (const t of tapTargets()) {
    // Boost lanes answer to drags, not taps. Leaving them out here is the whole fix: a pad
    // and a station gate can now sit on the same spot without trading presses.
    if (t.swipe) continue;
    const d = Math.hypot(p.x - t.at.x, p.y - t.at.y);
    if (d <= t.hit && d < bestD) {
      bestD = d;
      best = t.msg;
    }
  }

  if (!best) return false;
  send(best);
  return true;
}

// How much of a swipe `along` board px of travel along the rail amounts to, signed. The
// first few px count for nothing, so a tap that shifts under the finger lights nothing and
// still reads as a tap; past that it is linear up to a full swipe.
function boostProgress(along) {
  const m = Math.abs(along);
  if (m <= BOOST_SWIPE_SLOP) return 0;
  const f = Math.min(1, (m - BOOST_SWIPE_SLOP) / (BOOST_SWIPE_DIST - BOOST_SWIPE_SLOP));
  return along < 0 ? -f : f;
}

// The press in flight when it landed in a boost lane: { id, p0, boost, moved }. A press
// anywhere else is settled on the spot and never gets one of these.
let boardPtr = null;

// Board input is one gesture at a time. A press that lands outside every boost lane is a
// tap and resolves immediately, the way it always did. A press inside a lane is HELD: if
// the finger then runs along the rail it drives the pad, and if it stays put the press is
// handed to resolveTap on release, so whatever shares the lane -- usually a station gate --
// still gets its tap, just on the up stroke instead of the down.
function onBoardDown(ev) {
  const p = eventToBoard(ev);
  if (!p) return;
  // A powerup hunting for a target owns the board outright and the game is held still for
  // it, so pads are not swipeable while one is armed -- that tap must land instantly.
  const lane = armedPowerup() ? null : boostLaneAt(p);
  if (!lane) {
    if (resolveTap(p)) ev.preventDefault();
    return;
  }
  boardPtr = { id: ev.pointerId, p0: p, boost: lane.boost, moved: 0 };
  boostSwipe = { id: lane.boost.id, prog: 0 };
  ev.preventDefault();
  // The board is full-screen, but capture also keeps the gesture alive across a finger that
  // strays onto the HUD, and guarantees the up stroke comes back to us.
  try {
    canvas.setPointerCapture(ev.pointerId);
  } catch {}
}

function onBoardMove(ev) {
  if (!boardPtr || ev.pointerId !== boardPtr.id) return;
  const p = eventToBoard(ev);
  if (!p || !lastLayout?.rail) return;
  const dx = p.x - boardPtr.p0.x,
    dy = p.y - boardPtr.p0.y;
  boardPtr.moved = Math.max(boardPtr.moved, Math.hypot(dx, dy));
  // Re-read the axis rather than keeping the one from the down stroke: reverse can land
  // mid-gesture, and the swipe has to mean what it means NOW.
  const [ux, uy] = boostAxis(lastLayout.rail, boardPtr.boost.node);
  boostSwipe = { id: boardPtr.boost.id, prog: boostProgress(dx * ux + dy * uy) };
}

function onBoardUp(ev) {
  if (!boardPtr || ev.pointerId !== boardPtr.id) return;
  const { boost, p0, moved } = boardPtr;
  const prog = boostSwipe && boostSwipe.id === boost.id ? boostSwipe.prog : 0;
  boardPtr = null;
  boostSwipe = null;
  ev.preventDefault();
  if (Math.abs(prog) >= BOOST_SWIPE_DONE) {
    // boostArm TOGGLES on the server, so speak only when the swipe asks for a state the pad
    // is not already in: swiping an armed pad forward again must not turn it off.
    const want = prog > 0;
    if (want !== !!boost.armed) {
      send({ type: "boostArm", boost: boost.id });
      boostWish.set(boost.id, { armed: want, until: performance.now() + BOOST_WISH_MS });
    }
    return;
  }
  // Short of a swipe. If the finger never really left where it landed this was a tap on
  // whatever shares the lane; anything in between was a gesture given up on, and does
  // nothing on purpose.
  if (moved <= BOOST_TAP_SLOP) resolveTap(p0);
}

function onBoardCancel() {
  boardPtr = null;
  boostSwipe = null;
}

canvas.addEventListener("pointerdown", onBoardDown);
canvas.addEventListener("pointermove", onBoardMove);
canvas.addEventListener("pointerup", onBoardUp);
canvas.addEventListener("pointercancel", onBoardCancel);

// Set only when the hide below is what stopped the loop, so coming back into view cannot
// restart rendering that something else stopped on purpose -- notably game over, which
// calls stopRender() while #play is still the visible screen.
let pausedByHide = false;

document.addEventListener("visibilitychange", () => {
  if (document.visibilityState !== "visible") {
    // Browsers already throttle rAF on a hidden page, so this is belt-and-braces for the
    // cases where they do not: Android split screen, a partially occluded PWA window.
    // Only the board loop stops -- tickClock and the tutorial loop are separate rAFs.
    if (rafId != null) {
      stopRender();
      pausedByHide = true;
    }
    return;
  }
  if (lastLayout) requestWakeLock();
  if (timelineReady) renderServerTime = lastServerT - playoutDelay;
  // Safe to resume from a cold stop: the playout clock was just re-based on the line above.
  if (pausedByHide) {
    pausedByHide = false;
    startRender();
  }
});

function runIntro() {
  const intro1 = $("intro1");
  const intro2 = $("intro2");
  const tapBtn = $("tapToJoin");
  if (!intro1 || !intro2) return;

  let advanced = false;
  const toIntro2 = () => {
    if (advanced) return;
    advanced = true;
    const titleImg = intro2.querySelector(".intro2-title");
    if (titleImg && !titleImg.getAttribute("src")) titleImg.src = "title.svg";
    intro1.classList.add("hidden");
    intro2.classList.remove("hidden");
    let joined = false;
    intro2.addEventListener("pointerdown", () => {
      if (joined) return;
      joined = true;
          intro2.classList.add("hidden");
      connect();
    });
    tapBtn && tapBtn.classList.remove("hidden");
  };

  const t = setTimeout(toIntro2, 1000);
  intro1.addEventListener("pointerdown", () => {
    clearTimeout(t);
    toIntro2();
  });
}

if (sessionStorage.getItem("tg_ingame") === "1") {
  $("intro1").classList.add("hidden");
  $("intro2").classList.add("hidden");
  connect();
} else {
  runIntro();
}

window.__booted = true;
