#include "prx/libSceAgcDriver/Graphics/include/Context.hpp"
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <map>
#include <string>
#include <string_view>
#include <vector>

using namespace AgcDriver::Graphics;

namespace {

int failures = 0;

void Expect(bool condition, const std::string& what) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++failures;
}

constexpr std::uint32_t HostType = 0;
constexpr std::uint32_t DeviceType = 1;

struct MockDevice {
    std::deque<VkResult> results;
    std::vector<std::uint32_t> types;
    std::uint64_t next = 1;
};

MockDevice mock;

VKAPI_ATTR VkResult VKAPI_CALL mockAllocateMemory(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*, VkDeviceMemory* memory) {
    mock.types.push_back(info->memoryTypeIndex);
    auto result = VK_SUCCESS;
    if (!mock.results.empty()) {
        result = mock.results.front();
        mock.results.pop_front();
    }
    *memory = result == VK_SUCCESS ? reinterpret_cast<VkDeviceMemory>(static_cast<std::uintptr_t>(mock.next++)) : VK_NULL_HANDLE;
    return result;
}

PFN_vkVoidFunction VKAPI_CALL mockProc(VkDevice, const char* name) {
    if (std::string_view(name) == "vkAllocateMemory") return reinterpret_cast<PFN_vkVoidFunction>(mockAllocateMemory);
    return nullptr;
}

Context mockContext() {
    Context context{};
    context.deviceProc = mockProc;
    context.memory.memoryHeapCount = 2;
    context.memory.memoryHeaps[0] = {4096ull << 20u, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    context.memory.memoryHeaps[1] = {16384ull << 20u, 0};
    context.memory.memoryTypeCount = 2;
    context.memory.memoryTypes[HostType] = {0, 1};
    context.memory.memoryTypes[DeviceType] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    return context;
}

void Allocate(const char* what, std::deque<VkResult> results, std::uint32_t mask, VkResult expected, std::vector<std::uint32_t> types) {
    mock = MockDevice{};
    mock.results = std::move(results);
    const auto context = mockContext();
    VkDeviceMemory memory = VK_NULL_HANDLE;
    const auto result = context.AllocateDeviceMemory(mask, 1u << 20u, &memory, "test");
    Expect(result == expected, std::string(what) + ": unexpected result " + std::to_string(result));
    Expect(mock.types == types, std::string(what) + ": unexpected memory types tried");
    Expect((result == VK_SUCCESS) == (memory != VK_NULL_HANDLE), std::string(what) + ": memory handle does not match the result");
}

}

int main() {
    try {
        const std::uint32_t both = (1u << HostType) | (1u << DeviceType);
        Allocate("device memory available", {}, both, VK_SUCCESS, {DeviceType});
        Allocate("device memory exhausted", {VK_ERROR_OUT_OF_DEVICE_MEMORY}, both, VK_SUCCESS, {DeviceType, HostType});
        Allocate("no host type allowed", {VK_ERROR_OUT_OF_DEVICE_MEMORY}, 1u << DeviceType, VK_ERROR_OUT_OF_DEVICE_MEMORY, {DeviceType});
        Allocate("host memory exhausted", {VK_ERROR_OUT_OF_HOST_MEMORY}, both, VK_ERROR_OUT_OF_HOST_MEMORY, {DeviceType});
        Allocate("device lost", {VK_ERROR_DEVICE_LOST}, both, VK_ERROR_DEVICE_LOST, {DeviceType});
        Allocate("fallback fails", {VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_OUT_OF_HOST_MEMORY}, both, VK_ERROR_OUT_OF_HOST_MEMORY, {DeviceType, HostType});
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    if (failures != 0) return 1;
    std::puts("device memory fallback tests passed");
    return 0;
}
