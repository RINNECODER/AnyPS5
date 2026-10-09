#include "NativeNpIdentity.hpp"
#include "../NpServices.hpp"
#include "../../src/SceImageData.hpp"
#include <cpu/GuestThreads.hpp>
#include <algorithm>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace Cpu::Platform {
namespace {
constexpr std::string_view nid = "XDncXQIJUSk";
constexpr std::string_view family = "libSceNpManager";
// Minimal SCE scoped-identifier encoding, the inverse of the ELF loader's.
std::string scopedId(std::uint16_t value) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string result;
    do { result.insert(result.begin(), alphabet[value % 64]); value /= 64; } while (value);
    return result;
}
bool same(const SceImport& a, const SceImport& b) {
    return a.Nid == b.Nid && a.LibraryName == b.LibraryName && a.ModuleName == b.ModuleName &&
           a.LibraryId == b.LibraryId && a.ModuleId == b.ModuleId &&
           a.LibraryVersion == b.LibraryVersion && a.ModuleMajor == b.ModuleMajor && a.ModuleMinor == b.ModuleMinor;
}

// Narrow revalidation of the parser's retained ELF metadata. SceImageData has
// original symbol attributes but its Import index refers to editable public
// rows. Read the original scoped name and identities as well, so changing one
// of those rows cannot turn a different retained import into this service.
class OriginalRows {
    std::span<const std::byte> bytes;
    struct Segment { std::uint32_t type; std::uint64_t offset, address, size; };
    std::vector<Segment> segments;
    std::map<std::uint64_t,std::uint64_t> tags;
    std::vector<std::uint64_t> libraries, modules;
    std::uint64_t strings = 0, stringSize = 0;
    std::uint64_t read(std::uint64_t offset, unsigned count) const {
        if (offset > bytes.size() || count > bytes.size()-offset) throw std::runtime_error("NP retained metadata range");
        std::uint64_t value = 0;
        for (unsigned i=0;i<count;++i) value |= std::uint64_t(std::to_integer<unsigned char>(bytes[offset+i])) << (8*i);
        return value;
    }
    std::uint64_t table(std::uint64_t standard, std::uint64_t sce, std::uint64_t length) const {
        if (tags.contains(standard) == tags.contains(sce)) throw std::runtime_error("NP retained table identity");
        const bool native = tags.contains(standard);
        const auto address = tags.at(native ? standard : sce);
        for (const auto& s : segments) {
            if (!native && s.type == 0x61000000 && address <= s.size && length <= s.size-address)
                return s.offset+address;
            if (native && s.type == 1 && address >= s.address && address-s.address <= s.size &&
                length <= s.size-(address-s.address)) return s.offset+address-s.address;
        }
        throw std::runtime_error("NP retained table mapping");
    }
    std::string string(std::uint64_t offset) const {
        if (offset >= stringSize) throw std::runtime_error("NP retained string range");
        std::string value;
        for (; offset<stringSize; ++offset) {
            const auto c = static_cast<char>(read(strings+offset,1));
            if (!c) return value;
            value.push_back(c);
        }
        throw std::runtime_error("NP retained unterminated string");
    }
public:
    explicit OriginalRows(std::span<const std::byte> source) : bytes(source) {
        if (read(0,4) != 0x464c457f || read(4,3) != 0x010102 || read(16,2) != 0xfe10 ||
            read(18,2) != 62 || read(54,2) != 56)
            throw std::runtime_error("NP retained ELF identity");
        const auto phoff=read(32,8), count=read(56,2);
        if (!count || count>128 || phoff>bytes.size() || count>(bytes.size()-phoff)/56)
            throw std::runtime_error("NP retained ELF headers");
        for (std::uint64_t i=0;i<count;++i) {
            const auto p=phoff+i*56;
            Segment s{static_cast<std::uint32_t>(read(p,4)),read(p+8,8),read(p+16,8),read(p+32,8)};
            if (s.offset>bytes.size() || s.size>bytes.size()-s.offset) throw std::runtime_error("NP retained segment range");
            segments.push_back(s);
        }
        const auto dynamic=std::find_if(segments.begin(),segments.end(),[](const auto& s){return s.type==2;});
        if (dynamic==segments.end() || dynamic->size%16) throw std::runtime_error("NP retained dynamic segment");
        bool terminated=false;
        for (std::uint64_t n=0;n<dynamic->size;n+=16) {
            const auto tag=read(dynamic->offset+n,8), value=read(dynamic->offset+n+8,8);
            if (!tag) { terminated=true;break; }
            if (tag==0x61000015 || tag==0x61000049) libraries.push_back(value);
            else if (tag==0x6100000f || tag==0x61000045) modules.push_back(value);
            else if (tag==5 || tag==6 || tag==10 || tag==11 || tag==0x61000035 || tag==0x61000037 ||
                     tag==0x61000039 || tag==0x6100003b)
                if (!tags.emplace(tag,value).second) throw std::runtime_error("NP retained duplicate table");
        }
        if (!terminated || tags.contains(10)==tags.contains(0x61000037)) throw std::runtime_error("NP retained dynamic identity");
        stringSize=tags.at(tags.contains(10)?10:0x61000037);
        strings=table(5,0x61000035,stringSize);
    }
    bool agrees(std::size_t index, const SceImport& row, std::string_view scoped) const {
        if (tags.contains(11)==tags.contains(0x6100003b) || tags.at(tags.contains(11)?11:0x6100003b)!=24) return false;
        const auto symbols=table(6,0x61000039,(index+1)*24), offset=symbols+index*24;
        if (read(offset+4,1)!=0x12 || read(offset+5,1)!=0 || read(offset+6,2)!=0 ||
            read(offset+8,8)!=0 || read(offset+16,8)!=0 || string(read(offset,4))!=scoped) return false;
        auto identity=[&](const auto& entries, std::uint16_t id, std::uint16_t version) {
            bool found=false;
            for (const auto value:entries) if (static_cast<std::uint16_t>(value>>48)==id) {
                if (static_cast<std::uint16_t>(value>>32)!=version || string(value&0xffffffff)!=family) return false;
                found=true;
            }
            return found;
        };
        return identity(libraries,row.LibraryId,row.LibraryVersion) &&
               identity(modules,row.ModuleId,static_cast<std::uint16_t>(row.ModuleMajor*256+row.ModuleMinor));
    }
};
}

