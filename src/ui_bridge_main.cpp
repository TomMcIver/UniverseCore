#include "universe/demo_scenario.hpp"
#include "universe/ui/simulation_controller.hpp"
#include "universe/ui/web_protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using universe::SimulationConfig;
using universe::ui::ClientCommand;
using universe::ui::ClientCommandType;
using universe::ui::SimulationController;

constexpr std::uint32_t kProtocolVersion = 2U;
// Days advanced per 100 ms loop while jumping: 20 years in about 12 s, fast
// enough for the demo but slow enough to watch the village change.
constexpr std::uint64_t kJumpDaysPerTick = 60U;

struct RuntimeState {
    std::atomic<bool> running{true};
    std::atomic<bool> paused{true};
    std::atomic<std::uint32_t> speed{1};

    std::mutex queue_mutex;
    std::mutex output_mutex;
    std::deque<ClientCommand> command_queue;
};

void emit_line(RuntimeState& runtime, const std::string& json_line) {
    std::lock_guard<std::mutex> lock{runtime.output_mutex};
    std::cout << json_line << '\n';
}

void command_reader_thread(RuntimeState& runtime) {
    std::string line;
    while (runtime.running.load() && std::getline(std::cin, line)) {
        if (line.size() > 4096U) {
            emit_line(runtime, universe::ui::make_error_message(kProtocolVersion, "command_too_large", "Command exceeds max size"));
            continue;
        }

        std::string error;
        const auto parsed = universe::ui::parse_client_command(line, error);
        if (!parsed.has_value()) {
            emit_line(runtime, universe::ui::make_error_message(kProtocolVersion, "invalid_command", error));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock{runtime.queue_mutex};
            runtime.command_queue.push_back(*parsed);
        }
    }

    runtime.running.store(false);
}

std::optional<universe::DemoScenario> load_scenario_file(const std::string& path, std::string& error) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        error = "cannot open file";
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return universe::parse_demo_scenario(contents.str(), error);
}

std::vector<ClientCommand> drain_commands(RuntimeState& runtime) {
    std::vector<ClientCommand> commands;
    std::lock_guard<std::mutex> lock{runtime.queue_mutex};
    while (!runtime.command_queue.empty()) {
        commands.push_back(runtime.command_queue.front());
        runtime.command_queue.pop_front();
    }
    return commands;
}

}  // namespace

int main(int argc, char** argv) {
    SimulationConfig config{};
    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg{argv[i]};
            if (arg == "--seed" && i + 1 < argc) {
                config.seed = static_cast<std::uint64_t>(std::stoull(argv[++i]));
            } else if (arg == "--population" && i + 1 < argc) {
                config.initial_population = static_cast<std::uint64_t>(std::stoull(argv[++i]));
            } else if (arg == "--scenario" && i + 1 < argc) {
                const std::string path{argv[++i]};
                std::string error;
                const auto scenario = load_scenario_file(path, error);
                if (!scenario.has_value()) {
                    std::cerr << "Invalid scenario " << path << ": " << error << '\n';
                    return 2;
                }
                config = universe::make_demo_simulation_config(*scenario);
            }
        }
    } catch (const std::exception&) {
        std::cerr << "Invalid bridge startup arguments\n";
        return 2;
    }

    SimulationController controller{config};
    RuntimeState runtime{};

    std::thread reader{command_reader_thread, std::ref(runtime)};

    emit_line(runtime, universe::ui::make_hello_message(kProtocolVersion));
    auto previous_snapshot = universe::ui::build_world_snapshot(controller);
    emit_line(runtime, universe::ui::make_snapshot_message(kProtocolVersion, previous_snapshot, true));
    emit_line(runtime, universe::ui::make_status_message(kProtocolVersion, previous_snapshot.tick, true, 1U));
    std::size_t last_event_count = controller.events().size();
    // Set while a run_until_year jump is in progress; cleared on arrival or pause.
    std::optional<std::uint64_t> target_year{};

    const auto emit_status = [&](const std::uint64_t tick) {
        emit_line(runtime, universe::ui::make_status_message(
            kProtocolVersion, tick, runtime.paused.load(), runtime.speed.load(), target_year));
    };

    // Streams the events, world delta and status produced by the last advance.
    const auto publish_progress = [&]() {
        const auto events = controller.events();
        for (std::size_t i = last_event_count; i < events.size(); ++i) {
            emit_line(runtime, universe::ui::make_world_event_message(kProtocolVersion, controller, events[i]));
        }
        last_event_count = events.size();
        const auto snapshot = universe::ui::build_world_snapshot(controller);
        const auto delta = universe::ui::build_world_delta(snapshot, previous_snapshot);
        emit_line(runtime, universe::ui::make_delta_message(kProtocolVersion, delta));
        previous_snapshot = snapshot;
        emit_status(snapshot.tick);
    };

    std::uint64_t step_counter = 0;
    while (runtime.running.load()) {
        const auto commands = drain_commands(runtime);
        for (const auto& command : commands) {
            switch (command.type) {
                case ClientCommandType::Hello: {
                    emit_line(runtime, universe::ui::make_hello_message(kProtocolVersion));
                    break;
                }
                case ClientCommandType::Pause: {
                    runtime.paused.store(true);
                    target_year.reset();
                    emit_status(universe::ui::build_world_snapshot(controller).tick);
                    break;
                }
                case ClientCommandType::Resume: {
                    runtime.paused.store(false);
                    emit_status(universe::ui::build_world_snapshot(controller).tick);
                    break;
                }
                case ClientCommandType::SetSpeed: {
                    runtime.speed.store(command.speed);
                    emit_status(universe::ui::build_world_snapshot(controller).tick);
                    break;
                }
                case ClientCommandType::RunUntilYear: {
                    if (command.target_year <= controller.summary().year) {
                        emit_line(runtime, universe::ui::make_error_message(
                            kProtocolVersion, "invalid_command", "Target year must be after the current year"));
                        break;
                    }
                    target_year = command.target_year;
                    runtime.paused.store(false);
                    emit_status(universe::ui::build_world_snapshot(controller).tick);
                    break;
                }
                case ClientCommandType::SingleStep: {
                    controller.advance_days(1);
                    publish_progress();
                    break;
                }
                case ClientCommandType::RequestSnapshot: {
                    const auto snapshot = universe::ui::build_world_snapshot(controller);
                    emit_line(runtime, universe::ui::make_snapshot_message(kProtocolVersion, snapshot, true));
                    previous_snapshot = snapshot;
                    break;
                }
                case ClientCommandType::RequestViewModel: {
                    emit_line(runtime, universe::ui::make_view_model_message(kProtocolVersion, controller, command.view_model_kind, command.entity_id));
                    break;
                }
            }
        }

        if (target_year.has_value()) {
            if (controller.advance_toward_year(*target_year, kJumpDaysPerTick) == 0U ||
                controller.summary().year >= *target_year) {
                target_year.reset();
                runtime.paused.store(true);
            }
            publish_progress();
        } else if (!runtime.paused.load()) {
            const auto speed = runtime.speed.load();
            for (std::uint32_t i = 0; i < speed; ++i) {
                controller.advance_days(1);
            }
            publish_progress();
        } else if ((step_counter % 20U) == 0U) {
            emit_status(universe::ui::build_world_snapshot(controller).tick);
        }

        std::cout.flush();
        ++step_counter;
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    }

    if (reader.joinable()) {
        reader.join();
    }
    return 0;
}
