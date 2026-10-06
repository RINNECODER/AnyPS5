#include <cpu/SceTls.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <stdexcept>
#include <string>
#include <vector>

namespace Cpu {

namespace {
constexpr std::uint64_t pageSize = 4096;
constexpr std::uint64_t maximumTlsSize = 16 * 1024 * 1024;
constexpr std::uint64_t addressLimit = 0x800000000000;

std::uint64_t roundUp(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
}

SceTls::SceTls(Machine& guest, std::span<const std::byte> initialBytes,
               std::uint64_t size, std::uint64_t alignment, std::uint64_t allocationBase)
    : machine(guest), memorySize(size) {
    if (!size || size > maximumTlsSize || initialBytes.size() > size)
        throw std::invalid_argument("Invalid SCE TLS template or memory size");
    if (alignment == 0) alignment = 1;
    if (!std::has_single_bit(alignment) || alignment > maximumTlsSize)
        throw std::invalid_argument("Unsupported SCE TLS alignment");
    const auto allocationAlignment = std::max(alignment, pageSize);
    if (!allocationBase || allocationBase % allocationAlignment)
        throw std::invalid_argument("SCE TLS allocation address does not satisfy alignment");
    blockSize = roundUp(size, alignment);
    const auto tcbOffset = roundUp(blockSize, std::max<std::uint64_t>(alignment, 8));
    const auto dtvOffset = roundUp(tcbOffset + 16, pageSize);
    const auto mappedSize = roundUp(dtvOffset + 24, pageSize);
    if (allocationBase >= addressLimit || mappedSize > addressLimit - allocationBase)
        throw std::invalid_argument("SCE TLS allocation exceeds guest address range");
    fsBase = allocationBase + tcbOffset;
    tlsBase = fsBase - blockSize;
    const auto dtvBase = allocationBase + dtvOffset;
    std::vector<std::byte> bytes(static_cast<std::size_t>(mappedSize));
    std::copy(initialBytes.begin(), initialBytes.end(), bytes.begin() + static_cast<std::size_t>(tlsBase - allocationBase));
    const std::array<std::uint64_t, 2> tcb{fsBase, dtvBase};
    const std::array<std::uint64_t, 3> dtv{1, 1, tlsBase};
    const auto tcbBytes = std::as_bytes(std::span(tcb));
    const auto dtvBytes = std::as_bytes(std::span(dtv));
    std::copy(tcbBytes.begin(), tcbBytes.end(), bytes.begin() + static_cast<std::size_t>(tcbOffset));
    std::copy(dtvBytes.begin(), dtvBytes.end(), bytes.begin() + static_cast<std::size_t>(dtvOffset));
    machine.Map(allocationBase, static_cast<std::size_t>(mappedSize), Permission::Read | Permission::Write);
    machine.Write(allocationBase, bytes);
    machine.Set(Register::FsBase, fsBase);
}

std::uint64_t SceTls::FsBase() const { return fsBase; }
std::uint64_t SceTls::TlsBase() const { return tlsBase; }
std::uint64_t SceTls::ModuleId() const { return 1; }
std::uint64_t SceTls::MemorySize() const { return memorySize; }

std::uint64_t SceTls::Resolve(std::uint64_t moduleId, std::uint64_t offset) const {
    if (moduleId != 1)
        throw std::runtime_error("Unsupported SCE TLS module ID " + std::to_string(moduleId) + "; only the main module is initialized");
    if (offset >= memorySize)
        throw std::out_of_range("SCE TLS offset is outside the main module");
    return tlsBase + offset;
}

std::uint64_t SceTls::ResolveIndex(std::uint64_t guestIndexAddress) const {
    std::array<std::uint64_t, 2> index{};
    machine.Read(guestIndexAddress, std::as_writable_bytes(std::span(index)));
    return Resolve(index[0], index[1]);
}

std::uint64_t SceTls::Dtpoff(std::uint64_t moduleId, std::uint64_t offset) const {
    Resolve(moduleId, offset);
    return offset;
}

std::int64_t SceTls::Tpoff(std::uint64_t moduleId, std::uint64_t offset) const {
    Resolve(moduleId, offset);
    return static_cast<std::int64_t>(offset) - static_cast<std::int64_t>(blockSize);
}

}
