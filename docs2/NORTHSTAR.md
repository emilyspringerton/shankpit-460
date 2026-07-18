# NORTHSTAR — SHANKPIT-460 Esports Fork

**Status:** Draft v0 — first real scoping pass, per this repo's own CLAUDE.md ("write a real
NORTHSTAR.md before cutting code"). Founder direction captured verbatim below; this document
turns it into a build plan grounded in what already exists, not a wishlist.

**Mission (from CLAUDE.md, unchanged):** strip SHANKPIT into a lean, competitive esports FPS.
Low system specs required, competitive-first, not persistent-world-first.

**New scope, this pass (founder, 2026-07-18, verbatim across several messages):**
"esports will need accounts and at least basic matchmaking skip the backpack problem keep it
simple to start" / "refer to the 14 iduna northstars to see how shankpit esports might fit into
concepts established in those docs" / "basic web based stats like league of legends"

---

## 1. Don't build a bespoke system — this is a declared IDUNA consumer domain

IDUNA's own roadmap (`IDUNA/docs/kikoryu/VS0-VS13`, the "14 KIKORYU northstars") already declares
**a social tournaments platform as its primary product direction** (`VS2_TOURNAMENTS.md`:
`status: not-yet-built-still-relevant — THE PRIMARY PRODUCT DIRECTION`). Poker (NLHE) is named as
the *first* game on that platform, not the only one. SHANKPIT is already named inside that same
roadmap: `VS8_AURABORIALIS.md` restates VS8's reboot doctrine as "SHANKPIT season lineage."
Building shankpit-460 accounts/matchmaking as a standalone system would duplicate a platform the
founder has already committed IDUNA to. The right shape is: **shankpit-460 is the second game on
the VS2 tournaments platform**, reusing its identity, lifecycle, and economy doctrine, with its
own match-format specifics (deathmatch, not poker hands).

| Need | Reuse from | Status |
|---|---|---|
| Accounts / identity | VS0 (`IDUNA/docs/kikoryu/VS0_IDENTITY_GATE.md`) — Google OAuth, `gamertag`, THE_HONOR_CODE | **Already built for SHANKPIT specifically**: `IDUNA/internal/http/handlers/shankpit_auth.go` — full OAuth flow, issues an IDUNA JWT with `player_id`/`display_name`, redirects to `shankpit://auth?token=...`. Gap: the actual game server (`apps/server/src/main.c`) never validates this token — `PACKET_CONNECT` accepts any UDP packet with no auth check at all. |
| Match lifecycle | VS2's state machine shape (`CREATED → REGISTERING → STARTING → IN_PROGRESS → COMPLETE`), not its poker mechanics | Not built. See §3. |
| "No backpack" | VS2's **closed, non-redeemable economy** doctrine, verbatim: "chips exist only inside a tournament instance; every entrant starts with an equal stack. No persistent bankroll, no farming." | Already true of the running server's actual behavior — `phys_respawn` resets every player to a default loadout on spawn/respawn, no persistent inventory exists anywhere in `PlayerState`. **This is a non-goal to formally adopt, not a system to build.** |
| Stats / standings | VS10 (`IDUNA/docs/kikoryu/VS10_SCOREBOARDS.md`) — "derived recomputable projections... permanent archive" | Partially built: `IDUNA/internal/http/handlers/players.go` already has `GET /api/v1/players?sort=kills&limit=N` (leaderboard) and a per-player profile endpoint; `emily shankpit leaderboard` (emily.cli) already consumes it. Gap: nothing in shankpit-460 ever writes to it — see §4. |
| Reputation (later, not v0) | VS9 (`IDUNA/docs/kikoryu/VS9_REPUTATION.md`) — reliability/conduct signals | Not needed for "keep simple to start"; noted as the natural next layer once matchmaking exists (abandon-rate-aware queue priority). |

## 2. Accounts (v0)

Reuse `shankpit_auth.go` as-is — it already does exactly what's needed (Google OAuth → IDUNA
`players` row → JWT with `player_id`). New work is entirely on the game-server side:

- `PACKET_CONNECT` must carry the JWT (SHANKPIT's parent-repo `emily-bot` already has the wire
  convention for this — `SHANKPIT_AUTH_TOKEN` env / `~/.shankpit/auth.json`, a null-padded JWT
  field appended to the connect packet — shankpit-460's current `NetHeader`-only connect packet
  has no room for this; needs a new, versioned connect packet shape, or a JWT field appended
  after the existing 12-byte header).
- Server validates the JWT against IDUNA's JWKS before calling `ensure_slot_for_sender` — reject
  (no Welcome sent) on invalid/missing token. **One seat per identity** (VS2's hard constraint,
  directly applicable): reject a second concurrent connection for the same `player_id`.
- `slots[]`/`PlayerState` gain a `player_id` (currently only tracks a UDP `sockaddr_in` and an
  ephemeral slot index) so match results can be attributed to a real account, not a session.

## 3. Matchmaking (v0 — simplest viable, per "keep it simple to start")

Not VS2's full poker lifecycle — its *shape*, reduced to what a deathmatch quick-match actually
needs:

