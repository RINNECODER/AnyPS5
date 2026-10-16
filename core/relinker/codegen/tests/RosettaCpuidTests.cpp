// relinker --to-rosetta CPUID stub: lowers a CPUID site, links it the way the
// relinker does (jump at the site, stub returns after the absorbed
// instructions) and runs it under Rosetta. Every feature bit the stub adds must
// execute there, and every other leaf must stay what Rosetta reports.

#include <codegen/IAmd64OnlyConverter.hpp>
#include <codegen/x86/IAmd64OnlyInstructionMatcher.hpp>
#include <codegen/x86/RosettaCpuid.hpp>
#include <codegen/x86/ScalarCompareOperands.hpp>
#include <codegen/x86/StubBodyBuilder.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
    static constexpr std::size_t kDataOffset = 0x80;

    // `text` is one function, `data` is placed at kDataOffset (for RIP-relative operands).
    explicit LinkedSite(const Bytes& text = kFunction, const Bytes& data = {}) {
        const auto result = Convert(text, Codegen::Amd64OnlyTarget::Rosetta);
        if (result.Trampolines.size() != 1) return;
        const auto& site = result.Trampolines[0];
        _page = static_cast<std::uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
        if (_page == MAP_FAILED) {
            _page = nullptr;
            return;
        }
        std::memset(_page, 0xCC, 4096);
        std::copy(text.begin(), text.end(), _page);
        std::copy(data.begin(), data.end(), _page + kDataOffset);
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
    [[nodiscard]] const std::uint8_t* Address() const { return _page; }

    // Calls the site with CF set, below the caller's red zone. The carry goes to a register: a memory
    // operand could be addressed from rsp, which the asm has moved.
    Registers Call(std::uint32_t leaf, std::uint32_t subleaf) const {
        std::uint32_t out[4] = {};
        std::uint8_t carry = 0;
        __asm__ volatile(
            "sub $128, %%rsp\n\t"
            "stc\n\t"
            "call *%[fn]\n\t"
            "setc %[carry]\n\t"
            "add $128, %%rsp"
            : "+a"(leaf), "+c"(subleaf), [carry] "=&q"(carry)
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


// ---- RCPPS / RSQRTPS / RCPSS / RSQRTSS: every form must give the exact 1/x and 1/sqrt(x) ----

struct VectorFile {
    alignas(32) std::uint8_t Reg[16][32];
};

constexpr std::size_t kStackOperand = 0x20;  // guest operand [rsp + 0x28]: 0x20 above rsp before the call pushes a return address

// Loads the 16 ymm registers, calls the site with CF set below a 128-byte red zone, stores them back.
// rdx points at the memory operand; its 32 bytes are also copied to [rsp + kStackOperand] for rsp-based operands.
bool CallVectors(const std::uint8_t* function, const VectorFile& in, VectorFile& out, const std::uint8_t* memory) {
    std::uint8_t carry = 0;
    __asm__ volatile(
        "sub $128, %%rsp\n\t"
        "vmovups (%[mem]), %%ymm0\n\t"
        "vmovups %%ymm0, 0x20(%%rsp)\n\t"
        "vmovups 0(%[in]), %%ymm0\n\t"
        "vmovups 32(%[in]), %%ymm1\n\t"
        "vmovups 64(%[in]), %%ymm2\n\t"
        "vmovups 96(%[in]), %%ymm3\n\t"
        "vmovups 128(%[in]), %%ymm4\n\t"
        "vmovups 160(%[in]), %%ymm5\n\t"
        "vmovups 192(%[in]), %%ymm6\n\t"
        "vmovups 224(%[in]), %%ymm7\n\t"
        "vmovups 256(%[in]), %%ymm8\n\t"
        "vmovups 288(%[in]), %%ymm9\n\t"
        "vmovups 320(%[in]), %%ymm10\n\t"
        "vmovups 352(%[in]), %%ymm11\n\t"
        "vmovups 384(%[in]), %%ymm12\n\t"
        "vmovups 416(%[in]), %%ymm13\n\t"
        "vmovups 448(%[in]), %%ymm14\n\t"
        "vmovups 480(%[in]), %%ymm15\n\t"
        "stc\n\t"
        "call *%[fn]\n\t"
        "setc %[carry]\n\t"
        "vmovups %%ymm0, 0(%[out])\n\t"
        "vmovups %%ymm1, 32(%[out])\n\t"
        "vmovups %%ymm2, 64(%[out])\n\t"
        "vmovups %%ymm3, 96(%[out])\n\t"
        "vmovups %%ymm4, 128(%[out])\n\t"
        "vmovups %%ymm5, 160(%[out])\n\t"
        "vmovups %%ymm6, 192(%[out])\n\t"
        "vmovups %%ymm7, 224(%[out])\n\t"
        "vmovups %%ymm8, 256(%[out])\n\t"
        "vmovups %%ymm9, 288(%[out])\n\t"
        "vmovups %%ymm10, 320(%[out])\n\t"
        "vmovups %%ymm11, 352(%[out])\n\t"
        "vmovups %%ymm12, 384(%[out])\n\t"
        "vmovups %%ymm13, 416(%[out])\n\t"
        "vmovups %%ymm14, 448(%[out])\n\t"
        "vmovups %%ymm15, 480(%[out])\n\t"
        "add $128, %%rsp"
        : [carry] "=&q"(carry)
        : [fn] "r"(function), [in] "r"(&in), [out] "r"(&out), [mem] "d"(memory)
        : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15", "memory", "cc");
    return carry != 0;
}

enum class Operand { Register, BaseRdx, StackBase, RipRelative };

struct Form {
    bool Rsqrt;
    bool Scalar;
    bool Vex;
    bool Ymm;
    Operand Source;
};

struct RegSet {
    std::uint8_t Dst, Src, First;
};

// The guest instruction (followed by padding the jump can absorb and a ret).
Bytes Encode(const Form& form, const RegSet& regs) {
    Bytes out;
    const std::uint8_t opcode = form.Rsqrt ? 0x52 : 0x53;
    const bool registerSource = form.Source == Operand::Register;
    if (form.Vex) {
        out.push_back(0xC4);
        out.push_back(static_cast<std::uint8_t>((regs.Dst >= 8 ? 0x00 : 0x80) | 0x40 | (registerSource && regs.Src >= 8 ? 0x00 : 0x20) | 0x01));
        const std::uint8_t vvvv = form.Scalar ? regs.First : 0;
        out.push_back(static_cast<std::uint8_t>((((~vvvv) & 0x0F) << 3) | (form.Ymm ? 0x04 : 0x00) | (form.Scalar ? 0x02 : 0x00)));
    } else {
        if (form.Scalar)
            out.push_back(0xF3);
        const auto rex = static_cast<std::uint8_t>(0x40 | (regs.Dst >= 8 ? 4 : 0) | (registerSource && regs.Src >= 8 ? 1 : 0));
        if (rex != 0x40)
            out.push_back(rex);
        out.insert(out.end(), {0x0F});
    }
    out.push_back(opcode);
    const auto reg = static_cast<std::uint8_t>((regs.Dst & 7) << 3);
    switch (form.Source) {
    case Operand::Register:
        out.push_back(static_cast<std::uint8_t>(0xC0 | reg | (regs.Src & 7)));
        break;
    case Operand::BaseRdx:
        out.push_back(static_cast<std::uint8_t>(reg | 0x02));
        break;
    case Operand::StackBase:
        out.insert(out.end(), {static_cast<std::uint8_t>(0x40 | reg | 0x04), 0x24, static_cast<std::uint8_t>(kStackOperand + 8)});
        break;
    case Operand::RipRelative: {
        out.push_back(static_cast<std::uint8_t>(reg | 0x05));
        const auto displacement = static_cast<std::int32_t>(LinkedSite::kDataOffset) - static_cast<std::int32_t>(out.size() + 4);
        for (int byte = 0; byte < 4; ++byte)
            out.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(displacement) >> (byte * 8)));
        break;
    }
    }
    out.insert(out.end(), {0x90, 0x90, 0x90, 0x90, 0x90, 0xC3});
    return out;
}

std::string Describe(const Form& form, const RegSet& regs) {
    static const char* const kSource[] = {"reg", "[rdx]", "[rsp+8]", "[rip]"};
    char text[160];
    std::snprintf(text, sizeof text, "%s%s %s dst=%u src=%u first=%u %s", form.Vex ? "v" : "", form.Rsqrt ? (form.Scalar ? "rsqrtss" : "rsqrtps") : (form.Scalar ? "rcpss" : "rcpps"),
                  form.Ymm ? "ymm" : "xmm", regs.Dst, regs.Src, regs.First, kSource[static_cast<int>(form.Source)]);
    return text;
}

float Exact(const bool rsqrt, const float value) {
    volatile float input = value;
    volatile float root = rsqrt ? std::sqrt(input) : input;
    volatile float result = 1.0f / root;
    return result;
}

bool SameFloat(const float expected, const std::uint8_t* actual) {
    float value;
    std::memcpy(&value, actual, sizeof value);
    return std::isnan(expected) ? std::isnan(value) : std::memcmp(&expected, &value, sizeof value) == 0;
}

void CheckForm(const Form& form, const RegSet& regs, const std::array<float, 8>& lanes) {
    const Bytes text = Encode(form, regs);
    Bytes data(32);
    std::memcpy(data.data(), lanes.data(), 32);
    const LinkedSite site(text, data);
    const auto description = Describe(form, regs);
    if (!site.Ready()) {
        std::fprintf(stderr, "%s: could not link the stub\n", description.c_str());
        require(false, "Could not link a reciprocal stub");
        return;
    }
    VectorFile in{}, out{};
    for (int reg = 0; reg < 16; ++reg)
        for (int byte = 0; byte < 32; ++byte)
            in.Reg[reg][byte] = static_cast<std::uint8_t>(0x10 + reg * 32 + byte);
    // The memory operand holds the lanes; a register source holds them in the low 32 bytes of its register.
    if (form.Source == Operand::Register)
        std::memcpy(in.Reg[regs.Src], lanes.data(), 32);
    const VectorFile before = in;
    alignas(32) std::uint8_t memory[32];
    std::memcpy(memory, lanes.data(), 32);
    const bool carry = CallVectors(site.Address(), in, out, memory);
    require(carry, "A reciprocal stub changed CF");

    const std::uint8_t* sourceLanes = form.Source == Operand::Register ? before.Reg[regs.Src] : memory;
    std::uint8_t expected[32];
    if (form.Scalar) {
        const std::uint8_t* base = form.Vex ? before.Reg[regs.First] : before.Reg[regs.Dst];
        std::memcpy(expected, base, 32);
        float lane0;
        std::memcpy(&lane0, sourceLanes, 4);
        const float result = Exact(form.Rsqrt, lane0);
        std::memcpy(expected, &result, 4);
        if (form.Vex)
            std::memset(expected + 16, 0, 16);
    } else {
        const int count = form.Ymm ? 8 : 4;
        std::memcpy(expected, before.Reg[regs.Dst], 32);
        for (int lane = 0; lane < count; ++lane) {
            float value;
            std::memcpy(&value, sourceLanes + lane * 4, 4);
            const float result = Exact(form.Rsqrt, value);
            std::memcpy(expected + lane * 4, &result, 4);
        }
        if (form.Vex && !form.Ymm)
            std::memset(expected + 16, 0, 16);
    }
    for (int reg = 0; reg < 16; ++reg) {
        if (reg != regs.Dst) {
            if (std::memcmp(out.Reg[reg], before.Reg[reg], 32) != 0) {
                std::fprintf(stderr, "%s: register %d was clobbered\n", description.c_str(), reg);
                require(false, "A reciprocal stub clobbered a register other than its destination");
            }
            continue;
        }
        const int floats = form.Scalar ? 1 : (form.Ymm ? 8 : 4);
        bool exact = true;
        for (int lane = 0; lane < floats; ++lane)
            exact = exact && SameFloat(*reinterpret_cast<const float*>(expected + lane * 4), out.Reg[reg] + lane * 4);
        // Everything past the computed lanes is merged or zeroed bit for bit.
        exact = exact && std::memcmp(out.Reg[reg] + floats * 4, expected + floats * 4, 32 - floats * 4) == 0;
        if (!exact) {
            std::fprintf(stderr, "%s: result differs from the exact value or the merge is wrong\n", description.c_str());
            for (int lane = 0; lane < 8; ++lane) {
                float got, want;
                std::memcpy(&got, out.Reg[reg] + lane * 4, 4);
                std::memcpy(&want, expected + lane * 4, 4);
                std::fprintf(stderr, "  lane %d: got %08x want %08x\n", lane, *reinterpret_cast<std::uint32_t*>(&got), *reinterpret_cast<std::uint32_t*>(&want));
            }
            require(false, "A reciprocal form differs from the exact value or its merge semantics");
        }
    }
}

void ReciprocalExecution() {
    if (!Translated())
        return;
    const bool avx = Executes("AVX", [] { __asm__ volatile("vxorps %%ymm0, %%ymm0, %%ymm0\n\tvzeroupper" : : : "xmm0"); });
    require(avx, "Rosetta does not execute AVX");
    if (!avx)
        return;
    const std::array<std::array<float, 8>, 4> inputs = {{
        {1.0f, 4.0f, 0.25f, 2.0f, 0.0f, -0.0f, INFINITY, -1.0f},
        {2.0f, 1.00000012f, 0.99999994f, 3.0e-38f, 1.0e30f, 0.5f, 3.0f, 10.0f},
        {NAN, -INFINITY, -4.0f, 16.0f, 7.0f, 0.1f, 1.0e-45f, 12345.678f},
        {0.5f, 1.0f, 9.0f, 100.0f, 0.3f, 1.5f, 2.5f, 1.0e10f}}};
    const RegSet triples[] = {{1, 2, 3}, {1, 1, 1}, {1, 2, 1}, {1, 2, 2}, {3, 3, 1}, {9, 10, 11}, {10, 9, 15}, {15, 15, 15}, {0, 5, 0}, {0, 1, 2}, {7, 14, 8}};
    int forms = 0;
    for (const bool rsqrt : {false, true}) {
        for (const bool vex : {false, true}) {
            for (const bool scalar : {false, true}) {
                for (const bool ymm : {false, true}) {
                    if (ymm && (!vex || scalar))
                        continue;
                    for (const Operand source : {Operand::Register, Operand::BaseRdx, Operand::StackBase, Operand::RipRelative}) {
                        for (const auto& regs : triples) {
                            if (source != Operand::Register && regs.Src != 1 && regs.Src != 2)
                                continue;  // the source register is unused by a memory form
                            for (const auto& lanes : inputs) {
                                CheckForm({rsqrt, scalar, vex, ymm, source}, regs, lanes);
                                ++forms;
                            }
                        }
                    }
                }
            }
        }
    }
    std::printf("rosetta_cpuid: %d reciprocal form executions\n", forms);
}


// ---- CMPSS / CMPSD: Rosetta loses a full-width VEX load into the compare's destination in the same block ----

struct CompareForm {
    bool Vex;
    bool Double;
    std::uint8_t Predicate;
};

// cmp xmm1, (xmm2,) xmm3, predicate: the legacy form compares xmm1 itself (preloaded with the same value as xmm2).
Bytes EncodeCompare(const CompareForm& form) {
    const std::uint8_t pp = form.Double ? 0x03 : 0x02;
    if (form.Vex)  // VEX.128.F3/F2.0F C2 /r ib; vvvv = xmm2, ModRM reg = xmm1, rm = xmm3
        return {0xC5, static_cast<std::uint8_t>(0x80 | ((~2u & 0xF) << 3) | pp), 0xC2, 0xCB, form.Predicate};
    return {static_cast<std::uint8_t>(form.Double ? 0xF2 : 0xF3), 0x0F, 0xC2, 0xCB, form.Predicate};
}

// x86 compare predicates (imm8[3:0]; bit 4 only selects signalling or quiet).
bool ComparePredicate(const double a, const double b, const std::uint8_t predicate) {
    const bool unordered = std::isnan(a) || std::isnan(b);
    switch (predicate & 0x0F) {
    case 0: return !unordered && a == b;
    case 1: return !unordered && a < b;
    case 2: return !unordered && a <= b;
    case 3: return unordered;
    case 4: return unordered || a != b;
    case 5: return unordered || !(a < b);
    case 6: return unordered || !(a <= b);
    case 7: return !unordered;
    case 8: return unordered || a == b;
    case 9: return unordered || !(a >= b);
    case 10: return unordered || !(a > b);
    case 11: return false;
    case 12: return !unordered && a != b;
    case 13: return !unordered && a >= b;
    case 14: return !unordered && a > b;
    default: return true;
    }
}

// Calls fn with xmm0..xmm3 loaded from x and rdx = memory; returns rax.
std::uint64_t CallCompare(const std::uint8_t* function, const std::uint8_t (&x)[4][16], const void* memory) {
    std::uint64_t result = 0;
    __asm__ volatile(
        "sub $128, %%rsp\n\t"
        "movdqu 0(%[x]), %%xmm0\n\t"
        "movdqu 16(%[x]), %%xmm1\n\t"
        "movdqu 32(%[x]), %%xmm2\n\t"
        "movdqu 48(%[x]), %%xmm3\n\t"
        "call *%[fn]\n\t"
        "add $128, %%rsp"
        : "=a"(result)
        : [fn] "r"(function), [x] "r"(&x), "d"(memory)
        : "xmm0", "xmm1", "xmm2", "xmm3", "memory", "cc");
    return result;
}

// The text as it stands, on its own executable page (no relinking).
class RawSite {
public:
    explicit RawSite(const Bytes& text) {
        _page = static_cast<std::uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0));
        if (_page == MAP_FAILED) { _page = nullptr; return; }
        std::memset(_page, 0xCC, 4096);
        std::copy(text.begin(), text.end(), _page);
        if (mprotect(_page, 4096, PROT_READ | PROT_EXEC) != 0) { munmap(_page, 4096); _page = nullptr; }
    }
    ~RawSite() { if (_page) munmap(_page, 4096); }
    [[nodiscard]] const std::uint8_t* Address() const { return _page; }
private:
    std::uint8_t* _page = nullptr;
};

