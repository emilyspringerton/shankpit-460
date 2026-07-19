# Maps Pipeline Report — editor, AI generation, fuzz/bot evaluation

**Status:** Report only (2026-07-19). Track B of the two-track map strategy — nothing in here is
built except where explicitly marked SHIPPED. Founder direction, verbatim: "we want to use
intelligence to attempt to generate maps and also they need fine tuning via a level editor and
versions and also we can fuzz levels and run bots and see where the most emergent team behavior
emerges as we attempt to generate new competitive fps levels/maps." And on sequencing: "we will
have to keep the bare bones approach at first but can figure out how to optimize the ops
[later]." This report takes the second quote as binding on the first.

---

## 0. What shipped today (Track A), and why it's the pipeline's foundation

The one real dependency shared by an editor, AI generation, and fuzz-eval is that maps must be
**data, not C source**. That step is done:

- `packages/common/map_loader.h` — SHIPPED. Plain-text map format (`box`/`spawn`/`poi` lines,
  `#` comments), loaded at startup by both the live server (`apps/server/src/main.c`) and the
  live client (`apps/lobby/src/main.c`), with soft fallback to the old hardcoded stadium if the
  file is missing or malformed. `$SHANKPIT_MAP` selects the file.
- `maps/v0_shankpit.map` — SHIPPED. The launch deathmatch map, first map authored in the format:
  a base at each end (future CTF flag stands), a tiered mid base with the rocket-launcher spawn
  point on its crown (`poi rocket_launcher` — marker only, no pickup system exists yet),
  intentional asymmetry, rocks at the sides and ends. Playtested live: 6 `emily-bot` clients
  connected, fought, died, and respawned on it (see §3 for the ops hooks that made that possible).
- `$SHANKPIT_PORT` and `$SHANKPIT_CONNECT_SCENE=stadium` — SHIPPED. A second server instance can
  run beside the deployed one, and drop connections straight onto the loaded map instead of the
  garage hub. These two env vars are, deliberately, the first two ops primitives of the bot-eval
  loop below.

Everything below targets this format. That is the point of the format.

## 1. AI-assisted generation ("use intelligence to attempt to generate maps")

Given the format is ~40 lines of `box x y z w h d`, "intelligence generating maps" does not need
anything exotic to start. Concretely:

- **v1: LLM proposes a complete map file.** A prompt containing the format spec, the engine
  constants that matter (player height 6.47, tap-jump ~6, held-jump ~18, stadium envelope
  |x|,|z| ≤ 420), and a design brief ("two defensible ends, contested mid power position, no
  clean cross-map sightline at eye height") → the model emits a candidate `.map` file. This is
  cheap enough to generate dozens of candidates per session with claude-haiku.
- **A mechanical validator, not vibes.** Today's `map_verify` harness (written for v0, currently
  in scratchpad — promote it to `tools/map_verify/` as the first pipeline build item) already
  checks: parses cleanly, floor at index 0, everything inside the envelope, spawns in bounds and
  distributed across both ends, mid structure climbable tier-by-tier under real jump physics,
  projectile collision works. Generated candidates that fail the validator are discarded before
  any bot time is spent. Constraint checks worth adding: min/max sightline lengths (sample
  eye-height rays between spawn pairs), symmetry tolerance (mirror-distance metric with an
  allowed band — the brief *wants* bounded asymmetry, so this is a band, not zero), reachability
  (flood-fill over jumpable box tops), and per-spawn cover within N units.
- **Not yet:** learned generators, diffusion over layouts, evolutionary search. The
  propose→validate→bot-eval loop must exist and produce trustworthy scores first; a fancier
  proposer just plugs into the same Skuld slot later (§3).

## 2. Level editor + versioning ("fine tuning via a level editor and versions")

- **v0 editor is a text editor.** This is a real position, not a dodge: the format is one line
  per box, the NORTHSTAR mandates low-spec/no-heavyweight-pipeline, and `SHANKPIT_MAP=maps/x.map
  make lobby && bin/shank_lobby` is already a functional edit-reload loop. v0_shankpit.map was
  authored exactly this way today, with the validator as the safety net.
