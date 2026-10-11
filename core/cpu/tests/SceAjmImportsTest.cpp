#include <cpu/SceAjmImports.hpp>
#include <cpu/SceElf.hpp>
#include <nid/NidCompute.hpp>
#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Cpu::Permission;
using Cpu::Register;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr std::int32_t invalidContext = static_cast<std::int32_t>(0x80930002);
constexpr std::int32_t invalidInstance = static_cast<std::int32_t>(0x80930003);
constexpr std::int32_t invalidBatch = static_cast<std::int32_t>(0x80930004);
constexpr std::int32_t invalidParameter = static_cast<std::int32_t>(0x80930005);
constexpr std::int32_t outOfResources = static_cast<std::int32_t>(0x80930007);
constexpr std::uint64_t streamFlag = 1ull << 47;

// The 18 libSceAjm imports a retail title links, as NID and SDK name.
constexpr std::array<std::pair<const char*, const char*>, 18> titleImports{{
    {"dl+4eHSzUu4", "sceAjmInitialize"}, {"Q3dyFuwGn64", "sceAjmModuleRegister"},
    {"NVDXiUesSbA", "sceAjmBatchCancel"}, {"-qLsfDAywIY", "sceAjmBatchWait"},
    {"Wi7DtlLV+KI", "sceAjmModuleUnregister"}, {"MHur6qCsUus", "sceAjmFinalize"},
    {"MmpF1XsQiHw", "sceAjmBatchInitialize"}, {"ezM2OhNxzck", "sceAjmBatchJobInitialize"},
    {"39WxhR-ePew", "sceAjmBatchJobDecode"}, {"uJ3m8INuikg", "sceAjmBatchJobClearContext"},
    {"SkEwpiu3tZg", "sceAjmBatchJobSetGaplessDecode"}, {"5ldnD16rYZw", "sceAjmBatchJobSetResampleParametersEx"},
    {"SJ3i0DXP8vg", "sceAjmBatchJobDecodeSplit"}, {"JkdNCocpu1M", "sceAjmBatchJobGetResampleInfo"},
    {"3cAg7xN995U", "sceAjmBatchJobGetStatistics"}, {"5tOfnaClcqM", "sceAjmBatchStart"},
    {"AxoDrINp4J8", "sceAjmInstanceCreate"}, {"RbLbuKv8zho", "sceAjmInstanceDestroy"}}};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

Cpu::SceImport qualified(const std::string& nid, const char* library = "libSceAjm") {
    Cpu::SceImport value;
    value.Nid = nid;
    value.LibraryName = value.ModuleName = library;
    value.LibraryId = 1;
    value.ModuleId = 2;
    value.LibraryVersion = 1;
    value.ModuleMajor = 1;
    value.ModuleMinor = 1;
    return value;
}

struct Session {
    Cpu::Machine machine;
    std::unique_ptr<Cpu::SceAjmImports> imports = std::make_unique<Cpu::SceAjmImports>(machine, 0x7ffdd9000000ULL);
    static constexpr std::uint64_t batchBuffer = 0x3000, info = 0x3a00, error = 0x3a40, ids = 0x3a80,
                                   sideband = 0x3b00, input = 0x6000, output = 0x6400, list = 0x6800;

    Session() {
        require(std::string_view(Cpu::Machine::Backend()).find("Modern QEMU TCG") != std::string_view::npos,
                "Ajm fixture requires the native modern TCG backend");
        machine.Map(0x1000, 4096, rx);
        for (std::uint64_t page : {0x2000, 0x3000, 0x4000, 0x6000, 0x7000}) machine.Map(page, 4096, rw);
        constexpr std::array<std::uint8_t, 17> caller{
            0xff, 0x15, 0xfa, 0x0f, 0, 0, 0x48, 0x89, 0x05, 0x03, 0x10, 0, 0, 0x48, 0xff, 0xc3, 0x90};
        machine.Write(0x1000, std::as_bytes(std::span(caller)));
    }

