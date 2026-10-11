#pragma once

// Objective-C++ only.
#include <exception>
#include <utility>

namespace AgcDriver::Metal {

// Runs one synchronous draw/dispatch packet inside its own autorelease pool.
// Each packet snapshots every borrowed guest range into fresh shared MTLBuffers;
// the autoreleased MTLCommandBuffer (and the snapshot's NSArray) retain those
// buffers. Without a per-packet pool they live until the whole submission ends,
// so a submission with many draws over a multi-GiB mapping keeps one full copy
// per draw alive and the OS kills the process (SIGKILL, no crash report) once
// swap is exhausted. The pool is popped on the normal path; a thrown exception
// is captured first so the pool still drains, then rethrown unchanged.
template <typename Work>
void DrainPacketAutoreleases(Work&& work) {
    std::exception_ptr failure;
    @autoreleasepool {
        try {
            std::forward<Work>(work)();
        } catch (...) {
            failure = std::current_exception();
        }
    }
    if (failure) std::rethrow_exception(failure);
}

}
