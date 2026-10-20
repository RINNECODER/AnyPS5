#include "SceTypes.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <thread>

extern "C" {
int APS5_VABI sceUltInitialize();
int APS5_VABI sceUltFinalize();
int APS5_VABI _sceUltMutexCreate(void*, const char*, void*, const void*, std::uint32_t);
int APS5_VABI sceUltMutexLock(void*);
int APS5_VABI sceUltMutexTryLock(void*);
int APS5_VABI sceUltMutexUnlock(void*);
int APS5_VABI sceUltMutexDestroy(void*);
int APS5_VABI sceUltSemaphoreOptParamInitialize(void*, std::uint32_t);
int APS5_VABI sceUltQueueOptParamInitialize(void*, std::uint32_t);
int APS5_VABI sceUltQueueDataResourcePoolOptParamInitialize(void*, std::uint32_t);
int APS5_VABI sceUltWaitingQueueResourcePoolOptParamInitialize(void*, std::uint32_t);
int APS5_VABI sceUltUlthreadOptParamInitialize(void*, std::uint32_t);
int APS5_VABI _sceUltWaitingQueueResourcePoolCreate(void*, const char*, std::uint32_t, std::uint32_t, void*, const void*, std::uint32_t);
int APS5_VABI sceUltWaitingQueueResourcePoolDestroy(void*);
int APS5_VABI sceUltReaderWriterLockOptParamInitialize(void*, std::uint32_t);
int APS5_VABI sceUltReaderWriterLockCreate(void*, const char*, void*, const void*, std::uint32_t);
int APS5_VABI sceUltReaderWriterLockDestroy(void*);
int APS5_VABI sceUltReaderWriterLockLockRead(void*);
int APS5_VABI sceUltReaderWriterLockTryLockRead(void*);
int APS5_VABI sceUltReaderWriterLockUnlockRead(void*);
int APS5_VABI sceUltReaderWriterLockLockWrite(void*);
int APS5_VABI sceUltReaderWriterLockTryLockWrite(void*);
int APS5_VABI sceUltReaderWriterLockUnlockWrite(void*);
}

