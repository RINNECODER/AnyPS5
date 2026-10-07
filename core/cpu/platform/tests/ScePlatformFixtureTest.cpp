#include "../CandidateComponents.hpp"
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Test-audit contract: linked SCE scoped PLT/RELATIVE relocations reach real pointer-bearing
// Network/AJM state through ParseSce/LoadSce under native TCG, with fail-closed default admission.
// Regression: unresolved/mis-scoped PLT, wrong bias, host pointer casts, 8-byte AJM output,
// or finalize-success stubs fail independent C/host byte, error, lifetime and stack oracles.
// Existing flat component probes bypass SCE parsing/relocation; SceElf tests use other providers.
// No production seam: the fixture verifies its own linked import types below; SceImport still
// lacks type and production integration must retain its independent typed-import prerequisite.
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
constexpr std::array<const char*,3> scoped{
    "YuOW3dDAKYc#B#B", "dl+4eHSzUu4#C#C", "MHur6qCsUus#C#C"};

// Narrow fixture-only type reader. Never derive a production import type from a NID match.
void verifyLinkedFunctionTypes(const char* path) {
    std::ifstream stream(path,std::ios::binary);
    require(bool(stream),"Cannot read packaged platform fixture");
    const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(stream),{}};
    auto read = [&](std::uint64_t offset,std::size_t count) {
        require(count <= 8 && offset <= bytes.size() && count <= bytes.size()-offset,
                "Fixture typed metadata exceeds file");
        std::uint64_t value=0;
        for (std::size_t i=0;i<count;++i) value |= std::uint64_t(bytes[offset+i]) << (8*i);
        return value;
    };
    require(read(0,4)==0x464c457f && read(4,3)==0x010102 && read(18,2)==62,
            "Typed fixture must be linked little-endian x86 ELF64");
    require(read(54,2)==56,"Unexpected fixture program header layout");
    const auto phoff=read(32,8), count=read(56,2);
    std::uint64_t dynamic=0,dynamicSize=0,data=0,dataSize=0;
    for (std::uint64_t i=0;i<count;++i) {
        const auto p=phoff+i*56, type=read(p,4);
        if (type==2) { require(!dynamic,"Duplicate fixture dynamics"); dynamic=read(p+8,8); dynamicSize=read(p+32,8); }
        if (type==0x61000000) { require(!data,"Duplicate fixture SCE data"); data=read(p+8,8); dataSize=read(p+32,8); }
    }
    require(dynamic && data && dynamicSize%16==0 && data<=bytes.size() && dataSize<=bytes.size()-data,
            "Missing fixture SCE dynamic metadata");
    std::map<std::uint64_t,std::uint64_t> tags;
    bool terminated=false;
    for (std::uint64_t i=0;i<dynamicSize;i+=16) {
        const auto tag=read(dynamic+i,8), value=read(dynamic+i+8,8);
        if (!tag) { terminated=true; break; }
        // Identity tables legitimately repeat their tag. Only copy the symbol/string shape.
        if (tag==0x61000035 || tag==0x61000037 || tag==0x61000039 ||
            tag==0x6100003b || tag==0x6100003f)
            require(tags.emplace(tag,value).second,"Duplicate fixture symbol/string metadata");
    }
    require(terminated && tags.size()==5 && tags.at(0x6100003b)==24,
            "Missing fixture typed symbol metadata");
    const auto symOffset=tags.at(0x61000039), symSize=tags.at(0x6100003f);
    const auto strOffset=tags.at(0x61000035), strSize=tags.at(0x61000037);
    require(symSize%24==0 && symOffset<=dataSize && symSize<=dataSize-symOffset &&
            strOffset<=dataSize && strSize<=dataSize-strOffset,"Invalid fixture typed table bounds");
    std::set<std::string> found;
    for (std::uint64_t i=24;i<symSize;i+=24) {
        const auto p=data+symOffset+i;
        if (read(p+6,2)) continue;
        require(read(p+4,1)==0x12 && !read(p+5,1) && !read(p+8,8) && !read(p+16,8),
                "Fixture undefined import did not retain genuine GLOBAL STT_FUNC");
        const auto offset=read(p,4);
        require(offset<strSize,"Fixture import name exceeds table");
        std::string name;
        std::uint64_t end=offset;
        for (;end<strSize && read(data+strOffset+end,1);++end)
            name.push_back(static_cast<char>(read(data+strOffset+end,1)));
        require(end<strSize,"Unterminated fixture typed import");
        require(found.insert(name).second,"Duplicate fixture import");
    }
    require(found==std::set<std::string>(scoped.begin(),scoped.end()),"Unadmitted fixture typed import");
}

