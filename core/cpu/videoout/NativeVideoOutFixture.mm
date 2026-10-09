#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>
#include <cpu/SceNativeVideoOutBackend.hpp>
#include <cpu/SceElf.hpp>
#include "prx/libSceVideoOut/include/NativeMetalSession.hpp"
#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

// Authoring gate: this is the primary compiled-caller/mapping-identity keeper.
// A stale registered VA, wrong x86 stack argument, partial batch publication,
// tight-row read or early backing retirement changes independently read drawable
// pixels or externally observable registration results. Existing session tests
// use hand-marshalled calls, uniform tight rows and no mapping replacement.
namespace {
constexpr auto RW = Cpu::Permission::Read | Cpu::Permission::Write;
constexpr auto RX = Cpu::Permission::Read | Cpu::Permission::Execute;
constexpr std::uint64_t Control = 0x100002000ULL, Display = 0x400000000ULL;
constexpr std::uint64_t Attribute = Control + 128, Buffers = Control + 256, Status = Control + 4096 + 384;
constexpr std::uint32_t Width = 64, Height = 64, Pitch = 128;
constexpr std::array<unsigned char,4> Golden{23,67,149,255};
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class Action> void rejects(Action&& action, const char* expected) {
    try { action(); } catch (const std::exception& error) {
        require(std::string(error.what()).find(expected) != std::string::npos, error.what()); return;
    }
    throw std::runtime_error(std::string("Missing rejection: ") + expected);
}
template<class T> void store(Cpu::Machine& m, std::uint64_t address, const T& value) {
    m.Write(address,std::as_bytes(std::span(&value,1)));
}
template<class T> T load(Cpu::Machine& m, std::uint64_t address) {
    T value{}; m.Read(address,std::as_writable_bytes(std::span(&value,1))); return value;
}
Cpu::SceImport identity(const char* nid) {
    Cpu::SceImport value; value.Nid=nid; value.LibraryName=value.ModuleName="libSceVideoOut";
    value.LibraryVersion=1; value.ModuleMajor=value.ModuleMinor=1; value.LibraryId=39; value.ModuleId=40;
    return value;
}
struct Backing {
    std::unique_ptr<std::byte,decltype(&std::free)> bytes{nullptr,&std::free};
    explicit Backing(unsigned char poison=0xa7) {
        void* allocation=nullptr;
        require(posix_memalign(&allocation,65536,65536)==0,"Cannot allocate genuine page-backed display");
        bytes.reset(static_cast<std::byte*>(allocation)); std::memset(bytes.get(),poison,65536);
        for (unsigned y=0;y<Height;++y) for (unsigned x=0;x<Width;++x)
            std::memcpy(bytes.get()+(y*Pitch+x)*4,Golden.data(),4);
    }
    std::span<std::byte> Bytes() { return {bytes.get(),65536}; }
};

id<CAMetalDrawable> captured=nil;
IMP originalNextDrawable=nullptr;
std::promise<void>* captureReady=nullptr;
std::shared_future<void> captureRelease;
id<CAMetalDrawable> captureDrawable(CAMetalLayer* layer,SEL selector) {
    layer.framebufferOnly=NO;
    captured=reinterpret_cast<id<CAMetalDrawable> (*)(id,SEL)>(originalNextDrawable)(layer,selector);
    if(captureReady) {
        captureReady->set_value(); captureReady=nullptr;
        require(captureRelease.wait_for(std::chrono::seconds(5))==std::future_status::ready,
            "Pending-frame observation was not released");
    }
    return captured;
}
struct Capture {
    CAMetalLayer* layer;
    Class originalClass;
    explicit Capture(CAMetalLayer* target):layer(target),originalClass(object_getClass(target)) {
        const auto method=class_getInstanceMethod(originalClass,@selector(nextDrawable));
        originalNextDrawable=method_getImplementation(method);
        auto observed=objc_allocateClassPair(originalClass,"GPU07VideoOutCaptureLayer",0);
        require(observed!=Nil && class_addMethod(observed,@selector(nextDrawable),
            reinterpret_cast<IMP>(captureDrawable),method_getTypeEncoding(method)),"Cannot observe actual drawable");
        objc_registerClassPair(observed);
        require(class_getInstanceSize(observed)==class_getInstanceSize(originalClass),"Capture changed layer layout");
        object_setClass(layer,observed);
    }
    ~Capture() { captured=nil; object_setClass(layer,originalClass); }
};
struct PresentationHold {
    std::promise<void> ready,release;
    std::future<void> entered=ready.get_future();
    bool enabled=std::getenv("APS5_SYNC_FLIP")==nullptr;
    PresentationHold() { if(enabled) { captureRelease=release.get_future().share();captureReady=&ready; } }
    void Resume() { if(enabled) { release.set_value();enabled=false; } }
    ~PresentationHold() { if(enabled) { try { release.set_value(); } catch(...) {} } }
};
struct Completion {
    id<MTLCommandQueue> queue;
    std::promise<void> done;
    std::atomic<unsigned> count{0};
    std::weak_ptr<Backing> owner;
};
std::uint64_t clock(void*) { return 101; }
std::uint64_t counter(void*) { return 202; }
void completed(void* context,VideoOutConfig& config,std::int64_t argument) {
    auto& state=*static_cast<Completion*>(context);
    try {
        require(argument==0x1122334455667788LL && state.count==0 && config.flipStatus.count==0 &&
            config.flipStatus.flipPendingNum==1 && config.bufferPending[3]==1,
            "Completion was published before the actual frame or pending ticket was retired early");
        require(!state.owner.expired(),"Registered display owner retired during presentation");
        require(captured!=nil,"Native presenter did not acquire a real drawable");
        const auto texture=captured.texture;
        require(texture.width>=Width && texture.height>=Height,"Actual drawable has no pixels");
        const auto row=(texture.width*4+255)&~NSUInteger{255};
        auto bytes=[texture.device newBufferWithLength:row*texture.height options:MTLResourceStorageModeShared];
        require(bytes!=nil,"Cannot allocate independent pixel readback");
        std::memset(bytes.contents,0x35,bytes.length);
        auto command=[state.queue commandBuffer]; auto blit=[command blitCommandEncoder];
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0)
            sourceSize:MTLSizeMake(texture.width,texture.height,1) toBuffer:bytes destinationOffset:0
            destinationBytesPerRow:row destinationBytesPerImage:row*texture.height];
        [blit endEncoding]; [command commit]; [command waitUntilCompleted];
        require(command.status==MTLCommandBufferStatusCompleted,"Independent drawable readback failed");
        for (NSUInteger y=0;y<texture.height;++y) {
            for (NSUInteger x=0;x<texture.width;++x)
                require(std::memcmp(static_cast<unsigned char*>(bytes.contents)+y*row+x*4,Golden.data(),4)==0,
                    "Registered-pitch drawable pixel differs from independent golden");
            for (NSUInteger byte=texture.width*4;byte<row;++byte)
                require(static_cast<unsigned char*>(bytes.contents)[y*row+byte]==0x35,"Readback changed padding guard");
        }
        captured=nil; ++state.count; state.done.set_value();
    } catch (...) { state.done.set_exception(std::current_exception()); throw; }
}

