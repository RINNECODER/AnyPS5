// anyps5_rosetta_run: the engine executable of the Rosetta engine package
// (docs/RELEASES.md, docs/design/rosetta-direct-execution.md). Fork-only.
//
//   anyps5_rosetta_run [--resource-root DIR] [--diagnostics-json] [--sce-module PRX]... <eboot.bin> [guest args]
//   anyps5_rosetta_run --capabilities-json
//
// The MacPS launch contract: unself the eboot and the guest modules, relink them
// with `relinker --macos --to-rosetta` into ~/Library/Caches/AnyPS5/rosetta/<eboot
// sha256>/ (reused while the inputs and the relinker are unchanged; built in a
// .partial directory and renamed into place), lay out libs/ (the package's x86_64
// libs plus placeholders for libraries the build does not have), app0/ (the
// resource root, linked in read-only, beside the relinked sce_module/*.guest.prx)
// and a stub dylib for imports no library exports, then run the relinked title
// as a child process and report its exit. The package needs no developer tools:
// the stub dylibs are written by MachO.cpp, not clang.
//
// Diagnostics (--diagnostics-json) are JSON lines on stderr with schema_version 1:
// preparing (stage, progress 0..1, elapsed_ms), error (code, message) and
// guest_exit (exit_code, signal, seconds); libkernel adds startup at the guest
// entry point. The title's own output and the Metal frame-capture events share
// the stream. Environment variables (ANYPS5_FRAME_CAPTURE_DIR, ANYPS5_METAL_*,
// ANYPS5_TRACE_*, ...) pass through to the title.
#include "MachO.hpp"
#include "Unself.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/event.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

extern char** environ;

#ifndef ANYPS5_ROSETTA_ENGINE_VERSION
#define ANYPS5_ROSETTA_ENGINE_VERSION "dev"
#endif

namespace fs = std::filesystem;
using namespace Aps5Launcher;

namespace {

constexpr const char* kBackend = "Rosetta 2 direct execution (x86_64 relinked guest)";
constexpr const char* kVersion = ANYPS5_ROSETTA_ENGINE_VERSION;
// Bump when the cache layout or the relink inputs change meaning.
constexpr int kCacheFormat = 1;
// Imports the relinker resolves against libkernel's hooks, never stubbed.
const std::set<std::string> kHooks = {
    "_Aps5StartGuest_nid_no_patch", "_Aps5GuestTlsAllocate_nid_no_patch", "_Aps5GuestTlsRegister_nid_no_patch",
    "_Aps5GuestTlsGetAddr_nid_no_patch", "___aps5_get_pending_module_args_nid_no_patch",
    "___aps5_set_module_init_result_nid_no_patch"};
const std::regex kNidSymbol("^_[A-Za-z0-9+-]{11}(#|$)");
// C++ vtables, typeinfo and VTTs are data: a tracing stub in their place breaks the guest.
const std::regex kDataName("^_ZT[VIST]");

struct LaunchError : std::runtime_error {
    LaunchError(std::string code, const std::string& message, int exitCode)
        : std::runtime_error(message), code(std::move(code)), exitCode(exitCode) {}
    std::string code;
    int exitCode;
};
LaunchError InvalidArguments(const std::string& message) { return {"invalid_arguments", message, 2}; }
LaunchError InputUnavailable(const std::string& message) { return {"input_unavailable", message, 3}; }
LaunchError Unsupported(const std::string& message) { return {"unsupported_executable", message, 4}; }
LaunchError LoaderFailure(const std::string& message) { return {"loader_failure", message, 5}; }
LaunchError HostFailure(const std::string& message) { return {"host_failure", message, 6}; }

bool g_json = false;
const auto g_start = std::chrono::steady_clock::now();

long long ElapsedMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - g_start).count();
}

void WriteAll(int fd, const std::string& text) {
    const char* data = text.data();
    std::size_t left = text.size();
    while (left > 0) {
        const ssize_t written = ::write(fd, data, left);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return;
        data += written;
        left -= static_cast<std::size_t>(written);
    }
}

void Log(const char* format, ...) {
    char line[1024];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof line, format, args);
    va_end(args);
    WriteAll(STDERR_FILENO, std::string("[anyps5] ") + line + "\n");
}