    std::uint64_t callGate(std::uint64_t gate, std::initializer_list<std::uint64_t> arguments) {
        machine.Write(0x2000, std::as_bytes(std::span(&gate, 1)));
        constexpr std::array registers{Register::Rdi, Register::Rsi, Register::Rdx, Register::Rcx, Register::R8, Register::R9};
        std::size_t index = 0;
        for (auto value : arguments) {
            if (index < registers.size()) machine.Set(registers[index], value);
            else machine.Write(0x4f00 + (index - registers.size()) * 8, std::as_bytes(std::span(&value, 1)));
            ++index;
        }
        machine.Set(Register::Rsp, 0x4f00);
        machine.Set(Register::Rbx, 0x123456789abcdeff);
        require(machine.Run(0x1000, 0x1011, 100) == Cpu::StopReason::Address, "Gate did not return through the x86 CALL/RET caller");
        require(machine.Get(Register::Rsp) == 0x4f00 && machine.Get(Register::Rbx) == 0x123456789abcdf00, "Gate corrupted the stack");
        return machine.Get(Register::Rax);
    }

    std::int32_t call(const char* name, std::initializer_list<std::uint64_t> arguments, const char* library = "libSceAjm") {
        const auto gate = imports->Resolve(qualified(Nid::ComputeNid(name, ""), library));
        require(gate.has_value(), name);
        return static_cast<std::int32_t>(callGate(*gate, arguments));
    }

    template<class T> void put(std::uint64_t address, T value) { machine.Write(address, std::as_bytes(std::span(&value, 1))); }
    template<class T> T get(std::uint64_t address) {
        T value{};
        machine.Read(address, std::as_writable_bytes(std::span(&value, 1)));
        return value;
    }
    void fill(std::uint64_t address, std::size_t count, std::uint8_t byte) {
        const std::vector<std::uint8_t> bytes(count, byte);
        machine.Write(address, std::as_bytes(std::span(bytes)));
    }
    std::uint32_t context() {
        require(call("sceAjmInitialize", {0, ids}) == 0, "initialize");
        return get<std::uint32_t>(ids);
    }
    std::uint32_t instance(std::uint32_t ctx, std::uint32_t codec) {
        require(call("sceAjmInstanceCreate", {ctx, codec, 0, ids}) == 0, "instance create");
        return get<std::uint32_t>(ids);
    }
    void batch(std::uint64_t size = 0x800) { require(call("sceAjmBatchInitialize", {batchBuffer, size, info}) == 0, "batch init"); }
    std::int32_t start(std::uint32_t ctx) { return call("sceAjmBatchStart", {ctx, info, 0, error, ids + 8}); }
};

void titleImportsResolve() {
    Session s;
    for (const auto& [nid, name] : titleImports) {
        require(Nid::ComputeNid(name, "") == nid, name);
        require(s.imports->Resolve(qualified(nid)).has_value(), name);
    }
    require(!s.imports->Resolve(qualified(titleImports[0].first, "libSceOther")), "Foreign library resolved");
    auto skewed = qualified(titleImports[0].first);
    skewed.LibraryVersion = 2;
    require(!s.imports->Resolve(skewed), "Skewed library version resolved");
    require(!s.imports->Resolve(qualified(Nid::ComputeNid("sceAjmInitialize", ""), "libSceAudio3d")), "Ajm NID resolved as Audio3d");
    require(s.imports->Resolve(qualified(Nid::ComputeNid("sceAudio3dPortOpen", ""), "libSceAudio3d")).has_value(), "Audio3d unresolved");
}

void lifecycle() {
    Session s;
    require(s.call("sceAjmInitialize", {0, 0}) == invalidParameter, "null context pointer accepted");
    const auto ctx = s.context();
    require(ctx != 0 && s.context() != ctx, "contexts are not distinct");
    require(s.call("sceAjmModuleRegister", {ctx, 0, 0}) == 0, "register");
    require(s.call("sceAjmModuleRegister", {999, 0, 0}) == invalidContext, "unknown context registered");
    const auto mp3 = s.instance(ctx, 0), at9 = s.instance(ctx, 1);
    require(mp3 != at9 && (at9 >> 14) == 1, "instance id does not carry the codec");
    require(s.call("sceAjmInstanceCreate", {999, 0, 0, s.ids}) == invalidContext, "instance on unknown context");
    require(s.call("sceAjmInstanceDestroy", {ctx, at9}) == 0, "destroy");
    require(s.call("sceAjmInstanceDestroy", {ctx, at9}) == invalidInstance, "double destroy");
    require(s.call("sceAjmModuleUnregister", {ctx, 0}) == 0, "unregister");
    require(s.call("sceAjmFinalize", {ctx}) == 0, "finalize");
    require(s.call("sceAjmFinalize", {ctx}) == invalidContext, "double finalize");
    require(s.call("sceAjmInstanceDestroy", {ctx, mp3}) == invalidContext, "destroy after finalize");
}

