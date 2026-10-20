#include "SceTypes.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" {
int APS5_VABI sceHttpInit_nid_postfix(int, int, std::uint64_t);
int APS5_VABI sceHttpTerm_nid_postfix(int);
int APS5_VABI sceHttpCreateTemplate(int, const char*, int, int);
int APS5_VABI sceHttpDeleteTemplate(int);
int APS5_VABI sceHttpCreateConnectionWithURL(int, const char*, int);
int APS5_VABI sceHttpDeleteConnection(int);
int APS5_VABI sceHttpCreateRequestWithURL(int, int, const char*, std::uint64_t);
int APS5_VABI sceHttpDeleteRequest(int);
int APS5_VABI sceHttpSetAutoRedirect(int, int);
int APS5_VABI sceHttpGetAutoRedirect(int, int*);
int APS5_VABI sceHttpSetNonblock(int, int);
int APS5_VABI sceHttpGetNonblock(int, int*);
int APS5_VABI sceHttpTrySetNonblock(int, int);
int APS5_VABI sceHttpTryGetNonblock(int, int*);
int APS5_VABI sceHttpSetAcceptEncodingGZIPEnabled(int, int);
int APS5_VABI sceHttpGetAcceptEncodingGZIPEnabled(int, int*);
int APS5_VABI sceHttpSetDefaultAcceptEncodingGZIPEnabled(int, int);
}

