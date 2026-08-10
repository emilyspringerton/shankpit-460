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

## Founder Real-Time Direction

Whenever the founder gives real-time direction — a new ask, a correction, a "can we also..." —
route it through `emily observe -s info "Founder real-time: <summary>"` first, even if it isn't
this repo's usual domain, then sprint-plan it into `EMILY/BACKLOG.md` (`emily backlog curate`,
scoped into a real SECTION/sub-item, not just a one-line log), and only then implement. See
`EMILY/docs/THE_EMILY_WAY.md` Principle 18 ("Pave the Cow Paths").

## Frame-Break Reframing

Founder-sourced prompting technique (REDGARDEN/NORTHSTAR.md §28, full origin in
REDGARDEN/docs2/MULTI_AGENT_RD_RESEARCH_NOTES.md §5): given a request, name the underlying
structural/systemic pattern it's one instance of — one level of abstraction up — as an added
lens during planning/triage/judgment calls. Use it to spot the general case behind a specific
ask. It augments judgment, it does not replace doing the work: direct, concrete execution of
the literal task asked for still happens every time.

## Commit Protocol (standing instruction)

Always commit and push completed work immediately — don't wait to be asked. This is the default for every repo in this monorepo.
