#include "prx/libc/include/ApplicationHeap.hpp"
#include "prx/libc/include/NativeAotImage.hpp"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace {

std::atomic<const void*> processParameters{nullptr};

void Require(bool condition, const char* reason) {
    if (!condition) throw std::invalid_argument(reason);
}

bool Contains(const NativeAotImage& image, const void* pointer, std::size_t bytes) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (pointer == nullptr) return false;
    for (std::size_t index = 0; index < image.rangeCount; ++index) {
        const auto& range = image.ranges[index];
        const auto begin = reinterpret_cast<std::uintptr_t>(range.address);
        if (address >= begin && address - begin <= range.bytes && bytes <= range.bytes - (address - begin))
            return true;
    }
    return false;
}

template<class Value>
Value Read(const NativeAotImage& image, const void* pointer, std::size_t offset) {
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    Require(offset <= std::numeric_limits<std::uintptr_t>::max() - address, "native AOT metadata address overflow");
    const auto* source = reinterpret_cast<const void*>(address + offset);
    Require(Contains(image, source, sizeof(Value)), "native AOT metadata pointer outside registered data");
    Value value;
    std::memcpy(&value, source, sizeof(value));
    return value;
}

void CheckFunction(const NativeAotImage& image, const void* function) {
    if (function == nullptr) return;
    for (std::size_t index = 0; index < image.functionCount; ++index)
        if (image.functions[index] == function) return;
    throw std::invalid_argument("native AOT allocator callback is not registered native code");
}

}

namespace {

// One PT_LOAD of the user's eboot, as emitted by core/aot/native_image.py.
struct NativeAotSegment {
    std::uint64_t vaddr;
    std::uint64_t offset;
    std::uint64_t filesz;
    std::uint64_t memsz;
    std::uint64_t flags;
};

}

// Maps the guest data segments at their guest vaddrs from the user's own eboot, so the native
// image can reference title data without carrying it in the IR. The generated native_image_init
// applies the pointer relocations afterwards.
extern "C" void NativeAotMapImage_nid_no_patch(const NativeAotSegment* segments, std::size_t count) {
    Require(segments != nullptr || count == 0, "native AOT segment table is null");
    const char* path = std::getenv("ANYPS5_AOT_EBOOT");
    Require(path != nullptr && *path != 0, "ANYPS5_AOT_EBOOT is not set");
    const int descriptor = ::open(path, O_RDONLY);
    if (descriptor < 0) throw std::runtime_error(std::string("cannot open the title eboot: ") + path);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& segment = segments[index];
        Require(segment.memsz >= segment.filesz, "native AOT segment file size exceeds memory size");
        if (segment.memsz == 0) continue;
        void* base = ::mmap(reinterpret_cast<void*>(static_cast<std::uintptr_t>(segment.vaddr)),
                            static_cast<std::size_t>(segment.memsz), PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (base == MAP_FAILED) throw std::runtime_error("cannot map the title data at its guest address");
        std::size_t copied = 0;
        while (copied < segment.filesz) {
            const auto got = ::pread(descriptor, static_cast<char*>(base) + copied,
                                     static_cast<std::size_t>(segment.filesz - copied),
                                     static_cast<off_t>(segment.offset + copied));
            if (got <= 0) { ::close(descriptor); throw std::runtime_error("truncated title eboot while mapping data"); }
            copied += static_cast<std::size_t>(got);
        }
    }
    ::close(descriptor);
}

extern "C" void NativeAotRegisterImage_nid_no_patch(const NativeAotImage* image) {
    Require(image != nullptr && image->version == 1, "unsupported native AOT image descriptor");
    Require(image->ranges != nullptr && image->rangeCount != 0, "native AOT image has no data ranges");
    Require(image->functionCount == 0 || image->functions != nullptr, "native AOT image has no function table");
    for (std::size_t index = 0; index < image->rangeCount; ++index) {
        const auto& range = image->ranges[index];
        Require(range.address != nullptr && range.bytes != 0 && range.bytes <=
            std::numeric_limits<std::uintptr_t>::max() - reinterpret_cast<std::uintptr_t>(range.address),
            "invalid native AOT data range");
    }
    const auto* parameters = image->processParameters;
    Require(image->processParameterBytes >= 0x40 && Contains(*image, parameters, image->processParameterBytes),
        "native AOT process parameters outside registered data");
    const auto bytes = Read<std::uint64_t>(*image, parameters, 0);
    Require(bytes >= 0x40 && bytes <= image->processParameterBytes &&
        Read<std::uint32_t>(*image, parameters, 8) == 0x4942524f, "invalid native AOT process parameters");
    const auto* libc = Read<const void*>(*image, parameters, 0x38);
    const auto libcBytes = Read<std::uint64_t>(*image, libc, 0);
    Require(libcBytes >= 0x38 && Contains(*image, libc, libcBytes), "invalid native AOT libc parameters");
    const auto* replacement = Read<const void*>(*image, libc, 0x30);
    Require(Contains(*image, replacement, 0x78) && Read<std::uint64_t>(*image, replacement, 0) == 0x78 &&
        Read<std::uint64_t>(*image, replacement, 8) == 2, "unsupported native AOT allocator table");
    for (std::size_t offset = 0x10; offset < 0x70; offset += sizeof(void*))
        CheckFunction(*image, Read<const void*>(*image, replacement, offset));
    const void* previous = nullptr;
    if (!processParameters.compare_exchange_strong(previous, parameters, std::memory_order_acq_rel, std::memory_order_acquire))
        Require(previous == parameters, "a different native AOT main image is already registered");
}

const void* ApplicationProcessParameters_nid_no_patch() {
    const auto* parameters = processParameters.load(std::memory_order_acquire);
    if (parameters == nullptr) throw std::runtime_error("native AOT process parameters have not been registered");
    return parameters;
}
