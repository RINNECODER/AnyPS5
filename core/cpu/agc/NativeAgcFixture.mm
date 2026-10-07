#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cpu/SceAgcImports.hpp>
#include <cpu/SceElf.hpp>
#include "NativeAgcBackend.hpp"
#include "prx/libSceAgcDriver/Execution/include/MetalDriver.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>

// Authoring gate: this fixture owns translated guest import marshalling into
// native shader registration/submission. Existing PM4 replay bypasses Machine
// and import admission. Wrong pointer translation, packed argument extraction,
// cursor updates, relative pointer resolution, or shader/queue routing breaks
// observable readback and guards. No test-only provider entrypoint is used.
namespace {
constexpr auto RW = Cpu::Permission::Read | Cpu::Permission::Write;
constexpr auto RX = Cpu::Permission::Read | Cpu::Permission::Execute;
using Contract = Cpu::AgcAbiContract;
constexpr std::uint64_t Code = 0x500100, Output = 0x510100, Header = 0x520000,
    Builder = 0x530000, Commands = Builder + 256, Descriptor = Builder + 64, ShaderOutput = Builder + 128;
constexpr std::array<std::uint32_t, 24> Wave32Code{
    0x34020084, 0xd765000a, 0x000100c1, 0x3604009f, 0x7d880488, 0xbe880f6a, 0x7e160208, 0xd7600009,
    0x00010700, 0x7e180209, 0xbe9e037e, 0x7e1a0280, 0x7da80488, 0x7e1a0281, 0xbefe031e, 0xe0701000,
    0x80010a01, 0xe0701004, 0x80010b01, 0xe0701008, 0x80010c01, 0xe070100c, 0x80010d01, 0xbf810000};
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F&& function, const char* reason) {
    try { function(); } catch (const std::exception& error) {
        require(std::string(error.what()).find(reason) != std::string::npos, error.what()); return;
    }
    throw std::runtime_error(std::string("Missing AGC rejection: ") + reason);
}
Cpu::SceImport identity(const char* nid, bool driver) {
    Cpu::SceImport value;
    value.Nid=nid; value.LibraryName=driver ? "libSceAgcDriver" : "libSceAgc";
    value.ModuleName=value.LibraryName; value.LibraryId=41; value.ModuleId=42;
    value.LibraryVersion=1; value.ModuleMajor=1; value.ModuleMinor=1;
    return value;
}
template<class T> void store(Cpu::Machine& guest, std::uint64_t address, T value) {
    guest.Write(address, std::as_bytes(std::span(&value, 1)));
}
template<class T> T load(Cpu::Machine& guest, std::uint64_t address) {
    T value{}; guest.Read(address, std::as_writable_bytes(std::span(&value, 1))); return value;
}
struct Guest {
    Cpu::Machine Machine;
    std::unique_ptr<Cpu::SceAgcImports> Imports;
    explicit Guest(Cpu::SceAgcBackend backend, std::span<const Contract> contracts) {
        Machine.Map(0x1000,4096,RX); Machine.Map(0x2000,4096,RW); Machine.Map(0x3000,4096,RW);
        // Real x86 indirect GOT call, followed by a stored return and a preserved
        // callee-saved register continuation. Gate RET uses the CPU's real stack.
        constexpr std::array<std::uint8_t,17> caller{
            0xff,0x15,0xfa,0x0f,0,0, 0x48,0x89,0x05,0x03,0x10,0,0, 0x48,0xff,0xc3,0x90};
        Machine.Write(0x1000,std::as_bytes(std::span(caller)));
        Imports=std::make_unique<Cpu::SceAgcImports>(Machine,std::move(backend),contracts);
    }
    std::uint64_t call(const char* nid, bool driver, std::array<std::uint64_t,8> args={}) {
        constexpr std::array registers{Cpu::Register::Rdi,Cpu::Register::Rsi,Cpu::Register::Rdx,
            Cpu::Register::Rcx,Cpu::Register::R8,Cpu::Register::R9};
        for (std::size_t index=0; index<registers.size(); ++index) Machine.Set(registers[index],args[index]);
        Machine.Set(Cpu::Register::Rax,0xfedcba9876543210);
        Machine.Set(Cpu::Register::Rbx,0x123456789abcdef0);
        Machine.Set(Cpu::Register::Rsp,0x3fc0);
        Machine.Write(0x3fc0,std::as_bytes(std::span(args).last(2)));
        const auto gate=Imports->Resolve(identity(nid,driver),2,0);
        store(Machine,0x2000,gate);
        require(Machine.Run(0x1000,0x1011,100)==Cpu::StopReason::Address,"translated import did not return");
        require(Machine.Get(Cpu::Register::Rsp)==0x3fc0 && Machine.Get(Cpu::Register::Rbx)==0x123456789abcdef1,
                "AGC gate corrupted guest stack/continuation");
        const auto result=load<std::uint64_t>(Machine,0x2010);
        require(result==Machine.Get(Cpu::Register::Rax),"x86 caller failed to store AGC return");
        return result;
    }
};

