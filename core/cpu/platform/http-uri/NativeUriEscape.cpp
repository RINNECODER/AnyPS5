// SPDX-License-Identifier: GPL-2.0-or-later
#include "NativeUriEscape.hpp"
#include "../NetworkServices.hpp"
#include "../../src/SceImageData.hpp"
#include <cpu/GuestThreads.hpp>
#include <algorithm>
#include <limits>
#include <map>
#include <span>
#include <string_view>
#include <stdexcept>

namespace Cpu::Platform {
namespace {
constexpr std::uint64_t targetSize = 7895047;
constexpr std::array targetSha256{
    std::byte{0x38},std::byte{0xdb},std::byte{0x04},std::byte{0x7f},
    std::byte{0xd9},std::byte{0xdf},std::byte{0xd2},std::byte{0x7f},
    std::byte{0xc1},std::byte{0x7d},std::byte{0xfc},std::byte{0x0d},
    std::byte{0xd2},std::byte{0xcf},std::byte{0xf3},std::byte{0x1a},
    std::byte{0x2e},std::byte{0x05},std::byte{0x33},std::byte{0xac},
    std::byte{0x1b},std::byte{0xe2},std::byte{0x35},std::byte{0x0e},
    std::byte{0x50},std::byte{0x82},std::byte{0xf8},std::byte{0x49},
    std::byte{0x9f},std::byte{0x59},std::byte{0xc6},std::byte{0xb9}};
// The parser retains immutable ELF bytes, but its public Imports vector is
// editable. Corroborate the encoded identity in the retained symbol table, not
// just the editable row addressed by Symbol.Import. Both qualified profiles retain
// SCE import identities; their symbol/string tables may be standard or SCE. No
// generalized ELF service resolver is introduced here.
std::optional<std::uint64_t> field(std::span<const std::byte> bytes,
                                  std::uint64_t offset,std::uint64_t width) {
    if (width > 8 || offset > bytes.size() || width > bytes.size()-offset)
        return std::nullopt;
    std::uint64_t value = 0;
    for (unsigned i=0;i<width;++i)
        value |= std::uint64_t(std::to_integer<unsigned>(bytes[offset+i])) << (i*8);
    return value;
}
std::optional<std::uint64_t> sum(std::uint64_t a,std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max()-a) return std::nullopt;
    return a+b;
}
bool encodedRow(const SceImageData& parsed,std::size_t index,bool target) {
    const std::span bytes(parsed.Bytes);
    const auto phoff=field(bytes,32,8), phsize=field(bytes,54,2), count=field(bytes,56,2);
    if (!phoff || !phsize || *phsize != 56 || !count || *count > 128) return false;
    struct Load { std::uint64_t offset,address,size; };
    std::vector<Load> loads;
    std::optional<std::uint64_t> dynamic,dynamicSize,data,dataSize;
    for (std::uint64_t i=0;i<*count;++i) {
        const auto p=sum(*phoff,i*56);
        if (!p || *p > bytes.size() || 56 > bytes.size()-*p) return false;
        const auto type=field(bytes,*p,4), offset=field(bytes,*p+8,8),
                   address=field(bytes,*p+16,8), size=field(bytes,*p+32,8);
        if (!type || !offset || !address || !size ||
            *offset > bytes.size() || *size > bytes.size()-*offset) return false;
        if (*type == 1) loads.push_back({*offset,*address,*size});
        if (*type == 2) { if (dynamic) return false; dynamic=offset; dynamicSize=size; }
        if (*type == 0x61000000) { if (data) return false; data=offset; dataSize=size; }
    }
    if (!dynamic || !dynamicSize || *dynamicSize%16) return false;
    std::map<std::uint64_t,std::uint64_t> tags;
    std::vector<std::uint64_t> importedLibraries;
    bool terminated = false;
    for (std::uint64_t i=0;i<*dynamicSize;i+=16) {
        const auto tag=field(bytes,*dynamic+i,8), value=field(bytes,*dynamic+i+8,8);
        if (!tag || !value) return false;
        if (!*tag) { terminated=true; break; }
        if (*tag == 0x61000015 || *tag == 0x61000049) importedLibraries.push_back(*value);
        if (*tag == 5 || *tag == 6 || *tag == 10 || *tag == 11 ||
            *tag == 0x61000035 || *tag == 0x61000037 || *tag == 0x61000039 ||
            *tag == 0x6100003b || *tag == 0x6100003f)
            if (!tags.emplace(*tag,*value).second) return false;
    }
    if (!terminated) return false;
    const auto value = [&](std::uint64_t standard,std::uint64_t sce) -> std::optional<std::uint64_t> {
        if (tags.contains(standard)) return tags.at(standard);
        if (tags.contains(sce)) return tags.at(sce);
        return std::nullopt;
    };
    // The actual SELF retains standard virtual-addressed dynamic tables, while
    // the genuine public fixture has SCE-relative tables. Follow the same
    // precedence as ParseSce and corroborate each full span in retained bytes.
    const auto table = [&](std::uint64_t standard,std::uint64_t sce,std::uint64_t size)
            -> std::optional<std::uint64_t> {
        if (tags.contains(standard)) {
            const auto address=tags.at(standard);
            for (const auto& load:loads)
                if (address >= load.address && address-load.address <= load.size &&
                    size <= load.size-(address-load.address))
                    return load.offset+(address-load.address);
            return std::nullopt;
        }
        if (!data || !dataSize || !tags.contains(sce)) return std::nullopt;
        const auto offset=tags.at(sce);
        if (offset > *dataSize || size > *dataSize-offset) return std::nullopt;
        return *data+offset;
    };
    const auto entrySize=value(11,0x6100003b), stringBytes=value(10,0x61000037);
    if (!entrySize || *entrySize != 24 || !stringBytes ||
        parsed.Symbols.size() > std::numeric_limits<std::uint64_t>::max()/24 ||
        index >= parsed.Symbols.size()) return false;
    const auto symbols=table(6,0x61000039,parsed.Symbols.size()*24);
    const auto strings=table(5,0x61000035,*stringBytes);
    if (!symbols || !strings) return false;
    const auto matches = [&](std::uint64_t offset,std::string_view name) {
        if (offset >= *stringBytes || name.size()+1 > *stringBytes-offset) return false;
        for (std::size_t i=0;i<name.size();++i)
            if (bytes[*strings+offset+i] != static_cast<std::byte>(name[i])) return false;
        return bytes[*strings+offset+name.size()] == std::byte{0};
    };
    bool libraryObserved = false;
    for (const auto library:importedLibraries) {
        if ((library >> 48) != (target ? 3u : 1u)) continue;
        if (((library >> 32) & 0xffff) != 1 ||
            !matches(library & 0xffffffff,"libSceHttp")) return false;
        libraryObserved = true;
    }
    const auto name=field(bytes,*symbols+index*24,4);
    return libraryObserved && name &&
           matches(*name,target ? "YuOW3dDAKYc#D#E" : "YuOW3dDAKYc#B#B");
}
bool sameRow(const SceImport& a,const SceImport& b) {
    return a.Nid == b.Nid && a.LibraryName == b.LibraryName &&
           a.LibraryId == b.LibraryId && a.LibraryVersion == b.LibraryVersion &&
           a.ModuleName == b.ModuleName && a.ModuleId == b.ModuleId &&
           a.ModuleMajor == b.ModuleMajor && a.ModuleMinor == b.ModuleMinor;
}
}
struct NativeUriEscape::Impl {
    Machine& machine;
    std::weak_ptr<GuestThreads> threads;
    Configuration configuration;
    std::unique_ptr<NetworkServices> provider;
    std::optional<GuestThreads::WaitDomain> ownership;
    Impl(Machine& m,const std::shared_ptr<GuestThreads>& owner,Configuration config)
        : machine(m),threads(owner),configuration(config) {
        if (!owner) throw std::invalid_argument("Native URI needs a guest scheduler");
        owner->CheckIdleOwner();
        if (!config.GateBase || (config.GateBase & 4095) ||
            config.GateBase >= 0x7ffffffff000ULL)
            throw std::invalid_argument("Native URI invalid configured gate page");
        // This validates that the scheduler belongs to this exact Machine; the
        // domain claims no wait, guest mapping or callback before admission.
        ownership.emplace(owner->CreateWaitDomain(machine,[](GuestThreadHandle) {}));
    }
    ~Impl() {
        if (ownership) ownership->Withdraw();
        provider.reset();
    }
    bool source(const SceParsedImage& consumer,bool& target) const {
        if (!consumer.Data || consumer.SourceSha256 != consumer.Data->SourceSha256 ||
            consumer.SourceSize != consumer.Data->SourceSize ||
            consumer.OsAbi != 9 || consumer.AbiVersion != 2 ||
            field(consumer.Data->Bytes,7,1) != consumer.OsAbi ||
            field(consumer.Data->Bytes,8,1) != consumer.AbiVersion ||
            field(consumer.Data->Bytes,16,2) != consumer.Type)
            return false;
        const auto& parsed = *consumer.Data;
        target = configuration.EnableTargetConsumer && consumer.Type == 0xfe18 &&
                 consumer.SourceContainer == "plain_self" &&
                 consumer.Path.filename() == "libSceNpCppWebApi.prx" &&
                 parsed.SourceSha256 == targetSha256 && parsed.SourceSize == targetSize;
        if (target) return true;
        return configuration.EnablePublicFixture && configuration.PublicFixtureSize &&
               configuration.PublicFixtureSha256 != std::array<std::byte,32>{} &&
               consumer.Type == 0xfe10 && consumer.SourceContainer == "elf" &&
               consumer.Path.filename() == "NativeUriGuest.elf" &&
               parsed.SourceSha256 == configuration.PublicFixtureSha256 &&
               parsed.SourceSize == configuration.PublicFixtureSize;
    }
    bool row(const SceImport& import,const SceParsedImage& consumer,bool target) const {
        const auto& parsed = *consumer.Data;
        bool observedModule = false;
        for (const auto& module:parsed.ImportedModules)
            if (module.Name == import.ModuleName && module.Id == import.ModuleId &&
                module.Major == 1 && module.Minor == 1) observedModule = true;
        if (!observedModule) return false;
        const auto begin = target ? 43059u : 0u;
        const auto end = target ? 43060u : parsed.Symbols.size();
        if (end > parsed.Symbols.size()) return false;
        for (std::size_t index=begin;index<end;++index) {
            const auto& symbol = parsed.Symbols[index];
            if (!symbol.Import || *symbol.Import >= consumer.Imports.size() ||
                symbol.Type != 2 || symbol.Size || symbol.Section || symbol.Value ||
                symbol.Binding != 1 || symbol.Visibility != 0) continue;
            if (sameRow(consumer.Imports[*symbol.Import],import) &&
                encodedRow(parsed,index,target)) return true;
        }
        return false;
    }
};
NativeUriEscape::NativeUriEscape(Machine& machine,const std::shared_ptr<GuestThreads>& threads)
    : NativeUriEscape(machine,threads,Configuration{}) {}