void batchJobs() {
    Session s;
    const auto ctx = s.context();
    const auto at9 = s.instance(ctx, 1), mp3 = s.instance(ctx, 0);
    s.batch();
    require(s.get<std::uint64_t>(s.info) == s.batchBuffer && s.get<std::uint64_t>(s.info + 8) == 0 &&
            s.get<std::uint64_t>(s.info + 16) == 0x800, "batch info");
    s.fill(s.sideband, 0x100, 0xa7);
    const std::array<std::uint8_t, 4> config{0xfe, 0x1d, 0x00, 0x00};
    s.machine.Write(s.input, std::as_bytes(std::span(config)));
    // Decode before initialize reports NOT_INITIALIZED; after initialize the decode reaches the
    // unlinked codec and consumes and produces nothing.
    require(s.call("sceAjmBatchJobDecode", {s.info, at9, s.input + 0x10, 32, s.output, 64, s.sideband}) == 0, "decode job");
    require(s.call("sceAjmBatchJobInitialize", {s.info, at9, s.input, 4, s.sideband + 0x20}) == 0, "init job");
    require(s.call("sceAjmBatchJobDecode", {s.info, at9, s.input + 0x10, 32, s.output, 64, s.sideband + 0x40}) == 0, "decode job 2");
    require(s.call("sceAjmBatchJobDecode", {s.info, mp3, s.input + 0x10, 32, s.output, 64, s.sideband + 0x80}) == 0, "mp3 decode");
    require(s.call("sceAjmBatchJobClearContext", {s.info, at9, s.sideband + 0xc0}) == 0, "clear job");
    require(s.get<std::uint64_t>(s.info + 8) != 0, "jobs did not advance the batch offset");
    require(s.get<std::uint8_t>(s.sideband) == 0xa7, "job executed before start");
    require(s.start(ctx) == 0, "start");
    require(s.get<std::int32_t>(s.sideband) == 0x1, "decode before initialize is not NOT_INITIALIZED");
    require(s.get<std::int32_t>(s.sideband + 0x20) == 0, "initialize result");
    require(s.get<std::int32_t>(s.sideband + 0x40) == 0x40000000, "decode did not report the codec error");
    require(s.get<std::int32_t>(s.sideband + 0x80) == 0x40000000, "mp3 decode did not report the codec error");
    require(s.get<std::int32_t>(s.sideband + 0xc0) == 0, "clear result");
    require(s.get<std::uint64_t>(s.sideband + 0x48) == 0 && s.get<std::uint64_t>(s.sideband + 0x50) == 0,
            "decode sideband reports consumed or produced bytes");
    const auto handle = s.get<std::uint32_t>(s.ids + 8);
    require(s.call("sceAjmBatchCancel", {ctx, handle}) == 0, "cancel of a live batch");
    require(s.call("sceAjmBatchWait", {ctx, handle, 0, s.error}) == 0, "wait");
    require(s.call("sceAjmBatchWait", {ctx, handle, 0, s.error}) == invalidBatch, "waited twice");
    require(s.call("sceAjmBatchCancel", {ctx, handle}) == invalidBatch, "cancelled a waited batch");
    require(s.call("sceAjmBatchStart", {999, s.info, 0, s.error, s.ids + 8}) == invalidContext, "start on unknown context");
}

