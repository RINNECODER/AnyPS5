#include "KernelPrimitives.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceThreadImports.hpp>
#include <algorithm>
#include <array>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

// Test-audit: real compiled guest scheduling, transferred ownership, blocked PLT
// ABI and payload protect the public priority-inheritance contract. No donation,
// missing transitivity, premature recursive withdrawal, stale restoration or FIFO
// selection instead of eligible priority changes this independent trace. CPU07's
// equal/default priority fixture cannot expose inversion or priority ordering.
// No production seam: typed SCE resolver, attrs, scheduler and mutex lifetime own
// this boundary; expected traces are written independently of scheduler internals.
namespace {
constexpr std::uint64_t Bias = 0x1000000, Canary = 0x1badc0ffeef00d55ULL;
using State = std::array<std::uint64_t, 128>;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F action, const char* diagnostic) {
 try { action(); }
 catch (const std::exception& error) {
  require(std::string(error.what()).find(diagnostic) != std::string::npos, error.what()); return;
 }
 throw std::runtime_error("Expected qualified priority rejection");
}
Cpu::SceImport scoped(std::string_view nid) {
 Cpu::SceImport value; value.Nid = nid; value.LibraryName = value.ModuleName = "libkernel";
 value.LibraryVersion = value.ModuleMajor = value.ModuleMinor = 1;
 value.LibraryId = value.ModuleId = 1; return value;
}
struct Session {
 Cpu::Machine machine;
 std::shared_ptr<Cpu::GuestThreads> threads = std::make_shared<Cpu::GuestThreads>(machine);
 Cpu::SceThreadImports imports{machine, threads};
 Cpu::SceLifecycleImports lifecycle{machine};
 std::unique_ptr<Cpu::Platform::KernelPrimitives> mutexes;
 std::unique_ptr<Cpu::SceModules> graph;
 std::uint64_t address = 0;
 std::set<std::uint64_t> mutexGates;
 Session(const char* path, unsigned mode) {
  require(std::string(Cpu::Machine::Backend()).find("Modern QEMU TCG") != std::string::npos,
          "Priority fixture requires native Modern QEMU TCG");
  mutexes = std::make_unique<Cpu::Platform::KernelPrimitives>(machine, threads);
  lifecycle.SetProcessExitHandler([runtime = std::weak_ptr<Cpu::GuestThreads>(threads)](int code) {
   const auto owner = runtime.lock(); require(bool(owner), "Priority scheduler expired");
   owner->ProcessExitFromHostCall(code);
  });
  const std::array hosts{Cpu::SceHostModule{"libkernel.prx", {"libkernel", 0, 1, 1}, {{"libkernel", 0, 1}}}};
  graph = std::make_unique<Cpu::SceModules>(machine, Cpu::SceModuleFile{path, Bias},
   std::span<const Cpu::SceModuleFile>{}, hosts,
   [&](const Cpu::SceImport& row, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
    require(type == 2, "Linked priority import lost ELF function type");
    if (const auto gate = mutexes->Resolve(row, type)) {
     mutexGates.insert(*gate); return Cpu::SceResolvedImport{*gate, type};
    }
    if (const auto gate = imports.ResolvePriority(row, type)) return Cpu::SceResolvedImport{*gate, type};
    if (const auto gate = imports.Resolve(row, type)) return Cpu::SceResolvedImport{*gate, type};
    if (const auto gate = lifecycle.Resolve(row)) return Cpu::SceResolvedImport{*gate, type};
    return std::nullopt;
   });
  const auto& parsed = graph->Modules()[0].Image;
  require(parsed.Imports.size() == 20 && parsed.NeededFiles.empty() && parsed.NeededModules.size() == 1 &&
          parsed.Tls && mutexGates.size() == 9, "Priority fixture lost its actual import/TLS graph");
  require(std::set<std::uint32_t>(parsed.RelocationTypes.begin(), parsed.RelocationTypes.end()) ==
          std::set<std::uint32_t>{7, 8}, "Priority fixture lost genuine linked PLT/RELATIVE relocations");
  for (const auto& item : parsed.Exports) if (item.Type == 1 && item.Size == sizeof(State)) {
   require(!address, "Ambiguous priority receipt"); address = Bias + item.Value;
  }
  require(address && state() == State{}, "Priority loading executed guest code or lost receipt");
  put(address + 8, mode); put(address + 126*8, Canary); put(address + 127*8, ~Canary);
  Cpu::SetupSceEntry(machine, graph->Main(), {"public-priority-fixture"}, graph->EntryTerminationGate());
  threads->AdoptInitial({graph->Main().Entry, graph->InitialStack(), graph->Tls(), graph->ThreadTlsFactory(), mode == 9 ? 256 : 700});
  graph->SetExecutor(threads->ModuleExecutor()); graph->InitializeDependencies();
 }
 ~Session() { threads->Withdraw(); }
 State state() const {
  State value{}; machine.Read(address, std::as_writable_bytes(std::span(value))); return value;
 }
 void put(std::uint64_t where, std::uint64_t value) { machine.Write(where, std::as_bytes(std::span(&value,1))); }
 std::uint64_t get(std::uint64_t where) const {
  std::uint64_t value; machine.Read(where, std::as_writable_bytes(std::span(&value,1))); return value;
 }
 void parked() {
  // This budget is a progress ceiling. Explicit guest yields establish the stage;
  // no ordering depends on stopping at a particular instruction or quantum edge.
  Cpu::GuestPhaseBudget budget(100000);
  require(threads->RunEntry(budget) == Cpu::StopReason::InstructionLimit,
          "Priority setup did not retain its deliberately bounded idle stage");
  const auto r = state();
  require(r[2] == 1 && r[16] == 1 && r[18] == 1 && r[20] == 1 && r[5] == 0 && r[17] == 0,
          "Priority setup did not retain a real parked owner/high waiter and competitor");
 }
};
void trace(const State& r, std::initializer_list<std::uint64_t> expected) {
 if (r[5] != expected.size() || !std::equal(expected.begin(), expected.end(), r.begin()+64)) {
  std::cerr << "trace mode=" << r[1] << ":";
  for (unsigned i = 0; i < r[5] && i < 48; ++i) std::cerr << ' ' << r[64+i];
  std::cerr << '\n'; throw std::runtime_error("Independent guest priority/order/restoration oracle differs");
 }
}
void completed(const char* path, unsigned mode, std::initializer_list<std::uint64_t> order,
               std::initializer_list<std::uint64_t> returns) {
 Session session(path, mode);
 require(session.graph->RunMain(1000000, 10000) == Cpu::StopReason::Exit && session.machine.ExitCode() == 0,
         "Priority guest did not finish within the fixed progress ceiling");
 const auto r = session.state(); trace(r, order);
 require(r[0] == 0x5052494f52495459ULL && r[4] == 0 && r[43] == 0 && r[44] == 0 &&
         r[126] == Canary && r[127] == ~Canary, "Priority guest statuses/data/slot or receipt redzones differ");
 for (const auto index : {112,113,114,115,116})
  require(r[index] == 0x8002002d, "Priority attribute invalid argument status differs");
 for (const auto index : {117,119,120,121,122})
  require(r[index] == 0x80020016, "Copied/stale/null priority attribute slot was admitted");
 require(r[118] == 0xfeedbeef000002ffULL, "Rejected attribute getter changed output redzone/data");
 require(r[34] == 0 && r[35] == 0 && r[36] == 0 && r[32] && r[33],
         "Inherited blocked PLT return, preserved registers or stack/spill canary differ");
 require(r[38] == 0x80020010 && r[39] == 0x80020001,
         "Low owner stole or unlocked a mutex reserved for the eligible waiter");
 require(r[37] == 0x2131415161718191ULL && r[17] == 1,
         "Inherited waiter acquired before owner published independent protected data");
 require(std::equal(returns.begin(), returns.end(), r.begin()+48), "Priority guest join returned wrong child payload");
 std::set<std::uint64_t> identities{r[8], r[9], r[10], r[12]};
 if (mode == 1) identities.insert(r[11]);
 if (mode == 3) identities.insert(r[14]);
 if (mode == 3 || mode == 4 || mode == 8) identities.insert(r[13]);
 require(!identities.contains(0) && identities.size() == returns.size()+1,
         "Priority oracle did not execute distinct actual guest threads");
 if (mode == 2) require(r[40] == 1, "Recursive partial unlock released the blocked inheriting waiter");
 std::cout << "PASS native guest priority mode " << mode << '\n';
}
void runnableFifo(const char* path) {
 Session session(path,9);
 require(session.graph->RunMain(1000000,10000) == Cpu::StopReason::Exit && session.machine.ExitCode() == 0,
         "Equal priority runnable guest did not finish");
 const auto r=session.state(); trace(r,{60,61,62,70,71,72});
 require(r[0] == 0x5052494f52495459ULL && r[4] == 0 && r[44] == 0 && r[126] == Canary && r[127] == ~Canary &&
         r[48] == 0x2000 && r[49] == 0x2001 && r[50] == 0x2002,
         "Runnable FIFO guest status/payload/attribute output or redzones differ");
 const std::set<std::uint64_t> ids{r[8],r[9],r[10],r[11]};
 require(ids.size() == 4 && !ids.contains(0), "Runnable FIFO reused guest identity");
 std::cout << "PASS equal priority runnable FIFO across real explicit yields\n";
}
void cancellation(const char* path, bool corrupt) {
 Session session(path, corrupt ? 7 : 6); session.parked();
 const auto before = session.state();
 require(session.threads->BasePriority(before[9]) == 700 && session.threads->EffectivePriority(before[9]) == 300 &&
         session.threads->BasePriority(before[10]) == 300 && session.threads->BasePriority(before[12]) == 500,
         "Real blocked waiter did not donate to actual low owner without changing its base");
 if (corrupt) {
  require(before[32] && before[33] && session.get(before[32]) == before[33],
          "Cancellation control lacks its actual blocked executable return word");
  session.put(before[32], 0x1122334455667788ULL); session.put(session.address+3*8, 1);
  rejects([&] { session.graph->RunMain(1000000, 10000); }, "return word changed");
  require(session.state()[17] == 0 && session.state()[0] == 0,
          "Cancelled corrupt inheriting waiter published guest acquisition/completion");
 } else {
  const auto old = session.mutexGates; session.mutexes.reset();
  for (const auto gate : old) rejects([&] { session.machine.CheckAccess(gate,1,Cpu::Permission::Execute); },
                                     "Guest access denied");
  session.mutexes = std::make_unique<Cpu::Platform::KernelPrimitives>(session.machine, session.threads);
  const auto gate = session.mutexes->Resolve(scoped("9UK1vLZQft4"),2);
  require(gate && old.contains(*gate), "Replacement control did not reuse old provider gate page");
  require(session.state() == before && session.get(before[41]) == before[42],
          "Withdrawal/replacement fabricated success or replaced the stale owner token");
 }
 require(session.threads->BasePriority(before[9]) == 700 && session.threads->EffectivePriority(before[9]) == 700,
         "Wait cancellation retained priority donation on the real low owner");
 const auto settled = session.state(); Cpu::GuestPhaseBudget after(10000);
 require(session.threads->RunEntry(after) == Cpu::StopReason::Requested && after.Consumed() == 0 &&
         session.state() == settled, "Cancelled inherited wait retained executable success or stale replacement gate");
 std::cout << "PASS inherited waiter " << (corrupt ? "corrupt-return cancellation" : "provider withdrawal/replacement") << '\n';
}
void unsupported(const char* path) {
 for (unsigned mode : {10U,11U}) {
  Session session(path,mode);
  rejects([&] { session.graph->RunMain(1000000,10000); },
          mode == 10 ? "priority protocol" : "OTHER scheduling policy");
  const auto r=session.state();
  require(r[0] == 0 && r[5] == 0 && r[126] == Canary && r[127] == ~Canary,
          "Unsupported priority behavior published execution or changed redzones");
  if(mode != 10) {
   require(r[55] && r[56] && session.get(r[55]) == r[56], "Unsupported policy changed opaque attribute token");
   const auto output=session.address+57*8; session.put(output,0xfeedbeef00000000ULL);
   require(session.threads->AttributeGetPriority(r[55],output) == 0 &&
           session.get(output) == 0xfeedbeef0000012cULL,
           "Unsupported OTHER mutated stored priority or four-byte output span");
  }
 }
 std::cout << "PASS unsupported protection2/OTHER before mutation or completion\n";
}
void admission() {
 Cpu::Machine machine; auto threads = std::make_shared<Cpu::GuestThreads>(machine);
 Cpu::SceThreadImports imports(machine,threads);
 constexpr std::array nids{"nsYoNRywwNg","62KCwEMmzcM","DzES9hQF4f4","FXPWHNk8Of0","eXbUSpEaTsA","4+h9EzwKF4I"};
 for (unsigned i=0;i<nids.size();++i) {
  auto row = scoped(nids[i]); require(!imports.Resolve(row,2),"Legacy resolver admitted an opt-in priority family");
  require(imports.ResolvePriority(row,2).has_value(),"Explicit public priority provider rejected fixture contract");
  row.LibraryId=44;row.ModuleId=24;
  require(imports.ResolveTargetPriority(row,2).has_value(),"Target priority row rejected");
  // Import-table ids are per-image: any title, libc or module row is admitted.
  for (const auto ids:{std::pair{0,1},std::pair{4,5},std::pair{7,9}}) {
   auto other=row;other.LibraryId=ids.first;other.ModuleId=ids.second;
   require(imports.ResolveTargetPriority(other,2).has_value(),"Another image's priority import-table ids were refused");
  }
  for (const auto type:{0,1,6}) rejects([&]{imports.ResolveTargetPriority(row,type);},"scope/version/type");
  for (unsigned field=0;field<5;++field) {
   auto wrong=row;
   if(field==0)wrong.ModuleName="libc";
   if(field==1)wrong.LibraryName="libc";
   if(field==2)wrong.LibraryVersion=2;
   if(field==3)wrong.ModuleMajor=2;
   if(field==4)wrong.ModuleMinor=2;
   rejects([&]{imports.ResolveTargetPriority(wrong,2);},"scope/version/type");
  }
 }
 require(!imports.ResolveTargetPriority(scoped("AAAAAAAAAAA"),2),"Priority resolver fabricated another family");
 std::cout << "PASS title-agnostic priority scope/type and negative admission\n";
}
}
int main(int argc,char**argv) {
 try {
  require(argc==2,"Usage: KernelPriorityTest genuine-packaged.elf");
  completed(argv[1],0,{10,20,40,13},{0x1001,0x1003,0x1005});
  completed(argv[1],1,{10,30,20,40,13},{0x1001,0x1002,0x1003,0x1005});
  completed(argv[1],2,{10,11,20,40,13},{0x1001,0x1003,0x1005});
  completed(argv[1],3,{10,20,45,12,50,40,13},{0x1001,0x1003,0x1006,0x1005,0x1004});
  completed(argv[1],4,{10,20,50,40,13},{0x1001,0x1003,0x1004,0x1005});
  completed(argv[1],5,{40,10,20,13},{0x1001,0x1003,0x1005});
  completed(argv[1],8,{10,20,50,40,13},{0x1001,0x1003,0x1005,0x1004});
  runnableFifo(argv[1]);cancellation(argv[1],false);cancellation(argv[1],true);unsupported(argv[1]);admission();
  std::cout<<"PASS actual x86 SCE guest priority inheritance, transitivity, withdrawal, ABI and admission\n";
 } catch(const std::exception&error) {std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
}
