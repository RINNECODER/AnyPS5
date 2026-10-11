#pragma once

#include <cpu/Cpu.hpp>
#include <cpu/SceUpstreamPrxBridge.hpp>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <memory>
#include <optional>
#include <vector>

namespace Cpu {

struct SceImport;

// Native providers for the system, dialog and NP leaf exports SceUpstreamPrxBridge cannot
// take (nested guest pointers, callbacks, floats, objects the upstream body allocates, or
// bodies that throw NotImplemented). Each handler returns the status codes the upstream
// core/libs/prx body returns; dialogs report "finished" with the default (OK) result as the
// headless upstream builds do. Guest memory is only touched through Machine Read/Write.
// Save data: sceSaveDataMount3 creates or opens Root/<dirName> and asks Bind to map the guest
// mount "savedataN" onto it; sceSaveDataUmount2 asks Unbind to drop it. Without a storage
// (an empty Root) Mount3 reports the upstream internal error.
// Unknown NIDs and imports outside the audited module/library/version return nullopt.
// Machine must outlive this provider.
struct SceSaveDataStorage {
    std::filesystem::path Root;
    std::function<bool(const std::string& mount, const std::filesystem::path& host)> Bind;
    std::function<bool(const std::string& mount)> Unbind;
};

class SceLeafSystemImports {
public:
    explicit SceLeafSystemImports(Machine& machine, std::uint64_t gateBase = 0x7ffd90000000);
    ~SceLeafSystemImports();
    SceLeafSystemImports(const SceLeafSystemImports&) = delete;
    SceLeafSystemImports& operator=(const SceLeafSystemImports&) = delete;
    std::optional<std::uint64_t> Resolve(const SceImport& import);
    void SetSaveDataStorage(SceSaveDataStorage storage);
    // Every module/library scope Resolve can serve, for declaring them as host modules.
    static std::vector<SceUpstreamHost> Hosts();
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

}
