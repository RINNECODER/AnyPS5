#include <cpu/SceImports.hpp>
#include <cpu/SceModules.hpp>
#include <cpu/SceTls.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unistd.h>

namespace {
using Bytes = std::vector<std::byte>;
constexpr std::uint64_t MainBias = 0x1000000;
constexpr std::uint64_t GuestBias = 0x2000000;
constexpr std::uint64_t HostObject = 0x4000000;
constexpr std::string_view LengthNid = "j4ViWNHEgww";

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
Bytes readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    require(bool(stream), "Cannot open compiled module fixture");
    const auto size = stream.tellg();
    require(size > 0 && size < 8 * 1024 * 1024, "Compiled module fixture size is invalid");
    Bytes bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char*>(bytes.data()), size);
    require(bool(stream), "Cannot read compiled module fixture");
    return bytes;
}
std::uint64_t get(const Bytes& bytes, std::uint64_t offset, unsigned size = 8) {
    std::uint64_t result = 0;
    for (unsigned index = 0; index < size; ++index) result |= std::uint64_t(std::to_integer<unsigned>(bytes.at(offset + index))) << (index * 8);
    return result;
}
void put(Bytes& bytes, std::uint64_t offset, std::uint64_t value, unsigned size = 8) {
    for (unsigned index = 0; index < size; ++index) bytes.at(offset + index) = static_cast<std::byte>(value >> (index * 8));
}
std::uint64_t program(const Bytes& bytes, std::uint32_t type) {
    for (unsigned index = 0; index < get(bytes, 56, 2); ++index) {
        const auto offset = get(bytes, 32) + index * 56;
        if (get(bytes, offset, 4) == type) return offset;
    }
    throw std::runtime_error("Fixture program header missing");
}
std::uint64_t tag(const Bytes& bytes, std::uint64_t type) {
    const auto header = program(bytes, 2);
    const auto offset = get(bytes, header + 8);
    for (std::uint64_t index = offset; index < offset + get(bytes, header + 32); index += 16)
        if (get(bytes, index) == type) return index;
    throw std::runtime_error("Fixture dynamic tag missing");
}
void libraryAttributes(Bytes& bytes, std::uint64_t imports, std::uint64_t exports) {
    const auto header = program(bytes, 2);
    const auto offset = get(bytes, header + 8);
    for (auto index = offset; index < offset + get(bytes, header + 32); index += 16) {
        const auto type = get(bytes, index);
        if (type == 0x61000019 || type == 0x61000017)
            put(bytes, index + 8, (get(bytes, index + 8) & 0xffff000000000000ull) |
                (type == 0x61000019 ? imports : exports));
    }
}
std::uint64_t fileOffset(const Bytes& bytes, std::uint64_t address) {
    for (unsigned index = 0; index < get(bytes, 56, 2); ++index) {
        const auto offset = get(bytes, 32) + index * 56;
        if (get(bytes, offset, 4) == 1 && address >= get(bytes, offset + 16) &&
            address - get(bytes, offset + 16) < get(bytes, offset + 32))
            return get(bytes, offset + 8) + address - get(bytes, offset + 16);
    }
    throw std::runtime_error("Fixture initializer is not file-backed");
}
void changeImportType(Bytes& bytes, const std::string& nid, unsigned type) {
    const auto dynlib = get(bytes, program(bytes, 0x61000000) + 8);
    const auto symbols = dynlib + get(bytes, tag(bytes, 0x61000039) + 8);
    const auto strings = dynlib + get(bytes, tag(bytes, 0x61000035) + 8);
    const auto size = get(bytes, tag(bytes, 0x6100003f) + 8);
    for (std::uint64_t offset = symbols; offset < symbols + size; offset += 24) {
        if (get(bytes, offset + 6, 2) || !get(bytes, offset + 4, 1)) continue;
        const auto name = strings + get(bytes, offset, 4);
        bool matches = true;
        for (unsigned index = 0; index < nid.size(); ++index)
            matches = matches && get(bytes, name + index, 1) == static_cast<unsigned char>(nid[index]);
        if (matches && get(bytes, name + nid.size(), 1) == '#') {
            put(bytes, offset + 4, (get(bytes, offset + 4, 1) & 0xf0) | type, 1);
            return;
        }
    }
    throw std::runtime_error("Fixture typed import missing");
}
void changeImportSize(Bytes& bytes, std::string_view nid, std::uint64_t symbolSize) {
    const auto dynlib = get(bytes, program(bytes, 0x61000000) + 8);
    const auto symbols = dynlib + get(bytes, tag(bytes, 0x61000039) + 8);
    const auto strings = dynlib + get(bytes, tag(bytes, 0x61000035) + 8);
    const auto size = get(bytes, tag(bytes, 0x6100003f) + 8);
    for (auto offset = symbols; offset < symbols + size; offset += 24) {
        if (get(bytes, offset + 6, 2) || !get(bytes, offset + 4, 1)) continue;
        const auto name = strings + get(bytes, offset, 4);
        bool matches = true;
        for (unsigned index = 0; index < nid.size(); ++index)
            matches = matches && get(bytes, name + index, 1) == static_cast<unsigned char>(nid[index]);
        if (matches && get(bytes, name + nid.size(), 1) == '#') {
            put(bytes, offset + 16, symbolSize);
            return;
        }
    }
    throw std::runtime_error("Fixture sized import missing");
}
std::uint64_t redirectHostObject(Bytes& bytes) {
    const auto dynlib = get(bytes, program(bytes, 0x61000000) + 8);
    const auto symbols = dynlib + get(bytes, tag(bytes, 0x61000039) + 8);
    const auto strings = dynlib + get(bytes, tag(bytes, 0x61000035) + 8);
    const auto size = get(bytes, tag(bytes, 0x6100003f) + 8);
    constexpr std::string_view name = "OD70jrfOzHM#B#B";
    for (auto offset = symbols; offset < symbols + size; offset += 24) {
        if (get(bytes, offset + 6, 2) || (get(bytes, offset + 4, 1) & 15) != 1) continue;
        const auto location = strings + get(bytes, offset, 4);
        bool matches = get(bytes, location + name.size(), 1) == 0;
        for (unsigned index = 0; index < name.size(); ++index) {
            const auto value = get(bytes, location + index, 1);
            matches = matches && (value == static_cast<unsigned char>(name[index]) ||
                ((index == 12 || index == 14) && value == 'C'));
        }
        if (!matches) continue;
        put(bytes, location + 12, 'C', 1);
        put(bytes, location + 14, 'C', 1);
        const auto relocations = dynlib + get(bytes, tag(bytes, 0x6100002f) + 8);
        const auto relocationSize = get(bytes, tag(bytes, 0x61000031) + 8);
        for (auto relocation = relocations; relocation < relocations + relocationSize; relocation += 24)
            if (get(bytes, relocation + 8) == (((offset - symbols) / 24) << 32 | 6)) return get(bytes, relocation);
        throw std::runtime_error("Compiled OBJECT import lacks a genuine GLOB_DAT relocation");
    }
    throw std::runtime_error("Compiled main lacks the dependency counter OBJECT import");
}
std::uint64_t word(Cpu::Machine& machine, std::uint64_t address) {
    Bytes bytes(8);
    machine.Read(address, bytes);
    return get(bytes, 0);
}
const Cpu::SceExport& definition(const Cpu::SceModuleRecord& module, const std::string& nid, unsigned type) {
    const auto found = std::find_if(module.Image.Exports.begin(), module.Image.Exports.end(), [&](const auto& item) {
        return item.Identity.Nid == nid && item.Type == type;
    });
    require(found != module.Image.Exports.end(), "Compiled fixture lost an expected typed export");
    return *found;
}
std::uint64_t object(const Cpu::SceModuleRecord& module, const std::string& nid) {
    return module.LoadBias + definition(module, nid, 1).Value;
}
std::uint64_t tlsAddress(const Cpu::SceModuleRecord& module, const std::string& nid, const Cpu::SceTls& tls) {
    return tls.Resolve(module.TlsModuleId, definition(module, nid, 6).Value);
}
template<class Function> void rejects(Function&& function, const char* expected) {
    try { function(); }
    catch (const std::exception& error) {
        if (std::string(error.what()).find(expected) == std::string::npos)
            throw std::runtime_error(std::string("Wrong module graph failure: ") + error.what());
        return;
    }
    throw std::runtime_error(std::string("Missing module graph rejection: ") + expected);
}
const std::array hostModules{Cpu::SceHostModule{"libc.prx", {"libc", 0, 1, 1}, {{"libc", 0, 1}}}};

