#include <cpu/Self.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>

namespace Cpu {
namespace {
constexpr std::uint64_t MaximumSize = 256 * 1024 * 1024;
constexpr std::uint64_t KnownProperties = 1 | 2 | 4 | 8 | 0x800 | 0xf000 | 0x10000 | (0xffffull << 20);

[[noreturn]] void fail(const std::string& message) { throw std::runtime_error("Plain SELF decoder: " + message); }

bool fits(std::uint64_t offset, std::uint64_t size, std::uint64_t end) {
    return offset <= end && size <= end - offset;
}

std::uint64_t read(std::span<const std::byte> input, std::uint64_t offset, unsigned length) {
    if (!fits(offset, length, input.size())) fail("truncated container metadata");
    std::uint64_t value = 0;
    for (unsigned index = 0; index < length; ++index)
        value |= std::uint64_t(std::to_integer<unsigned>(input[offset + index])) << (index * 8);
    return value;
}

void write(std::span<std::byte> output, std::uint64_t offset, std::uint64_t value, unsigned length) {
    for (unsigned index = 0; index < length; ++index) output[offset + index] = static_cast<std::byte>(value >> (index * 8));
}

struct Entry {
    std::uint64_t Properties, Offset, StoredSize, DecodedSize;
    bool Data() const { return (Properties & 0x800) != 0; }
    std::uint64_t Index() const { return (Properties >> 20) & 0xffff; }
};
struct Program { std::uint32_t Type; std::uint64_t Offset, FileSize; };
}

bool IsSelf(std::span<const std::byte> input) noexcept {
    if (input.size() < 4) return false;
    const std::array first{std::byte{0x4f}, std::byte{0x15}, std::byte{0x3d}, std::byte{0x1d}};
    const std::array second{std::byte{0x54}, std::byte{0x14}, std::byte{0xf5}, std::byte{0xee}};
    return std::equal(first.begin(), first.end(), input.begin()) || std::equal(second.begin(), second.end(), input.begin());
}

DecodedSelf DecodePlainSelf(std::span<const std::byte> input) {
    if (!IsSelf(input)) fail("input is not a supported SELF container");
    if (input.size() < 32 || input.size() > MaximumSize) fail("container size exceeds supported bounds");
    if (read(input, 4, 1) != 0 || read(input, 5, 1) != 1 || read(input, 6, 1) != 1 ||
        read(input, 7, 1) != 0x12 || read(input, 8, 4) != 0x101 || read(input, 26, 2) != 0x22 || read(input, 28, 4))
        fail("unsupported SELF header profile");
    const auto headerSize = read(input, 12, 2);
    const auto metaSize = read(input, 14, 2);
    const auto fileSize = read(input, 16, 8);
    const auto count = read(input, 24, 2);
    if (!count || count > 2048 || !fits(32, count * 32, headerSize) || !fits(headerSize, metaSize, fileSize) ||
        fileSize > input.size() || fileSize < headerSize + metaSize) fail("invalid SELF header, metadata, or declared file bounds");
    const auto elfBase = 32 + count * 32;
    if (!fits(elfBase, 64, headerSize) || read(input, elfBase, 4) != 0x464c457f || read(input, elfBase + 4, 1) != 2 ||
        read(input, elfBase + 5, 1) != 1 || read(input, elfBase + 6, 1) != 1 || read(input, elfBase + 18, 2) != 62 ||
        read(input, elfBase + 20, 4) != 1 || read(input, elfBase + 52, 2) != 64) fail("invalid embedded x86-64 ELF header");
    const auto phOffset = read(input, elfBase + 32, 8);
    const auto phCount = read(input, elfBase + 56, 2);
    if (phOffset < 64 || read(input, elfBase + 54, 2) != 56 || !phCount || phCount > 1024 ||
        !fits(phOffset, phCount * 56, headerSize - elfBase)) fail("invalid embedded program header table");
    const auto elfHeaderSize = phOffset + phCount * 56;
    std::vector<Program> programs;
    std::uint64_t outputSize = elfHeaderSize;
    for (std::uint64_t index = 0; index < phCount; ++index) {
        const auto offset = elfBase + phOffset + index * 56;
        Program program{static_cast<std::uint32_t>(read(input, offset, 4)), read(input, offset + 8, 8), read(input, offset + 32, 8)};
        if (!fits(program.Offset, program.FileSize, MaximumSize)) fail("logical ELF program range overflows or exceeds size limit");
        outputSize = std::max(outputSize, program.Offset + program.FileSize);
        programs.push_back(program);
    }
    std::vector<Entry> entries;
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto offset = 32 + index * 32;
        Entry entry{read(input, offset, 8), read(input, offset + 8, 8), read(input, offset + 16, 8), read(input, offset + 24, 8)};
        if (entry.Properties & 2) fail("encrypted SELF segments are unsupported; plaintext executable bytes are required");
        if (entry.Properties & 8) fail("compressed SELF segments are unsupported");
        if ((entry.Properties & ~KnownProperties) ||
            (entry.Data() ? (entry.Properties & 0x10000) != 0 : (entry.Properties & 0x10000) == 0) ||
            (!entry.Data() && (entry.Properties & 0xf000))) fail("unsupported SELF segment properties");
        if (!entry.StoredSize || entry.StoredSize != entry.DecodedSize || entry.Offset % 16 ||
            entry.Offset < headerSize + metaSize || !fits(entry.Offset, entry.StoredSize, fileSize)) fail("invalid plaintext SELF segment range or size");
        for (const auto& previous : entries)
            if (entry.Offset < previous.Offset + previous.StoredSize && previous.Offset < entry.Offset + entry.StoredSize)
                fail("overlapping SELF container segment ranges");
        entries.push_back(entry);
    }
    std::set<std::uint64_t> dataPrograms, referencedEntries;
    for (const auto& entry : entries) {
        const auto index = entry.Index();
        if (entry.Data()) {
            if (index >= programs.size() || !dataPrograms.insert(index).second) fail("invalid or duplicate SELF data program index");
            const auto& program = programs[index];
            if (program.Type != 1 && program.Type != 0x61000010 && program.Type != 0x61000000 &&
                program.Type != 0x6fffff00 && program.Type != 0x61000002) fail("unsupported SELF data program type");
            if (entry.DecodedSize != program.FileSize) fail("SELF data size does not match ELF program size");
        } else {
            if (index >= entries.size() || !entries[index].Data() || !referencedEntries.insert(index).second)
                fail("invalid or duplicate SELF digest entry reference");
            const auto blockSize = 1ull << (12 + ((entries[index].Properties >> 12) & 15));
            const auto expected = ((entries[index].DecodedSize + blockSize - 1) / blockSize) * 32;
            if (entry.DecodedSize != expected) fail("SELF digest metadata size does not match data block count");
        }
    }
    for (std::size_t index = 0; index < entries.size(); ++index)
        if (entries[index].Data() && !referencedEntries.contains(index)) fail("SELF data segment lacks paired digest metadata");
    DecodedSelf result{std::vector<std::byte>(outputSize), {}};
    std::vector<std::pair<std::uint64_t, std::uint64_t>> written;
    const auto install = [&](std::uint64_t offset, std::span<const std::byte> bytes) {
        for (const auto& [begin, end] : written) {
            const auto overlapBegin = std::max(begin, offset), overlapEnd = std::min(end, offset + bytes.size());
            if (overlapBegin < overlapEnd && !std::equal(bytes.begin() + overlapBegin - offset, bytes.begin() + overlapEnd - offset,
                result.Bytes.begin() + overlapBegin)) fail("conflicting reconstructed ELF byte ranges");
        }
        std::copy(bytes.begin(), bytes.end(), result.Bytes.begin() + offset);
        written.emplace_back(offset, offset + bytes.size());
    };
    install(0, input.subspan(elfBase, elfHeaderSize));
    for (const auto& entry : entries)
        if (entry.Data()) install(programs[entry.Index()].Offset, input.subspan(entry.Offset, entry.StoredSize));
    bool version = false;
    for (const auto& program : programs) {
        if (program.Type != 0x6fffff01 || !program.FileSize) continue;
        if (version || program.FileSize != input.size() - fileSize) fail("invalid or unavailable appended SELF version data");
        install(program.Offset, input.subspan(fileSize, program.FileSize));
        version = true;
    }
    if (input.size() != fileSize && !version) fail("unaccounted trailing SELF bytes");
    std::sort(written.begin(), written.end());
    const auto covered = [&](std::uint64_t offset, std::uint64_t size) {
        auto cursor = offset;
        for (const auto& [begin, end] : written) {
            if (begin > cursor) break;
            if (end > cursor) cursor = end;
            if (cursor >= offset + size) return true;
        }
        return !size;
    };
    std::uint64_t retainedSize = elfHeaderSize;
    for (std::size_t index = 0; index < programs.size(); ++index) {
        const auto& program = programs[index];
        if (program.FileSize && !covered(program.Offset, program.FileSize)) {
            if (program.Type != 4) fail("missing required ELF program payload for index " + std::to_string(index));
            std::fill_n(result.Bytes.begin() + phOffset + index * 56, 56, std::byte{});
            result.NormalizationNotes.push_back("unavailable nonloadable PT_NOTE omitted at program index " + std::to_string(index));
            continue;
        }
        if (program.FileSize) retainedSize = std::max(retainedSize, program.Offset + program.FileSize);
    }
    result.Bytes.resize(retainedSize);
    if (read(result.Bytes, 40, 8) || read(result.Bytes, 58, 2) || read(result.Bytes, 60, 2) || read(result.Bytes, 62, 2)) {
        write(result.Bytes, 40, 0, 8);
        write(result.Bytes, 58, 0, 2);
        write(result.Bytes, 60, 0, 2);
        write(result.Bytes, 62, 0, 2);
        result.NormalizationNotes.push_back("SELF section table references removed; standalone reconstruction contains program headers and payloads");
    }
    return result;
}

}
