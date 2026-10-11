#include <stdexcept>
#include <string>

#if defined(__APPLE__)
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#endif

extern "C" void NotImplemented_nid_no_patch(const char* funcName) {
#if defined(__APPLE__)
    // Rosetta direct path (tools/rosetta/run_title.py): ANYPS5_UNIMPLEMENTED=continue logs each
    // name once, in the same form as the missing-NID stubs, and lets the caller return its
    // default instead of aborting the title.
    static const bool keepGoing = [] {
        const char* mode = std::getenv("ANYPS5_UNIMPLEMENTED");
        return mode != nullptr && std::string(mode) == "continue";
    }();
    if (keepGoing) {
        static std::mutex mutex;
        static std::set<std::string> seen;
        std::lock_guard lock(mutex);
        if (seen.insert(funcName).second) std::fprintf(stderr, "[rosetta] unimplemented import %s (library)\n", funcName);
        return;
    }
#endif
    throw std::runtime_error(std::string(funcName) + " not implemented");
}
