#include "prx/libc/include/Shutdown.hpp"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {
int order = 0;
int ranFirst = 0;
int ranStop = 0;
int ranLast = 0;

void Require(bool value) {
    if (!value) {
        std::fputs("shutdown_stop: check failed\n", stderr);
        std::abort();
    }
}

// Callbacks run in reverse registration order: Last, Stop, First.
void First() { ranFirst = ++order; }
// A shutdown callback that meets the requested stop (ProcessShutdown) must not fail the exit path.
void Stop() { ranStop = ++order; throw ProcessShutdown{}; }
void Last() { ranLast = ++order; }
}

int main() {
    LibcRegisterShutdown_nid_postfix(First);
    LibcRegisterShutdown_nid_postfix(Stop);
    LibcRegisterShutdown_nid_postfix(Last);
    try { LibcRunShutdown_nid_postfix(); }
    catch (...) { Require(false); }
    // The stop is swallowed and the remaining callbacks still ran.
    Require(ranLast == 1 && ranStop == 2 && ranFirst == 3);
    // A later waiter sees a finished, successful shutdown.
    try { LibcRunShutdown_nid_postfix(); }
    catch (...) { Require(false); }

    std::puts("Shutdown stop tests passed");
    return 0;
}
