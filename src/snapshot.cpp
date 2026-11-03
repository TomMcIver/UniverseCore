#include "universe/snapshot.hpp"

#include "universe/simulation.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace universe {

namespace {

constexpr std::uint32_t kSnapshotMagic = 0x524F4355U;  // "UCOR" little-endian
constexpr std::size_t kHeaderSize = 4U + 2U + 2U + 8U + 8U;

// Generous sanity bound for canonical region/settlement position, deliberately
// far outside simulation.cpp's actual placement radius (a few thousand units)
// rather than tightly coupled to it, since that constant is private to
// simulation.cpp's translation unit -- this just rejects corrupt/overflowed
// snapshot bytes, not real placement geometry.
constexpr std::int64_t kMaxSnapshotPositionMagnitude = 1'000'000;
constexpr std::int64_t kToolWearWorkUnitsPerBreak = 24;
constexpr std::int64_t kDwellingCapacityBonusPerBuilding = 18;
constexpr std::int64_t kFoodStoreCapacityBonusPerBuilding = 32;
constexpr std::int64_t kWorkshopCapacityBonusPerBuilding = 14;

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

constexpr std::uint16_t kMaxFacingDegreesExclusive = 360U;

// Reads the schema v25 section: one placement record per building in id order,
// then the ascending ids of people whose next occupation reassignment is silent.
[[nodiscard]] SnapshotLoadResult read_v25_placement_and_resync_section(SnapshotReader& reader, WorldState& state) {
    std::uint64_t placement_count = 0;
    if (!reader.read_u64(placement_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot building placement section is truncated");
    }
    if (placement_count != static_cast<std::uint64_t>(state.buildings.size())) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot building placement count does not match buildings");
    }

    std::unordered_map<BuildingId, Building*> building_by_id;
    building_by_id.reserve(state.buildings.size());
    for (auto& building : state.buildings) {
        building_by_id.emplace(building.id, &building);
    }

    BuildingId previous_id = 0;
    for (std::uint64_t i = 0; i < placement_count; ++i) {
        BuildingId building_id = 0;
        std::int64_t position_x = 0;
        std::int64_t position_z = 0;
        std::uint16_t facing_degrees = 0;
        if (!reader.read_u64(building_id) || !reader.read_i64(position_x) || !reader.read_i64(position_z) ||
            !reader.read_u16(facing_degrees)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot building placement record is truncated");
        }
        if (building_id <= previous_id) {
            return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot building placements must be in ascending id order");
        }
        previous_id = building_id;
        const auto building_it = building_by_id.find(building_id);
        if (building_it == building_by_id.end()) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot building placement references unknown building");
        }
        if (position_x < -kMaxSnapshotPositionMagnitude || position_x > kMaxSnapshotPositionMagnitude ||
            position_z < -kMaxSnapshotPositionMagnitude || position_z > kMaxSnapshotPositionMagnitude) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot building placement is out of range");
        }
        if (facing_degrees >= kMaxFacingDegreesExclusive) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot building facing must be below 360 degrees");
        }
        building_it->second->position_x = position_x;
        building_it->second->position_z = position_z;
        building_it->second->facing_degrees = facing_degrees;
    }

    std::uint64_t resync_count = 0;
    if (!reader.read_u64(resync_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot occupation resync section is truncated");
    }
    if (resync_count > reader.remaining() / 8U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot occupation resync section has invalid length");
    }

    std::unordered_map<PersonId, Person*> person_by_id;
    person_by_id.reserve(state.people.size());
    for (auto& person : state.people) {
        person_by_id.emplace(person.id, &person);
    }

    PersonId previous_person_id = 0;
    for (std::uint64_t i = 0; i < resync_count; ++i) {
        PersonId person_id = 0;
        if (!reader.read_u64(person_id)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot occupation resync id is truncated");
        }
        if (person_id <= previous_person_id) {
            return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot occupation resync ids must be ascending");
        }
        previous_person_id = person_id;
        const auto person_it = person_by_id.find(person_id);
        if (person_it == person_by_id.end()) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot occupation resync references unknown person");
        }
        person_it->second->occupation_history_resync_pending = true;
    }

    return SnapshotLoadResult{.ok = true, .code = SnapshotErrorCode::None, .message = {}};
}

constexpr std::uint16_t kViabilitySectionSchemaVersion = 26U;
constexpr std::uint16_t kCoinSectionSchemaVersion = 27U;
constexpr std::uint16_t kMerchantMemorySchemaVersion = 28U;

// Reads the schema v26 section: one record per settlement in ascending id
// order, carrying the days it has spent below the viable size and under food
// strain.
[[nodiscard]] SnapshotLoadResult read_v26_viability_section(SnapshotReader& reader, WorldState& state) {
    std::uint64_t record_count = 0;
    if (!reader.read_u64(record_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot settlement viability section is truncated");
    }
    if (record_count != static_cast<std::uint64_t>(state.settlements.size())) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot settlement viability count does not match settlements");
    }

    std::unordered_map<SettlementId, Settlement*> settlement_by_id;
    settlement_by_id.reserve(state.settlements.size());
    for (auto& settlement : state.settlements) {
        settlement_by_id.emplace(settlement.id, &settlement);
    }

    const auto ticks_per_day = days(1).ticks;
    const auto elapsed_days = state.time.ticks > 0 ? state.time.ticks / ticks_per_day : 0;
    const auto max_elapsed_days = static_cast<std::uint64_t>(elapsed_days);

    SettlementId previous_id = 0;
    for (std::uint64_t i = 0; i < record_count; ++i) {
        SettlementId settlement_id = 0;
        std::uint32_t days_below_viable_size = 0;
        std::uint32_t days_under_food_strain = 0;
        if (!reader.read_u64(settlement_id) || !reader.read_u32(days_below_viable_size) ||
            !reader.read_u32(days_under_food_strain)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot settlement viability record is truncated");
        }
        if (settlement_id <= previous_id) {
            return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot settlement viability records must be in ascending id order");
        }
        previous_id = settlement_id;
        const auto settlement_it = settlement_by_id.find(settlement_id);
        if (settlement_it == settlement_by_id.end()) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot settlement viability references unknown settlement");
        }
        // A count of daily updates cannot exceed the days that have elapsed.
        if (days_below_viable_size > max_elapsed_days || days_under_food_strain > max_elapsed_days) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement viability counter exceeds elapsed days");
        }
        settlement_it->second->days_below_viable_size = days_below_viable_size;
        settlement_it->second->days_under_food_strain = days_under_food_strain;
    }

    return SnapshotLoadResult{.ok = true, .code = SnapshotErrorCode::None, .message = {}};
}

// Reads the schema v27 section, last in the payload: settlement commons coin
// then household coin, each in ascending id order. Coin is never negative, and
// the world's total fits an i64: coin only moves, so every later sum is safe.
[[nodiscard]] SnapshotLoadResult read_v27_coin_section(SnapshotReader& reader, WorldState& state) {
    std::int64_t world_total = 0;
    std::uint64_t settlement_count = 0;
    if (!reader.read_u64(settlement_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot coin section is truncated");
    }
    if (settlement_count != static_cast<std::uint64_t>(state.settlements.size())) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot settlement coin count does not match settlements");
    }
    std::unordered_map<SettlementId, Settlement*> settlement_by_id;
    for (auto& settlement : state.settlements) {
        settlement_by_id.emplace(settlement.id, &settlement);
    }
    SettlementId previous_settlement = 0;
    for (std::uint64_t i = 0; i < settlement_count; ++i) {
        SettlementId id = 0;
        std::int64_t coin = 0;
        if (!reader.read_u64(id) || !reader.read_i64(coin)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot settlement coin record is truncated");
        }
        if (id <= previous_settlement) {
            return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot settlement coin records must be in ascending id order");
        }
        previous_settlement = id;
        const auto it = settlement_by_id.find(id);
        if (it == settlement_by_id.end()) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot settlement coin references unknown settlement");
        }
        if (coin < 0) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement coin is negative");
        }
        if (!checked_add_i64(world_total, coin, world_total)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot total coin overflows");
        }
        it->second->coin = coin;
    }

    std::uint64_t household_count = 0;
    if (!reader.read_u64(household_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot household coin section is truncated");
    }
    if (household_count != static_cast<std::uint64_t>(state.households.size())) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot household coin count does not match households");
    }
    std::unordered_map<HouseholdId, Household*> household_by_id;
    for (auto& household : state.households) {
        household_by_id.emplace(household.id, &household);
    }
    HouseholdId previous_household = 0;
    for (std::uint64_t i = 0; i < household_count; ++i) {
        HouseholdId id = 0;
        std::int64_t coin = 0;
        if (!reader.read_u64(id) || !reader.read_i64(coin)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot household coin record is truncated");
        }
        if (id <= previous_household) {
            return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot household coin records must be in ascending id order");
        }
        previous_household = id;
        const auto it = household_by_id.find(id);
        if (it == household_by_id.end()) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot household coin references unknown household");
        }
        if (coin < 0) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot household coin is negative");
        }
        if (!checked_add_i64(world_total, coin, world_total)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot total coin overflows");
        }
        it->second->coin = coin;
    }
    return SnapshotLoadResult{.ok = true, .code = SnapshotErrorCode::None, .message = {}};
}

// Reads the schema v28 section, last in the payload: what each merchant last saw
// of settlement prices, one row per (merchant, settlement, resource) in strictly
// ascending order. Every row names a living merchant and a known settlement, and
// a price no lower than the floor nor higher than the ceiling of its resource.
[[nodiscard]] SnapshotLoadResult read_v28_merchant_memory_section(SnapshotReader& reader, WorldState& state) {
    std::uint64_t row_count = 0;
    if (!reader.read_u64(row_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot merchant memory section is truncated");
    }

    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(state.people.size());
    for (const auto& person : state.people) {
        person_by_id.emplace(person.id, &person);
    }
    std::unordered_set<SettlementId> settlement_ids;
    for (const auto& settlement : state.settlements) {
        settlement_ids.insert(settlement.id);
    }

    state.merchant_price_memory.clear();
    for (std::uint64_t i = 0; i < row_count; ++i) {
        MerchantPriceMemory row{};
        std::uint8_t resource_raw = 0;
        if (!reader.read_u64(row.merchant_id) || !reader.read_u64(row.settlement_id) || !reader.read_u8(resource_raw) ||
            !reader.read_i64(row.price_millicoin) || !reader.read_i64(row.seen_at.ticks)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot merchant memory row is truncated");
        }
        if (resource_raw > static_cast<std::uint8_t>(ResourceType::BasicTool)) {
            return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot merchant memory names an unknown resource");
        }
        row.resource = static_cast<ResourceType>(resource_raw);

        if (!state.merchant_price_memory.empty()) {
            const auto& previous = state.merchant_price_memory.back();
            if (!(std::tie(previous.merchant_id, previous.settlement_id, previous.resource) <
                  std::tie(row.merchant_id, row.settlement_id, row.resource))) {
                return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot merchant memory rows must be in strictly ascending order");
            }
        }
        const auto person_it = person_by_id.find(row.merchant_id);
        if (person_it == person_by_id.end() || !person_it->second->alive || person_it->second->occupation != Occupation::Merchant) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot merchant memory must belong to a living merchant");
        }
        if (!settlement_ids.contains(row.settlement_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot merchant memory references unknown settlement");
        }
        const auto base = base_price_millicoin(row.resource);
        if (row.price_millicoin < (base * kPriceFloorPermille) / kPricePermilleScale ||
            row.price_millicoin > (base * kPriceCeilingPermille) / kPricePermilleScale) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot merchant memory price is outside the price range");
        }
        if (row.seen_at.ticks < 0 || row.seen_at.ticks > state.time.ticks) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot merchant memory time is outside the simulated past");
        }
        state.merchant_price_memory.push_back(row);
    }

    // The world must be one the simulation could have produced. Rows come in
    // whole groups of five prices (one per resource) per merchant and settlement.
    std::unordered_map<PersonId, std::uint64_t> groups_per_merchant;
    for (std::size_t i = 0; i < state.merchant_price_memory.size();) {
        const auto& first = state.merchant_price_memory[i];
        std::size_t end = i;
        while (end < state.merchant_price_memory.size() &&
               state.merchant_price_memory[end].merchant_id == first.merchant_id &&
               state.merchant_price_memory[end].settlement_id == first.settlement_id) {
            ++end;
        }
        if (end - i != 5U) {
            return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot merchant memory must hold all five prices of each settlement it knows");
        }
        ++groups_per_merchant[first.merchant_id];
        i = end;
    }

    std::unordered_map<HouseholdId, SettlementId> settlement_of_household;
    for (const auto& household : state.households) {
        settlement_of_household.emplace(household.id, household.settlement_id);
    }
    std::unordered_map<SettlementId, bool> settlement_active;
    std::uint64_t active_settlement_count = 0;
    for (const auto& settlement : state.settlements) {
        settlement_active.emplace(settlement.id, settlement.active);
        active_settlement_count += settlement.active ? 1U : 0U;
    }
    std::unordered_map<SettlementId, std::uint64_t> merchants_per_settlement;
    for (const auto& person : state.people) {
        if (!person.alive || person.occupation != Occupation::Merchant) {
            continue;
        }
        const auto household_it = settlement_of_household.find(person.household_id);
        if (household_it == settlement_of_household.end() || !settlement_active[household_it->second]) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot merchant must live in a household of an active settlement");
        }
        if (++merchants_per_settlement[household_it->second] > kMaxMerchantsPerSettlement) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement has more merchants than the bound allows");
        }
        if (!groups_per_merchant.contains(person.id)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot merchant must remember the prices it knows");
        }
    }
    if (active_settlement_count < 2U && !merchants_per_settlement.empty()) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot has merchants in a world with fewer than two active settlements");
    }
    return SnapshotLoadResult{.ok = true, .code = SnapshotErrorCode::None, .message = {}};
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

[[nodiscard]] bool is_valid_memory_id(const MemoryId id) noexcept {
    return id > 0U;
}

[[nodiscard]] bool is_valid_knowledge_id(const KnowledgeId id) noexcept {
    return id > 0U;
}

[[nodiscard]] bool is_valid_communication_id(const CommunicationId id) noexcept {
    return id > 0U;
}

[[nodiscard]] bool is_valid_record_id(const RecordId id) noexcept {
    return id > 0U;
}

[[nodiscard]] bool is_valid_institution_id(const InstitutionId id) noexcept {
    return id > 0U;
}

[[nodiscard]] bool is_valid_cohort_id(const CohortId id) noexcept {
    return id > 0U;
}

struct DirectMemoryKey {
    PersonId owner_id{};
    EventId event_id{};

    [[nodiscard]] bool operator==(const DirectMemoryKey& other) const noexcept {
        return owner_id == other.owner_id && event_id == other.event_id;
    }
};

struct DirectMemoryKeyHash {
    [[nodiscard]] std::size_t operator()(const DirectMemoryKey& key) const noexcept {
        const auto mixed = (static_cast<std::uint64_t>(key.owner_id) * 0x9E3779B97F4A7C15ULL) ^
            (static_cast<std::uint64_t>(key.event_id) + 0xD6E8FEB86659FD93ULL);
        return static_cast<std::size_t>(mixed);
    }
};

struct KnowledgeSemanticKey {
    PersonId owner_id{};
    KnowledgeFactKind kind{KnowledgeFactKind::PersonLifeStatus};
    std::uint64_t primary_subject_id{};
    std::uint64_t secondary_subject_id{};

    [[nodiscard]] bool operator==(const KnowledgeSemanticKey& other) const noexcept {
        return owner_id == other.owner_id &&
            kind == other.kind &&
            primary_subject_id == other.primary_subject_id &&
            secondary_subject_id == other.secondary_subject_id;
    }
};

struct KnowledgeSemanticKeyHash {
    [[nodiscard]] std::size_t operator()(const KnowledgeSemanticKey& key) const noexcept {
        const auto mixed =
            (static_cast<std::uint64_t>(key.owner_id) * 0x9E3779B97F4A7C15ULL) ^
            (static_cast<std::uint64_t>(key.kind) * 0xD6E8FEB86659FD93ULL) ^
            (key.primary_subject_id * 0xA24BAED4963EE407ULL) ^
            (key.secondary_subject_id * 0x94D049BB133111EBULL);
        return static_cast<std::size_t>(mixed);
    }
};

struct RecordSemanticKey {
    SettlementId settlement_id{};
    EventId source_event_id{};

    [[nodiscard]] bool operator==(const RecordSemanticKey& other) const noexcept {
        return settlement_id == other.settlement_id && source_event_id == other.source_event_id;
    }
};

struct RecordSemanticKeyHash {
    [[nodiscard]] std::size_t operator()(const RecordSemanticKey& key) const noexcept {
        const auto mixed =
            (key.settlement_id * 0x9E3779B97F4A7C15ULL) ^
            (key.source_event_id * 0xD6E8FEB86659FD93ULL);
        return static_cast<std::size_t>(mixed);
    }
};

struct RecordAccessSemanticKey {
    RecordId record_id{};
    PersonId reader_id{};

    [[nodiscard]] bool operator==(const RecordAccessSemanticKey& other) const noexcept {
        return record_id == other.record_id && reader_id == other.reader_id;
    }
};

