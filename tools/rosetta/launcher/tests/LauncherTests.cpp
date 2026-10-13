// The launcher's Mach-O writer at the dyld boundary: the stub dylibs it writes
// must load, resolve every name through the export trie (including names that
// are prefixes of others, as NID and NID#lib#module are), return 0 and log the
// first call; ReadMachO must read an ld-built image.
//
//   rosetta_launcher_tests <aps5_stub_runtime.dylib> <work dir>
#include "MachO.hpp"
#include "RelinkCachePrune.hpp"

#include <dlfcn.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace Aps5Launcher;

namespace {

int failures = 0;

void Check(bool condition, const std::string& what) {
    if (!condition) {
        std::fprintf(stdout, "FAIL: %s\n", what.c_str());
        ++failures;
    }
}

void Write(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: rosetta_launcher_tests <aps5_stub_runtime.dylib> <work dir>\n");
        return 2;
    }
    const fs::path runtime = fs::absolute(argv[1]), work = fs::absolute(argv[2]);
    fs::remove_all(work);
    fs::create_directories(work);

    // ld output: exports and dylib loads of the stub runtime itself.
    const MachOImage runtimeImage = ReadMachO(runtime.string());
    Check(runtimeImage.exports.count("_aps5_stub_common") && runtimeImage.exports.count("_aps5_trace_common"),
          "ReadMachO lists the runtime's exports");
    bool system = false;
    for (const auto& dylib : runtimeImage.dylibs) system |= dylib.find("libSystem") != std::string::npos;
    Check(system, "ReadMachO lists the runtime's libSystem load");

    StubDylib spec;
    spec.installName = (work / "aps5_unresolved.dylib").string();
    spec.runtimePath = runtime.string();
    spec.stubs = {"_AAAAAAAAAAA", "_AAAAAAAAAAA#B#C", "_AAAAAAAAAAB#B#C", "_zzzzzzzzzz-"};
    spec.stubLabels = {"AAAAAAAAAAA (first)", "AAAAAAAAAAA#B#C", "AAAAAAAAAAB", "zzzzzzzzzz-"};
    Write(spec.installName, BuildStubDylib(spec));

    const MachOImage written = ReadMachO(spec.installName);
    for (const auto& name : spec.stubs) Check(written.exports.count(name), "export trie holds " + name);

    // Calls log to stderr: capture it in a file.
    const fs::path log = work / "stderr.txt";
    const int saved = dup(STDERR_FILENO);
    const int capture = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    dup2(capture, STDERR_FILENO);
    void* image = dlopen(spec.installName.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (image == nullptr) {
        dup2(saved, STDERR_FILENO);
        std::fprintf(stdout, "FAIL: dlopen stub dylib: %s\n", dlerror());
        return 1;
    }
    std::vector<void*> addresses;
    for (const auto& name : spec.stubs) {
        void* symbol = dlsym(image, name.c_str() + 1);
        Check(symbol != nullptr, "dlsym " + name);
        addresses.push_back(symbol);
        if (symbol != nullptr) {
            using Stub = long (*)(long, long);
            Check(reinterpret_cast<Stub>(symbol)(7, 8) == 0, name + " returns 0");
            Check(reinterpret_cast<Stub>(symbol)(7, 8) == 0, name + " returns 0 again");
        }
    }
    for (std::size_t i = 0; i < addresses.size(); ++i)
        for (std::size_t j = i + 1; j < addresses.size(); ++j) Check(addresses[i] != addresses[j], "distinct stub addresses");
    const auto* count = static_cast<const unsigned*>(dlsym(image, "aps5_stub_count"));
    Check(count != nullptr && *count == spec.stubs.size(), "aps5_stub_count");
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(capture);

    std::stringstream text;
    text << std::ifstream(log).rdbuf();
    const std::string output = text.str();
    std::size_t lines = 0;
    for (std::size_t at = output.find("[rosetta] unimplemented import "); at != std::string::npos;
         at = output.find("[rosetta] unimplemented import ", at + 1))
        ++lines;
    Check(lines == spec.stubs.size(), "one log line per stub (got " + std::to_string(lines) + ")");
    Check(output.find("unimplemented import AAAAAAAAAAA (first)\n") != std::string::npos, "log uses the stub label");

    // A placeholder for a library the build lacks: loads by its @rpath install name.
    StubDylib placeholder;
    placeholder.installName = "@rpath/libSceAps5Missing.prx";
    const fs::path placeholderPath = work / "libSceAps5Missing.prx";
    Write(placeholderPath, BuildStubDylib(placeholder));
    void* missing = dlopen(placeholderPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    Check(missing != nullptr, std::string("dlopen placeholder: ") + (missing ? "" : dlerror()));

    // Relink cache pruning: keep the most recently used entries, never the one in use or a locked one.
    {
        const fs::path cache = work / "prune-cache";
        const auto name = [](char c) { return std::string(64, c); };
        const auto now = fs::file_time_type::clock::now();
        for (int index = 0; index < 5; ++index) {
            const std::string sha = name(static_cast<char>('a' + index));
            fs::create_directories(cache / sha / "out");
            Write(cache / sha / "out" / "eboot", {1, 2, 3});
            Write(cache / (sha + ".lock"), {});
            fs::last_write_time(cache / sha, now - std::chrono::hours(10 * (index + 1)));  // a newest ... e oldest
        }
        fs::create_directories(cache / "stubs");
        const int running = MarkRelinkEntryInUse(cache, name('c'));  // another launcher runs c
        Check(running >= 0, "mark in use");
        const int locked = ::open((cache / (name('d') + ".lock")).c_str(), O_RDWR);
        Check(locked >= 0 && ::flock(locked, LOCK_EX | LOCK_NB) == 0, "lock d");
        const auto removed = PruneRelinkCache(cache, 2, name('e'));  // e (oldest) is the one this launcher uses
        Check(removed.empty() || removed == std::vector<std::string>{}, "nothing removable when the rest are in use (got " + std::to_string(removed.size()) + ")");
        Check(fs::exists(cache / name('a')) && fs::exists(cache / name('b')), "the 2 newest stay");
        Check(fs::exists(cache / name('c')), "an entry held by another launcher stays");
        Check(fs::exists(cache / name('d')), "an entry with a live lock stays");
        Check(fs::exists(cache / name('e')), "the entry in use stays");
        ::close(locked);
        ::close(running);
        const auto second = PruneRelinkCache(cache, 2, name('e'));
        Check(second.size() == 2 && !fs::exists(cache / name('c')) && !fs::exists(cache / name('d')), "unlocked old entries go, oldest-but-in-use stays");
        Check(!fs::exists(cache / (name('c') + ".lock")), "their lock files go with them");
        Check(fs::exists(cache / name('e')) && fs::exists(cache / "stubs"), "in-use entry and stubs untouched");
        Check(PruneRelinkCache(cache, 0, name('a')).empty() && fs::exists(cache / name('e')), "keep 0 disables pruning");
    }

    std::fprintf(stdout, failures ? "%d failure(s)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
