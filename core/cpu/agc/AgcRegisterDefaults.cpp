#include "SceShaders.hpp"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

// Upstream sceAgcGetRegisterDefaults2 returns a host table of host pointers. The
// guest needs the same table at guest addresses, so it is laid out again here from
// the upstream compact data (the same header upstream builds its table from).
namespace Upstream {
struct CompactBank { ShaderRegister* registers; std::uint32_t registerCount; const std::uint16_t* offsets; std::uint32_t pointerCount; };
struct CompactRegisterDefaults { CompactBank banks[4]; std::uint32_t* types; std::uint32_t count; };
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-braces"
#include "prx/libSceAgc/Command/include/RegisterDefaultsData.hpp"
#pragma clang diagnostic pop
}

namespace Cpu {
namespace {
// Guest RegisterDefaults: tables[4] (ShaderRegister**), registerCounts[4], types, count.
struct GuestRegisterDefaults {
    std::uint64_t Tables[4];
    std::uint32_t RegisterCounts[4];
    std::uint64_t Types;
    std::uint32_t Count;
};
static_assert(sizeof(GuestRegisterDefaults) == 64 && offsetof(GuestRegisterDefaults, Count) == 0x38);
std::size_t align8(std::size_t value) { return (value + 7) & ~std::size_t{7}; }
}

std::vector<std::byte> AgcRegisterDefaultsGuestImage(std::uint32_t version, bool internal, std::uint64_t base) {
    if (version >= 14) throw std::runtime_error("SCE AGC: unsupported register defaults version");
    const auto* source = (internal ? Upstream::agcInternalRegDefaultsByVersion : Upstream::agcPublicRegDefaultsByVersion)[version];
    std::vector<std::byte> image(sizeof(GuestRegisterDefaults));
    const auto append = [&](const void* data, std::size_t bytes) {
        const auto at = align8(image.size());
        image.resize(at + bytes);
        if (bytes) std::memcpy(image.data() + at, data, bytes);
        return base + at;
    };
    GuestRegisterDefaults header{};
    for (std::size_t bank = 0; bank < 4; ++bank) {
        const auto& input = source->banks[bank];
        header.RegisterCounts[bank] = input.registerCount;
        if (!input.pointerCount) continue;
        const auto registers = append(input.registers, std::size_t{input.registerCount} * sizeof(ShaderRegister));
        std::vector<std::uint64_t> pointers;
        for (std::uint32_t index = 0; index < input.pointerCount; ++index) {
            if (input.offsets[index] >= input.registerCount) throw std::runtime_error("SCE AGC: register defaults pointer exceeds bank");
            pointers.push_back(registers + std::uint64_t{input.offsets[index]} * sizeof(ShaderRegister));
        }
        header.Tables[bank] = append(pointers.data(), pointers.size() * sizeof(std::uint64_t));
    }
    header.Count = source->count;
    if (source->count) header.Types = append(source->types, std::size_t{source->count} * sizeof(std::uint32_t));
    std::memcpy(image.data(), &header, sizeof(header));
    return image;
}
}
