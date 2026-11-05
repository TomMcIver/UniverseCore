# Godot 4.7 Runtime NavigationMesh Baking & Multi-Agent Pathing

Research for: procedural terrain (`apps/godot-client/scripts/terrain_generator.gd`) built at
runtime via `SurfaceTool` as an `ArrayMesh`, regenerated whenever the client receives a new
world snapshot/delta from the simulation server, needing navigation for potentially hundreds
of NPCs.

All claims below cite a primary source (official docs, engine source/issues, or real
community threads). Docs URLs are the 4.7 branch unless noted.

---

## 1. Runtime NavigationMesh baking API (not editor bake button)

Godot 4's navigation baking pipeline is split into "source geometry extraction" and
"baking," both callable purely from script at runtime:

- **`NavigationMeshSourceGeometryData3D`** — a plain data container for the triangle soup fed
  into the baker. Relevant methods (exact signatures per the 4.7 class reference):
  - `void add_mesh(mesh: Mesh, xform: Transform3D)` — pulls geometry straight out of a `Mesh`
    resource (this is the entry point for a procedural `ArrayMesh` built via `SurfaceTool`).
  - `void add_faces(faces: PackedVector3Array, xform: Transform3D)` — raw clockwise-wound
    triangle list, useful if you want to hand the baker your terrain's vertex buffer directly
    instead of wrapping it in a `Mesh`.
  - `void add_mesh_array(mesh_array: Array, xform: Transform3D)` — accepts the `Array` format
    returned by `ArrayMesh.surface_get_arrays()` / `SurfaceTool` output directly.
  - `void merge(other_geometry: NavigationMeshSourceGeometryData3D)` — combine geometry from
    multiple sources (e.g. terrain + static buildings) before one bake.
  - `AABB get_bounds()`, `bool has_data()`, `void clear()`.
  Source: [`class_navigationmeshsourcegeometrydata3d.html`](https://docs.godotengine.org/en/4.7/classes/class_navigationmeshsourcegeometrydata3d.html)

- **`NavigationServer3D.parse_source_geometry_data(navigation_mesh: NavigationMesh, source_geometry_data: NavigationMeshSourceGeometryData3D, root_node: Node, callback: Callable = Callable())`**
  — walks a `SceneTree` subtree and auto-populates a `NavigationMeshSourceGeometryData3D` from
  whatever geometry/collision nodes match the `NavigationMesh`'s parsed-geometry settings. Not
  strictly needed if you already have the `ArrayMesh` reference and call `add_mesh` yourself.
  Source: [`class_navigationserver3d.html`](https://docs.godotengine.org/en/4.7/classes/class_navigationserver3d.html)

- **`NavigationServer3D.bake_from_source_geometry_data(navigation_mesh: NavigationMesh, source_geometry_data: NavigationMeshSourceGeometryData3D, callback: Callable = Callable())`**
  — synchronous bake on the calling thread. Blocks until the polygon mesh is produced.

- **`NavigationServer3D.bake_from_source_geometry_data_async(navigation_mesh: NavigationMesh, source_geometry_data: NavigationMeshSourceGeometryData3D, callback: Callable = Callable())`**
  — same inputs, but runs on a background thread (see §4) and returns immediately; `callback`
  fires on completion.
  Source: [`class_navigationserver3d.html`](https://docs.godotengine.org/en/4.7/classes/class_navigationserver3d.html)

- **Attaching the result:** either
  - set `NavigationRegion3D.navigation_mesh = baked_mesh` (property, with generated
    getter/setter), which is the idiomatic path when you already have a region node in the
    tree; or
  - call `NavigationServer3D.region_set_navigation_mesh(region: RID, navigation_mesh: NavigationMesh)`
    directly against a region RID if you're managing regions without dedicated nodes.
  Source: [`class_navigationregion3d.html`](https://docs.godotengine.org/en/4.7/classes/class_navigationregion3d.html), [`class_navigationserver3d.html`](https://docs.godotengine.org/en/4.7/classes/class_navigationserver3d.html)

- **The node-level convenience wrapper**, if a `NavigationRegion3D` is already parenting the
  terrain mesh in the scene tree: `void bake_navigation_mesh(on_thread: bool = true)`. This
  internally does the parse+bake+assign sequence above and emits `bake_finished` when done
  (details in §4).
  Source: [`class_navigationregion3d.html`](https://docs.godotengine.org/en/4.7/classes/class_navigationregion3d.html)

There is **no** separate "`NavigationMeshGenerator`" class exposed in 4.7 — that name was the
Godot 3.x singleton. In Godot 4.x the equivalent functionality lives on `NavigationServer3D`
(`bake_from_source_geometry_data[_async]`) and the internal Recast-based baker is not
independently scriptable; you always go through `NavigationServer3D` or the
`NavigationRegion3D` node wrapper. (Confirmed by the absence of any `NavigationMeshGenerator`
class page in the 4.7 class reference and by `bake_navigation_mesh` being the only baking
entry point documented on `NavigationRegion3D`.)

---

## 2. Real-world cost of baking a large terrain navmesh at runtime

- The official 4.7 "Optimizing Navigation Performance" page states plainly that **"navigation
  baking is slow, and editor or compile-time baking navigation is usually a better option for
  games,"** and that runtime baking should be reserved for genuinely dynamic/generated terrain.
  It also warns that "even small meshes can take far longer to bake than what is possible to
  squeeze into a single frame," and calls out the classic mistake of feeding the baker one
  "big soup" of unpartitioned world geometry instead of splitting it into regions that only
  cover what's needed.
  Source: [`navigation_optimizing_performance.html`](https://docs.godotengine.org/en/4.7/tutorials/navigation/navigation_optimizing_performance.html)

- **Concrete real-world numbers** come from GitHub issue
  [godotengine/godot#96483](https://github.com/godotengine/godot/issues/96483)
  ("`NavigationServer3D::sync` could be optimized"): a reporter building a ~1720×500×1600-unit
  map from 2,650 meshes across 42 navmesh tiles found that **baking the 42 tiles took ~2
  seconds**, but **adding/syncing them into `NavigationServer3D` took ~15 minutes**. The root
  cause identified was `NavMap::sync`'s edge-merging step: an **O(N²) edge comparison with no
  spatial acceleration structure**, no caching of external-edge precomputation (recomputed
  every sync), and single-threaded execution. The issue directly compares this to a native
  Recast build of the same tile set completing in **5 milliseconds** — a roughly **180,000×**
  slower path inside Godot's sync step for this specific edge-merging pathology (not baking
  itself, but the map-sync/edge-merge phase that runs after baking, every time regions are
  added or changed).

- The same optimizing-performance doc gives the mechanism for *why* edge merging blows up:
  vertex-to-vertex merging (same grid cell) is cheap, but merging by "edge connection" — a
  second pass checking distance+angle between all edge pairs — is expensive, and its cost
  scales with how many separate region/tile boundaries exist. Recommendation given: maximize
  vertex-merge opportunities and minimize the number of edges that must fall back to the
  expensive angle/distance pass.
  Source: [`navigation_optimizing_performance.html`](https://docs.godotengine.org/en/4.7/tutorials/navigation/navigation_optimizing_performance.html)

- **Cheaper incremental/partial rebake path:** Godot 4.7 has no built-in "diff-only" or
  incremental navmesh rebake API — there is no method that re-bakes only a changed sub-region
  of an existing `NavigationMesh` and merges it back in. The only lever exposed is *regional
  partitioning*: keep the world split into multiple `NavigationRegion3D` instances (or
  multiple bakes assigned to separate region RIDs) so that a terrain-snapshot update only
  needs to re-bake and re-sync the regions whose geometry actually changed, not the whole
  world. This is exactly the workaround used by the community (see §5) — there is no engine
  primitive for true incremental re-baking of one region's mesh.
  Source: [`navigation_optimizing_performance.html`](https://docs.godotengine.org/en/4.7/tutorials/navigation/navigation_optimizing_performance.html);
  absence confirmed against the full `NavigationServer3D`/`NavigationMesh`/`NavigationRegion3D`
  4.7 API surface, none of which exposes a partial-region-update or diff bake call.

- A forward-looking (not yet shipped, proposal-stage) acknowledgment of this exact gap is
  Godot proposal
  [godot-proposals#12707](https://github.com/godotengine/godot-proposals/issues/12707)
  ("Add a NavigationWorld2D/3D as a navmesh high performance option for large worlds"), which
  explicitly states "many Godot users struggle to get navmesh and pathfinding working with even
  medium sized projects" and proposes new `NavigationWorld3D` / `NavigationGeometry` /
  `NavigationSeed` node types to automate chunked navmesh streaming with static vs. dynamic
  geometry modes. As of 4.7 this is **not implemented** — it's evidence the underlying
  ergonomics gap is recognized upstream, not a currently usable API.

---

## 3. NavigationAgent3D nodes vs. calling NavigationServer3D directly, at scale

- `NavigationAgent3D` is a `Node3D`-derived class (confirmed by the 4.7 class hierarchy: it
  inherits `Node`, holds its own `RID`, and is explicitly marked **experimental** in the 4.7
  docs, meaning its API "may change in future versions"). Each instance owns an avoidance
  callback and participates in per-physics-frame processing when avoidance or path-following
  helpers (`get_next_path_position()`, `distance_to_target()`, etc.) are used.
  Source: [`class_navigationagent3d.html`](https://docs.godotengine.org/en/4.7/classes/class_navigationagent3d.html)

- The 4.7 docs explicitly warn: **"avoidance processing with many registered agents has a
  significant performance cost and should only be enabled on agents that currently require
  it."** This is a direct, documented per-agent cost warning relevant to "hundreds of
  concurrent agents."
  Source: [`class_navigationagent3d.html`](https://docs.godotengine.org/en/4.7/classes/class_navigationagent3d.html)

- **Real-world regression report:** a Godot 4.2.2 user on the official forum reported
  framerate dropping from **2000–3000 fps down to 40–60 fps** after adding `NavigationAgent3D`
  path-following to a multi-agent scene. Root cause diagnosed in the thread: calling
  `get_next_path_position()` every `_physics_process()` tick even when a target was
  unreachable forces repeated expensive full-navmesh searches; the fix suggested was to stop
  querying every physics frame and instead re-query on a cooldown/timer.
  Source: [Godot Forum thread #64567](https://forum.godotengine.org/t/godot-terrible-performance-on-navigationagent3d/64567)

- **Direct-API alternative, confirmed by the Godot forum community** (thread: "How to handle
  large amounts of NavigationAgents / pathfinding?"): `NavigationAgent3D` nodes are optional —
  "their entire functionality can be replaced with scripts and direct calls to the
  NavigationServer API." Agents can be represented as lightweight data (position + target +
  RID) managed by a central manager/pool rather than as scene-tree nodes, calling
  `NavigationServer3D.map_get_path(map: RID, origin: Vector3, destination: Vector3, optimize: bool, navigation_layers: int = 1) -> PackedVector3Array`
  directly, or using `NavigationPathQueryObjects` for more advanced batched queries — the
  thread specifically recommends `NavigationPathQueryObjects` over per-agent nodes for
  "advanced uses" at scale (e.g. RTS-style large agent counts).
  Source: [Godot Forums thread #31934](https://godotforums.org/d/31934-how-to-handle-large-amounts-of-navigationagents-pathfinding)

- **Map synchronization delay:** the 4.7 `NavigationServer3D` reference states directly:
  **"Most NavigationServer3D changes take effect after the next physics frame and not
  immediately. This includes all changes made to maps, regions or agents."** The related
  method `map_get_iteration_id(map: RID) -> int` returns an identifier that increments each
  time a map finishes syncing (0 means it has never synced), letting a manager detect whether
  a `map_get_path()` call is being made against stale/pre-sync map state (e.g. immediately
  after a terrain region rebake, before the next physics frame has run). This is the
  documented mechanism behind the common "path query returns empty right after scene load or
  right after rebaking" complaints seen across the forum/issue tracker (e.g. the pattern
  described generically in the community write-ups: querying before the server's one-frame
  sync latency has elapsed returns stale or empty results, and the standard workaround is to
  defer the initial query with `call_deferred()`).
  Source: [`class_navigationserver3d.html`](https://docs.godotengine.org/en/4.7/classes/class_navigationserver3d.html)

  With hundreds of agents, this interacts two ways relevant to this client: (a) every agent
  querying `map_get_path()` in the same physics frame right after a terrain-regeneration
  rebake will all see the *same* stale/empty map state until the next physics tick — so a
  pool-based manager should gate new path requests on `map_get_iteration_id()` advancing past
  the pre-rebake value rather than firing immediately after `bake_finished`; and (b) because
  the sync is one global step per map per physics frame, batching many agents' path requests
  into a single manager call site (rather than N independent per-node calls scattered across
  `_physics_process`) is not just an ergonomics win but also makes it trivial to gate the
  whole batch on one iteration-id check.

---

## 4. Off-main-thread baking in Godot 4.7

- `NavigationRegion3D.bake_navigation_mesh(on_thread: bool = true)` — the `on_thread`
  parameter **defaults to `true`** in 4.7, i.e. calling it with no arguments already bakes on
  a background thread. The node emits the `bake_finished` signal ("notifies when the
  navigation mesh bake operation is completed") when done, and exposes `is_baking() -> bool`
  to poll status.
  Source: [`class_navigationregion3d.html`](https://docs.godotengine.org/en/4.7/classes/class_navigationregion3d.html)

- Doc-stated caveats on the same page: threaded baking can become **"very slow"** due to
  "heavy synchronization" when geometry is pulled live from `Mesh` resources (as opposed to
  raw arrays) because of thread-safety locking around mesh data access; and threading is
  **automatically disabled** on platforms without thread support (explicitly called out:
  "Web with threads disabled").
  Source: [`class_navigationregion3d.html`](https://docs.godotengine.org/en/4.7/classes/class_navigationregion3d.html)

- At the `NavigationServer3D` level, the equivalent explicit async entry point is
  `bake_from_source_geometry_data_async(navigation_mesh, source_geometry_data, callback)`,
  described as running "as an async task running on a background thread." Status can be
  polled with `NavigationServer3D.is_baking_navigation_mesh(navigation_mesh: NavigationMesh) -> bool`
  (this is the real 4.7 method name — **not** `is_baking_navigation_mesh_finished`, which does
  not exist in the API; completion is normally observed via the bake's `callback` argument or
  the node's `bake_finished` signal rather than polling).
  Source: [`class_navigationserver3d.html`](https://docs.godotengine.org/en/4.7/classes/class_navigationserver3d.html)

- Internally this threading is implemented via Godot's `WorkerThreadPool` (the engine's
  general-purpose background task pool used across baking, resource loading, etc. in 4.x);
  the 4.7 docs describe the async bake as backgrounded without requiring the caller to manage
  threads manually, consistent with the `WorkerThreadPool`-backed task model documented
  elsewhere in the 4.7 engine architecture docs.
  Source: [`class_navigationregion3d.html`](https://docs.godotengine.org/en/4.7/classes/class_navigationregion3d.html)

- **Change vs. older 4.0–4.2 behavior:** community bug reports from the 4.0/4.1 era (e.g.
  [godotengine/godot#59235](https://github.com/godotengine/godot/issues/59235), "Baking a
  Navmesh does not work at runtime") document that runtime baking was fragile/buggy in early
  4.x, and separate community write-ups ("Fix: Godot Navigation Mesh Async Bake Blocks Main
  Thread") describe cases where async bake still stalled the main thread due to the
  mesh-access synchronization issue mentioned above — i.e. the *default-on* background
  threading and its "heavy synchronization when pulling from Mesh resources" caveat are both
  long-standing, version-spanning characteristics, not new in 4.7, but the API surface
  (`on_thread` default `true`, explicit `_async` server method) is stable through the 4.x
  line up to 4.7.

---

## 5. Prior art: navigation on large, occasionally-regenerating procedural terrain

- **Zylann/godot_voxel** (the most widely used Godot voxel terrain module) has an open issue,
  [Zylann/godot_voxel#610](https://github.com/Zylann/godot_voxel/issues/610) ("Navmesh for
  Non-Blocky Voxel (Lod) Terrain"), tracking the exact problem this client faces:
  non-blocky/LOD voxel terrain doesn't map cleanly onto Godot's Recast-based navmesh baker,
  and there is no first-class solution — it remains an open design discussion, not a solved
  problem.
  Source: [Zylann/godot_voxel#610](https://github.com/Zylann/godot_voxel/issues/610)

- **voxel-tools documentation** states the core difficulty directly: "navmeshes are sensitive
  to source geometry, and voxel terrain is streamed, can be modified by players, and has
  highly variable shapes that require navigation to remain functional in-game. Any edit or
  streaming event requires constantly re-baking navmeshes to match the areas players are in."
  It lists experimental alternatives actually used by that community in lieu of navmesh
  baking: voxel-grid-based pathfinding, waypoint graphs with `AStar3D`, and raycast-based
  walkability checks — explicitly stating **"there is currently no general solution for
  dynamic voxel terrain AI navigation"** in Godot.
  Source: [Voxel Tools navigation docs](https://voxel-tools.readthedocs.io/en/latest/navigation/)

- **Terrain3D** (a popular Godot 4 heightmap terrain plugin) ships a concrete, working
  reference implementation for exactly this scenario: its `CodeGeneratedDemo` periodically
  re-bakes a `NavigationMesh` "in the area around the player" on a configurable
  `bake_cooldown` timer as the player moves, rather than re-baking the whole terrain or baking
  once statically. Its docs are explicit that this is a workaround, not a general solution:
  "navigation baking is slow, and editor or compile-time baking navigation is usually a
  better option for games... if your project has dynamic, generated terrain... you might need
  to use runtime navmesh baking." It also documents necessary trade-offs: smaller
  `mesh_size`/re-bake radius, larger `cell_size` for less voxel resolution, throttled
  `RETARGET_COOLDOWN` on NPC path re-queries because "if you have a lot of agents, that's going
  to come with a performance hit," and designing gameplay to tolerate navigation not being
  ready immediately (fallback behavior while baking is in flight).
  Source: [Terrain3D navigation docs](https://terrain3d.readthedocs.io/en/stable/docs/navigation.html)

- **Forum discussion** "Pathfinding 3D procedural terrain" on forum.godotengine.org confirms
  this pattern (radius-around-player rebake, not global rebake) is the community's de facto
  standard approach for procedural terrain, echoing Terrain3D's design.
  Source: [Godot Forum: Pathfinding 3D procedural terrain](https://forum.godotengine.org/t/pathfinding-3d-procedural-terrain/78004)

- **Official 4.7 performance docs reinforce the same chunking principle** generically (not
  voxel-specific): avoid "one big navmesh soup," prefer "multiple small nav meshes that cover
  only what you need, rather than one giant one covering the entire world" — i.e. use several
  `NavigationRegion3D` instances keyed to world chunks/regions instead of a single global
  region, so that a terrain update only touches the regions that changed.
  Source: [`navigation_optimizing_performance.html`](https://docs.godotengine.org/en/4.7/tutorials/navigation/navigation_optimizing_performance.html)

---

## Recommendation

For this client (procedural `ArrayMesh` terrain rebuilt on every snapshot/delta, potentially
hundreds of NPCs):

**Navmesh structure and rebaking**

1. **Do not use one global `NavigationRegion3D` covering the whole terrain.** Partition the
   world into per-chunk/per-region `NavigationRegion3D` instances (mirroring however
   `terrain_generator.gd` already tiles/chunks the terrain, or introducing chunking there if
   it currently emits one monolithic mesh). This is the one lever Godot 4.7 actually gives you
   in place of true incremental rebaking (§2), and it is the pattern every piece of prior art
   in §5 converges on independently (Terrain3D, the official perf docs, and the voxel_godot
   community's implicit workarounds).
2. **On snapshot/delta update, only rebake the regions whose geometry actually changed.**
   Diff the incoming terrain data against the previous snapshot at the chunk level (you likely
   already know which chunks changed from the delta), and only call
   `bake_from_source_geometry_data_async` for those regions' `NavigationMeshSourceGeometryData3D`
   (built via `add_mesh`/`add_mesh_array` straight from the `SurfaceTool`-produced `ArrayMesh`).
   Untouched chunks keep their existing baked `NavigationMesh` — this is the closest
   Godot 4.7 gets to "incremental" rebaking, since the engine itself has no partial-region bake
   primitive (§2).
3. **Always bake off the main thread.** Use `bake_from_source_geometry_data_async(...)`
   (server-level, if you're not routing through per-chunk `NavigationRegion3D` nodes) or rely
   on `NavigationRegion3D.bake_navigation_mesh()`'s `on_thread` default of `true` (§4). Gate
   gameplay logic that depends on the new navmesh on the bake's `callback`/`bake_finished`
   signal, not a fixed delay.
4. **After a rebake, gate new path queries on `map_get_iteration_id()` advancing**, not
   immediately on `bake_finished` — the server-level map sync itself lags one physics frame
   behind the region assignment (§3), and with hundreds of agents potentially all requesting
   paths in the same frame right after a chunk rebake, a shared "wait for iteration id to
   advance" check in the manager avoids every agent independently hitting the stale-map
   pathology described in the forum threads.
5. **Watch region count and edge-merge cost, not just per-chunk bake time.** The #96483 issue
   (§2) shows the dangerous cost center is `NavMap::sync`'s O(N²) edge-merging across many
   regions/tiles, not the Recast bake step itself. Keep chunk count reasonable (large enough
   chunks, not one region per tiny tile) and keep chunk boundaries grid-aligned so edges
   merge cheaply by vertex rather than falling into the expensive
   distance/angle edge-connection pass.

**Per-agent pathing for hundreds of NPCs**

6. **Do not give every NPC its own `NavigationAgent3D` node** if you expect hundreds of them
   simultaneously active. The class is explicitly experimental in 4.7, avoidance cost is
   documented to scale badly with agent count, and there's a first-hand community report of a
   50x framerate collapse (2000-3000fps → 40-60fps) from per-agent path-following alone (§3).
7. **Build a lightweight NPC pathing manager that calls `NavigationServer3D.map_get_path()`
   directly** from pooled agent data (position, target, map RID, navigation_layers) rather than
   per-node avoidance/physics-process overhead. Only fall back to real `NavigationAgent3D`
   nodes (with `avoidance_enabled = true`) for the small subset of NPCs that actually need
   local RVO avoidance (e.g. NPCs currently near the player/camera); leave background/distant
   NPCs on plain server path queries with no avoidance.
8. **Throttle path re-queries.** Don't call `map_get_path()` (or `get_next_path_position()` if
   using agent nodes) every physics frame per NPC — batch/stagger queries on a cooldown per
   NPC (as both the forum fix in §3 and Terrain3D's `RETARGET_COOLDOWN` guidance in §5
   recommend), especially right after a chunk rebake when many NPCs may need to re-path at
   once.

This combination — chunked regions rebaked only where changed, async off-thread baking gated
on iteration-id, and a pooled direct-`NavigationServer3D` manager instead of per-node agents
for the bulk of NPCs — is the synthesis of what Godot's own docs recommend generically, what
the `#96483` and forum performance reports warn against concretely, and what the
Terrain3D/voxel_godot prior art actually ships as a working pattern for regenerating
procedural terrain.
