#include "prx/libkernel/Pthread/include/GuestTlsMacOs.hpp"
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

static void Require(bool value, const char* name) {
    if (!value) {
        std::fprintf(stderr, "Guest TLS check failed: %s\n", name);
        std::abort();
    }
}

static void CheckInvalid(unsigned test) {
    const auto child = fork();
    Require(child >= 0, "fork negative case");
    if (child == 0) {
        GuestTlsDescriptor descriptor{1, 0, 0, 64, 16, 0};
        if (test == 0) descriptor.alignment = 3;
        if (test == 1) descriptor.blockSize = ~std::uint64_t(15);
        if (test == 2) descriptor.templateSize = 8;
        if (test == 3) Aps5GuestTlsGetAddr_nid_no_patch(nullptr);
        Aps5GuestTlsRegister_nid_no_patch(&descriptor);
        const GuestTlsIndex index{&descriptor, 65};
        Aps5GuestTlsGetAddr_nid_no_patch(&index);
        _exit(0);
    }
    int status = 0;
    Require(waitpid(child, &status, 0) == child, "wait negative case");
    Require(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT, "invalid TLS rejected");
}

int main() {
    for (unsigned test = 0; test < 5; ++test) CheckInvalid(test);
    constexpr std::uint64_t initial = 0x1122334455667788;
    GuestTlsDescriptor first{1, reinterpret_cast<std::uint64_t>(&initial), sizeof(initial), 8192, 4096, 0};
    GuestTlsDescriptor second{1, 0, 0, 64, 16, 0};
    Aps5GuestTlsRegister_nid_no_patch(&first);
    Aps5GuestTlsRegister_nid_no_patch(&second);
    std::array<std::thread, 8> workers;
    std::array<std::uint64_t*, 8> addresses{};
    std::atomic<unsigned> ready{0};
    std::atomic<bool> release{false};
    for (unsigned index = 0; index < workers.size(); ++index) {
        workers[index] = std::thread([&, index] {
            const GuestTlsIndex firstIndex{&first, 0};
            const GuestTlsIndex secondIndex{&second, 0};
            auto* value = static_cast<std::uint64_t*>(Aps5GuestTlsGetAddr_nid_no_patch(&firstIndex));
            auto* other = static_cast<std::uint64_t*>(Aps5GuestTlsGetAddr_nid_no_patch(&secondIndex));
            addresses[index] = value;
            Require(reinterpret_cast<std::uintptr_t>(value) % 4096 == 0, "block alignment");
            Require(*value == initial, "initialization template");
            Require(other != value && *other == 0, "independent module");
            for (std::size_t offset = sizeof(initial); offset < first.blockSize; ++offset)
                Require(reinterpret_cast<unsigned char*>(value)[offset] == 0, "zero initialization");
            *value = index + 1;
            *other = 100 + index;
            ready.fetch_add(1, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) std::this_thread::yield();
            Require(Aps5GuestTlsGetAddr_nid_no_patch(&firstIndex) == value && *value == index + 1, "per-thread persistence");
            Require(Aps5GuestTlsGetAddr_nid_no_patch(&secondIndex) == other && *other == 100 + index, "module persistence");
            const GuestTlsIndex offsetIndex{&first, 16};
            Require(Aps5GuestTlsGetAddr_nid_no_patch(&offsetIndex) == value + 2, "dynamic TLS offset");
        });
    }
    while (ready.load(std::memory_order_acquire) != workers.size()) std::this_thread::yield();
    for (unsigned left = 0; left < addresses.size(); ++left)
        for (unsigned right = left + 1; right < addresses.size(); ++right)
            Require(addresses[left] != addresses[right], "separate thread blocks");
    release.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
}
