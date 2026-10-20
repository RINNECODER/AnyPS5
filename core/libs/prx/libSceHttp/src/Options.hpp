#ifndef CORE_LIBS_PRX_LIBSCEHTTP_SRC_OPTIONS_HPP
#define CORE_LIBS_PRX_LIBSCEHTTP_SRC_OPTIONS_HPP

#include <optional>

struct HttpOptions {
    bool autoRedirect = true;
    bool nonblock = false;
};

void AddHttpTemplate(int templateId);
void AddHttpChild(int id, int parentId);
void RemoveHttpObject(int id);
std::optional<HttpOptions> FindHttpOptions(int id);
bool SetHttpOption(int id, bool HttpOptions::*option, bool enable);

#endif
