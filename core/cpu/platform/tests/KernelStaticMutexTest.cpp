#include "KernelPrimitives.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceThreadImports.hpp>
#include <array>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

// Authoring gate evidence: evidencecpu12/fixture/test-audit-authoring.md.
// The real linked libc static imports cross a boundary absent from CPU11's
// explicitly initialized mutex fixture. Independent payload/ownership/order
// observations expose premature acquisition/wake and identity/error regressions.
namespace {
constexpr std::uint64_t Bias=0x1000000;
constexpr std::uint64_t Busy=0x80020010, Payload=0x1020304050607080;
using State=std::array<std::uint64_t,96>;
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
template<class F> void rejects(F action,const char* diagnostic) {
 try { action(); }
 catch(const std::exception& error) {
  require(std::string(error.what()).find(diagnostic)!=std::string::npos,error.what());return;
 }
 throw std::runtime_error("Expected qualified static mutex rejection");
}
Cpu::SceImport scoped(std::string_view nid) {
 Cpu::SceImport value;value.Nid=nid;value.LibraryName=value.ModuleName="libkernel";
 value.LibraryVersion=value.ModuleMajor=value.ModuleMinor=1;value.LibraryId=0;value.ModuleId=1;return value;
}
struct Session {
 Cpu::Machine machine;
 std::shared_ptr<Cpu::GuestThreads> threads=std::make_shared<Cpu::GuestThreads>(machine);
 Cpu::SceThreadImports threadImports{machine,threads};
 Cpu::SceLifecycleImports lifecycle{machine};
 std::unique_ptr<Cpu::Platform::TargetKernelMutexes> mutexes;
 std::unique_ptr<Cpu::SceModules> graph;
 std::uint64_t receiptAddress=0;
 std::set<std::uint64_t> gates;
 bool withdrawn=false;
 Session(const char* path,unsigned mode) {
  require(std::string(Cpu::Machine::Backend()).find("Modern QEMU TCG")!=std::string::npos,
          "Static mutex fixture requires native Modern QEMU TCG");
  mutexes=std::make_unique<Cpu::Platform::TargetKernelMutexes>(machine,threads);
  lifecycle.SetProcessExitHandler([runtime=std::weak_ptr<Cpu::GuestThreads>(threads)](int code) {
   const auto owner=runtime.lock();require(bool(owner),"Static mutex process-exit scheduler expired");
   owner->ProcessExitFromHostCall(code);
  });
  const std::array hosts{Cpu::SceHostModule{"libkernel.prx",{"libkernel",1,1,1},{{"libkernel",0,1}}}};
  graph=std::make_unique<Cpu::SceModules>(machine,Cpu::SceModuleFile{path,Bias},
   std::span<const Cpu::SceModuleFile>{},hosts,
   [&](const Cpu::SceImport& import,std::uint8_t type)->std::optional<Cpu::SceResolvedImport> {
    require(type==2,"Linked static mutex import lost mandatory function type");
    if(const auto gate=mutexes->Resolve(import,type)) {
     gates.insert(*gate);return Cpu::SceResolvedImport{*gate,type};
    }
    if(const auto gate=mutexes->ResolveCondition(import,type,0)) {
     gates.insert(*gate);return Cpu::SceResolvedImport{*gate,type};
    }
    if(const auto gate=threadImports.Resolve(import,type)) return Cpu::SceResolvedImport{*gate,type};
    if(const auto gate=lifecycle.Resolve(import)) return Cpu::SceResolvedImport{*gate,type};
    return std::nullopt;
   });
  const auto& parsed=graph->Modules()[0].Image;
  require(parsed.Imports.size()==13 && parsed.NeededFiles.empty() && parsed.NeededModules.size()==1 &&
          parsed.Tls && gates.size()==8,"Static fixture lost linked libc/kernel imports or TLS");
  require(std::set<std::uint32_t>(parsed.RelocationTypes.begin(),parsed.RelocationTypes.end())==
          std::set<std::uint32_t>{7,8},"Static fixture lost genuine PLT/RELATIVE relocations");
  for(const auto& item:parsed.Exports) if(item.Type==1 && item.Size==sizeof(State)) {
   require(!receiptAddress,"Ambiguous static fixture receipt");receiptAddress=Bias+item.Value;
  }
  require(receiptAddress && receipt()==State{},"Loading executed guest fixture or lost receipt");
  put(receiptAddress+8,mode);
  if(mode==2 || mode==7) {machine.Map(0x6000,4096,Cpu::Permission::Read);put(receiptAddress+90*8,0x6000);}
  Cpu::SetupSceEntry(machine,graph->Main(),{"public-libc-static-mutex"},graph->EntryTerminationGate());
  threads->AdoptInitial({graph->Main().Entry,graph->InitialStack(),graph->Tls(),graph->ThreadTlsFactory()});
  graph->SetExecutor(threads->ModuleExecutor());graph->InitializeDependencies();
 }
 ~Session() {if(!withdrawn) threads->Withdraw();}
 State receipt() const {State s{};machine.Read(receiptAddress,std::as_writable_bytes(std::span(s)));return s;}
 void put(std::uint64_t p,std::uint64_t value) {machine.Write(p,std::as_bytes(std::span(&value,1)));}
 std::uint64_t get(std::uint64_t p) const {std::uint64_t v;machine.Read(p,std::as_writable_bytes(std::span(&v,1)));return v;}
 void withdraw() {threads->Withdraw();withdrawn=true;}
};
void completed(const char* path,unsigned mode) {
 Session session(path,mode);
 require(session.graph->RunMain(1000000,30000)==Cpu::StopReason::Exit && session.machine.ExitCode()==0,
         "Static mutex contention/condition guest did not finish");
 const auto s=session.receipt();
 require(s[0]==0x535441544d555458 && !s[4] && !s[5] && !s[6],
         "Static mutex status, guarded slot or exclusive payload differs");
 require(s[7] && s[8] && s[9] && s[7]!=s[8] && s[7]!=s[9] && s[8]!=s[9],
         "Static mutex fixture lost distinct actual guest identities");
 require(s[35]==s[30]+8 && !s[36] && !s[37] && s[38]==1 && !s[39],
         "Libc-shaped adjacent zero static slots or unowned zero unlock differs");
 require(s[31]>2 && s[31]!=s[30] && s[32]>2 && s[32]!=s[35] && s[33]==s[31] &&
         s[25]==s[31] && s[26]==s[32],"Mutex/condition lost opaque shared identity during wait");
 if(!(s[12]==3 && s[13]==3 && s[14]==3 && s[15]==3 && s[16]==1 && s[17]==1 && s[18]==2)) {
  std::cerr<<"static receipt mode="<<mode;
  for(unsigned index=12;index<=20;++index) std::cerr<<" ["<<index<<"]="<<s[index];
  std::cerr<<'\n';
 }
 require(s[12]==3 && s[13]==3 && s[14]==3 && s[15]==3 && s[16]==1 && s[17]==1 && s[18]==2,
         "Direct contenders acquired early or condition wait failed to release its actual mutex");
 require(s[19]==(mode==5 ? 0U : 1U) && s[20]==1 && s[21]==Busy && s[22]==1 && s[23]==Busy,
         "Condition broadcast bypassed reacquisition or foreign ownership while waiter yielded");
 require(s[24]==Payload+3 && s[64]==1 && s[65]==2 && s[46]==0x1200 && s[47]==0x1201,
         "Independent FIFO wake, protected payload or thread join result differs");
 for(unsigned role=0;role<2;++role)
  require(s[40+role]==0 && s[42+role]==Busy && s[44+role]==11,
          "Condition returned without default nonrecursive mutex ownership or POSIX error namespace");
 require(s[56]==11 && s[57]==Busy && s[58]==Busy && s[59]==Busy && s[60]==Busy && s[61]==1,
         "Static default/self ownership or live condition mutex destroy behavior differs");
 require(s[50]==22 && s[51]==22 && s[52]==22 && s[53]==22 && s[54]==s[31] &&
         s[55]==0x1122334455667788 && s[77]==22 && s[78]==22 && !s[79] && s[80]==22 && s[81]==s[32],
         "Copied/forged/live re-zero slot fabricated success or changed source identity");
 require(s[27]==1 && s[62]==22 && s[63]==22 && !s[69] && s[28]==2 && s[29]==2 &&
         s[70]==22 && s[71]==22 && s[72]==22 && !s[73],
         "Destroyed mutex/condition or destroyed re-zero slot fabricated static initialization");
 require(s[34]>2 && s[34]!=s[31] && s[74]==22 && s[75]==22 && s[76]==s[31],
         "Explicit recreation reused stale identity or accepted old token at the original slot");
}
void invalidPointer(const char* path,unsigned mode,const char* diagnostic) {
 Session session(path,mode);
 rejects([&]{session.graph->RunMain(500000,10000);},diagnostic);
 const auto s=session.receipt();
 require(!s[0] && !s[4] && !s[5] && !s[6] && s[31]>2 && session.get(s[30])==s[31] &&
         session.get(s[35])==0 && !s[12] && !s[13] && !s[15],
         "Invalid/read-only static acquisition changed original mutex/condition or started guest waiters");
 if(mode==2 || mode==7) require(session.get(0x6000)==0,"Read-only static slot changed before rejection");
}
void conditionRollback(const char* path,unsigned mode) {
 Session session(path,mode);
 require(session.graph->RunMain(500000,10000)==Cpu::StopReason::Exit && session.machine.ExitCode()==0,
         "Static condition rollback guest did not finish");
 const auto s=session.receipt();
 require(s[0]==0x524f4c4c4241434b && !s[4] && !s[5] && !s[6] && s[31]>2 &&
         s[82]==(mode==8 ? 1U : 22U) && !s[83] && s[84]==s[31] &&
         session.get(s[30])==s[31] && session.get(s[35])==0 && !s[12] && !s[15],
         "Unowned/forged condition mutex initialized its zero condition or changed original mutex identity");
}
void cancellation(const char* path) {
 Session session(path,3);Cpu::GuestPhaseBudget budget(30000);
 require(session.threads->RunEntry(budget)==Cpu::StopReason::InstructionLimit,"Static condition owner spin did not reach budget");
 const auto before=session.receipt();
 require(before[16]==1 && before[17]==1 && before[89]>0 && !before[15] && !before[0],
         "Cancellation fixture failed to retain actual static condition waiters");
 const auto gates=session.gates;session.mutexes.reset();
 for(const auto gate:gates) rejects([&]{session.machine.CheckAccess(gate,1,Cpu::Permission::Execute);},"Guest access denied");
 session.mutexes=std::make_unique<Cpu::Platform::TargetKernelMutexes>(session.machine,session.threads);
 const auto gate=session.mutexes->Resolve(scoped("7H0iTOciTLo"),2);
 require(gate && gates.contains(*gate),"Static cancellation did not reuse retired provider page");
 Cpu::GuestPhaseBudget retry(1000);
 require(session.threads->RunEntry(retry)==Cpu::StopReason::Requested && retry.Consumed()==0 && session.receipt()==before &&
         session.get(before[30])==before[31] && session.get(before[35])==before[32],
         "Provider replacement resumed old static waiter or changed original slots");
 session.withdraw();session.mutexes.reset();require(session.receipt()==before,"Static scheduler withdrawal fabricated success");
}
void targetAdmission() {
 Cpu::Machine machine;auto threads=std::make_shared<Cpu::GuestThreads>(machine);
 Cpu::Platform::TargetKernelMutexes target(machine,threads);
 for(const auto nid:{"7H0iTOciTLo","2Z+PpY6CaJg"}) {
  const auto row=scoped(nid);require(target.Resolve(row,2).has_value(),"POSIX mutex row rejected");
  // Any importing image: other import-table ids and the libScePosix export.
  auto other=row;other.LibraryId=43;other.ModuleId=24;other.LibraryName="libScePosix";
  require(target.Resolve(other,2).has_value(),"Another image's POSIX mutex row was refused");
  for(const auto type:{0,1,6}) rejects([&]{target.Resolve(row,type);},"scope/version/type");
  for(unsigned field=0;field<5;++field) {
   auto wrong=row;
   if(field==0) wrong.LibraryName="libc";
   if(field==1) wrong.ModuleName="libc";
   if(field==2) wrong.LibraryVersion=2;
   if(field==3) wrong.ModuleMajor=2;
   if(field==4) wrong.ModuleMinor=2;
   rejects([&]{target.Resolve(wrong,2);},"scope/version/type");
  }
 }
 require(!target.Resolve(scoped("AAAAAAAAAAA"),2),"Unknown static mutex NID fabricated gate");
}
}
int main(int argc,char** argv) {
 try {
  require(argc==2,"Usage: KernelStaticMutexTest packaged.elf");
  completed(argv[1],0);completed(argv[1],5);
  invalidPointer(argv[1],1,"Invalid kernel primitive guest slot");
  invalidPointer(argv[1],2,"permission");invalidPointer(argv[1],4,"Guest access denied");
  invalidPointer(argv[1],6,"Invalid kernel primitive guest slot");
  invalidPointer(argv[1],7,"permission");invalidPointer(argv[1],9,"Guest access denied");
  invalidPointer(argv[1],10,"Invalid kernel primitive guest slot");
  conditionRollback(argv[1],8);conditionRollback(argv[1],11);
  cancellation(argv[1]);targetAdmission();
  std::cout<<"PASS linked libc static mutex default/contend, unlock-before-broadcast and held-owner reacquire, FIFO/payload, POSIX/SCE errors, slot identity/re-zero/stale, invalid/read-only rollback and cancellation\n";
 } catch(const std::exception& error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
