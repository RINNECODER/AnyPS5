#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/runtime.h>
#include <cpu/SceNativeVideoOutBackend.hpp>
#include <cpu/SceElf.hpp>
#include "prx/libSceVideoOut/include/NativeMetalSession.hpp"
#include "prx/libSceVideoOut/include/VideoOutState.hpp"
#include "prx/libSceAgcDriver/Execution/include/DisplayBuffer.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

// Primary packed-PQ boundary keeper. Independently specified packed words,
// tiled offsets and poisoned linear rows reach a real drawable through compiled
// x86 callers and genuine native owned mappings. Lower two bits, channel/alpha
// variation and same-layer SDR reset catch truncation, swizzle, stride and
// transfer-metadata regressions missed by the existing uniform SDR keeper.
// No test-only production seam; PQ is passed encoded to the compositor.
namespace {
constexpr auto RW=Cpu::Permission::Read|Cpu::Permission::Write;
constexpr auto RX=Cpu::Permission::Read|Cpu::Permission::Execute;
constexpr std::uint64_t Control=0x100002000ULL,Attribute=Control+128,Rows=Control+256;
constexpr std::uint64_t Display=0x400000000ULL,Format=0x8100070422000000ULL;
constexpr std::uint32_t Width=259,Height=137,Pitch=320;
constexpr std::size_t Storage=6*65536,CommandOffset=Storage-64;
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class F>void rejects(F&& action,const char* expected){
    try{action();}catch(const std::exception& error){require(std::string(error.what()).find(expected)!=std::string::npos,error.what());return;}
    throw std::runtime_error(std::string("Missing native packed-format rejection: ")+expected);
}
template<class T>void store(Cpu::Machine& m,std::uint64_t address,const T& value){m.Write(address,std::as_bytes(std::span(&value,1)));}
std::size_t tiled(std::uint32_t x,std::uint32_t y){
    // Bit-contribution oracle retained from the independent public tile fixture;
    // deliberately does not call the production decoder or its address helper.
    constexpr std::array<unsigned,7> xs{4,8,128,256,0x2200,0x800,0x8400};
    constexpr std::array<unsigned,7> ys{16,32,64,0x1100,0x200,0x400,0x4800};
    unsigned offset=0;for(unsigned i=0;i<7;++i){if(x&(1u<<i))offset^=xs[i];if(y&(1u<<i))offset^=ys[i];}
    return (static_cast<std::size_t>(y/128)*3+x/128)*65536+offset;
}
struct Codes{std::uint32_t r,g,b,a;};
Codes codes(unsigned x,unsigned y){
    constexpr std::array<unsigned,8> knots{0,1,64,256,512,768,1022,1023};
    // Stable blocks leave independently testable sample interiors when the
    // production presenter linearly scales to a Retina drawable. Every channel
    // and alpha is constant within a block; distinct blocks retain low bits.
    const auto blockX=x/8,blockY=y/8;
    return {knots[(blockX+blockY)%8],(29*blockX+7*blockY+257)%1024,
        (17*blockX+31*blockY+769)%1024,(blockX+blockY)%4};
}
std::uint32_t guestWord(Codes c){return c.r|(c.g<<10)|(c.b<<20)|(c.a<<30);}
std::uint32_t nativeWord(Codes c){return c.b|(c.g<<10)|(c.r<<20)|(c.a<<30);}
struct Backing{
    std::unique_ptr<std::byte,decltype(&std::free)> data{nullptr,&std::free};
    explicit Backing(bool linear){void* allocation=nullptr;require(posix_memalign(&allocation,65536,Storage)==0,"Cannot allocate genuine HDR backing");
        data.reset(static_cast<std::byte*>(allocation));std::memset(data.get(),0xa7,Storage);
        for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x){const auto word=guestWord(codes(x,y));
            const auto offset=linear?(static_cast<std::size_t>(y)*Pitch+x)*4:tiled(x,y);std::memcpy(data.get()+offset,&word,4);}}
    std::span<std::byte> bytes(){return {data.get(),Storage};}
};
Cpu::SceImport identity(const char* nid){Cpu::SceImport i;i.Nid=nid;i.LibraryName=i.ModuleName="libSceVideoOut";
    i.LibraryVersion=1;i.ModuleMajor=i.ModuleMinor=1;i.LibraryId=39;i.ModuleId=40;return i;}
