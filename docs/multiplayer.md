# Online multiplayer: feasibility and security - 2026-11-15

Research for roadmap phase 12, the last phase. Nothing here is implemented: the runtime has
only ever run offline (`runtime/kernel/xnet.cpp`, see docs/runtime.md).

## What the game already has

Checked in the decrypted executable (`strings -n 5 game/extracted/default.pe`), the import list
(`runtime/generated/imports.cpp`) and period sources (end of this file).

- **Online modes.** Exhibition ("11v11") and Tackle Alley, as player matches and as ranked matches
  rated with TrueSkill (`Stats_Exhibition_Ranked_TrueSkill`, `Stats_TackleAlley_Ranked_TrueSkill`),
  plus leaderboards ("11v11 Ranked Leaderboard"), invites, a pregame lobby (`PregameLobby.xui`:
  choose side, select team, ready) and voice chat (XHV2: `RegisterRemoteTalker`,
  `SubmitIncomingChatData`). The publisher's fact sheet advertised competitive and co-op play
  online, live and split-screen; reviews list 1-2 players offline and 2 online.
- **A list of hosted games.** `AvailableMatches_HostedGames_Table`, `C_11v11_HostedGamesTable`,
  `C_TackleAlley_HostedGamesTable`. The game browser the project wants already exists inside the
  game: a session service only has to fill it with search results.
- **Its own network layer.** NaturalMotion's `NmgComms` (`comms_session.cpp`, `comms_packet.cpp`)
  sits on the system APIs. Its command names show the whole Xbox LIVE flow it relies on: session
  create, search, search by ID, join, modify, start, end and delete; arbitration registration;
  QoS; stats posting and reads; friends; remote gamer pictures and profile settings; title
  storage upload/download; string verification; marketplace offers; party invitations.
- **One host, clients join it.** Session packets go host <-> client
  (`ClientToHost_JoinRequest`, `HostToClient_Register`, `HostToClient_SessionStart`,
  `HostToClient_DropPlayers`, `RelayedUserDetails`, `VoiceData`, `ACKs`). Game packets cover the
  pregame lobby, coin toss, play selection, audibles, penalties, pauses, timeouts and replays.
- **In-match traffic looks host-authoritative.** `kGameMessage_Sync_Ai`, `_Sync_Match`,
  `_Sync_Referee` (`_Clocks`, `_GameFlags`), `_Sync_Team`, `Player_Standard`, `Ball_Standard`,
  `Camera_Update`, `PlaySound`, and controllers tagged by where they are
  (`CONTROLLER_LOCATION_TYPE_LOCAL`, `_LOCAL_NETWORK`, `_REMOTE_NETWORK`). That reads like the
  host simulating the match and sending its state, with the client sending its controller.
  **Not confirmed:** these may be an internal message bus that is also forwarded over the network.
  Open question 1.
- **No system link.** No "system link" text in the executable (ASCII or UTF-16) or the disc's
  loose files, and no source mentions it. (The packed archives may be compressed, so that part of
  the search is weaker.) Without system link, LAN tunneling like XLink Kai has nothing to carry,
  and the Xbox LIVE side has to be stood in for.
- **Imports the online part uses.** Sockets (`NetDll_socket`, `bind`, `connect`, `listen`,
  `accept`, `send`, `recv`, `select`, `WSASendTo`, `WSARecvFrom` ...); XNet
  (`XNetGetTitleXnAddr`, `XNetXnAddrToInAddr`, `XNetInAddrToXnAddr`, `XNetXnAddrToMachineId`,
  `XNetQosListen` / `Lookup` / `Release`, `XNetDnsLookup`); session handles
  (`XamSessionCreateHandle`, `XamSessionRefObjByHandle`); the XSession and stats calls, which go
  through `XMsgStartIORequest` / `XMsgInProcessCall`; sign-in and privileges
  (`XamUserGetSigninState`, `XamUserCheckPrivilege`, `XamUserGetMembershipTierFromXUID`,
  `XamUserGetOnlineCountryFromXUID`, `XamUserAreUsersFriends`); Guide screens (friends, gamer
  card, player review, sign-in, keyboard); voice (`XamVoiceCreate`, `XamVoiceSubmitPacket`,
  `XamVoiceHeadsetPresent`).

Why the topology matters: every player builds the executable from their own disc, with their own
compiler. In a lockstep game both machines run the simulation and must agree bit for bit, so any
floating-point difference between two builds would desync the match. Euphoria and PhysX are
float-heavy, and docs/feasibility.md already flags them. A host-authoritative game only needs both
sides to agree on the packet layouts, and they do when both run the same game executable.

## Ways to play together

| Option | What it needs | Notes |
|--------|---------------|-------|
| 1. Local multiplayer | Two to four controllers (phase 5) | Check Exhibition with two pads. Nothing new to build. |
| 2. Streamed local play (Steam Remote Play Together, Parsec, Sunshine/Moonlight) | Option 1 plus a third-party streamer | The remote player sees the host's screen and sends their pad. No game packets at all, and only the host needs the game. Latency is the stream's. |
| 3. Native online through the game's own Xbox LIVE modes | Runtime networking plus a session service (below) | The real thing: each player runs their own copy. This is phase 12. |
| 4. LAN tunneling (XLink Kai) | System link | Not possible: the game has no system link. |
| 5. New netcode (rollback etc.) | Replacing the game's network layer | Out of scope. |

