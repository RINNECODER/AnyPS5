#include "SceTypes.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

extern "C" {
int APS5_VABI sceUltInitialize();
int APS5_VABI sceUltFinalize();
int APS5_VABI _sceUltMutexCreate(void*, const char*, void*, const void*, std::uint32_t);
int APS5_VABI sceUltMutexLock(void*);
int APS5_VABI sceUltMutexTryLock(void*);
int APS5_VABI sceUltMutexUnlock(void*);
int APS5_VABI sceUltMutexDestroy(void*);
}

namespace {

constexpr int Ok = 0;
constexpr int Null = -2139029503;
constexpr int State = -2139029498;
constexpr int Busy = -2139029497;

void Require(bool value, int line) {
    if (value) return;
    std::fprintf(stderr, "ULT sync check failed at line %d\n", line);
    std::abort();
}

#define CHECK(value) Require((value), __LINE__)

struct alignas(8) Object {
    std::uint8_t bytes[256];
};

void MutexTryLock() {
    Object mutex{};
    CHECK(sceUltMutexTryLock(nullptr) == Null);
    CHECK(sceUltMutexTryLock(&mutex) == State);
    CHECK(_sceUltMutexCreate(&mutex, "trylock", nullptr, nullptr, 0) == Ok);
    CHECK(sceUltMutexTryLock(&mutex) == Ok);
    CHECK(sceUltMutexTryLock(&mutex) == Ok);
    std::atomic<int> other{0};
    std::thread([&] { other = sceUltMutexTryLock(&mutex); }).join();
    CHECK(other == Busy);
    CHECK(sceUltMutexUnlock(&mutex) == Ok);
    std::thread([&] { other = sceUltMutexTryLock(&mutex); }).join();
    CHECK(other == Busy);
    CHECK(sceUltMutexUnlock(&mutex) == Ok);
    CHECK(sceUltMutexUnlock(&mutex) == State);
    std::thread([&] {
        other = sceUltMutexTryLock(&mutex);
        if (other == Ok) other = sceUltMutexUnlock(&mutex);
    }).join();
    CHECK(other == Ok);
    CHECK(sceUltMutexLock(&mutex) == Ok);
    CHECK(sceUltMutexDestroy(&mutex) == Busy);
    CHECK(sceUltMutexUnlock(&mutex) == Ok);
    CHECK(sceUltMutexDestroy(&mutex) == Ok);
    CHECK(sceUltMutexTryLock(&mutex) == State);
}

}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    CHECK(sceUltInitialize() == Ok);
    if (std::strcmp(argv[1], "mutex-trylock") == 0) MutexTryLock();
    else return 2;
    CHECK(sceUltFinalize() == Ok);
    std::puts("ULT sync tests passed");
    return 0;
}
