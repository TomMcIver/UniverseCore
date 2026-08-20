#include "universe/simulation.hpp"

#include <cstdlib>
#include <iostream>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    const universe::SimulationConfig config{
        .seed = 42,
        .initial_population = 200,
        .birth_rate_per_10k = 250,
        .death_rate_per_10k = 160,
    };

    universe::Simulation a{config};
    universe::Simulation b{config};
    a.run_years(100);
    b.run_years(100);

    require(a.state().year == 100, "simulation should advance exactly 100 years");
    require(a.state().population == b.state().population, "same seed must reproduce population");
    require(a.state().total_births == b.state().total_births, "same seed must reproduce births");
    require(a.state().total_deaths == b.state().total_deaths, "same seed must reproduce deaths");

    auto different_config = config;
    different_config.seed = 43;
    universe::Simulation c{different_config};
    c.run_years(100);

    const bool differs =
        a.state().population != c.state().population ||
        a.state().total_births != c.state().total_births ||
        a.state().total_deaths != c.state().total_deaths;
    require(differs, "different seeds should normally produce different histories");

    std::cout << "All UniverseCore tests passed.\n";
    return 0;
}
