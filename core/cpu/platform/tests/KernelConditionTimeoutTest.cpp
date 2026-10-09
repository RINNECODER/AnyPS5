#include "KernelPrimitives.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceThreadImports.hpp>
#include <array>
#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

// Authoring gate: evidencecpu13/fixture/test-audit-authoring.md. Actual timed
// PLTs own release/deadline/reacquire, ABI, exact errno and cancellation contracts
// absent from older untimed controls. No test-only production export or seam.
namespace {
using namespace std::chrono;
constexpr std::uint64_t Bias=0x1000000, Busy=0x80020010, Perm=0x80020001, Invalid=0x80020016;
using State=std::array<std::uint64_t,128>;
void require(bool ok,const char* message) {if(!ok) throw std::runtime_error(message);}
template<class F> void rejects(F action,const char* diagnostic) {
 try {action();} catch(const std::exception& e) {
  require(std::string(e.what()).find(diagnostic)!=std::string::npos,e.what());return;
 } throw std::runtime_error("Expected timed-condition rejection");
}
Cpu::SceImport scoped(std::string_view nid,bool relative) {
 Cpu::SceImport i;i.Nid=nid;i.ModuleName="libkernel";i.LibraryName=relative ? "libkernel" : "libScePosix";
 i.LibraryVersion=i.ModuleMajor=i.ModuleMinor=1;i.LibraryId=relative ? 0 : 43;i.ModuleId=relative ? 1 : 24;return i;
}
struct Session {
 Cpu::Machine machine;
 std::shared_ptr<Cpu::GuestThreads> threads=std::make_shared<Cpu::GuestThreads>(machine);
 Cpu::SceThreadImports threadImports{machine,threads};Cpu::SceLifecycleImports lifecycle{machine};
 std::unique_ptr<Cpu::Platform::TargetKernelMutexes> mutexes;
 std::unique_ptr<Cpu::SceModules> graph;
 std::uint64_t address=0;std::set<std::uint64_t> gates;bool withdrawn=false,relative;
 Session(const char* path,bool rel,unsigned mode):relative(rel) {
  require(std::string(Cpu::Machine::Backend()).find("Modern QEMU TCG")!=std::string::npos,"Timed fixture requires native Modern QEMU TCG");
  mutexes=std::make_unique<Cpu::Platform::TargetKernelMutexes>(machine,threads);
  lifecycle.SetProcessExitHandler([runtime=std::weak_ptr<Cpu::GuestThreads>(threads)](int code) {
   const auto owner=runtime.lock();require(bool(owner),"Timed process-exit scheduler expired");owner->ProcessExitFromHostCall(code);
  });
  const std::array hosts{Cpu::SceHostModule{"libkernel.prx",{"libkernel",std::uint16_t(rel ? 1 : 24),1,1},
    rel ? std::vector<Cpu::SceLibraryIdentity>{{"libkernel",0,1}} : std::vector<Cpu::SceLibraryIdentity>{{"libkernel",44,1},{"libScePosix",43,1}}}};
  graph=std::make_unique<Cpu::SceModules>(machine,Cpu::SceModuleFile{path,Bias},std::span<const Cpu::SceModuleFile>{},hosts,
   [&](const Cpu::SceImport& import,std::uint8_t type)->std::optional<Cpu::SceResolvedImport> {
    require(type==2,"Timed fixture lost linked mandatory function type");
    if(const auto gate=mutexes->Resolve(import,type)) {gates.insert(*gate);return Cpu::SceResolvedImport{*gate,type};}
    if(const auto gate=mutexes->ResolveCondition(import,type,0)) {gates.insert(*gate);return Cpu::SceResolvedImport{*gate,type};}
    if(const auto gate=threadImports.Resolve(import,type)) return Cpu::SceResolvedImport{*gate,type};
    if(const auto gate=lifecycle.Resolve(import)) return Cpu::SceResolvedImport{*gate,type};
    return std::nullopt;
   });
  const auto& parsed=graph->Modules()[0].Image;
  require(parsed.Imports.size()==17 && parsed.NeededFiles.empty() && parsed.NeededModules.size()==1 && parsed.Tls && gates.size()==12,
          "Timed fixture lost genuine linked imports or TLS graph");
  require(std::set<std::uint32_t>(parsed.RelocationTypes.begin(),parsed.RelocationTypes.end())==std::set<std::uint32_t>{7,8},
          "Timed fixture lost genuine PLT/RELATIVE relocations");
  for(const auto& item:parsed.Exports) if(item.Type==1 && item.Size==sizeof(State)) {require(!address,"Ambiguous timed receipt");address=Bias+item.Value;}
  require(address && receipt()==State{},"Loading executed timed guest or lost receipt");put(1,mode);put(90,1);put(91,1);put(92,1);
  deadline(30ms);
  threads->SetOwnerBoundary([](bool){},1000ms);
  Cpu::SetupSceEntry(machine,graph->Main(),{"public-condition-timeout"},graph->EntryTerminationGate());
  threads->AdoptInitial({graph->Main().Entry,graph->InitialStack(),graph->Tls(),graph->ThreadTlsFactory()});
  graph->SetExecutor(threads->ModuleExecutor());graph->InitializeDependencies();
 }
 ~Session() {if(!withdrawn) threads->Withdraw();}
 State receipt() const {State s{};machine.Read(address,std::as_writable_bytes(std::span(s)));return s;}
 void write(std::uint64_t p,std::uint64_t v) {machine.Write(p,std::as_bytes(std::span(&v,1)));}
 void put(unsigned index,std::uint64_t v) {write(address+8*index,v);}
 std::uint64_t get(std::uint64_t p) const {std::uint64_t v;machine.Read(p,std::as_writable_bytes(std::span(&v,1)));return v;}
 void deadline(milliseconds delay) {
  const auto epoch=duration_cast<nanoseconds>((system_clock::now()+delay).time_since_epoch()).count();
  for(unsigned role=0;role<2;++role) {
   put(80+role*2,relative ? std::uint64_t(duration_cast<microseconds>(delay).count()) : std::uint64_t(epoch/1000000000));
   put(81+role*2,relative ? 0 : std::uint64_t(epoch%1000000000));
  }
 }
 std::uint64_t timeout() const {return relative ? 0x8002003cULL : 60ULL;}
 void withdraw() {if(!withdrawn) {threads->Withdraw();withdrawn=true;}}
};
void slotGuards(const Session& s,const State& r) {
 constexpr std::uint64_t canary=0x1badc0ffeef00d55ULL;
 require(s.get(r[30]-8)==canary && s.get(r[30]+8)==canary &&
         s.get(r[32]-8)==canary && s.get(r[32]+8)==canary,
         "Timed input rejection or cancellation damaged adjacent original slot guards");
}
void finished(Session& s) {
 require(s.graph->RunMain(3000000,30000)==Cpu::StopReason::Exit && s.machine.ExitCode()==0,"Timed guest failed to finish");
 const auto r=s.receipt();
 require(r[0]==0x54494d4544434f4eULL && !r[4] && !r[5] && !r[6],"Timed guest status/guards/protected payload differs");
 require(r[35]==2,"Timed mutex destroy sentinel differs");
}
void completed(const char* path,bool relative,unsigned mode,bool upperBits=false) {
 Session s(path,relative,mode);s.deadline(mode==1 ? 300ms : (mode==0 || mode==3 ? 100ms : 30ms));
 if(upperBits) {require(relative && mode==0,"Upper32 ABI case requires relative idle mode");s.put(80,0xaabbccdd00000000ULL|100000ULL);}
 unsigned idle=0;const auto start=steady_clock::now();
 s.threads->SetOwnerBoundary([&](bool waiting) {
  if(waiting) ++idle;
  if(mode==4 && steady_clock::now()-start>=70ms) s.put(100,1);
 },1000ms);
 const auto input=s.receipt();finished(s);const auto r=s.receipt();
 const unsigned count=mode==3 ? 2 : 1;
 require(r[14]==1 && r[15]==1 && r[12]==((1ULL<<count)-1) && r[13]==r[12],
         "Timed wait returned early or failed to release same owned mutex");
 require(r[20]==Busy && r[21]==Busy && r[34]==1 && r[23]==r[31] && r[24]==r[33],
         "Timed binding lost live destroy busy, original slot identity or sentinel");
 require(r[40] && r[41] && !r[43] && !r[44],"Timed suspended return stack/callee-saved ABI differs");
 require(r[42]==(mode==1 || mode==3 ? 0 : s.timeout()),"POSIX positive errno or relative SCE timeout differs");
 for(unsigned role=0;role<count;++role) {
  require(r[16+role]==1 && r[56+role]==0x1300+role,"Timed continuation completed twice or joined wrong payload");
  require(r[48+role]==(mode==2 ? 0 : Busy),"Timed wait returned before original owned mutex reacquisition");
 }
 if(mode==3) require(r[47]==s.timeout(),"Second competing waiter lost deadline after first signal");
 if(mode==1 || mode==3 || mode==4) require(!r[18],"Signal or deadline returned while original mutex still owned by parent");
 if(mode==4) require(r[22]==Busy,"Expired queued reacquisition dropped live condition binding");
 if(mode==2) require(r[52]==0 && r[53]==Busy && r[54]==Perm,"Timed recursive depth was not restored through full release/reacquire");
 for(unsigned i=80;i<84;++i) require(r[i]==input[i],"Timed input-only argument was overwritten");
 if(mode==0 || mode==3) require(idle>0 && steady_clock::now()-start>=15ms && steady_clock::now()-start<1000ms,
          "Actual owner idle did not drive a bounded real deadline");
}
void directError(const char* path,bool relative,unsigned cp,unsigned mp,std::uint64_t sec,std::uint64_t nano) {
 Session s(path,relative,6);s.put(90,cp);s.put(91,mp);s.put(80,sec);s.put(81,nano);finished(s);const auto r=s.receipt();
 require(r[60]==(relative ? Invalid : 22) && r[61]==r[31] && r[62]==r[33] && r[63]==Busy && !r[13],
         "Timed invalid input changed slots, unlocked owned mutex or fabricated a wait");
 if(cp==1) require(!r[61] && !r[34],"Invalid timed call published a static zero condition");
 require(r[80]==sec && r[81]==nano,"Invalid timed call overwrote input words");
}
void pointerError(const char* path,std::uint64_t pointer,bool unreadable=false,const char* diagnostic="Guest access denied") {
 Session s(path,false,6);s.put(92,pointer);
 if(pointer==0x6ff8) {s.machine.Map(0x6000,4096,Cpu::Permission::Read|Cpu::Permission::Write);}
 if(unreadable) {s.machine.Map(0x6000,4096,Cpu::Permission::Write);}
 rejects([&]{s.graph->RunMain(500000,10000);},diagnostic);const auto r=s.receipt();
 slotGuards(s,r);
 require(!r[0] && !r[13] && !s.get(r[30]) && s.get(r[32])==r[33] && !r[4] && !r[5],
         "Malformed timed pointer published condition or changed original mutex/guards");
}
void nullTime(const char* path) {
 Session s(path,false,6);s.put(92,0);finished(s);const auto r=s.receipt();
 require(r[60]==22 && !r[61] && r[62]==r[33] && r[63]==Busy && !r[34],
         "Null timespec changed static condition or owned mutex instead of positive EINVAL");
}
void slotPointer(const char* path,bool relative,unsigned index,std::uint64_t pointer,const char* diagnostic) {
 Session s(path,relative,6);s.put(index,pointer);
 rejects([&]{s.graph->RunMain(500000,10000);},diagnostic);const auto r=s.receipt();
 slotGuards(s,r);
 require(!r[0] && !r[13] && !s.get(r[30]) && s.get(r[32])==r[33],
         "Malformed timed slot pointer published static condition or changed original mutex");
}
void readonlyExpired(const char* path) {
 Session s(path,false,6);s.machine.Map(0x6000,4096,Cpu::Permission::Read|Cpu::Permission::Write);
 s.write(0x6000,0);s.write(0x6008,0);s.machine.Protect(0x6000,4096,Cpu::Permission::Read);s.put(92,0x6000);
 finished(s);const auto r=s.receipt();
 require(r[60]==60 && r[63]==Busy && r[34]==1 && s.get(0x6000)==0 && s.get(0x6008)==0,
         "Expired absolute read-only timespec required output permission or failed actual mutex reacquire");
}
void expired(const char* path,bool relative) {
 Session s(path,relative,6);s.put(80,0);s.put(81,0);finished(s);const auto r=s.receipt();
 slotGuards(s,r);
 require(r[60]==s.timeout() && r[63]==Busy && r[34]==1 && r[35]==2 &&
         r[61]>2 && r[62]==r[33] && !r[80] && !r[81],
         "Zero/expired time failed genuine timed suspension/reacquisition or changed input words");
}
void preEpochExpired(const char* path,std::uint64_t seconds) {
 Session s(path,false,7);s.put(80,seconds);s.put(81,0);finished(s);const auto r=s.receipt();
 slotGuards(s,r);
 require(r[60]==60 && r[42]==60 && r[63]==Busy && r[34]==1 && r[35]==2 &&
         r[61]>2 && r[62]==r[33] && r[80]==seconds && !r[81],
         "Normalized preepoch time rejected instead of genuine expiration/reacquisition or changed input words");
 require(r[40] && r[41] && !r[43] && !r[44] && r[106]==1 && r[107]==Busy && r[108]==1 && r[109]==0x1370,
         "Preepoch timed wait lost suspended ABI, real mutex release or contender ownership");
}
void saturated(const char* path,bool relative) {
 Session s(path,relative,0);s.put(80,relative ? 0xffffffffULL : 0x7fffffffffffffffULL);s.put(81,relative ? 0 : 999999999);
 unsigned idle=0;s.threads->SetOwnerBoundary([&](bool waiting){if(waiting) ++idle;},25ms);
 Cpu::GuestPhaseBudget budget(1000000);
 require(s.threads->RunEntry(budget)==Cpu::StopReason::Requested,"Oversized deadline wrapped into guest timeout or bypassed bounded owner cap");
 const auto r=s.receipt();slotGuards(s,r);require(idle>0 && r[14]==1 && r[15]==1 && !r[13] && !r[0] && r[16]==0 && r[40],
         "Oversized deadline control failed to park real waiter or fabricated completion");
}
void cancellation(const char* path,bool relative,unsigned kind) {
 Session s(path,relative,kind==0 ? 0 : 5);s.deadline(1000ms);State before{};
 if(kind==0) {
  s.threads->SetOwnerBoundary([&](bool waiting){if(waiting) {before=s.receipt();s.machine.RequestStop();}},100ms);
  Cpu::GuestPhaseBudget budget(1000000);require(s.threads->RunEntry(budget)==Cpu::StopReason::Requested,"Idle requeststop lost timed cancellation");
 } else {
  Cpu::GuestPhaseBudget budget(30000);require(s.threads->RunEntry(budget)==Cpu::StopReason::InstructionLimit,"Timed cancellation failed bounded owner spin");
  before=s.receipt();require(before[101]>0,"Timed cancellation did not reach actual held owner");
  if(kind==1) s.withdraw();else s.mutexes.reset();
 }
 slotGuards(s,before);
 require(before[14]==1 && before[15]==1 && !before[13] && before[40] && !before[0],"Timed cancellation did not retain real continuation");
 Cpu::GuestPhaseBudget retry(1000);
 if(kind==1) rejects([&]{s.threads->RunEntry(retry);},"runtime was withdrawn");
 else require(s.threads->RunEntry(retry)==Cpu::StopReason::Requested,"Canceled timed waiter remained runnable");
 require(!retry.Consumed(),"Canceled timed waiter consumed translated instructions");
 require(s.receipt()==before && s.get(before[30])==before[31] && s.get(before[32])==before[33],
         "Timed cancellation fabricated return/slot write");
 if(kind==1) {
  auto row=scoped("g+PZd2hiacg",relative);row.LibraryName="libkernel";if(!relative) row.LibraryId=44;
  const auto gate=s.mutexes->ResolveCondition(row,2,0);
  require(gate.has_value(),"Live timed provider destroy gate missing after scheduler withdrawal");
  s.machine.Map(0x71000000,4096,Cpu::Permission::Read|Cpu::Permission::Write);
  s.machine.Map(0x71001000,4096,Cpu::Permission::Read|Cpu::Permission::Execute);
  s.write(0x71000ff8,0x71001000);s.machine.Set(Cpu::Register::Rsp,0x71000ff8);s.machine.Set(Cpu::Register::Rdi,before[30]);
  require(s.machine.Run(*gate,0x71001000,1000)==Cpu::StopReason::Address && s.machine.Get(Cpu::Register::Rax)==0 &&
          s.get(before[30])==1 && s.receipt()==before,"Canceled timed waiter leaked live provider condition binding");
 }
 s.withdraw();s.mutexes.reset();require(s.receipt()==before,"Timed teardown fabricated completion");
}
void queuedCancellation(const char* path,bool relative,unsigned kind) {
 Session s(path,relative,4);s.deadline(100ms);const auto start=steady_clock::now();unsigned turns=0;
 s.threads->SetOwnerBoundary([&](bool){++turns;},500ms);
 do {
  Cpu::GuestPhaseBudget slice(30000);
  require(s.threads->RunEntry(slice)==Cpu::StopReason::InstructionLimit,
          "Queued timed cancellation failed to retain held original mutex");
 } while(steady_clock::now()-start<150ms);
 const auto before=s.receipt();slotGuards(s,before);
 require(turns>1 && before[14]==1 && before[15]==1 && !before[13] && before[40] && !before[0] && !before[100],
         "Expired timed cancellation lacked real suspended waiter and held owner");
 if(kind==0) s.machine.RequestStop();else if(kind==1) s.withdraw();else s.mutexes.reset();
 Cpu::GuestPhaseBudget retry(1000);
 if(kind==1) rejects([&]{s.threads->RunEntry(retry);},"runtime was withdrawn");
 else require(s.threads->RunEntry(retry)==Cpu::StopReason::Requested,"Expired reacquire cancellation remained runnable");
 require(!retry.Consumed() && s.receipt()==before &&
         s.get(before[30])==before[31] && s.get(before[32])==before[33],
         "Expired reacquire cancellation fabricated a return or mutated original slots");
 s.withdraw();s.mutexes.reset();require(s.receipt()==before,"Queued timed teardown fabricated completion");
}
void admission() {
 Cpu::Machine machine;auto threads=std::make_shared<Cpu::GuestThreads>(machine);Cpu::Platform::TargetKernelMutexes target(machine,threads);
 for(bool relative:{false,true}) {
  const auto row=scoped(relative ? "BmMjYxmew1w" : "27bAgiJmOh0",relative);
  require(target.ResolveCondition(row,2,0).has_value(),"Timed row rejected");
  // Import-table ids are per-image; another title's ids are admitted.
  auto other=row;other.LibraryId=7;other.ModuleId=9;
  require(target.ResolveCondition(other,2,0).has_value(),"Another image's timed import-table ids were refused");
  rejects([&]{target.ResolveCondition(row,2,1);},"type/size");
  for(auto type:{0,1,6}) rejects([&]{target.ResolveCondition(row,type,0);},"type/size");
  for(unsigned field=0;field<6;++field) {
   auto wrong=row;
   if(field==0) wrong.LibraryName="foreign";
   if(field==1) wrong.ModuleName="foreign";
   if(field==2) wrong.LibraryVersion=2;
   if(field==3) wrong.ModuleMajor=2;
   if(field==4) wrong.ModuleMinor=2;
   // SCE timed wait is a libkernel export only; POSIX may also come from libScePosix.
   if(field==5) wrong.LibraryName=relative ? "libScePosix" : "foreign";
   rejects([&]{target.ResolveCondition(wrong,2,0);},"scope/version");
  }
 }
}
}
int main(int argc,char** argv) {
 std::string context="arguments";
 try {
  require(argc==3,"Usage: KernelConditionTimeoutTest posix.elf relative.elf");
  for(bool relative:{false,true}) {
   const char* path=argv[relative ? 2 : 1];
   for(unsigned mode:{0,1,2,3,4}) {
    context=std::string(relative ? "relative" : "posix")+" completed mode="+std::to_string(mode);
    completed(path,relative,mode);
   }
   context=std::string(relative ? "relative" : "posix")+" input rollback";
   for(unsigned cp:{2,3,4}) directError(path,relative,cp,1,0,0);
   for(unsigned mp:{2,3}) directError(path,relative,1,mp,0,0);
   if(!relative) {
    directError(path,false,1,1,0,1000000000);
    directError(path,false,1,1,0,~0ULL);
   }
   slotPointer(path,relative,90,0,"Invalid kernel primitive guest slot");
   slotPointer(path,relative,91,0,"Invalid kernel primitive guest slot");
   slotPointer(path,relative,90,0x12345000,"Guest access denied");
   slotPointer(path,relative,91,0x12345000,"Guest access denied");
   context=std::string(relative ? "relative" : "posix")+" expired";expired(path,relative);
   context=std::string(relative ? "relative" : "posix")+" saturation";saturated(path,relative);
   for(unsigned kind:{0,1,2}) {
    context=std::string(relative ? "relative" : "posix")+" pending cancellation kind="+std::to_string(kind);cancellation(path,relative,kind);
    context=std::string(relative ? "relative" : "posix")+" expired queued cancellation kind="+std::to_string(kind);queuedCancellation(path,relative,kind);
   }
  }
  for(const auto seconds:{~0ULL,0x8000000000000000ULL}) {
   context=std::string("posix normalized preepoch expired ABI sec=")+(seconds==~0ULL ? "-1" : "INT64_MIN");
   preEpochExpired(argv[1],seconds);
  }
  context="relative canonical u32 ignores nonzero upper32 with genuine100ms idle/ABI/reacquire";
  completed(argv[2],true,0,true);
  context="posix input pointer extent/permission";
  pointerError(argv[1],0x12345000);pointerError(argv[1],0x6ff8);pointerError(argv[1],0x6000,true);
  pointerError(argv[1],~0ULL,false,"Invalid kernel condition timespec span");
  context="posix null/readonly expired";nullTime(argv[1]);readonlyExpired(argv[1]);
  context="exact timed admission";admission();
  std::cout<<"PASS linked exact2 timed conditions, genuine release/signal or bounded owner deadline/reacquire, recursive ownership, competing exactly-once wake, ABI, positive errno60/SCE timeout, input extent/permissions/rollback/saturation and stop/withdraw/provider teardown\n";
 } catch(const std::exception& e) {std::cerr<<"FAIL "<<context<<": "<<e.what()<<'\n';return 1;}
}