bool sameConsumer(const Cpu::SceImportConsumer& consumer, const Cpu::SceParsedImage& expected) {
    return consumer.Path == expected.Path && consumer.SourceSize == expected.SourceSize &&
        consumer.SourceSha256 == expected.SourceSha256;
}
void mapHostObject(Cpu::Machine& machine) {
    machine.Map(HostObject, 4096, Cpu::Permission::Read | Cpu::Permission::Write);
    Bytes value(8);
    put(value, 0, 1);
    machine.Write(HostObject, value);
    machine.Protect(HostObject, 4096, Cpu::Permission::Read);
}

void execute(const std::filesystem::path& mainPath, const std::filesystem::path& guestPath, bool wrongOracle,
             bool contextual = false, bool hostObject = false, bool replaceDependency = false,
             std::uint64_t mainLengthSize = 0) {
    Cpu::Machine machine;
    Cpu::SceImports imports(machine);
    if (hostObject) mapHostObject(machine);
    const std::array expectedSources{Cpu::ParseSce(mainPath), Cpu::ParseSce(guestPath)};
    std::array<unsigned, 2> lengths{};
    unsigned objects = 0, legacyCalls = 0;
    bool replaced = false;
    const Cpu::SceConsumerModuleResolver consumerResolver = contextual
        ? Cpu::SceConsumerModuleResolver{[&](const auto& consumer, const auto& import, std::uint8_t type, std::uint64_t size)
                -> std::optional<Cpu::SceResolvedImport> {
            const auto index = sameConsumer(consumer, expectedSources[0]) ? 0u : 1u;
            require(sameConsumer(consumer, expectedSources[index]), "Host callback received the wrong parsed consumer source");
            if (replaceDependency && !replaced) {
                require(index == 0, "Snapshot control must replace the dependency after main resolution begins");
                auto changed = readFile(guestPath);
                const auto& exports = expectedSources[1].Exports;
                const auto object = std::find_if(exports.begin(), exports.end(), [](const auto& item) {
                    return item.Identity.Nid == "L+OvOB7GHzo" && item.Type == 1;
                });
                require(object != exports.end(), "Snapshot fixture lost its actual compiled object");
                put(changed, fileOffset(changed, object->Value), 0x76543210);
                {
                    std::ofstream stream(guestPath, std::ios::binary | std::ios::trunc);
                    stream.write(reinterpret_cast<const char*>(changed.data()), changed.size());
                    require(bool(stream), "Cannot replace dependency during host resolution");
                }
                require(Cpu::ParseSce(guestPath).SourceSha256 != expectedSources[1].SourceSha256,
                        "Snapshot control did not change the on-disk dependency source");
                replaced = true;
            }
            if (hostObject && import.Nid == "OD70jrfOzHM") {
                require(index == 0 && type == 1, "Host callback lost the main consumer's genuine OBJECT type");
                require(size == 8, "Host callback lost the imported OBJECT size");
                ++objects;
                return Cpu::SceResolvedImport{HostObject, 1, 8};
            }
            require(type == 2, "Host callback lost a genuine FUNC type");
            if (import.Nid == LengthNid) {
                require(size == (index == 0 ? mainLengthSize : 0), "Host callback lost the imported FUNC size");
                require(import.LibraryName == "libc" && import.ModuleName == "libc" &&
                        import.LibraryVersion == 1 && import.ModuleMajor == 1 && import.ModuleMinor == 1 &&
                        import.LibraryId == 2 && import.ModuleId == 2,
                        "Shared compiled host function lost its exact scoped identity");
                ++lengths[index];
            }
            return Cpu::SceResolvedImport{imports.Resolve(import), type};
        }} : Cpu::SceConsumerModuleResolver{};
    const std::array dependencies{Cpu::SceModuleFile{guestPath, GuestBias}};
    Cpu::SceModules graph(machine, {mainPath, MainBias}, dependencies, hostModules,
        [&](const auto& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
            ++legacyCalls;
            require(type == 2, "Fixture unexpectedly requires a host data or TLS service");
            return Cpu::SceResolvedImport{imports.Resolve(import), 2, 0, 0, 0};
        }, std::nullopt, consumerResolver);
    if (contextual)
        require(!legacyCalls && lengths[0] == 1 && lengths[1] == 1 && objects == unsigned(hostObject),
                "Source-qualified host resolution reused another consumer's grant or called the legacy resolver");
    if (hostObject) {
        auto source = readFile(mainPath);
        require(word(machine, MainBias + redirectHostObject(source)) == HostObject,
                "Genuine compiled OBJECT relocation did not select the admitted host storage");
    }
    if (replaceDependency)
        require(replaced && word(machine, object(graph.Modules()[1], "L+OvOB7GHzo")) == 0x10203040,
                "Admitted consumer identity and mapped guest bytes came from different source snapshots");
    require(graph.Modules().size() == 2 && graph.Tls() && graph.Tls()->ModuleCount() == 2 &&
            graph.Modules()[0].TlsModuleId == 1 && graph.Modules()[1].TlsModuleId == 2 &&
            graph.Modules()[0].Init && graph.Modules()[1].Init && graph.Modules()[1].Fini,
            "Module graph lost actual initializer addresses or static TLS providers");
    auto& main = graph.Main();
    const auto& first = graph.Modules()[0];
    const auto& dependency = graph.Modules()[1];
    rejects([&] { graph.InitializeDependencies(); }, "configured main entry");
    Cpu::SetupSceEntry(machine, main, {"module-fixture", "17", "5", "7", "58", wrongOracle ? "3366582379" : "3366582378"}, imports.ExitGate());
    graph.InitializeDependencies();
    require(word(machine, object(first, "xHwC9ebKTAY")) == 0 &&
            word(machine, object(dependency, "OD70jrfOzHM")) == 1 &&
            word(machine, object(dependency, "h-nyiHettao")) == 1,
            "Loader must initialize dependencies once and leave main DT_INIT to main entry");
    rejects([&] { graph.InitializeDependencies(); }, "freshly loaded graph");
    require(machine.Run(main.Entry, 0, 2000000) == Cpu::StopReason::Exit && machine.ExitCode() == (wrongOracle ? 77 : 0),
            "Compiled module program disagreed with independent prime/Adler oracle or failure control flow");
    const std::array<std::uint64_t, 11> expected{1, 1, 12, 7, 58, 3366582378, 0x22334462, 0x31415938, 0x10203052, 63, 12};
    const auto state = object(first, "uhHbg8dKn0c");
    for (unsigned index = 0; index < expected.size(); ++index)
        require(word(machine, state + index * 8) == expected[index], "Compiled main observed incorrect module call, object, TLS, constructor order, or arithmetic result");
    require(word(machine, object(first, "3FaMAKNsSAY")) == object(dependency, "L+OvOB7GHzo") &&
            word(machine, tlsAddress(dependency, "pmoWhWcpcHY", *graph.Tls())) == 0x22334462 &&
            word(machine, tlsAddress(first, "rBiIvjUtJbs", *graph.Tls())) == 0x31415938,
            "Cross-module object relocation or TLS storage is incorrect");
    graph.FinalizeDependencies();
    require(word(machine, object(dependency, "A+KZd7Qeu3w")) == 1 &&
            word(machine, object(dependency, "h-nyiHettao")) == 123 &&
            word(machine, object(dependency, "L+OvOB7GHzo")) == 0x10203055 &&
            word(machine, tlsAddress(dependency, "pmoWhWcpcHY", *graph.Tls())) == 0x22334467,
            "Translated dependency DT_FINI did not finalize actual object/TLS state exactly once");
    rejects([&] { graph.FinalizeDependencies(); }, "successful initialization");
}

