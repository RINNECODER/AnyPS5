// Rosetta direct path: a CPU sampling profiler for development (fork-only).
//
// ANYPS5_PROFILE_SECONDS=N samples the program counter of whichever thread is
// on the CPU every millisecond of process CPU time (ITIMER_PROF, delivered to
// the running thread) for N seconds after libkernel loads, then prints
//   [profile] <samples> <percent> <image>+<offset> [guest=<vaddr>] [symbol]
// for the hottest addresses and 4 KiB code pages, and per image. Guest code
// has no symbols: look the guest vaddr up in the unrelinked ELF.
//
// ANYPS5_PROFILE_TRIGGER_FILE=<path> holds the N-second window until <path>
// exists (create it once the scene to profile is on screen) and prints
// "[profile] window start" when sampling begins.

#if defined(__APPLE__) && defined(__x86_64__)

#include <dlfcn.h>
#include <pthread.h>
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kCapacity = std::size_t{1} << 21;

std::uint64_t* g_samples = nullptr;
std::uint64_t* g_threads = nullptr;
// Per sample: the word at RSP (the caller of a leaf such as a syscall stub) and
// the first frame-pointer return addresses.
constexpr std::size_t kCallers = 6;
std::uint64_t* g_callers = nullptr;

bool ReadWord(std::uint64_t address, std::uint64_t* value) {
    mach_vm_size_t read = 0;
    return address != 0 && mach_vm_read_overwrite(mach_task_self(), address, sizeof(*value), reinterpret_cast<mach_vm_address_t>(value), &read) == KERN_SUCCESS &&
           read == sizeof(*value);
}
std::atomic<std::size_t> g_count{0};
std::atomic<bool> g_running{false};

void OnProfileTick(int, siginfo_t*, void* raw) {
    if (!g_running.load(std::memory_order_relaxed)) return;
    const auto* context = static_cast<ucontext_t*>(raw);
    const auto index = g_count.fetch_add(1, std::memory_order_relaxed);
    if (index >= kCapacity) return;
    g_samples[index] = context->uc_mcontext->__ss.__rip;
    std::uint64_t thread = 0;
    pthread_threadid_np(nullptr, &thread);
    g_threads[index] = thread;
    auto* callers = g_callers + index * kCallers;
    const auto& state = context->uc_mcontext->__ss;
    std::uint64_t word = 0;
    callers[0] = ReadWord(state.__rsp, &word) ? word : 0;
    std::uint64_t frame = state.__rbp;
    for (std::size_t depth = 1; depth < kCallers; ++depth) {
        std::uint64_t next = 0, returnAddress = 0;
        if (!ReadWord(frame, &next) || !ReadWord(frame + 8, &returnAddress) || returnAddress == 0) break;
        callers[depth] = returnAddress;
        if (next <= frame) break;
        frame = next;
    }
}

struct Location {
    std::string image = "?";
    std::uint64_t offset = 0;
    std::uint64_t guest = 0;
    bool hasGuest = false;
    std::string symbol;
};

Location Locate(std::uint64_t address) {
    Location location;
    Dl_info info{};
    if (!::dladdr(reinterpret_cast<void*>(address), &info) || info.dli_fbase == nullptr) return location;
    const char* image = info.dli_fname != nullptr ? info.dli_fname : "?";
    if (const char* slash = std::strrchr(image, '/')) image = slash + 1;
    location.image = image;
    location.offset = address - reinterpret_cast<std::uint64_t>(info.dli_fbase);
    unsigned long metaSize = 0;
    const auto* header = static_cast<const mach_header_64*>(info.dli_fbase);
    // Relinked images carry the (slid) guest load bias in __APS5DATA,__meta at offset 8.
    const auto* meta = reinterpret_cast<const std::uint64_t*>(getsectiondata(header, "__APS5DATA", "__meta", &metaSize));
    if (meta != nullptr && metaSize >= 16 && address >= meta[1]) {
        location.guest = address - meta[1];
        location.hasGuest = true;
    }
    if (info.dli_sname != nullptr) location.symbol = info.dli_sname;
    return location;
}

std::string Describe(const Location& location, std::uint64_t offset, std::uint64_t guest) {
    char text[512];
    if (location.hasGuest)
        std::snprintf(text, sizeof text, "%s+%#llx guest=%#llx %s", location.image.c_str(), static_cast<unsigned long long>(offset),
                      static_cast<unsigned long long>(guest), location.symbol.c_str());
    else
        std::snprintf(text, sizeof text, "%s+%#llx %s", location.image.c_str(), static_cast<unsigned long long>(offset), location.symbol.c_str());
    return text;
}

template<typename TKey>
std::vector<std::pair<TKey, std::size_t>> Top(const std::unordered_map<TKey, std::size_t>& counts, std::size_t limit) {
    std::vector<std::pair<TKey, std::size_t>> rows(counts.begin(), counts.end());
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) { return left.second > right.second; });
    if (rows.size() > limit) rows.resize(limit);
    return rows;
}

