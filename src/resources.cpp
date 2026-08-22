#include "universe/resources.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace universe {

bool checked_add_i64(const std::int64_t lhs, const std::int64_t rhs, std::int64_t& result) noexcept {
    if (rhs > 0 && lhs > (std::numeric_limits<std::int64_t>::max() - rhs)) {
        return false;
    }
    if (rhs < 0 && lhs < (std::numeric_limits<std::int64_t>::min() - rhs)) {
        return false;
    }

    result = lhs + rhs;
    return true;
}

bool checked_mul_i64(const std::int64_t lhs, const std::int64_t rhs, std::int64_t& result) noexcept {
    if (lhs == 0 || rhs == 0) {
        result = 0;
        return true;
    }

    if (lhs == -1 && rhs == std::numeric_limits<std::int64_t>::min()) {
        return false;
    }
    if (rhs == -1 && lhs == std::numeric_limits<std::int64_t>::min()) {
        return false;
    }

    const auto max_i64 = std::numeric_limits<std::int64_t>::max();
    const auto min_i64 = std::numeric_limits<std::int64_t>::min();

    if (lhs > 0) {
        if (rhs > 0) {
            if (lhs > (max_i64 / rhs)) {
                return false;
            }
        } else if (rhs < (min_i64 / lhs)) {
            return false;
        }
    } else if (rhs > 0) {
        if (lhs < (min_i64 / rhs)) {
            return false;
        }
    } else if (lhs != 0 && rhs < (max_i64 / lhs)) {
        return false;
    }

    result = lhs * rhs;
    return true;
}

std::int64_t apply_scaled_percent(const std::int64_t value, const std::uint32_t percent) noexcept {
    if (value <= 0) {
        return 0;
    }

    std::int64_t product = 0;
    if (!checked_mul_i64(value, static_cast<std::int64_t>(percent), product)) {
        return std::numeric_limits<std::int64_t>::max();
    }

    return std::max<std::int64_t>(product / 100, 0);
}

std::uint32_t household_base_productivity_percent(const std::uint64_t household_id) noexcept {
    return 85U + static_cast<std::uint32_t>(household_id % 31U);
}

std::uint32_t shortage_stress_delta_per_day(const std::int64_t required_food, const std::int64_t unmet_food) noexcept {
    if (required_food <= 0 || unmet_food <= 0) {
        return 0;
    }

    const auto unmet_percent = static_cast<std::int64_t>((unmet_food * 100) / required_food);
    if (unmet_percent >= 90) {
        return 28;
    }
    if (unmet_percent >= 70) {
        return 20;
    }
    if (unmet_percent >= 40) {
        return 12;
    }
    if (unmet_percent >= 10) {
        return 6;
    }
    return 3;
}

