#include "../NativeSocketServices.hpp"
#include "../../src/SceImageData.hpp"
#include <cpu/GuestThreads.hpp>
#include <cpu/SceThreadImports.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

// Test-audit authoring record: cpu14/fixtures-audit.json. No provider accessor,
// source assertion, fake socket or alternate scheduler supplies an oracle.
namespace {
using Cpu::Register;
using Cpu::Permission;
using Provider = Cpu::Platform::NativeSocketServices;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr std::array<std::string_view,3> Nids{"Q4qBuN-c0ZM","45ggEzakPJQ","HQOwnfMGipQ"};
constexpr std::uint64_t Data = 0x300000, Receipt = Data + 0x1000, Name = Data + 0x2000;
using State = std::array<std::uint64_t,32>;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void throws(F&& f) {
    try { f(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Guest memory permission/span control was accepted");
}
Cpu::SceImport scoped(std::string_view nid, std::string_view family = "libSceNet") {
    Cpu::SceImport row;
    row.Nid = nid; row.LibraryName = row.ModuleName = family;
    row.LibraryVersion = row.ModuleMajor = row.ModuleMinor = 1;
    row.LibraryId = row.ModuleId = 1;
    return row;
}
Provider::Configuration enabled(const Cpu::SceParsedImage& image) {
    Provider::Configuration value;
    value.EnablePublicFixtureCandidate = true;
    value.PublicFixtureSha256 = image.SourceSha256;
    value.PublicFixtureSize = image.SourceSize;
    return value;
}
bool sameMappings(const std::vector<Cpu::Mapping>& a, const std::vector<Cpu::Mapping>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t n=0; n<a.size(); ++n)
        if (a[n].Address != b[n].Address || a[n].Size != b[n].Size ||
            a[n].Permissions != b[n].Permissions || a[n].Borrowed != b[n].Borrowed) return false;
    return true;
}
using SocketInventory = std::map<int,int>;
SocketInventory osSockets() {
    struct rlimit limit{};
    require(::getrlimit(RLIMIT_NOFILE,&limit)==0 && limit.rlim_cur <= static_cast<rlim_t>(std::numeric_limits<int>::max()),
            "Cannot enumerate the actual OS descriptor range");
    SocketInventory result;
    for (int fd=0; fd<static_cast<int>(limit.rlim_cur); ++fd) {
        if (::fcntl(fd,F_GETFD)<0) continue;
        int type=0; socklen_t size=sizeof(type);
        if (::getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&size)==0 && size==sizeof(type)) result.emplace(fd,type);
    }
    return result;
}
SocketInventory added(const SocketInventory& before, const SocketInventory& after) {
    SocketInventory result;
    for (const auto& [fd,type]:after) if (!before.contains(fd)) result.emplace(fd,type);
    return result;
}
struct OsFd {
    int fd;
    explicit OsFd(int value):fd(value) { require(fd>=0,"Independent unrelated OS FD could not be opened"); }
    ~OsFd() { ::close(fd); }
    OsFd(const OsFd&)=delete;
};
struct Session {
    Cpu::Machine machine;
    std::shared_ptr<Cpu::GuestThreads> threads = std::make_shared<Cpu::GuestThreads>(machine);
    Cpu::SceThreadImports kernel{machine,threads};
    Cpu::SceParsedImage consumer;
    std::unique_ptr<Provider> provider;
    std::array<std::uint64_t,3> gates{};
    const Cpu::SceImport& importRow(unsigned n) const {
        const auto found=std::find_if(consumer.Imports.begin(),consumer.Imports.end(),[&](const auto& row) { return row.Nid==Nids[n]; });
        require(found!=consumer.Imports.end(),"Actual linked guest lost required scoped import"); return *found;
    }
    std::uint64_t resolve(Provider& owner,unsigned n) const {
        const auto& row=importRow(n);
        const auto index=static_cast<std::size_t>(&row-consumer.Imports.data());
        const auto symbol=std::find_if(consumer.Data->Symbols.begin(),consumer.Data->Symbols.end(),[&](const auto& entry) {
            return entry.Import && *entry.Import==index;
        });
        require(symbol!=consumer.Data->Symbols.end() && symbol->Type==2 && symbol->Size==0 && !symbol->Section,
                "Actual guest import lacks observed undefined STT_FUNC/size0");
        return owner.Resolve(row,symbol->Type,symbol->Size,consumer).value();
    }
    Session(const char* textPath, const char* elfPath, bool activate=true):consumer(Cpu::ParseSce(elfPath)) {
        require(std::string_view(Cpu::Machine::Backend()).find("Modern QEMU TCG")!=std::string_view::npos,
                "Socket fixture requires native modern TCG");
        (void)textPath; // Retained argv artifact is diagnostic; execution loads the actual SCE ELF.
        require(consumer.Type==0xfe10 && consumer.Imports.size()==3 && consumer.UnsupportedReasons.empty() &&
                std::set<std::uint32_t>(consumer.RelocationTypes.begin(),consumer.RelocationTypes.end())==std::set<std::uint32_t>{7,8},
                "Actual public guest lost its three function imports or real PLT/RELATIVE profile");
        machine.Map(0x100000,0x1000,rx);
        const std::array call{std::byte{0xff},std::byte{0xd0},std::byte{0x90}};
        machine.Write(0x100000,call);
        machine.Map(0x200000,0x10000,rw); machine.Map(Data,0x10000,rw);
        const std::array<std::uint64_t,1> sentinel{0x100002};
        machine.Write(0x20ffe8,std::as_bytes(std::span(sentinel)));
        const std::array<char,32> name{'n','n','n','n','n','n','n','n','n','n','n','n','n','n','n','n',
                                      'n','n','n','n','n','n','n','n','n','n','n','n','n','n','n',0};
        machine.Write(Name,std::as_bytes(std::span(name)));
        std::uint64_t entry=0x100000;
        if (activate) {
            provider=std::make_unique<Provider>(machine,threads,enabled(consumer));
            unsigned resolved=0;
            const auto loaded=Cpu::LoadSce(machine,elfPath,0x1000000,[&](const Cpu::SceImport& row) {
                unsigned n=0; while(n<Nids.size() && Nids[n]!=row.Nid) ++n;
                require(n<Nids.size(),"Actual loaded SCE guest requested an unexpected import");
                const auto& observed=importRow(n);
                require(row.LibraryName==observed.LibraryName && row.ModuleName==observed.ModuleName &&
                        row.LibraryId==observed.LibraryId && row.ModuleId==observed.ModuleId &&
                        row.LibraryVersion==observed.LibraryVersion && row.ModuleMajor==observed.ModuleMajor &&
                        row.ModuleMinor==observed.ModuleMinor,"Loaded import differs from actual independently parsed row");
                gates[n]=resolve(*provider,n); ++resolved; return gates[n];
            });
            require(resolved==3,"LoadSce did not relocate all three observed guest imports");
            entry=loaded.Entry;
        }
        machine.Set(Register::Rsp,0x20ffe8); machine.Set(Register::Rdi,Data);
        threads->AdoptInitial({entry,{0x200000,0x10000,rw,false},{},
            [](std::uint64_t) { return std::shared_ptr<Cpu::SceTls>{}; }});
    }
    ~Session() { provider.reset(); threads->Withdraw(); }
    State state() const { State s{}; machine.Read(Receipt,std::as_writable_bytes(std::span(s))); return s; }
    std::uint64_t call(unsigned gate, std::uint64_t a=0,std::uint64_t b=0,std::uint64_t c=0,std::uint64_t d=0) {
        machine.Set(Register::Rax,gates[gate]); machine.Set(Register::Rdi,a); machine.Set(Register::Rsi,b);
        machine.Set(Register::Rdx,c); machine.Set(Register::Rcx,d); machine.Set(Register::Rsp,0x20fff0);
        require(machine.Run(0x100000,0x100002,100)==Cpu::StopReason::Address,"Socket gate did not return to actual guest caller");
        require(machine.Get(Register::Rsp)==0x20fff0,"Socket gate changed the guest call stack");
        return machine.Get(Register::Rax);
    }
    std::uint32_t create(std::uint64_t name=0,std::uint64_t family=2,std::uint64_t type=1,std::uint64_t protocol=0) {
        return static_cast<std::uint32_t>(call(0,name,family,type,protocol));
    }
    std::uint32_t close(std::uint64_t fd) { return static_cast<std::uint32_t>(call(1,fd)); }
    std::uint64_t cell() { const auto value=call(2); machine.CheckAccess(value,4,rw); return value; }
    std::int32_t errnoValue(std::uint64_t p) { std::int32_t v{}; machine.Read(p,std::as_writable_bytes(std::span(&v,1))); return v; }
};
void scopes(const char* text,const char* elf) {
    Session s(text,elf,false);
    const auto before=s.machine.Mappings();
    Provider denied(s.machine,s.threads);
    for (unsigned n=0;n<Nids.size();++n) require(!denied.Resolve(s.importRow(n),2,0,s.consumer),"Default candidate admitted a fixture or retail source");
    require(sameMappings(before,s.machine.Mappings()),"Default-denied source allocated a gate or errno storage");
    Provider candidate(s.machine,s.threads,enabled(s.consumer));
    auto rejection=[&](const Cpu::SceImport& row,std::uint8_t type,std::uint64_t size,const Cpu::SceParsedImage& image) {
        bool rejected=false;
        try { rejected=!candidate.Resolve(row,type,size,image); } catch(const std::exception&) { rejected=true; }
        require(rejected,"Unqualified socket source/type/size/scope was admitted");
        require(sameMappings(before,s.machine.Mappings()),"Rejected socket import allocated provider mappings");
    };
    for (unsigned n=0;n<Nids.size();++n) {
        const auto valid=s.importRow(n);
        for (const auto type:{0,1,3,6}) rejection(valid,type,0,s.consumer);
        for (const auto size:{1ULL,8ULL,~0ULL}) rejection(valid,2,size,s.consumer);
        for (unsigned field=0;field<7;++field) {
            auto row=valid;
            if(field==0) row.LibraryName="foreign";
            if(field==1) row.ModuleName="foreign";
            if(field==2) ++row.LibraryVersion;
            if(field==3) ++row.ModuleMajor;
            if(field==4) ++row.ModuleMinor;
            if(field==5) ++row.LibraryId;
            if(field==6) ++row.ModuleId;
            rejection(row,2,0,s.consumer);
        }
        auto image=s.consumer; image.SourceSha256[0]^=std::byte{1}; rejection(valid,2,0,image);
        image=s.consumer; ++image.SourceSize; rejection(valid,2,0,image);
        image=s.consumer; image.Data.reset(); rejection(valid,2,0,image);
        image=s.consumer; image.SourceContainer="plain_self"; rejection(valid,2,0,image);
        image=s.consumer; image.Path="CivVII.elf"; rejection(valid,2,0,image);
        image=s.consumer; image.Type=3; rejection(valid,2,0,image);
        image=s.consumer; image.Imports.clear(); rejection(valid,2,0,image);
        image=s.consumer;
        auto changed=std::find_if(image.Imports.begin(),image.Imports.end(),[&](const auto& row) { return row.Nid==valid.Nid; });
        changed->Nid="foreign"; rejection(valid,2,0,image);
        image=s.consumer;
        changed=std::find_if(image.Imports.begin(),image.Imports.end(),[&](const auto& row) { return row.Nid==valid.Nid; });
        changed->LibraryName="foreign"; rejection(valid,2,0,image);
    }
    rejection(scoped("unknown"),2,0,s.consumer);
    const auto good=candidate.Resolve(s.importRow(0),2,0,s.consumer);
    require(good.has_value() && candidate.Resolve(s.importRow(0),2,0,s.consumer)==good,
            "Actual public source failed admission or repeat resolution changed gate");
    std::cout<<"scope receipt: default denial, exact actual ELF source/type2/size0/libSceNet1.1/v1, before-allocation rejection PASS\n";
}
void compiled(const char* text,const char* elf,bool child) {
    OsFd unrelatedSocket(::socket(AF_INET,SOCK_DGRAM,0));
    OsFd unrelatedFile(::open("/dev/null",O_RDONLY));
    const auto baseline=osSockets();
    Session s(text,elf);
    constexpr std::uint64_t observe=0x101000,exit=0x101010;
    s.machine.Map(observe,0x1000,rx);
    const std::array ret{std::byte{0xc3}};
    s.machine.Write(observe,ret); s.machine.Write(exit,ret);
    SocketInventory created; unsigned seen=0;
    s.machine.AddHostCall(observe,[&](Cpu::Machine& machine) {
        const auto stage=machine.Get(Register::Rdi); const auto r=s.state();
        if(stage==10 || stage==11) {
            const auto address=stage==10?r[2]:r[12];
            machine.CheckAccess(address,4,rw);
            require(address && address<0x800000000000ULL,"errno gate returned a host/noncanonical pointer");
            if(stage==11) require(r[12]!=r[2] && r[11]!=r[1],"Actual guest child shared initial errno address or identity");
            return;
        }
        if(stage==1) {
            created=added(baseline,osSockets());
            require(created.size()==2,"Independent OS oracle: guest IDs were not backed by two real sockets");
            std::multiset<int> types;
            for(const auto& [fd,type]:created) {
                types.insert(type);
                sockaddr_storage address{}; socklen_t length=sizeof(address);
                require(::getsockname(fd,reinterpret_cast<sockaddr*>(&address),&length)==0 &&
                        address.ss_family==AF_INET,"Actual guest socket is not IPv4");
                const auto statusFlags=::fcntl(fd,F_GETFL), descriptorFlags=::fcntl(fd,F_GETFD);
                require(statusFlags>=0 && descriptorFlags>=0 && (statusFlags&O_NONBLOCK)!=0 &&
                        (descriptorFlags&FD_CLOEXEC)!=0,
                        "Actual candidate socket lacks pinned nonblocking/close-on-exec flags");
            }
            require(types==std::multiset<int>{SOCK_STREAM,SOCK_DGRAM},"Actual TCP/UDP SO_TYPE does not match independent guest calls");
            ++seen;
        } else if(stage==2) {
            const auto now=osSockets();
            for(const auto& [fd,type]:created) {
                if(type==SOCK_STREAM) {
                    errno=0; require(::fcntl(fd,F_GETFD)==-1 && errno==EBADF,
                                     "Independent OS oracle: closed guest TCP still has a live host FD");
                } else require(now.contains(fd),"Closing TCP also closed the still-owned guest UDP");
            }
            require(added(baseline,now).size()==1,"Guest close leaked or released unexpected host socket count");
            ++seen;
        } else throw std::runtime_error("Unknown public guest OS-observation stage");
    });
    s.machine.AddHostCall(exit,[&](Cpu::Machine& machine) {
        s.threads->ProcessExitFromHostCall(static_cast<int>(machine.Get(Register::Rdi)));
    });
    auto kernel=[&](std::string_view nid) { return s.kernel.Resolve(scoped(nid,"libkernel"),2).value(); };
    const std::array<std::uint64_t,7> args{kernel("6UgtwV+0zb4"),kernel("onNY9Byn-W8"),
        kernel("aI+OeCz8xrQ"),observe,exit,Receipt,child?1ULL:0ULL};
    s.machine.Write(Data,std::as_bytes(std::span(args)));
    Cpu::GuestPhaseBudget budget(1000000);
    require(s.threads->RunEntry(budget)==Cpu::StopReason::Paused,"Compiled x86 socket guest failed to reach real process exit");
    const auto guestExit=s.threads->PendingEntryControl();
    require(guestExit.ExitCode.has_value(),"Compiled socket guest omitted process exit status");
    if(*guestExit.ExitCode!=0)
        throw std::runtime_error("Compiled socket guest reported failure exit "+std::to_string(*guestExit.ExitCode));
    const auto r=s.state();
    require(r[0]==0x4e4554534f434b54ULL && seen==2,"Independent compiled guest socket receipt did not complete");
    const auto remaining=added(baseline,osSockets());
    require(remaining.size()==1 && remaining.begin()->second==SOCK_DGRAM,"Fixture did not retain its one real teardown socket");
    s.provider.reset();
    require(osSockets()==baseline && ::fcntl(unrelatedFile.fd,F_GETFD)>=0 && ::fcntl(unrelatedSocket.fd,F_GETFD)>=0,
            "Provider teardown leaked owned sockets or closed an unrelated host FD");
    for(const auto gate:s.gates) throws([&] { s.machine.CheckAccess(gate,1,Permission::Execute); });
    std::cout<<"compiled receipt: ids="<<r[4]<<','<<r[5]<<" initial-thread="<<r[1]<<" initial-errno="<<r[2];
    if(child) std::cout<<" child-thread="<<r[11]<<" child-errno="<<r[12]<<" child-error="<<r[15]
                       <<" parent-preserved="<<r[10]<<" child-preserved="<<r[16];
    std::cout<<" host-fds=";
    for(const auto& [fd,type]:created) std::cout<<fd<<':'<<type<<',';
    std::cout<<" close/teardown/unrelated-resource PASS\n";
}
void ownership(const char* text,const char* elf) {
    const auto baseline=osSockets();
    Session first(text,elf);
    const auto firstGates=first.gates;
    // Allocate in the older provider before constructing the next one: a
    // constructor that resets process IDs must collide while both are live.
    const auto a=first.create();
    auto otherConfiguration=enabled(first.consumer);
    otherConfiguration.GateBase+=0x200000; otherConfiguration.ErrnoBase+=0x200000;
    auto second=std::make_unique<Provider>(first.machine,first.threads,otherConfiguration);
    std::array<std::uint64_t,3> secondGates{};
    for(unsigned n=0;n<3;++n) secondGates[n]=first.resolve(*second,n);
    first.gates=secondGates; const auto b=first.create(); first.gates=firstGates;
    require(static_cast<std::int32_t>(a)>=0 && static_cast<std::int32_t>(b)>=0 && a!=b,
            "Separate socket sessions reused the same virtual identity");
    const auto live=osSockets();
    require(added(baseline,live).size()==2,"Cross-session oracle lacks two genuine owned host sockets");
    first.gates=secondGates; const auto secondReject=first.close(a); first.gates=firstGates;
    require(secondReject==0x80410109U && first.close(b)==0x80410109U && osSockets()==live,
            "Cross-session descriptor closed or changed another session's actual socket");
    require(first.close(a)==0 && first.close(a)==0x80410109U,"Repeated socket close admitted a stale ID");
    const auto fresh=first.create();
    require(static_cast<std::int32_t>(fresh)>=0 && fresh!=a && first.close(a)==0x80410109U,
            "New socket reused or accepted a stale descriptor ID");
    first.provider.reset();
    first.provider=std::make_unique<Provider>(first.machine,first.threads,enabled(first.consumer));
    for(unsigned n=0;n<3;++n) first.gates[n]=first.resolve(*first.provider,n);
    const auto replacement=first.create();
    const auto replacementLive=osSockets();
    require(static_cast<std::int32_t>(replacement)>=0 && replacement!=a && replacement!=b && replacement!=fresh &&
            first.close(a)==0x80410109U && first.close(fresh)==0x80410109U && first.close(b)==0x80410109U &&
            osSockets()==replacementLive,
            "Replacement session admitted an old session descriptor");
    first.provider.reset(); second.reset();
    require(osSockets()==baseline,"Cross-session/stale control leaked OS sockets");
    std::cout<<"ownership receipt: sessions="<<a<<','<<b<<" fresh="<<fresh<<" replacement="<<replacement
             <<" stale/repeated/cross-session PASS\n";
}
void actualHostError(const char* text,const char* elf) {
    Session s(text,elf); const auto cell=s.cell();
    const auto baseline=osSockets();
    struct DescriptorLimit {
        struct rlimit original{};
        std::vector<int> owned;
        bool changed=false;
        void restore() {
            for(const auto fd:owned) ::close(fd);
            owned.clear();
            if(changed) {
                const auto result=::setrlimit(RLIMIT_NOFILE,&original);
                changed=false;
                require(result==0,"Failed to restore this test process's descriptor soft limit");
            }
        }
        ~DescriptorLimit() {
            for(const auto fd:owned) ::close(fd);
            if(changed && ::setrlimit(RLIMIT_NOFILE,&original)!=0)
                std::cerr<<"Descriptor-limit restoration failed in test process\n";
        }
    } limit;
    require(::getrlimit(RLIMIT_NOFILE,&limit.original)==0 && limit.original.rlim_cur<=static_cast<rlim_t>(std::numeric_limits<int>::max()),
            "Cannot read this test process's descriptor limit");
    int highest=-1;
    for(int fd=0;fd<static_cast<int>(limit.original.rlim_cur);++fd)
        if(::fcntl(fd,F_GETFD)>=0) highest=fd;
    auto lowered=limit.original;
    lowered.rlim_cur=std::min(limit.original.rlim_cur,static_cast<rlim_t>(highest)+33);
    limit.owned.reserve(static_cast<std::size_t>(lowered.rlim_cur));
    require(::setrlimit(RLIMIT_NOFILE,&lowered)==0,"Cannot lower descriptor soft limit in this test process");
    limit.changed=true;
    for(;;) {
        const auto fd=::open("/dev/null",O_RDONLY);
        if(fd<0) { require(errno==EMFILE,"Actual FD saturation did not produce EMFILE"); break; }
        limit.owned.push_back(fd);
    }
    require(s.create()==0x80410118U && s.errnoValue(cell)==24 && osSockets()==baseline,
            "Actual socket EMFILE was not translated into independent net error/errno24 without OS side effects");
    limit.restore();
    struct rlimit restored{};
    require(::getrlimit(RLIMIT_NOFILE,&restored)==0 && restored.rlim_cur==limit.original.rlim_cur &&
            restored.rlim_max==limit.original.rlim_max && osSockets()==baseline,
            "Actual EMFILE control failed to restore its own process limit/resources");
    std::cout<<"host error receipt: real socket EMFILE => 0x80410118/errno24; process limit and owned handles restored PASS\n";
}
void names(const char* text,const char* elf) {
    Session s(text,elf);
    const auto baseline=osSockets(); const auto p=s.cell();
    auto error=[&](std::uint64_t name,std::uint64_t family,std::uint64_t type,std::uint64_t protocol,
                   std::uint32_t expected,int expectedErrno) {
        require(s.create(name,family,type,protocol)==expected && s.errnoValue(p)==expectedErrno,
                "Independent candidate scalar/name error or errno literal differs");
        require(osSockets()==baseline,"Rejected candidate scalar/name created a real host resource");
    };
    error(~0ULL,28,1,0,0x8041012bU,43); // scalar rejection precedes inaccessible name
    error(~0ULL,2,3,0,0x8041012bU,43);
    error(~0ULL,2,1,17,0x80410129U,41);
    error(~0ULL,0x12345678ffffffffULL,1,0,0x8041012bU,43);
    const std::array<char,33> longName{'x','x','x','x','x','x','x','x','x','x','x','x','x','x','x','x',
        'x','x','x','x','x','x','x','x','x','x','x','x','x','x','x','x',0};
    s.machine.Write(Name+0x100,std::as_bytes(std::span(longName)));
    error(Name+0x100,2,1,0,0x8041013fU,63);
    auto memory=[&](std::uint64_t pointer) {
        throws([&] { s.create(pointer); });
        require(osSockets()==baseline,"Invalid guest name created a socket before access validation");
    };
    memory(0xdead000); memory(~0ULL); memory(0x800000000000ULL);
    const std::array<char,8> unterminated{'x','x','x','x','x','x','x','x'};
    s.machine.Write(Data+0xfff8,std::as_bytes(std::span(unterminated))); memory(Data+0xfff8);
    s.machine.Map(0x400000,0x1000,Permission::Write); memory(0x400000);
    // Put the complete allowed name at the last 32 readable bytes. No byte33
    // exists: the success and length rejection independently police the bound.
    s.machine.Map(0x410000,0x1000,rw);
    std::array<char,32> edgeName{}; edgeName.fill('e'); edgeName.back()=0;
    s.machine.Write(0x410fe0,std::as_bytes(std::span(edgeName)));
    s.machine.Protect(0x410000,0x1000,Permission::Read);
    const auto edgeId=s.create(0x410fe0);
    require(static_cast<std::int32_t>(edgeId)>=0 && s.close(edgeId)==0,
            "31-byte name at exact readable boundary accessed byte33 or rejected read-only input");
    s.machine.Protect(0x410000,0x1000,rw); edgeName.back()='e';
    s.machine.Write(0x410fe0,std::as_bytes(std::span(edgeName)));
    s.machine.Protect(0x410000,0x1000,Permission::Read);
    error(0x410fe0,2,1,0,0x8041013fU,63);
    // Read-only readable names remain valid; errno writes require full RW access.
    s.machine.Protect(Data,0x10000,Permission::Read);
    const auto valid=s.create(Name,0xfedcba9800000002ULL,0xaabbccdd00000001ULL,0xdeadbeef00000006ULL);
    require(static_cast<std::int32_t>(valid)>=0 && s.close(valid)==0,"Readable 31-byte name or high scalar ABI was rejected");
    const auto nullName=s.create();
    require(static_cast<std::int32_t>(nullName)>=0 && s.close(nullName)==0,"Nullable socket name was rejected");
    s.provider.reset();
    require(osSockets()==baseline,"Name acceptance teardown leaked a host FD");
    s.provider=std::make_unique<Provider>(s.machine,s.threads,enabled(s.consumer));
    for(unsigned n=0;n<3;++n) s.gates[n]=s.resolve(*s.provider,n);
    const auto replacementCell=s.cell();
    const auto mappings=s.machine.Mappings();
    const auto page=std::find_if(mappings.begin(),mappings.end(),[&](const auto& m) {
        return replacementCell>=m.Address && replacementCell-m.Address<m.Size;
    });
    require(page!=mappings.end(),"errno cell lacks its actual mapped guest allocation");
    s.machine.Protect(page->Address,page->Size,Permission::Read);
    throws([&] { s.call(2); }); throws([&] { s.create(); }); throws([&] { s.close(~0ULL); });
    require(osSockets()==baseline,"Read-only errno allocated a socket before checked output validation");
    s.machine.Protect(page->Address,page->Size,rw);
    std::cout<<"name receipt: null/31/32, scalar-before-name, high-s32, unmapped/write-only/wrapped spans, read-only-name/errno PASS\n";
}
}
int main(int argc,char** argv) {
    try {
        require(argc==4,"Usage: NativeSocketServicesTest flat-text actual-public-elf scope|resources|errno|names");
        const std::string_view mode=argv[3];
        if(mode=="scope") scopes(argv[1],argv[2]);
        else if(mode=="resources") { compiled(argv[1],argv[2],false); ownership(argv[1],argv[2]); actualHostError(argv[1],argv[2]); }
        else if(mode=="errno") compiled(argv[1],argv[2],true);
        else if(mode=="names") names(argv[1],argv[2]);
        else throw std::runtime_error("Unknown native socket control");
        std::cout<<"NativeSocketServicesTest "<<mode<<": PASS (public candidate only; retail admission denied)\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<"NativeSocketServicesTest: "<<error.what()<<'\n'; return 1; }
}