struct Receipt {
    std::uint64_t marker,required;
    std::uint32_t before,a,after,b;
    std::uint32_t query,shortOutput,escape,nullInput;
    std::uint32_t reservedInit,nullInit,finalizeA,staleA,finalizeB;
    std::array<unsigned char,32> output;
};
static_assert(offsetof(Receipt,output)==68 && sizeof(Receipt)==104);

void exercise(const char* path,bool baseline) {
    verifyLinkedFunctionTypes(path);
    const auto parsed=Cpu::ParseSce(path);
    if (parsed.Type!=0xfe10 || parsed.Imports.size()!=3 || !parsed.UnsupportedReasons.empty()) {
        std::string detail="Platform fixture lost its supported SCE import surface: type="+
            std::to_string(parsed.Type)+", imports="+std::to_string(parsed.Imports.size());
        for (const auto& reason:parsed.UnsupportedReasons) detail+="; "+reason;
        throw std::runtime_error(detail);
    }
    require(parsed.NeededFiles.empty() && parsed.NeededModules.size()==2,
            "Fixture dependency identities were lost or synthetic files introduced");
    require(std::set<std::uint32_t>(parsed.RelocationTypes.begin(),parsed.RelocationTypes.end())==
            std::set<std::uint32_t>{7,8} && parsed.RelocationCount>=4,
            "Genuine platform PLT/RELATIVE relocation set was lost");
    const std::map<std::string,std::string> expected{
        {"YuOW3dDAKYc","libSceHttp"},{"dl+4eHSzUu4","libSceAjm"},{"MHur6qCsUus","libSceAjm"}};
    std::set<std::string> seen;
    for (const auto& import:parsed.Imports) {
        const auto found=expected.find(import.Nid);
        require(found!=expected.end() && import.LibraryName==found->second && import.ModuleName==found->second &&
                import.LibraryVersion==1 && import.ModuleMajor==1 && import.ModuleMinor==1 &&
                import.LibraryId==(found->second=="libSceHttp"?1:2) && import.ModuleId==import.LibraryId,
                "Parsed platform import scope/version mismatched its typed fixture");
        require(seen.insert(import.Nid).second,"Duplicate parsed platform import");
    }
    Cpu::Machine machine;
    require(std::string(Cpu::Machine::Backend()).find("Modern QEMU TCG")!=std::string::npos,
            "SCE platform fixture requires modern native TCG");
    Cpu::Platform::CandidateComponents::Configuration configuration;
    if (!baseline) configuration.PublicAbiFixtures={Cpu::Platform::CandidateFamily::Network,
                                                    Cpu::Platform::CandidateFamily::Audio};
    auto components=std::make_unique<Cpu::Platform::CandidateComponents>(machine,configuration);
    std::set<std::uint64_t> resolvedGates;
    const auto image=Cpu::LoadSce(machine,path,0x1000000,[&](const Cpu::SceImport& import) {
        // Type 2 is supplied only after this test read the packaged STT_FUNC bits itself.
        const auto gate=components->Resolve(import,2);
        if (gate) resolvedGates.insert(*gate);
        return gate.value_or(0);
    });
    require(!baseline,"Default components unexpectedly admitted the fixture imports");
    require(resolvedGates.size()==3,"Loader did not resolve each real scoped import");
    constexpr std::uint64_t output=0x4000000,stack=0x5000000,stop=0x6000000;
    const auto rw=Cpu::Permission::Read|Cpu::Permission::Write;
    machine.Map(output,4096,rw); machine.Map(stack,4096,rw);
    machine.Map(stop,4096,Cpu::Permission::Read|Cpu::Permission::Execute);
    const std::array<std::byte,sizeof(Receipt)> untouched=[] {
        std::array<std::byte,sizeof(Receipt)> v{}; v.fill(std::byte{0xcc}); return v;
    }();
    machine.Write(output,untouched);
    machine.Write(stack+4088,std::as_bytes(std::span(&stop,1)));
    machine.Set(Cpu::Register::Rsp,stack+4088); machine.Set(Cpu::Register::Rdi,output);
    machine.Set(Cpu::Register::Rbx,0xcafef00d12345678ULL);
    require(machine.Run(image.Entry,stop,100000)==Cpu::StopReason::Address,"Linked SCE guest did not return");
    require(machine.Get(Cpu::Register::Rax)==0,"Linked SCE guest URI/AJM contract oracle failed");
    require(machine.Get(Cpu::Register::Rsp)==stack+4096 &&
            machine.Get(Cpu::Register::Rbx)==0xcafef00d12345678ULL,"Linked SCE guest corrupted caller state");
    Receipt receipt{};
    machine.Read(output,std::as_writable_bytes(std::span(&receipt,1)));
    require(receipt.marker==0x534345504c41544fULL && receipt.required==17 &&
            receipt.query==0 && receipt.shortOutput==0x80431022 && receipt.escape==0 && receipt.nullInput==0x804311fe,
            "Host URI marker/size/error oracle failed");
    auto expectedOutput=std::array<unsigned char,32>{}; expectedOutput.fill(0xa7);
    constexpr char encoded[]="Az0-_.~%20%2F%FF";
    std::memcpy(expectedOutput.data(),encoded,sizeof(encoded));
    require(receipt.output==expectedOutput,"Host exact URI bytes/NUL/untouched tail oracle failed");
    require(receipt.before==0xa7a7a7a7 && receipt.after==0x5e5e5e5e && receipt.a && receipt.b && receipt.a!=receipt.b &&
            receipt.reservedInit==0x80930005 && receipt.nullInit==0x80930005 && receipt.finalizeA==0 &&
            receipt.staleA==0x80930002 && receipt.finalizeB==0,"Host AJM context width/lifecycle/error oracle failed");
    components.reset();
    for (const auto gate:resolvedGates) {
        bool unmapped=false;
        try { machine.CheckAccess(gate,1,Cpu::Permission::Execute); }
        catch (const std::exception& e) {
            require(std::string(e.what()).find("Guest access denied")!=std::string::npos,
                    "Provider cleanup rejected for an unrelated reason");
            unmapped=true;
        }
        require(unmapped,"Component destruction retained executable gate storage");
    }
}
}
int main(int argc,char** argv) {
    bool baseline=false;
    try {
        require(argc==2 || (argc==3 && std::string(argv[2])=="--baseline"),
                "Usage: ScePlatformFixtureTest packaged.elf [--baseline]");
        baseline=argc==3;
        exercise(argv[1],baseline);
        std::cout << "PASS linked SCE public Network/AJM fixture: scoped typed PLT/RELATIVE, URI bytes, "
                     "AJM contexts/stale cleanup, guest stack and provider lifetime\n";
        return 0;
    } catch (const std::exception& error) {
        if (baseline && std::string(error.what()).find("unresolved import")!=std::string::npos)
            std::cerr << "Unsupported provider (expected default-admission baseline): ";
        std::cerr << error.what() << '\n';
        return 1;
    }
}