struct NativeNpIdentity::Impl {
    Machine& machine;
    std::weak_ptr<GuestThreads> threads;
    Configuration config;
    GuestThreads::WaitDomain owner;
    std::unique_ptr<NpServices> services;
    Impl(Machine& m, const std::shared_ptr<GuestThreads>& t, Configuration c)
        : machine(m), threads(t), config(c) {
        if (!t || c.SessionUserId==0xffffffffu) throw std::invalid_argument("NP identity requires an owned signed-out session");
        t->CheckIdleOwner();
        owner=t->CreateWaitDomain(m,{});
    }
    bool admits(const SceParsedImage& image, const SceImport& row, std::uint8_t type, std::uint64_t size) const {
        if (!image.Data || image.Type!=0xfe10 || type!=2 || size!=0 || row.Nid!=nid || row.LibraryName!=family || row.ModuleName!=family ||
            row.LibraryVersion!=1 || row.ModuleMajor!=1 || row.ModuleMinor!=1) return false;
        const auto& data=*image.Data;
        if (image.SourceSha256!=data.SourceSha256 || image.SourceSize!=data.SourceSize) return false;
        // Title-agnostic: any signed main executable, at whatever import-table
        // ids its own library/module metadata assigns to libSceNpManager.
        const bool title=config.EnableQualifiedConsumer && image.Path.filename()=="eboot.bin" &&
            image.SourceContainer=="plain_self";
        const bool fixture=config.EnablePublicFixtureCandidate && config.PublicFixtureSize &&
            image.Path.filename()=="NativeNpIdentityGuest.elf" && image.SourceContainer=="elf" && image.Type==0xfe10 &&
            data.SourceSha256==config.PublicFixtureSha256 && data.SourceSize==config.PublicFixtureSize &&
            row.LibraryId==1 && row.ModuleId==1;
        if (!title && !fixture) return false;
        const auto scoped=std::string(nid)+"#"+scopedId(row.LibraryId)+"#"+scopedId(row.ModuleId);
        try {
            OriginalRows original(data.Bytes);
            for (std::size_t n=0;n<data.Symbols.size();++n) {
                const auto& symbol=data.Symbols[n];
                if (!symbol.Import || *symbol.Import>=image.Imports.size() ||
                    symbol.Type!=2 || symbol.Size || symbol.Value || symbol.Section || symbol.Binding!=1 || symbol.Visibility ||
                    !same(row,image.Imports[*symbol.Import])) continue;
                if (original.agrees(n,row,scoped)) return true;
            }
        } catch (const std::exception&) { return false; }
        return false;
    }
};

NativeNpIdentity::NativeNpIdentity(Machine& m, const std::shared_ptr<GuestThreads>& t)
    : NativeNpIdentity(m,t,Configuration{}) {}
NativeNpIdentity::NativeNpIdentity(Machine& m, const std::shared_ptr<GuestThreads>& t, Configuration c)
    : impl(std::make_unique<Impl>(m,t,c)) {}
NativeNpIdentity::~NativeNpIdentity() = default;
std::optional<std::uint64_t> NativeNpIdentity::Resolve(const SceImport& row, std::uint8_t type,
        std::uint64_t size, const SceParsedImage& consumer) {
    if (!impl->admits(consumer,row,type,size)) return std::nullopt;
    const auto scheduler=impl->threads.lock();
    if (!scheduler) throw std::runtime_error("NP identity scheduler expired");
    scheduler->CheckIdleOwner();
    if (!impl->services) impl->services=std::make_unique<NpServices>(impl->machine,impl->config.SessionUserId,impl->config.GateBase);
    return impl->services->Resolve(row,type);
}
}
