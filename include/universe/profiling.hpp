#pragma once

// Optional profiling zones (Tracy). Compiled out entirely unless the build sets
// UNIVERSECORE_TRACY (CMake: -DUNIVERSECORE_ENABLE_TRACY=ON).
//
// Rules, so profiling can never change what the simulation does:
//   * A zone only observes. It never reads or writes canonical state, never
//     touches a random stream and never alters control flow.
//   * Zones are not part of the snapshot and are not canonical.
//   * With the macro undefined every UC_* macro expands to nothing, so the
//     canonical Podman build produces the same code as before.
//
// Usage:
//   void Simulation::process_trade_period() {
//       UC_ZONE("process_trade_period");
//       ...
//   }

#if defined(UNIVERSECORE_TRACY)
#include <tracy/Tracy.hpp>

// A named, scoped zone. The name must be a string literal.
#define UC_ZONE(name) ZoneScopedN(name)
// Marks the end of one logical frame (here: one simulated day boundary).
#define UC_FRAME_MARK() FrameMark
// Plots a numeric value on the Tracy timeline (population, trips in flight...).
#define UC_PLOT(name, value) TracyPlot(name, static_cast<int64_t>(value))
// Attaches text to the enclosing zone (seed, settlement id...).
#define UC_ZONE_TEXT(text, size) ZoneText(text, size)
#else
#define UC_ZONE(name) static_cast<void>(0)
#define UC_FRAME_MARK() static_cast<void>(0)
#define UC_PLOT(name, value) static_cast<void>(0)
#define UC_ZONE_TEXT(text, size) static_cast<void>(0)
#endif
