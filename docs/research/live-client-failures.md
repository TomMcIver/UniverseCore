# Live client failures: movement, collision, camera

Resolves #42 (map #41, Playable demo UI overhaul). This is a catalogue only; no code was changed.

## What was observed

- **Client and server code:** branch `worktree-settlement-tier-merge` @ `71d5f8f`. This is the newest Godot client and builds on `ui` @ `5be654c`. It adds the NPC daily schedule, canonical household offset, navmesh movement and real `world_delta`s. `ui` does not have these yet.
- **Bridge:** `universe_ui_bridge` rebuilt from that commit in the `universecore-dev` Podman image, served by `apps/web-client/server/ws-bridge.mjs`. Client: Godot 4.7.2, Forward+, 1280x720.
- **Scenario:** default config, seed 1, 200 people.
  - Fresh world: year 0, paused.
  - Advanced world: 64x speed until year 4. The population reached 206, and 237 people had ever existed.
- **Close-up and eye-level shots:** taken with a throwaway capture scene that moves the existing camera. The scene was not committed.

Screenshots are in [`live-client-failures/`](live-client-failures/).

| Shot | What it shows |
|---|---|
| `01-fresh-year0-overview.png` | Year 0 at the default camera: no buildings, flat terrain, empty event log |
| `02-year4-overview-inspector.png` | Year 4, with a building at 0% construction shown in the inspector |
| `03-year4-closeup.png` | Oblique close-up: crowd packed against a workshop |
| `04-year4-eye-level-crowd.png` | Eye level: villagers inside the waterwheel, all facing one way |
| `05-year4-eye-level-terrain-edge.png` | Eye level: a villager inside a market stall, and the world ending in a brown void |
| `06-year4-terrain-patch.png` | Zoomed out: the terrain is a finite floating square |

## Important context for ADR 0001

On this branch, person positions do not have a single owner. Three things combine:

1. **Server target.** The server sends each person's target as `Settlement.position + Household.settlement_offset` (`src/ui/web_protocol.cpp:290-323`). The household offset is canonical, a seeded integer in ±10 units set at founding (`src/simulation.cpp:111-121`).
2. **Client spread.** The client adds its own per-person spacing offset (`world_view.gd:298-308`).
3. **Client walk.** On a switch to `Travel`, the client teleports the villager to `target + jitter` and walks them back along a navmesh (`world_view.gd:235-238`).

So the villager movement design has to either absorb the client-side spread and walk into the C++ presentation layer, or explicitly redefine where the one owner sits. Buildings still have no canonical placement.

## Defects by owner

### A. Person positions and collision (C++ presentation layer, plus the client spread and walk above) — 8 defects

| # | Defect | Repro | Root cause |
|---|---|---|---|
| A1 | **Villagers stand inside buildings, stalls and waterwheels** (shots 03-05) | Advance to year 4, then look at any settlement at eye level | Household offsets (±10 units) and the building spiral (`web_protocol.cpp:339-354`, radius 4 + 3.2√i) are computed independently. Nothing reserves building footprints. |
| A2 | **Villagers packed shoulder to shoulder, overlapping each other** (shots 03, 04) | Same as A1 | Household offsets are a 21x21 integer grid with no minimum distance (`simulation.cpp:104-121`), so households collide. The client's per-person spread (1.5-3.5 units) is smaller than the crowd. |
| A3 | **Dead people are rendered as live villagers** after connecting or reconnecting | Advance to year 4, then connect a fresh client. It shows 237 villager nodes against a population of 206. | The snapshot includes dead people (`alive:false`, `web_protocol.cpp:315,567`). `WorldView.apply_snapshot` spawns every agent with no alive filter (`world_view.gd:73-81`). Only the delta path removes the dead (`web_protocol.cpp:418-423`). |
| A4 | **The "walk" teleports first**: when a villager switches to Travel, they jump to `target + jitter`, then walk home | Resume at 1x and watch a label change to `Travel` | This was proof-of-wiring logic (`world_view.gd:235-238`). Home and work are the same point, so there is no real destination. |
| A5 | **Walks go through buildings and ignore terrain height** | Watch a Travel walk near a building | The region navmesh is one flat quad at the height sampled at the bounding-box centre, with no obstacles carved (`main.gd:120-160`). Position is interpolated in 3D without re-grounding (`world_view.gd:404-419`). |
| A6 | **Avoidance for villagers near the camera does nothing** | Code inspection | A `NavigationAgent3D` with avoidance is created, but its velocity is never used; the node is moved directly (`world_view.gd:372-402`). "Near player" means near the camera, because there is no avatar (`world_view.gd:310-314`). |
| A7 | **Walk speed is tied to frame time, not simulation time** | Set 64x and watch a Travel walk | Walk speed is `3 units/s × playback speed` (`world_view.gd:356-358`). At 1x, one walk of a few units spans weeks of simulated days. At 64x it moves 192 units/s, which reads as teleporting. |
| A8 | **All villagers face the same way and never animate** (shot 04) | Look at any crowd at eye level | `_spawn_agent` never sets rotation or plays `character-a.glb`'s idle/walk animations (`world_view.gd:193-218`). |

### B. Building placement — 4 defects

