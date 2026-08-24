#include "universe/snapshot.hpp"

#include "universe/simulation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace universe {

namespace {

constexpr std::uint32_t kSnapshotMagic = 0x524F4355U;  // "UCOR" little-endian
constexpr std::size_t kHeaderSize = 4U + 2U + 2U + 8U + 8U;
constexpr std::int64_t kToolWearWorkUnitsPerBreak = 24;

[[nodiscard]] std::uint64_t fnv1a64(std::span<const std::byte> data) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : data) {
        hash ^= static_cast<std::uint64_t>(std::to_integer<unsigned char>(byte));
        hash *= 1099511628211ULL;
    }
    return hash;
}

class SnapshotWriter {
public:
    void write_u8(const std::uint8_t value) {
        bytes_.push_back(static_cast<std::byte>(value));
    }

    void write_bool(const bool value) {
        write_u8(value ? 1U : 0U);
    }

    void write_u16(const std::uint16_t value) {
        for (std::size_t i = 0; i < 2U; ++i) {
            const auto shifted = static_cast<std::uint16_t>(value >> (8U * i));
            write_u8(static_cast<std::uint8_t>(shifted & 0xFFU));
        }
    }

    void write_u32(const std::uint32_t value) {
        for (std::size_t i = 0; i < 4U; ++i) {
            const auto shifted = static_cast<std::uint32_t>(value >> (8U * i));
            write_u8(static_cast<std::uint8_t>(shifted & 0xFFU));
        }
    }

    void write_u64(const std::uint64_t value) {
        for (std::size_t i = 0; i < 8U; ++i) {
            const auto shifted = static_cast<std::uint64_t>(value >> (8U * i));
            write_u8(static_cast<std::uint8_t>(shifted & 0xFFU));
        }
    }

    void write_i64(const std::int64_t value) {
        std::uint64_t encoded = 0;
        std::memcpy(&encoded, &value, sizeof(value));
        write_u64(encoded);
    }

    [[nodiscard]] std::vector<std::byte> take() {
        return std::move(bytes_);
    }

private:
    std::vector<std::byte> bytes_{};
};

class SnapshotReader {
public:
    explicit SnapshotReader(std::span<const std::byte> data) : data_(data) {}

    [[nodiscard]] bool read_u8(std::uint8_t& value) {
        if (!can_read(1U)) {
            return false;
        }
        value = std::to_integer<std::uint8_t>(data_[offset_]);
        ++offset_;
        return true;
    }

    [[nodiscard]] bool read_bool(bool& value) {
        std::uint8_t raw = 0;
        if (!read_u8(raw)) {
            return false;
        }
        if (raw > 1U) {
            return false;
        }
        value = (raw == 1U);
        return true;
    }

    [[nodiscard]] bool read_u16(std::uint16_t& value) {
        std::uint64_t temp = 0;
        if (!read_le_integral(temp, 2U)) {
            return false;
        }
        value = static_cast<std::uint16_t>(temp);
        return true;
    }

    [[nodiscard]] bool read_u32(std::uint32_t& value) {
        std::uint64_t temp = 0;
        if (!read_le_integral(temp, 4U)) {
            return false;
        }
        value = static_cast<std::uint32_t>(temp);
        return true;
    }

    [[nodiscard]] bool read_u64(std::uint64_t& value) {
        return read_le_integral(value, 8U);
    }

    [[nodiscard]] bool read_i64(std::int64_t& value) {
        std::uint64_t encoded = 0;
        if (!read_u64(encoded)) {
            return false;
        }
        std::memcpy(&value, &encoded, sizeof(value));
        return true;
    }

    [[nodiscard]] bool fully_consumed() const noexcept {
        return offset_ == data_.size();
    }

    [[nodiscard]] std::size_t remaining() const noexcept {
        return data_.size() - offset_;
    }

private:
    [[nodiscard]] bool can_read(const std::size_t count) const noexcept {
        return count <= (data_.size() - offset_);
    }

    [[nodiscard]] bool read_le_integral(std::uint64_t& value, const std::size_t byte_count) {
        if (!can_read(byte_count)) {
            return false;
        }

        std::uint64_t result = 0;
        for (std::size_t i = 0; i < byte_count; ++i) {
            const auto current = static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(data_[offset_ + i]));
            result |= (current << (8U * i));
        }

        offset_ += byte_count;
        value = result;
        return true;
    }

    std::span<const std::byte> data_{};
    std::size_t offset_{0};
};

[[nodiscard]] SnapshotLoadResult load_error(const SnapshotErrorCode code, const char* message) {
    return SnapshotLoadResult{.ok = false, .code = code, .message = message};
}

[[nodiscard]] bool is_valid_person_id(const PersonId id) noexcept {
    return id > 0U;
}

[[nodiscard]] bool is_valid_household_id(const HouseholdId id) noexcept {
    return id > 0U;
}

[[nodiscard]] bool is_valid_settlement_id(const SettlementId id) noexcept {
    return id > 0U;
}

[[nodiscard]] bool is_valid_event_id(const EventId id) noexcept {
    return id > 0U;
}

}  // namespace

std::uint64_t snapshot_hash64(const std::span<const std::byte> data) noexcept {
    return fnv1a64(data);
}

