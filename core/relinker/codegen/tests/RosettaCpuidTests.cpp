// relinker --to-rosetta CPUID stub: lowers a CPUID site, links it the way the
// relinker does (jump at the site, stub returns after the absorbed
// instructions) and runs it under Rosetta. Every feature bit the stub adds must
// execute there, and every other leaf must stay what Rosetta reports.

#include <codegen/IAmd64OnlyConverter.hpp>
#include <codegen/x86/IAmd64OnlyInstructionMatcher.hpp>
#include <codegen/x86/RosettaCpuid.hpp>
#include <codegen/x86/StubBodyBuilder.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(__APPLE__) && defined(__x86_64__)
#include <csetjmp>
#include <csignal>
#include <sys/mman.h>
#include <sys/sysctl.h>
#define ROSETTA_CPUID_EXECUTE 1
#endif

namespace {

using Bytes = std::vector<std::uint8_t>;

int g_failures = 0;

void require(const bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++g_failures;
    }
}

// cpuid; mov [rdi], eax; mov [rdi+4], ebx; mov [rdi+8], ecx; mov [rdi+12], edx; ret
const Bytes kFunction = {0x0F, 0xA2, 0x89, 0x07, 0x89, 0x5F, 0x04, 0x89, 0x4F, 0x08, 0x89, 0x57, 0x0C, 0xC3};
constexpr std::uint64_t kFileOffset = 0x200;
constexpr std::uint64_t kAddress = 0x1000;

Domain::ProgramHeader Segment(const std::size_t size) {
    return {1, 5, kFileOffset, kAddress, 0, size, size, 16};
}

Codegen::ConvertResult Convert(const Bytes& text, const Codegen::Amd64OnlyTarget target) {
    Bytes file(kFileOffset + text.size(), 0xCC);
    std::copy(text.begin(), text.end(), file.begin() + kFileOffset);
    return Codegen::MakeAmd64OnlyConverter(target)->Convert(file, {Segment(text.size())});
}

void Matching() {
    const auto rosetta = Codegen::MakeAmd64OnlyInstructionMatcher(Codegen::Amd64OnlyTarget::Rosetta);
    const auto intel = Codegen::MakeAmd64OnlyInstructionMatcher(Codegen::Amd64OnlyTarget::Intel);
    const Bytes cpuid = {0x0F, 0xA2};
    const auto match = rosetta->Match(cpuid.data(), cpuid.size());
    require(match && match->Lowering == Codegen::Amd64OnlyLowering::Trampoline && match->Optional && match->InstructionName == "CPUID",
            "CPUID was not lowered through an optional stub for Rosetta");
    require(!intel->Match(cpuid.data(), cpuid.size()).has_value(), "CPUID was lowered for an Intel host");
    const Bytes prefixed = {0x66, 0x0F, 0xA2};
    require(!rosetta->Match(prefixed.data(), prefixed.size()).has_value(), "A prefixed 0F A2 was taken for CPUID");

    const auto result = Convert(kFunction, Codegen::Amd64OnlyTarget::Rosetta);
    require(result.Trampolines.size() == 1 && result.KeptCount == 0, "The CPUID site was not lowered");
    if (result.Trampolines.size() == 1)
        require(result.Trampolines[0].Length == 7, "CPUID did not absorb the two stores after it (7 bytes)");

    // A branch right after CPUID leaves no room for the jump: the site is kept as it is.
    Bytes branch = kFunction;
    branch[2] = 0xC3;
    const auto kept = Convert(branch, Codegen::Amd64OnlyTarget::Rosetta);
    require(kept.Trampolines.empty() && kept.KeptCount == 1, "CPUID followed by a return was not kept");
}

#ifdef ROSETTA_CPUID_EXECUTE

bool Translated() {
    int translated = 0;
    std::size_t size = sizeof translated;
    return sysctlbyname("sysctl.proc_translated", &translated, &size, nullptr, 0) == 0 && translated == 1;
}

struct Registers {
    std::uint32_t Eax, Ebx, Ecx, Edx;
    bool Carry;
};

Registers NativeCpuid(std::uint32_t leaf, std::uint32_t subleaf) {
    Registers out{};
    __asm__ volatile("cpuid" : "=a"(out.Eax), "=b"(out.Ebx), "=c"(out.Ecx), "=d"(out.Edx) : "a"(leaf), "c"(subleaf));
    return out;
}

