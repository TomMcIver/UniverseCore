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
    payload.write_u64(simulation.next_person_id_);
    payload.write_u64(simulation.next_household_id_);
    payload.write_u64(simulation.next_event_id_);
    payload.write_i64(simulation.demography_interval_.ticks);
    payload.write_i64(simulation.demography_next_run_.ticks);
    payload.write_u64(simulation.config_.seed);
    payload.write_u64(simulation.config_.initial_population);
    payload.write_bool(simulation.config_.enable_unrelated_random_probe);

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

        std::vector<PersonId> member_ids = household->member_ids;
        std::sort(member_ids.begin(), member_ids.end());
        payload.write_u64(static_cast<std::uint64_t>(member_ids.size()));
        for (const auto member_id : member_ids) {
            payload.write_u64(member_id);
        }
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
    std::uint64_t next_event_id = 0;
    std::int64_t demography_interval_ticks = 0;
    std::int64_t demography_next_run_ticks = 0;

    if (!payload_reader.read_u64(restored_state.seed) ||
        !payload_reader.read_i64(restored_state.time.ticks) ||
        !payload_reader.read_u64(restored_state.year) ||
        !payload_reader.read_u64(restored_state.population) ||
        !payload_reader.read_u64(restored_state.total_births) ||
        !payload_reader.read_u64(restored_state.total_deaths) ||
        !payload_reader.read_u64(next_person_id) ||
        !payload_reader.read_u64(next_household_id) ||
        !payload_reader.read_u64(next_event_id) ||
        !payload_reader.read_i64(demography_interval_ticks) ||
        !payload_reader.read_i64(demography_next_run_ticks) ||
        !payload_reader.read_u64(restored_config.seed) ||
        !payload_reader.read_u64(restored_config.initial_population) ||
        !payload_reader.read_bool(restored_config.enable_unrelated_random_probe)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot fixed section is truncated");
    }

    std::uint64_t people_count = 0;
    if (!payload_reader.read_u64(people_count)) {
        return load_error(SnapshotErrorCode::TruncatedData, "Snapshot people section is truncated");
    }

    if (people_count > payload_reader.remaining() / 28U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot people section has invalid length");
    }

    restored_state.people.clear();
    restored_state.people.reserve(static_cast<std::size_t>(people_count));
    std::unordered_set<PersonId> person_ids;

    for (std::uint64_t i = 0; i < people_count; ++i) {
        Person person{};
        std::uint8_t sex_raw = 0;
        bool has_mother = false;
        bool has_father = false;

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

    if (household_count > payload_reader.remaining() / 16U) {
        return load_error(SnapshotErrorCode::InvalidStructure, "Snapshot household section has invalid length");
    }

    restored_state.households.clear();
    restored_state.households.reserve(static_cast<std::size_t>(household_count));
    std::unordered_set<HouseholdId> household_ids;

    for (std::uint64_t i = 0; i < household_count; ++i) {
        Household household{};
        std::uint64_t member_count = 0;
        if (!payload_reader.read_u64(household.id) || !payload_reader.read_u64(member_count)) {
            return load_error(SnapshotErrorCode::TruncatedData, "Snapshot household record is truncated");
        }

        if (!is_valid_household_id(household.id)) {
            return load_error(SnapshotErrorCode::InvalidState, "Snapshot household id must be non-zero");
        }
        if (!household_ids.insert(household.id).second) {
            return load_error(SnapshotErrorCode::DuplicateId, "Snapshot contains duplicate household ids");
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

    std::unordered_map<PersonId, const Person*> person_by_id;
    person_by_id.reserve(restored_state.people.size());
    for (const auto& person : restored_state.people) {
        person_by_id.emplace(person.id, &person);
    }

    std::unordered_map<HouseholdId, const Household*> household_by_id;
    household_by_id.reserve(restored_state.households.size());
    for (const auto& household : restored_state.households) {
        household_by_id.emplace(household.id, &household);
    }

    std::unordered_map<PersonId, std::uint32_t> membership_counts;
    membership_counts.reserve(restored_state.people.size());

    for (const auto& household : restored_state.households) {
        for (const auto member_id : household.member_ids) {
            const auto person_it = person_by_id.find(member_id);
            if (person_it == person_by_id.end()) {
                return load_error(SnapshotErrorCode::InvalidReference, "Snapshot household member id references unknown person");
            }
            if (!person_it->second->alive) {
                return load_error(SnapshotErrorCode::InvalidState, "Snapshot household member references a dead person");
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

    std::sort(restored_state.people.begin(), restored_state.people.end(), [](const Person& lhs, const Person& rhs) {
        return lhs.id < rhs.id;
    });
    std::sort(restored_state.households.begin(), restored_state.households.end(), [](const Household& lhs, const Household& rhs) {
        return lhs.id < rhs.id;
    });
    for (auto& household : restored_state.households) {
        std::sort(household.member_ids.begin(), household.member_ids.end());
    }
    std::sort(restored_state.event_log.begin(), restored_state.event_log.end(), [](const SimulationEvent& lhs, const SimulationEvent& rhs) {
        return lhs.id < rhs.id;
    });

    destination.config_ = restored_config;
    destination.state_ = std::move(restored_state);
    destination.simulation_seed_ = SimulationSeed{destination.state_.seed};
    destination.next_person_id_ = next_person_id;
    destination.next_household_id_ = next_household_id;
    destination.next_event_id_ = next_event_id;
    destination.demography_interval_ = SimulationDuration{demography_interval_ticks};
    destination.demography_next_run_ = SimulationTime{demography_next_run_ticks};

    return SnapshotLoadResult{.ok = true, .code = SnapshotErrorCode::None, .message = ""};
}

}  // namespace universe