std::uint32_t food_stress_mortality_penalty_per_10k(const std::int64_t nutritional_stress_points, const bool is_child) noexcept {
    std::uint32_t penalty = 0;
    if (nutritional_stress_points >= 3'000) {
        penalty = 7'500;
    } else if (nutritional_stress_points >= 2'000) {
        penalty = 3'500;
    } else if (nutritional_stress_points >= 1'000) {
        penalty = 1'500;
    } else if (nutritional_stress_points >= 400) {
        penalty = 500;
    }

    if (!is_child) {
        return penalty;
    }

    const auto adjusted = static_cast<std::uint64_t>(penalty) * 5U / 4U;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(adjusted, 10'000U));
}

std::uint32_t food_stress_fertility_scale_percent(const std::int64_t nutritional_stress_points) noexcept {
    if (nutritional_stress_points >= 3'000) {
        return 5U;
    }
    if (nutritional_stress_points >= 2'000) {
        return 25U;
    }
    if (nutritional_stress_points >= 1'000) {
        return 55U;
    }
    if (nutritional_stress_points >= 400) {
        return 80U;
    }
    return 100U;
}

std::uint32_t nutrition_health_mortality_penalty_per_10k(
    const std::int32_t nutrition_points,
    const std::int32_t health_points,
    const bool is_child) noexcept {
    std::uint32_t penalty = 0;

    if (nutrition_points < 80 || health_points < 120) {
        penalty = 5'000;
    } else if (nutrition_points < 180 || health_points < 220) {
        penalty = 2'200;
    } else if (nutrition_points < 320 || health_points < 350) {
        penalty = 900;
    } else if (nutrition_points < 500 || health_points < 500) {
        penalty = 250;
    }

    if (!is_child) {
        return penalty;
    }

    const auto adjusted = static_cast<std::uint64_t>(penalty) * 5U / 4U;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(adjusted, 10'000U));
}

std::uint32_t nutrition_health_fertility_scale_percent(
    const std::int32_t nutrition_points,
    const std::int32_t health_points) noexcept {
    const auto combined = std::min(nutrition_points, health_points);
    if (combined < 100) {
        return 0U;
    }
    if (combined < 220) {
        return 12U;
    }
    if (combined < 350) {
        return 35U;
    }
    if (combined < 500) {
        return 65U;
    }
    if (combined < 700) {
        return 85U;
    }
    return 100U;
}

std::int64_t age_based_labour_capacity_units(
    const std::uint32_t age_years,
    const FoodModelParameters& parameters) noexcept {
    const auto adult_capacity = std::max<std::int64_t>(parameters.adult_daily_production_units, 0);
    if (adult_capacity <= 0) {
        return 0;
    }

    const auto child_capacity = std::max<std::int64_t>(parameters.child_daily_production_units, 0);

    if (age_years < 8U) {
        return 0;
    }
    if (age_years < 12U) {
        return std::max<std::int64_t>(child_capacity / 2, child_capacity > 0 ? 1 : 0);
    }
    if (age_years < 16U) {
        return std::max<std::int64_t>(child_capacity, child_capacity > 0 ? 1 : 0);
    }
    if (age_years < 18U) {
        return std::max<std::int64_t>((adult_capacity * 75) / 100, 1);
    }
    if (age_years < 40U) {
        return adult_capacity;
    }
    if (age_years < 55U) {
        return std::max<std::int64_t>((adult_capacity * 95) / 100, 1);
    }
    if (age_years < 65U) {
        return std::max<std::int64_t>((adult_capacity * 80) / 100, 1);
    }
    if (age_years < 75U) {
        return std::max<std::int64_t>((adult_capacity * 55) / 100, 1);
    }
    if (age_years < 85U) {
        return std::max<std::int64_t>((adult_capacity * 30) / 100, 1);
    }
    return std::max<std::int64_t>((adult_capacity * 10) / 100, 1);
}

std::uint32_t nutrition_health_labour_scale_percent(
    const std::int32_t nutrition_points,
    const std::int32_t health_points) noexcept {
    if (nutrition_points < 80 || health_points < 120) {
        return 0U;
    }

    const auto combined = std::min(nutrition_points, health_points);
    if (combined < 220) {
        return 25U;
    }
    if (combined < 350) {
        return 45U;
    }
    if (combined < 500) {
        return 65U;
    }
    if (combined < 700) {
        return 85U;
    }
    if (combined < 850) {
        return 95U;
    }
    return 100U;
}

std::int64_t person_labour_capacity_units(
    const bool alive,
    const std::uint32_t age_years,
    const std::int32_t nutrition_points,
    const std::int32_t health_points,
    const FoodModelParameters& parameters,
    const std::uint32_t labour_scale_percent) noexcept {
    if (!alive) {
        return 0;
    }

    auto capacity = age_based_labour_capacity_units(age_years, parameters);
    if (capacity <= 0) {
        return 0;
    }

    capacity = apply_scaled_percent(capacity, nutrition_health_labour_scale_percent(nutrition_points, health_points));
    if (capacity <= 0) {
        return 0;
    }

    capacity = apply_scaled_percent(capacity, labour_scale_percent);
    if (capacity <= 0) {
        return 0;
    }

    return std::max<std::int64_t>(capacity, 1);
}

}  // namespace universe