`QUEUING → STARTING → IN_PROGRESS → COMPLETE`

- **QUEUING**: authenticated players `POST` (or send a lobby-scene interaction — `apps2/lobby`
  already exists as a client-side voxel lobby scene, currently just a render target, not wired
  to anything) to join a queue. No skill-based matching in v0 — first-N-in, first match out (a
  min-player threshold, e.g. 2, starts a match). Skill-based queueing is a VS9-reputation-layer
  upgrade, explicitly deferred.
- **STARTING**: queued players get connect info for the (already-running, single) game server
  instance; v0 does **not** spin up per-match server instances — there is one persistent server,
  matches are just "who's currently active in it," matching today's actual deployment. Multiple
  concurrent match instances is a real future need (the founder's esports framing implies it
  eventually) but is out of scope for a first cut — noted as an open question in §5.
- **IN_PROGRESS**: unchanged from today's server behavior (deathmatch, `MATCH_DURATION` timer
  already exists in `services/game-server` — need to confirm/port an equivalent timer exists in
  the real `apps/server/src/main.c`, since that file wasn't checked for a match-duration/round-end
  concept during this pass).
- **COMPLETE**: match end triggers a result write — kills/deaths/duration per `player_id` — as an
  event (matching this codebase's house pattern: event-sourced, recomputable projections, not
  direct counter increments) that feeds §4's stats.

## 4. Web-based stats ("like League of Legends")

LoL's model — recent-match history + aggregate profile stats, public, per-account — maps almost
exactly onto what VS10 already prescribes ("derived recomputable projections... permanent
archive") and onto endpoints that already exist:

- `GET /api/v1/players?sort=kills&limit=N` — leaderboard. **Already live**, already consumed by
  `emily shankpit leaderboard`.
- Per-player profile endpoint — **already live** (`handleGetProfile`, slug-based).
- Missing: nothing currently *writes* match results into this store from shankpit-460 (§3's
  COMPLETE step is what closes that gap), and there's no actual **web page** yet — build one the
  same way `okemily.com/status.html` was built (static HTML, `fetch()` against the IDUNA JSON
  endpoint, no build step, no framework) rather than a new stack. Given `okemily.com` is the
  existing public front door, this likely belongs there or on a `shankpit.` subdomain per the
  FATES naming doctrine (`HQ-SPEC-INFRA-105`), not a new hosting setup.

## 5. Explicit non-goals for this pass (write these down so they don't get improvised later)

- No persistent inventory/loadout/cosmetics ("the backpack problem") — adopted as permanent
  doctrine (VS2's closed-economy reasoning), not just deferred.
- No skill-based matchmaking (MMR/ranking) — v0 is join-a-queue, not ranked. VS9 reputation is
  the natural upgrade path when this is revisited.
- No per-match server instances / dedicated match orchestration — v0 assumes the one persistent
  server IS the match; real concurrent-match support is a bigger infra question (ties into the
  deferred "Emily cluster"/dedicated-DB-node conversation from PRRJECT_FATBABY's own scaling
  work) and shouldn't be improvised here.
- No paid entry / entitlements — VS6's Stripe rails exist and are the mandated reuse path *if*
  this ever becomes paid, but nothing here assumes or blocks that.

## 6. Open questions

- ~~Does `apps/server/src/main.c` have a real match/round boundary today?~~ **Resolved, verified
  2026-07-18**: no. `local_init_match` runs once at startup; there is no round timer, no win
  condition, no COMPLETE event of any kind — the server just runs indefinitely with continuous
  respawn (the dead C server's `MATCH_DURATION`/kill-reset-on-timer concept does not exist in the
  real one). This means §3's COMPLETE step has no natural trigger yet: v0 needs an explicit
  match-end condition (timer, score cap, or an admin/director action) added to the real server
  before match results can ever be written — this is new server logic, not just wiring existing
  logic into the queue.
- Connect-packet JWT framing: extend the existing `NetHeader`+payload shape, or version the
  protocol? Given three incompatible server implementations already exist in this repo
  (`EMILY/BACKLOG.md` SECTION 155), any protocol change here should be the one moment this gets
  cleaned up, not another silent fork.
- Where does the stats page actually live (okemily.com path vs. new `shankpit.` subdomain)? Ties
  to FATES (`HQ-SPEC-INFRA-105`) naming doctrine, not a call to make in isolation.

## Build order

1. Verify match/round-boundary reality server-side (open question above) before anything else.
2. Wire JWT auth into `PACKET_CONNECT` (§2) — this is the actual prerequisite for everything
   else; matchmaking and stats are both meaningless without real per-account identity.
3. Minimal queue + QUEUING/STARTING/IN_PROGRESS/COMPLETE state machine (§3).
4. Match-result event → existing `/api/v1/players` projection (§4 backend half).
5. Static stats page (§4 frontend half) — smallest, most independently shippable piece; could
   even land before §3 against manually-entered/test data if sequencing makes sense.

Golden-indexed as SHANKPIT460-NORTH once landed (`EMILY/context/golden-docs-index.md`).
