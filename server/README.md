# The private multiplayer server

`mcla-server.js` is the matchmaking server the game's online modes talk to when `mcla_online` is on (see [docs/online.md](../docs/online.md) for the player's side). One player runs it; every player's game is pointed at it. It keeps who is signed in and which sessions (lobbies) exist, answers the game's session searches and hands out the QoS data hosts publish. The races themselves are played between the players' PCs over UDP and never pass through the server.

## Running it

[Node.js](https://nodejs.org/) 18 or newer, nothing else:

```bash
node server/mcla-server.js
```

Options: `--port 36000` (the default), `--bind 0.0.0.0` (every interface, the default; `127.0.0.1` for this PC only), `--verbose` (every request). `MCLA_SERVER_PORT` and `MCLA_SERVER_BIND` in the environment do the same.

Open `http://localhost:36000/` in a browser: a status page lists the players and sessions and refreshes itself. `/status` is the same as JSON.

Everything lives in memory. A player who sends no heartbeat for 60 seconds is dropped, and a session whose host is gone for 90 seconds with it. Restarting the server empties it; the games sign in again on their own within a few seconds.

## Reaching it

- **On one LAN**: give the players the server PC's LAN address, `192.168.1.20:36000` for example. Nothing to forward.
- **Across the internet**: forward TCP port 36000 on the server PC's router to it, and give the players its public address. Each player must also be reachable on the game's UDP port (the status page shows the port each game bound once it has set up its network, usually 1000): forward that UDP port on each player's router, or play from the same LAN. Two players behind the same router find each other by their LAN addresses.

The server trusts whoever can reach it: run it for people you know, or behind something that limits who can connect.

## The API

JSON in and out. 64-bit ids (XUIDs, session ids, machine ids, nonces) are 16 hex digits, since JSON numbers would lose their low bits. Addresses are dotted IPv4 text; a player's public address is whatever address the server saw the request from.

| Method and path | Body | Does |
|---|---|---|
| `GET /` | | The status page (HTML). |
| `GET /status` | | Players and sessions as JSON. |
| `POST /players` | `{xuid, name, mac, machineId, lanIp, port, titleId}` | Signs a player in (or updates them). Returns `{publicIp}`, the address the server saw. |
| `POST /players/:xuid/heartbeat` | the same | Keeps the player and the sessions they host alive. Every 15 s. |
| `GET /players/:xuid` | | One player. |
| `DELETE /players/:xuid` | | Signs out; their hosted sessions go. |
| `POST /sessions` | `{id, titleId, hostXuid, hostName, flags, publicSlots, privateSlots, state, nonce, key, host:{lanIp, port, mac, machineId}, contexts:[{id,value}], properties:[{id,type,data}], members:[{xuid,private}]}` | A host publishes a session. |
| `GET /sessions` | | Every session. |
| `POST /sessions/search` | `{titleId, numResults, contexts:[{id,value}], properties, excludeXuid}` | Sessions of the title with an open public slot, not in a race if the host forbids joining one, whose contexts agree with the search's. Includes each session's `qos`. |
| `GET /sessions/:id` | | One session, with `qos`. |
| `PUT /sessions/:id` | any of `flags, publicSlots, privateSlots, state, members, contexts, properties, qos` | The host updates it. |
| `DELETE /sessions/:id` | | The host deletes it. |
| `POST /sessions/:id/join` | `{members:[{xuid,private}]}` | Replaces the member list. |
| `POST /sessions/:id/leave` | `{xuids:[...]}` | Removes members. |
| `POST /sessions/:id/migrate` | a session like `POST /sessions` | The new host's session replaces `:id`. |
| `PUT /sessions/:id/qos` | `{qos: base64}` | The host's QoS data (XNetQosListen). |
| `GET /sessions/:id/qos` | | `{qos: base64}`. |
| `POST /stats` | `{sessionId, xuid, titleId, views:[{viewId, properties}]}` | A stats write. Kept (the last 1,000), not read back: there are no leaderboards yet. |
| `GET /stats` | | The last 100 writes. |

Session `state`: 0 lobby, 1 registration (arbitration done), 2 in game, 3 reporting, 4 deleted. `flags` are the game's `XSESSION_CREATE_*` bits (0x01 host, 0x02 presence, 0x04 stats, 0x08 matchmaking, 0x10 arbitration, 0x20 peer network, 0x400 no joining in progress). Contexts are the game's `XUSER_CONTEXT` ids and values (0x800A game type, 0x800B game mode); the server matches them exactly, except the presence context (0x8001).
