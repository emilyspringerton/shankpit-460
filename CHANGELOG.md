## 2026-08-16
- fix(portal): S169-08 -- portals now trigger on proximity alone, no hotkey needed. Founder: "portals should work without a hotkey" / "jump in them and you go thru." Both apps/server/src/main.c's authoritative check and apps/lobby/src/main.c's local-mode client prediction required the USE-button edge trigger ANDed onto scene_portal_triggered's own proximity check; removed on both sides, vehicle enter/exit (shares the same button) split into its own still-keypress-gated branch. Existing anti-retrigger guards (portal_cooldown_until_ms, transition_timer) unchanged. Both binaries build clean; real server+bot regression smoke test (emily-bot, 3 bots) PASS. Apple #13751, commit d48e57c. (sess-20260813-2154-dda37e8b)

## 2026-08-10
- fix(ci): PLAY.bat now invokes ShankPit.exe directly + ends with pause, matching REDGARDEN's own proven PLAY.bat exactly, instead of 'start ShankPit.exe' (opens a detached window with zero error visibility and no pause -- if anything else ever goes wrong, e.g. a missing DLL or a crash, the player would see nothing at all, indistinguishable from 'stuck'). Same commit as the ticket-secret fix's own follow-up hardening. (sess-20260809-1420-e9d3d7f8)

- fix(net): CI-bundled Windows client never had a valid connect ticket — root cause of every 'stuck in Osaka garage, can't move' report since this shipped. mint_client_ticket() reads SHANKPIT_TICKET_SECRET from the environment; that var only ever existed in this server's own shell config, never in the distributed PLAY.bat. Every real downloaded client signed its ticket with an empty-string key, the server's verify_connect_ticket() silently dropped it (no slot, no WELCOME, nothing logged) -- looked identical to a network/firewall block from every angle that doesn't know to check this. Fixed both tests.yml and release.yml to set SHANKPIT_TICKET_SECRET in the generated PLAY.bat, same pattern REDGARDEN's own PLAY.bat already uses for its ticket secret. Live-verified: a client using only this env var (no other config) connects clean, real WELCOME, real snapshot reconciliation. (sess-20260809-1420-e9d3d7f8)

## 2026-08-09

- ops: restarted stale live server (running since Aug 4 15:50, 5 days pre-dating the connect/movement priming-fix commits) + rebuilt/relaunched the emily-bot pool (bin/emily-bot binary had gone missing, rebuilt from apps2/emily-bot). Live-verified end-to-end under Xvfb: fresh client connects and moves cleanly (reconcile ack incrementing, no deadlock) against the fresh server. Root cause of founder's 'stuck in Osaka garage' report is almost certainly a stale cached client build predating the Aug 4 fixes -- code itself is correct and verified working (sess-20260809-1420-e9d3d7f8)

## 2026-08-04 (6)
- feat(input): real Xbox controller support -- pressure-sensitive fire (real trigger travel), dual-stick move/look, A/B/X/Y mapped to jump/crouch/reload/use. Keyboard/mouse remain the real fallback. gcc clean.

# SHANKPIT-460 Changelog

## 2026-08-04 (5)