// The site and its stub linked on one executable page, as the relinker links them.
class LinkedSite {
public:
    LinkedSite() {
        const auto result = Convert(kFunction, Codegen::Amd64OnlyTarget::Rosetta);
        if (result.Trampolines.size() != 1) return;
        const auto& site = result.Trampolines[0];
        _page = static_cast<std::uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
        if (_page == MAP_FAILED) {
            _page = nullptr;
            return;
        }
        std::memset(_page, 0xCC, 4096);
        std::copy(kFunction.begin(), kFunction.end(), _page);
        constexpr std::size_t stubOffset = 0x100;
        auto body = site.Body;
        const auto siteAddress = reinterpret_cast<std::uint64_t>(_page);
        const auto stubAddress = siteAddress + stubOffset;
        Codegen::ApplyStubRelocations(body, site.Relocations, siteAddress, stubAddress, 0);
        const auto back = static_cast<std::int32_t>(static_cast<std::int64_t>(siteAddress + site.Length) - static_cast<std::int64_t>(stubAddress + site.ReturnBranchOffset + 5));
        std::memcpy(body.data() + site.ReturnBranchOffset + 1, &back, 4);
        std::copy(body.begin(), body.end(), _page + stubOffset);
        _page[0] = 0xE9;
        const std::int32_t to = static_cast<std::int32_t>(stubOffset - 5);
        std::memcpy(_page + 1, &to, 4);
        if (mprotect(_page, 4096, PROT_READ | PROT_EXEC) != 0) {
            munmap(_page, 4096);
            _page = nullptr;
        }
    }
    ~LinkedSite() {
        if (_page) munmap(_page, 4096);
    }
    [[nodiscard]] bool Ready() const { return _page != nullptr; }

    // Calls the site with CF set, below the caller's red zone.
    Registers Call(std::uint32_t leaf, std::uint32_t subleaf) const {
        std::uint32_t out[4] = {};
        std::uint8_t carry = 0;
        __asm__ volatile(
            "sub $128, %%rsp\n\t"
            "stc\n\t"
            "call *%[fn]\n\t"
            "setc %[carry]\n\t"
            "add $128, %%rsp"
            : "+a"(leaf), "+c"(subleaf), [carry] "=m"(carry)
            : [fn] "r"(_page), "D"(out)
            : "rbx", "rdx", "memory", "cc");
        return {out[0], out[1], out[2], out[3], carry != 0};
    }

private:
    std::uint8_t* _page = nullptr;
};

sigjmp_buf g_fault;
void OnFault(int signal) { siglongjmp(g_fault, signal); }

// Runs one instruction of an advertised feature; false when Rosetta faults on it.
template<typename TBody>
bool Executes(const char* name, const TBody& body) {
    struct sigaction action {}, oldIll {}, oldSegv {};
    action.sa_handler = OnFault;
    sigaction(SIGILL, &action, &oldIll);
    sigaction(SIGSEGV, &action, &oldSegv);
    const int signal = sigsetjmp(g_fault, 1);
    if (signal == 0) body();
    sigaction(SIGILL, &oldIll, nullptr);
    sigaction(SIGSEGV, &oldSegv, nullptr);
    if (signal != 0) std::fprintf(stderr, "advertised %s faults under Rosetta (signal %d)\n", name, signal);
    return signal == 0;
}

