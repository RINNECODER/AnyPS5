#include <cpu/SceImports.hpp>
#include <cpu/SceModules.hpp>
#include <cpu/SceTls.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

namespace {
using Bytes = std::vector<std::byte>;
constexpr std::uint64_t MainBias = 0x1000000;
constexpr std::uint64_t GuestBias = 0x2000000;

void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
Bytes readFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    require(bool(stream), "Cannot open compiled module fixture");
    const auto size = stream.tellg();
    require(size > 0 && size < 1024 * 1024, "Compiled module fixture size is invalid");
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

void execute(const std::filesystem::path& mainPath, const std::filesystem::path& guestPath, bool wrongOracle) {
    Cpu::Machine machine;
    Cpu::SceImports imports(machine);
    const std::array dependencies{Cpu::SceModuleFile{guestPath, GuestBias}};
    Cpu::SceModules graph(machine, {mainPath, MainBias}, dependencies, hostModules,
        [&](const auto& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
            require(type == 2, "Fixture unexpectedly requires a host data or TLS service");
            return Cpu::SceResolvedImport{imports.Resolve(import), 2, 0, 0, 0};
        });
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
    Input(const Bytes& mainBytes, const Bytes& guestBytes) {
        directory = std::filesystem::temp_directory_path() / ("anyps5-modules-" + std::to_string(::getpid()) + "-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(directory), "Cannot create copied module fixture directory");
        main = directory / "sce-module-main.elf";
        guest = directory / "SceModuleGuest.prx";
        for (const auto& [path, bytes] : std::array{std::pair{main, mainBytes}, std::pair{guest, guestBytes}}) {
            std::ofstream stream(path, std::ios::binary);
            stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            require(bool(stream), "Cannot write copied module fixture");
        }
    }
    ~Input() { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
};

void invalidGraphs(const std::filesystem::path& mainPath, const std::filesystem::path& guestPath) {
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
        Case{"exported library attributes", [](auto&, auto& guest) { put(guest, tag(guest, 0x61000017) + 8, 1); }},
        Case{"unsupported dynamic", [](auto&, auto& guest) { put(guest, tag(guest, 0x61000017), 0x61234567); }},
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
        unsigned resolutions = 0;
        const std::array dependencies{Cpu::SceModuleFile{input.guest, GuestBias}};
        rejects([&] { Cpu::SceModules graph(machine, {input.main, MainBias}, dependencies, hostModules,
            [&](const auto& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
                ++resolutions;
                return Cpu::SceResolvedImport{imports.Resolve(import), type};
            }); }, test.Expected);
        require((!test.Early || resolutions == 0) && machine.Get(Cpu::Register::FsBase) == 0,
                "Invalid module metadata reached host resolution or installed TLS before rejection");
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
        require(argc == 3, "Usage: SceModulesTests main.elf dependency.prx");
        execute(argv[1], argv[2], false);
        execute(argv[1], argv[2], true);
        invalidGraphs(argv[1], argv[2]);
        std::cout << "PASS compiled SCE module graph calls, objects, TLS, independent prime/Adler results, dependency-only lifecycle, and strict failures\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