std::string JsonString(const std::string& value) {
    std::string out = "\"";
    for (unsigned char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof escaped, "\\u%04x", c);
                out += escaped;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

// One diagnostics line: {"schema_version":1,"event":...,...}.
class Event {
public:
    explicit Event(const char* name) : body_("{\"schema_version\":1,\"event\":" + JsonString(name)) {}
    Event& Str(const char* key, const std::string& value) { return Raw(key, JsonString(value)); }
    Event& Int(const char* key, long long value) { return Raw(key, std::to_string(value)); }
    Event& Num(const char* key, double value) {
        char text[32];
        std::snprintf(text, sizeof text, "%.3f", value);
        return Raw(key, text);
    }
    Event& Bool(const char* key, bool value) { return Raw(key, value ? "true" : "false"); }
    Event& Null(const char* key) { return Raw(key, "null"); }
    void Emit() {
        if (g_json) WriteAll(STDERR_FILENO, body_ + "}\n");
    }

private:
    Event& Raw(const char* key, const std::string& value) {
        body_ += "," + JsonString(key) + ":" + value;
        return *this;
    }
    std::string body_;
};

std::string StageMessage(const std::string& stage) {
    if (stage == "hash") return "Checking the game";
    if (stage == "waiting") return "Waiting for another launch of this game";
    if (stage == "unself") return "Reading the game's executable and modules";
    if (stage == "relink") return "Preparing the game for its first launch (once per game and engine version)";
    if (stage == "layout") return "Setting up the game";
    return "Starting the game";
}

void Preparing(const char* stage, double progress, bool estimated = false) {
    Event("preparing")
        .Str("stage", stage)
        .Str("message", StageMessage(stage))
        .Num("progress", std::clamp(progress, 0.0, 1.0))
        .Int("elapsed_ms", ElapsedMs())
        .Bool("estimated", estimated)
        .Emit();
}

std::string Capabilities() {
    std::ostringstream out;
    out << "{\"schema_version\":1,\"host_architecture\":\"arm64\",\"guest_architecture\":\"x86_64\","
        << "\"backend\":" << JsonString(kBackend) << ",\"engine_version\":" << JsonString(kVersion) << ","
        << "\"supported_formats\":[\"sce_elf64_x86_64\"],\"supported_containers\":[\"plain_self\"],"
        << "\"runtime_abi\":\"sce_sysv\",\"runtime_abis\":[\"sce_sysv\"],"
        << "\"resource_root_argument\":\"--resource-root\",\"sce_module_argument\":\"--sce-module\","
        << "\"diagnostics_json_argument\":\"--diagnostics-json\","
        << "\"diagnostics_events\":[\"preparing\",\"startup\",\"error\",\"guest_exit\",\"frames_presented\",\"frame_captured\"],"
        << "\"relink_cache\":\"~/Library/Caches/AnyPS5/rosetta\",\"ps5_game_runtime_ready\":false}\n";
    return out.str();
}

constexpr const char* kUsage =
    "usage: anyps5_rosetta_run [options] <eboot.bin> [guest args...]\n"
    "       anyps5_rosetta_run --capabilities-json | --version\n"
    "  --resource-root DIR      the title's data (app0); default: the eboot's directory\n"
    "  --sce-module PRX         a guest module to preload (repeatable; libc.prx first)\n"
    "  --diagnostics-json       JSON diagnostics events on stderr\n"
    "  --cache DIR              relink cache root (default ~/Library/Caches/AnyPS5/rosetta,\n"
    "                           or ANYPS5_ROSETTA_CACHE)\n"
    "  --relink                 relink even when the cache is current\n"
    "  --prepare-only           relink and lay out, then exit without running the title\n"
    "  --wall SECONDS           stop the title after SECONDS\n"
    "  --abort-on-unimplemented let unimplemented library functions throw\n"
    "  --trace REGEX            log guest calls to imports whose NID or name matches REGEX\n"
    "  --libs DIR, --relinker PATH, --stub-runtime PATH\n"
    "                           development overrides of the package layout\n";

struct Options {
    fs::path eboot, resourceRoot, cache, libs, relinker, stubRuntime;
    std::vector<fs::path> modules;
    std::vector<std::string> guestArgs;
    std::string trace;
    double wall = 0;
    bool relink = false, prepareOnly = false, abortOnUnimplemented = false;
};

Options ParseArguments(int argc, char** argv) {
    Options options;
    auto value = [&](int& index, const char* name) -> std::string {
        if (index + 1 >= argc) throw InvalidArguments(std::string(name) + " needs a value");
        return argv[++index];
    };
    int index = 1;
    for (; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--") {
            ++index;
            break;
        }
        if (argument.rfind("--", 0) != 0 || !options.eboot.empty()) {
            if (!options.eboot.empty()) break;  // guest arguments follow the eboot
            options.eboot = argument;
            continue;
        }
        if (argument == "--diagnostics-json") g_json = true;
        else if (argument == "--resource-root") options.resourceRoot = value(index, "--resource-root");
        else if (argument == "--sce-module") options.modules.emplace_back(value(index, "--sce-module"));
        else if (argument == "--cache") options.cache = value(index, "--cache");
        else if (argument == "--libs") options.libs = value(index, "--libs");
        else if (argument == "--relinker") options.relinker = value(index, "--relinker");
        else if (argument == "--stub-runtime") options.stubRuntime = value(index, "--stub-runtime");
        else if (argument == "--trace") options.trace = value(index, "--trace");
        else if (argument == "--relink") options.relink = true;
        else if (argument == "--prepare-only") options.prepareOnly = true;
        else if (argument == "--abort-on-unimplemented") options.abortOnUnimplemented = true;
        else if (argument == "--wall") {
            const std::string text = value(index, "--wall");
            char* end = nullptr;
            options.wall = std::strtod(text.c_str(), &end);
            if (end == text.c_str() || *end != '\0' || !(options.wall > 0)) throw InvalidArguments("--wall needs a positive number of seconds");
        } else throw InvalidArguments("unknown option " + argument);
    }
    for (; index < argc; ++index) {
        if (options.eboot.empty()) options.eboot = argv[index];
        else options.guestArgs.emplace_back(argv[index]);
    }
    if (options.eboot.empty()) throw InvalidArguments("no eboot.bin given");
    return options;
}