- **v1 editor: in-game overlay, not a separate tool.** The client already renders `map_geo` and
  has free-fly camera plumbing; a `--editor` mode needs: box under crosshair highlighted, nudge
  position/size on keys, add/delete box, write the `.map` file back out. That is a few hundred
  lines against code that exists, versus a separate tool that would need its own renderer,
  camera, input, and a second parser to keep in sync. A standalone editor app is the classic
  heavyweight-pipeline trap the NORTHSTAR forbids.
- **Versioning: git, and here's the argument rather than the assumption.** (a) Map files are
  small line-oriented text — diffs are meaningful ("moved mid crown 4 units east" is a one-line
  diff), merges work, blame answers "who moved this wall and when." (b) Every other durable
  artifact in EINHORN_INDUSTRIAL is already git-native (APPLES, golden docs, this repo) — a
  separate map-version store would be a second system to operate for zero marginal capability.
  (c) NORN (§3) needs content-addressed artifacts with lineage; git commits *are*
  content-addressed history, and the candidate hash can literally be the blob sha. What git does
  NOT give us is the quality lattice — "which map is currently golden / in rotation" — which is
  exactly the part NORN's Registry owns. Split: **git stores bytes and history; NORN's registry
  stores promotion state pointing at commits.** Named maps live in `maps/`; generated candidates
  land in `maps/candidates/` (or a branch) and only promotion moves them.

## 3. Fuzz-testing + bot evaluation — a NORN instantiation, not a new loop

Per `EMILY/docs/hq-specs/HQ-SPEC-PRIME-101-norn-loop-kernel.md`: "domains stop owning loops;
they own instantiations." Map generation is the seventh-or-so re-derivation of
propose→grade→gate→promote; we should not re-argue the hazard analysis. The instantiation:

| NORN slot | Maps instantiation |
|---|---|
| **Artifact** | A candidate `.map` file, content-hashed; `Lineage` = generator prompt/version + validator version + parent map (if a mutation) |
| **Proposer (Skuld)** | §1's LLM generator, and a dumb fuzzer besides (random perturbations of an existing map: move/resize/duplicate/delete boxes, then re-validate) — both are just Proposers, NORN doesn't care |
| **Oracle (Verdandi)** | A frozen bot-eval protocol: fixed emily-bot build, fixed bot count, fixed duration and seed policy, scoring the metric below. Version = hash of (bot binary + eval config). Frozen means frozen — changing the bot AI mid-campaign invalidates comparisons, exactly per PRIME-101 §5 |
| **Gate** | No-regression vs. the incumbent rotation map on the metric, plus the mechanical validator as a hard invariant |
| **Registry (Urd)** | Promotion events; every promotion also files an Apple, per house rules |

**The oracle's metric — a concrete proposal, since "emergent team behavior" must be measurable.**
Primary metric: **engagement dispersion** — divide the map into a 20×20 XZ grid, record every
damage event's location over an N-bot M-minute run, and score the normalized entropy of that
kill/damage-density heatmap, multiplied by a participation factor (fraction of bots with at
least one kill *and* one death). Rationale: a degenerate map (one killbox, one dominant power
position, or unreachable zones) concentrates engagements → low entropy; a map that produces
rotating fights over mid, both bases, and the flanks spreads them → high entropy. Participation
punishes spawn-trap maps that entropy alone might score well. Secondary diagnostics (recorded,
not gated on, until we trust them): time-to-first-engagement (pacing), per-spawn 10-second death
rate (spawn safety), position variance per bot (movement, not camping). When TDM bots exist,
team-clustering metrics can extend this to genuinely *team* behavior — the metric slot is
versioned oracle config, so that's a rotation, not a redesign.

