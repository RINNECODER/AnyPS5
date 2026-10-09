#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include "NativeAgcBackend.hpp"
#include "KernelEvents.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceAgcImports.hpp>
#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceThreadImports.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>

// Test-audit: KernelEventsNativeTest adds its event directly from the host and
// therefore cannot catch the absent target AddEqEvent admission. This fixture
// owns translated target AGC admission -> numeric queue -> native EOP -> idle
// scheduler delivery. General queue scheduling remains owned by CPU09 controls.
// Profile selection is title-agnostic; the executable under test is an
// independently authored public guest, never a private title file.
namespace {
constexpr std::uint64_t Bias=0x100000000, PageAddress=0x600000000,
    Output=PageAddress+32, Count=PageAddress+128, Canary=0x1badc0ffeef00d55ULL;
using State=std::array<std::uint64_t,64>;
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F action,std::string_view reason){
    try{action();}catch(const std::exception& error){
        require(std::string(error.what()).find(reason)!=std::string::npos,error.what());return;
    }
    throw std::runtime_error("Expected boundary rejection: "+std::string(reason));
}
Cpu::SceImport graphicsRow(std::string_view nid){
    Cpu::SceImport row;row.Nid=nid;row.LibraryName=row.ModuleName="libSceAgcDriver";
    row.LibraryVersion=row.ModuleMajor=row.ModuleMinor=1;row.LibraryId=row.ModuleId=2;return row;
}
struct Calls {
    unsigned adds=0,deletes=0;
    std::uint64_t handle=0,user=0;
    std::int32_t id=0;
};
struct Session {
    Cpu::Machine machine;
    std::shared_ptr<Cpu::GuestThreads> threads=std::make_shared<Cpu::GuestThreads>(machine);
    Cpu::SceThreadImports threadImports{machine,threads};
    Cpu::SceLifecycleImports lifecycle{machine};
    std::unique_ptr<Cpu::Platform::KernelEvents> queues;
    std::unique_ptr<Cpu::SceAgcImports> graphics;
    std::unique_ptr<Cpu::SceModules> graph;
    alignas(65536) std::array<std::byte,65536> page{};
    Calls calls;
    std::uint64_t address=0;
    unsigned idleBoundaries=0;
    const std::thread::id owner=std::this_thread::get_id();
    std::function<void(bool)> boundary;
    Session(const char* file,unsigned mode,AgcDriver::Metal::MetalDriver& driver,
            std::chrono::milliseconds maximumIdleWait=std::chrono::milliseconds(500)){
        require(std::string(Cpu::Machine::Backend()).find("Modern QEMU TCG")!=std::string::npos,
                "Target event fixture requires actual Modern QEMU TCG");
        page.fill(std::byte{0xa5});
        machine.MapBorrowed(PageAddress,std::span(page).first(4096),Cpu::Permission::Read|Cpu::Permission::Write,page);
        queues=std::make_unique<Cpu::Platform::KernelEvents>(machine,threads);
        lifecycle.SetProcessExitHandler([runtime=std::weak_ptr<Cpu::GuestThreads>(threads)](int code){
            auto owner=runtime.lock();require(bool(owner),"Target event scheduler expired");owner->ProcessExitFromHostCall(code);
        });
        auto backend=Cpu::MakeNativeAgcBackend(driver);
        backend.AddEvent=[this](std::uint64_t handle,std::int32_t id,std::uint64_t user){
            ++calls.adds;calls.handle=handle;calls.id=id;calls.user=user;
            return queues->AddGraphicsEvent(handle,id,user);
        };
        backend.DeleteEvent=[this](std::uint64_t handle,std::int32_t id){
            ++calls.deletes;calls.handle=handle;calls.id=id;
            return queues->DeleteGraphicsEvent(handle,id);
        };
        if(mode==10){
            constexpr std::array source{Cpu::AgcAbiContract::AddEqEvent,Cpu::AgcAbiContract::DeleteEqEvent};
            graphics=std::make_unique<Cpu::SceAgcImports>(machine,std::move(backend),source);
        }else{
            graphics=std::make_unique<Cpu::SceAgcImports>(machine,std::move(backend),Cpu::TargetAgcAdmissions());
        }
        const std::array hosts{
            Cpu::SceHostModule{"libkernel.prx",{"libkernel",0,1,1},{{"libkernel",0,1}}},
            Cpu::SceHostModule{"libSceAgcDriver.prx",{"libSceAgcDriver",0,1,1},{{"libSceAgcDriver",0,1}}}};
        graph=std::make_unique<Cpu::SceModules>(machine,Cpu::SceModuleFile{file,Bias},
            std::span<const Cpu::SceModuleFile>{},hosts,
            [&](const auto& row,std::uint8_t type)->std::optional<Cpu::SceResolvedImport>{
                require(type==2,"Target event fixture lost linked STT_FUNC");
                if(row.ModuleName=="libSceAgcDriver")return Cpu::SceResolvedImport{graphics->Resolve(row,type,0),type};
                if(const auto gate=queues->Resolve(row,type))return Cpu::SceResolvedImport{*gate,type};
                if(const auto gate=threadImports.Resolve(row,type))return Cpu::SceResolvedImport{*gate,type};
                if(const auto gate=lifecycle.Resolve(row))return Cpu::SceResolvedImport{*gate,type};
                return std::nullopt;
            });
        const auto& image=graph->Modules()[0].Image;
        require(image.Imports.size()==7 && image.NeededFiles.empty() && image.Tls,
                "Public target event guest import/TLS graph differs");
        require(std::set<std::uint32_t>(image.RelocationTypes.begin(),image.RelocationTypes.end())==std::set<std::uint32_t>{7,8},
                "Public target event guest lost genuine PLT/RELATIVE relocations");
        for(const auto& row:image.Exports)if(row.Type==1 && row.Size==sizeof(State)){
            require(!address,"Ambiguous target event receipt");address=Bias+row.Value;
        }
        require(address && state()==State{},"Public target event loading executed guest or lost receipt");
        put(1,mode);put(6,Output);put(7,Count);put(8,Output);
        put(16,PageAddress+4080);put(17,PageAddress+4094);put(18,PageAddress+4094);
        put(19,mode==2?1:mode==10?static_cast<std::uint32_t>(-7):0);
        put(20,mode==3||mode==10?0x8877665544332211ULL:0);
        put(21,mode==5?1:mode==10?static_cast<std::uint32_t>(-7):0);
        put(32,0xfeedbeef00000000ULL);put(60,Canary);put(61,~Canary);
        Cpu::SetupSceEntry(machine,graph->Main(),{"public-target-agc-events"},graph->EntryTerminationGate());
        threads->AdoptInitial({graph->Main().Entry,graph->InitialStack(),graph->Tls(),graph->ThreadTlsFactory()});
        threads->SetOwnerBoundary([this](bool waiting){
            threads->CheckIdleOwner();require(std::this_thread::get_id()==owner,"Target event guest owner migrated");
            idleBoundaries+=waiting;if(boundary)boundary(waiting);
        },maximumIdleWait);
        graph->SetExecutor(threads->ModuleExecutor());graph->InitializeDependencies();
    }
    ~Session(){graphics.reset();queues.reset();threads->Withdraw();}
    State state()const{State result{};machine.Read(address,std::as_writable_bytes(std::span(result)));return result;}
    void put(unsigned slot,std::uint64_t value){machine.Write(address+slot*8,std::as_bytes(std::span(&value,1)));}
    void guards()const{
        for(unsigned i=0;i<page.size();++i)
            if(!(i>=32 && i<64) && !(i>=128 && i<132))require(page[i]==std::byte{0xa5},"Event32/count4 overran independent redzone");
        const auto r=state();require(r[32]==0xfeedbeef00000000ULL && r[60]==Canary && r[61]==~Canary,
                                  "Immutable timeout4 or guest receipt guard changed");
    }
    void finish(){
        require(graph->RunMain(1000000,10000)==Cpu::StopReason::Exit && machine.ExitCode()==0,
                "Translated target event guest failed its finite progress limit");
        require(state()[0]==0x41474345564e5430ULL,"Translated target event guest missed final receipt");guards();
    }
};
// Bytes are independently specified, without sizeof/serialization of the
// production KernelEventRecord. Ident0/filter-14/flags0x21/fflags0/data1/udata0.
constexpr std::array<std::byte,32> Golden{
    std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},
    std::byte{0xf2},std::byte{0xff},std::byte{0x21},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},
    std::byte{1},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},
    std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{0}};