fs::path ExecutableDirectory() {
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) throw HostFailure("cannot locate the engine executable");
    path.resize(std::strlen(path.c_str()));
    std::error_code error;
    const fs::path resolved = fs::canonical(path, error);
    return (error ? fs::path(path) : resolved).parent_path();
}

std::string HexDigest(const unsigned char* digest, std::size_t size) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < size; ++i) out += hex[digest[i] >> 4], out += hex[digest[i] & 15];
    return out;
}

std::string Sha256File(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw InputUnavailable("cannot read " + path.filename().string());
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    std::vector<char> block(1 << 20);
    while (stream) {
        stream.read(block.data(), static_cast<std::streamsize>(block.size()));
        if (stream.gcount() > 0) CC_SHA256_Update(&context, block.data(), static_cast<CC_LONG>(stream.gcount()));
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &context);
    return HexDigest(digest, sizeof digest);
}

std::vector<std::uint8_t> ReadFile(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) throw InputUnavailable("cannot read " + path.filename().string());
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

// Writes `bytes` to `path` through a sibling temporary file and a rename.
void WriteAtomically(const fs::path& path, const void* bytes, std::size_t size, mode_t mode) {
    const fs::path temporary = path.string() + ".tmp-" + std::to_string(::getpid());
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(size));
        if (!stream) throw HostFailure("cannot write " + path.string());
    }
    ::chmod(temporary.c_str(), mode);
    fs::rename(temporary, path);
}

fs::path CacheRoot(const Options& options) {
    if (!options.cache.empty()) return options.cache;
    if (const char* configured = std::getenv("ANYPS5_ROSETTA_CACHE"); configured != nullptr && *configured != '\0') return configured;
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        const passwd* user = ::getpwuid(::getuid());
        home = user != nullptr ? user->pw_dir : nullptr;
    }
    if (home == nullptr) throw HostFailure("no home directory for the relink cache");
    return fs::path(home) / "Library" / "Caches" / "AnyPS5" / "rosetta";
}

std::vector<fs::path> SortedEntries(const fs::path& directory) {
    std::vector<fs::path> entries;
    std::error_code error;
    for (fs::directory_iterator it(directory, error), end; !error && it != end; it.increment(error)) entries.push_back(it->path());
    std::sort(entries.begin(), entries.end());
    return entries;
}

bool HasExtension(const fs::path& path, const char* extension) { return path.extension() == extension; }

struct GuestModules {
    std::map<std::string, fs::path> preload, deferred;
};

// Preloaded: --sce-module paths, then <title>/sce_module, <resource>/sce_module and,
// for a title directory apart from the resource root, <title>/*.prx. Deferred (for
// sceKernelLoadStartModule): the resource root's top-level *.prx.
GuestModules FindModules(const Options& options, const fs::path& title, const fs::path& resource) {
    GuestModules modules;
    std::error_code error;
    for (const auto& module : options.modules) {
        if (!fs::is_regular_file(module, error)) throw InputUnavailable("guest module unavailable: " + module.filename().string());
        modules.preload.emplace(module.filename().string(), fs::absolute(module));
    }
    for (const auto& directory : {title / "sce_module", resource / "sce_module"})
        for (const auto& path : SortedEntries(directory))
            if (HasExtension(path, ".prx") && fs::is_regular_file(path, error)) modules.preload.emplace(path.filename().string(), path);
    if (!fs::equivalent(title, resource, error))
        for (const auto& path : SortedEntries(title))
            if (HasExtension(path, ".prx") && fs::is_regular_file(path, error)) modules.preload.emplace(path.filename().string(), path);
    for (const auto& path : SortedEntries(resource))
        if (HasExtension(path, ".prx") && fs::is_regular_file(path, error) && !modules.preload.count(path.filename().string()))
            modules.deferred.emplace(path.filename().string(), path);
    return modules;
}

// ---- child processes -----------------------------------------------------------------

struct ChildResult {
    int status = 0;
    bool stopped = false;   // a stop signal reached the launcher
    bool timedOut = false;  // the wall limit expired
};

const int kStopSignals[] = {SIGTERM, SIGINT, SIGHUP};

void IgnoreStopSignals() {
    for (int signal : kStopSignals) ::signal(signal, SIG_IGN);
}

