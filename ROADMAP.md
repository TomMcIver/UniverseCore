# Roadmap

## Milestone 0: Repository and deterministic kernel

Status: **complete**

- [x] C++23/CMake project
- [x] headless executable
- [x] deterministic PRNG
- [x] seed/config handling
- [x] basic demographic smoke test
- [x] deterministic unit test
- [x] hierarchical seed derivation
- [x] canonical simulation clock
- [x] event types and event log
- [x] snapshot serialization

Exit criteria: same seed/config produces byte-identical canonical snapshot across supported platforms.

## Milestone 1: 200-person settlement

Status: **in progress**

- [x] person IDs
- [x] age/sex
- [x] households
- [x] kinship
- [x] fertility by age/context
- [x] mortality by age/context
- [x] food production/consumption
- [x] health/nutrition
- [x] basic simulation UI (optional SDL3 + Dear ImGui inspector with smoke test)
- [x] deterministic land/environment resources
- [x] deterministic settlements and local carrying capacity
- [x] occupations
- [x] resource inventory
- [ ] migration
- [x] settlement population statistics

Exit criteria: 100-year runs remain internally valid and produce explainable demographic histories.

## Milestone 2: Persistent history and knowledge

- [ ] event journal
- [ ] agent memory
- [ ] knowledge graph/state
- [ ] communication/teaching
- [ ] records
- [ ] knowledge loss
- [ ] basic discoveries
- [ ] institutions

Exit criteria: a discovery can originate with one person, spread through part of a population, survive or disappear, and be traced historically.

## Milestone 3: Economy and settlement growth

- [ ] work allocation
- [ ] production chains
- [ ] ownership/access
- [ ] exchange
- [ ] construction
- [ ] land/resource pressure
- [ ] inequality/household wealth

Exit criteria: settlement growth is constrained by real resources and labour rather than scripted population thresholds.

## Milestone 4: Simulation LOD

- [ ] cohort representation
- [ ] individual ↔ cohort promotion/demotion
- [ ] household aggregation
- [ ] causality preservation tests
- [ ] time acceleration policies

Exit criteria: simulate a much larger off-screen population cheaply, resolve a region to individuals, and preserve statistical/history consistency.

## Milestone 5: First AI cognition

- [ ] structured cognitive state
- [ ] planner interface
- [ ] inference adapter
- [ ] schema validation
- [ ] knowledge-constrained prompts/context
- [ ] deterministic fallback agent

Exit criteria: one significant NPC can make a model-assisted decision without accessing hidden world truth or directly mutating canonical state.

## Milestone 6: Unreal bridge

Only begin after Milestones 0-4 are stable enough to demonstrate a believable headless settlement.

- [ ] Unreal C++ project
- [ ] UniverseCore library adapter
- [ ] simple generated terrain
- [ ] visual settlement
- [ ] local NPC realization
- [ ] player controller
- [ ] save/load bridge

Exit criteria: player can enter a settlement whose visible state is generated from UniverseCore, leave for twenty simulated years, return, and see the resulting coherent world.

## Later

- ecology
- procedural geology/climate
- procedural biology
- cities/nations
- advanced technology discovery
- local granular physics
- planets
- orbital mechanics
- interstellar travel
- cloud simulation
- optional multiplayer
- alternate universes