void Admission() {
    Guest guest({},{});
    rejects([&]{guest.Imports->Resolve(identity("UglJIZjGssM",true),2,0);},"Unqualified");
    rejects([&]{guest.Imports->Resolve(identity("UglJIZjGssM",true),1,0);},"only function");
    rejects([&]{guest.Imports->Resolve(identity("UglJIZjGssM",true),2,8);},"only function");
    auto wrong=identity("UglJIZjGssM",true);wrong.ModuleMinor=2;
    rejects([&]{guest.Imports->Resolve(wrong,2,0);},"scope/version");
    wrong=identity("UglJIZjGssM",false);
    rejects([&]{guest.Imports->Resolve(wrong,2,0);},"scope/version");
    rejects([&]{guest.Imports->Resolve(identity("unsupported",true),2,0);},"Unsupported SCE AGC service");
}

void NativeRoute(id<MTLDevice> device, id<MTLLibrary> library) {
    // TCG aliases require the real complete host-page backing, including on
    // Apple Silicon hosts with 16 KiB pages. Driver-visible ranges stay 4 KiB.
    alignas(65536) std::array<std::byte,65536> codeBytes{},headerBytes{},builderBytes{};
    alignas(65536) std::array<std::uint32_t,16384> output{};
    codeBytes.fill(std::byte{0x7b}); headerBytes.fill(std::byte{0x7b}); builderBytes.fill(std::byte{0x7b});
    output.fill(0xdeadbeef);
    std::memcpy(codeBytes.data()+256,Wave32Code.data(),sizeof(Wave32Code));
    const auto originalCode=codeBytes;
    std::fill_n(headerBytes.begin(),272,std::byte{0});
    std::fill_n(builderBytes.begin(),144,std::byte{0});
    const std::array ranges{
        AgcDriver::NativeGuestMemory::BorrowedRange{0x500000,std::span(codeBytes).first(4096),false},
        AgcDriver::NativeGuestMemory::BorrowedRange{0x510000,std::as_writable_bytes(std::span(output)).first(4096),true},
        AgcDriver::NativeGuestMemory::BorrowedRange{Header,std::span(headerBytes).first(4096),true},
        AgcDriver::NativeGuestMemory::BorrowedRange{Builder,std::span(builderBytes).first(4096),true}};
    const std::array<std::span<std::byte>,4> fullBackings{codeBytes,std::as_writable_bytes(std::span(output)),headerBytes,builderBytes};
    AgcDriver::Metal::MetalDriver driver;
    std::atomic<unsigned> interrupts{0};
    driver.Configure((__bridge void*)device,(__bridge void*)library,ranges,[&](std::uint32_t queue){
        require(queue==0x20,"native provider changed compute EOP queue"); ++interrupts;
    });
    constexpr std::array contracts{
        Contract::CreateShaderRelativeHeader96,Contract::DcbSetShRegisterDirect,Contract::CbDispatchCommandBuffer56,
        Contract::SubmitDcbPacket16,Contract::AgrSubmitDcbPacket16,Contract::SubmitAcbPacket16,
        Contract::DcbWriteDataCommandBuffer56,Contract::AcbWriteDataCommandBuffer56,Contract::SuspendPoint,
        Contract::SubmitMultiDcbs,Contract::AgrSubmitMultiDcbs,Contract::SubmitMultiAcbs};
    Guest guest(Cpu::MakeNativeAgcBackend(driver),contracts);
    for (std::size_t index=0;index<ranges.size();++index) {
        const auto& range=ranges[index];
        guest.Machine.MapBorrowed(range.guestAddress,range.host,
            range.writable ? RW : Cpu::Permission::Read,fullBackings[index]);
    }
    try {
        // Independent 96-byte relative-header layout: pointers encode signed
        // offsets from their own guest field, never from host allocation bytes.
        store(guest.Machine,Header,std::uint32_t{0x34333231});
        store(guest.Machine,Header+4,std::uint32_t{0x18});
        store(guest.Machine,Header+8,std::uint64_t{128-8});
        store(guest.Machine,Header+32,std::uint64_t{256-32});
        store(guest.Machine,Header+64,std::uint32_t{272});
        store(guest.Machine,Header+68,static_cast<std::uint32_t>(sizeof(Wave32Code)));
        store(guest.Machine,Header+92,std::uint8_t{2});
        store(guest.Machine,Header+256,std::uint32_t{0x20c});
        store(guest.Machine,Header+264,std::uint32_t{0x20d});
        const auto preparedHeader=headerBytes;
        const auto preparedBuilder=builderBytes;
        store(guest.Machine,Header,std::uint32_t{0x12345678});
        const auto malformedHeader=headerBytes;
        rejects([&]{guest.call("f3dg2CSgRKY",false,{ShaderOutput,Header,Code});},"invalid relative shader header");
        require(headerBytes==malformedHeader && builderBytes==preparedBuilder,"malformed shader creation changed guest data");
        headerBytes=preparedHeader;
        guest.Machine.Map(0x540000,4096,RW);
        rejects([&]{guest.call("f3dg2CSgRKY",false,{ShaderOutput,Header,0x540000});},"borrowed");
        require(headerBytes==preparedHeader && builderBytes==preparedBuilder,"native shader admission failure changed guest header/output");
        // The production publication boundary must roll back a header if
        // capture rejects after the callback already wrote guest bytes.
        auto native=Cpu::MakeNativeAgcBackend(driver);
        const std::array publicationRanges{Cpu::AgcReadableRange{Header,272},Cpu::AgcReadableRange{Code,sizeof(Wave32Code)}};
        rejects([&]{native.RegisterShader(Header,publicationRanges,
            [&]{store(guest.Machine,Header,std::uint32_t{0x12345678});},
            [&]{guest.Machine.Write(Header,std::span(preparedHeader).first(272));});},"invalid shader header");
        require(headerBytes==preparedHeader && builderBytes==preparedBuilder,"failed native capture did not roll back published header");
        rejects([&]{native.RegisterShader(Header,publicationRanges,
            [&]{driver.SuspendPoint();store(guest.Machine,Header,std::uint32_t{0x12345678});},
            [&]{guest.Machine.Write(Header,std::span(preparedHeader).first(272));});},"reenter");
        require(headerBytes==preparedHeader && builderBytes==preparedBuilder,"native callback reentry changed guest data");
        require(guest.call("f3dg2CSgRKY",false,{ShaderOutput,Header,Code})==0,"shader gate returned error");
        require(load<std::uint64_t>(guest.Machine,ShaderOutput)==Header &&
                load<std::uint64_t>(guest.Machine,Header+8)==Header+128 &&
                load<std::uint64_t>(guest.Machine,Header+32)==Header+256 &&
                load<std::uint64_t>(guest.Machine,Header+16)==Code &&
                load<std::uint32_t>(guest.Machine,Header+260)==(Code>>8),
                "shader gate did not relocate/patch numeric guest fields");
        // Initialize the documented candidate command descriptor by byte offset.
        store(guest.Machine,Builder,Commands);store(guest.Machine,Builder+8,Builder+4096);
        store(guest.Machine,Builder+16,Commands);store(guest.Machine,Builder+24,Builder+4096);
        store(guest.Machine,Builder+48,std::uint32_t{16});
        const auto registerWord=[&](std::uint32_t offset,std::uint32_t value){
            const auto before=load<std::uint64_t>(guest.Machine,Builder+16);
            require(guest.call("pFLArOT53+w",false,{Builder,(std::uint64_t(value)<<32)|offset})==before,
                    "packed ShaderRegister ABI lost returned command address");
        };
        registerWord(0x207,64);registerWord(0x208,1);registerWord(0x209,1);
        registerWord(0x20c,static_cast<std::uint32_t>(Code>>8));registerWord(0x20d,0);
        registerWord(0x213,16);
        const std::array<std::uint32_t,8> userData{0,0,0,0,static_cast<std::uint32_t>(Output),4u<<16,256,0x01016fac};
        for (std::size_t index=0;index<userData.size();++index) registerWord(0x240+static_cast<std::uint32_t>(index),userData[index]);
        const auto dispatchAddress=load<std::uint64_t>(guest.Machine,Builder+16);
        require(guest.call("k3GhuSNmBLU",false,{Builder,1,1,1,0x8000})==dispatchAddress,"dispatch gate address differs");
        const std::array expectedDispatch{0xc0031500u,1u,1u,1u,0x8041u};
        std::array<std::uint32_t,5> dispatch{};
        guest.Machine.Read(dispatchAddress,std::as_writable_bytes(std::span(dispatch)));
        require(dispatch==expectedDispatch,"dispatch builder byte oracle differs");
        const auto end=load<std::uint64_t>(guest.Machine,Builder+16);
        const std::array<std::uint32_t,8> eop{0xc0064900,0,(1u<<29)|(1u<<24),0x510f00,0,0xabcdef12,0,0};
        guest.Machine.Write(end,std::as_bytes(std::span(eop)));
        store(guest.Machine,Descriptor,Commands);
        store(guest.Machine,Descriptor+8,static_cast<std::uint32_t>((end-Commands)/4+eop.size()));
        store(guest.Machine,Descriptor+12,std::uint32_t{0});
        const auto originalBuilder=builderBytes,originalHeader=headerBytes;
        rejects([&]{guest.call("gSRnr79F8tQ",true,{0x58,Descriptor});},"unsupported compute queue");
        require(output[960]==0xdeadbeef && interrupts==0,"invalid queue submitted native work");
        require(guest.call("gSRnr79F8tQ",true,{0x20,Descriptor})==0,"submission gate returned error");
        driver.WaitIdle();
        for (std::uint32_t thread=0;thread<64;++thread) {
            const auto lane=thread%32;
            const std::array<std::uint32_t,4> expected{lane,8,thread-lane+3,lane<8 ? 1u:0u};
            for (std::size_t item=0;item<4;++item)
                require(output[64+thread*4+item]==expected[item],"provider-to-Metal subgroup readback differs");
        }
        for (std::size_t index=0;index<output.size();++index)
            if ((index<64 || index>=320) && index!=960)
                require(output[index]==0xdeadbeef,"provider-to-Metal changed output guard");
        require(output[960]==0xabcdef12 && interrupts==1,"provider did not finish one ordered EOP");
        require(codeBytes==originalCode && builderBytes==originalBuilder && headerBytes==originalHeader,
                "native submit changed guest code/header/commands");
        require(guest.call("h9z6+0hEydk",false)==0,"suspend gate return differs");

        // Both writes must be admitted before a builder changes command bytes.
        const auto originalCommand=load<std::uint32_t>(guest.Machine,Commands);
        constexpr std::uint64_t separateCommands=0x540100;
        std::array<std::byte,4096> separateOriginal{};
        separateOriginal.fill(std::byte{0xa5});
        guest.Machine.Write(0x540000,separateOriginal);
        store(guest.Machine,Builder,std::uint64_t{0x540000});
        store(guest.Machine,Builder+8,std::uint64_t{0x541000});
        store(guest.Machine,Builder+16,separateCommands);
        store(guest.Machine,Builder+24,std::uint64_t{0x541000});
        const auto originalReadOnlyDescriptor=builderBytes;
        guest.Machine.Protect(Builder,4096,Cpu::Permission::Read);
        rejects([&]{guest.call("pFLArOT53+w",false,{Builder,(std::uint64_t{99}<<32)|0x207});},"permission");
        std::array<std::byte,4096> separateAfter{};
        guest.Machine.Read(0x540000,separateAfter);
        require(separateAfter==separateOriginal && builderBytes==originalReadOnlyDescriptor,
                "cursor permission rejection partially modified writable command output");
        guest.Machine.Protect(Builder,4096,RW);
        store(guest.Machine,Builder,Commands);
        store(guest.Machine,Builder+8,Builder+4096);
        store(guest.Machine,Builder+16,Commands);
        store(guest.Machine,Builder+24,Commands+4);
        rejects([&]{guest.call("pFLArOT53+w",false,{Builder,(std::uint64_t{99}<<32)|0x207});},"exhausted");
        require(load<std::uint32_t>(guest.Machine,Commands)==originalCommand,"exhausted builder modified command output");
        store(guest.Machine,Builder+24,Builder+4096);
        const std::array<std::uint32_t,2> payload{0x12345678,0x90abcdef};
        guest.Machine.Write(Builder+160,std::as_bytes(std::span(payload)));
        // The source ABI names this control "increment", but packet bit 16 is
        // WRITE_DATA's fixed-address selector: 0 advances, 1 overwrites the same
        // destination. Use distinct stack controls (advance=0, confirm=1).
        require(guest.call("i1jyy49AjXU",false,{Builder,2,0,Output,Builder+160,2,0,1})==Commands,
                "WRITE_DATA gate stack arguments/return differ");
        constexpr std::array<std::uint32_t,6> expectedWrite{0xc0043700,0x00100100,static_cast<std::uint32_t>(Output),0,0x12345678,0x90abcdef};
        std::array<std::uint32_t,6> written{};
        guest.Machine.Read(Commands,std::as_writable_bytes(std::span(written)));
        require(written==expectedWrite,"WRITE_DATA stack/payload byte oracle differs");
        store(guest.Machine,Descriptor+8,std::uint32_t{6});
        for (const auto* nid:{"UglJIZjGssM","AhGvpITrf4M"}) {
            output[64]=output[65]=0;
            require(guest.call(nid,true,{Descriptor})==0,"DCB/AGR packet return differs");
            driver.WaitIdle();
            require(output[64]==payload[0] && output[65]==payload[1],"DCB/AGR numeric packet submission lost command pointer");
        }
        store(guest.Machine,Builder+176,Commands);store(guest.Machine,Builder+184,std::uint32_t{6});
        for (const auto* nid:{"6UzEidRZwkg","+T8Xo6LtFJI"}) {
            require(guest.call(nid,true,{UINT64_MAX,UINT64_MAX,0})==0,"empty multi DCB inspected arrays");
            output[64]=output[65]=0;
            require(guest.call(nid,true,{Builder+176,Builder+184,1})==0,"multi DCB return differs");
            driver.WaitIdle();
            require(output[64]==payload[0] && output[65]==payload[1],"multi DCB array marshalling lost address/size");
        }
        require(guest.call("HF3YllT3mXU",true,{0x57,UINT64_MAX,UINT64_MAX,0})==0,"empty multi ACB inspected arrays");
        output[64]=output[65]=0;
        require(guest.call("HF3YllT3mXU",true,{0x57,Builder+176,Builder+184,1})==0,"multi ACB return differs");
        driver.WaitIdle();
        require(output[64]==payload[0] && output[65]==payload[1] && interrupts==1,"multi ACB queue/array marshalling differs");
        driver.Shutdown();
    } catch (...) { try { driver.Shutdown(); } catch (...) {} throw; }
    std::cout << "AGC candidate provider: x86 GOT gates -> guest-relative shader creation/registration -> native Metal, 256 independent subgroup results, guards and one EOP; strict admission and builder rejection passed\n";
}
}
int main(int argc,char** argv) {
    @autoreleasepool {
        try {
            require(argc==2,"AGC fixture requires utility metallib path");
            Admission();
            id<MTLDevice> device=MTLCreateSystemDefaultDevice();require(device!=nil,"Metal device unavailable");
            NSError* error=nil;
            const auto url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
            id<MTLLibrary> library=[device newLibraryWithURL:url error:&error];
            require(library!=nil,error ? error.localizedDescription.UTF8String : "utility library unavailable");
            NativeRoute(device,library);
            return 0;
        } catch (const std::exception& error) {std::cerr << error.what() << '\n';return 1;}
    }
}