pid_t Spawn(const std::vector<std::string>& argv, const std::vector<std::string>& environment, const fs::path& output) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (!output.empty()) {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, output.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    }
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sigset_t none, defaults;
    sigemptyset(&none);
    sigemptyset(&defaults);
    for (int signal : kStopSignals) sigaddset(&defaults, signal);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigmask(&attributes, &none);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_CLOEXEC_DEFAULT);
    // CLOEXEC_DEFAULT closes everything else; keep the standard streams.
    for (int fd = 0; fd <= 2; ++fd)
        if (output.empty() || fd == STDIN_FILENO) posix_spawn_file_actions_addinherit_np(&actions, fd);
    std::vector<char*> args, env;
    for (const auto& item : argv) args.push_back(const_cast<char*>(item.c_str()));
    args.push_back(nullptr);
    for (const auto& item : environment) env.push_back(const_cast<char*>(item.c_str()));
    env.push_back(nullptr);
    pid_t pid = 0;
    const int result = posix_spawn(&pid, argv[0].c_str(), &actions, &attributes, args.data(), env.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (result != 0) throw HostFailure("cannot start " + fs::path(argv[0]).filename().string() + ": " + std::strerror(result));
    return pid;
}

// Waits for `pid`, calling `tick` every `tickMs`. A stop signal to the launcher or
// the wall limit sends SIGTERM to the child, then SIGKILL 1.5 s later (MacPS
// escalates to SIGKILL on the launcher after 2 s).
template <typename Tick>
ChildResult Wait(pid_t pid, int tickMs, double wallSeconds, Tick tick) {
    ChildResult result;
    const int queue = ::kqueue();
    if (queue < 0) throw HostFailure("kqueue failed");
    std::vector<struct kevent> changes;
    for (int signal : kStopSignals) {
        changes.emplace_back();
        EV_SET(&changes.back(), signal, EVFILT_SIGNAL, EV_ADD, 0, 0, nullptr);
    }
    if (tickMs > 0) {
        changes.emplace_back();
        EV_SET(&changes.back(), 1, EVFILT_TIMER, EV_ADD, 0, tickMs, nullptr);
    }
    if (wallSeconds > 0) {
        changes.emplace_back();
        EV_SET(&changes.back(), 2, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, static_cast<intptr_t>(wallSeconds * 1000), nullptr);
    }
    // Last: a child that already exited fails this change (ESRCH), and waitpid covers it.
    changes.emplace_back();
    EV_SET(&changes.back(), pid, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, nullptr);
    ::kevent(queue, changes.data(), static_cast<int>(changes.size()), nullptr, 0, nullptr);
    auto stop = [&] {
        ::kill(pid, SIGTERM);
        struct kevent escalate;
        EV_SET(&escalate, 3, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 1500, nullptr);
        ::kevent(queue, &escalate, 1, nullptr, 0, nullptr);
    };
    while (true) {
        int status = 0;
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            result.status = status;
            break;
        }
        if (done < 0 && errno != EINTR) throw HostFailure("waitpid failed");
        struct kevent event;
        const int count = ::kevent(queue, nullptr, 0, &event, 1, nullptr);
        if (count <= 0) continue;
        if (event.filter == EVFILT_SIGNAL && !result.stopped && !result.timedOut) {
            result.stopped = true;
            stop();
        } else if (event.filter == EVFILT_TIMER && event.ident == 1) {
            tick();
        } else if (event.filter == EVFILT_TIMER && event.ident == 2 && !result.stopped) {
            result.timedOut = true;
            stop();
        } else if (event.filter == EVFILT_TIMER && event.ident == 3) {
            ::kill(pid, SIGKILL);
        }
    }
    ::close(queue);
    return result;
}

std::string SignalName(int signal) {
    if (signal <= 0 || signal >= NSIG) return std::to_string(signal);
    std::string name = std::string("SIG") + sys_signame[signal];
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return name;
}

int ExitCode(int status) {
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 1;
}

// A forked watchdog that kills the title if the launcher dies first (MacPS's
// SIGKILL escalation), so a title never outlives its session.
pid_t StartWatchdog(pid_t title) {
    const pid_t launcher = ::getpid();
    const pid_t watchdog = ::fork();
    if (watchdog != 0) return watchdog;
    const int null = ::open("/dev/null", O_RDWR);
    for (int fd = 0; fd <= 2; ++fd) ::dup2(null, fd);
    for (int fd = 3; fd < 256; ++fd) ::close(fd);
    const int queue = ::kqueue();
    struct kevent changes[2];
    EV_SET(&changes[0], launcher, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, nullptr);
    EV_SET(&changes[1], title, EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, nullptr);
    if (queue < 0 || ::kevent(queue, changes, 2, nullptr, 0, nullptr) < 0 || ::getppid() != launcher) {
        if (::getppid() != launcher) ::kill(title, SIGKILL);
        ::_exit(0);
    }
    while (true) {
        struct kevent event;
        if (::kevent(queue, nullptr, 0, &event, 1, nullptr) <= 0) continue;
        if (static_cast<pid_t>(event.ident) == launcher) ::kill(title, SIGKILL);
        ::_exit(0);
    }
}

