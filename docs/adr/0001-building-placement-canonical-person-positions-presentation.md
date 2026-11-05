# Building placement is canonical; person positions are presentation state

A completed building must stay where it is across save/load and time advance, so building placement becomes canonical state, like settlement positions in snapshot schema v20. A person's exact position moment to moment has no causal effect on the simulation, so it stays presentation state, derived in the C++ presentation layer from canonical activity and assigned buildings. That layer is the single owner of person positions. The Godot client renders them and does not correct them with client-side physics; client collision applies only to the player avatar.

## Considered Options

- Both presentation-only: rejected because building layout would not be stable across save/load and could not honestly show settlement change.
- Both canonical: rejected because it serializes reconstructible state every tick, violating seed-plus-deltas.