std::vector<std::byte> SnapshotCodec::serialize(const Simulation& simulation) {
    SnapshotWriter payload;
    payload.write_u64(simulation.state_.seed);
    payload.write_i64(simulation.state_.time.ticks);
    payload.write_u64(simulation.state_.year);
    payload.write_u64(simulation.state_.population);
    payload.write_u64(simulation.state_.total_births);
    payload.write_u64(simulation.state_.total_deaths);
    payload.write_i64(simulation.state_.total_food_stored);
    payload.write_i64(simulation.state_.total_food_produced);
    payload.write_i64(simulation.state_.total_food_consumed);
    payload.write_i64(simulation.state_.total_unmet_food);
    payload.write_u64(simulation.state_.households_in_shortage);
    payload.write_u64(simulation.state_.population_under_food_stress);
    payload.write_i64(simulation.state_.total_productive_capacity_per_day);
    payload.write_i64(simulation.state_.total_wild_food_capacity);
    payload.write_i64(simulation.state_.total_wild_food_stock);
    payload.write_i64(simulation.state_.total_wild_food_harvested);
    payload.write_i64(simulation.state_.total_wild_food_regenerated);
    payload.write_u64(simulation.state_.eligible_workers);
    payload.write_u64(simulation.state_.active_workers);
    payload.write_u64(simulation.state_.unallocated_workers);
    payload.write_u64(simulation.state_.cultivators);
    payload.write_u64(simulation.state_.gatherers);
    payload.write_u64(simulation.state_.wood_gatherers);
    payload.write_u64(simulation.state_.stone_extractors);
    payload.write_u64(simulation.state_.craft_workers);
    payload.write_i64(simulation.state_.total_labour_capacity);
    payload.write_i64(simulation.state_.total_labour_demand);
    payload.write_i64(simulation.state_.total_labour_allocated);
    payload.write_i64(simulation.state_.cultivation_labour_available);
    payload.write_i64(simulation.state_.cultivation_labour_allocated);
    payload.write_i64(simulation.state_.gathering_labour_available);
    payload.write_i64(simulation.state_.gathering_labour_allocated);
    payload.write_i64(simulation.state_.labour_shortfall);
    payload.write_i64(simulation.state_.total_wood_capacity);
    payload.write_i64(simulation.state_.total_wood_stock);
    payload.write_i64(simulation.state_.total_stone_capacity);
    payload.write_i64(simulation.state_.total_stone_remaining);
    payload.write_i64(simulation.state_.total_wood_harvested);
    payload.write_i64(simulation.state_.total_wood_regenerated);
    payload.write_i64(simulation.state_.total_stone_extracted);
    payload.write_i64(simulation.state_.total_wood_inventory);
    payload.write_i64(simulation.state_.total_stone_inventory);
    payload.write_i64(simulation.state_.total_timber_inventory);
    payload.write_i64(simulation.state_.total_basic_tool_inventory);
    payload.write_i64(simulation.state_.total_timber_produced);
    payload.write_i64(simulation.state_.total_basic_tool_produced);
    payload.write_i64(simulation.state_.total_basic_tool_broken);
    payload.write_u32(simulation.state_.average_tool_coverage_per_10k);
    payload.write_i64(simulation.state_.wood_labour_available);
    payload.write_i64(simulation.state_.wood_labour_allocated);
    payload.write_i64(simulation.state_.stone_labour_available);
    payload.write_i64(simulation.state_.stone_labour_allocated);
    payload.write_i64(simulation.state_.craft_labour_available);
    payload.write_i64(simulation.state_.craft_labour_allocated);
    payload.write_i64(simulation.state_.material_labour_demand);
    payload.write_i64(simulation.state_.material_labour_allocated);
    payload.write_i64(simulation.state_.material_labour_shortfall);
    payload.write_i64(simulation.state_.material_production_shortfall);
    payload.write_u64(simulation.next_person_id_);
    payload.write_u64(simulation.next_household_id_);
    payload.write_u64(simulation.next_region_id_);
    payload.write_u64(simulation.next_settlement_id_);
    payload.write_u64(simulation.next_event_id_);
    payload.write_i64(simulation.demography_interval_.ticks);
    payload.write_i64(simulation.demography_next_run_.ticks);
    payload.write_i64(simulation.food_interval_.ticks);
    payload.write_i64(simulation.food_next_run_.ticks);
    payload.write_u64(simulation.config_.seed);
    payload.write_u64(simulation.config_.initial_population);
    payload.write_bool(simulation.config_.enable_unrelated_random_probe);
    payload.write_i64(simulation.config_.initial_household_food_units);
    payload.write_u32(simulation.config_.food_production_scale_percent);
    payload.write_bool(simulation.config_.food_system_enabled);
    payload.write_u64(simulation.config_.initial_region_count);
    payload.write_i64(simulation.config_.base_region_area_units);
    payload.write_u32(simulation.config_.productive_effort_scale_percent);

    std::vector<const Person*> people_order;
    people_order.reserve(simulation.state_.people.size());
    for (const auto& person : simulation.state_.people) {
        people_order.push_back(&person);
    }
    std::sort(people_order.begin(), people_order.end(), [](const Person* lhs, const Person* rhs) {
        return lhs->id < rhs->id;
    });

    payload.write_u64(static_cast<std::uint64_t>(people_order.size()));
    for (const auto* person : people_order) {
        payload.write_u64(person->id);
        payload.write_i64(person->birth_year);
        payload.write_u8(static_cast<std::uint8_t>(person->sex));
        payload.write_bool(person->alive);
        payload.write_u64(person->household_id);
        payload.write_bool(person->mother_id.has_value());
        if (person->mother_id.has_value()) {
            payload.write_u64(*person->mother_id);
        }
        payload.write_bool(person->father_id.has_value());
        if (person->father_id.has_value()) {
            payload.write_u64(*person->father_id);
        }
        payload.write_u32(static_cast<std::uint32_t>(person->nutrition_points));
        payload.write_u32(static_cast<std::uint32_t>(person->health_points));
        payload.write_u32(person->consecutive_undernourished_days);
        payload.write_u8(static_cast<std::uint8_t>(person->occupation));
    }

    std::vector<const Household*> household_order;
    household_order.reserve(simulation.state_.households.size());
    for (const auto& household : simulation.state_.households) {
        household_order.push_back(&household);
    }
    std::sort(household_order.begin(), household_order.end(), [](const Household* lhs, const Household* rhs) {
        return lhs->id < rhs->id;
    });

    payload.write_u64(static_cast<std::uint64_t>(household_order.size()));
    for (const auto* household : household_order) {
        payload.write_u64(household->id);
        payload.write_u64(household->region_id);
        payload.write_u64(household->settlement_id);
        payload.write_i64(household->resources.food_inventory.value);
        payload.write_i64(household->resources.food_produced_total);
        payload.write_i64(household->resources.food_consumed_total);
        payload.write_i64(household->resources.unmet_food_total);
        payload.write_i64(household->resources.nutritional_stress_points);
        payload.write_u32(household->resources.consecutive_shortage_days);

        std::vector<PersonId> member_ids = household->member_ids;
        std::sort(member_ids.begin(), member_ids.end());
        payload.write_u64(static_cast<std::uint64_t>(member_ids.size()));
        for (const auto member_id : member_ids) {
            payload.write_u64(member_id);
        }
    }

    std::vector<const RegionEnvironmentState*> region_order;
    region_order.reserve(simulation.state_.regions.size());
    for (const auto& region : simulation.state_.regions) {
        region_order.push_back(&region);
    }
    std::sort(region_order.begin(), region_order.end(), [](const RegionEnvironmentState* lhs, const RegionEnvironmentState* rhs) {
        return lhs->id < rhs->id;
    });

    payload.write_u64(static_cast<std::uint64_t>(region_order.size()));
    for (const auto* region : region_order) {
        payload.write_u64(region->id);
        payload.write_i64(region->land_area_units);
        payload.write_u8(static_cast<std::uint8_t>(region->environment_class));
        payload.write_i64(region->productive_capacity_per_day);
        payload.write_i64(region->wild_food_capacity);
        payload.write_i64(region->wild_food_stock);
        payload.write_i64(region->wild_food_regeneration_per_day);
        payload.write_i64(region->wild_food_harvest_limit_per_day);
        payload.write_i64(region->cumulative_wild_food_harvested);
        payload.write_i64(region->cumulative_wild_food_regenerated);
    }

    std::vector<const Settlement*> settlement_order;
    settlement_order.reserve(simulation.state_.settlements.size());
    for (const auto& settlement : simulation.state_.settlements) {
        settlement_order.push_back(&settlement);
    }
    std::sort(settlement_order.begin(), settlement_order.end(), [](const Settlement* lhs, const Settlement* rhs) {
        return lhs->id < rhs->id;
    });

    payload.write_u64(static_cast<std::uint64_t>(settlement_order.size()));
    for (const auto* settlement : settlement_order) {
        payload.write_u64(settlement->id);
        payload.write_u64(settlement->region_id);
        payload.write_bool(settlement->active);
        payload.write_u32(settlement->access_share_points);
    }

    std::vector<const SimulationEvent*> event_order;
    event_order.reserve(simulation.state_.event_log.size());
    for (const auto& event : simulation.state_.event_log) {
        event_order.push_back(&event);
    }
    std::sort(event_order.begin(), event_order.end(), [](const SimulationEvent* lhs, const SimulationEvent* rhs) {
        return lhs->id < rhs->id;
    });

    payload.write_u64(static_cast<std::uint64_t>(event_order.size()));
    for (const auto* event : event_order) {
        payload.write_u64(event->id);
        payload.write_i64(event->timestamp.ticks);
        payload.write_u8(static_cast<std::uint8_t>(event->type));
        payload.write_u64(event->subject_person_id);
        payload.write_bool(event->mother_id.has_value());
        if (event->mother_id.has_value()) {
            payload.write_u64(*event->mother_id);
        }
        payload.write_bool(event->father_id.has_value());
        if (event->father_id.has_value()) {
            payload.write_u64(*event->father_id);
        }
        payload.write_bool(event->household_id.has_value());
        if (event->household_id.has_value()) {
            payload.write_u64(*event->household_id);
        }
    }

    payload.write_bool(simulation.config_.materials_system_enabled);
    payload.write_u32(simulation.config_.wood_resource_scale_percent);
    payload.write_u32(simulation.config_.stone_resource_scale_percent);
    payload.write_u32(simulation.config_.wood_regeneration_scale_percent);
    payload.write_u32(simulation.config_.material_crafting_scale_percent);

    payload.write_u64(static_cast<std::uint64_t>(region_order.size()));
    for (const auto* region : region_order) {
        payload.write_u64(region->id);
        payload.write_i64(region->wood_capacity);
        payload.write_i64(region->wood_stock);
        payload.write_i64(region->wood_regeneration_per_day);
        payload.write_i64(region->cumulative_wood_harvested);
        payload.write_i64(region->cumulative_wood_regenerated);
        payload.write_i64(region->stone_capacity);
        payload.write_i64(region->stone_remaining);
        payload.write_i64(region->cumulative_stone_extracted);
    }

    payload.write_u64(static_cast<std::uint64_t>(settlement_order.size()));
    for (const auto* settlement : settlement_order) {
        payload.write_u64(settlement->id);
        payload.write_i64(settlement->materials.wood.value);
        payload.write_i64(settlement->materials.stone.value);
        payload.write_i64(settlement->materials.timber.value);
        payload.write_i64(settlement->materials.basic_tool.value);
        payload.write_i64(settlement->tool_wear_work_units);
        payload.write_u32(settlement->last_tool_coverage_per_10k);
        payload.write_i64(settlement->cumulative_wood_extracted);
        payload.write_i64(settlement->cumulative_stone_extracted);
        payload.write_i64(settlement->cumulative_timber_produced);
        payload.write_i64(settlement->cumulative_basic_tool_produced);
        payload.write_i64(settlement->cumulative_basic_tool_broken);
    }

    auto payload_bytes = payload.take();
    const auto payload_hash = fnv1a64(payload_bytes);

    SnapshotWriter writer;
    writer.write_u32(kSnapshotMagic);
    writer.write_u16(kSchemaVersion);
    writer.write_u16(0U);
    writer.write_u64(static_cast<std::uint64_t>(payload_bytes.size()));
    writer.write_u64(payload_hash);

    auto snapshot = writer.take();
    snapshot.reserve(kHeaderSize + payload_bytes.size());
    snapshot.insert(snapshot.end(), payload_bytes.begin(), payload_bytes.end());
    return snapshot;
}

