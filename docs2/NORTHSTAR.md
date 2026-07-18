# NORTHSTAR — SHANKPIT-460 Esports Fork

**Status:** Draft v0 — first real scoping pass, per this repo's own CLAUDE.md ("write a real
NORTHSTAR.md before cutting code"). Founder direction captured verbatim below; this document
turns it into a build plan grounded in what already exists, not a wishlist.

**Mission (from CLAUDE.md, unchanged):** strip SHANKPIT into a lean, competitive esports FPS.
Low system specs required, competitive-first, not persistent-world-first.

**New scope, this pass (founder, 2026-07-18, verbatim across several messages):**
"esports will need accounts and at least basic matchmaking skip the backpack problem keep it
simple to start" / "refer to the 14 iduna northstars to see how shankpit esports might fit into
concepts established in those docs" / "basic web based stats like league of legends" / "we need a
name for the social tournament platform norse theme simple short memorable maybe we call it WOTAN"

**Name: WOTAN.** The platform this document scopes (§1–4) — VS2-shaped tournament identity,
matchmaking, and stats, of which SHANKPIT-460 is the first game. Live at
`okemily.com/tournaments.html`. Norse-mythic naming continues this codebase's existing convention
(NORN, FATES, KIKORYU) rather than introducing a new one.

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
| Accounts / identity | VS0 (`IDUNA/docs/kikoryu/VS0_IDENTITY_GATE.md`) — Google OAuth, `gamertag`, THE_HONOR_CODE | **Built and live (S156-02, 2026-07-18)**: `IDUNA/internal/http/handlers/shankpit_auth.go` (OAuth flow, issues IDUNA JWT) + `shankpit_ticket.go` (`POST /api/v1/shankpit/ticket` mints a short-lived HMAC-SHA256 connect ticket bound to `player_id`). `apps/server/src/main.c`'s `verify_connect_ticket` now checks every `PACKET_CONNECT` before allocating a slot, fails closed if `SHANKPIT_TICKET_SECRET` is unset, and enforces one-seat-per-identity via `find_slot_by_player_id`. (Design note: this uses a game-specific HMAC ticket, not JWT-in-C — founder chose that over implementing ECDSA verification in C.) |
| Match lifecycle | VS2's state machine shape (`CREATED → REGISTERING → STARTING → IN_PROGRESS → COMPLETE`), not its poker mechanics | **Built and live (S156-01/03).** `IDUNA/internal/http/handlers/shankpit_queue.go` implements `QUEUING → matched` (v0 collapses STARTING into the match response, since there's only one server to connect to). `apps/server/src/main.c`'s `complete_match()` provides the round-boundary/COMPLETE trigger. See §3. |
| "No backpack" | VS2's **closed, non-redeemable economy** doctrine, verbatim: "chips exist only inside a tournament instance; every entrant starts with an equal stack. No persistent bankroll, no farming." | Already true of the running server's actual behavior — `phys_respawn` resets every player to a default loadout on spawn/respawn, no persistent inventory exists anywhere in `PlayerState`. **This is a non-goal to formally adopt, not a system to build.** |
| Stats / standings | VS10 (`IDUNA/docs/kikoryu/VS10_SCOREBOARDS.md`) — "derived recomputable projections... permanent archive" | **Built and live (S156-04).** `IDUNA/internal/http/handlers/players.go`'s `GET /api/v1/players?sort=kills&limit=N` (leaderboard) and per-player profile endpoint are now actively written to: `complete_match()` POSTs kills/deaths per `player_id` to `POST /api/v1/players/{id}/session` on every match completion, authenticated as the new `SHANKPIT460-SERVER` M2M agent (gated behind a new `shankpit.match.write` permission — that endpoint previously had no permission check at all, a real gap found and closed alongside this work). Only the static web stats page (§4 frontend half, S156-05) remains open. |
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
- ~~Connect-packet JWT framing: extend the existing `NetHeader`+payload shape, or version the
  protocol?~~ **Resolved 2026-07-18 (S156-02)**: not JWT-in-C at all — founder chose a simple
  HMAC-SHA256 session ticket instead (IDUNA mints, C server verifies locally with no crypto
  library and no I/O), appended after the existing 12-byte `NetHeader`. Avoids the protocol-version
  question entirely for this cut.
- Where does the stats page actually live (okemily.com path vs. new `shankpit.` subdomain)? Ties
  to FATES (`HQ-SPEC-INFRA-105`) naming doctrine, not a call to make in isolation.

## Build order

1. ✅ Verify match/round-boundary reality server-side (open question above) before anything else. — S156-01, done.
2. ✅ Wire connect-ticket auth into `PACKET_CONNECT` (§2) — this is the actual prerequisite for everything
   else; matchmaking and stats are both meaningless without real per-account identity. — S156-02, done.
3. ✅ Minimal queue + QUEUING/matched state machine (§3). — S156-03, done.
4. ✅ Match-result event → existing `/api/v1/players` projection (§4 backend half). — S156-04, done
   (scoped down from "event-sourced" to a direct counter-increment call against the endpoint that
   already existed — reuse over redesign).
5. ⬜ Static stats page (§4 frontend half) — smallest, most independently shippable piece — the
   only item still open. See `EMILY/BACKLOG.md` SECTION 156, S156-05.

Golden-indexed as SHANKPIT460-NORTH once landed (`EMILY/context/golden-docs-index.md`).
