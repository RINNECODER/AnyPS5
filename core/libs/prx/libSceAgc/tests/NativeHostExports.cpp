#include "SceShaders.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Library {
    explicit Library(const char* path) : handle(dlopen(path, RTLD_NOW | RTLD_LOCAL)) {
        if (handle == nullptr) throw std::runtime_error(dlerror());
    }
    ~Library() { dlclose(handle); }
    void* Resolve(const char* name) const {
        auto* symbol = dlsym(handle, name);
        if (symbol == nullptr) throw std::runtime_error(std::string("Missing native host export: ") + name);
        return symbol;
    }
    void* handle;
};

void CheckInterpolantExports(const Library& library) {
    using Function = int (*)(ShaderRegister*, const Shader*, const Shader*);
    for (const auto& names : std::array<std::array<const char*, 2>, 2>{{
             {"HV4j+E0MBHE_nid_no_patch_cut", "sceAgcCreateInterpolantMapping"},
             {"dbOlWdppb4o_nid_no_patch_cut", "sceAgcUnknownCreateInterpolantMapping"}}}) {
        const auto alias = library.Resolve(names[0]);
        Require(alias == library.Resolve(names[1]), "Native NID alias does not resolve to its public function");
        struct Guarded {
            std::uint64_t before;
            std::array<ShaderRegister, 32> registers;
            std::uint64_t after;
        } output;
        std::memset(&output, 0xa5, sizeof(output));
        Require(reinterpret_cast<Function>(alias)(output.registers.data(), nullptr, nullptr) == 0,
                "Native interpolant alias returned an error");
        for (std::uint32_t i = 0; i < output.registers.size(); ++i) {
            Require(output.registers[i].offset == 0x191u + i && output.registers[i].value == i,
                    "Native interpolant alias changed the identity mapping");
        }
        Require(output.before == 0xa5a5a5a5a5a5a5a5ull && output.after == 0xa5a5a5a5a5a5a5a5ull,
                "Native interpolant alias wrote beyond its register output");
    }
}

void CheckUnimplementedExport(const Library& library) {
    using Function = int (*)();
    const auto alias = library.Resolve("vieBRwlh1Lw_nid_no_patch_cut");
    Require(alias == library.Resolve("sceAgcUnknown_vieBRwlh1Lw"), "Native stub NID alias resolves incorrectly");
    try {
        reinterpret_cast<Function>(alias)();
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()) == "sceAgcUnknown_vieBRwlh1Lw not implemented",
                "Native explicit stub changed its failure contract");
        return;
    }
    throw std::runtime_error("Native explicit stub returned success");
}

}

int main(int argc, char** argv) {
    try {
        Require(argc == 2, "Supply the native AGC host library path");
        const Library library(argv[1]);
        CheckInterpolantExports(library);
        CheckUnimplementedExport(library);
        std::puts("Native AGC host export aliases, public interpolation, and explicit stub failure passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
