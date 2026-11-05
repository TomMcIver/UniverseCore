# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

UniverseCore: a deterministic, headless C++23 simulation of a Bronze Age settlement world (the foundation of a civilization-to-multiverse game), plus thin presentation clients. `AGENTS.md` holds the full engineering rules and non-negotiable principles. Read it, and `docs/GAME_VISION.md`, `docs/ARCHITECTURE.md` and `ROADMAP.md`, before architectural changes. `CONTEXT.md` is the domain glossary (canonical vs presentation state, coin, price, days of cover, merchant, and so on). Use its terms. `docs/adr/` records decisions.

## Build and test

The canonical build/test environment is a **Podman container** (root `Containerfile`, image `universecore-dev`). Do not assume host compilers. The repo is bind-mounted at `/workspace` and builds into `build/container` (gitignored). Milestones count as complete only after the canonical Podman run passes.

```bash
podman build -t universecore-dev -f Containerfile .
# Fast iteration: incremental build + "fast" ctest label
podman run --rm -v "${PWD}:/workspace" -w /workspace universecore-dev ./scripts/container-test-fast.sh
# Integration label only
podman run --rm -v "${PWD}:/workspace" -w /workspace universecore-dev ./scripts/container-test-integration.sh
# Full gate (all labels, including long regression + LOD benchmark)
podman run --rm -v "${PWD}:/workspace" -w /workspace universecore-dev ./scripts/container-test.sh
# Single test binary or test name, after building
podman run --rm -v "${PWD}:/workspace" -w /workspace universecore-dev bash -lc "./scripts/container-build.sh && ctest --test-dir build/container -R universe_merchant_tests --output-on-failure"
# Run the sim
podman run --rm -v "${PWD}:/workspace" -w /workspace universecore-dev ./scripts/container-run.sh --seed 42 --years 100 --population 200
```

- On Windows, `scripts/container-test-volume.ps1` and `.sh` keep the build dir in a named Podman volume. `UNIVERSECORE_TEST_SUITE` selects `all|fast|integration|long`.
- Tests are plain executables (no framework), one per file in `tests/`. Each is registered in `CMakeLists.txt` with a ctest label of `fast`, `integration` or `long`. **A new test file needs a manual `add_executable`/`add_test`/`set_tests_properties` block there.**
- The SDL3 + ImGui inspector (`universe_ui`) is off by default (`-DUNIVERSECORE_BUILD_UI=ON`). See the `container-ui-*.sh` scripts.
- `.github/workflows/ci.yml` builds natively on Linux, macOS and Windows with plain CMake. It does not use Podman.
- Generated `*.ucore` snapshots are runtime outputs and must not be committed.

Web client (`apps/web-client`): `npm run bridge` (WebSocket bridge), `npm run dev`, `npm test` (vitest), `npm run build`. See `docs/PLAYCANVAS_WEB_CLIENT.md`.

Godot client (`apps/godot-client`, Godot 4.7). Import assets first with `--headless --editor --import --path .`, then run the tests with `Godot_v4.7.2-stable_win64_console.exe --headless --path . --script res://scripts/tests/run_tests.gd`. The demo flow is in `docs/demo/README.md`. Run it with `scripts/run-demo-bridge.ps1`, which starts the bridge on `scenarios/demo.json`.

## Architecture

Three layers. Authority only flows downward.

1. **`universe_core` library** (`src/simulation.cpp` ≈ 9k lines, `snapshot.cpp` ≈ 4.7k, plus `resources`, `trade_pricing`, `trade_merchants`, `trade_trip` and `simulation_trade.cpp` (trade trips), `demo_scenario`; headers in `include/universe/`). This holds all canonical state and logic. It has no engine, wall-clock or UI dependency. Executables: `universe_sim` (headless CLI, `main.cpp`), `universe_lod_benchmark`, `universe_demo_seed_search`, `universe_trade_calibration` (trade sweep, see ADR 0003).
2. **`universe_ui_support`** (`src/ui/`, `include/universe/ui/`). `SimulationController` is the command/query adapter (advance/reset/save/load commands in, read-only view models out). Also here are `web_protocol` DTOs, `life_history`, and `presentation_movement`, which derives where people appear from canonical activity. This layer is non-canonical.
3. **Clients**. `universe_ui_bridge` speaks a line-delimited protocol on stdio. `apps/web-client/server/ws-bridge.mjs` wraps it as a localhost WebSocket. Both the PlayCanvas/TypeScript client and the Godot client (`apps/godot-client`, the current main client) consume that WebSocket. Clients are presentation and input only. Browser or frame timing never advances the canonical sim.

Cross-cutting rules that shape most changes (details in `AGENTS.md`):

- **Determinism.** No `std::uniform_*_distribution` and no shared mutable RNG. Randomness derives from parent seed + domain + stable ID/period. Time uses fixed-width integer types. Equivalent advancement schedules must give identical state.
- **Canonical vs presentation state.** Anything the sim must reproduce is canonical and goes into the versioned snapshot. Reconstructible or display-only data (for example person positions) is presentation state and is never serialized. Building placement is canonical (ADR 0001).
- **Snapshot schema versioning.** Any canonical state change needs a schema version bump and save/load continuation tests. The snapshot schema is currently v29; check `snapshot.cpp` for the live number. Serialization order must be explicit, and raw memory must never be serialized.
- **State changes flow through systems and typed events** appended to the canonical event journal.
- **Finite-resource conservation.** Households belong to exactly one settlement. Settlement access conserves regional capacity. Coin is created once and only moves (ADR 0003).
- **Seed-sensitive scenarios.** Founding, fission and merchant behaviour are sensitive to the seed. The committed `scenarios/demo.json` (seed 39) and `tests/demo_scenario_tests.cpp` guard the demo world. Use `universe_demo_seed_search` when a change shifts outcomes. Tests that depend on organic founding have been fragile, so prefer the deterministic demo scenario.

## Project workflow conventions

- Issues live on GitHub (`TomMcIver/UniverseCore`) and are managed with `gh`. See `docs/agents/issue-tracker.md`, `triage-labels.md` and `domain.md`. Feature branches are named `feat/...` and merged by PR.
- `dashboard/data/project-status.json` is the status source of truth. Update it in verified milestone or task commits, and only mark work done after the canonical Podman run passed.
- Verify the Git identity and remote before pushing (Personal repo). Never force-push.
- New major subsystems need tests and docs. Update `README.md`, `ROADMAP.md` and `docs/ARCHITECTURE.md` status sections where relevant.
