#pragma once
#include "KernelEvents.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceAgcImports.hpp>
#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceThreadImports.hpp>
#include <cpu/SceVideoOutImports.hpp>
#include <array>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

namespace EventFixture {
inline constexpr std::uint64_t Bias=0x100000000, PageAddress=0x600000000,
    Output=PageAddress+16, Count=PageAddress+128, Canary=0x1badc0ffeef00d55ULL;
using State=std::array<std::uint64_t,160>;
inline void require(bool ok, const char* message) { if(!ok) throw std::runtime_error(message); }
template<class F> void rejects(F action, const char* reason) {
    try { action(); } catch(const std::exception& error) {
        require(std::string(error.what()).find(reason)!=std::string::npos,error.what()); return;
    }
    throw std::runtime_error(std::string("Missing event rejection: ")+reason);
}
inline Cpu::SceImport scoped(std::string_view nid) {
    Cpu::SceImport value; value.Nid=nid; value.LibraryName=value.ModuleName="libkernel";
    value.LibraryVersion=value.ModuleMajor=value.ModuleMinor=1;
    value.LibraryId=value.ModuleId=1; return value;
}
struct Session {
    Cpu::Machine machine;
    std::shared_ptr<Cpu::GuestThreads> threads=std::make_shared<Cpu::GuestThreads>(machine);
    Cpu::SceThreadImports imports{machine,threads};
    Cpu::SceLifecycleImports lifecycle{machine};
    std::unique_ptr<Cpu::Platform::KernelEvents> queues;
    std::unique_ptr<Cpu::SceAgcImports> graphics;
    std::unique_ptr<Cpu::SceVideoOutImports> videoOut;
    std::unique_ptr<Cpu::SceModules> graph;
    alignas(65536) std::array<std::byte,65536> page{};
    std::uint64_t address=0;
    unsigned boundaries=0, idleBoundaries=0;
    bool secondaryOutputs=false;
    bool withdrawn=false;
    const std::thread::id owner=std::this_thread::get_id();
    std::function<void(bool)> boundary;
    Session(const char* path,unsigned mode) {
        secondaryOutputs=mode==10 || mode==15;
        require(std::string(Cpu::Machine::Backend()).find("Modern QEMU TCG")!=std::string::npos,
                "Event fixture requires native Modern QEMU TCG");
        page.fill(std::byte{0xa5});
        machine.MapBorrowed(PageAddress,std::span(page).first(4096),
            Cpu::Permission::Read|Cpu::Permission::Write,page);
        queues=std::make_unique<Cpu::Platform::KernelEvents>(machine,threads);
        lifecycle.SetProcessExitHandler([runtime=std::weak_ptr<Cpu::GuestThreads>(threads)](int code){
            const auto owner=runtime.lock(); require(bool(owner),"Event scheduler expired");
            owner->ProcessExitFromHostCall(code);
        });
        Cpu::SceAgcBackend eventBackend;
        eventBackend.DeleteEvent=[this](std::uint64_t handle,std::int32_t id){
            return queues->DeleteGraphicsEvent(handle,id);
        };
        const std::array sourceContracts{Cpu::AgcAbiContract::DeleteEqEvent};
        graphics=std::make_unique<Cpu::SceAgcImports>(machine,std::move(eventBackend),sourceContracts);
        // sceVideoOutWaitVblank as the native runner routes it: parked in this provider.
        Cpu::SceVideoOutBackend videoBackend;
        videoBackend.WaitVblank=[this](std::int32_t output){ queues->WaitVideoOutVblank(output); return 0; };
        videoOut=std::make_unique<Cpu::SceVideoOutImports>(machine,std::move(videoBackend));
        const std::array hosts{
            Cpu::SceHostModule{"libkernel.prx",{"libkernel",0,1,1},{{"libkernel",0,1}}},
            Cpu::SceHostModule{"libSceAgcDriver.prx",{"libSceAgcDriver",0,1,1},{{"libSceAgcDriver",0,1}}},
            Cpu::SceHostModule{"libSceVideoOut.prx",{"libSceVideoOut",0,1,1},{{"libSceVideoOut",0,1}}}};
        graph=std::make_unique<Cpu::SceModules>(machine,Cpu::SceModuleFile{path,Bias},
            std::span<const Cpu::SceModuleFile>{},hosts,
            [&](const auto& row,std::uint8_t type)->std::optional<Cpu::SceResolvedImport>{
                require(type==2,"Event fixture lost genuine linked STT_FUNC");
                if(row.ModuleName=="libSceAgcDriver")
                    return Cpu::SceResolvedImport{graphics->Resolve(row,type,0),type};
                if(row.ModuleName=="libSceVideoOut")
                    return Cpu::SceResolvedImport{videoOut->ResolvePublicFixture(row),type};
                if(const auto gate=queues->Resolve(row,type)) return Cpu::SceResolvedImport{*gate,type};
                if(const auto gate=imports.Resolve(row,type)) return Cpu::SceResolvedImport{*gate,type};
                if(const auto gate=lifecycle.Resolve(row)) return Cpu::SceResolvedImport{*gate,type};
                return std::nullopt;
            });
        const auto& image=graph->Modules()[0].Image;
        require(image.Imports.size()==10 && image.NeededFiles.empty() && image.Tls,
                "Event fixture import/TLS graph differs");
        require(std::set<std::uint32_t>(image.RelocationTypes.begin(),image.RelocationTypes.end())==
                std::set<std::uint32_t>{7,8},"Event fixture lacks genuine PLT/RELATIVE relocations");
        for(const auto& row:image.Exports) if(row.Type==1 && row.Size==sizeof(State)) {
            require(!address,"Ambiguous event receipt"); address=Bias+row.Value;
        }
        require(address && state()==State{},"Event loading executed code or lost receipt");
        put(address+8,mode); put(address+6*8,Output); put(address+7*8,Count);
        put(address+158*8,Canary); put(address+159*8,~Canary);
        Cpu::SetupSceEntry(machine,graph->Main(),{"public-queue-fixture"},graph->EntryTerminationGate());
        threads->AdoptInitial({graph->Main().Entry,graph->InitialStack(),graph->Tls(),graph->ThreadTlsFactory()});
        threads->SetOwnerBoundary([this](bool waiting){
            threads->CheckIdleOwner();
            require(std::this_thread::get_id()==owner,"Owner boundary migrated machine ownership");
            ++boundaries; idleBoundaries+=waiting;
            if(boundary) boundary(waiting);
        },std::chrono::milliseconds(500));
        graph->SetExecutor(threads->ModuleExecutor()); graph->InitializeDependencies();
    }
    ~Session() { videoOut.reset(); graphics.reset(); queues.reset(); if(!withdrawn)threads->Withdraw(); }
    void withdraw(){threads->Withdraw();withdrawn=true;}
    State state() const { State r{};machine.Read(address,std::as_writable_bytes(std::span(r)));return r; }
    void put(std::uint64_t p,std::uint64_t value) { machine.Write(p,std::as_bytes(std::span(&value,1))); }
    std::uint64_t get(std::uint64_t p) const { std::uint64_t r{};machine.Read(p,std::as_writable_bytes(std::span(&r,1)));return r; }
    void finish(bool alreadyDeleted=false,const char* progressFailure="Event guest failed its fixed progress ceiling") {
        require(graph->RunMain(1000000,10000)==Cpu::StopReason::Exit && machine.ExitCode()==0,
                progressFailure);
        const auto r=state();
        require(r[0]==0x455155455545574bULL && r[16]==0 && r[37]==(alreadyDeleted?0x80020009ULL:0) && r[38]==0x80020009 &&
                r[39]==0x80020009 && r[40]==0 && r[41]!=r[2] && r[42]==0,
                "Create/delete/stale queue lifetime or status width differs");
        require(r[11]==0 && r[12]==0 && r[158]==Canary && r[159]==~Canary,
                "Blocked event ABI/registers/stack or receipt guards differ");
        guards();
    }
    void guards() const {
        for(unsigned i=0;i<page.size();++i)
            if(!((i>=16 && i<80)||(i>=128 && i<132)))
                if(!(secondaryOutputs && ((i>=272 && i<304)||(i>=384 && i<388))))
                    require(page[i]==std::byte{0xa5},"Event record/count width overran high-address redzone");
    }
};
inline void record(const State& r,unsigned slot,std::uint64_t id,std::uint64_t count,std::uint64_t user) {
    require(r[slot]==id && r[slot+1]==0x21fff2ULL && r[slot+2]==count && r[slot+3]==user,
            "Independent ident/filter/flags/fflags/data/udata byte oracle differs");
}
}
