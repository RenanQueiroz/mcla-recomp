#!/usr/bin/env node
// The private multiplayer server for the Midnight Club: Los Angeles
// recompilation (mcla_online). One player runs it; every player's game points
// at it (mcla_online_server = host:port). It keeps who is online and which
// sessions (lobbies) exist, and answers the game's matchmaking searches. The
// races themselves run between the players' PCs over UDP, not through here.
//
//   node mcla-server.js [--port 36000] [--bind 0.0.0.0] [--verbose]
//
// Plain Node.js (18 or newer), no packages. Everything is kept in memory:
// restarting the server empties it, and the games sign in again by themselves.
//
// The API, all JSON (server/README.md has the details):
//   GET    /                          a status page (a browser shows it)
//   GET    /status                    the same as JSON
//   POST   /players                   sign in: {xuid, name, mac, machineId, lanIp, port, titleId}
//   POST   /players/:xuid/heartbeat   every 15 s, the same body
//   GET    /players/:xuid
//   DELETE /players/:xuid             sign out
//   POST   /sessions                  a host publishes a session
//   GET    /sessions                  every session (debugging)
//   POST   /sessions/search           {titleId, numResults, contexts:[{id,value}], excludeXuid}
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

const http = require('http');

const args = process.argv.slice(2);
function argValue(name, fallback) {
  const i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
}
const PORT = parseInt(argValue('--port', process.env.MCLA_SERVER_PORT || '36000'), 10);
const BIND = argValue('--bind', process.env.MCLA_SERVER_BIND || '0.0.0.0');
const VERBOSE = args.includes('--verbose');

const PLAYER_TIMEOUT_MS = 60 * 1000;   // no heartbeat for this long: gone
const SESSION_TIMEOUT_MS = 90 * 1000;  // a session whose host is gone
const XCONTEXT_PRESENCE = 0x8001;

const players = new Map();   // xuid -> player
const sessions = new Map();  // id -> session
const stats = [];            // the last writes, newest last

function log(...parts) {
  console.log(new Date().toISOString().slice(11, 19), ...parts);
}
function trace(...parts) {
  if (VERBOSE) log(...parts);
}

// Hexadecimal ids are compared upper case without a 0x.
function normId(s) {
  if (typeof s !== 'string') return '';
  s = s.trim().toUpperCase();
  if (s.startsWith('0X')) s = s.slice(2);
  return /^[0-9A-F]{1,16}$/.test(s) ? s.padStart(16, '0') : '';
}

function clientIp(req) {
  let ip = req.socket.remoteAddress || '';
  if (ip.startsWith('::ffff:')) ip = ip.slice(7);
  if (ip === '::1') ip = '127.0.0.1';
  return ip;
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    const chunks = [];
    let size = 0;
    req.on('data', (c) => {
      size += c.length;
      if (size > 4 * 1024 * 1024) {
        reject(new Error('body too large'));
        req.destroy();
        return;
      }
      chunks.push(c);
    });
    req.on('end', () => {
      const text = Buffer.concat(chunks).toString('utf8');
      if (!text.trim()) return resolve({});
      try {
        resolve(JSON.parse(text));
      } catch (e) {
        reject(new Error('bad JSON: ' + e.message));
      }
    });
    req.on('error', reject);
  });
}

function send(res, status, body, type) {
  const text = typeof body === 'string' ? body : JSON.stringify(body);
  res.writeHead(status, {
    'Content-Type': type || 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(text),
    'Connection': 'close',
    'Cache-Control': 'no-store',
  });
  res.end(text);
}

// --- Players --------------------------------------------------------------------

