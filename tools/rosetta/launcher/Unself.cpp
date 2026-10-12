#include "Unself.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace Aps5Launcher {
namespace {

template <typename T>
T Read(const std::vector<std::uint8_t>& data, std::uint64_t offset) {
    if (offset > data.size() || data.size() - offset < sizeof(T)) throw std::runtime_error("truncated SELF container");
    T value;
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

void Copy(std::vector<std::uint8_t>& out, std::uint64_t to, const std::vector<std::uint8_t>& data, std::uint64_t from, std::uint64_t size) {
    if (from > data.size() || data.size() - from < size || to > out.size() || out.size() - to < size)
        throw std::runtime_error("SELF segment outside the container");
    std::memcpy(out.data() + to, data.data() + from, size);
}

struct Program {
    std::uint32_t type;
    std::uint64_t offset;
    std::uint64_t fileSize;
};

}  // namespace

std::vector<std::uint8_t> Unself(const std::vector<std::uint8_t>& data) {
    static const std::uint8_t kSelf[2][4] = {{0x4f, 0x15, 0x3d, 0x1d}, {0x54, 0x14, 0xf5, 0xee}};
    static const std::uint8_t kElf[4] = {0x7f, 'E', 'L', 'F'};
    if (data.size() < 4) throw std::runtime_error("not a SELF container");
    if (std::memcmp(data.data(), kSelf[0], 4) != 0 && std::memcmp(data.data(), kSelf[1], 4) != 0) {
        if (std::memcmp(data.data(), kElf, 4) == 0) return data;
        throw std::runtime_error("not a SELF container");
    }
    const auto fileSize = Read<std::uint64_t>(data, 16);
    const auto count = Read<std::uint16_t>(data, 24);
    const std::uint64_t elfBase = 32 + static_cast<std::uint64_t>(count) * 32;
    if (Read<std::uint32_t>(data, elfBase) != 0x464c457f) throw std::runtime_error("no embedded ELF header");
    const auto programOffset = Read<std::uint64_t>(data, elfBase + 32);
    const auto programCount = Read<std::uint16_t>(data, elfBase + 56);
    const std::uint64_t headerSize = programOffset + static_cast<std::uint64_t>(programCount) * 56;
    std::vector<Program> programs;
    std::uint64_t outSize = headerSize;
    for (std::uint16_t i = 0; i < programCount; ++i) {
        const std::uint64_t entry = elfBase + programOffset + static_cast<std::uint64_t>(i) * 56;
        Program program{Read<std::uint32_t>(data, entry), Read<std::uint64_t>(data, entry + 8), Read<std::uint64_t>(data, entry + 32)};
        if (program.offset > (1ull << 34) || program.fileSize > (1ull << 34)) throw std::runtime_error("SELF program header out of range");
        outSize = std::max(outSize, program.offset + program.fileSize);
        programs.push_back(program);
    }
    if (outSize > (1ull << 34)) throw std::runtime_error("SELF image too large");
    std::vector<std::uint8_t> out(outSize, 0);
    Copy(out, 0, data, elfBase, headerSize);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> covered;
    for (std::uint16_t i = 0; i < count; ++i) {
        const std::uint64_t entry = 32 + static_cast<std::uint64_t>(i) * 32;
        const auto properties = Read<std::uint64_t>(data, entry);
        const auto offset = Read<std::uint64_t>(data, entry + 8);
        const auto stored = Read<std::uint64_t>(data, entry + 16);
        if (properties & 2) throw std::runtime_error("encrypted SELF segment");
        if (properties & 8) throw std::runtime_error("compressed SELF segment");
        if (!(properties & 0x800)) continue;  // a digest entry, not data
        const std::size_t index = (properties >> 20) & 0xFFFF;
        if (index >= programs.size()) throw std::runtime_error("SELF segment names a missing program");
        Copy(out, programs[index].offset, data, offset, stored);
        covered.emplace_back(programs[index].offset, programs[index].offset + stored);
    }
    for (const auto& program : programs) {
        if (program.type == 0x6FFFFF01 && program.fileSize) {
            Copy(out, program.offset, data, fileSize, program.fileSize);
            covered.emplace_back(program.offset, program.offset + program.fileSize);
        }
    }
    std::uint64_t retained = headerSize;
    for (std::size_t i = 0; i < programs.size(); ++i) {
        const auto& program = programs[i];
        if (!program.fileSize) continue;
        const bool present = std::any_of(covered.begin(), covered.end(), [&](const auto& range) {
            return range.first <= program.offset && program.offset + program.fileSize <= range.second;
        });
        if (!present) {
            if (program.type != 4) throw std::runtime_error("missing payload for program " + std::to_string(i));
            std::memset(out.data() + programOffset + i * 56, 0, 56);  // drop the PT_NOTE without data
            continue;
        }
        retained = std::max(retained, program.offset + program.fileSize);
    }
    out.resize(retained);
    if (out.size() < 64) throw std::runtime_error("SELF ELF header truncated");
    const std::uint64_t noSections = 0;
    std::memcpy(out.data() + 40, &noSections, 8);  // e_shoff
    std::memset(out.data() + 58, 0, 6);             // e_shentsize, e_shnum, e_shstrndx
    return out;
}

}  // namespace Aps5Launcher