struct RecordAccessSemanticKeyHash {
    [[nodiscard]] std::size_t operator()(const RecordAccessSemanticKey& key) const noexcept {
        const auto mixed =
            (key.record_id * 0xA24BAED4963EE407ULL) ^
            (key.reader_id * 0x94D049BB133111EBULL);
        return static_cast<std::size_t>(mixed);
    }
};

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
    payload.write_u64(simulation.next_memory_id_);
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
        payload.write_u8(static_cast<std::uint8_t>(person->current_activity));
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
        payload.write_i64(region->position_x);
        payload.write_i64(region->position_z);
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
        payload.write_i64(settlement->position_x);
        payload.write_i64(settlement->position_z);
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
        if (event->type == EventType::Birth) {
            const auto* payload_data = std::get_if<BirthEventPayload>(&event->payload);
            if (payload_data == nullptr) {
                throw std::invalid_argument("Birth event payload mismatch");
            }
            payload.write_u64(payload_data->child_id);
            payload.write_u64(payload_data->mother_id);
            payload.write_bool(payload_data->father_id.has_value());
            if (payload_data->father_id.has_value()) {
                payload.write_u64(*payload_data->father_id);
            }
            payload.write_u64(payload_data->household_id);
        } else if (event->type == EventType::Death) {
            const auto* payload_data = std::get_if<DeathEventPayload>(&event->payload);
            if (payload_data == nullptr) {
                throw std::invalid_argument("Death event payload mismatch");
            }
            payload.write_u64(payload_data->person_id);
            payload.write_u64(payload_data->household_id);
        } else if (event->type == EventType::SettlementFounded) {
            const auto* payload_data = std::get_if<SettlementFoundedEventPayload>(&event->payload);
            if (payload_data == nullptr) {
                throw std::invalid_argument("SettlementFounded event payload mismatch");
            }
            payload.write_u64(payload_data->settlement_id);
            payload.write_u64(payload_data->region_id);
            payload.write_bool(payload_data->source_settlement_id.has_value());
            if (payload_data->source_settlement_id.has_value()) {
                payload.write_u64(*payload_data->source_settlement_id);
            }
        } else if (event->type == EventType::SettlementExtinct) {
            const auto* payload_data = std::get_if<SettlementExtinctEventPayload>(&event->payload);
            if (payload_data == nullptr) {
                throw std::invalid_argument("SettlementExtinct event payload mismatch");
            }
            payload.write_u64(payload_data->settlement_id);
            payload.write_u64(payload_data->region_id);
        } else if (event->type == EventType::DiscoveryMade) {
            const auto* payload_data = std::get_if<DiscoveryMadeEventPayload>(&event->payload);
            if (payload_data == nullptr) {
                throw std::invalid_argument("DiscoveryMade event payload mismatch");
            }
            payload.write_u32(payload_data->discovery_key);
            payload.write_u64(payload_data->discoverer_id);
            payload.write_u64(payload_data->settlement_id);
        } else if (event->type == EventType::InstitutionFounded) {
            const auto* payload_data = std::get_if<InstitutionFoundedEventPayload>(&event->payload);
            if (payload_data == nullptr) {
                throw std::invalid_argument("InstitutionFounded event payload mismatch");
            }
            payload.write_u64(payload_data->institution_id);
            payload.write_u8(static_cast<std::uint8_t>(payload_data->kind));
            payload.write_u64(payload_data->settlement_id);
            payload.write_u64(payload_data->founder_id);
        } else if (event->type == EventType::SettlementMerged) {
            const auto* payload_data = std::get_if<SettlementMergedEventPayload>(&event->payload);
            if (payload_data == nullptr) {
                throw std::invalid_argument("SettlementMerged event payload mismatch");
            }
            payload.write_u64(payload_data->survivor_settlement_id);
            payload.write_u64(payload_data->absorbed_settlement_id);
            payload.write_u64(payload_data->region_id);
        } else if (event->type == EventType::OccupationChanged) {
            const auto* payload_data = std::get_if<OccupationChangedEventPayload>(&event->payload);
            if (payload_data == nullptr) {
                throw std::invalid_argument("OccupationChanged event payload mismatch");
            }
            payload.write_u64(payload_data->person_id);
            payload.write_u8(static_cast<std::uint8_t>(payload_data->from));
            payload.write_u8(static_cast<std::uint8_t>(payload_data->to));
        } else if (event->type == EventType::HouseholdRelocated) {
            const auto* payload_data = std::get_if<HouseholdRelocatedEventPayload>(&event->payload);
            if (payload_data == nullptr) {
                throw std::invalid_argument("HouseholdRelocated event payload mismatch");
            }
            payload.write_u64(payload_data->household_id);
            payload.write_u64(payload_data->from_settlement_id);
            payload.write_u64(payload_data->to_settlement_id);
            payload.write_u64(static_cast<std::uint64_t>(payload_data->member_ids.size()));
            for (const auto member_id : payload_data->member_ids) {
                payload.write_u64(member_id);
            }
        } else {
            throw std::invalid_argument("Unknown event type");
        }
    }

    std::vector<const AgentMemory*> memory_order;
    memory_order.reserve(simulation.state_.agent_memories.size());
    for (const auto& memory : simulation.state_.agent_memories) {
        memory_order.push_back(&memory);
    }
    std::sort(memory_order.begin(), memory_order.end(), [](const AgentMemory* lhs, const AgentMemory* rhs) {
        return lhs->id < rhs->id;
    });

    payload.write_u64(static_cast<std::uint64_t>(memory_order.size()));
    for (const auto* memory : memory_order) {
        payload.write_u64(memory->id);
        payload.write_u64(memory->owner_id);
        payload.write_i64(memory->acquired_at.ticks);
        payload.write_u8(static_cast<std::uint8_t>(memory->source));
        payload.write_u64(memory->source_event_id);
        payload.write_u32(memory->confidence_per_10k);
    }

    std::vector<const AgentKnowledge*> knowledge_order;
    knowledge_order.reserve(simulation.state_.agent_knowledge.size());
    for (const auto& knowledge : simulation.state_.agent_knowledge) {
        knowledge_order.push_back(&knowledge);
    }
    std::sort(knowledge_order.begin(), knowledge_order.end(), [](const AgentKnowledge* lhs, const AgentKnowledge* rhs) {
        return lhs->id < rhs->id;
    });

    payload.write_u64(static_cast<std::uint64_t>(knowledge_order.size()));
    for (const auto* knowledge : knowledge_order) {
        payload.write_u64(knowledge->id);
        payload.write_u64(knowledge->owner_id);
        payload.write_i64(knowledge->acquired_at.ticks);
        payload.write_i64(knowledge->updated_at.ticks);
        payload.write_u8(static_cast<std::uint8_t>(knowledge->source));
        payload.write_u64(knowledge->source_memory_id);

        if (const auto* life = std::get_if<PersonLifeStatusKnowledgeFact>(&knowledge->fact); life != nullptr) {
            payload.write_u8(static_cast<std::uint8_t>(KnowledgeFactKind::PersonLifeStatus));
            payload.write_u64(life->person_id);
            payload.write_u8(static_cast<std::uint8_t>(life->status));
        } else if (const auto* household = std::get_if<PersonHouseholdKnowledgeFact>(&knowledge->fact); household != nullptr) {
            payload.write_u8(static_cast<std::uint8_t>(KnowledgeFactKind::PersonHousehold));
            payload.write_u64(household->person_id);
            payload.write_u64(household->household_id);
        } else if (const auto* settlement = std::get_if<SettlementStatusKnowledgeFact>(&knowledge->fact); settlement != nullptr) {
            payload.write_u8(static_cast<std::uint8_t>(KnowledgeFactKind::SettlementStatus));
            payload.write_u64(settlement->settlement_id);
            payload.write_u64(settlement->region_id);
            payload.write_bool(settlement->active);
        } else if (const auto* discovery = std::get_if<BasicDiscoveryKnowledgeFact>(&knowledge->fact); discovery != nullptr) {
            payload.write_u8(static_cast<std::uint8_t>(KnowledgeFactKind::BasicDiscovery));
            payload.write_u32(discovery->discovery_key);
        } else {
            throw std::invalid_argument("Knowledge fact payload mismatch");
        }

        payload.write_u32(knowledge->confidence_per_10k);
    }
    payload.write_u64(simulation.next_knowledge_id_);

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

    std::vector<const KnowledgeTransmission*> transmission_order;
    transmission_order.reserve(simulation.state_.knowledge_transmissions.size());
    for (const auto& transmission : simulation.state_.knowledge_transmissions) {
        transmission_order.push_back(&transmission);
    }
    std::sort(transmission_order.begin(), transmission_order.end(), [](const KnowledgeTransmission* lhs, const KnowledgeTransmission* rhs) {
        return lhs->id < rhs->id;
    });

    payload.write_u64(static_cast<std::uint64_t>(transmission_order.size()));
    for (const auto* transmission : transmission_order) {
        payload.write_u64(transmission->id);
        payload.write_u64(transmission->speaker_id);
        payload.write_u64(transmission->recipient_id);
        payload.write_u64(transmission->speaker_household_id);
        payload.write_u64(transmission->recipient_household_id);
        payload.write_u64(transmission->speaker_settlement_id);
        payload.write_u64(transmission->recipient_settlement_id);
        payload.write_i64(transmission->transmitted_at.ticks);
        payload.write_u8(static_cast<std::uint8_t>(transmission->method));
        payload.write_u64(transmission->speaker_memory_id);
        payload.write_u64(transmission->recipient_memory_id);
        payload.write_u32(transmission->confidence_per_10k);
    }

    std::vector<const KnowledgeRecord*> record_order;
    record_order.reserve(simulation.state_.knowledge_records.size());
    for (const auto& record : simulation.state_.knowledge_records) {
        record_order.push_back(&record);
    }
    std::sort(record_order.begin(), record_order.end(), [](const KnowledgeRecord* lhs, const KnowledgeRecord* rhs) {
        return lhs->id < rhs->id;
    });

    payload.write_u64(static_cast<std::uint64_t>(record_order.size()));
    for (const auto* record : record_order) {
        payload.write_u64(record->id);
        payload.write_u64(record->settlement_id);
        payload.write_u64(record->author_id);
        payload.write_u64(record->author_household_id);
        payload.write_i64(record->recorded_at.ticks);
        payload.write_u64(record->source_memory_id);
        payload.write_u64(record->source_event_id);
        payload.write_u32(record->confidence_per_10k);
    }

    std::vector<const KnowledgeRecordAccess*> access_order;
    access_order.reserve(simulation.state_.knowledge_record_accesses.size());
    for (const auto& access : simulation.state_.knowledge_record_accesses) {
        access_order.push_back(&access);
    }
    std::sort(access_order.begin(), access_order.end(), [](const KnowledgeRecordAccess* lhs, const KnowledgeRecordAccess* rhs) {
        if (lhs->record_id != rhs->record_id) {
            return lhs->record_id < rhs->record_id;
        }
        return lhs->reader_id < rhs->reader_id;
    });

    payload.write_u64(static_cast<std::uint64_t>(access_order.size()));
    for (const auto* access : access_order) {
        payload.write_u64(access->record_id);
        payload.write_u64(access->reader_id);
        payload.write_u64(access->reader_household_id);
        payload.write_u64(access->reader_settlement_id);
        payload.write_u64(access->reader_memory_id);
        payload.write_i64(access->read_at.ticks);
        payload.write_u32(access->confidence_per_10k);
    }

    std::vector<const Institution*> institution_order;
    institution_order.reserve(simulation.state_.institutions.size());
    for (const auto& institution : simulation.state_.institutions) {
        institution_order.push_back(&institution);
    }
    std::sort(institution_order.begin(), institution_order.end(), [](const Institution* lhs, const Institution* rhs) {
        return lhs->id < rhs->id;
    });

    payload.write_u64(static_cast<std::uint64_t>(institution_order.size()));
    for (const auto* institution : institution_order) {
        payload.write_u64(institution->id);
        payload.write_u8(static_cast<std::uint8_t>(institution->kind));
        payload.write_u64(institution->settlement_id);
        payload.write_u64(institution->founder_id);
        payload.write_i64(institution->founded_at.ticks);
        payload.write_bool(institution->active);

        std::vector<PersonId> members = institution->member_ids;
        std::sort(members.begin(), members.end());
        members.erase(std::unique(members.begin(), members.end()), members.end());
        payload.write_u64(static_cast<std::uint64_t>(members.size()));
        for (const auto member_id : members) {
            payload.write_u64(member_id);
        }

        std::vector<RecordId> preserved_records = institution->preserved_record_ids;
        std::sort(preserved_records.begin(), preserved_records.end());
        preserved_records.erase(std::unique(preserved_records.begin(), preserved_records.end()), preserved_records.end());
        payload.write_u64(static_cast<std::uint64_t>(preserved_records.size()));
        for (const auto record_id : preserved_records) {
            payload.write_u64(record_id);
        }

        std::vector<std::uint32_t> preserved_discoveries = institution->preserved_discovery_keys;
        std::sort(preserved_discoveries.begin(), preserved_discoveries.end());
        preserved_discoveries.erase(std::unique(preserved_discoveries.begin(), preserved_discoveries.end()), preserved_discoveries.end());
        payload.write_u64(static_cast<std::uint64_t>(preserved_discoveries.size()));
        for (const auto discovery_key : preserved_discoveries) {
            payload.write_u32(discovery_key);
        }
    }

    payload.write_u64(simulation.next_communication_id_);
    payload.write_u64(simulation.next_record_id_);
    payload.write_u64(simulation.next_institution_id_);
    payload.write_u64(simulation.next_building_id_);

    payload.write_u64(static_cast<std::uint64_t>(household_order.size()));
    for (const auto* household : household_order) {
        payload.write_u64(household->id);
        payload.write_i64(household->owned_materials.wood.value);
        payload.write_i64(household->owned_materials.stone.value);
        payload.write_i64(household->owned_materials.timber.value);
        payload.write_i64(household->owned_materials.basic_tool.value);
        payload.write_u32(household->productive_access_share_points);
        payload.write_u32(household->commons_access_share_points);
        payload.write_i64(household->cumulative_food_transferred_in);
        payload.write_i64(household->cumulative_food_transferred_out);
        payload.write_i64(household->cumulative_material_transferred_in);
        payload.write_i64(household->cumulative_material_transferred_out);
        payload.write_i64(household->derived_wealth_points);
        payload.write_u32(static_cast<std::uint32_t>(household->settlement_offset_x));
        payload.write_u32(static_cast<std::uint32_t>(household->settlement_offset_z));
    }

    payload.write_u64(static_cast<std::uint64_t>(settlement_order.size()));
    for (const auto* settlement : settlement_order) {
        payload.write_u64(settlement->id);
        payload.write_i64(settlement->commons_food.value);
        payload.write_i64(settlement->last_transfer_food_volume);
        payload.write_i64(settlement->last_exchange_volume);
        payload.write_u32(settlement->economic_viability_streak);
        payload.write_u32(settlement->economic_pressure_streak);
        payload.write_i64(settlement->dwelling_capacity_bonus);
        payload.write_i64(settlement->food_store_capacity_bonus);
        payload.write_i64(settlement->workshop_capacity_bonus);
        payload.write_i64(settlement->last_median_household_wealth);
        payload.write_i64(settlement->last_min_household_wealth);
        payload.write_i64(settlement->last_max_household_wealth);
        payload.write_u64(settlement->last_inequality_numerator);
        payload.write_u64(settlement->last_inequality_denominator);
        payload.write_u64(settlement->population);
        payload.write_u8(static_cast<std::uint8_t>(settlement->tier));
        payload.write_u8(static_cast<std::uint8_t>(settlement->pending_tier));
        payload.write_u32(settlement->tier_transition_streak);
    }

    std::vector<const Building*> building_order;
    building_order.reserve(simulation.state_.buildings.size());
    for (const auto& building : simulation.state_.buildings) {
        building_order.push_back(&building);
    }
    std::sort(building_order.begin(), building_order.end(), [](const Building* lhs, const Building* rhs) {
        return lhs->id < rhs->id;
    });

    payload.write_u64(static_cast<std::uint64_t>(building_order.size()));
    for (const auto* building : building_order) {
        payload.write_u64(building->id);
        payload.write_u8(static_cast<std::uint8_t>(building->type));
        payload.write_u64(building->settlement_id);
        payload.write_bool(building->owner_household_id.has_value());
        if (building->owner_household_id.has_value()) {
            payload.write_u64(*building->owner_household_id);
        }
        payload.write_bool(building->settlement_commons_owned);
        payload.write_u8(static_cast<std::uint8_t>(building->state));
        payload.write_i64(building->timber_required);
        payload.write_i64(building->stone_required);
        payload.write_i64(building->labour_required);
        payload.write_i64(building->timber_committed);
        payload.write_i64(building->stone_committed);
        payload.write_i64(building->labour_completed);
        payload.write_i64(building->started_at.ticks);
        payload.write_bool(building->completed_at.has_value());
        if (building->completed_at.has_value()) {
            payload.write_i64(building->completed_at->ticks);
        }
    }

    payload.write_u64(simulation.next_cohort_id_);
    payload.write_u8(static_cast<std::uint8_t>(simulation.state_.detail_level));

    std::vector<const RegionLodState*> region_lod_order;
    region_lod_order.reserve(simulation.state_.region_lod_states.size());
    for (const auto& region_lod : simulation.state_.region_lod_states) {
        region_lod_order.push_back(&region_lod);
    }
    std::sort(region_lod_order.begin(), region_lod_order.end(), [](const RegionLodState* lhs, const RegionLodState* rhs) {
        if (lhs->region_id != rhs->region_id) {
            return lhs->region_id < rhs->region_id;
        }
        return static_cast<std::uint8_t>(lhs->detail_level) < static_cast<std::uint8_t>(rhs->detail_level);
    });

    payload.write_u64(static_cast<std::uint64_t>(region_lod_order.size()));
    for (const auto* region_lod : region_lod_order) {
        payload.write_u64(region_lod->region_id);
        payload.write_u8(static_cast<std::uint8_t>(region_lod->detail_level));
    }

    std::vector<const PopulationCohort*> cohort_order;
    cohort_order.reserve(simulation.state_.population_cohorts.size());
    for (const auto& cohort : simulation.state_.population_cohorts) {
        cohort_order.push_back(&cohort);
    }
    std::sort(cohort_order.begin(), cohort_order.end(), [](const PopulationCohort* lhs, const PopulationCohort* rhs) {
        if (lhs->region_id != rhs->region_id) {
            return lhs->region_id < rhs->region_id;
        }
        if (lhs->settlement_id != rhs->settlement_id) {
            return lhs->settlement_id < rhs->settlement_id;
        }
        return lhs->id < rhs->id;
    });

    payload.write_u64(static_cast<std::uint64_t>(cohort_order.size()));
    for (const auto* cohort : cohort_order) {
        payload.write_u64(cohort->id);
        payload.write_u64(cohort->region_id);
        payload.write_u64(cohort->settlement_id);
        payload.write_u8(static_cast<std::uint8_t>(cohort->age_band));
        payload.write_u8(static_cast<std::uint8_t>(cohort->sex));
        payload.write_u8(static_cast<std::uint8_t>(cohort->occupation_group));
        payload.write_u8(static_cast<std::uint8_t>(cohort->health_band));
        payload.write_u8(static_cast<std::uint8_t>(cohort->wealth_band));
        payload.write_u32(cohort->knowledge_profile_id);
        payload.write_u64(cohort->population);
    }

    payload.write_u64(static_cast<std::uint64_t>(people_order.size()));
    for (const auto* person : people_order) {
        payload.write_u64(person->id);
        payload.write_bool(person->materialized_from_cohort);
        payload.write_bool(person->source_cohort_id.has_value());
        if (person->source_cohort_id.has_value()) {
            payload.write_u64(*person->source_cohort_id);
        }
    }

    std::vector<const SettlementAggregateState*> settlement_aggregate_order;
    settlement_aggregate_order.reserve(simulation.state_.settlement_aggregates.size());
    for (const auto& aggregate : simulation.state_.settlement_aggregates) {
        settlement_aggregate_order.push_back(&aggregate);
    }
    std::sort(settlement_aggregate_order.begin(), settlement_aggregate_order.end(), [](const SettlementAggregateState* lhs, const SettlementAggregateState* rhs) {
        return lhs->settlement_id < rhs->settlement_id;
    });

    payload.write_u64(static_cast<std::uint64_t>(settlement_aggregate_order.size()));
    for (const auto* aggregate : settlement_aggregate_order) {
        payload.write_u64(aggregate->settlement_id);
        payload.write_u64(aggregate->region_id);
        payload.write_u64(aggregate->household_count);
        payload.write_u64(aggregate->small_household_count);
        payload.write_u64(aggregate->medium_household_count);
        payload.write_u64(aggregate->large_household_count);
        payload.write_i64(aggregate->food_inventory);
        payload.write_i64(aggregate->food_produced_total);
        payload.write_i64(aggregate->food_consumed_total);
        payload.write_i64(aggregate->unmet_food_total);
        payload.write_i64(aggregate->labour_capacity);
        payload.write_i64(aggregate->labour_demand);
        payload.write_i64(aggregate->labour_allocated);
        payload.write_u64(aggregate->labour_low_household_count);
        payload.write_u64(aggregate->labour_medium_household_count);
        payload.write_u64(aggregate->labour_high_household_count);
        payload.write_u64(aggregate->wealth_low_household_count);
        payload.write_u64(aggregate->wealth_medium_household_count);
        payload.write_u64(aggregate->wealth_high_household_count);
        payload.write_u64(aggregate->food_buffer_low_household_count);
        payload.write_u64(aggregate->food_buffer_medium_household_count);
        payload.write_u64(aggregate->food_buffer_high_household_count);
        payload.write_i64(aggregate->wealth_points_total);
        payload.write_i64(aggregate->productive_access_points);
        payload.write_i64(aggregate->required_food_year);
        payload.write_i64(aggregate->unmet_food_year);
    }

    // Aggregate demography totals are written last so earlier field offsets stay
    // stable across schema revisions.
    // Schema v25 section: canonical building placement, then people whose next
    // occupation reassignment is recorded silently. Placed before the aggregate
    // totals so the totals remain the final 16 bytes.
    std::vector<const Building*> placement_order;
    placement_order.reserve(simulation.state_.buildings.size());
    for (const auto& building : simulation.state_.buildings) {
        placement_order.push_back(&building);
    }
    std::sort(placement_order.begin(), placement_order.end(), [](const Building* lhs, const Building* rhs) {
        return lhs->id < rhs->id;
    });
    payload.write_u64(static_cast<std::uint64_t>(placement_order.size()));
    for (const auto* building : placement_order) {
        payload.write_u64(building->id);
        payload.write_i64(building->position_x);
        payload.write_i64(building->position_z);
        payload.write_u16(building->facing_degrees);
    }

    std::vector<PersonId> resync_pending_ids;
    for (const auto& person : simulation.state_.people) {
        if (person.occupation_history_resync_pending) {
            resync_pending_ids.push_back(person.id);
        }
    }
    std::sort(resync_pending_ids.begin(), resync_pending_ids.end());
    payload.write_u64(static_cast<std::uint64_t>(resync_pending_ids.size()));
    for (const auto person_id : resync_pending_ids) {
        payload.write_u64(person_id);
    }

    // Schema v26 section: days each settlement has spent below the viable
    // size, written in settlement id order just before the totals.
    payload.write_u64(static_cast<std::uint64_t>(settlement_order.size()));
    for (const auto* settlement : settlement_order) {
        payload.write_u64(settlement->id);
        payload.write_u32(settlement->days_below_viable_size);
        payload.write_u32(settlement->days_under_food_strain);
    }

    payload.write_u64(simulation.state_.aggregate_births_total);
    payload.write_u64(simulation.state_.aggregate_deaths_total);

    // Schema v27 section, last in the payload: coin on every settlement commons
    // then every household, in ascending id order.
    payload.write_u64(static_cast<std::uint64_t>(settlement_order.size()));
    for (const auto* settlement : settlement_order) {
        payload.write_u64(settlement->id);
        payload.write_i64(settlement->coin);
    }
    payload.write_u64(static_cast<std::uint64_t>(household_order.size()));
    for (const auto* household : household_order) {
        payload.write_u64(household->id);
        payload.write_i64(household->coin);
    }

    // Schema v28 section, last in the payload: what each merchant last saw of
    // settlement prices, in the state's (merchant, settlement, resource) order.
    payload.write_u64(static_cast<std::uint64_t>(simulation.state_.merchant_price_memory.size()));
    for (const auto& memory : simulation.state_.merchant_price_memory) {
        payload.write_u64(memory.merchant_id);
        payload.write_u64(memory.settlement_id);
        payload.write_u8(static_cast<std::uint8_t>(memory.resource));
        payload.write_i64(memory.price_millicoin);
        payload.write_i64(memory.seen_at.ticks);
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

    if (version < kMinimumMigratableSchemaVersion || version > kSchemaVersion) {
        return load_error(SnapshotErrorCode::UnsupportedVersion, "Snapshot schema version is unsupported");
    }
    const bool is_v24_migration = version == kMinimumMigratableSchemaVersion;
    const bool has_viability_section = version >= kViabilitySectionSchemaVersion;
    const bool has_coin_section = version >= kCoinSectionSchemaVersion;
    const bool has_merchant_memory_section = version >= kMerchantMemorySchemaVersion;

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
    std::uint64_t next_memory_id = 0;
    std::uint64_t next_knowledge_id = 0;
    std::uint64_t next_communication_id = 0;
    std::uint64_t next_record_id = 0;
    std::uint64_t next_institution_id = 0;
    std::uint64_t next_building_id = 0;
    std::uint64_t next_cohort_id = 0;
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
        !payload_reader.read_u64(next_memory_id) ||
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

        std::uint8_t current_activity_raw = 0;
        if (!payload_reader.read_u32(nutrition_points) ||
            !payload_reader.read_u32(health_points) ||
            !payload_reader.read_u32(person.consecutive_undernourished_days) ||
            !payload_reader.read_u8(occupation_raw) ||
            !payload_reader.read_u8(current_activity_raw)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot person health fields are truncated");
        }

        if (nutrition_points > 1'000U || health_points > 1'000U) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot person health fields are out of range");
        }

        person.nutrition_points = static_cast<std::int32_t>(nutrition_points);
        person.health_points = static_cast<std::int32_t>(health_points);

        // Merchants exist only from schema v28; an older file naming one is invalid.
        const auto highest_occupation = has_merchant_memory_section ? Occupation::Merchant : Occupation::CraftWorker;
        if (occupation_raw > static_cast<std::uint8_t>(highest_occupation)) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot person occupation enum is invalid");
        }
        person.occupation = static_cast<Occupation>(occupation_raw);

        if (current_activity_raw > static_cast<std::uint8_t>(ActivityKind::Idle)) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot person current_activity enum is invalid");
        }
        person.current_activity = static_cast<ActivityKind>(current_activity_raw);

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

    if (region_count > payload_reader.remaining() / 80U) {
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
            !payload_reader.read_i64(region.position_x) ||
            !payload_reader.read_i64(region.position_z) ||
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

        if (region.position_x < -kMaxSnapshotPositionMagnitude || region.position_x > kMaxSnapshotPositionMagnitude ||
            region.position_z < -kMaxSnapshotPositionMagnitude || region.position_z > kMaxSnapshotPositionMagnitude) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot region position is out of range");
        }

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

    if (settlement_count > payload_reader.remaining() / 37U) {
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
            !payload_reader.read_i64(settlement.position_x) ||
            !payload_reader.read_i64(settlement.position_z) ||
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
        if (settlement.position_x < -kMaxSnapshotPositionMagnitude || settlement.position_x > kMaxSnapshotPositionMagnitude ||
            settlement.position_z < -kMaxSnapshotPositionMagnitude || settlement.position_z > kMaxSnapshotPositionMagnitude) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement position is out of range");
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

    if (event_count > payload_reader.remaining() / 26U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot event section has invalid length");
    }

    restored_state.event_log.clear();
    restored_state.event_log.reserve(static_cast<std::size_t>(event_count));
    std::unordered_set<EventId> event_ids;

    for (std::uint64_t i = 0; i < event_count; ++i) {
        SimulationEvent event{};
        std::uint8_t type_raw = 0;

        if (!payload_reader.read_u64(event.id) ||
            !payload_reader.read_i64(event.timestamp.ticks) ||
            !payload_reader.read_u8(type_raw)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot event record is truncated");
        }

        const auto max_event_type = is_v24_migration
            ? static_cast<std::uint8_t>(EventType::SettlementMerged)
            : static_cast<std::uint8_t>(EventType::HouseholdRelocated);
        if (type_raw > max_event_type) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot event type is invalid");
        }
        event.type = static_cast<EventType>(type_raw);

        if (event.type == EventType::Birth) {
            BirthEventPayload payload_data{};
            bool has_father = false;
            if (!payload_reader.read_u64(payload_data.child_id) ||
                !payload_reader.read_u64(payload_data.mother_id) ||
                !payload_reader.read_bool(has_father)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot birth event payload is truncated");
            }
            if (has_father) {
                PersonId father_id = 0;
                if (!payload_reader.read_u64(father_id)) {
                    return load_error(SnapshotErrorCode::TruncatedData, "Snapshot birth event father id is truncated");
                }
                payload_data.father_id = father_id;
            }
            if (!payload_reader.read_u64(payload_data.household_id)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot birth event household id is truncated");
            }
            event.payload = payload_data;
        } else if (event.type == EventType::Death) {
            DeathEventPayload payload_data{};
            if (!payload_reader.read_u64(payload_data.person_id) ||
                !payload_reader.read_u64(payload_data.household_id)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot death event payload is truncated");
            }
            event.payload = payload_data;
        } else if (event.type == EventType::SettlementFounded) {
            SettlementFoundedEventPayload payload_data{};
            bool has_source = false;
            if (!payload_reader.read_u64(payload_data.settlement_id) ||
                !payload_reader.read_u64(payload_data.region_id) ||
                !payload_reader.read_bool(has_source)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot founded event payload is truncated");
            }
            if (has_source) {
                SettlementId source_settlement_id = 0;
                if (!payload_reader.read_u64(source_settlement_id)) {
                    return load_error(SnapshotErrorCode::TruncatedData, "Snapshot founded event source id is truncated");
                }
                payload_data.source_settlement_id = source_settlement_id;
            }
            event.payload = payload_data;
        } else if (event.type == EventType::SettlementExtinct) {
            SettlementExtinctEventPayload payload_data{};
            if (!payload_reader.read_u64(payload_data.settlement_id) ||
                !payload_reader.read_u64(payload_data.region_id)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot extinct event payload is truncated");
            }
            event.payload = payload_data;
        } else if (event.type == EventType::DiscoveryMade) {
            DiscoveryMadeEventPayload payload_data{};
            if (!payload_reader.read_u32(payload_data.discovery_key) ||
                !payload_reader.read_u64(payload_data.discoverer_id) ||
                !payload_reader.read_u64(payload_data.settlement_id)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot discovery event payload is truncated");
            }
            event.payload = payload_data;
        } else if (event.type == EventType::InstitutionFounded) {
            InstitutionFoundedEventPayload payload_data{};
            std::uint8_t kind_raw = 0;
            if (!payload_reader.read_u64(payload_data.institution_id) ||
                !payload_reader.read_u8(kind_raw) ||
                !payload_reader.read_u64(payload_data.settlement_id) ||
                !payload_reader.read_u64(payload_data.founder_id)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot institution founded event payload is truncated");
            }
            if (kind_raw > static_cast<std::uint8_t>(InstitutionKind::KnowledgeKeeper)) {
                return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot institution founded event kind is invalid");
            }
            payload_data.kind = static_cast<InstitutionKind>(kind_raw);
            event.payload = payload_data;
        } else if (event.type == EventType::SettlementMerged) {
            SettlementMergedEventPayload payload_data{};
            if (!payload_reader.read_u64(payload_data.survivor_settlement_id) ||
                !payload_reader.read_u64(payload_data.absorbed_settlement_id) ||
                !payload_reader.read_u64(payload_data.region_id)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot settlement merged event payload is truncated");
            }
            event.payload = payload_data;
        } else if (event.type == EventType::OccupationChanged) {
            OccupationChangedEventPayload payload_data{};
            std::uint8_t from_raw = 0;
            std::uint8_t to_raw = 0;
            if (!payload_reader.read_u64(payload_data.person_id) ||
                !payload_reader.read_u8(from_raw) ||
                !payload_reader.read_u8(to_raw)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot occupation changed event payload is truncated");
            }
            const auto highest_event_occupation = has_merchant_memory_section ? Occupation::Merchant : Occupation::CraftWorker;
            if (from_raw > static_cast<std::uint8_t>(highest_event_occupation) ||
                to_raw > static_cast<std::uint8_t>(highest_event_occupation)) {
                return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot occupation changed event occupation is invalid");
            }
            payload_data.from = static_cast<Occupation>(from_raw);
            payload_data.to = static_cast<Occupation>(to_raw);
            event.payload = payload_data;
        } else if (event.type == EventType::HouseholdRelocated) {
            HouseholdRelocatedEventPayload payload_data{};
            std::uint64_t member_count = 0;
            if (!payload_reader.read_u64(payload_data.household_id) ||
                !payload_reader.read_u64(payload_data.from_settlement_id) ||
                !payload_reader.read_u64(payload_data.to_settlement_id) ||
                !payload_reader.read_u64(member_count)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot household relocated event payload is truncated");
            }
            if (member_count > payload_reader.remaining() / 8U) {
                return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot household relocated member list has invalid length");
            }
            payload_data.member_ids.reserve(static_cast<std::size_t>(member_count));
            for (std::uint64_t member = 0; member < member_count; ++member) {
                PersonId member_id = 0;
                if (!payload_reader.read_u64(member_id)) {
                    return load_error(SnapshotErrorCode::TruncatedData, "Snapshot household relocated member id is truncated");
                }
                payload_data.member_ids.push_back(member_id);
            }
            event.payload = payload_data;
        }

        if (!is_valid_event_id(event.id)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot event id must be non-zero");
        }

        if (!event_ids.insert(event.id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot contains duplicate event ids");
        }

        restored_state.event_log.push_back(event);
    }

    std::uint64_t memory_count = 0;
    if (!payload_reader.read_u64(memory_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot memory section is truncated");
    }

    if (memory_count > payload_reader.remaining() / 37U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot memory section has invalid length");
    }

    restored_state.agent_memories.clear();
    restored_state.agent_memories.reserve(static_cast<std::size_t>(memory_count));
    std::unordered_set<MemoryId> memory_ids;
    std::unordered_set<DirectMemoryKey, DirectMemoryKeyHash> direct_memory_keys;

    for (std::uint64_t i = 0; i < memory_count; ++i) {
        AgentMemory memory{};
        std::uint8_t source_raw = 0;

        if (!payload_reader.read_u64(memory.id) ||
            !payload_reader.read_u64(memory.owner_id) ||
            !payload_reader.read_i64(memory.acquired_at.ticks) ||
            !payload_reader.read_u8(source_raw) ||
            !payload_reader.read_u64(memory.source_event_id) ||
            !payload_reader.read_u32(memory.confidence_per_10k)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot memory record is truncated");
        }

        if (!is_valid_memory_id(memory.id)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot memory id must be non-zero");
        }
        if (!is_valid_person_id(memory.owner_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot memory owner id must be non-zero");
        }
        if (!is_valid_event_id(memory.source_event_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot memory source event id must be non-zero");
        }
        if (source_raw > static_cast<std::uint8_t>(MemorySource::Record)) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot memory source enum is invalid");
        }
        memory.source = static_cast<MemorySource>(source_raw);

        if (!memory_ids.insert(memory.id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot contains duplicate memory ids");
        }
        if (memory.acquired_at.ticks < 0) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot memory acquisition time must be non-negative");
        }
        if (memory.acquired_at.ticks > restored_state.time.ticks) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot memory acquisition time exceeds simulation time");
        }
        if (memory.confidence_per_10k > 10'000U) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot memory confidence is out of range");
        }
        if (memory.source == MemorySource::DirectObservation) {
            const auto inserted = direct_memory_keys.insert(DirectMemoryKey{
                .owner_id = memory.owner_id,
                .event_id = memory.source_event_id,
            });
            if (!inserted.second) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot duplicate direct memory for owner/event");
            }
        }

        restored_state.agent_memories.push_back(memory);
    }

    std::uint64_t knowledge_count = 0;
    if (!payload_reader.read_u64(knowledge_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot knowledge section is truncated");
    }

    if (knowledge_count > payload_reader.remaining() / 55U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot knowledge section has invalid length");
    }

    restored_state.agent_knowledge.clear();
    restored_state.agent_knowledge.reserve(static_cast<std::size_t>(knowledge_count));
    std::unordered_set<KnowledgeId> knowledge_ids;

    for (std::uint64_t i = 0; i < knowledge_count; ++i) {
        AgentKnowledge knowledge{};
        std::uint8_t source_raw = 0;
        std::uint8_t fact_kind_raw = 0;

        if (!payload_reader.read_u64(knowledge.id) ||
            !payload_reader.read_u64(knowledge.owner_id) ||
            !payload_reader.read_i64(knowledge.acquired_at.ticks) ||
            !payload_reader.read_i64(knowledge.updated_at.ticks) ||
            !payload_reader.read_u8(source_raw) ||
            !payload_reader.read_u64(knowledge.source_memory_id) ||
            !payload_reader.read_u8(fact_kind_raw)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot knowledge record is truncated");
        }

        if (!is_valid_knowledge_id(knowledge.id)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot knowledge id must be non-zero");
        }
        if (!is_valid_person_id(knowledge.owner_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot knowledge owner id must be non-zero");
        }
        if (!is_valid_memory_id(knowledge.source_memory_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot knowledge source memory id must be non-zero");
        }
        if (source_raw > static_cast<std::uint8_t>(KnowledgeSource::MemoryDerived)) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot knowledge source enum is invalid");
        }
        knowledge.source = static_cast<KnowledgeSource>(source_raw);

        if (!knowledge_ids.insert(knowledge.id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot contains duplicate knowledge ids");
        }

        if (fact_kind_raw == static_cast<std::uint8_t>(KnowledgeFactKind::PersonLifeStatus)) {
            PersonLifeStatusKnowledgeFact fact{};
            std::uint8_t status_raw = 0;
            if (!payload_reader.read_u64(fact.person_id) || !payload_reader.read_u8(status_raw)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot person-life knowledge payload is truncated");
            }
            if (status_raw > static_cast<std::uint8_t>(KnownLifeStatus::Dead)) {
                return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot person-life status enum is invalid");
            }
            fact.status = static_cast<KnownLifeStatus>(status_raw);
            knowledge.fact = fact;
        } else if (fact_kind_raw == static_cast<std::uint8_t>(KnowledgeFactKind::PersonHousehold)) {
            PersonHouseholdKnowledgeFact fact{};
            if (!payload_reader.read_u64(fact.person_id) || !payload_reader.read_u64(fact.household_id)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot person-household knowledge payload is truncated");
            }
            knowledge.fact = fact;
        } else if (fact_kind_raw == static_cast<std::uint8_t>(KnowledgeFactKind::SettlementStatus)) {
            SettlementStatusKnowledgeFact fact{};
            if (!payload_reader.read_u64(fact.settlement_id) ||
                !payload_reader.read_u64(fact.region_id) ||
                !payload_reader.read_bool(fact.active)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot settlement-status knowledge payload is truncated");
            }
            knowledge.fact = fact;
        } else if (fact_kind_raw == static_cast<std::uint8_t>(KnowledgeFactKind::BasicDiscovery)) {
            BasicDiscoveryKnowledgeFact fact{};
            if (!payload_reader.read_u32(fact.discovery_key)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot basic-discovery knowledge payload is truncated");
            }
            knowledge.fact = fact;
        } else {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot knowledge fact kind is invalid");
        }

        if (!payload_reader.read_u32(knowledge.confidence_per_10k)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot knowledge confidence is truncated");
        }

        restored_state.agent_knowledge.push_back(knowledge);
    }

    if (!payload_reader.read_u64(next_knowledge_id)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot next knowledge id is truncated");
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

    std::uint64_t transmission_count = 0;
    if (!payload_reader.read_u64(transmission_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot transmission section is truncated");
    }
    if (transmission_count > payload_reader.remaining() / 85U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot transmission section has invalid length");
    }

    restored_state.knowledge_transmissions.clear();
    restored_state.knowledge_transmissions.reserve(static_cast<std::size_t>(transmission_count));
    std::unordered_set<CommunicationId> transmission_ids;
    std::unordered_set<MemoryId> recipient_memory_ids;

    for (std::uint64_t i = 0; i < transmission_count; ++i) {
        KnowledgeTransmission transmission{};
        std::uint8_t method_raw = 0;

        if (!payload_reader.read_u64(transmission.id) ||
            !payload_reader.read_u64(transmission.speaker_id) ||
            !payload_reader.read_u64(transmission.recipient_id) ||
            !payload_reader.read_u64(transmission.speaker_household_id) ||
            !payload_reader.read_u64(transmission.recipient_household_id) ||
            !payload_reader.read_u64(transmission.speaker_settlement_id) ||
            !payload_reader.read_u64(transmission.recipient_settlement_id) ||
            !payload_reader.read_i64(transmission.transmitted_at.ticks) ||
            !payload_reader.read_u8(method_raw) ||
            !payload_reader.read_u64(transmission.speaker_memory_id) ||
            !payload_reader.read_u64(transmission.recipient_memory_id) ||
            !payload_reader.read_u32(transmission.confidence_per_10k)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot transmission record is truncated");
        }

        if (!is_valid_communication_id(transmission.id) ||
            !is_valid_person_id(transmission.speaker_id) ||
            !is_valid_person_id(transmission.recipient_id) ||
            !is_valid_household_id(transmission.speaker_household_id) ||
            !is_valid_household_id(transmission.recipient_household_id) ||
            !is_valid_settlement_id(transmission.speaker_settlement_id) ||
            !is_valid_settlement_id(transmission.recipient_settlement_id) ||
            !is_valid_memory_id(transmission.speaker_memory_id) ||
            !is_valid_memory_id(transmission.recipient_memory_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot transmission record contains invalid references");
        }
        if (transmission.speaker_id == transmission.recipient_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot transmission speaker and recipient must differ");
        }
        if (transmission.speaker_settlement_id != transmission.recipient_settlement_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot transmission must remain within one settlement");
        }
        if (method_raw > static_cast<std::uint8_t>(CommunicationMethod::Teaching)) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot transmission method enum is invalid");
        }
        transmission.method = static_cast<CommunicationMethod>(method_raw);
        if (transmission.transmitted_at.ticks < 0 || transmission.transmitted_at.ticks > restored_state.time.ticks) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot transmission time is out of range");
        }
        if (transmission.confidence_per_10k > 10'000U) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot transmission confidence must be <= 10000");
        }
        if (!transmission_ids.insert(transmission.id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot contains duplicate transmission ids");
        }
        if (!recipient_memory_ids.insert(transmission.recipient_memory_id).second) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot transmission recipient memory must be unique");
        }

        restored_state.knowledge_transmissions.push_back(transmission);
    }

    std::uint64_t record_count = 0;
    if (!payload_reader.read_u64(record_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot record section is truncated");
    }
    if (record_count > payload_reader.remaining() / 56U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot record section has invalid length");
    }

    restored_state.knowledge_records.clear();
    restored_state.knowledge_records.reserve(static_cast<std::size_t>(record_count));
    std::unordered_set<RecordId> record_ids;
    std::unordered_set<RecordSemanticKey, RecordSemanticKeyHash> record_semantic_keys;

    for (std::uint64_t i = 0; i < record_count; ++i) {
        KnowledgeRecord record{};
        if (!payload_reader.read_u64(record.id) ||
            !payload_reader.read_u64(record.settlement_id) ||
            !payload_reader.read_u64(record.author_id) ||
            !payload_reader.read_u64(record.author_household_id) ||
            !payload_reader.read_i64(record.recorded_at.ticks) ||
            !payload_reader.read_u64(record.source_memory_id) ||
            !payload_reader.read_u64(record.source_event_id) ||
            !payload_reader.read_u32(record.confidence_per_10k)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot record entry is truncated");
        }

        if (!is_valid_record_id(record.id) ||
            !is_valid_settlement_id(record.settlement_id) ||
            !is_valid_person_id(record.author_id) ||
            !is_valid_household_id(record.author_household_id) ||
            !is_valid_memory_id(record.source_memory_id) ||
            !is_valid_event_id(record.source_event_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot record entry contains invalid references");
        }
        if (record.recorded_at.ticks < 0 || record.recorded_at.ticks > restored_state.time.ticks) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot record time is out of range");
        }
        if (record.confidence_per_10k > 10'000U) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot record confidence must be <= 10000");
        }
        if (!record_ids.insert(record.id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot contains duplicate record ids");
        }
        const auto inserted_key = record_semantic_keys.insert(RecordSemanticKey{
            .settlement_id = record.settlement_id,
            .source_event_id = record.source_event_id,
        });
        if (!inserted_key.second) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot duplicate record semantic key");
        }

        restored_state.knowledge_records.push_back(record);
    }

    std::uint64_t record_access_count = 0;
    if (!payload_reader.read_u64(record_access_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot record access section is truncated");
    }
    if (record_access_count > payload_reader.remaining() / 48U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot record access section has invalid length");
    }

    restored_state.knowledge_record_accesses.clear();
    restored_state.knowledge_record_accesses.reserve(static_cast<std::size_t>(record_access_count));
    std::unordered_set<MemoryId> record_access_memory_ids;
    std::unordered_set<RecordAccessSemanticKey, RecordAccessSemanticKeyHash> record_access_semantic_keys;

    for (std::uint64_t i = 0; i < record_access_count; ++i) {
        KnowledgeRecordAccess access{};
        if (!payload_reader.read_u64(access.record_id) ||
            !payload_reader.read_u64(access.reader_id) ||
            !payload_reader.read_u64(access.reader_household_id) ||
            !payload_reader.read_u64(access.reader_settlement_id) ||
            !payload_reader.read_u64(access.reader_memory_id) ||
            !payload_reader.read_i64(access.read_at.ticks) ||
            !payload_reader.read_u32(access.confidence_per_10k)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot record access entry is truncated");
        }

        if (!is_valid_record_id(access.record_id) ||
            !is_valid_person_id(access.reader_id) ||
            !is_valid_household_id(access.reader_household_id) ||
            !is_valid_settlement_id(access.reader_settlement_id) ||
            !is_valid_memory_id(access.reader_memory_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot record access entry contains invalid references");
        }
        if (access.read_at.ticks < 0 || access.read_at.ticks > restored_state.time.ticks) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot record access time is out of range");
        }
        if (access.confidence_per_10k > 10'000U) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot record access confidence must be <= 10000");
        }
        if (!record_access_memory_ids.insert(access.reader_memory_id).second) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot record access reader memory must be unique");
        }
        const auto inserted_key = record_access_semantic_keys.insert(RecordAccessSemanticKey{
            .record_id = access.record_id,
            .reader_id = access.reader_id,
        });
        if (!inserted_key.second) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot duplicate record access semantic key");
        }

        restored_state.knowledge_record_accesses.push_back(access);
    }

    std::uint64_t institution_count = 0;
    if (!payload_reader.read_u64(institution_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot institution section is truncated");
    }
    if (institution_count > payload_reader.remaining() / 39U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot institution section has invalid length");
    }

    restored_state.institutions.clear();
    restored_state.institutions.reserve(static_cast<std::size_t>(institution_count));
    std::unordered_set<InstitutionId> institution_ids;

    for (std::uint64_t i = 0; i < institution_count; ++i) {
        Institution institution{};
        std::uint8_t kind_raw = 0;
        std::uint64_t member_count = 0;
        std::uint64_t preserved_record_count = 0;
        std::uint64_t preserved_discovery_count = 0;

        if (!payload_reader.read_u64(institution.id) ||
            !payload_reader.read_u8(kind_raw) ||
            !payload_reader.read_u64(institution.settlement_id) ||
            !payload_reader.read_u64(institution.founder_id) ||
            !payload_reader.read_i64(institution.founded_at.ticks) ||
            !payload_reader.read_bool(institution.active) ||
            !payload_reader.read_u64(member_count)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot institution record is truncated");
        }

        if (!is_valid_institution_id(institution.id)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot institution id must be non-zero");
        }
        if (kind_raw > static_cast<std::uint8_t>(InstitutionKind::KnowledgeKeeper)) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot institution kind enum is invalid");
        }
        institution.kind = static_cast<InstitutionKind>(kind_raw);
        if (!institution_ids.insert(institution.id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot contains duplicate institution ids");
        }
        if (member_count > payload_reader.remaining() / 8U) {
            return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot institution member list length is invalid");
        }

        std::unordered_set<PersonId> member_ids;
        institution.member_ids.reserve(static_cast<std::size_t>(member_count));
        for (std::uint64_t j = 0; j < member_count; ++j) {
            PersonId member_id = 0;
            if (!payload_reader.read_u64(member_id)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot institution member is truncated");
            }
            if (!is_valid_person_id(member_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot institution member id must be non-zero");
            }
            if (!member_ids.insert(member_id).second) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot institution has duplicate members");
            }
            institution.member_ids.push_back(member_id);
        }

        if (!payload_reader.read_u64(preserved_record_count)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot institution preserved-record count is truncated");
        }
        if (preserved_record_count > payload_reader.remaining() / 8U) {
            return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot institution preserved-record list length is invalid");
        }
        std::unordered_set<RecordId> preserved_record_ids;
        institution.preserved_record_ids.reserve(static_cast<std::size_t>(preserved_record_count));
        for (std::uint64_t j = 0; j < preserved_record_count; ++j) {
            RecordId record_id = 0;
            if (!payload_reader.read_u64(record_id)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot institution preserved record id is truncated");
            }
            if (!is_valid_record_id(record_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot institution preserved record id must be non-zero");
            }
            if (!preserved_record_ids.insert(record_id).second) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot institution has duplicate preserved records");
            }
            institution.preserved_record_ids.push_back(record_id);
        }

        if (!payload_reader.read_u64(preserved_discovery_count)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot institution preserved-discovery count is truncated");
        }
        if (preserved_discovery_count > payload_reader.remaining() / 4U) {
            return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot institution preserved-discovery list length is invalid");
        }
        std::unordered_set<std::uint32_t> preserved_discoveries;
        institution.preserved_discovery_keys.reserve(static_cast<std::size_t>(preserved_discovery_count));
        for (std::uint64_t j = 0; j < preserved_discovery_count; ++j) {
            std::uint32_t discovery_key = 0;
            if (!payload_reader.read_u32(discovery_key)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot institution preserved discovery key is truncated");
            }
            if (discovery_key == 0U) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot institution preserved discovery key must be non-zero");
            }
            if (!preserved_discoveries.insert(discovery_key).second) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot institution has duplicate preserved discovery keys");
            }
            institution.preserved_discovery_keys.push_back(discovery_key);
        }

        restored_state.institutions.push_back(std::move(institution));
    }

    if (!payload_reader.read_u64(next_communication_id)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot next communication id is truncated");
    }

    if (!payload_reader.read_u64(next_record_id)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot next record id is truncated");
    }

    if (!payload_reader.read_u64(next_institution_id)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot next institution id is truncated");
    }

    if (!payload_reader.read_u64(next_building_id)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot next building id is truncated");
    }

    std::uint64_t household_economy_count = 0;
    if (!payload_reader.read_u64(household_economy_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot household economy section is truncated");
    }
    if (household_economy_count != restored_state.households.size()) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot household economy section count mismatch");
    }

    std::unordered_map<HouseholdId, Household*> household_mutable_by_id;
    household_mutable_by_id.reserve(restored_state.households.size());
    for (auto& household : restored_state.households) {
        household_mutable_by_id.emplace(household.id, &household);
    }

    std::unordered_set<HouseholdId> household_economy_ids;
    for (std::uint64_t i = 0; i < household_economy_count; ++i) {
        HouseholdId household_id = 0;
        std::int64_t wood = 0;
        std::int64_t stone = 0;
        std::int64_t timber = 0;
        std::int64_t basic_tool = 0;
        std::uint32_t productive_share = 0;
        std::uint32_t commons_share = 0;
        std::int64_t food_in = 0;
        std::int64_t food_out = 0;
        std::int64_t material_in = 0;
        std::int64_t material_out = 0;
        std::int64_t wealth = 0;
        std::uint32_t offset_x_raw = 0;
        std::uint32_t offset_z_raw = 0;

        if (!payload_reader.read_u64(household_id) ||
            !payload_reader.read_i64(wood) ||
            !payload_reader.read_i64(stone) ||
            !payload_reader.read_i64(timber) ||
            !payload_reader.read_i64(basic_tool) ||
            !payload_reader.read_u32(productive_share) ||
            !payload_reader.read_u32(commons_share) ||
            !payload_reader.read_i64(food_in) ||
            !payload_reader.read_i64(food_out) ||
            !payload_reader.read_i64(material_in) ||
            !payload_reader.read_i64(material_out) ||
            !payload_reader.read_i64(wealth) ||
            !payload_reader.read_u32(offset_x_raw) ||
            !payload_reader.read_u32(offset_z_raw)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot household economy record is truncated");
        }

        if (!household_economy_ids.insert(household_id).second) {
            return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot household economy section has duplicate household ids");
        }

        const auto household_it = household_mutable_by_id.find(household_id);
        if (household_it == household_mutable_by_id.end()) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot household economy references unknown household");
        }
        if (wood < 0 || stone < 0 || timber < 0 || basic_tool < 0 ||
            productive_share > 10'000U || commons_share > 10'000U ||
            food_in < 0 || food_out < 0 || material_in < 0 || material_out < 0 || wealth < 0) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot household economy values must be non-negative");
        }

        auto* household = household_it->second;
        household->owned_materials.wood.value = wood;
        household->owned_materials.stone.value = stone;
        household->owned_materials.timber.value = timber;
        household->owned_materials.basic_tool.value = basic_tool;
        household->productive_access_share_points = productive_share;
        household->commons_access_share_points = commons_share;
        household->cumulative_food_transferred_in = food_in;
        household->cumulative_food_transferred_out = food_out;
        household->cumulative_material_transferred_in = material_in;
        household->cumulative_material_transferred_out = material_out;
        household->derived_wealth_points = wealth;
        household->settlement_offset_x = static_cast<std::int32_t>(offset_x_raw);
        household->settlement_offset_z = static_cast<std::int32_t>(offset_z_raw);
    }

    std::uint64_t settlement_economy_count = 0;
    if (!payload_reader.read_u64(settlement_economy_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot settlement economy section is truncated");
    }
    if (settlement_economy_count != restored_state.settlements.size()) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot settlement economy section count mismatch");
    }

    std::unordered_set<SettlementId> settlement_economy_ids;
    for (std::uint64_t i = 0; i < settlement_economy_count; ++i) {
        SettlementId settlement_id = 0;
        std::int64_t commons_food = 0;
        std::int64_t transfer_volume = 0;
        std::int64_t exchange_volume = 0;
        std::uint32_t viability_streak = 0;
        std::uint32_t pressure_streak = 0;
        std::int64_t dwelling_bonus = 0;
        std::int64_t food_store_bonus = 0;
        std::int64_t workshop_bonus = 0;
        std::int64_t median_wealth = 0;
        std::int64_t min_wealth = 0;
        std::int64_t max_wealth = 0;
        std::uint64_t inequality_numerator = 0;
        std::uint64_t inequality_denominator = 0;
        std::uint64_t population = 0;
        std::uint8_t tier_raw = 0;
        std::uint8_t pending_tier_raw = 0;
        std::uint32_t tier_transition_streak = 0;

        if (!payload_reader.read_u64(settlement_id) ||
            !payload_reader.read_i64(commons_food) ||
            !payload_reader.read_i64(transfer_volume) ||
            !payload_reader.read_i64(exchange_volume) ||
            !payload_reader.read_u32(viability_streak) ||
            !payload_reader.read_u32(pressure_streak) ||
            !payload_reader.read_i64(dwelling_bonus) ||
            !payload_reader.read_i64(food_store_bonus) ||
            !payload_reader.read_i64(workshop_bonus) ||
            !payload_reader.read_i64(median_wealth) ||
            !payload_reader.read_i64(min_wealth) ||
            !payload_reader.read_i64(max_wealth) ||
            !payload_reader.read_u64(inequality_numerator) ||
            !payload_reader.read_u64(inequality_denominator) ||
            !payload_reader.read_u64(population) ||
            !payload_reader.read_u8(tier_raw) ||
            !payload_reader.read_u8(pending_tier_raw) ||
            !payload_reader.read_u32(tier_transition_streak)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot settlement economy record is truncated");
        }
        if (tier_raw > static_cast<std::uint8_t>(SettlementTier::City) ||
            pending_tier_raw > static_cast<std::uint8_t>(SettlementTier::City)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement tier must be a known value");
        }

        if (!settlement_economy_ids.insert(settlement_id).second) {
            return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot settlement economy section has duplicate settlement ids");
        }
        const auto settlement_it = settlement_mutable_by_id.find(settlement_id);
        if (settlement_it == settlement_mutable_by_id.end()) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot settlement economy references unknown settlement");
        }
        if (commons_food < 0 || transfer_volume < 0 || exchange_volume < 0 ||
            dwelling_bonus < 0 || food_store_bonus < 0 || workshop_bonus < 0 ||
            median_wealth < 0 || min_wealth < 0 || max_wealth < 0) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement economy values must be non-negative");
        }

        auto* settlement = settlement_it->second;
        settlement->commons_food.value = commons_food;
        settlement->last_transfer_food_volume = transfer_volume;
        settlement->last_exchange_volume = exchange_volume;
        settlement->economic_viability_streak = viability_streak;
        settlement->economic_pressure_streak = pressure_streak;
        settlement->dwelling_capacity_bonus = dwelling_bonus;
        settlement->food_store_capacity_bonus = food_store_bonus;
        settlement->workshop_capacity_bonus = workshop_bonus;
        settlement->last_median_household_wealth = median_wealth;
        settlement->last_min_household_wealth = min_wealth;
        settlement->last_max_household_wealth = max_wealth;
        settlement->last_inequality_numerator = inequality_numerator;
        settlement->last_inequality_denominator = inequality_denominator;
        settlement->population = population;
        settlement->tier = static_cast<SettlementTier>(tier_raw);
        settlement->pending_tier = static_cast<SettlementTier>(pending_tier_raw);
        settlement->tier_transition_streak = tier_transition_streak;
    }

    std::uint64_t building_count = 0;
    if (!payload_reader.read_u64(building_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot building section is truncated");
    }

    restored_state.buildings.clear();
    restored_state.buildings.reserve(static_cast<std::size_t>(building_count));
    std::unordered_set<BuildingId> building_ids;
    for (std::uint64_t i = 0; i < building_count; ++i) {
        Building building{};
        std::uint8_t type_raw = 0;
        bool has_owner = false;
        bool has_completed = false;
        std::uint8_t state_raw = 0;

        if (!payload_reader.read_u64(building.id) ||
            !payload_reader.read_u8(type_raw) ||
            !payload_reader.read_u64(building.settlement_id) ||
            !payload_reader.read_bool(has_owner)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot building record is truncated");
        }
        if (has_owner) {
            HouseholdId owner_household_id = 0;
            if (!payload_reader.read_u64(owner_household_id)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot building owner id is truncated");
            }
            building.owner_household_id = owner_household_id;
        }
        if (!payload_reader.read_bool(building.settlement_commons_owned) ||
            !payload_reader.read_u8(state_raw) ||
            !payload_reader.read_i64(building.timber_required) ||
            !payload_reader.read_i64(building.stone_required) ||
            !payload_reader.read_i64(building.labour_required) ||
            !payload_reader.read_i64(building.timber_committed) ||
            !payload_reader.read_i64(building.stone_committed) ||
            !payload_reader.read_i64(building.labour_completed) ||
            !payload_reader.read_i64(building.started_at.ticks) ||
            !payload_reader.read_bool(has_completed)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot building progress fields are truncated");
        }
        if (has_completed) {
            SimulationTime completed_at{};
            if (!payload_reader.read_i64(completed_at.ticks)) {
                return load_error(SnapshotErrorCode::TruncatedData, "Snapshot building completion time is truncated");
            }
            building.completed_at = completed_at;
        }

        if (!building_ids.insert(building.id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot contains duplicate building ids");
        }
        if (type_raw > static_cast<std::uint8_t>(BuildingType::Workshop)) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot building type is invalid");
        }
        if (state_raw > static_cast<std::uint8_t>(ConstructionState::Complete)) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot construction state is invalid");
        }
        if (!settlement_mutable_by_id.contains(building.settlement_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot building references unknown settlement");
        }
        if (building.owner_household_id.has_value() && !household_mutable_by_id.contains(*building.owner_household_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot building references unknown owner household");
        }
        if (building.owner_household_id.has_value()) {
            const auto* owner_household = household_mutable_by_id.at(*building.owner_household_id);
            if (owner_household->settlement_id != building.settlement_id) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot building owner household/settlement mismatch");
            }
        }
        if (building.timber_required < 0 || building.stone_required < 0 || building.labour_required < 0 ||
            building.timber_committed < 0 || building.stone_committed < 0 || building.labour_completed < 0) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot building values must be non-negative");
        }
        if (building.timber_committed > building.timber_required ||
            building.stone_committed > building.stone_required ||
            building.labour_completed > building.labour_required) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot building committed values exceed requirements");
        }
        if (building.started_at.ticks < 0 || building.started_at.ticks > restored_state.time.ticks) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot building start time is invalid");
        }

        const auto construction_state = static_cast<ConstructionState>(state_raw);
        if (construction_state == ConstructionState::Complete) {
            if (!building.completed_at.has_value()) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot complete building must include completion time");
            }
            if (building.completed_at->ticks < building.started_at.ticks ||
                building.completed_at->ticks > restored_state.time.ticks) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot building completion time is invalid");
            }
        } else if (building.completed_at.has_value()) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot incomplete building must not include completion time");
        }

        building.type = static_cast<BuildingType>(type_raw);
        building.state = construction_state;
        restored_state.buildings.push_back(std::move(building));
    }

    std::uint8_t detail_level_raw = 0;
    if (!payload_reader.read_u64(next_cohort_id) ||
        !payload_reader.read_u8(detail_level_raw)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot LOD fixed section is truncated");
    }
    if (detail_level_raw > static_cast<std::uint8_t>(SimulationDetailLevel::RegionalCohort)) {
        return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot world detail level is invalid");
    }
    restored_state.detail_level = static_cast<SimulationDetailLevel>(detail_level_raw);

    std::uint64_t region_lod_count = 0;
    if (!payload_reader.read_u64(region_lod_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot region LOD section is truncated");
    }
    if (region_lod_count > payload_reader.remaining() / 9U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot region LOD section length is invalid");
    }

    restored_state.region_lod_states.clear();
    restored_state.region_lod_states.reserve(static_cast<std::size_t>(region_lod_count));
    std::unordered_set<RegionId> region_lod_ids;
    for (std::uint64_t i = 0; i < region_lod_count; ++i) {
        RegionLodState region_lod{};
        std::uint8_t region_detail_level_raw = 0;
        if (!payload_reader.read_u64(region_lod.region_id) ||
            !payload_reader.read_u8(region_detail_level_raw)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot region LOD record is truncated");
        }
        if (region_detail_level_raw > static_cast<std::uint8_t>(SimulationDetailLevel::RegionalCohort)) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot region detail level is invalid");
        }
        if (!region_ids.contains(region_lod.region_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot region LOD references unknown region");
        }
        if (!region_lod_ids.insert(region_lod.region_id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot region LOD contains duplicate region ids");
        }
        region_lod.detail_level = static_cast<SimulationDetailLevel>(region_detail_level_raw);
        restored_state.region_lod_states.push_back(region_lod);
    }

    std::uint64_t cohort_count = 0;
    if (!payload_reader.read_u64(cohort_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot cohort section is truncated");
    }
    if (cohort_count > payload_reader.remaining() / 41U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot cohort section length is invalid");
    }

    restored_state.population_cohorts.clear();
    restored_state.population_cohorts.reserve(static_cast<std::size_t>(cohort_count));
    std::unordered_set<CohortId> cohort_ids;
    for (std::uint64_t i = 0; i < cohort_count; ++i) {
        PopulationCohort cohort{};
        std::uint8_t age_band_raw = 0;
        std::uint8_t sex_raw = 0;
        std::uint8_t occupation_group_raw = 0;
        std::uint8_t health_band_raw = 0;
        std::uint8_t wealth_band_raw = 0;

        if (!payload_reader.read_u64(cohort.id) ||
            !payload_reader.read_u64(cohort.region_id) ||
            !payload_reader.read_u64(cohort.settlement_id) ||
            !payload_reader.read_u8(age_band_raw) ||
            !payload_reader.read_u8(sex_raw) ||
            !payload_reader.read_u8(occupation_group_raw) ||
            !payload_reader.read_u8(health_band_raw) ||
            !payload_reader.read_u8(wealth_band_raw) ||
            !payload_reader.read_u32(cohort.knowledge_profile_id) ||
            !payload_reader.read_u64(cohort.population)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot cohort record is truncated");
        }

        if (!is_valid_cohort_id(cohort.id)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot cohort id must be non-zero");
        }
        if (!cohort_ids.insert(cohort.id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot contains duplicate cohort ids");
        }
        if (!region_ids.contains(cohort.region_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot cohort references unknown region");
        }
        if (!settlement_mutable_by_id.contains(cohort.settlement_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot cohort references unknown settlement");
        }
        if (settlement_mutable_by_id.at(cohort.settlement_id)->region_id != cohort.region_id) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot cohort settlement/region mismatch");
        }
        if (cohort.population == 0U) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot cohort population must be non-zero");
        }
        if (age_band_raw > static_cast<std::uint8_t>(CohortAgeBand::Senior) ||
            sex_raw > static_cast<std::uint8_t>(BiologicalSex::Male) ||
            occupation_group_raw > static_cast<std::uint8_t>(CohortOccupationGroup::Craft) ||
            health_band_raw > static_cast<std::uint8_t>(CohortHealthBand::High) ||
            wealth_band_raw > static_cast<std::uint8_t>(CohortWealthBand::High)) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot cohort enum value is invalid");
        }

        cohort.age_band = static_cast<CohortAgeBand>(age_band_raw);
        cohort.sex = static_cast<BiologicalSex>(sex_raw);
        cohort.occupation_group = static_cast<CohortOccupationGroup>(occupation_group_raw);
        cohort.health_band = static_cast<CohortHealthBand>(health_band_raw);
        cohort.wealth_band = static_cast<CohortWealthBand>(wealth_band_raw);
        restored_state.population_cohorts.push_back(cohort);
    }

    std::uint64_t person_lod_metadata_count = 0;
    if (!payload_reader.read_u64(person_lod_metadata_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot person LOD metadata section is truncated");
    }
    if (person_lod_metadata_count != restored_state.people.size()) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot person LOD metadata count mismatch");
    }

    std::unordered_map<PersonId, std::size_t> person_index_by_id;
    person_index_by_id.reserve(restored_state.people.size());
    for (std::size_t index = 0; index < restored_state.people.size(); ++index) {
        person_index_by_id.emplace(restored_state.people[index].id, index);
    }

    std::unordered_set<PersonId> metadata_ids;
    metadata_ids.reserve(restored_state.people.size());
    for (std::uint64_t i = 0; i < person_lod_metadata_count; ++i) {
        PersonId person_id = 0;
        bool materialized_from_cohort = false;
        bool has_source_cohort = false;
        CohortId source_cohort_id = 0;
        if (!payload_reader.read_u64(person_id) ||
            !payload_reader.read_bool(materialized_from_cohort) ||
            !payload_reader.read_bool(has_source_cohort)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot person LOD metadata record is truncated");
        }
        if (has_source_cohort && !payload_reader.read_u64(source_cohort_id)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot person source cohort id is truncated");
        }
        if (!metadata_ids.insert(person_id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot person LOD metadata contains duplicate person ids");
        }
        if (!person_index_by_id.contains(person_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot person LOD metadata references unknown person");
        }
        if (has_source_cohort && !is_valid_cohort_id(source_cohort_id)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot person source cohort id must be non-zero");
        }

        auto& person = restored_state.people[person_index_by_id[person_id]];
        person.materialized_from_cohort = materialized_from_cohort;
        person.source_cohort_id = has_source_cohort ? std::optional<CohortId>{source_cohort_id} : std::nullopt;
        if (person.materialized_from_cohort && !person.source_cohort_id.has_value()) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot materialized person must retain source cohort id");
        }
    }

    std::uint64_t settlement_aggregate_count = 0;
    if (!payload_reader.read_u64(settlement_aggregate_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot settlement aggregate section is truncated");
    }
    if (settlement_aggregate_count != restored_state.settlements.size()) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot settlement aggregate count mismatch");
    }

    restored_state.settlement_aggregates.clear();
    restored_state.settlement_aggregates.reserve(static_cast<std::size_t>(settlement_aggregate_count));
    std::unordered_set<SettlementId> aggregate_ids;
    for (std::uint64_t i = 0; i < settlement_aggregate_count; ++i) {
        SettlementAggregateState aggregate{};
        if (!payload_reader.read_u64(aggregate.settlement_id) ||
            !payload_reader.read_u64(aggregate.region_id) ||
            !payload_reader.read_u64(aggregate.household_count) ||
            !payload_reader.read_u64(aggregate.small_household_count) ||
            !payload_reader.read_u64(aggregate.medium_household_count) ||
            !payload_reader.read_u64(aggregate.large_household_count) ||
            !payload_reader.read_i64(aggregate.food_inventory) ||
            !payload_reader.read_i64(aggregate.food_produced_total) ||
            !payload_reader.read_i64(aggregate.food_consumed_total) ||
            !payload_reader.read_i64(aggregate.unmet_food_total) ||
            !payload_reader.read_i64(aggregate.labour_capacity) ||
            !payload_reader.read_i64(aggregate.labour_demand) ||
            !payload_reader.read_i64(aggregate.labour_allocated) ||
            !payload_reader.read_u64(aggregate.labour_low_household_count) ||
            !payload_reader.read_u64(aggregate.labour_medium_household_count) ||
            !payload_reader.read_u64(aggregate.labour_high_household_count) ||
            !payload_reader.read_u64(aggregate.wealth_low_household_count) ||
            !payload_reader.read_u64(aggregate.wealth_medium_household_count) ||
            !payload_reader.read_u64(aggregate.wealth_high_household_count) ||
            !payload_reader.read_u64(aggregate.food_buffer_low_household_count) ||
            !payload_reader.read_u64(aggregate.food_buffer_medium_household_count) ||
            !payload_reader.read_u64(aggregate.food_buffer_high_household_count) ||
            !payload_reader.read_i64(aggregate.wealth_points_total) ||
            !payload_reader.read_i64(aggregate.productive_access_points) ||
            !payload_reader.read_i64(aggregate.required_food_year) ||
            !payload_reader.read_i64(aggregate.unmet_food_year)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot settlement aggregate record is truncated");
        }

        if (!aggregate_ids.insert(aggregate.settlement_id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot settlement aggregate contains duplicate settlement ids");
        }
        if (!settlement_mutable_by_id.contains(aggregate.settlement_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot settlement aggregate references unknown settlement");
        }
        if (settlement_mutable_by_id.at(aggregate.settlement_id)->region_id != aggregate.region_id) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot settlement aggregate region mismatch");
        }
        if (aggregate.small_household_count + aggregate.medium_household_count + aggregate.large_household_count != aggregate.household_count) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement aggregate household bands are inconsistent");
        }
        if (aggregate.food_inventory < 0 || aggregate.food_produced_total < 0 || aggregate.food_consumed_total < 0 ||
            aggregate.unmet_food_total < 0 || aggregate.labour_capacity < 0 || aggregate.labour_demand < 0 || aggregate.labour_allocated < 0) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement aggregate values must be non-negative");
        }
        if (aggregate.wealth_points_total < 0 || aggregate.productive_access_points < 0 ||
            aggregate.required_food_year < 0 || aggregate.unmet_food_year < 0) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement aggregate economic values must be non-negative");
        }
        if (aggregate.unmet_food_year > aggregate.required_food_year) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement aggregate unmet food exceeds yearly requirement");
        }
        if (aggregate.labour_low_household_count + aggregate.labour_medium_household_count +
                aggregate.labour_high_household_count != aggregate.household_count) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement aggregate labour bands are inconsistent");
        }
        if (aggregate.wealth_low_household_count + aggregate.wealth_medium_household_count +
                aggregate.wealth_high_household_count != aggregate.household_count) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement aggregate wealth bands are inconsistent");
        }
        if (aggregate.food_buffer_low_household_count + aggregate.food_buffer_medium_household_count +
                aggregate.food_buffer_high_household_count != aggregate.household_count) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement aggregate food buffer bands are inconsistent");
        }

        restored_state.settlement_aggregates.push_back(aggregate);
    }

    if (!is_v24_migration) {
        const auto section_result = read_v25_placement_and_resync_section(payload_reader, restored_state);
        if (!section_result.ok) {
            return section_result;
        }
    }

    // v24 and v25 snapshots carry no viability section: every settlement
    // starts its count of days below the viable size at zero.
    if (has_viability_section) {
        const auto viability_result = read_v26_viability_section(payload_reader, restored_state);
        if (!viability_result.ok) {
            return viability_result;
        }
    }

    if (!payload_reader.read_u64(restored_state.aggregate_births_total) ||
        !payload_reader.read_u64(restored_state.aggregate_deaths_total)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot aggregate demography totals are truncated");
    }

    // Snapshots before v27 carry no coin: every balance stays at zero.
    if (has_coin_section) {
        const auto coin_result = read_v27_coin_section(payload_reader, restored_state);
        if (!coin_result.ok) {
            return coin_result;
        }
    }

    // Snapshots before v28 carry no merchants: nobody remembers any prices.
    if (has_merchant_memory_section) {
        const auto memory_result = read_v28_merchant_memory_section(payload_reader, restored_state);
        if (!memory_result.ok) {
            return memory_result;
        }
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

    if (!is_valid_cohort_id(next_cohort_id)) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot next cohort id must be non-zero");
    }

    CohortId max_cohort_id = 0;
    for (const auto& cohort : restored_state.population_cohorts) {
        if (cohort.id > max_cohort_id) {
            max_cohort_id = cohort.id;
        }
    }
    const auto expected_next_cohort_id = max_cohort_id + 1U;
    if (next_cohort_id < expected_next_cohort_id) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot next cohort id is inconsistent");
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
        restored_state.eligible_workers = recomputed_eligible_workers;
        restored_state.active_workers = recomputed_active_workers;
        restored_state.unallocated_workers = recomputed_unallocated_workers;
        restored_state.cultivators = recomputed_cultivators;
        restored_state.gatherers = recomputed_gatherers;
        restored_state.total_labour_capacity = recomputed_total_capacity;
        restored_state.cultivation_labour_available = recomputed_cultivation_available;
        restored_state.gathering_labour_available = recomputed_gathering_available;
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

    std::uint64_t cohort_population = 0;
    for (const auto& cohort : restored_state.population_cohorts) {
        if (cohort_population > (std::numeric_limits<std::uint64_t>::max() - cohort.population)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot cohort population sum overflow");
        }
        cohort_population += cohort.population;
    }

    if (restored_state.detail_level == SimulationDetailLevel::Detailed) {
        if (!restored_state.population_cohorts.empty()) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot detailed mode must not contain aggregate cohorts");
        }
        if (restored_state.region_lod_states.size() != restored_state.regions.size()) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot detailed mode must include one LOD entry per region");
        }
        for (const auto& region_lod : restored_state.region_lod_states) {
            if (region_lod.detail_level != SimulationDetailLevel::Detailed) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot detailed mode cannot include non-detailed region LOD entries");
            }
        }
        if (restored_state.population != alive_count) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot population does not match alive person count");
        }
    } else {
        if (alive_count > (std::numeric_limits<std::uint64_t>::max() - cohort_population)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot total represented population overflow");
        }
        if (restored_state.population != (alive_count + cohort_population)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot population does not match detailed plus cohort representation");
        }
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
    std::unordered_map<SettlementId, EventId> founded_event_id_by_settlement;
    std::unordered_map<SettlementId, EventId> extinct_event_id_by_settlement;
    std::unordered_map<SettlementId, EventId> merged_event_id_by_absorbed_settlement;
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

        if (event.type == EventType::Birth) {
            ++birth_event_count;
            if (!std::holds_alternative<BirthEventPayload>(event.payload)) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot event payload does not match birth type");
            }

            const auto& payload_data = std::get<BirthEventPayload>(event.payload);
            if (payload_data.child_id == 0U || payload_data.mother_id == 0U || payload_data.household_id == 0U) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot birth event must include canonical child/mother/household ids");
            }
            if (!payload_data.father_id.has_value()) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot birth event must include mother and father ids");
            }

            const auto child_it = person_by_id.find(payload_data.child_id);
            const auto mother_it = person_by_id.find(payload_data.mother_id);
            const auto father_it = person_by_id.find(*payload_data.father_id);
            if (child_it == person_by_id.end() || mother_it == person_by_id.end() || father_it == person_by_id.end()) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot birth event parent references are invalid");
            }
            if (mother_it->second->sex != BiologicalSex::Female || father_it->second->sex != BiologicalSex::Male) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot birth event parent sexes are invalid");
            }
            if (!household_by_id.contains(payload_data.household_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot birth event household references unknown household");
            }
        } else if (event.type == EventType::Death) {
            ++death_event_count;
            if (!std::holds_alternative<DeathEventPayload>(event.payload)) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot event payload does not match death type");
            }

            const auto& payload_data = std::get<DeathEventPayload>(event.payload);
            if (payload_data.person_id == 0U || payload_data.household_id == 0U) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot death event must include canonical person/household ids");
            }
            if (!person_by_id.contains(payload_data.person_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot death event person references unknown person");
            }
            if (!household_by_id.contains(payload_data.household_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot death event household references unknown household");
            }
        } else if (event.type == EventType::SettlementFounded) {
            if (!std::holds_alternative<SettlementFoundedEventPayload>(event.payload)) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot event payload does not match settlement founded type");
            }

            const auto& payload_data = std::get<SettlementFoundedEventPayload>(event.payload);
            if (payload_data.settlement_id == 0U || payload_data.region_id == 0U) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot founded event must include settlement and region ids");
            }
            if (!settlement_by_id.contains(payload_data.settlement_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot founded event references unknown settlement");
            }
            if (!region_by_id.contains(payload_data.region_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot founded event references unknown region");
            }
            const auto* settlement = settlement_by_id.at(payload_data.settlement_id);
            if (settlement->region_id != payload_data.region_id) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot founded event settlement/region mismatch");
            }
            if (payload_data.source_settlement_id.has_value() &&
                !settlement_by_id.contains(*payload_data.source_settlement_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot founded event source settlement is invalid");
            }
            if (payload_data.source_settlement_id.has_value()) {
                const auto* source_settlement = settlement_by_id.at(*payload_data.source_settlement_id);
                if (source_settlement->region_id != payload_data.region_id) {
                    return load_error(SnapshotErrorCode::InvalidReference, "Snapshot founded event source settlement region mismatch");
                }
            }
            if (founded_event_id_by_settlement.contains(payload_data.settlement_id)) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement has duplicate founded events");
            }
            founded_event_id_by_settlement.emplace(payload_data.settlement_id, event.id);
        } else if (event.type == EventType::SettlementExtinct) {
            if (!std::holds_alternative<SettlementExtinctEventPayload>(event.payload)) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot event payload does not match settlement extinct type");
            }

            const auto& payload_data = std::get<SettlementExtinctEventPayload>(event.payload);
            if (payload_data.settlement_id == 0U || payload_data.region_id == 0U) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot extinct event must include settlement and region ids");
            }
            if (!settlement_by_id.contains(payload_data.settlement_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot extinct event references unknown settlement");
            }
            if (!region_by_id.contains(payload_data.region_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot extinct event references unknown region");
            }
            const auto* settlement = settlement_by_id.at(payload_data.settlement_id);
            if (settlement->region_id != payload_data.region_id) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot extinct event settlement/region mismatch");
            }
            if (extinct_event_id_by_settlement.contains(payload_data.settlement_id)) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement has duplicate extinct events");
            }
            extinct_event_id_by_settlement.emplace(payload_data.settlement_id, event.id);
        } else if (event.type == EventType::DiscoveryMade) {
            if (!std::holds_alternative<DiscoveryMadeEventPayload>(event.payload)) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot event payload does not match discovery made type");
            }

            const auto& payload_data = std::get<DiscoveryMadeEventPayload>(event.payload);
            if (!is_valid_person_id(payload_data.discoverer_id) || !is_valid_settlement_id(payload_data.settlement_id)) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot discovery event must include discoverer and settlement ids");
            }
            if (!person_by_id.contains(payload_data.discoverer_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot discovery event references unknown discoverer");
            }
            if (!settlement_by_id.contains(payload_data.settlement_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot discovery event references unknown settlement");
            }
        } else if (event.type == EventType::InstitutionFounded) {
            if (!std::holds_alternative<InstitutionFoundedEventPayload>(event.payload)) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot event payload does not match institution founded type");
            }

            const auto& payload_data = std::get<InstitutionFoundedEventPayload>(event.payload);
            if (!is_valid_institution_id(payload_data.institution_id) ||
                !is_valid_settlement_id(payload_data.settlement_id) ||
                !is_valid_person_id(payload_data.founder_id)) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot institution founded event references must be non-zero");
            }
            if (payload_data.kind != InstitutionKind::KnowledgeKeeper) {
                return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot institution founded event kind is invalid");
            }
            if (!settlement_by_id.contains(payload_data.settlement_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot institution founded event references unknown settlement");
            }
            if (!person_by_id.contains(payload_data.founder_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot institution founded event references unknown founder");
            }
        } else if (event.type == EventType::SettlementMerged) {
            if (!std::holds_alternative<SettlementMergedEventPayload>(event.payload)) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot event payload does not match settlement merged type");
            }

            const auto& payload_data = std::get<SettlementMergedEventPayload>(event.payload);
            if (payload_data.survivor_settlement_id == 0U || payload_data.absorbed_settlement_id == 0U ||
                payload_data.region_id == 0U) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot merged event must include survivor, absorbed and region ids");
            }
            if (payload_data.survivor_settlement_id == payload_data.absorbed_settlement_id) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot merged event survivor and absorbed settlement must differ");
            }
            if (!settlement_by_id.contains(payload_data.survivor_settlement_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot merged event references unknown survivor settlement");
            }
            if (!settlement_by_id.contains(payload_data.absorbed_settlement_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot merged event references unknown absorbed settlement");
            }
            if (!region_by_id.contains(payload_data.region_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot merged event references unknown region");
            }
            const auto* absorbed_settlement = settlement_by_id.at(payload_data.absorbed_settlement_id);
            if (absorbed_settlement->active) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot merged event absorbed settlement must be inactive");
            }
            if (merged_event_id_by_absorbed_settlement.contains(payload_data.absorbed_settlement_id)) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement has duplicate merged-away events");
            }
            merged_event_id_by_absorbed_settlement.emplace(payload_data.absorbed_settlement_id, event.id);
        } else if (event.type == EventType::OccupationChanged) {
            if (!std::holds_alternative<OccupationChangedEventPayload>(event.payload)) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot event payload does not match occupation changed type");
            }
            const auto& payload_data = std::get<OccupationChangedEventPayload>(event.payload);
            if (!person_by_id.contains(payload_data.person_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot occupation changed event references unknown person");
            }
            if (payload_data.from == payload_data.to) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot occupation changed event must record a real change");
            }
        } else if (event.type == EventType::HouseholdRelocated) {
            if (!std::holds_alternative<HouseholdRelocatedEventPayload>(event.payload)) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot event payload does not match household relocated type");
            }
            const auto& payload_data = std::get<HouseholdRelocatedEventPayload>(event.payload);
            if (!household_by_id.contains(payload_data.household_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot household relocated event references unknown household");
            }
            if (!settlement_by_id.contains(payload_data.from_settlement_id) ||
                !settlement_by_id.contains(payload_data.to_settlement_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot household relocated event references unknown settlement");
            }
            if (payload_data.from_settlement_id == payload_data.to_settlement_id) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot household relocated event must change settlement");
            }
            PersonId previous_member_id = 0;
            for (const auto member_id : payload_data.member_ids) {
                if (member_id <= previous_member_id || !person_by_id.contains(member_id)) {
                    return load_error(SnapshotErrorCode::InvalidReference, "Snapshot household relocated members must be known and ascending");
                }
                previous_member_id = member_id;
            }
        }
    }

    for (const auto& [settlement_id, extinct_event_id] : extinct_event_id_by_settlement) {
        const auto founded_it = founded_event_id_by_settlement.find(settlement_id);
        if (founded_it == founded_event_id_by_settlement.end()) {
            continue;
        }
        if (founded_it->second > extinct_event_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement founded/extinct event order is invalid");
        }
    }

    const auto expected_next_event_id = static_cast<EventId>(restored_state.event_log.size() + 1U);
    if (next_event_id != expected_next_event_id) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot next event id is inconsistent");
    }

    std::unordered_map<EventId, const SimulationEvent*> event_by_id;
    event_by_id.reserve(restored_state.event_log.size());
    for (const auto& event : restored_state.event_log) {
        event_by_id.emplace(event.id, &event);
    }

    std::unordered_map<MemoryId, const AgentMemory*> memory_by_id;
    memory_by_id.reserve(restored_state.agent_memories.size());
    for (const auto& memory : restored_state.agent_memories) {
        memory_by_id.emplace(memory.id, &memory);
    }

    MemoryId expected_memory_id = 1;
    std::unordered_set<DirectMemoryKey, DirectMemoryKeyHash> validated_direct_memory_keys;
    for (const auto& memory : restored_state.agent_memories) {
        if (memory.id != expected_memory_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot memory ids must be contiguous");
        }
        ++expected_memory_id;

        if (!person_by_id.contains(memory.owner_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot memory owner references unknown person");
        }

        const auto event_it = event_by_id.find(memory.source_event_id);
        if (event_it == event_by_id.end()) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot memory source event references unknown event");
        }
        const auto* source_event = event_it->second;

        if (source_event->timestamp > memory.acquired_at) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot memory acquisition must not precede source event");
        }

        if (memory.source == MemorySource::DirectObservation) {
            if (memory.acquired_at != source_event->timestamp) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot direct memory acquisition must equal source event timestamp");
            }

            const auto inserted = validated_direct_memory_keys.insert(DirectMemoryKey{
                .owner_id = memory.owner_id,
                .event_id = memory.source_event_id,
            });
            if (!inserted.second) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot duplicate direct memory for owner/event pair");
            }
        }
    }

    const auto expected_next_memory_id = static_cast<MemoryId>(restored_state.agent_memories.size() + 1U);
    if (next_memory_id != expected_next_memory_id) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot next memory id is inconsistent");
    }

    CommunicationId expected_communication_id = 1;
    std::unordered_set<Simulation::TransmissionSemanticKey, Simulation::TransmissionSemanticKeyHash> transmission_semantic_keys;
    for (const auto& transmission : restored_state.knowledge_transmissions) {
        if (transmission.id != expected_communication_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot communication ids must be contiguous");
        }
        ++expected_communication_id;

        if (!person_by_id.contains(transmission.speaker_id) || !person_by_id.contains(transmission.recipient_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot transmission references unknown person");
        }
        if (!household_by_id.contains(transmission.speaker_household_id) ||
            !household_by_id.contains(transmission.recipient_household_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot transmission references unknown household");
        }
        if (!settlement_by_id.contains(transmission.speaker_settlement_id) ||
            !settlement_by_id.contains(transmission.recipient_settlement_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot transmission references unknown settlement");
        }

        const auto speaker_memory_it = memory_by_id.find(transmission.speaker_memory_id);
        const auto recipient_memory_it = memory_by_id.find(transmission.recipient_memory_id);
        if (speaker_memory_it == memory_by_id.end() || recipient_memory_it == memory_by_id.end()) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot transmission references unknown memory");
        }

        const auto* speaker_memory = speaker_memory_it->second;
        const auto* recipient_memory = recipient_memory_it->second;
        if (speaker_memory->owner_id != transmission.speaker_id || recipient_memory->owner_id != transmission.recipient_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot transmission memory owner mismatch");
        }
        if (recipient_memory->source != MemorySource::Communication && recipient_memory->source != MemorySource::Teaching) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot transmission recipient memory must be communication-derived");
        }
        if (recipient_memory->source_event_id != speaker_memory->source_event_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot transmission source event mismatch");
        }
        if (recipient_memory->source == MemorySource::Communication && transmission.method != CommunicationMethod::Communication) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot communication memory must match transmission method");
        }
        if (recipient_memory->source == MemorySource::Teaching && transmission.method != CommunicationMethod::Teaching) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot teaching memory must match transmission method");
        }
        if (transmission.transmitted_at != recipient_memory->acquired_at) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot transmission time must match recipient memory time");
        }
        if (transmission.confidence_per_10k != recipient_memory->confidence_per_10k) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot transmission confidence must match recipient memory confidence");
        }

        const auto inserted = transmission_semantic_keys.insert(Simulation::TransmissionSemanticKey{
            .recipient_id = transmission.recipient_id,
            .speaker_memory_id = transmission.speaker_memory_id,
        });
        if (!inserted.second) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot duplicate transmission semantic key");
        }
    }

    if (next_communication_id != expected_communication_id) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot next communication id is inconsistent");
    }

    RecordId expected_record_id = 1;
    std::unordered_map<RecordId, const KnowledgeRecord*> record_by_id;
    record_by_id.reserve(restored_state.knowledge_records.size());
    std::unordered_set<RecordSemanticKey, RecordSemanticKeyHash> validated_record_semantic_keys;
    for (const auto& record : restored_state.knowledge_records) {
        if (record.id != expected_record_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot record ids must be contiguous");
        }
        ++expected_record_id;

        if (!person_by_id.contains(record.author_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot record author references unknown person");
        }
        if (!household_by_id.contains(record.author_household_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot record author household references unknown household");
        }
        if (!settlement_by_id.contains(record.settlement_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot record settlement references unknown settlement");
        }

        const auto source_memory_it = memory_by_id.find(record.source_memory_id);
        if (source_memory_it == memory_by_id.end()) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot record source memory references unknown memory");
        }
        const auto* source_memory = source_memory_it->second;
        if (source_memory->owner_id != record.author_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot record source memory owner mismatch");
        }
        if (source_memory->source_event_id != record.source_event_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot record source event mismatch");
        }
        if (source_memory->acquired_at > record.recorded_at) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot record cannot predate source memory");
        }

        const auto inserted = validated_record_semantic_keys.insert(RecordSemanticKey{
            .settlement_id = record.settlement_id,
            .source_event_id = record.source_event_id,
        });
        if (!inserted.second) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot duplicate record semantic key");
        }
        record_by_id.emplace(record.id, &record);
    }

    if (next_record_id != expected_record_id) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot next record id is inconsistent");
    }

    std::unordered_set<RecordAccessSemanticKey, RecordAccessSemanticKeyHash> validated_record_access_semantic_keys;
    std::unordered_set<MemoryId> validated_record_access_memory_ids;
    for (const auto& access : restored_state.knowledge_record_accesses) {
        if (!record_by_id.contains(access.record_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot record access references unknown record");
        }
        if (!person_by_id.contains(access.reader_id) || !household_by_id.contains(access.reader_household_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot record access references unknown reader entities");
        }
        if (!settlement_by_id.contains(access.reader_settlement_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot record access references unknown settlement");
        }

        const auto* record = record_by_id.at(access.record_id);
        if (record->settlement_id != access.reader_settlement_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot record access must remain within record settlement");
        }
        if (access.read_at < record->recorded_at) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot record access cannot predate record creation");
        }

        const auto reader_memory_it = memory_by_id.find(access.reader_memory_id);
        if (reader_memory_it == memory_by_id.end()) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot record access reader memory references unknown memory");
        }
        const auto* reader_memory = reader_memory_it->second;
        if (reader_memory->owner_id != access.reader_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot record access reader memory owner mismatch");
        }
        if (reader_memory->source != MemorySource::Record) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot record access reader memory must be record-derived");
        }
        if (reader_memory->source_event_id != record->source_event_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot record access source event mismatch");
        }
        if (reader_memory->acquired_at != access.read_at) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot record access read time must match reader memory time");
        }
        if (reader_memory->confidence_per_10k != access.confidence_per_10k) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot record access confidence must match reader memory");
        }

        const auto access_inserted = validated_record_access_semantic_keys.insert(RecordAccessSemanticKey{
            .record_id = access.record_id,
            .reader_id = access.reader_id,
        });
        if (!access_inserted.second) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot duplicate record access semantic key");
        }
        if (!validated_record_access_memory_ids.insert(access.reader_memory_id).second) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot duplicate record access reader memory");
        }
    }

    std::unordered_map<InstitutionId, const InstitutionFoundedEventPayload*> founded_event_by_institution;
    for (const auto& event : restored_state.event_log) {
        if (event.type != EventType::InstitutionFounded) {
            continue;
        }
        const auto& founded_payload = std::get<InstitutionFoundedEventPayload>(event.payload);
        if (founded_event_by_institution.contains(founded_payload.institution_id)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot institution has duplicate founded events");
        }
        founded_event_by_institution.emplace(founded_payload.institution_id, &founded_payload);
    }

    InstitutionId expected_institution_id = 1;
    for (const auto& institution : restored_state.institutions) {
        if (institution.id != expected_institution_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot institution ids must be contiguous");
        }
        ++expected_institution_id;

        if (institution.kind != InstitutionKind::KnowledgeKeeper) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot institution kind is invalid");
        }
        if (!settlement_by_id.contains(institution.settlement_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot institution references unknown settlement");
        }
        if (!person_by_id.contains(institution.founder_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot institution founder references unknown person");
        }
        if (institution.founded_at.ticks < 0 || institution.founded_at.ticks > restored_state.time.ticks) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot institution founding time is out of range");
        }

        std::unordered_set<PersonId> unique_members;
        for (const auto member_id : institution.member_ids) {
            if (!is_valid_person_id(member_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot institution member id must be non-zero");
            }
            if (!unique_members.insert(member_id).second) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot institution has duplicate member ids");
            }
            const auto person_it = person_by_id.find(member_id);
            if (person_it == person_by_id.end() || person_it->second == nullptr || !person_it->second->alive) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot institution member must reference alive person");
            }
            const auto household_it = household_by_id.find(person_it->second->household_id);
            if (household_it == household_by_id.end() || household_it->second == nullptr) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot institution member household is invalid");
            }
            if (household_it->second->settlement_id != institution.settlement_id) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot institution member must be local to institution settlement");
            }
        }

        std::unordered_set<RecordId> unique_records;
        for (const auto record_id : institution.preserved_record_ids) {
            if (!is_valid_record_id(record_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot institution preserved record id must be non-zero");
            }
            if (!unique_records.insert(record_id).second) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot institution has duplicate preserved record ids");
            }
            const auto record_it = record_by_id.find(record_id);
            if (record_it == record_by_id.end() || record_it->second == nullptr) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot institution preserved record references unknown record");
            }
            if (record_it->second->settlement_id != institution.settlement_id) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot institution preserved record must match institution settlement");
            }
        }

        std::unordered_set<std::uint32_t> unique_discoveries;
        for (const auto discovery_key : institution.preserved_discovery_keys) {
            if (discovery_key == 0U) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot institution preserved discovery key must be non-zero");
            }
            if (!unique_discoveries.insert(discovery_key).second) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot institution has duplicate preserved discovery keys");
            }
        }

        if (!founded_event_by_institution.contains(institution.id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot institution must have InstitutionFounded event");
        }
        const auto* founded_payload = founded_event_by_institution.at(institution.id);
        if (founded_payload->kind != institution.kind ||
            founded_payload->settlement_id != institution.settlement_id ||
            founded_payload->founder_id != institution.founder_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot institution founded event payload mismatch");
        }
    }

    if (next_institution_id != expected_institution_id) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot next institution id is inconsistent");
    }

    BuildingId max_building_id = 0;
    for (const auto& building : restored_state.buildings) {
        max_building_id = std::max(max_building_id, building.id);
    }
    const auto expected_next_building_id = (max_building_id == 0U) ? 1U : (max_building_id + 1U);
    if (next_building_id != expected_next_building_id) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot next building id is inconsistent");
    }

    std::unordered_set<KnowledgeSemanticKey, KnowledgeSemanticKeyHash> knowledge_semantic_keys;
    KnowledgeId expected_knowledge_id = 1;
    for (const auto& knowledge : restored_state.agent_knowledge) {
        if (knowledge.id != expected_knowledge_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot knowledge ids must be contiguous");
        }
        ++expected_knowledge_id;

        if (!person_by_id.contains(knowledge.owner_id)) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot knowledge owner references unknown person");
        }
        if (knowledge.acquired_at.ticks < 0 || knowledge.updated_at.ticks < 0) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot knowledge times must be non-negative");
        }
        if (knowledge.updated_at < knowledge.acquired_at) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot knowledge updated_at must not precede acquired_at");
        }
        if (knowledge.updated_at.ticks > restored_state.time.ticks) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot knowledge updated_at exceeds simulation time");
        }
        if (knowledge.confidence_per_10k > 10'000U) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot knowledge confidence is out of range");
        }
        if (knowledge.source != KnowledgeSource::MemoryDerived) {
            return load_error(SnapshotErrorCode::InvalidEnum, "Snapshot knowledge source enum is invalid");
        }

        const auto source_memory_it = memory_by_id.find(knowledge.source_memory_id);
        if (source_memory_it == memory_by_id.end()) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot knowledge source memory references unknown memory");
        }
        const auto* source_memory = source_memory_it->second;
        if (source_memory->owner_id != knowledge.owner_id) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot knowledge source memory owner mismatch");
        }
        if (source_memory->acquired_at > knowledge.updated_at) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot knowledge source memory occurs after knowledge update");
        }
        if (source_memory->acquired_at != knowledge.updated_at) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot knowledge updated_at must match source memory acquisition");
        }

        const auto source_event_it = event_by_id.find(source_memory->source_event_id);
        if (source_event_it == event_by_id.end()) {
            return load_error(SnapshotErrorCode::InvalidReference, "Snapshot knowledge source memory references unknown event");
        }
        const auto* source_event = source_event_it->second;

        KnowledgeSemanticKey semantic_key{};
        semantic_key.owner_id = knowledge.owner_id;

        if (const auto* fact = std::get_if<PersonLifeStatusKnowledgeFact>(&knowledge.fact); fact != nullptr) {
            if (!is_valid_person_id(fact->person_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot person-life knowledge references invalid person id");
            }
            if (!person_by_id.contains(fact->person_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot person-life knowledge references unknown person");
            }
            semantic_key.kind = KnowledgeFactKind::PersonLifeStatus;
            semantic_key.primary_subject_id = fact->person_id;

            if (fact->status == KnownLifeStatus::Alive) {
                if (source_event->type != EventType::Birth) {
                    return load_error(SnapshotErrorCode::InvalidState, "Alive life-status knowledge must derive from Birth event");
                }
                const auto& source_payload = std::get<BirthEventPayload>(source_event->payload);
                if (source_payload.child_id != fact->person_id) {
                    return load_error(SnapshotErrorCode::InvalidState, "Alive life-status knowledge subject/event mismatch");
                }
            } else {
                if (source_event->type != EventType::Death) {
                    return load_error(SnapshotErrorCode::InvalidState, "Dead life-status knowledge must derive from Death event");
                }
                const auto& source_payload = std::get<DeathEventPayload>(source_event->payload);
                if (source_payload.person_id != fact->person_id) {
                    return load_error(SnapshotErrorCode::InvalidState, "Dead life-status knowledge subject/event mismatch");
                }
            }
        } else if (const auto* household_fact = std::get_if<PersonHouseholdKnowledgeFact>(&knowledge.fact); household_fact != nullptr) {
            if (!is_valid_person_id(household_fact->person_id) || !is_valid_household_id(household_fact->household_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot person-household knowledge references invalid ids");
            }
            if (!person_by_id.contains(household_fact->person_id) || !household_by_id.contains(household_fact->household_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot person-household knowledge references unknown entities");
            }
            semantic_key.kind = KnowledgeFactKind::PersonHousehold;
            semantic_key.primary_subject_id = household_fact->person_id;

            if (source_event->type != EventType::Birth) {
                return load_error(SnapshotErrorCode::InvalidState, "Person-household knowledge must derive from Birth event");
            }
            const auto& source_payload = std::get<BirthEventPayload>(source_event->payload);
            if (source_payload.child_id != household_fact->person_id || source_payload.household_id != household_fact->household_id) {
                return load_error(SnapshotErrorCode::InvalidState, "Person-household knowledge subject/event mismatch");
            }
        } else if (const auto* settlement_fact = std::get_if<SettlementStatusKnowledgeFact>(&knowledge.fact); settlement_fact != nullptr) {
            if (!is_valid_settlement_id(settlement_fact->settlement_id) || settlement_fact->region_id == 0U) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot settlement-status knowledge references invalid ids");
            }
            if (!settlement_by_id.contains(settlement_fact->settlement_id) || !region_by_id.contains(settlement_fact->region_id)) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot settlement-status knowledge references unknown entities");
            }
            semantic_key.kind = KnowledgeFactKind::SettlementStatus;
            semantic_key.primary_subject_id = settlement_fact->settlement_id;

            if (settlement_fact->active) {
                if (source_event->type != EventType::SettlementFounded) {
                    return load_error(SnapshotErrorCode::InvalidState, "Active settlement knowledge must derive from SettlementFounded event");
                }
                const auto& source_payload = std::get<SettlementFoundedEventPayload>(source_event->payload);
                if (source_payload.settlement_id != settlement_fact->settlement_id || source_payload.region_id != settlement_fact->region_id) {
                    return load_error(SnapshotErrorCode::InvalidState, "Settlement founded knowledge subject/event mismatch");
                }
            } else {
                if (source_event->type != EventType::SettlementExtinct) {
                    return load_error(SnapshotErrorCode::InvalidState, "Inactive settlement knowledge must derive from SettlementExtinct event");
                }
                const auto& source_payload = std::get<SettlementExtinctEventPayload>(source_event->payload);
                if (source_payload.settlement_id != settlement_fact->settlement_id || source_payload.region_id != settlement_fact->region_id) {
                    return load_error(SnapshotErrorCode::InvalidState, "Settlement extinct knowledge subject/event mismatch");
                }
            }
        } else if (const auto* discovery_fact = std::get_if<BasicDiscoveryKnowledgeFact>(&knowledge.fact); discovery_fact != nullptr) {
            semantic_key.kind = KnowledgeFactKind::BasicDiscovery;
            semantic_key.primary_subject_id = discovery_fact->discovery_key;

            if (source_event->type != EventType::DiscoveryMade) {
                return load_error(SnapshotErrorCode::InvalidState, "Basic discovery knowledge must derive from DiscoveryMade event");
            }
            const auto& source_payload = std::get<DiscoveryMadeEventPayload>(source_event->payload);
            if (source_payload.discovery_key != discovery_fact->discovery_key) {
                return load_error(SnapshotErrorCode::InvalidState, "Basic discovery knowledge subject/event mismatch");
            }
        } else {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot knowledge fact variant is invalid");
        }

        const auto inserted = knowledge_semantic_keys.insert(semantic_key);
        if (!inserted.second) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot duplicate knowledge semantic key for owner");
        }
    }

    const auto expected_next_knowledge_id = static_cast<KnowledgeId>(restored_state.agent_knowledge.size() + 1U);
    if (next_knowledge_id != expected_next_knowledge_id) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot next knowledge id is inconsistent");
    }

    if (restored_state.aggregate_births_total > restored_state.total_births ||
        restored_state.aggregate_deaths_total > restored_state.total_deaths) {
        return load_error(SnapshotErrorCode::InvalidState, "Snapshot aggregate demography exceeds canonical totals");
    }

    // Aggregate cohort demography is intentionally event-free, so the journal
    // reconciles against the detailed portion of the canonical totals.
    if (restored_state.total_births - restored_state.aggregate_births_total != birth_event_count ||
        restored_state.total_deaths - restored_state.aggregate_deaths_total != death_event_count) {
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

    for (const auto& settlement : restored_state.settlements) {
        total_food_stored += std::max<std::int64_t>(0, settlement.commons_food.value);
    }

    for (const auto& person : restored_state.people) {
        if (!person.alive) {
            continue;
        }

        if (person.nutrition_points < 550 || person.health_points < 500) {
            ++population_under_food_stress;
        }
    }

    for (const auto& aggregate : restored_state.settlement_aggregates) {
        total_food_stored += std::max<std::int64_t>(0, aggregate.food_inventory);
        total_food_produced += std::max<std::int64_t>(0, aggregate.food_produced_total);
        total_food_consumed += std::max<std::int64_t>(0, aggregate.food_consumed_total);
        total_unmet_food += std::max<std::int64_t>(0, aggregate.unmet_food_total);
        if (aggregate.unmet_food_total > 0) {
            ++households_in_shortage;
            const auto stressed_households = std::min<std::uint64_t>(
                aggregate.household_count,
                static_cast<std::uint64_t>(aggregate.unmet_food_total / 2));
            population_under_food_stress += std::min<std::uint64_t>(
                stressed_households * 5U,
                aggregate.household_count * 5U);
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

    std::int64_t recomputed_total_household_wealth = 0;
    std::int64_t recomputed_total_settlement_commons_food = 0;
    std::int64_t recomputed_total_exchange_volume = 0;
    std::int64_t recomputed_total_transfer_volume = 0;
    std::uint64_t recomputed_active_buildings = 0;
    std::uint64_t recomputed_active_projects = 0;
    std::unordered_map<SettlementId, std::uint64_t> completed_dwellings_by_settlement;
    std::unordered_map<SettlementId, std::uint64_t> completed_food_stores_by_settlement;
    std::unordered_map<SettlementId, std::uint64_t> completed_workshops_by_settlement;

    for (const auto& household : restored_state.households) {
        if (household.owned_materials.wood.value < 0 ||
            household.owned_materials.stone.value < 0 ||
            household.owned_materials.timber.value < 0 ||
            household.owned_materials.basic_tool.value < 0 ||
            household.productive_access_share_points > 10'000U ||
            household.commons_access_share_points > 10'000U ||
            household.cumulative_food_transferred_in < 0 ||
            household.cumulative_food_transferred_out < 0 ||
            household.cumulative_material_transferred_in < 0 ||
            household.cumulative_material_transferred_out < 0 ||
            household.derived_wealth_points < 0) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot household economic values are out of range");
        }
        const auto recomputed_household_wealth =
            std::max<std::int64_t>(0, household.resources.food_inventory.value) +
            std::max<std::int64_t>(0, household.owned_materials.wood.value) * kWealthValueWood +
            std::max<std::int64_t>(0, household.owned_materials.stone.value) * kWealthValueStone +
            std::max<std::int64_t>(0, household.owned_materials.timber.value) * kWealthValueTimber +
            std::max<std::int64_t>(0, household.owned_materials.basic_tool.value) * kWealthValueBasicTool;
        if (household.derived_wealth_points != recomputed_household_wealth) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot household derived wealth is inconsistent");
        }
        recomputed_total_household_wealth += recomputed_household_wealth;
    }

    std::unordered_map<SettlementId, std::uint32_t> productive_access_by_settlement;
    std::unordered_map<SettlementId, std::uint32_t> commons_access_by_settlement;
    std::unordered_map<SettlementId, std::int64_t> owned_wood_by_settlement;
    std::unordered_map<SettlementId, std::int64_t> owned_stone_by_settlement;
    std::unordered_map<SettlementId, std::int64_t> owned_timber_by_settlement;
    std::unordered_map<SettlementId, std::int64_t> owned_tools_by_settlement;
    for (const auto& household : restored_state.households) {
        productive_access_by_settlement[household.settlement_id] += household.productive_access_share_points;
        commons_access_by_settlement[household.settlement_id] += household.commons_access_share_points;
        owned_wood_by_settlement[household.settlement_id] += household.owned_materials.wood.value;
        owned_stone_by_settlement[household.settlement_id] += household.owned_materials.stone.value;
        owned_timber_by_settlement[household.settlement_id] += household.owned_materials.timber.value;
        owned_tools_by_settlement[household.settlement_id] += household.owned_materials.basic_tool.value;
    }
    for (const auto& [settlement_id, share] : productive_access_by_settlement) {
        (void)settlement_id;
        if (share > 10'000U) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot productive access allocation exceeds settlement bounds");
        }
    }
    for (const auto& [settlement_id, share] : commons_access_by_settlement) {
        (void)settlement_id;
        if (share > 10'000U) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot commons access allocation exceeds settlement bounds");
        }
    }

    for (const auto& settlement : restored_state.settlements) {
        if (settlement.commons_food.value < 0 ||
            settlement.last_transfer_food_volume < 0 ||
            settlement.last_exchange_volume < 0 ||
            settlement.dwelling_capacity_bonus < 0 ||
            settlement.food_store_capacity_bonus < 0 ||
            settlement.workshop_capacity_bonus < 0 ||
            settlement.last_median_household_wealth < 0 ||
            settlement.last_min_household_wealth < 0 ||
            settlement.last_max_household_wealth < 0) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement economic values are out of range");
        }

        if (cohort_population == 0U &&
            (owned_wood_by_settlement[settlement.id] != settlement.materials.wood.value ||
             owned_stone_by_settlement[settlement.id] != settlement.materials.stone.value ||
             owned_timber_by_settlement[settlement.id] != settlement.materials.timber.value ||
             owned_tools_by_settlement[settlement.id] != settlement.materials.basic_tool.value)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot household ownership partition does not match settlement inventory");
        }

        recomputed_total_settlement_commons_food += settlement.commons_food.value;
        recomputed_total_exchange_volume += settlement.last_exchange_volume;
        recomputed_total_transfer_volume += settlement.last_transfer_food_volume;
    }

    for (const auto& building : restored_state.buildings) {
        if (building.state == ConstructionState::Complete) {
            ++recomputed_active_buildings;
            if (building.type == BuildingType::Dwelling) {
                ++completed_dwellings_by_settlement[building.settlement_id];
            } else if (building.type == BuildingType::FoodStore) {
                ++completed_food_stores_by_settlement[building.settlement_id];
            } else if (building.type == BuildingType::Workshop) {
                ++completed_workshops_by_settlement[building.settlement_id];
            }
        } else {
            ++recomputed_active_projects;
        }
    }

    for (const auto& settlement : restored_state.settlements) {
        const auto dwellings = completed_dwellings_by_settlement.contains(settlement.id)
            ? completed_dwellings_by_settlement[settlement.id]
            : 0U;
        const auto food_stores = completed_food_stores_by_settlement.contains(settlement.id)
            ? completed_food_stores_by_settlement[settlement.id]
            : 0U;
        const auto workshops = completed_workshops_by_settlement.contains(settlement.id)
            ? completed_workshops_by_settlement[settlement.id]
            : 0U;

        if (dwellings > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / kDwellingCapacityBonusPerBuilding) ||
            food_stores > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / kFoodStoreCapacityBonusPerBuilding) ||
            workshops > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / kWorkshopCapacityBonusPerBuilding)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement building bonus overflows supported range");
        }

        const auto expected_dwelling_bonus = static_cast<std::int64_t>(dwellings) * kDwellingCapacityBonusPerBuilding;
        const auto expected_food_store_bonus = static_cast<std::int64_t>(food_stores) * kFoodStoreCapacityBonusPerBuilding;
        const auto expected_workshop_bonus = static_cast<std::int64_t>(workshops) * kWorkshopCapacityBonusPerBuilding;
        if (settlement.dwelling_capacity_bonus != expected_dwelling_bonus ||
            settlement.food_store_capacity_bonus != expected_food_store_bonus ||
            settlement.workshop_capacity_bonus != expected_workshop_bonus) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot settlement building bonus totals are inconsistent");
        }
    }

    restored_state.total_household_wealth = recomputed_total_household_wealth;
    restored_state.total_settlement_commons_food = recomputed_total_settlement_commons_food;
    restored_state.total_exchange_volume = recomputed_total_exchange_volume;
    restored_state.total_transfer_volume = recomputed_total_transfer_volume;
    restored_state.active_buildings = recomputed_active_buildings;
    restored_state.active_projects = recomputed_active_projects;

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
    std::sort(restored_state.agent_memories.begin(), restored_state.agent_memories.end(), [](const AgentMemory& lhs, const AgentMemory& rhs) {
        return lhs.id < rhs.id;
    });
    std::sort(restored_state.agent_knowledge.begin(), restored_state.agent_knowledge.end(), [](const AgentKnowledge& lhs, const AgentKnowledge& rhs) {
        return lhs.id < rhs.id;
    });
    std::sort(restored_state.knowledge_transmissions.begin(), restored_state.knowledge_transmissions.end(), [](const KnowledgeTransmission& lhs, const KnowledgeTransmission& rhs) {
        return lhs.id < rhs.id;
    });
    std::sort(restored_state.knowledge_records.begin(), restored_state.knowledge_records.end(), [](const KnowledgeRecord& lhs, const KnowledgeRecord& rhs) {
        return lhs.id < rhs.id;
    });
    std::sort(restored_state.knowledge_record_accesses.begin(), restored_state.knowledge_record_accesses.end(), [](const KnowledgeRecordAccess& lhs, const KnowledgeRecordAccess& rhs) {
        if (lhs.record_id != rhs.record_id) {
            return lhs.record_id < rhs.record_id;
        }
        return lhs.reader_id < rhs.reader_id;
    });
    std::sort(restored_state.institutions.begin(), restored_state.institutions.end(), [](const Institution& lhs, const Institution& rhs) {
        return lhs.id < rhs.id;
    });
    std::sort(restored_state.region_lod_states.begin(), restored_state.region_lod_states.end(), [](const RegionLodState& lhs, const RegionLodState& rhs) {
        return lhs.region_id < rhs.region_id;
    });
    std::sort(restored_state.population_cohorts.begin(), restored_state.population_cohorts.end(), [](const PopulationCohort& lhs, const PopulationCohort& rhs) {
        if (lhs.region_id != rhs.region_id) {
            return lhs.region_id < rhs.region_id;
        }
        if (lhs.settlement_id != rhs.settlement_id) {
            return lhs.settlement_id < rhs.settlement_id;
        }
        return lhs.id < rhs.id;
    });
    std::sort(restored_state.settlement_aggregates.begin(), restored_state.settlement_aggregates.end(), [](const SettlementAggregateState& lhs, const SettlementAggregateState& rhs) {
        return lhs.settlement_id < rhs.settlement_id;
    });
    for (auto& institution : restored_state.institutions) {
        std::sort(institution.member_ids.begin(), institution.member_ids.end());
        std::sort(institution.preserved_record_ids.begin(), institution.preserved_record_ids.end());
        std::sort(institution.preserved_discovery_keys.begin(), institution.preserved_discovery_keys.end());
    }

    std::unordered_map<PersonId, std::vector<std::size_t>> rebuilt_knowledge_indices_by_owner;
    std::unordered_map<Simulation::KnowledgeSemanticKey, std::size_t, Simulation::KnowledgeSemanticKeyHash> rebuilt_knowledge_index_by_semantic_key;
    rebuilt_knowledge_indices_by_owner.reserve(restored_state.agent_knowledge.size());
    rebuilt_knowledge_index_by_semantic_key.reserve(restored_state.agent_knowledge.size());
    for (std::size_t index = 0; index < restored_state.agent_knowledge.size(); ++index) {
        const auto& knowledge = restored_state.agent_knowledge[index];
        Simulation::KnowledgeSemanticKey semantic_key{};
        semantic_key.owner_id = knowledge.owner_id;
        semantic_key.secondary_subject_id = 0U;

        if (const auto* life_fact = std::get_if<PersonLifeStatusKnowledgeFact>(&knowledge.fact); life_fact != nullptr) {
            semantic_key.fact_kind = KnowledgeFactKind::PersonLifeStatus;
            semantic_key.primary_subject_id = life_fact->person_id;
        } else if (const auto* household_fact = std::get_if<PersonHouseholdKnowledgeFact>(&knowledge.fact); household_fact != nullptr) {
            semantic_key.fact_kind = KnowledgeFactKind::PersonHousehold;
            semantic_key.primary_subject_id = household_fact->person_id;
        } else if (const auto* settlement_fact = std::get_if<SettlementStatusKnowledgeFact>(&knowledge.fact); settlement_fact != nullptr) {
            semantic_key.fact_kind = KnowledgeFactKind::SettlementStatus;
            semantic_key.primary_subject_id = settlement_fact->settlement_id;
        } else if (const auto* discovery_fact = std::get_if<BasicDiscoveryKnowledgeFact>(&knowledge.fact); discovery_fact != nullptr) {
            semantic_key.fact_kind = KnowledgeFactKind::BasicDiscovery;
            semantic_key.primary_subject_id = discovery_fact->discovery_key;
        } else {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot knowledge fact variant is invalid for index rebuild");
        }

        const auto inserted = rebuilt_knowledge_index_by_semantic_key.emplace(semantic_key, index);
        if (!inserted.second) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot duplicate knowledge semantic key for index rebuild");
        }
        rebuilt_knowledge_indices_by_owner[knowledge.owner_id].push_back(index);
    }

    destination.config_ = restored_config;
    destination.state_ = std::move(restored_state);
    destination.simulation_seed_ = SimulationSeed{destination.state_.seed};
    destination.next_person_id_ = next_person_id;
    destination.next_household_id_ = next_household_id;
    destination.next_region_id_ = next_region_id;
    destination.next_settlement_id_ = next_settlement_id;
    destination.next_event_id_ = next_event_id;
    destination.next_memory_id_ = next_memory_id;
    destination.next_knowledge_id_ = next_knowledge_id;
    destination.next_communication_id_ = next_communication_id;
    destination.next_record_id_ = next_record_id;
    destination.next_institution_id_ = next_institution_id;
    destination.next_building_id_ = next_building_id;
    destination.next_cohort_id_ = next_cohort_id;
    destination.demography_interval_ = SimulationDuration{demography_interval_ticks};
    destination.demography_next_run_ = SimulationTime{demography_next_run_ticks};
    destination.food_interval_ = SimulationDuration{food_interval_ticks};
    destination.food_next_run_ = SimulationTime{food_next_run_ticks};
    destination.knowledge_indices_by_owner_ = std::move(rebuilt_knowledge_indices_by_owner);
    destination.knowledge_index_by_semantic_key_ = std::move(rebuilt_knowledge_index_by_semantic_key);
    destination.rebuild_communication_indexes();
    destination.rebuild_record_indexes();

    // v24 -> v25 migration. Placement is a pure function of settlement
    // geography and lower-id buildings, so replaying it reproduces what v25
    // would have placed. v24 recorded no occupation history, so every living
    // person's next reassignment is treated as a resolution, never a change.
    if (is_v24_migration) {
        destination.recompute_building_placements();
        for (auto& person : destination.state_.people) {
            person.occupation_history_resync_pending = person.alive;
        }
    }

    return SnapshotLoadResult{.ok = true, .code = SnapshotErrorCode::None, .message = ""};
}

}  // namespace universe