| # | Defect | Repro | Root cause |
|---|---|---|---|
| B1 | **Year 0 has no buildings at all**, only fountain markers (shot 01) | Connect to a fresh world | No buildings exist at founding in the simulation. The demo's opening shot is an empty field. |
| B2 | **Construction state is not shown**: a workshop at 0% renders fully built (shot 02) | Advance, then click a building marked `ActiveConstruction` | `BuildingAssets.make_building(type)` takes only the type; `state` and `progress` are ignored. |
| B3 | **Every workshop is a watermill with its wheel on dry grass, and the wheel clips through the roof** (shots 02-04) | Look at any workshop | `building_assets.gd:18-40` bolts the `watermill.glb` wheel onto every Workshop, regardless of water. |
| B4 | **Placement is presentation-only and depends on order** | Code inspection | The spiral slot is the building's index within its settlement in list order (`web_protocol.cpp:349-350`). Any removal, reorder or settlement merge shifts every later building. There is no canonical placement (ADR 0001). |

### C. Terrain — 4 defects

| # | Defect | Repro | Root cause |
|---|---|---|---|
| C1 | **The world ends in a void**: the terrain is a finite 300x300-unit square over a brown sky floor (shots 05, 06) | Zoom out fully, or stand at eye level near a settlement edge | `terrain_generator.gd` builds a single 150x150-cell patch (cell 2.0) centred on the first active settlement (`:35-37`, `:82-99`). The procedural sky's ground colour shows below the horizon (`main.gd:203-207`). |
| C2 | **Terrain is generated once and never follows the world** | Found a settlement in another region later in a run | It is generated only from `initial_snapshot` (`main.gd:270`). Settlements founded later outside the patch float over the void, because the height sampler clamps to the edge. |
| C3 | **Terrain is flat and featureless** (sampled relief under 2 units within 60 units of the village; shots 01, 06) | Any view | The noise is at a fixed seed of 1337, not the world seed, with a small `height_scale`. Environment class only tints the colour. There are no paths, vegetation, water or resources, so a workshop's waterwheel has no river. |
| C4 | **The terrain has no collision** | Code inspection | It is a visual `MeshInstance3D` only. Nothing can stand on it physically, which blocks the player avatar (map notes: the avatar needs real terrain collision). |

### D. Camera feel (Godot) — 5 defects

| # | Defect | Repro | Root cause |
|---|---|---|---|
| D1 | **Rotating spins the camera around itself, not the village** | Press Z/C or middle-drag | `_rotate` changes the camera's own yaw (`orbit_camera.gd:111-113`). There is no orbit pivot, despite the file name. |
| D2 | **WASD and right-drag pan ignore yaw** | Rotate 90°, then press W | Pan uses fixed world X/Z (`orbit_camera.gd:55-66`, drag at `:82-100`). After rotating, "up" moves sideways on screen. |
| D3 | **Zoom slides the view off target** | Scroll while pitched | Zoom changes only `position.y` (`orbit_camera.gd:107-109`), so the ground point under the screen centre moves. `min_height` is an absolute 8, not relative to the terrain. |
| D4 | **No bounds**: you can pan into the void | Hold WASD | No clamp to the terrain or world extent. |
| D5 | **Centring happens once, on the first settlement only** | Let settlement 1 go extinct and reconnect; or try to find another region | `_center_camera_on_settlements` runs once (`main.gd:169`). There is no "go to settlement" or focus-on-selection. |

### E. Rendering, HUD and readability — 6 defects

| # | Defect | Repro | Root cause |
|---|---|---|---|
| E1 | **Every building and settlement marker is destroyed and rebuilt 10 times a second** while running | Resume at any speed | The server lists every present building and settlement as "updated" in every delta (`web_protocol.cpp:458,480`). The client handles updates as free-then-spawn (`world_view.gd:174-191`). Cost grows with building count. |
| E2 | **Activity labels show through walls and are illegible at overview distance** (shots 03-05) | Any view with villagers | A `Label3D` with `no_depth_test = true` on every agent (`world_view.gd:201-207`). |
| E3 | **The event log shows raw IDs** ("Birth -- entity 237", "SettlementFounded" with no detail), and the box is empty and untitled at start | Advance a few days | `event_log.gd:26` formats only the type and ID. |
| E4 | **The HUD shows the raw tick** (`Tick 154828800`), and the inspector shows raw fields (`Commons owned: true`, `Owner household: None`) | Any snapshot or click | This is a direct wire dump (`hud.gd`, `inspector_panel.gd`). There is no view model. |
| E5 | **Speed tops out at 8x in the HUD**, although the protocol allows 64; time advance is slow for a demo | Try to advance 20 years | HUD buttons `[1,2,4,8]` (`hud.gd:68`). Measured: 64x reached about 4 years in 3.5 s, which is ~65% of the nominal 640 days/s. |
| E6 | **The overview camera frames part of a settlement under the HUD panel** (shots 02, 03) | Default camera after advancing | Fixed panel positions over a free 3D view, with no safe-area framing. |

## Summary

| Owner | Count |
|---|---|
| A. Person positions and collision | 8 |
| B. Building placement | 4 |
| C. Terrain | 4 |
| D. Camera feel | 5 |
| E. Rendering, HUD, readability | 6 |
| **Total** | **27** |

## Notes for downstream tickets

- **Design canonical building placement** should cover B1, B2 and B4, and reserve footprints for A1.
- **Design activity-driven villager movement** should cover A1, A2 and A4-A8, and decide where the client-side spread and walk live (see the context section above).
- **Prototype the demo village look** should cover B3, C1, C3 and E2.
- **Prototype the command-view to ground-view descent** should cover C4 and D1-D5.
- **Define the typed UI binding pattern** should cover E3, E4 and E1 (real update diffs).
- A3 and E1 are cheap, standalone bug fixes that can go in any time.