namespace {

constexpr int InvalidId = static_cast<int>(0x80431100u);
constexpr int InvalidValue = static_cast<int>(0x804311FEu);
constexpr int UnknownId = 0x7fff0000;

void Require(bool value, const char* what) {
    if (value) return;
    std::fprintf(stderr, "%s\n", what);
    std::abort();
}

int Get(int (APS5_VABI *getter)(int, int*), int id) {
    int value = -1;
    Require(getter(id, &value) == 0, "an option getter failed on a live object");
    return value;
}

void CheckAutoRedirect(int context) {
    const int pattern = sceHttpCreateTemplate(context, "anyps5-test", 2, 0);
    Require(Get(sceHttpGetAutoRedirect, pattern) == 1, "a template does not follow redirects by default");
    Require(sceHttpSetAutoRedirect(pattern, 0) == 0, "sceHttpSetAutoRedirect failed on a template");
    Require(Get(sceHttpGetAutoRedirect, pattern) == 0, "sceHttpGetAutoRedirect does not return what was set");
    const int connection = sceHttpCreateConnectionWithURL(pattern, "http://example.com/", 1);
    const int request = sceHttpCreateRequestWithURL(connection, 0, "http://example.com/a", 0);
    Require(Get(sceHttpGetAutoRedirect, connection) == 0 && Get(sceHttpGetAutoRedirect, request) == 0, "a connection or request did not take its parent's redirect option");
    Require(sceHttpSetAutoRedirect(request, 7) == 0 && Get(sceHttpGetAutoRedirect, request) == 1, "a nonzero value did not enable redirects");
    Require(Get(sceHttpGetAutoRedirect, connection) == 0, "setting a request changed its connection");
    Require(sceHttpSetAutoRedirect(pattern, 1) == 0 && Get(sceHttpGetAutoRedirect, connection) == 0, "an existing connection followed a later template change");
    Require(sceHttpGetAutoRedirect(request, nullptr) == InvalidValue, "a null output was not refused");
    Require(sceHttpGetAutoRedirect(UnknownId, nullptr) == InvalidValue, "the output pointer is not checked before the id");
    int value = 0;
    Require(sceHttpGetAutoRedirect(UnknownId, &value) == InvalidId && sceHttpSetAutoRedirect(UnknownId, 1) == InvalidId, "an unknown id was not refused");
    Require(sceHttpDeleteRequest(request) == 0 && sceHttpGetAutoRedirect(request, &value) == InvalidId, "a deleted request still has options");
    Require(sceHttpDeleteConnection(connection) == 0 && sceHttpDeleteTemplate(pattern) == 0, "delete failed");
}

void CheckNonblock(int context) {
    const int pattern = sceHttpCreateTemplate(context, "anyps5-test", 2, 0);
    Require(Get(sceHttpGetNonblock, pattern) == 0 && Get(sceHttpTryGetNonblock, pattern) == 0, "a template is nonblocking by default");
    Require(sceHttpSetNonblock(pattern, 1) == 0, "sceHttpSetNonblock failed on a template");
    const int connection = sceHttpCreateConnectionWithURL(pattern, "http://example.com/", 1);
    Require(Get(sceHttpGetNonblock, connection) == 1, "a connection did not take its template's nonblocking option");
    Require(sceHttpTrySetNonblock(connection, 0) == 0 && Get(sceHttpTryGetNonblock, connection) == 0 && Get(sceHttpGetNonblock, connection) == 0, "sceHttpTrySetNonblock did not change the option sceHttpGetNonblock reads");
    Require(Get(sceHttpGetNonblock, pattern) == 1, "setting a connection changed its template");
    int value = 0;
    Require(sceHttpGetNonblock(connection, nullptr) == InvalidValue && sceHttpTryGetNonblock(connection, nullptr) == InvalidValue, "a null output was not refused");
    Require(sceHttpGetNonblock(UnknownId, &value) == InvalidId && sceHttpTryGetNonblock(UnknownId, &value) == InvalidId, "an unknown id was not refused by the getters");
    Require(sceHttpSetNonblock(UnknownId, 1) == InvalidId && sceHttpTrySetNonblock(UnknownId, 1) == InvalidId, "an unknown id was not refused by the setters");
    Require(sceHttpDeleteConnection(connection) == 0 && sceHttpDeleteTemplate(pattern) == 0, "delete failed");
}

void CheckAcceptEncodingGzip(int context) {
    const int plain = sceHttpCreateTemplate(context, "anyps5-test", 2, 0);
    Require(Get(sceHttpGetAcceptEncodingGZIPEnabled, plain) == 0, "a template asks for gzip by default");
    Require(sceHttpSetDefaultAcceptEncodingGZIPEnabled(context, 1) == 0, "sceHttpSetDefaultAcceptEncodingGZIPEnabled failed");
    const int compressed = sceHttpCreateTemplate(context, "anyps5-test", 2, 0);
    Require(Get(sceHttpGetAcceptEncodingGZIPEnabled, compressed) == 1, "a new template did not take its context's gzip default");
    Require(Get(sceHttpGetAcceptEncodingGZIPEnabled, plain) == 0, "the context default changed an existing template");
    const int connection = sceHttpCreateConnectionWithURL(compressed, "http://example.com/", 1);
    Require(sceHttpSetAcceptEncodingGZIPEnabled(connection, 0) == 0 && Get(sceHttpGetAcceptEncodingGZIPEnabled, connection) == 0, "sceHttpGetAcceptEncodingGZIPEnabled does not return what was set");
    Require(Get(sceHttpGetAcceptEncodingGZIPEnabled, compressed) == 1, "setting a connection changed its template");
    const int other = sceHttpInit_nid_postfix(1, 1, 0x10000);
    const int otherTemplate = sceHttpCreateTemplate(other, "anyps5-test", 2, 0);
    Require(Get(sceHttpGetAcceptEncodingGZIPEnabled, otherTemplate) == 0, "one context's gzip default reached another context");
    int value = 0;
    Require(sceHttpGetAcceptEncodingGZIPEnabled(connection, nullptr) == InvalidValue, "a null output was not refused");
    Require(sceHttpGetAcceptEncodingGZIPEnabled(UnknownId, &value) == InvalidId && sceHttpSetAcceptEncodingGZIPEnabled(UnknownId, 1) == InvalidId, "an unknown id was not refused");
    Require(sceHttpSetDefaultAcceptEncodingGZIPEnabled(UnknownId, 1) == InvalidId, "an unknown context was not refused");
    Require(sceHttpDeleteTemplate(otherTemplate) == 0 && sceHttpTerm_nid_postfix(other) == 0, "the second context did not close");
    Require(sceHttpSetDefaultAcceptEncodingGZIPEnabled(other, 1) == InvalidId, "a closed context still takes a gzip default");
    Require(sceHttpSetDefaultAcceptEncodingGZIPEnabled(context, 0) == 0, "the gzip default could not be reset");
    Require(sceHttpDeleteConnection(connection) == 0 && sceHttpDeleteTemplate(compressed) == 0 && sceHttpDeleteTemplate(plain) == 0, "delete failed");
}

}

int main() {
    const int context = sceHttpInit_nid_postfix(1, 1, 0x10000);
    Require(context > 0, "sceHttpInit failed");
    CheckAutoRedirect(context);
    CheckNonblock(context);
    CheckAcceptEncodingGzip(context);
    Require(sceHttpTerm_nid_postfix(context) == 0, "sceHttpTerm failed");
    Require(sceHttpSetDefaultAcceptEncodingGZIPEnabled(context, 1) == static_cast<int>(0x80431001u), "a gzip default was taken before sceHttpInit");
    std::puts("HTTP option tests passed");
    return 0;
}
