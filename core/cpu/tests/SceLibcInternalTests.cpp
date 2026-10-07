#include <cpu/SceLifecycleImports.hpp>
#include <cpu/GuestThreads.hpp>
#include <cpu/SceThreadImports.hpp>
#include <cpu/SceModules.hpp>
#include <array>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace {
constexpr std::uint64_t MainBias = 0x100000000, ConsumerBias = 0x200000000, GuestBias = 0x300000000;
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
    std::array<std::uint64_t, 35> Addresses{};
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
void graph(const Receipt& receipt, const std::string& name, unsigned policy = 0, unsigned probe = 0) {
    Cpu::Machine machine;
    Cpu::SceLifecycleImports lifecycle(machine);
    auto threads = std::make_shared<Cpu::GuestThreads>(machine);
    Cpu::SceThreadImports kernel(machine, threads);
    const auto root = receipt.Root / name;
    const std::array dependencies{Cpu::SceModuleFile{root / "SceInternalConsumer.prx", ConsumerBias},
                                 Cpu::SceModuleFile{root / "SceInternalLibc.prx", GuestBias}};
    std::vector hosts{
        Cpu::SceHostModule{"libkernel.prx", {"libkernel", 0, 1, 1}, {{"libkernel", 0, 1}}},
        Cpu::SceHostModule{"libSceLibcInternal.sprx", {"libSceLibcInternal", 0, 1, 1}, {{"libSceLibcInternal", 0, 1}}}};
    if (policy == 5) hosts.push_back({"OtherInternal.prx", {"libSceLibcInternal", 0, 1, 1}, {{"libSceLibcInternal", 0, 1}}});
    auto resolver = [&](const Cpu::SceImport& import, std::uint8_t type) -> std::optional<Cpu::SceResolvedImport> {
        if (import.ModuleName != "libkernel") return std::nullopt;
        auto address = kernel.Resolve(import, type);
        if (!address) address = lifecycle.Resolve(import);
        return address ? std::optional(Cpu::SceResolvedImport{*address, type}) : std::nullopt;
    };
    const auto identity = receipt.Cases.at(name);
    std::optional<Cpu::SceLibcInternalProvider> provider = Cpu::SceLibcInternalProvider{"SceInternalLibc.prx", identity.Digest, identity.Size};
    if (policy == 1) provider.reset();
    if (policy == 2) provider->SourceSha256[0] ^= std::byte{1};
    if (policy == 3) ++provider->SourceSize;
    if (policy == 4) provider->Filename = "different.prx";
    Cpu::SceModules modules(machine, {root / "SceInternalMain.elf", MainBias}, dependencies, hosts, resolver, provider);
    require(words<11>(machine, GuestBias + receipt.Addresses[0]) == std::array<std::uint64_t, 11>{},
            "Loading executed libc-owned guest state");
    const auto actual = words<22>(machine, MainBias + receipt.Addresses[2]);
    for (unsigned i = 0; i < actual.size(); ++i)
        require(actual[i] == GuestBias + receipt.Addresses[i < 9 ? 4 + i : 14 + i - 9], "Internal relocation did not preserve supplied guest function destination");
    Cpu::SceImport exit; exit.Nid = "6Z83sYWFlA8"; exit.ModuleName = exit.LibraryName = "libkernel";
    exit.LibraryVersion = exit.ModuleMajor = exit.ModuleMinor = 1;
    Cpu::SetupSceEntry(machine, modules.Main(), {"internal-forwarding"}, lifecycle.Resolve(exit).value());
    threads->AdoptInitial({modules.Main().Entry, modules.InitialStack(), modules.Tls(), modules.ThreadTlsFactory()});
    const auto errno_address = threads->ActiveErrnoAddress();
    std::array<unsigned char, 12> errno_guard; errno_guard.fill(0xa7);
    machine.Write(errno_address, std::as_bytes(std::span(errno_guard)));
    modules.InitializeDependencies();
    require(words<11>(machine, GuestBias + receipt.Addresses[0]) == std::array<std::uint64_t, 11>{1, 0, 1},
            "Supplied guest provider initialization did not execute once before entry");
    require(words<1>(machine, ConsumerBias + receipt.Addresses[13])[0] == 0x006b6f726564726fULL,
            "Forwarded consumer initializer did not run after provider initialization");
    if (probe) {
        constexpr std::uint64_t fs = 0x400000000;
        machine.Map(fs, 4096, Cpu::Permission::Read | Cpu::Permission::Write);
        std::array<unsigned char, 16> guard; guard.fill(0xa7);
        machine.Write(fs + 0x20, std::as_bytes(std::span(guard)));
        machine.Set(Cpu::Register::FsBase, fs);
        rejects([&] { machine.Run(MainBias + receipt.Addresses[31 + probe], 0, 1000000); }, "Unsupported guest interrupt 69");
        std::array<unsigned char, 16> observed;
        machine.Read(fs + 0x20, std::as_writable_bytes(std::span(observed)));
        guard[8] = probe == 2 ? 0x0a : 0x0b; guard[9] = 0; guard[10] = 2; guard[11] = 0xa0;
        require(observed == guard, "Guest termination changed payload width or adjacent TLS bytes");
        require(words<1>(machine, MainBias + receipt.Addresses[27])[0] == (probe == 3 ? 0 : 1),
                "Guest termination returned to its caller or skipped destination");
        require(words<11>(machine, GuestBias + receipt.Addresses[0])[1] == 0, "Exception route substituted successful CRT cleanup");
        threads->Withdraw();
        std::cout << "PASS guest FS error-slot/int45 nonreturn boundary " << probe << '\n'; return;
    }
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
    put(128, std::string_view("pabcd\0", 6)); put(144, std::string_view("qwx\0", 4));
    require(output == expected, "Independent byte oracle disagreed with guest copy/set/padding/truncation/output or adjacent-byte preservation");
    std::array<char, 9> printed;
    machine.Read(GuestBias + receipt.Addresses[1], std::as_writable_bytes(std::span(printed)));
    require(printed == std::array<char, 9>{'p', 'r', 'i', 'n', 't', ':', '9', '1', 0}, "printf changed guest-owned destination or integer ABI");
    const auto extra = words<26>(machine, MainBias + receipt.Addresses[27]);
    const std::array<std::uint64_t, 26> expected_extra{
        MainBias + receipt.Addresses[28], MainBias + receipt.Addresses[28] + 4096,
        MainBias + receipt.Addresses[28] + 256, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, UINT64_MAX,
        0x100000001, 10, UINT64_MAX - 1, 2, UINT64_MAX, 21, 0, 0, 0x100000001, 0x1122334455667788, 0x8877665544332211, 0xa7a7a7a7};
    require(extra == expected_extra, "Independent heap/status/numeric/64-bit-pointer oracle disagreed with guest forwarding");
    std::array<unsigned char, 168> stats;
    machine.Read(MainBias + receipt.Addresses[29], std::as_writable_bytes(std::span(stats)));
    std::array<unsigned char, 168> expected_stats; expected_stats.fill(0xa7);
    const auto stat_word = [&](unsigned offset, std::uint64_t value, unsigned size) {
        for (unsigned i = 0; i < size; ++i) expected_stats[offset + i] = static_cast<unsigned char>(value >> (8 * i));
    };
    for (unsigned i = 0; i < 3; ++i) {
        stat_word(i * 56 + 8, i == 2 ? 39 : 40, 2); stat_word(i * 56 + 10, 1, 2);
        if (i < 2) { stat_word(i * 56 + 16, 4096, 8); stat_word(i * 56 + 24, 4096, 8);
            stat_word(i * 56 + 32, 32, 8); stat_word(i * 56 + 40, i ? 0 : 32, 8); }
    }
    require(stats == expected_stats, "Stats corrupted size/version/reserved/adjacent bytes or positive-error output");
    std::array<unsigned char, 8> retained;
    machine.Read(MainBias + receipt.Addresses[28] + 256, std::as_writable_bytes(std::span(retained)));
    require(retained == std::array<unsigned char, 8>{0xb0, 0xb1, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7},
            "Moving realloc lost independently expected guest data");
    require(words<12>(machine, MainBias + receipt.Addresses[30]) == std::array<std::uint64_t, 12>{33, 11, 44, 22},
            "Guest callback DSO filter/reverse-order/reentrancy oracle failed");
    machine.Read(errno_address, std::as_writable_bytes(std::span(errno_guard)));
    std::array<unsigned char, 12> expected_errno; expected_errno.fill(0xa7);
    expected_errno[0] = 34; expected_errno[1] = expected_errno[2] = expected_errno[3] = 0;
    require(errno_guard == expected_errno, "_Stoul changed kernel-owned guest errno width/neighbors or invalid-base errno");
    modules.FinalizeDependencies();
    require(words<11>(machine, GuestBias + receipt.Addresses[0]) == std::array<std::uint64_t, 11>{1, 1, 2, 1, 1, 2, 3, 2, 3, 1, 4},
            "Guest calls/finalization counters disagreed with independent provider lifetime oracle");
    require(words<12>(machine, MainBias + receipt.Addresses[30]) == std::array<std::uint64_t, 12>{33, 11, 44, 22, 77, 66, 55},
            "Guest CRT did not retain callback objects through finalization in reverse registration order");
    require(words<1>(machine, ConsumerBias + receipt.Addresses[13])[0] == 0x5a6b6f726564726fULL,
            "Consumer guest DSO callback did not run exactly during consumer finalization");
    require(words<8>(machine, GuestBias + receipt.Addresses[31]) == std::array<std::uint64_t, 8>{2, 2, 2, 0, 1},
            "Guest heap/create/destroy/delete ownership disagreed with independent lifetime oracle");
    threads->Withdraw();
    rejects([&] { modules.FinalizeDependencies(); }, "successful initialization");
    std::cout << "PASS 22 actual x86 Internal relocations to supplied guest destinations, independent byte/varargs and lifecycle oracles\n";
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "Usage: SceLibcInternalTests receipt.txt"); Receipt receipt(argv[1]);
#ifdef CPU06_BASELINE
        rejects([&] { graph(receipt, "valid"); }, "unresolved or wrongly typed host import");
        std::cout << "PASS pre-fix compiled guest fails only at unresolved Internal import\n";
#else
        graph(receipt, "valid");
        for (unsigned probe : {1, 2, 3}) graph(receipt, "valid", 0, probe);
        for (unsigned policy : {1, 2, 3, 4, 5}) rejects([&] { graph(receipt, "valid", policy); },
            policy == 1 ? "unresolved or wrongly typed host import" : policy < 4 ? "source identity mismatch" :
            policy == 4 ? "missing supplied libc Internal source provider" : "ambiguous named module/version provider");
        for (const auto& [name, reason] : std::array{
            std::pair{"consumer-library", "unresolved typed import scope/version provider"},
            std::pair{"consumer-module", "missing named module/version provider"},
            std::pair{"consumer-library-version", "unresolved typed import scope/version provider"},
            std::pair{"consumer-module-major", "missing named module/version provider"},
            std::pair{"consumer-module-minor", "missing named module/version provider"},
            std::pair{"consumer-size", "libc Internal import size exceeds target storage"},
            std::pair{"provider-reserved", "invalid libc Internal target function"},
            std::pair{"provider-visibility", "invalid libc Internal target function"},
            std::pair{"provider-binding", "missing qualified libc Internal target export"},
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
