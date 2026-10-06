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
constexpr std::uint64_t hostPageSize = 16384;
constexpr std::uint64_t maximumTlsSize = 16 * 1024 * 1024;
constexpr std::size_t maximumModuleCount = pageSize / sizeof(std::uint64_t) - 2;
constexpr std::uint64_t addressLimit = 0x800000000000;
constexpr std::uint64_t minimumTcbSize = 0x30;

std::uint64_t roundUp(std::uint64_t value, std::uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}
}

SceTls::SceTls(Machine& guest, std::span<const std::byte> initialBytes,
               std::uint64_t size, std::uint64_t alignment, std::uint64_t allocationBase,
               SceTlsActivation activation)
    : SceTls(guest, std::array{SceTlsModuleTemplate{1, initialBytes, size, alignment}}, allocationBase, activation) {}

SceTls::SceTls(Machine& guest, std::span<const SceTlsModuleTemplate> templates,
               std::uint64_t allocationBase, SceTlsActivation activation) : machine(guest) {
    if (activation != SceTlsActivation::Activate && activation != SceTlsActivation::Deferred)
        throw std::invalid_argument("Invalid SCE TLS activation policy");
    if (templates.empty() || templates.size() > maximumModuleCount)
        throw std::invalid_argument("Invalid SCE TLS module count");
    std::uint64_t totalOffset = 0;
    std::uint64_t maximumAlignment = 8;
    modules.reserve(templates.size());
    for (std::size_t index = 0; index < templates.size(); ++index) {
        const auto& module = templates[index];
        if (module.ModuleId != index + 1)
            throw std::invalid_argument("SCE TLS module IDs must be ordered and contiguous from main module 1");
        if (module.MemorySize > maximumTlsSize || module.InitialBytes.size() > module.MemorySize)
            throw std::invalid_argument("Invalid SCE TLS template or memory size");
        const auto alignment = std::max<std::uint64_t>(module.Alignment, 1);
        if (!std::has_single_bit(alignment) || alignment > maximumTlsSize)
            throw std::invalid_argument("Unsupported SCE TLS alignment");
        if (module.MemorySize) {
            totalOffset = roundUp(totalOffset + module.MemorySize, alignment);
            if (totalOffset > maximumTlsSize)
                throw std::invalid_argument("SCE TLS total static size exceeds 16 MiB");
            maximumAlignment = std::max(maximumAlignment, alignment);
        }
        modules.push_back({module.MemorySize, module.MemorySize ? totalOffset : 0, 0});
    }
    const auto mappingAlignment = templates.size() == 1 ? pageSize : hostPageSize;
    const auto allocationAlignment = std::max(maximumAlignment, mappingAlignment);
    if (!allocationBase || allocationBase % allocationAlignment)
        throw std::invalid_argument("SCE TLS allocation address does not satisfy alignment");
    const auto tcbOffset = roundUp(totalOffset, maximumAlignment);
    if (tcbOffset > maximumTlsSize)
        throw std::invalid_argument("SCE TLS total static size exceeds 16 MiB");
    const auto dtvOffset = roundUp(tcbOffset + minimumTcbSize, pageSize);
    const auto dtvSize = (modules.size() + 2) * sizeof(std::uint64_t);
    const auto mappedSize = roundUp(dtvOffset + dtvSize, mappingAlignment);
    if (allocationBase >= addressLimit || mappedSize > addressLimit - allocationBase)
        throw std::invalid_argument("SCE TLS allocation exceeds guest address range");
    fsBase = allocationBase + tcbOffset;
    const auto dtvBase = allocationBase + dtvOffset;
    std::vector<std::byte> bytes(static_cast<std::size_t>(mappedSize));
    std::vector<std::uint64_t> dtv(modules.size() + 2);
    dtv[0] = 1;
    dtv[1] = modules.size();
    for (std::size_t index = 0; index < modules.size(); ++index) {
        auto& module = modules[index];
        if (module.memorySize) {
            module.base = fsBase - module.threadOffset;
            const auto initialBytes = templates[index].InitialBytes;
            std::copy(initialBytes.begin(), initialBytes.end(), bytes.begin() + static_cast<std::size_t>(module.base - allocationBase));
            dtv[index + 2] = module.base;
        }
    }
    const std::array<std::uint64_t, 2> tcb{fsBase, dtvBase};
    const auto tcbBytes = std::as_bytes(std::span(tcb));
    const auto dtvBytes = std::as_bytes(std::span(dtv));
    std::copy(tcbBytes.begin(), tcbBytes.end(), bytes.begin() + static_cast<std::size_t>(tcbOffset));
    std::copy(dtvBytes.begin(), dtvBytes.end(), bytes.begin() + static_cast<std::size_t>(dtvOffset));
    allocation = {allocationBase, static_cast<std::size_t>(mappedSize), Permission::Read | Permission::Write, false};
    machine.Map(allocation.Address, allocation.Size, allocation.Permissions);
    try {
        machine.Write(allocation.Address, bytes);
        if (activation == SceTlsActivation::Activate) Activate();
    } catch (...) {
        machine.Unmap(allocation.Address, allocation.Size);
        throw;
    }
}

void SceTls::Activate() {
    machine.CheckAccess(allocation.Address, allocation.Size, allocation.Permissions);
    machine.Set(Register::FsBase, fsBase);
}
Mapping SceTls::Allocation() const { return allocation; }
std::uint64_t SceTls::FsBase() const { return fsBase; }
std::uint64_t SceTls::TlsBase() const { return modules.front().base; }
std::uint64_t SceTls::TlsBase(std::uint64_t moduleId) const { return moduleLayout(moduleId).base; }
std::uint64_t SceTls::ModuleId() const { return 1; }
std::uint64_t SceTls::MemorySize() const { return modules.front().memorySize; }
std::uint64_t SceTls::MemorySize(std::uint64_t moduleId) const { return moduleLayout(moduleId).memorySize; }
std::uint64_t SceTls::ModuleCount() const { return modules.size(); }

const SceTls::ModuleLayout& SceTls::moduleLayout(std::uint64_t moduleId) const {
    if (moduleId == 0 || moduleId > modules.size())
        throw std::runtime_error("Unsupported SCE TLS module ID " + std::to_string(moduleId) + "; only initialized static modules are available");
    return modules[static_cast<std::size_t>(moduleId - 1)];
}

std::uint64_t SceTls::Resolve(std::uint64_t moduleId, std::uint64_t offset) const {
    const auto& module = moduleLayout(moduleId);
    if (offset >= module.memorySize)
        throw std::out_of_range(moduleId == 1 ? "SCE TLS offset is outside the main module" : "SCE TLS offset is outside module " + std::to_string(moduleId));
    return module.base + offset;
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
    return static_cast<std::int64_t>(offset) - static_cast<std::int64_t>(moduleLayout(moduleId).threadOffset);
}

}
