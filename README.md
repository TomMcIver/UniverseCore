# UniverseCore

Headless simulation core for a procedural, physically grounded civilization-to-multiverse game.

The project deliberately starts without Unreal Engine. The initial objective is to prove deterministic world generation, population dynamics, persistent history, simulation level-of-detail, and AI-agent integration independently of rendering.

## Current milestone

**Prototype 1 foundation: deterministic settlement simulation + inspector UI**

- C++23
- Cross-platform CMake build
- Stable custom PRNG
- Seeded simulation
- Canonical fixed-width simulation clock
- Stable person IDs
- Individual birth year / age progression
- Biological sex for demographic simulation
- Alive/dead lifecycle
- Parent relationships for newborns
- Household membership
- Age-dependent mortality/fertility
- Scheduled yearly demography on canonical time boundaries
- Timestamped canonical birth/death event log
- Versioned deterministic canonical snapshots
- Deterministic household food production/storage/consumption
- Deterministic regional environment resources (finite productive capacity + renewable wild stock)
- Deterministic settlement layer between regions and households
- Deterministic local carrying-capacity pressure and settlement-level resource access
- Deterministic person occupations with settlement-driven labour specialization
- Deterministic person-level nutrition/health affecting demographic outcomes
- Optional SDL3 + Dear ImGui simulation inspector (`UNIVERSECORE_BUILD_UI=ON`)
- UI command/query adapter over canonical simulation state
- UI smoke test validating deterministic advance/save/load behavior
- Snapshot schema v6 with deterministic occupation + labour persistence/validation
- Reproducibility tests
- No engine dependency

## Build

## Canonical development environment (Podman)

UniverseCore's canonical build/test environment is a Podman container using the repository's root `Containerfile`.

Build the development image:

```bash
podman build -t universecore-dev -f Containerfile .
```

Run full build + tests (Windows/macOS):

```bash
podman run --rm \
	-v "${PWD}:/workspace" \
	-w /workspace \
	universecore-dev \
	./scripts/container-test.sh
```

Run full build + tests (SELinux-enabled Linux host):

```bash
podman run --rm \
	-v "${PWD}:/workspace:Z" \
	-w /workspace \
	universecore-dev \
	./scripts/container-test.sh
```

Run simulation with forwarded arguments:

```bash
podman run --rm \
	-v "${PWD}:/workspace" \
	-w /workspace \
	universecore-dev \
	./scripts/container-run.sh --seed 42 --years 100 --population 200
```

Run constrained-food or famine abstractions for debugging:

```bash
podman run --rm \
	-v "${PWD}:/workspace" \
	-w /workspace \
	universecore-dev \
	./scripts/container-run.sh --seed 42 --years 100 --population 200 --food-scale 90
```

```bash
podman run --rm \
	-v "${PWD}:/workspace" \
	-w /workspace \
	universecore-dev \
	./scripts/container-run.sh --seed 42 --years 100 --population 200 --food-scale 40
```

Run labour-constrained diagnostics by reducing effective labour capacity:

```bash
podman run --rm \
	-v "${PWD}:/workspace" \
	-w /workspace \
	universecore-dev \
	./scripts/container-run.sh --seed 42 --years 100 --population 200 --effort-scale 55
```

Run yearly calibration diagnostics with detailed derived metrics:

```bash
podman run --rm \
	-v "${PWD}:/workspace" \
	-w /workspace \
	universecore-dev \
	./scripts/container-run.sh --seed 42 --years 150 --population 200 --report-interval 1
```

To print recent canonical simulation events from the run:

```bash
podman run --rm \
	-v "${PWD}:/workspace" \
	-w /workspace \
	universecore-dev \
	./scripts/container-run.sh --seed 42 --years 20 --population 200 --dump-events 10
```

To save a canonical snapshot after simulation:

```bash
podman run --rm \
	-v "${PWD}:/workspace" \
	-w /workspace \
	universecore-dev \
	./scripts/container-run.sh --seed 42 --years 250 --population 200 --save build/settlement_250.ucore
```

To load a snapshot and continue simulation deterministically:

```bash
podman run --rm \
	-v "${PWD}:/workspace" \
	-w /workspace \
	universecore-dev \
	./scripts/container-run.sh --load build/settlement_250.ucore --years 250 --save build/settlement_500.ucore
```

Container builds write artifacts to `build/container` in the mounted repository.

### macOS / Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/universe_sim --seed 42 --years 100 --population 200
```

Optional UI build + smoke test:

```bash
cmake -S . -B build-ui -DCMAKE_BUILD_TYPE=Release -DUNIVERSECORE_BUILD_UI=ON
cmake --build build-ui -j
ctest --test-dir build-ui --output-on-failure
./build-ui/universe_ui --smoke-test
```

Run the interactive inspector:

```bash
./build-ui/universe_ui
```

### Windows PowerShell

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\universe_sim.exe --seed 42 --years 100 --population 200
```

Optional UI build + smoke test:

```powershell
cmake -S . -B build-ui -DUNIVERSECORE_BUILD_UI=ON
cmake --build build-ui --config Release
ctest --test-dir build-ui -C Release --output-on-failure
.\build-ui\Release\universe_ui.exe --smoke-test
```

Run the interactive inspector:

```powershell
.\build-ui\Release\universe_ui.exe
```

## Read first

Coding agents must read:

1. `AGENTS.md`
2. `docs/GAME_VISION.md`
3. `docs/ARCHITECTURE.md`
4. `ROADMAP.md`

Do not add Unreal Engine until the headless simulation milestones in `ROADMAP.md` are proven.
