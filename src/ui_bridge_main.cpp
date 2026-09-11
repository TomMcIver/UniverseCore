#include "universe/ui/simulation_controller.hpp"
#include "universe/ui/web_protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using universe::SimulationConfig;
using universe::ui::ClientCommand;
using universe::ui::ClientCommandType;
using universe::ui::SimulationController;

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
            emit_line(runtime, universe::ui::make_error_message(1U, "command_too_large", "Command exceeds max size"));
            continue;
        }

        std::string error;
        const auto parsed = universe::ui::parse_client_command(line, error);
        if (!parsed.has_value()) {
            emit_line(runtime, universe::ui::make_error_message(1U, "invalid_command", error));
            continue;
        }

        {
            std::lock_guard<std::mutex> lock{runtime.queue_mutex};
            runtime.command_queue.push_back(*parsed);
        }
    }

    runtime.running.store(false);
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
            }
        }
    } catch (const std::exception&) {
        std::cerr << "Invalid bridge startup arguments\n";
        return 2;
    }

    SimulationController controller{config};
    RuntimeState runtime{};

    std::thread reader{command_reader_thread, std::ref(runtime)};

    emit_line(runtime, universe::ui::make_hello_message(1U));
    auto initial_snapshot = universe::ui::build_world_snapshot(controller);
    emit_line(runtime, universe::ui::make_snapshot_message(1U, initial_snapshot, true));
    emit_line(runtime, universe::ui::make_status_message(1U, initial_snapshot.tick, true, 1U));
    std::size_t last_event_count = controller.events().size();

    std::uint64_t step_counter = 0;
    while (runtime.running.load()) {
        const auto commands = drain_commands(runtime);
        for (const auto& command : commands) {
            switch (command.type) {
                case ClientCommandType::Hello: {
                    emit_line(runtime, universe::ui::make_hello_message(1U));
                    break;
                }
                case ClientCommandType::Pause: {
                    runtime.paused.store(true);
                    const auto snapshot = universe::ui::build_world_snapshot(controller);
                    emit_line(runtime, universe::ui::make_status_message(1U, snapshot.tick, true, runtime.speed.load()));
                    break;
                }
                case ClientCommandType::Resume: {
                    runtime.paused.store(false);
                    const auto snapshot = universe::ui::build_world_snapshot(controller);
                    emit_line(runtime, universe::ui::make_status_message(1U, snapshot.tick, false, runtime.speed.load()));
                    break;
                }
                case ClientCommandType::SetSpeed: {
                    runtime.speed.store(command.speed);
                    const auto snapshot = universe::ui::build_world_snapshot(controller);
                    emit_line(runtime, universe::ui::make_status_message(1U, snapshot.tick, runtime.paused.load(), runtime.speed.load()));
                    break;
                }
                case ClientCommandType::SingleStep: {
                    controller.advance_days(1);
                    const auto events = controller.events();
                    for (std::size_t i = last_event_count; i < events.size(); ++i) {
                        emit_line(runtime, universe::ui::make_world_event_message(1U, events[i]));
                    }
                    last_event_count = events.size();
                    const auto snapshot = universe::ui::build_world_snapshot(controller);
                    emit_line(runtime, universe::ui::make_snapshot_message(1U, snapshot, false));
                    emit_line(runtime, universe::ui::make_status_message(1U, snapshot.tick, runtime.paused.load(), runtime.speed.load()));
                    break;
                }
                case ClientCommandType::RequestSnapshot: {
                    const auto snapshot = universe::ui::build_world_snapshot(controller);
                    emit_line(runtime, universe::ui::make_snapshot_message(1U, snapshot, true));
                    break;
                }
                case ClientCommandType::RequestEntityDetails: {
                    emit_line(runtime, universe::ui::make_entity_details_message(1U, controller, command.entity_id));
                    break;
                }
            }
        }

        if (!runtime.paused.load()) {
            const auto speed = runtime.speed.load();
            for (std::uint32_t i = 0; i < speed; ++i) {
                controller.advance_days(1);
            }

            const auto events = controller.events();
            for (std::size_t i = last_event_count; i < events.size(); ++i) {
                emit_line(runtime, universe::ui::make_world_event_message(1U, events[i]));
            }
            last_event_count = events.size();

            const auto snapshot = universe::ui::build_world_snapshot(controller);
            emit_line(runtime, universe::ui::make_snapshot_message(1U, snapshot, false));
            emit_line(runtime, universe::ui::make_status_message(1U, snapshot.tick, false, speed));
        } else if ((step_counter % 20U) == 0U) {
            const auto snapshot = universe::ui::build_world_snapshot(controller);
            emit_line(runtime, universe::ui::make_status_message(1U, snapshot.tick, true, runtime.speed.load()));
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
