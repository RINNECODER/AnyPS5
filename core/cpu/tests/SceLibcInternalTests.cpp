#include <cpu/SceLifecycleImports.hpp>
#include <cpu/SceModules.hpp>
#include <array>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
constexpr std::uint64_t MainBias = 0x1000000, ConsumerBias = 0x2000000, GuestBias = 0x3000000;
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
template<class Function> void rejects(Function&& function, const char* expected) {
    try { function(); }
    catch (const std::exception& error) {
        if (std::string(error.what()).find(expected) == std::string::npos)
            throw std::runtime_error(std::string("Wrong Internal contract rejection: ") + error.what());
        std::cout << "PASS rejection " << expected << '\n'; return;
    }
    throw std::runtime_error(std::string("Missing Internal contract rejection: ") + expected);
}
struct Receipt {
    std::filesystem::path Root;
    std::array<std::uint64_t, 14> Addresses{};
    struct Identity { std::array<std::byte, 32> Digest{}; std::uint64_t Size = 0; };
    std::map<std::string, Identity> Cases;
    explicit Receipt(const std::filesystem::path& path) : Root(path.parent_path()) {
        std::ifstream input(path); require(bool(input), "Cannot open independent Internal receipt");
        std::string line; require(bool(std::getline(input, line)), "Missing linker address receipt");
        std::istringstream addresses(line);
        for (auto& address : Addresses) require(bool(addresses >> address), "Missing linker symbol value");
        while (std::getline(input, line)) {
            std::istringstream record(line); std::string name, digest; Identity identity;
            require(bool(record >> name >> digest >> identity.Size) && digest.size() == 64, "Invalid source receipt");
            for (unsigned i = 0; i < 32; ++i) identity.Digest[i] = std::byte(std::stoul(digest.substr(i * 2, 2), nullptr, 16));
            require(Cases.emplace(name, identity).second, "Duplicate Internal case receipt");
        }
    }
};
template<std::size_t Size> std::array<std::uint64_t, Size> words(Cpu::Machine& machine, std::uint64_t address) {
    std::array<std::uint64_t, Size> result;
    machine.Read(address, std::as_writable_bytes(std::span(result))); return result;
}
void graph(const Receipt& receipt, const std::string& name, unsigned policy = 0) {
    Cpu::Machine machine;
    Cpu::SceLifecycleImports lifecycle(machine);
    const auto root = receipt.Root / name;
    const std::array dependencies{Cpu::SceModuleFile{root / "SceInternalConsumer.prx", ConsumerBias},
                                 Cpu::SceModuleFile{root / "SceInternalLibc.prx", GuestBias}};
    std::vector hosts{
        Cpu::SceHostModule{"libkernel.prx", {"libkernel", 0, 1, 1}, {{"libkernel", 0, 1}}},
        Cpu::SceHostModule{"libSceLibcInternal.sprx", {"libSceLibcInternal", 0, 1, 1}, {{"libSceLibcInternal", 0, 1}}}};
    if (policy == 5) hosts.push_back({"OtherInternal.prx", {"libSceLibcInternal", 0, 1, 1}, {{"libSceLibcInternal", 0, 1}}});
    auto resolver = [&](const Cpu::SceImport& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
        if (import.ModuleName != "libkernel") return std::nullopt;
        const auto address = lifecycle.Resolve(import);
        return address ? std::optional(Cpu::SceResolvedImport{*address, type}) : std::nullopt;
    };
#ifdef CPU04_BASELINE
    Cpu::SceModules modules(machine, {root / "SceInternalMain.elf", MainBias}, dependencies, hosts, resolver);
#else
    const auto identity = receipt.Cases.at(name);
    std::optional<Cpu::SceLibcInternalProvider> provider = Cpu::SceLibcInternalProvider{"SceInternalLibc.prx", identity.Digest, identity.Size};
    if (policy == 1) provider.reset();
    if (policy == 2) provider->SourceSha256[0] ^= std::byte{1};
    if (policy == 3) ++provider->SourceSize;
    if (policy == 4) provider->Filename = "different.prx";
    Cpu::SceModules modules(machine, {root / "SceInternalMain.elf", MainBias}, dependencies, hosts, resolver, provider);
#endif
    require(words<11>(machine, GuestBias + receipt.Addresses[0]) == std::array<std::uint64_t, 11>{},
            "Loading executed libc-owned guest state");
    const auto actual = words<9>(machine, MainBias + receipt.Addresses[2]);
    for (unsigned i = 0; i < actual.size(); ++i)
        require(actual[i] == GuestBias + receipt.Addresses[4 + i], "Internal relocation did not preserve supplied guest function destination");
    Cpu::SceImport exit; exit.Nid = "6Z83sYWFlA8"; exit.ModuleName = exit.LibraryName = "libkernel";
    exit.LibraryVersion = exit.ModuleMajor = exit.ModuleMinor = 1;
    Cpu::SetupSceEntry(machine, modules.Main(), {"internal-forwarding"}, lifecycle.Resolve(exit).value());
    modules.InitializeDependencies();
    require(words<11>(machine, GuestBias + receipt.Addresses[0]) == std::array<std::uint64_t, 11>{1, 0, 1},
            "Supplied guest provider initialization did not execute once before entry");
    require(words<1>(machine, ConsumerBias + receipt.Addresses[13])[0] == 0x006b6f726564726fULL,
            "Forwarded consumer initializer did not run after provider initialization");
    require(machine.Run(modules.Main().Entry, 0, 1000000) == Cpu::StopReason::Exit && machine.ExitCode() == 0,
            "Compiled guest rejected memory/string pointer returns, comparisons or variadic register/stack/XMM arguments");
    std::array<unsigned char, 160> output;
    machine.Read(MainBias + receipt.Addresses[3], std::as_writable_bytes(std::span(output)));
    std::array<unsigned char, 160> expected; expected.fill(0xa7);
    const auto put = [&](unsigned offset, std::string_view text) {
        for (unsigned i = 0; i < text.size(); ++i) expected[offset + i] = static_cast<unsigned char>(text[i]);
    };
    put(1, "native"); put(8, "444"); put(16, std::string_view("xy\0\0\0", 5)); put(24, "lon");
    put(40, std::string_view("guest:11,22,33,44,55,66,77:3.5\0", 31)); put(112, std::string_view("gues\0", 5));
    require(output == expected, "Independent byte oracle disagreed with guest copy/set/padding/truncation/output or adjacent-byte preservation");
    std::array<char, 9> printed;
    machine.Read(GuestBias + receipt.Addresses[1], std::as_writable_bytes(std::span(printed)));
    require(printed == std::array<char, 9>{'p', 'r', 'i', 'n', 't', ':', '9', '1', 0}, "printf changed guest-owned destination or integer ABI");
    modules.FinalizeDependencies();
    require(words<11>(machine, GuestBias + receipt.Addresses[0]) == std::array<std::uint64_t, 11>{1, 1, 2, 1, 1, 2, 3, 2, 3, 1, 4},
            "Guest calls/finalization counters disagreed with independent provider lifetime oracle");
    rejects([&] { modules.FinalizeDependencies(); }, "successful initialization");
    std::cout << "PASS nine actual x86 Internal relocations to supplied guest destinations, independent byte/varargs and lifecycle oracles\n";
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "Usage: SceLibcInternalTests receipt.txt"); Receipt receipt(argv[1]);
#ifdef CPU04_BASELINE
        rejects([&] { graph(receipt, "valid"); }, "unresolved or wrongly typed host import");
        std::cout << "PASS pre-fix compiled guest fails only at unresolved Internal import\n";
