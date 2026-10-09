#include <cpu/SceImportStubs.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace Cpu {
namespace {
// Sorted {NID, name} pairs generated from the HLE export declarations under core/libs/prx.
constexpr std::pair<std::string_view, std::string_view> NidNames[] = {
#include "SceNidNames.inc"
};
constexpr std::size_t TrapCapacity = 8192;
constexpr std::uint64_t ObjectCapacity = 1 << 20;
constexpr std::uint64_t ObjectLimit = 64 * 1024;

std::string identity(const SceImport& import) {
    return "nid=" + import.Nid + " library=" + import.LibraryName + ":" + std::to_string(import.LibraryVersion) +
           " module=" + import.ModuleName + ":" + std::to_string(import.ModuleMajor) + "." +
           std::to_string(import.ModuleMinor);
}
}

std::optional<std::string_view> SceNidName(std::string_view nid) {
    const auto found = std::lower_bound(std::begin(NidNames), std::end(NidNames), nid,
        [](const auto& entry, std::string_view value) { return entry.first < value; });
    if (found == std::end(NidNames) || found->first != nid) return std::nullopt;
    return found->second;
}

struct SceImportStubs::Impl {
    using Key = std::tuple<std::string, std::string, std::uint16_t, std::string, std::uint8_t, std::uint8_t, std::uint8_t>;
    Machine& machine;
    SceImportStubOptions options;
    SceHostTrampolines traps;
    std::map<Key, SceResolvedImport> bound;
    std::uint64_t objectsUsed = 0;
    bool objectsMapped = false;

    Impl(Machine& guest, SceImportStubOptions value)
        : machine(guest), options(std::move(value)), traps(guest, options.GateBase, TrapCapacity) {
        if (!options.ObjectBase || (options.ObjectBase & 4095) || options.ObjectBase >= 0x7ffffffff000)
            throw std::invalid_argument("SCE import stubs require a nonzero aligned low canonical object page");
    }

    void log(SceImportStubEvent event) const {
        if (options.Log) options.Log(event);
    }

    std::uint64_t trap(const SceUnresolvedImport& unresolved) {
        const auto name = SceNidName(unresolved.Import.Nid);
        const auto consumer = unresolved.Consumer.Path.filename().string();
        const auto message = "Unsupported SCE unresolved import called: " +
            std::string(name ? *name : std::string_view("unknown")) + " " + identity(unresolved.Import) +
            " consumer=" + consumer;
        auto event = SceImportStubEvent{SceImportStubEventKind::Called, SceImportStubBinding::Trap, consumer,
            unresolved.Import, name, unresolved.Reason, options.ReturnValue, {}};
        return traps.Add([this, event = std::move(event), message, reported = std::make_shared<bool>(false)](Machine& guest) {
            if (!*reported) { *reported = true; log(event); }
            if (!options.ReturnValue) throw std::runtime_error(message);
            guest.Set(Register::Rax, *options.ReturnValue);
        });
    }

    std::uint64_t object(std::uint64_t size) {
        const auto bytes = (std::max<std::uint64_t>(size, 8) + 15) & ~std::uint64_t{15};
        if (size > ObjectLimit || bytes > ObjectCapacity - objectsUsed)
            throw std::runtime_error("Unsupported SCE unresolved object import exceeds zeroed stub storage");
        if (!objectsMapped) {
            machine.Map(options.ObjectBase, ObjectCapacity, Permission::Read | Permission::Write);
            objectsMapped = true;
        }
        const auto address = options.ObjectBase + objectsUsed;
        objectsUsed += bytes;
        return address;
    }

    SceResolvedImport bind(const SceUnresolvedImport& unresolved) {
        if (unresolved.Type != 1 && unresolved.Type != 2)
            throw std::runtime_error("Unsupported SCE unresolved import type for " + unresolved.Import.Nid);
        const auto& import = unresolved.Import;
        const Key key{import.Nid, import.LibraryName, import.LibraryVersion, import.ModuleName,
                      import.ModuleMajor, import.ModuleMinor, static_cast<std::uint8_t>(unresolved.Weak ? 0 : unresolved.Type)};
        if (const auto found = bound.find(key); found != bound.end()) {
            if (unresolved.Type == 1 && unresolved.Size > found->second.Size)
                throw std::runtime_error("Unsupported SCE unresolved object import size mismatch for " + import.Nid);
            auto result = found->second;
            result.Type = unresolved.Type;
            return result;
        }
        SceImportStubBinding binding = SceImportStubBinding::Trap;
        SceResolvedImport result{0, unresolved.Type, 0};
        if (unresolved.Weak) binding = SceImportStubBinding::WeakZero;
        else if (unresolved.Type == 1) {
            binding = SceImportStubBinding::ZeroObject;
            result.Address = object(unresolved.Size);
            result.Size = std::max<std::uint64_t>(unresolved.Size, 8);
        } else result.Address = trap(unresolved);
        bound.emplace(key, result);
        log({SceImportStubEventKind::Bound, binding, unresolved.Consumer.Path.filename().string(), import,
             SceNidName(import.Nid), unresolved.Reason, std::nullopt, {}});
        return result;
    }
};

SceImportStubs::SceImportStubs(Machine& machine, SceImportStubOptions options)
    : impl(std::make_shared<Impl>(machine, std::move(options))) {}
SceImportStubs::~SceImportStubs() = default;

SceResolvedImport SceImportStubs::Bind(const SceUnresolvedImport& import) { return impl->bind(import); }

SceLazyImports SceImportStubs::Policy() {
    return {
        [state = std::weak_ptr<Impl>(impl)](const SceUnresolvedImport& import) {
            const auto context = state.lock();
            if (!context) throw std::runtime_error("SCE import stub runtime has expired");
            return context->bind(import);
        },
        [state = std::weak_ptr<Impl>(impl)](const std::string& filename) {
            const auto context = state.lock();
            if (!context) throw std::runtime_error("SCE import stub runtime has expired");
            SceImportStubEvent event;
            event.Kind = SceImportStubEventKind::MissingModule;
            event.Module = filename;
            context->log(std::move(event));
        }};
}

}
