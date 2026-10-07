#include "KernelPrimitives.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceThreadImports.hpp>
#include <algorithm>
#include <array>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

// Test-audit: independent peer counters from actual non-yielding guest loops
// expose policy3 admission with FIFO scheduling, FIFO rotation, lost priority or
// inheritance. The legacy priority fixture yields explicitly and rejects RR.
// Actual mutex donation/blocked ABI and odd split budgets cover the new RR
// interaction; existing FIFO controls cannot expose its residual quantum risk.
// No new production seam: all execution uses real linked SCE gates and Machine.
namespace {
constexpr std::uint64_t Bias=0x1000000,Canary=0x1badc0ffeef00d55ULL,Iterations=100000;
using State=std::array<std::uint64_t,128>;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F action,const char* text) {
 try {action();} catch(const std::exception& error) {
  require(std::string(error.what()).find(text)!=std::string::npos,error.what());return;
 }
 throw std::runtime_error(std::string("Missing RR rejection: ")+text);
}
struct Session {
 Cpu::Machine machine;
 std::shared_ptr<Cpu::GuestThreads> threads=std::make_shared<Cpu::GuestThreads>(machine);
 Cpu::SceThreadImports imports{machine,threads};
 Cpu::SceLifecycleImports lifecycle{machine};
 std::unique_ptr<Cpu::Platform::KernelPrimitives> mutexes;
 std::unique_ptr<Cpu::SceModules> graph;
 std::uint64_t address=0;
 unsigned boundaries=0;
 std::array<std::int32_t,3> observedBasePriorities{-1,-1,-1};
 const std::thread::id owner=std::this_thread::get_id();
 Session(const char* path,unsigned mode) {
  require(std::string(Cpu::Machine::Backend()).find("Modern QEMU TCG")!=std::string::npos,
          "RR fixture requires native Modern QEMU TCG");
  mutexes=std::make_unique<Cpu::Platform::KernelPrimitives>(machine,threads);
  lifecycle.SetProcessExitHandler([this](int code){threads->ProcessExitFromHostCall(code);});
  const std::array hosts{Cpu::SceHostModule{"libkernel.prx",{"libkernel",0,1,1},{{"libkernel",0,1}}}};
  graph=std::make_unique<Cpu::SceModules>(machine,Cpu::SceModuleFile{path,Bias},
   std::span<const Cpu::SceModuleFile>{},hosts,
   [&](const Cpu::SceImport& row,std::uint8_t type)->std::optional<Cpu::SceResolvedImport> {
    require(type==2,"RR fixture lost genuine linked function import type");
    if(const auto gate=mutexes->Resolve(row,type))return Cpu::SceResolvedImport{*gate,type};
    if(const auto gate=imports.ResolvePriority(row,type))return Cpu::SceResolvedImport{*gate,type};
    if(const auto gate=imports.Resolve(row,type))return Cpu::SceResolvedImport{*gate,type};
    if(const auto gate=lifecycle.Resolve(row))return Cpu::SceResolvedImport{*gate,type};
    return std::nullopt;
   });
  const auto& image=graph->Modules()[0].Image;
  require(image.Imports.size()==18 && image.NeededFiles.empty() && image.NeededModules.size()==1 && image.Tls,
          "RR fixture lost real linked imports, owned TLS or module scope");
  require(std::set<std::uint32_t>(image.RelocationTypes.begin(),image.RelocationTypes.end())==
          std::set<std::uint32_t>{7,8},"RR fixture lacks genuine PLT/RELATIVE relocations");
  for(const auto& row:image.Exports)if(row.Type==1 && row.Size==sizeof(State)) {
   require(!address,"Ambiguous RR receipt");address=Bias+row.Value;
  }
  require(address && state()==State{},"RR loading executed guest or lost receipt");
  put(address+8,mode);put(address+126*8,Canary);put(address+127*8,~Canary);
  Cpu::SetupSceEntry(machine,graph->Main(),{"public-rr-fixture"},graph->EntryTerminationGate());
  threads->AdoptInitial({graph->Main().Entry,graph->InitialStack(),graph->Tls(),graph->ThreadTlsFactory(),
                       mode==4 || mode==8 || mode==10?700:256});
  threads->SetOwnerBoundary([this,mode](bool){
   threads->CheckIdleOwner();require(std::this_thread::get_id()==owner,"RR execution migrated owner");++boundaries;
   if(mode>=5 && mode<=7) {
    const auto r=state();
    // Cache once while each long-running guest is live; joins later reap it.
    for(unsigned i=0;i!=3;++i)if(r[8+i] && observedBasePriorities[i]<0)
     observedBasePriorities[i]=threads->BasePriority(r[8+i]);
   }
  },std::chrono::milliseconds(500));
  graph->SetExecutor(threads->ModuleExecutor());graph->InitializeDependencies();
 }
 ~Session(){threads->Withdraw();}
 State state()const{State r{};machine.Read(address,std::as_writable_bytes(std::span(r)));return r;}
 void put(std::uint64_t p,std::uint64_t value){machine.Write(p,std::as_bytes(std::span(&value,1)));}
 std::uint64_t get(std::uint64_t p)const{
  std::uint64_t value{};machine.Read(p,std::as_writable_bytes(std::span(&value,1)));return value;
 }
 void finish(){
  require(graph->RunMain(10000000,10000)==Cpu::StopReason::Exit && machine.ExitCode()==0,
          "RR guest did not finish its fixed instruction ceiling");
  const auto r=state();
  if(r[0]!=0x524f554e44524f42ULL || r[4] || r[6] || r[126]!=Canary || r[127]!=~Canary)
   std::cerr<<"RR receipt mode="<<r[1]<<" completion="<<std::hex<<r[0]
            <<" statuses="<<r[4]<<" slotGuards="<<r[6]<<" canaries="<<r[126]<<'/'<<r[127]<<std::dec<<'\n';
  require(r[0]==0x524f554e44524f42ULL && r[4]==0 && r[6]==0 && r[126]==Canary && r[127]==~Canary,
          "RR guest completion/status/opaque slot or receipt guards differ");
  require(r[52]==0xfeedbeef000002bcULL && r[53]==0xfeedbeef000002bcULL &&
          r[54]==0xfeedbeef0000012cULL,"RR/FIFO policy reset or four-byte sched_param output differs");
 }
};
void pair(const char* path,unsigned mode,bool overlap) {
 Session s(path,mode);s.finish();const auto r=s.state();
 require(r[16]==Iterations && r[17]==Iterations && r[48]==0x2000 && r[49]==0x2001,
         "RR pair did not execute both actual guest workers or return independent payloads");
 const std::set<std::uint64_t> ids{r[8],r[9],r[12]};
 require(ids.size()==3 && !ids.contains(0),"RR pair reused thread identity");
 if(overlap) require(r[20]>0 && r[20]<Iterations && r[21]>0 && r[21]<Iterations,
                     "Non-yielding equal-priority RR guest failed independent overlapping-progress oracle");
 else require(r[20]==0 && r[21]==Iterations && r[22]==0 && r[23]==Iterations,
              "FIFO or higher-priority guest was rotated behind an equal/lower-priority peer");
 if(mode>=5 && mode<=7)require(r[10] && !ids.contains(r[10]) && r[50]==0x4000 &&
                              s.observedBasePriorities==std::array<std::int32_t,3>{300,300,300},
                              "RR inheritance control lost distinct parent or copied base priority");
 std::cout<<"PASS native translated non-yielding RR/FIFO/priority/inheritance mode "<<mode<<'\n';
}
void donation(const char* path) {
 Session s(path,4);s.finish();const auto r=s.state();
 constexpr std::array<std::uint64_t,4> order{10,20,40,11};
 require(r[5]==order.size() && std::equal(order.begin(),order.end(),r.begin()+64),
         "RR inherited owner/high/medium/restoration order differs");
 require(r[30]==1 && r[31]==1 && r[32]==1 && r[35]==1 && r[40] && r[41] &&
         r[42]==0 && r[43]==0 && r[44]==0 && r[48]==0x3000 && r[49]==0x3001 && r[50]==0x3002,
         "RR blocked wake lost actual PLT ABI, protected payload or join return");
 std::cout<<"PASS actual RR mutex blocking, donated owner priority and withdrawal with saved guest ABI\n";
}
void parked(Session& s) {
 Cpu::GuestPhaseBudget phase(100000);
 require(s.threads->RunEntry(phase)==Cpu::StopReason::InstructionLimit && phase.Consumed()==100000,
         "RR cancellation did not retain bounded parked guest");
 const auto r=s.state();
 require(r[2]==1 && r[30]==1 && r[31]==1 && r[32]==1 && r[35]==0 && r[5]==0 &&
         s.threads->BasePriority(r[8])==700 && s.threads->EffectivePriority(r[8])==300,
         "RR cancellation lacked actual blocked donated low owner and high waiter");
}
void blockedCancellation(const char* path,bool corrupt) {
 Session s(path,8);parked(s);const auto before=s.state();
 require(before[40] && before[41] && s.get(before[40])==before[41],
         "RR blocked cancellation lacks real executable return word");
 if(corrupt) {
  s.put(before[40],0x1122334455667788ULL);s.put(s.address+3*8,1);
  rejects([&]{s.graph->RunMain(1000000,10000);},"return word changed");
 } else s.mutexes.reset();
 const auto settled=s.state();Cpu::GuestPhaseBudget residual(37);
 require(s.threads->RunEntry(residual)==Cpu::StopReason::Requested && residual.Consumed()==0 &&
         s.state()==settled && settled[35]==0 && settled[0]==0 &&
         s.threads->EffectivePriority(before[8])==700,
         "RR blocked cancellation retained donation or manufactured guest continuation success");
 std::cout<<"PASS RR blocked "<<(corrupt?"corrupt-return":"provider-withdrawal")<<" cancellation\n";
}
void wakeQuantum(const char* path) {
 Session s(path,10);Cpu::GuestPhaseBudget parked(100000);
 require(s.threads->RunEntry(parked)==Cpu::StopReason::InstructionLimit && parked.Consumed()==100000,
         "RR wake quantum control did not retain bounded parked guest");
 const auto before=s.state();
 require(before[2]==1 && before[30]==1 && before[31]==1 && before[34]==0 && before[35]==0 &&
         before[8] && before[9] && before[8]!=before[9],
         "RR wake quantum oracle did not park two actual blocked workers");
 s.put(s.address+3*8,1);s.finish();const auto r=s.state();
 require(r[34]==1 && r[35]==1 && r[48]==0x5000 && r[49]==0x5001,
         "Actual blocked RR wake retained near-expired residual instead of a full logical turn");
 std::cout<<"PASS actual guest3900-nop pre-sleep/full-turn wake before equal peer\n";
}
void budgetAndCancellation(const char* path) {
 constexpr std::uint64_t Total=200003;
 State a{};
 // The native bridge owns one CPU context. Settle and destroy the first
 // actual session before constructing the independent split-budget session.
 {
  Session whole(path,9);Cpu::GuestPhaseBudget uninterrupted(Total);
  require(whole.threads->RunEntry(uninterrupted)==Cpu::StopReason::InstructionLimit &&
          uninterrupted.Consumed()==Total && uninterrupted.Remaining()==0,
          "RR owner budget was refreshed at a quantum boundary");
  a=whole.state();
  require(a[0]==0 && a[16]>0 && a[17]>0 && whole.boundaries>3,
          "RR whole-budget control lacked actual peer progress");
  whole.machine.RequestStop();Cpu::GuestPhaseBudget settle(1);
  require(whole.threads->RunEntry(settle)==Cpu::StopReason::Requested && settle.Consumed()==0 && whole.state()==a,
          "RR whole-budget control failed settlement");
 }
 Session split(path,9);
 for(const auto part:{8193ULL,37ULL,Total-8193-37}) {
  Cpu::GuestPhaseBudget piece(part);
  require(split.threads->RunEntry(piece)==Cpu::StopReason::InstructionLimit &&
          piece.Consumed()==part && piece.Remaining()==0,
          "RR short residual owner budget overran or refreshed");
 }
 const auto b=split.state();
 require(a==b,
         "Split owner budgets changed RR residual quantum or actual guest progress");
 split.machine.RequestStop();Cpu::GuestPhaseBudget stopped(10000);
 require(split.threads->RunEntry(stopped)==Cpu::StopReason::Requested && stopped.Consumed()==0 && split.state()==b,
         "RR RequestStop executed a canceled runnable guest");
 Cpu::GuestPhaseBudget again(1);
 require(split.threads->RunEntry(again)==Cpu::StopReason::Requested && again.Consumed()==0 && split.state()==b,
         "RR canceled guest continuation survived terminal settlement");
 std::cout<<"PASS RR cumulative200003 split8193/37 residual quantum and runnable cancellation\n";
}
}
int main(int argc,char** argv) {
 try {
  require(argc==2,"Usage: KernelRoundRobinTest genuine-packaged.elf");
  pair(argv[1],0,true);pair(argv[1],1,false);pair(argv[1],2,false);
  pair(argv[1],5,true);pair(argv[1],6,true);pair(argv[1],7,false);
  donation(argv[1]);blockedCancellation(argv[1],false);blockedCancellation(argv[1],true);
  wakeQuantum(argv[1]);budgetAndCancellation(argv[1]);
  std::cout<<"PASS actual linked x86 SCE guest deterministic RR slicing; game_runtime_ready=false\n";
 }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
