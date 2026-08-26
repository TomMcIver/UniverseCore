#pragma once

#include <cstdint>

namespace universe {

using SeedValue = std::uint64_t;

struct SimulationSeed {
    SeedValue value{};
};

enum class SeedDomain : std::uint64_t {
    Person = 0x2EEA7F6A67B31589ULL,
    Household = 0x8D63B0905C0A9F71ULL,
    Fertility = 0xA31D84F2D6094C57ULL,
    Mortality = 0x6C5791B84AFD20E3ULL,
    InitialPopulation = 0xFA42D6BE138C05ADULL,
    Birth = 0x15D9A721CB5FE980ULL,
    Sex = 0xC783450EF9B2146AULL,
    HouseholdAssignment = 0x4D1278E6A3CF0B95ULL,
    ParentSelection = 0xB540DF7C9186EA23ULL,
    UnrelatedProbe = 0x9A6CBF35217DE804ULL,
    FoodProduction = 0x2BD9AE10C4F3D77AULL,
    Region = 0xD84A3C5F17BE9032ULL,
    RegionAssignment = 0x6EAB1D278C05F43BULL,
    RegionEnvironmentClass = 0x89F45CA20E13B7D2ULL,
    RegionProductiveCapacity = 0xB91EC2D45A6708F1ULL,
    RegionWildCapacity = 0x41C5A278D9B3EF64ULL,
    RegionDailyVariation = 0x734DE90FC1A2BB57ULL,
    RegionWildHarvest = 0xE13AC59472FB0D8EULL,
    Settlement = 0x4E9F2A60D713B8C4ULL,
    SettlementAssignment = 0x9D3C2F7AB16450E1ULL,
    SettlementFounding = 0x21ACF89E57D4306BULL,
    SettlementHouseholdSelection = 0xB3E2D1904A6FC87DULL,
    RegionWoodCapacity = 0x672F9D30B8A5E14CULL,
    RegionStoneCapacity = 0x9CB42DE1086FA753ULL,
    RegionWoodRegeneration = 0x3FA9C1D42BE67580ULL,
    RegionWoodDailyVariation = 0x7BD143AFE6059C21ULL,
    RegionStoneDailyVariation = 0xC5A21F94D83BE067ULL,
    CommunicationTeacherSelection = 0x5173E0A5B96274D1ULL,
    CommunicationKnowledgeSelection = 0xAD204B79E3F6C58AULL,
    CommunicationAttempt = 0x38D4A71C20BF96E4ULL,
};

[[nodiscard]] constexpr std::uint64_t mix_u64(const std::uint64_t value) noexcept {
    std::uint64_t z = value + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

[[nodiscard]] constexpr SimulationSeed derive_seed(
    const SimulationSeed parent,
    const SeedDomain domain,
    const std::uint64_t stable_id) noexcept {
    const auto domain_value = static_cast<std::uint64_t>(domain);
    const auto mixed_parent = mix_u64(parent.value ^ domain_value);
    const auto mixed_id = mix_u64(stable_id ^ (domain_value << 1U));
    return SimulationSeed{mix_u64(mixed_parent ^ mixed_id)};
}

[[nodiscard]] constexpr std::uint64_t seed_random_u64(const SimulationSeed seed) noexcept {
    return mix_u64(seed.value ^ 0xD1B54A32D192ED03ULL);
}

[[nodiscard]] constexpr std::uint64_t seed_random_bounded(const SimulationSeed seed, const std::uint64_t bound) noexcept {
    if (bound == 0U) {
        return 0U;
    }
    return seed_random_u64(seed) % bound;
}

[[nodiscard]] constexpr bool seed_chance_per_10k(const SimulationSeed seed, const std::uint32_t threshold) noexcept {
    if (threshold == 0U) {
        return false;
    }
    if (threshold >= 10'000U) {
        return true;
    }
    return seed_random_bounded(seed, 10'000U) < threshold;
}

}  // namespace universe
