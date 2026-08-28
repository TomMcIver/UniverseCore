#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace universe {

class Simulation;

enum class SnapshotErrorCode : std::uint8_t {
    None = 0,
    InvalidMagic = 1,
    UnsupportedVersion = 2,
    TruncatedData = 3,
    InvalidChecksum = 4,
    InvalidStructure = 5,
    InvalidEnum = 6,
    DuplicateId = 7,
    InvalidReference = 8,
    InvalidState = 9,
};

struct SnapshotLoadResult {
    bool ok{false};
    SnapshotErrorCode code{SnapshotErrorCode::InvalidStructure};
    std::string message{};
};

class SnapshotCodec {
public:
    static constexpr std::uint16_t kSchemaVersion = 17;

    [[nodiscard]] static std::vector<std::byte> serialize(const Simulation& simulation);
    [[nodiscard]] static SnapshotLoadResult deserialize(std::span<const std::byte> data, Simulation& destination);
};

[[nodiscard]] std::uint64_t snapshot_hash64(std::span<const std::byte> data) noexcept;

}  // namespace universe