void Execution() {
    if (!Translated()) {
        std::printf("rosetta_cpuid: not running under Rosetta, execution checks skipped\n");
        return;
    }
    const LinkedSite site;
    require(site.Ready(), "Could not link the CPUID stub on an executable page");
    if (!site.Ready()) return;

    struct Leaf {
        std::uint32_t Leaf, Subleaf;
    };
    for (const Leaf leaf : {Leaf{0, 0}, Leaf{1, 0}, Leaf{7, 0}, Leaf{7, 1}, Leaf{0xD, 0}, Leaf{0xD, 1}, Leaf{0xD, 2}, Leaf{0x80000000u, 0}, Leaf{0x80000001u, 0}, Leaf{0x80000008u, 0}}) {
        auto expected = NativeCpuid(leaf.Leaf, leaf.Subleaf);
        if (leaf.Leaf == 1) expected.Ecx |= Codegen::RosettaCpuid::kLeaf1Ecx;
        if (leaf.Leaf == 7 && leaf.Subleaf == 0) expected.Ebx |= Codegen::RosettaCpuid::kLeaf7Ebx;
        if (leaf.Leaf == 0x80000001u) expected.Ecx |= Codegen::RosettaCpuid::kExtendedLeaf1Ecx;
        const auto got = site.Call(leaf.Leaf, leaf.Subleaf);
        if (got.Eax != expected.Eax || got.Ebx != expected.Ebx || got.Ecx != expected.Ecx || got.Edx != expected.Edx) {
            std::fprintf(stderr, "leaf %x.%u: got %08x %08x %08x %08x, expected %08x %08x %08x %08x\n", leaf.Leaf, leaf.Subleaf, got.Eax, got.Ebx, got.Ecx, got.Edx,
                         expected.Eax, expected.Ebx, expected.Ecx, expected.Edx);
            require(false, "The stub's CPUID differs from Rosetta's plus the Zen 2 features");
        }
        require(got.Carry, "The stub did not preserve RFLAGS");
    }

    // What a feature check does: AVX needs OSXSAVE and XCR0 with SSE and AVX state.
    const auto leaf1 = site.Call(1, 0);
    const auto leaf7 = site.Call(7, 0);
    require((leaf1.Ecx & (1u << 27)) && (leaf1.Ecx & (1u << 28)), "OSXSAVE and AVX are not advertised");
    require((leaf7.Ebx & (1u << 5)) && (leaf7.Ebx & (1u << 3)) && (leaf7.Ebx & (1u << 8)), "AVX2, BMI1 and BMI2 are not advertised");
    require(!(leaf7.Ebx & (1u << 19)) && !(leaf7.Ebx & (1u << 18)) && !(leaf7.Ebx & (1u << 29)), "ADX, RDSEED or SHA is advertised although Rosetta faults on it");
    std::uint32_t xcr0 = 0, xcr0High = 0;
    require(Executes("XGETBV", [&] { __asm__ volatile("xgetbv" : "=a"(xcr0), "=d"(xcr0High) : "c"(0)); }) && (xcr0 & 6) == 6,
            "XGETBV does not report SSE and AVX state although OSXSAVE is advertised");

    alignas(64) static std::uint8_t area[4096];
    require(Executes("XSAVE", [&] { __asm__ volatile("xsave (%0)\n\txrstor (%0)" : : "r"(area), "a"(7), "d"(0) : "memory"); }), "XSAVE/XRSTOR");
    require(Executes("AVX2", [] { __asm__ volatile("vpcmpeqd %%ymm0, %%ymm0, %%ymm0\n\tvpaddd %%ymm0, %%ymm0, %%ymm1\n\tvzeroupper" : : : "xmm0", "xmm1"); }), "AVX2");
    require(Executes("FMA", [] { __asm__ volatile("vfmadd231ps %%ymm0, %%ymm0, %%ymm1\n\tvzeroupper" : : : "xmm1"); }), "FMA");
    require(Executes("F16C", [] { __asm__ volatile("vcvtph2ps %%xmm0, %%ymm1\n\tvzeroupper" : : : "xmm1"); }), "F16C");
    std::uint64_t value = 0xF0;
    require(Executes("BMI1", [&] { __asm__ volatile("andn %0, %0, %0\n\ttzcnt %0, %0" : "+r"(value)); }), "BMI1");
    require(Executes("BMI2", [&] { std::uint64_t mask = 0xF0; __asm__ volatile("pdep %1, %0, %0\n\tshlx %1, %0, %0" : "+r"(value) : "r"(mask)); }), "BMI2");
    std::uint64_t one = 1;
    require(Executes("LZCNT", [&] { __asm__ volatile("lzcnt %0, %0" : "+r"(one)); }) && one == 63, "LZCNT executes as BSR");
    std::uint32_t big = 0x11223344, swapped = 0;
    require(Executes("MOVBE", [&] { __asm__ volatile("movbe %1, %0" : "=r"(swapped) : "m"(big)); }) && swapped == 0x44332211u, "MOVBE");
    require(Executes("RDRAND", [&] { std::uint64_t random; __asm__ volatile("rdrand %0" : "=r"(random)); }), "RDRAND");
}

#endif

}

int main() {
    Matching();
#ifdef ROSETTA_CPUID_EXECUTE
    Execution();
#endif
    if (g_failures != 0) return EXIT_FAILURE;
    std::printf("rosetta_cpuid: ok\n");
    return EXIT_SUCCESS;
}
