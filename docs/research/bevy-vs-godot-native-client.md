# Native Desktop Client Engine Comparison: Bevy vs Godot (vs alternatives)

**Date:** 2026-09-18
**Related issue:** [#9](https://github.com/TomMcIver/UniverseCore/issues/9) (sub-issue of map issue #8)
**Scope:** Compare open-source, native-desktop game engines for UniverseCore's future player-facing client. Bevy (Rust) is the headline candidate; Godot is the baseline comparator; other candidates are included only if genuinely competitive.
**Not in scope:** Re-litigating the open-source constraint (Unity/Unreal are excluded by prior decision) or committing to a final choice — this is a research input for that decision.

## Visual target, stated plainly

The aspirational reference points are **Mount & Blade II: Bannerlord**, **Manor Lords**, and **Kingmakers** — grounded, semi-realistic medieval visuals: believable outdoor lighting (sun, fog, time-of-day), materials that read as real dirt/grass/stone, terrain that isn't flat-shaded cartoon tiles. Manor Lords itself is built on Unreal Engine 4, so that exact fidelity tier is explicitly **not** the bar an open-source engine choice is being held to here. The question is relative: which open-source, native-desktop engine gets **closer** to that grounded/atmospheric look, versus which one defaults to a flatter, more stylized (Civ-6-like) presentation.

---

## Candidates evaluated

| Engine | Language | License | Included? |
|---|---|---|---|
| **Bevy** | Rust | Dual MIT / Apache-2.0 [(GitHub)](https://github.com/bevyengine/bevy) | Yes — headline candidate |
| **Godot 4** | GDScript / C# / C++ (GDExtension) | MIT | Yes — baseline comparator |
| **Fyrox** | Rust | MIT (per crates.io/lib.rs listing) [(lib.rs)](https://lib.rs/crates/fyrox) | Yes — only other genuinely competitive Rust-native option |
| **O3DE (Open 3D Engine)** | C++ | Apache 2.0 / MIT [(GitHub)](https://github.com/o3de/o3de) | Considered, excluded — see below |
| **Flax Engine** | C++/C# | Source-available + commercial royalty tiers, **not** OSI-style open source [(Phoronix)](https://www.phoronix.com/news/Flax-Engine-1.1.6217) | Excluded — fails the "genuinely open-source" gate |

**Why O3DE is excluded despite Apache-2.0 licensing:** O3DE's Atom renderer targets AAA/simulation-grade fidelity and is a legitimate Unreal-class open-source option in principle, but as of 2026 "there are no major shipped commercial games built on O3DE comparable to titles built on its competitors," and it is "best suited to studios, enterprises, and research teams" with dedicated rendering/tooling engineers, not solo devs with no game-engine background [(labhub.hopto.org)](https://labhub.hopto.org/blog/culture/2026-05-16-open-source-game-engines-2026-godot-4-4-defold-bevy-gamemaker-phaser-construct-stride-pixijs-deep-dive?lang=en). Its tooling/build complexity and near-absent hobbyist ecosystem make it a poor match for criterion 5 (learning curve) even though it might win criterion 1 (fidelity ceiling) on paper. Noted for completeness, not scored in depth below.

**Why Flax is excluded:** despite technical merit, it is not free/libre — "companies with under $250K annual revenue pay nothing, larger ones owe 4% royalties," and by strict open-source definition it is "source-available plus a commercial license" [(Phoronix)](https://www.phoronix.com/news/Flax-Engine-1.1.6217). That fails criterion 6 outright.

---

## 1. Rendering fidelity for a grounded-medieval look

### Bevy

- Bevy's renderer is built around `bevy_pbr` / `StandardMaterial`, implementing a standard metallic-roughness PBR model [(bevy.org PBR example)](https://bevy.org/examples/3d-rendering/pbr/), [(docs.rs bevy::pbr)](https://docs.rs/bevy/latest/bevy/pbr/index.html).
- Global illumination is still maturing: `bevy_light`/`bevy_pbr` expose `LightProbe` components — `EnvironmentMapLight` (reflection probes) and `IrradianceVolume` (baked diffuse GI) — but this is manual/baked, not automatic scene-wide GI [(DeepWiki: Lighting and Shadows)](https://deepwiki.com/bevyengine/bevy/5.6-lighting-and-shadows).
- Bevy 0.18 (early 2026) added an **atmosphere occlusion** system that lets a procedural sky/atmosphere affect scene lighting directly, plus continued PBR material fixes, and further work on the experimental **Solari** real-time ray-traced renderer [(alternativeto.net, Bevy 0.18 release notes)](https://alternativeto.net/news/2026/1/bevy-0-18-brings-atmospheric-occlusion-pbr-shading-solari-improvements-and-font-variations). Solari is explicitly experimental/opt-in, not the default rendering path.
- Terrain: no first-party terrain renderer. Community crates like `bevy_generative` provide real-time procedural terrain/texture/planet mesh generation [(GitHub: bevy_generative)](https://github.com/manankarnik/bevy_generative), but there is no equivalent of Godot's clipmap terrain systems maintained at similar scale.
- Camera style: Bevy is camera-agnostic — it's a general 3D renderer with no built-in strategy-camera or third-person-controller convention, so either a Bannerlord-style ground camera or a Manor-Lords-style elevated camera is equally "from scratch" work.

### Godot 4

- Default desktop renderer is **Forward+**, Vulkan-based, supporting clustered forward rendering with thousands of dynamic lights, **SDFGI** (signed distance field global illumination) and **VoxelGI** for realistic indirect lighting in open-world scenes, and **volumetric fog** with a PBR scattering/absorption/emission model exposed as Density/Albedo/Emission [(Godot docs: Volumetric fog)](https://docs.godotengine.org/en/stable/tutorials/3d/volumetric_fog.html), [(Godot Forum/engine docs: renderers overview)](https://docs.godotengine.org/en/stable/tutorials/rendering/renderers.html).
- These are the concrete building blocks for exactly the target look: sun/directional light + SDFGI + volumetric fog + PBR materials is the standard recipe for "grounded outdoor atmosphere," and it ships as default engine functionality, not a bolt-on crate.
- Terrain: no built-in terrain system either (same gap as Bevy), but the third-party ecosystem is comparatively mature — **Terrain3D** (GPU-driven clipmap terrain, 64×64m up to 65.5×65.5km, up to 32 textures, foliage instancing) and **MTerrain** (GDExtension-based, C++, region-divided terrain for open-world games) are actively maintained, widely used plugins [(Terrain3D, Godot Asset Library)](https://godotengine.org/asset-library/asset/3892), [(Godot-MTerrain-plugin, GitHub)](https://github.com/mohsenph69/Godot-MTerrain-plugin).
- Community-made environment demos (e.g. "Outskirts," an 18th-century Dutch-town environment demo) are cited as evidence Godot 4's Forward+ pipeline can produce "realistic, atmospherically rich 3D environments" with "well-composed details that convey a sense of peaceful beauty" [(itch.io: Outskirts)](https://jedenjenda23.itch.io/outskirts-godot-environment-demo) — this is closer in spirit to the Bannerlord/Manor-Lords target than Bevy's currently-thinner terrain/atmosphere story, though it is still well short of AAA fidelity and is a hand-built showcase, not a guarantee of what a solo dev achieves without similar effort.
- Camera style: Godot's `Camera3D` node plus its long history of both third-person action games and top-down/RTS games (its 2D/3D node system doesn't privilege one camera style) means either ground-level or elevated strategic framing is equally well-trodden.

### Fyrox

- 3D-focused Rust engine, most distinctive for shipping a Unity/Godot-style visual editor (scene hierarchy, property inspector, asset browser, 3D viewport) unlike Bevy's code-only workflow [(GitHub: fyrox status via search summary)](https://lib.rs/crates/fyrox).
- No independent evidence found of Fyrox reaching Godot's SDFGI/volumetric-fog feature parity; it is generally characterized as behind both Bevy and Godot in ecosystem maturity and rendering feature breadth, and developed largely by a single maintainer rather than a large contributor base — a fidelity and momentum risk for a project targeting a specific atmospheric look. Included in this comparison for completeness (criterion asked to consider genuinely competitive alternatives), but not treated as a serious contender for rendering fidelity below.

**Fidelity verdict:** Godot ships more of the "grounded outdoor atmosphere" toolkit (SDFGI + volumetric fog + mature third-party terrain plugins) as ready-to-use, documented, default-renderer features. Bevy has the same underlying PBR foundation and is closing the gap (atmosphere system, light probes, experimental ray tracing), but as of Bevy 0.18 more of that stack is either manual/baked or experimental, and terrain tooling is thinner. Neither engine gets remotely close to Manor Lords/Bannerlord/Kingmakers (all built on commercial AAA engines with dedicated art teams) — that gap is expected and explicitly out of scope to close. Godot is the more direct path to "reads as real dirt/grass/stone, not flat cartoon tiles" without custom engine work; Bevy can get there but currently requires assembling more of the pipeline yourself.

---

## 2. UI overlay capability (inspector/HUD/event-log panel)

### Bevy

- Native `bevy_ui` exists but is a low-level, code-first UI system.
- The practical answer for an inspector/HUD-style panel is **`bevy_egui`**, an Egui integration for Bevy [(GitHub: bevy_egui)](https://github.com/vladbat00/bevy_egui), combined with **`bevy-inspector-egui`**, which "puts an egui-powered inspector panel into your Bevy app with a few lines of code," giving live entity/component/resource inspection and editing [(GitHub: bevy-inspector-egui)](https://github.com/jakobhellermann/bevy-inspector-egui). Both crates track Bevy's release cadence aggressively (0.36–0.37 already targeting Bevy 0.19 as of early 2026) [(docs.rs bevy-inspector-egui)](https://docs.rs/crate/bevy-inspector-egui/latest).
- Custom panels (sidebar, event log) are built the same way any egui app is built — `egui::Window`/`egui::SidePanel` docked against the render viewport via a `PrimaryEguiContext` marker component [(DeepWiki: Custom Layouts and Windows)](https://deepwiki.com/jakobhellermann/bevy-inspector-egui/6.2-custom-layouts-and-windows). This is a close cousin to the current PlayCanvas web client's sidebar in spirit (an immediate-mode debug/data panel), and is a well-worn path — egui is the de facto standard for exactly this kind of tool panel in the Rust game-dev ecosystem.

### Godot

- Godot has a first-party, mature `Control`-node UI system (themes, containers, anchors, signals) built into the engine and editor from day one — this is a strict superset of what an inspector/HUD/sidebar needs, and it's the same system Godot's own editor is built with.
- No extra dependency or crate is needed; this is arguably lower-friction than Bevy+egui because it's one system, officially maintained, with GDScript/C# and full designer tooling (drag-and-drop layout in the editor) rather than immediate-mode code panels.

**UI verdict:** both can build the required panel without fighting the engine. Godot's built-in `Control` system is more polished and has a visual editor for layout; Bevy's egui-based path is functionally equivalent for an inspector/debug-style HUD (which is what the PlayCanvas sidebar is) and is proven, actively maintained, and idiomatic in the Rust ecosystem, but is a third-party dependency, not first-party.

---

## 3. Asset pipeline maturity

### Godot

- **Native glTF 2.0 import**: dragging a `.glb`/`.gltf` file into the FileSystem dock imports it directly, no plugin required; `.glb` (self-contained, embedded PBR textures) is the recommended format [(search summary of Godot assets pipeline docs)](https://docs.godotengine.org/en/stable/tutorials/assets_pipeline/index.html).
- Free/CC0 asset availability: broad — Godot's own Asset Library plus general glTF/CC0 marketplaces (itch.io, Sketchfab, Kenney, Quaternius, Poly Haven) all target glTF/GLB directly, so grounded-medieval props/buildings/characters at reasonable fidelity are readily importable.
- Procedural generation without an artist: Terrain3D/MTerrain (above) cover terrain meshes; for placeholder buildings/units, `CSGShape3D` nodes (built-in constructive solid geometry) let a non-artist block out primitive building forms directly in the editor — a capability with no Bevy equivalent as a first-party feature.

### Bevy

- **glTF import**: `bevy_gltf` is a first-party plugin (`AssetLoader` + type defs) that loads glTF 2.0 files including meshes, materials, textures, sub-scenes, and animation clips [(Unofficial Bevy Cheat Book: GLTF)](https://bevy-cheatbook.github.io/3d/gltf.html), [(docs.rs bevy::gltf)](https://docs.rs/bevy/latest/bevy/gltf/index.html). This is functionally comparable to Godot's native import — both treat glTF as a first-class format.
- Free/CC0 assets: same external marketplaces apply (glTF is a universal format); no Bevy-specific asset-store disadvantage for source material.
- Procedural generation without an artist: `bevy_generative` (procedural maps/textures/terrain/planets, real-time) [(GitHub: bevy_generative)](https://github.com/manankarnik/bevy_generative) is a viable community-maintained substitute for Godot's Terrain3D/MTerrain, though smaller/less battle-tested than Godot's terrain ecosystem. There is no Bevy equivalent to Godot's built-in CSG primitive-blockout workflow — placeholder building geometry in Bevy means either scripting primitive mesh generation yourself or importing simple glTF blockouts made in Blender.

**Asset pipeline verdict:** roughly comparable for importing external glTF assets (both are first-class, both plug into the same CC0 ecosystem). Godot has a maturity edge for **in-engine, no-artist-needed placeholder content** — CSG primitives plus longer-established terrain plugins — versus Bevy's smaller, more code-centric procedural-generation crates.

---

## 4. Integration ease with UniverseCore's WebSocket/JSON protocol

Context: UniverseCore's boundary is `universe::ui::web_protocol` (C++23) — JSON-over-WebSocket, client commands `Hello`/`Pause`/`Resume`/`SetSpeed`/`SingleStep`/`RequestSnapshot`/`RequestEntityDetails`, and server messages for `hello`/`status`/`snapshot`/`entity_details`/`world_event`/`error` (confirmed directly from `include/universe/ui/web_protocol.hpp` and `src/ui/web_protocol.cpp` in this repo). This is intentionally engine-neutral — nothing about it assumes or rewards a C++ client; a same-language match to UniverseCore's C++ implementation does not imply tighter coupling is required or beneficial, since the transport is already an arm's-length WebSocket boundary either way.

### Bevy (Rust)

- `tokio-tungstenite` is the standard, actively maintained WebSocket client/server implementation for Rust's Tokio async runtime — "a lightweight stream-based WebSocket implementation," each socket exposing `Stream`/`Sink` for incoming/outgoing messages [(GitHub: snapview/tokio-tungstenite)](https://github.com/snapview/tokio-tungstenite), [(docs.rs tokio-tungstenite)](https://docs.rs/tokio-tungstenite).
- `bevy_websocket_adapter` (community crate) wraps this specifically for Bevy: "a simple adapter to receive WebSocket messages in your bevy games as native Rust types," using `serde_json` for JSON (de)serialization so incoming protocol messages map straight onto native Rust structs [(GitHub: bevy-websocket-adapter)](https://github.com/xide/bevy-websocket-adapter).
- Given `web_protocol.hpp`'s message shapes are plain JSON objects, mirroring them as `#[derive(Serialize, Deserialize)]` Rust structs (matching `ClientCommandType`, `WorldSnapshotView`, etc.) is direct, idiomatic, and gets compile-time type safety on both the outgoing command and incoming snapshot/event shapes — arguably a strength over a dynamically-typed client.

### Godot

- `WebSocketPeer` is a first-party, built-in class implementing RFC 6455 WebSocket client/server behavior, available identically from GDScript and C# [(Godot docs: WebSocketPeer, stable)](https://docs.godotengine.org/en/stable/classes/class_websocketpeer.html), [(Godot C# API docs)](https://straydragon.github.io/godot-csharp-api-doc/4.4-stable/main/Godot.WebSocketPeer.html).
- Usage pattern: `connect_to_url()`, then poll every frame; GDScript sends via `send()` for text frames (JSON), C# via `SendText()` — both explicitly recommended for "third-party text-based API (e.g. when using Json formatted messages)," which is exactly UniverseCore's protocol shape.
- JSON parsing: GDScript has a built-in `JSON` singleton; C# can use `System.Text.Json` or Newtonsoft — both trivial, well-documented paths.
- No native/C++ client is required or advantageous: Godot's GDExtension system lets you write C++ that plugs into the engine as native classes [(godot-cpp, GitHub)](https://github.com/godotengine/godot-cpp), and native WebSocket support is confirmed available for desktop GDExtension use, but this is strictly more complex than just using the built-in `WebSocketPeer` from GDScript/C# — there is no reason to reach for GDExtension/C++ here since the protocol boundary is already engine-neutral over the wire.

**Integration verdict:** both are straightforward — this criterion does not meaningfully favor either engine. Bevy's Rust+serde path gives static typing on the wire format at the cost of an extra community crate (`bevy_websocket_adapter`) beyond the engine itself; Godot's `WebSocketPeer` is zero-dependency and identical in GDScript or C#, at the cost of dynamic JSON parsing. Neither requires or benefits from being written in C++ to match UniverseCore's implementation language — the protocol is deliberately engine-neutral, and Godot's built-in client actually requires *less* integration glue than Bevy's community-crate approach.

---

## 5. Learning curve and community/tooling maturity (no prior game-engine background)

### Bevy

- Learning cost is explicitly "front-loaded, stacking the Rust curve and the ECS curve together" — programmers without prior ECS exposure "feel lost initially" because Bevy requires "a whole new paradigm of how to write and structure your game" [(Rustify: Rust for Game Development with Bevy, 2026)](https://rustify.rs/articles/rust-for-game-development-bevy-2026).
- Concretely: "if you're coming from GDScript or C#, expect four to six weeks of frustration before you're productive," with "2–3 weeks of adjustment time" specifically for the borrow checker interacting with ECS patterns; borrow-checker/ECS friction is reported as one of the top recurring questions in Bevy's Discord `#help` channel [(same source)](https://rustify.rs/articles/rust-for-game-development-bevy-2026).
- As of early 2026, **Bevy has no official visual editor** — it "remains a code-first engine... If you are shipping a commercial game with a team of non-programmers who need a visual editor, Bevy is not your tool today." The community `bevy_editor_prototypes` repo was archived April 2026, with editor work continuing informally [(Rin Oxide substack: Bevy's Fifth Birthday: The Editor)](https://rinoxide.substack.com/p/bevys-fifth-birthday-the-editor), [(GitHub: bevy_editor_prototypes)](https://github.com/bevyengine/bevy_editor_prototypes). For a solo developer, this means no drag-and-drop scene composition, no visual terrain/lighting tuning outside code — everything is written and iterated in Rust.
- Community: Bevy is "the most popular Rust game engine with 44,000+ GitHub stars, an active Discord, and regular releases" [(search summary, multiple sources)](https://aarambhdevhub.medium.com/rust-game-engines-in-2026-bevy-vs-macroquad-vs-ggez-vs-fyrox-which-one-should-you-actually-use-9bf93669e83f) — strong community relative to other Rust engines, but Bevy also requires learning Rust itself as a prerequisite if not already fluent, which is a real cost layered on top of engine/ECS learning even though UniverseCore itself is C++, not Rust.

### Godot

- Godot's learning curve is broadly considered the gentlest of the credible options here: GDScript is a Python-like scripting language designed specifically to be approachable for people without programming background; the editor provides visual scene composition, drag-and-drop node hierarchies, and a live-editable Inspector — the opposite of Bevy's code-only workflow.
- Community/tooling maturity: Godot is a long-established, large open-source project with extensive official documentation (`docs.godotengine.org`), a huge asset library, and years of accumulated tutorials/courses across both 2D and 3D use cases — the single largest and most beginner-oriented open-source game-engine community of the candidates considered.
- One Hacker News comment from a Bevy maintainer, cited in community discussion, states plainly that "Godot clearly has a significant [maturity edge]" over Bevy [(Hacker News)](https://news.ycombinator.com/item?id=35997948) — notable because it's a Bevy insider making the concession, not a competitor.
- For a developer with **no prior game-engine background** specifically (the stated condition here), Godot's built-in editor, gentler scripting language, and much larger volume of beginner-oriented tutorials represent meaningfully less friction than Bevy's Rust+ECS+code-only combination.

**Learning-curve verdict:** clear edge to Godot for a developer with no game-engine background. Bevy's ECS paradigm plus Rust's ownership model plus the current lack of any visual editor stack three real learning costs on top of each other; none of those costs disappear because UniverseCore's simulation core happens to be C++ — the client is a separate, engine-neutral consumer of a WebSocket API regardless of language choice.

---

## 6. Licensing

All engines carried into the detailed comparison are genuinely open source:

- **Bevy** — dual **MIT / Apache-2.0**, at the user's option [(GitHub: bevyengine/bevy)](https://github.com/bevyengine/bevy), [(Bevy relicensing issue #2373)](https://github.com/bevyengine/bevy/issues/2373).
- **Godot** — **MIT** license, unrestricted, no royalties, no revenue thresholds.
- **Fyrox** — **MIT** license per its package listing [(lib.rs: fyrox)](https://lib.rs/crates/fyrox).
- **O3DE** (mentioned for completeness, not scored above) — **Apache 2.0 / MIT** [(GitHub: o3de/o3de)](https://github.com/o3de/o3de) — also passes the licensing bar, but excluded on practicality grounds (criteria 3 and 5), not licensing.
- **Flax** — excluded specifically because it fails this criterion: source-available with mandatory commercial royalties above a revenue threshold, not unrestricted open source [(Phoronix)](https://www.phoronix.com/news/Flax-Engine-1.1.6217).

No licensing differentiation between Bevy and Godot — both are unencumbered, permissive, and safe for any downstream use.

---

## Recommendation

**Godot 4 is the more defensible default choice for this specific project, but the margin is not overwhelming, and there is a legitimate case for Bevy if the team is willing to accept more up-front friction for a better long-term integration story.**

Reasoning, criterion by criterion:

- **Fidelity (1):** Godot edge. SDFGI + volumetric fog + mature terrain plugins (Terrain3D/MTerrain) are ready-made, documented, default-renderer tools for exactly the "grounded outdoor atmosphere" the Bannerlord/Manor-Lords/Kingmakers references represent. Bevy has the same PBR foundation and is actively closing the gap (0.18's atmosphere system, light probes, experimental Solari ray tracing), but as of today more of that stack is manual, baked, or experimental rather than turnkey. **Neither will get remotely close to the AAA references** — that's expected and explicitly not the bar.
- **UI overlay (2):** Roughly even. Godot's native `Control` system is first-party and editor-integrated; Bevy+egui (`bevy_egui` + `bevy-inspector-egui`) is a proven, actively-maintained third-party stack that does the same job for an inspector/HUD/event-log panel.
- **Asset pipeline (3):** Slight Godot edge, mainly for no-artist placeholder content (built-in CSG primitives, more mature terrain tooling). Both import glTF/CC0 assets equally well as first-class citizens.
- **Integration (4):** A wash. Both consume UniverseCore's engine-neutral WebSocket/JSON protocol cleanly — Godot's built-in `WebSocketPeer` needs zero extra dependencies from GDScript or C#; Bevy needs one extra community crate (`bevy_websocket_adapter`) but gets static Rust typing on the wire format in return. UniverseCore being C++ does not favor either engine — the protocol boundary was designed to be crossed by any language.
- **Learning curve (5):** Clear Godot edge for a developer with no prior game-engine background — visual editor, gentler scripting language, larger and more beginner-oriented documentation/tutorial base, versus Bevy's code-only workflow stacking Rust's ownership model on top of the ECS paradigm (realistically 4–6 weeks of friction per community sources, even for a developer already comfortable in C++).
- **Licensing (6):** Tied — both fully permissive open source.

**Where this is genuinely close, and should not be overstated as a landslide:** if the priority is a Rust-native ecosystem, static typing on the protocol boundary, or a bet on Bevy's clearly upward trajectory (atmosphere/lighting work shipping steadily through 2026, a large and growing community, egui giving a perfectly workable UI panel), Bevy is a reasonable, defensible choice — just not the lower-friction one today. The fidelity gap between the two engines, while real, is smaller than the learning-curve gap; a developer who invests the weeks to get comfortable with Bevy's ECS is not stuck with a visibly worse-looking game, just a harder-won one, and Godot's terrain-plugin/GI advantage is a maturity gap that could plausibly narrow within Bevy's release cadence.

**Bottom line:** for a developer starting with no game-engine background, wanting to get to a grounded-medieval-looking client with the least fighting-the-tool overhead, **Godot 4** is the recommended starting point. It is not a "correct answer with no downside" — it is the lower-risk, faster-to-atmosphere option given the stated constraints, not a categorical win over Bevy on every axis.
