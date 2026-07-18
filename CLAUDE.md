# SHANKPIT-460 — Codename for the Competitive Esports Fork

Forked from `SHANKPIT` at tag `460` (commit `55b80f7`, 2026-03-31, "Merge pull request #114 ...
add-katana-weapon-with-blade-dash-ability"). Full history up to that point is preserved; this repo
diverges from `SHANKPIT` from here forward as its own project.

## Mission

Strip SHANKPIT down into a lean, competitive esports FPS — "a stripped-down racecar" is the
guiding metaphor: minimal, tuned purely for competitive performance, no excess weight. Two
concrete constraints that follow directly from that metaphor and from the target audience:

1. **Low system specs required to run.** The whole point is reaching a large global audience,
   including players on hardware that couldn't run SHANKPIT's fuller feature set. Every system
   added here should be evaluated against what it costs on low-end hardware, not just whether it's
   fun on a dev machine.
2. **Competitive-first, not persistent-world-first.** SHANKPIT (the parent repo) carries
   DragonsNShit's persistent-world/season-lineage ambitions. This fork is explicitly NOT that — it
   inherits the UDP FPS core and server-authoritative model, not the persistent-world backend, the
   BedWars mini-game layer, or the TYLER/MPT narrative bridge. Those stay in `SHANKPIT`.

## Status

Just forked (2026-07-18) — this is the starting snapshot, not yet scoped into a design/stripping
plan. What specifically gets cut vs. kept, and what the actual esports feature set looks like
(ranked matchmaking? spectator mode? tick-rate targets for low-end hardware?), is real design work
still to be done — deliberately not improvised here. Treat the next step as writing a real
NORTHSTAR.md for this fork before cutting code, per the Emily Way's "spec before implementation."

## Inherited from SHANKPIT (verify current relevance before relying on any of this)

- Server-authoritative UDP FPS core, Go backend
- `go test ./...` before committing; dated `CHANGELOG.md` entries for meaningful changes
- See parent repo's `docs2/NORTHSTAR.md` for the persistent-world ambitions this fork is
  deliberately NOT carrying forward

## Related Repos

- `SHANKPIT` — the parent repo this was forked from; diverges from here forward
- `EMILY` — RSI loop / backlog coordination for cross-repo work
