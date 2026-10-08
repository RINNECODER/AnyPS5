#pragma once
#include <cpu/Cpu.hpp>
#include <cpu/SceElf.hpp>
#include <array>
#include <memory>
#include <optional>

namespace Cpu { class GuestThreads; }
namespace Cpu::Platform {
// Consumer-qualified four-argument URI ABI. Encoding and bounded guest memory
// behavior use the existing public engineering NetworkServices implementation;
// this component makes no firmware HTTP transport or account-service claim.
class NativeUriEscape final {
public:
    struct Configuration {
        bool EnableTargetConsumer = false;
        bool EnablePublicFixture = false;
        std::array<std::byte, 32> PublicFixtureSha256{};
        std::uint64_t PublicFixtureSize = 0;
        std::uint64_t GateBase = 0x7ffdc7000000ULL;
    };
    // Machine and idle guest scheduler must outlive this provider. Resolve and
    // teardown belong to the idle CPU owner, after guest execution has drained.
    NativeUriEscape(Machine&, const std::shared_ptr<GuestThreads>&);
    NativeUriEscape(Machine&, const std::shared_ptr<GuestThreads>&, Configuration);
    ~NativeUriEscape();
    NativeUriEscape(const NativeUriEscape&) = delete;
    NativeUriEscape& operator=(const NativeUriEscape&) = delete;
    // No page or callback is allocated before exact immutable parser source,
    // undefined global FUNC row, type/size and scope checks all succeed.
    std::optional<std::uint64_t> Resolve(const SceImport&, std::uint8_t observedType,
                                       std::uint64_t observedSize,
                                       const SceParsedImage& actualConsumer);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