function upsertPlayer(body, ip) {
  const xuid = normId(body.xuid);
  if (!xuid) return null;
  const existing = players.get(xuid);
  const player = existing || { xuid, firstSeen: Date.now() };
  player.name = String(body.name || player.name || 'Player').slice(0, 15);
  player.mac = String(body.mac || player.mac || '').toUpperCase().slice(0, 12);
  player.machineId = normId(body.machineId) || player.machineId || ('FA' + xuid.slice(2));
  player.lanIp = String(body.lanIp || player.lanIp || ip);
  player.publicIp = ip;
  player.port = Number(body.port) || player.port || 0;
  player.titleId = Number(body.titleId) || player.titleId || 0;
  player.lastSeen = Date.now();
  players.set(xuid, player);
  // The sessions this player hosts stay alive with the player.
  for (const s of sessions.values()) {
    if (s.hostXuid === xuid) {
      s.lastSeen = player.lastSeen;
      s.host.publicIp = ip;
      if (player.port) s.host.port = player.port;
      if (player.lanIp) s.host.lanIp = player.lanIp;
    }
  }
  return player;
}

function playerJson(p) {
  return {
    xuid: p.xuid, name: p.name, mac: p.mac, machineId: p.machineId,
    lanIp: p.lanIp, publicIp: p.publicIp, port: p.port, titleId: p.titleId,
    lastSeen: new Date(p.lastSeen).toISOString(),
  };
}

// --- Sessions -------------------------------------------------------------------

