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

## README Reality — SAGA reconciliation (standing instruction, monorepo-wide)

Founder real-time, 2026-09-18: if a change of yours **substantially changes the claim of this project's core README**,
then per SAGA protocols (`EMILY/docs/SAGA_SYSTEM_AUDIT_2026-07-18.md`, HQ-SPEC-DOC-102: intent ↔ claim ledger ↔ reality)
you **must update `README.md` in the same unit of work** so it reflects current reality. The README is the project's public
claim; it must not lag behind the code.

- **When it applies:** a capability is added or removed; status moves ("design only" → "working", "planned" → "shipped");
  the stack, build, run or install steps change; a claim in the README is now false or stale; or you add a **meaningful,
  genuinely interesting piece of kit** (a new tool, engine capability, protocol, pipeline, game system). For that last case
  especially: put it in the README — what it is, how to run it, and its honest status and limits.
- **When it does not:** ordinary fixes, refactors and small features that leave the README's claims true.
- **How:** re-read the README against what you just changed; fix or delete stale lines (including "not built yet" notes that
  are now built); verify any new claim by actually running it, and mark anything untested as untested; commit the README
  with (or immediately after) the change, and mention it in the CHANGELOG entry.

## Frame-Break Reframing

Founder-sourced prompting technique (REDGARDEN/NORTHSTAR.md §28, full origin in
REDGARDEN/docs2/MULTI_AGENT_RD_RESEARCH_NOTES.md §5): given a request, name the underlying
structural/systemic pattern it's one instance of — one level of abstraction up — as an added
lens during planning/triage/judgment calls. Use it to spot the general case behind a specific
ask. It augments judgment, it does not replace doing the work: direct, concrete execution of
the literal task asked for still happens every time.

## Commit Protocol (standing instruction)

Always commit and push completed work immediately — don't wait to be asked. This is the default for every repo in this monorepo.

Every commit — human-written or produced by automated code paths (git-commit helpers in emily-agent, emily.cli, IDUNA handlers, etc.) — must carry the active `emily session` fingerprint as a `session: <tag>` trailer (blank line, then the trailer). This was silently missing from several independently-implemented automated commit helpers across the monorepo until an audit on 2026-08-10 (founder, real-time: "where in the fuck is my llm session id anywhere"). If you add a new automated git-commit code path anywhere, wire in the session tag the same way — don't assume an existing helper already does it.
