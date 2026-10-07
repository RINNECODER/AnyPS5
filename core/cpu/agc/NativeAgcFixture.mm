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
#include <fstream>
#include <vector>
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
        Machine.Map(0x1000,4096,RX); Machine.Map(0x2000,4096,RW); Machine.Map(0x3000,4096,RW); Machine.Map(0x4000,4096,RX);
        // Real x86 indirect GOT call, followed by a stored return and a preserved
        // callee-saved register continuation. Gate RET uses the CPU's real stack.
        constexpr std::array<std::uint8_t,17> caller{
            0xff,0x15,0xfa,0x0f,0,0, 0x48,0x89,0x05,0x03,0x10,0,0, 0x48,0xff,0xc3,0x90};
        Machine.Write(0x1000,std::as_bytes(std::span(caller)));
        Imports=std::make_unique<Cpu::SceAgcImports>(Machine,std::move(backend),contracts);
    }
    std::uint64_t compiled(const char* file, std::array<std::uint64_t,6> args) {
        std::ifstream input(file, std::ios::binary);
        require(input.good(), "compiled target caller missing");
        const std::vector<char> bytes((std::istreambuf_iterator<char>(input)), {});
        require(!bytes.empty() && bytes.size() <= 4096, "compiled target caller outside code-page bound");
        Machine.Write(0x4000, std::as_bytes(std::span(bytes)));
        constexpr std::array registers{Cpu::Register::Rdi, Cpu::Register::Rsi, Cpu::Register::Rdx,
            Cpu::Register::Rcx, Cpu::Register::R8, Cpu::Register::R9};
        for (std::size_t i=0; i<args.size(); ++i) Machine.Set(registers[i], args[i]);
        Machine.Set(Cpu::Register::Rsp, 0x3fc8);
        store(Machine, 0x3fc8, std::uint64_t{0x1011});
        require(Machine.Run(0x4000, 0x1011, 1000) == Cpu::StopReason::Address,
                "compiled target caller did not return");
        require(Machine.Get(Cpu::Register::Rsp) == 0x3fd0, "compiled target caller corrupted stack");
        return Machine.Get(Cpu::Register::Rax);
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

void NativeRoute(id<MTLDevice> device, id<MTLLibrary> library, char** callers) {
    // TCG aliases require the real complete host-page backing, including on
    // Apple Silicon hosts with 16 KiB pages. Driver-visible ranges stay 4 KiB.
    alignas(65536) std::array<std::byte,65536> codeBytes{},headerBytes{},builderBytes{},targetBuilderBytes{};
    alignas(65536) std::array<std::uint32_t,16384> output{},targetOutput{};
    codeBytes.fill(std::byte{0x7b}); headerBytes.fill(std::byte{0x7b}); builderBytes.fill(std::byte{0x7b});
    output.fill(0xdeadbeef); targetOutput.fill(0xdeadbeef); targetBuilderBytes.fill(std::byte{0x7b});
    std::memcpy(codeBytes.data()+256,Wave32Code.data(),sizeof(Wave32Code));
    const auto originalCode=codeBytes;
    std::fill_n(headerBytes.begin(),272,std::byte{0});
    std::fill_n(builderBytes.begin(),144,std::byte{0});
    const std::array ranges{
        AgcDriver::NativeGuestMemory::BorrowedRange{0x500000,std::span(codeBytes).first(4096),false},
        AgcDriver::NativeGuestMemory::BorrowedRange{0x510000,std::as_writable_bytes(std::span(output)).first(4096),true},
        AgcDriver::NativeGuestMemory::BorrowedRange{Header,std::span(headerBytes).first(4096),true},
        AgcDriver::NativeGuestMemory::BorrowedRange{Builder,std::span(builderBytes).first(4096),true},
        AgcDriver::NativeGuestMemory::BorrowedRange{Output-256+0x100000000ull,std::as_writable_bytes(std::span(targetOutput)).first(4096),true},
        AgcDriver::NativeGuestMemory::BorrowedRange{Builder+0x100000000ull,std::span(targetBuilderBytes).first(4096),true}};
    const std::array<std::span<std::byte>,6> fullBackings{codeBytes,std::as_writable_bytes(std::span(output)),headerBytes,builderBytes,
        std::as_writable_bytes(std::span(targetOutput)),targetBuilderBytes};
    AgcDriver::Metal::MetalDriver driver;
    std::atomic<unsigned> interrupts{0};
    std::atomic<std::uint32_t> lastQueue{UINT32_MAX};
    driver.Configure((__bridge void*)device,(__bridge void*)library,ranges,[&](std::uint32_t queue){
        require(queue==0x20 || queue==0,"native provider changed EOP queue"); lastQueue=queue; ++interrupts;
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
        require(output[960]==0xabcdef12 && interrupts==1 && lastQueue==0x20,"provider did not finish one ordered EOP");
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
        // Compiler-produced x86 callers now exercise the independently observed
        // target shapes. The earlier synthetic controls remain above unchanged.
        auto sourceImports=std::move(guest.Imports);
        constexpr auto targetBuilder=Builder+0x100000000ull, targetCommandsAddress=Commands+0x100000000ull;
        constexpr auto targetDescriptor=targetBuilder+64, targetShaderOutput=targetBuilder+128;
        const std::array<std::uint32_t,8> targetEop{0xc0064900,0,(1u<<29)|(1u<<24),0x510f00,1,0xabcdef12,0,0};
        std::copy(builderBytes.begin(),builderBytes.end(),targetBuilderBytes.begin());
        // Low command memory is a distinct, valid NOP stream. A native pointer
        // truncation therefore fails the output/EOP oracle, not an unrelated
        // packet-format guard, and cannot silently use aliased high bytes.
        std::array<std::uint32_t,76> decoy{}; decoy[0]=0xc04a1000;
        guest.Machine.Write(Commands,std::as_bytes(std::span(decoy)));
        constexpr auto targetHash="a6df51ec222136f337f86e9be5fa3013417ddc44bc22a6c8d514c0199cf8c397";
        guest.Imports=std::make_unique<Cpu::SceAgcImports>(guest.Machine, Cpu::MakeNativeAgcBackend(driver),
            Cpu::QualifiedAgcAdmissionsForImage(targetHash), 0x7ffdf3001000);
        require(Cpu::QualifiedAgcAdmissionsForImage("different-image").empty(), "unknown image gained target admission");
        rejects([&]{guest.Imports->Resolve(identity("HF3YllT3mXU",true),2,0);},"Unqualified");
        rejects([&]{guest.Imports->Resolve(identity("pFLArOT53+w",false),2,0);},"Unqualified");

        // Reconstruct the corroborated 304-byte compute geometry from public
        // types and independent offsets, using our synthetic ISA. No retail
        // shader or binary bytes are copied. Zero-count sharp tables point one
        // past the header, as in the inspected target headers.
        std::fill_n(headerBytes.begin(),304,std::byte{0});
        store(guest.Machine,Header,std::uint32_t{0x34333231});
        store(guest.Machine,Header+4,std::uint32_t{0x18});
        store(guest.Machine,Header+8,std::uint64_t{224-8});
        store(guest.Machine,Header+32,std::uint64_t{144-32});
        store(guest.Machine,Header+40,std::uint64_t{96-40});
        store(guest.Machine,Header+64,std::uint32_t{304});
        store(guest.Machine,Header+68,static_cast<std::uint32_t>(sizeof(Wave32Code)));
        store(guest.Machine,Header+76,std::uint32_t{14});
        store(guest.Machine,Header+88,std::uint16_t{48});
        store(guest.Machine,Header+92,std::uint8_t{10});
        store(guest.Machine,Header+112,std::uint32_t{1});
        for (std::uint32_t i=0;i<10;++i) store(guest.Machine,Header+144+i*8, std::uint32_t{0x20c+i});
        store(guest.Machine,Header+224,std::uint64_t{280-224});
        store(guest.Machine,Header+268,std::uint16_t{11});
        for (std::uint32_t i=0;i<4;++i) store(guest.Machine,Header+232+i*8,std::uint64_t{304-(232+i*8)});
        const auto relativeTargetHeader=headerBytes;
        const auto shaderGate=guest.Imports->Resolve(identity("f3dg2CSgRKY",false),2,0);
        require(guest.compiled(callers[2],{shaderGate,targetShaderOutput,Header,Code})==0,"compiled shader caller return differs");
        require(load<std::uint64_t>(guest.Machine,targetShaderOutput)==Header &&
            load<std::uint64_t>(guest.Machine,Header+8)==Header+224 &&
            load<std::uint64_t>(guest.Machine,Header+224)==Header+280 &&
            load<std::uint64_t>(guest.Machine,Header+256)==Header+304 &&
            load<std::uint32_t>(guest.Machine,Header+148)==(Code>>8), "target header304 relative publication differs");
        require(std::equal(headerBytes.begin()+304,headerBytes.end(),relativeTargetHeader.begin()+304),
            "target shader publication changed header redzone");

        store(guest.Machine,targetBuilder,targetCommandsAddress); store(guest.Machine,targetBuilder+8,targetBuilder+4096);
        store(guest.Machine,targetBuilder+16,targetCommandsAddress); store(guest.Machine,targetBuilder+24,targetBuilder+4096);
        store(guest.Machine,targetBuilder+32,std::uint64_t{0x123456789abcdef0});
        store(guest.Machine,targetBuilder+40,std::uint64_t{0xfedcba9876543210});
        store(guest.Machine,targetBuilder+48,std::uint32_t{16});
        constexpr auto targetDestination=Output+0x800+0x100000000ull;
        std::array<std::uint32_t,64> zeros{};
        guest.Machine.Write(targetBuilder+0xc00,std::as_bytes(std::span(zeros)));
        const auto writeGate=guest.Imports->Resolve(identity("i1jyy49AjXU",false),2,0);
        const auto preservedDescriptor=targetBuilderBytes;
        require(guest.compiled(callers[0],{writeGate,targetBuilder,targetDestination,targetBuilder+0xc00})==targetCommandsAddress,
            "compiled target WRITE_DATA return differs");
        std::array<std::uint32_t,68> targetPacket{};
        guest.Machine.Read(targetCommandsAddress,std::as_writable_bytes(std::span(targetPacket)));
        require(targetPacket[0]==0xc0423700 && targetPacket[1]==0x40100200 &&
            targetPacket[2]==static_cast<std::uint32_t>(targetDestination) && targetPacket[3]==1 &&
            std::all_of(targetPacket.begin()+4,targetPacket.end(),[](auto value){return value==0;}) &&
            load<std::uint64_t>(guest.Machine,targetBuilder+16)==targetCommandsAddress+272,
            "compiled target WRITE_DATA count64/dst5 oracle differs");
        require(std::equal(targetBuilderBytes.begin()+32,targetBuilderBytes.begin()+56,preservedDescriptor.begin()+32),
            "target builder changed callback/userdata/reserved fields");
        guest.Machine.Write(targetCommandsAddress+272,std::as_bytes(std::span(targetEop)));
        const auto capturedTargetCommands=std::span(targetBuilderBytes).subspan(256,304);
        const std::vector<std::byte> targetCommands(capturedTargetCommands.begin(),capturedTargetCommands.end());
        for (const auto* nid:{"UglJIZjGssM","AhGvpITrf4M"}) {
            guest.Machine.Write(targetCommandsAddress,targetCommands);
            std::fill(targetOutput.begin()+576,targetOutput.begin()+640,0xeeeeeeee);
            const auto expectedInterrupts=interrupts.load()+1;
            const auto gate=guest.Imports->Resolve(identity(nid,true),2,0);
            require(guest.compiled(callers[1],{gate,targetCommandsAddress,76})==0,"compiled packet16 submission return differs");
            // Both the compiled local descriptor and command storage are reused
            // before WaitIdle. Native capture must already own the command copy.
            std::fill_n(targetBuilderBytes.begin()+256,304,std::byte{0xcc});
            driver.WaitIdle();
            require(std::all_of(targetOutput.begin()+576,targetOutput.begin()+640,[](auto value){return value==0;}) &&
                targetOutput[575]==0xdeadbeef && targetOutput[640]==0xdeadbeef && targetOutput[960]==0xabcdef12 &&
                interrupts==expectedInterrupts && lastQueue==0,
                "target packet snapshot lifetime/output64/EOP differs");
        }
        store(guest.Machine,targetBuilder+16,targetCommandsAddress);
        const auto acbWriteGate=guest.Imports->Resolve(identity("eZ4+17OQz4Q",false),2,0);
        require(guest.compiled(callers[4],{acbWriteGate,targetBuilder,targetDestination+0x100,targetBuilder+160})==targetCommandsAddress,
            "compiled target ACB WRITE_DATA return differs");
        std::array<std::uint32_t,6> targetAcb{};
        guest.Machine.Read(targetCommandsAddress,std::as_writable_bytes(std::span(targetAcb)));
        require(targetAcb==std::array{0xc0043700u,0x00100200u,static_cast<std::uint32_t>(targetDestination+0x100),1u,
            0x12345678u,0x90abcdefu},"target ACB WRITE_DATA packet oracle differs");
        guest.Machine.Write(targetCommandsAddress+24,std::as_bytes(std::span(targetEop)));
        store(guest.Machine,targetDescriptor,targetCommandsAddress); store(guest.Machine,targetDescriptor+8,std::uint32_t{14});
        store(guest.Machine,targetDescriptor+12,std::uint32_t{0xa55af100});
        require(guest.call("gSRnr79F8tQ",true,{0x20,targetDescriptor})==0,"target ACB WRITE_DATA submit return differs");
        driver.WaitIdle();
        require(targetOutput[640]==0x12345678 && targetOutput[641]==0x90abcdef && targetOutput[639]==0 && targetOutput[642]==0xdeadbeef &&
            interrupts==4 && lastQueue==0x20,"target ACB WRITE_DATA output/EOP differs");
        store(guest.Machine,targetBuilder+16,targetCommandsAddress);
        const auto dispatchGate=guest.Imports->Resolve(identity("k3GhuSNmBLU",false),2,0);
        require(guest.compiled(callers[3],{dispatchGate,targetBuilder,1})==targetCommandsAddress,"compiled dispatch return differs");
        std::array<std::uint32_t,5> targetDispatch{};
        guest.Machine.Read(targetCommandsAddress,std::as_writable_bytes(std::span(targetDispatch)));
        require(targetDispatch==std::array{0xc0031500u,1u,1u,1u,0x41u},"target dispatch modifier1 oracle differs");
        // Modifier1 proves the target builder's packet contract. The independent
        // subgroup kernel is wave32, so its numerical run explicitly supplies
        // wave32|enable. Modifier1 selects wave64 and cannot use wave32 goldens;
        // this fixture makes no wave64 or proprietary shader execution claim.
        store(guest.Machine,targetBuilder+16,targetCommandsAddress);
        require(guest.compiled(callers[3],{dispatchGate,targetBuilder,0x8001})==targetCommandsAddress,
            "compiled wave32 dispatch return differs");
        guest.Machine.Read(targetCommandsAddress,std::as_writable_bytes(std::span(targetDispatch)));
        require(targetDispatch==std::array{0xc0031500u,1u,1u,1u,0x8041u},"compiled wave32 forwarding packet differs");
        guest.Machine.Write(targetCommandsAddress+20,std::as_bytes(std::span(targetEop)));
        store(guest.Machine,targetDescriptor,targetCommandsAddress); store(guest.Machine,targetDescriptor+8,std::uint32_t{13});
        std::fill(output.begin()+64,output.begin()+320,0xeeeeeeee);
        store(guest.Machine,targetDescriptor+12,std::uint32_t{0xa55af100});
        require(guest.call("gSRnr79F8tQ",true,{0x20,targetDescriptor})==0,"target ACB packet return differs");
        driver.WaitIdle();
        require(lastQueue==0x20 && interrupts==5,"compiled target dispatch did not complete ACB EOP");
        for (std::uint32_t thread=0;thread<64;++thread) {
            const auto lane=thread%32;
            const std::array<std::uint32_t,4> expected{lane,8,thread-lane+3,lane<8 ? 1u:0u};
            for (std::size_t item=0;item<4;++item)
                require(output[64+thread*4+item]==expected[item],"target dispatch native subgroup output differs");
        }
        store(guest.Machine,targetDescriptor+8,std::uint32_t{0x100000});
        const auto rejectedBuilder=targetBuilderBytes;
        rejects([&]{guest.call("UglJIZjGssM",true,{targetDescriptor});},"qualified bound");
        rejects([&]{guest.call("i1jyy49AjXU",false,{targetBuilder,0,0,targetDestination,targetBuilder+0xc00,64,0,1});},"qualified target memory");
        rejects([&]{guest.call("k3GhuSNmBLU",false,{targetBuilder,1,1,1,0});},"qualified target forwarding");
        require(targetBuilderBytes==rejectedBuilder && interrupts==5,"target policy rejection changed guest/native state");
        driver.Shutdown();
    } catch (...) { try { driver.Shutdown(); } catch (...) {} throw; }
    std::cout << "AGC candidate provider: x86 GOT gates -> guest-relative shader creation/registration -> native Metal, 256 independent subgroup results, guards and one original EOP; compiled target header304/count64/packet16/dispatch1, snapshot lifetime and policy rejections passed\n";
}
}
int main(int argc,char** argv) {
    @autoreleasepool {
        try {
            require(argc==7,"AGC fixture requires utility metallib and five compiled guest callers");
            Admission();
            id<MTLDevice> device=MTLCreateSystemDefaultDevice();require(device!=nil,"Metal device unavailable");
            NSError* error=nil;
            const auto url=[NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[1]]];
            id<MTLLibrary> library=[device newLibraryWithURL:url error:&error];
            require(library!=nil,error ? error.localizedDescription.UTF8String : "utility library unavailable");
            NativeRoute(device,library,argv+2);
            return 0;
        } catch (const std::exception& error) {std::cerr << error.what() << '\n';return 1;}
    }
}
