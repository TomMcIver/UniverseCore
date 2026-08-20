# AGENTS.md

## Purpose

This repository is the simulation foundation for a game in which the player begins in a Bronze/Iron Age civilization and can progress, potentially over geological and astronomical timescales, through industrialization, computing, AI, spaceflight, interstellar civilization, exotic but theoretically motivated physics, universe-scale engineering, and eventually traversal into other universes.

The player is effectively persistent across history but not invulnerable. Early death causes rebirth into the continuing world. Later civilizations can discover technological continuity mechanisms such as cloning, synthetic bodies, backups, or other physically justified systems.

The project's defining technical idea is **hierarchical causal simulation**: the world should behave as though it exists at arbitrarily high detail, while only resolving the detail necessary for current causal interactions.

## Mandatory reading order

Before making architectural changes, read:

1. `docs/GAME_VISION.md`
2. `docs/ARCHITECTURE.md`
3. `ROADMAP.md`

## Non-negotiable principles

### 1. Simulation first, renderer second

The core simulation must not depend on Unreal Engine. It must be runnable headlessly from the command line. Unreal will later consume simulation state through a dedicated adapter.

### 2. Determinism

Given the same version, seed, configuration, and ordered event stream, the simulation should reproduce the same world state.

Avoid platform-dependent randomness. Do not use `std::uniform_*_distribution` for world-critical generation unless cross-platform reproducibility has been proven. Prefer explicit integer algorithms and versioned generators.

### 3. Seed + deltas, not stored infinity

Unobserved reality is generated deterministically from hierarchical seeds. Persistent storage records only information that cannot be reconstructed from the seed, especially player/environment modifications, historically consequential events, persistent agents, discovered knowledge, and generated details that have become canon.

### 4. Simulation level of detail

Everything must be capable of representation at multiple resolutions where practical:

- Statistical
- Aggregate
- Entity
- Detailed
- Physical

Examples:

- Distant population: cohorts and distributions.
- Nearby population: households and individual agents.
- Player conversation: high-resolution cognition.
- Beach at distance: aggregate granular field.
- Sand in the player's hand: individual grains.

Resolution promotion must preserve consistency with the lower-resolution state. Demotion must preserve consequential deltas.

### 5. No fake global knowledge

NPCs may only reason from information available to them through perception, memory, communication, education, records, experimentation, and inference. Engine truth is not character knowledge.

Technology must be represented as knowledge + capabilities + physical prerequisites, not simply a linear research-point tree.

### 6. LLMs are not NPC state

An NPC's durable identity is structured simulation state. Expensive models are reasoning accelerators used only when required. Model output must be converted into validated structured state transitions before affecting canonical simulation state.

### 7. No arbitrary scripted history

The engine may define physical, biological, economic, and institutional rules. It should not force predetermined inventions, wars, empires, religions, or demographic outcomes simply because a historical date has been reached.

### 8. Physically grounded, not atomically simulated

Do not attempt to simulate every atom. Resolve matter to the level necessary for observable causal behaviour. Microscopic or atomic properties may be represented by validated aggregate models until a mechanic requires finer resolution.

### 9. Time acceleration is not free player power

Simulation acceleration represents abstraction or relinquished agency, not a magical ability to manipulate external time while acting normally. Important unresolved local interactions constrain how aggressively the simulation can be coarse-grained.

### 10. Measure before optimizing

Do not prematurely introduce distributed systems, GPU kernels, databases, ECS frameworks, or cloud dependencies. Prototype the semantic model first, benchmark it, then replace bottlenecks.

## Current engineering rules

- Language: C++23.
- Build: CMake.
- Canonical dev/test environment: Podman container via root `Containerfile` + `scripts/container-*.sh`.
- Runtime dependencies: none until justified.
- Prefer value types and explicit ownership.
- No global mutable state.
- Canonical simulation behaviour must not depend on mutable sequential RNG consumption order.
- Derive scoped deterministic randomness from parent seed + explicit domain + stable identifier/time.
- Represent canonical elapsed time with fixed-width integer simulation time types.
- Canonical simulation logic must not depend on wall-clock time or frame time.
- Canonical state changes require evaluating snapshot schema impact and versioning.
- Never serialize raw C++ object memory, pointers, padding, or ABI-dependent layouts.
- Canonical serialized collection order must be explicit and deterministic.
- Hidden mutable state that affects future outcomes must be either serialized or deterministically reconstructible.
- Canonical resource quantities must use fixed-width integer units with explicitly documented semantics.
- Resource-system randomness must be scoped by stable identity + resource domain + canonical period identifier.
- Simulation state changes should flow through explicit systems/events.
- Every world-critical algorithm needs deterministic tests.
- Every persisted schema will eventually require versioning and migrations.
- Keep the headless executable working at all times.
- New major subsystems require tests and documentation.

## Container workflow

- Do not assume host-installed compilers, CMake, Ninja, or platform SDKs.
- Build/test/run through Podman by bind-mounting the repository into `/workspace`.
- Use `build/container` as the default generated build directory.

## Immediate goal

Build a headless Bronze Age settlement simulator capable of starting from roughly 200 people and running for at least 100 simulated years with coherent:

- individuals
- age and sex distributions
- households/families
- births and deaths
- food production/consumption
- occupations
- migration
- resources
- settlement growth
- relationships
- knowledge transfer
- basic institutions
- event/history recording

The first real validation scenario is:

> Start a settlement, simulate twenty years without player intervention, then resolve the world back to individual detail and verify that the resulting people, families, buildings, resources, knowledge and recorded history are causally consistent.

## Explicitly out of scope for now

Do not implement yet:

- Unreal Engine integration
- photorealistic rendering
- planetary rendering
- orbital mechanics
- full chemistry
- genetics
- procedural alien biology
- LLM dialogue
- multiplayer
- cloud services
- wormholes
- multiverse traversal
- grain-level sand physics

These are future consumers of the same core architecture. Building them before the settlement simulation is credible would obscure foundational errors.
