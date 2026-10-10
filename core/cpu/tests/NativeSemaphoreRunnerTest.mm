#include <cpu/NativeModuleRunner.hpp>
#include <cpu/GuestThreads.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void denied(F operation, const char* message) {
    bool rejected = false;
    try { operation(); } catch (const std::runtime_error&) { rejected = true; }
    require(rejected, message);
}
Cpu::SceImport semaphore(const char* nid) {
    Cpu::SceImport import;
    import.Nid = nid; import.LibraryName = import.ModuleName = "libkernel";
    import.LibraryId = 44; import.ModuleId = 24;
    import.LibraryVersion = import.ModuleMajor = import.ModuleMinor = 1;
    return import;
}
void run(const char* publicImage, const char* utility) {
    const auto parsed = Cpu::ParseSce(publicImage);
    const Cpu::SceImportConsumer actual{publicImage, parsed.SourceSize, parsed.SourceSha256};
    Cpu::Machine machine;
    auto threads = std::make_shared<Cpu::GuestThreads>(machine);
    Cpu::NativeModuleRunnerConfiguration config;
    config.UtilityMetallib = utility;
    config.WindowTitle = "Native semaphore source selector fixture";
    config.Width = config.Height = 64;
    Cpu::NativeModuleRunner runner(machine, threads, actual, config);
    // Any importing image is admitted: the parsed public image keeps its actual
    // name/hash/size, and a synthetic second image shares the same gates.
    Cpu::SceImportConsumer other{"game_module.prx", 4242, {}};
    other.SourceSha256.fill(std::byte{0x5a});
    const auto mappingsBefore = machine.Mappings().size();
    std::vector<std::uint64_t> gates;
    for (const auto* nid : {"188x57JYp0g", "R1Jvn8bSCW8", "Zxa0VhQVTsk", "4czppHBiriw", "12wOHk8ywb0", "4DM06U2BNEY"}) {
        const auto import = semaphore(nid);
        const auto accepted = runner.Resolve(actual, import, 2, 0);
        require(accepted && accepted->Address && accepted->Type == 2 && accepted->Size == 0,
                "Semaphore import from the parsed public image failed native routing");
        for (const auto gate : gates) require(gate != accepted->Address, "Two semaphore contracts reused one callable gate");
        gates.push_back(accepted->Address);
        const auto repeated = runner.Resolve(other, import, 2, 0);
        require(repeated && repeated->Address == accepted->Address, "Another image's same row changed live gate identity");
        for (const auto type : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{6}})
            denied([&] { runner.Resolve(actual, import, type, 0); }, "Nonfunction semaphore import admitted");
        denied([&] { runner.Resolve(actual, import, 2, 8); }, "Sized semaphore import admitted");
        for (unsigned field = 0; field < 5; ++field) {
            auto bad = import;
            if (field == 0) bad.LibraryName = "other";
            if (field == 1) bad.ModuleName = "other";
            if (field == 2) ++bad.LibraryVersion;
            if (field == 3) ++bad.ModuleMajor;
            if (field == 4) ++bad.ModuleMinor;
            denied([&] { runner.Resolve(actual, bad, 2, 0); }, "Wrong semaphore import scope/version admitted");
        }
        // Import-table ids are per-image; another image's ids get their own gate.
        auto ids = import; ++ids.LibraryId; ++ids.ModuleId;
        const auto moved = runner.Resolve(other, ids, 2, 0);
        require(moved && moved->Address && moved->Address != accepted->Address,
                "Another image's semaphore import-table ids were refused");
        gates.push_back(moved->Address);
    }
    require(machine.Mappings().size() == mappingsBefore, "Semaphore selection allocated new provider mappings");
    require(!runner.Resolve(actual, semaphore("unknown-semaphore"), 2, 0), "Unknown semaphore import claimed");
    runner.Shutdown(); runner.Shutdown();
    for (const auto gate : gates)
        denied([&] { machine.CheckAccess(gate, 1, Cpu::Permission::Execute); }, "Shutdown retained semaphore callable mapping");
    denied([&] { runner.Resolve(actual, semaphore("188x57JYp0g"), 2, 0); }, "Resolution after native shutdown succeeded");
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 3, "Usage: NativeSemaphoreRunnerTest public.elf utility.metallib");
        run(argv[1], argv[2]);
        std::cout << "Native semaphore title-agnostic selector and owned shutdown PASS; metadata admission is not target execution\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