function hostFromBody(body, player, ip) {
  const h = body.host || {};
  return {
    xuid: normId(h.xuid) || (player && player.xuid) || '',
    name: String(h.name || (player && player.name) || body.hostName || '').slice(0, 15),
    mac: String(h.mac || (player && player.mac) || '').toUpperCase().slice(0, 12),
    machineId: normId(h.machineId) || (player && player.machineId) || '',
    lanIp: String(h.lanIp || (player && player.lanIp) || ip),
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

function createSession(body, ip) {
  const id = normId(body.id);
  if (!id) return null;
  const hostXuid = normId(body.hostXuid);
  const player = players.get(hostXuid);
  const session = {
    id,
    titleId: Number(body.titleId) || (player && player.titleId) || 0,
    hostXuid,
    hostName: String(body.hostName || (player && player.name) || '').slice(0, 15),
    flags: Number(body.flags) >>> 0,
    publicSlots: Number(body.publicSlots) || 0,
    privateSlots: Number(body.privateSlots) || 0,
    state: Number(body.state) || 0,
    nonce: normId(body.nonce) || '0000000000000000',
    key: String(body.key || '').toUpperCase().slice(0, 32),
    host: hostFromBody(body, player, ip),
    contexts: cleanContexts(body.contexts),
    properties: cleanProperties(body.properties),
    members: cleanMembers(body.members),
    qos: typeof body.qos === 'string' ? body.qos : '',
    created: Date.now(),
    lastSeen: Date.now(),
  };
  sessions.set(id, session);
  return session;
}

function updateSession(session, body, ip) {
  if (body.flags !== undefined) session.flags = Number(body.flags) >>> 0;
  if (body.publicSlots !== undefined) session.publicSlots = Number(body.publicSlots) || 0;
  if (body.privateSlots !== undefined) session.privateSlots = Number(body.privateSlots) || 0;
  if (body.state !== undefined) session.state = Number(body.state) || 0;
  if (body.members !== undefined) session.members = cleanMembers(body.members);
  if (body.contexts !== undefined) session.contexts = cleanContexts(body.contexts);
  if (body.properties !== undefined) session.properties = cleanProperties(body.properties);
  if (body.host !== undefined) session.host = hostFromBody(body, players.get(session.hostXuid), ip);
  if (typeof body.qos === 'string') session.qos = body.qos;
  session.lastSeen = Date.now();
}

function slotCounts(s) {
  const filledPrivate = s.members.filter((m) => m.private).length;
  const filledPublic = s.members.length - filledPrivate;
  return {
    filledPublic,
    filledPrivate,
    openPublic: Math.max(0, s.publicSlots - filledPublic),
    openPrivate: Math.max(0, s.privateSlots - filledPrivate),
  };
}

function sessionJson(s, withQos) {
  const slots = slotCounts(s);
  const j = {
    id: s.id, titleId: s.titleId, hostXuid: s.hostXuid, hostName: s.hostName,
    flags: s.flags, publicSlots: s.publicSlots, privateSlots: s.privateSlots,
    ...slots, state: s.state, nonce: s.nonce, key: s.key, host: s.host,
    contexts: s.contexts, properties: s.properties, members: s.members,
    created: new Date(s.created).toISOString(),
  };
  if (withQos) j.qos = s.qos;
  return j;
}

// A search matches sessions of the title with open public slots, not in a race
// when the host forbids joining one, whose contexts agree with the search's
// on every context both sides name (the presence context aside).
function searchSessions(body) {
  const titleId = Number(body.titleId) || 0;
  const exclude = normId(body.excludeXuid);
  const wanted = cleanContexts(body.contexts).filter((c) => c.id !== XCONTEXT_PRESENCE);
  const max = Math.max(1, Math.min(Number(body.numResults) || 50, 200));
  const JOIN_IN_PROGRESS_DISABLED = 0x400;
  const results = [];
  for (const s of sessions.values()) {
    if (titleId && s.titleId && s.titleId !== titleId) continue;
    if (exclude && s.hostXuid === exclude) continue;
    if (s.state === 4) continue;  // deleted
    if (s.state === 2 && (s.flags & JOIN_IN_PROGRESS_DISABLED)) continue;
    if (slotCounts(s).openPublic === 0) continue;
    let ok = true;
    for (const want of wanted) {
      const have = s.contexts.find((c) => c.id === want.id);
      if (have && have.value !== want.value) { ok = false; break; }
      if (!have) { ok = false; break; }
    }
    if (!ok) continue;
    results.push(s);
    if (results.length >= max) break;
  }
  // Oldest lobbies first: they have waited longest.
  results.sort((a, b) => a.created - b.created);
  return results.map((s) => sessionJson(s, true));
}

function expire() {
  const now = Date.now();
  for (const [xuid, p] of players) {
    if (now - p.lastSeen > PLAYER_TIMEOUT_MS) {
      players.delete(xuid);
      log(`player ${p.name} (${xuid}) timed out`);
    }
  }
  for (const [id, s] of sessions) {
    if (now - s.lastSeen > SESSION_TIMEOUT_MS) {
      sessions.delete(id);
      log(`session ${id} of ${s.hostName || s.hostXuid} timed out`);
    }
  }
}
setInterval(expire, 5000).unref();

// --- The status page --------------------------------------------------------------

function escapeHtml(s) {
  return String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
}

function statusJson() {
  return {
    server: 'mcla private server',
    uptimeSeconds: Math.floor(process.uptime()),
    players: [...players.values()].map(playerJson),
    sessions: [...sessions.values()].map((s) => sessionJson(s, false)),
  };
}

function statusHtml() {
  const st = statusJson();
  const rows = (items, cols) =>
    items.length
      ? items.map((it) => '<tr>' + cols.map((c) => `<td>${escapeHtml(c(it))}</td>`).join('') + '</tr>').join('')
      : `<tr><td colspan="${cols.length}"><i>none</i></td></tr>`;
  const flagNames = (f) => {
    const names = [];
    if (f & 0x01) names.push('host');
    if (f & 0x02) names.push('presence');
    if (f & 0x04) names.push('stats');
    if (f & 0x08) names.push('matchmaking');
    if (f & 0x10) names.push('arbitration');
    if (f & 0x20) names.push('peer network');
    if (f & 0x400) names.push('no join in progress');
    return names.join(', ') || '0';
  };
  const stateNames = ['lobby', 'registration', 'in game', 'reporting', 'deleted'];
  return `<!doctype html><meta charset="utf-8"><title>MCLA private server</title>
<meta http-equiv="refresh" content="5">
<style>body{font:14px system-ui,sans-serif;margin:2em;color:#222}table{border-collapse:collapse;margin:1em 0}td,th{border:1px solid #ccc;padding:4px 8px;text-align:left}th{background:#eee}code{background:#f4f4f4;padding:1px 4px}</style>
<h1>MCLA private server</h1>
<p>Up ${st.uptimeSeconds} s. ${st.players.length} player(s) signed in, ${st.sessions.length} session(s). This page refreshes every 5 seconds; <a href="/status">/status</a> is the same as JSON.</p>
<h2>Players</h2>
<table><tr><th>Name</th><th>XUID</th><th>LAN address</th><th>Public address</th><th>Game port</th><th>Last seen</th></tr>
${rows(st.players, [(p) => p.name, (p) => p.xuid, (p) => p.lanIp, (p) => p.publicIp, (p) => p.port || '-', (p) => p.lastSeen])}</table>
<h2>Sessions</h2>
<table><tr><th>Id</th><th>Host</th><th>State</th><th>Players</th><th>Slots (open/total)</th><th>Flags</th><th>Contexts</th></tr>
${rows(st.sessions, [
  (s) => s.id,
  (s) => `${s.hostName || s.hostXuid} @ ${s.host.lanIp}/${s.host.publicIp}:${s.host.port}`,
  (s) => stateNames[s.state] || s.state,
  (s) => s.members.length,
  (s) => `${s.openPublic}/${s.publicSlots} public, ${s.openPrivate}/${s.privateSlots} private`,
  (s) => flagNames(s.flags),
  (s) => s.contexts.map((c) => `0x${c.id.toString(16)}=${c.value}`).join(' '),
])}</table>
<p>Players set <code>mcla_online = true</code> and <code>mcla_online_server = "&lt;this PC&gt;:${PORT}"</code> in <code>mcla.toml</code> (or the launcher's <i>More mcla.exe options</i>). Game traffic is UDP on the game port shown above: forward it on the router for play across the internet.</p>`;
}

// --- Routing ----------------------------------------------------------------------

async function handle(req, res) {
  const url = new URL(req.url, 'http://localhost');
  const parts = url.pathname.split('/').filter(Boolean);
  const method = req.method;
  const ip = clientIp(req);
  trace(`${method} ${url.pathname} from ${ip}`);

  if (parts.length === 0) {
    return send(res, 200, statusHtml(), 'text/html; charset=utf-8');
  }
  if (parts[0] === 'status' && method === 'GET') {
    return send(res, 200, statusJson());
  }

  const body = method === 'GET' || method === 'DELETE' ? {} : await readBody(req);

  // Players.
  if (parts[0] === 'players') {
    if (parts.length === 1 && method === 'POST') {
      const player = upsertPlayer(body, ip);
      if (!player) return send(res, 400, { error: 'xuid required' });
      log(`sign-in: ${player.name} (${player.xuid}) from ${player.lanIp} / ${ip}, game port ${player.port || '?'}`);
      return send(res, 201, { ok: true, publicIp: ip, player: playerJson(player) });
    }
    const xuid = normId(parts[1]);
    if (!xuid) return send(res, 404, { error: 'no such player' });
    if (parts.length === 3 && parts[2] === 'heartbeat' && method === 'POST') {
      body.xuid = xuid;
      const player = upsertPlayer(body, ip);
      return send(res, 200, { ok: true, publicIp: ip });
    }
    const player = players.get(xuid);
    if (parts.length === 2 && method === 'GET') {
      return player ? send(res, 200, playerJson(player)) : send(res, 404, { error: 'no such player' });
    }
    if (parts.length === 2 && method === 'DELETE') {
      if (player) log(`sign-out: ${player.name} (${xuid})`);
      players.delete(xuid);
      for (const [id, s] of sessions) {
        if (s.hostXuid === xuid) sessions.delete(id);
      }
      return send(res, 200, { ok: true });
    }
    return send(res, 405, { error: 'method not allowed' });
  }

  // Sessions.
  if (parts[0] === 'sessions') {
    if (parts.length === 1 && method === 'GET') {
      return send(res, 200, [...sessions.values()].map((s) => sessionJson(s, false)));
    }
    if (parts.length === 1 && method === 'POST') {
      const session = createSession(body, ip);
      if (!session) return send(res, 400, { error: 'id required' });
      log(`session ${session.id}: created by ${session.hostName || session.hostXuid} ` +
          `(${session.publicSlots} public + ${session.privateSlots} private slots, flags 0x${session.flags.toString(16)})`);
      return send(res, 201, sessionJson(session, false));
    }
    if (parts.length === 2 && parts[1] === 'search' && method === 'POST') {
      const results = searchSessions(body);
      log(`search by ${normId(body.excludeXuid) || '?'}: ${results.length} of ${sessions.size} session(s)`);
      return send(res, 200, results);
    }
    const id = normId(parts[1]);
    const session = id ? sessions.get(id) : null;
    if (parts.length === 3 && parts[2] === 'migrate' && method === 'POST') {
      if (session) sessions.delete(id);
      const moved = createSession(body, ip);
      if (!moved) return send(res, 400, { error: 'id required' });
      log(`session ${id} migrated to ${moved.id}, hosted by ${moved.hostName || moved.hostXuid}`);
      return send(res, 201, sessionJson(moved, false));
    }
    if (!session) return send(res, 404, { error: 'no such session' });
    if (parts.length === 2) {
      if (method === 'GET') return send(res, 200, sessionJson(session, true));
      if (method === 'PUT') {
        updateSession(session, body, ip);
        trace(`session ${id}: updated (state ${session.state}, ${session.members.length} members)`);
        return send(res, 200, sessionJson(session, false));
      }
      if (method === 'DELETE') {
        sessions.delete(id);
        log(`session ${id}: deleted by its host`);
        return send(res, 200, { ok: true });
      }
    }
    if (parts.length === 3 && method === 'POST' && parts[2] === 'join') {
      session.members = cleanMembers(body.members);
      session.lastSeen = Date.now();
      log(`session ${id}: ${session.members.length} member(s)`);
      return send(res, 200, sessionJson(session, false));
    }
    if (parts.length === 3 && method === 'POST' && parts[2] === 'leave') {
      const leaving = new Set((Array.isArray(body.xuids) ? body.xuids : []).map(normId));
      session.members = session.members.filter((m) => !leaving.has(m.xuid));
      session.lastSeen = Date.now();
      return send(res, 200, sessionJson(session, false));
    }
    if (parts.length === 3 && parts[2] === 'qos') {
      if (method === 'PUT') {
        session.qos = typeof body.qos === 'string' ? body.qos : '';
        session.lastSeen = Date.now();
        trace(`session ${id}: QoS data, ${Buffer.from(session.qos, 'base64').length} bytes`);
        return send(res, 200, { ok: true });
      }
      if (method === 'GET') return send(res, 200, { qos: session.qos || '' });
    }
    return send(res, 405, { error: 'method not allowed' });
  }

  if (parts[0] === 'stats') {
    if (method === 'POST') {
      stats.push({ at: Date.now(), ...body });
      if (stats.length > 1000) stats.splice(0, stats.length - 1000);
      trace(`stats from ${normId(body.xuid)} for session ${normId(body.sessionId)}`);
      return send(res, 200, { ok: true });
    }
    if (method === 'GET') return send(res, 200, stats.slice(-100));
  }

  return send(res, 404, { error: 'not found' });
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
  log(`MCLA private server listening on ${BIND}:${PORT}`);
  log(`players set mcla_online = true and mcla_online_server = "<this PC's address>:${PORT}"`);
  log(`open http://localhost:${PORT}/ in a browser for the status page`);
});
