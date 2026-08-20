#pragma once

#include <cstdint>

namespace universe {

using Seed = std::uint64_t;

struct SimulationConfig {
    Seed seed{1};
    std::uint64_t initial_population{200};
    std::uint32_t birth_rate_per_10k{250};
    std::uint32_t death_rate_per_10k{160};
};

struct WorldState {
    Seed seed{};
    std::uint64_t year{};
    std::uint64_t population{};
    std::uint64_t total_births{};
    std::uint64_t total_deaths{};
};

class Simulation {
public:
    explicit Simulation(SimulationConfig config);

    [[nodiscard]] const WorldState& state() const noexcept;
    void step_year();
    void run_years(std::uint64_t years);

private:
    class DeterministicRng {
    public:
        explicit DeterministicRng(Seed seed) noexcept;
        [[nodiscard]] std::uint64_t next_u64() noexcept;
        [[nodiscard]] bool chance_per_10k(std::uint32_t threshold) noexcept;

    private:
        std::uint64_t state_;
    };

    [[nodiscard]] std::uint64_t sample_events(std::uint64_t trials, std::uint32_t rate_per_10k);

    SimulationConfig config_;
    WorldState state_;
    DeterministicRng rng_;
};

}  // namespace universe
