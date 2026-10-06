#include "prx/libSceAgcDriver/Graphics/include/MeshDraw.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include <stdexcept>
#include <string>

namespace AgcDriver::Graphics {
namespace {

void Require(bool condition, const char* reason) {
    if (!condition) throw std::runtime_error(std::string("AGC graphics: ") + reason);
}

}

std::array<std::uint32_t, 4> MeshIndexBufferDescriptor(const Pm4::DrawParameters& draw, std::uint64_t unreadAddress) {
    const auto address = draw.indexed ? draw.indexAddress : unreadAddress;
    const auto bytes = draw.indexed ? (static_cast<std::uint64_t>(draw.indexCount) * draw.indexSize + 3u) & ~std::uint64_t{3} : 4u;
    Require(address != 0 && bytes != 0 && bytes <= 0xffffffffu && (address >> 48u) == 0, "invalid mesh index buffer range");
    constexpr std::uint32_t RawWord3 = 0x31016facu;
    return {static_cast<std::uint32_t>(address), static_cast<std::uint32_t>(address >> 32u) & 0xffffu, static_cast<std::uint32_t>(bytes), RawWord3};
}

}
