#include "prx/libc/include/general/VabiMacros.hpp"
#include "prx/libc/include/Shutdown.hpp"
#include "prx/libSceSystemService/SystemService.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <cstring>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif
extern "C" int APS5_VABI sceSystemServiceLoadExec(const char*, const char* const*);
extern "C" int APS5_VABI __cxa_atexit_nid_postfix(void (APS5_VABI *func)(void*), void* arg, void* dsoHandle);
extern "C" void APS5_VABI _Exit_nid_postfix(int);
extern "C" void APS5_VABI catchReturnFromMain_nid_postfix(int);
using GuestQuickExitCallback = void (APS5_VABI*)();
extern "C" int APS5_VABI at_quick_exit_nid_postfix(GuestQuickExitCallback);
extern "C" void APS5_VABI quick_exit_nid_postfix(int);
namespace {
std::atomic<bool> awaiting{false};
void AwaitExit() {
    awaiting = true;
    awaiting.notify_one();
    try {
        LibcAwaitExit_nid_postfix();
    } catch (const ProcessShutdown&) {
        std::puts("Guest thread resumed during exit");
        std::fflush(stdout);
        std::_Exit(6);
    }
}
void Cleanup() {
    std::thread(AwaitExit).detach();
    awaiting.wait(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::puts("Guest shutdown completed");
    std::fflush(stdout);
}
void ExitHandlerRan() {
    std::puts("Guest exit handler ran");
    std::fflush(stdout);
    std::_Exit(5);
}
void APS5_VABI UnexpectedFinalizer(void*) { ExitHandlerRan(); }
int quickOrder = 0;
void APS5_VABI QuickSecond() { quickOrder = 1; }
void APS5_VABI QuickFirst() {
    if (quickOrder != 1) std::_Exit(1);
    std::puts("Guest quick_exit handlers completed");
    std::fflush(stdout);
}
void Require(bool value) { if (!value) std::abort(); }
void UnexpectedCleanup() { std::_Exit(3); }
#ifdef _WIN32
void WINAPI UnexpectedDetach(PVOID) { TerminateProcess(GetCurrentProcess(), 4); }
#endif
constexpr char RelaunchMarker[] = "ANYPS5_TEST_RELAUNCH_MARKER";
constexpr char Relaunched[] = "ANYPS5_TEST_RELAUNCHED";
void SetVariable(const char* name, const char* value) {
#ifdef _WIN32
    Require(SetEnvironmentVariableA(name, value) != 0);
    Require(_putenv_s(name, value) == 0);
#else
    Require(setenv(name, value, 1) == 0);
#endif
}
void AppendMarker(const char* line) {
    const char* marker = std::getenv(RelaunchMarker);
    Require(marker != nullptr);
    std::ofstream(marker, std::ios::app) << line << '\n';
}
void RelaunchedShutdown() { AppendMarker("shutdown"); }
int RunChild(const char* self, const char* mode) {
#ifdef _WIN32
    (void)self;
    std::wstring executable(32768, L'\0');
    executable.resize(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
    std::wstring commandLine = L"\"" + executable + L"\" " + std::wstring(mode, mode + std::strlen(mode));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process) != 0);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    const pid_t child = fork();
    Require(child >= 0);
    if (child == 0) {
        execl(self, self, mode, static_cast<char*>(nullptr));
        std::_Exit(127);
    }
    int status = 0;
    Require(waitpid(child, &status, 0) == child);
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}
int DriveRelaunch(const char* self) {
    const auto marker = std::filesystem::temp_directory_path() / ("anyps5-relaunch-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    SetVariable(RelaunchMarker, marker.string().c_str());
    Require(RunChild(self, "--relaunch") == 0);
    std::string text;
    for (int attempt = 0; attempt < 200 && text.find("relaunched") == std::string::npos; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::ifstream file(marker);
        text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    std::error_code ignored;
    std::filesystem::remove(marker, ignored);
    Require(text == "shutdown\nrelaunched\n");
    std::puts("Guest relaunch completed");
    return 0;
}
}
int main(int argc, char** argv) {
    const int firstArgument = argc > 0 && std::strncmp(argv[0], "--", 2) == 0 ? 0 : 1;
    if (argc > firstArgument && std::strcmp(argv[firstArgument], "--requested-exit-driver") == 0) {
        Require(RunChild(argv[0], "--requested-exit") == 0);
        return 0;
    }
    if (argc > firstArgument && std::strcmp(argv[firstArgument], "--immediate") == 0) {
        LibcRegisterShutdown_nid_postfix(UnexpectedCleanup);
        Require(std::atexit(UnexpectedCleanup) == 0);
#ifdef _WIN32
        auto slot = FlsAlloc(UnexpectedDetach);
        Require(slot != FLS_OUT_OF_INDEXES && FlsSetValue(slot, &slot));
#endif
        _Exit_nid_postfix(0);
        return 2;
    }
    if (argc > 1 && std::strcmp(argv[1], "--return-from-main") == 0) {
        catchReturnFromMain_nid_postfix(0);
        return 2;
    }
    if (argc > 1 && std::strcmp(argv[1], "--quick-exit") == 0) {
        Require(at_quick_exit_nid_postfix(QuickFirst) == 0);
        Require(at_quick_exit_nid_postfix(QuickSecond) == 0);
        quick_exit_nid_postfix(0);
        return 2;
    }
    if (argc > 1 && std::strcmp(argv[1], "--relaunch-driver") == 0) {
        return DriveRelaunch(argv[0]);
    }
    if (argc > 1 && std::strcmp(argv[1], "--relaunch") == 0) {
        if (std::getenv(Relaunched) != nullptr) {
            AppendMarker("relaunched");
            return 0;
        }
        LibcRegisterShutdown_nid_postfix(RelaunchedShutdown);
        SetVariable(Relaunched, "1");
        sceSystemServiceLoadExec("/app0/eboot.bin", nullptr);
        return 2;
    }
    if (argc > 1) {
        LibcRegisterShutdown_nid_postfix(Cleanup);
        Require(__cxa_atexit_nid_postfix(UnexpectedFinalizer, nullptr, nullptr) == 0);
        Require(std::atexit(ExitHandlerRan) == 0);
        if (std::strcmp(argv[firstArgument], "--requested-exit") == 0) {
            LibcRequestExit_nid_postfix(0);
            LibcRequestExit_nid_postfix(7);
            LibcAwaitExit_nid_postfix();
        }
        sceSystemServiceLoadExec("exit", nullptr);
        return 2;
    }
    Require(sceSystemServiceLoadExec(nullptr, nullptr) == SYSTEM_SERVICE_ERROR_PARAMETER);
    Require(sceSystemServiceLoadExec("", nullptr) == SYSTEM_SERVICE_ERROR_PARAMETER);
    bool rejected = false;
    try { sceSystemServiceLoadExec("/app0/another.bin", nullptr); }
    catch (const std::runtime_error&) { rejected = true; }
    Require(rejected);
    rejected = false;
    const char* const arguments[] = {"--mode", nullptr};
    try { sceSystemServiceLoadExec("/app0/eboot.bin", arguments); }
    catch (const std::runtime_error&) { rejected = true; }
    Require(rejected);
}
