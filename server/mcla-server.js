#!/usr/bin/env node
// The online service for the Midnight Club: Los Angeles recompilation
// (mcla_online): one Node.js app that is the website where players create an
// account, the account system the game signs in with, and the matchmaking
// server the game's online modes talk to. Players create an account on the
// site, put the account's game key in mcla.toml, and the game signs in with
// it; the server keeps who is online and which sessions (lobbies) exist and
// answers the game's searches. Races run between the players' PCs over UDP.
//
//   node mcla-server.js [--port 36000] [--bind 0.0.0.0] [--data ./data]
//                       [--base-path /mcla] [--patch-url URL] [--verbose]
//
// Plain Node.js (16 or newer), no packages. Made to run on hosting that
// manages one Node.js app (cPanel / Phusion Passenger, as GoDaddy's does, or a
// platform that runs "npm start"): it listens on PORT when the host sets it,
// on every interface, trusts X-Forwarded-For from the host's proxy, serves
// under PASSENGER_BASE_URI when the app is mounted on a sub-path, and keeps
// accounts, players and sessions in JSON files in the data folder, so nothing
// is lost when the host stops an idle app or runs more than one copy. The
// cookie secret is made on first start and kept there, or comes from
// MCLA_COOKIE_SECRET when the host's disk does not persist.
//
// The API, all JSON (server/README.md has the details). Game routes need
// "Authorization: Bearer <game key>" (the key from the account page):
//   GET    /                          the website
//   GET    /status                    public counts, lobbies and who is online
//   POST   /players                   sign in: {mac, machineId, lanIp, port, titleId} -> {xuid, name, publicIp}
//   POST   /players/:xuid/heartbeat   every 15 s, the same body
//   GET    /players/:xuid
//   DELETE /players/:xuid             sign out
//   POST   /sessions                  a host publishes a session
//   GET    /sessions                  every session (the host's view)
//   POST   /sessions/search           {titleId, numResults, contexts:[{id,value}]}
//   GET    /sessions/:id
//   PUT    /sessions/:id              the host updates flags, slots, state, members, contexts
//   DELETE /sessions/:id
//   POST   /sessions/:id/join         {members:[{xuid,private}]} (the whole list)
//   POST   /sessions/:id/leave        {xuids:[...]}
//   POST   /sessions/:id/migrate      the new host's session (replaces :id)
//   PUT    /sessions/:id/qos          {qos: base64}: the host's QoS data
//   GET    /sessions/:id/qos          {qos: base64}
//   POST   /stats                     a session's stats write (kept, not read back)

'use strict';

const crypto = require('crypto');
const fs = require('fs');
const http = require('http');
const path = require('path');

// --- Configuration -----------------------------------------------------------------

const args = process.argv.slice(2);
function argValue(name, fallback) {
  const i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
}
const HOSTED = !!process.env.PORT;  // started by a host's app manager (Passenger)
const PORT = parseInt(process.env.PORT || argValue('--port', process.env.MCLA_SERVER_PORT || '36000'), 10);
const BIND = argValue('--bind', process.env.MCLA_SERVER_BIND || '0.0.0.0');
const DATA_DIR = path.resolve(argValue('--data', process.env.MCLA_DATA_DIR || path.join(__dirname, 'data')));
const BASE = (argValue('--base-path', process.env.PASSENGER_BASE_URI || '') || '').replace(/\/+$/, '');
const PATCH_URL = argValue('--patch-url', process.env.MCLA_PATCH_URL || 'https://github.com/RenanQueiroz/mcla-recomp/releases');
const SITE_NAME = process.env.MCLA_SITE_NAME || 'Midnight Club: Los Angeles Online';
// The server the game points at when mcla_online_server is not set. When this
// site is that server, the account page leaves the mcla_online_server line out.
const GAME_DEFAULT_SERVER = (process.env.MCLA_GAME_DEFAULT_SERVER || argValue('--game-default-server', 'https://mclaonline.com')).replace(/\/+$/, '').toLowerCase();
const TRUST_PROXY = args.includes('--trust-proxy') || (!args.includes('--no-trust-proxy') && HOSTED);
const VERBOSE = args.includes('--verbose') || process.env.MCLA_VERBOSE === '1';

const PLAYER_TIMEOUT_MS = 60 * 1000;   // no heartbeat for this long: gone
const SESSION_TIMEOUT_MS = 90 * 1000;  // a session whose host is gone
const COOKIE_DAYS = 30;
const XCONTEXT_PRESENCE = 0x8001;
const JOIN_IN_PROGRESS_DISABLED = 0x400;

// Experience. A race finished and a race won each pay XP once per session and
// player. The game reports results with a stats write at the end of a race
// (XSessionWriteStats), which the online layer forwards here. Which property in
// that write marks the winner is the game's own (its leaderboard schema) and is
// read off the log of the first real race: set MCLA_WIN_PROPERTY to its id (hex)
// and MCLA_WIN_VALUE to the value a win carries. Until then wins are off and only
// finishes count.
const XP_FINISH = parseInt(process.env.MCLA_XP_FINISH || argValue('--xp-finish', '50'), 10) || 0;
const XP_WIN = parseInt(process.env.MCLA_XP_WIN || argValue('--xp-win', '150'), 10) || 0;
const WIN_PROPERTY = (parseInt(process.env.MCLA_WIN_PROPERTY || argValue('--win-property', '0'), 16) || 0) >>> 0;
const WIN_VALUE = parseInt(process.env.MCLA_WIN_VALUE || argValue('--win-value', '1'), 10) || 0;
const MAX_LEVEL = 60;

function log(...parts) {
  console.log(new Date().toISOString().slice(0, 19).replace('T', ' '), ...parts);
}
function trace(...parts) {
  if (VERBOSE) log(...parts);
}

// --- JSON files: what survives restarts and is shared between copies of the app ----

class JsonStore {
  constructor(name, initial) {
    this.file = path.join(DATA_DIR, name);
    this.data = initial;
    this.mtime = 0;
    this.size = -1;
    this.load();
  }
  load() {
    try {
      const st = fs.statSync(this.file);
      if (st.mtimeMs === this.mtime && st.size === this.size) return;
      this.data = JSON.parse(fs.readFileSync(this.file, 'utf8'));
      this.mtime = st.mtimeMs;
      this.size = st.size;
    } catch (e) {
      if (e.code !== 'ENOENT') log(`${path.basename(this.file)}: not read (${e.message}); starting from what is in memory`);
    }
  }
  // Picks up what another copy of the app wrote.
  refresh() {
    this.load();
  }
  save() {
    fs.mkdirSync(DATA_DIR, { recursive: true });
    const tmp = `${this.file}.${process.pid}.tmp`;
    fs.writeFileSync(tmp, JSON.stringify(this.data, null, 1));
    fs.renameSync(tmp, this.file);
    const st = fs.statSync(this.file);
    this.mtime = st.mtimeMs;
    this.size = st.size;
  }
}

const accounts = new JsonStore('accounts.json', { nextId: 1, accounts: [] });
const state = new JsonStore('state.json', { players: {}, sessions: {}, stats: [] });
const secrets = new JsonStore('secret.json', {});
if (process.env.MCLA_COOKIE_SECRET) {
  secrets.data.cookieSecret = process.env.MCLA_COOKIE_SECRET;  // the host's, not written to disk
} else if (!secrets.data.cookieSecret) {
  secrets.data.cookieSecret = crypto.randomBytes(32).toString('hex');
  secrets.save();
}

function refreshAll() {
  accounts.refresh();
  state.refresh();
}

// --- Accounts ------------------------------------------------------------------------

// An Xbox gamertag: 1 to 15 letters, digits and single spaces, starting with a
// letter, no space at the end. It is the name the other players see.
function validateGamertag(name) {
  const s = String(name || '').trim();
  if (s.length < 1 || s.length > 15) return 'The gamertag must be 1 to 15 characters.';
  if (!/^[A-Za-z][A-Za-z0-9 ]*$/.test(s)) return 'Letters, digits and spaces only, starting with a letter.';
  if (/ {2}/.test(s)) return 'One space at a time.';
  return null;
}

function hashPassword(password, salt) {
  salt = salt || crypto.randomBytes(16).toString('hex');
  const hash = crypto.scryptSync(password, salt, 32).toString('hex');
  return `scrypt:${salt}:${hash}`;
}
function verifyPassword(password, stored) {
  const parts = String(stored || '').split(':');
  if (parts.length !== 3 || parts[0] !== 'scrypt') return false;
  const expected = Buffer.from(parts[2], 'hex');
  const actual = crypto.scryptSync(password, parts[1], 32);
  return expected.length === actual.length && crypto.timingSafeEqual(expected, actual);
}
function newGameKey() {
  return crypto.randomBytes(20).toString('hex').toUpperCase();
}
// The player's online XUID, from the account's number: the same on every PC.
function xuidFor(id) {
  const h = crypto.createHash('sha256').update(`mcla-xuid:${id}:${secrets.data.cookieSecret}`).digest('hex');
  return ('0009' + h.slice(0, 12)).toUpperCase();
}