**Telemetry: what exists vs. what's needed.** `apps2/emily-bot` (the live bot — verified: it
targets the real `apps/server/src/main.c` wire protocol) already observes per-bot position, yaw,
health, weapon, deaths, respawns, damage dealt/taken from snapshots, and already exits
PASS/FAIL. Missing for the oracle: (a) damage/kill **locations** — emily-bot sees its own
position and peers' positions each snapshot, so death location is derivable client-side with no
server change: log `(tick, bot, x, z, event)` NDJSON to a file; (b) a `-scenario` flag bundling
fixed seed/duration/weapon policy so runs are comparable; (c) a tiny scorer that folds the
NDJSON into the metric. All bare-bones, all in Go, no C changes required for v1. One real C-side
caveat found while playtesting today, which distorts any cover-quality metric: **hitscan weapons
never consult map geometry** — `update_weapons` → `check_hit_location` does a sphere test per
target with no wall trace, so bullets shoot through walls (only projectiles collide, via
`trace_map`, which is itself an endpoint-in-box check, not a segment trace). For a competitive
FPS this is a real gameplay gap, not just an eval nuisance; flagged for the founder as its own
backlog item. Until fixed, the eval metric measures movement/spawn geometry more than cover.

**Gate tier — arguing `prime_ack`, per PRIME-101 §4's own framework.** `autonomous` requires a
reality-rooted oracle; bot-eval is a *simulation* of player behavior, and current bots are
aim-at-nearest patrollers whose "emergent behavior" is a weak proxy for humans — an early oracle
this unproven should not self-promote maps into the rotation real players see. `biometric` is
for promotion feeding physical/financial execution paths; a map rotation is not that. So:
**`prime_ack`** — bot-eval scores + validator + heatmap render attached to a promotion proposal;
Emily Prime (in practice, the founder at this stage) acks before rotation. Per PRIME-101, tier
relaxation is itself a gated promotion: once real-player telemetry (S156-04 match results, the
actual reality root here) accumulates and correlates with bot-eval scores, *that evidence* is
the eval for relaxing map promotion to `autonomous`. Quiescence (`quiesce_after_rejections`)
applies as-is: if 50 candidates in a row fail to beat v0_shankpit, the campaign stops burning
budget and says so.

## 4. Physics note — "Super Rumble, a little more turbo" (flagged, not changed)

The reference is a Meta Quest 3 Horizons title (Halo-ish FPS feel), external to this codebase,
so no numbers were invented and no constants were changed today. Current live tunables
(`packages/common/physics.h`): `MAX_SPEED 0.95`/tick, `ACCEL 0.6`, `FRICTION 0.15`,
`JUMP_FORCE 0.95`, `GRAVITY_FLOAT 0.025` / `GRAVITY_DROP 0.075`, plus slide (`SLIDE_FRICTION
0.01`) and katana dash at `5.8`/tick. At ~2.8 engine units per meter this is already
faster-than-Halo pacing. Proposal for founder sign-off, expressed as deltas on real values, not
absolutes: **+10–15% `MAX_SPEED` (0.95 → 1.05–1.10) with `ACCEL` raised to keep
time-to-max-speed constant, jump unchanged** — then A/B it on v0_shankpit with the bot loop's
time-to-first-engagement as a sanity metric and a human feel pass as the decider. One-line
change when approved; the map was sized for current speeds and stays valid at +15%.

## 5. Sequencing — next vs. later bets

**Next (small, each independently shippable, in order):**
1. Promote `map_verify` from scratchpad harness to `tools/map_verify/` and add the sightline /
   symmetry-band / reachability checks (§1). It is the shared gatekeeper for everything else.
2. emily-bot: NDJSON event log + `-scenario` flag + scorer (§3 telemetry). No C changes.
3. First generation campaign run by hand: LLM proposes ~20 candidates → validator filters →
   bot-eval scores → founder picks; file the results as Apples. This proves the loop *manually*
   before any NORN plumbing exists — bare bones first, exactly per the founder's sequencing.
4. Hitscan wall-occlusion fix in C (real segment trace, server-side) — gameplay correctness
   that also unblocks honest cover metrics.

**Later bets (real, but gated on the above proving out):**
- In-game overlay editor (§2 v1).
- Formal NORN instantiation (registry, `nornd` schedule, budgets) replacing the manual campaign
  — worth it only once campaigns run repeatedly.
- Smarter proposers (mutation search over scored populations), TDM/objective-mode bots and true
  team-behavior metrics, rocket-launcher weapon + pickup system so the v0 `poi` becomes live
  gameplay, and per-map envelope metadata (bounds/kill-Y in the file rather than inherited from
  the stadium scene).
