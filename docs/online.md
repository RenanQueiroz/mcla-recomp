# Online play through a private server

Xbox LIVE for the Xbox 360 version of *Midnight Club: Los Angeles* is gone, and the recompiled game, until now, ran with the SDK's stand-ins for LIVE: signed in locally, no network link, every session call answered with nothing. This fork can instead sign the profile in to a **private server** that one player runs, so that the game's online modes (lobbies, matchmaking, races against other players) work between the people who point their games at that server.

What the server does is small: it keeps who is signed in, which sessions (lobbies) exist and what the game asks about them. The races themselves run **between the players' PCs over UDP**, as they did between consoles; the server never sees that traffic.

## Setting it up

**One player runs the server.** It needs [Node.js](https://nodejs.org/) 18 or newer:

```bash
node server/mcla-server.js
```

It listens on TCP port 36000 and prints a status page address (`http://localhost:36000/`) that lists players and sessions. [server/README.md](../server/README.md) has its options, how to reach it from outside the LAN, and its API.

**Every player turns the online layer on** and points it at the server. In `mcla.toml` next to `mcla.exe` (the launcher's *More* › *Open the game folder*):

```toml
mcla_online = true
mcla_online_server = "192.168.1.20:36000"
```

or as options under the launcher's *Advanced* › *More mcla.exe options*: `--mcla_online=true --mcla_online_server=192.168.1.20:36000`. The address is the server PC's: its LAN address for players on the same network, its public address (with TCP 36000 forwarded on its router) for players elsewhere.

Each player's **name** is their `user_name` setting (the launcher's *Player name*). Two players must not share a name: the name is what makes a player's online identity.

Start the game. The log (`--log_file=run.log --log_level=info`) says `online: signed in to ... as ...` when the server answered, and the server's status page lists the player. The game then treats the profile as signed in to LIVE, with every privilege, and its online menus talk to the server.

## Reaching each other

The game sends its race traffic to the other players' addresses directly:

- **Same LAN**: it works as is. Players behind the same router are given each other's LAN addresses.
- **Across the internet**: each player must be reachable on the UDP port their game uses. The log says `online: the game bound UDP port N` once the game sets up its network (the server's status page shows it too, under *Game port*). Forward that UDP port on the router to the PC, or use a VPN that puts everyone on one network (Hamachi, ZeroTier, Tailscale and the like, which also make the server's address a plain LAN address).

There is no relay through the server and no NAT traversal yet; a player who cannot be reached will not get into a race, even though the lobby lists them.

## What works and what does not

| | State |
|---|---|
| Sign-in to LIVE, online XUID, privileges | Done: the profile is signed in to LIVE while the server answers, locally when it does not (the game is told when that changes). |
| Finding and joining sessions (XSessionSearch, create, join, leave, modify, start, end, delete, host migration, arbitration) | Done against the server. Searches match the game's own contexts (game type and mode). |
| QoS (the connection quality probe before joining) | Answered from the server with the data the host published; the measured values (20 to 30 ms, 2 Mbit/s) are fixed, not measured. |
| Race traffic | The game's own UDP sockets, which the SDK already maps to real sockets. |
| Leaderboards and stats | Writes are kept by the server; reads return empty tables, and the leaderboard enumerator fails cleanly. The game shows no leaderboards. |
| Rockstar's title server (Social Club, Rate My Ride, shared content) | Not implemented: the game is told the service is unreachable and goes on without it. `mcla_online_title_server` can point it at an address for experiments. |
| Friends, invites, messages, marketplace | Not implemented: no friends, no invites; the game's queries get empty answers. |
| Voice | Not implemented. |

On one PC the whole way in has been seen to work: the game signs in, and its online tab searches the server for lobbies and hosts one when it finds none (the lobby shows on the server's status page). What has not happened yet is a second PC finding that lobby and racing in it (see [KNOWN-ISSUES.md](../KNOWN-ISSUES.md)): the join, QoS and race path is built from what the game asks for, what the Xbox 360's LIVE APIs answered, and Xenia's netplay implementation of the same APIs. Reports with `--mcla_online_log=true` in the game's log, which writes every online call and request, are what will make it work for everyone.

## Settings

| Setting | In `mcla.toml` | Default | Does |
|---|---|---|---|
| Online | `mcla_online` | `false` | Signs in to the private server; off, the game stays offline as before. Next start. |
| Server | `mcla_online_server` | `"127.0.0.1:36000"` | The private server, `host:port`. Next start. |
| Title server | `mcla_online_title_server` | `""` | An IPv4 address to give the game for Rockstar's title server; empty, there is none. For experiments. Next start. |
| Online log | `mcla_online_log` | `false` | Logs every online call and every request to the server. |

## How it is built

The recompiled code calls the Xbox kernel's exports as `__imp__<name>`, which the SDK's `rexruntime.dll` provides. The online module (`src/online/`) defines the same symbols, and the linker takes the game's own definitions over the DLL's, so no SDK patch is needed: `NetDll_XNet*` (addresses, keys, QoS, the title server), `XamUser*` (sign-in state, XUIDs, privileges), `XMsg*` (the XGI session messages, with everything else passed on to the SDK's apps) and the session handles. Two of the game's own start-up wrappers are replaced too (`sub_8244A258`, `sub_8244A490`): they looked up `WSAStartupEx` and `XNetStartupEx` by ordinal, which the SDK only stubs.

Consoles are told apart by their XNADDR: MAC address, LAN and public address, game port. A player's identity (online XUID, machine id, MAC) is derived from their name, so it is the same on every start. `XNetXnAddrToInAddr` gives the game the address that reaches a peer: the LAN address when both sit behind the same public address, the public one otherwise.