void splitJobsAndSidebands() {
    Session s;
    const auto ctx = s.context();
    const auto at9 = s.instance(ctx, 1);
    s.batch();
    s.fill(s.sideband, 0x200, 0xa7);
    s.put<std::uint64_t>(s.list, s.input); s.put<std::uint64_t>(s.list + 8, 16);
    s.put<std::uint64_t>(s.list + 16, s.input + 16); s.put<std::uint64_t>(s.list + 24, 16);
    s.put<std::uint64_t>(s.list + 32, s.output); s.put<std::uint64_t>(s.list + 40, 64);
    const std::array<std::uint8_t, 4> config{1, 2, 3, 4};
    s.machine.Write(s.input, std::as_bytes(std::span(config)));
    require(s.call("sceAjmBatchJobInitialize", {s.info, at9, s.input, 4, s.sideband}) == 0, "init");
    require(s.call("sceAjmBatchJobDecodeSplit", {s.info, at9, s.list, 2, s.list + 32, 1, s.sideband + 0x10}) == 0, "split decode");
    s.put<std::uint32_t>(s.input + 0x40, 1000); s.put<std::uint16_t>(s.input + 0x44, 8);
    require(s.call("sceAjmBatchJobSetGaplessDecode", {s.info, at9, s.input + 0x40, 1, s.sideband + 0x30}) == 0, "gapless");
    require(s.call("sceAjmBatchJobSetResampleParametersEx", {s.info, at9, 0, s.sideband + 0x40}) == 0, "resample parameters");
    require(s.call("sceAjmBatchJobGetResampleInfo", {s.info, at9, s.sideband + 0x50}) == 0, "resample info");
    require(s.call("sceAjmBatchJobGetStatistics", {s.info, s.sideband + 0x90}) == 0, "statistics");
    // An unknown instance reports INVALID_PARAMETER in its own sideband, not as a call failure.
    require(s.call("sceAjmBatchJobClearContext", {s.info, 0x7777, s.sideband + 0xb0}) == 0, "unknown instance append");
    s.fill(s.sideband + 0x90, 24, 0xa7);
    require(s.start(ctx) == 0, "start");
    require(s.get<std::int32_t>(s.sideband + 0x10) == 0x40000000, "split decode result");
    require(s.get<std::int32_t>(s.sideband + 0x30) == 0 && s.get<std::int32_t>(s.sideband + 0x40) == 0, "job results");
    require(s.get<std::int32_t>(s.sideband + 0x50) == 0, "resample info result");
    require(s.get<float>(s.sideband + 0x58) == 1.0f && s.get<std::int32_t>(s.sideband + 0x5c) == 0, "resample info body");
    require(s.get<std::uint64_t>(s.sideband + 0x90) == 0 && s.get<std::uint64_t>(s.sideband + 0xa0) == 0, "statistics not zeroed");
    require(s.get<std::int32_t>(s.sideband + 0xb0) == 4, "unknown instance result");
}

void runJobsUseStackArguments() {
    Session s;
    const auto ctx = s.context();
    const auto at9 = s.instance(ctx, 1);
    s.batch();
    s.fill(s.sideband, 0x40, 0xa7);
    require(s.call("sceAjmBatchJobControl", {s.info, at9, 1ull << 14, s.input, 8, s.sideband, 8}) == 0, "control initialize");
    require(s.call("sceAjmBatchJobControl", {s.info, at9, 0, 0, 0, s.sideband, 8}) == invalidParameter, "empty control flags");
    require(s.call("sceAjmBatchJobRun", {s.info, at9, streamFlag, s.input, 32, s.output, 64, s.sideband + 0x10, 24}) == 0, "run job");
    s.put<std::uint64_t>(s.list, s.input); s.put<std::uint64_t>(s.list + 8, 32);
    s.put<std::uint64_t>(s.list + 16, s.output); s.put<std::uint64_t>(s.list + 24, 64);
    require(s.call("sceAjmBatchJobRunSplit", {s.info, at9, 0, s.list, 1, s.list + 16, 1, s.sideband + 0x30, 8}) == 0, "run split");
    require(s.start(ctx) == 0, "start");
    require(s.get<std::int32_t>(s.sideband) == 0, "control initialize result");
    require(s.get<std::int32_t>(s.sideband + 0x10) == 0x40000000, "run result");
    require(s.get<std::int32_t>(s.sideband + 0x30) == 0x40000000, "run split result");
}

void batchLimitsAndMemorySafety() {
    Session s;
    const auto ctx = s.context();
    const auto at9 = s.instance(ctx, 1);
    require(s.call("sceAjmBatchInitialize", {0, 64, s.info}) == invalidParameter, "null batch buffer");
    s.batch(64);
    require(s.call("sceAjmBatchJobClearContext", {s.info, at9, s.sideband}) == 0, "first record fits");
    require(s.call("sceAjmBatchJobClearContext", {s.info, at9, s.sideband}) == 0, "second record fits");
    require(s.get<std::uint64_t>(s.info + 8) == 64, "records are not 32 bytes");
    require(s.call("sceAjmBatchJobClearContext", {s.info, at9, s.sideband}) == outOfResources, "overflow not rejected");
    require(s.get<std::uint64_t>(s.info + 8) == 64, "failed append moved the offset");
    // A sideband pointing at unmapped guest memory fails the batch without running any job.
    s.batch();
    s.put<std::uint32_t>(s.sideband, 0x55555555);
    require(s.call("sceAjmBatchJobClearContext", {s.info, at9, s.sideband}) == 0, "append");
    require(s.call("sceAjmBatchJobClearContext", {s.info, at9, 0x70000000}) == 0, "append with unmapped sideband");
    s.fill(s.error, 32, 0);
    require(s.start(ctx) == invalidParameter, "unmapped sideband started");
    require(s.get<std::int32_t>(s.sideband) == 0x55555555, "failed batch ran an earlier job");
    require(s.get<std::int32_t>(s.error) == invalidParameter && s.get<std::uint32_t>(s.error + 16) == 32,
            "batch error does not name the failing command offset");
}

