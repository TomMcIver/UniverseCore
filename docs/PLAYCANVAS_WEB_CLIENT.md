# PlayCanvas Web Client (MVP Vertical Slice)

## Architecture

This repository now supports a first browser-facing renderer path without changing simulation authority:

```text
Native C++ Simulation (authoritative)
        │
        ▼
SimulationController + web protocol DTOs (engine-neutral read/control)
        │
        ▼
universe_ui_bridge (line-delimited local protocol process)
        │
        ▼
apps/web-client/server/ws-bridge.mjs (localhost WebSocket adapter)
        │
        ▼
apps/web-client (TypeScript + PlayCanvas)
```

Rules preserved:
- C++ remains authoritative for all simulation logic.
- Frontend remains presentation/input only.
- Commands are queued and applied on the bridge simulation loop.
- Browser timing does not advance canonical simulation logic.

## Prerequisites (verified)

- Windows 10/11
- Node.js + npm
- Podman machine running

No host MSVC toolchain is required for this workflow because the bridge is built and executed in Podman.

## Local Run (verified on this repository)

### 1) Build native bridge in canonical container

From repository root:

```bash
podman build -t universecore-dev -f Containerfile .
podman run --rm -v "${PWD}:/workspace" -w /workspace universecore-dev bash -lc "cmake -S . -B build/container -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build/container --target universe_ui_bridge"
```

### 2) Start localhost WebSocket transport

From `apps/web-client`:

```bash
npm install
npm run bridge
```

On Windows, `npm run bridge` launches `universe_ui_bridge` through Podman by default.

Defaults:
- bind host: `127.0.0.1`
- WebSocket URL: `ws://127.0.0.1:19081`

Optional env vars:
- `UNIVERSE_WS_PORT`
- `UNIVERSE_BRIDGE_BIN` (override default bridge command)

### 3) Start PlayCanvas client

From `apps/web-client` in another terminal:

```bash
npm run dev -- --host 127.0.0.1 --port 5173
```

Open:

```text
http://127.0.0.1:5173/
```

Expected connection state:

```text
Disconnected -> Connecting -> Connected
```

## Protocol (v1)

Envelope shape:

```json
{
  "protocolVersion": 1,
  "type": "initial_snapshot",
  "tick": 123,
  "payload": {}
}
```

Server -> client:
- `hello`
- `initial_snapshot`
- `world_delta`
- `entity_details`
- `simulation_status`
- `world_event`
- `error`

Client -> server:
- `hello`
- `pause`
- `resume`
- `set_speed`
- `single_step`
- `request_snapshot`
- `request_entity_details`

## Adding a renderer-visible field

Follow this order:

```text
simulation canonical state
→ SimulationController read-only access
→ web protocol DTO/message
→ TypeScript protocol type
→ renderer/UI presentation
```

Do not add simulation logic to TypeScript.

## Adding a command

Follow this order:

```text
UI command emit
→ protocol validation parse_client_command
→ bridge command queue
→ simulation loop applies command
→ outgoing status/snapshot message
```

Do not mutate simulation state from network callbacks directly.

## Troubleshooting

- `ECONNREFUSED 127.0.0.1:19081`: bridge is not running; start `npm run bridge`.
- Bridge exits immediately: rebuild `universe_ui_bridge` in `build/container` and restart bridge.
- Protocol mismatch: ensure `protocolVersion: 1` on both C++ and TypeScript sides.
- No browser connection after bridge restart: frontend auto-reconnects; if local policy blocks WS, reload page.

## Future Unreal integration

This path is intentionally renderer-disposable. Unreal should consume the same engine-neutral read/control boundary (not PlayCanvas APIs).