function findAccount(pred) {
  return accounts.data.accounts.find(pred) || null;
}
function accountByGamertag(name) {
  const lower = String(name || '').trim().toLowerCase();
  return findAccount((a) => a.gamertagLower === lower);
}
function accountByEmail(email) {
  const lower = String(email || '').trim().toLowerCase();
  return lower ? findAccount((a) => a.emailLower === lower) : null;
}
function accountByKey(key) {
  const k = String(key || '').trim().toUpperCase();
  return k ? findAccount((a) => a.gameKey === k) : null;
}
function accountById(id) {
  return findAccount((a) => a.id === id);
}

function createAccount(gamertag, email, password) {
  const account = {
    id: accounts.data.nextId++,
    gamertag: gamertag.trim(),
    gamertagLower: gamertag.trim().toLowerCase(),
    email: email.trim(),
    emailLower: email.trim().toLowerCase(),
    password: hashPassword(password),
    gameKey: newGameKey(),
    createdAt: new Date().toISOString(),
    lastSeen: null,
    xp: 0,
    racesFinished: 0,
    racesWon: 0,
    history: [],   // the last races: {at, sessionId, result, xp}
    awarded: [],   // "finish:<session>" and "win:<session>" keys already paid
  };
  account.xuid = xuidFor(account.id);
  accounts.data.accounts.push(account);
  accounts.save();
  return account;
}

// --- Levels -----------------------------------------------------------------------

// Level 2 takes 200 XP, and every level after it 100 XP more than the one
// before: level 3 at 500, level 5 at 1,400, level 10 at 5,400, level 20 at 20,900.
function xpForLevel(level) {
  const n = Math.max(0, level - 1);
  return n * 200 + 100 * n * (n - 1) / 2;
}
function levelFromXp(xp) {
  let level = 1;
  while (level < MAX_LEVEL && xp >= xpForLevel(level + 1)) level++;
  return level;
}
const LEVEL_TITLES = [[50, 'Midnight Club'], [35, 'Legend'], [20, 'Elite'], [10, 'Pro'], [5, 'Street Racer'], [1, 'Rookie']];
function levelTitle(level) {
  for (const [min, title] of LEVEL_TITLES) if (level >= min) return title;
  return 'Rookie';
}
// Accounts made before levels existed get the fields on first use.
function ensureProgress(account) {
  if (typeof account.xp !== 'number') account.xp = 0;
  if (typeof account.racesFinished !== 'number') account.racesFinished = 0;
  if (typeof account.racesWon !== 'number') account.racesWon = 0;
  if (!Array.isArray(account.history)) account.history = [];
  if (!Array.isArray(account.awarded)) account.awarded = [];
  return account;
}
function progressOf(account) {
  ensureProgress(account);
  const level = levelFromXp(account.xp);
  const floor = xpForLevel(level);
  const next = level < MAX_LEVEL ? xpForLevel(level + 1) : floor;
  const into = account.xp - floor;
  const need = Math.max(1, next - floor);
  return {
    level, title: levelTitle(level), xp: account.xp, into, need,
    pct: level < MAX_LEVEL ? Math.min(100, Math.round(100 * into / need)) : 100,
    toNext: level < MAX_LEVEL ? next - account.xp : 0,
  };
}
// Pays XP once per key; false when it was paid before.
function awardXp(account, key, amount, result, sessionId) {
  ensureProgress(account);
  if (account.awarded.includes(key)) return false;
  account.awarded.push(key);
  if (account.awarded.length > 200) account.awarded.splice(0, account.awarded.length - 200);
  const before = levelFromXp(account.xp);
  account.xp += amount;
  if (result === 'won') account.racesWon++;
  if (result === 'finished') account.racesFinished++;
  account.history.push({ at: new Date().toISOString(), sessionId, result, xp: amount });
  if (account.history.length > 20) account.history.splice(0, account.history.length - 20);
  const after = levelFromXp(account.xp);
  log(`${account.gamertag}: +${amount} XP, race ${result} (${account.xp} XP, level ${after}${after > before ? ', up from ' + before : ''})`);
  return true;
}
// A property's value as a number, from the bytes the game stored (big-endian).
function propertyNumber(p) {
  const hex = String(p.data || '');
  if (!/^[0-9A-Fa-f]+$/.test(hex) || hex.length > 16) return null;
  const type = Number(p.type);
  if (type === 1 && hex.length >= 8) { const v = parseInt(hex.slice(0, 8), 16); return v > 0x7fffffff ? v - 0x100000000 : v; }
  if (type === 2 && hex.length >= 16) return Number(BigInt.asIntN(64, BigInt('0x' + hex.slice(0, 16))));
  if (type === 5 && hex.length >= 8) return Buffer.from(hex.slice(0, 8), 'hex').readFloatBE(0);
  if (type === 3 && hex.length >= 16) return Buffer.from(hex.slice(0, 16), 'hex').readDoubleBE(0);
  return null;
}
function leaderboard(limit) {
  return accounts.data.accounts
    .map((a) => ({ account: a, p: progressOf(a) }))
    .filter((e) => e.p.xp > 0 || e.account.racesFinished > 0)
    .sort((x, y) => y.p.xp - x.p.xp || x.account.gamertagLower.localeCompare(y.account.gamertagLower))
    .slice(0, limit || 100)
    .map((e, i) => ({ rank: i + 1, name: e.account.gamertag, level: e.p.level, title: e.p.title, xp: e.p.xp, wins: e.account.racesWon, races: e.account.racesFinished }));
}

// --- Website sessions (a signed cookie) ----------------------------------------------

function sign(text) {
  return crypto.createHmac('sha256', secrets.data.cookieSecret).update(text).digest('hex').slice(0, 32);
}
function makeCookie(account) {
  const exp = Date.now() + COOKIE_DAYS * 86400 * 1000;
  const body = `${account.id}.${exp}`;
  return `${body}.${sign(body)}`;
}
function parseCookies(req) {
  const out = {};
  for (const part of String(req.headers.cookie || '').split(';')) {
    const i = part.indexOf('=');
    if (i > 0) out[part.slice(0, i).trim()] = decodeURIComponent(part.slice(i + 1).trim());
  }
  return out;
}
function accountFromCookie(req) {
  const token = parseCookies(req).mcla_session;
  if (!token) return null;
  const parts = token.split('.');
  if (parts.length !== 3) return null;
  const body = `${parts[0]}.${parts[1]}`;
  const expected = sign(body);
  if (expected.length !== parts[2].length ||
      !crypto.timingSafeEqual(Buffer.from(expected), Buffer.from(parts[2]))) return null;
  if (Number(parts[1]) < Date.now()) return null;
  return accountById(Number(parts[0]));
}
function sessionCookieHeader(value, req) {
  const secure = isHttps(req) ? '; Secure' : '';
  if (!value) return `mcla_session=; Path=${BASE || '/'}; Max-Age=0; HttpOnly; SameSite=Lax${secure}`;
  return `mcla_session=${value}; Path=${BASE || '/'}; Max-Age=${COOKIE_DAYS * 86400}; HttpOnly; SameSite=Lax${secure}`;
}

// --- Requests ------------------------------------------------------------------------

function clientIp(req) {
  let ip = req.socket.remoteAddress || '';
  if (TRUST_PROXY) {
    const forwarded = String(req.headers['x-forwarded-for'] || '').split(',')[0].trim();
    if (forwarded) ip = forwarded;
  }
  if (ip.startsWith('::ffff:')) ip = ip.slice(7);
  if (ip === '::1') ip = '127.0.0.1';
  return ip;
}
function isHttps(req) {
  const proto = TRUST_PROXY ? String(req.headers['x-forwarded-proto'] || '').split(',')[0].trim() : '';
  return proto === 'https';
}
function requestHost(req) {
  const forwarded = TRUST_PROXY ? String(req.headers['x-forwarded-host'] || '').split(',')[0].trim() : '';
  return forwarded || req.headers.host || `localhost:${PORT}`;
}
// What players put in mcla_online_server: how this site was reached.
function siteUrl(req) {
  return `${isHttps(req) ? 'https' : 'http'}://${requestHost(req)}${BASE}`;
}

function readBody(req, limit) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    req.on('data', (c) => {
      size += c.length;
      if (size > (limit || 4 * 1024 * 1024)) {
        reject(new Error('body too large'));
        req.destroy();
        return;
      }
      chunks.push(c);
    });
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    req.on('error', reject);
  });
}
async function readJson(req) {
  const text = await readBody(req);
  if (!text.trim()) return {};
  try {
    return JSON.parse(text);
  } catch (e) {
    throw new Error('bad JSON: ' + e.message);
  }
}
async function readForm(req) {
  const text = await readBody(req, 64 * 1024);
  const out = {};
  for (const [k, v] of new URLSearchParams(text)) out[k] = v;
  return out;
}

function send(res, status, body, type, extraHeaders) {
  const text = typeof body === 'string' ? body : JSON.stringify(body);
  res.writeHead(status, Object.assign({
    'Content-Type': type || 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(text),
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff',
  }, extraHeaders || {}));
  res.end(text);
}
function redirect(res, location, extraHeaders) {
  res.writeHead(303, Object.assign({ Location: BASE + location, 'Cache-Control': 'no-store' }, extraHeaders || {}));
  res.end();
}

