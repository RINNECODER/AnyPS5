#include <cpu/SceVideoOutImports.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Authoring gate: owner of production admission and success-only status output.
// Missing ELF metadata/default qualification or >=0-as-success widens execution
// or corrupts caller memory. Existing fixtures exercise public synthetic routes
// and native success0 only. No production seam is introduced.
namespace {
constexpr auto RW=Cpu::Permission::Read|Cpu::Permission::Write;
constexpr auto RX=Cpu::Permission::Read|Cpu::Permission::Execute;
constexpr std::uint64_t Data=0x100002000ULL,Attr=Data+128,Rows=Data+256,Status=Data+384;
void require(bool value,const char* text) { if(!value)throw std::runtime_error(text); }
template<class F> void rejects(F&& f,const char* text) {
    try{f();}catch(const std::exception& e){require(std::string(e.what()).find(text)!=std::string::npos,e.what());return;}
    throw std::runtime_error(std::string("Missing admission rejection: ")+text);
}
Cpu::SceImport identity(const char* nid) {
    Cpu::SceImport value;value.Nid=nid;value.LibraryName=value.ModuleName="libSceVideoOut";
    value.LibraryVersion=1;value.ModuleMajor=value.ModuleMinor=1;value.LibraryId=39;value.ModuleId=40;return value;
}
template<class T> void store(Cpu::Machine& m,std::uint64_t address,const T& value){m.Write(address,std::as_bytes(std::span(&value,1)));}
struct Caller {
    Cpu::Machine machine;
    std::array<std::vector<char>,7> sections;
    explicit Caller(const char* const* files) {
        machine.Map(0x1000,4096,RX);machine.Map(0x4000,4096,RX);machine.Map(0x5000,4096,RW);machine.Map(Data,4096,RW);
        for(unsigned i=0;i<7;++i){std::ifstream input(files[i],std::ios::binary);require(input.good(),"Missing typed caller");
            sections[i]=std::vector<char>((std::istreambuf_iterator<char>(input)),{});require(!sections[i].empty()&&sections[i].size()<4096,"Caller size outside bound");}
    }
    std::uint64_t call(unsigned service,std::uint64_t gate,const std::array<std::uint64_t,8>& args) {
        machine.Write(0x4000,std::as_bytes(std::span(sections[service])));store(machine,Data,args);
        machine.Set(Cpu::Register::Rdi,gate);machine.Set(Cpu::Register::Rsi,Data);machine.Set(Cpu::Register::Rsp,0x5fc8);
        store(machine,0x5fc8,std::uint64_t{0x1000});
        require(machine.Run(0x4000,0x1000,1000)==Cpu::StopReason::Address&&machine.Get(Cpu::Register::Rsp)==0x5fd0,"Typed caller did not return");
        return machine.Get(Cpu::Register::Rax);
    }
};
void run(const char* const* files) {
    Caller guest(files);
    std::int32_t statusResult=1;unsigned statusCalls=0;
    Cpu::SceVideoOutBackend callbacks;
    callbacks.GetOutputStatus=[&](std::int32_t handle){require(handle==37,"Status handle width differs");++statusCalls;
        Cpu::SceVideoOutStatus status;status.Resolution=99;status.Flags=0x1122334455667788ULL;return Cpu::SceVideoOutStatusResult{statusResult,status};};
    Cpu::SceVideoOutImports publicImports(guest.machine,callbacks);
#ifdef GPU07_OLD_ROUTES
    auto statusGate=publicImports.Resolve(identity("utPrVdxio-8"));
#else
    auto statusGate=publicImports.ResolvePublicFixture(identity("utPrVdxio-8"));
#endif
    std::array<std::byte,48> sentinel;sentinel.fill(std::byte{0xa5});guest.machine.Write(Status,sentinel);
    require(guest.call(1,statusGate,{37,Status})==1,"Positive status result was not preserved");
    std::array<std::byte,48> after;guest.machine.Read(Status,after);
    require(after==sentinel&&statusCalls==1,"Positive non-success status overwrote caller output");
    statusResult=-7;require(static_cast<std::uint32_t>(guest.call(1,statusGate,{37,Status}))==static_cast<std::uint32_t>(-7),"Negative status result width differs");
    guest.machine.Read(Status,after);require(after==sentinel&&statusCalls==2,"Negative status result changed caller output");
#ifndef GPU07_OLD_ROUTES
    rejects([&]{publicImports.Resolve(identity("utPrVdxio-8"),2,0);},"no qualified admission");
    const auto admissions=Cpu::TargetVideoOutAdmissions();
    require(!admissions.empty(),"Title-agnostic VideoOut profile is empty");
    Cpu::SceVideoOutImports unavailable(guest.machine,{},0x7ffdfc000000,admissions);
    rejects([&]{unavailable.Resolve(identity("utPrVdxio-8"),2,0);},"without native backend");
    Cpu::SceVideoOutImports target(guest.machine,callbacks,0x7ffdfb000000,admissions);
    for(const auto type:std::array<std::uint8_t,2>{0,1})rejects([&]{target.Resolve(identity("utPrVdxio-8"),type,0);},"ELF symbol");
    rejects([&]{target.Resolve(identity("utPrVdxio-8"),2,8);},"ELF symbol");
    auto wrong=identity("utPrVdxio-8");wrong.LibraryName="libSceVideoOutForeign";rejects([&]{target.Resolve(wrong,2,0);},"scope/version");
    // Import-table ids are per-image; any title's ids are admitted.
    wrong=identity("utPrVdxio-8");wrong.LibraryId=3;wrong.ModuleId=4;
    require(target.Resolve(wrong,2,0)!=0,"Another image's import-table ids were refused");
    wrong=identity("utPrVdxio-8");wrong.ModuleMinor=2;rejects([&]{target.Resolve(wrong,2,0);},"scope/version");
    statusResult=0;const auto qualifiedStatus=target.Resolve(identity("utPrVdxio-8"),2,0);
    require(guest.call(1,qualifiedStatus,{37,Status})==0&&statusCalls==3,"Qualified status0 was not callable");
    const auto attrGate=target.Resolve(identity("PjS5uASwcV8"),2,0);
    guest.call(3,attrGate,{Attr,0x8000000000000000ULL,0,1920,1080,0,0,0});
    Cpu::SceVideoOutAttribute attribute;guest.machine.Read(Attr,std::as_writable_bytes(std::span(&attribute,1)));
    require(attribute.Width==1920&&attribute.Height==1080&&attribute.TilingMode==0&&attribute.PixelFormat==0x8000000000000000ULL,"Qualified attribute field bytes differ");
    guest.call(3,attrGate,{Attr,0x8100070422000000ULL,0,1920,1080,0,0,0});
    guest.machine.Read(Attr,std::as_writable_bytes(std::span(&attribute,1)));
    require(attribute.PixelFormat==0x8100070422000000ULL&&attribute.TilingMode==0,
        "Exact packed PQ format was not preserved by compiled qualified setter");
    const auto attrBefore=attribute;
    for(const auto args:std::array<std::array<std::uint64_t,8>,4>{{
        {Attr,0x8100070422000001ULL,0,1920,1080,0,0,0},
        {Attr,0x8100070522000000ULL,0,1920,1080,0,0,0},
        {Attr,0x8100060422000000ULL,0,1920,1080,0,0,0},
        {Attr,0x8000000000000000ULL,1,1920,1080,0,0,0}}}) {
        rejects([&]{guest.call(3,attrGate,args);},"outside qualified target use");
        guest.machine.Read(Attr,std::as_writable_bytes(std::span(&attribute,1)));
        require(std::memcmp(&attribute,&attrBefore,sizeof(attribute))==0,"Unqualified attribute call partially wrote output");
    }
    unsigned registrations=0;std::uint64_t expectedFormat=0;callbacks.RegisterBuffers=[&](auto h,auto group,auto start,auto rows,const auto& a,auto category){
        require(h==37&&group==0&&start==0&&rows.size()==3&&category==0&&a.Width==1920&&
            a.PixelFormat==expectedFormat,"Qualified registration marshalling differs");++registrations;return -71;};
    Cpu::SceVideoOutImports registered(guest.machine,callbacks,0x7ffdfa000000,admissions);
    const auto registerGate=registered.Resolve(identity("rKBUtgRrtbk"),2,0);
    const std::array<Cpu::SceVideoOutBuffer,3> rows{{{0x400000000ULL,0,{}},{0x400100000ULL,0,{}},{0x400200000ULL,0,{}}}};store(guest.machine,Rows,rows);
    for(const auto format:std::array<std::uint64_t,2>{0x8000000000000000ULL,0x8100070422000000ULL}) {
        auto accepted=attribute;accepted.PixelFormat=format;store(guest.machine,Attr,accepted);expectedFormat=format;
        require(static_cast<std::uint32_t>(guest.call(4,registerGate,{37,0,0,Rows,3,Attr,0,0}))==static_cast<std::uint32_t>(-71),"Qualified register result differs");
    }
    require(registrations==2,"Qualified SDR/PQ registration callback count differs");
    for(const auto format:std::array<std::uint64_t,3>{0x8100070422000001ULL,0x8100070522000000ULL,0x8100060422000000ULL}) {
        auto unsupported=attribute;unsupported.PixelFormat=format;store(guest.machine,Attr,unsupported);
        rejects([&]{guest.call(4,registerGate,{37,0,0,Rows,3,Attr,0,0});},"outside qualified target use");
        require(registrations==2,"Unknown packed format reached native registration callback");
    }
    store(guest.machine,Attr,attribute);
    rejects([&]{guest.call(4,registerGate,{37,0,0,Rows,2,Attr,0,0});},"outside qualified target use");
    require(registrations==2,"Expanded registration shape reached native callback");
#endif
    std::cout<<"PASS compiled status success-only writes; source/type/scope/default/capability admission; bounded target attribute80/register3\n";
}
}
int main(int argc,const char* argv[]){try{require(argc==8,"Admission fixture needs seven typed callers");run(argv+1);return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
