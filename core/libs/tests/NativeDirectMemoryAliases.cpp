#include "prx/libc/include/GuestWriteWatch.hpp"
#include <cstddef>
#include <cstdint>
#include <source_location>
#include <stdexcept>
#include <string>

extern "C" {
int sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t, std::size_t, int, std::int64_t*);
int sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
int sceKernelMunmap(void*, std::size_t);
int sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
}

static void Require(bool condition, std::source_location source = std::source_location::current()) {
    if (!condition) throw std::runtime_error("native direct alias failure at " + std::to_string(source.line()));
}

int main() {
    constexpr std::size_t page = 0x4000;
    Require(GuestWriteWatch::GuestWriteWatchAvailable_nid_postfix());
    const auto watched = [](const void* pointer, std::size_t offset) {
        return GuestWriteWatch::GuestWriteWatchCovers_nid_postfix(reinterpret_cast<std::uintptr_t>(pointer) + offset, page);
    };
    std::int64_t physical = 0;
    Require(sceKernelAllocateDirectMemory(0, 0x7fffffffffll, page * 4, 0, 0, &physical) == 0);
    void* whole = nullptr;
    Require(sceKernelMapDirectMemory(&whole, page * 4, 3, 0, physical, 0) == 0);
    Require(watched(whole, 0) && watched(whole, page * 3));
    void* pair = nullptr;
    Require(sceKernelMapDirectMemory(&pair, page * 2, 3, 0, physical + page, 0) == 0);
    Require(watched(whole, 0) && !watched(whole, page) && !watched(whole, page * 2) && watched(whole, page * 3));
    Require(!watched(pair, 0) && !watched(pair, page));
    static_cast<volatile unsigned char*>(pair)[3] = 42;
    Require(static_cast<volatile unsigned char*>(whole)[page + 3] == 42);
    Require(sceKernelMunmap(pair, page) == 0);
    void* single = nullptr;
    Require(sceKernelMapDirectMemory(&single, page, 3, 0, physical + page, 0) == 0);
    Require(!watched(single, 0));
    Require(sceKernelMunmap(single, page) == 0);
    Require(sceKernelMunmap(static_cast<unsigned char*>(pair) + page, page) == 0);
    void* replaced = whole;
    Require(sceKernelMapDirectMemory(&replaced, page, 3, 0x10, physical + page * 3, 0) == 0 && replaced == whole);
    Require(!watched(whole, 0) && !watched(whole, page * 3));
    void* alone = nullptr;
    Require(sceKernelMapDirectMemory(&alone, page, 3, 0, physical, 0) == 0);
    Require(watched(alone, 0));
    void* again = nullptr;
    Require(sceKernelMapDirectMemory(&again, page, 3, 0, physical, 0) == 0);
    Require(!watched(alone, 0) && !watched(again, 0));
    Require(sceKernelMunmap(again, page) == 0);
    Require(sceKernelMunmap(alone, page) == 0);
    Require(sceKernelMunmap(whole, page * 4) == 0);
    Require(sceKernelReleaseDirectMemory(physical, page * 4) == 0);
}