struct NativeEop {
    static constexpr std::uint64_t CodeAddress=0x520100,HeaderAddress=0x530100,ComputeOutput=0x510100;
    // Existing public RDNA producer from NativeAgcFixture/Pm4ComputeReplay.
    // The numerical oracle below is specified from subgroup semantics, not
    // from a decoder, translated shader, driver or observed output.
    static constexpr std::array<std::uint32_t,24> Wave32Code{
        0x34020084,0xd765000a,0x000100c1,0x3604009f,0x7d880488,0xbe880f6a,0x7e160208,0xd7600009,
        0x00010700,0x7e180209,0xbe9e037e,0x7e1a0280,0x7da80488,0x7e1a0281,0xbefe031e,0xe0701000,
        0x80010a01,0xe0701004,0x80010b01,0xe0701008,0x80010c01,0xe070100c,0x80010d01,0xbf810000};
    static constexpr std::array<std::uint32_t,35> ComputeCommands{
        0xc0037600,0x207,64,1,1,
        0xc0027600,0x20c,static_cast<std::uint32_t>(CodeAddress>>8),0,
        0xc0017600,0x213,16,
        0xc0087600,0x240,0,0,0,0,static_cast<std::uint32_t>(ComputeOutput),4u<<16,256,0x11016fac,
        0xc0031500,1,1,1,0x8041,
        0xc0064900,0,(1u<<29)|(1u<<24),0x510f00,0,0xabcdef12,0,0};
    AgcDriver::Metal::MetalDriver& driver;
    Session& session;
    alignas(65536) std::array<std::uint32_t,16384> commands{},output{};
    alignas(65536) std::array<std::byte,65536> code{},header{};
    std::array<std::byte,sizeof(Shader)+sizeof(ShaderUserData)> publishedHeader{};
    std::array<std::byte,65536> pageBeforeCompute{};
    bool computeSubmitted=false;
    std::atomic<unsigned> callbacks{0};
    std::atomic<bool> producerOwner{false},wrongId{false},producerGuestChanged{false};
    NativeEop(id<MTLDevice> device,id<MTLLibrary> library,AgcDriver::Metal::MetalDriver& d,Session& s):driver(d),session(s){
        output.fill(0xdeadbeef);commands.fill(0xdeadbeef);code.fill(std::byte{0x7b});header.fill(std::byte{0x7b});
        std::memcpy(code.data()+256,Wave32Code.data(),sizeof(Wave32Code));
        // Fixed public header layout, encoded with numeric guest addresses.
        // The zero-filled ShaderUserData is wholly contained in this header.
        static_assert(sizeof(Shader)==96 && sizeof(ShaderUserData)==56);
        const auto field=[&](std::size_t offset,auto value){std::memcpy(publishedHeader.data()+offset,&value,sizeof(value));};
        field(0,std::uint32_t{0x34333231});field(4,std::uint32_t{0x18});
        field(8,HeaderAddress+96);field(16,CodeAddress);
        field(64,static_cast<std::uint32_t>(publishedHeader.size()));
        field(68,static_cast<std::uint32_t>(sizeof(Wave32Code)));field(92,std::uint8_t{0}); // Registered compute type, not relative input type2.
        const std::array ranges{
            AgcDriver::NativeGuestMemory::BorrowedRange{0x500000,std::as_writable_bytes(std::span(commands)).first(4096),false},
            AgcDriver::NativeGuestMemory::BorrowedRange{0x510000,std::as_writable_bytes(std::span(output)).first(4096),true},
            AgcDriver::NativeGuestMemory::BorrowedRange{0x520000,std::span(code).first(4096),false},
            AgcDriver::NativeGuestMemory::BorrowedRange{0x530000,std::span(header).first(4096),true}};
        const auto publish=s.queues->EopPublisher();
        driver.Configure((__bridge void*)device,(__bridge void*)library,ranges,
            [this,publish,owner=s.owner](std::uint32_t id){
                if(std::this_thread::get_id()==owner)producerOwner=true;
                if(id!=0)wrongId=true;
                const auto previous=callbacks.fetch_add(1);
                // Before this first publication the guest remains parked.
                // Read its borrowed backing only; do not access Machine or
                // write guest bytes from this native worker callback.
                if(computeSubmitted && previous==0 && session.page!=pageBeforeCompute)producerGuestChanged=true;
                publish(id);
            });
        const std::array shaderRanges{
            AgcDriver::Metal::ReadableGuestRange{HeaderAddress,publishedHeader.size()},
            AgcDriver::Metal::ReadableGuestRange{CodeAddress,sizeof(Wave32Code)}};
        const auto previousHeader=header;
        driver.RegisterShaderWithPublication(HeaderAddress,shaderRanges,
            [&]{std::copy(publishedHeader.begin(),publishedHeader.end(),header.begin()+256);},
            [&]{header=previousHeader;});
    }
    ~NativeEop(){driver.Shutdown();}
    void submitCompute(){
        session.threads->CheckIdleOwner();
        require(!computeSubmitted && callbacks==0,"Compute EOP fixture attempted repeated dispatch");
        pageBeforeCompute=session.page;
        std::copy(ComputeCommands.begin(),ComputeCommands.end(),commands.begin());
        computeSubmitted=true;
        Cpu::MakeNativeAgcBackend(driver).Submit(0x500000,static_cast<std::uint32_t>(ComputeCommands.size()),0,0);
        // No WaitIdle here: the actual EOP mailbox must wake the owner first.
    }
    void computeGolden()const{
        require(computeSubmitted,"Compute golden examined without a real submission");
        for(unsigned thread=0;thread<64;++thread){
            const auto lane=thread%32;
            // Two 32-lane waves: lane number, eight predicate bits, lane3's
            // thread value from its own wave, and lane<8 EXEC store predicate.
            const std::array<std::uint32_t,4> expected{lane,8,thread<32?3u:35u,lane<8?1u:0u};
            for(unsigned result=0;result<4;++result)
                require(output[64+thread*4+result]==expected[result],"EOP owner wake preceded actual Metal subgroup result or produced wrong compute data");
        }
        for(unsigned i=0;i<output.size();++i)
            if((i<64 || i>=320) && i!=960)
                require(output[i]==0xdeadbeef,"Actual Metal compute changed untouched output/allocation guards");
    }
    void ownerWakeCompute()const{
        session.threads->CheckIdleOwner();
        require(callbacks==1 && !producerOwner && !wrongId && !producerGuestChanged,
                "Compute waiter woke without exactly one real mailbox-only worker EOP callback");
        computeGolden();
        require(output[960]==0xabcdef12,"Compute-following RELEASE_MEM worker sentinel differs");
        require(std::equal(ComputeCommands.begin(),ComputeCommands.end(),commands.begin()),"Native dispatch changed captured compute commands");
        for(unsigned i=ComputeCommands.size();i<commands.size();++i)
            require(commands[i]==0xdeadbeef,"Native dispatch changed command allocation guard");
        require(std::equal(publishedHeader.begin(),publishedHeader.end(),header.begin()+256),"Native dispatch changed registered self-contained header");
        require(std::memcmp(code.data()+256,Wave32Code.data(),sizeof(Wave32Code))==0,"Native dispatch changed read-only RDNA shader");
        for(unsigned i=0;i<code.size();++i){
            if(i<256 || i>=256+sizeof(Wave32Code))require(code[i]==std::byte{0x7b},"Native dispatch changed code allocation guard");
            if(i<256 || i>=256+publishedHeader.size())require(header[i]==std::byte{0x7b},"Native dispatch changed header allocation guard");
        }
    }
    // Label-only RELEASE_MEM control. Its value is a native worker store,
    // distinct from the actual Metal numerical results checked at owner wake.
    void fireLabel(std::uint32_t value){
        session.threads->CheckIdleOwner();
        const auto before=session.page;
        const auto prior=callbacks.load();
        const std::array<std::uint32_t,8> packet{0xc0064900,0,(1u<<29)|(1u<<24),0x510f00,0,value,0,0};
        std::copy(packet.begin(),packet.end(),commands.begin());
        Cpu::MakeNativeAgcBackend(driver).Submit(0x500000,8,0,0);
        driver.WaitIdle();
        require(callbacks==prior+1 && !producerOwner && !wrongId,"Genuine native EOP did not publish exactly once from its completion thread");
        require(session.page==before,"Native producer wrote guest event/count before owner delivery");
        require(output[960]==value,"Label-only RELEASE_MEM failed native worker sentinel write");
        if(computeSubmitted)computeGolden();
        else for(unsigned i=0;i<output.size();++i)if(i!=960)
            require(output[i]==0xdeadbeef,"Label-only RELEASE_MEM changed untouched native output guard");
    }
};
void targetAdmission(){
    Cpu::Machine machine;
    const auto selected=Cpu::TargetAgcAdmissions();
    // AGC gate mappings last for the Machine lifetime, including after their
    // provider expires. Keep these independent admission cases on distinct
    // pages rather than relying on provider destruction to recycle a mapping.
    constexpr std::uint64_t ControlGateBase=0x7ffdf3000000;
    // Descriptor-selected capability is distinct from the older explicit enum
    // capability fixture, which never admits the newly qualified target policy.
    {
        Cpu::SceAgcImports missing(machine,{},selected,ControlGateBase);
        for(auto nid:{"w2rJhmD+dsE","DL2RXaXOy88"})
            rejects([&]{missing.Resolve(graphicsRow(nid),2,0);},"native backend contract is unavailable");
    }
    auto threads=std::make_shared<Cpu::GuestThreads>(machine);
    Cpu::Platform::KernelEvents queue(machine,threads);
    unsigned invokes=0;
    Cpu::SceAgcBackend backend;
    backend.AddEvent=[&](std::uint64_t handle,std::int32_t id,std::uint64_t user){++invokes;return queue.AddGraphicsEvent(handle,id,user);};
    backend.DeleteEvent=[&](std::uint64_t handle,std::int32_t id){++invokes;return queue.DeleteGraphicsEvent(handle,id);};
    {
        Cpu::SceAgcImports unselected(machine,backend,std::span<const Cpu::AgcAbiContract>{},ControlGateBase+0x1000);
        for(auto nid:{"w2rJhmD+dsE","DL2RXaXOy88"})
            rejects([&]{unselected.Resolve(graphicsRow(nid),2,0);},"Unqualified SCE AGC target ABI contract");
    }
    {
        Cpu::SceAgcImports qualified(machine,backend,selected,ControlGateBase+0x3000);
        for(auto nid:{"w2rJhmD+dsE","DL2RXaXOy88"}){
            const auto row=graphicsRow(nid);
            require(qualified.Resolve(row,2,0)!=0,"Exact target event admission did not allocate a genuine gate");
            for(unsigned field=0;field<5;++field){
                auto wrong=row;
                if(field==0)wrong.LibraryName="libSceAgc";
                if(field==1)wrong.ModuleName="other";
                if(field==2)wrong.LibraryVersion=2;
                if(field==3)wrong.ModuleMajor=2;
                if(field==4)wrong.ModuleMinor=2;
                rejects([&]{qualified.Resolve(wrong,2,0);},"scope/version");
            }
            rejects([&]{qualified.Resolve(row,1,0);},"only function symbols");
            rejects([&]{qualified.Resolve(row,2,8);},"only function symbols");
        }
    }
    require(invokes==0,"Resolution/admission invoked numeric queue backend");queue.Shutdown();threads->Withdraw();
    std::cout<<"PASS descriptor-selected target Add/Delete admission, identity/default/capability guards\n";
}
void actualNativeBoundary(const char* file,id<MTLDevice> device,id<MTLLibrary> library){
    AgcDriver::Metal::MetalDriver driver;Session s(file,0,driver,std::chrono::seconds(10));NativeEop native(device,library,driver,s);
    bool submitted=false,wakeChecked=false,late=false;
    s.boundary=[&](bool waiting){
        const auto r=s.state();
        if(waiting && !submitted){
            require(r[3]==1 && r[4]==0 && r[5]==0 && !r[0] && s.calls.adds==1 && s.calls.deletes==0,
                    "Target AddEqEvent did not actually precede parked translated wait");
            require(r[2]>0xffffffffULL && s.calls.handle==r[2] && s.calls.id==0 && s.calls.user==0,
                    "Translated AddEqEvent lost numeric64 queue handle or qualified arguments");
            native.submitCompute();submitted=true;
        }
        if(submitted && r[3]==1)
            require(s.page==native.pageBeforeCompute,"Native producer published guest output before owner continuation");
        if(r[3]==3 && !late){
            require(submitted && r[10]==0 && r[11]==1 && r[9]==0 && s.calls.deletes==1,
                    "Translated wait/wake/DeleteEqEvent did not finish on its persistent owner");
            // First post-wake owner boundary. No host WaitIdle has run since
            // submission: results must already be available through the EOP.
            native.ownerWakeCompute();wakeChecked=true;
            require(std::equal(Golden.begin(),Golden.end(),s.page.begin()+32),
                    "Compute EOP owner wake missed independent guest event32 bytes");
            native.fireLabel(0x13579bdf);late=true; // Label-only late control, no replacement subscription.
        }
    };
    s.finish();const auto r=s.state();
    require(submitted && wakeChecked && late && s.idleBoundaries && native.callbacks==2 && r[10]==0 && r[11]==1 &&
            r[12]==0x8002003c && r[13]==0 && r[14]==0 && r[15]==0x80020009 &&
            r[22]==0x8002000e && r[23]==0x80020016,
            "Native target wait/wake/delete, empty late poll or pointer-null statuses differ");
    require(std::equal(Golden.begin(),Golden.end(),s.page.begin()+32),"Native target event32 differs from independent byte golden");
    std::array<std::byte,32> first{},after{};
    std::memcpy(first.data(),r.data()+24,32);std::memcpy(after.data(),r.data()+28,32);
    require(first==Golden && after==Golden,"Translated guest saw stale or widened event record after deletion");
    std::cout<<"PASS translated target add -> actual Metal wave32 compute -> real EOP mailbox -> owner wake checks numerical output/guards before WaitIdle -> translated delete; late label-only EOP without replacement drops\n";
}
void rejectedArguments(const char* file){
    for(unsigned mode:{1U,2U,3U,4U,5U,6U,7U,8U}){
        AgcDriver::Metal::MetalDriver driver;Session s(file,mode,driver);const auto before=s.page;
        // Fail at the first owning scheduler boundary if the intended guard is
        // removed. Unsupported add/delete must never reach stage1's yield;
        // bad-span cases may reach that pre-call yield, but may neither park
        // nor return from their subsequent wait. This is an observable guest
        // continuation oracle, not a timeout or an alternate rejection reason.
        s.boundary=[&](bool waiting){
            const auto r=s.state();
            if(mode<=5)
                require(!waiting && r[3]!=1 && r[3]!=2 && r[3]!=3 && !r[0],
                        "Negative control: rejected target AGC call reached guest continuation");
            else
                require(!waiting && r[3]!=2 && r[3]!=3 && !r[0],
                        "Negative control: invalid guest wait span parked or returned");
        };
        const std::string_view reason=mode==1||mode==4?"invalid event queue":mode==2||mode==3||mode==5?
            "graphics event arguments outside qualified target use":"Guest access denied";
        rejects([&]{s.graph->RunMain(1000000,10000);},reason);
        const auto r=s.state();
        require(r[4]==0 && r[2]>0xffffffffULL && r[0]==0 && s.page==before,
                "Rejected target call partially published event/count or completed guest");
        if(mode==1)require(s.calls.adds==1 && s.calls.deletes==0 && s.calls.handle==Output,
                           "Invalid numeric handle did not reach genuine queue identity validation");
        if(mode==2||mode==3)require(s.calls.adds==0 && s.calls.deletes==0 && r[3]==10,
                                  "Unsupported target selector/udata escaped policy before callback");
        if(mode==4)require(s.calls.adds==1 && s.calls.deletes==1 && s.calls.handle==Output && r[3]==11,
                           "Invalid numeric delete did not reach genuine queue identity validation");
        if(mode==5)require(s.calls.adds==1 && s.calls.deletes==0 && r[3]==11,
                           "Unsupported delete selector escaped target policy");
        if(mode>=6)require(s.calls.adds==1 && s.calls.deletes==0 && r[3]==1,
                           "Invalid guest wait span missed admitted target subscription");
        s.guards();
    }
    std::cout<<"PASS translated target bad numeric handles, selectors/udata and event/count/timeout spans preserve guarded guest outputs\n";
}
void sourceWidth(const char* file){
    AgcDriver::Metal::MetalDriver driver;Session s(file,10,driver);s.finish();const auto r=s.state();
    require(r[4]==0 && r[5]==0 && r[9]==0 && r[14]==0 && s.calls.adds==1 && s.calls.deletes==1 &&
            s.calls.id==-7 && s.calls.user==0x8877665544332211ULL && s.calls.handle==r[2],
            "Explicit source contract lost signed32 event or numeric64 handle/udata");
    for(auto value:s.page)require(value==std::byte{0xa5},"Source-width add/delete touched guest event outputs");
    std::cout<<"PASS translated explicit source contract retains signed32 event and numeric64 udata independently of target policy\n";
}
void cancellation(const char* file,id<MTLDevice> device,id<MTLLibrary> library){
    AgcDriver::Metal::MetalDriver driver;Session s(file,9,driver);NativeEop native(device,library,driver,s);
    bool stopped=false;
    s.boundary=[&](bool waiting){if(waiting && !stopped){
        const auto r=s.state();require(r[3]==1 && r[5]==0 && s.calls.adds==1,"Cancellation lacked genuinely admitted parked target wait");
        s.machine.RequestStop();stopped=true;
    }};
    const auto before=s.page;
    require(s.graph->RunMain(1000000,10000)==Cpu::StopReason::Requested && stopped,
            "Cancelled target wait manufactured a guest return");
    require(s.state()[0]==0 && s.state()[3]==1 && s.page==before,"Cancelled target wait published guest output");
    native.fireLabel(0xabcdef12); // Label-only real worker EOP after terminal cancellation, before owner withdrawal.
    Cpu::GuestPhaseBudget budget(10000);
    require(s.threads->RunEntry(budget)==Cpu::StopReason::Requested && budget.Consumed()==0 && s.page==before,
            "Real late EOP resumed terminal target wait");
    s.queues->Shutdown();native.fireLabel(0x13579bdf);
    require(s.page==before && native.callbacks==2,"Retained native publisher wrote after provider shutdown");s.guards();
    std::cout<<"PASS cancelled translated target wait stays terminal after actual label-only worker EOP and retained post-shutdown completion\n";
}
}
int main(int argc,char** argv){
    @autoreleasepool{
        try{
            require(argc==3,"Usage: TargetAgcEventsFixture public-guest.elf utility.metallib");
            targetAdmission();
            id<MTLDevice> device=MTLCreateSystemDefaultDevice();require(device!=nil,"Target event native fixture requires Metal device");
            NSError* error=nil;
            id<MTLLibrary> library=[device newLibraryWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]]] error:&error];
            require(library!=nil,"Target event native fixture requires existing utility metallib");
            actualNativeBoundary(argv[1],device,library);rejectedArguments(argv[1]);sourceWidth(argv[1]);cancellation(argv[1],device,library);
            std::cout<<"PASS target AGC numeric event boundary; public fixture only; game_runtime_ready=false\n";return 0;
        }catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
    }
}
