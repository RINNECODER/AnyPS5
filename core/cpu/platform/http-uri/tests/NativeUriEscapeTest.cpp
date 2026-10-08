#include "../NativeUriEscape.hpp"
#include "../../../src/SceImageData.hpp"
#include <cpu/GuestThreads.hpp>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

// Authoring gate: external http-uri-job/fixture-audit.json. Real ELF linker
// metadata, guest execution and mapping/output oracles; no new production seam.
namespace {
using Cpu::Permission;
using Cpu::Register;
using Provider = Cpu::Platform::NativeUriEscape;
constexpr auto rw = Permission::Read | Permission::Write;
constexpr auto rx = Permission::Read | Permission::Execute;
constexpr std::uint64_t Args = 0x300000, OutputPage = 0x302000, Output = OutputPage + 16;
constexpr std::uint64_t RequiredPage = 0x303000, Required = RequiredPage + 8;
constexpr std::uint64_t Input = 0x304000, Receipt = 0x305000, Stop = 0x100000;
using ReceiptWords = std::array<std::uint64_t, 6>;
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F&& f) {
    try { f(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Invalid guest pointer was accepted by the genuine imported call");
}
template<class F> void rejectsAccess(F&& f) {
    try { f(); } catch (const std::exception& error) {
        const std::string_view reason = error.what();
        require(reason.find("Guest access denied") != std::string_view::npos ||
                reason.find("invalid guest span") != std::string_view::npos ||
                reason.find("Guest unmapped instruction fetch") != std::string_view::npos,
                "Negative guest access failed for an unrelated oracle/loader reason");
        return;
    }
    throw std::runtime_error("Invalid guest span was accepted by the genuine imported call");
}
bool sameMappings(const std::vector<Cpu::Mapping>& a, const std::vector<Cpu::Mapping>& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t n = 0; n < a.size(); ++n)
        if (a[n].Address != b[n].Address || a[n].Size != b[n].Size ||
            a[n].Permissions != b[n].Permissions || a[n].Borrowed != b[n].Borrowed) return false;
    return true;
}
bool sameImport(const Cpu::SceImport& a, const Cpu::SceImport& b) {
    return a.Nid == b.Nid && a.LibraryName == b.LibraryName && a.ModuleName == b.ModuleName &&
        a.LibraryId == b.LibraryId && a.ModuleId == b.ModuleId && a.LibraryVersion == b.LibraryVersion &&
        a.ModuleMajor == b.ModuleMajor && a.ModuleMinor == b.ModuleMinor;
}
Provider::Configuration enabled(const Cpu::SceParsedImage& image) {
    Provider::Configuration value;
    value.EnablePublicFixture = true;
    value.PublicFixtureSha256 = image.SourceSha256;
    value.PublicFixtureSize = image.SourceSize;
    return value;
}
struct Session {
    Cpu::Machine machine;
    std::shared_ptr<Cpu::GuestThreads> threads = std::make_shared<Cpu::GuestThreads>(machine);
    Cpu::SceParsedImage consumer;
    std::unique_ptr<Provider> provider;
    std::uint64_t gate = 0, entry = 0;
    const Cpu::SceImport& row() const {
        require(consumer.Imports.size() == 1, "Fixture is not a one-import linked SCE executable");
        return consumer.Imports.front();
    }
    const Cpu::SceImageData::Symbol& symbol() const {
        const auto found = std::find_if(consumer.Data->Symbols.begin(), consumer.Data->Symbols.end(),
            [](const auto& value) { return value.Import && *value.Import == 0; });
        require(found != consumer.Data->Symbols.end(), "Linked import lost its retained typed symbol row");
        return *found;
    }
    explicit Session(const char* path, bool activate = true) : consumer(Cpu::ParseSce(path)) {
        require(std::string_view(Cpu::Machine::Backend()).find("Modern QEMU TCG") != std::string_view::npos,
                "Native URI acceptance requires modern native TCG");
        require(consumer.Path.filename() == "NativeUriGuest.elf" && consumer.SourceContainer == "elf" &&
                consumer.Type == 0xfe10 && consumer.Data && consumer.UnsupportedReasons.empty() &&
                consumer.RelocationCount >= 2 &&
                std::set<std::uint32_t>(consumer.RelocationTypes.begin(), consumer.RelocationTypes.end()) ==
                    std::set<std::uint32_t>{7, 8},
                "Actual linked URI ELF lost its immutable source or PLT/RELATIVE relocations");
        require(row().Nid == "YuOW3dDAKYc" && row().LibraryName == "libSceHttp" && row().ModuleName == "libSceHttp" &&
                row().LibraryId == 1 && row().ModuleId == 1 && row().LibraryVersion == 1 &&
                row().ModuleMajor == 1 && row().ModuleMinor == 1 && symbol().Type == 2 &&
                symbol().Size == 0 && symbol().Section == 0,
                "Genuine function import identity/type/size changed");
        machine.Map(Stop, 0x1000, rx);
        const std::array nop{std::byte{0x90}}; machine.Write(Stop, nop);
        machine.Map(0x200000, 0x10000, rw);
        machine.Map(Args, 0x10000, rw);
        if (activate) {
            provider = std::make_unique<Provider>(machine, threads, enabled(consumer));
            unsigned resolved = 0;
            const auto loaded = Cpu::LoadSce(machine, path, 0x1000000, [&](const Cpu::SceImport& loadedRow) {
                require(sameImport(loadedRow, row()), "LoadSce requested a row different from the independently parsed import");
                gate = provider->Resolve(loadedRow, symbol().Type, symbol().Size, consumer).value();
                ++resolved; return gate;
            });
            require(resolved == 1, "LoadSce failed to bind exactly the genuine URI PLT import");
            entry = loaded.Entry;
        }
        machine.Set(Register::Rsp, 0x20ffe8);
        threads->AdoptInitial({entry ? entry : Stop, {0x200000, 0x10000, rw, false}, {},
            [](std::uint64_t) { return std::shared_ptr<Cpu::SceTls>{}; }});
        resetOutputs();
        const std::array<unsigned char, 11> input{'A','z','0','-','_','.','~',' ','/',255,0};
        machine.Write(Input, std::as_bytes(std::span(input)));
    }
    ~Session() { provider.reset(); threads->Withdraw(); }
    void fill(std::uint64_t p, std::size_t size) {
        const std::vector<std::byte> bytes(size, std::byte{0xa7}); machine.Write(p, bytes);
    }
    void resetOutputs() { fill(OutputPage, 64); fill(RequiredPage, 32); }
    std::vector<std::byte> read(std::uint64_t p, std::size_t size) {
        std::vector<std::byte> bytes(size); machine.Read(p, bytes); return bytes;
    }
    ReceiptWords receipt() {
        ReceiptWords values{}; machine.Read(Receipt, std::as_writable_bytes(std::span(values))); return values;
    }
    std::uint64_t required() {
        std::uint64_t value = 0; machine.Read(Required, std::as_writable_bytes(std::span(&value, 1))); return value;
    }
    void invoke(std::uint64_t mode, std::uint64_t out = Output, std::uint64_t needed = Required,
                std::uint64_t capacity = 17, std::uint64_t input = Input) {
        const std::array<std::uint64_t, 6> arguments{out, needed, capacity, input, Receipt, mode};
        machine.Write(Args, std::as_bytes(std::span(arguments)));
        machine.Write(0x20ffe8, std::as_bytes(std::span(&Stop, 1)));
        machine.Set(Register::Rdi, Args); machine.Set(Register::Rsp, 0x20ffe8);
        require(machine.Run(entry, Stop, 100000) == Cpu::StopReason::Address,
                "Actual linked URI guest did not return through its genuine imported call");
        require(machine.Get(Register::Rsp) == 0x20fff0, "Actual imported guest call damaged the stack");
        require(machine.Get(Register::Rax) == 0, "Actual linked guest's independent width/output oracle failed");
    }
};
void scope(const char* path) {
    Session s(path, false);
    const auto before = s.machine.Mappings();
    Provider denied(s.machine, s.threads);
    require(!denied.Resolve(s.row(), 2, 0, s.consumer), "Default policy admitted the public linked guest");
    require(sameMappings(before, s.machine.Mappings()), "Default denial allocated a URI gate");
    auto targetOnly = enabled(s.consumer); targetOnly.EnablePublicFixture = false; targetOnly.EnableTargetConsumer = true;
    Provider target(s.machine, s.threads, targetOnly);
    require(!target.Resolve(s.row(), 2, 0, s.consumer), "Target enable alone admitted an unrelated ELF fixture");
    require(sameMappings(before, s.machine.Mappings()), "Target-only rejection allocated a URI gate");
    Provider owner(s.machine, s.threads, enabled(s.consumer));
    require(sameMappings(before, s.machine.Mappings()), "Constructing a qualified owner eagerly allocated a URI gate");
    auto deny = [&](const Cpu::SceImport& row, std::uint8_t type, std::uint64_t size,
                    const Cpu::SceParsedImage& image) {
        bool rejected = false;
        try { rejected = !owner.Resolve(row, type, size, image); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Unqualified consumer/source/import identity passed typed URI admission");
        require(sameMappings(before, s.machine.Mappings()), "Rejected typed URI admission allocated a mapping");
    };
    for (const auto type : {0, 1, 3, 6}) deny(s.row(), type, 0, s.consumer);
    for (const auto size : {1ULL, 8ULL, ~0ULL}) deny(s.row(), 2, size, s.consumer);
    for (unsigned field = 0; field < 8; ++field) {
        auto row = s.row();
        if (field == 0) row.Nid = "foreign";
        if (field == 1) row.LibraryName = "foreign";
        if (field == 2) row.ModuleName = "foreign";
        if (field == 3) ++row.LibraryVersion;
        if (field == 4) ++row.ModuleMajor;
        if (field == 5) ++row.ModuleMinor;
        if (field == 6) ++row.LibraryId;
        if (field == 7) ++row.ModuleId;
        deny(row, 2, 0, s.consumer);
    }
    for (unsigned field = 0; field < 9; ++field) {
        auto image = s.consumer;
        if (field == 0) image.SourceSha256[0] ^= std::byte{1};
        if (field == 1) ++image.SourceSize;
        if (field == 2) image.Data.reset();
        if (field == 3) image.SourceContainer = "plain_self";
        if (field == 4) image.Path = "libSceNpCppWebApi.prx";
        if (field == 5) image.Type = 0xfe18;
        if (field == 6) image.Imports.clear();
        if (field == 7) image.OsAbi = 0;
        if (field == 8) image.AbiVersion = 0;
        deny(s.row(), 2, 0, image);
    }
    for (unsigned field = 0; field < 8; ++field) {
        auto image = s.consumer;
        auto& retained = image.Imports.front();
        if (field == 0) retained.Nid = "foreign";
        if (field == 1) retained.LibraryName = "foreign";
        if (field == 2) retained.ModuleName = "foreign";
        if (field == 3) ++retained.LibraryVersion;
        if (field == 4) ++retained.ModuleMajor;
        if (field == 5) ++retained.ModuleMinor;
        if (field == 6) ++retained.LibraryId;
        if (field == 7) ++retained.ModuleId;
        deny(s.row(), 2, 0, image);
    }
    for (unsigned field = 0; field < 9; ++field) {
        auto image = s.consumer;
        auto data = std::make_shared<Cpu::SceImageData>(*image.Data);
        image.Data = data;
        auto& retained = *std::find_if(data->Symbols.begin(), data->Symbols.end(),
            [](const auto& value) { return value.Import && *value.Import == 0; });
        if (field == 0) data->SourceSha256[0] ^= std::byte{1};
        if (field == 1) ++data->SourceSize;
        if (field == 2) retained.Type = 1;
        if (field == 3) retained.Size = 8;
        if (field == 4) retained.Import.reset();
        if (field == 5) retained.Section = 1;
        if (field == 6) retained.Value = 1;
        if (field == 7) retained.Binding = 2;
        if (field == 8) retained.Visibility = 3;
        deny(s.row(), 2, 0, image);
    }
    for (unsigned field = 0; field < 4; ++field) {
        auto image = s.consumer;
        auto data = std::make_shared<Cpu::SceImageData>(*image.Data); image.Data = data;
        require(!data->ImportedModules.empty(), "Linked fixture lost its module identity table");
        if (field == 0) data->ImportedModules.front().Name = "foreign";
        if (field == 1) ++data->ImportedModules.front().Id;
        if (field == 2) ++data->ImportedModules.front().Major;
        if (field == 3) ++data->ImportedModules.front().Minor;
        deny(s.row(), 2, 0, image);
    }
    auto changedConfiguration = enabled(s.consumer); changedConfiguration.PublicFixtureSha256[0] ^= std::byte{1};
    Provider wrongHash(s.machine, s.threads, changedConfiguration);
    require(!wrongHash.Resolve(s.row(), 2, 0, s.consumer), "Fixture enable admitted another pinned hash");
    changedConfiguration = enabled(s.consumer); ++changedConfiguration.PublicFixtureSize;
    Provider wrongSize(s.machine, s.threads, changedConfiguration);
    require(!wrongSize.Resolve(s.row(), 2, 0, s.consumer), "Fixture enable admitted another pinned size");
    require(sameMappings(before, s.machine.Mappings()), "Wrong source pin allocated a URI gate");
    for (const auto variant : {"uri-wrong-nid", "uri-wrong-abi", "uri-wrong-library"}) {
        auto image = Cpu::ParseSce(s.consumer.Path.parent_path() / variant / "NativeUriGuest.elf");
        require(image.Data && image.Imports.size() == 1 && image.Type == 0xfe10 &&
                image.SourceSha256 != s.consumer.SourceSha256,
                "Alternative input is not a fresh genuinely parsed distinct linked ELF");
        if (std::string_view(variant) == "uri-wrong-nid") {
            require(image.Imports.front().Nid == "AAAAAAAAAAA" && image.OsAbi == 9 && image.AbiVersion == 2,
                    "Wrong-NID parser fixture lost its genuine retained row");
            image.Imports.front() = s.row();
        } else if (std::string_view(variant) == "uri-wrong-abi") {
            require(image.OsAbi == 0 && image.AbiVersion == 0,
                    "Wrong-ABI parser fixture does not retain independent ABI 0/0");
            image.OsAbi = 9; image.AbiVersion = 2;
        } else {
            const auto& actual = image.Imports.front();
            require(actual.Nid == s.row().Nid && actual.LibraryName == "libSceHttp" && actual.LibraryId == 1 &&
                    actual.LibraryVersion == 2 && actual.ModuleName == "libSceHttp" && actual.ModuleId == 1 &&
                    actual.ModuleMajor == 1 && actual.ModuleMinor == 1 && image.OsAbi == 9 && image.AbiVersion == 2,
                    "Wrong-library parser fixture lost its independent version2/URI-NID/module1.1 contract");
            image.Imports.front() = s.row();
        }
        Provider variantOwner(s.machine, s.threads, enabled(image));
        require(!variantOwner.Resolve(s.row(), 2, 0, image),
                "Mutable top row/header spoof over a separately pinned genuine ELF passed immutable source admission");
        require(sameMappings(before, s.machine.Mappings()), "Spoofed independently pinned ELF allocated a URI gate");
    }
    const auto good = owner.Resolve(s.row(), 2, 0, s.consumer);
    require(good && owner.Resolve(s.row(), 2, 0, s.consumer) == good,
            "Good typed import was rejected or repeated resolution changed its callback");
    require(!sameMappings(before, s.machine.Mappings()), "Accepted URI import has no real executable guest allocation");
    s.machine.CheckAccess(*good, 1, Permission::Execute);
    {
        auto configuration = enabled(s.consumer);
        configuration.GateBase += 4096;
        s.machine.Map(configuration.GateBase, 4096, rw);
        const std::array marker{std::byte{0x31}, std::byte{0x72}, std::byte{0xa3}, std::byte{0xb4}};
        s.machine.Write(configuration.GateBase, marker);
        const auto protectedMappings = s.machine.Mappings();
        auto colliding = std::make_unique<Provider>(s.machine, s.threads, configuration);
        rejects([&] { colliding->Resolve(s.row(), 2, 0, s.consumer); });
        colliding.reset();
        require(sameMappings(protectedMappings, s.machine.Mappings()) &&
                s.read(configuration.GateBase, marker.size()) == std::vector<std::byte>(marker.begin(), marker.end()),
                "Lazy admission replaced or teardown removed an unrelated mapped gate page");
    }
    std::cout << "scope receipt: default/target-only denial; immutable hash/size, retained row/type/size, source shape; lazy allocation PASS\n";
}
void compiled(const char* path) {
    Session s(path);
    require(s.required() == 0xa7a7a7a7a7a7a7a7ULL, "Required64 high-half canary was not initialized");
    s.invoke(0);
    require(s.receipt() == ReceiptWords{0x555249434f4d504cULL, 0, 17, 0x80431022, 0, 17},
            "Real PLT/RELATIVE guest query/too-small/exact-fit receipt differs");
    auto output = std::vector<std::byte>(64, std::byte{0xa7});
    constexpr char literal[] = "Az0-_.~%20%2F%FF";
    std::memcpy(output.data() + 16, literal, sizeof(literal));
    require(s.read(OutputPage, 64) == output, "Genuine imported call changed exact URI bytes, NUL or surrounding canaries");
    auto required = std::vector<std::byte>(32, std::byte{0xa7});
    constexpr std::array<std::byte, 8> literal17{std::byte{17}, std::byte{0}, std::byte{0}, std::byte{0},
        std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}};
    std::copy(literal17.begin(), literal17.end(), required.begin() + 8);
    require(s.read(RequiredPage, 32) == required, "Required64 cell width or either guard was damaged");
    std::cout << "compiled receipt: genuine one-PLT/RELATIVE source; required64=17 from nonzero high bits; exact fit/canaries PASS\n";
}
void memory(const char* path) {
    Session s(path);
    auto failed = [&](std::uint64_t output, std::uint64_t required) {
        s.resetOutputs();
        // Cover the actually mapped prefixes of the two invalid end-spans,
        // together with preceding guards; normal paired cells alone miss them.
        constexpr std::uint64_t tail = 0x30ffe0;
        s.fill(tail, 32);
        const auto beforeOutput = s.read(OutputPage, 64), beforeRequired = s.read(RequiredPage, 32);
        const auto beforeTail = s.read(tail, 32);
        rejectsAccess([&] { s.invoke(1, output, required); });
        require(s.read(OutputPage, 64) == beforeOutput && s.read(RequiredPage, 32) == beforeRequired,
                "Rejected actual imported call partially wrote output/required pair or either canary");
        require(s.read(tail, 32) == beforeTail,
                "Rejected end-span call modified a mapped required/output prefix or preceding tail guard");
    };
    // All failures run the same genuine ELF's imported call, not a synthetic gate stub.
    s.machine.Protect(RequiredPage, 0x1000, Permission::Read);
    failed(Output, Required);
    s.machine.Protect(RequiredPage, 0x1000, rw);
    failed(Output, 0xdead000);
    failed(Output, ~0ULL - 3);
    failed(Output, 0x30fffc); // Required64 straddles the end of the mapped data span.
    s.machine.Protect(OutputPage, 0x1000, Permission::Read);
    failed(Output, Required);
    s.machine.Protect(OutputPage, 0x1000, rw);
    failed(0xdead000, Required);
    failed(~0ULL - 3, Required);
    failed(0x30fff8, Required); // Entire required cell is valid, but 17-byte output is not.
    s.resetOutputs(); s.invoke(1, Output, Required, 16);
    require(s.receipt()[0] == 0x55524953494e474cULL && s.receipt()[1] == 0x80431022U && s.required() == 17 &&
            s.read(OutputPage, 64) == std::vector<std::byte>(64, std::byte{0xa7}),
            "Too-small genuine imported call touched output or failed to publish required64");
    std::cout << "memory receipt: imported read-only/unmapped/wrapping/end-span required64 and output rejection; atomic pair/canaries PASS\n";
}
void lifetime(const char* path) {
    Session s(path);
    s.invoke(0);
    const auto previousGate = s.gate;
    s.provider.reset();
    rejectsAccess([&] { s.machine.CheckAccess(previousGate, 1, Permission::Execute); });
    rejectsAccess([&] { s.invoke(1); });
    s.provider = std::make_unique<Provider>(s.machine, s.threads, enabled(s.consumer));
    s.gate = s.provider->Resolve(s.row(), s.symbol().Type, s.symbol().Size, s.consumer).value();
    require(s.gate == previousGate, "Replacement did not reuse the original PLT slot's configured address");
    s.resetOutputs(); s.invoke(0);
    require(s.receipt() == ReceiptWords{0x555249434f4d504cULL, 0, 17, 0x80431022, 0, 17},
            "Replacement callback left the genuine ELF import bound to expired provider state");
    std::cout << "lifetime receipt: owned executable gate released; stale PLT call denied; replacement real guest callback PASS\n";
}
}
int main(int argc, char** argv) {
    try {
        require(argc == 3, "Usage: NativeUriEscapeTest NativeUriGuest.elf scope|compiled|memory|lifetime");
        const std::string_view mode = argv[2];
        if (mode == "scope") scope(argv[1]);
        else if (mode == "compiled") compiled(argv[1]);
        else if (mode == "memory") memory(argv[1]);
        else if (mode == "lifetime") lifetime(argv[1]);
        else throw std::runtime_error("Unknown native URI acceptance control");
        std::cout << "NativeUriEscapeTest " << mode << ": PASS (public ELF execution; no retail game admission claim)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "NativeUriEscapeTest: " << error.what() << '\n'; return 1;
    }
}