// A few tries a minute per address for the forms, against guessing.
const attempts = new Map();
function rateLimited(ip, what, max, windowMs) {
  const key = `${what}:${ip}`;
  const now = Date.now();
  const entry = attempts.get(key) || { count: 0, start: now };
  if (now - entry.start > windowMs) { entry.count = 0; entry.start = now; }
  entry.count++;
  attempts.set(key, entry);
  if (attempts.size > 10000) attempts.clear();
  return entry.count > max;
}

// Forms are posted from this site only.
function sameOrigin(req) {
  const origin = req.headers.origin || (req.headers.referer ? req.headers.referer.replace(/^(https?:\/\/[^/]+).*/, '$1') : '');
  if (!origin) return true;
  return origin.replace(/^https?:\/\//, '') === requestHost(req);
}

// --- Ids and players --------------------------------------------------------------------

function normId(s) {
  if (typeof s !== 'string') return '';
  s = s.trim().toUpperCase();
  if (s.startsWith('0X')) s = s.slice(2);
  return /^[0-9A-F]{1,16}$/.test(s) ? s.padStart(16, '0') : '';
}

function upsertPlayer(account, body, ip) {
  const players = state.data.players;
  const existing = players[account.xuid];
  const player = existing || { xuid: account.xuid, firstSeen: Date.now() };
  player.name = account.gamertag;
  player.accountId = account.id;
  player.mac = String(body.mac || player.mac || '').toUpperCase().slice(0, 12);
  player.machineId = normId(body.machineId) || player.machineId || ('FA' + account.xuid.slice(2));
  player.lanIp = String(body.lanIp || player.lanIp || ip).slice(0, 45);
  player.publicIp = ip;
  player.port = Number(body.port) || player.port || 0;
  player.titleId = Number(body.titleId) || player.titleId || 0;
  player.lastSeen = Date.now();
  players[account.xuid] = player;
  for (const s of Object.values(state.data.sessions)) {
    if (s.hostXuid === account.xuid) {
      s.lastSeen = player.lastSeen;
      s.host.publicIp = ip;
      if (player.port) s.host.port = player.port;
      if (player.lanIp) s.host.lanIp = player.lanIp;
    }
  }
  account.lastSeen = new Date().toISOString();
  return player;
}

function playerJson(p) {
  return {
    xuid: p.xuid, name: p.name, mac: p.mac, machineId: p.machineId,
    lanIp: p.lanIp, publicIp: p.publicIp, port: p.port, titleId: p.titleId,
    lastSeen: new Date(p.lastSeen).toISOString(),
  };
}

// --- Sessions ----------------------------------------------------------------------------

function hostFromBody(body, player, account, ip) {
  const h = body.host || {};
  return {
    xuid: account.xuid,
    name: account.gamertag,
    mac: String(h.mac || (player && player.mac) || '').toUpperCase().slice(0, 12),
    machineId: normId(h.machineId) || (player && player.machineId) || ('FA' + account.xuid.slice(2)),
    lanIp: String(h.lanIp || (player && player.lanIp) || ip).slice(0, 45),
    publicIp: ip,
    port: Number(h.port) || (player && player.port) || 0,
  };
}
function cleanContexts(list) {
  if (!Array.isArray(list)) return [];
  return list
    .filter((c) => c && Number.isFinite(Number(c.id)))
    .map((c) => ({ id: Number(c.id) >>> 0, value: Number(c.value) >>> 0 }))
    .slice(0, 64);
}
function cleanProperties(list) {
  if (!Array.isArray(list)) return [];
  return list
    .filter((p) => p && Number.isFinite(Number(p.id)))
    .map((p) => ({
      id: Number(p.id) >>> 0,
      type: Number(p.type) || ((Number(p.id) >>> 28) & 0xF),
      data: String(p.data || '').toUpperCase().slice(0, 8192),
    }))
    .slice(0, 64);
}
function cleanMembers(list) {
  if (!Array.isArray(list)) return [];
  const out = [];
  for (const m of list) {
    const xuid = normId(m && m.xuid);
    if (!xuid || out.some((x) => x.xuid === xuid)) continue;
    out.push({ xuid, private: !!(m && m.private), userIndex: Number(m && m.userIndex) || 0 });
    if (out.length >= 64) break;
  }
  return out;
}

function createSession(account, body, ip) {
  const id = normId(body.id);
  if (!id) return null;
  const player = state.data.players[account.xuid];
  const session = {
    id,
    titleId: Number(body.titleId) || (player && player.titleId) || 0,
    hostXuid: account.xuid,
    hostName: account.gamertag,
    flags: Number(body.flags) >>> 0,
    publicSlots: Number(body.publicSlots) || 0,
    privateSlots: Number(body.privateSlots) || 0,
    state: Number(body.state) || 0,
    nonce: normId(body.nonce) || '0000000000000000',
    key: String(body.key || '').toUpperCase().slice(0, 32),
    host: hostFromBody(body, player, account, ip),
    contexts: cleanContexts(body.contexts),
    properties: cleanProperties(body.properties),
    members: cleanMembers(body.members),
    qos: typeof body.qos === 'string' ? body.qos.slice(0, 16384) : '',
    created: Date.now(),
    lastSeen: Date.now(),
  };
  state.data.sessions[id] = session;
  return session;
}
function updateSession(session, body, account, ip) {
  if (body.flags !== undefined) session.flags = Number(body.flags) >>> 0;
  if (body.publicSlots !== undefined) session.publicSlots = Number(body.publicSlots) || 0;
  if (body.privateSlots !== undefined) session.privateSlots = Number(body.privateSlots) || 0;
  if (body.state !== undefined) session.state = Number(body.state) || 0;
  if (body.members !== undefined) session.members = cleanMembers(body.members);
  if (body.contexts !== undefined) session.contexts = cleanContexts(body.contexts);
  if (body.properties !== undefined) session.properties = cleanProperties(body.properties);
  if (body.host !== undefined) session.host = hostFromBody(body, state.data.players[account.xuid], account, ip);
  if (typeof body.qos === 'string') session.qos = body.qos.slice(0, 16384);
  session.lastSeen = Date.now();
}
function slotCounts(s) {
  const filledPrivate = s.members.filter((m) => m.private).length;
  const filledPublic = s.members.length - filledPrivate;
  return {
    filledPublic, filledPrivate,
    openPublic: Math.max(0, s.publicSlots - filledPublic),
    openPrivate: Math.max(0, s.privateSlots - filledPrivate),
  };
}
function sessionJson(s, withQos) {
  const j = {
    id: s.id, titleId: s.titleId, hostXuid: s.hostXuid, hostName: s.hostName,
    flags: s.flags, publicSlots: s.publicSlots, privateSlots: s.privateSlots,
    ...slotCounts(s), state: s.state, nonce: s.nonce, key: s.key, host: s.host,
    contexts: s.contexts, properties: s.properties, members: s.members,
    created: new Date(s.created).toISOString(),
  };
  if (withQos) j.qos = s.qos;
  return j;
}

// Sessions of the title with an open public slot, not in a race when the host
// forbids joining one, whose contexts agree with the search's on every context
// the search names (the presence context aside), the searcher's own left out.
function searchSessions(account, body) {
  const titleId = Number(body.titleId) || 0;
  const wanted = cleanContexts(body.contexts).filter((c) => c.id !== XCONTEXT_PRESENCE);
  const max = Math.max(1, Math.min(Number(body.numResults) || 50, 200));
  const results = [];
  for (const s of Object.values(state.data.sessions)) {
    if (titleId && s.titleId && s.titleId !== titleId) continue;
    if (s.hostXuid === account.xuid) continue;
    if (s.state === 4) continue;
    if (s.state === 2 && (s.flags & JOIN_IN_PROGRESS_DISABLED)) continue;
    if (slotCounts(s).openPublic === 0) continue;
    let ok = true;
    for (const want of wanted) {
      const have = s.contexts.find((c) => c.id === want.id);
      if (!have || have.value !== want.value) { ok = false; break; }
    }
    if (!ok) continue;
    results.push(s);
  }
  results.sort((a, b) => a.created - b.created);
  return results.slice(0, max).map((s) => sessionJson(s, true));
}

function expire() {
  const now = Date.now();
  let changed = false;
  for (const [xuid, p] of Object.entries(state.data.players)) {
    if (now - p.lastSeen > PLAYER_TIMEOUT_MS) {
      delete state.data.players[xuid];
      log(`player ${p.name} (${xuid}) timed out`);
      changed = true;
    }
  }
  for (const [id, s] of Object.entries(state.data.sessions)) {
    if (now - s.lastSeen > SESSION_TIMEOUT_MS) {
      delete state.data.sessions[id];
      log(`session ${id} of ${s.hostName || s.hostXuid} timed out`);
      changed = true;
    }
  }
  return changed;
}

function publicStatus() {
  const players = Object.values(state.data.players).sort((a, b) => a.name.localeCompare(b.name));
  const sessions = Object.values(state.data.sessions).sort((a, b) => a.created - b.created);
  return {
    site: SITE_NAME,
    accounts: accounts.data.accounts.length,
    online: players.length,
    players: players.map((p) => {
      const a = accountById(p.accountId);
      return { name: p.name, level: a ? progressOf(a).level : 1, since: new Date(p.firstSeen).toISOString() };
    }),
    topRacers: leaderboard(10),
    sessions: sessions.map((s) => {
      const slots = slotCounts(s);
      return {
        id: s.id, host: s.hostName, state: s.state, players: s.members.length,
        openPublic: slots.openPublic, publicSlots: s.publicSlots, flags: s.flags,
        created: new Date(s.created).toISOString(),
      };
    }),
  };
}

// --- The website -----------------------------------------------------------------------

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

const STATE_NAMES = ['in the lobby', 'registering', 'racing', 'reporting', 'closed'];

// The look: one dark panel, crimson at the top fading through black to a
// deep teal, a soft red glow behind the centre and a huge ghosted "LA" in it;
// light geometric type (Outfit) for the place at the top left, a heavy name
// at the bottom left over a row of avatar chips, a drawn circuit at the top
// right and the live numbers stacked at the bottom right with the last one in
// yellow; a white sign-in button and a hamburger square in the nav.
function layout(req, title, body, account, options) {
  const h = escapeHtml;
  const opts = options || {};
  const arrow = '<svg width="16" height="16" viewBox="0 0 16 16" fill="none" aria-hidden="true"><circle cx="8" cy="8" r="7" stroke="currentColor" stroke-width="1.3"/><path d="M6 8h5M9 5.8 11.2 8 9 10.2" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"/></svg>';
  const nav = account
    ? `<a class="signin" href="${BASE}/account">${arrow}<span>${h(account.gamertag)}</span></a>
       <form method="post" action="${BASE}/logout" class="inline"><button class="iconbtn" type="submit" title="Log out" aria-label="Log out"><svg width="16" height="16" viewBox="0 0 16 16" fill="none" aria-hidden="true"><path d="M6 3H3.5A1.5 1.5 0 0 0 2 4.5v7A1.5 1.5 0 0 0 3.5 13H6M10 11l3-3-3-3M13 8H6" stroke="currentColor" stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round"/></svg></button></form>`
    : `<a class="signin" href="${BASE}/login">${arrow}<span>Sign in</span></a>
       <a class="iconbtn" href="${BASE}/register" title="Create account" aria-label="Create account"><svg width="18" height="18" viewBox="0 0 18 18" fill="none" aria-hidden="true"><path d="M3 5h12M3 9h12M3 13h12" stroke="currentColor" stroke-width="1.4" stroke-linecap="round"/></svg></a>`;
  return `<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>${h(title)} · ${h(SITE_NAME)}</title>
<link rel="preconnect" href="https://fonts.googleapis.com"><link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Outfit:wght@200;300;400;500;600;700;800&display=swap" rel="stylesheet">
<style>
:root{--frame:#141416;--text:#fff;--muted:rgba(255,255,255,.58);--faint:rgba(255,255,255,.3);--line:rgba(255,255,255,.09);--glass:rgba(255,255,255,.045);--yellow:#f5c518;--red:#ff2a36;--good:#4ade80;--bad:#ff6b7a}
*{box-sizing:border-box}html{scroll-behavior:smooth}
body{margin:0;min-height:100vh;background:#0a0d14;color:var(--text);font:16px/1.55 Outfit,system-ui,Segoe UI,sans-serif;-webkit-font-smoothing:antialiased}
a{color:#fff}
.frame{margin:0;padding:0}
.panel{position:relative;overflow:hidden;min-height:100vh;
  background:#0a0d14 url("${BASE}/assets/la-sunset.webp") top center/100% auto no-repeat}
.panel::before{content:"";position:absolute;inset:0;pointer-events:none;background:linear-gradient(180deg,rgba(14,6,18,.38) 0%,rgba(14,6,18,.42) 30%,rgba(10,9,18,.74) 58%,rgba(10,13,20,.96) 78%,#0a0d14 100%)}
.panel>*{position:relative}
/* The nav strip. */
.topnav{display:flex;align-items:center;justify-content:space-between;gap:20px;padding:30px 44px 8px;text-shadow:0 1px 12px rgba(0,0,0,.35)}
.logo{display:inline-flex;align-items:baseline;gap:6px;text-decoration:none;color:#fff;font-weight:800;font-size:34px;font-style:italic;letter-spacing:-.04em;line-height:1}
.logo b{font-weight:800;color:var(--red)}.logo small{font-style:normal;font-weight:500;font-size:11px;letter-spacing:.28em;text-transform:uppercase;color:var(--muted);margin-left:6px}
.topnav .links{display:flex;gap:46px;font-size:16px;font-weight:300;letter-spacing:.01em}.topnav .links a{color:rgba(255,255,255,.75);text-decoration:none}.topnav .links a:hover{color:#fff}
.topnav .right{display:flex;align-items:center;gap:12px}
.signin{display:inline-flex;align-items:center;gap:10px;white-space:nowrap;background:#fff;color:#111;text-decoration:none;font-weight:500;font-size:15px;padding:14px 24px;border-radius:9px;transition:transform .15s}.signin:hover{transform:translateY(-1px)}
.iconbtn{display:inline-flex;align-items:center;justify-content:center;width:50px;height:50px;border-radius:9px;background:rgba(255,255,255,.08);color:rgba(255,255,255,.8);border:0;cursor:pointer;text-decoration:none}.iconbtn:hover{background:rgba(255,255,255,.14)}
form.inline{display:inline}button.link{background:none;border:0;color:var(--muted);cursor:pointer;font:inherit;padding:0}
main{padding:10px 44px 40px}
/* The hero. */
.hero{position:relative;min-height:max(850px,calc(100vh - 112px));overflow:hidden;margin:0 -44px;padding:0 44px}
.hero .ghost{position:absolute;left:50%;top:44%;transform:translate(-50%,-50%);font-weight:800;font-size:clamp(220px,36vw,560px);line-height:1;letter-spacing:-.06em;color:rgba(255,255,255,.07);filter:blur(1.5px);user-select:none;pointer-events:none;white-space:nowrap}
.hero .place{position:absolute;left:44px;top:104px}
.hero .place .where{font-size:16px;font-weight:300;color:rgba(255,255,255,.8);margin-bottom:18px;text-shadow:0 1px 10px rgba(0,0,0,.4)}.hero .place .where span{margin-left:8px}
.hero .place h2{margin:0;font-weight:200;font-size:clamp(44px,5.6vw,84px);line-height:1.02;letter-spacing:-.025em;text-shadow:0 2px 18px rgba(0,0,0,.45)}
.hero .track{position:absolute;right:44px;top:128px;width:min(460px,36vw);height:auto}
.hero .track img{width:100%;height:auto;display:block;filter:drop-shadow(0 14px 34px rgba(0,0,0,.55))}
.hero .bottomleft{position:absolute;left:44px;bottom:48px;max-width:70%}
.chips{display:flex;align-items:center;gap:14px;margin-bottom:22px}.avatars{display:flex}
.avatar{width:50px;height:50px;border-radius:50%;background:#2a2d35;border:3px solid #15161b;margin-left:-14px;display:flex;align-items:center;justify-content:center;font-weight:600;font-size:14px;color:#fff;box-shadow:0 6px 16px rgba(0,0,0,.35)}
.avatar:first-child{margin-left:0;background:radial-gradient(circle at 35% 35%,#ff4a4a,#b4000f 70%);color:var(--yellow);font-weight:800;font-size:12px;letter-spacing:.02em}
.chips .all{display:inline-flex;align-items:center;justify-content:center;height:50px;padding:0 20px;margin-left:-10px;border-radius:999px;background:var(--yellow);color:#111;font-weight:600;text-decoration:none;border:3px solid #15161b}
.hero h1.name{margin:0;font-weight:700;font-size:clamp(64px,8.4vw,150px);line-height:.9;letter-spacing:-.045em;color:#fff}
.hero .ctas{display:flex;align-items:center;gap:22px;margin-top:26px;flex-wrap:wrap}.hero .ctas a.plain{color:rgba(255,255,255,.75);text-decoration:none;font-weight:300;border-bottom:1px solid rgba(255,255,255,.3)}
.hero .points{position:absolute;right:44px;bottom:48px;text-align:right}
.hero .points .label{font-size:16px;font-weight:300;color:var(--muted);margin-bottom:18px;display:flex;justify-content:flex-end;align-items:center;gap:10px}
.hero .points .n{font-weight:300;font-size:clamp(52px,6.4vw,104px);line-height:.88;letter-spacing:-.04em;color:rgba(255,255,255,.42)}.hero .points .n+.n{color:rgba(255,255,255,.24)}
.hero .points .n.gold{font-weight:700;font-size:clamp(70px,8.4vw,138px);color:var(--yellow);margin-top:4px}
.hero .points .legend{font-size:12px;font-weight:300;color:var(--faint);letter-spacing:.08em;text-transform:uppercase;margin-top:12px}
/* Sections and cards. */
.grid{display:grid;grid-template-columns:1fr 1fr;gap:18px;margin-top:8px}#top{grid-column:1/-1}
.card{background:var(--glass);border:1px solid var(--line);border-radius:16px;padding:26px 28px;backdrop-filter:blur(6px)}
.card h2{font-weight:600;font-size:26px;letter-spacing:-.02em;margin:0 0 12px}.card h3{font-size:12px;letter-spacing:.24em;text-transform:uppercase;color:var(--muted);margin:22px 0 10px;font-weight:500}
ol.steps{padding-left:0;list-style:none;counter-reset:step;margin:0}ol.steps li{position:relative;padding:12px 0 12px 54px;counter-increment:step;border-bottom:1px solid var(--line);color:rgba(255,255,255,.82);font-weight:300}ol.steps li:last-child{border-bottom:0}
ol.steps li::before{content:counter(step,decimal-leading-zero);position:absolute;left:0;top:10px;font-weight:600;font-size:22px;color:var(--yellow);letter-spacing:-.02em}
code,pre{font:13.5px/1.5 ui-monospace,Consolas,monospace;background:rgba(0,0,0,.35);border:1px solid var(--line);border-radius:10px;color:#fff}code{padding:2px 6px}pre{padding:14px 16px;overflow:auto;margin:12px 0}
table{width:100%;border-collapse:collapse;margin-top:8px}th,td{text-align:left;padding:10px 10px;border-bottom:1px solid var(--line);vertical-align:top;font-weight:300}th{color:var(--muted);font-weight:500;font-size:11px;text-transform:uppercase;letter-spacing:.18em}
label{display:block;margin:14px 0 6px;color:var(--muted);font-size:12px;letter-spacing:.14em;text-transform:uppercase;font-weight:500}
input{width:100%;padding:13px 14px;border-radius:10px;border:1px solid var(--line);background:rgba(0,0,0,.35);color:#fff;font:inherit}input:focus{outline:2px solid var(--yellow);border-color:transparent}
.pill-button,button.primary{display:inline-flex;align-items:center;justify-content:center;background:#fff;color:#111;font:500 15px/1 Outfit,system-ui,sans-serif;padding:15px 26px;border-radius:9px;border:0;text-decoration:none;cursor:pointer;transition:transform .15s}.pill-button:hover,button.primary:hover{transform:translateY(-1px)}
button.secondary,.pill-button.ghost{background:rgba(255,255,255,.08);color:#fff;border:1px solid var(--line);font:500 14px/1 Outfit,system-ui,sans-serif;padding:13px 20px;border-radius:9px;cursor:pointer;text-decoration:none}button.secondary:hover{background:rgba(255,255,255,.14)}
.narrow{max-width:480px;margin:10px auto}.error{background:rgba(255,42,54,.14);border:1px solid rgba(255,107,122,.5);color:#ffd7dc;padding:10px 14px;border-radius:10px;margin:12px 0}.ok{background:rgba(74,222,128,.12);border:1px solid rgba(74,222,128,.45);color:#d4f8e2;padding:10px 14px;border-radius:10px;margin:12px 0}
.muted{color:var(--muted)}.key{font:16px ui-monospace,Consolas,monospace;letter-spacing:1px;word-break:break-all;background:rgba(0,0,0,.35);border:1px dashed var(--yellow);border-radius:10px;padding:12px 14px}.row{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin:10px 0}
.pill{display:inline-block;padding:3px 12px;border-radius:999px;font-size:12px;letter-spacing:.08em;text-transform:uppercase;background:rgba(255,255,255,.06);border:1px solid var(--line);color:var(--muted);vertical-align:middle;font-weight:500}.pill.on{background:rgba(74,222,128,.12);color:var(--good);border-color:rgba(74,222,128,.35)}
.racers{display:flex;flex-wrap:wrap;gap:8px}
.lvl{display:inline-flex;align-items:center;gap:12px;margin-top:16px;padding:6px 14px 6px 6px;border-radius:999px;background:rgba(255,255,255,.08);border:1px solid var(--line)}.lvl b{display:inline-flex;align-items:center;height:30px;padding:0 12px;border-radius:999px;background:var(--yellow);color:#111;font-weight:700;font-size:13px;letter-spacing:.06em}.lvl span{font-size:14px;font-weight:300;color:rgba(255,255,255,.8)}
.lvl.big{margin:0}.progress{display:flex;align-items:center;gap:18px;flex-wrap:wrap;margin:4px 0 20px}.progress .xp{flex:1;min-width:220px}.xpbar{height:8px;border-radius:999px;background:rgba(255,255,255,.1);overflow:hidden}.xpbar div{height:100%;background:var(--yellow);border-radius:999px}.xptext{font-size:13px;font-weight:300;color:var(--muted);margin-top:8px}
b.gold{color:var(--yellow)}table.lb td:first-child{width:36px}.pill i{font-style:normal;opacity:.6;margin-left:4px}
footer{text-align:center;color:rgba(255,255,255,.45);font-size:12.5px;padding:22px 16px 28px;font-weight:300}footer a{color:rgba(255,255,255,.7)}
@media (min-width:1500px){.topnav{padding:34px 72px 8px}main{padding:10px 72px 48px}.hero{margin:0 -72px;padding:0 72px}.hero .place,.hero .bottomleft{left:72px}.hero .track,.hero .points{right:72px}.hero .place{top:130px}.hero .track{top:140px}}
@media (max-width:980px){.grid{grid-template-columns:1fr}.topnav{padding:22px 24px 4px}.topnav .links{display:none}main{padding:8px 24px 30px}.hero{margin:0 -24px;padding:0 24px;min-height:0}.panel{min-height:0}
  .hero .place,.hero .track,.hero .bottomleft,.hero .points{position:static;max-width:none}.hero{display:flex;flex-direction:column;gap:30px;padding-top:40px;padding-bottom:30px}
  .hero .track{width:min(360px,78vw);align-self:flex-end;margin-top:-6px}.hero .points{text-align:left}.hero .points .label{justify-content:flex-start}.hero .ghost{top:38%}}
@media (max-width:520px){.panel{background-size:auto 760px}.logo{font-size:28px}.logo small{display:none}.signin{padding:12px 14px;font-size:13px;gap:8px}.hero h1.name{font-size:clamp(56px,17vw,90px)}.avatar,.chips .all{width:42px;height:42px}.chips .all{width:auto}}
</style></head>
<body><div class="frame">
<div class="panel">
<header class="topnav">
  <a class="logo" href="${BASE}/">MC<b>LA</b><small>Online</small></a>
  <nav class="links"><a href="${BASE}/#lobbies">Lobbies</a><a href="${BASE}/leaderboard">Leaderboard</a><a href="${BASE}/#play">How to play</a><a href="${BASE}/account">Account</a></nav>
  <div class="right">${nav}</div>
</header>
<main>${body}</main>
</div>
<footer>A fan-run service for the PC recompilation of <i>Midnight Club: Los Angeles Complete Edition</i>. Not affiliated with Rockstar Games or Take-Two Interactive. <a href="${BASE}/status">Status (JSON)</a></footer>
</div></body></html>`;
}

function homePage(req, account) {
  const h = escapeHtml;
  const st = publicStatus();
  const url = siteUrl(req);
  const sessionRows = st.sessions.length
    ? st.sessions.map((s) => `<tr><td>${h(s.host)}</td><td>${h(STATE_NAMES[s.state] || s.state)}</td><td>${s.players} / ${s.publicSlots}</td><td class="muted">${h(s.created.slice(11, 19))} UTC</td></tr>`).join('')
    : '<tr><td colspan="4" class="muted">No lobby is open right now. Start one from the game and it shows up here.</td></tr>';
  const playerList = st.players.length ? st.players.map((p) => `<span class="pill on">${h(p.name)} <i>${p.level}</i></span>`).join('') : '<span class="muted">Nobody is in the game right now.</span>';
  const topRows = st.topRacers.map((r) => `<tr><td class="muted">${r.rank}</td><td>${h(r.name)}</td><td><b class="gold">${r.level}</b> <span class="muted">${h(r.title)}</span></td><td>${r.wins}</td><td>${r.xp}</td></tr>`).join('');
  const initials = (name) => name.split(' ').filter(Boolean).slice(0, 2).map((w) => w[0].toUpperCase()).join('') || '?';
  const avatars = ['<span class="avatar" title="Midnight Club: Los Angeles">MC</span>']
    .concat(st.players.slice(0, 4).map((p) => `<span class="avatar" title="${h(p.name)}">${h(initials(p.name))}</span>`))
    .join('');
  // The big name: the signed-in racer's, split like a driver's, or the call to race.
  let name = 'Race<br>online';
  if (account) {
    const words = account.gamertag.split(' ');
    name = words.length > 1 ? `${h(words[0])}<br>${h(words.slice(1).join(' '))}` : h(account.gamertag);
  }
  const ctas = account
    ? `<a class="pill-button" href="${BASE}/account">Your account and game key</a>`
    : `<a class="pill-button" href="${BASE}/register">Create an account</a><a class="plain" href="${BASE}/login">I already have one</a>`;
  return layout(req, 'Home', `
<section class="hero">
  <div class="ghost">LA</div>
  <div class="place">
    <div class="where">Los Angeles, California <svg width="18" height="14" viewBox="0 0 18 14" aria-hidden="true" style="vertical-align:-2px;margin-left:6px"><rect width="18" height="14" rx="2" fill="#fff"/><path fill="#15161b" d="M0 0h3v3.5H0zM6 0h3v3.5H6zM12 0h3v3.5h-3zM3 3.5h3V7H3zM9 3.5h3V7H9zM15 3.5h3V7h-3zM0 7h3v3.5H0zM6 7h3v3.5H6zM12 7h3v3.5h-3zM3 10.5h3V14H3zM9 10.5h3V14H9zM15 10.5h3V14h-3z"/></svg></div>
    <h2>Midnight Club<br>Los Angeles</h2>
  </div>
  <div class="track"><img src="${BASE}/assets/mcla-logo.png" alt="Midnight Club: Los Angeles" width="760" height="175"></div>
  <div class="bottomleft">
    <div class="chips"><div class="avatars">${avatars}</div><a class="all" href="#lobbies">All</a></div>
    <h1 class="name">${name}</h1>${account ? `<div class="lvl"><b>LVL ${progressOf(account).level}</b><span>${h(progressOf(account).title)} · ${progressOf(account).xp} XP</span></div>` : ''}
    <div class="ctas">${ctas}</div>
  </div>
  <div class="points">
    <div class="label">Live right now <svg width="18" height="18" viewBox="0 0 18 18" fill="none" aria-hidden="true"><circle cx="9" cy="9" r="7.5" stroke="currentColor" stroke-width="1.2"/><path d="M9 8v5M9 5.5v.5" stroke="currentColor" stroke-width="1.3" stroke-linecap="round"/></svg></div>
    <div class="n">${st.online}</div>
    <div class="n">${st.sessions.length}</div>
    <div class="n gold">${st.accounts}</div>
    <div class="legend">in the game · open lobbies · racers signed up</div>
  </div>
</section>
<div class="grid">
  <div class="card" id="top"><h2>Top racers</h2>
    ${topRows ? `<table class="lb"><tr><th>#</th><th>Racer</th><th>Level</th><th>Wins</th><th>XP</th></tr>${topRows}</table>` : `<p class="muted">Nobody has raced yet. A race finished online is ${XP_FINISH} XP, a win ${XP_WIN} XP on top.</p>`}
    <p class="muted" style="margin:14px 0 0;font-weight:300"><a href="${BASE}/leaderboard">The whole leaderboard</a>. A race finished online is ${XP_FINISH} XP, a win ${XP_WIN} XP on top; level 2 takes 200 XP, and each level after it 100 XP more than the one before.</p>
  </div>
  <div class="card" id="play"><h2>How to play online</h2>
    <ol class="steps">
      <li>Install the recompilation with the online patch (<a href="${h(PATCH_URL)}">download</a>) and run the game once so it builds.</li>
      <li><a href="${BASE}/register">Create an account</a>. Your gamertag is the name other racers see.</li>
      <li>Open your <a href="${BASE}/account">account page</a>, copy the lines into <code>mcla.toml</code> next to <code>mcla.exe</code> (the launcher's <i>More › Open the game folder</i>), and start the game.</li>
      <li>In the pause menu, open the online tab. It finds the open lobbies here, or hosts one when there is none.</li>
    </ol>
    <p class="muted" style="margin:14px 0 0;font-weight:300">Hosting a lobby across the internet needs UDP port 1000 forwarded to your PC on your router; joining usually works without it. Players on one LAN need nothing.</p>
  </div>
  <div class="card" id="lobbies"><h2>Open lobbies</h2>
    <table><tr><th>Host</th><th>State</th><th>Racers</th><th>Since</th></tr>${sessionRows}</table>
    <h3>In the game now</h3><div class="racers">${playerList}</div>
  </div>
</div>
<p class="muted" style="margin:26px 0 0;font-size:13px;font-weight:300">The game reaches this service at <code>${h(url)}</code>.</p>`, account);
}

function registerPage(req, values, error) {
  const h = escapeHtml;
  const v = values || {};
  return layout(req, 'Create account', `
<div class="narrow card"><h2>Create your account</h2>
<p class="muted">The gamertag is what other racers see in lobbies and races: 1 to 15 letters, digits and spaces.</p>
${error ? `<div class="error">${h(error)}</div>` : ''}
<form method="post" action="${BASE}/register">
<label for="gamertag">Gamertag</label><input id="gamertag" name="gamertag" maxlength="15" required autofocus value="${h(v.gamertag || '')}" autocomplete="username">
<label for="email">Email <span class="muted">(optional, for logging in if you forget the gamertag)</span></label><input id="email" name="email" type="email" value="${h(v.email || '')}" autocomplete="email">
<label for="password">Password <span class="muted">(8 characters or more)</span></label><input id="password" name="password" type="password" minlength="8" required autocomplete="new-password">
<label for="password2">Password again</label><input id="password2" name="password2" type="password" minlength="8" required autocomplete="new-password">
<p style="margin-top:20px"><button class="primary" type="submit">Create account</button></p>
</form>
<p class="muted">Already have one? <a href="${BASE}/login">Log in</a>.</p></div>`, null);
}

function loginPage(req, values, error) {
  const h = escapeHtml;
  const v = values || {};
  return layout(req, 'Log in', `
<div class="narrow card"><h2>Log in</h2>
${error ? `<div class="error">${h(error)}</div>` : ''}
<form method="post" action="${BASE}/login">
<label for="login">Gamertag or email</label><input id="login" name="login" required autofocus value="${h(v.login || '')}" autocomplete="username">
<label for="password">Password</label><input id="password" name="password" type="password" required autocomplete="current-password">
<p style="margin-top:20px"><button class="primary" type="submit">Log in</button></p>
</form>
<p class="muted">New here? <a href="${BASE}/register">Create an account</a>.</p></div>`, null);
}

function accountPage(req, account, notice, error) {
  const h = escapeHtml;
  const url = siteUrl(req);
  const online = !!state.data.players[account.xuid];
  const pr = progressOf(account);
  const isDefault = url.toLowerCase() === GAME_DEFAULT_SERVER;
  const snippet = isDefault
    ? `mcla_online = true\nmcla_online_key = "${account.gameKey}"`
    : `mcla_online = true\nmcla_online_server = "${url}"\nmcla_online_key = "${account.gameKey}"`;
  const historyRows = account.history.slice().reverse().map((r) =>
    `<tr><td class="muted">${h(String(r.at).replace('T', ' ').slice(0, 16))} UTC</td><td>${r.result === 'won' ? '<b class="gold">Won</b>' : 'Finished'}</td><td>+${r.xp} XP</td></tr>`).join('');
  return layout(req, 'Account', `
<div class="grid">
<div class="card"><h2>${h(account.gamertag)} <span class="pill ${online ? 'on' : ''}">${online ? 'in the game now' : 'not in the game'}</span></h2>
<div class="progress">
  <div class="lvl big"><b>LVL ${pr.level}</b><span>${h(pr.title)}</span></div>
  <div class="xp"><div class="xpbar"><div style="width:${pr.pct}%"></div></div>
  <div class="xptext">${pr.xp} XP${pr.toNext ? ` · ${pr.toNext} more for level ${pr.level + 1}` : ' · the top level'} · ${account.racesWon} win${account.racesWon === 1 ? '' : 's'} in ${account.racesFinished} race${account.racesFinished === 1 ? '' : 's'}</div></div>
</div>
<table>
<tr><th>Gamertag</th><td>${h(account.gamertag)}</td></tr>
<tr><th>Online XUID</th><td><code>${h(account.xuid)}</code></td></tr>
<tr><th>Email</th><td>${account.email ? h(account.email) : '<span class="muted">none</span>'}</td></tr>
<tr><th>Member since</th><td>${h(account.createdAt.slice(0, 10))}</td></tr>
<tr><th>Last in the game</th><td>${account.lastSeen ? h(account.lastSeen.replace('T', ' ').slice(0, 16)) + ' UTC' : '<span class="muted">never</span>'}</td></tr>
</table>
${notice ? `<div class="ok">${h(notice)}</div>` : ''}${error ? `<div class="error">${h(error)}</div>` : ''}
<h3>Recent races</h3>
${historyRows ? `<table><tr><th>When</th><th>Result</th><th>XP</th></tr>${historyRows}</table>` : '<p class="muted">No race yet. Finish one online and it shows here with its XP.</p>'}
<h3>Your game key</h3>
<p class="muted">The key signs your game in. Treat it like a password: anyone with it plays as you. Make a new one if it leaks; the old one stops working at once.</p>
<p class="key" id="key">${h(account.gameKey)}</p>
<div class="row"><button class="secondary" type="button" onclick="navigator.clipboard&&navigator.clipboard.writeText(document.getElementById('key').textContent).then(()=>{this.textContent='Copied'})">Copy key</button>
<form method="post" action="${BASE}/account/key" class="inline" onsubmit="return confirm('Make a new key? The game must be given the new one.')"><button class="secondary" type="submit">Make a new key</button></form></div>
</div>
<div class="card"><h2>Put it in the game</h2>
<p>Add these lines to <code>mcla.toml</code>, next to <code>mcla.exe</code> (in the launcher: <i>More › Open the game folder</i>), or paste them as options under <i>Advanced › More mcla.exe options</i> with <code>--</code> in front of each. Then start the game.${isDefault ? ' The game already points at this server, so no address is needed.' : ''}</p>
<pre id="snippet">${h(snippet)}</pre>
<div class="row"><button class="secondary" type="button" onclick="navigator.clipboard&&navigator.clipboard.writeText(document.getElementById('snippet').textContent).then(()=>{this.textContent='Copied'})">Copy the lines</button></div>
<p class="muted">The game's log says <code>online: signed in to ${h(url)} as ${h(account.gamertag)}</code> when it worked. If it says the key was refused, make a new key here and copy it again.</p>
<h3>Change password</h3>
<form method="post" action="${BASE}/account/password">
<label for="current">Current password</label><input id="current" name="current" type="password" required autocomplete="current-password">
<label for="password">New password</label><input id="password" name="password" type="password" minlength="8" required autocomplete="new-password">
<label for="password2">New password again</label><input id="password2" name="password2" type="password" minlength="8" required autocomplete="new-password">
<p style="margin-top:16px"><button class="secondary" type="submit">Change password</button></p>
</form></div>
</div>`, account);
}

function leaderboardPage(req, account) {
  const h = escapeHtml;
  const rows = leaderboard(100).map((r) => `<tr><td class="muted">${r.rank}</td><td>${h(r.name)}</td><td><b class="gold">${r.level}</b> <span class="muted">${h(r.title)}</span></td><td>${r.wins}</td><td>${r.races}</td><td>${r.xp}</td></tr>`).join('');
  return layout(req, 'Leaderboard', `
<div class="card"><h2>Leaderboard</h2>
<p class="muted" style="font-weight:300">A race finished online is ${XP_FINISH} XP, a win ${XP_WIN} XP on top. Level 2 takes 200 XP, and each level after it 100 XP more than the one before. The titles: Rookie, Street Racer at level 5, Pro at 10, Elite at 20, Legend at 35, Midnight Club at 50.</p>
${rows ? `<table class="lb"><tr><th>#</th><th>Racer</th><th>Level</th><th>Wins</th><th>Races</th><th>XP</th></tr>${rows}</table>` : '<p class="muted">Nobody has raced yet.</p>'}
</div>`, account);
}

function errorPage(req, status, text, account) {
  return layout(req, `${status}`, `<div class="narrow card"><h2>${status}</h2><p>${escapeHtml(text)}</p><p><a href="${BASE}/">Home</a></p></div>`, account);
}

// --- Routing: the website ----------------------------------------------------------------

async function handleSite(req, res, parts, method, ip) {
  const account = accountFromCookie(req);
  const first = parts[0] || '';

  if (parts.length === 0) {
    return send(res, 200, homePage(req, account), 'text/html; charset=utf-8');
  }
  if (first === 'status' && method === 'GET') {
    return send(res, 200, publicStatus());
  }
  if (first === 'leaderboard' && method === 'GET') {
    return send(res, 200, leaderboardPage(req, account), 'text/html; charset=utf-8');
  }
  if (first === 'register') {
    if (method === 'GET') return send(res, account ? 303 : 200, account ? '' : registerPage(req), 'text/html; charset=utf-8', account ? { Location: `${BASE}/account` } : {});
    if (method === 'POST') {
      if (!sameOrigin(req)) return send(res, 403, errorPage(req, 403, 'The form was sent from another site.'), 'text/html; charset=utf-8');
      if (rateLimited(ip, 'register', 5, 60 * 60 * 1000)) return send(res, 429, registerPage(req, {}, 'Too many accounts from this address; try again later.'), 'text/html; charset=utf-8');
      const f = await readForm(req);
      const bad = validateGamertag(f.gamertag);
      if (bad) return send(res, 400, registerPage(req, f, bad), 'text/html; charset=utf-8');
      if (String(f.password || '').length < 8) return send(res, 400, registerPage(req, f, 'The password needs 8 characters or more.'), 'text/html; charset=utf-8');
      if (f.password !== f.password2) return send(res, 400, registerPage(req, f, 'The two passwords differ.'), 'text/html; charset=utf-8');
      if (f.email && !/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(f.email)) return send(res, 400, registerPage(req, f, 'That is not an email address.'), 'text/html; charset=utf-8');
      if (accountByGamertag(f.gamertag)) return send(res, 400, registerPage(req, f, 'That gamertag is taken.'), 'text/html; charset=utf-8');
      if (f.email && accountByEmail(f.email)) return send(res, 400, registerPage(req, f, 'An account already uses that email.'), 'text/html; charset=utf-8');
      const created = createAccount(f.gamertag, f.email || '', f.password);
      log(`account created: ${created.gamertag} (#${created.id}, XUID ${created.xuid}) from ${ip}`);
      return redirect(res, '/account', { 'Set-Cookie': sessionCookieHeader(makeCookie(created), req) });
    }
  }
  if (first === 'login') {
    if (method === 'GET') return send(res, account ? 303 : 200, account ? '' : loginPage(req), 'text/html; charset=utf-8', account ? { Location: `${BASE}/account` } : {});
    if (method === 'POST') {
      if (!sameOrigin(req)) return send(res, 403, errorPage(req, 403, 'The form was sent from another site.'), 'text/html; charset=utf-8');
      if (rateLimited(ip, 'login', 20, 10 * 60 * 1000)) return send(res, 429, loginPage(req, {}, 'Too many tries from this address; wait a few minutes.'), 'text/html; charset=utf-8');
      const f = await readForm(req);
      const found = accountByGamertag(f.login) || accountByEmail(f.login);
      if (!found || !verifyPassword(String(f.password || ''), found.password)) {
        return send(res, 401, loginPage(req, { login: f.login }, 'Wrong gamertag, email or password.'), 'text/html; charset=utf-8');
      }
      return redirect(res, '/account', { 'Set-Cookie': sessionCookieHeader(makeCookie(found), req) });
    }
  }
  if (first === 'logout' && method === 'POST') {
    return redirect(res, '/', { 'Set-Cookie': sessionCookieHeader('', req) });
  }
  if (first === 'account') {
    if (!account) return redirect(res, '/login');
    if (parts.length === 1 && method === 'GET') return send(res, 200, accountPage(req, account), 'text/html; charset=utf-8');
    if (method === 'POST' && !sameOrigin(req)) return send(res, 403, errorPage(req, 403, 'The form was sent from another site.', account), 'text/html; charset=utf-8');
    if (parts[1] === 'key' && method === 'POST') {
      account.gameKey = newGameKey();
      accounts.save();
      log(`account ${account.gamertag}: new game key`);
      return send(res, 200, accountPage(req, account, 'A new key was made. Put it in mcla.toml; the old one no longer works.'), 'text/html; charset=utf-8');
    }
    if (parts[1] === 'password' && method === 'POST') {
      const f = await readForm(req);
      if (!verifyPassword(String(f.current || ''), account.password)) return send(res, 400, accountPage(req, account, null, 'The current password is wrong.'), 'text/html; charset=utf-8');
      if (String(f.password || '').length < 8) return send(res, 400, accountPage(req, account, null, 'The new password needs 8 characters or more.'), 'text/html; charset=utf-8');
      if (f.password !== f.password2) return send(res, 400, accountPage(req, account, null, 'The two new passwords differ.'), 'text/html; charset=utf-8');
      account.password = hashPassword(f.password);
      accounts.save();
      return send(res, 200, accountPage(req, account, 'The password was changed.'), 'text/html; charset=utf-8');
    }
  }
  return send(res, 404, errorPage(req, 404, 'There is nothing at this address.', account), 'text/html; charset=utf-8');
}

// --- Routing: the game's API ----------------------------------------------------------------

function gameAccount(req) {
  const auth = String(req.headers.authorization || '');
  const m = /^Bearer\s+([0-9A-Fa-f]{20,64})\s*$/.exec(auth);
  return m ? accountByKey(m[1]) : null;
}

async function handleApi(req, res, parts, method, ip) {
  const account = gameAccount(req);
  if (!account) {
    return send(res, 401, {
      error: 'account key required',
      website: siteUrl(req),
      hint: 'Create an account on the website and put its game key in mcla.toml as mcla_online_key.',
    });
  }
  const body = method === 'GET' || method === 'DELETE' ? {} : await readJson(req);
  const players = state.data.players;
  const sessions = state.data.sessions;

  if (parts[0] === 'players') {
    if (parts.length === 1 && method === 'POST') {
      const player = upsertPlayer(account, body, ip);
      accounts.save();
      state.save();
      log(`sign-in: ${account.gamertag} (${account.xuid}) from ${player.lanIp} / ${ip}, game port ${player.port || '?'}`);
      return send(res, 201, { ok: true, publicIp: ip, xuid: account.xuid, name: account.gamertag, player: playerJson(player) });
    }
    if (parts.length === 3 && parts[2] === 'heartbeat' && method === 'POST') {
      upsertPlayer(account, body, ip);
      state.save();
      return send(res, 200, { ok: true, publicIp: ip, xuid: account.xuid, name: account.gamertag });
    }
    const xuid = normId(parts[1]);
    if (!xuid) return send(res, 404, { error: 'no such player' });
    if (parts.length === 2 && method === 'GET') {
      const p = players[xuid];
      return p ? send(res, 200, playerJson(p)) : send(res, 404, { error: 'no such player' });
    }
    if (parts.length === 2 && method === 'DELETE') {
      if (xuid !== account.xuid) return send(res, 403, { error: 'not your player' });
      log(`sign-out: ${account.gamertag} (${xuid})`);
      delete players[xuid];
      for (const [id, s] of Object.entries(sessions)) if (s.hostXuid === xuid) delete sessions[id];
      state.save();
      return send(res, 200, { ok: true });
    }
    return send(res, 405, { error: 'method not allowed' });
  }

  if (parts[0] === 'sessions') {
    if (parts.length === 1 && method === 'GET') {
      return send(res, 200, Object.values(sessions).map((s) => sessionJson(s, false)));
    }
    if (parts.length === 1 && method === 'POST') {
      const session = createSession(account, body, ip);
      if (!session) return send(res, 400, { error: 'id required' });
      state.save();
      log(`session ${session.id}: created by ${account.gamertag} (${session.publicSlots} public + ${session.privateSlots} private slots, flags 0x${session.flags.toString(16)})`);
      return send(res, 201, sessionJson(session, false));
    }
    if (parts.length === 2 && parts[1] === 'search' && method === 'POST') {
      const results = searchSessions(account, body);
      log(`search by ${account.gamertag}: ${results.length} of ${Object.keys(sessions).length} session(s)`);
      return send(res, 200, results);
    }
    const id = normId(parts[1]);
    const session = id ? sessions[id] : null;
    if (parts.length === 3 && parts[2] === 'migrate' && method === 'POST') {
      if (session) delete sessions[id];
      const moved = createSession(account, body, ip);
      if (!moved) return send(res, 400, { error: 'id required' });
      state.save();
      log(`session ${id} migrated to ${moved.id}, hosted by ${account.gamertag}`);
      return send(res, 201, sessionJson(moved, false));
    }
    if (!session) return send(res, 404, { error: 'no such session' });
    if (parts.length === 2 && method === 'GET') return send(res, 200, sessionJson(session, true));
    if (parts.length === 3 && parts[2] === 'qos' && method === 'GET') return send(res, 200, { qos: session.qos || '' });
    // The rest changes the session: its host only.
    if (session.hostXuid !== account.xuid) return send(res, 403, { error: 'not your session' });
    if (parts.length === 2) {
      if (method === 'PUT') {
        updateSession(session, body, account, ip);
        state.save();
        trace(`session ${id}: updated (state ${session.state}, ${session.members.length} members)`);
        return send(res, 200, sessionJson(session, false));
      }
      if (method === 'DELETE') {
        delete sessions[id];
        state.save();
        log(`session ${id}: deleted by its host`);
        return send(res, 200, { ok: true });
      }
    }
    if (parts.length === 3 && method === 'POST' && parts[2] === 'join') {
      session.members = cleanMembers(body.members);
      session.lastSeen = Date.now();
      state.save();
      log(`session ${id}: ${session.members.length} member(s)`);
      return send(res, 200, sessionJson(session, false));
    }
    if (parts.length === 3 && method === 'POST' && parts[2] === 'leave') {
      const leaving = new Set((Array.isArray(body.xuids) ? body.xuids : []).map(normId));
      session.members = session.members.filter((m) => !leaving.has(m.xuid));
      session.lastSeen = Date.now();
      state.save();
      return send(res, 200, sessionJson(session, false));
    }
    if (parts.length === 3 && parts[2] === 'qos' && method === 'PUT') {
      session.qos = typeof body.qos === 'string' ? body.qos.slice(0, 16384) : '';
      session.lastSeen = Date.now();
      state.save();
      trace(`session ${id}: QoS data, ${Buffer.from(session.qos, 'base64').length} bytes`);
      return send(res, 200, { ok: true });
    }
    return send(res, 405, { error: 'method not allowed' });
  }

  if (parts[0] === 'stats') {
    if (method === 'POST') {
      const sessionId = normId(body.sessionId);
      const targetXuid = normId(body.xuid) || account.xuid;
      const views = Array.isArray(body.views) ? body.views : [];
      state.data.stats.push({ at: Date.now(), by: account.xuid, xuid: targetXuid, sessionId, views });
      if (state.data.stats.length > 500) state.data.stats.splice(0, state.data.stats.length - 500);
      // Every write goes to the log in full: the first real race shows which
      // property carries the finishing position (MCLA_WIN_PROPERTY).
      const summary = views.map((v) => `view 0x${(Number(v.viewId) >>> 0).toString(16)}: ` +
        (Array.isArray(v.properties) ? v.properties.map((p) => `0x${(Number(p.id) >>> 0).toString(16)}=${propertyNumber(p) ?? String(p.data || '').slice(0, 32)}`).join(' ') : '')).join(' | ');
      log(`stats by ${account.gamertag} for ${targetXuid === account.xuid ? 'self' : targetXuid} in session ${sessionId || '?'}: ${summary || '(no views)'}`);

      // XP. A player's write for themselves, or the host's write for a member
      // of their session (how the Xbox reported arbitrated races), for a
      // session the server knows and the player is in.
      const target = findAccount((a) => a.xuid === targetXuid);
      const session = sessionId ? state.data.sessions[sessionId] : null;
      const isSelf = targetXuid === account.xuid;
      const isHostWrite = !!session && session.hostXuid === account.xuid;
      const inSession = !!session && (session.hostXuid === targetXuid || session.members.some((m) => m.xuid === targetXuid));
      const awards = [];
      if (!target) log(`no XP: no account has XUID ${targetXuid}`);
      else if (!session) log(`no XP for ${target.gamertag}: session ${sessionId || '?'} is not one the server knows`);
      else if (!inSession) log(`no XP for ${target.gamertag}: not in session ${sessionId}`);
      else if (!(isSelf || isHostWrite)) log(`no XP for ${target.gamertag}: ${account.gamertag} is neither them nor the host`);
      else if (!views.length) log(`no XP for ${target.gamertag}: the write has no views`);
      else {
        if (awardXp(target, `finish:${sessionId}`, XP_FINISH, 'finished', sessionId)) awards.push({ result: 'finished', xp: XP_FINISH });
        const won = WIN_PROPERTY !== 0 && views.some((v) => Array.isArray(v.properties) &&
          v.properties.some((p) => (Number(p.id) >>> 0) === WIN_PROPERTY && propertyNumber(p) === WIN_VALUE));
        if (won && awardXp(target, `win:${sessionId}`, XP_WIN, 'won', sessionId)) awards.push({ result: 'won', xp: XP_WIN });
        if (awards.length) accounts.save();
      }
      state.save();
      return send(res, 200, { ok: true, awards, progress: target ? progressOf(target) : null });
    }
    if (method === 'GET') return send(res, 200, state.data.stats.slice(-100));
  }
  return send(res, 404, { error: 'not found' });
}

const API_ROOTS = new Set(['players', 'sessions', 'stats']);

// Static files from the assets folder next to the script (the background
// photo): by plain name only, so nothing outside the folder can be asked for.
const ASSETS_DIR = path.join(__dirname, 'assets');
const ASSET_TYPES = { '.webp': 'image/webp', '.jpg': 'image/jpeg', '.jpeg': 'image/jpeg', '.png': 'image/png', '.svg': 'image/svg+xml', '.ico': 'image/x-icon' };
function sendAsset(req, res, name) {
  if (!/^[A-Za-z0-9_-]+\.[A-Za-z0-9]+$/.test(name)) return send(res, 404, { error: 'not found' });
  const type = ASSET_TYPES[path.extname(name).toLowerCase()];
  const file = path.join(ASSETS_DIR, name);
  let st;
  try { st = fs.statSync(file); } catch (e) { st = null; }
  if (!type || !st || !st.isFile()) return send(res, 404, { error: 'not found' });
  const etag = `"${st.size}-${Math.floor(st.mtimeMs)}"`;
  if (req.headers['if-none-match'] === etag) { res.writeHead(304); return res.end(); }
  res.writeHead(200, { 'Content-Type': type, 'Content-Length': st.size, 'Cache-Control': 'public, max-age=86400', 'ETag': etag });
  fs.createReadStream(file).pipe(res);
}

async function handle(req, res) {
  refreshAll();
  if (expire()) state.save();

  let pathname = new URL(req.url, 'http://localhost').pathname;
  if (BASE && pathname.startsWith(BASE)) pathname = pathname.slice(BASE.length) || '/';
  const parts = pathname.split('/').filter(Boolean);
  const method = req.method;
  const ip = clientIp(req);
  trace(`${method} ${pathname} from ${ip}`);

  if (parts[0] === 'assets' && parts.length === 2 && method === 'GET') {
    return sendAsset(req, res, parts[1]);
  }
  if (parts.length && API_ROOTS.has(parts[0])) {
    return handleApi(req, res, parts, method, ip);
  }
  return handleSite(req, res, parts, method, ip);
}

const server = http.createServer((req, res) => {
  handle(req, res).catch((e) => {
    log(`error on ${req.method} ${req.url}: ${e.message}`);
    try { send(res, 400, { error: e.message }); } catch (_) { /* closed */ }
  });
});
server.on('error', (e) => {
  console.error(`cannot listen on ${BIND}:${PORT}: ${e.message}`);
  process.exit(1);
});
server.listen(PORT, BIND, () => {
  log(`${SITE_NAME}: listening on ${BIND}:${PORT}${BASE ? ` under ${BASE}` : ''}${TRUST_PROXY ? ', behind a proxy' : ''}`);
  log(`data in ${DATA_DIR}: ${accounts.data.accounts.length} account(s)`);
  if (!HOSTED) log(`the website is at http://localhost:${PORT}${BASE}/`);
});
