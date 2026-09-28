# Demo village: reusable assets and Godot add-ons

Research for [#43](https://github.com/TomMcIver/UniverseCore/issues/43) (child of wayfinder map #41, "Playable demo UI overhaul").
Researched 2026-09-28. Everything is sourced from the asset's own page, repo, or the official Godot docs unless marked *(secondary)*.
This list is the starting point for the demo's asset source list.

## Pinned engine

- `apps/godot-client/project.godot` has `config/features=PackedStringArray("4.7", "Forward Plus")`, so the project targets **Godot 4.7, Forward+**.
- The test runner header (`scripts/tests/run_tests.gd`) runs `Godot_v4.7.2-stable_win64_console.exe`. No Containerfile or script downloads Godot, so the engine is a host binary outside the Podman workflow. That is a risk (see below).
- The docs at docs.godotengine.org/en/stable are currently the 4.7 docs.

## What `apps/godot-client/assets` already contains

(on the local `ui` branch at `5be654c`, which was not yet pushed to `origin/ui` when this was written; recorded in `apps/godot-client/assets/CREDITS.md`)

| Path | Source | Licence |
|---|---|---|
| `buildings/` `wall-wood`, `wall-wood-door`, `roof-gable`, `roof-gable-end` (unused), `watermill`, `stall-red`, `fountain-round` `.glb` + shared `Textures/colormap.png` | [Kenney Fantasy Town Kit 2.0](https://kenney.nl/assets/fantasy-town-kit) | CC0 |
| `characters/character-a.glb`, `character-b.glb` + `Textures/texture-a/b.png` | [Kenney Blocky Characters 2.0](https://kenney.nl/assets/blocky-characters) | CC0 |

Verified by reading the glTF JSON chunks:
- `character-a.glb` has 27 embedded animations, including `idle`, `walk`, `sprint`, `sit`, `pick-up`, `interact-left/right`, `emote-yes/no` and `die`. That is enough for a placeholder player avatar and for walking NPCs.
- `wall-wood.glb` has a single mesh node and **no collision nodes**. As assumed, the Kenney meshes ship without collision.
- The Kenney naming convention is kebab-case filenames with one shared `colormap.png` atlas per kit.
- The terrain is not an asset. `scripts/terrain_generator.gd` builds a vertex-coloured `SurfaceTool` mesh from sim region data and has **no collision shape**.

## Recommended shortlist

| # | Candidate | Use | Licence | Godot 4.7 | Collision |
|---|---|---|---|---|---|
| 1 | [Kenney Fantasy Town Kit 2.0](https://kenney.nl/assets/fantasy-town-kit) (160 models, single texture map) | Base visual language: walls, roofs, stalls, mills, fences, hedges, lanterns, 5 road pieces *(contents per [Kenney on X](https://x.com/KenneyNL/status/1951978055502557270) / search summary; confirm in the download)* | CC0 | Yes (plain glTF) | **None.** Generate it on import |
| 2 | [Kenney Blocky Characters 2.0](https://kenney.nl/assets/blocky-characters) (20 characters, animated) | Player avatar and villagers. Already in the repo and has idle/walk/sprint | CC0 | Yes | None. Use a `CapsuleShape3D` on the `CharacterBody3D` |
| 3 | [Kenney Nature Kit](https://kenney.nl/assets/nature-kit) (330 files, v1.0, 2020) | Trees, rocks, bushes, cliffs. Same flat-colour low-poly language | CC0 | Yes | None |
| 4 | [Kenney Survival Kit 2.0](https://kenney.nl/assets/survival-kit) (80 models, tagged Survival/Nature) | Campfires, tents, tools, crates: Bronze Age dressing | CC0 | Yes | None |
| 5 | **Built-in Godot import physics** ([Advanced Import Settings](https://docs.godotengine.org/en/stable/tutorials/assets_pipeline/importing_3d_scenes/advanced_import_settings.html)): *Generate > Physics*, Body Type **Static**, Shape Type **Trimesh** for buildings (or Box for props) | Building/prop collision without an add-on | Engine (MIT) | Native | Generates `StaticBody3D` + shapes |
| 6 | **Built-in [`HeightMapShape3D`](https://docs.godotengine.org/en/stable/classes/class_heightmapshape3d.html)** fed from `terrain_generator.gd`'s height samples | Terrain collision for the ground view. Keeps terrain sim-driven and deterministic | Engine (MIT) | Native | Yes. The docs say it is faster than `ConcavePolygonShape3D` |
| 7 | [Third Person Controller](https://godotengine.org/asset-library/asset/3934) by Jeh3no, v2.2, [repo](https://github.com/Jeh3no/Godot-Third-Person-Controller), updated 2026-06-21 | Reference/starting point for the ground-view controller (FSM: idle/walk/run/jump/in-air, free + shoulder camera, camera collision toggle). Swap its plush model for Blocky Characters | MIT (the model comes from Gtibo's Godot-Plush-Character, also MIT) | **"4.4 – 4.7: Fully supported"** (README) | Character capsule only |
| 8 | [Poly Haven](https://polyhaven.com/license) **HDRIs only**, e.g. [Kloofendal 48d Partly Cloudy (Pure Sky)](https://polyhaven.com/a/kloofendal_48d_partly_cloudy_puresky) | Sky / ambient / reflection source for `WorldEnvironment`. A pure sky with no photographic ground won't clash with flat-shaded Kenney geometry | CC0, no attribution required | Yes (EXR/HDR `Sky` panorama) | n/a |

Default for vegetation scatter: a small deterministic GDScript scatter that places Kenney Nature Kit meshes into `MultiMeshInstance3D`, seeded from sim IDs to match the determinism rule in AGENTS.md. Only adopt [ProtonScatter](https://godotengine.org/asset-library/asset/1866) (MIT, v4.1, updated 2026-01-02, [repo](https://github.com/HungryProton/scatter)) if hand-authored editor placement becomes necessary. It is an editor-time procedural tool.

## Rejected (for the demo)

- **Poly Haven textures and models.** Photoreal PBR, which clashes with Kenney's single-colormap flat look.
- **[Terrain3D](https://godotengine.org/asset-library/asset/3892)** (MIT, v1.0.2, 2026-05-19). High quality, and its collision modes are documented ([docs](https://terrain3d.readthedocs.io/en/stable/docs/collision.html)), but the listing says "4.4–4.6+" and its release notes say *"I don't know about 4.7"*. It is also an editable-heightmap GDExtension, while our terrain is generated from sim state. Overkill for one village.
- **M Terrain / HTerrain / other terrain plugins.** Same mismatch: they are authored-terrain tools. The built-in `HeightMapShape3D` is enough.
- **[Spatial Gardener](https://godotengine.org/asset-library/asset/2037)** (MIT, v1.4.1). Last updated 2025-03-05, listed for 4.4, and it is an editor paint tool rather than data-driven.
- **[Godot Road Generator](https://godotengine.org/asset-library/asset/3379)** (MIT, v0.9.0). Built for highway/lane networks, supports "4.3–4.6", and its terrain flattening needs Terrain3D. Use Fantasy Town Kit road tiles or a `Path3D` + `CSGPolygon3D` dirt strip instead.
- **[Sky3D](https://godotengine.org/asset-library/asset/5167)** (MIT, v2.1, 4.3+). A realistic look, and it runs its own time-of-day clock. That would compete with the sim-driven `scripts/world/day_night.gd`, and UI state must not become the source of truth. Revisit only if we can drive it externally.
- **Other third-person controllers** (Asset Library IDs 4324, 2268, 1894, 1815, 3408). Older (≤4.4 listings) or camera-only. The official [TPS Demo](https://godotengine.org/asset-library/asset/2710) is a shooter-sized reference, not a drop-in.
- **Kenney Mini Characters / Mini Forest.** "Mini" series proportions differ from Blocky Characters. Mixing them breaks consistency.
- **Kenney Graveyard Kit.** Halloween/horror tone. Maybe cherry-pick fences only.
- **Quaternius Medieval Village MegaKit** (CC0). Previously recommended in `free-medieval-assets-procedural-terrain.md`, but its style differs from Kenney's, and itch.io downloads failed from this environment (see CREDITS.md).

## Open risks

1. **No Godot binary in the canonical workflow.** 4.7.2 is a host install and is not in the Podman container. Headless client tests aren't reproducible in CI until it is pinned.
2. **Collision is entirely ours.** No shortlisted asset ships collision. Trimesh import on modular wall pieces gives many small bodies. Consider one `BoxShape3D` per assembled building footprint instead, and verify door gaps if interiors ever matter.
3. **The Fantasy Town Kit contents list is unverified** from the primary page (it gives only a count). Confirm the road/fence/tree pieces in the downloaded pack before designing around them.
4. **Mixing Kenney kits.** Nature Kit (2020) and Survival Kit 2.0 use their own colormaps and palettes. Check them side by side in `debug_building.tscn` before committing. A palette-remap pass may be needed.
5. **The add-on version gap.** Most Asset Library listings stop at 4.4–4.6. Only the Jeh3no controller explicitly claims 4.7. Treat any other add-on as untested on 4.7.
6. **Asset source of truth.** Keep adding every imported file to `assets/CREDITS.md` (source URL, licence, version), and do not commit whole packs.