namespace {

constexpr int Ok = 0;
constexpr int Null = -2139029503;
constexpr int Invalid = -2139029500;
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

void OptParams() {
    using Initializer = int (APS5_VABI *)(void*, std::uint32_t);
    for (const Initializer initialize : {sceUltSemaphoreOptParamInitialize, sceUltQueueOptParamInitialize, sceUltQueueDataResourcePoolOptParamInitialize,
                                         sceUltWaitingQueueResourcePoolOptParamInitialize, sceUltUlthreadOptParamInitialize, sceUltReaderWriterLockOptParamInitialize}) {
        std::uint8_t bytes[136];
        std::memset(bytes, 0xa5, sizeof(bytes));
        CHECK(initialize(nullptr, 0) == Null);
        CHECK(initialize(bytes, 0) == Ok);
        for (std::size_t index = 0; index < 128; ++index) CHECK(bytes[index] == 0);
        for (std::size_t index = 128; index < sizeof(bytes); ++index) CHECK(bytes[index] == 0xa5);
    }
}


void WaitUntil(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!flag.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(flag.load());
}

void Settle() {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

void ReaderWriterValidation() {
    Object rwlock{};
    Object pool{};
    std::memset(&rwlock, 0x5a, sizeof(rwlock));
    CHECK(sceUltReaderWriterLockCreate(nullptr, "rwlock", nullptr, nullptr, 0) == Null);
    CHECK(sceUltReaderWriterLockCreate(&rwlock, "rwlock", &pool, nullptr, 0) == Invalid);
    for (const auto call : {sceUltReaderWriterLockDestroy, sceUltReaderWriterLockLockRead, sceUltReaderWriterLockTryLockRead, sceUltReaderWriterLockUnlockRead,
                            sceUltReaderWriterLockLockWrite, sceUltReaderWriterLockTryLockWrite, sceUltReaderWriterLockUnlockWrite}) {
        CHECK(call(nullptr) == Null);
        CHECK(call(&rwlock) == State);
    }
    CHECK(sceUltReaderWriterLockCreate(&rwlock, "rwlock", nullptr, nullptr, 0) == Ok);
    for (const auto byte : rwlock.bytes) CHECK(byte == 0);
    CHECK(sceUltReaderWriterLockCreate(&rwlock, "rwlock", nullptr, nullptr, 0) == State);
    CHECK(sceUltReaderWriterLockUnlockRead(&rwlock) == State);
    CHECK(sceUltReaderWriterLockUnlockWrite(&rwlock) == State);
    CHECK(sceUltReaderWriterLockLockRead(&rwlock) == Ok);
    CHECK(sceUltReaderWriterLockDestroy(&rwlock) == Busy);
    CHECK(sceUltReaderWriterLockUnlockWrite(&rwlock) == State);
    CHECK(sceUltReaderWriterLockUnlockRead(&rwlock) == Ok);
    CHECK(sceUltReaderWriterLockLockWrite(&rwlock) == Ok);
    CHECK(sceUltReaderWriterLockLockWrite(&rwlock) == State && sceUltReaderWriterLockTryLockWrite(&rwlock) == State);
    CHECK(sceUltReaderWriterLockLockRead(&rwlock) == State && sceUltReaderWriterLockTryLockRead(&rwlock) == State);
    CHECK(sceUltReaderWriterLockDestroy(&rwlock) == Busy);
    std::atomic<int> other{0};
    std::thread([&] { other = sceUltReaderWriterLockUnlockWrite(&rwlock); }).join();
    CHECK(other == State);
    CHECK(sceUltReaderWriterLockUnlockWrite(&rwlock) == Ok);
    CHECK(sceUltReaderWriterLockDestroy(&rwlock) == Ok);
    CHECK(sceUltReaderWriterLockLockRead(&rwlock) == State && sceUltReaderWriterLockDestroy(&rwlock) == State);
}

void ReaderWriterReaders() {
    Object rwlock{};
    CHECK(sceUltReaderWriterLockCreate(&rwlock, "readers", nullptr, nullptr, 0) == Ok);
    CHECK(sceUltReaderWriterLockLockRead(&rwlock) == Ok);
    std::atomic<int> other{0};
    std::thread([&] {
        other = sceUltReaderWriterLockTryLockRead(&rwlock);
        if (other == Ok) other = sceUltReaderWriterLockLockRead(&rwlock);
    }).join();
    CHECK(other == Ok);
    CHECK(sceUltReaderWriterLockTryLockWrite(&rwlock) == Busy);
    std::atomic<bool> started{false};
    std::atomic<bool> written{false};
    std::thread writer([&] {
        started = true;
        other = sceUltReaderWriterLockLockWrite(&rwlock);
        written = true;
        if (other == Ok) other = sceUltReaderWriterLockUnlockWrite(&rwlock);
    });
    WaitUntil(started);
    for (int held = 3; held > 0; --held) {
        Settle();
        CHECK(!written.load());
        CHECK(sceUltReaderWriterLockUnlockRead(&rwlock) == Ok);
    }
    writer.join();
    CHECK(written.load() && other == Ok);
    CHECK(sceUltReaderWriterLockUnlockRead(&rwlock) == State);
    CHECK(sceUltReaderWriterLockDestroy(&rwlock) == Ok);
}

void ReaderWriterWriter() {
    Object rwlock{};
    CHECK(sceUltReaderWriterLockCreate(&rwlock, "writer", nullptr, nullptr, 0) == Ok);
    CHECK(sceUltReaderWriterLockTryLockWrite(&rwlock) == Ok);
    std::atomic<int> other{0};
    std::thread([&] { other = sceUltReaderWriterLockTryLockRead(&rwlock); }).join();
    CHECK(other == Busy);
    std::thread([&] { other = sceUltReaderWriterLockTryLockWrite(&rwlock); }).join();
    CHECK(other == Busy);
    std::atomic<bool> started{false};
    std::atomic<bool> read{false};
    std::thread reader([&] {
        started = true;
        other = sceUltReaderWriterLockLockRead(&rwlock);
        read = true;
        if (other == Ok) other = sceUltReaderWriterLockUnlockRead(&rwlock);
    });
    WaitUntil(started);
    Settle();
    CHECK(!read.load());
    CHECK(sceUltReaderWriterLockDestroy(&rwlock) == Busy);
    CHECK(sceUltReaderWriterLockUnlockWrite(&rwlock) == Ok);
    reader.join();
    CHECK(read.load() && other == Ok);
    CHECK(sceUltReaderWriterLockTryLockWrite(&rwlock) == Ok && sceUltReaderWriterLockUnlockWrite(&rwlock) == Ok);
    CHECK(sceUltReaderWriterLockDestroy(&rwlock) == Ok);
}

void ReaderWriterPool() {
    Object pool{};
    Object rwlock{};
    std::uint8_t workArea[2048];
    CHECK(_sceUltWaitingQueueResourcePoolCreate(&pool, "pool", 2, 2, workArea, nullptr, 0) == Ok);
    CHECK(sceUltReaderWriterLockCreate(&rwlock, "pooled", &pool, nullptr, 0) == Ok);
    CHECK(sceUltReaderWriterLockLockWrite(&rwlock) == Ok);
    std::atomic<bool> started{false};
    std::atomic<int> other{0};
    std::thread reader([&] {
        started = true;
        other = sceUltReaderWriterLockLockRead(&rwlock);
        if (other == Ok) other = sceUltReaderWriterLockUnlockRead(&rwlock);
    });
    WaitUntil(started);
    Settle();
    CHECK(sceUltWaitingQueueResourcePoolDestroy(&pool) == Busy);
    CHECK(sceUltReaderWriterLockUnlockWrite(&rwlock) == Ok);
    reader.join();
    CHECK(other == Ok);
    CHECK(sceUltReaderWriterLockDestroy(&rwlock) == Ok);
    CHECK(sceUltWaitingQueueResourcePoolDestroy(&pool) == Ok);
}

void ReaderWriterFinalize() {
    Object rwlock{};
    CHECK(sceUltReaderWriterLockCreate(&rwlock, "finalize", nullptr, nullptr, 0) == Ok);
    CHECK(sceUltReaderWriterLockLockWrite(&rwlock) == Ok);
    std::atomic<bool> started{false};
    std::atomic<int> other{0};
    std::thread waiter([&] {
        started = true;
        other = sceUltReaderWriterLockLockWrite(&rwlock);
    });
    WaitUntil(started);
    Settle();
    CHECK(sceUltFinalize() == Ok);
    waiter.join();
    CHECK(other == State);
    CHECK(sceUltReaderWriterLockUnlockWrite(&rwlock) == State);
    CHECK(sceUltInitialize() == Ok);
}

}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    CHECK(sceUltInitialize() == Ok);
    if (std::strcmp(argv[1], "mutex-trylock") == 0) MutexTryLock();
    else if (std::strcmp(argv[1], "opt-params") == 0) OptParams();
    else if (std::strcmp(argv[1], "rwlock-validation") == 0) ReaderWriterValidation();
    else if (std::strcmp(argv[1], "rwlock-readers") == 0) ReaderWriterReaders();
    else if (std::strcmp(argv[1], "rwlock-writer") == 0) ReaderWriterWriter();
    else if (std::strcmp(argv[1], "rwlock-pool") == 0) ReaderWriterPool();
    else if (std::strcmp(argv[1], "rwlock-finalize") == 0) ReaderWriterFinalize();
    else return 2;
    CHECK(sceUltFinalize() == Ok);
    std::puts("ULT sync tests passed");
    return 0;
}
