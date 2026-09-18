# Investigation: stdin/stdout <-> WebSocket bridging for ui_bridge_main

**Date:** 2026-09-18
**Status:** Resolved — component exists and is documented; earlier orientation pass missed it because it lives under `apps/web-client/server/`, not `src/`.

## Answer

The bridging happens in a small Node.js script:

- **File:** `apps/web-client/server/ws-bridge.mjs` (113 lines)
- It `spawn()`s the native `universe_ui_bridge` binary (built from `src/ui_bridge_main.cpp`) with `stdio: ['pipe', 'pipe', 'pipe']` (`ws-bridge.mjs:45-48`).
- It opens a `ws` package `WebSocketServer` bound to `127.0.0.1:19081` by default (`ws-bridge.mjs:9-10`, `ws-bridge.mjs:55`: `const wss = new WebSocketServer({ host, port, maxPayload: 16 * 1024 })`).
- Each line the child process writes to stdout is read via `readline` (`createInterface({ input: bridge.stdout })`, `ws-bridge.mjs:67-72`) and broadcast verbatim to all connected WebSocket clients.
- Each line on stderr is wrapped into a JSON `error` envelope and also broadcast (`ws-bridge.mjs:74-79`).
- Inbound WebSocket messages from the browser are written straight to the child's stdin (`bridge.stdin.write(...)`, `ws-bridge.mjs:89-95`).
- Origin allow-list restricts browser connections to `http://127.0.0.1:5173` / `http://localhost:5173` (`ws-bridge.mjs:50-53`, `81-86`).
- Port and bridge-binary path are overridable via `UNIVERSE_WS_PORT` and `UNIVERSE_BRIDGE_BIN` env vars (`ws-bridge.mjs:10`, `13`).
- On Windows (no `UNIVERSE_BRIDGE_BIN` override), the script itself shells out through Podman to run the binary inside the `universecore-dev` container image (`ws-bridge.mjs:22-34`: `podman run --rm -i -v <repo>:/workspace -w /workspace universecore-dev ./build/container/universe_ui_bridge`). On non-Windows it runs the locally built binary directly at `build/container/universe_ui_bridge` (`ws-bridge.mjs:36-37`).

## How it's started (dev workflow)

Confirmed in `apps/web-client/package.json`:
```
"scripts": {
  "dev": "vite",
  "build": "tsc -b && vite build",
  "preview": "vite preview",
  "bridge": "node server/ws-bridge.mjs"
}
```
(`apps/web-client/package.json:6-11`) — the `bridge` npm script is the canonical entry point (`npm run bridge` from `apps/web-client/`).

`ws` (`^8.18.0`) is a direct dependency of `apps/web-client` (`apps/web-client/package.json:12-15`), and `apps/web-client/node_modules/ws` is present on disk, confirming it's installed, not just declared.

`docs/PLAYCANVAS_WEB_CLIENT.md` documents the full three-process dev workflow end to end (lines 1-79):
1. Build the native bridge in the canonical Podman container (`Containerfile`, target `universe_ui_bridge`) — lines 39-46.
2. From `apps/web-client`: `npm install && npm run bridge` — lines 48-57. Comment at line 57 explicitly notes: "On Windows, `npm run bridge` launches `universe_ui_bridge` through Podman by default."
3. From `apps/web-client` in another terminal: `npm run dev -- --host 127.0.0.1 --port 5173` — lines 67-73.
4. Documented defaults: bind host `127.0.0.1`, WebSocket URL `ws://127.0.0.1:19081` (lines 59-61), matching `ws-bridge.mjs:9-10`.
5. Troubleshooting section (lines 146-151) explicitly references `ECONNREFUSED 127.0.0.1:19081` and instructs `npm run bridge` to fix it.

The architecture diagram in that doc (lines 7-21) names `apps/web-client/server/ws-bridge.mjs` explicitly as the "(localhost WebSocket adapter)" stage between `universe_ui_bridge` and the PlayCanvas client.

## Other locations checked (per the investigation brief)

1. **`scripts/`** — contains only container/build orchestration scripts (`container-build.sh`, `container-run.sh`, `container-test*.sh`, `container-ui-*.sh`, `build-windows-ui.ps1`, `run-windows-ui.ps1`, `windows-ui-cross-build.sh`, `run-viability-matrix.ps1`). None reference port 19081, WebSocket, or spawn+pipe `ui_bridge_main`. The actual bridge script lives under `apps/web-client/server/`, not `scripts/`.
2. **`apps/web-client/vite.config.ts`** — no dev-server proxy config; it only sets `server.host = '127.0.0.1'` and `server.port = 5173` (`vite.config.ts:1-8`). The browser client connects directly to `ws://127.0.0.1:19081` (confirmed separately in `apps/web-client/src/network.ts` per the task brief) rather than being proxied through Vite.
3. **`Containerfile`** and **`Containerfile.windows-ui`** — both are pure build-toolchain images (Ubuntu 24.04 + build-essential/cmake/ninja/git, the windows-ui variant adds mingw-w64). Neither has a `CMD`/`ENTRYPOINT`, and neither references port 19081 or any process supervisor. They only provide the environment used to compile `universe_ui_bridge`; the container is invoked ad hoc by `podman run` in `ws-bridge.mjs` and in `docs/PLAYCANVAS_WEB_CLIENT.md`, not by the container's own entrypoint.
4. **`docs/PLAYCANVAS_WEB_CLIENT.md`** and **`README.md`** — `PLAYCANVAS_WEB_CLIENT.md` fully documents the bridge (see above). `README.md` was not found to contain additional bridge-specific detail beyond what's in the PlayCanvas doc (general project README).
5. **Git history** — `git log --oneline --all -- apps/web-client/server/ws-bridge.mjs` returns a single commit: `5ef2de9 feat: add PlayCanvas web client and localhost WS adapter`. This is the same commit currently at `HEAD` per the branch's recent commit log, i.e. the bridge was added in one shot alongside the web client, not built incrementally or ever deleted/recreated. No evidence of an earlier, now-deleted websocket bridge attempt.
6. **Dependencies** — `ws` is declared and installed (see above) in `apps/web-client`. No `socket.io`, `websocket-stream`, or `stdio-websocket` packages found.
7. **Untracked files** — `git status --porcelain` (repeated at investigation time) shows only `?? docs/agents/` as untracked; `apps/web-client/server/ws-bridge.mjs` and `apps/web-client/package.json`'s `bridge` script are both tracked/committed (part of commit `5ef2de9`), not stray local files.

## Note on `docs/agents/`

`docs/agents/` (untracked at investigation time) contains `domain.md`, `issue-tracker.md`, and `triage-labels.md` — these are an issue-tracking/triage convention, not a research-notes convention, so this investigation was placed at `docs/research/websocket-bridge-investigation.md` instead (new `docs/research/` directory), per the fallback instructed in the task brief.
