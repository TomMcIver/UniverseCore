# Free/Open 3D Asset Sources and Procedural Terrain for a Grounded Medieval Look

Research for UniverseCore #10. Target visual reference: Mount & Blade II: Bannerlord,
Manor Lords, Kingmakers — grounded, semi-realistic medieval. Explicitly NOT Civilization
6's cartoon-hex/stylized look. Target engines: Bevy (Rust) and Godot, both consuming
glTF, so glTF-compatible assets are preferred throughout.

All license claims below were verified by fetching the primary source page directly
(WebFetch) except where noted as "search-result only" (OpenGameArt.org pages returned a
TLS/SSL negotiation error from the fetch tool on every attempt — `error:100000f7:SSL
routines:OPENSSL_internal:WRONG_VERSION_NUMBER` — so those entries are backed by search
snippets and the submission's public listing rather than a direct page fetch; the OGA
license field is normally reliable and worth a manual spot-check before committing).

---

## 1. Kenney.nl

Kenney publishes everything under CC0 (public domain). Verified directly:

- **[Castle Kit](https://kenney.nl/assets/castle-kit)** — 75 pieces. Page fetch confirms:
  "released under the Creative Commons CC0 license", linking directly to
  `https://creativecommons.org/publicdomain/zero/1.0/`.
- **[Medieval Town (Base)](https://kenney.nl/assets/medieval-town-base)** — 65 assets,
  CC0 (confirmed via search result summary of the same page pattern Kenney uses site-wide;
  direct fetch of this specific URL 404'd on one attempt, but the pack is listed on
  kenney.nl's own assets index and Kenney's site-wide license statement below applies
  uniformly).
- **[Retro Medieval Kit](https://kenney.nl/assets/retro-medieval-kit)** and
  **[Fantasy Town Kit](https://kenney.nl/assets/fantasy-town-kit)** — same CC0 terms,
  useful for stylized props if a lower-fidelity fallback style is ever needed.
- Kenney's general licensing position (per site-wide FAQ/support content surfaced in
  search): "All game assets on the asset pages are public domain licensed (CC0)... free
  to use, even in commercial projects... Attribution is not required but if you choose to
  credit the source, refer to 'Kenney', 'Kenney.nl' or 'www.kenney.nl'." Source:
  [kenney.nl/support](https://kenney.nl/support), corroborated by
  [Kenney's own statement on X](https://x.com/KenneyNL/status/1989626866336993559).

**Format note:** Kenney packs are typically distributed with glTF among the export
formats (OBJ/FBX/glTF/Blender), consistent with Kenney's stated cross-engine intent —
confirm per-pack on the download page since older packs sometimes ship OBJ/FBX only.

**Relevance to medieval style:** Castle Kit and Medieval Town (Base) are low-poly,
flat-shaded/vertex-colored — closer to a clean stylized look than Bannerlord/Manor Lords
grit. Useful as base-mesh geometry (wall segments, roofs, gates) to retexture with PBR
materials, not as drop-in finished assets for a "grounded" look.

---

## 2. Quaternius

Quaternius (quaternius.com, also mirrored on itch.io as `quaternius.itch.io` and via
Patreon) is CC0-first but not universally CC0 — some newer/premium packs are Patreon-gated
or dual-licensed. Verified directly:

- **[Medieval Village MegaKit](https://quaternius.com/packs/medievalvillagemegakit.html)**
  — 300+ modular environment pieces, snap-to-grid walls (interior+exterior), roofs,
  stairs. Fetch confirms: **License: CC0**
  (`https://creativecommons.org/publicdomain/zero/1.0/`), "Free to use in personal,
  educational and commercial projects." No attribution required. **Formats confirmed
  available: glTF, FBX, OBJ** (plus .blend in the "Source" tier).
  Also mirrored at [quaternius.itch.io/medieval-village-megakit](https://quaternius.itch.io/medieval-village-megakit).
- **[Medieval Village Pack](https://quaternius.com/packs/medievalvillage.html)** — 40+
  models (houses, props), same CC0 terms per search-result corroboration; also listed on
  [Poly Pizza](https://poly.pizza/bundle/Medieval-Village-Pack-NsHhjhlrfY) as a
  CC0-compatible bundle and on the [Godot Asset Store](https://store.godotengine.org/asset/quaternius/medieval-village-megakit/).
- General Quaternius licensing caveat worth flagging for the issue: not everything on
  quaternius.com is CC0 — the site now runs some content through Patreon tiers, and
  overall roughly 60-70% of the catalog is free/CC0 per third-party summaries surfaced in
  search. **Action item: check the license badge on each individual pack page before
  use** — don't assume CC0 site-wide the way Kenney's catalog allows.

**Relevance to medieval style:** Quaternius's style is stylized/low-poly with flat
shading, similar aesthetic bucket to Kenney but slightly more detailed geometry. Good
for background/filler buildings and modular kit-bashing; still visually distinct from
Bannerlord/Manor Lords' higher-frequency PBR detail.

---

## 3. OpenGameArt.org

OGA licenses are set **per submission** by the individual uploader — CC0, CC-BY 3.0/4.0,
CC-BY-SA, and GPL all appear across the site, so each asset must be checked individually.
Direct WebFetch of opengameart.org pages failed with a TLS error on every attempt in this
session; the entries below are sourced from OGA's own search-result snippets/listings
(title + license shown in the search index), which is how OGA surfaces license info on
listing pages, but should get a manual click-through confirmation before any asset is
actually pulled into the project.

- **[Modular Medieval Building Pack](https://opengameart.org/content/modular-medieval-building-pack)**
  — license listed as **CC0 / Public Domain**, .blend source files.
- **[LowPoly Medieval Buildings](https://opengameart.org/content/lowpoly-medieval-buildings)**
  — license listed as **CC0**, ships as Blend/OBJ/FBX.
- **[Medieval Town (base) 3D assets](https://opengameart.org/content/medieval-town-base-3d-assets)**
  — license listed as **CC0**; walls, roofs, floors, roads for a modular town (this is a
  community mirror/derivative of the Kenney base pack, not to be confused with Kenney's
  own page).
- **[Isometric Medieval Buildings](https://opengameart.org/content/isometric-medieval-buildings)**
  and **[#2](https://opengameart.org/content/isometric-medieval-buildings-2)** — license
  listed as **CC0**. Isometric-projection sprites/renders, likely only useful as texture
  reference rather than 3D geometry.
- **[Modular Medieval House Pack 01](https://opengameart.org/content/modular-medieval-house-pack-01)**,
  **[04](https://opengameart.org/content/modular-medieval-house-pack-04)**,
  **[05](https://opengameart.org/content/modular-medieval-house-pack-05)** — license
  listed as **CC0**.
- **[3D Buildings](https://opengameart.org/content/3d-buidlings)** — **CC-BY 4.0**
  (attribution required), 30+ building models pitched at RTS-style fantasy-medieval
  games. This is the one non-CC0 entry in this set — flag for attribution tracking if
  used.

**Relevance to medieval style:** OGA's medieval building packs are contributed by many
different hobbyist artists over many years — expect wide variance in poly density,
texture resolution, and style even within a single pack's "CC0" umbrella. Good for
filling out background/secondary structures cheaply; not a stylistically unified
source on its own.

---

## 4. itch.io

itch.io hosts both original packs and mirrors of Kenney/Quaternius content, filterable by
the CC0 tag directly: [itch.io/game-assets/assets-cc0/tag-medieval](https://itch.io/game-assets/assets-cc0/tag-medieval).

- **[Medieval Village MegaKit (Quaternius)](https://quaternius.itch.io/medieval-village-megakit)**
  — same CC0 pack as above, itch.io mirror.
- **[Medieval Voxel Pack (CC0-Based) by Dhil](https://itsdhil.itch.io/medieval-voxel-pack-cc0-based)**
  — voxel-style houses/farms/weapons/props/nature, "CC0 – Free for personal & commercial
  use." Voxel aesthetic is a style mismatch for Bannerlord-grounded look; useful only for
  a very different visual direction.
- **[Free CC0 Medieval Props by 3dmodelscc0](https://3dmodelscc0.itch.io/free-cc0-medieval-props)**
  — 10 props (cannon, crossbow, gallows, guillotine, market stall, goblet, beer mug, sack
  of grain, scroll, skeleton key) under CC0 public domain. Good small prop filler; too
  small a set to be a primary source.
- **[3D Retro Medieval/Fantasy Building Kit by chilly_durango](https://itch.io/game-assets/tag-3d-model/tag-medieval)**
  (surfaced via the tag-medieval + 3d-model itch.io listing) — PSX/late-90s-PC low-fi
  style; explicitly a style mismatch for the grounded/semi-realistic target, useful only
  as a stopgap greybox aesthetic.
- **[Highlands – Fantasy 3D Buildings Pack by Visions Mind](https://itch.io/game-assets/tag-3d-model/tag-medieval)**
  — stylized fantasy-strategy buildings, again more toward a Civ-style stylization than
  Bannerlord grit; worth a look only if the team ends up compromising on style direction.

**Relevance to medieval style:** itch.io's main value here is aggregation/discovery
(dedicated CC0+medieval filter) rather than unique high-fidelity content — most of the
strongest hits are re-listings of Kenney/Quaternius work already covered above.

---

## 5. Sketchfab

Sketchfab supports several distinct license tracks on downloadable models, and the
**license badge shown on each model page is the actual controlling term** — always check
it per-model rather than assuming.

**License variants observed on Sketchfab (per Sketchfab's own community blog, confirmed
by fetch, and Sketchfab's 2014 CC announcement):**
- **CC0 (Public Domain Dedication)** — newer program, notably used for 3D cultural
  heritage scans; per Sketchfab's blog "[Sketchfab Launches Public Domain Dedication for
  3D Cultural Heritage](https://sketchfab.com/blogs/community/sketchfab-launches-public-domain-dedication-for-3d-cultural-heritage/)",
  users "can copy, modify, distribute and perform the work, even for commercial
  purposes, all without asking permission." No attribution needed.
- **CC-BY (Attribution)** — Sketchfab has offered this since 2014
  ([Creative Commons' own announcement](https://creativecommons.org/2014/10/30/sketchfab-announces-cc-licensing-microsoft-htc-and-british-museum-early-adopters/)).
  Reuse requires you to "give appropriate credit, provide a link to the license, and
  indicate if changes were made" — i.e., name the author, link the specific CC license
  version, and note if the model was modified.
- **CC-BY-SA (ShareAlike)** — attribution as above, *plus* any derivative you publish
  must be re-licensed under the same CC-BY-SA terms.
- **CC-BY-NC / CC-BY-NC-SA (NonCommercial)** — same attribution requirement, but
  explicitly bars commercial use, which rules these out for a commercial or
  ambiguously-licensed game project unless the project itself is non-commercial.
- **CC-BY-ND (NoDerivatives)** — attribution required, and the model may not be
  altered/remixed and redistributed in modified form — restrictive for a game pipeline
  that typically re-textures/re-rigs imported models.
- Separately, Sketchfab also sells models under its own **proprietary Standard/Editorial
  commercial licenses** (not Creative Commons at all) — confirmed via
  [sketchfab.com/licenses](https://sketchfab.com/licenses): Editorial License is
  restricted to non-commercial/educational/news use; Standard License is a paid
  royalty-free license that still prohibits redistributing the file standalone. These
  are unrelated to the free CC-licensed track and irrelevant unless the team decides to
  pay for specific paid models.

**Specific models found (verify the badge on the model page before use — search
snippets are not a substitute for reading the actual license badge in this case):**
- **[Medieval Fun Fair – CC0 Asset Pack by Polygonal Mind](https://sketchfab.com/3d-models/medieval-fun-fair-cc0-asset-pack-d46e293701f1467a961b3eddfa0792db)**
  — explicitly labeled CC0 in the title; a fairground/market prop set, useful for
  village-life set dressing.
- **[Medieval Cart by sch0i](https://sketchfab.com/3d-models/medieval-cart-5388ccf5011646d8ba0ae9e58e7ba777)**
  and **[Medieval Cart by leone.giulio.92](https://sketchfab.com/3d-models/medieval-cart-6788a348fe2d4403b29c16221d2eaf54)**
  — two independent cart models; license tier not confirmed by direct fetch in this
  session, check the page's license badge (search snippets did not surface CC0 vs CC-BY
  for these two specifically).
- **[Medieval Peasant Outfit Free by Red_Ilya](https://sketchfab.com/3d-models/medieval-peasant-outfit-free-e7aa183b848646558ceb3a3f449e344b)**
  — search result indicates **CC Attribution** license (attribution required).
- **[Low-Poly Medieval Peasant Character by Keash](https://sketchfab.com/3d-models/low-poly-medieval-peasant-character-1c188c6fb8d74d7aad10a8c4c62e50d4)**
  — burlap/hessian shirt, olive pants, leather belt; search result indicates **CC
  Attribution** license as well.
- Broader discovery: [sketchfab.com/tags/peasant](https://sketchfab.com/tags/peasant) and
  [sketchfab.com/tags/cc0](https://sketchfab.com/tags/cc0) are the right entry points for
  further character/prop search — filter explicitly by the CC0 tag or by "Downloadable +
  license" facets in Sketchfab's own search UI when actually sourcing.

**Relevance to medieval style:** Sketchfab is the most promising source for
higher-fidelity single hero props (carts, wells) and character bases, because individual
artists upload PBR-textured, higher-poly work there — but it is the least consistent
source stylistically since every model comes from a different author with different
software/skill level, and most usable free character content requires attribution
tracking (CC-BY), which is an ongoing legal/credits-page obligation, not a one-time cost.

---

## 6. Procedural terrain and lighting techniques

### Heightmap-based terrain generation
The standard approach in both engines: a grayscale/float heightmap image (or Perlin/
simplex noise function) displaces a grid mesh's vertex Y, then normals are recomputed for
lighting. Both target engines have multiple community implementations rather than a
single blessed built-in:

**Bevy:**
- **[bevy_terrain](https://github.com/kurtkuehnert/bevy_terrain)** — the most
  sophisticated option, built for a bachelor's thesis on large-scale terrain rendering;
  implements a "UDLOD" (uniform-distance level of detail) algorithm plus a chunked
  clipmap data structure for streaming large heightmaps efficiently. See its
  [implementation notes](https://github.com/kurtkuehnert/bevy_terrain/blob/main/docs/implementation.md).
  Best fit if terrain needs to scale to a large open world with LOD.
- **[bevy_mesh_terrain](https://lib.rs/crates/bevy_mesh_terrain)** — simpler, chunk-based
  heightfield plugin (conceptually like Minecraft chunking but with heightfields instead
  of voxels); lower complexity, good starting point.
- **[bevy_heightmap](https://crates.io/crates/bevy_heightmap)** — minimal utility crate
  that turns a heightmap PNG (or a procedural value function) into a Bevy mesh; a good
  low-level building block if the team wants to write custom LOD/streaming logic on top.
- Bevy has no terrain-specific built-in beyond its general-purpose `Mesh` API — any
  terrain system is a third-party crate or hand-rolled mesh generation
  ([reference walkthrough](http://clynamen.github.io/blog/2021/01/04/terrain_generation_bevy/),
  [kcstuff.com procedural-generation write-up](https://kcstuff.com/blog/procedural-generation-bevy)).
- **Vegetation scattering for Bevy:** several early-stage crates exist —
  [bevy_procedural_grass](https://github.com/jadedbay/bevy_procedural_grass) (generates
  grass on top of an arbitrary mesh, targets Bevy 0.12 per its README — check version
  compatibility against the project's current Bevy version), 
  [bevy_feronia](https://lib.rs/crates/bevy_feronia) (declarative foliage/wind shader
  toolkit, more visually ambitious), and
  [FrostyGrass](https://github.com/DavidHospital/FrostyGrass) (GPU-instanced grass
  renderer). None of these are as mature/maintained as Godot's terrain ecosystem — expect
  to patch or fork.

**Godot:**
- **[Terrain3D](https://godotassetlibrary.com/asset/9w8fzH/terrain3d)** — the current
  best-in-class option for Godot 4.4+: GPU-driven clipmap mesh terrain, foliage
  instancing with up to 10 LOD levels, sculpting, holes, texture painting with
  texture-detiling, and color/wetness painting. Written in C++ as a GDExtension for
  performance. This is the closest thing to an "industry standard" free Godot terrain
  plugin as of 2026.
- **[HTerrain (Zylann)](https://github.com/Zylann/godot_heightmap_plugin)** — the older,
  pure-GDScript heightmap plugin; documented at
  [hterrain-plugin.readthedocs.io](https://hterrain-plugin.readthedocs.io/). Offers three
  texture-blending modes: `CLASSIC4` (simple 4-texture RGBA splatmap weighting, cheapest),
  `ARRAY` (up to 256 textures via a texture array, more constrained pipeline), and
  `LOW_POLY` (no textures, vertex-color only). Good documentation, easier to hack on
  since it's GDScript, but lower performance ceiling than Terrain3D.
- **[TerraBrush](https://github.com/spimort/TerraBrush)** — heightmap editor with
  bump-map-driven blending and camera-distance-limited foliage; a lighter-weight
  alternative to Terrain3D/HTerrain.
- **[M Terrain](https://godotengine.org/asset-library/asset/2097)** — includes a grass
  system with collision support, positioned for building "large forests of trees
  efficiently."
- **Splatmap-driven texture blending** (relevant to both engines conceptually): a
  splatmap stores per-texture blend weights in RGBA channels; slope, height, or a
  hand-authored "soil quality" data layer can drive the weights procedurally (e.g., high
  slope → rock texture, low elevation + high moisture → mud/grass, near roads → dirt).
  HTerrain's CLASSIC4 shader is the textbook implementation of this pattern.

### Lighting / atmosphere (cheap but high perceptual payoff)
Cited in a general-engine survey since the specific Bevy/Godot APIs (fog volumes,
directional light + sky shaders) are largely equivalent in capability:
- **Volumetric fog / god rays via ray-marching** is the standard modern technique —
  volumetric clouds, ground fog, and crepuscular rays ("god rays") all reuse the same
  ray-march-through-a-density-field approach
  ([Cinevva: volumetric clouds and weather effects](https://app.cinevva.com/blog/2026-05-04-volumetric-clouds-and-weather),
  [gamedeveloper.com: atmospheric scattering and volumetric fog, part 1](https://www.gamedeveloper.com/programming/atmospheric-scattering-and-volumetric-fog-algorithm-part-1)).
  Full raymarched volumetrics are the "nice" version but are also the most expensive.
- **Cheap fallback for constrained budgets:** altitude-based fog using a simple gradient
  texture/exponential-height-fog formula gives most of the perceived depth-and-mood
  benefit for a fraction of the cost, with billboard/particle sprites simulating
  localized god-ray shafts near strong light sources rather than a full volumetric pass
  ([game-developers.org: volumetric lighting explained](https://game-developers.org/volumetric-lighting-explained),
  [moonjump.com: god rays — how it works](https://moonjump.com/game-dev-mechanics-volumetric-lighting-god-rays-how-it-works/)).
  This "cheap altitude fog + directional sun shafts" combo is very likely the right
  starting point for a Bevy/Godot indie-scale project rather than committing to full
  ray-marched volumetrics immediately.
- **Time-of-day / atmospheric scattering:** physically-motivated sky color comes from
  Rayleigh + Mie scattering + ozone absorption computed against sun angle; most shipped
  games simplify this to 4-12 discrete weather/lighting presets blended over 30-120
  second transitions rather than a fully continuous physical sim
  ([Kronnect: Fog in Unity URP — general technique reference](https://kronnect.com/company/blog/fog-in-unity-urp-complete-guide/)).
  This preset-blend approach is directly portable to Bevy (swap directional light color/
  intensity + ambient + fog params on a timer) or Godot (`WorldEnvironment` + a
  `Sky` shader driven by a day/night `Curve`/`Gradient` resource) without needing full
  atmospheric physics.
- **Practical read for "grounded" feel specifically:** Bannerlord/Manor Lords' grounded
  look leans heavily on (a) desaturated, slightly warm/cool time-of-day lighting rather
  than saturated fantasy palettes, (b) subtle ground fog in valleys/mornings, and (c)
  strong directional key light with soft ambient fill — all of which are cheap to
  approximate with a single directional light + exponential height fog + a modest
  color-grading LUT, well before any volumetric ray-marching is needed.

---

## 7. Hybrid feasibility assessment

**What the hybrid approach (free asset packs + procedural terrain + atmospheric
lighting) can plausibly achieve:** a competent, readable, functionally complete medieval
strategy/sim environment — buildings, roads, carts, basic character silhouettes,
naturalistic ground cover, weather/time-of-day mood — built at effectively zero art
budget and with an engine-appropriate (Bevy/Godot) technical pipeline. For a
prototype-to-early-alpha stage, or for a project where gameplay/simulation depth is the
actual differentiator (as UniverseCore's broader scope suggests), this is a genuinely
sound strategy: Kenney/Quaternius CC0 kits give a consistent enough base for
*within-kit* geometry, OGA/Sketchfab fill specific gaps (carts, props, peasant
silhouettes), and procedural terrain + cheap atmospheric fog/time-of-day lighting do
most of the "grounded feel" work that Bannerlord/Manor Lords get from expensive
production art — because a lot of that grounded feel is actually a lighting/atmosphere
effect, not a modeling-fidelity effect, and that part is cheap to replicate.

**Where it will visibly fall short of Bannerlord/Manor Lords/Kingmakers, honestly:**
1. **Cross-pack style mismatch.** Kenney (flat-shaded low-poly), Quaternius (stylized
   low-poly, slightly higher detail), OGA (wildly inconsistent — depends on which
   hobbyist made it), and Sketchfab (individually high-quality but no shared texture
   language) do not share a lighting model, texture resolution, or level of surface
   noise/wear. Put side by side in one scene, the seams are obvious — this is the single
   biggest tell that assets are aggregated rather than art-directed, and no amount of
   post-processing fully hides it.
2. **Building variety and tileset repetition.** Even the best free modular kits
   (Kenney's Medieval Town Base, Quaternius's MegaKit) have on the order of dozens of
   unique pieces, not the hundreds of hand-crafted, procedurally-varied building
   footprints Manor Lords uses to avoid visible repetition at settlement scale. Expect
   noticeably repetitive villages/towns unless significant effort goes into kit-bashing
   variety on top of the base pieces.
3. **Character rig/animation incompatibility.** Free character models (Sketchfab
   peasants, Quaternius/Kenney simple humanoids) come from different rigging
   conventions, bone naming, and animation sets. Getting a unified walk/idle/combat
   animation set across characters sourced from multiple authors typically requires
   re-rigging to a common skeleton (e.g., Mixamo re-targeting) — a real, non-trivial
   integration cost the free-asset story doesn't advertise.
4. **Texture resolution / PBR material quality gap.** Bannerlord and Manor Lords use
   production-grade PBR materials (high-res normal/roughness/AO maps, weathering,
   micro-detail) that free CC0 packs mostly do not include — Kenney/Quaternius ship
   flat-colored or low-res textures by design (small download size, broad compatibility).
   Closing this gap requires either sourcing separately-licensed PBR texture sets (e.g.,
   from ambientCG or Poly Haven, not covered in this research pass but worth a follow-up)
   and reapplying them to the free geometry, or accepting a visibly flatter/cleaner look
   than the AAA references.
5. **Procedural terrain will read as "good enough," not "beautiful," without texture
   authoring effort.** A splatmap-blended heightmap with off-the-shelf tiling textures
   gets convincing at a glance but shows visible tiling/repetition on close inspection
   unless combined with detail-normal maps, macro-variation noise, and decals — all of
   which are additional (if individually cheap) implementation steps beyond "install
   Terrain3D/HTerrain and paint."

**Overall verdict:** this hybrid stack is the right choice for getting a UniverseCore
prototype to a "recognizably medieval, not embarrassing, definitely not Civ-6-cartoon"
state quickly and for free, and the procedural-terrain + atmospheric-lighting half of the
plan is genuinely capable of doing more visual heavy lifting than the asset-pack half —
but it will plateau at a clearly "indie aggregate" look, not a Bannerlord/Manor
Lords-tier look, without a subsequent, deliberate art pass (a consistent PBR texture
set, re-rigged/unified characters, and kit-bashed building variety). Budget for that pass
as a distinct later milestone rather than assuming the free assets alone will close the
gap.

---

## Recommendation

**Start with, in this order:**
1. **[Quaternius Medieval Village MegaKit](https://quaternius.com/packs/medievalvillagemegakit.html)**
   (CC0, glTF/FBX/OBJ, 300+ modular pieces) as the primary building/structure kit — it has
   the best piece count and grid-snap modularity of anything surveyed, directly reducing
   the "repetition" risk called out above.
2. **[Kenney Castle Kit](https://kenney.nl/assets/castle-kit)** and
   **[Medieval Town (Base)](https://kenney.nl/assets/medieval-town-base)** (both CC0) as a
   secondary/filler kit and for early greyboxing — Kenney's asset naming/scale
   conventions are also unusually well documented, which helps a small team move fast.
3. **[Free CC0 Medieval Props (3dmodelscc0, itch.io)](https://3dmodelscc0.itch.io/free-cc0-medieval-props)**
   and **[Medieval Fun Fair CC0 pack (Sketchfab, Polygonal Mind)](https://sketchfab.com/3d-models/medieval-fun-fair-cc0-asset-pack-d46e293701f1467a961b3eddfa0792db)**
   to round out market/village-life set dressing (carts, stalls, everyday props) without
   attribution overhead.
4. For characters, budget for attribution tracking and expect re-rigging: the
   [Sketchfab peasant models](https://sketchfab.com/tags/peasant) found are CC-BY (credit
   required), and will need retargeting onto one shared skeleton before they can share an
   animation set.
5. On the terrain/lighting side: prototype with **Godot Terrain3D** (if Godot is chosen —
   it's the most capable, actively maintained option found) or **bevy_terrain** /
   **bevy_mesh_terrain** (if Bevy — pick `bevy_mesh_terrain` first for simplicity, escalate
   to `bevy_terrain` only if LOD/streaming at scale becomes a real requirement), paired
   from day one with a simple exponential height-fog + single-directional-light +
   day/night color-grade setup — this lighting combo is cheap and, per the assessment
   above, is likely to matter more to the "grounded" perception than any single asset
   pack choice.

**Hybrid-feasibility bottom line:** free CC0/CC-BY medieval assets plus procedural
heightmap terrain plus cheap atmospheric lighting will get UniverseCore to a solid,
non-embarrassing, clearly-not-Civ-6 medieval look at zero art budget — but it will look
like a well-assembled indie project, not a Bannerlord/Manor Lords-tier production, until
a dedicated follow-up pass adds a unified PBR texture language, re-rigged/animation-
unified characters, and hand-authored building variety on top of the kit-bashed
geometry; plan and schedule that follow-up pass explicitly rather than treating the
free-asset hybrid as a finished visual solution.