NativeUriEscape::NativeUriEscape(Machine& machine,const std::shared_ptr<GuestThreads>& threads,
                               Configuration configuration)
    : impl(std::make_unique<Impl>(machine,threads,configuration)) {}
NativeUriEscape::~NativeUriEscape() = default;
std::optional<std::uint64_t> NativeUriEscape::Resolve(const SceImport& import,
        std::uint8_t type,std::uint64_t size,const SceParsedImage& consumer) {
    bool target = false;
    if (import.Nid != NetworkServices::UriEscapeNid || type != 2 || size ||
        !impl->source(consumer,target) || import.LibraryName != "libSceHttp" ||
        import.ModuleName != "libSceHttp" || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1 ||
        import.LibraryId != (target ? 3 : 1) || import.ModuleId != (target ? 4 : 1) ||
        !impl->row(import,consumer,target)) return std::nullopt;
    const auto owner = impl->threads.lock();
    if (!owner) throw std::runtime_error("Native URI guest scheduler expired");
    owner->CheckIdleOwner();
    if (!impl->provider) {
        // Existing Map semantics can replace a mapping, so explicitly require
        // a vacant page before constructing the retained URI implementation.
        const auto base = impl->configuration.GateBase;
        for (const auto& mapping:impl->machine.Mappings())
            if (mapping.Address < base + 4096 && base < mapping.Address + mapping.Size)
                throw std::invalid_argument("Native URI gate page already mapped");
        impl->provider = std::make_unique<NetworkServices>(impl->machine,base);
    }
    return impl->provider->Resolve(import,type);
}
}
