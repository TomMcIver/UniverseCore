# UniverseCore

Headless simulation core for a procedural, physically grounded civilization-to-multiverse game.

The project deliberately starts without Unreal Engine. The initial objective is to prove deterministic world generation, population dynamics, persistent history, simulation level-of-detail, and AI-agent integration independently of rendering.

## Current milestone

**Prototype 1 foundation: deterministic individual demographics**

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

To print recent canonical simulation events from the run:

```bash
podman run --rm \
	-v "${PWD}:/workspace" \
	-w /workspace \
	universecore-dev \
	./scripts/container-run.sh --seed 42 --years 20 --population 200 --dump-events 10
```

Container builds write artifacts to `build/container` in the mounted repository.

### macOS / Linux

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/universe_sim --seed 42 --years 100 --population 200
```

### Windows PowerShell

```powershell
cmake -S . -B build
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\Release\universe_sim.exe --seed 42 --years 100 --population 200
```

## Read first

Coding agents must read:

1. `AGENTS.md`
2. `docs/GAME_VISION.md`
3. `docs/ARCHITECTURE.md`
4. `ROADMAP.md`

Do not add Unreal Engine until the headless simulation milestones in `ROADMAP.md` are proven.