std::vector<std::string> BaseEnvironment() {
    std::vector<std::string> environment;
    for (char** entry = environ; *entry != nullptr; ++entry)
        if (std::strncmp(*entry, "DYLD_", 5) != 0) environment.emplace_back(*entry);
    return environment;
}

void SetEnvironment(std::vector<std::string>& environment, const std::string& key, const std::string& value, bool override) {
    const std::string prefix = key + "=";
    for (auto& entry : environment)
        if (entry.rfind(prefix, 0) == 0) {
            if (override) entry = prefix + value;
            return;
        }
    environment.push_back(prefix + value);
}

// ---- relink ---------------------------------------------------------------------------

struct Paths {
    fs::path libs, relinker, stubRuntime;
};

Paths PackagePaths(const Options& options) {
    const fs::path root = ExecutableDirectory().parent_path();
    Paths paths{options.libs.empty() ? root / "libs" : options.libs, options.relinker.empty() ? root / "bin" / "relinker" : options.relinker,
                options.stubRuntime.empty() ? root / "share" / "aps5_stub_runtime.dylib" : options.stubRuntime};
    std::error_code error;
    if (!fs::is_directory(paths.libs, error) || !fs::is_regular_file(paths.relinker, error) || !fs::is_regular_file(paths.stubRuntime, error))
        throw HostFailure("the engine package is incomplete (libs/, bin/relinker or share/aps5_stub_runtime.dylib is missing)");
    paths.libs = fs::canonical(paths.libs);
    paths.relinker = fs::canonical(paths.relinker);
    paths.stubRuntime = fs::canonical(paths.stubRuntime);
    return paths;
}

std::string Stamp(const std::string& sha, const Paths& paths, const GuestModules& modules) {
    std::ostringstream stamp;
    struct stat info {};
    ::stat(paths.relinker.c_str(), &info);
    stamp << "format " << kCacheFormat << "\neboot " << sha << "\nrelinker " << paths.relinker.string() << " " << info.st_size << " "
          << info.st_mtimespec.tv_sec << "." << info.st_mtimespec.tv_nsec << "\n";
    std::error_code error;
    for (const auto& [name, path] : modules.preload) stamp << "preload " << name << " " << fs::file_size(path, error) << "\n";
    for (const auto& [name, path] : modules.deferred) stamp << "deferred " << name << " " << fs::file_size(path, error) << "\n";
    return stamp.str();
}

std::string ReadText(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream text;
    text << stream.rdbuf();
    return text.str();
}

std::string Tail(const fs::path& path, std::size_t lines) {
    const std::string text = ReadText(path);
    std::size_t start = text.size(), seen = 0;
    while (start > 0 && seen <= lines)
        if (text[--start] == '\n') ++seen;
    return text.substr(start);
}

void UnselfTo(const fs::path& input, const fs::path& output) {
    std::vector<std::uint8_t> elf;
    try {
        elf = Unself(ReadFile(input));
    } catch (const LaunchError&) {
        throw;
    } catch (const std::exception& error) {
        throw Unsupported(input.filename().string() + ": " + error.what());
    }
    WriteAtomically(output, elf.data(), elf.size(), 0644);
}

