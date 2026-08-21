#pragma once

#include <cstdint>

namespace universe {

enum class ResourceType : std::uint8_t {
    Food = 0,
};

struct ResourceAmount {
    std::int64_t value{};
};

struct HouseholdResourceState {
    ResourceAmount food_inventory{};
    std::int64_t food_produced_total{};
    std::int64_t food_consumed_total{};
    std::int64_t unmet_food_total{};
    std::int64_t nutritional_stress_points{};
    std::uint32_t consecutive_shortage_days{};
};

struct FoodModelParameters {
    std::int64_t adult_daily_food_units{10};
    std::int64_t child_daily_food_units{6};
    std::int64_t adult_daily_production_units{22};
    std::int64_t child_daily_production_units{6};
    std::int64_t stress_recovery_per_day{6};
    std::int64_t max_stress_points{5'000};
    std::int32_t min_person_nutrition_points{0};
    std::int32_t max_person_nutrition_points{1'000};
    std::int32_t min_person_health_points{0};
    std::int32_t max_person_health_points{1'000};
};

[[nodiscard]] bool checked_add_i64(std::int64_t lhs, std::int64_t rhs, std::int64_t& result) noexcept;
[[nodiscard]] bool checked_mul_i64(std::int64_t lhs, std::int64_t rhs, std::int64_t& result) noexcept;

[[nodiscard]] std::int64_t apply_scaled_percent(std::int64_t value, std::uint32_t percent) noexcept;

[[nodiscard]] std::uint32_t household_base_productivity_percent(std::uint64_t household_id) noexcept;

[[nodiscard]] std::uint32_t shortage_stress_delta_per_day(
    std::int64_t required_food,
    std::int64_t unmet_food) noexcept;

[[nodiscard]] std::uint32_t food_stress_mortality_penalty_per_10k(
    std::int64_t nutritional_stress_points,
    bool is_child) noexcept;

[[nodiscard]] std::uint32_t food_stress_fertility_scale_percent(
    std::int64_t nutritional_stress_points) noexcept;

[[nodiscard]] std::uint32_t nutrition_health_mortality_penalty_per_10k(
    std::int32_t nutrition_points,
    std::int32_t health_points,
    bool is_child) noexcept;

[[nodiscard]] std::uint32_t nutrition_health_fertility_scale_percent(
    std::int32_t nutrition_points,
    std::int32_t health_points) noexcept;

[[nodiscard]] std::int64_t age_based_labour_capacity_units(
    std::uint32_t age_years,
    const FoodModelParameters& parameters) noexcept;

[[nodiscard]] std::uint32_t nutrition_health_labour_scale_percent(
    std::int32_t nutrition_points,
    std::int32_t health_points) noexcept;

[[nodiscard]] std::int64_t person_labour_capacity_units(
    bool alive,
    std::uint32_t age_years,
    std::int32_t nutrition_points,
    std::int32_t health_points,
    const FoodModelParameters& parameters,
    std::uint32_t labour_scale_percent) noexcept;

}  // namespace universe
