# Open Live for Open Halo 2: design

Status 2026-10-17: implemented on the native build up to M5 (see "Implementation status"
at the end). Roadmap item "Live HLE" in
`C:/Users/andre/.claude/plans/wild-squishing-haven.md` (after P2P online system link).

Owner's goal: "a proper Xbox Live experience that costs me nothing for server infrastructure".
That means signing in with a local gamertag, friends and presence, matchmaking (Quick Match,
OptiMatch, custom games), parties and invites, and hosts that migrate. All of it runs peer to
peer, plus the existing free room relay (`wss://skatemods.com/rooms`).

**Recommendation in one line:** use approach A. Replace the title's public XOnline API (47
11-byte thunks plus a few XNet QoS calls) with host code. That code answers from a local
profile and a small "facilitator" that runs over the unchanged relay. Leave XNet, the
secure-association code and all game traffic exactly as they are now (system-link mode, one
virtual cable). The estimate is about 95-140 hours. Approach B (a fake Live server on the
virtual NIC) is 3-5x that work, carries more legal and taint risk, and cannot talk to
Insignia anyway.

---

## 0. Ground rules and sources

* **Allowed sources:**
  * the CC0 decomp `D:/n64work/halo2/third_party/halo2-decompiled`, for game-side names and
    `@retail` addresses;
  * our own lifted C, call graph and traces;
  * `tools/xdis.py`;
  * strings and section tables from the XBE, read in the dirty room, with only the facts
    carried over;
  * public descriptions of the original Xbox Live and of XDK API semantics (XboxDevWiki and
    similar).
* **Not allowed:** Microsoft XDK headers, libraries or samples. Write all struct layouts
  (`XONLINE_USER`, `XONLINE_MATCH_SEARCHRESULT`, `XNADDR`, `XNKID`/`XNKEY`, the friend and
  presence records) as our own definitions. Base them on public docs and on measured guest
  memory, and name the source for each field. Code from GPL projects or projects with unknown
  licences (Insignia is closed, Cxbx-R and xemu are GPL/LGPL) is also not allowed.
* **No retail content in Open Live data.** The title downloads several files from Live title
  storage: playlists ("hoppers"), the message of the day and the network configuration
  (section 1.4). Open Live serves files we author ourselves, in the format the title parses.
  The format is a fact; the content is ours.
* The UI text "Xbox Live" belongs to the title's own strings. Whether the clean UI pack calls
  it "Open Live" is an owner decision (trademark hygiene), outside this design.

---

## 1. How Halo 2 reaches Live

### 1.1 Libraries and layout (from the XBE)

* Library versions: `XAPILIB`, `XBOXKRNL`, `DSOUND`, `XVOICE`, `VOICMAIL`, `D3D8LTCG`,
  `XGRAPHCL`, `LIBCMT`, `LIBCPMT` at 1.0.5849, and **`XONLINES` 1.0.5849** (flags `0x200c`).
  Title id `0x4D530064`. The title version used in session checks is `0x2651` (9809):
  `session_search_version_allowed`, `online_session_search.cpp`.
* Sections:
  * `XONLINE` 0x3A0620-0x3CCFD4: the XOnline library;
  * `XNET` 0x3CCFE0-0x3E0400: the network stack, sockets, key exchange and QoS;
  * `XON_RD` 0x56ADE0: XOnline read-only data.
* **The public XOnline API is a thunk table.** 47 functions at 0x3A0620-0x3AEBB0 have this
  form:
  ```
  mov ecx, [0x0048480C]   ; the library's singleton object
  jmp <implementation>    ; a C++ method inside XONLINE
  ```
  Byte pattern `8B 0D 0C 48 48 00 E9`. Every game-side call goes through one of these thunks
  or through a few direct entry points (startup and cleanup). The API boundary is therefore
  explicit in the binary, which is exactly what HLE wants.
* All APIs are `__stdcall`: arguments are on the guest stack, the callee pops them and the
  result is in `eax`.

### 1.2 The entry points the game calls

Method: every `RECOMP_ABI_CALL(0x...)` in the generated C whose target lies in XONLINE or XNET
and whose caller lies in `.text`. That gives 92 XONLINE targets and 36 XNET targets. Each
caller's ordered call list was then aligned with the XDK names in the decomp body of the same
`@retail` function. The scan script is in this session's scratchpad (re-runnable,
`scan.py`/`align.py`). "Caller" below is the decomp name of the game function.

**Startup and sign-in** (`online_tasks.cpp`, `unknown_06b590.cpp`, `transport.cpp`)

