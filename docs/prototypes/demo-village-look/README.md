# Demo village look — live client before/after (ticket #55)

All eight shots are from the live Godot 4.7.2 client, connected to the same `universe_ui_bridge` world (default config, seed 1), paused at year 6 with population 202. Each before/after pair uses an identical camera, set with `UNIVERSECORE_CAPTURE_VIEW=command|oblique|ground|overview`.

| View | Before | After |
|---|---|---|
| Command (default camera) | `before-command.png` | `after-command.png` |
| Oblique | `before-oblique.png` | `after-oblique.png` |
| Ground level (eye height) | `before-ground.png` | `after-ground.png` |
| Overview | `before-overview.png` | `after-overview.png` |

At year 6 every building in this world is still `ActiveConstruction`, so the after shots show roofless walls at their real construction progress, not finished houses.

## What is backed by simulation data

- **Fields:** each household with living `SubsistenceCultivator` members gets one fenced plot on the settlement edge, and the plot grows with the household's cultivator count. Placement is deterministic from settlement and household IDs. Plots appear, grow, shrink and disappear as households change.
- **Forest ring:** tree density scales with the nearest region's `woodStock / woodCapacity` and environment class. Trees the current stock can't support render as stumps.
- **Wildflowers:** density follows the region's wild-food richness.
- **Ground colour:** follows environment class and wild-food richness.
- **Buildings:** type and construction state/progress. A planned building is a staked plot, one under construction is roofless walls at a height that follows `progress`, and a complete one gets its roof.

## What is presentation-only

These have no simulation counterpart and are placeholders until canonical placement exists (#53):
- terrain height;
- tracks, which join a region's settlements along a minimum spanning tree;
- the path from each building's door to its common;
- which way each building faces.

## Fixed catalogue defects

- **B2:** construction state shown.
- **B3:** no watermills.
- **C1–C4:** streamed world-space terrain tiles, a ground skirt with no void, real relief, and HeightMapShape3D collision.
- **E1:** buildings and settlements are no longer rebuilt on every delta.
- **Black grass:** foliage normals now face up.
