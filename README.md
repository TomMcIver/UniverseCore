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
- Timestamped canonical append-only event journal
- Typed event payloads for `Birth`, `Death`, `SettlementFounded`, and `SettlementExtinct`
- Canonical deterministic agent memory foundation (person-specific direct observation with event provenance)
- Canonical deterministic agent knowledge state foundation (typed per-person structured facts derived from observed memories with provenance)
- Canonical deterministic communication/teaching transmission (local same-settlement propagation with provenance-preserving memory chains)
- Versioned deterministic canonical snapshots
- Deterministic household food production/storage/consumption
- Deterministic regional environment resources (finite productive capacity + renewable wild stock)
- Deterministic settlement layer between regions and households
- Deterministic local carrying-capacity pressure and settlement-level resource access
- Deterministic person occupations with settlement-driven labour specialization
- Deterministic person-level nutrition/health affecting demographic outcomes
- Deterministic material opportunity/extraction/inventory/crafting chain
- Canonical material outputs: Timber and BasicTool
- Deterministic tool durability loop: wear accumulation, breakage, replacement pressure
- Deterministic tool coverage and bounded productivity effects on food/material output
- Material-environment abstractions: renewable wood stock + finite stone stock
- Optional SDL3 + Dear ImGui simulation inspector (`UNIVERSECORE_BUILD_UI=ON`)
- UI command/query adapter over canonical simulation state
- UI smoke test validating deterministic advance/save/load behavior
- Snapshot schema v12 with deterministic typed-event/material/environment/inventory/tool-durability/agent-memory/agent-knowledge/knowledge-transmission persistence/validation
- Reproducibility tests
- No engine dependency

Current M2A/M2B modelling limitations (intentional):

- aggregate material quantities rather than item-instance objects
- settlement-level inventories only
- no private ownership or household wealth mechanics
- no trade or market layer
- no building/construction system
- no technology gating for these material chains
- generic `BasicTool` abstraction (non-specialized tooling taxonomy)

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

Run fast developer feedback tests (incremental build + fast suite only):

```bash
podman run --rm \
	-v "${PWD}:/workspace" \
	-w /workspace \
	universecore-dev \
	./scripts/container-test-fast.sh
```

Run integration suite only:

```bash
podman run --rm \
	-v "${PWD}:/workspace" \
	-w /workspace \
	universecore-dev \
	./scripts/container-test-integration.sh
```

Run long regression suite only:

```bash
podman run --rm \
	-v "${PWD}:/workspace" \
	-w /workspace \
	universecore-dev \
	bash -lc "./scripts/container-build.sh && ctest --test-dir build/container -L long --output-on-failure"
```

`container-test-fast.sh` is for development iteration only.
`container-test.sh` remains the complete verification command for milestone/ship gates.

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

Run the deterministic long-horizon viability matrix benchmark (noncanonical analysis tooling):

```powershell
./scripts/run-viability-matrix.ps1 -SeedStart 1 -SeedEnd 50 -Years 1000 -Population 200 -FoodScale 100 -EffortScale 100 -OutputDir build/viability/baseline_p200_s1_50
```

This writes:

- `per_seed_per_year.csv`
- `per_seed_summary.csv`
- `horizon_summary.csv`
- `summary.txt`

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

To keep generated files on a Podman-managed Linux volume instead of the Windows bind mount:

```powershell
./scripts/container-test-volume.ps1 -Suite all
./scripts/container-test-volume.ps1 -Suite fast
./scripts/container-test-volume.ps1 -Suite integration
./scripts/container-test-volume.ps1 -Suite long
```

Equivalent shell wrapper:

```bash
./scripts/container-test-volume.sh
UNIVERSECORE_TEST_SUITE=fast ./scripts/container-test-volume.sh
UNIVERSECORE_TEST_SUITE=long ./scripts/container-test-volume.sh
```

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

Containerized UI incremental build + smoke test:

```bash
./scripts/container-ui-test.sh
```

Volume-backed UI build + smoke test (recommended on Windows bind mounts):

```powershell
./scripts/container-ui-test-volume.ps1
./scripts/container-ui-test-volume.ps1 -Clean
```

Shell equivalent:

```bash
./scripts/container-ui-test-volume.sh
UNIVERSECORE_UI_CLEAN=1 ./scripts/container-ui-test-volume.sh
```

Containerized clean UI verification (forces clean dependency/build state):

```bash
./scripts/container-ui-verify-clean.sh
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

### Windows native UI launch (no host compiler required)

If native `cmake`/MSVC/MinGW are unavailable on Windows, build a Windows `universe_ui.exe`
through Podman + MinGW-w64 cross-compilation:

```powershell
./scripts/build-windows-ui.ps1
```

This writes host-runnable artifacts to:

```text
build/windows-ui/UniverseCore/universe_ui.exe
build/windows-ui/UniverseCore/SDL3.dll
```

Launch the interactive inspector:

```powershell
./scripts/run-windows-ui.ps1
```

`build-windows-ui.ps1` uses a Podman-managed Linux build volume (`universecore-winui-build`)
for generated files and dependency extraction reliability, while keeping source in the
Windows checkout.

## Read first

Coding agents must read:

1. `AGENTS.md`
2. `docs/GAME_VISION.md`
3. `docs/ARCHITECTURE.md`
4. `ROADMAP.md`

Do not add Unreal Engine until the headless simulation milestones in `ROADMAP.md` are proven.
