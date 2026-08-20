#pragma once

#include <cstdint>
#include <limits>

namespace universe {

struct SimulationDuration {
    std::int64_t ticks{};
};

struct SimulationTime {
    std::int64_t ticks{};
};

[[nodiscard]] constexpr bool operator==(SimulationTime lhs, SimulationTime rhs) noexcept {
    return lhs.ticks == rhs.ticks;
}

[[nodiscard]] constexpr bool operator<(SimulationTime lhs, SimulationTime rhs) noexcept {
    return lhs.ticks < rhs.ticks;
}

[[nodiscard]] constexpr bool operator<=(SimulationTime lhs, SimulationTime rhs) noexcept {
    return lhs.ticks <= rhs.ticks;
}

[[nodiscard]] constexpr bool operator>=(SimulationTime lhs, SimulationTime rhs) noexcept {
    return lhs.ticks >= rhs.ticks;
}

[[nodiscard]] constexpr bool operator>(SimulationTime lhs, SimulationTime rhs) noexcept {
    return lhs.ticks > rhs.ticks;
}

[[nodiscard]] constexpr SimulationDuration operator-(SimulationTime lhs, SimulationTime rhs) noexcept {
    return SimulationDuration{lhs.ticks - rhs.ticks};
}

[[nodiscard]] constexpr bool checked_add_time(
    SimulationTime base,
    SimulationDuration delta,
    SimulationTime& result) noexcept {
    if (delta.ticks > 0 && base.ticks > (std::numeric_limits<std::int64_t>::max() - delta.ticks)) {
        return false;
    }
    if (delta.ticks < 0 && base.ticks < (std::numeric_limits<std::int64_t>::min() - delta.ticks)) {
        return false;
    }
    result = SimulationTime{base.ticks + delta.ticks};
    return true;
}

[[nodiscard]] constexpr SimulationDuration seconds(std::int64_t count) noexcept {
    return SimulationDuration{count};
}

[[nodiscard]] constexpr SimulationDuration minutes(std::int64_t count) noexcept {
    return SimulationDuration{count * 60};
}

[[nodiscard]] constexpr SimulationDuration hours(std::int64_t count) noexcept {
    return SimulationDuration{count * 60 * 60};
}

[[nodiscard]] constexpr SimulationDuration days(std::int64_t count) noexcept {
    return SimulationDuration{count * 24 * 60 * 60};
}

struct DemographicCalendarAdapter {
    static constexpr std::int64_t kDaysPerYear = 365;

    [[nodiscard]] static constexpr SimulationDuration year_duration() noexcept {
        return days(kDaysPerYear);
    }

    [[nodiscard]] static constexpr std::uint64_t demographic_year(SimulationTime time) noexcept {
        if (time.ticks <= 0) {
            return 0;
        }
        return static_cast<std::uint64_t>(time.ticks / year_duration().ticks);
    }
};

}  // namespace universe
