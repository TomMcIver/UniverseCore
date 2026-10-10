#include "universe/simulation.hpp"

#include "universe/snapshot.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace universe {

namespace {
constexpr std::uint32_t kAdultAgeYears = 18;
constexpr std::int32_t kOccupationMinimumNutritionPoints = 160;
constexpr std::int32_t kOccupationMinimumHealthPoints = 160;
constexpr std::int64_t kMinRegionAreaUnits = 10;
constexpr std::int64_t kMaxRegionAreaUnits = 10'000;

// Canonical geography constants (ticket #24/#25): position is derived,
// deterministic (x, z) in fixed-width integer "world units", not floating
// point, per the project's ban on floats in canonical state. Scoped to a
// single settled area on one world (per UniverseCore issue #23's resolution),
// not a multi-planet coordinate hierarchy.
constexpr std::int64_t kWorldHalfExtentUnits = 4'000;
constexpr std::int64_t kMinRegionDistanceUnits = 220;
constexpr std::int64_t kMinSettlementDistanceUnits = 24;
constexpr std::uint32_t kPositionPlacementMaxAttempts = 48;
constexpr std::uint32_t kPositionPlacementWidenEvery = 8;

struct Point2D {
    std::int64_t x{};
    std::int64_t z{};
};

[[nodiscard]] std::int64_t squared_distance(const Point2D lhs, const Point2D rhs) noexcept {
    const auto dx = lhs.x - rhs.x;
    const auto dz = lhs.z - rhs.z;
    return (dx * dx) + (dz * dz);
}

// Deterministic min-distance rejection sampling: try candidates drawn from
// derive_seed(seed, domain, attempt), widening the search box every
// kPositionPlacementWidenEvery attempts, accepting the first candidate at
// least min_distance from every already-placed point. Falls back to a fixed
// octant lattice (still deterministic, still separated) if every attempt is
// rejected, mirroring the presentation layer's golden-angle-spiral fallback.
[[nodiscard]] Point2D pick_spaced_position(
    const SimulationSeed seed,
    const SeedDomain domain,
    const std::int64_t center_x,
    const std::int64_t center_z,
    const std::int64_t initial_radius,
    const std::int64_t min_distance,
    const std::vector<Point2D>& placed) noexcept {
    auto radius = std::max<std::int64_t>(initial_radius, min_distance);
    for (std::uint32_t attempt = 0; attempt < kPositionPlacementMaxAttempts; ++attempt) {
        const auto span = static_cast<std::uint64_t>((radius * 2) + 1);
        const auto x_seed = derive_seed(seed, domain, static_cast<std::uint64_t>(attempt) << 1U);
        const auto z_seed = derive_seed(seed, domain, (static_cast<std::uint64_t>(attempt) << 1U) | 1U);
        const auto offset_x = static_cast<std::int64_t>(seed_random_bounded(x_seed, span)) - radius;
        const auto offset_z = static_cast<std::int64_t>(seed_random_bounded(z_seed, span)) - radius;
        const Point2D candidate{center_x + offset_x, center_z + offset_z};

        bool far_enough = true;
        for (const auto& other : placed) {
            if (squared_distance(candidate, other) < (min_distance * min_distance)) {
                far_enough = false;
                break;
            }
        }
        if (far_enough) {
            return candidate;
        }
        if (attempt % kPositionPlacementWidenEvery == (kPositionPlacementWidenEvery - 1U)) {
            radius += min_distance;
        }
    }

    static constexpr std::array<Point2D, 8> kOctantDirections{{
        {1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1},
    }};
    const auto index = static_cast<std::int64_t>(placed.size());
    const auto ring = (index / 8) + 1;
    const auto slot = static_cast<std::size_t>(index % 8);
    const auto fallback_radius = min_distance * ring;
    const auto direction = kOctantDirections[slot];
    return Point2D{center_x + (direction.x * fallback_radius), center_z + (direction.z * fallback_radius)};
}

[[nodiscard]] std::int64_t settlement_initial_placement_radius(const RegionEnvironmentState& region) noexcept {
    return std::max<std::int64_t>(kMinSettlementDistanceUnits, region.land_area_units / 4);
}

[[nodiscard]] Point2D place_settlement_point(
    const RegionEnvironmentState& region, const SimulationSeed settlement_seed_value, const std::vector<Point2D>& placed) {
    return pick_spaced_position(
        settlement_seed_value, SeedDomain::SettlementPosition, region.position_x, region.position_z,
        settlement_initial_placement_radius(region), kMinSettlementDistanceUnits, placed);
}

// Ticket #35/#38: a household's offset from its settlement's canonical
// position, derived once at founding. Simple seeded jitter, not the
// min-distance rejection sampling regions/settlements use -- households are
// far denser than settlements, and overlapping household offsets is not a
// correctness bug the way overlapping settlements was (no sub-settlement
// geography/Building position exists yet to distinguish one household's
// point from another's in the first place).
constexpr std::int32_t kHouseholdOffsetMaxUnits = 10;

void assign_household_offset(Household& household, const SimulationSeed household_seed_value) noexcept {
    const auto span = static_cast<std::uint64_t>((kHouseholdOffsetMaxUnits * 2) + 1);
    const auto x_seed = derive_seed(household_seed_value, SeedDomain::HouseholdPosition, 0U);
    const auto z_seed = derive_seed(household_seed_value, SeedDomain::HouseholdPosition, 1U);
    household.settlement_offset_x =
        static_cast<std::int32_t>(seed_random_bounded(x_seed, span)) - kHouseholdOffsetMaxUnits;
    household.settlement_offset_z =
        static_cast<std::int32_t>(seed_random_bounded(z_seed, span)) - kHouseholdOffsetMaxUnits;
}

// Canonical building placement (ticket #46): household yards strung along a
// deterministic track network growing out of the settlement's common, with
// shared stores and workshops by the common. Integer geometry only -- a
// 10-degree direction table scaled by kDirectionScale -- so placement never
// depends on platform floating point.
constexpr std::int64_t kDirectionScale = 1024;
constexpr std::size_t kDirectionCount = 36;
constexpr std::array<Point2D, kDirectionCount> kDirections{{
    {1024, 0}, {1008, 178}, {962, 350}, {887, 512}, {784, 658}, {658, 784}, {512, 887}, {350, 962}, {178, 1008},
    {0, 1024}, {-178, 1008}, {-350, 962}, {-512, 887}, {-658, 784}, {-784, 658}, {-887, 512}, {-962, 350}, {-1008, 178},
    {-1024, 0}, {-1008, -178}, {-962, -350}, {-887, -512}, {-784, -658}, {-658, -784}, {-512, -887}, {-350, -962}, {-178, -1008},
    {0, -1024}, {178, -1008}, {350, -962}, {512, -887}, {658, -784}, {784, -658}, {887, -512}, {962, -350}, {1008, -178},
}};
constexpr std::size_t kQuarterTurn = kDirectionCount / 4;
constexpr std::size_t kHalfTurn = kDirectionCount / 2;
constexpr std::uint64_t kTrackCount = 3;
constexpr std::int64_t kTrackAngleJitterSteps = 2;
constexpr std::int64_t kBuildingMinSeparationUnits = 7;
constexpr std::int64_t kFirstPlotDistanceUnits = 12;
constexpr std::int64_t kPlotSpacingUnits = 9;
constexpr std::int64_t kPlotSetbackUnits = 6;
constexpr std::int64_t kPlotJitterUnits = 2;
constexpr std::array<std::int64_t, 5> kCommonRingRadiiUnits{{8, 11, 14, 17, 20}};
constexpr std::array<std::int64_t, 2> kYardRadiiUnits{{7, 12}};
constexpr std::size_t kCommonRingDirectionStep = 7;
constexpr std::size_t kYardDirectionStep = 4;

[[nodiscard]] Point2D offset_along(const Point2D origin, const std::size_t direction, const std::int64_t distance) noexcept {
    const auto unit = kDirections[direction % kDirectionCount];
    return Point2D{origin.x + ((unit.x * distance) / kDirectionScale), origin.z + ((unit.z * distance) / kDirectionScale)};
}

[[nodiscard]] std::uint16_t facing_degrees_for(const std::size_t direction) noexcept {
    return static_cast<std::uint16_t>((direction % kDirectionCount) * (360U / kDirectionCount));
}

[[nodiscard]] bool placement_is_free(const Point2D candidate, const std::vector<Point2D>& obstacles) noexcept {
    const auto min_sq = kBuildingMinSeparationUnits * kBuildingMinSeparationUnits;
    return std::none_of(obstacles.begin(), obstacles.end(), [&](const Point2D other) {
        return squared_distance(candidate, other) < min_sq;
    });
}

[[nodiscard]] std::int64_t seeded_offset(const SimulationSeed seed, const std::uint64_t stable_id, const std::int64_t magnitude) noexcept {
    const auto span = static_cast<std::uint64_t>((magnitude * 2) + 1);
    return static_cast<std::int64_t>(seed_random_bounded(derive_seed(seed, SeedDomain::BuildingPlacement, stable_id), span)) - magnitude;
}

[[nodiscard]] std::array<std::size_t, kTrackCount> settlement_track_directions(const SimulationSeed settlement_seed_value) noexcept {
    const auto base = seed_random_bounded(derive_seed(settlement_seed_value, SeedDomain::SettlementTracks, 0U), kDirectionCount);
    std::array<std::size_t, kTrackCount> directions{};
    for (std::uint64_t track = 0; track < kTrackCount; ++track) {
        const auto jitter_span = static_cast<std::uint64_t>((kTrackAngleJitterSteps * 2) + 1);
        const auto jitter = static_cast<std::int64_t>(
            seed_random_bounded(derive_seed(settlement_seed_value, SeedDomain::SettlementTracks, track + 1U), jitter_span)) -
            kTrackAngleJitterSteps;
        const auto spread = static_cast<std::int64_t>(track * (kDirectionCount / kTrackCount));
        const auto raw = static_cast<std::int64_t>(base) + spread + jitter + static_cast<std::int64_t>(kDirectionCount);
        directions[track] = static_cast<std::size_t>(raw % static_cast<std::int64_t>(kDirectionCount));
    }
    return directions;
}

struct PlacementChoice {
    Point2D point{};
    std::uint16_t facing_degrees{};
};

// Plot k alternates tracks, then sides of the track, then steps outward, so a
// growing village fills its tracks evenly and extends them as it grows.
[[nodiscard]] PlacementChoice track_plot(
    const Point2D centre,
    const std::array<std::size_t, kTrackCount>& tracks,
    const SimulationSeed building_seed,
    const std::uint64_t plot_index) noexcept {
    const auto track_direction = tracks[static_cast<std::size_t>(plot_index % kTrackCount)];
    const auto step = plot_index / kTrackCount;
    const auto along = kFirstPlotDistanceUnits + (static_cast<std::int64_t>(step / 2U) * kPlotSpacingUnits);
    const bool left_side = (step % 2U) == 1U;
    const auto side_direction = left_side ? track_direction + kQuarterTurn : track_direction + (kQuarterTurn * 3U);
    const auto on_track = offset_along(centre, track_direction, along);
    auto plot = offset_along(on_track, side_direction, kPlotSetbackUnits);
    plot.x += seeded_offset(building_seed, plot_index << 1U, kPlotJitterUnits);
    plot.z += seeded_offset(building_seed, (plot_index << 1U) | 1U, kPlotJitterUnits);
    return PlacementChoice{.point = plot, .facing_degrees = facing_degrees_for(side_direction + kHalfTurn)};
}

// Ticket #34/#37: a deterministic daily activity pick. Seeded selection
// among the candidate ActivityKinds gives real day-to-day and
// person-to-person variety, and Travel is one of the picked kinds so the
// Godot client (ticket #40) has real activity-transition points to hang
// navmesh-path proof-of-wiring on, even though home and work resolve to
// the same canonical point this slice (no Building position yet -- see
// #35).
[[nodiscard]] ActivityKind pick_seeded_activity(
    const SimulationSeed seed, const std::span<const ActivityKind> options) noexcept {
    if (options.empty()) {
        return ActivityKind::Idle;
    }
    const auto index = seed_random_bounded(seed, options.size());
    return options[static_cast<std::size_t>(index)];
}

// Ticket #34/#37, corrected: process_schedule_period only ever runs at an
// exact day boundary (the simulation has no sub-day tick granularity --
// advance_days(1) is the only stepping unit anywhere, including the UI
// bridge's own loop), so a genuine "hour of day" can never actually be
// observed at the moment this runs -- state_.time.ticks is always an exact
// multiple of one day there. An earlier version of this function computed
// an hour_of_day from that always-day-aligned time and branched on it,
// which meant every person's current_activity was permanently stuck at
// the hour-0 branch (Sleep) forever, since hour_of_day was always 0. Fixed
// by picking one activity per day per person directly, with no hour
// dimension: employed people mostly Work, otherwise (or unemployed) a
// seeded pick across the full non-Work range, so Travel and every other
// kind actually occur.
constexpr std::uint32_t kWorkDayChancePer10k = 7'000U;

[[nodiscard]] ActivityKind derive_daily_activity(
    const SimulationSeed person_schedule_seed,
    const std::uint64_t food_period,
    const Occupation occupation) noexcept {
    const auto day_seed = derive_seed(person_schedule_seed, SeedDomain::Schedule, food_period);

    // A merchant does no settlement work (ADR 0003), so it spends its days as an
    // unassigned adult does: teaching, keeping records and discovering. Its days
    // away on a trip will be modelled when trips exist.
    if (occupation != Occupation::None && occupation != Occupation::Merchant &&
        seed_chance_per_10k(day_seed, kWorkDayChancePer10k)) {
        return ActivityKind::Work;
    }

    static constexpr std::array<ActivityKind, 10> kNonWorkOptions{
        ActivityKind::Sleep, ActivityKind::Travel, ActivityKind::Eat, ActivityKind::Social,
        ActivityKind::Teaching, ActivityKind::RecordKeeping, ActivityKind::Discovery,
        ActivityKind::Childcare, ActivityKind::Rest, ActivityKind::Idle,
    };
    const auto pick_seed = derive_seed(day_seed, SeedDomain::Schedule, 1U);
    return pick_seeded_activity(pick_seed, kNonWorkOptions);
}

constexpr std::uint32_t kSettlementShareScale = 10'000;
// A day is strained for a settlement when at least 1 in this many of its
// households went short of food.
constexpr std::uint64_t kFoodStrainShortageDenominator = 3;
constexpr std::uint32_t kInitialAllocatedSettlementShare = 8'500;
constexpr std::uint32_t kHighPressureThresholdPer10k = 8'500;
constexpr std::uint32_t kFoundingMinimumUnallocatedShare = 1'500;
constexpr std::uint32_t kFoundingDefaultShare = 900;
// Pressure-driven fission (ADR 0002). A settlement splits when it is simply
// too big, or when it is big enough to split and has been short of food for
// two years running. A split must leave the daughter and the source at least
// these sizes, so some strain-driven candidates cannot split at all.
constexpr std::uint64_t kFissionPopulationThreshold = 120;
constexpr std::uint64_t kFissionStrainMinimumPopulation = 40;
constexpr std::uint32_t kFissionStrainDays = 2U * 365U;
constexpr std::uint64_t kFissionMinimumDaughterPopulation = kViableSettlementSize;
constexpr std::uint64_t kFissionMinimumSourcePopulation = 40;
// A daughter aims for this fraction of the source (1 in N), never below the minimum.
constexpr std::uint64_t kFissionDaughterTargetDivisor = 4;
constexpr std::uint64_t kFoundingMinimumEligibleWorkers = 4;
constexpr std::uint32_t kFoundingMinimumSourceShareAfterSplit = 1'500;
constexpr std::uint64_t kTargetHouseholdsPerSettlement = 8;
constexpr std::uint64_t kMaxInitialSettlementsPerRegion = 4;
constexpr std::int64_t kAssignmentRequiredCoveragePercent = 140;
constexpr std::int64_t kSettlementMaterialShareScale = 10'000;
constexpr std::int64_t kToolWearWorkUnitsPerBreak = 24;
constexpr std::uint32_t kToolProductivityBonusPercentAtFullCoverage = 45;
constexpr std::uint32_t kCommunicationChanceSameSettlementPer10k = 2'800;
constexpr std::uint32_t kTeachingChanceSameSettlementPer10k = 1'600;
constexpr std::uint32_t kCommunicationChanceSameHouseholdPer10k = 4'200;
constexpr std::uint32_t kTeachingChanceSameHouseholdPer10k = 6'400;
constexpr std::uint32_t kCommunicationConfidenceCapPer10k = 7'000;
constexpr std::uint32_t kTeachingConfidenceCapPer10k = 9'000;
constexpr std::uint32_t kRecordCreationChancePer10k = 1'200;
constexpr std::uint32_t kRecordReadChancePer10k = 1'900;
constexpr std::uint32_t kRecordWriteConfidenceCapPer10k = 9'500;
constexpr std::uint32_t kRecordReadConfidenceCapPer10k = 8'500;
constexpr std::uint32_t kKnowledgeDecayDirectPerYearPer10k = 120;
constexpr std::uint32_t kKnowledgeDecayTeachingPerYearPer10k = 260;
constexpr std::uint32_t kKnowledgeDecayRecordPerYearPer10k = 320;
constexpr std::uint32_t kKnowledgeDecayCommunicationPerYearPer10k = 420;
constexpr std::uint32_t kKnowledgeForgetThresholdPer10k = 1'200;
constexpr std::uint64_t kKnowledgeDecayGraceYears = 180;
constexpr std::uint32_t kDiscoveryAttemptPerSettlementPer10k = 1'800;
constexpr std::uint32_t kInstitutionFoundingChancePerSettlementPer10k = 1'000;
constexpr std::uint64_t kInstitutionMinimumSettlementPopulation = 24;
constexpr std::uint64_t kInstitutionMinimumMembers = 3;
constexpr std::uint64_t kInstitutionTargetMembers = 6;
constexpr std::uint32_t kInstitutionTeachingConfidenceCapPer10k = 9'200;
constexpr std::array<std::uint32_t, 3> kBasicDiscoveryKeys{1U, 2U, 3U};
constexpr std::int64_t kFoodCommonsTransferReserveDays = 14;
// Coin endowment (ADR 0003): the one place coin is created. A household starts
// with kCoinPerStockValue coin per unit of starting stock value (trade_pricing.hpp),
// and its settlement's commons with this percentage of what its households were given.
constexpr std::int64_t kCommonsCoinPercentOfHouseholds = 25;
constexpr std::int64_t kDwellingTimberRequired = 20;
constexpr std::int64_t kDwellingStoneRequired = 8;
constexpr std::int64_t kDwellingLabourRequired = 560;
constexpr std::int64_t kFoodStoreTimberRequired = 26;
constexpr std::int64_t kFoodStoreStoneRequired = 12;
constexpr std::int64_t kFoodStoreLabourRequired = 680;
constexpr std::int64_t kWorkshopTimberRequired = 18;
constexpr std::int64_t kWorkshopStoneRequired = 20;
constexpr std::int64_t kWorkshopLabourRequired = 620;
// Construction runs once per demographic year, so its labour budget is annual:
// idle workers' daily capacity plus this share of employed workers' daily
// capacity (seasonal slack outside the daily food model), times days per year.
constexpr std::int64_t kConstructionSeasonalLabourSharePer10k = 50;
constexpr std::int64_t kDwellingCapacityBonusPerBuilding = 18;
constexpr std::int64_t kFoodStoreCapacityBonusPerBuilding = 32;
constexpr std::int64_t kWorkshopCapacityBonusPerBuilding = 14;
constexpr std::int64_t kCommonsReserveTargetDays = 10;
constexpr std::int64_t kCommonsReplenishDays = 8;
constexpr std::int64_t kFoodCrisisBufferDays = 7;
constexpr std::uint64_t kAggregatePeoplePerHousehold = 5;
// M4E resolution-aware cadence: aggregate settlements advance in monthly
// chunks rather than daily steps. Detailed regions keep the daily cadence.
constexpr std::uint64_t kAggregateFoodPeriodDays = 30;
constexpr std::uint32_t kAggregateEligibleWorkersPer10k = 5'600;
constexpr std::uint32_t kAggregateCultivatorSharePer10k = 4'500;
constexpr std::uint32_t kAggregateGathererSharePer10k = 2'500;
constexpr std::uint32_t kAggregateWoodSharePer10k = 1'300;
constexpr std::uint32_t kAggregateStoneSharePer10k = 900;
constexpr std::uint32_t kAggregateCraftSharePer10k = 800;
constexpr std::uint32_t kAggregateBirthRatePer10k = 180;
constexpr std::uint32_t kAggregateDeathRatePer10k = 140;

struct LabourDemandProfile {
    std::int64_t cultivation_labour_demand{};
    std::int64_t gathering_labour_demand{};

    [[nodiscard]] std::int64_t total() const noexcept {
        return cultivation_labour_demand + gathering_labour_demand;
    }
};

struct MaterialTargetProfile {
    std::int64_t wood_target{};
    std::int64_t stone_target{};
    std::int64_t timber_target{};
    std::int64_t basic_tool_target{};
};

struct ConstructionDemandProfile {
    std::int64_t timber_remaining{};
    std::int64_t stone_remaining{};
    std::int64_t labour_remaining{};
    std::uint64_t active_projects{};
};

[[nodiscard]] ConstructionDemandProfile construction_demand_for_settlement(
    const SettlementId settlement_id,
    const std::vector<Building>& buildings) noexcept {
    ConstructionDemandProfile demand{};
    for (const auto& building : buildings) {
        if (building.settlement_id != settlement_id || building.state == ConstructionState::Complete) {
            continue;
        }

        ++demand.active_projects;
        demand.timber_remaining += std::max<std::int64_t>(0, building.timber_required - building.timber_committed);
        demand.stone_remaining += std::max<std::int64_t>(0, building.stone_required - building.stone_committed);
        demand.labour_remaining += std::max<std::int64_t>(0, building.labour_required - building.labour_completed);
    }
    return demand;
}

[[nodiscard]] MaterialTargetProfile material_targets_for_settlement(
    const std::uint64_t alive_population,
    const std::uint64_t eligible_workers) noexcept {
    const auto worker_i64 = static_cast<std::int64_t>(eligible_workers);
    const auto population_i64 = static_cast<std::int64_t>(alive_population);

    const auto basic_tool_target = std::max<std::int64_t>(4, (worker_i64 + 2) / 3);
    const auto timber_target = std::max<std::int64_t>(8, population_i64 / 2);
    const auto wood_target = (timber_target * 2) + basic_tool_target + std::max<std::int64_t>(2, population_i64 / 4);
    const auto stone_target = basic_tool_target + std::max<std::int64_t>(2, population_i64 / 5);

    return MaterialTargetProfile{
        .wood_target = wood_target,
        .stone_target = stone_target,
        .timber_target = timber_target,
        .basic_tool_target = basic_tool_target,
    };
}

[[nodiscard]] bool is_productive_occupation(const Occupation occupation) noexcept {
    return occupation == Occupation::SubsistenceCultivator ||
        occupation == Occupation::WildFoodGatherer ||
        occupation == Occupation::WoodGatherer ||
        occupation == Occupation::StoneExtractor ||
        occupation == Occupation::CraftWorker;
}

[[nodiscard]] std::uint32_t tool_coverage_per_10k(
    const std::int64_t tool_inventory,
    const std::uint64_t productive_workers) noexcept {
    if (productive_workers == 0U || tool_inventory <= 0) {
        return 0U;
    }

    const auto covered_workers = std::min<std::uint64_t>(
        productive_workers,
        static_cast<std::uint64_t>(tool_inventory));
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(10'000U, (covered_workers * 10'000U) / productive_workers));
}

[[nodiscard]] std::uint32_t tool_bonus_percent(const std::uint32_t coverage_per_10k) noexcept {
    return static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(std::min<std::uint32_t>(coverage_per_10k, 10'000U)) *
         kToolProductivityBonusPercentAtFullCoverage) /
        10'000U);
}

[[nodiscard]] std::uint32_t tool_output_scale_percent(const std::uint32_t coverage_per_10k) noexcept {
    return 100U + tool_bonus_percent(coverage_per_10k);
}

[[nodiscard]] std::int64_t compute_daily_food_output_target(
    const std::int64_t daily_food_required,
    const std::int64_t stored_food,
    const std::int64_t output_opportunity) noexcept {
    if (daily_food_required <= 0 || output_opportunity <= 0) {
        return 0;
    }

    const auto buffer_days = stored_food / std::max<std::int64_t>(1, daily_food_required);
    std::int64_t target = 0;
    if (buffer_days >= 90) {
        target = (daily_food_required * 90) / 100;
    } else if (buffer_days >= 45) {
        target = daily_food_required;
    } else if (buffer_days >= 14) {
        target = daily_food_required + (daily_food_required / 5);
    } else {
        target = daily_food_required + ((daily_food_required * 45) / 100);
    }

    return std::min(output_opportunity, std::max<std::int64_t>(target, 0));
}

[[nodiscard]] LabourDemandProfile split_demand_by_opportunity(
    const std::int64_t target_output,
    const std::int64_t cultivation_opportunity,
    const std::int64_t gathering_opportunity) noexcept {
    LabourDemandProfile demand{};
    const auto cultivation = std::max<std::int64_t>(0, cultivation_opportunity);
    const auto gathering = std::max<std::int64_t>(0, gathering_opportunity);
    const auto total_opportunity = cultivation + gathering;
    if (target_output <= 0 || total_opportunity <= 0) {
        return demand;
    }

    demand.cultivation_labour_demand = (target_output * cultivation) / total_opportunity;
    demand.gathering_labour_demand = target_output - demand.cultivation_labour_demand;
    return demand;
}

std::uint32_t bounded_u32(const std::uint64_t value) {
    return static_cast<std::uint32_t>(std::min(value, static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max())));
}

[[nodiscard]] std::uint64_t ceil_div_u64(const std::uint64_t value, const std::uint64_t divisor) noexcept {
    if (divisor == 0U) {
        return 0U;
    }
    return (value / divisor) + ((value % divisor) == 0U ? 0U : 1U);
}

std::int32_t clamp_i32(const std::int32_t value, const std::int32_t lo, const std::int32_t hi) {
    return std::min(std::max(value, lo), hi);
}

std::int64_t clamp_i64(const std::int64_t value, const std::int64_t lo, const std::int64_t hi) {
    return std::min(std::max(value, lo), hi);
}

[[nodiscard]] CohortAgeBand age_band_from_years(const std::uint32_t age_years) noexcept {
    if (age_years < 18U) {
        return CohortAgeBand::Child;
    }
    if (age_years < 30U) {
        return CohortAgeBand::YoungAdult;
    }
    if (age_years < 60U) {
        return CohortAgeBand::Adult;
    }
    return CohortAgeBand::Senior;
}

[[nodiscard]] CohortOccupationGroup occupation_group_from_person(const Occupation occupation) noexcept {
    if (occupation == Occupation::SubsistenceCultivator || occupation == Occupation::WildFoodGatherer) {
        return CohortOccupationGroup::Food;
    }
    if (occupation == Occupation::WoodGatherer || occupation == Occupation::StoneExtractor) {
        return CohortOccupationGroup::Materials;
    }
    if (occupation == Occupation::CraftWorker) {
        return CohortOccupationGroup::Craft;
    }
    return CohortOccupationGroup::Unspecified;
}

// Resolves a population group's coarse occupation back to a concrete one. The
// roll is scoped by the caller so the choice is stable across runs.
[[nodiscard]] Occupation occupation_from_cohort_group(const CohortOccupationGroup group, const SimulationSeed roll_seed) noexcept {
    switch (group) {
        case CohortOccupationGroup::Food:
            return seed_random_bounded(roll_seed, 2U) == 0U ? Occupation::SubsistenceCultivator : Occupation::WildFoodGatherer;
        case CohortOccupationGroup::Materials:
            return seed_random_bounded(roll_seed, 2U) == 0U ? Occupation::WoodGatherer : Occupation::StoneExtractor;
        case CohortOccupationGroup::Craft:
            return Occupation::CraftWorker;
        default:
            return Occupation::None;
    }
}

[[nodiscard]] CohortHealthBand health_band_from_points(const std::int32_t points) noexcept {
    if (points < 400) {
        return CohortHealthBand::Low;
    }
    if (points < 750) {
        return CohortHealthBand::Medium;
    }
    return CohortHealthBand::High;
}

[[nodiscard]] CohortWealthBand wealth_band_from_points(const std::int64_t points) noexcept {
    if (points < 80) {
        return CohortWealthBand::Low;
    }
    if (points < 220) {
        return CohortWealthBand::Medium;
    }
    return CohortWealthBand::High;
}

std::int64_t environment_class_productive_units_per_area(const EnvironmentClass environment_class) {
    switch (environment_class) {
        case EnvironmentClass::ProductiveArable:
            return 15;
        case EnvironmentClass::Woodland:
            return 9;
        case EnvironmentClass::Marginal:
            return 6;
        case EnvironmentClass::WaterWetland:
            return 5;
    }
    return 0;
}

std::int64_t environment_class_wild_capacity_units_per_area(const EnvironmentClass environment_class) {
    switch (environment_class) {
        case EnvironmentClass::ProductiveArable:
            return 190;
        case EnvironmentClass::Woodland:
            return 250;
        case EnvironmentClass::Marginal:
            return 140;
        case EnvironmentClass::WaterWetland:
            return 220;
    }
    return 0;
}

[[nodiscard]] bool event_payload_matches_type(const SimulationEvent& event) noexcept {
    switch (event.type) {
        case EventType::Birth:
            return std::holds_alternative<BirthEventPayload>(event.payload);
        case EventType::Death:
            return std::holds_alternative<DeathEventPayload>(event.payload);
        case EventType::SettlementFounded:
            return std::holds_alternative<SettlementFoundedEventPayload>(event.payload);
        case EventType::SettlementExtinct:
            return std::holds_alternative<SettlementExtinctEventPayload>(event.payload);
        case EventType::DiscoveryMade:
            return std::holds_alternative<DiscoveryMadeEventPayload>(event.payload);
        case EventType::InstitutionFounded:
            return std::holds_alternative<InstitutionFoundedEventPayload>(event.payload);
        case EventType::SettlementMerged:
            return std::holds_alternative<SettlementMergedEventPayload>(event.payload);
        case EventType::OccupationChanged:
            return std::holds_alternative<OccupationChangedEventPayload>(event.payload);
        case EventType::HouseholdRelocated:
            return std::holds_alternative<HouseholdRelocatedEventPayload>(event.payload);
    }
    return false;
}
}

Simulation::Simulation(SimulationConfig config)
    : config_(config),
      state_{.seed = config.seed, .time = SimulationTime{0}, .year = 0, .population = config.initial_population},
      simulation_seed_{config.seed} {
    if (config_.initial_household_food_units < 0) {
        throw std::invalid_argument("Initial household food units must be non-negative");
    }
    if (config_.food_production_scale_percent > 10'000U) {
        throw std::invalid_argument("Food production scale percent must be <= 10000");
    }
    if (config_.initial_region_count == 0U) {
        throw std::invalid_argument("Initial region count must be positive");
    }
    if (config_.base_region_area_units < kMinRegionAreaUnits || config_.base_region_area_units > kMaxRegionAreaUnits) {
        throw std::invalid_argument("Base region area units out of supported range");
    }
    if (config_.productive_effort_scale_percent > 10'000U) {
        throw std::invalid_argument("Productive effort scale percent must be <= 10000");
    }
    if (config_.wood_resource_scale_percent > 10'000U) {
        throw std::invalid_argument("Wood resource scale percent must be <= 10000");
    }
    if (config_.stone_resource_scale_percent > 10'000U) {
        throw std::invalid_argument("Stone resource scale percent must be <= 10000");
    }
    if (config_.wood_regeneration_scale_percent > 10'000U) {
        throw std::invalid_argument("Wood regeneration scale percent must be <= 10000");
    }
    if (config_.material_crafting_scale_percent > 10'000U) {
        throw std::invalid_argument("Material crafting scale percent must be <= 10000");
    }

    if (!config_.food_system_enabled) {
        food_next_run_ = SimulationTime{std::numeric_limits<std::int64_t>::max()};
    }

    initialize_population();
    initialize_households();
    initialize_regions();
    assign_households_to_regions();
    initialize_settlements();
    initialize_region_lod_states();
    initialize_settlement_aggregates();
    endow_initial_coin();
    process_household_access_allocation();
    process_occupation_reassignment(0);
    refresh_population_count();
    refresh_food_aggregates();
    refresh_environment_aggregates();
    refresh_labour_aggregates();
    refresh_material_aggregates();
    refresh_economic_aggregates();
}

const WorldState& Simulation::state() const noexcept {
    return state_;
}

SimulationTime Simulation::time() const noexcept {
    return state_.time;
}

std::span<const AgentMemory> Simulation::agent_memories() const noexcept {
    return state_.agent_memories;
}

std::vector<AgentMemory> Simulation::memories_for_person(const PersonId person_id) const {
    std::vector<AgentMemory> memories;
    for (const auto& memory : state_.agent_memories) {
        if (memory.owner_id == person_id) {
            memories.push_back(memory);
        }
    }
    return memories;
}

std::span<const KnowledgeTransmission> Simulation::knowledge_transmissions() const noexcept {
    return state_.knowledge_transmissions;
}

std::vector<KnowledgeTransmission> Simulation::transmissions_for_recipient(const PersonId person_id) const {
    std::vector<KnowledgeTransmission> transmissions;
    for (const auto& transmission : state_.knowledge_transmissions) {
        if (transmission.recipient_id == person_id) {
            transmissions.push_back(transmission);
        }
    }
    return transmissions;
}

std::optional<KnowledgeTransmission> Simulation::transmission_for_memory(const MemoryId recipient_memory_id) const {
    const auto index_it = communication_index_by_recipient_memory_.find(recipient_memory_id);
    if (index_it == communication_index_by_recipient_memory_.end()) {
        return std::nullopt;
    }
    if (index_it->second >= state_.knowledge_transmissions.size()) {
        return std::nullopt;
    }
    return state_.knowledge_transmissions[index_it->second];
}

std::span<const KnowledgeRecord> Simulation::knowledge_records() const noexcept {
    return state_.knowledge_records;
}

std::vector<KnowledgeRecord> Simulation::records_for_settlement(const SettlementId settlement_id) const {
    std::vector<KnowledgeRecord> records;
    for (const auto& record : state_.knowledge_records) {
        if (record.settlement_id == settlement_id) {
            records.push_back(record);
        }
    }
    return records;
}

std::span<const Institution> Simulation::institutions() const noexcept {
    return state_.institutions;
}

std::vector<Institution> Simulation::institutions_for_settlement(const SettlementId settlement_id) const {
    std::vector<Institution> institutions;
    for (const auto& institution : state_.institutions) {
        if (institution.settlement_id == settlement_id) {
            institutions.push_back(institution);
        }
    }
    return institutions;
}

std::optional<KnowledgeRecordAccess> Simulation::record_access_for_memory(const MemoryId reader_memory_id) const {
    const auto index_it = record_access_index_by_memory_.find(reader_memory_id);
    if (index_it == record_access_index_by_memory_.end()) {
        return std::nullopt;
    }
    if (index_it->second >= state_.knowledge_record_accesses.size()) {
        return std::nullopt;
    }
    return state_.knowledge_record_accesses[index_it->second];
}

bool Simulation::person_remembers_event(const PersonId person_id, const EventId event_id) const noexcept {
    for (const auto& memory : state_.agent_memories) {
        if (memory.owner_id == person_id && memory.source_event_id == event_id) {
            return true;
        }
    }
    return false;
}


struct KnowledgeFactDescriptor {
    KnowledgeFactKind kind{KnowledgeFactKind::PersonLifeStatus};
    std::uint64_t primary_subject_id{};
    std::uint64_t secondary_subject_id{};
};

[[nodiscard]] KnowledgeFactDescriptor describe_knowledge_fact(const KnowledgeFact& fact) {
    if (const auto* person_life = std::get_if<PersonLifeStatusKnowledgeFact>(&fact); person_life != nullptr) {
        return KnowledgeFactDescriptor{
            .kind = KnowledgeFactKind::PersonLifeStatus,
            .primary_subject_id = person_life->person_id,
            .secondary_subject_id = 0U,
        };
    }

    if (const auto* person_household = std::get_if<PersonHouseholdKnowledgeFact>(&fact); person_household != nullptr) {
        return KnowledgeFactDescriptor{
            .kind = KnowledgeFactKind::PersonHousehold,
            .primary_subject_id = person_household->person_id,
            .secondary_subject_id = 0U,
        };
    }

    if (const auto* settlement = std::get_if<SettlementStatusKnowledgeFact>(&fact); settlement != nullptr) {
        return KnowledgeFactDescriptor{
            .kind = KnowledgeFactKind::SettlementStatus,
            .primary_subject_id = settlement->settlement_id,
            .secondary_subject_id = 0U,
        };
    }

    if (const auto* discovery = std::get_if<BasicDiscoveryKnowledgeFact>(&fact); discovery != nullptr) {
        return KnowledgeFactDescriptor{
            .kind = KnowledgeFactKind::BasicDiscovery,
            .primary_subject_id = discovery->discovery_key,
            .secondary_subject_id = 0U,
        };
    }

    throw std::invalid_argument("Unknown knowledge fact variant");
}

std::uint64_t Simulation::memory_count_for_person(const PersonId person_id) const noexcept {
    std::uint64_t count = 0;
    for (const auto& memory : state_.agent_memories) {
        if (memory.owner_id == person_id) {
            ++count;
        }
    }
    return count;
}

std::span<const AgentKnowledge> Simulation::agent_knowledge() const noexcept {
    return state_.agent_knowledge;
}

std::vector<AgentKnowledge> Simulation::knowledge_for_person(const PersonId person_id) const {
    std::vector<AgentKnowledge> facts;
    const auto owner_it = knowledge_indices_by_owner_.find(person_id);
    if (owner_it == knowledge_indices_by_owner_.end()) {
        return facts;
    }

    facts.reserve(owner_it->second.size());
    for (const auto index : owner_it->second) {
        if (index >= state_.agent_knowledge.size()) {
            continue;
        }
        if (state_.agent_knowledge[index].confidence_per_10k == 0U) {
            continue;
        }
        facts.push_back(state_.agent_knowledge[index]);
    }
    return facts;
}

std::uint64_t Simulation::knowledge_count_for_person(const PersonId person_id) const noexcept {
    const auto owner_it = knowledge_indices_by_owner_.find(person_id);
    if (owner_it == knowledge_indices_by_owner_.end()) {
        return 0U;
    }
    std::uint64_t count = 0U;
    for (const auto index : owner_it->second) {
        if (index >= state_.agent_knowledge.size()) {
            continue;
        }
        if (state_.agent_knowledge[index].confidence_per_10k == 0U) {
            continue;
        }
        ++count;
    }
    return count;
}

std::optional<KnownLifeStatus> Simulation::known_life_status(
    const PersonId knower_id,
    const PersonId subject_id) const noexcept {
    const KnowledgeSemanticKey key{
        .owner_id = knower_id,
        .fact_kind = KnowledgeFactKind::PersonLifeStatus,
        .primary_subject_id = subject_id,
        .secondary_subject_id = 0U,
    };
    const auto key_it = knowledge_index_by_semantic_key_.find(key);
    if (key_it == knowledge_index_by_semantic_key_.end() || key_it->second >= state_.agent_knowledge.size()) {
        return std::nullopt;
    }
    if (state_.agent_knowledge[key_it->second].confidence_per_10k == 0U) {
        return std::nullopt;
    }

    const auto* fact = std::get_if<PersonLifeStatusKnowledgeFact>(&state_.agent_knowledge[key_it->second].fact);
    if (fact == nullptr) {
        return std::nullopt;
    }

    return fact->status;
}

std::optional<HouseholdId> Simulation::known_household_for_person(
    const PersonId knower_id,
    const PersonId subject_id) const noexcept {
    const KnowledgeSemanticKey key{
        .owner_id = knower_id,
        .fact_kind = KnowledgeFactKind::PersonHousehold,
        .primary_subject_id = subject_id,
        .secondary_subject_id = 0U,
    };
    const auto key_it = knowledge_index_by_semantic_key_.find(key);
    if (key_it == knowledge_index_by_semantic_key_.end() || key_it->second >= state_.agent_knowledge.size()) {
        return std::nullopt;
    }
    if (state_.agent_knowledge[key_it->second].confidence_per_10k == 0U) {
        return std::nullopt;
    }

    const auto* fact = std::get_if<PersonHouseholdKnowledgeFact>(&state_.agent_knowledge[key_it->second].fact);
    if (fact == nullptr) {
        return std::nullopt;
    }

    return fact->household_id;
}

std::optional<SettlementStatusKnowledgeFact> Simulation::known_settlement_status(
    const PersonId knower_id,
    const SettlementId settlement_id) const noexcept {
    const KnowledgeSemanticKey key{
        .owner_id = knower_id,
        .fact_kind = KnowledgeFactKind::SettlementStatus,
        .primary_subject_id = settlement_id,
        .secondary_subject_id = 0U,
    };
    const auto key_it = knowledge_index_by_semantic_key_.find(key);
    if (key_it == knowledge_index_by_semantic_key_.end() || key_it->second >= state_.agent_knowledge.size()) {
        return std::nullopt;
    }
    if (state_.agent_knowledge[key_it->second].confidence_per_10k == 0U) {
        return std::nullopt;
    }

    const auto* fact = std::get_if<SettlementStatusKnowledgeFact>(&state_.agent_knowledge[key_it->second].fact);
    if (fact == nullptr) {
        return std::nullopt;
    }

    return *fact;
}

bool Simulation::knows_basic_discovery(const PersonId knower_id, const std::uint32_t discovery_key) const noexcept {
    if (discovery_key == 0U) {
        return false;
    }

    const KnowledgeSemanticKey key{
        .owner_id = knower_id,
        .fact_kind = KnowledgeFactKind::BasicDiscovery,
        .primary_subject_id = discovery_key,
        .secondary_subject_id = 0U,
    };
    const auto key_it = knowledge_index_by_semantic_key_.find(key);
    if (key_it == knowledge_index_by_semantic_key_.end() || key_it->second >= state_.agent_knowledge.size()) {
        return false;
    }
    if (state_.agent_knowledge[key_it->second].confidence_per_10k == 0U) {
        return false;
    }

    const auto* fact = std::get_if<BasicDiscoveryKnowledgeFact>(&state_.agent_knowledge[key_it->second].fact);
    if (fact == nullptr) {
        return false;
    }
    return fact->discovery_key == discovery_key;
}

std::vector<std::byte> Simulation::save_snapshot() const {
    return SnapshotCodec::serialize(*this);
}

SnapshotLoadResult Simulation::load_snapshot(const std::span<const std::byte> data, Simulation& destination) {
    return SnapshotCodec::deserialize(data, destination);
}

void Simulation::set_region_detail_level(const RegionId region_id, const SimulationDetailLevel level) {
    if (find_region(region_id) == nullptr) {
        throw std::invalid_argument("Unknown region id for detail-level change");
    }

    auto found = false;
    for (auto& entry : state_.region_lod_states) {
        if (entry.region_id == region_id) {
            entry.detail_level = level;
            found = true;
            break;
        }
    }
    if (!found) {
        state_.region_lod_states.push_back(RegionLodState{.region_id = region_id, .detail_level = level});
        std::sort(state_.region_lod_states.begin(), state_.region_lod_states.end(), [](const RegionLodState& lhs, const RegionLodState& rhs) {
            return lhs.region_id < rhs.region_id;
        });
    }

    rebuild_global_detail_level();
}

SimulationDetailLevel Simulation::region_detail_level(const RegionId region_id) const noexcept {
    for (const auto& entry : state_.region_lod_states) {
        if (entry.region_id == region_id) {
            return entry.detail_level;
        }
    }
    return SimulationDetailLevel::Detailed;
}

void Simulation::add_aggregate_population(
    const RegionId region_id,
    const SettlementId settlement_id,
    const std::uint64_t population) {
    if (population == 0U) {
        return;
    }

    const auto* region = find_region(region_id);
    if (region == nullptr) {
        throw std::invalid_argument("Unknown region id for aggregate population");
    }
    (void)region;

    SettlementId effective_settlement_id = settlement_id;
    if (effective_settlement_id == 0U) {
        // Only the share still unclaimed in this region may be taken, otherwise
        // regional access allocation would exceed its capacity.
        std::uint32_t claimed_share = 0U;
        for (const auto& existing : state_.settlements) {
            if (existing.active && existing.region_id == region_id) {
                claimed_share += existing.access_share_points;
            }
        }
        if (claimed_share >= kSettlementShareScale) {
            throw std::invalid_argument("Region has no remaining access share for an aggregate settlement");
        }
        const auto available_share = kSettlementShareScale - claimed_share;

        effective_settlement_id = next_settlement_id();
        state_.settlements.push_back(Settlement{
            .id = effective_settlement_id,
            .region_id = region_id,
            .active = true,
            .access_share_points = available_share,
        });
        assign_position_for_new_settlement(effective_settlement_id, region_id);
        state_.settlement_aggregates.push_back(SettlementAggregateState{
            .settlement_id = effective_settlement_id,
            .region_id = region_id,
        });
        std::sort(state_.settlements.begin(), state_.settlements.end(), [](const Settlement& lhs, const Settlement& rhs) {
            return lhs.id < rhs.id;
        });
        std::sort(state_.settlement_aggregates.begin(), state_.settlement_aggregates.end(), [](const SettlementAggregateState& lhs, const SettlementAggregateState& rhs) {
            return lhs.settlement_id < rhs.settlement_id;
        });
    }

    const auto* settlement = find_settlement(effective_settlement_id);
    if (settlement == nullptr || settlement->region_id != region_id) {
        throw std::invalid_argument("Unknown settlement id for aggregate population");
    }

    set_region_detail_level(region_id, SimulationDetailLevel::SettlementAggregate);

    const auto male_population = population / 2U;
    const auto female_population = population - male_population;

    const auto push_cohort = [&](const std::uint64_t cohort_population, const BiologicalSex sex) {
        if (cohort_population == 0U) {
            return;
        }
        state_.population_cohorts.push_back(PopulationCohort{
            .id = next_cohort_id_++,
            .region_id = region_id,
            .settlement_id = effective_settlement_id,
            .age_band = CohortAgeBand::Adult,
            .sex = sex,
            .occupation_group = CohortOccupationGroup::Food,
            .health_band = CohortHealthBand::Medium,
            .wealth_band = CohortWealthBand::Medium,
            .knowledge_profile_id = 1U,
            .population = cohort_population,
        });
    };

    push_cohort(female_population, BiologicalSex::Female);
    push_cohort(male_population, BiologicalSex::Male);

    std::sort(state_.population_cohorts.begin(), state_.population_cohorts.end(), [](const PopulationCohort& lhs, const PopulationCohort& rhs) {
        if (lhs.region_id != rhs.region_id) {
            return lhs.region_id < rhs.region_id;
        }
        if (lhs.settlement_id != rhs.settlement_id) {
            return lhs.settlement_id < rhs.settlement_id;
        }
        return lhs.id < rhs.id;
    });

    rebuild_settlement_aggregate_distribution(effective_settlement_id);
    refresh_population_count();
    refresh_food_aggregates();
    refresh_labour_aggregates();
    refresh_material_aggregates();
    refresh_economic_aggregates();
}

std::uint64_t Simulation::aggregate_population() const noexcept {
    std::uint64_t total = 0;
    for (const auto& cohort : state_.population_cohorts) {
        total += cohort.population;
    }
    return total;
}

std::uint64_t Simulation::detailed_population() const noexcept {
    std::uint64_t total = 0;
    for (const auto& person : state_.people) {
        if (person.alive) {
            ++total;
        }
    }
    return total;
}

std::uint64_t Simulation::current_demographic_year() const noexcept {
    return DemographicCalendarAdapter::demographic_year(state_.time);
}

std::uint64_t Simulation::current_food_period() const noexcept {
    if (state_.time.ticks <= 0) {
        return 0;
    }

    const auto day_ticks = days(1).ticks;
    return static_cast<std::uint64_t>(state_.time.ticks / day_ticks);
}

void Simulation::advance(const SimulationDuration duration) {
    if (duration.ticks < 0) {
        throw std::invalid_argument("Simulation duration must be non-negative");
    }

    SimulationTime target_time{};
    if (!checked_add_time(state_.time, duration, target_time)) {
        throw std::overflow_error("Simulation time overflow");
    }

    run_scheduled_systems_until(target_time);
    state_.time = target_time;
    state_.year = current_demographic_year();
}

void Simulation::run_scheduled_systems_until(const SimulationTime target_time) {
    while (true) {
        SimulationTime next_time = demography_next_run_;
        if (config_.food_system_enabled && food_next_run_ < next_time) {
            next_time = food_next_run_;
        }

        if (next_time > target_time) {
            break;
        }

        state_.time = next_time;
        state_.year = current_demographic_year();

        if (config_.food_system_enabled && food_next_run_ <= next_time) {
            process_food_period(current_food_period());
            process_schedule_period(current_food_period());

            SimulationTime next_food_run{};
            if (!checked_add_time(food_next_run_, food_interval_, next_food_run)) {
                throw std::overflow_error("Scheduled food system time overflow");
            }
            food_next_run_ = next_food_run;
        }

        if (demography_next_run_ <= next_time) {
            process_demography_period(state_.year);

            SimulationTime next_run{};
            if (!checked_add_time(demography_next_run_, demography_interval_, next_run)) {
                throw std::overflow_error("Scheduled demography time overflow");
            }
            demography_next_run_ = next_run;
        }
    }
}

void Simulation::process_food_period(const std::uint64_t food_period) {
    process_household_access_allocation();

    struct HouseholdDayState {
        Household* household{};
        std::vector<Person*> alive_members{};
        std::vector<std::int64_t> per_person_required{};
        std::int64_t required_food{};
        std::int64_t cultivation_labour_available{};
        std::int64_t gathering_labour_available{};
        std::int64_t cultivation_labour_allocated{};
        std::int64_t gathering_labour_allocated{};
        std::int64_t produced_food{};
    };

    std::vector<HouseholdDayState> household_states;
    household_states.reserve(state_.households.size());

    std::unordered_map<PersonId, Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<SettlementId, Settlement*> settlement_by_id;
    settlement_by_id.reserve(state_.settlements.size());
    for (auto& settlement : state_.settlements) {
        settlement_by_id.emplace(settlement.id, &settlement);
    }

    for (auto& settlement : state_.settlements) {
        settlement.last_tool_coverage_per_10k = 0U;
        settlement.last_transfer_food_volume = 0;
        settlement.last_exchange_volume = 0;
    }

    for (auto& household : state_.households) {
        // LOD gate: households in non-detailed regions are executed by
        // process_aggregate_food_period, never by per-person daily loops.
        if (!is_region_detailed(household.region_id)) {
            continue;
        }

        HouseholdDayState household_state{};
        household_state.household = &household;
        household_state.alive_members.reserve(household.member_ids.size());
        for (const auto member_id : household.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it == person_by_id.end()) {
                continue;
            }

            auto* person = person_it->second;
            if (person->alive) {
                household_state.alive_members.push_back(person);
            }
        }

        std::sort(household_state.alive_members.begin(), household_state.alive_members.end(), [](const Person* lhs, const Person* rhs) {
            return lhs->id < rhs->id;
        });

        household_state.per_person_required.reserve(household_state.alive_members.size());
        for (const auto* person : household_state.alive_members) {
            const auto person_required = food_requirement_for_person(*person);
            if (!checked_add_i64(household_state.required_food, person_required, household_state.required_food)) {
                throw std::overflow_error("Food accounting overflow");
            }

            if (occupation_eligible_for_productive_work(*person)) {
                const auto person_capacity = productive_effort_for_person(*person);
                if (person->occupation == Occupation::SubsistenceCultivator) {
                    if (!checked_add_i64(
                            household_state.cultivation_labour_available,
                            person_capacity,
                            household_state.cultivation_labour_available)) {
                        throw std::overflow_error("Cultivation labour accounting overflow");
                    }
                } else if (person->occupation == Occupation::WildFoodGatherer) {
                    if (!checked_add_i64(
                            household_state.gathering_labour_available,
                            person_capacity,
                            household_state.gathering_labour_available)) {
                        throw std::overflow_error("Gathering labour accounting overflow");
                    }
                }
            }

            household_state.per_person_required.push_back(person_required);
        }

        household_states.push_back(std::move(household_state));
    }

    std::sort(household_states.begin(), household_states.end(), [](const HouseholdDayState& lhs, const HouseholdDayState& rhs) {
        return lhs.household->id < rhs.household->id;
    });

    std::unordered_map<SettlementId, std::vector<HouseholdDayState*>> household_states_by_settlement;
    household_states_by_settlement.reserve(state_.settlements.size());
    for (auto& household_state : household_states) {
        household_states_by_settlement[household_state.household->settlement_id].push_back(&household_state);
    }

    state_.eligible_workers = 0;
    state_.active_workers = 0;
    state_.unallocated_workers = 0;
    state_.cultivators = 0;
    state_.gatherers = 0;
    state_.wood_gatherers = 0;
    state_.stone_extractors = 0;
    state_.craft_workers = 0;
    state_.total_labour_capacity = 0;
    state_.total_labour_demand = 0;
    state_.total_labour_allocated = 0;
    state_.cultivation_labour_available = 0;
    state_.cultivation_labour_allocated = 0;
    state_.gathering_labour_available = 0;
    state_.gathering_labour_allocated = 0;
    state_.labour_shortfall = 0;
    state_.wood_labour_available = 0;
    state_.stone_labour_available = 0;
    state_.craft_labour_available = 0;
    state_.wood_labour_available = 0;
    state_.stone_labour_available = 0;
    state_.craft_labour_available = 0;

    for (const auto& person : state_.people) {
        if (!occupation_eligible_for_productive_work(person)) {
            continue;
        }

        const auto capacity = productive_effort_for_person(person);
        ++state_.eligible_workers;
        if (!checked_add_i64(state_.total_labour_capacity, capacity, state_.total_labour_capacity)) {
            throw std::overflow_error("Total labour capacity overflow");
        }

        if (person.occupation == Occupation::SubsistenceCultivator) {
            ++state_.active_workers;
            ++state_.cultivators;
            if (!checked_add_i64(state_.cultivation_labour_available, capacity, state_.cultivation_labour_available)) {
                throw std::overflow_error("Cultivation available labour overflow");
            }
        } else if (person.occupation == Occupation::WildFoodGatherer) {
            ++state_.active_workers;
            ++state_.gatherers;
            if (!checked_add_i64(state_.gathering_labour_available, capacity, state_.gathering_labour_available)) {
                throw std::overflow_error("Gathering available labour overflow");
            }
        } else if (person.occupation == Occupation::WoodGatherer) {
            ++state_.active_workers;
            ++state_.wood_gatherers;
            if (!checked_add_i64(state_.wood_labour_available, capacity, state_.wood_labour_available)) {
                throw std::overflow_error("Wood available labour overflow");
            }
        } else if (person.occupation == Occupation::StoneExtractor) {
            ++state_.active_workers;
            ++state_.stone_extractors;
            if (!checked_add_i64(state_.stone_labour_available, capacity, state_.stone_labour_available)) {
                throw std::overflow_error("Stone available labour overflow");
            }
        } else if (person.occupation == Occupation::CraftWorker) {
            ++state_.active_workers;
            ++state_.craft_workers;
            if (!checked_add_i64(state_.craft_labour_available, capacity, state_.craft_labour_available)) {
                throw std::overflow_error("Craft available labour overflow");
            }
        } else {
            // Unallocated means not assigned to settlement labour; a merchant does no
            // settlement work, so it is counted here (the UI lists merchants separately).
            ++state_.unallocated_workers;
        }
    }

    for (auto& region : state_.regions) {
        std::vector<SettlementId> active_settlement_ids;
        active_settlement_ids.reserve(state_.settlements.size());
        for (const auto& settlement : state_.settlements) {
            if (settlement.active && settlement.region_id == region.id && settlement.access_share_points > 0U) {
                active_settlement_ids.push_back(settlement.id);
            }
        }
        std::sort(active_settlement_ids.begin(), active_settlement_ids.end());

        auto productive_available = apply_scaled_percent(region.productive_capacity_per_day, config_.food_production_scale_percent);
        const auto productive_seed = derive_seed(region_seed(region.id), SeedDomain::RegionDailyVariation, food_period);
        const auto productive_variation_percent = 85U + static_cast<std::uint32_t>(seed_random_bounded(productive_seed, 31U));
        productive_available = apply_scaled_percent(productive_available, productive_variation_percent);

        auto wild_harvest_available = std::min(region.wild_food_stock, region.wild_food_harvest_limit_per_day);
        const auto wild_seed = derive_seed(region_seed(region.id), SeedDomain::RegionWildHarvest, food_period);
        const auto wild_variation_percent = 80U + static_cast<std::uint32_t>(seed_random_bounded(wild_seed, 41U));
        wild_harvest_available = apply_scaled_percent(wild_harvest_available, wild_variation_percent);
        wild_harvest_available = std::min(wild_harvest_available, region.wild_food_stock);

        std::int64_t distributed_wild = 0;
        auto productive_remaining = productive_available;
        auto wild_harvest_remaining = wild_harvest_available;

        for (const auto settlement_id : active_settlement_ids) {
            const auto settlement_it = settlement_by_id.find(settlement_id);
            auto* settlement = (settlement_it != settlement_by_id.end()) ? settlement_it->second : nullptr;
            if (settlement == nullptr || settlement->access_share_points == 0U) {
                continue;
            }

            const auto grouped_households_it = household_states_by_settlement.find(settlement_id);
            if (grouped_households_it == household_states_by_settlement.end()) {
                continue;
            }

            const auto& settlement_households = grouped_households_it->second;
            std::int64_t settlement_required_food = 0;
            std::int64_t settlement_stored_food = 0;
            std::int64_t settlement_cultivation_available = 0;
            std::int64_t settlement_gathering_available = 0;
            std::uint64_t settlement_productive_workers = 0;
            bool has_region_households = false;
            for (auto* household_state : settlement_households) {
                if (household_state->household->region_id != region.id) {
                    continue;
                }
                has_region_households = true;

                if (!checked_add_i64(settlement_required_food, household_state->required_food, settlement_required_food) ||
                    !checked_add_i64(settlement_stored_food, household_state->household->resources.food_inventory.value, settlement_stored_food) ||
                    !checked_add_i64(
                        settlement_cultivation_available,
                        household_state->cultivation_labour_available,
                        settlement_cultivation_available) ||
                    !checked_add_i64(
                        settlement_gathering_available,
                        household_state->gathering_labour_available,
                        settlement_gathering_available)) {
                    throw std::overflow_error("Settlement labour/demand aggregation overflow");
                }

                for (const auto* person : household_state->alive_members) {
                    if (!occupation_eligible_for_productive_work(*person)) {
                        continue;
                    }
                    if (is_productive_occupation(person->occupation)) {
                        ++settlement_productive_workers;
                    }
                }
            }

            if (!has_region_households) {
                continue;
            }

            auto& tool_inventory = settlement->materials.basic_tool.value;
            const auto tools_used = std::min<std::uint64_t>(
                settlement_productive_workers,
                tool_inventory > 0 ? static_cast<std::uint64_t>(tool_inventory) : 0U);

            if (tools_used > 0U) {
                if (!checked_add_i64(
                        settlement->tool_wear_work_units,
                        static_cast<std::int64_t>(tools_used),
                        settlement->tool_wear_work_units)) {
                    throw std::overflow_error("Tool wear accumulation overflow");
                }

                auto broken_tools = settlement->tool_wear_work_units / kToolWearWorkUnitsPerBreak;
                settlement->tool_wear_work_units %= kToolWearWorkUnitsPerBreak;
                broken_tools = std::min<std::int64_t>(broken_tools, std::max<std::int64_t>(0, tool_inventory));

                if (broken_tools > 0) {
                    if (!checked_sub_nonnegative_i64(tool_inventory, broken_tools, tool_inventory) ||
                        !checked_add_i64(
                            settlement->cumulative_basic_tool_broken,
                            broken_tools,
                            settlement->cumulative_basic_tool_broken)) {
                        throw std::overflow_error("Tool breakage accounting overflow");
                    }
                }
            }

            settlement->last_tool_coverage_per_10k =
                tool_coverage_per_10k(tool_inventory, settlement_productive_workers);
            const auto output_scale_percent = tool_output_scale_percent(settlement->last_tool_coverage_per_10k);

            auto settlement_productive_budget =
                (productive_available * static_cast<std::int64_t>(settlement->access_share_points)) /
                static_cast<std::int64_t>(kSettlementShareScale);
            auto settlement_wild_budget =
                (wild_harvest_available * static_cast<std::int64_t>(settlement->access_share_points)) /
                static_cast<std::int64_t>(kSettlementShareScale);

            settlement_productive_budget = apply_scaled_percent(settlement_productive_budget, output_scale_percent);
            settlement_wild_budget = apply_scaled_percent(settlement_wild_budget, output_scale_percent);
            settlement_productive_budget = std::min(
                std::max<std::int64_t>(0, settlement_productive_budget),
                std::max<std::int64_t>(0, productive_remaining));
            settlement_wild_budget = std::min(
                std::max<std::int64_t>(0, settlement_wild_budget),
                std::max<std::int64_t>(0, wild_harvest_remaining));

            const auto output_target = compute_daily_food_output_target(
                settlement_required_food,
                settlement_stored_food,
                settlement_productive_budget + settlement_wild_budget);
            const auto labour_demand = split_demand_by_opportunity(
                output_target,
                settlement_productive_budget,
                settlement_wild_budget);

            if (!checked_add_i64(state_.total_labour_demand, labour_demand.total(), state_.total_labour_demand)) {
                throw std::overflow_error("Total labour demand overflow");
            }

            const auto settlement_cultivation_allocated =
                std::min(settlement_cultivation_available, std::max<std::int64_t>(0, labour_demand.cultivation_labour_demand));
            const auto settlement_gathering_allocated =
                std::min(settlement_gathering_available, std::max<std::int64_t>(0, labour_demand.gathering_labour_demand));

            if (!checked_add_i64(
                    state_.cultivation_labour_allocated,
                    settlement_cultivation_allocated,
                    state_.cultivation_labour_allocated) ||
                !checked_add_i64(
                    state_.gathering_labour_allocated,
                    settlement_gathering_allocated,
                    state_.gathering_labour_allocated)) {
                throw std::overflow_error("Allocated labour aggregate overflow");
            }

            std::int64_t distributed_cultivation_allocation = 0;
            for (auto* household_state : settlement_households) {
                if (settlement_cultivation_allocated <= 0 || settlement_cultivation_available <= 0 ||
                    household_state->cultivation_labour_available <= 0) {
                    continue;
                }
                household_state->cultivation_labour_allocated =
                    (settlement_cultivation_allocated * household_state->cultivation_labour_available) /
                    settlement_cultivation_available;
                if (!checked_add_i64(
                        distributed_cultivation_allocation,
                        household_state->cultivation_labour_allocated,
                        distributed_cultivation_allocation)) {
                    throw std::overflow_error("Cultivation allocation distribution overflow");
                }
            }

            auto cultivation_remainder = settlement_cultivation_allocated - distributed_cultivation_allocation;
            for (auto* household_state : settlement_households) {
                if (cultivation_remainder <= 0) {
                    break;
                }
                if (household_state->cultivation_labour_available <= 0) {
                    continue;
                }
                ++household_state->cultivation_labour_allocated;
                --cultivation_remainder;
            }

            std::int64_t distributed_gathering_allocation = 0;
            for (auto* household_state : settlement_households) {
                if (settlement_gathering_allocated <= 0 || settlement_gathering_available <= 0 ||
                    household_state->gathering_labour_available <= 0) {
                    continue;
                }
                household_state->gathering_labour_allocated =
                    (settlement_gathering_allocated * household_state->gathering_labour_available) /
                    settlement_gathering_available;
                if (!checked_add_i64(
                        distributed_gathering_allocation,
                        household_state->gathering_labour_allocated,
                        distributed_gathering_allocation)) {
                    throw std::overflow_error("Gathering allocation distribution overflow");
                }
            }

            auto gathering_remainder = settlement_gathering_allocated - distributed_gathering_allocation;
            for (auto* household_state : settlement_households) {
                if (gathering_remainder <= 0) {
                    break;
                }
                if (household_state->gathering_labour_available <= 0) {
                    continue;
                }
                ++household_state->gathering_labour_allocated;
                --gathering_remainder;
            }

            std::int64_t settlement_cultivation_output = 0;
            std::int64_t settlement_gathering_output = 0;
            if (!checked_add_i64(
                    settlement_cultivation_output,
                    std::min(settlement_productive_budget, settlement_cultivation_allocated),
                    settlement_cultivation_output) ||
                !checked_add_i64(
                    settlement_gathering_output,
                    std::min(settlement_wild_budget, settlement_gathering_allocated),
                    settlement_gathering_output)) {
                throw std::overflow_error("Specialized output overflow");
            }

            std::int64_t distributed_productive = 0;
            for (auto* household_state : settlement_households) {
                if (settlement_cultivation_allocated <= 0 || household_state->cultivation_labour_allocated <= 0) {
                    continue;
                }
                const auto produced =
                    (settlement_cultivation_output * household_state->cultivation_labour_allocated) /
                    settlement_cultivation_allocated;
                household_state->produced_food += produced;
                if (!checked_add_i64(distributed_productive, produced, distributed_productive)) {
                    throw std::overflow_error("Settlement productive distribution overflow");
                }
            }

            auto productive_remainder = settlement_cultivation_output - distributed_productive;
            for (auto* household_state : settlement_households) {
                if (productive_remainder <= 0) {
                    break;
                }
                if (household_state->cultivation_labour_allocated <= 0) {
                    continue;
                }
                ++household_state->produced_food;
                --productive_remainder;
            }

            std::int64_t settlement_distributed_wild = 0;
            for (auto* household_state : settlement_households) {
                if (settlement_gathering_allocated <= 0 || household_state->gathering_labour_allocated <= 0) {
                    continue;
                }
                const auto wild_for_household =
                    (settlement_gathering_output * household_state->gathering_labour_allocated) /
                    settlement_gathering_allocated;
                household_state->produced_food += wild_for_household;
                if (!checked_add_i64(settlement_distributed_wild, wild_for_household, settlement_distributed_wild)) {
                    throw std::overflow_error("Settlement wild distribution overflow");
                }
            }

            auto wild_remainder = settlement_gathering_output - settlement_distributed_wild;
            for (auto* household_state : settlement_households) {
                if (wild_remainder <= 0) {
                    break;
                }
                if (household_state->gathering_labour_allocated <= 0) {
                    continue;
                }
                ++household_state->produced_food;
                --wild_remainder;
                ++settlement_distributed_wild;
            }

            if (!checked_add_i64(distributed_wild, settlement_distributed_wild, distributed_wild)) {
                throw std::overflow_error("Region wild distribution overflow");
            }

            if (!checked_sub_nonnegative_i64(
                    productive_remaining,
                    settlement_cultivation_output,
                    productive_remaining) ||
                !checked_sub_nonnegative_i64(
                    wild_harvest_remaining,
                    settlement_distributed_wild,
                    wild_harvest_remaining)) {
                throw std::overflow_error("Region capacity accounting underflow");
            }
        }

        region.wild_food_stock = std::max<std::int64_t>(0, region.wild_food_stock - distributed_wild);
        if (!checked_add_i64(region.cumulative_wild_food_harvested, distributed_wild, region.cumulative_wild_food_harvested)) {
            throw std::overflow_error("Region cumulative harvest overflow");
        }

        if (region.wild_food_stock < region.wild_food_capacity) {
            const auto deficit = region.wild_food_capacity - region.wild_food_stock;
            const auto regeneration = std::min(deficit, region.wild_food_regeneration_per_day);
            if (!checked_add_i64(region.wild_food_stock, regeneration, region.wild_food_stock) ||
                !checked_add_i64(region.cumulative_wild_food_regenerated, regeneration, region.cumulative_wild_food_regenerated)) {
                throw std::overflow_error("Region regeneration overflow");
            }
        }
    }

    if (!checked_add_i64(
            state_.cultivation_labour_allocated,
            state_.gathering_labour_allocated,
            state_.total_labour_allocated)) {
        throw std::overflow_error("Total allocated labour overflow");
    }
    state_.labour_shortfall = std::max<std::int64_t>(0, state_.total_labour_demand - state_.total_labour_allocated);

    for (auto& household_state : household_states) {
        auto& household = *household_state.household;
        const auto produced_food = household_state.produced_food;

        std::int64_t inventory_after_production = 0;
        if (!checked_add_i64(household.resources.food_inventory.value, produced_food, inventory_after_production)) {
            throw std::overflow_error("Food inventory overflow on production");
        }
        household.resources.food_inventory.value = inventory_after_production;

        if (!checked_add_i64(household.resources.food_produced_total, produced_food, household.resources.food_produced_total)) {
            throw std::overflow_error("Food produced total overflow");
        }
    }

    std::vector<SettlementId> grouped_settlement_ids;
    grouped_settlement_ids.reserve(household_states_by_settlement.size());
    for (const auto& [settlement_id, grouped_households] : household_states_by_settlement) {
        if (!grouped_households.empty()) {
            grouped_settlement_ids.push_back(settlement_id);
        }
    }
    std::sort(grouped_settlement_ids.begin(), grouped_settlement_ids.end());

    for (const auto settlement_id : grouped_settlement_ids) {
        const auto grouped_it = household_states_by_settlement.find(settlement_id);
        if (grouped_it == household_states_by_settlement.end()) {
            continue;
        }
        const auto& grouped_households = grouped_it->second;

        std::int64_t settlement_required = 0;
        std::int64_t settlement_inventory = 0;
        for (const auto* household_state : grouped_households) {
            if (!checked_add_i64(settlement_required, household_state->required_food, settlement_required) ||
                !checked_add_i64(
                    settlement_inventory,
                    household_state->household->resources.food_inventory.value,
                    settlement_inventory)) {
                throw std::overflow_error("Settlement pooling aggregation overflow");
            }
        }

        const auto settlement_consumed = std::min(settlement_inventory, settlement_required);
        const auto settlement_leftover = std::max<std::int64_t>(0, settlement_inventory - settlement_consumed);

        std::vector<std::int64_t> household_allocated(grouped_households.size(), 0);
        std::vector<std::int64_t> household_leftover(grouped_households.size(), 0);

        std::int64_t distributed_consumed = 0;
        for (std::size_t i = 0; i < grouped_households.size(); ++i) {
            const auto* household_state = grouped_households[i];
            std::int64_t allocated = 0;
            if (settlement_consumed > 0 && settlement_required > 0 && household_state->required_food > 0) {
                allocated = (settlement_consumed * household_state->required_food) / settlement_required;
            }
            household_allocated[i] = allocated;
            if (!checked_add_i64(distributed_consumed, allocated, distributed_consumed)) {
                throw std::overflow_error("Settlement pooled consumed distribution overflow");
            }
        }

        auto consumed_remainder = settlement_consumed - distributed_consumed;
        for (std::size_t i = 0; i < grouped_households.size(); ++i) {
            const auto* household_state = grouped_households[i];
            if (consumed_remainder <= 0) {
                break;
            }
            if (household_state->required_food <= 0) {
                continue;
            }
            ++household_allocated[i];
            --consumed_remainder;
        }

        std::int64_t distributed_leftover = 0;
        for (std::size_t i = 0; i < grouped_households.size(); ++i) {
            const auto* household_state = grouped_households[i];
            std::int64_t leftover = 0;
            if (settlement_leftover > 0 && settlement_inventory > 0 && household_state->household->resources.food_inventory.value > 0) {
                leftover =
                    (settlement_leftover * household_state->household->resources.food_inventory.value) /
                    settlement_inventory;
            }
            household_leftover[i] = leftover;
            if (!checked_add_i64(distributed_leftover, leftover, distributed_leftover)) {
                throw std::overflow_error("Settlement pooled leftover distribution overflow");
            }
        }

        auto leftover_remainder = settlement_leftover - distributed_leftover;
        for (std::size_t i = 0; i < grouped_households.size(); ++i) {
            const auto* household_state = grouped_households[i];
            if (leftover_remainder <= 0) {
                break;
            }
            if (household_state->household->resources.food_inventory.value <= 0) {
                continue;
            }
            ++household_leftover[i];
            --leftover_remainder;
        }

        for (std::size_t i = 0; i < grouped_households.size(); ++i) {
            auto* household_state = grouped_households[i];
            auto& household = *household_state->household;
            const auto consumed_food = household_allocated[i];
            const auto unmet_food = std::max<std::int64_t>(0, household_state->required_food - consumed_food);
            household.resources.food_inventory.value = std::max<std::int64_t>(0, household_leftover[i]);

            if (!checked_add_i64(household.resources.food_consumed_total, consumed_food, household.resources.food_consumed_total) ||
                !checked_add_i64(household.resources.unmet_food_total, unmet_food, household.resources.unmet_food_total)) {
                throw std::overflow_error("Food accounting totals overflow");
            }

            if (unmet_food > 0) {
                ++household.resources.consecutive_shortage_days;

                const auto delta = static_cast<std::int64_t>(shortage_stress_delta_per_day(household_state->required_food, unmet_food));
                std::int64_t new_stress = 0;
                if (!checked_add_i64(household.resources.nutritional_stress_points, delta, new_stress)) {
                    throw std::overflow_error("Food stress overflow");
                }
                household.resources.nutritional_stress_points = std::min<std::int64_t>(new_stress, food_parameters_.max_stress_points);
            } else {
                household.resources.consecutive_shortage_days = 0;
                household.resources.nutritional_stress_points = std::max<std::int64_t>(
                    0,
                    household.resources.nutritional_stress_points - food_parameters_.stress_recovery_per_day);
            }

            std::vector<std::int64_t> per_person_allocated(household_state->alive_members.size(), 0);
            if (consumed_food > 0 && household_state->required_food > 0 && !household_state->alive_members.empty()) {
                std::int64_t allocated_sum = 0;
                for (std::size_t member_index = 0; member_index < household_state->alive_members.size(); ++member_index) {
                    per_person_allocated[member_index] =
                        (consumed_food * household_state->per_person_required[member_index]) /
                        household_state->required_food;
                    if (!checked_add_i64(allocated_sum, per_person_allocated[member_index], allocated_sum)) {
                        throw std::overflow_error("Per-person food allocation overflow");
                    }
                }

                auto remainder = consumed_food - allocated_sum;
                for (std::size_t member_index = 0;
                     member_index < household_state->alive_members.size() && remainder > 0;
                     ++member_index) {
                    ++per_person_allocated[member_index];
                    --remainder;
                }
            }

            for (std::size_t member_index = 0; member_index < household_state->alive_members.size(); ++member_index) {
                apply_person_nutrition_and_health(
                    *household_state->alive_members[member_index],
                    household_state->per_person_required[member_index],
                    per_person_allocated[member_index]);
            }
        }
    }

    if (config_.materials_system_enabled) {
        process_materials_period(food_period);
        process_local_exchange(current_demographic_year());
    }

    process_household_stabilization_transfers();
    process_aggregate_food_period(food_period);

    refresh_food_aggregates();
    refresh_environment_aggregates();
    refresh_material_aggregates();
    refresh_economic_aggregates();
    advance_settlement_daily_counters();
}

// Ticket #37: re-derives every detailed-resolution alive person's
// current_activity for today, deterministically from person_seed +
// SeedDomain::Schedule + food_period. Cohort aggregates carry no Person
// struct at all, so they are naturally excluded -- consistent with the
// existing LOD demotion philosophy of dropping detailed-only state.
void Simulation::process_schedule_period(const std::uint64_t food_period) {
    for (auto& person : state_.people) {
        if (!person.alive) {
            continue;
        }
        const auto schedule_seed = person_seed(person.id);
        person.current_activity = derive_daily_activity(schedule_seed, food_period, person.occupation);

        if (person.current_activity == ActivityKind::Rest) {
            person.health_points = clamp_i32(
                person.health_points + 2,
                food_parameters_.min_person_health_points,
                food_parameters_.max_person_health_points);
        }
    }
}

void Simulation::process_materials_period(const std::uint64_t food_period) {
    state_.wood_labour_available = 0;
    state_.wood_labour_allocated = 0;
    state_.stone_labour_available = 0;
    state_.stone_labour_allocated = 0;
    state_.craft_labour_available = 0;
    state_.craft_labour_allocated = 0;
    state_.material_labour_demand = 0;
    state_.material_labour_allocated = 0;
    state_.material_labour_shortfall = 0;
    state_.material_production_shortfall = 0;

    struct SettlementMaterialDayState {
        Settlement* settlement{};
        std::uint64_t alive_population{};
        std::uint64_t eligible_workers{};
        std::int64_t wood_labour_available{};
        std::int64_t stone_labour_available{};
        std::int64_t craft_labour_available{};
        std::int64_t wood_quota{};
        std::int64_t stone_quota{};
    };

    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<SettlementId, SettlementMaterialDayState> by_settlement;
    by_settlement.reserve(state_.settlements.size());
    for (auto& settlement : state_.settlements) {
        if (!settlement.active) {
            continue;
        }

        by_settlement.emplace(
            settlement.id,
            SettlementMaterialDayState{.settlement = &settlement});
    }

    for (const auto& household : state_.households) {
        auto settlement_it = by_settlement.find(household.settlement_id);
        if (settlement_it == by_settlement.end()) {
            continue;
        }

        auto& settlement_state = settlement_it->second;
        for (const auto member_id : household.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it == person_by_id.end() || !person_it->second->alive) {
                continue;
            }

            ++settlement_state.alive_population;
            const auto& person = *person_it->second;
            if (!occupation_eligible_for_productive_work(person)) {
                continue;
            }

            ++settlement_state.eligible_workers;
            const auto capacity = productive_effort_for_person(person);
            if (person.occupation == Occupation::WoodGatherer) {
                if (!checked_add_i64(
                        settlement_state.wood_labour_available,
                        capacity,
                        settlement_state.wood_labour_available)) {
                    throw std::overflow_error("Wood labour availability overflow");
                }
            } else if (person.occupation == Occupation::StoneExtractor) {
                if (!checked_add_i64(
                        settlement_state.stone_labour_available,
                        capacity,
                        settlement_state.stone_labour_available)) {
                    throw std::overflow_error("Stone labour availability overflow");
                }
            } else if (person.occupation == Occupation::CraftWorker) {
                if (!checked_add_i64(
                        settlement_state.craft_labour_available,
                        capacity,
                        settlement_state.craft_labour_available)) {
                    throw std::overflow_error("Craft labour availability overflow");
                }
            }
        }
    }

    std::vector<RegionId> region_ids;
    region_ids.reserve(state_.regions.size());
    for (const auto& region : state_.regions) {
        region_ids.push_back(region.id);
    }
    std::sort(region_ids.begin(), region_ids.end());

    for (const auto region_id : region_ids) {
        auto* region = find_region_mutable(region_id);
        if (region == nullptr) {
            continue;
        }

        std::vector<SettlementMaterialDayState*> active_settlements;
        active_settlements.reserve(by_settlement.size());
        std::int64_t total_share = 0;
        for (auto& [settlement_id, settlement_state] : by_settlement) {
            (void)settlement_id;
            if (settlement_state.settlement->region_id != region_id || !settlement_state.settlement->active ||
                settlement_state.settlement->access_share_points == 0U) {
                continue;
            }

            active_settlements.push_back(&settlement_state);
            total_share += settlement_state.settlement->access_share_points;
        }

        std::sort(active_settlements.begin(), active_settlements.end(), [](const SettlementMaterialDayState* lhs, const SettlementMaterialDayState* rhs) {
            return lhs->settlement->id < rhs->settlement->id;
        });

        if (active_settlements.empty() || total_share <= 0) {
            continue;
        }

        auto wood_available = std::max<std::int64_t>(0, region->wood_stock);
        auto stone_available = std::max<std::int64_t>(0, region->stone_remaining);

        const auto wood_daily_seed = derive_seed(region_seed(region_id), SeedDomain::RegionWoodDailyVariation, food_period);
        const auto stone_daily_seed = derive_seed(region_seed(region_id), SeedDomain::RegionStoneDailyVariation, food_period);
        wood_available = apply_scaled_percent(
            wood_available,
            85U + static_cast<std::uint32_t>(seed_random_bounded(wood_daily_seed, 31U)));
        stone_available = apply_scaled_percent(
            stone_available,
            85U + static_cast<std::uint32_t>(seed_random_bounded(stone_daily_seed, 31U)));
        wood_available = std::min(wood_available, region->wood_stock);
        stone_available = std::min(stone_available, region->stone_remaining);

        std::int64_t distributed_wood_quota = 0;
        std::int64_t distributed_stone_quota = 0;
        for (auto* settlement_state : active_settlements) {
            settlement_state->wood_quota =
                (wood_available * static_cast<std::int64_t>(settlement_state->settlement->access_share_points)) /
                static_cast<std::int64_t>(kSettlementMaterialShareScale);
            settlement_state->stone_quota =
                (stone_available * static_cast<std::int64_t>(settlement_state->settlement->access_share_points)) /
                static_cast<std::int64_t>(kSettlementMaterialShareScale);
            distributed_wood_quota += settlement_state->wood_quota;
            distributed_stone_quota += settlement_state->stone_quota;
        }

        auto wood_quota_remainder = wood_available - distributed_wood_quota;
        auto stone_quota_remainder = stone_available - distributed_stone_quota;
        for (auto* settlement_state : active_settlements) {
            if (wood_quota_remainder > 0) {
                ++settlement_state->wood_quota;
                --wood_quota_remainder;
            }
            if (stone_quota_remainder > 0) {
                ++settlement_state->stone_quota;
                --stone_quota_remainder;
            }
        }

        std::int64_t extracted_wood_region = 0;
        std::int64_t extracted_stone_region = 0;

        for (auto* settlement_state : active_settlements) {
            const auto targets = material_targets_for_settlement(
                settlement_state->alive_population,
                settlement_state->eligible_workers);
            const auto construction_demand =
                construction_demand_for_settlement(settlement_state->settlement->id, state_.buildings);

            auto& inventory = settlement_state->settlement->materials;
            std::int64_t timber_target_with_construction = 0;
            if (!checked_add_i64(targets.timber_target, construction_demand.timber_remaining, timber_target_with_construction)) {
                throw std::overflow_error("Timber target with construction overflow");
            }
            std::int64_t stone_target_with_construction = 0;
            if (!checked_add_i64(targets.stone_target, construction_demand.stone_remaining, stone_target_with_construction)) {
                throw std::overflow_error("Stone target with construction overflow");
            }
            const auto timber_deficit = std::max<std::int64_t>(0, timber_target_with_construction - inventory.timber.value);
            const auto tool_deficit = std::max<std::int64_t>(0, targets.basic_tool_target - inventory.basic_tool.value);
            const auto stone_deficit = std::max<std::int64_t>(0, stone_target_with_construction - inventory.stone.value);

            std::int64_t timber_wood_recipe_need = 0;
            std::int64_t tool_wood_recipe_need = 0;
            if (!checked_mul_i64(timber_deficit, material_parameters_.wood_per_timber, timber_wood_recipe_need) ||
                !checked_mul_i64(tool_deficit, material_parameters_.wood_per_basic_tool, tool_wood_recipe_need)) {
                throw std::overflow_error("Wood recipe need overflow");
            }
            std::int64_t wood_recipe_need = 0;
            if (!checked_add_i64(timber_wood_recipe_need, tool_wood_recipe_need, wood_recipe_need)) {
                throw std::overflow_error("Wood recipe need aggregation overflow");
            }

            std::int64_t stone_recipe_need = 0;
            if (!checked_mul_i64(tool_deficit, material_parameters_.stone_per_basic_tool, stone_recipe_need)) {
                throw std::overflow_error("Stone recipe need overflow");
            }

            const auto wood_reserve_need = std::max<std::int64_t>(0, targets.wood_target - inventory.wood.value);
            const auto stone_reserve_need = stone_deficit;
            std::int64_t wood_total_need = 0;
            if (!checked_add_i64(wood_recipe_need, wood_reserve_need, wood_total_need)) {
                throw std::overflow_error("Wood total need overflow");
            }
            std::int64_t stone_total_need = 0;
            if (!checked_add_i64(stone_recipe_need, stone_reserve_need, stone_total_need)) {
                throw std::overflow_error("Stone total need overflow");
            }

            const auto output_scale_percent =
                tool_output_scale_percent(settlement_state->settlement->last_tool_coverage_per_10k);
            const auto effective_wood_labour_available = apply_scaled_percent(
                std::max<std::int64_t>(0, settlement_state->wood_labour_available),
                output_scale_percent);
            const auto effective_stone_labour_available = apply_scaled_percent(
                std::max<std::int64_t>(0, settlement_state->stone_labour_available),
                output_scale_percent);

            const auto extracted_wood = safe_min3_i64(
                std::max<std::int64_t>(0, wood_total_need),
                std::max<std::int64_t>(0, settlement_state->wood_quota),
                effective_wood_labour_available);
            const auto extracted_stone = safe_min3_i64(
                std::max<std::int64_t>(0, stone_total_need),
                std::max<std::int64_t>(0, settlement_state->stone_quota),
                effective_stone_labour_available);

            if (!checked_sub_nonnegative_i64(region->wood_stock, extracted_wood, region->wood_stock) ||
                !checked_sub_nonnegative_i64(region->stone_remaining, extracted_stone, region->stone_remaining)) {
                throw std::overflow_error("Material extraction exceeded region stock");
            }

            if (!checked_add_i64(region->cumulative_wood_harvested, extracted_wood, region->cumulative_wood_harvested) ||
                !checked_add_i64(region->cumulative_stone_extracted, extracted_stone, region->cumulative_stone_extracted) ||
                !checked_add_i64(inventory.wood.value, extracted_wood, inventory.wood.value) ||
                !checked_add_i64(inventory.stone.value, extracted_stone, inventory.stone.value) ||
                !checked_add_i64(
                    settlement_state->settlement->cumulative_wood_extracted,
                    extracted_wood,
                    settlement_state->settlement->cumulative_wood_extracted) ||
                !checked_add_i64(
                    settlement_state->settlement->cumulative_stone_extracted,
                    extracted_stone,
                    settlement_state->settlement->cumulative_stone_extracted)) {
                throw std::overflow_error("Material extraction accounting overflow");
            }

            if (!checked_add_i64(extracted_wood_region, extracted_wood, extracted_wood_region) ||
                !checked_add_i64(extracted_stone_region, extracted_stone, extracted_stone_region) ||
                !checked_add_i64(state_.wood_labour_allocated, extracted_wood, state_.wood_labour_allocated) ||
                !checked_add_i64(state_.stone_labour_allocated, extracted_stone, state_.stone_labour_allocated)) {
                throw std::overflow_error("Material labour accounting overflow");
            }

            auto craft_labour_remaining = apply_scaled_percent(
                std::max<std::int64_t>(0, settlement_state->craft_labour_available),
                config_.material_crafting_scale_percent);
            craft_labour_remaining = apply_scaled_percent(craft_labour_remaining, output_scale_percent);
            if (!checked_add_i64(
                    craft_labour_remaining,
                    std::max<std::int64_t>(0, settlement_state->settlement->workshop_capacity_bonus),
                    craft_labour_remaining)) {
                throw std::overflow_error("Workshop bonus labour overflow");
            }

            const auto craft_demand_total = timber_deficit + tool_deficit;
            std::int64_t material_labour_increment = 0;
            if (!checked_add_i64(craft_demand_total, extracted_wood, material_labour_increment) ||
                !checked_add_i64(material_labour_increment, extracted_stone, material_labour_increment) ||
                !checked_add_i64(
                    state_.material_labour_demand,
                    material_labour_increment,
                    state_.material_labour_demand)) {
                throw std::overflow_error("Material labour demand overflow");
            }

            const auto tools_possible = safe_min3_i64(
                tool_deficit,
                craft_labour_remaining / std::max<std::int64_t>(1, material_parameters_.craft_labour_per_basic_tool),
                safe_min3_i64(
                    inventory.wood.value / std::max<std::int64_t>(1, material_parameters_.wood_per_basic_tool),
                    inventory.stone.value / std::max<std::int64_t>(1, material_parameters_.stone_per_basic_tool),
                    std::numeric_limits<std::int64_t>::max()));

            if (tools_possible > 0) {
                inventory.wood.value -= tools_possible * material_parameters_.wood_per_basic_tool;
                inventory.stone.value -= tools_possible * material_parameters_.stone_per_basic_tool;
                if (!checked_add_i64(inventory.basic_tool.value, tools_possible, inventory.basic_tool.value)) {
                    throw std::overflow_error("Basic tool inventory overflow");
                }
                if (!checked_add_i64(
                        settlement_state->settlement->cumulative_basic_tool_produced,
                        tools_possible,
                        settlement_state->settlement->cumulative_basic_tool_produced)) {
                    throw std::overflow_error("Basic tool cumulative production overflow");
                }
                craft_labour_remaining -= tools_possible * material_parameters_.craft_labour_per_basic_tool;
            }

            const auto timber_possible = safe_min3_i64(
                timber_deficit,
                craft_labour_remaining / std::max<std::int64_t>(1, material_parameters_.craft_labour_per_timber),
                inventory.wood.value / std::max<std::int64_t>(1, material_parameters_.wood_per_timber));

            if (timber_possible > 0) {
                inventory.wood.value -= timber_possible * material_parameters_.wood_per_timber;
                if (!checked_add_i64(inventory.timber.value, timber_possible, inventory.timber.value)) {
                    throw std::overflow_error("Timber inventory overflow");
                }
                if (!checked_add_i64(
                        settlement_state->settlement->cumulative_timber_produced,
                        timber_possible,
                        settlement_state->settlement->cumulative_timber_produced)) {
                    throw std::overflow_error("Timber cumulative production overflow");
                }
                craft_labour_remaining -= timber_possible * material_parameters_.craft_labour_per_timber;
            }

            const auto used_craft_labour =
                (tools_possible * material_parameters_.craft_labour_per_basic_tool) +
                (timber_possible * material_parameters_.craft_labour_per_timber);
            if (!checked_add_i64(
                    state_.craft_labour_allocated,
                    used_craft_labour,
                    state_.craft_labour_allocated)) {
                throw std::overflow_error("Craft labour allocation overflow");
            }

            const auto production_shortfall =
                std::max<std::int64_t>(0, tool_deficit - tools_possible) +
                std::max<std::int64_t>(0, timber_deficit - timber_possible);
            if (!checked_add_i64(
                    state_.material_production_shortfall,
                    production_shortfall,
                    state_.material_production_shortfall)) {
                throw std::overflow_error("Material production shortfall overflow");
            }
        }

        if (region->wood_stock < region->wood_capacity) {
            const auto deficit = region->wood_capacity - region->wood_stock;
            const auto regeneration = std::min(deficit, std::max<std::int64_t>(0, region->wood_regeneration_per_day));
            if (!checked_add_i64(region->wood_stock, regeneration, region->wood_stock) ||
                !checked_add_i64(region->cumulative_wood_regenerated, regeneration, region->cumulative_wood_regenerated)) {
                throw std::overflow_error("Wood regeneration overflow");
            }
        }

        (void)extracted_wood_region;
        (void)extracted_stone_region;
    }

    for (const auto& [settlement_id, settlement_state] : by_settlement) {
        (void)settlement_id;
        if (!checked_add_i64(
                state_.wood_labour_available,
                settlement_state.wood_labour_available,
                state_.wood_labour_available) ||
            !checked_add_i64(
                state_.stone_labour_available,
                settlement_state.stone_labour_available,
                state_.stone_labour_available) ||
            !checked_add_i64(
                state_.craft_labour_available,
                settlement_state.craft_labour_available,
                state_.craft_labour_available)) {
            throw std::overflow_error("Material labour availability aggregate overflow");
        }
    }

    state_.material_labour_allocated =
        state_.wood_labour_allocated + state_.stone_labour_allocated + state_.craft_labour_allocated;
    state_.material_labour_shortfall =
        std::max<std::int64_t>(0, state_.material_labour_demand - state_.material_labour_allocated);
}

void Simulation::process_demography_period(const std::uint64_t demographic_year) {
    process_mortality(demographic_year);
    process_births(demographic_year);
    process_settlement_restructuring(demographic_year);
    process_aggregate_demography_period(demographic_year);
    process_basic_discoveries(demographic_year);
    process_communication_teaching(demographic_year);
    process_records(demographic_year);
    process_institutions(demographic_year);
    process_knowledge_loss(demographic_year);
    process_household_access_allocation();
    process_local_exchange(demographic_year);
    process_construction(demographic_year);
    process_local_exchange(demographic_year);

    std::unordered_map<PersonId, Occupation> occupations_before;
    occupations_before.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        occupations_before.emplace(person.id, person.occupation);
    }
    process_occupation_reassignment(demographic_year);
    process_merchant_recruitment(demographic_year);
    record_occupation_changes(occupations_before);

    process_household_access_allocation();
    refresh_population_count();
    refresh_food_aggregates();
    refresh_environment_aggregates();
    refresh_labour_aggregates();
    refresh_material_aggregates();
    refresh_economic_aggregates();
}

void Simulation::process_aggregate_food_period(const std::uint64_t food_period) {
    // Chunked monthly advancement: one aggregate step covers kAggregateFoodPeriodDays
    // of flow, so off-screen population costs ~1/30th of daily execution.
    if (food_period % kAggregateFoodPeriodDays != 0U) {
        return;
    }
    const auto chunk_days = static_cast<std::int64_t>(kAggregateFoodPeriodDays);

    std::unordered_map<SettlementId, SettlementAggregateState*> aggregate_by_settlement;
    aggregate_by_settlement.reserve(state_.settlement_aggregates.size());
    for (auto& aggregate : state_.settlement_aggregates) {
        aggregate_by_settlement.emplace(aggregate.settlement_id, &aggregate);
    }

    std::unordered_map<SettlementId, std::uint64_t> population_by_settlement;
    population_by_settlement.reserve(state_.settlements.size());
    for (const auto& cohort : state_.population_cohorts) {
        if (!is_region_detailed(cohort.region_id)) {
            population_by_settlement[cohort.settlement_id] += cohort.population;
        }
    }

    for (auto& region : state_.regions) {
        std::vector<Settlement*> aggregate_settlements;
        aggregate_settlements.reserve(state_.settlements.size());
        for (auto& settlement : state_.settlements) {
            if (!settlement.active || settlement.region_id != region.id || is_settlement_detailed(settlement.id) || settlement.access_share_points == 0U) {
                continue;
            }
            aggregate_settlements.push_back(&settlement);
        }

        std::sort(aggregate_settlements.begin(), aggregate_settlements.end(), [](const Settlement* lhs, const Settlement* rhs) {
            return lhs->id < rhs->id;
        });

        if (aggregate_settlements.empty()) {
            continue;
        }

        auto productive_available = apply_scaled_percent(region.productive_capacity_per_day, config_.food_production_scale_percent);
        const auto productive_seed = derive_seed(region_seed(region.id), SeedDomain::RegionDailyVariation, food_period);
        const auto productive_variation_percent = 85U + static_cast<std::uint32_t>(seed_random_bounded(productive_seed, 31U));
        productive_available = apply_scaled_percent(productive_available, productive_variation_percent);
        productive_available *= chunk_days;

        auto wild_harvest_available = std::min(region.wild_food_stock, region.wild_food_harvest_limit_per_day * chunk_days);
        const auto wild_seed = derive_seed(region_seed(region.id), SeedDomain::RegionWildHarvest, food_period);
        const auto wild_variation_percent = 80U + static_cast<std::uint32_t>(seed_random_bounded(wild_seed, 41U));
        wild_harvest_available = apply_scaled_percent(wild_harvest_available, wild_variation_percent);
        wild_harvest_available = std::min(wild_harvest_available, region.wild_food_stock);

        auto productive_remaining = std::max<std::int64_t>(0, productive_available);
        auto wild_remaining = std::max<std::int64_t>(0, wild_harvest_available);

        for (auto* settlement : aggregate_settlements) {
            const auto aggregate_it = aggregate_by_settlement.find(settlement->id);
            if (aggregate_it == aggregate_by_settlement.end()) {
                continue;
            }
            auto* aggregate = aggregate_it->second;
            const auto population = population_by_settlement.contains(settlement->id)
                ? population_by_settlement[settlement->id]
                : 0U;

            aggregate->region_id = settlement->region_id;
            aggregate->household_count = ceil_div_u64(population, kAggregatePeoplePerHousehold);
            aggregate->small_household_count = aggregate->household_count / 3U;
            aggregate->medium_household_count = aggregate->household_count / 3U;
            aggregate->large_household_count =
                aggregate->household_count - aggregate->small_household_count - aggregate->medium_household_count;

            if (population == 0U) {
                aggregate->labour_capacity = 0;
                aggregate->labour_demand = 0;
                aggregate->labour_allocated = 0;
                continue;
            }

            const auto eligible_workers = (population * kAggregateEligibleWorkersPer10k) / 10'000U;
            const auto cultivators = (eligible_workers * kAggregateCultivatorSharePer10k) / 10'000U;
            const auto gatherers = (eligible_workers * kAggregateGathererSharePer10k) / 10'000U;
            const auto wood_workers = (eligible_workers * kAggregateWoodSharePer10k) / 10'000U;
            const auto stone_workers = (eligible_workers * kAggregateStoneSharePer10k) / 10'000U;
            const auto craft_workers = (eligible_workers * kAggregateCraftSharePer10k) / 10'000U;

            // Workforce diagnostics are published once per year by
            // refresh_labour_aggregates, not accumulated on the food cadence.
            (void)cultivators;
            (void)gatherers;

            const auto labour_capacity = static_cast<std::int64_t>(eligible_workers) * 6 * chunk_days;
            aggregate->labour_capacity = labour_capacity;

            auto settlement_productive_budget =
                (productive_available * static_cast<std::int64_t>(settlement->access_share_points)) /
                static_cast<std::int64_t>(kSettlementShareScale);
            auto settlement_wild_budget =
                (wild_harvest_available * static_cast<std::int64_t>(settlement->access_share_points)) /
                static_cast<std::int64_t>(kSettlementShareScale);

            settlement_productive_budget = std::min(std::max<std::int64_t>(0, settlement_productive_budget), productive_remaining);
            settlement_wild_budget = std::min(std::max<std::int64_t>(0, settlement_wild_budget), wild_remaining);

            const auto required_food = static_cast<std::int64_t>(population) * 2 * chunk_days;
            const auto output_target = compute_daily_food_output_target(
                required_food,
                aggregate->food_inventory,
                settlement_productive_budget + settlement_wild_budget);
            const auto produced_food = std::min<std::int64_t>(output_target, labour_capacity);
            const auto available_food = aggregate->food_inventory + produced_food;
            const auto consumed_food = std::min<std::int64_t>(required_food, std::max<std::int64_t>(0, available_food));
            const auto unmet_food = std::max<std::int64_t>(0, required_food - consumed_food);
            aggregate->food_inventory = std::max<std::int64_t>(0, available_food - consumed_food);
            aggregate->food_produced_total += produced_food;
            aggregate->food_consumed_total += consumed_food;
            aggregate->unmet_food_total += unmet_food;
            // Rolling yearly window consumed by aggregate demography.
            aggregate->required_food_year += required_food;
            aggregate->unmet_food_year += unmet_food;

            ++aggregate_settlement_update_count_;

            aggregate->labour_demand = std::min<std::int64_t>(required_food, settlement_productive_budget + settlement_wild_budget);
            aggregate->labour_allocated = std::min(aggregate->labour_demand, labour_capacity);

            productive_remaining = std::max<std::int64_t>(0, productive_remaining - settlement_productive_budget);
            wild_remaining = std::max<std::int64_t>(0, wild_remaining - settlement_wild_budget);

            const auto wood_extract_quota = std::min<std::int64_t>(
                std::max<std::int64_t>(0, region.wood_stock),
                static_cast<std::int64_t>(wood_workers) * 2 * chunk_days);
            const auto stone_extract_quota = std::min<std::int64_t>(
                std::max<std::int64_t>(0, region.stone_remaining),
                static_cast<std::int64_t>(stone_workers) * 2 * chunk_days);

            settlement->materials.wood.value += wood_extract_quota;
            settlement->materials.stone.value += stone_extract_quota;
            settlement->cumulative_wood_extracted += wood_extract_quota;
            settlement->cumulative_stone_extracted += stone_extract_quota;

            region.wood_stock = std::max<std::int64_t>(0, region.wood_stock - wood_extract_quota);
            region.stone_remaining = std::max<std::int64_t>(0, region.stone_remaining - stone_extract_quota);
            region.cumulative_wood_harvested += wood_extract_quota;
            region.cumulative_stone_extracted += stone_extract_quota;

            const auto timber_produced = std::min<std::int64_t>(
                settlement->materials.wood.value / 2,
                static_cast<std::int64_t>(craft_workers) * chunk_days);
            const auto tools_produced = std::min<std::int64_t>(
                std::min<std::int64_t>(settlement->materials.timber.value, settlement->materials.stone.value),
                static_cast<std::int64_t>(craft_workers / 2) * chunk_days);

            settlement->materials.wood.value -= timber_produced * 2;
            settlement->materials.timber.value += timber_produced;
            settlement->materials.timber.value -= tools_produced;
            settlement->materials.stone.value -= tools_produced;
            settlement->materials.basic_tool.value += tools_produced;
            settlement->cumulative_timber_produced += timber_produced;
            settlement->cumulative_basic_tool_produced += tools_produced;
        }

        if (region.wood_stock < region.wood_capacity) {
            const auto deficit = region.wood_capacity - region.wood_stock;
            const auto regeneration =
                std::min(deficit, std::max<std::int64_t>(0, region.wood_regeneration_per_day) * chunk_days);
            region.wood_stock += regeneration;
            region.cumulative_wood_regenerated += regeneration;
        }

        const auto distributed_wild = std::max<std::int64_t>(0, wild_harvest_available - wild_remaining);
        region.wild_food_stock = std::max<std::int64_t>(0, region.wild_food_stock - distributed_wild);
        region.cumulative_wild_food_harvested += distributed_wild;
        if (region.wild_food_stock < region.wild_food_capacity) {
            const auto deficit = region.wild_food_capacity - region.wild_food_stock;
            const auto regeneration =
                std::min(deficit, std::max<std::int64_t>(0, region.wild_food_regeneration_per_day) * chunk_days);
            region.wild_food_stock += regeneration;
            region.cumulative_wild_food_regenerated += regeneration;
        }
    }

}

namespace {

// Band-averaged demographic rates derived from the canonical per-age tables in
// mortality_rate_per_10k / fertility_rate_per_10k, averaged over each band's
// age span so aggregate execution reproduces detailed demography in the mean.
[[nodiscard]] std::uint32_t cohort_mortality_rate_per_10k(const CohortAgeBand band) noexcept {
    switch (band) {
        case CohortAgeBand::Child:      return 40U;   // ages 0-17
        case CohortAgeBand::YoungAdult: return 20U;   // ages 18-29
        case CohortAgeBand::Adult:      return 33U;   // ages 30-59
        case CohortAgeBand::Senior:     return 790U;  // ages 60+
    }
    return 20U;
}

[[nodiscard]] std::uint32_t cohort_fertility_rate_per_10k(const CohortAgeBand band) noexcept {
    switch (band) {
        case CohortAgeBand::YoungAdult: return 2'450U;  // ages 18-29
        case CohortAgeBand::Adult:      return 797U;    // ages 30-59, zero past 49
        case CohortAgeBand::Child:
        case CohortAgeBand::Senior:
            return 0U;
    }
    return 0U;
}

// Years spent in each band; drives the annual aging-out fraction.
[[nodiscard]] std::uint64_t cohort_band_span_years(const CohortAgeBand band) noexcept {
    switch (band) {
        case CohortAgeBand::Child:      return 18U;
        case CohortAgeBand::YoungAdult: return 12U;
        case CohortAgeBand::Adult:      return 30U;
        case CohortAgeBand::Senior:     return 0U;  // seniors exit via mortality only
    }
    return 0U;
}

[[nodiscard]] std::uint32_t cohort_health_mortality_modifier_per_10k(const CohortHealthBand band) noexcept {
    switch (band) {
        case CohortHealthBand::Low:    return 150U;
        case CohortHealthBand::Medium: return 0U;
        case CohortHealthBand::High:   return 0U;
    }
    return 0U;
}

// Deterministic proportional draw: exact integer part plus a seeded carry for
// the fractional remainder, so repeated runs match and no rounding bias
// accumulates across years.
[[nodiscard]] std::uint64_t deterministic_share(
    const std::uint64_t population,
    const std::uint64_t numerator,
    const std::uint64_t denominator,
    const SimulationSeed seed) noexcept {
    if (population == 0U || numerator == 0U || denominator == 0U) {
        return 0U;
    }
    const auto scaled = population * numerator;
    auto count = scaled / denominator;
    const auto remainder = scaled % denominator;
    if (remainder > 0U && seed_random_bounded(seed, denominator) < remainder) {
        ++count;
    }
    return std::min(count, population);
}

}  // namespace

std::unordered_set<PersonId> Simulation::collect_canonized_person_ids() const {
    // Entities carrying durable provenance cannot be collapsed into anonymous
    // cohorts: their exact identity is referenced by history that must survive.
    std::unordered_set<PersonId> canonized;

    for (const auto& institution : state_.institutions) {
        canonized.insert(institution.founder_id);
        for (const auto member_id : institution.member_ids) {
            canonized.insert(member_id);
        }
    }

    for (const auto& record : state_.knowledge_records) {
        canonized.insert(record.author_id);
    }

    for (const auto& event : state_.event_log) {
        if (event.type == EventType::DiscoveryMade) {
            const auto& payload = std::get<DiscoveryMadeEventPayload>(event.payload);
            canonized.insert(payload.discoverer_id);
        } else if (event.type == EventType::InstitutionFounded) {
            const auto& payload = std::get<InstitutionFoundedEventPayload>(event.payload);
            canonized.insert(payload.founder_id);
        }
    }

    canonized.erase(0U);
    return canonized;
}

void Simulation::merge_population_cohorts() {
    struct CohortKey {
        RegionId region_id{};
        SettlementId settlement_id{};
        CohortAgeBand age_band{};
        BiologicalSex sex{};
        CohortOccupationGroup occupation_group{};
        CohortHealthBand health_band{};
        CohortWealthBand wealth_band{};
        std::uint32_t knowledge_profile_id{};

        [[nodiscard]] bool operator==(const CohortKey& other) const = default;
    };

    const auto key_of = [](const PopulationCohort& cohort) {
        return CohortKey{
            .region_id = cohort.region_id,
            .settlement_id = cohort.settlement_id,
            .age_band = cohort.age_band,
            .sex = cohort.sex,
            .occupation_group = cohort.occupation_group,
            .health_band = cohort.health_band,
            .wealth_band = cohort.wealth_band,
            .knowledge_profile_id = cohort.knowledge_profile_id,
        };
    };

    // Canonical ordering first, so the surviving identity for each key is a
    // deterministic function of world state rather than of insertion order.
    std::sort(state_.population_cohorts.begin(), state_.population_cohorts.end(), [](const PopulationCohort& lhs, const PopulationCohort& rhs) {
        if (lhs.region_id != rhs.region_id) {
            return lhs.region_id < rhs.region_id;
        }
        if (lhs.settlement_id != rhs.settlement_id) {
            return lhs.settlement_id < rhs.settlement_id;
        }
        return lhs.id < rhs.id;
    });

    std::vector<PopulationCohort> merged;
    merged.reserve(state_.population_cohorts.size());
    for (const auto& cohort : state_.population_cohorts) {
        if (cohort.population == 0U) {
            continue;
        }

        // Cohorts are sorted by (region, settlement, id), so a matching key can
        // only be in the trailing run for the same settlement. Scanning just
        // that run keeps merging linear in world size rather than quadratic.
        PopulationCohort* existing = nullptr;
        for (auto it = merged.rbegin(); it != merged.rend(); ++it) {
            if (it->region_id != cohort.region_id || it->settlement_id != cohort.settlement_id) {
                break;
            }
            if (key_of(*it) == key_of(cohort)) {
                existing = &(*it);
                break;
            }
        }

        if (existing == nullptr) {
            merged.push_back(cohort);
            continue;
        }

        existing->population += cohort.population;
        // The lowest cohort id for a key survives: stable and order-independent.
        existing->id = std::min(existing->id, cohort.id);
    }

    state_.population_cohorts = std::move(merged);
}

void Simulation::process_aggregate_cohort_aging(const std::uint64_t demographic_year) {
    std::vector<PopulationCohort> aged_out;
    aged_out.reserve(state_.population_cohorts.size());

    for (auto& cohort : state_.population_cohorts) {
        if (is_region_detailed(cohort.region_id) || cohort.population == 0U) {
            continue;
        }

        const auto span = cohort_band_span_years(cohort.age_band);
        if (span == 0U) {
            continue;
        }

        const auto seed = derive_seed(
            settlement_seed(cohort.settlement_id),
            SeedDomain::CohortAging,
            cohort.id ^ (demographic_year * 0x9E3779B97F4A7C15ULL));
        const auto matured = deterministic_share(cohort.population, 1U, span, seed);
        if (matured == 0U) {
            continue;
        }

        cohort.population -= matured;

        auto next = cohort;
        next.population = matured;
        next.id = next_cohort_id_++;
        switch (cohort.age_band) {
            case CohortAgeBand::Child:
                next.age_band = CohortAgeBand::YoungAdult;
                // Children take up work when they reach working age.
                next.occupation_group = CohortOccupationGroup::Food;
                break;
            case CohortAgeBand::YoungAdult:
                next.age_band = CohortAgeBand::Adult;
                break;
            case CohortAgeBand::Adult:
                next.age_band = CohortAgeBand::Senior;
                next.occupation_group = CohortOccupationGroup::Unspecified;
                break;
            case CohortAgeBand::Senior:
                break;
        }
        aged_out.push_back(next);
    }

    for (const auto& cohort : aged_out) {
        state_.population_cohorts.push_back(cohort);
    }
}

void Simulation::process_aggregate_cohort_mortality(const std::uint64_t demographic_year) {
    std::unordered_map<SettlementId, const SettlementAggregateState*> aggregate_by_settlement;
    aggregate_by_settlement.reserve(state_.settlement_aggregates.size());
    for (const auto& aggregate : state_.settlement_aggregates) {
        aggregate_by_settlement.emplace(aggregate.settlement_id, &aggregate);
    }

    for (auto& cohort : state_.population_cohorts) {
        if (is_region_detailed(cohort.region_id) || cohort.population == 0U) {
            continue;
        }

        ++aggregate_cohort_update_count_;

        auto rate = cohort_mortality_rate_per_10k(cohort.age_band) +
                    cohort_health_mortality_modifier_per_10k(cohort.health_band);

        // Food scarcity raises mortality, mirroring the detailed nutrition path.
        const auto aggregate_it = aggregate_by_settlement.find(cohort.settlement_id);
        if (aggregate_it != aggregate_by_settlement.end()) {
            const auto* aggregate = aggregate_it->second;
            if (aggregate->required_food_year > 0 && aggregate->unmet_food_year > 0) {
                const auto stress_per_10k = static_cast<std::uint64_t>(
                    (aggregate->unmet_food_year * 10'000) / aggregate->required_food_year);
                rate += static_cast<std::uint32_t>(std::min<std::uint64_t>(stress_per_10k * 2U, 6'000U));
            }
        }

        rate = std::min<std::uint32_t>(rate, 10'000U);

        const auto seed = derive_seed(
            settlement_seed(cohort.settlement_id),
            SeedDomain::CohortMortality,
            cohort.id ^ (demographic_year * 0xC2B2AE3D27D4EB4FULL));
        const auto deaths = deterministic_share(cohort.population, rate, 10'000U, seed);
        if (deaths == 0U) {
            continue;
        }

        cohort.population -= deaths;
        state_.total_deaths += deaths;
        state_.aggregate_deaths_total += deaths;
    }
}

void Simulation::process_aggregate_cohort_fertility(const std::uint64_t demographic_year) {
    std::unordered_map<SettlementId, const SettlementAggregateState*> aggregate_by_settlement;
    aggregate_by_settlement.reserve(state_.settlement_aggregates.size());
    for (const auto& aggregate : state_.settlement_aggregates) {
        aggregate_by_settlement.emplace(aggregate.settlement_id, &aggregate);
    }

    // A settlement needs adult males present for births, matching the detailed
    // rule that a father must exist.
    std::unordered_map<SettlementId, std::uint64_t> adult_males_by_settlement;
    for (const auto& cohort : state_.population_cohorts) {
        if (is_region_detailed(cohort.region_id) || cohort.sex != BiologicalSex::Male) {
            continue;
        }
        if (cohort.age_band == CohortAgeBand::YoungAdult || cohort.age_band == CohortAgeBand::Adult) {
            adult_males_by_settlement[cohort.settlement_id] += cohort.population;
        }
    }

    std::vector<PopulationCohort> newborn_cohorts;
    for (const auto& cohort : state_.population_cohorts) {
        if (is_region_detailed(cohort.region_id) || cohort.population == 0U) {
            continue;
        }
        if (cohort.sex != BiologicalSex::Female) {
            continue;
        }

        auto rate = cohort_fertility_rate_per_10k(cohort.age_band);
        if (rate == 0U) {
            continue;
        }

        const auto males_it = adult_males_by_settlement.find(cohort.settlement_id);
        if (males_it == adult_males_by_settlement.end() || males_it->second == 0U) {
            continue;
        }

        // Sustained scarcity suppresses fertility.
        const auto aggregate_it = aggregate_by_settlement.find(cohort.settlement_id);
        if (aggregate_it != aggregate_by_settlement.end()) {
            const auto* aggregate = aggregate_it->second;
            if (aggregate->required_food_year > 0 && aggregate->unmet_food_year > 0) {
                const auto stress_per_10k = static_cast<std::uint64_t>(
                    (aggregate->unmet_food_year * 10'000) / aggregate->required_food_year);
                const auto retained = (stress_per_10k >= 10'000U) ? 0U : (10'000U - stress_per_10k);
                rate = static_cast<std::uint32_t>((static_cast<std::uint64_t>(rate) * retained) / 10'000U);
            }
        }
        if (rate == 0U) {
            continue;
        }

        const auto seed = derive_seed(
            settlement_seed(cohort.settlement_id),
            SeedDomain::CohortFertility,
            cohort.id ^ (demographic_year * 0x8EBC6AF09C88C6E3ULL));
        const auto births = deterministic_share(cohort.population, rate, 10'000U, seed);
        if (births == 0U) {
            continue;
        }

        const auto sex_seed = derive_seed(
            settlement_seed(cohort.settlement_id),
            SeedDomain::CohortSexSplit,
            cohort.id ^ (demographic_year * 0x589965CC75374CC3ULL));
        const auto female_births = deterministic_share(births, 5'000U, 10'000U, sex_seed);
        const auto male_births = births - female_births;

        const auto push_newborns = [&](const std::uint64_t count, const BiologicalSex sex) {
            if (count == 0U) {
                return;
            }
            newborn_cohorts.push_back(PopulationCohort{
                .id = next_cohort_id_++,
                .region_id = cohort.region_id,
                .settlement_id = cohort.settlement_id,
                .age_band = CohortAgeBand::Child,
                .sex = sex,
                .occupation_group = CohortOccupationGroup::Unspecified,
                .health_band = cohort.health_band,
                .wealth_band = cohort.wealth_band,
                .knowledge_profile_id = cohort.knowledge_profile_id,
                .population = count,
            });
        };

        push_newborns(female_births, BiologicalSex::Female);
        push_newborns(male_births, BiologicalSex::Male);

        state_.total_births += births;
        state_.aggregate_births_total += births;
    }

    for (const auto& cohort : newborn_cohorts) {
        state_.population_cohorts.push_back(cohort);
    }
}

void Simulation::process_aggregate_demography_period(const std::uint64_t demographic_year) {
    // Aging runs first so newborns created this year stay in the Child band.
    process_aggregate_cohort_aging(demographic_year);
    process_aggregate_cohort_mortality(demographic_year);
    process_aggregate_cohort_fertility(demographic_year);

    merge_population_cohorts();

    for (auto& aggregate : state_.settlement_aggregates) {
        if (is_settlement_detailed(aggregate.settlement_id)) {
            continue;
        }
        // Reset the yearly stress window now that it has been consumed.
        aggregate.required_food_year = 0;
        aggregate.unmet_food_year = 0;
    }

    for (const auto& settlement : state_.settlements) {
        if (!is_settlement_detailed(settlement.id)) {
            rebuild_settlement_aggregate_distribution(settlement.id);
        }
    }

    refresh_population_count();
}

void Simulation::promote_region_to_detailed(const RegionId region_id) {
    set_region_detail_level(region_id, SimulationDetailLevel::Detailed);

    std::unordered_map<SettlementId, std::vector<HouseholdId>> households_by_settlement;
    for (auto& household : state_.households) {
        if (household.region_id == region_id) {
            households_by_settlement[household.settlement_id].push_back(household.id);
        }
    }

    auto assign_household_for_settlement = [&](const SettlementId settlement_id, const std::size_t ordinal) -> Household* {
        auto& candidates = households_by_settlement[settlement_id];
        for (const auto household_id : candidates) {
            auto* household = [&]() -> Household* {
                for (auto& entry : state_.households) {
                    if (entry.id == household_id) {
                        return &entry;
                    }
                }
                return nullptr;
            }();
            if (household == nullptr) {
                continue;
            }
            std::uint64_t alive_count = 0;
            for (const auto member_id : household->member_ids) {
                for (const auto& person : state_.people) {
                    if (person.id == member_id && person.alive) {
                        ++alive_count;
                    }
                }
            }
            if (alive_count < kAggregatePeoplePerHousehold) {
                return household;
            }
        }

        auto new_household = Household{
            .id = next_household_id(),
            .region_id = region_id,
            .settlement_id = settlement_id,
        };
        assign_household_position_offset(new_household);
        state_.households.push_back(new_household);
        candidates.push_back(new_household.id);
        (void)ordinal;
        for (auto& household : state_.households) {
            if (household.id == new_household.id) {
                return &household;
            }
        }
        return nullptr;
    };

    std::vector<PopulationCohort> remaining;
    remaining.reserve(state_.population_cohorts.size());
    for (const auto& cohort : state_.population_cohorts) {
        if (cohort.region_id != region_id) {
            remaining.push_back(cohort);
            continue;
        }

        std::vector<Person*> dormant;
        for (auto& person : state_.people) {
            if (person.source_cohort_id.has_value() && *person.source_cohort_id == cohort.id && !person.alive) {
                dormant.push_back(&person);
            }
        }
        std::sort(dormant.begin(), dormant.end(), [](const Person* lhs, const Person* rhs) {
            return lhs->id < rhs->id;
        });

        std::uint64_t restored = 0;
        for (auto* person : dormant) {
            if (restored >= cohort.population) {
                break;
            }
            person->alive = true;
            // Demotion drops the exact occupation and keeps only the group, so
            // a restored person must be resolved from it again; left at None
            // they produce nothing until the next annual reassignment.
            person->occupation = occupation_from_cohort_group(
                cohort.occupation_group,
                derive_seed(settlement_seed(cohort.settlement_id), SeedDomain::InitialPopulation, cohort.id ^ restored));
            person->occupation_history_resync_pending = true;
            auto* household = [&]() -> Household* {
                for (auto& entry : state_.households) {
                    if (entry.id == person->household_id) {
                        return &entry;
                    }
                }
                return nullptr;
            }();
            if (household != nullptr &&
                std::find(household->member_ids.begin(), household->member_ids.end(), person->id) == household->member_ids.end()) {
                household->member_ids.push_back(person->id);
            }
            ++restored;
        }

        for (std::uint64_t index = restored; index < cohort.population; ++index) {
            const auto* settlement = find_settlement(cohort.settlement_id);
            if (settlement == nullptr) {
                continue;
            }

            auto* household = assign_household_for_settlement(cohort.settlement_id, static_cast<std::size_t>(index));
            if (household == nullptr) {
                continue;
            }
            const auto person_id = next_person_id();
            const auto roll_seed = derive_seed(settlement_seed(cohort.settlement_id), SeedDomain::InitialPopulation, cohort.id ^ index);

            std::uint32_t age_years = 30U;
            if (cohort.age_band == CohortAgeBand::Child) {
                age_years = static_cast<std::uint32_t>(seed_random_bounded(roll_seed, 18U));
            } else if (cohort.age_band == CohortAgeBand::YoungAdult) {
                age_years = 18U + static_cast<std::uint32_t>(seed_random_bounded(roll_seed, 12U));
            } else if (cohort.age_band == CohortAgeBand::Adult) {
                age_years = 30U + static_cast<std::uint32_t>(seed_random_bounded(roll_seed, 30U));
            } else {
                age_years = 60U + static_cast<std::uint32_t>(seed_random_bounded(roll_seed, 25U));
            }

            const Occupation occupation = occupation_from_cohort_group(cohort.occupation_group, roll_seed);

            state_.people.push_back(Person{
                .id = person_id,
                .birth_year = current_year_signed() - static_cast<std::int64_t>(age_years),
                .sex = cohort.sex,
                .alive = true,
                .household_id = household->id,
                .nutrition_points = cohort.health_band == CohortHealthBand::Low ? 420 : (cohort.health_band == CohortHealthBand::Medium ? 680 : 860),
                .health_points = cohort.health_band == CohortHealthBand::Low ? 420 : (cohort.health_band == CohortHealthBand::Medium ? 680 : 860),
                .occupation = occupation,
                .materialized_from_cohort = true,
                .source_cohort_id = cohort.id,
                .occupation_history_resync_pending = true,
            });
            household->member_ids.push_back(person_id);
        }
    }

    state_.population_cohorts = std::move(remaining);

    // Restored members were appended out of id order. Snapshots store members
    // sorted by id, so unsorted live state would diverge from a reloaded run.
    for (auto& household : state_.households) {
        if (household.region_id == region_id) {
            std::sort(household.member_ids.begin(), household.member_ids.end());
        }
    }

    const auto recompute_household_wealth = [](Household& household) {
        const auto wealth_food = std::max<std::int64_t>(0, household.resources.food_inventory.value);
        const auto wealth_wood = std::max<std::int64_t>(0, household.owned_materials.wood.value) * kWealthValueWood;
        const auto wealth_stone = std::max<std::int64_t>(0, household.owned_materials.stone.value) * kWealthValueStone;
        const auto wealth_timber = std::max<std::int64_t>(0, household.owned_materials.timber.value) * kWealthValueTimber;
        const auto wealth_tools = std::max<std::int64_t>(0, household.owned_materials.basic_tool.value) * kWealthValueBasicTool;
        household.derived_wealth_points = wealth_food + wealth_wood + wealth_stone + wealth_timber + wealth_tools;
    };

    for (const auto& settlement : state_.settlements) {
        if (settlement.region_id != region_id) {
            continue;
        }

        std::vector<Household*> settlement_households;
        for (auto& household : state_.households) {
            if (household.region_id == region_id && household.settlement_id == settlement.id) {
                settlement_households.push_back(&household);
            }
        }

        if (settlement_households.empty()) {
            continue;
        }

        std::sort(settlement_households.begin(), settlement_households.end(), [](const Household* lhs, const Household* rhs) {
            return lhs->id < rhs->id;
        });

        Household* canonical_owner = settlement_households.front();
        for (auto* household : settlement_households) {
            if (!household->member_ids.empty()) {
                canonical_owner = household;
                break;
            }
        }

        for (auto* household : settlement_households) {
            household->owned_materials = SettlementMaterialInventory{};
        }
        canonical_owner->owned_materials = settlement.materials;

        // Hand the aggregate food ledger back to households and clear it, so
        // stored food is counted exactly once across the LOD transition.
        for (auto& aggregate : state_.settlement_aggregates) {
            if (aggregate.settlement_id != settlement.id) {
                continue;
            }

            auto food_remaining = std::max<std::int64_t>(0, aggregate.food_inventory);
            const auto household_share =
                food_remaining / static_cast<std::int64_t>(settlement_households.size());
            for (auto* household : settlement_households) {
                household->resources.food_inventory.value += household_share;
                food_remaining -= household_share;
            }
            // Deterministic remainder to the lowest-id household.
            settlement_households.front()->resources.food_inventory.value += food_remaining;

            canonical_owner->resources.food_produced_total += aggregate.food_produced_total;
            canonical_owner->resources.food_consumed_total += aggregate.food_consumed_total;
            canonical_owner->resources.unmet_food_total += aggregate.unmet_food_total;

            aggregate.food_inventory = 0;
            aggregate.food_produced_total = 0;
            aggregate.food_consumed_total = 0;
            aggregate.unmet_food_total = 0;
            aggregate.required_food_year = 0;
            aggregate.unmet_food_year = 0;
            aggregate.wealth_points_total = 0;
            aggregate.household_count = 0;
            aggregate.small_household_count = 0;
            aggregate.medium_household_count = 0;
            aggregate.large_household_count = 0;
            aggregate.labour_low_household_count = 0;
            aggregate.labour_medium_household_count = 0;
            aggregate.labour_high_household_count = 0;
            aggregate.wealth_low_household_count = 0;
            aggregate.wealth_medium_household_count = 0;
            aggregate.wealth_high_household_count = 0;
            aggregate.food_buffer_low_household_count = 0;
            aggregate.food_buffer_medium_household_count = 0;
            aggregate.food_buffer_high_household_count = 0;
            aggregate.labour_capacity = 0;
            aggregate.labour_demand = 0;
            aggregate.labour_allocated = 0;
        }

        for (auto* household : settlement_households) {
            recompute_household_wealth(*household);
        }
    }

    refresh_population_count();
    refresh_food_aggregates();
    refresh_labour_aggregates();
    refresh_material_aggregates();
    refresh_economic_aggregates();
}

void Simulation::demote_region_to_settlement_aggregate(const RegionId region_id) {
    set_region_detail_level(region_id, SimulationDetailLevel::SettlementAggregate);

    struct CohortBuild {
        PopulationCohort template_value{};
        std::uint64_t population{};
    };

    std::unordered_map<CohortId, CohortBuild> by_source_cohort;

    std::unordered_map<HouseholdId, const Household*> household_by_id;
    household_by_id.reserve(state_.households.size());
    for (const auto& household : state_.households) {
        household_by_id.emplace(household.id, &household);
    }

    // Every living person in the region folds into aggregate representation --
    // not only those previously materialized from a cohort. Otherwise a
    // natively-simulated settlement would keep running per-person loops while
    // nominally marked as an aggregate.
    struct FoldKey {
        SettlementId settlement_id{};
        CohortAgeBand age_band{};
        BiologicalSex sex{};
        CohortOccupationGroup occupation_group{};
        CohortHealthBand health_band{};
        CohortWealthBand wealth_band{};

        [[nodiscard]] bool operator==(const FoldKey& other) const = default;
    };

    // Deterministic assignment of a cohort id for anonymous people that have no
    // prior cohort anchor, keyed by canonical bucket and ordered by person id.
    std::vector<std::pair<FoldKey, CohortId>> fold_key_to_cohort;
    const auto cohort_id_for_key = [&](const FoldKey& key) {
        for (const auto& [candidate, cohort_id] : fold_key_to_cohort) {
            if (candidate == key) {
                return cohort_id;
            }
        }
        const auto cohort_id = next_cohort_id_++;
        fold_key_to_cohort.emplace_back(key, cohort_id);
        return cohort_id;
    };

    // Canonized entities keep their exact identity and stay resident through the
    // transition; only anonymous population folds into cohorts.
    const auto canonized_ids = collect_canonized_person_ids();

    std::vector<Person*> fold_order;
    fold_order.reserve(state_.people.size());
    for (auto& person : state_.people) {
        if (!person.alive || canonized_ids.contains(person.id)) {
            continue;
        }
        const auto household_it = household_by_id.find(person.household_id);
        if (household_it == household_by_id.end() || household_it->second->region_id != region_id) {
            continue;
        }
        fold_order.push_back(&person);
    }
    std::sort(fold_order.begin(), fold_order.end(), [](const Person* lhs, const Person* rhs) {
        return lhs->id < rhs->id;
    });

    for (auto* person_ptr : fold_order) {
        auto& person = *person_ptr;
        const auto household_it = household_by_id.find(person.household_id);
        const auto* household = household_it->second;

        const auto key = FoldKey{
            .settlement_id = household->settlement_id,
            .age_band = age_band_from_years(age_years(person)),
            .sex = person.sex,
            .occupation_group = occupation_group_from_person(person.occupation),
            .health_band = health_band_from_points(person.health_points),
            .wealth_band = wealth_band_from_points(household->derived_wealth_points),
        };

        // Reuse an existing cohort anchor when this person was promoted from
        // one, so repeated demote/promote cycles never reroll identity.
        const auto cohort_id = person.source_cohort_id.has_value()
            ? *person.source_cohort_id
            : cohort_id_for_key(key);

        if (!by_source_cohort.contains(cohort_id)) {
            by_source_cohort.emplace(cohort_id, CohortBuild{
                .template_value = PopulationCohort{
                    .id = cohort_id,
                    .region_id = region_id,
                    .settlement_id = key.settlement_id,
                    .age_band = key.age_band,
                    .sex = key.sex,
                    .occupation_group = key.occupation_group,
                    .health_band = key.health_band,
                    .wealth_band = key.wealth_band,
                    .knowledge_profile_id = 1U,
                    .population = 0U,
                },
                .population = 0U,
            });
        }

        ++by_source_cohort[cohort_id].population;
        person.source_cohort_id = cohort_id;
        remove_from_household(person.household_id, person.id);
        person.alive = false;
        person.occupation = Occupation::None;
    }
    // A folded merchant is no longer a person to remember prices.
    prune_merchant_memory();

    // Exact conservation: household food, materials and wealth move into the
    // settlement aggregate ledger rather than vanishing with the detail level.
    std::unordered_map<SettlementId, SettlementAggregateState*> aggregate_by_settlement;
    aggregate_by_settlement.reserve(state_.settlement_aggregates.size());
    for (auto& aggregate : state_.settlement_aggregates) {
        aggregate_by_settlement.emplace(aggregate.settlement_id, &aggregate);
    }

    for (auto& household : state_.households) {
        if (household.region_id != region_id) {
            continue;
        }
        // Households still holding canonized residents keep their exact
        // resources; only fully-folded households hand theirs to the aggregate.
        if (!household.member_ids.empty()) {
            continue;
        }

        auto* settlement = find_settlement_mutable(household.settlement_id);
        const auto aggregate_it = aggregate_by_settlement.find(household.settlement_id);
        if (aggregate_it != aggregate_by_settlement.end()) {
            auto* aggregate = aggregate_it->second;
            aggregate->food_inventory += std::max<std::int64_t>(0, household.resources.food_inventory.value);
            aggregate->food_produced_total += household.resources.food_produced_total;
            aggregate->food_consumed_total += household.resources.food_consumed_total;
            aggregate->unmet_food_total += household.resources.unmet_food_total;
            aggregate->wealth_points_total += household.derived_wealth_points;
        }

        if (settlement != nullptr) {
            if (!checked_add_i64(settlement->coin, household.coin, settlement->coin)) {
                throw std::overflow_error("Settlement coin overflow while folding a household");
            }
            settlement->materials.wood.value += household.owned_materials.wood.value;
            settlement->materials.stone.value += household.owned_materials.stone.value;
            settlement->materials.timber.value += household.owned_materials.timber.value;
            settlement->materials.basic_tool.value += household.owned_materials.basic_tool.value;
        }

        household.resources = HouseholdResourceState{};
        household.owned_materials = SettlementMaterialInventory{};
        household.coin = 0;
        household.derived_wealth_points = 0;
    }

    std::vector<PopulationCohort> retained;
    retained.reserve(state_.population_cohorts.size() + by_source_cohort.size());
    for (const auto& cohort : state_.population_cohorts) {
        if (cohort.region_id != region_id) {
            retained.push_back(cohort);
        }
    }
    for (const auto& [cohort_id, build] : by_source_cohort) {
        auto cohort = build.template_value;
        cohort.population = build.population;
        retained.push_back(cohort);
        if (cohort_id >= next_cohort_id_) {
            next_cohort_id_ = cohort_id + 1U;
        }
    }

    state_.population_cohorts = std::move(retained);

    for (const auto& settlement : state_.settlements) {
        if (settlement.region_id == region_id) {
            rebuild_settlement_aggregate_distribution(settlement.id);
        }
    }

    refresh_population_count();
    refresh_food_aggregates();
    refresh_labour_aggregates();
    refresh_material_aggregates();
    refresh_economic_aggregates();
}

void Simulation::process_basic_discoveries(const std::uint64_t demographic_year) {
    std::unordered_set<std::uint32_t> discovered_keys;
    discovered_keys.reserve(kBasicDiscoveryKeys.size());
    for (const auto& event : state_.event_log) {
        if (event.type != EventType::DiscoveryMade) {
            continue;
        }
        const auto* payload = std::get_if<DiscoveryMadeEventPayload>(&event.payload);
        if (payload == nullptr || payload->discovery_key == 0U) {
            continue;
        }
        discovered_keys.insert(payload->discovery_key);
    }

    if (discovered_keys.size() >= kBasicDiscoveryKeys.size()) {
        return;
    }

    std::unordered_map<HouseholdId, SettlementId> settlement_by_household;
    settlement_by_household.reserve(state_.households.size());
    for (const auto& household : state_.households) {
        settlement_by_household.emplace(household.id, household.settlement_id);
    }

    std::vector<SettlementId> settlement_ids;
    settlement_ids.reserve(state_.settlements.size());
    for (const auto& settlement : state_.settlements) {
        if (settlement.active) {
            settlement_ids.push_back(settlement.id);
        }
    }
    std::sort(settlement_ids.begin(), settlement_ids.end());

    for (const auto settlement_id : settlement_ids) {
        if (discovered_keys.size() >= kBasicDiscoveryKeys.size()) {
            break;
        }

        const auto attempt_seed = derive_seed(
            settlement_seed(settlement_id),
            SeedDomain::DiscoveryAttempt,
            demographic_year);
        if (!seed_chance_per_10k(attempt_seed, kDiscoveryAttemptPerSettlementPer10k)) {
            continue;
        }

        std::vector<std::uint32_t> undiscovered_keys;
        undiscovered_keys.reserve(kBasicDiscoveryKeys.size());
        for (const auto key : kBasicDiscoveryKeys) {
            if (!discovered_keys.contains(key)) {
                undiscovered_keys.push_back(key);
            }
        }
        if (undiscovered_keys.empty()) {
            continue;
        }

        const auto key_seed = derive_seed(
            settlement_seed(settlement_id),
            SeedDomain::DiscoveryKeySelection,
            demographic_year);
        const auto key_index = static_cast<std::size_t>(seed_random_bounded(key_seed, undiscovered_keys.size()));
        const auto discovery_key = undiscovered_keys[key_index];

        std::vector<PersonId> discoverer_candidates;
        std::vector<PersonId> discovering_candidates;
        for (const auto& person : state_.people) {
            if (!person.alive || age_years(person) < kAdultAgeYears) {
                continue;
            }
            const auto settlement_it = settlement_by_household.find(person.household_id);
            if (settlement_it == settlement_by_household.end() || settlement_it->second != settlement_id) {
                continue;
            }
            discoverer_candidates.push_back(person.id);
            if (person.current_activity == ActivityKind::Discovery) {
                discovering_candidates.push_back(person.id);
            }
        }

        if (discoverer_candidates.empty()) {
            continue;
        }

        // Ticket #37: prefer people whose schedule currently has them
        // actually doing Discovery -- a real hook from schedule state into
        // this system -- but fall back to the full settlement pool so a
        // settlement never goes permanently discovery-silent just because
        // nobody's schedule happens to line up at this exact hour.
        auto& effective_discoverer_candidates =
            discovering_candidates.empty() ? discoverer_candidates : discovering_candidates;

        std::sort(effective_discoverer_candidates.begin(), effective_discoverer_candidates.end());
        const auto discoverer_seed = derive_seed(
            settlement_seed(settlement_id),
            SeedDomain::DiscoveryPersonSelection,
            demographic_year ^ discovery_key);
        const auto discoverer_index = static_cast<std::size_t>(
            seed_random_bounded(discoverer_seed, effective_discoverer_candidates.size()));
        const auto discoverer_id = effective_discoverer_candidates[discoverer_index];

        const auto event_id = emit_discovery_made_event(discovery_key, discoverer_id, settlement_id);
        const std::array<PersonId, 1> observer_ids{discoverer_id};
        append_direct_observation_memories(event_id, observer_ids, state_.time);
        discovered_keys.insert(discovery_key);
    }
}

void Simulation::process_communication_teaching(const std::uint64_t demographic_year) {
    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<HouseholdId, SettlementId> settlement_by_household;
    settlement_by_household.reserve(state_.households.size());
    for (const auto& household : state_.households) {
        settlement_by_household.emplace(household.id, household.settlement_id);
    }

    std::vector<PersonId> recipient_ids;
    recipient_ids.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        if (person.alive) {
            recipient_ids.push_back(person.id);
        }
    }
    std::sort(recipient_ids.begin(), recipient_ids.end());

    for (const auto recipient_id : recipient_ids) {
        const auto recipient_it = person_by_id.find(recipient_id);
        if (recipient_it == person_by_id.end()) {
            continue;
        }
        const auto* recipient = recipient_it->second;
        if (recipient == nullptr || !recipient->alive) {
            continue;
        }

        std::vector<PersonId> household_candidates;
        std::vector<PersonId> settlement_candidates;
        const auto recipient_settlement_it = settlement_by_household.find(recipient->household_id);
        if (recipient_settlement_it == settlement_by_household.end()) {
            continue;
        }
        const auto recipient_settlement_id = recipient_settlement_it->second;
        for (const auto& candidate : state_.people) {
            if (!candidate.alive || candidate.id == recipient_id) {
                continue;
            }
            if (knowledge_count_for_person(candidate.id) == 0U) {
                continue;
            }

            if (candidate.household_id == recipient->household_id) {
                household_candidates.push_back(candidate.id);
                continue;
            }

            if (candidate.household_id == 0U || candidate.household_id == recipient->household_id) {
                continue;
            }

            const auto candidate_settlement_it = settlement_by_household.find(candidate.household_id);
            if (candidate_settlement_it == settlement_by_household.end()) {
                continue;
            }

            if (recipient_settlement_id == candidate_settlement_it->second) {
                settlement_candidates.push_back(candidate.id);
            }
        }

        const auto& teacher_candidates = household_candidates.empty() ? settlement_candidates : household_candidates;
        if (teacher_candidates.empty()) {
            continue;
        }

        const auto selection_base_seed = derive_seed(
            person_seed(recipient_id),
            SeedDomain::CommunicationTeacherSelection,
            demographic_year);

        const auto teacher_index = static_cast<std::size_t>(
            seed_random_bounded(selection_base_seed, teacher_candidates.size()));
        const auto teacher_id = teacher_candidates[teacher_index];
        const bool same_household = !household_candidates.empty();

        const auto attempt_seed = derive_seed(
            selection_base_seed,
            SeedDomain::CommunicationAttempt,
            teacher_id);
        const auto teaching_chance = same_household
            ? kTeachingChanceSameHouseholdPer10k
            : kTeachingChanceSameSettlementPer10k;
        const auto communication_chance = same_household
            ? kCommunicationChanceSameHouseholdPer10k
            : kCommunicationChanceSameSettlementPer10k;

        const bool teaching = seed_chance_per_10k(attempt_seed, teaching_chance);
        const auto communication_seed = derive_seed(
            attempt_seed,
            SeedDomain::CommunicationAttempt,
            demographic_year + recipient_id + teacher_id);
        const bool communication = teaching || seed_chance_per_10k(communication_seed, communication_chance);
        if (!communication) {
            continue;
        }

        auto teacher_knowledge = knowledge_for_person(teacher_id);
        if (teacher_knowledge.empty()) {
            continue;
        }
        std::sort(teacher_knowledge.begin(), teacher_knowledge.end(), [](const AgentKnowledge& lhs, const AgentKnowledge& rhs) {
            return lhs.id < rhs.id;
        });

        std::vector<AgentKnowledge> transmissible_knowledge;
        transmissible_knowledge.reserve(teacher_knowledge.size());
        for (const auto& knowledge : teacher_knowledge) {
            if (knowledge.source_memory_id == 0U) {
                continue;
            }

            const TransmissionSemanticKey semantic_key{
                .recipient_id = recipient_id,
                .speaker_memory_id = knowledge.source_memory_id,
            };
            if (transmission_semantic_keys_.contains(semantic_key)) {
                continue;
            }

            transmissible_knowledge.push_back(knowledge);
        }

        if (transmissible_knowledge.empty()) {
            continue;
        }

        const auto knowledge_seed = derive_seed(
            person_seed(recipient_id),
            SeedDomain::CommunicationKnowledgeSelection,
            (demographic_year * 1'000'003ULL) ^ teacher_id);
        const auto knowledge_index = static_cast<std::size_t>(
            seed_random_bounded(knowledge_seed, transmissible_knowledge.size()));
        const auto& selected_knowledge = transmissible_knowledge[knowledge_index];

        EventId source_event_id = 0U;
        for (const auto& memory : state_.agent_memories) {
            if (memory.id == selected_knowledge.source_memory_id) {
                source_event_id = memory.source_event_id;
                break;
            }
        }

        if (source_event_id == 0U) {
            continue;
        }

        const auto confidence_cap = teaching ? kTeachingConfidenceCapPer10k : kCommunicationConfidenceCapPer10k;
        const auto confidence = std::min<std::uint32_t>(selected_knowledge.confidence_per_10k, confidence_cap);
        const auto teacher_it = person_by_id.find(teacher_id);
        if (teacher_it == person_by_id.end() || teacher_it->second == nullptr) {
            continue;
        }
        const auto speaker_settlement_it = settlement_by_household.find(teacher_it->second->household_id);
        if (speaker_settlement_it == settlement_by_household.end()) {
            continue;
        }
        const auto recipient_memory_id = append_memory(AgentMemory{
            .id = 0,
            .owner_id = recipient_id,
            .acquired_at = state_.time,
            .source = teaching ? MemorySource::Teaching : MemorySource::Communication,
            .source_event_id = source_event_id,
            .confidence_per_10k = confidence,
        });

        append_knowledge_transmission(KnowledgeTransmission{
            .id = 0,
            .speaker_id = teacher_id,
            .recipient_id = recipient_id,
            .speaker_household_id = teacher_it->second->household_id,
            .recipient_household_id = recipient->household_id,
            .speaker_settlement_id = speaker_settlement_it->second,
            .recipient_settlement_id = recipient_settlement_id,
            .transmitted_at = state_.time,
            .method = teaching ? CommunicationMethod::Teaching : CommunicationMethod::Communication,
            .speaker_memory_id = selected_knowledge.source_memory_id,
            .recipient_memory_id = recipient_memory_id,
            .confidence_per_10k = confidence,
        });
    }
}

void Simulation::process_records(const std::uint64_t demographic_year) {
    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<HouseholdId, SettlementId> settlement_by_household;
    settlement_by_household.reserve(state_.households.size());
    for (const auto& household : state_.households) {
        settlement_by_household.emplace(household.id, household.settlement_id);
    }

    std::vector<SettlementId> settlement_ids;
    settlement_ids.reserve(state_.settlements.size());
    for (const auto& settlement : state_.settlements) {
        if (settlement.active) {
            settlement_ids.push_back(settlement.id);
        }
    }
    std::sort(settlement_ids.begin(), settlement_ids.end());

    for (const auto settlement_id : settlement_ids) {
        const auto attempt_seed = derive_seed(
            settlement_seed(settlement_id),
            SeedDomain::RecordAttempt,
            demographic_year);
        if (!seed_chance_per_10k(attempt_seed, kRecordCreationChancePer10k)) {
            continue;
        }

        std::vector<PersonId> author_candidates;
        std::vector<PersonId> record_keeping_candidates;
        for (const auto& person : state_.people) {
            if (!person.alive || age_years(person) < kAdultAgeYears) {
                continue;
            }
            const auto household_it = settlement_by_household.find(person.household_id);
            if (household_it == settlement_by_household.end() || household_it->second != settlement_id) {
                continue;
            }
            if (knowledge_count_for_person(person.id) == 0U) {
                continue;
            }
            author_candidates.push_back(person.id);
            if (person.current_activity == ActivityKind::RecordKeeping) {
                record_keeping_candidates.push_back(person.id);
            }
        }

        if (author_candidates.empty()) {
            continue;
        }

        // Ticket #37: same schedule-preference-with-fallback hook as
        // Discovery above -- prefer people currently scheduled for
        // RecordKeeping, but never let an empty match silence records
        // entirely for a settlement.
        auto& effective_author_candidates =
            record_keeping_candidates.empty() ? author_candidates : record_keeping_candidates;

        std::sort(effective_author_candidates.begin(), effective_author_candidates.end());
        const auto author_seed = derive_seed(
            settlement_seed(settlement_id),
            SeedDomain::RecordAuthorSelection,
            demographic_year);
        const auto author_index = static_cast<std::size_t>(
            seed_random_bounded(author_seed, effective_author_candidates.size()));
        const auto author_id = effective_author_candidates[author_index];

        auto author_knowledge = knowledge_for_person(author_id);
        if (author_knowledge.empty()) {
            continue;
        }
        std::sort(author_knowledge.begin(), author_knowledge.end(), [](const AgentKnowledge& lhs, const AgentKnowledge& rhs) {
            return lhs.id < rhs.id;
        });

        std::vector<AgentKnowledge> recordable_knowledge;
        recordable_knowledge.reserve(author_knowledge.size());
        for (const auto& knowledge : author_knowledge) {
            if (knowledge.source_memory_id == 0U) {
                continue;
            }
            recordable_knowledge.push_back(knowledge);
        }

        if (recordable_knowledge.empty()) {
            continue;
        }

        const auto knowledge_seed = derive_seed(
            settlement_seed(settlement_id),
            SeedDomain::RecordKnowledgeSelection,
            (demographic_year * 1'000'003ULL) ^ author_id);
        const auto knowledge_index = static_cast<std::size_t>(
            seed_random_bounded(knowledge_seed, recordable_knowledge.size()));
        const auto& selected_knowledge = recordable_knowledge[knowledge_index];

        EventId source_event_id = 0U;
        for (const auto& memory : state_.agent_memories) {
            if (memory.id == selected_knowledge.source_memory_id) {
                source_event_id = memory.source_event_id;
                break;
            }
        }
        if (source_event_id == 0U) {
            continue;
        }

        const RecordSemanticKey source_event_key{
            .settlement_id = settlement_id,
            .source_event_id = source_event_id,
        };
        if (record_semantic_keys_.contains(source_event_key)) {
            continue;
        }

        const auto author_it = person_by_id.find(author_id);
        if (author_it == person_by_id.end() || author_it->second == nullptr) {
            continue;
        }
        const auto author_household_id = author_it->second->household_id;
        const auto author_settlement_it = settlement_by_household.find(author_household_id);
        if (author_settlement_it == settlement_by_household.end() || author_settlement_it->second != settlement_id) {
            continue;
        }

        append_knowledge_record(KnowledgeRecord{
            .id = 0,
            .settlement_id = settlement_id,
            .author_id = author_id,
            .author_household_id = author_household_id,
            .recorded_at = state_.time,
            .source_memory_id = selected_knowledge.source_memory_id,
            .source_event_id = source_event_id,
            .confidence_per_10k = std::min<std::uint32_t>(selected_knowledge.confidence_per_10k, kRecordWriteConfidenceCapPer10k),
        });
    }

    std::vector<PersonId> reader_ids;
    reader_ids.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        if (person.alive) {
            reader_ids.push_back(person.id);
        }
    }
    std::sort(reader_ids.begin(), reader_ids.end());

    for (const auto reader_id : reader_ids) {
        const auto reader_it = person_by_id.find(reader_id);
        if (reader_it == person_by_id.end() || reader_it->second == nullptr) {
            continue;
        }

        const auto reader = reader_it->second;
        const auto reader_settlement_it = settlement_by_household.find(reader->household_id);
        if (reader_settlement_it == settlement_by_household.end()) {
            continue;
        }
        const auto reader_settlement_id = reader_settlement_it->second;

        std::vector<KnowledgeRecord> readable_records;
        for (const auto& record : state_.knowledge_records) {
            if (record.settlement_id != reader_settlement_id) {
                continue;
            }
            if (person_remembers_event(reader_id, record.source_event_id)) {
                continue;
            }

            const RecordAccessSemanticKey semantic_key{
                .record_id = record.id,
                .reader_id = reader_id,
            };
            if (record_access_semantic_keys_.contains(semantic_key)) {
                continue;
            }

            readable_records.push_back(record);
        }

        if (readable_records.empty()) {
            continue;
        }

        const auto attempt_seed = derive_seed(
            person_seed(reader_id),
            SeedDomain::RecordReadAttempt,
            demographic_year);
        if (!seed_chance_per_10k(attempt_seed, kRecordReadChancePer10k)) {
            continue;
        }

        std::sort(readable_records.begin(), readable_records.end(), [](const KnowledgeRecord& lhs, const KnowledgeRecord& rhs) {
            return lhs.id < rhs.id;
        });

        const auto selection_seed = derive_seed(
            person_seed(reader_id),
            SeedDomain::RecordReadSelection,
            (demographic_year * 1'000'003ULL) ^ reader_settlement_id);
        const auto record_index = static_cast<std::size_t>(
            seed_random_bounded(selection_seed, readable_records.size()));
        const auto& selected_record = readable_records[record_index];

        const auto read_confidence = std::min<std::uint32_t>(
            selected_record.confidence_per_10k,
            kRecordReadConfidenceCapPer10k);
        const auto reader_memory_id = append_memory(AgentMemory{
            .id = 0,
            .owner_id = reader_id,
            .acquired_at = state_.time,
            .source = MemorySource::Record,
            .source_event_id = selected_record.source_event_id,
            .confidence_per_10k = read_confidence,
        });

        append_knowledge_record_access(KnowledgeRecordAccess{
            .record_id = selected_record.id,
            .reader_id = reader_id,
            .reader_household_id = reader->household_id,
            .reader_settlement_id = reader_settlement_id,
            .reader_memory_id = reader_memory_id,
            .read_at = state_.time,
            .confidence_per_10k = read_confidence,
        });
    }
}

void Simulation::process_institutions(const std::uint64_t demographic_year) {
    std::unordered_map<HouseholdId, SettlementId> settlement_by_household;
    settlement_by_household.reserve(state_.households.size());
    for (const auto& household : state_.households) {
        settlement_by_household.emplace(household.id, household.settlement_id);
    }

    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<HouseholdId, const Household*> household_by_id;
    household_by_id.reserve(state_.households.size());
    for (const auto& household : state_.households) {
        household_by_id.emplace(household.id, &household);
    }

    std::unordered_map<RecordId, const KnowledgeRecord*> record_by_id;
    record_by_id.reserve(state_.knowledge_records.size());
    for (const auto& record : state_.knowledge_records) {
        record_by_id.emplace(record.id, &record);
    }

    std::unordered_map<EventId, const SimulationEvent*> event_by_id;
    event_by_id.reserve(state_.event_log.size());
    for (const auto& event : state_.event_log) {
        event_by_id.emplace(event.id, &event);
    }

    std::unordered_map<MemoryId, const AgentMemory*> memory_by_id;
    memory_by_id.reserve(state_.agent_memories.size());
    for (const auto& memory : state_.agent_memories) {
        memory_by_id.emplace(memory.id, &memory);
    }

    std::vector<SettlementId> active_settlement_ids;
    active_settlement_ids.reserve(state_.settlements.size());
    for (const auto& settlement : state_.settlements) {
        if (settlement.active) {
            active_settlement_ids.push_back(settlement.id);
        }
    }
    std::sort(active_settlement_ids.begin(), active_settlement_ids.end());

    for (const auto settlement_id : active_settlement_ids) {
        bool has_active_institution = false;
        for (const auto& institution : state_.institutions) {
            if (institution.settlement_id == settlement_id && institution.active) {
                has_active_institution = true;
                break;
            }
        }
        if (has_active_institution) {
            continue;
        }

        std::vector<PersonId> local_alive_people;
        std::vector<PersonId> founder_candidates;
        std::unordered_set<std::uint32_t> local_discovery_keys;
        bool has_local_record = false;

        for (const auto& record : state_.knowledge_records) {
            if (record.settlement_id == settlement_id) {
                has_local_record = true;
                break;
            }
        }

        for (const auto& person : state_.people) {
            if (!person.alive || person.household_id == 0U) {
                continue;
            }

            const auto household_it = settlement_by_household.find(person.household_id);
            if (household_it == settlement_by_household.end() || household_it->second != settlement_id) {
                continue;
            }

            local_alive_people.push_back(person.id);
            if (age_years(person) < kAdultAgeYears) {
                continue;
            }

            auto person_knowledge = knowledge_for_person(person.id);
            if (person_knowledge.empty()) {
                continue;
            }

            bool knows_local_discovery = false;
            for (const auto& knowledge : person_knowledge) {
                if (knowledge.confidence_per_10k == 0U) {
                    continue;
                }
                const auto* discovery_fact = std::get_if<BasicDiscoveryKnowledgeFact>(&knowledge.fact);
                if (discovery_fact == nullptr || discovery_fact->discovery_key == 0U) {
                    continue;
                }
                local_discovery_keys.insert(discovery_fact->discovery_key);
                knows_local_discovery = true;
            }

            if (knows_local_discovery) {
                founder_candidates.push_back(person.id);
            }
        }

        if (local_alive_people.size() < kInstitutionMinimumSettlementPopulation ||
            founder_candidates.empty() ||
            local_discovery_keys.empty() ||
            !has_local_record) {
            continue;
        }

        const auto founding_attempt_seed = derive_seed(
            settlement_seed(settlement_id),
            SeedDomain::InstitutionFoundingAttempt,
            demographic_year);
        if (!seed_chance_per_10k(founding_attempt_seed, kInstitutionFoundingChancePerSettlementPer10k)) {
            continue;
        }

        std::sort(founder_candidates.begin(), founder_candidates.end());
        const auto founder_seed = derive_seed(
            settlement_seed(settlement_id),
            SeedDomain::InstitutionFounderSelection,
            demographic_year);
        const auto founder_index = static_cast<std::size_t>(seed_random_bounded(founder_seed, founder_candidates.size()));
        const auto founder_id = founder_candidates[founder_index];

        std::vector<PersonId> member_candidates;
        for (const auto& person : state_.people) {
            if (!person.alive || person.household_id == 0U) {
                continue;
            }
            const auto household_it = settlement_by_household.find(person.household_id);
            if (household_it == settlement_by_household.end() || household_it->second != settlement_id) {
                continue;
            }
            if (knowledge_count_for_person(person.id) == 0U) {
                continue;
            }
            member_candidates.push_back(person.id);
        }

        std::sort(member_candidates.begin(), member_candidates.end());
        std::vector<PersonId> initial_members;
        if (!member_candidates.empty()) {
            struct ScoredMember {
                std::uint64_t score{};
                PersonId person_id{};
            };
            std::vector<ScoredMember> scored_members;
            scored_members.reserve(member_candidates.size());
            for (const auto person_id : member_candidates) {
                const auto member_seed = derive_seed(
                    settlement_seed(settlement_id),
                    SeedDomain::InstitutionMemberSelection,
                    demographic_year ^ person_id);
                scored_members.push_back(ScoredMember{.score = member_seed.value, .person_id = person_id});
            }
            std::sort(scored_members.begin(), scored_members.end(), [](const ScoredMember& lhs, const ScoredMember& rhs) {
                if (lhs.score != rhs.score) {
                    return lhs.score < rhs.score;
                }
                return lhs.person_id < rhs.person_id;
            });

            for (const auto& scored_member : scored_members) {
                initial_members.push_back(scored_member.person_id);
                if (initial_members.size() >= kInstitutionTargetMembers) {
                    break;
                }
            }
        }

        if (std::find(initial_members.begin(), initial_members.end(), founder_id) == initial_members.end()) {
            initial_members.push_back(founder_id);
        }
        std::sort(initial_members.begin(), initial_members.end());
        initial_members.erase(std::unique(initial_members.begin(), initial_members.end()), initial_members.end());
        if (initial_members.size() < kInstitutionMinimumMembers) {
            continue;
        }

        std::vector<RecordId> preserved_record_ids;
        for (const auto& record : state_.knowledge_records) {
            if (record.settlement_id == settlement_id) {
                preserved_record_ids.push_back(record.id);
            }
        }
        std::sort(preserved_record_ids.begin(), preserved_record_ids.end());
        preserved_record_ids.erase(std::unique(preserved_record_ids.begin(), preserved_record_ids.end()), preserved_record_ids.end());

        std::vector<std::uint32_t> preserved_discovery_keys;
        preserved_discovery_keys.reserve(local_discovery_keys.size());
        for (const auto key : local_discovery_keys) {
            if (key > 0U) {
                preserved_discovery_keys.push_back(key);
            }
        }
        std::sort(preserved_discovery_keys.begin(), preserved_discovery_keys.end());
        preserved_discovery_keys.erase(
            std::unique(preserved_discovery_keys.begin(), preserved_discovery_keys.end()),
            preserved_discovery_keys.end());

        const auto institution_id = next_institution_id();
        state_.institutions.push_back(Institution{
            .id = institution_id,
            .kind = InstitutionKind::KnowledgeKeeper,
            .settlement_id = settlement_id,
            .founder_id = founder_id,
            .founded_at = state_.time,
            .active = true,
            .member_ids = initial_members,
            .preserved_record_ids = preserved_record_ids,
            .preserved_discovery_keys = preserved_discovery_keys,
        });

        const auto event_id = emit_institution_founded_event(
            institution_id,
            InstitutionKind::KnowledgeKeeper,
            settlement_id,
            founder_id);
        append_direct_observation_memories(event_id, initial_members, state_.time);
    }

    std::sort(state_.institutions.begin(), state_.institutions.end(), [](const Institution& lhs, const Institution& rhs) {
        return lhs.id < rhs.id;
    });

    for (auto& institution : state_.institutions) {
        const auto* settlement = find_settlement(institution.settlement_id);
        if (settlement == nullptr || !settlement->active) {
            institution.active = false;
            institution.member_ids.clear();
            continue;
        }

        std::vector<PersonId> valid_members;
        valid_members.reserve(institution.member_ids.size());
        for (const auto member_id : institution.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it == person_by_id.end() || person_it->second == nullptr || !person_it->second->alive) {
                continue;
            }
            const auto household_it = household_by_id.find(person_it->second->household_id);
            if (household_it == household_by_id.end() || household_it->second == nullptr) {
                continue;
            }
            if (household_it->second->settlement_id != institution.settlement_id) {
                continue;
            }
            valid_members.push_back(member_id);
        }
        std::sort(valid_members.begin(), valid_members.end());
        valid_members.erase(std::unique(valid_members.begin(), valid_members.end()), valid_members.end());
        institution.member_ids = std::move(valid_members);

        if (institution.member_ids.size() < kInstitutionTargetMembers) {
            std::unordered_set<PersonId> existing_members;
            existing_members.reserve(institution.member_ids.size());
            for (const auto member_id : institution.member_ids) {
                existing_members.insert(member_id);
            }

            struct ScoredCandidate {
                std::uint64_t score{};
                PersonId person_id{};
            };
            std::vector<ScoredCandidate> add_candidates;
            for (const auto& person : state_.people) {
                if (!person.alive || person.household_id == 0U) {
                    continue;
                }
                const auto household_it = settlement_by_household.find(person.household_id);
                if (household_it == settlement_by_household.end() || household_it->second != institution.settlement_id) {
                    continue;
                }
                if (existing_members.contains(person.id) || knowledge_count_for_person(person.id) == 0U) {
                    continue;
                }

                const auto member_seed = derive_seed(
                    settlement_seed(institution.settlement_id),
                    SeedDomain::InstitutionMemberSelection,
                    (demographic_year * 1'000'003ULL) ^ institution.id ^ person.id);
                add_candidates.push_back(ScoredCandidate{.score = member_seed.value, .person_id = person.id});
            }

            std::sort(add_candidates.begin(), add_candidates.end(), [](const ScoredCandidate& lhs, const ScoredCandidate& rhs) {
                if (lhs.score != rhs.score) {
                    return lhs.score < rhs.score;
                }
                return lhs.person_id < rhs.person_id;
            });

            for (const auto& candidate : add_candidates) {
                if (institution.member_ids.size() >= kInstitutionTargetMembers) {
                    break;
                }
                institution.member_ids.push_back(candidate.person_id);
            }

            std::sort(institution.member_ids.begin(), institution.member_ids.end());
            institution.member_ids.erase(std::unique(institution.member_ids.begin(), institution.member_ids.end()), institution.member_ids.end());
        }

        institution.active = institution.member_ids.size() >= kInstitutionMinimumMembers;
        if (!institution.active) {
            continue;
        }

        for (const auto& record : state_.knowledge_records) {
            if (record.settlement_id != institution.settlement_id) {
                continue;
            }
            institution.preserved_record_ids.push_back(record.id);
        }
        std::sort(institution.preserved_record_ids.begin(), institution.preserved_record_ids.end());
        institution.preserved_record_ids.erase(
            std::unique(institution.preserved_record_ids.begin(), institution.preserved_record_ids.end()),
            institution.preserved_record_ids.end());

        std::unordered_set<std::uint32_t> discovered_keys;

        for (const auto member_id : institution.member_ids) {
            auto member_knowledge = knowledge_for_person(member_id);
            for (const auto& knowledge : member_knowledge) {
                if (knowledge.confidence_per_10k == 0U) {
                    continue;
                }
                const auto* discovery_fact = std::get_if<BasicDiscoveryKnowledgeFact>(&knowledge.fact);
                if (discovery_fact != nullptr && discovery_fact->discovery_key > 0U) {
                    discovered_keys.insert(discovery_fact->discovery_key);
                }
            }
        }

        for (const auto record_id : institution.preserved_record_ids) {
            const auto record_it = record_by_id.find(record_id);
            if (record_it == record_by_id.end() || record_it->second == nullptr) {
                continue;
            }
            const auto event_it = event_by_id.find(record_it->second->source_event_id);
            if (event_it == event_by_id.end() || event_it->second == nullptr) {
                continue;
            }
            if (event_it->second->type != EventType::DiscoveryMade) {
                continue;
            }

            const auto* discovery_payload = std::get_if<DiscoveryMadeEventPayload>(&event_it->second->payload);
            if (discovery_payload != nullptr && discovery_payload->discovery_key > 0U) {
                discovered_keys.insert(discovery_payload->discovery_key);
            }
        }

        institution.preserved_discovery_keys.clear();
        institution.preserved_discovery_keys.reserve(discovered_keys.size());
        for (const auto key : discovered_keys) {
            institution.preserved_discovery_keys.push_back(key);
        }
        std::sort(institution.preserved_discovery_keys.begin(), institution.preserved_discovery_keys.end());

        std::unordered_set<EventId> preserved_record_source_events;
        for (const auto record_id : institution.preserved_record_ids) {
            const auto record_it = record_by_id.find(record_id);
            if (record_it != record_by_id.end() && record_it->second != nullptr) {
                preserved_record_source_events.insert(record_it->second->source_event_id);
            }
        }

        struct InstitutionSourceCandidate {
            PersonId speaker_id{};
            MemoryId speaker_memory_id{};
            EventId source_event_id{};
            std::uint32_t confidence_per_10k{};
            std::optional<std::uint32_t> discovery_key{};
        };

        std::vector<InstitutionSourceCandidate> source_candidates;
        for (const auto member_id : institution.member_ids) {
            auto member_knowledge = knowledge_for_person(member_id);
            std::sort(member_knowledge.begin(), member_knowledge.end(), [](const AgentKnowledge& lhs, const AgentKnowledge& rhs) {
                return lhs.id < rhs.id;
            });

            for (const auto& knowledge : member_knowledge) {
                if (knowledge.confidence_per_10k == 0U || knowledge.source_memory_id == 0U) {
                    continue;
                }

                const auto source_memory_it = memory_by_id.find(knowledge.source_memory_id);
                if (source_memory_it == memory_by_id.end() || source_memory_it->second == nullptr) {
                    continue;
                }
                const auto* source_memory = source_memory_it->second;
                if (source_memory->owner_id != member_id || source_memory->source_event_id == 0U) {
                    continue;
                }

                bool include = false;
                std::optional<std::uint32_t> discovery_key = std::nullopt;
                if (const auto* discovery_fact = std::get_if<BasicDiscoveryKnowledgeFact>(&knowledge.fact); discovery_fact != nullptr) {
                    if (std::binary_search(
                            institution.preserved_discovery_keys.begin(),
                            institution.preserved_discovery_keys.end(),
                            discovery_fact->discovery_key)) {
                        include = true;
                        discovery_key = discovery_fact->discovery_key;
                    }
                }

                if (!include && preserved_record_source_events.contains(source_memory->source_event_id)) {
                    include = true;
                }

                if (!include) {
                    continue;
                }

                source_candidates.push_back(InstitutionSourceCandidate{
                    .speaker_id = member_id,
                    .speaker_memory_id = knowledge.source_memory_id,
                    .source_event_id = source_memory->source_event_id,
                    .confidence_per_10k = knowledge.confidence_per_10k,
                    .discovery_key = discovery_key,
                });
            }
        }

        if (source_candidates.empty()) {
            continue;
        }

        std::sort(source_candidates.begin(), source_candidates.end(), [](const InstitutionSourceCandidate& lhs, const InstitutionSourceCandidate& rhs) {
            if (lhs.speaker_memory_id != rhs.speaker_memory_id) {
                return lhs.speaker_memory_id < rhs.speaker_memory_id;
            }
            return lhs.speaker_id < rhs.speaker_id;
        });
        source_candidates.erase(
            std::unique(source_candidates.begin(), source_candidates.end(), [](const InstitutionSourceCandidate& lhs, const InstitutionSourceCandidate& rhs) {
                return lhs.speaker_memory_id == rhs.speaker_memory_id && lhs.speaker_id == rhs.speaker_id;
            }),
            source_candidates.end());

        std::unordered_set<PersonId> member_ids;
        member_ids.reserve(institution.member_ids.size());
        for (const auto member_id : institution.member_ids) {
            member_ids.insert(member_id);
        }

        std::vector<PersonId> recipient_candidates;
        for (const auto& person : state_.people) {
            if (!person.alive || person.household_id == 0U || member_ids.contains(person.id)) {
                continue;
            }

            const auto household_it = settlement_by_household.find(person.household_id);
            if (household_it == settlement_by_household.end() || household_it->second != institution.settlement_id) {
                continue;
            }
            recipient_candidates.push_back(person.id);
        }

        if (recipient_candidates.empty()) {
            continue;
        }
        std::sort(recipient_candidates.begin(), recipient_candidates.end());

        const auto source_seed = derive_seed(
            settlement_seed(institution.settlement_id),
            SeedDomain::InstitutionTeachingSourceSelection,
            demographic_year ^ institution.id);
        const auto source_index = static_cast<std::size_t>(seed_random_bounded(source_seed, source_candidates.size()));
        const auto& selected_source = source_candidates[source_index];

        std::vector<PersonId> eligible_recipients;
        eligible_recipients.reserve(recipient_candidates.size());
        for (const auto recipient_id : recipient_candidates) {
            if (selected_source.discovery_key.has_value() &&
                knows_basic_discovery(recipient_id, *selected_source.discovery_key)) {
                continue;
            }

            if (transmission_semantic_keys_.contains(TransmissionSemanticKey{
                    .recipient_id = recipient_id,
                    .speaker_memory_id = selected_source.speaker_memory_id,
                })) {
                continue;
            }
            eligible_recipients.push_back(recipient_id);
        }

        if (eligible_recipients.empty()) {
            continue;
        }

        const auto recipient_seed = derive_seed(
            settlement_seed(institution.settlement_id),
            SeedDomain::InstitutionTeachingRecipientSelection,
            (demographic_year * 1'000'003ULL) ^ institution.id ^ selected_source.speaker_memory_id);
        const auto recipient_index = static_cast<std::size_t>(seed_random_bounded(recipient_seed, eligible_recipients.size()));
        const auto recipient_id = eligible_recipients[recipient_index];

        const auto speaker_it = person_by_id.find(selected_source.speaker_id);
        const auto recipient_it = person_by_id.find(recipient_id);
        if (speaker_it == person_by_id.end() || recipient_it == person_by_id.end() ||
            speaker_it->second == nullptr || recipient_it->second == nullptr) {
            continue;
        }

        const auto speaker_household_it = household_by_id.find(speaker_it->second->household_id);
        const auto recipient_household_it = household_by_id.find(recipient_it->second->household_id);
        if (speaker_household_it == household_by_id.end() || recipient_household_it == household_by_id.end() ||
            speaker_household_it->second == nullptr || recipient_household_it->second == nullptr) {
            continue;
        }

        const auto confidence = std::min<std::uint32_t>(
            selected_source.confidence_per_10k,
            kInstitutionTeachingConfidenceCapPer10k);
        const auto recipient_memory_id = append_memory(AgentMemory{
            .id = 0,
            .owner_id = recipient_id,
            .acquired_at = state_.time,
            .source = MemorySource::Teaching,
            .source_event_id = selected_source.source_event_id,
            .confidence_per_10k = confidence,
        });

        append_knowledge_transmission(KnowledgeTransmission{
            .id = 0,
            .speaker_id = selected_source.speaker_id,
            .recipient_id = recipient_id,
            .speaker_household_id = speaker_household_it->second->id,
            .recipient_household_id = recipient_household_it->second->id,
            .speaker_settlement_id = institution.settlement_id,
            .recipient_settlement_id = institution.settlement_id,
            .transmitted_at = state_.time,
            .method = CommunicationMethod::Teaching,
            .speaker_memory_id = selected_source.speaker_memory_id,
            .recipient_memory_id = recipient_memory_id,
            .confidence_per_10k = confidence,
        });
    }
}

void Simulation::process_knowledge_loss(const std::uint64_t demographic_year) {
    std::unordered_map<MemoryId, const AgentMemory*> memory_by_id;
    memory_by_id.reserve(state_.agent_memories.size());
    for (const auto& memory : state_.agent_memories) {
        memory_by_id.emplace(memory.id, &memory);
    }

    for (auto& knowledge : state_.agent_knowledge) {
        if (knowledge.updated_at >= state_.time) {
            continue;
        }

        const auto stale_duration_ticks = state_.time.ticks - knowledge.updated_at.ticks;
        const auto stale_years = static_cast<std::uint64_t>(
            stale_duration_ticks / DemographicCalendarAdapter::year_duration().ticks);
        if (stale_years < kKnowledgeDecayGraceYears) {
            continue;
        }

        const auto source_memory_it = memory_by_id.find(knowledge.source_memory_id);
        if (source_memory_it == memory_by_id.end() || source_memory_it->second == nullptr) {
            continue;
        }

        const auto* source_memory = source_memory_it->second;
        std::uint32_t decay = kKnowledgeDecayDirectPerYearPer10k;
        if (source_memory->source == MemorySource::Communication) {
            decay = kKnowledgeDecayCommunicationPerYearPer10k;
        } else if (source_memory->source == MemorySource::Teaching) {
            decay = kKnowledgeDecayTeachingPerYearPer10k;
        } else if (source_memory->source == MemorySource::Record) {
            decay = kKnowledgeDecayRecordPerYearPer10k;
        }

        if (knowledge.confidence_per_10k <= decay) {
            knowledge.confidence_per_10k = 0U;
        } else {
            knowledge.confidence_per_10k -= decay;
        }

        if (knowledge.confidence_per_10k > kKnowledgeForgetThresholdPer10k) {
            continue;
        }

        const auto forget_seed = derive_seed(
            person_seed(knowledge.owner_id),
            SeedDomain::KnowledgeForgetAttempt,
            (knowledge.id * 1'000'003ULL) ^ demographic_year);
        const auto roll_seed = derive_seed(
            forget_seed,
            SeedDomain::KnowledgeForgetRoll,
            knowledge.source_memory_id);

        const auto deficit = kKnowledgeForgetThresholdPer10k - knowledge.confidence_per_10k;
        const auto forget_chance = std::min<std::uint32_t>(
            10'000U,
            2'000U + (deficit * 6U));
        if (!seed_chance_per_10k(roll_seed, forget_chance)) {
            continue;
        }

        knowledge.confidence_per_10k = 0U;
    }
}

PersonId Simulation::next_person_id() noexcept {
    return next_person_id_++;
}

HouseholdId Simulation::next_household_id() noexcept {
    return next_household_id_++;
}

RegionId Simulation::next_region_id() noexcept {
    return next_region_id_++;
}

SettlementId Simulation::next_settlement_id() noexcept {
    return next_settlement_id_++;
}

MemoryId Simulation::next_memory_id() noexcept {
    return next_memory_id_++;
}

KnowledgeId Simulation::next_knowledge_id() noexcept {
    return next_knowledge_id_++;
}

CommunicationId Simulation::next_communication_id() noexcept {
    return next_communication_id_++;
}

RecordId Simulation::next_record_id() noexcept {
    return next_record_id_++;
}

InstitutionId Simulation::next_institution_id() noexcept {
    return next_institution_id_++;
}

BuildingId Simulation::next_building_id() noexcept {
    return next_building_id_++;
}

SimulationSeed Simulation::root_seed() const noexcept {
    return simulation_seed_;
}

SimulationSeed Simulation::person_seed(const PersonId person_id) const noexcept {
    return derive_seed(root_seed(), SeedDomain::Person, person_id);
}

SimulationSeed Simulation::household_seed(const HouseholdId household_id) const noexcept {
    return derive_seed(root_seed(), SeedDomain::Household, household_id);
}

void Simulation::assign_household_position_offset(Household& household) const noexcept {
    assign_household_offset(household, household_seed(household.id));
}

SimulationSeed Simulation::region_seed(const RegionId region_id) const noexcept {
    return derive_seed(root_seed(), SeedDomain::Region, region_id);
}

SimulationSeed Simulation::settlement_seed(const SettlementId settlement_id) const noexcept {
    return derive_seed(root_seed(), SeedDomain::Settlement, settlement_id);
}

bool Simulation::sample_sex(const SimulationSeed seed) const noexcept {
    const auto sex_seed = derive_seed(seed, SeedDomain::Sex, 0U);
    return seed_chance_per_10k(sex_seed, 5'000U);
}

std::uint32_t Simulation::mortality_rate_per_10k(const std::uint32_t age_years) const noexcept {
    if (age_years == 0U) {
        return 380;
    }
    if (age_years <= 4U) {
        return 35;
    }
    if (age_years <= 14U) {
        return 15;
    }
    if (age_years <= 49U) {
        return 20;
    }
    if (age_years <= 64U) {
        return 60;
    }
    if (age_years <= 74U) {
        return 180;
    }
    if (age_years <= 84U) {
        return 900;
    }
    if (age_years <= 94U) {
        return 2'500;
    }
    if (age_years <= 104U) {
        return 5'000;
    }
    return 10'000;
}

std::uint32_t Simulation::mortality_penalty_from_person_health_per_10k(const Person& person) const noexcept {
    const auto is_child = age_years(person) < kAdultAgeYears;
    return nutrition_health_mortality_penalty_per_10k(person.nutrition_points, person.health_points, is_child);
}

std::uint32_t Simulation::fertility_rate_per_10k(const std::uint32_t age_years) const noexcept {
    if (age_years < 18U || age_years > 49U) {
        return 0;
    }
    if (age_years <= 24U) {
        return 2'200;
    }
    if (age_years <= 34U) {
        return 2'800;
    }
    if (age_years <= 39U) {
        return 1'300;
    }
    if (age_years <= 44U) {
        return 500;
    }
    return 180;
}

std::uint32_t Simulation::sample_initial_age_years(const PersonId person_id) const noexcept {
    const auto base_seed = derive_seed(person_seed(person_id), SeedDomain::InitialPopulation, 0U);
    const auto roll = static_cast<std::uint32_t>(seed_random_bounded(base_seed, 100U));
    if (roll < 35U) {
        const auto age_seed = derive_seed(base_seed, SeedDomain::InitialPopulation, 1U);
        return static_cast<std::uint32_t>(seed_random_bounded(age_seed, 15U));
    }
    if (roll < 85U) {
        const auto age_seed = derive_seed(base_seed, SeedDomain::InitialPopulation, 2U);
        return 15U + static_cast<std::uint32_t>(seed_random_bounded(age_seed, 35U));
    }
    const auto age_seed = derive_seed(base_seed, SeedDomain::InitialPopulation, 3U);
    return 50U + static_cast<std::uint32_t>(seed_random_bounded(age_seed, 41U));
}

std::int64_t Simulation::current_year_signed() const noexcept {
    const auto year = current_demographic_year();
    const auto max_i64_u64 = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (year > max_i64_u64) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(year);
}

std::uint32_t Simulation::age_years(const Person& person) const noexcept {
    const auto current_year = current_year_signed();
    if (current_year <= person.birth_year) {
        return 0;
    }

    const auto diff = current_year - person.birth_year;
    return bounded_u32(static_cast<std::uint64_t>(diff));
}

bool Simulation::has_any_alive_adult_male() const noexcept {
    for (const auto& person : state_.people) {
        if (person.alive && person.sex == BiologicalSex::Male && age_years(person) >= kAdultAgeYears) {
            return true;
        }
    }
    return false;
}

bool Simulation::is_alive_adult_male_in_household(const HouseholdId household_id) const noexcept {
    for (const auto& person : state_.people) {
        if (!person.alive || person.household_id != household_id || person.sex != BiologicalSex::Male) {
            continue;
        }
        if (age_years(person) >= kAdultAgeYears) {
            return true;
        }
    }
    return false;
}

std::uint32_t Simulation::alive_count_in_household(const HouseholdId household_id) const noexcept {
    std::uint32_t count = 0;
    for (const auto& person : state_.people) {
        if (person.alive && person.household_id == household_id) {
            ++count;
        }
    }
    return count;
}

std::int64_t Simulation::food_requirement_for_person(const Person& person) const noexcept {
    return age_years(person) >= kAdultAgeYears
        ? food_parameters_.adult_daily_food_units
        : food_parameters_.child_daily_food_units;
}

std::int64_t Simulation::productive_effort_for_person(const Person& person) const noexcept {
    return person_labour_capacity_units(
        person.alive,
        age_years(person),
        person.nutrition_points,
        person.health_points,
        food_parameters_,
        config_.productive_effort_scale_percent);
}

bool Simulation::occupation_eligible_for_productive_work(const Person& person) const noexcept {
    if (!person.alive) {
        return false;
    }
    if (age_years(person) < kAdultAgeYears) {
        return false;
    }
    if (person.nutrition_points < kOccupationMinimumNutritionPoints || person.health_points < kOccupationMinimumHealthPoints) {
        return false;
    }
    return productive_effort_for_person(person) > 0;
}

std::uint32_t Simulation::effective_fertility_rate_per_10k(const Person& person) const noexcept {
    const auto base_rate = fertility_rate_per_10k(age_years(person));
    if (base_rate == 0U) {
        return 0U;
    }

    const auto household_alive = alive_count_in_household(person.household_id);
    if (household_alive >= 24U) {
        return std::max<std::uint32_t>(base_rate / 20U, 1U);
    }

    const auto capacity_remaining = 24U - household_alive;
    const auto scaled = static_cast<std::uint64_t>(base_rate) * capacity_remaining;
    auto effective = std::max<std::uint32_t>(static_cast<std::uint32_t>(scaled / 24U), 1U);

    const auto person_scale = nutrition_health_fertility_scale_percent(person.nutrition_points, person.health_points);
    effective = static_cast<std::uint32_t>((static_cast<std::uint64_t>(effective) * person_scale) / 100U);
    return effective;
}

void Simulation::apply_person_nutrition_and_health(
    Person& person,
    const std::int64_t required_food,
    const std::int64_t allocated_food) {
    // Canonical per-person daily processing site. Instrumented so tests can
    // prove aggregate settlements never reach this path.
    ++person_daily_update_count_;

    const auto required = std::max<std::int64_t>(required_food, 0);
    const auto allocated = std::max<std::int64_t>(allocated_food, 0);

    const auto deficit = std::max<std::int64_t>(required - allocated, 0);
    const auto age = age_years(person);

    std::int32_t nutrition_delta = 0;
    if (deficit <= 0) {
        nutrition_delta = 8;
        person.consecutive_undernourished_days = 0;
    } else {
        const auto ratio_points = (required > 0) ? static_cast<std::int32_t>((deficit * 100) / required) : 100;
        nutrition_delta = -(5 + ratio_points / 12);
        ++person.consecutive_undernourished_days;
    }

    person.nutrition_points = clamp_i32(
        person.nutrition_points + nutrition_delta,
        food_parameters_.min_person_nutrition_points,
        food_parameters_.max_person_nutrition_points);

    std::int32_t health_delta = 0;
    if (person.nutrition_points >= 750) {
        health_delta = 3;
    } else if (person.nutrition_points >= 550) {
        health_delta = 0;
    } else if (person.nutrition_points >= 350) {
        health_delta = -2;
    } else {
        health_delta = -5;
    }

    if (age < 5U && deficit > 0) {
        health_delta -= 2;
    }

    if (person.consecutive_undernourished_days >= 20U && deficit > 0) {
        health_delta -= 2;
    }

    person.health_points = clamp_i32(
        person.health_points + health_delta,
        food_parameters_.min_person_health_points,
        food_parameters_.max_person_health_points);
}

std::int64_t Simulation::household_stress_points(const HouseholdId household_id) const noexcept {
    for (const auto& household : state_.households) {
        if (household.id == household_id) {
            return household.resources.nutritional_stress_points;
        }
    }
    return 0;
}

std::optional<PersonId> Simulation::sample_father_in_household(
    const HouseholdId household_id,
    const SimulationSeed selection_seed) noexcept {
    std::vector<PersonId> candidates;
    for (const auto& person : state_.people) {
        if (!person.alive || person.household_id != household_id || person.sex != BiologicalSex::Male) {
            continue;
        }
        if (age_years(person) >= kAdultAgeYears) {
            candidates.push_back(person.id);
        }
    }

    if (candidates.empty()) {
        return std::nullopt;
    }

    std::sort(candidates.begin(), candidates.end());
    const auto scoped_seed = derive_seed(selection_seed, SeedDomain::ParentSelection, household_id);
    const auto index = static_cast<std::size_t>(seed_random_bounded(scoped_seed, candidates.size()));
    return candidates[index];
}

std::optional<PersonId> Simulation::sample_father_global(const SimulationSeed selection_seed) noexcept {
    std::vector<PersonId> candidates;
    for (const auto& person : state_.people) {
        if (!person.alive || person.sex != BiologicalSex::Male) {
            continue;
        }
        if (age_years(person) >= kAdultAgeYears) {
            candidates.push_back(person.id);
        }
    }

    if (candidates.empty()) {
        return std::nullopt;
    }

    std::sort(candidates.begin(), candidates.end());
    const auto scoped_seed = derive_seed(selection_seed, SeedDomain::ParentSelection, 0U);
    const auto index = static_cast<std::size_t>(seed_random_bounded(scoped_seed, candidates.size()));
    return candidates[index];
}

void Simulation::remove_from_household(const HouseholdId household_id, const PersonId person_id) {
    for (auto& household : state_.households) {
        if (household.id != household_id) {
            continue;
        }

        auto& members = household.member_ids;
        members.erase(std::remove(members.begin(), members.end(), person_id), members.end());
        return;
    }
}

void Simulation::initialize_population() {
    state_.people.clear();
    state_.people.reserve(static_cast<std::size_t>(config_.initial_population));

    for (std::uint64_t i = 0; i < config_.initial_population; ++i) {
        (void)i;
        const auto id = next_person_id();
        const auto age = sample_initial_age_years(id);
        const auto sex = sample_sex(person_seed(id)) ? BiologicalSex::Female : BiologicalSex::Male;

        state_.people.push_back(Person{
            .id = id,
            .birth_year = -static_cast<std::int64_t>(age),
            .sex = sex,
            .alive = true,
            .household_id = 0,
            .mother_id = std::nullopt,
            .father_id = std::nullopt,
        });
    }
}

void Simulation::initialize_households() {
    state_.households.clear();
    if (state_.people.empty()) {
        return;
    }

    std::vector<PersonId> adult_ids;
    std::vector<PersonId> child_ids;
    for (const auto& person : state_.people) {
        if (age_years(person) >= kAdultAgeYears) {
            adult_ids.push_back(person.id);
        } else {
            child_ids.push_back(person.id);
        }
    }

    std::uint64_t household_count = 1;
    if (!adult_ids.empty()) {
        household_count = (static_cast<std::uint64_t>(adult_ids.size()) + 1U) / 2U;
        household_count = std::max<std::uint64_t>(household_count, 1U);
    }

    state_.households.reserve(static_cast<std::size_t>(household_count));
    for (std::uint64_t i = 0; i < household_count; ++i) {
        auto new_household = Household{
            .id = next_household_id(),
            .region_id = 0,
            .settlement_id = 0,
            .member_ids = {},
            .resources = HouseholdResourceState{.food_inventory = ResourceAmount{config_.initial_household_food_units}},
        };
        assign_household_position_offset(new_household);
        state_.households.push_back(new_household);
    }

    const auto assign_person_to_household = [this](const PersonId person_id, const HouseholdId household_id) {
        for (auto& person : state_.people) {
            if (person.id == person_id) {
                person.household_id = household_id;
                break;
            }
        }

        for (auto& household : state_.households) {
            if (household.id == household_id) {
                household.member_ids.push_back(person_id);
                break;
            }
        }
    };

    if (!adult_ids.empty()) {
        for (std::size_t i = 0; i < adult_ids.size(); ++i) {
            const auto household_index = std::min(i / 2U, state_.households.size() - 1U);
            assign_person_to_household(adult_ids[i], state_.households[household_index].id);
        }
    }

    for (const auto child_id : child_ids) {
        const auto assignment_seed = derive_seed(person_seed(child_id), SeedDomain::HouseholdAssignment, 0U);
        const auto household_index = static_cast<std::size_t>(seed_random_bounded(assignment_seed, state_.households.size()));
        assign_person_to_household(child_id, state_.households[household_index].id);
    }

    std::sort(state_.households.begin(), state_.households.end(), [](const Household& lhs, const Household& rhs) {
        return lhs.id < rhs.id;
    });
    for (auto& household : state_.households) {
        std::sort(household.member_ids.begin(), household.member_ids.end());
    }
}

RegionEnvironmentState Simulation::generate_region_state(const RegionId region_id) const {
    const auto seed = region_seed(region_id);

    const auto area_variation_seed = derive_seed(seed, SeedDomain::RegionProductiveCapacity, 0U);
    const auto area_variation = static_cast<std::int64_t>(seed_random_bounded(area_variation_seed, 41U)) - 20;
    auto land_area_units = config_.base_region_area_units + ((config_.base_region_area_units * area_variation) / 100);
    land_area_units = clamp_i64(land_area_units, kMinRegionAreaUnits, kMaxRegionAreaUnits);

    const auto environment_seed = derive_seed(seed, SeedDomain::RegionEnvironmentClass, 0U);
    const auto class_roll = static_cast<std::uint32_t>(seed_random_bounded(environment_seed, 100U));
    EnvironmentClass environment_class = EnvironmentClass::Marginal;
    if (class_roll < 45U) {
        environment_class = EnvironmentClass::ProductiveArable;
    } else if (class_roll < 70U) {
        environment_class = EnvironmentClass::Woodland;
    } else if (class_roll >= 90U) {
        environment_class = EnvironmentClass::WaterWetland;
    }

    std::int64_t productive_capacity_per_day = 0;
    if (!checked_mul_i64(land_area_units, environment_class_productive_units_per_area(environment_class), productive_capacity_per_day)) {
        throw std::overflow_error("Region productive capacity overflow");
    }

    std::int64_t wild_food_capacity = 0;
    if (!checked_mul_i64(land_area_units, environment_class_wild_capacity_units_per_area(environment_class), wild_food_capacity)) {
        throw std::overflow_error("Region wild food capacity overflow");
    }

    const auto productive_jitter_seed = derive_seed(seed, SeedDomain::RegionProductiveCapacity, 1U);
    const auto productive_jitter_percent = 90U + static_cast<std::uint32_t>(seed_random_bounded(productive_jitter_seed, 21U));
    productive_capacity_per_day = apply_scaled_percent(productive_capacity_per_day, productive_jitter_percent);

    const auto wild_jitter_seed = derive_seed(seed, SeedDomain::RegionWildCapacity, 1U);
    const auto wild_jitter_percent = 90U + static_cast<std::uint32_t>(seed_random_bounded(wild_jitter_seed, 21U));
    wild_food_capacity = apply_scaled_percent(wild_food_capacity, wild_jitter_percent);

    const auto wild_food_regeneration_per_day = std::max<std::int64_t>(1, wild_food_capacity / 120);
    const auto wild_food_harvest_limit_per_day = std::max<std::int64_t>(1, wild_food_capacity / 20);

    std::int64_t wood_capacity = 0;
    if (!checked_mul_i64(land_area_units, 180, wood_capacity)) {
        throw std::overflow_error("Region wood capacity overflow");
    }
    const auto wood_seed = derive_seed(seed, SeedDomain::RegionWoodCapacity, 1U);
    const auto wood_jitter_percent = 85U + static_cast<std::uint32_t>(seed_random_bounded(wood_seed, 31U));
    wood_capacity = apply_scaled_percent(wood_capacity, wood_jitter_percent);
    wood_capacity = apply_scaled_percent(wood_capacity, config_.wood_resource_scale_percent);

    std::int64_t stone_capacity = 0;
    if (!checked_mul_i64(land_area_units, 90, stone_capacity)) {
        throw std::overflow_error("Region stone capacity overflow");
    }
    const auto stone_seed = derive_seed(seed, SeedDomain::RegionStoneCapacity, 1U);
    const auto stone_jitter_percent = 85U + static_cast<std::uint32_t>(seed_random_bounded(stone_seed, 31U));
    stone_capacity = apply_scaled_percent(stone_capacity, stone_jitter_percent);
    stone_capacity = apply_scaled_percent(stone_capacity, config_.stone_resource_scale_percent);

    auto wood_regeneration_per_day = std::max<std::int64_t>(1, wood_capacity / 90);
    wood_regeneration_per_day = apply_scaled_percent(wood_regeneration_per_day, config_.wood_regeneration_scale_percent);
    wood_regeneration_per_day = std::max<std::int64_t>(wood_regeneration_per_day, 0);

    return RegionEnvironmentState{
        .id = region_id,
        .land_area_units = land_area_units,
        .environment_class = environment_class,
        .productive_capacity_per_day = productive_capacity_per_day,
        .wild_food_capacity = wild_food_capacity,
        .wild_food_stock = wild_food_capacity,
        .wild_food_regeneration_per_day = wild_food_regeneration_per_day,
        .wild_food_harvest_limit_per_day = wild_food_harvest_limit_per_day,
        .cumulative_wild_food_harvested = 0,
        .cumulative_wild_food_regenerated = 0,
        .wood_capacity = wood_capacity,
        .wood_stock = wood_capacity,
        .wood_regeneration_per_day = wood_regeneration_per_day,
        .cumulative_wood_harvested = 0,
        .cumulative_wood_regenerated = 0,
        .stone_capacity = stone_capacity,
        .stone_remaining = stone_capacity,
        .cumulative_stone_extracted = 0,
    };
}

void Simulation::initialize_regions() {
    state_.regions.clear();
    state_.regions.reserve(static_cast<std::size_t>(config_.initial_region_count));
    for (std::uint64_t i = 0; i < config_.initial_region_count; ++i) {
        (void)i;
        const auto region_id = next_region_id();
        state_.regions.push_back(generate_region_state(region_id));
    }
    std::sort(state_.regions.begin(), state_.regions.end(), [](const RegionEnvironmentState& lhs, const RegionEnvironmentState& rhs) {
        return lhs.id < rhs.id;
    });
    assign_region_positions();
}

void Simulation::assign_region_positions() {
    std::vector<Point2D> placed;
    placed.reserve(state_.regions.size());
    for (auto& region : state_.regions) {
        const auto point = pick_spaced_position(
            region_seed(region.id), SeedDomain::RegionPosition, 0, 0, kWorldHalfExtentUnits, kMinRegionDistanceUnits, placed);
        region.position_x = point.x;
        region.position_z = point.z;
        placed.push_back(point);
    }
}

void Simulation::assign_households_to_regions() {
    if (state_.regions.empty()) {
        return;
    }

    std::vector<RegionId> region_ids;
    region_ids.reserve(state_.regions.size());
    for (const auto& region : state_.regions) {
        region_ids.push_back(region.id);
    }
    std::sort(region_ids.begin(), region_ids.end());

    for (auto& household : state_.households) {
        const auto assignment_seed = derive_seed(household_seed(household.id), SeedDomain::RegionAssignment, 0U);
        const auto region_index = static_cast<std::size_t>(seed_random_bounded(assignment_seed, region_ids.size()));
        household.region_id = region_ids[region_index];
        household.settlement_id = 0;
    }
}

void Simulation::initialize_settlements() {
    state_.settlements.clear();

    std::vector<RegionId> region_ids;
    region_ids.reserve(state_.regions.size());
    for (const auto& region : state_.regions) {
        region_ids.push_back(region.id);
    }
    std::sort(region_ids.begin(), region_ids.end());

    for (const auto region_id : region_ids) {
        std::vector<Household*> region_households;
        region_households.reserve(state_.households.size());
        for (auto& household : state_.households) {
            if (household.region_id == region_id) {
                household.settlement_id = 0;
                region_households.push_back(&household);
            }
        }

        std::sort(region_households.begin(), region_households.end(), [](const Household* lhs, const Household* rhs) {
            return lhs->id < rhs->id;
        });

        if (region_households.empty()) {
            continue;
        }

        auto settlement_count = (static_cast<std::uint64_t>(region_households.size()) + (kTargetHouseholdsPerSettlement - 1U)) /
            kTargetHouseholdsPerSettlement;
        settlement_count = std::max<std::uint64_t>(1U, std::min<std::uint64_t>(settlement_count, kMaxInitialSettlementsPerRegion));
        if (config_.initial_settlements_per_region > 0U) {
            // An explicit request replaces the automatic layout, but every
            // settlement still needs at least one household.
            settlement_count = std::max<std::uint64_t>(
                1U,
                std::min<std::uint64_t>(config_.initial_settlements_per_region, region_households.size()));
        }

        const auto share_seed = derive_seed(region_seed(region_id), SeedDomain::SettlementAssignment, 1U);
        const auto share_jitter = static_cast<std::int32_t>(seed_random_bounded(share_seed, 1'001U)) - 500;
        const auto allocated_share_i32 = clamp_i32(
            static_cast<std::int32_t>(kInitialAllocatedSettlementShare) + share_jitter,
            static_cast<std::int32_t>(settlement_count),
            static_cast<std::int32_t>(kSettlementShareScale));
        const auto allocated_share = static_cast<std::uint32_t>(allocated_share_i32);

        std::vector<SettlementId> settlement_ids;
        settlement_ids.reserve(static_cast<std::size_t>(settlement_count));

        const auto base_share = allocated_share / static_cast<std::uint32_t>(settlement_count);
        auto share_remainder = allocated_share % static_cast<std::uint32_t>(settlement_count);
        for (std::uint64_t i = 0; i < settlement_count; ++i) {
            const auto settlement_id = next_settlement_id();
            std::uint32_t share = base_share;
            if (share_remainder > 0U) {
                ++share;
                --share_remainder;
            }

            state_.settlements.push_back(Settlement{
                .id = settlement_id,
                .region_id = region_id,
                .active = true,
                .access_share_points = share,
            });
            settlement_ids.push_back(settlement_id);
        }

        assign_settlement_positions(region_id, settlement_ids);

        for (auto* household : region_households) {
            const auto assignment_seed = derive_seed(household_seed(household->id), SeedDomain::SettlementAssignment, 0U);
            const auto settlement_index = static_cast<std::size_t>(seed_random_bounded(assignment_seed, settlement_ids.size()));
            household->settlement_id = settlement_ids[settlement_index];
        }

        std::unordered_map<SettlementId, std::uint64_t> settlement_household_counts;
        settlement_household_counts.reserve(settlement_ids.size());
        for (const auto settlement_id : settlement_ids) {
            settlement_household_counts.emplace(settlement_id, 0U);
        }
        for (const auto* household : region_households) {
            ++settlement_household_counts[household->settlement_id];
        }

        for (const auto settlement_id : settlement_ids) {
            auto count_it = settlement_household_counts.find(settlement_id);
            if (count_it == settlement_household_counts.end() || count_it->second > 0U) {
                continue;
            }

            Household* donor_household = nullptr;
            SettlementId donor_settlement_id = 0;
            std::uint64_t donor_count = 0;
            for (const auto donor_id : settlement_ids) {
                const auto donor_it = settlement_household_counts.find(donor_id);
                if (donor_it == settlement_household_counts.end() || donor_it->second <= 1U) {
                    continue;
                }
                if (donor_it->second > donor_count || (donor_it->second == donor_count && donor_id < donor_settlement_id)) {
                    donor_count = donor_it->second;
                    donor_settlement_id = donor_id;
                }
            }

            if (donor_count <= 1U) {
                continue;
            }

            for (auto* household : region_households) {
                if (household->settlement_id == donor_settlement_id) {
                    donor_household = household;
                    break;
                }
            }

            if (donor_household == nullptr) {
                continue;
            }

            donor_household->settlement_id = settlement_id;
            --settlement_household_counts[donor_settlement_id];
            ++settlement_household_counts[settlement_id];
        }
    }

    std::sort(state_.settlements.begin(), state_.settlements.end(), [](const Settlement& lhs, const Settlement& rhs) {
        return lhs.id < rhs.id;
    });
}

void Simulation::assign_settlement_positions(const RegionId region_id, const std::vector<SettlementId>& settlement_ids) {
    const auto* region = find_region(region_id);
    if (region == nullptr) {
        return;
    }

    std::vector<Point2D> placed;
    placed.reserve(settlement_ids.size());
    for (const auto settlement_id : settlement_ids) {
        auto* settlement = find_settlement_mutable(settlement_id);
        if (settlement == nullptr) {
            continue;
        }
        const auto point = place_settlement_point(*region, settlement_seed(settlement_id), placed);
        settlement->position_x = point.x;
        settlement->position_z = point.z;
        placed.push_back(point);
    }
}

void Simulation::assign_position_for_new_settlement(const SettlementId settlement_id, const RegionId region_id) {
    const auto* region = find_region(region_id);
    auto* settlement = find_settlement_mutable(settlement_id);
    if (region == nullptr || settlement == nullptr) {
        return;
    }

    std::vector<Point2D> placed;
    for (const auto& existing : state_.settlements) {
        if (existing.region_id == region_id && existing.id != settlement_id) {
            placed.push_back(Point2D{existing.position_x, existing.position_z});
        }
    }

    const auto point = place_settlement_point(*region, settlement_seed(settlement_id), placed);
    settlement->position_x = point.x;
    settlement->position_z = point.z;
}

void Simulation::initialize_region_lod_states() {
    state_.region_lod_states.clear();
    state_.region_lod_states.reserve(state_.regions.size());
    for (const auto& region : state_.regions) {
        state_.region_lod_states.push_back(RegionLodState{.region_id = region.id, .detail_level = SimulationDetailLevel::Detailed});
    }
    std::sort(state_.region_lod_states.begin(), state_.region_lod_states.end(), [](const RegionLodState& lhs, const RegionLodState& rhs) {
        return lhs.region_id < rhs.region_id;
    });
    state_.detail_level = SimulationDetailLevel::Detailed;
}

// The only place coin is created (ADR 0003). Every household is endowed in
// proportion to the value of what it starts with, and each settlement's commons
// in proportion to what its households were given. After this, coin only moves.
void Simulation::endow_initial_coin() {
    const auto add_checked = [](const std::int64_t lhs, const std::int64_t rhs) {
        std::int64_t sum = 0;
        if (!checked_add_i64(lhs, rhs, sum)) {
            throw std::overflow_error("Coin endowment overflow");
        }
        return sum;
    };
    const auto mul_checked = [](const std::int64_t lhs, const std::int64_t rhs) {
        std::int64_t product = 0;
        if (!checked_mul_i64(lhs, rhs, product)) {
            throw std::overflow_error("Coin endowment overflow");
        }
        return product;
    };

    std::unordered_map<SettlementId, std::int64_t> household_coin_by_settlement;
    for (auto& household : state_.households) {
        const auto& materials = household.owned_materials;
        auto stock_value = std::max<std::int64_t>(0, household.resources.food_inventory.value);
        stock_value = add_checked(stock_value, mul_checked(std::max<std::int64_t>(0, materials.wood.value), kWealthValueWood));
        stock_value = add_checked(stock_value, mul_checked(std::max<std::int64_t>(0, materials.stone.value), kWealthValueStone));
        stock_value = add_checked(stock_value, mul_checked(std::max<std::int64_t>(0, materials.timber.value), kWealthValueTimber));
        stock_value = add_checked(stock_value, mul_checked(std::max<std::int64_t>(0, materials.basic_tool.value), kWealthValueBasicTool));
        household.coin = mul_checked(stock_value, kCoinPerStockValue);
        auto& settlement_total = household_coin_by_settlement[household.settlement_id];
        settlement_total = add_checked(settlement_total, household.coin);
    }
    // The floored remainder of the commons share is deliberately never issued.
    for (auto& settlement : state_.settlements) {
        const auto it = household_coin_by_settlement.find(settlement.id);
        settlement.coin = it == household_coin_by_settlement.end()
            ? 0
            : mul_checked(it->second, kCommonsCoinPercentOfHouseholds) / 100;
    }
}

// Prices follow days of cover (ADR 0003): a settlement's stock of each resource
// against its own daily need. Stock is the commons plus what its detailed
// households hold, plus the aggregate ledger for food when its region is
// demoted. Need is the daily food requirement of its people (detailed members
// and cohorts), outstanding construction and crafting demand spread over a
// horizon for wood, stone and timber, and tool wear for tools. Everything is read
// from canonical state, so nothing is stored and a reload prices identically.
std::optional<SettlementPrices> Simulation::settlement_prices(const SettlementId settlement_id) const {
    const auto* settlement = find_settlement(settlement_id);
    if (settlement == nullptr || !settlement->active) {
        return std::nullopt;
    }

    // A price query never throws: an input too large to hold saturates, and the
    // pricing curve then reads it as a very large stock or need.
    constexpr auto kSaturated = std::numeric_limits<std::int64_t>::max();
    const auto add = [](const std::int64_t lhs, const std::int64_t rhs) {
        std::int64_t sum = 0;
        return checked_add_i64(lhs, rhs, sum) ? sum : kSaturated;
    };
    const auto mul = [](const std::int64_t lhs, const std::int64_t rhs) {
        std::int64_t product = 0;
        return checked_mul_i64(lhs, rhs, product) ? product : kSaturated;
    };
    const auto held = [](const std::int64_t value) { return std::max<std::int64_t>(0, value); };

    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::int64_t food_stock = held(settlement->commons_food.value);
    std::int64_t wood_stock = held(settlement->materials.wood.value);
    std::int64_t stone_stock = held(settlement->materials.stone.value);
    std::int64_t timber_stock = held(settlement->materials.timber.value);
    std::int64_t tool_stock = held(settlement->materials.basic_tool.value);
    std::int64_t daily_food_need = 0;
    std::uint64_t alive_population = 0;
    std::uint64_t eligible_workers = 0;
    std::uint64_t productive_workers = 0;

    for (const auto& household : state_.households) {
        if (household.settlement_id != settlement_id) {
            continue;
        }
        // Household owned_materials are the settlement stock shared out among its
        // households each day, so only settlement->materials is counted here.
        food_stock = add(food_stock, held(household.resources.food_inventory.value));
        for (const auto member_id : household.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it == person_by_id.end() || !person_it->second->alive) {
                continue;
            }
            const auto& person = *person_it->second;
            ++alive_population;
            daily_food_need = add(daily_food_need, food_requirement_for_person(person));
            if (occupation_eligible_for_productive_work(person)) {
                ++eligible_workers;
            }
            if (is_productive_occupation(person.occupation)) {
                ++productive_workers;
            }
        }
    }

    // A demoted region holds its people as cohorts and its food in a ledger.
    for (const auto& aggregate : state_.settlement_aggregates) {
        if (aggregate.settlement_id == settlement_id) {
            food_stock = add(food_stock, held(aggregate.food_inventory));
        }
    }
    for (const auto& cohort : state_.population_cohorts) {
        if (cohort.settlement_id != settlement_id) {
            continue;
        }
        const auto population = static_cast<std::int64_t>(cohort.population);
        const auto per_person = cohort.age_band == CohortAgeBand::Child
            ? food_parameters_.child_daily_food_units
            : food_parameters_.adult_daily_food_units;
        daily_food_need = add(daily_food_need, mul(population, per_person));
        alive_population += cohort.population;
        if (cohort.age_band == CohortAgeBand::YoungAdult || cohort.age_band == CohortAgeBand::Adult) {
            const auto workers = (cohort.population * kAggregateEligibleWorkersPer10k) / 10'000U;
            eligible_workers += workers;
            productive_workers += workers;
        }
    }

    const auto construction = construction_demand_for_settlement(settlement_id, state_.buildings);
    const auto tool_target = material_targets_for_settlement(alive_population, eligible_workers).basic_tool_target;
    const auto tool_deficit = std::max<std::int64_t>(0, tool_target - tool_stock);
    // Wood is needed to craft only the timber not already in stock.
    const auto timber_to_craft = std::max<std::int64_t>(0, construction.timber_remaining - timber_stock);
    const auto wood_outstanding = add(
        mul(timber_to_craft, material_parameters_.wood_per_timber),
        mul(tool_deficit, material_parameters_.wood_per_basic_tool));
    const auto stone_outstanding = add(construction.stone_remaining, mul(tool_deficit, material_parameters_.stone_per_basic_tool));

    SettlementPrices prices{};
    const auto price_into = [&prices](const ResourceType resource, const std::int64_t stock, const std::int64_t need_milli) {
        prices.millicoin[static_cast<std::size_t>(resource)] = price_millicoin(resource, stock, need_milli);
    };
    price_into(ResourceType::Food, food_stock, mul(daily_food_need, kNeedMilliPerUnit));
    price_into(ResourceType::Wood, wood_stock, daily_need_milli_from_outstanding(wood_outstanding));
    price_into(ResourceType::Stone, stone_stock, daily_need_milli_from_outstanding(stone_outstanding));
    price_into(ResourceType::Timber, timber_stock, daily_need_milli_from_outstanding(construction.timber_remaining));
    price_into(
        ResourceType::BasicTool,
        tool_stock,
        mul(static_cast<std::int64_t>(productive_workers), kNeedMilliPerUnit) / kToolWearWorkUnitsPerBreak);
    return prices;
}

void Simulation::initialize_settlement_aggregates() {
    state_.settlement_aggregates.clear();
    state_.settlement_aggregates.reserve(state_.settlements.size());
    for (const auto& settlement : state_.settlements) {
        state_.settlement_aggregates.push_back(SettlementAggregateState{
            .settlement_id = settlement.id,
            .region_id = settlement.region_id,
            .household_count = 0U,
            .small_household_count = 0U,
            .medium_household_count = 0U,
            .large_household_count = 0U,
            .food_inventory = 0,
            .food_produced_total = 0,
            .food_consumed_total = 0,
            .unmet_food_total = 0,
            .labour_capacity = 0,
            .labour_demand = 0,
            .labour_allocated = 0,
        });
    }
    std::sort(state_.settlement_aggregates.begin(), state_.settlement_aggregates.end(), [](const SettlementAggregateState& lhs, const SettlementAggregateState& rhs) {
        return lhs.settlement_id < rhs.settlement_id;
    });
}

bool Simulation::is_region_detailed(const RegionId region_id) const noexcept {
    return region_detail_level(region_id) == SimulationDetailLevel::Detailed;
}

bool Simulation::is_settlement_detailed(const SettlementId settlement_id) const noexcept {
    const auto* settlement = find_settlement(settlement_id);
    if (settlement == nullptr) {
        return true;
    }
    return is_region_detailed(settlement->region_id);
}

bool Simulation::is_household_detailed(const HouseholdId household_id) const noexcept {
    for (const auto& household : state_.households) {
        if (household.id == household_id) {
            return is_region_detailed(household.region_id);
        }
    }
    return true;
}

bool Simulation::is_person_detailed(const Person& person) const noexcept {
    return is_household_detailed(person.household_id);
}

void Simulation::rebuild_settlement_aggregate_distribution(const SettlementId settlement_id) {
    SettlementAggregateState* aggregate = nullptr;
    for (auto& candidate : state_.settlement_aggregates) {
        if (candidate.settlement_id == settlement_id) {
            aggregate = &candidate;
            break;
        }
    }

    const auto* settlement = find_settlement(settlement_id);
    if (aggregate == nullptr && settlement != nullptr) {
        state_.settlement_aggregates.push_back(SettlementAggregateState{
            .settlement_id = settlement_id,
            .region_id = settlement->region_id,
        });
        aggregate = &state_.settlement_aggregates.back();
    }

    if (aggregate == nullptr) {
        return;
    }

    std::uint64_t aggregate_population = 0;
    std::uint64_t dependent_population = 0;   // children and seniors
    std::uint64_t working_population = 0;     // young adults and adults
    std::uint64_t wealth_low_population = 0;
    std::uint64_t wealth_medium_population = 0;
    std::uint64_t wealth_high_population = 0;
    for (const auto& cohort : state_.population_cohorts) {
        if (cohort.settlement_id != settlement_id) {
            continue;
        }
        aggregate_population += cohort.population;
        if (cohort.age_band == CohortAgeBand::Child || cohort.age_band == CohortAgeBand::Senior) {
            dependent_population += cohort.population;
        } else {
            working_population += cohort.population;
        }
        switch (cohort.wealth_band) {
            case CohortWealthBand::Low:    wealth_low_population += cohort.population; break;
            case CohortWealthBand::Medium: wealth_medium_population += cohort.population; break;
            case CohortWealthBand::High:   wealth_high_population += cohort.population; break;
        }
    }

    const auto household_count = ceil_div_u64(aggregate_population, kAggregatePeoplePerHousehold);
    aggregate->household_count = household_count;

    // Size distribution tracks the dependency ratio: settlements carrying more
    // children and seniors hold proportionally more large households.
    if (household_count == 0U) {
        aggregate->small_household_count = 0U;
        aggregate->medium_household_count = 0U;
        aggregate->large_household_count = 0U;
    } else {
        const auto dependent_per_10k = (aggregate_population == 0U)
            ? 0U
            : (dependent_population * 10'000U) / aggregate_population;
        const auto large_share_per_10k = std::min<std::uint64_t>(6'000U, dependent_per_10k);
        const auto small_share_per_10k = 10'000U - large_share_per_10k -
            std::min<std::uint64_t>(10'000U - large_share_per_10k, 3'400U);
        aggregate->large_household_count = (household_count * large_share_per_10k) / 10'000U;
        aggregate->small_household_count = (household_count * small_share_per_10k) / 10'000U;
        aggregate->medium_household_count =
            household_count - aggregate->large_household_count - aggregate->small_household_count;
    }

    // Labour-capacity distribution from the working-age share.
    const auto working_per_10k = (aggregate_population == 0U)
        ? 0U
        : (working_population * 10'000U) / aggregate_population;
    aggregate->labour_high_household_count = (household_count * working_per_10k) / 10'000U;
    const auto labour_remainder = household_count - aggregate->labour_high_household_count;
    aggregate->labour_medium_household_count = labour_remainder / 2U;
    aggregate->labour_low_household_count = labour_remainder - aggregate->labour_medium_household_count;

    // Wealth distribution from the cohort wealth bands actually present.
    if (aggregate_population == 0U) {
        aggregate->wealth_low_household_count = 0U;
        aggregate->wealth_medium_household_count = 0U;
        aggregate->wealth_high_household_count = 0U;
    } else {
        aggregate->wealth_low_household_count =
            (household_count * wealth_low_population) / aggregate_population;
        aggregate->wealth_high_household_count =
            (household_count * wealth_high_population) / aggregate_population;
        aggregate->wealth_medium_household_count =
            household_count - aggregate->wealth_low_household_count - aggregate->wealth_high_household_count;
        (void)wealth_medium_population;
    }

    // Food-buffer distribution: days of stored food per household relative to
    // the settlement's daily requirement.
    if (household_count == 0U) {
        aggregate->food_buffer_low_household_count = 0U;
        aggregate->food_buffer_medium_household_count = 0U;
        aggregate->food_buffer_high_household_count = 0U;
    } else {
        const auto daily_requirement = std::max<std::int64_t>(1, static_cast<std::int64_t>(aggregate_population) * 2);
        const auto buffer_days_per_100 =
            (std::max<std::int64_t>(0, aggregate->food_inventory) * 100) / daily_requirement;
        if (buffer_days_per_100 >= 300) {
            aggregate->food_buffer_high_household_count = household_count;
            aggregate->food_buffer_medium_household_count = 0U;
            aggregate->food_buffer_low_household_count = 0U;
        } else if (buffer_days_per_100 >= 100) {
            aggregate->food_buffer_medium_household_count = household_count;
            aggregate->food_buffer_high_household_count = 0U;
            aggregate->food_buffer_low_household_count = 0U;
        } else {
            aggregate->food_buffer_low_household_count = household_count;
            aggregate->food_buffer_medium_household_count = 0U;
            aggregate->food_buffer_high_household_count = 0U;
        }
    }

    if (settlement != nullptr) {
        aggregate->productive_access_points = static_cast<std::int64_t>(settlement->access_share_points);
    }

    std::sort(state_.settlement_aggregates.begin(), state_.settlement_aggregates.end(), [](const SettlementAggregateState& lhs, const SettlementAggregateState& rhs) {
        return lhs.settlement_id < rhs.settlement_id;
    });
}

void Simulation::rebuild_global_detail_level() {
    SimulationDetailLevel level = SimulationDetailLevel::Detailed;
    for (const auto& region_lod : state_.region_lod_states) {
        if (region_lod.detail_level == SimulationDetailLevel::RegionalCohort) {
            level = SimulationDetailLevel::RegionalCohort;
            break;
        }
        if (region_lod.detail_level == SimulationDetailLevel::SettlementAggregate) {
            level = SimulationDetailLevel::SettlementAggregate;
        }
    }
    state_.detail_level = level;
}

RegionEnvironmentState* Simulation::find_region_mutable(const RegionId region_id) noexcept {
    for (auto& region : state_.regions) {
        if (region.id == region_id) {
            return &region;
        }
    }
    return nullptr;
}

const RegionEnvironmentState* Simulation::find_region(const RegionId region_id) const noexcept {
    for (const auto& region : state_.regions) {
        if (region.id == region_id) {
            return &region;
        }
    }
    return nullptr;
}

Settlement* Simulation::find_settlement_mutable(const SettlementId settlement_id) noexcept {
    for (auto& settlement : state_.settlements) {
        if (settlement.id == settlement_id) {
            return &settlement;
        }
    }
    return nullptr;
}

const Settlement* Simulation::find_settlement(const SettlementId settlement_id) const noexcept {
    for (const auto& settlement : state_.settlements) {
        if (settlement.id == settlement_id) {
            return &settlement;
        }
    }
    return nullptr;
}

std::int64_t Simulation::settlement_accessible_food_capacity_per_day(const Settlement& settlement) const noexcept {
    if (!settlement.active || settlement.access_share_points == 0U) {
        return 0;
    }

    const auto* region = find_region(settlement.region_id);
    if (region == nullptr) {
        return 0;
    }

    const auto productive = (region->productive_capacity_per_day * static_cast<std::int64_t>(settlement.access_share_points)) /
        static_cast<std::int64_t>(kSettlementShareScale);
    const auto wild = (region->wild_food_harvest_limit_per_day * static_cast<std::int64_t>(settlement.access_share_points)) /
        static_cast<std::int64_t>(kSettlementShareScale);
    std::int64_t capacity = 0;
    if (!checked_add_i64(productive, wild, capacity)) {
        capacity = std::numeric_limits<std::int64_t>::max();
    }
    if (!checked_add_i64(capacity, settlement.dwelling_capacity_bonus, capacity)) {
        capacity = std::numeric_limits<std::int64_t>::max();
    }
    if (!checked_add_i64(capacity, settlement.food_store_capacity_bonus, capacity)) {
        capacity = std::numeric_limits<std::int64_t>::max();
    }
    return std::max<std::int64_t>(0, capacity);
}

std::uint32_t Simulation::settlement_pressure_per_10k(const Settlement& settlement) const noexcept {
    if (!settlement.active) {
        return 0U;
    }

    std::int64_t total_required = 0;
    for (const auto& household : state_.households) {
        if (household.settlement_id != settlement.id) {
            continue;
        }
        for (const auto member_id : household.member_ids) {
            for (const auto& person : state_.people) {
                if (person.id == member_id && person.alive) {
                    total_required += food_requirement_for_person(person);
                    break;
                }
            }
        }
    }

    for (const auto& cohort : state_.population_cohorts) {
        if (cohort.settlement_id != settlement.id) {
            continue;
        }
        const auto cohort_required = static_cast<std::int64_t>(std::min<std::uint64_t>(
            cohort.population,
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 2U))) * 2;
        total_required += cohort_required;
    }

    if (total_required <= 0) {
        return 0U;
    }

    const auto capacity = settlement_accessible_food_capacity_per_day(settlement);
    if (capacity <= 0) {
        return 100'000U;
    }

    const auto ratio = (total_required * 10'000) / capacity;
    if (ratio < 0) {
        return 0U;
    }
    return static_cast<std::uint32_t>(std::min<std::int64_t>(ratio, 100'000));
}

void Simulation::process_settlement_extinction() {
    for (auto& settlement : state_.settlements) {
        if (!settlement.active) {
            continue;
        }

        std::uint64_t alive_members = 0;
        for (const auto& household : state_.households) {
            if (household.settlement_id != settlement.id) {
                continue;
            }
            for (const auto member_id : household.member_ids) {
                for (const auto& person : state_.people) {
                    if (person.id == member_id && person.alive) {
                        ++alive_members;
                        break;
                    }
                }
            }
        }

        std::uint64_t aggregate_members = 0;
        for (const auto& cohort : state_.population_cohorts) {
            if (cohort.settlement_id == settlement.id) {
                aggregate_members += cohort.population;
            }
        }

        if (aggregate_members > 0U) {
            continue;
        }

        if (alive_members == 0U) {
            settlement.active = false;
            settlement.access_share_points = 0U;
            emit_settlement_extinct_event(settlement.id, settlement.region_id);
        }
    }
}

// Chooses which of a source settlement's households move to a new settlement,
// without changing any state, so a split that cannot meet the minimum sizes
// leaves the world untouched. The order is scoped by the source and the year,
// never by consumption order. The returned households list the movers first.
std::optional<Simulation::FissionPlan> Simulation::plan_fission_households(
    const Settlement& source,
    const std::uint64_t demographic_year,
    const std::unordered_set<PersonId>& alive_person_ids) {
    struct Candidate {
        Household* household{};
        std::uint64_t alive_members{};
    };

    std::vector<Candidate> candidates;
    std::uint64_t source_population = 0;
    for (auto& household : state_.households) {
        if (household.settlement_id != source.id) {
            continue;
        }
        std::uint64_t alive_members = 0;
        for (const auto member_id : household.member_ids) {
            alive_members += alive_person_ids.contains(member_id) ? 1U : 0U;
        }
        if (alive_members == 0U) {
            continue;
        }
        candidates.push_back(Candidate{.household = &household, .alive_members = alive_members});
        source_population += alive_members;
    }

    if (source_population < kFissionMinimumSourcePopulation + kFissionMinimumDaughterPopulation) {
        return std::nullopt;
    }

    std::sort(candidates.begin(), candidates.end(), [&](const Candidate& lhs, const Candidate& rhs) {
        const auto lhs_seed = derive_seed(household_seed(lhs.household->id), SeedDomain::SettlementHouseholdSelection, demographic_year ^ source.id);
        const auto rhs_seed = derive_seed(household_seed(rhs.household->id), SeedDomain::SettlementHouseholdSelection, demographic_year ^ source.id);
        if (lhs_seed.value != rhs_seed.value) {
            return lhs_seed.value < rhs_seed.value;
        }
        return lhs.household->id < rhs.household->id;
    });

    const auto max_moved_population = source_population - kFissionMinimumSourcePopulation;
    const auto target_moved_population =
        std::max<std::uint64_t>(kFissionMinimumDaughterPopulation, source_population / kFissionDaughterTargetDivisor);

    std::vector<Household*> moving_households;
    std::vector<Household*> staying_households;
    std::uint64_t moved_population = 0;
    for (const auto& candidate : candidates) {
        const bool fits = moved_population + candidate.alive_members <= max_moved_population;
        if (fits && moved_population < target_moved_population) {
            moving_households.push_back(candidate.household);
            moved_population += candidate.alive_members;
        } else {
            staying_households.push_back(candidate.household);
        }
    }
    if (moved_population < kFissionMinimumDaughterPopulation) {
        return std::nullopt;
    }

    FissionPlan plan;
    plan.move_count = static_cast<std::uint64_t>(moving_households.size());
    plan.households = std::move(moving_households);
    plan.households.insert(plan.households.end(), staying_households.begin(), staying_households.end());
    return plan;
}

void Simulation::process_settlement_founding(const std::uint64_t demographic_year) {
    std::unordered_set<PersonId> alive_person_ids;
    alive_person_ids.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        if (person.alive) {
            alive_person_ids.insert(person.id);
        }
    }

    std::vector<RegionId> region_ids;
    region_ids.reserve(state_.regions.size());
    for (const auto& region : state_.regions) {
        region_ids.push_back(region.id);
    }
    std::sort(region_ids.begin(), region_ids.end());

    for (const auto region_id : region_ids) {
        std::vector<Settlement*> active_settlements;
        active_settlements.reserve(state_.settlements.size());
        std::uint32_t allocated_share = 0;
        for (auto& settlement : state_.settlements) {
            if (!settlement.active || settlement.region_id != region_id) {
                continue;
            }
            active_settlements.push_back(&settlement);
            allocated_share = std::min<std::uint32_t>(kSettlementShareScale, allocated_share + settlement.access_share_points);
        }

        if (active_settlements.empty()) {
            continue;
        }

        const auto unallocated_share = (allocated_share >= kSettlementShareScale)
            ? 0U
            : (kSettlementShareScale - allocated_share);

        std::sort(active_settlements.begin(), active_settlements.end(), [](const Settlement* lhs, const Settlement* rhs) {
            return lhs->id < rhs->id;
        });

        Settlement* source = nullptr;
        std::uint32_t best_pressure = 0;

        for (auto* candidate : active_settlements) {
            std::uint64_t alive_population = 0;
            std::uint64_t eligible_workers = 0;
            std::int64_t required_food = 0;
            std::int64_t stored_food = std::max<std::int64_t>(0, candidate->commons_food.value);
            for (const auto& household : state_.households) {
                if (household.region_id != region_id || household.settlement_id != candidate->id) {
                    continue;
                }

                if (!checked_add_i64(stored_food, household.resources.food_inventory.value, stored_food)) {
                    throw std::overflow_error("Settlement founding stored food aggregation overflow");
                }

                for (const auto member_id : household.member_ids) {
                    for (const auto& person : state_.people) {
                        if (person.id == member_id && person.alive) {
                            ++alive_population;
                            if (!checked_add_i64(required_food, food_requirement_for_person(person), required_food)) {
                                throw std::overflow_error("Settlement founding required food aggregation overflow");
                            }
                            if (occupation_eligible_for_productive_work(person)) {
                                ++eligible_workers;
                            }
                        }
                    }
                }
            }

            const bool is_overgrown = alive_population >= kFissionPopulationThreshold;
            const bool is_strained = alive_population >= kFissionStrainMinimumPopulation &&
                candidate->days_under_food_strain >= kFissionStrainDays;
            if (!is_overgrown && !is_strained) {
                continue;
            }

            if (required_food <= 0 || eligible_workers < kFoundingMinimumEligibleWorkers) {
                continue;
            }

            const auto pressure = settlement_pressure_per_10k(*candidate);

            std::uint32_t candidate_assigned_share = 0U;
            if (unallocated_share >= kFoundingMinimumUnallocatedShare) {
                candidate_assigned_share = std::min<std::uint32_t>(unallocated_share, kFoundingDefaultShare);
            } else if (candidate->access_share_points > kFoundingMinimumSourceShareAfterSplit) {
                const auto transferable = candidate->access_share_points - kFoundingMinimumSourceShareAfterSplit;
                candidate_assigned_share = std::min<std::uint32_t>(transferable, kFoundingDefaultShare);
            }

            if (candidate_assigned_share == 0U) {
                continue;
            }

            if (source == nullptr || pressure > best_pressure || (pressure == best_pressure && candidate->id < source->id)) {
                source = candidate;
                best_pressure = pressure;
            }
        }

        if (source == nullptr) {
            continue;
        }

        auto plan = plan_fission_households(*source, demographic_year, alive_person_ids);
        if (!plan.has_value()) {
            continue;
        }
        auto& source_alive_households = plan->households;
        const auto move_count = plan->move_count;

        // Decide the land share before changing anything, so a split that cannot
        // get one consumes no settlement id and resets no strain count.
        std::uint32_t assigned_share = 0U;
        bool share_comes_from_source = false;
        if (unallocated_share >= kFoundingMinimumUnallocatedShare) {
            assigned_share = std::min<std::uint32_t>(unallocated_share, kFoundingDefaultShare);
        } else if (source->access_share_points > kFoundingMinimumSourceShareAfterSplit) {
            const auto transferable = source->access_share_points - kFoundingMinimumSourceShareAfterSplit;
            assigned_share = std::min<std::uint32_t>(transferable, kFoundingDefaultShare);
            share_comes_from_source = true;
        }

        if (assigned_share == 0U) {
            continue;
        }

        const auto new_settlement_id = next_settlement_id();
        const auto source_settlement_id = source->id;
        if (share_comes_from_source) {
            source->access_share_points -= assigned_share;
        }
        // The split is the response to strain, so the source starts counting again.
        source->days_under_food_strain = 0U;

        state_.settlements.push_back(Settlement{
            .id = new_settlement_id,
            .region_id = region_id,
            .active = true,
            .access_share_points = assigned_share,
        });
        assign_position_for_new_settlement(new_settlement_id, region_id);
        state_.settlement_aggregates.push_back(SettlementAggregateState{
            .settlement_id = new_settlement_id,
            .region_id = region_id,
        });
        std::sort(state_.settlement_aggregates.begin(), state_.settlement_aggregates.end(), [](const SettlementAggregateState& lhs, const SettlementAggregateState& rhs) {
            return lhs.settlement_id < rhs.settlement_id;
        });

        for (std::uint64_t i = 0; i < move_count; ++i) {
            source_alive_households[i]->settlement_id = new_settlement_id;
        }

        std::vector<PersonId> moved_observers;
        for (std::uint64_t i = 0; i < move_count; ++i) {
            for (const auto member_id : source_alive_households[i]->member_ids) {
                if (alive_person_ids.contains(member_id)) {
                    moved_observers.push_back(member_id);
                }
            }
        }
        std::sort(moved_observers.begin(), moved_observers.end());
        moved_observers.erase(std::unique(moved_observers.begin(), moved_observers.end()), moved_observers.end());

        const auto event_id = emit_settlement_founded_event(new_settlement_id, region_id, source_settlement_id);
        append_direct_observation_memories(event_id, moved_observers, state_.time);

        std::vector<const Household*> moved_households(
            source_alive_households.begin(), source_alive_households.begin() + static_cast<std::ptrdiff_t>(move_count));
        std::sort(moved_households.begin(), moved_households.end(), [](const Household* lhs, const Household* rhs) {
            return lhs->id < rhs->id;
        });
        for (const auto* household : moved_households) {
            emit_household_relocated_event(*household, source_settlement_id, new_settlement_id);
        }
    }

    std::sort(state_.settlements.begin(), state_.settlements.end(), [](const Settlement& lhs, const Settlement& rhs) {
        return lhs.id < rhs.id;
    });
}

// The aggregate ledger of a settlement in a demoted region moves with it, so
// the stock and household bands its population groups carry are not stranded.
static void merge_aggregate_ledgers(
    std::vector<SettlementAggregateState>& ledgers, const SettlementId survivor_id, const SettlementId absorbed_id) {
    SettlementAggregateState* survivor_ledger = nullptr;
    SettlementAggregateState* absorbed_ledger = nullptr;
    for (auto& ledger : ledgers) {
        if (ledger.settlement_id == survivor_id) {
            survivor_ledger = &ledger;
        } else if (ledger.settlement_id == absorbed_id) {
            absorbed_ledger = &ledger;
        }
    }
    if (survivor_ledger != nullptr && absorbed_ledger != nullptr) {
        survivor_ledger->household_count += absorbed_ledger->household_count;
        survivor_ledger->small_household_count += absorbed_ledger->small_household_count;
        survivor_ledger->medium_household_count += absorbed_ledger->medium_household_count;
        survivor_ledger->large_household_count += absorbed_ledger->large_household_count;
        survivor_ledger->food_inventory += absorbed_ledger->food_inventory;
        survivor_ledger->food_produced_total += absorbed_ledger->food_produced_total;
        survivor_ledger->food_consumed_total += absorbed_ledger->food_consumed_total;
        survivor_ledger->unmet_food_total += absorbed_ledger->unmet_food_total;
        survivor_ledger->labour_capacity += absorbed_ledger->labour_capacity;
        survivor_ledger->labour_demand += absorbed_ledger->labour_demand;
        survivor_ledger->labour_allocated += absorbed_ledger->labour_allocated;
        survivor_ledger->labour_low_household_count += absorbed_ledger->labour_low_household_count;
        survivor_ledger->labour_medium_household_count += absorbed_ledger->labour_medium_household_count;
        survivor_ledger->labour_high_household_count += absorbed_ledger->labour_high_household_count;
        survivor_ledger->wealth_low_household_count += absorbed_ledger->wealth_low_household_count;
        survivor_ledger->wealth_medium_household_count += absorbed_ledger->wealth_medium_household_count;
        survivor_ledger->wealth_high_household_count += absorbed_ledger->wealth_high_household_count;
        survivor_ledger->food_buffer_low_household_count += absorbed_ledger->food_buffer_low_household_count;
        survivor_ledger->food_buffer_medium_household_count += absorbed_ledger->food_buffer_medium_household_count;
        survivor_ledger->food_buffer_high_household_count += absorbed_ledger->food_buffer_high_household_count;
        survivor_ledger->wealth_points_total += absorbed_ledger->wealth_points_total;
        survivor_ledger->productive_access_points += absorbed_ledger->productive_access_points;
        survivor_ledger->required_food_year += absorbed_ledger->required_food_year;
        survivor_ledger->unmet_food_year += absorbed_ledger->unmet_food_year;
        *absorbed_ledger = SettlementAggregateState{
            .settlement_id = absorbed_ledger->settlement_id,
            .region_id = absorbed_ledger->region_id,
        };
    }
}

void Simulation::merge_settlements(const SettlementId survivor_id, const SettlementId absorbed_id) {
    auto* survivor = find_settlement_mutable(survivor_id);
    auto* absorbed = find_settlement_mutable(absorbed_id);
    if (survivor == nullptr || absorbed == nullptr) {
        return;
    }

    std::vector<HouseholdId> relocated_household_ids;
    for (auto& household : state_.households) {
        if (household.settlement_id == absorbed_id) {
            household.settlement_id = survivor_id;
            household.region_id = survivor->region_id;
            relocated_household_ids.push_back(household.id);
        }
    }
    std::sort(relocated_household_ids.begin(), relocated_household_ids.end());
    for (auto& cohort : state_.population_cohorts) {
        if (cohort.settlement_id == absorbed_id) {
            cohort.settlement_id = survivor_id;
        }
    }
    for (auto& institution : state_.institutions) {
        if (institution.settlement_id == absorbed_id) {
            institution.settlement_id = survivor_id;
        }
    }
    for (auto& record : state_.knowledge_records) {
        if (record.settlement_id == absorbed_id) {
            record.settlement_id = survivor_id;
        }
    }
    // An access must stay within its record's settlement, so readers of the
    // moved records now read in the survivor. The semantic key indexes are
    // keyed by settlement, so they are rebuilt to match what a reload produces.
    for (auto& access : state_.knowledge_record_accesses) {
        if (access.reader_settlement_id == absorbed_id) {
            access.reader_settlement_id = survivor_id;
        }
    }
    rebuild_record_indexes();

    survivor->materials.wood.value += absorbed->materials.wood.value;
    survivor->materials.stone.value += absorbed->materials.stone.value;
    survivor->materials.timber.value += absorbed->materials.timber.value;
    survivor->materials.basic_tool.value += absorbed->materials.basic_tool.value;
    survivor->cumulative_wood_extracted += absorbed->cumulative_wood_extracted;
    survivor->cumulative_stone_extracted += absorbed->cumulative_stone_extracted;
    survivor->cumulative_timber_produced += absorbed->cumulative_timber_produced;
    survivor->cumulative_basic_tool_produced += absorbed->cumulative_basic_tool_produced;
    survivor->cumulative_basic_tool_broken += absorbed->cumulative_basic_tool_broken;

    absorbed->materials.wood.value = 0;
    absorbed->materials.stone.value = 0;
    absorbed->materials.timber.value = 0;
    absorbed->materials.basic_tool.value = 0;

    // Commons food is finite regional stock, so it moves with the households.
    survivor->commons_food.value += absorbed->commons_food.value;
    if (!checked_add_i64(survivor->coin, absorbed->coin, survivor->coin)) {
        throw std::overflow_error("Settlement coin overflow while merging");
    }
    absorbed->coin = 0;
    absorbed->commons_food.value = 0;

    merge_aggregate_ledgers(state_.settlement_aggregates, survivor_id, absorbed_id);

    const auto combined_share =
        static_cast<std::uint64_t>(survivor->access_share_points) + static_cast<std::uint64_t>(absorbed->access_share_points);
    survivor->access_share_points = static_cast<std::uint32_t>(std::min<std::uint64_t>(combined_share, kSettlementShareScale));

    const auto absorbed_region_id = absorbed->region_id;
    absorbed->active = false;
    absorbed->access_share_points = 0U;

    emit_settlement_merged_event(survivor_id, absorbed_id, absorbed_region_id);

    // Absorbed buildings keep their settlement_id and canonical placement,
    // standing where they were built as the absorbed site's old quarter.
    for (const auto household_id : relocated_household_ids) {
        for (const auto& household : state_.households) {
            if (household.id == household_id) {
                if (alive_count_in_household(household_id) > 0U) {
                    emit_household_relocated_event(household, absorbed_id, survivor_id);
                }
                break;
            }
        }
    }
}

// People alive in each settlement right now, from households and aggregate
// cohorts. Settlement::population is refreshed daily, so inside the annual
// restructuring it still lacks that year's births.
static std::unordered_map<SettlementId, std::uint64_t> live_population_by_settlement(const WorldState& state) {
    std::unordered_set<PersonId> alive_person_ids;
    alive_person_ids.reserve(state.people.size());
    for (const auto& person : state.people) {
        if (person.alive) {
            alive_person_ids.insert(person.id);
        }
    }

    std::unordered_map<SettlementId, std::uint64_t> population;
    for (const auto& household : state.households) {
        for (const auto member_id : household.member_ids) {
            if (alive_person_ids.contains(member_id)) {
                ++population[household.settlement_id];
            }
        }
    }
    for (const auto& cohort : state.population_cohorts) {
        population[cohort.settlement_id] += cohort.population;
    }
    return population;
}

// ADR 0002: a settlement that has stayed below the viable size for five years
// is absorbed by the nearest viable neighbour in its region. Only the survivor
// must show sustained viability; the absorbed side needs no streak or minimum
// population of its own, which is what lets the small settlements fission
// leaves behind actually be resolved. The survivor is judged on its live
// population, so one that has just grown past the viable size can absorb now.
void Simulation::process_settlement_merging() {
    auto live_population = live_population_by_settlement(state_);
    std::vector<SettlementId> absorbed_candidate_ids;
    for (const auto& settlement : state_.settlements) {
        if (settlement.active && settlement.days_below_viable_size >= kAbsorptionBelowViableDays) {
            absorbed_candidate_ids.push_back(settlement.id);
        }
    }
    std::sort(absorbed_candidate_ids.begin(), absorbed_candidate_ids.end());

    for (const auto absorbed_id : absorbed_candidate_ids) {
        const auto* absorbed = find_settlement(absorbed_id);
        if (absorbed == nullptr || !absorbed->active) {
            continue;
        }

        const Settlement* survivor = nullptr;
        std::int64_t survivor_distance_squared = 0;
        for (const auto& candidate : state_.settlements) {
            if (!candidate.active || candidate.id == absorbed_id || candidate.region_id != absorbed->region_id) {
                continue;
            }
            if (live_population[candidate.id] < kViableSettlementSize ||
                candidate.economic_viability_streak < kSurvivorViabilityStreakDays) {
                continue;
            }

            const auto dx = candidate.position_x - absorbed->position_x;
            const auto dz = candidate.position_z - absorbed->position_z;
            const auto distance_squared = (dx * dx) + (dz * dz);
            if (distance_squared > kSettlementMergeDistanceUnits * kSettlementMergeDistanceUnits) {
                continue;
            }

            const bool is_better = survivor == nullptr || distance_squared < survivor_distance_squared ||
                (distance_squared == survivor_distance_squared &&
                 (live_population[candidate.id] > live_population[survivor->id] ||
                  (live_population[candidate.id] == live_population[survivor->id] && candidate.id < survivor->id)));
            if (is_better) {
                survivor = &candidate;
                survivor_distance_squared = distance_squared;
            }
        }

        if (survivor != nullptr) {
            live_population[survivor->id] += live_population[absorbed_id];
            live_population[absorbed_id] = 0U;
            merge_settlements(survivor->id, absorbed_id);
        }
    }
}

void Simulation::process_settlement_restructuring(const std::uint64_t demographic_year) {
    process_settlement_extinction();
    process_settlement_founding(demographic_year);
    process_settlement_merging();
    process_settlement_extinction();
}

void Simulation::clear_occupations_for_extinct_or_invalid_households() {
    std::unordered_map<HouseholdId, SettlementId> household_settlement;
    household_settlement.reserve(state_.households.size());
    for (const auto& household : state_.households) {
        household_settlement.emplace(household.id, household.settlement_id);
    }

    std::unordered_map<SettlementId, bool> settlement_active;
    settlement_active.reserve(state_.settlements.size());
    for (const auto& settlement : state_.settlements) {
        settlement_active.emplace(settlement.id, settlement.active);
    }

    for (auto& person : state_.people) {
        if (!occupation_eligible_for_productive_work(person)) {
            person.occupation = Occupation::None;
            continue;
        }

        const auto household_it = household_settlement.find(person.household_id);
        if (household_it == household_settlement.end()) {
            person.occupation = Occupation::None;
            continue;
        }

        const auto settlement_it = settlement_active.find(household_it->second);
        if (settlement_it == settlement_active.end() || !settlement_it->second) {
            person.occupation = Occupation::None;
        }
    }
}

void Simulation::process_occupation_reassignment(const std::uint64_t demographic_year) {
    (void)demographic_year;
    clear_occupations_for_extinct_or_invalid_households();

    std::unordered_map<PersonId, Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::vector<SettlementId> settlement_ids;
    settlement_ids.reserve(state_.settlements.size());
    for (const auto& settlement : state_.settlements) {
        if (settlement.active) {
            settlement_ids.push_back(settlement.id);
        }
    }
    std::sort(settlement_ids.begin(), settlement_ids.end());

    for (const auto settlement_id : settlement_ids) {
        const auto* settlement = find_settlement(settlement_id);
        if (settlement == nullptr || !settlement->active) {
            continue;
        }

        const auto* region = find_region(settlement->region_id);
        if (region == nullptr) {
            continue;
        }

        std::int64_t settlement_required_food = 0;
        std::int64_t settlement_stored_food = std::max<std::int64_t>(0, settlement->commons_food.value);
        std::uint64_t settlement_alive_population = 0;

        struct Candidate {
            Person* person{};
            std::int64_t labour_capacity{};
            bool assigned{false};
        };

        std::vector<Candidate> candidates;
        candidates.reserve(128);

        for (const auto& household : state_.households) {
            if (household.settlement_id != settlement_id) {
                continue;
            }

            if (!checked_add_i64(
                    settlement_stored_food,
                    household.resources.food_inventory.value,
                    settlement_stored_food)) {
                throw std::overflow_error("Settlement stored food aggregation overflow");
            }

            for (const auto member_id : household.member_ids) {
                const auto person_it = person_by_id.find(member_id);
                if (person_it == person_by_id.end()) {
                    continue;
                }
                Person* person = person_it->second;
                if (!person->alive) {
                    continue;
                }

                ++settlement_alive_population;

                const auto required = food_requirement_for_person(*person);
                if (!checked_add_i64(settlement_required_food, required, settlement_required_food)) {
                    throw std::overflow_error("Settlement required food aggregation overflow");
                }

                if (!occupation_eligible_for_productive_work(*person)) {
                    person->occupation = Occupation::None;
                    continue;
                }

                // A merchant is not a labour candidate: it is trained and
                // retired by process_merchant_recruitment, not by allocation.
                if (person->occupation == Occupation::Merchant) {
                    continue;
                }

                candidates.push_back(Candidate{
                    .person = person,
                    .labour_capacity = productive_effort_for_person(*person),
                    .assigned = false,
                });
            }
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& lhs, const Candidate& rhs) {
            return lhs.person->id < rhs.person->id;
        });

        const auto construction_demand = construction_demand_for_settlement(settlement_id, state_.buildings);

        const auto cultivation_opportunity =
            (region->productive_capacity_per_day * static_cast<std::int64_t>(settlement->access_share_points)) /
            static_cast<std::int64_t>(kSettlementShareScale);
        const auto gathering_opportunity =
            (std::min(region->wild_food_stock, region->wild_food_harvest_limit_per_day) *
             static_cast<std::int64_t>(settlement->access_share_points)) /
            static_cast<std::int64_t>(kSettlementShareScale);

        std::int64_t output_opportunity = 0;
        if (!checked_add_i64(cultivation_opportunity, gathering_opportunity, output_opportunity)) {
            throw std::overflow_error("Food output opportunity overflow");
        }

        const auto base_food_target = compute_daily_food_output_target(
            settlement_required_food,
            settlement_stored_food,
            output_opportunity);

        auto assignment_target = base_food_target;
        std::int64_t commons_target = 0;
        if (!checked_mul_i64(settlement_required_food, kCommonsReserveTargetDays, commons_target)) {
            throw std::overflow_error("Commons reserve target overflow");
        }
        commons_target = std::max<std::int64_t>(0, commons_target);
        const auto commons_deficit = std::max<std::int64_t>(0, commons_target - std::max<std::int64_t>(0, settlement->commons_food.value));
        if (commons_deficit > 0) {
            const auto replenish_target = ceil_div_u64(
                static_cast<std::uint64_t>(commons_deficit),
                static_cast<std::uint64_t>(std::max<std::int64_t>(1, kCommonsReplenishDays)));

            std::int64_t replenished_target = 0;
            if (!checked_add_i64(base_food_target, static_cast<std::int64_t>(replenish_target), replenished_target)) {
                throw std::overflow_error("Replenished assignment target overflow");
            }

            assignment_target = std::max(
                assignment_target,
                std::min<std::int64_t>(
                    output_opportunity,
                    replenished_target));
        }

        const auto pressure = settlement_pressure_per_10k(*settlement);
        const auto food_buffer_days = settlement_required_food > 0
            ? settlement_stored_food / std::max<std::int64_t>(1, settlement_required_food)
            : 0;
        const bool food_crisis =
            settlement_required_food > 0 &&
            (pressure >= kHighPressureThresholdPer10k || food_buffer_days < kFoodCrisisBufferDays);

        if (settlement_required_food > 0) {
            std::int64_t required_coverage_target = 0;
            if (!checked_mul_i64(settlement_required_food, kAssignmentRequiredCoveragePercent, required_coverage_target)) {
                throw std::overflow_error("Required food coverage target overflow");
            }
            required_coverage_target /= 100;
            assignment_target = std::max(assignment_target, required_coverage_target);
        }
        assignment_target = std::min(assignment_target, output_opportunity);
        const auto demand = split_demand_by_opportunity(assignment_target, cultivation_opportunity, gathering_opportunity);

        std::int64_t cultivation_remaining = std::max<std::int64_t>(0, demand.cultivation_labour_demand);
        std::int64_t gathering_remaining = std::max<std::int64_t>(0, demand.gathering_labour_demand);

        for (auto& candidate : candidates) {
            if (candidate.person->occupation != Occupation::SubsistenceCultivator || cultivation_remaining <= 0) {
                continue;
            }
            candidate.assigned = true;
            candidate.person->occupation = Occupation::SubsistenceCultivator;
            cultivation_remaining = std::max<std::int64_t>(0, cultivation_remaining - candidate.labour_capacity);
        }

        for (auto& candidate : candidates) {
            if (candidate.assigned || candidate.person->occupation != Occupation::WildFoodGatherer || gathering_remaining <= 0) {
                continue;
            }
            candidate.assigned = true;
            candidate.person->occupation = Occupation::WildFoodGatherer;
            gathering_remaining = std::max<std::int64_t>(0, gathering_remaining - candidate.labour_capacity);
        }

        for (auto& candidate : candidates) {
            if (candidate.assigned) {
                continue;
            }

            if (cultivation_remaining <= 0 && gathering_remaining <= 0) {
                break;
            }

            if (cultivation_remaining >= gathering_remaining && cultivation_remaining > 0) {
                candidate.person->occupation = Occupation::SubsistenceCultivator;
                candidate.assigned = true;
                cultivation_remaining = std::max<std::int64_t>(0, cultivation_remaining - candidate.labour_capacity);
            } else if (gathering_remaining > 0) {
                candidate.person->occupation = Occupation::WildFoodGatherer;
                candidate.assigned = true;
                gathering_remaining = std::max<std::int64_t>(0, gathering_remaining - candidate.labour_capacity);
            }
        }

        auto* settlement_mutable = find_settlement_mutable(settlement_id);
        if (settlement_mutable != nullptr) {
            std::uint64_t eligible_workers = 0;
            for (const auto& candidate : candidates) {
                if (candidate.labour_capacity > 0) {
                    ++eligible_workers;
                }
            }

            const auto targets = material_targets_for_settlement(settlement_alive_population, eligible_workers);
            const auto tool_deficit = std::max<std::int64_t>(0, targets.basic_tool_target - settlement_mutable->materials.basic_tool.value);
            std::int64_t timber_target_with_construction = 0;
            if (!checked_add_i64(targets.timber_target, construction_demand.timber_remaining, timber_target_with_construction)) {
                throw std::overflow_error("Settlement timber target overflow");
            }
            std::int64_t stone_target_with_construction = 0;
            if (!checked_add_i64(targets.stone_target, construction_demand.stone_remaining, stone_target_with_construction)) {
                throw std::overflow_error("Settlement stone target overflow");
            }
            const auto timber_deficit = std::max<std::int64_t>(0, timber_target_with_construction - settlement_mutable->materials.timber.value);
            const auto stone_deficit = std::max<std::int64_t>(0, stone_target_with_construction - settlement_mutable->materials.stone.value);

            const auto wood_reserve_need = std::max<std::int64_t>(0, targets.wood_target - settlement_mutable->materials.wood.value);
            std::int64_t timber_wood_need = 0;
            std::int64_t tool_wood_need = 0;
            if (!checked_mul_i64(timber_deficit, material_parameters_.wood_per_timber, timber_wood_need) ||
                !checked_mul_i64(tool_deficit, material_parameters_.wood_per_basic_tool, tool_wood_need)) {
                throw std::overflow_error("Settlement wood material need overflow");
            }
            std::int64_t wood_total_need = 0;
            if (!checked_add_i64(wood_reserve_need, timber_wood_need, wood_total_need) ||
                !checked_add_i64(wood_total_need, tool_wood_need, wood_total_need)) {
                throw std::overflow_error("Settlement total wood need overflow");
            }

            std::int64_t tool_stone_need = 0;
            if (!checked_mul_i64(tool_deficit, material_parameters_.stone_per_basic_tool, tool_stone_need)) {
                throw std::overflow_error("Settlement stone material need overflow");
            }
            std::int64_t stone_total_need = 0;
            if (!checked_add_i64(stone_deficit, tool_stone_need, stone_total_need)) {
                throw std::overflow_error("Settlement total stone need overflow");
            }

            std::int64_t craft_need = 0;
            if (!checked_add_i64(timber_deficit, tool_deficit, craft_need)) {
                throw std::overflow_error("Settlement craft need overflow");
            }
            const bool prioritize_craft_for_tools = tool_deficit > 0;

            if (food_crisis) {
                wood_total_need /= 4;
                stone_total_need /= 4;
                craft_need /= 4;
            }

            auto assign_for_occupation = [&](const Occupation occupation, std::int64_t& demand_value) {
                if (demand_value <= 0) {
                    return false;
                }

                for (auto& candidate : candidates) {
                    if (candidate.assigned || candidate.person->occupation != occupation) {
                        continue;
                    }
                    candidate.assigned = true;
                    candidate.person->occupation = occupation;
                    demand_value = std::max<std::int64_t>(0, demand_value - candidate.labour_capacity);
                    return true;
                }

                for (auto& candidate : candidates) {
                    if (candidate.assigned) {
                        continue;
                    }
                    candidate.assigned = true;
                    candidate.person->occupation = occupation;
                    demand_value = std::max<std::int64_t>(0, demand_value - candidate.labour_capacity);
                    return true;
                }

                return false;
            };

            while (true) {
                const auto has_demand = wood_total_need > 0 || stone_total_need > 0 || craft_need > 0;
                if (!has_demand) {
                    break;
                }

                bool any_unassigned = false;
                for (const auto& candidate : candidates) {
                    if (!candidate.assigned) {
                        any_unassigned = true;
                        break;
                    }
                }
                if (!any_unassigned) {
                    break;
                }

                bool assigned = false;
                if (prioritize_craft_for_tools && craft_need > 0 && wood_total_need <= 0 && stone_total_need <= 0) {
                    assigned = assign_for_occupation(Occupation::CraftWorker, craft_need);
                } else if (wood_total_need >= stone_total_need && wood_total_need >= craft_need && wood_total_need > 0) {
                    assigned = assign_for_occupation(Occupation::WoodGatherer, wood_total_need);
                } else if (stone_total_need >= craft_need && stone_total_need > 0) {
                    assigned = assign_for_occupation(Occupation::StoneExtractor, stone_total_need);
                } else if (craft_need > 0) {
                    assigned = assign_for_occupation(Occupation::CraftWorker, craft_need);
                } else {
                    break;
                }

                if (!assigned) {
                    break;
                }
            }
        }

        for (auto& candidate : candidates) {
            if (!candidate.assigned) {
                candidate.person->occupation = Occupation::None;
            }
        }
    }

    refresh_labour_aggregates();
}

// ADR 0003: trade grows where it pays. Once a year each settlement compares its
// prices with those of the neighbours within a day's walk, on goods its commons
// actually holds. If the best margin clears the threshold it trains one adult
// as a merchant (seeded choice among its eligible adults who have no assignment, in ascending id
// order); if none does, its merchants retire to ordinary labour. A world with
// fewer than two active settlements has nowhere to trade, so it has no merchants.
// This runs inside the occupation bracket, so every appointment and retirement
// is recorded by the existing occupation-changed event.
void Simulation::process_merchant_recruitment(const std::uint64_t demographic_year) {
    std::vector<SettlementId> settlement_ids;
    for (const auto& settlement : state_.settlements) {
        if (settlement.active) {
            settlement_ids.push_back(settlement.id);
        }
    }
    std::sort(settlement_ids.begin(), settlement_ids.end());

    std::unordered_map<SettlementId, SettlementPrices> prices;
    for (const auto settlement_id : settlement_ids) {
        if (const auto settlement_prices_now = settlement_prices(settlement_id); settlement_prices_now.has_value()) {
            prices.emplace(settlement_id, *settlement_prices_now);
        }
    }

    std::unordered_map<PersonId, Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    const bool has_trading_partner = settlement_ids.size() >= 2U;
    for (const auto settlement_id : settlement_ids) {
        const auto* settlement = find_settlement(settlement_id);
        const auto own_prices_it = prices.find(settlement_id);
        if (settlement == nullptr || own_prices_it == prices.end()) {
            continue;
        }

        const std::array<bool, 5> holds{
            settlement->commons_food.value > 0,
            settlement->materials.wood.value > 0,
            settlement->materials.stone.value > 0,
            settlement->materials.timber.value > 0,
            settlement->materials.basic_tool.value > 0,
        };

        std::vector<SettlementId> neighbour_ids;
        std::optional<PriceMargin> best;
        if (has_trading_partner) {
            for (const auto other_id : settlement_ids) {
                const auto* other = find_settlement(other_id);
                const auto other_prices_it = prices.find(other_id);
                if (other_id == settlement_id || other == nullptr || other_prices_it == prices.end() ||
                    !within_days_of_walking(
                        settlement->position_x, settlement->position_z, other->position_x, other->position_z, kDaysWalkKnownNeighbours)) {
                    continue;
                }
                neighbour_ids.push_back(other_id);
                const auto margin = best_margin(own_prices_it->second, other_prices_it->second, holds);
                if (margin.has_value() && (!best.has_value() || margin->margin_permille > best->margin_permille)) {
                    best = margin;
                }
            }
        }
        const bool trade_pays = margin_clears_threshold(best);

        std::vector<Person*> merchants;
        std::vector<Person*> candidates;
        std::uint64_t eligible_adults = 0;
        for (const auto& household : state_.households) {
            if (household.settlement_id != settlement_id) {
                continue;
            }
            for (const auto member_id : household.member_ids) {
                const auto person_it = person_by_id.find(member_id);
                if (person_it == person_by_id.end() || !person_it->second->alive) {
                    continue;
                }
                auto* person = person_it->second;
                if (person->occupation == Occupation::Merchant) {
                    merchants.push_back(person);
                }
                if (occupation_eligible_for_productive_work(*person)) {
                    ++eligible_adults;
                    // Spare means unassigned: the labour allocation that just ran left
                    // this adult with no work, so training them costs no production.
                    if (person->occupation == Occupation::None) {
                        candidates.push_back(person);
                    }
                }
            }
        }
        const auto by_id = [](const Person* lhs, const Person* rhs) { return lhs->id < rhs->id; };
        std::sort(merchants.begin(), merchants.end(), by_id);
        std::sort(candidates.begin(), candidates.end(), by_id);

        // A merger can leave more merchants than the bound allows: the highest
        // ids return to ordinary work.
        while (merchants.size() > kMaxMerchantsPerSettlement) {
            merchants.back()->occupation = Occupation::None;
            merchants.pop_back();
        }

        const auto action = decide_merchant_action(
            trade_pays, merchants.size(), eligible_adults, candidates.size(), settlement->days_under_food_strain > 0U);
        if (action == MerchantAction::Retire) {
            for (auto* merchant : merchants) {
                merchant->occupation = Occupation::None;
            }
            continue;
        }
        if (action != MerchantAction::Appoint) {
            continue;
        }

        const auto selection_seed = derive_seed(settlement_seed(settlement_id), SeedDomain::MerchantSelection, demographic_year);
        auto* chosen = candidates[static_cast<std::size_t>(seed_random_bounded(selection_seed, candidates.size()))];
        chosen->occupation = Occupation::Merchant;

        // A new merchant knows its home settlement's prices and its neighbours'.
        std::vector<SettlementId> known{settlement_id};
        known.insert(known.end(), neighbour_ids.begin(), neighbour_ids.end());
        for (const auto known_id : known) {
            const auto known_prices = prices.at(known_id);
            for (std::size_t resource = 0; resource < known_prices.millicoin.size(); ++resource) {
                state_.merchant_price_memory.push_back(MerchantPriceMemory{
                    .merchant_id = chosen->id,
                    .settlement_id = known_id,
                    .resource = static_cast<ResourceType>(resource),
                    .price_millicoin = known_prices.millicoin[resource],
                    .seen_at = state_.time,
                });
            }
        }
    }

    prune_merchant_memory();
}

// A merchant who is gone, dead or no longer a merchant keeps no memory, and the
// rest stays in (merchant, settlement, resource) order.
void Simulation::prune_merchant_memory() {
    std::unordered_set<PersonId> living_merchants;
    for (const auto& person : state_.people) {
        if (person.alive && person.occupation == Occupation::Merchant) {
            living_merchants.insert(person.id);
        }
    }
    std::erase_if(state_.merchant_price_memory, [&living_merchants](const MerchantPriceMemory& memory) {
        return !living_merchants.contains(memory.merchant_id);
    });
    std::sort(
        state_.merchant_price_memory.begin(),
        state_.merchant_price_memory.end(),
        [](const MerchantPriceMemory& lhs, const MerchantPriceMemory& rhs) {
            return std::tie(lhs.merchant_id, lhs.settlement_id, lhs.resource) <
                std::tie(rhs.merchant_id, rhs.settlement_id, rhs.resource);
        });
}

void Simulation::refresh_labour_aggregates() {
    state_.eligible_workers = 0;
    state_.active_workers = 0;
    state_.unallocated_workers = 0;
    state_.cultivators = 0;
    state_.gatherers = 0;
    state_.wood_gatherers = 0;
    state_.stone_extractors = 0;
    state_.craft_workers = 0;
    state_.total_labour_capacity = 0;
    state_.total_labour_demand = 0;
    state_.total_labour_allocated = 0;
    state_.cultivation_labour_available = 0;
    state_.cultivation_labour_allocated = 0;
    state_.gathering_labour_available = 0;
    state_.gathering_labour_allocated = 0;
    state_.labour_shortfall = 0;
    state_.wood_labour_available = 0;
    state_.wood_labour_allocated = 0;
    state_.stone_labour_available = 0;
    state_.stone_labour_allocated = 0;
    state_.craft_labour_available = 0;
    state_.craft_labour_allocated = 0;
    state_.material_labour_allocated = 0;

    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
        if (!occupation_eligible_for_productive_work(person)) {
            continue;
        }

        const auto capacity = productive_effort_for_person(person);
        ++state_.eligible_workers;
        state_.total_labour_capacity += capacity;

        if (person.occupation == Occupation::SubsistenceCultivator) {
            ++state_.active_workers;
            ++state_.cultivators;
            state_.cultivation_labour_available += capacity;
        } else if (person.occupation == Occupation::WildFoodGatherer) {
            ++state_.active_workers;
            ++state_.gatherers;
            state_.gathering_labour_available += capacity;
        } else if (person.occupation == Occupation::WoodGatherer) {
            ++state_.active_workers;
            ++state_.wood_gatherers;
            state_.wood_labour_available += capacity;
        } else if (person.occupation == Occupation::StoneExtractor) {
            ++state_.active_workers;
            ++state_.stone_extractors;
            state_.stone_labour_available += capacity;
        } else if (person.occupation == Occupation::CraftWorker) {
            ++state_.active_workers;
            ++state_.craft_workers;
            state_.craft_labour_available += capacity;
        } else {
            ++state_.unallocated_workers;
        }
    }

    for (const auto& settlement : state_.settlements) {
        if (!settlement.active) {
            continue;
        }

        const auto* region = find_region(settlement.region_id);
        if (region == nullptr) {
            continue;
        }

        std::int64_t required_food = 0;
        std::int64_t stored_food = 0;
        std::int64_t cultivation_available = 0;
        std::int64_t gathering_available = 0;

        for (const auto& household : state_.households) {
            if (household.settlement_id != settlement.id) {
                continue;
            }

            stored_food += household.resources.food_inventory.value;
            for (const auto member_id : household.member_ids) {
                const auto person_it = person_by_id.find(member_id);
                if (person_it == person_by_id.end() || !person_it->second->alive) {
                    continue;
                }

                const auto* person = person_it->second;
                required_food += food_requirement_for_person(*person);
                if (!occupation_eligible_for_productive_work(*person)) {
                    continue;
                }

                const auto capacity = productive_effort_for_person(*person);
                if (person->occupation == Occupation::SubsistenceCultivator) {
                    cultivation_available += capacity;
                } else if (person->occupation == Occupation::WildFoodGatherer) {
                    gathering_available += capacity;
                }
            }
        }

        const auto cultivation_opportunity =
            (region->productive_capacity_per_day * static_cast<std::int64_t>(settlement.access_share_points)) /
            static_cast<std::int64_t>(kSettlementShareScale);
        const auto gathering_opportunity =
            (std::min(region->wild_food_stock, region->wild_food_harvest_limit_per_day) *
             static_cast<std::int64_t>(settlement.access_share_points)) /
            static_cast<std::int64_t>(kSettlementShareScale);

        const auto output_target = compute_daily_food_output_target(
            required_food,
            stored_food,
            cultivation_opportunity + gathering_opportunity);
        const auto demand = split_demand_by_opportunity(output_target, cultivation_opportunity, gathering_opportunity);

        state_.total_labour_demand += demand.total();

        const auto cultivation_allocated = std::min(cultivation_available, std::max<std::int64_t>(0, demand.cultivation_labour_demand));
        const auto gathering_allocated = std::min(gathering_available, std::max<std::int64_t>(0, demand.gathering_labour_demand));
        state_.cultivation_labour_allocated += cultivation_allocated;
        state_.gathering_labour_allocated += gathering_allocated;
    }

    // Aggregate settlements contribute their workforce once here, derived from
    // cohort composition, rather than accumulating inside the food cadence.
    for (const auto& cohort : state_.population_cohorts) {
        if (is_region_detailed(cohort.region_id) || cohort.population == 0U) {
            continue;
        }
        if (cohort.age_band != CohortAgeBand::YoungAdult && cohort.age_band != CohortAgeBand::Adult) {
            continue;
        }

        const auto eligible_workers = cohort.population;
        state_.eligible_workers += eligible_workers;
        switch (cohort.occupation_group) {
            case CohortOccupationGroup::Food:
                state_.active_workers += eligible_workers;
                state_.cultivators += (eligible_workers * kAggregateCultivatorSharePer10k) / 10'000U;
                state_.gatherers += (eligible_workers * kAggregateGathererSharePer10k) / 10'000U;
                break;
            case CohortOccupationGroup::Materials:
                state_.active_workers += eligible_workers;
                state_.wood_gatherers += (eligible_workers * kAggregateWoodSharePer10k) / 10'000U;
                state_.stone_extractors += (eligible_workers * kAggregateStoneSharePer10k) / 10'000U;
                break;
            case CohortOccupationGroup::Craft:
                state_.active_workers += eligible_workers;
                state_.craft_workers += eligible_workers;
                break;
            case CohortOccupationGroup::Unspecified:
                state_.unallocated_workers += eligible_workers;
                break;
        }
    }

    // World labour allocation totals describe detailed execution only. Aggregate
    // settlements advance on their own chunk cadence, so their labour figures
    // stay in the per-settlement records rather than being folded in here where
    // the two cadences would desync between yearly refreshes.
    state_.total_labour_allocated = state_.cultivation_labour_allocated + state_.gathering_labour_allocated;
    state_.labour_shortfall = std::max<std::int64_t>(0, state_.total_labour_demand - state_.total_labour_allocated);
    state_.material_labour_allocated =
        state_.wood_labour_allocated + state_.stone_labour_allocated + state_.craft_labour_allocated;
    state_.material_labour_shortfall =
        std::max<std::int64_t>(0, state_.material_labour_demand - state_.material_labour_allocated);
}

void Simulation::process_mortality(const std::uint64_t demographic_year) {
    std::unordered_map<PersonId, Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<HouseholdId, Household*> household_by_id;
    household_by_id.reserve(state_.households.size());
    for (auto& household : state_.households) {
        household_by_id.emplace(household.id, &household);
    }

    std::unordered_map<HouseholdId, bool> household_detailed;
    household_detailed.reserve(state_.households.size());
    for (const auto& household : state_.households) {
        household_detailed.emplace(household.id, is_region_detailed(household.region_id));
    }
    const auto person_is_detailed = [&](const Person& person) {
        const auto it = household_detailed.find(person.household_id);
        return it == household_detailed.end() || it->second;
    };

    for (auto& person : state_.people) {
        if (!person.alive) {
            continue;
        }

        // LOD gate: aggregate regions run cohort mortality, not per-person rolls.
        if (!person_is_detailed(person)) {
            continue;
        }

        ++person_demography_update_count_;

        const auto baseline = mortality_rate_per_10k(age_years(person));
        const auto food_penalty = mortality_penalty_from_person_health_per_10k(person);
        const auto mortality_rate = std::min<std::uint32_t>(10'000U, baseline + food_penalty);
        const auto mortality_seed = derive_seed(person_seed(person.id), SeedDomain::Mortality, demographic_year);
        if (config_.enable_unrelated_random_probe) {
            const auto probe_seed = derive_seed(person_seed(person.id), SeedDomain::UnrelatedProbe, demographic_year);
            (void)seed_random_u64(probe_seed);
        }

        if (seed_chance_per_10k(mortality_seed, mortality_rate)) {
            const auto household_id = person.household_id;
            person.alive = false;
            remove_from_household(household_id, person.id);
            ++state_.total_deaths;

            std::vector<PersonId> observers;
            const auto household_it = household_by_id.find(household_id);
            if (household_it != household_by_id.end()) {
                for (const auto member_id : household_it->second->member_ids) {
                    const auto person_it = person_by_id.find(member_id);
                    if (person_it != person_by_id.end() && person_it->second->alive) {
                        observers.push_back(member_id);
                    }
                }
            }

            const auto event_id = emit_death_event(person.id, household_id);
            append_direct_observation_memories(event_id, observers, state_.time);
        }
    }
}

void Simulation::process_births(const std::uint64_t demographic_year) {
    struct PendingBirth {
        PersonId mother_id{};
        std::optional<PersonId> father_id{};
        HouseholdId household_id{};
    };

    std::vector<PendingBirth> pending_births;
    pending_births.reserve(64);

    std::unordered_map<HouseholdId, Household*> household_by_id;
    household_by_id.reserve(state_.households.size());
    for (auto& household : state_.households) {
        household_by_id.emplace(household.id, &household);
    }

    std::unordered_set<PersonId> alive_person_ids;
    alive_person_ids.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        if (person.alive) {
            alive_person_ids.insert(person.id);
        }
    }

    std::unordered_map<HouseholdId, bool> household_detailed;
    household_detailed.reserve(state_.households.size());
    for (const auto& household : state_.households) {
        household_detailed.emplace(household.id, is_region_detailed(household.region_id));
    }
    const auto person_is_detailed = [&](const Person& person) {
        const auto it = household_detailed.find(person.household_id);
        return it == household_detailed.end() || it->second;
    };

    for (const auto& person : state_.people) {
        if (!person.alive || person.sex != BiologicalSex::Female) {
            continue;
        }

        // LOD gate: aggregate regions run cohort fertility, not per-person rolls.
        if (!person_is_detailed(person)) {
            continue;
        }

        ++person_demography_update_count_;

        const auto fertility_rate = effective_fertility_rate_per_10k(person);
        if (fertility_rate == 0U) {
            continue;
        }

        if (!has_any_alive_adult_male()) {
            continue;
        }

        const auto fertility_seed = derive_seed(person_seed(person.id), SeedDomain::Fertility, demographic_year);
        if (config_.enable_unrelated_random_probe) {
            const auto probe_seed = derive_seed(person_seed(person.id), SeedDomain::UnrelatedProbe, demographic_year ^ 0xABCDEFU);
            (void)seed_random_u64(probe_seed);
        }

        if (seed_chance_per_10k(fertility_seed, fertility_rate)) {
            auto father_id = sample_father_in_household(person.household_id, fertility_seed);
            if (!father_id.has_value()) {
                father_id = sample_father_global(fertility_seed);
            }

            pending_births.push_back(PendingBirth{
                .mother_id = person.id,
                .father_id = father_id,
                .household_id = person.household_id,
            });
        }
    }

    for (const auto& birth : pending_births) {
        const auto newborn_id = next_person_id();
        const auto newborn_seed = derive_seed(person_seed(newborn_id), SeedDomain::Birth, demographic_year);
        const auto sex = sample_sex(newborn_seed) ? BiologicalSex::Female : BiologicalSex::Male;

        state_.people.push_back(Person{
            .id = newborn_id,
            .birth_year = current_year_signed(),
            .sex = sex,
            .alive = true,
            .household_id = birth.household_id,
            .mother_id = birth.mother_id,
            .father_id = birth.father_id,
        });

        auto household_it = household_by_id.find(birth.household_id);
        if (household_it != household_by_id.end()) {
            household_it->second->member_ids.push_back(newborn_id);
        }
        alive_person_ids.insert(newborn_id);

        ++state_.total_births;

        std::vector<PersonId> observers;
        const auto observer_household_it = household_by_id.find(birth.household_id);
        if (observer_household_it != household_by_id.end()) {
            for (const auto member_id : observer_household_it->second->member_ids) {
                if (member_id == newborn_id) {
                    continue;
                }
                if (alive_person_ids.contains(member_id)) {
                    observers.push_back(member_id);
                }
            }
        }
        if (alive_person_ids.contains(birth.mother_id) &&
            !std::binary_search(observers.begin(), observers.end(), birth.mother_id)) {
            const auto insert_it = std::lower_bound(observers.begin(), observers.end(), birth.mother_id);
            observers.insert(insert_it, birth.mother_id);
        }
        observers.erase(std::unique(observers.begin(), observers.end()), observers.end());

        const auto event_id = emit_birth_event(newborn_id, birth.household_id, birth.mother_id, birth.father_id);
        append_direct_observation_memories(event_id, observers, state_.time);
    }
}

EventId Simulation::append_event(SimulationEvent event) {
    if (!event_payload_matches_type(event)) {
        throw std::invalid_argument("Event payload does not match event type");
    }
    event.id = next_event_id_++;
    state_.event_log.push_back(event);
    return event.id;
}

EventId Simulation::emit_death_event(const PersonId person_id, const HouseholdId household_id) {
    const auto event_id = append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::Death,
        .payload = DeathEventPayload{
            .person_id = person_id,
            .household_id = household_id,
        },
    });
    return event_id;
}

EventId Simulation::emit_birth_event(
    const PersonId child_id,
    const HouseholdId household_id,
    const PersonId mother_id,
    const std::optional<PersonId> father_id) {
    const auto event_id = append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::Birth,
        .payload = BirthEventPayload{
            .child_id = child_id,
            .mother_id = mother_id,
            .father_id = father_id,
            .household_id = household_id,
        },
    });
    return event_id;
}

EventId Simulation::emit_settlement_founded_event(
    const SettlementId settlement_id,
    const RegionId region_id,
    const std::optional<SettlementId> source_settlement_id) {
    const auto event_id = append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::SettlementFounded,
        .payload = SettlementFoundedEventPayload{
            .settlement_id = settlement_id,
            .region_id = region_id,
            .source_settlement_id = source_settlement_id,
        },
    });
    return event_id;
}

EventId Simulation::emit_settlement_extinct_event(const SettlementId settlement_id, const RegionId region_id) {
    const auto event_id = append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::SettlementExtinct,
        .payload = SettlementExtinctEventPayload{
            .settlement_id = settlement_id,
            .region_id = region_id,
        },
    });
    return event_id;
}

EventId Simulation::emit_settlement_merged_event(
    const SettlementId survivor_settlement_id,
    const SettlementId absorbed_settlement_id,
    const RegionId region_id) {
    const auto event_id = append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::SettlementMerged,
        .payload = SettlementMergedEventPayload{
            .survivor_settlement_id = survivor_settlement_id,
            .absorbed_settlement_id = absorbed_settlement_id,
            .region_id = region_id,
        },
    });
    return event_id;
}

void Simulation::record_occupation_changes(const std::unordered_map<PersonId, Occupation>& occupations_before) {
    std::vector<Person*> ordered;
    ordered.reserve(state_.people.size());
    for (auto& person : state_.people) {
        ordered.push_back(&person);
    }
    std::sort(ordered.begin(), ordered.end(), [](const Person* lhs, const Person* rhs) {
        return lhs->id < rhs->id;
    });

    for (auto* person : ordered) {
        if (!person->alive) {
            continue;
        }
        if (person->occupation_history_resync_pending) {
            person->occupation_history_resync_pending = false;
            continue;
        }
        const auto before_it = occupations_before.find(person->id);
        if (before_it != occupations_before.end() && before_it->second != person->occupation) {
            emit_occupation_changed_event(person->id, before_it->second, person->occupation);
        }
    }
}

EventId Simulation::emit_occupation_changed_event(const PersonId person_id, const Occupation from, const Occupation to) {
    return append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::OccupationChanged,
        .payload = OccupationChangedEventPayload{
            .person_id = person_id,
            .from = from,
            .to = to,
        },
    });
}

EventId Simulation::emit_household_relocated_event(
    const Household& household,
    const SettlementId from_settlement_id,
    const SettlementId to_settlement_id) {
    std::vector<PersonId> member_ids;
    member_ids.reserve(household.member_ids.size());
    for (const auto member_id : household.member_ids) {
        for (const auto& person : state_.people) {
            if (person.id == member_id && person.alive) {
                member_ids.push_back(member_id);
                break;
            }
        }
    }
    std::sort(member_ids.begin(), member_ids.end());
    return append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::HouseholdRelocated,
        .payload = HouseholdRelocatedEventPayload{
            .household_id = household.id,
            .from_settlement_id = from_settlement_id,
            .to_settlement_id = to_settlement_id,
            .member_ids = std::move(member_ids),
        },
    });
}

void Simulation::place_building(Building& building, const std::span<const Building> earlier_buildings) const {
    const auto* settlement = find_settlement(building.settlement_id);
    if (settlement == nullptr) {
        building.position_x = 0;
        building.position_z = 0;
        building.facing_degrees = 0;
        return;
    }

    // Every earlier building in the region is an obstacle, including those
    // of extinct or absorbed settlements, whose buildings keep their place.
    std::vector<Point2D> obstacles;
    obstacles.reserve(earlier_buildings.size());
    for (const auto& earlier : earlier_buildings) {
        const auto* earlier_settlement = find_settlement(earlier.settlement_id);
        if (earlier_settlement != nullptr && earlier_settlement->region_id == settlement->region_id) {
            obstacles.push_back(Point2D{earlier.position_x, earlier.position_z});
        }
    }

    const Point2D centre{settlement->position_x, settlement->position_z};
    const auto settlement_seed_value = settlement_seed(settlement->id);
    const auto building_seed = derive_seed(settlement_seed_value, SeedDomain::BuildingPlacement, building.id);
    const auto start_direction = static_cast<std::size_t>(seed_random_bounded(building_seed, kDirectionCount));

    const auto accept = [&](const PlacementChoice choice) {
        building.position_x = choice.point.x;
        building.position_z = choice.point.z;
        building.facing_degrees = choice.facing_degrees;
    };

    const bool shared_service = building.settlement_commons_owned && building.type != BuildingType::Dwelling;
    if (shared_service) {
        for (const auto radius : kCommonRingRadiiUnits) {
            for (std::size_t step = 0; step < kDirectionCount; ++step) {
                const auto direction = start_direction + (step * kCommonRingDirectionStep);
                const auto candidate = offset_along(centre, direction, radius);
                if (placement_is_free(candidate, obstacles)) {
                    accept(PlacementChoice{.point = candidate, .facing_degrees = facing_degrees_for(direction + kHalfTurn)});
                    return;
                }
            }
        }
    }

    if (building.owner_household_id.has_value()) {
        const Household* owner = nullptr;
        for (const auto& household : state_.households) {
            if (household.id == *building.owner_household_id) {
                owner = &household;
                break;
            }
        }
        if (owner != nullptr) {
            const Point2D yard{centre.x + owner->settlement_offset_x, centre.z + owner->settlement_offset_z};
            for (const auto radius : kYardRadiiUnits) {
                for (std::size_t step = 0; step < kDirectionCount / kYardDirectionStep; ++step) {
                    const auto direction = start_direction + (step * kYardDirectionStep);
                    const auto candidate = offset_along(yard, direction, radius);
                    if (placement_is_free(candidate, obstacles)) {
                        accept(PlacementChoice{.point = candidate, .facing_degrees = facing_degrees_for(direction + kHalfTurn)});
                        return;
                    }
                }
            }
        }
    }

    // Plots extend outward without bound and obstacles are finite, so the
    // first free plot always exists.
    const auto tracks = settlement_track_directions(settlement_seed_value);
    for (std::uint64_t plot_index = 0;; ++plot_index) {
        const auto choice = track_plot(centre, tracks, building_seed, plot_index);
        if (placement_is_free(choice.point, obstacles)) {
            accept(choice);
            return;
        }
    }
}

void Simulation::recompute_building_placements() {
    std::vector<Building> ordered = state_.buildings;
    std::sort(ordered.begin(), ordered.end(), [](const Building& lhs, const Building& rhs) {
        return lhs.id < rhs.id;
    });
    for (std::size_t i = 0; i < ordered.size(); ++i) {
        place_building(ordered[i], std::span<const Building>{ordered.data(), i});
    }

    std::unordered_map<BuildingId, const Building*> placed_by_id;
    placed_by_id.reserve(ordered.size());
    for (const auto& building : ordered) {
        placed_by_id.emplace(building.id, &building);
    }
    for (auto& building : state_.buildings) {
        const auto* placed = placed_by_id.at(building.id);
        building.position_x = placed->position_x;
        building.position_z = placed->position_z;
        building.facing_degrees = placed->facing_degrees;
    }
}

EventId Simulation::emit_discovery_made_event(
    const std::uint32_t discovery_key,
    const PersonId discoverer_id,
    const SettlementId settlement_id) {
    const auto event_id = append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::DiscoveryMade,
        .payload = DiscoveryMadeEventPayload{
            .discovery_key = discovery_key,
            .discoverer_id = discoverer_id,
            .settlement_id = settlement_id,
        },
    });
    return event_id;
}

EventId Simulation::emit_institution_founded_event(
    const InstitutionId institution_id,
    const InstitutionKind kind,
    const SettlementId settlement_id,
    const PersonId founder_id) {
    const auto event_id = append_event(SimulationEvent{
        .id = 0,
        .timestamp = state_.time,
        .type = EventType::InstitutionFounded,
        .payload = InstitutionFoundedEventPayload{
            .institution_id = institution_id,
            .kind = kind,
            .settlement_id = settlement_id,
            .founder_id = founder_id,
        },
    });
    return event_id;
}

MemoryId Simulation::append_memory(AgentMemory memory) {
    if (memory.owner_id == 0U || memory.source_event_id == 0U) {
        throw std::invalid_argument("Agent memory owner and source event ids must be non-zero");
    }
    if (memory.acquired_at.ticks < 0) {
        throw std::invalid_argument("Agent memory acquisition time must be non-negative");
    }
    if (memory.acquired_at > state_.time) {
        throw std::invalid_argument("Agent memory acquisition time must not exceed simulation time");
    }
    if (memory.confidence_per_10k > 10'000U) {
        throw std::invalid_argument("Agent memory confidence must be <= 10000");
    }
    if (memory.source != MemorySource::DirectObservation &&
        memory.source != MemorySource::Communication &&
        memory.source != MemorySource::Teaching &&
        memory.source != MemorySource::Record) {
        throw std::invalid_argument("Agent memory source is invalid");
    }

    memory.id = next_memory_id();
    state_.agent_memories.push_back(memory);
    derive_knowledge_from_memory(memory);
    return memory.id;
}

void Simulation::append_knowledge_transmission(KnowledgeTransmission transmission) {
    if (transmission.speaker_id == 0U ||
        transmission.recipient_id == 0U ||
        transmission.speaker_household_id == 0U ||
        transmission.recipient_household_id == 0U ||
        transmission.speaker_settlement_id == 0U ||
        transmission.recipient_settlement_id == 0U ||
        transmission.speaker_memory_id == 0U ||
        transmission.recipient_memory_id == 0U) {
        throw std::invalid_argument("Knowledge transmission references must be non-zero");
    }
    if (transmission.speaker_id == transmission.recipient_id) {
        throw std::invalid_argument("Knowledge transmission speaker and recipient must differ");
    }
    if (transmission.speaker_settlement_id != transmission.recipient_settlement_id) {
        throw std::invalid_argument("Knowledge transmission must remain within one settlement");
    }
    if (transmission.transmitted_at.ticks < 0 || transmission.transmitted_at > state_.time) {
        throw std::invalid_argument("Knowledge transmission time is invalid");
    }
    if (transmission.confidence_per_10k > 10'000U) {
        throw std::invalid_argument("Knowledge transmission confidence must be <= 10000");
    }

    const AgentMemory* speaker_memory = nullptr;
    const AgentMemory* recipient_memory = nullptr;
    for (const auto& memory : state_.agent_memories) {
        if (memory.id == transmission.speaker_memory_id) {
            speaker_memory = &memory;
        }
        if (memory.id == transmission.recipient_memory_id) {
            recipient_memory = &memory;
        }
        if (speaker_memory != nullptr && recipient_memory != nullptr) {
            break;
        }
    }

    if (speaker_memory == nullptr || recipient_memory == nullptr) {
        throw std::invalid_argument("Knowledge transmission must reference existing memories");
    }
    if (speaker_memory->owner_id != transmission.speaker_id) {
        throw std::invalid_argument("Knowledge transmission speaker mismatch");
    }
    if (recipient_memory->owner_id != transmission.recipient_id) {
        throw std::invalid_argument("Knowledge transmission recipient mismatch");
    }
    if (recipient_memory->source != MemorySource::Communication &&
        recipient_memory->source != MemorySource::Teaching) {
        throw std::invalid_argument("Knowledge transmission recipient memory must be communication-derived");
    }
    if (recipient_memory->source == MemorySource::Communication && transmission.method != CommunicationMethod::Communication) {
        throw std::invalid_argument("Knowledge transmission method/source mismatch for communication memory");
    }
    if (recipient_memory->source == MemorySource::Teaching && transmission.method != CommunicationMethod::Teaching) {
        throw std::invalid_argument("Knowledge transmission method/source mismatch for teaching memory");
    }
    if (speaker_memory->source_event_id != recipient_memory->source_event_id) {
        throw std::invalid_argument("Knowledge transmission memory event mismatch");
    }

    const Household* speaker_household = nullptr;
    const Household* recipient_household = nullptr;
    for (const auto& household : state_.households) {
        if (household.id == transmission.speaker_household_id) {
            speaker_household = &household;
        }
        if (household.id == transmission.recipient_household_id) {
            recipient_household = &household;
        }
        if (speaker_household != nullptr && recipient_household != nullptr) {
            break;
        }
    }

    if (speaker_household == nullptr || recipient_household == nullptr) {
        throw std::invalid_argument("Knowledge transmission households must exist");
    }
    if (speaker_household->settlement_id != transmission.speaker_settlement_id ||
        recipient_household->settlement_id != transmission.recipient_settlement_id) {
        throw std::invalid_argument("Knowledge transmission household/settlement mismatch");
    }

    const TransmissionSemanticKey semantic_key{
        .recipient_id = transmission.recipient_id,
        .speaker_memory_id = transmission.speaker_memory_id,
    };
    if (transmission_semantic_keys_.contains(semantic_key)) {
        throw std::invalid_argument("Duplicate knowledge transmission semantic key");
    }

    transmission.id = next_communication_id();
    state_.knowledge_transmissions.push_back(transmission);
    const auto index = state_.knowledge_transmissions.size() - 1U;
    communication_index_by_recipient_memory_[transmission.recipient_memory_id] = index;
    transmission_semantic_keys_.insert(semantic_key);
}

void Simulation::append_knowledge_record(KnowledgeRecord record) {
    if (record.settlement_id == 0U ||
        record.author_id == 0U ||
        record.author_household_id == 0U ||
        record.source_memory_id == 0U ||
        record.source_event_id == 0U) {
        throw std::invalid_argument("Knowledge record references must be non-zero");
    }
    if (record.recorded_at.ticks < 0 || record.recorded_at > state_.time) {
        throw std::invalid_argument("Knowledge record time is invalid");
    }
    if (record.confidence_per_10k > 10'000U) {
        throw std::invalid_argument("Knowledge record confidence must be <= 10000");
    }

    const Person* author = nullptr;
    for (const auto& person : state_.people) {
        if (person.id == record.author_id) {
            author = &person;
            break;
        }
    }
    if (author == nullptr || !author->alive) {
        throw std::invalid_argument("Knowledge record author must exist and be alive");
    }
    if (author->household_id != record.author_household_id) {
        throw std::invalid_argument("Knowledge record author household mismatch");
    }

    const Household* author_household = nullptr;
    for (const auto& household : state_.households) {
        if (household.id == record.author_household_id) {
            author_household = &household;
            break;
        }
    }
    if (author_household == nullptr) {
        throw std::invalid_argument("Knowledge record author household must exist");
    }
    if (author_household->settlement_id != record.settlement_id) {
        throw std::invalid_argument("Knowledge record settlement mismatch with author household");
    }

    const AgentMemory* source_memory = nullptr;
    for (const auto& memory : state_.agent_memories) {
        if (memory.id == record.source_memory_id) {
            source_memory = &memory;
            break;
        }
    }
    if (source_memory == nullptr) {
        throw std::invalid_argument("Knowledge record source memory must exist");
    }
    if (source_memory->owner_id != record.author_id) {
        throw std::invalid_argument("Knowledge record author must own source memory");
    }
    if (source_memory->source_event_id != record.source_event_id) {
        throw std::invalid_argument("Knowledge record source event mismatch");
    }
    if (source_memory->acquired_at > record.recorded_at) {
        throw std::invalid_argument("Knowledge record must not precede source memory");
    }

    const RecordSemanticKey semantic_key{
        .settlement_id = record.settlement_id,
        .source_event_id = record.source_event_id,
    };
    if (record_semantic_keys_.contains(semantic_key)) {
        throw std::invalid_argument("Duplicate knowledge record semantic key");
    }

    record.id = next_record_id();
    state_.knowledge_records.push_back(record);
    record_semantic_keys_.insert(semantic_key);
}

void Simulation::append_knowledge_record_access(KnowledgeRecordAccess access) {
    if (access.record_id == 0U ||
        access.reader_id == 0U ||
        access.reader_household_id == 0U ||
        access.reader_settlement_id == 0U ||
        access.reader_memory_id == 0U) {
        throw std::invalid_argument("Knowledge record access references must be non-zero");
    }
    if (access.read_at.ticks < 0 || access.read_at > state_.time) {
        throw std::invalid_argument("Knowledge record access time is invalid");
    }
    if (access.confidence_per_10k > 10'000U) {
        throw std::invalid_argument("Knowledge record access confidence must be <= 10000");
    }

    const KnowledgeRecord* record = nullptr;
    for (const auto& candidate : state_.knowledge_records) {
        if (candidate.id == access.record_id) {
            record = &candidate;
            break;
        }
    }
    if (record == nullptr) {
        throw std::invalid_argument("Knowledge record access must reference existing record");
    }
    if (record->settlement_id != access.reader_settlement_id) {
        throw std::invalid_argument("Knowledge record access must remain within record settlement");
    }
    if (access.read_at < record->recorded_at) {
        throw std::invalid_argument("Knowledge record access must not predate record creation");
    }

    const Person* reader = nullptr;
    for (const auto& person : state_.people) {
        if (person.id == access.reader_id) {
            reader = &person;
            break;
        }
    }
    if (reader == nullptr || !reader->alive) {
        throw std::invalid_argument("Knowledge record reader must exist and be alive");
    }
    if (reader->household_id != access.reader_household_id) {
        throw std::invalid_argument("Knowledge record access reader household mismatch");
    }

    const Household* reader_household = nullptr;
    for (const auto& household : state_.households) {
        if (household.id == access.reader_household_id) {
            reader_household = &household;
            break;
        }
    }
    if (reader_household == nullptr) {
        throw std::invalid_argument("Knowledge record access reader household must exist");
    }
    if (reader_household->settlement_id != access.reader_settlement_id) {
        throw std::invalid_argument("Knowledge record access settlement mismatch with reader household");
    }

    const AgentMemory* reader_memory = nullptr;
    for (const auto& memory : state_.agent_memories) {
        if (memory.id == access.reader_memory_id) {
            reader_memory = &memory;
            break;
        }
    }
    if (reader_memory == nullptr) {
        throw std::invalid_argument("Knowledge record access reader memory must exist");
    }
    if (reader_memory->owner_id != access.reader_id) {
        throw std::invalid_argument("Knowledge record access reader memory owner mismatch");
    }
    if (reader_memory->source != MemorySource::Record) {
        throw std::invalid_argument("Knowledge record access memory must be record-derived");
    }
    if (reader_memory->source_event_id != record->source_event_id) {
        throw std::invalid_argument("Knowledge record access event mismatch");
    }
    if (reader_memory->acquired_at != access.read_at) {
        throw std::invalid_argument("Knowledge record access time must match reader memory time");
    }
    if (reader_memory->confidence_per_10k != access.confidence_per_10k) {
        throw std::invalid_argument("Knowledge record access confidence must match reader memory");
    }

    const RecordAccessSemanticKey semantic_key{
        .record_id = access.record_id,
        .reader_id = access.reader_id,
    };
    if (record_access_semantic_keys_.contains(semantic_key)) {
        throw std::invalid_argument("Duplicate knowledge record access semantic key");
    }
    if (record_access_index_by_memory_.contains(access.reader_memory_id)) {
        throw std::invalid_argument("Knowledge record access memory id must be unique");
    }

    state_.knowledge_record_accesses.push_back(access);
    std::sort(state_.knowledge_record_accesses.begin(), state_.knowledge_record_accesses.end(), [](const KnowledgeRecordAccess& lhs, const KnowledgeRecordAccess& rhs) {
        if (lhs.record_id != rhs.record_id) {
            return lhs.record_id < rhs.record_id;
        }
        return lhs.reader_id < rhs.reader_id;
    });
    rebuild_record_indexes();
}

void Simulation::append_direct_observation_memories(
    const EventId source_event_id,
    std::span<const PersonId> observer_ids,
    const SimulationTime acquired_at) {
    for (const auto observer_id : observer_ids) {
        if (observer_id == 0U) {
            continue;
        }

        const auto memory_id = append_memory(AgentMemory{
            .id = 0,
            .owner_id = observer_id,
            .acquired_at = acquired_at,
            .source = MemorySource::DirectObservation,
            .source_event_id = source_event_id,
            .confidence_per_10k = 10'000U,
        });
        (void)memory_id;
    }
}

void Simulation::derive_knowledge_from_memory(const AgentMemory& memory) {
    if (memory.source != MemorySource::DirectObservation &&
        memory.source != MemorySource::Communication &&
        memory.source != MemorySource::Teaching &&
        memory.source != MemorySource::Record) {
        return;
    }

    if (memory.source_event_id == 0U || memory.source_event_id > state_.event_log.size()) {
        throw std::invalid_argument("Memory source event id is out of range for knowledge derivation");
    }

    const auto& source_event = state_.event_log[static_cast<std::size_t>(memory.source_event_id - 1U)];
    if (source_event.id != memory.source_event_id) {
        throw std::invalid_argument("Memory source event id does not match canonical event order");
    }

    if (source_event.type == EventType::Birth) {
        const auto& payload = std::get<BirthEventPayload>(source_event.payload);
        upsert_knowledge(
            memory.owner_id,
            memory.acquired_at,
            memory.id,
            memory.confidence_per_10k,
            KnowledgeFact{PersonLifeStatusKnowledgeFact{
                .person_id = payload.child_id,
                .status = KnownLifeStatus::Alive,
            }},
            KnowledgeFactKind::PersonLifeStatus,
            payload.child_id,
            0U);

        upsert_knowledge(
            memory.owner_id,
            memory.acquired_at,
            memory.id,
            memory.confidence_per_10k,
            KnowledgeFact{PersonHouseholdKnowledgeFact{
                .person_id = payload.child_id,
                .household_id = payload.household_id,
            }},
            KnowledgeFactKind::PersonHousehold,
            payload.child_id,
            0U);
        return;
    }

    if (source_event.type == EventType::Death) {
        const auto& payload = std::get<DeathEventPayload>(source_event.payload);
        upsert_knowledge(
            memory.owner_id,
            memory.acquired_at,
            memory.id,
            memory.confidence_per_10k,
            KnowledgeFact{PersonLifeStatusKnowledgeFact{
                .person_id = payload.person_id,
                .status = KnownLifeStatus::Dead,
            }},
            KnowledgeFactKind::PersonLifeStatus,
            payload.person_id,
            0U);
        return;
    }

    if (source_event.type == EventType::SettlementFounded) {
        const auto& payload = std::get<SettlementFoundedEventPayload>(source_event.payload);
        upsert_knowledge(
            memory.owner_id,
            memory.acquired_at,
            memory.id,
            memory.confidence_per_10k,
            KnowledgeFact{SettlementStatusKnowledgeFact{
                .settlement_id = payload.settlement_id,
                .region_id = payload.region_id,
                .active = true,
            }},
            KnowledgeFactKind::SettlementStatus,
            payload.settlement_id,
            0U);
        return;
    }

    if (source_event.type == EventType::SettlementExtinct) {
        const auto& payload = std::get<SettlementExtinctEventPayload>(source_event.payload);
        upsert_knowledge(
            memory.owner_id,
            memory.acquired_at,
            memory.id,
            memory.confidence_per_10k,
            KnowledgeFact{SettlementStatusKnowledgeFact{
                .settlement_id = payload.settlement_id,
                .region_id = payload.region_id,
                .active = false,
            }},
            KnowledgeFactKind::SettlementStatus,
            payload.settlement_id,
            0U);
        return;
    }

    if (source_event.type == EventType::DiscoveryMade) {
        const auto& payload = std::get<DiscoveryMadeEventPayload>(source_event.payload);
        upsert_knowledge(
            memory.owner_id,
            memory.acquired_at,
            memory.id,
            memory.confidence_per_10k,
            KnowledgeFact{BasicDiscoveryKnowledgeFact{
                .discovery_key = payload.discovery_key,
            }},
            KnowledgeFactKind::BasicDiscovery,
            payload.discovery_key,
            0U);
        return;
    }
}

void Simulation::upsert_knowledge(
    const PersonId owner_id,
    const SimulationTime acquired_at,
    const MemoryId source_memory_id,
    const std::uint32_t confidence_per_10k,
    KnowledgeFact fact,
    const KnowledgeFactKind fact_kind,
    const std::uint64_t primary_subject_id,
    const std::uint64_t secondary_subject_id) {
    if (owner_id == 0U || source_memory_id == 0U || primary_subject_id == 0U) {
        throw std::invalid_argument("Knowledge upsert references must be non-zero");
    }
    if (acquired_at.ticks < 0 || acquired_at > state_.time) {
        throw std::invalid_argument("Knowledge upsert acquisition time is invalid");
    }
    if (confidence_per_10k > 10'000U) {
        throw std::invalid_argument("Knowledge confidence must be <= 10000");
    }

    const KnowledgeSemanticKey semantic_key{
        .owner_id = owner_id,
        .fact_kind = fact_kind,
        .primary_subject_id = primary_subject_id,
        .secondary_subject_id = secondary_subject_id,
    };

    const auto existing_it = knowledge_index_by_semantic_key_.find(semantic_key);
    if (existing_it == knowledge_index_by_semantic_key_.end()) {
        AgentKnowledge knowledge{};
        knowledge.id = next_knowledge_id();
        knowledge.owner_id = owner_id;
        knowledge.acquired_at = acquired_at;
        knowledge.updated_at = acquired_at;
        knowledge.source = KnowledgeSource::MemoryDerived;
        knowledge.source_memory_id = source_memory_id;
        knowledge.fact = std::move(fact);
        knowledge.confidence_per_10k = confidence_per_10k;

        state_.agent_knowledge.push_back(std::move(knowledge));
        const auto index = state_.agent_knowledge.size() - 1U;
        knowledge_indices_by_owner_[owner_id].push_back(index);
        knowledge_index_by_semantic_key_.emplace(semantic_key, index);
        return;
    }

    if (existing_it->second >= state_.agent_knowledge.size()) {
        throw std::invalid_argument("Knowledge semantic index is out of range");
    }

    auto& knowledge = state_.agent_knowledge[existing_it->second];
    if (knowledge.owner_id != owner_id) {
        throw std::invalid_argument("Knowledge owner mismatch during upsert");
    }

    if (fact_kind == KnowledgeFactKind::PersonLifeStatus) {
        const auto* existing_life = std::get_if<PersonLifeStatusKnowledgeFact>(&knowledge.fact);
        const auto* incoming_life = std::get_if<PersonLifeStatusKnowledgeFact>(&fact);
        if (existing_life != nullptr && incoming_life != nullptr &&
            existing_life->status == KnownLifeStatus::Dead &&
            incoming_life->status == KnownLifeStatus::Alive) {
            return;
        }
    }

    if (acquired_at < knowledge.acquired_at) {
        knowledge.acquired_at = acquired_at;
    }

    if (acquired_at > knowledge.updated_at ||
        (acquired_at == knowledge.updated_at && source_memory_id >= knowledge.source_memory_id)) {
        knowledge.updated_at = acquired_at;
        knowledge.source = KnowledgeSource::MemoryDerived;
        knowledge.source_memory_id = source_memory_id;
        knowledge.fact = std::move(fact);
        knowledge.confidence_per_10k = confidence_per_10k;
    }
}

void Simulation::rebuild_knowledge_indexes() {
    knowledge_indices_by_owner_.clear();
    knowledge_index_by_semantic_key_.clear();
    knowledge_indices_by_owner_.reserve(state_.agent_knowledge.size());
    knowledge_index_by_semantic_key_.reserve(state_.agent_knowledge.size());

    for (std::size_t index = 0; index < state_.agent_knowledge.size(); ++index) {
        const auto& knowledge = state_.agent_knowledge[index];
        const auto descriptor = describe_knowledge_fact(knowledge.fact);

        const KnowledgeSemanticKey semantic_key{
            .owner_id = knowledge.owner_id,
            .fact_kind = descriptor.kind,
            .primary_subject_id = descriptor.primary_subject_id,
            .secondary_subject_id = descriptor.secondary_subject_id,
        };

        const auto inserted = knowledge_index_by_semantic_key_.emplace(semantic_key, index);
        if (!inserted.second) {
            throw std::invalid_argument("Duplicate knowledge semantic key found while rebuilding indexes");
        }
        knowledge_indices_by_owner_[knowledge.owner_id].push_back(index);
    }
}

void Simulation::rebuild_communication_indexes() {
    communication_index_by_recipient_memory_.clear();
    transmission_semantic_keys_.clear();
    communication_index_by_recipient_memory_.reserve(state_.knowledge_transmissions.size());
    transmission_semantic_keys_.reserve(state_.knowledge_transmissions.size());

    for (std::size_t index = 0; index < state_.knowledge_transmissions.size(); ++index) {
        const auto& transmission = state_.knowledge_transmissions[index];
        communication_index_by_recipient_memory_[transmission.recipient_memory_id] = index;
        transmission_semantic_keys_.insert(TransmissionSemanticKey{
            .recipient_id = transmission.recipient_id,
            .speaker_memory_id = transmission.speaker_memory_id,
        });
    }
}

void Simulation::rebuild_record_indexes() {
    record_semantic_keys_.clear();
    record_access_semantic_keys_.clear();
    record_access_index_by_memory_.clear();
    record_semantic_keys_.reserve(state_.knowledge_records.size());
    record_access_semantic_keys_.reserve(state_.knowledge_record_accesses.size());
    record_access_index_by_memory_.reserve(state_.knowledge_record_accesses.size());

    for (const auto& record : state_.knowledge_records) {
        record_semantic_keys_.insert(RecordSemanticKey{
            .settlement_id = record.settlement_id,
            .source_event_id = record.source_event_id,
        });
    }

    for (std::size_t index = 0; index < state_.knowledge_record_accesses.size(); ++index) {
        const auto& access = state_.knowledge_record_accesses[index];
        record_access_semantic_keys_.insert(RecordAccessSemanticKey{
            .record_id = access.record_id,
            .reader_id = access.reader_id,
        });
        record_access_index_by_memory_[access.reader_memory_id] = index;
    }
}

void Simulation::refresh_population_count() noexcept {
    std::uint64_t alive_count = 0;
    for (const auto& person : state_.people) {
        if (person.alive) {
            ++alive_count;
        }
    }

    std::uint64_t cohort_population = 0;
    for (const auto& cohort : state_.population_cohorts) {
        cohort_population += cohort.population;
    }

    state_.population = alive_count + cohort_population;
}

void Simulation::refresh_food_aggregates() noexcept {
    state_.total_food_stored = 0;
    state_.total_food_produced = 0;
    state_.total_food_consumed = 0;
    state_.total_unmet_food = 0;
    state_.households_in_shortage = 0;
    state_.population_under_food_stress = 0;

    for (const auto& household : state_.households) {
        state_.total_food_stored += household.resources.food_inventory.value;
        state_.total_food_produced += household.resources.food_produced_total;
        state_.total_food_consumed += household.resources.food_consumed_total;
        state_.total_unmet_food += household.resources.unmet_food_total;

        const bool shortage = household.resources.consecutive_shortage_days > 0U;
        if (shortage) {
            ++state_.households_in_shortage;
        }

    }

    for (const auto& settlement : state_.settlements) {
        state_.total_food_stored += std::max<std::int64_t>(0, settlement.commons_food.value);
    }

    for (const auto& person : state_.people) {
        if (!person.alive) {
            continue;
        }

        if (person.nutrition_points < 550 || person.health_points < 500) {
            ++state_.population_under_food_stress;
        }
    }

    for (const auto& aggregate : state_.settlement_aggregates) {
        state_.total_food_stored += std::max<std::int64_t>(0, aggregate.food_inventory);
        state_.total_food_produced += std::max<std::int64_t>(0, aggregate.food_produced_total);
        state_.total_food_consumed += std::max<std::int64_t>(0, aggregate.food_consumed_total);
        state_.total_unmet_food += std::max<std::int64_t>(0, aggregate.unmet_food_total);
        if (aggregate.unmet_food_total > 0) {
            ++state_.households_in_shortage;
            const auto stressed_households = std::min<std::uint64_t>(aggregate.household_count, static_cast<std::uint64_t>(aggregate.unmet_food_total / 2));
            state_.population_under_food_stress += std::min<std::uint64_t>(
                stressed_households * kAggregatePeoplePerHousehold,
                aggregate.household_count * kAggregatePeoplePerHousehold);
        }
    }
}

void Simulation::refresh_environment_aggregates() noexcept {
    state_.total_productive_capacity_per_day = 0;
    state_.total_wild_food_capacity = 0;
    state_.total_wild_food_stock = 0;
    state_.total_wild_food_harvested = 0;
    state_.total_wild_food_regenerated = 0;
    state_.total_wood_capacity = 0;
    state_.total_wood_stock = 0;
    state_.total_stone_capacity = 0;
    state_.total_stone_remaining = 0;
    state_.total_wood_harvested = 0;
    state_.total_wood_regenerated = 0;
    state_.total_stone_extracted = 0;

    for (const auto& region : state_.regions) {
        state_.total_productive_capacity_per_day += region.productive_capacity_per_day;
        state_.total_wild_food_capacity += region.wild_food_capacity;
        state_.total_wild_food_stock += region.wild_food_stock;
        state_.total_wild_food_harvested += region.cumulative_wild_food_harvested;
        state_.total_wild_food_regenerated += region.cumulative_wild_food_regenerated;
        state_.total_wood_capacity += region.wood_capacity;
        state_.total_wood_stock += region.wood_stock;
        state_.total_stone_capacity += region.stone_capacity;
        state_.total_stone_remaining += region.stone_remaining;
        state_.total_wood_harvested += region.cumulative_wood_harvested;
        state_.total_wood_regenerated += region.cumulative_wood_regenerated;
        state_.total_stone_extracted += region.cumulative_stone_extracted;
    }
}

void Simulation::refresh_material_aggregates() noexcept {
    state_.total_wood_inventory = 0;
    state_.total_stone_inventory = 0;
    state_.total_timber_inventory = 0;
    state_.total_basic_tool_inventory = 0;
    state_.total_timber_produced = 0;
    state_.total_basic_tool_produced = 0;
    state_.total_basic_tool_broken = 0;
    state_.average_tool_coverage_per_10k = 0;
    std::uint64_t active_settlement_count = 0;
    std::uint64_t coverage_sum = 0;

    for (const auto& settlement : state_.settlements) {
        state_.total_wood_inventory += settlement.materials.wood.value;
        state_.total_stone_inventory += settlement.materials.stone.value;
        state_.total_timber_inventory += settlement.materials.timber.value;
        state_.total_basic_tool_inventory += settlement.materials.basic_tool.value;
        state_.total_timber_produced += settlement.cumulative_timber_produced;
        state_.total_basic_tool_produced += settlement.cumulative_basic_tool_produced;
        state_.total_basic_tool_broken += settlement.cumulative_basic_tool_broken;
        if (settlement.active) {
            ++active_settlement_count;
            coverage_sum += settlement.last_tool_coverage_per_10k;
        }
    }

    if (active_settlement_count > 0U) {
        state_.average_tool_coverage_per_10k = bounded_u32(coverage_sum / active_settlement_count);
    }
}

void Simulation::process_household_access_allocation() {
    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<SettlementId, std::vector<Household*>> households_by_settlement;
    households_by_settlement.reserve(state_.settlements.size());
    for (auto& household : state_.households) {
        household.productive_access_share_points = 0U;
        household.commons_access_share_points = 0U;

        bool has_alive_member = false;
        for (const auto member_id : household.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it != person_by_id.end() && person_it->second->alive) {
                has_alive_member = true;
                break;
            }
        }

        if (has_alive_member) {
            households_by_settlement[household.settlement_id].push_back(&household);
        }
    }

    for (auto& [settlement_id, households] : households_by_settlement) {
        std::sort(households.begin(), households.end(), [](const Household* lhs, const Household* rhs) {
            return lhs->id < rhs->id;
        });

        std::vector<std::int64_t> capacities(households.size(), 0);
        std::int64_t total_capacity = 0;
        for (std::size_t i = 0; i < households.size(); ++i) {
            auto* household = households[i];
            std::int64_t capacity = 0;
            for (const auto member_id : household->member_ids) {
                const auto person_it = person_by_id.find(member_id);
                if (person_it == person_by_id.end()) {
                    continue;
                }

                const auto* person = person_it->second;
                if (!occupation_eligible_for_productive_work(*person)) {
                    continue;
                }
                capacity += productive_effort_for_person(*person);
            }
            capacities[i] = std::max<std::int64_t>(0, capacity);
            total_capacity += capacities[i];
        }

        if (households.empty()) {
            continue;
        }

        if (total_capacity <= 0) {
            const auto base = kSettlementShareScale / static_cast<std::uint32_t>(households.size());
            auto remainder = static_cast<std::uint32_t>(kSettlementShareScale - (base * static_cast<std::uint32_t>(households.size())));
            for (auto* household : households) {
                household->productive_access_share_points = base;
                if (remainder > 0U) {
                    ++household->productive_access_share_points;
                    --remainder;
                }
                household->commons_access_share_points = household->productive_access_share_points;
            }
            continue;
        }

        std::uint32_t allocated = 0;
        for (std::size_t i = 0; i < households.size(); ++i) {
            auto* household = households[i];
            const auto share = static_cast<std::uint32_t>(
                std::min<std::int64_t>(
                    kSettlementShareScale,
                    (capacities[i] * static_cast<std::int64_t>(kSettlementShareScale)) / total_capacity));
            household->productive_access_share_points = share;
            household->commons_access_share_points = share;
            allocated = std::min<std::uint32_t>(kSettlementShareScale, allocated + share);
        }

        auto remainder = static_cast<std::int64_t>(kSettlementShareScale) - static_cast<std::int64_t>(allocated);
        for (auto* household : households) {
            if (remainder <= 0) {
                break;
            }
            ++household->productive_access_share_points;
            ++household->commons_access_share_points;
            --remainder;
        }
        (void)settlement_id;
    }
}

void Simulation::process_household_stabilization_transfers() {
    std::unordered_map<SettlementId, Settlement*> settlement_by_id;
    settlement_by_id.reserve(state_.settlements.size());
    for (auto& settlement : state_.settlements) {
        settlement_by_id.emplace(settlement.id, &settlement);
    }

    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    for (auto& household : state_.households) {
        auto settlement_it = settlement_by_id.find(household.settlement_id);
        if (settlement_it == settlement_by_id.end() || !settlement_it->second->active) {
            continue;
        }

        std::int64_t required_food = 0;
        for (const auto member_id : household.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it == person_by_id.end() || !person_it->second->alive) {
                continue;
            }
            required_food += food_requirement_for_person(*person_it->second);
        }

        const auto reserve = required_food * kFoodCommonsTransferReserveDays;
        auto surplus = household.resources.food_inventory.value - reserve;
        if (surplus <= 0) {
            continue;
        }

        auto* settlement = settlement_it->second;
        household.resources.food_inventory.value -= surplus;
        settlement->commons_food.value += surplus;
        household.cumulative_food_transferred_out += surplus;
        settlement->last_transfer_food_volume += surplus;
    }

    for (auto& household : state_.households) {
        auto settlement_it = settlement_by_id.find(household.settlement_id);
        if (settlement_it == settlement_by_id.end() || !settlement_it->second->active) {
            continue;
        }

        std::int64_t required_food = 0;
        for (const auto member_id : household.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it == person_by_id.end() || !person_it->second->alive) {
                continue;
            }
            required_food += food_requirement_for_person(*person_it->second);
        }
        if (required_food <= 0) {
            continue;
        }

        const auto reserve = required_food * kFoodCommonsTransferReserveDays;
        if (household.resources.food_inventory.value >= reserve) {
            continue;
        }

        auto* settlement = settlement_it->second;
        const auto need = reserve - household.resources.food_inventory.value;
        const auto transfer = std::min<std::int64_t>(need, std::max<std::int64_t>(0, settlement->commons_food.value));
        if (transfer <= 0) {
            continue;
        }

        settlement->commons_food.value -= transfer;
        household.resources.food_inventory.value += transfer;
        household.cumulative_food_transferred_in += transfer;
        settlement->last_transfer_food_volume += transfer;
    }
}

void Simulation::process_local_exchange(const std::uint64_t demographic_year) {
    (void)demographic_year;

    std::unordered_map<SettlementId, std::vector<Household*>> households_by_settlement;
    households_by_settlement.reserve(state_.settlements.size());
    for (auto& household : state_.households) {
        households_by_settlement[household.settlement_id].push_back(&household);
    }

    for (auto& settlement : state_.settlements) {
        auto it = households_by_settlement.find(settlement.id);
        if (it == households_by_settlement.end()) {
            continue;
        }

        auto& households = it->second;
        std::sort(households.begin(), households.end(), [](const Household* lhs, const Household* rhs) {
            return lhs->id < rhs->id;
        });

        if (households.empty()) {
            continue;
        }

        std::vector<std::uint32_t> shares;
        shares.reserve(households.size());
        std::uint32_t total_share = 0;
        for (const auto* household : households) {
            shares.push_back(household->productive_access_share_points);
            total_share += household->productive_access_share_points;
        }

        if (total_share == 0U) {
            shares.assign(households.size(), 1U);
            total_share = static_cast<std::uint32_t>(households.size());
        }

        auto allocate_and_apply = [&](const std::int64_t total_units, auto&& getter) {
            const auto capped_total = std::max<std::int64_t>(0, total_units);
            std::vector<std::int64_t> allocations(households.size(), 0);

            std::int64_t allocated = 0;
            for (std::size_t i = 0; i < households.size(); ++i) {
                const auto share_i64 = static_cast<std::int64_t>(shares[i]);
                const auto divisor = static_cast<std::int64_t>(total_share);
                const auto quotient = capped_total / divisor;
                const auto remainder = capped_total % divisor;

                if (quotient > 0 && share_i64 > 0 && quotient > (std::numeric_limits<std::int64_t>::max() / share_i64)) {
                    throw std::overflow_error("Exchange ownership allocation overflow");
                }

                const auto base = quotient * share_i64;
                const auto rem_component = (remainder * share_i64) / divisor;
                if (base > (std::numeric_limits<std::int64_t>::max() - rem_component)) {
                    throw std::overflow_error("Exchange ownership allocation overflow");
                }

                const auto value = base + rem_component;
                allocations[i] = std::max<std::int64_t>(0, value);
                if (!checked_add_i64(allocated, allocations[i], allocated)) {
                    throw std::overflow_error("Exchange allocation accumulation overflow");
                }
            }

            auto remainder = capped_total - allocated;
            for (std::size_t i = 0; i < households.size() && remainder > 0; ++i) {
                ++allocations[i];
                --remainder;
            }

            std::int64_t movement = 0;
            for (std::size_t i = 0; i < households.size(); ++i) {
                auto* household = households[i];
                const auto old_value = getter(*household);
                const auto new_value = allocations[i];

                if (new_value > old_value) {
                    household->cumulative_material_transferred_in += (new_value - old_value);
                    movement += (new_value - old_value);
                } else if (old_value > new_value) {
                    household->cumulative_material_transferred_out += (old_value - new_value);
                    movement += (old_value - new_value);
                }

                getter(*household) = new_value;
            }

            settlement.last_exchange_volume += movement / 2;
        };

        allocate_and_apply(settlement.materials.wood.value, [](Household& household) -> std::int64_t& {
            return household.owned_materials.wood.value;
        });
        allocate_and_apply(settlement.materials.stone.value, [](Household& household) -> std::int64_t& {
            return household.owned_materials.stone.value;
        });
        allocate_and_apply(settlement.materials.timber.value, [](Household& household) -> std::int64_t& {
            return household.owned_materials.timber.value;
        });
        allocate_and_apply(settlement.materials.basic_tool.value, [](Household& household) -> std::int64_t& {
            return household.owned_materials.basic_tool.value;
        });
    }
}

// Builders source their own shortfall before building: they hew timber from
// the settlement's wood stock and quarry stone from the region, bounded by the
// settlement's regional access share. Returns the labour spent.
std::int64_t Simulation::source_construction_materials(
    Settlement& settlement,
    const std::int64_t timber_shortfall,
    const std::int64_t stone_shortfall,
    const std::int64_t labour_budget) {
    auto labour_remaining = std::max<std::int64_t>(0, labour_budget);
    auto* region = find_region_mutable(settlement.region_id);

    auto regional_share = [&](const std::int64_t regional_stock) {
        return (std::max<std::int64_t>(0, regional_stock) * static_cast<std::int64_t>(settlement.access_share_points)) /
            kSettlementMaterialShareScale;
    };

    const auto wood_needed_for_timber =
        timber_shortfall * std::max<std::int64_t>(1, material_parameters_.wood_per_timber) -
        std::max<std::int64_t>(0, settlement.materials.wood.value);
    if (region != nullptr && wood_needed_for_timber > 0) {
        const auto wood_felled = safe_min3_i64(wood_needed_for_timber, regional_share(region->wood_stock), labour_remaining);
        if (wood_felled > 0) {
            if (!checked_sub_nonnegative_i64(region->wood_stock, wood_felled, region->wood_stock)) {
                throw std::overflow_error("Construction felling exceeded region wood");
            }
            region->cumulative_wood_harvested += wood_felled;
            settlement.cumulative_wood_extracted += wood_felled;
            settlement.materials.wood.value += wood_felled;
            labour_remaining -= wood_felled;
        }
    }

    const auto labour_per_timber = std::max<std::int64_t>(1, material_parameters_.craft_labour_per_timber);
    const auto wood_per_timber = std::max<std::int64_t>(1, material_parameters_.wood_per_timber);
    const auto timber_made = safe_min3_i64(
        timber_shortfall,
        std::max<std::int64_t>(0, settlement.materials.wood.value) / wood_per_timber,
        labour_remaining / labour_per_timber);
    if (timber_made > 0) {
        settlement.materials.wood.value -= timber_made * wood_per_timber;
        settlement.materials.timber.value += timber_made;
        settlement.cumulative_timber_produced += timber_made;
        labour_remaining -= timber_made * labour_per_timber;
    }

    if (region != nullptr && stone_shortfall > 0) {
        const auto stone_quarried = safe_min3_i64(stone_shortfall, regional_share(region->stone_remaining), labour_remaining);
        if (stone_quarried > 0) {
            if (!checked_sub_nonnegative_i64(region->stone_remaining, stone_quarried, region->stone_remaining)) {
                throw std::overflow_error("Construction quarrying exceeded region stone");
            }
            region->cumulative_stone_extracted += stone_quarried;
            settlement.cumulative_stone_extracted += stone_quarried;
            settlement.materials.stone.value += stone_quarried;
            labour_remaining -= stone_quarried;
        }
    }

    return std::max<std::int64_t>(0, labour_budget) - labour_remaining;
}

void Simulation::process_construction(const std::uint64_t demographic_year) {
    (void)demographic_year;

    std::unordered_map<SettlementId, std::uint64_t> alive_population_by_settlement;
    std::unordered_map<SettlementId, std::uint64_t> eligible_workers_by_settlement;
    alive_population_by_settlement.reserve(state_.settlements.size());
    eligible_workers_by_settlement.reserve(state_.settlements.size());

    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state_.people.size());
    for (const auto& person : state_.people) {
        person_by_id.emplace(person.id, &person);
    }

    for (const auto& household : state_.households) {
        for (const auto member_id : household.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it == person_by_id.end() || !person_it->second->alive) {
                continue;
            }

            ++alive_population_by_settlement[household.settlement_id];
            if (occupation_eligible_for_productive_work(*person_it->second)) {
                ++eligible_workers_by_settlement[household.settlement_id];
            }
        }
    }

    auto find_building_mutable = [&](const BuildingId building_id) -> Building* {
        for (auto& building : state_.buildings) {
            if (building.id == building_id) {
                return &building;
            }
        }
        return nullptr;
    };

    std::unordered_map<SettlementId, std::vector<BuildingId>> buildings_by_settlement;
    buildings_by_settlement.reserve(state_.settlements.size());
    for (const auto& building : state_.buildings) {
        buildings_by_settlement[building.settlement_id].push_back(building.id);
    }

    for (auto& settlement : state_.settlements) {
        if (!settlement.active) {
            continue;
        }

        auto& settlement_buildings = buildings_by_settlement[settlement.id];
        std::sort(settlement_buildings.begin(), settlement_buildings.end());

        std::uint64_t complete_dwellings = 0;
        std::uint64_t complete_food_stores = 0;
        std::uint64_t complete_workshops = 0;
        std::uint64_t active_dwellings = 0;
        std::uint64_t active_food_stores = 0;
        std::uint64_t active_workshops = 0;

        for (const auto building_id : settlement_buildings) {
            const auto* building = find_building_mutable(building_id);
            if (building == nullptr) {
                continue;
            }
            const bool is_complete = building->state == ConstructionState::Complete;
            if (building->type == BuildingType::Dwelling) {
                is_complete ? ++complete_dwellings : ++active_dwellings;
            } else if (building->type == BuildingType::FoodStore) {
                is_complete ? ++complete_food_stores : ++active_food_stores;
            } else if (building->type == BuildingType::Workshop) {
                is_complete ? ++complete_workshops : ++active_workshops;
            }
        }

        const auto alive_population = alive_population_by_settlement.contains(settlement.id)
            ? alive_population_by_settlement[settlement.id]
            : 0U;
        const auto eligible_workers = eligible_workers_by_settlement.contains(settlement.id)
            ? eligible_workers_by_settlement[settlement.id]
            : 0U;

        std::int64_t required_food = 0;
        std::int64_t stored_food = std::max<std::int64_t>(0, settlement.commons_food.value);
        std::int64_t unallocated_labour_capacity = 0;
        std::int64_t allocated_labour_capacity = 0;
        for (const auto& household : state_.households) {
            if (household.settlement_id != settlement.id) {
                continue;
            }

            if (!checked_add_i64(stored_food, household.resources.food_inventory.value, stored_food)) {
                throw std::overflow_error("Construction stored food aggregation overflow");
            }

            for (const auto member_id : household.member_ids) {
                const auto person_it = person_by_id.find(member_id);
                if (person_it == person_by_id.end() || !person_it->second->alive) {
                    continue;
                }

                const auto& person = *person_it->second;
                if (!checked_add_i64(required_food, food_requirement_for_person(person), required_food)) {
                    throw std::overflow_error("Construction required food aggregation overflow");
                }
                if (!occupation_eligible_for_productive_work(person)) {
                    continue;
                }

                // Unassigned adults are the labour construction draws on. A merchant is
                // one of them until trips exist: it does no assigned work, so it counts
                // as unallocated rather than as allocated labour. (A merchant away on a
                // trip will be left out when trips are built.)
                const bool is_unassigned = person.occupation == Occupation::None || person.occupation == Occupation::Merchant;
                auto& labour_pool = is_unassigned ? unallocated_labour_capacity : allocated_labour_capacity;
                if (!checked_add_i64(labour_pool, productive_effort_for_person(person), labour_pool)) {
                    throw std::overflow_error("Construction labour capacity overflow");
                }
            }
        }

        const auto pressure = settlement_pressure_per_10k(settlement);
        const auto food_buffer_days = required_food > 0
            ? stored_food / std::max<std::int64_t>(1, required_food)
            : 0;
        const bool subsistence_constrained =
            required_food > 0 &&
            (pressure >= kHighPressureThresholdPer10k || food_buffer_days < kFoodCrisisBufferDays);

        const auto target_dwellings = std::max<std::uint64_t>(1U, ceil_div_u64(alive_population, 30U));
        const auto target_food_stores = std::max<std::uint64_t>(1U, ceil_div_u64(alive_population, 60U));
        const auto target_workshops = std::max<std::uint64_t>(1U, ceil_div_u64(eligible_workers, 45U));

        auto queue_project = [&](const BuildingType type, const std::int64_t timber_required, const std::int64_t stone_required, const std::int64_t labour_required) {
            Building project{};
            project.id = next_building_id();
            project.type = type;
            project.settlement_id = settlement.id;
            project.settlement_commons_owned = true;
            project.state = ConstructionState::Planned;
            project.timber_required = timber_required;
            project.stone_required = stone_required;
            project.labour_required = labour_required;
            project.started_at = state_.time;
            place_building(project, state_.buildings);
            state_.buildings.push_back(project);
            settlement_buildings.push_back(project.id);
        };

        if (complete_dwellings + active_dwellings < target_dwellings) {
            queue_project(BuildingType::Dwelling, kDwellingTimberRequired, kDwellingStoneRequired, kDwellingLabourRequired);
        }
        if (complete_food_stores + active_food_stores < target_food_stores) {
            queue_project(BuildingType::FoodStore, kFoodStoreTimberRequired, kFoodStoreStoneRequired, kFoodStoreLabourRequired);
        }
        if (complete_workshops + active_workshops < target_workshops) {
            queue_project(BuildingType::Workshop, kWorkshopTimberRequired, kWorkshopStoneRequired, kWorkshopLabourRequired);
        }

        std::sort(settlement_buildings.begin(), settlement_buildings.end());

        std::int64_t labour_budget = 0;
        if (!subsistence_constrained) {
            const auto seasonal_share =
                (std::max<std::int64_t>(0, allocated_labour_capacity) * kConstructionSeasonalLabourSharePer10k) / 10'000;
            std::int64_t daily_budget = 0;
            if (!checked_add_i64(std::max<std::int64_t>(0, unallocated_labour_capacity), seasonal_share, daily_budget) ||
                !checked_mul_i64(daily_budget, DemographicCalendarAdapter::kDaysPerYear, labour_budget)) {
                throw std::overflow_error("Construction annual labour budget overflow");
            }

            const auto demand = construction_demand_for_settlement(settlement.id, state_.buildings);
            const auto timber_shortfall = std::max<std::int64_t>(0, demand.timber_remaining - settlement.materials.timber.value);
            const auto stone_shortfall = std::max<std::int64_t>(0, demand.stone_remaining - settlement.materials.stone.value);
            labour_budget -= source_construction_materials(settlement, timber_shortfall, stone_shortfall, labour_budget);
        }
        for (const auto building_id : settlement_buildings) {
            auto* building = find_building_mutable(building_id);
            if (building == nullptr) {
                continue;
            }
            if (building->state == ConstructionState::Complete) {
                continue;
            }

            if (building->state == ConstructionState::Planned) {
                building->state = ConstructionState::ActiveConstruction;
            }

            const auto timber_need = std::max<std::int64_t>(0, building->timber_required - building->timber_committed);
            const auto timber_commit = std::min<std::int64_t>(timber_need, std::max<std::int64_t>(0, settlement.materials.timber.value));
            settlement.materials.timber.value -= timber_commit;
            building->timber_committed += timber_commit;

            const auto stone_need = std::max<std::int64_t>(0, building->stone_required - building->stone_committed);
            const auto stone_commit = std::min<std::int64_t>(stone_need, std::max<std::int64_t>(0, settlement.materials.stone.value));
            settlement.materials.stone.value -= stone_commit;
            building->stone_committed += stone_commit;

            const auto labour_need = std::max<std::int64_t>(0, building->labour_required - building->labour_completed);
            const auto labour_commit = std::min<std::int64_t>(labour_need, std::max<std::int64_t>(0, labour_budget));
            labour_budget -= labour_commit;
            building->labour_completed += labour_commit;

            if (building->timber_committed >= building->timber_required &&
                building->stone_committed >= building->stone_required &&
                building->labour_completed >= building->labour_required) {
                building->state = ConstructionState::Complete;
                building->completed_at = state_.time;
            }
        }
    }

    std::unordered_map<SettlementId, std::uint64_t> completed_dwellings_by_settlement;
    std::unordered_map<SettlementId, std::uint64_t> completed_food_stores_by_settlement;
    std::unordered_map<SettlementId, std::uint64_t> completed_workshops_by_settlement;
    completed_dwellings_by_settlement.reserve(state_.settlements.size());
    completed_food_stores_by_settlement.reserve(state_.settlements.size());
    completed_workshops_by_settlement.reserve(state_.settlements.size());

    for (const auto& building : state_.buildings) {
        if (building.state != ConstructionState::Complete) {
            continue;
        }

        if (building.type == BuildingType::Dwelling) {
            ++completed_dwellings_by_settlement[building.settlement_id];
        } else if (building.type == BuildingType::FoodStore) {
            ++completed_food_stores_by_settlement[building.settlement_id];
        } else if (building.type == BuildingType::Workshop) {
            ++completed_workshops_by_settlement[building.settlement_id];
        }
    }

    for (auto& settlement : state_.settlements) {
        const auto completed_dwellings = completed_dwellings_by_settlement.contains(settlement.id)
            ? completed_dwellings_by_settlement[settlement.id]
            : 0U;
        const auto completed_food_stores = completed_food_stores_by_settlement.contains(settlement.id)
            ? completed_food_stores_by_settlement[settlement.id]
            : 0U;
        const auto completed_workshops = completed_workshops_by_settlement.contains(settlement.id)
            ? completed_workshops_by_settlement[settlement.id]
            : 0U;

        settlement.dwelling_capacity_bonus = static_cast<std::int64_t>(completed_dwellings) * kDwellingCapacityBonusPerBuilding;
        settlement.food_store_capacity_bonus = static_cast<std::int64_t>(completed_food_stores) * kFoodStoreCapacityBonusPerBuilding;
        settlement.workshop_capacity_bonus = static_cast<std::int64_t>(completed_workshops) * kWorkshopCapacityBonusPerBuilding;
    }
}

// Runs once per simulated day, after settlement populations are refreshed.
// It must not run from refresh_economic_aggregates itself: that is also called
// by demotion, promotion and population injection, which would add days that
// never elapsed.
void Simulation::advance_settlement_daily_counters() {
    struct HouseholdShortage {
        std::uint64_t households{0};
        std::uint64_t short_households{0};
    };
    std::unordered_map<SettlementId, HouseholdShortage> shortage_by_settlement;
    shortage_by_settlement.reserve(state_.settlements.size());
    for (const auto& household : state_.households) {
        if (household.member_ids.empty()) {
            continue;
        }
        auto& shortage = shortage_by_settlement[household.settlement_id];
        ++shortage.households;
        if (household.resources.consecutive_shortage_days > 0U) {
            ++shortage.short_households;
        }
    }

    const auto saturating_increment = [](std::uint32_t& counter) noexcept {
        if (counter < std::numeric_limits<std::uint32_t>::max()) {
            ++counter;
        }
    };

    for (auto& settlement : state_.settlements) {
        if (!settlement.active || settlement.population >= kViableSettlementSize) {
            settlement.days_below_viable_size = 0U;
        } else {
            saturating_increment(settlement.days_below_viable_size);
        }

        const auto shortage_it = shortage_by_settlement.find(settlement.id);
        const bool is_strained = settlement.active && shortage_it != shortage_by_settlement.end() &&
            shortage_it->second.short_households * kFoodStrainShortageDenominator >= shortage_it->second.households;
        if (is_strained) {
            saturating_increment(settlement.days_under_food_strain);
        } else {
            settlement.days_under_food_strain = 0U;
        }
    }
}

void Simulation::refresh_economic_aggregates() noexcept {
    const auto saturating_increment_u32 = [](const std::uint32_t value) noexcept {
        return value < std::numeric_limits<std::uint32_t>::max() ? static_cast<std::uint32_t>(value + 1U)
                                                                 : std::numeric_limits<std::uint32_t>::max();
    };
    const auto add_u64_checked = [](const std::uint64_t left, const std::uint64_t right, std::uint64_t& out) noexcept {
        if (left > std::numeric_limits<std::uint64_t>::max() - right) {
            out = std::numeric_limits<std::uint64_t>::max();
            return false;
        }
        out = left + right;
        return true;
    };
    const auto mul_u64_checked = [](const std::uint64_t left, const std::uint64_t right, std::uint64_t& out) noexcept {
        if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left) {
            out = std::numeric_limits<std::uint64_t>::max();
            return false;
        }
        out = left * right;
        return true;
    };

    state_.total_household_wealth = 0;
    state_.total_settlement_commons_food = 0;
    state_.active_buildings = 0;
    state_.active_projects = 0;
    state_.total_exchange_volume = 0;
    state_.total_transfer_volume = 0;

    std::unordered_map<SettlementId, std::vector<std::int64_t>> wealth_by_settlement;
    wealth_by_settlement.reserve(state_.settlements.size());

    std::unordered_map<SettlementId, std::uint64_t> population_by_settlement;
    population_by_settlement.reserve(state_.settlements.size());

    constexpr std::uint64_t kTownPopulationThreshold = 1'000U;
    constexpr std::uint64_t kCityPopulationThreshold = 10'000U;
    constexpr std::uint32_t kTierHysteresisDays = 365U;

    const auto tier_rank = [](const SettlementTier tier) noexcept { return static_cast<std::uint8_t>(tier); };
    const auto tier_from_population = [](const std::uint64_t population) noexcept {
        if (population >= kCityPopulationThreshold) {
            return SettlementTier::City;
        }
        if (population >= kTownPopulationThreshold) {
            return SettlementTier::Town;
        }
        return SettlementTier::Village;
    };
    const auto refresh_tier = [&](Settlement& settlement) noexcept {
        const auto population_tier = tier_from_population(settlement.population);
        auto target_tier = settlement.tier;
        if (tier_rank(population_tier) > tier_rank(settlement.tier)) {
            target_tier = settlement.economic_viability_streak > 0U ? population_tier : settlement.tier;
        } else if (tier_rank(population_tier) < tier_rank(settlement.tier)) {
            target_tier = population_tier;
        }
        if (settlement.economic_viability_streak == 0U && target_tier == settlement.tier &&
            settlement.tier != SettlementTier::Village) {
            target_tier = static_cast<SettlementTier>(tier_rank(settlement.tier) - 1U);
        }

        if (target_tier == settlement.tier) {
            settlement.pending_tier = settlement.tier;
            settlement.tier_transition_streak = 0U;
            return;
        }

        if (target_tier == settlement.pending_tier) {
            settlement.tier_transition_streak = saturating_increment_u32(settlement.tier_transition_streak);
        } else {
            settlement.pending_tier = target_tier;
            settlement.tier_transition_streak = 1U;
        }

        if (settlement.tier_transition_streak >= kTierHysteresisDays) {
            settlement.tier = target_tier;
            settlement.pending_tier = target_tier;
            settlement.tier_transition_streak = 0U;
        }
    };

    for (auto& household : state_.households) {
        const auto wealth_food = std::max<std::int64_t>(0, household.resources.food_inventory.value);
        const auto wealth_wood = std::max<std::int64_t>(0, household.owned_materials.wood.value) * kWealthValueWood;
        const auto wealth_stone = std::max<std::int64_t>(0, household.owned_materials.stone.value) * kWealthValueStone;
        const auto wealth_timber = std::max<std::int64_t>(0, household.owned_materials.timber.value) * kWealthValueTimber;
        const auto wealth_tools = std::max<std::int64_t>(0, household.owned_materials.basic_tool.value) * kWealthValueBasicTool;
        household.derived_wealth_points = wealth_food + wealth_wood + wealth_stone + wealth_timber + wealth_tools;
        state_.total_household_wealth += household.derived_wealth_points;
        wealth_by_settlement[household.settlement_id].push_back(household.derived_wealth_points);
        population_by_settlement[household.settlement_id] += static_cast<std::uint64_t>(household.member_ids.size());
    }

    for (const auto& cohort : state_.population_cohorts) {
        population_by_settlement[cohort.settlement_id] += cohort.population;
    }

    for (auto& settlement : state_.settlements) {
        const auto population_it = population_by_settlement.find(settlement.id);
        settlement.population = population_it != population_by_settlement.end() ? population_it->second : 0U;
        state_.total_settlement_commons_food += std::max<std::int64_t>(0, settlement.commons_food.value);
        state_.total_exchange_volume += std::max<std::int64_t>(0, settlement.last_exchange_volume);
        state_.total_transfer_volume += std::max<std::int64_t>(0, settlement.last_transfer_food_volume);

        const auto pressure = settlement_pressure_per_10k(settlement);
        if (pressure >= kHighPressureThresholdPer10k) {
            settlement.economic_pressure_streak = saturating_increment_u32(settlement.economic_pressure_streak);
        } else {
            settlement.economic_pressure_streak = 0U;
        }

        const auto wealth_it = wealth_by_settlement.find(settlement.id);
        if (wealth_it == wealth_by_settlement.end() || wealth_it->second.empty()) {
            settlement.last_median_household_wealth = 0;
            settlement.last_min_household_wealth = 0;
            settlement.last_max_household_wealth = 0;
            settlement.last_inequality_numerator = 0;
            settlement.last_inequality_denominator = 0;
            settlement.economic_viability_streak = 0U;
            refresh_tier(settlement);
            continue;
        }

        auto wealths = wealth_it->second;
        std::sort(wealths.begin(), wealths.end());
        settlement.last_min_household_wealth = wealths.front();
        settlement.last_max_household_wealth = wealths.back();
        settlement.last_median_household_wealth = wealths[wealths.size() / 2U];

        std::uint64_t total_wealth = 0;
        for (const auto wealth : wealths) {
            total_wealth += static_cast<std::uint64_t>(std::max<std::int64_t>(0, wealth));
        }

        if (total_wealth == 0U) {
            settlement.last_inequality_numerator = 0;
            settlement.last_inequality_denominator = 0;
        } else {
            std::uint64_t pairwise_gap = 0;
            std::uint64_t prefix_sum = 0;
            for (std::size_t i = 0; i < wealths.size(); ++i) {
                const auto wealth_u64 = static_cast<std::uint64_t>(std::max<std::int64_t>(0, wealths[i]));
                std::uint64_t weighted = 0;
                mul_u64_checked(wealth_u64, static_cast<std::uint64_t>(i), weighted);

                if (weighted >= prefix_sum) {
                    std::uint64_t updated_pairwise_gap = 0;
                    add_u64_checked(pairwise_gap, weighted - prefix_sum, updated_pairwise_gap);
                    pairwise_gap = updated_pairwise_gap;
                }

                std::uint64_t updated_prefix_sum = 0;
                add_u64_checked(prefix_sum, wealth_u64, updated_prefix_sum);
                prefix_sum = updated_prefix_sum;
            }

            std::uint64_t numerator = 0;
            mul_u64_checked(pairwise_gap, 2U, numerator);

            std::uint64_t denominator = 0;
            mul_u64_checked(static_cast<std::uint64_t>(wealths.size()), total_wealth, denominator);
            settlement.last_inequality_numerator = std::min(numerator, denominator);
            settlement.last_inequality_denominator = denominator;
        }

        if (pressure <= 5'500U && settlement.last_median_household_wealth > 0) {
            settlement.economic_viability_streak = saturating_increment_u32(settlement.economic_viability_streak);
        } else {
            settlement.economic_viability_streak = 0U;
        }

        refresh_tier(settlement);
    }

    for (const auto& building : state_.buildings) {
        if (building.state == ConstructionState::Complete) {
            ++state_.active_buildings;
        } else {
            ++state_.active_projects;
        }
    }
}

void Simulation::step_year() {
    advance(DemographicCalendarAdapter::year_duration());
}

void Simulation::run_years(const std::uint64_t years) {
    const auto years_i64_limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (years > years_i64_limit) {
        throw std::overflow_error("Requested simulation years exceed supported range");
    }

    const auto yearly = DemographicCalendarAdapter::year_duration().ticks;
    const auto years_i64 = static_cast<std::int64_t>(years);
    if (years_i64 > 0 && yearly > (std::numeric_limits<std::int64_t>::max() / years_i64)) {
        throw std::overflow_error("Requested simulation duration overflow");
    }

    advance(SimulationDuration{years_i64 * yearly});
}

}  // namespace universe