void Report() {
    const auto total = std::min(g_count.load(), kCapacity);
    std::unordered_map<std::uint64_t, std::size_t> addresses;
    for (std::size_t index = 0; index < total; ++index) ++addresses[g_samples[index]];
    std::unordered_map<std::string, std::size_t> images, symbols;
    std::unordered_map<std::uint64_t, std::size_t> pages;
    std::map<std::uint64_t, Location> located;
    for (const auto& [address, count] : addresses) {
        const auto& location = located.emplace(address, Locate(address)).first->second;
        images[location.image] += count;
        if (!location.symbol.empty() && !location.hasGuest) symbols[location.image + " " + location.symbol] += count;
        pages[address & ~std::uint64_t{0xfff}] += count;
    }
    const auto percent = [&](std::size_t count) { return total == 0 ? 0.0 : 100.0 * static_cast<double>(count) / static_cast<double>(total); };
    std::fprintf(stderr, "[profile] %zu samples (1 per ms of CPU time)\n", total);
    // Busiest threads and what each spends its time in (per image).
    std::unordered_map<std::uint64_t, std::size_t> threads;
    for (std::size_t index = 0; index < total; ++index) ++threads[g_threads[index]];
    for (const auto& [thread, count] : Top(threads, 6)) {
        std::unordered_map<std::string, std::size_t> perImage;
        for (std::size_t index = 0; index < total; ++index)
            if (g_threads[index] == thread) perImage[located.at(g_samples[index]).image] += 1;
        std::string summary;
        for (const auto& [image, hits] : Top(perImage, 5)) summary += " " + image + ":" + std::to_string(hits);
        std::fprintf(stderr, "[profile] thread %llu %6zu %5.1f%%%s\n", static_cast<unsigned long long>(thread), count, percent(count), summary.c_str());
        std::unordered_map<std::string, std::size_t> perSymbol;
        for (std::size_t index = 0; index < total; ++index) {
            if (g_threads[index] != thread) continue;
            const auto& location = located.at(g_samples[index]);
            perSymbol[location.hasGuest ? location.image + " (guest)" : location.image + " " + location.symbol] += 1;
        }
        std::map<std::uint64_t, std::string> names;
        const auto name = [&](std::uint64_t address) -> const std::string& {
            auto found = names.find(address);
            if (found != names.end()) return found->second;
            const auto location = Locate(address);
            std::string text = location.hasGuest ? location.image + "@" + std::to_string(location.guest) : location.image + ":" + (location.symbol.empty() ? "?" : location.symbol);
            if (text.size() > 90) text.resize(90);
            return names.emplace(address, text).first->second;
        };
        for (const auto& [symbol, hits] : Top(perSymbol, 8)) {
            std::fprintf(stderr, "[profile]   %6zu %s\n", hits, symbol.substr(0, 160).c_str());
            std::unordered_map<std::string, std::size_t> stacks;
            for (std::size_t index = 0; index < total; ++index) {
                if (g_threads[index] != thread) continue;
                const auto& location = located.at(g_samples[index]);
                if ((location.hasGuest ? location.image + " (guest)" : location.image + " " + location.symbol) != symbol) continue;
                std::string stack;
                for (std::size_t depth = 0; depth < kCallers; ++depth)
                    if (const auto caller = g_callers[index * kCallers + depth]; caller != 0) stack += " < " + name(caller);
                ++stacks[stack];
            }
            for (const auto& [stack, count] : Top(stacks, 3)) std::fprintf(stderr, "[profile]          %5zu%s\n", count, stack.c_str());
        }
    }
    for (const auto& [image, count] : Top(images, 12)) std::fprintf(stderr, "[profile] image %6zu %5.1f%% %s\n", count, percent(count), image.c_str());
    for (const auto& [symbol, count] : Top(symbols, 20)) std::fprintf(stderr, "[profile] host  %6zu %5.1f%% %s\n", count, percent(count), symbol.c_str());
    for (const auto& [page, count] : Top(pages, 20)) {
        const auto location = Locate(page + 0x800);
        std::fprintf(stderr, "[profile] page  %6zu %5.1f%% %s\n", count, percent(count),
                     Describe(location, location.offset & ~std::uint64_t{0xfff}, location.guest & ~std::uint64_t{0xfff}).c_str());
    }
    for (const auto& [address, count] : Top(addresses, 30)) {
        const auto& location = located.at(address);
        std::fprintf(stderr, "[profile] pc    %6zu %5.1f%% %s\n", count, percent(count), Describe(location, location.offset, location.guest).c_str());
    }
}

__attribute__((constructor)) void StartGuestProfiler() {
    const char* value = std::getenv("ANYPS5_PROFILE_SECONDS");
    if (value == nullptr || *value == 0) return;
    const double seconds = std::strtod(value, nullptr);
    if (seconds <= 0) return;
    g_samples = static_cast<std::uint64_t*>(std::calloc(kCapacity, sizeof(std::uint64_t)));
    g_threads = static_cast<std::uint64_t*>(std::calloc(kCapacity, sizeof(std::uint64_t)));
    g_callers = static_cast<std::uint64_t*>(std::calloc(kCapacity * kCallers, sizeof(std::uint64_t)));
    if (g_samples == nullptr || g_threads == nullptr || g_callers == nullptr) return;
    struct sigaction action {};
    action.sa_sigaction = OnProfileTick;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    if (::sigaction(SIGPROF, &action, nullptr) != 0) return;
    const char* trigger = std::getenv("ANYPS5_PROFILE_TRIGGER_FILE");
    std::string triggerPath = trigger != nullptr ? trigger : "";
    std::thread([seconds, triggerPath] {
        while (!triggerPath.empty() && ::access(triggerPath.c_str(), F_OK) != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        g_running.store(true);
        itimerval timer{};
        timer.it_interval.tv_usec = 1000;
        timer.it_value.tv_usec = 1000;
        ::setitimer(ITIMER_PROF, &timer, nullptr);
        if (!triggerPath.empty()) std::fprintf(stderr, "[profile] window start\n");
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        itimerval off{};
        ::setitimer(ITIMER_PROF, &off, nullptr);
        g_running.store(false);
        Report();
    }).detach();
}

}

#endif
