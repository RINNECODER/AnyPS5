#import <AppKit/AppKit.h>
#include <cpu/NativeModuleRunner.hpp>
#include <cpu/GuestThreads.hpp>
#include <cpu/SceImports.hpp>
#include <cpu/SceUpstreamPrxBridge.hpp>
#include <cpu/SceElf.hpp>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <tuple>
#include <unistd.h>

namespace {
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
Cpu::SceImportConsumer consumer(const Cpu::SceParsedImage& image) {
    return {image.Path,image.SourceSize,image.SourceSha256};
}
Cpu::NativeModuleRunnerConfiguration configuration(const Cpu::SceParsedImage& image,const char* utility,bool np) {
    Cpu::NativeModuleRunnerConfiguration c;
    c.UtilityMetallib=utility;c.Width=c.Height=64;c.WindowTitle="Native service public integration";
    c.MaximumWallTime=std::chrono::milliseconds(30000);c.MaximumIdleWait=std::chrono::milliseconds(5000);
    const Cpu::NativeServiceConsumerProfile p{image.SourceSha256,image.SourceSize};
    if(np)c.PublicNpIdentity=p;else c.PublicUriEscape=p;
    return c;
}
Cpu::SceImport service(const Cpu::SceParsedImage& image,bool np) {
    const auto nid=np?"XDncXQIJUSk":"YuOW3dDAKYc";
    for(const auto& row:image.Imports)if(row.Nid==nid)return row;
    throw std::runtime_error("Genuine linked service import missing");
}
auto mappings(const Cpu::Machine& machine) {
    std::vector<std::tuple<std::uint64_t,std::uint64_t,Cpu::Permission>> result;
    for(const auto& m:machine.Mappings())result.emplace_back(m.Address,m.Size,m.Permissions);
    return result;
}
template<class Action> void rejects(Action action,const char* contract) {
    bool refused=false;
    try{action();}catch(const std::exception&){refused=true;}
    require(refused,contract);
}
void compiled(const char* mainPath,const char* depPath,const char* utility,bool np) {
    Cpu::Machine machine;
    auto threads=std::make_shared<Cpu::GuestThreads>(machine);
    const auto image=Cpu::ParseSce(mainPath),dep=Cpu::ParseSce(depPath);
    Cpu::NativeModuleRunner runner(machine,threads,consumer(image),configuration(image,utility,np));
    runner.RegisterParsedConsumer(image);runner.RegisterParsedConsumer(dep);
    Cpu::SceImports libc(machine);
    libc.SetProcessExitHandler([threads](int status){threads->ProcessExitFromHostCall(status);});
    Cpu::SceUpstreamPrxBridge local(machine);
    std::vector<Cpu::SceHostModule> hosts{{"libc.prx",{"libc",0,1,1},{{"libc",0,1}}}};
    runner.AddHostModules(hosts);
    const std::array deps{Cpu::SceModuleFile{depPath,0x2000000}};
    unsigned reached=0;
    const auto expected=service(image,np);
    Cpu::SceModules graph(machine,{mainPath,0x1000000},deps,hosts,{},std::nullopt,
        [&](const auto& actual,const auto& row,std::uint8_t type,std::uint64_t size)->std::optional<Cpu::SceResolvedImport>{
            if(auto selected=runner.Resolve(actual,row,type,size)) {
                if(row.Nid==expected.Nid)++reached;
                return selected;
            }
            if(row.Nid==expected.Nid)
                throw std::runtime_error("Production service import reached untyped fallback");
            if(type!=2||size)return std::nullopt;
            if(auto gate=local.Resolve(row))return Cpu::SceResolvedImport{*gate,2};
            return Cpu::SceResolvedImport{libc.Resolve(row),2};
        });
    require(reached==1,"Actual SceModules production owner omitted typed service route");
    Cpu::SetupSceEntry(machine,graph.Main(),{mainPath},graph.EntryTerminationGate());
    threads->AdoptInitial({graph.Main().Entry,graph.InitialStack(),graph.Tls(),graph.ThreadTlsFactory()});
    graph.SetExecutor(threads->ModuleExecutor());runner.ActivateBeforeInitializers();
    const auto published=mappings(machine);
    rejects([&]{runner.RegisterParsedConsumer(image);},"Active owner accepted late parsed source registration");
    require(mappings(machine)==published,"Late registration mutated active owner mappings");
    graph.InitializeDependencies();
    require(graph.RunMain()==Cpu::StopReason::Exit&&machine.ExitCode()==0,
        "Compiled parent-route guest failed independent NP/URI golden checks or libc exit");
    const auto gate=runner.Resolve(consumer(image),expected,2,0);
    require(bool(gate),"Successful service route lost cached gate");
    const auto address=gate->Address;
    machine.CheckAccess(address,1,Cpu::Permission::Execute);
    runner.Shutdown();runner.Shutdown();
    rejects([&]{machine.CheckAccess(address,1,Cpu::Permission::Execute);},"Idle shutdown retained owned service gate");
    rejects([&]{runner.Resolve(consumer(image),expected,2,0);},"Shutdown owner accepted service dispatch");
}
void admission(const char* mainPath,const char* utility,bool np,const std::string& mode) {
    const auto original=Cpu::ParseSce(mainPath);
    const auto row=service(original,np);
    if(mode=="admission") {
        Cpu::Machine machine;auto threads=std::make_shared<Cpu::GuestThreads>(machine);
        Cpu::NativeModuleRunner runner(machine,threads,consumer(original),configuration(original,utility,np));
        const auto before=mappings(machine);
        unsigned fallback=0;
        auto dispatch=[&](const auto& source,const auto& import,std::uint8_t type,std::uint64_t size){
            if(auto gate=runner.Resolve(source,import,type,size))return *gate;
            ++fallback;return Cpu::SceResolvedImport{0x1234,2};
        };
        rejects([&]{dispatch(consumer(original),row,2,0);},"Unregistered source reached untyped fallback");
        runner.RegisterParsedConsumer(original);
        auto source=consumer(original);source.SourceSha256[0]^=std::byte{1};
        rejects([&]{dispatch(source,row,2,0);},"Wrong source hash reached untyped fallback");
        source=consumer(original);source.Path += ".different";
        rejects([&]{dispatch(source,row,2,0);},"Same hash and size from unregistered path reached untyped fallback");
        source=consumer(original);++source.SourceSize;
        rejects([&]{dispatch(source,row,2,0);},"Wrong source size reached untyped fallback");
        rejects([&]{dispatch(consumer(original),row,1,0);},"Non-FUNC service reached untyped fallback");
        rejects([&]{dispatch(consumer(original),row,2,8);},"Nonzero service symbol size reached untyped fallback");
        auto malformed=row;++malformed.LibraryId;
        rejects([&]{dispatch(consumer(original),malformed,2,0);},"Wrong scoped row reached untyped fallback");
        require(fallback==0&&mappings(machine)==before,"Rejected service allocated gates or invoked untyped fallback");
        runner.Shutdown();
    }
    if(mode=="retained") {
        // Fresh ParseSce of a genuine encoded wrong NID, with only its editable
        // row changed. The retained encoded row must still refuse admission.
        auto wrong=Cpu::ParseSce(std::filesystem::path(mainPath).parent_path()/"wrong-row"/std::filesystem::path(mainPath).filename());
        for(auto& candidate:wrong.Imports)if(candidate.Nid=="AAAAAAAAAAA")candidate=row;
        Cpu::Machine machine;auto threads=std::make_shared<Cpu::GuestThreads>(machine);
        Cpu::NativeModuleRunner runner(machine,threads,consumer(wrong),configuration(wrong,utility,np));
        runner.RegisterParsedConsumer(wrong);
        const auto before=mappings(machine);
        rejects([&]{runner.Resolve(consumer(wrong),row,2,0);},
            "Mutable import row bypassed authoritative retained symbol");
        require(mappings(machine)==before,"Retained-row mismatch allocated service gate");
        runner.Shutdown();
    }
    if(mode=="snapshot") {
        const auto dir=std::filesystem::temp_directory_path()/ ("anyps5-service-snapshot-"+std::to_string(getpid())+(np?"-np":"-uri"));
        std::filesystem::create_directory(dir);
        struct Cleanup { std::filesystem::path Path;~Cleanup(){std::error_code e;std::filesystem::remove_all(Path,e);} } cleanup{dir};
        const auto path=dir/std::filesystem::path(mainPath).filename();
        std::filesystem::copy_file(mainPath,path);
        auto parsed=Cpu::ParseSce(path);const auto source=consumer(parsed);
        Cpu::Machine machine;auto threads=std::make_shared<Cpu::GuestThreads>(machine);
        Cpu::NativeModuleRunner runner(machine,threads,source,configuration(parsed,utility,np));
        runner.RegisterParsedConsumer(parsed);
        // Caller-owned vectors and source bytes at the path can both change.
        parsed.Imports.clear();parsed.Data.reset();parsed.SourceSha256.fill(std::byte{});parsed.SourceSize=0;
        {std::ofstream replaced(path,std::ios::binary|std::ios::trunc);replaced<<"replaced public fixture";}
        const auto gate=runner.Resolve(source,row,2,0);
        require(bool(gate),"Registered consumer snapshot aliased mutable caller or reread guest path");
        const auto address=gate->Address;
        runner.Shutdown();
        rejects([&]{machine.CheckAccess(address,1,Cpu::Permission::Execute);},"Snapshot admitted gate survived idle teardown");
    }
}
}
int main(int argc,char** argv) {
    @autoreleasepool {try {
        require(argc==6,"Usage: native-service-test main.elf dependency.prx utility.metallib np-identity|http-uri compiled|admission|retained|snapshot");
        const bool np=std::string(argv[4])=="np-identity";
        require(np||std::string(argv[4])=="http-uri","Unknown public profile");
        if(std::string(argv[5])=="compiled")compiled(argv[1],argv[2],argv[3],np);
        else {const std::string mode=argv[5];require(mode=="admission"||mode=="retained"||mode=="snapshot","Unknown owner-boundary control");admission(argv[1],argv[3],np,mode);}
        std::cout<<"Production native service "<<argv[4]<<' '<<argv[5]<<" PASS; explicit public fixture only\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
}