SnapshotLoadResult SnapshotCodec::deserialize(const std::span<const std::byte> data, Simulation& destination) {
    if (data.size() < kHeaderSize) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot header is truncated");
    }

    SnapshotReader reader{data};
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint64_t payload_size_u64 = 0;
    std::uint64_t payload_hash = 0;

    if (!reader.read_u32(magic) || !reader.read_u16(version) || !reader.read_u16(reserved) || !reader.read_u64(payload_size_u64) || !reader.read_u64(payload_hash)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot header is malformed");
    }

    if (magic != kSnapshotMagic) {
        return load_error(SnapshotErrorCode::InvalidMagic, "Snapshot magic does not match");
    }

    if (version != kSchemaVersion) {
        return load_error(SnapshotErrorCode::UnsupportedVersion, "Snapshot schema version is unsupported");
    }

    if (reserved != 0U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot reserved header field must be zero");
    }

    if (payload_size_u64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot payload length exceeds supported size");
    }

    const auto payload_size = static_cast<std::size_t>(payload_size_u64);
    if (reader.remaining() != payload_size) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot payload length mismatch");
    }

    const std::span<const std::byte> payload{data.data() + kHeaderSize, payload_size};
    if (fnv1a64(payload) != payload_hash) {
        return load_error(SnapshotErrorCode::InvalidChecksum, "Snapshot payload checksum mismatch");
    }

    SnapshotReader payload_reader{payload};
    WorldState restored_state{};
    SimulationConfig restored_config{};

    std::uint64_t next_person_id = 0;
    std::uint64_t next_household_id = 0;
    std::uint64_t next_region_id = 0;
    std::uint64_t next_settlement_id = 0;
    std::uint64_t next_event_id = 0;
    std::int64_t demography_interval_ticks = 0;
    std::int64_t demography_next_run_ticks = 0;
    std::int64_t food_interval_ticks = 0;
    std::int64_t food_next_run_ticks = 0;

    if (!payload_reader.read_u64(restored_state.seed) ||
        !payload_reader.read_i64(restored_state.time.ticks) ||
        !payload_reader.read_u64(restored_state.year) ||
        !payload_reader.read_u64(restored_state.population) ||
        !payload_reader.read_u64(restored_state.total_births) ||
        !payload_reader.read_u64(restored_state.total_deaths) ||
        !payload_reader.read_i64(restored_state.total_food_stored) ||
        !payload_reader.read_i64(restored_state.total_food_produced) ||
        !payload_reader.read_i64(restored_state.total_food_consumed) ||
        !payload_reader.read_i64(restored_state.total_unmet_food) ||
        !payload_reader.read_u64(restored_state.households_in_shortage) ||
        !payload_reader.read_u64(restored_state.population_under_food_stress) ||
        !payload_reader.read_i64(restored_state.total_productive_capacity_per_day) ||
        !payload_reader.read_i64(restored_state.total_wild_food_capacity) ||
        !payload_reader.read_i64(restored_state.total_wild_food_stock) ||
        !payload_reader.read_i64(restored_state.total_wild_food_harvested) ||
        !payload_reader.read_i64(restored_state.total_wild_food_regenerated) ||
        !payload_reader.read_u64(restored_state.eligible_workers) ||
        !payload_reader.read_u64(restored_state.active_workers) ||
        !payload_reader.read_u64(restored_state.unallocated_workers) ||
        !payload_reader.read_u64(restored_state.cultivators) ||
        !payload_reader.read_u64(restored_state.gatherers) ||
        !payload_reader.read_u64(restored_state.wood_gatherers) ||
        !payload_reader.read_u64(restored_state.stone_extractors) ||
        !payload_reader.read_u64(restored_state.craft_workers) ||
        !payload_reader.read_i64(restored_state.total_labour_capacity) ||
        !payload_reader.read_i64(restored_state.total_labour_demand) ||
        !payload_reader.read_i64(restored_state.total_labour_allocated) ||
        !payload_reader.read_i64(restored_state.cultivation_labour_available) ||
        !payload_reader.read_i64(restored_state.cultivation_labour_allocated) ||
        !payload_reader.read_i64(restored_state.gathering_labour_available) ||
        !payload_reader.read_i64(restored_state.gathering_labour_allocated) ||
        !payload_reader.read_i64(restored_state.labour_shortfall) ||
        !payload_reader.read_i64(restored_state.total_wood_capacity) ||
        !payload_reader.read_i64(restored_state.total_wood_stock) ||
        !payload_reader.read_i64(restored_state.total_stone_capacity) ||
        !payload_reader.read_i64(restored_state.total_stone_remaining) ||
        !payload_reader.read_i64(restored_state.total_wood_harvested) ||
        !payload_reader.read_i64(restored_state.total_wood_regenerated) ||
        !payload_reader.read_i64(restored_state.total_stone_extracted) ||
        !payload_reader.read_i64(restored_state.total_wood_inventory) ||
        !payload_reader.read_i64(restored_state.total_stone_inventory) ||
        !payload_reader.read_i64(restored_state.total_timber_inventory) ||
        !payload_reader.read_i64(restored_state.total_basic_tool_inventory) ||
        !payload_reader.read_i64(restored_state.total_timber_produced) ||
        !payload_reader.read_i64(restored_state.total_basic_tool_produced) ||
        !payload_reader.read_i64(restored_state.total_basic_tool_broken) ||
        !payload_reader.read_u32(restored_state.average_tool_coverage_per_10k) ||
        !payload_reader.read_i64(restored_state.wood_labour_available) ||
        !payload_reader.read_i64(restored_state.wood_labour_allocated) ||
        !payload_reader.read_i64(restored_state.stone_labour_available) ||
        !payload_reader.read_i64(restored_state.stone_labour_allocated) ||
        !payload_reader.read_i64(restored_state.craft_labour_available) ||
        !payload_reader.read_i64(restored_state.craft_labour_allocated) ||
        !payload_reader.read_i64(restored_state.material_labour_demand) ||
        !payload_reader.read_i64(restored_state.material_labour_allocated) ||
        !payload_reader.read_i64(restored_state.material_labour_shortfall) ||
        !payload_reader.read_i64(restored_state.material_production_shortfall) ||
        !payload_reader.read_u64(next_person_id) ||
        !payload_reader.read_u64(next_household_id) ||
        !payload_reader.read_u64(next_region_id) ||
        !payload_reader.read_u64(next_settlement_id) ||
        !payload_reader.read_u64(next_event_id) ||
        !payload_reader.read_i64(demography_interval_ticks) ||
        !payload_reader.read_i64(demography_next_run_ticks) ||
        !payload_reader.read_i64(food_interval_ticks) ||
        !payload_reader.read_i64(food_next_run_ticks) ||
        !payload_reader.read_u64(restored_config.seed) ||
        !payload_reader.read_u64(restored_config.initial_population) ||
        !payload_reader.read_bool(restored_config.enable_unrelated_random_probe) ||
        !payload_reader.read_i64(restored_config.initial_household_food_units) ||
        !payload_reader.read_u32(restored_config.food_production_scale_percent) ||
        !payload_reader.read_bool(restored_config.food_system_enabled) ||
        !payload_reader.read_u64(restored_config.initial_region_count) ||
        !payload_reader.read_i64(restored_config.base_region_area_units) ||
        !payload_reader.read_u32(restored_config.productive_effort_scale_percent)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot fixed section is truncated");
    }

    std::uint64_t people_count = 0;
    if (!payload_reader.read_u64(people_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot people section is truncated");
    }

    if (people_count > payload_reader.remaining() / 41U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot people section has invalid length");
    }

    restored_state.people.clear();
    restored_state.people.reserve(static_cast<std::size_t>(people_count));
    std::unordered_set<PersonId> person_ids;

    for (std::uint64_t i = 0; i < people_count; ++i) {
        Person person{};
        std::uint8_t sex_raw = 0;
        std::uint8_t occupation_raw = 0;
        bool has_mother = false;
        bool has_father = false;
        std::uint32_t nutrition_points = 0;
        std::uint32_t health_points = 0;

        if (!payload_reader.read_u64(person.id) ||
            !payload_reader.read_i64(person.birth_year) ||
            !payload_reader.read_u8(sex_raw) ||
            !payload_reader.read_bool(person.alive) ||
            !payload_reader.read_u64(person.household_id) ||
            !payload_reader.read_bool(has_mother)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot person record is truncated");
        }

        if (sex_raw > 1U) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot person sex value is invalid");
        }
        person.sex = static_cast<BiologicalSex>(sex_raw);

        if (has_mother) {
            PersonId mother_id = 0;
            if (!payload_reader.read_u64(mother_id)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot person mother id is truncated");
            }
            person.mother_id = mother_id;
        }

        if (!payload_reader.read_bool(has_father)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot person father flag is truncated");
        }

        if (has_father) {
            PersonId father_id = 0;
            if (!payload_reader.read_u64(father_id)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot person father id is truncated");
            }
            person.father_id = father_id;
        }

        if (!payload_reader.read_u32(nutrition_points) ||
            !payload_reader.read_u32(health_points) ||
            !payload_reader.read_u32(person.consecutive_undernourished_days) ||
            !payload_reader.read_u8(occupation_raw)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot person health fields are truncated");
        }

        if (nutrition_points > 1'000U || health_points > 1'000U) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot person health fields are out of range");
        }

        person.nutrition_points = static_cast<std::int32_t>(nutrition_points);
        person.health_points = static_cast<std::int32_t>(health_points);

        if (occupation_raw > static_cast<std::uint8_t>(Occupation::CraftWorker)) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot person occupation enum is invalid");
        }
        person.occupation = static_cast<Occupation>(occupation_raw);

        if (!is_valid_person_id(person.id)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot person id must be non-zero");
        }

        if (!person_ids.insert(person.id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot contains duplicate person ids");
        }

        restored_state.people.push_back(person);
    }

    std::uint64_t household_count = 0;
    if (!payload_reader.read_u64(household_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot household section is truncated");
    }

    if (household_count > payload_reader.remaining() / 76U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot household section has invalid length");
    }

    restored_state.households.clear();
    restored_state.households.reserve(static_cast<std::size_t>(household_count));
    std::unordered_set<HouseholdId> household_ids;

    for (std::uint64_t i = 0; i < household_count; ++i) {
        Household household{};
        std::uint64_t member_count = 0;
        if (!payload_reader.read_u64(household.id) ||
            !payload_reader.read_u64(household.region_id) ||
            !payload_reader.read_u64(household.settlement_id) ||
            !payload_reader.read_i64(household.resources.food_inventory.value) ||
            !payload_reader.read_i64(household.resources.food_produced_total) ||
            !payload_reader.read_i64(household.resources.food_consumed_total) ||
            !payload_reader.read_i64(household.resources.unmet_food_total) ||
            !payload_reader.read_i64(household.resources.nutritional_stress_points) ||
            !payload_reader.read_u32(household.resources.consecutive_shortage_days) ||
            !payload_reader.read_u64(member_count)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot household record is truncated");
        }

        if (!is_valid_household_id(household.id)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot household id must be non-zero");
        }
        if (!household_ids.insert(household.id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot contains duplicate household ids");
        }

        if (household.resources.food_inventory.value < 0 ||
            household.resources.food_produced_total < 0 ||
            household.resources.food_consumed_total < 0 ||
            household.resources.unmet_food_total < 0 ||
            household.resources.nutritional_stress_points < 0) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot household food state must be non-negative");
        }

        if (member_count > payload_reader.remaining() / 8U) {
            return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot household member list length is invalid");
        }

        household.member_ids.reserve(static_cast<std::size_t>(member_count));
        std::unordered_set<PersonId> household_member_ids;
        for (std::uint64_t j = 0; j < member_count; ++j) {
            PersonId member_id = 0;
            if (!payload_reader.read_u64(member_id)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot household member record is truncated");
            }
            if (!is_valid_person_id(member_id)) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot household member id must be non-zero");
            }
            if (!household_member_ids.insert(member_id).second) {
                return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot household contains duplicate member ids");
            }
            household.member_ids.push_back(member_id);
        }

        restored_state.households.push_back(std::move(household));
    }

    std::uint64_t region_count = 0;
    if (!payload_reader.read_u64(region_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot region section is truncated");
    }

    if (region_count > payload_reader.remaining() / 64U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot region section has invalid length");
    }

    restored_state.regions.clear();
    restored_state.regions.reserve(static_cast<std::size_t>(region_count));
    std::unordered_set<RegionId> region_ids;

    for (std::uint64_t i = 0; i < region_count; ++i) {
        RegionEnvironmentState region{};
        std::uint8_t environment_class_raw = 0;

        if (!payload_reader.read_u64(region.id) ||
            !payload_reader.read_i64(region.land_area_units) ||
            !payload_reader.read_u8(environment_class_raw) ||
            !payload_reader.read_i64(region.productive_capacity_per_day) ||
            !payload_reader.read_i64(region.wild_food_capacity) ||
            !payload_reader.read_i64(region.wild_food_stock) ||
            !payload_reader.read_i64(region.wild_food_regeneration_per_day) ||
            !payload_reader.read_i64(region.wild_food_harvest_limit_per_day) ||
            !payload_reader.read_i64(region.cumulative_wild_food_harvested) ||
            !payload_reader.read_i64(region.cumulative_wild_food_regenerated)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot region record is truncated");
        }

        if (region.id == 0U) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot region id must be non-zero");
        }
        if (!region_ids.insert(region.id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot contains duplicate region ids");
        }
        if (environment_class_raw > 3U) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot region environment class is invalid");
        }
        region.environment_class = static_cast<EnvironmentClass>(environment_class_raw);

        if (region.land_area_units <= 0 ||
            region.productive_capacity_per_day < 0 ||
            region.wild_food_capacity < 0 ||
            region.wild_food_stock < 0 ||
            region.wild_food_regeneration_per_day < 0 ||
            region.wild_food_harvest_limit_per_day < 0 ||
            region.cumulative_wild_food_harvested < 0 ||
            region.cumulative_wild_food_regenerated < 0) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot region values must be non-negative (except ids)");
        }
        if (region.wild_food_stock > region.wild_food_capacity) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot region stock cannot exceed capacity");
        }

        restored_state.regions.push_back(region);
    }

    std::uint64_t settlement_count = 0;
    if (!payload_reader.read_u64(settlement_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot settlement section is truncated");
    }

    if (settlement_count > payload_reader.remaining() / 21U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot settlement section has invalid length");
    }

    restored_state.settlements.clear();
    restored_state.settlements.reserve(static_cast<std::size_t>(settlement_count));
    std::unordered_set<SettlementId> settlement_ids;

    for (std::uint64_t i = 0; i < settlement_count; ++i) {
        Settlement settlement{};
        if (!payload_reader.read_u64(settlement.id) ||
            !payload_reader.read_u64(settlement.region_id) ||
            !payload_reader.read_bool(settlement.active) ||
            !payload_reader.read_u32(settlement.access_share_points)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot settlement record is truncated");
        }

        if (!is_valid_settlement_id(settlement.id)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement id must be non-zero");
        }
        if (!settlement_ids.insert(settlement.id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot contains duplicate settlement ids");
        }
        if (settlement.access_share_points > 10'000U) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement share points are out of range");
        }
        if (!settlement.active && settlement.access_share_points != 0U) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot extinct settlement must not retain active share allocation");
        }

        restored_state.settlements.push_back(settlement);
    }

    std::uint64_t event_count = 0;
    if (!payload_reader.read_u64(event_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot event section is truncated");
    }

    if (event_count > payload_reader.remaining() / 27U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot event section has invalid length");
    }

    restored_state.event_log.clear();
    restored_state.event_log.reserve(static_cast<std::size_t>(event_count));
    std::unordered_set<EventId> event_ids;

    for (std::uint64_t i = 0; i < event_count; ++i) {
        SimulationEvent event{};
        std::uint8_t type_raw = 0;
        bool has_mother = false;
        bool has_father = false;
        bool has_household = false;

        if (!payload_reader.read_u64(event.id) ||
            !payload_reader.read_i64(event.timestamp.ticks) ||
            !payload_reader.read_u8(type_raw) ||
            !payload_reader.read_u64(event.subject_person_id) ||
            !payload_reader.read_bool(has_mother)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot event record is truncated");
        }

        if (type_raw > 1U) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot event type is invalid");
        }
        event.type = static_cast<EventType>(type_raw);

        if (has_mother) {
            PersonId mother_id = 0;
            if (!payload_reader.read_u64(mother_id)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot event mother id is truncated");
            }
            event.mother_id = mother_id;
        }

        if (!payload_reader.read_bool(has_father)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot event father flag is truncated");
        }
        if (has_father) {
            PersonId father_id = 0;
            if (!payload_reader.read_u64(father_id)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot event father id is truncated");
            }
            event.father_id = father_id;
        }

        if (!payload_reader.read_bool(has_household)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot event household flag is truncated");
        }
        if (has_household) {
            HouseholdId household_id = 0;
            if (!payload_reader.read_u64(household_id)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot event household id is truncated");
            }
            event.household_id = household_id;
        }

        if (!is_valid_event_id(event.id)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot event id must be non-zero");
        }

        if (!event_ids.insert(event.id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot contains duplicate event ids");
        }

        restored_state.event_log.push_back(event);
    }

    if (!payload_reader.read_bool(restored_config.materials_system_enabled) ||
        !payload_reader.read_u32(restored_config.wood_resource_scale_percent) ||
        !payload_reader.read_u32(restored_config.stone_resource_scale_percent) ||
        !payload_reader.read_u32(restored_config.wood_regeneration_scale_percent) ||
        !payload_reader.read_u32(restored_config.material_crafting_scale_percent)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot material config section is truncated");
    }

    std::uint64_t region_material_count = 0;
    if (!payload_reader.read_u64(region_material_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot region material section is truncated");
    }
    if (region_material_count != restored_state.regions.size()) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot region material section count mismatch");
    }

    std::unordered_map<RegionId, RegionEnvironmentState*> region_mutable_by_id;
    region_mutable_by_id.reserve(restored_state.regions.size());
    for (auto& region : restored_state.regions) {
        region_mutable_by_id.emplace(region.id, &region);
    }

    std::unordered_set<RegionId> region_material_ids;
    for (std::uint64_t i = 0; i < region_material_count; ++i) {
        RegionId region_id = 0;
        std::int64_t wood_capacity = 0;
        std::int64_t wood_stock = 0;
        std::int64_t wood_regeneration_per_day = 0;
        std::int64_t cumulative_wood_harvested = 0;
        std::int64_t cumulative_wood_regenerated = 0;
        std::int64_t stone_capacity = 0;
        std::int64_t stone_remaining = 0;
        std::int64_t cumulative_stone_extracted = 0;

        if (!payload_reader.read_u64(region_id) ||
            !payload_reader.read_i64(wood_capacity) ||
            !payload_reader.read_i64(wood_stock) ||
            !payload_reader.read_i64(wood_regeneration_per_day) ||
            !payload_reader.read_i64(cumulative_wood_harvested) ||
            !payload_reader.read_i64(cumulative_wood_regenerated) ||
            !payload_reader.read_i64(stone_capacity) ||
            !payload_reader.read_i64(stone_remaining) ||
            !payload_reader.read_i64(cumulative_stone_extracted)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot region material record is truncated");
        }

        if (!region_material_ids.insert(region_id).second) {
            return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot region material section has duplicate region ids");
        }

        const auto region_it = region_mutable_by_id.find(region_id);
        if (region_it == region_mutable_by_id.end()) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot region material section references unknown region");
        }

        if (wood_capacity < 0 || wood_stock < 0 || wood_regeneration_per_day < 0 ||
            cumulative_wood_harvested < 0 || cumulative_wood_regenerated < 0 ||
            stone_capacity < 0 || stone_remaining < 0 || cumulative_stone_extracted < 0) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot region material values must be non-negative");
        }
        if (wood_stock > wood_capacity) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot wood stock cannot exceed capacity");
        }
        if (stone_remaining > stone_capacity) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot stone remaining cannot exceed capacity");
        }

        auto* region = region_it->second;
        region->wood_capacity = wood_capacity;
        region->wood_stock = wood_stock;
        region->wood_regeneration_per_day = wood_regeneration_per_day;
        region->cumulative_wood_harvested = cumulative_wood_harvested;
        region->cumulative_wood_regenerated = cumulative_wood_regenerated;
        region->stone_capacity = stone_capacity;
        region->stone_remaining = stone_remaining;
        region->cumulative_stone_extracted = cumulative_stone_extracted;
    }

    std::uint64_t settlement_material_count = 0;
    if (!payload_reader.read_u64(settlement_material_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot settlement material section is truncated");
    }
    if (settlement_material_count != restored_state.settlements.size()) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot settlement material section count mismatch");
    }

    std::unordered_map<SettlementId, Settlement*> settlement_mutable_by_id;
    settlement_mutable_by_id.reserve(restored_state.settlements.size());
    for (auto& settlement : restored_state.settlements) {
        settlement_mutable_by_id.emplace(settlement.id, &settlement);
    }

    std::unordered_set<SettlementId> settlement_material_ids;
    for (std::uint64_t i = 0; i < settlement_material_count; ++i) {
        SettlementId settlement_id = 0;
        std::int64_t wood = 0;
        std::int64_t stone = 0;
        std::int64_t timber = 0;
        std::int64_t basic_tool = 0;
        std::int64_t tool_wear_work_units = 0;
        std::uint32_t last_tool_coverage_per_10k = 0;
        std::int64_t cumulative_wood_extracted = 0;
        std::int64_t cumulative_stone_extracted = 0;
        std::int64_t cumulative_timber_produced = 0;
        std::int64_t cumulative_basic_tool_produced = 0;
        std::int64_t cumulative_basic_tool_broken = 0;

        if (!payload_reader.read_u64(settlement_id) ||
            !payload_reader.read_i64(wood) ||
            !payload_reader.read_i64(stone) ||
            !payload_reader.read_i64(timber) ||
            !payload_reader.read_i64(basic_tool) ||
            !payload_reader.read_i64(tool_wear_work_units) ||
            !payload_reader.read_u32(last_tool_coverage_per_10k) ||
            !payload_reader.read_i64(cumulative_wood_extracted) ||
            !payload_reader.read_i64(cumulative_stone_extracted) ||
            !payload_reader.read_i64(cumulative_timber_produced) ||
            !payload_reader.read_i64(cumulative_basic_tool_produced) ||
            !payload_reader.read_i64(cumulative_basic_tool_broken)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot settlement material record is truncated");
        }

        if (!settlement_material_ids.insert(settlement_id).second) {
            return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot settlement material section has duplicate settlement ids");
        }

        const auto settlement_it = settlement_mutable_by_id.find(settlement_id);
        if (settlement_it == settlement_mutable_by_id.end()) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot settlement material section references unknown settlement");
        }

        if (wood < 0 || stone < 0 || timber < 0 || basic_tool < 0 ||
            tool_wear_work_units < 0 ||
            last_tool_coverage_per_10k > 10'000U ||
            cumulative_wood_extracted < 0 || cumulative_stone_extracted < 0 ||
            cumulative_timber_produced < 0 || cumulative_basic_tool_produced < 0 ||
            cumulative_basic_tool_broken < 0) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement material values must be non-negative");
        }
        if (tool_wear_work_units >= kToolWearWorkUnitsPerBreak) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot tool wear remainder is out of canonical range");
        }
        if (cumulative_basic_tool_broken > cumulative_basic_tool_produced) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot broken tools cannot exceed cumulative production");
        }

        auto* settlement = settlement_it->second;
        settlement->materials.wood.value = wood;
        settlement->materials.stone.value = stone;
        settlement->materials.timber.value = timber;
        settlement->materials.basic_tool.value = basic_tool;
        settlement->tool_wear_work_units = tool_wear_work_units;
        settlement->last_tool_coverage_per_10k = last_tool_coverage_per_10k;
        settlement->cumulative_wood_extracted = cumulative_wood_extracted;
        settlement->cumulative_stone_extracted = cumulative_stone_extracted;
        settlement->cumulative_timber_produced = cumulative_timber_produced;
        settlement->cumulative_basic_tool_produced = cumulative_basic_tool_produced;
        settlement->cumulative_basic_tool_broken = cumulative_basic_tool_broken;
    }

    if (!payload_reader.fully_consumed()) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot payload has trailing undecoded bytes");
    }

    if (restored_state.seed != restored_config.seed) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot state/config seed mismatch");
    }

    if (demography_interval_ticks <= 0) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot scheduler interval must be positive");
    }

    if (demography_interval_ticks != DemographicCalendarAdapter::year_duration().ticks) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot scheduler interval is incompatible with this runtime");
    }

    if (restored_state.time.ticks < 0) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot simulation time must be non-negative");
    }

    const auto expected_year = DemographicCalendarAdapter::demographic_year(restored_state.time);
    if (restored_state.year != expected_year) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot derived year does not match canonical time");
    }

    if (demography_next_run_ticks <= restored_state.time.ticks) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot scheduler next run must be strictly in the future");
    }

    if (food_interval_ticks <= 0) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot food interval must be positive");
    }

    if (food_interval_ticks != days(1).ticks) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot food interval is incompatible with this runtime");
    }

    if (restored_config.food_system_enabled && food_next_run_ticks <= restored_state.time.ticks) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot food next run must be strictly in the future");
    }

    if (restored_config.initial_household_food_units < 0) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot initial household food must be non-negative");
    }

    if (restored_config.food_production_scale_percent > 10'000U) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot food production scale is out of range");
    }

    if (restored_config.initial_region_count == 0U) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot initial region count must be positive");
    }

    if (restored_config.base_region_area_units <= 0) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot base region area units must be positive");
    }

    if (restored_config.productive_effort_scale_percent > 10'000U) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot productive effort scale is out of range");
    }
    if (restored_config.wood_resource_scale_percent > 10'000U ||
        restored_config.stone_resource_scale_percent > 10'000U ||
        restored_config.wood_regeneration_scale_percent > 10'000U ||
        restored_config.material_crafting_scale_percent > 10'000U) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot material scale configuration is out of range");
    }

    if (restored_state.total_labour_capacity < 0 ||
        restored_state.total_labour_demand < 0 ||
        restored_state.total_labour_allocated < 0 ||
        restored_state.cultivation_labour_available < 0 ||
        restored_state.cultivation_labour_allocated < 0 ||
        restored_state.gathering_labour_available < 0 ||
        restored_state.gathering_labour_allocated < 0 ||
        restored_state.labour_shortfall < 0 ||
        restored_state.wood_labour_available < 0 ||
        restored_state.wood_labour_allocated < 0 ||
        restored_state.stone_labour_available < 0 ||
        restored_state.stone_labour_allocated < 0 ||
        restored_state.craft_labour_available < 0 ||
        restored_state.craft_labour_allocated < 0 ||
        restored_state.material_labour_demand < 0 ||
        restored_state.material_labour_allocated < 0 ||
        restored_state.material_labour_shortfall < 0 ||
        restored_state.material_production_shortfall < 0 ||
        restored_state.total_wood_inventory < 0 ||
        restored_state.total_stone_inventory < 0 ||
        restored_state.total_timber_inventory < 0 ||
        restored_state.total_basic_tool_inventory < 0 ||
        restored_state.total_timber_produced < 0 ||
        restored_state.total_basic_tool_produced < 0 ||
        restored_state.total_basic_tool_broken < 0 ||
        restored_state.average_tool_coverage_per_10k > 10'000U) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot labour fields must be non-negative");
    }

    if (restored_state.eligible_workers != (restored_state.active_workers + restored_state.unallocated_workers)) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot eligible worker count must match active+unallocated");
    }

    if (restored_state.total_labour_allocated !=
            (restored_state.cultivation_labour_allocated + restored_state.gathering_labour_allocated) ||
        restored_state.total_labour_allocated > restored_state.total_labour_capacity ||
        restored_state.cultivation_labour_allocated > restored_state.cultivation_labour_available ||
        restored_state.gathering_labour_allocated > restored_state.gathering_labour_available) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot allocated labour fields are inconsistent");
    }

    if (restored_state.total_labour_demand < restored_state.total_labour_allocated) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot labour demand cannot be below allocated labour");
    }

    const auto expected_shortfall = restored_state.total_labour_demand - restored_state.total_labour_allocated;
    if (restored_state.labour_shortfall != expected_shortfall) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot labour shortfall must equal demand minus allocated labour");
    }

    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(restored_state.people.size());
    for (const auto& person : restored_state.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::uint64_t recomputed_eligible_workers = 0;
    std::uint64_t recomputed_active_workers = 0;
    std::uint64_t recomputed_unallocated_workers = 0;
    std::uint64_t recomputed_cultivators = 0;
    std::uint64_t recomputed_gatherers = 0;
    std::uint64_t recomputed_wood_gatherers = 0;
    std::uint64_t recomputed_stone_extractors = 0;
    std::uint64_t recomputed_craft_workers = 0;
    std::int64_t recomputed_total_capacity = 0;
    std::int64_t recomputed_cultivation_available = 0;
    std::int64_t recomputed_gathering_available = 0;
    std::int64_t recomputed_wood_available = 0;
    std::int64_t recomputed_stone_available = 0;
    std::int64_t recomputed_craft_available = 0;

    const auto capacity_parameters = FoodModelParameters{};
    for (const auto& person : restored_state.people) {
        const auto current_year = (restored_state.year > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
            ? std::numeric_limits<std::int64_t>::max()
            : static_cast<std::int64_t>(restored_state.year);
        const auto age = (current_year > person.birth_year)
            ? static_cast<std::uint32_t>(current_year - person.birth_year)
            : 0U;
        const auto labour_capacity = person_labour_capacity_units(
            person.alive,
            age,
            person.nutrition_points,
            person.health_points,
            capacity_parameters,
            restored_config.productive_effort_scale_percent);

        const bool hard_ineligible = !person.alive || age < 18U;
        const bool eligible = !hard_ineligible &&
            person.nutrition_points >= 160 &&
            person.health_points >= 160 &&
            labour_capacity > 0;

        if (hard_ineligible && person.occupation != Occupation::None) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot dead/child person must not hold productive occupation");
        }

        if (!eligible) {
            continue;
        }

        ++recomputed_eligible_workers;
        recomputed_total_capacity += labour_capacity;

        if (person.occupation == Occupation::SubsistenceCultivator) {
            ++recomputed_active_workers;
            ++recomputed_cultivators;
            recomputed_cultivation_available += labour_capacity;
        } else if (person.occupation == Occupation::WildFoodGatherer) {
            ++recomputed_active_workers;
            ++recomputed_gatherers;
            recomputed_gathering_available += labour_capacity;
        } else if (person.occupation == Occupation::WoodGatherer) {
            ++recomputed_active_workers;
            ++recomputed_wood_gatherers;
            recomputed_wood_available += labour_capacity;
        } else if (person.occupation == Occupation::StoneExtractor) {
            ++recomputed_active_workers;
            ++recomputed_stone_extractors;
            recomputed_stone_available += labour_capacity;
        } else if (person.occupation == Occupation::CraftWorker) {
            ++recomputed_active_workers;
            ++recomputed_craft_workers;
            recomputed_craft_available += labour_capacity;
        } else {
            ++recomputed_unallocated_workers;
        }
    }

    if (restored_state.eligible_workers != recomputed_eligible_workers ||
        restored_state.active_workers != recomputed_active_workers ||
        restored_state.unallocated_workers != recomputed_unallocated_workers ||
        restored_state.cultivators != recomputed_cultivators ||
        restored_state.gatherers != recomputed_gatherers ||
        restored_state.total_labour_capacity != recomputed_total_capacity ||
        restored_state.cultivation_labour_available != recomputed_cultivation_available ||
        restored_state.gathering_labour_available != recomputed_gathering_available) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot occupation/labour aggregates are inconsistent");
    }

    restored_state.wood_gatherers = recomputed_wood_gatherers;
    restored_state.stone_extractors = recomputed_stone_extractors;
    restored_state.craft_workers = recomputed_craft_workers;
    restored_state.wood_labour_available = recomputed_wood_available;
    restored_state.stone_labour_available = recomputed_stone_available;
    restored_state.craft_labour_available = recomputed_craft_available;

    std::unordered_map<HouseholdId, const Household*> household_by_id;
    household_by_id.reserve(restored_state.households.size());
    for (const auto& household : restored_state.households) {
        household_by_id.emplace(household.id, &household);
    }

    std::unordered_map<RegionId, const RegionEnvironmentState*> region_by_id;
    region_by_id.reserve(restored_state.regions.size());
    RegionId max_region_id = 0;
    for (const auto& region : restored_state.regions) {
        region_by_id.emplace(region.id, &region);
        max_region_id = std::max(max_region_id, region.id);
    }

    if (restored_state.regions.empty()) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot must contain at least one region");
    }

    const auto expected_next_region_id = (max_region_id == 0U) ? 1U : (max_region_id + 1U);
    if (next_region_id != expected_next_region_id) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot next region id is inconsistent");
    }

    std::unordered_map<SettlementId, const Settlement*> settlement_by_id;
    settlement_by_id.reserve(restored_state.settlements.size());
    SettlementId max_settlement_id = 0;
    std::unordered_map<RegionId, std::uint32_t> settlement_share_by_region;

    if (!restored_state.households.empty() && restored_state.settlements.empty()) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot households require settlement records");
    }

    for (const auto& settlement : restored_state.settlements) {
        if (!region_by_id.contains(settlement.region_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot settlement references unknown region");
        }

        settlement_by_id.emplace(settlement.id, &settlement);
        max_settlement_id = std::max(max_settlement_id, settlement.id);
        if (settlement.active) {
            settlement_share_by_region[settlement.region_id] += settlement.access_share_points;
        }
    }

    for (const auto& [region_id, allocated_share] : settlement_share_by_region) {
        (void)region_id;
        if (allocated_share > 10'000U) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement allocations exceed regional capacity");
        }
    }

    const auto expected_next_settlement_id = (max_settlement_id == 0U) ? 1U : (max_settlement_id + 1U);
    if (next_settlement_id != expected_next_settlement_id) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot next settlement id is inconsistent");
    }

    std::unordered_map<PersonId, std::uint32_t> membership_counts;
    membership_counts.reserve(restored_state.people.size());

    for (const auto& household : restored_state.households) {
        if (!region_by_id.contains(household.region_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot household references unknown region");
        }
        if (!settlement_by_id.contains(household.settlement_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot household references unknown settlement");
        }
        const auto* settlement = settlement_by_id.at(household.settlement_id);
        if (settlement->region_id != household.region_id) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot household settlement/region mismatch");
        }

        for (const auto member_id : household.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it == person_by_id.end()) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot household member id references unknown person");
            }
            if (!person_it->second->alive) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot household member references a dead person");
            }
            if (!settlement->active) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot extinct settlement cannot contain alive household members");
            }
            if (person_it->second->household_id != household.id) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot person household id does not match household membership");
            }
            ++membership_counts[member_id];
        }
    }

    std::uint64_t alive_count = 0;
    PersonId max_person_id = 0;
    for (const auto& person : restored_state.people) {
        max_person_id = std::max(max_person_id, person.id);

        if (!household_by_id.contains(person.household_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot person references unknown household");
        }

        if (person.alive) {
            ++alive_count;
            const auto count_it = membership_counts.find(person.id);
            const auto count = (count_it == membership_counts.end()) ? 0U : count_it->second;
            if (count != 1U) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot alive person must appear exactly once in household membership");
            }
        }

        if (person.mother_id.has_value()) {
            const auto mother_it = person_by_id.find(*person.mother_id);
            if (mother_it == person_by_id.end()) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot mother id references unknown person");
            }
            if (mother_it->second->sex != BiologicalSex::Female) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot mother id must reference a female person");
            }
            if (mother_it->second->birth_year >= person.birth_year) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot mother must be older than child");
            }
            if (!person.father_id.has_value()) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot child with mother must have father id");
            }
        }

        if (person.father_id.has_value()) {
            const auto father_it = person_by_id.find(*person.father_id);
            if (father_it == person_by_id.end()) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot father id references unknown person");
            }
            if (father_it->second->sex != BiologicalSex::Male) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot father id must reference a male person");
            }
            if (father_it->second->birth_year >= person.birth_year) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot father must be older than child");
            }
        }

    }

    if (restored_state.population != alive_count) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot population does not match alive person count");
    }

    const auto expected_next_person_id = (max_person_id == 0U) ? 1U : (max_person_id + 1U);
    if (next_person_id != expected_next_person_id) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot next person id is inconsistent");
    }

    HouseholdId max_household_id = 0;
    for (const auto& household : restored_state.households) {
        max_household_id = std::max(max_household_id, household.id);
    }
    const auto expected_next_household_id = (max_household_id == 0U) ? 1U : (max_household_id + 1U);
    if (next_household_id != expected_next_household_id) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot next household id is inconsistent");
    }

    EventId expected_event_id = 1;
    SimulationTime previous_event_time{0};
    bool saw_event = false;
    std::uint64_t birth_event_count = 0;
    std::uint64_t death_event_count = 0;
    for (const auto& event : restored_state.event_log) {
        if (event.id != expected_event_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot event ids must be contiguous");
        }
        ++expected_event_id;

        if (event.timestamp.ticks > restored_state.time.ticks) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot event timestamp exceeds simulation time");
        }

        if (saw_event && event.timestamp < previous_event_time) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot event timestamps must be monotonic");
        }
        previous_event_time = event.timestamp;
        saw_event = true;

        if (!person_by_id.contains(event.subject_person_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot event subject references unknown person");
        }

        if (event.household_id.has_value() && !household_by_id.contains(*event.household_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot event household references unknown household");
        }

        if (event.type == EventType::Birth) {
            ++birth_event_count;
            if (!event.mother_id.has_value() || !event.father_id.has_value()) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot birth event must include mother and father ids");
            }
            const auto mother_it = person_by_id.find(*event.mother_id);
            const auto father_it = person_by_id.find(*event.father_id);
            if (mother_it == person_by_id.end() || father_it == person_by_id.end()) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot birth event parent references are invalid");
            }
            if (mother_it->second->sex != BiologicalSex::Female || father_it->second->sex != BiologicalSex::Male) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot birth event parent sexes are invalid");
            }
        } else {
            ++death_event_count;
            if (event.mother_id.has_value() || event.father_id.has_value()) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot death event must not include parent ids");
            }
        }
    }

    const auto expected_next_event_id = static_cast<EventId>(restored_state.event_log.size() + 1U);
    if (next_event_id != expected_next_event_id) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot next event id is inconsistent");
    }

    if (restored_state.total_births != birth_event_count || restored_state.total_deaths != death_event_count) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot event totals do not match cumulative counters");
    }

    std::int64_t total_productive_capacity_per_day = 0;
    std::int64_t total_wild_food_capacity = 0;
    std::int64_t total_wild_food_stock = 0;
    std::int64_t total_wild_food_harvested = 0;
    std::int64_t total_wild_food_regenerated = 0;
    std::int64_t total_wood_capacity = 0;
    std::int64_t total_wood_stock = 0;
    std::int64_t total_stone_capacity = 0;
    std::int64_t total_stone_remaining = 0;
    std::int64_t total_wood_harvested = 0;
    std::int64_t total_wood_regenerated = 0;
    std::int64_t total_stone_extracted = 0;

    for (const auto& region : restored_state.regions) {
        total_productive_capacity_per_day += region.productive_capacity_per_day;
        total_wild_food_capacity += region.wild_food_capacity;
        total_wild_food_stock += region.wild_food_stock;
        total_wild_food_harvested += region.cumulative_wild_food_harvested;
        total_wild_food_regenerated += region.cumulative_wild_food_regenerated;
        total_wood_capacity += region.wood_capacity;
        total_wood_stock += region.wood_stock;
        total_stone_capacity += region.stone_capacity;
        total_stone_remaining += region.stone_remaining;
        total_wood_harvested += region.cumulative_wood_harvested;
        total_wood_regenerated += region.cumulative_wood_regenerated;
        total_stone_extracted += region.cumulative_stone_extracted;
    }

    if (restored_state.total_productive_capacity_per_day != total_productive_capacity_per_day ||
        restored_state.total_wild_food_capacity != total_wild_food_capacity ||
        restored_state.total_wild_food_stock != total_wild_food_stock ||
        restored_state.total_wild_food_harvested != total_wild_food_harvested ||
        restored_state.total_wild_food_regenerated != total_wild_food_regenerated) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot environment aggregate totals are inconsistent");
    }

    restored_state.total_wood_capacity = total_wood_capacity;
    restored_state.total_wood_stock = total_wood_stock;
    restored_state.total_stone_capacity = total_stone_capacity;
    restored_state.total_stone_remaining = total_stone_remaining;
    restored_state.total_wood_harvested = total_wood_harvested;
    restored_state.total_wood_regenerated = total_wood_regenerated;
    restored_state.total_stone_extracted = total_stone_extracted;

    std::int64_t total_wood_inventory = 0;
    std::int64_t total_stone_inventory = 0;
    std::int64_t total_timber_inventory = 0;
    std::int64_t total_basic_tool_inventory = 0;
    std::int64_t total_timber_produced = 0;
    std::int64_t total_basic_tool_produced = 0;
    std::int64_t total_basic_tool_broken = 0;
    std::uint64_t coverage_sum = 0;
    std::uint64_t active_settlement_count = 0;

    for (const auto& settlement : restored_state.settlements) {
        total_wood_inventory += settlement.materials.wood.value;
        total_stone_inventory += settlement.materials.stone.value;
        total_timber_inventory += settlement.materials.timber.value;
        total_basic_tool_inventory += settlement.materials.basic_tool.value;
        total_timber_produced += settlement.cumulative_timber_produced;
        total_basic_tool_produced += settlement.cumulative_basic_tool_produced;
        total_basic_tool_broken += settlement.cumulative_basic_tool_broken;
        if (settlement.active) {
            ++active_settlement_count;
            coverage_sum += settlement.last_tool_coverage_per_10k;
        }
    }

    restored_state.total_wood_inventory = total_wood_inventory;
    restored_state.total_stone_inventory = total_stone_inventory;
    restored_state.total_timber_inventory = total_timber_inventory;
    restored_state.total_basic_tool_inventory = total_basic_tool_inventory;
    restored_state.total_timber_produced = total_timber_produced;
    restored_state.total_basic_tool_produced = total_basic_tool_produced;
    restored_state.total_basic_tool_broken = total_basic_tool_broken;
    if (restored_state.total_basic_tool_broken > restored_state.total_basic_tool_produced) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot total broken tools cannot exceed cumulative production");
    }
    restored_state.average_tool_coverage_per_10k =
        active_settlement_count > 0
            ? static_cast<std::uint32_t>(coverage_sum / active_settlement_count)
            : 0U;

    std::int64_t total_food_stored = 0;
    std::int64_t total_food_produced = 0;
    std::int64_t total_food_consumed = 0;
    std::int64_t total_unmet_food = 0;
    std::uint64_t households_in_shortage = 0;
    std::uint64_t population_under_food_stress = 0;
    std::unordered_map<HouseholdId, bool> household_in_stress;
    household_in_stress.reserve(restored_state.households.size());

    for (const auto& household : restored_state.households) {
        total_food_stored += household.resources.food_inventory.value;
        total_food_produced += household.resources.food_produced_total;
        total_food_consumed += household.resources.food_consumed_total;
        total_unmet_food += household.resources.unmet_food_total;
        if (household.resources.consecutive_shortage_days > 0U) {
            ++households_in_shortage;
        }
        household_in_stress.emplace(household.id, household.resources.nutritional_stress_points > 0);
    }

    for (const auto& person : restored_state.people) {
        if (!person.alive) {
            continue;
        }

        if (person.nutrition_points < 550 || person.health_points < 500) {
            ++population_under_food_stress;
        }
    }

    if (restored_state.total_food_stored != total_food_stored ||
        restored_state.total_food_produced != total_food_produced ||
        restored_state.total_food_consumed != total_food_consumed ||
        restored_state.total_unmet_food != total_unmet_food ||
        restored_state.households_in_shortage != households_in_shortage ||
        restored_state.population_under_food_stress != population_under_food_stress) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot food aggregate totals are inconsistent");
    }

    std::sort(restored_state.people.begin(), restored_state.people.end(), [](const Person& lhs, const Person& rhs) {
        return lhs.id < rhs.id;
    });
    std::sort(restored_state.households.begin(), restored_state.households.end(), [](const Household& lhs, const Household& rhs) {
        return lhs.id < rhs.id;
    });
    for (auto& household : restored_state.households) {
        std::sort(household.member_ids.begin(), household.member_ids.end());
    }
    std::sort(restored_state.regions.begin(), restored_state.regions.end(), [](const RegionEnvironmentState& lhs, const RegionEnvironmentState& rhs) {
        return lhs.id < rhs.id;
    });
    std::sort(restored_state.settlements.begin(), restored_state.settlements.end(), [](const Settlement& lhs, const Settlement& rhs) {
        return lhs.id < rhs.id;
    });
    std::sort(restored_state.event_log.begin(), restored_state.event_log.end(), [](const SimulationEvent& lhs, const SimulationEvent& rhs) {
        return lhs.id < rhs.id;
    });

    destination.config_ = restored_config;
    destination.state_ = std::move(restored_state);
    destination.simulation_seed_ = SimulationSeed{destination.state_.seed};
    destination.next_person_id_ = next_person_id;
    destination.next_household_id_ = next_household_id;
    destination.next_region_id_ = next_region_id;
    destination.next_settlement_id_ = next_settlement_id;
    destination.next_event_id_ = next_event_id;
    destination.demography_interval_ = SimulationDuration{demography_interval_ticks};
    destination.demography_next_run_ = SimulationTime{demography_next_run_ticks};
    destination.food_interval_ = SimulationDuration{food_interval_ticks};
    destination.food_next_run_ = SimulationTime{food_next_run_ticks};

    return SnapshotLoadResult{.ok = true, .code = SnapshotErrorCode::None, .message = ""};
}

}  // namespace universe
