#include "prx/libSceHttp/src/Options.hpp"
#include "prx/libc/include/General.hpp"
#include "prx/libSceHttp/src/HttpErrors.hpp"
#include <mutex>
#include <unordered_map>

namespace {

std::mutex g_optionsMutex;
std::unordered_map<int, HttpOptions> g_objectOptions;

}

void AddHttpTemplate(int templateId) {
    std::lock_guard lock(g_optionsMutex);
    g_objectOptions[templateId] = HttpOptions{};
}

void AddHttpChild(int id, int parentId) {
    std::lock_guard lock(g_optionsMutex);
    const auto parent = g_objectOptions.find(parentId);
    g_objectOptions[id] = parent != g_objectOptions.end() ? parent->second : HttpOptions{};
}

void RemoveHttpObject(int id) {
    std::lock_guard lock(g_optionsMutex);
    g_objectOptions.erase(id);
}

std::optional<HttpOptions> FindHttpOptions(int id) {
    std::lock_guard lock(g_optionsMutex);
    const auto found = g_objectOptions.find(id);
    if (found == g_objectOptions.end()) return std::nullopt;
    return found->second;
}

bool SetHttpOption(int id, bool HttpOptions::*option, bool enable) {
    std::lock_guard lock(g_optionsMutex);
    const auto found = g_objectOptions.find(id);
    if (found == g_objectOptions.end()) return false;
    found->second.*option = enable;
    return true;
}

extern "C" {

int APS5_VABI sceHttpGetAutoRedirect(int id, int* enable) {
    if (!enable) return ERROR_INVALID_VALUE;
    const auto options = FindHttpOptions(id);
    if (!options) return ERROR_INVALID_ID;
    *enable = options->autoRedirect ? 1 : 0;
    return 0;
}

}
