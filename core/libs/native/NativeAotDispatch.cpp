// Fork-only: runtime dispatch table for the uniform ABI. An indirect branch whose target the
// compiler could not prove is lowered to NativeAotDispatch(value); the generated per-image
// table maps a guest code address to its native function. Unknown targets return null and the
// compiled site reports and traps. Registered once by the generated native_image_init.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>

namespace {

struct NativeAotDispatchEntry {
    std::uint64_t guest;
    const void* native;
};

const NativeAotDispatchEntry* dispatchEntries = nullptr;
std::size_t dispatchCount = 0;

void Require(bool condition, const char* reason) {
    if (!condition) throw std::invalid_argument(reason);
}

}

extern "C" void NativeAotRegisterDispatch_nid_no_patch(const NativeAotDispatchEntry* entries, std::size_t count) {
    Require(count == 0 || entries != nullptr, "native AOT dispatch table is null");
    dispatchEntries = entries;
    dispatchCount = count;
}

extern "C" void* NativeAotDispatch_nid_no_patch(std::uint64_t value) {
    const NativeAotDispatchEntry* entries = dispatchEntries;
    const std::size_t count = dispatchCount;
    std::size_t low = 0, high = count;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        if (entries[middle].guest < value)
            low = middle + 1;
        else
            high = middle;
    }
    if (low < count && entries[low].guest == value)
        return const_cast<void*>(entries[low].native);
    return nullptr;
}

extern "C" void NativeAotReportDispatchFailure_nid_no_patch(std::uint64_t site, std::uint64_t value) {
    std::fprintf(stderr, "[aot] dispatch miss at 0x%llx: target 0x%llx\n",
                 static_cast<unsigned long long>(site), static_cast<unsigned long long>(value));
}
