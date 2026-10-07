#import <AppKit/AppKit.h>
#include <cpu/NativeModuleRunner.hpp>
#include <cpu/GuestThreads.hpp>
#include <cpu/SceImports.hpp>
#include <cpu/SceNativeVideoOutBackend.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceTls.hpp>
#include "prx/libSceVideoOut/include/NativeMetalSession.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Authoring gate: actual production assembly is the owner. Component fixtures
// wire their own compositor/session and cannot catch an inactive/late runner,
// missing owner pump, disconnected Runtime, or wrong teardown order. No test
// production seam or private executable bytes are introduced. The explicitly
// synthetic existing VideoOut public route is never target qualification proof.
namespace {
constexpr auto rw = Cpu::Permission::Read | Cpu::Permission::Write;
constexpr auto rx = Cpu::Permission::Read | Cpu::Permission::Execute;
constexpr std::uint64_t Page=16384, Code=0x6000000, Data=0x6010000;
constexpr std::uint64_t RuntimeData=0x500000000, Display=0x500100000;
void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
template<class Action> void rejects(Action action,const char* expected) {
    try { action(); } catch(const std::exception& e) {
        require(std::string(e.what()).find(expected)!=std::string::npos,e.what()); return;
    }
    throw std::runtime_error(std::string("Missing assembly rejection: ")+expected);
}
std::uint64_t word(Cpu::Machine& m,std::uint64_t address) {
    std::uint64_t value=0; m.Read(address,std::as_writable_bytes(std::span(&value,1))); return value;
}
std::uint64_t object(const Cpu::SceModuleRecord& module,const char* nid) {
    for(const auto& e:module.Image.Exports) if(e.Identity.Nid==nid&&e.Type==1) return module.LoadBias+e.Value;
    throw std::runtime_error("Actual linked fixture object missing");
}
Cpu::SceImport scoped(const char* nid,const char* library) {
    Cpu::SceImport i; i.Nid=nid; i.LibraryName=i.ModuleName=library;
    i.LibraryVersion=i.ModuleMajor=i.ModuleMinor=1; i.LibraryId=39; i.ModuleId=40; return i;
}
Cpu::SceImportConsumer consumer(const Cpu::SceParsedImage& p) { return {p.Path,p.SourceSize,p.SourceSha256}; }
void nativeWord(Cpu::NativeModuleRunner& runner,std::uint64_t address,std::uint64_t expected) {
    const std::array spans{AgcDriver::Metal::ReadableGuestRange{address,8}};
    runner.Graphics().Driver().WithValidatedReadableRanges(spans,[&] {
        const auto bytes=AgcDriver::NativeGuestMemory::ContiguousBorrowedRange(address,8);
        std::uint64_t actual=0;std::memcpy(&actual,bytes.data(),8);
        require(actual==expected,"Actual assembled native binding lost translated/Runtime bytes");
    });
}
NSWindow* window(const std::string& title) {
    for(NSWindow* w in NSApp.windows) if([w.title isEqualToString:[NSString stringWithUTF8String:title.c_str()]]) return w;
    throw std::runtime_error("Assembly fixture window missing");
}
void postKey(NSWindow* w,bool down) {
    NSEvent* event=[NSEvent keyEventWithType:(down?NSEventTypeKeyDown:NSEventTypeKeyUp)
        location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:w.windowNumber
        context:nil characters:@"a" charactersIgnoringModifiers:@"a" isARepeat:NO keyCode:0];
    require(event!=nil,"Cannot enqueue actual AppKit fixture key event");
    [NSApp postEvent:event atStart:NO];
}
void assertPumped(Cpu::NativeModuleRunner& runner,const char* phase,bool down) {
    const auto batch=runner.Graphics().Window().DrainEvents();
    require(std::any_of(batch.events.begin(),batch.events.end(),[down](const auto& event) {
        const auto key=std::get_if<::KeyboardInputEvent>(&event.payload);
        return key&&key->keyCode==0x04&&key->pressed==down&&!key->resetKeys&&!key->connectionChange;
    }),(std::string("Assembly owner boundary failed to pump posted AppKit input in actual guest phase: ")+phase).c_str());
}
// Explicit public x86 machine-code producer, using the production thread gates.
// Child creation allocates its genuine owned stack/errno/TLS through the runner.
struct X86 {
    std::vector<std::uint8_t> bytes;
    void op(std::initializer_list<std::uint8_t> values){bytes.insert(bytes.end(),values);}
    void imm(std::uint64_t value){for(unsigned j=0;j<8;++j)bytes.push_back(value>>(j*8));}
    void mov(unsigned opcode,std::uint64_t value){op({0x48,static_cast<std::uint8_t>(opcode)});imm(value);}
    void call(std::uint64_t gate){mov(0xb8,gate);op({0xff,0xd0});}
};
void childAssembly(Cpu::Machine& machine,std::shared_ptr<Cpu::GuestThreads> threads,
                   Cpu::NativeModuleRunner& runner,const Cpu::SceImportConsumer& source,
                   std::uint64_t tlsOffset,std::int64_t tlsDisplacement) {
    const auto create=runner.Resolve(source,scoped("6UgtwV+0zb4","libkernel"),2,0);
    const auto join=runner.Resolve(source,scoped("onNY9Byn-W8","libkernel"),2,0);
    require(create&&join,"Native assembly omitted production ordinary thread provider");
    constexpr std::uint64_t Child=Code+512, Observe=Code+1024, Return=Code+2048;
    auto childVisits=std::make_shared<unsigned>(0);
    auto& driver=runner.Graphics().Driver();
    machine.AddHostCall(Observe,[weak=std::weak_ptr<Cpu::GuestThreads>(threads),&runner,&driver,childVisits,tlsOffset](Cpu::Machine& m) {
        const auto threads=weak.lock();require(bool(threads),"Actual child scheduler expired");
        require(threads->ActiveTls()!=nullptr,"Actual created child omitted frozen static TLS");
        const auto tls=threads->ActiveTls()->Allocation();
        const auto stack=m.Get(Cpu::Register::Rsp);
        const std::array spans{AgcDriver::Metal::ReadableGuestRange{tls.Address,tls.Size},
                              AgcDriver::Metal::ReadableGuestRange{stack,8}};
        driver.WithValidatedReadableRanges(spans,[&] {
            const auto bytes=AgcDriver::NativeGuestMemory::ContiguousBorrowedRange(tls.Address,tls.Size);
            require(bytes.size()==tls.Size,"Actual child TLS is absent from native mapping table");
            const auto tlsWord=threads->ActiveTls()->Resolve(1,tlsOffset);
            require(word(m,tlsWord)==0x2468,"Actual child FS store did not update its own TLS");
            std::uint64_t native=0;std::memcpy(&native,bytes.data()+tlsWord-tls.Address,8);
            require(native==0x2468,"Native child TLS lost its independent translated FS store");
            rejects([&]{AgcDriver::NativeGuestMemory::ContiguousBorrowedRange(tlsWord,8,true);},
                "read-only"); // High CPU TLS stays readable; written-page ABI cannot represent GPU writes.
            const auto returnBytes=AgcDriver::NativeGuestMemory::ContiguousBorrowedRange(stack,8);
            std::uint64_t pc=0;std::memcpy(&pc,returnBytes.data(),8);
            require(pc==Child+29,"Actual newly owned child stack lacks its translated CALL return word");
        });
        require(runner.MappingGeneration()>1,"Actual child allocation bypassed live native publication");
        ++*childVisits;
    });
    X86 child;child.op({0x48,0x83,0xec,0x08});child.op({0x64,0x48,0xc7,0x04,0x25});
    for(unsigned i=0;i<4;++i)child.bytes.push_back(static_cast<std::uint64_t>(tlsDisplacement)>>(i*8));
    child.op({0x68,0x24,0,0});child.call(Observe);
    child.op({0x48,0x83,0xc4,0x08});child.mov(0xb8,0x13579bdf);child.op({0xc3});
    machine.Write(Child,std::as_bytes(std::span(child.bytes)));
    X86 caller;caller.op({0x48,0x83,0xec,0x08});caller.mov(0xbf,Data);
    caller.op({0x48,0x31,0xf6});caller.mov(0xba,Child);caller.mov(0xb9,0);
    caller.op({0x45,0x31,0xc0});caller.call(create->Address);
    caller.mov(0xb8,Data);caller.op({0x48,0x8b,0x38});caller.mov(0xbe,Data+8);caller.call(join->Address);
    caller.op({0x48,0x83,0xc4,0x08,0x31,0xc0,0xc3});
    machine.Write(Code,std::as_bytes(std::span(caller.bytes)));
    Cpu::GuestPhaseBudget budget(100000);
    const auto result=threads->InvokeModule({Cpu::GuestModuleCallKind::Initialize,Code,Return,{}},budget);
    require(result.Reason==Cpu::StopReason::Address&&result.ReturnValue==0&&*childVisits==1&&
        word(machine,Data)!=0&&word(machine,Data+8)==0x13579bdf,
        "Real assembled scheduler did not create/execute/join child through native-published stack/TLS");
}
void idleAssembly(Cpu::Machine& machine,std::shared_ptr<Cpu::GuestThreads> threads,
                  Cpu::NativeModuleRunner& runner,NSWindow* fixtureWindow) {
    // Synthetic public provider domain. Its wake is gated by actual delivered
    // AppKit input after the translated caller parked, not a fake EOP receipt.
    struct State { Cpu::GuestThreadHandle caller=0;unsigned delivered=0; };
    auto state=std::make_shared<State>();
    auto domain=std::make_shared<Cpu::GuestThreads::WaitDomain>(threads->CreateWaitDomain(machine,[](auto){}));
    domain->SetOwnerPump([weak=std::weak_ptr<Cpu::GuestThreads::WaitDomain>(domain),state,&runner] {
        const auto domain=weak.lock();if(!domain||!state->caller||!domain->IsWaiting(state->caller,71))return false;
        const auto batch=runner.Graphics().Window().DrainEvents();
        const bool delivered=std::any_of(batch.events.begin(),batch.events.end(),[](const auto& event) {
            const auto key=std::get_if<::KeyboardInputEvent>(&event.payload);
            return key&&key->keyCode==0x04&&key->pressed&&!key->resetKeys&&!key->connectionChange;
        });
        if(delivered) {++state->delivered;require(domain->Wake(state->caller,71,0x2468),"Idle input wake lost exact suspended call");}
        return true;
    });
    constexpr auto WaitGate=Code+1280,Caller=Code+256;
    machine.AddHostCall(WaitGate,[domain,state,fixtureWindow](Cpu::Machine&) {
        state->caller=domain->ActiveThread();domain->BlockFromHostCall(71);postKey(fixtureWindow,true);
    });
    X86 caller;caller.op({0x48,0x83,0xec,8});caller.call(WaitGate);caller.op({0x48,0x83,0xc4,8,0xc3});
    machine.Write(Caller,std::as_bytes(std::span(caller.bytes)));
    runner.Graphics().Window().DrainEvents();
    Cpu::GuestPhaseBudget budget(1000);
    const auto result=threads->InvokeModule({Cpu::GuestModuleCallKind::Initialize,Caller,Code+2048,{}},budget);
    require(result.Reason==Cpu::StopReason::Address&&result.ReturnValue==0x2468&&state->delivered==1,
        "Actual native idle owner failed to pump AppKit and resume the parked translated call");
    domain->Withdraw();
}
void compute(Cpu::Machine& machine,Cpu::NativeModuleRunner& runner) {
    constexpr std::array<std::uint32_t,24> program{
        0x34020084,0xd765000a,0x000100c1,0x3604009f,0x7d880488,0xbe880f6a,0x7e160208,0xd7600009,
        0x00010700,0x7e180209,0xbe9e037e,0x7e1a0280,0x7da80488,0x7e1a0281,0xbefe031e,0xe0701000,
        0x80010a01,0xe0701004,0x80010b01,0xe0701008,0x80010c01,0xe070100c,0x80010d01,0xbf810000};
    const auto code=Code+4096,headerAddress=Code+8192;
    machine.Write(code,std::as_bytes(std::span(program)));
    std::array<std::byte,sizeof(Shader)+sizeof(ShaderUserData)> headerBytes{};
    Shader shader{};shader.file_header=0x34333231;shader.version=0x18;
    shader.code=reinterpret_cast<const volatile void*>(code);
    shader.user_data=reinterpret_cast<ShaderUserData*>(headerAddress+sizeof(Shader));
    shader.header_size=headerBytes.size();shader.shader_size=sizeof(program);
    std::memcpy(headerBytes.data(),&shader,sizeof(shader));machine.Write(headerAddress,headerBytes);
    runner.Graphics().Driver().RegisterShader(reinterpret_cast<const Shader*>(headerAddress));
    std::vector<std::uint32_t> packets;
    const auto registers=[&](std::uint32_t first,auto values) {
        packets.push_back(0xc0007600u|(static_cast<std::uint32_t>(values.size())<<16));
        packets.push_back(first);packets.insert(packets.end(),values.begin(),values.end());
    };
    registers(0x207,std::array<std::uint32_t,3>{64,1,1});
    registers(0x20c,std::array<std::uint32_t,2>{static_cast<std::uint32_t>(code>>8),static_cast<std::uint32_t>(code>>40)});
    registers(0x213,std::array<std::uint32_t,1>{16});
    const auto output=RuntimeData+4096;
    registers(0x240,std::array<std::uint32_t,8>{0,0,0,0,static_cast<std::uint32_t>(output),
        static_cast<std::uint32_t>((output>>32)&0xffff)|(4u<<16),256,0x01016fac});
    const std::array<std::uint32_t,5> dispatch{0xc0031500,1,1,1,0x8041};
    packets.insert(packets.end(),dispatch.begin(),dispatch.end());
    machine.Write(Data+4096,std::as_bytes(std::span(packets)));
    runner.Graphics().Driver().SubmitCommandBuffer(Data+4096,packets.size(),0,0);
    runner.Graphics().Driver().WaitIdle();
    std::array<std::byte,Page> actual{};machine.Read(RuntimeData,actual);
    for(unsigned tid=0;tid<64;++tid) {
        const unsigned lane=tid%32;
        const std::array<std::uint32_t,4> golden{lane,8,tid-lane+3,lane<8?1u:0u};
        for(unsigned column=0;column<4;++column) {
            std::uint32_t value=0;std::memcpy(&value,actual.data()+4096+(tid*4+column)*4,4);
            require(value==golden[column],"Assembled native Metal output lost independent lane/count/readlane/EXEC golden");
        }
    }
    for(std::size_t i=0;i<actual.size();++i)if(i<4096||i>=5120)
        require(actual[i]==std::byte{0xa5},"Assembled native dispatch changed shared Runtime byte guards");
}
void qualifiedRejections(const char* utility) {
    // Recorded target certificate only. No private bytes are mapped or executed.
    // This synthetic metadata boundary proves strict rejection, not the target
    // caller's runtime behavior or a positive public-image target admission.
    Cpu::SceImportConsumer source;source.Path="eboot.bin";source.SourceSize=102560655;
    constexpr char hash[]="a6df51ec222136f337f86e9be5fa3013417ddc44bc22a6c8d514c0199cf8c397";
    const auto digit=[](char c){return c<='9'?c-'0':c-'a'+10;};
    for(unsigned i=0;i<32;++i)source.SourceSha256[i]=static_cast<std::byte>((digit(hash[i*2])<<4)|digit(hash[i*2+1]));
    Cpu::Machine machine;auto threads=std::make_shared<Cpu::GuestThreads>(machine);
    Cpu::NativeModuleRunnerConfiguration config;config.UtilityMetallib=utility;
    config.WindowTitle="Native runner static certificate rejection fixture";config.Width=config.Height=64;
    Cpu::NativeModuleRunner runner(machine,threads,source,config);
    auto import=scoped("Up36PTk687E","libSceVideoOut");
    rejects([&]{runner.Resolve(source,import,1,0);},"ELF symbol");
    rejects([&]{runner.Resolve(source,import,2,8);},"ELF symbol");
    auto wrong=source;wrong.SourceSha256[0]^=std::byte{1};
    rejects([&]{runner.Resolve(wrong,import,2,0);},"actual consumer source");
    wrong=source;--wrong.SourceSize;
    rejects([&]{runner.Resolve(wrong,import,2,0);},"actual consumer source");
    wrong=source;wrong.Path="public-fixture.elf";
    rejects([&]{runner.Resolve(wrong,import,2,0);},"actual consumer source");
    auto badScope=import;badScope.LibraryId=0;
    rejects([&]{runner.Resolve(source,badScope,2,0);},"scope/version");
    auto unknown=import;unknown.Nid="not-a-real-nid";
    rejects([&]{runner.Resolve(source,unknown,2,0);},"service");
    // CPU12's POSIX pair is size-zero in the actual pinned libc image. The
    // aggregate resolver must also reject fabricated nonzero-size metadata,
    // because the component's legacy mutex resolver accepts only a type.
    auto libc=source;libc.Path="libc.prx";libc.SourceSize=1875018;
    constexpr char libcHash[]="78a080fdeccc28f2aa76356e97f82a35b3ba09deba8408dfce27db28fa0ce67f";
    for(unsigned i=0;i<32;++i)libc.SourceSha256[i]=static_cast<std::byte>((digit(libcHash[i*2])<<4)|digit(libcHash[i*2+1]));
    for(const auto nid:{"7H0iTOciTLo","2Z+PpY6CaJg"}) {
        auto mutex=scoped(nid,"libkernel");mutex.LibraryId=0;mutex.ModuleId=1;
        rejects([&]{runner.Resolve(libc,mutex,1,0);},"scope/version/type");
        rejects([&]{runner.Resolve(libc,mutex,2,8);},"scope/version/type");
        auto malformed=libc;--malformed.SourceSize;
        rejects([&]{runner.Resolve(malformed,mutex,2,0);},"consumer source/import");
        malformed=libc;malformed.SourceSha256[0]^=std::byte{1};
        rejects([&]{runner.Resolve(malformed,mutex,2,0);},"consumer source/import");
    }
    runner.Shutdown();runner.Shutdown();
}
void run(const char* mainPath,const char* dependencyPath,const char* utility,const char* mode) {
    Cpu::Machine machine;
    auto threads=std::make_shared<Cpu::GuestThreads>(machine);
    const auto parsed=Cpu::ParseSce(mainPath);const auto source=consumer(parsed);
    Cpu::NativeModuleRunnerConfiguration config;config.UtilityMetallib=utility;
    config.WindowTitle="Native module assembly public fixture";config.Width=config.Height=64;
    Cpu::NativeModuleRunner runner(machine,threads,source,config);
    require(runner.MappingGeneration()==0&&runner.Memory()->Snapshot().Generation==0,
        "Native assembly was published before graph/entry construction");
    rejects([&]{runner.Memory()->MapFlexible(RuntimeData,Page,0x33,0x90);},"before initial publication");
    Cpu::SceImports imports(machine);
    imports.SetProcessExitHandler([threads](int status){threads->ProcessExitFromHostCall(status);});
    const std::array deps{Cpu::SceModuleFile{dependencyPath,0x2000000}};
    const std::array hosts{Cpu::SceHostModule{"libc.prx",{"libc",0,1,1},{{"libc",0,1}}}};
    Cpu::SceModules graph(machine,{mainPath,0x1000000},deps,hosts,{},std::nullopt,
        [&](const auto& actual,const auto& import,std::uint8_t type,std::uint64_t size)->std::optional<Cpu::SceResolvedImport>{
            if(auto resolved=runner.Resolve(actual,import,type,size))return resolved;
            if(type!=2||size)return std::nullopt;
            return Cpu::SceResolvedImport{imports.Resolve(import),2};
        });
    machine.Map(Code,Page,rx);machine.Map(Data,Page,rw);
    Cpu::SetupSceEntry(machine,graph.Main(),{"native-fixture","17","5","7","58","3366582378"},graph.EntryTerminationGate());
    threads->AdoptInitial({graph.Main().Entry,graph.InitialStack(),graph.Tls(),graph.ThreadTlsFactory()});
    auto executor=threads->ModuleExecutor();
    unsigned initialized=0,finalized=0,entered=0;
    NSWindow* fixtureWindow=window(config.WindowTitle);
    const auto invoke=executor.Invoke;
    executor.Invoke=[&](const Cpu::GuestModuleCall& call,Cpu::GuestPhaseBudget& budget) {
        require(runner.MappingGeneration()>=1,"Actual initializer/finalizer preceded native initial publication");
        runner.Graphics().Window().DrainEvents();postKey(fixtureWindow,false);
        const auto result=invoke(call,budget);assertPumped(runner,call.Kind==Cpu::GuestModuleCallKind::Initialize?"initialize":"finalize",false);
        if(call.Kind==Cpu::GuestModuleCallKind::Initialize)++initialized;else++finalized;
        return result;
    };
    const auto entry=executor.RunEntry;
    executor.RunEntry=[&](Cpu::GuestPhaseBudget& budget) {
        const bool terminal=threads->PendingEntryControl().Kind!=Cpu::GuestEntryControlKind::None;
        const bool closing=std::string(mode)=="close";
        if(!entered&&!closing) {runner.Graphics().Window().DrainEvents();postKey(fixtureWindow,true);}
        const auto result=entry(budget);
        if(!entered&&!closing)assertPumped(runner,"entry",true);
        if(!terminal)++entered;
        return result;
    };
    graph.SetExecutor(std::move(executor));runner.ActivateBeforeInitializers();
    require(runner.MappingGeneration()==1,"Initial native assembly publication is not generation1");
    nativeWord(runner,object(graph.Modules()[1],"L+OvOB7GHzo"),0x10203040);
    rejects([&]{runner.ActivateBeforeInitializers();},"only once");
    rejects([&]{runner.Resolve(source,scoped("Up36PTk687E","libSceVideoOut"),2,0);},"actual consumer source");
    require(!runner.Resolve(source,scoped("not-a-real-nid","unclaimed-provider"),2,0),"Unclaimed native import invented a provider");
    const auto tlsExport=std::find_if(graph.Modules()[0].Image.Exports.begin(),graph.Modules()[0].Image.Exports.end(),[](const auto& e){
        return e.Identity.Nid=="rBiIvjUtJbs"&&e.Type==6;
    });
    require(tlsExport!=graph.Modules()[0].Image.Exports.end(),"Actual linked main TLS export missing");
    childAssembly(machine,threads,runner,source,tlsExport->Value,graph.Tls()->Tpoff(1,tlsExport->Value));
    idleAssembly(machine,threads,runner,fixtureWindow);
    require(runner.Memory()->MapFlexible(RuntimeData,Page,0x33,0x90)==RuntimeData,"Assembly shared Runtime mapping failed");
    std::array<std::byte,Page> guards;guards.fill(std::byte{0xa5});machine.Write(RuntimeData,guards);
    compute(machine,runner);
    if(std::string(mode)=="initializer-failure") {
        constexpr std::array<std::uint8_t,6> failCode{0xb8,7,0,0,0,0xc3};
        machine.Write(graph.Modules()[1].Init,std::as_bytes(std::span(failCode)));
        rejects([&]{graph.InitializeDependencies();},"failure status");
        require(initialized==1&&entered==0,"Failed actual initializer continued into guest entry");
        runner.Shutdown();runner.Shutdown();
        require(runner.Memory()->Snapshot().Views.empty(),"Initializer failure retained shared Runtime after native drain");
        rejects([&]{runner.Graphics();},"drained");
        rejects([&]{runner.Resolve(source,scoped("unknown-ended-import","unclaimed-provider"),2,0);},"Native module import resolution after shutdown");return;
    }
    graph.InitializeDependencies();
    require(initialized==1,"Actual graph did not run one dependency initializer");
    nativeWord(runner,object(graph.Modules()[1],"L+OvOB7GHzo"),0x10203041);
    // Existing public route is synthetic execution coverage of registered
    // native-buffer teardown; production Resolve correctly rejects this source.
    machine.Map(Display,Page,rw);
    const auto call=[&](const char* nid,std::array<std::uint64_t,6> arguments) {
        const auto gate=runner.Graphics().ResolveVideoOutPublicFixture(scoped(nid,"libSceVideoOut"));
        X86 code;code.op({0x48,0x83,0xec,8});code.call(gate);code.op({0x48,0x83,0xc4,8,0xc3});
        machine.Write(Code,std::as_bytes(std::span(code.bytes)));
        Cpu::GuestPhaseBudget budget(1000);
        const auto result=threads->InvokeModule({Cpu::GuestModuleCallKind::Initialize,Code,Code+2048,
            {arguments[0],arguments[1],arguments[2]}},budget);
        // Register calls with <=3 args use module-call ABI. Attribute/register
        // calls use an x86 producer below, preserving the production scheduler.
        require(result.Reason==Cpu::StopReason::Address&&result.ReturnValue,"Synthetic public VideoOut invocation failed");
        return *result.ReturnValue;
    };
    const auto handle=call("Up36PTk687E",{255,0,0,0,0,0});require(handle==1,"Synthetic actual output open failed");
    const auto attrGate=runner.Graphics().ResolveVideoOutPublicFixture(scoped("PjS5uASwcV8","libSceVideoOut"));
    const auto registerGate=runner.Graphics().ResolveVideoOutPublicFixture(scoped("rKBUtgRrtbk","libSceVideoOut"));
    const std::uint64_t buffer=Display;machine.Write(Data+512,std::as_bytes(std::span(&buffer,1)));
    X86 registration;registration.op({0x48,0x83,0xec,24,0x48,0xc7,0x04,0x24,0,0,0,0,0x48,0xc7,0x44,0x24,8,0,0,0,0});
    registration.mov(0xbf,Data+256);registration.mov(0xbe,0x8000000000000000ULL);registration.mov(0xba,1);
    registration.mov(0xb9,64);registration.op({0x41,0xb8,64,0,0,0,0x45,0x31,0xc9});registration.call(attrGate);
    registration.mov(0xbf,handle);registration.mov(0xbe,0);registration.mov(0xba,0);
    registration.mov(0xb9,Data+512);registration.op({0x41,0xb8,1,0,0,0});
    registration.op({0x49,0xb9});registration.imm(Data+256);registration.call(registerGate);
    registration.op({0x48,0x83,0xc4,24,0xc3});machine.Write(Code,std::as_bytes(std::span(registration.bytes)));
    Cpu::GuestPhaseBudget registrationBudget(2000);
    const auto registered=threads->InvokeModule({Cpu::GuestModuleCallKind::Initialize,Code,Code+2048,{}},registrationBudget);
    require(registered.Reason==Cpu::StopReason::Address&&registered.ReturnValue==0,"Synthetic native buffer registration failed");
    rejects([&]{machine.Unmap(Display,Page);},"registered");
    if(std::string(mode)=="close") {
        const auto state=object(graph.Modules()[0],"uhHbg8dKn0c");
        std::array<std::uint64_t,11> before{},after{};
        machine.Read(state,std::as_writable_bytes(std::span(before)));
        runner.Graphics().Window().RequestCloseMainThread();
        require(graph.RunMain()==Cpu::StopReason::Requested,"Owner boundary ignored actual native window close");
        machine.Read(state,std::as_writable_bytes(std::span(after)));
        require(before==after&&finalized==0,"Closed window continued main initializer/entry or finalization");
    } else {
        require(graph.RunMain()==Cpu::StopReason::Exit&&machine.ExitCode()==0&&finalized==1&&entered>=1,
            "Actual assembled init/entry/fini did not preserve public independent arithmetic exit");
        const auto state=object(graph.Modules()[0],"uhHbg8dKn0c");
        constexpr std::array<std::uint64_t,11> golden{1,1,12,7,58,3366582378,0x22334462,0x31415938,0x10203052,63,12};
        for(unsigned j=0;j<golden.size();++j)nativeWord(runner,state+j*8,golden[j]);
        nativeWord(runner,object(graph.Modules()[1],"A+KZd7Qeu3w"),1);
        nativeWord(runner,object(graph.Modules()[1],"h-nyiHettao"),123);
    }
    runner.Shutdown();runner.Shutdown();
    require(!fixtureWindow.visible&&runner.Memory()->Snapshot().Views.empty(),"Assembly did not close window/drain graphics before Runtime cleanup");
    rejects([&]{runner.Graphics();},"drained");
        rejects([&]{runner.Resolve(source,scoped("unknown-ended-import","unclaimed-provider"),2,0);},"Native module import resolution after shutdown");
    machine.Unmap(Display,Page); // Registered output must have retired before hook removal.
}
}
int main(int argc,char** argv) {
    @autoreleasepool {try {
        require(argc==5,"Usage: NativeModuleRunnerTest main.elf dependency.prx utility.metallib lifecycle|close|initializer-failure");
        if(std::string(argv[4])=="qualified-rejection") qualifiedRejections(argv[3]);
        else run(argv[1],argv[2],argv[3],argv[4]);
        std::cout<<"Actual native runner public assembly "<<argv[4]<<" PASS; synthetic public VideoOut route is not target admission/gameplay evidence\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
}
