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
Cpu::SceImportConsumer certificate() {
    // Static recorded selector metadata only. It is never used to relabel a
    // parsed public image, map a target image, or execute a target gate.
    Cpu::SceImportConsumer consumer;
    consumer.Path = "eboot.bin"; consumer.SourceSize = 102560655;
    constexpr char hash[] = "a6df51ec222136f337f86e9be5fa3013417ddc44bc22a6c8d514c0199cf8c397";
    const auto digit = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (unsigned i = 0; i < 32; ++i)
        consumer.SourceSha256[i] = static_cast<std::byte>((digit(hash[i * 2]) << 4) | digit(hash[i * 2 + 1]));
    return consumer;
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
    const auto target = certificate();
    const auto mappingsBefore = machine.Mappings().size();
    std::vector<std::uint64_t> gates;
    for (const auto* nid : {"188x57JYp0g", "R1Jvn8bSCW8", "Zxa0VhQVTsk", "4czppHBiriw"}) {
        const auto import = semaphore(nid);
        const auto accepted = runner.Resolve(target, import, 2, 0);
        require(accepted && accepted->Address && accepted->Type == 2 && accepted->Size == 0,
                "Exact static semaphore selector metadata failed native routing");
        // Establish the recognized route first so missing production wiring
        // cannot make a negative-source control pass through another guard.
        // The parsed public synthetic image keeps its actual hash/size/name.
        denied([&] { runner.Resolve(actual, import, 2, 0); }, "Parsed public source was not explicitly rejected by recognized semaphore route");
        for (const auto gate : gates) require(gate != accepted->Address, "Four contracts reused one callable gate");
        gates.push_back(accepted->Address);
        const auto repeated = runner.Resolve(target, import, 2, 0);
        require(repeated && repeated->Address == accepted->Address, "Repeated selection changed live gate identity");
        for (const auto type : {std::uint8_t{0}, std::uint8_t{1}, std::uint8_t{6}})
            denied([&] { runner.Resolve(target, import, type, 0); }, "Nonfunction semaphore import admitted");
        denied([&] { runner.Resolve(target, import, 2, 8); }, "Sized semaphore import admitted");
        for (unsigned field = 0; field < 3; ++field) {
            auto bad = target;
            if (field == 0) bad.Path = "other.bin";
            if (field == 1) bad.SourceSha256[0] ^= std::byte{1};
            if (field == 2) --bad.SourceSize;
            denied([&] { runner.Resolve(bad, import, 2, 0); }, "Wrong actual consumer name/hash/size admitted");
        }
        for (unsigned field = 0; field < 7; ++field) {
            auto bad = import;
            if (field == 0) bad.LibraryName = "other";
            if (field == 1) bad.ModuleName = "other";
            if (field == 2) ++bad.LibraryId;
            if (field == 3) ++bad.ModuleId;
            if (field == 4) ++bad.LibraryVersion;
            if (field == 5) ++bad.ModuleMajor;
            if (field == 6) ++bad.ModuleMinor;
            denied([&] { runner.Resolve(target, bad, 2, 0); }, "Wrong semaphore import scope/version admitted");
        }
    }
    require(machine.Mappings().size() == mappingsBefore, "Rejected selectors allocated new provider mappings");
    for (const auto* unknown : {"12wOHk8ywb0", "4DM06U2BNEY", "unknown-semaphore"})
        require(!runner.Resolve(actual, semaphore(unknown), 2, 0), "Unqualified Poll/Cancel or unknown import claimed");
    runner.Shutdown(); runner.Shutdown();
    for (const auto gate : gates)
        denied([&] { machine.CheckAccess(gate, 1, Cpu::Permission::Execute); }, "Shutdown retained semaphore callable mapping");
    denied([&] { runner.Resolve(target, semaphore("188x57JYp0g"), 2, 0); }, "Resolution after native shutdown succeeded");
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 3, "Usage: NativeSemaphoreRunnerTest public.elf utility.metallib");
        run(argv[1], argv[2]);
        std::cout << "Native semaphore source selector and owned shutdown PASS; static metadata admission is not target execution\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
