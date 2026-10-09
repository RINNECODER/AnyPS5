#include "KernelSemaphores.hpp"
#include "KernelPrimitives.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceThreadImports.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

// Test-audit primary boundary: actual linked SCE semaphore PLT calls executing
// in GuestThreads. Opaque64/output width, parked continuation, priority/stable
// ties, exact consumption, delete-vs-selected wake, memory prevalidation and
// provider/source lifetime each change independent guest receipts below.
// Credible regressions are out4/truncation/FIFO/fake-success/missing reservation,
// overflow/recycled token/stale completion and selector bypass. Existing kernel
// controls import no semaphore NIDs. No test-only production entry is added;
// synthetic execution uses the explicit engineering provider only.
namespace {
constexpr std::uint64_t Bias=0x1000000, Canary=0x1badc0ffeef00d55ULL;
constexpr std::uint64_t Invalid=0x80020016, Missing=0x80020003, Deleted=0x8002000d;
constexpr std::uint64_t Page=0x71000000;
using State=std::array<std::uint64_t,128>;
void require(bool ok,const char* message) {if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F action) {
 bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}
 require(rejected,"Expected fail-closed semaphore rejection");
}
Cpu::SceImport scoped(std::string_view nid) {
 Cpu::SceImport row;row.Nid=nid;row.LibraryName=row.ModuleName="libkernel";
 row.LibraryVersion=row.ModuleMajor=row.ModuleMinor=1;row.LibraryId=44;row.ModuleId=24;return row;
}
struct Session {
 Cpu::Machine machine;
 std::shared_ptr<Cpu::GuestThreads> threads=std::make_shared<Cpu::GuestThreads>(machine);
 Cpu::SceThreadImports threadImports{machine,threads};
 Cpu::SceLifecycleImports lifecycle{machine};
 std::unique_ptr<Cpu::Platform::KernelSemaphores> semaphores;
 std::unique_ptr<Cpu::Platform::KernelPrimitives> mutexes;
 std::unique_ptr<Cpu::SceModules> graph;
 std::uint64_t address=0;
 std::set<std::uint64_t> gates;
 bool schedulerWithdrawn=false;
 Session(const char* path,unsigned mode) {
  require(std::string(Cpu::Machine::Backend()).find("Modern QEMU TCG")!=std::string::npos,
          "Semaphore fixture requires native Modern QEMU TCG");
  semaphores=std::make_unique<Cpu::Platform::KernelSemaphores>(machine,threads);
  mutexes=std::make_unique<Cpu::Platform::KernelPrimitives>(machine,threads);
  lifecycle.SetProcessExitHandler([runtime=std::weak_ptr<Cpu::GuestThreads>(threads)](int code){
   const auto owner=runtime.lock();require(bool(owner),"Semaphore scheduler expired");owner->ProcessExitFromHostCall(code);
  });
  const std::array hosts{Cpu::SceHostModule{"libkernel.prx",{"libkernel",1,1,1},{{"libkernel",1,1}}}};
  graph=std::make_unique<Cpu::SceModules>(machine,Cpu::SceModuleFile{path,Bias},
   std::span<const Cpu::SceModuleFile>{},hosts,
   [&](const Cpu::SceImport& row,std::uint8_t type)->std::optional<Cpu::SceResolvedImport>{
    require(type==2,"Actual semaphore fixture import lost ELF FUNC type");
    if(const auto gate=semaphores->Resolve(row,type,0)){gates.insert(*gate);return Cpu::SceResolvedImport{*gate,type};}
    if(const auto gate=mutexes->Resolve(row,type))return Cpu::SceResolvedImport{*gate,type};
    if(const auto gate=threadImports.ResolvePriority(row,type))return Cpu::SceResolvedImport{*gate,type};
    if(const auto gate=threadImports.Resolve(row,type))return Cpu::SceResolvedImport{*gate,type};
    if(const auto gate=lifecycle.Resolve(row))return Cpu::SceResolvedImport{*gate,type};
    return std::nullopt;
   });
  const auto& parsed=graph->Modules()[0].Image;
  require(parsed.Imports.size()==20 && parsed.NeededFiles.empty() && parsed.NeededModules.size()==1 &&
          parsed.Tls && gates.size()==4,"Semaphore fixture lost genuine four-import/thread/TLS graph");
  require(std::set<std::uint32_t>(parsed.RelocationTypes.begin(),parsed.RelocationTypes.end())==std::set<std::uint32_t>{7,8},
          "Semaphore fixture lost genuine linked PLT/RELATIVE relocations");
  for(const auto& item:parsed.Exports)if(item.Type==1 && item.Size==sizeof(State)){
   require(!address,"Ambiguous compiled semaphore receipt");address=Bias+item.Value;
  }
  require(address && receipt()==State{},"Loading ran semaphore guest code or lost receipt");
  put(address+8,mode);put(address+126*8,Canary);put(address+127*8,~Canary);
  Cpu::SetupSceEntry(machine,graph->Main(),{"public-semaphore-fixture"},graph->EntryTerminationGate());
  threads->AdoptInitial({graph->Main().Entry,graph->InitialStack(),graph->Tls(),graph->ThreadTlsFactory()});
  threads->SetOwnerBoundary([](bool){},std::chrono::milliseconds(50));
  graph->SetExecutor(threads->ModuleExecutor());graph->InitializeDependencies();
 }
 ~Session(){if(!schedulerWithdrawn)threads->Withdraw();}
 void withdraw(){threads->Withdraw();schedulerWithdrawn=true;}
 State receipt()const {State value{};machine.Read(address,std::as_writable_bytes(std::span(value)));return value;}
 void put(std::uint64_t where,std::uint64_t value){machine.Write(where,std::as_bytes(std::span(&value,1)));}
 std::uint64_t get(std::uint64_t where)const {std::uint64_t value;machine.Read(where,std::as_writable_bytes(std::span(&value,1)));return value;}
 void dump()const {const auto r=receipt();std::cerr<<"guest semaphore fault receipt mode="<<r[1]<<" magic="<<r[0]
   <<" failures="<<r[4]<<" parked="<<r[6]<<" returned="<<r[7]<<" handle="<<r[16]<<" statuses:";
   for(unsigned i=70;i<98;++i)std::cerr<<' '<<r[i];std::cerr<<" trace:";
   for(unsigned i=0;i<r[5] && i<16;++i)std::cerr<<' '<<r[100+i];std::cerr<<'\n';
 }
 void finish(){Cpu::StopReason reason;
  try{reason=graph->RunMain(1000000,10000);}catch(...){dump();throw;}
  if(reason!=Cpu::StopReason::Exit || machine.ExitCode()!=0)dump();
  require(reason==Cpu::StopReason::Exit,"Semaphore translated guest did not reach process exit");
 }
 State parked(){Cpu::GuestPhaseBudget budget(30000);const auto reason=threads->RunEntry(budget);const auto value=receipt();
   const bool parked=reason==Cpu::StopReason::InstructionLimit && value[63]==1 && value[61]>0 &&
     value[6]==7 && value[7]==0 && value[60]==0 && value[32] && value[33];
   if(!parked)dump();require(parked,"Semaphore null wait fabricated return or failed to park actual guest continuation");return value;}
};
void common(const State& r) {
 require(r[0]==0x53454d4150484f52ULL && r[4]==0 && r[20]==Canary && r[21]==Canary &&
         r[126]==Canary && r[127]==~Canary,"Semaphore receipt, TLS, status or immediate neighbor canaries differ");
 require((r[16]>>32)!=0 && (r[16]&0xffffffff)!=0,"Opaque64 guest identity lost significant upper bits");
}
void trace(const State& r,std::initializer_list<std::uint64_t> values) {
 if(r[5]!=values.size() || !std::equal(values.begin(),values.end(),r.begin()+100)) {
  std::cerr<<"guest semaphore trace mode="<<r[1]<<":";
  for(unsigned i=0;i<r[5] && i<16;++i)std::cerr<<' '<<r[100+i];std::cerr<<'\n';
  throw std::runtime_error("Independent semaphore priority/stable-tie trace differs");
 }
}
void completed(const char* path,unsigned mode) {
 Session s(path,mode);s.finish();const auto r=s.receipt();common(r);
 const unsigned count=mode==9?2:3;
 require(r[60]==0 && r[62]==(1ULL<<count)-1 && r[7]==(1ULL<<count)-1,
         "Semaphore Wait returned before peer Signal/Delete or failed exact wake");
 require(r[32] && r[33] && r[35]==0 && r[36]==0,"Semaphore suspended PC/SP/register/spill restoration differs");
 std::set<std::uint64_t> ids{r[11]};for(unsigned i=0;i<count;++i){ids.insert(r[8+i]);require(r[48+i]==0x3000+i,"Semaphore guest join payload differs");}
 require(ids.size()==count+1 && !ids.contains(0),"Semaphore control lost actual distinct guest threads");
 if(mode==9){trace(r,{2,1});require(r[64]==2 && r[40]==0 && r[41]==0,"Signal did not skip nonfitting priority waiter and reserve exact count");}
 else {
  trace(r,{2,3,1});
  for(unsigned i=0;i<3;++i)require(r[40+i]==(mode==1 || (mode==3 && i!=1)?Deleted:0),
   "Delete changed selected success or returned wrong still-blocked result");
  if(mode==0)require(r[64]==2 && r[65]==6,"Signal1 reserved tokens twice or woke more than one eligible waiter");
 }
 std::cout<<"PASS actual semaphore guest mode "<<mode<<" PC/SP/TLS/callee-saved and owned completion\n";
}
void counts(const char* path,unsigned mode) {
 Session s(path,mode);s.finish();const auto r=s.receipt();common(r);
 require(r[17]>r[16] && r[17]>>32 && r[96]==Missing && r[97]==Missing,"Deleted semaphore identity recycled or stale admission changed");
 if(mode==5)require(r[70]==Invalid && r[71]==0 && r[72]==0 && r[73]==0,"INT32_MAX overflow or exact large consumption differs");
 else {
  for(unsigned i:{70,72,73,80,81,83})require(r[i]==0,"Positive signed low32 consume/signal rejected poisoned upper bits or lost tokens");
  for(unsigned i:{71,74,75,76,77,78,79,82,87,88,93,94,95})require(r[i]==Invalid,"Unsupported/count/overflow guard failed before mutation");
  for(unsigned i:{84,85,86})require(r[i]==Missing,"64-bit token admitted low32 alias or changed high word");
  require(r[89]==0xfeedbeefdeadbeefULL,"Rejected Create changed eight-byte output");
 }
 std::cout<<"PASS guest signed count, upper poison, overflow and fresh/stale opaque64 mode "<<mode<<'\n';
}
// A real higher-priority mutex donor makes effective!=base before semaphore
// insertion. This distinguishes public base-at-block snapshot from accidentally
// sorting by donated/effective priority; ordinary priority tests cannot do so.
void baseSnapshot(const char* path) {
 Session s(path,13);Cpu::GuestPhaseBudget budget(30000);
 require(s.threads->RunEntry(budget)==Cpu::StopReason::InstructionLimit,"Snapshot fixture did not retain parked donation stage");
 const auto before=s.receipt();
 require(before[63]==1 && before[6]==3 && before[7]==0 && before[67]==1 && before[68]==0 &&
         s.threads->BasePriority(before[8])==600 && s.threads->EffectivePriority(before[8])==300 &&
         s.threads->BasePriority(before[9])==400,"Snapshot control lacks real donation differing from semaphore base priority");
 s.put(s.address+92*8,1);s.finish();const auto r=s.receipt();common(r);trace(r,{2,1});
 require(r[64]==2 && r[40]==0 && r[41]==0 && r[68]==1 && r[69]==0x4000 && r[35]==0 && r[36]==0,
         "Semaphore selected donated effective priority instead of base-at-block snapshot");
 std::cout<<"PASS real mutex donation distinguishes semaphore base snapshot from effective priority\n";
}
void identity(const char* path) {
 // The native translator has one bridge context. Capture the first session's
 // truly live token, terminate that session, then replay its exact opaque64
 // value into a fresh session's genuine imports. This detects identity reuse
 // without treating simultaneous bridge creation as a semaphore failure.
 State before{};{Session first(path,2);before=first.parked();}
 Session second(path,6);second.put(second.address+90*8,before[16]);second.finish();const auto r=second.receipt();common(r);
 require(r[16]!=before[16] && r[70]==Missing && r[71]==Missing && r[72]==Missing,
         "Fresh session admitted previous-session opaque semaphore token or recycled identity");
 std::cout<<"PASS actual imported cross-session full64 handle rejection\n";
}
void memory(const char* path) {
 // A valid aligned page-end out8 and its immediately preceding word are real
 // guest memory; the following page is absent, detecting accidental out16 too.
 {Session s(path,7);s.machine.Map(Page,4096,Cpu::Permission::Read|Cpu::Permission::Write);
  s.put(Page+4080,Canary);s.put(Page+4088,0xfeedbeefdeadbeefULL);s.put(s.address+90*8,Page+4088);s.finish();
  const auto r=s.receipt();common(r);require(r[70]==0 && s.get(Page+4088)==r[16] && s.get(Page+4080)==Canary,
   "Create did not write full8 at exact page-end or damaged immediate neighbor");}
 for(unsigned control=0;control<5;++control){
  Session s(path,7);s.machine.Map(Page,4096,Cpu::Permission::Read|Cpu::Permission::Write);
  s.put(Page+4080,Canary);s.put(Page+4088,0xfeedbeefdeadbeefULL);
  const auto output=control==0?Page+4092:control==1?Page+8192:Page+4088;
  s.put(s.address+90*8,output);
  if(control==2)s.machine.Protect(Page,4096,Cpu::Permission::Read);
  if(control>=3){s.put(s.address+98*8,1);s.put(s.address+91*8,control==3?Page+8192:Page);
   if(control==4){std::array<std::byte,4096> bytes;bytes.fill(std::byte{'x'});s.machine.Write(Page,bytes);}}
  rejects([&]{s.graph->RunMain(1000000,10000);});const auto r=s.receipt();
  require(r[0]==0 && r[16]==0 && r[70]==0 && r[7]==0,"Invalid memory published semaphore result or success");
  if(control!=4)require(s.get(Page+4080)==Canary && s.get(Page+4088)==0xfeedbeefdeadbeefULL,
    "Invalid full8/name Create mutated guest memory before rejection");
 }
 {Session s(path,7);s.put(s.address+90*8,0);s.finish();require(s.receipt()[70]==Invalid && !s.receipt()[16],"Null output fabricated semaphore success");}
 {Session s(path,7);s.machine.Map(Page,4096,Cpu::Permission::Read|Cpu::Permission::Write);
  s.put(Page+4088,0xfeedbeefdeadbeefULL);s.put(s.address+90*8,Page+4088);s.put(s.address+98*8,1);s.finish();
  require(s.receipt()[70]==Invalid && !s.receipt()[16] && s.get(Page+4088)==0xfeedbeefdeadbeefULL,
          "Null name changed output or manufactured semaphore success");}
 std::cout<<"PASS imported out8 page-end, short/unmapped/read-only/name guards before mutation\n";
}
void lifecycle(const char* path) {
 for(unsigned control=0;control<3;++control){
  Session s(path,2);const auto before=s.parked();const auto gates=s.gates;
  if(control==0)s.semaphores.reset();else if(control==1)s.semaphores->Shutdown();else s.withdraw();
  Cpu::GuestPhaseBudget retry(10000);
  if(control==2)rejects([&]{s.threads->RunEntry(retry);});
  else require(s.threads->RunEntry(retry)==Cpu::StopReason::Requested,"Provider shutdown retained runnable semaphore continuation");
  require(retry.Consumed()==0 && s.receipt()==before,"Provider/scheduler shutdown manufactured successful semaphore return");
  if(control==0){for(const auto gate:gates)rejects([&]{s.machine.CheckAccess(gate,1,Cpu::Permission::Execute);});
   s.semaphores=std::make_unique<Cpu::Platform::KernelSemaphores>(s.machine,s.threads);
   const auto gate=s.semaphores->Resolve(scoped("188x57JYp0g"),2,0);require(gate && gates.contains(*gate),"Replacement gate reuse control is absent");
   require(s.receipt()==before && s.get(before[18])==before[16],"Provider replacement wrote stale guest slot");}
 }
 {Session s(path,2);const auto before=s.parked();s.machine.RequestStop();Cpu::GuestPhaseBudget retry(10000);
  require(s.threads->RunEntry(retry)==Cpu::StopReason::Requested && retry.Consumed()==0 && s.receipt()==before,
   "Owner stop resumed parked semaphore successfully");}
 {Session s(path,12);const auto before=s.parked();s.put(s.address+92*8,1);
  s.threads->SetOwnerBoundary([&](bool){if(s.receipt()[66])s.machine.RequestStop();},std::chrono::milliseconds(10));
  Cpu::GuestPhaseBudget budget(100000);require(s.threads->RunEntry(budget)==Cpu::StopReason::Requested,
   "Selected/deleted pending semaphore callback ignored owner stop");
  const auto stopped=s.receipt();require(stopped[66]==1 && stopped[7]==0 && stopped[0]==0 && stopped[16]==before[16],
   "Selected wake returned after owner stop or lacked actual signal/delete stage");
  const auto gates=s.gates;s.semaphores.reset();
  for(const auto gate:gates)rejects([&]{s.machine.CheckAccess(gate,1,Cpu::Permission::Execute);});
  require(s.receipt()==stopped,"Selected-object provider destruction manufactured stale guest writes");
 }
 {Session s(path,11);s.finish();const auto r=s.receipt();require(r[6]==7 && r[7]==0 && r[0]==0,"Guest process exit manufactured semaphore completion");}
 {Session s(path,10);unsigned idle=0;s.threads->SetOwnerBoundary([&](bool waiting){if(waiting)++idle;},std::chrono::milliseconds(5));
  Cpu::GuestPhaseBudget budget(100000);require(s.threads->RunEntry(budget)==Cpu::StopReason::Requested && idle>0 &&
    s.receipt()[60]==1 && s.receipt()[0]==0 && s.receipt()[70]==0,
    "Diagnostic idle cap became semaphore timeout/success or blocked host owner");}
 std::cout<<"PASS actual parked withdrawal, replacement, owner stop, guest exit and diagnostic idle cancellation\n";
}
void continuation(const char* path) {
 Session s(path,8);const auto before=s.parked();require(s.get(before[32])==before[33],"Control lacks actual wait return word");
 s.put(before[32],0x1122334455667788ULL);s.put(s.address+92*8,1);
 rejects([&]{s.graph->RunMain(1000000,10000);});const auto after=s.receipt();
 require(after[7]!=7 && after[40]==0 && after[0]==0 && s.get(before[32])==0x1122334455667788ULL,
  "Corrupt suspended continuation fabricated semaphore completion");
 Cpu::GuestPhaseBudget retry(10000);require(s.threads->RunEntry(retry)==Cpu::StopReason::Requested && retry.Consumed()==0 && s.receipt()==after,
  "Rejected owner completion retained runnable stale wake");
 std::cout<<"PASS exact suspended gate corruption rejects before owned semaphore return\n";
}
void admission() {
 Cpu::Machine machine;auto threads=std::make_shared<Cpu::GuestThreads>(machine);
 Cpu::Platform::TargetKernelSemaphores target(machine,threads);
 constexpr std::uint64_t GatePage=0x7ffdc3000000;
 std::array<std::byte,4096> originalGateBytes{};
 machine.Read(GatePage,originalGateBytes);
 const auto originalMappingCount=machine.Mappings().size();
 constexpr std::array nids{"188x57JYp0g","R1Jvn8bSCW8","Zxa0VhQVTsk","4czppHBiriw"};
 for(const auto nid:nids){const auto row=scoped(nid);
  for(unsigned control=0;control<8;++control){auto wrong=row;unsigned type=2;std::uint64_t size=0;
   if(control==0)type=1;if(control==1)size=8;
   if(control==2)wrong.LibraryName="foreign";if(control==3)wrong.ModuleName="foreign";
   if(control==4)wrong.LibraryVersion=2;if(control==5)wrong.ModuleMajor=2;if(control==6)wrong.ModuleMinor=2;
   if(control==7)type=6;
   bool denied=false;try{target.Resolve(wrong,type,size);}catch(const std::exception&){denied=true;}
   if(!denied)std::cerr<<"semaphore selector unexpected admission nid="<<nid<<" control="<<control
                      <<" type="<<type<<" symbol_size="<<size<<'\n';
   require(denied,"Target semaphore type/size/scope rejection absent");
   std::array<std::byte,4096> afterGateBytes{};machine.Read(GatePage,afterGateBytes);
   require(afterGateBytes==originalGateBytes && machine.Mappings().size()==originalMappingCount,
           "Invalid selector changed callable gate bytes/mapping before admission");
  }
 }
 for(const auto nid:nids){
  require(target.Resolve(scoped(nid),2,0).has_value(),"Semaphore row rejected");
  // Import-table ids are per-image; another title's ids are admitted.
  auto other=scoped(nid);other.LibraryId=1;other.ModuleId=1;
  require(target.Resolve(other,2,0).has_value(),"Another image's semaphore import-table ids were refused");
 }
 for(const auto nid:{"12wOHk8ywb0","4DM06U2BNEY","AAAAAAAAAAA"})
  require(!target.Resolve(scoped(nid),2,0),"Unobserved Poll/Cancel/unknown NID fabricated callable semaphore gate");
 std::cout<<"PASS native selector only: title-agnostic four FUNC2 size0 scopes; no title execution\n";
}
}
int main(int argc,char** argv) {
 try{require(argc==3,"Usage: KernelSemaphoreTest packaged.elf case");const std::string name=argv[2];
  if(name=="priority"){completed(argv[1],0);completed(argv[1],9);baseSnapshot(argv[1]);}else if(name=="deletion")completed(argv[1],1);
  else if(name=="selected")completed(argv[1],3);else if(name=="count")counts(argv[1],4);
  else if(name=="overflow")counts(argv[1],5);else if(name=="identity")identity(argv[1]);
  else if(name=="memory")memory(argv[1]);else if(name=="lifecycle")lifecycle(argv[1]);
  else if(name=="continuation")continuation(argv[1]);else if(name=="admission")admission();else throw std::runtime_error("Unknown semaphore case");
 }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}return 0;
}