struct Input {
    std::filesystem::path directory;
    std::filesystem::path main;
    std::filesystem::path guest;
    Input(const Bytes& mainBytes, const Bytes& guestBytes, const char* guestFilename = "SceModuleGuest.prx") {
        directory = std::filesystem::temp_directory_path() / ("anyps5-modules-" + std::to_string(::getpid()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(directory), "Cannot create copied module fixture directory");
        main = directory / "sce-module-main.elf";
        guest = directory / guestFilename;
        for (const auto& [path, bytes] : std::array{std::pair{main, mainBytes}, std::pair{guest, guestBytes}}) {
            std::ofstream stream(path, std::ios::binary);
            stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            require(bool(stream), "Cannot write copied module fixture");
        }
    }
    ~Input() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
};

void consumerFailures(const std::filesystem::path& mainPath, const std::filesystem::path& guestPath, bool hostObject = false) {
    const auto mainBytes = readFile(mainPath), guestBytes = readFile(guestPath);
    const std::array expectedSources{Cpu::ParseSce(mainPath), Cpu::ParseSce(guestPath)};
    // Each row reaches a genuine host symbol in one consumer; the legacy route
    // could admit it, so rejection must come from the authoritative callback.
    for (unsigned target = 0; target < (hostObject ? 1u : 2u); ++target) for (bool wrongType : {false, true}) {
        Cpu::Machine machine;
        Cpu::SceImports imports(machine);
        if (hostObject) mapHostObject(machine);
        unsigned legacyCalls = 0, denied = 0;
        const auto existingRanges = machine.Mappings().size();
        const std::array dependencies{Cpu::SceModuleFile{guestPath, GuestBias}};
        rejects([&] { Cpu::SceModules graph(machine, {mainPath, MainBias}, dependencies, hostModules,
            [&](const auto& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
                ++legacyCalls;
                if (type == 1) return Cpu::SceResolvedImport{HostObject, 1, 8};
                return Cpu::SceResolvedImport{imports.Resolve(import), type};
            }, std::nullopt,
            [&](const auto& consumer, const auto& import, std::uint8_t type, std::uint64_t) -> std::optional<Cpu::SceResolvedImport> {
                require(sameConsumer(consumer, expectedSources[0]) || sameConsumer(consumer, expectedSources[1]),
                        "Rejection callback received another consumer's source identity");
                const bool isObject = hostObject && import.Nid == "OD70jrfOzHM";
                require(type == (isObject ? 1 : 2), "Compiled host import lost its FUNC/OBJECT type");
                if ((isObject || (!hostObject && import.Nid == LengthNid)) && sameConsumer(consumer, expectedSources[target])) {
                    ++denied;
                    if (!wrongType) return std::nullopt;
                    return isObject ? Cpu::SceResolvedImport{HostObject, 2} : Cpu::SceResolvedImport{imports.Resolve(import), 1, 8};
                }
                return Cpu::SceResolvedImport{imports.Resolve(import), type};
            }); }, "unresolved or wrongly typed host import");
        require(denied == 1 && !legacyCalls && machine.Mappings().size() == existingRanges &&
                machine.Get(Cpu::Register::FsBase) == 0,
                "Consumer rejection fell back to legacy resolution or mutated guest memory/TLS");
    }
    if (hostObject) return;

    // Changing unreferenced source bytes preserves the compiled program and
    // filename, but must invalidate source authority for that dependency.
    for (bool changeSize : {false, true}) {
        auto poisoned = guestBytes;
        if (changeSize) poisoned.push_back(std::byte{0x5a});
        else poisoned.at(9) ^= std::byte{1}; // Unused ELF identification padding; guest code/data stay unchanged.
        Input input(mainBytes, guestBytes);
        const auto originalMain = Cpu::ParseSce(input.main), originalGuest = Cpu::ParseSce(input.guest);
        {
            std::ofstream stream(input.guest, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(poisoned.data()), poisoned.size());
            require(bool(stream), "Cannot replace same-name consumer fixture");
        }
        const auto changedGuest = Cpu::ParseSce(input.guest);
        require(changedGuest.Path == originalGuest.Path && changedGuest.SourceSha256 != originalGuest.SourceSha256 &&
                (changeSize ? changedGuest.SourceSize != originalGuest.SourceSize : changedGuest.SourceSize == originalGuest.SourceSize),
                "Tamper control did not isolate source digest or size under the same filename");
        execute(input.main, input.guest, false); // Old unqualified owner still admits the poisoned consumer.
        Cpu::Machine machine;
        Cpu::SceImports imports(machine);
        unsigned legacyCalls = 0, poisonedCalls = 0;
        const std::array dependencies{Cpu::SceModuleFile{input.guest, GuestBias}};
        rejects([&] { Cpu::SceModules graph(machine, {input.main, MainBias}, dependencies, hostModules,
            [&](const auto& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
                ++legacyCalls;
                return Cpu::SceResolvedImport{imports.Resolve(import), type};
            }, std::nullopt,
            [&](const auto& consumer, const auto& import, std::uint8_t type, std::uint64_t) -> std::optional<Cpu::SceResolvedImport> {
                if (consumer.Path == originalGuest.Path) {
                    require(sameConsumer(consumer, changedGuest), "Tampered consumer reused a stale parsed source identity");
                    ++poisonedCalls;
                }
                if (!sameConsumer(consumer, originalMain) && !sameConsumer(consumer, originalGuest)) return std::nullopt;
                return Cpu::SceResolvedImport{imports.Resolve(import), type};
            }); }, "unresolved or wrongly typed host import");
        require(poisonedCalls == 1 && !legacyCalls, "Changed same-name consumer inherited an earlier host grant");
    }
}

struct CrtReceipt {
    std::string Kind;
    std::filesystem::path Main;
    std::filesystem::path Guest;
    Cpu::SceCrtCertificate Certificate;
    std::uint64_t State;
};

std::vector<CrtReceipt> crtReceipts(const std::filesystem::path& path) {
    std::ifstream input(path);
    require(bool(input), "Cannot open independent CRT fixture receipt");
    std::vector<CrtReceipt> receipts;
    std::string line;
    while (std::getline(input, line)) {
        std::istringstream record(line);
        CrtReceipt receipt;
        std::string filename, digest;
        require(bool(record >> receipt.Kind >> filename >> digest), "Invalid CRT fixture receipt");
        require((receipt.Kind == "elf" || receipt.Kind == "plain_self") && digest.size() == 64,
                "Invalid CRT fixture source kind or digest");
        constexpr std::string_view hex = "0123456789abcdef";
        for (unsigned index = 0; index < receipt.Certificate.SourceSha256.size(); ++index) {
            const auto high = hex.find(digest[index * 2]);
            const auto low = hex.find(digest[index * 2 + 1]);
            require(high != std::string_view::npos && low != std::string_view::npos, "Invalid CRT fixture SHA-256");
            receipt.Certificate.SourceSha256[index] = static_cast<std::byte>((high << 4) | low);
        }
        std::array<std::uint64_t, 10> values{};
        for (auto& value : values) {
            std::string field;
            require(bool(record >> field), "Incomplete CRT fixture layout");
            std::size_t end = 0;
            value = std::stoull(field, &end, 0);
            require(end == field.size(), "Invalid CRT fixture layout integer");
        }
        std::string extra;
        require(!(record >> extra), "Unexpected CRT fixture receipt fields");
        const auto owner = [](std::uint64_t size, Cpu::SceCrtArrayOwner expected) {
            return size ? expected : Cpu::SceCrtArrayOwner::Unsupported;
        };
        receipt.Certificate.SourceSize = values[0];
        receipt.Certificate.Init = values[1];
        receipt.Certificate.Fini = values[2];
        receipt.Certificate.Preinit = {values[3], values[4], owner(values[4], Cpu::SceCrtArrayOwner::DtInit)};
        receipt.Certificate.InitArray = {values[5], values[6], owner(values[6], Cpu::SceCrtArrayOwner::DtInit)};
        receipt.Certificate.FiniArray = {values[7], values[8], owner(values[8], Cpu::SceCrtArrayOwner::DtFini)};
        receipt.State = values[9];
        receipt.Main = path.parent_path() / "sce-crt-main.elf";
        receipt.Guest = path.parent_path() / filename;
        receipts.push_back(std::move(receipt));
    }
    require(receipts.size() == 2 && receipts[0].Kind == "elf" && receipts[1].Kind == "plain_self" &&
            receipts[0].Certificate.SourceSize > 1024 * 1024,
            "Independent CRT receipt must cover chunked ELF hashing and original SELF identity");
    return receipts;
}

void requireCrtState(Cpu::Machine& machine, std::uint64_t state, const std::array<std::uint64_t, 9>& expected) {
    for (unsigned index = 0; index < expected.size(); ++index)
        require(word(machine, state + index * 8) == expected[index],
                "Compiled CRT violated callback ordering, counters, or independent arithmetic result");
}

void executeCrt(const CrtReceipt& receipt) {
    Cpu::Machine machine;
    Cpu::SceImports imports(machine);
    const std::array dependencies{Cpu::SceModuleFile{receipt.Guest, GuestBias, receipt.Certificate}};
    Cpu::SceModules graph(machine, {receipt.Main, MainBias}, dependencies, hostModules,
        [&](const auto& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
            require(type == 2, "CRT fixture unexpectedly needs a host data or TLS provider");
            return Cpu::SceResolvedImport{imports.Resolve(import), type};
        });
    const auto state = GuestBias + receipt.State;
    requireCrtState(machine, state, {0, 0, 0, 0, 0, 0, 0, 5, 0});
    Cpu::SetupSceEntry(machine, graph.Main(), {"crt-fixture"}, imports.ExitGate());
    graph.InitializeDependencies();
    requireCrtState(machine, state, {123, 1, 0, 1, 1, 0, 0, 82, 0});
    require(machine.Run(graph.Main().Entry, 0, 1000000) == Cpu::StopReason::Exit && machine.ExitCode() == 0,
            "Actual compiled CRT main rejected initialization state");
    requireCrtState(machine, state, {1234, 1, 0, 1, 1, 0, 0, 577, 1});
    graph.FinalizeDependencies();
    requireCrtState(machine, state, {1234576, 1, 1, 1, 1, 1, 1, 82583, 1});
}

void invalidGraphs(const std::filesystem::path& mainPath, const std::filesystem::path& guestPath,
                   std::span<const CrtReceipt> receipts) {
    const auto mainBytes = readFile(mainPath);
    const auto guestBytes = readFile(guestPath);
    struct Case { const char* Expected; std::function<void(Bytes&, Bytes&)> Change; bool Early = true; };
    const auto crtArray = [](std::uint64_t pointerTag, std::uint64_t sizeTag, std::uint64_t functionTag) {
        return [=](Bytes&, Bytes& guest) {
            const auto dynlib = get(guest, program(guest, 0x61000000) + 8);
            const auto relocations = dynlib + get(guest, tag(guest, 0x6100002f) + 8);
            const auto size = get(guest, tag(guest, 0x61000031) + 8);
            for (std::uint64_t offset = relocations; offset < relocations + size; offset += 24) {
                if ((get(guest, offset + 8) & 0xffffffff) != 8) continue;
                const auto target = get(guest, offset);
                fileOffset(guest, target);
                fileOffset(guest, target + 7);
                put(guest, offset + 16, get(guest, tag(guest, functionTag) + 8));
                const auto pointer = tag(guest, 0x61000017);
                const auto length = tag(guest, 0x6ffffff9);
                put(guest, pointer, pointerTag);
                put(guest, pointer + 8, target);
                put(guest, length, sizeTag);
                put(guest, length + 8, 8);
                return;
            }
            throw std::runtime_error("Compiled fixture lacks a mapped RELATIVE slot for the CRT array contract");
        };
    };
    const std::array cases{
        Case{"unsupported CRT-array ownership", crtArray(32, 33, 12)},
        Case{"unsupported CRT-array ownership", crtArray(25, 27, 12)},
        Case{"unsupported CRT-array ownership", crtArray(26, 28, 13)},
        Case{"named module/version", [](auto&, auto& guest) { const auto location = tag(guest, 0x61000043) + 8; put(guest, location, get(guest, location) ^ (3ull << 40)); }},
        Case{"imported library attributes", [](auto& main, auto&) { libraryAttributes(main, 2, 1); }},
        Case{"imported library attributes", [](auto& main, auto&) { libraryAttributes(main, 4, 1); }},
        Case{"imported library attributes", [](auto& main, auto&) { libraryAttributes(main, 0xb, 1); }},
        Case{"imported library attributes", [](auto& main, auto&) { libraryAttributes(main, 0xd, 1); }},
        Case{"imported library attributes", [](auto& main, auto&) { libraryAttributes(main, (1ull << 47) | 9, 1); }},
        Case{"exported library attributes", [](auto&, auto& guest) { libraryAttributes(guest, 9, 2); }},
        Case{"exported library attributes", [](auto&, auto& guest) { libraryAttributes(guest, 9, 8); }},
        Case{"exported library attributes", [](auto&, auto& guest) { libraryAttributes(guest, 9, 3); }},
        Case{"exported library attributes", [](auto&, auto& guest) { libraryAttributes(guest, 9, (1ull << 47) | 1); }},
        Case{"unsupported dynamic", [](auto&, auto& guest) { put(guest, tag(guest, 0x61000017), 0x61234567); }},
        Case{"typed import scope/version", [](auto& main, auto&) {
            const auto dynlib = get(main, program(main, 0x61000000) + 8);
            const auto strings = dynlib + get(main, tag(main, 0x61000035) + 8);
            const auto symbols = dynlib + get(main, tag(main, 0x61000039) + 8);
            const auto size = get(main, tag(main, 0x6100003f) + 8);
            for (auto offset = symbols; offset < symbols + size; offset += 24) {
                if (get(main, offset + 6, 2) || (get(main, offset + 4, 1) & 15) != 1) continue;
                const auto name = strings + get(main, offset, 4);
                put(main, name, get(main, name, 1) == 'Q' ? 'R' : 'Q', 1);
                return;
            }
            throw std::runtime_error("Compiled fixture lacks an object import for the wrong-NID contract");
        }, false},
        Case{"symbol type mismatch", [](auto& main, auto&) { changeImportType(main, "L+OvOB7GHzo", 2); }, false},
        Case{"typed import scope/version", [](auto&, auto& guest) {
            const auto location = tag(guest, 0x61000047) + 8;
            put(guest, location, get(guest, location) ^ (3ull << 32));
        }, false},
    };
    for (const auto& test : cases) {
        auto changedMain = mainBytes, changedGuest = guestBytes;
        test.Change(changedMain, changedGuest);
        Input input(changedMain, changedGuest);
        Cpu::Machine machine;
        Cpu::SceImports imports(machine);
        const auto existingRanges = machine.Mappings().size();
        unsigned resolutions = 0;
        const std::array dependencies{Cpu::SceModuleFile{input.guest, GuestBias}};
        rejects([&] { Cpu::SceModules graph(machine, {input.main, MainBias}, dependencies, hostModules,
            [&](const auto& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
                ++resolutions;
                return Cpu::SceResolvedImport{imports.Resolve(import), type};
            }); }, test.Expected);
        require((!test.Early || resolutions == 0) && machine.Get(Cpu::Register::FsBase) == 0 &&
                machine.Mappings().size() == existingRanges,
                "Invalid module metadata reached host resolution, installed TLS, or mapped guest storage before rejection");
    }
    {
        Cpu::Machine machine;
        rejects([&] { Cpu::SceModules graph(machine, {mainPath, MainBias}, {}, hostModules, {}); }, "missing DT_NEEDED");
    }
    {
        Cpu::Machine machine;
        const std::array dependencies{Cpu::SceModuleFile{guestPath, MainBias}};
        rejects([&] { Cpu::SceModules graph(machine, {mainPath, MainBias}, dependencies, hostModules, {}); }, "mapping ranges overlap");
    }
    struct CertificateCase {
        const char* Expected;
        std::function<void(std::optional<Cpu::SceCrtCertificate>&)> Change;
    };
    const std::array certificateCases{
        CertificateCase{"unsupported CRT-array ownership", [](auto& crt) { crt.reset(); }},
        CertificateCase{"source identity mismatch", [](auto& crt) { crt->SourceSha256[0] ^= std::byte{1}; }},
        CertificateCase{"source identity mismatch", [](auto& crt) { --crt->SourceSize; }},
        CertificateCase{"layout mismatch", [](auto& crt) { ++crt->Init; }},
        CertificateCase{"layout mismatch", [](auto& crt) { ++crt->Fini; }},
        CertificateCase{"layout mismatch", [](auto& crt) { crt->InitArray.Address += 8; }},
        CertificateCase{"layout mismatch", [](auto& crt) { crt->FiniArray.Size += 8; }},
        CertificateCase{"array owner mismatch", [](auto& crt) { crt->InitArray.Owner = Cpu::SceCrtArrayOwner::DtFini; }},
        CertificateCase{"array owner mismatch", [](auto& crt) { crt->FiniArray.Owner = Cpu::SceCrtArrayOwner::DtInit; }},
        CertificateCase{"array owner mismatch", [](auto& crt) { crt->Preinit.Owner = Cpu::SceCrtArrayOwner::DtInit; }},
    };
    if (!receipts.empty()) {
        const auto& receipt = receipts.front();
        for (const auto& test : certificateCases) {
            std::optional crt{receipt.Certificate};
            test.Change(crt);
            Cpu::Machine machine;
            Cpu::SceImports imports(machine);
            const auto existingRanges = machine.Mappings().size();
            unsigned resolutions = 0;
            const std::array dependencies{Cpu::SceModuleFile{receipt.Guest, GuestBias, crt}};
            rejects([&] { Cpu::SceModules graph(machine, {receipt.Main, MainBias}, dependencies, hostModules,
                [&](const auto& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
                    ++resolutions;
                    return Cpu::SceResolvedImport{imports.Resolve(import), type};
                }); }, test.Expected);
            require(!resolutions && machine.Get(Cpu::Register::FsBase) == 0 && machine.Mappings().size() == existingRanges,
                    "Invalid CRT certificate reached host resolution, TLS, or mapped guest storage");
        }
        auto changedSource = readFile(receipt.Guest);
        changedSource.back() ^= std::byte{1};
        Input sourceInput(readFile(receipt.Main), changedSource, "SceCrtGuest.prx");
        Cpu::Machine machine;
        Cpu::SceImports imports(machine);
        const auto existingRanges = machine.Mappings().size();
        unsigned resolutions = 0;
        const std::array dependencies{Cpu::SceModuleFile{sourceInput.guest, GuestBias, receipt.Certificate}};
        rejects([&] { Cpu::SceModules graph(machine, {sourceInput.main, MainBias}, dependencies, hostModules,
            [&](const auto& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
                ++resolutions;
                return Cpu::SceResolvedImport{imports.Resolve(import), type};
            }); }, "source identity mismatch");
        require(!resolutions && machine.Get(Cpu::Register::FsBase) == 0 && machine.Mappings().size() == existingRanges,
                "Changed final source chunk was accepted before guest mutation");
    }
    auto changedGuest = guestBytes;
    const auto init = get(changedGuest, tag(changedGuest, 12) + 8);
    const auto location = fileOffset(changedGuest, init);
    put(changedGuest, location, 0xb8, 1);
    put(changedGuest, location + 1, 7, 4);
    put(changedGuest, location + 5, 0xc3, 1);
    Input input(mainBytes, changedGuest);
    Cpu::Machine machine;
    Cpu::SceImports imports(machine);
    const std::array dependencies{Cpu::SceModuleFile{input.guest, GuestBias}};
    Cpu::SceModules graph(machine, {input.main, MainBias}, dependencies, hostModules,
        [&](const auto& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> { return Cpu::SceResolvedImport{imports.Resolve(import), type}; });
    Cpu::SetupSceEntry(machine, graph.Main(), {"fixture"}, imports.ExitGate());
    rejects([&] { graph.InitializeDependencies(); }, "failure status");
    require(word(machine, object(graph.Modules()[0], "xHwC9ebKTAY")) == 0,
            "Failed dependency initialization reached main-owned initialization");
    rejects([&] { graph.FinalizeDependencies(); }, "successful initialization");
}
}

int main(int argc, char** argv) {
    try {
        require(argc == 3 || argc == 4, "Usage: SceModulesTests main.elf dependency.prx [crt-receipt.txt]");
        auto mainBytes = readFile(argv[1]), guestBytes = readFile(argv[2]);
        libraryAttributes(mainBytes, 9, 1);
        libraryAttributes(guestBytes, 9, 1);
        Input attributed(mainBytes, guestBytes);
        execute(attributed.main, attributed.guest, false);
        execute(attributed.main, attributed.guest, true);
        execute(attributed.main, attributed.guest, false, true);
        auto sizedMain = mainBytes;
        changeImportSize(sizedMain, LengthNid, 17);
        Input sizedInput(sizedMain, guestBytes);
        execute(sizedInput.main, sizedInput.guest, false, true, false, false, 17);
        consumerFailures(attributed.main, attributed.guest);
        auto hostObjectMain = mainBytes;
        redirectHostObject(hostObjectMain);
        changeImportSize(hostObjectMain, "OD70jrfOzHM", 8);
        Input hostObjectInput(hostObjectMain, guestBytes);
        execute(hostObjectInput.main, hostObjectInput.guest, false, true, true);
        consumerFailures(hostObjectInput.main, hostObjectInput.guest, true);
        Input snapshotInput(mainBytes, guestBytes);
        execute(snapshotInput.main, snapshotInput.guest, false, true, false, true);
        const auto receipts = argc == 4 ? crtReceipts(argv[3]) : std::vector<CrtReceipt>{};
        for (const auto& receipt : receipts) executeCrt(receipt);
        invalidGraphs(attributed.main, attributed.guest, receipts);
        std::cout << "PASS compiled SCE module graph calls, objects, TLS, independent prime/Adler results, dependency-only lifecycle, and strict failures\n";
        std::cout << "PASS per-consumer host FUNC/OBJECT authority, same-name source tampering, strict rejection without fallback, and parsed-snapshot mapping\n";
        if (!receipts.empty())
            std::cout << "PASS original ELF/SELF certification, chunked source identity, compiled CRT ordering, and certificate preflight failures\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