struct GuestCaller {
    Cpu::Machine& machine;
    Cpu::SceNativeGraphicsSession& session;
    std::array<std::vector<char>,7> sections;
    GuestCaller(Cpu::Machine& m,Cpu::SceNativeGraphicsSession& s,const char* const* files):machine(m),session(s) {
        m.Map(0x1000,4096,RX); m.Map(0x4000,4096,RX); m.Map(0x5000,4096,RW); m.Map(Control,8192,RW);
        for (std::size_t i=0;i<sections.size();++i) {
            std::ifstream input(files[i],std::ios::binary);
            require(input.good(),"Missing compiler-produced x86 caller");
            sections[i]=std::vector<char>((std::istreambuf_iterator<char>(input)),{});
            require(!sections[i].empty() && sections[i].size()<4096,"Compiled x86 caller exceeded bound");
        }
    }
    std::uint64_t call(unsigned index,const char* nid,const std::array<std::uint64_t,8>& args,bool qualified=false) {
        #ifdef GPU07_OLD_ROUTES
        const auto gate=session.ResolveVideoOut(identity(nid));
#else
        const auto gate=qualified ? session.ResolveVideoOut(identity(nid),2,0) : session.ResolveVideoOutPublicFixture(identity(nid));
#endif
        machine.Write(0x4000,std::as_bytes(std::span(sections[index]))); store(machine,Control,args);
        machine.Set(Cpu::Register::Rdi,gate); machine.Set(Cpu::Register::Rsi,Control);
        machine.Set(Cpu::Register::Rsp,0x5fc8); store(machine,0x5fc8,std::uint64_t{0x1000});
        machine.Set(Cpu::Register::Rbx,0x123456789abcdef0ULL);
        store(machine,0x5f00,std::uint64_t{0xd1d2d3d4d5d6d7d8ULL});
        require(machine.Run(0x4000,0x1000,1000)==Cpu::StopReason::Address,"Compiled x86 VideoOut caller did not return");
        require(machine.Get(Cpu::Register::Rsp)==0x5fd0 && machine.Get(Cpu::Register::Rbx)==0x123456789abcdef0ULL &&
            load<std::uint64_t>(machine,0x5f00)==0xd1d2d3d4d5d6d7d8ULL,"Compiled VideoOut caller corrupted continuation");
        return machine.Get(Cpu::Register::Rax);
    }
};