struct Caller{
    Cpu::Machine& m;Cpu::SceNativeGraphicsSession& s;std::array<std::vector<char>,7> sections;
    Caller(Cpu::Machine& machine,Cpu::SceNativeGraphicsSession& session,const char* const* files):m(machine),s(session){
        m.Map(0x1000,4096,RX);m.Map(0x4000,4096,RX);m.Map(0x5000,4096,RW);m.Map(Control,4096,RW);
        for(unsigned i=0;i<7;++i){std::ifstream input(files[i],std::ios::binary);require(input.good(),"Missing compiler-produced VideoOut caller");
            sections[i]=std::vector<char>((std::istreambuf_iterator<char>(input)),{});require(!sections[i].empty()&&sections[i].size()<4096,"Compiled caller bound differs");}}
    std::uint64_t call(unsigned index,const char* nid,std::array<std::uint64_t,8> a,bool qualified){
        const auto gate=qualified?s.ResolveVideoOut(identity(nid),2,0):s.ResolveVideoOutPublicFixture(identity(nid));
        m.Write(0x4000,std::as_bytes(std::span(sections[index])));store(m,Control,a);
        m.Set(Cpu::Register::Rdi,gate);m.Set(Cpu::Register::Rsi,Control);m.Set(Cpu::Register::Rsp,0x5fc8);store(m,0x5fc8,std::uint64_t{0x1000});
        require(m.Run(0x4000,0x1000,1000)==Cpu::StopReason::Address&&m.Get(Cpu::Register::Rsp)==0x5fd0,"Compiled packed-PQ caller did not return");
        return m.Get(Cpu::Register::Rax);}
};
id<CAMetalDrawable> captured=nil;IMP originalNext=nullptr;bool expectedHdr=true,expectedBlack=false;
std::mutex frameErrorMutex;std::exception_ptr frameError;
std::exception_ptr observedFrameError(){std::lock_guard lock(frameErrorMutex);return frameError;}
id<CAMetalDrawable> nextDrawable(CAMetalLayer* layer,SEL selector){
    try{
    require(layer.pixelFormat==(expectedHdr?MTLPixelFormatBGR10A2Unorm:MTLPixelFormatBGRA8Unorm),"Native drawable format lost packed ten-bit storage or SDR reset");
    require(layer.wantsExtendedDynamicRangeContent==expectedHdr,"PQ EDR handoff or SDR reset differs");
    require(!expectedBlack||layer.opaque,"Synthetic native black clear lost its opaque contract");
    if(expectedHdr){const auto space=layer.colorspace;require(space!=nullptr,"Packed PQ layer lost transfer/primaries metadata");
        const auto name=CGColorSpaceCopyName(space);const bool matches=name&&CFEqual(name,kCGColorSpaceITUR_2100_PQ);if(name)CFRelease(name);
        require(matches,"Native layer is not the independently named BT2100 PQ colorspace");}
    else require(layer.colorspace==nullptr,"SDR layer retained HDR transfer metadata");
    require(layer.EDRMetadata==nil,"Fixture acquired invented HDR mastering metadata");
    layer.framebufferOnly=NO;captured=reinterpret_cast<id<CAMetalDrawable>(*)(id,SEL)>(originalNext)(layer,selector);return captured;
    }catch(...){
        // Metadata assertions occur before the production completion callback.
        // Preserve their exact failure for the main fixture instead of leaving
        // its completion future to report an unrelated timeout.
        std::lock_guard lock(frameErrorMutex);if(!frameError)frameError=std::current_exception();throw;
    }
}
struct Capture{
    CAMetalLayer* layer;Class prior;
    explicit Capture(CAMetalLayer* l):layer(l),prior(object_getClass(l)){
        const auto method=class_getInstanceMethod(prior,@selector(nextDrawable));originalNext=method_getImplementation(method);
        const auto observed=objc_allocateClassPair(prior,"GPU09PackedPqCaptureLayer",0);
        require(observed&&class_addMethod(observed,@selector(nextDrawable),reinterpret_cast<IMP>(nextDrawable),method_getTypeEncoding(method)),"Cannot observe native HDR drawable");
        objc_registerClassPair(observed);require(class_getInstanceSize(observed)==class_getInstanceSize(prior),"Capture changed native layer layout");object_setClass(layer,observed);}
    ~Capture(){captured=nil;object_setClass(layer,prior);}
};
struct Completion{
    id<MTLCommandQueue> queue;std::promise<void> done;unsigned frames=0;std::uint64_t priorCount=0;std::weak_ptr<Backing> owner;
};
std::uint64_t clock(void*){return 101;}std::uint64_t counter(void*){return 202;}
void completed(void* context,VideoOutConfig& config,std::int64_t argument){
    auto& c=*static_cast<Completion*>(context);
    try{require(argument==37&&config.flipStatus.count==c.priorCount&&config.flipStatus.flipPendingNum==1&&!c.owner.expired(),"Packed HDR frame completed with wrong lifetime or ticket");
        require(captured!=nil,"Packed HDR native presenter acquired no actual drawable");const auto texture=captured.texture;
        require(texture.width>=Width&&texture.height>=Height&&texture.width%Width==0&&texture.height%Height==0&&texture.width/Width==texture.height/Height,"Drawable scale differs from independent pixel oracle");
        const auto row=(texture.width*4+255)&~NSUInteger{255};
        const auto bytes=[texture.device newBufferWithLength:row*texture.height options:MTLResourceStorageModeShared];require(bytes!=nil,"Cannot allocate independent HDR readback");
        std::memset(bytes.contents,0x35,bytes.length);auto command=[c.queue commandBuffer];auto blit=[command blitCommandEncoder];
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(texture.width,texture.height,1)
            toBuffer:bytes destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row*texture.height];
        [blit endEncoding];[command commit];[command waitUntilCompleted];require(command.status==MTLCommandBufferStatusCompleted,"Independent HDR readback failed");
        const auto scale=texture.width/Width;NSUInteger checkedPixels=0;
        for(NSUInteger y=0;y<texture.height;++y){for(NSUInteger x=0;x<texture.width;++x){
            const auto sourceX=x/scale,sourceY=y/scale;
            // Check only the middle four source pixels of each stable 8x8
            // block. Their filter footprint cannot touch another code block;
            // no copied production interpolation algorithm supplies a golden.
            if(expectedHdr&&!expectedBlack&&(sourceX%8<2||sourceX%8>5||sourceY%8<2||sourceY%8>5))continue;
            std::uint32_t actual=0;
            std::memcpy(&actual,static_cast<unsigned char*>(bytes.contents)+y*row+x*4,4);
            const auto expected=expectedBlack?0xc0000000u:(expectedHdr?nativeWord(codes(sourceX,sourceY)):0xff954317u);
            require(actual==expected,"Native packed PQ drawable differs from independent full-ten-bit/channel/alpha golden");++checkedPixels;}
            for(NSUInteger b=texture.width*4;b<row;++b)require(static_cast<unsigned char*>(bytes.contents)[y*row+b]==0x35,"Native HDR readback overwrote padding");}
        require(checkedPixels>texture.width*texture.height/8,"Packed PQ drawable oracle checked too few block-interior pixels");
        captured=nil;++c.frames;c.done.set_value();
    }catch(...){c.done.set_exception(std::current_exception());throw;}
}
void present(Cpu::Machine& m,Cpu::SceNativeGraphicsSession& s,Completion& c,std::uint32_t handle,unsigned index,std::uint64_t priorCount=0){
    {std::lock_guard lock(frameErrorMutex);frameError=nullptr;}
    c.priorCount=priorCount;c.done=std::promise<void>{};auto finished=c.done.get_future();
    const std::array<std::uint32_t,6> packet{AgcDriver::FlipPacketHeader,handle,index,1,37,0};
    m.Write(Display+3*0x100000+CommandOffset,std::as_bytes(std::span(packet)));
    s.Driver().SubmitCommandBuffer(Display+3*0x100000+CommandOffset,packet.size(),0,0);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
    while(finished.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready&&std::chrono::steady_clock::now()<deadline){
        if(const auto error=observedFrameError())std::rethrow_exception(error);
        s.Window().PumpMainThread(std::chrono::milliseconds(1));
    }
    if(const auto error=observedFrameError())std::rethrow_exception(error);
    require(finished.wait_for(std::chrono::milliseconds(0))==std::future_status::ready,"No actual packed-PQ frame completion");finished.get();s.Driver().WaitIdle();
}
void run(const char* utility,const char* const* files){
    Cpu::Machine machine;auto owners=std::make_shared<std::array<std::shared_ptr<Backing>,4>>();
    std::array<AgcDriver::NativeGuestMemory::BorrowedRange,4> ranges;
    for(unsigned i=0;i<4;++i){(*owners)[i]=std::make_shared<Backing>(i==3);const auto address=Display+i*0x100000;
        machine.MapBorrowed(address,(*owners)[i]->bytes(),RW);ranges[i]={address,(*owners)[i]->bytes(),false,171+i};}
    AnyPS5::Host::NativeMetalSessionConfiguration config;config.window={"GPU09 encoded packed PQ fixture",Width,Height};
    config.utilityMetallib=utility;config.initialRanges=ranges;config.initialRangeOwner=owners;config.initialGeneration=17;
    Completion completion;completion.owner=(*owners)[0];const VideoOutCompletionCallbacks callbacks{&completion,clock,counter,completed};
    auto session=Cpu::SceNativeGraphicsSession::CreateMainThread(machine,config,callbacks,{},0x7ffdfd000000,
        Cpu::QualifiedVideoOutAdmissionsForImage("a6df51ec222136f337f86e9be5fa3013417ddc44bc22a6c8d514c0199cf8c397"));
    const auto target=session->Window().Presentation(Width,Height);auto layer=(__bridge CAMetalLayer*)target.metalLayer(target.context);
    completion.queue=[layer.device newCommandQueue];require(completion.queue!=nil,"Cannot allocate actual native HDR readback queue");Capture capture(layer);Caller guest(machine,*session,files);
    const auto handle=guest.call(0,"Up36PTk687E",{255,0,0,0},true);
    guest.call(3,"PjS5uASwcV8",{Attribute,Format,0,Width,Height,0,0,0},true);
    const std::array<Cpu::SceVideoOutBuffer,3> rows{{{Display,0,{}},{Display+0x100000,0,{}},{Display+0x200000,0,{}}}};store(machine,Rows,rows);
    require(guest.call(4,"rKBUtgRrtbk",{handle,0,0,Rows,3,Attribute,0,0},true)==0,"Exact qualified packed PQ three-buffer native registration failed");
    // The native format owner independently fails closed, even without the
    // qualified guest gate. Storage footprint is four bytes, with six tiles.
    const AgcDriver::DisplayBuffer tiledDescription{Display,Format,Width,Height};
    require(AgcDriver::DisplayBufferSize(tiledDescription)==Storage,"Packed PQ tiled storage footprint differs from independent six-tile oracle");
    const AgcDriver::DisplayBuffer linearDescription{Display+3*0x100000,Format,Width,Height,1,Pitch};
    require(AgcDriver::DisplayBufferSize(linearDescription)==static_cast<std::size_t>(Pitch)*Height*4,"Packed PQ linear footprint lost four-byte texel or row pitch");
    for(const auto format:std::array<std::uint64_t,3>{0x8100070422000001ULL,0x8100070522000000ULL,0x8100060422000000ULL}){
        auto unknown=tiledDescription;unknown.pixelFormat=format;
        rejects([&]{AgcDriver::DisplayBufferSize(unknown);},"unsupported display pixel format");
        rejects([&]{AgcDriver::DecodeDisplayBufferPqHdr(unknown,(*owners)[0]->bytes());},"exact qualified packed HDR format");
    }
    auto compressed=tiledDescription;compressed.dccAddress=Display+0x80000;
    rejects([&]{AgcDriver::DisplayBufferSize(compressed);},"packed PQ HDR DCC is unsupported");
    rejects([&]{AgcDriver::DecodeDisplayBuffer(tiledDescription,(*owners)[0]->bytes());},"native PQ presentation path");
    rejects([&]{AgcDriver::DecodeDisplayBufferPqHdr(tiledDescription,(*owners)[0]->bytes().first(Storage-1));},"invalid display buffer size");
    const auto original=std::vector<std::byte>((*owners)[0]->bytes().begin(),(*owners)[0]->bytes().end());
    present(machine,*session,completion,handle,0);require(std::equal(original.begin(),original.end(),(*owners)[0]->bytes().begin()),"Tiled HDR presentation altered source or tile padding");
    // Existing synthetic internal BLACK=-2 emits an opaque native clear. This
    // does not admit negative indices to the qualified target flip producer.
    // A null-buffer clear must preserve the active packed PQ layer identity.
    expectedBlack=true;
    present(machine,*session,completion,handle,static_cast<std::uint32_t>(VIDEO_OUT_BUFFER_INDEX_BLACK),1);
    require(completion.frames==2&&!completion.owner.expired(),"Synthetic native black clear lost registered owner or second completion");
    require(std::equal(original.begin(),original.end(),(*owners)[0]->bytes().begin()),"Synthetic black clear altered retained HDR backing");
    expectedBlack=false;
    require(guest.call(6,"N5KDtkIjjJ4",{handle,0},true)==0&&guest.call(2,"uquVH4-Du78",{handle},true)==0,"Qualified HDR backing did not retire after completion");
    const auto publicHandle=guest.call(0,"Up36PTk687E",{255,0,0,0},false);
    guest.call(3,"PjS5uASwcV8",{Attribute,Format,1,Width,Height,0,0,0},false);
    Cpu::SceVideoOutAttribute attr;machine.Read(Attribute,std::as_writable_bytes(std::span(&attr,1)));attr.PitchInPixel=Pitch;store(machine,Attribute,attr);
    const Cpu::SceVideoOutBuffer row{Display+3*0x100000,0,{}};store(machine,Rows,row);
    require(guest.call(4,"rKBUtgRrtbk",{publicHandle,2,3,Rows,1,Attribute,0,0},false)==0,"Public padded linear HDR registration failed");
    completion.owner=(*owners)[3];const auto linearBefore=std::vector<std::byte>((*owners)[3]->bytes().begin(),(*owners)[3]->bytes().end());
    present(machine,*session,completion,publicHandle,3);
    for(std::size_t i=0;i<Storage;++i)if(i<CommandOffset||i>=CommandOffset+24)require((*owners)[3]->bytes()[i]==linearBefore[i],"Packed HDR presentation changed source or poisoned row padding");
    require(guest.call(6,"N5KDtkIjjJ4",{publicHandle,2},false)==0&&guest.call(2,"uquVH4-Du78",{publicHandle},false)==0,"Public HDR registration did not retire");
    // Same real layer must drop PQ/EDR metadata for the unchanged SDR route.
    expectedHdr=false;for(unsigned y=0;y<Height;++y)for(unsigned x=0;x<Width;++x){const std::uint32_t sdr=0xff954317u;std::memcpy((*owners)[3]->data.get()+(static_cast<std::size_t>(y)*Pitch+x)*4,&sdr,4);}
    const auto sdrHandle=guest.call(0,"Up36PTk687E",{255,0,0,0},false);attr.PixelFormat=0x8000000000000000ULL;store(machine,Attribute,attr);
    require(guest.call(4,"rKBUtgRrtbk",{sdrHandle,2,3,Rows,1,Attribute,0,0},false)==0,"SDR reset registration failed");present(machine,*session,completion,sdrHandle,3);
    require(guest.call(6,"N5KDtkIjjJ4",{sdrHandle,2},false)==0&&guest.call(2,"uquVH4-Du78",{sdrHandle},false)==0,"SDR reset registration failed to retire");
    session->RequestStop();session->ShutdownAfterCpuStoppedMainThread();require(completion.frames==4,"Packed-PQ route fabricated or repeated completion");
    std::cout<<"PASS compiled exact target HDR tiled registration -> actual BGR10A2 drawable; full ten-bit asymmetric channels/alpha/endpoints; synthetic internal opaque black retains PQ/EDR; separate public padded linear HDR; source/tile/readback guards; named BT2100 PQ/EDR handoff; same-layer SDR reset; no physical HDR claim\n";
}
}
int main(int argc,const char* argv[]){@autoreleasepool{try{require(argc==9,"Packed HDR fixture requires metallib and seven compiled callers");run(argv[1],argv+2);return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}}
