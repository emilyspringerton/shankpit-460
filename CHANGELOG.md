# SHANKPIT-460 Changelog

## 2026-07-18
- fix(deploy): deploy_linux.sh built the dead, non-compiling services/game-server/src/server.c instead of the real apps/server/src/main.c -- delegated to 'make server' instead. Verified: clean rebuild produces a binary byte-identical (md5sum match) to the one already running in production. shankpit-460 c38657c.
- Forked from `SHANKPIT` at tag `460` (commit `55b80f7`). New project codename `shankpit-460`:
  stripped-down, low-system-spec competitive esports FPS, targeting a large global audience.
  Full history preserved up to the fork point; diverges independently from here forward. Not yet
  scoped into a design/stripping plan — that's the next real step (NORTHSTAR.md).
