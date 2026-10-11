#include "prx/libc/include/General.hpp"
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
extern "C" {
int APS5_VABI execvp_nid_postfix(const char*, char* const*);
int APS5_VABI execv_nid_postfix(const char*, char* const*);
int APS5_VABI execve_nid_postfix(const char*, char* const*, char* const*);
int* APS5_VABI __error_nid_postfix();
}
static constexpr int GUEST_ENOENT = 2;
static constexpr int GUEST_EACCES = 13;
static constexpr int GUEST_EFAULT = 14;
static constexpr int GUEST_ENOTDIR = 20;
static void Require(bool value) { if (!value) std::abort(); }
static bool ReportsNotImplemented(const char* file, char* const* arguments) {
    try {
        execvp_nid_postfix(file, arguments);
    } catch (const std::runtime_error& error) {
        return std::string(error.what()) == "execvp: executable replacement not implemented";
    }
    return false;
}
template <typename TCall> static bool Throws(TCall call, const std::string& message) {
    try {
        call();
    } catch (const std::runtime_error& error) {
        return error.what() == message;
    }
    return false;
}
template <typename TCall> static bool Fails(TCall call, int error) {
    *__error_nid_postfix() = 0;
    return call() == -1 && *__error_nid_postfix() == error;
}
static void RequireBoth(const char* path, char* const* arguments, int error) {
    Require(Fails([&] { return execv_nid_postfix(path, arguments); }, error));
    Require(Fails([&] { return execve_nid_postfix(path, arguments, nullptr); }, error));
}
int main() {
    char shell[] = "sh";
    char option[] = "-c";
    char command[] = "exit 0";
    char* arguments[] = {shell, option, command, nullptr};
    Require(ReportsNotImplemented("sh", arguments));
    Require(ReportsNotImplemented("/bin/sh", arguments));
    Require(ReportsNotImplemented(nullptr, nullptr));

    const auto name = "anyps5-exec-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto directory = std::filesystem::temp_directory_path() / name;
    Require(std::filesystem::create_directory(directory));
    { std::ofstream output(directory / "program"); output << "program"; }
    AddPathAlias_nid_no_patch("/exec-alias", directory.string().c_str());
    char variable[] = "HOME=/";
    char* environment[] = {variable, nullptr};

    RequireBoth(nullptr, arguments, GUEST_EFAULT);
    RequireBoth("/exec-alias/program", nullptr, GUEST_EFAULT);
    RequireBoth(nullptr, nullptr, GUEST_EFAULT);
    RequireBoth("/exec-alias/missing", arguments, GUEST_ENOENT);
    RequireBoth("/exec-alias/missing/program", arguments, GUEST_ENOENT);
    RequireBoth("", arguments, GUEST_ENOENT);
    RequireBoth("/exec-alias/program/", arguments, GUEST_ENOTDIR);
    RequireBoth("/exec-alias", arguments, GUEST_EACCES);
    RequireBoth("/exec-alias/", arguments, GUEST_EACCES);

    Require(Throws([&] { execv_nid_postfix("/exec-alias/program", arguments); }, "execv: executable replacement not implemented"));
    Require(Throws([&] { execve_nid_postfix("/exec-alias/program", arguments, environment); }, "execve: executable replacement not implemented"));
    Require(Throws([&] { execve_nid_postfix("/exec-alias/program", arguments, nullptr); }, "execve: executable replacement not implemented"));

    std::string fittingPath = "/exec-alias/";
    while (fittingPath.size() < 1016) fittingPath += "./";
    fittingPath += "missing";
    Require(fittingPath.size() == 1023);
    RequireBoth(fittingPath.c_str(), arguments, GUEST_ENOENT);
    const std::string longPath = fittingPath + "s";
    Require(Throws([&] { execv_nid_postfix(longPath.c_str(), arguments); }, "execv: path of PATH_MAX bytes or more"));
    Require(Throws([&] { execve_nid_postfix(longPath.c_str(), arguments, nullptr); }, "execve: path of PATH_MAX bytes or more"));

    std::string fitting(262143, 'a');
    char* fittingArguments[] = {fitting.data(), nullptr};
    RequireBoth("/exec-alias/missing", fittingArguments, GUEST_ENOENT);
    char* overflowing[] = {fitting.data(), variable, nullptr};
    Require(Throws([&] { execv_nid_postfix("/exec-alias/missing", overflowing); }, "execv: arguments and environment exceed ARG_MAX"));
    Require(Throws([&] { execve_nid_postfix("/exec-alias/missing", fittingArguments, environment); }, "execve: arguments and environment exceed ARG_MAX"));

    RemovePathAlias_nid_no_patch("/exec-alias");
    std::filesystem::remove_all(directory);
}