template<typename T>
void Fill(std::uint8_t (&reg)[16], const T low, const std::uint64_t pattern) {
    std::memcpy(reg, &pattern, 8);
    std::memcpy(reg + 8, &pattern, 8);
    std::memcpy(reg, &low, sizeof low);
}

void ScalarCompareExecution() {
    if (!Translated())
        return;
    const bool avx = Executes("AVX", [] { __asm__ volatile("vxorps %%ymm0, %%ymm0, %%ymm0\n\tvzeroupper" : : : "xmm0"); });
    if (!avx)
        return;
    int lost = 0, sequences = 0, masks = 0;
    for (const bool isDouble : {false, true}) {
        for (const bool vex : {false, true}) {
            for (std::uint8_t predicate = 0; predicate < (vex ? 32 : 8); ++predicate) {
                const CompareForm form{vex, isDouble, predicate};
                const Bytes compare = EncodeCompare(form);
                const auto split = Codegen::DecodeScalarCompare(compare.data(), compare.size());
                const bool lowered = split.has_value();
                char name[64];
                std::snprintf(name, sizeof name, "%s%s predicate %u", vex ? "VCMP" : "CMP", isDouble ? "SD" : "SS", predicate);

                // 1. compare into xmm1; vmovaps xmm1, [rdx]; v(u)comis xmm1, xmm0; pushfq; pop rax; ret.
                //    [rdx] and xmm0 hold the same value, so ZF must be set and CF and PF clear.
                Bytes sequence = compare;
                const Bytes tail = {0xC5, 0xF8, 0x28, 0x0A, 0xC5, static_cast<std::uint8_t>(isDouble ? 0xF9 : 0xF8), 0x2E, 0xC8, 0x9C, 0x58, 0xC3};
                sequence.insert(sequence.end(), tail.begin(), tail.end());
                alignas(16) std::uint8_t loaded[16] = {};
                std::uint8_t x[4][16] = {};
                if (isDouble) {
                    const double value = 309.0;
                    Fill(x[0], value, 0); std::memcpy(loaded, &value, 8);
                    Fill(x[1], value, 0); Fill(x[2], value, 0); Fill(x[3], value, 0);
                } else {
                    const float value = 309.0f;
                    Fill(x[0], value, 0); std::memcpy(loaded, &value, 4);
                    Fill(x[1], value, 0); Fill(x[2], value, 0); Fill(x[3], value, 0);
                }
                const auto equal = [](const std::uint64_t flags) { return (flags & 0x45) == 0x40; };
                const RawSite raw(sequence);
                const bool rawEqual = raw.Address() && equal(CallCompare(raw.Address(), x, loaded));
                ++sequences;
                if (!rawEqual) {
                    ++lost;
                    if (!lowered) {
                        std::fprintf(stderr, "%s: Rosetta loses the load after it, and it is not split\n", name);
                        require(false, "A scalar compare Rosetta mistranslates is not split");
                    }
                }
                if (lowered) {
                    const LinkedSite site(sequence);
                    require(site.Ready() && equal(CallCompare(site.Address(), x, loaded)), "A split scalar compare still loses the following load");
                }

                // 2. The compare's own result, merge included: vmovq rax, xmm1; ret.
                Bytes result = compare;
                const Bytes movq = {0xC4, 0xE1, 0xF9, 0x7E, 0xC8, 0xC3};
                result.insert(result.end(), movq.begin(), movq.end());
                const double pairs[][2] = {{1, 2}, {2, 1}, {2, 2}, {NAN, 1}};
                for (const auto& pair : pairs) {
                    std::uint8_t in[4][16] = {};
                    const std::uint64_t first = 0x1111111122222222ull, destination = 0x3333333344444444ull;
                    if (isDouble) {
                        Fill(in[1], pair[0], destination); Fill(in[2], pair[0], first); Fill(in[3], pair[1], 0);
                    } else {
                        Fill(in[1], static_cast<float>(pair[0]), destination); Fill(in[2], static_cast<float>(pair[0]), first); Fill(in[3], static_cast<float>(pair[1]), 0);
                    }
                    const bool truth = ComparePredicate(pair[0], pair[1], vex ? predicate : static_cast<std::uint8_t>(predicate & 7));
                    // VEX takes bits 63:32 from the first source, the legacy form keeps the destination's.
                    const std::uint64_t upper = (vex ? first : destination) & 0xFFFFFFFF00000000ull;
                    const std::uint64_t expected = isDouble ? (truth ? ~0ull : 0ull) : (upper | (truth ? 0xFFFFFFFFull : 0ull));
                    std::uint64_t got = 0;
                    if (lowered) {
                        const LinkedSite site(result);
                        got = site.Ready() ? CallCompare(site.Address(), in, loaded) : ~expected;
                    } else {
                        const RawSite site(result);
                        got = site.Address() ? CallCompare(site.Address(), in, loaded) : ~expected;
                    }
                    ++masks;
                    if (got != expected) {
                        std::fprintf(stderr, "%s (%g, %g): got %016llx want %016llx\n", name, pair[0], pair[1], static_cast<unsigned long long>(got), static_cast<unsigned long long>(expected));
                        require(false, "A scalar compare gives the wrong result");
                    }
                }
            }
        }
    }
    std::printf("rosetta_cpuid: %d compare + load sequences (Rosetta loses the load in %d of them as written); %d compare results\n", sequences, lost, masks);
}

#endif

}

int main() {
    Matching();
#ifdef ROSETTA_CPUID_EXECUTE
    Execution();
    ReciprocalExecution();
    ScalarCompareExecution();
#endif
    if (g_failures != 0) return EXIT_FAILURE;
    std::printf("rosetta_cpuid: ok\n");
    return EXIT_SUCCESS;
}
