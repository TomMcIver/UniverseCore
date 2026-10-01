#include "universe/simulation.hpp"
#include "universe/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace {

constexpr std::int64_t kMinBuildingSeparationUnits = 6;
constexpr std::int64_t kMaxBuildingDistanceFromCentreUnits = 160;
constexpr std::int64_t kMaxCommonsDistanceFromCentreUnits = 20;
constexpr std::size_t kSnapshotHeaderSize = 24U;
constexpr std::size_t kAggregateTotalsTailSize = 16U;
constexpr std::size_t kPlacementRecordSize = 26U;

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

universe::SimulationConfig demo_config(const std::uint64_t seed) {
    return universe::SimulationConfig{
        .seed = seed,
        .initial_population = 200,
        .enable_unrelated_random_probe = false,
        .initial_household_food_units = 80,
        .food_production_scale_percent = 100,
        .food_system_enabled = true,
    };
}

const universe::Settlement* find_settlement(const universe::WorldState& state, const universe::SettlementId id) {
    for (const auto& settlement : state.settlements) {
        if (settlement.id == id) {
            return &settlement;
        }
    }
    return nullptr;
}

const universe::Person* find_person(const universe::WorldState& state, const universe::PersonId id) {
    for (const auto& person : state.people) {
        if (person.id == id) {
            return &person;
        }
    }
    return nullptr;
}

std::int64_t squared_distance(const std::int64_t ax, const std::int64_t az, const std::int64_t bx, const std::int64_t bz) {
    const auto dx = ax - bx;
    const auto dz = az - bz;
    return (dx * dx) + (dz * dz);
}

