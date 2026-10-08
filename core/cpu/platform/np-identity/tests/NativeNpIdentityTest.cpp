#include "../NativeNpIdentity.hpp"
#include "../../../src/SceImageData.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceModules.hpp>
#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>

// Contracts and coverage gap: external np-identity-job/fixture/test-audit.json.
// This uses actual linked SCE FUNC/PLT relocations and the consumer-aware module
// resolver, not a mock provider or a precomputed API-return guest.
namespace {
using Provider = Cpu::Platform::NativeNpIdentity;
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr std::uint64_t Data = 0x300000, Output = 0x310000, Stack = 0x320000;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
bool mappingsEqual(const std::vector<Cpu::Mapping>& a, const std::vector<Cpu::Mapping>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t n=0;n<a.size();++n)
        if (a[n].Address!=b[n].Address || a[n].Size!=b[n].Size ||
            a[n].Permissions!=b[n].Permissions || a[n].Borrowed!=b[n].Borrowed) return false;
    return true;
}
Provider::Configuration publicProfile(const Cpu::SceParsedImage& image) {
    Provider::Configuration config;
    config.EnablePublicFixtureCandidate = true;
    config.PublicFixtureSha256 = image.SourceSha256;
    config.PublicFixtureSize = image.SourceSize;
    return config;
}
const Cpu::SceImport& actualRow(const Cpu::SceParsedImage& image) {
    require(image.Imports.size()==1 && image.Imports[0].Nid=="XDncXQIJUSk",
            "Public linked SCE lost its sole actual GetOnlineId import");
    return image.Imports[0];
}
void scope(const char* path) {
    const auto image = Cpu::ParseSce(path);
    const auto row = actualRow(image);
    Cpu::Machine machine;
    auto threads = std::make_shared<Cpu::GuestThreads>(machine);
    const auto before = machine.Mappings();
    Provider disabled(machine,threads);
    require(!disabled.Resolve(row,2,0,image),"Default configuration admitted a fixture/retail consumer");
    require(mappingsEqual(before,machine.Mappings()),"Default denial allocated NP gates");
    Provider candidate(machine,threads,publicProfile(image));
    auto deny=[&](const Cpu::SceImport& wrong,std::uint8_t type,std::uint64_t size,const Cpu::SceParsedImage& source) {
        bool rejected=false;
        try { rejected=!candidate.Resolve(wrong,type,size,source); }
        catch (const std::exception&) { rejected=true; }
        require(rejected,"Unqualified NP consumer, row, type or size was admitted");
        require(mappingsEqual(before,machine.Mappings()),"Rejected typed NP import allocated a gate");
    };
    for (const auto type:{0,1,3,6}) deny(row,type,0,image);
    for (const auto size:{1ULL,8ULL,~0ULL}) deny(row,2,size,image);
    for (unsigned field=0;field<8;++field) {
        auto wrong=row;
        if(field==0) wrong.Nid="foreign";
        if(field==1) wrong.LibraryName="libSceNpAuth";
        if(field==2) wrong.ModuleName="libSceNpAuth";
        if(field==3) ++wrong.LibraryVersion;
        if(field==4) ++wrong.ModuleMajor;
        if(field==5) ++wrong.ModuleMinor;
        if(field==6) ++wrong.LibraryId;
        if(field==7) ++wrong.ModuleId;
        deny(wrong,2,0,image);
    }
    for (unsigned field=0;field<12;++field) {
        auto wrong=image;
        if(field==0) wrong.SourceSha256[0]^=std::byte{1};
        if(field==1) ++wrong.SourceSize;
        if(field==2) wrong.SourceContainer="plain_self";
        if(field==3) wrong.Path="eboot.bin";
        if(field==4) wrong.Type=3;
        if(field==5) wrong.Data.reset();
        if(field==6) wrong.Imports.clear();
        if(field==7) wrong.Imports[0].Nid="forged";
        if(field>=8) {
            auto data=std::make_shared<Cpu::SceImageData>(*wrong.Data);
            if(field==8) data->SourceSha256[0]^=std::byte{1};
            if(field==9) ++data->SourceSize;
            if(field>=10) {
                const auto symbol=std::find_if(data->Symbols.begin(),data->Symbols.end(),[](const auto& item){return item.Import.has_value();});
                require(symbol!=data->Symbols.end(),"Actual parsed NP symbol missing in source mutation control");
                if(field==10) symbol->Type=1;
                if(field==11) symbol->Size=8;
            }
            wrong.Data=data;
        }
        deny(row,2,0,wrong);
    }
    // Mutable parsed-row edits cannot substitute an immutable source symbol.
    auto forged=image;
    forged.Imports[0].LibraryId=7; forged.Imports[0].ModuleId=8;
    deny(forged.Imports[0],2,0,forged);
    // Deliberately forged retained metadata: this is not a naturally parsed
    // altered source. Top-level/source identities and the mutable import row
    // remain valid, while the original scoped name disagrees. This isolates
    // OriginalRows' admission guard rather than an earlier profile rejection.
    forged=image;
    auto changedData=std::make_shared<Cpu::SceImageData>(*image.Data);
    constexpr std::string_view scopedName="XDncXQIJUSk#B#B";
    const auto isName=[](std::byte byte,char letter){return std::to_integer<unsigned char>(byte)==static_cast<unsigned char>(letter);};
    const auto name=std::search(changedData->Bytes.begin(),changedData->Bytes.end(),scopedName.begin(),scopedName.end(),isName);
    require(name!=changedData->Bytes.end(),"Original linked scoped NP name missing in retained-metadata control");
    require(std::search(name+scopedName.size(),changedData->Bytes.end(),scopedName.begin(),scopedName.end(),isName)==changedData->Bytes.end(),
            "Retained-metadata control requires one unambiguous scoped NP symbol name");
    constexpr std::string_view foreignNid="ABCDEFGHIJK";
    for(std::size_t n=0;n<foreignNid.size();++n) name[n]=static_cast<std::byte>(foreignNid[n]);
    forged.Data=changedData;
    deny(row,2,0,forged);

    // Modern TCG permits one Machine bridge context, so a concurrent foreign
    // Machine cannot exercise the provider's owner guard. Expiry still has a
    // real observable boundary using another scheduler on this same Machine.
    {
        auto expiring=std::make_shared<Cpu::GuestThreads>(machine);
        Provider orphan(machine,expiring,publicProfile(image));
        expiring.reset();
        bool expired=false;
        try { (void)orphan.Resolve(row,2,0,image); }
        catch(const std::exception& e) { expired=std::string_view(e.what()).find("scheduler expired")!=std::string_view::npos; }
        require(expired && mappingsEqual(before,machine.Mappings()),
                "Expired NP scheduler admitted or allocated the lazy provider gate");
    }
    const auto gate=candidate.Resolve(row,2,0,image);
    require(gate && candidate.Resolve(row,2,0,image)==gate,"Actual public NP source failed stable lazy resolution");
    machine.CheckAccess(*gate,1,Permission::Execute);
    std::cout << "scope: exact source/immutable parser/type/size/scoped symbol, lazy denial PASS\n";
}
void compiled(const char* path) {
    auto image=Cpu::ParseSce(path);
    const auto& row=actualRow(image);
    require(image.Type==0xfe10 && image.UnsupportedReasons.empty() &&
            std::set<std::uint32_t>(image.RelocationTypes.begin(),image.RelocationTypes.end())==std::set<std::uint32_t>{7,8},
            "Guest must retain real FUNC PLT and RELATIVE relocation contracts");
    const auto imported=std::find_if(image.Data->Symbols.begin(),image.Data->Symbols.end(),[](const auto& s){return s.Import.has_value();});
    require(imported!=image.Data->Symbols.end() && imported->Type==2 && imported->Size==0 && !imported->Section,
            "Linked NP import is not undefined STT_FUNC/size0");
    Cpu::Machine machine;
    require(std::string_view(Cpu::Machine::Backend()).find("Modern QEMU TCG")!=std::string_view::npos,
            "NP fixture requires native modern TCG");
    auto threads=std::make_shared<Cpu::GuestThreads>(machine);
    auto provider=std::make_unique<Provider>(machine,threads,publicProfile(image));
    std::uint64_t gate=0; unsigned resolved=0;
    // The graph requires a named host module and matching library version even
    // for a single-import public fixture. The exact resolver below admits only
    // this consumer's GetOnlineId NID; no synthetic PRX or graph bypass exists.
    const std::array hosts{Cpu::SceHostModule{"libSceNpManager.prx",
        {"libSceNpManager",1,1,1}, {{"libSceNpManager",1,1}}}};
    Cpu::SceModules modules(machine,{path,0x1000000},{},hosts,
        [](const Cpu::SceImport&,std::uint8_t)->std::optional<Cpu::SceResolvedImport> {
            throw std::runtime_error("Typed NP rejection incorrectly fell back to unqualified resolver");
        },std::nullopt,
        [&](const Cpu::SceImportConsumer& source,const Cpu::SceImport& request,std::uint8_t type,std::uint64_t size)
            ->std::optional<Cpu::SceResolvedImport> {
            require(source.Path==image.Path && source.SourceSha256==image.SourceSha256 && source.SourceSize==image.SourceSize,
                    "Module loader supplied a different consumer than the retained parsed image");
            require(request.Nid==row.Nid && request.LibraryId==row.LibraryId && request.ModuleId==row.ModuleId,
                    "Module loader changed actual scoped import identity");
            const auto result=provider->Resolve(request,type,size,image);
            if(!result) return std::nullopt;
            gate=*result; ++resolved;
            return Cpu::SceResolvedImport{*result,type,size};
        });
    require(resolved==1 && gate,"Consumer-aware module graph did not relocate the actual NP import");
    machine.Map(Data,4096,rw); machine.Map(Output,4096,rw); machine.Map(Stack,4096,rw);
    machine.Map(0x100000,4096,Permission::Read|Permission::Execute);
    std::array<std::byte,4096> canary{}; canary.fill(std::byte{0xa7});
    machine.Write(Output,canary); machine.Protect(Output,4096,Permission::Read);
    constexpr std::array saved{Register::Rbx,Register::Rbp,Register::R12,Register::R13,Register::R14,Register::R15};
    auto invoke=[&](std::uint64_t user,std::uint64_t output,std::uint64_t expected) {
        const std::array<std::uint64_t,3> args{user,output,Data+0x100};
        machine.Write(Data,std::as_bytes(std::span(args)));
        machine.Write(Stack,canary);
        constexpr std::uint64_t continuation=0x100000;
        constexpr std::uint64_t rsp=Stack+0xfe8;
        machine.Write(rsp,std::as_bytes(std::span(&continuation,1)));
        for(auto reg:saved) machine.Set(reg,0x1122334455667788ULL);
        machine.Set(Register::Rsp,rsp); machine.Set(Register::Rdi,Data);
        require(machine.Run(modules.Main().Entry,continuation,10000)==Cpu::StopReason::Address,
                "Actual linked NP guest did not return to its caller");
        require(machine.Get(Register::Rsp)==rsp+8,"Actual NP guest corrupted the caller stack");
        for(auto reg:saved) require(machine.Get(reg)==0x1122334455667788ULL,"NP guest/provider corrupted a preserved register");
        std::array<std::uint64_t,4> receipt{};
        machine.Read(Data+0x100,std::as_writable_bytes(std::span(receipt)));
        require(receipt[0]==expected && receipt[1]==expected && receipt[2]==1 && receipt[3]==0x4e504944454e5449ULL,
                "Actual NP CALL produced wrong signed result, low-EAX failure branch or continuation receipt");
        std::array<std::byte,4096> actual{}; machine.Read(Output,actual);
        require(actual==canary,"Signed-out query dereferenced/wrote fabricated online identity bytes");
        std::array<std::byte,3840> guard{}; machine.Read(Stack,guard);
        require(std::equal(guard.begin(),guard.end(),canary.begin()),"Guest call overwrote stack outside its call frames");
    };
    for(auto output:std::array<std::uint64_t,3>{Output+1,0xdead000,~0ULL})
        invoke(0xaabbccdd10000000ULL,output,0xffffffff80550006ULL);
    invoke(0x10000000,0,0xffffffff80550003ULL);
    invoke(0x10000001,Output+1,0xffffffff80550003ULL);
    invoke(~0ULL,Output+1,0xffffffff80550003ULL);
    provider.reset();
    bool gone=false;
    try { machine.CheckAccess(gate,1,Permission::Execute); } catch(const std::exception&) { gone=true; }
    require(gone,"Lazy NP provider teardown retained its owned gate page");
    auto config=publicProfile(image); config.SessionUserId=0x10000042;
    provider=std::make_unique<Provider>(machine,threads,config);
    require(provider->Resolve(row,2,0,image)==gate,"Replacement provider could not bind the released actual PLT address");
    invoke(0x10000000,Output+1,0xffffffff80550003ULL);
    invoke(0x10000042,Output+1,0xffffffff80550006ULL);
    std::cout << "compiled: real module-graph PLT/CALL, signed-out low-EAX branch, full identity page, read-only/unmapped output, high user bits, session replacement PASS\n";
}
}
int main(int argc,char** argv) {
    try {
        require(argc==3,"Usage: NativeNpIdentityTest NativeNpIdentityGuest.elf scope|compiled");
        const std::string_view mode=argv[2];
        if(mode=="scope") scope(argv[1]);
        else if(mode=="compiled") compiled(argv[1]);
        else throw std::runtime_error("Unknown native NP identity fixture mode");
        return 0;
    } catch(const std::exception& e) { std::cerr << "NativeNpIdentityTest: " << e.what() << '\n'; return 1; }
}
