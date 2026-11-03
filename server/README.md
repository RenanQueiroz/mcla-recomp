# The online server: website, accounts and matchmaking

`mcla-server.js` is one Node.js app that is three things for the game's online patch (`mcla_online`; the player's side is in [docs/online.md](../docs/online.md)):

- **The website.** Players create an account there (a gamertag, a password, an email if they like) and get a **game key**. The account page shows the three lines to put in `mcla.toml`. The home page lists who is in the game and which lobbies are open.
- **The account system.** The game signs in with the key (`Authorization: Bearer <key>` on every request). The account's gamertag and online XUID become the player's identity in the game, so two PCs can never be the same player and a name belongs to whoever registered it first.
- **The matchmaking server.** It keeps who is online and which sessions (lobbies) exist, answers the game's lobby searches and hands out the QoS data hosts publish. The races themselves run between the players' PCs over UDP and never pass through the server.

Plain Node.js 16 or newer, no packages. Everything it keeps is in JSON files in its `data` folder (accounts, who is online, the lobbies, a secret for the login cookies), so a restart loses nothing.

## Running it yourself

```bash
node server/mcla-server.js
```

Options: `--port 36000` (the default), `--bind 0.0.0.0` (every interface, the default; `127.0.0.1` for this PC only), `--data ./data` (the folder for the JSON files; by default `data` next to the script), `--base-path /mcla` (when the app lives under a path of a site), `--patch-url URL` (the download link on the home page), `--verbose` (every request), `--trust-proxy` (take the client's address from `X-Forwarded-For`). The environment does the same: `PORT`, `MCLA_SERVER_BIND`, `MCLA_DATA_DIR`, `PASSENGER_BASE_URI`, `MCLA_PATCH_URL`, `MCLA_SITE_NAME`, `MCLA_VERBOSE`.

Open `http://localhost:36000/` in a browser for the site.

## Hosting it on shared Node.js hosting (GoDaddy, cPanel)

The app was made for a host that gives you one Node.js app managed by Phusion Passenger, as cPanel's *Setup Node.js App* does on GoDaddy's web hosting:

1. Upload the `server` folder's files (`mcla-server.js`, `app.js`, `package.json`) to the application root the host gives you, for example `mcla-online`.
2. In *Setup Node.js App*: application root `mcla-online`, application URL your domain (or a sub-path of it), **application startup file `app.js`**, Node 16 or newer, production mode. Create it and start it.
3. Open the domain: the home page is there, and the account page shows the right address for `mcla_online_server` (it is taken from how the site was reached, `https://yourdomain.com`).

What the app does for that kind of host, by itself: it listens on the port in `PORT` (Passenger sets it) on the loopback, trusts `X-Forwarded-For` and `X-Forwarded-Proto` from the host's proxy (so players get their real public address, and cookies are marked secure under HTTPS), honours `PASSENGER_BASE_URI` when the app is under a sub-path, and keeps its state in files, re-reading them before each request, so it survives Passenger stopping the app when nobody has used it for a while and starting more than one copy of it. Give the `data` folder a place outside the web-served tree if the host's application root is served as static files (`MCLA_DATA_DIR` in the app's environment variables); `accounts.json` holds password hashes and game keys.

HTTPS: the game reaches the site with WinHTTP on Windows, so a host's certificate works as it does in a browser, and the three lines on the account page say `https://...`. If the host forces HTTPS, nothing more is needed. On Linux and the PS5 the game's client is HTTP only (no TLS there yet).

## Reaching each other in the game

The server only lists lobbies. Race traffic goes directly between the players on the game's UDP port (1000; the log says `the game bound UDP port N`):

- **Same LAN**: nothing to do. Players behind one router get each other's LAN addresses.
- **Across the internet**: the player who **hosts** a lobby needs UDP 1000 forwarded on their router to their PC. Joining usually works without it: the joiner's game sends first, which opens the mapping in its router. A VPN that puts everyone on one network (Hamachi, ZeroTier, Tailscale) also does.

## The API

JSON in and out. The game's routes need `Authorization: Bearer <game key>`; without it they answer 401 with the website's address. 64-bit ids (XUIDs, session ids, machine ids, nonces) are 16 hex digits. Addresses are dotted IPv4 text; a player's public address is what the server saw the request come from.

| Method and path | Needs | Body | Does |
|---|---|---|---|
| `GET /` | | | The website. `/register`, `/login`, `/account` (`POST` for the forms and for `/logout`, `/account/key`, `/account/password`). |
| `GET /status` | | | Public counts, who is in the game (names), the open lobbies. |
| `POST /players` | key | `{mac, machineId, lanIp, port, titleId}` | Signs the account's player in. Returns `{xuid, name, publicIp}`: the identity the game takes. |
| `POST /players/:xuid/heartbeat` | key | the same | Keeps the player and the lobbies they host alive. Every 15 s. |
| `GET /players/:xuid` | key | | One player (for arbitration's machine ids). |
| `DELETE /players/:xuid` | key, own | | Signs out; the player's lobbies go. |
| `POST /sessions` | key | `{id, titleId, flags, publicSlots, privateSlots, state, nonce, key, host:{lanIp, port, mac, machineId}, contexts:[{id,value}], properties:[{id,type,data}], members:[{xuid,private}]}` | The account publishes a lobby it hosts. |
| `GET /sessions` | key | | Every lobby, in full. |
| `POST /sessions/search` | key | `{titleId, numResults, contexts:[{id,value}], properties}` | Lobbies of the title with an open public slot, not in a race when the host forbids joining one, whose contexts agree with the search's; the searcher's own left out. Each comes with its `qos`. |
| `GET /sessions/:id` | key | | One lobby, with `qos`. |
| `PUT /sessions/:id` | key, host | any of `flags, publicSlots, privateSlots, state, members, contexts, properties, qos` | The host updates it. |
| `DELETE /sessions/:id` | key, host | | The host deletes it. |
| `POST /sessions/:id/join` | key, host | `{members:[{xuid,private}]}` | Replaces the member list. |
| `POST /sessions/:id/leave` | key, host | `{xuids:[...]}` | Removes members. |
| `POST /sessions/:id/migrate` | key | a lobby like `POST /sessions` | The new host's lobby replaces `:id`. |
| `PUT /sessions/:id/qos` | key, host | `{qos: base64}` | The host's QoS data (XNetQosListen). |
| `GET /sessions/:id/qos` | key | | `{qos: base64}`. |
| `POST /stats` | key | `{sessionId, titleId, views:[{viewId, properties}]}` | A stats write. Kept (the last 500), not read back: no leaderboards yet. |

Session `state`: 0 lobby, 1 registration (arbitration done), 2 in game, 3 reporting, 4 deleted. `flags` are the game's `XSESSION_CREATE_*` bits (0x01 host, 0x02 presence, 0x04 stats, 0x08 matchmaking, 0x10 arbitration, 0x20 peer network, 0x400 no joining in progress). Contexts are the game's `XUSER_CONTEXT` ids and values (0x800A game type, 0x800B game mode); the server matches them exactly, except the presence context (0x8001).

## Accounts, in detail

A gamertag is 1 to 15 letters, digits and single spaces, starting with a letter, unique whatever the case. Passwords are 8 characters or more, stored as scrypt hashes with a salt of their own. The game key is 40 hex digits made from random bytes; a player can make a new one at any time on the account page, and the old one stops working at once. The online XUID is derived from the account's number (it begins `0009`, as LIVE's did). Logins stay for 30 days in an HttpOnly cookie signed with the server's secret. The forms accept a few tries a minute per address (5 accounts an hour, 20 logins in 10 minutes).

There is no email sending, so no password reset by mail: a forgotten password means a new account for now, or an edit of `data/accounts.json` by whoever runs the server.
