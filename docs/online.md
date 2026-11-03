# Online play

Xbox LIVE for the Xbox 360 version of *Midnight Club: Los Angeles* is gone, and the recompiled game, until now, ran with the SDK's stand-ins for LIVE: signed in locally, no network link, every session call answered with nothing. This fork can instead sign the profile in to an **online server** that the fork's maintainer, or anyone, runs: a website where players create an account, and the matchmaking behind the game's online modes (lobbies, matchmaking, races against other players).

What the server does is small: it keeps the accounts, who is signed in, which sessions (lobbies) exist and what the game asks about them. The races themselves run **between the players' PCs over UDP**, as they did between consoles; the server never sees that traffic.

## Playing online

1. **Create an account** on the server's website. The gamertag you choose is the name the other racers see; it is yours from then on, on any PC.
2. Open your **account page**. It shows your **game key** and the three lines to put in the game's settings:
   ```toml
   mcla_online = true
   mcla_online_server = "https://the-site.example"
   mcla_online_key = "YOUR KEY"
   ```
   Put them in `mcla.toml` next to `mcla.exe` (the launcher's *More* › *Open the game folder*), or as options under the launcher's *Advanced* › *More mcla.exe options* with `--` in front of each (`--mcla_online=true --mcla_online_server=https://the-site.example --mcla_online_key=YOUR KEY`).
3. **Start the game.** The log (`--log_file=run.log --log_level=info`) says `online: signed in to ... as <your gamertag>` when it worked, and the website's home page shows you under *In the game now*. The game then treats the profile as signed in to LIVE, under your gamertag, with every privilege, and its online menus talk to the server.
4. In the pause menu, open the **online tab**. It finds the open lobbies and joins one, or hosts a lobby when there is none; the lobby shows on the website.

The game key is what signs you in: anyone who has it plays as you. Make a new one on the account page if it leaks (the old one stops working at once, and a game still running with it is signed out at its next heartbeat).

Without a key, or with one the server refuses, the game stays offline as before and the log says why (`mcla_online_key is empty` or `the server ... refused the game key`).

## Reaching each other

The game sends its race traffic to the other players' addresses directly:

- **Same LAN**: it works as is. Players behind the same router are given each other's LAN addresses.
- **Across the internet**: the player who **hosts** a lobby must be reachable on the UDP port their game uses, 1000 (the log says `the game bound UDP port 1000`; the website's lobby list shows the host). Forward UDP 1000 on the router to that PC. Joining usually works without forwarding: the joiner's game sends first, which opens the way back through its router. A VPN that puts everyone on one network (Hamachi, ZeroTier, Tailscale and the like) also does.

There is no relay through the server and no NAT traversal yet; a host who cannot be reached will not get anyone into their race, even though the lobby lists them.

## Running the server

One Node.js app, no packages, made for shared hosting (GoDaddy's Node.js apps and other cPanel hosts) as well as a PC: [server/README.md](../server/README.md) has the steps for both, the options and the API.

## What works and what does not

| | State |
|---|---|
| Accounts | Done: a website with registration, login, an account page with the game key and the settings to copy, a new key at any time, password change. No password reset by email. |
| Sign-in to LIVE, online XUID, gamertag, privileges | Done: the game signs in with the key; the account's gamertag and XUID are the player's identity in the game while the server answers (`XamUserGetName`, `XamUserGetGamerTag` and the sign-in info give the account's name), and the profile is signed in locally when it does not. |
| Finding and joining sessions (XSessionSearch, create, join, leave, modify, start, end, delete, host migration, arbitration) | Done against the server. Searches match the game's own contexts (game type and mode); the server only lets a lobby's host change it. |
| QoS (the connection quality probe before joining) | Answered from the server with the data the host published; the measured values (20 to 30 ms, 2 Mbit/s) are fixed, not measured. |
| Race traffic | The game's own UDP sockets, which the SDK already maps to real sockets. |
| HTTPS | Windows: yes (WinHTTP, the system's certificates and proxy). Linux and the PS5 build: HTTP only. |
| Leaderboards and stats | Writes are kept by the server; reads return empty tables, and the leaderboard enumerator fails cleanly. The game shows no leaderboards. |
| Rockstar's title server (Social Club, Rate My Ride, shared content) | Not implemented: the game is told the service is unreachable and goes on without it. `mcla_online_title_server` can point it at an address for experiments. |
| Friends, invites, messages, marketplace | Not implemented: no friends, no invites; the game's queries get empty answers. |
| Voice | Not implemented. |

On one PC the whole way in has been seen to work: the game signs in, and its online tab searches the server for lobbies and hosts one when it finds none (the lobby shows on the website). What has not happened yet is a second PC finding that lobby and racing in it (see [KNOWN-ISSUES.md](../KNOWN-ISSUES.md)): the join, QoS and race path is built from what the game asks for, what the Xbox 360's LIVE APIs answered, and Xenia's netplay implementation of the same APIs. Reports with `--mcla_online_log=true` in the game's log, which writes every online call and request, are what will make it work for everyone.

## Settings

| Setting | In `mcla.toml` | Default | Does |
|---|---|---|---|
| Online | `mcla_online` | `false` | Signs in to the online server; off, the game stays offline as before. Next start. |
| Server | `mcla_online_server` | `"127.0.0.1:36000"` | The server, as the account page shows it: `https://the-site.example`, or `host:port` for a server on the LAN. Next start. |
| Game key | `mcla_online_key` | `""` | The account's game key from the account page. Empty: offline. Next start. |
| Title server | `mcla_online_title_server` | `""` | An IPv4 address to give the game for Rockstar's title server; empty, there is none. For experiments. Next start. |
| Online log | `mcla_online_log` | `false` | Logs every online call and every request to the server. |

## How it is built

The recompiled code calls the Xbox kernel's exports as `__imp__<name>`, which the SDK's `rexruntime.dll` provides. The online module (`src/online/`) defines the same symbols, and the linker takes the game's own definitions over the DLL's, so no SDK patch is needed: `NetDll_XNet*` (addresses, keys, QoS, the title server), `XamUser*` (sign-in state, XUIDs, names, privileges), `XMsg*` (the XGI session messages, with everything else passed on to the SDK's apps) and the session handles. Two of the game's own start-up wrappers are replaced too (`sub_8244A258`, `sub_8244A490`): they looked up `WSAStartupEx` and `XNetStartupEx` by ordinal, which the SDK only stubs.

Consoles are told apart by their XNADDR: a MAC address and machine id derived from the PC's name and Windows user (the same on every start), LAN and public address, game port. The player's identity (gamertag, online XUID) is the account's. `XNetXnAddrToInAddr` gives the game the address that reaches a peer: the LAN address when both sit behind the same public address, the public one otherwise.