Recommendation: write up option 2 as soon as option 1 is verified, since it costs nothing, and
build option 3 at the end.

## Native online: what has to be built

1. **Real sockets** in `xnet.cpp`, as docs/runtime.md sketches: a host socket per guest socket,
   readiness reported through `kernel::waitForObjects`. VDP sockets (the console's
   game-data-plus-voice UDP) become UDP with their header kept.
2. **XNet addressing.** On the console, an `XNADDR` and the IN_ADDR that `XNetXnAddrToInAddr`
   returns are addresses of secure associations that the system maps to real peers. The runtime
   does that mapping: each peer gets a virtual address, and the socket layer sends to that peer's
   real (or relayed) endpoint.
3. **XSession and QoS.** The messages behind `XMsgStartIORequest` (create, search, search by ID,
   join/leave local and remote, modify, start, end, delete, get details, arbitration register)
   become calls to the session service. A QoS lookup returns the host's published payload (the
   game parses it: "found [%u] contexts in QoS payload") and a measured ping.
4. **Online sign-in.** The local profile signed in to LIVE, an online XUID, the multiplayer and
   communication privileges granted, a gamertag taken from settings.
5. **Services that correctly answer "nothing"**: marketplace, title storage, friends. String
   verification accepts names (or runs a local word filter).
6. **Later**: voice (XHV2 to SDL3 capture and playback), stats and leaderboards, friends and
   gamer cards through phase 11's overlay, invites.

### The session service

A small, self-hostable server:
- hosts register a session (mode, slots, properties, QoS payload) and send heartbeats; a session
  without heartbeats expires
- search returns open sessions; join by ID; invite codes for private games
- rendezvous for UDP hole punching (each side learns the other's public endpoint)
- an optional relay for hosts that can't be reached directly, and for players who don't want to
  share their IP address
- per-session keys (see Security, threat 2)

Xenia Canary's netplay fork does this for Xenia: Xenia Web Services, a self-hostable REST API, and
UPnP port forwarding, which it ships turned off for security reasons. It is the first thing to
read; check its license before reusing any code.

### The game browser

The game's own Hosted Games table comes first. A separate list (in the launcher from phase 10, or
a web page) can show open sessions and start the game straight into one. It would use the game's
own invite path (`INVITATION_HANDLER_STATE_SEARCH_FOR_SESSION`, search by ID, join, then the
pregame lobby), the way an accepted invite works on the console.

### Compatibility check

On join, both sides compare the runtime's network protocol version and the hash of
`default.xex`. A different region or title update can change the game's packets. A mismatch is
refused with a message saying why.

## Security

On the console, only signed, unmodified consoles could send a game packets: XNet encrypted and
authenticated all title traffic, and Xbox LIVE vouched for each console. The game's packet code was
written for that, so we can't assume it checks what it reads. Here, anyone who can reach the
socket can send it anything. Threats, most severe first:

### 1. A malicious peer taking over your computer

The path: a crafted packet, then a memory bug in the game's own parser (packets, voice data, QoS
payloads, profile and gamer-picture data), then control of the guest, then control of the host
process.

What already contains it:
- Guest loads and stores are base + 32-bit address, so they stay inside the 4 GB guest window.
- There is no JIT. Guest code can't create new code, only reuse existing functions.

What doesn't. These were found while writing this. They're fine offline, where the guest is the
game, but they must change before any networking ships:
- **The indirect-call table is writable from guest code.** The host function pointers sit inside
  guest memory at 0x82E20000-0x83EE7D90 (`PPC_LOOKUP_FUNC`, filled by `buildFunctionTable` in
  `runtime/main.cpp:197`), mapped read-write like the rest of the window
  (`runtime/memory.cpp:117`). A guest write there sends the next indirect call anywhere in the
  host process, and a guest read leaks host addresses, which defeats ASLR. Fix: `mprotect` the
  table read-only once it is built.
- **`..` in a guest path climbs above the mount root** (`runtime/kernel/files.cpp:131`). Through the
  writable `save:` mount, a hijacked guest could create or overwrite any file the user can, such as
  shell startup files. Fix: clamp at the root and refuse symlinks that lead out of it.
- **Imports trust guest lengths.** `mem::host(addr)` is `base + addr`. An import that copies `len`
  bytes from a guest pointer near the top of the window reads or writes past the 4 GB mapping.
  Fix: one checked helper for every guest buffer an import touches.
- **Other host code that parses guest data**: the PM4 command processor, the shader translator,
  texture untiling and the FFmpeg XMA decoder. Offline they only see the game's own data. Fuzzing
  them would be worth it.

Defense in depth: run the game in a sandbox that allows only the game directory, its saves, the
GPU, audio, input and its one network socket (Landlock and seccomp, or bubblewrap / Flatpak). Then
even a full takeover can't read the rest of the home directory.