void audio3d() {
    Session s;
    const char* lib = "libSceAudio3d";
    s.fill(s.sideband, 0x40, 0xa7);
    require(s.call("sceAudio3dGetDefaultOpenParameters", {s.sideband}, lib) == 0, "defaults");
    require(s.get<std::uint64_t>(s.sideband) == 0x20 && s.get<std::uint32_t>(s.sideband + 8) == 256 &&
            s.get<std::uint32_t>(s.sideband + 16) == 512 && s.get<std::uint32_t>(s.sideband + 20) == 2 &&
            s.get<std::uint32_t>(s.sideband + 24) == 2, "default open parameters");
    constexpr std::int32_t notReady = static_cast<std::int32_t>(0x80EA0007);
    constexpr std::int32_t invalidPort = static_cast<std::int32_t>(0x80EA0002);
    constexpr std::int32_t invalidObject = static_cast<std::int32_t>(0x80EA0003);
    require(s.call("sceAudio3dPortOpen", {0xff, s.sideband, s.ids}, lib) == notReady, "open before initialize");
    require(s.call("sceAudio3dInitialize", {0}, lib) == 0, "initialize");
    require(s.call("sceAudio3dPortOpen", {0xff, s.sideband, s.ids}, lib) == 0 && s.get<std::uint32_t>(s.ids) == 0, "open");
    require(s.call("sceAudio3dTerminate", {}, lib) == notReady, "terminate with open port");
    require(s.call("sceAudio3dPortAdvance", {0}, lib) == 0, "advance");
    s.fill(s.ids, 8, 0xa7);
    require(s.call("sceAudio3dPortGetQueueLevel", {0, s.ids, s.ids + 4}, lib) == 0, "queue level");
    require(s.get<std::uint32_t>(s.ids) == 1 && s.get<std::uint32_t>(s.ids + 4) == 1, "queue level values");
    require(s.call("sceAudio3dPortPush", {0, 0}, lib) == 0, "async push");
    require(s.call("sceAudio3dPortFlush", {0}, lib) == 0, "flush");
    require(s.call("sceAudio3dObjectReserve", {0, s.ids}, lib) == 0, "reserve");
    const auto object = s.get<std::uint32_t>(s.ids);
    require(object != 0 && object != 0xFFFFFFFF, "object id");
    s.put<std::uint32_t>(s.input, 7);
    s.put<std::uint32_t>(s.list, 0x10001); s.put<std::uint64_t>(s.list + 8, s.input); s.put<std::uint64_t>(s.list + 16, 4);
    require(s.call("sceAudio3dObjectSetAttributes", {0, object, 1, s.list}, lib) == 0, "object attributes");
    require(s.call("sceAudio3dObjectUnreserve", {0, object}, lib) == 0, "unreserve");
    require(s.call("sceAudio3dObjectUnreserve", {0, object}, lib) == invalidObject, "double unreserve");
    require(s.call("sceAudio3dPortClose", {0}, lib) == 0, "close");
    require(s.call("sceAudio3dPortClose", {0}, lib) == invalidPort, "double close");
    require(s.call("sceAudio3dTerminate", {}, lib) == 0, "terminate");
}

void lifetime() {
    Session s;
    const auto gate = s.imports->Resolve(qualified(titleImports[0].first)).value();
    require(s.imports->Resolve(qualified(titleImports[0].first)) == gate, "gate is not stable");
    s.imports.reset();
    try { s.callGate(gate, {0, s.ids}); }
    catch (const std::exception&) { return; }
    throw std::runtime_error("Expired Ajm provider accepted a call");
}
}

int main() {
    try {
        titleImportsResolve();
        lifecycle();
        batchJobs();
        splitJobsAndSidebands();
        runJobsUseStackArguments();
        batchLimitsAndMemorySafety();
        audio3d();
        lifetime();
        std::cout << "PASS native x86 Ajm batch/context/instance gates, stack arguments, checked memory and Audio3d port\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
