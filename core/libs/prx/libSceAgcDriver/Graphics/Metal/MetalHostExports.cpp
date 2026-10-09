// Host services the shared AGC sources import, provided for the native Metal build, which links
// neither the Vulkan driver's shader registry nor libkernel.
#include "prx/libSceAgcDriver/Execution/include/ShaderPreparationScope.hpp"
#include "prx/libc/include/General.hpp"
#include <sys/mman.h>
#include <cstddef>
#include <cstdint>

// Ahead-of-time shader preparation: the Vulkan driver records prepared artifacts inside these
// transactions (Driver/Shaders/ShaderRegistry.cpp). The Metal backend compiles its pipelines when
// a draw or dispatch first uses them, so it keeps no prepared state and the scope is empty.
extern "C" AgcDriver::DriverDetail::ShaderPreparationTransaction* AgcDriverBeginShaderPreparation_nid_postfix() {
    return nullptr;
}

extern "C" void AgcDriverCommitShaderPreparation_nid_postfix(AgcDriver::DriverDetail::ShaderPreparationTransaction* transaction) {
    static_cast<void>(transaction);
}

extern "C" void AgcDriverEndShaderPreparation_nid_postfix(AgcDriver::DriverDetail::ShaderPreparationTransaction* transaction) noexcept {
    static_cast<void>(transaction);
}

// The global data share (Pm4::GdsAddress) is a private anonymous mapping. Here it is host memory
// outside every borrowed guest range, so native guest memory checks refuse GDS packet accesses
// instead of serving them.
extern "C" void* APS5_VABI mmap_nid_postfix(void* address, std::size_t length, int protection, int flags, int descriptor, std::int64_t offset) noexcept {
    constexpr int GuestPrivateAnonymous = 0x1002;
    if (address != nullptr || length == 0 || (protection & ~(PROT_READ | PROT_WRITE)) != 0 ||
        flags != GuestPrivateAnonymous || descriptor != -1 || offset != 0) {
        return MAP_FAILED;
    }
    return ::mmap(nullptr, length, protection, MAP_PRIVATE | MAP_ANON, -1, 0);
}
