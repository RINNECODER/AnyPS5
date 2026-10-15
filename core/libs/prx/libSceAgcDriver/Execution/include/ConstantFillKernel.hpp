#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace AgcDriver {

// A dispatch of the guest's constant buffer fill kernel (the kernel the Vulkan driver runs as a
// buffer fill, Driver::matchesFillKernel): invocation i of groups[0] * 64 stores the 16 bytes of
// user data 4-7 to record i of the buffer in user data 0-3 (stride 16, format 32_32_32_32, records
// at or past num_records dropped). Games clear DCC keys with it. The bytes it stores are known
// before it runs: [base, base + bytes) holds `pattern` repeated. A dispatch whose last group is
// partial (`partialThreads`, a thread-count DISPATCH_DIRECT not divisible by 64) is not matched, as on
// the Vulkan path: its inactive lanes store nothing.
struct ConstantFill {
    std::uint64_t base = 0;
    std::uint64_t bytes = 0;
    std::array<std::uint32_t, 4> pattern{};
};

inline std::optional<ConstantFill> MatchConstantFill(std::span<const std::uint32_t> code, std::span<const std::uint32_t> userData,
    const std::array<std::uint32_t, 3>& numThreads, const std::array<std::uint32_t, 3>& groups,
    const std::array<std::uint32_t, 3>& partialThreads) {
    // index = group id * 64 + thread id; v0..v3 = user data 4-7; an idxen buffer store of v0..v3; s_endpgm
    static constexpr std::array<std::uint32_t, 9> Kernel{0xd7460004u, 0x04010c08u, 0x7e000204u, 0x7e020205u, 0x7e040206u,
        0x7e060207u, 0xe01c2000u, 0x80000004u, 0xbf810000u};
    if (userData.size() < 8 || numThreads != std::array<std::uint32_t, 3>{64, 1, 1} || groups[1] != 1 || groups[2] != 1 ||
        partialThreads != std::array<std::uint32_t, 3>{}) return std::nullopt;
    if (code.size() < Kernel.size() || !std::equal(Kernel.begin(), Kernel.end(), code.begin())) return std::nullopt;
    const auto stride = (userData[1] >> 16u) & 0x3fffu;
    const bool swizzled = ((userData[1] >> 31u) & 1u) != 0;
    const auto dstSel = userData[3] & 0xfffu;
    const bool addTid = ((userData[3] >> 23u) & 1u) != 0;
    const auto type = userData[3] >> 30u;
    const auto format = (userData[3] >> 12u) & 0x7fu;
    if (type != 0 || stride != 16 || swizzled || addTid || dstSel != 0xfacu || format != 0x4bu) return std::nullopt;
    const auto records = std::min<std::uint64_t>(std::uint64_t{groups[0]} * 64u, userData[2]);
    const auto base = userData[0] | (static_cast<std::uint64_t>(userData[1] & 0xffffu) << 32u);
    if (base == 0 || records == 0) return std::nullopt;
    return ConstantFill{base, records * 16u, {userData[4], userData[5], userData[6], userData[7]}};
}

}