| VA | API | Game caller(s) |
|---|---|---|
| 0x3A0C39 | XOnlineStartup (direct, not a thunk) | 0x8D840 transport start |
| 0x3A0D36 | XOnlineCleanup (direct) | 0x8D970 `transport_shutdown` |
| 0x3A0636 | XOnlineGetUsers | 0x6C800 `online_get_users` |
| 0x3A0641 | XOnlineLogon | 0x6C8B0 (sign-in) |
| 0x3A065B | XOnlineChangeLogonUsers | 0x6C8B0, 0x6CC10 (add/remove a pad's user) |
| 0x3A0650 | XOnlineLogonTaskGetResults | 0x6CD50 `online_task_get_logon_status` |
| 0x3A0666 | XOnlineChangeLogonUsersTaskGetResults | 0x6C750 |
| 0x3A0671 | XOnlineGetLogonUsers | 0x6BFF0, 0x6C000, 0x6C6C0, 0x6CED0, 0x8C660 |
| 0x3A067C | XOnlineGetServiceInfo | 0x6BFF0, 0x6C000, 0x6D220 |
| 0x3A0620 | XOnlineTaskContinue | 0x6C670 `online_task_continue` (the pump for every task) |
| 0x3A062B | XOnlineTaskClose | 0x6B640 |
| 0x3AD1A6 | XOnlineTaskProgress (inferred from `(handle, &pct, 0, 0)` and pct*0.01) | 0xB49A0 |
| 0x3A11AE | XOnlineGetNatType | 0x6B550 `online_get_nat_type` |
| 0x3A0687 | XOnlineSaveLogonState | 0x22387B (before launching the downloader XBE) |
| 0x3A0692 | **unidentified**, one dword argument (0) | 0x236926 (UI handler beside the dashboard launchers) |
| 0x3A069D | XOnlineOfferingIsNewContentAvailable | 0x8D4D0 |
| 0x3A0774 | XOnlineNotificationSetState | 0x6C1D0, 0x6D040 (own presence blob) |
| 0x3A0798 | XOnlineGetNotificationEx | 0x6D080 |

**Matchmaking** (`online_matchmaking.cpp`, `online_session_search.cpp`)

| VA | API | Game caller |
|---|---|---|
| 0x3A06A8 | XOnlineMatchSessionCreate | 0x8FCC0 |
| 0x3A06B7 | XOnlineMatchSessionUpdate | 0x8FE20 |
| 0x3A06E7 | XOnlineMatchSessionGetInfo | 0x8FD90 (then XNetGetTitleXnAddr) |
| 0x3A06F2 | XOnlineMatchSessionDelete | 0x90020 |
| 0x3A0710 | XOnlineMatchSessionFindFromID | 0x900E0 (join a friend's session) |
| 0x3A072E | XOnlineMatchSearch | 0x8FA80 (procedure 1, ≤50 results, 7 integer result attributes) |
| 0x3A0753 | XOnlineMatchSearchResultsLen | 0x8FA80 |
| 0x3A073D | XOnlineMatchSearchGetResults | 0x8FB30, 0x90160 |
| 0x3A0748 | XOnlineMatchSearchParse | 0x8FB30, 0x90160 |

**Friends, presence, invites, mute list**

| VA | API |
|---|---|
| 0x3A07A3 / 07AE / 07B9 / 07C4 | FriendsStartup / FriendsEnumerate / FriendsEnumerateFinish / FriendsGetLatest |
| 0x3A07CF | FriendsGetTitleName |
| 0x3A07DA / 0806 | FriendsRemove / FriendsAnswerRequest |
| 0x3A07E5 / 0811 / 081C / 0827 | FriendsGameInvite / FriendsAnswerGameInvite / FriendsJoinGame / FriendsGetAcceptedGameInvite |
| 0x3A835D / 838A | FriendsRequestEx / FriendsRequestByNameEx |
| 0x3A8321 / 834E | GameInviteSend / GameInviteAnswer |
| 0x3A8316 / 695E / 6F9B / 6FA6 / 6969 | PresenceInit / PresenceAdd / PresenceSubmit / PresenceGetLatest / PresenceClear |
| 0x3A075E, 0x3A0769 | two title-id predicates (the decomp calls both XOnlineTitleIdIsSameTitle; adjacent implementations 0x3B4740 and 0x3B474F) |
| 0x3A0841 / 0832 / 084C / 086E | MutelistStartup / Get / Add / Remove |

**Messages, storage, stats, teams, arbitration, feedback, queries** (needed only so they fail
cleanly)

| Group | VAs |
|---|---|
| Messages | 0x3A9D65 (DetailsGetResultsProperty), 9D74, 9D83 (attachment download), 9D8E (Create), 9DB8 (Destroy), 9DC3 (SetProperty), 0x3AB04A (Enumerate), 0x3AB060 (Send), 0x3AB2A6 (Details) |
| Storage | 0x3AB6B1 (CreateServerPath), 0x3AB6DB (Download), 0x3AB6EA / 0x3AB6F9 (DownloadToMemory and results), 0x3AD149 (UploadByServerPath), 0x3AD176 (UploadFromMemory) |
| Stats | 0x3A08C0 (Write), 0x3A08CB (Read), 0x3A08D6 (ReadGetResult), 0x3AB6A6 (WriteGetResult) |
| Teams (clans) | 0x3AEBA5, 0x3AEBB0, 0x3AFB80, 0x3AFB8F, 0x3AFBB9, 0x3AFBF1, 0x3AFC26, 0x3AFC50, 0x3AFC76, 0x3AFCA3, 0x3A8399, 0x3A83D4, 0x3A8407 |
| Arbitration | 0x3ADC4C (CreateRoundID), 0x3AE32B (Register), 0x3AE33A (ExtendRound), 0x3AE36A (Report) |
| Other | 0x3A0890 FeedbackSend; 0x3B0254 / 0x3B0263 QuerySearch and its results |

**XNet entry points the game calls.** These are kept lifted, except QoS (section 3.5):

| Group | VAs |
|---|---|
| Startup | GetEthernetLinkStatus 0x3CD576, XNetStartup 0x3CD58D, WSA* 0x3CD67B, XNetCleanup 0x3CD688 |
| Keys and addresses | Random 0x3CD0C0, CreateKey 0x3CD0CB, RegisterKey 0x3CD0D6, UnregisterKey 0x3CD0E1, XnAddrToInAddr 0x3CD0EC, TsAddrToInAddr 0x3CD103, InAddrToXnAddr 0x3CD32D, GetTitleXnAddr 0x3CD17D |
| Connections | 0x3CD344 (probably UnregisterInAddr; the alignment said XNetConnect, so confirm by trace), XNetConnect 0x3CD34F, GetConnectStatus 0x3CD35A |
| QoS | QosListen 0x3CD147, QosLookup 0x3CD156, QosRelease 0x3CD172, QosServiceLookup 0x3CD365 |
| Sockets | 0x3CD188-0x3CD254, WSAGetLastError 0x3CD718 |

### 1.3 Sign-in order and what it needs back

1. **Boot.** Transport start (0x8D840):
   * XNetGetEthernetLinkStatus, then XNetStartup with the title's parameters (24 sockets, 8
     keys, 40 security associations, QoS limit 0xFF);
   * WSAStartup;
   * **XOnlineStartup(&0)**. If it fails, the title runs XNetCleanup and has no network at
     all. It must succeed even when the player is offline.
   * Then XNetGetTitleXnAddr twice (0x7AE70 caches the MAC; 0x7A9B0 caches the XNADDR).
2. **Title, Choose Profile.** The game profile is a local save, not a Live account.
3. **"Sign in to Xbox Live" dialog, then the account list.** `online_get_users` calls
   XOnlineGetUsers(users[], &count). Today this would be the accounts the dashboard stored on
   the disk. 0x6C830 checks `XONLINE_USER_OPTION_REQUIRE_PASSCODE` and asks for the 4-button
   passcode if it is set.
4. **Logon** (0x6C8B0): XOnlineLogon(users[4] with the chosen user in the pad's slot,
   services[13], 13, NULL, &task). The services are, in the table `g_467178`:
   0x2, 0x4, 0x6, 0x7, 0x8, 0x9, 0xD, 0xF, 0x10, 0x12, 0x13, 0x14, and the title id
   0x4D530064. All have `connect=false`. Their names are not needed for HLE.
5. **Pump.** Every frame `online_task_update` runs XOnlineTaskContinue(task):
   * `XONLINETASK_S_RUNNING` / `_RUNNING_IDLE` / `_RESULTS_AVAIL` keep the task going;
   * a failure sets flags 0x24.

   XOnlineLogonTaskGetResults then maps the result: `S_LOGON_CONNECTION_ESTABLISHED` gives 1
   (signed in), `S_OK` gives 0. The handled errors are CANNOT_ACCESS_SERVICE,
   CONNECTION_LOST, INVALID_USER, KICKED_BY_DUPLICATE_LOGON, SERVERS_TOO_BUSY,
   UPDATE_REQUIRED, USER_ACCOUNT_REQUIRES_MANAGEMENT and S_LOGON_USER_HAS_MESSAGE.
6. **After logon.** The title then:
   * reads XOnlineGetLogonUsers() (a pointer to 4 `XONLINE_USER`; the per-user `hr` is
     checked for HAS_MESSAGE / REQUIRES_MANAGEMENT);
   * reads XOnlineGetServiceInfo (only service 0x14 would lead to XNetConnect, and its
     connect flag is false);
   * starts friends and the mute list, initialises presence and publishes its own presence
     with XOnlineNotificationSetState;
   * polls XOnlineGetNotificationEx;
   * calls XOnlineOfferingIsNewContentAvailable;
   * downloads title-storage files (section 1.4).
7. **Sign-out and controller changes** use XOnlineChangeLogonUsers. XOnlineCleanup runs only
   at transport shutdown.

**Minimum needed to reach a signed-in main menu:**
* Startup returns S_OK.
* GetUsers returns ≥1 account.
* Logon returns a handle whose Continue returns `XONLINETASK_S_RUNNING` then
  `XONLINE_S_LOGON_CONNECTION_ESTABLISHED`.
* LogonTaskGetResults returns the same.
* GetLogonUsers returns a stable guest pointer whose user in the pad's slot has a non-zero
  XUID, the gamertag and `hr=S_OK`.
* NatType returns OPEN or MODERATE.
* Notification calls return "nothing".

### 1.4 Title storage the game expects

The XBE carries these wide strings:
* `network_configuration.dat`, `bungienet_user.dat`, `message_of_the_day.dat` (file offset
  0x44CA2A, with a leading `\`);
* `multiplayer_hopper.dat` (0x44F8E2);
* UI tokens `hopper_name`, `hopper_description`, `playlist_blocks` and
  `matchmaking_countdown`.

The game downloads these through the storage API. 0xB4120 builds `"%d/%s"` with 0x2651 as
the folder and asks XOnlineStorageCreateServerPath for a path. 0xB4450 / 0xB4810 then
download it, driven by the pending-transfer table in `unknown_08e1e0.cpp`.

**Quick Match playlists are therefore server data.** Open Live has to serve an authored
`multiplayer_hopper.dat`, or the matchmaking menus will be empty. Its format comes from the
parsing code (to be located; start from `network_session_interface_get_value_49c8`, used by
`parse_hopper_name`, and the readers of `g_network_configuration`). This is one of the
biggest unknowns: milestone M2.

### 1.5 What traffic goes where on real Live

Facts from the XONLINE strings (file offsets in `default.xbe`, 0x485000-0x486960) and public
descriptions of Live:

* **DNS** for `MACS<x><realm>.XBOXLIVE.COM`, `AS…` and `TGS…` (format `"%s%s%.4s.XBOXLIVE.COM"`),
  plus the realm `XBOX.COM` and `krbtgt`.
* **Kerberos.** The machine account (MACS) comes first, then AS and TGS for the users and
  services. This is UDP to the KDCs. Public descriptions add Xbox-specific pre-auth and
  key-exchange extensions, keyed by per-console secrets in the EEPROM. Our kernel returns a
  zero `XboxHDKey` and serial `XR0000000000`, so this route has no identity to present.
* **Security Gateway (SG).** With the service ticket, XNet opens a secure association to the
  SG on **UDP 3074**:
  * a Diffie-Hellman key exchange (this is where `XcModExp` and `XcPKEncPublic` come in, and
    the latter is still a stub, STATUS decision 77);
  * then encrypted and authenticated traffic;
  * through it, TCP connections carry HTTP-like requests with `Content-type: xon/%x` to
    service front ends: `/xmatch/xmatchhost.srf`, `xmatchclient.srf`,
    `xmatchhostdelete.srf`, `/xpnfront/xpresence.srf`, `xnotification.srf`,
    `xmessaging.srf`, `xteams.srf`, `/xstats/*.ashx`, `/XArb/*.ashx`, `/xqry/*.ashx`,
    `/stfd/readfile|writefile|resolvename.ashx`, `/xbos/ContentAvailable.ashx`,
    `/msgserver/*`, `/fbserver/fbserver.ashx`, `/xsig/*`.
* **Game traffic is P2P and never touches Microsoft servers.** Peers learn each other's
  XNADDR, plus the session XNKID/XNKEY, from matchmaking results (`HostAddress`,
  `SessionID`, `KeyExchangeKey`) or from invites. Then:
  * the game calls XNetRegisterKey and XNetXnAddrToInAddr;
  * XNet does a direct key exchange with the peer on UDP 3074 (the SG helps only with NAT
    traversal);
  * game sockets run over that secure association.
* **QoS.** Before joining, clients run XNetQosLookup against candidate hosts. The host's
  XNetQosListen reply carries a title blob: Halo 2 puts its session description in it, and
  0x7E080 parses it into 0x53 dwords. `online_session_search.cpp` 0x905A0 merges it into the
  result, and 0x91000 scores and sorts by skill distance, ping and fill.
* Bungie's stats upload (`/upload_server/stats.ashx`, `POST %s HTTP/1.0` in the game's own
  .rdata) is game-side HTTP to Bungie, not Live. With no DNS it just fails.
* **Host migration** is game logic (`network_session` in `.text`, e.g. 0x61E00 "host
  reestablish", `network_session_host_leave(..., migrate)`). It runs over the existing peer
  associations. Live is only told afterwards: the new host creates or updates the match
  session.

---

## 2. Approaches compared

### (A) HLE at the XOnline API boundary

Replace the 47 thunks and the two direct entry points with host functions, plus the four XNet
QoS calls. They answer from a local profile and from a facilitator that uses the relay and
WebRTC. XNet stays lifted and in system-link mode: no SG, so keys and secure associations work
exactly as in today's online system link.

| | |
|---|---|
| Effort | ~95-140 h (section 3.8). About 45 functions get real behaviour; about 40 get clean "service unavailable / empty" answers. |
| Risk | Medium-low. The main risks are three unknowns: (1) whether XNet accepts a Live-style session XNKID with no SG logon (mitigation: our MatchSessionCreate mints system-link-flagged ids, see 3.5); (2) the hopper file format; (3) struct layouts. All three can be measured with a trace pass before any code. |
| Legal | Clean. Our own host code implements public API semantics; names come from a CC0 decomp; layouts are written ourselves. No crypto keys, no Microsoft protocol reimplementation, no third-party servers. |
| Reach | Every menu (sign-in, friends, Quick Match, OptiMatch, party, invites) can come up within M1-M5. Works the same in native and web builds. |
| Limits | Halo 2 only (other titles would need their own small shims, though most of it is generic XOnline). No interop with real-console players on Insignia. |

### (B) Network-level emulation of Live

Keep the lifted XOnline library. Answer its DNS, Kerberos, SG key exchange and the
`xon/*` HTTP services from a fake server inside the NIC model, as private servers do.

| | |
|---|---|
| Effort | 300-600 h+. It needs: Kerberos with Xbox extensions; the SG handshake, crypto and packet format; the TCP-over-SG framing; each service's binary request and response formats (matchmaking, presence and notification, messaging, storage, stats...); per-console machine accounts. Then cross-user state (presence, matchmaking) still needs the same facilitator as A, because the "server" lives in each player's own process. |
| Risk | High. The formats are undocumented and taken from the retail library. Any miss means a silent failure deep inside lifted crypto code. `XcPKEncPublic` / `XcPKDecPrivate` / `XcVerifyPKCS1Signature` must be real, and Live's public keys are retail data embedded in the binaries. |
| Legal | Interoperability RE is defensible, but the server needs keys and constants read from the retail XBE or kernel. Under the clean-room rules those are retail data and cannot ship. Pointing at Insignia instead is a terms-of-service matter (below). |
| Reach | Nothing works until logon fully works: no partial progress. |
| Upside | Title-agnostic: one server for every XDK title. A version could in principle talk to Insignia, which is out of scope. |

**Insignia.** Insignia is a free, closed-source Live replacement. Halo 2 went into public beta
on 2024-03-15. Community wikis say it works with xemu. Its connection guide covers consoles
only: DNS 46.101.64.175, a Setup Assistant that registers the console, and an account made
with an emailed code. It says "Modded consoles with a nulled HDD key cannot connect".

Its Terms say nothing about emulators, recompilations or third-party clients. The relevant
clauses are "You may not attempt to gain unauthorised access to Insignia, other accounts,
computer systems…" and "may not … damage, disable, overburden, or impair Insignia". No public
statement about recomps was found.

Our runtime has a zero HDD key and a fake serial. Connecting would mean using real console
identity data (dirty) and Insignia's permission. **Contacting Insignia is the owner's call.**
This design does not depend on it.

**Which reaches "matchmaking/parties/friends menus work" sooner:** A, clearly. B has a long
all-or-nothing stretch (DNS, Kerberos, SG, xmatch) before the first menu reacts.

**Recommendation: A.**

---

## 3. Design of approach A

### 3.1 Where the hooks go

* Today `RECOMP_ABI_CALL(va, fn)` calls the lifted function directly. `recomp_lookup_manual`
  sees only indirect calls, so overrides cannot go there.
* **New post-lift pass** `h2lift/xo_hle_pass.py`, run by `tools/relift.sh` next to
  `mmio_pass.py`. It replaces the bodies of the listed `sub_XXXXXXXX` (the 47 thunks,
  0x3A0C39, 0x3A0D36, and the QoS functions 0x3CD147/156/172/365) with
  `xo_hle_call(0xVA);`. A generated table maps each VA to a host handler and its stdcall
  argument count.
* The handler reads arguments with `MEM32(g_esp+4+4*i)`, sets `g_eax`, pops
  `g_esp += 4 + 4*n` and calls `RECOMP_NOTE_RET(4*n)` so the sanitizer's ABI check passes.
* A switch `RECOMP_OPEN_LIVE=0` routes to the lifted body instead. Keep the lifted functions
  under a renamed symbol (`sub_XXXXXXXX_lifted`), which gives A/B testing for free.
* **Step zero, before any behaviour:** `RECOMP_XO_TRACE=1` logs each call's VA, arguments,
  result and caller. Run it through boot, the Sign-in dialog and each Xbox Live menu with the
  real library. This turns the alignment in 1.2 into confirmed facts, settles 0x3A0692,
  0x3CD344 and the two title-id predicates, and records the struct sizes the title passes
  (`XONLINE_USER` stride, search-result layout, friend record size).
* **Guest memory.** Some returned pointers must live in guest RAM (GetLogonUsers' array,
  search results, friend arrays, message details). Allocate them from the host kernel's pool
  allocator (ExAllocatePool / MmAllocateContiguousMemory in `src/kernel`) once per task and
  free them on TaskClose. Task handles are opaque values, `0x0E5A0000 | slot`; the title only
  compares them with 0 and NONE.
* Same code for native and web. The facilitator transport is `olive.c` (libdatachannel plus
  WebSocket) and `olive.js` on the page, with a postMessage bridge, like h2net.

### 3.2 Data model

* **Account (local).** One or more per installation, in `openlive.json` beside `halo2.ini`
  (web: per-viewer storage, with an export/import button):
  * `gamertag`: Xbox rules, ≤15 chars, letters, digits and space, first character a letter;
  * `xuid`: 64-bit, minted once, random with Xbox-style flag bits;
  * `secret`: 128-bit random, for signing friend tokens;
  * `passcode` (optional), `friends[]`, `mutes[]`, `settings`.

  GetUsers reports these; XOnlineLogon "signs in" the chosen one.
* **Identity on the wire.** `xuid` plus `gamertag` plus a public key or HMAC token minted from
  `secret`. Friend links store the friend's xuid and token, so a later impostor with the same
  gamertag is rejected. Gamertag uniqueness:
  * **Phase 1 (relay only):** first come, first served while online. The name claim room
    `h2n-<tag>` (space becomes `_`, lowercased) is held by the signed-in owner, so a second
    sign-in with the same name sees an occupied room. The name then becomes `Tag#2`, or the
    user gets a "name in use" error. Offline names are not reserved.
  * **Phase 2 (optional Worker, owner deploy):** a real registry (gamertag to xuid plus public
    key) on a free Cloudflare Worker with KV or a Durable Object.
* **MAC address.** Today stations are told apart by Ethernet address only, so every player
  needs a globally unique MAC: `02:` plus 5 bytes of hash(xuid) (locally administered). Wire
  it where nvnet / EEPROM `XC_FACTORY_ETHERNET_ADDR` is answered.
* **XNADDR.** Whatever XNet produces in system-link mode: ina 0.0.0.1, abEnet = MAC, no
  online fields. Carried opaquely in our messages.
* **Session.**
  ```
  { kid (8 B), key (16 B), host_xuid, host_xnaddr, public/private filled/open,
    attrs[≤15] (from MatchSessionCreate/Update), qos_blob (from XNetQosListen),
    hopper/version }
  ```
  The kid is minted by our MatchSessionCreate (3.5).
* **Room names on the relay.** All ≤24 chars of `[A-Za-z0-9_-]`; `b32(x)` is Crockford base32
  of 64 bits, 13 chars.

  | Room | Purpose | Holder |
  |---|---|---|
  | `h2u-<b32(xuid)>` | inbox and presence handshake for a user | the user, while signed in |
  | `h2n-<tag>` | gamertag claim and lookup (FriendsRequestByName) | the user, while signed in |
  | `pub-h2<v>-<b32(kid)>` | public advertisement of a matchmade session; the list **map label** = search bucket, e.g. `h2.9809.qm.<hopper>` or `h2.9809.om.<gametype>` | the session host |
  | `h2s-<b32(kid)>` | a session's own signaling room, optional (3.4) | members, briefly |

### 3.3 Facilitator protocol (over the unchanged relay)

* Binary payloads after the relay's u32 member id: one type byte, then CBOR or a fixed
  struct.
  * Types `o`/`a`/`c` are already used by h2net (SDP offer, answer, ICE candidate).
  * Open Live adds `H` (hello: xuid, tag, token, presence), `P` (presence update), `F`
    (friend request), `f` (friend answer), `I` (game invite: session kid/key/host xnaddr,
    title id), `i` (invite answer), `Q` (QoS / session-info request), `q` (QoS reply: fill,
    attrs, qos_blob, host xnaddr, kid, key), `M` (mute and other notices), `X` (bye).
* **Inbox pattern.**
  * To reach user U, open a WebSocket to `h2u-<U>`. U is the oldest member; the relay's
    `welcome` gives you U's member id. Send the message and wait for the reply.
  * Then either leave, or upgrade to a WebRTC data channel (label `olive`) for an ongoing
    link: friends' presence, party members.
  * The relay caps a room at 10 members, and this pattern keeps room occupancy at about 2.
* **Presence without a server.**
  * On sign-in a user holds their inbox and, for each friend, briefly visits the friend's
    inbox: if the friend is there, exchange `H` and upgrade to a data channel.
  * Presence updates (XOnlineNotificationSetState blob, title id, "joinable") then flow over
    those P2P channels. A friend not online shows as offline.
  * Offline friend requests and invites cannot be stored with no server: they are delivered
    the next time both are online, re-sent by the sender's client. The optional Worker can
    hold a mailbox.

### 3.4 Matchmaking mapping

* **Host advertises** (XOnlineMatchSessionCreate):
  * mint kid and key, store the attributes;
  * hold `pub-h2…-<kid>` with `?map=<bucket label>`;
  * the task completes → GetInfo returns kid and key, and the game registers them with XNet
    itself.

  Update replaces attributes in memory. If the bucket label must change, the host re-opens
  the room under the new label, since the relay sets the label only at room creation. Delete
  leaves the room.
* **Search** (XOnlineMatchSearch, procedure 1):
  * The facilitator maps the title's attribute ranges to bucket labels. The exact attribute
    meanings come from trace step zero plus `function_0b4a90`.
  * It GETs `/rooms/list?map=<label>` for each bucket (the relay returns ≤50 rooms per call,
    sorted by member count).
  * For up to 16 candidates in parallel, it opens the advert room and sends `Q`. The host
    answers `q` with fill counts, the 7 integer result attributes, its XNADDR, kid and key,
    and the QoS blob.
  * It closes the rooms and returns `XONLINETASK_S_RESULTS_AVAIL`.
  * GetResults fills guest `XONLINE_MATCH_SEARCHRESULT` records; Parse decodes the 7 integer
    attributes.
* **FindFromID** (join a friend's or invited session): ask the host through its inbox, or
  through the advert room if public.
* **Custom games and parties.** In Halo 2 these are game-hosted sessions joined by invite or
  "join friend", with no search. They need only presence (joinable flag plus session ref in
  the presence blob), GameInvite and FindFromID. OptiMatch is the same as Quick Match with
  different bucket labels.
* **More than 10 players.** Halo 2 sessions reach 16 (Big Team). Today's h2net mesh is
  limited to one room of ≤10. Open Live switches h2net to **pairwise signaling through
  inboxes**:
  * to connect to a peer, signal through that peer's inbox and leave;
  * the full mesh (16 players = 120 data channels) is then independent of room size.
  * Owner shortcut: allow 16 members for `h2s-` rooms in the relay (a one-line
    `MAX_MEMBERS` change, owner deploy) and keep h2net's one-room model.
* **h2net changes.** One process can hold several peer sets: party plus game during a
  transition. The learning switch spans them all, and `h2net_add_peer(xuid)` /
  `h2net_drop_peer(xuid)` are driven by the facilitator rather than by room membership.

### 3.5 XNet secure associations and QoS on the P2P mesh

* **Keys.** The game passes our kid and key to XNetRegisterKey and the peer's XNADDR to
  XNetXnAddrToInAddr. XNet then key-exchanges on the virtual Ethernet, exactly as in system
  link, which already works (STATUS decisions 77-80).
  * **Risk:** XNet may treat a kid whose flag bits say "online peer" as needing an SG
    logon. Mitigation: mint kids with XNetCreateKey's own system-link form. Call the lifted
    XNetCreateKey from the HLE (0x3CD0CB), or copy its flag bits as measured by the trace.
    Test early (M4a).
* **Precondition.** The two stations must share the mesh before the key exchange starts. The
  facilitator therefore connects the data channel to the host (and to the other members it
  learns) when GetResults or FindFromID hands back a host XNADDR. In practice: when the
  QoS/info reply arrives, the joiner already has a channel open to the host, so the title's
  XNetXnAddrToInAddr sees a reachable MAC.
* **QoS HLE** (4 functions):
  * `XNetQosListen(kid, data, size, bps, flags)`: store the host's blob per kid; the
    facilitator serves it in `q` replies.
  * `XNetQosLookup(n, xnaddrs, kids, keys, …, &pxnqos)`: returns at once a guest `XNQOS`
    whose entries fill in over time. Each entry gets the blob and an RTT, measured as WebRTC
    RTT if a channel is up, otherwise relay round trip time. The flags are set per entry
    (complete, target contacted/disabled).
  * `XNetQosRelease`: frees it. `XNetQosServiceLookup`: returns "no service".

  Halo 2 sorts candidates on these numbers (0x91000), so the RTTs must be real.

  Note: system link already calls QosListen and Release (decision 78). Keep the lifted path
  when Open Live is off.
* **NAT.**
  * Data channels use public STUN only, with no TURN (free). XOnlineGetNatType reports OPEN
    / MODERATE / STRICT from a STUN behaviour test, so the title's own host selection avoids
    strict-NAT hosts.
  * Pairs that cannot connect: fall back to carrying that pair's frames through the relay
    (it forwards binary ≤64 KB), with a hard cap. A match is ~75 frames/s per player, and
    Cloudflare bills Durable Object WebSocket messages; on a free plan that burns the daily
    quota fast.
  * Hosts are better chosen among open-NAT players.

### 3.6 Host migration

* Already implemented by the game (`network_session`), provided:
  * every member keeps channels to every other member (full mesh, as h2net does now);
  * the relay's `host-left` notice does not tear the mesh down (h2net already ignores it,
    `h2net.c` ~392).
* The new host's game code calls XOnlineMatchSessionCreate / Update for the migrated session.
  The HLE then takes over the advertisement (new `pub-` room, same kid if the title keeps it)
  and updates presence "joinable" for friends.
* The old host's advert room disappears with it. Searchers drop stale entries: a `Q` that
  times out after 2 s.
* Test: an 8-instance headless match, kill the host process, expect the game's own migration
  screen and play to continue (M6).

### 3.7 Functions to implement, in order

1. **Boot and trace.** XOnlineStartup / Cleanup; the trace pass; TaskContinue / TaskClose /
   TaskProgress (the generic task engine).
2. **Sign-in.**
   * GetUsers, Logon, LogonTaskGetResults, GetLogonUsers, ChangeLogonUsers (+GetResults),
     GetServiceInfo (returns `XONLINE_E_…` "not found" for all), GetNatType, SaveLogonState
     (zeros; only used before launching Downloader.xbe).
   * NotificationSetState (store and broadcast own presence), GetNotificationEx (pending
     friend request/invite flags), OfferingIsNewContentAvailable (S_FALSE / none).
   * MutelistStartup / Get / Add / Remove (local list), FriendsStartup (empty list).
3. **Title storage.** CreateServerPath (echo a path), DownloadToMemory (+GetResults) and
   Download, served from `openlive/storage/9809/*.dat` (our authored files; web: fetched
   beside the game data). Upload* returns success and discards, or keeps a local copy.
4. **Matchmaking.** MatchSessionCreate / Update / GetInfo / Delete / FindFromID, MatchSearch /
   ResultsLen / GetResults / Parse, plus the QoS HLE (XNetQosListen / Lookup / Release /
   ServiceLookup).
5. **Friends and presence.**
   * FriendsEnumerate / EnumerateFinish / GetLatest, RequestEx / RequestByNameEx,
     AnswerRequest, Remove, GetTitleName ("Halo 2" for our title id, "?" otherwise).
   * PresenceInit / Add / Submit / GetLatest / Clear, the two title-id predicates (same
     title = `0x4D530064`).
6. **Invites and parties.** GameInviteSend / Answer, FriendsGameInvite / AnswerGameInvite /
   JoinGame / GetAcceptedGameInvite.
   * Message* only as far as the title routes friend requests, invites and team recruits
     through message blocks (`online_message_block_send_*` 0x8F500-0x8F8B0): Create /
     SetProperty / Send / Destroy map to an `I`/`F` facilitator message; Enumerate /
     Details return what was received this session.
7. **Clean failures.** Stats Read / Write (+results: "service unavailable" or empty
   leaderboards), Teams* (no clans), Arbitration* (succeed and skip: no ranked integrity
   without a server), FeedbackSend (S_OK, dropped), QuerySearch (empty). Each one is checked
   in the UI to show a normal "not available" message rather than hanging a task.

### 3.8 Milestones and estimates

| M | Content | Hours | Done when |
|---|---|---|---|
| M0 | HLE pass, dispatch table, stdcall helpers, guest allocator, task engine, `RECOMP_XO_TRACE` on the real library through every Live menu | 6-10 | trace sheet confirms all VAs and struct sizes; `RECOMP_OPEN_LIVE=0` is bit-identical to today |
| M1 | Sign-in (group 2), local accounts in `openlive.json` | 10-14 | headless pad script: title → Choose Profile → Sign in → gamertag shown → Xbox Live menu opens (screenshot) |
| M2 | Title storage: locate the parsers, document the formats of `multiplayer_hopper.dat`, `message_of_the_day.dat` and `network_configuration.dat`, author clean files (our playlists: e.g. Rumble Pit / Team Slayer / Team Skirmish / Big Team) | 10-18 | the Quick Match and OptiMatch lists show our hoppers; MOTD shows our text |
| M3 | Facilitator (`olive.c` / `olive.js`): inbox rooms, name claim, pairwise signaling, h2net multi-peer-set API | 14-20 | 4 native + 1 browser instance find each other by xuid with no room code typed |
| M4 | Matchmaking plus QoS HLE (a: kid-flag experiment first, 2 h) | 16-24 | two instances: A hosts (Quick Match) → B's search finds it → B joins → match starts; then OptiMatch |
| M5 | Friends, presence, requests by name, invites, join friend | 14-22 | A adds B by gamertag; B accepts; presence "in lobby, joinable"; A invites, B accepts, B lands in A's party |
| M6 | Parties, custom games, 16-player mesh, host migration | 10-16 | 8-instance party → matchmake as a squad; kill host → migration completes |
| M7 | Clean failures (stats, teams, arbitration, messages, feedback, query) | 4-6 | every Xbox Live menu item visited by script; no hung task, no crash |
| M8 | Web parity, NAT-type test, real two-NAT test, docs, `STATUS.md` | 10-16 | browser ↔ native Quick Match; one test across two real home networks |
| | **Total** | **~95-140** | |

### 3.9 Owner items

1. **Relay changes = owner deploy** (`wrangler deploy`; auto mode blocks deploys). None are
   required for Phase 1. Optional:
   * allow 16 members for `h2s-`/`h2u-` prefixes (simplifies 3.4);
   * a room-label update message, so the advert need not be re-created on Update;
   * a separate tiny free Worker "olive" (gamertag registry, offline mailboxes for friend
     requests and invites, a session table with range queries).
2. **Free-tier budget.** The relay is shared with Skate 3. Open Live's signaling is small: a
   few KB per peer pair, plus one list GET and ≤16 short room visits per search. Relaying
   game frames for strict-NAT pairs is not small, so cap or disable it. Check Cloudflare's
   Durable Object free-plan limits before launch.
3. **Insignia.** Whether to contact them. No public stance on recomps was found. Their ToS
   forbids unauthorized access, and a nulled HDD key cannot connect. Open Live does not need
   them.
4. **Naming.** "Xbox Live" in the title's UI and our docs vs "Open Live" (trademark), and
   the default playlists to ship.
5. **Gamertag policy** in Phase 1: names are unique only while online.

### 3.10 Open questions to settle by trace (M0) before coding

* Identity of 0x3A0692 (an implementation with a 0xC00-byte frame, called with 0 from
  0x236926), and of 0x3CD344 (UnregisterInAddr?).
* `XONLINE_USER` stride and field offsets as Halo 2 uses them. The decomp touches `xuid`,
  `dwUserFlags`, `dwUserOptions` and `hr`; `g_54e8e0` + 0x470 holds a copy.
* `XONLINE_MATCH_SEARCHRESULT` and attribute layouts (`function_0b4a90`, `function_0b4b80`,
  `g_44050c`), and the meaning of the 7 integer result attributes and the ≤15 session
  attributes.
* The presence blob: 0xFCF1 signature, time byte and flags byte, decoded in
  `online_friends.cpp` 0x8D230. Halo 2 friends lists show "in lobby, 12 minutes" from it.
* Whether XNet accepts our kid while "not logged on" (M4a).

---

## Implementation status (2026-10-17, native and web builds)

Switches: `RECOMP_OPEN_LIVE=1` (ini `open_live = 1`; web: `?link=openlive&env=RECOMP_OPEN_LIVE=1`)
answers the API; off (the default everywhere), the wrappers call the title's own library (no
behaviour change). `RECOMP_XO_TRACE=1` traces either path. `RECOMP_OL_GAMERTAG` / ini `gamertag`
names the first local account (`openlive.json`; web: in the save folder of the private file
system, so it survives page loads).

| Part | Where | State |
|---|---|---|
| Hooks (111 entry points) | `h2lift/xo_hle_pass.py` (run by `tools/relift.sh`), `port/xr/src/openlive.c` | done, native and web (`-DOPENHALO2_OPEN_LIVE` in `port/web/build_web.sh`) |
| Sign-in, tasks, stats (empty), arbitration (accepted) | `openlive_api.c` | done |
| Title storage (our MOTD, our playlists) | `openlive_api.c`, `tools/ol_storage.py` | done; web fetches `openlive/storage/...` from the site; network configuration and bungienet user file not served (title defaults) |
| Facilitator (hold/visit rooms on the relay) | `net/olive.c` (+ `net/olive_ws_web.h` on the web) | done; holds the relay drops are reopened; a visit asks every member and takes the first reply (the relay keeps a killed process's member for a while) |
| Multi-room mesh | `net/h2net.c` (native), `net/h2net_web.c` + `port/web/net.js` (web: one `h2net.js` mesh per room) | done |
| Matchmaking, QoS, session mesh | `openlive_match.c` | done: Quickmatch/Optimatch, solo and as a party (squad) |
| Sessions past a relay room (more than 10) | `openlive_match.c` (inbox signaling), `h2net_join_ext` | done natively; the web build stays in the room (no external rooms there yet) |
| Host migration | the title's own (`network_session`) over the full mesh | works (host's process killed mid-match) |
| Friends, presence, requests, join friend, custom games | `openlive_friends.c` | done |
| Game/party invites | `openlive_friends.c` (messages) | done: the invite is a type-3 message (Msg column icon, message screen, Accept joins the party) |
| Clans | `openlive_clans.c` | done: create, invite (type-4 message), accept, members and ranks; the record lives with the founder |
| Messages | `openlive_friends.c` | invites and clan invites only; "Send Message" text/voice messages are dropped |

Differences from the design above, found by measurement:
* Discovery uses private bucket rooms `h2q9809-<playlist>-<shard>` that every advertising host
  holds, not `pub-` rooms: the relay's public directory is listed by Skate 3's room browser.
* The mesh room is named after every key the title registers (`h2s-<kid>`), not the Live
  session id: the joiner registers the host's network session key taken from the QoS data.
* The title marks Live session ids as online-peer ids (0x7A8D7 sets bit 7 of byte 0); XNet then
  refuses the plain address (WSAEINVAL). The hooks hand the title's XNet unmarked copies.
* Matchmaking needs stats (six values per playlist, 0x74AA0) and a successful arbitration
  registration (or the match ends); both are answered locally.
* Halo 2 (this XDK) carries game invites and clan invites as Live *messages*, not friend-state
  bits: `XOnlineMessageEnumerate` summaries (0x40 bytes, 0x8EBD0: sender +0, type +0xC, context
  +0x10, time +0x18, id +0x20, title +0x28, gamertag +0x30), details property 0x581 = the
  session id, `XOnlineGetNotificationEx` for the toast. The context of a clan invite is the clan.
* Playlist minimums come from the title's hopper rules: Free For All 4 players and solo only,
  Team Slayer 6, Training Ground 4, Duel 2.
* Sessions over 10 players: a station the mesh room turns away joins an external h2net room;
  its signaling goes through inboxes ('M' member -> host roster, 'R' roster, 'N' batched SDP and
  candidates, in order per member); the outside station offers, room members answer. The host
  of a registered key is the remote whose QoS data carries it (bit 7 aside), else the host last
  measured.
* Emscripten runs WebSocket callbacks on the page's main thread whatever thread is asked for;
  the web facilitator therefore never waits for the main thread (requests and events are posted).

Evidence: `D:/n64work/halo2/reports/openlive/` (M1-M5) and `D:/n64work/halo2/reports/openlive2/`:
`inv_icon_B.png`, `inv_message_B.png`, `inv_accepted_lobby.png` (invites);
`M6_party_formed.png`, `M6_squad_party_ffa_too_large.png`, `M6_squad_matchmaking_3of4.png`,
`M6_four_matched.png`, `M6_four_player_match.png`, `M6_host_migration_after_kill.png` +
`M6_host_migration_log.txt`; `M6_overflow_join_via_inboxes.png`,
`M6_overflow_four_player_match.png`, `M6_overflow_status_D.txt`; `M7_clan_created.png`,
`M7_clan_invite_received.png`, `M7_clan_two_members.png`; `M8_native_vs_browser_duel.png`,
`M8_native_vs_browser_postgame.png`, `M8_native_log.txt`, `M8_browser_log.txt`.

Rigs: native `tools/ol_boot.sh`, `ol_trio.sh` (up to four copies: build/xr, xr_b, xr_c, xr_d;
`XENV_d=...` per copy), `ol_drive.py` (reads the screen: `tolive`, `optimatch<row>`, `state`),
`ol_keys.sh` (`wait@N`), `ol_sheet.sh`, `ol_kill.sh`, `ol_visit.py` (probe a relay room);
web `ol_wboot.sh`, `ol_wdrive.py`, `ol_wkeys.sh`, `ol_wkill.sh`, `ol_wstack.py`.
`RECOMP_NO_HOST_PAD=1` (runtime) keeps a controller in use elsewhere out of headless copies.

Not done: Content Download (skipped), text/voice messages, sessions past 10 in the browser (it
cannot take an external room yet), a test across two real home networks (all tests ran on one
machine), NAT-type reporting, and the optional owner items of 3.9 (relay changes, registry).
