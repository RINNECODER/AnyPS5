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
    // HV4j+E0MBHE and dbOlWdppb4o hash directly to sceAgcCreateInterpolantMapping_0100 and
    // sceAgcCreateInterpolantMapping_10000; upstream names the exports after their NIDs.
    for (const auto& name : std::array<const char*, 2>{
             "sceAgcCreateInterpolantMapping_0100",
             "sceAgcCreateInterpolantMapping_10000"}) {
        const auto alias = library.Resolve(name);
        struct Guarded {
            std::uint64_t before;
            std::array<ShaderRegister, 32> registers;
            std::uint64_t after;
        } output;
        std::memset(&output, 0xa5, sizeof(output));
        Require(reinterpret_cast<Function>(alias)(output.registers.data(), nullptr, nullptr) == 0,
                "Native interpolant export returned an error");
        for (std::uint32_t i = 0; i < output.registers.size(); ++i) {
            Require(output.registers[i].offset == 0x191u + i && output.registers[i].value == i,
                    "Native interpolant export changed the identity mapping");
        }
        Require(output.before == 0xa5a5a5a5a5a5a5a5ull && output.after == 0xa5a5a5a5a5a5a5a5ull,
                "Native interpolant export wrote beyond its register output");
    }

    const auto update = library.Resolve("sceAgcUpdateInterpolantMapping_10000");
    Shader ps{};
    struct Guarded {
        std::uint64_t before;
        std::array<ShaderRegister, 32> registers;
        std::uint64_t after;
    } output;
    std::memset(&output, 0xa5, sizeof(output));
    Require(reinterpret_cast<Function>(update)(output.registers.data(), nullptr, &ps) == 0,
            "Native update interpolant export returned an error");
    Require(output.before == 0xa5a5a5a5a5a5a5a5ull && output.after == 0xa5a5a5a5a5a5a5a5ull,
            "Native update interpolant export wrote beyond its register output");
}

void CheckUnimplementedExport(const Library& library) {
    using Function = void* (*)();
    const auto alias = library.Resolve("zARR5aCmkoY_nid_no_patch_cut");
    Require(alias == library.Resolve("sceAgcDcbA_zARR5aCmkoY"), "Native stub NID alias resolves incorrectly");
    try {
        reinterpret_cast<Function>(alias)();
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()) == "sceAgcDcbA_zARR5aCmkoY not implemented",
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
