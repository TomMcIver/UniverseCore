# UniverseCore

Headless simulation core for a procedural, physically grounded civilization-to-multiverse game.

The project deliberately starts without Unreal Engine. The initial objective is to prove deterministic world generation, population dynamics, persistent history, simulation level-of-detail, and AI-agent integration independently of rendering.

## Current milestone

**Prototype 0: deterministic demographic smoke test**

- C++23
- Cross-platform CMake build
- Stable custom PRNG
- Seeded simulation
- Population births/deaths
- Reproducibility tests
- No engine dependency

## Build

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