// Baseline bridge selects the old production mapping route only; all oracle
// assertions and input bytes remain identical across old and fixed runs.
void mutate(Cpu::SceNativeGraphicsSession& session,
    std::span<const AgcDriver::NativeGuestMemory::BorrowedRange> ranges,std::uint64_t generation,
    const std::function<void()>& cpu,std::shared_ptr<const void> previous,std::shared_ptr<const void> next) {
#ifdef GPU07_OLD_ROUTES
    session.Driver().MutateBorrowedRanges(ranges,generation,cpu,std::move(previous),std::move(next));
#else
    session.MutateBorrowedRanges(ranges,generation,cpu,std::move(previous),std::move(next));
#endif
}
void run(const char* utility,const char* const* callers) {
    Cpu::Machine machine;
    auto backing=std::make_shared<Backing>();
    const auto original=std::vector<std::byte>(backing->Bytes().begin(),backing->Bytes().end());
    machine.MapBorrowed(Display,backing->Bytes(),RW);
    auto second=std::make_shared<Backing>(0x32),third=std::make_shared<Backing>(0x64);
    constexpr std::uint64_t display2=Display+0x100000,display3=Display+0x200000;
    machine.MapBorrowed(display2,second->Bytes(),RW); machine.MapBorrowed(display3,third->Bytes(),RW);
    struct InitialOwners { std::shared_ptr<Backing> first,second,third; };
    auto initialOwner=std::make_shared<InitialOwners>(InitialOwners{backing,second,third});
    const std::array initialRanges{
        AgcDriver::NativeGuestMemory::BorrowedRange{Display,backing->Bytes(),false,171},
        AgcDriver::NativeGuestMemory::BorrowedRange{display2,second->Bytes(),false,172},
        AgcDriver::NativeGuestMemory::BorrowedRange{display3,third->Bytes(),false,173}};
    const std::array ranges{initialRanges[0]};
    AnyPS5::Host::NativeMetalSessionConfiguration configuration;
    configuration.window={"GPU07 compiled VideoOut backing fixture",Width,Height};
    configuration.utilityMetallib=utility; configuration.initialRanges=initialRanges;
    configuration.initialRangeOwner=initialOwner; configuration.initialGeneration=17;
    Completion completion; completion.owner=backing;
    auto finished=completion.done.get_future();
    const VideoOutCompletionCallbacks callbacks{&completion,clock,counter,completed};
    #ifdef GPU07_OLD_ROUTES
    auto session=Cpu::SceNativeGraphicsSession::CreateMainThread(machine,configuration,callbacks);
#else
    auto session=Cpu::SceNativeGraphicsSession::CreateMainThread(machine,configuration,callbacks,{},0x7ffdfd000000,
        Cpu::TargetVideoOutAdmissions());
#endif
    auto target=session->Window().Presentation(Width,Height);
    auto layer=(__bridge CAMetalLayer*)target.metalLayer(target.context);
    completion.queue=[layer.device newCommandQueue]; require(completion.queue!=nil,"No actual readback queue");
    Capture capture(layer);
    GuestCaller guest(machine,*session,callers);
    // Qualified production subset: independently allocated three-buffer batch.
    // This establishes target-shaped native registration, not tiled scanout.
    const auto targetHandle=guest.call(0,"Up36PTk687E",{255,0,0,0},true);
    guest.call(3,"PjS5uASwcV8",{Attribute,0x8000000000000000ULL,0,Width,Height,0,0,0},true);
    const std::array<Cpu::SceVideoOutBuffer,3> targetRows{{{Display,0,{}},{display2,0,{}},{display3,0,{}}}};
    store(machine,Buffers,targetRows);
    require(guest.call(4,"rKBUtgRrtbk",{targetHandle,0,0,Buffers,3,Attribute,0,0},true)==0 &&
        guest.call(5,"CBiu4mCE1DA",{targetHandle,0},true)==0 &&
        guest.call(1,"utPrVdxio-8",{targetHandle,Status},true)==0,
        "Qualified compiled three-backing registration/rate/status failed");
    const auto targetStatus=load<Cpu::SceVideoOutStatus>(machine,Status);
    require(targetStatus.Resolution==1 && targetStatus.DynamicRange==1 && targetStatus.RefreshRate==3 &&
        !targetStatus.Flags && targetStatus.Reserved==std::array<std::uint64_t,3>{},
        "Qualified status48 output/padding differs from native state");
    require(guest.call(6,"N5KDtkIjjJ4",{targetHandle,0},true)==0 &&
        guest.call(2,"uquVH4-Du78",{targetHandle},true)==0,"Qualified unregister/close failed");
    mutate(*session,ranges,18,[&]{machine.Unmap(display2,65536);machine.Unmap(display3,65536);},initialOwner,backing);
    initialOwner.reset();second.reset();third.reset();
    const auto handle=guest.call(0,"Up36PTk687E",{0xfedcba98000000ffULL,0,0,0});
    require(handle>0 && handle<=UINT32_MAX,"Compiled open did not return an opaque live handle");
    std::array<std::byte,96> guardedAttribute; guardedAttribute.fill(std::byte{0x7b});
    machine.Write(Attribute-8,guardedAttribute);
    guest.call(3,"PjS5uASwcV8",{Attribute,0x8000000000000000ULL,1,Width,Height,0,0,0});
    auto attribute=load<Cpu::SceVideoOutAttribute>(machine,Attribute);
    require(attribute.Width==Width && attribute.Height==Height && attribute.TilingMode==1 &&
        attribute.PixelFormat==0x8000000000000000ULL && attribute.PitchInPixel==0 && !attribute.Option &&
        !attribute.DccControl && !attribute.DccClearColor && !attribute.Reserved0 && !attribute.Pad0 &&
        attribute.Reserved1==std::array<std::uint64_t,3>{},"Compiled attribute layout/padding differs");
    require(load<std::uint64_t>(machine,Attribute-8)==0x7b7b7b7b7b7b7b7bULL &&
        load<std::uint64_t>(machine,Attribute+80)==0x7b7b7b7b7b7b7b7bULL,"Attribute changed adjacent guest guards");
    // Explicit public fixture extension: source-supported linear pitch, separate
    // from the target caller's observed tiled attribute shape.
    attribute.PitchInPixel=Pitch; store(machine,Attribute,attribute);
    const std::array<Cpu::SceVideoOutBuffer,2> rows{{{Display,0,{}},{Display+0x100000,0,{}}}};
    store(machine,Buffers,rows);
    rejects([&]{guest.call(4,"rKBUtgRrtbk",{handle,2,3,Buffers,2,Attribute,0,0});},"readable guest range");
    require(guest.call(4,"rKBUtgRrtbk",{handle,2,3,Buffers,1,Attribute,0,0})==0,
        "Failed second row partially published first slot or group");
    rejects([&]{guest.call(4,"rKBUtgRrtbk",{handle,1,3,Buffers,1,Attribute,0,0});},"slot is occupied");
    rejects([&]{guest.call(4,"rKBUtgRrtbk",{handle,2,4,Buffers,1,Attribute,0,0});},"set is occupied");
    require(guest.call(5,"CBiu4mCE1DA",{handle,0})==0,"Compiled flip-rate return differs");
    std::array<std::byte,64> statusGuard; statusGuard.fill(std::byte{0x9d}); machine.Write(Status-8,statusGuard);
    machine.Protect(Control+4096,4096,Cpu::Permission::Read);
    rejects([&]{guest.call(1,"utPrVdxio-8",{handle,Status});},"permission");
    machine.Protect(Control+4096,4096,RW);
    require(guest.call(1,"utPrVdxio-8",{handle,Status})==0,"Compiled status return differs");
    const auto status=load<Cpu::SceVideoOutStatus>(machine,Status);
    require(status.Resolution==1 && status.DynamicRange==1 && status.RefreshRate==3 && !status.Flags &&
        status.Reserved==std::array<std::uint64_t,3>{},"Status bytes/padding differ from native output");
    require(load<std::uint64_t>(machine,Status-8)==0x9d9d9d9d9d9d9d9dULL &&
        load<std::uint64_t>(machine,Status+48)==0x9d9d9d9d9d9d9d9dULL,"Status changed adjacent guards");
    const auto statusBefore=load<Cpu::SceVideoOutStatus>(machine,Status);
    rejects([&]{guest.call(1,"utPrVdxio-8",{0x7fffffff,Status});},"handle");
    const auto statusAfter=load<Cpu::SceVideoOutStatus>(machine,Status);
    require(std::memcmp(&statusBefore,&statusAfter,sizeof(statusBefore))==0,
        "Invalid handle modified status output before success");

    auto replacement=std::make_shared<Backing>(0x31);
    const std::array rebound{AgcDriver::NativeGuestMemory::BorrowedRange{Display,replacement->Bytes(),false,171}};
    bool cpuRebound=false;
    rejects([&]{mutate(*session,rebound,19,[&]{
        cpuRebound=true; machine.ReplaceBorrowed(Display,replacement->Bytes(),RW);
    },backing,replacement);},"registered VideoOut backing cannot be retired or rebound");
    require(!cpuRebound && !completion.owner.expired(),
        "Rejected registered backing replacement changed CPU mapping or owner");
    const std::array reidentified{AgcDriver::NativeGuestMemory::BorrowedRange{Display,backing->Bytes(),false,172}};
    rejects([&]{mutate(*session,reidentified,19,[&]{cpuRebound=true;},backing,backing);},
        "registered VideoOut backing cannot be retired or rebound");
    require(!cpuRebound,"Registered identity rejection invoked CPU mutation");

    constexpr std::uint64_t unrelated=0x700000000ULL;
    struct Owners { std::shared_ptr<Backing> display,extra; };
    auto owners=std::make_shared<Owners>(Owners{backing,replacement});
    const std::array augmented{ranges[0],AgcDriver::NativeGuestMemory::BorrowedRange{unrelated,replacement->Bytes(),true,271}};
    mutate(*session,augmented,19,[&]{
        rejects([&]{mutate(*session,augmented,20,[]{},owners,owners);},"mapping callback reentry");
        rejects([&]{guest.call(6,"N5KDtkIjjJ4",{handle,2});},"mapping callback reentry");
        rejects([&]{guest.call(2,"uquVH4-Du78",{handle});},"mapping callback reentry");
        rejects([&]{guest.call(4,"rKBUtgRrtbk",{handle,1,5,Buffers,1,Attribute,0,0});},"mapping callback reentry");
        machine.MapBorrowed(unrelated,replacement->Bytes(),RW);
    },backing,owners);
    require(load<std::uint32_t>(machine,unrelated)==0xff954317,
        "Unrelated mapping generation failed to publish genuine CPU backing");
    configuration.initialRangeOwner.reset(); configuration.initialRanges={};
    std::weak_ptr<Backing> owner=backing; backing.reset();
    require(!owner.expired(),"Native session lost genuine backing ownership");
    const std::array<std::uint32_t,6> flip{AgcDriver::FlipPacketHeader,static_cast<std::uint32_t>(handle),3,1,0x55667788,0x11223344};
    machine.Write(Display+49152,std::as_bytes(std::span(flip)));
    PresentationHold held;
    session->Driver().SubmitCommandBuffer(Display+49152,flip.size(),0,0);
    if(held.enabled) {
        const auto waitUntil=std::chrono::steady_clock::now()+std::chrono::seconds(3);
        while(held.entered.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready &&
            std::chrono::steady_clock::now()<waitUntil) session->Window().PumpMainThread(std::chrono::milliseconds(1));
        require(held.entered.wait_for(std::chrono::milliseconds(0))==std::future_status::ready,
            "Pending-frame observation did not reach real drawable acquisition");
        rejects([&]{guest.call(6,"N5KDtkIjjJ4",{handle,2});},"pending flip");
        rejects([&]{guest.call(2,"uquVH4-Du78",{handle});},"pending flip");
        require(completion.count==0 && !owner.expired(),"Rejected pending retirement completed or released the frame");
        held.Resume();
    }
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(finished.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready &&
        std::chrono::steady_clock::now()<deadline) session->Window().PumpMainThread(std::chrono::milliseconds(1));
    require(finished.wait_for(std::chrono::milliseconds(0))==std::future_status::ready,"No actual frame completion");
    finished.get(); session->Driver().WaitIdle();
    {
        auto retained=owner.lock(); require(retained!=nullptr,"Display owner lost before unregister");
        const auto bytes=retained->Bytes();
        for(std::size_t i=0;i<bytes.size();++i)
            if(i<49152 || i>=49152+sizeof(flip)) require(bytes[i]==original[i],"Presentation changed source/pitch guards");
    }
    require(guest.call(6,"N5KDtkIjjJ4",{handle,2})==0 && guest.call(2,"uquVH4-Du78",{handle})==0,
        "Compiled unregister/close failed after presentation");
    rejects([&]{guest.call(5,"CBiu4mCE1DA",{handle,0});},"handle");
    const auto reopened=guest.call(0,"Up36PTk687E",{255,0,0,0});
    require(reopened!=handle,"Reopened output reused a retired opaque handle");
    rejects([&]{guest.call(6,"N5KDtkIjjJ4",{handle,2});},"handle");
    require(guest.call(2,"uquVH4-Du78",{reopened})==0,"Reopened output failed to close");
    const std::array surviving{augmented[1]};
    mutate(*session,surviving,20,[&]{machine.Unmap(Display,65536);},owners,replacement);
    rejects([&]{machine.CheckAccess(Display,4,Cpu::Permission::Read);},"access denied");
    owners.reset(); replacement.reset();
    session->RequestStop(); session->ShutdownAfterCpuStoppedMainThread();
    require(owner.expired(),"Native session retained backing after drain and teardown");
    require(completion.count==1,"Native route fabricated or repeated completion");
    std::cout<<"PASS compiler-produced x86 callers -> ARM64 TCG -> seven public VideoOut routes; qualified three-distinct-backing registration; high VA; separate public 128-pixel pitch and poisoned padding; actual drawable readback/completion; transaction retry; owner retirement\n";
}
}
int main(int argc,const char* argv[]) {
    @autoreleasepool {
        try { require(argc==9,"VideoOut fixture requires metallib and seven compiled callers"); run(argv[1],argv+2); return 0; }
        catch(const std::exception& error) { std::cerr<<"FAIL "<<error.what()<<'\n'; return 1; }
    }
}