#else
        graph(receipt, "valid");
        for (unsigned policy : {1, 2, 3, 4, 5}) rejects([&] { graph(receipt, "valid", policy); },
            policy == 1 ? "unresolved or wrongly typed host import" : policy < 4 ? "source identity mismatch" :
            policy == 4 ? "missing supplied libc Internal source provider" : "ambiguous named module/version provider");
        for (const auto& [name, reason] : std::array{
            std::pair{"consumer-library", "unresolved typed import scope/version provider"},
            std::pair{"consumer-module", "missing named module/version provider"},
            std::pair{"consumer-library-version", "unresolved typed import scope/version provider"},
            std::pair{"consumer-module-major", "missing named module/version provider"},
            std::pair{"consumer-module-minor", "missing named module/version provider"},
            std::pair{"consumer-type", "unresolved or wrongly typed host import"},
            std::pair{"consumer-nid", "unresolved or wrongly typed host import"},
            std::pair{"matched-unsupported-nid", "unresolved or wrongly typed host import"},
            std::pair{"provider-type", "invalid libc Internal target function"},
            std::pair{"provider-ambiguity", "ambiguous libc Internal target export"},
            std::pair{"provider-storage", "no executable file-backed storage"},
            std::pair{"provider-permission", "SCE ELF loader: unmapped or inaccessible guest range"},
            std::pair{"provider-relro", "loses execute permission to RELRO"},
            std::pair{"provider-module", "target module identity mismatch"},
            std::pair{"provider-library", "missing qualified libc Internal target export"},
            std::pair{"provider-library-version", "missing qualified libc Internal target export"}})
            rejects([&] { graph(receipt, name); }, reason);
#endif
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; return 1; }
}
