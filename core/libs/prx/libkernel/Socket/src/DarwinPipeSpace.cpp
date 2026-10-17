// Kept apart from Udp.cpp: <sys/event.h> defines EVFILT_* macros that collide with the guest equeue constants.
#ifdef __APPLE__
#include <cstdint>
#include <stdexcept>
#include <string>
#include <sys/event.h>
#include <unistd.h>

namespace GuestSockets {

// Darwin has no F_GETPIPE_SZ; its own kqueue reports the free space of a pipe's write end.
void DarwinPipeWriteSpace(int descriptor, std::int64_t* data, bool* eof, bool* writable) {
    const int queue = ::kqueue();
    if (queue < 0) throw std::runtime_error("kevent: kqueue for pipe descriptor " + std::to_string(descriptor) + " failed");
    struct kevent change{};
    EV_SET(&change, descriptor, EVFILT_WRITE, EV_ADD, 0, 0, nullptr);
    struct kevent event{};
    const timespec immediately{};
    const int count = ::kevent(queue, &change, 1, &event, 1, &immediately);
    ::close(queue);
    if (count < 0 || (count == 1 && (event.flags & EV_ERROR) != 0))
        throw std::runtime_error("kevent: querying pipe descriptor " + std::to_string(descriptor) + " failed");
    *eof = count == 1 && (event.flags & EV_EOF) != 0;
    *writable = count == 1 && !*eof;
    *data = *writable ? static_cast<std::int64_t>(event.data) : 0;
}

}
#endif
