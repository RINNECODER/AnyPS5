#include <cpu/GuestThreads.hpp>
#include <cpu/SceImports.hpp>
#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceThreadImports.hpp>
#include <array>
#include <iostream>
#include <stdexcept>
#include <thread>

// Test-audit: existing linked ThreadHomebrew supplies real dependency init/fini,
// entry, child progress and TLS. Its existing test has no external owner hook.
// Hook omission in a phase, ownership migration, reentry or per-slice budget
// reset changes these assertions without a test-only production entrypoint.
namespace {
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
struct OwnerGraph {
    Cpu::Machine machine;
    std::shared_ptr<Cpu::GuestThreads> threads=std::make_shared<Cpu::GuestThreads>(machine);
    Cpu::SceImports libc{machine};Cpu::SceThreadImports imports{machine,threads};
    Cpu::SceLifecycleImports lifecycle{machine};
    std::unique_ptr<Cpu::SceModules> graph;
    std::uint64_t stateAddress=0;unsigned initialization=0,entry=0,finalization=0;
    bool initializing=true,reentryRejected=false;
    const auto static constexpr MainBias=0x1000000ULL,DependencyBias=0x2000000ULL;
    OwnerGraph(const char* main,const char* dependency) {
        const auto exit=[this](int value){threads->ProcessExitFromHostCall(value);};
        libc.SetProcessExitHandler(exit);lifecycle.SetProcessExitHandler(exit);
        const std::array dependencies{Cpu::SceModuleFile{dependency,DependencyBias}};
        const std::array hosts{
            Cpu::SceHostModule{"libkernel.prx",{"libkernel",0,1,1},{{"libkernel",0,1}}},
            Cpu::SceHostModule{"libc.prx",{"libc",0,1,1},{{"libc",0,1}}}};
        graph=std::make_unique<Cpu::SceModules>(machine,Cpu::SceModuleFile{main,MainBias},dependencies,hosts,
            [&](const auto& row,std::uint8_t type)->std::optional<Cpu::SceResolvedImport>{
                require(type==2,"Owner graph import lost function type");
                if(const auto gate=imports.Resolve(row,type))return Cpu::SceResolvedImport{*gate,type};
                if(const auto gate=lifecycle.Resolve(row))return Cpu::SceResolvedImport{*gate,type};
                return Cpu::SceResolvedImport{libc.Resolve(row),type};
            });
        for(const auto& item:graph->Modules()[1].Image.Exports)
            if(item.Identity.Nid=="ij1xrwBH1O0" && item.Type==1 && item.Size==24*8)
                stateAddress=DependencyBias+item.Value;
        require(stateAddress && graph->Modules()[1].Init && graph->Modules()[1].Fini,
                "Owner graph lacks actual lifecycle exports");
        Cpu::SetupSceEntry(machine,graph->Main(),{"thread-homebrew","0","33644544"},graph->EntryTerminationGate());
        threads->AdoptInitial({graph->Main().Entry,graph->InitialStack(),graph->Tls(),graph->ThreadTlsFactory()});
        const auto owner=std::this_thread::get_id();
        threads->SetOwnerBoundary([this,owner](bool){
            threads->CheckIdleOwner();require(owner==std::this_thread::get_id(),"Owner graph migrated CPU ownership");
            if(initializing)++initialization;
            else if(state()[1])++finalization;
            else ++entry;
            if(!reentryRejected) {
                Cpu::GuestPhaseBudget attempt(1);
                try {threads->RunEntry(attempt);}
                catch(const std::exception& error) {
                    require(std::string(error.what()).find("nested public guest execution")!=std::string::npos,error.what());
                    require(attempt.Consumed()==0,"Nested owner hook executed guest instructions");reentryRejected=true;
                }
                require(reentryRejected,"Owner boundary accepted reentrant guest execution");
            }
        },std::chrono::milliseconds(500));
        graph->SetExecutor(threads->ModuleExecutor());
        graph->InitializeDependencies(0x1020304050607080ULL,0x8877665544332211ULL,0xabcdef0198765432ULL);
        initializing=false;
    }
    ~OwnerGraph(){threads->Withdraw();}
    std::array<std::uint64_t,24> state() const {
        std::array<std::uint64_t,24> value{};machine.Read(stateAddress,std::as_writable_bytes(std::span(value)));return value;
    }
};
}
int main(int argc,char** argv) {
    try {
        require(argc==3,"Usage: KernelEventsOwnerTest thread-main.elf ThreadGuest.prx");
        {
            OwnerGraph s(argv[1],argv[2]);
            require(s.graph->RunMain(2000000,100000)==Cpu::StopReason::Exit && s.machine.ExitCode()==0,
                    "Actual owner graph did not complete init/main/fini");
            const auto state=s.state();
            require(s.initialization && s.entry>2 && s.finalization && s.reentryRejected &&
                    state[0]==1 && state[1]==1 && state[2]==2 && state[23]==0,
                    "Owner hook missed graph lifecycle phase or changed its actual result");
        }
        {
            OwnerGraph s(argv[1],argv[2]);Cpu::GuestPhaseBudget budget(8193);
            require(s.threads->RunEntry(budget)==Cpu::StopReason::InstructionLimit &&
                    budget.Consumed()==8193 && budget.Remaining()==0 && s.entry>=3 && s.state()[1]==0,
                    "Between-slice boundary refreshed cumulative phase budget or finalized prematurely");
        }
        std::cout<<"PASS persistent idle owner across actual graph init/main/fini, reentry rejection and cumulative8193 instruction phase\n";
    }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