- fix(lobby): retry the initial CONNECT itself, not just the priming UserCmd. Proactive follow-up
  while investigating the priming-UserCmd retry fix earlier the same day -- same real class of
  bug, one stage earlier in the handshake: `net_connect()` was only ever called once at boot, with
  nothing to retry it if the initial CONNECT packet, or the server's WELCOME reply, is lost in
  transit. A lost CONNECT (or lost WELCOME) leaves the client showing its own local-mode fallback
  scene forever -- indistinguishable from every other "connected but nothing happens" symptom
  already chased down this session, just one handshake stage earlier. Retries every 2s for as
  long as `my_client_id` is still unset (never welcomed) -- safe to repeat since the guard means
  it only ever fires before any real WELCOME has landed, so no risk of double-joining or wasting a
  real one-seat-per-identity slot on an already-connected session. Verified live under Xvfb
  against the real local production server: clean single connect (exactly one real "CLIENT 10
  CONNECTED" log line, no retry spam once WELCOME lands), same real cmd_seen=1/Clients:10 result
  as the other fixes today -- no regression, real resilience against a failure mode nothing
  covered before. `gcc -Wall` clean. CI green with a real artifact (`ShankPit_Builds_15`).
  `7a6e202`.

## 2026-08-04 (4)

- fix(lobby): retry the priming UserCmd and the initial CONNECT itself, not one-shot. Founder,
  live, after testing the WELCOME-time priming fix: "ok still same thing i am queued into a game
  but i am stuck in osaka garage and cant move." Real gap: that fix sent exactly one priming
  UserCmd on WELCOME, a single unreliable UDP packet with no retry -- fine over this same box's
  own loopback (near-zero packet loss), but a genuinely remote connection over the real internet
  can lose that one packet and land back in the exact same stuck state with no recovery. Now
  retries on the real per-frame throttle interval for as long as `net_have_initial_local_
  snapshot_sync` hasn't flipped true. Same real gap found and fixed one stage earlier too: the
  initial CONNECT itself (`net_connect`) was also only ever sent once at boot, with nothing to
  retry it if the CONNECT packet or the server's WELCOME reply is lost -- now retries every 2s for
  as long as `my_client_id` is still unset, safe to repeat since it only ever fires before any
  real WELCOME has landed. Verified live under Xvfb against the real local production server both
  times: fresh connect immediately shows "slot=10 active=1 welcomed=1 cmd_seen=1
  player_active=1" and "Clients: 10", no regression from the original one-shot fix's own verified
  behavior, now with real resilience to packet loss neither original fix had. `gcc -Wall` clean.
  CI green with a real artifact (`ShankPit_Builds_13`). `68597e3`.

## 2026-08-04 (3)

- fix(lobby): break the connect/movement deadlock -- send a priming UserCmd on WELCOME. Founder,
  live, after the SERVER_HOST fix let a real remote client connect for the first time: "ok it
  says connected to okemily.com in the console i am in the osaka garage and i cant move and the
  dagger is equipped" -> "i dont see any enemies." Real, complete deadlock found by reading both
  sides of the wire protocol: the client's own per-frame movement block only ever calls
  `net_send_cmd` once `net_have_initial_local_snapshot_sync` is true (set the moment a real
  snapshot containing this client's own id arrives), but `server_broadcast` only includes a
  client in ANY snapshot once `slots[client_id].cmd_seen` is true -- set only inside
  `process_user_cmd`, which only runs on a real `PACKET_USERCMD` from that client. A freshly
  connected player could therefore never move and never appear in a snapshot for anyone else to
  see: the client waits for a snapshot that will never arrive because the server waits for a
  command that will never be sent. Likely never hit before this session -- the connect-ticket bug
  fixed earlier the same day meant no real remote client had ever gotten past the connection
  stage at all until today. Fixed by sending one real, neutral (zero movement, no buttons)
  UserCmd immediately on WELCOME -- the exact same packet shape/send path the normal per-frame
  loop already uses, just fired once up front. Verified live under Xvfb against the real local
  production server: before the fix, the new slot never appeared in the server's own periodic
  status log at all (client log showed real connect-attempt success with no server-side
  progress); after the fix, "slot=10 active=1 welcomed=1 cmd_seen=1 player_active=1" and
  "Clients: 10" (9 bots + the real client) appear immediately, and the client's own rendered
  camera position/world state visibly changes frame to frame (real snapshot-driven sync) instead
  of staying frozen on one static view. `gcc -Wall` clean. CI green with a real artifact
  (`ShankPit_Builds_11`). `ee69bfc`.

## 2026-08-04 (2)

- fix(lobby): default SERVER_HOST pointed at a real but unrelated server. Founder, live, after
  testing the direct-boot change: "i joined a game but its on s.farthq.com i think shankpit 460
  will be running on servers on the okemily server (this localhost)." Confirmed by DNS lookup:
  `s.farthq.com` resolves to `194.195.120.185`, a real but completely different server -- not this
  box. This box's own real public IP (`198.58.107.85`) is exactly `okemily.com`'s own DNS record.
  `shankpit460-server.service` and the real 9-bot pool this session already shipped both run on
  THIS box only -- a client defaulting to `s.farthq.com` could never see either one, matching the
  founder's own live report of "no bots" and a jittery/stale session on whatever that other server
  actually is. Default `SERVER_HOST` changed to `okemily.com`. Founder separately ran the
  already-queued `sudo-queue/10-shankpit460-firewall.sh` live during the same session, opening
  6969/udp for real external traffic. Verified DNS/connect-attempt locally (client log: "Connected
  to okemily.com..."); full external reachability (a real remote client, not same-box) couldn't be
  confirmed from this same box -- a same-box UDP round-trip test timed out both before and after
  the firewall rule, consistent with hairpin NAT (a box often can't reach its own public IP from
  inside itself, a different, unrelated limitation from whether a genuinely remote client can). CI
  green with a real artifact (`ShankPit_Builds_9`). `ddc5ea9`.

## 2026-08-04

- feat(server,lobby): deathmatch win condition, real client ticket auth, direct-boot to bot pool.
  Founder: "the client will boot into matchmaking directly - first into the bot pool - deathmatch
  first to 13 kills wins or 5minutes" -> "for now just the bot pool we will bring the lobby back
  once we get the bot matches working" -> "have the bot games be like 10 players so the bot pool
  should be 9." Added a real 13-kill win condition alongside the existing 5-minute timer
  (`complete_match`, checked every tick); default match length 10 -> 5 min to match. Considered
  and reverted a server-simulated bot-AI system before committing -- `bot_ai.h`'s own dead-code
  `bot_think()` looked like the fix, but this fork already has a proven, live, real bot-pool
  mechanism (`shankpit460-emily-bot.service`, real network-connected processes, the same
  REDGARDEN-parity pattern this fork's own backlog already names as the model, S170-83) --
  bumped that pool 6 -> 9 instead of duplicating it. Real bug found and fixed in the client
  while wiring the boot-flow change: `apps/lobby`'s own `net_connect()` never sent a connect
  ticket at all, despite the server requiring one (fail-closed) since S156-02 -- the real
  graphical client could never actually connect to the real production server. Fixed by minting
  a real, valid HMAC-SHA256 ticket client-side (mirrors `apps2/emily-bot`'s own already-proven
  approach), not yet tied to a real IDUNA identity (real, separate scope, explicit gap not a
  silent one). Client now boots directly into a networked bot-pool match instead of waiting at
  the lobby menu; lobby code left fully intact for later. Verified live end-to-end under Xvfb
  against the real local production server: client skipped the lobby, connected with a real
  ticket ("CLIENT 10 CONNECTED"), rendered the real in-game HUD alongside the 9 already-connected
  bots; kill-count win condition separately verified live ("MATCH_WIN_BY_KILLS client=3
  kills=13"). `gcc -Wall` clean. `5340dc3`.

## 2026-07-25

- docs: "NOT THE REAL SERVER" warning comments on the two dead server implementations
  (S155-02, `EMILY/BACKLOG.md`). `services/game-server/src/server.c` and `apps2/server-go/
  main.go` are both fully superseded by `apps/server/src/main.c` and confirmed unreferenced by
  any script, Makefile, or systemd unit in this repo — dead history, not live code. Per the
  backlog item's own framing ("don't delete unilaterally... at minimum add a loud comment until
  then"), took the minimal safe option rather than deleting or folding into a full NORTHSTAR
  scoping pass. No build/test impact — comment-only change, and neither file is part of `go.work`
  or any Makefile target regardless.

## 2026-07-24

- ops: emily-bot fill pool 1 -> 6 bots + new firewall script (S170-83, "operational parity" with
  REDGARDEN's Knights of the Void). Founder: "shankpit460 lobby and matchmaking pariuty to
  REDGARDEN knights of the void" → clarified as "same bot and pools setup"/"same matchmaking."
  Investigated first: shankpit-460's matchmaking (IDUNA queue + one persistent server) is an
  intentional, documented, different architecture from REDGARDEN's per-match ephemeral servers
  (`docs2/NORTHSTAR.md` §3), not an unfinished port -- flagged this rather than regressing it, and
  the founder confirmed operational-parity-only scope. `ops/systemd/shankpit460-emily-bot.service`
  bumped `-bots 1` → `-bots 6` (FFA deathmatch, not team-based, and this repo's own low-spec
  mandate argues against matching REDGARDEN's MOBA-scale 19). Verified live: server log shows all
  6 bots welcomed and fighting (`Clients: 6`, real hit events). New
  `sudo-queue/10-shankpit460-firewall.sh` opens 6969/udp -- same class of gap that caused
  REDGARDEN's S170-72/S170-85 (localhost bots bypass ufw entirely, masking a real external-connect
  blocker), checked proactively before it became a live incident here. Also worth noting the other
  direction: this repo's ticket-secret handling (`EnvironmentFile=`) is already better than what
  shipped for REDGARDEN tonight (`Environment=` plaintext) -- flagged in EMILY/BACKLOG.md S170-83
  as something to carry back the other way later, not copy from REDGARDEN.

## 2026-07-18
- feat(server): report match results to IDUNA on completion (S156-04) -- complete_match() now POSTs kills/deaths per authenticated client to IDUNA's existing POST /api/v1/players/{id}/session before the per-round reset, feeding the leaderboard/profile endpoints already consumed by `emily shankpit leaderboard`. Server authenticates as the new SHANKPIT460-SERVER M2M agent via IDUNA's existing POST /api/v1/auth/agent, using a new minimal self-contained HTTP/1.1 client (packages/common/http_client.h, no TLS, no external library) plus a tiny JSON field scanner. Deliberately best-effort, not fail-closed -- IDUNA being unreachable must never block the round timer. Caught live: first draft looked for a "token" field in the auth response, but IDUNA actually returns "access_token" -- would have silently no-op'd every match. End-to-end verified: direct-curl agent auth+POST confirmed via IDUNA's leaderboard, and a live match against emily-bot's unregistered player_ids correctly logged a graceful 404-and-continue per player without blocking match completion. Deployed to production. shankpit-460 8587f25.
- feat(server): IDUNA connect-ticket auth on PACKET_CONNECT (S156-02) -- server now requires a valid HMAC-SHA256 ticket (minted by IDUNA's new POST /api/v1/shankpit/ticket) before allocating a slot; fails closed if SHANKPIT_TICKET_SECRET is unset or the MAC/expiry don't check out. Also enforces one-seat-per-identity (VS2): a second concurrent connect for an already-connected player_id is rejected, not migrated. Self-contained HMAC-SHA256 (packages/common/hmac_sha256.h, no external crypto library), verified against RFC 4231 test vectors 1/2 and cross-checked against Go's crypto/hmac output. emily-bot gained matching ticket minting plus -bad-ticket/-no-ticket/-same-identity test modes. End-to-end testing surfaced and fixed a real auth bypass: PACKET_USERCMD used ensure_slot_for_sender, which auto-welcomes any unrecognized address regardless of ticket status -- a client could skip CONNECT entirely and get in free via USERCMD. Fixed by using the existing lookup-only find_slot_by_addr for USERCMD/DISCONNECT; only the verified CONNECT path may allocate a new slot. All four scenarios (valid, bad ticket, no ticket, duplicate identity) verified against an isolated instance before deploying to the production shankpit460-server systemd unit. shankpit-460 e78cc07.
- feat(server): match/round-boundary logic (S156-01) -- the server previously had none at all (local_init_match ran once at startup, no timer, no COMPLETE event). Added --match-minutes (default 10), a complete_match() that logs standings under a greppable MATCH_COMPLETE marker, resets kills/deaths, respawns everyone fresh (closed economy doctrine). Deliberately server-side only for this cut -- no wire protocol change. Live-verified with a 1-minute test match via emily-bot: fired on schedule with real nonzero standings, combat resumed cleanly. shankpit-460 718b2e9.
- fix(server): death now observable over the network -- katana_apply_damage (real hit function for all weapons) respawned synchronously within the same tick as a kill, so STATE_DEAD never persisted long enough for any snapshot broadcast to see it (deaths=0 respawns=0 confirmed across 3 separate combat E2E runs despite real damage). Fixed with a respawn_delay_ticks countdown (~2.9s) decremented in update_entity; removed the old respawn_time check which was dead code for PvP and non-functional (always zero-delay) anyway. Also clamps health to 0 on death (was leaving it negative, which wraps to garbage on the wire's unsigned char). Live-verified: deaths/respawns now nonzero and consistent. shankpit-460 d185fc5.
- fix(deploy): deploy_linux.sh built the dead, non-compiling services/game-server/src/server.c instead of the real apps/server/src/main.c -- delegated to 'make server' instead. Verified: clean rebuild produces a binary byte-identical (md5sum match) to the one already running in production. shankpit-460 c38657c.
- Forked from `SHANKPIT` at tag `460` (commit `55b80f7`). New project codename `shankpit-460`:
  stripped-down, low-system-spec competitive esports FPS, targeting a large global audience.
  Full history preserved up to the fork point; diverges independently from here forward. Not yet
  scoped into a design/stripping plan — that's the next real step (NORTHSTAR.md).

## 2026-07-19
- fix(ci): workflows triggered on branches: ["master"] but this fork's actual default branch is
  "main" (forked from SHANKPIT, which really is master; nobody updated the copied workflows) — CI
  has never fired on normal push/PR activity, only workflow_dispatch/tags. Fixed in both
  release.yml and tests.yml.
- fix(ci): SDL2 dependency was pinned to 2.28.5 via a bare libsdl.org direct download (no retry) —
  brought to parity with the parent SHANKPIT repo's already-fixed 2.30.10 via GitHub releases with
  --retry-connrefused --tries=3.

## 2026-07-19 (3)
- feat(map): external map file format + v0 launch deathmatch map — geometry moves out of
  hardcoded C for the first time. New packages/common/map_loader.h (plain-text format: box/
  spawn/poi lines, '#' comments; the exact shape of the live Box primitive, nothing richer) is
  loaded at startup by both the live server (apps/server/src/main.c) and the live client
  (apps/lobby/src/main.c), served under SCENE_STADIUM with soft fallback to the old hardcoded
  stadium if the file is missing/malformed. $SHANKPIT_MAP selects the file. maps/v0_shankpit.map
  is the first map authored in the format, per founder brief: a base at each end (future CTF
  flag stands — geometry stays CTF-plausible, ships DM-only), a tiered mid base with the
  rocket-launcher spawn poi on its crown (marker only; no pickup system exists yet — flagged),
  intentional asymmetry, rocks on the flanks and ends. release.yml now ships maps/ in both the
  client and world bundles (a client/server pair must load the same file or their geometry
  disagrees). Verified two ways: a 17-check harness against the built physics (load, envelope,
  spawns hit both bases, mid-crown climbable tier-by-tier under real jump constants, projectile
  collision, perimeter push-back, bad-file fallback) and a live playtest — 6 emily-bot clients
  connected, fought, died, respawned directly on the new map.
- ops(server): $SHANKPIT_PORT (bind port override) and $SHANKPIT_CONNECT_SCENE=stadium (drop
  new connections straight onto the arena map instead of the garage hub). Defaults unchanged.
  These are the first two ops primitives of the map bot-eval loop, and what made the live
  playtest possible next to the running production server.
- docs: docs2/maps-report.md — Track B report (report only, no code): LLM map generation against
  the new format, editor strategy (text-first, then in-game overlay; explicitly not a standalone
  tool), git-as-map-versioning argued, and the fuzz/bot-eval loop framed as a NORN instantiation
  (HQ-SPEC-PRIME-101) with a concrete oracle metric (engagement-dispersion entropy x
  participation) and a prime_ack gate tier argued per the spec's own framework. Two findings
  flagged for the founder: hitscan weapons never consult map geometry (bullets pass through
  walls — only projectiles collide; real competitive-integrity gap), and the "Super Rumble, more
  turbo" physics ask recorded as a concrete +10-15% MAX_SPEED delta proposal on current
  constants, deliberately not applied without sign-off.

## 2026-07-19 (2)
- ops: shankpit460-emily-bot.service — keeps one emily-bot permanently queued into the live game
  server so there's always an opponent present. ops/systemd/shankpit460-emily-bot.service; ticket
  secret via EnvironmentFile, not a CLI flag (avoids leaking it into ps/systemctl status output).
- docs(northstar): section 7 — spatial audio backlog. SHANKPIT (parent) already has a working
  SDL2 spatial audio engine (packages/audio/) with MIDI-style synthesized placeholder tones and
  yaw/distance panning — this is a port, not a fresh build. Real missing piece: no
  interface/abstraction to later swap in real sound assets. Not started, design record only.
