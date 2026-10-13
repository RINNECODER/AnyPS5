#include "prx/libc/include/general/VabiMacros.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>

extern "C" {
int APS5_VABI sceSslCreateConnection();
int APS5_VABI sceSslDeleteConnection();
int APS5_VABI sceSslEnableVerifyOption();
int APS5_VABI sceSslConnect();
int APS5_VABI sceSslRead();
int APS5_VABI sceSslWrite();
int APS5_VABI sceConvertKeycodeGetCharacter();
int APS5_VABI sceNpSessionSignalingGetGroupFromSessionId();
int APS5_VABI _ZN3sce4Json5Value30setElementAccessFailureHandlerEPFviPKNS0_6StringEPKmPKS1_S8_PvES9_();
int APS5_VABI _ZN3sce4Json5Value22setSpecialFloatHandlerEPFKS1_NS0_12FunctionTypeEdPS2_PvPbES5_();
int APS5_VABI _ZN3sce4Json5Value22setTypeMismatchHandlerEPFRKS1_NS0_9ValueTypeEPS2_S5_PvES6_();
int APS5_VABI _ZNK3sce4Json5ValuecvbEv();
}

namespace {

struct Export {
    const char* name;
    int (APS5_VABI *function)();
};

const Export exports[] = {
    {"sceSslCreateConnection", sceSslCreateConnection},
    {"sceSslDeleteConnection", sceSslDeleteConnection},
    {"sceSslEnableVerifyOption", sceSslEnableVerifyOption},
    {"sceSslConnect", sceSslConnect},
    {"sceSslRead", sceSslRead},
    {"sceSslWrite", sceSslWrite},
    {"sceConvertKeycodeGetCharacter", sceConvertKeycodeGetCharacter},
    {"sceNpSessionSignalingGetGroupFromSessionId", sceNpSessionSignalingGetGroupFromSessionId},
    {"_ZN3sce4Json5Value30setElementAccessFailureHandlerEPFviPKNS0_6StringEPKmPKS1_S8_PvES9_", _ZN3sce4Json5Value30setElementAccessFailureHandlerEPFviPKNS0_6StringEPKmPKS1_S8_PvES9_},
    {"_ZN3sce4Json5Value22setSpecialFloatHandlerEPFKS1_NS0_12FunctionTypeEdPS2_PvPbES5_", _ZN3sce4Json5Value22setSpecialFloatHandlerEPFKS1_NS0_12FunctionTypeEdPS2_PvPbES5_},
    {"_ZN3sce4Json5Value22setTypeMismatchHandlerEPFRKS1_NS0_9ValueTypeEPS2_S5_PvES6_", _ZN3sce4Json5Value22setTypeMismatchHandlerEPFRKS1_NS0_9ValueTypeEPS2_S5_PvES6_},
    {"_ZNK3sce4Json5ValuecvbEv", _ZNK3sce4Json5ValuecvbEv},
};

bool ReportsNotImplemented(const Export& entry) {
    try {
        entry.function();
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()) == std::string(entry.name) + " not implemented") return true;
        std::fprintf(stderr, "%s threw: %s\n", entry.name, error.what());
        return false;
    }
    std::fprintf(stderr, "%s returned instead of reporting that it is not implemented\n", entry.name);
    return false;
}

}

int main() {
    bool correct = true;
    for (const auto& entry : exports) correct &= ReportsNotImplemented(entry);
    return correct ? 0 : 1;
}