// Relinks into <cache>/<sha>/out unless the stamp there matches; returns the out directory.
fs::path Relink(const Options& options, const Paths& paths, const fs::path& cache, const std::string& sha, const fs::path& eboot,
                const GuestModules& modules) {
    const fs::path final = cache / sha;
    const std::string stamp = Stamp(sha, paths, modules);
    std::error_code error;
    if (!options.relink && fs::is_regular_file(final / "out" / "eboot", error) && ReadText(final / "relink.stamp") == stamp) {
        Log("relink cached");
        return final / "out";
    }
    // Under the lock: leftovers of a launcher that was killed mid-relink go first.
    for (const auto& entry : SortedEntries(cache)) {
        const std::string name = entry.filename().string();
        if (name.rfind(sha + ".partial-", 0) == 0 || name.rfind(sha + ".old-", 0) == 0) fs::remove_all(entry, error);
    }
    const fs::path work = cache / (sha + ".partial-" + std::to_string(::getpid()));
    for (const auto& directory : {work / "src", work / "mods" / "sce_module", work / "mods" / "deferred", work / "out"}) fs::create_directories(directory);

    // Unself: about 10% of the preparation.
    const double total = 1.0 + static_cast<double>(modules.preload.size() + modules.deferred.size());
    double done = 0;
    std::uintmax_t inputBytes = fs::file_size(eboot);
    Preparing("unself", 0.0);
    UnselfTo(eboot, work / "src" / "input.elf");
    Preparing("unself", 0.1 * (++done / total));
    for (const auto& [name, path] : modules.preload) {
        UnselfTo(path, work / "mods" / "sce_module" / name);
        inputBytes += fs::file_size(path);
        Preparing("unself", 0.1 * (++done / total));
    }
    for (const auto& [name, path] : modules.deferred) {
        UnselfTo(path, work / "mods" / "deferred" / name);
        inputBytes += fs::file_size(path);
        Preparing("unself", 0.1 * (++done / total));
    }

    std::vector<std::string> command = {paths.relinker.string(), "--macos", "--to-rosetta", "--sce-module-path", (work / "mods").string()};
    if (!modules.deferred.empty()) command.insert(command.end(), {"--module-dir", "deferred"});
    command.push_back((work / "src" / "input.elf").string());
    command.push_back((work / "out" / "eboot").string());
    Log("relinking %zu preloaded and %zu deferred modules (first launch only)", modules.preload.size(), modules.deferred.size());
    const auto started = std::chrono::steady_clock::now();
    // The relinker handles roughly 4 MB of input per second under Rosetta; the
    // estimate only paces the progress events and never exceeds 0.95.
    const double expected = std::max(5.0, static_cast<double>(inputBytes) / (4.0 * 1024 * 1024));
    const pid_t relinker = Spawn(command, BaseEnvironment(), work / "relink.log");
    const ChildResult result = Wait(relinker, 1000, 0, [&] {
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        Preparing("relink", 0.1 + 0.85 * (1.0 - std::exp(-seconds / expected)), true);
    });
    if (result.stopped) {
        fs::remove_all(work, error);
        throw LaunchError("stopped", "stopped while relinking", 128 + SIGTERM);
    }
    if (ExitCode(result.status) != 0 || !fs::is_regular_file(work / "out" / "eboot", error)) {
        const fs::path log = cache / (sha + ".relink-failed.log");
        fs::rename(work / "relink.log", log, error);
        Log("relinker output (last lines):\n%s", Tail(log, 20).c_str());
        fs::remove_all(work, error);
        throw LoaderFailure("the relinker failed (exit " + std::to_string(ExitCode(result.status)) + "); log: " + log.string());
    }
    ::chmod((work / "out" / "eboot").c_str(), 0755);
    fs::remove_all(work / "src", error);
    fs::remove_all(work / "mods", error);
    WriteAtomically(work / "relink.stamp", stamp.data(), stamp.size(), 0644);
    // Keep what the title wrote beside the eboot (save data, download0, temp0).
    if (fs::is_directory(final / "out", error))
        for (const auto& entry : SortedEntries(final / "out")) {
            const std::string name = entry.filename().string();
            if (name != "eboot" && name != "app0" && name != "libs") fs::rename(entry, work / "out" / name, error);
        }
    const fs::path old = cache / (sha + ".old-" + std::to_string(::getpid()));
    if (fs::exists(final, error)) fs::rename(final, old);
    fs::rename(work, final);
    fs::remove_all(old, error);
    Log("relinked in %.1fs", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    return final / "out";
}

// ---- layout ---------------------------------------------------------------------------

std::map<std::string, std::string> NidNames() {
    std::map<std::string, std::string> names;
    const char* configured = std::getenv("ANYPS5_NID_DB");
    const fs::path path = configured != nullptr && *configured != '\0' ? fs::path(configured) : fs::temp_directory_path() / "anyps5-nid-db" / "aerolib.csv";
    std::ifstream stream(path);
    std::string nid, name;
    while (stream >> nid >> name) {
        names.emplace(nid, name);
        stream.ignore(1 << 16, '\n');
    }
    return names;
}

std::string NidOf(const std::string& symbol) { return symbol.substr(1, symbol.find('#') == std::string::npos ? std::string::npos : symbol.find('#') - 1); }

std::string NidLabel(const std::string& symbol, const std::map<std::string, std::string>& names) {
    const std::string nid = NidOf(symbol);
    const auto found = names.find(nid);
    return found == names.end() ? nid : nid + " (" + found->second + ")";
}

void CollectGuestImages(const fs::path& directory, std::vector<fs::path>& images) {
    std::error_code error;
    for (fs::recursive_directory_iterator it(directory, fs::directory_options::none, error), end; !error && it != end; it.increment(error)) {
        if (it->is_symlink(error)) {
            it.disable_recursion_pending();
            continue;
        }
        const std::string name = it->path().filename().string();
        if (it->is_regular_file(error) && name.size() > 10 && name.compare(name.size() - 10, 10, ".guest.prx") == 0) images.push_back(it->path());
    }
    std::sort(images.begin(), images.end());
}

struct Layout {
    std::size_t images = 0, imports = 0;
    std::vector<std::string> missingLibs, unresolved;
    fs::path stubLibrary;
};

Layout LayOut(const Options& options, const Paths& paths, const fs::path& cache, const fs::path& out, const fs::path& resource) {
    Layout layout;
    std::vector<fs::path> images = {out / "eboot"};
    CollectGuestImages(out / "app0", images);
    std::set<std::string> imported, rpathLibs, available;
    for (const auto& image : images) {
        MachOImage parsed = ReadMachO(image.string());
        imported.insert(parsed.binds.begin(), parsed.binds.end());
        for (const auto& dylib : parsed.dylibs)
            if (dylib.rfind("@rpath/", 0) == 0) rpathLibs.insert(dylib.substr(7));
        available.merge(parsed.exports);
    }
    layout.images = images.size();
    layout.imports = imported.size();

    // libs/: links to the package's libraries (and AnyPS5Utilities.metallib beside them).
    const fs::path libs = out / "libs";
    std::error_code error;
    fs::remove_all(libs, error);
    fs::create_directories(libs);
    for (const auto& library : SortedEntries(paths.libs)) {
        if (!fs::is_regular_file(library, error)) continue;
        fs::create_symlink(library, libs / library.filename());
        if (rpathLibs.count(library.filename().string())) available.merge(ReadMachO(library.string()).exports);
    }
    for (const auto& name : rpathLibs)
        if (!fs::exists(libs / name, error)) {
            layout.missingLibs.push_back(name);
            StubDylib placeholder;
            placeholder.installName = "@rpath/" + name;
            const auto bytes = BuildStubDylib(placeholder);
            WriteAtomically(libs / name, bytes.data(), bytes.size(), 0755);
        }

    // Only NIDs: other names (relinker hooks, ___cxa_atexit) come from libkernel or libSystem.
    for (const auto& name : imported)
        if (!available.count(name) && !kHooks.count(name) && std::regex_search(name, kNidSymbol)) layout.unresolved.push_back(name);
    const auto names = NidNames();
    std::vector<std::string> traced;
    if (!options.trace.empty()) {
        std::regex pattern;
        try {
            pattern = std::regex(options.trace);
        } catch (const std::regex_error&) {
            throw InvalidArguments("--trace is not a valid regular expression");
        }
        const std::set<std::string> unresolved(layout.unresolved.begin(), layout.unresolved.end());
        for (const auto& symbol : imported) {
            if (unresolved.count(symbol) || kHooks.count(symbol) || !std::regex_search(symbol, kNidSymbol)) continue;
            const std::string nid = NidOf(symbol);
            const auto found = names.find(nid);
            const std::string name = found == names.end() ? "" : found->second;
            if (std::regex_search(name, kDataName)) continue;
            if (std::regex_search(nid, pattern) || std::regex_search(name, pattern)) traced.push_back(symbol);
        }
        Log("tracing %zu imports matching %s", traced.size(), options.trace.c_str());
    }
    if (!layout.unresolved.empty() || !traced.empty()) {
        StubDylib stubs;
        stubs.installName = (cache / "stubs" / "aps5_unresolved.dylib").string();
        stubs.runtimePath = paths.stubRuntime.string();
        stubs.stubs = layout.unresolved;
        for (const auto& symbol : stubs.stubs) stubs.stubLabels.push_back(NidLabel(symbol, names));
        stubs.traced = traced;
        for (const auto& symbol : traced) stubs.tracedLabels.push_back(NidLabel(symbol, names));
        fs::create_directories(cache / "stubs");
        const auto bytes = BuildStubDylib(stubs);
        WriteAtomically(stubs.installName, bytes.data(), bytes.size(), 0755);
        layout.stubLibrary = stubs.installName;
    }

    // app0: the resource root linked in read-only, plus the relinker outputs.
    const fs::path app0 = out / "app0";
    fs::create_directories(app0);
    std::vector<fs::path> links;
    for (fs::recursive_directory_iterator it(app0, fs::directory_options::none, error), end; !error && it != end; it.increment(error))
        if (it->is_symlink(error)) {
            links.push_back(it->path());
            it.disable_recursion_pending();
        }
    for (const auto& link : links) fs::remove(link, error);
    for (const auto& entry : SortedEntries(resource)) {
        const fs::path target = app0 / entry.filename();
        if (entry.filename() == "sce_module" && fs::is_directory(entry, error)) {
            fs::create_directories(target);
            for (const auto& module : SortedEntries(entry))
                if (!fs::exists(fs::symlink_status(target / module.filename(), error))) fs::create_symlink(module, target / module.filename());
        } else if (!fs::exists(fs::symlink_status(target, error))) {
            fs::create_symlink(entry, target);
        }
    }
    for (const auto& module : SortedEntries(app0 / "deferred"))
        if (module.string().size() > 10 && module.string().compare(module.string().size() - 10, 10, ".guest.prx") == 0 &&
            !fs::exists(fs::symlink_status(app0 / module.filename(), error)))
            fs::create_symlink(fs::path("deferred") / module.filename(), app0 / module.filename());

    std::ostringstream summary;
    summary << "{\"images\":" << layout.images << ",\"imports\":" << layout.imports << ",\"rpath_libs\":" << rpathLibs.size()
            << ",\"missing_libs\":[";
    for (std::size_t i = 0; i < layout.missingLibs.size(); ++i) summary << (i ? "," : "") << JsonString(layout.missingLibs[i]);
    summary << "],\"unresolved\":" << layout.unresolved.size() << ",\"traced\":" << traced.size()
            << ",\"stub_library\":" << (layout.stubLibrary.empty() ? "null" : JsonString(layout.stubLibrary.string())) << "}\n";
    const std::string text = summary.str();
    WriteAtomically(cache / "layout.json", text.data(), text.size(), 0644);
    return layout;
}

int Launch(const Options& options) {
    std::error_code error;
    const fs::path eboot = fs::absolute(options.eboot).lexically_normal();
    if (!fs::is_regular_file(eboot, error)) throw InputUnavailable("the executable is unavailable");
    const fs::path title = eboot.parent_path();
    const fs::path resource = options.resourceRoot.empty() ? title : fs::absolute(options.resourceRoot).lexically_normal();
    if (!fs::is_directory(resource, error)) throw InputUnavailable("the resource root is unavailable");
    const Paths paths = PackagePaths(options);
    const GuestModules modules = FindModules(options, title, resource);

    const fs::path cacheRoot = CacheRoot(options);
    fs::create_directories(cacheRoot, error);
    if (!fs::is_directory(cacheRoot, error)) throw HostFailure("cannot create the relink cache " + cacheRoot.string());
    Preparing("hash", 0.0);
    const std::string sha = Sha256File(eboot);
    // One preparation per title at a time; the lock is released before the title runs.
    const int lock = ::open((cacheRoot / (sha + ".lock")).c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (lock < 0) throw HostFailure("cannot lock the relink cache");
    if (::flock(lock, LOCK_EX | LOCK_NB) != 0) {
        Preparing("waiting", 0.0);
        ::flock(lock, LOCK_EX);
    }
    fs::path out;
    Layout layout;
    try {
        out = Relink(options, paths, cacheRoot, sha, eboot, modules);
        Preparing("layout", 0.95);
        layout = LayOut(options, paths, cacheRoot / sha, out, resource);
    } catch (const fs::filesystem_error& failure) {
        ::close(lock);
        throw HostFailure(std::string("relink cache: ") + failure.what());
    } catch (...) {
        ::close(lock);
        throw;
    }
    ::close(lock);
    Log("%zu images, %zu imported symbols, %zu libraries stubbed, %zu imports stubbed", layout.images, layout.imports,
        layout.missingLibs.size(), layout.unresolved.size());
    Preparing("ready", 1.0);
    if (options.prepareOnly) return 0;

    std::vector<std::string> environment = BaseEnvironment();
    if (!options.abortOnUnimplemented) SetEnvironment(environment, "ANYPS5_UNIMPLEMENTED", "continue", false);
    if (g_json) SetEnvironment(environment, "ANYPS5_DIAGNOSTICS_JSON", "1", true);
    if (!layout.stubLibrary.empty()) SetEnvironment(environment, "DYLD_INSERT_LIBRARIES", layout.stubLibrary.string(), true);
    std::vector<std::string> argv = {(out / "eboot").string()};
    argv.insert(argv.end(), options.guestArgs.begin(), options.guestArgs.end());
    const auto started = std::chrono::steady_clock::now();
    // The title runs in its out/ directory (app0/ and libs/ beside the eboot).
    if (::chdir(out.c_str()) != 0) throw HostFailure("cannot enter the relink cache");
    const pid_t child = Spawn(argv, environment, fs::path());
    const pid_t watchdog = StartWatchdog(child);
    const ChildResult result = Wait(child, 0, options.wall, [] {});
    if (watchdog > 0) {
        ::kill(watchdog, SIGKILL);
        ::waitpid(watchdog, nullptr, 0);
    }
    const int code = ExitCode(result.status);
    Event exit("guest_exit");
    exit.Int("exit_code", code);
    if (WIFSIGNALED(result.status)) exit.Str("signal", SignalName(WTERMSIG(result.status)));
    else exit.Null("signal");
    exit.Num("seconds", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count())
        .Bool("stopped", result.stopped)
        .Bool("timed_out", result.timedOut)
        .Emit();
    Log("title exited with %d", code);
    return code;
}

}  // namespace

int main(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--capabilities-json") == 0) {
            WriteAll(STDOUT_FILENO, Capabilities());
            return 0;
        }
        if (std::strcmp(argv[index], "--version") == 0) {
            WriteAll(STDOUT_FILENO, std::string(kVersion) + "\n");
            return 0;
        }
        if (std::strcmp(argv[index], "--help") == 0 || std::strcmp(argv[index], "-h") == 0) {
            WriteAll(STDOUT_FILENO, kUsage);
            return 0;
        }
        if (std::strcmp(argv[index], "--") == 0) break;
    }
    IgnoreStopSignals();
    try {
        return Launch(ParseArguments(argc, argv));
    } catch (const LaunchError& failure) {
        Event("error").Str("code", failure.code).Str("message", failure.what()).Emit();
        Log("error: %s", failure.what());
        if (failure.code == "invalid_arguments") WriteAll(STDERR_FILENO, kUsage);
        return failure.exitCode;
    } catch (const std::exception& failure) {
        Event("error").Str("code", "host_failure").Str("message", failure.what()).Emit();
        Log("error: %s", failure.what());
        return 6;
    }
}