void write_u16_le(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

void write_u64_le(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint64_t value) {
    for (std::size_t i = 0; i < 8U; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
}

constexpr std::size_t kViabilityRecordSize = 16U;

// Rewrites a v26 snapshot as v25 by removing the v26-only section (settlement
// viability counters) that sits immediately before the 16-byte aggregate
// demography tail.
std::vector<std::byte> downgrade_to_v25(const std::vector<std::byte>& v26_bytes, const universe::WorldState& state) {
    const auto section_size = 8U + (state.settlements.size() * kViabilityRecordSize);
    const auto tail_start = v26_bytes.size() - kAggregateTotalsTailSize;
    const auto section_start = tail_start - section_size;

    std::vector<std::byte> v25_bytes;
    v25_bytes.reserve(v26_bytes.size() - section_size);
    v25_bytes.insert(v25_bytes.end(), v26_bytes.begin(), v26_bytes.begin() + static_cast<std::ptrdiff_t>(section_start));
    v25_bytes.insert(v25_bytes.end(), v26_bytes.begin() + static_cast<std::ptrdiff_t>(tail_start), v26_bytes.end());

    write_u16_le(v25_bytes, 4U, 25U);
    const auto payload_size = v25_bytes.size() - kSnapshotHeaderSize;
    write_u64_le(v25_bytes, 8U, static_cast<std::uint64_t>(payload_size));
    const std::span<const std::byte> payload{v25_bytes.data() + kSnapshotHeaderSize, payload_size};
    write_u64_le(v25_bytes, 16U, universe::snapshot_hash64(payload));
    return v25_bytes;
}

// Rewrites a v25 snapshot as v24 by removing the v25-only section that sits
// immediately before the 16-byte aggregate demography tail.
std::vector<std::byte> downgrade_to_v24(const std::vector<std::byte>& v25_bytes, const universe::WorldState& state) {
    std::uint64_t resync_count = 0;
    for (const auto& person : state.people) {
        if (person.occupation_history_resync_pending) {
            ++resync_count;
        }
    }
    const auto section_size = 8U + (state.buildings.size() * kPlacementRecordSize) + 8U + (resync_count * 8U);
    const auto tail_start = v25_bytes.size() - kAggregateTotalsTailSize;
    const auto section_start = tail_start - section_size;

    std::vector<std::byte> v24_bytes;
    v24_bytes.reserve(v25_bytes.size() - section_size);
    v24_bytes.insert(v24_bytes.end(), v25_bytes.begin(), v25_bytes.begin() + static_cast<std::ptrdiff_t>(section_start));
    v24_bytes.insert(v24_bytes.end(), v25_bytes.begin() + static_cast<std::ptrdiff_t>(tail_start), v25_bytes.end());

    write_u16_le(v24_bytes, 4U, 24U);
    const auto payload_size = v24_bytes.size() - kSnapshotHeaderSize;
    write_u64_le(v24_bytes, 8U, static_cast<std::uint64_t>(payload_size));
    const std::span<const std::byte> payload{v24_bytes.data() + kSnapshotHeaderSize, payload_size};
    write_u64_le(v24_bytes, 16U, universe::snapshot_hash64(payload));
    return v24_bytes;
}

void require_same_building_placements(const universe::WorldState& lhs, const universe::WorldState& rhs, const char* context) {
    require(lhs.buildings.size() == rhs.buildings.size(), context);
    for (std::size_t i = 0; i < lhs.buildings.size(); ++i) {
        require(lhs.buildings[i].id == rhs.buildings[i].id, context);
        require(lhs.buildings[i].position_x == rhs.buildings[i].position_x, context);
        require(lhs.buildings[i].position_z == rhs.buildings[i].position_z, context);
        require(lhs.buildings[i].facing_degrees == rhs.buildings[i].facing_degrees, context);
    }
}

void test_occupation_changes_are_recorded_only_on_real_changes() {
    universe::Simulation sim{demo_config(42)};
    require(sim.state().event_log.empty(), "initial occupation assignment must not record life events");

    sim.run_years(20);

    std::uint64_t occupation_changes = 0;
    for (const auto& event : sim.state().event_log) {
        if (event.type != universe::EventType::OccupationChanged) {
            continue;
        }
        ++occupation_changes;
        require(std::holds_alternative<universe::OccupationChangedEventPayload>(event.payload), "occupation changed payload must match type");
        const auto& payload = std::get<universe::OccupationChangedEventPayload>(event.payload);
        require(payload.from != payload.to, "occupation changed event must record a real change");
        require(find_person(sim.state(), payload.person_id) != nullptr, "occupation changed event must reference an existing person");
        require(event.timestamp.ticks > 0, "occupation changes are never recorded for initial assignment");
    }
    require(occupation_changes > 0U, "twenty simulated years must record at least one occupation change");
}

void test_household_relocations_accompany_settlement_founding() {
    const std::uint64_t seeds[] = {42U, 7U, 11U, 19U};
    bool saw_founding = false;

    for (const auto seed : seeds) {
        universe::Simulation sim{demo_config(seed)};
        sim.run_years(30);
        const auto& state = sim.state();

        std::unordered_map<universe::SettlementId, std::uint64_t> relocations_into;
        for (const auto& event : state.event_log) {
            if (event.type != universe::EventType::HouseholdRelocated) {
                continue;
            }
            require(std::holds_alternative<universe::HouseholdRelocatedEventPayload>(event.payload), "relocation payload must match type");
            const auto& payload = std::get<universe::HouseholdRelocatedEventPayload>(event.payload);
            require(payload.from_settlement_id != payload.to_settlement_id, "relocation must change settlement");
            require(find_settlement(state, payload.from_settlement_id) != nullptr, "relocation source settlement must exist");
            require(find_settlement(state, payload.to_settlement_id) != nullptr, "relocation destination settlement must exist");
            ++relocations_into[payload.to_settlement_id];
        }

        for (const auto& event : state.event_log) {
            if (event.type != universe::EventType::SettlementFounded) {
                continue;
            }
            const auto& payload = std::get<universe::SettlementFoundedEventPayload>(event.payload);
            if (!payload.source_settlement_id.has_value()) {
                continue;
            }
            saw_founding = true;
            require(relocations_into[payload.settlement_id] >= 2U, "a founded settlement must record the households that moved into it");
        }

        if (saw_founding) {
            break;
        }
    }

    require(saw_founding, "at least one test seed must found a settlement within thirty years");
}

void test_population_group_merging_never_records_life_events() {
    universe::Simulation sim{demo_config(42)};
    sim.run_years(5);

    std::vector<universe::RegionId> region_ids;
    for (const auto& region : sim.state().regions) {
        region_ids.push_back(region.id);
    }

    std::unordered_set<universe::PersonId> alive_before;
    for (const auto& person : sim.state().people) {
        if (person.alive) {
            alive_before.insert(person.id);
        }
    }

    const auto events_before = sim.state().event_log.size();
    for (const auto region_id : region_ids) {
        sim.demote_region_to_settlement_aggregate(region_id);
    }
    require(sim.state().event_log.size() == events_before, "demotion into population groups must not record events");

    std::unordered_set<universe::PersonId> resident_through_demotion;
    for (const auto& person : sim.state().people) {
        if (person.alive) {
            resident_through_demotion.insert(person.id);
        }
    }

    for (const auto region_id : region_ids) {
        sim.promote_region_to_detailed(region_id);
    }
    require(sim.state().event_log.size() == events_before, "promotion out of population groups must not record events");

    std::unordered_set<universe::PersonId> resolved_by_promotion;
    for (const auto& person : sim.state().people) {
        if (person.alive && !resident_through_demotion.contains(person.id)) {
            require(person.occupation_history_resync_pending, "every person resolved by promotion must be marked for occupation resync");
            resolved_by_promotion.insert(person.id);
        }
    }
    require(!resolved_by_promotion.empty(), "promotion must resolve people");
    (void)alive_before;

    const auto resolution_time = sim.state().time;
    sim.run_years(1);

    for (std::size_t i = events_before; i < sim.state().event_log.size(); ++i) {
        const auto& event = sim.state().event_log[i];
        if (event.type == universe::EventType::OccupationChanged) {
            const auto& payload = std::get<universe::OccupationChangedEventPayload>(event.payload);
            require(
                !resolved_by_promotion.contains(payload.person_id),
                "the first reassignment after promotion must not record an occupation change");
        }
        if (event.type == universe::EventType::Death) {
            const auto& payload = std::get<universe::DeathEventPayload>(event.payload);
            const auto* person = find_person(sim.state(), payload.person_id);
            require(person != nullptr && !person->alive, "a death event must only exist for a person who is dead");
            require(event.timestamp > resolution_time, "no death event may be dated at the resolution transition");
        }
    }

    for (const auto& person : sim.state().people) {
        require(!person.occupation_history_resync_pending || !person.alive, "the resync mark clears after one reassignment");
    }
}

void test_building_placement_is_canonical_spaced_and_fixed() {
    universe::Simulation sim{demo_config(42)};
    sim.run_years(10);
    const auto early_buildings = sim.state().buildings;
    require(!early_buildings.empty(), "ten simulated years must plan buildings");

    sim.run_years(10);
    const auto& state = sim.state();

    for (const auto& early : early_buildings) {
        for (const auto& later : state.buildings) {
            if (later.id == early.id) {
                require(later.position_x == early.position_x && later.position_z == early.position_z, "building placement must never move");
                require(later.facing_degrees == early.facing_degrees, "building facing must never change");
            }
        }
    }

    for (std::size_t i = 0; i < state.buildings.size(); ++i) {
        const auto& building = state.buildings[i];
        require(building.facing_degrees < 360U, "building facing must be a whole degree below 360");

        const auto* settlement = find_settlement(state, building.settlement_id);
        require(settlement != nullptr, "building settlement must exist");
        const auto centre_distance_sq = squared_distance(building.position_x, building.position_z, settlement->position_x, settlement->position_z);
        require(
            centre_distance_sq <= kMaxBuildingDistanceFromCentreUnits * kMaxBuildingDistanceFromCentreUnits,
            "building must sit near its settlement");

        const bool is_commons_service = building.settlement_commons_owned && building.type != universe::BuildingType::Dwelling;
        if (is_commons_service) {
            require(
                centre_distance_sq <= kMaxCommonsDistanceFromCentreUnits * kMaxCommonsDistanceFromCentreUnits,
                "shared stores and workshops must sit by the common");
        } else {
            require(centre_distance_sq > 0, "dwellings must not sit on the common itself");
        }

        for (std::size_t j = i + 1U; j < state.buildings.size(); ++j) {
            const auto& other = state.buildings[j];
            const auto* other_settlement = find_settlement(state, other.settlement_id);
            if (other_settlement == nullptr || other_settlement->region_id != settlement->region_id) {
                continue;
            }
            require(
                squared_distance(building.position_x, building.position_z, other.position_x, other.position_z) >=
                    kMinBuildingSeparationUnits * kMinBuildingSeparationUnits,
                "buildings in a region must never overlap");
        }
    }

    universe::Simulation twin{demo_config(42)};
    twin.run_years(20);
    require_same_building_placements(state, twin.state(), "same seed must reproduce identical building placements");

    auto replay = sim;
    replay.recompute_building_placements();
    require_same_building_placements(state, replay.state(), "replaying placement in id order must reproduce planning-time placement");
}

void test_v25_snapshot_continuation_preserves_life_events_and_placement() {
    universe::Simulation reference{demo_config(42)};
    reference.run_years(6);
    std::vector<universe::RegionId> region_ids;
    for (const auto& region : reference.state().regions) {
        region_ids.push_back(region.id);
    }
    for (const auto region_id : region_ids) {
        reference.demote_region_to_settlement_aggregate(region_id);
        reference.promote_region_to_detailed(region_id);
    }

    const auto saved = reference.save_snapshot();
    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    const auto load_result = universe::Simulation::load_snapshot(saved, restored);
    if (!load_result.ok) {
        std::cerr << "load failed: " << load_result.message << '\n';
    }
    require(load_result.ok, "v25 snapshot must load");
    require(restored.save_snapshot() == saved, "v25 snapshot with resync marks must round-trip byte for byte");

    // Continuation is checked on a world that has not been through a
    // demote/promote cycle: base schema v24 already diverges after such a
    // cycle, independent of life events and placement.
    universe::Simulation uninterrupted{demo_config(42)};
    uninterrupted.run_years(12);
    universe::Simulation continued_source{demo_config(42)};
    continued_source.run_years(6);
    universe::Simulation continued{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(continued_source.save_snapshot(), continued).ok, "v25 snapshot must load for continuation");
    continued.run_years(6);
    require(uninterrupted.save_snapshot() == continued.save_snapshot(), "save/load continuation must match uninterrupted simulation");

    bool saw_life_event = false;
    for (const auto& event : continued.state().event_log) {
        saw_life_event = saw_life_event || event.type == universe::EventType::OccupationChanged;
    }
    require(saw_life_event, "continuation coverage must include recorded occupation changes");
}

void test_equivalent_time_advance_schedules_match() {
    const auto year_ticks = universe::DemographicCalendarAdapter::year_duration().ticks;
    const auto total_ticks = year_ticks * 12;

    universe::Simulation yearly{demo_config(42)};
    yearly.run_years(12);

    universe::Simulation uneven{demo_config(42)};
    const std::int64_t chunks[] = {universe::days(1).ticks, universe::days(37).ticks, year_ticks * 3, universe::hours(5).ticks};
    std::int64_t advanced = 0;
    for (const auto chunk : chunks) {
        uneven.advance(universe::SimulationDuration{chunk});
        advanced += chunk;
    }
    uneven.advance(universe::SimulationDuration{total_ticks - advanced});

    require(yearly.save_snapshot() == uneven.save_snapshot(), "equivalent time-advance schedules must produce identical canonical state");
}

void test_v24_snapshots_migrate() {
    universe::Simulation fresh{demo_config(42)};
    require(fresh.state().buildings.empty() && fresh.state().event_log.empty(), "a fresh world has no buildings or events");
    const auto v24_fresh = downgrade_to_v24(downgrade_to_v25(fresh.save_snapshot(), fresh.state()), fresh.state());

    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    const auto fresh_result = universe::Simulation::load_snapshot(v24_fresh, restored);
    if (!fresh_result.ok) {
        std::cerr << "v24 load failed: " << fresh_result.message << '\n';
    }
    require(fresh_result.ok, "a v24 snapshot must load under schema v25");
    for (const auto& person : restored.state().people) {
        require(
            person.occupation_history_resync_pending == person.alive,
            "migration must treat every living person's next reassignment as a resolution");
    }
    require_same_building_placements(fresh.state(), restored.state(), "migration must reproduce building placement");

    universe::Simulation resaved{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(restored.save_snapshot(), resaved).ok, "a migrated snapshot must re-save as valid v25");

    universe::Simulation aged{demo_config(42)};
    aged.run_years(10);
    const auto v24_with_life_events = downgrade_to_v24(downgrade_to_v25(aged.save_snapshot(), aged.state()), aged.state());
    universe::Simulation rejected{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    const auto aged_result = universe::Simulation::load_snapshot(v24_with_life_events, rejected);
    require(!aged_result.ok, "v24 snapshots cannot contain v25 life event types");
    require(aged_result.code == universe::SnapshotErrorCode::InvalidEnum, "v25 event types in a v24 snapshot are an invalid enum");
}

constexpr std::int64_t kCounterCheckDays = 10;

// Fewer people than the viable size, so the whole world is below it.
universe::SimulationConfig tiny_config(const std::uint64_t seed) {
    auto config = demo_config(seed);
    config.initial_population = 12;
    return config;
}

void require_counters_match_population(const universe::Simulation& sim, const std::uint32_t expected_days, const char* message) {
    bool saw_below = false;
    bool saw_viable = false;
    for (const auto& settlement : sim.state().settlements) {
        if (!settlement.active) {
            require(settlement.days_below_viable_size == 0U, "an inactive settlement never accrues viability days");
            continue;
        }
        if (settlement.population < universe::kViableSettlementSize) {
            saw_below = true;
            require(settlement.days_below_viable_size == expected_days, message);
        } else {
            saw_viable = true;
            require(settlement.days_below_viable_size == 0U, "a viable settlement must have no days below viable size");
        }
    }
    require(saw_below || saw_viable, "world must have an active settlement to check");
}

void test_days_below_viable_size_counts_one_per_day() {
    universe::Simulation sim{tiny_config(42)};
    for (const auto& settlement : sim.state().settlements) {
        require(settlement.days_below_viable_size == 0U, "a fresh world has not accrued any days below viable size");
    }

    sim.advance(universe::days(kCounterCheckDays));

    bool saw_below_viable = false;
    for (const auto& settlement : sim.state().settlements) {
        saw_below_viable = saw_below_viable || (settlement.active && settlement.population < universe::kViableSettlementSize);
    }
    require(saw_below_viable, "a 12-person world must hold a settlement below viable size");
    require_counters_match_population(sim, static_cast<std::uint32_t>(kCounterCheckDays), "one day below viable size must add exactly one to the counter");
}

void test_viable_settlements_keep_a_zero_counter() {
    universe::Simulation sim{demo_config(42)};
    sim.advance(universe::days(kCounterCheckDays));
    bool saw_viable = false;
    for (const auto& settlement : sim.state().settlements) {
        saw_viable = saw_viable || (settlement.active && settlement.population >= universe::kViableSettlementSize);
    }
    require(saw_viable, "the default 200-person world must hold a viable settlement");
    require_counters_match_population(sim, static_cast<std::uint32_t>(kCounterCheckDays), "below-viable days must match elapsed days");
}

void test_v26_viability_counters_survive_save_load_continuation() {
    universe::Simulation uninterrupted{tiny_config(42)};
    uninterrupted.advance(universe::days(kCounterCheckDays));

    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(uninterrupted.save_snapshot(), restored).ok, "v26 snapshot must load");
    require_counters_match_population(restored, static_cast<std::uint32_t>(kCounterCheckDays), "counters must survive a snapshot round trip");

    uninterrupted.advance(universe::days(5));
    restored.advance(universe::days(5));
    require_counters_match_population(restored, static_cast<std::uint32_t>(kCounterCheckDays + 5), "counters keep counting after a reload");
    require(uninterrupted.save_snapshot() == restored.save_snapshot(), "save/load continuation must match the uninterrupted run");
}

void test_food_strain_days_survive_save_load_and_migrate_to_zero() {
    universe::Simulation source{demo_config(42)};
    source.advance(universe::days(60));
    bool saw_strain_days = false;
    for (const auto& settlement : source.state().settlements) {
        saw_strain_days = saw_strain_days || settlement.days_under_food_strain > 0U;
    }
    require(saw_strain_days, "the source world must hold a non-zero strain counter for these checks to prove anything");

    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(source.save_snapshot(), restored).ok, "v26 snapshot with strain days must load");
    require(restored.state().settlements.size() == source.state().settlements.size(), "settlement count must round-trip");
    for (std::size_t i = 0; i < source.state().settlements.size(); ++i) {
        require(
            restored.state().settlements[i].days_under_food_strain == source.state().settlements[i].days_under_food_strain,
            "strain days must survive a snapshot round trip");
    }

    const auto v25 = downgrade_to_v25(source.save_snapshot(), source.state());
    universe::Simulation migrated{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(v25, migrated).ok, "a v25 snapshot must load under schema v26");
    for (const auto& settlement : migrated.state().settlements) {
        require(settlement.days_under_food_strain == 0U, "migration must start every settlement's strain days at zero");
    }
}

void test_snapshot_taken_at_a_year_boundary_round_trips_with_inactive_settlements() {
    // Merges and extinctions happen in the annual step, after the last daily
    // viability update, so a snapshot taken right there can hold an inactive
    // settlement that still carries a count.
    universe::Simulation sim{demo_config(42)};
    sim.run_years(5);
    bool saw_inactive_with_count = false;
    for (const auto& settlement : sim.state().settlements) {
        saw_inactive_with_count = saw_inactive_with_count || (!settlement.active && settlement.days_below_viable_size > 0U);
    }
    require(saw_inactive_with_count, "the year-boundary world must hold an inactive settlement with a stale count");

    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    const auto result = universe::Simulation::load_snapshot(sim.save_snapshot(), restored);
    require(result.ok, "a year-boundary snapshot with an inactive settlement count must load");
    require(restored.save_snapshot() == sim.save_snapshot(), "a year-boundary snapshot must round-trip byte for byte");
}

void test_v25_snapshots_migrate_with_zeroed_viability_counters() {
    universe::Simulation aged{tiny_config(42)};
    aged.advance(universe::days(kCounterCheckDays));
    bool had_nonzero = false;
    for (const auto& settlement : aged.state().settlements) {
        had_nonzero = had_nonzero || settlement.days_below_viable_size > 0U;
    }
    require(had_nonzero, "the source world must hold a non-zero counter for the migration to prove anything");

    const auto v25 = downgrade_to_v25(aged.save_snapshot(), aged.state());
    universe::Simulation restored{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    const auto result = universe::Simulation::load_snapshot(v25, restored);
    if (!result.ok) {
        std::cerr << "v25 load failed: " << result.message << '\n';
    }
    require(result.ok, "a v25 snapshot must load under schema v26");
    for (const auto& settlement : restored.state().settlements) {
        require(settlement.days_below_viable_size == 0U, "migration must start every settlement's counter at zero");
    }

    universe::Simulation resaved{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    require(universe::Simulation::load_snapshot(restored.save_snapshot(), resaved).ok, "a migrated snapshot must re-save as valid v26");
    require(resaved.save_snapshot() == restored.save_snapshot(), "a migrated snapshot must round-trip byte for byte");
}

}  // namespace

int main() {
    test_occupation_changes_are_recorded_only_on_real_changes();
    test_household_relocations_accompany_settlement_founding();
    test_population_group_merging_never_records_life_events();
    test_building_placement_is_canonical_spaced_and_fixed();
    test_v25_snapshot_continuation_preserves_life_events_and_placement();
    test_equivalent_time_advance_schedules_match();
    test_v24_snapshots_migrate();
    test_days_below_viable_size_counts_one_per_day();
    test_viable_settlements_keep_a_zero_counter();
    test_v26_viability_counters_survive_save_load_continuation();
    test_food_strain_days_survive_save_load_and_migrate_to_zero();
    test_snapshot_taken_at_a_year_boundary_round_trips_with_inactive_settlements();
    test_v25_snapshots_migrate_with_zeroed_viability_counters();
    std::cout << "life event and building placement tests passed\n";
    return 0;
}
