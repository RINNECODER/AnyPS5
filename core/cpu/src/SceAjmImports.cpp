#include <cpu/SceAjmImports.hpp>
#include <cpu/SceElf.hpp>
#include <cpu/SceHostTrampolines.hpp>
#include <nid/NidCompute.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// Ported from upstream core/libs/prx/libSceAjm.native (batch records, job semantics, error codes)
// and core/libs/prx/libSceAudio3d. Upstream dereferences guest pointers on the host; here every
// access goes through Machine::Read/Write. Codec decoding (ffmpeg, LibAtrac9) is not linked.
namespace Cpu {
namespace {

constexpr std::int32_t ajmErrorInvalidContext = static_cast<std::int32_t>(0x80930002);
constexpr std::int32_t ajmErrorInvalidInstance = static_cast<std::int32_t>(0x80930003);
constexpr std::int32_t ajmErrorInvalidBatch = static_cast<std::int32_t>(0x80930004);
constexpr std::int32_t ajmErrorInvalidParameter = static_cast<std::int32_t>(0x80930005);
constexpr std::int32_t ajmErrorOutOfResources = static_cast<std::int32_t>(0x80930007);

constexpr std::int32_t resultNotInitialized = 0x00000001;
constexpr std::int32_t resultInvalidParameter = 0x00000004;
constexpr std::int32_t resultCodecError = 0x40000000;

constexpr std::uint64_t controlReset = 1ull << 13;
constexpr std::uint64_t controlInitialize = 1ull << 14;
constexpr std::uint64_t sidebandGaplessDecode = 1ull << 45;
constexpr std::uint64_t sidebandStream = 1ull << 47;
constexpr std::uint32_t codecMp3 = 0;

constexpr std::size_t resultBytes = 8;
constexpr std::size_t streamSidebandBytes = 16;
constexpr std::size_t resampleInfoBytes = 40;
constexpr std::size_t statisticsBytes = 24;
constexpr std::size_t maximumParameterBytes = 16;
constexpr std::size_t bufferBytes = 16;
constexpr std::size_t batchInfoBytes = 24;
constexpr std::size_t batchErrorBytes = 32;
constexpr std::size_t recordBytes = 32;
constexpr std::size_t maximumBuffers = 255;

enum class JobKind : std::uint8_t {
    Initialize = 1, ClearContext = 2, SetGaplessDecode = 3, Run = 4, GetStatistics = 5,
    Control = 6, SetResampleParameters = 7, GetResampleInfo = 8
};

constexpr std::int32_t audio3dErrorInvalidPort = static_cast<std::int32_t>(0x80EA0002);
constexpr std::int32_t audio3dErrorInvalidObject = static_cast<std::int32_t>(0x80EA0003);
constexpr std::int32_t audio3dErrorInvalidParameter = static_cast<std::int32_t>(0x80EA0004);
constexpr std::int32_t audio3dErrorOutOfResources = static_cast<std::int32_t>(0x80EA0006);
constexpr std::int32_t audio3dErrorNotReady = static_cast<std::int32_t>(0x80EA0007);
constexpr std::uint32_t audio3dObjectInvalid = 0xFFFFFFFF;
constexpr std::uint32_t audio3dSampleRate = 48000;
constexpr std::uint32_t audio3dAdvanceAndPush = 2;
constexpr std::uint32_t audio3dResetState = 0x20000;

enum class Op {
    AjmInitialize, AjmFinalize, AjmModuleRegister, AjmModuleUnregister, AjmMemoryRegister, AjmMemoryUnregister,
    AjmInstanceCreate, AjmInstanceDestroy, AjmBatchInitialize, AjmBatchStart, AjmBatchWait, AjmBatchCancel,
    JobInitialize, JobClearContext, JobSetGapless, JobSetResample, JobGetResampleInfo, JobGetStatistics,
    JobDecode, JobDecodeSplit, JobRun, JobRunSplit, JobControl,
    A3dDefaults, A3dInitialize, A3dTerminate, A3dPortOpen, A3dPortClose, A3dPortAdvance, A3dPortPush,
    A3dQueueLevel, A3dPortFlush, A3dPortSetAttribute, A3dObjectReserve, A3dObjectUnreserve, A3dObjectSetAttributes
};

struct Entry { const char* name; Op op; bool audio3d; };
constexpr std::array<Entry, 37> entries{{
    {"sceAjmInitialize", Op::AjmInitialize, false}, {"sceAjmFinalize", Op::AjmFinalize, false},
    {"sceAjmModuleRegister", Op::AjmModuleRegister, false}, {"sceAjmModuleUnregister", Op::AjmModuleUnregister, false},
    {"sceAjmMemoryRegister", Op::AjmMemoryRegister, false}, {"sceAjmMemoryUnregister", Op::AjmMemoryUnregister, false},
    {"sceAjmInstanceCreate", Op::AjmInstanceCreate, false}, {"sceAjmInstanceDestroy", Op::AjmInstanceDestroy, false},
    {"sceAjmBatchInitialize", Op::AjmBatchInitialize, false}, {"sceAjmBatchStart", Op::AjmBatchStart, false},
    {"sceAjmBatchWait", Op::AjmBatchWait, false}, {"sceAjmBatchCancel", Op::AjmBatchCancel, false},
    {"sceAjmBatchJobInitialize", Op::JobInitialize, false}, {"sceAjmBatchJobClearContext", Op::JobClearContext, false},
    {"sceAjmBatchJobSetGaplessDecode", Op::JobSetGapless, false},
    {"sceAjmBatchJobSetResampleParametersEx", Op::JobSetResample, false},
    {"sceAjmBatchJobSetResampleParameters", Op::JobSetResample, false},
    {"sceAjmBatchJobGetResampleInfo", Op::JobGetResampleInfo, false},
    {"sceAjmBatchJobGetStatistics", Op::JobGetStatistics, false},
    {"sceAjmBatchJobDecode", Op::JobDecode, false}, {"sceAjmBatchJobDecodeSplit", Op::JobDecodeSplit, false},
    {"sceAjmBatchJobRun", Op::JobRun, false}, {"sceAjmBatchJobRunSplit", Op::JobRunSplit, false},
    {"sceAjmBatchJobControl", Op::JobControl, false},
    {"sceAudio3dGetDefaultOpenParameters", Op::A3dDefaults, true}, {"sceAudio3dInitialize", Op::A3dInitialize, true},
    {"sceAudio3dTerminate", Op::A3dTerminate, true}, {"sceAudio3dPortOpen", Op::A3dPortOpen, true},
    {"sceAudio3dPortClose", Op::A3dPortClose, true}, {"sceAudio3dPortAdvance", Op::A3dPortAdvance, true},
    {"sceAudio3dPortPush", Op::A3dPortPush, true}, {"sceAudio3dPortGetQueueLevel", Op::A3dQueueLevel, true},
    {"sceAudio3dPortFlush", Op::A3dPortFlush, true}, {"sceAudio3dPortSetAttribute", Op::A3dPortSetAttribute, true},
    {"sceAudio3dObjectReserve", Op::A3dObjectReserve, true}, {"sceAudio3dObjectUnreserve", Op::A3dObjectUnreserve, true},
    {"sceAudio3dObjectSetAttributes", Op::A3dObjectSetAttributes, true}}};

template<class T> T load(Machine& guest, std::uint64_t address) {
    T value{};
    guest.CheckAccess(address, sizeof(T), Permission::Read);
    guest.Read(address, std::as_writable_bytes(std::span(&value, 1)));
    return value;
}

std::vector<std::uint8_t> loadBytes(Machine& guest, std::uint64_t address, std::size_t size) {
    std::vector<std::uint8_t> bytes(size);
    if (!size) return bytes;
    guest.CheckAccess(address, size, Permission::Read);
    guest.Read(address, std::as_writable_bytes(std::span(bytes)));
    return bytes;
}

void storeBytes(Machine& guest, std::uint64_t address, std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return;
    guest.CheckAccess(address, bytes.size(), Permission::Write);
    guest.Write(address, std::as_bytes(bytes));
}

template<class T> void store(Machine& guest, std::uint64_t address, const T& value) {
    guest.CheckAccess(address, sizeof(T), Permission::Write);
    guest.Write(address, std::as_bytes(std::span(&value, 1)));
}

template<class T> void put(std::vector<std::uint8_t>& bytes, std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

template<class T> T get(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

struct Args {
    Machine& guest;
    std::uint64_t operator[](std::size_t index) const {
        constexpr std::array registers{Register::Rdi, Register::Rsi, Register::Rdx, Register::Rcx, Register::R8, Register::R9};
        if (index < registers.size()) return guest.Get(registers[index]);
        // Host gates are entered with the return address at [rsp]; stack arguments follow it.
        return load<std::uint64_t>(guest, guest.Get(Register::Rsp) + 8 + (index - registers.size()) * 8);
    }
    std::uint32_t u32(std::size_t index) const { return static_cast<std::uint32_t>((*this)[index]); }
};

struct Job {
    JobKind kind{};
    std::uint32_t instance = 0;
    std::uint64_t flags = 0;
    std::uint64_t sideband = 0;
    std::uint32_t sidebandSize = 0;
    std::uint8_t inputCount = 0;
    std::uint8_t outputCount = 0;
    std::vector<std::uint8_t> buffers;
    std::vector<std::uint8_t> parameters;
};

struct Instance {
    std::uint32_t context = 0;
    std::uint32_t codec = 0;
    bool initialized = false;
    std::uint32_t gaplessTotal = 0;
    std::uint16_t gaplessSkip = 0;
    float resampleRatio = 1.0f;
};

struct Port {
    bool open = false;
    std::uint32_t queueDepth = 0;
    std::chrono::steady_clock::duration frame{};
    std::uint32_t advanced = 0;
    std::deque<std::chrono::steady_clock::time_point> playing;
    std::uint32_t maxObjects = 0;
    std::uint32_t nextObject = 0;
    std::set<std::uint32_t> objects;
};

}

struct SceAjmImports::Impl {
    Machine& machine;
    SceHostTrampolines trampolines;
    std::map<std::string, Entry> byNid;
    std::map<std::string, std::uint64_t> gates;
    std::mutex lock;
    std::uint32_t nextContext = 1, nextInstance = 1, nextBatch = 1;
    std::set<std::uint32_t> contexts, batches;
    std::map<std::uint32_t, Instance> instances;
    bool audio3dInitialized = false;
    Port port;

    Impl(Machine& guest, std::uint64_t base)
        : machine(guest), trampolines(guest, base, 128, "SCE Ajm/Audio3d import") {
        for (const auto& entry : entries)
            byNid.emplace(Nid::ComputeNid(entry.name, ""), entry);
    }

    static void fault(const std::string& what) { throw std::runtime_error("Unsupported SCE Ajm/Audio3d: " + what); }

    // Result sideband: {result, internalResult} then zeros up to the requested size.
    void writeSideband(const Job& job, std::int32_t result, std::size_t zeroTo = 0) {
        if (!job.sideband || job.sidebandSize < resultBytes) return;
        std::vector<std::uint8_t> bytes(std::max<std::size_t>(resultBytes, std::min<std::size_t>(zeroTo, job.sidebandSize)));
        put(bytes, 0, result);
        storeBytes(machine, job.sideband, bytes);
    }

    // Records use upstream's private batch layout inside the guest-reserved buffer.
    std::int32_t append(std::uint64_t infoAddress, JobKind kind, std::uint32_t instance, std::uint64_t flags,
                        std::uint64_t sideband, std::uint64_t sidebandSize, const std::vector<std::uint8_t>& buffers,
                        std::size_t inputs, std::size_t outputs, const std::vector<std::uint8_t>& parameters) {
        if (!infoAddress) return ajmErrorInvalidParameter;
        auto info = loadBytes(machine, infoAddress, batchInfoBytes);
        const auto buffer = get<std::uint64_t>(info, 0);
        auto offset = get<std::uint64_t>(info, 8);
        const auto size = get<std::uint64_t>(info, 16);
        if (!buffer) return ajmErrorInvalidParameter;
        if (inputs > maximumBuffers || outputs > maximumBuffers || parameters.size() > maximumParameterBytes ||
            sidebandSize > std::numeric_limits<std::uint32_t>::max())
            return ajmErrorInvalidParameter;
        const std::size_t parameterBytes = (parameters.size() + 7u) & ~std::size_t{7};
        const std::size_t bytes = recordBytes + (inputs + outputs) * bufferBytes + parameterBytes;
        if (offset > size || bytes > size - offset) return ajmErrorOutOfResources;
        std::vector<std::uint8_t> record(bytes);
        put<std::uint8_t>(record, 0, static_cast<std::uint8_t>(kind));
        put<std::uint8_t>(record, 1, static_cast<std::uint8_t>(inputs));
        put<std::uint8_t>(record, 2, static_cast<std::uint8_t>(outputs));
        put<std::uint8_t>(record, 3, static_cast<std::uint8_t>(parameters.size()));
        put<std::uint32_t>(record, 4, instance);
        put<std::uint64_t>(record, 8, flags);
        put<std::uint64_t>(record, 16, sideband);
        put<std::uint32_t>(record, 24, static_cast<std::uint32_t>(sidebandSize));
        put<std::uint32_t>(record, 28, static_cast<std::uint32_t>(bytes));
        std::copy(buffers.begin(), buffers.end(), record.begin() + recordBytes);
        std::copy(parameters.begin(), parameters.end(), record.begin() + recordBytes + (inputs + outputs) * bufferBytes);
        storeBytes(machine, buffer + offset, record);
        offset += bytes;
        store(machine, infoAddress + 8, offset);
        return 0;
    }

    std::vector<std::uint8_t> bufferList(std::uint64_t address, std::size_t count) {
        return loadBytes(machine, address, count * bufferBytes);
    }

    std::vector<std::uint8_t> singleBuffer(std::uint64_t address, std::uint64_t size) {
        std::vector<std::uint8_t> buffer(bufferBytes);
        put(buffer, 0, address);
        put(buffer, 8, size);
        return buffer;
    }

    static std::vector<std::uint8_t> join(std::vector<std::uint8_t> first, const std::vector<std::uint8_t>& second) {
        first.insert(first.end(), second.begin(), second.end());
        return first;
    }

    void execute(const Job& job) {
        if (job.kind == JobKind::GetStatistics) {
            if (job.sideband && job.sidebandSize)
                storeBytes(machine, job.sideband, std::vector<std::uint8_t>(std::min<std::size_t>(job.sidebandSize, statisticsBytes)));
            return;
        }
        const auto found = instances.find(job.instance);
        if (found == instances.end()) { writeSideband(job, resultInvalidParameter); return; }
        auto& instance = found->second;
        switch (job.kind) {
        case JobKind::Initialize:
            instance.initialized = true;
            writeSideband(job, 0);
            break;
        case JobKind::ClearContext:
            writeSideband(job, 0);
            break;
        case JobKind::SetGaplessDecode:
            if (job.parameters.size() >= 8) {
                instance.gaplessTotal = get<std::uint32_t>(job.parameters, 0);
                instance.gaplessSkip = get<std::uint16_t>(job.parameters, 4);
            }
            writeSideband(job, 0);
            break;
        case JobKind::Control:
            if (job.flags & controlInitialize) instance.initialized = true;
            writeSideband(job, 0);
            break;
        case JobKind::SetResampleParameters:
            if (job.parameters.size() >= 4) instance.resampleRatio = get<float>(job.parameters, 0);
            writeSideband(job, 0);
            break;
        case JobKind::GetResampleInfo:
            writeSideband(job, 0, resultBytes + resampleInfoBytes);
            if (job.sideband && job.sidebandSize >= resultBytes + 4)
                store(machine, job.sideband + resultBytes, instance.resampleRatio);
            break;
        case JobKind::Run: {
            if (!instance.initialized) { writeSideband(job, resultNotInitialized); break; }
            // No codec is linked: the job reports a codec error and consumes/produces nothing.
            writeSideband(job, job.inputCount || job.outputCount ? resultCodecError : 0,
                          resultBytes + ((job.flags & sidebandStream) ? streamSidebandBytes : 0));
            break;
        }
        default:
            writeSideband(job, resultInvalidParameter);
        }
    }

    std::int32_t start(std::uint64_t infoAddress, std::uint64_t errorAddress, std::uint64_t batchAddress) {
        if (!infoAddress || !batchAddress) return ajmErrorInvalidParameter;
        const auto info = loadBytes(machine, infoAddress, batchInfoBytes);
        const auto buffer = get<std::uint64_t>(info, 0);
        const auto used = get<std::uint64_t>(info, 8);
        if (used > get<std::uint64_t>(info, 16) || (used && !buffer)) return ajmErrorInvalidParameter;
        const auto records = loadBytes(machine, buffer, used);
        std::vector<Job> jobs;
        for (std::size_t cursor = 0; cursor < records.size();) {
            if (records.size() - cursor < recordBytes) return ajmErrorInvalidParameter;
            const std::vector<std::uint8_t> head(records.begin() + cursor, records.begin() + cursor + recordBytes);
            Job job;
            job.kind = static_cast<JobKind>(get<std::uint8_t>(head, 0));
            job.inputCount = get<std::uint8_t>(head, 1);
            job.outputCount = get<std::uint8_t>(head, 2);
            const auto parameterSize = get<std::uint8_t>(head, 3);
            job.instance = get<std::uint32_t>(head, 4);
            job.flags = get<std::uint64_t>(head, 8);
            job.sideband = get<std::uint64_t>(head, 16);
            job.sidebandSize = get<std::uint32_t>(head, 24);
            const auto bytes = get<std::uint32_t>(head, 28);
            const std::size_t buffersEnd = recordBytes + (job.inputCount + job.outputCount) * bufferBytes;
            if (bytes < buffersEnd + parameterSize || bytes > records.size() - cursor) return ajmErrorInvalidParameter;
            job.buffers.assign(records.begin() + cursor + recordBytes, records.begin() + cursor + buffersEnd);
            job.parameters.assign(records.begin() + cursor + buffersEnd, records.begin() + cursor + buffersEnd + parameterSize);
            jobs.push_back(std::move(job));
            cursor += bytes;
        }
        std::size_t commandOffset = 0;
        for (const auto& job : jobs) {
            if (job.sideband && job.sidebandSize) {
                try { machine.CheckAccess(job.sideband, job.sidebandSize, Permission::Write); }
                catch (const std::exception&) {
                    if (errorAddress) {
                        std::vector<std::uint8_t> error(batchErrorBytes);
                        put(error, 0, ajmErrorInvalidParameter);
                        put<std::uint64_t>(error, 8, buffer + commandOffset);
                        put<std::uint32_t>(error, 16, static_cast<std::uint32_t>(commandOffset));
                        storeBytes(machine, errorAddress, error);
                    }
                    return ajmErrorInvalidParameter;
                }
            }
            commandOffset += recordBytes + (job.inputCount + job.outputCount) * bufferBytes + ((job.parameters.size() + 7u) & ~std::size_t{7});
        }
        for (const auto& job : jobs) execute(job);
        if (errorAddress) storeBytes(machine, errorAddress, std::vector<std::uint8_t>(batchErrorBytes));
        const auto id = nextBatch++;
        batches.insert(id);
        store(machine, batchAddress, id);
        return 0;
    }

    using Clock = std::chrono::steady_clock;
    std::uint32_t queueLevel(Clock::time_point now) {
        while (!port.playing.empty() && port.playing.front() <= now) port.playing.pop_front();
        return port.advanced + static_cast<std::uint32_t>(port.playing.size());
    }
    bool portOpen(std::uint32_t id) const { return id == 0 && port.open; }

    std::int32_t ajm(Op op, const Args& a) {
        switch (op) {
        case Op::AjmInitialize: {
            if (!a[1]) return ajmErrorInvalidParameter;
            const auto id = nextContext++;
            contexts.insert(id);
            store(machine, a[1], id);
            return 0;
        }
        case Op::AjmFinalize:
            if (!contexts.erase(a.u32(0))) return ajmErrorInvalidContext;
            std::erase_if(instances, [&](const auto& item) { return item.second.context == a.u32(0); });
            return 0;
        case Op::AjmModuleRegister: case Op::AjmModuleUnregister:
        case Op::AjmMemoryRegister: case Op::AjmMemoryUnregister:
            return contexts.contains(a.u32(0)) ? 0 : ajmErrorInvalidContext;
        case Op::AjmInstanceCreate: {
            if (!contexts.contains(a.u32(0))) return ajmErrorInvalidContext;
            if (!a[3]) return ajmErrorInvalidParameter;
            const auto codec = a.u32(1);
            Instance created;
            created.context = a.u32(0);
            created.codec = codec;
            created.initialized = codec == codecMp3;
            std::uint32_t id;
            do { id = (codec << 14) | (nextInstance++ & 0x3FFF); } while (instances.contains(id));
            instances.emplace(id, created);
            store(machine, a[3], id);
            return 0;
        }
        case Op::AjmInstanceDestroy: {
            if (!contexts.contains(a.u32(0))) return ajmErrorInvalidContext;
            const auto found = instances.find(a.u32(1));
            if (found == instances.end() || found->second.context != a.u32(0)) return ajmErrorInvalidInstance;
            instances.erase(found);
            return 0;
        }
        case Op::AjmBatchInitialize: {
            if (!a[0] || !a[2]) return ajmErrorInvalidParameter;
            std::vector<std::uint8_t> info(batchInfoBytes);
            put(info, 0, a[0]);
            put<std::uint64_t>(info, 16, a[1]);
            storeBytes(machine, a[2], info);
            return 0;
        }
        case Op::AjmBatchStart:
            if (!contexts.contains(a.u32(0))) return ajmErrorInvalidContext;
            return start(a[1], a[3], a[4]);
        case Op::AjmBatchWait:
            if (!contexts.contains(a.u32(0))) return ajmErrorInvalidContext;
            if (!batches.erase(a.u32(1))) return ajmErrorInvalidBatch;
            if (a[3]) storeBytes(machine, a[3], std::vector<std::uint8_t>(batchErrorBytes));
            return 0;
        case Op::AjmBatchCancel:
            if (!contexts.contains(a.u32(0))) return ajmErrorInvalidContext;
            return batches.contains(a.u32(1)) ? 0 : ajmErrorInvalidBatch;
        case Op::JobInitialize: {
            auto parameters = a[2] && a[3] ? loadBytes(machine, a[2], std::min<std::size_t>(a[3], maximumParameterBytes))
                                           : std::vector<std::uint8_t>{};
            return append(a[0], JobKind::Initialize, a.u32(1), 0, a[4], resultBytes, {}, 0, 0, parameters);
        }
        case Op::JobClearContext:
            return append(a[0], JobKind::ClearContext, a.u32(1), 0, a[2], resultBytes, {}, 0, 0, {});
        case Op::JobSetGapless: {
            auto parameters = a[2] ? loadBytes(machine, a[2], 8) : std::vector<std::uint8_t>{};
            return append(a[0], JobKind::SetGaplessDecode, a.u32(1), a.u32(3) ? 1 : 0, a[4], resultBytes, {}, 0, 0, parameters);
        }
        case Op::JobSetResample: {
            // The ratio travels in xmm0/xmm1, which the host-call interface does not expose; no
            // codec is linked, so the ratio never affects an output and 1.0 is recorded.
            std::vector<std::uint8_t> parameters(4);
            put(parameters, 0, 1.0f);
            return append(a[0], JobKind::SetResampleParameters, a.u32(1), 0, a[3], resultBytes, {}, 0, 0, parameters);
        }
        case Op::JobGetResampleInfo:
            return append(a[0], JobKind::GetResampleInfo, a.u32(1), 0, a[2], resultBytes + resampleInfoBytes, {}, 0, 0, {});
        case Op::JobGetStatistics:
            return append(a[0], JobKind::GetStatistics, 0, 0, a[1], statisticsBytes, {}, 0, 0, {});
        case Op::JobDecode:
            return append(a[0], JobKind::Run, a.u32(1), sidebandStream, a[6], resultBytes + streamSidebandBytes,
                          join(singleBuffer(a[2], a[3]), singleBuffer(a[4], a[5])), 1, 1, {});
        case Op::JobDecodeSplit: {
            if (a[3] > maximumBuffers || a[5] > maximumBuffers) return ajmErrorInvalidParameter;
            return append(a[0], JobKind::Run, a.u32(1), sidebandStream, a[6], resultBytes + streamSidebandBytes,
                          join(bufferList(a[2], a[3]), bufferList(a[4], a[5])), a[3], a[5], {});
        }
        case Op::JobRun:
            return append(a[0], JobKind::Run, a.u32(1), a[2], a[7], a[8],
                          join(singleBuffer(a[3], a[4]), singleBuffer(a[5], a[6])), 1, 1, {});
        case Op::JobRunSplit: {
            if (a[4] > maximumBuffers || a[6] > maximumBuffers) return ajmErrorInvalidParameter;
            return append(a[0], JobKind::Run, a.u32(1), a[2], a[7], a[8],
                          join(bufferList(a[3], a[4]), bufferList(a[5], a[6])), a[4], a[6], {});
        }
        case Op::JobControl: {
            const auto flags = a[2];
            constexpr auto supported = controlReset | controlInitialize | sidebandGaplessDecode;
            if (!flags || (flags & ~supported) || ((flags & sidebandGaplessDecode) && !(flags & controlReset)))
                return ajmErrorInvalidParameter;
            if (a[3] && a[4] > 0xFFFF) return ajmErrorInvalidParameter;
            return append(a[0], JobKind::Control, a.u32(1), flags, a[5], resultBytes,
                          a[4] ? singleBuffer(a[3], a[4]) : std::vector<std::uint8_t>{}, a[4] ? 1 : 0, 0, {});
        }
        default:
            fault("unknown Ajm operation");
        }
        return 0;
    }

    std::int32_t audio3d(Op op, const Args& a) {
        switch (op) {
        case Op::A3dDefaults: {
            if (!a[0]) return 0;
            std::vector<std::uint8_t> defaults(0x20);
            put<std::uint64_t>(defaults, 0, 0x20);
            put<std::uint32_t>(defaults, 8, 256);
            put<std::uint32_t>(defaults, 16, 512);
            put<std::uint32_t>(defaults, 20, 2);
            put<std::uint32_t>(defaults, 24, 2);
            storeBytes(machine, a[0], defaults);
            return 0;
        }
        case Op::A3dInitialize:
            if (a[0]) return audio3dErrorInvalidParameter;
            if (audio3dInitialized) fault("Audio3d repeated initialization");
            audio3dInitialized = true;
            return 0;
        case Op::A3dTerminate:
            if (!audio3dInitialized || port.open) return audio3dErrorNotReady;
            audio3dInitialized = false;
            return 0;
        case Op::A3dPortOpen: {
            if (!audio3dInitialized) return audio3dErrorNotReady;
            if (static_cast<std::int32_t>(a[0]) != 0xFF || !a[1] || !a[2]) return audio3dErrorInvalidParameter;
            const auto parameters = loadBytes(machine, a[1], 0x20);
            const auto size = get<std::uint64_t>(parameters, 0);
            std::uint32_t beds = 2;
            if (size == 0x10 || size == 0x18) fault("Audio3d parameter size selects an unsupported buffer mode");
            if (size == 0x28) beds = get<std::uint32_t>(loadBytes(machine, a[1], 0x28), 0x20);
            else if (size != 0x20) return audio3dErrorInvalidParameter;
            const auto granularity = get<std::uint32_t>(parameters, 8);
            const auto maxObjects = get<std::uint32_t>(parameters, 16);
            const auto depth = get<std::uint32_t>(parameters, 20);
            const auto mode = get<std::uint32_t>(parameters, 24);
            if (get<std::uint32_t>(parameters, 12) != 0 || granularity < 0x100 || (granularity & 0xFF) ||
                !maxObjects || !depth || mode > audio3dAdvanceAndPush || (beds != 2 && beds != 3))
                return audio3dErrorInvalidParameter;
            if (mode != audio3dAdvanceAndPush) fault("Audio3d buffer mode " + std::to_string(mode));
            if (beds != 2) fault("Audio3d 3 beds");
            if (port.open) return audio3dErrorOutOfResources;
            port = Port{};
            port.open = true;
            port.queueDepth = depth;
            port.maxObjects = maxObjects;
            port.frame = std::chrono::duration_cast<Clock::duration>(
                std::chrono::nanoseconds(std::uint64_t{granularity} * 1000000000u / audio3dSampleRate));
            store<std::uint32_t>(machine, a[2], 0);
            return 0;
        }
        case Op::A3dPortClose:
            if (!portOpen(a.u32(0))) return audio3dErrorInvalidPort;
            port = Port{};
            return 0;
        case Op::A3dPortAdvance:
            if (!portOpen(a.u32(0))) return audio3dErrorInvalidPort;
            if (queueLevel(Clock::now()) >= port.queueDepth) fault("Audio3d queue is full");
            ++port.advanced;
            return 0;
        case Op::A3dQueueLevel: {
            if (!portOpen(a.u32(0))) return audio3dErrorInvalidPort;
            if (!a[1] && !a[2]) return audio3dErrorInvalidParameter;
            const auto level = queueLevel(Clock::now());
            if (a[1]) store(machine, a[1], level);
            if (a[2]) store(machine, a[2], port.queueDepth - level);
            return 0;
        }
        case Op::A3dPortPush:
            return 0; // handled by invoke
        case Op::A3dPortFlush:
            if (!portOpen(a.u32(0))) return audio3dErrorInvalidPort;
            port.advanced = 0;
            port.playing.clear();
            return 0;
        case Op::A3dPortSetAttribute: {
            if (!portOpen(a.u32(0))) return audio3dErrorInvalidPort;
            if (!a[2]) return audio3dErrorInvalidParameter;
            if (a[3] != 4) fault("Audio3d attribute size " + std::to_string(a[3]));
            const auto id = a.u32(1);
            if (id != 0x10001 && id != 0x10002 && id != 0x10003) fault("Audio3d attribute " + std::to_string(id));
            return 0;
        }
        case Op::A3dObjectReserve: {
            if (!a[1]) return audio3dErrorInvalidParameter;
            store(machine, a[1], audio3dObjectInvalid);
            if (!portOpen(a.u32(0))) return audio3dErrorInvalidPort;
            if (port.objects.size() >= port.maxObjects) return audio3dErrorOutOfResources;
            do { ++port.nextObject; } while (!port.nextObject || port.nextObject == audio3dObjectInvalid || port.objects.contains(port.nextObject));
            port.objects.insert(port.nextObject);
            store(machine, a[1], port.nextObject);
            return 0;
        }
        case Op::A3dObjectUnreserve:
            if (!portOpen(a.u32(0))) return audio3dErrorInvalidPort;
            return port.objects.erase(a.u32(1)) ? 0 : audio3dErrorInvalidObject;
        case Op::A3dObjectSetAttributes: {
            if (!portOpen(a.u32(0))) return audio3dErrorInvalidPort;
            if (!a[2] || !a[3]) return audio3dErrorInvalidParameter;
            if (!port.objects.contains(a.u32(1))) return audio3dErrorInvalidObject;
            if (a[2] > 4096) return audio3dErrorInvalidParameter;
            const auto attributes = loadBytes(machine, a[3], a[2] * 24);
            for (std::uint64_t index = 0; index < a[2]; ++index) {
                if (get<std::uint32_t>(attributes, index * 24) == audio3dResetState) continue;
                if (!get<std::uint64_t>(attributes, index * 24 + 8)) return audio3dErrorInvalidParameter;
            }
            return 0;
        }
        default:
            fault("unknown Audio3d operation");
        }
        return 0;
    }

    // Moves advanced frames onto the playing timeline; returns when a blocking push must wait until.
    std::int32_t push(std::uint32_t id, std::uint32_t blocking, Clock::time_point& waitUntil, bool& wait) {
        if (!portOpen(id)) return audio3dErrorInvalidPort;
        if (blocking > 1) fault("Audio3d blocking mode " + std::to_string(blocking));
        const auto now = Clock::now();
        const auto level = queueLevel(now);
        auto end = port.playing.empty() ? now : port.playing.back();
        for (; port.advanced != 0; --port.advanced) {
            end += port.frame;
            port.playing.push_back(end);
        }
        if (blocking == 1 && level >= port.queueDepth) {
            waitUntil = port.playing[level - port.queueDepth];
            wait = true;
        }
        return 0;
    }

    void invoke(Machine& guest, const Entry& entry) {
        std::int32_t result = 0;
        Clock::time_point waitUntil{};
        bool wait = false;
        {
            std::lock_guard guard(lock);
            const Args args{guest};
            if (entry.op == Op::A3dPortPush) result = push(args.u32(0), args.u32(1), waitUntil, wait);
            else result = entry.audio3d ? audio3d(entry.op, args) : ajm(entry.op, args);
        }
        if (wait) std::this_thread::sleep_until(waitUntil);
        guest.Set(Register::Rax, static_cast<std::uint32_t>(result));
    }
};

SceAjmImports::SceAjmImports(Machine& machine, std::uint64_t gateBase)
    : impl(std::make_shared<Impl>(machine, gateBase)) {}
SceAjmImports::~SceAjmImports() = default;

std::optional<std::uint64_t> SceAjmImports::Resolve(const SceImport& import) {
    const auto found = impl->byNid.find(import.Nid);
    if (found == impl->byNid.end()) return std::nullopt;
    const std::string library = found->second.audio3d ? "libSceAudio3d" : "libSceAjm";
    if (import.LibraryName != library || import.ModuleName != library || import.LibraryVersion != 1 ||
        import.ModuleMajor != 1 || import.ModuleMinor != 1)
        return std::nullopt;
    if (const auto gate = impl->gates.find(import.Nid); gate != impl->gates.end()) return gate->second;
    const auto gate = impl->trampolines.Add([state = std::weak_ptr<Impl>(impl), entry = found->second](Machine& guest) {
        const auto context = state.lock();
        if (!context) throw std::runtime_error("SCE Ajm/Audio3d import runtime has expired");
        context->invoke(guest, entry);
    });
    impl->gates.emplace(import.Nid, gate);
    return gate;
}

}
