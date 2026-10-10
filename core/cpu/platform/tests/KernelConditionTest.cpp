#include "KernelPrimitives.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceThreadImports.hpp>
#include <array>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

// Test-audit: actual condition PLT calls release and reacquire the same owned
// mutex, park until an explicit FIFO signal/broadcast, and retain slot identity
// and cancellation. Missing release/reacquire, token wake, partial broadcast or
// stale identities break independent guest ownership/data/order/return oracles.
// Existing mutex/event/RR tests import no condition calls and miss this crossing.
// No test-only production seam: typed SceModules and GuestThreads own execution.
namespace {
constexpr std::uint64_t Bias=0x1000000;
constexpr std::uint64_t Busy=0x80020010, Perm=0x80020001, Invalid=0x80020016;
using State=std::array<std::uint64_t,128>;
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
template<class F> void rejects(F action,const char* diagnostic) {
 try { action(); }
 catch(const std::exception& error) {
  require(std::string(error.what()).find(diagnostic)!=std::string::npos,error.what()); return;
 }
 throw std::runtime_error("Expected qualified condition rejection");
}
Cpu::SceImport scoped(std::string_view nid) {
 Cpu::SceImport value;value.Nid=nid;value.LibraryName=value.ModuleName="libkernel";
 value.LibraryVersion=value.ModuleMajor=value.ModuleMinor=1;value.LibraryId=44;value.ModuleId=24;return value;
}
struct Session {
    Cpu::Machine machine;
    std::shared_ptr<Cpu::GuestThreads> threads = std::make_shared<Cpu::GuestThreads>(machine);
    Cpu::SceThreadImports threadImports{machine, threads};
    Cpu::SceLifecycleImports lifecycle{machine};
    std::unique_ptr<Cpu::Platform::TargetKernelMutexes> mutexes;
    std::unique_ptr<Cpu::SceModules> graph;
    std::uint64_t receiptAddress = 0;
    std::set<std::uint64_t> gates;
    bool withdrawn = false;
    Session(const char* path, unsigned mode) {
        require(std::string(Cpu::Machine::Backend()).find("Modern QEMU TCG") != std::string::npos,
                "Condition scheduler fixture requires native Modern QEMU TCG");
        mutexes = std::make_unique<Cpu::Platform::TargetKernelMutexes>(machine, threads);
        lifecycle.SetProcessExitHandler([runtime = std::weak_ptr<Cpu::GuestThreads>(threads)](int code) {
            const auto owner = runtime.lock();
            require(bool(owner), "Condition fixture process-exit scheduler expired");
            owner->ProcessExitFromHostCall(code);
        });
        const std::array hosts{Cpu::SceHostModule{"libkernel.prx", {"libkernel", 24, 1, 1}, {{"libkernel", 44, 1},{"libScePosix", 43, 1}}}};
        graph = std::make_unique<Cpu::SceModules>(machine, Cpu::SceModuleFile{path, Bias},
                std::span<const Cpu::SceModuleFile>{}, hosts,
                [&](const Cpu::SceImport& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
                    require(type == 2, "Actual linked condition import lost its mandatory ELF function type");
                    if (const auto gate = mutexes->Resolve(import, type)) {
                        gates.insert(*gate); return Cpu::SceResolvedImport{*gate, type};
                    }
                    if (const auto gate = mutexes->ResolveCondition(import, type, 0)) {
                        gates.insert(*gate); return Cpu::SceResolvedImport{*gate, type};
                    }
                    if (const auto gate = threadImports.ResolveTargetPriority(import, type)) return Cpu::SceResolvedImport{*gate,type};
                    if (const auto gate = threadImports.Resolve(import, type)) return Cpu::SceResolvedImport{*gate, type};
                    if (const auto gate = lifecycle.Resolve(import)) return Cpu::SceResolvedImport{*gate, type};
                    return std::nullopt;
                });
        const auto& parsed = graph->Modules()[0].Image;
        require(parsed.Imports.size() == 29 && parsed.NeededFiles.empty() && parsed.NeededModules.size() == 1 &&
                parsed.Tls && gates.size() == 19, "Linked fixture lost its actual kernel/thread import and TLS graph");
        require(std::set<std::uint32_t>(parsed.RelocationTypes.begin(), parsed.RelocationTypes.end()) ==
                std::set<std::uint32_t>{7, 8}, "Condition fixture lost its genuine PLT/RELATIVE relocations");
        for (const auto& item : parsed.Exports) if (item.Type == 1 && item.Size == sizeof(State)) {
            require(!receiptAddress, "Ambiguous compiled fixture receipt"); receiptAddress = Bias + item.Value;
        }
        require(receiptAddress && receipt() == State{}, "Loading ran guest fixture code or lost the receipt");
        put(receiptAddress + 1 * 8, mode);
        if(mode==9) {machine.Map(0x6000,4096,Cpu::Permission::Read);put(receiptAddress+96*8,0x6000);}
        Cpu::SetupSceEntry(machine, graph->Main(), {"public-kernel-condition-fixture"}, graph->EntryTerminationGate());
        threads->AdoptInitial({graph->Main().Entry, graph->InitialStack(), graph->Tls(), graph->ThreadTlsFactory()});
        graph->SetExecutor(threads->ModuleExecutor());
        graph->InitializeDependencies();
    }
    ~Session() { if (!withdrawn) threads->Withdraw(); }
    State receipt() const {
        State state{}; machine.Read(receiptAddress, std::as_writable_bytes(std::span(state))); return state;
    }
    void put(std::uint64_t address, std::uint64_t value) { machine.Write(address, std::as_bytes(std::span(&value, 1))); }
    std::uint64_t get(std::uint64_t address) const {
        std::uint64_t value; machine.Read(address, std::as_writable_bytes(std::span(&value, 1))); return value;
    }
    void withdraw() { threads->Withdraw(); withdrawn = true; }
};

void completed(const char* path,unsigned mode) {
 Session session(path,mode);
 require(session.graph->RunMain(30000000,30000)==Cpu::StopReason::Exit && session.machine.ExitCode()==0,
         "Condition wait/signal/broadcast guest did not finish");
 const auto s=session.receipt();const unsigned count=mode==5 ? 2 : 3;
 require(s[0]==0x434f4e4457414954ULL && s[4]==0 && s[5]==0 && s[6]==0,
         "Condition status, guest payload or adjacent slot guards differ");
 require(s[7] && s[8] && s[9] && s[7]!=s[8] && s[8]!=s[9],"Fixture lost distinct guest identities");
 require(s[12]==((1ULL<<count)-1) && s[13]==s[12] && s[15]==count && s[16]==1 && s[19]==1,
         "Empty signal accumulated a token, wait lost wake, or owner failed atomic release");
 require(s[17]==Busy && s[18]==Busy,
         "Pending condition wait did not keep condition and mutex destroy busy");
 require(s[29]==0,"Condition destroy after broadcast returned busy while waiters only awaited the mutex (PLAT-15)");
 require(s[31]>2 && s[31]!=s[30] && s[33]>2 && s[33]!=s[32],"Condition/mutex did not retain opaque slot identities");
 require(s[95]>2 && s[95]!=s[31],"Static zero condition failed lazy initialization with fresh identity");
 require(s[34]==1 && s[35]==2,"Condition/mutex destroyed sentinel differs");
 require(s[40] && s[41] && s[42]==0 && s[43]==0 && s[44]==0,
         "Condition wait resumed wrong return value, callee-saved ABI, stack or spill canary");
 for(unsigned role=0;role<count;++role) {
  require(s[52+role]==0x3000+role,"Condition thread join returned wrong guest payload");
  const unsigned expected=mode==8 ? (role==0 ? 2 : (role==1 ? 3 : 1)) : role+1;
  require(s[64+role]==expected,"Condition signal FIFO or mutex priority reacquire order differs");
  if(role) require(s[45+role]==0,"Condition wait returned an unexpected error");
  if(mode!=7) require(s[48+role]==Busy,"Condition wait returned without reacquiring its owned mutex");
 }
 require(s[90]==0,"Copied live condition handle was rejected (PLAT-06)");
 require(s[91]==Invalid && s[92]==Perm && s[93]==Invalid && s[94]==Invalid && s[97]==22,
         "Destroyed or stale condition handle or unowned-mutex wait fabricated success");
 if(mode==0 || mode==5 || mode==7) {
  require(s[21]==1 && s[22]==Busy && s[27]==1 && s[28]==1,
          "Signal-one woke an extra waiter or Wait returned before actual mutex reacquisition");
  if(mode!=5) require(s[23]==1 && s[24]==Busy && s[25]==Perm && s[26]==Busy,
          "Foreign acquire/unlock/destroy bypassed condition waiter's exclusive (recursive) ownership");
 } else require(s[28]==0,"Broadcast returned waiters while parent still held their mutex");
 if(mode==6) {
  for(unsigned role=0;role!=3;++role) require(s[80+role]==100000,"RR condition worker lost finite progress");
  require(s[84]>0 && s[84]<100000,"Condition wake failed equal-priority RR interleaving");
 }
}
void cancellation(const char* path,unsigned mode=2) {
 Session session(path,mode);Cpu::GuestPhaseBudget budget(30000);
 require(session.threads->RunEntry(budget)==Cpu::StopReason::InstructionLimit,"Bounded owner did not park real condition waiter");
 const auto before=session.receipt();
 require(before[16]==1 && before[19]==1 && (mode==11 ? before[101]>0 : before[20]>0) && before[13]==0 && before[40],
         "Cancellation control did not retain actual suspended condition continuation");
 if(mode==11) {
  require(before[21]==1 && before[100]==1 && budget.Consumed()==30000 &&
          session.threads->BasePriority(before[7])==700 && session.threads->EffectivePriority(before[7])==500,
          "Condition key transfer lost effective-priority donation to actual mutex owner");
 }
 const auto gates=session.gates;session.mutexes.reset();
 for(const auto gate:gates) rejects([&]{session.machine.CheckAccess(gate,1,Cpu::Permission::Execute);},"Guest access denied");
 session.mutexes=std::make_unique<Cpu::Platform::TargetKernelMutexes>(session.machine,session.threads);
 const auto replacement=session.mutexes->ResolveCondition(scoped("WKAXJ4XBPQ4"),2,0);
 require(replacement && gates.contains(*replacement),"Cancellation control did not reuse retired gate page");
 Cpu::GuestPhaseBudget retry(10000);
 require(session.threads->RunEntry(retry)==Cpu::StopReason::Requested && retry.Consumed()==0,
         "Condition cancellation resumed old waiter through a replacement provider");
 require(session.receipt()==before && session.get(before[30])==before[31] && session.get(before[32])==before[33],
         "Canceled condition fabricated return, ownership or guest slot writes");
 session.withdraw();session.mutexes.reset();require(session.receipt()==before,"Scheduler withdrawal fabricated condition success");
}
void schedulerCancellationCleanup(const char* path) {
 Session session(path,2);Cpu::GuestPhaseBudget parked(30000);
 require(session.threads->RunEntry(parked)==Cpu::StopReason::InstructionLimit && session.receipt()[16]==1,
         "Scheduler cancellation cleanup did not park an actual condition waiter");
 const auto before=session.receipt();session.withdraw();
 require(session.receipt()==before,"Scheduler cancellation fabricated a condition return");
 // Keep the provider alive: an independent fresh translated Destroy must see
 // no stopped waiter binding. Provider destruction cannot hide leaked users.
 const auto gate=session.mutexes->ResolveCondition(scoped("g+PZd2hiacg"),2,0);
 require(gate.has_value(),"Live provider condition destroy gate missing after scheduler withdrawal");
 session.machine.Map(0x71000000,4096,Cpu::Permission::Read|Cpu::Permission::Write);
 session.machine.Map(0x71001000,4096,Cpu::Permission::Read|Cpu::Permission::Execute);
 session.put(0x71000ff8,0x71001000);
 session.machine.Set(Cpu::Register::Rsp,0x71000ff8);
 session.machine.Set(Cpu::Register::Rdi,before[30]);
 require(session.machine.Run(*gate,0x71001000,1000)==Cpu::StopReason::Address &&
         session.machine.Get(Cpu::Register::Rax)==0 && session.get(before[30])==1 && session.receipt()==before,
         "Stopped condition waiter leaked its live provider binding or fabricated guest success");
}

void invalidPointer(const char* path) {
 {Session session(path,3);
 rejects([&]{session.graph->RunMain(500000,10000);},"Invalid kernel primitive guest slot");
 require(session.receipt()[0]==0 && session.receipt()[13]==0,"Invalid condition pointer wrote completion");}
 Session readonly(path,9);
 rejects([&]{readonly.graph->RunMain(500000,10000);},"permission");
 require(readonly.receipt()[0]==0 && readonly.get(0x6000)==0,"Read-only condition pointer wrote completion or slot");
}
void corruptContinuation(const char* path) {
 Session session(path,4);
 rejects([&]{session.graph->RunMain(500000,10000);},"return word changed");
 const auto s=session.receipt();
 require(s[16]==1 && s[19]==1 && s[40] && s[41] && session.get(s[40])==0x1122334455667788ULL && !s[13] && !s[0],
         "Corruption control did not change actual condition return frame without fabricating completion");
 Cpu::GuestPhaseBudget retry(10000);
 require(session.threads->RunEntry(retry)==Cpu::StopReason::Requested && retry.Consumed()==0 && session.receipt()==s,
         "Corrupt condition wait retained runnable fabricated success");
}
void foreignProvider(const char* path) {
 Session session(path,2);Cpu::GuestPhaseBudget parked(30000);
 require(session.threads->RunEntry(parked)==Cpu::StopReason::InstructionLimit && session.receipt()[19]==1,
         "Foreign identity control did not park live original condition waiter");
 const auto before=session.receipt();
 // A distinct provider on the SAME owner Machine receives the original slot,
 // not a copied slot. Its non-overlapping gate page prevents aliasing either
 // provider; the real translated call cannot admit the first provider's token.
 Cpu::Platform::KernelPrimitives foreign(session.machine,session.threads,0x7ffdc3000000);
 const auto gate=foreign.Resolve(scoped("kDh-NfxgMtE"),2);
 require(gate.has_value(),"Foreign provider condition signal gate missing");
 const auto saved=session.machine.CaptureContext();
 session.machine.Map(0x71000000,4096,Cpu::Permission::Read|Cpu::Permission::Write);
 session.machine.Map(0x71001000,4096,Cpu::Permission::Read|Cpu::Permission::Execute);
 session.put(0x71000ff8,0x71001000);
 session.machine.Set(Cpu::Register::Rsp,0x71000ff8);
 session.machine.Set(Cpu::Register::Rdi,before[30]);
 session.machine.Set(Cpu::Register::Rip,*gate);
 require(session.machine.RunSlice(*gate,0x71001000,1000)==Cpu::StopReason::Address &&
         session.machine.Get(Cpu::Register::Rax)==Invalid,
         "Foreign condition provider admitted original live slot/token");
 session.machine.RestoreContext(saved);
 require(session.receipt()==before && session.get(before[30])==before[31],
         "Foreign condition rejection mutated original slot or suspended guest");
}

void targetAdmission() {
 Cpu::Machine machine;auto threads=std::make_shared<Cpu::GuestThreads>(machine);
 Cpu::Platform::TargetKernelMutexes target(machine,threads);
 constexpr std::array<std::string_view,12> nids{"2Tb92quprl0","g+PZd2hiacg","WKAXJ4XBPQ4","kDh-NfxgMtE","JGgj7Uvrl+A",
  "m5-2bsNfv7s","waPcxYiR3WA","0TyVk4MSLt0","RXXqi4CtF8w","Op8TBGY5KHg","2MOy+rUfuhQ","mkx2fVhNMsg"};
 unsigned admitted=0;
 // Import-table ids are per-image: every row is admitted at any title's ids.
 for(unsigned owner=0;owner!=3;++owner) for(unsigned op=0;op!=nids.size();++op) {
  auto row=scoped(nids[op]);
  const bool posix=op>=7;
  row.LibraryId=owner==0 ? (posix ? 43 : 44) : (owner==1 ? 0 : 7);
  row.ModuleId=owner==0 ? 24 : (owner==1 ? 1 : 9);
  if(owner==0 && posix) row.LibraryName="libScePosix";
  require(target.ResolveCondition(row,2,0).has_value(),"Condition row refused for an arbitrary image");++admitted;
  rejects([&]{target.ResolveCondition(row,2,1);},"type/size");
  for(const auto type:{0,1,6}) rejects([&]{target.ResolveCondition(row,type,0);},"type/size");
  for(unsigned field=0;field!=5;++field) {
   auto wrong=row;
   if(field==0) wrong.LibraryName="foreign";
   if(field==1) wrong.ModuleName="foreign";
   if(field==2) wrong.LibraryVersion=2;
   if(field==3) wrong.ModuleMajor=2;
   if(field==4) wrong.ModuleMinor=2;
   rejects([&]{target.ResolveCondition(wrong,2,0);},"scope/version");
  }
 }
 require(admitted==36,"Condition row coverage differs");
 // CPU13 exact timed admissions are owned by KernelConditionTimeoutTest;
 // retain the older unknown-NID denial without duplicating timed contracts.
 for(const auto nid:{"AAAAAAAAAAA"})
  require(!target.ResolveCondition(scoped(nid),2,0),"Unknown condition row fabricated a gate");
}

}
int main(int argc,char** argv) {
 try {
  require(argc==2,"Usage: KernelConditionTest packaged.elf");
  for(const auto mode:{0U,1U,5U,6U,7U,8U}) completed(argv[1],mode);
  cancellation(argv[1]);cancellation(argv[1],11);schedulerCancellationCleanup(argv[1]);invalidPointer(argv[1]);corruptContinuation(argv[1]);foreignProvider(argv[1]);targetAdmission();
  std::cout << "PASS linked SCE condition atomic release/reacquire, signal-one FIFO, broadcast-all, recursive ownership, priority/RR, identity, invalid pointer, ABI and cancellation\n";
 } catch(const std::exception& error) {std::cerr << "FAIL " << error.what() << '\n';return 1;}
}
