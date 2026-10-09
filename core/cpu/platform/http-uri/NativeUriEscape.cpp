// SPDX-License-Identifier: GPL-2.0-or-later
#include "NativeUriEscape.hpp"
#include "../NetworkServices.hpp"
#include "../../src/SceImageData.hpp"
#include <cpu/GuestThreads.hpp>
#include <algorithm>
#include <limits>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <stdexcept>

namespace Cpu::Platform {
namespace {
// Minimal SCE scoped-identifier encoding, the inverse of the ELF loader's.
std::string scopedId(std::uint16_t value) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string result;
    do { result.insert(result.begin(), alphabet[value % 64]); value /= 64; } while (value);
    return result;
}
// The parser retains immutable ELF bytes, but its public Imports vector is
// editable. Corroborate the encoded identity in the retained symbol table, not
// just the editable row addressed by Symbol.Import. Both admitted profiles retain
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
bool encodedRow(const SceImageData& parsed,std::size_t index,const SceImport& import) {
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
        if ((library >> 48) != import.LibraryId) continue;
        if (((library >> 32) & 0xffff) != 1 ||
            !matches(library & 0xffffffff,"libSceHttp")) return false;
        libraryObserved = true;
    }
    const auto name=field(bytes,*symbols+index*24,4);
    return libraryObserved && name &&
           matches(*name,import.Nid+"#"+scopedId(import.LibraryId)+"#"+scopedId(import.ModuleId));
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
        // Any signed WebApi module version: its source identity is not an
        // admission input; scope and the retained symbol row are.
        target = configuration.EnableTargetConsumer && consumer.Type == 0xfe18 &&
                 consumer.SourceContainer == "plain_self" &&
                 consumer.Path.filename() == "libSceNpCppWebApi.prx";
        if (target) return true;
        return configuration.EnablePublicFixture && configuration.PublicFixtureSize &&
               configuration.PublicFixtureSha256 != std::array<std::byte,32>{} &&
               consumer.Type == 0xfe10 && consumer.SourceContainer == "elf" &&
               consumer.Path.filename() == "NativeUriGuest.elf" &&
               parsed.SourceSha256 == configuration.PublicFixtureSha256 &&
               parsed.SourceSize == configuration.PublicFixtureSize;
    }
    bool row(const SceImport& import,const SceParsedImage& consumer) const {
        const auto& parsed = *consumer.Data;
        bool observedModule = false;
        for (const auto& module:parsed.ImportedModules)
            if (module.Name == import.ModuleName && module.Id == import.ModuleId &&
                module.Major == 1 && module.Minor == 1) observedModule = true;
        if (!observedModule) return false;
        for (std::size_t index=0;index<parsed.Symbols.size();++index) {
            const auto& symbol = parsed.Symbols[index];
            if (!symbol.Import || *symbol.Import >= consumer.Imports.size() ||
                symbol.Type != 2 || symbol.Size || symbol.Section || symbol.Value ||
                symbol.Binding != 1 || symbol.Visibility != 0) continue;
            if (sameRow(consumer.Imports[*symbol.Import],import) &&
                encodedRow(parsed,index,import)) return true;
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
        (!target && (import.LibraryId != 1 || import.ModuleId != 1)) ||
        !impl->row(import,consumer)) return std::nullopt;
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
