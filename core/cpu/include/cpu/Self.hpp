#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace Cpu {

struct DecodedSelf {
    std::vector<std::byte> Bytes;
    std::vector<std::string> NormalizationNotes;
};

bool IsSelf(std::span<const std::byte> input) noexcept;
DecodedSelf DecodePlainSelf(std::span<const std::byte> input);

}