### 2. Strangers injecting packets

The runtime puts back what XNet provided. Only peers the session service admitted can talk to a
session. Each gets a per-session key, and every packet is encrypted and authenticated (libsodium
or the Noise protocol, for example). Anything else is dropped before the game sees it. Packet
sizes are capped as on the console, and rates are limited. This doesn't protect against a
malicious player inside your own session, so threat 1 still needs fixing.

### 3. Your IP address

With direct peer-to-peer, the players in a session see each other's IP addresses, and the session
service sees everyone's. The risks are denial-of-service attacks on a home connection and a rough
location. Mitigations: relay mode (peers see only the relay), invite-only sessions, and a plain
statement in the UI of what hosting exposes.

### 4. Opening the router

UPnP opens a port on the router to the whole internet, and many routers' UPnP code has bugs of its
own. Prefer hole punching through the session service with relay fallback, and offer UPnP only as
an opt-in that's off by default, as Xenia's netplay fork does.

### 5. Running the session service

Someone has to run and pay for it, and it faces the internet:
- HTTPS only, rate limits per IP, size limits on everything it stores
- player names and session descriptions are free text: a word filter (the game asks for string
  verification anyway) and a way to report abuse
- store the minimum (IP, session properties, heartbeat) and keep logs briefly; a public instance
  has privacy obligations (GDPR for EU players)
- self-hostable, with the server address in `settings.ini`, so the project isn't a single point of
  failure
- it's the project's own service: no Microsoft or Xbox LIVE names or logos, and it never connects
  to Microsoft's servers

### 6. Identity, cheating and rankings

No console certificate or Microsoft account stands behind a player. An XUID is a 64-bit number
anyone can claim, and the host runs the simulation, so a modified host can change the match. Ranked
matches, TrueSkill and leaderboards can't be trusted. Start with player matches only. Leaderboards
come later if at all, labelled unverified. Optional: a key pair per installation registered with
the session service, so names can't be impersonated.

### 7. Voice chat

Microphone capture is off by default, with push-to-talk and per-player mute. Voice packets get the
same encryption as game data.

### 8. Legal

Each player needs their own copy of the game (the build needs their disc), and the service carries
no game content. The game's menus say "Xbox LIVE"; that's the game's own text and stays. The
service and the project's own UI don't use Microsoft's names or logos.

## Order of work

All of it waits until the game is complete offline. The issues carry the label
`after the game is done`.

0. **Research.** No network code needed.
   - Confirm the netcode model: follow `kGameMessage_Sync_*` and `kGameMessage_Controller_Standard`
     in the code (`tools/fninfo.py`, `GUEST_TRACE`). Does an offline match already route those
     messages through the comms layer? (#108)
   - List every XSession message, XNet call and sign-in check the online menus make, with the
     existing import logging, and check whether Backbreaker is on Xenia netplay's compatibility
     list. (#109)
   - Verify local multiplayer with two controllers (option 1), then write up streamed play
     (option 2). (#110)
1. **Security prerequisites**, before any networking is merged: write-protect the call table
   (#111), clamp guest paths (#112), a checked guest-buffer helper (#113), a sandbox.
2. **LAN proof**: two instances on one machine or LAN, a local session service, plain sockets, the
   signed-in state. Goal: from the Xbox LIVE menu to a pregame lobby, then a kickoff.
3. **Internet play**: the session service, hole punching, encryption, the Hosted Games table, the
   compatibility check.
4. **Relay, voice, invites and the external game browser.**
5. **Maybe**: stats and leaderboards (unverified), friends.

## Open questions

1. Is in-match play host-authoritative or lockstep? This decides whether separately compiled
   builds can play each other at all.
2. Which ports and socket types does the game ask for? The import logging shows that.
3. Does starting a player match need arbitration or a stats session, or only ranked?
4. How good can latency get? Reviews criticized the original online play, so the best case may be
   "as it was in 2010".
5. Did a title update change the network code? Players on different versions couldn't play.
6. Who runs the public session service, and is a project-run instance wanted at all, or only
   self-hosted ones for groups of friends?

## Sources

- [Backbreaker (video game), Wikipedia](https://en.wikipedia.org/wiki/Backbreaker_(video_game)):
  online Exhibition and Tackle Alley, reception of the online play
- [Backbreaker official fact sheet](https://www.osftw.com/press/166/backbreaker-official-fact-sheet):
  competitive and co-op play online, live and split-screen
- [Xenia Canary netplay fork](https://github.com/AdrianCassar/xenia-canary) and its
  [config wiki](https://github.com/AdrianCassar/xenia-canary/wiki/Config-Setup): Xenia Web
  Services, UPnP off by default, network modes
- [Xbox network, Emulation General Wiki](https://emulation.gametechwiki.com/index.php/Xbox_network):
  overview of Xbox LIVE replacements
- [Xbox 360 XLink Kai setup](https://teamxlink.co.uk/wiki/Xbox_360_XLink_Kai_Setup): system-link
  tunneling and its limits
